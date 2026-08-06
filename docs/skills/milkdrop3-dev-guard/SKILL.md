---
name: milkdrop3-dev-guard
description: |
  Y2Kmeter 项目 milkdrop3 模块的 AI 开发防呆规范。用于指导对 source/ui/modules/Milkdrop3Module.*、
  Milkdrop3Api.*、Md3DebugLog.*、Y2KStandaloneApp.cpp（启动序列）以及第三方引擎
  third_party/milkdrop3/code/vis_milk2/pluginshell.h/cpp 的功能迭代与维护。
  包含 6 条架构铁律（数据流/坐标/z-order/线程/生命周期/启动序列）、引擎修改白名单、
  禁止事项清单（含加载器锁陷阱）、关键符号事实核对表、修改后 12 项自检、
  RelWithDebInfo 编译验证（日志驱动模式，含增量构建）与双静态检查脚本。
  触发场景：修改 milkdrop3 相关代码、排查渲染偏移、排查引擎音频接入、新增预设交互按钮、
  变更模块尺寸/预览、回滚 D3D9 窗口层级、修改 pluginshell 引擎侧字段、
  改动 Y2KStandaloneApp::initialise 启动序列、排查 setVisible 卡死或 LdrLockLoaderLock 死锁、
  处理控件栏 overlay 显隐 z-order 问题、修改 ControlBarOverlay 或 D3dChildWindow 的 WndProc 消息处理、
  为 milkdrop3 预设跳转弹窗新增 EDIT 输入框、在模块内绘制标题栏/关闭按钮相关自定义 UI、
  排查全软件（非模块）外框黑边或标题栏文字丢失、排查 JUCE 原生窗口 UWPUIViewSettings/DwmSetWindowAttribute。
  排除项：不覆盖其它可视化模块（如 projectM/Spectrogram3D 等）；不涉及 Y2Kmeter 之外的项目。
license: internal
compatibility: |
  Windows 10/11 x64；Visual Studio 2026 (MSVC 14.51)；CMake ≥ 3.22；
  CLion 或命令行 Git Bash / cmd；构建类型 RelWithDebInfo；生成器 NMake Makefiles。
metadata:
  author: y2kmeter-team
  version: "1.3.1"
  data-classification: internal
  audit-level: medium
  scope:
    project: Y2Kmeter
    module: milkdrop3
---

# MilkDrop3 Module Development Guard

Y2Kmeter 项目 `milkdrop3` 模块的 AI 开发防呆规范。将 `docs/MilkDrop3_Integration.md`（v6→v22 共 20+ 轮踩坑）蒸馏为可执行约束，避免重复导致的编译失败、初始化死锁、GPU 挂起、坐标偏移。

## 1. 触发条件

命中以下任一条件时**必须**加载并遵循本 Skill：

- 修改 `source/ui/modules/Milkdrop3Module.*` / `Milkdrop3Api.*` / `Md3DebugLog.*`
- 修改 `third_party/milkdrop3/code/vis_milk2/pluginshell.h` / `.cpp`
- 修改 `source/standalone/Y2KStandaloneApp.cpp::initialise` **启动序列**（含 DPI 感知、`addToDesktop`、`setVisible`、`pluginHolder` 与 `mainWindow` 的先后关系）
- 在 `ModuleWorkspace` 调整 `ModuleType::milkdrop3` 的注册、尺寸、预览
- 排查 milkdrop3 的画面偏移、音频不响应、预设不切换、初始化卡死、右键菜单冲突
- 在 `PluginEditor` / `Y2KStandaloneApp` 调整 `openGLContext.attachTo(...)` 挂载顺序
- 用户堆栈里出现 `LdrLockLoaderLock` / `<unknown> 0x00007ffcdffcd9f8` 之类的地址，或 `juce::Component::toFront` / `juce::ResizableWindow::visibilityChanged` 卡死
- 修改 `Milkdrop3Module::ControlBarOverlay` / `D3dChildWindow::WndProc` 的显隐/z-order/鼠标消息处理
- 为跳转预设弹窗新增/修改 `CreateWindowExW(L"EDIT", ...)` 子控件或 `EditSubclassProc`（键盘处理）
- 在 milkdrop3 模块内自定义绘制 `ModulePanel` 标题栏、关闭按钮或访问其成员
- 用户报告「整个软件外框有黑边」「标题栏变小圆圈」「窗口右侧边缺失」等**全软件**外观异常（大概率是误改 JUCE 原生窗口代码）
- 用户问题中出现关键词：`milkdrop3`、`pluginshell`、`m_bY2kExternalSpectrumValid`、`D3dChildWindow`、`m_szSongTitle`、`D3D9 popup`、`LdrLockLoaderLock`、`TimerThreadBoot`、`SetProcessDpiAwarenessContext`、`UWPUIViewSettings`、`DwmSetWindowAttribute`、`juce_Windowing_windows.cpp`、`ControlBarOverlay`、`closeButtonPressed`

