-- Wisp 2.0 Reactive Chart Application (Declarative UI)
-- Reactive Line Chart, Bar Chart, Live Data Updates, and Zero-Alloc Redraws

local series = wisp.state({ 35, 62, 48, 85, 72, 95, 60, 80, 45, 90, 75, 110 })
local bar_data = wisp.state({ 45, 78, 52, 90, 65, 84, 40 })
local bar_labels = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" }
local auto_stream = wisp.state(true)
local hover_info = wisp.state("Hover over charts for details")

local app = wisp.app.create("chart_demo", {
  title = "Reactive Vector Charts",
  app_id = "org.wisp.chart_demo",
  width = 680,
  height = 560,
  type = "floating",
  autostart = true,
})

-- Simulate live data streaming
local function push_data_point()
  local cur = series:get()
  local next_val = math.max(20, math.min(130, cur[#cur] + math.random(-25, 30)))
  local updated = {}
  for i = 2, #cur do updated[#updated + 1] = cur[i] end
  updated[#updated + 1] = next_val
  series:set(updated)

  local cur_bars = bar_data:get()
  local updated_bars = {}
  for i = 1, #cur_bars do
    updated_bars[i] = math.max(15, math.min(100, cur_bars[i] + math.random(-10, 10)))
  end
  bar_data:set(updated_bars)
end

app:render(function()
  return wisp.ui.col {
    gap = 12,
    padding = 16,
    background = "#11111B",
    sizing = { width = "grow", height = "grow" },

    -- Header
    wisp.ui.row {
      sizing = { width = "grow" },
      align = { y = "center" },
      wisp.ui.label { text = "Real-Time Reactive Charts", size = 18, color = "#CDD6F4" },
      wisp.ui.spacer { flex = 1 },
      wisp.ui.label { text = hover_info:get(), size = 12, color = "#89B4FA" },
    },

    -- Controls
    wisp.ui.card {
      gap = 12,
      background = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 10,
      wisp.ui.row {
        gap = 10,
        align = { y = "center" },
        wisp.ui.button { text = "Add Point", on_click = function() push_data_point() end },
        wisp.ui.button {
          text = "Shuffle Bars",
          on_click = function()
            local b = {}
            for i = 1, 7 do b[i] = math.random(20, 100) end
            bar_data:set(b)
          end
        },
        wisp.ui.button {
          text = "Reset Stream",
          on_click = function()
            series:set({ 35, 62, 48, 85, 72, 95, 60, 80, 45, 90, 75, 110 })
            bar_data:set({ 45, 78, 52, 90, 65, 84, 40 })
          end
        },
        wisp.ui.spacer { flex = 1 },
        wisp.ui.label { text = "Auto-Stream (via Timer):", size = 12, color = "#BAC2DE" },
        wisp.ui.switch { value = auto_stream },
      }
    },

    -- Charts Grid: Line Chart (Top) and Bar Chart (Bottom)
    wisp.ui.card {
      background = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 12,
      sizing = { width = "grow", height = "grow" },
      wisp.ui.canvas {
        id = "charts_canvas",
        width = 640,
        height = 430,
        draw = function(g)
          g:clear("#181825")

          -- ==================== 1. LINE CHART ====================
          local lx = 40
          local ly = 25
          local lw = 570
          local lh = 170

          -- Title
          g:text(lx, ly - 8, "SYSTEM TELEMETRY (LIVE LINE CHART)", "#CDD6F4", 12)

          -- Background grid lines
          for i = 0, 4 do
            local gy = ly + (lh / 4) * i
            local val = math.floor(140 - (140 / 4) * i)
            g:line(lx, gy, lx + lw, gy, "#313244", 1)
            g:text(lx - 28, gy - 5, tostring(val), "#6C7086", 10)
          end

          local data = series:get()
          local n = #data
          if n >= 2 then
            local step = lw / (n - 1)
            local pts = {}
            for i = 1, n do
              local px = lx + (i - 1) * step
              local py = ly + lh - (data[i] / 140) * lh
              pts[#pts + 1] = { px, py }
            end

            -- Gradient area fill under curve
            g:path(function(p)
              p:move_to(pts[1][1], ly + lh)
              for i = 1, #pts do p:line_to(pts[i][1], pts[i][2]) end
              p:line_to(pts[#pts][1], ly + lh)
              p:close()
            end, { fill = "#89B4FA26" })

            -- Smooth connecting line
            g:polyline(pts, "#89B4FA", 2.5)

            -- Data point markers
            for i = 1, #pts do
              g:circle(pts[i][1], pts[i][2], 4, "#181825")
              g:circle(pts[i][1], pts[i][2], 2.5, "#89B4FA")
            end
          end

          -- ==================== 2. BAR CHART ====================
          local bx = 40
          local by = 245
          local bw = 570
          local bh = 150

          -- Title
          g:text(bx, by - 8, "WEEKLY RESOURCE UTILIZATION (BAR CHART)", "#CDD6F4", 12)

          -- Grid
          for i = 0, 3 do
            local gy = by + (bh / 3) * i
            local val = math.floor(100 - (100 / 3) * i)
            g:line(bx, gy, bx + bw, gy, "#313244", 1)
            g:text(bx - 26, gy - 5, val .. "%", "#6C7086", 10)
          end

          local bars = bar_data:get()
          local bar_count = #bars
          local col_width = bw / bar_count
          local pad = 14
          local actual_w = col_width - pad * 2

          local colors = { "#A6E3A1", "#94E2D5", "#89DCEB", "#74C7EC", "#89B4FA", "#B4BEFE", "#CBA6F7" }

          for i = 1, bar_count do
            local val = bars[i]
            local h = (val / 100) * bh
            local x = bx + (i - 1) * col_width + pad
            local y = by + bh - h

            -- Bar body with rounded top
            g:round_rect(x, y, actual_w, h, 4, colors[((i - 1) % #colors) + 1])

            -- Bar value text above
            g:text(x + actual_w * 0.5 - 8, y - 12, tostring(val), "#CDD6F4", 10)

            -- Label below
            local lbl = bar_labels[i] or tostring(i)
            g:text(x + actual_w * 0.5 - 10, by + bh + 14, lbl, "#A6ADC8", 11)
          end
        end,
        on_hover = function(x, y)
          hover_info:set(string.format("Cursor at (%d, %d)", math.floor(x), math.floor(y)))
        end
      }
    }
  }
end)

wisp.timer.every(2000, function()
  if auto_stream:get() then
    push_data_point()
  end
end)

return app
