#pragma once
#include <lua.h>

/* Register the wisp.widget Lua wrapper: runs the _make_widget chunk and
 * installs package.preload["wisp.widget"]. Call after the wisp global exists. */
void widget_register_lua(lua_State *L);
