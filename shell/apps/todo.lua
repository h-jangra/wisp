-- Wisp Native Task Application

local app = wisp.app("todo", {
  title = "Wisp Tasks",
  width = 400,
  height = 520,
})

local items = wisp.state({
  { text = "Audit Wisp runtime", done = true },
  { text = "Stabilize Wayland lifecycle", done = true },
  { text = "Ship reliable desktop suite", done = false },
})
local new_text = wisp.state("")

local function add_item()
  local txt = new_text:get()
  if txt and #txt > 0 then
    local current = items:get()
    local next_items = {}
    for _, it in ipairs(current) do
      table.insert(next_items, { text = it.text, done = it.done })
    end
    table.insert(next_items, { text = txt, done = false })
    items:set(next_items)
    new_text:set("")
    app:redraw()
  end
end

local function toggle_item(idx)
  local current = items:get()
  local next_items = {}
  for i, it in ipairs(current) do
    table.insert(next_items, { text = it.text, done = (i == idx) and not it.done or it.done })
  end
  items:set(next_items)
  app:redraw()
end

wisp.keymap.set("<Esc>", app.close)
wisp.keymap.set("<CR>", add_item)

app:build(function(ui)
  ui.column({ gap = 12, padding = 16, bg = "#181825" }, function()
    ui.row({ gap = 8 }, function()
      ui.text("Tasks", { size = 20, color = "#CDD6F4" })
      ui.spacer(10)
      ui.button("Close", app.close)
    end)

    ui.row({ gap = 8 }, function()
      ui.input({
        placeholder = "Add a task...",
        value = new_text:get(),
        on_change = function(val) new_text:set(val) end,
        width = 280,
        height = 36,
      })
      ui.button("Add", add_item)
    end)

    ui.separator({})

    for i, item in ipairs(items:get()) do
      ui.row({ gap = 10, padding = 8, bg = "#1E1E2E", radius = 6 }, function()
        ui.button(item.done and "[x]" or "[ ]", function() toggle_item(i) end)
        ui.text(item.text, {
          size = 14,
          color = item.done and "#6C7086" or "#CDD6F4"
        })
      end)
    end
  end)
end)

return app:run()
