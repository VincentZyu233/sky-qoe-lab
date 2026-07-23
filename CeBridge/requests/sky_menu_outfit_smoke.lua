reinitializeSymbolhandler(true)

local getEffective = getAddressSafe('SkyQoEMenu.SkyQoE_GetEffectiveOutfitId')
local getCount = getAddressSafe('SkyQoEMenu.SkyQoE_GetOutfitCatalogCount')
local findIndex = getAddressSafe('SkyQoEMenu.SkyQoE_FindOutfitIndex')
local queueIndex = getAddressSafe('SkyQoEMenu.SkyQoE_QueueOutfitByIndex')
if not getEffective or not getCount or not findIndex or not queueIndex then
  return '{"ok":false,"error":"outfit changer exports are unavailable"}'
end

local slot = 0
local originalId = executeCodeEx(0, 5000, getEffective, slot)
local count = executeCodeEx(0, 5000, getCount, slot)
local originalOneBased = executeCodeEx(0, 5000, findIndex, slot, originalId)
if not originalId or not count or count < 2 or not originalOneBased or
   originalOneBased < 1 or originalOneBased > count then
  return '{"ok":false,"error":"current body outfit is absent from the catalog"}'
end

local testIndex = originalOneBased % count
if executeCodeEx(0, 5000, queueIndex, slot, testIndex) ~= 1 then
  return '{"ok":false,"error":"could not queue the test outfit"}'
end

local changedId = originalId
for _ = 1, 100 do
  sleep(50)
  changedId = executeCodeEx(0, 5000, getEffective, slot)
  if changedId and changedId ~= originalId then break end
end
if not changedId or changedId == originalId then
  return '{"ok":false,"error":"test outfit was not applied within 5 seconds"}'
end

if executeCodeEx(0, 5000, queueIndex, slot, originalOneBased - 1) ~= 1 then
  return string.format(
    '{"ok":false,"error":"test outfit applied but restore could not be queued","changedId":%u}',
    changedId)
end

local restoredId = changedId
for _ = 1, 100 do
  sleep(50)
  restoredId = executeCodeEx(0, 5000, getEffective, slot)
  if restoredId == originalId then break end
end
if restoredId ~= originalId then
  return string.format(
    '{"ok":false,"error":"original outfit was not restored within 5 seconds","originalId":%u,"currentId":%u}',
    originalId, restoredId or 0)
end

return string.format(
  '{"ok":true,"slot":%d,"catalogCount":%d,"originalIndex":%d,"testIndex":%d,"originalId":%u,"changedId":%u,"restoredId":%u}',
  slot, count, originalOneBased - 1, testIndex, originalId, changedId, restoredId)
