#define _GNU_SOURCE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wc23-extensions"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "scripting/scripting.h"
#include "core/types.h"
#include "core/widget.h"
#include "render/shapes.h"
#include "render/text.h"
#include "render/image.h"
#include "render/buffer.h"
#include "render/clay_render.h"
#include "layout/layout.h"
#include "layout/clay_layout.h"
#include "services/system.h"
#include "wayland/wayland.h"
#include "core/config.h"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>

#if WISP_HAS_LUA

lua_State *Lm;
size_t lua_used, lua_limit;
long lua_ticks_total = LUA_TICKS_DEFAULT;
static long lua_ticks;
struct widget *lua_cur;
struct widget *lua_ctx;
struct rect *lua_dmg;

#define MAX_TIMERS 64

struct wisp_timer {
	int id;
	struct widget *owner;
	int lua_cb;
	int64_t next_us;
	int32_t interval_ms;
	bool repeat;
	bool active;
};

static struct wisp_timer timers[MAX_TIMERS];
static int next_timer_id = 1;

static int timer_add(struct widget *owner, int lua_cb, int32_t interval_ms, bool repeat)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!timers[i].active) {
			timers[i].id = next_timer_id++;
			timers[i].owner = owner;
			timers[i].lua_cb = lua_cb;
			timers[i].interval_ms = interval_ms;
			timers[i].repeat = repeat;
			timers[i].next_us = now_us() + (int64_t)interval_ms * 1000;
			timers[i].active = true;
			return timers[i].id;
		}
	}
	return -1;
}

void timer_cancel(int id)
{
	if (id <= 0)
		return;
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i].active && timers[i].id == id) {
			if (Lm && timers[i].lua_cb != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, timers[i].lua_cb);
			timers[i].lua_cb = LUA_NOREF;
			timers[i].active = false;
			timers[i].owner = NULL;
			break;
		}
	}
}

static void timers_cleanup_for_widget(struct widget *wgt)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i].active && timers[i].owner == wgt) {
			if (Lm && timers[i].lua_cb != LUA_NOREF)
				luaL_unref(Lm, LUA_REGISTRYINDEX, timers[i].lua_cb);
			timers[i].lua_cb = LUA_NOREF;
			timers[i].active = false;
			timers[i].owner = NULL;
		}
	}
}

static int *lua_event_slot(struct widget *wgt, const char *ev)
{
	if (!wgt)
		return NULL;
	if (strcmp(ev, "build") == 0)
		return &wgt->lua_build;
	if (strcmp(ev, "draw") == 0)
		return &wgt->lua_draw;
	if (strcmp(ev, "timer") == 0)
		return &wgt->lua_timer;
	if (strcmp(ev, "event") == 0)
		return &wgt->lua_event;
	if (strcmp(ev, "click") == 0)
		return &wgt->lua_click;
	if (strcmp(ev, "hover") == 0)
		return &wgt->lua_hover;
	if (strcmp(ev, "leave") == 0)
		return &wgt->lua_leave;
	if (strcmp(ev, "press") == 0)
		return &wgt->lua_press;
	if (strcmp(ev, "release") == 0)
		return &wgt->lua_release;
	if (strcmp(ev, "scroll") == 0)
		return &wgt->lua_scroll;
	return NULL;
}

static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	(void)ud;
	if (nsize == 0) {
		lua_used -= osize;
		free(ptr);
		return NULL;
	}
	if (lua_limit && (lua_used - osize + nsize > lua_limit))
		return NULL;
	void *np = realloc(ptr, nsize);
	if (np)
		lua_used = lua_used - osize + nsize;
	return np;
}

static void lua_tick_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	if ((lua_ticks -= 1000) > 0)
		return;
	luaL_error(L, "script exceeded instruction budget");
}

static void lua_report(lua_State *L, struct widget *wgt, const char *what)
{
	const char *err = lua_tostring(L, -1);
	fprintf(stderr, "wisp: lua[%s] %s: %s\n", wgt ? wgt->name : "core", what,
		err ? err : "unknown error");
	lua_pop(L, 1);
}

bool lua_script_call(struct widget *wgt, int *ref, const char *what, const char *arg)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	int nargs = 0;
	if (arg) {
		lua_pushstring(L, arg);
		nargs = 1;
	}
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		luaL_unref(Lm ? Lm : L, LUA_REGISTRYINDEX, *ref);
		*ref = LUA_NOREF;
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	if (wgt->want_destroy && wgt->state == BUF_AVAILABLE)
		widget_teardown(wgt);
	return ok;
}

void lua_event(struct widget *wgt, const char *ev)
{
	if (!wgt || !wgt->used || wgt->want_destroy)
		return;
	int *slot = lua_event_slot(wgt, ev);
	if (!slot)
		return;
	lua_script_call(wgt, slot, ev, ev);
}

void lua_dispatch(struct widget *wgt, struct widget *target, const char *ev)
{
	if (!target || !target->used || target->want_destroy)
		return;
	int *slot = lua_event_slot(target, ev);
	if (!slot || *slot == LUA_NOREF)
		return;
	lua_script_call(wgt, slot, ev, ev);
}

bool lua_script_call_xy(struct widget *wgt, int *ref, const char *what, int32_t x, int32_t y)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushinteger(L, x);
	lua_pushinteger(L, y);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		luaL_unref(Lm ? Lm : L, LUA_REGISTRYINDEX, *ref);
		*ref = LUA_NOREF;
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	if (wgt->want_destroy && wgt->state == BUF_AVAILABLE)
		widget_teardown(wgt);
	return ok;
}

bool lua_script_call_scroll(struct widget *wgt, int *ref, const char *what, int dir, int32_t x, int32_t y)
{
	if (!wgt || !wgt->used || wgt->want_destroy || !wgt->lua_L || *ref == LUA_NOREF)
		return false;
	lua_State *L = wgt->lua_L;
	lua_sethook(L, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
	lua_pushinteger(L, dir);
	lua_pushinteger(L, x);
	lua_pushinteger(L, y);
	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	bool ok = true;
	if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
		lua_report(L, wgt, what);
		luaL_unref(Lm ? Lm : L, LUA_REGISTRYINDEX, *ref);
		*ref = LUA_NOREF;
		ok = false;
	}
	lua_cur = prev_cur;
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, top);
	if (wgt->want_destroy && wgt->state == BUF_AVAILABLE)
		widget_teardown(wgt);
	return ok;
}

void lua_dispatch_click(struct widget *wgt, struct widget *target, int32_t x, int32_t y)
{
	if (!target || !target->used || target->want_destroy)
		return;
	int *slot = lua_event_slot(target, "click");
	if (!slot || *slot == LUA_NOREF)
		return;
	lua_script_call_xy(wgt, slot, "click", x, y);
	wgt->want_redraw = 1;
}

void lua_dispatch_scroll(struct widget *wgt, struct widget *target, int dir, int32_t x, int32_t y)
{
	if (!target || !target->used || target->want_destroy)
		return;
	int *slot = lua_event_slot(target, "scroll");
	if (!slot || *slot == LUA_NOREF)
		return;
	lua_script_call_scroll(wgt, slot, "scroll", dir, x, y);
	wgt->want_redraw = 1;
}

static uint32_t parse_color_value(lua_State *L, int idx)
{
	if (lua_isnoneornil(L, idx))
		return 0xFFFFFFFFu;
	if (lua_type(L, idx) == LUA_TSTRING) {
		const char *s = lua_tostring(L, idx);
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '#')
			s++;
		else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			s += 2;
		char *end = NULL;
		unsigned long v = strtoul(s, &end, 16);
		size_t len = end ? (size_t)(end - s) : 0;
		if (len == 6)
			return 0xFF000000u | (uint32_t)v;
		if (len == 8)
			return (uint32_t)v;
		if (len == 3) {
			unsigned int r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
			return 0xFF000000u | (r * 0x11u << 16) | (g * 0x11u << 8) | (b * 0x11u);
		}
		return 0xFFFFFFFFu;
	}
	return (uint32_t)luaL_checkinteger(L, idx);
}

/* --- wisp.system --- */
static int lua_system_battery(lua_State *L)
{
	struct sys_battery b;
	sys_get_battery(&b);
	lua_createtable(L, 0, 5);
	lua_pushboolean(L, b.present);
	lua_setfield(L, -2, "present");
	lua_pushinteger(L, b.percentage);
	lua_setfield(L, -2, "percentage");
	lua_pushstring(L, b.status);
	lua_setfield(L, -2, "status");
	lua_pushboolean(L, b.charging);
	lua_setfield(L, -2, "charging");
	lua_pushnumber(L, b.power_watts);
	lua_setfield(L, -2, "power_watts");
	return 1;
}

static int lua_system_cpu(lua_State *L)
{
	struct sys_cpu c;
	sys_get_cpu(&c);
	lua_createtable(L, 0, 1);
	lua_pushnumber(L, c.usage);
	lua_setfield(L, -2, "usage");
	return 1;
}

