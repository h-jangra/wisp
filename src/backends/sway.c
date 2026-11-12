#define _GNU_SOURCE
#include "backends/backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>

/* Sway / i3 IPC backend (Sway, i3, and wlroots compositors exposing the
 * i3-compat socket such as River via rivertile wrappers).
 * Set SWAYSOCK or I3SOCK to point at the IPC socket; otherwise the backend
 * is skipped and the generic fallback is used.
 *
 * Window counts per workspace come from a best-effort single pass over
 * GET_TREE; failure to parse degrades to windows=0 like the generic backend.
 */

#define SWAY_MAGIC "i3-ipc"
#define SWAY_MAGIC_LEN 6
#define SWAY_HDR (SWAY_MAGIC_LEN + 8)

#define MSG_RUN_COMMAND 0
#define MSG_GET_WORKSPACES 1
#define MSG_SUBSCRIBE 2
#define MSG_GET_TREE 4
#define MSG_GET_VERSION 7

#define TREE_MAX (1u << 20)

static char sway_sock_path[512];
static int sway_event_fd = -1;
static wisp_workspace_info_t sway_ws[64];
static int sway_ws_uid[64]; /* unique node id per workspace (matches GET_TREE) */
static int sway_ws_count = 0;
static int sway_active = 1;
static char sway_title[512];
static int64_t sway_title_at_us = 0;
static char sway_event_buf[16384];
static size_t sway_event_len = 0;

static int64_t sway_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int sway_connect(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sway_sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* One request/response round trip. Returns payload bytes, -1 on error. */
static int sway_call(uint32_t type, const char *payload, char *out, size_t out_len)
{
	int fd = sway_connect();
	if (fd < 0)
		return -1;
	size_t plen = payload ? strlen(payload) : 0;
	size_t total = SWAY_HDR + plen;
	uint8_t *msg = malloc(total ? total : 1);
	if (!msg) {
		close(fd);
		return -1;
	}
	memcpy(msg, SWAY_MAGIC, SWAY_MAGIC_LEN);
	uint32_t pl32 = (uint32_t)plen, ty32 = type;
	memcpy(msg + SWAY_MAGIC_LEN, &pl32, 4);
	memcpy(msg + SWAY_MAGIC_LEN + 4, &ty32, 4);
	if (plen)
		memcpy(msg + SWAY_HDR, payload, plen);
	size_t off = 0;
	while (off < total) {
		ssize_t w = write(fd, msg + off, total - off);
		if (w <= 0) {
			free(msg);
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	free(msg);

	uint8_t hdr[SWAY_HDR];
	size_t got = 0;
	while (got < SWAY_HDR) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, 500) != 1) {
			close(fd);
			return -1;
		}
		ssize_t r = read(fd, hdr + got, SWAY_HDR - got);
		if (r <= 0) {
			close(fd);
			return -1;
		}
		got += (size_t)r;
	}
	if (memcmp(hdr, SWAY_MAGIC, SWAY_MAGIC_LEN) != 0) {
		close(fd);
		return -1;
	}
	uint32_t resp_len = 0, resp_type = 0;
	memcpy(&resp_len, hdr + SWAY_MAGIC_LEN, 4);
	memcpy(&resp_type, hdr + SWAY_MAGIC_LEN + 4, 4);
	if (resp_type != type || resp_len >= out_len) {
		/* Drain and give up; either way the response is unusable. */
		char drain[4096];
		uint32_t left = resp_len;
		while (left > 0) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			if (poll(&pfd, 1, 200) != 1)
				break;
			ssize_t r = read(fd, drain, left < sizeof(drain) ? left : sizeof(drain));
			if (r <= 0)
				break;
			left -= (uint32_t)r;
		}
		close(fd);
		return -1;
	}
	got = 0;
	while (got < resp_len) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, 1000) != 1) {
			close(fd);
			return -1;
		}
		ssize_t r = read(fd, out + got, resp_len - got);
		if (r <= 0) {
			close(fd);
			return -1;
		}
		got += (size_t)r;
	}
	out[got] = '\0';
	close(fd);
	return (int)got;
}
/* --- Minimal JSON token scanning (sway/i3 output, not general purpose) --- */

