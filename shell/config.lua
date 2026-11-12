-- Wisp Desktop Shell Configuration
-- Evaluated on startup and reloaded on file change.

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
  transparent = true,
  autostart = true,
  visible = false,
  order = 2,
})

-- Exclusive panels (only one open at a time; managed by lib/panels)
local panel_names = {
  "control_center", "wifi", "bluetooth", "audio", "display",
  "battery", "clipboard", "calendar", "launcher",
}
local panel_paths = {
  control_center = "shell/panels/control_center.lua",
  wifi = "shell/panels/wifi.lua",
  bluetooth = "shell/panels/bluetooth.lua",
  audio = "shell/panels/audio.lua",
  display = "shell/panels/display.lua",
  battery = "shell/panels/battery.lua",
  clipboard = "shell/panels/clipboard.lua",
  calendar = "shell/panels/calendar.lua",
  launcher = "shell/apps/launcher.lua",
}
for i, name in ipairs(panel_names) do
  wisp.widget.register(name, {
    path = panel_paths[name],
    anchor = (name == "launcher") and "top_left" or "top_right",
    margin = (name == "launcher") and { top = 30, left = 10 }
      or (name == "calendar") and { top = 30, right = 100 }
      or { top = 30, right = 10 },
    autostart = true,
    visible = false,
    keyboard = true,
    exclusive_group = "shell_panels",
    order = 2 + i,
  })
end

-- Standalone desktop applications (launched via wisp open <name>)
local apps = {
  { name = "calculator", path = "shell/apps/calculator.lua" },
  { name = "annotation", path = "shell/apps/annotation.lua" },
  { name = "image_viewer", path = "shell/apps/image_viewer.lua" },
  { name = "screenshot", path = "shell/apps/screenshot.lua" },
  { name = "todo", path = "shell/apps/todo.lua" },
  { name = "desktop_tools", path = "shell/apps/desktop_tools.lua" },
  { name = "legacy_control_center", path = "shell/apps/control_center.lua" },
  { name = "legacy_clipboard", path = "shell/apps/clipboard.lua" },
}
for _, a in ipairs(apps) do
  wisp.widget.register(a.name, {
    path = a.path,
    autostart = false,
  })
end

-- Keyboard shortcuts (global; keys are not swallowed)
wisp.keyboard.bind("Super+Space", function() wisp.widget.toggle("launcher") end)
wisp.keyboard.bind("Super+C", function() wisp.widget.toggle("calendar") end)
wisp.keyboard.bind("Super+V", function() wisp.widget.toggle("clipboard") end)
wisp.keyboard.bind("Super+I", function() wisp.widget.toggle("control_center") end)
wisp.keyboard.bind("Super+W", function() wisp.widget.toggle("wifi") end)
wisp.keyboard.bind("Super+B", function() wisp.widget.toggle("bluetooth") end)
wisp.keyboard.bind("Super+A", function() wisp.widget.toggle("audio") end)
wisp.keyboard.bind("Super+D", function() wisp.widget.toggle("display") end)
wisp.keyboard.bind("Super+P", function() wisp.widget.toggle("battery") end)
wisp.keyboard.bind("Print", function() wisp.widget.open("screenshot") end)
