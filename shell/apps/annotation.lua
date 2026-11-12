-- Wisp Native Screen Annotation & Markup Application

local app_width = 960
local app_height = 640

local status_msg = wisp.state("Capturing screen...")
local current_tool = wisp.state("pen")
local current_color = wisp.state("#F38BA8")
local brush_size = wisp.state(3.0)
local text_prompt = wisp.state("Annotation")

local annotations = wisp.state({})
local redo_stack = wisp.state({})

local captured_image = nil
local active_shape = nil
local canvas_w = app_width - 24
local canvas_h = app_height - 120

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

local app = wisp.app("annotation", {
  title = "Annotation",
  width = app_width,
  height = app_height,
})

-- Screen capture
wisp.screen.capture({}, function(image, err)
  if image then
    captured_image = image
    status_msg:set(string.format("Captured %dx%d - Ready to annotate",
      image:width(), image:height()))
  else
    status_msg:set("Screen capture unavailable: " .. tostring(err or "error"))
  end
  app:redraw()
end)

local function undo()
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

local function redo()
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

local function clear()
  annotations:set({})
  redo_stack:set({})
  active_shape = nil
  app:redraw()
end

local function save()
  if not captured_image then
    status_msg:set("No screenshot to save")
    app:redraw()
    return
  end

  local export_img = captured_image:clone()
  if not export_img then
    status_msg:set("Failed to clone screenshot buffer")
    app:redraw()
    return
  end

  local iw = export_img:width()
  local ih = export_img:height()
  local sx = (canvas_w > 0) and (iw / canvas_w) or 1.0
  local sy = (canvas_h > 0) and (ih / canvas_h) or 1.0

  local list = annotations:get()
  export_img:render(function(g)
    for _, s in ipairs(list) do
      local sw = (s.width or 2.0) * sx
      if s.type == "pen" and s.pts and #s.pts >= 2 then
        local scaled_pts = {}
        for _, pt in ipairs(s.pts) do
          table.insert(scaled_pts, { pt[1] * sx, pt[2] * sy })
        end
        g:polyline(scaled_pts, s.color, sw)
      elseif s.type == "pen" and s.pts and #s.pts == 1 then
        g:circle(s.pts[1][1] * sx, s.pts[1][2] * sy, sw * 0.5, s.color)
      elseif s.type == "rect" then
        local rx = math.min(s.x1, s.x2) * sx
        local ry = math.min(s.y1, s.y2) * sy
        local rw = math.abs(s.x2 - s.x1) * sx
        local rh = math.abs(s.y2 - s.y1) * sy
        g:rect(rx, ry, rw, rh, s.color, sw)
      elseif s.type == "circle" then
        local cx = (s.x1 + s.x2) * 0.5 * sx
        local cy = (s.y1 + s.y2) * 0.5 * sy
        local r = math.sqrt(((s.x2 - s.x1) * sx)^2 + ((s.y2 - s.y1) * sy)^2) * 0.5
        g:circle(cx, cy, r, s.color, sw)
      elseif s.type == "arrow" then
        g:arrow(s.x1 * sx, s.y1 * sy, s.x2 * sx, s.y2 * sy, s.color, sw, sw * 4.0)
      elseif s.type == "text" then
        g:text(s.x * sx, s.y * sy, s.text or "", s.color, 16.0 * sx)
      end
    end
  end)

  local out_path = os.getenv("HOME") .. "/Pictures/annotation_" .. os.date("%Y%m%d_%H%M%S") .. ".png"
  local ok, err = export_img:save(out_path)
  if not ok then
    out_path = "/tmp/annotation_" .. os.date("%Y%m%d_%H%M%S") .. ".png"
    ok, err = export_img:save(out_path)
  end
  export_img:destroy()

  if ok then
    status_msg:set("Saved: " .. out_path)
  else
    status_msg:set("Save error: " .. tostring(err or "failed"))
  end
  app:redraw()
end

local function draw_single_shape(g, s)
  local w = s.width or 2.0
  if s.type == "pen" and s.pts then
    if #s.pts >= 2 then
      g:polyline(s.pts, s.color, w)
    elseif #s.pts == 1 then
      g:circle(s.pts[1][1], s.pts[1][2], w * 0.5, s.color)
    end
  elseif s.type == "rect" then
    local rx = math.min(s.x1, s.x2)
    local ry = math.min(s.y1, s.y2)
    local rw = math.abs(s.x2 - s.x1)
    local rh = math.abs(s.y2 - s.y1)
    g:rect(rx, ry, rw, rh, s.color, w)
  elseif s.type == "circle" then
    local cx = (s.x1 + s.x2) * 0.5
    local cy = (s.y1 + s.y2) * 0.5
    local r = math.sqrt((s.x2 - s.x1)^2 + (s.y2 - s.y1)^2) * 0.5
    g:circle(cx, cy, r, s.color, w)
  elseif s.type == "arrow" then
    g:arrow(s.x1, s.y1, s.x2, s.y2, s.color, w, w * 4.0)
  elseif s.type == "text" then
    g:text(s.x, s.y, s.text or "", s.color, 16.0)
  end
end

