# 窗口 / 布局 / 锁定 / auto-hide / 多屏 DPI

## 1. 窗口行为模型（v2.8.5 起解耦）

三个独立子系统，不再互相耦合：

- **L（布局锁定）** = 纯交互闸门：锁窗口尺寸/位置 + 所有子组件位置/尺寸/存在性。
- **Hide（auto-hide）** = 一次性几何收缩 + 悬停浮层。
- **Stick** = 驻留条状布局（AppBar 占位）。

## 2. 布局锁定（L 按钮）关键规则

- **冻结窗口用 `setResizeLimits(cur,cur,cur,cur)`，不要用 `setResizable(false)`** —— 后者重建 native HWND（DWM 重绘）→ 整屏闪一下。
- **构造期只记意图**：`pendingLockApplyOnAttach=true`，等 `visibilityChanged`（顶层已 attach、尺寸就绪）再 `applyLayoutLocked`；否则 0×0 触发 jassert。
- **分层下沉冻结**：父级拦不住子组件；`ModulePanel`/`VirtuPetModule`/`ModuleWorkspace` 各自的 `mouseMove/mouseDown/mouseDoubleClick/isInterestedInFileDrag` 都要早退（`isPanelLayoutLocked` 上溯查询）。
- 锁定态 clamp 守卫加 `!isLayoutLocked()`（意外 resize 不压缩模块）。
- **保留可用**（否则用户被锁死）：顶栏 4 按钮、双击标题栏切全屏（fullscreen 不走 resize limits）、底部 toolbar 全部（主题/Save-Load/FPS/GAIN/Source/Hide）。

## 3. auto-hide（Hide 按钮）关键机制

- **mouseEnter 不可靠**（窗口 resize 会触发虚假 mouseEnter）→ 用 `mouseMove` 驱动 hover show。
- **mouseExit 用屏幕坐标 + 顶层窗口 bounds 裁决**（跨越子组件会误触发）；不要用 `Desktop::getMainMouseSource().getScreenPosition()`（缓存滞后），用 `MouseEvent::getScreenPosition()`。
- `autoHideNeedsExitFirst`：resize 后要求鼠标先离开一次，才允许 hover show。
- `suppressAutoShowCounter`（int，timer 递减）：`setBounds()` 产生的异步 WM_SIZE/mouse 事件在回调返回后才抵达，`bool` 挡不住，必须用计数器覆盖 ~300ms 窗口期。
- 多层兜底订阅：`TopLevelExitWatcher`（顶层窗口级 mouseExit）、`TopLevelFocusWatcher`（focus 轮询）、`AutoHideChildWatcher`（`addMouseListener(watcher, true)` 收嵌套子组件事件）。
- 顶层 `ResizableWindow` 与 Editor **各有 constrainer**：shrink/expand 都要同步放宽/夹紧 RW 层 limits。

## 4. Stick（驻留条）

- 三槽独立布局空间：模块只进当前槽；退出 Stick 后普通布局里没有它。
- 升级时旧 Stick 槽里的 floating 记录被刻意忽略（只迁 normal 槽）。
- `third_party/JUCE` 的 `withParentAreaOverride` 是仓库内本地扩展（升级 JUCE 要重应用）。
- 全屏中进 stick 先退出全屏（否则备份的是全屏矩形）。

## 5. 多屏混合 DPI（Windows 专属，mac/Linux 保持原逻辑）

**核心结论：窗口几何一律物理像素**（`#if JUCE_WINDOWS`）。

- 工具函数在 `PluginEditor.cpp` 顶部匿名命名空间：`Y2KMonitorPx` / `y2kGetCursorPosPx` / `y2kMoveWindowToRectPx` / `y2kGetMonitorPxForPoint` / `y2kLogicalToPx` 等。
- **跨屏定位用 `SetWindowPos`/`MoveWindow`（物理像素）**；JUCE 跨屏 `setBounds` 会用"窗口当前屏"的 scale 换算，目标屏≠当前屏时原点用错。
- **选屏用点（光标/窗口中心）或物理矩形**；"逻辑∩逻辑"选屏在错表下是自证循环。
- 拖窗用**绝对式** `SetWindowPos(SWP_NOSIZE)`（目标=按下物理位置 + 光标物理位移），自带纠错、不与 `WM_DPICHANGED` 打架。
- 松手吸附基准 = **光标所在屏** `rcWork`，阈值 8 DIP × 该屏 scale。
- `ensureWindowOnScreen()`（10Hz 兜底）：窗口完全离开所有屏则拉回最近工作区；伪全屏在 rcWork 变化时重贴合；最小化态跳过（`GetWindowRect` 返回 -32000 哨兵）。
- 伪全屏禁止拖抬头；全屏退出落盘"进入前 bounds"（否则下次启动是"铺满屏但非全屏"的普通窗口）。
- 诊断开关 `Y2KM_SNAP_DEBUG=1` → `%TEMP%\Y2Kmeter_snap_debug.txt`（dump JUCE Displays 表 + Win32 显示器表）。

