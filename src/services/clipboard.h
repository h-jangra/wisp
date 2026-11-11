#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>

#define CLIPBOARD_DEFAULT_MAX_ENTRIES 100
#define CLIPBOARD_DEFAULT_MAX_SIZE (64 * 1024)
#define CLIPBOARD_MAX_TRANSFERS 4
#define CLIPBOARD_MAX_SUBSCRIBERS 32

struct clipboard_entry {
	uint32_t id;
	char *text;
	size_t length;
	int64_t timestamp_us;
};

struct clipboard_config {
	int max_entries;
	size_t max_entry_size;
	bool persist;
	char persist_path[256];
};

typedef void (*clipboard_read_cb)(const char *text, const char *err, void *user_data);

void clipboard_init(void);
void clipboard_shutdown(void);

bool clipboard_set(const char *text, size_t len);
void clipboard_clear(void);
const char *clipboard_get_current(size_t *out_len);
bool clipboard_is_sensitive(void);

/* History management */
int clipboard_history_count(void);
const struct clipboard_entry *clipboard_history_get(int idx);
bool clipboard_history_remove(int idx);
void clipboard_history_clear(void);
void clipboard_history_configure(int max_entries, size_t max_size, int persist, const char *path);
void clipboard_history_get_config(struct clipboard_config *cfg);
void clipboard_history_add(const char *text, size_t len, int64_t now_us, bool sensitive);

/* Asynchronous transfer queue */
bool clipboard_request_async(clipboard_read_cb cb, void *user_data, int app_slot, int widget_slot, int lua_ref);
int clipboard_get_poll_fds(struct pollfd *pfds, int max);
void clipboard_handle_poll(const struct pollfd *pfds, int count);
void clipboard_feed_async_data(const char *text, size_t len);

/* Change subscribers */
uint32_t clipboard_subscribe(int lua_ref, int app_slot, int widget_slot);
bool clipboard_unsubscribe(uint32_t sub_id);
void clipboard_notify_subscribers(const char *text);

/* Lifecycle cleanup */
void clipboard_cleanup_app(int app_slot);
void clipboard_cleanup_widget(int widget_slot);

/* Wayland hooks */
void clipboard_wayland_init(void);
void clipboard_wayland_shutdown(void);
bool clipboard_mime_is_supported(const char *mime);
bool clipboard_mime_is_sensitive(const char *mime);
