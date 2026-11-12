#include "core/app.h"
#include "core/widget.h"
#include "services/clipboard.h"
#include "services/keyboard.h"
#include "services/screen.h"
#include "services/process.h"
#include "services/fs.h"
#include "services/audio.h"
#include "services/notify.h"
#include "scripting/scripting.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct wisp_app g_apps[MAX_APPS];
int g_app_count = 0;

void app_system_init(void)
{
	memset(g_apps, 0, sizeof(g_apps));
	for (int i = 0; i < MAX_APPS; i++) {
		g_apps[i].lua_init_ref = LUA_NOREF;
		g_apps[i].lua_close_ref = LUA_NOREF;
		g_apps[i].lua_build_ref = LUA_NOREF;
		g_apps[i].lua_env_ref = LUA_NOREF;
		for (int j = 0; j < MAX_APP_WINDOWS; j++)
			g_apps[i].windows[j] = -1;
	}
	g_app_count = 0;
}

struct wisp_app *app_find(const char *name)
{
	if (!name || !name[0])
		return NULL;
	for (int i = 0; i < MAX_APPS; i++) {
		if (g_apps[i].used && strcmp(g_apps[i].name, name) == 0)
			return &g_apps[i];
	}
	size_t nlen = strlen(name);
	for (int i = 0; i < MAX_APPS; i++) {
		if (g_apps[i].used && strncmp(g_apps[i].name, name, nlen) == 0)
			return &g_apps[i];
	}
	return NULL;
}

struct wisp_app *app_create(const char *name, const char *title, const char *app_id)
{
	if (!name || !name[0])
		return NULL;

	struct wisp_app *app = app_find(name);
	if (app)
		return app;

	for (int i = 0; i < MAX_APPS; i++) {
		if (!g_apps[i].used) {
			app = &g_apps[i];
			memset(app, 0, sizeof(*app));
			snprintf(app->name, sizeof(app->name), "%s", name);
			if (title && title[0])
				snprintf(app->title, sizeof(app->title), "%s", title);
			else
				snprintf(app->title, sizeof(app->title), "%s", name);
			if (app_id && app_id[0])
				snprintf(app->app_id, sizeof(app->app_id), "%s", app_id);
			else
				snprintf(app->app_id, sizeof(app->app_id), "org.wisp.%s", name);

			app->used = true;
			app->state = APP_STATE_RUNNING;
			app->lua_init_ref = LUA_NOREF;
			app->lua_close_ref = LUA_NOREF;
			app->lua_build_ref = LUA_NOREF;
			app->lua_env_ref = LUA_NOREF;
			for (int j = 0; j < MAX_APP_WINDOWS; j++)
				app->windows[j] = -1;
			g_app_count++;
			return app;
		}
	}
	fprintf(stderr, "wisp: application registry full (%d max)\n", MAX_APPS);
	return NULL;
}

bool app_add_window(struct wisp_app *app, int widget_slot)
{
	if (!app || widget_slot < 0 || widget_slot >= MAX_WIDGETS)
		return false;
	if (app->window_count >= MAX_APP_WINDOWS)
		return false;

	for (int i = 0; i < app->window_count; i++) {
		if (app->windows[i] == widget_slot)
			return true;
	}

	app->windows[app->window_count++] = widget_slot;
	widgets[widget_slot].app_id_slot = (int)(app - g_apps);
	return true;
}

void app_remove_window(struct wisp_app *app, int widget_slot)
{
	if (!app || widget_slot < 0)
		return;
	for (int i = 0; i < app->window_count; i++) {
		if (app->windows[i] == widget_slot) {
			for (int j = i; j < app->window_count - 1; j++)
				app->windows[j] = app->windows[j + 1];
			app->windows[--app->window_count] = -1;
			break;
		}
	}
	if (app->window_count == 0 && app->state == APP_STATE_RUNNING) {
		app_close(app->name);
	}
}

bool app_open(const char *name, char *err_buf, size_t err_len)
{
	struct wisp_app *app = app_find(name);
	if (!app) {
		if (err_buf) snprintf(err_buf, err_len, "application '%s' not found", name);
		return false;
	}

	app->state = APP_STATE_RUNNING;
	if (app->window_count > 0) {
		for (int i = 0; i < app->window_count; i++) {
			int slot = app->windows[i];
			if (slot >= 0 && slot < MAX_WIDGETS && widgets[slot].used) {
				widget_show(&widgets[slot]);
			}
		}
		return true;
	}

	if (app->script_path[0] != '\0') {
		struct widget *w = widget_load_dynamic(app->script_path);
		if (w)
			return true;
		if (err_buf) snprintf(err_buf, err_len, "failed to reload application script '%s'", app->script_path);
		return false;
	}

	return true;
}

