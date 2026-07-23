# Sky QoE Mod Research

这是 Sky 非正式 QoE 模组的持续研究工作区，当前目标包括稳定读取本地玩家状态、提供内置调试菜单，并逐步识别场景实体与 QoE 功能入口喵。

## 当前环境

- 当前游戏进程名为 `Sky.exe`，安装路径为 `G:\GGames\Steam\steamapps\common\Sky Children of the Light\Sky.exe` 喵。
- 当前 `Sky.exe` 磁盘文件大小为 `43,579,608` 字节，最后修改时间为 `2026-07-18 00:54:55` 喵。
- 当前内存映像基址为 `0x140000000`，PE `SizeOfImage` 为 `0x2FB2000`，PE 时间戳为 `0x6A582C8E` 喵。
- `Sky_dump.bin` 是从模块基址导出的内存布局映像，大小为 `0x2FB0000`，仅缺 PE 声明映像末尾的 `0x2000` 喵。
- `Sky_full_dump.bin` 是带零洞的连续虚拟地址区间，文件起始 VA 为 `0xAFF61000`，其中 `Sky.exe` 位于文件偏移 `0x9009F000` 喵。
- 当前 Cheat Engine 版本为 `7.7.0.10621`，进程名为 `cheatengine-x86_64-SSE4-AVX2.exe` 喵。

## 已确认函数

以下地址均为当前游戏版本的 RVA，运行时地址需要加 `Sky.exe` 模块基址喵。

| RVA | 暂定名称 | 已确认行为 |
| --- | --- | --- |
| `0x1696910` | `GetLocalAvatar` | 从玩家管理器的固定槽位池中选择活动 Avatar 喵 |
| `0x15FBA00` | `GetOutfitResourceName` | 获取指定穿搭槽位的有效 ID，并将 ID 解析为资源名喵 |
| `0x15FA840` | `GetEffectiveOutfitId` | 在基础 ID、临时覆盖 ID 和回退穿搭之间选择有效 ID 喵 |
| `0xD0EEF0` | `FindOutfitRecord` | 通过 32 位 ID 在穿搭数据库哈希表中查找大小为 `0xE50` 的记录喵 |
| `0xD0EEB0` | `GetOutfitRecordName` | 返回穿搭记录中 MSVC `std::string` 保存的资源名喵 |
| `0x7868E0` | `PrintCurrOutfit` 回调 | 获取本地 Avatar 的 outfit，并查询槽位 `0,3,6,2,1`，但没有稳定日志输出喵 |
| `0x786FD0` | `ClearOutfit` 回调 | 获取本地 Avatar 后跳转到 outfit 清理函数喵 |

## 玩家与穿搭结构

玩家管理器包含最多约 60 个固定大小槽位喵。

| 对象 | 偏移 | 含义 |
| --- | --- | --- |
| Manager | `+0x30 + index*0x10B20` | 第 `index` 个 Avatar 地址喵 |
| Manager | `+0xB880 + index*0x10B20` | 第 `index` 个 Avatar 活动字节喵 |
| Avatar | `+0x18` | 玩家变换对象指针喵 |
| Avatar | `+0x58` | outfit 对象指针喵 |
| Avatar | `+0xB850` | 活动字节喵 |
| Avatar | `+0x109EC` | 状态标志，`GetLocalAvatar(..., true)` 只接受 `0x08` 位为 1 的 Avatar；`flags=0x1AC` 是有效候选喵 |
| Outfit | `+0x08` | Avatar 反向指针，此关系由 `GetEffectiveOutfitId` 明确使用喵 |
| Outfit | `+0x10` | 穿搭资源数据库指针喵 |
| Outfit | `+0x44` | 控制回退穿搭选择的状态字段喵 |
| Outfit | `+0x48` | 回退穿搭对象指针喵 |
| Outfit | `+0x54 + slot*4` | 10 个基础穿搭 ID 喵 |
| Outfit | `+0x1D48 + slot*4` | 10 个临时覆盖 ID 喵 |
| Outfit | `+0x1D7C + slot*4` | 10 个临时覆盖启用字段喵 |

玩家变换对象中，`+0x00` 与 `+0x10` 保存两份同步位置，`+0x50`、`+0x60`、`+0x70` 分别保存右、上、前三个方向基向量喵。

## 穿搭槽位枚举

枚举顺序来自运行时注册表 `OutfitSlotType` 喵。

