-- Wisp Native Desktop Calculator

local app = wisp.app("calculator", {
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
  app:redraw()
end

local function input_dot()
  if clear_next:get() then
    display:set("0.")
    clear_next:set(false)
  elseif not string.find(display:get(), "%.") then
    display:set(display:get() .. ".")
  end
  app:redraw()
end

local function set_op(next_op)
  prev:set(tonumber(display:get()) or 0)
  op:set(next_op)
  clear_next:set(true)
  app:redraw()
end

local function compute()
  local o = op:get()
  local p = prev:get()
  if not o or p == nil then return end
  local cur = tonumber(display:get()) or 0
  local res = 0
  if o == "+" then
    res = p + cur
  elseif o == "-" then
    res = p - cur
  elseif o == "x" or o == "*" then
    res = p * cur
  elseif o == "/" then
    res = (cur ~= 0) and (p / cur) or 0
  end
  display:set(tostring(res))
  op:set(nil)
  prev:set(nil)
  clear_next:set(true)
  app:redraw()
end

local function clear_all()
  display:set("0")
  op:set(nil)
  prev:set(nil)
  clear_next:set(false)
  app:redraw()
end

app:build(function(ui)
  ui.column({ padding = 16, gap = 12, bg = "#181825" }, function()
    -- Header with Close button
    ui.row({ gap = 8 }, function()
      ui.text("Calculator", { size = 16, color = "#CDD6F4" })
      ui.spacer(8)
      ui.button("Close", app.close)
    end)

    -- Display
    ui.card({ padding = 16, radius = 0, bg = "#1E1E2E" }, function()
      ui.text(display:get(), { size = 32, color = "#CDD6F4" })
    end)

    -- Row 1
    ui.row({ gap = 8 }, function()
      ui.button("C", { bg = "#F38BA8", color = "#11111B", width = 68, height = 54, radius = 0, on_click = clear_all })
      ui.button("+/-", {
        bg = "#45475A",
        width = 68,
        height = 54,
        radius = 8,
        on_click = function()
          local v = tonumber(display:get()) or 0
          display:set(tostring(-v))
          app:redraw()
        end
      })
      ui.button("%", {
        bg = "#45475A",
        width = 68,
        height = 54,
        radius = 8,
        on_click = function()
          local v = tonumber(display:get()) or 0
          display:set(tostring(v / 100))
          app:redraw()
        end
      })
      ui.button("/",
        { bg = "#89B4FA", color = "#11111B", width = 68, height = 54, radius = 0, on_click = function() set_op("/") end })
    end)

    -- Row 2
    ui.row({ gap = 8 }, function()
      ui.button("7", { bg = "#313244", width = 68, height = 54, radius = 0, on_click = function() input_digit(7) end })
      ui.button("8", { bg = "#313244", width = 68, height = 54, radius = 0, on_click = function() input_digit(8) end })
      ui.button("9", { bg = "#313244", width = 68, height = 54, radius = 0, on_click = function() input_digit(9) end })
      ui.button("x",
        { bg = "#89B4FA", color = "#11111B", width = 68, height = 54, radius = 0, on_click = function() set_op("x") end })
    end)

    -- Row 3
    ui.row({ gap = 8 }, function()
      ui.button("4", { bg = "#313244", width = 68, height = 54, radius = 0, on_click = function() input_digit(4) end })
      ui.button("5", { bg = "#313244", width = 68, height = 54, radius = 0, on_click = function() input_digit(5) end })
      ui.button("6", { bg = "#313244", width = 68, height = 54, radius = 0, on_click = function() input_digit(6) end })
      ui.button("-",
        { bg = "#89B4FA", color = "#11111B", width = 68, height = 54, radius = 8, on_click = function() set_op("-") end })
    end)

    -- Row 4
    ui.row({ gap = 8 }, function()
      ui.button("1", { bg = "#313244", width = 68, height = 54, radius = 8, on_click = function() input_digit(1) end })
      ui.button("2", { bg = "#313244", width = 68, height = 54, radius = 8, on_click = function() input_digit(2) end })
      ui.button("3", { bg = "#313244", width = 68, height = 54, radius = 8, on_click = function() input_digit(3) end })
      ui.button("+",
        { bg = "#89B4FA", color = "#11111B", width = 68, height = 54, radius = 8, on_click = function() set_op("+") end })
    end)

    -- Row 5
    ui.row({ gap = 8 }, function()
      ui.button("0", { bg = "#313244", width = 144, height = 54, radius = 8, on_click = function() input_digit(0) end })
      ui.button(".", { bg = "#313244", width = 68, height = 54, radius = 8, on_click = input_dot })
      ui.button("=", { bg = "#A6E3A1", color = "#11111B", width = 68, height = 54, radius = 8, on_click = compute })
    end)
  end)
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

return app:run()
