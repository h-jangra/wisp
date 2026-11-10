#include "render/clay_nanovg.h"
#include "core/types.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAX_CUSTOM_PAYLOADS 256
static struct custom_render_payload custom_payloads[MAX_CUSTOM_PAYLOADS];
static int custom_payload_count = 0;

#define MAX_IMAGE_CACHE 64
struct image_cache_entry {
	char path[256];
	int handle;
};
static struct image_cache_entry image_cache[MAX_IMAGE_CACHE];
static int image_cache_count = 0;

struct custom_render_payload *custom_payload_alloc(void)
{
	if (custom_payload_count >= MAX_CUSTOM_PAYLOADS)
		return NULL;
	struct custom_render_payload *p = &custom_payloads[custom_payload_count++];
	memset(p, 0, sizeof(*p));
	p->scale = 1.0f;
	p->opacity = 1.0f;
	return p;
}

void custom_payload_reset(void)
{
	custom_payload_count = 0;
}

void nanovg_image_cache_clear(NVGcontext *vg)
{
	if (!vg)
		return;
	for (int i = 0; i < image_cache_count; i++) {
		if (image_cache[i].handle > 0)
			nvgDeleteImage(vg, image_cache[i].handle);
	}
	image_cache_count = 0;
}

static int get_or_load_image(NVGcontext *vg, const char *path)
{
	if (!vg || !path || !*path)
		return 0;

	for (int i = 0; i < image_cache_count; i++) {
		if (strcmp(image_cache[i].path, path) == 0)
			return image_cache[i].handle;
	}

	if (image_cache_count >= MAX_IMAGE_CACHE)
		return 0;

	int handle = nvgCreateImage(vg, path, 0);
	if (handle > 0) {
		snprintf(image_cache[image_cache_count].path, sizeof(image_cache[image_cache_count].path), "%s", path);
		image_cache[image_cache_count].handle = handle;
		image_cache_count++;
		return handle;
	}
	return 0;
}

static inline NVGcolor nvg_from_argb(uint32_t argb)
{
	unsigned char a = (argb >> 24) & 0xFF;
	unsigned char r = (argb >> 16) & 0xFF;
	unsigned char g = (argb >> 8) & 0xFF;
	unsigned char b = argb & 0xFF;
	return nvgRGBA(r, g, b, a);
}