bool app_toggle(const char *name, char *err_buf, size_t err_len)
{
	struct wisp_app *app = app_find(name);
	if (!app) {
		if (err_buf) snprintf(err_buf, err_len, "application '%s' not found", name);
		return false;
	}

	if (app->state == APP_STATE_RUNNING && app->window_count > 0) {
		bool any_visible = false;
		for (int i = 0; i < app->window_count; i++) {
			int slot = app->windows[i];
			if (slot >= 0 && slot < MAX_WIDGETS && widgets[slot].used && widgets[slot].visible) {
				any_visible = true;
				break;
			}
		}
		if (any_visible) {
			return app_close(name);
		} else {
			return app_open(name, err_buf, err_len);
		}
	} else {
		return app_open(name, err_buf, err_len);
	}
}

bool app_close(const char *name)
{
	struct wisp_app *app = app_find(name);
	if (!app)
		return false;

	if (app->state == APP_STATE_CLOSED && app->window_count == 0)
		return true;

	app->state = APP_STATE_CLOSED;

	int app_slot = (int)(app - g_apps);
	clipboard_cleanup_app(app_slot);
	keyboard_cleanup_app(app_slot);
	screen_cleanup_app(app_slot);
	process_cleanup_app(app_slot);
	fs_cleanup_app(app_slot);
	audio_cleanup_app(app_slot);
	notify_cleanup_app(app_slot);

	if (Lm) {
		lua_getglobal(Lm, "wisp");
		if (lua_istable(Lm, -1)) {
			lua_getfield(Lm, -1, "_app_cleanup");
			if (lua_isfunction(Lm, -1)) {
				lua_pushstring(Lm, name);
				lua_pcall(Lm, 1, 0, 0);
			} else {
				lua_pop(Lm, 1);
			}
		}
		lua_pop(Lm, 1);

		if (app->lua_init_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_init_ref);
			app->lua_init_ref = LUA_NOREF;
		}
		if (app->lua_close_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_close_ref);
			app->lua_close_ref = LUA_NOREF;
		}
		if (app->lua_build_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_build_ref);
			app->lua_build_ref = LUA_NOREF;
		}
		if (app->lua_env_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_env_ref);
			app->lua_env_ref = LUA_NOREF;
		}
	}

	for (int i = 0; i < app->window_count; i++) {
		int slot = app->windows[i];
		app->windows[i] = -1;
		if (slot >= 0 && slot < MAX_WIDGETS && widgets[slot].used) {
			widget_destroy(&widgets[slot]);
		}
	}
	app->window_count = 0;
	return true;
}

bool app_reload(const char *name)
{
	struct wisp_app *app = app_find(name);
	if (!app)
		return false;

	int app_slot = (int)(app - g_apps);
	clipboard_cleanup_app(app_slot);
	keyboard_cleanup_app(app_slot);
	screen_cleanup_app(app_slot);
	process_cleanup_app(app_slot);
	fs_cleanup_app(app_slot);
	audio_cleanup_app(app_slot);
	notify_cleanup_app(app_slot);

	if (Lm) {
		lua_getglobal(Lm, "wisp");
		if (lua_istable(Lm, -1)) {
			lua_getfield(Lm, -1, "_app_cleanup");
			if (lua_isfunction(Lm, -1)) {
				lua_pushstring(Lm, name);
				lua_pcall(Lm, 1, 0, 0);
			} else {
				lua_pop(Lm, 1);
			}
		}
		lua_pop(Lm, 1);
	}

	for (int i = 0; i < app->window_count; i++) {
		int slot = app->windows[i];
		if (slot >= 0 && slot < MAX_WIDGETS && widgets[slot].used) {
			widgets[slot].want_redraw = 1;
		}
	}
	return true;
}

void app_teardown(struct wisp_app *app)
{
	if (!app || !app->used)
		return;
	app_close(app->name);
	if (Lm) {
		if (app->lua_init_ref != LUA_NOREF) luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_init_ref);
		if (app->lua_close_ref != LUA_NOREF) luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_close_ref);
		if (app->lua_build_ref != LUA_NOREF) luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_build_ref);
		if (app->lua_env_ref != LUA_NOREF) luaL_unref(Lm, LUA_REGISTRYINDEX, app->lua_env_ref);
	}
	app->used = false;
	app->state = APP_STATE_CLOSED;
	if (g_app_count > 0)
		g_app_count--;
}

void app_shutdown_all(void)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (g_apps[i].used)
			app_teardown(&g_apps[i]);
	}
}

void app_list_print(FILE *out)
{
	if (!out) return;
	fprintf(out, "%-16s %-16s %-10s %-8s %-20s %s\n", "APPLICATION", "TYPE", "STATE", "WINDOWS", "APP_ID", "TITLE");
	for (int i = 0; i < MAX_APPS; i++) {
		if (g_apps[i].used) {
			const char *type = (g_apps[i].state == APP_STATE_RUNNING && g_apps[i].window_count > 0)
					   ? "active-instance" : "registered-app";
			fprintf(out, "%-16s %-16s %-10s %-8d %-20s %s\n",
				g_apps[i].name,
				type,
				g_apps[i].state == APP_STATE_RUNNING ? "running" : "closed",
				g_apps[i].window_count,
				g_apps[i].app_id,
				g_apps[i].title);
		}
	}
}
