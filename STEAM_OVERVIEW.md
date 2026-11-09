# Y2Kmeter Steam 版本维护说明

> **这份文档只讲 Steam 渠道独有的东西**（渠道隔离、成就、云存档、Steam 专用构建与打包）。
> 通用架构、模块系统、分析链路、UI 框架等请见 [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md)，本文不重复。
>
> 适用范围：`3.0.6` 起。AppID `4972120` / Depot `4972121`。
> 最后更新：2026-11-09（成就 16 → 7 收敛、Steam 渠道首次接入）。

---

## 0. 三分钟速览

### 0.1 三条流水线必须互相隔离

| 流水线 | 开关 | 构建目录 | 产物目录 | 含 Steam |
|---|---|---|---|---|
| 官网安装包（Inno Setup） | `Y2K_STEAM_BUILD=OFF`（默认） | `cmake-build-release-visual-studio/` | `dist/` | ❌ 零依赖 |
| 插件（VST3 / AU） | 同上 | 同上 | 同上 | ❌ |
| **Steam 版** | `Y2K_STEAM_BUILD=ON` | `build-steam/` 或 `build-steam-ninja/` | `dist-steam/` | ✅ 仅 Standalone |

### 0.2 五条不变量（破坏任何一条都会出事）

1. **默认构建不得出现任何 Steam 符号** —— 官网 exe 在没有 `steam_api64.dll` 的机器上必须能启动。
2. **`Y2K_STEAM_BUILD` 只能在独立构建目录里打开** —— 否则安装包会打进带 Steam 依赖的 exe。
3. **`Y2Kmeter_installer.iss` 与 `dist/` 一行都不要动** —— Steam 产物只写 `dist-steam/`。
4. **VST3 / AU 目标不得链接、编入任何 Steam 代码** —— 插件宿主里加载 Steam DLL 只会带来崩溃风险。
5. **所有 Steam 调用必须有降级路径** —— Steam 没启动 / 无许可时程序功能必须完全正常。

---

## 1. 文件地图

| 文件 | 职责 | 编进哪个 target |
|---|---|---|
| `source/steam/Y2KSteam.h` | 对外接口（`initialise / tick / unlock / isUnlocked / indicateProgress / restartRequested …`）+ 后端函数表声明。**不含任何 Steamworks 头** | 共享静态库 |
| `source/steam/Y2KSteam.cpp` | 调度层：只持有 `Backend*` 并转发；无后端时全部走空实现 | 共享静态库 |
| `source/steam/Y2KSteamSteamworks.cpp` | **全仓唯一** `#include <steam/steam_api.h>` 的地方；真实 Steamworks 调用；静态初始化阶段注册后端 | **只有 `Y2Kmeter_Standalone`**，且仅 `Y2K_STEAM_BUILD=ON` |
| `source/steam/Y2KAchievements.h/.cpp` | 成就触发逻辑 + 跨会话累计值持久化（7 条） | 共享静态库 |
| `cmake/steam_appid.txt` | 开发期用 AppID（内容 `4972120`），POST_BUILD 拷到构建产物目录 | 不进任何 target |
| `build_steam.ps1` | configure → build → 打包 → 自检 → 写 `manifest.json` | 工具脚本 |
| `CMakeLists.txt` 末尾 `if (Y2K_STEAM_BUILD)` 段 | SDK 校验、ASCII 暂存、宏、链接、DLL/appid 拷贝 | — |
| `CMakePresets.json` 的 `steam` / `steam-ninja` | 两套隔离构建树 | — |

> `docs/` 在 `.gitignore` 里（本地文档目录），**Steam 相关文档入库的只有本文件**。

---

## 2. 架构：为什么不是 `#if Y2K_STEAM` 双实现

需求书最初设想「`Y2KSteam.cpp` 里 `#if Y2K_STEAM` 双实现，把 `Y2K_STEAM` 定义在 `Y2Kmeter_Standalone` 上」。**这条路在 JUCE 8 下不成立**：

