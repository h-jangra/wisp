#define _GNU_SOURCE
#include "services/clipboard.h"
#include "services/system.h"
#include "wayland/wayland.h"
#include "scripting/scripting.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

struct clip_transfer {
	int fd;
	char *buf;
	size_t len;
	size_t cap;
	bool is_change_read;
	bool sensitive;
	bool active;
};

struct clip_getter {
	clipboard_read_cb native_cb;
	void *user_data;
	int lua_cb_ref;
	int app_slot;
	int widget_slot;
	bool active;
};

struct clip_subscriber {
	uint32_t id;
	int lua_cb_ref;
	int app_slot;
	int widget_slot;
	bool active;
};

static struct clipboard_config g_config = {
	.max_entries = CLIPBOARD_DEFAULT_MAX_ENTRIES,
	.max_entry_size = CLIPBOARD_DEFAULT_MAX_SIZE,
	.persist = false,
	.persist_path = {0}
};

static char *g_current_text = NULL;
static size_t g_current_len = 0;
static bool g_has_selection = false;
static bool g_is_sensitive = false;

static struct clipboard_entry *g_history = NULL;
static int g_history_count = 0;
static int g_history_cap = 0;
static uint32_t g_next_history_id = 1;

static struct clip_transfer g_transfers[CLIPBOARD_MAX_TRANSFERS];
static struct clip_getter g_getters[16];
static int g_getter_count = 0;

static struct clip_subscriber g_subs[CLIPBOARD_MAX_SUBSCRIBERS];
static int g_sub_count = 0;
static uint32_t g_next_sub_id = 1;

/* Active Wayland offers */
static struct zwlr_data_control_offer_v1 *g_dc_active_offer = NULL;
static char g_dc_chosen_mime[64] = {0};
static bool g_dc_has_supported_mime = false;
static bool g_dc_is_sensitive = false;

static struct wl_data_offer *g_wl_active_offer = NULL;
static char g_wl_chosen_mime[64] = {0};
static bool g_wl_has_supported_mime = false;
static bool g_wl_is_sensitive = false;

/* Active Wayland data sources when Wisp owns selection */
static struct zwlr_data_control_source_v1 *g_dc_source = NULL;
static struct wl_data_source *g_wl_source = NULL;

/* MIME helpers */
bool clipboard_mime_is_supported(const char *mime)
{
	if (!mime)
		return false;
	if (strcmp(mime, "text/plain;charset=utf-8") == 0)
		return true;
	if (strcmp(mime, "text/plain") == 0)
		return true;
	if (strcmp(mime, "UTF8_STRING") == 0)
		return true;
	if (strcmp(mime, "STRING") == 0)
		return true;
	return false;
}

bool clipboard_mime_is_sensitive(const char *mime)
{
	if (!mime)
		return false;
	if (strcasestr(mime, "password") != NULL)
		return true;
	if (strcasestr(mime, "keepass") != NULL)
		return true;
	if (strcasestr(mime, "1password") != NULL)
		return true;
	if (strcmp(mime, "x-kde-passwordManagerHint") == 0)
		return true;
	return false;
}

/* Forward declarations */
static void start_offer_read(bool is_change_read);
static void persist_save(void);
static void persist_load(void);

void clipboard_init(void)
{
	g_config.max_entries = CLIPBOARD_DEFAULT_MAX_ENTRIES;
	g_config.max_entry_size = CLIPBOARD_DEFAULT_MAX_SIZE;
	g_config.persist = false;
	g_config.persist_path[0] = '\0';

	g_current_text = NULL;
	g_current_len = 0;
	g_has_selection = false;
	g_is_sensitive = false;

	g_history = NULL;
	g_history_count = 0;
	g_history_cap = 0;
	g_next_history_id = 1;

	memset(g_transfers, 0, sizeof(g_transfers));
	for (int i = 0; i < CLIPBOARD_MAX_TRANSFERS; i++)
		g_transfers[i].fd = -1;

	memset(g_getters, 0, sizeof(g_getters));
	for (int i = 0; i < 16; i++)
		g_getters[i].lua_cb_ref = LUA_NOREF;
	g_getter_count = 0;

	memset(g_subs, 0, sizeof(g_subs));
	for (int i = 0; i < CLIPBOARD_MAX_SUBSCRIBERS; i++)
		g_subs[i].lua_cb_ref = LUA_NOREF;
	g_sub_count = 0;
	g_next_sub_id = 1;
}

