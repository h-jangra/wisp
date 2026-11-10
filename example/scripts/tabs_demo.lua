local wisp = wisp

state.active_tab = state.active_tab or 1
state.tab_clicks = state.tab_clicks or 0
state.toggle_sync = (state.toggle_sync ~= false)
state.toggle_dark = (state.toggle_dark ~= false)
state.tab_feedback = state.tab_feedback or "Tab 1: Stats Active"

local tabs = { "Stats", "Toggles", "About" }

wisp.widget.create({
  name = "tabs_demo",
  size = { width = 300, height = 210 },
  anchor = "top_left",
  margin = { top = 206, left = 16 },

  on_click = function()
    state.tab_clicks = state.tab_clicks + 1
  end,

  on_scroll = function(dir)
    if dir > 0 then
      state.active_tab = (state.active_tab % #tabs) + 1
    else
      state.active_tab = (state.active_tab - 2 + #tabs) % #tabs + 1
    end
    state.tab_feedback = string.format("Tab: %s (Scrolled)", tabs[state.active_tab])
  end,

  build = function()
    local tab_headers = {}
    for i, name in ipairs(tabs) do
      local is_active = (state.active_tab == i)
      table.insert(tab_headers, Button {
        id = "tab_btn_" .. i,
        grow = true,
        text = name,
        background = is_active and "#1976D2" or "#242424",
        color = is_active and "#FFFFFF" or "#B0BEC5",
        size = 12,
        radius = 6,
        padding = { x = 8, y = 6 },
        on_click = function()
          state.active_tab = i
          state.tab_feedback = "Switched to " .. name
        end,
      })
    end

    local body_view
    if state.active_tab == 1 then
      local cpu = wisp.system.cpu()
      local mem = wisp.system.memory()
      body_view = Column {
        gap = 6,
        Row {
          valign = "center",
          Text("Total Interactions", { size = 12, color = "#B0BEC5" }),
          Spacer(),
          Text(tostring(state.tab_clicks), { size = 15, color = "#90CAF9" }),
        },
        Row {
          valign = "center",
          Text("CPU Usage", { size = 12, color = "#B0BEC5" }),
          Spacer(),
          Text(string.format("%.1f%%", cpu.usage), { size = 12, color = "#FFFFFF" }),
        },
        Progress {
          value = cpu.usage,
          max = 100,
          height = 6,
          radius = 3,
          color = "#90CAF9",
          background = "#333333",
        },
        Row {
          valign = "center",
          Text("Memory Usage", { size = 12, color = "#B0BEC5" }),
          Spacer(),
          Text(string.format("%.0f%%", mem.percentage), { size = 12, color = "#FFFFFF" }),
        },
      }
    elseif state.active_tab == 2 then
      body_view = Column {
        gap = 6,
        Row {
          valign = "center",
          Text("Auto-Sync Metrics", { size = 12, color = "#FFFFFF" }),
          Spacer(),
          Switch {
            id = "toggle_sync_btn",
            value = state.toggle_sync,
            on_change = function(v)
              state.toggle_sync = v
              state.tab_feedback = v and "Auto-Sync Enabled" or "Auto-Sync Disabled"
            end,
          },
        },
        Row {
          valign = "center",
          Text("Dark Theme Mode", { size = 12, color = "#FFFFFF" }),
          Spacer(),
          Switch {
            id = "toggle_dark_btn",
            value = state.toggle_dark,
            on_change = function(v)
              state.toggle_dark = v
              state.tab_feedback = v and "Dark Theme ON" or "Light Theme ON"
            end,
          },
        },
        Text("Click each row to toggle switches", { size = 10, color = "#B0BEC5" }),
      }
    else
      body_view = Column {
        gap = 6,
        Row {
          Text("Framework:", { size = 12, color = "#B0BEC5" }),
          Spacer(),
          Text("Wisp 0.1.0 Wayland", { size = 12, color = "#90CAF9" }),
        },
        Row {
          Text("Layout Engine:", { size = 12, color = "#B0BEC5" }),
          Spacer(),
          Text("Clay High-Speed C", { size = 12, color = "#FFFFFF" }),
        },
        Row {
          Text("Scripting:", { size = 12, color = "#B0BEC5" }),
          Spacer(),
          Text("Embedded Lua 5.4", { size = 12, color = "#FFFFFF" }),
        },
      }
    end

    return Card {
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      radius = 12,
      padding = 14,
      gap = 8,
      Row { gap = 6, children = tab_headers },
      Separator { color = "#383838", thickness = 1 },
      body_view,
      Spacer(),
      Container {
        background = "#242424",
        border = { color = "#383838", width = 1 },
        radius = 6,
        padding = { x = 8, y = 3 },
        Text(state.tab_feedback, { size = 10, color = "#90CAF9" }),
      },
    }
  end,
})
