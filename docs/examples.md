# Practical Widget & Application Examples

This guide contains complete, runnable examples for Wisp 2.0 demonstrating both **Immediate-Mode 2D Graphics** (`wisp.draw`) and **Declarative UI** (`wisp.render`) with modern styling, reactive state, and full Wayland interactivity.

Each example can be launched directly with:

```bash
# Launch a standalone application:
./build/wisp shell/apps/calculator.lua
./build/wisp example/apps/bouncing_ball.lua
./build/wisp shell/apps/todo.lua
./build/wisp example/apps/reactive_counter.lua
./build/wisp shell/apps/annotation.lua
```

---

## 1. Wisp Desktop Calculator (`calculator.lua`) — Declarative UI

A clean desktop calculator using declarative UI tables, reactive state, and keyboard shortcuts:

```lua
local state = wisp.state({
    display = "0",
    history = "",
    prev_num = nil,
    op = nil,
    clear_on_next = false,
})

local function input_digit(d)
    if state.clear_on_next or state.display == "0" then
        state.display = tostring(d)
        state.clear_on_next = false
    elseif #state.display < 14 then
        state.display = state.display .. tostring(d)
    end
end

local function compute()
    if state.op and state.prev_num then
        local current = tonumber(state.display) or 0
        local result = 0
        if state.op == "+" then result = state.prev_num + current
        elseif state.op == "−" then result = state.prev_num - current
        elseif state.op == "×" then result = state.prev_num * current
        elseif state.op == "÷" then
            result = (current == 0) and "Error" or (state.prev_num / current)
        end
        state.history = state.prev_num .. " " .. state.op .. " " .. current .. " ="
        state.display = tostring(result)
        state.prev_num = nil
        state.op = nil
        state.clear_on_next = true
    end
end

function wisp.load()
    wisp.opt.title = "Calculator"
    wisp.opt.width = 340
    wisp.opt.height = 480
    wisp.opt.background = "#181825"
    wisp.keymap.set("<C-q>", function() wisp.app.quit() end)
end

function wisp.render()
    return wisp.ui.col {
        padding = 16,
        gap = 12,
        background = "#181825",
        sizing = { width = "grow", height = "grow" },

        -- Display Screen
        wisp.ui.card {
            background = "#11111B",
            border = { color = "#313244", width = 1 },
            radius = 8,
            padding = 14,
            gap = 4,
            wisp.ui.label { text = state.history ~= "" and state.history or " ", size = 12, color = "#6C7086" },
            wisp.ui.label { text = state.display, size = 32, color = "#CDD6F4" },
        },

        -- Keypad Rows
        wisp.ui.row {
            gap = 8,
            wisp.ui.button { text = "C", on_click = function() state.display = "0"; state.history = "" end },
            wisp.ui.button { text = "±", on_click = function() state.display = tostring(-(tonumber(state.display) or 0)) end },
            wisp.ui.button { text = "%", on_click = function() state.display = tostring((tonumber(state.display) or 0) / 100) end },
            wisp.ui.button { text = "÷", on_click = function() state.op = "÷"; state.prev_num = tonumber(state.display); state.clear_on_next = true end },
        },
        wisp.ui.row {
            gap = 8,
            wisp.ui.button { text = "7", on_click = function() input_digit(7) end },
            wisp.ui.button { text = "8", on_click = function() input_digit(8) end },
            wisp.ui.button { text = "9", on_click = function() input_digit(9) end },
            wisp.ui.button { text = "×", on_click = function() state.op = "×"; state.prev_num = tonumber(state.display); state.clear_on_next = true end },
        },
        wisp.ui.row {
            gap = 8,
            wisp.ui.button { text = "4", on_click = function() input_digit(4) end },
            wisp.ui.button { text = "5", on_click = function() input_digit(5) end },
            wisp.ui.button { text = "6", on_click = function() input_digit(6) end },
            wisp.ui.button { text = "−", on_click = function() state.op = "−"; state.prev_num = tonumber(state.display); state.clear_on_next = true end },
        },
        wisp.ui.row {
            gap = 8,
            wisp.ui.button { text = "1", on_click = function() input_digit(1) end },
            wisp.ui.button { text = "2", on_click = function() input_digit(2) end },
            wisp.ui.button { text = "3", on_click = function() input_digit(3) end },
            wisp.ui.button { text = "+", on_click = function() state.op = "+"; state.prev_num = tonumber(state.display); state.clear_on_next = true end },
        },
        wisp.ui.row {
            gap = 8,
            wisp.ui.button { text = "0", on_click = function() input_digit(0) end },
            wisp.ui.button { text = ".", on_click = function() if not state.display:find("%.") then state.display = state.display .. "." end end },
            wisp.ui.button { text = "=", on_click = compute },
        },
    }
end
```

