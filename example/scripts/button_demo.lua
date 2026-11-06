local wisp = wisp

state.clicks = state.clicks or 0
state.scroll_delta = state.scroll_delta or 0
state.switch_on = (state.switch_on ~= false)
state.last_action = state.last_action or "Ready"

wisp.widget.create({
  name = "button_demo",
  size = { width = 280, height = 210 },
  anchor = "bottom_right",
  margin = { bottom = 16, right = 16 },

  on_click = function(x, y)
    -- Hit testing based on logical coordinates
    if y >= 118 and y <= 152 then
      if x >= 14 and x <= 130 then
        -- Increment button clicked
        state.clicks = state.clicks + 1
        state.last_action = "+1 Increment"
      elseif x >= 140 and x <= 266 then
        -- Decrement button clicked
        state.clicks = math.max(0, state.clicks - 1)
        state.last_action = "-1 Decrement"
      end
    elseif y >= 160 and y <= 195 then
      if x >= 14 and x <= 130 then
        -- Reset button clicked
        state.clicks = 0
        state.scroll_delta = 0
        state.last_action = "Reset to 0"
      elseif x >= 140 and x <= 266 then
        -- Toggle switch clicked
        state.switch_on = not state.switch_on
        state.last_action = state.switch_on and "Switch ON" or "Switch OFF"
      end
    else
      -- Clicking card surface increments click counter
      state.clicks = state.clicks + 1
      state.last_action = string.format("Surface @ (%d,%d)", x, y)
    end
  end,

  on_scroll = function(dir, x, y)
    state.scroll_delta = state.scroll_delta + dir
    state.clicks = math.max(0, state.clicks + dir)
    state.last_action = (dir > 0) and "Scroll Down (+1)" or "Scroll Up (-1)"
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
      success = "#66BB6A",
      surface = "#282828"
    }

    return wisp.layout.container({
      background = colors.paper or "#1E1E1EEE",
      border = { color = colors.border or "#383838", width = 1 },
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
                    wisp.ui.text("MUI Action Card", { size = 14, color = colors.text_primary }),
                    wisp.ui.text("Click & Scroll Demo", { size = 11, color = colors.text_secondary }),
                  }
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = state.switch_on and "#1B5E20" or "#37474F",
                  corner_radius = 12,
                  padding = { top = 3, bottom = 3, left = 8, right = 8 },
                  children = {
                    wisp.ui.text(state.switch_on and "ACTIVE" or "PAUSED", {
                      size = 10,
                      color = state.switch_on and "#A5D6A7" or "#B0BEC5"
                    })
                  }
                })
              }
            }),

            -- Divider
            wisp.ui.separator({ color = colors.border, thickness = 1 }),

            -- Counter & Status Metric Row
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.layout.column({
                  gap = 2,
                  children = {
                    wisp.ui.text(string.format("Count: %d", state.clicks), { size = 18, color = colors.primary }),
                    wisp.ui.text(string.format("Wheel: %+d", state.scroll_delta), { size = 11, color = colors.text_secondary }),
                  }
                }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = colors.surface,
                  border = { color = colors.border, width = 1 },
                  corner_radius = 6,
                  padding = { top = 4, bottom = 4, left = 8, right = 8 },
                  children = {
                    wisp.ui.text(state.last_action, { size = 11, color = colors.text_primary })
                  }
                })
              }
            }),

            -- Action Buttons Row 1: Contained & Outlined
            wisp.layout.row({
              gap = 8,
              children = {
                wisp.layout.container({
                  sizing = { width = "grow" },
                  background = colors.primary_main,
                  corner_radius = 8,
                  padding = { top = 7, bottom = 7, left = 12, right = 12 },
                  align = { x = "center", y = "center" },
                  children = {
                    wisp.ui.text("+ Increment", { size = 12, color = "#FFFFFF" })
                  }
                }),
                wisp.layout.container({
                  sizing = { width = "grow" },
                  background = "#00000000",
                  border = { color = colors.primary, width = 1 },
                  corner_radius = 8,
                  padding = { top = 7, bottom = 7, left = 12, right = 12 },
                  align = { x = "center", y = "center" },
                  children = {
                    wisp.ui.text("- Decrement", { size = 12, color = colors.primary })
                  }
                })
              }
            }),

            -- Action Buttons Row 2: Reset Button & Toggle Switch
            wisp.layout.row({
              align = { y = "center" },
              gap = 8,
              children = {
                wisp.layout.container({
                  sizing = { width = "grow" },
                  background = colors.surface,
                  border = { color = colors.border, width = 1 },
                  corner_radius = 8,
                  padding = { top = 6, bottom = 6, left = 10, right = 10 },
                  align = { x = "center", y = "center" },
                  children = {
                    wisp.ui.text("Reset", { size = 11, color = colors.text_secondary })
                  }
                }),
                wisp.layout.container({
                  sizing = { width = "grow" },
                  background = colors.surface,
                  border = { color = colors.border, width = 1 },
                  corner_radius = 8,
                  padding = { top = 5, bottom = 5, left = 8, right = 8 },
                  align = { y = "center" },
                  children = {
                    wisp.layout.row({
                      align = { y = "center" },
                      children = {
                        wisp.ui.text("Toggle", { size = 11, color = colors.text_secondary }),
                        wisp.layout.spacer({ flex = 1 }),
                        -- Visual MUI switch
                        wisp.layout.container({
                          width = 32,
                          height = 18,
                          background = state.switch_on and colors.primary_main or "#424242",
                          corner_radius = 9,
                          padding = 2,
                          children = {
                            wisp.layout.row({
                              sizing = { width = "grow", height = "grow" },
                              align = { y = "center" },
                              children = state.switch_on and {
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
                    })
                  }
                })
              }
            })
          }
        })
      }
    })
  end
})
