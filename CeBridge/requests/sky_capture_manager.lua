local target = getAddress('Sky.exe+1696910')
local MAX_HITS = 256

if CodexSkyCapture and CodexSkyCapture.armed then
  return true, CodexSkyCapture.breakpointId, string.format('0x%X', target), 'already armed'
end

local capture = {
  target = target,
  armed = true,
  hit = false,
  hits = 0,
}

CodexSkyCapture = capture

local function continueExecution()
  pcall(debug_continueFromBreakpoint, co_run)
  return 1
end

local function findLocalAvatar(manager)
  for index = 0, 59 do
    local avatar = manager + 0x30 + index * 0x10B20
    local active = readBytes(avatar + 0xB850, 1, false)
    local flags = readSmallInteger(avatar + 0x109EC)

    if active and active ~= 0 and flags and (flags & 0x08) == 0 then
      local outfit = readQword(avatar + 0x58)
      if outfit and outfit ~= 0 and readQword(outfit + 8) == avatar then
        return avatar, outfit, index, flags
      end
    end
  end
end

local success, breakpointInfo = debug_setBreakpoint(target, function()
  if not capture.armed then return continueExecution() end

  capture.hits = capture.hits + 1
  capture.lastManager = RCX
  capture.lastDl = RDX % 256

  local avatar, outfit, avatarIndex, flags = findLocalAvatar(RCX)
  if not avatar then
    if capture.hits >= MAX_HITS then
      capture.armed = false
      capture.error = 'no matching local-avatar manager within ' .. MAX_HITS .. ' hits'
      if capture.breakpointInfo then
        pcall(debug_removeBreakpointByID, capture.breakpointInfo)
      else
        pcall(debug_removeBreakpoint, capture.target)
      end
    end
    return continueExecution()
  end

  capture.hit = true
  capture.manager = RCX
  capture.avatar = avatar
  capture.outfit = outfit
  capture.avatarIndex = avatarIndex
  capture.avatarFlags = flags
  capture.dl = capture.lastDl
  capture.armed = false
  capture.hitAt = os.time()

  if capture.breakpointInfo then
    pcall(debug_removeBreakpointByID, capture.breakpointInfo)
  else
    pcall(debug_removeBreakpoint, capture.target)
  end

  return continueExecution()
end)

capture.success = success
capture.breakpointInfo = breakpointInfo
capture.breakpointId = type(breakpointInfo) == 'table' and breakpointInfo.ID or breakpointInfo

if not success then
  capture.armed = false
  capture.error = tostring(breakpointInfo)
  return false, capture.error, string.format('0x%X', target), 'failed'
end

return success, capture.breakpointId, string.format('0x%X', target), 'armed'
