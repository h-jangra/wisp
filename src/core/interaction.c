#include "core/interaction.h"
#include "core/widget.h"
#include "scripting/scripting.h"
#include "wayland/wayland.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef XKB_KEY_NoSymbol
#define XKB_KEY_NoSymbol 0
#define XKB_KEY_Tab 0xff09
#define XKB_KEY_ISO_Left_Tab 0xfe20
#define XKB_KEY_Return 0xff0d
#define XKB_KEY_KP_Enter 0xff8d
#define XKB_KEY_space 0x0020
#define XKB_KEY_Escape 0xff1b
#define XKB_KEY_BackSpace 0xff08
#define XKB_KEY_Left 0xff51
#define XKB_KEY_Up 0xff52
#define XKB_KEY_Right 0xff53
#define XKB_KEY_Down 0xff54
#endif

void interact_tree_init(struct interact_tree *tree)
{
	if (!tree)
		return;
	memset(tree, 0, sizeof(*tree));
	tree->hovered_idx = -1;
	tree->pressed_idx = -1;
	tree->focused_idx = -1;
}

void interact_tree_clear(struct interact_tree *tree)
{
	if (!tree)
		return;
	tree->node_count = 0;
	tree->hovered_idx = -1;
	tree->pressed_idx = -1;
	tree->focused_idx = -1;
}

static void unref_slot(struct lua_State *L, int *slot)
{
	if (slot && *slot != LUA_NOREF) {
		struct lua_State *s = L ? L : Lm;
		if (s)
			luaL_unref(s, LUA_REGISTRYINDEX, *slot);
		*slot = LUA_NOREF;
	}
}

void interact_node_unref(struct interact_node *n, struct lua_State *L)
{
	if (!n)
		return;
	unref_slot(L, &n->lua_on_click);
	unref_slot(L, &n->lua_on_right_click);
	unref_slot(L, &n->lua_on_double_click);
	unref_slot(L, &n->lua_on_middle_click);
	unref_slot(L, &n->lua_on_press);
	unref_slot(L, &n->lua_on_release);
	unref_slot(L, &n->lua_on_hover);
	unref_slot(L, &n->lua_on_leave);
	unref_slot(L, &n->lua_on_scroll);
	unref_slot(L, &n->lua_on_drag);
	unref_slot(L, &n->lua_on_change);
	unref_slot(L, &n->lua_on_focus);
	unref_slot(L, &n->lua_on_blur);
	unref_slot(L, &n->lua_on_key);
	unref_slot(L, &n->lua_on_submit);
	unref_slot(L, &n->lua_on_mount);
	unref_slot(L, &n->lua_on_update);
	unref_slot(L, &n->lua_on_unmount);
	unref_slot(L, &n->lua_on_outside_click);
	unref_slot(L, &n->lua_on_draw);
}

void interact_tree_prune(struct interact_tree *tree, struct lua_State *L, struct widget *wgt)
{
	if (!tree)
		return;
	int old_hovered = tree->hovered_idx;
	int old_pressed = tree->pressed_idx;
	int old_focused = tree->focused_idx;
	tree->hovered_idx = -1;
	tree->pressed_idx = -1;
	tree->focused_idx = -1;

	int write_idx = 0;
	for (int i = 0; i < tree->node_count; i++) {
		if (tree->nodes[i].frame_seen == tree->current_frame) {
			if (i == old_hovered) tree->hovered_idx = write_idx;
			if (i == old_pressed) tree->pressed_idx = write_idx;
			if (i == old_focused) tree->focused_idx = write_idx;
			if (write_idx != i)
				tree->nodes[write_idx] = tree->nodes[i];
			write_idx++;
		} else {
			if (tree->nodes[i].lua_on_unmount != LUA_NOREF && wgt && wgt->lua_L) {
				int ref = tree->nodes[i].lua_on_unmount;
				lua_script_call(wgt, &ref, "on_unmount", NULL);
			}
			if (i == old_focused && tree->nodes[i].lua_on_blur != LUA_NOREF && wgt && wgt->lua_L) {
				int ref = tree->nodes[i].lua_on_blur;
				lua_script_call(wgt, &ref, "on_blur", NULL);
			}
			interact_node_unref(&tree->nodes[i], L);
		}
	}
	tree->node_count = write_idx;
}

void interact_tree_destroy(struct interact_tree *tree, struct lua_State *L)
{
	if (!tree)
		return;
	for (int i = 0; i < tree->node_count; i++)
		interact_node_unref(&tree->nodes[i], L);
	if (tree->nodes) {
		free(tree->nodes);
		tree->nodes = NULL;
	}
	tree->node_capacity = 0;
	interact_tree_clear(tree);
}

