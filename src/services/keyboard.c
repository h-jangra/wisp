#define _GNU_SOURCE
#include "services/keyboard.h"
#include "scripting/scripting.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <xkbcommon/xkbcommon.h>

static struct key_binding g_bindings[MAX_KEY_BINDINGS];
static int g_binding_count = 0;
static uint32_t g_next_id = 1;

void keyboard_init(void)
{
	memset(g_bindings, 0, sizeof(g_bindings));
	for (int i = 0; i < MAX_KEY_BINDINGS; i++)
		g_bindings[i].lua_cb_ref = LUA_NOREF;
	g_binding_count = 0;
	g_next_id = 1;
}

void keyboard_shutdown(void)
{
	for (int i = 0; i < MAX_KEY_BINDINGS; i++) {
		if (g_bindings[i].active) {
			if (Lm && g_bindings[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_bindings[i].lua_cb_ref);
			g_bindings[i].active = false;
			g_bindings[i].lua_cb_ref = LUA_NOREF;
		}
	}
	g_binding_count = 0;
}

int keyboard_binding_count(void)
{
	return g_binding_count;
}

bool keyboard_parse_combo(const char *combo_str, uint32_t *out_sym, uint32_t *out_mods)
{
	if (!combo_str || !out_sym || !out_mods)
		return false;

	char buf[128];
	strncpy(buf, combo_str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	uint32_t mods = 0;
	uint32_t sym = 0;

	char *saveptr = NULL;
	char *token = strtok_r(buf, "+-", &saveptr);
	while (token) {
		/* Trim leading/trailing whitespace */
		while (*token == ' ' || *token == '\t') token++;
		char *end = token + strlen(token) - 1;
		while (end > token && (*end == ' ' || *end == '\t')) {
			*end = '\0';
			end--;
		}

		if (strcasecmp(token, "ctrl") == 0 || strcasecmp(token, "control") == 0) {
			mods |= KEY_MOD_CTRL;
		} else if (strcasecmp(token, "shift") == 0) {
			mods |= KEY_MOD_SHIFT;
		} else if (strcasecmp(token, "alt") == 0 || strcasecmp(token, "mod1") == 0) {
			mods |= KEY_MOD_ALT;
		} else if (strcasecmp(token, "super") == 0 || strcasecmp(token, "win") == 0 ||
			   strcasecmp(token, "logo") == 0 || strcasecmp(token, "mod4") == 0) {
			mods |= KEY_MOD_SUPER;
		} else {
			/* Key symbol */
			xkb_keysym_t s = XKB_KEY_NoSymbol;
			if (strcasecmp(token, "enter") == 0 || strcasecmp(token, "return") == 0)
				s = XKB_KEY_Return;
			else if (strcasecmp(token, "esc") == 0 || strcasecmp(token, "escape") == 0)
				s = XKB_KEY_Escape;
			else if (strcasecmp(token, "space") == 0)
				s = XKB_KEY_space;
			else if (strcasecmp(token, "backspace") == 0)
				s = XKB_KEY_BackSpace;
			else if (strcasecmp(token, "tab") == 0)
				s = XKB_KEY_Tab;
			else if (strcasecmp(token, "del") == 0 || strcasecmp(token, "delete") == 0)
				s = XKB_KEY_Delete;
			else if (strcasecmp(token, "ins") == 0 || strcasecmp(token, "insert") == 0)
				s = XKB_KEY_Insert;
			else if (strcasecmp(token, "up") == 0)
				s = XKB_KEY_Up;
			else if (strcasecmp(token, "down") == 0)
				s = XKB_KEY_Down;
			else if (strcasecmp(token, "left") == 0)
				s = XKB_KEY_Left;
			else if (strcasecmp(token, "right") == 0)
				s = XKB_KEY_Right;
			else if (strcasecmp(token, "pageup") == 0 || strcasecmp(token, "pgup") == 0)
				s = XKB_KEY_Page_Up;
			else if (strcasecmp(token, "pagedown") == 0 || strcasecmp(token, "pgdn") == 0)
				s = XKB_KEY_Page_Down;
			else if (strcasecmp(token, "home") == 0)
				s = XKB_KEY_Home;
			else if (strcasecmp(token, "end") == 0)
				s = XKB_KEY_End;
			else
				s = xkb_keysym_from_name(token, XKB_KEYSYM_CASE_INSENSITIVE);

			if (s != XKB_KEY_NoSymbol)
				sym = xkb_keysym_to_lower(s);
		}
		token = strtok_r(NULL, "+-", &saveptr);
	}

	if (sym == 0)
		return false;

	*out_sym = sym;
	*out_mods = mods;
	return true;
}

uint32_t keyboard_bind(const char *combo_str, int lua_cb_ref, int widget_slot, int app_slot, bool allow_repeat)
{
	uint32_t sym = 0, mods = 0;
	if (!keyboard_parse_combo(combo_str, &sym, &mods))
		return 0;

	for (int i = 0; i < MAX_KEY_BINDINGS; i++) {
		if (!g_bindings[i].active) {
			struct key_binding *b = &g_bindings[i];
			b->id = g_next_id++;
			b->keysym = sym;
			b->mods = mods;
			b->allow_repeat = allow_repeat;
			b->lua_cb_ref = lua_cb_ref;
			b->widget_slot = widget_slot;
			b->app_slot = app_slot;
			b->active = true;
			g_binding_count++;
			return b->id;
		}
	}
	return 0;
}

bool keyboard_unbind(uint32_t binding_id)
{
	if (binding_id == 0)
		return false;
	for (int i = 0; i < MAX_KEY_BINDINGS; i++) {
		if (g_bindings[i].active && g_bindings[i].id == binding_id) {
			if (Lm && g_bindings[i].lua_cb_ref != LUA_NOREF) {
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_bindings[i].lua_cb_ref);
				g_bindings[i].lua_cb_ref = LUA_NOREF;
			}
			g_bindings[i].active = false;
			g_binding_count--;
			return true;
		}
	}
	return false;
}

void keyboard_cleanup_app(int app_slot)
{
	if (app_slot < 0)
		return;
	for (int i = 0; i < MAX_KEY_BINDINGS; i++) {
		if (g_bindings[i].active && g_bindings[i].app_slot == app_slot) {
			if (Lm && g_bindings[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_bindings[i].lua_cb_ref);
			g_bindings[i].active = false;
			g_bindings[i].lua_cb_ref = LUA_NOREF;
			g_binding_count--;
		}
	}
}

void keyboard_cleanup_widget(int widget_slot)
{
	if (widget_slot < 0)
		return;
	for (int i = 0; i < MAX_KEY_BINDINGS; i++) {
		if (g_bindings[i].active && g_bindings[i].widget_slot == widget_slot) {
			if (Lm && g_bindings[i].lua_cb_ref != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, g_bindings[i].lua_cb_ref);
			g_bindings[i].active = false;
			g_bindings[i].lua_cb_ref = LUA_NOREF;
			g_binding_count--;
		}
	}
}

bool keyboard_dispatch_key(uint32_t sym, uint32_t mods, bool is_press, bool is_repeat,
			   int focused_widget_slot, int focused_app_slot)
{
	if (!is_press)
		return false;

	uint32_t sym_lower = xkb_keysym_to_lower(sym);
	bool handled = false;

	for (int i = 0; i < MAX_KEY_BINDINGS; i++) {
		struct key_binding *b = &g_bindings[i];
		if (!b->active)
			continue;
		if (is_repeat && !b->allow_repeat)
			continue;
		if (b->keysym != sym_lower || b->mods != mods)
			continue;

		/* Check application and widget scope */
		if (b->app_slot >= 0 && b->app_slot != focused_app_slot)
			continue;
		if (b->widget_slot >= 0 && b->widget_slot != focused_widget_slot)
			continue;

		if (Lm && b->lua_cb_ref != LUA_NOREF) {
			lua_ticks_total = LUA_TICKS_DEFAULT;
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, b->lua_cb_ref);
			if (lua_isfunction(Lm, -1)) {
				if (lua_pcall(Lm, 0, 0, 0) != LUA_OK) {
					fprintf(stderr, "wisp: hotkey callback error: %s\n",
						lua_tostring(Lm, -1));
					lua_pop(Lm, 1);
				}
			} else {
				lua_pop(Lm, 1);
			}
			handled = true;
		}
	}
	return handled;
}
