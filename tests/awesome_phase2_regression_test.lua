-- ============================================================================
-- WISP PHASE 2 COMPREHENSIVE ARCHITECTURE & SHELL REGRESSION TEST SUITE
-- Tests AwesomeWM architecture hardening:
--   1. Real nested widget trees & measurement
--   2. Dynamic child lifecycle (add, remove, reset, parent links)
--   3. Upward signal invalidation bubbling (layout vs redraw)
--   4. Constraint container mathematics (max, min, exact)
--   5. Standard widgets (textbox, progressbar, slider, checkbox, button, input)
--   6. Declarative composition & get_children_by_id
--   7. awful.popup & awful.tooltip lifecycle & collision avoidance
--   8. Live panel compilation verification
-- ============================================================================

local base = wisp.widget.base
local layout = wisp.layout
local container = wisp.container
local object = wisp.object

local pass_count = 0
local fail_count = 0

local function assert_true(cond, msg)
  if cond then
    pass_count = pass_count + 1
  else
    fail_count = fail_count + 1
    error("FAIL: " .. tostring(msg))
  end
end

local function assert_eq(a, b, msg)
  if a == b then
    pass_count = pass_count + 1
  else
    fail_count = fail_count + 1
    error(string.format("FAIL: %s (expected %s, got %s)", tostring(msg), tostring(b), tostring(a)))
  end
end

