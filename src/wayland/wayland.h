#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "zwlr_layer_shell_v1.h"

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
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_output *output;
	struct wl_pointer *pointer;
	struct wl_surface *pointer_surface;
	int32_t ptr_x, ptr_y;
	bool have_argb;
};

extern struct wayland_context wl;
extern struct output_entry outputs[MAX_OUTPUTS];
extern int output_count;

void surface_preferred_scale(void *data, struct wl_surface *surface, int32_t factor);
extern const struct wl_surface_listener surface_listener;
extern const struct zwlr_layer_surface_v1_listener layer_surface_listener;
extern const struct wl_registry_listener registry_listener;
extern const struct wl_interface xdg_popup_interface;
extern const struct wl_pointer_listener pointer_listener;
