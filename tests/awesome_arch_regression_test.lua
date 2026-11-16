-- Regression test suite for AwesomeWM-inspired Architecture in Wisp
-- Covers: wisp.object, wisp.widget.base, wisp.layout, wisp.container,
-- wisp.popup, wisp.tooltip, signal invalidation, measurement, layout,
-- and Clay compilation.

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

print("======================================================================")
print("RUNNING AWESOMEWM-INSPIRED WIDGET & LAYOUT ARCHITECTURE REGRESSION TESTS")
print("======================================================================")

-- ── 1. gears.object / wisp.object ───────────────────────────────────────────
print("\n[TEST 1] gears.object / wisp.object signal system...")
local obj = wisp.object()
assert_true(obj ~= nil, "wisp.object() creates instance")
assert_true(type(obj.connect_signal) == "function", "connect_signal must be function")
assert_true(type(obj.disconnect_signal) == "function", "disconnect_signal must be function")
assert_true(type(obj.emit_signal) == "function", "emit_signal must be function")

local signal_fired = 0
local signal_args = nil
local function on_test(self, a, b)
  signal_fired = signal_fired + 1
  signal_args = { a, b }
end

obj:connect_signal("test::signal", on_test)
assert_true(obj:has_signal("test::signal"), "has_signal returns true")
obj:emit_signal("test::signal", 42, "hello")
assert_eq(signal_fired, 1, "signal callback executed")
assert_eq(signal_args[1], 42, "first signal argument preserved")
assert_eq(signal_args[2], "hello", "second signal argument preserved")

obj:disconnect_signal("test::signal", on_test)
obj:emit_signal("test::signal", 99, "world")
assert_eq(signal_fired, 1, "disconnected callback does not run")

local target = { name = "custom_table" }
wisp.object.apply_to(target)
assert_true(type(target.connect_signal) == "function", "apply_to mixes in signal methods")
local custom_fired = false
target:connect_signal("custom::event", function() custom_fired = true end)
target:emit_signal("custom::event")
assert_true(custom_fired, "mixed-in signal fired")
print("  ✓ wisp.object signals, connect, disconnect, and apply_to verified")

-- ── 2. wibox.widget.base / wisp.widget.base ─────────────────────────────────
print("\n[TEST 2] wibox.widget.base / wisp.widget.base widget lifecycle...")
local base = wisp.widget.base
assert_true(base ~= nil, "wisp.widget.base must exist")
assert_true(type(base.make_widget) == "function", "make_widget must exist")
assert_true(type(base.fit_widget) == "function", "fit_widget must exist")
assert_true(type(base.layout_widget) == "function", "layout_widget must exist")
assert_true(type(base.place_widget_at) == "function", "place_widget_at must exist")

local w = base.make_widget(nil, "my_widget", { forced_width = 120, forced_height = 40 })
assert_true(w.is_widget, "w.is_widget is true")
assert_eq(w:get_visible(), true, "default visible is true")
assert_eq(w:get_opacity(), 1.0, "default opacity is 1.0")
assert_eq(w:get_forced_width(), 120, "forced_width is 120")
assert_eq(w:get_forced_height(), 40, "forced_height is 40")

-- Child management & hierarchy
local c1 = base.make_widget(nil, "child1", { forced_width = 30, forced_height = 20 })
local c2 = base.make_widget(nil, "child2", { forced_width = 40, forced_height = 25 })
local c3 = base.make_widget(nil, "child3_nested", { forced_width = 10, forced_height = 10 })
c2:add(c3)
w:add(c1)
w:add(c2)

