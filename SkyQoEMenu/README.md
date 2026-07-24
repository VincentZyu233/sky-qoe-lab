# Sky QoE Menu

`SkyQoEMenu.dll` v0.6.1 是注入 Sky 进程的本地 ImGui QoE 与调试菜单喵。

菜单使用独立透明 D3D11 窗口覆盖 Sky 的 Vulkan 窗口，不 Hook Vulkan swapchain；只有外部程序明确调用聊天发送端点时才进入受限发送队列喵。

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
- 本机 HTTP API 可读取玩家、房间成员、环境、TGCL 对象、服饰、实体与聊天，并异步排队聊天发送喵。
- Sky 失去前台焦点时 Overlay 仍保持显示，仅在游戏最小化或窗口不可见时隐藏喵。
- `Insert` 显示或隐藏菜单，`End` 安全停止 HTTP、移除 Hook 并卸载 DLL 喵。

两个自动功能默认关闭喵。

烛火默认间隔为 900 ms，特效默认间隔为 35 ms，特效池达到 2800/3000 时自动暂停并在低于 2400 后恢复喵。

## 外部接口

注入成功后异步 HTTP 服务监听 `127.0.0.1:27891`，只绑定 IPv4 loopback 喵。

```text
GET /health
GET /v1/state
GET /v1/player
GET /v1/world
GET /v1/environment
GET /v1/entities
GET /v1/room
GET /v1/objects?offset=0&limit=100&search=BstNode
GET /v1/outfits
GET /v1/chat/status
GET /v1/chat/messages?after=0&limit=50
POST /v1/chat/send
GET /v1/tasks/{id}
GET /v1/schema
```

聊天发送 Body 必须是 `{"message":"text"}`；成功排队返回 `202` 和任务 ID，随后用 `/v1/tasks/{id}` 查询状态喵。

HTTP 使用 4 个固定工作线程、16 个连接等待队列、1500 ms 超时、16 KiB Header 与 4 KiB Body 上限；错误请求返回结构化 JSON，不直接进入游戏线程喵。

聊天消息环固定为 256 项，发送队列固定为 8 项，两次发送至少间隔 1200 ms 且 10 秒最多 5 项喵。

任务 `succeeded` 只表示文本已复制进游戏聊天子系统且原生提交函数已经返回，不是服务器送达回执喵。

`SkyQoE_CopySnapshotJson` 导出与 `/v1/state` 使用同一套 JSON 序列化代码喵。

现有导出还包括 Manager/Avatar/Outfit 读取、服饰目录计数与换装队列、相对传送、菜单切换、安全卸载和两个自动功能的开关/间隔控制喵。

HTTP 除受限聊天发送外均为只读；传送、换装和两个自动功能仍只通过菜单或控制导出操作喵。

## 单文件加载器

`SkyQoELoader.exe` 是原生 Win64 常驻加载器，同一次构建产生的 `SkyQoEMenu.dll` 会以 `RCDATA` 资源编入 EXE 喵。

在其他电脑上只需复制并双击 `SkyQoELoader.exe`，不需要携带独立 DLL、Cheat Engine、.NET 运行时或编译工具链喵。

无参数启动会打开常驻窗口，每秒刷新 Sky 是否运行、PID、模块基址、PE 入口地址、构建兼容性和菜单模块状态喵。

“注入”只在菜单尚未加载时启用；“重载菜单”会先调用安全卸载导出，等待旧模块消失后再注入内嵌版本喵。

按钮操作在后台线程执行，完成后加载器窗口继续存在；关闭加载器窗口不会停止或卸载已经注入的模组菜单喵。

加载器只查找 `Sky.exe`，并在注入前校验目标为 AMD64、PE timestamp 为 `0x6A582C8E`、`SizeOfImage` 为 `0x2FB2000` 喵。

运行时会把内嵌 DLL 校验后写到 `%LOCALAPPDATA%\SkyQoELab\payloads\<版本-哈希>\SkyQoEMenu.dll`，再通过目标系统模块中的 `LoadLibraryW` 地址完成加载喵。

如果同名菜单已经加载，加载器会从远程 PE 导出表定位 `SkyQoE_RequestShutdown`，等待旧模块完整卸载后再注入内嵌版本，不会重复映射两个菜单喵。

```powershell
# 打开常驻图形界面
.\SkyQoELoader.exe

# 只验证 EXE 内嵌 payload，不访问游戏进程
.\SkyQoELoader.exe --check --quiet

# 输出进程、入口地址和菜单状态
.\SkyQoELoader.exe --status --quiet

# 命令行首次注入或安全重载
.\SkyQoELoader.exe --inject --quiet
.\SkyQoELoader.exe --reload --quiet

# 兼容旧版自动选择注入或重载的行为
.\SkyQoELoader.exe --auto --quiet
.\SkyQoELoader.exe --quiet
```

加载器默认使用当前用户权限；只有 Sky 本身以管理员身份运行时，加载器才需要用相同权限启动喵。

当前加载器没有 Authenticode 代码签名，新电脑上的 Windows SmartScreen 可能显示未知发布者；应先核对 Release 中的 `SHA256SUMS.txt` 再运行喵。

每次菜单功能更新后重新构建 `SkyQoELoader` 即可得到包含新 DLL 的单文件版本，CMake 依赖会先链接菜单、复制 payload、重编资源，再链接加载器喵。

仓库的 `build-release.yml` 会在 push 到 `main` 或 `master` 后使用 MSVC x64、静态 `/MT` 运行库和 `/utf-8` 源码编码完成同样的构建、自检和 `tools-latest` Release 更新喵。

## 构建

本机工具位于 `.tools/gcc`、`.tools/ninja` 和 `.tools/imgui`，MinHook 1.3.4 与 nlohmann/json 3.11.3 由 CMake FetchContent 固定版本获取喵。

```powershell
cmake -S .\SkyQoEMenu -B .\.build\SkyQoEMenu-v060 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=.\.tools\gcc\bin\gcc.exe `
  -DCMAKE_CXX_COMPILER=.\.tools\gcc\bin\g++.exe `
  -DCMAKE_MAKE_PROGRAM=.\.tools\ninja\ninja.exe `
  -DSKYQOE_IMGUI_DIR=.\.tools\imgui

cmake --build .\.build\SkyQoEMenu-v060 --parallel
```

已经注入的 DLL 会被 Sky 锁定，不能原地覆盖；继续开发时应改用新的 build 目录喵。

加载器注入的是用户缓存副本，因此不会锁住构建目录中的 `SkyQoEMenu.dll` 或 `SkyQoELoader.exe` 喵。

## 验证

```powershell
.\.build\SkyQoEMenu-v060\SkyQoEMenuApiTest.exe `
  .\.build\SkyQoEMenu-v060\SkyQoEMenu.dll

.\.build\SkyQoEMenu-v060\SkyLevelAssetInspect.exe RainForest `
  'G:\GGames\Steam\steamapps\common\Sky Children of the Light\data\assets\rain\Data\Levels\RainForest\Objects.level.bin'

.\.build\SkyQoEMenu-v060\SkyQoELoader.exe --check --quiet
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
