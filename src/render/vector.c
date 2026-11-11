#include "render/vector.h"
#include "render/renderer_native.h"
#include "render/clay_nanovg.h"
#include "scripting/scripting.h"
#include "core/widget.h"
#include "core/interaction.h"
#include "services/screen.h"
#include "nanovg.h"
#include <lua/lauxlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VG_MAX_STATES 16
#define VG_MAX_PATH_POINTS 2048
#define M_PI_F 3.14159265358979323846f

struct vg_state {
	float a, b, c, d, tx, ty;
	int clip_x0, clip_y0, clip_x1, clip_y1;
	bool has_clip;
	uint32_t fill_color;
	uint32_t stroke_color;
	float stroke_width;
	bool has_fill;
	bool has_stroke;
};

struct wisp_vg_ctx {
	struct widget *wgt;
	uint32_t *pixels;
	uint32_t sw, sh;
	float scale;
	float canvas_x, canvas_y, canvas_w, canvas_h;

	struct vg_state state_stack[VG_MAX_STATES];
	int state_sp;

	struct vg_point path[VG_MAX_PATH_POINTS];
	int path_len;
	float cur_x, cur_y;
	float start_x, start_y;

	NVGcontext *nvg;
};

struct wisp_cached_path {
	struct vg_point *points;
	int count;
};

static struct wisp_vg_ctx g_vg_ctx;

static inline void vg_transform_point(struct wisp_vg_ctx *vg, float x, float y, float *ox, float *oy)
{
	struct vg_state *st = &vg->state_stack[vg->state_sp];
	*ox = st->a * x + st->c * y + st->tx;
	*oy = st->b * x + st->d * y + st->ty;
}

static inline float vg_get_scale(struct wisp_vg_ctx *vg)
{
	struct vg_state *st = &vg->state_stack[vg->state_sp];
	float det = fabsf(st->a * st->d - st->b * st->c);
	return sqrtf(det > 0.0f ? det : 1.0f);
}

static inline bool vg_is_unrotated(struct wisp_vg_ctx *vg)
{
	struct vg_state *st = &vg->state_stack[vg->state_sp];
	return (fabsf(st->b) < 1e-5f && fabsf(st->c) < 1e-5f && st->a > 0.0f && st->d > 0.0f);
}

static void vg_add_point(struct wisp_vg_ctx *vg, float x, float y, uint8_t op)
{
	if (vg->path_len >= VG_MAX_PATH_POINTS)
		return;
	vg->path[vg->path_len].x = x;
	vg->path[vg->path_len].y = y;
	vg->path[vg->path_len].op = op;
	vg->path_len++;
}

static void vg_subdivide_quad(struct wisp_vg_ctx *vg, float x0, float y0, float cx, float cy, float x1, float y1)
{
	const int steps = 12;
	float dt = 1.0f / (float)steps;
	for (int i = 1; i <= steps; i++) {
		float t = (float)i * dt;
		float it = 1.0f - t;
		float px = it * it * x0 + 2.0f * it * t * cx + t * t * x1;
		float py = it * it * y0 + 2.0f * it * t * cy + t * t * y1;
		vg_add_point(vg, px, py, VG_OP_LINE_TO);
	}
}

static void vg_subdivide_cubic(struct wisp_vg_ctx *vg, float x0, float y0,
                               float c1x, float c1y, float c2x, float c2y,
                               float x1, float y1)
{
	const int steps = 16;
	float dt = 1.0f / (float)steps;
	for (int i = 1; i <= steps; i++) {
		float t = (float)i * dt;
		float it = 1.0f - t;
		float px = it * it * it * x0 + 3.0f * it * it * t * c1x + 3.0f * it * t * t * c2x + t * t * t * x1;
		float py = it * it * it * y0 + 3.0f * it * it * t * c1y + 3.0f * it * t * t * c2y + t * t * t * y1;
		vg_add_point(vg, px, py, VG_OP_LINE_TO);
	}
}

/* --- Path methods (p:*) --- */
static int path_move_to(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	float tx, ty;
	vg_transform_point(vg, x, y, &tx, &ty);
	vg_add_point(vg, tx, ty, VG_OP_MOVE_TO);
	vg->cur_x = tx; vg->cur_y = ty;
	vg->start_x = tx; vg->start_y = ty;
	if (vg->nvg)
		nvgMoveTo(vg->nvg, x, y);
	return 0;
}

static int path_line_to(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	float tx, ty;
	vg_transform_point(vg, x, y, &tx, &ty);
	if (vg->path_len == 0) {
		vg_add_point(vg, tx, ty, VG_OP_MOVE_TO);
		vg->start_x = tx; vg->start_y = ty;
	} else {
		vg_add_point(vg, tx, ty, VG_OP_LINE_TO);
	}
	vg->cur_x = tx; vg->cur_y = ty;
	if (vg->nvg)
		nvgLineTo(vg->nvg, x, y);
	return 0;
}

static int path_quad_to(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float cx = (float)luaL_optnumber(L, 2, 0.0);
	float cy = (float)luaL_optnumber(L, 3, 0.0);
	float x = (float)luaL_optnumber(L, 4, 0.0);
	float y = (float)luaL_optnumber(L, 5, 0.0);
	float tcx, tcy, tx, ty;
	vg_transform_point(vg, cx, cy, &tcx, &tcy);
	vg_transform_point(vg, x, y, &tx, &ty);
	if (vg->path_len == 0) {
		vg_add_point(vg, vg->cur_x, vg->cur_y, VG_OP_MOVE_TO);
		vg->start_x = vg->cur_x; vg->start_y = vg->cur_y;
	}
	vg_subdivide_quad(vg, vg->cur_x, vg->cur_y, tcx, tcy, tx, ty);
	vg->cur_x = tx; vg->cur_y = ty;
	if (vg->nvg)
		nvgQuadTo(vg->nvg, cx, cy, x, y);
	return 0;
}

