-- Wisp Desktop Shell Configuration
-- Canonical modular shell with 1 bar and 7 exclusive panels:
--   Bar:    launcher, workspaces | date | tray, memory, bluetooth, wifi, audio, display, battery
--   Panels: launcher, calendar, bluetooth, wifi, audio, display, battery

local panels = require("lib.panels")

wisp.setup({
  scale = 1,
  font_size = 11,
})

-- Status bar (exclusive zone, always visible)
wisp.widget.register("bar", {
  path = "shell/bar/bar.lua",
  anchor = "top_left_right",
  height = 26,
  exclusive = 26,
  autostart = true,
  visible = true,
  order = 1,
})

-- Fullscreen click catcher (closes all panels on outside click)
wisp.widget.register("panel_dismiss", {
  path = "shell/lib/panel_dismiss.lua",
  anchor = "top_bottom_left_right",
  margin = { top = 30 },
  layer = "overlay",
  transparent = true,
  autostart = true,
  visible = false,
  order = 2,
})

-- Shared Popup & Tooltip Overlay
wisp.widget.register("wisp_popup", {
  path = "shell/lib/popup_widget.lua",
  anchor = "top_left",
  layer = "overlay",
  transparent = true,
  autostart = true,
  visible = false,
  order = 100,
})

-- Exclusive panels (only one open at a time; managed by lib/panels)
local panel_definitions = {
  { name = "launcher",  path = "shell/panels/launcher.lua",  anchor = "top_left",  margin = { top = 30, left = 10 } },
  { name = "calendar",  path = "shell/panels/calendar.lua",  anchor = "top_right", margin = { top = 30, right = 100 } },
  { name = "bluetooth", path = "shell/panels/bluetooth.lua", anchor = "top_right", margin = { top = 30, right = 10 } },
  { name = "wifi",      path = "shell/panels/wifi.lua",      anchor = "top_right", margin = { top = 30, right = 10 } },
  { name = "audio",     path = "shell/panels/audio.lua",     anchor = "top_right", margin = { top = 30, right = 10 } },
  { name = "display",   path = "shell/panels/display.lua",   anchor = "top_right", margin = { top = 30, right = 10 } },
  { name = "battery",   path = "shell/panels/battery.lua",   anchor = "top_right", margin = { top = 30, right = 10 } },
}

for i, p in ipairs(panel_definitions) do
  wisp.widget.register(p.name, {
    path = p.path,
    layer = "overlay",
    anchor = p.anchor,
    margin = p.margin,
    autostart = true,
    visible = false,
    keyboard = true,
    exclusive_group = "shell_panels",
    order = 2 + i,
  })
end

-- Global Keyboard Shortcuts
wisp.keyboard.bind("Super+Space", function() panels.toggle("launcher") end)
wisp.keyboard.bind("Super+C",     function() panels.toggle("calendar") end)
wisp.keyboard.bind("Super+B",     function() panels.toggle("bluetooth") end)
wisp.keyboard.bind("Super+W",     function() panels.toggle("wifi") end)
wisp.keyboard.bind("Super+A",     function() panels.toggle("audio") end)
wisp.keyboard.bind("Super+D",     function() panels.toggle("display") end)
wisp.keyboard.bind("Super+P",     function() panels.toggle("battery") end)
wisp.keyboard.bind("Escape",      function() panels.close_all() end)
