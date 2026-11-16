-- Wisp 2.0 Desktop Control Center
-- Demonstrates Declarative UI with Clay: Column, Row, Card, Label, Button, Input, Slider, Switch, Spacer, Separator

local state = {
  active_tab = 1,
  tabs = { "Network", "Display", "Sound", "System" },
  wifi_enabled = true,
  bluetooth_enabled = true,
  airplane_mode = false,
  dark_mode = true,
  brightness = 75,
  volume = 65,
  search_query = "",
  status_msg = "Ready",
  devices = {
    { id = "w1", name = "Home-5G", type = "wifi", signal = "98%", connected = true },
    { id = "w2", name = "Office-Guest", type = "wifi", signal = "72%", connected = false },
    { id = "b1", name = "Sony WH-1000XM4", type = "bt", signal = "Connected", connected = true },
    { id = "b2", name = "Keychron K2", type = "bt", signal = "Paired", connected = true },
  }
}

local app = wisp.app.create("control_center", {
  title = "Control Center",
  app_id = "org.wisp.control_center",
  width = 680,
  height = 540,
  type = "floating",
  autostart = true,
})

app:render(function()
  local tab_buttons = {}
  for i, tab_name in ipairs(state.tabs) do
    local is_active = (state.active_tab == i)
    table.insert(tab_buttons, wisp.ui.button {
      text = tab_name,
      bg = is_active and "#89B4FA" or "#313244",
      color = is_active and "#11111B" or "#CDD6F4",
      radius = 6,
      padding = 8,
      on_click = function()
        state.active_tab = i
        state.status_msg = "Selected " .. tab_name
        app:redraw()
      end
    })
  end

  local dev_cards = {}
  for _, dev in ipairs(state.devices) do
    table.insert(dev_cards, wisp.ui.card {
      padding = 10,
      radius = 8,
      bg = dev.connected and "#313244" or "#1E1E2E",
      wisp.ui.row {
        gap = 12,
        wisp.ui.label { text = (dev.type == "wifi" and "[WiFi] " or "[BT] ") .. dev.name, size = 14 },
        wisp.ui.spacer { flex = 1 },
        wisp.ui.label { text = dev.signal, size = 12, color = dev.connected and "#A6E3A1" or "#A6ADC8" },
        wisp.ui.button {
          text = dev.connected and "Disconnect" or "Connect",
          bg = dev.connected and "#45475A" or "#89B4FA",
          color = dev.connected and "#CDD6F4" or "#11111B",
          radius = 4,
          on_click = function()
            dev.connected = not dev.connected
            state.status_msg = (dev.connected and "Connected to " or "Disconnected from ") .. dev.name
            app:redraw()
          end
        }
      }
    })
  end

  return wisp.ui.col {
    padding = 16,
    gap = 12,
    bg = "#11111B",

    -- Header
    wisp.ui.row {
      wisp.ui.label { text = "Control Center", size = 18, color = "#CDD6F4" },
      wisp.ui.spacer { flex = 1 },
      wisp.ui.label { text = state.status_msg, size = 12, color = "#A6ADC8" },
      wisp.ui.spacer { width = 12 },
      wisp.ui.button {
        text = "Reset",
        bg = "#313244",
        color = "#CDD6F4",
        radius = 6,
        on_click = function()
          state.brightness = 75
          state.volume = 65
          state.status_msg = "Settings reset"
          app:redraw()
        end
      }
    },

    -- Tabs
    wisp.ui.row {
      gap = 8,
      table.unpack(tab_buttons)
    },

    wisp.ui.separator {},

    -- Sliders
    wisp.ui.row {
      gap = 16,
      wisp.ui.col {
        gap = 4,
        wisp.ui.label { text = "Brightness: " .. state.brightness .. "%", size = 12 },
        wisp.ui.slider {
          value = state.brightness,
          min = 0,
          max = 100,
          on_change = function(v) state.brightness = math.floor(v); app:redraw() end
        }
      },
      wisp.ui.col {
        gap = 4,
        wisp.ui.label { text = "Volume: " .. state.volume .. "%", size = 12 },
        wisp.ui.slider {
          value = state.volume,
          min = 0,
          max = 100,
          on_change = function(v) state.volume = math.floor(v); app:redraw() end
        }
      }
    },

    wisp.ui.separator {},

    -- Device List
    wisp.ui.label { text = "Devices", size = 14, color = "#CDD6F4" },
    wisp.ui.col {
      gap = 6,
      table.unpack(dev_cards)
    }
  }
end)

wisp.keymap.set("<C-q>", function()
  wisp.app.quit()
end)

return app
