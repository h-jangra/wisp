#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "zwlr_layer_shell_v1.h"
#include "cursor_shape_v1.h"

#define MAX_OUTPUTS 8

struct output_entry {
	uint32_t name;
	struct wl_output *output;
	int32_t scale;
	char make[32];
	char model[32];
	bool valid;
};

struct wayland_context {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_output *output;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_touch *touch;
	struct wl_surface *pointer_surface;
	struct wl_surface *keyboard_surface;
	struct wl_surface *touch_surface;
	int32_t ptr_x, ptr_y;
	uint32_t ptr_buttons;
	bool have_argb;
	uint32_t seat_caps;
	uint32_t layer_shell_version;

	/* Cursor shape */
	struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wp_cursor_shape_device_v1 *cursor_shape_dev;
	uint32_t pointer_serial;
	uint32_t current_cursor_shape;

	/* XKB */
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_map;
	struct xkb_state *xkb_state;
};

extern struct wayland_context wl;
extern struct output_entry outputs[MAX_OUTPUTS];
extern int output_count;

struct widget *widget_find_by_surface(struct wl_surface *surface);
void wayland_input_shutdown(void);
void wayland_set_cursor_shape(uint32_t shape);

void surface_preferred_scale(void *data, struct wl_surface *surface, int32_t factor);
extern const struct wl_surface_listener surface_listener;
extern const struct zwlr_layer_surface_v1_listener layer_surface_listener;
extern const struct wl_registry_listener registry_listener;
extern const struct wl_interface xdg_popup_interface;
extern const struct wl_pointer_listener pointer_listener;
extern const struct wl_keyboard_listener keyboard_listener;
extern const struct wl_touch_listener touch_listener;
extern const struct wl_seat_listener seat_listener;
