#include "render/clay_render.h"
#include "render/renderer_native.h"
#include "render/clay_nanovg.h"
#include "render/text.h"
#include "render/vector.h"
#include "services/screen.h"
#include "layout/clay_layout.h"
#include "core/interaction.h"
#include <stdlib.h>
#include <string.h>

static void clay_render_one(struct widget *wgt, Clay_RenderCommand *cmd)
{
	if (!wgt || !cmd)
		return;

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
		struct screen_image *simg = screen_image_from_ptr(id->imageData);
		if (simg) {
			renderer_screen_image(simg, bx, by, bw, bh, id->cornerRadius.topLeft, 1.0f);
		} else {
			const char *path = (const char *)id->imageData;
			if (path && *path)
				renderer_image(path, bx, by, bw, bh, id->cornerRadius.topLeft, 1.0f);
		}
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
			                thumb_rad * 2, thumb_rad * 2,
			                thumb_rad, 4.0f, 0.0f, 1.0f, 0x66000000u);
			renderer_circle(thumb_cx, thumb_cy, thumb_rad, p->color2);

			if (p->is_focused) {
				renderer_border(bx - 2.0f, by - 2.0f, bw + 4.0f, bh + 4.0f,
				                r + 2.0f, 1.5f, 1.5f, 1.5f, 1.5f, 0xFF89B4FAu);
			}
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

			if (p->is_focused) {
				renderer_border(bx - 2.0f, by - 2.0f, bw + 4.0f, bh + 4.0f,
				                r + 2.0f, 1.5f, 1.5f, 1.5f, 1.5f, 0xFF89B4FAu);
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
			                thumb_r * 2, thumb_r * 2,
			                thumb_r, 4.0f, 0.0f, 1.0f, 0x80000000u);
			renderer_circle(thumb_cx, thumb_cy, thumb_r, p->color3);

			if (p->is_focused) {
				renderer_border(bx - 2.0f, by - 2.0f, bw + 4.0f, bh + 4.0f,
				                4.0f, 1.5f, 1.5f, 1.5f, 1.5f, 0xFF89B4FAu);
			}
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
			uint32_t bdr_col = p->is_focused ? p->color2 : (p->is_hovered ? (p->color4 ? p->color4 : p->color2) : p->color3);
			renderer_round_rect(bx, by, bw, bh, r, p->color1);
			float bw_val = p->is_focused ? 1.8f : (p->is_hovered ? 1.2f : 1.0f);
			renderer_border(bx, by, bw, bh, r, bw_val, bw_val, bw_val, bw_val, bdr_col);

			if (p->is_focused) {
				int cur_tw = 0;
				int font_sz = (p->min_val > 0.0f) ? (int)p->min_val : 13;
				if (p->text[0] != '\0')
					cur_tw = text_measure(p->text, font_sz);
				float cur_h = (float)font_sz + 2.0f;
				float cur_y = by + (bh - cur_h) * 0.5f;
				renderer_rect(bx + 12.0f + (float)cur_tw, cur_y, 1.5f, cur_h, bdr_col);
			}
		} else if (p->type == CUSTOM_RENDER_CANVAS) {
			wisp_vg_render_canvas(wgt, p, bx, by, bw, bh);
		}
		break;
	}

	case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START:
	case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:
	default:
		break;
	}
}

void clay_render_commands(struct widget *wgt, Clay_RenderCommandArray *commands, struct rect *dmg)
{
	(void)dmg;
	if (!wgt || !commands)
		return;

	for (int32_t i = 0; i < commands->length; i++) {
		Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(commands, i);
		if (!cmd)
			continue;
		clay_render_one(wgt, cmd);
	}
}

/* --- Phase 4: bounded paint cache for paint-only frames ------------------ */

static void clay_cache_release(struct paint_cache *c)
{
	free(c->cmds);
	free(c->payloads);
	free(c->texts);
	c->cmds = NULL;
	c->payloads = NULL;
	c->texts = NULL;
	c->capacity = 0;
	c->length = 0;
	c->text_cap = 0;
	c->text_len = 0;
	c->valid = false;
}