static int lua_system_memory(lua_State *L)
{
	struct sys_memory m;
	sys_get_memory(&m);
	lua_createtable(L, 0, 5);
	lua_pushinteger(L, m.total_mb);
	lua_setfield(L, -2, "total_mb");
	lua_pushinteger(L, m.used_mb);
	lua_setfield(L, -2, "used_mb");
	lua_pushinteger(L, m.free_mb);
	lua_setfield(L, -2, "free_mb");
	lua_pushinteger(L, m.avail_mb);
	lua_setfield(L, -2, "avail_mb");
	lua_pushnumber(L, m.percentage);
	lua_setfield(L, -2, "percentage");
	return 1;
}

static int lua_system_uptime(lua_State *L)
{
	lua_pushnumber(L, sys_get_uptime());
	return 1;
}

static int lua_system_loadavg(lua_State *L)
{
	double l1, l5, l15;
	sys_get_loadavg(&l1, &l5, &l15);
	lua_pushnumber(L, l1);
	lua_pushnumber(L, l5);
	lua_pushnumber(L, l15);
	return 3;
}

static const luaL_Reg lua_system_api[] = {
	{"battery", lua_system_battery},
	{"cpu", lua_system_cpu},
	{"memory", lua_system_memory},
	{"uptime", lua_system_uptime},
	{"loadavg", lua_system_loadavg},
	{NULL, NULL},
};

/* --- wisp.style --- */
static int lua_style_color(lua_State *L)
{
	uint32_t c = parse_color_value(L, 1);
	lua_pushinteger(L, (lua_Integer)c);
	return 1;
}

static const luaL_Reg lua_style_api[] = {
	{"color", lua_style_color},
	{NULL, NULL},
};

/* --- wisp.timer --- */
static int lua_timer_every(lua_State *L)
{
	int32_t ms = (int32_t)luaL_checkinteger(L, 1);
	if (ms < 10)
		ms = 10;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	struct widget *owner = lua_ctx ? lua_ctx : lua_cur;
	int id = timer_add(owner, ref, ms, true);
	lua_pushinteger(L, id);
	return 1;
}

static int lua_timer_after(lua_State *L)
{
	int32_t ms = (int32_t)luaL_checkinteger(L, 1);
	if (ms < 1)
		ms = 1;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	struct widget *owner = lua_ctx ? lua_ctx : lua_cur;
	int id = timer_add(owner, ref, ms, false);
	lua_pushinteger(L, id);
	return 1;
}

static int lua_timer_cancel(lua_State *L)
{
	int id = (int)luaL_checkinteger(L, 1);
	timer_cancel(id);
	return 0;
}

static const luaL_Reg lua_timer_api[] = {
	{"every", lua_timer_every},
	{"after", lua_timer_after},
	{"cancel", lua_timer_cancel},
	{NULL, NULL},
};

/* --- wisp.widget --- */
static struct widget *resolve_widget_arg(lua_State *L, int idx)
{
	if (lua_istable(L, idx)) {
		lua_getfield(L, idx, "id");
		if (lua_isinteger(L, -1)) {
			int id = (int)lua_tointeger(L, -1);
			lua_pop(L, 1);
			if (id >= 0 && id < MAX_WIDGETS && widgets[id].used)
				return &widgets[id];
		} else {
			lua_pop(L, 1);
		}
	} else if (lua_isstring(L, idx)) {
		const char *name = lua_tostring(L, idx);
		for (int k = 0; k < MAX_WIDGETS; k++) {
			if (widgets[k].used && strcmp(widgets[k].name, name) == 0)
				return &widgets[k];
		}
	}
	return lua_cur ? lua_cur : lua_ctx;
}

static int lua_widget_redraw(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		w->want_redraw = 1;
	return 0;
}

static int lua_widget_show(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_show(w);
	return 0;
}

static int lua_widget_hide(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_hide(w);
	return 0;
}

static int lua_widget_size(lua_State *L)
{
	struct widget *w = lua_cur ? lua_cur : lua_ctx;
	int arg = 1;
	if (lua_istable(L, 1) || lua_isstring(L, 1)) {
		w = resolve_widget_arg(L, 1);
		arg = 2;
	}
	if (!w || !w->used)
		return 0;
	int nw = (int)luaL_checkinteger(L, arg);
	int nh = (int)luaL_checkinteger(L, arg + 1);
	if (nw > 0) w->pend_w = (uint32_t)(nw < 8 ? 8 : (nw > 4096 ? 4096 : nw));
	if (nh > 0) w->pend_h = (uint32_t)(nh < 8 ? 8 : (nh > 4096 ? 4096 : nh));
	w->want_resize = 1;
	return 0;
}

static int lua_widget_anchor(lua_State *L)
{
	struct widget *w = lua_cur ? lua_cur : lua_ctx;
	int arg = 1;
	if (lua_istable(L, 1) || lua_isstring(L, 1)) {
		if (lua_gettop(L) >= 2) {
			w = resolve_widget_arg(L, 1);
			arg = 2;
		}
	}
	if (!w || !w->used)
		return 0;
	const char *s = luaL_checkstring(L, arg);
	w->anchor = parse_anchor(s);
	if (w->layer_surface)
		zwlr_layer_surface_v1_set_anchor(w->layer_surface, w->anchor);
	return 0;
}

static int lua_widget_destroy(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (w && w->used)
		widget_destroy(w);
	return 0;
}

static int lua_widget_get(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used && strcmp(widgets[k].name, name) == 0) {
			lua_createtable(L, 0, 2);
			lua_pushinteger(L, k);
			lua_setfield(L, -2, "id");
			lua_pushstring(L, widgets[k].name);
			lua_setfield(L, -2, "name");
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

static int lua_widget_list(lua_State *L)
{
	lua_newtable(L);
	int idx = 1;
	for (int k = 0; k < MAX_WIDGETS; k++) {
		if (widgets[k].used) {
			lua_createtable(L, 0, 2);
			lua_pushinteger(L, k);
			lua_setfield(L, -2, "id");
			lua_pushstring(L, widgets[k].name);
			lua_setfield(L, -2, "name");
			lua_rawseti(L, -2, idx++);
		}
	}
	return 1;
}

static int lua_widget_load_script(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	struct widget *w = widget_load_dynamic(path);
	if (!w) {
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 2);
	lua_pushinteger(L, (int)(w - widgets));
	lua_setfield(L, -2, "id");
	lua_pushstring(L, w->name);
	lua_setfield(L, -2, "name");
	return 1;
}

static int lua_widget_unload_script(lua_State *L)
{
	const char *target = luaL_checkstring(L, 1);
	bool ok = widget_unload_dynamic(target);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_widget_reload_script(lua_State *L)
{
	struct widget *w = resolve_widget_arg(L, 1);
	if (!w || !w->used) {
		lua_pushboolean(L, false);
		return 1;
	}
	bool ok = lua_widget_reload(w);
	lua_pushboolean(L, ok);
	return 1;
}

static int lua_widget_memory(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)lua_used);
	lua_pushinteger(L, (lua_Integer)lua_limit);
	return 2;
}

static int lua_widget_limits(lua_State *L)
{
	lua_createtable(L, 0, 4);
	lua_pushinteger(L, MAX_WIDGETS);
	lua_setfield(L, -2, "max_widgets");
	lua_pushinteger(L, (lua_Integer)lua_limit);
	lua_setfield(L, -2, "lua_mem_limit");
	lua_pushinteger(L, lua_ticks_total);
	lua_setfield(L, -2, "lua_insn_limit");
	lua_pushinteger(L, LUA_CHUNK_MAX);
	lua_setfield(L, -2, "lua_chunk_max");
	return 1;
}

static int lua_widget_scale(lua_State *L)
{
	struct widget *w = lua_cur ? lua_cur : lua_ctx;
	if (lua_isinteger(L, 1)) {
		int factor = (int)lua_tointeger(L, 1);
		if (w && w->used && factor >= 1 && factor <= 4)
			surface_preferred_scale(w, w->surface, factor);
	}
	if (w)
		lua_pushinteger(L, w->scale > 0 ? w->scale : 1);
	else
		lua_pushinteger(L, 1);
	return 1;
}

