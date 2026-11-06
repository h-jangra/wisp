#include "wayland/wayland.h"
#include "core/types.h"
#include "core/widget.h"
#include <stdio.h>
#include <string.h>

const struct wl_interface xdg_popup_interface = {"xdg_popup", 1, 0, NULL, 0, NULL};

struct wayland_context wl;
struct output_entry outputs[MAX_OUTPUTS];
int output_count = 0;

static void configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
		      uint32_t width, uint32_t height)
{
	struct widget *wgt = data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);

	if (width == 0 || height == 0) {
		width = wgt->pend_w;
		height = wgt->pend_h;
	}
	/* Skip resize if dimensions are unchanged or widget is hidden —
	 * alloc_buffer will be triggered when the widget becomes visible. */
	if ((wgt->buffer && (int)width * wgt->scale == (int)wgt->width &&
	     (int)height * wgt->scale == (int)wgt->height) || !wgt->visible)
		return;

	wgt->pend_w = width;
	wgt->pend_h = height;
	wgt->want_resize = 1;
}

static void closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)ls;
	widget_destroy(data);
}

const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = configure,
	.closed = closed,
};

/*
 * Called as a wl_surface preferred_buffer_scale event listener and also
 * directly from output_scale when a display scale changes.
 *
 * Converts the current physical dimensions back to logical (÷ old_scale),
 * stores them in pend_w/pend_h, and sets want_resize so alloc_buffer will
 * create a new buffer of pend_w*new_scale × pend_h*new_scale.
 */
void surface_preferred_scale(void *data, struct wl_surface *surface,
			    int32_t factor)
{
	(void)surface;
	struct widget *wgt = data;
	if (!wgt || factor < 1 || factor == wgt->scale)
		return;
	int old_scale = wgt->scale > 0 ? wgt->scale : 1;
	int32_t logical_w = (int32_t)(wgt->width / old_scale);
	int32_t logical_h = (int32_t)(wgt->height / old_scale);
	if (logical_w < 1)
		logical_w = wgt->pend_w > 0 ? wgt->pend_w : 1;
	if (logical_h < 1)
		logical_h = wgt->pend_h > 0 ? wgt->pend_h : 1;
	wgt->pend_w = logical_w;
	wgt->pend_h = logical_h;
	wgt->scale = factor;
	wgt->want_resize = 1;
}

static void output_geometry(void *data, struct wl_output *wl_output,
			    int32_t x, int32_t y, int32_t physical_width,
			    int32_t physical_height, int32_t subpixel,
			    const char *make, const char *model, int32_t transform)
{
	(void)wl_output; (void)x; (void)y; (void)physical_width; (void)physical_height;
	(void)subpixel; (void)transform;
	struct output_entry *oe = data;
	if (make) snprintf(oe->make, sizeof(oe->make), "%s", make);
	if (model) snprintf(oe->model, sizeof(oe->model), "%s", model);
}

static void output_mode(void *data, struct wl_output *wl_output,
			uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
	(void)data; (void)wl_output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *wl_output)
{
	(void)data; (void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
	(void)wl_output;
	struct output_entry *oe = data;
	if (factor < 1) factor = 1;
	oe->scale = factor;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (wgt->used && wgt->visible && wgt->scale != factor)
			surface_preferred_scale(wgt, wgt->surface, factor);
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void surface_enter(void *data, struct wl_surface *surface,
			  struct wl_output *output)
{
	(void)surface;
	struct widget *wgt = data;
	if (!wgt)
		return;
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].output == output) {
			if (outputs[i].scale > 0 && outputs[i].scale != wgt->scale &&
			    wgt->applied_scale == wgt->scale) {
				surface_preferred_scale(wgt, surface, outputs[i].scale);
			}
			break;
		}
	}
}

static void surface_leave(void *data, struct wl_surface *surface,
			  struct wl_output *output)
{
	(void)data;
	(void)surface;
	(void)output;
}

static void surface_preferred_transform(void *data, struct wl_surface *surface,
					uint32_t transform)
{
	(void)data;
	(void)surface;
	(void)transform;
}

const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
	.preferred_buffer_scale = surface_preferred_scale,
	.preferred_buffer_transform = surface_preferred_transform,
};

static void shm_format(void *d, struct wl_shm *shm, uint32_t format)
{
	(void)d;
	(void)shm;
	if (format == WL_SHM_FORMAT_ARGB8888)
		wl.have_argb = true;
}

static const struct wl_shm_listener shm_listener = {shm_format};

