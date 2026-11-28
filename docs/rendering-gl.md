# 渲染 / GL / OpenGL / Milkdrop / Spectrogram3D

## 1. 渲染架构现状（v3.5.0 统一嵌入 + 浮动 GL 管线）

- **Editor 级 `juce::OpenGLContext`**（`componentPaintingEnabled=true`，`attachTo(*this)`）使主窗口进入 GPU 合成管线 → 所有组件 paint 走统一 GPU → 单次 SwapBuffers → Z-order 正确、零遮盖。**OpenGLContext 必须放在类末尾**（保证反向析构时最先 detach），构造末尾 `attachTo`、析构起始 `detach`。
- **GL 直渲染模块框架 `GlDirectModuleBase` + 共享渲染器 `GlDirectRenderer`**（`source/ui/GlDirectRenderer.h`，v3.5.0 抽离）：派生类实现 4 个纯虚 `buildGlSnapshot` / `getGlCanvasBounds` / `paintGlOverlay` / `paintLegacyContent`，GL 直渲染 + CPU 回退双路径，**两条路径绘制内容与 `SkinCraft` 工艺必须一致**。`Spectrogram3DModule` 也已接入 `GlDirectModuleBase`（纹理上传 + 增量绘制）。
- **浮动态模块（pop-out 窗口）全量迁移 OpenGL（v3.5.0）**：`FloatingModuleWindow`（非 MilkDrop 的全部 16 类模块）现在自带独立的 `juce::OpenGLContext` + 独立 `GlDirectRenderer` 实例，并在 Windows 真透明模式下同步启用 DWM 硬件 Alpha（见 §1.2）。挂载成功后标记 `panel->setFloatingGlHosted(true)`：`GlDirectModuleBase::wantsGlSnapshot()` 返回 `true`（走与嵌入态完全一致的 GL 管线），同时解除 `ModulePanel` 的 30Hz 浮动态重绘节流，达成脱离/嵌入视觉样式与帧率 100% 统一。只有当浮动窗口未挂 GL 或 GL 不可用时，才退回消息线程软件光栅化并保持 30Hz 限流 + `repaintHover` 补齐定时器兜底。
- **浮动窗口 GL 生命周期、首帧防闪与异步销毁铁律（v3.5.0）**：
  - `FloatingModuleWindow` 必须通过子级容器 **`GlHostComponent`** 挂载 `OpenGLContext`（与 `Y2KMainWindow → Editor` 同构，绝不直接挂在顶层 `DocumentWindow` 自身），否则 Windows `WM_PAINT` 会绕过 `CachedComponentImage` 触发软件 GDI `paintLegacyContent()` 造成重绘闪屏。
  - Windows 弹出浮动窗口前启用 **`DWMWA_CLOAK`** + `CloakRevealTimer` 遮罩，直至首帧 GL `SwapBuffers` 完成且子 GL HWND 可见后才解除遮罩，消除脱离瞬间白/黑闪；停靠/关闭过渡期由 `glPreserveActiveOnClose_` 保持子模块 GL 状态位不提前回退 CPU 路径，Z-order 协调统一经 `source/ui/ModuleZOrder.h`。
  - `FloatingModuleWindow::detachGlContext()` 必须带 `std::atomic<bool> glClosing_` 守卫，无条件调用 `glContext_.detach()` → `setRenderer(nullptr)` → `glDirectRenderer_.reset()`，绝不在 GL 线程可能仍活跃时提前销毁 `glDirectRenderer_` 或子组件。
  - 从浮动窗口点击「停靠 / 关闭」必须经 `MessageManager::callAsync` + `SafePointer` 推迟到 `WM_LBUTTONUP` 之后，通过 `clearContentComponent()` 卸载内容（绝不用 `setContentNonOwned(panel, false)` 覆盖），再同步释放窗口，彻底杜绝 `WM_LBUTTONUP` 同步毁窗与 `std::function` 自覆写 UAF。
- **Milkdrop**（libprojectM 4 native OpenGL）用**独立** GL context（`componentPaintingEnabled=false`），单实例互斥（`std::atomic gActiveProjectMInstances`）。

### 1.2 真透明模式与 Windows DWM 硬件 Alpha（v3.5.0）