- `juce_add_plugin(Y2Kmeter …)` 创建的是 **静态库** `Y2Kmeter`（输出名 `Y2Kmeter_SharedCode`）；
  `PluginProcessor/PluginEditor/ModuleWorkspace/Y2KStandaloneApp …` 全部编进这个静态库；
- `Y2Kmeter_Standalone` 只是链接它的可执行封装，`target_compile_definitions(Y2Kmeter_Standalone …)`
  **传不到共享库里的源文件** → 埋点会静默走空实现，成就永远不解锁；
- 反过来把 `Y2K_STEAM` 加在共享库上，VST3 / AU 链接后同样会带上 `steam_api64` 依赖（违反不变量 4）。

所以采用 **运行期后端注册**：

```
Y2KSteamSteamworks.cpp（exe，静态初始化）
        │  detail::installBackend(&g_backend)
        ▼
y2k::steam  ←── 共享库里的 unlock()/tick()/…（无后端时是空实现）
        ▲
        └── 业务代码（PluginEditor / ModuleWorkspace / AudioRecorder …）
```

效果：

- 业务调用点**永远不写 `#if`**，只认 `y2k::steam` / `y2k::ach`；
- 默认构建 / 插件目标里后端从未注册 → 同一份代码走 no-op，**零 Steam 符号**；
- Steam 版 exe 里后端已注册 → 同一份代码走真实实现。

### 2.1 线程约定（硬性）

- `y2k::steam::*` 与 `y2k::ach::*` **只允许消息线程调用**（Timer 回调、按钮回调、UI 事件）。
- 音频线程 / 采集线程 / GL 线程里要触发，必须 `juce::MessageManager::callAsync` 抛回消息线程
  —— 参考 `AudioRecorder::closeTakeWriter()`：采集线程落盘后搭在已有的 `callAsync` 里解锁。

### 2.2 `y2k::ach` 的会话概念

- `attach(PropertiesFile&)` 只在 **Standalone 启动时**调用一次；`detach()` 在退出时调用。
- 插件宿主里从未 `attach()` → `isActive() == false` → 所有 `report*` 立即返回，
  **零磁盘写入、零 Steam 调用**。VST3 里点标题打开官网不会误解锁。

---

## 3. 构建

### 3.1 两个 preset

| configure | build | 生成器 | 构建目录 | 适用 |
|---|---|---|---|---|
| `steam` | `steam-release` | Visual Studio 18 2026 | `build-steam/` | 命令行 / CI |
| `steam-ninja` | `steam-ninja-release` | Ninja + `cl` | `build-steam-ninja/` | CLion（VS 工具链 + Ninja） |

两者都只构建 `Y2Kmeter_Standalone`，产物路径相同：

```
<build dir>\Y2Kmeter_artefacts\Release\Standalone\
├── Y2Kmeter.exe            ← Steam 版（含成就）
├── steam_api64.dll         ← POST_BUILD 自动拷贝
├── steam_appid.txt         ← 开发期调试用，**不发布**
├── glew32.dll
└── projectM-4.dll
```

### 3.2 命令行

```powershell
# 全流程（Visual Studio 生成器）
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_steam.ps1

# Ninja 树 / 只打包 / 指定 SDK
... -File .\build_steam.ps1 -Preset steam-ninja
... -File .\build_steam.ps1 -SkipBuild
... -File .\build_steam.ps1 -SteamSdk "D:\SDK\Steamworks"
```

脚本参数：`-Config`（默认 `Release`）、`-Preset`（默认 `steam`）、`-BuildDir`（默认按 preset 推导）、
`-SteamSdk`（默认 `I:/Steam相关/SteamSdk`）、`-SkipBuild`。

### 3.3 CLion

⚠️ **preset 派生出来的 profile，Toolchain 字段是只读的**。CLion 按 preset 的 `generator`
去匹配工具链，匹配不到就退回默认工具链（常见是 MSYS2 MinGW64），界面上改不了。
这是 CLion 的行为，不是工程配置错误。

两条可用路线：

1. **普通 CMake Profile（最稳）**：Toolchain 保持现有的「Visual Studio + Ninja」、Build type `Release`、
   Build directory `build-steam`、CMake options `-DY2K_STEAM_BUILD=ON`（`-DY2KM_STEAM_SDK=…` 可省）。