void clipboard_shutdown(void)
{
	clipboard_history_clear();
	free(g_history);
	g_history = NULL;
	g_history_cap = 0;

	if (g_current_text) {
		free(g_current_text);
		g_current_text = NULL;
	}
	g_current_len = 0;
	g_has_selection = false;

	for (int i = 0; i < CLIPBOARD_MAX_TRANSFERS; i++) {
		if (g_transfers[i].active) {
			if (g_transfers[i].fd >= 0)
				close(g_transfers[i].fd);
			free(g_transfers[i].buf);
			g_transfers[i].active = false;
			g_transfers[i].fd = -1;
		}
	}

	for (int i = 0; i < 16; i++) {
		if (g_getters[i].active) {
			if (Lm && g_getters[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_getters[i].lua_cb_ref);
			g_getters[i].active = false;
			g_getters[i].lua_cb_ref = LUA_NOREF;
		}
	}
	g_getter_count = 0;

	for (int i = 0; i < CLIPBOARD_MAX_SUBSCRIBERS; i++) {
		if (g_subs[i].active) {
			if (Lm && g_subs[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
			g_subs[i].active = false;
			g_subs[i].lua_cb_ref = LUA_NOREF;
		}
	}
	g_sub_count = 0;

	if (g_dc_source) {
		zwlr_data_control_source_v1_destroy(g_dc_source);
		g_dc_source = NULL;
	}
	if (g_wl_source) {
		wl_data_source_destroy(g_wl_source);
		g_wl_source = NULL;
	}
	if (g_dc_active_offer) {
		zwlr_data_control_offer_v1_destroy(g_dc_active_offer);
		g_dc_active_offer = NULL;
	}
	if (g_wl_active_offer) {
		wl_data_offer_destroy(g_wl_active_offer);
		g_wl_active_offer = NULL;
	}
}

const char *clipboard_get_current(size_t *out_len)
{
	if (!g_has_selection) {
		if (out_len) *out_len = 0;
		return NULL;
	}
	if (out_len) *out_len = g_current_len;
	return g_current_text ? g_current_text : "";
}

bool clipboard_is_sensitive(void)
{
	return g_is_sensitive;
}

/* --- History Foundation --- */

int clipboard_history_count(void)
{
	return g_history_count;
}

const struct clipboard_entry *clipboard_history_get(int idx)
{
	if (idx < 0 || idx >= g_history_count)
		return NULL;
	return &g_history[idx];
}

bool clipboard_history_remove(int idx)
{
	if (idx < 0 || idx >= g_history_count)
		return false;

	free(g_history[idx].text);
	for (int i = idx; i < g_history_count - 1; i++)
		g_history[i] = g_history[i + 1];

	g_history_count--;
	if (g_config.persist)
		persist_save();
	return true;
}

void clipboard_history_clear(void)
{
	for (int i = 0; i < g_history_count; i++)
		free(g_history[i].text);
	g_history_count = 0;
	if (g_config.persist)
		persist_save();
}

void clipboard_history_configure(int max_entries, size_t max_size, int persist, const char *path)
{
	if (max_entries > 0 && max_entries <= 1000)
		g_config.max_entries = max_entries;
	if (max_size >= 256 && max_size <= 10 * 1024 * 1024)
		g_config.max_entry_size = max_size;
	if (persist >= 0)
		g_config.persist = (persist != 0);
	if (path && path[0]) {
		strncpy(g_config.persist_path, path, sizeof(g_config.persist_path) - 1);
		g_config.persist_path[sizeof(g_config.persist_path) - 1] = '\0';
	}

	/* Prune existing entries if max_entries decreased */
	while (g_history_count > g_config.max_entries) {
		free(g_history[g_history_count - 1].text);
		g_history_count--;
	}

	if (g_config.persist)
		persist_load();
}

void clipboard_history_get_config(struct clipboard_config *cfg)
{
	if (cfg)
		*cfg = g_config;
}

void clipboard_history_add(const char *text, size_t len, int64_t now_us, bool sensitive)
{
	if (!text || len == 0 || sensitive)
		return;

	/* Enforce max entry size */
	if (len > g_config.max_entry_size)
		len = g_config.max_entry_size;

	/* Deduplication of consecutive identical entries */
	if (g_history_count > 0 && g_history[0].length == len &&
	    memcmp(g_history[0].text, text, len) == 0) {
		g_history[0].timestamp_us = now_us;
		return;
	}

	/* Ensure capacity */
	if (g_history_cap < g_config.max_entries) {
		int new_cap = g_config.max_entries;
		struct clipboard_entry *new_hist = realloc(g_history, sizeof(struct clipboard_entry) * new_cap);
		if (!new_hist)
			return;
		g_history = new_hist;
		g_history_cap = new_cap;
	}

	/* If at limit, evict oldest entry */
	if (g_history_count >= g_config.max_entries) {
		free(g_history[g_history_count - 1].text);
		g_history_count--;
	}

	/* Shift down to insert at index 0 (most recent first) */
	for (int i = g_history_count; i > 0; i--)
		g_history[i] = g_history[i - 1];

	char *copied = malloc(len + 1);
	if (!copied)
		return;
	memcpy(copied, text, len);
	copied[len] = '\0';

	g_history[0].id = g_next_history_id++;
	g_history[0].text = copied;
	g_history[0].length = len;
	g_history[0].timestamp_us = now_us;
	g_history_count++;

	if (g_config.persist)
		persist_save();
}

static void persist_save(void)
{
	if (!g_config.persist || !g_config.persist_path[0])
		return;
	FILE *f = fopen(g_config.persist_path, "w");
	if (!f)
		return;
	for (int i = 0; i < g_history_count; i++) {
		fprintf(f, "%u\t%zu\t%ld\n", g_history[i].id, g_history[i].length, (long)g_history[i].timestamp_us);
		fwrite(g_history[i].text, 1, g_history[i].length, f);
		fputc('\n', f);
	}
	fclose(f);
}

static void persist_load(void)
{
	if (!g_config.persist || !g_config.persist_path[0])
		return;
	FILE *f = fopen(g_config.persist_path, "r");
	if (!f)
		return;
	clipboard_history_clear();

	uint32_t id;
	size_t len;
	long ts;
	while (fscanf(f, "%u\t%zu\t%ld\n", &id, &len, &ts) == 3) {
		if (len > g_config.max_entry_size)
			len = g_config.max_entry_size;
		char *buf = malloc(len + 1);
		if (!buf)
			break;
		size_t read_bytes = fread(buf, 1, len, f);
		buf[read_bytes] = '\0';
		int ch = fgetc(f);
		(void)ch;

		if (g_history_count < g_config.max_entries) {
			if (g_history_count >= g_history_cap) {
				g_history_cap = g_config.max_entries;
				g_history = realloc(g_history, sizeof(struct clipboard_entry) * g_history_cap);
			}
			g_history[g_history_count].id = id;
			g_history[g_history_count].text = buf;
			g_history[g_history_count].length = read_bytes;
			g_history[g_history_count].timestamp_us = ts;
			g_history_count++;
			if (id >= g_next_history_id)
				g_next_history_id = id + 1;
		} else {
			free(buf);
			break;
		}
	}
	fclose(f);
}

/* --- Subscribers --- */

uint32_t clipboard_subscribe(int lua_ref, int app_slot, int widget_slot)
{
	for (int i = 0; i < CLIPBOARD_MAX_SUBSCRIBERS; i++) {
		if (!g_subs[i].active) {
			g_subs[i].id = g_next_sub_id++;
			g_subs[i].lua_cb_ref = lua_ref;
			g_subs[i].app_slot = app_slot;
			g_subs[i].widget_slot = widget_slot;
			g_subs[i].active = true;
			g_sub_count++;
			return g_subs[i].id;
		}
	}
	return 0;
}

bool clipboard_unsubscribe(uint32_t sub_id)
{
	if (sub_id == 0)
		return false;
	for (int i = 0; i < CLIPBOARD_MAX_SUBSCRIBERS; i++) {
		if (g_subs[i].active && g_subs[i].id == sub_id) {
			if (Lm && g_subs[i].lua_cb_ref != LUA_NOREF) {
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
				g_subs[i].lua_cb_ref = LUA_NOREF;
			}
			g_subs[i].active = false;
			g_sub_count--;
			return true;
		}
	}
	return false;
}

void clipboard_notify_subscribers(const char *text)
{
	if (!Lm || g_sub_count == 0)
		return;

	for (int i = 0; i < CLIPBOARD_MAX_SUBSCRIBERS; i++) {
		struct clip_subscriber *s = &g_subs[i];
		if (!s->active || s->lua_cb_ref == LUA_NOREF)
			continue;

		lua_ticks_total = LUA_TICKS_DEFAULT;
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, s->lua_cb_ref);
		if (lua_isfunction(Lm, -1)) {
			if (text)
				lua_pushstring(Lm, text);
			else
				lua_pushnil(Lm);
			if (lua_pcall(Lm, 1, 0, 0) != LUA_OK) {
				fprintf(stderr, "wisp: clipboard on_change error: %s\n",
					lua_tostring(Lm, -1));
				lua_pop(Lm, 1);
			}
		} else {
			lua_pop(Lm, 1);
		}
	}
}

/* --- Wayland Source Event Listeners --- */

static void dc_source_send(void *data, struct zwlr_data_control_source_v1 *source,
			   const char *mime_type, int32_t fd)
{
	(void)data; (void)source; (void)mime_type;
	if (g_current_text && g_current_len > 0) {
		/* Nonblocking write of current selection */
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		size_t written = 0;
		while (written < g_current_len) {
			ssize_t r = write(fd, g_current_text + written, g_current_len - written);
			if (r > 0) {
				written += r;
			} else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				break;
			} else {
				break;
			}
		}
	}
	close(fd);
}

static void dc_source_cancelled(void *data, struct zwlr_data_control_source_v1 *source)
{
	(void)data;
	if (g_dc_source == source) {
		zwlr_data_control_source_v1_destroy(g_dc_source);
		g_dc_source = NULL;
	}
}

static const struct zwlr_data_control_source_v1_listener dc_source_listener = {
	.send = dc_source_send,
	.cancelled = dc_source_cancelled,
};

static void wl_source_send(void *data, struct wl_data_source *source,
			   const char *mime_type, int32_t fd)
{
	(void)data; (void)source; (void)mime_type;
	if (g_current_text && g_current_len > 0) {
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		size_t written = 0;
		while (written < g_current_len) {
			ssize_t r = write(fd, g_current_text + written, g_current_len - written);
			if (r > 0) {
				written += r;
			} else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				break;
			} else {
				break;
			}
		}
	}
	close(fd);
}

