-- Canonical Wisp Top Bar
-- High-performance retained widget architecture: built once, updated reactively

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

local stl    = require("style")
local panels = require("lib.panels")
local bat    = require("services.battery")
local net    = require("services.network")
local bt     = require("services.bluetooth")
local audio  = require("services.audio")

local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

-- ── Workspace state ──────────────────────────────────────────────────────────
local ws_active = wisp.workspace.active()
local ws_list   = wisp.workspace.list()
local ws_buttons = {}
local ws_container

-- ── Retained subwidgets ─────────────────────────────────────────────────────
local date_btn
local mem_label
local bt_btn
local wifi_btn
local audio_btn
local batt_btn
local right_section

-- Ghost button: flat, borderless, vertically centred in bar.
local function ghost_btn(text, color, on_click, bg, tooltip)
  return wisp.widget.button {
    text     = text,
    size     = F.body,
    color    = color,
    bg       = bg or C.bg,
    width    = "fit",
    height   = 20,
    padding  = { left = 5, right = 5, top = 1, bottom = 1 },
    radius   = R,
    tooltip  = tooltip,
    on_click = on_click,
  }
end

-- Value label (e.g. memory percentage or tray indicators).
local function value_label(text, color, tooltip)
  return wisp.widget.textbox {
    text    = text,
    size    = F.caption,
    color   = color or C.fg_muted,
    width   = "fit",
    height  = 20,
    padding = { left = 4, right = 4, top = 3, bottom = 3 },
    tooltip = tooltip,
  }
end

local function make_workspace_button(ws)
  local is_act = (ws.id == ws_active)
  local is_occ = (ws.windows and ws.windows > 0)
  local s      = is_act and stl.workspace.active
                 or (is_occ and stl.workspace.occupied or stl.workspace.empty)
  local btn = wisp.widget.button {
    id        = "ws_" .. ws.id,
    text      = ws.name or tostring(ws.id),
    size      = F.body_sm,
    color     = s.color or C.fg_muted,
    bg        = s.bg or C.bg,
    width     = "fit",
    min_width = 24,
    height    = 20,
    padding   = { left = 7, right = 7, top = 1, bottom = 1 },
    radius    = s.radius or R,
    on_click  = function()
      ws_active = ws.id
      wisp.workspace.switch(ws.id)
      if wisp.widget("bar") then wisp.widget("bar"):redraw() end
    end,
  }
  btn._ws_id = ws.id
  btn._ws_windows = ws.windows or 0
  return btn
end

local function rebuild_workspace_buttons()
  if not ws_container then return end
  ws_buttons = {}
  local children = {}
  for i = 1, #ws_list do
    local ws = ws_list[i]
    local btn = make_workspace_button(ws)
    ws_buttons[ws.id] = btn
    table.insert(children, btn)
  end
  ws_container:set_children(children)
end

local function update_workspace_states()
  if not ws_container then return end
  for i = 1, #ws_list do
    local ws = ws_list[i]
    local btn = ws_buttons[ws.id]
    if btn then
      local is_act = (ws.id == ws_active)
      local is_occ = (ws.windows and ws.windows > 0)
      local s      = is_act and stl.workspace.active
                     or (is_occ and stl.workspace.occupied or stl.workspace.empty)
      btn:set_color(s.color or C.fg_muted)
      btn:set_bg(s.bg or C.bg)
    end
  end
end

-- ── Reactive update routines for specific widgets ────────────────────────────
local function update_clock()
  if not date_btn then return end
  local text = "󰸗 " .. os.date("%a, %b %d  %H:%M")
  date_btn:set_text(text)
end

local function update_memory()
  if not mem_label then return end
  local mem = (wisp.system and wisp.system.memory and wisp.system.memory()) or { percentage = 0 }
  local pct = math.floor(mem.percentage or 0)
  mem_label:set_text("󰍛 " .. pct .. "%")
  mem_label.tooltip = "Memory: " .. pct .. "%"
end

