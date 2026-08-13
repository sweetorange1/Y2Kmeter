# Y2Kmeter v2.6.0 开发总结

> 本文档记录 v2.6.0 版本基于 Git 修订号 `e4bc0b78b47adb92b6b7244edc5c3da0835ce68d`
> 之后引入问题的前次修复遗留缺陷，以及本轮（第二轮）针对遗留缺陷的根因分析与修复方案。

---

## 1. 背景：前次修复的遗留缺陷

前次（第一轮）修复针对 `e4bc0b78` 之后引入的两个问题做了如下调整，但均存在遗留缺陷：

| 问题 | 第一轮修复内容 | 遗留缺陷 |
|:---:|---|---|
| Bug 1：脱离 Milkdrop 后存档读取卡 Idle | 移除构造函数主线程预扫描预设；`newOpenGLContextCreated()` 恢复无条件 `ScanPresetFiles()` | 仅修复了“预设列表可能为空”的时序问题，但**未触及真正的根因**（Windows 端强制 Core Profile 与 Editor/GLView projectM handle 时序竞争），因此现象依旧 |
| Bug 2：脱离后预设控制区挤压视频区 | `layoutContent()` 移除预留空间逻辑；新增 `GLView::paint()` 覆盖绘制控制栏 | 控制栏确实不再挤压视频区，但控制栏被移到 `GLView::paint` 后，交互代码的 `repaint()` 未触发 `GLView` 重绘，引入新的交互回归 |

本轮（第二轮）针对上述两个遗留缺陷做进一步定位与修复。

---

## 2. 问题 1：脱离 Milkdrop 模块后重新打开软件仍卡在 Idle 动画

### 2.1 现象

将 Milkdrop 模块脱离（floating）后退出软件，重新打开软件并读取存档，模块持续卡在初始 Idle 动画（带耳机的“M”标志旋转），无法自动进入正常预设动画。手动切换一次预设后动画恢复正常。

### 2.2 根因

问题由两个叠加因素导致：

**根因 A：Windows 端强制 OpenGL Core Profile**

`MilkdropModule::GLView` 构造函数中无条件调用：

```cpp
open_gl_context_.setOpenGLVersionRequired(juce::OpenGLContext::openGL3_2);
```

该调用最初为 macOS 引入（macOS 默认 Legacy Profile 会导致 projectM shader 编译失败），但未做平台区分。Windows 端被强制改为 Core Profile 后，部分预设 shader 编译失败，projectM 回退到 Idle 动画。手动切换预设时 GL 状态已稳定、shader 重新编译成功，因此表现为“手动切一次即恢复”。

**根因 B：Editor 与 GLView 的 projectM handle 时序竞争**

存档恢复时，`openGLContext.attachTo(*this)` 异步创建 Editor projectM handle，与 `loadInitialModules()` 同步恢复 floating 模块（触发 `SuspendMilkdropEditorRendererForFloating`）可能交错。若 Editor handle 尚未创建时 Suspend 早退（`isAttached()` 为 false 或 `milkdrop_pm_handle_ == nullptr` 跳过销毁），随后 Editor handle 又在 GLView handle 之后异步创建，两者共存导致 Windows libprojectM/GLEW 全局指针表互相干扰。

### 2.3 修复方案

1. `setOpenGLVersionRequired(openGL3_2)` 仅在 macOS 强制（`#if JUCE_MAC`），Windows 恢复默认 GL 版本。
2. 新增 `milkdrop_renderer_suspended_` 挂起标志，在脱离态期间阻止 Editor handle 被异步创建：

   - `SuspendMilkdropEditorRendererForFloating()` 开头置 `true`（即便 Editor 上下文尚未 attach 也先置位）；
   - `ResumeMilkdropEditorRendererAfterFloating()` 开头置 `false`；
   - `Y2KmeterAudioProcessorEditor::newOpenGLContextCreated()` 开头检查该标志，挂起期间直接 `return`。

---

## 3. 问题 2：脱离后预设控制台行为异常

### 3.1 现象

Milkdrop 模块脱离后：

1. 预设控制台不会自动隐藏；
2. 点击控制按钮无按下动画反馈；
3. 点击 “auto” 按钮未展开时间控制条。