static int lua_widget_create(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	struct widget *wgt = lua_ctx ? lua_ctx : lua_cur;
	if (!wgt) {
		wgt = widget_add(true, 100, 50, NULL);
		if (!wgt)
			return 0;
	}

	lua_getfield(L, 1, "name");
	if (lua_isstring(L, -1))
		snprintf(wgt->name, sizeof(wgt->name), "%s", lua_tostring(L, -1));
	lua_pop(L, 1);

	int w = 0, h = 0;
	lua_getfield(L, 1, "size");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "width");
		w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "height");
		h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	if (w <= 0) {
		lua_getfield(L, 1, "width");
		w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (h <= 0) {
		lua_getfield(L, 1, "height");
		h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (w > 0)
		wgt->pend_w = wgt->width = (uint32_t)(w < 8 ? 8 : (w > 4096 ? 4096 : w));
	if (h > 0)
		wgt->pend_h = wgt->height = (uint32_t)(h < 8 ? 8 : (h > 4096 ? 4096 : h));

	lua_getfield(L, 1, "anchor");
	if (lua_isstring(L, -1))
		wgt->anchor = parse_anchor(lua_tostring(L, -1));
	lua_pop(L, 1);

	lua_getfield(L, 1, "margin");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) wgt->margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) wgt->margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) wgt->margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) wgt->margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1)) {
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) wgt->margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "visible");
	if (!lua_isnil(L, -1))
		wgt->visible = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "build");
	if (lua_isfunction(L, -1)) {
		if (wgt->lua_build != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, wgt->lua_build);
		wgt->lua_build = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_pop(L, 1);
	}

	const char *evt_names[] = {
		"on_click", "click",
		"on_scroll", "scroll",
		"on_hover", "hover",
		"on_leave", "leave",
		"on_press", "press",
		"on_release", "release"
	};
	int *evt_slots[] = {
		&wgt->lua_click, &wgt->lua_click,
		&wgt->lua_scroll, &wgt->lua_scroll,
		&wgt->lua_hover, &wgt->lua_hover,
		&wgt->lua_leave, &wgt->lua_leave,
		&wgt->lua_press, &wgt->lua_press,
		&wgt->lua_release, &wgt->lua_release
	};
	for (size_t k = 0; k < sizeof(evt_names)/sizeof(evt_names[0]); k++) {
		lua_getfield(L, 1, evt_names[k]);
		if (lua_isfunction(L, -1)) {
			if (*evt_slots[k] != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, *evt_slots[k]);
			*evt_slots[k] = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			lua_pop(L, 1);
		}
	}

	if (kid_interactive(wgt) && wgt->surface)
		widget_input_region(wgt);

	if (wgt->layer_surface) {
		zwlr_layer_surface_v1_set_size(wgt->layer_surface, wgt->width, wgt->height);
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2], wgt->margin[3]);
	}
	wgt->want_redraw = 1;

	lua_createtable(L, 0, 3);
	lua_pushinteger(L, (int)(wgt - widgets));
	lua_setfield(L, -2, "id");
	lua_pushstring(L, wgt->name);
	lua_setfield(L, -2, "name");
	lua_pushcfunction(L, lua_widget_redraw);
	lua_setfield(L, -2, "redraw");
	return 1;
}

static struct config_staging *cur_stage = NULL;

bool lua_widget_apply_spec(struct widget *wgt, const struct staged_widget_spec *spec)
{
	if (!wgt || !spec)
		return false;

	wgt->from_config = true;
	if (spec->name[0])
		snprintf(wgt->name, sizeof(wgt->name), "%s", spec->name);

	if (spec->path[0]) {
		if (!lua_widget_load(wgt, spec->path))
			return false;
	} else if (spec->build_ref != LUA_NOREF) {
		if (!wgt->lua_L && Lm) {
			wgt->lua_L = lua_newthread(Lm);
			wgt->lua_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
		}
		if (wgt->lua_build != LUA_NOREF && Lm)
			luaL_unref(Lm, LUA_REGISTRYINDEX, wgt->lua_build);
		lua_rawgeti(Lm, LUA_REGISTRYINDEX, spec->build_ref);
		wgt->lua_build = luaL_ref(Lm, LUA_REGISTRYINDEX);
		if (spec->width > 0 && spec->height > 0) {
			wgt->width = spec->width;
			wgt->height = spec->height;
			wgt->pend_w = spec->width;
			wgt->pend_h = spec->height;
		}
	}

	if (spec->has_size) {
		wgt->width = spec->width;
		wgt->height = spec->height;
		wgt->pend_w = spec->width;
		wgt->pend_h = spec->height;
	}
	if (spec->has_anchor)
		wgt->anchor = spec->anchor;
	if (spec->has_margin)
		memcpy(wgt->margin, spec->margin, sizeof(wgt->margin));
	if (spec->has_visible)
		wgt->visible = spec->visible;
	if (spec->scale >= 1 && spec->scale <= 4)
		wgt->scale = spec->scale;

	if (wgt->layer_surface) {
		zwlr_layer_surface_v1_set_size(wgt->layer_surface, wgt->width, wgt->height);
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2],
						 wgt->margin[3]);
	}
	wgt->want_redraw = 1;
	return true;
}

static int lua_wisp_setup(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	lua_getfield(L, 1, "scale");
	if (lua_isinteger(L, -1)) {
		int sc = (int)lua_tointeger(L, -1);
		if (sc >= 1 && sc <= 4) {
			if (cur_stage)
				cur_stage->scale = sc;
			else {
				for (int k = 0; k < MAX_WIDGETS; k++)
					if (widgets[k].used)
						surface_preferred_scale(&widgets[k], widgets[k].surface, sc);
			}
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "font");
	if (lua_isstring(L, -1)) {
		const char *f = lua_tostring(L, -1);
		if (cur_stage)
			snprintf(cur_stage->font, sizeof(cur_stage->font), "%s", f);
		else
			setenv("WISP_FONT", f, 1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "font_size");
	if (lua_isinteger(L, -1)) {
		int fs = (int)lua_tointeger(L, -1);
		if (fs > 0) {
			if (cur_stage)
				cur_stage->font_size = fs;
			else {
				char b[16];
				snprintf(b, sizeof(b), "%d", fs);
				setenv("WISP_SIZE", b, 1);
			}
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "default_anchor");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		lua_getfield(L, 1, "anchor");
	}
	if (lua_isstring(L, -1)) {
		uint32_t a = parse_anchor(lua_tostring(L, -1));
		if (cur_stage) {
			cur_stage->default_anchor = a;
			cur_stage->has_default_anchor = true;
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "default_margin");
	if (!lua_istable(L, -1) && !lua_isinteger(L, -1)) {
		lua_pop(L, 1);
		lua_getfield(L, 1, "margin");
	}
	if (lua_istable(L, -1) && cur_stage) {
		cur_stage->has_default_margin = true;
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) cur_stage->default_margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1) && cur_stage) {
		cur_stage->has_default_margin = true;
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) cur_stage->default_margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "lua_mem_limit");
	if (lua_isinteger(L, -1)) {
		size_t lim = (size_t)lua_tointeger(L, -1);
		if (cur_stage)
			cur_stage->lua_mem_limit = lim;
		else
			lua_limit = lim;
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "lua_insn_limit");
	if (lua_isinteger(L, -1)) {
		long insn = (long)lua_tointeger(L, -1);
		if (cur_stage)
			cur_stage->lua_insn_limit = insn;
		else
			lua_ticks_total = insn;
	}
	lua_pop(L, 1);

	return 0;
}

static int lua_wisp_widget_register(lua_State *L)
{
	int table_idx = 1;
	const char *pos_name = NULL;
	if (lua_isstring(L, 1)) {
		pos_name = lua_tostring(L, 1);
		table_idx = 2;
	}
	luaL_checktype(L, table_idx, LUA_TTABLE);

	struct staged_widget_spec spec;
	memset(&spec, 0, sizeof(spec));
	spec.build_ref = LUA_NOREF;
	spec.visible = true;

	if (pos_name && pos_name[0]) {
		snprintf(spec.name, sizeof(spec.name), "%s", pos_name);
	} else {
		lua_getfield(L, table_idx, "name");
		if (lua_isstring(L, -1))
			snprintf(spec.name, sizeof(spec.name), "%s", lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	lua_getfield(L, table_idx, "path");
	if (lua_isstring(L, -1))
		snprintf(spec.path, sizeof(spec.path), "%s", lua_tostring(L, -1));
	lua_pop(L, 1);

	if (!spec.name[0]) {
		if (spec.path[0]) {
			const char *slash = strrchr(spec.path, '/');
			const char *base = slash ? slash + 1 : spec.path;
			snprintf(spec.name, sizeof(spec.name), "%s", base);
			char *dot = strrchr(spec.name, '.');
			if (dot) *dot = '\0';
		} else {
			static int anon_count = 0;
			snprintf(spec.name, sizeof(spec.name), "widget_%d", ++anon_count);
		}
	}

	int w = 0, h = 0;
	lua_getfield(L, table_idx, "size");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "width");
		if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "height");
		if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	if (w <= 0) {
		lua_getfield(L, table_idx, "width");
		if (lua_isinteger(L, -1)) w = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (h <= 0) {
		lua_getfield(L, table_idx, "height");
		if (lua_isinteger(L, -1)) h = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (w > 0 && h > 0) {
		spec.width = (uint32_t)(w < 8 ? 8 : (w > 4096 ? 4096 : w));
		spec.height = (uint32_t)(h < 8 ? 8 : (h > 4096 ? 4096 : h));
		spec.has_size = true;
	}

	lua_getfield(L, table_idx, "anchor");
	if (lua_isstring(L, -1)) {
		spec.anchor = parse_anchor(lua_tostring(L, -1));
		spec.has_anchor = true;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "margin");
	if (lua_istable(L, -1)) {
		spec.has_margin = true;
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) spec.margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) spec.margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) spec.margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) spec.margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1)) {
		spec.has_margin = true;
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) spec.margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "visible");
	if (!lua_isnil(L, -1)) {
		spec.visible = lua_toboolean(L, -1);
		spec.has_visible = true;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "scale");
	if (lua_isinteger(L, -1)) {
		int sc = (int)lua_tointeger(L, -1);
		if (sc >= 1 && sc <= 4)
			spec.scale = sc;
	}
	lua_pop(L, 1);

	lua_getfield(L, table_idx, "build");
	if (lua_isfunction(L, -1)) {
		spec.build_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_pop(L, 1);
	}

	if (cur_stage) {
		if (cur_stage->widget_count < MAX_WIDGETS) {
			if (!spec.has_anchor && cur_stage->has_default_anchor) {
				spec.anchor = cur_stage->default_anchor;
				spec.has_anchor = true;
			}
			if (!spec.has_margin && cur_stage->has_default_margin) {
				memcpy(spec.margin, cur_stage->default_margin, sizeof(spec.margin));
				spec.has_margin = true;
			}
			cur_stage->widgets[cur_stage->widget_count++] = spec;
		}
	} else {
		struct widget *w = widget_add(spec.has_visible ? spec.visible : true,
					      spec.width > 0 ? spec.width : 100,
					      spec.height > 0 ? spec.height : 50, NULL);
		if (w)
			lua_widget_apply_spec(w, &spec);
	}

	lua_createtable(L, 0, 2);
	lua_pushstring(L, spec.name);
	lua_setfield(L, -2, "name");
	return 1;
}