static void render_vector_icon(NVGcontext *vg, const char *name, float x, float y, float w, float h, NVGcolor color)
{
	if (!vg || !name)
		return;

	float cx = x + w * 0.5f;
	float cy = y + h * 0.5f;
	float s = (w < h ? w : h) * 0.5f;

	nvgBeginPath(vg);

	if (strcmp(name, "check") == 0) {
		nvgMoveTo(vg, cx - s * 0.6f, cy);
		nvgLineTo(vg, cx - s * 0.15f, cy + s * 0.45f);
		nvgLineTo(vg, cx + s * 0.65f, cy - s * 0.45f);
		nvgStrokeWidth(vg, s * 0.28f);
		nvgStrokeColor(vg, color);
		nvgLineCap(vg, NVG_ROUND);
		nvgLineJoin(vg, NVG_ROUND);
		nvgStroke(vg);
	} else if (strcmp(name, "close") == 0 || strcmp(name, "x") == 0) {
		nvgMoveTo(vg, cx - s * 0.5f, cy - s * 0.5f);
		nvgLineTo(vg, cx + s * 0.5f, cy + s * 0.5f);
		nvgMoveTo(vg, cx + s * 0.5f, cy - s * 0.5f);
		nvgLineTo(vg, cx - s * 0.5f, cy + s * 0.5f);
		nvgStrokeWidth(vg, s * 0.25f);
		nvgStrokeColor(vg, color);
		nvgLineCap(vg, NVG_ROUND);
		nvgStroke(vg);
	} else if (strcmp(name, "arrow_up") == 0) {
		nvgMoveTo(vg, cx, cy + s * 0.6f);
		nvgLineTo(vg, cx, cy - s * 0.6f);
		nvgMoveTo(vg, cx - s * 0.4f, cy - s * 0.2f);
		nvgLineTo(vg, cx, cy - s * 0.6f);
		nvgLineTo(vg, cx + s * 0.4f, cy - s * 0.2f);
		nvgStrokeWidth(vg, s * 0.22f);
		nvgStrokeColor(vg, color);
		nvgLineCap(vg, NVG_ROUND);
		nvgStroke(vg);
	} else if (strcmp(name, "arrow_down") == 0) {
		nvgMoveTo(vg, cx, cy - s * 0.6f);
		nvgLineTo(vg, cx, cy + s * 0.6f);
		nvgMoveTo(vg, cx - s * 0.4f, cy + s * 0.2f);
		nvgLineTo(vg, cx, cy + s * 0.6f);
		nvgLineTo(vg, cx + s * 0.4f, cy + s * 0.2f);
		nvgStrokeWidth(vg, s * 0.22f);
		nvgStrokeColor(vg, color);
		nvgLineCap(vg, NVG_ROUND);
		nvgStroke(vg);
	} else if (strcmp(name, "chevron_down") == 0) {
		nvgMoveTo(vg, cx - s * 0.5f, cy - s * 0.2f);
		nvgLineTo(vg, cx, cy + s * 0.3f);
		nvgLineTo(vg, cx + s * 0.5f, cy - s * 0.2f);
		nvgStrokeWidth(vg, s * 0.25f);
		nvgStrokeColor(vg, color);
		nvgLineCap(vg, NVG_ROUND);
		nvgLineJoin(vg, NVG_ROUND);
		nvgStroke(vg);
	} else if (strcmp(name, "chevron_up") == 0) {
		nvgMoveTo(vg, cx - s * 0.5f, cy + s * 0.2f);
		nvgLineTo(vg, cx, cy - s * 0.3f);
		nvgLineTo(vg, cx + s * 0.5f, cy + s * 0.2f);
		nvgStrokeWidth(vg, s * 0.25f);
		nvgStrokeColor(vg, color);
		nvgLineCap(vg, NVG_ROUND);
		nvgLineJoin(vg, NVG_ROUND);
		nvgStroke(vg);
	} else if (strcmp(name, "search") == 0) {
		nvgCircle(vg, cx - s * 0.2f, cy - s * 0.2f, s * 0.45f);
		nvgStrokeWidth(vg, s * 0.2f);
		nvgStrokeColor(vg, color);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgMoveTo(vg, cx + s * 0.15f, cy + s * 0.15f);
		nvgLineTo(vg, cx + s * 0.65f, cy + s * 0.65f);
		nvgStrokeWidth(vg, s * 0.22f);
		nvgStrokeColor(vg, color);
		nvgLineCap(vg, NVG_ROUND);
		nvgStroke(vg);
	} else if (strcmp(name, "play") == 0) {
		nvgMoveTo(vg, cx - s * 0.35f, cy - s * 0.55f);
		nvgLineTo(vg, cx + s * 0.55f, cy);
		nvgLineTo(vg, cx - s * 0.35f, cy + s * 0.55f);
		nvgClosePath(vg);
		nvgFillColor(vg, color);
		nvgFill(vg);
	} else if (strcmp(name, "pause") == 0) {
		nvgRect(vg, cx - s * 0.45f, cy - s * 0.5f, s * 0.3f, s * 1.0f);
		nvgRect(vg, cx + s * 0.15f, cy - s * 0.5f, s * 0.3f, s * 1.0f);
		nvgFillColor(vg, color);
		nvgFill(vg);
	} else if (strcmp(name, "settings") == 0 || strcmp(name, "gear") == 0) {
		nvgCircle(vg, cx, cy, s * 0.65f);
		nvgStrokeWidth(vg, s * 0.22f);
		nvgStrokeColor(vg, color);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, s * 0.25f);
		nvgFillColor(vg, color);
		nvgFill(vg);
	} else if (strcmp(name, "wifi") == 0) {
		nvgArc(vg, cx, cy + s * 0.3f, s * 0.8f, -2.4f, -0.74f, NVG_CW);
		nvgStrokeWidth(vg, s * 0.18f);
		nvgStrokeColor(vg, color);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgArc(vg, cx, cy + s * 0.3f, s * 0.5f, -2.4f, -0.74f, NVG_CW);
		nvgStrokeWidth(vg, s * 0.18f);
		nvgStrokeColor(vg, color);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy + s * 0.35f, s * 0.15f);
		nvgFillColor(vg, color);
		nvgFill(vg);
	} else if (strcmp(name, "battery") == 0) {
		nvgRoundedRect(vg, cx - s * 0.7f, cy - s * 0.35f, s * 1.25f, s * 0.7f, s * 0.12f);
		nvgStrokeWidth(vg, s * 0.15f);
		nvgStrokeColor(vg, color);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, cx + s * 0.6f, cy - s * 0.15f, s * 0.15f, s * 0.3f, s * 0.05f);
		nvgFillColor(vg, color);
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, cx - s * 0.58f, cy - s * 0.25f, s * 0.7f, s * 0.5f, s * 0.08f);
		nvgFillColor(vg, color);
		nvgFill(vg);
	} else if (strcmp(name, "volume") == 0) {
		nvgMoveTo(vg, cx - s * 0.6f, cy - s * 0.25f);
		nvgLineTo(vg, cx - s * 0.3f, cy - s * 0.25f);
		nvgLineTo(vg, cx, cy - s * 0.55f);
		nvgLineTo(vg, cx, cy + s * 0.55f);
		nvgLineTo(vg, cx - s * 0.3f, cy + s * 0.25f);
		nvgLineTo(vg, cx - s * 0.6f, cy + s * 0.25f);
		nvgClosePath(vg);
		nvgFillColor(vg, color);
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgArc(vg, cx, cy, s * 0.45f, -0.7f, 0.7f, NVG_CW);
		nvgStrokeWidth(vg, s * 0.18f);
		nvgStrokeColor(vg, color);
		nvgStroke(vg);
	} else {
		/* Default: Circle dot */
		nvgCircle(vg, cx, cy, s * 0.4f);
		nvgFillColor(vg, color);
		nvgFill(vg);
	}
}

