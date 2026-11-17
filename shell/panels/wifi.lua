-- Wi-Fi System Panel for Wisp
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
local net = require("services.network")
local panels = require("lib.panels")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local widget = wisp.widget("wifi", {
  width = 360,
  height = 440,
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
local section_title
local empty_state
local list_layout
local scroll_container

local row_cache = {}
local is_scanning = false

local function handle_connect(ssid, row_obj)
  if row_obj then row_obj:set_connecting() end
  if header then header:set_subtitle("Connecting to " .. ssid .. "…") end

  net.connect(ssid, nil, function(success, msg)
    if success then
      if header then
        header:set_subtitle(ssid .. " • Connected")
        header:set_icon("󰤨")
      end
      if row_obj then
        row_obj:update({
          ssid = ssid,
          connected = true,
          signal = row_obj.signal,
          security = row_obj.security,
        }, handle_connect)
      end
    else
      if header then header:set_subtitle("Connection failed") end
      if row_obj then
        row_obj:update({
          ssid = ssid,
          connected = false,
          signal = row_obj.signal,
          security = row_obj.security,
        }, handle_connect)
      end
    end
  end)
end

local function make_network_row(item)
  local sig_icon = item.signal > 60 and "󰤨" or (item.signal > 30 and "󰤥" or "󰤟")

  local conn_btn = wisp.widget.button {
    text = item.connected and "Connected" or "Connect",
    size = F.caption,
    color = item.connected and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg,
    bg = item.connected and C.accent or (stl.surface and stl.surface.sunken or C.bg_light),
    border = { color = item.connected and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 },
    radius = tokens and tokens.radius_sm or 2,
    padding = { left = 6, right = 6, top = 2, bottom = 2 },
  }

  local row = ui.Row {
    icon = sig_icon,
    icon_color = item.connected and C.accent or C.fg_muted,
    title = item.ssid,
    subtitle = item.security .. " • " .. item.signal .. "%",
    active = item.connected,
    action = conn_btn,
  }

  local row_obj = {
    row = row,
    conn_btn = conn_btn,
    ssid = item.ssid,
    connected = item.connected,
    signal = item.signal,
    security = item.security,
  }

  function row_obj:update(data, on_connect)
    local is_conn = data.connected == true
    local sig = data.signal or 0
    local sec = data.security or "Open"
    local icon = sig > 60 and "󰤨" or (sig > 30 and "󰤥" or "󰤟")

    self.connected = is_conn
    self.signal = sig
    self.security = sec

    self.row:set_icon(icon, is_conn and C.accent or C.fg_muted)
    self.row:set_title(data.ssid, is_conn and C.accent or C.fg)
    self.row:set_subtitle(sec .. " • " .. sig .. "%")
    self.row:set_active(is_conn)

    if is_conn then
      self.conn_btn:set_text("Connected")
      self.conn_btn:set_color(stl.content and stl.content.on_accent or C.bg_darker)
      self.conn_btn:set_bg(C.accent)
      self.conn_btn:set_border({ color = C.accent, width = 1 })
      self.conn_btn:set_on_click(nil)
      self.row:set_on_click(nil)
    else
      self.conn_btn:set_text("Connect")
      self.conn_btn:set_color(C.fg)
      self.conn_btn:set_bg(stl.surface and stl.surface.sunken or C.bg_light)
      self.conn_btn:set_border({ color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 })
      local click_fn = function()
        if on_connect then on_connect(data.ssid, self) end
      end
      self.conn_btn:set_on_click(click_fn)
      self.row:set_on_click(click_fn)
    end
  end

  function row_obj:set_connecting()
    self.conn_btn:set_text("Connecting…")
    self.conn_btn:set_color(C.accent)
    self.conn_btn:set_bg(stl.surface and stl.surface.sunken or C.bg_light)
    self.conn_btn:set_on_click(nil)
    self.row:set_on_click(nil)
  end

  row_obj:update(item, handle_connect)
  return row_obj
end

local function apply_network_list(networks)
  if not list_layout then return end

  local status = net.get_status()
  if status and status.unavailable then
    empty_state:set_state("󰖪", "Network services unavailable", "Check NetworkManager daemon status")
    list_layout:set_children({ empty_state })
    return
  end

  if status and not status.enabled then
    empty_state:set_state("󰤭", "Wi-Fi is turned off", "Toggle the radio switch above to enable")
    list_layout:set_children({ empty_state })
    return
  end

  if not networks or #networks == 0 then
    empty_state:set_state("󰤟", "No networks found", "Ensure an access point is within range")
    list_layout:set_children({ empty_state })
    return
  end

  local cards = {}
  local seen = {}

  for _, item in ipairs(networks) do
    local row_obj = row_cache[item.ssid]
    if row_obj then
      row_obj:update(item, handle_connect)
    else
      row_obj = make_network_row(item)
      row_cache[item.ssid] = row_obj
    end
    seen[item.ssid] = true
    table.insert(cards, row_obj.row)
  end

  for ssid, _ in pairs(row_cache) do
    if not seen[ssid] then row_cache[ssid] = nil end
  end

  list_layout:set_children(cards)
end

local function trigger_scan()
  if is_scanning then return end
  local status = net.get_status()
  if status and not status.enabled then return end

  is_scanning = true
  if section_title then section_title:set_action_text("Scanning…") end

  net.scan_networks(function(networks, success)
    is_scanning = false
    if section_title then section_title:set_action_text("Scan") end
    if success or #networks > 0 then
      apply_network_list(networks)
    else
      empty_state:set_state("󰖪", "Scan error", "Failed to retrieve access points")
      list_layout:set_children({ empty_state })
    end
  end)
end

local function update_from_status(status)
  if not status then return end
  if header then
    header:set_subtitle(status.connected and (status.ssid .. " • Connected") or "Not Connected")
    header:set_icon(status.icon or "󰤨")
  end
  if toggle_row then
    toggle_row:set_state(status.enabled, status.enabled and "Enabled" or "Disabled")
  end
  if not status.enabled then
    empty_state:set_state("󰤭", "Wi-Fi is turned off", "Toggle the radio switch above to enable")
    if list_layout then list_layout:set_children({ empty_state }) end
  end
end

net.subscribe(function(status)
  update_from_status(status)
end)

widget:build(function()
  local status = net.get_status()

  header = ui.PanelHeader("Wi-Fi Networks", status and status.connected and (status.ssid .. " • Connected") or "Not Connected", function()
    panels.close("wifi")
  end, status and status.icon or "󰤨")

  toggle_row = ui.ToggleRow("Wi-Fi Radio", "󰤨", status and status.enabled, function()
    local new_state = net.toggle_wifi()
    if new_state then
      trigger_scan()
    else
      apply_network_list({})
    end
  end, status and status.enabled and "Enabled" or "Disabled")

  section_title = ui.SectionTitle("Available Networks", "Scan", function()
    trigger_scan()
  end)

  empty_state = ui.EmptyState(
    (status and not status.enabled) and "󰤭" or "󰤩",
    (status and not status.enabled) and "Wi-Fi is turned off" or "Scanning for networks…",
    (status and not status.enabled) and "Toggle the radio switch above to enable" or "Querying wireless interfaces"
  )

  list_layout = wisp.layout.fixed.vertical {
    spacing = S.xs,
    empty_state,
  }

  scroll_container = wisp.container.scroll(list_layout)

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
      section_title,
      scroll_container,
    }
  }
end)

widget:on_show(function()
  local status = net.get_status()
  update_from_status(status)
  if status and status.enabled then
    trigger_scan()
  end
end)

return widget:run()