| 槽位 | 名称 |
| --- | --- |
| `0` | `Body` 喵 |
| `1` | `Wing` 喵 |
| `2` | `Hair` 喵 |
| `3` | `Mask` 喵 |
| `4` | `Neck` 喵 |
| `5` | `Feet` 喵 |
| `6` | `Horn` 喵 |
| `7` | `Face` 喵 |
| `8` | `Prop` 喵 |
| `9` | `Hat` 喵 |

## 穿搭数据库布局

| 偏移 | 含义 |
| --- | --- |
| `+0x5080` | 哈希表哨兵节点指针喵 |
| `+0x5090` | 桶数组指针，每个桶占 `0x10` 字节喵 |
| `+0x50A8` | 桶掩码，使用 `hash & mask` 选桶喵 |
| `+0x50C0 + index*0xE50` | 穿搭记录数组喵 |
| Record `+0x00` | 32 位穿搭 ID 喵 |
| Record `+0x08` | 32 位槽位枚举喵 |
| Record `+0x10` | MSVC `std::string` 资源名喵 |

ID 哈希使用 MurmurHash3 finalizer 形式：依次执行 `x ^= x>>16`、乘 `0x85EBCA6B`、`x ^= x>>13`、乘 `0xC2B2AE35`、`x ^= x>>16` 喵。

## 当前验证状态

- 静态反汇编、槽位枚举和数据库算法已经完成喵。
- 菜单使用 `Avatar+0x58 == Outfit`、`Outfit+0x08 == Avatar`、active、`flags&8` 与数据库指针联合校验，并以每次 8 MiB 的预算分片扫描可读私有内存喵。
- CE Bridge 已安装到 `C:\Program Files\Cheat Engine\autorun\codex_bridge.lua`，并已连接当前 CE 7.7 实例喵。
- CE Bridge 已验证正常返回、`print` 捕获、Lua 错误堆栈、语法错误和连续多次连接喵。
- CE 7.7 的断点唯一句柄是 `debug_setBreakpoint` 返回的完整信息表，只有将该表交给 `debug_removeBreakpointByID` 才能精确删除喵。
- `2026-07-23 15:25` 的实机验证确认旧判定把 `flags&8` 方向写反，导致捕获器连续命中 256 次后以异常码 `0x4001000A` 退出；默认流程不再依赖调试断点喵。
- `SkyQoEMenu.dll` v0.1.1 提供完整快照 JSON 导出，CE Bridge 可用 `sky_menu_snapshot.lua` 读取 Avatar、Outfit、数据库、10 个槽位和坐标候选喵。
- v0.1.1 实机自动发现会额外要求 active=`1`、Outfit 不在 Avatar 内嵌范围、数据库不在 Outfit 内嵌范围、哈希表头可读且至少一个有效 ID 能解析到资源名，以排除偶然双向指针伪结构喵。
- v0.1.1 已连续三次稳定读取同一对象链，10/10 槽位均解析出与 `Body, Wing, Hair, Mask, Neck, Feet, Horn, Face, Prop, Hat` 对应的资源名，运行时地址只作当次验证样本而不能硬编码喵。
- v0.2.0 在玩家页加入统一距离输入与前、后、左、右、上、下六向精准位移控件；平面方向按角色朝向计算，上下固定使用世界 Y 轴喵。
- 精准位移会实时重读变换对象、校验方向基与目标坐标，同步写入两份位置；任一写入失败都会恢复原值喵。
- v0.2.0 已在实机完成前后、左右、上下各 5 厘米成对测试，六次调用全部成功，平面与垂直坐标变化正确且最终精确回到起点喵。
- 同名 DLL 热卸载重载后必须完整刷新 CE 符号处理器再调用导出；`sky_menu_snapshot.lua` 已内置该刷新，避免旧地址导致目标进程异常退出喵。
- 所有 RVA 都属于当前 `0x6A582C8E` 构建，后续版本必须先校验 AOB 或重新定位喵。

## CE Bridge 自主操作流程

CE Bridge 让工作区终端直接控制已经打开的 Cheat Engine，不再需要手工复制 Lua 到 CE Lua Engine 喵。

