-- Shared UI Components for Wisp Shell & System Panels
-- Swiss Technical Interface Architecture: Proofline editorial hierarchy, Oxide precision, Swiss Moss consistency.
-- Built on wisp.layout, wisp.container, and wisp.widget with native Clay compilation.

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

local ui = (wisp and wisp.ui) or {}

-- ── 1. Hairline Rule & Divider ───────────────────────────────────────────────
function ui.Divider(label_or_vert)
  if type(label_or_vert) == "string" then
    return wisp.layout.fixed.horizontal {
      spacing = S.sm,
      width = "fill",
      align = { y = "center" },
      wisp.widget.separator { color = stl.stroke and stl.stroke.hairline or B.inactive, thickness = 1, flex = 1 },
      wisp.widget.textbox { text = tostring(label_or_vert), size = F.caption, color = C.fg_muted },
      wisp.widget.separator { color = stl.stroke and stl.stroke.hairline or B.inactive, thickness = 1, flex = 1 },
    }
  end
  return wisp.widget.separator {
    color = stl.stroke and stl.stroke.hairline or B.inactive,
    thickness = 1,
    orientation = (label_or_vert == true) and "vertical" or "horizontal",
  }
end
ui.Separator = ui.Divider

-- ── 2. Swiss Panel Header ────────────────────────────────────────────────────
function ui.PanelHeader(title, subtitle, on_close, icon)
  if type(title) == "table" then
    local opts = title
    title = opts.title
    subtitle = opts.subtitle or opts.detail
    on_close = opts.on_close or opts.onClose
    icon = opts.icon
  end

  local left = wisp.layout.fixed.horizontal {
    spacing = S.sm,
    align = { y = "center" },
  }
  local icon_box = nil
  if icon then
    icon_box = wisp.widget.textbox {
      id = "header_icon",
      text = icon,
      size = F.heading,
      color = C.accent,
      width = "fit",
    }
    left:add(icon_box)
  end

  local title_box = wisp.widget.textbox {
    id = "header_title",
    text = title or "Panel",
    size = F.title,
    color = C.fg,
  }
  local sub_box = wisp.widget.textbox {
    id = "header_subtitle",
    text = subtitle or "",
    size = F.caption,
    color = C.fg_muted,
  }
  local title_col = wisp.layout.fixed.vertical {
    spacing = 1,
    title_box,
    sub_box,
  }
  left:add(title_col)

  local right = nil
  if on_close then
    right = wisp.widget.button {
      text = "✕",
      size = F.body_sm,
      color = C.fg_muted,
      bg = stl.surface and stl.surface.sunken or C.bg_darker,
      border = { color = stl.stroke and stl.stroke.hairline or B.inactive, width = 1 },
      radius = tokens and tokens.radius_sm or 2,
      padding = { left = 6, right = 6, top = 2, bottom = 2 },
      on_click = on_close,
      tooltip = "Close",
    }
  end

  local header_bar = wisp.layout.align.horizontal(left, nil, right)
  header_bar.align = { y = "center" }

  header_bar.title_box = title_box
  header_bar.subtitle_box = sub_box
  header_bar.icon_box = icon_box
  header_bar.close_btn = right

  function header_bar:set_title(t) title_box:set_text(t) end
  function header_bar:set_subtitle(s) sub_box:set_text(s) end
  function header_bar:set_icon(ic) if icon_box then icon_box:set_text(ic) end end

  return header_bar
end
ui.Header = ui.PanelHeader

