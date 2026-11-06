#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/types.h"

#define LUA_MEM_DEFAULT (16u << 20)
#define LUA_TICKS_DEFAULT 10000000L
#define LUA_CHUNK_MAX (256 * 1024)

struct config_staging;
struct staged_widget_spec;

#if WISP_HAS_LUA

/**
 * Lua VM globals.
 *
 * Architecture: single master state Lm; each widget gets a coroutine thread
 * (lua_newthread) stored in Lm's registry as lua_thread_ref.  Isolation is
 * achieved via environment table substitution — each widget's _G is a fresh
 * table that copies references to standard libs but isolates global writes.
 *
 * lua_cur  — widget whose Lua callback is currently executing (NULL outside calls).
 *            widget_destroy defers teardown when lua_cur == wgt to prevent use-after-free.
 * lua_ctx  — widget being configured or owning the current timer registration.
 *            Persists between calls; used by wisp.timer.* and wisp.widget.* API.
 */
extern lua_State *Lm;
extern size_t lua_used, lua_limit;
extern long lua_ticks_total;
extern struct widget *lua_cur;
extern struct widget *lua_ctx;
extern struct rect *lua_dmg;

void shutdown_lua(void);
void timer_cancel(int id);
int lua_timers_ms(int64_t now);
void lua_run_due(int64_t now);
void lua_widget_draw(struct widget *wgt);
void lua_widget_free(struct widget *wgt);
bool lua_widget_load(struct widget *wgt, const char *path);
bool lua_widget_reload(struct widget *wgt);
bool lua_widget_from_source(struct widget *wgt, const char *src, size_t len, const char *chunkid);
/**
 * Hot-reload from source with rollback.
 * Creates a new thread and runs lua_env_setup before touching the live widget.
 * Only if that succeeds does it free the old state and swap in the new one.
 * On failure the widget continues running its previous version unchanged.
 * State table is preserved across reloads (copied from old env to new env).
 */
bool lua_widget_reload_from_source(struct widget *wgt, const char *src, size_t len, const char *chunkid);
/**
 * Call a Lua callback ref on wgt's thread.  On error the callback ref is
 * unreffed (set to LUA_NOREF) and false is returned — the script won't fire again.
 * Instruction budget (lua_ticks_total) is reset before each call.
 */
bool lua_script_call(struct widget *wgt, int *ref, const char *what, const char *arg);
/**
 * Execute a widget's named event slot (e.g. "draw", "timer").
 * Dispatches via lua_script_call on wgt's own thread.
 */
void lua_event(struct widget *wgt, const char *ev);
/**
 * Execute a named event slot on target using wgt's thread context.
 * Used for pointer events where the top-level widget owns the thread but
 * the event fires on a child widget.
 */
void lua_dispatch(struct widget *wgt, struct widget *target, const char *ev);
void lua_dispatch_click(struct widget *wgt, struct widget *target, int32_t x, int32_t y);
void lua_dispatch_scroll(struct widget *wgt, struct widget *target, int dir, int32_t x, int32_t y);
void lua_vm_init(size_t mem_bytes);
/**
 * Execute a config Lua file into a staging buffer without affecting live widgets.
 * Sets cur_stage so that wisp.setup() and wisp.widget.register() write into stage
 * instead of applying changes directly.  Returns false on parse or runtime error
 * (stage is partially populated but callers should discard it via lua_config_staging_free).
 */
bool lua_config_execute(const char *path, struct config_staging *stage);
bool lua_config_execute_source(const char *src, size_t len, const char *chunkid, struct config_staging *stage);
/** Release Lua registry refs held by staged build callbacks. Must be called even on failed execute. */
void lua_config_staging_free(struct config_staging *stage);
bool lua_widget_apply_spec(struct widget *wgt, const struct staged_widget_spec *spec);

#else
extern struct widget *lua_cur;
static inline void shutdown_lua(void) {}
static inline void lua_run_due(int64_t now) { (void)now; }
static inline int lua_timers_ms(int64_t now) { (void)now; return -1; }
static inline void lua_widget_free(struct widget *w) { (void)w; }
static inline void lua_widget_draw(struct widget *w) { (void)w; }
static inline bool lua_widget_reload(struct widget *w) { (void)w; return false; }
static inline bool lua_widget_load(struct widget *w, const char *path) {
	(void)w; (void)path;
	fprintf(stderr, "wisp: Lua scripting disabled at build time\n");
	return false;
}
static inline void lua_event(struct widget *wgt, const char *ev) { (void)wgt; (void)ev; }
static inline void lua_dispatch(struct widget *wgt, struct widget *target, const char *ev) { (void)wgt; (void)target; (void)ev; }
static inline void lua_dispatch_click(struct widget *wgt, struct widget *target, int32_t x, int32_t y) { (void)wgt; (void)target; (void)x; (void)y; }
static inline void lua_dispatch_scroll(struct widget *wgt, struct widget *target, int dir, int32_t x, int32_t y) { (void)wgt; (void)target; (void)dir; (void)x; (void)y; }
static inline void lua_vm_init(size_t mem_bytes) { (void)mem_bytes; }
static inline bool lua_config_execute(const char *path, struct config_staging *stage) {
	(void)path; (void)stage; return false;
}
static inline bool lua_config_execute_source(const char *src, size_t len, const char *chunkid, struct config_staging *stage) {
	(void)src; (void)len; (void)chunkid; (void)stage; return false;
}
static inline void lua_config_staging_free(struct config_staging *stage) { (void)stage; }
static inline bool lua_widget_apply_spec(struct widget *wgt, const struct staged_widget_spec *spec) {
	(void)wgt; (void)spec; return false;
}

#endif