static int path_bezier_to(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float c1x = (float)luaL_optnumber(L, 2, 0.0);
	float c1y = (float)luaL_optnumber(L, 3, 0.0);
	float c2x = (float)luaL_optnumber(L, 4, 0.0);
	float c2y = (float)luaL_optnumber(L, 5, 0.0);
	float x = (float)luaL_optnumber(L, 6, 0.0);
	float y = (float)luaL_optnumber(L, 7, 0.0);
	float tc1x, tc1y, tc2x, tc2y, tx, ty;
	vg_transform_point(vg, c1x, c1y, &tc1x, &tc1y);
	vg_transform_point(vg, c2x, c2y, &tc2x, &tc2y);
	vg_transform_point(vg, x, y, &tx, &ty);
	if (vg->path_len == 0) {
		vg_add_point(vg, vg->cur_x, vg->cur_y, VG_OP_MOVE_TO);
		vg->start_x = vg->cur_x; vg->start_y = vg->cur_y;
	}
	vg_subdivide_cubic(vg, vg->cur_x, vg->cur_y, tc1x, tc1y, tc2x, tc2y, tx, ty);
	vg->cur_x = tx; vg->cur_y = ty;
	if (vg->nvg)
		nvgBezierTo(vg->nvg, c1x, c1y, c2x, c2y, x, y);
	return 0;
}

static int path_close(lua_State *L)
{
	(void)L;
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	if (vg->path_len > 0) {
		if (vg->cur_x != vg->start_x || vg->cur_y != vg->start_y)
			vg_add_point(vg, vg->start_x, vg->start_y, VG_OP_LINE_TO);
		vg_add_point(vg, vg->start_x, vg->start_y, VG_OP_CLOSE);
		vg->cur_x = vg->start_x; vg->cur_y = vg->start_y;
	}
	if (vg->nvg)
		nvgClosePath(vg->nvg);
	return 0;
}

static int path_fill(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	uint32_t col = parse_color_value(L, 2);
	vg->state_stack[vg->state_sp].fill_color = col;
	vg->state_stack[vg->state_sp].has_fill = true;
	return 0;
}

static int path_stroke(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	uint32_t col = parse_color_value(L, 2);
	float w = (float)luaL_optnumber(L, 3, 1.0);
	vg->state_stack[vg->state_sp].stroke_color = col;
	vg->state_stack[vg->state_sp].stroke_width = w;
	vg->state_stack[vg->state_sp].has_stroke = true;
	return 0;
}

static const luaL_Reg path_methods[] = {
	{"move_to", path_move_to},
	{"line_to", path_line_to},
	{"quad_to", path_quad_to},
	{"quadratic_to", path_quad_to},
	{"bezier_to", path_bezier_to},
	{"cubic_to", path_bezier_to},
	{"close", path_close},
	{"fill", path_fill},
	{"stroke", path_stroke},
	{NULL, NULL},
};

/* --- Graphics context methods (g:*) --- */
static int vg_clear(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	uint32_t col = parse_color_value(L, 2);
	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgRect(vg->nvg, 0, 0, vg->canvas_w, vg->canvas_h);
		nvgFillColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
		nvgFill(vg->nvg);
	} else {
		renderer_rect(vg->canvas_x, vg->canvas_y, vg->canvas_w, vg->canvas_h, col);
	}
	return 0;
}

static int vg_rect(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	float w = (float)luaL_optnumber(L, 4, 0.0);
	float h = (float)luaL_optnumber(L, 5, 0.0);
	uint32_t col = parse_color_value(L, 6);
	if (w <= 0.0f || h <= 0.0f || (col >> 24) == 0)
		return 0;

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgRect(vg->nvg, x, y, w, h);
		nvgFillColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
		nvgFill(vg->nvg);
		return 0;
	}

	if (vg_is_unrotated(vg)) {
		float tx, ty;
		vg_transform_point(vg, x, y, &tx, &ty);
		float sc = vg_get_scale(vg);
		renderer_rect(tx, ty, w * sc, h * sc, col);
	} else {
		float pts[8];
		vg_transform_point(vg, x, y, &pts[0], &pts[1]);
		vg_transform_point(vg, x + w, y, &pts[2], &pts[3]);
		vg_transform_point(vg, x + w, y + h, &pts[4], &pts[5]);
		vg_transform_point(vg, x, y + h, &pts[6], &pts[7]);
		renderer_polygon(pts, 4, col);
	}
	return 0;
}

static int vg_round_rect(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	float w = (float)luaL_optnumber(L, 4, 0.0);
	float h = (float)luaL_optnumber(L, 5, 0.0);
	float r = (float)luaL_optnumber(L, 6, 0.0);
	uint32_t col = parse_color_value(L, 7);
	if (w <= 0.0f || h <= 0.0f || (col >> 24) == 0)
		return 0;

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgRoundedRect(vg->nvg, x, y, w, h, r);
		nvgFillColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
		nvgFill(vg->nvg);
		return 0;
	}

	if (vg_is_unrotated(vg)) {
		float tx, ty;
		vg_transform_point(vg, x, y, &tx, &ty);
		float sc = vg_get_scale(vg);
		renderer_round_rect(tx, ty, w * sc, h * sc, r * sc, col);
	} else {
		/* Rotated round rect via polygon approximation */
		struct vg_point pts[32];
		int n = 0;
		const int steps = 4;
		float corners[4][2] = { {x + w - r, y + r}, {x + w - r, y + h - r}, {x + r, y + h - r}, {x + r, y + r} };
		float angles[4] = { -M_PI_F * 0.5f, 0.0f, M_PI_F * 0.5f, M_PI_F };
		for (int c = 0; c < 4; c++) {
			for (int s = 0; s < steps; s++) {
				float a = angles[c] + ((float)s / (float)steps) * (M_PI_F * 0.5f);
				float px = corners[c][0] + cosf(a) * r;
				float py = corners[c][1] + sinf(a) * r;
				vg_transform_point(vg, px, py, &pts[n].x, &pts[n].y);
				pts[n].op = (n == 0) ? VG_OP_MOVE_TO : VG_OP_LINE_TO;
				n++;
			}
		}
		renderer_path_fill(pts, n, col);
	}
	return 0;
}

