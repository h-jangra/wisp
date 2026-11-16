-- Non-blocking command cache for services.
-- Returns the last known output immediately; refreshes in the background
-- via wisp.async.exec when the cached value is older than ttl_ms. The first
-- call for a key warms the cache synchronously so callers never see an empty
-- result on first use. Falls back to a synchronous read when async execution
-- is unavailable (e.g. check mode).
local M = {}

local entries = {}

local function run_sync(cmd)
  if not io or not io.popen then return "" end
  local ok, p = pcall(io.popen, cmd .. " 2>/dev/null")
  if not ok or not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

function M.cached(key, cmd, ttl_ms)
  ttl_ms = ttl_ms or 5000
  local e = entries[key]
  local now = os.time() * 1000
  if not e then
    e = { value = run_sync(cmd), at = now, pending = false }
    entries[key] = e
    return e.value
  end
  if (now - e.at) < ttl_ms then
    return e.value
  end
  if not e.pending then
    e.pending = true
    local ok = pcall(function()
      wisp.async.exec("sh", { "-c", cmd }, function(out, code)
        if type(out) == "string" then e.value = out end
        e.at = os.time() * 1000
        e.pending = false
      end)
    end)
    if not ok then
      e.value = run_sync(cmd)
      e.at = os.time() * 1000
      e.pending = false
    end
  end
  return e.value
end

-- Force a synchronous refresh (used after mutating commands).
function M.refresh(key, cmd)
  local e = entries[key]
  local v = run_sync(cmd)
  if e then
    e.value = v
    e.at = os.time() * 1000
    e.pending = false
  else
    entries[key] = { value = v, at = os.time() * 1000, pending = false }
  end
  return v
end

return M