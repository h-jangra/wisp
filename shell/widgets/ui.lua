-- Shared UI Components for Wisp Shell & System Panels
-- Built only from stl (shell/style.lua); no hardcoded styling.

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
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local ui = {}

function ui.PanelHeader(title, subtitle, on_close, icon)
  local children = {}
  if icon then
    table.insert(children, wisp.ui.label { text = icon, size = F.heading, color = stl.color.accent })
    table.insert(children, wisp.ui.spacer { width = S.sm })
  end
  table.insert(children, wisp.ui.col {
    gap = 2,
    wisp.ui.label { text = title, size = F.title, color = C.fg },
    subtitle and wisp.ui.label { text = subtitle, size = F.caption, color = C.fg_muted } or nil,
  })
  table.insert(children, wisp.ui.spacer { flex = 1 })
  if on_close then
    table.insert(children, wisp.ui.button {
      text = "✕",
      size = F.body_sm,
      color = C.fg_muted,
      bg = C.bg_light,
      radius = R,
      padding = 4,
      on_click = on_close,
    })
  end
  return wisp.ui.row {
    align = { y = "center" },
    gap = S.sm,
    padding = 0,
    table.unpack(children)
  }
end

function ui.Card(opts, ...)
  opts = opts or {}
  local bg = opts.bg or opts.background or C.bg_dark
  local border_color = opts.border_color or B.inactive
  local radius = opts.radius or R
  local pad = opts.padding or S.md
  local gap = opts.gap or S.sm

  local card_opts = {
    bg = bg,
    border = { color = border_color, width = B.width },
    radius = radius,
    padding = pad,
    gap = gap,
    on_click = opts.on_click,
  }

  local n = select("#", ...)
  if n > 0 then
    for i = 1, n do
      local child = select(i, ...)
      if child ~= nil then table.insert(card_opts, child) end
    end
  else
    for _, k in ipairs(opts) do
      table.insert(card_opts, k)
    end
  end

  return wisp.ui.card(card_opts)
end

function ui.Button(opts)
  opts = opts or {}
  local active = opts.active == true
  local bg = opts.bg or (active and C.accent or C.bg_light)
  local fg = opts.color or (active and C.bg_darker or C.fg)
  local border_col = opts.border_color or (active and C.accent or B.inactive)

  return wisp.ui.button {
    text = opts.text or "Button",
    size = opts.size or F.body,
    color = fg,
    bg = bg,
    border = { color = border_col, width = B.width },
    radius = opts.radius or R,
    padding = opts.padding or S.control_y,
    on_click = opts.on_click,
    width = opts.width,
  }
end

function ui.IconButton(icon, on_click, opts)
  opts = opts or {}
  local active = opts.active == true
  local bg = opts.bg or (active and C.accent or C.bg_light)
  local fg = opts.color or (active and C.bg_darker or C.fg)

  return wisp.ui.button {
    text = icon,
    size = opts.size or F.title,
    color = fg,
    bg = bg,
    border = { color = active and C.accent or B.inactive, width = 1 },
    radius = R,
    padding = opts.padding or 6,
    on_click = on_click,
  }
end

function ui.ToggleRow(label, icon, active, on_toggle, subtitle)
  return ui.Card({
    bg = active and C.bg_light or C.bg_dark,
    border_color = active and C.accent or B.inactive,
    padding = S.md,
    on_click = on_toggle,
  }, wisp.ui.row {
    align = { y = "center" },
    gap = S.md,
    icon and wisp.ui.label { text = icon, size = F.heading, color = active and C.accent or C.fg_muted } or nil,
    wisp.ui.col {
      gap = 2,
      wisp.ui.label { text = label, size = F.body, color = active and C.fg or C.fg_muted },
      subtitle and wisp.ui.label { text = subtitle, size = F.caption, color = active and C.accent or C.fg_muted } or nil,
    },
    wisp.ui.spacer { flex = 1 },
    wisp.ui.switch {
      checked = active,
      active_color = C.accent,
      inactive_color = C.muted,
      on_change = on_toggle,
    }
  })
end

function ui.SliderRow(label, icon, value, min_val, max_val, on_change, unit)
  unit = unit or "%"
  local val_int = math.floor(value or 0)
  return ui.Card({
    bg = C.bg_dark,
    padding = S.md,
  }, wisp.ui.col {
    gap = S.xs,
    wisp.ui.row {
      align = { y = "center" },
      icon and wisp.ui.label { text = icon, size = F.body, color = C.accent } or nil,
      wisp.ui.spacer { width = S.xs },
      wisp.ui.label { text = label, size = F.body, color = C.fg },
      wisp.ui.spacer { flex = 1 },
      wisp.ui.label { text = tostring(val_int) .. unit, size = F.body_sm, color = C.accent },
    },
    wisp.ui.slider {
      value = value,
      min = min_val or 0,
      max = max_val or 100,
      track_color = C.bg_light,
      fill_color = C.accent,
      thumb_color = C.fg,
      on_change = on_change,
    }
  })
end

function ui.SearchInput(id, value, placeholder, on_change, on_submit)
  return wisp.ui.input {
    id = id,
    value = value or "",
    placeholder = placeholder or "Search...",
    background = C.bg_dark,
    border_color = B.inactive,
    focus_color = C.accent,
    text_color = C.fg,
    placeholder_color = C.fg_muted,
    radius = R,
    height = S.control_h,
    on_change = on_change,
    on_submit = on_submit,
  }
end

function ui.TabNav(tabs, active_idx, on_select)
  local buttons = {}
  for i, tab in ipairs(tabs) do
    local is_active = (active_idx == i)
    table.insert(buttons, wisp.ui.button {
      text = tab,
      size = F.caption,
      color = is_active and C.bg_darker or C.fg,
      bg = is_active and C.accent or C.bg_dark,
      border = { color = is_active and C.accent or B.inactive, width = 1 },
      radius = R,
      padding = 5,
      on_click = function() on_select(i) end,
    })
  end
  return wisp.ui.row {
    gap = S.xs,
    table.unpack(buttons)
  }
end

function ui.Chip(text, opts)
  opts = opts or {}
  local bg = opts.bg or C.bg_light
  local fg = opts.color or C.fg
  return wisp.ui.row {
    padding = { left = 6, right = 6, top = 2, bottom = 2 },
    bg = bg,
    border = { color = opts.border or B.inactive, width = 1 },
    radius = R,
    wisp.ui.label { text = text, size = opts.size or F.caption, color = fg }
  }
end

function ui.Separator(vertical)
  return wisp.ui.separator {
    color = B.inactive,
    thickness = 1,
    vertical = vertical or false,
  }
end

function ui.EmptyState(icon, text)
  return wisp.ui.col {
    align = { x = "center", y = "center" },
    padding = 24,
    gap = S.sm,
    icon and wisp.ui.label { text = icon, size = F.display, color = C.fg_muted } or nil,
    wisp.ui.label { text = text or "No items found", size = F.body_sm, color = C.fg_muted },
  }
end

function ui.SectionTitle(title, action_text, on_action)
  local row = {
    align = { y = "center" },
    wisp.ui.label { text = string.upper(title), size = F.caption, color = C.fg_muted },
    wisp.ui.spacer { flex = 1 },
  }
  if action_text and on_action then
    table.insert(row, wisp.ui.button {
      text = action_text,
      size = F.caption,
      color = C.accent,
      bg = C.bg,
      padding = 2,
      on_click = on_action,
    })
  end
  return wisp.ui.row(row)
end

return ui
