# Sky QoE Menu

`SkyQoEMenu.dll` v0.1.1 是注入 Sky 进程的本地 ImGui 模组菜单喵。

首版使用独立透明 D3D11 窗口覆盖在 Sky 的 Vulkan 窗口上，不 Hook 游戏 swapchain，也不发出任何服务器请求喵。

## 当前功能

- 功能注册与逐项启停菜单喵。
- 当前构建 PE 时间戳与映像大小校验喵。
- Manager、Avatar、Outfit 与穿搭数据库状态喵。
- 10 个穿搭槽位的基础 ID、生效 ID 与资源名喵。
- Avatar 内存中的动态三浮点坐标候选扫描喵。
- 可调原始偏移与连续 float 查看器喵。
- 无断点的分片 Avatar 自动发现，每次刷新最多扫描 8 MiB 可读私有内存喵。
- 自动发现联合校验 active、flags、对象分离、Outfit 双向指针、数据库哈希表和可解析资源名，避免把 Avatar 内嵌对象识别成 Outfit 喵。
- `SkyQoE_CopySnapshotJson` 导出完整玩家、穿搭槽位和坐标候选 JSON 喵。
- `Insert` 切换菜单，`End` 安全卸载 DLL 喵。

## 构建

工作区工具路径如下喵：

```text
.tools/gcc/bin/g++.exe
.tools/ninja/ninja.exe
.tools/imgui
```

配置与构建命令如下喵：

```powershell
$env:PATH="D:\Downloads\temp\.tools\gcc\bin;D:\Downloads\temp\.tools\ninja;$env:PATH"
cmake -S .\SkyQoEMenu -B .\.build\SkyQoEMenu -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build .\.build\SkyQoEMenu
```

## CE 热注入

DLL 可以通过 CE Bridge 调用 `injectDLL` 热注入，不需要重启 Sky 喵。

正常情况下菜单会自行发现本地 Avatar，不需要设置断点或手动提供 Manager 喵。

Bridge 可通过 `CeBridge/requests/sky_menu_snapshot.lua` 调用 `SkyQoE_CopySnapshotJson` 读取完整快照喵。

诊断时仍可调用 `SkyQoE_SetManager` 手动提供 Manager；`SkyQoE_GetAvatar`、`SkyQoE_GetOutfit`、`SkyQoE_GetOutfitDatabase` 与 `SkyQoE_GetEffectiveOutfitId` 可读取标量状态喵。

当前版本只支持 PE 时间戳 `0x6A582C8E`、`SizeOfImage=0x2FB2000` 的 Sky 构建喵。

离线 API 回归测试如下喵：

```powershell
.\.build\SkyQoEMenu\SkyQoEMenuApiTest.exe .\.build\SkyQoEMenu\SkyQoEMenu.dll
```
