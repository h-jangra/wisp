#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/inotify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/types.h"
#include "core/widget.h"
#include "core/config.h"
#include "core/app.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "render/renderer.h"
#include "services/system.h"
#include "services/clipboard.h"
#include "services/keyboard.h"
#include "scripting/scripting.h"
#include "wayland/wayland.h"

struct widget widgets[MAX_WIDGETS];
static struct widget *widget_hover = NULL;
static struct widget *captured_widget = NULL;

void widget_teardown(struct widget *wgt)
{
	if (!wgt || !wgt->used)
		return;
	int slot = (int)(wgt - widgets);
	int app_slot = wgt->app_id_slot;
	wgt->app_id_slot = -1;
	keyboard_cleanup_widget(slot);
	clipboard_cleanup_widget(slot);

	for (int k = 0; k < wgt->n_kids; k++) {
		int idx = wgt->kid_slots[k];
		if (idx >= 0 && idx < MAX_WIDGETS && &widgets[idx] != wgt &&
		    widgets[idx].used)
			widget_teardown(&widgets[idx]);
	}
	wgt->n_kids = 0;
	if (captured_widget == wgt)
		captured_widget = NULL;
	if (widget_hover == wgt)
		widget_hover = NULL;
	if (wl.pointer_surface == wgt->surface)
		wl.pointer_surface = NULL;
	if (wl.keyboard_surface == wgt->surface)
		wl.keyboard_surface = NULL;
	if (wl.touch_surface == wgt->surface)
		wl.touch_surface = NULL;
	if (inotify_fd >= 0 && wgt->watch_wd >= 0) {
		int old_wd = wgt->watch_wd;
		wgt->watch_wd = -1;
		bool other = false;
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (&widgets[k] != wgt && widgets[k].used && widgets[k].watch_wd == old_wd) {
				other = true;
				break;
			}
		}
		if (!other)
			inotify_rm_watch(inotify_fd, old_wd);
	}
	wgt->script_path[0] = '\0';
	free((void *)wgt->text);
	wgt->text = NULL;
	interact_tree_destroy(&wgt->interact, wgt->lua_L);
	lua_widget_free(wgt);
	scaled_release(wgt->scaled);
	wgt->scaled = NULL;
	wgt->last_scaled = NULL;
	image_release(wgt->img);
	wgt->img = NULL;
	if (wgt->use_egl && g_renderer && g_renderer->surface_destroy)
		g_renderer->surface_destroy(wgt);
	if (wgt->xdg_toplevel)
		xdg_toplevel_destroy(wgt->xdg_toplevel);
	if (wgt->xdg_surface)
		xdg_surface_destroy(wgt->xdg_surface);
	wgt->xdg_toplevel = NULL;
	wgt->xdg_surface = NULL;
	if (wgt->layer_surface)
		zwlr_layer_surface_v1_destroy(wgt->layer_surface);
	if (wgt->surface)
		wl_surface_destroy(wgt->surface);
	wgt->layer_surface = NULL;
	wgt->surface = NULL;
	if (wgt->buffer)
		wl_buffer_destroy(wgt->buffer);
	wgt->buffer = NULL;
	if (wgt->pixels)
		munmap(wgt->pixels, wgt->size);
	wgt->pixels = NULL;
	wgt->size = 0;
	wgt->used = false;
	wgt->visible = false;
	wgt->lua_mem_used = 0;
	int my_slot = (int)(wgt - widgets);
	for (int i = 0; i < g_registry.count; i++) {
		if (g_registry.entries[i].instance_slot == my_slot) {
			g_registry.entries[i].instance_slot = -1;
			g_registry.entries[i].state = WIDGET_STATE_UNLOADED;
			break;
		}
	}
	if (app_slot >= 0 && app_slot < MAX_APPS && g_apps[app_slot].used) {
		app_remove_window(&g_apps[app_slot], my_slot);
	}
	lua_widget_slot_reset(my_slot);
	fprintf(stderr, "wisp: widget '%s' removed\n", wgt->name);
}

void widget_show(struct widget *wgt)
{
	if (!wgt->used || wgt->visible || wgt->want_destroy)
		return;
	wgt->visible = true;
	wgt->need_attach = true;
	if (!wgt->buffer)
		wgt->want_resize = 1;
	wgt->want_redraw = 1;
	if (wgt->lua_L && wgt->lua_timer != LUA_NOREF)
		wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
}

void widget_hide(struct widget *wgt)
{
	if (!wgt->used || !wgt->visible)
		return;
	if (wgt->surface) {
		wl_surface_attach(wgt->surface, NULL, 0, 0);
		wl_surface_commit(wgt->surface);
	}
	if (wgt->pixels && wgt->size > 0)
		madvise(wgt->pixels, wgt->size, MADV_DONTNEED);
	wgt->visible = false;
	wgt->want_redraw = 0;
}

void widget_destroy(struct widget *wgt)
{
	if (!wgt || !wgt->used)
		return;
	wgt->visible = false;
	wgt->want_destroy = true;
	wgt->want_redraw = 0;
	wgt->lua_next = 0;
	if (wgt->surface) {
		wl_surface_attach(wgt->surface, NULL, 0, 0);
		wl_surface_commit(wgt->surface);
	}
	if (lua_cur == wgt)
		return;
	if (wgt->state == BUF_AVAILABLE) {
		widget_teardown(wgt);
		return;
	}
}

bool kid_interactive(struct widget *kid)
{
	if (!kid)
		return false;
	if (kid->kind == WIDGET_BUTTON)
		return true;
	if (kid->interact.node_count > 0 || kid->lua_build != LUA_NOREF)
		return true;
	return kid->lua_click != LUA_NOREF || kid->lua_hover != LUA_NOREF ||
	       kid->lua_leave != LUA_NOREF || kid->lua_press != LUA_NOREF ||
	       kid->lua_release != LUA_NOREF || kid->lua_scroll != LUA_NOREF ||
	       kid->lua_drag != LUA_NOREF || kid->lua_double_click != LUA_NOREF ||
	       kid->lua_key != LUA_NOREF || kid->keyboard_interactive;
}

