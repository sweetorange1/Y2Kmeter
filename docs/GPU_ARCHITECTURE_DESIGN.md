# Y2Kmeter GPU 渲染架构设计

> 版本：v2.3 | 日期：2026-07-27
> 状态：Phase 1 (Milkdrop) 已完成

---

## 1. 架构概览

```
┌──────────────────────────────────────────────────────────────────┐
│                    第一层：CPU 音频分析                           │
│                                                                  │
│  AudioInput → AnalyserHub (FFT/Oscilloscope/Loudness/Dynamics)   │
│                     │                                            │
│                     ▼                                            │
│  FrameSnapshot（atomic shared_ptr swap，30/60Hz）                │
│    · spectrumMag[1024], spectrumData[160], spectrumMagLo[4096]   │
│    · oscL[2048], oscR[2048]                                      │
│    · loudness, phase, dynamics snapshots                         │
│    · tickCount（单调递增帧序号）                                  │
└──────────────────────────────────────────────────────────────────┘
                          │
                          ▼  FrameListener::onFrame() 分发
┌──────────────────────────────────────────────────────────────────┐
│               第二层：Editor GL 合成管线                          │
│                                                                  │
│  Editor::renderOpenGL()                                          │
│  （Editor 唯一 OpenGL 4.1 上下文，CPE=true，CachedImage FBO）    │
│    │                                                             │
│    ├─ 1) projectM → Offscreen FBO（openglRenderFrameFbo）       │
│    │       · fbo_w/fbo_h = max_module_size × dpi / scale        │
│    │       · 单例 FBO，所有模块共享同一帧 projectM 输出          │
│    │                                                             │
│    ├─ 2) Offscreen FBO → FBO 0，blit 到各模块 correct position  │
│    │       · 跨 FBO blit，无同 FBO 重叠的未定义行为              │
│    │       · src: offscreen FBO 的 (0,0,s_w,s_h)                │
│    │       · dst: 各模块在 FBO 0 上的 dest rect（左下角原点）    │
│    │       · GL_LINEAR 实现降采样→上采样模糊效果                 │
│    │                                                             │
│    └─ 3) JUCE CPE 叠加 UI 组件（标题栏、按钮、其他模块）         │
│                                                                  │
│  ★ 永远不 clear FBO 0（CachedImage 合成面）                      │
│    clear 会破坏 JUCE 已合成或即将合成的所有组件内容               │
└──────────────────────────────────────────────────────────────────┘
```

### 关键设计原则

1. **Editor 拥有唯一 GL 上下文**：实现 `juce::OpenGLRenderer`，所有 GPU 模块在 `renderOpenGL()` 中渲染。
2. **Offscreen FBO 隔离 projectM**：projectM 内部 `glBindFramebuffer(0)` 无法阻止，但可通过 `openglRenderFrameFbo(fbo_id)` 让 projectM 渲染到独立 FBO。
3. **零 CPU 像素回读**：不再有 `glReadPixels`、PBO、`juce::Image` 像素转换、`g.drawImage`。
4. **内容区透明绘制**：`MilkdropModule::paint()` 使用 `transparentBlack` 填充，保留 projectM 帧。
5. **坐标系统**：`getLocalPoint(milk, point)` 纯组件树遍历 → `openGLContext.getRenderingScale()` DPI 缩放 → Y-flip 物理像素。

---

## 2. Milkdrop 最终实现（v2.3.0）

### 2.1 Editor 实现 OpenGLRenderer

```cpp
// PluginEditor.h
class Y2KmeterAudioProcessorEditor
  : public juce::AudioProcessorEditor
  , public juce::OpenGLRenderer       // ← 关键
  , private juce::Timer
{
  // projectM 状态（由 Editor GL 上下文持有）
  projectm_handle milkdrop_pm_handle_;

  // Offscreen FBO（projectM 渲染目标）
  GLuint milkdrop_render_fbo_   = 0;
  GLuint milkdrop_render_tex_   = 0;
  int    milkdrop_render_scale_ = 1;  // 1=全分辨率, 2=半, 4=1/4
  int    milkdrop_last_fbo_w_   = 0;  // FBO 尺寸变化检测
  int    milkdrop_last_fbo_h_   = 0;

  // PCM 缓冲（UI 线程写 → GL 线程读）
  std::mutex          milkdrop_pcm_mutex_;
  std::vector<float>  milkdrop_pending_pcm_;

public:
  // OpenGLRenderer
  void newOpenGLContextCreated() override;  // GLEW→projectm_create→FBO 创建
  void renderOpenGL() override;            // 主渲染管线
  void openGLContextClosing() override;    // projectm_destroy→FBO 销毁

  // Bridge APIs（供 MilkdropModule::GLView 在 UI 线程调用）
  void PushMilkdropPcm(...);
  void RequestMilkdropPresetDelta(int delta);
  void RequestMilkdropRenderScale();       // 循环 1→2→4→1
  int  GetMilkdropRenderScale() const noexcept;
};
```