static const char *sway_find_key(const char *p, const char *end, const char *key)
{
	size_t klen = strlen(key);
	while (p + klen + 1 < end) {
		const char *hit = memmem(p, (size_t)(end - p), key, klen);
		if (!hit)
			return NULL;
		/* Ensure it is a key: preceding char is ',' or '{' and following is ':' */
		const char *q = hit - 1;
		while (q >= p && (*q == ' ' || *q == '\t'))
			q--;
		const char *after = hit + klen;
		while (after < end && (*after == ' ' || *after == '\t'))
			after++;
		if (after < end && *after == ':' && (q < p || *q == ',' || *q == '{'))
			return after + 1;
		p = hit + 1;
	}
	return NULL;
}

static bool sway_value_string(const char **pp, const char *end, char *out, size_t out_len)
{
	const char *p = *pp;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p >= end || *p != '"')
		return false;
	p++;
	size_t o = 0;
	while (p < end && *p != '"') {
		char c = *p;
		if (c == '\\' && p + 1 < end) {
			p++;
			switch (*p) {
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'n': c = ' '; break;
			case 't': c = ' '; break;
			default: c = ' '; break;
			}
		}
		if (o + 1 < out_len)
			out[o++] = c;
		p++;
	}
	out[o] = '\0';
	*pp = p < end ? p + 1 : end;
	return true;
}

static bool sway_value_int(const char **pp, const char *end, int *out)
{
	const char *p = *pp;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	bool neg = false;
	if (p < end && *p == '-') {
		neg = true;
		p++;
	}
	int v = 0;
	bool any = false;
	while (p < end && *p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		p++;
		any = true;
	}
	if (!any)
		return false;
	*out = neg ? -v : v;
	*pp = p;
	return true;
}

static bool sway_value_bool(const char **pp, const char *end, bool *out)
{
	const char *p = *pp;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if ((size_t)(end - p) >= 4 && memcmp(p, "true", 4) == 0) {
		*out = true;
		*pp = p + 4;
		return true;
	}
	if ((size_t)(end - p) >= 5 && memcmp(p, "false", 5) == 0) {
		*out = false;
		*pp = p + 5;
		return true;
	}
	return false;
}

/* Skip over any JSON value starting at v. */
static const char *sway_skip_value(const char *v, const char *end)
{
	while (v < end && (*v == ' ' || *v == '\t'))
		v++;
	if (v >= end)
		return v;
	if (*v == '"') {
		v++;
		while (v < end) {
			char c = *v;
			if (c == '\\' && v + 1 < end) {
				v += 2;
				continue;
			}
			if (c == '"')
				return v + 1;
			v++;
		}
		return end;
	}
	if (*v == '{' || *v == '[') {
		int depth = 0;
		bool istr = false, esc = false;
		for (; v < end; v++) {
			char c = *v;
			if (esc) {
				esc = false;
				continue;
			}
			if (istr) {
				if (c == '\\')
					esc = true;
				else if (c == '"')
					istr = false;
				continue;
			}
			if (c == '"')
				istr = true;
			else if (c == '{' || c == '[')
				depth++;
			else if (c == '}' || c == ']') {
				depth--;
				if (depth == 0)
					return v + 1;
			}
		}
		return end;
	}
	while (v < end && *v != ',' && *v != '}' && *v != ']')
		v++;
	return v;
}