static struct widget *find_hit_target(struct widget *wgt, int32_t x, int32_t y)
{
	for (int k = wgt->n_kids - 1; k >= 0; k--) {
		struct widget *kid = &widgets[wgt->kid_slots[k]];
		if (!kid->used || kid->want_destroy)
			continue;
		struct widget *hit = find_hit_target(kid, x, y);
		if (hit)
			return hit;
		if (kid_interactive(kid) &&
		    x >= kid->x && x < kid->x + (int)kid->width &&
		    y >= kid->y && y < kid->y + (int)kid->height)
			return kid;
	}
	return NULL;
}

void widget_pointer(struct widget *wgt, int type, int button, int32_t x, int32_t y)
{
	if (!wgt) {
		if (captured_widget && captured_widget->used && !captured_widget->want_destroy &&
		    (type == WISP_IN_MOTION || type == WISP_IN_RELEASE)) {
			wgt = captured_widget;
		} else {
			return;
		}
	}
	if (wgt->parent >= 0 || !wgt->used || wgt->want_destroy)
		return;

	if (type == WISP_IN_PRESS)
		captured_widget = wgt;

	float lx = (float)x;
	float ly = (float)y;

	if (wgt->interact.node_count > 0 || wgt->lua_build != LUA_NOREF ||
	    wgt->lua_click != LUA_NOREF || wgt->lua_hover != LUA_NOREF ||
	    wgt->lua_leave != LUA_NOREF || wgt->lua_press != LUA_NOREF ||
	    wgt->lua_release != LUA_NOREF || wgt->lua_drag != LUA_NOREF) {
		if (interact_handle_pointer(&wgt->interact, wgt, type, button, lx, ly, now_us()))
			wgt->want_redraw = 1;
		if (type == WISP_IN_RELEASE && captured_widget == wgt)
			captured_widget = NULL;
		return;
	}

	struct widget *target = find_hit_target(wgt, x, y);
	if (!target && kid_interactive(wgt))
		target = wgt;
	if (type == WISP_IN_ENTER) {
		if (widget_hover && widget_hover != target)
			lua_dispatch(wgt, widget_hover, "leave");
		widget_hover = target;
		if (target)
			lua_dispatch(wgt, target, "hover");
	} else if (type == WISP_IN_LEAVE) {
		if (widget_hover) {
			lua_dispatch(wgt, widget_hover, "leave");
			widget_hover = NULL;
		}
	} else if (type == WISP_IN_MOTION) {
		if (widget_hover && widget_hover != target)
			lua_dispatch(wgt, widget_hover, "leave");
		if (target && target != widget_hover)
			lua_dispatch(wgt, target, "hover");
		widget_hover = target;
	} else if (type == WISP_IN_PRESS) {
		if (target)
			lua_dispatch(wgt, target, "press");
	} else if (type == WISP_IN_RELEASE) {
		if (target) {
			lua_dispatch(wgt, target, "release");
			if (target->kind == WIDGET_BUTTON || target->lua_click != LUA_NOREF)
				lua_dispatch_click(wgt, target, x, y);
		}
		if (captured_widget == wgt)
			captured_widget = NULL;
	}
}

void widget_scroll(struct widget *wgt, int32_t x, int32_t y, int dir, float dx, float dy)
{
	if (!wgt || wgt->parent >= 0 || !wgt->used || wgt->want_destroy)
		return;

	float lx = (float)x;
	float ly = (float)y;

	if (wgt->interact.node_count > 0 || wgt->lua_scroll != LUA_NOREF) {
		if (interact_handle_scroll(&wgt->interact, wgt, lx, ly, dir, dx, dy, now_us()))
			wgt->want_redraw = 1;
		return;
	}

	struct widget *target = find_hit_target(wgt, x, y);
	if (!target && kid_interactive(wgt))
		target = wgt;
	if (target)
		lua_dispatch_scroll(wgt, target, dir, x, y);
}

void widget_key_event(struct widget *wgt, uint32_t keycode, uint32_t sym,
		      uint32_t unicode, const char *utf8_str, uint32_t mods,
		      bool is_press)
{
	(void)keycode;
	if (!wgt || wgt->parent >= 0 || !wgt->used || wgt->want_destroy)
		return;

	if (interact_handle_key(&wgt->interact, wgt, sym, unicode, utf8_str, mods, is_press, now_us()))
		wgt->want_redraw = 1;

	if (wgt->lua_key != LUA_NOREF && wgt->lua_L) {
		int ref = wgt->lua_key;
		if (lua_script_call_key(wgt, &ref, "key", sym, unicode, mods, is_press))
			wgt->want_redraw = 1;
	}
}

void widget_focus_event(struct widget *wgt, bool focused)
{
	if (!wgt || wgt->parent >= 0 || !wgt->used || wgt->want_destroy)
		return;

	if (focused) {
		if (wgt->lua_focus != LUA_NOREF && wgt->lua_L) {
			int ref = wgt->lua_focus;
			lua_script_call(wgt, &ref, "on_focus", NULL);
			wgt->want_redraw = 1;
		}
	} else {
		if (wgt->interact.focused_idx >= 0 && wgt->interact.focused_idx < wgt->interact.node_count) {
			struct interact_node *n = &wgt->interact.nodes[wgt->interact.focused_idx];
			n->state &= ~INTERACT_STATE_FOCUSED;
			if (n->lua_on_blur != LUA_NOREF && wgt->lua_L) {
				int ref = n->lua_on_blur;
				lua_script_call(wgt, &ref, "on_blur", NULL);
			}
			wgt->interact.focused_idx = -1;
			wgt->want_redraw = 1;
		}
		if (wgt->lua_blur != LUA_NOREF && wgt->lua_L) {
			int ref = wgt->lua_blur;
			lua_script_call(wgt, &ref, "on_blur", NULL);
			wgt->want_redraw = 1;
		}
	}
}