### 2.2 渲染管线（最终方案）

```cpp
// Editor::renderOpenGL() 核心逻辑

// Step 0: 收集最大模块尺寸 → FBO 大小
int max_w = 256, max_h = 256;
// 递归遍历 measureMax...

// 缩放后的 FBO 尺寸（降采样时缩小）
int fbo_w = max_w * dpi / scale;
int fbo_h = max_h * dpi / scale;

// Step 1: 分配/重分配 offscreen FBO（首帧或尺寸变化时）
if (need_realloc) {
  glBindFramebuffer(GL_FRAMEBUFFER, milkdrop_render_fbo_);
  glBindTexture(GL_TEXTURE_2D, milkdrop_render_tex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbo_w, fbo_h, ...);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ...);
}

// Step 2: 渲染 projectM
if (api.hasOpenglRenderFrameFbo()) {
  // 主路径：projectM → offscreen FBO
  glBindFramebuffer(GL_FRAMEBUFFER, milkdrop_render_fbo_);
  glViewport(0, 0, fbo_w, fbo_h);
  projectm_opengl_render_frame_fbo(pmHandle, milkdrop_render_fbo_);
} else {
  // 降级路径：projectM → FBO 0 (0,0)
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  projectm_opengl_render_frame(pmHandle);
  // FBO 0 → offscreen FBO（跨 FBO 复制）
  glBlitFramebuffer(0, 0, fbo_w, fbo_h, 0, 0, fbo_w, fbo_h,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

// Step 3: 跨 FBO blit：offscreen FBO → FBO 0 各模块位置
glBindFramebuffer(GL_READ_FRAMEBUFFER, milkdrop_render_fbo_);
glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

for each MilkdropModule:
  // 坐标：getLocalPoint + dpi × scale + Y-flip
  glBlitFramebuffer(0, 0, src_w, src_h,
                    dest_x, dest_y, dest_x + dest_w, dest_y + dest_h,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);

// ★ 永远不 clear FBO 0（保护 JUCE UI）
```

### 2.3 坐标系统（最终版）

```cpp
// 关键：使用 getLocalPoint(milk, point) 而非 localAreaToGlobal/getLocalArea
// getLocalPoint 走纯 JUCE 组件树父子关系遍历，不经手 native peer，与 CPE FBO 原点一致

float dpi_scale = static_cast<float>(openGLContext.getRenderingScale());
int   editor_h  = getHeight();  // 逻辑高度

for each milk:
  juce::Rectangle<int> content_local = milk->GetContentLocalBounds();
  juce::Point<int> tl = getLocalPoint(milk, content_local.getPosition());
  juce::Point<int> br = getLocalPoint(milk, content_local.getBottomRight());

  int logic_x = tl.x, logic_y = tl.y;
  int logic_w = br.x - tl.x, logic_h = br.y - tl.y;

  // 逻辑坐标 → 物理像素（FBO 0，原点在左下角）
  int gl_x = static_cast<int>(logic_x * dpi_scale);
  int gl_y = static_cast<int>((editor_h - (logic_y + logic_h)) * dpi_scale);
  int gl_w = static_cast<int>(logic_w * dpi_scale);
  int gl_h = static_cast<int>(logic_h * dpi_scale);

  // 降采样坐标（projectM 内部渲染尺寸）
  int src_w = gl_w / milkdrop_render_scale_;
  int src_h = gl_h / milkdrop_render_scale_;
```

### 2.4 GLView 降级为纯 CPU 组件

