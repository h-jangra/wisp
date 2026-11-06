local wisp = wisp

state.volume = state.volume or 65
state.brightness = state.brightness or 80
state.scroll_feed = state.scroll_feed or "Scroll over sliders"
state.last_dir = state.last_dir or 0
state.prev_volume = state.prev_volume or 65

wisp.widget.create({
  name = "scroll_demo",
  size = { width = 280, height = 180 },
  anchor = "top_left",
  margin = { top = 16, left = 16 },

  on_scroll = function(dir, x, y)
    local step = 5 * dir
    if y < 95 then
      -- Upper half: Volume control
      state.volume = math.max(0, math.min(100, state.volume + step))
      state.last_dir = dir
      state.scroll_feed = string.format("Volume %s5%% -> %d%%", dir > 0 and "+" or "", state.volume)
    else
      -- Lower half: Brightness control
      state.brightness = math.max(0, math.min(100, state.brightness + step))
      state.last_dir = dir
      state.scroll_feed = string.format("Brightness %s5%% -> %d%%", dir > 0 and "+" or "", state.brightness)
    end
  end,

  on_click = function(x, y)
    if y < 95 then
      -- Click volume row to toggle Mute
      if state.volume > 0 then
        state.prev_volume = state.volume
        state.volume = 0
        state.scroll_feed = "Muted (0%)"
      else
        state.volume = state.prev_volume > 0 and state.prev_volume or 50
        state.scroll_feed = string.format("Unmuted (%d%%)", state.volume)
      end
    else
      -- Click brightness row to toggle Day (80%) / Night (20%)
      if state.brightness > 50 then
        state.brightness = 20
        state.scroll_feed = "Night Mode (20%)"
      else
        state.brightness = 80
        state.scroll_feed = "Day Mode (80%)"
      end
    end
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
      warning = "#FFA726",
      surface = "#242424"
    }

    local vol_color = (state.volume == 0) and "#EF5350" or colors.primary
    local bright_color = colors.warning

    return wisp.layout.container({
      background = colors.paper,
      border = { color = colors.border, width = 1 },
      corner_radius = 12,
      padding = 14,
      sizing = { width = "grow", height = "grow" },
      children = {
        wisp.layout.column({
          gap = 8,
          children = {
            -- Header
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.layout.column({
                  gap = 2,
                  children = {
                    wisp.ui.text("MUI Scroll Controller", { size = 14, color = colors.text_primary }),
                    wisp.ui.text("Scroll wheel adjusts sliders", { size = 11, color = colors.text_secondary }),
                  }
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = colors.surface,
                  border = { color = colors.border, width = 1 },
                  corner_radius = 12,
                  padding = { top = 3, bottom = 3, left = 8, right = 8 },
                  children = {
                    wisp.ui.text(state.scroll_feed, { size = 10, color = colors.primary })
                  }
                })
              }
            }),

            wisp.ui.separator({ color = colors.border, thickness = 1 }),

            -- Slider 1: Volume Row
            wisp.layout.column({
              gap = 4,
              children = {
                wisp.layout.row({
                  align = { y = "center" },
                  children = {
                    wisp.ui.text(state.volume == 0 and "Volume (Muted)" or "Volume", {
                      size = 12,
                      color = (state.volume == 0) and "#EF5350" or colors.text_primary
                    }),
                    wisp.layout.spacer({ flex = 1 }),
                    wisp.ui.text(string.format("%d%%", state.volume), {
                      size = 12,
                      color = vol_color
                    })
                  }
                }),
                wisp.ui.progress({
                  value = state.volume,
                  max = 100,
                  height = 7,
                  radius = 4,
                  color = vol_color,
                  background = "#333333"
                })
              }
            }),

            -- Slider 2: Brightness Row
            wisp.layout.column({
              gap = 4,
              children = {
                wisp.layout.row({
                  align = { y = "center" },
                  children = {
                    wisp.ui.text("Brightness", { size = 12, color = colors.text_primary }),
                    wisp.layout.spacer({ flex = 1 }),
                    wisp.ui.text(string.format("%d%%", state.brightness), {
                      size = 12,
                      color = bright_color
                    })
                  }
                }),
                wisp.ui.progress({
                  value = state.brightness,
                  max = 100,
                  height = 7,
                  radius = 4,
                  color = bright_color,
                  background = "#333333"
                })
              }
            }),

            -- Footer Hint
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text("Click upper/lower half to toggle mute / night mode", {
                  size = 10,
                  color = colors.text_secondary
                })
              }
            })
          }
        })
      }
    })
  end
})
