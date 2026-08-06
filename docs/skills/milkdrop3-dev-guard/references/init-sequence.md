# 铁律 6：Standalone 启动序列（Y2KStandaloneApp::initialise）

> 本节为 [SKILL.md](../SKILL.md) §4.2 铁律 6 的详细版。这是 milkdrop3 集成 20+ 轮踩坑中**最严重的一次教训**（v22 后修复），花费用户大量返工时间。

---

## 症状回顾

- 用户报告：软件界面无法打开，进程持续卡死。
- 完整堆栈：
  ```
  <unknown>                               0x00007ffcdffcd9f8       ← ntdll!LdrLockLoaderLock
  juce::Component::toFront(bool)          juce_Component.cpp:656
  juce::ResizableWindow::visibilityChanged() juce_ResizableWindow.cpp:183
  juce::Component::sendVisibilityChangeMessage() juce_Component.cpp:372
  juce::Component::setVisible(bool)       juce_Component.cpp:353
  y2k::Y2KStandaloneApp::initialise       Y2KStandaloneApp.cpp:280
  juce::JUCEApplicationBase::initialiseApp()
  juce::JUCEApplicationBase::main()
  ```
- `0x00007ffc*d9f8` 是 `ntdll.dll` 中 `LdrLockLoaderLock`（Windows PE loader lock）的偏移地址，用来验证判断的关键地址签名。

---

## 根本原因

Windows 上每个进程有**一把全局 DLL 加载器锁**（loader lock）。任何触发 `LoadLibraryEx` / `FreeLibrary` 的路径都会争抢它。触发者包括：

- `CreateWindowExW`（隐式加载 `uxtheme.dll` / `dwmapi.dll` / `dcomp.dll`）
- `SetForegroundWindow` / `SetWindowPos` / `BringWindowToTop`（触发 DWM 组合）
- COM `CoCreateInstance`（WASAPI 后端加载音频驱动 DLL）
- `SetProcessDpiAwarenessContext` 内部的 `dcomp.dll` 加载

如果**主线程**和**音频线程**分别持有对方需要的锁，就会死锁。表现为：

- **主线程**：栈里有 `setVisible → toFront → peer->toFront → SetForegroundWindow → LoadLibrary(uxtheme)` 在等 loader lock；
- **音频线程**：栈里有 `WASAPI IAudioClient::Initialize → CoCreateInstance → LoadLibrary(mmdevapi/audiodg 驱动 DLL)` 持有 loader lock 但在等某个 COM 内部的 wait。

---

## 铁律细节

`Y2KStandaloneApp::initialise` 的启动序列**必须**严格遵循如下 7 段，每段之间**不允许**穿插任何 Win32 API 调用、任何 workaround：

```
1. SetProcessDpiAwarenessContext(PMv2)   ← Windows 独有；仅本函数第一行
2. pluginHolder = std::make_unique<StandalonePluginHolder>(...)  ← 音频线程在此启动
3. 主题恢复 + shouldMuteInput=false
4. pluginHolder->reloadPluginState()
5. mainWindow = std::make_unique<Y2KMainWindow>(name, black)    ← 只构造，不 addToDesktop
6. createEditor + setContentNonOwned + populateAudioSources + ...
7. restoreBounds
   ─────────────────────────────────────
   mainWindow->addToDesktop();
   mainWindow->setVisible(true);         ← 二者必须紧挨、最后
```

对应的 v2.3.4 提交是 `89924448`，可 `git show 89924448:source/standalone/Y2KStandaloneApp.cpp` 复核。

---

## 常见反模式（**严禁**）

| # | 反模式 | 后果 |
|---|---|---|
| 1 | `mainWindow` 创建/`addToDesktop()` 提前到 `pluginHolder` 之前 | Native peer 早于音频线程创建；后续 `setVisible(true) → toFront` 与 audio 线程 CoInitialize 竞争 loader lock |
| 2 | 在 `initialise` 中显式 `mainWindow->setVisible(false)` 然后到函数末尾再 `setVisible(true)` | 触发 `visibilityChanged`→`toFront` 两次；第二次通常死锁 |
| 3 | `TimerThreadBoot` / `SharedResourcePointer<TimerThread>` 预热 | 治标不治本；预热本身也会加载 DLL 并可能提前触发死锁 |
| 4 | 在 initialise 中使用 `SetThreadDpiAwarenessContext` | 应用级只用 `SetProcessDpiAwarenessContext`；线程级 DPI 切换会与 DWM composition 争 lock |
| 5 | 在 `addToDesktop` 与 `setVisible(true)` 之间加任何逻辑 | 二者必须原子化紧挨 |
| 6 | 在 `Milkdrop3Module` 构造函数里同步 `CreateWindowExW`（违反铁律 4） | 若模块在 Editor 挂载期被构造，会在 loader lock 竞争窗口爆炸 |

---

## 如何验证

**自动化**：运行 `py -3 docs/skills/milkdrop3-dev-guard/scripts/check_init_sequence.py`。脚本会静态扫描 `Y2KStandaloneApp.cpp`，命中上述反模式则返回非零退出码。

**手工**：
```bash
grep -n "SetProcess\|TimerThreadBoot\|addToDesktop\|setVisible\|mainWindow = std\|pluginHolder = std::make" \
     source/standalone/Y2KStandaloneApp.cpp | head -20
```
预期输出（顺序关键）：
```
… BOOL WINAPI SetProcessDpiAwarenessContext(HANDLE);
… ::SetProcessDpiAwarenessContext(...);
… pluginHolder = std::make_unique<...>(
… mainWindow = std::make_unique<Y2KMainWindow>(
… mainWindow->addToDesktop();
… mainWindow->setVisible(true);
```
如果 `mainWindow = std::make_unique` 出现在 `pluginHolder = std::make_unique` **之前**，立即回滚。

---

## 历史事故

- **v22（本轮）**：AI 为了"避免 milkdrop3 D3D9 popup 因 DPI 差异偏移"，在 initialise 顶端加了 `SetProcessDpiAwarenessContext`（正确），但**同时**把 `mainWindow` 创建提前、加了 `setVisible(false)`+末尾 `setVisible(true)` 的分裂显示、还加了 `TimerThreadBoot` workaround（错误）。结果：软件无法启动。
- **修复**：回滚到 v2.3.4 序列，仅保留最前面的 `SetProcessDpiAwarenessContext`；其余照旧。
