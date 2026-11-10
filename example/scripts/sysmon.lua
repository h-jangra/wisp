local wisp = wisp

state.sysmon_view = state.sysmon_view or 1
local views = { "Overview", "Load & Uptime" }

wisp.widget.create({
  name = "sysmon",
  size = { width = 260, height = 135 },
  anchor = "top_right",
  margin = { top = 216, right = 12 },
  interval = 2000,

  on_click = function()
    state.sysmon_view = (state.sysmon_view % #views) + 1
  end,

  on_scroll = function()
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

    local body_view
    if state.sysmon_view == 1 then
      body_view = Column {
        gap = 4,
        Row {
          valign = "center",
          Text("CPU Utilization", { size = 11, color = "#B0BEC5" }),
          Spacer(),
          Text(string.format("%.1f%%", cpu.usage), { size = 12, color = "#FFFFFF" }),
        },
        Progress {
          value = cpu.usage,
          max = 100,
          height = 6,
          radius = 3,
          color = cpu.usage > 80 and "#EF5350" or "#90CAF9",
          background = "#333333",
        },
        Row {
          valign = "center",
          Text("RAM Usage", { size = 11, color = "#B0BEC5" }),
          Spacer(),
          Text(string.format("%.1f / %.1f GB", mem.used_mb / 1024, mem.total_mb / 1024), { size = 11, color = "#FFFFFF" }),
        },
        Progress {
          value = mem.percentage,
          max = 100,
          height = 6,
          radius = 3,
          color = mem.percentage > 85 and "#EF5350" or "#CE93D8",
          background = "#333333",
        },
      }
    else
      body_view = Column {
        gap = 4,
        Row {
          valign = "center",
          Text("Load Average", { size = 11, color = "#B0BEC5" }),
          Spacer(),
          Text(string.format("%.2f  %.2f  %.2f", l1, l5, l15), { size = 11, color = "#90CAF9" }),
        },
        Row {
          valign = "center",
          Text("System Uptime", { size = 11, color = "#B0BEC5" }),
          Spacer(),
          Text(string.format("%dh %dm", hours, mins), { size = 12, color = "#FFFFFF" }),
        },
        Row {
          valign = "center",
          Text("Free Memory", { size = 11, color = "#B0BEC5" }),
          Spacer(),
          Text(string.format("%.1f GB free", mem.avail_mb / 1024), { size = 11, color = "#66BB6A" }),
        },
      }
    end

    return Card {
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      radius = 12,
      padding = 12,
      gap = 6,
      Row {
        valign = "center",
        Text(views[state.sysmon_view], { size = 13, color = "#FFFFFF" }),
        Spacer(),
        Chip(status_chip_text, { background = status_chip_bg, color = status_chip_col, size = 10, radius = 10, px = 8, py = 2 }),
      },
      Separator { color = "#383838", thickness = 1 },
      body_view,
    }
  end,
})
