local wisp = wisp

state.year = state.year or 2026
state.month = state.month or 11
state.selected = state.selected or 6

local month_names = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
}

local function days_in_month(y, m)
  if m == 2 then
    if (y % 4 == 0 and y % 100 ~= 0) or (y % 400 == 0) then
      return 29
    end
    return 28
  end
  if m == 4 or m == 6 or m == 9 or m == 11 then
    return 30
  end
  return 31
end

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

local function change_month(delta)
  state.month = state.month + delta
  if state.month > 12 then
    state.month = 1
    state.year = state.year + 1
  elseif state.month < 1 then
    state.month = 12
    state.year = state.year - 1
  end
  state.selected = 1
end

wisp.widget.create({
  name = "calendar",
  size = { width = 250, height = 240 },
  anchor = "bottom_left",
  margin = { bottom = 16, left = 16 },

  on_scroll = function(dir)
    change_month(dir)
  end,

  build = function()
    local dim = days_in_month(state.year, state.month)
    local offset = first_weekday(state.year, state.month)

    local day_rows = {}
    local day = 1
    local max_weeks = math.ceil((offset + dim) / 7)
    for week = 0, max_weeks - 1 do
      local row = {}
      for col = 0, 6 do
        local idx = week * 7 + col
        if idx >= offset and day <= dim then
          local d = day
          local is_sel = (state.selected == d)
          table.insert(row, Button {
            id = "day_" .. d,
            width = 28,
            height = 20,
            text = tostring(d),
            background = is_sel and "#1976D2" or "#242424",
            color = is_sel and "#FFFFFF" or "#CDD6F4",
            size = 11,
            radius = 6,
            padding = 0,
            border = is_sel and { color = "#90CAF9", width = 1 } or nil,
            on_click = function()
              state.selected = d
            end,
          })
          day = day + 1
        else
          table.insert(row, Spacer { width = 28, height = 20 })
        end
      end
      table.insert(day_rows, Row { gap = 4, children = row })
    end

    local day_labels = { "S", "M", "T", "W", "T", "F", "S" }
    local label_elements = {}
    for _, lbl in ipairs(day_labels) do
      table.insert(label_elements, Container {
        width = 28,
        height = 16,
        align = "center",
        Text(lbl, { size = 10, color = "#B0BEC5" }),
      })
    end

    return Card {
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      radius = 12,
      padding = 12,
      gap = 6,
      Row {
        valign = "center",
        Button {
          id = "btn_prev",
          text = "<",
          background = "#2D2D2D",
          color = "#90CAF9",
          size = 11,
          radius = 4,
          padding = { x = 8, y = 2 },
          on_click = function() change_month(-1) end,
        },
        Spacer(),
        Text(string.format("%s %d", month_names[state.month], state.year), { size = 13, color = "#FFFFFF" }),
        Spacer(),
        Button {
          id = "btn_next",
          text = ">",
          background = "#2D2D2D",
          color = "#90CAF9",
          size = 11,
          radius = 4,
          padding = { x = 8, y = 2 },
          on_click = function() change_month(1) end,
        },
      },
      Separator { color = "#383838", thickness = 1 },
      Row { gap = 4, children = label_elements },
      Column { gap = 4, children = day_rows },
      Spacer(),
      Row {
        valign = "center",
        Text(string.format("Selected: %s %d, %d", month_names[state.month], state.selected, state.year), { size = 10, color = "#90CAF9" }),
        Spacer(),
        Text("Scroll: Month", { size = 9, color = "#78909C" }),
      },
    }
  end,
})
