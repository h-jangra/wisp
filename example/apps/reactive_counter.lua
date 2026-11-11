-- Wisp 2.0 Reactive Counter & Todo App (Declarative UI)

local count = wisp.state(0)
local enabled = wisp.state(true)
local brightness = wisp.state(50)
local todos = wisp.state({ "Build Wisp core", "Implement Wisp 2.0 API", "Benchmark RAM" })
local input_text = wisp.state("")

local app = wisp.app.create("reactive_demo", {
  title = "Reactive State Demo",
  app_id = "org.wisp.reactive_demo",
  width = 380,
  height = 480,
  type = "floating",
  autostart = true,
})

app:render(function()
  local todo_items = {}
  for idx, item in ipairs(todos:get()) do
    table.insert(todo_items, wisp.ui.row {
      gap = 8,
      wisp.ui.label { text = "• " .. tostring(item), size = 12, color = "#CDD6F4" },
      wisp.ui.spacer(8),
      wisp.ui.button {
        text = "✕",
        bg = "#F38BA822",
        color = "#F38BA8",
        radius = 4,
        on_click = function()
          local list = todos:get()
          local updated = {}
          for i, v in ipairs(list) do
            if i ~= idx then table.insert(updated, v) end
          end
          todos:set(updated)
        end
      }
    })
  end

  return wisp.ui.col {
    gap = 12,
    padding = 16,
    bg = "#11111B",

    -- Header
    wisp.ui.row {
      wisp.ui.label { text = "Reactive Counter", size = 18, color = "#CDD6F4" },
      wisp.ui.spacer(12),
      wisp.ui.label {
        text = "Status: " .. (enabled:get() and "Active" or "Paused"),
        size = 11,
        color = enabled:get() and "#A6E3A1" or "#F38BA8"
      }
    },

    -- Counter Card
    wisp.ui.card {
      gap = 10,
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 12,

      wisp.ui.row {
        wisp.ui.label { text = "Count:", size = 14, color = "#A6ADC8" },
        wisp.ui.spacer(6),
        wisp.ui.label { text = tostring(count:get()), size = 22, color = "#89B4FA" },
        wisp.ui.spacer(12),
        wisp.ui.switch {
          value = enabled:get(),
          on_change = function(v) enabled:set(v) end
        }
      },

      wisp.ui.row {
        gap = 8,
        wisp.ui.button {
          id = "btn_dec",
          text = "-1",
          on_click = function()
            if enabled:get() then count:update(function(v) return v - 1 end) end
          end
        },
        wisp.ui.button {
          id = "btn_inc",
          text = "+1",
          on_click = function()
            if enabled:get() then count:update(function(v) return v + 1 end) end
          end
        },
        wisp.ui.button {
          id = "btn_batch",
          text = "+10",
          on_click = function()
            if enabled:get() then count:set(count:get() + 10) end
          end
        },
        wisp.ui.button {
          id = "btn_reset",
          text = "Reset",
          on_click = function() count:set(0) end
        }
      }
    },

    -- Slider Card
    wisp.ui.card {
      gap = 8,
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 12,
      wisp.ui.row {
        wisp.ui.label { text = "Brightness:", size = 13, color = "#A6ADC8" },
        wisp.ui.spacer(8),
        wisp.ui.label { text = tostring(math.floor(brightness:get())) .. "%", size = 13, color = "#FAB387" }
      },
      wisp.ui.slider {
        value = brightness:get(),
        min = 0,
        max = 100,
        step = 1,
        on_change = function(v) brightness:set(v) end
      }
    },

    -- Todo Card
    wisp.ui.card {
      gap = 8,
      bg = "#181825",
      border = { color = "#313244", width = 1 },
      radius = 8,
      padding = 12,
      wisp.ui.label { text = "Reactive Todo List", size = 14, color = "#CDD6F4" },
      wisp.ui.row {
        gap = 6,
        wisp.ui.input {
          id = "todo_input",
          value = input_text:get(),
          placeholder = "New task...",
          on_change = function(v) input_text:set(v) end
        },
        wisp.ui.button {
          text = "Add",
          on_click = function()
            local txt = input_text:get()
            if txt and txt ~= "" then
              local list = todos:get()
              local updated = {}
              for _, v in ipairs(list) do table.insert(updated, v) end
              table.insert(updated, txt)
              todos:set(updated)
              input_text:set("")
            end
          end
        }
      },
      wisp.ui.col(table.unpack(todo_items))
    }
  }
end)

return app