static void wl_source_cancelled(void *data, struct wl_data_source *source)
{
	(void)data;
	if (g_wl_source == source) {
		wl_data_source_destroy(g_wl_source);
		g_wl_source = NULL;
	}
}

static const struct wl_data_source_listener wl_source_listener = {
	.target = NULL,
	.send = wl_source_send,
	.cancelled = wl_source_cancelled,
};

/* --- Setting and Clearing Clipboard --- */

bool clipboard_set(const char *text, size_t len)
{
	if (!text) {
		clipboard_clear();
		return true;
	}

	if (len > g_config.max_entry_size)
		len = g_config.max_entry_size;

	char *new_buf = malloc(len + 1);
	if (!new_buf)
		return false;
	memcpy(new_buf, text, len);
	new_buf[len] = '\0';

	free(g_current_text);
	g_current_text = new_buf;
	g_current_len = len;
	g_has_selection = true;
	g_is_sensitive = false;

	/* Wayland data control protocol */
	if (wl.data_control_mgr && wl.data_control_dev) {
		if (g_dc_source) {
			zwlr_data_control_source_v1_destroy(g_dc_source);
			g_dc_source = NULL;
		}
		g_dc_source = zwlr_data_control_manager_v1_create_data_source(wl.data_control_mgr);
		if (g_dc_source) {
			zwlr_data_control_source_v1_offer(g_dc_source, "text/plain;charset=utf-8");
			zwlr_data_control_source_v1_offer(g_dc_source, "text/plain");
			zwlr_data_control_source_v1_add_listener(g_dc_source, &dc_source_listener, NULL);
			zwlr_data_control_device_v1_set_selection(wl.data_control_dev, g_dc_source);
		}
	}

	/* Fallback standard Wayland data device protocol */
	if (wl.data_device_mgr && wl.data_device) {
		if (g_wl_source) {
			wl_data_source_destroy(g_wl_source);
			g_wl_source = NULL;
		}
		g_wl_source = wl_data_device_manager_create_data_source(wl.data_device_mgr);
		if (g_wl_source) {
			wl_data_source_offer(g_wl_source, "text/plain;charset=utf-8");
			wl_data_source_offer(g_wl_source, "text/plain");
			wl_data_source_add_listener(g_wl_source, &wl_source_listener, NULL);
			wl_data_device_set_selection(wl.data_device, g_wl_source, wl.last_serial);
		}
	}

	clipboard_history_add(g_current_text, g_current_len, now_us(), false);
	clipboard_notify_subscribers(g_current_text);
	return true;
}

