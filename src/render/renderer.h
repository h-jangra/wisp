#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct widget;

struct wisp_renderer {
	const char *name;
	bool (*init)(struct wl_display *display);
	void (*shutdown)(void);
	bool (*surface_create)(struct widget *wgt, uint32_t width, uint32_t height);
	void (*surface_destroy)(struct widget *wgt);
	void (*surface_resize)(struct widget *wgt, uint32_t width, uint32_t height);
	bool (*begin_frame)(struct widget *wgt);
	void (*end_frame)(struct widget *wgt);
	void *(*get_vg)(void);
};

extern const struct wisp_renderer native_renderer;
extern const struct wisp_renderer egl_renderer;
extern const struct wisp_renderer *g_renderer;

bool renderer_init(struct wl_display *display);
void renderer_shutdown(void);
void renderer_set_backend(const struct wisp_renderer *backend);
void *renderer_get_vg(void);