- `BackdropMode::transparent` 与 `SkinId` 完全正交，继承当前皮肤的全部仪表工艺，仅将背景替换成透明。
- **Windows 禁止走 `WS_EX_LAYERED` 软件合成**：全屏多模块下 `UpdateLayeredWindow` 会把帧率打到 ~10fps 并导致立体声场 FBO 卡死。v3.5.0 改用 **DWM 硬件透明**：`DwmEnableBlurBehindWindow`（1×1 不可见 HRGN 触发每像素 Alpha 合成）+ vendored JUCE 补丁 **0009**（`juce_OpenGL_windows.h` 启用 `PFD_SUPPORT_COMPOSITION` 与带 Alpha 位像素格式优选）+ GL 渲染入口清 FBO 0 `glClearColor(0, 0, 0, 0)`（仅透明模式清底，非透明模式仍严禁 clear FBO 0）。主窗口与浮动窗口走同一套逻辑。

### 1.3 Windows 60Hz 帧节奏与多窗口渲染线程隔离（Patch 0010 + `VBlankPacer`）

- **根治 60Hz 屏 VSync 量化掉帧（30fps 悬崖）**：60Hz 显示器上单个 `16.67ms` 窗口只要轻微抖动就跨过下一个 VBlank 跌成 `33.33ms = 30fps`，而 144/240Hz 屏仅滑落 `4~7ms`。`source/ui/VBlankPacer.h`（纯 C++17）采用带 `1ms` 抖动容差的相位累加器，在 60Hz 屏 @ 60fps 上限下稳定 1:1 跟帧，同时保持高刷屏分频精度。
- **外部 VBlank 帧合并（JUCE 补丁 0010）**：Windows/非 macOS 下主 Editor 对 `OpenGLContext` 启用 `setExternalFramePacing(true)`，所有组件标脏与 `RequestGlFrame()` 统一合并到一次 `flushPacedRepaint()`（内置 `100ms` 看门狗防休眠/锁屏卡死），并跳过 `renderFrame()` 中对受控非连续上下文的固定 `2ms` sleep，消除冗余 mid-frame 重绘与 `MessageManager::Lock` 竞争。
- **多窗口独立 `RenderThread`**：`FloatingModuleWindow` 与 `MilkdropModule::GLView` 启用 `setUseDedicatedRenderThread(true)`（补丁 0010），避免多个窗口共享 JUCE 全局渲染线程并在主窗口 VSync `SwapBuffers` 后串行排队把整机帧率腰斩。

### 1.4 ToyBox GPU 批渲染管线（`GlToyRenderer` + `renderOpenGLPostComponentBuffer`）

- **补丁 0010 钩子**：`OpenGLRenderer::renderOpenGLPostComponentBuffer()` 在 `drawComponentBuffer()` 合成完全部组件层之后、已释放 `MessageManager::Lock` 且紧贴 `swap()` 之前执行，确保 ToyBox 视觉上恒叠在全部模块之上且对消息线程零锁阻塞。
- **数据/渲染解耦**：`ToyKit.*` / `ToyClock` 保持零 GL 头文件依赖，每帧发布无锁 POD 渲染载荷与图集更新（`source/ui/toys/ToyGlTypes.h`）；`y2ktoy::GlToyRenderer`（`source/ui/toys/GlToyRenderer.h`）在 GL 线程以单次批绘制完成全部自由 emoji / 粒子 / 光晕（shader 内做模块抽屉排除矩形裁切），4K 400 个 Party emoji 实测 CPU `0.030ms` / GPU `0.024ms`、`overlay=0.0ms`。
- **降级与诊断口径**：当 GL 上下文未挂载时自动回退至 `ToyOverlay` CPU 绘制；`ToyClock::DiagSnapshot::degradedFreeActors` 独立统计真实降级粒子数（与受 `visibleBounds` 裁剪的 `fullEmoji` 解耦），写入 `[TOY]` 诊断日志 `degraded=<n>`。

### 1.1 组件层性能规则（v3.4.3）

