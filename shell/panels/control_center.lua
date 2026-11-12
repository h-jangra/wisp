-- Control Center Master Panel for Wisp

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
local net = require("services.network")
local bt = require("services.bluetooth")
local audio = require("services.audio")
local bright = require("services.brightness")
local bat = require("services.battery")

local C = tokens.color
local S = tokens.space
local F = tokens.font

local wifi_stat = net.get_status()
local bt_stat = bt.get_status()
local vol = audio.get_volume()
local muted = audio.is_muted()
local cur_bright = bright.get_brightness()
local bat_info = bat.get_status()
local power_profile = bat.get_power_profile()

local widget = wisp.widget("control_center", {
  width = 420,
  height = 540,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  interval = 3000,
})

widget.on_refresh = function()
  wifi_stat = net.get_status()
  bt_stat = bt.get_status()
  vol = audio.get_volume()
  muted = audio.is_muted()
  cur_bright = bright.get_brightness()
  bat_info = bat.get_status()
  power_profile = bat.get_power_profile()
  widget:redraw()
end

wisp.keyboard.bind("Escape", function() panels.close("control_center") end)

widget:build(function()
  local cpu_usage = 0
  local mem_usage = 0
  local mem_text = "0 MB"
  local uptime_str = "0h 0m"

  if wisp and wisp.system then
    if wisp.system.cpu then
      local c = wisp.system.cpu()
      cpu_usage = math.floor((c and c.usage or 0) + 0.5)
    end
    if wisp.system.memory then
      local m = wisp.system.memory()
      mem_usage = math.floor((m and m.percentage or 0) + 0.5)
      mem_text = string.format("%d / %d MB", m and m.used_mb or 0, m and m.total_mb or 0)
    end
    if wisp.system.uptime then
      local up = math.floor(wisp.system.uptime() or 0)
      local hours = math.floor(up / 3600)
      local mins = math.floor((up % 3600) / 60)
      uptime_str = string.format("%dh %dm", hours, mins)
    end
  end

  local function open_panel(panel_name)
    panels.close("control_center")
    panels.open(panel_name)
  end

  return wisp.ui.col {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    gap = S.panel_gap,

    ui.PanelHeader("Control Center", os.date("%A, %B %d"), function()
      panels.close("control_center")
    end, "󰍜"),

    -- 2x2 Quick Toggles Grid
    wisp.ui.col {
      gap = S.xs,
      wisp.ui.row {
        gap = S.xs,
        wisp.ui.card({
          bg = wifi_stat.enabled and C.bg_light or C.bg_dark,
          border_color = wifi_stat.enabled and C.accent or tokens.border.inactive,
          padding = S.md,
          on_click = function()
            net.toggle_wifi()
            widget.on_refresh()
          end,
        }, wisp.ui.row {
          align = { y = "center" },
          gap = S.sm,
          wisp.ui.label { text = wifi_stat.icon, size = F.heading, color = wifi_stat.enabled and C.accent or C.fg_muted },
          wisp.ui.col {
            gap = 2,
            wisp.ui.label { text = "Wi-Fi", size = F.body_sm, color = C.fg },
            wisp.ui.label { text = wifi_stat.connected and wifi_stat.ssid or (wifi_stat.enabled and "On" or "Off"), size = F.caption, color = C.fg_muted },
          }
        }),

        wisp.ui.card({
          bg = bt_stat.powered and C.bg_light or C.bg_dark,
          border_color = bt_stat.powered and C.accent or tokens.border.inactive,
          padding = S.md,
          on_click = function()
            bt.toggle_power()
            widget.on_refresh()
          end,
        }, wisp.ui.row {
          align = { y = "center" },
          gap = S.sm,
          wisp.ui.label { text = bt_stat.icon, size = F.heading, color = bt_stat.powered and C.accent or C.fg_muted },
          wisp.ui.col {
            gap = 2,
            wisp.ui.label { text = "Bluetooth", size = F.body_sm, color = C.fg },
            wisp.ui.label { text = bt_stat.powered and (bt_stat.connected_count .. " connected") or "Off", size = F.caption, color = C.fg_muted },
          }
        })
      },

      wisp.ui.row {
        gap = S.xs,
        wisp.ui.card({
          bg = muted and C.bg_light or C.bg_dark,
          border_color = muted and C.urgent or tokens.border.inactive,
          padding = S.md,
          on_click = function()
            muted = audio.toggle_mute()
            widget.on_refresh()
          end,
        }, wisp.ui.row {
          align = { y = "center" },
          gap = S.sm,
          wisp.ui.label { text = muted and "󰖁" or "󰕾", size = F.heading, color = muted and C.urgent or C.fg_muted },
          wisp.ui.col {
            gap = 2,
            wisp.ui.label { text = "Audio Mute", size = F.body_sm, color = C.fg },
            wisp.ui.label { text = muted and "Muted" or (vol .. "%"), size = F.caption, color = C.fg_muted },
          }
        }),

        wisp.ui.card({
          bg = C.bg_dark,
          border_color = tokens.border.inactive,
          padding = S.md,
          on_click = function()
            local next_p = (power_profile == "power-saver") and "balanced"
              or ((power_profile == "balanced") and "performance" or "power-saver")
            bat.set_power_profile(next_p)
            power_profile = next_p
            widget.on_refresh()
          end,
        }, wisp.ui.row {
          align = { y = "center" },
          gap = S.sm,
          wisp.ui.label { text = "󰓅", size = F.heading, color = C.accent },
          wisp.ui.col {
            gap = 2,
            wisp.ui.label { text = "Power Profile", size = F.body_sm, color = C.fg },
            wisp.ui.label { text = power_profile, size = F.caption, color = C.accent },
          }
        })
      }
    },

    -- Sliders
    ui.SliderRow("Display Brightness", bright.get_icon(cur_bright), cur_bright, 1, 100, function(v)
      cur_bright = math.floor(v)
      bright.set_brightness(cur_bright)
      widget:redraw()
    end, "%"),

    ui.SliderRow("Output Volume", audio.get_icon(vol, muted), vol, 0, 100, function(v)
      vol = math.floor(v)
      audio.set_volume(vol)
      widget:redraw()
    end, "%"),

    -- System Metrics
    ui.Card({
      bg = C.bg_dark,
      padding = S.md,
      gap = S.sm,
    }, wisp.ui.col {
      gap = S.xs,
      wisp.ui.row {
        wisp.ui.label { text = "󰍛 CPU: " .. cpu_usage .. "%", size = F.caption, color = C.fg },
        wisp.ui.spacer { flex = 1 },
        wisp.ui.label { text = "󰘚 RAM: " .. mem_usage .. "% (" .. mem_text .. ")", size = F.caption, color = C.fg },
      },
      wisp.ui.row {
        wisp.ui.label { text = bat_info.icon .. " Battery: " .. (bat_info.percentage or 100) .. "% (" .. bat_info.status .. ")", size = F.caption, color = C.fg_muted },
        wisp.ui.spacer { flex = 1 },
        wisp.ui.label { text = "󰥔 Up: " .. uptime_str, size = F.caption, color = C.fg_muted },
      }
    }),

    -- Direct Shortcuts
    ui.SectionTitle("Panels"),

    wisp.ui.row {
      gap = S.xs,
      wisp.ui.button { text = "󰤨 Wi-Fi", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("wifi") end },
      wisp.ui.button { text = "󰂯 BT", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("bluetooth") end },
      wisp.ui.button { text = "󰕾 Audio", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("audio") end },
      wisp.ui.button { text = "󰃠 Display", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("display") end },
      wisp.ui.button { text = "󰁹 Battery", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("battery") end },
      wisp.ui.button { text = "󰅍 Clip", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("clipboard") end },
      wisp.ui.button { text = "󰸗 Cal", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("calendar") end },
    }
  }
end)

return widget:run()
