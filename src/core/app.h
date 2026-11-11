#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "core/types.h"

#define MAX_APPS 32
#define MAX_APP_WINDOWS 8

enum app_lifecycle_state {
	APP_STATE_CLOSED = 0,
	APP_STATE_RUNNING = 1,
};

struct wisp_app {
	char name[64];
	char title[128];
	char app_id[64];
	char script_path[256];
	enum app_lifecycle_state state;
	int windows[MAX_APP_WINDOWS];
	int window_count;
	int lua_init_ref;
	int lua_close_ref;
	int lua_build_ref;
	int lua_env_ref;
	bool used;
};

extern struct wisp_app g_apps[MAX_APPS];
extern int g_app_count;

void app_system_init(void);
struct wisp_app *app_find(const char *name);
struct wisp_app *app_create(const char *name, const char *title, const char *app_id);
bool app_open(const char *name, char *err_buf, size_t err_len);
bool app_close(const char *name);
bool app_reload(const char *name);
bool app_add_window(struct wisp_app *app, int widget_slot);
void app_remove_window(struct wisp_app *app, int widget_slot);
void app_teardown(struct wisp_app *app);
void app_shutdown_all(void);
void app_list_print(FILE *out);
