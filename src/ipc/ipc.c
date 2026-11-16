#define _GNU_SOURCE
#include "ipc/ipc.h"
#include "core/types.h"
#include "widget/widget.h"
#include "core/config.h"
#include "core/app.h"
#include "scripting/scripting.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>
#include "ipc/frame.h"

#define WISP_SOCK_PATH_MAX sizeof(((struct sockaddr_un *)0)->sun_path)
#define IPC_LINE_MAX 4096

/* --- Framed server: bounded per-client state machine driven by the poll loop --- */

enum { IPC_FREE = 0, IPC_READING, IPC_WRITING };
enum { IPC_PHASE_SNIFF, IPC_PHASE_PAYLOAD, IPC_PHASE_LINE };

struct ipc_client_slot {
	int fd;
	int state;
	int phase;
	bool framed;
	bool too_long;
	unsigned char hdr[4];
	size_t hdr_off;
	unsigned char *payload;   /* IPC_MAX_FRAME, malloc'd on accept */
	size_t payload_off;
	size_t payload_len;
	char *out;
	size_t out_len, out_off;
	uint32_t gen;
};

static struct ipc_client_slot g_slots[IPC_MAX_CLIENTS];
static bool g_slots_ready;
static int g_slot_order[IPC_MAX_CLIENTS];

static void slots_ensure_init(void)
{
	if (g_slots_ready)
		return;
	for (int i = 0; i < IPC_MAX_CLIENTS; i++)
		g_slots[i].fd = -1;
	g_slots_ready = true;
}

static void slot_free(struct ipc_client_slot *s)
{
	if (s->fd >= 0)
		close(s->fd);
	free(s->payload);
	free(s->out);
	memset(s, 0, sizeof(*s));
	s->fd = -1;
}

static void slots_reset(void)
{
	slots_ensure_init();
	for (int i = 0; i < IPC_MAX_CLIENTS; i++)
		slot_free(&g_slots[i]);
}

static struct ipc_client_slot *slot_alloc(void)
{
	for (int i = 0; i < IPC_MAX_CLIENTS; i++)
		if (g_slots[i].state == IPC_FREE)
			return &g_slots[i];
	return NULL;
}

static char *capture_command(const char *cmd)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *m = open_memstream(&buf, &len);
	if (!m)
		return strdup("error: out of memory\n");
	ipc_execute_command(cmd, m);
	fclose(m);
	return buf ? buf : strdup("");
}

static void slot_write(struct ipc_client_slot *s);

/* Take ownership of text; queue as-is for the legacy raw-text protocol. */
static void slot_queue_raw(struct ipc_client_slot *s, char *text)
{
	if (!text)
		text = strdup("error: out of memory\n");
	if (!text) {
		slot_free(s);
		return;
	}
	s->out = text;
	s->out_len = strlen(text);
	s->out_off = 0;
	s->state = IPC_WRITING;
}

/* Queue an error frame (framed protocol only). */
static void slot_queue_error(struct ipc_client_slot *s, uint32_t gen, const char *msg)
{
	size_t cap = strlen(msg) * 6 + 64;
	char *resp = malloc(cap);
	if (!resp) {
		slot_free(s);
		return;
	}
	int rl = ipc_json_encode_response(resp, cap, gen, false, msg);
	if (rl < 0) {
		free(resp);
		slot_free(s);
		return;
	}
	s->out = malloc((size_t)rl + 4);
	if (!s->out) {
		free(resp);
		slot_free(s);
		return;
	}
	ipc_frame_prefix((unsigned char *)s->out, (size_t)rl);
	memcpy(s->out + 4, resp, (size_t)rl);
	free(resp);
	s->out_len = (size_t)rl + 4;
	s->out_off = 0;
	s->state = IPC_WRITING;
}

/* text starts with "error" (or is empty) => ok=false for non-ok-first output;
 * every executor error message starts with "error". */
static bool text_is_error(const char *text)
{
	return strncmp(text, "error", 5) == 0;
}

