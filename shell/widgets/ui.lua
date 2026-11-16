-- Shared UI Components for Wisp Shell & System Panels
-- Built using AwesomeWM-inspired wibox widget/layout/container architecture.
-- Fully reactive, composable, and compiled to native Clay elements.

local function _ensure_style_on_path()
  local ok = pcall(require, "style")
  if ok then return end
  local home = os.getenv("HOME") or ""
  local cands = { "shell", "./shell", home .. "/.config/wisp/shell", "/home/hj/Projects/wisp/shell", "/usr/share/wisp/shell" }
  for _, d in ipairs(cands) do
    local f = io.open(d .. "/style.lua", "r")
    if f then
      f:close()
      package.path = d .. "/?.lua;" .. d .. "/?/init.lua;" .. package.path
      return
    end
  end
end
_ensure_style_on_path()

local stl = require("style")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius
local B = stl.border

local ui = {}
setmetatable(ui, { __index = wisp.ui })

function ui.PanelHeader(title, subtitle, on_close, icon)
  local left = wibox.layout.fixed.horizontal {
    spacing = S.sm,
  }
  if icon then
    left:add(wibox.widget.textbox { text = icon, size = F.heading, color = stl.color.accent })
  end
  local title_col = wibox.layout.fixed.vertical {
    spacing = 2,
    wibox.widget.textbox { text = title, size = F.title, color = C.fg },
  }
  if subtitle then
    title_col:add(wibox.widget.textbox { text = subtitle, size = F.caption, color = C.fg_muted })
  end
  left:add(title_col)

  local right = nil
  if on_close then
    right = wibox.widget.button {
      text = "✕",
      size = F.body_sm,
      color = C.fg_muted,
      bg = C.bg_light,
      radius = R,
      padding = 4,
      on_click = on_close,
    }
  end

  return wibox.layout.align.horizontal(left, nil, right)
end

function ui.Card(opts, ...)
  opts = opts or {}
  local bg = opts.bg or opts.background or C.bg_dark
  local border_color = opts.border_color or B.inactive
  local radius = opts.radius or R
  local pad = opts.padding or S.md
  local gap = opts.gap or S.sm

  local card = wibox.container.background {
    bg = bg,
    border = { color = border_color, width = B.width },
    radius = radius,
    padding = pad,
    gap = gap,
    on_click = opts.on_click,
    width = opts.width,
    height = opts.height,
    flex = opts.flex,
    align = opts.align,
    justify = opts.justify,
    direction = opts.direction,
    clip = opts.clip,
  }

  local n = select("#", ...)
  if n > 0 then
    for i = 1, n do
      local child = select(i, ...)
      if child ~= nil then card:add(child) end
    end
  else
    for _, k in ipairs(opts) do
      if type(k) == "table" then card:add(k) end
    end
  end

  return card
end

function ui.Button(opts)
  opts = opts or {}
  local active = opts.active == true
  local bg = opts.bg or (active and C.accent or C.bg_light)
  local fg = opts.color or (active and C.bg_darker or C.fg)
  local border_col = opts.border_color or (active and C.accent or B.inactive)

  return wibox.widget.button {
    text = opts.text or opts.label or "Button",
    size = opts.size or F.body,
    color = fg,
    bg = bg,
    border = { color = border_col, width = B.width },
    radius = opts.radius or R,
    padding = opts.padding or S.control_y,
    on_click = opts.on_click,
    width = opts.width,
    height = opts.height,
    flex = opts.flex,
    tooltip = opts.tooltip,
    tooltip_placement = opts.tooltip_placement or opts.placement,
  }
end

function ui.IconButton(icon, on_click, opts)
  opts = opts or {}
  local active = opts.active == true
  local bg = opts.bg or (active and C.accent or C.bg_light)
  local fg = opts.color or (active and C.bg_darker or C.fg)

  return wibox.widget.button {
    text = icon,
    size = opts.size or F.title,
    color = fg,
    bg = bg,
    border = { color = active and C.accent or B.inactive, width = 1 },
    radius = R,
    padding = opts.padding or 6,
    on_click = on_click,
    tooltip = opts.tooltip,
    tooltip_placement = opts.tooltip_placement or opts.placement,
  }
end

