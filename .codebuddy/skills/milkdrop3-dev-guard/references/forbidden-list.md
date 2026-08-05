# Forbidden List (禁止事项详解)

> 本文档为 [SKILL.md](../SKILL.md) §4.3 的详细版。含每条禁止的**根因**与**历史事故轮次**。

---

## F1. 第三方引擎白名单外的修改

`third_party/milkdrop3/**` 的修改必须只发生在：

| 文件 | 允许的修改 |
|---|---|
| `vis_milk2/pluginshell.h` | 在 public 区新增/维护 `m_y2k*` 前缀的外部注入字段 |
| `vis_milk2/pluginshell.cpp::AnalyzeNewSound` | 在 `time_to_frequency_domain(...)` 之后消费并 consume-once |

**理由**：v6~v14 阶段已完成 x64 / MSVC / DirectXMath 兼容修复。之后任何触动 `plugin.cpp` / `state.cpp` / `dxcontext.cpp` / `menu.cpp` / `ns-eel2/**` 都可能在 LNK 阶段引发大批错误，恢复成本极高。

**处理**：白名单外的引擎修改需求必须先向用户明示并等待确认。

---

## F2. 右键处理

**禁止**：
- `Milkdrop3Module::mouseDown` 内 `if (e.mods.isRightButtonDown())` 分支。
- `D3dChildWindow::WndProc` 内 `case WM_RBUTTONDOWN:`。

**根因**：基类 `ModulePanel::mouseDown` 的默认右键行为承担「弹出 workspace 添加模块菜单」责任。模块内自行覆写会与 workspace 菜单冲突。

**替代**：所有 milkdrop3 交互（切预设、聚焦、拖动、点击弹窗）只用左键。

---

## F3. 从 D3D9 popup 向 JUCE 父窗口转发事件

**禁止**：`D3dChildWindow::WndProc` 内 `PostMessage(GetParent(hwnd), WM_LBUTTONDOWN, ...)` 或 `SendMessageW(...)`。

**根因**：附录 D.4 第三、四轮记录，此路径存在偶发失效与并发问题。

**替代**：直接在 popup 侧处理：
```cpp
case WM_LBUTTONDOWN:
  juce::MessageManager::callAsync([owner = weak_owner]{
    if (auto strong = owner.lock()) strong->NextPreset(...);
  });
  return 0;
```

---

## F4. enterModalState 与独立子组件浮层

**禁止**：
- `enterModalState(true)` —— 会锁死软件全局交互（附录 E.1 轮次 6）。
- 独立 `juce::Component` 子组件作为浮层 —— 被 D3D9 HWND 遮挡（附录 E.1 轮次 6）。

**替代**：预设跳转、消息等弹窗都做成 `Milkdrop3Module` 内部状态驱动的 in-place 面板（扩展控制栏高度让出 popup 空间）。

---

## F5. 残留调试日志与 belt-and-suspenders 代码

v6~v19 修复偏移期间引入过大量调试日志，v20 已全部清理。**禁止**再次引入：

| 反模式 | 根因 |
|---|---|
| `MonitorFromPoint + GetDpiForMonitor` 只做日志不参与计算 | 死代码，且会误导后续维护者 |
| `SetThreadDpiAwarenessContext(...)` | 进程级已 PMv2，函数/线程级重复切换无收益 |
| `GetDpiForWindow(parent_hwnd)` 算 `kDpiScale` | 与 `logicalToPhysical` 结果不一致时反而制造 bug |
| `Md3BuildTagLogger` / `MD3_BUILD_TAG = "vNN_YYYYMMDD_..."` | 构建标签在实际排障中未发挥作用，反而污染 pdb |
| `#pragma message` 编译期噪音 | 淹没真实警告 |

---

## F6. 已删死接口的复活

v20 清理阶段删除的接口/字段**禁止**复活：

