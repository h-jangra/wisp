#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "core/types.h"
#include "render/renderer.h"

void renderer_native_init(void);
void renderer_native_shutdown(void);

void renderer_begin(struct widget *wgt, float width, float height, float scale);
void renderer_end(struct widget *wgt);

void renderer_clip(float x, float y, float w, float h);
void renderer_clip_end(void);

void renderer_rect(float x, float y, float w, float h, uint32_t color);
void renderer_round_rect(float x, float y, float w, float h, float radius, uint32_t color);
void renderer_round_rect_varying(float x, float y, float w, float h,
                                float rtl, float rtr, float rbr, float rbl,
                                uint32_t color);
void renderer_border(float x, float y, float w, float h,
                     float radius, float top_w, float bot_w, float left_w, float right_w,
                     uint32_t color);
void renderer_line(float x1, float y1, float x2, float y2, float width, uint32_t color);
void renderer_circle(float cx, float cy, float radius, uint32_t color);
void renderer_text(const char *text, int len, float x, float y, float size, uint32_t color);
struct screen_image;
void renderer_image(const char *path, float x, float y, float w, float h, float radius, float alpha);
void renderer_screen_image(struct screen_image *img, float x, float y, float w, float h, float radius, float alpha);

void renderer_shadow(float x, float y, float w, float h, float radius, float blur,
                     float sx, float sy, uint32_t color);
void renderer_gradient(float x, float y, float w, float h, float radius,
                      uint32_t color1, uint32_t color2, bool radial);
void renderer_icon(const char *name, float x, float y, float w, float h, uint32_t color);
struct vg_point;
void renderer_polygon(const float *pts, int count, uint32_t color);
void renderer_path_fill(const struct vg_point *pts, int count, uint32_t color);
