#include "backends/backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

typedef struct {
	int id;
	int windows;
	char name[64];
} cached_ws_t;

static int hypr_event_fd = -1;
static cached_ws_t hypr_cached_workspaces[64];
static int hypr_cached_count = 0;
static int hypr_cached_active = 1;

static char hypr_timed_title[512];
static int64_t hypr_timed_title_at_us = 0;

static const char *hypr_runtime_path(char *out, size_t len, const char *leaf)
{
	const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	if (!sig || !xdg) return NULL;
	if (snprintf(out, len, "%s/hypr/%s/%s", xdg, sig, leaf) >= (int)len) return NULL;
	return out;
}

/* Send a request to the Hyprland command socket and read the full reply.
 * Replaces fork+exec'ing hyprctl on the hot path. One-off blocking call
 * with a 500ms timeout so a wedged compositor can't hang the UI forever;
 * on failure callers see buf[0] == '\0'. */
static char *hypr_query(const char *req, char *buf, size_t len)
{
	if (!buf || len == 0) return NULL;
	buf[0] = '\0';
	char path[512];
	if (!hypr_runtime_path(path, sizeof(path), ".socket.sock")) return NULL;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return NULL;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); return NULL; }
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return NULL; }
	size_t rlen = strlen(req);
	if (write(fd, req, rlen) != (ssize_t)rlen) { close(fd); return NULL; }
	size_t off = 0;
	while (off + 1 < len) {
		ssize_t r = read(fd, buf + off, len - off - 1);
		if (r <= 0) break;
		off += (size_t)r;
	}
	buf[off] = '\0';
	close(fd);
	return buf;
}

static int64_t hypr_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Send a `dispatch <args>` request and read the one-line response.
 * Hyprland >= 0.53 (Lua config) evals args as hl.dispatch(...), so a
 * workspace switch needs the Lua dispatcher call. Older Hyprland parsed
 * `workspace N` directly. The reply is "ok" or an error string; we only
 * trust "ok". Bounded at ~150ms; called on click, never in the poll loop. */
static bool hypr_dispatch(const char *args)
{
	char path[512];
	if (!hypr_runtime_path(path, sizeof(path), ".socket.sock")) return false;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return false;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); return false; }
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	bool ok = false;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (errno == EINPROGRESS) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			if (poll(&pfd, 1, 25) != 1 || !(pfd.revents & POLLOUT)) { close(fd); return false; }
		} else {
			close(fd);
			return false;
		}
	}
	char buf[256];
	int n = snprintf(buf, sizeof(buf), "dispatch %s", args);
	if (n > 0 && (size_t)n < sizeof(buf) && write(fd, buf, (size_t)n) == (ssize_t)n) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, 150) == 1 && (pfd.revents & POLLIN)) {
			char resp[512];
			ssize_t r = recv(fd, resp, sizeof(resp) - 1, 0);
			if (r > 0) { resp[r] = '\0'; ok = strncmp(resp, "ok", 2) == 0; }
		}
	}
	close(fd);
	return ok;
}

static bool hypr_load_state(void)
{
	char buf[8192];
	hypr_query("j/workspaces", buf, sizeof(buf));
	hypr_cached_count = 0;
	char *p = buf;
	while (hypr_cached_count < 64) {
		char *idp = strstr(p, "\"id\"");
		if (!idp) break;
		idp = strchr(idp, ':');
		if (!idp) break;
		int id = atoi(idp + 1);
		char *wp = strstr(idp, "\"windows\"");
		int wins = 0;
		if (wp) { wp = strchr(wp, ':'); if (wp) wins = atoi(wp + 1); }
		hypr_cached_workspaces[hypr_cached_count].id = id;
		hypr_cached_workspaces[hypr_cached_count].windows = wins;
		snprintf(hypr_cached_workspaces[hypr_cached_count].name, sizeof(hypr_cached_workspaces[hypr_cached_count].name), "%d", id);
		hypr_cached_count++;
		p = idp + 1;
	}
	hypr_query("j/activeworkspace", buf, sizeof(buf));
	hypr_cached_active = 1;
	char *id_str = strstr(buf, "\"id\"");
	if (id_str) {
		char *colon = strchr(id_str, ':');
		if (colon) hypr_cached_active = atoi(colon + 1);
	}
	if (hypr_cached_count == 0) {
		hypr_cached_workspaces[0].id = 1;
		hypr_cached_workspaces[0].windows = 0;
		snprintf(hypr_cached_workspaces[0].name, sizeof(hypr_cached_workspaces[0].name), "1");
		hypr_cached_count = 1;
	}
	return true;
}