static int vg_rect_outline(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	float w = (float)luaL_optnumber(L, 4, 0.0);
	float h = (float)luaL_optnumber(L, 5, 0.0);
	uint32_t col = parse_color_value(L, 6);
	float bw = (float)luaL_optnumber(L, 7, 1.0);
	if (w <= 0.0f || h <= 0.0f || (col >> 24) == 0 || bw <= 0.0f)
		return 0;

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgRect(vg->nvg, x + bw * 0.5f, y + bw * 0.5f, w - bw, h - bw);
		nvgStrokeWidth(vg->nvg, bw);
		nvgStrokeColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
		nvgStroke(vg->nvg);
		return 0;
	}

	if (vg_is_unrotated(vg)) {
		float tx, ty;
		vg_transform_point(vg, x, y, &tx, &ty);
		float sc = vg_get_scale(vg);
		renderer_border(tx, ty, w * sc, h * sc, 0.0f, bw * sc, bw * sc, bw * sc, bw * sc, col);
	} else {
		float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
		vg_transform_point(vg, x, y, &p0x, &p0y);
		vg_transform_point(vg, x + w, y, &p1x, &p1y);
		vg_transform_point(vg, x + w, y + h, &p2x, &p2y);
		vg_transform_point(vg, x, y + h, &p3x, &p3y);
		float sc = vg_get_scale(vg);
		renderer_line(p0x, p0y, p1x, p1y, bw * sc, col);
		renderer_line(p1x, p1y, p2x, p2y, bw * sc, col);
		renderer_line(p2x, p2y, p3x, p3y, bw * sc, col);
		renderer_line(p3x, p3y, p0x, p0y, bw * sc, col);
	}
	return 0;
}

static int vg_circle(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float cx = (float)luaL_optnumber(L, 2, 0.0);
	float cy = (float)luaL_optnumber(L, 3, 0.0);
	float r = (float)luaL_optnumber(L, 4, 0.0);
	uint32_t col = parse_color_value(L, 5);
	float bw = (float)luaL_optnumber(L, 6, 0.0);
	if (r <= 0.0f || (col >> 24) == 0)
		return 0;

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgCircle(vg->nvg, cx, cy, r);
		if (bw > 0.0f) {
			nvgStrokeWidth(vg->nvg, bw);
			nvgStrokeColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
			nvgStroke(vg->nvg);
		} else {
			nvgFillColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
			nvgFill(vg->nvg);
		}
		return 0;
	}

	float tcx, tcy;
	vg_transform_point(vg, cx, cy, &tcx, &tcy);
	float sc = vg_get_scale(vg);

	if (bw <= 0.0f) {
		renderer_circle(tcx, tcy, r * sc, col);
	} else {
		renderer_border(tcx - r * sc, tcy - r * sc, r * 2.0f * sc, r * 2.0f * sc,
		                r * sc, bw * sc, bw * sc, bw * sc, bw * sc, col);
	}
	return 0;
}

static int vg_ellipse(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float cx = (float)luaL_optnumber(L, 2, 0.0);
	float cy = (float)luaL_optnumber(L, 3, 0.0);
	float rx = (float)luaL_optnumber(L, 4, 0.0);
	float ry = (float)luaL_optnumber(L, 5, 0.0);
	uint32_t col = parse_color_value(L, 6);
	float bw = (float)luaL_optnumber(L, 7, 0.0);
	if (rx <= 0.0f || ry <= 0.0f || (col >> 24) == 0)
		return 0;

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgEllipse(vg->nvg, cx, cy, rx, ry);
		if (bw > 0.0f) {
			nvgStrokeWidth(vg->nvg, bw);
			nvgStrokeColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
			nvgStroke(vg->nvg);
		} else {
			nvgFillColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
			nvgFill(vg->nvg);
		}
		return 0;
	}

	const int steps = 32;
	struct vg_point pts[32];
	for (int i = 0; i < steps; i++) {
		float a = (float)i * (2.0f * M_PI_F / (float)steps);
		float px = cx + cosf(a) * rx;
		float py = cy + sinf(a) * ry;
		vg_transform_point(vg, px, py, &pts[i].x, &pts[i].y);
		pts[i].op = (i == 0) ? VG_OP_MOVE_TO : VG_OP_LINE_TO;
	}

	if (bw <= 0.0f) {
		renderer_path_fill(pts, steps, col);
	} else {
		float sc = vg_get_scale(vg);
		for (int i = 0; i < steps; i++) {
			int next = (i + 1 < steps) ? i + 1 : 0;
			renderer_line(pts[i].x, pts[i].y, pts[next].x, pts[next].y, bw * sc, col);
		}
	}
	return 0;
}

static int vg_line(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x1 = (float)luaL_optnumber(L, 2, 0.0);
	float y1 = (float)luaL_optnumber(L, 3, 0.0);
	float x2 = (float)luaL_optnumber(L, 4, 0.0);
	float y2 = (float)luaL_optnumber(L, 5, 0.0);
	uint32_t col = parse_color_value(L, 6);
	float w = (float)luaL_optnumber(L, 7, 1.0);
	if ((col >> 24) == 0 || w <= 0.0f)
		return 0;

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgMoveTo(vg->nvg, x1, y1);
		nvgLineTo(vg->nvg, x2, y2);
		nvgStrokeWidth(vg->nvg, w);
		nvgStrokeColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
		nvgStroke(vg->nvg);
		return 0;
	}

	float tx1, ty1, tx2, ty2;
	vg_transform_point(vg, x1, y1, &tx1, &ty1);
	vg_transform_point(vg, x2, y2, &tx2, &ty2);
	float sc = vg_get_scale(vg);
	renderer_line(tx1, ty1, tx2, ty2, w * sc, col);
	return 0;
}

