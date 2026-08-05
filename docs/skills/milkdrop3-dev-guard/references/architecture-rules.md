# Architecture Rules (6 铁律细节)

> 本文档为 [SKILL.md](../SKILL.md) §4.2 的详细版。SKILL.md 主要用于快速触发；这里承载真实源码引用、失败案例与决策依据。

---

## 铁律 1：音频数据流是「Hub → 快照 → Injector → 引擎」的四段单向链

### 数据流图

```
AnalyserHub                              (音频线程 push)
    │
    ▼   onFrame(FrameSnapshot&)          UI 线程回调
Milkdrop3Module::onFrame()
    │                                    加锁写入
    ▼
AudioSnapshot { interleaved[], specL/specR[], sample_rate, has_pcm, has_spectrum }
    │
    ▼   PreRenderInjector                每帧 RenderFrame 开头执行
Milkdrop3Api::FeedPcm + FeedSpectrum
    │
    ▼
CPluginShell::m_sound.fWaveform / m_sound.fSpectrum
    │  (m_bY2kExternalSpectrumValid=true 时 AnalyzeNewSound 短路内建 FFT)
    ▼
CPlugin::RenderFrame → D3D9 Present
```

### 关键约束

- **必须**：`Milkdrop3Module` 构造函数末尾调用 `hub_->addFrameListener(this)`；析构第 3 步 `removeFrameListener`。
- **必须**：任何新数据源都从 `AudioSnapshot` 结构追加字段 → `onFrame` 写入 → 通过 injector 投喂。
- **禁止**：绕过 injector 直接在 `Milkdrop3Api::RenderFrame` 开头塞新逻辑；这会让 injector 机制失效。

### 历史事故（附录 D.4）

v20 之前 14 轮版本中 `Milkdrop3Module` 未注册 `FrameListener`，导致引擎 `m_sound` 长期为零向量。表现是"MilkDrop 在跑，但视觉效果与音频完全脱钩"。v20 补一行 `addFrameListener` 后音频才真正接入。

---

## 铁律 2：坐标空间必须显式 logicalToPhysical

### 前提

进程在 `PluginProcessor` / `PluginEditor` 阶段调用 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`。之后所有 Win32 API 期望**物理像素**。

### 规则

**必须**：
```cpp
auto phys = juce::Desktop::getInstance().getDisplays()
              .logicalToPhysical(juce::Rectangle<int>{ logicalX, logicalY, logicalW, logicalH });