## 2. 前置检查（动手前必核 12 事）

修改前**必须**并行 `grep_search` 核对以下事实，不要凭记忆：

| # | 事实 |
|---|---|
| A | `NUM_FREQUENCIES == 512`（`defines.h`，不是 `shell_defines.h`） |
| B | `m_szSongTitle` 是 `wchar_t[512]`，不是 256 |
| C | `m_bY2kExternalSpectrumValid` / `m_y2kExternalSpectrum[2][NUM_FREQUENCIES]` 是 `pluginshell.h` public 成员且 consume-once |
| D | `AnalyserHub::FrameSnapshot::spectrumMag` 大小 = `spectrumMagSize = 1024`；**不存在** `AudioSnapshot::kSpectrumSize` |
| E | Milkdrop3Api 已删除 `Initialize / CreateRenderWindow / SetPresetDir`，禁止复活 |
| F | 进程为 PMv2；JUCE→Win32 必须 `logicalToPhysical` |
| G | `Y2KStandaloneApp::initialise` 启动序列必须是：`SetProcessDpiAwarenessContext(PMv2)` → `pluginHolder` → 主题恢复 → `reloadPluginState` → `mainWindow`（**不 addToDesktop**）→ `createEditor` + `setContentNonOwned` → `restoreBounds` → **`addToDesktop() + setVisible(true)` 紧挨在函数尾部**。任何在两者之间穿插 `setVisible(false)` / `TimerThreadBoot` / 重型对象构造都属违规 |
| H | 用户堆栈中 `<unknown> 0x00007ffc*d9f8` 表示 `ntdll!LdrLockLoaderLock`——**必然**是启动序列违规导致的 DLL 加载器锁死锁，不是 milkdrop3 内部问题 |
| I | `ModulePanel::closeButtonPressed` / `closeButtonHovered` 是 **`private`**（不是 protected），Milkdrop3Module 作为子类**不可直接访问**；`titleText` / `getTitleBarBounds()` / `getCloseButtonBounds()` / `getContentBounds()` 是 `protected` 可访问 |
| J | Win32 `SetWindowPos(hwnd, hWndInsertAfter, ...)` 的 `hWndInsertAfter` 语义 = 把 `hwnd` 放到 `hWndInsertAfter` **之后（下方）**；要让 overlay 位于 D3D popup **上方**，必须传 `HWND_TOP`，**不能**传 `d3d_child_hwnd_` |
| K | Win32 单行 EDIT 控件中 Enter 键的处理**必须放在 `WM_KEYDOWN` 里检测 `VK_RETURN`**，不能依赖 `WM_CHAR`（是否收到取决于父窗口是否为 dialog、是否有 IsDialogMessage 循环，不可靠） |
| L | `third_party/JUCE/**` 尤其是 `modules/juce_gui_basics/native/juce_Windowing_windows.cpp`（`UWPUIViewSettings` / `DwmSetWindowAttribute` / 渲染器选择 / 窗口样式）是**全软件共享代码**，改动会影响**所有**顶层窗口的外框/标题栏/DPI 行为。milkdrop3 的任何修复都**不得**触碰此文件 |

**详细验证命令与出处** → [references/symbol-facts.md](references/symbol-facts.md)

## 3. 执行步骤