- **组件层每次 repaint 都会让脏区内所有组件按 z 序重画**（不是只画发起者）。一个"内容很少变"的组件只要压在每帧刷新的模块上，就会每帧整块重画。低频组件（电脑状态、计算器、悬停提示……）用 `y2k::ui::setGlSafeImageCache()`（实现见 `ModulePanel.cpp`）挂整组件位图缓存：只有自己失效的区域才重画进缓存，被连带重绘时只贴一张图。
  - 缓存固定用软件 Image、只在 GL 合成目标启用（判据 `getPreferredImageTypeForTemporaryImages()` 是否为 `OpenGLImageType`，**不要**用 `getCurrentContext() != nullptr`，见 §2.1）；浮动态窗口照原路径画。
  - 不要给每帧自绘的组件挂（等于多画一次 + 每帧上传纹理）。
- **"画一次复用"的缓存必须并入 `PinkXP::getAppearanceGeneration()`**（主题 / 皮肤 / 自定义配色变化 +1）。否则只有被 repaint 波及的区域会更新：典型症状是"模块内容在动、抬头颜色停在上一个主题"，切主题后必须删掉重加模块才恢复（v3.4.3 踩过）。`setGlSafeImageCache` 已内置该判断。
- **渐变填充不便宜**：JUCE 的 GL 渲染器每次换填充都会丢掉缓存的"渐变颜色表纹理"。本仓库补丁 0007 按颜色停靠点复用、并把纹理挂在 `OpenGLContext` 关联对象上跨帧保留 —— 新皮肤大量使用渐变时这是关键项。
- **`g.drawRect(r, 1)` 在 GL 下走 EdgeTable + 多矩形裁剪求交**（比 4 次 `fillRect` 贵一个数量级）。每帧重绘的 chrome 边框用 4 次 `fillRect`（见 `PinkXP::chromeFrame`）。
- GL 直渲染模块的 overlay 里，静态文字走 `PinkXP::drawCachedText`，动态数值才走 `drawText`。`drawCachedText` 有 `Rectangle<int>` / `Rectangle<float>` 两个重载，花括号实参 `{x,y,w,h}` 会二义，必须显式写类型。

## 2. GL Image 消费规则（macOS 崩溃高发区）

1. **`OpenGLContext::getCurrentContext() != nullptr` 只说明"有 GL 上下文"，不等于"画进 GL"**。GL 线程上的离屏软件绘制（`createComponentSnapshot`）同样满足。判据要用"绘制目标身份"，不是上下文存在性。
2. **`OpenGLImageType` 的 Image 只能喂给 GL 上下文**。跨后端消费（CG / Direct2D / 软件光栅）中只有 Windows 软件光栅恰好容错；macOS 会越界读崩溃（Windows 不炸）。
3. **`Image::BitmapData` 的 `lineStride` 可为负**（bottom-up），`data` 也不一定指向首行。把 `data` 当连续 buffer 喂第三方 API（`CFDataCreate`+`CGImageCreate`）必炸。
4. **`createComponentSnapshot` 是"软件绘制"强信号**：新增此类调用点时，模块内部离屏缓存类型必须匹配；**v3.5.0 起所有 CPU 离屏位图缓存一律显式传入 `juce::SoftwareImageType{}`，严禁省略参数走平台默认后端或创建 `juce::OpenGLImageType()`**。覆盖范围包括：桌面纹理共享池 `PinkXP::getSharedDesktopTexture`、Editor `desktopCacheImage`、进程级静态预缩 `1280px` 的 `sCachedLogo`（避免 3000×3000 原图 36MB 解码且 VST3 卸载安全）、以及全量模块与调色板离屏层（`Spectrum/Spectrogram/Spectrogram3D/Loudness/Oscilloscope/Phase/StereoField/PalettePanel`）。原因：① 避免 Windows 在未显式指定类型时隐式拉起 Direct2D/COM 工厂（防首帧卡顿与 AMD `atidxx64.dll` 卸载死锁）；② 杜绝模块在 Editor 与 `FloatingModuleWindow` 两个独立 `OpenGLContext` 间迁移时因 `OpenGLImageType` 持有裸 `OpenGLContext*` 触发悬垂指针 / `abort()` 崩溃。

## 3. Milkdrop（projectM）避坑清单