void service_widget(struct widget *wgt)
{
	if (!wgt->used)
		return;
	/* Child widgets that need an update are skipped here — they are
	 * composited by their parent's lua_widget_draw() call. */
	if (wgt->want_resize || wgt->want_redraw)
	if (wgt->parent >= 0)
		return;
	if (wgt->want_destroy) {
		if (wgt->state == BUF_AVAILABLE)
			widget_teardown(wgt);
		return;
	}
	if (!wgt->visible)
		return;
	if (wgt->use_egl) {
		if (wgt->want_resize) {
			wgt->want_resize = 0;
			uint32_t pw = (uint32_t)wgt->pend_w * wgt->scale;
			uint32_t ph = (uint32_t)wgt->pend_h * wgt->scale;
			if (!wgt->egl_surface) {
				if (g_renderer && g_renderer->surface_create)
					g_renderer->surface_create(wgt, pw, ph);
			} else {
				if (g_renderer && g_renderer->surface_resize)
					g_renderer->surface_resize(wgt, pw, ph);
			}
			if (wgt->surface && (kid_interactive(wgt) || wgt->interact.node_count > 0))
				widget_input_region(wgt);
			wgt->want_redraw = 1;
		}
		if (wgt->want_redraw && wgt->egl_surface) {
			wgt->want_redraw = 0;
			if (wgt->lua_L || wgt->kind != WIDGET_NONE)
				lua_widget_draw(wgt);
		}
		return;
	}
	if (wgt->state == BUF_AVAILABLE) {
		if (wgt->want_resize) {
			wgt->want_resize = 0;
			alloc_buffer(wgt, (uint32_t)wgt->pend_w * wgt->scale,
				     (uint32_t)wgt->pend_h * wgt->scale);
			if (wgt->surface)
				widget_input_region(wgt);
			if (wgt->layer_surface && (wgt->keyboard_interactive || wgt->interact.focused_idx >= 0)) {
				if (wl.layer_shell_version >= 4) {
					zwlr_layer_surface_v1_set_keyboard_interactivity(
						wgt->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
				}
			}
			wgt->want_redraw = 1;
		}
		if (wgt->want_redraw && wgt->pixels) {
			wgt->want_redraw = 0;
			if (wgt->lua_L || wgt->kind != WIDGET_NONE)
				lua_widget_draw(wgt);
		}
	} else if ((wgt->want_redraw || wgt->want_resize) && wgt->state == BUF_ATTACHED) {
		wgt->state = BUF_BUSY;
	}
}

struct widget *widget_add(bool visible, uint32_t width,
			 uint32_t height, const char *img_path)
{
	if (!wl.compositor)
		return NULL;
	struct widget *wgt = NULL;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (!widgets[k].used) {
			wgt = &widgets[k];
			break;
		}
	}
	if (!wgt) {
		fprintf(stderr, "wisp: widget registry full (%d slots)\n", MAX_WIDGETS);
		return NULL;
	}
	lua_widget_slot_reset((int)(wgt - widgets));
	*wgt = (struct widget){
		.width = width,
		.height = height,
		.state = BUF_AVAILABLE,
		.used = true,
		.visible = visible,
		.scale = 1,
		.pend_w = width,
		.pend_h = height,
		.watch_wd = -1,
		.parent = -1,
		.lua_env = LUA_NOREF,
		.lua_draw = LUA_NOREF,
		.lua_timer = LUA_NOREF,
		.lua_event = LUA_NOREF,
		.lua_thread_ref = LUA_NOREF,
		.lua_click = LUA_NOREF,
		.lua_hover = LUA_NOREF,
		.lua_leave = LUA_NOREF,
		.lua_press = LUA_NOREF,
		.lua_release = LUA_NOREF,
		.lua_scroll = LUA_NOREF,
		.lua_drag = LUA_NOREF,
		.lua_double_click = LUA_NOREF,
		.lua_key = LUA_NOREF,
		.lua_focus = LUA_NOREF,
		.lua_blur = LUA_NOREF,
	};
	interact_tree_init(&wgt->interact);
	snprintf(wgt->name, sizeof(wgt->name), "widget");

	wgt->surface = wl_compositor_create_surface(wl.compositor);
	wl_surface_add_listener(wgt->surface, &surface_listener, wgt);
	wgt->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		wl.layer_shell, wgt->surface, wl.output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		wgt->name);
	if (!wgt->layer_surface) {
		fprintf(stderr, "wisp: failed to create layer surface for %s\n", wgt->name);
		if (wgt->surface)
			wl_surface_destroy(wgt->surface);
		wgt->surface = NULL;
		wgt->used = false;
		return NULL;
	}
	wgt->img = img_path ? image_get(img_path) : NULL;
	zwlr_layer_surface_v1_add_listener(wgt->layer_surface, &layer_surface_listener, wgt);
	zwlr_layer_surface_v1_set_size(wgt->layer_surface, width, height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		wgt->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	struct wl_region *region = wl_compositor_create_region(wl.compositor);
	wl_region_add(region, 0, 0, 0, 0);
	/* Start with an empty input region so undecorated surfaces don't steal
	 * pointer events.  Call widget_input_region() after adding children. */
	wl_surface_set_input_region(wgt->surface, region);
	wl_region_destroy(region);
	return wgt;
}

