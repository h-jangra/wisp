-- Bluetooth System Panel for Wisp
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
local bt = require("services.bluetooth")
local C = tokens.color
local S = tokens.space
local F = tokens.font

local status = bt.get_status()
local devices = bt.get_devices()

local widget = wisp.widget("bluetooth", {
  width = 360,
  height = 440,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  interval = 5000,
})

widget.on_refresh = function()
  status = bt.get_status()
  if status.powered then
    devices = bt.get_devices()
  else
    devices = {}
  end
  widget:redraw()
end

widget:build(function()
  local dev_rows = {}
  if not status.powered then
    table.insert(dev_rows, ui.EmptyState("󰂲", "Bluetooth is disabled"))
  elseif #devices == 0 then
    table.insert(dev_rows, ui.EmptyState("󰂯", "No paired devices found"))
  else
    for _, dev in ipairs(devices) do
      local is_conn = dev.connected

      local left = wibox.layout.fixed.horizontal {
        spacing = S.md,
        wibox.widget.textbox {
          text = is_conn and "󰂱" or "󰂯",
          size = F.heading,
          color = is_conn and C.accent or C.fg_muted,
        },
        wibox.layout.fixed.vertical {
          spacing = 2,
          flex = 1,
          wibox.widget.textbox { text = dev.name, size = F.body, color = is_conn and C.accent or C.fg, wrap = true },
          wibox.widget.textbox { text = dev.mac, size = F.caption, color = C.fg_muted },
        }
      }

      local right = wibox.widget.button {
        text = is_conn and "Disconnect" or "Connect",
        size = F.caption,
        color = is_conn and C.fg or C.bg_darker,
        bg = is_conn and C.bg_light or C.accent,
        radius = tokens.radius,
        padding = 4,
        on_click = function()
          if is_conn then
            bt.disconnect(dev.mac)
          else
            bt.connect(dev.mac)
          end
          widget.on_refresh()
        end
      }

      table.insert(dev_rows, ui.Card({
        width = "fill",
        bg = is_conn and C.bg_light or C.bg_dark,
        border_color = is_conn and C.accent or tokens.border.inactive,
        padding = S.md,
      }, wibox.layout.align.horizontal(left, nil, right)))
    end
  end

  local list_layout = wibox.layout.fixed.vertical {
    spacing = S.xs,
    table.unpack(dev_rows),
  }

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Bluetooth Devices", status.powered and (status.connected_count .. " connected") or "Disabled", function()
        panels.close("bluetooth")
      end, status.icon),

      ui.ToggleRow("Bluetooth Power", "󰂯", status.powered, function()
        bt.toggle_power()
        widget.on_refresh()
      end, status.powered and status.name or "Disabled"),

      ui.SectionTitle("Paired Devices", "Refresh", function()
        widget.on_refresh()
      end),

      wibox.container.scroll(list_layout),
    }
  }
end)

return widget:run()
