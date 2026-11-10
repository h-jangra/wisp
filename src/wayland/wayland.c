#include "wayland/wayland.h"
#include "core/types.h"
#include "core/widget.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

const struct wl_interface xdg_popup_interface = {"xdg_popup", 1, 0, NULL, 0, NULL};

struct wayland_context wl;
struct output_entry outputs[MAX_OUTPUTS];
int output_count = 0;

struct widget *widget_find_by_surface(struct wl_surface *surface)
{
	if (!surface)
		return NULL;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used && widgets[k].surface == surface)
			return &widgets[k];
	}
	return NULL;
}

static void configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
		      uint32_t width, uint32_t height)
{
	struct widget *wgt = data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);

	if (width == 0 || height == 0) {
		width = wgt->pend_w;
		height = wgt->pend_h;
	}
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

/* --- Pointer Events --- */

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
			  struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)pointer;
	wl.pointer_serial = serial;
	wl.current_cursor_shape = 0;
	wl.pointer_surface = surface;
	wl.ptr_x = (int32_t)wl_fixed_to_double(x);
	wl.ptr_y = (int32_t)wl_fixed_to_double(y);
	struct widget *wgt = widget_find_by_surface(surface);
	if (wgt)
		widget_pointer(wgt, WISP_IN_ENTER, 0, wl.ptr_x, wl.ptr_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
			  struct wl_surface *surface)
{
	(void)data; (void)pointer; (void)serial;
	wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	struct widget *wgt = widget_find_by_surface(surface);
	if (wgt)
		widget_pointer(wgt, WISP_IN_LEAVE, 0, wl.ptr_x, wl.ptr_y);
	if (wl.pointer_surface == surface)
		wl.pointer_surface = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
			   wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)pointer; (void)time;
	wl.ptr_x = (int32_t)wl_fixed_to_double(x);
	wl.ptr_y = (int32_t)wl_fixed_to_double(y);
	struct widget *wgt = widget_find_by_surface(wl.pointer_surface);
	if (wgt)
		widget_pointer(wgt, WISP_IN_MOTION, 0, wl.ptr_x, wl.ptr_y);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
	(void)data; (void)pointer; (void)time;
	wl.pointer_serial = serial;
	struct widget *wgt = widget_find_by_surface(wl.pointer_surface);
	int btn = WISP_BUTTON_LEFT;
	if (button == BTN_RIGHT)
		btn = WISP_BUTTON_RIGHT;
	else if (button == BTN_MIDDLE)
		btn = WISP_BUTTON_MIDDLE;

	int evt = (state == WL_POINTER_BUTTON_STATE_RELEASED) ? WISP_IN_RELEASE : WISP_IN_PRESS;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		wl.ptr_buttons |= (1u << (btn - 1));
	else
		wl.ptr_buttons &= ~(1u << (btn - 1));

	widget_pointer(wgt, evt, btn, wl.ptr_x, wl.ptr_y);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
			 uint32_t axis, wl_fixed_t value)
{
	(void)data; (void)pointer; (void)time;
	struct widget *wgt = widget_find_by_surface(wl.pointer_surface);
	if (wgt) {
		double d = wl_fixed_to_double(value);
		float dx = 0.0f, dy = 0.0f;
		if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
			dx = (float)d;
		else
			dy = (float)d;
		int dir = (d > 0.0) ? 1 : -1;
		widget_scroll(wgt, wl.ptr_x, wl.ptr_y, dir, dx, dy);
	}
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
	(void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
	(void)data; (void)pointer; (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
	(void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
	(void)data; (void)pointer; (void)axis; (void)discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
	(void)data; (void)pointer; (void)axis; (void)value120;
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis, uint32_t direction)
{
	(void)data; (void)pointer; (void)axis; (void)direction;
}

static void pointer_warp(void *data, struct wl_pointer *pointer, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	(void)data; (void)pointer; (void)surface_x; (void)surface_y;
}

const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
	.warp = pointer_warp,
};

/* --- Keyboard Events --- */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
			    uint32_t format, int32_t fd, uint32_t size)
{
	(void)data; (void)keyboard;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_str == MAP_FAILED) {
		close(fd);
		return;
	}
	if (wl.xkb_state) {
		xkb_state_unref(wl.xkb_state);
		wl.xkb_state = NULL;
	}
	if (wl.xkb_map) {
		xkb_keymap_unref(wl.xkb_map);
		wl.xkb_map = NULL;
	}
	if (!wl.xkb_ctx)
		wl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (wl.xkb_ctx) {
		wl.xkb_map = xkb_keymap_new_from_string(wl.xkb_ctx, map_str,
							XKB_KEYMAP_FORMAT_TEXT_V1,
							XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (wl.xkb_map)
			wl.xkb_state = xkb_state_new(wl.xkb_map);
	}
	munmap(map_str, size);
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
			   uint32_t serial, struct wl_surface *surface,
			   struct wl_array *keys)
{
	(void)data; (void)keyboard; (void)serial; (void)keys;
	wl.keyboard_surface = surface;
	struct widget *wgt = widget_find_by_surface(surface);
	if (wgt)
		widget_focus_event(wgt, true);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
			   uint32_t serial, struct wl_surface *surface)
{
	(void)data; (void)keyboard; (void)serial;
	struct widget *wgt = widget_find_by_surface(surface);
	if (wgt)
		widget_focus_event(wgt, false);
	if (wl.keyboard_surface == surface)
		wl.keyboard_surface = NULL;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
			       uint32_t serial, uint32_t mods_depressed,
			       uint32_t mods_latched, uint32_t mods_locked,
			       uint32_t group)
{
	(void)data; (void)keyboard; (void)serial;
	if (wl.xkb_state)
		xkb_state_update_mask(wl.xkb_state, mods_depressed,
				      mods_latched, mods_locked, 0, 0, group);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
			 uint32_t serial, uint32_t time, uint32_t key,
			 uint32_t state)
{
	(void)data; (void)keyboard; (void)serial; (void)time;
	if (!wl.keyboard_surface)
		return;
	struct widget *wgt = widget_find_by_surface(wl.keyboard_surface);
	if (!wgt)
		return;

	xkb_keycode_t keycode = key + 8;
	xkb_keysym_t sym = 0;
	uint32_t utf32 = 0;
	char utf8_buf[16] = {0};
	uint32_t mods = 0;

	if (wl.xkb_state) {
		sym = xkb_state_key_get_one_sym(wl.xkb_state, keycode);
		utf32 = xkb_state_key_get_utf32(wl.xkb_state, keycode);
		xkb_state_key_get_utf8(wl.xkb_state, keycode, utf8_buf, sizeof(utf8_buf));
		if (xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE))
			mods |= 1;
		if (xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))
			mods |= 2;
		if (xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE))
			mods |= 4;
	}

	bool is_press = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
	widget_key_event(wgt, keycode, sym, utf32, utf8_buf, mods, is_press);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
				 int32_t rate, int32_t delay)
{
	(void)data; (void)keyboard; (void)rate; (void)delay;
}