static void hypr_try_open_events(void)
{
	if (hypr_event_fd >= 0) return;
	const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	if (!sig || !xdg) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/hypr/%s/.socket2.sock", xdg, sig);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); return; }
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return; }
	hypr_event_fd = fd;
}

static bool hypr_init(void)
{
	char buf[64];
	if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) return false;
	buf[0] = '\0';
	hypr_query("j/version", buf, sizeof(buf));
	if (buf[0] == '\0') return false;
	hypr_load_state();
	hypr_try_open_events();
	return true;
}

static void hypr_shutdown(void)
{
	if (hypr_event_fd >= 0) {
		close(hypr_event_fd);
		hypr_event_fd = -1;
	}
}

static int hypr_active_workspace(void)
{
	return hypr_cached_active;
}

static bool hypr_list_workspaces(wisp_workspace_info_t *out, int max, int *count)
{
	if (!out || max < 1 || !count) return false;
	int n = hypr_cached_count < max ? hypr_cached_count : max;
	for (int i = 0; i < n; i++) {
		out[i].id = hypr_cached_workspaces[i].id;
		out[i].windows = hypr_cached_workspaces[i].windows;
		memcpy(out[i].name, hypr_cached_workspaces[i].name, sizeof(out[i].name));
	}
	*count = n;
	return true;
}

static bool hypr_switch_workspace(int id)
{
	char args[128];
	/* Hyprland >= 0.53 (Lua config): dispatcher is a Lua expression. */
	snprintf(args, sizeof(args), "hl.dsp.focus({ workspace = \"%d\" })", id);
	bool ok = hypr_dispatch(args);
	if (!ok) {
		/* Legacy plain-text dispatcher syntax */
		snprintf(args, sizeof(args), "workspace %d", id);
		ok = hypr_dispatch(args);
	}
	if (ok) hypr_cached_active = id; /* re-syncs from socket2 events */
	return ok;
}

static bool hypr_active_window_title(char *buf, size_t len)
{
	if (!buf || len == 0) return false;
	/* Short TTL cache: avoid a socket round-trip for the window title on every redraw. */
	static const int64_t TITLE_TTL_US = 1000000;
	const char *src = NULL;
	int64_t now = hypr_now_us();
	if (hypr_timed_title[0] != '\0' && now - hypr_timed_title_at_us < TITLE_TTL_US) {
		src = hypr_timed_title;
	} else {
		char tmp[1024];
		hypr_query("j/activewindow", tmp, sizeof(tmp));
		char *t = strstr(tmp, "\"title\"");
		if (t) {
			t = strchr(t, ':');
			if (t) {
				t = strchr(t, '"');
				if (t) {
					t++;
					size_t i = 0;
					while (t[i] && t[i] != '"' && i + 1 < sizeof(hypr_timed_title)) { hypr_timed_title[i] = t[i]; i++; }
					hypr_timed_title[i] = '\0';
					hypr_timed_title_at_us = now;
					src = hypr_timed_title;
				}
			}
		}
	}
	if (!src || src[0] == '\0') { buf[0] = '\0'; return false; }
	size_t n = strlen(src);
	if (n >= len) n = len - 1;
	memcpy(buf, src, n);
	buf[n] = '\0';
	return true;
}

static void hypr_notify(void)
{
	backend_notify_workspace_event((const wisp_workspace_info_t *)hypr_cached_workspaces,
		hypr_cached_count,
		hypr_cached_active);
}