```
改造前（PBO 回读方案，已废弃）:
  GLView : juce::Component + juce::OpenGLRenderer
    ├─ juce::OpenGLContext glContext (CPE=false, 独立 HWND)
    ├─ projectm_handle pmHandle
    ├─ PBO[2] + downscale FBO  (降采样回读)
    ├─ FrameSlot[3]             (三缓冲图像队列)
    └─ ReadbackScale            (1:1 / 1:2 / 1:4)

改造后:
  GLView : juce::Component + juce::Timer
    ├─ startTimerHz(30)         (驱动 Overlay 刷新)
    ├─ PushPcm() → Editor::PushMilkdropPcm()
    ├─ RequestPresetDelta() → Editor::RequestMilkdropPresetDelta()
    ├─ RequestRenderScale() → Editor::RequestMilkdropRenderScale()
    └─ IsRenderReady() → Editor::IsMilkdropRenderReady()
```

### 2.5 数据流

```
  音频线程                    UI 线程                       GL 线程
  ────────                    ────────                      ────────
   PCM 帧                     AnalyserHub
     ↓                          ↓
  AnalyserHub              FrameListener
     ↓                    MilkdropModule::onFrame()
  FrameSnapshot              ├─ 交错 LRLR
     ↓                       └─ glView->PushPcm(tmp, N)
  hub.dispatch()                   ↓
                          Editor::PushMilkdropPcm()
                            └─ mutex lock
                               milkdrop_pending_pcm_ = ...
                                                            Editor::renderOpenGL()
                                                              ├─ consume PCM (mutex)
                                                              ├─ consume preset requests
                                                              ├─ projectm_set_window_size
                                                              ├─ projectM → offscreen FBO
                                                              ├─ blit offscreen FBO → FBO 0
                                                              └─ (JUCE CPE 合成 UI)
```

---

## 3. 渲染分辨率控制

### 按钮位置

Overlay 控制栏布局（从左到右）：
`[<]  PresetName  [1:n]  [auto]  [>]  [?]`

### 实现

```
milkdrop_render_scale_ 循环: 1 → 2 → 4 → 1

比例  | projectM 纹理大小（相对值）| 效果          | 性能
1:1   | 100%                      | 原始画质      | 1×
1:2   | 50%                       | 轻微模糊      | ~4×  (1/4 像素)
1:4   | 25%                       | 明显模糊/马赛克 | ~16× (1/16 像素)
```

降采样渲染 → `glBlitFramebuffer(GL_LINEAR)` 自动双线性上采样填满模块区域。

### 代码链路

```
MilkdropModule Overlay [1:n] 按钮
  → executeOverlayAction(kRenderScale)
    → glView->RequestRenderScale()
      → findParentComponentOfClass<Editor>()
        → Editor::RequestMilkdropRenderScale()
          → milkdrop_render_scale_ 循环 1→2→4→1
```

---

## 4. 修改文件清单

| 文件 | 变化说明 |
|------|----------|
| **PluginEditor.h** | +60 行：`juce::OpenGLRenderer` 继承 + projectM 管理成员（handle + offscreen FBO + scale） + 桥接 API 声明 |
| **PluginEditor.cpp** | +250 行/-80 行：`newOpenGLContextCreated`（GLEW+projectM+FBO 创建）、`renderOpenGL`（FBO+blit 管线）、`openGLContextClosing`（FBO 销毁）、桥接方法、版本号更新 |
| **MilkdropModule.h** | -150 行：删除 `OpenGLRenderer`/`OpenGLContext`/`ReadbackScale`/`FrameSlot`/PBO 成员；GLView 降级为纯 Timer；新增 `kRenderScale` 枚举值 + `kResBtnW` + `RequestRenderScale` |
| **MilkdropModule.cpp** | -620 行/+100 行：删除 PBO 渲染管线；新增 `paint()` 跳过内容区；PCM/Preset/RenderScale 桥接到 Editor；`[1:n]` 按钮 UI 交互完整实现 |

---

## 5. 各模块 GPU 化方案（规划）

### 5.1 Milkdrop（moduleTypeId=19）—— ✅ Phase 1 已完成

### 5.2 Spectrogram3D（moduleTypeId=17）—— 优先级 P0

Fragment Shader 一次计算所有像素。详见原文档 Phase 2。

### 5.3 其余模块（Oscilloscope/Spectrum/Waveform 等）

参见原文档 Phase 3-7。

---

## 6. 性能数据

| 场景 | 改造前（PBO） | 改造后（Editor GL） |
|------|:---:|:---:|
| Milkdrop paintContent | ~19.8ms (PBO回读+drawImage) | <0.5ms (只画叠加控件) |
| GPU→CPU DMA | 0.48MB/帧 (PBO) | 0 |
| projectM 帧路径 | FBO→PBO→CPU→Image→GPU | Offscreen FBO→glBlitFramebuffer→FBO 0 |
| 降采样性能 | 独立 PBO 尺寸缩小 | 共享 offscreen FBO，自然获得性能提升 |

