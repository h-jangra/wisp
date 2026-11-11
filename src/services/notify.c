#define _GNU_SOURCE
#include "services/notify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <lua/lua.h>
#include <lua/lauxlib.h>

extern lua_State *Lm;

static sd_bus *g_bus = NULL;
static struct notify_handle g_notifs[NOTIFY_MAX_ACTIVE];
static int g_notif_total_sent = 0;

static int on_notification_closed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	uint32_t id = 0;
	uint32_t reason = 0;
	if (sd_bus_message_read(m, "uu", &id, &reason) < 0)
		return 0;

	for (int i = 0; i < NOTIFY_MAX_ACTIVE; i++) {
		if (g_notifs[i].active && g_notifs[i].id == id) {
			if (g_notifs[i].lua_close_ref != LUA_NOREF && Lm) {
				int ref = g_notifs[i].lua_close_ref;
				g_notifs[i].lua_close_ref = LUA_NOREF;
				lua_rawgeti(Lm, LUA_REGISTRYINDEX, ref);
				lua_pushinteger(Lm, (lua_Integer)reason);
				lua_pcall(Lm, 1, 0, 0);
				luaL_unref(Lm, LUA_REGISTRYINDEX, ref);
			}
			g_notifs[i].active = false;
			break;
		}
	}
	return 0;
}

static int on_action_invoked(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	uint32_t id = 0;
	const char *action_key = NULL;
	if (sd_bus_message_read(m, "us", &id, &action_key) < 0)
		return 0;

	for (int i = 0; i < NOTIFY_MAX_ACTIVE; i++) {
		if (g_notifs[i].active && g_notifs[i].id == id) {
			if (g_notifs[i].lua_action_ref != LUA_NOREF && Lm) {
				lua_rawgeti(Lm, LUA_REGISTRYINDEX, g_notifs[i].lua_action_ref);
				lua_pushstring(Lm, action_key ? action_key : "");
				lua_pcall(Lm, 1, 0, 0);
			}
			break;
		}
	}
	return 0;
}

void notify_service_init(void)
{
	for (int i = 0; i < NOTIFY_MAX_ACTIVE; i++) {
		g_notifs[i].active = false;
		g_notifs[i].id = 0;
		g_notifs[i].lua_close_ref = LUA_NOREF;
		g_notifs[i].lua_action_ref = LUA_NOREF;
	}

	int r = sd_bus_open_user(&g_bus);
	if (r < 0 || !g_bus) {
		g_bus = NULL;
		return;
	}

	sd_bus_match_signal(g_bus, NULL, "org.freedesktop.Notifications",
	                    "/org/freedesktop/Notifications",
	                    "org.freedesktop.Notifications",
	                    "NotificationClosed", on_notification_closed, NULL);

	sd_bus_match_signal(g_bus, NULL, "org.freedesktop.Notifications",
	                    "/org/freedesktop/Notifications",
	                    "org.freedesktop.Notifications",
	                    "ActionInvoked", on_action_invoked, NULL);
}

void notify_service_shutdown(void)
{
	for (int i = 0; i < NOTIFY_MAX_ACTIVE; i++) {
		if (g_notifs[i].active) {
			notify_close(&g_notifs[i]);
		}
	}
	if (g_bus) {
		sd_bus_close(g_bus);
		sd_bus_unref(g_bus);
		g_bus = NULL;
	}
}

bool notify_is_available(void)
{
	return (g_bus != NULL);
}

