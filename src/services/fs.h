#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <poll.h>

#define FS_MAX_WATCHERS 32
#define FS_MAX_READ_BYTES (16 * 1024 * 1024)

struct fs_watcher {
	bool active;
	int id;
	int wd;
	int app_slot;
	char path[512];
	int lua_cb_ref;
};

void fs_service_init(void);
void fs_service_shutdown(void);

char *fs_read(const char *path, size_t max_bytes, size_t *out_len, char *err_buf, size_t err_len);
bool fs_write(const char *path, const void *data, size_t len, bool append, char *err_buf, size_t err_len);
bool fs_exists(const char *path);
bool fs_mkdir(const char *path, bool recursive, char *err_buf, size_t err_len);
bool fs_remove(const char *path, bool recursive, char *err_buf, size_t err_len);

struct fs_watcher *fs_watch(const char *path, int lua_cb_ref, int app_slot, char *err_buf, size_t err_len);
void fs_watcher_close(struct fs_watcher *w);

int fs_get_poll_fds(struct pollfd *pfds, int max);
void fs_handle_poll(const struct pollfd *pfds, int count);
void fs_cleanup_app(int app_slot);
void fs_get_metrics(int *out_active_watchers);