print("======================================================================")
print("RUNNING WISP PHASE 2 AWESOMEWM ARCHITECTURE HARDENING TESTS")
print("======================================================================")

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 1: Real Nested Widget Trees & Measurement
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 1] Real Nested Widget Trees & Measurement...")
do
  local leaf1 = wibox.widget.textbox { text = "Leaf 1", size = 12 } -- len=6, sz=12 => ~43px
  local leaf2 = wibox.widget.textbox { text = "Leaf 2 Long Text", size = 12 }

  local inner_box = wibox.container.background {
    bg = "#1e1e2e",
    padding = 10,
    widget = wibox.layout.fixed.vertical {
      spacing = 5,
      leaf1,
      leaf2,
    }
  }

  local outer_margin = wibox.container.margin {
    left = 15,
    right = 15,
    top = 8,
    bottom = 8,
    widget = inner_box,
  }

  local root = wibox.layout.align.horizontal {
    first = outer_margin,
    second = wibox.widget.separator { orientation = "vertical", thickness = 2 },
    third = wibox.widget.button { text = "Action", width = 80, height = 30 },
  }

  -- Check parent links down the tree
  assert_eq(leaf1._private.parent, inner_box.widget, "leaf1 parent is inner layout")
  assert_eq(inner_box.widget._private.parent, inner_box, "inner layout parent is inner_box background")
  assert_eq(inner_box._private.parent, outer_margin, "inner_box parent is outer_margin")
  assert_eq(outer_margin._private.parent, root, "outer_margin parent is root align layout")

  -- Check fit measurement
  local rw, rh = root:fit({}, 1000, 1000)
  assert_true(rw > 100, "root measured width includes margins, padding, and children: " .. rw)
  assert_true(rh >= 30, "root measured height accommodates children: " .. rh)

  -- Check compilation to Clay
  local clay = root:to_clay()
  assert_eq(clay.type, "row", "root layout compiles to Clay row")
  assert_eq(#clay, 3, "root row has 3 columns (first, second, third)")
  print("  ✓ Nested widget trees, parent hierarchy, and measurement verified")
end

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 2: Dynamic Child Lifecycle (Add, Remove, Reset, Stale Parent Cleanup)
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 2] Dynamic Child Lifecycle & Stale Parent Cleanup...")
do
  local parent_col = wibox.layout.fixed.vertical { spacing = 4 }
  local c1 = wibox.widget.textbox { text = "Child 1" }
  local c2 = wibox.widget.textbox { text = "Child 2" }
  local c3 = wibox.widget.textbox { text = "Child 3" }

  parent_col:add(c1)
  parent_col:add(c2)
  parent_col:add(c3)

  assert_eq(#parent_col:get_children(), 3, "3 children added")
  assert_eq(c1._private.parent, parent_col, "c1 linked to parent")
  assert_eq(c2._private.parent, parent_col, "c2 linked to parent")
  assert_eq(c3._private.parent, parent_col, "c3 linked to parent")

  -- Remove middle child c2
  local removed = parent_col:remove(2)
  assert_eq(removed, c2, "c2 was removed by index")
  assert_eq(#parent_col:get_children(), 2, "2 children remain after remove")
  assert_eq(c2._private.parent, nil, "c2 parent reference cleared on removal")
  assert_eq(parent_col:get_children()[1], c1, "c1 is first child")
  assert_eq(parent_col:get_children()[2], c3, "c3 is second child")

  -- Remove by widget instance
  parent_col:remove_widgets(c1)
  assert_eq(#parent_col:get_children(), 1, "1 child remains after remove_widgets")
  assert_eq(c1._private.parent, nil, "c1 parent reference cleared")

  -- Reset all children
  parent_col:reset()
  assert_eq(#parent_col:get_children(), 0, "0 children after reset")
  assert_eq(c3._private.parent, nil, "c3 parent reference cleared after reset")

  -- Re-add to another container cleanly
  local new_box = wibox.container.background {}
  new_box:set_widget(c3)
  assert_eq(c3._private.parent, new_box, "c3 successfully relinked to new parent")
  new_box:set_widget(nil)
  assert_eq(c3._private.parent, nil, "c3 unlinked when set_widget(nil)")

  print("  ✓ Dynamic child addition, removal, reset, and unlinking verified")
end

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 3: Upward Signal Invalidation Bubbling
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 3] Upward Signal Invalidation Bubbling...")
do
  local layout_changes = 0
  local redraw_needed = 0

  local root_card = wibox.container.background {
    bg = "#11111b",
    padding = 10,
  }

  root_card:connect_signal("widget::layout_changed", function()
    layout_changes = layout_changes + 1
  end)
  root_card:connect_signal("widget::redraw_needed", function()
    redraw_needed = redraw_needed + 1
  end)

  local text_widget = wibox.widget.textbox { text = "Initial" }
  local margin = wibox.container.margin {
    left = 4,
    widget = text_widget,
  }
  root_card:set_widget(margin)

  -- Changing text should trigger both layout_changed and redraw_needed up the chain
  text_widget:set_text("Updated Text With New Length")
  assert_true(layout_changes >= 1, "layout_changed bubbled up from nested leaf to root")
  assert_true(redraw_needed >= 1, "redraw_needed bubbled up from nested leaf to root")

  -- Changing color should trigger redraw_needed
  local prev_redraws = redraw_needed
  text_widget:set_color("#89b4fa")
  assert_true(redraw_needed > prev_redraws, "property::color triggers redraw_needed bubbling")

  print("  ✓ Signal bubbling along parent hierarchy verified without closures")
end

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 4: Constraint Container Mathematics
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 4] Constraint Container Mathematics...")
do
  local child = wibox.widget.base.make_widget(nil, "constrained_child")
  function child:fit(ctx, w, h)
    return 200, 150
  end

  -- Max constraint: clamps if child exceeds
  local max_c = wibox.container.constraint(child, "max", 120, 100)
  local mw, mh = max_c:fit({}, 500, 500)
  assert_eq(mw, 120, "max strategy clamped width to 120")
  assert_eq(mh, 100, "max strategy clamped height to 100")

  -- Min constraint: expands if child is smaller
  local min_c = wibox.container.constraint(child, "min", 300, 250)
  local nw, nh = min_c:fit({}, 500, 500)
  assert_eq(nw, 300, "min strategy expanded width to 300")
  assert_eq(nh, 250, "min strategy expanded height to 250")

  -- Exact constraint: forces exact size regardless of child fit
  local exact_c = wibox.container.constraint(child, "exact", 88, 44)
  local ew, eh = exact_c:fit({}, 500, 500)
  assert_eq(ew, 88, "exact strategy forced width to 88")
  assert_eq(eh, 44, "exact strategy forced height to 44")

  print("  ✓ Constraint container strategies (max, min, exact) verified")
end

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 5: Standard Widgets Properties, Setters & Clay Output
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 5] Standard Widgets Properties, Setters & Clay Output...")
do
  -- Progressbar
  local pb = wibox.widget.progressbar { value = 35, max_value = 100, color = "#a6e3a1" }
  assert_eq(pb:get_value(), 35, "progressbar get_value")
  pb:set_value(72)
  assert_eq(pb:get_value(), 72, "progressbar set_value")
  local pb_clay = pb:to_clay()
  assert_eq(pb_clay.type, "progress", "progressbar clay type")
  assert_eq(pb_clay.value, 72, "progressbar clay value")

  -- Slider
  local sl = wibox.widget.slider { value = 50, minimum = 0, maximum = 100 }
  assert_eq(sl:get_value(), 50, "slider get_value")
  sl:set_value(80)
  assert_eq(sl:get_value(), 80, "slider set_value")
  local sl_clay = sl:to_clay()
  assert_eq(sl_clay.type, "slider", "slider clay type")
  assert_eq(sl_clay.value, 80, "slider clay value")

  -- Checkbox
  local cb = wibox.widget.checkbox { checked = false }
  assert_eq(cb:get_checked(), false, "checkbox get_checked false")
  cb:set_checked(true)
  assert_eq(cb:get_checked(), true, "checkbox set_checked true")
  local cb_clay = cb:to_clay()
  assert_eq(cb_clay.type, "switch", "checkbox clay type")
  assert_eq(cb_clay.checked, true, "checkbox clay checked")

  -- Separator
  local sep = wibox.widget.separator { orientation = "vertical", thickness = 3 }
  local sep_clay = sep:to_clay()
  assert_eq(sep_clay.type, "separator", "separator clay type")
  assert_eq(sep_clay.vertical, true, "separator vertical")

  -- Spacer
  local spc = wibox.widget.spacer { width = 16 }
  local spc_clay = spc:to_clay()
  assert_eq(spc_clay.type, "spacer", "spacer clay type")
  assert_eq(spc_clay.width, 16, "spacer clay width")

  -- Input
  local inp = wibox.widget.input { id = "test_inp", value = "hello" }
  assert_eq(inp:get_text(), "hello", "input get_text")
  inp:set_text("world")
  assert_eq(inp:get_text(), "world", "input set_text")
  local inp_clay = inp:to_clay()
  assert_eq(inp_clay.type, "input", "input clay type")
  assert_eq(inp_clay.value, "world", "input clay value")

  print("  ✓ All standard widgets, properties, signals, and Clay compilation verified")
end

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 6: Declarative Composition & get_children_by_id
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 6] Declarative Composition & get_children_by_id...")
do
  local tree = wibox.widget {
    layout = wibox.layout.fixed.vertical,
    spacing = 10,
    {
      widget = wibox.widget.textbox,
      id = "my_title",
      text = "Declarative Title",
    },
    {
      layout = wibox.layout.fixed.horizontal,
      spacing = 5,
      {
        widget = wibox.widget.button,
        id = "my_btn",
        text = "Click Me",
      },
      {
        widget = wibox.widget.progressbar,
        id = "my_prog",
        value = 40,
      }
    }
  }

  local title = tree:get_children_by_id("my_title")
  assert_true(title ~= nil, "my_title found via get_children_by_id")
  assert_eq(title:get_text(), "Declarative Title", "title text correct")

  local btn = tree:get_children_by_id("my_btn")
  assert_true(btn ~= nil, "my_btn found via get_children_by_id")
  assert_eq(btn.text, "Click Me", "button text correct")

  local prog = tree:get_children_by_id("my_prog")
  assert_true(prog ~= nil, "my_prog found via get_children_by_id")
  assert_eq(prog:get_value(), 40, "progressbar value correct")

  local clay = tree:to_clay()
  assert_eq(clay.type, "column", "declarative root is column")
  assert_eq(#clay, 2, "root has 2 top-level children in clay")
  print("  ✓ Declarative layout and widget tree ID lookup verified")
end

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 7: awful.popup & awful.tooltip Lifecycle
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 7] awful.popup & awful.tooltip Lifecycle...")
do
  local content = wibox.widget.textbox { text = "Popup Content" }
  local pop = awful.popup {
    widget = content,
    placement = "top_right",
    visible = false,
  }

  assert_eq(pop.visible, false, "popup initially hidden")
  pop:show(content, "bottom", 100, 200)
  assert_eq(pop.visible, true, "popup visible after show")
  pop:hide()
  assert_eq(pop.visible, false, "popup hidden after hide")

  local tip = awful.tooltip {
    text = "Tooltip Information",
    delay = 150,
  }
  assert_true(tip ~= nil, "tooltip instance created")
  tip:show(300, 400)
  assert_eq(tip.visible, true, "tooltip visible after show")
  tip:hide()
  assert_eq(tip.visible, false, "tooltip hidden after hide")

  print("  ✓ awful.popup and awful.tooltip lifecycle verified")