### 3.2 根因

第一轮为了修“控制栏覆盖不挤压视频区”，把控制栏绘制从 `MilkdropModule::paintContent` 移到 `GLView::paint`（依赖 `setComponentPaintingEnabled(true)` 合成到 projectM GL 帧之上）。但交互代码中的 `repaint(...)` 调用的是 `MilkdropModule::repaint`，**并不会触发子组件 `GLView` 的 `paintComponent` 重绘**，导致：

- 自动隐藏（`checkOverlayAutoHide → setFocusVisual(false)`）后控制栏不消失；
- 按钮按下/悬停状态无反馈；
- `auto` 行展开状态不刷新。

### 3.3 修复方案

在所有会改变控制栏视觉状态的交互路径上补充 `glView->repaint()`，让 `GLView::paint` 被重新调度：

- `GLView::timerCallback()`（30Hz 轮询，驱动自动隐藏与 auto 展开刷新）；
- `setFocusVisual()`（聚焦/失焦即时刷新）；
- `mouseDown / mouseUp / mouseMove / mouseDrag / mouseExit`（按钮按下、悬停、拖拽反馈）；
- `toggleAutoMode()`、`applyAutoInterval()`（auto 行展开与间隔更新）。

---

## 4. 涉及文件与具体代码变更点

### 4.1 `source/ui/modules/MilkdropModule.cpp`

- `GLView::GLView()` 构造函数：`setOpenGLVersionRequired(openGL3_2)` 由无条件调用改为 `#if JUCE_MAC` 包裹，Windows 端恢复默认 GL 版本。
- `GLView::timerCallback()`：在 `owner_.repaint()` 后追加 `repaint()`（触发 GLView 自身重绘）。
- `MilkdropModule::setFocusVisual()`：末尾追加 `glView->repaint()`。
- `MilkdropModule::mouseDown / mouseUp / mouseMove / mouseDrag / mouseExit`：在 `repaint()` 后追加 `glView->repaint()`。
- `MilkdropModule::toggleAutoMode()`、`applyAutoInterval()`：`repaint()` 后追加 `glView->repaint()`。

### 4.2 `source/ui/modules/MilkdropModule.h`

- `GLView` 新增 `void paint(juce::Graphics& g) override;` 声明（第一轮引入，本轮保留）。

### 4.3 `PluginEditor.cpp`

- `Y2KmeterAudioProcessorEditor::newOpenGLContextCreated()`：开头新增 `milkdrop_renderer_suspended_` 检查，挂起期间直接 `return`。
- `SuspendMilkdropEditorRendererForFloating()`：开头置 `milkdrop_renderer_suspended_ = true`。
- `ResumeMilkdropEditorRendererAfterFloating()`：开头置 `milkdrop_renderer_suspended_ = false`。

### 4.4 `PluginEditor.h`

- 新增成员 `std::atomic<bool> milkdrop_renderer_suspended_{ false };`。

### 4.5 版本号更新（v2.5.x → v2.6.0）

- `CMakeLists.txt`：`project(Y2Kmeter VERSION 2.6.0 ...)`、`juce_add_plugin(... VERSION 2.6.0 ...)`。
- `Y2Kmeter_installer.iss`：`#define MyAppVersion "2.6.0"`。
- `PluginEditor.cpp`：关于页面标题/抬头 4 处 `"v2.5.6"` → `"v2.6.0"`。
- `PROJECT_OVERVIEW.md`：当前版本标识 `2.5.7` → `2.6.0`。
- `MACOS_ADAPTATION_DIFFS.md`：当前打包版本标识 `v2.5.6` → `v2.6.0`。

---

## 5. 编译建议

本轮改动涉及两个头文件：

- `source/ui/modules/MilkdropModule.h`（`GLView` 新增虚函数 `paint`，影响虚表布局）；
- `PluginEditor.h`（新增 `milkdrop_renderer_suspended_` 成员）。

按项目约定，修改 `.h` 后**必须执行全量验证构建**（不能只做增量构建）。建议使用 `project-maintenance-guard` 的独立构建脚本（`build/skill-verify/`，与 CLion 构建目录隔离）进行完整构建验证。