void clipboard_clear(void)
{
	free(g_current_text);
	g_current_text = NULL;
	g_current_len = 0;
	g_has_selection = false;
	g_is_sensitive = false;

	if (wl.data_control_dev) {
		if (g_dc_source) {
			zwlr_data_control_source_v1_destroy(g_dc_source);
			g_dc_source = NULL;
		}
		zwlr_data_control_device_v1_set_selection(wl.data_control_dev, NULL);
	}
	if (wl.data_device) {
		if (g_wl_source) {
			wl_data_source_destroy(g_wl_source);
			g_wl_source = NULL;
		}
		wl_data_device_set_selection(wl.data_device, NULL, wl.last_serial);
	}

	clipboard_notify_subscribers(NULL);
}

/* --- Dispatching Getters --- */

static void notify_getters(const char *text, const char *err)
{
	for (int i = 0; i < 16; i++) {
		if (!g_getters[i].active)
			continue;

		struct clip_getter g = g_getters[i];
		g_getters[i].active = false;
		g_getter_count--;

		if (g.native_cb) {
			g.native_cb(text, err, g.user_data);
		} else if (Lm && g.lua_cb_ref != LUA_NOREF) {
			lua_ticks_total = LUA_TICKS_DEFAULT;
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, g.lua_cb_ref);
			if (lua_isfunction(Lm, -1)) {
				if (text) {
					lua_pushstring(Lm, text);
					lua_pushnil(Lm);
				} else {
					lua_pushnil(Lm);
					if (err)
						lua_pushstring(Lm, err);
					else
						lua_pushnil(Lm);
				}
				if (lua_pcall(Lm, 2, 0, 0) != LUA_OK) {
					fprintf(stderr, "wisp: clipboard get callback error: %s\n",
						lua_tostring(Lm, -1));
					lua_pop(Lm, 1);
				}
			} else {
				lua_pop(Lm, 1);
			}
			luaL_unref(Lm, LUA_REGISTRYINDEX, g.lua_cb_ref);
		}
	}
}

