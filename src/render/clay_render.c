#include "render/clay_render.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "layout/clay_layout.h"

void clay_render_commands(struct widget *wgt, Clay_RenderCommandArray *commands, struct rect *dmg)
{
	if (!wgt || !wgt->pixels || !commands)
		return;

	int S = wgt->scale > 0 ? wgt->scale : 1;

	for (int32_t i = 0; i < commands->length; i++) {
		Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(commands, i);
		if (!cmd)
			continue;

		int bx = (int)(cmd->boundingBox.x * S);
		int by = (int)(cmd->boundingBox.y * S);
		int bw = (int)(cmd->boundingBox.width * S);
		int bh = (int)(cmd->boundingBox.height * S);

		switch (cmd->commandType) {
		case CLAY_RENDER_COMMAND_TYPE_NONE:
			break;

		case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
			Clay_RectangleRenderData *rd = &cmd->renderData.rectangle;
			uint32_t c = clay_color_to_argb(rd->backgroundColor);
			if ((c >> 24) == 0)
				break;
			float r = rd->cornerRadius.topLeft;
			if (r > 0.0f) {
				int rad = (int)(r * S);
				rrect(wgt->pixels, wgt->width, wgt->height, dmg,
				      bx, by, bw, bh, rad, c);
			} else {
				fill_rect(wgt->pixels, wgt->width, wgt->height, dmg,
					  bx, by, bw, bh, c);
			}
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_BORDER: {
			Clay_BorderRenderData *bd = &cmd->renderData.border;
			uint32_t c = clay_color_to_argb(bd->color);
			if ((c >> 24) == 0)
				break;
			int top_w = (int)(bd->width.top * S);
			int bot_w = (int)(bd->width.bottom * S);
			int left_w = (int)(bd->width.left * S);
			int right_w = (int)(bd->width.right * S);

			if (top_w > 0)
				fill_rect(wgt->pixels, wgt->width, wgt->height, dmg,
					  bx, by, bw, top_w, c);
			if (bot_w > 0)
				fill_rect(wgt->pixels, wgt->width, wgt->height, dmg,
					  bx, by + bh - bot_w, bw, bot_w, c);
			if (left_w > 0)
				fill_rect(wgt->pixels, wgt->width, wgt->height, dmg,
					  bx, by, left_w, bh, c);
			if (right_w > 0)
				fill_rect(wgt->pixels, wgt->width, wgt->height, dmg,
					  bx + bw - right_w, by, right_w, bh, c);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_TEXT: {
			Clay_TextRenderData *td = &cmd->renderData.text;
			uint32_t c = clay_color_to_argb(td->textColor);
			if ((c >> 24) == 0)
				break;
			int fsize = td->fontSize > 0 ? (int)td->fontSize : 14;
			text_set_size(fsize * S);
			int ty = (int)((cmd->boundingBox.y + 0.82f * fsize) * S);
			draw_text_slice(wgt->pixels, wgt->width, wgt->height, dmg,
					td->stringContents.chars,
					td->stringContents.length,
					bx, ty, c);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
			Clay_ImageRenderData *id = &cmd->renderData.image;
			const char *path = (const char *)id->imageData;
			if (path)
				draw_image_path(wgt, dmg, path, bx, by, bw, bh);
			break;
		}

		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
		case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START:
		case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:
		case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
		default:
			break;
		}
	}
}
