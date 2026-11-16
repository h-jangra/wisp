-- Display & Brightness System Panel for Wisp
-- AwesomeWM-inspired widget architecture (wibox.layout, wibox.container, wibox.widget)

local function _shell_root()
  local home = os.getenv("HOME") or ""
  local cands = { "examples", "./examples", "shell", "./shell", home .. "/.config/wisp/shell", "/home/hj/Projects/wisp/shell", "/usr/share/wisp/shell" }
  for _, d in ipairs(cands) do
    local f = io.open(d .. "/style.lua", "r")
    if f then f:close(); return d end
  end
  return "shell"
end
local _sr = _shell_root()
package.path = _sr .. "/?.lua;" .. _sr .. "/?/init.lua;" .. package.path

local stl = require("style")
local tokens = stl

local ui = require("widgets.ui")
local panels = require("lib.panels")
local bright = require("services.brightness")
local C = tokens.color
local S = tokens.space
local F = tokens.font

local cur_bright = bright.get_brightness()
local mon = bright.get_monitor_info()

local widget = wisp.widget("display", {
  width = 360,
  height = 400,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  interval = 4000,
})

widget.on_refresh = function()
  cur_bright = bright.get_brightness()
  mon = bright.get_monitor_info()
  widget:redraw()
end

widget:build(function()
  local presets = { 25, 50, 75, 100 }
  local preset_row = wibox.layout.fixed.horizontal { spacing = S.xs }
  for _, p in ipairs(presets) do
    preset_row:add(wibox.widget.button {
      text = p .. "%",
      size = F.caption,
      color = (cur_bright == p) and C.bg_darker or C.fg,
      bg = (cur_bright == p) and C.accent or C.bg_dark,
      border = { color = (cur_bright == p) and C.accent or tokens.border.inactive, width = 1 },
      radius = tokens.radius,
      padding = 5,
      on_click = function()
        cur_bright = p
        bright.set_brightness(p)
        widget:redraw()
      end
    })
  end

  local presets_bar = wibox.layout.align.horizontal(
    wibox.widget.textbox { text = "Presets:", size = F.caption, color = C.fg_muted },
    nil,
    preset_row
  )

  local monitor_card = ui.Card({
    width = "fill",
    bg = C.bg_dark,
    padding = S.md,
  }, wibox.layout.fixed.vertical {
    spacing = S.xs,
    wibox.layout.align.horizontal(
      wibox.widget.textbox { text = mon.name, size = F.body, color = C.fg },
      nil,
      ui.Chip(mon.refresh_rate, { bg = C.bg_light, color = C.accent, size = F.caption })
    ),
    wibox.layout.align.horizontal(
      wibox.widget.textbox { text = mon.description, size = F.caption, color = C.fg_muted, wrap = true },
      nil,
      wibox.widget.textbox { text = mon.resolution .. " • scale " .. mon.scale, size = F.caption, color = C.fg_light }
    )
  })

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Display & Brightness", cur_bright .. "% Brightness", function()
        panels.close("display")
      end, bright.get_icon(cur_bright)),

      ui.SliderRow("Screen Brightness", bright.get_icon(cur_bright), cur_bright, 1, 100, function(val)
        cur_bright = math.floor(val)
        bright.set_brightness(cur_bright)
        widget:redraw()
      end, "%"),

      presets_bar,

      ui.SectionTitle("Active Display"),

      monitor_card,
    }
  }
end)

return widget:run()
