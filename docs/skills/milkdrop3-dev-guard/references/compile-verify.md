# Compile Verify (编译验证详解)

> 本文档为 [SKILL.md](../SKILL.md) §6 与 §8.1 的详细版。

---

## 1. 环境要求

| 项 | 值 |
|---|---|
| 操作系统 | Windows 10 / 11 x64 |
| 编译器 | MSVC 14.51（VS 2026 Professional） |
| SDK | Windows Kits 10.0.26100.0 或 10.0.28000.0 |
| CMake | ≥ 3.22 |
| 构建类型 | RelWithDebInfo |
| 生成器 | NMake Makefiles（由 CMake 自动决定） |
| 目标架构 | x64 |
| 构建目录 | `build/skill-verify/`（**独立**于 CLion 的 `cmake-build-relwithdebinfo-visual-studio/`） |

---

## 2. 构建脚本参考（工具链层面）

> **⚠️ AI 工作流**：§2 描述的是底层构建脚本的行为。AI 在 milkdrop3 开发中**不应直接调用**这些脚本（会触发 `terminal` 工具超时），而应使用后台启动器 `_bg_build.bat` / `_bg_incremental_build.bat`。完整 AI 工作流见 [§8](#8-ai-编译验证协作流程四步闭环)。

脚本已从"静默构建"升级为"日志驱动"模式，专门适配 AI 终端的输出捕获机制：

- 每进入一个新阶段，脚本**立即**向控制台打印 `[SKILL-BUILD] [N/3] ...` 标记——AI 终端从第一秒就能看到进度，不会因"零输出"而触发超时；
- 所有详细的 CMake / NMake / MSVC 编译输出**写入日志文件** `build\skill-verify\_build_log.txt`，AI 事后读取该文件即可拿到完整构建记录；
- 脚本结束前向日志写入明确的 `[PASS]` 或 `[FAIL]` 一行，并传播退出码（0 = 成功，非 0 = 失败）。

### 2.1 完整构建（含 CMake configure）

```bash
# 从 I:/Y2KMeter/ Git Bash 执行（注意：必须用引号包裹反斜杠路径）
cmd //c "docs\skills\milkdrop3-dev-guard\scripts\build_skill_verify.bat"
```

行为：1) VS toolchain → 2) CMake configure（仅首次）→ 3) `cmake --build -j` 全量构建。

### 2.2 增量构建（跳过 configure，仅编译变更文件）

CMake cache 已存在、只改了 `.cpp` 文件时，用增量构建速度更快：

```bash
cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_incremental_build.bat"
```

行为：直接 `cmake --build -j` 触发 NMake 增量编译，不改动的 translation unit 不重编。

> **前提**：`build\skill-verify\CMakeCache.txt` 必须已存在（即至少跑过一次完整构建）。

### 2.3 解读结果

编译命令执行完毕后（哪怕 AI 终端超时），读取日志文件即可判断：

```bash
# 检查是否成功
tail -5 build/skill-verify/_build_log.txt
# 期望看到: [SKILL-BUILD] [PASS] All targets built successfully.

# 查看所有错误（若失败）
grep -in "error " build/skill-verify/_build_log.txt | head -30

# 查看链接错误（若失败）
grep -in "LNK" build/skill-verify/_build_log.txt | head -20
```

> **注意**：`cmake --build` 完整构建耗时数分钟（首次更长），AI 终端可能因超时提前返回。但日志文件在构建过程中持续写入，因此即使终端超时，日志仍然完整——AI 只需用 `read_file` 或 `grep` 读日志即可获得最终结果。

---

## 3. 期望输出（控制台）

AI 在终端中会立即看到以下进度标记（这才是关键——解决了"零输出超时"问题）：

```
[SKILL-BUILD] [1/3] Initializing VS 2026 toolchain...
[SKILL-BUILD] VS toolchain ready.
[SKILL-BUILD] [2/3] CMake configure...
[SKILL-BUILD] CMake configure done.    （或 "skip configure"）
[SKILL-BUILD] [3/3] Building with NMake (this may take several minutes)...
[SKILL-BUILD] Log: I:\Y2KMeter\build\skill-verify\_build_log.txt
[SKILL-BUILD] Build phase finished, exit code: 0
[SKILL-BUILD] [PASS] All targets built successfully.
```

