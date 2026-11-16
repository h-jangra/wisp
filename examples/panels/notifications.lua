-- Notifications System Panel for Wisp
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

local C = tokens.color
local S = tokens.space
local F = tokens.font

local dnd_active = false

local widget = wisp.widget("notifications", {
  width = 360,
  height = 420,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
})

widget:build(function()
  local svc = (wisp.system and wisp.system.services and wisp.system.services()) or {}
  local notif_count = tonumber(svc.notify_active) or 0
  local total_sent = tonumber(svc.notify_total_sent) or 0

  local metrics_card = ui.Card({
    bg = C.bg_dark,
    border_color = tokens.border.inactive,
    padding = S.md,
    width = "fill",
  }, wibox.layout.fixed.vertical {
    spacing = S.xs,
    wibox.layout.align.horizontal(
      wibox.widget.textbox { text = "󰂚 Total Delivered", size = F.body_sm, color = C.fg },
      nil,
      wibox.widget.textbox { text = tostring(total_sent), size = F.caption, color = C.accent }
    ),
    wibox.layout.align.horizontal(
      wibox.widget.textbox { text = "󰍜 Active Alerts", size = F.body_sm, color = C.fg },
      nil,
      wibox.widget.textbox { text = tostring(notif_count), size = F.caption, color = notif_count > 0 and C.urgent or C.fg_muted }
    ),
  })

  local test_btn = wibox.widget.button {
    text = "Send Test Notification",
    size = F.body_sm,
    color = C.fg,
    bg = C.bg_dark,
    border = { color = tokens.border.inactive, width = 1 },
    width = "fill",
    padding = 6,
    radius = tokens.radius,
    on_click = function()
      if wisp.notify and wisp.notify.send then
        wisp.notify.send({
          title = "Wisp Shell",
          body = "Notification system is active and responsive.",
          urgency = "normal",
          app_name = "Wisp",
        })
      end
      widget:redraw()
    end
  }

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Notifications", notif_count .. " active", function()
        panels.close("notifications")
      end, "󰂚"),

      ui.ToggleRow("Do Not Disturb", "󰂛", dnd_active, function()
        dnd_active = not dnd_active
        widget:redraw()
      end, dnd_active and "Mute notifications" or "Showing notifications"),

      ui.SectionTitle("Recent", "Clear All", function()
        widget:redraw()
      end),

      metrics_card,

      wibox.widget.spacer { flex = 1 },

      test_btn,
    }
  }
end)

return widget:run()
