/* wisp.widget - Lua-facing widget module. Owns the _make_widget object
 * wrapper moved verbatim from the scripting.c prelude. The C-side bindings
 * (create/register/redraw/...) stay in scripting.c (they are part of the
 * wisp.widget C API); this file owns the Lua object API (build/draw/redraw/
 * invalidate/close/size/run/show/hide). */
#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>

#include "widget/lua_widget.h"

const char *wisp_widget_lua =
	"local _orig_widget_create = wisp.widget.create\n"
	"local function _make_widget(name, opts)\n"
	"  if type(name) == 'table' and opts == nil then\n"
	"    opts = name\n"
	"    name = opts.name or opts.id or 'widget'\n"
	"  end\n"
	"  opts = opts or {}\n"
	"  opts.name = name\n"
	"  local raw = _orig_widget_create(opts)\n"
	"  local wgt = raw or {}\n"
	"  wgt._is_widget = true\n"
	"  wgt.id = raw and raw.id or 0\n"
	"  wgt.name = name\n"
	"  wgt._gen = (wisp.widget._gen and wisp.widget._gen(wgt.id)) or 0\n"
	"  if wisp.object and wisp.object.apply_to then wisp.object.apply_to(wgt) end\n"
	"  local function _dead(self)\n"
	"    local s = self or wgt\n"
	"    local id = s.id\n"
	"    if type(id) ~= 'number' then return false end\n"
	"    if not wisp.widget._gen then return false end\n"
	"    local g = s._gen\n"
	"    if g == nil then return false end\n"
	"    return wisp.widget._gen(id) ~= g\n"
	"  end\n"
	"  function wgt:build(fn)\n"
	"    if type(self) == 'function' and fn == nil then fn = self; self = wgt end\n"
	"    if type(fn) ~= 'function' then return self end\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.set_build(target_id, function()\n"
	"      local prev_app = wisp._current_rendering_app\n"
	"      wisp._current_rendering_app = self or wgt\n"
	"      wisp._ui_build.stack = {}\n"
	"      wisp._ui_build.root = nil\n"
	"      local res = fn(wisp.ui)\n"
	"      wisp._current_rendering_app = prev_app\n"
	"      if res ~= nil then\n"
	"        if type(res) == 'table' and type(res.to_clay) == 'function' then\n"
	"          res = res:to_clay()\n"
	"        end\n"
	"        return res\n"
	"      end\n"
	"      local root = wisp._ui_build.root\n"
	"      if type(root) == 'table' and type(root.to_clay) == 'function' then\n"
	"        root = root:to_clay()\n"
	"      end\n"
	"      return root\n"
	"    end)\n"
	"    return self\n"
	"  end\n"
	"  wgt.render = wgt.build\n"
	"  function wgt:invalidate(kind)\n"
	"    if _dead(self) then return self end\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    if wisp.widget.invalidate then\n"
	"      wisp.widget.invalidate(target_id, kind or 'layout')\n"
	"    else\n"
	"      wisp.widget.redraw(target_id)\n"
	"    end\n"
	"    return self\n"
	"  end\n"
	"  if wgt.connect_signal then\n"
	"    wgt:connect_signal('widget::layout_changed', function()\n"
	"      wgt:invalidate('layout')\n"
	"    end)\n"
	"    wgt:connect_signal('widget::redraw_needed', function()\n"
	"      wgt:invalidate('paint')\n"
	"    end)\n"
	"  end\n"
	"  function wgt:draw(fn)\n"
	"    if type(self) == 'function' and fn == nil then fn = self; self = wgt end\n"
	"    if type(fn) ~= 'function' then return self end\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.set_draw(target_id, fn)\n"
	"    return self\n"
	"  end\n"
	"  function wgt:redraw()\n"
	"    if _dead(self) then return self end\n"
	"    return self:invalidate('paint')\n"
	"  end\n"
	"  function wgt:close()\n"
	"    if _dead(self) then return end\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.destroy(target_id)\n"
	"  end\n"
	"  wgt.destroy = wgt.close\n"
	"  function wgt:size(w, h)\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    if w and h then\n"
	"      wisp.widget.size(target_id, w, h)\n"
	"      return self\n"
	"    else\n"
	"      return wisp.widget.size(target_id)\n"
	"    end\n"
	"  end\n"
	"  function wgt:get_width() local target_id = (self and self.id) or wgt.id; return wisp.widget.size(target_id) end\n"
	"  function wgt:get_height() local target_id = (self and self.id) or wgt.id; local _, h = wisp.widget.size(target_id); return h end\n"
	"  function wgt:run()\n"
	"    self:redraw()\n"
	"    return self\n"
	"  end\n"
	"  function wgt:show()\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.show(target_id)\n"
	"    return self\n"
	"  end\n"
	"  function wgt:hide()\n"
	"    local target_id = (self and self.id) or wgt.id\n"
	"    wisp.widget.hide(target_id)\n"
	"    return self\n"
	"  end\n"
	"  return wgt\n"
	"end\n"
	"wisp.widget.create = function(opts) return _make_widget(opts and (opts.name or opts.id), opts) end\n"
	"setmetatable(wisp.widget, {\n"
	"  __call = function(_, name, opts)\n"
	"    if type(name) == 'table' and opts == nil then\n"
	"      if name.layout ~= nil or (name.widget ~= nil and type(name.widget) ~= 'string') then\n"
	"        if wisp.widget.base and wisp.widget.base.make_widget_declarative then\n"
	"          return wisp.widget.base.make_widget_declarative(name)\n"
	"        end\n"
	"      end\n"
	"    end\n"
	"    return _make_widget(name, opts)\n"
	"  end\n"
	"})\n";

static int preload_upvalue(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1)); /* upvalue: the module table */
	return 1;
}

void widget_register_lua(lua_State *L)
{
	if (luaL_dostring(L, wisp_widget_lua) != LUA_OK) {
		fprintf(stderr, "wisp: failed to load wisp.widget: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		return;
	}
	/* require("wisp.widget") returns the same table */
	int top = lua_gettop(L);
	lua_getglobal(L, "package");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "preload");
		if (lua_istable(L, -1)) {
			lua_getglobal(L, "wisp");
			if (lua_istable(L, -1)) {
				lua_getfield(L, -1, "widget");
				if (lua_istable(L, -1)) {
					lua_pushcclosure(L, preload_upvalue, 1);
					lua_setfield(L, -3, "wisp.widget");
				} else {
					lua_pop(L, 1);
				}
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_settop(L, top);
}