static const luaL_Reg lua_widget_api[] = {
	{"create", lua_widget_create},
	{"register", lua_wisp_widget_register},
	{"redraw", lua_widget_redraw},
	{"show", lua_widget_show},
	{"hide", lua_widget_hide},
	{"size", lua_widget_size},
	{"anchor", lua_widget_anchor},
	{"destroy", lua_widget_destroy},
	{"get", lua_widget_get},
	{"list", lua_widget_list},
	{"load", lua_widget_load_script},
	{"unload", lua_widget_unload_script},
	{"reload", lua_widget_reload_script},
	{"memory", lua_widget_memory},
	{"limits", lua_widget_limits},
	{"scale", lua_widget_scale},
	{NULL, NULL},
};

/* --- Clay layout build pipeline --- */
static int opt_table_int(lua_State *L, int t, const char *k, int def)
{
	lua_getfield(L, t, k);
	int val = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : def;
	lua_pop(L, 1);
	return val;
}

static double opt_table_number(lua_State *L, int t, const char *k, double def)
{
	lua_getfield(L, t, k);
	double val = lua_isnumber(L, -1) ? (double)lua_tonumber(L, -1) : def;
	lua_pop(L, 1);
	return val;
}

static uint32_t opt_table_color(lua_State *L, int t, const char *k, uint32_t def)
{
	lua_getfield(L, t, k);
	uint32_t val = lua_isnil(L, -1) ? def : parse_color_value(L, -1);
	lua_pop(L, 1);
	return val;
}

static const char *opt_table_string(lua_State *L, int t, const char *k)
{
	lua_getfield(L, t, k);
	const char *val = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	return val;
}

/*
 * Recursively translate a Lua table tree into Clay element declarations.
 * Each node must have a "_type" field (injected by wisp.layout.* / wisp.ui.* helpers).
 * If "_type" is absent, the node is treated as "text" when it has a "text" field,
 * or as a "container" otherwise.
 * Depth is capped at 32 to prevent unbounded recursion from malformed scripts.
 *
 * Progress bar is implemented as two nested Clay elements:
 *   outer = background rect (SIZING_GROW width, fixed height)
 *   inner = bar fill        (SIZING_PERCENT(pct) width, SIZING_GROW height)
 */