local function draw(g)
  g:clear("#181825")
  if captured_image then
    g:image(captured_image, 0, 0, canvas_w, canvas_h)
  end

  local list = annotations:get()
  for _, s in ipairs(list) do
    draw_single_shape(g, s)
  end
  if active_shape then
    draw_single_shape(g, active_shape)
  end
end

local function on_press(x, y, btn)
  local tool = current_tool:get()
  local col = current_color:get()
  local bsize = brush_size:get()

  if tool == "pen" then
    active_shape = {
      type = "pen",
      color = col,
      width = bsize,
      pts = { { x, y } }
    }
  elseif tool == "rect" or tool == "circle" or tool == "arrow" then
    active_shape = {
      type = tool,
      color = col,
      width = bsize,
      x1 = x,
      y1 = y,
      x2 = x,
      y2 = y
    }
  elseif tool == "text" then
    local list = annotations:get()
    local updated = {}
    for i = 1, #list do updated[i] = list[i] end
    table.insert(updated, {
      type = "text",
      color = col,
      text = text_prompt:get(),
      x = x,
      y = y
    })
    annotations:set(updated)
    redo_stack:set({})
  end
  app:redraw()
end

local function on_drag(x, y, dx, dy)
  if not active_shape then return end
  if active_shape.type == "pen" then
    table.insert(active_shape.pts, { x, y })
  else
    active_shape.x2 = x
    active_shape.y2 = y
  end
  app:redraw()
end

local function on_release(x, y, btn)
  if not active_shape then return end
  if active_shape.type == "pen" then
    table.insert(active_shape.pts, { x, y })
  else
    active_shape.x2 = x
    active_shape.y2 = y
  end
  local list = annotations:get()
  local updated = {}
  for i = 1, #list do updated[i] = list[i] end
  table.insert(updated, active_shape)
  annotations:set(updated)
  redo_stack:set({})
  active_shape = nil
  app:redraw()
end

-- Keyboard shortcuts
wisp.keymap.set("<Esc>", function()
  if captured_image then
    captured_image:destroy()
    captured_image = nil
  end
  app.close()
end)
wisp.keymap.set("<C-z>", undo)
wisp.keymap.set("<C-y>", redo)
wisp.keymap.set("<C-s>", save)
wisp.keymap.set("1", function() current_tool:set("pen"); app:redraw() end)
wisp.keymap.set("2", function() current_tool:set("rect"); app:redraw() end)
wisp.keymap.set("3", function() current_tool:set("circle"); app:redraw() end)
wisp.keymap.set("4", function() current_tool:set("arrow"); app:redraw() end)
wisp.keymap.set("5", function() current_tool:set("text"); app:redraw() end)

app:build(function(ui)
  ui.column({ gap = 8, padding = 8, bg = "#11111B" }, function()
    -- Top bar: title, status, actions
    ui.row({ gap = 8 }, function()
      ui.text("Annotation", { color = "#CDD6F4", size = 15 })
      ui.spacer(8)
      ui.text(status_msg:get(), { color = "#A6ADC8", size = 11 })
      ui.spacer(8)
      ui.button("Save", save)
      ui.button("Undo", undo)
      ui.button("Redo", redo)
      ui.button("Clear", clear)
      ui.button("Close", app.close)
    end)

    -- Toolbar: tools, brush size, palette
    ui.card({ bg = "#181825", padding = 6, gap = 6 }, function()
      ui.row({ gap = 6 }, function()
        -- Tool selector
        for _, tname in ipairs({ "pen", "rect", "circle", "arrow", "text" }) do
          local is_sel = (tname == current_tool:get())
          ui.button(string.upper(string.sub(tname, 1, 1)) .. string.sub(tname, 2), {
            bg = is_sel and "#89B4FA" or "#313244",
            color = is_sel and "#11111B" or "#CDD6F4",
            radius = 4,
            on_click = function()
              current_tool:set(tname)
              app:redraw()
            end
          })
        end

        ui.spacer(8)
        ui.text("Size:", { color = "#A6ADC8", size = 12 })
        ui.button("-", function()
          local sz = math.max(1.0, brush_size:get() - 1.0)
          brush_size:set(sz)
          app:redraw()
        end)
        ui.text(string.format("%d", math.floor(brush_size:get())), { color = "#CDD6F4", size = 12 })
        ui.button("+", function()
          local sz = math.min(20.0, brush_size:get() + 1.0)
          brush_size:set(sz)
          app:redraw()
        end)

        ui.spacer(12)
        -- Palette
        for _, col in ipairs(palette) do
          local is_active = (col == current_color:get())
          ui.button(is_active and "●" or "", {
            bg = col,
            width = 22,
            height = 22,
            radius = 11,
            on_click = function()
              current_color:set(col)
              app:redraw()
            end
          })
        end
      end)
    end)

    -- Canvas Area
    ui.card({ bg = "#181825", padding = 4 }, function()
      ui.canvas({
        id = "annotation_canvas",
        width = canvas_w,
        height = canvas_h,
        draw = draw,
        on_press = on_press,
        on_drag = on_drag,
        on_release = on_release,
      })
    end)
  end)
end)

return app:run()
