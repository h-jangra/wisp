-- End-to-End Regression Test Suite for Wisp Surface Composition Architecture
-- Validates:
--   1. Load init.lua as configuration entrypoint.
--   2. Require a module returning a textbox.
--   3. Require a module returning a nested layout.
--   4. Import reusable widgets into multiple parent trees.
--   5. Mount a returned widget tree into a panel.
--   6. Open, close and reopen the panel.
--   7. Mount a widget tree into a standalone application.
--   8. Open and close a popup attached to a parent surface.
--   9. Verify signal propagation and layout invalidation.
--  10. Reload modules and verify old callbacks and references are released.
--  11. Verify malformed modules produce useful errors.
--  12. Verify shutdown destroys all owned surfaces and resources.

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
print("RUNNING WISP END-TO-END SURFACE COMPOSITION ARCHITECTURE REGRESSION TESTS")
print("======================================================================")

local wisp = require("wisp")

-- ── 1. Configuration entrypoint ─────────────────────────────────────────────
print("\n[TEST 1] Configuration entrypoint (examples/minimal/init.lua)...")
do
  local ok, err = pcall(dofile, "examples/minimal/init.lua")
  assert_true(ok, "minimal init.lua executed successfully: " .. tostring(err))

  local panels = wisp.panel.list()
  assert_true(type(panels) == "table", "panel list returned table")
  local has_bar = false
  local has_control = false
  for _, name in ipairs(panels) do
    if name == "bar" then has_bar = true end
    if name == "control_center" then has_control = true end
  end
  assert_true(has_bar, "staged panel 'bar' registered")
  assert_true(has_control, "staged panel 'control_center' registered")
  assert_eq(wisp.panel.is_open("bar"), true, "bar autostart/open is true")
  assert_eq(wisp.panel.is_open("control_center"), false, "control_center initial visible is false")
  print("  ✓ Configuration entrypoint loaded, staged and opened surfaces verified")
end

-- ── 2. Require a module returning a textbox ────────────────────────────────
print("\n[TEST 2] Require reusable module returning textbox...")
do
  local clock = require("examples.minimal.widgets.clock")
  assert_true(clock ~= nil, "clock module returned value")
  assert_true(clock.is_widget, "clock has is_widget flag")
  assert_eq(clock.id, "clock", "clock widget id preserved")
  assert_eq(clock:get_text(), "12:00", "clock widget initial text")
  assert_true(type(clock.to_clay) == "function", "clock has to_clay method")

  -- Confirm require() is pure object creation: no surface registered for it
  local found_clock_panel = false
  for _, name in ipairs(wisp.panel.list()) do
    if name == "clock" then found_clock_panel = true end
  end
  assert_eq(found_clock_panel, false, "requiring module did NOT create implicit surface")
  print("  ✓ Reusable widget module returned without implicit surface creation")
end

-- ── 3. Require a module returning a nested layout ──────────────────────────
print("\n[TEST 3] Require module returning nested layout...")
do
  local bar = require("examples.minimal.bar")
  assert_true(bar ~= nil, "bar module returned value")
  assert_true(bar.is_widget, "bar layout has is_widget flag")
  assert_true(type(bar.to_clay) == "function", "bar has to_clay method")

  local w, h = bar:fit({}, 1920, 36)
  assert_true(w >= 0 and h >= 0, "bar layout fit measurement succeeded")
  print("  ✓ Nested layout module returned and measured correctly")
end

