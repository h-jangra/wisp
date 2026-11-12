#include "render/renderer_native.h"
#include "render/buffer.h"
#include "render/text.h"
#include "render/image.h"
#include "render/shapes.h"
#include "render/vector.h"
#include "services/screen.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

#define MAX_CLIP_DEPTH 16

struct clip_box {
	int x0, y0, x1, y1;
};

static struct {
	struct widget *wgt;
	uint32_t *pixels;
	uint32_t sw, sh;
	float scale;
	struct rect dmg;
	struct clip_box clip_stack[MAX_CLIP_DEPTH];
	int clip_sp;
} r_ctx, r_ctx_saved;

static inline void damage_box(int x0, int y0, int x1, int y1)
{
	if (x1 <= x0 || y1 <= y0)
		return;
	rect_add(&r_ctx.dmg, r_ctx.sw, r_ctx.sh, x0, y0, x1 - x0, y1 - y0);
}

static inline uint32_t blend_color_cov(uint32_t dst, uint32_t src, uint32_t cov)
{
	uint32_t sa = (((src >> 24) & 0xFF) * cov + 127) / 255;
	if (sa == 0)
		return dst;
	if (dst == 0) {
		if (cov >= 255)
			return src;
		return (sa << 24) | (src & 0x00FFFFFFu);
	}
	if (sa >= 255)
		return src;

	uint32_t da = (dst >> 24) & 0xFF;
	if (da == 0) {
		if (cov >= 255)
			return src;
		return (sa << 24) | (src & 0x00FFFFFFu);
	}

	uint32_t inv_sa = 255 - sa;
	uint32_t sr = (src >> 16) & 0xFF;
	uint32_t sg = (src >> 8) & 0xFF;
	uint32_t sb = src & 0xFF;

	uint32_t dr = (dst >> 16) & 0xFF;
	uint32_t dg = (dst >> 8) & 0xFF;
	uint32_t db = dst & 0xFF;

	uint32_t r = (sr * sa + dr * inv_sa + 127) / 255;
	uint32_t g = (sg * sa + dg * inv_sa + 127) / 255;
	uint32_t b = (sb * sa + db * inv_sa + 127) / 255;
	uint32_t a = sa + (da * inv_sa + 127) / 255;
	if (a > 255) a = 255;

	return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t lerp_color(uint32_t c1, uint32_t c2, float t)
{
	if (t <= 0.0f) return c1;
	if (t >= 1.0f) return c2;

	uint32_t a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
	uint32_t a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;

	uint32_t a = (uint32_t)(a1 + (a2 - a1) * t + 0.5f);
	uint32_t r = (uint32_t)(r1 + (r2 - r1) * t + 0.5f);
	uint32_t g = (uint32_t)(g1 + (g2 - g1) * t + 0.5f);
	uint32_t b = (uint32_t)(b1 + (b2 - b1) * t + 0.5f);

	return (a << 24) | (r << 16) | (g << 8) | b;
}

void renderer_native_init(void)
{
	init_text();
}

void renderer_native_shutdown(void)
{
	shutdown_text();
	shutdown_images();
}

void renderer_begin(struct widget *wgt, float width, float height, float scale)
{
	(void)width;
	(void)height;
	r_ctx.wgt = wgt;
	r_ctx.pixels = wgt ? wgt->pixels : NULL;
	r_ctx.sw = wgt ? wgt->width : 0;
	r_ctx.sh = wgt ? wgt->height : 0;
	r_ctx.scale = scale > 0.0f ? scale : 1.0f;
	r_ctx.dmg = (struct rect){0, 0, 0, 0, false};
	r_ctx.clip_sp = 0;
	r_ctx.clip_stack[0] = (struct clip_box){0, 0, (int)r_ctx.sw, (int)r_ctx.sh};

	if (r_ctx.pixels && r_ctx.sw > 0 && r_ctx.sh > 0) {
		uint32_t clear_color = 0;
		if (wgt && !wgt->transparent) {
			if (wgt->bg_color != 0)
				clear_color = wgt->bg_color;
			else if (wgt->surf_type == SURFACE_TOPLEVEL)
				clear_color = 0xFF181825u;
		}
		if (clear_color == 0) {
			memset(r_ctx.pixels, 0, (size_t)r_ctx.sw * r_ctx.sh * sizeof(uint32_t));
		} else {
			size_t total = (size_t)r_ctx.sw * r_ctx.sh;
			for (size_t i = 0; i < total; i++)
				r_ctx.pixels[i] = clear_color;
		}
		damage_box(0, 0, (int)r_ctx.sw, (int)r_ctx.sh);
	}
}

void renderer_end(struct widget *wgt)
{
	if (!wgt || !wgt->pixels)
		return;

	if (wgt->surface && r_ctx.dmg.valid)
		scene_commit(wgt, &r_ctx.dmg);

	r_ctx.wgt = NULL;
	r_ctx.pixels = NULL;
}

void renderer_begin_buffer(uint32_t *pixels, uint32_t sw, uint32_t sh, float scale)
{
	r_ctx_saved = r_ctx;
	r_ctx.wgt = NULL;
	r_ctx.pixels = pixels;
	r_ctx.sw = sw;
	r_ctx.sh = sh;
	r_ctx.scale = scale > 0.0f ? scale : 1.0f;
	r_ctx.dmg = (struct rect){0, 0, 0, 0, false};
	r_ctx.clip_sp = 0;
	r_ctx.clip_stack[0] = (struct clip_box){0, 0, (int)sw, (int)sh};
}

void renderer_end_buffer(void)
{
	r_ctx = r_ctx_saved;
}

void renderer_clip(float x, float y, float w, float h)
{
	if (r_ctx.clip_sp >= MAX_CLIP_DEPTH - 1)
		return;

	float S = r_ctx.scale;
	int cx0 = (int)floorf(x * S);
	int cy0 = (int)floorf(y * S);
	int cx1 = (int)ceilf((x + w) * S);
	int cy1 = (int)ceilf((y + h) * S);

	struct clip_box top = r_ctx.clip_stack[r_ctx.clip_sp];
	if (cx0 < top.x0) cx0 = top.x0;
	if (cy0 < top.y0) cy0 = top.y0;
	if (cx1 > top.x1) cx1 = top.x1;
	if (cy1 > top.y1) cy1 = top.y1;
	if (cx1 < cx0) cx1 = cx0;
	if (cy1 < cy0) cy1 = cy0;

	r_ctx.clip_sp++;
	r_ctx.clip_stack[r_ctx.clip_sp] = (struct clip_box){cx0, cy0, cx1, cy1};
}

void renderer_clip_end(void)
{
	if (r_ctx.clip_sp > 0)
		r_ctx.clip_sp--;
}

void renderer_round_rect_varying(float x, float y, float w, float h,
                                float rtl, float rtr, float rbr, float rbl,
                                uint32_t color)
{
	if (!r_ctx.pixels || (color >> 24) == 0 || w <= 0.0f || h <= 0.0f)
		return;

	float S = r_ctx.scale;
	float fx0 = x * S, fy0 = y * S;
	float fx1 = (x + w) * S, fy1 = (y + h) * S;
	float fw = fx1 - fx0, fh = fy1 - fy0;

	float r0 = rtl * S, r1 = rtr * S, r2 = rbr * S, r3 = rbl * S;
	float max_r = (fw < fh ? fw : fh) * 0.5f;
	if (r0 > max_r) r0 = max_r;
	if (r1 > max_r) r1 = max_r;
	if (r2 > max_r) r2 = max_r;
	if (r3 > max_r) r3 = max_r;
	if (r0 < 0.0f) r0 = 0.0f;
	if (r1 < 0.0f) r1 = 0.0f;
	if (r2 < 0.0f) r2 = 0.0f;
	if (r3 < 0.0f) r3 = 0.0f;

	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	int ix0 = (int)floorf(fx0), iy0 = (int)floorf(fy0);
	int ix1 = (int)ceilf(fx1), iy1 = (int)ceilf(fy1);

	int bx0 = ix0 < clip.x0 ? clip.x0 : ix0;
	int by0 = iy0 < clip.y0 ? clip.y0 : iy0;
	int bx1 = ix1 > clip.x1 ? clip.x1 : ix1;
	int by1 = iy1 > clip.y1 ? clip.y1 : iy1;

	if (bx0 >= bx1 || by0 >= by1)
		return;

	uint32_t sw = r_ctx.sw;
	uint32_t *dst = r_ctx.pixels;
	damage_box(bx0, by0, bx1, by1);

	bool no_radius = (r0 <= 0.5f && r1 <= 0.5f && r2 <= 0.5f && r3 <= 0.5f);
	bool opaque = ((color >> 24) == 0xFF);

	if (no_radius) {
		for (int py = by0; py < by1; py++) {
			uint32_t *row = &dst[py * sw + bx0];
			int len = bx1 - bx0;
			if (opaque) {
				for (int i = 0; i < len; i++) row[i] = color;
			} else {
				for (int i = 0; i < len; i++) row[i] = blend_color_cov(row[i], color, 255);
			}
		}
		return;
	}

	for (int py = by0; py < by1; py++) {
		float Y = (float)py + 0.5f;
		uint32_t *row = &dst[py * sw];

		for (int px = bx0; px < bx1; px++) {
			float X = (float)px + 0.5f;
			float cov = 1.0f;

			if (X < fx0 + r0 && Y < fy0 + r0 && r0 > 0.0f) {
				float dx = X - (fx0 + r0);
				float dy = Y - (fy0 + r0);
				float dist = sqrtf(dx * dx + dy * dy);
				cov = r0 - dist + 0.5f;
			} else if (X > fx1 - r1 && Y < fy0 + r1 && r1 > 0.0f) {
				float dx = X - (fx1 - r1);
				float dy = Y - (fy0 + r1);
				float dist = sqrtf(dx * dx + dy * dy);
				cov = r1 - dist + 0.5f;
			} else if (X > fx1 - r2 && Y > fy1 - r2 && r2 > 0.0f) {
				float dx = X - (fx1 - r2);
				float dy = Y - (fy1 - r2);
				float dist = sqrtf(dx * dx + dy * dy);
				cov = r2 - dist + 0.5f;
			} else if (X < fx0 + r3 && Y > fy1 - r3 && r3 > 0.0f) {
				float dx = X - (fx0 + r3);
				float dy = Y - (fy1 - r3);
				float dist = sqrtf(dx * dx + dy * dy);
				cov = r3 - dist + 0.5f;
			}

			if (cov <= 0.0f)
				continue;
			if (cov >= 1.0f) {
				row[px] = opaque ? color : blend_color_cov(row[px], color, 255);
			} else {
				uint32_t ucov = (uint32_t)(cov * 255.0f + 0.5f);
				row[px] = blend_color_cov(row[px], color, ucov);
			}
		}
	}
}

void renderer_round_rect(float x, float y, float w, float h, float radius, uint32_t color)
{
	renderer_round_rect_varying(x, y, w, h, radius, radius, radius, radius, color);
}

void renderer_rect(float x, float y, float w, float h, uint32_t color)
{
	renderer_round_rect_varying(x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, color);
}

void renderer_border(float x, float y, float w, float h,
                     float radius, float top_w, float bot_w, float left_w, float right_w,
                     uint32_t color)
{
	if (!r_ctx.pixels || (color >> 24) == 0 || w <= 0.0f || h <= 0.0f)
		return;

	if (top_w <= 0.0f && bot_w <= 0.0f && left_w <= 0.0f && right_w <= 0.0f)
		return;

	float S = r_ctx.scale;
	bool uniform_stroke = (top_w == bot_w && top_w == left_w && top_w == right_w);

	if (uniform_stroke && radius > 0.0f) {
		float stroke = top_w * S;
		float fx0 = x * S, fy0 = y * S;
		float fx1 = (x + w) * S, fy1 = (y + h) * S;
		float R_out = radius * S;
		float R_in = R_out > stroke ? R_out - stroke : 0.0f;

		struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
		int bx0 = (int)floorf(fx0), by0 = (int)floorf(fy0);
		int bx1 = (int)ceilf(fx1), by1 = (int)ceilf(fy1);
		if (bx0 < clip.x0) bx0 = clip.x0;
		if (by0 < clip.y0) by0 = clip.y0;
		if (bx1 > clip.x1) bx1 = clip.x1;
		if (by1 > clip.y1) by1 = clip.y1;
		if (bx0 >= bx1 || by0 >= by1)
			return;

		damage_box(bx0, by0, bx1, by1);
		uint32_t sw = r_ctx.sw;
		uint32_t *dst = r_ctx.pixels;

		for (int py = by0; py < by1; py++) {
			float Y = (float)py + 0.5f;
			uint32_t *row = &dst[py * sw];

			for (int px = bx0; px < bx1; px++) {
				float X = (float)px + 0.5f;

				/* Outer SDF */
				float d_out = 1.0f;
				if (X < fx0 + R_out && Y < fy0 + R_out) {
					float dx = X - (fx0 + R_out), dy = Y - (fy0 + R_out);
					d_out = R_out - sqrtf(dx * dx + dy * dy) + 0.5f;
				} else if (X > fx1 - R_out && Y < fy0 + R_out) {
					float dx = X - (fx1 - R_out), dy = Y - (fy0 + R_out);
					d_out = R_out - sqrtf(dx * dx + dy * dy) + 0.5f;
				} else if (X > fx1 - R_out && Y > fy1 - R_out) {
					float dx = X - (fx1 - R_out), dy = Y - (fy1 - R_out);
					d_out = R_out - sqrtf(dx * dx + dy * dy) + 0.5f;
				} else if (X < fx0 + R_out && Y > fy1 - R_out) {
					float dx = X - (fx0 + R_out), dy = Y - (fy1 - R_out);
					d_out = R_out - sqrtf(dx * dx + dy * dy) + 0.5f;
				}
				if (d_out <= 0.0f) continue;
				if (d_out > 1.0f) d_out = 1.0f;

				/* Inner SDF */
				float d_in = 0.0f;
				float ifx0 = fx0 + stroke, ify0 = fy0 + stroke;
				float ifx1 = fx1 - stroke, ify1 = fy1 - stroke;
				if (X >= ifx0 && X <= ifx1 && Y >= ify0 && Y <= ify1) {
					d_in = 1.0f;
					if (X < ifx0 + R_in && Y < ify0 + R_in && R_in > 0.0f) {
						float dx = X - (ifx0 + R_in), dy = Y - (ify0 + R_in);
						d_in = R_in - sqrtf(dx * dx + dy * dy) + 0.5f;
					} else if (X > ifx1 - R_in && Y < ify0 + R_in && R_in > 0.0f) {
						float dx = X - (ifx1 - R_in), dy = Y - (ify0 + R_in);
						d_in = R_in - sqrtf(dx * dx + dy * dy) + 0.5f;
					} else if (X > ifx1 - R_in && Y > ify1 - R_in && R_in > 0.0f) {
						float dx = X - (ifx1 - R_in), dy = Y - (ify1 - R_in);
						d_in = R_in - sqrtf(dx * dx + dy * dy) + 0.5f;
					} else if (X < ifx0 + R_in && Y > ify1 - R_in && R_in > 0.0f) {
						float dx = X - (ifx0 + R_in), dy = Y - (ify1 - R_in);
						d_in = R_in - sqrtf(dx * dx + dy * dy) + 0.5f;
					}
					if (d_in < 0.0f) d_in = 0.0f;
					if (d_in > 1.0f) d_in = 1.0f;
				}

				float cov = d_out - d_in;
				if (cov > 0.0f) {
					uint32_t ucov = (uint32_t)(cov * 255.0f + 0.5f);
					if (ucov > 255) ucov = 255;
					row[px] = blend_color_cov(row[px], color, ucov);
				}
			}
		}
	} else {
		/* Non-rounded or asymmetric borders as solid rects without corner overlap */
		if (top_w > 0.0f)
			renderer_rect(x, y, w, top_w, color);
		if (bot_w > 0.0f)
			renderer_rect(x, y + h - bot_w, w, bot_w, color);
		float mid_y = y + top_w;
		float mid_h = h - top_w - bot_w;
		if (mid_h > 0.0f) {
			if (left_w > 0.0f)
				renderer_rect(x, mid_y, left_w, mid_h, color);
			if (right_w > 0.0f)
				renderer_rect(x + w - right_w, mid_y, right_w, mid_h, color);
		}
	}
}

void renderer_circle(float cx, float cy, float radius, uint32_t color)
{
	if (!r_ctx.pixels || (color >> 24) == 0 || radius <= 0.0f)
		return;

	float S = r_ctx.scale;
	float fcx = cx * S, fcy = cy * S, fr = radius * S;

	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	int bx0 = (int)floorf(fcx - fr);
	int by0 = (int)floorf(fcy - fr);
	int bx1 = (int)ceilf(fcx + fr);
	int by1 = (int)ceilf(fcy + fr);

	if (bx0 < clip.x0) bx0 = clip.x0;
	if (by0 < clip.y0) by0 = clip.y0;
	if (bx1 > clip.x1) bx1 = clip.x1;
	if (by1 > clip.y1) by1 = clip.y1;
	if (bx0 >= bx1 || by0 >= by1)
		return;

	damage_box(bx0, by0, bx1, by1);
	uint32_t sw = r_ctx.sw;
	uint32_t *dst = r_ctx.pixels;
	bool opaque = ((color >> 24) == 0xFF);

	for (int py = by0; py < by1; py++) {
		float dy = ((float)py + 0.5f) - fcy;
		uint32_t *row = &dst[py * sw];

		for (int px = bx0; px < bx1; px++) {
			float dx = ((float)px + 0.5f) - fcx;
			float dist = sqrtf(dx * dx + dy * dy);
			float cov = fr - dist + 0.5f;

			if (cov <= 0.0f)
				continue;
			if (cov >= 1.0f) {
				row[px] = opaque ? color : blend_color_cov(row[px], color, 255);
			} else {
				uint32_t ucov = (uint32_t)(cov * 255.0f + 0.5f);
				row[px] = blend_color_cov(row[px], color, ucov);
			}
		}
	}
}

void renderer_line(float x1, float y1, float x2, float y2, float width, uint32_t color)
{
	if (!r_ctx.pixels || (color >> 24) == 0 || width <= 0.0f)
		return;

	float S = r_ctx.scale;
	float fx1 = x1 * S, fy1 = y1 * S;
	float fx2 = x2 * S, fy2 = y2 * S;
	float fw = width * S;
	float hr = fw * 0.5f;

	float min_x = (fx1 < fx2 ? fx1 : fx2) - hr - 1.0f;
	float max_x = (fx1 > fx2 ? fx1 : fx2) + hr + 1.0f;
	float min_y = (fy1 < fy2 ? fy1 : fy2) - hr - 1.0f;
	float max_y = (fy1 > fy2 ? fy1 : fy2) + hr + 1.0f;

	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	int bx0 = (int)floorf(min_x);
	int by0 = (int)floorf(min_y);
	int bx1 = (int)ceilf(max_x);
	int by1 = (int)ceilf(max_y);

	if (bx0 < clip.x0) bx0 = clip.x0;
	if (by0 < clip.y0) by0 = clip.y0;
	if (bx1 > clip.x1) bx1 = clip.x1;
	if (by1 > clip.y1) by1 = clip.y1;
	if (bx0 >= bx1 || by0 >= by1)
		return;

	damage_box(bx0, by0, bx1, by1);
	uint32_t sw = r_ctx.sw;
	uint32_t *dst = r_ctx.pixels;

	float vx = fx2 - fx1, vy = fy2 - fy1;
	float len_sq = vx * vx + vy * vy;

	for (int py = by0; py < by1; py++) {
		float Y = (float)py + 0.5f;
		uint32_t *row = &dst[py * sw];

		for (int px = bx0; px < bx1; px++) {
			float X = (float)px + 0.5f;
			float u = len_sq > 0.0001f ? ((X - fx1) * vx + (Y - fy1) * vy) / len_sq : 0.0f;
			if (u < 0.0f) u = 0.0f;
			if (u > 1.0f) u = 1.0f;

			float qx = fx1 + u * vx;
			float qy = fy1 + u * vy;
			float dx = X - qx, dy = Y - qy;
			float dist = sqrtf(dx * dx + dy * dy);
			float cov = hr - dist + 0.5f;

			if (cov <= 0.0f)
				continue;
			if (cov >= 1.0f) {
				row[px] = blend_color_cov(row[px], color, 255);
			} else {
				uint32_t ucov = (uint32_t)(cov * 255.0f + 0.5f);
				row[px] = blend_color_cov(row[px], color, ucov);
			}
		}
	}
}

void renderer_shadow(float x, float y, float w, float h, float radius, float blur,
                     float sx, float sy, uint32_t color)
{
	if (!r_ctx.pixels || (color >> 24) == 0 || blur <= 0.0f || w <= 0.0f || h <= 0.0f)
		return;

	float S = r_ctx.scale;
	float fx = (x + sx) * S, fy = (y + sy) * S;
	float fw = w * S, fh = h * S;
	float r = radius * S;
	float b = blur * S;

	float max_r = (fw < fh ? fw : fh) * 0.5f;
	if (r > max_r) r = max_r;
	if (r < 0.0f) r = 0.0f;

	float spread = b * 2.0f;
	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	int bx0 = (int)floorf(fx - spread);
	int by0 = (int)floorf(fy - spread);
	int bx1 = (int)ceilf(fx + fw + spread);
	int by1 = (int)ceilf(fy + fh + spread);

	if (bx0 < clip.x0) bx0 = clip.x0;
	if (by0 < clip.y0) by0 = clip.y0;
	if (bx1 > clip.x1) bx1 = clip.x1;
	if (by1 > clip.y1) by1 = clip.y1;
	if (bx0 >= bx1 || by0 >= by1)
		return;

	damage_box(bx0, by0, bx1, by1);
	uint32_t sw = r_ctx.sw;
	uint32_t *dst = r_ctx.pixels;

	float cx = fx + fw * 0.5f;
	float cy = fy + fh * 0.5f;
	float hw = fw * 0.5f;
	float hh = fh * 0.5f;

	for (int py = by0; py < by1; py++) {
		float Y = (float)py + 0.5f;
		float dy = fabsf(Y - cy) - hh + r;
		uint32_t *row = &dst[py * sw];

		for (int px = bx0; px < bx1; px++) {
			float X = (float)px + 0.5f;
			float dx = fabsf(X - cx) - hw + r;

			float d;
			if (dx > 0.0f && dy > 0.0f) {
				d = sqrtf(dx * dx + dy * dy) - r;
			} else {
				d = (dx > dy ? dx : dy) - r;
			}

			float u = d / b;
			float alpha;
			if (u <= 0.0f) {
				alpha = 1.0f;
			} else if (u >= 1.0f) {
				continue;
			} else {
				float t = 1.0f - u;
				alpha = t * t * (3.0f - 2.0f * t);
			}

			uint32_t ucov = (uint32_t)(alpha * 255.0f + 0.5f);
			if (ucov > 0)
				row[px] = blend_color_cov(row[px], color, ucov);
		}
	}
}

void renderer_gradient(float x, float y, float w, float h, float radius,
                      uint32_t color1, uint32_t color2, bool radial)
{
	if (!r_ctx.pixels || w <= 0.0f || h <= 0.0f)
		return;

	float S = r_ctx.scale;
	float fx0 = x * S, fy0 = y * S;
	float fx1 = (x + w) * S, fy1 = (y + h) * S;
	float fw = fx1 - fx0, fh = fy1 - fy0;
	float r = radius * S;

	float max_r = (fw < fh ? fw : fh) * 0.5f;
	if (r > max_r) r = max_r;
	if (r < 0.0f) r = 0.0f;

	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	int bx0 = (int)floorf(fx0), by0 = (int)floorf(fy0);
	int bx1 = (int)ceilf(fx1), by1 = (int)ceilf(fy1);

	if (bx0 < clip.x0) bx0 = clip.x0;
	if (by0 < clip.y0) by0 = clip.y0;
	if (bx1 > clip.x1) bx1 = clip.x1;
	if (by1 > clip.y1) by1 = clip.y1;
	if (bx0 >= bx1 || by0 >= by1)
		return;

	damage_box(bx0, by0, bx1, by1);
	uint32_t sw = r_ctx.sw;
	uint32_t *dst = r_ctx.pixels;

	float cx = fx0 + fw * 0.5f;
	float cy = fy0 + fh * 0.5f;
	float R = (fw > fh ? fw : fh) * 0.5f;
	if (R <= 0.0f) R = 1.0f;

	for (int py = by0; py < by1; py++) {
		float Y = (float)py + 0.5f;
		uint32_t *row = &dst[py * sw];

		for (int px = bx0; px < bx1; px++) {
			float X = (float)px + 0.5f;

			/* Rounded corner SDF mask */
			float cov = 1.0f;
			if (r > 0.0f) {
				if (X < fx0 + r && Y < fy0 + r) {
					float dx = X - (fx0 + r), dy = Y - (fy0 + r);
					cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
				} else if (X > fx1 - r && Y < fy0 + r) {
					float dx = X - (fx1 - r), dy = Y - (fy0 + r);
					cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
				} else if (X > fx1 - r && Y > fy1 - r) {
					float dx = X - (fx1 - r), dy = Y - (fy1 - r);
					cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
				} else if (X < fx0 + r && Y > fy1 - r) {
					float dx = X - (fx0 + r), dy = Y - (fy1 - r);
					cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
				}
			}

			if (cov <= 0.0f)
				continue;

			float t;
			if (radial) {
				float dx = X - cx, dy = Y - cy;
				t = sqrtf(dx * dx + dy * dy) / R;
			} else {
				t = (Y - fy0) / fh;
			}

			uint32_t col = lerp_color(color1, color2, t);
			if (cov >= 1.0f) {
				row[px] = blend_color_cov(row[px], col, 255);
			} else {
				uint32_t ucov = (uint32_t)(cov * 255.0f + 0.5f);
				row[px] = blend_color_cov(row[px], col, ucov);
			}
		}
	}
}

void renderer_text(const char *text, int len, float x, float y, float size, uint32_t color)
{
	if (!r_ctx.pixels || !text || len <= 0 || (color >> 24) == 0)
		return;

	float S = r_ctx.scale;
	int fsize = (int)(size * S + 0.5f);
	if (fsize < 6) fsize = 6;
	text_set_size(fsize);

	int ascent = text_ascent(fsize);
	int descent = text_descent(fsize);
	int line_h = text_line_height(fsize);
	int v_offset = (line_h - (ascent + descent)) / 2;
	if (v_offset < 0) v_offset = 0;

	int start_x = (int)(x * S + 0.5f);
	int start_y = (int)(y * S + ascent + v_offset);

	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	struct rect text_dmg = {0};
	draw_text_slice_clipped(r_ctx.pixels, r_ctx.sw, r_ctx.sh, &text_dmg,
	                        text, len, start_x, start_y, color,
	                        clip.x0, clip.y0, clip.x1, clip.y1);
	if (text_dmg.valid)
		damage_box(text_dmg.x, text_dmg.y, text_dmg.x + text_dmg.w, text_dmg.y + text_dmg.h);
}

void renderer_image(const char *path, float x, float y, float w, float h, float radius, float alpha)
{
	if (!r_ctx.pixels || !path || !*path || w <= 0.0f || h <= 0.0f || alpha <= 0.0f)
		return;

	struct image *img = image_get(path);
	if (!img)
		return;

	float S = r_ctx.scale;
	int tw = (int)(w * S), th = (int)(h * S);
	if ((int64_t)img->w * th < (int64_t)img->h * tw)
		tw = (int)((int64_t)img->w * th / img->h);
	else if ((int64_t)img->w * th > (int64_t)img->h * tw)
		th = (int)((int64_t)img->h * tw / img->w);
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;

	struct img_scaled *s = scaled_get(img, tw, th);
	if (!s) {
		image_release(img);
		return;
	}

	float fx0 = x * S + (w * S - s->w) * 0.5f;
	float fy0 = y * S + (h * S - s->h) * 0.5f;
	float r = radius * S;

	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	int bx0 = (int)floorf(fx0), by0 = (int)floorf(fy0);
	int bx1 = bx0 + s->w, by1 = by0 + s->h;

	if (bx0 < clip.x0) bx0 = clip.x0;
	if (by0 < clip.y0) by0 = clip.y0;
	if (bx1 > clip.x1) bx1 = clip.x1;
	if (by1 > clip.y1) by1 = clip.y1;

	if (bx0 < bx1 && by0 < by1) {
		damage_box(bx0, by0, bx1, by1);
		uint32_t sw = r_ctx.sw;
		uint32_t *dst = r_ctx.pixels;
		uint32_t global_alpha = (uint32_t)(alpha * 255.0f + 0.5f);
		if (global_alpha > 255) global_alpha = 255;

		for (int py = by0; py < by1; py++) {
			int sy_idx = py - (int)fy0;
			uint32_t *row = &dst[py * sw];
			const uint32_t *src_row = &s->pix[sy_idx * s->w];

			for (int px = bx0; px < bx1; px++) {
				int sx_idx = px - (int)fx0;
				uint32_t src_col = src_row[sx_idx];

				float cov = 1.0f;
				if (r > 0.0f) {
					float X = (float)px + 0.5f, Y = (float)py + 0.5f;
					float fx1 = fx0 + s->w, fy1 = fy0 + s->h;
					if (X < fx0 + r && Y < fy0 + r) {
						float dx = X - (fx0 + r), dy = Y - (fy0 + r);
						cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
					} else if (X > fx1 - r && Y < fy0 + r) {
						float dx = X - (fx1 - r), dy = Y - (fy0 + r);
						cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
					} else if (X > fx1 - r && Y > fy1 - r) {
						float dx = X - (fx1 - r), dy = Y - (fy1 - r);
						cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
					} else if (X < fx0 + r && Y > fy1 - r) {
						float dx = X - (fx0 + r), dy = Y - (fy1 - r);
						cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
					}
				}

				if (cov <= 0.0f)
					continue;

				uint32_t ucov = (uint32_t)(cov * global_alpha + 0.5f);
				if (ucov > 255) ucov = 255;
				row[px] = blend_color_cov(row[px], src_col, ucov);
			}
		}
	}

	scaled_release(s);
	image_release(img);
}

void renderer_screen_image(struct screen_image *img, float x, float y, float w, float h, float radius, float alpha)
{
	if (!r_ctx.pixels || !img || img->destroyed || !img->pixels || w <= 0.0f || h <= 0.0f || alpha <= 0.0f)
		return;

	float S = r_ctx.scale;
	int tw = (int)(w * S);
	int th = (int)(h * S);
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;

	float fx0 = x * S;
	float fy0 = y * S;
	float r = radius * S;

	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	int bx0 = (int)floorf(fx0), by0 = (int)floorf(fy0);
	int bx1 = bx0 + tw, by1 = by0 + th;

	if (bx0 < clip.x0) bx0 = clip.x0;
	if (by0 < clip.y0) by0 = clip.y0;
	if (bx1 > clip.x1) bx1 = clip.x1;
	if (by1 > clip.y1) by1 = clip.y1;

	if (bx0 < bx1 && by0 < by1) {
		damage_box(bx0, by0, bx1, by1);
		uint32_t sw = r_ctx.sw;
		uint32_t *dst = r_ctx.pixels;
		uint32_t global_alpha = (uint32_t)(alpha * 255.0f + 0.5f);
		if (global_alpha > 255) global_alpha = 255;

		bool is_bgr = (img->format == WL_SHM_FORMAT_XBGR8888 || img->format == WL_SHM_FORMAT_ABGR8888);

		for (int py = by0; py < by1; py++) {
			int sy = (tw == (int)img->width && th == (int)img->height)
				? (py - (int)fy0)
				: (int)((int64_t)(py - (int)fy0) * img->height / th);
			if (sy < 0) sy = 0; else if ((uint32_t)sy >= img->height) sy = img->height - 1;

			uint32_t *dst_row = &dst[py * sw];
			const uint32_t *src_row = (const uint32_t *)((const uint8_t *)img->pixels + (size_t)sy * img->stride);

			if (tw == (int)img->width && th == (int)img->height && global_alpha == 255 && r <= 0.0f && !is_bgr) {
				memcpy(&dst_row[bx0], &src_row[bx0 - (int)fx0], (bx1 - bx0) * sizeof(uint32_t));
			} else {
				for (int px = bx0; px < bx1; px++) {
					int sx = (tw == (int)img->width && th == (int)img->height)
						? (px - (int)fx0)
						: (int)((int64_t)(px - (int)fx0) * img->width / tw);
					if (sx < 0) sx = 0; else if ((uint32_t)sx >= img->width) sx = img->width - 1;

					uint32_t px_val = src_row[sx];
					if (is_bgr) {
						px_val = (px_val & 0xFF00FF00u) | ((px_val & 0xFFu) << 16) | ((px_val >> 16) & 0xFFu);
					}

					float cov = 1.0f;
					if (r > 0.0f) {
						float X = (float)px + 0.5f, Y = (float)py + 0.5f;
						float fx1 = fx0 + tw, fy1 = fy0 + th;
						if (X < fx0 + r && Y < fy0 + r) {
							float dx = X - (fx0 + r), dy = Y - (fy0 + r);
							cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
						} else if (X > fx1 - r && Y < fy0 + r) {
							float dx = X - (fx1 - r), dy = Y - (fy0 + r);
							cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
						} else if (X > fx1 - r && Y > fy1 - r) {
							float dx = X - (fx1 - r), dy = Y - (fy1 - r);
							cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
						} else if (X < fx0 + r && Y > fy1 - r) {
							float dx = X - (fx0 + r), dy = Y - (fy1 - r);
							cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
						}
					}
					if (cov <= 0.0f) continue;
					uint32_t ucov = (uint32_t)(cov * global_alpha + 0.5f);
					if (ucov > 255) ucov = 255;
					dst_row[px] = blend_color_cov(dst_row[px], px_val, ucov);
				}
			}
		}
	}
}

void renderer_icon(const char *name, float x, float y, float w, float h, uint32_t color)
{
	if (!name || !*name || (color >> 24) == 0)
		return;

	float cx = x + w * 0.5f;
	float cy = y + h * 0.5f;
	float s = (w < h ? w : h) * 0.5f;

	if (strcmp(name, "check") == 0) {
		renderer_line(cx - s * 0.6f, cy, cx - s * 0.15f, cy + s * 0.45f, s * 0.28f, color);
		renderer_line(cx - s * 0.15f, cy + s * 0.45f, cx + s * 0.65f, cy - s * 0.45f, s * 0.28f, color);
	} else if (strcmp(name, "close") == 0 || strcmp(name, "x") == 0) {
		renderer_line(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f, s * 0.25f, color);
		renderer_line(cx + s * 0.5f, cy - s * 0.5f, cx - s * 0.5f, cy + s * 0.5f, s * 0.25f, color);
	} else if (strcmp(name, "arrow_up") == 0) {
		renderer_line(cx, cy + s * 0.6f, cx, cy - s * 0.6f, s * 0.22f, color);
		renderer_line(cx - s * 0.4f, cy - s * 0.2f, cx, cy - s * 0.6f, s * 0.22f, color);
		renderer_line(cx + s * 0.4f, cy - s * 0.2f, cx, cy - s * 0.6f, s * 0.22f, color);
	} else if (strcmp(name, "arrow_down") == 0) {
		renderer_line(cx, cy - s * 0.6f, cx, cy + s * 0.6f, s * 0.22f, color);
		renderer_line(cx - s * 0.4f, cy + s * 0.2f, cx, cy + s * 0.6f, s * 0.22f, color);
		renderer_line(cx + s * 0.4f, cy + s * 0.2f, cx, cy + s * 0.6f, s * 0.22f, color);
	} else if (strcmp(name, "chevron_down") == 0) {
		renderer_line(cx - s * 0.5f, cy - s * 0.2f, cx, cy + s * 0.3f, s * 0.25f, color);
		renderer_line(cx, cy + s * 0.3f, cx + s * 0.5f, cy - s * 0.2f, s * 0.25f, color);
	} else if (strcmp(name, "chevron_up") == 0) {
		renderer_line(cx - s * 0.5f, cy + s * 0.2f, cx, cy - s * 0.3f, s * 0.25f, color);
		renderer_line(cx, cy - s * 0.3f, cx + s * 0.5f, cy + s * 0.2f, s * 0.25f, color);
	} else if (strcmp(name, "search") == 0) {
		renderer_border(cx - s * 0.5f, cy - s * 0.5f, s * 0.8f, s * 0.8f, s * 0.4f,
		                s * 0.18f, s * 0.18f, s * 0.18f, s * 0.18f, color);
		renderer_line(cx + s * 0.12f, cy + s * 0.12f, cx + s * 0.65f, cy + s * 0.65f, s * 0.22f, color);
	} else if (strcmp(name, "pause") == 0) {
		renderer_round_rect(cx - s * 0.45f, cy - s * 0.5f, s * 0.3f, s * 1.0f, s * 0.08f, color);
		renderer_round_rect(cx + s * 0.15f, cy - s * 0.5f, s * 0.3f, s * 1.0f, s * 0.08f, color);
	} else if (strcmp(name, "play") == 0) {
		renderer_line(cx - s * 0.35f, cy - s * 0.5f, cx - s * 0.35f, cy + s * 0.5f, s * 0.25f, color);
		renderer_line(cx - s * 0.35f, cy - s * 0.5f, cx + s * 0.45f, cy, s * 0.25f, color);
		renderer_line(cx - s * 0.35f, cy + s * 0.5f, cx + s * 0.45f, cy, s * 0.25f, color);
	} else if (strcmp(name, "battery") == 0) {
		renderer_border(cx - s * 0.7f, cy - s * 0.35f, s * 1.25f, s * 0.7f, s * 0.12f,
		                s * 0.14f, s * 0.14f, s * 0.14f, s * 0.14f, color);
		renderer_round_rect(cx + s * 0.58f, cy - s * 0.15f, s * 0.15f, s * 0.3f, s * 0.05f, color);
		renderer_round_rect(cx - s * 0.56f, cy - s * 0.23f, s * 0.7f, s * 0.46f, s * 0.06f, color);
	} else if (strcmp(name, "wifi") == 0) {
		/* Arcs simulated via concentric rings and center dot */
		renderer_border(cx - s * 0.7f, cy - s * 0.5f, s * 1.4f, s * 1.4f, s * 0.7f,
		                s * 0.16f, s * 0.16f, s * 0.16f, s * 0.16f, color);
		renderer_border(cx - s * 0.42f, cy - s * 0.22f, s * 0.84f, s * 0.84f, s * 0.42f,
		                s * 0.16f, s * 0.16f, s * 0.16f, s * 0.16f, color);
		renderer_circle(cx, cy + s * 0.32f, s * 0.16f, color);
	} else if (strcmp(name, "volume") == 0) {
		renderer_round_rect(cx - s * 0.6f, cy - s * 0.22f, s * 0.3f, s * 0.44f, s * 0.08f, color);
		renderer_line(cx - s * 0.35f, cy - s * 0.22f, cx - s * 0.05f, cy - s * 0.48f, s * 0.18f, color);
		renderer_line(cx - s * 0.35f, cy + s * 0.22f, cx - s * 0.05f, cy + s * 0.48f, s * 0.18f, color);
		renderer_line(cx - s * 0.05f, cy - s * 0.48f, cx - s * 0.05f, cy + s * 0.48f, s * 0.18f, color);
		renderer_border(cx - s * 0.3f, cy - s * 0.4f, s * 0.8f, s * 0.8f, s * 0.4f,
		                s * 0.16f, s * 0.16f, s * 0.16f, s * 0.16f, color);
	} else if (strcmp(name, "settings") == 0 || strcmp(name, "gear") == 0) {
		renderer_border(cx - s * 0.6f, cy - s * 0.6f, s * 1.2f, s * 1.2f, s * 0.6f,
		                s * 0.22f, s * 0.22f, s * 0.22f, s * 0.22f, color);
		renderer_circle(cx, cy, s * 0.22f, color);
	} else {
		renderer_circle(cx, cy, s * 0.4f, color);
	}
}

void renderer_path_fill(const struct vg_point *pts, int count, uint32_t color)
{
	if (!r_ctx.pixels || (color >> 24) == 0 || count < 3 || !pts)
		return;

	float S = r_ctx.scale;
	struct clip_box clip = r_ctx.clip_stack[r_ctx.clip_sp];
	if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1)
		return;

	float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
	for (int i = 0; i < count; i++) {
		float px = pts[i].x * S;
		float py = pts[i].y * S;
		if (px < min_x) min_x = px;
		if (px > max_x) max_x = px;
		if (py < min_y) min_y = py;
		if (py > max_y) max_y = py;
	}

	int bx0 = (int)floorf(min_x);
	int by0 = (int)floorf(min_y);
	int bx1 = (int)ceilf(max_x);
	int by1 = (int)ceilf(max_y);

	if (bx0 < clip.x0) bx0 = clip.x0;
	if (by0 < clip.y0) by0 = clip.y0;
	if (bx1 > clip.x1) bx1 = clip.x1;
	if (by1 > clip.y1) by1 = clip.y1;

	if (bx0 >= bx1 || by0 >= by1)
		return;

	damage_box(bx0, by0, bx1, by1);
	uint32_t sw = r_ctx.sw;
	uint32_t *dst = r_ctx.pixels;
	bool opaque = ((color >> 24) == 0xFF);

	for (int py = by0; py < by1; py++) {
		float Y = (float)py + 0.5f;
		float x_ints[128];
		int n_ints = 0;

		int c_start = 0;
		while (c_start < count) {
			int c_end = c_start + 1;
			while (c_end < count && pts[c_end].op != VG_OP_MOVE_TO)
				c_end++;

			int c_len = c_end - c_start;
			if (c_len >= 3) {
				for (int i = c_start; i < c_end; i++) {
					int j = (i + 1 < c_end) ? (i + 1) : c_start;
					float y0 = pts[i].y * S;
					float y1 = pts[j].y * S;
					if ((y0 <= Y && y1 > Y) || (y1 <= Y && y0 > Y)) {
						if (n_ints < 128) {
							float x0 = pts[i].x * S;
							float x1 = pts[j].x * S;
							float xi = x0 + (Y - y0) * (x1 - x0) / (y1 - y0);
							x_ints[n_ints++] = xi;
						}
					}
				}
			}
			c_start = c_end;
		}

		for (int i = 1; i < n_ints; i++) {
			float key = x_ints[i];
			int j = i - 1;
			while (j >= 0 && x_ints[j] > key) {
				x_ints[j + 1] = x_ints[j];
				j--;
			}
			x_ints[j + 1] = key;
		}

		uint32_t *row = &dst[py * sw];
		for (int i = 0; i + 1 < n_ints; i += 2) {
			float x_start = x_ints[i];
			float x_end = x_ints[i + 1];
			if (x_end <= x_start)
				continue;
			if (x_start < (float)clip.x0) x_start = (float)clip.x0;
			if (x_end > (float)clip.x1) x_end = (float)clip.x1;
			if (x_start >= x_end)
				continue;

			int ix_start = (int)floorf(x_start);
			int ix_end = (int)ceilf(x_end);
			if (ix_start < clip.x0) ix_start = clip.x0;
			if (ix_end > clip.x1) ix_end = clip.x1;

			for (int px = ix_start; px < ix_end; px++) {
				float left = (float)px;
				float right = (float)(px + 1);
				float cov_start = left < x_start ? x_start : left;
				float cov_end = right > x_end ? x_end : right;
				float cov = cov_end - cov_start;
				if (cov <= 0.0f)
					continue;
				if (cov >= 1.0f) {
					row[px] = opaque ? color : blend_color_cov(row[px], color, 255);
				} else {
					uint32_t ucov = (uint32_t)(cov * 255.0f + 0.5f);
					row[px] = blend_color_cov(row[px], color, ucov);
				}
			}
		}
	}
}