static void hypr_parse_event_line(const char *line)
{
	// Hyprland sends e.g. "workspace>>2" or "activewindow>>Title" or
	// "movewindow>>...". Only window/workspace topology events change
	// workspace occupancy, so windowtitle>> is ignored entirely.
	if (strncmp(line, "workspace>>", 11) == 0) {
		int id = atoi(line + 11);
		if (id > 0) hypr_cached_active = id;
		hypr_load_state();
		hypr_notify();
		return;
	}
	if (strncmp(line, "activeworkspace>>", 17) == 0) {
		int id = atoi(line + 17);
		if (id > 0) hypr_cached_active = id;
		hypr_notify();
		return;
	}
	if (strncmp(line, "newwindow>>", 11) == 0 ||
	    strncmp(line, "openwindow>>", 12) == 0 ||
	    strncmp(line, "closewindow>>", 13) == 0 ||
	    strncmp(line, "movewindow>>", 12) == 0 ||
	    strncmp(line, "createworkspace>>", 17) == 0 ||
	    strncmp(line, "destroyworkspace>>", 18) == 0) {
		hypr_load_state();
		hypr_notify();
		return;
	}
}

// Accumulates bytes across read() calls so a line split between two
// reads isn't parsed as two partial events.
static char hypr_event_buf[8192];
static size_t hypr_event_len = 0;

static void hypr_process_events(const char *data, size_t n)
{
	if (n > sizeof(hypr_event_buf) || hypr_event_len + n > sizeof(hypr_event_buf)) {
		// Overflow: drop the burst rather than misparse it. State is
		// re-synced on the next workspace event or reconnect.
		hypr_event_len = 0;
		if (n > sizeof(hypr_event_buf)) return;
	}
	memcpy(hypr_event_buf + hypr_event_len, data, n);
	hypr_event_len += n;
	size_t start = 0;
	for (size_t i = 0; i < hypr_event_len; i++) {
		if (hypr_event_buf[i] != '\n') continue;
		hypr_event_buf[i] = '\0';
		char *line = hypr_event_buf + start;
		size_t l = strcspn(line, "\r");
		line[l] = '\0';
		if (line[0]) hypr_parse_event_line(line);
		start = i + 1;
	}
	if (start > 0) {
		memmove(hypr_event_buf, hypr_event_buf + start, hypr_event_len - start);
		hypr_event_len -= start;
	}
}

static int hypr_get_poll_fds(struct pollfd *pfds, int max)
{
	if (!pfds || max < 1) return 0;
	if (hypr_event_fd < 0) return 0;
	pfds[0].fd = hypr_event_fd;
	pfds[0].events = POLLIN;
	return 1;
}

static void hypr_reconnect(void)
{
	if (hypr_event_fd >= 0) close(hypr_event_fd);
	hypr_event_fd = -1;
	hypr_event_len = 0;
	hypr_try_open_events();
	if (hypr_event_fd >= 0) {
		// Re-sync state after a compositor restart and tell subscribers.
		hypr_load_state();
		hypr_notify();
	}
}

static void hypr_handle_poll(const struct pollfd *pfds, int count)
{
	if (!pfds || count < 1) return;
	if (!(pfds[0].revents & (POLLIN | POLLERR | POLLHUP))) return;
	if (pfds[0].revents & (POLLERR | POLLHUP)) {
		hypr_reconnect();
		return;
	}
	char buf[4096];
	for (;;) {
		ssize_t n = read(hypr_event_fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			hypr_reconnect();
			break;
		}
		if (n == 0) {
			hypr_reconnect();
			break;
		}
		hypr_process_events(buf, (size_t)n);
		break;
	}
}

const struct wisp_backend hyprland_backend = {
	.name = "hyprland",
	.capabilities = WISP_CAP_WORKSPACES | WISP_CAP_ACTIVE_WINDOW | WISP_CAP_WORKSPACE_EVENTS,
	.init = hypr_init,
	.shutdown = hypr_shutdown,
	.active_workspace = hypr_active_workspace,
	.list_workspaces = hypr_list_workspaces,
	.switch_workspace = hypr_switch_workspace,
	.active_window_title = hypr_active_window_title,
	.get_poll_fds = hypr_get_poll_fds,
	.handle_poll = hypr_handle_poll,
};
