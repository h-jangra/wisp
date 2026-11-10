local wisp = wisp

state.use_24h = (state.use_24h == true)
state.show_seconds = (state.show_seconds ~= false)
state.tz_offset = state.tz_offset or 0

wisp.widget.create({
  name = "clock",
  size = { width = 230, height = 90 },
  anchor = "top_right",
  margin = { top = 12, right = 12 },
  interval = 1000,

  on_click = function()
    state.use_24h = not state.use_24h
  end,

  on_scroll = function(dir)
    state.tz_offset = math.max(-12, math.min(14, state.tz_offset + dir))
  end,

  build = function()
    local now = os.time() + (state.tz_offset * 3600)
    local time_format = state.use_24h and (state.show_seconds and "%H:%M:%S" or "%H:%M") or (state.show_seconds and "%I:%M:%S" or "%I:%M")
    local time_str = os.date(time_format, now)
    local ampm = state.use_24h and "24H" or os.date("%p", now)
    local date_str = os.date("%a, %b %d", now)
    local tz_label = (state.tz_offset == 0) and "LOCAL" or string.format("UTC%+d", state.tz_offset)

    return Card {
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      radius = 12,
      padding = { x = 14, y = 10 },
      align = "center",
      gap = 4,
      Row {
        valign = "center",
        gap = 8,
        Text(time_str, { size = 22, color = "#FFFFFF" }),
        Chip(ampm, { background = "#1976D2", color = "#FFFFFF", size = 10, radius = 4, px = 6, py = 2 }),
        (state.tz_offset ~= 0) and Chip(tz_label, { background = "#242424", border = { color = "#383838", width = 1 }, color = "#90CAF9", size = 10, radius = 4, px = 5, py = 2 }) or nil,
      },
      Row {
        valign = "center",
        Text(date_str, { size = 11, color = "#B0BEC5" }),
        Spacer(10),
        Text("Click: 12/24h • Scroll: TZ", { size = 9, color = "#78909C" }),
      },
    }
  end,
})
