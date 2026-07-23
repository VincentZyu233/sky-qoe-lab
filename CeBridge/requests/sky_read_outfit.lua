local slotNames = {
  'Body',
  'Wing',
  'Hair',
  'Mask',
  'Neck',
  'Feet',
  'Horn',
  'Face',
  'Prop',
  'Hat',
}

local function unsigned32(value)
  if value == nil then return nil end
  if value < 0 then return value + 0x100000000 end
  return value
end

local function jsonEscape(value)
  if value == nil then return 'null' end
  local text = tostring(value)
  return '"' .. text:gsub('[%z\1-\31\\"]', function(character)
    if character == '"' then return '\\"' end
    if character == '\\' then return '\\\\' end
    if character == '\b' then return '\\b' end
    if character == '\f' then return '\\f' end
    if character == '\n' then return '\\n' end
    if character == '\r' then return '\\r' end
    if character == '\t' then return '\\t' end
    return string.format('\\u%04X', string.byte(character))
  end) .. '"'
end

local function readMsvcString(address)
  local length = readQword(address + 0x10)
  local capacity = readQword(address + 0x18)
  if not length or not capacity or length > 4096 or capacity < length then return nil end

  local dataAddress = address
  if capacity >= 16 then
    dataAddress = readQword(address)
  end

  if not dataAddress or dataAddress == 0 then return nil end
  return readString(dataAddress, length, false)
end

local function resolveResourceName(database, slot, id)
  if not id or id == 0 then return nil end

  local mask = unsigned32(readInteger(database + 0x50A8))
  local sentinel = readQword(database + 0x5080)
  local buckets = readQword(database + 0x5090)
  if not mask or not sentinel or not buckets then return nil end

  local hash = id & 0xFFFFFFFF
  hash = ((hash ~ (hash >> 16)) * 0x85EBCA6B) & 0xFFFFFFFF
  hash = ((hash ~ (hash >> 13)) * 0xC2B2AE35) & 0xFFFFFFFF
  hash = (hash ~ (hash >> 16)) & 0xFFFFFFFF

  local bucketAddress = buckets + (hash & mask) * 0x10
  local first = readQword(bucketAddress)
  local current = readQword(bucketAddress + 8)
  if not first or not current or current == sentinel then return nil end

  for _ = 1, 4096 do
    local nodeId = unsigned32(readInteger(current + 0x10))
    if nodeId == id then
      local recordIndex = unsigned32(readInteger(current + 0x14))
      if not recordIndex or recordIndex > 0x100000 then return nil end

      local record = database + 0x50C0 + recordIndex * 0xE50
      local recordId = unsigned32(readInteger(record))
      local recordSlot = unsigned32(readInteger(record + 8))
      if recordId ~= id or recordSlot ~= slot then return nil end
      return readMsvcString(record + 0x10)
    end

    if current == first then return nil end
    current = readQword(current + 8)
    if not current or current == 0 then return nil end
  end

  return nil
end

local manager = CodexSkyCapture and CodexSkyCapture.manager
if not manager then
  return '{"ok":false,"error":"manager capture is pending"}'
end

local avatar
local avatarIndex
for index = 0, 59 do
  local candidate = manager + 0x30 + index * 0x10B20
  local active = readBytes(candidate + 0xB850, 1, false)
  local flags = readSmallInteger(candidate + 0x109EC)
  if active and active ~= 0 and flags and (flags & 0x08) ~= 0 then
    local outfit = readQword(candidate + 0x58)
    if outfit and outfit ~= 0 and readQword(outfit + 8) == candidate then
      avatar = candidate
      avatarIndex = index
      break
    end
  end
end

if not avatar then
  return '{"ok":false,"error":"no validated local avatar in captured manager"}'
end

local outfit = readQword(avatar + 0x58)
local database = readQword(outfit + 0x10)
if not database or database == 0 then
  return '{"ok":false,"error":"outfit database pointer is null"}'
end

local slots = {}
for slot = 0, 9 do
  local baseId = unsigned32(readInteger(outfit + 0x54 + slot * 4)) or 0
  local overrideId = unsigned32(readInteger(outfit + 0x1D48 + slot * 4)) or 0
  local overrideFlag = unsigned32(readInteger(outfit + 0x1D7C + slot * 4)) or 0
  local effectiveId = overrideFlag ~= 0 and overrideId or baseId
  local resourceName = resolveResourceName(database, slot, effectiveId)

  slots[#slots + 1] = '{"index":' .. slot
    .. ',"type":' .. jsonEscape(slotNames[slot + 1])
    .. ',"baseId":' .. baseId
    .. ',"overrideId":' .. overrideId
    .. ',"overrideFlag":' .. overrideFlag
    .. ',"effectiveId":' .. effectiveId
    .. ',"resourceName":' .. jsonEscape(resourceName) .. '}'
end

return '{"ok":true,"manager":' .. jsonEscape(string.format('0x%X', manager))
  .. ',"avatarIndex":' .. avatarIndex
  .. ',"avatar":' .. jsonEscape(string.format('0x%X', avatar))
  .. ',"outfit":' .. jsonEscape(string.format('0x%X', outfit))
  .. ',"database":' .. jsonEscape(string.format('0x%X', database))
  .. ',"slots":[' .. table.concat(slots, ',') .. ']}'
