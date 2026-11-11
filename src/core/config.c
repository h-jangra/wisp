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

struct widget_registry g_registry = {
	.count = 0,
};

struct staged_widget_spec *registry_find(const char *name)
{
	if (!name || !name[0])
		return NULL;
	for (int i = 0; i < g_registry.count; i++) {
		if (strcmp(g_registry.entries[i].name, name) == 0)
			return &g_registry.entries[i];
	}
	return NULL;
}

bool registry_add_or_update(const struct staged_widget_spec *spec)
{
	if (!spec || !spec->name[0])
		return false;
	for (int i = 0; i < g_registry.count; i++) {
		if (strcmp(g_registry.entries[i].name, spec->name) == 0) {
			int slot = g_registry.entries[i].instance_slot;
			enum widget_lifecycle_state st = g_registry.entries[i].state;
			g_registry.entries[i] = *spec;
			g_registry.entries[i].instance_slot = slot;
			g_registry.entries[i].state = st;
			return true;
		}
	}
	if (g_registry.count >= MAX_WIDGETS)
		return false;
	g_registry.entries[g_registry.count] = *spec;
	g_registry.entries[g_registry.count].instance_slot = -1;
	g_registry.entries[g_registry.count].state = WIDGET_STATE_UNLOADED;
	g_registry.entries[g_registry.count].last_error[0] = '\0';
	g_registry.count++;
	return true;
}

static bool dep_has_cycle(struct staged_widget_spec *entries, int count, int idx, bool *visited, bool *stack)
{
	visited[idx] = true;
	stack[idx] = true;
	struct staged_widget_spec *sp = &entries[idx];
	for (int d = 0; d < sp->dep_count; d++) {
		for (int j = 0; j < count; j++) {
			if (strcmp(entries[j].name, sp->deps[d]) == 0) {
				if (!visited[j] && dep_has_cycle(entries, count, j, visited, stack))
					return true;
				else if (stack[j])
					return true;
				break;
			}
		}
	}
	stack[idx] = false;
	return false;
}

bool registry_validate_dependencies(struct staged_widget_spec *entries, int count, char *err, size_t err_len)
{
	for (int i = 0; i < count; i++) {
		struct staged_widget_spec *sp = &entries[i];
		for (int d = 0; d < sp->dep_count; d++) {
			bool found = false;
			for (int j = 0; j < count; j++) {
				if (strcmp(entries[j].name, sp->deps[d]) == 0) {
					found = true;
					if (!entries[j].enabled) {
						if (err) snprintf(err, err_len, "widget '%s' depends on disabled widget '%s'",
								  sp->name, sp->deps[d]);
						return false;
					}
					break;
				}
			}
			if (!found) {
				if (err) snprintf(err, err_len, "widget '%s' depends on unregistered widget '%s'",
						  sp->name, sp->deps[d]);
				return false;
			}
		}
	}

	bool visited[MAX_WIDGETS] = {0};
	bool stack[MAX_WIDGETS] = {0};
	for (int i = 0; i < count; i++) {
		if (!visited[i] && dep_has_cycle(entries, count, i, visited, stack)) {
			if (err) snprintf(err, err_len, "circular dependency detected involving '%s'", entries[i].name);
			return false;
		}
	}
	return true;
}

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

	if (access("wisp.lua", R_OK) == 0) {
		snprintf(out_path, out_len, "wisp.lua");
		return true;
	}
	if (access("example/wisp.lua", R_OK) == 0) {
		snprintf(out_path, out_len, "example/wisp.lua");
		return true;
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
void config_resolve_paths(struct config_staging *stage)
{
	const char *base = g_config.watch_dir[0] ? g_config.watch_dir : ".";
	for (int i = 0; i < stage->widget_count; i++) {
		struct staged_widget_spec *sp = &stage->widgets[i];
		if (!sp->path[0] || sp->path[0] == '/')
			continue;
		char resolved[sizeof(sp->path)];
		int n = snprintf(resolved, sizeof(resolved), "%s/%s", base, sp->path);
		if (n > 0 && (size_t)n < sizeof(sp->path))
			memcpy(sp->path, resolved, (size_t)n + 1);
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
	char val_err[256];
	if (!registry_validate_dependencies(stage.widgets, stage.widget_count, val_err, sizeof(val_err)))
		fprintf(stderr, "wisp: dependency warning: %s\n", val_err);

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
	if (stage.lua_mem_limit > 0)
		lua_limit = stage.lua_mem_limit;
	if (stage.lua_insn_limit > 0)
		lua_ticks_total = stage.lua_insn_limit;

	g_registry.count = stage.widget_count;
	for (int i = 0; i < stage.widget_count; i++) {
		g_registry.entries[i] = stage.widgets[i];
		g_registry.entries[i].instance_slot = -1;
		g_registry.entries[i].state = WIDGET_STATE_UNLOADED;
		g_registry.entries[i].last_error[0] = '\0';
	}

	int autostart_indices[MAX_WIDGETS];
	int n_autostart = 0;
	for (int i = 0; i < g_registry.count; i++) {
		if (g_registry.entries[i].autostart && g_registry.entries[i].enabled)
			autostart_indices[n_autostart++] = i;
	}
	for (int i = 1; i < n_autostart; i++) {
		int key = autostart_indices[i];
		int j = i - 1;
		while (j >= 0 && g_registry.entries[autostart_indices[j]].order > g_registry.entries[key].order) {
			autostart_indices[j + 1] = autostart_indices[j];
			j--;
		}
		autostart_indices[j + 1] = key;
	}
	for (int i = 0; i < n_autostart; i++) {
		struct staged_widget_spec *sp = &g_registry.entries[autostart_indices[i]];
		char err[256];
		if (!widget_open_named(sp->name, err, sizeof(err)))
			fprintf(stderr, "wisp: autostart widget '%s' failed: %s\n", sp->name, err);
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
	char val_err[256];
	if (!registry_validate_dependencies(stage.widgets, stage.widget_count, val_err, sizeof(val_err)))
		fprintf(stderr, "wisp: dependency warning: %s\n", val_err);

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
	if (stage.lua_mem_limit > 0)
		lua_limit = stage.lua_mem_limit;
	if (stage.lua_insn_limit > 0)
		lua_ticks_total = stage.lua_insn_limit;

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

	struct widget_registry old_reg = g_registry;
	g_registry.count = stage.widget_count;
	for (int i = 0; i < stage.widget_count; i++) {
		g_registry.entries[i] = stage.widgets[i];
		g_registry.entries[i].instance_slot = -1;
		g_registry.entries[i].state = WIDGET_STATE_UNLOADED;
		g_registry.entries[i].last_error[0] = '\0';

		for (int j = 0; j < old_reg.count; j++) {
			if (strcmp(old_reg.entries[j].name, stage.widgets[i].name) == 0) {
				g_registry.entries[i].instance_slot = old_reg.entries[j].instance_slot;
				g_registry.entries[i].state = old_reg.entries[j].state;
				break;
			}
		}

		int slot = g_registry.entries[i].instance_slot;
		if (slot >= 0 && slot < MAX_WIDGETS && widgets[slot].used) {
			struct widget *w = &widgets[slot];
			if (g_registry.entries[i].path[0] && strcmp(w->script_path, g_registry.entries[i].path) != 0)
				lua_widget_load(w, g_registry.entries[i].path);
			lua_widget_apply_spec(w, &g_registry.entries[i]);
		} else if (g_registry.entries[i].autostart && g_registry.entries[i].enabled) {
			char err[256];
			widget_open_named(g_registry.entries[i].name, err, sizeof(err));
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