1. **完成前置检查**（§2）：并行 `grep_search` 核对全部 12 项事实。
2. **对照适用范围与白名单**（§4.1）：确认修改文件在白名单内，否则先向用户确认。
3. **遵循 6 条架构铁律**（§4.2）：详见 [references/architecture-rules.md](references/architecture-rules.md)。
4. **规避禁止事项**（§4.3）：详见 [references/forbidden-list.md](references/forbidden-list.md)。
5. **编辑代码**：遵循项目 C++17 编码规范。
6. **修改后自检**（§5）：逐项回答 12 个问题。
7. **编译验证**（§6）：先静态自检（`read_lints` + `py -3 ...check_init_sequence.py` + `py -3 ...check_forbidden_patterns.py`），再用 `_bg_build.bat` 或 `_bg_incremental_build.bat` 后台启动构建，最后轮询日志 `build\skill-verify\_build_log.txt` 直到出现 `[PASS]` / `[FAIL]`。完整四步闭环见 [compile-verify.md §8](references/compile-verify.md)。
8. **文档同步**：追加轮次记录到 `docs/MilkDrop3_Integration.md` 附录 D/E。
9. **不擅自升级版本号 / commit / push**：仅在用户明确确认稳定后再执行。

## 4. 核心约束（Rules）

### 4.1 适用范围与白名单

**Y2Kmeter 侧（自由修改）**：`source/ui/modules/Milkdrop3Module.*`、`Milkdrop3Api.*`、`Md3DebugLog.*`

**第三方引擎白名单（严格）**：只允许修改下列两点，其它引擎文件**必须先向用户确认**：

| 文件 | 允许的修改 |
|---|---|
| `vis_milk2/pluginshell.h` | 在 public 区新增/维护 `m_y2k*` 前缀的外部注入字段 |
| `vis_milk2/pluginshell.cpp::AnalyzeNewSound` | 在 `time_to_frequency_domain` 之后消费上述字段并 consume-once 复位 |

**联动点（改前先确认必要性）**：`ModuleWorkspace.*`、`PluginEditor.*`、`Y2KStandaloneApp.cpp`、`CMakeLists.txt`。

### 4.2 6 条架构铁律（简版）

1. **数据流**：`AnalyserHub → FrameSnapshot → AudioSnapshot → Injector → Milkdrop3Api::FeedPcm/FeedSpectrum → CPluginShell`。构造函数**必须** `hub_->addFrameListener(this)`。
2. **坐标**：进程 PMv2。JUCE 坐标 → Win32 API 前必须 `juce::Desktop::getInstance().getDisplays().logicalToPhysical(...)`。
3. **z-order**：D3D9 `WS_POPUP` 恒在 JUCE `Graphics` 之上。要**可见**的 JUCE 内容必须位于 popup 上方的独占区。
4. **线程**：`Milkdrop3Module` 构造函数轻量；重型堆分配延迟到 `startTimer(5)` 或 `MessageManager::callAsync`。`openGLContext.attachTo(*this)` 不得在 Editor 构造末尾同步执行。
5. **生命周期**：`~Milkdrop3Module` 严格 7 步：`RemovePreRenderInjector → stopTimer → removeFrameListener → d3d_window_.reset → api_.Destroy → release(Spectrum) → release(Oscilloscope)`。
6. **启动序列（Standalone）**：`Y2KStandaloneApp::initialise` 严格按 v2.3.4 序列：`SetProcessDpiAwarenessContext(PMv2)` → `pluginHolder` → 主题恢复 → `reloadPluginState` → **`mainWindow = std::make_unique<...>()`（不 addToDesktop）** → `createEditor + setContentNonOwned` → `restoreBounds` → 函数末尾 **`mainWindow->addToDesktop(); mainWindow->setVisible(true);` 紧挨在一起**。禁止在中间插 `setVisible(false)` 或 `TimerThreadBoot` 之类的 workaround。违反此铁律的直接后果是 `setVisible(true) → toFront → peer->toFront → SetForegroundWindow → LoadLibrary(uxtheme/dcomp)` 与 audio 线程持有的 `LdrLockLoaderLock` 竞争死锁（堆栈 `<unknown> 0x00007ffc*d9f8`）。

**完整细节与失败案例** → [references/architecture-rules.md](references/architecture-rules.md)

### 4.3 禁止事项（清单）