struct widget *widget_add_toplevel(bool visible, uint32_t width,
				   uint32_t height, const char *title,
				   const char *app_id)
{
	struct widget *wgt = NULL;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (!widgets[k].used) {
			wgt = &widgets[k];
			break;
		}
	}
	if (!wgt) {
		fprintf(stderr, "wisp: surface registry full (%d slots)\n", MAX_WIDGETS);
		return NULL;
	}
	lua_widget_slot_reset((int)(wgt - widgets));
	*wgt = (struct widget){
		.width = width,
		.height = height,
		.state = BUF_AVAILABLE,
		.used = true,
		.visible = visible,
		.scale = 1,
		.pend_w = width,
		.pend_h = height,
		.watch_wd = -1,
		.parent = -1,
		.surf_type = SURFACE_TOPLEVEL,
		.app_id_slot = -1,
		.lua_env = LUA_NOREF,
		.lua_draw = LUA_NOREF,
		.lua_timer = LUA_NOREF,
		.lua_event = LUA_NOREF,
		.lua_thread_ref = LUA_NOREF,
		.lua_click = LUA_NOREF,
		.lua_hover = LUA_NOREF,
		.lua_leave = LUA_NOREF,
		.lua_press = LUA_NOREF,
		.lua_release = LUA_NOREF,
		.lua_scroll = LUA_NOREF,
		.lua_drag = LUA_NOREF,
		.lua_double_click = LUA_NOREF,
		.lua_key = LUA_NOREF,
		.lua_focus = LUA_NOREF,
		.lua_blur = LUA_NOREF,
		.keyboard_interactive = true,
	};
	interact_tree_init(&wgt->interact);
	const char *t = (title && title[0]) ? title : "wisp";
	const char *aid = (app_id && app_id[0]) ? app_id : "org.wisp.app";
	snprintf(wgt->title, sizeof(wgt->title), "%s", t);
	snprintf(wgt->app_id, sizeof(wgt->app_id), "%s", aid);
	snprintf(wgt->name, sizeof(wgt->name), "%s", t);

	if (!wl.compositor || !wl.wm_base) {
		/* Headless mode without Wayland compositor (e.g. selftest) */
		return wgt;
	}

	wgt->surface = wl_compositor_create_surface(wl.compositor);
	wl_surface_add_listener(wgt->surface, &surface_listener, wgt);

	wgt->xdg_surface = xdg_wm_base_get_xdg_surface(wl.wm_base, wgt->surface);
	if (!wgt->xdg_surface) {
		fprintf(stderr, "wisp: failed to create xdg_surface for %s\n", wgt->name);
		wl_surface_destroy(wgt->surface);
		wgt->surface = NULL;
		wgt->used = false;
		return NULL;
	}
	xdg_surface_add_listener(wgt->xdg_surface, &xdg_surface_listener, wgt);

	wgt->xdg_toplevel = xdg_surface_get_toplevel(wgt->xdg_surface);
	if (!wgt->xdg_toplevel) {
		fprintf(stderr, "wisp: failed to create xdg_toplevel for %s\n", wgt->name);
		xdg_surface_destroy(wgt->xdg_surface);
		wgt->xdg_surface = NULL;
		wl_surface_destroy(wgt->surface);
		wgt->surface = NULL;
		wgt->used = false;
		return NULL;
	}
	xdg_toplevel_add_listener(wgt->xdg_toplevel, &xdg_toplevel_listener, wgt);

	xdg_toplevel_set_title(wgt->xdg_toplevel, wgt->title);
	xdg_toplevel_set_app_id(wgt->xdg_toplevel, wgt->app_id);

	wl_surface_set_input_region(wgt->surface, NULL);
	wl_surface_commit(wgt->surface);

	return wgt;
}

void widget_start_move(struct widget *wgt)
{
	if (!wgt || wgt->surf_type != SURFACE_TOPLEVEL || !wgt->xdg_toplevel || !wl.seat || !wl.pointer_serial)
		return;
	xdg_toplevel_move(wgt->xdg_toplevel, wl.seat, wl.pointer_serial);
}

void widget_start_resize(struct widget *wgt, uint32_t edges)
{
	if (!wgt || wgt->surf_type != SURFACE_TOPLEVEL || !wgt->xdg_toplevel || !wl.seat || !wl.pointer_serial)
		return;
	xdg_toplevel_resize(wgt->xdg_toplevel, wl.seat, wl.pointer_serial, edges);
}

void widget_set_fullscreen(struct widget *wgt, bool fullscreen)
{
	if (!wgt) return;
	if (wgt->surf_type == SURFACE_TOPLEVEL && wgt->xdg_toplevel) {
		if (fullscreen)
			xdg_toplevel_set_fullscreen(wgt->xdg_toplevel, NULL);
		else
			xdg_toplevel_unset_fullscreen(wgt->xdg_toplevel);
	}
}

void widget_set_maximized(struct widget *wgt, bool maximized)
{
	if (!wgt) return;
	if (wgt->surf_type == SURFACE_TOPLEVEL && wgt->xdg_toplevel) {
		if (maximized)
			xdg_toplevel_set_maximized(wgt->xdg_toplevel);
		else
			xdg_toplevel_unset_maximized(wgt->xdg_toplevel);
	}
}

void widget_set_minimized(struct widget *wgt)
{
	if (wgt && wgt->surf_type == SURFACE_TOPLEVEL && wgt->xdg_toplevel)
		xdg_toplevel_set_minimized(wgt->xdg_toplevel);
}

void widget_set_title(struct widget *wgt, const char *title)
{
	if (!wgt || !title)
		return;
	snprintf(wgt->title, sizeof(wgt->title), "%s", title);
	if (wgt->surf_type == SURFACE_TOPLEVEL && wgt->xdg_toplevel)
		xdg_toplevel_set_title(wgt->xdg_toplevel, wgt->title);
}