static void slot_process_inner(struct ipc_client_slot *s)
{
	if (s->too_long) {
		if (s->framed)
			slot_queue_error(s, s->gen, "error: command too long\n");
		else
			slot_queue_raw(s, strdup("error: command too long\n"));
		return;
	}
	if (s->framed) {
		uint32_t gen = 0;
		char verb[32];
		char *args[IPC_MAX_ARGS];
		int argc = 0;
		if (ipc_frame_decode((char *)s->payload, s->payload_len, &gen, verb, sizeof(verb),
				     args, IPC_MAX_ARGS, &argc) != 0 || !ipc_verb_valid(verb)) {
			slot_queue_error(s, gen, "error: invalid request\n");
			return;
		}
		s->gen = gen;
		char cmd[IPC_LINE_MAX];
		if (ipc_build_command(cmd, sizeof(cmd), verb, argc, args) != 0) {
			slot_queue_error(s, gen, "error: command too long\n");
			return;
		}
		char *text = capture_command(cmd);
		if (!text) {
			slot_queue_error(s, gen, "error: out of memory\n");
			return;
		}
		size_t tlen = strlen(text);
		size_t cap = tlen * 6 + 64;
		char *pkt = malloc(cap + 4);
		if (pkt) {
			int rl = ipc_json_encode_response(pkt + 4, cap, gen, !text_is_error(text), text);
			if (rl >= 0) {
				ipc_frame_prefix((unsigned char *)pkt, (size_t)rl);
				s->out_len = (size_t)rl + 4;
				s->out_off = 0;
				s->out = pkt;
				s->state = IPC_WRITING;
				free(text);
				return;
			}
			free(pkt);
		}
		free(text);
		slot_queue_error(s, gen, "error: out of memory\n");
		return;
	}
	s->payload[s->payload_off] = '\0';
	slot_queue_raw(s, capture_command((char *)s->payload));
}

/* Every transition to IPC_WRITING flushes immediately (non-blocking);
 * whatever does not fit stays in the poll loop's POLLOUT path. */
static void slot_process(struct ipc_client_slot *s)
{
	slot_process_inner(s);
	if (s->state == IPC_WRITING)
		slot_write(s);
}

/* Read until EAGAIN/EOF or a complete request; on completion, process it. */
static void slot_read(struct ipc_client_slot *s)
{
	for (;;) {
		if (s->phase == IPC_PHASE_SNIFF) {
			ssize_t n = read(s->fd, s->hdr + s->hdr_off, 4 - s->hdr_off);
			if (n < 0) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) return;
				slot_free(s);
				return;
			}
			if (n == 0) {
				slot_free(s);
				return;
			}
			s->hdr_off += (size_t)n;
			if (s->hdr_off < 4)
				return;
			size_t flen = ipc_frame_len(s->hdr);
			if (s->hdr[0] == 0 && s->hdr[1] == 0) {
				if (flen > IPC_MAX_FRAME) {
					slot_queue_error(s, 0, "error: frame too large\n");
					return;
				}
				s->framed = true;
				s->payload_len = flen;
				s->payload_off = 0;
				s->phase = IPC_PHASE_PAYLOAD;
				continue;
			}
			s->framed = false;
			memcpy(s->payload, s->hdr, 4);
			s->payload_off = 4;
			s->phase = IPC_PHASE_LINE;
			continue;
		}
		if (s->phase == IPC_PHASE_PAYLOAD) {
			size_t room = s->payload_len - s->payload_off;
			if (room == 0) {
				slot_process(s); /* frame complete */
				return;
			}
			ssize_t n = read(s->fd, s->payload + s->payload_off, room);
			if (n < 0) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) return;
				slot_free(s);
				return;
			}
			if (n == 0) {
				slot_free(s); /* peer closed mid-frame */
				return;
			}
			s->payload_off += (size_t)n;
			if (s->payload_off < s->payload_len)
				return;
			slot_process(s);
			return;
		}
		/* IPC_PHASE_LINE: legacy line protocol */
		size_t room = IPC_LINE_MAX - 1 - s->payload_off;
		if (room == 0) {
			s->too_long = true;
			slot_process(s);
			return;
		}
		size_t before = s->payload_off;
		ssize_t n = read(s->fd, s->payload + s->payload_off, room);
		if (n < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			slot_free(s);
			return;
		}
		if (n == 0) {
			if (s->payload_off > 0)
				slot_process(s); /* EOF terminates the line */
			else
				slot_free(s);
			return;
		}
		s->payload_off += (size_t)n;
		for (size_t i = before; i < s->payload_off; i++) {
			if (s->payload[i] == '\n' || s->payload[i] == '\r') {
				s->payload[i] = '\0';
				s->payload_off = i;
				slot_process(s);
				return;
			}
		}
	}
}

