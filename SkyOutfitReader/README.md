# Sky Outfit Reader

`SkyOutfitReader.exe` 是只读的 Windows x64 外部穿搭读取器原型喵。

它会查找当前运行的 `Sky.exe`，读取进程内存并输出包含 Manager、Avatar、Outfit、数据库地址、10 个穿搭槽位 ID 与资源名的格式化 JSON 喵。

## 使用

```powershell
.\SkyOutfitReader.exe
.\SkyOutfitReader.exe --diagnostic
```

默认模式只输出已确认的穿搭快照，`--diagnostic` 会附加有限的数据库候选诊断信息喵。

成功时退出码为 `0`，扫描或权限错误时退出码为 `1`，错误信息同样以 JSON 写到标准错误喵。

## 边界

- 读取器不向 Sky 写内存，不设置断点，也不发送网络请求喵。
- 读取器依赖当前游戏版本的结构特征，Sky 更新后可能需要同步更新扫描规则喵。
- 无法打开 Sky 进程时，应确认游戏已启动且当前用户具有读取该进程的权限喵。
- 当前自包含构建可在 64 位 Windows 上直接运行，不要求预装 .NET 运行时喵。

源码位于本目录的 `Program.cs`，最新预编译版本位于仓库的 `tools-20260723` Release 喵。
