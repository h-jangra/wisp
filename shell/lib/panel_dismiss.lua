-- Fullscreen transparent click catcher: closes all open panels on click.

local function _shell_root()
  local home = os.getenv("HOME") or ""
  local cands = { "shell", "./shell", home .. "/.config/wisp/shell", "/home/hj/Projects/wisp/shell", "/usr/share/wisp/shell" }
  for _, d in ipairs(cands) do
    local f = io.open(d .. "/style.lua", "r")
    if f then f:close(); return d end
  end
  return "shell"
end
local _sr = _shell_root()
package.path = _sr .. "/?.lua;" .. _sr .. "/?/init.lua;" .. package.path

local panels = require("lib.panels")

local widget = wisp.widget("panel_dismiss", {
  anchor = "top_bottom_left_right",
  transparent = true,
  visible = false,
  click_through = false,
  on_click = function() panels.close_all() end,
})

widget:build(function()
  return wisp.ui.row { width = "fill", height = "fill" }
end)

return widget:run()