详细的 CMake / NMake / MSVC 逐行输出（如 `[ 15%] Building CXX object ...`）写入日志文件，不在控制台刷屏。日志尾部应见：

```
[100%] Built target Y2Kmeter_Standalone
[100%] Built target Y2Kmeter_VST3
[SKILL-BUILD] [PASS] All targets built successfully.
```

产物路径：
- `build/skill-verify/Y2Kmeter_artefacts/RelWithDebInfo/Standalone/Y2Kmeter.exe`
- `build/skill-verify/Y2Kmeter_artefacts/RelWithDebInfo/VST3/Y2Kmeter.vst3/Contents/x86_64-win/Y2Kmeter.vst3`

---

## 4. 何时跳过完整编译

| 场景 | 处理 |
|---|---|
| 仅改 `.md` 文档 | 跳过 |
| 仅改 `Milkdrop3Module.cpp` / `Milkdrop3Api.cpp` / 其他 `.cpp` 且 `read_lints` 通过 | 增量 build（`_incremental_build.bat` / `_bg_incremental_build.bat`，速度比完整构建快数倍） |
| 涉及 `pluginshell.h` 或任何其他 `.h`（含 `Milkdrop3Api.h` / `Milkdrop3Module.h`） | **必须**完整编译 |
| 涉及 `pluginshell.cpp` | 同普通 `.cpp`，增量即可（不改头文件就不需要 re-configure） |
| 涉及 `CMakeLists.txt` | **必须**完整编译（会自动触发 re-configure） |

---

## 5. 常见编译失败对照

### 5.1 链接错误

| 错误消息 | 根因 | 修复 |
|---|---|---|
| `LNK2001: 无法解析的外部符号 "g_use_C_locale"` | Milkdrop3Api.cpp 顶部全局符号被误删 | 恢复 `_locale_t g_use_C_locale;` |
| `LNK2001: 无法解析的外部符号 "keyMappings"` | 同上 | 恢复 `char keyMappings[8];` |
| `LNK2019 __asm` 相关未解析符号 | `asm-nseel-x86-msvc.c` x64 stub 被删 | 恢复 `#ifdef _M_IX86 ... #else DECL_STUB(fn)` 结构 |
| `LNK2019 D3DXCreateFontW` 等 D3DX9 符号 | CMakeLists.txt 中 `d3dx9.lib` / `legacy_stdio_definitions.lib` 依赖丢失 | 检查 `target_link_libraries` |

### 5.2 编译错误

| 错误消息 | 根因 | 修复 |
|---|---|---|
| `C2065: 'i' 未声明的标识符` | 上游 `for(i=0;...)` 在 MSVC x64 严格模式失败 | 手工补 `int` 声明 |
| `C2059: 缺少 ;` in `Milkdrop3Api.h` line 31 附近 | 块注释含 `*/` 提前关闭 | 检查注释 |
| `C4996: sscanf/strcpy` 之类的 deprecated | 未定义 `_CRT_SECURE_NO_WARNINGS` | 该宏应已在引擎 target 上定义；勿在 Y2Kmeter 侧 undef |
| `C2039: 'kSpectrumSize' is not a member of 'AudioSnapshot'` | 错记符号名 | 用 `AnalyserHub::spectrumMagSize` |
| `C2065: 'closeButtonPressed' 未声明的标识符`（在 `Milkdrop3Module.cpp::paint`） | `ModulePanel::closeButtonPressed` 是 **`private`**，子类不可直接引用 | 删除自定义标题栏绘制，让 `ModulePanel::paint(g)` 走默认路径（只用 `paintContent` / `paintOverChildren`） |
| `C2065: 'oldPn'/'nullBr'/'oldBr2' 未声明的标识符`（在 `PaintJumpDialog` 的 Cancel 按钮段） | copy-paste 从 Go 按钮得来；C++ 块作用域禁止跨 `if` 块引用局部变量 | Cancel 段重新声明（建议改名为 `oldPnCn` / `nullBrCn` / `oldBrCn`） |
| `C2061: 语法错误: 标识符 'hdc'/'FillRect'` 大面积 | 方法体多一个（或少一个）`}`，后续代码脱离类作用域 | 用 `read_file` 从该方法头 `void PaintJumpDialog(...)` 到结尾 `}` 逐行校对括号匹配 |
| `C2065: '方法名' 未声明的标识符`，伴 `C4183 缺少返回类型` | 同上——括号不匹配导致成员函数被解析为全局函数 | 同上 |
| `C2440: '<function-style-cast>': 无法从"std::wstring"转换为"juce::String"` | `juce::String` 没有接受 `std::wstring` 的构造函数；常见于调用返回 `std::wstring` 的 API 后直接传给 `juce::String()` | 加 `.c_str()`：`juce::String(api.GetXxx().c_str())`。`juce::String` 接受 `const wchar_t*` |