static void slot_write(struct ipc_client_slot *s)
{
	if (s->out_len == 0) {
		slot_free(s);
		return;
	}
	for (;;) {
		ssize_t n = write(s->fd, s->out + s->out_off, s->out_len - s->out_off);
		if (n < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			slot_free(s);
			return;
		}
		if (n == 0)
			return;
		s->out_off += (size_t)n;
		if (s->out_off >= s->out_len) {
			slot_free(s); /* one-shot: connection closes after response */
			return;
		}
	}
}

void ipc_server_accept(int server_fd)
{
	slots_ensure_init();
	for (;;) {
		int fd = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (fd < 0)
			return; /* EAGAIN: backlog drained; else nothing to do */
		struct ipc_client_slot *s = slot_alloc();
		if (!s) {
			close(fd); /* at capacity */
			continue;
		}
		s->fd = fd;
		s->state = IPC_READING;
		s->phase = IPC_PHASE_SNIFF;
		s->payload = malloc(IPC_MAX_FRAME + 1);
		if (!s->payload) {
			slot_free(s);
			continue;
		}
	}
}

int ipc_server_poll(struct pollfd *pfds)
{
	slots_ensure_init();
	int n = 0;
	for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
		if (g_slots[i].state == IPC_FREE)
			continue;
		g_slot_order[n] = i;
		pfds[n].fd = g_slots[i].fd;
		pfds[n].events = (g_slots[i].state == IPC_WRITING) ? POLLOUT : POLLIN;
		pfds[n].revents = 0;
		n++;
	}
	return n;
}

void ipc_server_handle_poll(const struct pollfd *pfds, int count)
{
	for (int i = 0; i < count; i++) {
		struct ipc_client_slot *s = &g_slots[g_slot_order[i]];
		if (s->state == IPC_FREE)
			continue;
		if (pfds[i].revents & (POLLERR | POLLNVAL)) {
			slot_free(s);
			continue;
		}
		if (s->state == IPC_READING && (pfds[i].revents & (POLLIN | POLLHUP)))
			slot_read(s);
		else if (s->state == IPC_WRITING && (pfds[i].revents & (POLLOUT | POLLHUP)))
			slot_write(s);
	}
}

bool ipc_has_pending_writes(void)
{
	slots_ensure_init();
	for (int i = 0; i < IPC_MAX_CLIENTS; i++)
		if (g_slots[i].state == IPC_WRITING)
			return true;
	return false;
}

size_t ipc_escape_arg(const char *in, char *out, size_t out_len)
{
	if (!out_len) return 0;
	if (!in || !in[0]) {
		if (out_len > 2) { out[0] = '\\'; out[1] = 'e'; out[2] = '\0'; return 2; }
		out[0] = '\0';
		return 0;
	}
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 2 < out_len; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '\\' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			out[j++] = '\\';
			if (j + 1 >= out_len) break;
			out[j++] = (c == '\n') ? 'n' : (c == '\r') ? 'r' : (c == '\t') ? 't' : (char)c;
		} else {
			out[j++] = (char)c;
		}
	}
	out[j < out_len ? j : out_len - 1] = '\0';
	return j;
}