static int vg_polyline(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	if (!lua_istable(L, 2))
		return 0;
	uint32_t col = parse_color_value(L, 3);
	float w = (float)luaL_optnumber(L, 4, 1.0);
	if ((col >> 24) == 0 || w <= 0.0f)
		return 0;

	int len = (int)lua_rawlen(L, 2);
	if (len < 2)
		return 0;

	lua_rawgeti(L, 2, 1);
	bool nested = lua_istable(L, -1);
	lua_pop(L, 1);

	float prev_x = 0, prev_y = 0;
	float sc = vg_get_scale(vg);

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgStrokeWidth(vg->nvg, w);
		nvgStrokeColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
	}

	if (nested) {
		for (int i = 1; i <= len; i++) {
			lua_rawgeti(L, 2, i);
			lua_rawgeti(L, -1, 1);
			float x = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);
			lua_rawgeti(L, -1, 2);
			float y = (float)lua_tonumber(L, -1);
			lua_pop(L, 2);

			if (vg->nvg) {
				if (i == 1) nvgMoveTo(vg->nvg, x, y);
				else nvgLineTo(vg->nvg, x, y);
			} else {
				float tx, ty;
				vg_transform_point(vg, x, y, &tx, &ty);
				if (i > 1)
					renderer_line(prev_x, prev_y, tx, ty, w * sc, col);
				prev_x = tx; prev_y = ty;
			}
		}
	} else {
		for (int i = 1; i + 1 <= len; i += 2) {
			lua_rawgeti(L, 2, i);
			float x = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);
			lua_rawgeti(L, 2, i + 1);
			float y = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);

			if (vg->nvg) {
				if (i == 1) nvgMoveTo(vg->nvg, x, y);
				else nvgLineTo(vg->nvg, x, y);
			} else {
				float tx, ty;
				vg_transform_point(vg, x, y, &tx, &ty);
				if (i > 1)
					renderer_line(prev_x, prev_y, tx, ty, w * sc, col);
				prev_x = tx; prev_y = ty;
			}
		}
	}

	if (vg->nvg)
		nvgStroke(vg->nvg);
	return 0;
}

static int vg_polygon(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	if (!lua_istable(L, 2))
		return 0;
	uint32_t col = parse_color_value(L, 3);
	bool fill = true;
	if (lua_isboolean(L, 4))
		fill = lua_toboolean(L, 4);

	int len = (int)lua_rawlen(L, 2);
	if (len < 3)
		return 0;

	lua_rawgeti(L, 2, 1);
	bool nested = lua_istable(L, -1);
	lua_pop(L, 1);

	/* shortcut: stack buffer up to 512 vertices, upgrade to dynamic if required */
	struct vg_point pts[512];
	int count = 0;

	if (nested) {
		count = len < 512 ? len : 512;
		for (int i = 1; i <= count; i++) {
			lua_rawgeti(L, 2, i);
			lua_rawgeti(L, -1, 1);
			float x = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);
			lua_rawgeti(L, -1, 2);
			float y = (float)lua_tonumber(L, -1);
			lua_pop(L, 2);
			vg_transform_point(vg, x, y, &pts[i - 1].x, &pts[i - 1].y);
			pts[i - 1].op = (i == 1) ? VG_OP_MOVE_TO : VG_OP_LINE_TO;
		}
	} else {
		count = (len / 2) < 512 ? (len / 2) : 512;
		for (int i = 0; i < count; i++) {
			lua_rawgeti(L, 2, i * 2 + 1);
			float x = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);
			lua_rawgeti(L, 2, i * 2 + 2);
			float y = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);
			vg_transform_point(vg, x, y, &pts[i].x, &pts[i].y);
			pts[i].op = (i == 0) ? VG_OP_MOVE_TO : VG_OP_LINE_TO;
		}
	}

	if (count < 3)
		return 0;

	if (vg->nvg) {
		nvgBeginPath(vg->nvg);
		nvgMoveTo(vg->nvg, pts[0].x, pts[0].y);
		for (int i = 1; i < count; i++)
			nvgLineTo(vg->nvg, pts[i].x, pts[i].y);
		nvgClosePath(vg->nvg);
		if (fill) {
			nvgFillColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
			nvgFill(vg->nvg);
		} else {
			nvgStrokeWidth(vg->nvg, 1.0f);
			nvgStrokeColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
			nvgStroke(vg->nvg);
		}
		return 0;
	}

	if (fill) {
		renderer_path_fill(pts, count, col);
	} else {
		float sc = vg_get_scale(vg);
		for (int i = 0; i < count; i++) {
			int next = (i + 1 < count) ? i + 1 : 0;
			renderer_line(pts[i].x, pts[i].y, pts[next].x, pts[next].y, 1.0f * sc, col);
		}
	}
	return 0;
}

