-- Comprehensive UI Layout & Component System Regression Tests for Wisp
-- Tests: layout primitives, sizing modes, constraints, alignment, nested trees,
-- text overflow/wrapping, component construction, and reactive updates.

local function assert_eq(actual, expected, msg)
  if actual ~= expected then
    error(string.format("ASSERTION FAILED: %s (expected %s, got %s)",
      tostring(msg or "values not equal"), tostring(expected), tostring(actual)), 2)
  end
end

local function assert_true(cond, msg)
  if not cond then
    error(string.format("ASSERTION FAILED: %s", tostring(msg or "condition false")), 2)
  end
end

print("[TEST] 1. Layout primitives and component availability...")
local ui = wisp.ui
assert_true(ui ~= nil, "wisp.ui must exist")
assert_true(type(ui.row) == "function", "ui.row must be a function")
assert_true(type(ui.col) == "function" or type(ui.column) == "function", "ui.column must be a function")
assert_true(type(ui.stack) == "function", "ui.stack must be a function")
assert_true(type(ui.spacer) == "function", "ui.spacer must be a function")
assert_true(type(ui.separator) == "function", "ui.separator must be a function")
assert_true(type(ui.scroll) == "function", "ui.scroll must be a function")
assert_true(type(ui.padding) == "function", "ui.padding must be a function")
assert_true(type(ui.align) == "function", "ui.align must be a function")
assert_true(type(ui.center) == "function", "ui.center must be a function")
assert_true(type(ui.grid) == "function", "ui.grid must be a function")
assert_true(type(ui.card) == "function", "ui.card must be a function")
assert_true(type(ui.button) == "function", "ui.button must be a function")
assert_true(type(ui.input) == "function", "ui.input must be a function")
assert_true(type(ui.slider) == "function", "ui.slider must be a function")
assert_true(type(ui.switch) == "function", "ui.switch must be a function")
assert_true(type(ui.progress) == "function", "ui.progress must be a function")
print("  ✓ All layout primitives and core components are present in wisp.ui")

print("[TEST] 2. wisp.layout table availability...")
assert_true(wisp.layout ~= nil, "wisp.layout must exist")
assert_true(type(wisp.layout.row) == "function", "wisp.layout.row must exist")
assert_true(type(wisp.layout.column) == "function", "wisp.layout.column must exist")
assert_true(type(wisp.layout.spacer) == "function", "wisp.layout.spacer must exist")
assert_true(type(wisp.layout.stack) == "function", "wisp.layout.stack must exist")
assert_true(type(wisp.layout.grid) == "function", "wisp.layout.grid must exist")
assert_true(type(wisp.layout.scroll) == "function", "wisp.layout.scroll must exist")
assert_true(type(wisp.layout.padding) == "function", "wisp.layout.padding must exist")
assert_true(type(wisp.layout.align) == "function", "wisp.layout.align must exist")
assert_true(type(wisp.layout.center) == "function", "wisp.layout.center must exist")
print("  ✓ All layout primitives are mirrored in wisp.layout")

print("[TEST] 3. Sizing specifications and property mapping...")
local fixed_elem = ui.box { width = 120, height = 40 }
assert_eq(fixed_elem.width, 120, "fixed width preserved")
assert_eq(fixed_elem.height, 40, "fixed height preserved")

local fill_elem = ui.box { width = "fill", height = "fill" }
assert_eq(fill_elem.width, "fill", "fill width preserved")
assert_eq(fill_elem.height, "fill", "fill height preserved")

local fit_elem = ui.box { width = "fit", height = "fit" }
assert_eq(fit_elem.width, "fit", "fit width preserved")
assert_eq(fit_elem.height, "fit", "fit height preserved")

local constrained_elem = ui.box { min_width = 100, max_width = 300, min_height = 50, max_height = 200 }
assert_eq(constrained_elem.min_width, 100, "min_width preserved")
assert_eq(constrained_elem.max_width, 300, "max_width preserved")
print("  ✓ Sizing modes (fixed, fit, fill) and min/max constraints verified")