- 引擎白名单外修改（§4.1）
- 右键处理：`isRightButtonDown` / `WM_RBUTTONDOWN`
- 从 D3D9 popup 向 JUCE 父窗口 `PostMessage` / `SendMessage` 转发
- `enterModalState(true)` 与独立子组件浮层
- 残留调试日志：`MonitorFromPoint` / `GetDpiForMonitor` / `GetDpiForWindow` / `SetThreadDpiAwarenessContext` / `MD3_BUILD_TAG`
- 复活已删接口：`Api::Initialize` / `CreateRenderWindow` / `SetPresetDir` / `render_scale_` / `CycleRenderScale` / `hub_retained_`
- `ModuleWorkspace::getDefaultSizeForType` / `getHoverPreviewImage` 中 `factory(milkdrop3)` 或 `new Milkdrop3Module`
- 有符号→无符号隐式回绕的 PCM 字节转换
- 假设 `LoadRandomPreset` 一定随机（`m_bSequentialPresetOrder=true` 时会顺序切）
- **触碰 `third_party/JUCE/**` 尤其是 `juce_Windowing_windows.cpp`**（新增）：即使排查 milkdrop3 相关 DPI/边框问题，也不能改 `UWPUIViewSettings` / `DwmSetWindowAttribute` / `DWMWA_NCRENDERING_POLICY` / renderer 选择 / 窗口样式 / `getBorderThickness`；一旦触碰，全软件外框、标题栏文本、右侧边、Win11 DWM 行为都可能发生跨模块回归
- **直接访问 `ModulePanel::closeButtonPressed` / `closeButtonHovered`**（新增）：这两个成员在基类是 `private`，子类不可直接引用。若确需在 Milkdrop3Module 里画自定义标题栏，`titleText` / `getTitleBarBounds()` / `getCloseButtonBounds()` 可用，但按钮 hover/press 状态必须由子类自行维护或通过让基类完成绘制（即 `ModulePanel::paint(g)` 走默认路径）来规避
- **用 `d3d_child_hwnd_` 作为 `SetWindowPos(hwnd, hWndInsertAfter, ...)` 的第二参数**（新增）：Win32 语义是「把 `hwnd` 置于 `hWndInsertAfter` **之后（下方）**」；overlay 想置顶必须传 `HWND_TOP`（或 `HWND_TOPMOST`），传 D3D popup HWND 反而会把 overlay 塞到 popup 下方导致彻底不可见
- **在 EDIT 子控件里通过 `WM_CHAR` 处理 `VK_RETURN`**（新增）：单行 EDIT 中 Enter 键是否投递到 `WM_CHAR` 依赖父窗口是否为 dialog / 是否有 `IsDialogMessage` 循环，不可靠；`VK_RETURN` 与 `VK_ESCAPE` 必须统一放到 `WM_KEYDOWN` 处理
- 启动序列违规（详见 [init-sequence.md](references/init-sequence.md)）：
  - 把 `mainWindow` 创建 / `addToDesktop` **提前**到 `pluginHolder` 之前
  - 在 initialise 中做 `setVisible(false)` 然后到函数末尾再 `setVisible(true)`
  - 引入 `TimerThreadBoot` / `SharedResourcePointer<TimerThread>` 预热之类的 workaround
  - 在 initialise 中调用 `SetThreadDpiAwarenessContext`（应用级只用 `SetProcessDpiAwarenessContext`）
  - 在 `addToDesktop` 与 `setVisible(true)` 之间穿插任何非平凡代码

**每条禁止的根因与历史事故轮次** → [references/forbidden-list.md](references/forbidden-list.md)

## 5. 修改后自检（AI 必答 12 问）

任一失答/失守则视为修改不完整：