static int vg_text(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	const char *str = luaL_optstring(L, 4, "");
	uint32_t col = parse_color_value(L, 5);
	float size = (float)luaL_optnumber(L, 6, 14.0);
	if (!str || !*str || (col >> 24) == 0)
		return 0;

	if (vg->nvg) {
		nvgFontSize(vg->nvg, size);
		nvgFontFace(vg->nvg, "default");
		nvgFillColor(vg->nvg, nvgRGBA((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF));
		nvgText(vg->nvg, x, y + size, str, NULL);
		return 0;
	}

	float tx, ty;
	vg_transform_point(vg, x, y, &tx, &ty);
	float sc = vg_get_scale(vg);
	renderer_text(str, (int)strlen(str), tx, ty, size * sc, col);
	return 0;
}

static int vg_clip(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	float w = (float)luaL_optnumber(L, 4, 0.0);
	float h = (float)luaL_optnumber(L, 5, 0.0);

	if (vg->nvg) {
		nvgIntersectScissor(vg->nvg, x, y, w, h);
		return 0;
	}

	float tx0, ty0, tx1, ty1;
	vg_transform_point(vg, x, y, &tx0, &ty0);
	vg_transform_point(vg, x + w, y + h, &tx1, &ty1);
	float min_x = tx0 < tx1 ? tx0 : tx1;
	float max_x = tx0 > tx1 ? tx0 : tx1;
	float min_y = ty0 < ty1 ? ty0 : ty1;
	float max_y = ty0 > ty1 ? ty0 : ty1;

	renderer_clip(min_x, min_y, max_x - min_x, max_y - min_y);
	vg->state_stack[vg->state_sp].has_clip = true;
	return 0;
}

static int vg_reset_clip(lua_State *L)
{
	(void)L;
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	if (vg->nvg) {
		nvgResetScissor(vg->nvg);
	} else {
		renderer_clip_end();
		vg->state_stack[vg->state_sp].has_clip = false;
	}
	return 0;
}

static int vg_translate(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float dx = (float)luaL_optnumber(L, 2, 0.0);
	float dy = (float)luaL_optnumber(L, 3, 0.0);
	struct vg_state *st = &vg->state_stack[vg->state_sp];
	st->tx = st->a * dx + st->c * dy + st->tx;
	st->ty = st->b * dx + st->d * dy + st->ty;
	if (vg->nvg)
		nvgTranslate(vg->nvg, dx, dy);
	return 0;
}

static int vg_scale(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float sx = (float)luaL_optnumber(L, 2, 1.0);
	float sy = (float)luaL_optnumber(L, 3, sx);
	struct vg_state *st = &vg->state_stack[vg->state_sp];
	st->a *= sx; st->b *= sx;
	st->c *= sy; st->d *= sy;
	if (vg->nvg)
		nvgScale(vg->nvg, sx, sy);
	return 0;
}

static int vg_rotate(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float angle = (float)luaL_optnumber(L, 2, 0.0);
	float cos_a = cosf(angle);
	float sin_a = sinf(angle);
	struct vg_state *st = &vg->state_stack[vg->state_sp];
	float a = st->a, b = st->b, c = st->c, d = st->d;
	st->a = a * cos_a + c * sin_a;
	st->b = b * cos_a + d * sin_a;
	st->c = -a * sin_a + c * cos_a;
	st->d = -b * sin_a + d * cos_a;
	if (vg->nvg)
		nvgRotate(vg->nvg, angle);
	return 0;
}

static int vg_save(lua_State *L)
{
	(void)L;
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	if (vg->state_sp < VG_MAX_STATES - 1) {
		vg->state_stack[vg->state_sp + 1] = vg->state_stack[vg->state_sp];
		vg->state_sp++;
	}
	if (vg->nvg)
		nvgSave(vg->nvg);
	return 0;
}

static int vg_restore(lua_State *L)
{
	(void)L;
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	if (vg->state_sp > 0) {
		if (vg->state_stack[vg->state_sp].has_clip)
			renderer_clip_end();
		vg->state_sp--;
	}
	if (vg->nvg)
		nvgRestore(vg->nvg);
	return 0;
}

static int vg_gradient(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 2, 0.0);
	float y = (float)luaL_optnumber(L, 3, 0.0);
	float w = (float)luaL_optnumber(L, 4, 0.0);
	float h = (float)luaL_optnumber(L, 5, 0.0);
	uint32_t c1 = parse_color_value(L, 6);
	uint32_t c2 = parse_color_value(L, 7);
	bool radial = lua_toboolean(L, 8);
	float r = (float)luaL_optnumber(L, 9, 0.0);

	float tx, ty;
	vg_transform_point(vg, x, y, &tx, &ty);
	float sc = vg_get_scale(vg);
	renderer_gradient(tx, ty, w * sc, h * sc, r * sc, c1, c2, radial);
	return 0;
}

static void vg_render_path_internal(struct wisp_vg_ctx *vg, const struct vg_point *pts, int count,
                                    bool do_fill, uint32_t fill_col,
                                    bool do_stroke, uint32_t stroke_col, float stroke_w)
{
	if (count <= 0)
		return;

	if (do_fill && (fill_col >> 24) != 0)
		renderer_path_fill(pts, count, fill_col);

	if (do_stroke && (stroke_col >> 24) != 0 && stroke_w > 0.0f) {
		float sc = vg_get_scale(vg);
		int c_start = 0;
		while (c_start < count) {
			int c_end = c_start + 1;
			while (c_end < count && pts[c_end].op != VG_OP_MOVE_TO)
				c_end++;

			for (int i = c_start; i + 1 < c_end; i++)
				renderer_line(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, stroke_w * sc, stroke_col);

			if (c_end > c_start && pts[c_end - 1].op == VG_OP_CLOSE)
				renderer_line(pts[c_end - 1].x, pts[c_end - 1].y, pts[c_start].x, pts[c_start].y, stroke_w * sc, stroke_col);

			c_start = c_end;
		}
	}
}

