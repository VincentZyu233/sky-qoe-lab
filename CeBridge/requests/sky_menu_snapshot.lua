loadNewSymbols()

local copySnapshot = getAddressSafe('SkyQoEMenu.SkyQoE_CopySnapshotJson')
if not copySnapshot then
  return '{"ok":false,"error":"SkyQoEMenu.dll is not loaded or symbols are stale"}'
end

local required = executeCodeEx(0, 5000, copySnapshot, 0, 0)
if not required or required <= 1 or required > 1024 * 1024 then
  return '{"ok":false,"error":"invalid snapshot size"}'
end

local buffer = allocateMemory(required)
if not buffer then
  return '{"ok":false,"error":"snapshot buffer allocation failed"}'
end

local written = executeCodeEx(0, 5000, copySnapshot, buffer, required)
local json
if written == required then
  json = readString(buffer, required - 1, false)
else
  json = '{"ok":false,"error":"snapshot copy failed"}'
end
deAlloc(buffer)
return json
