-- Calendar & Date System Panel for Wisp
-- Matches the clock calendar layout: hero date, year rail,
-- ISO week grid with 6 always-visible rows, month nav anchored at the bottom.

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
local C = tokens.color
local S = tokens.space
local F = tokens.font

local now = os.date("*t")
local cur_year = now.year
local cur_month = now.month
local today_day = now.day
local selected_day = today_day

local month_names = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
}
local weekday_captions = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" }

local CELL_W, CELL_H, CELL_GAP = 45, 36, 4
local WK_W, WK_GAP = 24, 12
local GRID_ROWS = 6

local function is_leap(y)
  return (y % 4 == 0 and y % 100 ~= 0) or (y % 400 == 0)
end

local function days_in_month(y, m)
  if m == 2 then return is_leap(y) and 29 or 28 end
  if m == 4 or m == 6 or m == 9 or m == 11 then return 30 end
  return 31
end

-- 0 = Sunday (matches the SUN-first header)
local function first_weekday(y, m)
  if m < 3 then
    m = m + 12
    y = y - 1
  end
  local k = y % 100
  local j = math.floor(y / 100)
  local h = (1 + math.floor(13 * (m + 1) / 5) + k + math.floor(k / 4) + math.floor(j / 4) - 2 * j) % 7
  return (h + 6) % 7
end

local function iso_week(y, m, d)
  local ts = os.time({ year = y, month = m, day = d, hour = 12 })
  return tonumber(os.date("%V", ts))
end

local function year_progress(y)
  if y ~= now.year then return y < now.year and 100 or 0 end
  return math.floor((tonumber(os.date("%j")) / (is_leap(y) and 366 or 365)) * 100)
end

local function change_month(delta)
  cur_month = cur_month + delta
  if cur_month > 12 then
    cur_month = 1
    cur_year = cur_year + 1
  elseif cur_month < 1 then
    cur_month = 12
    cur_year = cur_year - 1
  end
  selected_day = 1
end

local widget = wisp.widget("calendar", {
  width = 420,
  height = 432,
  anchor = "top_right",
  margin = { top = 30, right = 100 },
  exclusive = false,
  transparent = true,
  visible = false,
  keyboard = true,
  interval = 60000,
})

wisp.keyboard.bind("Escape", function() panels.close("calendar") end)