- 禁止嵌套 OpenGL 上下文（Editor GL=true 时子组件不能再附加独立 context）；"两个独立 context 不同 surface"≠嵌套。
- **Airspace Problem 无 API workaround**：GL HWND 覆盖 GDI 客户区只能靠"主窗口 GPU 合成管线"根治。
- DPI 用**物理像素**：`getWidth()` 是逻辑尺寸，`glReadPixels/glViewport` 需物理尺寸，在 `renderOpenGL` 里 `glGetIntegerv(GL_VIEWPORT)` 获取。
- projectM 内部强制 `glBindFramebuffer(0)`，**不要用自定义 FBO 限制它**。
- GLEW 函数指针绑定 HGLRC：重建 context 后必须 `FreeLibrary`+`LoadLibrary` 全量 reload。
- projectM handle 必须在 GL 线程创建/销毁；析构先同步 detach 等 GL 收尾。
- `componentPaintingEnabled(true)` 下 CachedImage 会被 ModulePanel 底色覆盖 → paintContent 用 `drawImageAt` 覆盖。
- `GLint/GLuint` 是全局作用域（khrplatform.h），**不加** `juce::gl::` 前缀。
- `glBlitFramebuffer` 格式不匹配时显卡驱动会直接丢弃该次拷贝，`glError` 仍为 0。
- `triggerRepaint()` 不可靠驱动 UI 重绘；用 `juce::Timer`（消息线程回调）才可靠。
- 不要在 `paint`/`paintContent` 内调 `repaint()`（干扰 CachedImage）。
- `hasKeyboardFocus()` 是组件级 API，判断窗口激活要用 `TopLevelWindow::getActiveTopLevelWindow()`。
- `enterModalState()` 非阻塞；恢复用 `ModalComponentManager::Callback::modalStateFinished()`。
- GLView 原生 HWND 会覆盖模态弹窗 → 弹窗前 `glView->setVisible(false)`。
- 不要在构造器 `addAndMakeVisible` 子组件（hover snapshot 构造触发断点），延迟到首次使用。
- 命名空间不要与 C struct 同名（`projectM-4/types.h` 的 `struct projectm` 冲突 → 用 `_api` 后缀）。
- JUCE GL 默认 legacy profile；Core Profile 必须显式 `setOpenGLVersionRequired(openGL4_1)`。
- `juce::Timer` + CPU Image 安全；+ WebView2 危险（vtable 竞态）。
- `paint()` 需窗口可见 OS 才调度 → 不能"等 paint 再 setVisible"；启动闪屏用纯黑底色。
- **`renderOpenGLPreLock`**（仅 Windows、GL 线程未持消息锁）：只做 projectM 按需懒初始化 + 预设切换 + 渲染 + 后处理，**不得触碰 Component / 消息线程状态**。
- **异步 9,921 预设扫描 + 按需懒初始化（v3.5.0 启动优化）**：
  - 工作区无嵌入态 Milkdrop（或仅在浮动态）时，`newOpenGLContextCreated()` 跳过 `initMilkdropEditorResources()`（零 DLL / 零扫描 / 零着色器开销），后续用户添加模块时由 `renderOpenGLPreLock()`（Win）/ `renderOpenGL()`（macOS）懒创建。
  - 存在嵌入态 Milkdrop 时，通过 `startMilkdropPresetScanAsync()` 在后台线程用 `RangedDirectoryIterator` 扫盘排序；若 GL 首帧先于扫描完成，用 `findFastInitialMilkdropPresetPath()` 在 `<0.5ms` 内单步加载首个/目标预设出首帧，扫描就绪后由 `source/ui/MilkdropPresetIndex.h`（纯 C++17 `reconcileLoadedPresetIndex`）在锁外无损对账真实索引并消费在途 `jump/delta/random`（启动保存索引仅作无用户交互时的兜底，绝不吞掉用户在扫描窗口内的手动点击）。
  - 扫描线程启停与 `ToggleLibrary`/`Rescan` 同步重扫统一串行在 `milkdrop_scan_lifecycle_mutex_` 下（worker 线程绝不获取该锁，保证持锁 `join()` 无死锁，彻底消除跨线程对 `joinable std::thread` 并发赋值触发 `std::terminate()`），析构由原子 `editorBeingDestructed` + `milkdrop_scan_shutdown_` 双重封门并在 `openGLContext.detach()` 前后安全回收。
