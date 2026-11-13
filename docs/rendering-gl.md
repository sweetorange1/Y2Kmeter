# 渲染 / GL / OpenGL / Milkdrop / Spectrogram3D

## 1. 渲染架构现状

- **Editor 级 `juce::OpenGLContext`**（`componentPaintingEnabled=true`，`attachTo(*this)`）使主窗口进入 GPU 合成管线 → 所有组件 paint 走统一 GPU → 单次 SwapBuffers → Z-order 正确、零遮盖。**OpenGLContext 必须放在类末尾**（保证反向析构时最先 detach），构造末尾 `attachTo`、析构起始 `detach`。
- **GL 直渲染模块框架 `GlDirectModuleBase`**（v2.9.4 引入）：派生类实现 4 个纯虚 `buildGlSnapshot` / `getGlCanvasBounds` / `paintGlOverlay` / `paintLegacyContent`，GL 直渲染 + CPU 回退双路径，**两条路径绘制内容必须一致**。
- **Milkdrop**（libprojectM 4 native OpenGL）用**独立** GL context（`componentPaintingEnabled=false`），单实例互斥（`std::atomic gActiveProjectMInstances`）。
- **Spectrogram3D** 最终是**纯 CPU**（曾 GPU shader 迁移，15+ 轮调试后回退）。
- **浮动态模块（pop-out 窗口）= 纯 CPU 软件绘制**：模块被移出 Editor 组件树后，
  Editor 的 GL 遍历不再覆盖它（`GlDirectModuleBase::wantsGlSnapshot()` 浮动态返回
  false），每帧都退化成消息线程上的整区软件光栅化。再叠加"按住拖拽 + 悬停标尺"的
  高频鼠标事件（高回报率鼠标可达数百 Hz）就会把消息线程打满，表现为**整个进程**
  卡顿 → `ModulePanel` 把浮动态的逐帧重绘（`repaintFrameContent`）与悬停重绘
  （`repaintHover`）统一限流到 `floatingRepaintHz`（30Hz）；被限流丢掉的悬停帧由
  `repaintHover` 的补齐定时器兜底，避免十字线停在上一帧位置。嵌入态（GL 直渲染）
  不节流，行为与旧版逐像素一致。

## 2. GL Image 消费规则（macOS 崩溃高发区）

1. **`OpenGLContext::getCurrentContext() != nullptr` 只说明"有 GL 上下文"，不等于"画进 GL"**。GL 线程上的离屏软件绘制（`createComponentSnapshot`）同样满足。判据要用"绘制目标身份"，不是上下文存在性。
2. **`OpenGLImageType` 的 Image 只能喂给 GL 上下文**。跨后端消费（CG / Direct2D / 软件光栅）中只有 Windows 软件光栅恰好容错；macOS 会越界读崩溃（Windows 不炸）。
3. **`Image::BitmapData` 的 `lineStride` 可为负**（bottom-up），`data` 也不一定指向首行。把 `data` 当连续 buffer 喂第三方 API（`CFDataCreate`+`CGImageCreate`）必炸。
4. **`createComponentSnapshot` 是"软件绘制"强信号**：新增此类调用点时，模块内部离屏缓存类型必须匹配；持 GL 纹理 Image 的模块要读 `ModulePanel::setSoftwarePaintTarget()/isSoftwarePaintTarget()` 回退软件 Image（目前 `Spectrogram3DModule`）。

## 3. Milkdrop（projectM）避坑清单

- 禁止嵌套 OpenGL 上下文（Editor GL=true 时子组件不能再附加独立 context）；"两个独立 context 不同 surface"≠嵌套。
- **Airspace Problem 无 API workaround**：GL HWND 覆盖 GDI 客户区只能靠"主窗口 GPU 合成管线"根治。
- DPI 用**物理像素**：`getWidth()` 是逻辑尺寸，`glReadPixels/glViewport` 需物理尺寸，在 `renderOpenGL` 里 `glGetIntegerv(GL_VIEWPORT)` 获取。
- projectM 内部强制 `glBindFramebuffer(0)`，**不要用自定义 FBO 限制它**。
- GLEW 函数指针绑定 HGLRC：重建 context 后必须 `FreeLibrary`+`LoadLibrary` 全量 reload。
- projectM handle 必须在 GL 线程创建/销毁；析构先同步 detach 等 GL 收尾。
- `componentPaintingEnabled(true)` 下 CachedImage 会被 ModulePanel 底色覆盖 → paintContent 用 `drawImageAt` 覆盖。
- `GLint/GLuint` 是全局作用域（khrplatform.h），**不加** `juce::gl::` 前缀。
- `glBlitFramebuffer` 格式不匹配时驱动静默丢弃，`glError` 仍为 0。
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
- **`renderOpenGLPreLock`**（仅 Windows、GL 线程未持消息锁）：只做 projectM 渲染 + 后处理，**不得触碰 Component / 消息线程状态**。
- Milkdrop 静音 PCM 用静态只读数组（免每帧 16KB 堆分配清零）。

## 4. 着色器文本

- JUCE `translate*ShaderToV3` 只做"文本追加 + 关键字替换"，**不替换已有的 `#version`**；喂已带 `#version` 的源会双 `#version`。要复用翻译，先剥掉源里的版本行。
- GL 后处理/着色器类资源初始化失败**必须留日志**（否则功能整体消失而 UI 无提示，bug 潜伏整版本）。

## 5. 文本绘制

- JUCE `drawText(text, rect, ...)` 是**裁剪式**排版：放不下就丢字形，不挤压、不加省略号。挤压用 `drawFittedText`；省略号 `useEllipsesIfTooBig=true`。
- 单字符/短文本最易踩：0.5px 就足以让第一颗 glyph 完全消失（表现为"标签随机丢失"，与主题/窗口宽度无关）。
- 自建文本位图缓存：宽度必须 `ceil` 并留余量，不能用 `round`。
- `PinkXP::getFont()` 有 **1.5× 放大**（正文），窄标签走位图要按 1.5× 反推尺寸，或直接 `axisFont=true`（坐标轴专用，不缩放）。

## 6. Milkdrop 效果系统（v2.6.x 起，详见 PROJECT_OVERVIEW.md §3.4）

- 注册表驱动：`MilkdropEffect` 元数据 + `GetMilkdropEffectDefs()`，effects 面板动态生成。
- 38 个后处理效果（`efftop`/`effbottom` 二分类）；color 面板（染色）、wave 面板（波形样式）、tweak 面板（后处理 uv 几何畸变 + 万花镜）。
- 视觉状态统一为 `MilkdropVisualState`（含 `offset` 成员承载 tweak 畸变），全局共享并持久化到 Processor host state。