struct widget *widget_add_child(int parent, int kind, uint32_t width,
			 uint32_t height, const char *img_path)
{
	int k = 0;
	while (k < MAX_WIDGETS && widgets[k].used)
		k++;
	if (k >= MAX_WIDGETS) {
		fprintf(stderr, "wisp: widget registry full (%d slots)\n", MAX_WIDGETS);
		return NULL;
	}
	if (parent >= MAX_WIDGETS || parent < -1)
		return NULL;
	struct widget *owner = (parent >= 0) ? &widgets[parent] : NULL;
	if (owner && !owner->used)
		owner = NULL;
	if (owner && owner->n_kids < MAX_KIDS)
		owner->kid_slots[owner->n_kids++] = k;
	struct widget *w = &widgets[k];
	memset(w, 0, sizeof(*w));
	snprintf(w->name, sizeof(w->name), "kid%d", k);
	w->parent = parent;
	w->kid = true;
	w->used = true;
	w->visible = true;
	w->state = BUF_AVAILABLE;
	w->kind = kind;
	w->width = width;
	w->height = height;
	w->pend_w = width;
	w->pend_h = height;
	w->scale = owner ? owner->scale : 1;
	w->img = img_path ? image_get(img_path) : NULL;
	w->background = COLOR_IDLE;
	w->lua_env = LUA_NOREF;
	w->lua_draw = LUA_NOREF;
	w->lua_timer = LUA_NOREF;
	w->lua_event = LUA_NOREF;
	w->lua_thread_ref = LUA_NOREF;
	w->lua_click = LUA_NOREF;
	w->lua_hover = LUA_NOREF;
	w->lua_leave = LUA_NOREF;
	w->lua_press = LUA_NOREF;
	w->lua_release = LUA_NOREF;
	w->lua_scroll = LUA_NOREF;
	w->watch_wd = -1;
	return w;
}

void widget_kill(struct widget *wgt)
{
	if (!wgt || !wgt->used)
		return;
	wgt->want_destroy = 1;
	for (int k = 0; k < wgt->n_kids; k++)
		widget_kill(&widgets[wgt->kid_slots[k]]);
	wgt->n_kids = 0;
}

void widget_detach(struct widget *wgt)
{
	if (wgt->parent < 0)
		return;
	struct widget *owner = &widgets[wgt->parent];
	for (int k = 0; k < owner->n_kids; k++) {
		if (owner->kid_slots[k] == (int)(wgt - widgets)) {
			for (int m = k; m + 1 < owner->n_kids; m++)
				owner->kid_slots[m] = owner->kid_slots[m + 1];
			owner->n_kids--;
			break;
		}
	}
}

static int kid_natural_w(struct widget *kid)
{
	switch (kid->kind) {
	case WIDGET_TEXT:
		return kid->text ? text_measure(kid->text, kid->text_size) : kid->text_size;
	case WIDGET_IMAGE:
		return kid->img ? kid->img->w : kid->img_w;
	case WIDGET_BUTTON: {
		if (kid->width > 0)
			return (int)kid->width;
		int w = kid->text ? text_measure(kid->text, kid->text_size) : kid->text_size;
		return w + kid->pad[3] + kid->pad[1] + 2 * kid->radius;
	}
	case WIDGET_SPACER:
		return kid->img_w > 0 ? kid->img_w : kid->width;
	case WIDGET_PROGRESS:
	case WIDGET_SEPARATOR:
		return kid->width;
	default:
		return kid->width;
	}
}

static int kid_natural_h(struct widget *kid)
{
	switch (kid->kind) {
	case WIDGET_TEXT:
		return kid->text_size > 0 ? kid->text_size : kid->height;
	case WIDGET_IMAGE:
		return kid->img ? kid->img->h : kid->img_h;
	case WIDGET_BUTTON: {
		if (kid->height > 0)
			return (int)kid->height;
		int h = kid->text ? kid->text_size + kid->pad[0] + kid->pad[2] + 2 * kid->radius
				  : kid->text_size;
		return h;
	}
	case WIDGET_SPACER:
		return kid->img_h > 0 ? kid->img_h : kid->height;
	case WIDGET_PROGRESS:
		return kid->height > 0 ? kid->height : 12;
	case WIDGET_SEPARATOR:
		return kid->height > 0 ? kid->height : 1;
	default:
		return kid->height;
	}
}

static void widget_calc_size(struct widget *wgt)
{
	if (!wgt->used || wgt->want_destroy)
		return;
	if (wgt->n_kids <= 0 || (wgt->kind != WIDGET_BOX && wgt->kind != WIDGET_ROW && wgt->kind != WIDGET_COLUMN))
		return;

	for (int k = 0; k < wgt->n_kids; k++)
		widget_calc_size(&widgets[wgt->kid_slots[k]]);

	if (wgt->kind == WIDGET_COLUMN) {
		int maxw = 0, total = wgt->pad[0] + wgt->pad[2];
		for (int k = 0; k < wgt->n_kids; k++) {
			struct widget *kid = &widgets[wgt->kid_slots[k]];
			if (!kid->width)
				kid->width = (uint32_t)kid_natural_w(kid);
			if (!kid->height)
				kid->height = (uint32_t)kid_natural_h(kid);
			if ((int)kid->width > maxw)
				maxw = (int)kid->width;
			total += (int)kid->height;
		}
		if (wgt->n_kids > 1)
			total += wgt->gap * (wgt->n_kids - 1);
		if (wgt->height == 0 || (int)wgt->height < total)
			wgt->height = (uint32_t)(total < 8 ? 8 : total);
		if (wgt->width == 0 || (int)wgt->width < maxw)
			wgt->width = (uint32_t)(maxw < 8 ? 8 : maxw);
	} else {
		int maxh = 0, total = wgt->pad[3] + wgt->pad[1];
		for (int k = 0; k < wgt->n_kids; k++) {
			struct widget *kid = &widgets[wgt->kid_slots[k]];
			if (!kid->width)
				kid->width = (uint32_t)kid_natural_w(kid);
			if (!kid->height)
				kid->height = (uint32_t)kid_natural_h(kid);
			if ((int)kid->height > maxh)
				maxh = (int)kid->height;
			total += (int)kid->width;
		}
		if (wgt->n_kids > 1)
			total += wgt->gap * (wgt->n_kids - 1);
		if (wgt->width == 0 || (int)wgt->width < total)
			wgt->width = (uint32_t)(total < 8 ? 8 : total);
		if (wgt->height == 0 || (int)wgt->height < maxh)
			wgt->height = (uint32_t)(maxh < 8 ? 8 : maxh);
	}
}

