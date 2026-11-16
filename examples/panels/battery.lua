-- Battery & Power Management System Panel for Wisp
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
local bat = require("services.battery")
local C = tokens.color
local S = tokens.space
local F = tokens.font

local info = bat.get_status()
local profile = bat.get_power_profile()

local widget = wisp.widget("battery", {
  width = 360,
  height = 410,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  interval = 4000,
})

widget.on_refresh = function()
  info = bat.get_status()
  profile = bat.get_power_profile()
  widget:redraw()
end

widget:build(function()
  local pct = info.percentage or 100
  local chg = info.charging == true
  local bar_col = chg and C.green or (pct < 20 and C.urgent or C.accent)

  local profiles = {
    { id = "power-saver", label = "Power Saver", icon = "󰌪" },
    { id = "balanced", label = "Balanced", icon = "󰗑" },
    { id = "performance", label = "Performance", icon = "󰓅" },
  }

  local profile_row = wibox.layout.flex.horizontal { spacing = S.xs }
  for _, p in ipairs(profiles) do
    local is_act = (profile == p.id)
    profile_row:add(wibox.widget.button {
      flex = 1,
      text = p.icon .. " " .. p.label,
      size = F.caption,
      color = is_act and C.bg_darker or C.fg,
      bg = is_act and C.accent or C.bg_dark,
      border = { color = is_act and C.accent or tokens.border.inactive, width = 1 },
      radius = tokens.radius,
      padding = 6,
      on_click = function()
        bat.set_power_profile(p.id)
        profile = p.id
        widget:redraw()
      end
    })
  end

  local top_right = wibox.layout.fixed.horizontal {
    spacing = S.xs,
    ui.Chip(chg and "Charging" or (pct < 20 and "Low" or "Normal"), {
      bg = chg and C.green or (pct < 20 and C.urgent or C.bg_light),
      color = (chg or pct < 20) and C.bg_darker or C.fg,
      size = F.caption,
    }),
    wibox.widget.textbox { text = pct .. "%", size = F.heading, color = bar_col },
  }

  local top_row = wibox.layout.align.horizontal(
    wibox.widget.textbox { text = info.icon .. " Level", size = F.body, color = C.fg },
    nil,
    top_right
  )

  local bottom_row = wibox.layout.align.horizontal(
    wibox.widget.textbox { text = "Status: " .. info.status, size = F.caption, color = C.fg_muted },
    nil,
    (info.power_watts and info.power_watts > 0)
      and wibox.widget.textbox { text = string.format("%.1fW draw", info.power_watts), size = F.caption, color = C.fg_muted }
      or nil
  )

  local battery_card = ui.Card({
    width = "fill",
    bg = C.bg_dark,
    padding = S.md,
    gap = S.sm,
  }, wibox.layout.fixed.vertical {
    spacing = S.sm,
    top_row,
    wibox.widget.progressbar {
      value = pct,
      max_value = 100,
      height = 8,
      radius = 0,
      color = bar_col,
      background_color = C.bg_light,
    },
    bottom_row,
  })

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Battery & Power", pct .. "% • " .. info.status, function()
        panels.close("battery")
      end, info.icon),

      battery_card,

      ui.SectionTitle("Power Profile"),

      profile_row,
    }
  }
end)

return widget:run()
