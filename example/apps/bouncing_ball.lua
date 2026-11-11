-- Wisp 2.0 Bouncing Ball (Immediate-Mode 2D Drawing)

wisp.opt.title = "Bouncing Ball"
wisp.opt.size = { 640, 480 }
wisp.opt.fps = 60

local x, y = 320, 240
local vx, vy = 280, 210
local radius = 24
local trail = {}

function wisp.load()
    -- Initialize on load
end

function wisp.update(dt)
    x = x + vx * dt
    y = y + vy * dt

    local w = wisp.graphics.getWidth()
    local h = wisp.graphics.getHeight()

    if x - radius < 0 then
        x = radius
        vx = math.abs(vx)
    elseif x + radius > w then
        x = w - radius
        vx = -math.abs(vx)
    end

    if y - radius < 0 then
        y = radius
        vy = math.abs(vy)
    elseif y + radius > h then
        y = h - radius
        vy = -math.abs(vy)
    end

    table.insert(trail, 1, { x = x, y = y })
    if #trail > 12 then
        table.remove(trail)
    end
end

function wisp.draw()
    wisp.graphics.clear(0.09, 0.09, 0.14, 1.0)

    -- Draw trail
    for i, pt in ipairs(trail) do
        local alpha = (1.0 - (i / #trail)) * 0.4
        local r = radius * (1.0 - (i / #trail) * 0.5)
        wisp.graphics.setColor(0.54, 0.71, 0.98, alpha)
        wisp.graphics.circle("fill", pt.x, pt.y, r)
    end

    -- Draw main ball
    wisp.graphics.setColor(0.95, 0.55, 0.66, 1.0)
    wisp.graphics.circle("fill", x, y, radius)

    -- Draw border
    wisp.graphics.setColor(1.0, 1.0, 1.0, 0.8)
    wisp.graphics.setLineWidth(2.0)
    wisp.graphics.circle("line", x, y, radius)

    -- Draw status text
    wisp.graphics.setColor(0.8, 0.84, 0.96, 1.0)
    wisp.graphics.print("Wisp 2.0 Immediate Drawing | Press <C-q> to quit", 20, 20, { size = 16 })
end

wisp.keymap.set("<C-q>", function()
    wisp.app.quit()
end)

wisp.keymap.set("<Space>", function()
    vx = vx * 1.2
    vy = vy * 1.2
end)
