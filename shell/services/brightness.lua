-- Display & Brightness System Integration
-- Asynchronous brightness adjustments with cached monitor snapshot and reactive events

local bright = {}

local cached_brightness = 100
local cached_monitor = nil
local subscribers = {}

local function notify_subscribers()
  for _, sub in ipairs(subscribers) do
    pcall(sub, cached_brightness, cached_monitor)
  end
end

local function run_async(cmd, args, callback)
  if wisp and wisp.async and type(wisp.async.exec) == "function" then
    local ok, proc = pcall(wisp.async.exec, cmd, args, function(out, code)
      if callback then callback(out or "", code or 0) end
    end)
    if ok and proc then return true end
  end
  if callback then callback("", -1) end
  return false
end

function bright.get_brightness()
  local cur_f = io.open("/sys/class/backlight/amdgpu_bl2/brightness", "r") or io.open("/sys/class/backlight/intel_backlight/brightness", "r")
  local max_f = io.open("/sys/class/backlight/amdgpu_bl2/max_brightness", "r") or io.open("/sys/class/backlight/intel_backlight/max_brightness", "r")
  if cur_f and max_f then
    local cur = tonumber(cur_f:read("*a")) or 0
    local max = tonumber(max_f:read("*a")) or 1
    cur_f:close()
    max_f:close()
    cached_brightness = math.floor((cur / max) * 100 + 0.5)
    return cached_brightness
  end
  return cached_brightness
end

function bright.set_brightness(pct, callback)
  pct = math.max(1, math.min(100, math.floor(pct or 0)))
  cached_brightness = pct
  notify_subscribers()

  run_async("brightnessctl", { "set", string.format("%d%%", pct) }, function(out, code)
    if callback then callback(pct) end
  end)
  return pct
end

function bright.get_icon(pct)
  pct = pct or cached_brightness
  if pct < 25 then
    return "󰃝"
  elseif pct < 50 then
    return "󰃞"
  elseif pct < 75 then
    return "󰃟"
  else
    return "󰃠"
  end
end

function bright.get_monitor_info(callback)
  if cached_monitor and not callback then return cached_monitor end

  run_async("hyprctl", { "monitors", "-j" }, function(raw, code)
    local name = raw:match('"name":%s*"([^"]+)"') or "Display"
    local make = raw:match('"make":%s*"([^"]+)"') or ""
    local model = raw:match('"model":%s*"([^"]+)"') or ""
    local width = raw:match('"width":%s*(%d+)') or "1920"
    local height = raw:match('"height":%s*(%d+)') or "1080"
    local hz = raw:match('"refreshRate":%s*([%d%.]+)') or "60"
    local scale = raw:match('"scale":%s*([%d%.]+)') or "1"

    cached_monitor = {
      name = name,
      description = (make ~= "" and model ~= "") and (make .. " " .. model) or name,
      resolution = width .. "x" .. height,
      refresh_rate = math.floor(tonumber(hz) or 60) .. "Hz",
      scale = scale .. "x",
    }
    notify_subscribers()
    if callback then callback(cached_monitor) end
  end)

  return cached_monitor or {
    name = "Default Display",
    description = "Primary Monitor",
    resolution = "1920x1080",
    refresh_rate = "60Hz",
    scale = "1.0x",
  }
end

function bright.subscribe(fn)
  if type(fn) ~= "function" then return function() end end
  table.insert(subscribers, fn)
  return function()
    for i, f in ipairs(subscribers) do
      if f == fn then table.remove(subscribers, i); break end
    end
  end
end

bright.on_change = bright.subscribe

bright.get_brightness()
bright.get_monitor_info()

return bright
