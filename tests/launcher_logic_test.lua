-- Headless launcher logic checks (filtering, selection clamp, submit fallback)
package.path = "shell/?.lua;shell/?/init.lua;" .. package.path

local function filter(items, query)
  local q = (query or ""):lower()
  local out = {}
  for _, app in ipairs(items) do
    local hay = (app.name .. " " .. (app.comment or "") .. " " .. (app.category or "")):lower()
    if q == "" or hay:find(q, 1, true) then out[#out + 1] = app end
  end
  return out
end

local function clamp(selected, n)
  if n == 0 then return 1 end
  if selected < 1 then return 1 end
  if selected > n then return n end
  return selected
end

local function submit_pick(list, selected)
  return list[selected] or list[1]
end

local items = {
  {name="Alpha", comment="first", category="Utility", exec="a"},
  {name="Beta", comment="media tool", category="Media", exec="b"},
  {name="Gamma", comment="", category="Dev", exec="c"},
}

-- filtering
assert(#filter(items, "") == 3, "empty query")
assert(#filter(items, "alp") == 1, "name match")
assert(#filter(items, "media") == 1, "comment/category match")
assert(#filter(items, "zzz") == 0, "no match")

-- Up/Down clamping
assert(clamp(0, 3) == 1, "clamp low")
assert(clamp(5, 3) == 3, "clamp high")
assert(clamp(2, 0) == 1, "clamp empty")
assert(clamp(-5, 0) == 1, "clamp empty low")

-- selection after refresh/filter (list shrinks)
local q = "a" -- Alpha, Beta, Gamma all contain a
local list = filter(items, q)
local sel = 3
list = filter(items, "alp") -- shrinks to 1
sel = clamp(sel, #list)
assert(sel == 1 and #list == 1, "clamp after filter shrink")
list = filter(items, "zzz")
sel = clamp(sel, #list)
assert(sel == 1 and #list == 0, "clamp after empty")

-- Enter fallback
local l2 = filter(items, "")
assert(submit_pick(l2, 2).name == "Beta", "selected pick")
assert(submit_pick(l2, 99).name == "Alpha", "fallback to first")
assert(submit_pick({}, 1) == nil, "empty submit")

-- callback/teardown simulation (no stale state on close/reopen)
local opened, closed, launched = 0, 0, {}
local function open() opened = opened + 1 end
local function close() closed = closed + 1 end
local function launch(app) if app then launched[#launched+1] = app.exec end; close() end
open(); launch(submit_pick(l2, 1)); open(); launch(submit_pick(filter(items,"zzz"), 1)); close()
assert(opened == 2 and closed == 3, "open/close callbacks")
assert(#launched == 1 and launched[1] == "a", "launch only valid selection")

print("launcher logic: ok")