| # | 问题 | 通过标准 |
|---|---|---|
| A | 构造函数是否 `addFrameListener(this)`？析构是否 `removeFrameListener`？ | 两者都必须存在 |
| B | 析构顺序是否为铁律 5 的 7 步？ | 严格遵守，`removeFrameListener` 早于 `Destroy` |
| C | 是否新增了 `MonitorFromPoint` / `GetDpiForMonitor` / `GetDpiForWindow` / `SetThreadDpiAwarenessContext` / `MD3_BUILD_TAG`？ | 一律不允许 |
| D | 是否复活已删死接口？ | 一律不允许 |
| E | 是否新增右键处理？是否向 JUCE 父窗口转发消息？ | 一律不允许 |
| F | 是否在 `getDefaultSizeForType` / `getHoverPreviewImage` 中 `factory(milkdrop3)` / `new Milkdrop3Module`？ | 一律不允许 |
| G | 是否触碰 `Y2KStandaloneApp::initialise`？若是，序列是否严格为 `SetProcessDpiAwarenessContext → pluginHolder → mainWindow(不 addToDesktop) → createEditor → restoreBounds → addToDesktop+setVisible(true)`？ | 若违反 → 立即回滚 |
| H | 是否引入 `TimerThreadBoot` / `SetThreadDpiAwarenessContext` / 拆分 `setVisible(false)+setVisible(true)` 之类的 workaround？ | 一律不允许 |
| I | 是否触碰了 `third_party/JUCE/**` 尤其是 `juce_Windowing_windows.cpp`（`UWPUIViewSettings` / `DwmSetWindowAttribute` / 渲染器选择 / 窗口样式 / `getBorderThickness`）？ | 一律不允许；若已改必须 `git checkout -- third_party/JUCE` 回滚 |
| J | 是否在 Milkdrop3Module 里直接访问 `closeButtonPressed` / `closeButtonHovered`？ | 一律不允许，二者是 ModulePanel 的 `private` 成员 |
| K | 若新写了 `SetWindowPos(overlay, ??, ...)` 想置顶 overlay：`hWndInsertAfter` 是否是 `HWND_TOP`，而**不是**任何 D3D 相关 HWND？ | 必须 `HWND_TOP`；置底才用具体 HWND |
| L | 若新加了 EDIT 子控件的键盘处理：`VK_RETURN` 是否在 `WM_KEYDOWN` 中检测（不是 `WM_CHAR`）？ | 必须 `WM_KEYDOWN`，与 `VK_ESCAPE` 保持一致 |

**自动化检查**：
- `py -3 docs/skills/milkdrop3-dev-guard/scripts/check_init_sequence.py` — 扫描启动序列反模式（铁律 6）
- `py -3 docs/skills/milkdrop3-dev-guard/scripts/check_forbidden_patterns.py` — 扫描 milkdrop3 侧禁止事项（JUCE 触碰、右键、调试痕迹、私有成员访问、`hWndInsertAfter` 语义错用、`WM_CHAR + VK_RETURN`）

## 6. 编译验证

CLion 使用 CMake `RelWithDebInfo` + VS 工具链 + `NMake Makefiles`。构建脚本使用独立目录 `build/skill-verify` 避免污染 CLion 目录。

### 6.1 后台启动构建（不阻塞 AI 终端）

AI 的 `terminal` 工具有超时限制（完整编译 2~5 分钟远超限制），因此**必须**使用后台启动器——构建脚本在独立的最小化 cmd 窗口中运行，AI 终端立即恢复控制权：

```bash
# 完整构建（含 CMake configure；适用于首次构建或修改 .h / CMakeLists.txt）
cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_bg_build.bat"

# 增量构建（跳过 configure；仅适用于只改 .cpp 且 cache 已存在）
cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_bg_incremental_build.bat"
```

**禁止**直接调用 `build_skill_verify.bat` 或 `_incremental_build.bat`——它们会让 `terminal` 工具超时。

### 6.2 AI 轮询日志判定结果

后台构建启动后（终端看到 `[SKILL-BUILD] Background build launched.`），AI 需**反复轮询日志文件**等待结果。每次间隔 5~10 秒：

```bash
# 快速查看日志尾部（推荐，不会超时）
tail -10 build/skill-verify/_build_log.txt

# 或用 grep 搜索结论标记
grep -E "\[PASS\]|\[FAIL\]" build/skill-verify/_build_log.txt
```

轮询期间日志尾部进度示例：
```
[SKILL-BUILD] [1/3] Initializing VS 2026 toolchain...   → 继续等待（VS 初始化中）
[SKILL-BUILD] [3/3] Building with NMake...               → 等待 10 秒再查（编译中）
[SKILL-BUILD] [PASS] All targets built successfully.     → ✅ 成功
[SKILL-BUILD] [FAIL] Build FAILED ...                    → ❌ 失败，定位错误
```

若 poll 2~3 分钟后仍未出现 `[PASS]`/`[FAIL]`，说明后台构建进程可能异常退出。此时应检查日志文件大小是否持续增长（`wc -c build/skill-verify/_build_log.txt`），若停滞不动则重新启动后台构建。

### 6.3 何时跳过完整编译

- 仅改 `.md` 文档 → 跳过
- 仅改 `.cpp` 且 `read_lints` 通过 → 增量 build（`_bg_incremental_build.bat`）
- 涉及 `.h` / `pluginshell.h` / `CMakeLists.txt` → **必须**完整编译（`_bg_build.bat`）
- 仅改 `.cpp`（含 `pluginshell.cpp`）且 `read_lints` 通过 + CMake cache 存在 → 增量 build（`_bg_incremental_build.bat`）