static void clay_build_from_lua(lua_State *L, int idx, int depth)
{
	if (depth > 32 || !lua_istable(L, idx))
		return;

	idx = lua_absindex(L, idx);
	const char *type = opt_table_string(L, idx, "_type");

	if (!type) {
		lua_getfield(L, idx, "text");
		if (lua_isstring(L, -1))
			type = "text";
		else
			type = "container";
		lua_pop(L, 1);
	}

	if (strcmp(type, "text") == 0) {
		const char *text = opt_table_string(L, idx, "text");
		if (!text) {
			lua_rawgeti(L, idx, 1);
			if (lua_isstring(L, -1))
				text = lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		if (!text)
			text = "";

		int size = opt_table_int(L, idx, "size", 14);
		uint32_t col = opt_table_color(L, idx, "color", 0xFFFFFFFFu);
		const char *wrap = opt_table_string(L, idx, "wrap");

		Clay_TextElementConfigWrapMode wrap_mode = CLAY_TEXT_WRAP_NONE;
		if (wrap && strcmp(wrap, "words") == 0)
			wrap_mode = CLAY_TEXT_WRAP_WORDS;
		else if (wrap && strcmp(wrap, "all") == 0)
			wrap_mode = CLAY_TEXT_WRAP_NEWLINES;

		Clay_TextElementConfig textConfig = {
			.textColor = clay_color_from_argb(col),
			.fontSize = (uint16_t)size,
			.wrapMode = wrap_mode
		};
		Clay_String str = {
			.length = (int32_t)strlen(text),
			.chars = text
		};
		Clay__OpenTextElement(str, textConfig);
		return;
	}

	if (strcmp(type, "spacer") == 0) {
		int flex = opt_table_int(L, idx, "flex", 1);
		int w = opt_table_int(L, idx, "width", 0);
		int h = opt_table_int(L, idx, "height", 0);

		Clay_ElementDeclaration decl = {0};
		if (flex > 0) {
			decl.layout.sizing.width = CLAY_SIZING_GROW(0, 0);
			decl.layout.sizing.height = CLAY_SIZING_GROW(0, 0);
		} else {
			decl.layout.sizing.width = (w > 0) ? CLAY_SIZING_FIXED(w) : CLAY_SIZING_FIT(0, 0);
			decl.layout.sizing.height = (h > 0) ? CLAY_SIZING_FIXED(h) : CLAY_SIZING_FIT(0, 0);
		}
		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "rect") == 0) {
		int w = opt_table_int(L, idx, "width", 0);
		int h = opt_table_int(L, idx, "height", 0);
		uint32_t col = opt_table_color(L, idx, "color", 0xFFFFFFFFu);
		int r = opt_table_int(L, idx, "radius", 0);

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = (w > 0) ? CLAY_SIZING_FIXED(w) : CLAY_SIZING_GROW(0, 0);
		decl.layout.sizing.height = (h > 0) ? CLAY_SIZING_FIXED(h) : CLAY_SIZING_GROW(0, 0);
		decl.backgroundColor = clay_color_from_argb(col);
		if (r > 0)
			decl.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "separator") == 0) {
		int thick = opt_table_int(L, idx, "thickness", 1);
		bool vert = false;
		lua_getfield(L, idx, "vertical");
		if (lua_isboolean(L, -1))
			vert = lua_toboolean(L, -1);
		lua_pop(L, 1);
		uint32_t col = opt_table_color(L, idx, "color", 0x44FFFFFFu);

		Clay_ElementDeclaration decl = {0};
		if (vert) {
			decl.layout.sizing.width = CLAY_SIZING_FIXED(thick);
			decl.layout.sizing.height = CLAY_SIZING_GROW(0, 0);
		} else {
			decl.layout.sizing.width = CLAY_SIZING_GROW(0, 0);
			decl.layout.sizing.height = CLAY_SIZING_FIXED(thick);
		}
		decl.backgroundColor = clay_color_from_argb(col);
		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "progress") == 0) {
		double val = opt_table_number(L, idx, "value", 0.0);
		double max_val = opt_table_number(L, idx, "max", 100.0);
		int h = opt_table_int(L, idx, "height", 8);
		int w = opt_table_int(L, idx, "width", 0);
		int r = opt_table_int(L, idx, "radius", 4);
		uint32_t bar_col = opt_table_color(L, idx, "color", 0xFF89B4FAu);
		uint32_t bg_col = opt_table_color(L, idx, "background", 0xFF313244u);

		Clay_ElementDeclaration outer = {0};
		outer.layout.sizing.width = (w > 0) ? CLAY_SIZING_FIXED(w) : CLAY_SIZING_GROW(0, 0);
		outer.layout.sizing.height = CLAY_SIZING_FIXED(h);
		outer.backgroundColor = clay_color_from_argb(bg_col);
		if (r > 0)
			outer.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

		Clay__OpenElement();
		Clay__ConfigureOpenElement(outer);

		float pct = (max_val > 0.0) ? (float)(val / max_val) : 0.0f;
		if (pct < 0.0f) pct = 0.0f;
		if (pct > 1.0f) pct = 1.0f;

		if (pct > 0.001f) {
			Clay_ElementDeclaration inner = {0};
			inner.layout.sizing.width = CLAY_SIZING_PERCENT(pct);
			inner.layout.sizing.height = CLAY_SIZING_GROW(0, 0);
			inner.backgroundColor = clay_color_from_argb(bar_col);
			if (r > 0)
				inner.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

			Clay__OpenElement();
			Clay__ConfigureOpenElement(inner);
			Clay__CloseElement();
		}

		Clay__CloseElement();
		return;
	}

	if (strcmp(type, "image") == 0) {
		const char *src = opt_table_string(L, idx, "src");
		if (!src)
			src = opt_table_string(L, idx, "path");
		int w = opt_table_int(L, idx, "width", 24);
		int h = opt_table_int(L, idx, "height", 24);
		int r = opt_table_int(L, idx, "radius", 0);

		Clay_ElementDeclaration decl = {0};
		decl.layout.sizing.width = CLAY_SIZING_FIXED(w);
		decl.layout.sizing.height = CLAY_SIZING_FIXED(h);
		decl.image.imageData = (void *)src;
		if (r > 0)
			decl.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

		Clay__OpenElement();
		Clay__ConfigureOpenElement(decl);
		Clay__CloseElement();
		return;
	}

	/* Containers: column, row, container, box, button */
	Clay_ElementDeclaration decl = {0};

	if (strcmp(type, "column") == 0)
		decl.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
	else
		decl.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;

	decl.layout.childGap = (uint16_t)opt_table_int(L, idx, "gap", 0);

	/* Padding */
	lua_getfield(L, idx, "padding");
	if (lua_istable(L, -1)) {
		decl.layout.padding.top = (uint16_t)opt_table_int(L, -1, "top", 0);
		decl.layout.padding.right = (uint16_t)opt_table_int(L, -1, "right", 0);
		decl.layout.padding.bottom = (uint16_t)opt_table_int(L, -1, "bottom", 0);
		decl.layout.padding.left = (uint16_t)opt_table_int(L, -1, "left", 0);
	} else if (lua_isinteger(L, -1)) {
		uint16_t pad = (uint16_t)lua_tointeger(L, -1);
		decl.layout.padding = (Clay_Padding){ pad, pad, pad, pad };
	}
	lua_pop(L, 1);

	/* Alignment */
	lua_getfield(L, idx, "align");
	if (lua_istable(L, -1)) {
		const char *ax = opt_table_string(L, -1, "x");
		if (ax && strcmp(ax, "center") == 0)
			decl.layout.childAlignment.x = CLAY_ALIGN_X_CENTER;
		else if (ax && strcmp(ax, "right") == 0)
			decl.layout.childAlignment.x = CLAY_ALIGN_X_RIGHT;

		const char *ay = opt_table_string(L, -1, "y");
		if (ay && strcmp(ay, "center") == 0)
			decl.layout.childAlignment.y = CLAY_ALIGN_Y_CENTER;
		else if (ay && strcmp(ay, "bottom") == 0)
			decl.layout.childAlignment.y = CLAY_ALIGN_Y_BOTTOM;
	}
	lua_pop(L, 1);

	/* Sizing */
	int fixed_w = opt_table_int(L, idx, "width", 0);
	int fixed_h = opt_table_int(L, idx, "height", 0);
	decl.layout.sizing.width = (fixed_w > 0) ? CLAY_SIZING_FIXED(fixed_w) : CLAY_SIZING_GROW(0, 0);
	decl.layout.sizing.height = (fixed_h > 0) ? CLAY_SIZING_FIXED(fixed_h) : (depth == 0 ? CLAY_SIZING_GROW(0, 0) : CLAY_SIZING_FIT(0, 0));

	lua_getfield(L, idx, "sizing");
	if (lua_istable(L, -1)) {
		const char *sw = opt_table_string(L, -1, "width");
		if (sw && strcmp(sw, "grow") == 0)
			decl.layout.sizing.width = CLAY_SIZING_GROW(0, 0);
		else if (sw && strcmp(sw, "fit") == 0)
			decl.layout.sizing.width = CLAY_SIZING_FIT(0, 0);

		const char *sh = opt_table_string(L, -1, "height");
		if (sh && strcmp(sh, "grow") == 0)
			decl.layout.sizing.height = CLAY_SIZING_GROW(0, 0);
		else if (sh && strcmp(sh, "fit") == 0)
			decl.layout.sizing.height = CLAY_SIZING_FIT(0, 0);
	}
	lua_pop(L, 1);

	/* Background */
	uint32_t bg = opt_table_color(L, idx, "background", 0);
	if (bg == 0)
		bg = opt_table_color(L, idx, "color", 0);
	if (bg != 0)
		decl.backgroundColor = clay_color_from_argb(bg);

	/* Corner radius */
	int r = opt_table_int(L, idx, "corner_radius", 0);
	if (r <= 0)
		r = opt_table_int(L, idx, "radius", 0);
	if (r > 0)
		decl.cornerRadius = (Clay_CornerRadius){ (float)r, (float)r, (float)r, (float)r };

	/* Border */
	lua_getfield(L, idx, "border");
	if (lua_istable(L, -1)) {
		uint32_t b_col = opt_table_color(L, -1, "color", 0xFFFFFFFFu);
		int bw = opt_table_int(L, -1, "width", 1);
		decl.border.color = clay_color_from_argb(b_col);
		decl.border.width = (Clay_BorderWidth){ (uint16_t)bw, (uint16_t)bw, (uint16_t)bw, (uint16_t)bw, 0 };
	}
	lua_pop(L, 1);

	Clay__OpenElement();
	Clay__ConfigureOpenElement(decl);

	/* If button with text, emit text element */
	if (strcmp(type, "button") == 0) {
		const char *btn_text = opt_table_string(L, idx, "text");
		if (btn_text) {
			int b_size = opt_table_int(L, idx, "size", 14);
			uint32_t b_col = opt_table_color(L, idx, "color", 0xFFFFFFFFu);
			Clay_TextElementConfig textConfig = {
				.textColor = clay_color_from_argb(b_col),
				.fontSize = (uint16_t)b_size,
				.wrapMode = CLAY_TEXT_WRAP_NONE
			};
			Clay_String str = {
				.length = (int32_t)strlen(btn_text),
				.chars = btn_text
			};
			Clay__OpenTextElement(str, textConfig);
		}
	}

	/* Process children */
	lua_getfield(L, idx, "children");
	if (lua_istable(L, -1)) {
		int ch_len = (int)lua_rawlen(L, -1);
		for (int i = 1; i <= ch_len; i++) {
			lua_rawgeti(L, -1, i);
			clay_build_from_lua(L, -1, depth + 1);
			lua_pop(L, 1);
		}
	} else {
		/* Also check numeric array elements on the container itself */
		int ch_len = (int)lua_rawlen(L, idx);
		for (int i = 1; i <= ch_len; i++) {
			lua_rawgeti(L, idx, i);
			clay_build_from_lua(L, -1, depth + 1);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	Clay__CloseElement();
}

void lua_widget_draw(struct widget *wgt)
{
	if (!wgt || !wgt->pixels)
		return;

	struct rect dmg = {0};
	lua_cur = wgt;
	lua_dmg = &dmg;

	if (wgt->lua_build != LUA_NOREF && wgt->lua_L) {
		int S = wgt->scale > 0 ? wgt->scale : 1;
		float lw = (float)(wgt->width / S);
		float lh = (float)(wgt->height / S);
		clay_layout_begin(lw, lh);

		lua_State *L = wgt->lua_L;
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_build);
		if (lua_isfunction(L, -1)) {
			if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
				clay_build_from_lua(L, lua_gettop(L), 0);
			} else {
				lua_report(L, wgt, "build");
			}
		}
		lua_settop(L, top);

		Clay_RenderCommandArray cmds = clay_layout_end();
		clay_render_commands(wgt, &cmds, &dmg);
	} else if (wgt->lua_draw != LUA_NOREF && wgt->lua_L) {
		fill_rect(wgt->pixels, wgt->width, wgt->height, &dmg, 0, 0,
			  (int)wgt->width, (int)wgt->height, wgt->paint);
		lua_script_call(wgt, &wgt->lua_draw, "draw", NULL);
	}

	lua_cur = NULL;
	lua_dmg = NULL;
	if (wgt->surface)
		scene_commit(wgt, &dmg);
}

static void open_lib(lua_State *L, const char *name, lua_CFunction f)
{
	luaL_requiref(L, name, f, 1);
	lua_pop(L, 1);
}

static const char *wisp_prelude_lua =
	"wisp.layout = {\n"
	"  column = function(opts) opts = opts or {}; opts._type = 'column'; return opts end,\n"
	"  row = function(opts) opts = opts or {}; opts._type = 'row'; return opts end,\n"
	"  container = function(opts) opts = opts or {}; opts._type = 'container'; return opts end,\n"
	"  spacer = function(opts) opts = opts or {}; opts._type = 'spacer'; return opts end,\n"
	"  floating = function(opts) opts = opts or {}; opts._type = 'floating'; return opts end,\n"
	"}\n"
	"wisp.ui = {\n"
	"  text = function(text, opts)\n"
	"    if type(text) == 'table' then\n"
	"      opts = text\n"
	"    else\n"
	"      opts = opts or {}\n"
	"      opts.text = text\n"
	"    end\n"
	"    opts._type = 'text'\n"
	"    return opts\n"
	"  end,\n"
	"  rect = function(opts) opts = opts or {}; opts._type = 'rect'; return opts end,\n"
	"  image = function(opts) opts = opts or {}; opts._type = 'image'; return opts end,\n"
	"  progress = function(opts) opts = opts or {}; opts._type = 'progress'; return opts end,\n"
	"  separator = function(opts) opts = opts or {}; opts._type = 'separator'; return opts end,\n"
	"  button = function(opts) opts = opts or {}; opts._type = 'button'; return opts end,\n"
	"}\n";

