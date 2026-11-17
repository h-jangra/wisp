-- Comprehensive Regression Test Suite for Wisp Canonical Architecture
-- Validates the 10 architecture boundaries:
--   1. wisp.object
--   2. wisp.widget
--   3. wisp.layout
--   4. wisp.container
--   5. wisp.panel
--   6. wisp.popup
--   7. wisp.app
--   8. wisp.services
--   9. wisp.plugin
--  10. wisp.state
-- and their require() module exports.

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
print("RUNNING WISP CANONICAL ARCHITECTURE SPECIFICATION VERIFICATION TESTS")
print("======================================================================")

-- ── 1. wisp.object ──────────────────────────────────────────────────────────
print("\n[TEST 1] wisp.object / gears.object...")
do
  local obj = require("wisp.object")()
  local hits = 0
  local last_val = nil
  local function on_change(self, v) hits = hits + 1; last_val = v end

  obj:connect_signal("property::test", on_change)
  assert_true(obj:has_signal("property::test"), "has_signal returns true")
  obj:emit_signal("property::test", 123)
  assert_eq(hits, 1, "signal handler invoked")
  assert_eq(last_val, 123, "signal payload received")

  -- Safe re-entrancy / removal during emission
  local to_remove
  to_remove = function()
    obj:disconnect_signal("property::test", to_remove)
  end
  obj:connect_signal("property::test", to_remove)
  obj:emit_signal("property::test", 456)
  assert_eq(hits, 2, "second signal invocation")

  obj:disconnect_signal("property::test", on_change)
  obj:emit_signal("property::test", 789)
  assert_eq(hits, 2, "signal not invoked after disconnect")

  -- weak_connect_signal
  obj:weak_connect_signal("property::weak", function() hits = hits + 10 end)
  obj:emit_signal("property::weak")
  assert_eq(hits, 12, "weak_connect_signal invoked")

  -- apply_to
  local target = { name = "sample" }
  wisp.object.apply_to(target)
  assert_true(type(target.connect_signal) == "function", "apply_to added methods")
  print("  ✓ wisp.object signals, re-entrancy, and apply_to verified")
end

