#pragma once
#include "core/types.h"

/**
 * Allocate widget slot, create wl_surface and zwlr_layer_surface_v1.
 * Input region is initialised to empty (0,0,0,0) — call widget_input_region()
 * after adding interactive children to expose them to pointer events.
 * Returns NULL if the compositor is not ready or the registry is full.
 */
struct widget *widget_add(bool visible, uint32_t width,
			 uint32_t height, const char *img_path);
struct widget *widget_add_toplevel(bool visible, uint32_t width,
				   uint32_t height, const char *title,
				   const char *app_id);
void widget_start_move(struct widget *wgt);
void widget_start_resize(struct widget *wgt, uint32_t edges);
void widget_set_fullscreen(struct widget *wgt, bool fullscreen);
void widget_set_maximized(struct widget *wgt, bool maximized);
void widget_set_minimized(struct widget *wgt);
void widget_set_title(struct widget *wgt, const char *title);
/** Allocate a child widget slot with no Wayland surfaces (composited by parent). */
struct widget *widget_add_child(int parent, int kind, uint32_t width,
			 uint32_t height, const char *img_path);
void widget_kill(struct widget *wgt);
void widget_detach(struct widget *wgt);
void widget_layout(struct widget *wgt);
/**
 * Rebuild the wl_surface input region from all interactive children.
 * Call after widget_layout() whenever children change position or kind.
 */
void widget_input_region(struct widget *wgt);
void widget_show(struct widget *wgt);
void widget_hide(struct widget *wgt);
/**
 * Deferred destroy: marks want_destroy and detaches from Wayland (null buffer commit).
 * If lua_cur == wgt (called from inside one of this widget's own Lua callbacks),
 * actual teardown is deferred to service_widget() to prevent use-after-free.
 */
void widget_destroy(struct widget *wgt);
/**
 * Immediate teardown: recursively frees children, removes inotify watch (only
 * if no other widget shares the same watch descriptor), frees Lua state, image
 * and scaled-image refs, destroys Wayland objects, and munmaps pixels.
 * Must not be called while lua_cur == wgt (use widget_destroy for safe deferred teardown).
 */
void widget_teardown(struct widget *wgt);
/**
 * Per-frame servicing: handles deferred teardown, buffer reallocation, and redraw.
 *
 * The double-if guard `if (want_resize || want_redraw) if (parent >= 0) return`
 * means: child widgets that need an update are skipped here — they are composited
 * by their parent's draw call, not serviced independently.
 */
void service_widget(struct widget *wgt);
extern struct widget *g_runtime_registered_widget;
struct widget *widget_load_dynamic(const char *path);
bool widget_unload_dynamic(const char *name_or_slot);
bool widget_open_named(const char *name, char *err_buf, size_t err_len);
bool widget_close_named(const char *name, char *err_buf, size_t err_len);
bool widget_toggle_named(const char *name, char *err_buf, size_t err_len);
bool widget_reload_named(const char *name, char *err_buf, size_t err_len);
void widget_list(FILE *out);
void widget_runtime_status(FILE *out);

bool kid_interactive(struct widget *kid);
void widget_pointer(struct widget *wgt, int type, int button, int32_t x, int32_t y);
void widget_scroll(struct widget *wgt, int32_t x, int32_t y, int dir, float dx, float dy);
void widget_key_event(struct widget *wgt, uint32_t keycode, uint32_t sym, uint32_t unicode, const char *utf8_str, uint32_t mods, bool is_press);
void widget_focus_event(struct widget *wgt, bool focused);
