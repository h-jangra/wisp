local wisp = wisp

state.panel_page = state.panel_page or 1
local pages = { "Overview", "Metrics" }

wisp.widget.create({
  name = "panel",
  size = { width = 290, height = 190 },
  anchor = "top_left",
  margin = { top = 16, left = 16 },

  on_click = function(x, y)
    -- Clicking toggles between pages
    state.panel_page = (state.panel_page % #pages) + 1
  end,

  on_scroll = function(dir, x, y)
    -- Scrolling cycles pages
    state.panel_page = (state.panel_page % #pages) + 1
  end,

  build = function()
    local cpu = wisp.system.cpu()
    local mem = wisp.system.memory()
    local bat = wisp.system.battery()

    local content = {}

    if state.panel_page == 1 then
      content = {
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("CPU Load", { size = 12, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.1f%%", cpu.usage), { size = 12, color = "#90CAF9" })
          }
        }),
        wisp.ui.progress({
          value = cpu.usage,
          max = 100,
          height = 6,
          radius = 3,
          color = "#90CAF9",
          background = "#333333"
        }),

        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Memory (RAM)", { size = 12, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.0f%%", mem.percentage), { size = 12, color = "#CE93D8" })
          }
        }),
        wisp.ui.progress({
          value = mem.percentage,
          max = 100,
          height = 6,
          radius = 3,
          color = "#CE93D8",
          background = "#333333"
        }),

        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Battery Level", { size = 12, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%d%% (%s)", bat.percentage, bat.status), { size = 12, color = "#66BB6A" })
          }
        }),
        wisp.ui.progress({
          value = bat.percentage,
          max = 100,
          height = 6,
          radius = 3,
          color = "#66BB6A",
          background = "#333333"
        })
      }
    else
      local l1, l5, l15 = wisp.system.loadavg()
      local uptime_sec = math.floor(wisp.system.uptime())
      local hours = math.floor(uptime_sec / 3600)
      local mins = math.floor((uptime_sec % 3600) / 60)

      content = {
        wisp.layout.row({
          children = {
            wisp.ui.text("1m Load Avg:", { size = 12, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.2f", l1), { size = 12, color = "#90CAF9" })
          }
        }),
        wisp.layout.row({
          children = {
            wisp.ui.text("5m / 15m Load:", { size = 12, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.2f / %.2f", l5, l15), { size = 12, color = "#FFFFFF" })
          }
        }),
        wisp.layout.row({
          children = {
            wisp.ui.text("Uptime:", { size = 12, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%d hours, %d mins", hours, mins), { size = 12, color = "#66BB6A" })
          }
        }),
        wisp.layout.row({
          children = {
            wisp.ui.text("Available RAM:", { size = 12, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.1f GB", mem.avail_mb / 1024), { size = 12, color = "#FFFFFF" })
          }
        })
      }
    end

    return wisp.layout.container({
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      corner_radius = 12,
      padding = 14,
      sizing = { width = "grow", height = "grow" },
      children = {
        wisp.layout.column({
          gap = 8,
          children = {
            -- MUI Card Header
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.layout.column({
                  gap = 2,
                  children = {
                    wisp.ui.text("System Overview", { size = 14, color = "#FFFFFF" }),
                    wisp.ui.text(pages[state.panel_page], { size = 11, color = "#90CAF9" }),
                  }
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = "#2D2D2D",
                  corner_radius = 10,
                  padding = { top = 2, bottom = 2, left = 8, right = 8 },
                  children = {
                    wisp.ui.text(string.format("Page %d/2", state.panel_page), { size = 10, color = "#B0BEC5" })
                  }
                })
              }
            }),

            wisp.ui.separator({ color = "#383838", thickness = 1 }),

            wisp.layout.column({
              gap = 6,
              children = content
            }),

            wisp.layout.spacer({ flex = 1 }),

            wisp.ui.text("Click or scroll anywhere to flip view", {
              size = 9,
              color = "#78909C"
            })
          }
        })
      }
    })
  end
})

wisp.timer.every(2000, function() end)