void clay_render_nanovg(NVGcontext *vg, struct widget *wgt, Clay_RenderCommandArray *commands)
{
	(void)wgt;
	if (!vg || !commands)
		return;

	for (int32_t i = 0; i < commands->length; i++) {
		Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(commands, i);
		if (!cmd)
			continue;

		float bx = cmd->boundingBox.x;
		float by = cmd->boundingBox.y;
		float bw = cmd->boundingBox.width;
		float bh = cmd->boundingBox.height;

		switch (cmd->commandType) {
		case CLAY_RENDER_COMMAND_TYPE_NONE:
			break;

		case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
			Clay_RectangleRenderData *rd = &cmd->renderData.rectangle;
			Clay_Color c = rd->backgroundColor;
			if (c.a <= 0.0f)
				break;

			NVGcolor col = nvgRGBA((unsigned char)c.r, (unsigned char)c.g,
					       (unsigned char)c.b, (unsigned char)c.a);
			Clay_CornerRadius cr = rd->cornerRadius;

			nvgBeginPath(vg);
			if (cr.topLeft > 0.0f || cr.topRight > 0.0f ||
			    cr.bottomLeft > 0.0f || cr.bottomRight > 0.0f) {
				nvgRoundedRectVarying(vg, bx, by, bw, bh,
						      cr.topLeft, cr.topRight,
						      cr.bottomRight, cr.bottomLeft);
			} else {
				nvgRect(vg, bx, by, bw, bh);
			}
			nvgFillColor(vg, col);
			nvgFill(vg);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_BORDER: {
			Clay_BorderRenderData *bd = &cmd->renderData.border;
			Clay_Color c = bd->color;
			if (c.a <= 0.0f)
				break;

			NVGcolor col = nvgRGBA((unsigned char)c.r, (unsigned char)c.g,
					       (unsigned char)c.b, (unsigned char)c.a);
			float top_w = (float)bd->width.top;
			float bot_w = (float)bd->width.bottom;
			float left_w = (float)bd->width.left;
			float right_w = (float)bd->width.right;
			Clay_CornerRadius cr = bd->cornerRadius;

			if (top_w > 0.0f && top_w == bot_w && top_w == left_w && top_w == right_w) {
				float inset = top_w * 0.5f;
				nvgBeginPath(vg);
				if (cr.topLeft > 0.0f || cr.topRight > 0.0f ||
				    cr.bottomLeft > 0.0f || cr.bottomRight > 0.0f) {
					nvgRoundedRectVarying(vg, bx + inset, by + inset,
							      bw - inset * 2.0f, bh - inset * 2.0f,
							      cr.topLeft, cr.topRight,
							      cr.bottomRight, cr.bottomLeft);
				} else {
					nvgRect(vg, bx + inset, by + inset,
						bw - inset * 2.0f, bh - inset * 2.0f);
				}
				nvgStrokeWidth(vg, top_w);
				nvgStrokeColor(vg, col);
				nvgStroke(vg);
			} else {
				if (top_w > 0.0f) {
					nvgBeginPath(vg);
					nvgRect(vg, bx, by, bw, top_w);
					nvgFillColor(vg, col);
					nvgFill(vg);
				}
				if (bot_w > 0.0f) {
					nvgBeginPath(vg);
					nvgRect(vg, bx, by + bh - bot_w, bw, bot_w);
					nvgFillColor(vg, col);
					nvgFill(vg);
				}
				if (left_w > 0.0f) {
					nvgBeginPath(vg);
					nvgRect(vg, bx, by, left_w, bh);
					nvgFillColor(vg, col);
					nvgFill(vg);
				}
				if (right_w > 0.0f) {
					nvgBeginPath(vg);
					nvgRect(vg, bx + bw - right_w, by, right_w, bh);
					nvgFillColor(vg, col);
					nvgFill(vg);
				}
			}
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_TEXT: {
			Clay_TextRenderData *td = &cmd->renderData.text;
			Clay_Color c = td->textColor;
			if (c.a <= 0.0f)
				break;

			NVGcolor col = nvgRGBA((unsigned char)c.r, (unsigned char)c.g,
					       (unsigned char)c.b, (unsigned char)c.a);
			float fontSize = td->fontSize > 0 ? (float)td->fontSize : 14.0f;
			nvgFontSize(vg, fontSize);
			nvgFontFace(vg, "default");
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, col);
			nvgText(vg, bx, by, td->stringContents.chars,
				td->stringContents.chars + td->stringContents.length);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
			Clay_ImageRenderData *imd = &cmd->renderData.image;
			const char *path = (const char *)imd->imageData;
			if (!path)
				break;

			int handle = get_or_load_image(vg, path);
			if (handle > 0) {
				Clay_CornerRadius cr = imd->cornerRadius;
				NVGpaint imgPaint = nvgImagePattern(vg, bx, by, bw, bh, 0.0f, handle, 1.0f);
				nvgBeginPath(vg);
				if (cr.topLeft > 0.0f || cr.topRight > 0.0f ||
				    cr.bottomLeft > 0.0f || cr.bottomRight > 0.0f) {
					nvgRoundedRectVarying(vg, bx, by, bw, bh,
							      cr.topLeft, cr.topRight,
							      cr.bottomRight, cr.bottomLeft);
				} else {
					nvgRect(vg, bx, by, bw, bh);
				}
				nvgFillPaint(vg, imgPaint);
				nvgFill(vg);
			}
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
			nvgSave(vg);
			nvgIntersectScissor(vg, bx, by, bw, bh);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
			nvgRestore(vg);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
			Clay_CustomRenderData *cd = &cmd->renderData.custom;
			struct custom_render_payload *p = cd ? (struct custom_render_payload *)cd->customData : NULL;
			if (!p)
				break;

			/* Optional global opacity and rotation */
			bool has_transform = (p->rotation != 0.0f || (p->scale != 1.0f && p->type != CUSTOM_RENDER_BUTTON) || p->opacity < 1.0f);
			if (has_transform) {
				nvgSave(vg);
				if (p->opacity < 1.0f)
					nvgGlobalAlpha(vg, p->opacity);
				if (p->rotation != 0.0f || p->scale != 1.0f) {
					float cx = bx + bw * 0.5f;
					float cy = by + bh * 0.5f;
					nvgTranslate(vg, cx, cy);
					if (p->rotation != 0.0f)
						nvgRotate(vg, p->rotation * 0.0174532925f);
					if (p->scale != 1.0f)
						nvgScale(vg, p->scale, p->scale);
					nvgTranslate(vg, -cx, -cy);
				}
			}

			if (p->type == CUSTOM_RENDER_SHADOW && p->shadow_blur > 0.0f) {
				NVGcolor col = nvg_from_argb(p->shadow_color);
				NVGcolor trans = nvgRGBA(0, 0, 0, 0);
				NVGpaint shadowPaint = nvgBoxGradient(vg, bx + p->shadow_x, by + p->shadow_y,
								      bw, bh, p->radius, p->shadow_blur,
								      col, trans);
				nvgBeginPath(vg);
				nvgRect(vg, bx + p->shadow_x - p->shadow_blur * 2.0f,
					by + p->shadow_y - p->shadow_blur * 2.0f,
					bw + p->shadow_blur * 4.0f,
					bh + p->shadow_blur * 4.0f);
				nvgFillPaint(vg, shadowPaint);
				nvgFill(vg);
			} else if (p->type == CUSTOM_RENDER_GRADIENT) {
				if (p->shadow_blur > 0.0f) {
					NVGcolor scol = nvg_from_argb(p->shadow_color);
					NVGcolor strans = nvgRGBA(0, 0, 0, 0);
					NVGpaint shadowPaint = nvgBoxGradient(vg, bx + p->shadow_x, by + p->shadow_y,
									      bw, bh, p->radius, p->shadow_blur,
									      scol, strans);
					nvgBeginPath(vg);
					nvgRect(vg, bx + p->shadow_x - p->shadow_blur * 2.0f,
						by + p->shadow_y - p->shadow_blur * 2.0f,
						bw + p->shadow_blur * 4.0f,
						bh + p->shadow_blur * 4.0f);
					nvgFillPaint(vg, shadowPaint);
					nvgFill(vg);
				}
				NVGcolor c1 = nvg_from_argb(p->color1);
				NVGcolor c2 = nvg_from_argb(p->color2);
				NVGpaint grad;
				if (p->is_radial) {
					grad = nvgRadialGradient(vg, bx + bw * 0.5f, by + bh * 0.5f,
								 0.0f, bw * 0.5f, c1, c2);
				} else {
					grad = nvgLinearGradient(vg, bx, by, bx, by + bh, c1, c2);
				}
				nvgBeginPath(vg);
				if (p->radius > 0.0f)
					nvgRoundedRect(vg, bx, by, bw, bh, p->radius);
				else
					nvgRect(vg, bx, by, bw, bh);
				nvgFillPaint(vg, grad);
				nvgFill(vg);
			} else if (p->type == CUSTOM_RENDER_BUTTON) {
				float scale = p->scale > 0.0f ? p->scale : 1.0f;
				float cx = bx + bw * 0.5f;
				float cy = by + bh * 0.5f;

				nvgSave(vg);
				if (scale != 1.0f) {
					nvgTranslate(vg, cx, cy);
					nvgScale(vg, scale, scale);
					nvgTranslate(vg, -cx, -cy);
				}

				if (p->shadow_blur > 0.0f) {
					NVGcolor scol = nvg_from_argb(p->shadow_color);
					NVGcolor strans = nvgRGBA(0, 0, 0, 0);
					NVGpaint sp = nvgBoxGradient(vg, bx + p->shadow_x, by + p->shadow_y,
								    bw, bh, p->radius, p->shadow_blur,
								    scol, strans);
					nvgBeginPath(vg);
					nvgRect(vg, bx + p->shadow_x - p->shadow_blur * 2.0f,
						by + p->shadow_y - p->shadow_blur * 2.0f,
						bw + p->shadow_blur * 4.0f,
						bh + p->shadow_blur * 4.0f);
					nvgFillPaint(vg, sp);
					nvgFill(vg);
				}

				NVGcolor col = nvg_from_argb(p->color1);
				nvgBeginPath(vg);
				if (p->radius > 0.0f)
					nvgRoundedRect(vg, bx, by, bw, bh, p->radius);
				else
					nvgRect(vg, bx, by, bw, bh);
				nvgFillColor(vg, col);
				nvgFill(vg);

				if (p->is_focused) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, bx - 2.0f, by - 2.0f, bw + 4.0f, bh + 4.0f, (p->radius > 0.0f ? p->radius + 2.0f : 2.0f));
					nvgStrokeColor(vg, nvgRGBA(0x89, 0xB4, 0xFA, 255));
					nvgStrokeWidth(vg, 1.5f);
					nvgStroke(vg);
				}

				nvgRestore(vg);
			} else if (p->type == CUSTOM_RENDER_SWITCH) {
				/* Pill toggle track */
				float r = bh * 0.5f;
				NVGcolor track_col = nvg_from_argb(p->color1);
				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx, by, bw, bh, r);
				nvgFillColor(vg, track_col);
				nvgFill(vg);

				/* Animated thumb position */
				float thumb_rad = (bh - 6.0f) * 0.5f;
				float travel = bw - bh;
				float val = p->value < 0.0f ? 0.0f : (p->value > 1.0f ? 1.0f : p->value);
				float thumb_cx = bx + r + val * travel;
				float thumb_cy = by + r;

				/* Thumb shadow */
				NVGpaint sp = nvgBoxGradient(vg, thumb_cx - thumb_rad, thumb_cy - thumb_rad + 1.0f,
							     thumb_rad * 2.0f, thumb_rad * 2.0f,
							     thumb_rad, 4.0f,
							     nvgRGBA(0, 0, 0, 100), nvgRGBA(0, 0, 0, 0));
				nvgBeginPath(vg);
				nvgRect(vg, thumb_cx - thumb_rad - 4.0f, thumb_cy - thumb_rad - 3.0f,
					thumb_rad * 2.0f + 8.0f, thumb_rad * 2.0f + 8.0f);
				nvgFillPaint(vg, sp);
				nvgFill(vg);

				/* Thumb circle */
				NVGcolor thumb_col = nvg_from_argb(p->color2);
				nvgBeginPath(vg);
				nvgCircle(vg, thumb_cx, thumb_cy, thumb_rad);
				nvgFillColor(vg, thumb_col);
				nvgFill(vg);
			} else if (p->type == CUSTOM_RENDER_CHECKBOX) {
				float r = p->radius > 0.0f ? p->radius : 4.0f;
				NVGcolor box_col = nvg_from_argb(p->color1);
				NVGcolor border_col = nvg_from_argb(p->color2);
				NVGcolor check_col = nvg_from_argb(p->color3);

				/* Box fill */
				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx, by, bw, bh, r);
				nvgFillColor(vg, box_col);
				nvgFill(vg);

				/* Box border */
				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx + 0.5f, by + 0.5f, bw - 1.0f, bh - 1.0f, r);
				nvgStrokeWidth(vg, 1.2f);
				nvgStrokeColor(vg, border_col);
				nvgStroke(vg);

				/* Vector Checkmark */
				if (p->value > 0.0f) {
					nvgBeginPath(vg);
					nvgMoveTo(vg, bx + bw * 0.22f, by + bh * 0.52f);
					nvgLineTo(vg, bx + bw * 0.42f, by + bh * 0.72f);
					nvgLineTo(vg, bx + bw * 0.78f, by + bh * 0.28f);
					nvgStrokeWidth(vg, 2.2f * (p->value > 1.0f ? 1.0f : p->value));
					nvgStrokeColor(vg, check_col);
					nvgLineCap(vg, NVG_ROUND);
					nvgLineJoin(vg, NVG_ROUND);
					nvgStroke(vg);
				}
			} else if (p->type == CUSTOM_RENDER_SLIDER) {
				float track_h = 6.0f;
				float track_y = by + (bh - track_h) * 0.5f;
				NVGcolor bg_col = nvg_from_argb(p->color1);
				NVGcolor fill_col = nvg_from_argb(p->color2);
				NVGcolor thumb_col = nvg_from_argb(p->color3);

				/* Track background */
				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx, track_y, bw, track_h, 3.0f);
				nvgFillColor(vg, bg_col);
				nvgFill(vg);

				/* Active fill portion */
				float val = p->value < 0.0f ? 0.0f : (p->value > 1.0f ? 1.0f : p->value);
				float fill_w = bw * val;
				if (fill_w > 0.0f) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, bx, track_y, fill_w, track_h, 3.0f);
					nvgFillColor(vg, fill_col);
					nvgFill(vg);
				}

				/* Thumb */
				float thumb_cx = bx + fill_w;
				float thumb_cy = by + bh * 0.5f;
				float thumb_r = 8.0f * (p->scale > 0.0f ? p->scale : 1.0f);

				NVGpaint sp = nvgBoxGradient(vg, thumb_cx - thumb_r, thumb_cy - thumb_r + 1.0f,
							     thumb_r * 2.0f, thumb_r * 2.0f,
							     thumb_r, 4.0f,
							     nvgRGBA(0, 0, 0, 120), nvgRGBA(0, 0, 0, 0));
				nvgBeginPath(vg);
				nvgRect(vg, thumb_cx - thumb_r - 4.0f, thumb_cy - thumb_r - 3.0f,
					thumb_r * 2.0f + 8.0f, thumb_r * 2.0f + 8.0f);
				nvgFillPaint(vg, sp);
				nvgFill(vg);

				nvgBeginPath(vg);
				nvgCircle(vg, thumb_cx, thumb_cy, thumb_r);
				nvgFillColor(vg, thumb_col);
				nvgFill(vg);
			} else if (p->type == CUSTOM_RENDER_PROGRESS) {
				float r = p->radius > 0.0f ? p->radius : bh * 0.5f;
				NVGcolor bg_col = nvg_from_argb(p->color1);
				NVGcolor fill_col = nvg_from_argb(p->color2);

				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx, by, bw, bh, r);
				nvgFillColor(vg, bg_col);
				nvgFill(vg);

				float val = p->value < 0.0f ? 0.0f : (p->value > 1.0f ? 1.0f : p->value);
				float fw = bw * val;
				if (fw > 0.0f) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, bx, by, fw, bh, r);
					nvgFillColor(vg, fill_col);
					nvgFill(vg);
				}
			} else if (p->type == CUSTOM_RENDER_ICON) {
				NVGcolor col = nvg_from_argb(p->color1);
				render_vector_icon(vg, p->text, bx, by, bw, bh, col);
			} else if (p->type == CUSTOM_RENDER_INPUT) {
				float r = p->radius > 0.0f ? p->radius : 6.0f;
				NVGcolor bg_col = nvg_from_argb(p->color1);
				NVGcolor bdr_col = nvg_from_argb(p->is_focused ? p->color2 : p->color3);

				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx, by, bw, bh, r);
				nvgFillColor(vg, bg_col);
				nvgFill(vg);

				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx + 0.5f, by + 0.5f, bw - 1.0f, bh - 1.0f, r);
				nvgStrokeWidth(vg, p->is_focused ? 1.8f : 1.0f);
				nvgStrokeColor(vg, bdr_col);
				nvgStroke(vg);

				/* Blinking cursor bar */
				if (p->is_focused) {
					float text_w = 0.0f;
					if (p->text[0] != '\0') {
						float bounds[4];
						nvgFontSize(vg, 13.0f);
						nvgFontFace(vg, "default");
						text_w = nvgTextBounds(vg, 0, 0, p->text, NULL, bounds);
					}
					float cur_x = bx + 12.0f + text_w;
					float cur_y = by + 7.0f;
					float cur_h = bh - 14.0f;
					nvgBeginPath(vg);
					nvgRect(vg, cur_x, cur_y, 1.5f, cur_h);
					nvgFillColor(vg, bdr_col);
					nvgFill(vg);
				}
			}

			if (has_transform)
				nvgRestore(vg);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START:
		case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:
		default:
			break;
		}
	}
}
