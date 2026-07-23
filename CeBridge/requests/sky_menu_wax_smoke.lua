reinitializeSymbolhandler(true)

local setInterval = getAddressSafe('SkyQoEMenu.SkyQoE_SetWaxLoopInterval')
local setEnabled = getAddressSafe('SkyQoEMenu.SkyQoE_SetWaxLoopEnabled')
if not setInterval or not setEnabled then
  return '{"ok":false,"error":"wax loop control exports are unavailable"}'
end

local interval = executeCodeEx(0, 5000, setInterval, 10000)
local enabled = executeCodeEx(0, 5000, setEnabled, 1)
if interval ~= 10000 or enabled ~= 1 then
  executeCodeEx(0, 5000, setEnabled, 0)
  return '{"ok":false,"error":"wax smoke test could not be armed"}'
end

sleep(500)
local disabled = executeCodeEx(0, 5000, setEnabled, 0)
return string.format('{"ok":true,"intervalMs":%d,"disabledResult":%d}', interval, disabled or -1)