CreateWindowExW(..., phys.getX(), phys.getY(), phys.getWidth(), phys.getHeight(), ...);
```
反向读回 `GetWindowRect` 用作 JUCE 坐标时用 `physicalToLogical`。

**禁止**：
- 直接把 `getScreenPosition()` / `localPointToGlobal()` 交给 `CreateWindowExW`。
- 用 `GetDpiForWindow(parent_hwnd) * logical` 手工换算（跨屏时错位）。
- 引入调试性 DPI 探测代码（只写日志、不参与计算的 `MonitorFromPoint + GetDpiForMonitor`）。

### 历史事故

v19 之前偏移随离原点距离增大——三屏场景下右侧屏偏移最大、左侧最小。根因是把 JUCE 逻辑 DIP 直接喂给 `CreateWindowExW`。修复文件：`Milkdrop3Module.cpp::D3dChildWindow::CreateHWNDOnly` / `Reposition` / `timerCallback phase=-1`。

---

## 铁律 3：D3D9 popup 永远在 JUCE 之上

D3D9 popup（`WS_POPUP` owned by root）是原生 HWND，Windows 窗口 Z 序中**恒定位于 JUCE 任意 `Graphics` 绘制之上**。

### 规则

**必须**：任何需要**可见**的 JUCE 内容（按钮、文本、面板、弹窗）都要位于 popup 上方的 JUCE 独占区——在 `layoutContent` 中把 popup 的 y 或 height 显式让出对应偏移，再在让出区内绘制 JUCE 元素。参考 `Milkdrop3Module.cpp::layoutContent` 的 `effective_offset` 三态计算。

**禁止**：
- 用 `paintOverChildren` / `Component::toFront` 试图把 JUCE 绘制"盖"到 popup 上——永远不可见。
- 在被 popup 覆盖的区域添加 `TextEditor` / `TextButton` / 子组件——即使 Component 逻辑上位于其上，Windows 也会用原生 HWND 遮挡。

### 唯一例外

引擎自绘 overlay：`m_bShowPresetInfo` 每帧右上角写预设名；`LaunchSongTitleAnim()` 触发中央大字动画。这是 D3D9 surface 内部绘制，不受 z-order 限制。

---

## 铁律 4：线程与生命周期——重型堆分配不能与 GL 渲染线程并发

### 前提

Standalone 与 VST3 Editor 都会在某个时刻调用 `openGLContext.attachTo(*this)` 启动 GL 渲染线程。GL 线程与主线程共享 CRT 堆。一旦二者同时做重型堆分配（字体加载、GDI 位图、TextEditor 组件树），会在 heap lock 上死锁。

### 规则

**必须**：
- `Milkdrop3Module` 构造函数**轻量**——除 `retain / addFrameListener` 外不做任何 D3D9 / TextEditor / ComboBox / GDI 字体等重型堆分配。
- 需要重型堆分配的初始化通过 `startTimer(5)` 或 `MessageManager::callAsync` 延迟到下一个消息迭代。
- 所有 `TextEditor` / `ComboBox` 用 `std::unique_ptr` 懒创建，不在构造期实例化。
- `Editor` 构造函数**末尾不得**同步执行 `openGLContext.attachTo(*this)`；改成 `attachOpenGLContext()` + `callAsync` 延迟启动。
- `Standalone` 模式下 `handleAudioSourceChanged` 中的重建组件树要在 `attachTo` 之前完成。

**禁止**：
- 在 `getDefaultSizeForType()` / `getHoverPreviewImage()` 里 `factory(milkdrop3)` 或 `new Milkdrop3Module`（附录 E.1 轮次 5）。
- 在 `Editor` 构造末尾同步 `openGLContext.attachTo(*this)`（附录 E.3）。

---

## 铁律 5：析构顺序不可逆转

`~Milkdrop3Module` 必须严格按下列顺序：

```cpp
Milkdrop3Module::~Milkdrop3Module() {
  // 1) 停止投喂，避免使用已释放的 snapshot
  if (injector_token_ != 0) {
    api_.RemovePreRenderInjector(injector_token_);
    injector_token_ = 0;
  }
  // 2) 停止 timer
  stopTimer();
  // 3) 停止 onFrame 回调 —— 必须早于 4/5
  if (hub_ != nullptr) hub_->removeFrameListener(this);
  // 4) 销毁 D3D9 popup HWND
  d3d_window_.reset();
  // 5) 销毁 D3D9 Device + CPlugin
  api_.Destroy();
  // 6-7) 释放 hub 引用计数
  if (hub_ != nullptr) {
    hub_->release(Kind::Spectrum);
    hub_->release(Kind::Oscilloscope);
  }
}
```

**禁止**：颠倒 3↔4↔5。若先 `Destroy()` 再 `removeFrameListener`，`onFrame` 可能在 UI 线程调用一个已析构的 API。

---

## 铁律 6：Standalone 启动序列（Y2KStandaloneApp::initialise）

完整详情已拆到 [init-sequence.md](init-sequence.md)。这里仅给一行铁律与决策参考：

> **启动序列必须为**：`SetProcessDpiAwarenessContext(PMv2)` → `pluginHolder` → `主题/静音恢复` → `reloadPluginState` → `mainWindow = make_unique(...)`（**不 addToDesktop**）→ `createEditor + setContentNonOwned + populateAudioSources` → `restoreBounds` → 函数末尾 `mainWindow->addToDesktop(); mainWindow->setVisible(true);` 紧挨。

任何在 initialise 中插入 `mainWindow 提前 addToDesktop` / `setVisible(false)+setVisible(true)` 分裂 / `TimerThreadBoot` / `SetThreadDpiAwarenessContext` 都会引发 `<unknown> 0x00007ffc*d9f8`（`ntdll!LdrLockLoaderLock`）死锁。

---

## 常见需求 → 正确落地方式

### A. 接入一种新的音频分析数据

1. `AudioSnapshot` 追加字段 + `has_xxx` 布尔；
2. `onFrame` 加锁写入；
3. 已有 injector 内新增分支（推荐）或 `AddPreRenderInjector` 注册新闭包；
4. 若引擎要消费，走 `pluginshell.h` public 区新增 `m_y2k*` 字段 + `AnalyzeNewSound` 内 consume-once。

### B. 加一个新按钮

1. 扩展 `Milkdrop3Module::GetControlBarBtnRect` / `HeaderButton` 枚举；
2. `PaintControlBar` 追加绘制；
3. `HitTestControlBarBtn` 追加命中区；
4. `ExecuteOverlayAction` 追加分支；
5. 位于 popup 上方 JUCE 独占区。

### C. 改预设名 / OSD

**必须**：走引擎路径 `m_bShowPresetInfo` + `LaunchSongTitleAnim`。
**禁止**：JUCE `Graphics::drawText` 在 popup 区绘制字幕。

### D. 改模块尺寸 / 预览

修改 `ModuleWorkspace::getDefaultSizeForType` 的**尺寸表**即可。**禁止**改为 factory 构造实例来获取尺寸（铁律 4）。

### E. 回滚 D3D9 window 层级

v10~v18 已把 `WS_CHILD` / owned popup / unowned popup / `HWND_TOP` / `WS_EX_TOOLWINDOW` / `SWP_FRAMECHANGED` 全部试过。当前唯一有效的组合是 owned `WS_POPUP` + PMv2 + `logicalToPhysical`。若确认要动，先与用户明示动机。