void clay_cache_capture(struct widget *wgt, Clay_RenderCommandArray *cmds)
{
	if (!wgt || !cmds)
		return;
	if (wgt->use_egl || cmds->length > PAINT_CACHE_MAX_CMDS) {
		clay_cache_free(wgt);
		return;
	}

	struct paint_cache *c = wgt->paint_cache;
	if (!c) {
		c = calloc(1, sizeof(*c));
		if (!c)
			return;
		wgt->paint_cache = c;
	}

	/* Image and canvas commands carry pointers that do not survive the
	 * frame (Lua strings, screen-capture buffers, per-frame payload pool),
	 * so a frame containing them is never replayed. */
	bool replayable = true;
	size_t text_need = 0;
	for (int32_t i = 0; i < cmds->length; i++) {
		Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(cmds, i);
		if (!cmd)
			continue;
		if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE ||
		    cmd->commandType == CLAY_RENDER_COMMAND_TYPE_CUSTOM) {
			struct custom_render_payload *p =
				(struct custom_render_payload *)cmd->renderData.custom.customData;
			if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE ||
			    (p && p->type == CUSTOM_RENDER_CANVAS)) {
				replayable = false;
				break;
			}
		}
		if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_TEXT)
			text_need += (size_t)cmd->renderData.text.stringContents.length + 1;
	}
	if (!replayable || text_need > PAINT_CACHE_MAX_TEXT) {
		clay_cache_free(wgt);
		return;
	}

	if (c->capacity < cmds->length) {
		int32_t cap = c->capacity ? c->capacity : 32;
		while (cap < cmds->length)
			cap *= 2;
		Clay_RenderCommand *nc = realloc(c->cmds, (size_t)cap * sizeof(*nc));
		if (nc)
			c->cmds = nc;
		struct custom_render_payload *np = realloc(c->payloads, (size_t)cap * sizeof(*np));
		if (np)
			c->payloads = np;
		if (!nc || !np) {
			clay_cache_free(wgt); /* consistent pointers; retry next frame */
			return;
		}
		c->capacity = cap;
	}
	if (c->text_cap < text_need) {
		size_t cap = c->text_cap ? c->text_cap : 512;
		while (cap < text_need)
			cap *= 2;
		char *nt = realloc(c->texts, cap);
		if (!nt) {
			clay_cache_free(wgt);
			return;
		}
		c->texts = nt;
		c->text_cap = cap;
	}

	c->length = cmds->length;
	c->text_len = 0;
	for (int32_t i = 0; i < cmds->length; i++) {
		Clay_RenderCommand *src = Clay_RenderCommandArray_Get(cmds, i);
		Clay_RenderCommand *dst = &c->cmds[i];
		*dst = *src; /* shallow copy; strings/payloads re-pointed below */
		if (dst->commandType == CLAY_RENDER_COMMAND_TYPE_TEXT) {
			size_t len = (size_t)dst->renderData.text.stringContents.length;
			memcpy(c->texts + c->text_len, dst->renderData.text.stringContents.chars, len);
			c->texts[c->text_len + len] = '\0';
			dst->renderData.text.stringContents.chars = c->texts + c->text_len;
			c->text_len += len + 1;
		} else if (dst->commandType == CLAY_RENDER_COMMAND_TYPE_CUSTOM) {
			struct custom_render_payload *src_p =
				(struct custom_render_payload *)dst->renderData.custom.customData;
			if (src_p) {
				c->payloads[i] = *src_p;
				dst->renderData.custom.customData = &c->payloads[i];
			}
		}
	}
	c->valid = true;
}

bool clay_cache_valid(struct widget *wgt)
{
	return wgt && wgt->paint_cache && wgt->paint_cache->valid &&
	       wgt->paint_cache->length > 0;
}

bool clay_cache_render(struct widget *wgt)
{
	if (!clay_cache_valid(wgt))
		return false;

	struct paint_cache *c = wgt->paint_cache;
	for (int32_t i = 0; i < c->length; i++) {
		Clay_RenderCommand *cmd = &c->cmds[i];
		/* Paint-time patch: button hover/press transitions live in the
		 * retained interact nodes; the cached command keeps last frame's
		 * geometry, only the animated paint fields are refreshed. */
		if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_CUSTOM) {
			struct custom_render_payload *p =
				(struct custom_render_payload *)cmd->renderData.custom.customData;
			if (p && p->type == CUSTOM_RENDER_BUTTON) {
				for (int n = 0; n < wgt->interact.node_count; n++) {
					struct interact_node *node = &wgt->interact.nodes[n];
					if (node->clay_id == cmd->id) {
						p->color1 = node->current_bg;
						p->scale = node->current_scale;
						p->is_focused = (node->state & INTERACT_STATE_FOCUSED) != 0;
						break;
					}
				}
			}
		}
		clay_render_one(wgt, cmd);
	}
	return true;
}

void clay_cache_invalidate(struct widget *wgt)
{
	if (wgt && wgt->paint_cache)
		wgt->paint_cache->valid = false;
}

void clay_cache_free(struct widget *wgt)
{
	if (!wgt || !wgt->paint_cache)
		return;
	clay_cache_release(wgt->paint_cache);
	free(wgt->paint_cache);
	wgt->paint_cache = NULL;
}
