-- Bluetooth System Panel for Wisp
-- Swiss Technical Architecture: Proofline editorial hierarchy, Oxide precision, retained widgets, zero polling

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
local panels = require("lib.panels")
local bt = require("services.bluetooth")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local widget = wisp.widget("bluetooth", {
  width = 360,
  height = 420,
  anchor = "top_right",
  margin = { top = 30, right = 10 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  -- interval is intentionally nil: event-driven, zero polling
})

local header
local toggle_row
local section_head
local empty_state
local list_layout
local row_cache = {}

local function apply_device_list(devices, status)
  if not list_layout then return end
  status = status or bt.get_status()
  devices = devices or bt.get_devices()

  if not status.powered then
    empty_state:set_state("󰂲", "Bluetooth is disabled", "Toggle the power switch above to enable")
    list_layout:set_children({ empty_state })
    return
  end

  if #devices == 0 then
    empty_state:set_state("󰂯", "No paired devices", "Pair devices via bluetoothctl or system settings")
    list_layout:set_children({ empty_state })
    return
  end

  local rows = {}
  local seen = {}

  for _, dev in ipairs(devices) do
    local is_conn = dev.connected
    local is_connecting = dev.connecting == true
    local existing = row_cache[dev.mac]

    if existing then
      existing.row:set_title(dev.name, is_conn and C.accent or C.fg)
      existing.row:set_subtitle(dev.mac .. (is_conn and " • Connected" or (is_connecting and " • Connecting…" or " • Paired")))
      existing.row:set_icon(is_conn and "󰂱" or "󰂯", is_conn and C.accent or C.fg_muted)
      existing.row:set_active(is_conn)

      if is_conn then
        existing.btn:set_text("Disconnect")
        existing.btn:set_color(C.fg)
        existing.btn:set_bg(stl.surface and stl.surface.sunken or C.bg_light)
        existing.btn:set_border({ color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 })
      elseif is_connecting then
        existing.btn:set_text("Connecting…")
        existing.btn:set_color(C.accent)
        existing.btn:set_bg(stl.surface and stl.surface.sunken or C.bg_light)
        existing.btn:set_border({ color = C.accent, width = 1 })
      else
        existing.btn:set_text("Connect")
        existing.btn:set_color(stl.content and stl.content.on_accent or C.bg_darker)
        existing.btn:set_bg(C.accent)
        existing.btn:set_border({ color = C.accent, width = 1 })
      end
    else
      local btn = wisp.widget.button {
        text = is_conn and "Disconnect" or (is_connecting and "Connecting…" or "Connect"),
        size = F.caption,
        color = is_conn and C.fg or (stl.content and stl.content.on_accent or C.bg_darker),
        bg = is_conn and (stl.surface and stl.surface.sunken or C.bg_light) or C.accent,
        border = { color = is_conn and (stl.stroke and stl.stroke.hairline or B.inactive) or C.accent, width = 1 },
        radius = tokens and tokens.radius_sm or 2,
        padding = { left = 6, right = 6, top = 2, bottom = 2 },
        on_click = function()
          if is_conn then
            bt.disconnect(dev.mac)
          else
            bt.connect(dev.mac)
          end
        end
      }

      local row = ui.Row {
        icon = is_conn and "󰂱" or "󰂯",
        icon_color = is_conn and C.accent or C.fg_muted,
        title = dev.name,
        subtitle = dev.mac .. (is_conn and " • Connected" or (is_connecting and " • Connecting…" or " • Paired")),
        active = is_conn,
        action = btn,
        on_click = function()
          if is_conn then
            bt.disconnect(dev.mac)
          else
            bt.connect(dev.mac)
          end
        end
      }

      row_cache[dev.mac] = { row = row, btn = btn, mac = dev.mac }
    end

    seen[dev.mac] = true
    table.insert(rows, row_cache[dev.mac].row)
  end

  for mac, _ in pairs(row_cache) do
    if not seen[mac] then row_cache[mac] = nil end
  end

  list_layout:set_children(rows)
end

local function update_from_status(status, devices)
  status = status or bt.get_status()
  if header then
    local sub = status.powered and (status.connected_count > 0 and (status.connected_count .. " connected") or "Powered on") or "Disabled"
    header:set_title("Bluetooth")
    header:set_subtitle(sub)
    header:set_icon(status.icon or "󰂯")
  end
  if toggle_row then
    toggle_row:set_state(status.powered, status.powered and status.name or "Disabled")
  end
  apply_device_list(devices, status)
end

bt.subscribe(function(status, devices)
  update_from_status(status, devices)
end)

widget:build(function()
  local status = bt.get_status()

  header = ui.PanelHeader("Bluetooth", status.powered and (status.connected_count > 0 and (status.connected_count .. " connected") or "Powered on") or "Disabled", function()
    panels.close("bluetooth")
  end, status.icon or "󰂯")

  toggle_row = ui.ToggleRow("Bluetooth Power", "󰂯", status.powered, function()
    bt.toggle_power()
  end, status.powered and status.name or "Disabled")

  section_head = ui.SectionTitle("Paired Devices", "Refresh", function()
    bt.refresh(function(st, devs)
      update_from_status(st, devs)
    end)
  end)

  empty_state = ui.EmptyState(
    status.powered and "󰂯" or "󰂲",
    status.powered and "No paired devices" or "Bluetooth is disabled",
    status.powered and "Pair devices via bluetoothctl or system settings" or "Toggle the power switch above to enable"
  )

  list_layout = wisp.layout.fixed.vertical {
    spacing = S.xs,
    empty_state,
  }

  local scroll_area = wisp.container.scroll(list_layout)

  apply_device_list()

  return wisp.container.background {
    bg = stl.surface and stl.surface.canvas or C.bg,
    border = { color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 },
    radius = tokens and tokens.radius_md or 4,
    padding = S.panel_pad,
    widget = wisp.layout.fixed.vertical {
      spacing = S.panel_gap,
      header,
      toggle_row,
      ui.Divider(),
      section_head,
      scroll_area,
    }
  }
end)

widget:on_show(function()
  bt.refresh(function(st, devs)
    update_from_status(st, devs)
  end)
end)

return widget:run()
