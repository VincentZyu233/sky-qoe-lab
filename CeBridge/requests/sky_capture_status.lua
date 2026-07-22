if not CodexSkyCapture then
  return false, 'missing', nil, nil, nil
end

local manager = CodexSkyCapture.manager
return CodexSkyCapture.hit == true,
  manager and string.format('0x%X', manager) or 'pending',
  CodexSkyCapture.dl,
  CodexSkyCapture.armed,
  CodexSkyCapture.breakpointId,
  CodexSkyCapture.hits,
  CodexSkyCapture.lastManager and string.format('0x%X', CodexSkyCapture.lastManager) or nil,
  CodexSkyCapture.avatar and string.format('0x%X', CodexSkyCapture.avatar) or nil,
  CodexSkyCapture.error
