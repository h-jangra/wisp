local wisp = wisp

state.selected_tile = state.selected_tile or 1

wisp.widget.create({
  name = "nested_layouts",
  size = { width = 280, height = 170 },
  anchor = "top_left",
  margin = { top = 210, left = 16 },

  on_click = function(x, y)
    -- 2x2 grid hit testing
    local col = (x < 140) and 1 or 2
    local row = (y < 105) and 1 or 2
    state.selected_tile = (row - 1) * 2 + col
  end,

  on_scroll = function(dir, x, y)
    -- Scrolling cycles through tiles
    if dir > 0 then
      state.selected_tile = (state.selected_tile % 4) + 1
    else
      state.selected_tile = (state.selected_tile - 2 + 4) % 4 + 1
    end
  end,

  build = function()
    local cpu = wisp.system.cpu()
    local mem = wisp.system.memory()

    local tiles = {
      { label = "CPU", val = string.format("%.1f%%", cpu.usage), color = "#90CAF9" },
      { label = "RAM", val = string.format("%.0f%%", mem.percentage), color = "#CE93D8" },
      { label = "DISK", val = "42%", color = "#66BB6A" },
      { label = "NET", val = "1.2 MB/s", color = "#FFA726" }
    }

    local function make_tile(i)
      local t = tiles[i]
      local is_sel = (state.selected_tile == i)
      return wisp.layout.container({
        background = is_sel and "#263238" or "#242424",
        border = { color = is_sel and t.color or "#383838", width = is_sel and 2 or 1 },
        padding = 8,
        corner_radius = 8,
        sizing = { width = "grow" },
        children = {
          wisp.layout.column({
            gap = 2,
            children = {
              wisp.ui.text(t.label, { size = 10, color = "#B0BEC5" }),
              wisp.ui.text(t.val, { size = 13, color = is_sel and "#FFFFFF" or t.color })
            }
          })
        }
      })
    end

    return wisp.layout.container({
      background = "#1E1E1EEE",
      border = { color = "#383838", width = 1 },
      corner_radius = 12,
      padding = 12,
      sizing = { width = "grow", height = "grow" },
      children = {
        wisp.layout.column({
          gap = 8,
          children = {
            -- Header
            wisp.layout.row({
              align = { y = "center" },
              children = {
                wisp.ui.text("MUI Responsive Grid", { size = 13, color = "#FFFFFF" }),
                wisp.layout.spacer({ flex = 1 }),
                wisp.layout.container({
                  background = "#2D2D2D",
                  corner_radius = 10,
                  padding = { top = 2, bottom = 2, left = 6, right = 6 },
                  children = {
                    wisp.ui.text(string.format("Tile #%d", state.selected_tile), { size = 10, color = "#90CAF9" })
                  }
                })
              }
            }),

            wisp.ui.separator({ color = "#383838", thickness = 1 }),

            -- 2x2 Grid (Row 1)
            wisp.layout.row({
              gap = 8,
              children = {
                make_tile(1),
                make_tile(2)
              }
            }),

            -- 2x2 Grid (Row 2)
            wisp.layout.row({
              gap = 8,
              children = {
                make_tile(3),
                make_tile(4)
              }
            })
          }
        })
      }
    })
  end
})

wisp.timer.every(2000, function() end)
