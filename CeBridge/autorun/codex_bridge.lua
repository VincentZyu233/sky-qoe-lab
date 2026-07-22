local PIPE_NAME = 'codex_ce_bridge_v1'
local MAX_REQUEST_SIZE = 4 * 1024 * 1024

if type(_G.CodexCEBridge) == 'table' and _G.CodexCEBridge.running then
  print('[Codex CE Bridge] already running on \\.\\pipe\\' .. PIPE_NAME)
  return
end

local function jsonEscape(value)
  local text = tostring(value or '')
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

local function safeToString(value)
  local ok, result = pcall(tostring, value)
  if ok then return result end
  return '<tostring failed: ' .. tostring(result) .. '>'
end

local function encodeResponse(ok, output, values, errorMessage)
  local encodedValues = {}
  for index = 1, #values do
    local value = values[index]
    encodedValues[#encodedValues + 1] = '{"type":' .. jsonEscape(value.type)
      .. ',"value":' .. jsonEscape(value.value) .. '}'
  end

  return '{"ok":' .. (ok and 'true' or 'false')
    .. ',"output":' .. jsonEscape(table.concat(output, '\n'))
    .. ',"returns":[' .. table.concat(encodedValues, ',') .. ']'
    .. ',"error":' .. (errorMessage and jsonEscape(errorMessage) or 'null')
    .. ',"processId":' .. tostring(getOpenedProcessID() or 0)
    .. ',"bridgeVersion":"1.0.0"}'
end

local function executeRequest(source)
  local output = {}
  local previousPrint = print

  _G.print = function(...)
    local values = table.pack(...)
    local parts = {}
    for index = 1, values.n do
      parts[index] = safeToString(values[index])
    end
    output[#output + 1] = table.concat(parts, '\t')
  end

  local chunk, loadError = load(source, '=(codex-ce-bridge)', 't', _G)
  if not chunk then
    _G.print = previousPrint
    return encodeResponse(false, output, {}, loadError)
  end

  local results = table.pack(xpcall(chunk, debug.traceback))
  _G.print = previousPrint

  if not results[1] then
    return encodeResponse(false, output, {}, results[2])
  end

  local values = {}
  for index = 2, results.n do
    values[#values + 1] = {
      type = type(results[index]),
      value = safeToString(results[index]),
    }
  end

  return encodeResponse(true, output, values, nil)
end

local bridge = {
  name = PIPE_NAME,
  version = '1.0.0',
  startedAt = os.time(),
  running = true,
}

_G.CodexCEBridge = bridge

function bridge.stop()
  bridge.running = false
  if bridge.thread then bridge.thread.terminate() end
  if bridge.server then bridge.server.destroy() end
end

bridge.thread = createThread(function(thread)
  thread.freeOnTerminate(false)
  thread.Name = 'Codex CE Bridge'

  local server = createPipe(PIPE_NAME, MAX_REQUEST_SIZE, MAX_REQUEST_SIZE, 2)
  bridge.server = server

  if not server or not server.Valid then
    bridge.error = 'createPipe failed'
    bridge.running = false
    return
  end

  while not thread.Terminated do
    local connection = server.acceptConnection(true)
    if connection then
      connection.Timeout = 0

      while not thread.Terminated and connection.Connected do
        local requestLength = connection.readDword()
        if not requestLength then break end

        local response
        if requestLength > MAX_REQUEST_SIZE then
          response = encodeResponse(false, {}, {}, 'request exceeds 4 MiB')
        else
          local source = connection.readString(requestLength)
          if source == nil then break end
          response = thread.synchronize(function()
            return executeRequest(source)
          end)
        end

        if not connection.writeDword(#response) then break end
        if not connection.writeString(response) then break end
      end

      connection.destroy()
    end
  end

  server.destroy()
  bridge.server = nil
  bridge.running = false
end)

print('[Codex CE Bridge] ready on \\.\\pipe\\' .. PIPE_NAME)
