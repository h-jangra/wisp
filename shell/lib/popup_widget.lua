-- Dedicated Overlay Surface for Tooltips and Popups

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

local popup = require("lib.popup")

local widget = wisp.widget("wisp_popup", {
  anchor = "top_left",
  layer = "overlay",
  transparent = true,
  visible = false,
  width = 120,
  height = 32,
  order = 100,
  on_key = function(sym)
    if sym == 65307 or sym == 27 then
      popup.hide()
    end
  end,
})

widget:build(function()
  return popup.render_content()
end)

return widget:run()
