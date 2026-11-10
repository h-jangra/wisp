#include "render/clay_render.h"
#include "render/renderer_native.h"
#include "render/clay_nanovg.h"
#include "render/text.h"
#include "layout/clay_layout.h"

void clay_render_commands(struct widget *wgt, Clay_RenderCommandArray *commands, struct rect *dmg)
{
	(void)dmg;
	if (!wgt || !commands)
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
			uint32_t c = clay_color_to_argb(rd->backgroundColor);
			if ((c >> 24) == 0)
				break;
			Clay_CornerRadius cr = rd->cornerRadius;
			if (cr.topLeft > 0.0f || cr.topRight > 0.0f || cr.bottomLeft > 0.0f || cr.bottomRight > 0.0f) {
				renderer_round_rect_varying(bx, by, bw, bh,
				                            cr.topLeft, cr.topRight,
				                            cr.bottomRight, cr.bottomLeft, c);
			} else {
				renderer_rect(bx, by, bw, bh, c);
			}
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_BORDER: {
			Clay_BorderRenderData *bd = &cmd->renderData.border;
			uint32_t c = clay_color_to_argb(bd->color);
			if ((c >> 24) == 0)
				break;
			Clay_CornerRadius cr = bd->cornerRadius;
			renderer_border(bx, by, bw, bh, cr.topLeft,
			                bd->width.top, bd->width.bottom,
			                bd->width.left, bd->width.right, c);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_TEXT: {
			Clay_TextRenderData *td = &cmd->renderData.text;
			uint32_t c = clay_color_to_argb(td->textColor);
			if ((c >> 24) == 0)
				break;
			float fsize = td->fontSize > 0 ? (float)td->fontSize : 14.0f;
			renderer_text(td->stringContents.chars, td->stringContents.length,
			              bx, by, fsize, c);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
			Clay_ImageRenderData *id = &cmd->renderData.image;
			const char *path = (const char *)id->imageData;
			if (path && *path)
				renderer_image(path, bx, by, bw, bh, id->cornerRadius.topLeft, 1.0f);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
			renderer_clip(bx, by, bw, bh);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
			renderer_clip_end();
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
			Clay_CustomRenderData *cd = &cmd->renderData.custom;
			struct custom_render_payload *p = cd ? (struct custom_render_payload *)cd->customData : NULL;
			if (!p)
				break;

			if (p->type == CUSTOM_RENDER_SHADOW && p->shadow_blur > 0.0f) {
				renderer_shadow(bx, by, bw, bh, p->radius, p->shadow_blur,
				                p->shadow_x, p->shadow_y, p->shadow_color);
			} else if (p->type == CUSTOM_RENDER_GRADIENT) {
				if (p->shadow_blur > 0.0f)
					renderer_shadow(bx, by, bw, bh, p->radius, p->shadow_blur,
					                p->shadow_x, p->shadow_y, p->shadow_color);
				renderer_gradient(bx, by, bw, bh, p->radius, p->color1, p->color2, p->is_radial);
			} else if (p->type == CUSTOM_RENDER_BUTTON) {
				float s = p->scale > 0.0f ? p->scale : 1.0f;
				float nbw = bw * s, nbh = bh * s;
				float nbx = bx + (bw - nbw) * 0.5f, nby = by + (bh - nbh) * 0.5f;

				if (p->shadow_blur > 0.0f)
					renderer_shadow(nbx, nby, nbw, nbh, p->radius * s, p->shadow_blur * s,
					                p->shadow_x, p->shadow_y, p->shadow_color);
				if (p->radius > 0.0f)
					renderer_round_rect(nbx, nby, nbw, nbh, p->radius * s, p->color1);
				else
					renderer_rect(nbx, nby, nbw, nbh, p->color1);

				if (p->is_focused) {
					renderer_border(nbx - 2.0f, nby - 2.0f, nbw + 4.0f, nbh + 4.0f,
					                p->radius > 0.0f ? p->radius * s + 2.0f : 2.0f,
					                1.5f, 1.5f, 1.5f, 1.5f, 0xFF89B4FAu);
				}
			} else if (p->type == CUSTOM_RENDER_SWITCH) {
				float r = bh * 0.5f;
				renderer_round_rect(bx, by, bw, bh, r, p->color1);

				float thumb_rad = (bh - 6.0f) * 0.5f;
				float travel = bw - bh;
				float val = p->value < 0.0f ? 0.0f : (p->value > 1.0f ? 1.0f : p->value);
				float thumb_cx = bx + r + val * travel;
				float thumb_cy = by + r;

				renderer_shadow(thumb_cx - thumb_rad, thumb_cy - thumb_rad,
				                thumb_rad * 2.0f, thumb_rad * 2.0f,
				                thumb_rad, 4.0f, 0.0f, 1.0f, 0x66000000u);
				renderer_circle(thumb_cx, thumb_cy, thumb_rad, p->color2);
			} else if (p->type == CUSTOM_RENDER_CHECKBOX) {
				float r = p->radius > 0.0f ? p->radius : 4.0f;
				renderer_round_rect(bx, by, bw, bh, r, p->color1);
				renderer_border(bx, by, bw, bh, r, 1.2f, 1.2f, 1.2f, 1.2f, p->color2);

				if (p->value > 0.0f) {
					float val = p->value > 1.0f ? 1.0f : p->value;
					renderer_line(bx + bw * 0.22f, by + bh * 0.52f,
					              bx + bw * 0.42f, by + bh * 0.72f, 2.2f * val, p->color3);
					renderer_line(bx + bw * 0.42f, by + bh * 0.72f,
					              bx + bw * 0.78f, by + bh * 0.28f, 2.2f * val, p->color3);
				}
			} else if (p->type == CUSTOM_RENDER_SLIDER) {
				float track_h = 6.0f;
				float track_y = by + (bh - track_h) * 0.5f;
				renderer_round_rect(bx, track_y, bw, track_h, 3.0f, p->color1);

				float val = p->value < 0.0f ? 0.0f : (p->value > 1.0f ? 1.0f : p->value);
				float fill_w = bw * val;
				if (fill_w > 0.0f)
					renderer_round_rect(bx, track_y, fill_w, track_h, 3.0f, p->color2);

				float thumb_cx = bx + fill_w;
				float thumb_cy = by + bh * 0.5f;
				float thumb_r = 8.0f * (p->scale > 0.0f ? p->scale : 1.0f);

				renderer_shadow(thumb_cx - thumb_r, thumb_cy - thumb_r,
				                thumb_r * 2.0f, thumb_r * 2.0f,
				                thumb_r, 4.0f, 0.0f, 1.0f, 0x80000000u);
				renderer_circle(thumb_cx, thumb_cy, thumb_r, p->color3);
			} else if (p->type == CUSTOM_RENDER_PROGRESS) {
				float r = p->radius > 0.0f ? p->radius : bh * 0.5f;
				renderer_round_rect(bx, by, bw, bh, r, p->color1);

				float val = p->value < 0.0f ? 0.0f : (p->value > 1.0f ? 1.0f : p->value);
				float fw = bw * val;
				if (fw > 0.0f)
					renderer_round_rect(bx, by, fw, bh, r, p->color2);
			} else if (p->type == CUSTOM_RENDER_ICON) {
				renderer_icon(p->text, bx, by, bw, bh, p->color1);
			} else if (p->type == CUSTOM_RENDER_INPUT) {
				float r = p->radius > 0.0f ? p->radius : 6.0f;
				uint32_t bdr_col = p->is_focused ? p->color2 : p->color3;
				renderer_round_rect(bx, by, bw, bh, r, p->color1);
				renderer_border(bx, by, bw, bh, r,
				                p->is_focused ? 1.8f : 1.0f,
				                p->is_focused ? 1.8f : 1.0f,
				                p->is_focused ? 1.8f : 1.0f,
				                p->is_focused ? 1.8f : 1.0f,
				                bdr_col);

				if (p->is_focused) {
					int cur_tw = 0;
					if (p->text[0] != '\0')
						cur_tw = text_measure(p->text, 13);
					renderer_rect(bx + 12.0f + (float)cur_tw, by + 7.0f, 1.5f, bh - 14.0f, bdr_col);
				}
			}
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START:
		case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:
		default:
			break;
		}
	}
}