- CE 自启动脚本源码位于 `CeBridge/autorun/codex_bridge.lua`，安装副本位于 `C:\Program Files\Cheat Engine\autorun\codex_bridge.lua` 喵。
- Bridge 只监听本机命名管道 `\\.\pipe\codex_ce_bridge_v1`，不开放 TCP 端口；按私人本机插件的需求，不使用令牌或加密喵。
- `.NET 9` 客户端位于 `CeBridge/client/`，请求格式是 4 字节 little-endian 长度加 UTF-8 Lua，响应格式是同样长度前缀的 JSON 喵。
- Bridge 后台线程接收请求，再通过 `thread.synchronize` 在 CE 主线程执行 Lua，因此可调用 CE 的进程、符号、GUI 与扫描 API 喵。
- `CeBridge/requests/sky_menu_snapshot.lua` 会完整刷新 CE 符号处理器，调用 `SkyQoE_CopySnapshotJson`，并返回玩家、变换、穿搭和坐标候选 JSON 喵。
- Bridge 源码、客户端和请求脚本可以提交到远端仓库；`C:\Program Files` 下的安装副本、构建产物和本地捕获不进入仓库喵。

每次 Sky 重启后必须重新取得 PID 并执行 `openProcess(newPid)`，不能继续使用 CE 中显示的旧 PID 喵。

目标进程切换后先验证 `getAddress('Sky.exe') == 0x140000000`，再做一次可立即释放的 4 KiB `allocateMemory` 测试，确认句柄可写喵。

完成验证后才调用 `injectDLL` 注入 `.build/SkyQoEMenu/SkyQoEMenu.dll`，并用 Windows 模块列表、Overlay HWND、版本导出和快照 JSON 四项共同确认成功喵。

当前机器的 Scoop `.NET` 环境变量会错误指向不完整的 SDK，运行 Bridge 客户端时应显式使用系统 `.NET 9` 喵：

```powershell
$env:DOTNET_ROOT='C:\Program Files\dotnet'
& 'C:\Program Files\dotnet\dotnet.exe' `
  '.\CeBridge\client\bin\Release\net9.0-windows\CeBridgeClient.dll' ping