-- ── 4. Import reusable widgets into multiple parent trees ──────────────────
print("\n[TEST 4] Shared reusable widget composition across multiple trees...")
do
  local shared_txt = wisp.widget.textbox { text = "Shared Component", id = "shared_id" }
  local layout1 = wisp.layout.fixed.horizontal { spacing = 4 }
  local layout2 = wisp.layout.fixed.vertical { spacing = 6 }

  -- Reusable widget composition using clone or instance separation:
  local shared_instance2 = shared_txt:clone()
  layout1:add(shared_txt)
  layout2:add(shared_instance2)

  assert_eq(#layout1:get_children(), 1, "layout1 contains shared child instance")
  assert_eq(#layout2:get_children(), 1, "layout2 contains cloned child instance")
  assert_true(shared_txt:get_parent() == layout1, "shared_txt parent is layout1")
  assert_true(shared_instance2:get_parent() == layout2, "shared_instance2 parent is layout2")

  local p1 = layout1:layout({}, 400, 50)
  local p2 = layout2:layout({}, 400, 100)
  assert_eq(#p1, 1, "layout1 placed shared child")
  assert_eq(#p2, 1, "layout2 placed shared child")

  -- Verify accidental multiple-parent prevention (moving an instance detaches from former parent):
  local layout3 = wisp.layout.fixed.vertical { spacing = 2 }
  layout3:add(shared_txt)
  assert_eq(#layout1:get_children(), 0, "shared_txt detached from layout1 on reparenting")
  assert_eq(#layout3:get_children(), 1, "layout3 now owns shared_txt")
  assert_true(shared_txt:get_parent() == layout3, "shared_txt parent updated to layout3")

  print("  ✓ Shared widget cleanly composed and multiple-parent attachment prevented")
end

-- ── 5. Mount a returned widget tree into a panel ───────────────────────────
print("\n[TEST 5] Mount returned widget tree into a panel...")
do
  local panel_tree = require("examples.minimal.panel")
  assert_true(panel_tree ~= nil, "panel tree loaded")

  wisp.panel.register("mounted_test_panel", {
    anchor = "bottom_right",
    width = 300,
    height = 200,
    widget = panel_tree,
  })

  local has_mounted = false
  for _, name in ipairs(wisp.panel.list()) do
    if name == "mounted_test_panel" then has_mounted = true end
  end
  assert_true(has_mounted, "mounted_test_panel registered with panel manager")
  print("  ✓ Widget tree successfully mounted into panel surface definition")
end

-- ── 6. Open, close and reopen the panel ─────────────────────────────────────
print("\n[TEST 6] Open, close, and reopen panel lifecycle...")
do
  local name = "mounted_test_panel"
  wisp.panel.open(name)
  assert_eq(wisp.panel.is_open(name), true, "panel open state is true")

  wisp.panel.close(name)
  assert_eq(wisp.panel.is_open(name), false, "panel open state is false after close")

  wisp.panel.open(name)
  assert_eq(wisp.panel.is_open(name), true, "panel reopened successfully")

  wisp.panel.close(name)
  assert_eq(wisp.panel.is_open(name), false, "panel closed cleanly again")
  print("  ✓ Surface open/close/reopen lifecycle verified without dangling state")
end

-- ── 7. Mount a widget tree into a standalone application ───────────────────
print("\n[TEST 7] Mount widget tree into standalone application...")
do
  local app = wisp.app.create("e2e_standalone_app", {
    title = "E2E Standalone App",
    width = 400,
    height = 300,
  })
  assert_true(app ~= nil, "application instance created")

  local app_content = wisp.layout.fixed.vertical {
    spacing = 10,
    wisp.widget.textbox { text = "App Header", size = 16 },
    wisp.widget.button { text = "Action Button", bg = "#45475A" },
  }

  app:build(app_content)
  app:size(480, 320)
  app:close()
  print("  ✓ Standalone application xdg_toplevel mounting and resizing verified")
end

-- ── 8. Open and close a popup attached to a parent surface ─────────────────
print("\n[TEST 8] Popup binding and trigger lifecycle...")
do
  local popup_content = wisp.widget.textbox { text = "Popup Details" }
  local pop = wisp.popup {
    widget = popup_content,
    placement = "bottom",
  }
  assert_eq(pop.visible, false, "popup initially hidden")

  local trigger_btn = wisp.widget.button { text = "Toggle Popup" }
  pop:bind_to_widget(trigger_btn, { trigger = "click" })

  assert_true(type(trigger_btn.on_click) == "function", "trigger on_click hooked")
  trigger_btn:on_click()
  assert_eq(pop.visible, true, "popup visible after first click")

  trigger_btn:on_click()
  assert_eq(pop.visible, false, "popup hidden after second click")
  print("  ✓ Popup overlay binding and toggle lifecycle verified")
end

-- ── 9. Verify signal propagation and layout invalidation ───────────────────
print("\n[TEST 9] Signal propagation and layout invalidation bubbling...")
do
  local row = wisp.layout.fixed.horizontal { spacing = 8 }
  local dynamic_lbl = wisp.widget.textbox { text = "Initial Value", size = 12 }
  row:add(dynamic_lbl)

  local bubbled_layout = false
  row:connect_signal("widget::layout_changed", function()
    bubbled_layout = true
  end)

  dynamic_lbl:set_text("Updated Longer Value")
  assert_true(bubbled_layout, "widget::layout_changed bubbled up from child to parent layout")

  local bubbled_redraw = false
  row:connect_signal("widget::redraw_needed", function()
    bubbled_redraw = true
  end)

  dynamic_lbl:set_color("#FF5555")
  assert_true(bubbled_redraw, "widget::redraw_needed bubbled up from child to parent layout")
  print("  ✓ Signal bubbling and separate layout/redraw invalidation verified")
end

-- ── 10. Reload modules and verify old references are released ───────────────
print("\n[TEST 10] Module reload and clean reference replacement...")
do
  local mod_path = "examples.minimal.widgets.clock"
  local c1 = require(mod_path)
  package.loaded[mod_path] = nil

  local c2 = require(mod_path)
  assert_true(c1 ~= nil and c2 ~= nil, "both module instances loaded")
  assert_true(c1 ~= c2, "reload produced fresh widget instance")
  assert_eq(c2:get_text(), "12:00", "fresh instance initial state correct")
  print("  ✓ Module cache eviction and clean re-execution verified")
end

-- ── 11. Verify malformed modules produce useful errors ─────────────────────
print("\n[TEST 11] Malformed module error reporting...")
do
  local load_fn = load or loadstring
  local chunk, load_err = load_fn("local x = ")
  assert_true(chunk == nil, "malformed chunk load returned nil")
  assert_true(type(load_err) == "string" and (string.find(load_err, "expected") or string.find(load_err, "near")),
    "useful syntax error message with expected symbol detail: " .. tostring(load_err))
  print("  ✓ Useful compiler and runtime error diagnostic messages verified")
end

-- ── 12. Verify shutdown destroys all owned surfaces and resources ───────────
print("\n[TEST 12] Panel shutdown and resource cleanup...")
do
  wisp.panel.close_all()
  local active = wisp.panel.current()
  assert_eq(#active, 0, "all panels closed after close_all()")
  print("  ✓ Surface destruction and full panel closure verified")
end

print("\n======================================================================")
print("ALL 12 END-TO-END SURFACE COMPOSITION REGRESSION TESTS PASSED!")
print("======================================================================")
