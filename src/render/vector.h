#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <lua/lua.h>
#include "core/types.h"

enum vg_point_op {
	VG_OP_MOVE_TO = 0,
	VG_OP_LINE_TO,
	VG_OP_CLOSE,
};

struct vg_point {
	float x, y;
	uint8_t op;
};

struct widget;
struct custom_render_payload;
struct screen_image;

void wisp_vg_init(lua_State *L);
void wisp_vg_shutdown(lua_State *L);

void wisp_vg_render_canvas(struct widget *wgt, struct custom_render_payload *p,
                           float bx, float by, float bw, float bh);
void wisp_vg_render_canvas_nanovg(void *vg, struct widget *wgt, struct custom_render_payload *p,
                                  float bx, float by, float bw, float bh);
void wisp_vg_render_surface(struct widget *wgt);
void wisp_vg_render_surface_nanovg(void *vg, struct widget *wgt);
bool wisp_vg_render_image(struct screen_image *img, lua_State *L, int func_idx);
