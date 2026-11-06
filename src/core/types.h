#pragma once
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "zwlr_layer_shell_v1.h"

#ifndef WISP_HAS_LUA
#define WISP_HAS_LUA 1
#endif

#if WISP_HAS_LUA
#include <lua/lua.h>
#include <lua/lauxlib.h>
#include <lua/lualib.h>
#else
struct lua_State;
#define LUA_NOREF (-2)
#endif

#define COLOR_IDLE   0xDD303030u
#define COLOR_ACTIVE 0xDD3050A0u
#define MAX_WIDGETS  128
#define MAX_KIDS     64

enum widget_kind {
	WIDGET_NONE = 0,
	WIDGET_BOX,
	WIDGET_ROW,
	WIDGET_COLUMN,
	WIDGET_TEXT,
	WIDGET_IMAGE,
	WIDGET_BUTTON,
	WIDGET_SPACER,
	WIDGET_PROGRESS,
	WIDGET_SEPARATOR,
};

/*
 * Buffer state machine:
 *   AVAILABLE → scene_commit()   → ATTACHED
 *   ATTACHED  → buffer_release() → AVAILABLE  (compositor fires when it's done reading)
 *   ATTACHED  → service_widget() → BUSY        (redraw needed but compositor still holds buffer)
 *   BUSY      → buffer_release() → AVAILABLE  (escape: buffer_release fires regardless of state)
 *
 * While ATTACHED or BUSY, wgt->pixels must not be written — the compositor may be reading
 * the same mmap-backed shared memory.
 */
enum buffer_state {
	BUF_AVAILABLE,
	BUF_ATTACHED,
	BUF_BUSY,
};

struct rect {
	int x, y, w, h;
	bool valid;
};

struct image;
struct img_scaled;

struct widget {
	char name[32];
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wl_buffer *buffer;
	/* Pixels is mmap-backed shared memory (memfd). The compositor reads it after scene_commit
	 * until buffer_release fires; do not write while state != BUF_AVAILABLE. */
	uint32_t *pixels;
	size_t size;
	/* width/height are the physical buffer dimensions (logical × scale).
	 * pend_w/pend_h are the pending logical dimensions (in points, before scale is applied).
	 * alloc_buffer is called with pend_w*scale × pend_h*scale when want_resize fires. */
	uint32_t width, height;
	enum buffer_state state;
	bool used;
	bool visible;
	bool want_destroy;
	struct image *img;
	struct img_scaled *scaled;       /* borrowed ref; released in widget_teardown */
	int scale;
	int applied_scale;               /* scale applied to the wl_surface at last alloc */
	uint32_t paint;
	/* last_dmg/last_paint/last_scaled track the previous frame for frame_same() deduplication */
	struct rect last_dmg;
	uint32_t last_paint;
	struct img_scaled *last_scaled;
	bool drew_once, need_attach;
	uint32_t draws;
	struct lua_State *lua_L;         /* per-widget coroutine thread (NULL if no script) */
	int lua_thread_ref;              /* LUA_REGISTRYINDEX ref keeping the thread alive in Lm */
	/* All lua_* int fields are Lua registry refs (LUA_NOREF = -2 means unset). */
	int lua_env, lua_build, lua_draw, lua_timer, lua_event;
	int lua_click, lua_hover, lua_leave, lua_press, lua_release, lua_scroll;
	int32_t lua_timer_ms;
	int64_t lua_next;                /* absolute CLOCK_MONOTONIC timestamp (µs) for next legacy timer fire */
	char script_path[128];
	bool from_config;
	const char *text;                /* heap-allocated (strdup); freed in widget_teardown */
	int watch_wd;                    /* inotify watch descriptor (-1 if not watching) */
	uint32_t anchor;
	int32_t margin[4];               /* top, right, bottom, left */
	int kind;
	int parent;                      /* index into widgets[], or -1 for top-level */
	bool kid;
	int kid_slots[MAX_KIDS];
	int n_kids;
	int pad[4];                      /* top, right, bottom, left padding for layout containers */
	int gap;
	/* align: 0=start, 1=center, 2=end; controls cross-axis placement in widget_calc_pos */
	int align;
	uint32_t background;
	int radius;
	int text_size;
	uint32_t text_color;
	int text_align;
	float progress;
	int img_w, img_h;
	int x, y;                        /* position relative to parent for child widgets */
	/* volatile sig_atomic_t for signal-handler visibility (not for thread safety) */
	volatile sig_atomic_t want_redraw, want_resize;
	volatile sig_atomic_t pend_w, pend_h; /* logical dimensions, written by signal handlers */
};

extern struct widget widgets[MAX_WIDGETS];
extern volatile sig_atomic_t running;
extern volatile sig_atomic_t redraw_request;
extern volatile sig_atomic_t realloc_request;
extern bool failed;
extern int inotify_fd;

void fail(const char *what);