void ipc_unescape_token(char *tok)
{
	if (!tok) return;
	if (tok[0] == '\\' && tok[1] == 'e' && tok[2] == '\0') {
		tok[0] = '\0';
		return;
	}
	char *r = tok, *w = tok;
	while (*r) {
		if (*r == '\\' && r[1]) {
			r++;
			if (*r == 'n') *w++ = '\n';
			else if (*r == 'r') *w++ = '\r';
			else if (*r == 't') *w++ = '\t';
			else if (*r == 'e' && w == tok && r[1] == '\0') { /* \e mid? treat as literal e marker edge */ *w++ = '\0'; }
			else *w++ = *r;
			r++;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

int ipc_parse_argv(char *line, char **argv, int max)
{
	int argc = 0;
	char *p = line;
	if (!p) return 0;
	while (*p && argc < max) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		char *start = p;
		while (*p) {
			if (*p == '\\' && p[1]) { p += 2; continue; }
			if (*p == ' ' || *p == '\t') break;
			p++;
		}
		if (*p) *p++ = '\0';
		ipc_unescape_token(start);
		argv[argc++] = start;
	}
	return argc;
}

static void get_ipc_socket_path(char *buf, size_t len)
{
	const char *override = getenv("WISP_IPC_SOCKET");
	if (override && override[0]) {
		snprintf(buf, len, "%s", override);
		return;
	}
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir && runtime_dir[0]) {
		snprintf(buf, len, "%s/wisp.sock", runtime_dir);
		return;
	}
	const char *user = getenv("USER");
	if (user && user[0])
		snprintf(buf, len, "/tmp/wisp-%s.sock", user);
	else
		snprintf(buf, len, "/tmp/wisp-%u.sock", (unsigned)getuid());
}

int ipc_server_init(void)
{
	slots_reset();
	char sock_path[WISP_SOCK_PATH_MAX];
	get_ipc_socket_path(sock_path, sizeof(sock_path));

	int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (test_fd >= 0) {
		struct sockaddr_un sun;
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);
		if (connect(test_fd, (struct sockaddr *)&sun, sizeof(sun)) == 0) {
			close(test_fd);
			return -EADDRINUSE;
		}
		close(test_fd);
	}

	unlink(sock_path);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		close(fd);
		return -1;
	}
	chmod(sock_path, S_IRUSR | S_IWUSR);
	if (listen(fd, 16) < 0) {
		close(fd);
		unlink(sock_path);
		return -1;
	}
	return fd;
}

void ipc_server_shutdown(int fd)
{
	slots_reset();
	if (fd >= 0) {
		close(fd);
		char sock_path[WISP_SOCK_PATH_MAX];
		get_ipc_socket_path(sock_path, sizeof(sock_path));
		unlink(sock_path);
	}
}

int ipc_build_command(char *out, size_t out_len, const char *verb, int argc, char *const *argv)
{
	if (!out || !out_len || !verb) return -1;
	int n = snprintf(out, out_len, "%s", verb);
	if (n < 0 || (size_t)n >= out_len) return -1;
	size_t pos = (size_t)n;
	for (int j = 0; j < argc; j++) {
		if (pos + 1 >= out_len) return -1;
		out[pos++] = ' ';
		const char *in = argv[j];
		if (!in || !in[0]) {
			if (pos + 3 >= out_len) return -1;
			out[pos++] = '\\'; out[pos++] = 'e';
			continue;
		}
		for (size_t k = 0; in[k]; k++) {
			unsigned char c = (unsigned char)in[k];
			if (pos + 3 >= out_len) return -1;
			if (c == '\\' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
				out[pos++] = '\\';
				out[pos++] = (c == '\n') ? 'n' : (c == '\r') ? 'r' : (c == '\t') ? 't' : (char)c;
			} else {
				out[pos++] = (char)c;
			}
		}
	}
	out[pos] = '\0';
	return 0;
}

