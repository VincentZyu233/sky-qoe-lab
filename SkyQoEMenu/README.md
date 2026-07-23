# Sky QoE Menu

`SkyQoEMenu.dll` v0.4.1 是注入 Sky 进程的本地 ImGui QoE 与调试菜单喵。

菜单使用独立透明 D3D11 窗口覆盖 Sky 的 Vulkan 窗口，不 Hook Vulkan swapchain，也不主动发送服务器请求喵。

## 功能

- 校验 Sky PE 时间戳 `0x6A582C8E` 与 `SizeOfImage=0x2FB2000` 后才启用写入或 Hook 功能喵。
- 无断点分片发现 Manager、Avatar、Transform、Outfit 和穿搭数据库喵。
- 显示 10 个穿搭槽位的基础 ID、覆盖 ID、生效 ID 和资源名喵。
- “更衣”页加载全部 1536 项服饰定义，十个部位均可用左右箭头或下拉列表即时更换喵。
- 更衣页可按 `internal_name` 做大小写不敏感的多关键词筛选，列表和左右箭头只遍历匹配项喵。
- 提供前、后、左、右、上、下六向精准位移与统一距离输入喵。
- 扫描 GameRoot、房间人数、附近 Transform 和当前关卡字符串喵。
- 完整解析 TGCL `Objects.level.bin`，显示对象、属性、Wax 生成器和房间上限喵。
- “循环传到烛火”按最近未访问 Wax 生成器坐标循环传送喵。
- “循环生成全特效”在 EmitterBarn 游戏线程逐个生成 104 个已验证本地定义喵。
- 左上常驻 HUD 汇总玩家、坐标、房间、关卡、实体和自动功能状态喵。
- Sky 失去前台焦点时 Overlay 仍保持显示，仅在游戏最小化或窗口不可见时隐藏喵。
- `Insert` 显示或隐藏菜单，`End` 安全停止 HTTP、移除 Hook 并卸载 DLL 喵。

两个自动功能默认关闭喵。

烛火默认间隔为 900 ms，特效默认间隔为 35 ms，特效池达到 2800/3000 时自动暂停并在低于 2400 后恢复喵。

## 外部接口

注入成功后只读 HTTP 服务监听 `127.0.0.1:27891` 喵。

```text
GET /health
GET /v1/state
GET /v1/player
GET /v1/world
GET /v1/outfits
GET /v1/schema
```

`SkyQoE_CopySnapshotJson` 导出与 `/v1/state` 使用同一套 JSON 序列化代码喵。

现有导出还包括 Manager/Avatar/Outfit 读取、服饰目录计数与换装队列、相对传送、菜单切换、安全卸载和两个自动功能的开关/间隔控制喵。

HTTP 只读，控制导出主要供 CE Bridge 自动化测试使用喵。

## 单文件加载器

`SkyQoELoader.exe` 是原生 Win64 一键加载器，同一次构建产生的 `SkyQoEMenu.dll` 会以 `RCDATA` 资源编入 EXE 喵。

在其他电脑上只需复制并双击 `SkyQoELoader.exe`，不需要携带独立 DLL、Cheat Engine、.NET 运行时或编译工具链喵。

加载器只查找 `Sky.exe`，并在注入前校验目标为 AMD64、PE timestamp 为 `0x6A582C8E`、`SizeOfImage` 为 `0x2FB2000` 喵。

运行时会把内嵌 DLL 校验后写到 `%LOCALAPPDATA%\SkyQoELab\payloads\<版本-哈希>\SkyQoEMenu.dll`，再通过目标系统模块中的 `LoadLibraryW` 地址完成加载喵。

如果同名菜单已经加载，加载器会从远程 PE 导出表定位 `SkyQoE_RequestShutdown`，等待旧模块完整卸载后再注入内嵌版本，不会重复映射两个菜单喵。

```powershell
# 双击时执行的默认行为：注入或安全重载并显示结果
.\SkyQoELoader.exe

# 只验证 EXE 内嵌 payload，不访问游戏进程
.\SkyQoELoader.exe --check --quiet

# 注入或重载，但不显示结果对话框
.\SkyQoELoader.exe --quiet
```

加载器默认使用当前用户权限；只有 Sky 本身以管理员身份运行时，加载器才需要用相同权限启动喵。

当前加载器没有 Authenticode 代码签名，新电脑上的 Windows SmartScreen 可能显示未知发布者；应先核对 Release 中的 `SHA256SUMS.txt` 再运行喵。

每次菜单功能更新后重新构建 `SkyQoELoader` 即可得到包含新 DLL 的单文件版本，CMake 依赖会先链接菜单、复制 payload、重编资源，再链接加载器喵。

仓库的 `build-release.yml` 会在 push 到 `main` 或 `master` 后使用 MSVC x64、静态 `/MT` 运行库和 `/utf-8` 源码编码完成同样的构建、自检和 `tools-latest` Release 更新喵。

## 构建

本机工具位于 `.tools/gcc`、`.tools/ninja` 和 `.tools/imgui`，MinHook 1.3.4 与 nlohmann/json 3.11.3 由 CMake FetchContent 固定版本获取喵。

```powershell
cmake -S .\SkyQoEMenu -B .\.build\SkyQoEMenu-v041 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=.\.tools\gcc\bin\gcc.exe `
  -DCMAKE_CXX_COMPILER=.\.tools\gcc\bin\g++.exe `
  -DCMAKE_MAKE_PROGRAM=.\.tools\ninja\ninja.exe `
  -DSKYQOE_IMGUI_DIR=.\.tools\imgui

cmake --build .\.build\SkyQoEMenu-v041 --parallel
```

已经注入的 DLL 会被 Sky 锁定，不能原地覆盖；继续开发时应改用新的 build 目录喵。

加载器注入的是用户缓存副本，因此不会锁住构建目录中的 `SkyQoEMenu.dll` 或 `SkyQoELoader.exe` 喵。

## 验证

```powershell
.\.build\SkyQoEMenu-v041\SkyQoEMenuApiTest.exe `
  .\.build\SkyQoEMenu-v041\SkyQoEMenu.dll

.\.build\SkyQoEMenu-v041\SkyLevelAssetInspect.exe RainForest `
  'G:\GGames\Steam\steamapps\common\Sky Children of the Light\data\assets\rain\Data\Levels\RainForest\Objects.level.bin'

.\.build\SkyQoEMenu-v041\SkyQoELoader.exe --check --quiet
```

Harness 用于验证菜单布局、HTTP 端点和正常关闭后的端口释放喵。

实机换装回归使用 `CeBridge/requests/sky_menu_outfit_smoke.lua`，脚本会切换一个 Body 定义、验证生效 ID 并恢复原服装喵。

## 已知边界

- 所有 RVA 只适用于时间戳 `0x6A582C8E` 的当前 Sky 构建喵。
- 当前关卡要等待约 27 秒的首轮私有内存共识扫描，不能采用首个历史 telemetry 字符串喵。
- 烛火循环使用 TGCL 静态生成器坐标，尚未按运行时拾取状态剔除已清空点喵。
- 特效目录来自 214 个静态调用点归纳出的 104 个已加载定义，不声称覆盖资源包中从未被当前可执行文件引用的全部素材喵。
- 更衣目录依赖当前 Sky 安装中的 `data/assets/initial/Data/Resources/OutfitDefs.json`，资源缺失或 JSON 结构异常时会禁用更衣控件而不直接写 ID 喵。
- 详细地址、格式、工具、实机结果和失败路径统一记录在根目录 `README.md` 喵。
