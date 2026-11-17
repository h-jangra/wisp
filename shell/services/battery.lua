-- Battery & Power Profile Integration
-- Non-blocking, asynchronous profile control with cached system status

local bat = {}

local cached_profile = "balanced"
local subscribers = {}

local function notify_subscribers()
  for _, sub in ipairs(subscribers) do
    pcall(sub, bat.get_status(), cached_profile)
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

function bat.get_status()
  local info = nil
  if wisp and wisp.system and wisp.system.battery then
    local ok, res = pcall(wisp.system.battery)
    if ok and res then info = res end
  end

  if not info or not info.present then
    local cap_f = io.open("/sys/class/power_supply/BAT0/capacity", "r")
    local stat_f = io.open("/sys/class/power_supply/BAT0/status", "r")
    if cap_f and stat_f then
      local cap = tonumber(cap_f:read("*a")) or 100
      local stat = (stat_f:read("*a") or "Unknown"):gsub("%s+", "")
      cap_f:close()
      stat_f:close()
      info = {
        present = true,
        percentage = cap,
        status = stat,
        charging = (stat == "Charging" or stat == "Full"),
        power_watts = 0.0,
      }
    else
      info = {
        present = false,
        percentage = 100,
        status = "AC Powered",
        charging = true,
        power_watts = 0.0,
      }
    end
  end

  local pct = info.percentage or 100
  local chg = info.charging == true
  local icon = "󰁹"

  if chg then
    icon = "󰂄"
  elseif pct < 15 then
    icon = "󰁺"
  elseif pct < 35 then
    icon = "󰁼"
  elseif pct < 60 then
    icon = "󰁾"
  elseif pct < 85 then
    icon = "󰂀"
  else
    icon = "󰁹"
  end

  info.icon = icon
  return info
end

function bat.get_power_profile()
  return cached_profile
end

function bat.refresh_power_profile(callback)
  run_async("powerprofilesctl", { "get" }, function(out, code)
    if code == 0 and out ~= "" then
      local prof = out:gsub("%s+$", "")
      if prof ~= "" then cached_profile = prof end
    end
    notify_subscribers()
    if callback then callback(cached_profile) end
  end)
end

function bat.set_power_profile(profile, callback)
  if profile and profile ~= "" then
    cached_profile = profile
    notify_subscribers()
    run_async("powerprofilesctl", { "set", profile }, function()
      if callback then callback(profile) end
    end)
  end
  return profile
end

function bat.subscribe(fn)
  if type(fn) ~= "function" then return function() end end
  table.insert(subscribers, fn)
  return function()
    for i, f in ipairs(subscribers) do
      if f == fn then table.remove(subscribers, i); break end
    end
  end
end

bat.on_change = bat.subscribe

bat.refresh_power_profile()

return bat
