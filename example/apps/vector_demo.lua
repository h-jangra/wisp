-- Wisp Native Vector Graphics Demo
-- Demonstrates Milestone 4: Native Vector Graphics API
-- Shapes, Paths, Curves, Transformations, Clipping, and Gradients

local rotation_angle = wisp.state(0.0)
local scale_factor = wisp.state(1.0)
local show_clipped = wisp.state(true)
local current_shape = wisp.state("all")

local app = wisp.app.create("vector_demo", {
  title = "Vector Graphics Demo",
  app_id = "org.wisp.vector_demo",
  width = 640,
  height = 540,
  type = "floating",
  autostart = true,
})

app:render(function(ui)
  return ui:column({ gap = 12, padding = 16, background = "#11111B", sizing = { width = "grow", height = "grow" } }, function()
    -- Title Header
    ui:row({ sizing = { width = "grow" }, align = { y = "center" } }, function()
      ui:text("Wisp Native Vector Graphics", { size = 16, color = "#CDD6F4" })
      ui:spacer({ flex = 1 })
      ui:text("Zero external dependencies", { size = 11, color = "#A6ADC8" })
    end)

    -- Interactive Control Bar
    ui:card({ gap = 10, background = "#181825", border = { color = "#313244", width = 1 }, radius = 8, padding = 10 }, function()
      ui:row({ gap = 12, align = { y = "center" } }, function()
        ui:text("Rotate: " .. math.floor(rotation_angle:get() * 57.2958) .. "°", { size = 12, color = "#BAC2DE" })
        ui:button("-15°", function() rotation_angle:update(function(a) return a - 0.2618 end) end, { id = "rot_dec" })
        ui:button("+15°", function() rotation_angle:update(function(a) return a + 0.2618 end) end, { id = "rot_inc" })
        ui:button("Reset", function() rotation_angle:set(0.0); scale_factor:set(1.0) end, { id = "rot_reset" })

        ui:spacer({ flex = 1 })

        ui:text("Clip Preview:", { size = 12, color = "#BAC2DE" })
        ui:switch({ value = show_clipped })
      end)
    end)

    -- Vector Canvas
    ui:card({ background = "#181825", border = { color = "#313244", width = 1 }, radius = 8, padding = 8, sizing = { width = "grow", height = "grow" } }, function()
      ui:canvas({
        id = "vg_canvas",
        width = 600,
        height = 420,
        draw = function(g)
          -- Clear canvas background
          g:clear("#181825")

          -- 1. Linear & Radial Gradients (cheap native fill)
          g:gradient(10, 10, 180, 50, "#89B4FA", "#CBA6F7", false, 0)
          g:text(20, 28, "Linear Gradient", "#11111B", 13)

          g:gradient(205, 10, 180, 50, "#F38BA8", "#FAB387", true, 80)
          g:text(215, 28, "Radial Gradient", "#11111B", 13)

          -- 2. Basic Primitives
          -- Filled rect and outline
          g:rect(10, 75, 80, 50, "#313244")
          g:rect_outline(10, 75, 80, 50, "#89B4FA", 2)
          g:text(18, 95, "Rectangle", "#CDD6F4", 11)

          -- Rounded rect
          g:round_rect(105, 75, 80, 50, 10, "#45475A")
          g:text(115, 95, "Rounded", "#CDD6F4", 11)

          -- Circle & Ellipse
          g:circle(235, 100, 25, "#A6E3A1")
          g:text(220, 95, "Circle", "#11111B", 10)

          g:ellipse(310, 100, 35, 22, "#F9E2AF")
          g:text(293, 95, "Ellipse", "#11111B", 10)

          -- Lines & Polylines
          g:line(10, 140, 380, 140, "#45475A", 1.5)
          g:polyline({ {10, 160}, {40, 180}, {70, 150}, {100, 175}, {130, 155} }, "#89B4FA", 2.5)
          g:text(10, 190, "Polyline", "#BAC2DE", 11)

          -- Convex Polygon
          g:polygon({ {170, 185}, {200, 150}, {230, 185}, {215, 205}, {185, 205} }, "#CBA6F7", true)
          g:polygon({ {170, 185}, {200, 150}, {230, 185}, {215, 205}, {185, 205} }, "#FFFFFF", false)
          g:text(180, 178, "Polygon", "#11111B", 10)

          -- 3. Path API: Quadratic and Cubic Bezier Curves
          g:path(function(p)
            p:move_to(260, 200)
            p:quad_to(310, 140, 360, 200)
            p:close()
          end, { fill = "#F38BA855", stroke = "#F38BA8", stroke_width = 2 })
          g:text(285, 185, "Quad Curve", "#F38BA8", 10)

          g:path(function(p)
            p:move_to(10, 270)
            p:bezier_to(50, 210, 90, 310, 130, 250)
            p:bezier_to(170, 190, 210, 290, 250, 240)
            p:line_to(250, 290)
            p:line_to(10, 290)
            p:close()
          end, { fill = "#89DCEB44", stroke = "#89DCEB", stroke_width = 2 })
          g:text(15, 280, "Cubic Bezier Path Wave", "#89DCEB", 11)

          -- 4. Dynamic Transformations (Matrix Stack: Translate, Scale, Rotate)
          local angle = rotation_angle:get()
          g:save()
          g:translate(480, 130)
          g:rotate(angle)
          g:scale(scale_factor:get())

          -- Draw a transformed gear/star shape
          g:path(function(p)
            local points = 8
            local r_outer = 45
            local r_inner = 22
            for i = 0, points * 2 - 1 do
              local a = (i * math.pi) / points
              local r = (i % 2 == 0) and r_outer or r_inner
              local px = math.cos(a) * r
              local py = math.sin(a) * r
              if i == 0 then p:move_to(px, py) else p:line_to(px, py) end
            end
            p:close()
          end, { fill = "#FAB387", stroke = "#11111B", stroke_width = 2 })
          g:circle(0, 0, 10, "#181825")
          g:restore()
          g:text(435, 195, "Transform: Rotate & Scale", "#BAC2DE", 10)

          -- 5. Rectangular Clipping Region
          if show_clipped:get() then
            g:save()
            g:clip(320, 230, 260, 100)
            g:rect(320, 230, 260, 100, "#1E1E2E")
            g:rect_outline(320, 230, 260, 100, "#A6E3A1", 1)

            -- Circles overflowing the clip boundary
            for i = 1, 6 do
              g:circle(310 + i * 40, 280, 30, (i % 2 == 0) and "#89B4FA88" or "#F38BA888")
            end
            g:text(330, 245, "Clipped Viewport (bounds 260x100)", "#A6E3A1", 11)
            g:restore()
          end

          -- Footer text
          g:text(10, 395, "Native scanline rasterization + EGL/GLES2 hardware accelerated paths", "#6C7086", 11)
        end
      })
    end)
  end)
end)
