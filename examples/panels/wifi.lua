-- Wi-Fi System Panel for Wisp
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
local ui = require("widgets.ui")
local net = require("services.network")
local panels = require("lib.panels")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local widget

local status = net.get_status()
local networks = {}
local scan_state = (status and not status.enabled) and "disabled" or "idle"
local scan_scheduled = false
local last_scan = 0

local function rescan()
  status = net.get_status()
  if status.unavailable then
    networks = {}
    scan_state = "error"
  elseif not status.enabled then
    networks = {}
    scan_state = "disabled"
  else
    local ok, list = pcall(net.scan_networks)
    networks = ok and list or {}
    scan_state = ok and "ready" or "error"
  end
  last_scan = os.time()
  if widget then widget:redraw() end
end

local function schedule_scan()
  if scan_scheduled then return end
  scan_scheduled = true
  wisp.timer.after(1, function()
    scan_scheduled = false
    rescan()
  end)
end

local widget = wisp.widget("wifi", {
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

widget:build(function()
  if scan_state == "idle" or os.time() - last_scan >= 4 then
    schedule_scan()
  end

  local net_rows = {}
  if scan_state == "ready" then
    for _, item in ipairs(networks) do
      local is_conn = item.connected
      local sig_icon = item.signal > 60 and "󰤨" or (item.signal > 30 and "󰤥" or "󰤟")

      local left = wibox.layout.fixed.horizontal {
        spacing = S.md,
        wibox.widget.textbox {
          text = sig_icon,
          size = F.heading,
          color = is_conn and C.accent or C.fg_muted,
        },
        wibox.layout.fixed.vertical {
          spacing = 2,
          flex = 1,
          wibox.widget.textbox { text = item.ssid, size = F.body, color = is_conn and C.accent or C.fg, wrap = true },
          wibox.widget.textbox { text = item.security .. " • " .. item.signal .. "%", size = F.caption, color = C.fg_muted },
        }
      }

      local right = is_conn and ui.Chip("Connected", { bg = C.accent, color = C.bg_darker, size = F.caption })
        or wibox.widget.button {
          text = "Connect",
          size = F.caption,
          color = C.fg,
          bg = C.bg_light,
          radius = R,
          padding = 4,
          on_click = function()
            net.connect(item.ssid)
            rescan()
          end
        }

      table.insert(net_rows, ui.Card({
        width = "fill",
        bg = is_conn and C.bg_light or C.bg_dark,
        border_color = is_conn and C.accent or B.inactive,
        padding = S.md,
        on_click = function()
          if not is_conn then
            net.connect(item.ssid)
            rescan()
          end
        end,
      }, wibox.layout.align.horizontal(left, nil, right)))
    end
  end

  local body
  if (status and not status.enabled) or scan_state == "disabled" then
    body = ui.EmptyState("󰤭", "Wi-Fi is turned off")
  elseif scan_state == "error" or (status and status.unavailable) then
    body = ui.EmptyState("󰖪", "Network services unavailable")
  elseif scan_state == "idle" then
    body = ui.EmptyState("󰤩", "Scanning for networks…")
  elseif #networks == 0 then
    body = ui.EmptyState("󰤟", "No networks found")
  else
    local list_layout = wibox.layout.fixed.vertical {
      spacing = S.sm,
      table.unpack(net_rows),
    }
    body = wibox.container.scroll(list_layout)
  end

  return wibox.container.background {
    bg = C.bg,
    border = { color = B.active, width = B.width },
    radius = R,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Wi-Fi Networks", status and status.connected and status.ssid or "Not Connected", function()
        panels.close("wifi")
      end, status and status.icon),

      ui.ToggleRow("Wi-Fi Radio", "󰤨", status and status.enabled, function()
        net.toggle_wifi()
        rescan()
      end, status and status.enabled and "Enabled" or "Disabled"),

      ui.SectionTitle("Available Networks", "Scan", function()
        rescan()
      end),

      body,
    }
  }
end)

return widget:run()
