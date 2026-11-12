#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "zwlr_layer_shell_v1.h"
#include "cursor_shape_v1.h"
#include "xdg_shell.h"
#include "zwlr_data_control_v1.h"
#include "zwlr_screencopy_v1.h"

#define MAX_OUTPUTS 8

struct output_entry {
	uint32_t name;
	struct wl_output *output;
	int32_t scale;
	int32_t width;
	int32_t height;
	char make[32];
	char model[32];
	bool valid;
};

struct wayland_context {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct xdg_wm_base *wm_base;
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
	uint32_t last_serial;

	/* Data device / Clipboard */
	struct wl_data_device_manager *data_device_mgr;
	struct wl_data_device *data_device;
	struct zwlr_data_control_manager_v1 *data_control_mgr;
	struct zwlr_data_control_device_v1 *data_control_dev;

	/* Screencopy */
	struct zwlr_screencopy_manager_v1 *screencopy_mgr;
	uint32_t screencopy_version;
	bool has_ext_image_copy;

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
void wayland_keyboard_repeat_tick(int64_t now_us);
int wayland_keyboard_repeat_next_ms(int64_t now_us);
bool wayland_key_is_down(const char *name);
uint32_t wayland_get_modifiers(void);
void wayland_set_key_down_for_test(uint32_t key, xkb_keysym_t sym, bool is_press, uint32_t mods);

void surface_preferred_scale(void *data, struct wl_surface *surface, int32_t factor);
extern const struct wl_surface_listener surface_listener;
extern const struct zwlr_layer_surface_v1_listener layer_surface_listener;
extern const struct xdg_wm_base_listener wm_base_listener;
extern const struct xdg_surface_listener xdg_surface_listener;
extern const struct xdg_toplevel_listener xdg_toplevel_listener;
extern const struct wl_registry_listener registry_listener;
extern const struct wl_pointer_listener pointer_listener;
extern const struct wl_keyboard_listener keyboard_listener;
extern const struct wl_touch_listener touch_listener;
extern const struct wl_seat_listener seat_listener;
