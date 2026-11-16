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
  margin = { top = 30 },
  transparent = true,
  visible = false,
  click_through = false,
  on_click = function()
    local pop = (wisp and wisp.popup) or package.loaded["lib.popup"]
    if pop and pop.hide then pop.hide() end
    panels.close_all()
  end,
  on_key = function(sym)
    if sym == 65307 or sym == 27 then
      local pop = (wisp and wisp.popup) or package.loaded["lib.popup"]
      if pop and pop.hide then pop.hide() end
      panels.close_all()
    end
  end,
})

widget:build(function()
  return wibox.layout.fixed.horizontal { width = "fill", height = "fill" }
end)

return widget:run()
