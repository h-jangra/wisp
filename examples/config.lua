-- Wisp Desktop Shell Configuration (Complete Example)
-- Evaluated on startup and reloaded on file change.

local function _ex_root()
  local home = os.getenv("HOME") or ""
  local cands = { "examples", "./examples", home .. "/.config/wisp/examples", "/home/hj/Projects/wisp/examples", "shell", "./shell" }
  for _, d in ipairs(cands) do
    local f = io.open(d .. "/style.lua", "r")
    if f then f:close(); return d end
  end
  return "examples"
end
local _er = _ex_root()
package.path = _er .. "/?.lua;" .. _er .. "/?/init.lua;" .. package.path

local panels = require("lib.panels")

wisp.setup({
  scale = 1,
  font_size = 11,
})

-- Status bar (exclusive zone, always visible)
wisp.widget.register("bar", {
  path = "examples/bar/bar.lua",
  anchor = "top_left_right",
  height = 26,
  exclusive = 26,
  autostart = true,
  visible = true,
  order = 1,
})

-- Fullscreen click catcher (closes all panels on outside click)
wisp.widget.register("panel_dismiss", {
  path = "examples/lib/panel_dismiss.lua",
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
  path = "examples/lib/popup_widget.lua",
  anchor = "top_left",
  layer = "overlay",
  transparent = true,
  autostart = true,
  visible = false,
  order = 100,
})

-- Exclusive panels (only one open at a time; managed by lib/panels)
local panel_names = {
  "control_center", "wifi", "bluetooth", "audio", "display",
  "battery", "clipboard", "calendar", "launcher", "power", "notifications",
}
local panel_paths = {
  control_center = "examples/panels/control_center.lua",
  wifi = "examples/panels/wifi.lua",
  bluetooth = "examples/panels/bluetooth.lua",
  audio = "examples/panels/audio.lua",
  display = "examples/panels/display.lua",
  battery = "examples/panels/battery.lua",
  clipboard = "examples/panels/clipboard.lua",
  calendar = "examples/panels/calendar.lua",
  launcher = "examples/apps/launcher.lua",
  power = "examples/panels/power.lua",
  notifications = "examples/panels/notifications.lua",
}
for i, name in ipairs(panel_names) do
  wisp.widget.register(name, {
    path = panel_paths[name],
    layer = "overlay",
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
  { name = "calculator", path = "examples/apps/calculator.lua" },
  { name = "annotation", path = "examples/apps/annotation.lua" },
  { name = "image_viewer", path = "examples/apps/image_viewer.lua" },
  { name = "screenshot", path = "examples/apps/screenshot.lua" },
  { name = "todo", path = "examples/apps/todo.lua" },
  { name = "desktop_tools", path = "examples/apps/desktop_tools.lua" },
  { name = "legacy_control_center", path = "examples/apps/control_center.lua" },
  { name = "legacy_clipboard", path = "examples/apps/clipboard.lua" },
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
wisp.keyboard.bind("Super+N", function() wisp.widget.toggle("notifications") end)
wisp.keyboard.bind("Super+X", function() wisp.widget.toggle("power") end)
wisp.keyboard.bind("Print", function() wisp.widget.open("screenshot") end)
-- Escape closes whichever panel is open (unscoped: config-time binds have no
-- widget slot, so they fire regardless of which surface holds focus).
wisp.keyboard.bind("Escape", function() panels.close_all() end)
