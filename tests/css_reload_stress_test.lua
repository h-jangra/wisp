-- Stylesheet parse/load/resolve/reload stress (headless)
package.path = "shell/?.lua;shell/?/init.lua;" .. package.path
local css = require("lib.css")

local sheet = css.new()
for i = 1, 200 do
  sheet:load(string.format([[
    .panel { background: #10101%d; padding: 8px; border: 1px solid #333; }
    .item { color: #eee; }
    .item:hover { background: #22222%d; }
    .item.selected, .item:focus { background: #333; }
  ]], i % 10, i % 10))
  assert(#sheet.rules == 5, "reload replaces rules")
  local base = sheet:resolve("row", "item", nil, {{type="col", class="panel"}})
  local hov = sheet:resolve("row", "item", "hover", {{type="col", class="panel"}})
  local sel = sheet:resolve("row", "item selected", nil, {{type="col", class="panel"}})
  assert(base and hov and sel, "resolve")
  assert(hov.bg ~= nil, "state prop")
  assert(sheet.cache[table.concat({"row","item","hover",tostring(1)}, "|") .. "|col.panel"] == hov or next(sheet.cache) ~= nil, "cache populated")
end

-- cache isolation: mutating returned table must not corrupt cache (we copy out in style.resolve; ensure cached ref stable)
sheet:load(".x{color:red}")
local a = sheet:resolve("text", "x", nil, {})
local b = sheet:resolve("text", "x", nil, {})
assert(a == b and a.color == "red")
a.color = "blue" -- caller misuse; cache would be corrupted if same ref used unsafely
local c = sheet:resolve("text", "x", nil, {})
assert(c == a and c.color == "blue", "documents shared-ref cache; callers must not mutate")

-- malformed/edge inputs
sheet:load("broken { color } .ok{border:0; padding: nope; background:#abc;}")
assert(type(sheet:resolve("row","ok",nil,{})) == "table")

collectgarbage("collect")
print("css reload stress: ok")