-- ── 3. Swiss Editorial Row ───────────────────────────────────────────────────
function ui.Row(opts)
  opts = opts or {}
  local icon = opts.icon
  local title = opts.title or opts.label or ""
  local subtitle = opts.subtitle or opts.detail or ""
  local active = opts.active == true
  local on_click = opts.on_click
  local action = opts.action or opts.right

  local icon_box = nil
  if icon then
    icon_box = wisp.widget.textbox {
      text = icon,
      size = opts.icon_size or F.heading,
      color = opts.icon_color or (active and C.accent or C.fg_muted),
      width = "fit",
    }
  end

  local title_box = wisp.widget.textbox {
    text = title,
    size = opts.title_size or F.body,
    color = opts.title_color or (active and C.accent or C.fg),
    wrap = true,
  }

  local sub_box = nil
  if subtitle and subtitle ~= "" then
    sub_box = wisp.widget.textbox {
      text = subtitle,
      size = F.caption,
      color = C.fg_muted,
      wrap = true,
    }
  end

  local text_col = wisp.layout.fixed.vertical {
    spacing = 1,
    flex = 1,
    title_box,
  }
  if sub_box then text_col:add(sub_box) end

  local left = wisp.layout.fixed.horizontal {
    spacing = S.sm,
    align = { y = "center" },
  }
  if icon_box then left:add(icon_box) end
  left:add(text_col)

  local right_elem = nil
  if action then
    if type(action) == "string" then
      right_elem = wisp.widget.textbox { text = action, size = F.caption, color = C.fg_muted }
    elseif type(action) == "table" then
      right_elem = action
    end
  end

  local inner = wisp.layout.align.horizontal(left, nil, right_elem)
  inner.align = { y = "center" }

  local row_card = wisp.container.background {
    bg = active and (stl.surface and stl.surface.active or C.bg_light) or (stl.surface and stl.surface.raised or C.bg_dark),
    border = {
      color = active and (stl.stroke and stl.stroke.focus or C.accent) or (stl.stroke and stl.stroke.hairline or B.inactive),
      width = 1,
    },
    radius = tokens and tokens.radius_sm or 2,
    padding = opts.padding or { left = 8, right = 8, top = 6, bottom = 6 },
    on_click = on_click,
    width = "fill",
  }
  row_card:add(inner)

  row_card.icon_box = icon_box
  row_card.title_box = title_box
  row_card.sub_box = sub_box
  row_card.right_elem = right_elem

  function row_card:set_title(t, color)
    title_box:set_text(t)
    if color then title_box:set_color(color) end
  end

  function row_card:set_subtitle(s, color)
    if sub_box then
      sub_box:set_text(s)
      if color then sub_box:set_color(color) end
    end
  end

  function row_card:set_icon(ic, color)
    if icon_box then
      if ic then icon_box:set_text(ic) end
      if color then icon_box:set_color(color) end
    end
  end

  function row_card:set_active(is_act)
    local act_bg = stl.surface and stl.surface.active or C.bg_light
    local inact_bg = stl.surface and stl.surface.raised or C.bg_dark
    local act_bdr = stl.stroke and stl.stroke.focus or C.accent
    local inact_bdr = stl.stroke and stl.stroke.hairline or B.inactive
    row_card.bg = is_act and act_bg or inact_bg
    row_card.border = { color = is_act and act_bdr or inact_bdr, width = 1 }
    row_card:emit_signal("widget::redraw_needed")
    if icon_box then icon_box:set_color(is_act and C.accent or C.fg_muted) end
    title_box:set_color(is_act and C.accent or C.fg)
  end

  return row_card
end