widget:build(function()
  local dim = days_in_month(cur_year, cur_month)
  local offset = first_weekday(cur_year, cur_month)
  local selected_day_c = math.min(selected_day, dim)
  local selected_name = os.date("%A", os.time({ year = cur_year, month = cur_month, day = selected_day_c, hour = 12 }))

  local function shift_month(y, m, delta)
    m = m + delta
    if m > 12 then m, y = 1, y + 1
    elseif m < 1 then m, y = 12, y - 1 end
    return y, m
  end
  local prev_y, prev_m = shift_month(cur_year, cur_month, -1)
  local next_y, next_m = shift_month(cur_year, cur_month, 1)
  local prev_dim = days_in_month(prev_y, prev_m)

  -- Week numbers, one per row; Thursday is the ISO rule-setter for the row.
  local grid_start = 1 - offset
  local wk_labels = {}
  for row = 0, GRID_ROWS - 1 do
    local thursday = grid_start + 4 + row * 7
    table.insert(wk_labels, wisp.ui.row {
      width = WK_W,
      height = CELL_H,
      align = { x = "center", y = "center" },
      wisp.ui.label { text = tostring(iso_week(cur_year, cur_month, thursday)), size = F.caption, color = tokens.border.subtle },
    })
  end

  local day_rows = {}
  for row = 0, GRID_ROWS - 1 do
    local cells = {}
    for col = 0, 6 do
      local n = grid_start + row * 7 + col
      local cy, cm, d, in_month
      if n >= 1 and n <= dim then
        cy, cm, d, in_month = cur_year, cur_month, n, true
      elseif n < 1 then
        cy, cm, d, in_month = prev_y, prev_m, prev_dim + n, false
      else
        cy, cm, d, in_month = next_y, next_m, n - dim, false
      end
      local is_today = in_month and cy == now.year and cm == now.month and d == today_day
      local is_sel = in_month and d == selected_day_c

      local bg = is_sel and C.bg_light or "transparent"
      local fg = is_sel and C.accent or (in_month and C.fg or C.selection)

      local by, bm, bd = cy, cm, d
      table.insert(cells, wisp.ui.button {
        id = "cal_day_" .. by .. "_" .. bm .. "_" .. bd,
        text = tostring(d),
        size = F.body,
        color = fg,
        bg = bg,
        border = is_today and { color = C.accent, width = 1 } or nil,
        radius = tokens.radius,
        width = CELL_W,
        height = CELL_H,
        padding = 0,
        on_click = function()
          cur_year, cur_month, selected_day = by, bm, bd
          widget:redraw()
        end,
      })
    end
    table.insert(day_rows, wisp.ui.row { gap = CELL_GAP, table.unpack(cells) })
  end

  local weekday_headers = {}
  for _, lbl in ipairs(weekday_captions) do
    table.insert(weekday_headers, wisp.ui.row {
      width = CELL_W,
      align = { x = "center" },
      wisp.ui.label { text = lbl, size = F.caption, color = C.fg_muted },
    })
  end

  local grid_height = 16 + 6 + GRID_ROWS * (CELL_H + 6) - 6
  local year_pct = year_progress(cur_year)

  return wisp.ui.col {
    bg = C.bg,
    border = { color = tokens.border.active, width = tokens.border.width },
    radius = tokens.radius,
    padding = S.panel_pad,
    gap = 12,

    -- Hero: calendar icon + large selected date, close button right
    wisp.ui.row {
      align = { y = "center" },
      gap = S.md,
      height = 46,
      wisp.ui.label { text = "󰸗", size = 40, color = C.accent },
      wisp.ui.col {
        gap = 0,
        wisp.ui.label { text = month_names[cur_month] .. " " .. selected_day_c, size = 30, color = C.fg },
        wisp.ui.label { text = selected_name, size = F.caption, color = C.fg_muted },
      },
      wisp.ui.spacer { flex = 1 },
      wisp.ui.button {
        text = "✕",
        size = F.body_sm,
        color = C.fg_muted,
        bg = C.bg_light,
        radius = tokens.radius,
        padding = 4,
        on_click = function() panels.close("calendar") end,
      },
    },

    -- Year progress rail
    wisp.ui.row {
      align = { y = "center" },
      gap = S.md,
      height = 16,
      wisp.ui.label { text = tostring(cur_year), size = F.caption, color = C.fg_muted },
      wisp.ui.progress {
        value = year_pct,
        max = 100,
        height = 6,
        radius = 3,
        color = C.fg,
        background = C.bg_light,
        grow = true,
      },
      wisp.ui.label { text = tostring(year_pct) .. "%", size = F.caption, color = C.fg },
    },

    -- Week-number gutter | hairline | day grid
    wisp.ui.row {
      gap = WK_GAP,
      height = grid_height,
      wisp.ui.col {
        gap = 6,
        wisp.ui.row {
          width = WK_W,
          height = 16,
          align = { x = "center", y = "center" },
          wisp.ui.label { text = "W", size = F.caption, color = C.fg_muted },
        },
        table.unpack(wk_labels),
      },
      wisp.ui.separator { vertical = true, height = grid_height, color = tokens.border.inactive },
      wisp.ui.col {
        gap = 6,
        wisp.ui.row {
          gap = CELL_GAP,
          height = 16,
          table.unpack(weekday_headers),
        },
        table.unpack(day_rows),
      },
    },

    -- Month stepping, anchored to the grid's outer bounds; the month label jumps home
    wisp.ui.row {
      align = { y = "center" },
      height = 28,
      wisp.ui.button {
        text = "‹",
        size = F.heading,
        color = C.fg_muted,
        bg = "transparent",
        width = 34,
        height = 28,
        padding = 0,
        on_click = function() change_month(-1); widget:redraw() end,
      },
      wisp.ui.spacer { flex = 1 },
      wisp.ui.label {
        text = string.upper(month_names[cur_month] .. " " .. cur_year),
        size = F.body_sm,
        color = C.fg_muted,
        on_click = function()
          cur_year, cur_month, selected_day = now.year, now.month, now.day
          widget:redraw()
        end,
      },
      wisp.ui.spacer { flex = 1 },
      wisp.ui.button {
        text = "›",
        size = F.heading,
        color = C.fg_muted,
        bg = "transparent",
        width = 34,
        height = 28,
        padding = 0,
        on_click = function() change_month(1); widget:redraw() end,
      },
    },
  }
end)

return widget:run()