2. **用 `steam-ninja` 预设**：它显式写了 `generator: Ninja` 与 `CMAKE_C(XX)_COMPILER=cl`，
   正常能匹配到 VS 工具链；若仍被绑到 MinGW，回到路线 1。

> `steam` 与 `steam-ninja` **不能共用同一个已存在的构建目录**（CMake 会因生成器变更报错）。

### 3.4 SDK 路径处理

官方 SDK 常放在含中文的目录。CMake 在 configure 阶段把用到的内容复制到
`<build dir>/steam_sdk/`（纯 ASCII），再从那里 include / link：

```cmake
set(Y2KM_STEAM_SDK "I:/Steam相关/SteamSdk" CACHE PATH "…")
# → <build>/steam_sdk/public/steam/*.h  +  <build>/steam_sdk/lib/steam_api64.{lib,dll}
```

必需文件（缺一即 `FATAL_ERROR`）：`public/steam/steam_api.h`、`redistributable_bin/win64/steam_api64.lib`、
`…/steam_api64.dll`。

### 3.5 编译选项要点

| 项 | 值 | 原因 |
|---|---|---|
| `Y2K_STEAM=1` | 仅 `Y2Kmeter_Standalone` | 后端 TU 的 `#if Y2K_STEAM` |
| `/utf-8` | 仅 `Y2Kmeter_Standalone` | 后端文件含中文注释，避免 MSVC 按 ANSI 代码页误读 |
| `/wd4828` | 仅 `Y2Kmeter_Standalone` | 官方 SDK 头里有 Latin-1 注释字节，否则每次编译刷 3 条警告 |

---

## 4. 产物契约与上传

```
dist-steam\
├── manifest.json           ← 构建元数据（不进 depot）
└── content\                ← 唯一进 depot 的目录（上传时作为 ContentRoot）
    ├── Y2Kmeter.exe
    ├── steam_api64.dll
    ├── glew32.dll
    ├── projectM-4.dll
    └── assets\
        ├── VirtuPet\            （2652 个 png）
        ├── milkdrop_presets\    （9921 个 .milk + 1 个 .ini）
        └── milkdrop_textures\   （66 个 jpg）
```

`manifest.json` 字段：`product_version` / `steam_enabled` / `built_at`（带 `+0800`）/ `config` /
`cmake_preset` / `exe{name,bytes,sha256}` / `steam_api_dll{…}` / `file_count` / `total_bytes` / `notes`。

**打包脚本的自检（任一失败即非 0 退出）**：