void renderer_polygon(const float *pts, int count, uint32_t color)
{
	if (!pts || count < 3)
		return;

	/* shortcut: stack-allocated temp points up to 512 vertices, upgrade to heap if huge polygon required */
	struct vg_point tmp[512];
	int n = count < 512 ? count : 512;
	for (int i = 0; i < n; i++) {
		tmp[i].x = pts[i * 2];
		tmp[i].y = pts[i * 2 + 1];
		tmp[i].op = (i == 0) ? VG_OP_MOVE_TO : VG_OP_LINE_TO;
	}
	renderer_path_fill(tmp, n, color);
}

/* Wisp native renderer implementation of wisp_renderer interface */
static bool native_renderer_init_fn(struct wl_display *display)
{
	(void)display;
	renderer_native_init();
	return true;
}

static void native_renderer_shutdown_fn(void)
{
	renderer_native_shutdown();
}

static bool native_renderer_surface_create_fn(struct widget *wgt, uint32_t width, uint32_t height)
{
	wgt->use_egl = false;
	return alloc_buffer(wgt, width, height);
}

static void native_renderer_surface_destroy_fn(struct widget *wgt)
{
	if (wgt->pixels) {
		munmap(wgt->pixels, wgt->size);
		wgt->pixels = NULL;
	}
	if (wgt->buffer) {
		wl_buffer_destroy(wgt->buffer);
		wgt->buffer = NULL;
	}
}

static void native_renderer_surface_resize_fn(struct widget *wgt, uint32_t width, uint32_t height)
{
	alloc_buffer(wgt, width, height);
}

static bool native_renderer_begin_frame_fn(struct widget *wgt)
{
	if (!wgt || !wgt->pixels)
		return false;
	int S = wgt->scale > 0 ? wgt->scale : 1;
	renderer_begin(wgt, (float)(wgt->width / S), (float)(wgt->height / S), (float)S);
	return true;
}

static void native_renderer_end_frame_fn(struct widget *wgt)
{
	renderer_end(wgt);
}

static void *native_renderer_get_vg_fn(void)
{
	return NULL;
}

const struct wisp_renderer native_renderer = {
	.name = "native_shm",
	.init = native_renderer_init_fn,
	.shutdown = native_renderer_shutdown_fn,
	.surface_create = native_renderer_surface_create_fn,
	.surface_destroy = native_renderer_surface_destroy_fn,
	.surface_resize = native_renderer_surface_resize_fn,
	.begin_frame = native_renderer_begin_frame_fn,
	.end_frame = native_renderer_end_frame_fn,
	.get_vg = native_renderer_get_vg_fn,
};
