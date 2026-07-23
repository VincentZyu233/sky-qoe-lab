reinitializeSymbolhandler(true)

local setInterval = getAddressSafe('SkyQoEMenu.SkyQoE_SetLocalEffectInterval')
local setEnabled = getAddressSafe('SkyQoEMenu.SkyQoE_SetLocalEffectLoopEnabled')
if not setInterval or not setEnabled then
  return '{"ok":false,"error":"local effect control exports are unavailable"}'
end

local interval = executeCodeEx(0, 5000, setInterval, 35)
local enabled = executeCodeEx(0, 5000, setEnabled, 1)
if interval ~= 35 or enabled ~= 1 then
  executeCodeEx(0, 5000, setEnabled, 0)
  return '{"ok":false,"error":"local effect cycle could not be armed"}'
end

sleep(5500)
local disabled = executeCodeEx(0, 5000, setEnabled, 0)
return string.format('{"ok":true,"intervalMs":%d,"disabledResult":%d}', interval, disabled or -1)