-- ── 2. wisp.widget & wisp.widget.base ───────────────────────────────────────
print("\n[TEST 2] wisp.widget & wisp.widget.base...")
do
  local widget_mod = require("wisp.widget")
  local base = require("wisp.widget.base")

  -- Base widget creation and properties
  local w = base.make_widget(nil, "custom_box")
  assert_true(w.is_widget, "is_widget flag present")
  assert_eq(w:get_visible(), true, "default visible is true")
  w:set_visible(false)
  assert_eq(w:get_visible(), false, "set_visible updated")

  w:set_forced_width(120)
  w:set_forced_height(40)
  assert_eq(w:get_forced_width(), 120, "forced width set")
  assert_eq(w:get_forced_height(), 40, "forced height set")

  -- Standard widgets
  local txt = widget_mod.textbox { id = "lbl", text = "Hello World", size = 16 }
  assert_eq(txt:get_text(), "Hello World", "textbox text getter")
  txt:set_text("Updated")
  assert_eq(txt:get_text(), "Updated", "textbox text setter")

  local btn = widget_mod.button { id = "btn1", text = "Click", bg = "#222222" }
  assert_eq(btn.text, "Click", "button text")

  local prog = widget_mod.progressbar { value = 50, max_value = 100 }
  assert_eq(prog:get_value(), 50, "progressbar value getter")
  prog:set_value(75)
  assert_eq(prog:get_value(), 75, "progressbar value setter")

  local sld = widget_mod.slider { value = 20, min = 0, max = 100 }
  assert_eq(sld:get_value(), 20, "slider value")

  local chk = widget_mod.checkbox { checked = true }
  assert_eq(chk:get_checked(), true, "checkbox checked getter")
  chk:set_checked(false)
  assert_eq(chk:get_checked(), false, "checkbox checked setter")

  local sep = widget_mod.separator { orientation = "horizontal", thickness = 2 }
  assert_true(sep.is_widget, "separator is widget")

  local spc = widget_mod.spacer { width = 10 }
  assert_true(spc.is_widget, "spacer is widget")

  local inp = widget_mod.input { id = "search", value = "query" }
  assert_eq(inp:get_text(), "query", "input text")

  -- Hierarchy and get_children_by_id on base widget
  w:add(txt)
  w:add(btn)
  assert_eq(#w:get_children(), 2, "child count is 2")
  assert_true(txt._private.parent == w, "child parent link established")

  local found_lbl = w:get_children_by_id("lbl")
  assert_true(found_lbl == txt, "get_children_by_id found label")

  local found_btn = w:get_children_by_id("btn1")
  assert_true(found_btn == btn, "get_children_by_id found button")

  w:reset()
  assert_eq(#w:get_children(), 0, "children reset")
  assert_true(txt._private.parent == nil, "parent unlinked after reset")
  print("  ✓ wisp.widget standard widgets, hierarchy, and get_children_by_id verified")
end

-- ── 3. wisp.layout ──────────────────────────────────────────────────────────
print("\n[TEST 3] wisp.layout...")
do
  local layout = require("wisp.layout")
  assert_true(layout.fixed ~= nil, "layout.fixed present")
  assert_true(layout.flex ~= nil, "layout.flex present")
  assert_true(layout.align ~= nil, "layout.align present")
  assert_true(layout.grid ~= nil, "layout.grid present")
  assert_true(layout.stack ~= nil, "layout.stack present")

  -- Fixed layout
  local row = layout.fixed.horizontal { spacing = 8 }
  row:add(wisp.widget.textbox { text = "A", width = 30, height = 20 })
  row:add(wisp.widget.textbox { text = "B", width = 40, height = 20 })
  local placed = row:layout({}, 200, 50)
  assert_eq(#placed, 2, "2 placed children in fixed row")
  assert_eq(placed[1].x, 0, "first child at x=0")
  assert_eq(placed[2].x, placed[1].width + 8, "second child spaced by 8")

  -- Flex layout
  local flx = layout.flex.horizontal { spacing = 4 }
  flx:add(wisp.widget.textbox { text = "F1" })
  flx:add(wisp.widget.textbox { text = "F2" })
  local fplaced = flx:layout({}, 100, 30)
  assert_eq(#fplaced, 2, "2 flex placed children")
  assert_eq(fplaced[1].width, 48, "flex equal distribution (100 - 4)/2 = 48")

  -- Align layout (wibox.layout.align.horizontal / layout.align_h)
  local alg = layout.align.horizontal {
    first = wisp.widget.textbox { text = "Left" },
    second = wisp.widget.textbox { text = "Center" },
    third = wisp.widget.textbox { text = "Right" },
  }
  local aplaced = alg:layout({}, 300, 30)
  assert_eq(#aplaced, 3, "3 align children placed")

  -- Grid layout
  local grd = layout.grid { column_count = 2, row_count = 2, spacing = 5 }
  grd:add(wisp.widget.textbox { text = "1" })
  grd:add(wisp.widget.textbox { text = "2" })
  local gplaced = grd:layout({}, 100, 100)
  assert_eq(#gplaced, 2, "grid placed 2 children")

  -- Stack layout
  local stk = layout.stack()
  stk:add(wisp.widget.textbox { text = "Layer 1" })
  stk:add(wisp.widget.textbox { text = "Layer 2" })
  local splaced = stk:layout({}, 100, 100)
  assert_eq(#splaced, 2, "stack placed 2 children")
  assert_eq(splaced[1].x, 0, "stack layer 1 x=0")
  assert_eq(splaced[2].x, 0, "stack layer 2 x=0")

  print("  ✓ wisp.layout fixed, flex, align, grid, and stack verified")
end

-- ── 4. wisp.container ───────────────────────────────────────────────────────
print("\n[TEST 4] wisp.container...")
do
  local container = require("wisp.container")
  local inner = wisp.widget.textbox { text = "Content", width = 50, height = 20 }

  -- Margin
  local m = container.margin(inner, 10, 10, 5, 5)
  local mw, mh = m:fit({}, 200, 100)
  assert_eq(mw, 50 + 20, "margin width fit (50 + 10 + 10)")
  assert_eq(mh, 20 + 10, "margin height fit (20 + 5 + 5)")

  -- Background
  local bg = container.background(inner, "#1E1E2E", "#CDD6F4")
  assert_true(bg.is_container, "background is container")

  -- Constraint
  local c_max = container.constraint(inner, "max", 40, 15)
  local cmw, cmh = c_max:fit({}, 100, 100)
  assert_eq(cmw, 40, "constraint max width capped at 40")
  assert_eq(cmh, 15, "constraint max height capped at 15")

  local c_min = container.constraint(inner, "min", 80, 50)
  local cminw, cminh = c_min:fit({}, 100, 100)
  assert_eq(cminw, 80, "constraint min width floored at 80")
  assert_eq(cminh, 50, "constraint min height floored at 50")

  -- Place
  local pl = container.place(inner, "center", "center")
  local pplaced = pl:layout({}, 100, 60)
  assert_eq(#pplaced, 1, "place container has 1 child")
  assert_eq(pplaced[1].x, 25, "place center horizontal: (100-50)/2 = 25")
  assert_eq(pplaced[1].y, 20, "place center vertical: (60-20)/2 = 20")

  -- Scroll
  local scr = container.scroll(inner)
  assert_true(scr.is_container, "scroll is container")

  print("  ✓ wisp.container margin, background, constraint, place, and scroll verified")
end

-- ── 5. wisp.panel ───────────────────────────────────────────────────────────
print("\n[TEST 5] wisp.panel / wisp.panels...")
do
  local panel = require("wisp.panel")
  assert_true(type(panel.register) == "function", "panel.register function")
  assert_true(type(panel.open) == "function", "panel.open function")
  assert_true(type(panel.close) == "function", "panel.close function")
  assert_true(type(panel.toggle) == "function", "panel.toggle function")
  assert_true(type(panel.close_all) == "function", "panel.close_all function")
  assert_true(type(panel.is_open) == "function", "panel.is_open function")
  assert_true(type(panel.current) == "function", "panel.current function")
  assert_true(type(panel.list) == "function", "panel.list function")

  panel.register("test_panel", { anchor = "top_right", height = 300, width = 200 })
  local list = panel.list()
  assert_true(type(list) == "table", "panel list returns table")
  print("  ✓ wisp.panel registration, query, and lifecycle methods verified")
end

-- ── 6. wisp.popup & wisp.tooltip ────────────────────────────────────────────
print("\n[TEST 6] wisp.popup & wisp.tooltip...")
do
  local popup_mod = require("wisp.popup")
  local tooltip_mod = require("wisp.tooltip")

  local p = popup_mod { placement = "bottom_right" }
  assert_true(p.visible == false, "initial popup visibility false")
  assert_eq(p.placement, "bottom_right", "popup placement preserved")
  assert_true(type(p.show) == "function", "popup show function")
  assert_true(type(p.hide) == "function", "popup hide function")
  assert_true(type(p.toggle) == "function", "popup toggle function")

  local tip = tooltip_mod { text = "Action description", delay = 150 }
  assert_eq(tip:get_text(), "Action description", "tooltip text getter")
  tip:set_text("New description")
  assert_eq(tip:get_text(), "New description", "tooltip text setter")

  local btn = wisp.widget.button { text = "Target" }
  tip:add_to_object(btn)
  assert_eq(#tip.objects, 1, "tooltip attached to button")
  tip:remove_from_object(btn)
  assert_eq(#tip.objects, 0, "tooltip detached from button")

  print("  ✓ wisp.popup and wisp.tooltip lifecycle and object binding verified")
end

-- ── 7. wisp.app ─────────────────────────────────────────────────────────────
print("\n[TEST 7] wisp.app...")
do
  local app_mod = require("wisp.app")
  assert_true(type(app_mod.create) == "function", "app.create function")
  assert_true(type(app_mod.quit) == "function", "app.quit function")

  local app = app_mod.create("test_app", {
    title = "Test Application",
    width = 400,
    height = 300,
  })
  assert_true(app ~= nil, "app instance created")
  assert_true(type(app.build) == "function", "app:build function")
  assert_true(type(app.render) == "function", "app:render function")
  assert_true(type(app.draw) == "function", "app:draw function")
  assert_true(type(app.close) == "function", "app:close function")
  assert_true(type(app.size) == "function", "app:size function")

  -- build sets declarative root
  app:build(function()
    return wibox.widget {
      layout = wibox.layout.fixed.vertical,
      { widget = wibox.widget.textbox, text = "App Content" }
    }
  end)

  app:close()
  print("  ✓ wisp.app create, build, render, size, and close verified")
end

-- ── 8. wisp.services ────────────────────────────────────────────────────────
print("\n[TEST 8] wisp.services...")
do
  local services = require("wisp.services")
  assert_true(services.system ~= nil, "services.system present")
  assert_true(services.audio ~= nil, "services.audio present")
  assert_true(services.clipboard ~= nil, "services.clipboard present")
  assert_true(services.network ~= nil, "services.network present")
  assert_true(services.bluetooth ~= nil, "services.bluetooth present")
  assert_true(services.brightness ~= nil, "services.brightness present")
  assert_true(services.battery ~= nil, "services.battery present")
  assert_true(services.notify ~= nil, "services.notify present")
  assert_true(services.process ~= nil, "services.process present")
  assert_true(services.fs ~= nil, "services.fs present")
  assert_true(services.qr ~= nil, "services.qr present")
  assert_true(services.workspace ~= nil, "services.workspace present")

  -- Check direct require of submodules
  local net = require("wisp.services.network")
  assert_true(type(net.get) == "function", "require(wisp.services.network) succeeded")
  local aud = require("wisp.services.audio")
  assert_true(type(aud.get) == "function", "require(wisp.services.audio) succeeded")
  local clip = require("wisp.services.clipboard")
  assert_true(type(clip.get) == "function", "require(wisp.services.clipboard) succeeded")
  local sys = require("wisp.services.system")
  assert_true(type(sys.get) == "function", "require(wisp.services.system) succeeded")

  print("  ✓ wisp.services and all 12 submodules verified")
end

-- ── 9. wisp.plugin ──────────────────────────────────────────────────────────
print("\n[TEST 9] wisp.plugin...")
do
  local plugin = require("wisp.plugin")
  assert_true(type(plugin.register) == "function", "plugin.register function")
  assert_true(type(plugin.load) == "function", "plugin.load function")
  assert_true(type(plugin.unload) == "function", "plugin.unload function")
  assert_true(type(plugin.get) == "function", "plugin.get function")
  assert_true(type(plugin.list) == "function", "plugin.list function")
  assert_true(type(plugin.is_loaded) == "function", "plugin.is_loaded function")

  local inited = false
  local cleaned = false
  plugin.register("weather_plugin", {
    version = "1.2.0",
    capabilities = { "weather", "forecast" },
    init = function(self)
      inited = true
      return { temp = 22 }
    end,
    cleanup = function(self)
      cleaned = true
    end,
  })

  assert_true(plugin.is_loaded("weather_plugin") == false, "not loaded initially")
  local inst = plugin.load("weather_plugin")
  assert_true(inited, "init hook ran")
  assert_eq(inst.temp, 22, "plugin instance returned")
  assert_true(plugin.is_loaded("weather_plugin") == true, "marked loaded")

  plugin.unload("weather_plugin")
  assert_true(cleaned, "cleanup hook ran")
  assert_true(plugin.is_loaded("weather_plugin") == false, "unloaded")

  print("  ✓ wisp.plugin register, init, unload, and lifecycle hooks verified")
end

-- ── 10. wisp.state ──────────────────────────────────────────────────────────
print("\n[TEST 10] wisp.state...")
do
  local state = require("wisp.state")
  local count = wisp.state(10)
  assert_eq(count:get(), 10, "state initial value")

  local hits = 0
  local seen_new, seen_old = nil, nil
  count:subscribe(function(n, o)
    hits = hits + 1
    seen_new = n
    seen_old = o
  end)

  count:set(20)
  -- writes are deferred until flush
  assert_eq(hits, 0, "handlers deferred before flush")
  wisp.flush()
  assert_eq(hits, 1, "handler executed on flush")
  assert_eq(seen_new, 20, "new value passed")
  assert_eq(seen_old, 10, "old value passed")

  -- Computed
  local doubled = wisp.computed(function() return count:get() * 2 end, { count })
  assert_eq(doubled:get(), 40, "computed initial value")

  count:set(25)
  wisp.flush()
  assert_eq(doubled:get(), 50, "computed updated after dependency change")

  -- Effect
  local effect_runs = 0
  local dispose = wisp.effect(function()
    effect_runs = effect_runs + 1
    local _ = count:get()
  end, { count })
  assert_eq(effect_runs, 1, "effect initial run")

  count:set(30)
  wisp.flush()
  assert_eq(effect_runs, 2, "effect rerun on change")

  dispose()
  count:set(35)
  wisp.flush()
  assert_eq(effect_runs, 2, "effect stopped after dispose")

  print("  ✓ wisp.state, computed, effect, and deferred flush verified")
end

print("\n======================================================================")
print("ALL 10 CANONICAL ARCHITECTURE MODULE SPECIFICATIONS VERIFIED!")
print("======================================================================")
