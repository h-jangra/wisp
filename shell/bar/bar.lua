-- Compositor-agnostic Wisp bar (Wayland shell-inspired)

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

local C = stl.color
local S = stl.space
local F = stl.font
local T = stl  -- alias
local R = stl.radius
local B = stl.border

-- Workspace state: cached from backend, updated by subscription (event-driven, no polling)
local ws_active = wisp.workspace.active()
local ws_list = wisp.workspace.list()

local function refresh_state()
  local nxt = wisp.workspace.list()
  local changed = #nxt ~= #ws_list
  if not changed then
    for i = 1, #nxt do
      if nxt[i].id ~= ws_list[i].id or nxt[i].windows ~= ws_list[i].windows then
        changed = true
        break
      end
    end
  end
  ws_list = nxt
  local act = wisp.workspace.active()
  if act ~= ws_active then ws_active = act end
  return changed
end

local function workspace_buttons()
  local buttons = {}
  for i = 1, #ws_list do
    local ws = ws_list[i]
    local is_act = ws.id == ws_active
    local is_occ = ws.windows > 0
    local s = is_act and stl.workspace.active or (is_occ and stl.workspace.occupied or stl.workspace.empty)
    local bg = s.bg or C.bg
    local fg = s.color or C.fg_muted
    local radius = s.radius or R
    table.insert(buttons, wisp.ui.button {
      id = "ws_" .. ws.id,
      text = ws.name or tostring(ws.id),
      size = F.body_sm,
      color = fg,
      bg = bg,
      width = "fit",
      height = F.body_sm + 6,
      padding = { left = 8, right = 8, top = 1, bottom = 1 },
      radius = radius,
      style = s,
      on_click = function()
        ws_active = ws.id
        refresh_state()
        wisp.workspace.switch(ws.id)
        widget:redraw()
      end,
    })
  end
  return buttons
end

local widget = wisp.widget("bar", {
  height = stl.bar.height,
  anchor = "top_left_right",
  margin = { top = 0, right = 0, bottom = 0, left = 0 },
  exclusive = T.bar.exclusive,
  transparent = false,
  visible = true,
  interval = 2000,
})

wisp.workspace.subscribe(function(state)
  local changed = false
  if state.active ~= ws_active then
    ws_active = state.active
    changed = true
  end
  if #state.list ~= #ws_list then
    changed = true
  else
    for i = 1, #state.list do
      local nw = state.list[i]
      local ow = ws_list[i]
      if nw.id ~= ow.id or nw.windows ~= ow.windows then
        changed = true
        break
      end
    end
  end
  ws_list = state.list
  if changed then widget:redraw() end
end)

widget:build(function()
  local win_title = wisp.workspace.active_window()
  local clock_text = os.date("%a %b %d  %I:%M %p")

  return wisp.ui.row {
    bg = C.bg,
    height = stl.bar.height,
    align = { y = "center" },
    padding = { left = 10, right = 10, top = 0, bottom = 0 },
    gap = S.sm,

    -- Left: launcher + workspaces + active window
    wisp.ui.row {
      width = "fit",
      align = { y = "center" },
      gap = S.xs,
      wisp.ui.button {
        text = "󰣇",
        size = F.title,
        color = C.accent,
        bg = C.bg,
        width = "fit",
        height = 20,
        padding = { left = 4, right = 4, top = 1, bottom = 1 },
        radius = R,
        on_click = function() panels.toggle("launcher") end,
      },
      table.unpack(workspace_buttons()),
      wisp.ui.separator { color = B.inactive, thickness = 1, vertical = true, height = 14 },
      wisp.ui.label {
        text = win_title or "Desktop",
        size = F.caption,
        color = C.fg_muted,
        width = "fit",
      },
    },

    -- Center: clock
    wisp.ui.spacer { flex = 1 },
    wisp.ui.button {
      text = "󰸗 " .. clock_text,
      size = F.body_sm,
      color = C.fg,
      bg = C.bg,
      width = "fit",
      height = 20,
      padding = { left = 8, right = 8, top = 1, bottom = 1 },
      radius = R,
      on_click = function() panels.toggle("calendar") end,
    },
    wisp.ui.spacer { flex = 1 },

    -- Right: status icons
    wisp.ui.row {
      width = "fit",
      align = { y = "center" },
      gap = S.xs,
      wisp.ui.button {
        text = "󰘚", size = F.body, color = C.fg_muted, bg = C.bg,
        width = "fit", height = 20, padding = { left = 4, right = 4, top = 1, bottom = 1 },
        radius = R,
        on_click = function() panels.toggle("clipboard") end,
      },
      wisp.ui.button {
        text = "󰤨", size = F.body, color = C.accent, bg = C.bg,
        width = "fit", height = 20, padding = { left = 4, right = 4, top = 1, bottom = 1 },
        radius = R,
        on_click = function() panels.toggle("wifi") end,
      },
      wisp.ui.button {
        text = "󰂯", size = F.body, color = C.fg_muted, bg = C.bg,
        width = "fit", height = 20, padding = { left = 4, right = 4, top = 1, bottom = 1 },
        radius = R,
        on_click = function() panels.toggle("audio") end,
      },
      wisp.ui.button {
        text = "󰃠", size = F.caption, color = C.fg, bg = C.bg,
        width = "fit", height = 20, padding = { left = 4, right = 4, top = 1, bottom = 1 },
        radius = R,
        on_click = function() panels.toggle("display") end,
      },
      wisp.ui.button {
        text = "󰍜", size = F.title, color = C.accent, bg = C.bg_light,
        width = "fit", height = 20, padding = { left = 6, right = 6, top = 1, bottom = 1 },
        radius = R,
        on_click = function() panels.toggle("control_center") end,
      },
    },
  }
end)

return widget:run()
