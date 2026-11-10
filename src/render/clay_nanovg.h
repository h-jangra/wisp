#pragma once
#include "nanovg.h"
#include "layout/clay_layout.h"

struct widget;

enum custom_render_type {
	CUSTOM_RENDER_NONE = 0,
	CUSTOM_RENDER_GRADIENT,
	CUSTOM_RENDER_SHADOW,
	CUSTOM_RENDER_BUTTON,
	CUSTOM_RENDER_SWITCH,
	CUSTOM_RENDER_CHECKBOX,
	CUSTOM_RENDER_SLIDER,
	CUSTOM_RENDER_PROGRESS,
	CUSTOM_RENDER_ICON,
	CUSTOM_RENDER_INPUT,
	CUSTOM_RENDER_SCROLLBAR,
};

struct custom_render_payload {
	enum custom_render_type type;
	uint32_t color1;
	uint32_t color2;
	uint32_t color3;
	float radius;
	float shadow_blur;
	float shadow_x;
	float shadow_y;
	uint32_t shadow_color;
	float scale;
	float rotation;
	float opacity;
	float value;       /* 0..1 for progress, slider, switch, checkbox */
	float min_val;
	float max_val;
	bool is_radial;    /* for gradient */
	bool is_focused;   /* for input */
	char text[128];    /* for icon name or input text/placeholder */
};

struct custom_render_payload *custom_payload_alloc(void);
void custom_payload_reset(void);
void clay_render_nanovg(NVGcontext *vg, struct widget *wgt, Clay_RenderCommandArray *commands);
void nanovg_image_cache_clear(NVGcontext *vg);
