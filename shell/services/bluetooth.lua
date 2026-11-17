-- Bluetooth System Integration
-- Fully asynchronous, non-blocking with cached snapshot and reactive events

local bt = {}

local cached_status = {
  powered = false,
  name = "Bluetooth",
  discovering = false,
  connected_count = 0,
  icon = "󰂲",
}
local cached_devices = {}
local subscribers = {}
local is_refreshing = false

local function notify_subscribers()
  for _, sub in ipairs(subscribers) do
    pcall(sub, cached_status, cached_devices)
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

function bt.refresh(callback)
  if is_refreshing then return end
  is_refreshing = true

  run_async("bluetoothctl", { "show" }, function(show_raw, show_code)
    local powered = show_raw:match("Powered:%s*([%a]+)") == "yes"
    local name = show_raw:match("Name:%s*([^\r\n]+)") or "Bluetooth"
    local discovering = show_raw:match("Discovering:%s*([%a]+)") == "yes"

    run_async("bluetoothctl", { "devices", "Connected" }, function(conn_raw, conn_code)
      local connected_map = {}
      local connected_count = 0
      for mac in conn_raw:gmatch("Device%s+([%w:]+)") do
        connected_map[mac] = true
        connected_count = connected_count + 1
      end

      cached_status.powered = powered
      cached_status.name = name
      cached_status.discovering = discovering
      cached_status.connected_count = connected_count
      cached_status.icon = not powered and "󰂲" or (connected_count > 0 and "󰂱" or "󰂯")

      if powered then
        run_async("bluetoothctl", { "devices" }, function(dev_raw, dev_code)
          is_refreshing = false
          local list = {}
          for line in dev_raw:gmatch("[^\r\n]+") do
            local mac, dev_name = line:match("Device%s+([%w:]+)%s+(.*)$")
            if mac then
              table.insert(list, {
                mac = mac,
                name = dev_name or mac,
                connected = connected_map[mac] == true,
              })
            end
          end
          table.sort(list, function(a, b)
            if a.connected ~= b.connected then return a.connected end
            return a.name < b.name
          end)
          cached_devices = list
          notify_subscribers()
          if callback then callback(cached_status, cached_devices) end
        end)
      else
        is_refreshing = false
        cached_devices = {}
        notify_subscribers()
        if callback then callback(cached_status, cached_devices) end
      end
    end)
  end)
end

function bt.get_status()
  return cached_status
end

function bt.get_devices()
  return cached_devices
end

function bt.toggle_power(state, callback)
  local cur = cached_status.powered
  local next_state = (state ~= nil) and state or not cur
  cached_status.powered = next_state
  cached_status.icon = not next_state and "󰂲" or "󰂯"
  notify_subscribers()

  run_async("bluetoothctl", { "power", next_state and "on" or "off" }, function()
    bt.refresh(callback)
  end)
  return next_state
end

function bt.connect(mac, callback)
  if not mac or mac == "" then
    if callback then callback(false) end
    return false
  end

  for _, d in ipairs(cached_devices) do
    if d.mac == mac then d.connecting = true break end
  end
  notify_subscribers()

  run_async("bluetoothctl", { "connect", mac }, function(out, code)
    local success = (code == 0) or (out:match("successful") ~= nil)
    bt.refresh(function()
      if callback then callback(success, out) end
    end)
  end)
  return true
end

function bt.disconnect(mac, callback)
  if not mac or mac == "" then
    if callback then callback(false) end
    return false
  end

  run_async("bluetoothctl", { "disconnect", mac }, function(out, code)
    local success = (code == 0) or (out:match("disconnect") ~= nil)
    bt.refresh(function()
      if callback then callback(success, out) end
    end)
  end)
  return true
end

function bt.subscribe(fn)
  if type(fn) ~= "function" then return function() end end
  table.insert(subscribers, fn)
  return function()
    for i, f in ipairs(subscribers) do
      if f == fn then table.remove(subscribers, i); break end
    end
  end
end

bt.on_change = bt.subscribe

bt.refresh()

return bt