- `Milkdrop3Api::Initialize()`（合一大函数，已拆为 `Initialize_CreateRenderWindow / Initialize_SetPaths / Initialize_PreInit / Initialize_CreateDevice / Initialize_PluginInit` 五步）
- `Milkdrop3Api::CreateRenderWindow(HWND, int, int)`（单句包装，已被 `Initialize_CreateRenderWindow` 替代）
- `Milkdrop3Api::SetPresetDir(...)`（外部调用不生效——路径由 `Initialize_PreInit` 内固定为 `<EXE_DIR>\milkdrop_presets\`）
- `Milkdrop3Api::CycleRenderScale / ApplyRenderScale / render_scale_`
- `Milkdrop3Module::hub_retained_` 冗余布尔（已由 RAII + release 保证）

**如果新需求需要类似能力**，必须重新设计接口而非复活旧接口。

---

## F7. 死锁陷阱：主线程 vs GL 线程堆分配

**禁止**：
- 在 `ModuleWorkspace::getDefaultSizeForType(ModuleType::milkdrop3)` 中 `factory(milkdrop3)` 或 `new Milkdrop3Module` 拿实例算尺寸（附录 E.1 轮次 5）。
- 在 `getHoverPreviewImage` 中渲染真实 milkdrop3 模块（附录 E.1 轮次 5）。
- `Editor` 构造函数末尾同步 `openGLContext.attachTo(*this)`（附录 E.3）。
- `renderOpenGL` 里做字体 `measureMax` 之类的重型 GDI 调用。

**替代**：
- 尺寸表用静态常量。
- 预览用静态占位图。
- GL attach 用 `attachOpenGLContext()` + `MessageManager::callAsync` 延迟。

---

## F8. UB 与野生转换

**禁止**：
- `static_cast<unsigned char>(static_cast<int8_t>(sample * 127.0f))` 依赖有符号→无符号隐式回绕（UB）。
- `lround` 越界后再窄化。
- 假设 `LoadRandomPreset` 一定随机——`m_bSequentialPresetOrder=true` 时它走 `m_nCurrentPreset++`。

**正确**：
```cpp
// PCM float → unsigned char (0..255, centered at 128)
float clamped = std::clamp(sample, -1.0f, 1.0f);
unsigned char byte = static_cast<unsigned char>(clamped * 127.0f + 128.0f);
```

```cpp
// Random preset (临时关闭 sequential)
bool prev = plugin->m_bSequentialPresetOrder;
plugin->m_bSequentialPresetOrder = false;
plugin->LoadRandomPreset(blend);
plugin->m_bSequentialPresetOrder = prev;
```

---

## F9. wchar 缓冲越界

**禁止**：`swprintf(m_szSongTitle, L"%s", ...)` 之类无长度限制的写入。

**正确**：`_snwprintf_s(dst, 512, _TRUNCATE, L"%ls", src)`（`m_szSongTitle` 是 `wchar_t[512]`）。

---

## F10. 改动 Standalone 启动序列

**禁止**：在 `Y2KStandaloneApp::initialise` 中引入以下任何一项：

- `mainWindow` 创建提前到 `pluginHolder = std::make_unique<...>()` 之前
- 先 `addToDesktop()` 再 `setVisible(false)`，然后到函数末尾才 `setVisible(true)` （分裂显示）
- `TimerThreadBoot` / `SharedResourcePointer<TimerThread>` 预热 dummy
- `SetThreadDpiAwarenessContext`（应用级只能用 `SetProcessDpiAwarenessContext`）
- 在 `addToDesktop()` 与 `setVisible(true)` 之间插入任何代码

**根因**：Windows 进程共享一把 `LdrLockLoaderLock`。一旦主线程的 native 路径（`setVisible → toFront → SetForegroundWindow → LoadLibrary(uxtheme/dcomp)`）与 audio 线程（`WASAPI CoCreateInstance → LoadLibrary(mmdevapi/driver dlls)`）为锁开战，主线程会卡在 `0x00007ffc*d9f8`（`ntdll!LdrLockLoaderLock`）无法恢复。

**正确**：严格遵循铁律 6 （→ [architecture-rules.md](architecture-rules.md) / [init-sequence.md](init-sequence.md)）：
```
SetProcessDpiAwarenessContext(PMv2)
pluginHolder = std::make_unique<StandalonePluginHolder>(...)
主题恢复 + shouldMuteInput=false
reloadPluginState()
mainWindow = std::make_unique<Y2KMainWindow>(...)   // 不 addToDesktop
createEditor + setContentNonOwned + populateAudioSources + ...
restoreBounds
mainWindow->addToDesktop();
mainWindow->setVisible(true);
```

**历史事故**：v22。具体回顾见 [init-sequence.md](init-sequence.md) 末尾。

---

## F11. 触碰 `third_party/JUCE/**` 尤其是 `juce_Windowing_windows.cpp`

**禁止**：为解决 milkdrop3 相关的 DPI / 边框 / 层级问题去修改 JUCE 原生窗口代码，包括但不限于：

- `UWPUIViewSettings` / 注释掉 `UWPUIViewSettings uwpViewSettings;`
- `DwmSetWindowAttribute` / `DWMWA_NCRENDERING_POLICY` / `DWMWA_USE_IMMERSIVE_DARK_MODE`
- `DwmExtendFrameIntoClientArea`
- `renderer` / `Direct2D` / `SoftwareImageType` 相关的渲染后端选择
- `WS_THICKFRAME` / `WS_BORDER` / `WS_EX_*` 之类的窗口样式改动
- `getBorderThickness()` / `getContentComponentBorder()`（`ResizableWindow`）

**根因**：这些代码属于**全软件共享**的原生窗口创建路径，一次修改会同时改变**所有**顶层窗口的外框、标题栏、DPI 行为。历史上（本轮）尝试用它们去消除 D3D9 popup 偏移，反而引发：

- 软件整体外围出现黑色粗边框；
- 标题栏文字（软件名 / 版本 / 网站）变成一个小圆圈；
- 修补后又出现"右侧边缺肉、左侧还在"的非对称边框。

**替代**：milkdrop3 的坐标/边框问题只能在 milkdrop3 内部解决——用 `logicalToPhysical` 换算、用 owned popup 的 z-order、用引擎 overlay 绘制。**JUCE 目录必须保持 pristine**：`git status third_party/JUCE` 期望永远空。

**处理**：若已误改，立即 `git checkout -- third_party/JUCE` 完整回滚。

---

## F12. 直接访问 `ModulePanel::closeButtonPressed` / `closeButtonHovered`

**禁止**：在 `Milkdrop3Module`（或任何 `ModulePanel` 子类）里写：

```cpp
if (closeButtonPressed) g.fillRect(...);
if (closeButtonHovered) g.setColour(...);
```

**根因**：这两个成员在 `ModulePanel` 中位于 `private` 段（不是 `protected`），子类 C++ 访问控制**禁止**引用。历史上（本轮第 12 轮 FAIL）AI 试图在 `Milkdrop3Module::paint` 里画自定义标题栏 + 关闭按钮，直接照抄基类逻辑 → MSVC C2065 大面积报错。

**可用的 protected 接口**（`ModulePanel` 子类可用）：
- `titleText`
- `getTitleBarBounds()`
- `getCloseButtonBounds()`
- `getContentBounds()`
- `paintContent(...)`
- `layoutContent(...)`

**替代**：
- 首选让 `ModulePanel::paint(g)` 走默认路径完成标题栏 + 关闭按钮绘制；子类只用 `paintContent`/`paintOverChildren` 追加自己的内容。
- 若确实需要覆写整个 `paint`：hover/press 状态由子类**自行**用 `mouseEnter/mouseExit/mouseDown/mouseUp` 维护；**不要**去动基类的 private。

---

## F13. `SetWindowPos` 中把 `hWndInsertAfter` 参数误当"置于其上"

**禁止**：

```cpp
// 想把 overlay 提到 D3D popup 之上，写成：
SetWindowPos(overlay_hwnd_, d3d_child_hwnd_, 0, 0, 0, 0,
             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // ← 反了！
```

**根因**：Win32 `SetWindowPos(hwnd, hWndInsertAfter, ...)` 的语义是"把 `hwnd` 放到 `hWndInsertAfter` **之后（下方）**"。传 `d3d_child_hwnd_` 相当于把 overlay 塞到 D3D popup 之下，导致 overlay 被 D3D 全部覆盖不可见。历史上（本轮第 3 轮）该错误让"抬头控制区完全无法展开"卡了整整一轮。

**正确**：

```cpp
// overlay 想置顶：
SetWindowPos(overlay_hwnd_, HWND_TOP, 0, 0, 0, 0,
             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
```

若坚持要用 owned popup 的相对顺序控制层级：确保 overlay HWND **在 D3D popup 之后创建**（Windows 会按创建顺序给同一 owner 的 popup 排 z-order，后创建的靠上）；这已经是 milkdrop3 现在的做法，无需再用 `hWndInsertAfter` 手工干预。

---

## F14. 在 EDIT 子控件 `WM_CHAR` 里检测 `VK_RETURN`

**禁止**：

```cpp
case WM_CHAR:
  if (wparam == VK_RETURN) { self->DoPresetJump(); return 0; }
  if (wparam == VK_ESCAPE) { self->CloseJumpDialog(); return 0; }
  break;
```

**根因**：单行 `EDIT` 控件里 Enter 键是否投递到 `WM_CHAR` 依赖：

- 父窗口是否为 dialog（我们不是）；
- 消息循环里是否有 `IsDialogMessage` （我们没有）；
- `ES_WANTRETURN` 样式（我们没设）。

三者都不满足时 Enter 常常**不**触发 `WM_CHAR`，用户输入完编号回车后完全无反应，需要重新点击模块聚焦一次才发现"编号变了但预设没切"。本轮第 1-2 轮反复踩坑。

**正确**：`VK_RETURN` 与 `VK_ESCAPE` 统一放到 `WM_KEYDOWN`：

```cpp
case WM_KEYDOWN:
  if (wparam == VK_RETURN) { self->DoPresetJump();     return 0; }
  if (wparam == VK_ESCAPE) { self->CloseJumpDialog();  return 0; }
  break;
```

---

## F15. 复用相邻 `if` / `for` 块内声明的局部变量

**禁止**：

```cpp
if (!goPressed) {
  HPEN oldPn = static_cast<HPEN>(SelectObject(hdc, goPen));
  HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  HBRUSH oldBr2 = static_cast<HBRUSH>(SelectObject(hdc, nullBr));
  Rectangle(hdc, ...);
  SelectObject(hdc, oldPn);
  SelectObject(hdc, oldBr2);
  DeleteObject(goPen);
}

if (!cnPressed) {                     // ← 相邻块
  HPEN cnPen = CreatePen(...);
  oldPn = static_cast<HPEN>(SelectObject(hdc, cnPen));  // C2065 未声明
  oldBr2 = ...;                                          // C2065
  ...
}
```

**根因**：C++ 块作用域规则——`if` 块内声明的变量仅在该块可见，相邻块看不到。历史上（本轮第 14 轮）copy-paste Go 按钮代码得到 Cancel 按钮时踩到，5 处 C2065。

**正确**：Cancel 段独立声明（或把公共变量提到两块之外）：

```cpp
if (!cnPressed) {
  HPEN cnPen = CreatePen(PS_SOLID, 1, theme_pink300);
  HPEN oldPnCn = static_cast<HPEN>(SelectObject(hdc, cnPen));
  HBRUSH nullBrCn = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  HBRUSH oldBrCn = static_cast<HBRUSH>(SelectObject(hdc, nullBrCn));
  Rectangle(hdc, ...);
  SelectObject(hdc, oldPnCn);
  SelectObject(hdc, oldBrCn);
  DeleteObject(cnPen);
}
```

**通用防御**：写完一段绘制/资源管理代码后，若要 copy-paste 复用，**必须**给每个块的局部变量重命名或重新声明，禁止跨块隐式复用。
