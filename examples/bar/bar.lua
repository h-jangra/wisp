-- Compositor-agnostic Wisp bar (Wayland shell-inspired)
-- Redesigned: true screen-centering for clock, truncated window title,
-- polished status cluster, event-driven updates, no manual signature cache.

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

local stl   = require("style")
local panels = require("lib.panels")
local bat   = require("services.battery")
local net   = require("services.network")
local bt    = require("services.bluetooth")
local audio = require("services.audio")

local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

-- ── Workspace state ──────────────────────────────────────────────────────────
local ws_active = wisp.workspace.active()
local ws_list   = wisp.workspace.list()

-- ── Utility ──────────────────────────────────────────────────────────────────
local function truncate(s, max_chars)
  s = s or ""
  if #s <= max_chars then return s end
  return s:sub(1, max_chars - 1) .. "…"
end

-- ── Widget definition ─────────────────────────────────────────────────────────
local widget = wisp.widget("bar", {
  height    = stl.bar.height,
  anchor    = "top_left_right",
  margin    = { top = 0, right = 0, bottom = 0, left = 0 },
  exclusive = stl.bar.exclusive,
  transparent = false,
  visible   = true,
  interval  = 1000,   -- 1 s keeps clock within 1 s of wall-clock; cheap since
                      -- the layout engine diffs and skips unchanged subtrees.
})