**完整流程、后台原理与错误定位 →** [references/compile-verify.md](references/compile-verify.md)，尤其是 §8「AI 编译验证协作流程（四步闭环）」

## 7. 示例

### 7.1 正确示例：新增一路"节拍强度"外部注入

1. `AudioSnapshot` 追加 `float beat_strength; bool has_beat;`
2. `Milkdrop3Module::onFrame` 加锁写入
3. `pluginshell.h` public 区新增 `bool m_bY2kExternalBeatValid; float m_y2kExternalBeat;`（白名单允许，`m_y2k` 前缀）
4. `pluginshell.cpp::AnalyzeNewSound` 末尾 consume-once
5. `AddPreRenderInjector` 注册闭包
6. 运行 §6 编译验证

### 7.2 错误示例（应立即拒绝）

- ❌ 在 `Milkdrop3Module` 构造函数里 `new D3dChildWindow()` — 违反铁律 4
- ❌ `paintOverChildren` 里 `g.drawText` 画预设名 — 违反铁律 3；不可见
- ❌ `WndProc` 里 `PostMessage(GetParent(hwnd), WM_LBUTTONDOWN, ...)` — 违反 §4.3
- ❌ 改 `plugin.cpp` 的预设加载逻辑 — 白名单外，需先向用户确认
- ❌ 为了去除软件外框黑边而改 `juce_Windowing_windows.cpp` 中的 `UWPUIViewSettings` / `renderer` / `DwmSetWindowAttribute` — 违反 §4.3 JUCE 禁区；会引发全软件回归
- ❌ 在 `Milkdrop3Module::paint(g)` 里直接 `if (closeButtonPressed) ...` — 基类 private 成员，不可直接访问；尽量让 `ModulePanel::paint(g)` 自己走默认路径
- ❌ `SetWindowPos(overlay_hwnd_, d3d_child_hwnd_, ...)` 想把 overlay 置于 popup 之上 — Win32 语义相反，应使用 `HWND_TOP`
- ❌ 在 `EditSubclassProc` 的 `WM_CHAR` 分支里检测 `VK_RETURN` 并触发 `DoPresetJump()` — 不可靠，必须搬到 `WM_KEYDOWN`
- ❌ 在块作用域内混用变量（如把 `if (!goPressed) { HPEN oldPn = ... }` 里声明的 `oldPn` 在相邻的 `if (!cnPressed) { ... }` 内直接使用） — C++ 作用域禁止，必须重新声明

**更多常见需求 → 正确落地方式** → [references/architecture-rules.md](references/architecture-rules.md) 末尾章节

## 8. 错误处理

### 8.1 常见编译失败

| 错误消息 | 快速定位 |
|---|---|
| `LNK2001 g_use_C_locale` / `keyMappings` | 恢复 `Milkdrop3Api.cpp` 顶部全局符号 |
| `LNK2019 __asm` 未解析 | 恢复 `asm-nseel-x86-msvc.c` 的 x64 `DECL_STUB` |
| `C2065 'i' 未声明` | MSVC x64 严格模式，手工补 `int` |
| `C2059 缺少 ;` in `Milkdrop3Api.h` | 块注释含 `*/` 提前关闭 |
| `C2065 'closeButtonPressed'/'closeButtonHovered' 未声明的标识符`（在 Milkdrop3Module.cpp） | 基类 `ModulePanel` 的 private 成员，子类不可访问。删除自定义标题栏绘制，或让 `paint(g)` 调用 `ModulePanel::paint(g)` |
| `C2065 'oldPn'/'nullBr'/'oldBr2' 未声明的标识符`（在 `PaintJumpDialog` Cancel 按钮段） | copy-paste 从 Go 按钮得来；块作用域局内变量不能跨块使用，重新声明（或重命名） |
| `C2061 语法错误: 标识符 'hdc'/'FillRect'`（成员函数内大面积） | 方法体多/少一个 `}`，导致后续代码脱离类作用域。用 `read_file` 逐段校对括号匹配 |
| `C2440: '<function-style-cast>': 无法从'std::wstring'转换为'juce::String'` | `std::wstring` 不能隐式构造 `juce::String`，`juce::String` 接受 `const wchar_t*` 但不接受 `std::wstring`。用 `.c_str()` 获取原始指针：`juce::String(wstr.c_str())` 或 `juce::String(wstr.data())` |