---

## 7. 踩坑记录

### 坑1：openglRenderFrame 内部强制 glBindFramebuffer(0)

**症状**：尝试将 projectM 渲染到自定义 offscreen FBO，但 `glBlitFramebuffer` 读到的是空内容。

**根因**：`projectm_opengl_render_frame()` 内部固定调用 `glBindFramebuffer(0)`，无论调用前绑定了什么 FBO 都会被重置。因此基于"projectM 渲染到自定义 FBO"的所有方案全部失效。

**修复**：使用 `projectm_opengl_render_frame_fbo(handle, fbo_id)` API。该 API 接受 FBO ID 参数，projectM 渲染到指定 FBO。降级路径（DLL 不提供该符号）：projectM 渲染到 FBO 0 (0,0) → 跨 FBO blit 搬运到 offscreen FBO → 再 blit 到各模块位置。

### 坑2：同 FBO 上 glBlitFramebuffer 源/目重叠 = 未定义行为

**症状**：当模块显示区与 FBO 0 的 (0,0) 区域重叠时，视频内容留在 (0,0) 而非模块位置。表现为"左下角固定有视频，模块移上去才能看到"。

**根因**：`glBlitFramebuffer` 在同一 FBO 上读写重叠区域是 OpenGL 规范禁止的未定义行为。必须跳过重叠模块的 blit，但跳过又导致内容滞留。

**修复**：始终使用跨 FBO blit（READ=offscreen FBO, DRAW=FBO 0），根上杜绝重叠问题。此方案不需要任何重叠检测。

### 坑3：glClear 在 FBO 0 (CachedImage) 上破坏 JUCE UI

**症状**：屏幕左下角出现黑色矩形（大小 = projectM 渲染窗口）、视频重影、帧撕裂。

**根因**：`renderOpenGL()` 运行在 FBO 0 上（JUCE 的 CachedImage 合成面）。在 FBO 0 上调用 `glClear` 会不可逆地擦除 JUCE 已合成或即将合成的所有组件内容——不只是 projectM 的输出，还有工具栏、模块边框、标题栏等。

**修复**：★ 永远不 clear FBO 0 ★。projectM 的 (0,0) 残留无需清理，因为该区域会被模块 blit 或 JUCE UI 覆盖。

### 坑4：getScreenPosition / localAreaToGlobal 坐标不可靠

**症状**：多次修正坐标计算后视频仍不跟随模块移动。

**根因**：
- `getScreenPosition()` 返回 OS 屏幕坐标，受窗口位置、标题栏、DPI 虚拟化影响
- `localAreaToGlobal()` → `getLocalPoint(nullptr, ...)` 走 native peer 坐标，多 DPI 下 peer 转换不是精确互逆

**修复**：使用 `getLocalPoint(milk, point)`，这是 JUCE 最底层的组件树纯父子遍历，不经手任何 native peer，与 CPE FBO 原点完全一致。

### 坑5：projectM 小尺寸渲染的固有偏差

对于非常小的 `setWindowSize(w, h)`（例如 <250×250 像素），projectM 的 mesh（128×80 网格）和预设 shader 的运算精度导致渲染结果在视觉上略有偏移。这不是坐标系统错误，而是 projectM 引擎的固有特性。可通过增加 mesh size 或对极小模块使用 1:1 scale 缓解。

---

## 8. 实施计划

### Phase 1：Milkdrop 零拷贝 ✅ 已完成（v2.3.0）

### Phase 2：Spectrogram3D GPU Shader

| # | 任务 | 文件 |
|---|------|------|
| 2.1 | 编写片段着色器 | 嵌入代码或资源文件 |
| 2.2 | Spectrogram3DModule 实现 GPU 路径 | FineSplitModules.cpp |

### Phase 3-7：Oscilloscope / Spectrum / Waveform / 其余模块

参见原规划。

---

## 9. 实施约定

1. **每完成一个 Phase，在 Intel 平台跑一次性能测试**，确认帧率改善。
2. **每 Phase 独立可编译、可运行**。
3. **不自动升级版本号**，待各 Phase 确认稳定后再统一升级。
4. **所有 GPU 运算保持在 Editor 的单一 GL 上下文中**，不引入额外上下文。