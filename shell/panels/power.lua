-- Power & Session System Panel for Wisp
-- AwesomeWM-inspired widget architecture (wibox.layout, wibox.container, wibox.widget)

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

local C = tokens.color
local S = tokens.space
local F = tokens.font

local widget = wisp.widget("power", {
  width = 340,
  height = 360,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
})

local function run_cmd(cmd)
  if wisp and wisp.process and wisp.process.spawn then
    wisp.process.spawn("sh", { args = { "-c", cmd } })
  elseif io and io.popen then
    pcall(io.popen, cmd .. " 2>/dev/null &")
  end
end

local function power_action(name, cmd)
  panels.close("power")
  run_cmd(cmd)
end

widget:build(function()
  local actions = {
    { icon = "󰌾", label = "Lock Session", desc = "Lock desktop session", cmd = "loginctl lock-session || hyprlock || swaylock", color = C.accent },
    { icon = "󰤄", label = "Suspend", desc = "Sleep system to RAM", cmd = "systemctl suspend", color = C.yellow },
    { icon = "󰑐", label = "Reboot", desc = "Restart computer", cmd = "systemctl reboot", color = C.orange },
    { icon = "󰐥", label = "Power Off", desc = "Shut down computer", cmd = "systemctl poweroff", color = C.urgent },
  }

  local action_cards = {}
  for _, act in ipairs(actions) do
    local left = wibox.layout.fixed.horizontal {
      spacing = S.md,
      wibox.widget.textbox { text = act.icon, size = F.heading, color = act.color },
      wibox.layout.fixed.vertical {
        spacing = 2,
        wibox.widget.textbox { text = act.label, size = F.body, color = C.fg },
        wibox.widget.textbox { text = act.desc, size = F.caption, color = C.fg_muted },
      }
    }

    local right = wibox.widget.textbox { text = "󰅂", size = F.body, color = C.fg_muted }

    table.insert(action_cards, ui.Card({
      bg = C.bg_dark,
      border_color = tokens.border.inactive,
      padding = S.md,
      width = "fill",
      on_click = function() power_action(act.label, act.cmd) end,
    }, wibox.layout.align.horizontal(left, nil, right)))
  end

  local cards_col = wibox.layout.fixed.vertical {
    spacing = S.sm,
    width = "fill",
    table.unpack(action_cards),
  }

  local cancel_btn = wibox.widget.button {
    text = "Cancel",
    size = F.body_sm,
    color = C.fg,
    bg = C.bg_dark,
    width = "fill",
    padding = 6,
    radius = tokens.radius,
    on_click = function() panels.close("power") end,
  }

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Power & Session", "Session options", function()
        panels.close("power")
      end, "󰐥"),

      cards_col,

      wibox.widget.spacer { flex = 1 },

      cancel_btn,
    }
  }
end)

return widget:run()
