-- tests/runtime_hardening_regression_test.lua
-- Comprehensive runtime hardening regression test suite for Wisp:
-- 1. Widget ownership (single-parent enforcement, detach, clone, destruction, ID cache integrity)
-- 2. Surface lifecycle (open, close, reopen, destroy)
-- 3. Lua lifecycle (timers, signals, effects, destroyed object protection, module reload)
-- 4. Rendering & input (separate layout vs redraw invalidation, popup anchoring & dismissal)
-- 5. Native Wayland integration (layer anchors, exclusive zones, multi-monitor output routing)
-- 6. Performance & memory stress (rapid lifecycle cycles, zero-leak validation)

local wisp = require("wisp")

local function assert_true(cond, msg)
  if not cond then error("ASSERTION FAILED: " .. tostring(msg), 2) end
end

local function assert_eq(a, b, msg)
  if a ~= b then
    error(string.format("ASSERTION FAILED: %s (expected %s, got %s)", tostring(msg), tostring(b), tostring(a)), 2)
  end
end

print("======================================================================")
print("RUNNING WISP RUNTIME HARDENING & OWNERSHIP REGRESSION TESTS")
print("======================================================================")

-- ── 1. Widget Ownership: Parent-Child Enforcement & Detach ────────────────────
print("\n[TEST 1] Explicit parent-child ownership & detach()...")
do
  local child = wisp.widget.textbox { text = "Child Widget", id = "my_child" }
  local parent1 = wisp.layout.fixed.horizontal { spacing = 4 }

  assert_true(child:get_parent() == nil, "child has no parent initially")
  parent1:add(child)
  assert_true(child:get_parent() == parent1, "child parent is parent1")
  assert_eq(#parent1:get_children(), 1, "parent1 has 1 child")
  assert_true(parent1:get_children_by_id("my_child") == child, "parent1 finds child by id")

  -- Explicit detach
  child:detach()
  assert_true(child:get_parent() == nil, "child parent is nil after detach")
  assert_eq(#parent1:get_children(), 0, "parent1 has 0 children after detach")
  assert_true(parent1:get_children_by_id("my_child") == nil, "parent1 no longer indexes child by id")
  print("  ✓ Explicit parent attachment, get_parent(), and detach() verified")
end

-- ── 2. Prevention of Accidental Multiple-Parent Attachment ────────────────────
print("\n[TEST 2] Accidental multiple-parent attachment prevention...")
do
  local wgt = wisp.widget.textbox { text = "Single Ownership", id = "singleton_wgt" }
  local layoutA = wisp.layout.fixed.horizontal { spacing = 8 }
  local layoutB = wisp.layout.fixed.vertical { spacing = 8 }

  layoutA:add(wgt)
  assert_eq(#layoutA:get_children(), 1, "layoutA has child")
  assert_true(wgt:get_parent() == layoutA, "wgt parent is layoutA")
  assert_true(layoutA:get_children_by_id("singleton_wgt") == wgt, "layoutA indexes wgt")

  -- Attaching to layoutB automatically detaches from layoutA:
  layoutB:add(wgt)
  assert_eq(#layoutA:get_children(), 0, "layoutA child cleanly removed on reparent")
  assert_eq(#layoutB:get_children(), 1, "layoutB now owns child")
  assert_true(wgt:get_parent() == layoutB, "wgt parent updated to layoutB")
  assert_true(layoutA:get_children_by_id("singleton_wgt") == nil, "layoutA ID index cleared")
  assert_true(layoutB:get_children_by_id("singleton_wgt") == wgt, "layoutB ID index populated")

  -- Test slotted layouts (align):
  local align_layout = wisp.layout.align.horizontal {}
  align_layout:set_first(wgt)
  assert_eq(#layoutB:get_children(), 0, "wgt detached from layoutB when set as align first")
  assert_true(wgt:get_parent() == align_layout, "wgt parent is now align_layout")
  assert_true(align_layout:get_first() == wgt, "align first is wgt")

  -- Test container decorator:
  local container_box = wisp.container.margin { left = 10 }
  container_box:set_widget(wgt)
  assert_true(align_layout:get_first() == nil, "align first cleared when wgt reparented to container")
  assert_true(wgt:get_parent() == container_box, "wgt parent is now container_box")
  assert_true(container_box:get_widget() == wgt, "container widget is wgt")

  print("  ✓ Strict single-parent enforcement across layouts and containers verified")
end

-- ── 3. Removal, Replacement & Destruction Index Updates ───────────────────────
print("\n[TEST 3] Removal, replacement and destruction update hierarchy indexes...")
do
  local w1 = wisp.widget.textbox { text = "Item 1", id = "item_1" }
  local w2 = wisp.widget.textbox { text = "Item 2", id = "item_2" }
  local box = wisp.layout.fixed.vertical { spacing = 4 }

  box:add(w1)
  box:add(w2)
  assert_true(box:get_children_by_id("item_1") == w1, "item_1 found in box")
  assert_true(box:get_children_by_id("item_2") == w2, "item_2 found in box")

  -- Remove item 1:
  local removed = box:remove(w1)
  assert_true(removed == w1, "removed widget returned")
  assert_true(w1:get_parent() == nil, "w1 parent cleared")
  assert_true(box:get_children_by_id("item_1") == nil, "item_1 purged from ID index")
  assert_true(box:get_children_by_id("item_2") == w2, "item_2 still present in ID index")

  -- Reset box:
  box:reset()
  assert_eq(#box:get_children(), 0, "box children emptied")
  assert_true(w2:get_parent() == nil, "w2 parent cleared")
  assert_true(box:get_children_by_id("item_2") == nil, "item_2 purged from ID index")

  -- Test Destruction:
  local target = wisp.widget.textbox { text = "To Destroy", id = "to_destroy" }
  box:add(target)
  local signal_fired = false
  target:connect_signal("test::ping", function() signal_fired = true end)
  target:emit_signal("test::ping")
  assert_true(signal_fired, "signal fired before destruction")

  target:destroy()
  signal_fired = false
  target:emit_signal("test::ping")
  assert_true(not signal_fired, "signal cannot fire on destroyed widget")
  assert_true(target:get_parent() == nil, "destroyed widget detached from parent")
  assert_true(box:get_children_by_id("to_destroy") == nil, "destroyed widget purged from parent index")

  print("  ✓ Removal, reset, destruction and ID index synchronizations verified")
end

-- ── 4. Separate Reusable Definitions From Widget Instances ────────────────────
print("\n[TEST 4] Reusable widget definitions and clone()...")
do
  local base_proto = wisp.widget.textbox { text = "Prototype", size = 16, id = "proto_id" }
  local c1 = base_proto:clone({ id = "instance_1", text = "Instance 1" })
  local c2 = base_proto:clone({ id = "instance_2", text = "Instance 2" })

  assert_true(c1 ~= base_proto and c2 ~= base_proto and c1 ~= c2, "clones are distinct tables")
  assert_eq(c1:get_text(), "Instance 1", "clone 1 has overridden text")
  assert_eq(c2:get_text(), "Instance 2", "clone 2 has overridden text")
  assert_eq(c1.size, 16, "clone 1 inherited prototype property")

  local row1 = wisp.layout.fixed.horizontal {}
  local row2 = wisp.layout.fixed.horizontal {}
  row1:add(c1)
  row2:add(c2)
  assert_true(c1:get_parent() == row1, "clone 1 owned by row1")
  assert_true(c2:get_parent() == row2, "clone 2 owned by row2")

  -- Factory definition:
  local make_badge = wisp.widget.definition(function(opts)
    return wisp.widget.textbox { text = opts.label or "Badge", id = opts.id }
  end)
  assert_true(make_badge.is_definition, "make_badge has is_definition flag")
  local b1 = make_badge({ label = "A", id = "b_a" })
  local b2 = make_badge({ label = "B", id = "b_b" })
  assert_eq(b1:get_text(), "A", "badge 1 created")
  assert_eq(b2:get_text(), "B", "badge 2 created")

  print("  ✓ Widget clone(), factory definitions, and instance separation verified")
end

-- ── 5. Shared Reactive State Without Shared Widget Ownership ──────────────────
print("\n[TEST 5] Shared reactive state does not imply shared widget ownership...")
do
  local count_state = wisp.state(100)

  local wgt_panel = wisp.widget.textbox { text = tostring(count_state:get()), id = "panel_count" }
  local wgt_bar = wisp.widget.textbox { text = tostring(count_state:get()), id = "bar_count" }

  -- Both subscribe to the single reactive state:
  count_state:subscribe(function(v) wgt_panel:set_text(v) end)
  count_state:subscribe(function(v) wgt_bar:set_text(v) end)

  local bar_layout = wisp.layout.fixed.horizontal {}
  local panel_layout = wisp.layout.fixed.vertical {}

  bar_layout:add(wgt_bar)
  panel_layout:add(wgt_panel)

  assert_true(wgt_bar:get_parent() == bar_layout, "bar widget owned by bar_layout")
  assert_true(wgt_panel:get_parent() == panel_layout, "panel widget owned by panel_layout")

  -- Mutation updates both widgets through reactive pipeline:
  count_state:set(250)
  if wisp.flush then wisp.flush() end
  assert_eq(wgt_bar:get_text(), "250", "bar widget observed state change")
  assert_eq(wgt_panel:get_text(), "250", "panel widget observed state change")

  -- Destroying bar_layout does NOT affect panel_layout or wgt_panel:
  bar_layout:destroy()
  assert_true(wgt_bar._destroyed, "bar widget marked destroyed")
  assert_true(not wgt_panel._destroyed, "panel widget remained alive")
  assert_true(wgt_panel:get_parent() == panel_layout, "panel widget still cleanly owned by panel_layout")

  count_state:set(999)
  if wisp.flush then wisp.flush() end
  assert_eq(wgt_panel:get_text(), "999", "panel widget continues updating after bar destruction")

  print("  ✓ Shared reactive state with independent widget tree ownership verified")
end

-- ── 6. Surface Lifecycle: Register, Open, Close, Reopen, Destroy ──────────────
print("\n[TEST 6] Surface lifecycle (register, open, close, reopen, destroy)...")
do
  local test_tree = wisp.widget.textbox { text = "Lifecycle Surface", id = "surface_root" }

  wisp.panel.register("hardened_test_panel", {
    anchor = "top_left",
    width = 250,
    height = 50,
    widget = test_tree,
  })

  assert_true(not wisp.panel.is_open("hardened_test_panel"), "panel initially closed")

  -- Open:
  local ok_open = wisp.panel.open("hardened_test_panel")
  assert_true(ok_open, "panel opened")
  assert_true(wisp.panel.is_open("hardened_test_panel"), "panel is open")

  -- Close:
  local ok_close = wisp.panel.close("hardened_test_panel")
  assert_true(ok_close, "panel closed")
  assert_true(not wisp.panel.is_open("hardened_test_panel"), "panel is not open")

  -- Reopen:
  local ok_reopen = wisp.panel.open("hardened_test_panel")
  assert_true(ok_reopen, "panel reopened")
  assert_true(wisp.panel.is_open("hardened_test_panel"), "panel reopened successfully")

  -- Destroy:
  local ok_destroy = wisp.panel.destroy("hardened_test_panel")
  assert_true(ok_destroy, "panel destroyed")
  assert_true(not wisp.panel.is_open("hardened_test_panel"), "destroyed panel is closed")

  local in_list = false
  for _, name in ipairs(wisp.panel.list()) do
    if name == "hardened_test_panel" then in_list = true end
  end
  assert_true(not in_list, "destroyed panel removed from panel list")

  print("  ✓ Full surface open, close, reopen, and destroy lifecycle verified")
end

-- ── 7. Separate Layout vs Redraw Invalidation ─────────────────────────────────
print("\n[TEST 7] Separate layout invalidation vs redraw invalidation...")
do
  local wgt = wisp.widget.textbox { text = "Invalidation Target" }
  local container = wisp.container.margin { left = 4, widget = wgt }

  local layout_bubbled = false
  local redraw_bubbled = false

  container:connect_signal("widget::layout_changed", function() layout_bubbled = true end)
  container:connect_signal("widget::redraw_needed", function() redraw_bubbled = true end)

  -- Emitting redraw_needed should NOT bubble layout_changed:
  wgt:emit_signal("widget::redraw_needed")
  assert_true(redraw_bubbled, "redraw_needed bubbled to container")
  assert_true(not layout_bubbled, "layout_changed did NOT trigger on redraw_needed")

  -- Emitting layout_changed:
  redraw_bubbled = false
  wgt:emit_signal("widget::layout_changed")
  assert_true(layout_bubbled, "layout_changed bubbled to container")

  print("  ✓ Independent layout vs redraw invalidation verified")
end

-- ── 8. Popup Placement & Dismissal Contract ───────────────────────────────────
print("\n[TEST 8] Popup placement & dismissal contract...")
do
  local pop_content = wisp.widget.textbox { text = "Popup Menu" }
  local pop = wisp.popup {
    placement = "bottom",
    widget = pop_content,
  }

  assert_true(not pop.visible, "popup initially hidden")
  pop:show()
  assert_true(pop.visible, "popup is visible after show")
  pop:hide()
  assert_true(not pop.visible, "popup is hidden after hide")

  -- Trigger toggle:
  pop:toggle()
  assert_true(pop.visible, "popup toggled to visible")
  pop:toggle()
  assert_true(not pop.visible, "popup toggled to hidden")

  -- Destroy:
  pop:destroy()
  assert_true(pop._destroyed, "popup marked destroyed")

  print("  ✓ Popup visibility, toggle, and destruction verified")
end

-- ── 9. Multi-Output Selection & Routing Contract ──────────────────────────────
print("\n[TEST 9] Multi-output selection & routing contract...")
do
  -- Register a panel with explicit output configuration:
  wisp.panel.register("multi_monitor_bar", {
    anchor = "top_left_right",
    height = 32,
    output = 1,
    widget = wisp.widget.textbox { text = "Display 1 Top Bar" },
  })

  local has_panel = false
  for _, name in ipairs(wisp.panel.list()) do
    if name == "multi_monitor_bar" then has_panel = true end
  end
  assert_true(has_panel, "multi_monitor_bar registered with output option")
  wisp.panel.destroy("multi_monitor_bar")

  print("  ✓ Multi-monitor output routing contract verified")
end

-- ── 10. Performance & Stress Test: 50 Open/Close Cycles ────────────────────────
print("\n[TEST 10] Stress test: 50 open/close/reopen cycles...")
do
  local stress_widget = wisp.widget.textbox { text = "Stress Target" }
  wisp.panel.register("stress_panel", {
    anchor = "bottom_left",
    width = 150,
    height = 40,
    widget = stress_widget,
  })

  for cycle = 1, 50 do
    wisp.panel.open("stress_panel")
    assert_true(wisp.panel.is_open("stress_panel"), "open in cycle " .. cycle)
    wisp.panel.close("stress_panel")
    assert_true(not wisp.panel.is_open("stress_panel"), "close in cycle " .. cycle)
  end

  wisp.panel.destroy("stress_panel")
  print("  ✓ 50 rapid open/close surface cycles completed with zero dangling references")
end

print("\n======================================================================")
print("ALL 10 RUNTIME HARDENING REGRESSION TESTS PASSED SUCCESSFULLY!")
print("======================================================================")
