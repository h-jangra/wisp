local wisp = wisp

state.use_24h = (state.use_24h == true)
state.show_seconds = (state.show_seconds ~= false)
state.tz_offset = state.tz_offset or 0

wisp.widget.create({
  name = "clock",
  size = { width = 230, height = 90 },
  anchor = "top_right",
  margin = { top = 12, right = 12 },

  on_click = function(x, y)
    -- Clicking toggles between 12-hour and 24-hour display
    state.use_24h = not state.use_24h
  end,

  on_scroll = function(dir, x, y)
    -- Scrolling adjusts custom timezone offset
    state.tz_offset = math.max(-12, math.min(14, state.tz_offset + dir))
  end,

  build = function()
    local mui = wisp.mui or {}
    local colors = mui.colors or {
      paper = "#1E1E1EEE",
      border = "#383838",
      primary = "#90CAF9",
      primary_main = "#1976D2",
      text_primary = "#FFFFFF",
      text_secondary = "#B0BEC5",
      surface = "#242424"
    }

    local now = os.time() + (state.tz_offset * 3600)
    local time_format = state.use_24h and (state.show_seconds and "%H:%M:%S" or "%H:%M") or (state.show_seconds and "%I:%M:%S" or "%I:%M")
    local time_str = os.date(time_format, now)
    local ampm = state.use_24h and "24H" or os.date("%p", now)
    local date_str = os.date("%a, %b %d", now)
    local tz_label = (state.tz_offset == 0) and "LOCAL" or string.format("UTC%+d", state.tz_offset)

    return wisp.layout.container({
      background = colors.paper,
      border = { color = colors.border, width = 1 },
      corner_radius = 12,
      padding = { top = 10, bottom = 10, left = 14, right = 14 },
      sizing = { width = "grow", height = "grow" },
      children = {
        wisp.layout.column({
          align = { x = "center", y = "center" },
          gap = 4,
          children = {
            wisp.layout.row({
              align = { y = "center" },
              gap = 8,
              children = {
                wisp.ui.text(time_str, { size = 22, color = colors.text_primary }),
                wisp.layout.container({
                  background = colors.primary_main,
                  corner_radius = 4,
                  padding = { top = 2, bottom = 2, left = 6, right = 6 },
                  children = {
                    wisp.ui.text(ampm, { size = 10, color = "#FFFFFF" })
                  }
                }),
                (state.tz_offset ~= 0) and wisp.layout.container({
                  background = colors.surface,
                  border = { color = colors.border, width = 1 },
                  corner_radius = 4,
                  padding = { top = 2, bottom = 2, left = 5, right = 5 },
                  children = {
                    wisp.ui.text(tz_label, { size = 10, color = colors.primary })
                  }
                }) or nil
              }
            }),
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text(date_str, { size = 11, color = colors.text_secondary }),
                wisp.layout.spacer({ width = 10 }),
                wisp.ui.text("Click: 12/24h • Scroll: TZ", { size = 9, color = "#78909C" })
              }
            })
          }
        })
      }
    })
  end
})

wisp.timer.every(1000, function() end)
