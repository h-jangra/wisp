-- Wi-Fi System Panel for Wisp

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
local ui = require("widgets.ui")
local net = require("services.network")
local panels = require("lib.panels")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local status = net.get_status()
local networks = net.scan_networks()

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

widget.on_refresh = function()
  status = net.get_status()
  if status.enabled then
    networks = net.scan_networks()
  end
  widget:redraw()
end

wisp.keyboard.bind("Escape", function() panels.close("wifi") end)

widget:build(function()
  local net_rows = {}
  if not status.enabled then
    table.insert(net_rows, ui.EmptyState("󰤭", "Wi-Fi is turned off"))
  elseif #networks == 0 then
    table.insert(net_rows, ui.EmptyState("󰤟", "No networks found"))
  else
    for _, item in ipairs(networks) do
      local is_conn = item.connected
      table.insert(net_rows, ui.Card({
        bg = is_conn and C.bg_light or C.bg_dark,
        border_color = is_conn and C.accent or B.inactive,
        padding = S.md,
        on_click = function()
          if not is_conn then
            net.connect(item.ssid)
            widget.on_refresh()
          end
        end,
      }, wisp.ui.row {
        align = { y = "center" },
        gap = S.md,
        wisp.ui.label {
          text = item.signal > 60 and "󰤨" or (item.signal > 30 and "󰤥" or "󰤟"),
          size = F.heading,
          color = is_conn and C.accent or C.fg_muted,
        },
        wisp.ui.col {
          gap = 2,
          wisp.ui.label { text = item.ssid, size = F.body, color = is_conn and C.accent or C.fg },
          wisp.ui.label { text = item.security .. " • " .. item.signal .. "%", size = F.caption, color = C.fg_muted },
        },
        wisp.ui.spacer { flex = 1 },
        is_conn and ui.Chip("Connected", { bg = C.accent, color = C.bg_darker, size = F.caption })
          or wisp.ui.button {
            text = "Connect",
            size = F.caption,
            color = C.fg,
            bg = C.bg_light,
            radius = R,
            padding = 4,
            on_click = function()
              net.connect(item.ssid)
              widget.on_refresh()
            end
          }
      }))
    end
  end

  return wisp.ui.col {
    bg = C.bg,
    border = { color = B.active, width = B.width },
    radius = R,
    padding = S.panel_pad,
    gap = S.panel_gap,

    ui.PanelHeader("Wi-Fi Networks", status.connected and status.ssid or "Not Connected", function()
      panels.close("wifi")
    end, status.icon),

    ui.ToggleRow("Wi-Fi Radio", "󰤨", status.enabled, function()
      net.toggle_wifi()
      widget.on_refresh()
    end, status.enabled and "Enabled" or "Disabled"),

    ui.SectionTitle("Available Networks", "Scan", function()
      widget.on_refresh()
    end),

    wisp.ui.scroll {
      gap = S.xs,
      table.unpack(net_rows)
    }
  }
end)

return widget:run()