static bool sway_load_state(void)
{
	char buf[8192];
	if (sway_call(MSG_GET_WORKSPACES, NULL, buf, sizeof(buf)) < 0)
		return false;

	sway_ws_count = 0;
	const char *end = buf + strlen(buf);
	const char *p = buf;
	while (sway_ws_count < 64) {
		const char *id_val = sway_find_key(p, end, "\"id\"");
		if (!id_val)
			break;
		const char *num_val = sway_find_key(id_val, end, "\"num\"");
		const char *name_val = sway_find_key(id_val, end, "\"name\"");
		const char *vis_val = sway_find_key(id_val, end, "\"visible\"");
		const char *foc_val = sway_find_key(id_val, end, "\"focused\"");
		if (!num_val || !name_val || !foc_val)
			break;
		int num = 0;
		const char *q = num_val;
		if (!sway_value_int(&q, end, &num))
			break;
		char name[64];
		q = name_val;
		if (!sway_value_string(&q, end, name, sizeof(name)))
			break;
		bool visible = true;
		if (vis_val) {
			q = vis_val;
			sway_value_bool(&q, end, &visible);
		}
		bool focused = false;
		q = foc_val;
		if (!sway_value_bool(&q, end, &focused))
			break;
		wisp_workspace_info_t *w = &sway_ws[sway_ws_count];
		w->id = num > 0 ? num : 0;
		{
			q = id_val;
			int uid = 0;
			if (sway_value_int(&q, end, &uid))
				sway_ws_uid[sway_ws_count] = uid;
			else
				sway_ws_uid[sway_ws_count] = 0;
		}
		/* Sway workspace ids are unique node ids; fall back to them when num is 0/negative. */
		if (w->id <= 0)
			w->id = sway_ws_uid[sway_ws_count];
		w->windows = 0;
		snprintf(w->name, sizeof(w->name), "%s", name);
		if (focused)
			sway_active = w->id;
		sway_ws_count++;
		p = foc_val; /* continue after this entry (focused is the last tracked key in practice) */
		const char *next = strstr(p, "}");
		p = next ? next + 1 : end;
	}
	if (sway_ws_count == 0) {
		sway_ws[0].id = 1;
		sway_ws[0].windows = 0;
		sway_ws_uid[0] = 0;
		snprintf(sway_ws[0].name, sizeof(sway_ws[0].name), "1");
		sway_ws_count = 1;
	}

	/* Single pass over GET_TREE for per-workspace window counts and the
	 * focused window title. Failures leave both at zero/empty. */
	char *tree = malloc(TREE_MAX + 1);
	if (!tree)
		return true;
	int tlen = sway_call(MSG_GET_TREE, NULL, tree, TREE_MAX);
	if (tlen > 0) {
		const char *tend = tree + tlen;
		const char *tp = tree;
		int cur = -1;
		size_t cur_name_off = 0, cur_name_len = 0;
		bool cur_name_seen = false, focus_pending = false;
		char focused_name[512];
		focused_name[0] = '\0';
		while (tp < tend) {
			const char *t = sway_find_key(tp, tend, "\"type\"");
			const char *nid = sway_find_key(tp, tend, "\"id\"");
			const char *nname = sway_find_key(tp, tend, "\"name\"");
			const char *nfocus = sway_find_key(tp, tend, "\"focused\"");
			const char *napp = sway_find_key(tp, tend, "\"app_id\"");
			const char *nwprop = sway_find_key(tp, tend, "\"window_properties\"");

			/* Advance past the earliest of the tracked keys. */
			const char *next = tend;
			const char *cands[] = {t, nid, nname, nfocus, napp, nwprop};
			for (int i = 0; i < 6; i++)
				if (cands[i] && cands[i] < next)
					next = cands[i];
			if (next == tend)
				break;

			if (t && t == next) {
				const char *v = t;
				char tok[32];
				if (sway_value_string(&v, tend, tok, sizeof(tok))) {
					/* Keep cur as armed by the workspace node's id. */
					cur_name_seen = false;
					focus_pending = false;
				}
				tp = v;
				continue;
			}
			if (nid && nid == next) {
				const char *v = nid;
				int id = 0;
				if (sway_value_int(&v, tend, &id)) {
					/* Tree node ids are globally unique; match known workspace uids. */
					for (int i = 0; i < sway_ws_count; i++)
						if (sway_ws_uid[i] && sway_ws_uid[i] == id) {
							cur = i;
							break;
						}
				}
				tp = v;
				continue;
			}
			if (nname && nname == next) {
				const char *v = nname;
				char name[512];
				if (sway_value_string(&v, tend, name, sizeof(name))) {
					cur_name_off = (size_t)(v - tree) - (strlen(name) + 2);
					cur_name_len = strlen(name);
					cur_name_seen = true;
					if (focus_pending) {
						snprintf(focused_name, sizeof(focused_name), "%s", name);
						focus_pending = false;
					}
				}
				tp = v;
				continue;
			}
			if (nfocus && nfocus == next) {
				const char *v = nfocus;
				bool f = false;
				if (sway_value_bool(&v, tend, &f) && f) {
					if (cur_name_seen)
						snprintf(focused_name, sizeof(focused_name), "%.*s", (int)cur_name_len, tree + cur_name_off);
					else
						focus_pending = true;
				}
				tp = v;
				continue;
			}
			if ((napp && napp == next) || (nwprop && nwprop == next)) {
				/* One window per app_id (Wayland) or window_properties (XWayland) key. */
				if (cur >= 0 && cur < sway_ws_count)
					sway_ws[cur].windows++;
				tp = sway_skip_value(next, tend);
				continue;
			}
			/* Unreachable: every candidate key has a branch above. */
			tp = next + 1;
		}
		if (focused_name[0] != '\0') {
			snprintf(sway_title, sizeof(sway_title), "%s", focused_name);
			sway_title_at_us = sway_now_us();
		}
	}
	free(tree);
	return true;
}
static void sway_notify(void)
{
	backend_notify_workspace_event((const wisp_workspace_info_t *)sway_ws,
		sway_ws_count,
		sway_active);
}

