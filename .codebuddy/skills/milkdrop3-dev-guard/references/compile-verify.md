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

## 2. 一键编译

从项目根目录 `I:/Y2KMeter/` 执行：

```bash
# Git Bash / PowerShell / cmd 均可
cmd //c docs/skills/milkdrop3-dev-guard/scripts/build_skill_verify.bat
```

脚本行为：
1. 调用 `vcvars64.bat` 建立 VS 编译环境；
2. 若 `build/skill-verify/` 不存在，先做一次 configure（`cmake -S . -B build/skill-verify -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo`）；
3. `cmake --build build/skill-verify -j` 触发多核编译。

---

## 3. 期望输出

编译尾部应见：
```
[100%] Linking CXX shared module Y2Kmeter_artefacts\RelWithDebInfo\VST3\Y2Kmeter.vst3\...
[100%] Built target Y2Kmeter_Standalone
[100%] Built target Y2Kmeter_VST3
```

产物路径：
- `build/skill-verify/Y2Kmeter_artefacts/RelWithDebInfo/Standalone/Y2Kmeter.exe`
- `build/skill-verify/Y2Kmeter_artefacts/RelWithDebInfo/VST3/Y2Kmeter.vst3/Contents/x86_64-win/Y2Kmeter.vst3`

同时 `install` 环节会把 VST3 及 `projectM-4.dll` / `glew32.dll` 复制到 `C:\Program Files\Common Files\VST3\` 与产物旁。

---

## 4. 何时跳过完整编译

| 场景 | 处理 |
|---|---|
| 仅改 `.md` 文档 | 跳过 |
| 仅改 `Milkdrop3Module.cpp` / `Milkdrop3Api.cpp` 且 `read_lints` 通过 | 增量 build（`cmake --build build/skill-verify -j`，无需重 configure） |
| 涉及 `pluginshell.h/cpp` | **必须**完整编译 |
| 涉及任何 `.h`（含 `Milkdrop3Api.h` / `Milkdrop3Module.h`） | **必须**完整编译 |
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

## 8. AI 编译验证协作原则（防卡死）

本项目历史上多次出现 AI 在命令行内反复尝试编译、又一封段时间只在与 Git Bash 的 `cmd //c` 转义斗争。为避免浪费轮次：

### 8.1 优先静态自检

在 AI 侧完成代码修改后，**优先**运行以下静态检查而非完整编译：

```bash
# 1) lint
read_lints 相关文件

# 2) 启动序列自检
python docs/skills/milkdrop3-dev-guard/scripts/check_init_sequence.py

# 3) milkdrop3 禁止事项自检
python docs/skills/milkdrop3-dev-guard/scripts/check_forbidden_patterns.py
```

三项都过→交给用户在 CLion 里编译（RelWithDebInfo）。

### 8.2 避免在 Git Bash 里 `cmd //c` 调发 完整编译

已知坐坑：`cmd //c I:\Y2KMeter\...bat` 在 Git Bash 里会因转义递归而变成 `I:Y2KMeter...bat` 无法执行。不要反复尝试各种反斜杠数量。若必须从 AI 侧发起完整编译，优先使用：

1. **CLion 确定可行**：用 `powershell -Command "& { ...vcvars... ; cmake --build ... }"`（仅在使用时验证）。
2. **必要时**：直接把完整编译任务交回用户，AI 仅进行静态自检。
3. 若编译超时 3 次：**停手**，告知用户“需手工在 CLion 编译”。

### 8.3 修改后必跑的两个静态脚本

| 脚本 | 作用 |
|---|---|
| `scripts/check_init_sequence.py` | 对照铁律 6 扫描 `Y2KStandaloneApp::initialise` 启动序列反模式 |
| `scripts/check_forbidden_patterns.py` | 对照 §4.3 扫描 milkdrop3 侧需避免的写法（右键处理、JUCE 触碰、调试痕迹、`WM_CHAR + VK_RETURN`、`SetWindowPos + d3d_child_hwnd_`） |