static void widget_calc_pos(struct widget *wgt)
{
	if (!wgt->used || wgt->want_destroy)
		return;
	if (wgt->n_kids <= 0 || (wgt->kind != WIDGET_BOX && wgt->kind != WIDGET_ROW && wgt->kind != WIDGET_COLUMN))
		return;

	if (wgt->kind == WIDGET_COLUMN) {
		int content_w = wgt->width - wgt->pad[3] - wgt->pad[1];
		int cursor = wgt->y + wgt->pad[0];
		for (int k = 0; k < wgt->n_kids; k++) {
			struct widget *kid = &widgets[wgt->kid_slots[k]];
			kid->y = cursor;
			int rem = content_w - (int)kid->width;
			/* align: 0=start → +0, 1=center → +rem/2, 2=end → +rem */
			kid->x = wgt->x + wgt->pad[3] + (rem > 0 ? wgt->align * rem / 2 : 0);
			cursor += (int)kid->height + wgt->gap;
			widget_calc_pos(kid);
		}
	} else {
		int content_h = wgt->height - wgt->pad[0] - wgt->pad[2];
		int cursor = wgt->x + wgt->pad[3];
		for (int k = 0; k < wgt->n_kids; k++) {
			struct widget *kid = &widgets[wgt->kid_slots[k]];
			kid->x = cursor;
			int rem = content_h - (int)kid->height;
			kid->y = wgt->y + wgt->pad[0] + (rem > 0 ? wgt->align * rem / 2 : 0);
			cursor += (int)kid->width + wgt->gap;
			widget_calc_pos(kid);
		}
	}
}

void widget_layout(struct widget *wgt)
{
	if (!wgt->used || wgt->want_destroy)
		return;
	widget_calc_size(wgt);
	widget_calc_pos(wgt);
}

struct widget *g_runtime_registered_widget = NULL;

struct widget *widget_load_dynamic(const char *path)
{
	if (!path || !path[0])
		return NULL;

	if (!Lm)
		lua_vm_init(0);
	if (!Lm) {
		fprintf(stderr, "wisp: Lua runtime not initialized\n");
		return NULL;
	}

	g_runtime_registered_widget = NULL;

	/* Reset wisp lifecycle callbacks and pending options */
	lua_getglobal(Lm, "wisp");
	if (lua_istable(Lm, -1)) {
		lua_pushnil(Lm); lua_setfield(Lm, -2, "load");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "update");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "draw");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "render");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "keypressed");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "keyreleased");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "mousepressed");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "mousereleased");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "_active_widget");
		lua_pushnil(Lm); lua_setfield(Lm, -2, "_runtime_app_registered");

		lua_getfield(Lm, -1, "opt");
		if (lua_istable(Lm, -1)) {
			lua_pushstring(Lm, "_pending");
			lua_newtable(Lm);
			lua_rawset(Lm, -3);
			lua_pushstring(Lm, "_wgt");
			lua_pushnil(Lm);
			lua_rawset(Lm, -3);
		}
		lua_pop(Lm, 1);
	}
	lua_pop(Lm, 1);

	/* Execute script exactly once */
	if (luaL_dofile(Lm, path) != LUA_OK) {
		fprintf(stderr, "wisp: error executing '%s': %s\n", path, lua_tostring(Lm, -1));
		lua_pop(Lm, 1);
		return NULL;
	}

	/* Trigger auto-mount if Wisp 2.0 app callbacks or options were defined */
	lua_getglobal(Lm, "wisp");
	if (lua_istable(Lm, -1)) {
		lua_getfield(Lm, -1, "_auto_mount");
		if (lua_isfunction(Lm, -1)) {
			lua_pushstring(Lm, path);
			if (lua_pcall(Lm, 1, 0, 0) != LUA_OK) {
				fprintf(stderr, "wisp: auto_mount failed for '%s': %s\n", path, lua_tostring(Lm, -1));
				lua_pop(Lm, 1);
			}
		} else {
			lua_pop(Lm, 1);
		}
	}
	lua_pop(Lm, 1);

	if (g_runtime_registered_widget) {
		struct widget *wgt = g_runtime_registered_widget;
		fprintf(stderr, "wisp: dynamic app '%s' registered from %s\n", wgt->name, path);
		if (wgt->surface)
			wl_surface_commit(wgt->surface);
		return wgt;
	}

	return NULL;
}

bool widget_unload_dynamic(const char *name_or_slot)
{
	if (!name_or_slot || !name_or_slot[0])
		return false;
	struct widget *target = NULL;
	if (name_or_slot[0] >= '0' && name_or_slot[0] <= '9' && !name_or_slot[1]) {
		int idx = name_or_slot[0] - '0';
		if (idx >= 0 && idx < MAX_WIDGETS && widgets[idx].used)
			target = &widgets[idx];
	}
	if (!target) {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			struct widget *w = &widgets[k];
			if (!w->used)
				continue;
			const char *base = strrchr(w->script_path, '/');
			const char *sname = base ? base + 1 : w->script_path;
			if (strcmp(w->name, name_or_slot) == 0 ||
			    strcmp(w->script_path, name_or_slot) == 0 ||
			    strcmp(sname, name_or_slot) == 0) {
				target = w;
				break;
			}
		}
	}
	if (!target) {
		fprintf(stderr, "wisp: widget '%s' not found for unload\n", name_or_slot);
		return false;
	}
	fprintf(stderr, "wisp: unloading widget '%s'\n", target->name);
	widget_destroy(target);
	return true;
}

