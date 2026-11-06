local wisp = wisp

state.show_details = (state.show_details == true)
state.power_profile = state.power_profile or 1
local profiles = { "Balanced", "Performance", "Power Saver" }

wisp.widget.create({
  name = "battery",
  size = { width = 240, height = 96 },
  anchor = "top_right",
  margin = { top = 110, right = 12 },

  on_click = function(x, y)
    -- Clicking toggles between compact and detailed stats view
    state.show_details = not state.show_details
  end,

  on_scroll = function(dir, x, y)
    -- Scrolling cycles through power profile modes
    if dir > 0 then
      state.power_profile = (state.power_profile % #profiles) + 1
    else
      state.power_profile = (state.power_profile - 2 + #profiles) % #profiles + 1
    end
  end,

  build = function()
    local bat = wisp.system.battery()
    local is_low = bat.percentage < 20 and not bat.charging
    local bar_color = bat.charging and "#66BB6A" or (is_low and "#EF5350" or "#90CAF9")
    local status_text = bat.charging and "⚡ Charging" or (is_low and "⚠ Low Battery" or bat.status)
    local status_bg = bat.charging and "#1B5E20" or (is_low and "#B71C1C" or "#263238")
    local status_color = bat.charging and "#A5D6A7" or (is_low and "#FFCDD2" or "#90CAF9")

    return wisp.layout.container({
      background = "#1E1E1EEE",
      border = { color = is_low and "#EF5350" or "#383838", width = 1 },
      corner_radius = 12,
      padding = 12,
      sizing = { width = "grow", height = "grow" },
      children = {
        wisp.layout.column({
          gap = 6,
          children = {
            -- Header Row with Typography and Status Chip
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text("Battery", { size = 13, color = "#FFFFFF" }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = status_bg,
                  corner_radius = 10,
                  padding = { top = 2, bottom = 2, left = 8, right = 8 },
                  children = {
                    wisp.ui.text(status_text, { size = 10, color = status_color })
                  }
                }),
                wisp.layout.spacer({ width = 8 }),
                wisp.ui.text(string.format("%d%%", bat.percentage), {
                  size = 14,
                  color = "#FFFFFF"
                })
              }
            }),

            -- MUI LinearProgress Bar
            wisp.ui.progress({
              value = bat.percentage,
              max = 100,
              height = 6,
              radius = 3,
              color = bar_color,
              background = "#333333"
            }),

            -- Bottom Details / Profile Row
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text(state.show_details and string.format("%.1fW draw", bat.power_watts or 0.0) or profiles[state.power_profile], {
                  size = 11,
                  color = "#B0BEC5"
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.ui.text("Click: Stats • Scroll: Profile", {
                  size = 9,
                  color = "#78909C"
                })
              }
            })
          }
        })
      }
    })
  end
})

wisp.timer.every(5000, function() end)