struct interact_node *interact_node_add(struct interact_tree *tree, const char *tag,
					enum interact_node_type type,
					float x, float y, float w, float h)
{
	if (!tree || tree->node_count >= MAX_INTERACT_NODES)
		return NULL;

	if (tree->node_count >= tree->node_capacity) {
		int new_cap = tree->node_capacity > 0 ? tree->node_capacity * 2 : 16;
		if (new_cap > MAX_INTERACT_NODES)
			new_cap = MAX_INTERACT_NODES;
		struct interact_node *new_nodes = realloc(tree->nodes, (size_t)new_cap * sizeof(struct interact_node));
		if (!new_nodes)
			return NULL;
		tree->nodes = new_nodes;
		tree->node_capacity = new_cap;
	}

	int idx = tree->node_count++;
	struct interact_node *node = &tree->nodes[idx];
	memset(node, 0, sizeof(*node));
	node->id = (uint32_t)(idx + 1);
	node->type = type;
	if (tag)
		snprintf(node->tag, sizeof(node->tag), "%s", tag);
	node->x = x;
	node->y = y;
	node->w = w;
	node->h = h;
	node->scale_normal = 1.0f;
	node->scale_hover = 1.0f;
	node->scale_pressed = 1.0f;
	node->current_scale = 1.0f;
	node->min_val = 0.0f;
	node->max_val = 1.0f;
	node->step_val = 0.0f;
	anim_prop_init(&node->scale_anim, 1.0f);
	anim_prop_init(&node->bg_t, 1.0f);
	anim_prop_init(&node->val_anim, 0.0f);
	node->transition_duration_ms = 140;
	node->transition_easing = EASING_EASE_OUT_QUAD;

	node->lua_on_click = LUA_NOREF;
	node->lua_on_right_click = LUA_NOREF;
	node->lua_on_double_click = LUA_NOREF;
	node->lua_on_middle_click = LUA_NOREF;
	node->lua_on_press = LUA_NOREF;
	node->lua_on_release = LUA_NOREF;
	node->lua_on_hover = LUA_NOREF;
	node->lua_on_leave = LUA_NOREF;
	node->lua_on_scroll = LUA_NOREF;
	node->lua_on_drag = LUA_NOREF;
	node->lua_on_change = LUA_NOREF;
	node->lua_on_focus = LUA_NOREF;
	node->lua_on_blur = LUA_NOREF;
	node->lua_on_key = LUA_NOREF;
	node->lua_on_submit = LUA_NOREF;
	node->lua_on_mount = LUA_NOREF;
	node->lua_on_update = LUA_NOREF;
	node->lua_on_unmount = LUA_NOREF;
	node->lua_on_outside_click = LUA_NOREF;
	node->lua_on_draw = LUA_NOREF;
	node->mounted = false;

	node->active = true;
	return node;
}

int interact_node_find(struct interact_tree *tree, const char *tag)
{
	if (!tree || !tag)
		return -1;
	for (int i = 0; i < tree->node_count; i++) {
		if (strcmp(tree->nodes[i].tag, tag) == 0)
			return i;
	}
	return -1;
}

void interact_node_set_clip(struct interact_node *node, float cx, float cy, float cw, float ch)
{
	if (!node)
		return;
	node->has_clip = true;
	node->clip_x = cx;
	node->clip_y = cy;
	node->clip_w = cw;
	node->clip_h = ch;
}

int interact_hit_test(struct interact_tree *tree, float x, float y)
{
	if (!tree)
		return -1;

	int best_idx = -1;
	int best_z = -32768;

	/* Top of visual stack first */
	for (int i = tree->node_count - 1; i >= 0; i--) {
		struct interact_node *node = &tree->nodes[i];

		if (!node->active || (node->state & INTERACT_STATE_DISABLED))
			continue;
		if (node->has_clip) {
			if (x < node->clip_x || x >= node->clip_x + node->clip_w ||
			    y < node->clip_y || y >= node->clip_y + node->clip_h)
				continue;
		}
		if (x >= node->x && x < node->x + node->w &&
		    y >= node->y && y < node->y + node->h) {
			if (best_idx == -1 || node->z_order > best_z) {
				best_idx = i;
				best_z = node->z_order;
			}
		}
	}
	return best_idx;
}

static void start_node_transition(struct interact_node *node, uint32_t target_bg,
				  float target_scale, int64_t now_us)
{
	if (node->current_bg != target_bg) {
		node->anim_from_bg = node->current_bg;
		node->anim_to_bg = target_bg;
		node->bg_t.current = 0.0f;
		anim_prop_start(&node->bg_t, 1.0f, node->transition_duration_ms,
				node->transition_easing, now_us);
	}
	if (node->current_scale != target_scale) {
		anim_prop_start(&node->scale_anim, target_scale,
				node->transition_duration_ms, node->transition_easing, now_us);
	}
}

