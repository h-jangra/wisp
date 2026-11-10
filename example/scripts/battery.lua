local wisp = wisp

state.show_details = (state.show_details == true)
state.power_profile = state.power_profile or 1
local profiles = { "Balanced", "Performance", "Power Saver" }

wisp.widget.create({
  name = "battery",
  size = { width = 240, height = 96 },
  anchor = "top_right",
  margin = { top = 110, right = 12 },
  interval = 5000,

  on_click = function()
    state.show_details = not state.show_details
  end,

  on_scroll = function(dir)
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

    return Card {
      background = "#1E1E1EEE",
      border = { color = is_low and "#EF5350" or "#383838", width = 1 },
      radius = 12,
      padding = 12,
      gap = 6,
      Row {
        valign = "center",
        Text("Battery", { size = 13, color = "#FFFFFF" }),
        Spacer(),
        Chip(status_text, { background = status_bg, color = status_color, size = 10, radius = 10, px = 8, py = 2 }),
        Spacer(8),
        Text(string.format("%d%%", bat.percentage), { size = 14, color = "#FFFFFF" }),
      },
      Progress {
        value = bat.percentage,
        max = 100,
        height = 6,
        radius = 3,
        color = bar_color,
        background = "#333333",
      },
      Row {
        valign = "center",
        Text(state.show_details and string.format("%.1fW draw", bat.power_watts or 0.0) or profiles[state.power_profile], {
          size = 11,
          color = "#B0BEC5",
        }),
        Spacer(),
        Text("Click: Stats • Scroll: Profile", {
          size = 9,
          color = "#78909C",
        }),
      },
    }
  end,
})