static const char *wisp_prelude_mui_lua =
	"wisp.mui = {\n"
	"  colors = {\n"
	"    primary = '#90CAF9',\n"
	"    primary_main = '#1976D2',\n"
	"    secondary = '#CE93D8',\n"
	"    background = '#121212EE',\n"
	"    paper = '#1E1E1EEE',\n"
	"    surface = '#242424',\n"
	"    surface_hover = '#2E2E2E',\n"
	"    border = '#383838',\n"
	"    divider = '#2D2D2D',\n"
	"    text_primary = '#FFFFFF',\n"
	"    text_secondary = '#B0BEC5',\n"
	"    text_disabled = '#78909C',\n"
	"    success = '#66BB6A',\n"
	"    warning = '#FFA726',\n"
	"    error = '#EF5350',\n"
	"    info = '#29B6F6',\n"
	"  },\n"
	"  card = function(opts)\n"
	"    opts = opts or {}\n"
	"    return wisp.layout.container({\n"
	"      background = opts.background or '#1E1E1EEE',\n"
	"      border = opts.border or { color = opts.border_color or '#383838', width = opts.border_width or 1 },\n"
	"      corner_radius = opts.radius or opts.corner_radius or 12,\n"
	"      padding = opts.padding or 14,\n"
	"      sizing = opts.sizing or { width = 'grow', height = 'grow' },\n"
	"      children = opts.children or {}\n"
	"    })\n"
	"  end,\n"
	"  chip = function(text, opts)\n"
	"    opts = opts or {}\n"
	"    return wisp.layout.container({\n"
	"      background = opts.background or '#2D2D2D',\n"
	"      border = opts.border or { color = opts.border_color or '#424242', width = 1 },\n"
	"      corner_radius = opts.radius or 12,\n"
	"      padding = opts.padding or { top = 3, bottom = 3, left = 8, right = 8 },\n"
	"      align = { x = 'center', y = 'center' },\n"
	"      children = {\n"
	"        wisp.ui.text(text, { size = opts.size or 11, color = opts.color or '#90CAF9' })\n"
	"      }\n"
	"    })\n"
	"  end,\n"
	"  button = function(opts)\n"
	"    opts = opts or {}\n"
	"    local variant = opts.variant or 'contained'\n"
	"    local bg = (variant == 'contained') and (opts.background or '#1976D2') or (opts.background or '#00000000')\n"
	"    local col = (variant == 'contained') and (opts.color or '#FFFFFF') or (opts.color or '#90CAF9')\n"
	"    local bdr = (variant == 'outlined') and (opts.border or { color = opts.border_color or '#90CAF9', width = 1 }) or nil\n"
	"    return wisp.layout.container({\n"
	"      background = bg,\n"
	"      border = bdr,\n"
	"      corner_radius = opts.radius or 8,\n"
	"      padding = opts.padding or { top = 6, bottom = 6, left = 14, right = 14 },\n"
	"      align = { x = 'center', y = 'center' },\n"
	"      children = {\n"
	"        wisp.ui.text(opts.text or 'Button', { size = opts.size or 12, color = col })\n"
	"      }\n"
	"    })\n"
	"  end,\n"
	"  switch = function(active, opts)\n"
	"    opts = opts or {}\n"
	"    local bg = active and (opts.active_color or '#1976D2') or (opts.inactive_color or '#424242')\n"
	"    local thumb_col = opts.thumb_color or '#FFFFFF'\n"
	"    local thumb = wisp.ui.rect({ width = 14, height = 14, radius = 7, color = thumb_col })\n"
	"    local row_ch = {}\n"
	"    if active then\n"
	"      table.insert(row_ch, wisp.layout.spacer({ flex = 1 }))\n"
	"      table.insert(row_ch, thumb)\n"
	"    else\n"
	"      table.insert(row_ch, thumb)\n"
	"      table.insert(row_ch, wisp.layout.spacer({ flex = 1 }))\n"
	"    end\n"
	"    return wisp.layout.container({\n"
	"      width = 38,\n"
	"      height = 20,\n"
	"      background = bg,\n"
	"      corner_radius = 10,\n"
	"      padding = 3,\n"
	"      children = {\n"
	"        wisp.layout.row({\n"
	"          sizing = { width = 'grow', height = 'grow' },\n"
	"          align = { y = 'center' },\n"
	"          children = row_ch\n"
	"        })\n"
	"      }\n"
	"    })\n"
	"  end,\n"
	"  linear_progress = function(val, max_val, opts)\n"
	"    opts = opts or {}\n"
	"    return wisp.ui.progress({\n"
	"      value = val,\n"
	"      max = max_val or 100,\n"
	"      height = opts.height or 6,\n"
	"      radius = opts.radius or 3,\n"
	"      color = opts.color or '#90CAF9',\n"
	"      background = opts.background or '#333333'\n"
	"    })\n"
	"  end\n"
	"}\n";

void lua_vm_init(size_t mem_bytes)
{
	if (Lm)
		return;

	if (mem_bytes > 0) {
		lua_limit = mem_bytes;
	} else {
		const char *env_mem = getenv("WISP_LUA_MEM_LIMIT");
		if (env_mem) {
			char *end = NULL;
			long val = strtol(env_mem, &end, 0);
			if (val > 0) {
				if (end && (*end == 'k' || *end == 'K'))
					val <<= 10;
				else if (end && (*end == 'm' || *end == 'M'))
					val <<= 20;
				lua_limit = (size_t)val;
			}
		} else {
			lua_limit = LUA_MEM_DEFAULT;
		}
	}

	const char *env_insn = getenv("WISP_LUA_INSN_LIMIT");
	if (env_insn) {
		long val = strtol(env_insn, NULL, 0);
		if (val > 0)
			lua_ticks_total = val;
	}

	Lm = lua_newstate(lua_alloc, NULL);
	if (!Lm)
		return;

	open_lib(Lm, "_G", luaopen_base);
	open_lib(Lm, "table", luaopen_table);
	open_lib(Lm, "string", luaopen_string);
	open_lib(Lm, "math", luaopen_math);
	open_lib(Lm, "io", luaopen_io);
	open_lib(Lm, "os", luaopen_os);

	/* Security sandbox: remove dangerous OS/IO functions.
	 * io.open and os.date/time/clock are intentionally retained. */
	lua_getglobal(Lm, "os");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "execute");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "exit");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "remove");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "rename");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "tmpname");
	lua_pop(Lm, 1);

	lua_getglobal(Lm, "io");
	lua_pushnil(Lm);
	lua_setfield(Lm, -2, "popen");
	lua_pop(Lm, 1);

	/* Master wisp table */
	lua_newtable(Lm);
	lua_pushcfunction(Lm, lua_wisp_setup);
	lua_setfield(Lm, -2, "setup");

	/* Sub-namespaces */
	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_widget_api, 0);
	lua_setfield(Lm, -2, "widget");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_timer_api, 0);
	lua_setfield(Lm, -2, "timer");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_system_api, 0);
	lua_setfield(Lm, -2, "system");

	lua_newtable(Lm);
	luaL_setfuncs(Lm, lua_style_api, 0);

	/* Predefined color palette */
	lua_createtable(Lm, 0, 10);
	lua_pushinteger(Lm, 0xEE181825u); lua_setfield(Lm, -2, "background");
	lua_pushinteger(Lm, 0xDD313244u); lua_setfield(Lm, -2, "surface");
	lua_pushinteger(Lm, 0xFFCDD6F4u); lua_setfield(Lm, -2, "text");
	lua_pushinteger(Lm, 0xFFA6ADC8u); lua_setfield(Lm, -2, "subtext");
	lua_pushinteger(Lm, 0xFF89B4FAu); lua_setfield(Lm, -2, "blue");
	lua_pushinteger(Lm, 0xFFA6E3A1u); lua_setfield(Lm, -2, "green");
	lua_pushinteger(Lm, 0xFFF38BA8u); lua_setfield(Lm, -2, "red");
	lua_pushinteger(Lm, 0xFFF9E2AFu); lua_setfield(Lm, -2, "yellow");
	lua_pushinteger(Lm, 0xFFCBA6F7u); lua_setfield(Lm, -2, "mauve");
	lua_setfield(Lm, -2, "colors");

	lua_setfield(Lm, -2, "style");

	/* Expose wisp to global table */
	lua_setglobal(Lm, "wisp");

	/* Run prelude to populate wisp.layout and wisp.ui helper constructors */
	if (luaL_dostring(Lm, wisp_prelude_lua) != LUA_OK) {
		fprintf(stderr, "wisp: failed to load prelude: %s\n", lua_tostring(Lm, -1));
		lua_pop(Lm, 1);
	}
	if (luaL_dostring(Lm, wisp_prelude_mui_lua) != LUA_OK) {
		fprintf(stderr, "wisp: failed to load mui prelude: %s\n", lua_tostring(Lm, -1));
		lua_pop(Lm, 1);
	}

	lua_newtable(Lm);
	lua_setglobal(Lm, "state");
}