完整表 → [references/compile-verify.md](references/compile-verify.md)

### 8.2 常见运行时故障

| 现象 | 根因 | 修复 |
|---|---|---|
| 画面在左上角，偏移随距离增大 | 违反坐标铁律 | `CreateHWNDOnly` / `Reposition` 加 `logicalToPhysical` |
| MilkDrop 音频响应为零 | 忘记 `addFrameListener` | 构造函数追加 |
| 添加模块瞬间卡死 | 主线程 vs GL 线程堆分配 | 用 `startTimer(5)` / `callAsync` 延迟 |
| 预设名 JUCE 侧看不见 | 违反 z-order 铁律 | 改用 `m_bShowPresetInfo` + `LaunchSongTitleAnim` |
| 控件栏 overlay 在与 D3D 交互后彻底不可见 / 难以恢复 | `SetWindowPos` 的 `hWndInsertAfter` 错传了 D3D popup HWND，把 overlay 塑到了 popup 之下 | 改为 `SetWindowPos(overlay, HWND_TOP, ...)`；若仍需要相对顺序，应以 owned popup 创建顺序为准 |
| 预设跳转弹窗输入编号后回车不生效（需重新聚焦才看到编号变了） | Enter 处理写在 `WM_CHAR`，单行 EDIT 中行为不可靠 | 将 `VK_RETURN` 处理搬到 `WM_KEYDOWN`，与 `VK_ESCAPE` 保持一致 |
| 全软件外框多了黑边 / 标题栏变小圆圈 / 右侧边消失 | 误改了 `juce_Windowing_windows.cpp`（UWPUIViewSettings / renderer / 窗口样式 / borderThickness 之一） | `git checkout -- third_party/JUCE/**` 回滚；milkdrop3 的 DPI 问题只能在 milkdrop3 内部解决 |
| 拖 UI 时偶发假死 / DPI 变化后布局错乱 | initialise 里用了 `SetThreadDpiAwarenessContext` 而非 `SetProcessDpiAwarenessContext` | 仅在进程级设一次 PMv2 |
| 软件启动即卡死，堆栈含 `Component::toFront` + `visibilityChanged` + `setVisible` + `initialise`，`<unknown> 0x00007ffc*d9f8` | 违反启动序列铁律（LdrLockLoaderLock） | 回滚 `Y2KStandaloneApp::initialise` 到 §4.2 铁律 6 描述的顺序；删除 `TimerThreadBoot` 与 `setVisible(false)` 分裂 |

### 8.3 与本 Skill 冲突时的处理

用户指令要求做 §4.3 禁止事项内的动作、或触及引擎白名单外文件时：
1. **必须**先向用户明示该操作在本 Skill 中的禁止条款；
2. 说明历史上此路径的失败次数与后果（引用 `docs/MilkDrop3_Integration.md` 附录 D/E 对应轮次）；
3. 等待用户明确确认后再动手，或提出替代方案。

## 9. 关联资源

- **详细规范**：[architecture-rules](references/architecture-rules.md) · [forbidden-list](references/forbidden-list.md) · [symbol-facts](references/symbol-facts.md) · [compile-verify](references/compile-verify.md) · [lessons](references/lessons.md)
- **编译脚本**：[scripts/build_skill_verify.bat](scripts/build_skill_verify.bat) · [scripts/_bg_build.bat](scripts/_bg_build.bat)（后台启动器）
- **主项目文档**：[docs/MilkDrop3_Integration.md](../../MilkDrop3_Integration.md)（历史踩坑详录）· [PROJECT_OVERVIEW.md](../../../PROJECT_OVERVIEW.md)

---

**最后更新**：2026-08-06（v1.3.1：compile-verify.md 新增 §9「已知限制与排查指南」记录 vcvars 句柄继承锁、%~dp0 跨环境、start /MIN 监控盲区；§4 与 SKILL.md §6.3 细分 pluginshell.h 与 .cpp 构建触发条件；§5.2 与 SKILL.md §8.1 补充 C2440 std::wstring→juce::String 错误案例；lessons.md 新增 v24 章节；§2 与 §6.2 消除 AI 不可执行的"检查任务栏"指引）