const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* --- Touch Events --- */

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial,
		       uint32_t time, struct wl_surface *surface, int32_t id,
		       wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)touch; (void)serial; (void)time; (void)id;
	wl.touch_surface = surface;
	int32_t px = (int32_t)wl_fixed_to_double(x);
	int32_t py = (int32_t)wl_fixed_to_double(y);
	wl.ptr_x = px;
	wl.ptr_y = py;
	struct widget *wgt = widget_find_by_surface(surface);
	if (wgt) {
		widget_pointer(wgt, WISP_IN_ENTER, 0, px, py);
		widget_pointer(wgt, WISP_IN_PRESS, WISP_BUTTON_LEFT, px, py);
	}
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial,
		     uint32_t time, int32_t id)
{
	(void)data; (void)touch; (void)serial; (void)time; (void)id;
	struct widget *wgt = widget_find_by_surface(wl.touch_surface);
	if (wgt) {
		widget_pointer(wgt, WISP_IN_RELEASE, WISP_BUTTON_LEFT, wl.ptr_x, wl.ptr_y);
		widget_pointer(wgt, WISP_IN_LEAVE, 0, wl.ptr_x, wl.ptr_y);
	}
	wl.touch_surface = NULL;
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time,
			 int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)touch; (void)time; (void)id;
	int32_t px = (int32_t)wl_fixed_to_double(x);
	int32_t py = (int32_t)wl_fixed_to_double(y);
	wl.ptr_x = px;
	wl.ptr_y = py;
	struct widget *wgt = widget_find_by_surface(wl.touch_surface);
	if (wgt)
		widget_pointer(wgt, WISP_IN_MOTION, 0, px, py);
}

static void touch_frame(void *data, struct wl_touch *touch)
{
	(void)data; (void)touch;
}

