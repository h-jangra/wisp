#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEY_MOD_SHIFT (1 << 0)
#define KEY_MOD_CTRL  (1 << 1)
#define KEY_MOD_ALT   (1 << 2)
#define KEY_MOD_SUPER (1 << 3)

#define MAX_KEY_BINDINGS 64

struct key_binding {
	uint32_t id;
	uint32_t keysym;
	uint32_t mods;
	bool allow_repeat;
	int lua_cb_ref;
	int widget_slot;
	int app_slot;
	bool active;
};

void keyboard_init(void);
void keyboard_shutdown(void);

bool keyboard_parse_combo(const char *combo_str, uint32_t *out_sym, uint32_t *out_mods);
uint32_t keyboard_bind(const char *combo_str, int lua_cb_ref, int widget_slot, int app_slot, bool allow_repeat);
bool keyboard_unbind(uint32_t binding_id);
int keyboard_binding_count(void);

void keyboard_cleanup_app(int app_slot);
void keyboard_cleanup_widget(int widget_slot);

bool keyboard_dispatch_key(uint32_t sym, uint32_t mods, bool is_press, bool is_repeat,
			   int focused_widget_slot, int focused_app_slot);