struct notify_handle *notify_send(const char *title, const char *body,
                                 const char *urgency, int timeout_ms,
                                 const char *app_name, const char *icon,
                                 char *const actions[], int app_slot,
                                 char *err_buf, size_t err_len)
{
	if (!g_bus) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "D-Bus session bus unavailable");
		return NULL;
	}

	int slot = -1;
	for (int i = 0; i < NOTIFY_MAX_ACTIVE; i++) {
		if (!g_notifs[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "max active notifications (%d) reached", NOTIFY_MAX_ACTIVE);
		return NULL;
	}

	sd_bus_message *m = NULL;
	int r = sd_bus_message_new_method_call(g_bus, &m,
	                                       "org.freedesktop.Notifications",
	                                       "/org/freedesktop/Notifications",
	                                       "org.freedesktop.Notifications",
	                                       "Notify");
	if (r < 0 || !m) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "failed to create notify D-Bus message: %s", strerror(-r));
		return NULL;
	}

	const char *aname = (app_name && app_name[0]) ? app_name : "Wisp";
	const char *aicon = (icon && icon[0]) ? icon : "";
	const char *sum = (title && title[0]) ? title : "";
	const char *bod = (body && body[0]) ? body : "";

	/* 1. app_name, 2. replaces_id (0), 3. app_icon, 4. summary, 5. body */
	sd_bus_message_append(m, "suss", aname, (uint32_t)0, aicon, sum);
	sd_bus_message_append(m, "s", bod);

	/* 6. actions (as) */
	sd_bus_message_open_container(m, 'a', "s");
	if (actions) {
		for (char *const *ap = actions; *ap; ap++) {
			sd_bus_message_append(m, "s", *ap);
		}
	}
	sd_bus_message_close_container(m);

	/* 7. hints (a{sv}) */
	uint8_t urg_byte = 1; /* 0=low, 1=normal, 2=critical */
	if (urgency) {
		if (strcmp(urgency, "low") == 0) urg_byte = 0;
		else if (strcmp(urgency, "critical") == 0) urg_byte = 2;
	}

	sd_bus_message_open_container(m, 'a', "{sv}");
	sd_bus_message_open_container(m, 'e', "sv");
	sd_bus_message_append(m, "s", "urgency");
	sd_bus_message_open_container(m, 'v', "y");
	sd_bus_message_append(m, "y", urg_byte);
	sd_bus_message_close_container(m);
	sd_bus_message_close_container(m);
	sd_bus_message_close_container(m);

	/* 8. expire_timeout (i) */
	int32_t tout = (timeout_ms >= 0) ? timeout_ms : -1;
	sd_bus_message_append(m, "i", tout);

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	r = sd_bus_call(g_bus, m, 2000000, &error, &reply);
	sd_bus_message_unref(m);

	if (r < 0) {
		if (err_buf && err_len > 0)
			snprintf(err_buf, err_len, "notify call failed: %s", error.message ? error.message : strerror(-r));
		sd_bus_error_free(&error);
		return NULL;
	}

	uint32_t notif_id = 0;
	sd_bus_message_read(reply, "u", &notif_id);
	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);

	struct notify_handle *handle = &g_notifs[slot];
	handle->active = true;
	handle->id = notif_id;
	handle->app_slot = app_slot;
	handle->lua_close_ref = LUA_NOREF;
	handle->lua_action_ref = LUA_NOREF;

	g_notif_total_sent++;
	return handle;
}

bool notify_close(struct notify_handle *n)
{
	if (!n || !n->active || !g_bus || n->id == 0)
		return false;

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_call_method(g_bus,
	                   "org.freedesktop.Notifications",
	                   "/org/freedesktop/Notifications",
	                   "org.freedesktop.Notifications",
	                   "CloseNotification",
	                   &error, NULL, "u", n->id);
	sd_bus_error_free(&error);

	if (n->lua_close_ref != LUA_NOREF && Lm) {
		luaL_unref(Lm, LUA_REGISTRYINDEX, n->lua_close_ref);
		n->lua_close_ref = LUA_NOREF;
	}
	if (n->lua_action_ref != LUA_NOREF && Lm) {
		luaL_unref(Lm, LUA_REGISTRYINDEX, n->lua_action_ref);
		n->lua_action_ref = LUA_NOREF;
	}
	n->active = false;
	return true;
}

void notify_set_close_cb(struct notify_handle *n, int lua_cb_ref)
{
	if (!n || !n->active) return;
	if (n->lua_close_ref != LUA_NOREF && Lm)
		luaL_unref(Lm, LUA_REGISTRYINDEX, n->lua_close_ref);
	n->lua_close_ref = lua_cb_ref;
}

void notify_set_action_cb(struct notify_handle *n, int lua_cb_ref)
{
	if (!n || !n->active) return;
	if (n->lua_action_ref != LUA_NOREF && Lm)
		luaL_unref(Lm, LUA_REGISTRYINDEX, n->lua_action_ref);
	n->lua_action_ref = lua_cb_ref;
}

int notify_get_poll_fds(struct pollfd *pfds, int max)
{
	if (!g_bus || !pfds || max <= 0) return 0;
	int fd = sd_bus_get_fd(g_bus);
	if (fd < 0) return 0;

	int events = sd_bus_get_events(g_bus);
	pfds[0] = (struct pollfd){ .fd = fd, .events = (short)events };
	return 1;
}

void notify_handle_poll(const struct pollfd *pfds, int count)
{
	(void)pfds;
	(void)count;
	notify_service_tick();
}

void notify_service_tick(void)
{
	if (!g_bus) return;
	while (sd_bus_process(g_bus, NULL) > 0) {
		/* process pending messages / signals */
	}
}

void notify_cleanup_app(int app_slot)
{
	for (int i = 0; i < NOTIFY_MAX_ACTIVE; i++) {
		if (g_notifs[i].active && g_notifs[i].app_slot == app_slot) {
			notify_close(&g_notifs[i]);
		}
	}
}

void notify_get_metrics(bool *out_available, int *out_active, int *out_total_sent)
{
	if (out_available) *out_available = (g_bus != NULL);
	int active = 0;
	for (int i = 0; i < NOTIFY_MAX_ACTIVE; i++) {
		if (g_notifs[i].active) active++;
	}
	if (out_active) *out_active = active;
	if (out_total_sent) *out_total_sent = g_notif_total_sent;
}
