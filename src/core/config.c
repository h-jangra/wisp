#define _GNU_SOURCE
#include "core/config.h"
#include "core/widget.h"
#include "core/types.h"
#include "layout/layout.h"
#include "scripting/scripting.h"
#include "services/system.h"
#include "wayland/wayland.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/inotify.h>

struct config_state g_config = {
	.watch_wd = -1,
	.scale = 1,
	.loaded = false,
};

bool config_find_path(const char *cli_path, char *out_path, size_t out_len)
{
	if (!out_path || out_len == 0)
		return false;

	if (cli_path && cli_path[0]) {
		if (access(cli_path, R_OK) == 0) {
			snprintf(out_path, out_len, "%s", cli_path);
			return true;
		}
		fprintf(stderr, "wisp: config '%s' not accessible: %s\n", cli_path, strerror(errno));
		return false;
	}

	const char *env_cfg = getenv("WISP_CONFIG");
	if (env_cfg && env_cfg[0] && access(env_cfg, R_OK) == 0) {
		snprintf(out_path, out_len, "%s", env_cfg);
		return true;
	}

	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0]) {
		snprintf(out_path, out_len, "%s/wisp/wisp.lua", xdg);
		if (access(out_path, R_OK) == 0)
			return true;
	} else {
		const char *home = getenv("HOME");
		if (home && home[0]) {
			snprintf(out_path, out_len, "%s/.config/wisp/wisp.lua", home);
			if (access(out_path, R_OK) == 0)
				return true;
		}
	}

#ifdef WISP_DATA_DIR
	snprintf(out_path, out_len, "%s/wisp.lua", WISP_DATA_DIR);
	if (access(out_path, R_OK) == 0)
		return true;
#endif

	if (access("/usr/share/wisp/wisp.lua", R_OK) == 0) {
		snprintf(out_path, out_len, "/usr/share/wisp/wisp.lua");
		return true;
	}

	if (access("/etc/wisp/wisp.lua", R_OK) == 0) {
		snprintf(out_path, out_len, "/etc/wisp/wisp.lua");
		return true;
	}

	return false;
}

/* Resolve any relative paths in widget specs against the config file's directory. */
static void config_resolve_paths(struct config_staging *stage)
{
	if (!g_config.watch_dir[0])
		return;
	for (int i = 0; i < stage->widget_count; i++) {
		struct staged_widget_spec *sp = &stage->widgets[i];
		if (!sp->path[0] || sp->path[0] == '/')
			continue;
		char resolved[sizeof(g_config.watch_dir) + sizeof(sp->path)];
		snprintf(resolved, sizeof(resolved), "%s/%s", g_config.watch_dir, sp->path);
		snprintf(sp->path, sizeof(sp->path), "%s", resolved);
	}
}

bool config_init(const char *cli_path, int *forced_scale)
{
	config_shutdown();

	if (!config_find_path(cli_path, g_config.active_path, sizeof(g_config.active_path)))
		return false;

	const char *fname = NULL;
	get_dir_and_file(g_config.active_path, g_config.watch_dir, sizeof(g_config.watch_dir), &fname);
	if (fname)
		snprintf(g_config.watch_file, sizeof(g_config.watch_file), "%s", fname);

	struct config_staging stage;
	memset(&stage, 0, sizeof(stage));
	stage.scale = (forced_scale && *forced_scale >= 1) ? *forced_scale : 1;

	if (!lua_config_execute(g_config.active_path, &stage)) {
		lua_config_staging_free(&stage);
		return false;
	}

	config_resolve_paths(&stage);

	if (stage.scale >= 1 && stage.scale <= 4) {
		g_config.scale = stage.scale;
		if (forced_scale)
			*forced_scale = stage.scale;
	}
	if (stage.font[0])
		setenv("WISP_FONT", stage.font, 1);
	if (stage.font_size > 0) {
		char b[16];
		snprintf(b, sizeof(b), "%d", stage.font_size);
		setenv("WISP_SIZE", b, 1);
	}

	for (int i = 0; i < stage.widget_count; i++) {
		struct staged_widget_spec *sp = &stage.widgets[i];
		struct widget *w = widget_add(sp->has_visible ? sp->visible : true,
					      sp->width > 0 ? sp->width : 100,
					      sp->height > 0 ? sp->height : 50, NULL);
		if (w) {
			if (forced_scale && *forced_scale >= 1)
				w->scale = *forced_scale;
			lua_widget_apply_spec(w, sp);
		}
	}

	lua_config_staging_free(&stage);
	g_config.loaded = true;
	return true;
}