bool interact_handle_pointer(struct interact_tree *tree, struct widget *wgt,
			     int event_type, int button, float x, float y, int64_t now_us)
{
	if (!tree || !wgt)
		return false;

	static bool in_dispatch = false;
	if (in_dispatch)
		return false;
	in_dispatch = true;

	bool state_changed = false;
	int hit_idx = interact_hit_test(tree, x, y);


	switch (event_type) {
	case WISP_IN_MOTION:
	case WISP_IN_ENTER: {
		tree->pointer_inside = true;
		float dx = x - tree->last_ptr_x;
		float dy = y - tree->last_ptr_y;

		/* Drag handling when a node has pointer capture */
		if (tree->pressed_idx >= 0 && tree->pressed_idx < tree->node_count) {
			struct interact_node *cur = &tree->nodes[tree->pressed_idx];
			if (cur->type == NODE_SLIDER && cur->w > 0.0f) {
				float ratio = (x - cur->x) / cur->w;
				if (ratio < 0.0f) ratio = 0.0f;
				if (ratio > 1.0f) ratio = 1.0f;
				float val = cur->min_val + ratio * (cur->max_val - cur->min_val);
				if (cur->step_val > 0.0f) {
					int steps = (int)((val - cur->min_val) / cur->step_val + 0.5f);
					val = cur->min_val + (float)steps * cur->step_val;
					if (val > cur->max_val) val = cur->max_val;
					if (val < cur->min_val) val = cur->min_val;
				}
				if (cur->value != val) {
					cur->value = val;
					cur->val_anim.current = (cur->max_val > cur->min_val) ?
						(val - cur->min_val) / (cur->max_val - cur->min_val) : 0.0f;
					if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
						int ref = cur->lua_on_change;
						lua_script_call_num(wgt, &ref, "on_change", (double)val);
					}
					state_changed = true;
				}
			}
			if (cur->lua_on_drag != LUA_NOREF && wgt->lua_L) {
				int ref = cur->lua_on_drag;
				float lx = (cur->type == NODE_CANVAS) ? (x - cur->x) : x;
				float ly = (cur->type == NODE_CANVAS) ? (y - cur->y) : y;
				lua_script_call_drag(wgt, &ref, "on_drag", lx, ly, dx, dy);
				state_changed = true;
			}
			if (wgt->lua_drag != LUA_NOREF && wgt->lua_L) {
				int ref = wgt->lua_drag;
				lua_script_call_drag(wgt, &ref, "on_drag", x, y, dx, dy);
				state_changed = true;
			}
		}

		/* Hover transition */
		if (hit_idx != tree->hovered_idx) {
			if (tree->hovered_idx >= 0 && tree->hovered_idx < tree->node_count) {
				struct interact_node *prev = &tree->nodes[tree->hovered_idx];
				prev->state &= ~INTERACT_STATE_HOVERED;
				uint32_t target_bg = (prev->state & INTERACT_STATE_PRESSED) ?
						     prev->bg_pressed : prev->bg_normal;
				float target_sc = (prev->state & INTERACT_STATE_PRESSED) ?
						  prev->scale_pressed : prev->scale_normal;
				start_node_transition(prev, target_bg, target_sc, now_us);
				if (prev->lua_on_leave != LUA_NOREF && wgt->lua_L) {
					int ref = prev->lua_on_leave;
					lua_script_call(wgt, &ref, "on_leave", NULL);
				}
				state_changed = true;
			}
			tree->hovered_idx = hit_idx;
			if (hit_idx >= 0) {
				struct interact_node *cur = &tree->nodes[hit_idx];
				cur->state |= INTERACT_STATE_HOVERED;
				uint32_t target_bg = (cur->state & INTERACT_STATE_PRESSED) ?
						     cur->bg_pressed : cur->bg_hover;
				float target_sc = (cur->state & INTERACT_STATE_PRESSED) ?
						  cur->scale_pressed : cur->scale_hover;
				start_node_transition(cur, target_bg, target_sc, now_us);
				if (cur->lua_on_hover != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_hover;
					lua_script_call_xy(wgt, &ref, "on_hover", (int32_t)x, (int32_t)y);
				}
				state_changed = true;
			}
		}

		if (hit_idx < 0 && wgt->lua_hover != LUA_NOREF && event_type == WISP_IN_ENTER && wgt->lua_L) {
			int ref = wgt->lua_hover;
			lua_script_call_xy(wgt, &ref, "on_hover", (int32_t)x, (int32_t)y);
			state_changed = true;
		}

		tree->last_ptr_x = x;
		tree->last_ptr_y = y;

		/* Update Wayland cursor shape */
		if (tree->pressed_idx >= 0) {
			wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
		} else if (hit_idx >= 0) {
			struct interact_node *cur = &tree->nodes[hit_idx];
			if (cur->state & INTERACT_STATE_DISABLED)
				wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED);
			else if (cur->type == NODE_INPUT)
				wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
			else if (cur->type == NODE_BUTTON || cur->type == NODE_SWITCH ||
			         cur->type == NODE_CHECKBOX || cur->type == NODE_SLIDER ||
			         cur->lua_on_click != LUA_NOREF)
				wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
			else
				wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		} else if (wgt->lua_click != LUA_NOREF) {
			wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
		} else {
			wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		}
		break;
	}

	case WISP_IN_LEAVE: {
		tree->pointer_inside = false;
		wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		if (tree->hovered_idx >= 0 && tree->hovered_idx < tree->node_count) {
			struct interact_node *prev = &tree->nodes[tree->hovered_idx];
			prev->state &= ~INTERACT_STATE_HOVERED;
			start_node_transition(prev, prev->bg_normal, prev->scale_normal, now_us);
			if (prev->lua_on_leave != LUA_NOREF && wgt->lua_L) {
				int ref = prev->lua_on_leave;
				lua_script_call(wgt, &ref, "on_leave", NULL);
			}
			state_changed = true;
		}
		tree->hovered_idx = -1;
		if (wgt->lua_leave != LUA_NOREF && wgt->lua_L) {
			int ref = wgt->lua_leave;
			lua_script_call(wgt, &ref, "on_leave", NULL);
			state_changed = true;
		}
		break;
	}

	case WISP_IN_PRESS: {
		tree->pressed_idx = hit_idx;
		tree->pressed_button = button;
		tree->last_ptr_x = x;
		tree->last_ptr_y = y;

		/* Outside click / popup dismissal */
		for (int i = 0; i < tree->node_count; i++) {
			struct interact_node *n = &tree->nodes[i];
			if (n->active && n->lua_on_outside_click != LUA_NOREF) {
				if (x < n->x || x >= n->x + n->w || y < n->y || y >= n->y + n->h) {
					int ref = n->lua_on_outside_click;
					lua_script_call(wgt, &ref, "on_outside_click", NULL);
					state_changed = true;
				}
			}
		}

		if (hit_idx >= 0) {
			struct interact_node *cur = &tree->nodes[hit_idx];
			cur->state |= INTERACT_STATE_PRESSED;

			/* Focus handling */
			if (cur->type == NODE_INPUT || cur->focusable) {
				if (tree->focused_idx >= 0 && tree->focused_idx != hit_idx &&
				    tree->focused_idx < tree->node_count) {
					struct interact_node *prev_foc = &tree->nodes[tree->focused_idx];
					prev_foc->state &= ~INTERACT_STATE_FOCUSED;
					if (prev_foc->lua_on_blur != LUA_NOREF && wgt->lua_L) {
						int ref = prev_foc->lua_on_blur;
						lua_script_call(wgt, &ref, "on_blur", NULL);
					}
				}
				tree->focused_idx = hit_idx;
				cur->state |= INTERACT_STATE_FOCUSED;
				if (cur->lua_on_focus != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_focus;
					lua_script_call(wgt, &ref, "on_focus", NULL);
				}
			} else if (tree->focused_idx >= 0 && tree->focused_idx < tree->node_count) {
				struct interact_node *prev_foc = &tree->nodes[tree->focused_idx];
				prev_foc->state &= ~INTERACT_STATE_FOCUSED;
				if (prev_foc->lua_on_blur != LUA_NOREF && wgt->lua_L) {
					int ref = prev_foc->lua_on_blur;
					lua_script_call(wgt, &ref, "on_blur", NULL);
				}
				tree->focused_idx = -1;
			}

			/* Slider immediate jump on press */
			if (cur->type == NODE_SLIDER && cur->w > 0.0f) {
				float ratio = (x - cur->x) / cur->w;
				if (ratio < 0.0f) ratio = 0.0f;
				if (ratio > 1.0f) ratio = 1.0f;
				float val = cur->min_val + ratio * (cur->max_val - cur->min_val);
				cur->value = val;
				cur->val_anim.current = ratio;
				if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_change;
					lua_script_call_num(wgt, &ref, "on_change", (double)val);
				}
			}

			start_node_transition(cur, cur->bg_pressed, cur->scale_pressed, now_us);
			if (cur->lua_on_press != LUA_NOREF && wgt->lua_L) {
				int ref = cur->lua_on_press;
				float lx = (cur->type == NODE_CANVAS) ? (x - cur->x) : x;
				float ly = (cur->type == NODE_CANVAS) ? (y - cur->y) : y;
				lua_script_call_xy_btn(wgt, &ref, "on_press", lx, ly, button);
			}
			state_changed = true;
		}

		if (wgt->lua_press != LUA_NOREF && wgt->lua_L) {
			int ref = wgt->lua_press;
			lua_script_call_xy_btn(wgt, &ref, "on_press", x, y, button);
			state_changed = true;
		}
		break;
	}

	case WISP_IN_RELEASE: {
		int p_idx = tree->pressed_idx;
		tree->pressed_idx = -1;

		if (p_idx >= 0 && p_idx < tree->node_count) {
			struct interact_node *cur = &tree->nodes[p_idx];
			cur->state &= ~INTERACT_STATE_PRESSED;
			uint32_t target_bg = (hit_idx == p_idx) ? cur->bg_hover : cur->bg_normal;
			float target_scale = (hit_idx == p_idx) ? cur->scale_hover : cur->scale_normal;
			start_node_transition(cur, target_bg, target_scale, now_us);
			if (cur->lua_on_release != LUA_NOREF && wgt->lua_L) {
				int ref = cur->lua_on_release;
				float lx = (cur->type == NODE_CANVAS) ? (x - cur->x) : x;
				float ly = (cur->type == NODE_CANVAS) ? (y - cur->y) : y;
				lua_script_call_xy_btn(wgt, &ref, "on_release", lx, ly, button);
			}
			state_changed = true;

			/* Click / Toggle actions if released over pressed element */
			if (hit_idx == p_idx) {
				bool is_double = (cur->last_press_time_us > 0 &&
						  (now_us - cur->last_press_time_us) < 350000LL &&
						  cur->last_press_button == (uint32_t)button &&
						  fabsf(x - cur->last_press_x) < 8.0f &&
						  fabsf(y - cur->last_press_y) < 8.0f);

				if (is_double && cur->lua_on_double_click != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_double_click;
					lua_script_call_xy_btn(wgt, &ref, "on_double_click", x, y, button);
					state_changed = true;
				}

				if (button == WISP_BUTTON_RIGHT && cur->lua_on_right_click != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_right_click;
					lua_script_call_xy(wgt, &ref, "on_right_click", (int32_t)x, (int32_t)y);
					state_changed = true;
				} else if (button == WISP_BUTTON_MIDDLE && cur->lua_on_middle_click != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_middle_click;
					lua_script_call_xy(wgt, &ref, "on_middle_click", (int32_t)x, (int32_t)y);
					state_changed = true;
				} else {
					if (cur->type == NODE_SWITCH) {
						bool new_val = !(cur->value > 0.5f);
						cur->value = new_val ? 1.0f : 0.0f;
						anim_prop_start(&cur->val_anim, cur->value,
								cur->transition_duration_ms > 0 ? cur->transition_duration_ms : 150,
								EASING_EASE_OUT_QUAD, now_us);
						if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
							int ref = cur->lua_on_change;
							lua_script_call_bool(wgt, &ref, "on_change", new_val);
						}
						state_changed = true;
					} else if (cur->type == NODE_CHECKBOX) {
						bool new_val = !(cur->value > 0.5f);
						cur->value = new_val ? 1.0f : 0.0f;
						anim_prop_start(&cur->val_anim, cur->value, 120,
								EASING_EASE_OUT_QUAD, now_us);
						if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
							int ref = cur->lua_on_change;
							lua_script_call_bool(wgt, &ref, "on_change", new_val);
						}
						state_changed = true;
					}

					if (cur->lua_on_click != LUA_NOREF && wgt->lua_L) {
						int ref = cur->lua_on_click;
						lua_script_call_xy_btn(wgt, &ref, "on_click", x, y, button);
						state_changed = true;
					}
				}

				cur->last_press_time_us = now_us;
				cur->last_press_x = x;
				cur->last_press_y = y;
				cur->last_press_button = (uint32_t)button;
			}
		}

		if (wgt->lua_release != LUA_NOREF && wgt->lua_L) {
			int ref = wgt->lua_release;
			lua_script_call_xy_btn(wgt, &ref, "on_release", x, y, button);
			state_changed = true;
		}

		/* Widget-level click if released on widget surface */
		if ((p_idx < 0 || p_idx == hit_idx) && wgt->lua_click != LUA_NOREF && wgt->lua_L) {
			int ref = wgt->lua_click;
			lua_script_call_xy_btn(wgt, &ref, "on_click", x, y, button);
			state_changed = true;
		}

		if (hit_idx >= 0) {
			struct interact_node *cur = &tree->nodes[hit_idx];
			if (cur->type == NODE_INPUT)
				wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
			else if (cur->type == NODE_BUTTON || cur->type == NODE_SWITCH ||
			         cur->type == NODE_CHECKBOX || cur->type == NODE_SLIDER ||
			         cur->lua_on_click != LUA_NOREF)
				wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
			else
				wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		} else if (wgt->lua_click != LUA_NOREF) {
			wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
		} else {
			wayland_set_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		}
		break;
	}

	default:
		break;
	}

	in_dispatch = false;
	return state_changed;
}