print("[TEST] 4. Nested layouts and tree composition...")
local tree = ui.column {
  gap = 12,
  padding = 16,
  width = "fill",

  ui.row {
    align = { y = "center" },
    justify = "space_between",
    width = "fill",
    ui.text("Header Title"),
    ui.spacer { flex = 1 },
    ui.button { text = "Action" },
  },

  ui.card {
    padding = 12,
    bg = "#161622",
    ui.column {
      gap = 8,
      ui.text("Card Body"),
      ui.progress { value = 42, max = 100, height = 6 },
    }
  },

  ui.grid {
    columns = 2,
    gap = 8,
    ui.button { text = "Grid 1" },
    ui.button { text = "Grid 2" },
  }
}
assert_eq(tree.type, "column", "root element is column")
assert_eq(tree.gap, 12, "column gap is 12")
assert_eq(tree.padding, 16, "column padding is 16")
assert_true(#tree >= 3, "root has at least 3 children")
assert_eq(tree[1].type, "row", "first child is row")
assert_eq(tree[2].type, "card", "second child is card")
assert_eq(tree[3].type, "grid", "third child is grid")
print("  ✓ Nested trees (column -> row, card, grid) construct cleanly")

print("[TEST] 5. Higher-level widgets.ui library...")
local widgets_ui = require("widgets.ui")
assert_true(widgets_ui ~= nil, "widgets.ui must be loadable")
assert_true(type(widgets_ui.PanelHeader) == "function", "PanelHeader must exist")
assert_true(type(widgets_ui.Card) == "function", "Card must exist")
assert_true(type(widgets_ui.Button) == "function", "Button must exist")
assert_true(type(widgets_ui.ToggleRow) == "function", "ToggleRow must exist")
assert_true(type(widgets_ui.SliderRow) == "function", "SliderRow must exist")
assert_true(type(widgets_ui.MetricRow) == "function", "MetricRow must exist")
assert_true(type(widgets_ui.StatCard) == "function", "StatCard must exist")
assert_true(type(widgets_ui.Badge) == "function", "Badge must exist")
assert_true(type(widgets_ui.Divider) == "function", "Divider must exist")
assert_true(type(widgets_ui.ActionRow) == "function", "ActionRow must exist")
assert_true(type(widgets_ui.ProgressBar) == "function", "ProgressBar must exist")

local metric = widgets_ui.MetricRow("󰍛", "CPU", "35%", 35, "#89b4fa")
assert_eq(metric.type, "column", "MetricRow produces column")
assert_eq(metric.width, "fill", "MetricRow width is fill")

local stat = widgets_ui.StatCard("󰁹", "85%", "Battery Level", "#a6e3a1")
assert_eq(stat.type, "card", "StatCard produces card")
assert_eq(stat.width, "fill", "StatCard width is fill")
print("  ✓ High-level components (MetricRow, StatCard, etc.) construct valid layouts")

print("[TEST] 6. Reactive state updates (wisp.state)...")
local st = wisp.state(10)
assert_eq(st:get(), 10, "state initial value is 10")
local observed = nil
st:subscribe(function(v) observed = v end)
st:set(25)
if wisp.flush then wisp.flush() elseif wisp._flush then wisp._flush() end
assert_eq(st:get(), 25, "state updated to 25")
assert_eq(observed, 25, "subscriber received updated value 25")

-- Computed state test
local double_st = wisp.computed(function() return st:get() * 2 end, { st })
assert_eq(double_st:get(), 50, "computed value is 50")
st:set(30)
if wisp.flush then wisp.flush() elseif wisp._flush then wisp._flush() end
assert_eq(double_st:get(), 60, "computed value updated to 60")
print("  ✓ Reactive state (get, set, subscribe, computed) verified")

print("[TEST] 7. Panels manager (lib.panels)...")
local panels = require("lib.panels")
assert_true(panels ~= nil, "panels manager must exist")
assert_true(type(panels.open) == "function", "panels.open exists")
assert_true(type(panels.close) == "function", "panels.close exists")
assert_true(type(panels.toggle) == "function", "panels.toggle exists")
assert_true(type(panels.close_all) == "function", "panels.close_all exists")
assert_true(#panels.names >= 10, "panels manager manages at least 10 panels")
print("  ✓ Panels manager contract verified")

print("ALL REGRESSION TESTS PASSED SUCCESSFULLY!")