static void sway_reload_and_notify(void)
{
	sway_load_state();
	sway_notify();
}

/* We subscribe only to workspace events, so every payload object is one. */
static void sway_process_events(const char *data, size_t n)
{
	size_t len = sway_event_len;
	if (len + n > sizeof(sway_event_buf)) {
		/* Overflow: drop the burst rather than misparse. */
		len = 0;
		if (n > sizeof(sway_event_buf))
			return;
	}
	memcpy(sway_event_buf + len, data, n);
	len += n;

	/* Extract balanced top-level JSON objects (sway sends them without newlines). */
	size_t i = 0;
	while (i < len) {
		if (sway_event_buf[i] != '{') {
			i++;
			continue;
		}
		int depth = 0;
		bool in_str = false, esc = false;
		size_t j = i;
		bool complete = false;
		for (; j < len; j++) {
			char c = sway_event_buf[j];
			if (esc) {
				esc = false;
				continue;
			}
			if (in_str) {
				if (c == '\\')
					esc = true;
				else if (c == '"')
					in_str = false;
				continue;
			}
			if (c == '"')
				in_str = true;
			else if (c == '{')
				depth++;
			else if (c == '}')
				depth--;
			if (depth == 0) {
				complete = true;
				j++;
				break;
			}
		}
		if (!complete)
			break;
		sway_event_buf[j] = '\0';
		sway_reload_and_notify();
		i = j;
	}
	if (i > 0) {
		memmove(sway_event_buf, sway_event_buf + i, len - i);
		len -= i;
	}
	sway_event_len = len;
}

static void sway_try_open_events(void)
{
	if (sway_event_fd >= 0)
		return;
	int fd = sway_connect();
	if (fd < 0)
		return;
	const char *payload = "[\"workspace\"]";
	uint32_t type = MSG_SUBSCRIBE, plen = (uint32_t)strlen(payload);
	uint8_t msg[SWAY_HDR + 16];
	memcpy(msg, SWAY_MAGIC, SWAY_MAGIC_LEN);
	memcpy(msg + SWAY_MAGIC_LEN, &plen, 4);
	memcpy(msg + SWAY_MAGIC_LEN + 4, &type, 4);
	memcpy(msg + SWAY_HDR, payload, plen);
	if (write(fd, msg, SWAY_HDR + plen) != (ssize_t)(SWAY_HDR + plen)) {
		close(fd);
		return;
	}
	char resp[256];
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	if (poll(&pfd, 1, 500) == 1) {
		/* Consume the subscription ack; we don't care about its contents. */
		while (read(fd, resp, sizeof(resp)) > 0)
			break;
	}
	/* Switch to non-blocking reads for the event loop. */
	fcntl(fd, F_SETFL, O_NONBLOCK);
	sway_event_fd = fd;
}