local children = w:get_children()
assert_eq(#children, 2, "root has 2 direct children")
local all_ch = w:get_all_children()
assert_eq(#all_ch, 3, "get_all_children returns 3 descendants recursively")

-- Recursive signal emission
local child_signal_count = 0
c3:connect_signal("broadcast", function() child_signal_count = child_signal_count + 1 end)
w:emit_signal_recursive("broadcast")
assert_eq(child_signal_count, 1, "recursive signal reached leaf child")

-- Measurement (fit)
local fw, fh = base.fit_widget(nil, {}, w, 800, 600)
assert_eq(fw, 120, "fit respects forced_width")
assert_eq(fh, 40, "fit respects forced_height")

-- Invalidation bubbling
local layout_changed_fired = false
w:connect_signal("widget::layout_changed", function() layout_changed_fired = true end)
c1:set_forced_width(60)
assert_true(layout_changed_fired, "child layout change bubbled up to parent")

local redraw_needed_fired = false
w:connect_signal("widget::redraw_needed", function() redraw_needed_fired = true end)
c1:set_opacity(0.8)
assert_true(redraw_needed_fired, "child redraw needed bubbled up to parent")

print("  ✓ wisp.widget.base hierarchy, measurement, and signal bubbling verified")

-- ── 3. wisp.layout algorithms (fixed, flex, align, grid, stack) ─────────────
print("\n[TEST 3] wisp.layout algorithms...")
local layout = wisp.layout
assert_true(layout.fixed ~= nil, "layout.fixed exists")
assert_true(layout.flex ~= nil, "layout.flex exists")
assert_true(layout.align ~= nil, "layout.align exists")
assert_true(layout.grid ~= nil, "layout.grid exists")
assert_true(layout.stack ~= nil, "layout.stack exists")

-- Fixed horizontal layout
local fix_h = layout.fixed.horizontal {
  spacing = 10,
  base.make_widget(nil, "w1", { forced_width = 50, forced_height = 30 }),
  base.make_widget(nil, "w2", { forced_width = 70, forced_height = 40 }),
}
local fit_w, fit_h = fix_h:fit({}, 500, 500)
assert_eq(fit_w, 50 + 10 + 70, "fixed.horizontal fit width is sum + spacing (130)")
assert_eq(fit_h, 40, "fixed.horizontal fit height is max child height (40)")

local placed_h = fix_h:layout({}, 500, 500)
assert_eq(#placed_h, 2, "placed 2 children")
assert_eq(placed_h[1].x, 0, "first child placed at x=0")
assert_eq(placed_h[1].width, 50, "first child width=50")
assert_eq(placed_h[2].x, 60, "second child placed at x=60 (50 + 10)")
assert_eq(placed_h[2].width, 70, "second child width=70")

-- Flex horizontal layout
local flx_h = layout.flex.horizontal {
  spacing = 10,
  base.make_widget(nil, "f1"),
  base.make_widget(nil, "f2"),
}
local placed_flx = flx_h:layout({}, 210, 50)
assert_eq(#placed_flx, 2, "placed 2 flex children")
assert_eq(placed_flx[1].width, 100, "first flex child allocated exactly 100 px ((210-10)/2)")
assert_eq(placed_flx[2].width, 100, "second flex child allocated exactly 100 px")
assert_eq(placed_flx[2].x, 110, "second flex child placed at x=110 (100 + 10)")

-- Align layout (3 slots: first, second, third)
local al_h = wibox.layout.align.horizontal {
  first  = base.make_widget(nil, "left", { forced_width = 80, forced_height = 30 }),
  second = base.make_widget(nil, "center", { forced_width = 60, forced_height = 30 }),
  third  = base.make_widget(nil, "right", { forced_width = 100, forced_height = 30 }),
}
local placed_al = al_h:layout({}, 400, 30)
assert_eq(#placed_al, 3, "placed 3 align children")
assert_eq(placed_al[1].x, 0, "first placed at x=0")
assert_eq(placed_al[1].width, 80, "first width=80")
assert_eq(placed_al[3].x, 300, "third placed at x=300 (400 - 100)")
assert_eq(placed_al[3].width, 100, "third width=100")
-- Center slot placed between first and third: rem = 400 - 80 - 100 = 220; center_x = 80 + (220-60)/2 = 160
assert_eq(placed_al[2].x, 160, "center slot truly centered between outer slots at x=160")
assert_eq(placed_al[2].width, 60, "center width=60")

-- Grid layout
local gr = layout.grid {
  columns = 3,
  gap = 8,
  base.make_widget(nil, "g1"),
  base.make_widget(nil, "g2"),
}
assert_eq(gr.columns, 3, "grid columns=3")

-- Stack layout
local stk = layout.stack {
  base.make_widget(nil, "s1", { forced_width = 100, forced_height = 50 }),
  base.make_widget(nil, "s2", { forced_width = 80, forced_height = 90 }),
}
local stk_w, stk_h = stk:fit({}, 500, 500)
assert_eq(stk_w, 100, "stack fit width is max child width (100)")
assert_eq(stk_h, 90, "stack fit height is max child height (90)")
local placed_stk = stk:layout({}, 200, 200)
assert_eq(placed_stk[1].x, 0, "first layer placed at 0,0")
assert_eq(placed_stk[2].x, 0, "second layer placed at 0,0 (layered stack)")
print("  ✓ fixed, flex, align, grid, and stack layout algorithms verified")

-- ── 4. wisp.container decorators ────────────────────────────────────────────
print("\n[TEST 4] wisp.container decorators (margin, background, constraint, place, scroll)...")
local cont = wisp.container
assert_true(cont ~= nil, "wisp.container must exist")
assert_true(cont.margin ~= nil, "container.margin must exist")
assert_true(cont.background ~= nil, "container.background must exist")
assert_true(cont.constraint ~= nil, "container.constraint must exist")
assert_true(cont.place ~= nil, "container.place must exist")
assert_true(cont.scroll ~= nil, "container.scroll must exist")

-- Margin container
local inner = base.make_widget(nil, "inner", { forced_width = 100, forced_height = 50 })
local m = cont.margin {
  left = 10, right = 15, top = 5, bottom = 8,
  child = inner,
}
local mw, mh = m:fit({}, 500, 500)
assert_eq(mw, 100 + 10 + 15, "margin container fit width includes horizontal margins (125)")
assert_eq(mh, 50 + 5 + 8, "margin container fit height includes vertical margins (63)")
local placed_m = m:layout({}, 200, 200)
assert_eq(placed_m[1].x, 10, "child placed offset by left margin (10)")
assert_eq(placed_m[1].y, 5, "child placed offset by top margin (5)")
assert_eq(placed_m[1].width, 200 - 10 - 15, "child width reduced by margins (175)")

-- Constraint container (strategy = max)
local bounded = cont.constraint {
  strategy = "max",
  width = 150,
  height = 80,
  child = base.make_widget(nil, "big", { forced_width = 300, forced_height = 200 }),
}
local cw, ch = bounded:fit({}, 500, 500)
assert_eq(cw, 150, "constraint clamped width to max 150")
assert_eq(ch, 80, "constraint clamped height to max 80")

-- Place container (alignment within available space)
local pl = cont.place {
  halign = "right",
  valign = "center",
  child = base.make_widget(nil, "btn", { forced_width = 50, forced_height = 20 }),
}
local placed_pl = pl:layout({}, 200, 100)
assert_eq(placed_pl[1].x, 150, "right-aligned child placed at x=150 (200 - 50)")
assert_eq(placed_pl[1].y, 40, "center-aligned child placed at y=40 ((100 - 20) / 2)")

-- Background container
local bg = cont.background {
  bg = "#1E1E2E",
  radius = 8,
  padding = 6,
  child = inner,
}
local bg_w, bg_h = bg:fit({}, 500, 500)
assert_eq(bg_w, 100 + 12, "background fit includes padding (112)")
assert_eq(bg_h, 50 + 12, "background fit includes padding (62)")
print("  ✓ margin, background, constraint, and place containers verified")

-- ── 5. Declarative Composition & Clay Compilation ──────────────────────────
print("\n[TEST 5] Declarative Composition & Clay Compilation...")
local declarative_tree = wisp.widget {
  layout = wibox.layout.align.horizontal,
  first = wisp.container.margin {
    margins = 8,
    child = wisp.ui.label { text = "Left Label" },
  },
  second = wisp.ui.button { text = "Center Button" },
  third = wisp.container.background {
    bg = "#11111B",
    radius = 4,
    child = wisp.ui.label { text = "Right Status" },
  },
}

assert_true(declarative_tree ~= nil, "declarative_tree constructed")
assert_true(type(declarative_tree.to_clay) == "function", "has to_clay compiler")

local clay_elem = declarative_tree:to_clay()
assert_eq(clay_elem.type, "row", "align.horizontal compiles to Clay row")
assert_true(#clay_elem >= 3, "clay row contains at least 3 slot elements")
assert_eq(clay_elem[1].type, "row", "first slot is row")
assert_eq(clay_elem[2].type, "container", "second slot is container")
assert_eq(clay_elem[3].type, "row", "third slot is row")
print("  ✓ Declarative tree compiles to native Clay layout elements")

-- ── 6. awful.popup & awful.tooltip Lifecycles ───────────────────────────────
print("\n[TEST 6] awful.popup & awful.tooltip Lifecycles...")
local popup = wisp.popup {
  placement = "top",
  widget = wisp.ui.label { text = "Popup Content" },
}
assert_true(popup ~= nil, "wisp.popup creates instance")
assert_eq(popup.placement, "top", "placement is top")
assert_eq(popup.visible, false, "initially not visible")

local tooltip = wisp.tooltip {
  text = "Helpful Info",
  delay = 150,
}
assert_true(tooltip ~= nil, "wisp.tooltip creates instance")
assert_eq(tooltip:get_text(), "Helpful Info", "tooltip text retrieved")

local test_btn = base.make_widget(nil, "tip_button")
tooltip:add_to_object(test_btn)
assert_eq(#tooltip.objects, 1, "test_btn added to tooltip objects")
tooltip:remove_from_object(test_btn)
assert_eq(#tooltip.objects, 0, "test_btn removed from tooltip objects")
print("  ✓ popup and tooltip lifecycle and object binding verified")

-- ── 7. Namespace Aliasing (AwesomeWM Compatibility) ─────────────────────────
print("\n[TEST 7] AwesomeWM namespace compatibility (gears, wibox, awful)...")
assert_true(gears ~= nil, "gears namespace exists")
assert_true(gears.object == wisp.object, "gears.object maps to wisp.object")
assert_true(wibox ~= nil, "wibox namespace exists")
assert_true(wibox.widget.base == wisp.widget.base, "wibox.widget.base maps to wisp.widget.base")
assert_true(wibox.layout ~= nil, "wibox.layout exists")
assert_true(wibox.container == wisp.container, "wibox.container maps to wisp.container")
assert_true(awful ~= nil, "awful namespace exists")
assert_true(awful.popup == wisp.popup, "awful.popup maps to wisp.popup")
assert_true(awful.tooltip == wisp.tooltip, "awful.tooltip maps to wisp.tooltip")
print("  ✓ AwesomeWM namespaces (gears, wibox, awful) cleanly mapped")

print("\n======================================================================")
print("ALL AWESOMEWM ARCHITECTURAL REGRESSION TESTS PASSED SUCCESSFULLY!")
print("======================================================================")