static int vg_path(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	int opts_idx = 3;

	if (lua_isuserdata(L, 2)) {
		struct wisp_cached_path *cp = (struct wisp_cached_path *)luaL_checkudata(L, 2, "WispCachedPath");
		if (!cp || cp->count <= 0)
			return 0;

		/* Apply current transform to cached points */
		/* shortcut: stack buffer for transformed cached points up to 1024, upgrade to heap if huge */
		struct vg_point tmp[1024];
		int n = cp->count < 1024 ? cp->count : 1024;
		for (int i = 0; i < n; i++) {
			vg_transform_point(vg, cp->points[i].x, cp->points[i].y, &tmp[i].x, &tmp[i].y);
			tmp[i].op = cp->points[i].op;
		}

		bool do_fill = false, do_stroke = false;
		uint32_t fill_col = 0, stroke_col = 0;
		float stroke_w = 1.0f;

		if (lua_istable(L, opts_idx)) {
			lua_getfield(L, opts_idx, "fill");
			if (!lua_isnil(L, -1)) { do_fill = true; fill_col = parse_color_value(L, -1); }
			lua_pop(L, 1);
			lua_getfield(L, opts_idx, "stroke");
			if (!lua_isnil(L, -1)) { do_stroke = true; stroke_col = parse_color_value(L, -1); }
			lua_pop(L, 1);
			lua_getfield(L, opts_idx, "stroke_width");
			if (!lua_isnumber(L, -1)) {
				lua_pop(L, 1);
				lua_getfield(L, opts_idx, "width");
			}
			if (lua_isnumber(L, -1)) stroke_w = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);
		}
		vg_render_path_internal(vg, tmp, n, do_fill, fill_col, do_stroke, stroke_col, stroke_w);
		return 0;
	}

	if (!lua_isfunction(L, 2))
		return 0;

	vg->path_len = 0;
	vg->cur_x = 0; vg->cur_y = 0;
	vg->start_x = 0; vg->start_y = 0;
	vg->state_stack[vg->state_sp].has_fill = false;
	vg->state_stack[vg->state_sp].has_stroke = false;

	if (vg->nvg)
		nvgBeginPath(vg->nvg);

	/* Push path builder object */
	lua_pushvalue(L, 2);
	lua_getfield(L, LUA_REGISTRYINDEX, "wisp_path_obj");
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		fprintf(stderr, "wisp: path error: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	bool do_fill = vg->state_stack[vg->state_sp].has_fill;
	bool do_stroke = vg->state_stack[vg->state_sp].has_stroke;
	uint32_t fill_col = vg->state_stack[vg->state_sp].fill_color;
	uint32_t stroke_col = vg->state_stack[vg->state_sp].stroke_color;
	float stroke_w = vg->state_stack[vg->state_sp].stroke_width > 0.0f ? vg->state_stack[vg->state_sp].stroke_width : 1.0f;

	if (lua_istable(L, opts_idx)) {
		lua_getfield(L, opts_idx, "fill");
		if (!lua_isnil(L, -1)) { do_fill = true; fill_col = parse_color_value(L, -1); }
		lua_pop(L, 1);
		lua_getfield(L, opts_idx, "stroke");
		if (!lua_isnil(L, -1)) { do_stroke = true; stroke_col = parse_color_value(L, -1); }
		lua_pop(L, 1);
		lua_getfield(L, opts_idx, "stroke_width");
		if (!lua_isnumber(L, -1)) {
			lua_pop(L, 1);
			lua_getfield(L, opts_idx, "width");
		}
		if (lua_isnumber(L, -1)) stroke_w = (float)lua_tonumber(L, -1);
		lua_pop(L, 1);
	}

	if (vg->nvg) {
		if (do_fill) {
			nvgFillColor(vg->nvg, nvgRGBA((fill_col >> 16) & 0xFF, (fill_col >> 8) & 0xFF, fill_col & 0xFF, (fill_col >> 24) & 0xFF));
			nvgFill(vg->nvg);
		}
		if (do_stroke) {
			nvgStrokeWidth(vg->nvg, stroke_w);
			nvgStrokeColor(vg->nvg, nvgRGBA((stroke_col >> 16) & 0xFF, (stroke_col >> 8) & 0xFF, stroke_col & 0xFF, (stroke_col >> 24) & 0xFF));
			nvgStroke(vg->nvg);
		}
	} else {
		vg_render_path_internal(vg, vg->path, vg->path_len, do_fill, fill_col, do_stroke, stroke_col, stroke_w);
	}
	return 0;
}

static int cached_path_gc(lua_State *L)
{
	struct wisp_cached_path *cp = (struct wisp_cached_path *)luaL_checkudata(L, 1, "WispCachedPath");
	if (cp && cp->points) {
		free(cp->points);
		cp->points = NULL;
		cp->count = 0;
	}
	return 0;
}

