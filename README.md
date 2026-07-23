# Sky QoE Mod Research

这是 Sky 非正式 QoE 模组的持续研究工作区，当前目标是让本机外部程序稳定读取当前玩家自身的完整穿搭信息喵。

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
- 所有 RVA 都属于当前 `0x6A582C8E` 构建，后续版本必须先校验 AOB 或重新定位喵。

## 项目目录

- `SkyOutfitReader/` 是只读外部穿搭读取器原型喵。
- `CeBridge/` 是供终端直接控制当前 Cheat Engine 实例的本机命名管道桥喵。
- `CeBridge/requests/` 固化了玩家管理器捕获、捕获状态查询与穿搭 JSON 读取逻辑喵。
- `SkyQoEMenu/` 是透明 D3D11 ImGui 内置菜单与只读玩家状态快照实现喵。
- `test/20260723/` 保存好友码、HAR、协议字符串、指针反查和崩溃转储结构检查脚本喵。
- `docs/plan/20260723.尝试复现一个测身高接口捏/20260723.好友码与远端穿搭协议初步还原.md` 记录好友码测身高接口的当前逆向结论喵。
- `Sky_dump.bin` 与 `Sky_full_dump.bin` 是当前版本的分析输入，不应被自动修改喵。