/* --- Active Asynchronous Reading --- */

static void start_offer_read(bool is_change_read)
{
	/* Find a free transfer slot */
	int slot = -1;
	for (int i = 0; i < CLIPBOARD_MAX_TRANSFERS; i++) {
		if (!g_transfers[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return;

	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
		notify_getters(NULL, "failed to create transfer pipe");
		return;
	}

	bool sensitive = false;
	if (g_dc_active_offer && g_dc_has_supported_mime) {
		sensitive = g_dc_is_sensitive;
		zwlr_data_control_offer_v1_receive(g_dc_active_offer, g_dc_chosen_mime, fds[1]);
	} else if (g_wl_active_offer && g_wl_has_supported_mime) {
		sensitive = g_wl_is_sensitive;
		wl_data_offer_receive(g_wl_active_offer, g_wl_chosen_mime, fds[1]);
	} else {
		close(fds[0]);
		close(fds[1]);
		notify_getters(NULL, "unsupported MIME type");
		return;
	}
	close(fds[1]); /* Close write end in our process */

	size_t cap = g_config.max_entry_size;
	char *buf = malloc(cap + 1);
	if (!buf) {
		close(fds[0]);
		notify_getters(NULL, "out of memory for transfer");
		return;
	}

	g_transfers[slot].fd = fds[0];
	g_transfers[slot].buf = buf;
	g_transfers[slot].len = 0;
	g_transfers[slot].cap = cap;
	g_transfers[slot].is_change_read = is_change_read;
	g_transfers[slot].sensitive = sensitive;
	g_transfers[slot].active = true;
}

bool clipboard_request_async(clipboard_read_cb cb, void *user_data, int app_slot, int widget_slot, int lua_ref)
{
	/* If we already hold current selection and no pending remote offer */
	if (g_has_selection && g_current_text && !g_dc_active_offer && !g_wl_active_offer) {
		if (cb) {
			cb(g_current_text, NULL, user_data);
		} else if (Lm && lua_ref != LUA_NOREF) {
			lua_ticks_total = LUA_TICKS_DEFAULT;
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, lua_ref);
			if (lua_isfunction(Lm, -1)) {
				lua_pushstring(Lm, g_current_text);
				lua_pushnil(Lm);
				if (lua_pcall(Lm, 2, 0, 0) != LUA_OK) {
					fprintf(stderr, "wisp: clipboard get callback error: %s\n",
						lua_tostring(Lm, -1));
					lua_pop(Lm, 1);
				}
			} else {
				lua_pop(Lm, 1);
			}
			luaL_unref(Lm, LUA_REGISTRYINDEX, lua_ref);
		}
		return true;
	}

	/* Check if any transfer is in progress */
	bool transfer_running = false;
	for (int i = 0; i < CLIPBOARD_MAX_TRANSFERS; i++) {
		if (g_transfers[i].active) {
			transfer_running = true;
			break;
		}
	}

	/* If clipboard is empty and no Wayland offers and no active transfer */
	if (!g_has_selection && !g_dc_active_offer && !g_wl_active_offer && !transfer_running) {
		if (cb) {
			cb(NULL, NULL, user_data);
		} else if (Lm && lua_ref != LUA_NOREF) {
			lua_ticks_total = LUA_TICKS_DEFAULT;
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, lua_ref);
			if (lua_isfunction(Lm, -1)) {
				lua_pushnil(Lm);
				lua_pushnil(Lm);
				if (lua_pcall(Lm, 2, 0, 0) != LUA_OK) {
					fprintf(stderr, "wisp: clipboard get callback error: %s\n",
						lua_tostring(Lm, -1));
					lua_pop(Lm, 1);
				}
			} else {
				lua_pop(Lm, 1);
			}
			luaL_unref(Lm, LUA_REGISTRYINDEX, lua_ref);
		}
		return true;
	}

	/* Queue getter */
	int slot = -1;
	for (int i = 0; i < 16; i++) {
		if (!g_getters[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return false;

	g_getters[slot].native_cb = cb;
	g_getters[slot].user_data = user_data;
	g_getters[slot].lua_cb_ref = lua_ref;
	g_getters[slot].app_slot = app_slot;
	g_getters[slot].widget_slot = widget_slot;
	g_getters[slot].active = true;
	g_getter_count++;

	/* If a transfer is not already running, start one */
	if (!transfer_running)
		start_offer_read(false);

	return true;
}

int clipboard_get_poll_fds(struct pollfd *pfds, int max)
{
	int count = 0;
	for (int i = 0; i < CLIPBOARD_MAX_TRANSFERS && count < max; i++) {
		if (g_transfers[i].active && g_transfers[i].fd >= 0) {
			pfds[count].fd = g_transfers[i].fd;
			pfds[count].events = POLLIN;
			pfds[count].revents = 0;
			count++;
		}
	}
	return count;
}

void clipboard_handle_poll(const struct pollfd *pfds, int count)
{
	for (int p = 0; p < count; p++) {
		if (pfds[p].fd < 0 || !(pfds[p].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;

		for (int i = 0; i < CLIPBOARD_MAX_TRANSFERS; i++) {
			struct clip_transfer *t = &g_transfers[i];
			if (!t->active || t->fd != pfds[p].fd)
				continue;

			bool done = false;
			if (pfds[p].revents & POLLIN) {
				while (t->len < t->cap) {
					size_t space = t->cap - t->len;
					if (space > 4096) space = 4096;
					ssize_t r = read(t->fd, t->buf + t->len, space);
					if (r > 0) {
						t->len += r;
						if (t->len >= t->cap) {
							done = true;
							break;
						}
					} else if (r == 0) {
						done = true;
						break;
					} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
						if (pfds[p].revents & POLLHUP)
							done = true;
						break;
					} else {
						done = true;
						break;
					}
				}
			} else if (pfds[p].revents & (POLLHUP | POLLERR)) {
				done = true;
			}

			if (done) {
				close(t->fd);
				t->fd = -1;
				t->buf[t->len] = '\0';

				free(g_current_text);
				g_current_text = t->buf;
				g_current_len = t->len;
				g_has_selection = (t->len > 0);
				g_is_sensitive = t->sensitive;

				t->active = false;
				t->buf = NULL;

				if (g_has_selection)
					clipboard_history_add(g_current_text, g_current_len, now_us(), g_is_sensitive);

				notify_getters(g_has_selection ? g_current_text : NULL, NULL);
				if (t->is_change_read)
					clipboard_notify_subscribers(g_has_selection ? g_current_text : NULL);
			}
			break;
		}
	}
}

void clipboard_feed_async_data(const char *text, size_t len)
{
	/* In-memory asynchronous feeder for testing transfer pipes */
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
		return;

	int slot = -1;
	for (int i = 0; i < CLIPBOARD_MAX_TRANSFERS; i++) {
		if (!g_transfers[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}

	if (text && len > 0) {
		ssize_t written = write(fds[1], text, len);
		(void)written;
	}
	close(fds[1]);

	size_t cap = g_config.max_entry_size;
	char *buf = malloc(cap + 1);
	if (!buf) {
		close(fds[0]);
		return;
	}

	g_transfers[slot].fd = fds[0];
	g_transfers[slot].buf = buf;
	g_transfers[slot].len = 0;
	g_transfers[slot].cap = cap;
	g_transfers[slot].is_change_read = false;
	g_transfers[slot].sensitive = false;
	g_transfers[slot].active = true;
}

/* --- Lifecycle Cleanup --- */

void clipboard_cleanup_app(int app_slot)
{
	if (app_slot < 0)
		return;
	for (int i = 0; i < 16; i++) {
		if (g_getters[i].active && g_getters[i].app_slot == app_slot) {
			if (Lm && g_getters[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_getters[i].lua_cb_ref);
			g_getters[i].active = false;
			g_getters[i].lua_cb_ref = LUA_NOREF;
			g_getter_count--;
		}
	}
	for (int i = 0; i < CLIPBOARD_MAX_SUBSCRIBERS; i++) {
		if (g_subs[i].active && g_subs[i].app_slot == app_slot) {
			if (Lm && g_subs[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
			g_subs[i].active = false;
			g_subs[i].lua_cb_ref = LUA_NOREF;
			g_sub_count--;
		}
	}
}

void clipboard_cleanup_widget(int widget_slot)
{
	if (widget_slot < 0)
		return;
	for (int i = 0; i < 16; i++) {
		if (g_getters[i].active && g_getters[i].widget_slot == widget_slot) {
			if (Lm && g_getters[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_getters[i].lua_cb_ref);
			g_getters[i].active = false;
			g_getters[i].lua_cb_ref = LUA_NOREF;
			g_getter_count--;
		}
	}
	for (int i = 0; i < CLIPBOARD_MAX_SUBSCRIBERS; i++) {
		if (g_subs[i].active && g_subs[i].widget_slot == widget_slot) {
			if (Lm && g_subs[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_subs[i].lua_cb_ref);
			g_subs[i].active = false;
			g_subs[i].lua_cb_ref = LUA_NOREF;
			g_sub_count--;
		}
	}
}

/* --- Wayland Offer Listeners --- */

static void dc_offer_offer(void *data, struct zwlr_data_control_offer_v1 *offer,
			   const char *mime_type)
{
	(void)data; (void)offer;
	if (clipboard_mime_is_sensitive(mime_type))
		g_dc_is_sensitive = true;

	if (clipboard_mime_is_supported(mime_type)) {
		if (!g_dc_has_supported_mime || strcmp(mime_type, "text/plain;charset=utf-8") == 0) {
			strncpy(g_dc_chosen_mime, mime_type, sizeof(g_dc_chosen_mime) - 1);
			g_dc_chosen_mime[sizeof(g_dc_chosen_mime) - 1] = '\0';
			g_dc_has_supported_mime = true;
		}
	}
}

static const struct zwlr_data_control_offer_v1_listener dc_offer_listener = {
	.offer = dc_offer_offer,
};

static void dc_device_data_offer(void *data, struct zwlr_data_control_device_v1 *dev,
				 struct zwlr_data_control_offer_v1 *id)
{
	(void)data; (void)dev;
	g_dc_has_supported_mime = false;
	g_dc_is_sensitive = false;
	g_dc_chosen_mime[0] = '\0';
	zwlr_data_control_offer_v1_add_listener(id, &dc_offer_listener, NULL);
}

static void dc_device_selection(void *data, struct zwlr_data_control_device_v1 *dev,
				struct zwlr_data_control_offer_v1 *id)
{
	(void)data; (void)dev;
	if (g_dc_active_offer && g_dc_active_offer != id)
		zwlr_data_control_offer_v1_destroy(g_dc_active_offer);
	g_dc_active_offer = id;

	if (!id) {
		free(g_current_text);
		g_current_text = NULL;
		g_current_len = 0;
		g_has_selection = false;
		g_is_sensitive = false;
		clipboard_notify_subscribers(NULL);
		notify_getters(NULL, NULL);
		return;
	}

	if (g_sub_count > 0 || g_getter_count > 0)
		start_offer_read(true);
}

static void dc_device_primary_selection(void *data, struct zwlr_data_control_device_v1 *dev,
					struct zwlr_data_control_offer_v1 *id)
{
	(void)data; (void)dev; (void)id;
	/* Primary selection can be ignored or handled similarly */
}

static void dc_device_finished(void *data, struct zwlr_data_control_device_v1 *dev)
{
	(void)data;
	if (wl.data_control_dev == dev) {
		zwlr_data_control_device_v1_destroy(wl.data_control_dev);
		wl.data_control_dev = NULL;
	}
}

const struct zwlr_data_control_device_v1_listener data_control_device_listener = {
	.data_offer = dc_device_data_offer,
	.selection = dc_device_selection,
	.finished = dc_device_finished,
	.primary_selection = dc_device_primary_selection,
};

static void wl_offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type)
{
	(void)data; (void)offer;
	if (clipboard_mime_is_sensitive(mime_type))
		g_wl_is_sensitive = true;

	if (clipboard_mime_is_supported(mime_type)) {
		if (!g_wl_has_supported_mime || strcmp(mime_type, "text/plain;charset=utf-8") == 0) {
			strncpy(g_wl_chosen_mime, mime_type, sizeof(g_wl_chosen_mime) - 1);
			g_wl_chosen_mime[sizeof(g_wl_chosen_mime) - 1] = '\0';
			g_wl_has_supported_mime = true;
		}
	}
}

static void wl_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t source_actions)
{
	(void)data; (void)offer; (void)source_actions;
}

static void wl_offer_action(void *data, struct wl_data_offer *offer, uint32_t dnd_action)
{
	(void)data; (void)offer; (void)dnd_action;
}

static const struct wl_data_offer_listener wl_offer_listener = {
	.offer = wl_offer_offer,
	.source_actions = wl_offer_source_actions,
	.action = wl_offer_action,
};

static void wl_data_device_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *id)
{
	(void)data; (void)dev;
	g_wl_has_supported_mime = false;
	g_wl_is_sensitive = false;
	g_wl_chosen_mime[0] = '\0';
	wl_data_offer_add_listener(id, &wl_offer_listener, NULL);
}

static void wl_data_device_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *id)
{
	(void)data; (void)dev;
	/* If data control is already active, prefer data control */
	if (wl.data_control_dev)
		return;

	if (g_wl_active_offer && g_wl_active_offer != id)
		wl_data_offer_destroy(g_wl_active_offer);
	g_wl_active_offer = id;

	if (!id) {
		free(g_current_text);
		g_current_text = NULL;
		g_current_len = 0;
		g_has_selection = false;
		g_is_sensitive = false;
		clipboard_notify_subscribers(NULL);
		notify_getters(NULL, NULL);
		return;
	}

	if (g_sub_count > 0 || g_getter_count > 0)
		start_offer_read(true);
}

const struct wl_data_device_listener data_device_listener = {
	.data_offer = wl_data_device_data_offer,
	.enter = NULL,
	.leave = NULL,
	.motion = NULL,
	.drop = NULL,
	.selection = wl_data_device_selection,
};

void clipboard_wayland_init(void)
{
	if (wl.data_control_mgr && wl.seat && !wl.data_control_dev) {
		wl.data_control_dev = zwlr_data_control_manager_v1_get_data_device(wl.data_control_mgr, wl.seat);
		zwlr_data_control_device_v1_add_listener(wl.data_control_dev, &data_control_device_listener, NULL);
	}
	if (wl.data_device_mgr && wl.seat && !wl.data_device) {
		wl.data_device = wl_data_device_manager_get_data_device(wl.data_device_mgr, wl.seat);
		wl_data_device_add_listener(wl.data_device, &data_device_listener, NULL);
	}
}

void clipboard_wayland_shutdown(void)
{
	if (wl.data_control_dev) {
		zwlr_data_control_device_v1_destroy(wl.data_control_dev);
		wl.data_control_dev = NULL;
	}
	if (wl.data_device) {
		wl_data_device_release(wl.data_device);
		wl.data_device = NULL;
	}
}