static int ipc_client_send_opt(const char *cmd, bool verbose_conn_err)
{
	char sock_path[WISP_SOCK_PATH_MAX];
	get_ipc_socket_path(sock_path, sizeof(sock_path));

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		if (verbose_conn_err)
			perror("wisp: socket");
		return -1;
	}

	/* Fail loudly instead of hanging forever against a daemon that
	 * predates the frame protocol. */
	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);

	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		if (verbose_conn_err)
			fprintf(stderr, "wisp: cannot connect to running instance at '%s': %s\n",
				sock_path, strerror(errno));
		close(fd);
		return -1;
	}

	size_t cmd_len = strlen(cmd);
	char *argv_buf = malloc(cmd_len + 1);
	int status = 1;
	if (!argv_buf)
		goto done;
	memcpy(argv_buf, cmd, cmd_len + 1);

	char *argv[IPC_MAX_ARGS + 1];
	int argc = ipc_parse_argv(argv_buf, argv, IPC_MAX_ARGS + 1);
	if (argc == 0) {
		status = 0; /* empty command: no-op */
		free(argv_buf);
		goto done;
	}

	uint32_t gen = (uint32_t)time(NULL);
	char req[IPC_MAX_FRAME + 64];
	int rl = ipc_json_encode_request(req, sizeof(req), gen, argv[0], argc - 1,
					     argc > 1 ? &argv[1] : NULL);
	free(argv_buf);
	if (rl < 0) {
		fprintf(stderr, "wisp: command too long\n");
		goto done;
	}

	unsigned char pkt[4 + IPC_MAX_FRAME + 64];
	ipc_frame_prefix(pkt, (size_t)rl);
	memcpy(pkt + 4, req, (size_t)rl);
	size_t pkt_len = 4 + (size_t)rl, off = 0;
	while (off < pkt_len) {
		ssize_t n = write(fd, pkt + off, pkt_len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (verbose_conn_err)
				perror("wisp: write");
			goto done;
		}
		off += (size_t)n;
	}

	unsigned char resp[IPC_MAX_FRAME + 4];
	size_t resp_len = 0;
	bool timed_out = false;
	for (;;) {
		ssize_t n = read(fd, resp + resp_len, sizeof(resp) - 1 - resp_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				timed_out = true;
			break;
		}
		if (n == 0)
			break;
		resp_len += (size_t)n;
		if (resp_len >= sizeof(resp) - 1)
			break;
	}
	resp[resp_len] = '\0';
	if (resp_len == 0) {
		if (timed_out)
			fprintf(stderr, "wisp: no response from daemon (daemon may predate the frame protocol)\n");
		else if (verbose_conn_err)
			fprintf(stderr, "wisp: connection closed without response\n");
		goto done;
	}
	if (resp_len >= 4 && resp[0] == 0 && resp[1] == 0 &&
	    ipc_frame_len(resp) == resp_len - 4) {
		uint32_t rgen;
		bool rok = false;
		char text[IPC_MAX_FRAME + 1];
		if (ipc_json_decode_response((char *)resp + 4, resp_len - 4, &rgen, &rok,
					       text, sizeof(text)) != 0) {
			fprintf(stderr, "wisp: malformed response from daemon\n");
			goto done;
		}
		/* gen 0 is the daemon's "unparsed request" sentinel; only echoes match. */
		if (rgen != 0 && rgen != gen) {
			fprintf(stderr, "wisp: response gen mismatch (daemon restarted?)\n");
			goto done;
		}		fputs(text, rok ? stdout : stderr);
		status = rok ? 0 : 1;
		goto done;
	}
	/* Legacy raw-text response (pre-frame daemon): print as-is. */
	bool err = strncmp((char *)resp, "error:", 6) == 0;
	fputs((char *)resp, err ? stderr : stdout);
	status = err ? 1 : 0;

done:
	close(fd);
	return status;
}