function ui.ToggleRow(label, icon, active, on_toggle, subtitle)
  local left = wibox.layout.fixed.horizontal {
    spacing = S.md,
  }
  if icon then
    left:add(wibox.widget.textbox { text = icon, size = F.heading, color = active and C.accent or C.fg_muted })
  end
  local col = wibox.layout.fixed.vertical {
    spacing = 2,
    wibox.widget.textbox { text = label, size = F.body, color = active and C.fg or C.fg_muted },
  }
  if subtitle then
    col:add(wibox.widget.textbox { text = subtitle, size = F.caption, color = active and C.accent or C.fg_muted })
  end
  left:add(col)

  local right = wibox.widget.checkbox {
    checked = active,
    active_color = C.accent,
    inactive_color = C.muted,
    on_change = on_toggle,
  }

  local inner = wibox.layout.align.horizontal(left, nil, right)
  return ui.Card({
    bg = active and C.bg_light or C.bg_dark,
    border_color = active and C.accent or B.inactive,
    padding = S.md,
    width = "fill",
    on_click = on_toggle,
  }, inner)
end

function ui.SliderRow(label, icon, value, min_val, max_val, on_change, unit)
  unit = unit or "%"
  local val_int = math.floor(value or 0)
  local header_left = wibox.layout.fixed.horizontal {
    spacing = S.xs,
  }
  if icon then
    header_left:add(wibox.widget.textbox { text = icon, size = F.body, color = C.accent })
  end
  header_left:add(wibox.widget.textbox { text = label, size = F.body, color = C.fg })

  local header_right = wibox.widget.textbox { text = tostring(val_int) .. unit, size = F.body_sm, color = C.accent }
  local header = wibox.layout.align.horizontal(header_left, nil, header_right)

  local slider = wibox.widget.slider {
    value = value,
    minimum = min_val or 0,
    maximum = max_val or 100,
    bar_color = C.bg_light,
    fill_color = C.accent,
    handle_color = C.fg,
    on_change = on_change,
  }

  return ui.Card({
    bg = C.bg_dark,
    padding = S.md,
  }, wibox.layout.fixed.vertical {
    spacing = S.xs,
    header,
    slider,
  })
end

function ui.SearchInput(id, value, placeholder, on_change, on_submit)
  return wibox.widget.input {
    id = id,
    value = value or "",
    placeholder = placeholder or "Search...",
    width = "fill",
    height = S.control_h or 36,
    radius = R or 8,
    font_size = F.body_sm or 13,
    padding_x = 12,
    padding_y = 8,
    background = C.bg_dark,
    border_color = B.inactive,
    focus_color = C.accent,
    hover_color = C.fg_muted,
    text_color = C.fg,
    placeholder_color = C.fg_muted,
    on_change = on_change,
    on_submit = on_submit,
  }
end