static void registry_global(void *d, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	(void)d;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		wl.compositor = wl_registry_bind(registry, name,
						 &wl_compositor_interface,
						 version < 6 ? version : 6);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(wl.shm, &shm_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		wl.layer_shell = wl_registry_bind(
			registry, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, wl_pointer_interface.name) == 0) {
		wl.pointer = wl_registry_bind(registry, name, &wl_pointer_interface,
					 version < 5 ? version : 5);
		wl_pointer_add_listener(wl.pointer, &pointer_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		for (int i = 0; i < MAX_OUTPUTS; i++) {
			if (!outputs[i].valid) {
				outputs[i].valid = true;
				outputs[i].name = name;
				outputs[i].scale = 1;
				outputs[i].make[0] = '\0';
				outputs[i].model[0] = '\0';
				outputs[i].output = wl_registry_bind(
					registry, name, &wl_output_interface,
					version < 2 ? version : 2);
				wl_output_add_listener(outputs[i].output, &output_listener, &outputs[i]);
				if (!wl.output)
					wl.output = outputs[i].output;
				output_count++;
				break;
			}
		}
	}
}

static void registry_global_remove(void *d, struct wl_registry *registry, uint32_t name)
{
	(void)d;
	(void)registry;
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		if (outputs[i].valid && outputs[i].name == name) {
			if (wl.output == outputs[i].output) {
				wl.output = NULL;
				for (int j = 0; j < MAX_OUTPUTS; j++) {
					if (j != i && outputs[j].valid) {
						wl.output = outputs[j].output;
						break;
					}
				}
			}
			wl_output_destroy(outputs[i].output);
			outputs[i].valid = false;
			outputs[i].output = NULL;
			output_count--;
			break;
		}
	}
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
			  struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)pointer; (void)serial;
	wl.pointer_surface = surface;
	wl.ptr_x = (int32_t)(x >> 16);
	wl.ptr_y = (int32_t)(y >> 16);
	struct widget *wgt = NULL;
	if (surface) {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && widgets[k].surface == surface) {
				wgt = &widgets[k];
				break;
			}
		}
	}
	if (!wgt)
		return;
	widget_pointer(wgt, WISP_IN_ENTER, wl.ptr_x, wl.ptr_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
			  struct wl_surface *surface)
{
	(void)data; (void)pointer; (void)serial;
	struct widget *wgt = NULL;
	if (surface) {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && widgets[k].surface == surface) {
				wgt = &widgets[k];
				break;
			}
		}
	}
	if (wgt)
		widget_pointer(wgt, WISP_IN_LEAVE, wl.ptr_x, wl.ptr_y);
	wl.pointer_surface = NULL;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
	(void)data; (void)pointer; (void)serial; (void)time; (void)button;
	struct widget *wgt = NULL;
	if (wl.pointer_surface) {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && widgets[k].surface == wl.pointer_surface) {
				wgt = &widgets[k];
				break;
			}
		}
	}
	if (!wgt)
		return;
	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		widget_pointer(wgt, WISP_IN_RELEASE, wl.ptr_x, wl.ptr_y);
	else
		widget_pointer(wgt, WISP_IN_PRESS, wl.ptr_x, wl.ptr_y);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
			uint32_t axis, wl_fixed_t value)
{
	(void)data; (void)pointer; (void)time; (void)axis;
	struct widget *wgt = NULL;
	if (wl.pointer_surface) {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && widgets[k].surface == wl.pointer_surface) {
				wgt = &widgets[k];
				break;
			}
		}
	}
	if (wgt) {
		int dir = (value < 0) ? -1 : 1;
		widget_scroll(wgt, wl.ptr_x, wl.ptr_y, dir);
	}
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
			  wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)pointer; (void)time;
	wl.ptr_x = (int32_t)(x >> 16);
	wl.ptr_y = (int32_t)(y >> 16);
	struct widget *wgt = NULL;
	if (wl.pointer_surface) {
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && widgets[k].surface == wl.pointer_surface) {
				wgt = &widgets[k];
				break;
			}
		}
	}
	if (wgt)
		widget_pointer(wgt, WISP_IN_MOTION, wl.ptr_x, wl.ptr_y);
}

const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = NULL,
	.axis_source = NULL,
	.axis_stop = NULL,
	.axis_discrete = NULL,
	.axis_value120 = NULL,
	.axis_relative_direction = NULL,
	.warp = NULL,
};

const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};
