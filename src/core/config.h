#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "core/types.h"

/**
 * Intermediate capture of one widget declaration from a config file.
 * has_* flags distinguish an explicit field from an absent one (absence
 * means "inherit from defaults or keep current").
 */
struct staged_widget_spec {
	char name[64];
	char path[256];
	uint32_t width, height;
	uint32_t anchor;
	int32_t margin[4];
	bool visible;
	bool has_margin;
	bool has_size;
	bool has_anchor;
	bool has_visible;
	int scale;
	int build_ref;
	size_t lua_mem_limit;
};

/**
 * Staging buffer populated by lua_config_execute() before any live state is
 * mutated.  If the config script errors, this is simply discarded, leaving
 * the running widgets untouched.
 */
struct config_staging {
	int scale;
	char font[256];
	int font_size;
	uint32_t default_anchor;
	int32_t default_margin[4];
	bool has_default_anchor;
	bool has_default_margin;
	size_t lua_mem_limit;
	long lua_insn_limit;
	struct staged_widget_spec widgets[MAX_WIDGETS];
	int widget_count;
};

struct config_state {
	char active_path[512];
	char watch_dir[512];
	char watch_file[128];
	int watch_wd;
	int scale;
	bool loaded;
};

extern struct config_state g_config;

/**
 * Locate the config file using the following search order:
 *   1. cli_path (if provided)
 *   2. WISP_CONFIG environment variable
 *   3. $XDG_CONFIG_HOME/wisp/wisp.lua  (or ~/.config/wisp/wisp.lua)
 *   4. WISP_DATA_DIR/wisp.lua           (installed system default, baked in at build time)
 *   5. /usr/share/wisp/wisp.lua         (fallback for non-standard installs)
 *   6. /etc/wisp/wisp.lua               (site-wide override)
 */
bool config_find_path(const char *cli_path, char *out_path, size_t out_len);
bool config_init(const char *cli_path, int *forced_scale);
/**
 * config_reload() diff algorithm:
 *   1. config-owned widgets absent from the new spec are destroyed.
 *   2. widgets present in both are updated (script reloaded if path changed).
 *   3. new widget specs are created from scratch.
 * Matching is by name, so renaming a widget in the config is treated as
 * destroy-old + create-new.
 */
bool config_reload(int *forced_scale);
void config_shutdown(void);
void config_setup_watch(int inotify_fd);
bool config_handle_inotify(int inotify_fd, int wd, const char *name, uint32_t mask, int *forced_scale);
