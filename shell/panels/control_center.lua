-- Control Center Master Panel for Wisp
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

  local wifi_card = ui.Card({
    bg = wifi_stat.enabled and C.bg_light or C.bg_dark,
    border_color = wifi_stat.enabled and C.accent or tokens.border.inactive,
    padding = S.md,
    flex = 1,
    on_click = function()
      net.toggle_wifi()
      widget.on_refresh()
    end,
  }, wibox.layout.fixed.horizontal {
    spacing = S.sm,
    wibox.widget.textbox { text = wifi_stat.icon, size = F.heading, color = wifi_stat.enabled and C.accent or C.fg_muted },
    wibox.layout.fixed.vertical {
      spacing = 2,
      wibox.widget.textbox { text = "Wi-Fi", size = F.body_sm, color = C.fg },
      wibox.widget.textbox { text = wifi_stat.connected and wifi_stat.ssid or (wifi_stat.enabled and "On" or "Off"), size = F.caption, color = C.fg_muted },
    }
  })

  local bt_card = ui.Card({
    bg = bt_stat.powered and C.bg_light or C.bg_dark,
    border_color = bt_stat.powered and C.accent or tokens.border.inactive,
    padding = S.md,
    flex = 1,
    on_click = function()
      bt.toggle_power()
      widget.on_refresh()
    end,
  }, wibox.layout.fixed.horizontal {
    spacing = S.sm,
    wibox.widget.textbox { text = bt_stat.icon, size = F.heading, color = bt_stat.powered and C.accent or C.fg_muted },
    wibox.layout.fixed.vertical {
      spacing = 2,
      wibox.widget.textbox { text = "Bluetooth", size = F.body_sm, color = C.fg },
      wibox.widget.textbox { text = bt_stat.powered and (bt_stat.connected_count .. " connected") or "Off", size = F.caption, color = C.fg_muted },
    }
  })

  local mute_card = ui.Card({
    bg = muted and C.bg_light or C.bg_dark,
    border_color = muted and C.urgent or tokens.border.inactive,
    padding = S.md,
    flex = 1,
    on_click = function()
      muted = audio.toggle_mute()
      widget.on_refresh()
    end,
  }, wibox.layout.fixed.horizontal {
    spacing = S.sm,
    wibox.widget.textbox { text = muted and "󰖁" or "󰕾", size = F.heading, color = muted and C.urgent or C.fg_muted },
    wibox.layout.fixed.vertical {
      spacing = 2,
      wibox.widget.textbox { text = "Audio Mute", size = F.body_sm, color = C.fg },
      wibox.widget.textbox { text = muted and "Muted" or (vol .. "%"), size = F.caption, color = C.fg_muted },
    }
  })

  local power_card = ui.Card({
    bg = C.bg_dark,
    border_color = tokens.border.inactive,
    padding = S.md,
    flex = 1,
    on_click = function()
      local next_p = (power_profile == "power-saver") and "balanced"
        or ((power_profile == "balanced") and "performance" or "power-saver")
      bat.set_power_profile(next_p)
      power_profile = next_p
      widget.on_refresh()
    end,
  }, wibox.layout.fixed.horizontal {
    spacing = S.sm,
    wibox.widget.textbox { text = "󰓅", size = F.heading, color = C.accent },
    wibox.layout.fixed.vertical {
      spacing = 2,
      wibox.widget.textbox { text = "Power Profile", size = F.body_sm, color = C.fg },
      wibox.widget.textbox { text = power_profile, size = F.caption, color = C.accent },
    }
  })

  local toggles_grid = wibox.layout.fixed.vertical {
    spacing = S.xs,
    width = "fill",
    wibox.layout.flex.horizontal { spacing = S.xs, wifi_card, bt_card },
    wibox.layout.flex.horizontal { spacing = S.xs, mute_card, power_card },
  }

  local bat_row = wibox.layout.align.horizontal(
    wibox.layout.fixed.horizontal {
      spacing = S.xs,
      wibox.widget.textbox { text = bat_info.icon, size = F.body, color = C.green },
      wibox.widget.textbox { text = "Battery", size = F.body_sm, color = C.fg },
    },
    nil,
    wibox.widget.textbox { text = (bat_info.percentage or 100) .. "% • " .. bat_info.status, size = F.caption, color = C.green }
  )

  local uptime_row = wibox.layout.align.horizontal(
    wibox.layout.fixed.horizontal {
      spacing = S.xs,
      wibox.widget.textbox { text = "󰥔", size = F.body, color = C.cyan },
      wibox.widget.textbox { text = "Uptime", size = F.body_sm, color = C.fg },
    },
    nil,
    wibox.widget.textbox { text = uptime_str, size = F.caption, color = C.cyan }
  )

  local metrics_card = ui.Card({
    bg = C.bg_dark,
    border_color = tokens.border.inactive,
    padding = S.md,
    gap = S.sm,
    width = "fill",
  }, wibox.layout.fixed.vertical {
    spacing = S.sm,
    width = "fill",
    ui.MetricRow("󰍛", "CPU Usage", cpu_usage .. "%", cpu_usage, C.accent),
    ui.MetricRow("󰘚", "Memory", mem_text .. " (" .. mem_usage .. "%)", mem_usage, C.magenta),
    bat_row,
    uptime_row,
  })

  local panel_shortcuts = wibox.layout.flex.horizontal {
    spacing = S.xs,
    width = "fill",
    wibox.widget.button { text = "󰤨 Wi-Fi", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("wifi") end },
    wibox.widget.button { text = "󰂯 BT", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("bluetooth") end },
    wibox.widget.button { text = "󰕾 Audio", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("audio") end },
    wibox.widget.button { text = "󰃠 Display", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("display") end },
    wibox.widget.button { text = "󰁹 Battery", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("battery") end },
    wibox.widget.button { text = "󰅍 Clip", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("clipboard") end },
    wibox.widget.button { text = "󰸗 Cal", size = F.caption, color = C.fg, bg = C.bg_dark, padding = 4, on_click = function() open_panel("calendar") end },
    wibox.widget.button { text = "󰐥 Power", size = F.caption, color = C.urgent, bg = C.bg_dark, padding = 4, on_click = function() open_panel("power") end },
  }

  return wibox.container.background {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    widget = wibox.layout.fixed.vertical {
      spacing = S.panel_gap,

      ui.PanelHeader("Control Center", os.date("%A, %B %d"), function()
        panels.close("control_center")
      end, "󰍜"),

      toggles_grid,

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

      metrics_card,

      ui.SectionTitle("Panels"),

      panel_shortcuts,
    }
  }
end)

return widget:run()
