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

### 1.1 组件层性能规则（v3.4.3）

- **组件层每次 repaint 都会让脏区内所有组件按 z 序重画**（不是只画发起者）。一个"内容很少变"的组件只要压在每帧刷新的模块上，就会每帧整块重画。低频组件（电脑状态、计算器、悬停提示……）用 `y2k::ui::setGlSafeImageCache()`（实现见 `ModulePanel.cpp`）挂整组件位图缓存：只有自己失效的区域才重画进缓存，被连带重绘时只贴一张图。
  - 缓存固定用软件 Image、只在 GL 合成目标启用（判据 `getPreferredImageTypeForTemporaryImages()` 是否为 `OpenGLImageType`，**不要**用 `getCurrentContext() != nullptr`，见 §2.1）；浮动态窗口照原路径画。
  - 不要给每帧自绘的组件挂（等于多画一次 + 每帧上传纹理）。
- **渐变填充不便宜**：JUCE 的 GL 渲染器每次换填充都会丢掉缓存的"渐变颜色表纹理"。本仓库补丁 0007 按颜色停靠点复用、并把纹理挂在 `OpenGLContext` 关联对象上跨帧保留 —— 新皮肤大量使用渐变时这是关键项。
- **`g.drawRect(r, 1)` 在 GL 下走 EdgeTable + 多矩形裁剪求交**（比 4 次 `fillRect` 贵一个数量级）。每帧重绘的 chrome 边框用 4 次 `fillRect`（见 `PinkXP::chromeFrame`）。
- GL 直渲染模块的 overlay 里，静态文字走 `PinkXP::drawCachedText`，动态数值才走 `drawText`。`drawCachedText` 有 `Rectangle<int>` / `Rectangle<float>` 两个重载，花括号实参 `{x,y,w,h}` 会二义，必须显式写类型。

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
- **垂直居中统一走 `PinkXP::FontOptics`**（`opticalNudge` / `fitFontToHeight`，v3.4.1）：`drawText(..., centred)` 是按**行框**居中的，各字体 ascent/descent 比例不同 → 换皮肤 / 换字体后同一控件里的文字漂 0.5~2px。规则：拉丁字体对齐像素复古基准（默认皮肤零回归），CJK 字体按方块字墨迹精确居中。**不要再写 `translate(0, -1)` 这类手调补偿**，那是按某一套字体调的，换字体就会跑。
- **图标字符走 `SymbolGlyphs::drawIconGlyph`**：按字形**轮廓墨迹**居中。注意 `GlyphArrangement::getBoundingBox` / `PositionedGlyph::getBounds` 返回的是**行框**（advance × ascent+descent），拿它做"居中"等于没居中（`_` 依旧贴底、`*` 依旧顶天）；真实墨迹必须 `PositionedGlyph::createPath(path)` 后取 `path.getBounds()`。
- 徽标 / 标题这类"整张位图"摆放：对齐的是位图里的**文字排版框**（`StaticText::boxTop / boxHeight`），不是整张位图 —— 位图底部多留 1px，居中整图会让文字永远高 1px。
- **自适应字号（按可用宽度逐轮缩字号）必须缓存结果**（`SystemMonitorModule` 的 `fitFont`）：每次绘制 5 轮 `GlyphArrangement::getStringWidth`，在低频组件上会被连带重绘放大到每帧数十次。同理，分段描边能合并成一条 Path 就合并（同色同 alpha 的段互不重叠时结果完全一致）。

## 6. Milkdrop 效果系统（v2.6.x 起，详见 PROJECT_OVERVIEW.md §3.4）

- 注册表驱动：`MilkdropEffect` 元数据 + `GetMilkdropEffectDefs()`，effects 面板动态生成。
- 38 个后处理效果（`efftop`/`effbottom` 二分类）；color 面板（染色）、wave 面板（波形样式）、tweak 面板（后处理 uv 几何畸变 + 万花镜）。
- 视觉状态统一为 `MilkdropVisualState`（含 `offset` 成员承载 tweak 畸变），全局共享并持久化到 Processor host state。