```

构建客户端时还需要清除错误的 `MSBuildSDKsPath`，再调用 `C:\Program Files\dotnet\dotnet.exe build` 喵。

默认工作流不使用调试断点，也不使用 CE 的完整模块 `DissectCode:dissect()` 或 `createRipRelativeScanner()`，因为前者曾导致游戏异常退出，后两者会长时间占用 Bridge 主线程喵。

## 雨林烛火状态捕获

`2026-07-23 16:22` 在雨林地图、附近烛火刻意保持未拾取时完成了状态捕获喵。

本地捕获目录是 `captures/20260723-162258-rainforest-wax/`，该目录已加入 `.gitignore`，因为完整内存可能包含会话状态，不应上传 GitHub 喵。

### 捕获内容

| 文件 | 内容 |
| --- | --- |
| `Sky-8224-full.dmp` | 完整用户态内存、内存区域、线程、句柄和卸载模块信息，文件大小 `3,375,341,880` 字节喵 |
| `capture-manifest.json` | 捕获上下文、时间、构建、玩家锚点、文件大小与哈希清单喵 |
| `menu-snapshot*.json` | 完整转储前后的菜单 JSON 快照喵 |
| `modules.json` | 当时的 131 个已加载模块喵 |
| `threads.json` | 当时的 75 个线程及状态喵 |
| `memory-regions.json` | 3835 个虚拟内存区域的基址、大小、保护、状态和类型喵 |
| `dump-summary.json` | 捕获段、扫描字节、关键词计数和未捕获小页摘要喵 |
| `world-string-index.json` | 动态私有内存中的 18058 条世界相关字符串及虚拟地址喵 |
| `world-elements-curated.json` | 精简后的 296 条场景、烛火运行时、行为与可收集物候选喵 |
| `anchors/` | Avatar、Transform、Outfit 与穿搭数据库头的定向原始内存喵 |

完整转储头是 `MDMP`，SHA-256 是 `33100F8D47F45D2ABD3761CE46233C1CE419F02105D6A4A33CAB3D328765DE99` 喵。

转储实际包含 `2394` 个内存段和 `3,374,788,608` 字节数据，其中动态私有内存扫描覆盖 `2,633,940,992` 字节喵。

199 个合计约 1.43 MiB 的已提交小页没有出现在 Memory64 数据流中，已记录在 `dump-summary.json`，不影响完整转储主体喵。

### 玩家锚点

以下地址只属于本次 PID `8224` 捕获，不能跨进程硬编码喵：

| 对象 | 捕获地址 |
| --- | ---: |
| Avatar | `0x1392CAC0` 喵 |
| Transform | `0x111E2770` 喵 |
| Outfit | `0x111EACC0` 喵 |
| Outfit database | `0x13055DA0` 喵 |

转储前位置为 `(32.6669, 105.6510, -74.04535)`，转储后为 `(32.69534, 105.6516, -74.03335)`，对象地址保持不变喵。

### 世界元素线索

私有内存字符串 `Levels/RainForest/Candles.level` 位于本次捕获地址 `0xAB92660`，可作为当前雨林烛火关卡的场景线索喵。

本次捕获还出现了以下高价值名称喵：

| 捕获地址 | 字符串 |
| ---: | --- |
| `0x5CDD80` | `Create WaxChunk` 喵 |
| `0x5CDF70` | `Pop wax chunk (remote) 1982950791` 喵 |
| `0x9C8C760` | `OnWaxPickup` 喵 |
| `0x9C8C8B0` | `WaxChunk` 喵 |
| `0x9CA7310` | `CreateWaxChunkRpc` 喵 |
| `0x9CA7450` | `WaxPickupBehavior` 喵 |
| `0x9CA7B90` | `ActivateWaxChunkRpc` 喵 |
| `0x9CA8CD0` | `WaxPickupSpawnerZone` 喵 |
| `0xBDA65B0` | `onWaxSpawnEvents` 喵 |
| `0xBD21100` | `SpawnWaxChunk` 喵 |
| `0xBF1CB50` | `autoCollectWax` 喵 |
| `0xBF4FF50` | `collectAllWaxMarker` 喵 |

这些地址是捕获中字符串实例的地址，不是已经确认的函数入口或实体对象地址喵。

后续应从这些字符串做静态 xref，并在转储中查找引用字符串或对应类型描述符的私有对象；再将候选对象坐标与玩家位置、拾取前后差分进行交叉验证喵。

### 捕获方法

第一步通过 CE Bridge 调用 `sky_menu_snapshot.lua` 固化玩家、Transform、Outfit 和穿搭数据库锚点，全程不使用断点喵。

系统 `comsvcs.dll, MiniDump` 入口虽然返回成功码但没有产生文件，因此不能把其退出码当作捕获成功依据喵。

随后新增 `SkyProcessDump`，直接调用 Windows `MiniDumpWriteDump`，并启用完整内存、完整内存信息、句柄、线程、进程线程数据、卸载模块和令牌信息喵。

```powershell
.\.build\SkyQoEMenu\SkyProcessDump.exe 8224 `
  '.\captures\20260723-162258-rainforest-wax\Sky-8224-full.dmp'
```

`07_index_world_state_dump.py` 使用 MemoryInfoList 选择 `MEM_COMMIT + MEM_PRIVATE` 区域，以 8 MiB 分块扫描 ASCII 与 UTF-16 关键词字符串喵。

`08_curate_world_state_index.py` 从通用索引筛选场景路径、WaxChunk、WaxPickup、生成、拾取与自动收集相关名称喵。

`09_extract_state_anchors.py` 根据菜单快照，从完整转储直接提取四块稳定锚点内存并生成 SHA-256 清单喵。

```powershell
uv run --with minidump python .\test\20260723\07_index_world_state_dump.py `
  .\captures\20260723-162258-rainforest-wax\Sky-8224-full.dmp `
  .\captures\20260723-162258-rainforest-wax
```

## 项目目录

- `SkyOutfitReader/` 是只读外部穿搭读取器原型喵。
- `CeBridge/` 是供终端直接控制当前 Cheat Engine 实例的本机命名管道桥喵。
- `CeBridge/requests/` 固化了玩家管理器捕获、捕获状态查询与穿搭 JSON 读取逻辑喵。
- `SkyQoEMenu/` 是透明 D3D11 ImGui 内置菜单与只读玩家状态快照实现喵。
- `test/20260723/` 保存好友码、HAR、协议字符串、指针反查和崩溃转储结构检查脚本喵。
- `docs/plan/20260723.尝试复现一个测身高接口捏/20260723.好友码与远端穿搭协议初步还原.md` 记录好友码测身高接口的当前逆向结论喵。
- `Sky_dump.bin` 与 `Sky_full_dump.bin` 是当前版本的分析输入，不应被自动修改喵。