static void touch_cancel(void *data, struct wl_touch *touch)
{
	(void)data; (void)touch;
	struct widget *wgt = widget_find_by_surface(wl.touch_surface);
	if (wgt)
		widget_pointer(wgt, WISP_IN_LEAVE, 0, wl.ptr_x, wl.ptr_y);
	wl.touch_surface = NULL;
}

static void touch_shape(void *data, struct wl_touch *touch, int32_t id,
			wl_fixed_t major, wl_fixed_t minor)
{
	(void)data; (void)touch; (void)id; (void)major; (void)minor;
}

static void touch_orientation(void *data, struct wl_touch *touch, int32_t id,
			      wl_fixed_t orientation)
{
	(void)data; (void)touch; (void)id; (void)orientation;
}

const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
	.shape = touch_shape,
	.orientation = touch_orientation,
};

/* --- Seat Management --- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	(void)data;
	wl.seat_caps = caps;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl.pointer) {
		wl.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(wl.pointer, &pointer_listener, NULL);
		if (wl.cursor_shape_mgr)
			wl.cursor_shape_dev = wp_cursor_shape_manager_v1_get_pointer(wl.cursor_shape_mgr, wl.pointer);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl.pointer) {
		if (wl.cursor_shape_dev) {
			wp_cursor_shape_device_v1_destroy(wl.cursor_shape_dev);
			wl.cursor_shape_dev = NULL;
		}
		wl_pointer_release(wl.pointer);
		wl.pointer = NULL;
		wl.pointer_surface = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl.keyboard) {
		wl.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(wl.keyboard, &keyboard_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl.keyboard) {
		wl_keyboard_release(wl.keyboard);
		wl.keyboard = NULL;
		wl.keyboard_surface = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !wl.touch) {
		wl.touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(wl.touch, &touch_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && wl.touch) {
		wl_touch_release(wl.touch);
		wl.touch = NULL;
		wl.touch_surface = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data; (void)seat; (void)name;
}

const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* --- Registry --- */

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
		wl.layer_shell_version = version < 4 ? version : 4;
		wl.layer_shell = wl_registry_bind(
			registry, name, &zwlr_layer_shell_v1_interface, wl.layer_shell_version);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		uint32_t v = version < 2 ? version : 2;
		wl.cursor_shape_mgr = wl_registry_bind(
			registry, name, &wp_cursor_shape_manager_v1_interface, v);
		if (wl.pointer && !wl.cursor_shape_dev)
			wl.cursor_shape_dev = wp_cursor_shape_manager_v1_get_pointer(wl.cursor_shape_mgr, wl.pointer);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		wl.seat = wl_registry_bind(registry, name, &wl_seat_interface,
					   version < 7 ? version : 7);
		wl_seat_add_listener(wl.seat, &seat_listener, NULL);
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

const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

void wayland_set_cursor_shape(uint32_t shape)
{
	if (!wl.cursor_shape_dev || !wl.pointer_serial || shape == 0)
		return;
	if (wl.current_cursor_shape == shape)
		return;
	wl.current_cursor_shape = shape;
	wp_cursor_shape_device_v1_set_shape(wl.cursor_shape_dev, wl.pointer_serial, shape);
}

void wayland_input_shutdown(void)
{
	if (wl.cursor_shape_dev) {
		wp_cursor_shape_device_v1_destroy(wl.cursor_shape_dev);
		wl.cursor_shape_dev = NULL;
	}
	if (wl.cursor_shape_mgr) {
		wp_cursor_shape_manager_v1_destroy(wl.cursor_shape_mgr);
		wl.cursor_shape_mgr = NULL;
	}
	if (wl.pointer) {
		wl_pointer_release(wl.pointer);
		wl.pointer = NULL;
	}
	if (wl.keyboard) {
		wl_keyboard_release(wl.keyboard);
		wl.keyboard = NULL;
	}
	if (wl.touch) {
		wl_touch_release(wl.touch);
		wl.touch = NULL;
	}
	if (wl.seat) {
		wl_seat_destroy(wl.seat);
		wl.seat = NULL;
	}
	if (wl.xkb_state) {
		xkb_state_unref(wl.xkb_state);
		wl.xkb_state = NULL;
	}
	if (wl.xkb_map) {
		xkb_keymap_unref(wl.xkb_map);
		wl.xkb_map = NULL;
	}
	if (wl.xkb_ctx) {
		xkb_context_unref(wl.xkb_ctx);
		wl.xkb_ctx = NULL;
	}
}