local function update_bluetooth()
  if not bt_btn then return end
  local btst = bt.get_status()
  local bt_on = btst.powered
  local icon = btst.icon or (bt_on and "󰂯" or "󰂲")
  local color = bt_on and C.accent or C.fg_muted
  local tt = "Bluetooth: " .. (bt_on and (btst.connected and "Connected" or "On") or "Off")
  bt_btn:set_text(icon)
  bt_btn:set_color(color)
  bt_btn:set_tooltip(tt)
end

local function update_network()
  if not wifi_btn then return end
  local netst = net.get_status()
  local net_on = netst.enabled and netst.connected
  local icon = netst.icon or "󰤨"
  local color = net_on and C.accent or C.fg_muted
  local tt = "Network: " .. (netst.ssid or (netst.enabled and "Disconnected" or "Disabled"))
  wifi_btn:set_text(icon)
  wifi_btn:set_color(color)
  wifi_btn:set_tooltip(tt)
end

local function update_audio()
  if not audio_btn then return end
  local vol = audio.get_volume()
  local muted = audio.is_muted()
  local vol_icon = audio.get_icon(vol, muted)
  local vol_text = muted and vol_icon or (vol_icon .. " " .. tostring(vol) .. "%")
  local color = muted and C.red or C.fg
  local tt = "Audio: " .. (muted and "Muted" or (tostring(vol) .. "%"))
  audio_btn:set_text(vol_text)
  audio_btn:set_color(color)
  audio_btn:set_tooltip(tt)
end

local function update_battery()
  if not batt_btn then return end
  local batt = bat.get_status()
  if not batt.present then
    batt_btn.visible = false
    return
  end
  batt_btn.visible = true
  local pct = math.floor(batt.percentage or 0)
  local batt_color = batt.charging and C.green or (pct < 20 and C.urgent or C.fg)
  local text = (batt.icon or "󰁹") .. " " .. pct .. "%"
  local tt = "Battery: " .. pct .. "% " .. (batt.charging and "(Charging)" or "")
  batt_btn:set_text(text)
  batt_btn:set_color(batt_color)
  batt_btn:set_tooltip(tt)
end

-- ── Widget definition ─────────────────────────────────────────────────────────
local widget = wisp.widget("bar", {
  height      = stl.bar.height,
  anchor      = "top_left_right",
  margin      = { top = 0, right = 0, bottom = 0, left = 0 },
  exclusive   = stl.bar.exclusive,
  transparent = false,
  visible     = true,
  interval    = 1000, -- 1 s refresh for clock and system metrics
})

-- On 1s tick: ONLY update the data labels in place without rebuilding the widget tree
widget:connect_signal("tick", function()
  update_clock()
  update_memory()
  update_battery()
  update_network()
  update_bluetooth()
end)

