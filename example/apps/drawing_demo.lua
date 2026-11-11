-- Wisp 2.0 Freehand Vector Drawing Application (Declarative UI)
-- Pointer interaction (press, drag, release), real-time stroke rendering, undo, and brush controls

local strokes = wisp.state({})
local current_color = wisp.state("#89B4FA")
local brush_size = wisp.state(3.0)
local status_msg = wisp.state("Click and drag on canvas to draw freehand vectors")

local palette = {
  "#89B4FA", -- Blue
  "#A6E3A1", -- Green
  "#F9E2AF", -- Yellow
  "#FAB387", -- Peach
  "#F38BA8", -- Red
  "#CBA6F7", -- Mauve
  "#CDD6F4", -- White
}

local active_stroke = nil

local app = wisp.app.create("drawing_demo", {
  title = "Vector Whiteboard & Drawing",
  app_id = "org.wisp.drawing_demo",
  width = 680,
  height = 580,
  type = "floating",
  autostart = true,
})

app:render(function()
  local color_buttons = {}
  for i, col in ipairs(palette) do
    local is_sel = (current_color:get() == col)
    color_buttons[#color_buttons + 1] = wisp.ui.button {
      text = is_sel and "●" or " ",
      on_click = function() current_color:set(col) end,
      style = {
        background = col,
        radius = 12,
        padding = { top = 2, bottom = 2, left = 8, right = 8 },
        border = is_sel and { color = "#FFFFFF", width = 2 } or nil,
      }
    }
  end

  return wisp.ui.col {
    gap = 12,
    padding = 16,
    background = "#11111B",
    sizing = { width = "grow", height = "grow" },

    -- Header
    wisp.ui.row {
      sizing = { width = "grow" },
      align = { y = "center" },
      wisp.ui.label { text = "Vector Freehand Drawing", size = 18, color = "#CDD6F4" },
      wisp.ui.spacer { flex = 1 },
      wisp.ui.label { text = status_msg:get(), size = 12, color = "#A6ADC8" },
    },

    -- Tool Palette & Brush Controls
    wisp.ui.card {
      background = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 10,
      wisp.ui.row {
        gap = 10,
        align = { y = "center" },
        wisp.ui.label { text = "Colors:", size = 12, color = "#BAC2DE" },
        table.unpack(color_buttons),
        wisp.ui.spacer { flex = 1 },
        wisp.ui.label { text = "Brush: " .. math.floor(brush_size:get()) .. "px", size = 12, color = "#BAC2DE" },
        wisp.ui.button {
          text = "-",
          on_click = function() brush_size:update(function(s) return math.max(1, s - 1) end) end,
        },
        wisp.ui.button {
          text = "+",
          on_click = function() brush_size:update(function(s) return math.min(20, s + 1) end) end,
        },
        wisp.ui.button {
          text = "Undo",
          on_click = function()
            local list = strokes:get()
            if #list > 0 then
              local updated = {}
              for i = 1, #list - 1 do updated[i] = list[i] end
              strokes:set(updated)
              status_msg:set("Undid last stroke (remaining: " .. #updated .. ")")
            end
          end,
        },
        wisp.ui.button {
          text = "Clear",
          on_click = function()
            strokes:set({})
            status_msg:set("Canvas cleared")
          end,
        },
      }
    },

    -- Interactive Drawing Canvas
    wisp.ui.card {
      background = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 8,
      sizing = { width = "grow", height = "grow" },
      wisp.ui.canvas {
        id = "draw_canvas",
        width = 640,
        height = 430,
        draw = function(g)
          g:clear("#181825")

          -- Subtle background grid dots
          for x = 20, 620, 25 do
            for y = 20, 410, 25 do
              g:rect(x, y, 1, 1, "#313244")
            end
          end

          -- Render all committed strokes
          local stroke_list = strokes:get()
          for _, s in ipairs(stroke_list) do
            if #s.pts >= 2 then
              g:polyline(s.pts, s.color, s.width)
            elseif #s.pts == 1 then
              g:circle(s.pts[1][1], s.pts[1][2], s.width * 0.5, s.color)
            end
          end

          -- Render currently active in-progress stroke
          if active_stroke and #active_stroke.pts > 0 then
            if #active_stroke.pts >= 2 then
              g:polyline(active_stroke.pts, active_stroke.color, active_stroke.width)
            else
              g:circle(active_stroke.pts[1][1], active_stroke.pts[1][2], active_stroke.width * 0.5, active_stroke.color)
            end
          end
        end,

        on_press = function(x, y, btn)
          active_stroke = {
            color = current_color:get(),
            width = brush_size:get(),
            pts = { { x, y } }
          }
          status_msg:set(string.format("Drawing at (%d, %d)", math.floor(x), math.floor(y)))
          app:redraw()
        end,

        on_drag = function(x, y, dx, dy)
          if active_stroke then
            table.insert(active_stroke.pts, { x, y })
            app:redraw()
          end
        end,

        on_release = function(x, y, btn)
          if active_stroke then
            table.insert(active_stroke.pts, { x, y })
            local list = strokes:get()
            local updated = {}
            for i = 1, #list do updated[i] = list[i] end
            table.insert(updated, active_stroke)
            active_stroke = nil
            strokes:set(updated)
            status_msg:set(string.format("Strokes: %d", #updated))
          end
        end
      }
    }
  }
end)

return app