void ipc_server_get_path(char *buf, size_t len)
{
	get_ipc_socket_path(buf, len);
}

int ipc_client_send(const char *cmd)
{
	/* Pass -1 through so callers can detect "daemon not running". */
	return ipc_client_send_opt(cmd, true);
}

int ipc_client_send_silent(const char *cmd)
{
	return ipc_client_send_opt(cmd, false);
}

static bool is_script_file(const char *path)
{
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool try_script_candidate(const char *path, char *out_path, size_t out_len)
{
	if (is_script_file(path)) {
		snprintf(out_path, out_len, "%s", path);
		return true;
	}
	return false;
}

static bool resolve_script_path(const char *name, char *out_path, size_t out_len)
{
	if (!name || !name[0] || !out_path || out_len == 0)
		return false;

	char test_path[512];

	if (try_script_candidate(name, out_path, out_len))
		return true;
	if (g_config.watch_dir[0]) {
		snprintf(test_path, sizeof(test_path), "%s/%s", g_config.watch_dir, name);
		if (try_script_candidate(test_path, out_path, out_len))
			return true;
	}

	snprintf(test_path, sizeof(test_path), "%s.lua", name);
	if (try_script_candidate(test_path, out_path, out_len))
		return true;
	if (g_config.watch_dir[0]) {
		snprintf(test_path, sizeof(test_path), "%s/%s.lua", g_config.watch_dir, name);
		if (try_script_candidate(test_path, out_path, out_len))
			return true;
	}

	/* directory form: name/name.lua (e.g. bar/bar.lua) */
	struct stat st;
	if (stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
		snprintf(test_path, sizeof(test_path), "%s/%s.lua", name, name);
		if (try_script_candidate(test_path, out_path, out_len))
			return true;
	}

	const char *dirs[] = {
		g_config.watch_dir[0] ? g_config.watch_dir : NULL,
		"apps",
		"scripts",
		"examples",
		"/usr/share/wisp/apps",
		"/usr/share/wisp/scripts",
		"/usr/share/wisp/examples"
	};

	for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
		if (!dirs[i]) continue;
		const char *cands[] = {
			"%s/%s.lua", "%s/%s", "%s/apps/%s.lua", "%s/scripts/%s.lua", "%s/lua/%s.lua", "%s/lua/%s", NULL
		};
		for (int c = 0; cands[c]; c++) {
			snprintf(test_path, sizeof(test_path), cands[c], dirs[i], name);
			if (try_script_candidate(test_path, out_path, out_len))
				return true;
		}
		snprintf(test_path, sizeof(test_path), "%s/%s/%s.lua", dirs[i], name, name);
		if (try_script_candidate(test_path, out_path, out_len))
			return true;
		snprintf(test_path, sizeof(test_path), "%s/lua/%s/%s.lua", dirs[i], name, name);
		if (try_script_candidate(test_path, out_path, out_len))
			return true;
	}

	/* Prefix match in dirs */
	size_t nlen = strlen(name);
	for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
		if (!dirs[i]) continue;
		const char *subdirs[] = { "", "/apps", "/scripts" };
		for (size_t s = 0; s < 3; s++) {
			char dir_path[512];
			snprintf(dir_path, sizeof(dir_path), "%s%s", dirs[i], subdirs[s]);
			DIR *d = opendir(dir_path);
			if (!d) continue;
			struct dirent *ent;
			while ((ent = readdir(d)) != NULL) {
				if (ent->d_name[0] == '.') continue;
				if (strncmp(ent->d_name, name, nlen) == 0) {
					snprintf(test_path, sizeof(test_path), "%s/%s", dir_path, ent->d_name);
					if (is_script_file(test_path)) {
						snprintf(out_path, out_len, "%s", test_path);
						closedir(d);
						return true;
					}
					/* matched directory: try <match>/<name>.lua */
					if (stat(test_path, &st) == 0 && S_ISDIR(st.st_mode)) {
						char inner[512];
						snprintf(inner, sizeof(inner), "%s/%s.lua", test_path, name);
						if (try_script_candidate(inner, out_path, out_len)) { closedir(d); return true; }
						snprintf(inner, sizeof(inner), "%s/%s.lua", test_path, ent->d_name);
						if (try_script_candidate(inner, out_path, out_len)) { closedir(d); return true; }
					}
				}
			}
			closedir(d);
		}
	}

	return false;
}