### 5.3 CMake 阶段错误

| 错误消息 | 根因 | 修复 |
|---|---|---|
| `Could not find MSBuild.exe` / vcvars 找不到 | 脚本中 vcvars 路径不匹配你的 VS 安装 | 编辑 `scripts/build_skill_verify.bat` 中的 `VCVARS=...` |
| `CMake Error: The source ... does not match ...` | build 目录与源目录关联错乱 | 删掉 `build/skill-verify/` 重来 |

---

## 6. 常见运行时故障对照

| 现象 | 根因 | 修复 |
|---|---|---|
| 画面在左上，偏移随离原点距离增大 | 违反坐标铁律 | `CreateHWNDOnly` / `Reposition` 加 `logicalToPhysical` |
| MilkDrop 一直是零向量音频 | 忘记 `addFrameListener` | 构造函数追加 |
| 添加模块瞬间卡死 | 主线程 vs GL 线程堆分配（线程铁律） | 用 `startTimer(5)` / `callAsync` 延迟 |
| 拖动模块 D3D9 窗口跟不上 | Reposition 未每帧检查或 DPI 未换算 | 检查 timer phase 4 + `logicalToPhysical` |
| 预设名 JUCE 侧看不见 | 违反 z-order 铁律 | 改用 `m_bShowPresetInfo` + `LaunchSongTitleAnim` |
| 右键菜单不弹出 workspace 菜单 | 模块内自行覆写右键 | 删掉自定义右键处理 |
| `WM_LBUTTONDOWN` 转发时机偶发丢失 | `PostMessage(GetParent(hwnd), ...)` 路径不可靠 | 改在 popup 侧 `callAsync` 处理 |
| 软件根本打不开，堆栈含 `<unknown> 0x00007ffc*d9f8` + `Component::toFront` + `visibilityChanged` + `Y2KStandaloneApp::initialise` | Standalone 启动序列违反铁律 6（`ntdll!LdrLockLoaderLock` 死锁） | 回滚 `Y2KStandaloneApp::initialise` 到 v2.3.4 序列，删除 `TimerThreadBoot` 与 `setVisible(false)+setVisible(true)` 分裂。详见 [init-sequence.md](init-sequence.md) |
| 控件栏 overlay 与 D3D 交互后彻底不可见、无法重新展开 | `SetWindowPos(overlay, d3d_child_hwnd_, ...)` 把 overlay 塑到了 D3D 下面 | `SetWindowPos(overlay, HWND_TOP, ...)` |
| 跳转预设弹窗回车后预设未切换（需重新聚焦才发现编号变了） | Enter 处理写在 `WM_CHAR`（单行 EDIT 不可靠） | 搬到 `WM_KEYDOWN` |
| 全软件外围多了黑边 / 标题栏变小圆圈 / 右侧边消失 | 误改 `juce_Windowing_windows.cpp`（UWPUIViewSettings / renderer / 窗口样式 / borderThickness） | `git checkout -- third_party/JUCE/**` 回滚；DPI 问题只在 milkdrop3 内部解决 |

---

## 7. 环境变量与工具链选择建议

如果你的 VS 安装在不同路径，或用了不同版本：

编辑 `scripts/build_skill_verify.bat` 顶部：
```bat
set VCVARS="<你的 VS 路径>\VC\Auxiliary\Build\vcvars64.bat"
```

若你偏好 Ninja 生成器：将 `-G "NMake Makefiles"` 改为 `-G "Ninja"`（需自行确保 `ninja` 在 PATH 中）。