- **嵌入态跳过无效 30Hz 组件标脏**：Windows 嵌入态（`!isFloating && !attached`）动画帧由 `onFrame() → RequestGlFrame()` 驱动，`GLView::timerCallback()` 仅在首次就绪、聚焦态 / 透明显隐 / 收藏库态 / `lastSwitchMs` / `1.2s` 切换提示条或聚焦时预设索引与总数变化时才调用 `owner_.repaint()`；展开的 5 个子面板（Auto / Color / Effects / Wave / Tweak）的悬停高亮改由 `mouseMove` / `mouseExit` 按需触发 `repaint()`，空闲时零额外组件重绘。
- Milkdrop 静音 PCM 用静态只读数组（免每帧 16KB 堆分配清零）。

## 4. 着色器文本

- JUCE `translate*ShaderToV3` 只做"文本追加 + 关键字替换"，**不替换已有的 `#version`**；喂已带 `#version` 的源会双 `#version`。要复用翻译，先剥掉源里的版本行。
- GL 后处理/着色器类资源初始化失败**必须留日志**（否则功能整体消失而 UI 无提示，bug 潜伏整版本）。

## 5. 文本绘制

- JUCE `drawText(text, rect, ...)` 是**裁剪式**排版：放不下就丢字形，不挤压、不加省略号。挤压用 `drawFittedText`；省略号 `useEllipsesIfTooBig=true`。
- 单字符/短文本最易踩：0.5px 就足以让第一颗 glyph 完全消失（表现为"标签随机丢失"，与主题/窗口宽度无关）。
- 自建文本位图缓存：宽度必须 `ceil` 并留余量，不能用 `round`。
- `PinkXP::getFont()` 有 **1.5× 放大**（正文），窄标签走位图要按 1.5× 反推尺寸，或直接 `axisFont=true`（坐标轴专用，不缩放）。
- **垂直居中统一走 `PinkXP::FontOptics`**（`opticalNudge` / `fitFontToHeight`，v3.4.1）：`drawText(..., centred)` 是按**行框**居中的，各字体 ascent/descent 比例不同 → 换皮肤 / 换字体后同一控件里的文字漂 0.5~2px。规则：拉丁字体对齐像素复古基准（默认皮肤零回归），CJK 字体按方块字墨迹精确居中。**不要再写 `translate(0, -1)` 这类手调补偿**，那是按某一套字体调的，换字体就会跑。
- **图标字符走 `SymbolGlyphs::drawIconGlyph`**：按字形**轮廓墨迹**居中。注意 `GlyphArrangement::getBoundingBox` / `PositionedGlyph::getBounds` 返回的是**行框**（advance × ascent+descent），拿它做"居中"等于没居中（`_` 依旧贴底、`*` 依旧顶天）；真实墨迹必须 `PositionedGlyph::createPath(path)` 后取 `path.getBounds()`。
- 徽标 / 标题这类"整张位图"摆放：对齐的是位图里的**文字排版框**（`StaticText::boxTop / boxHeight`），不是整张位图 —— 位图底部多留 1px，居中整图会让文字永远高 1px。
- **自适应字号（按可用宽度逐轮缩字号）必须缓存结果**（`SystemMonitorModule` 的 `fitFont`）：每次绘制 5 轮 `GlyphArrangement::getStringWidth`，在低频组件上会被连带重绘放大到每帧数十次。同理，分段描边能合并成一条 Path 就合并（同色同 alpha 的段互不重叠时结果完全一致）。

## 6. Milkdrop 效果系统（v2.6.x 起，详见 PROJECT_OVERVIEW.md §3.4）

- 注册表驱动：`MilkdropEffect` 元数据 + `GetMilkdropEffectDefs()`，effects 面板动态生成。
- 38 个后处理效果（`efftop`/`effbottom` 二分类）；color 面板（染色）、wave 面板（波形样式）、tweak 面板（后处理 uv 几何畸变 + 万花镜）。
- 视觉状态统一为 `MilkdropVisualState`（含 `offset` 成员承载 tweak 畸变），全局共享并持久化到 Processor host state。
