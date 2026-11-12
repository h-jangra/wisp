-- Display & Brightness System Integration

local bright = {}

local function run_cmd(cmd)
  if not io or not io.popen then return "" end
  local ok, p = pcall(io.popen, cmd .. " 2>/dev/null")
  if not ok or not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

function bright.get_brightness()
  local raw = run_cmd("brightnessctl -m")
  local pct = raw:match(",(%d+)%%")
  if pct then
    return tonumber(pct) or 100
  end

  local cur_f = io.open("/sys/class/backlight/amdgpu_bl2/brightness", "r") or io.open("/sys/class/backlight/intel_backlight/brightness", "r")
  local max_f = io.open("/sys/class/backlight/amdgpu_bl2/max_brightness", "r") or io.open("/sys/class/backlight/intel_backlight/max_brightness", "r")
  if cur_f and max_f then
    local cur = tonumber(cur_f:read("*a")) or 0
    local max = tonumber(max_f:read("*a")) or 1
    cur_f:close()
    max_f:close()
    return math.floor((cur / max) * 100 + 0.5)
  end

  return 100
end

function bright.set_brightness(pct)
  pct = math.max(1, math.min(100, math.floor(pct or 0)))
  run_cmd(string.format("brightnessctl set %d%%", pct))
  return pct
end

function bright.get_icon(pct)
  pct = pct or bright.get_brightness()
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

function bright.get_monitor_info()
  local raw = run_cmd("hyprctl monitors -j")
  local name = raw:match('"name":%s*"([^"]+)"') or "Display"
  local make = raw:match('"make":%s*"([^"]+)"') or ""
  local model = raw:match('"model":%s*"([^"]+)"') or ""
  local width = raw:match('"width":%s*(%d+)') or "1920"
  local height = raw:match('"height":%s*(%d+)') or "1080"
  local hz = raw:match('"refreshRate":%s*([%d%.]+)') or "60"
  local scale = raw:match('"scale":%s*([%d%.]+)') or "1"

  return {
    name = name,
    description = (make ~= "" and model ~= "") and (make .. " " .. model) or name,
    resolution = width .. "x" .. height,
    refresh_rate = math.floor(tonumber(hz) or 60) .. "Hz",
    scale = scale .. "x",
  }
end

return bright
