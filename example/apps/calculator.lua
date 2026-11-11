-- Wisp 2.0 Desktop Calculator (Declarative UI with Clay)

local app = wisp.app.create("calculator", {
    title = "Wisp Calculator",
    app_id = "org.wisp.calculator",
    width = 340,
    height = 500,
    background = "#181825",
})

local display = wisp.state("0")
local op = wisp.state(nil)
local prev = wisp.state(nil)
local clear_next = wisp.state(false)

local function input_digit(d)
    if clear_next:get() or display:get() == "0" then
        display:set(tostring(d))
        clear_next:set(false)
    elseif #display:get() < 14 then
        display:set(display:get() .. tostring(d))
    end
end

local function input_dot()
    if clear_next:get() then
        display:set("0.")
        clear_next:set(false)
    elseif not string.find(display:get(), "%.") then
        display:set(display:get() .. ".")
    end
end

local function set_op(next_op)
    prev:set(tonumber(display:get()) or 0)
    op:set(next_op)
    clear_next:set(true)
end

local function compute()
    local o = op:get()
    local p = prev:get()
    if not o or p == nil then return end
    local cur = tonumber(display:get()) or 0
    local res = 0
    if o == "+" then res = p + cur
    elseif o == "-" then res = p - cur
    elseif o == "x" or o == "*" then res = p * cur
    elseif o == "/" then res = (cur ~= 0) and (p / cur) or 0
    end
    display:set(tostring(res))
    op:set(nil)
    prev:set(nil)
    clear_next:set(true)
end

local function clear_all()
    display:set("0")
    op:set(nil)
    prev:set(nil)
    clear_next:set(false)
end

local function calc_btn(text, on_click, bg, color)
    return wisp.ui.button {
        text = text,
        on_click = on_click,
        bg = bg or "#313244",
        color = color or "#CDD6F4",
        radius = 8,
        width = 68,
        height = 54,
    }
end

app:render(function()
    return wisp.ui.col {
        padding = 16,
        gap = 12,
        bg = "#181825",

        -- Display Screen
        wisp.ui.card {
            padding = 16,
            radius = 12,
            bg = "#1E1E2E",
            wisp.ui.label {
                text = display:get(),
                size = 32,
                color = "#CDD6F4",
            }
        },

        -- Keypad Rows
        wisp.ui.row {
            gap = 8,
            calc_btn("C", clear_all, "#F38BA8", "#11111B"),
            calc_btn("+/-", function()
                local v = tonumber(display:get()) or 0
                display:set(tostring(-v))
            end, "#45475A"),
            calc_btn("%", function()
                local v = tonumber(display:get()) or 0
                display:set(tostring(v / 100))
            end, "#45475A"),
            calc_btn("/", function() set_op("/") end, "#89B4FA", "#11111B"),
        },

        wisp.ui.row {
            gap = 8,
            calc_btn("7", function() input_digit(7) end),
            calc_btn("8", function() input_digit(8) end),
            calc_btn("9", function() input_digit(9) end),
            calc_btn("x", function() set_op("x") end, "#89B4FA", "#11111B"),
        },

        wisp.ui.row {
            gap = 8,
            calc_btn("4", function() input_digit(4) end),
            calc_btn("5", function() input_digit(5) end),
            calc_btn("6", function() input_digit(6) end),
            calc_btn("-", function() set_op("-") end, "#89B4FA", "#11111B"),
        },

        wisp.ui.row {
            gap = 8,
            calc_btn("1", function() input_digit(1) end),
            calc_btn("2", function() input_digit(2) end),
            calc_btn("3", function() input_digit(3) end),
            calc_btn("+", function() set_op("+") end, "#89B4FA", "#11111B"),
        },

        wisp.ui.row {
            gap = 8,
            calc_btn("0", function() input_digit(0) end, nil, nil),
            calc_btn(".", input_dot),
            calc_btn("=", compute, "#A6E3A1", "#11111B"),
        },
    }
end)

wisp.keymap.set("<C-q>", function()
    wisp.app.quit()
end)

wisp.keymap.set("<Esc>", clear_all)
wisp.keymap.set("<CR>", compute)

for d = 0, 9 do
    wisp.keymap.set(tostring(d), function() input_digit(d) end)
end
wisp.keymap.set(".", input_dot)
wisp.keymap.set("+", function() set_op("+") end)
wisp.keymap.set("-", function() set_op("-") end)
wisp.keymap.set("*", function() set_op("x") end)
wisp.keymap.set("/", function() set_op("/") end)

return app
