-- Tests for Tooltip, Popup, and Input components
local ui = require("widgets.ui")

print("[TEST] 1. wisp.ui and widgets.ui inheritance...")
assert(wisp.ui ~= nil, "wisp.ui must exist")
assert(ui ~= nil, "widgets.ui must exist")
assert(ui.popup ~= nil, "ui.popup must exist")
assert(ui.tooltip ~= nil, "ui.tooltip must exist")
assert(wisp.ui.popup ~= nil, "wisp.ui.popup must exist")
assert(wisp.ui.tooltip ~= nil, "wisp.ui.tooltip must exist")
print("  ✓ ui inheritance and functions verified")

print("[TEST] 2. Button with tooltip option...")
local btn_clicked = false
local btn = wisp.ui.button({
  label = "Wi-Fi",
  tooltip = "Network settings",
  on_click = function() btn_clicked = true end,
})
assert(btn.type == "button", "btn.type must be button")
assert(btn.text == "Wi-Fi", "btn.text must be Wi-Fi")
assert(btn.tooltip == "Network settings", "btn.tooltip must be preserved")
assert(type(btn.on_hover) == "function", "btn.on_hover must be hooked for tooltip")
assert(type(btn.on_leave) == "function", "btn.on_leave must be hooked for tooltip")
assert(type(btn.on_click) == "function", "btn.on_click must be present")
btn.on_click()
assert(btn_clicked == true, "on_click must execute")
print("  ✓ Button with tooltip verified")

print("[TEST] 3. Icon with tooltip option...")
local ic = wisp.ui.icon("volume", {
  tooltip = "Audio settings",
  size = 18,
})
assert(ic.type == "icon", "ic.type must be icon")
assert(ic.name == "volume", "ic.name must be volume")
assert(ic.tooltip == "Audio settings", "ic.tooltip must be preserved")
assert(type(ic.on_hover) == "function", "ic.on_hover must be hooked for tooltip")
assert(type(ic.on_leave) == "function", "ic.on_leave must be hooked for tooltip")
print("  ✓ Icon with tooltip verified")

print("[TEST] 4. ui.popup API with hover trigger...")
local trigger_elem = wisp.ui.button({ text = "Hover Me" })
local pop_elem = wisp.ui.popup({
  trigger = trigger_elem,
  content = wisp.ui.label({ text = "Popup Body" }),
  placement = "bottom",
  trigger_mode = "hover",
})
assert(pop_elem == trigger_elem, "popup should return wrapped trigger element")
assert(type(pop_elem.on_hover) == "function", "pop_elem.on_hover must be hooked")
assert(type(pop_elem.on_leave) == "function", "pop_elem.on_leave must be hooked")
print("  ✓ ui.popup hover mode verified")

print("[TEST] 5. ui.popup API with click trigger...")
local click_trigger = wisp.ui.button({ text = "Click Me" })
local pop_click = wisp.ui.popup({
  trigger = click_trigger,
  content = function() return wisp.ui.label({ text = "Dynamic Content" }) end,
  placement = "top",
  trigger_mode = "click",
})
assert(pop_click == click_trigger, "popup should return wrapped trigger element")
assert(type(pop_click.on_click) == "function", "pop_click.on_click must be hooked")
print("  ✓ ui.popup click mode verified")

print("[TEST] 6. ui.tooltip decorator...")
local target_elem = wisp.ui.label({ text = "Hover Label" })
local decorated = wisp.ui.tooltip(target_elem, "Label Tooltip", "right")
assert(decorated == target_elem, "tooltip decorator returns target")
assert(decorated.tooltip == "Label Tooltip", "tooltip property attached")
assert(decorated.tooltip_placement == "right", "tooltip_placement property attached")
assert(type(decorated.on_hover) == "function", "on_hover attached")
assert(type(decorated.on_leave) == "function", "on_leave attached")
print("  ✓ ui.tooltip decorator verified")

print("[TEST] 7. SearchInput layout & properties...")
local inp = ui.SearchInput("search_id", "initial", "Search...", function(v) end, function(v) end)
assert(inp.type == "input", "inp.type must be input")
assert(inp.width == "fill", "SearchInput must fill width")
assert(inp.padding_x == 12, "SearchInput padding_x must be 12")
assert(inp.padding_y == 8, "SearchInput padding_y must be 8")
assert(inp.font_size ~= nil, "SearchInput font_size must be specified")
assert(inp.hover_color ~= nil, "SearchInput hover_color must be specified")
assert(inp.focus_color ~= nil, "SearchInput focus_color must be specified")
print("  ✓ SearchInput properties verified")

print("ALL TOOLTIP, POPUP AND INPUT TESTS PASSED!")