end

-- ────────────────────────────────────────────────────────────────────────────
-- TEST 8: Live Panel Composition & Compilation Check
-- ────────────────────────────────────────────────────────────────────────────
print("\n[TEST 8] Live Panel Composition & Compilation Check...")
do
  local ui = require("widgets.ui")

  -- Verify Card
  local card = ui.Card({ bg = "#181825", padding = 8 }, wibox.widget.textbox { text = "Inside Card" })
  local card_clay = card:to_clay()
  assert_eq(card_clay.type, "card", "ui.Card compiles to card clay element")

  -- Verify PanelHeader
  local hdr = ui.PanelHeader("Panel Title", "Panel Subtitle", function() end, "󰍜")
  local hdr_clay = hdr:to_clay()
  assert_eq(hdr_clay.type, "row", "ui.PanelHeader compiles to row clay element")

  -- Verify ToggleRow
  local toggle = ui.ToggleRow("Wi-Fi", "󰤨", true, function() end, "Connected")
  local toggle_clay = toggle:to_clay()
  assert_eq(toggle_clay.type, "card", "ui.ToggleRow compiles to card clay element")

  -- Verify SliderRow
  local srow = ui.SliderRow("Volume", "󰕾", 65, 0, 100, function() end, "%")
  local srow_clay = srow:to_clay()
  assert_eq(srow_clay.type, "card", "ui.SliderRow compiles to card clay element")

  -- Verify MetricRow
  local mrow = ui.MetricRow("󰍛", "CPU", "14%", 14, "#89b4fa")
  local mrow_clay = mrow:to_clay()
  assert_eq(mrow_clay.type, "column", "ui.MetricRow compiles to column clay element")

  print("  ✓ Migrated shell UI components compile cleanly to native Clay")
end

print("\n======================================================================")
print(string.format("PHASE 2 REGRESSION TESTS COMPLETED: %d passed, %d failed", pass_count, fail_count))
print("======================================================================")