static int vg_create_path(lua_State *L)
{
	if (!lua_isfunction(L, 2))
		return 0;

	struct wisp_vg_ctx *vg = &g_vg_ctx;
	/* Save current state and set identity for building local coordinates */
	struct vg_state saved_st = vg->state_stack[vg->state_sp];
	vg->state_stack[vg->state_sp] = (struct vg_state){ .a = 1, .d = 1 };
	vg->path_len = 0;
	vg->cur_x = 0; vg->cur_y = 0;
	vg->start_x = 0; vg->start_y = 0;

	lua_pushvalue(L, 2);
	lua_getfield(L, LUA_REGISTRYINDEX, "wisp_path_obj");
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		fprintf(stderr, "wisp: create_path error: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	vg->state_stack[vg->state_sp] = saved_st;

	struct wisp_cached_path *cp = (struct wisp_cached_path *)lua_newuserdata(L, sizeof(struct wisp_cached_path));
	memset(cp, 0, sizeof(*cp));
	luaL_setmetatable(L, "WispCachedPath");

	if (vg->path_len > 0) {
		cp->points = malloc((size_t)vg->path_len * sizeof(struct vg_point));
		if (cp->points) {
			memcpy(cp->points, vg->path, (size_t)vg->path_len * sizeof(struct vg_point));
			cp->count = vg->path_len;
		}
	}
	return 1;
}

static int vg_image(lua_State *L)
{
	struct wisp_vg_ctx *vg = &g_vg_ctx;
	float x = (float)luaL_optnumber(L, 3, 0.0);
	float y = (float)luaL_optnumber(L, 4, 0.0);
	float w = (float)luaL_optnumber(L, 5, 0.0);
	float h = (float)luaL_optnumber(L, 6, 0.0);

	struct screen_image *simg = NULL;
	const char *path = NULL;

	if (lua_isuserdata(L, 2)) {
		struct screen_image **ud = (struct screen_image **)luaL_testudata(L, 2, "WispImage");
		if (ud && *ud)
			simg = screen_image_from_ptr(*ud);
	} else if (lua_isstring(L, 2)) {
		path = lua_tostring(L, 2);
	}

	if (!simg && !path)
		return 0;

	if (w <= 0.0f) w = simg ? (float)simg->width : 100.0f;
	if (h <= 0.0f) h = simg ? (float)simg->height : 100.0f;

	float tx0, ty0;
	vg_transform_point(vg, x, y, &tx0, &ty0);

	if (vg->nvg) {
		int handle = 0;
		if (simg) {
			handle = screen_image_get_or_upload_nvg(vg->nvg, simg);
		} else if (path) {
			handle = nvgCreateImage(vg->nvg, path, 0);
		}
		if (handle > 0) {
			NVGpaint imgPaint = nvgImagePattern(vg->nvg, tx0, ty0, w, h, 0.0f, handle, 1.0f);
			nvgBeginPath(vg->nvg);
			nvgRect(vg->nvg, tx0, ty0, w, h);
			nvgFillPaint(vg->nvg, imgPaint);
			nvgFill(vg->nvg);
		}
	} else if (simg) {
		renderer_screen_image(simg, tx0, ty0, w, h, 0.0f, 1.0f);
	} else if (path) {
		renderer_image(path, tx0, ty0, w, h, 0.0f, 1.0f);
	}
	return 0;
}

static int vg_draw_path(lua_State *L)
{
	return vg_path(L);
}

static int vg_get_width(lua_State *L)
{
	(void)L;
	lua_pushnumber(L, g_vg_ctx.canvas_w);
	return 1;
}

static int vg_get_height(lua_State *L)
{
	(void)L;
	lua_pushnumber(L, g_vg_ctx.canvas_h);
	return 1;
}

static const luaL_Reg vg_methods[] = {
	{"clear", vg_clear},
	{"rect", vg_rect},
	{"round_rect", vg_round_rect},
	{"rect_outline", vg_rect_outline},
	{"stroke_rect", vg_rect_outline},
	{"circle", vg_circle},
	{"ellipse", vg_ellipse},
	{"line", vg_line},
	{"polyline", vg_polyline},
	{"polygon", vg_polygon},
	{"text", vg_text},
	{"image", vg_image},
	{"clip", vg_clip},
	{"clip_end", vg_reset_clip},
	{"reset_clip", vg_reset_clip},
	{"translate", vg_translate},
	{"scale", vg_scale},
	{"rotate", vg_rotate},
	{"rotation", vg_rotate},
	{"save", vg_save},
	{"restore", vg_restore},
	{"gradient", vg_gradient},
	{"path", vg_path},
	{"create_path", vg_create_path},
	{"draw_path", vg_draw_path},
	{"getWidth", vg_get_width},
	{"getHeight", vg_get_height},
	{NULL, NULL},
};

void wisp_vg_init(lua_State *L)
{
	if (!L)
		return;

	/* Create WispGraphics metatable */
	if (luaL_newmetatable(L, "WispGraphics")) {
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		luaL_setfuncs(L, vg_methods, 0);
	}
	lua_pop(L, 1);

	/* Create WispPath metatable */
	if (luaL_newmetatable(L, "WispPath")) {
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		luaL_setfuncs(L, path_methods, 0);
	}
	lua_pop(L, 1);

	/* Create WispCachedPath metatable */
	if (luaL_newmetatable(L, "WispCachedPath")) {
		lua_pushcfunction(L, cached_path_gc);
		lua_setfield(L, -2, "__gc");
	}
	lua_pop(L, 1);

	/* Pre-allocate singleton userdata objects in registry for zero-alloc drawing */
	void **g_ud = (void **)lua_newuserdata(L, sizeof(void *));
	*g_ud = &g_vg_ctx;
	luaL_setmetatable(L, "WispGraphics");
	lua_setfield(L, LUA_REGISTRYINDEX, "wisp_graphics_obj");

	void **p_ud = (void **)lua_newuserdata(L, sizeof(void *));
	*p_ud = &g_vg_ctx;
	luaL_setmetatable(L, "WispPath");
	lua_setfield(L, LUA_REGISTRYINDEX, "wisp_path_obj");
}

void wisp_vg_shutdown(lua_State *L)
{
	if (!L)
		return;
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "wisp_graphics_obj");
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "wisp_path_obj");
}

void wisp_vg_render_canvas(struct widget *wgt, struct custom_render_payload *p,
                           float bx, float by, float bw, float bh)
{
	if (!wgt || !wgt->lua_L || bw <= 0.0f || bh <= 0.0f)
		return;

	int draw_ref = p ? p->lua_draw_ref : LUA_NOREF;
	if (draw_ref == LUA_NOREF && p && p->node_idx >= 0 && p->node_idx < wgt->interact.node_count)
		draw_ref = wgt->interact.nodes[p->node_idx].lua_on_draw;

	if (draw_ref == LUA_NOREF)
		return;

	lua_State *L = wgt->lua_L;
	int S = wgt->scale > 0 ? wgt->scale : 1;

	/* Setup graphics context state */
	g_vg_ctx.wgt = wgt;
	g_vg_ctx.pixels = wgt->pixels;
	g_vg_ctx.sw = wgt->width;
	g_vg_ctx.sh = wgt->height;
	g_vg_ctx.scale = (float)S;
	g_vg_ctx.canvas_x = bx;
	g_vg_ctx.canvas_y = by;
	g_vg_ctx.canvas_w = bw;
	g_vg_ctx.canvas_h = bh;
	g_vg_ctx.nvg = NULL;

	g_vg_ctx.state_sp = 0;
	g_vg_ctx.state_stack[0] = (struct vg_state){
		.a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 1.0f,
		.tx = bx, .ty = by,
		.has_clip = false
	};

	renderer_clip(bx, by, bw, bh);

	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, draw_ref);
	if (lua_isfunction(L, -1)) {
		lua_getfield(L, LUA_REGISTRYINDEX, "wisp_graphics_obj");
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			fprintf(stderr, "wisp: canvas draw error: %s\n", lua_tostring(L, -1));
		}
	}
	lua_settop(L, top);

	while (g_vg_ctx.state_sp > 0) {
		if (g_vg_ctx.state_stack[g_vg_ctx.state_sp].has_clip)
			renderer_clip_end();
		g_vg_ctx.state_sp--;
	}
	renderer_clip_end();
}

