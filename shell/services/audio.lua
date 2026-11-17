-- Audio & Volume Integration (wisp.audio + wpctl/PipeWire)
-- Fully asynchronous, non-blocking, event-driven with cached state snapshots

local audio = {}

local cached_vol = 50
local cached_muted = false
local cached_sinks = {}
local subscribers = {}
local is_refreshing = false

local function notify_subscribers()
  for _, sub in ipairs(subscribers) do
    pcall(sub, { volume = cached_vol, muted = cached_muted, sinks = cached_sinks })
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

function audio.refresh(callback)
  if is_refreshing then return end
  is_refreshing = true

  run_async("wpctl", { "status" }, function(raw, code)
    is_refreshing = false
    if code == 0 and raw ~= "" then
      local sinks = {}
      local in_sinks = false
      for line in raw:gmatch("[^\r\n]+") do
        if line:match("Sinks:") then
          in_sinks = true
        elseif in_sinks and (line:match("Sources:") or line:match("Filters:") or line:match("Streams:")) then
          in_sinks = false
        elseif in_sinks then
          local is_default = line:match("%*") ~= nil
          local id, name, vol_str = line:match("(%d+)%.%s+([^%[]+)%s*%[vol:%s*([%d%.]+)")
          if id and name then
            local v = tonumber(vol_str) and math.floor(tonumber(vol_str) * 100 + 0.5) or 100
            table.insert(sinks, {
              id = tonumber(id),
              name = name:gsub("%s+$", ""),
              volume = v,
              is_default = is_default,
            })
            if is_default then
              cached_vol = v
            end
          end
        end
      end

      if #sinks > 0 then
        cached_sinks = sinks
      end
    end

    run_async("wpctl", { "get-volume", "@DEFAULT_AUDIO_SINK@" }, function(vol_raw, vol_code)
      if vol_code == 0 then
        local num = vol_raw:match("Volume:%s*([%d%.]+)")
        if num then cached_vol = math.floor(tonumber(num) * 100 + 0.5) end
        cached_muted = vol_raw:match("%[MUTED%]") ~= nil
      end
      notify_subscribers()
      if callback then callback() end
    end)
  end)
end

function audio.get_volume()
  if wisp and wisp.audio and wisp.audio.is_available() then
    local v = wisp.audio.volume()
    if type(v) == "number" then
      cached_vol = math.floor(v * 100 + 0.5)
      return cached_vol
    end
  end
  return cached_vol
end

function audio.set_volume(pct, callback)
  pct = math.max(0, math.min(150, math.floor(pct or 0)))
  cached_vol = pct
  if wisp and wisp.audio and wisp.audio.is_available() then
    wisp.audio.set_volume(pct / 100)
  end
  run_async("wpctl", { "set-volume", "@DEFAULT_AUDIO_SINK@", string.format("%.2f", pct / 100) }, function(out, code)
    notify_subscribers()
    if callback then callback(pct) end
  end)
  return pct
end

function audio.is_muted()
  if wisp and wisp.audio and wisp.audio.is_available() then
    local m = wisp.audio.muted()
    if type(m) == "boolean" then
      cached_muted = m
      return cached_muted
    end
  end
  return cached_muted
end

function audio.set_muted(muted, callback)
  cached_muted = (muted == true)
  if wisp and wisp.audio and wisp.audio.is_available() then
    wisp.audio.set_muted(cached_muted)
  end
  run_async("wpctl", { "set-mute", "@DEFAULT_AUDIO_SINK@", cached_muted and "1" or "0" }, function(out, code)
    notify_subscribers()
    if callback then callback(cached_muted) end
  end)
  return cached_muted
end

function audio.toggle_mute(callback)
  return audio.set_muted(not cached_muted, callback)
end

function audio.get_icon(vol, muted)
  vol = vol or cached_vol
  if muted == nil then muted = cached_muted end
  if muted or vol == 0 then
    return "󰖁"
  elseif vol < 33 then
    return "󰕿"
  elseif vol < 66 then
    return "󰖀"
  else
    return "󰕾"
  end
end

function audio.get_sinks()
  if #cached_sinks == 0 then
    table.insert(cached_sinks, { id = 1, name = "Default Output", volume = cached_vol, is_default = true })
    audio.refresh()
  end
  return cached_sinks
end

function audio.set_default_sink(id, callback)
  if id and id > 0 then
    for _, s in ipairs(cached_sinks) do
      s.is_default = (s.id == id)
    end
    notify_subscribers()
    run_async("wpctl", { "set-default", tostring(id) }, function(out, code)
      audio.refresh(callback)
    end)
  end
end

function audio.subscribe(fn)
  if type(fn) ~= "function" then return function() end end
  table.insert(subscribers, fn)
  return function()
    for i, f in ipairs(subscribers) do
      if f == fn then table.remove(subscribers, i); break end
    end
  end
end

audio.on_change = audio.subscribe

-- Warm initial audio snapshot non-blockingly
audio.refresh()

return audio
