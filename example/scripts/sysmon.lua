local wisp = wisp

state.sysmon_view = state.sysmon_view or 1
local views = { "Overview", "Load & Uptime" }

wisp.widget.create({
  name = "sysmon",
  size = { width = 260, height = 135 },
  anchor = "top_right",
  margin = { top = 216, right = 12 },

  on_click = function(x, y)
    -- Clicking toggles between Overview and Load & Uptime view
    state.sysmon_view = (state.sysmon_view % #views) + 1
  end,

  on_scroll = function(dir, x, y)
    -- Scrolling cycles view modes
    state.sysmon_view = (state.sysmon_view % #views) + 1
  end,

  build = function()
    local cpu = wisp.system.cpu()
    local mem = wisp.system.memory()
    local l1, l5, l15 = wisp.system.loadavg()
    local uptime_sec = math.floor(wisp.system.uptime())
    local hours = math.floor(uptime_sec / 3600)
    local mins = math.floor((uptime_sec % 3600) / 60)

    local is_heavy = (cpu.usage > 80 or mem.percentage > 85)
    local status_chip_text = is_heavy and "HEAVY" or "OPTIMAL"
    local status_chip_bg = is_heavy and "#B71C1C" or "#1B5E20"
    local status_chip_col = is_heavy and "#FFCDD2" or "#A5D6A7"

    local body_children = {}

    if state.sysmon_view == 1 then
      -- Dual Gauge Overview (CPU & RAM)
      body_children = {
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("CPU Utilization", { size = 11, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.1f%%", cpu.usage), { size = 12, color = "#FFFFFF" })
          }
        }),
        wisp.ui.progress({
          value = cpu.usage,
          max = 100,
          height = 6,
          radius = 3,
          color = cpu.usage > 80 and "#EF5350" or "#90CAF9",
          background = "#333333"
        }),
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("RAM Usage", { size = 11, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.1f / %.1f GB", mem.used_mb / 1024, mem.total_mb / 1024), {
              size = 11,
              color = "#FFFFFF"
            })
          }
        }),
        wisp.ui.progress({
          value = mem.percentage,
          max = 100,
          height = 6,
          radius = 3,
          color = mem.percentage > 85 and "#EF5350" or "#CE93D8",
          background = "#333333"
        })
      }
    else
      -- Load Average & Uptime Details
      body_children = {
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Load Average", { size = 11, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.2f  %.2f  %.2f", l1, l5, l15), {
              size = 11,
              color = "#90CAF9"
            })
          }
        }),
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("System Uptime", { size = 11, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%dh %dm", hours, mins), {
              size = 12,
              color = "#FFFFFF"
            })
          }
        }),
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Free Memory", { size = 11, color = "#B0BEC5" }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.1f GB free", mem.avail_mb / 1024), {
              size = 11,
              color = "#66BB6A"
            })
          }
        })
      }
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
            -- Card Header
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text(views[state.sysmon_view], { size = 13, color = "#FFFFFF" }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = status_chip_bg,
                  corner_radius = 10,
                  padding = { top = 2, bottom = 2, left = 8, right = 8 },
                  children = {
                    wisp.ui.text(status_chip_text, { size = 10, color = status_chip_col })
                  }
                })
              }
            }),

            wisp.ui.separator({ color = "#383838", thickness = 1 }),

            wisp.layout.column({
              gap = 4,
              sizing = { width = "grow" },
              children = body_children
            })
          }
        })
      }
    })
  end
})

wisp.timer.every(2000, function() end)