/*
 * Set up an isolated script environment and execute the script source.
 *
 * Isolation model:
 *   - A fresh table `env` becomes the script's _G.
 *   - All keys from the real _G are shallow-copied into env (standard libs available).
 *   - The `wisp` global and `_G = env` are set explicitly.
 *   - On reload, the `state` table is copied from the previous env so scripts can
 *     persist data across hot-reloads.
 *   - The script is loaded with env as its upvalue-1 (environment).
 *   - Any table returned by the script is merged into env (e.g. { width=, build= }).
 *
 * Returns the stack index of env on success, or -1 on error (error already reported).
 */
static int lua_env_setup(struct widget *wgt, lua_State *L, const char *src,
			 size_t len, const char *chunkid)
{
	lua_createtable(L, 0, 32);
	int env = lua_gettop(L);

	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
	int g = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, g) != 0) {
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_rawset(L, env);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	lua_getglobal(L, "wisp");
	lua_setfield(L, env, "wisp");
	lua_pushvalue(L, env);
	lua_setfield(L, env, "_G");

	if (wgt && wgt->lua_L && wgt->lua_env != LUA_NOREF) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, wgt->lua_env);
		lua_getfield(L, -1, "state");
		if (lua_istable(L, -1))
			lua_setfield(L, env, "state");
		else
			lua_pop(L, 1);
		lua_pop(L, 1);
	} else {
		lua_createtable(L, 0, 0);
		lua_setfield(L, env, "state");
	}

	if (luaL_loadbufferx(L, src, len, chunkid, "t") != LUA_OK ||
	    !lua_isfunction(L, -1)) {
		lua_report(L, wgt, "load");
		return -1;
	}
	lua_pushvalue(L, env);
	if (!lua_setupvalue(L, -2, 1)) {
		lua_report(L, wgt, "env");
		return -1;
	}

	struct widget *prev_cur = lua_cur;
	lua_cur = wgt;
	int status = lua_pcall(L, 0, 1, 0);
	lua_cur = prev_cur;

	if (status != LUA_OK) {
		lua_report(L, wgt, "config");
		return -1;
	}

	if (lua_istable(L, -1)) {
		int cfg = lua_gettop(L);
		lua_pushnil(L);
		while (lua_next(L, cfg) != 0) {
			lua_pushvalue(L, -2);
			lua_pushvalue(L, -2);
			lua_rawset(L, env);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	return env;
}

static bool lua_widget_config(struct widget *wgt, lua_State *L, int env)
{
	int n;
	lua_getfield(L, env, "width");
	n = (int)lua_tointeger(L, -1);
	if (n > 0)
		wgt->pend_w = wgt->width = (uint32_t)(n < 8 ? 8 : (n > 4096 ? 4096 : n));
	lua_pop(L, 1);

	lua_getfield(L, env, "height");
	n = (int)lua_tointeger(L, -1);
	if (n > 0)
		wgt->pend_h = wgt->height = (uint32_t)(n < 8 ? 8 : (n > 4096 ? 4096 : n));
	lua_pop(L, 1);

	lua_getfield(L, env, "anchor");
	const char *s = lua_tostring(L, -1);
	if (s)
		wgt->anchor = parse_anchor(s);
	lua_pop(L, 1);

	lua_getfield(L, env, "margin");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "top");
		if (!lua_isnil(L, -1)) wgt->margin[0] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "right");
		if (!lua_isnil(L, -1)) wgt->margin[1] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "bottom");
		if (!lua_isnil(L, -1)) wgt->margin[2] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, -1, "left");
		if (!lua_isnil(L, -1)) wgt->margin[3] = (int32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	} else if (lua_isinteger(L, -1)) {
		int32_t m = (int32_t)lua_tointeger(L, -1);
		for (int i = 0; i < 4; i++) wgt->margin[i] = m;
	}
	lua_pop(L, 1);

	lua_getfield(L, env, "visible");
	if (!lua_isnil(L, -1))
		wgt->visible = lua_toboolean(L, -1);
	lua_pop(L, 1);

	/* Check for build callback */
	lua_getfield(L, env, "build");
	if (lua_isfunction(L, -1)) {
		if (wgt->lua_build != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, wgt->lua_build);
		wgt->lua_build = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_pop(L, 1);
	}

	const char *evt_names[] = {
		"on_click", "click",
		"on_scroll", "scroll",
		"on_hover", "hover",
		"on_leave", "leave",
		"on_press", "press",
		"on_release", "release"
	};
	int *evt_slots[] = {
		&wgt->lua_click, &wgt->lua_click,
		&wgt->lua_scroll, &wgt->lua_scroll,
		&wgt->lua_hover, &wgt->lua_hover,
		&wgt->lua_leave, &wgt->lua_leave,
		&wgt->lua_press, &wgt->lua_press,
		&wgt->lua_release, &wgt->lua_release
	};
	for (size_t k = 0; k < sizeof(evt_names)/sizeof(evt_names[0]); k++) {
		lua_getfield(L, env, evt_names[k]);
		if (lua_isfunction(L, -1)) {
			if (*evt_slots[k] != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, *evt_slots[k]);
			*evt_slots[k] = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			lua_pop(L, 1);
		}
	}

	if (kid_interactive(wgt) && wgt->surface)
		widget_input_region(wgt);

	const char *cb_names[3] = {"on_draw", "on_timer", "on_event"};
	int *cb_refs[3] = {&wgt->lua_draw, &wgt->lua_timer, &wgt->lua_event};
	for (int k = 0; k < 3; k++) {
		lua_getfield(L, env, cb_names[k]);
		if (lua_isfunction(L, -1))
			*cb_refs[k] = luaL_ref(L, LUA_REGISTRYINDEX);
		else
			lua_pop(L, 1);
	}

	lua_getfield(L, env, "on_timer_ms");
	if (lua_isfunction(L, -1)) {
		if (lua_pcall(L, 0, 1, 0) == LUA_OK)
			n = (int)lua_tointeger(L, -1);
		else
			n = 1000;
	} else {
		n = (int)lua_tointeger(L, -1);
	}
	wgt->lua_timer_ms = n < 10 ? 1000 : (n > 600000 ? 600000 : n);
	lua_pop(L, 1);

	if (wgt->layer_surface) {
		zwlr_layer_surface_v1_set_size(wgt->layer_surface, wgt->width, wgt->height);
		zwlr_layer_surface_v1_set_anchor(wgt->layer_surface, wgt->anchor);
		zwlr_layer_surface_v1_set_margin(wgt->layer_surface, wgt->margin[0],
						 wgt->margin[1], wgt->margin[2], wgt->margin[3]);
	}
	return true;
}

void lua_widget_free(struct widget *wgt)
{
	if (Lm) {
		timers_cleanup_for_widget(wgt);
		int *refs[] = {&wgt->lua_env, &wgt->lua_build, &wgt->lua_draw, &wgt->lua_timer,
			       &wgt->lua_event, &wgt->lua_thread_ref, &wgt->lua_click,
			       &wgt->lua_hover, &wgt->lua_leave, &wgt->lua_press,
			       &wgt->lua_release, &wgt->lua_scroll};
		for (size_t k = 0; k < sizeof(refs)/sizeof(refs[0]); k++) {
			if (*refs[k] != LUA_NOREF) {
				luaL_unref(Lm, LUA_REGISTRYINDEX, *refs[k]);
				*refs[k] = LUA_NOREF;
			}
		}
		lua_gc(Lm, LUA_GCCOLLECT, 0);
	}
	wgt->lua_L = NULL;
}

bool lua_widget_from_source(struct widget *wgt, const char *src,
			    size_t len, const char *chunkid)
{
	if (wgt->lua_L)
		return true;
	lua_vm_init(0);
	if (!Lm)
		return false;

	wgt->lua_env = wgt->lua_build = wgt->lua_draw = wgt->lua_timer = wgt->lua_event = LUA_NOREF;
	wgt->lua_click = wgt->lua_hover = wgt->lua_leave = LUA_NOREF;
	wgt->lua_press = wgt->lua_release = wgt->lua_scroll = LUA_NOREF;

	lua_State *L = lua_newthread(Lm);
	if (!L)
		return false;
	wgt->lua_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
	wgt->lua_L = L;
	lua_ctx = wgt;

	int env = lua_env_setup(wgt, L, src, len, chunkid);
	if (env < 0) {
		lua_widget_free(wgt);
		return false;
	}
	lua_widget_config(wgt, L, env);
	wgt->lua_env = luaL_ref(L, LUA_REGISTRYINDEX);
	wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;
	lua_event(wgt, "loaded");
	lua_ctx = wgt;
	return true;
}

