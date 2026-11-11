#define _GNU_SOURCE
#include "services/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <lua/lua.h>
#include <lua/lauxlib.h>
#include <lauxlib.h>

extern lua_State *Lm;

static struct fs_watcher g_watchers[FS_MAX_WATCHERS];
static int g_watcher_seq = 1;
static int g_fs_inotify_fd = -1;

void fs_service_init(void)
{
	for (int i = 0; i < FS_MAX_WATCHERS; i++) {
		memset(&g_watchers[i], 0, sizeof(struct fs_watcher));
		g_watchers[i].wd = -1;
		g_watchers[i].lua_cb_ref = LUA_NOREF;
	}
	g_fs_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (g_fs_inotify_fd < 0) {
		fprintf(stderr, "wisp: fs inotify_init1 failed: %s\n", strerror(errno));
	}
}

void fs_service_shutdown(void)
{
	for (int i = 0; i < FS_MAX_WATCHERS; i++) {
		if (g_watchers[i].active) {
			fs_watcher_close(&g_watchers[i]);
		}
	}
	if (g_fs_inotify_fd >= 0) {
		close(g_fs_inotify_fd);
		g_fs_inotify_fd = -1;
	}
}

char *fs_read(const char *path, size_t max_bytes, size_t *out_len, char *err_buf, size_t err_len)
{
	if (!path || !path[0]) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "empty path");
		return NULL;
	}

	struct stat st;
	if (stat(path, &st) != 0) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
		return NULL;
	}

	if (S_ISDIR(st.st_mode)) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "path is a directory");
		return NULL;
	}

	size_t limit = (max_bytes > 0 && max_bytes < FS_MAX_READ_BYTES) ? max_bytes : FS_MAX_READ_BYTES;
	size_t to_read = (size_t)st.st_size;
	if (to_read > limit) to_read = limit;

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
		return NULL;
	}

	char *buf = malloc(to_read + 1);
	if (!buf) {
		close(fd);
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "out of memory");
		return NULL;
	}

	size_t total = 0;
	while (total < to_read) {
		ssize_t nr = read(fd, buf + total, to_read - total);
		if (nr < 0) {
			if (errno == EINTR) continue;
			free(buf);
			close(fd);
			if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
			return NULL;
		}
		if (nr == 0) break;
		total += (size_t)nr;
	}
	close(fd);
	buf[total] = '\0';
	if (out_len) *out_len = total;
	return buf;
}

bool fs_write(const char *path, const void *data, size_t len, bool append, char *err_buf, size_t err_len)
{
	if (!path || !path[0]) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "empty path");
		return false;
	}

	int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
	int fd = open(path, flags, 0644);
	if (fd < 0) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
		return false;
	}

	if (data && len > 0) {
		size_t written = 0;
		const char *p = (const char *)data;
		while (written < len) {
			ssize_t nw = write(fd, p + written, len - written);
			if (nw < 0) {
				if (errno == EINTR) continue;
				close(fd);
				if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
				return false;
			}
			written += (size_t)nw;
		}
	}
	close(fd);
	return true;
}

bool fs_exists(const char *path)
{
	return (path && path[0] && access(path, F_OK) == 0);
}

bool fs_mkdir(const char *path, bool recursive, char *err_buf, size_t err_len)
{
	if (!path || !path[0]) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "empty path");
		return false;
	}

	if (!recursive) {
		if (mkdir(path, 0755) != 0 && errno != EEXIST) {
			if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
			return false;
		}
		return true;
	}

	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t len = strlen(tmp);
	if (len == 0) return false;
	if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
				return false;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
		return false;
	}
	return true;
}

static bool remove_dir_recursive(const char *path)
{
	DIR *d = opendir(path);
	if (!d) return (rmdir(path) == 0);

	struct dirent *entry;
	char subpath[1024];
	while ((entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);
		struct stat st;
		if (lstat(subpath, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				remove_dir_recursive(subpath);
			} else {
				unlink(subpath);
			}
		}
	}
	closedir(d);
	return (rmdir(path) == 0);
}

bool fs_remove(const char *path, bool recursive, char *err_buf, size_t err_len)
{
	if (!path || !path[0]) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "empty path");
		return false;
	}

	struct stat st;
	if (lstat(path, &st) != 0) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
		return false;
	}

	if (S_ISDIR(st.st_mode)) {
		if (recursive) {
			if (!remove_dir_recursive(path)) {
				if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
				return false;
			}
		} else {
			if (rmdir(path) != 0) {
				if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
				return false;
			}
		}
	} else {
		if (unlink(path) != 0) {
			if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
			return false;
		}
	}
	return true;
}