1. `content\` 下不得有 `steam_appid.txt`、`*.pdb`、`*.ilk`、`*.exp`、`*.lib`、`*.log`、`*.tmp`；
2. exe 二进制里必须能搜到 ASCII 字符串 `steam_api64.dll`（防「忘了开 `Y2K_STEAM`」）；
3. `manifest.json` 里的 `sha256` 与实际文件一致；
4. 4 个运行时文件齐全（exe / steam_api64.dll / glew32.dll / projectM-4.dll）。

文件数参考：4 + 2652 + 9922 + 66 = **12644**。

---

## 5. 成就（7 条，2026-11-09 定稿）

| # | API Name | 英文 / 中文 | 触发点 |
|---|---|---|---|
| 1 | `ACH_FIRST_BOOT` | First Boot / 初次启动 | `Y2KStandaloneApp::initialise()`（每次启动都尝试，unlock 幂等） |
| 2 | `ACH_MARATHON` | Long Haul / 长跑选手 | `PluginEditor::timerCallback()` 1 Hz → `ach::sessionTick(dt)` 累计 10 h |
| 3 | `ACH_MILKDROP_DIVER` | Milkdrop Diver / 深潜 | `PluginEditor::timerCallback()` 每 3 拍（≈300 ms）采样当前预设路径，按文件名去重累计 10 个 |
| 4 | `ACH_PALETTE_SMITH` | Palette Smith / 调色师 | `ThemeLibrary::saveTheme()` 新增成功（覆盖同名不算） |
| 5 | `ACH_CAPTURE` | Tape It / 录下来 | `AudioRecorder::exportLastSeconds()`；以及 `closeTakeWriter()` 的 `callAsync`（手动停止 + 1 h 上限收尾） |
| 6 | `ACH_LOUDNESS_MASTER` | Loudness Master / 响度达标 | `AnalyserHub` 分发前统一判定（短期 −14 LUFS ±0.5 持续 2 s） |
| 7 | `ACH_ISAAC_DEPT_STORE` | Isaac Department Store / 艾萨克百货大楼 | 点击左上角标题文字 → `openWebsiteFromTitleClick()`（三处入口共用） |

### 5.1 已删除的 10 条（**不要加回来**）

`ACH_PIXEL_PERFECT`、`ACH_DUAL_VIEW`、`ACH_WALL_OF_SOUND`、`ACH_MAXIMIZE`、`ACH_THEME_COLLECTOR`、
`ACH_TOYBOX_OVERDRIVE`、`ACH_PRESET_IMPORT`、`ACH_BILINGUAL`、`ACH_FPS_HUNTER`、`ACH_DESKTOP_WIDGET`。

后台已删除这些定义，代码里再调 `SetAchievement` 只会静默返回 false（不崩、不报错），
但会留下死代码与误判，一律不要恢复。

### 5.2 累计值持久化（键名**绝对不能改**）

| 键 | 类型 | 用途 |
|---|---|---|
| `steam.runtimeSeconds` | int（秒） | #2 长跑选手进度 |
| `steam.presetCount` + `steam.preset0..15` | int / 文件名 | #3 深潜：已见过的预设（按文件名去重，上限 16 条） |

- 每 **5 分钟**落盘一次 + 退出时再落一次（`ach::sessionTick` 内），崩溃 / 强杀不丢进度。
- **不要**改成每次调用都 `saveIfNeeded()` —— Auto-Cloud 会频繁上传。
- 已废弃的 `steam.topmostSeconds` 会在 `Y2KStandaloneApp::sanitizeLegacySettings()` 里被清掉。
- 这些键会随 Auto-Cloud 同步到同账号的其它机器，属于预期行为。

### 5.3 新增 / 删除一条成就的标准流程

1. 后台创建并发布（拿到精确的 API Name）；
2. `source/steam/Y2KAchievements.h` 的 `ids` 加/删常量 + 接口声明；
3. `.cpp` 实现（累计型想好存哪个键、阈值多少）；
4. 在业务点调 `y2k::ach::report*`（**消息线程**；音频/GL 线程用 `callAsync`）；
5. `grep -r "ach::report"` 确认调用点数量与成就条数一致；
6. 更新本文件 §5 表格。

### 5.4 关于 Stat / 进度条

后台目前**没有**创建任何 Stat，所以代码**不调** `SetStat`，成就也没有进度条。
以后要加：在成就编辑器建 Stat → 在对应解锁点前补一次 `SetStat`（累计型取本地值与远端较大者），
触发逻辑本身不用改。

---

## 6. 云存档（Auto-Cloud）

### 6.1 结论：**不写任何同步代码**

后台 Auto-Cloud 已经在 Steam 客户端侧完成「启动前下载 / 退出后上传」，程序只需正常读写
`%APPDATA%\Y2Kmeter\Y2Kmeter.settings`。

| 后台项 | 值 |
|---|---|
| 配额 | 1 MB / 32 文件 |
| 根路径 | `WinAppDataRoaming` + `Y2Kmeter` + `*.settings`（跨平台，**非递归**） |
| 动态云同步 | 关闭（本程序不处理 `RemoteStorageLocalFileChange_t`） |
| 开发者专属模式 | **当前开启**（见 §6.3） |

推论：`telemetry.clientId` 存在 `Y2Kmeter.telemetry` 里、文件名不匹配 `*.settings`，
所以**天然不会跨机同步**（两台机器不会混成一个用户）。不要把它挪进 `.settings`。

### 6.2 代码侧唯一要做的事：`audioSetup` 设备存在性校验

跨机同步后，`audioSetup` 里的 `audioOutputDeviceName` 是**源机器**的具体设备名。实现见
`Y2KStandaloneApp::reconcileAudioSetupAfterRestore()`（在 `StandalonePluginHolder` 建好之后调用）：

- 设备已成功打开 → 与 settings 对账；
- 名字 / 设备类型对不上 → 把**实际**设备名、类型、通道掩码写回，静默（不弹窗）；
- 一个设备都没打开 → 保留原存档，不动。

对非云存档用户同样有收益：换声卡 / 拔 USB 声卡后不再需要手动重选。

### 6.3 ⚠️ 发售前必做

后台「仅为开发人员启用云支持」目前是**勾选**状态（因为已向外部测试者分发序列号，避免他们被自动同步）。
**发售前必须取消勾选并重新发布**，否则普通玩家永远用不上云存档。这一步只能在后台做，代码不涉及。

### 6.4 不要做的事

| 禁止 | 原因 |
|---|---|
| 调 `ISteamRemoteStorage::FileWrite` 写设置 | 会与 Auto-Cloud 双写冲突 |
| 把 `telemetry.clientId` 移进 `*.settings` | 两台机器会被统计成同一个用户 |
| 启用动态云同步 | 需要处理 `RemoteStorageLocalFileChange_t`，本程序未实现 |
| 在音频/实时回调里写 settings | 频繁写会造成反复上传 |
| 把 `milkdrop_presets/`、`milkdrop_textures/` 纳入同步 | 体积 / 数量太大 |

### 6.5 已知风险：多实例

`moreThanOneInstanceAllowed()` 仍返回 `true`，多开 + 云同步会出现「后写者胜」的配置覆盖。
建议 Steam 版改为单实例（一行改动），但这属于产品决策，尚未实施。

---

## 7. 开发期调试

### 7.1 `steam_appid.txt`

- 内容只有一行 `4972120`，源文件在 `cmake/steam_appid.txt`；
- POST_BUILD 复制到 `<build>/…/Standalone/steam_appid.txt`，作用是**脱离 Steam 客户端调试**时也能初始化；
- **绝对不能进 `dist-steam/content/`**（打包脚本会检查）；上传 depot 后客户端 AppID 判断会异常。

### 7.2 Steam 控制台

```bat
steam.exe -console

