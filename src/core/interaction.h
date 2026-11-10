#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "core/animation.h"

#define MAX_INTERACT_NODES 256

#define WISP_IN_ENTER   1
#define WISP_IN_LEAVE   2
#define WISP_IN_PRESS   3
#define WISP_IN_RELEASE 4
#define WISP_IN_SCROLL  5
#define WISP_IN_MOTION  6
#define WISP_IN_KEY     7

#define WISP_BUTTON_LEFT   1
#define WISP_BUTTON_MIDDLE 2
#define WISP_BUTTON_RIGHT  3

enum interact_node_type {
	NODE_GENERIC = 0,
	NODE_BUTTON,
	NODE_SWITCH,
	NODE_CHECKBOX,
	NODE_SLIDER,
	NODE_SCROLLAREA,
	NODE_INPUT,
	NODE_TEXT,
	NODE_CONTAINER,
	NODE_TAB_ITEM,
};

enum interact_state {
	INTERACT_STATE_NORMAL   = 0,
	INTERACT_STATE_HOVERED  = (1 << 0),
	INTERACT_STATE_PRESSED  = (1 << 1),
	INTERACT_STATE_FOCUSED  = (1 << 2),
	INTERACT_STATE_DISABLED = (1 << 3),
	INTERACT_STATE_SELECTED = (1 << 4),
};

struct widget;
struct lua_State;

struct interact_node {
	uint32_t id;
	uint32_t clay_id;
	char tag[64];
	enum interact_node_type type;
	float x, y, w, h;
	float clip_x, clip_y, clip_w, clip_h;
	bool has_clip;
	int z_order;
	uint32_t state;
	bool focusable;

	/* Dynamic values (Slider, Progress, Switch, Scroll, etc.) */
	float value;
	float target_value;
	float min_val;
	float max_val;
	float step_val;
	struct anim_property val_anim;

	/* Scroll container values */
	float scroll_x;
	float scroll_y;
	float max_scroll_x;
	float max_scroll_y;

	/* Text input buffer */
	char text[256];
	char placeholder[128];
	int cursor_pos;

	/* Styling configurations */
	uint32_t bg_normal;
	uint32_t bg_hover;
	uint32_t bg_pressed;
	uint32_t bg_disabled;
	uint32_t bg_focused;
	uint32_t fg_normal;
	uint32_t fg_hover;
	uint32_t fg_pressed;
	uint32_t fg_disabled;
	uint32_t border_color;
	uint32_t border_focus_color;
	float border_width;
	float radius;
	float scale_normal;
	float scale_hover;
	float scale_pressed;

	/* Current live values */
	uint32_t current_bg;
	uint32_t current_fg;
	float current_scale;

	/* Animation properties */
	struct anim_property bg_t;
	uint32_t anim_from_bg;
	uint32_t anim_to_bg;
	struct anim_property scale_anim;

	int64_t transition_duration_ms;
	enum anim_easing transition_easing;

	/* Lua callback references */
	int lua_on_click;
	int lua_on_right_click;
	int lua_on_double_click;
	int lua_on_middle_click;
	int lua_on_press;
	int lua_on_release;
	int lua_on_hover;
	int lua_on_leave;
	int lua_on_scroll;
	int lua_on_drag;
	int lua_on_change;
	int lua_on_focus;
	int lua_on_blur;
	int lua_on_key;
	int lua_on_submit;

	/* Timing for double-click */
	int64_t last_press_time_us;
	float last_press_x;
	float last_press_y;
	uint32_t last_press_button;

	bool active;
	uint32_t frame_seen;
};

struct interact_tree {
	struct interact_node nodes[MAX_INTERACT_NODES];
	int node_count;
	int hovered_idx;
	int pressed_idx;
	int focused_idx;
	uint32_t current_frame;
	float last_ptr_x;
	float last_ptr_y;
	bool pointer_inside;
	uint32_t pressed_button;
};

void interact_tree_init(struct interact_tree *tree);
void interact_tree_clear(struct interact_tree *tree);
void interact_tree_destroy(struct interact_tree *tree, struct lua_State *L);
void interact_tree_prune(struct interact_tree *tree, struct lua_State *L);
void interact_node_unref(struct interact_node *n, struct lua_State *L);
struct interact_node *interact_node_add(struct interact_tree *tree, const char *tag,
					enum interact_node_type type,
					float x, float y, float w, float h);
int interact_node_find(struct interact_tree *tree, const char *tag);
int interact_hit_test(struct interact_tree *tree, float x, float y);
void interact_node_set_clip(struct interact_node *node, float cx, float cy, float cw, float ch);
bool interact_handle_pointer(struct interact_tree *tree, struct widget *wgt,
			     int event_type, int button, float x, float y, int64_t now_us);
bool interact_handle_scroll(struct interact_tree *tree, struct widget *wgt,
			    float x, float y, int dir, float dx, float dy, int64_t now_us);
bool interact_handle_key(struct interact_tree *tree, struct widget *wgt,
			 uint32_t sym, uint32_t unicode, const char *utf8_str,
			 uint32_t mods, bool is_press, int64_t now_us);
bool interact_update_animations(struct interact_tree *tree, int64_t now_us);