/* Shared open/toggle tail: resolve name to a script path and execute it.
 * verb is "opened" or "toggled" for the success message. */
static void ipc_run_script_fallback(const char *name, int sargc, char **sargv,
				     const char *verb, FILE *out)
{
	char found_path[512] = {0};
	if (!resolve_script_path(name, found_path, sizeof(found_path))) {
		fprintf(out, "error: '%s' is not registered\n", name);
		return;
	}
	struct widget *w = NULL;
	bool exec_ok = widget_execute_dynamic_args(found_path, sargc, sargv, &w);
	if (exec_ok && w) {
		fprintf(out, "ok: %s '%s'\n", verb, name);
		redraw_request = 1;
	} else if (exec_ok) {
		fprintf(out, "error: script loaded but registered no widget/app: '%s'\n", found_path);
	} else {
		fprintf(out, "error: failed to load script '%s'\n", found_path);
	}
}

void ipc_execute_command(const char *raw_cmd, FILE *out)
{
	if (!raw_cmd || !out)
		return;
	char cmd[IPC_LINE_MAX];
	snprintf(cmd, sizeof(cmd), "%s", raw_cmd);
	char *p = cmd + strlen(cmd);
	while (p > cmd && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t'))
		*--p = '\0';
	char *c = cmd;
	while (*c == ' ' || *c == '\t')
		c++;
	if (!*c)
		return;

	char err[256] = {0};

	if (strcmp(c, "list") == 0) {
		widget_list(out);
		if (g_app_count > 0) {
			fprintf(out, "\n");
			app_list_print(out);
		}
	} else if (strcmp(c, "apps") == 0) {
		app_list_print(out);
	} else if (strcmp(c, "status") == 0) {
		widget_runtime_status(out);
	} else if (strncmp(c, "open ", 5) == 0) {
		char *argv[64];
		int argc = ipc_parse_argv(c, argv, 64);
		if (argc < 2) {
			fprintf(out, "error: missing widget name for 'open'\n");
			fflush(out);
			return;
		}
		const char *name = argv[1];
		int sargc = argc - 2;
		char **sargv = sargc > 0 ? &argv[2] : NULL;
		struct staged_widget_spec *sp = registry_find(name);
		if (sp) {
			if (widget_open_named_args(name, err, sizeof(err), sargc, sargv)) {
				fprintf(out, "ok: opened widget '%s'\n", name);
				redraw_request = 1;
			} else {
				fprintf(out, "error: %s\n", err[0] ? err : "failed to open widget");
			}
		} else if (app_open_args(name, err, sizeof(err), sargc, sargv)) {
			fprintf(out, "ok: opened app '%s'\n", name);
			redraw_request = 1;
		} else {
			ipc_run_script_fallback(name, sargc, sargv, "opened", out);
		}
	} else if (strncmp(c, "close ", 6) == 0) {
		const char *name = c + 6;
		while (*name == ' ') name++;
		struct wisp_app *app = app_find(name);
		if (app && app->state == APP_STATE_RUNNING) {
			app_close(name);
			fprintf(out, "ok: closed app '%s'\n", name);
			redraw_request = 1;
		} else if (widget_close_named(name, err, sizeof(err))) {
			fprintf(out, "ok: closed widget '%s'\n", name);
			redraw_request = 1;
		} else if (app_close(name)) {
			fprintf(out, "ok: closed app '%s'\n", name);
			redraw_request = 1;
		} else {
			fprintf(out, "error: %s\n", err[0] ? err : "failed to close target");
		}
	} else if (strncmp(c, "toggle ", 7) == 0) {
		char *argv[64];
		int argc = ipc_parse_argv(c, argv, 64);
		if (argc < 2) {
			fprintf(out, "error: missing widget name for 'toggle'\n");
			fflush(out);
			return;
		}
		const char *name = argv[1];
		int sargc = argc - 2;
		char **sargv = sargc > 0 ? &argv[2] : NULL;
		struct staged_widget_spec *sp = registry_find(name);
		if (sp) {
			if (widget_toggle_named_args(name, err, sizeof(err), sargc, sargv)) {
				fprintf(out, "ok: toggled widget '%s'\n", name);
				redraw_request = 1;
			} else {
				fprintf(out, "error: %s\n", err[0] ? err : "failed to toggle widget");
			}
		} else if (app_find(name)) {
			if (app_toggle_args(name, err, sizeof(err), sargc, sargv)) {
				fprintf(out, "ok: toggled app '%s'\n", name);
				redraw_request = 1;
			} else {
				fprintf(out, "error: %s\n", err[0] ? err : "failed to toggle app");
			}
		} else {
			ipc_run_script_fallback(name, sargc, sargv, "toggled", out);
		}
	} else if (strncmp(c, "reload ", 7) == 0) {
		const char *name = c + 7;
		while (*name == ' ') name++;
		if (widget_reload_named(name, err, sizeof(err))) {
			fprintf(out, "ok: reloaded widget '%s'\n", name);
			redraw_request = 1;
		} else if (app_reload(name)) {
			fprintf(out, "ok: reloaded app '%s'\n", name);
			redraw_request = 1;
		} else {
			fprintf(out, "error: %s\n", err[0] ? err : "failed to reload target");
		}
	} else if (strcmp(c, "reload") == 0) {
		if (config_reload(NULL)) {
			fprintf(out, "ok: config reloaded\n");
			redraw_request = 1;
		} else {
			fprintf(out, "error: config reload failed\n");
		}
	} else if (strcmp(c, "kill") == 0 || strcmp(c, "stop") == 0) {
		fprintf(out, "ok: stopping wisp daemon\n");
		fflush(out);
		running = 0;
		return;
	} else if (strncmp(c, "load ", 5) == 0) {
		char *argv[64];
		int argc = ipc_parse_argv(c, argv, 64);
		if (argc < 2) {
			fprintf(out, "error: missing path for 'load'\n");
			fflush(out);
			return;
		}
		const char *path = argv[1];
		int sargc = argc - 2;
		char **sargv = sargc > 0 ? &argv[2] : NULL;
		char found_path[512] = {0};
		struct widget *w = NULL;
		bool exec_ok = false;
		if (resolve_script_path(path, found_path, sizeof(found_path))) {
			struct stat st;
			if (stat(found_path, &st) == 0 && S_ISREG(st.st_mode))
				exec_ok = widget_execute_dynamic_args(found_path, sargc, sargv, &w);
		}
		if (exec_ok && w) fprintf(out, "ok: loaded '%s'\n", found_path[0] ? found_path : path);
		else if (exec_ok) fprintf(out, "error: script loaded but registered no widget/app: '%s'\n", found_path[0] ? found_path : path);
		else fprintf(out, "error: failed to load '%s'\n", path);
	} else if (strncmp(c, "unload ", 7) == 0) {
		const char *name = c + 7;
		while (*name == ' ') name++;
		if (widget_unload_dynamic(name)) fprintf(out, "ok: unloaded '%s'\n", name);
		else fprintf(out, "error: failed to unload '%s'\n", name);
	} else {
		fprintf(out, "error: unrecognized command '%s' (commands: open, close, toggle, reload, list, status, stop)\n", c);
	}
	fflush(out);
}

void handle_stdin_command(char *cmd)
{
	ipc_execute_command(cmd, stdout);
}

void widget_list_status(FILE *out)
{
	widget_runtime_status(out);
}
