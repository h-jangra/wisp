-- Battery & Power Management System Panel for Wisp

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

wisp.keyboard.bind("Escape", function() panels.close("battery") end)

widget:build(function()
  local pct = info.percentage or 100
  local chg = info.charging == true
  local bar_col = chg and C.green or (pct < 20 and C.urgent or C.accent)

  local profiles = {
    { id = "power-saver", label = "Power Saver", icon = "󰌪" },
    { id = "balanced", label = "Balanced", icon = "󰗑" },
    { id = "performance", label = "Performance", icon = "󰓅" },
  }

  local profile_buttons = {}
  for _, p in ipairs(profiles) do
    local is_act = (profile == p.id)
    table.insert(profile_buttons, wisp.ui.button {
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

  return wisp.ui.col {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    gap = S.panel_gap,

    ui.PanelHeader("Battery & Power", pct .. "% • " .. info.status, function()
      panels.close("battery")
    end, info.icon),

    ui.Card({
      bg = C.bg_dark,
      padding = S.md,
      gap = S.sm,
    }, wisp.ui.col {
      gap = S.sm,
      wisp.ui.row {
        align = { y = "center" },
        wisp.ui.label { text = info.icon .. " Level", size = F.body, color = C.fg },
        wisp.ui.spacer { flex = 1 },
        ui.Chip(chg and "Charging" or (pct < 20 and "Low" or "Normal"), {
          bg = chg and C.green or (pct < 20 and C.urgent or C.bg_light),
          color = (chg or pct < 20) and C.bg_darker or C.fg,
          size = F.caption,
        }),
        wisp.ui.spacer { width = S.xs },
        wisp.ui.label { text = pct .. "%", size = F.heading, color = bar_col },
      },
      wisp.ui.progress {
        value = pct,
        max = 100,
        height = 8,
        radius = 0,
        color = bar_col,
        background = C.bg_light,
      },
      wisp.ui.row {
        wisp.ui.label { text = "Status: " .. info.status, size = F.caption, color = C.fg_muted },
        wisp.ui.spacer { flex = 1 },
        (info.power_watts and info.power_watts > 0)
          and wisp.ui.label { text = string.format("%.1fW draw", info.power_watts), size = F.caption, color = C.fg_muted }
          or nil,
      }
    }),

    ui.SectionTitle("Power Profile"),

    wisp.ui.row {
      gap = S.xs,
      table.unpack(profile_buttons)
    }
  }
end)

return widget:run()
