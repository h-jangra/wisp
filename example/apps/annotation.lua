-- Wisp 2.0 Native Screen Capture & Vector Annotation Application (Declarative UI)

local app_width = 960
local app_height = 640

local capture_state = wisp.state("capturing")
local status_msg = wisp.state("Capturing screen...")
local current_tool = wisp.state("pen")
local current_color = wisp.state("#F38BA8")
local brush_size = wisp.state(3.0)
local text_value = wisp.state("Note")

local annotations = wisp.state({})
local redo_stack = wisp.state({})

local captured_image = nil
local active_shape = nil

local palette = {
  "#F38BA8", -- Red
  "#FAB387", -- Peach
  "#F9E2AF", -- Yellow
  "#A6E3A1", -- Green
  "#89B4FA", -- Blue
  "#CBA6F7", -- Mauve
  "#CDD6F4", -- White
  "#11111B", -- Black
}

local app = wisp.app.create("annotation", {
  title = "Screen Annotation & Markup",
  app_id = "org.wisp.annotation",
  width = app_width,
  height = app_height,
  type = "floating",
  autostart = true,
})

wisp.screen.capture({}, function(image, err)
  if image then
    captured_image = image
    capture_state:set("ready")
    status_msg:set(string.format("Captured %dx%d (%s) - Ready to annotate",
      image:width(), image:height(), image:format()))
  else
    capture_state:set("error")
    status_msg:set("Screen capture error: " .. tostring(err or "failed"))
  end
  app:redraw()
end)

local function undo_annotation()
  local list = annotations:get()
  if #list > 0 then
    local updated = {}
    for i = 1, #list - 1 do updated[i] = list[i] end
    local popped = list[#list]
    local redos = redo_stack:get()
    local updated_redos = {}
    for i = 1, #redos do updated_redos[i] = redos[i] end
    table.insert(updated_redos, popped)
    annotations:set(updated)
    redo_stack:set(updated_redos)
    app:redraw()
  end
end

local function redo_annotation()
  local redos = redo_stack:get()
  if #redos > 0 then
    local updated_redos = {}
    for i = 1, #redos - 1 do updated_redos[i] = redos[i] end
    local item = redos[#redos]
    local list = annotations:get()
    local updated = {}
    for i = 1, #list do updated[i] = list[i] end
    table.insert(updated, item)
    annotations:set(updated)
    redo_stack:set(updated_redos)
    app:redraw()
  end
end

local function clear_annotations()
  annotations:set({})
  redo_stack:set({})
  app:redraw()
end

wisp.keymap.set("<Esc>", function()
  if captured_image then
    captured_image:destroy()
    captured_image = nil
  end
  app:close()
end)

wisp.keymap.set("<C-z>", undo_annotation)
wisp.keymap.set("<C-y>", redo_annotation)

app:render(function()
  local color_buttons = {}
  for _, col in ipairs(palette) do
    local is_active = (col == current_color:get())
    table.insert(color_buttons, wisp.ui.button {
      text = is_active and "●" or "",
      bg = col,
      width = 24,
      height = 24,
      radius = 12,
      on_click = function()
        current_color:set(col)
        app:redraw()
      end
    })
  end

  local tools = { "pen", "rect", "circle", "arrow", "text" }
  local tool_buttons = {}
  for _, tname in ipairs(tools) do
    local is_sel = (tname == current_tool:get())
    table.insert(tool_buttons, wisp.ui.button {
      id = "tool_" .. tname,
      text = string.upper(string.sub(tname, 1, 1)) .. string.sub(tname, 2),
      bg = is_sel and "#89B4FA" or "#313244",
      color = is_sel and "#11111B" or "#CDD6F4",
      radius = 6,
      on_click = function()
        current_tool:set(tname)
        app:redraw()
      end
    })
  end

  local c_w = app_width - 24
  local c_h = app_height - 110

  return wisp.ui.col {
    gap = 8,
    padding = 12,
    bg = "#11111B",

    -- Header & Status
    wisp.ui.row {
      wisp.ui.label { text = "Annotation & Markup Tool", size = 15, color = "#CDD6F4" },
      wisp.ui.spacer(12),
      wisp.ui.label { text = status_msg:get(), size = 11, color = "#A6ADC8" },
      wisp.ui.spacer(10),
      wisp.ui.button {
        id = "btn_undo",
        text = "Undo",
        on_click = undo_annotation
      },
      wisp.ui.button {
        id = "btn_redo",
        text = "Redo",
        on_click = redo_annotation
      },
      wisp.ui.button {
        id = "btn_clear",
        text = "Clear",
        on_click = clear_annotations
      }
    },

    -- Toolbar Card
    wisp.ui.card {
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 8,
      gap = 8,
      wisp.ui.row {
        gap = 6,
        wisp.ui.row(table.unpack(tool_buttons)),
        wisp.ui.spacer(12),
        wisp.ui.row(table.unpack(color_buttons))
      }
    },

    -- Canvas Area
    wisp.ui.card {
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 4,
      wisp.ui.canvas {
        id = "annotation_canvas",
        width = c_w,
        height = c_h,
        draw = function(g)
          g:clear("#181825")
          if captured_image then
            g:image(captured_image, 0, 0, c_w, c_h)
          end

          local function draw_shape(s)
            if s.type == "pen" then
              if #s.pts >= 2 then
                g:polyline(s.pts, s.color, s.width)
              elseif #s.pts == 1 then
                g:circle(s.pts[1][1], s.pts[1][2], s.width * 0.5, s.color)
              end
            elseif s.type == "rect" then
              local rx = math.min(s.x1, s.x2)
              local ry = math.min(s.y1, s.y2)
              local rw = math.abs(s.x2 - s.x1)
              local rh = math.abs(s.y2 - s.y1)
              g:rect(rx, ry, rw, rh, s.color, s.width)
            elseif s.type == "circle" then
              local cx = (s.x1 + s.x2) * 0.5
              local cy = (s.y1 + s.y2) * 0.5
              local r = math.sqrt((s.x2 - s.x1)^2 + (s.y2 - s.y1)^2) * 0.5
              g:circle(cx, cy, r, s.color, s.width)
            elseif s.type == "text" then
              g:text(s.text or "", s.x, s.y, s.color, 16)
            end
          end

          local list = annotations:get()
          for _, s in ipairs(list) do
            draw_shape(s)
          end
          if active_shape then
            draw_shape(active_shape)
          end
        end
      }
    }
  }
end)

return app