-- ── Event subscriptions ───────────────────────────────────────────────────────
wisp.workspace.subscribe(function(state)
  local changed = (state.active ~= ws_active) or (#state.list ~= #ws_list)
  if not changed then
    for i = 1, #state.list do
      local nw, ow = state.list[i], ws_list[i]
      if nw.id ~= ow.id or nw.windows ~= ow.windows then
        changed = true; break
      end
    end
  end
  ws_active = state.active
  ws_list   = state.list
  if changed then widget:redraw() end
end)

if wisp.audio and type(wisp.audio.on_change) == "function" then
  wisp.audio.on_change(function() widget:redraw() end)
end

if wisp.battery and type(wisp.battery.on_change) == "function" then
  wisp.battery.on_change(function() widget:redraw() end)
end

if wisp.network and type(wisp.network.on_change) == "function" then
  wisp.network.on_change(function() widget:redraw() end)
end

-- ── Sub-components ────────────────────────────────────────────────────────────

-- Ghost button: flat, borderless, vertically centred in bar.
local function ghost_btn(text, color, on_click, bg, tooltip)
  return wibox.widget.button {
    text    = text,
    size    = F.body,
    color   = color,
    bg      = bg or C.bg,
    width   = "fit",
    height  = 20,
    padding = { left = 5, right = 5, top = 1, bottom = 1 },
    radius  = R,
    tooltip = tooltip,
    on_click = on_click,
  }
end

-- Small value label (e.g. memory percentage).
local function value_label(text, color, tooltip)
  return wibox.widget.textbox {
    text    = text,
    size    = F.caption,
    color   = color or C.fg_muted,
    width   = "fit",
    height  = 20,
    padding = { left = 2, right = 2, top = 3, bottom = 3 },
    tooltip = tooltip,
  }
end

-- Vertical separator.
local function vsep()
  return wibox.widget.separator {
    color       = B.inactive,
    thickness   = 1,
    orientation = "vertical",
    height      = 14,
  }
end

-- Workspace buttons with minimum width guarantee.
local function workspace_buttons()
  local buttons = {}
  for i = 1, #ws_list do
    local ws    = ws_list[i]
    local is_act = ws.id == ws_active
    local is_occ = ws.windows > 0
    local s     = is_act and stl.workspace.active
                  or (is_occ and stl.workspace.occupied or stl.workspace.empty)
    table.insert(buttons, wibox.widget.button {
      id      = "ws_" .. ws.id,
      text    = ws.name or tostring(ws.id),
      size    = F.body_sm,
      color   = s.color or C.fg_muted,
      bg      = s.bg or C.bg,
      -- At least 24 px wide so single-digit names look balanced.
      width   = "fit",
      min_width = 24,
      height  = 20,
      padding = { left = 7, right = 7, top = 1, bottom = 1 },
      radius  = s.radius or R,
      on_click = function()
        ws_active = ws.id
        wisp.workspace.switch(ws.id)
        widget:redraw()
      end,
    })
  end
  return buttons
end

-- ── Build ─────────────────────────────────────────────────────────────────────
widget:build(function()
  local win_title  = truncate(wisp.workspace.active_window() or "Desktop", 32)
  local clock_text = os.date("%H:%M")

  -- Status snapshot.
  local batt  = bat.get_status()
  local mem   = (wisp.system and wisp.system.memory and wisp.system.memory()) or { percentage = 0 }
  local netst = net.get_status()
  local btst  = bt.get_status()
  local vol   = audio.get_volume()
  local muted = audio.is_muted()
  local svc   = (wisp.system and wisp.system.services and wisp.system.services()) or {}
  local notif = tonumber(svc.notify_active) or 0

  -- ── Status cluster ──────────────────────────────────────────────────────────
  local status_cluster = {}

  -- Clipboard toggle
  table.insert(status_cluster, ghost_btn("󰘚", C.fg_muted, function() panels.toggle("clipboard") end, nil, "Clipboard history"))

  -- Notifications: show count badge only when > 0
  local notif_text = notif > 0 and ("󰂚 " .. tostring(notif)) or "󰂚"
  table.insert(status_cluster, value_label(notif_text, notif > 0 and C.accent or C.fg_muted, "Notifications"))

  -- Battery (hidden on AC-only machines)
  if batt.present then
    local pct = math.floor(batt.percentage or 0) .. "%"
    table.insert(status_cluster, ghost_btn(
      batt.icon .. " " .. pct,
      batt.charging and C.green or C.fg,
      function() panels.toggle("battery") end,
      nil,
      "Battery status"
    ))
  end

  -- Memory
  table.insert(status_cluster, value_label("󰍛 " .. math.floor(mem.percentage or 0) .. "%", C.fg_muted, "Memory usage"))

  -- Network (icon only in bar; full detail in panel)
  local net_on = netst.enabled and netst.connected
  table.insert(status_cluster, ghost_btn(
    netst.icon or "󰤨",
    net_on and C.accent or C.fg_muted,
    function() panels.toggle("wifi") end,
    nil,
    "Network settings"
  ))

  -- Bluetooth
  local bt_on = btst.powered
  table.insert(status_cluster, ghost_btn(
    btst.icon or "󰂯",
    bt_on and C.accent or C.fg_muted,
    function() panels.toggle("bluetooth") end,
    nil,
    "Bluetooth settings"
  ))

  -- Audio: icon-only when muted, icon+percentage otherwise
  local vol_icon = audio.get_icon(vol, muted)
  local vol_text = muted and vol_icon or (vol_icon .. " " .. tostring(vol) .. "%")
  table.insert(status_cluster, ghost_btn(
    vol_text,
    muted and C.red or C.fg,
    function() panels.toggle("audio") end,
    nil,
    "Audio settings"
  ))

  -- Separator + display + control-center
  table.insert(status_cluster, vsep())
  table.insert(status_cluster, ghost_btn("󰃠", C.fg_muted, function() panels.toggle("display") end, nil, "Display & Brightness"))
  table.insert(status_cluster, ghost_btn("󰍜", C.accent, function() panels.toggle("control_center") end, C.bg_light, "Control Center"))

  -- ── Root layout ─────────────────────────────────────────────────────────────
  --
  -- AwesomeWM-style wisp.layout.align.horizontal:
  --   First slot:  Left section (launcher, workspaces, window title)
  --   Second slot: Center section (clock)
  --   Third slot:  Right section (status cluster)

  local left_section = wibox.layout.fixed.horizontal {
    align  = { y = "center" },
    spacing = S.xs,

    -- Launcher (distro/Arch icon, slightly larger)
    wibox.widget.button {
      text     = "󰣇",
      size     = F.icon,       -- 16 px — more prominent than body
      color    = C.accent,
      bg       = C.bg,
      width    = "fit",
      height   = 20,
      padding  = { left = 5, right = 5, top = 1, bottom = 1 },
      radius   = R,
      tooltip  = "Application Launcher",
      on_click = function() panels.toggle("launcher") end,
    },

    -- Workspace buttons
    table.unpack(workspace_buttons()),

    -- Separator
    vsep(),

    -- Window title (truncated to 32 chars, width capped to prevent overflow)
    wibox.widget.textbox {
      text   = win_title,
      size   = F.caption,
      color  = C.fg_muted,
      width  = "fit",
    },
  }

  local center_section = wibox.widget.button {
    text     = "󰸗 " .. clock_text,
    size     = F.body,         -- body (11 px) — slightly more prominent
    color    = C.fg,
    bg       = C.bg,
    width    = "fit",
    height   = 20,
    padding  = { left = 10, right = 10, top = 1, bottom = 1 },
    radius   = R,
    tooltip  = "Calendar & Clock",
    on_click = function() panels.toggle("calendar") end,
  }

  local right_section = wibox.layout.fixed.horizontal {
    align  = { x = "right", y = "center" },
    spacing = S.xs,
    table.unpack(status_cluster),
  }

  return wibox.layout.align.horizontal {
    id      = "bar_root",
    bg      = C.bg,
    height  = stl.bar.height,
    width   = "fill",
    align   = { y = "center" },
    padding = { left = 8, right = 8, top = 0, bottom = 0 },
    first   = left_section,
    second  = center_section,
    third   = right_section,
  }
end)

return widget:run()
