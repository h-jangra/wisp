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
  size = { width = 250, height = 230 },
  anchor = "bottom_left",
  margin = { bottom = 16, left = 16 },

  on_click = function(x, y)
    if y <= 38 then
      -- Header nav buttons
      if x < 60 then
        change_month(-1)
      elseif x > 190 then
        change_month(1)
      end
      return
    end

    -- Day grid hit testing
    local grid_top = 68
    local row_h = 24
    local col_w = 32
    local grid_left = 12

    if y >= grid_top and y < grid_top + (5 * row_h) then
      local row = math.floor((y - grid_top) / row_h)
      local col = math.floor((x - grid_left) / col_w)
      if col >= 0 and col < 7 then
        local offset = first_weekday(state.year, state.month)
        local day_idx = row * 7 + col - offset + 1
        local max_days = days_in_month(state.year, state.month)
        if day_idx >= 1 and day_idx <= max_days then
          state.selected = day_idx
        end
      end
    end
  end,

  on_scroll = function(dir, x, y)
    -- Scrolling changes month
    change_month(dir)
  end,

  build = function()
    local dim = days_in_month(state.year, state.month)
    local offset = first_weekday(state.year, state.month)

    local day_rows = {}
    local day = 1
    for week = 0, 4 do
      local row = {}
      for col = 0, 6 do
        local idx = week * 7 + col
        if idx >= offset and day <= dim then
          local d = day
          local is_sel = (state.selected == d)
          table.insert(row, wisp.layout.container({
            sizing = { width = 28, height = 20 },
            background = is_sel and "#1976D2" or "#242424",
            border = is_sel and { color = "#90CAF9", width = 1 } or nil,
            corner_radius = 6,
            align = { x = "center", y = "center" },
            children = {
              wisp.ui.text(tostring(d), {
                size = 11,
                color = is_sel and "#FFFFFF" or "#CDD6F4"
              })
            }
          }))
          day = day + 1
        else
          table.insert(row, wisp.layout.spacer({ width = 28, height = 20, flex = 0 }))
        end
      end
      table.insert(day_rows, wisp.layout.row({ gap = 4, children = row }))
    end

    local day_labels = { "S", "M", "T", "W", "T", "F", "S" }
    local label_elements = {}
    for _, lbl in ipairs(day_labels) do
      table.insert(label_elements, wisp.layout.container({
        sizing = { width = 28, height = 16 },
        align = { x = "center", y = "center" },
        children = {
          wisp.ui.text(lbl, { size = 10, color = "#B0BEC5" })
        }
      }))
    end

    return wisp.layout.container({
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      corner_radius = 12,
      padding = 12,
      sizing = { width = "grow", height = "grow" },
      children = {
        wisp.layout.column({
          gap = 6,
          children = {
            -- Calendar Header
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.layout.container({
                  background = "#2D2D2D",
                  corner_radius = 4,
                  padding = { top = 2, bottom = 2, left = 6, right = 6 },
                  children = {
                    wisp.ui.text("<", { size = 11, color = "#90CAF9" })
                  }
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.ui.text(string.format("%s %d", month_names[state.month], state.year), {
                  size = 13,
                  color = "#FFFFFF"
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = "#2D2D2D",
                  corner_radius = 4,
                  padding = { top = 2, bottom = 2, left = 6, right = 6 },
                  children = {
                    wisp.ui.text(">", { size = 11, color = "#90CAF9" })
                  }
                })
              }
            }),

            wisp.ui.separator({ color = "#383838", thickness = 1 }),

            -- Day of week headers
            wisp.layout.row({
              gap = 4,
              children = label_elements
            }),

            -- Days grid
            wisp.layout.column({
              gap = 4,
              children = day_rows
            }),

            wisp.layout.spacer({ flex = 1 }),

            -- Footer info
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text(string.format("Selected: %s %d, %d", month_names[state.month], state.selected, state.year), {
                  size = 10,
                  color = "#90CAF9"
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.ui.text("Scroll: Month", { size = 9, color = "#78909C" })
              }
            })
          }
        })
      }
    })
  end
})