bool config_reload(int *forced_scale)
{
	if (!g_config.active_path[0])
		return false;

	struct config_staging stage;
	memset(&stage, 0, sizeof(stage));
	stage.scale = (forced_scale && *forced_scale >= 1) ? *forced_scale : 1;

	if (!lua_config_execute(g_config.active_path, &stage)) {
		fprintf(stderr, "wisp: config reload failed, rolling back to previous configuration\n");
		lua_config_staging_free(&stage);
		return false;
	}

	config_resolve_paths(&stage);

	if (stage.scale >= 1 && stage.scale <= 4) {
		g_config.scale = stage.scale;
		if (forced_scale)
			*forced_scale = stage.scale;
		for (int k = 0; k < MAX_WIDGETS; k++)
			if (widgets[k].used)
				surface_preferred_scale(&widgets[k], widgets[k].surface, stage.scale);
	}
	if (stage.font[0])
		setenv("WISP_FONT", stage.font, 1);
	if (stage.font_size > 0) {
		char b[16];
		snprintf(b, sizeof(b), "%d", stage.font_size);
		setenv("WISP_SIZE", b, 1);
	}

	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *w = &widgets[k];
		if (!w->used || !w->from_config)
			continue;
		bool retained = false;
		for (int i = 0; i < stage.widget_count; i++) {
			if (strcmp(w->name, stage.widgets[i].name) == 0) {
				retained = true;
				break;
			}
		}
		if (!retained)
			widget_destroy(w);
	}

	for (int i = 0; i < stage.widget_count; i++) {
		struct staged_widget_spec *sp = &stage.widgets[i];
		struct widget *w = NULL;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && widgets[k].from_config &&
			    strcmp(widgets[k].name, sp->name) == 0) {
				w = &widgets[k];
				break;
			}
		}
		if (w) {
			if (sp->path[0] && strcmp(w->script_path, sp->path) != 0)
				lua_widget_load(w, sp->path);
			lua_widget_apply_spec(w, sp);
		} else {
			struct widget *nw = widget_add(sp->has_visible ? sp->visible : true,
						       sp->width > 0 ? sp->width : 100,
						       sp->height > 0 ? sp->height : 50, NULL);
			if (nw) {
				if (forced_scale && *forced_scale >= 1)
					nw->scale = *forced_scale;
				lua_widget_apply_spec(nw, sp);
			}
		}
	}

	lua_config_staging_free(&stage);
	redraw_request = 1;
	fprintf(stderr, "wisp: config '%s' reloaded successfully\n", g_config.active_path);
	return true;
}

void config_shutdown(void)
{
	g_config.active_path[0] = '\0';
	g_config.watch_dir[0] = '\0';
	g_config.watch_file[0] = '\0';
	g_config.watch_wd = -1;
	g_config.loaded = false;
}

void config_setup_watch(int inotify_fd)
{
	if (inotify_fd >= 0 && g_config.watch_dir[0]) {
		g_config.watch_wd = inotify_add_watch(inotify_fd, g_config.watch_dir,
						      IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	}
}

bool config_handle_inotify(int inotify_fd, int wd, const char *name, uint32_t mask, int *forced_scale)
{
	(void)inotify_fd;
	if (wd == g_config.watch_wd && g_config.watch_file[0] &&
	    strcmp(name, g_config.watch_file) == 0) {
		if (mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
			config_reload(forced_scale);
			return true;
		}
	}
	return false;
}