static bool sway_init(void)
{
	const char *sock = getenv("SWAYSOCK");
	if (!sock || !sock[0])
		sock = getenv("I3SOCK");
	if (!sock || !sock[0])
		return false;
	struct sockaddr_un probe;
	if (strlen(sock) >= sizeof(probe.sun_path))
		return false; /* Too long for a unix socket address; degrade to generic. */
	struct stat st;
	if (stat(sock, &st) < 0)
		return false;
	snprintf(sway_sock_path, sizeof(sway_sock_path), "%s", sock);
	char buf[256];
	if (sway_call(MSG_GET_VERSION, NULL, buf, sizeof(buf)) < 0)
		return false;
	sway_load_state();
	sway_try_open_events();
	return true;
}

static void sway_shutdown(void)
{
	if (sway_event_fd >= 0) {
		close(sway_event_fd);
		sway_event_fd = -1;
	}
}

static int sway_active_workspace(void)
{
	return sway_active;
}

static bool sway_list_workspaces(wisp_workspace_info_t *out, int max, int *count)
{
	if (!out || max < 1 || !count)
		return false;
	int n = sway_ws_count < max ? sway_ws_count : max;
	for (int i = 0; i < n; i++) {
		out[i] = sway_ws[i];
	}
	*count = n;
	return true;
}

static bool sway_switch_workspace(int id)
{
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "workspace number %d", id);
	char resp[256];
	if (sway_call(MSG_RUN_COMMAND, cmd, resp, sizeof(resp)) < 0)
		return false;
	bool ok = strstr(resp, "\"success\": true") != NULL;
	if (ok)
		sway_active = id; /* re-syncs on the next workspace event */
	return ok;
}

static bool sway_active_window_title(char *buf, size_t len)
{
	if (!buf || len == 0)
		return false;
	int64_t now = sway_now_us();
	if (sway_title[0] == '\0' || now - sway_title_at_us >= 1000000) {
		/* Refresh from the tree (cheap on typical setups). */
		sway_load_state();
	}
	if (sway_title[0] == '\0') {
		buf[0] = '\0';
		return false;
	}
	size_t n = strlen(sway_title);
	if (n >= len)
		n = len - 1;
	memcpy(buf, sway_title, n);
	buf[n] = '\0';
	return true;
}

static int sway_get_poll_fds(struct pollfd *pfds, int max)
{
	if (!pfds || max < 1)
		return 0;
	if (sway_event_fd < 0)
		return 0;
	pfds[0].fd = sway_event_fd;
	pfds[0].events = POLLIN;
	return 1;
}

static void sway_reconnect(void)
{
	if (sway_event_fd >= 0)
		close(sway_event_fd);
	sway_event_fd = -1;
	sway_event_len = 0;
	sway_try_open_events();
	if (sway_event_fd >= 0)
		sway_reload_and_notify();
}

static void sway_handle_poll(const struct pollfd *pfds, int count)
{
	if (!pfds || count < 1)
		return;
	if (!(pfds[0].revents & (POLLIN | POLLERR | POLLHUP)))
		return;
	if (pfds[0].revents & (POLLERR | POLLHUP)) {
		sway_reconnect();
		return;
	}
	char buf[4096];
	for (;;) {
		ssize_t n = read(sway_event_fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			sway_reconnect();
			break;
		}
		if (n == 0) {
			sway_reconnect();
			break;
		}
		sway_process_events(buf, (size_t)n);
	}
}

const struct wisp_backend sway_backend = {
	.name = "sway",
	.capabilities = WISP_CAP_WORKSPACES | WISP_CAP_ACTIVE_WINDOW | WISP_CAP_WORKSPACE_EVENTS,
	.init = sway_init,
	.shutdown = sway_shutdown,
	.active_workspace = sway_active_workspace,
	.list_workspaces = sway_list_workspaces,
	.switch_workspace = sway_switch_workspace,
	.active_window_title = sway_active_window_title,
	.get_poll_fds = sway_get_poll_fds,
	.handle_poll = sway_handle_poll,
};