若你偏好 VS 多配置生成器：将 `-G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo` 改为 `-G "Visual Studio 18 2026" -A x64`，并在 `cmake --build` 加 `--config RelWithDebInfo`。此模式与 CLion "让 CMake 决定" 的行为不同，仅推荐 CI 使用。

---

## 8. AI 编译验证协作流程（四步闭环）

以下流程是 AI 在 milkdrop3 代码修改后**自主完成编译验证**的标准步骤。核心改进：**构建脚本以后台进程启动（`start /MIN`），AI 终端立即恢复控制权**，然后通过轮询日志文件等待构建结果。

---

### Step 1：静态自检（必须先行）

在触发编译前，先用静态检查排除低级错误：

```bash
# 1) MSVC lint
read_lints source/ui/modules/Milkdrop3Module.cpp  (及其他修改过的 .cpp/.h)

# 2) 启动序列自检
py -3 docs/skills/milkdrop3-dev-guard/scripts/check_init_sequence.py

# 3) milkdrop3 禁止事项自检
py -3 docs/skills/milkdrop3-dev-guard/scripts/check_forbidden_patterns.py
```

三项任一未通过 → 先修复再进入 Step 2。

---

### Step 2：后台启动构建（不阻塞 AI 终端）

**不要**直接运行 `build_skill_verify.bat` 或 `_incremental_build.bat`——这会卡在 `terminal` 工具里直到超时。改用后台启动器：

| 场景 | 命令 |
|---|---|
| CMake cache 不存在（首次构建） 或 修改了 `.h`（含 `pluginshell.h`）/ `CMakeLists.txt` | `cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_bg_build.bat"` |
| 只修改了 `.cpp`（含 `pluginshell.cpp`）且 CMake cache 已存在（增量） | `cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_bg_incremental_build.bat"` |

**执行效果**：
- 一个最小化的 cmd 窗口被启动（标题栏 `Y2K-Skill-Build` / `Y2K-Skill-IncBuild`），内部执行真正的构建脚本
- AI 的 `terminal` 工具**立即返回**（0.1 秒内），不会超时
- 终端输出看到 `[SKILL-BUILD] Background build launched.` 即可

> **原理**：`_bg_*.bat` 内部使用 `start /MIN cmd /c ...` 创建一个全新的独立进程来运行构建。即使 AI 终端断开，构建进程仍在后台运行，日志持续写入 `build\skill-verify\_build_log.txt`。

---

### Step 3：轮询日志判定通过/失败

构建在后台运行期间，AI 需要**反复读取日志文件**来跟踪进度。每次轮询间隔 5~10 秒：

```bash
# 方式 A：用 terminal 快速查看日志尾部（推荐，最快）
tail -10 build/skill-verify/_build_log.txt

# 方式 B：用 grep 检查是否已有结论
grep -E "\[PASS\]|\[FAIL\]" build/skill-verify/_build_log.txt
```

**polling 循环规则**：

| 日志尾部状态 | 含义 | 动作 |
|---|---|---|
| 无日志文件 或 只有 `=== Y2Kmeter Skill Build Start ===` | 构建刚启动，VS toolchain 初始化中 | 等待 5 秒，重新 poll |
| 看到 `[SKILL-BUILD] [2/3]` 或 `[3/3]` | NMake 正在编译 | 等待 10 秒，重新 poll |
| 看到 `[SKILL-BUILD] [PASS] ...` | ✅ 编译成功 | 进入 §5/§6 对照表排查；回到 Step 2 后台重构建 |
| 看到 `[SKILL-BUILD] [FAIL] ...` | ❌ 编译失败 | 进入 Step 4 定位错误 |
| 日志文件存在但长时间（>3 min 全量 / >1 min 增量）无 `[PASS]`/`[FAIL]` | 构建可能卡死 | 检查后台窗口是否仍在运行（任务栏可见 `Y2K-Skill-*` 窗口），若窗口已消失则重新启动 |

