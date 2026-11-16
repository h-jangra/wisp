-- Lightweight, Reusable Popup and Tooltip Engine for Wisp
-- Supports hover tooltips, click-triggered popups, edge collision avoidance,
-- configurable delay, flicker-free pointer transitions, and keyboard/outside dismissal.

local M = {}

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

local stl = require("style")
local C = stl.color
local S = stl.space
local F = stl.font
local R = stl.radius

M.widget_name = "wisp_popup"
M.is_open = false
M.active_mode = nil -- "tooltip" or "popup"
M.active_text = nil
M.active_content = nil
M.hover_timer = nil
M.leave_timer = nil

local function get_screen_size()
  local scr_w, scr_h = 1920, 1080
  if wisp and wisp.screen and wisp.screen.outputs then
    local ok, outs = pcall(wisp.screen.outputs)
    if ok and type(outs) == "table" and outs[1] then
      scr_w = outs[1].width or scr_w
      scr_h = outs[1].height or scr_h
    end
  end
  return scr_w, scr_h
end

local function cancel_timers()
  if M.hover_timer and wisp and wisp.timer and wisp.timer.cancel then
    pcall(wisp.timer.cancel, M.hover_timer)
    M.hover_timer = nil
  end
  if M.leave_timer and wisp and wisp.timer and wisp.timer.cancel then
    pcall(wisp.timer.cancel, M.leave_timer)
    M.leave_timer = nil
  end
end

-- Screen collision handling
local function calculate_geometry(rel_x, rel_y, req_w, req_h, placement)
  placement = placement or "bottom"
  local wx, wy = 0, 0
  if wisp and wisp.widget and wisp.widget.screen_pos then
    local ok, x0, y0 = pcall(wisp.widget.screen_pos)
    if ok and x0 and y0 then
      wx, wy = x0, y0
    end
  end

  local abs_x = wx + (rel_x or 0)
  local abs_y = wy + (rel_y or 0)

  local scr_w, scr_h = get_screen_size()
  local px, py = abs_x, abs_y

  if placement == "bottom" then
    px = abs_x - (req_w / 2)
    py = abs_y + 14
    if py + req_h > scr_h - 8 then
      py = abs_y - req_h - 10 -- flip above if overflow
    end
  elseif placement == "top" then
    px = abs_x - (req_w / 2)
    py = abs_y - req_h - 10
    if py < 8 then
      py = abs_y + 14 -- flip below if overflow
    end
  elseif placement == "right" then
    px = abs_x + 14
    py = abs_y - (req_h / 2)
    if px + req_w > scr_w - 8 then
      px = abs_x - req_w - 10 -- flip left
    end
  elseif placement == "left" then
    px = abs_x - req_w - 10
    py = abs_y - (req_h / 2)
    if px < 8 then
      px = abs_x + 14 -- flip right
    end
  end

  -- Clamp inside screen boundaries
  px = math.max(8, math.min(scr_w - req_w - 8, px))
  py = math.max(8, math.min(scr_h - req_h - 8, py))

  return math.floor(px), math.floor(py)
end

function M.render_content()
  if not M.is_open then
    return wibox.layout.fixed.vertical { width = 1, height = 1 }
  end

  if M.active_mode == "tooltip" then
    return wibox.container.background {
      bg = C.bg_darker,
      border = { color = C.accent, width = 1 },
      radius = R,
      padding = { left = 8, right = 8, top = 4, bottom = 4 },
      width = "fit",
      height = "fit",
      on_hover = cancel_timers,
      on_leave = function() M.schedule_hide(100) end,
      widget = wibox.container.place(
        wibox.widget.textbox {
          text = tostring(M.active_text or ""),
          size = F.caption or 11,
          color = C.fg,
        },
        "center"
      )
    }
  elseif M.active_mode == "popup" and M.active_content then
    local inner
    if type(M.active_content) == "function" then
      inner = M.active_content()
    elseif type(M.active_content) == "table" then
      inner = M.active_content
    end
    return wibox.container.background {
      bg = C.bg_dark,
      border = { color = stl.border.inactive, width = 1 },
      radius = R,
      padding = S.sm,
      width = "fill",
      height = "fill",
      on_hover = cancel_timers,
      on_leave = function() M.schedule_hide(150) end,
      widget = inner or wibox.layout.fixed.vertical {},
    }
  end

  return wibox.widget.base.make_widget(nil, "empty")
end

local function apply_popup_window(px, py, pw, ph)
  if not (wisp and wisp.widget) then return end
  pcall(wisp.widget.size, M.widget_name, pw, ph)
  pcall(wisp.widget.anchor, M.widget_name, "top_left")
  if wisp.widget.margin then
    pcall(wisp.widget.margin, M.widget_name, { top = py, left = px, right = 0, bottom = 0 })
  end
  pcall(wisp.widget.show, M.widget_name)
  pcall(wisp.widget.redraw, M.widget_name)
end

function M.show_tooltip(text, x, y, placement, opts)
  if not text or text == "" then return end
  opts = opts or {}
  local delay = opts.delay or 250 -- configurable hover delay ms

  cancel_timers()

  local function do_show()
    M.active_mode = "tooltip"
    M.active_text = text
    M.is_open = true

    local str_len = #tostring(text)
    local pw = math.max(64, math.floor(str_len * 7.2) + 20)
    local ph = 28
    local px, py = calculate_geometry(x, y, pw, ph, placement or "bottom")

    apply_popup_window(px, py, pw, ph)
  end

  if delay <= 0 or not (wisp and wisp.timer and wisp.timer.after) then
    do_show()
  else
    M.hover_timer = wisp.timer.after(delay, do_show)
  end
end

function M.hide_tooltip()
  cancel_timers()
  if not M.is_open or M.active_mode ~= "tooltip" then return end
  M.is_open = false
  M.active_text = nil
  if wisp and wisp.widget then
    pcall(wisp.widget.hide, M.widget_name)
  end
end

function M.show(content, placement, x, y, opts)
  opts = opts or {}
  cancel_timers()

  M.active_mode = "popup"
  M.active_content = content
  M.is_open = true

  local pw = opts.width or 220
  local ph = opts.height or 140
  local px, py = calculate_geometry(x, y, pw, ph, placement or "bottom")

  apply_popup_window(px, py, pw, ph)
end

function M.hide()
  cancel_timers()
  if not M.is_open then return end
  M.is_open = false
  M.active_content = nil
  M.active_text = nil
  if wisp and wisp.widget then
    pcall(wisp.widget.hide, M.widget_name)
  end
end

function M.schedule_hide(delay_ms)
  delay_ms = delay_ms or 120
  cancel_timers()
  if wisp and wisp.timer and wisp.timer.after then
    M.leave_timer = wisp.timer.after(delay_ms, function()
      M.hide()
    end)
  else
    M.hide()
  end
end

function M.toggle(content, placement, x, y, opts)
  if M.is_open and M.active_mode == "popup" then
    M.hide()
  else
    M.show(content, placement, x, y, opts)
  end
end

if wisp then
  wisp.popup = M
end

return M