function ui.TabNav(tabs, active_idx, on_select)
  local row = wibox.layout.fixed.horizontal {
    spacing = S.xs,
  }
  for i, tab in ipairs(tabs) do
    local is_active = (active_idx == i)
    row:add(wibox.widget.button {
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
  return row
end

function ui.Chip(text, opts)
  opts = opts or {}
  local bg = opts.bg or C.bg_light
  local fg = opts.color or C.fg
  return wibox.container.background {
    bg = bg,
    border = { color = opts.border or B.inactive, width = 1 },
    radius = R,
    padding = { left = 6, right = 6, top = 2, bottom = 2 },
    widget = wibox.widget.textbox { text = text, size = opts.size or F.caption, color = fg },
  }
end

function ui.Separator(vertical)
  return wibox.widget.separator {
    color = B.inactive,
    thickness = 1,
    orientation = vertical and "vertical" or "horizontal",
  }
end

function ui.EmptyState(icon, text)
  local col = wibox.layout.fixed.vertical {
    spacing = S.sm,
    padding = 24,
    align = { x = "center", y = "center" },
  }
  if icon then
    col:add(wibox.widget.textbox { text = icon, size = F.display, color = C.fg_muted })
  end
  col:add(wibox.widget.textbox { text = text or "No items found", size = F.body_sm, color = C.fg_muted })
  return col
end

function ui.SectionTitle(title, action_text, on_action)
  local left = wibox.widget.textbox { text = string.upper(title), size = F.caption, color = C.fg_muted }
  local right = nil
  if action_text and on_action then
    right = wibox.widget.button {
      text = action_text,
      size = F.caption,
      color = C.accent,
      bg = C.bg,
      padding = 2,
      on_click = on_action,
    }
  end
  return wibox.layout.align.horizontal(left, nil, right)
end

function ui.MetricRow(icon, label, value_text, percentage, color)
  color = color or C.accent
  local left = wibox.layout.fixed.horizontal {
    spacing = S.xs,
  }
  if icon then
    left:add(wibox.widget.textbox { text = icon, size = F.body, color = color })
  end
  left:add(wibox.widget.textbox { text = label, size = F.body_sm, color = C.fg })

  local right = wibox.widget.textbox { text = value_text, size = F.caption, color = color }
  local header = wibox.layout.align.horizontal(left, nil, right)

  local col = wibox.layout.fixed.vertical {
    spacing = 4,
    width = "fill",
    header,
  }
  if percentage ~= nil then
    local pct = math.max(0, math.min(100, tonumber(percentage) or 0))
    col:add(wibox.widget.progressbar {
      value = pct,
      max_value = 100,
      height = 5,
      radius = 0,
      color = color,
      background_color = C.bg_light,
    })
  end
  return col
end

function ui.StatCard(icon, value, label, color, on_click)
  color = color or C.accent
  local col = wibox.layout.fixed.vertical {
    spacing = 2,
    align = { x = "center" },
  }
  if icon then
    col:add(wibox.widget.textbox { text = icon, size = F.heading, color = color })
  end
  col:add(wibox.widget.textbox { text = tostring(value), size = F.title, color = C.fg })
  if label then
    col:add(wibox.widget.textbox { text = label, size = F.caption, color = C.fg_muted })
  end

  return ui.Card({
    bg = C.bg_dark,
    border_color = B.inactive,
    padding = S.md,
    on_click = on_click,
    width = "fill",
  }, col)
end

function ui.Badge(text, color, bg)
  return ui.Chip(text, { color = color or C.fg, bg = bg or C.bg_light })
end

function ui.Divider(label)
  if not label then
    return ui.Separator(false)
  end
  return wibox.layout.fixed.horizontal {
    spacing = S.sm,
    width = "fill",
    align = { y = "center" },
    wibox.widget.separator { color = B.inactive, thickness = 1, flex = 1 },
    wibox.widget.textbox { text = tostring(label), size = F.caption, color = C.fg_muted },
    wibox.widget.separator { color = B.inactive, thickness = 1, flex = 1 },
  }
end

function ui.InfoRow(label, value, icon)
  local left = wibox.layout.fixed.horizontal {
    spacing = S.xs,
  }
  if icon then
    left:add(wibox.widget.textbox { text = icon, size = F.body_sm, color = C.accent })
  end
  left:add(wibox.widget.textbox { text = label, size = F.body_sm, color = C.fg_muted })

  local right = wibox.widget.textbox { text = tostring(value), size = F.body_sm, color = C.fg }
  return wibox.layout.align.horizontal(left, nil, right)
end

function ui.ActionRow(icon, title, subtitle, right_widget, on_click)
  local left = wibox.layout.fixed.horizontal {
    spacing = S.sm,
  }
  if icon then
    left:add(wibox.widget.textbox { text = icon, size = F.heading, color = C.accent })
  end
  local col = wibox.layout.fixed.vertical {
    spacing = 2,
    wibox.widget.textbox { text = title, size = F.body_sm, color = C.fg },
  }
  if subtitle then
    col:add(wibox.widget.textbox { text = subtitle, size = F.caption, color = C.fg_muted })
  end
  left:add(col)

  local inner = wibox.layout.align.horizontal(left, nil, right_widget)
  return ui.Card({
    bg = C.bg_dark,
    border_color = B.inactive,
    padding = S.md,
    width = "fill",
    on_click = on_click,
  }, inner)
end

function ui.ProgressBar(value, max_val, color, bg, height)
  return wibox.widget.progressbar {
    value = value or 0,
    max_value = max_val or 100,
    height = height or 6,
    radius = 0,
    color = color or C.accent,
    background_color = bg or C.bg_light,
  }
end

function ui.state(initial)
  if wisp and wisp.state then
    return wisp.state(initial)
  end
  local val = initial
  local subs = {}
  return {
    get = function() return val end,
    set = function(self, v)
      val = v
      for _, cb in ipairs(subs) do pcall(cb, v) end
    end,
    subscribe = function(self, cb)
      table.insert(subs, cb)
      return function()
        for i, f in ipairs(subs) do
          if f == cb then table.remove(subs, i); break end
        end
      end
    end
  }
end

return ui