---

## 2. Bouncing Ball Simulation (`bouncing_ball.lua`) — Immediate Mode

A high-performance physics simulation demonstrating LÖVE-style immediate-mode drawing, delta-time updates, and keyboard shortcuts:

```lua
local balls = {}
local gravity = 400
local friction = 0.98

function wisp.load()
    wisp.opt.title = "Wisp 2.0 Physics Simulator"
    wisp.opt.width = 640
    wisp.opt.height = 480
    wisp.opt.fps = 60

    wisp.keymap.set("<C-q>", function() wisp.app.quit() end)
    wisp.keymap.set("<space>", function()
        balls[#balls + 1] = {
            x = math.random(50, 590),
            y = 50,
            vx = math.random(-200, 200),
            vy = math.random(-50, 100),
            radius = math.random(12, 24),
            color = "#" .. string.format("%06x", math.random(0, 0xFFFFFF)),
        }
    end)
end

function wisp.update(dt)
    local w = wisp.graphics.getWidth()
    local h = wisp.graphics.getHeight()
    for _, b in ipairs(balls) do
        b.vy = b.vy + gravity * dt
        b.x = b.x + b.vx * dt
        b.y = b.y + b.vy * dt

        if b.x - b.radius < 0 then
            b.x = b.radius
            b.vx = -b.vx * friction
        elseif b.x + b.radius > w then
            b.x = w - b.radius
            b.vx = -b.vx * friction
        end

        if b.y + b.radius > h then
            b.y = h - b.radius
            b.vy = -b.vy * friction
            b.vx = b.vx * friction
        end
    end
end

function wisp.draw()
    wisp.graphics.clear("#181825")
    for _, b in ipairs(balls) do
        wisp.graphics.circle("fill", b.x, b.y, b.radius, b.color)
    end
    wisp.graphics.print("Balls: " .. #balls .. " (Press Space to add, Ctrl+Q to quit)", 16, 20, 14, "#CDD6F4")
end

function wisp.mousepressed(x, y, button)
    balls[#balls + 1] = {
        x = x,
        y = y,
        vx = math.random(-150, 150),
        vy = math.random(-250, -50),
        radius = 16,
        color = "#89B4FA",
    }
end
```

---

## 3. Todo Application (`todo.lua`) — Declarative UI

A task manager demonstrating text input, list rendering, item toggling, and deletion:

```lua
local items = wisp.state({
    { text = "Learn Wisp 2.0 API", done = true },
    { text = "Build Wayland widgets", done = false },
    { text = "Ship desktop application", done = false },
})
local new_text = wisp.state("")

function wisp.load()
    wisp.opt.title = "Wisp Todo"
    wisp.opt.width = 400
    wisp.opt.height = 500
end

function wisp.render()
    local item_views = {}
    local current = items:get()
    for i, it in ipairs(current) do
        item_views[#item_views + 1] = wisp.ui.row {
            gap = 10,
            align = { y = "center" },
            wisp.ui.button {
                text = it.done and "☑" or "☐",
                on_click = function()
                    it.done = not it.done
                    items:set(current)
                end,
            },
            wisp.ui.label {
                text = it.text,
                color = it.done and "#6C7086" or "#CDD6F4",
                size = 14,
            },
            wisp.ui.spacer { flex = 1 },
            wisp.ui.button {
                text = "✕",
                on_click = function()
                    table.remove(current, i)
                    items:set(current)
                end,
            },
        }
    end

    return wisp.ui.col {
        padding = 16,
        gap = 12,
        background = "#181825",
        sizing = { width = "grow", height = "grow" },
        wisp.ui.label { text = "Tasks", size = 20, color = "#CDD6F4" },
        wisp.ui.row {
            gap = 8,
            wisp.ui.input {
                placeholder = "Add a task...",
                value = new_text,
            },
            wisp.ui.button {
                text = "Add",
                on_click = function()
                    local t = new_text:get()
                    if t and #t > 0 then
                        table.insert(current, { text = t, done = false })
                        items:set(current)
                        new_text:set("")
                    end
                end,
            },
        },
        wisp.ui.separator {},
        table.unpack(item_views),
    }
end
```