static void add_input_regions(struct wl_region *region, struct widget *wgt)
{
	for (int k = wgt->n_kids - 1; k >= 0; k--) {
		struct widget *kid = &widgets[wgt->kid_slots[k]];
		if (!kid->used || kid->want_destroy)
			continue;
		add_input_regions(region, kid);
		if (kid_interactive(kid) && kid->x >= 0 && kid->y >= 0 &&
		    kid->width > 0 && kid->height > 0)
			wl_region_add(region, kid->x, kid->y, (uint32_t)kid->width, (uint32_t)kid->height);
	}
	if (kid_interactive(wgt) && wgt->width > 0 && wgt->height > 0) {
		wl_region_add(region, 0, 0, (uint32_t)wgt->width, (uint32_t)wgt->height);
	}
}

void widget_input_region(struct widget *wgt)
{
	if (!wl.compositor || !wgt || !wgt->surface)
		return;

	if (kid_interactive(wgt) || wgt->interact.node_count > 0 ||
	    wgt->lua_build != LUA_NOREF || wgt->lua_click != LUA_NOREF ||
	    wgt->lua_hover != LUA_NOREF || wgt->lua_scroll != LUA_NOREF ||
	    wgt->lua_drag != LUA_NOREF || wgt->keyboard_interactive) {
		wl_surface_set_input_region(wgt->surface, NULL);
		return;
	}

	struct wl_region *region = wl_compositor_create_region(wl.compositor);
	if (!region)
		return;
	wl_region_add(region, 0, 0, 0, 0);
	add_input_regions(region, wgt);
	wl_surface_set_input_region(wgt->surface, region);
	wl_region_destroy(region);
}

bool widget_open_named(const char *name, char *err_buf, size_t err_len)
{
	if (!name || !name[0]) {
		if (err_buf) snprintf(err_buf, err_len, "empty widget name");
		return false;
	}
	struct staged_widget_spec *sp = registry_find(name);
	if (!sp) {
		if (err_buf) snprintf(err_buf, err_len, "widget '%s' is not registered", name);
		return false;
	}
	if (!sp->enabled) {
		if (err_buf) snprintf(err_buf, err_len, "widget '%s' is disabled in configuration", name);
		return false;
	}
	for (int i = 0; i < sp->dep_count; i++) {
		const char *dep = sp->deps[i];
		struct staged_widget_spec *dep_sp = registry_find(dep);
		if (!dep_sp) {
			if (err_buf) snprintf(err_buf, err_len, "dependency '%s' is not registered", dep);
			return false;
		}
		if (!dep_sp->enabled) {
			if (err_buf) snprintf(err_buf, err_len, "dependency '%s' is disabled", dep);
			return false;
		}
		if (dep_sp->instance_slot < 0 || !widgets[dep_sp->instance_slot].used) {
			if (!widget_open_named(dep, err_buf, err_len))
				return false;
		}
	}
	if (!sp->allow_multiple && sp->instance_slot >= 0 && widgets[sp->instance_slot].used) {
		struct widget *w = &widgets[sp->instance_slot];
		if (!w->visible) {
			widget_show(w);
			sp->state = WIDGET_STATE_VISIBLE;
		}
		sp->last_error[0] = '\0';
		return true;
	}
	if (sp->path[0] && access(sp->path, R_OK) != 0) {
		snprintf(sp->last_error, sizeof(sp->last_error), "script '%s' not found or unreadable", sp->path);
		if (err_buf) snprintf(err_buf, err_len, "%s", sp->last_error);
		return false;
	}
	struct widget *w = NULL;
	if (wl.compositor) {
		w = widget_add(sp->has_visible ? sp->visible : true,
			       sp->width > 0 ? sp->width : 100,
			       sp->height > 0 ? sp->height : 50, NULL);
		if (!w) {
			if (err_buf) snprintf(err_buf, err_len, "no free widget slots");
			return false;
		}
	} else {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (!widgets[k].used) {
				w = &widgets[k];
				break;
			}
		}
		if (!w) {
			if (err_buf) snprintf(err_buf, err_len, "no free widget slots");
			return false;
		}
		lua_widget_slot_reset((int)(w - widgets));
		*w = (struct widget){
			.width = sp->width > 0 ? sp->width : 100,
			.height = sp->height > 0 ? sp->height : 50,
			.state = BUF_AVAILABLE,
			.used = true,
			.visible = sp->has_visible ? sp->visible : true,
			.scale = 1,
			.pend_w = sp->width > 0 ? sp->width : 100,
			.pend_h = sp->height > 0 ? sp->height : 50,
			.watch_wd = -1,
			.parent = -1,
			.lua_env = LUA_NOREF,
			.lua_draw = LUA_NOREF,
			.lua_timer = LUA_NOREF,
			.lua_event = LUA_NOREF,
			.lua_thread_ref = LUA_NOREF,
			.lua_click = LUA_NOREF,
			.lua_hover = LUA_NOREF,
			.lua_leave = LUA_NOREF,
			.lua_press = LUA_NOREF,
			.lua_release = LUA_NOREF,
			.lua_scroll = LUA_NOREF,
			.lua_drag = LUA_NOREF,
			.lua_double_click = LUA_NOREF,
			.lua_key = LUA_NOREF,
			.lua_focus = LUA_NOREF,
			.lua_blur = LUA_NOREF,
		};
		interact_tree_init(&w->interact);
	}
	w->scale = (sp->scale >= 1 && sp->scale <= 4) ? sp->scale : (g_config.scale >= 1 ? g_config.scale : 1);
	snprintf(w->name, sizeof(w->name), "%s", sp->name);
	if (!lua_widget_apply_spec(w, sp)) {
		snprintf(sp->last_error, sizeof(sp->last_error), "script initialization failed");
		if (err_buf) snprintf(err_buf, err_len, "%s", sp->last_error);
		widget_teardown(w);
		return false;
	}
	snprintf(w->name, sizeof(w->name), "%s", sp->name);
	sp->instance_slot = (int)(w - widgets);
	sp->state = w->visible ? WIDGET_STATE_VISIBLE : WIDGET_STATE_LOADED;
	sp->last_error[0] = '\0';
	if (w->surface)
		wl_surface_commit(w->surface);
	return true;
}