**关键原则**：
- **不要**用 `read_file` 反复读整个日志（文件可能数 MB），优先用 `terminal tail` 或 `grep_search`
- **不要**在启动后台构建后立即 poll——至少等 3~5 秒让 VS toolchain 初始化完成
- 全量构建首轮编译可能需要 2~5 分钟，增量构建通常 20~60 秒
- 构建完成后后台 cmd 窗口自动关闭（exit code 0 时）或保持打开显示错误信息（exit code ≠ 0 时）

---

### Step 4：定位错误（仅在 Step 3 判定 FAIL 时）

```bash
# 查看所有编译/链接错误
grep -in "error " build/skill-verify/_build_log.txt | head -30

# 仅过滤链接错误
grep -in "LNK" build/skill-verify/_build_log.txt

# 查看错误上下文（前 3 行 + 后 3 行）
grep -in -B3 -A3 "error " build/skill-verify/_build_log.txt | head -60
```

拿到错误行后，进入 compile-verify.md §5/§6 对照表排查；回到 Step 2 后台重构建。

---

### 闭环约束

- **必须**使用 `_bg_build.bat` / `_bg_incremental_build.bat` 后台启动器，**禁止**直接调同步脚本——后者必然导致终端超时，且进程可能被杀导致日志不完整。
- **不要**跳过 Step 1 直接编译——大部分低级错误（`closeButtonPressed` private 访问、`WM_CHAR+VK_RETURN`、`SetWindowPos` 语义反向等）在静态阶段就能发现，省去编译等待。
- **不要**因一次 poll 看不到 `[PASS]`/`[FAIL]` 就认为构建失败——耐心轮询，全量构建最多等 5 分钟。
- **不要**在构建进行中手动删 `_build_log.txt` 或 `build\skill-verify\`——后台进程正在写入。
- 若连续 2 次编译 FAIL 且无法从日志定位根因 → 把 `_build_log.txt` 尾部 50 行贴给用户求助。
- 若后台构建窗口无响应或日志停止更新超过 3 分钟 → 手动关闭 `Y2K-Skill-*` 标题栏的 cmd 窗口，重新启动。

### 8.3 修改后必跑的两个静态脚本

| 脚本 | 作用 |
|---|---|
| `scripts/check_init_sequence.py` | 对照铁律 6 扫描 `Y2KStandaloneApp::initialise` 启动序列反模式 |
| `scripts/check_forbidden_patterns.py` | 对照 §4.3 扫描 milkdrop3 侧需避免的写法（右键处理、JUCE 触碰、调试痕迹、`WM_CHAR + VK_RETURN`、`SetWindowPos + d3d_child_hwnd_`） |

---

## 9. 已知限制与排错指南（v1.3.0 补充）

### 9.1 vcvars 输出重定向与文件句柄继承锁

**问题**：`build_skill_verify.bat` / `_incremental_build.bat` 中，若将 vcvars 的输出重定向到主日志文件（`call "%VCVARS%" >> "%LOG%" 2>&1`），vcvars 的子进程（`vcvarsall.bat`、`cl.exe` 环境探测等）会**继承日志文件的句柄**。当这些子进程持有句柄时，后续的 `cmake` / `nmake` 写入 `>> "%LOG%"` 会静默失败（"The process cannot access the file because it is being used by another process"），导致日志文件只有 vcvars 输出、CMake/NMake 输出完全丢失，但 exit code 被误报为 0（成功）。

**修复**（已在 v1.3.0 版本中实施）：
```bat
REM 正确做法 —— vcvars 输出重定向到 nul
call "%VCVARS%" >nul 2>&1

REM 错误做法 —— 重定向到主日志文件
call "%VCVARS%" >> "%LOG%" 2>&1   ← 会锁住文件！
```

控制台仍会打印 `[SKILL-BUILD] [1/3] ...` 和 `[SKILL-BUILD] VS toolchain ready.` 等进度标记，AI 可据此判断 VS 初始化是否成功。vcvars 自身 ~300 行的详细输出来源信息丢给 `nul`，不影响排错。

> **教训**：任何包含子进程 spawn 的外部命令（`call` 的 .bat、`start`、管道等），其输出**不要**重定向到主进程也在写的日志文件。要么单独写临时文件，要么丢 `nul`。

### 9.2 `%~dp0` 在 Git Bash `cmd //c` 调用下的路径解析不可靠