bool interact_handle_scroll(struct interact_tree *tree, struct widget *wgt,
			    float x, float y, int dir, float dx, float dy, int64_t now_us)
{
	(void)now_us;
	if (!tree || !wgt)
		return false;

	static bool in_dispatch = false;
	if (in_dispatch)
		return false;
	in_dispatch = true;

	bool handled = false;
	int hit_idx = interact_hit_test(tree, x, y);

	if (hit_idx >= 0) {
		struct interact_node *cur = &tree->nodes[hit_idx];
		if (cur->type == NODE_SCROLLAREA) {
			cur->scroll_y += (dy != 0.0f) ? dy * 20.0f : (float)dir * 28.0f;
			cur->scroll_x += dx * 20.0f;
			if (cur->scroll_y < 0.0f) cur->scroll_y = 0.0f;
			if (cur->max_scroll_y > 0.0f && cur->scroll_y > cur->max_scroll_y)
				cur->scroll_y = cur->max_scroll_y;
			if (cur->scroll_x < 0.0f) cur->scroll_x = 0.0f;
			if (cur->max_scroll_x > 0.0f && cur->scroll_x > cur->max_scroll_x)
				cur->scroll_x = cur->max_scroll_x;
			cur->value = cur->scroll_y;

			if (cur->lua_on_scroll != LUA_NOREF && wgt->lua_L) {
				int ref = cur->lua_on_scroll;
				lua_script_call_num(wgt, &ref, "on_scroll", (double)cur->scroll_y);
			}
			handled = true;
		} else if (cur->type == NODE_SLIDER) {
			float step = cur->step_val > 0.0f ? cur->step_val : (cur->max_val - cur->min_val) * 0.05f;
			cur->value += (float)dir * step;
			if (cur->value < cur->min_val) cur->value = cur->min_val;
			if (cur->value > cur->max_val) cur->value = cur->max_val;
			if (cur->max_val > cur->min_val)
				cur->val_anim.current = (cur->value - cur->min_val) / (cur->max_val - cur->min_val);
			if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
				int ref = cur->lua_on_change;
				lua_script_call_num(wgt, &ref, "on_change", (double)cur->value);
			}
			handled = true;
		} else if (cur->lua_on_scroll != LUA_NOREF && wgt->lua_L) {
			int ref = cur->lua_on_scroll;
			lua_script_call_scroll(wgt, &ref, "on_scroll", dir, (int32_t)x, (int32_t)y);
			handled = true;
		}
	}

	if (wgt->lua_scroll != LUA_NOREF && wgt->lua_L) {
		int ref = wgt->lua_scroll;
		lua_script_call_scroll(wgt, &ref, "on_scroll", dir, (int32_t)x, (int32_t)y);
		handled = true;
	}

	in_dispatch = false;
	return handled;
}