### 混合 DPI 经验（易错）

1. "物理矩形精确相接"在混合 DPI 下不是不变量（DIP 相邻 ≠ 物理相邻，有上千像素空洞）。
2. 依赖"物理相接"推导布局的代码（如 JUCE `Displays::updateToLogical` 图遍历）必然判错。
3. vendored JUCE 补丁必须就地打 `[Y2K patch]` 注释，否则升级整包复发。

## 6. 窗口恢复

- 恢复紧凑预设（如 `horizontal bar`）前先 `prepareConstrainerForWindowRestore(w,h)` 放宽 constrainer min/max —— 否则默认 `minH=420` 会在 `setBounds` 阶段把低高度存档夹大。
- 改版本号后增量构建不重写 `*_resources.rc`（见 build-toolchain.md）。
- **macOS 原生全屏的几何存档（v3.4.3 bug3）**：布局坐标是按退出时的 canvas 存的，几何必须存同一尺寸 —— `saveMainWindowBounds()` 在全屏态直接把当前（全屏）矩形写进 `window.*`（旧实现跳过写入、保留上一次的小窗口尺寸 → 重启后模块全挤在上方）。同时 `restoreMainWindowBounds()` 在 `window.nativeFullScreen=1` 时**跳过**旧几何恢复（本次启动会进全屏，小窗口只会闪一下）。

## 6.1 布局预设与全屏状态（v3.4.3 bug4 / v3.5.0 场景化重构）

- v3.5.0 删除历史旧预设，重构为 **4 分组 14 套常规预设 + 4 套 Stick 吸附条**（全部采用存活模块、零重叠、完整支持 Undo/Redo）：
  - **日常与桌面**：`Default Workspace(1)` / `Compact Mini Bar(4)` / `Desktop Companion(5)` / `Top Dock Bar(2)` / `Bottom Dock Bar(3)`；
  - **混音与音频分析**：`Mixing Console(6)` / `Mastering & Broadcast(7)` / `Vocal & Podcast(8)` / `DJ & Beat Analysis(9)`；
  - **视觉与直播**：`Stream / OBS Overlay(10)` / `Music Visualizer (MV)(11)` / `Pure MilkDrop(12)`；
  - **全屏指挥台**：`Full: Deep Analysis(13)` / `Full: All-in-One(14)`（17 块无缝密排覆盖全部 16 类功能模块）；
  - **Stick 吸附条（仅驻留态）**：`Bar: Classic(2/3)` / `Bar: Mixing(15)` / `Bar: Visuals(16)` / `Bar: Desk Companion(17)`。
- 全屏预设（`musicVisualizerMv(11)` / `onlyMilkdrop(12)` / `fullDeepAnalysis(13)` / `fullAllInOne(14)`）在 macOS 会 `setFullScreen(true)` 并把 Editor / 顶层窗口的 `resizeLimits` 锁死到整屏尺寸。
- 因此 `applyLayoutPreset()` 做了两件事：①目标不是全屏预设且当前处于原生全屏 → 先 `setFullScreen(false)`，轮询到真的离开全屏（`continueLayoutPresetAfterFullScreenExit`，80ms × 40）再重新应用；②应用前统一放开 Editor / 窗口的 resizeLimits，各预设分支随后按自己的目标尺寸重设。
- 漏掉①的表现：全屏预设切到默认 / 横条预设后窗口仍铺满整屏，只有模块被换掉。
- 新增任何"直接改窗口尺寸"的入口，都要同样先放开 resizeLimits。

## 6.2 浮动模块窗口的脱离 / 停靠 / 关闭时序（v3.5.0）

- 浮动窗口（`FloatingModuleWindow`）现在运行独立 `OpenGLContext` + `GlDirectRenderer`（详见 `rendering-gl.md` §1）。
- 点击浮动窗口标题栏的「停靠（Pop-out）」或「关闭（×）」按钮时，**不得在 `WM_LBUTTONUP` / `mouseUp` 栈帧内同步销毁窗口或原位覆写 `panel->onPopOutClicked`**：必须先把回调按值拷贝到局部栈变量，再经 `MessageManager::callAsync` + `SafePointer` 统一路由到 `dockFloatingModule()` / `closeFloatingModule()`，停靠前必须 `clearContentComponent()` 解除归属，且支持双击幂等去重。

## 7. 相关引用

- 全屏/布局预设语义（MV、Studio Monitor、FULL 磁贴预设等）见 `PROJECT_OVERVIEW.md` §4.4。
- 双击标题栏全屏按平台拆分（macOS 原生全屏 / Windows 伪最大化）见 `PROJECT_OVERVIEW.md` §4.3。