bool widget_close_named(const char *name, char *err_buf, size_t err_len)
{
	if (!name || !name[0]) {
		if (err_buf) snprintf(err_buf, err_len, "empty widget name");
		return false;
	}
	struct staged_widget_spec *sp = registry_find(name);
	if (sp) {
		if (sp->instance_slot >= 0 && widgets[sp->instance_slot].used) {
			widget_teardown(&widgets[sp->instance_slot]);
			sp->instance_slot = -1;
			sp->state = WIDGET_STATE_UNLOADED;
			return true;
		}
		return true;
	}
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used && strcmp(widgets[k].name, name) == 0) {
			widget_teardown(&widgets[k]);
			return true;
		}
	}
	if (err_buf) snprintf(err_buf, err_len, "widget '%s' is not registered", name);
	return false;
}

bool widget_toggle_named(const char *name, char *err_buf, size_t err_len)
{
	if (!name || !name[0]) {
		if (err_buf) snprintf(err_buf, err_len, "empty widget name");
		return false;
	}
	struct staged_widget_spec *sp = registry_find(name);
	if (sp) {
		if (sp->instance_slot >= 0 && widgets[sp->instance_slot].used && widgets[sp->instance_slot].visible)
			return widget_close_named(name, err_buf, err_len);
		else
			return widget_open_named(name, err_buf, err_len);
	}
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used && strcmp(widgets[k].name, name) == 0)
			return widget_close_named(name, err_buf, err_len);
	}
	if (err_buf) snprintf(err_buf, err_len, "widget '%s' is not registered", name);
	return false;
}

bool widget_reload_named(const char *name, char *err_buf, size_t err_len)
{
	if (!name || !name[0]) {
		if (err_buf) snprintf(err_buf, err_len, "empty widget name");
		return false;
	}
	struct staged_widget_spec *sp = registry_find(name);
	if (sp) {
		if (sp->instance_slot >= 0 && widgets[sp->instance_slot].used) {
			bool ok = lua_widget_reload(&widgets[sp->instance_slot]);
			if (!ok) {
				snprintf(sp->last_error, sizeof(sp->last_error), "reload failed, keeping current version");
				if (err_buf) snprintf(err_buf, err_len, "%s", sp->last_error);
				return false;
			}
			sp->last_error[0] = '\0';
			return true;
		}
		if (sp->path[0] && access(sp->path, R_OK) != 0) {
			snprintf(sp->last_error, sizeof(sp->last_error), "script '%s' not accessible", sp->path);
			if (err_buf) snprintf(err_buf, err_len, "%s", sp->last_error);
			return false;
		}
		if (err_buf) snprintf(err_buf, err_len, "widget '%s' is registered but not open", name);
		return true;
	}
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used && (strcmp(widgets[k].name, name) == 0 || strcmp(widgets[k].script_path, name) == 0))
			return lua_widget_reload(&widgets[k]);
	}
	if (err_buf) snprintf(err_buf, err_len, "widget '%s' is not registered", name);
	return false;
}

void widget_list(FILE *out)
{
	fprintf(out, "NAME             STATE       AUTOSTART  ENABLED  ORDER  PATH\n");
	for (int i = 0; i < g_registry.count; i++) {
		struct staged_widget_spec *sp = &g_registry.entries[i];
		const char *state_str = "unloaded";
		if (sp->instance_slot >= 0 && sp->instance_slot < MAX_WIDGETS && widgets[sp->instance_slot].used)
			state_str = widgets[sp->instance_slot].visible ? "visible" : "loaded";
		fprintf(out, "%-16s %-11s %-10s %-8s %-6d %s\n",
			sp->name, state_str,
			sp->autostart ? "yes" : "no",
			sp->enabled ? "yes" : "no",
			sp->order,
			sp->path[0] ? sp->path : "(inline)");
	}
	fflush(out);
}

void widget_runtime_status(FILE *out)
{
	fprintf(out, "=== wisp runtime status ===\n");
#if WISP_HAS_LUA
	fprintf(out, "Lua VM memory: %zu bytes (limit: %zu bytes, peak: %zu bytes, failures: %zu)\n",
		lua_used, lua_limit, lua_peak, lua_alloc_failures);
#else
	fprintf(out, "Lua VM: disabled at build time\n");
#endif
	int active = 0;
	for (int k = 0; k < MAX_WIDGETS; k++)
		if (widgets[k].used) active++;
	fprintf(out, "Compositor: connected (registry: %d widgets, %d active)\n", g_registry.count, active);
	fprintf(out, "\n[Registered Widgets]\n");
	widget_list(out);

	fprintf(out, "\n[Active Widget Slots]\n");
	fprintf(out, "SLOT  NAME             TYPE    STATE     SIZE       ANCHOR  TIMER    DRAWS  MEMORY   SCRIPT\n");
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *w = &widgets[k];
		if (!w->used)
			continue;
		const char *type = w->lua_L ? "lua" : "native";
		const char *state = w->want_destroy ? "teardown" : (w->visible ? "visible" : "hidden");
		char mem_str[32] = "-";
		if (w->lua_L)
			snprintf(mem_str, sizeof(mem_str), "%zu KB", w->lua_mem_used / 1024);
		fprintf(out, "[%2d]  %-16s %-7s %-9s %4ux%-4u 0x%02x    %5dms  %-6u %-8s %s\n",
			k, w->name, type, state, w->width, w->height, w->anchor,
			w->lua_L ? w->lua_timer_ms : 0, w->draws, mem_str,
			w->script_path[0] ? w->script_path : "-");
	}
	bool have_errors = false;
	for (int i = 0; i < g_registry.count; i++) {
		if (g_registry.entries[i].last_error[0]) {
			if (!have_errors) {
				fprintf(out, "\n[Diagnostics & Errors]\n");
				have_errors = true;
			}
			fprintf(out, "  %s: %s\n", g_registry.entries[i].name, g_registry.entries[i].last_error);
		}
	}
	fprintf(out, "============================\n");
	fflush(out);
}