struct fs_watcher *fs_watch(const char *path, int lua_cb_ref, int app_slot, char *err_buf, size_t err_len)
{
	if (g_fs_inotify_fd < 0) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "inotify not available");
		return NULL;
	}
	if (!path || !path[0]) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "empty path");
		return NULL;
	}

	int slot = -1;
	for (int i = 0; i < FS_MAX_WATCHERS; i++) {
		if (!g_watchers[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "max active watchers (%d) reached", FS_MAX_WATCHERS);
		return NULL;
	}

	uint32_t mask = IN_MODIFY | IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO;
	int wd = inotify_add_watch(g_fs_inotify_fd, path, mask);
	if (wd < 0) {
		if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", strerror(errno));
		return NULL;
	}

	struct fs_watcher *w = &g_watchers[slot];
	w->active = true;
	w->id = g_watcher_seq++;
	w->wd = wd;
	w->app_slot = app_slot;
	w->lua_cb_ref = lua_cb_ref;
	snprintf(w->path, sizeof(w->path), "%s", path);

	return w;
}

void fs_watcher_close(struct fs_watcher *w)
{
	if (!w || !w->active) return;
	if (g_fs_inotify_fd >= 0 && w->wd >= 0) {
		inotify_rm_watch(g_fs_inotify_fd, w->wd);
		w->wd = -1;
	}
	if (w->lua_cb_ref != LUA_NOREF && Lm) {
		luaL_unref(Lm, LUA_REGISTRYINDEX, w->lua_cb_ref);
		w->lua_cb_ref = LUA_NOREF;
	}
	w->active = false;
}

int fs_get_poll_fds(struct pollfd *pfds, int max)
{
	if (!pfds || max <= 0 || g_fs_inotify_fd < 0) return 0;
	int active = 0;
	for (int i = 0; i < FS_MAX_WATCHERS; i++) {
		if (g_watchers[i].active) { active++; break; }
	}
	if (active == 0) return 0;

	pfds[0] = (struct pollfd){ .fd = g_fs_inotify_fd, .events = POLLIN };
	return 1;
}

void fs_handle_poll(const struct pollfd *pfds, int count)
{
	if (!pfds || count <= 0 || g_fs_inotify_fd < 0) return;
	if (!(pfds[0].revents & POLLIN)) return;

	char ibuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t ilen;
	while ((ilen = read(g_fs_inotify_fd, ibuf, sizeof(ibuf))) > 0) {
		for (char *p = ibuf; p < ibuf + ilen; ) {
			struct inotify_event *ev = (struct inotify_event *)p;
			for (int i = 0; i < FS_MAX_WATCHERS; i++) {
				if (g_watchers[i].active && g_watchers[i].wd == ev->wd &&
				    g_watchers[i].lua_cb_ref != LUA_NOREF && Lm) {
					const char *ev_name = "modify";
					if (ev->mask & IN_CREATE) ev_name = "create";
					else if (ev->mask & IN_DELETE) ev_name = "delete";
					else if (ev->mask & IN_ATTRIB) ev_name = "attrib";
					else if (ev->mask & (IN_MOVED_FROM | IN_MOVED_TO)) ev_name = "move";

					const char *name = (ev->len > 0 && ev->name[0]) ? ev->name : g_watchers[i].path;
					lua_rawgeti(Lm, LUA_REGISTRYINDEX, g_watchers[i].lua_cb_ref);
					lua_pushstring(Lm, ev_name);
					lua_pushstring(Lm, g_watchers[i].path);
					lua_pushstring(Lm, name);
					lua_pcall(Lm, 3, 0, 0);
				}
			}
			p += sizeof(struct inotify_event) + ev->len;
		}
	}
}

void fs_cleanup_app(int app_slot)
{
	for (int i = 0; i < FS_MAX_WATCHERS; i++) {
		if (g_watchers[i].active && g_watchers[i].app_slot == app_slot) {
			fs_watcher_close(&g_watchers[i]);
		}
	}
}

void fs_get_metrics(int *out_active_watchers)
{
	int active = 0;
	for (int i = 0; i < FS_MAX_WATCHERS; i++) {
		if (g_watchers[i].active) active++;
	}
	if (out_active_watchers) *out_active_watchers = active;
}