**问题**：`_bg_build.bat` / `_bg_incremental_build.bat` 最初使用 `%~dp0..\..\..\..` 推导项目根目录。但在 Git Bash 的 `cmd //c "docs\skills\...\script.bat"` 模式下，`%~dp0` 有时解析为 Git Bash 的临时目录而非脚本所在目录，导致 `cd /d` 切到错误路径、后续脚本失败。

**修复**（已在 v1.3.0 版本中实施）：后台启动器脚本使用硬编码绝对路径 `I:\Y2KMeter`，避免任何运行时路径推导：
```bat
set "PROJECT_ROOT=I:\Y2KMeter"
cd /d "%PROJECT_ROOT%"
...
start "Y2K-Skill-Build" /MIN cmd /c "cd /d I:\Y2KMeter && I:\Y2KMeter\docs\skills\milkdrop3-dev-guard\scripts\build_skill_verify.bat"
```

`build_skill_verify.bat` 和 `_incremental_build.bat` 不依赖 `%~dp0`（使用相对路径 `build\skill-verify\`），因此正常。

> **教训**：跨环境调用的脚本（被 `cmd //c` 从 Git Bash 启动）不要依赖 `%~dp0`，使用绝对路径。如果将来需要移植到其他机器或驱动器，需手动修改脚本中的硬编码路径。

### 9.3 `start /MIN` 后台窗口的监控盲区

**问题**：AI 无法直接观察 Windows 桌面 GUI，因此看不到 `start /MIN` 创建的 `Y2K-Skill-*` 最小化 cmd 窗口。如果构建进程异常挂起或静默退出（exit code 0 但产物缺失），AI 只能通过日志文件的时间戳停滞来推测。

**已知场景与应对**：

| 异常场景 | 表现 | AI 应对 |
|---|---|---|
| vcvars 调用卡住（罕见，VS 安装损坏时） | 日志停在 `[SKILL-BUILD] [1/3]`，无后续输出 | 等待 1 分钟后 `rm -rf build/skill-verify` 重建 |
| NMake 编译挂起（引擎死循环链接等） | 日志停在某个 `[XX%]` 进度 | 等待 5 分钟后重试；可尝试 `tasklist \| grep nmake` 确认 |
| 后台窗口被意外关闭 | 日志突然停止，最后一行不是 `[PASS]`/`[FAIL]` | 重新 `_bg_build.bat` |
| 日志文件锁残留（上次后台进程未清理） | 日志只有 vcvars，CMake/NMake 输出缺失 | `rm -rf build/skill-verify && _bg_build.bat` |

### 9.4 增量 vs 全量构建的精确判定

`compile-verify.md` §4 和 `SKILL.md` §6.3 中的判定规则已从 v1.3.0 起修正为：

| 条件 | 构建类型 | 原因 |
|---|---|---|
| CMake cache 不存在 | 全量 `_bg_build.bat` | 必须 configure |
| 修改了 `.h` 文件（含 `pluginshell.h`） | 全量 `_bg_build.bat` | 头文件变化需要重新扫描依赖图（NMake 自动识别，但安全起见走全量） |
| 修改了 `CMakeLists.txt` | 全量 `_bg_build.bat` | 必须 re-configure |
| 只改了 `.cpp` 文件（含 `pluginshell.cpp`、`Milkdrop3Window.cpp` 等）且 cache 存在 | 增量 `_bg_incremental_build.bat` | 跳过 configure，NMake 自动检测并只编译变更的 translation unit |

> **注意**：`pluginshell.cpp` 是 `.cpp` 文件，修改它**不需要**全量重构建，增量即可。只有 `pluginshell.h`（头文件）才需要全量构建。

### 9.5 轮询最大次数建议

后台构建启动后，AI 不应无限轮询。建议轮询上限：

- 全量构建：最多 poll 12 次（每次间隔 10 秒 ≈ 2 分钟）。超过后检查日志是否仍在增长，若停滞则重新启动。
- 增量构建：最多 poll 6 次（每次间隔 10 秒 ≈ 1 分钟）。

若达到上限仍无 `[PASS]`/`[FAIL]`，应假设后台进程异常退出，`rm -rf build/skill-verify` 后重试全量构建。