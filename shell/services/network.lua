-- NetworkManager Wi-Fi Integration for Wisp
-- Fully asynchronous, non-blocking, event-driven with cached snapshots

local net = {}

local cached_status = {
  enabled = true,
  connected = false,
  ssid = "Disconnected",
  signal = 0,
  icon = "󰤟",
  unavailable = false,
}

local cached_networks = {}
local scan_in_flight = false
local scan_waiters = {}
local subscribers = {}
local status_refreshing = false
local last_status_time = 0

local function compute_icon(connected, signal)
  if not connected then return "󰤟" end
  if signal < 25 then return "󰤟"
  elseif signal < 50 then return "󰤢"
  elseif signal < 75 then return "󰤥"
  else return "󰤨" end
end

local function notify_subscribers()
  for _, sub in ipairs(subscribers) do
    pcall(sub, cached_status)
  end
end

local function run_async(cmd, args, callback)
  if wisp and wisp.async and type(wisp.async.exec) == "function" then
    local ok, proc = pcall(wisp.async.exec, cmd, args, function(out, code)
      if callback then callback(out or "", code or 0) end
    end)
    if ok and proc then return true end
  end
  -- Fallback if async unavailable (e.g. check mode)
  if callback then callback("", -1) end
  return false
end

local function parse_status_output(raw)
  local connected = false
  local active_ssid = ""
  local active_signal = 0

  for line in raw:gmatch("[^\r\n]+") do
    local is_act, ssid, sig = line:match("^([^:]*):([^:]*):(.*)$")
    if is_act == "yes" and ssid and ssid ~= "" then
      connected = true
      active_ssid = ssid
      active_signal = tonumber(sig) or 0
      break
    end
  end

  local old_conn = cached_status.connected
  local old_ssid = cached_status.ssid
  local old_sig  = cached_status.signal

  cached_status.enabled = true
  cached_status.unavailable = false
  cached_status.connected = connected
  cached_status.ssid = connected and active_ssid or "Disconnected"
  cached_status.signal = active_signal
  cached_status.icon = compute_icon(connected, active_signal)

  if old_conn ~= cached_status.connected or old_ssid ~= cached_status.ssid or math.abs(old_sig - cached_status.signal) > 5 then
    notify_subscribers()
  end
end

function net.refresh_status(callback)
  if status_refreshing then return end
  status_refreshing = true

  run_async("nmcli", { "-t", "-f", "active,ssid,signal", "dev", "wifi", "list", "--rescan", "no" }, function(out, code)
    status_refreshing = false
    last_status_time = os.time()
    if code == 0 and out ~= "" then
      parse_status_output(out)
    elseif code ~= 0 then
      -- Check if wifi radio is disabled
      run_async("nmcli", { "radio", "wifi" }, function(rad_out, rad_code)
        if rad_code == 0 then
          local en = rad_out:match("enabled") ~= nil
          cached_status.enabled = en
          cached_status.connected = false
          cached_status.ssid = en and "Disconnected" or "Wi-Fi Disabled"
          cached_status.signal = 0
          cached_status.icon = "󰤭"
          cached_status.unavailable = false
        else
          cached_status.enabled = false
          cached_status.connected = false
          cached_status.ssid = "Network services unavailable"
          cached_status.signal = 0
          cached_status.icon = "󰤭"
          cached_status.unavailable = true
        end
        notify_subscribers()
        if callback then callback(cached_status) end
      end)
      return
    end
    if callback then callback(cached_status) end
  end)
end

function net.get_status()
  local now = os.time()
  if (now - last_status_time) > 5 and not status_refreshing then
    net.refresh_status()
  end
  return cached_status
end

function net.is_wifi_enabled()
  return cached_status.enabled
end

function net.toggle_wifi(state, callback)
  if state == nil then
    state = not cached_status.enabled
  end
  cached_status.enabled = state
  if not state then
    cached_status.connected = false
    cached_status.ssid = "Wi-Fi Disabled"
    cached_status.signal = 0
    cached_status.icon = "󰤭"
    notify_subscribers()
  end

  local arg = state and "on" or "off"
  run_async("nmcli", { "radio", "wifi", arg }, function(out, code)
    if state then
      net.refresh_status(function()
        if callback then callback(state) end
      end)
    else
      if callback then callback(state) end
    end
  end)
  return state
end

local function parse_scan_output(raw)
  local list = {}
  local seen = {}

  for line in raw:gmatch("[^\r\n]+") do
    local is_act, ssid, sig, sec = line:match("^([^:]*):([^:]*):([^:]*):(.*)$")
    if ssid and ssid ~= "" and not seen[ssid] then
      seen[ssid] = true
      table.insert(list, {
        ssid = ssid,
        signal = tonumber(sig) or 0,
        security = (sec and sec ~= "") and sec or "Open",
        connected = (is_act == "yes"),
      })
    end
  end

  table.sort(list, function(a, b)
    if a.connected ~= b.connected then return a.connected end
    return a.signal > b.signal
  end)

  return list
end

function net.scan_networks(callback, force_rescan)
  if type(callback) == "function" then
    table.insert(scan_waiters, callback)
  end

  if scan_in_flight then
    return cached_networks
  end

  scan_in_flight = true
  local args = { "-t", "-f", "active,ssid,signal,security", "dev", "wifi", "list" }
  if force_rescan == false then
    table.insert(args, "--rescan")
    table.insert(args, "no")
  end

  local launched = run_async("nmcli", args, function(out, code)
    scan_in_flight = false
    local waiters = scan_waiters
    scan_waiters = {}

    if code == 0 then
      cached_networks = parse_scan_output(out)
      -- Also update connected network in cached_status
      for _, item in ipairs(cached_networks) do
        if item.connected then
          cached_status.connected = true
          cached_status.ssid = item.ssid
          cached_status.signal = item.signal
          cached_status.icon = compute_icon(true, item.signal)
          break
        end
      end
    end

    for _, waiter in ipairs(waiters) do
      pcall(waiter, cached_networks, code == 0)
    end
  end)

  if not launched then
    scan_in_flight = false
    local waiters = scan_waiters
    scan_waiters = {}
    for _, waiter in ipairs(waiters) do
      pcall(waiter, cached_networks, false)
    end
  end

  return cached_networks
end

function net.connect(ssid, password, callback)
  if not ssid or ssid == "" then
    if callback then callback(false, "Invalid SSID") end
    return false
  end

  local args = { "dev", "wifi", "connect", ssid }
  if password and password ~= "" then
    table.insert(args, "password")
    table.insert(args, password)
  end

  run_async("nmcli", args, function(out, code)
    local success = (code == 0) or (out:match("successfully") ~= nil) or (out:match("activated") ~= nil)
    if success then
      cached_status.connected = true
      cached_status.ssid = ssid
      for _, item in ipairs(cached_networks) do
        item.connected = (item.ssid == ssid)
        if item.connected then
          cached_status.signal = item.signal
          cached_status.icon = compute_icon(true, item.signal)
        end
      end
      notify_subscribers()
    end
    if callback then callback(success, out) end
  end)

  return true
end

function net.subscribe(fn)
  if type(fn) ~= "function" then return function() end end
  table.insert(subscribers, fn)
  return function()
    for i, f in ipairs(subscribers) do
      if f == fn then
        table.remove(subscribers, i)
        break
      end
    end
  end
end

net.on_change = net.subscribe

-- Initial fast status warm (non-blocking)
net.refresh_status()

return net