-- ── 4. Swiss Card / Surface Well ─────────────────────────────────────────────
function ui.Card(opts, ...)
  opts = opts or {}
  local bg = opts.bg or opts.background or (stl.surface and stl.surface.raised or C.bg_dark)
  local border_color = opts.border_color or (stl.stroke and stl.stroke.hairline or B.inactive)
  local radius = opts.radius or (tokens and tokens.radius_sm or R or 2)
  local pad = opts.padding or S.md
  local gap = opts.gap or S.sm

  local card = wisp.container.background {
    bg = bg,
    border = { color = border_color, width = B.width or 1 },
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
  card.type = "card"
  card.width = opts.width

  function card:set_bg(b) card.bg = b; card:emit_signal('property::bg', b); card:emit_signal('widget::redraw_needed') end
  function card:set_border_color(c) card.border = { color = c, width = B.width or 1 }; card:emit_signal('widget::redraw_needed') end
  function card:set_on_click(fn) card.on_click = fn end

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

-- ── 5. Swiss Button ──────────────────────────────────────────────────────────
function ui.Button(opts)
  opts = opts or {}
  local active = opts.active == true
  local variant = opts.variant or (active and "primary" or "secondary")
  local bg = opts.bg
  local fg = opts.color
  local border_col = opts.border_color

  if not bg then
    if variant == "primary" then
      bg = C.accent
      fg = fg or (stl.content and stl.content.on_accent or C.bg_darker)
      border_col = border_col or C.accent
    elseif variant == "quiet" or variant == "ghost" then
      bg = "transparent"
      fg = fg or C.fg
      border_col = border_col or "transparent"
    elseif variant == "danger" then
      bg = stl.surface and stl.surface.raised or C.bg_dark
      fg = fg or (stl.status and stl.status.danger or C.red)
      border_col = border_col or (stl.status and stl.status.danger or C.red)
    else -- secondary
      bg = stl.surface and stl.surface.raised or C.bg_light
      fg = fg or C.fg
      border_col = border_col or (stl.stroke and stl.stroke.hairline or B.inactive)
    end
  end

  return wisp.widget.button {
    text = opts.text or opts.label or "Button",
    size = opts.size or F.body_sm,
    color = fg,
    bg = bg,
    border = { color = border_col or B.inactive, width = 1 },
    radius = opts.radius or (tokens and tokens.radius_sm or 2),
    padding = opts.padding or { left = 8, right = 8, top = 3, bottom = 3 },
    on_click = opts.on_click,
    width = opts.width,
    height = opts.height or 22,
    flex = opts.flex,
    tooltip = opts.tooltip,
    tooltip_placement = opts.tooltip_placement or opts.placement,
  }
end

function ui.IconButton(icon, on_click, opts)
  opts = opts or {}
  local active = opts.active == true
  local bg = opts.bg or (active and C.accent or (stl.surface and stl.surface.raised or C.bg_light))
  local fg = opts.color or (active and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg)

  return wisp.widget.button {
    text = icon,
    size = opts.size or F.heading,
    color = fg,
    bg = bg,
    border = { color = active and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 },
    radius = opts.radius or (tokens and tokens.radius_sm or 2),
    padding = opts.padding or 4,
    on_click = on_click,
    tooltip = opts.tooltip,
    tooltip_placement = opts.tooltip_placement or opts.placement,
  }
end

-- ── 6. Swiss Status Badge & Chip ─────────────────────────────────────────────
function ui.Badge(text_or_opts, color_or_opts, bg)
  local text = text_or_opts
  local opts = {}
  if type(text_or_opts) == "table" then
    opts = text_or_opts
    text = opts.text or opts.label or ""
  elseif type(color_or_opts) == "table" then
    opts = color_or_opts
  elseif type(color_or_opts) == "string" then
    opts = { color = color_or_opts, bg = bg }
  end
  local variant = opts.variant or "default"
  local dot = (opts.dot ~= false) and (not opts.color)

  local badge_bg = opts.bg or (stl.surface and stl.surface.sunken or C.bg_darker)
  local badge_fg = opts.color or C.fg_muted
  local border_color = opts.border or (stl.stroke and stl.stroke.hairline or B.inactive)

  if variant == "accent" or variant == "connected" or variant == "active" then
    badge_bg = opts.bg or (stl.accent and stl.accent.soft or C.bg_light)
    badge_fg = opts.color or C.accent
    border_color = opts.border or C.accent
  elseif variant == "warning" then
    badge_bg = opts.bg or (stl.status and stl.status.warning_soft or C.bg_light)
    badge_fg = opts.color or (stl.status and stl.status.warning or C.yellow)
    border_color = badge_fg
  elseif variant == "danger" or variant == "error" then
    badge_bg = opts.bg or (stl.status and stl.status.danger_soft or C.bg_light)
    badge_fg = opts.color or (stl.status and stl.status.danger or C.red)
    border_color = badge_fg
  end

  local content_layout = wisp.layout.fixed.horizontal {
    spacing = 4,
    align = { y = "center" },
  }
  if dot then
    content_layout:add(wisp.widget.textbox {
      text = "●",
      size = 7,
      color = badge_fg,
    })
  end
  local text_elem = wisp.widget.textbox {
    text = tostring(text or ""),
    size = opts.size or F.caption,
    color = badge_fg,
  }
  content_layout:add(text_elem)

  local badge = wisp.container.background {
    bg = badge_bg,
    border = { color = border_color, width = 1 },
    radius = tokens and tokens.radius_sm or 2,
    padding = opts.padding or { left = 6, right = 6, top = 2, bottom = 2 },
    widget = content_layout,
  }
  badge.text_elem = text_elem
  function badge:set_text(t) text_elem:set_text(t) end
  return badge
end
ui.Chip = ui.Badge

-- ── 7. Toggle Switch Row ─────────────────────────────────────────────────────
function ui.ToggleRow(label, icon, active, on_toggle, subtitle)
  local left = wisp.layout.fixed.horizontal {
    spacing = S.md,
    align = { y = "center" },
  }
  local icon_box = nil
  if icon then
    icon_box = wisp.widget.textbox {
      text = icon,
      size = F.heading,
      color = active and C.accent or C.fg_muted,
      width = "fit",
    }
    left:add(icon_box)
  end

  local title_box = wisp.widget.textbox {
    text = label,
    size = F.body,
    color = active and C.fg or C.fg_muted,
  }
  local sub_box = nil
  if subtitle then
    sub_box = wisp.widget.textbox {
      text = subtitle,
      size = F.caption,
      color = active and C.accent or C.fg_muted,
    }
  end
  local col = wisp.layout.fixed.vertical {
    spacing = 1,
    title_box,
  }
  if sub_box then col:add(sub_box) end
  left:add(col)

  local right = wisp.widget.checkbox {
    checked = active,
    active_color = C.accent,
    inactive_color = stl.stroke and stl.stroke.control or C.muted,
    on_change = on_toggle,
  }

  local inner = wisp.layout.align.horizontal(left, nil, right)
  inner.align = { y = "center" }

  local card = ui.Card({
    bg = active and (stl.surface and stl.surface.active or C.bg_light) or (stl.surface and stl.surface.raised or C.bg_dark),
    border_color = active and (stl.stroke and stl.stroke.focus or C.accent) or (stl.stroke and stl.stroke.hairline or B.inactive),
    padding = S.md,
    width = "fill",
    on_click = on_toggle,
  }, inner)

  card.icon_box = icon_box
  card.title_box = title_box
  card.sub_box = sub_box
  card.checkbox = right
  function card:set_state(is_act, sub)
    if icon_box then icon_box:set_color(is_act and C.accent or C.fg_muted) end
    title_box:set_color(is_act and C.fg or C.fg_muted)
    if sub_box and sub then
      sub_box:set_text(sub)
      sub_box:set_color(is_act and C.accent or C.fg_muted)
    end
    if right.set_checked then right:set_checked(is_act) end
    card:set_bg(is_act and (stl.surface and stl.surface.active or C.bg_light) or (stl.surface and stl.surface.raised or C.bg_dark))
    card:set_border_color(is_act and (stl.stroke and stl.stroke.focus or C.accent) or (stl.stroke and stl.stroke.hairline or B.inactive))
  end
  return card
end

-- ── 8. Fluid Debounced Slider Row ────────────────────────────────────────────
function ui.SliderRow(label, icon, value, min_val, max_val, on_change, unit)
  if type(label) == "table" then
    local opts = label
    label = opts.label or opts.title or "Value"
    icon = opts.icon
    value = opts.value or 0
    min_val = opts.min or opts.minimum or 0
    max_val = opts.max or opts.maximum or 100
    on_change = opts.on_change
    unit = opts.unit or "%"
  end
  unit = unit or "%"
  local val_int = math.floor(value or 0)

  local header_left = wisp.layout.fixed.horizontal {
    spacing = S.xs,
    align = { y = "center" },
  }
  local icon_box = nil
  if icon then
    icon_box = wisp.widget.textbox { text = icon, size = F.body, color = C.accent }
    header_left:add(icon_box)
  end
  header_left:add(wisp.widget.textbox { text = label, size = F.body, color = C.fg })

  local header_right = wisp.widget.textbox {
    text = tostring(val_int) .. unit,
    size = F.body_sm,
    color = C.accent,
  }
  local header = wisp.layout.align.horizontal(header_left, nil, header_right)

  local pending_timer = nil
  local latest_val = val_int

  local slider = wisp.widget.slider {
    value = value,
    minimum = min_val or 0,
    maximum = max_val or 100,
    bar_color = stl.surface and stl.surface.sunken or C.bg_light,
    fill_color = C.accent,
    handle_color = C.fg,
    on_change = function(val)
      local v = math.floor(val or 0)
      latest_val = v
      header_right:set_text(tostring(v) .. unit)

      -- Debounce hardware/service dispatch to avoid event loop starvation
      if not pending_timer then
        if wisp and wisp.timer and type(wisp.timer.after) == "function" then
          pending_timer = wisp.timer.after(50, function()
            pending_timer = nil
            if on_change then on_change(latest_val) end
          end)
        else
          if on_change then on_change(latest_val) end
        end
      end
    end,
  }

  local card = ui.Card({
    bg = stl.surface and stl.surface.raised or C.bg_dark,
    padding = S.md,
  }, wisp.layout.fixed.vertical {
    spacing = S.xs,
    header,
    slider,
  })

  card.header_right = header_right
  card.icon_box = icon_box
  card.slider = slider

  function card:set_value(val)
    local v = math.floor(val or 0)
    header_right:set_text(tostring(v) .. unit)
    if slider.set_value then slider:set_value(v) end
  end

  function card:set_icon(ic)
    if icon_box and ic then icon_box:set_text(ic) end
  end

  return card
end

-- ── 9. Swiss Search Input ────────────────────────────────────────────────────
function ui.SearchInput(id, value, placeholder, on_change, on_submit)
  return wisp.widget.input {
    id = id,
    value = value or "",
    placeholder = placeholder or "Search…",
    width = "fill",
    height = S.control_h or 26,
    radius = tokens and tokens.radius_sm or 2,
    font_size = F.body_sm or 11,
    padding_x = 12,
    padding_y = 8,
    background = stl.surface and stl.surface.sunken or C.bg_dark,
    border_color = stl.stroke and stl.stroke.hairline or B.inactive,
    focus_color = C.accent,
    hover_color = C.fg_muted,
    text_color = C.fg,
    placeholder_color = C.fg_muted,
    on_change = on_change,
    on_submit = on_submit,
  }
end

-- ── 10. Segmented Tab Navigation ─────────────────────────────────────────────
function ui.TabNav(tabs, active_idx, on_select)
  local row = wisp.layout.fixed.horizontal {
    spacing = S.xs,
  }
  for i, tab in ipairs(tabs) do
    local is_active = (active_idx == i)
    row:add(wisp.widget.button {
      text = tab,
      size = F.caption,
      color = is_active and (stl.content and stl.content.on_accent or C.bg_darker) or C.fg,
      bg = is_active and C.accent or (stl.surface and stl.surface.sunken or C.bg_dark),
      border = { color = is_active and C.accent or (stl.stroke and stl.stroke.hairline or B.inactive), width = 1 },
      radius = tokens and tokens.radius_sm or 2,
      padding = { left = 8, right = 8, top = 3, bottom = 3 },
      on_click = function() on_select(i) end,
    })
  end
  return row
end

-- ── 11. Minimalist Empty State ───────────────────────────────────────────────
function ui.EmptyState(icon, text, detail)
  local col = wisp.layout.fixed.vertical {
    spacing = S.xs,
    padding = 24,
    align = { x = "center", y = "center" },
  }
  local icon_box = nil
  if icon then
    icon_box = wisp.widget.textbox { text = icon, size = F.display, color = C.fg_muted }
    col:add(icon_box)
  end
  local text_box = wisp.widget.textbox { text = text or "No items found", size = F.body_sm, color = C.fg }
  col:add(text_box)

  local detail_box = nil
  if detail and detail ~= "" then
    detail_box = wisp.widget.textbox { text = detail, size = F.caption, color = C.fg_muted }
    col:add(detail_box)
  end

  col.icon_box = icon_box
  col.text_box = text_box
  col.detail_box = detail_box

  function col:set_state(ic, t, d)
    if icon_box and ic then icon_box:set_text(ic) end
    if text_box and t then text_box:set_text(t) end
    if detail_box and d then detail_box:set_text(d) end
  end
  return col
end

-- ── 12. Section Header with Action ───────────────────────────────────────────
function ui.SectionTitle(title, action_text, on_action)
  local left = wisp.widget.textbox {
    text = string.upper(title),
    size = F.caption,
    color = C.fg_muted,
  }
  local right = nil
  if action_text and on_action then
    right = wisp.widget.button {
      text = action_text,
      size = F.caption,
      color = C.accent,
      bg = "transparent",
      padding = { left = 4, right = 4, top = 1, bottom = 1 },
      border = { color = "transparent", width = 1 },
      on_click = on_action,
    }
  end
  local row = wisp.layout.align.horizontal(left, nil, right)
  row.title_box = left
  row.action_btn = right
  function row:set_action_text(t)
    if right and right.set_text then right:set_text(t) end
  end
  return row
end

-- ── 13. Metric & Progress Row ────────────────────────────────────────────────
function ui.MetricRow(icon, label, value_text, percentage, color)
  color = color or C.accent
  local left = wisp.layout.fixed.horizontal {
    spacing = S.xs,
    align = { y = "center" },
  }
  if icon then
    left:add(wisp.widget.textbox { text = icon, size = F.body, color = color })
  end
  left:add(wisp.widget.textbox { text = label, size = F.body_sm, color = C.fg })

  local right = wisp.widget.textbox { text = value_text, size = F.caption, color = color }
  local header = wisp.layout.align.horizontal(left, nil, right)

  local col = wisp.layout.fixed.vertical {
    spacing = 4,
    width = "fill",
    header,
  }
  col.type = "column"
  col.width = "fill"
  if percentage ~= nil then
    local pct = math.max(0, math.min(100, tonumber(percentage) or 0))
    col:add(wisp.widget.progressbar {
      value = pct,
      max_value = 100,
      height = 4,
      radius = 0,
      color = color,
      background_color = stl.surface and stl.surface.sunken or C.bg_light,
    })
  end
  return col
end

-- ── 14. Stat Card ────────────────────────────────────────────────────────────
function ui.StatCard(icon, value, label, color, on_click)
  color = color or C.accent
  local col = wisp.layout.fixed.vertical {
    spacing = 2,
    align = { x = "center" },
  }
  if icon then
    col:add(wisp.widget.textbox { text = icon, size = F.heading, color = color })
  end
  col:add(wisp.widget.textbox { text = tostring(value), size = F.title, color = C.fg })
  if label then
    col:add(wisp.widget.textbox { text = label, size = F.caption, color = C.fg_muted })
  end
  local card = ui.Card({
    bg = stl.surface and stl.surface.raised or C.bg_dark,
    border_color = stl.stroke and stl.stroke.hairline or B.inactive,
    padding = S.md,
    on_click = on_click,
    width = "fill",
  }, col)
  card.type = "card"
  card.width = "fill"
  return card
end

-- ── 15. Action Row ───────────────────────────────────────────────────────────
function ui.ActionRow(icon, title, subtitle, right_widget, on_click)
  return ui.Row {
    icon = icon,
    title = title,
    subtitle = subtitle,
    action = right_widget,
    on_click = on_click,
  }
end

-- ── 16. Info Row ─────────────────────────────────────────────────────────────
function ui.InfoRow(label, value, icon)
  local left = wisp.layout.fixed.horizontal {
    spacing = S.xs,
    align = { y = "center" },
  }
  if icon then
    left:add(wisp.widget.textbox { text = icon, size = F.body_sm, color = C.accent })
  end
  left:add(wisp.widget.textbox { text = label, size = F.body_sm, color = C.fg_muted })
  local right = wisp.widget.textbox { text = tostring(value), size = F.body_sm, color = C.fg }
  return wisp.layout.align.horizontal(left, nil, right)
end

-- ── 17. Progress Bar ─────────────────────────────────────────────────────────
function ui.ProgressBar(value, max_val, color, bg, height)
  return wisp.widget.progressbar {
    value = value or 0,
    max_value = max_val or 100,
    height = height or 4,
    radius = 0,
    color = color or C.accent,
    background_color = bg or (stl.surface and stl.surface.sunken or C.bg_light),
  }
end

-- ── 18. Reactive State Helper ────────────────────────────────────────────────
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