bool lua_widget_reload_from_source(struct widget *wgt, const char *src,
				   size_t len, const char *chunkid)
{
	if (!wgt || !wgt->used || !Lm)
		return false;

	lua_State *new_L = lua_newthread(Lm);
	if (!new_L)
		return false;
	int new_thread_ref = luaL_ref(Lm, LUA_REGISTRYINDEX);
	lua_ctx = wgt;

	int new_env = lua_env_setup(wgt, new_L, src, len, chunkid);
	if (new_env < 0) {
		luaL_unref(Lm, LUA_REGISTRYINDEX, new_thread_ref);
		lua_gc(Lm, LUA_GCCOLLECT, 0);
		fprintf(stderr, "wisp: widget '%s' reload failed, rolled back to previous version\n",
			wgt->name);
		return false;
	}

	uint32_t old_w = wgt->width, old_h = wgt->height;
	timers_cleanup_for_widget(wgt);
	lua_widget_free(wgt);

	wgt->lua_L = new_L;
	wgt->lua_thread_ref = new_thread_ref;
	lua_widget_config(wgt, new_L, new_env);
	wgt->lua_env = luaL_ref(new_L, LUA_REGISTRYINDEX);
	wgt->lua_next = now_us() + (int64_t)wgt->lua_timer_ms * 1000;

	if (wgt->width != old_w || wgt->height != old_h)
		wgt->want_resize = 1;
	wgt->want_redraw = 1;
	lua_event(wgt, "reloaded");
	lua_ctx = wgt;
	fprintf(stderr, "wisp: widget '%s' reloaded successfully\n", wgt->name);
	return true;
}

bool lua_widget_reload(struct widget *wgt)
{
	if (!wgt || !wgt->used || !wgt->script_path[0])
		return false;
	FILE *f = fopen(wgt->script_path, "r");
	if (!f) {
		fprintf(stderr, "wisp: reload failed: cannot open %s\n", wgt->script_path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = (fsize > 0 && fsize <= LUA_CHUNK_MAX) ? malloc((size_t)fsize) : NULL;
	if (src && fread(src, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(src);
		src = NULL;
	}
	fclose(f);
	if (!src) {
		fprintf(stderr, "wisp: reload failed: cannot read %s\n", wgt->script_path);
		return false;
	}
	bool ok = lua_widget_reload_from_source(wgt, src, (size_t)fsize, wgt->script_path);
	free(src);
	return ok;
}

bool lua_widget_load(struct widget *wgt, const char *path)
{
	if (wgt->lua_L)
		return true;
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "wisp: can't open script %s — widget skipped\n", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = (fsize > 0 && fsize <= LUA_CHUNK_MAX) ? malloc((size_t)fsize) : NULL;
	if (src && fread(src, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(src);
		src = NULL;
	}
	fclose(f);
	if (!src) {
		fprintf(stderr, "wisp: can't read script %s — widget skipped\n", path);
		return false;
	}
	const char *base = strrchr(path, '/');
	snprintf(wgt->name, sizeof(wgt->name), "%s", base ? base + 1 : path);
	snprintf(wgt->script_path, sizeof(wgt->script_path), "%s", path);
	bool ok = lua_widget_from_source(wgt, src, (size_t)fsize, path);
	free(src);
	if (!ok)
		return false;

	if (inotify_fd >= 0) {
		if (wgt->watch_wd >= 0) {
			int old_wd = wgt->watch_wd;
			wgt->watch_wd = -1;
			bool other = false;
			for (int k = 0; k < MAX_WIDGETS; k++) {
				if (&widgets[k] != wgt && widgets[k].used && widgets[k].watch_wd == old_wd) {
					other = true;
					break;
				}
			}
			if (!other)
				inotify_rm_watch(inotify_fd, old_wd);
		}
		char dir[128];
		const char *fname = NULL;
		get_dir_and_file(path, dir, sizeof(dir), &fname);
		wgt->watch_wd = inotify_add_watch(inotify_fd, dir,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM);
	}
	wgt->paint = COLOR_IDLE;
	if (!wgt->visible)
		widget_hide(wgt);
	return true;
}

void shutdown_lua(void)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timers[i].active && timers[i].lua_cb != LUA_NOREF && Lm)
			luaL_unref(Lm, LUA_REGISTRYINDEX, timers[i].lua_cb);
		timers[i].active = false;
		timers[i].lua_cb = LUA_NOREF;
	}
	if (Lm) {
		fprintf(stderr, "wisp: lua final memory = %zu bytes\n", lua_used);
		lua_close(Lm);
	}
	Lm = NULL;
	lua_used = 0;
}

int lua_timers_ms(int64_t now)
{
	int ms = -1;

	/* 1. Multi-timers */
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!timers[i].active || timers[i].lua_cb == LUA_NOREF)
			continue;
		if (timers[i].owner && (!timers[i].owner->used || !timers[i].owner->visible || timers[i].owner->want_destroy))
			continue;
		int64_t due = (timers[i].next_us - now + 999) / 1000;
		if (due < 0)
			due = 0;
		if (ms < 0 || due < ms)
			ms = (int)due;
	}

	/* 2. Single-timer fallback */
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || !wgt->lua_L || !wgt->visible ||
		    wgt->want_destroy || wgt->lua_timer == LUA_NOREF)
			continue;
		int64_t due = (wgt->lua_next - now + 999) / 1000;
		if (due < 0)
			due = 0;
		if (ms < 0 || due < ms)
			ms = (int)due;
	}
	return ms;
}

void lua_run_due(int64_t now)
{
	/* 1. Multi-timers */
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!timers[i].active || timers[i].lua_cb == LUA_NOREF || now < timers[i].next_us)
			continue;
		struct widget *wgt = timers[i].owner;
		if (wgt && (!wgt->used || !wgt->visible || wgt->want_destroy))
			continue;

		int cb_ref = timers[i].lua_cb;
		if (timers[i].repeat) {
			timers[i].next_us = now + (int64_t)timers[i].interval_ms * 1000;
		} else {
			timers[i].lua_cb = LUA_NOREF;
			timers[i].active = false;
			timers[i].owner = NULL;
		}

		if (wgt && wgt->lua_L) {
			lua_script_call(wgt, &cb_ref, "timer", NULL);
			wgt->want_redraw = 1;
		} else if (Lm) {
			lua_rawgeti(Lm, LUA_REGISTRYINDEX, cb_ref);
			lua_pcall(Lm, 0, 0, 0);
		}

		if (!timers[i].repeat && cb_ref != LUA_NOREF && Lm)
			luaL_unref(Lm, LUA_REGISTRYINDEX, cb_ref);
	}

	/* 2. Single-timer fallback */
	for (int k = 0; k < MAX_WIDGETS; k++) {
		struct widget *wgt = &widgets[k];
		if (!wgt->used || !wgt->lua_L || !wgt->visible ||
		    wgt->want_destroy || wgt->lua_timer == LUA_NOREF || now < wgt->lua_next)
			continue;
		wgt->lua_next = now + (int64_t)wgt->lua_timer_ms * 1000;
		lua_script_call(wgt, &wgt->lua_timer, "timer", NULL);
		wgt->want_redraw = 1;
	}
}

void lua_config_staging_free(struct config_staging *stage)
{
	if (!stage || !Lm)
		return;
	for (int i = 0; i < stage->widget_count; i++) {
		if (stage->widgets[i].build_ref != LUA_NOREF) {
			luaL_unref(Lm, LUA_REGISTRYINDEX, stage->widgets[i].build_ref);
			stage->widgets[i].build_ref = LUA_NOREF;
		}
	}
	stage->widget_count = 0;
}

bool lua_config_execute_source(const char *src, size_t len, const char *chunkid, struct config_staging *stage)
{
	lua_vm_init(0);
	if (!Lm || !stage)
		return false;

	struct config_staging *prev_stage = cur_stage;
	cur_stage = stage;

	lua_sethook(Lm, lua_tick_hook, LUA_MASKCOUNT, 1000);
	lua_ticks = lua_ticks_total;

	int top = lua_gettop(Lm);
	int status = luaL_loadbufferx(Lm, src, len, chunkid ? chunkid : "config", "t");
	if (status != LUA_OK) {
		const char *err = lua_tostring(Lm, -1);
		fprintf(stderr, "wisp: config error in '%s': %s\n", chunkid ? chunkid : "config", err ? err : "load error");
		lua_settop(Lm, top);
		lua_sethook(Lm, NULL, 0, 0);
		cur_stage = prev_stage;
		return false;
	}

	status = lua_pcall(Lm, 0, 0, 0);
	lua_sethook(Lm, NULL, 0, 0);
	if (status != LUA_OK) {
		const char *err = lua_tostring(Lm, -1);
		fprintf(stderr, "wisp: config runtime error in '%s': %s\n", chunkid ? chunkid : "config", err ? err : "runtime error");
		lua_settop(Lm, top);
		cur_stage = prev_stage;
		return false;
	}

	lua_settop(Lm, top);
	cur_stage = prev_stage;
	return true;
}

bool lua_config_execute(const char *path, struct config_staging *stage)
{
	if (!path || !path[0])
		return false;
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "wisp: cannot open config file '%s': %s\n", path, strerror(errno));
		return false;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || sz > LUA_CHUNK_MAX) {
		fprintf(stderr, "wisp: config '%s' exceeds size limit (%ld bytes)\n", path, sz);
		fclose(f);
		return false;
	}
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return false;
	}
	size_t nr = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[nr] = '\0';

	bool ok = lua_config_execute_source(buf, nr, path, stage);
	free(buf);
	return ok;
}

#else

struct widget *lua_cur = NULL;

#endif
