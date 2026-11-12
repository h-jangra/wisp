-- Display & Brightness System Panel for Wisp

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

wisp.keyboard.bind("Escape", function() panels.close("display") end)

widget:build(function()
  local presets = { 25, 50, 75, 100 }
  local preset_buttons = {}
  for _, p in ipairs(presets) do
    table.insert(preset_buttons, wisp.ui.button {
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

  return wisp.ui.col {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    gap = S.panel_gap,

    ui.PanelHeader("Display & Brightness", cur_bright .. "% Brightness", function()
      panels.close("display")
    end, bright.get_icon(cur_bright)),

    ui.SliderRow("Screen Brightness", bright.get_icon(cur_bright), cur_bright, 1, 100, function(val)
      cur_bright = math.floor(val)
      bright.set_brightness(cur_bright)
      widget:redraw()
    end, "%"),

    wisp.ui.row {
      align = { y = "center" },
      gap = S.xs,
      wisp.ui.label { text = "Presets:", size = F.caption, color = C.fg_muted },
      wisp.ui.spacer { flex = 1 },
      table.unpack(preset_buttons)
    },

    ui.SectionTitle("Active Display"),

    ui.Card({
      bg = C.bg_dark,
      padding = S.md,
    }, wisp.ui.col {
      gap = S.xs,
      wisp.ui.row {
        wisp.ui.label { text = mon.name, size = F.body, color = C.fg },
        wisp.ui.spacer { flex = 1 },
        ui.Chip(mon.refresh_rate, { bg = C.bg_light, color = C.accent, size = F.caption }),
      },
      wisp.ui.row {
        wisp.ui.label { text = mon.description, size = F.caption, color = C.fg_muted },
        wisp.ui.spacer { flex = 1 },
        wisp.ui.label { text = mon.resolution .. " • scale " .. mon.scale, size = F.caption, color = C.fg_light },
      }
    })
  }
end)

return widget:run()