void wisp_vg_render_canvas_nanovg(void *vg, struct widget *wgt, struct custom_render_payload *p,
                                  float bx, float by, float bw, float bh)
{
	if (!vg || !wgt || !wgt->lua_L || bw <= 0.0f || bh <= 0.0f)
		return;

	int draw_ref = p ? p->lua_draw_ref : LUA_NOREF;
	if (draw_ref == LUA_NOREF && p && p->node_idx >= 0 && p->node_idx < wgt->interact.node_count)
		draw_ref = wgt->interact.nodes[p->node_idx].lua_on_draw;

	if (draw_ref == LUA_NOREF)
		return;

	lua_State *L = wgt->lua_L;
	NVGcontext *nvg = (NVGcontext *)vg;

	g_vg_ctx.wgt = wgt;
	g_vg_ctx.pixels = NULL;
	g_vg_ctx.canvas_x = bx;
	g_vg_ctx.canvas_y = by;
	g_vg_ctx.canvas_w = bw;
	g_vg_ctx.canvas_h = bh;
	g_vg_ctx.nvg = nvg;

	g_vg_ctx.state_sp = 0;
	g_vg_ctx.state_stack[0] = (struct vg_state){
		.a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 1.0f,
		.tx = 0, .ty = 0,
		.has_clip = false
	};

	nvgSave(nvg);
	nvgIntersectScissor(nvg, bx, by, bw, bh);
	nvgTranslate(nvg, bx, by);

	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, draw_ref);
	if (lua_isfunction(L, -1)) {
		lua_getfield(L, LUA_REGISTRYINDEX, "wisp_graphics_obj");
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			fprintf(stderr, "wisp: canvas nvg draw error: %s\n", lua_tostring(L, -1));
		}
	}
	lua_settop(L, top);

	nvgRestore(nvg);
}

void wisp_vg_render_surface(struct widget *wgt)
{
	if (!wgt || !wgt->lua_L || wgt->lua_draw == LUA_NOREF || !wgt->pixels)
		return;

	lua_State *L = wgt->lua_L;
	int S = wgt->scale > 0 ? wgt->scale : 1;
	float bw = (float)(wgt->width / S);
	float bh = (float)(wgt->height / S);

	g_vg_ctx.wgt = wgt;
	g_vg_ctx.pixels = wgt->pixels;
	g_vg_ctx.sw = wgt->width;
	g_vg_ctx.sh = wgt->height;
	g_vg_ctx.scale = (float)S;
	g_vg_ctx.canvas_x = 0;
	g_vg_ctx.canvas_y = 0;
	g_vg_ctx.canvas_w = bw;
	g_vg_ctx.canvas_h = bh;
	g_vg_ctx.nvg = NULL;

	g_vg_ctx.state_sp = 0;
	g_vg_ctx.state_stack[0] = (struct vg_state){
		.a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 1.0f,
		.tx = 0, .ty = 0,
		.has_clip = false
	};

	renderer_begin(wgt, bw, bh, (float)S);
	renderer_clip(0, 0, bw, bh);

	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_draw);
	if (lua_isfunction(L, -1)) {
		lua_getfield(L, LUA_REGISTRYINDEX, "wisp_graphics_obj");
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			fprintf(stderr, "wisp: surface draw error: %s\n", lua_tostring(L, -1));
		}
	}
	lua_settop(L, top);

	while (g_vg_ctx.state_sp > 0) {
		if (g_vg_ctx.state_stack[g_vg_ctx.state_sp].has_clip)
			renderer_clip_end();
		g_vg_ctx.state_sp--;
	}
	renderer_clip_end();
	renderer_end(wgt);
}

void wisp_vg_render_surface_nanovg(void *vg, struct widget *wgt)
{
	if (!vg || !wgt || !wgt->lua_L || wgt->lua_draw == LUA_NOREF)
		return;

	lua_State *L = wgt->lua_L;
	NVGcontext *nvg = (NVGcontext *)vg;
	int S = wgt->scale > 0 ? wgt->scale : 1;
	float bw = (float)(wgt->width / S);
	float bh = (float)(wgt->height / S);

	g_vg_ctx.wgt = wgt;
	g_vg_ctx.pixels = NULL;
	g_vg_ctx.canvas_x = 0;
	g_vg_ctx.canvas_y = 0;
	g_vg_ctx.canvas_w = bw;
	g_vg_ctx.canvas_h = bh;
	g_vg_ctx.nvg = nvg;

	g_vg_ctx.state_sp = 0;
	g_vg_ctx.state_stack[0] = (struct vg_state){
		.a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 1.0f,
		.tx = 0, .ty = 0,
		.has_clip = false
	};

	nvgSave(nvg);
	nvgScissor(nvg, 0, 0, bw, bh);

	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_draw);
	if (lua_isfunction(L, -1)) {
		lua_getfield(L, LUA_REGISTRYINDEX, "wisp_graphics_obj");
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			fprintf(stderr, "wisp: surface nvg draw error: %s\n", lua_tostring(L, -1));
		}
	}
	lua_settop(L, top);

	nvgRestore(nvg);
}
