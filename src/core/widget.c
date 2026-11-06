#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/inotify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/types.h"
#include "core/widget.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "services/system.h"
#include "scripting/scripting.h"
#include "wayland/wayland.h"

struct widget widgets[MAX_WIDGETS];
static struct widget *widget_hover;

void widget_teardown(struct widget *wgt)
{
	for (int k = 0; k < wgt->n_kids; k++) {
		int idx = wgt->kid_slots[k];
		if (idx >= 0 && idx < MAX_WIDGETS && &widgets[idx] != wgt &&
		    widgets[idx].used)
			widget_teardown(&widgets[idx]);
	}
	wgt->n_kids = 0;
	if (widget_hover == wgt)
		widget_hover = NULL;
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
	lua_widget_free(wgt);
	scaled_release(wgt->scaled);
	wgt->scaled = NULL;
	wgt->last_scaled = NULL;
	image_release(wgt->img);
	wgt->img = NULL;
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
	return kid->lua_click != LUA_NOREF || kid->lua_hover != LUA_NOREF ||
	       kid->lua_leave != LUA_NOREF || kid->lua_press != LUA_NOREF ||
	       kid->lua_release != LUA_NOREF || kid->lua_scroll != LUA_NOREF;
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

void widget_pointer(struct widget *wgt, int type, int32_t x, int32_t y)
{
	if (!wgt || wgt->parent >= 0 || !wgt->used)
		return;
	struct widget *target = find_hit_target(wgt, x, y);
	if (!target && kid_interactive(wgt))
		target = wgt;
	if (type == WISP_IN_ENTER) {
		if (widget_hover && widget_hover != target)
			lua_dispatch(wgt, widget_hover, "leave");
		widget_hover = target;
		if (target)
			lua_dispatch(wgt, target, "hover");
		return;
	}
	if (type == WISP_IN_LEAVE) {
		if (widget_hover) {
			lua_dispatch(wgt, widget_hover, "leave");
			widget_hover = NULL;
		}
		return;
	}
	if (type == WISP_IN_MOTION) {
		if (widget_hover && widget_hover != target)
			lua_dispatch(wgt, widget_hover, "leave");
		if (target && target != widget_hover)
			lua_dispatch(wgt, target, "hover");
		widget_hover = target;
		return;
	}
	if (type == WISP_IN_SCROLL) {
		if (target)
			lua_dispatch_scroll(wgt, target, 1, x, y);
		return;
	}
	if (!target)
		return;
	if (type == WISP_IN_PRESS) {
		lua_dispatch(wgt, target, "press");
		return;
	}
	if (type == WISP_IN_RELEASE) {
		lua_dispatch(wgt, target, "release");
		if (target->kind == WIDGET_BUTTON || target->lua_click != LUA_NOREF)
			lua_dispatch_click(wgt, target, x, y);
	}
}

void widget_scroll(struct widget *wgt, int32_t x, int32_t y, int dir)
{
	if (!wgt || wgt->parent >= 0 || !wgt->used)
		return;
	struct widget *target = find_hit_target(wgt, x, y);
	if (!target && kid_interactive(wgt))
		target = wgt;
	if (target)
		lua_dispatch_scroll(wgt, target, dir, x, y);
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
	if (wgt->state == BUF_AVAILABLE) {
		if (wgt->want_resize) {
			wgt->want_resize = 0;
			alloc_buffer(wgt, (uint32_t)wgt->pend_w * wgt->scale,
				     (uint32_t)wgt->pend_h * wgt->scale);
			if (wgt->surface && kid_interactive(wgt))
				widget_input_region(wgt);
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
	};
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

struct widget *widget_load_dynamic(const char *path)
{
	if (!path || !path[0])
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "wisp: cannot open script '%s'\n", path);
		return NULL;
	}
	fclose(f);

	struct widget *wgt = widget_add(true, 1, 1, NULL);
	if (!wgt)
		return NULL;
	if (!lua_widget_load(wgt, path)) {
		widget_teardown(wgt);
		return NULL;
	}
	fprintf(stderr, "wisp: dynamic widget '%s' loaded from %s\n", wgt->name, path);
	if (wgt->surface)
		wl_surface_commit(wgt->surface);
	return wgt;
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
	struct wl_region *region = wl_compositor_create_region(wl.compositor);
	if (!region)
		return;
	wl_region_add(region, 0, 0, 0, 0);
	add_input_regions(region, wgt);
	wl_surface_set_input_region(wgt->surface, region);
	wl_region_destroy(region);
}