-- ── Event subscriptions ───────────────────────────────────────────────────────
wisp.workspace.subscribe(function(state)
  local list_changed = (#state.list ~= #ws_list)
  if not list_changed then
    for i = 1, #state.list do
      if state.list[i].id ~= ws_list[i].id or state.list[i].windows ~= ws_list[i].windows then
        list_changed = true
        break
      end
    end
  end
  ws_active = state.active
  ws_list   = state.list
  if list_changed then
    rebuild_workspace_buttons()
  else
    update_workspace_states()
  end
end)

if wisp.audio and type(wisp.audio.on_change) == "function" then
  wisp.audio.on_change(function() update_audio() end)
end

if wisp.battery and type(wisp.battery.on_change) == "function" then
  wisp.battery.on_change(function() update_battery() end)
end

if wisp.network and type(wisp.network.on_change) == "function" then
  wisp.network.on_change(function() update_network() end)
end

if net and type(net.subscribe) == "function" then
  net.subscribe(function() update_network() end)
end

-- ── Build ─────────────────────────────────────────────────────────────────────
widget:build(function()
  -- Status snapshots for initial layout
  local batt  = bat.get_status()
  local mem   = (wisp.system and wisp.system.memory and wisp.system.memory()) or { percentage = 0 }
  local netst = net.get_status()
  local btst  = bt.get_status()
  local vol   = audio.get_volume()
  local muted = audio.is_muted()

  -- ── 1. Left Section: launcher, workspaces ───────────────────────────────────
  local launcher_btn = wisp.widget.button {
    text     = "󰣇",
    size     = F.icon,
    color    = C.accent,
    bg       = C.bg,
    width    = "fit",
    height   = 20,
    padding  = { left = 6, right = 6, top = 1, bottom = 1 },
    radius   = R,
    tooltip  = "Applications (Super+Space)",
    on_click = function() panels.toggle("launcher") end,
  }

  ws_container = wisp.layout.fixed.horizontal {
    spacing = S.xs,
  }
  rebuild_workspace_buttons()

  local left_section = wisp.layout.fixed.horizontal {
    align   = { y = "center" },
    spacing = S.xs,
    launcher_btn,
    ws_container,
  }

  -- ── 2. Center Section: date ─────────────────────────────────────────────────
  local date_text = "󰸗 " .. os.date("%a, %b %d  %H:%M")
  date_btn = wisp.widget.button {
    text     = date_text,
    size     = F.body,
    color    = C.fg,
    bg       = C.bg,
    width    = "fit",
    height   = 20,
    padding  = { left = 10, right = 10, top = 1, bottom = 1 },
    radius   = R,
    tooltip  = "Calendar (Super+C)",
    on_click = function() panels.toggle("calendar") end,
  }

  -- ── 3. Right Section: tray, memory, bluetooth, wifi, audio, display, battery ─
  local tray_btn = ghost_btn("󰍜", C.fg_muted, nil, nil, "System Tray")

  local mem_pct = math.floor(mem.percentage or 0)
  mem_label = value_label("󰍛 " .. mem_pct .. "%", C.fg_muted, "Memory: " .. mem_pct .. "%")

  local bt_on = btst.powered
  bt_btn = ghost_btn(
    btst.icon or (bt_on and "󰂯" or "󰂲"),
    bt_on and C.accent or C.fg_muted,
    function() panels.toggle("bluetooth") end,
    nil,
    "Bluetooth: " .. (bt_on and (btst.connected and "Connected" or "On") or "Off")
  )

  local net_on = netst.enabled and netst.connected
  wifi_btn = ghost_btn(
    netst.icon or "󰤨",
    net_on and C.accent or C.fg_muted,
    function() panels.toggle("wifi") end,
    nil,
    "Network: " .. (netst.ssid or (netst.enabled and "Disconnected" or "Disabled"))
  )

  local vol_icon = audio.get_icon(vol, muted)
  local vol_text = muted and vol_icon or (vol_icon .. " " .. tostring(vol) .. "%")
  audio_btn = ghost_btn(
    vol_text,
    muted and C.red or C.fg,
    function() panels.toggle("audio") end,
    nil,
    "Audio: " .. (muted and "Muted" or (tostring(vol) .. "%"))
  )

  local disp_btn = ghost_btn("󰃠", C.fg_muted, function() panels.toggle("display") end, nil, "Display & Brightness")

  local pct = math.floor(batt.percentage or 0)
  local batt_color = batt.charging and C.green or (pct < 20 and C.urgent or C.fg)
  batt_btn = ghost_btn(
    (batt.icon or "󰁹") .. " " .. pct .. "%",
    batt_color,
    function() panels.toggle("battery") end,
    nil,
    "Battery: " .. pct .. "% " .. (batt.charging and "(Charging)" or "")
  )
  if not batt.present then batt_btn.visible = false end

  right_section = wisp.layout.fixed.horizontal {
    align   = { x = "right", y = "center" },
    spacing = S.xs,
    tray_btn,
    mem_label,
    bt_btn,
    wifi_btn,
    audio_btn,
    disp_btn,
    batt_btn,
  }

  -- ── Root align layout ───────────────────────────────────────────────────────
  return wisp.layout.align.horizontal {
    id      = "bar_root",
    bg      = C.bg,
    height  = stl.bar.height,
    width   = "fill",
    align   = { y = "center" },
    padding = { left = 8, right = 8, top = 0, bottom = 0 },
    first   = left_section,
    second  = date_btn,
    third   = right_section,
  }
end)

return widget:run()