bool interact_handle_key(struct interact_tree *tree, struct widget *wgt,
			 uint32_t sym, uint32_t unicode, const char *utf8_str,
			 uint32_t mods, bool is_press, int64_t now_us)
{
	if (!tree || !wgt)
		return false;

	static bool in_dispatch = false;
	if (in_dispatch)
		return false;
	in_dispatch = true;

	bool shift = (mods & 1) != 0;
	bool state_changed = false;

	if (is_press) {
		/* Tab navigation */
		if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
			int count = tree->node_count;
			if (count > 0) {
				int dir = shift ? -1 : 1;
				int start = (tree->focused_idx >= 0) ? tree->focused_idx : (shift ? count : -1);
				int next = -1;
				for (int step = 1; step <= count; step++) {
					int candidate = (start + dir * step) % count;
					if (candidate < 0)
						candidate += count;
					struct interact_node *n = &tree->nodes[candidate];
					if (n->active && !(n->state & INTERACT_STATE_DISABLED) &&
					    (n->focusable || n->type == NODE_INPUT || n->type == NODE_BUTTON ||
					     n->type == NODE_SWITCH || n->type == NODE_CHECKBOX || n->type == NODE_SLIDER)) {
						next = candidate;
						break;
					}
				}
				if (next >= 0 && next != tree->focused_idx) {
					if (tree->focused_idx >= 0 && tree->focused_idx < count) {
						struct interact_node *prev = &tree->nodes[tree->focused_idx];
						prev->state &= ~INTERACT_STATE_FOCUSED;
						if (prev->lua_on_blur != LUA_NOREF && wgt->lua_L) {
							int ref = prev->lua_on_blur;
							lua_script_call(wgt, &ref, "on_blur", NULL);
						}
					}
					tree->focused_idx = next;
					struct interact_node *cur = &tree->nodes[next];
					cur->state |= INTERACT_STATE_FOCUSED;
					if (cur->lua_on_focus != LUA_NOREF && wgt->lua_L) {
						int ref = cur->lua_on_focus;
						lua_script_call(wgt, &ref, "on_focus", NULL);
					}
					state_changed = true;
				}
			}
			in_dispatch = false;
			return state_changed;
		}

		/* Escape: blur & dismiss popups */
		if (sym == XKB_KEY_Escape) {
			for (int i = 0; i < tree->node_count; i++) {
				struct interact_node *n = &tree->nodes[i];
				if (n->active && n->lua_on_outside_click != LUA_NOREF) {
					int ref = n->lua_on_outside_click;
					lua_script_call(wgt, &ref, "on_outside_click", NULL);
					state_changed = true;
				}
			}
			if (tree->focused_idx >= 0 && tree->focused_idx < tree->node_count) {
				struct interact_node *prev = &tree->nodes[tree->focused_idx];
				prev->state &= ~INTERACT_STATE_FOCUSED;
				if (prev->lua_on_blur != LUA_NOREF && wgt->lua_L) {
					int ref = prev->lua_on_blur;
					lua_script_call(wgt, &ref, "on_blur", NULL);
				}
				tree->focused_idx = -1;
				state_changed = true;
			}
			in_dispatch = false;
			return state_changed;
		}

		/* Enter or Space activation */
		if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) &&
		    tree->focused_idx >= 0 && tree->focused_idx < tree->node_count) {
			struct interact_node *cur = &tree->nodes[tree->focused_idx];
			if (cur->type == NODE_BUTTON) {
				if (cur->lua_on_click != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_click;
					lua_script_call_xy_btn(wgt, &ref, "on_click", cur->x + cur->w * 0.5f,
							       cur->y + cur->h * 0.5f, WISP_BUTTON_LEFT);
				}
				state_changed = true;
			} else if (cur->type == NODE_SWITCH) {
				bool new_val = !(cur->value > 0.5f);
				cur->value = new_val ? 1.0f : 0.0f;
				anim_prop_start(&cur->val_anim, cur->value, 150, EASING_EASE_OUT_QUAD, now_us);
				if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_change;
					lua_script_call_bool(wgt, &ref, "on_change", new_val);
				}
				state_changed = true;
			} else if (cur->type == NODE_CHECKBOX) {
				bool new_val = !(cur->value > 0.5f);
				cur->value = new_val ? 1.0f : 0.0f;
				anim_prop_start(&cur->val_anim, cur->value, 120, EASING_EASE_OUT_QUAD, now_us);
				if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_change;
					lua_script_call_bool(wgt, &ref, "on_change", new_val);
				}
				state_changed = true;
			} else if (cur->type == NODE_INPUT && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
				if (cur->lua_on_submit != LUA_NOREF && wgt->lua_L) {
					int ref = cur->lua_on_submit;
					lua_script_call(wgt, &ref, "on_submit", cur->text);
				}
				state_changed = true;
			}
			in_dispatch = false;
			return state_changed;
		}

		/* Arrow keys for Slider and Focus traversal */
		if (tree->focused_idx >= 0 && tree->focused_idx < tree->node_count) {
			struct interact_node *cur = &tree->nodes[tree->focused_idx];
			if (cur->type == NODE_SLIDER) {
				float step = cur->step_val > 0.0f ? cur->step_val : (cur->max_val - cur->min_val) * 0.05f;
				if (sym == XKB_KEY_Left || sym == XKB_KEY_Down) {
					cur->value = (cur->value - step < cur->min_val) ? cur->min_val : cur->value - step;
					if (cur->max_val > cur->min_val)
						cur->val_anim.current = (cur->value - cur->min_val) / (cur->max_val - cur->min_val);
					if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
						int ref = cur->lua_on_change;
						lua_script_call_num(wgt, &ref, "on_change", (double)cur->value);
					}
					in_dispatch = false;
					return true;
				} else if (sym == XKB_KEY_Right || sym == XKB_KEY_Up) {
					cur->value = (cur->value + step > cur->max_val) ? cur->max_val : cur->value + step;
					if (cur->max_val > cur->min_val)
						cur->val_anim.current = (cur->value - cur->min_val) / (cur->max_val - cur->min_val);
					if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
						int ref = cur->lua_on_change;
						lua_script_call_num(wgt, &ref, "on_change", (double)cur->value);
					}
					in_dispatch = false;
					return true;
				}
			} else if (cur->type != NODE_INPUT && (sym == XKB_KEY_Left || sym == XKB_KEY_Up || sym == XKB_KEY_Right || sym == XKB_KEY_Down)) {
				int dir = (sym == XKB_KEY_Left || sym == XKB_KEY_Up) ? -1 : 1;
				int count = tree->node_count;
				int next = -1;
				for (int step = 1; step <= count; step++) {
					int candidate = (tree->focused_idx + dir * step) % count;
					if (candidate < 0)
						candidate += count;
					struct interact_node *n = &tree->nodes[candidate];
					if (n->active && !(n->state & INTERACT_STATE_DISABLED) &&
					    (n->focusable || n->type == NODE_BUTTON || n->type == NODE_TAB_ITEM ||
					     n->type == NODE_SWITCH || n->type == NODE_CHECKBOX || n->type == NODE_SLIDER)) {
						next = candidate;
						break;
					}
				}
				if (next >= 0 && next != tree->focused_idx) {
					cur->state &= ~INTERACT_STATE_FOCUSED;
					if (cur->lua_on_blur != LUA_NOREF && wgt->lua_L) {
						int ref = cur->lua_on_blur;
						lua_script_call(wgt, &ref, "on_blur", NULL);
					}
					tree->focused_idx = next;
					struct interact_node *nxt = &tree->nodes[next];
					nxt->state |= INTERACT_STATE_FOCUSED;
					if (nxt->lua_on_focus != LUA_NOREF && wgt->lua_L) {
						int ref = nxt->lua_on_focus;
						lua_script_call(wgt, &ref, "on_focus", NULL);
					}
					if (nxt->type == NODE_TAB_ITEM && nxt->lua_on_click != LUA_NOREF && wgt->lua_L) {
						int ref = nxt->lua_on_click;
						lua_script_call_xy_btn(wgt, &ref, "on_click", nxt->x + nxt->w * 0.5f,
								       nxt->y + nxt->h * 0.5f, WISP_BUTTON_LEFT);
					}
					in_dispatch = false;
					return true;
				}
			}
		}

		/* Text input */
		if (tree->focused_idx >= 0 && tree->focused_idx < tree->node_count) {
			struct interact_node *cur = &tree->nodes[tree->focused_idx];
			if (cur->type == NODE_INPUT) {
				size_t len = strlen(cur->text);
				if (sym == XKB_KEY_BackSpace) {
					if (len > 0) {
						size_t idx = len - 1;
						while (idx > 0 && ((unsigned char)cur->text[idx] & 0xC0) == 0x80)
							idx--;
						cur->text[idx] = '\0';
						if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
							int ref = cur->lua_on_change;
							lua_script_call(wgt, &ref, "on_change", cur->text);
						}
						in_dispatch = false;
						return true;
					}
				} else if (utf8_str && utf8_str[0] && (unsigned char)utf8_str[0] >= 32) {
					size_t ulen = strlen(utf8_str);
					if (len + ulen < sizeof(cur->text)) {
						memcpy(cur->text + len, utf8_str, ulen + 1);
						if (cur->lua_on_change != LUA_NOREF && wgt->lua_L) {
							int ref = cur->lua_on_change;
							lua_script_call(wgt, &ref, "on_change", cur->text);
						}
						in_dispatch = false;
						return true;
					}
				}
			}
		}
	}

	if (wgt->lua_key != LUA_NOREF && wgt->lua_L) {
		int ref = wgt->lua_key;
		lua_script_call_key(wgt, &ref, "on_key", sym, unicode, mods, is_press);
		state_changed = true;
	}

	in_dispatch = false;
	return state_changed;
}

bool interact_update_animations(struct interact_tree *tree, int64_t now_us)
{
	if (!tree)
		return false;

	bool any_active = false;
	for (int i = 0; i < tree->node_count; i++) {
		struct interact_node *node = &tree->nodes[i];
		if (!node->active)
			continue;

		if (node->bg_t.active) {
			float t = anim_prop_eval(&node->bg_t, now_us);
			node->current_bg = color_lerp(node->anim_from_bg, node->anim_to_bg, t);
			any_active = true;
		}
		if (node->scale_anim.active) {
			node->current_scale = anim_prop_eval(&node->scale_anim, now_us);
			any_active = true;
		}
		if (node->val_anim.active) {
			anim_prop_eval(&node->val_anim, now_us);
			any_active = true;
		}
	}
	return any_active;
}
