#pragma once
#include <lua.h>

/* Register the wisp.ui module: runs the UI DSL chunk and installs
 * package.preload["wisp.ui"]. Call after the wisp global exists. */
void ui_register_lua(lua_State *L);