achievement_clear 4972120 ACH_ISAAC_DEPT_STORE   :: 清单条，重新测
reset_all_stats 4972120                          :: 清全部成就与统计

testappcloudpaths 4972120                        :: 观察 Auto-Cloud 路径
set_spew_level 4 4
testappcloudpaths 0
set_spew_level 0 0
```

### 7.3 诊断日志

在 exe 同目录放空文件 `Y2Kmeter_diag_on.txt`（或设环境变量 `Y2K_DIAG_LOG=1`），日志写到
`桌面\Y2Kmeter_Diag\Y2Kmeter_diag_*.log`。启动阶段会有一行：

```
steam backend=1 ready=1
```

| 读数 | 含义 |
|---|---|
| `backend=0` | 编的不是 Steam 版（没开 `Y2K_STEAM_BUILD`，或在跑官网/插件构建） |
| `backend=1 ready=0` | 是 Steam 版，但 `SteamAPI_Init()` 失败（Steam 未运行 / 无许可）→ 已静默降级 |
| `backend=1 ready=1` | 正常 |

### 7.4 建议的回归步骤（Steam 版 exe 直接双击即可，不必先上传 depot）

1. 首启 → First Boot 通知；
2. 点左上角标题文字 → 浏览器打开官网 + 艾萨克百货大楼通知（chrome 隐藏态也测一次）；
3. 保存一个自定义配色 → Palette Smith；导出一次录音 → Tape It；
4. 播 −14 LUFS 附近的素材 2 s → Loudness Master；
5. 切 10 个不同预设 → Milkdrop Diver（可用 `achievement_clear` 反复测）；
6. 重复触发同一成就 → 无重复弹窗；
7. 关掉 Steam 客户端再启动 exe → 不崩、功能正常、仅成就不解锁；
8. 把 settings 里 `audioOutputDeviceName` 改成不存在的设备 → 正常出声、无弹窗、退出后键值被改回真实设备。

---

## 8. 踩坑记录

| # | 现象 | 根因 / 结论 |
|---|---|---|
| 1 | `error C2039: "RequestCurrentStats": 不是 "ISteamUserStats" 的成员` | SDK **v1.58 起删除**该接口（v1.65 头里只剩注释），统计/成就由 `SteamAPI_Init()` 自动拉取。代码已不调用；换回 <1.58 的老 SDK 才需要加回 |
| 2 | `warning C4828`（`steamhttpenums.h` 等） | 官方头含 Latin-1 注释字节；已对 Standalone 加 `/wd4828` |
| 3 | CLion 里 `steam` 预设被绑到 MSYS2 MinGW64，且工具链改不了 | preset 派生的 profile 工具链只读，按 `generator` 匹配；用普通 Profile 或 `steam-ninja` 预设 |
| 4 | 两条 preset 混用同一个构建目录 → CMake 报生成器变更 | `steam` = `build-steam`，`steam-ninja` = `build-steam-ninja`，不可混用 |
| 5 | 从 `steamapps` 目录直接双击 exe，程序「闪一下就没了」 | `SteamAPI_RestartAppIfNecessary()` 返回 true，Steam 已拉起新实例；本进程必须立刻退出（`bootAborted = true` → 跳过状态快照）。这是预期行为 |
| 6 | 响度成就很难触发 | 早期挂在 `LoudnessModule::onFrame`，只有「响度」模块在屏上才走；已上移到 `AnalyserHub` 分发前统一判定，**各模块里不得再调**（会重复累计） |
| 7 | 多开 + 云同步导致主题被覆盖 | 尚未解决（见 §6.5） |
| 8 | 换机后无声 | `audioSetup` 里存的是源机器设备名；已加对账逻辑（§6.2） |

---

## 9. 维护检查清单

**改完成就 / 埋点后**

- [ ] `grep -r "ach::report"` 与 `ach::sessionTick` 的调用点数 = 成就条数（当前 7 + 心跳）
- [ ] 没有在音频线程 / GL 线程直接调 `y2k::ach::*`
- [ ] 没有重新引入 §5.1 里已删除的 API Name

**改完构建配置后**

- [ ] 默认构建（`Y2K_STEAM_BUILD=OFF`）产物目录里没有 `steam_api64.*`
- [ ] 默认构建 exe 的 `dumpbin /dependents` 不含 `steam_api64.dll`
- [ ] `Y2Kmeter_installer.iss` 无改动（`git diff` 为空）
- [ ] `dist-steam/content/` 里没有 `steam_appid.txt`

**发版前**

- [ ] 版本号（`CMakeLists.txt` 的 `project(Y2Kmeter VERSION …)`）已更新
- [ ] 后台「仅为开发人员启用云支持」已**取消**并发布
- [ ] 后台成就图标 / 名称 / 描述齐全，且与 `ids` 完全一致
- [ ] 商店页「Steam 成就」「云存档」复选框在功能可测后再勾

---

## 10. 相关文件索引

- 构建与打包：`CMakeLists.txt`（末尾 Steamworks 段）、`CMakePresets.json`、`build_steam.ps1`、`cmake/steam_appid.txt`
- Steam 封装：`source/steam/Y2KSteam.h`、`Y2KSteam.cpp`、`Y2KSteamSteamworks.cpp`
- 成就逻辑：`source/steam/Y2KAchievements.h`、`Y2KAchievements.cpp`
- 接入点：`source/standalone/Y2KStandaloneApp.cpp`（初始化 / 心跳 / 退出 / `audioSetup` 对账）、
  `PluginEditor.cpp`（标题点击、1 Hz 心跳、预设采样）、`source/analysis/AnalyserHub.cpp`（响度判定）、
  `source/analysis/AudioRecorder.cpp`、`source/ui/palette/ThemeLibrary.cpp`
- 通用架构：`PROJECT_OVERVIEW.md`
