# Codex CE Bridge

该桥接通过本机命名管道 `\\.\pipe\codex_ce_bridge_v1` 让工作区终端直接执行 Cheat Engine Lua 喵。

## 组成

- `autorun/codex_bridge.lua` 由 CE 启动时自动加载，并在后台线程监听管道喵。
- `client/` 是发送 Lua 并打印 JSON 响应的 .NET 9 命令行客户端喵。
- `requests/` 保存 Sky 玩家管理器捕获、状态查询和完整穿搭读取脚本喵。
- Lua 请求通过 `thread.synchronize` 在 CE 主线程执行，因此可以安全调用 GUI、扫描器、调试器和进程相关 API 喵。

## 协议

请求由 4 字节 little-endian 长度和 UTF-8 Lua 源码组成喵。

响应由 4 字节 little-endian 长度和 UTF-8 JSON 组成，JSON 包含 `ok`、`output`、`returns`、`error`、`processId` 与 `bridgeVersion` 喵。

该私人插件没有令牌或加密，也不监听 TCP 端口喵。

## 安装

将 `autorun/codex_bridge.lua` 复制为 `C:\Program Files\Cheat Engine\autorun\codex_bridge.lua` 后，CE 下次启动会自动加载喵。

## 客户端示例

```powershell
CeBridgeClient.exe ping
CeBridgeClient.exe exec "return getOpenedProcessID(), getAddress('Sky.exe')"
CeBridgeClient.exe file .\request.lua
CeBridgeClient.exe --timeout 60000 file .\long-scan.lua
```

客户端返回非零退出码表示连接失败、协议失败或 CE Lua 执行失败喵。

## Sky 请求脚本

```powershell
CeBridgeClient.exe file .\CeBridge\requests\sky_capture_manager.lua
CeBridgeClient.exe file .\CeBridge\requests\sky_capture_status.lua
CeBridgeClient.exe file .\CeBridge\requests\sky_read_outfit.lua
```

`sky_capture_manager.lua` 使用带专属回调的执行断点，每次回调都会显式调用 `debug_continueFromBreakpoint(co_run)`；找到有效本地 Avatar 后会按唯一 ID 自行删除，连续 256 次未匹配也会自动撤销以避免拖慢游戏喵。

`sky_read_outfit.lua` 返回一个 JSON 字符串，其中包含 Manager、Avatar、Outfit、数据库地址和全部 10 个槽位的 ID 与资源名喵。

## 已验证环境

- 当前桥已在 CE `7.7` 中通过命名管道连接测试喵。
- 已验证多次连接、`print` 捕获、多返回值、`nil`、运行时错误堆栈与语法错误回传喵。
- CE 7.7 的 `debug_setBreakpoint` 第二返回值是包含 `ID`、`DTID`、`PID` 的表，精确删除时必须把完整返回表传给 `debug_removeBreakpointByID`，而不是只传 `result.ID` 喵。
- `debug_removeBreakpoint(address)` 对同地址重复断点的列表刷新存在延迟，调试脚本应优先保存并使用唯一断点 ID 喵。
