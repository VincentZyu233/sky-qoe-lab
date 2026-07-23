local dllPath = [[D:\Downloads\temp\.build\SkyQoEMenu-v032\SkyQoEMenu.dll]]

local function findMenuModule()
  for _, module in ipairs(enumModules()) do
    if string.lower(module.Name or '') == 'skyqoemenu.dll' then
      return module
    end
  end
  return nil
end

local processId = getOpenedProcessID()
if not processId or processId == 0 then
  return '{"ok":false,"error":"Cheat Engine has no open process"}'
end

local previous = findMenuModule()
if previous then
  reinitializeSymbolhandler(true)
  local shutdown = getAddressSafe('SkyQoEMenu.SkyQoE_RequestShutdown')
  if not shutdown then
    return '{"ok":false,"error":"loaded menu has no shutdown export"}'
  end
  if executeCodeEx(0, 5000, shutdown, 0) ~= 1 then
    return '{"ok":false,"error":"menu shutdown request failed"}'
  end
  for _ = 1, 100 do
    sleep(50)
    if not findMenuModule() then break end
  end
  if findMenuModule() then
    return '{"ok":false,"error":"old menu did not unload within 5 seconds"}'
  end
end

if not injectDLL(dllPath) then
  return '{"ok":false,"error":"injectDLL failed"}'
end

local loaded = nil
for _ = 1, 100 do
  sleep(50)
  loaded = findMenuModule()
  if loaded then break end
end
if not loaded then
  return '{"ok":false,"error":"new menu did not appear within 5 seconds"}'
end

reinitializeSymbolhandler(true)
local getVersion = getAddressSafe('SkyQoEMenu.SkyQoE_GetVersion')
if not getVersion then
  return '{"ok":false,"error":"new menu version export is unavailable"}'
end
local version = executeCodeEx(0, 5000, getVersion, 0)
if version ~= 0x00030000 then
  return string.format('{"ok":false,"error":"unexpected menu version","version":%d}',
                       version or 0)
end

return string.format('{"ok":true,"pid":%d,"module":"0x%X","size":%d,"version":"0.3.0"}',
                     processId, loaded.Address or 0, loaded.Size or 0)
