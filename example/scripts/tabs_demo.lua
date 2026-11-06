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

  on_click = function(x, y)
    state.tab_clicks = state.tab_clicks + 1

    -- Tab strip at the top (y roughly 12 to 45)
    if y <= 45 then
      if x < 98 then
        state.active_tab = 1
        state.tab_feedback = "Switched to Stats"
      elseif x < 198 then
        state.active_tab = 2
        state.tab_feedback = "Switched to Toggles"
      else
        state.active_tab = 3
        state.tab_feedback = "Switched to About"
      end
      return
    end

    -- Tab-specific clicks
    if state.active_tab == 1 then
      state.tab_feedback = string.format("Stats clicked (%d)", state.tab_clicks)
    elseif state.active_tab == 2 then
      if y >= 60 and y <= 100 then
        state.toggle_sync = not state.toggle_sync
        state.tab_feedback = state.toggle_sync and "Auto-Sync Enabled" or "Auto-Sync Disabled"
      elseif y > 100 then
        state.toggle_dark = not state.toggle_dark
        state.tab_feedback = state.toggle_dark and "Dark Theme ON" or "Light Theme ON"
      end
    else
      state.tab_feedback = string.format("About clicked at (%d,%d)", x, y)
    end
  end,

  on_scroll = function(dir, x, y)
    -- Scrolling cycles through tabs
    if dir > 0 then
      state.active_tab = (state.active_tab % #tabs) + 1
    else
      state.active_tab = (state.active_tab - 2 + #tabs) % #tabs + 1
    end
    state.tab_feedback = string.format("Tab: %s (Scrolled)", tabs[state.active_tab])
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

    -- Build tab header elements
    local tab_headers = {}
    for i, name in ipairs(tabs) do
      local is_active = (state.active_tab == i)
      table.insert(tab_headers, wisp.layout.container({
        sizing = { width = "grow" },
        background = is_active and colors.primary_main or colors.surface,
        corner_radius = 6,
        padding = { top = 5, bottom = 5, left = 8, right = 8 },
        align = { x = "center", y = "center" },
        children = {
          wisp.ui.text(name, {
            size = 12,
            color = is_active and "#FFFFFF" or colors.text_secondary
          })
        }
      }))
    end

    -- Content depending on active tab
    local content_children = {}

    if state.active_tab == 1 then
      -- Stats Tab
      local cpu = wisp.system.cpu()
      local mem = wisp.system.memory()
      content_children = {
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Total Interactions", { size = 12, color = colors.text_secondary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(tostring(state.tab_clicks), { size = 15, color = colors.primary })
          }
        }),
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("CPU Usage", { size = 12, color = colors.text_secondary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.1f%%", cpu.usage), { size = 12, color = colors.text_primary })
          }
        }),
        wisp.ui.progress({
          value = cpu.usage,
          max = 100,
          height = 6,
          radius = 3,
          color = colors.primary,
          background = "#333333"
        }),
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Memory Usage", { size = 12, color = colors.text_secondary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text(string.format("%.0f%%", mem.percentage), { size = 12, color = colors.text_primary })
          }
        })
      }
    elseif state.active_tab == 2 then
      -- Toggles Tab
      content_children = {
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Auto-Sync Metrics", { size = 12, color = colors.text_primary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.layout.container({
              width = 34,
              height = 18,
              background = state.toggle_sync and colors.primary_main or "#424242",
              corner_radius = 9,
              padding = 2,
              children = {
                wisp.layout.row({
                  sizing = { width = "grow", height = "grow" },
                  align = { y = "center" },
                  children = state.toggle_sync and {
                    wisp.layout.spacer({ flex = 1 }),
                    wisp.ui.rect({ width = 14, height = 14, radius = 7, color = "#FFFFFF" })
                  } or {
                    wisp.ui.rect({ width = 14, height = 14, radius = 7, color = "#FFFFFF" }),
                    wisp.layout.spacer({ flex = 1 })
                  }
                })
              }
            })
          }
        }),
        wisp.layout.row({
          align = { y = "center" },
          children = {
            wisp.ui.text("Dark Theme Mode", { size = 12, color = colors.text_primary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.layout.container({
              width = 34,
              height = 18,
              background = state.toggle_dark and colors.primary_main or "#424242",
              corner_radius = 9,
              padding = 2,
              children = {
                wisp.layout.row({
                  sizing = { width = "grow", height = "grow" },
                  align = { y = "center" },
                  children = state.toggle_dark and {
                    wisp.layout.spacer({ flex = 1 }),
                    wisp.ui.rect({ width = 14, height = 14, radius = 7, color = "#FFFFFF" })
                  } or {
                    wisp.ui.rect({ width = 14, height = 14, radius = 7, color = "#FFFFFF" }),
                    wisp.layout.spacer({ flex = 1 })
                  }
                })
              }
            })
          }
        }),
        wisp.ui.text("Click each row to toggle switches", { size = 10, color = colors.text_secondary })
      }
    else
      -- About Tab
      content_children = {
        wisp.layout.row({
          children = {
            wisp.ui.text("Framework:", { size = 12, color = colors.text_secondary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text("Wisp 0.1.0 Wayland", { size = 12, color = colors.primary })
          }
        }),
        wisp.layout.row({
          children = {
            wisp.ui.text("Layout Engine:", { size = 12, color = colors.text_secondary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text("Clay High-Speed C", { size = 12, color = colors.text_primary })
          }
        }),
        wisp.layout.row({
          children = {
            wisp.ui.text("Scripting:", { size = 12, color = colors.text_secondary }),
            wisp.layout.spacer({ flex = 1 }),
            wisp.ui.text("Embedded Lua 5.4", { size = 12, color = colors.text_primary })
          }
        })
      }
    end

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
            -- Tab bar strip
            wisp.layout.row({
              gap = 6,
              children = tab_headers
            }),

            -- Divider
            wisp.ui.separator({ color = colors.border, thickness = 1 }),

            -- Active Panel Body
            wisp.layout.column({
              gap = 6,
              sizing = { width = "grow" },
              children = content_children
            }),

            wisp.layout.spacer({ flex = 1 }),

            -- Status footer banner
            wisp.layout.container({
              background = colors.surface,
              border = { color = colors.border, width = 1 },
              corner_radius = 6,
              padding = { top = 3, bottom = 3, left = 8, right = 8 },
              children = {
                wisp.ui.text(state.tab_feedback, { size = 10, color = colors.primary })
              }
            })
          }
        })
      }
    })
  end
})
