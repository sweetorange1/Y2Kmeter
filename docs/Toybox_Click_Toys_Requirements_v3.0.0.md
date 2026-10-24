# Toybox 点击解压互动 需求案（Click Toys）

> 状态：第 1 阶段已实现（见附录 C）；**第 2 阶段实测评审与整改需求已追加（见附录 D、E，2026-10-24）**。附录 D/E 与正文冲突时，以附录 D/E 为准。
> 目标版本：**v3.0.0**（大更新内容之一；版本号随发布流程统一抬升，本案不单独 bump）
> 日期：2026-10-24（附录 D/E 追加于同日，追加时只改文档、未改任何代码）
> 范围：点击软件内**非交互区域**时触发的解压小互动：画布空白区 + 各模块内部的非控件区域
> 目标一句话：**点哪儿都有反馈，每个模块的反馈都不一样，全部用 emoji 表达，并且在 GL 直渲染架构下一次做对。**
>
> 阅读指引：
> - §3 是对源码的事实核对（带文件:行号），后文所有约束都出自这里；
> - **§4（GL 兼容性）和 §5（Emoji 方案）是本案的第一约束**，上一版草案没有覆盖，请重点评审；
> - §8 是逐模块玩法，每条都标注了"实现层级"和"依赖的几何/数据"，可直接拆任务。
>
> 行号以 2026-10-24 的代码为准；实现时如有漂移，以函数名为准。

---

## 0. 摘要

| 项 | 内容 |
|---|---|
| 做什么 | 点击画布空白处喷出 emoji 并带物理，掉出窗口后销毁；点击 19 个模块类型内部的非控件区域，各有专属小玩法；连击字幕与 3 个彩蛋 |
| 怎么画 | **全部用 emoji**，从系统彩色 emoji 字体一次性烘焙成内存图集，运行时只画图，不画字 |
| 怎么跑 | 一个挂在 Editor 的全局鼠标监听器 + 一个鼠标穿透的叠加层 + 订阅现有 `AnalyserHub` 帧时钟的动画驱动 |
| GL 策略 | **零 GL 代码**。所有效果都画在 JUCE 组件层（恒叠在 GL 内容之上），不碰 `renderOpenGL`、VBO、shader、`VertexStream` |
| 输入 | **纯鼠标**。不处理任何键盘事件，也不依赖修饰键 |
| 不做 | 统计、成就、任何新持久化数据（只保存一个"互动档位"设置）；VirtuPet 和 Milkdrop 模块内部不做互动 |
| 规模 | 约 23 人日（估算），分 M0 到 M5；M0 是 1.5 天的技术验证闸门，通过后才进入正式开发 |

---

## 1. 已确认的产品决策

| # | 决策 | 对方案的影响 |
|---|---|---|
| 1 | Standalone 默认 Normal，插件宿主默认 Chill | 档位定义见 §10；两种形态默认值不同 |
| 2 | 保留"模块当平台"：精灵可在模块标题栏上沿弹跳 | 单向平台碰撞，弹性约 0.45，最多弹 2 次，之后必然掉出窗口（§8.1） |
| 3 | **不自绘精灵，全部用 emoji** | 取消像素精灵库与美术成本；新增 emoji 烘焙、探测与降级方案（§5），这是本案最大的新增技术点 |
| 4 | 要彩蛋 | Clippo、Y2K BUG、FPS 玩笑（§8.4）；全部纯鼠标触发 |
| 5 | **统计和成就不在 v3.0.0** | 删除统计、成就、徽章及其持久化；只保留"本次运行内存里的计数"用于连击与彩蛋 |
| 6 | 宠物联动仅动画 | 只触发 VirtuPet 的动画，不改饥饿、健康、运动状态（§8.4） |
| 7 | **不要键盘操作，纯鼠标** | 不新增 `keyPressed`，不抢键盘焦点；修饰键（Ctrl/Alt/Shift/Cmd）一律忽略，不触发也不改变行为；上一版"Alt+点击爆破拼豆"取消 |
| 8 | 这些内容属于 v3.0.0 大更新 | 版本号跟随发布流程；本案内所有"版本"指 v3.0.0 范围 |

---

## 2. 目标与非目标

### 2.1 目标

- G1 点击反馈**当帧**出现（延迟不超过 1 帧）。
- G2 每个模块类型的玩法不同，画布另有独立玩法。
- G3 **在 Windows GL 接管态、Windows 未接管态、macOS 软件路径、浮动窗口模块、插件宿主**五种情形下表现一致、无拖影、无闪烁。
- G4 不影响任何既有交互与分析结果。
- G5 可关闭、可降级，空闲零开销。

### 2.2 非目标（v3.0.0 不做）

- 统计、成就、排行、任何新持久化数据（档位设置除外）。
- 声音。Standalone 的 Loopback 会把本软件的输出采回分析链，发声会污染测量。
- 任何键盘操作与快捷键。
- 需要**真实改变 GL 画面内容**的效果（例如瀑布纹理盖章、S3D 山体变形、GL 顶点级位移）。这类效果列入 backlog（§16）。
- VirtuPet、Milkdrop 模块内部的点击互动（它们的点击语义已被占用）。
- 新增第三方依赖（Plan C 仅引入资源文件，见 §5.5）。

---

## 3. 现状技术事实（证据表）

> 本节所有结论都来自对源码的核对，不是假设。"约束"一列是对本案的直接要求。

### 3.1 渲染与合成

| # | 事实 | 证据 | 约束 |
|---|---|---|---|
| F1 | Editor 级 `OpenGLContext` **仅非 macOS 挂载**：`setContinuousRepainting(false)`、`setComponentPaintingEnabled(true)`、`attachTo(*this)`。macOS 不挂载，全部走软件光栅 | `PluginEditor.cpp:1831-1836`，原因注释 `:1802-1811` | 同一份效果必须在 GL 合成和软件两条路径下都正确 |
| F2 | JUCE 每帧顺序：**先** `renderer->renderOpenGL()`（GL 直渲染内容），**再**把组件缓存里**无效区域**重绘进持久 FBO，**最后** `drawComponentBuffer()` 叠加到屏幕 | `third_party/JUCE/.../juce_OpenGLContext.cpp:414-439`、`:520-549`；实际编译副本 `D:/Code/_deps/JUCE`（8.0.12）行为一致 | 组件层内容**恒在 GL 内容之上**；组件缓存**跨帧持久**，旧位置不主动清除就会留下拖影 |
| F3 | 组件的 `paint()` 在 **GL 渲染线程**执行，同时持有消息线程锁 | `juce_OpenGLContext.cpp:388`（`scopedLock.emplace`）、`:522`（断言持锁）；`PROJECT_OVERVIEW.md` 记录 GL 线程占 CPU 71% 的实测 | 彩蛋的 `paint` 必须极轻；其中禁止分配、IO、创建 Image、调用 `repaint()` |
| F4 | 8 个模块是 `GlDirectModuleBase`：Waveform、Phase、OscilloscopeWave、StereoField、Oscilloscope、Spectrum、Spectrogram、Spectrogram3D。接管后外壳透明、画布区**挖洞**，只保留 `paintGlOverlay` 文字；内容**不是子组件** | `GlDirectModule.h:35-181`，`GlDirectModule.cpp:94-114` | 点击仍落到 `ModulePanel`；不能用"实色遮盖"处理 GL 画布区 |
| F5 | 浮动/dock 切换时 `glDirectActive` 复位，模块回退 `paintLegacyContent`；浮动态没有 GL 渲染目标 | `GlDirectModule.h:104-116` | 彩蛋逻辑不得依赖"当前是否被 GL 接管" |
| F6 | 历史教训：永不 `glClear` FBO 0；GL 迁移成本远超预期；"不要在 `paint` 内调 `repaint()`"；"`triggerRepaint` 不可靠，用 Timer 驱动重绘" | `docs/GPU_ARCHITECTURE_DESIGN.md` §7 §8；`PROJECT_OVERVIEW.md` §6.32 第 10、11 条 | 本案**不新增任何 GL 代码** |
| F7 | `ModulePanel` 构造时 `setOpaque(true)`；**重载了 `repaint()` / `repaint(Rectangle)`**：做重绘合并（`pendingFullRepaint`/`pendingAreaRepaint`）和可见性门控（`isVisuallyActiveInWorkspace`）；合并标志在 `paintOverChildren` 开头清零 | `ModulePanel.cpp:80`、`:85-99`、`:319-326`、`:372-441` | 彩蛋对面板的重绘必须走 `ModulePanel::repaint(area)`；彩蛋绘制必须插在 `paintOverChildren` 的标志清零之后 |
| F8 | GL 纹理缓存按 `ImagePixelData` 缓存 Image 的纹理；Editor 设了 96MB 上限；CPU 端修改 Image 会触发整张重传 | `juce_OpenGLGraphicsContext.cpp:53-100`，`PluginEditor.cpp:5963`，注释 `:1805-1809` | emoji 图集烘焙后**只读**，绝不再修改像素 |

### 3.2 点击与事件

| # | 事实 | 证据 | 约束 |
|---|---|---|---|
| F9 | `ModulePanel::mouseDown`：右键 → toFront → 布局锁定则 return → 关闭/弹出按钮 → 8px 边缘缩放 → 标题栏拖动；**内容区内部点击什么都不做** | `ModulePanel.cpp:542-611` | 内容区点击是现成空位；但布局锁定时会提前 return，所以不能把钩子放在 `mouseDown` 里 |
| F10 | 工作区空白处：布局锁定直接 return；右键弹"添加模块"菜单；双击新增；左键处理拼豆贴画 | `ModuleWorkspace.cpp:1738-1764`、`:1915` | 画布玩法避开右键与双击；拼豆贴画 `PerlerImageLayer` 区域排除 |
| F11 | 项目已有"顶层窗口全局鼠标监听"先例 `addMouseListener(listener, true)` | `ModuleWorkspace.cpp:764` | 复用同一模式 |
| F12 | 会吞点击的子组件：DynamicsCrest 右侧 42px、Spectrum 顶部工具栏、Spectrogram/S3D 右侧 42px、Waveform/Oscilloscope/OscWave 按钮条与滑条、EQ 的两个滑条与标签；EQ 的 `PixelEqGraph` 会拦截但转发左键 `mouseDown` 给 `EqModule`；Milkdrop `GLView` 全转发；VirtuPet 自己重写 `mouseDown` | 逐模块核对（`source/ui/modules/*`） | 命中判定要排除控件；Label 若要纳入玩法可改 `setInterceptsMouseClicks(false,false)` |
| F13 | 标题栏由 Editor 在 `paintOverChildren` 绘制，工作区 `overlayPassThroughRect` 让标题栏点击穿透到 Editor | `Window_Behavior_Model_v2.8.5.md` §标题栏 | 标题栏彩蛋走 Editor 级判定 |
| F14 | 工具栏有 `fpsLabel`（只读 Label，显示 "xx.x fps"）和 `fpsBtn`（循环按钮） | `ModuleWorkspace.cpp:393`、`:472`、`:3056` | FPS 玩笑只绑定 `fpsLabel`，不碰 `fpsBtn` |

### 3.3 时钟与数据

| # | 事实 | 证据 | 约束 |
|---|---|---|---|
| F15 | `AnalyserHub` 每帧在**消息线程**分发 `FrameListener::onFrame(FrameSnapshot)`，节拍跟随 vblank 或兜底 Timer，频率为用户 FPS 上限（30/60/120）；`FrameSnapshot` 带 `timeSec`、`dtSec`（钳制到 1/1000 到 0.1）、`tickCount` | `AnalyserHub.h:396-476`，`PluginEditor.cpp:5746-5790` | **动画时钟就用 `FrameListener`**，不另起 Timer；这样彩蛋的重绘与模块自己的重绘合并进同一个 GL 帧 |
| F16 | Editor 已有同构先例：`FpsFrameListener`、`ChromeHiddenOverlay`、`TutorialOverlay`（按需创建/销毁，"引导不活动时子组件列表中完全不存在此组件"） | `PluginEditor.cpp:432-445`、`:486`、`:637`，注释 `PluginEditor.h:750-760` | 叠加层采用同样的"按需创建、闲置销毁"原则 |
| F17 | `FrameSnapshot` 字段只在对应 `Kind` 活跃时有效（`has(kind)`）；不应为了彩蛋去 retain 任何分析 Kind | `AnalyserHub.h:398-436` | 音频联动只被动读取，字段无效时退化为固定强度 |

### 3.4 文字与 Emoji

| # | 事实 | 证据 | 约束 |
|---|---|---|---|
| F18 | 项目像素字体 PinkXP（Silkscreen）**不含 emoji 字形**，直接画会回退到系统字体 | `PluginEditor.cpp:3548-3549` | 绝不依赖字体回退；必须显式指定系统 emoji 字体 |
| F19 | JUCE 8.0.12 的彩色字形链（软件渲染器与 GL 渲染器共用）：**位图层（PNG/sbix）→ COLRv0 分层 → （仅 Android 实现）系统回退 → 单色轮廓**；Windows/macOS 没有系统回退实现（`colourGlyphSupport == nullptr` 时返回空） | `juce_Typeface.cpp:330-337`、`:649-693`；`TypefaceFallbackColourGlyphSupport` 仅在 `juce_Fonts_android.cpp` 实现 | 位图型字体（Apple Color Emoji 为 sbix 位图）和 COLRv0 字形可直接渲染；**COLRv1-only 的字形在 Windows/macOS 上会退化成单色轮廓**。Segoe UI Emoji 据已知以 COLRv0 为主，但 Windows 11 新字形是否含 COLRv1 **尚未在本机验证**，所以必须探测，且由 M0 实测定案 |
| F20 | 字形层经 `drawImage` 绘制；旋转/变换文字时不保证命中字形缓存 | `juce_RenderingHelpers.h:2642-2692` | **禁止每帧用 `drawText` 画 emoji**；一律烘焙成图集再画图 |
| F21 | Windows 上默认 Image 实际是 `Direct2DPixelData`，其全局 DirectX 单例曾导致 DLL 卸载时卡死；项目已为此在 Editor 析构里主动清理 | `PluginEditor.cpp:1961-1977` | 图集必须用 `juce::SoftwareImageType` 创建，且在 Editor 析构里先于 DLL 卸载释放 |
| F22 | 源码注释里已大量使用 emoji（主题枚举注释 🪩🫧🩸 等），说明团队工具链对 UTF-8 emoji 无障碍 | `PinkXPStyle.h:212-224` | 源码中的 emoji 常量用 `\u` 转义或 UTF-8 字面量均可，但 MSVC 需保持 UTF-8 with BOM |

### 3.5 其他接入点

| # | 事实 | 证据 |
|---|---|---|
| F23 | Standalone 设置接入需三处：读（`ui.fpsLimit` 约 `:665`）、写（约 `:1299`）、加入 `managedKeys`（`:1057-1074`，保护式重写） | `Y2KStandaloneApp.cpp` |
| F24 | 插件状态在 `PluginProcessor.cpp` 的 `<PBEQ_State>` 里，主题相关属性在 `:664-666` | `PluginProcessor.cpp` |
| F25 | VirtuPet 动画枚举含 `angry=8`、`yell=31`、`startled=33`；`forceAnimation(int, bool)` 与 `hasAnimation(int)` 是 **private** | `VirtuPetModule.h:79-113`、`:176-178` |
| F26 | 单元测试模式：`tests/palette_test.cpp` + CMake target `Y2Kmeter_palette_test` + `add_test` | `CMakeLists.txt:1267-1277` |
| F27 | 布局预设、存档里**不应**出现任何彩蛋状态 | `ModuleWorkspace` 的 `saveModuleSpecificState` 机制 |

---

## 4. GL 兼容性设计（本案第一约束）

### 4.1 总原则

> **彩蛋只活在 JUCE 组件层；GL 层对彩蛋完全无感。**

由 F2 可知，组件层内容恒叠在 GL 内容之上，所以不需要、也不允许去碰 GL。把"GL 兼容"变成"组件层脏矩形管理"的问题，后者是项目里已被验证的成熟技术。

### 4.2 实现层级（每个效果必须归入其一）

| 层级 | 定义 | 允许做什么 | 适用 |
|---|---|---|---|
| **L1 叠加层**（必达） | 只在 `ModulePanel::paintOverChildren` 或 Editor 级 `ToyOverlay` 里用 `Graphics` 绘制；对模块**只读**（几何、数据） | 画环、闪光、粒子、气泡、emoji、条带、相纸；半透明叠加 | **所有**模块；GL 模块只能用 L1 |
| **L2 显示层钩子**（增强，限 CPU 模块） | 模块在**自己的 `paintContent` 里**读取一个显示偏移量（如指针角度偏移） | 指针摆动、读数压扁、拼豆位移 | 仅纯 JUCE 模块（VU、PhaseCorr、DR、EQ） |
| **B 级数据钩子**（可选，延后） | GL 模块在 `buildGlSnapshot()`（UI 线程、纯 CPU 构顶点）里叠加位移，使 GL 画面真实变化 | Spectrum 曲线被拨动、Waveform 列高调制、Phase 指针偏移 | **v3.0.0 不交付**，仅在 M5 有余量时评估；macOS 无 GL 接管，需另走 `paintLegacyContent`，因此默认降级为 L1 |

**L1 的"遮盖法"（先用背景色盖住原元素再画动画元素）只允许用在 CPU 模块的平色背景上**。在 GL 画布区域禁止遮盖（原因见 R8）。

### 4.3 硬性规则

| 规则 | 内容 | 违反后果（均有历史先例） |
|---|---|---|
| **R1 零 GL 代码** | 彩蛋文件中不得出现 `juce::gl`、`gl[A-Z]` 调用、`OpenGLShaderProgram`、`OpenGLFrameBuffer`，不改 `renderOpenGL`，不改 `VertexStream` 协议，不改 `GlDirectModuleBase` 的接管流程 | GPU 迁移曾耗 15+ 轮调试；评审用 `grep` 机检 |
| **R2 不读回 GL** | 禁止 `glReadPixels`、禁止截取 GL 画面做"拍立得"类效果 | 读回会同步阻塞；拍立得改用模块已有的数据（`snapshotL/R`）画缩略折线 |
| **R3 脏矩形** | 每个 actor 每帧记录"上一帧包围盒"和"本帧包围盒"，只重绘二者的并集；每帧合并为不超过 4 个矩形；全窗重绘仅在包围盒超过窗口面积 40% 时 | 组件缓存是持久 FBO，漏清旧位置会留拖影（`GlDirectModule.h:108-110` 已记录 dock 回来拖影的先例） |
| **R4 paint 极轻** | `paint` 在 GL 线程持锁执行：不分配、不 IO、不创建 Image、不字符串格式化（气泡文本预先生成）、不调用 `repaint()`；100 粒子 `paint` 目标低于 0.3 ms（M1 实测校准） | 锁住消息线程，UI 卡顿 |
| **R5 重绘走正确入口** | 对 `ModulePanel` 一律通过 `ModulePanel*` 调 `repaint(area)`（复用合并与可见性门控）；对 `ToyOverlay` 用 `juce::Component::repaint(rect)`；`paintOverChildren` 中彩蛋绘制插在三行标志清零之后、早退 `return` 之前 | 在基类指针上调用会绕开合并，或被 `pendingFullRepaint` 吞掉 |
| **R6 图集只读且为软件图像** | `juce::SoftwareImageType` 创建；烘焙完成后绝不改像素；Editor 析构时先释放 | F8、F21：整张重传；DLL 卸载卡死 |
| **R7 不依赖接管状态** | 命中几何一律用模块已有的 `area*`/`getCanvasBounds()`/`getGlCanvasBounds()` 等**与渲染路径无关**的值 | F5：dock/浮动往返时状态翻转 |
| **R8 GL 画布禁止实色遮盖** | GL 画布区是透明洞；对其只能做半透明叠加或独立元素绘制，不得用 `fillRect(实色)` 假装还原背景；不使用依赖目标像素的混合（差值、擦除） | 遮盖会盖住实时 GL 内容，视觉上是一块死区 |
| **R9 时钟复用** | 动画驱动 = `AnalyserHub::FrameListener`；不另起 `juce::Timer`；120Hz 下可隔帧推进，运动全部按 `dtSec` 积分 | 避免额外 GL 帧和 Milkdrop 额外渲染 |
| **R10 叠加层按需** | `ToyOverlay` 首次需要时才 `addChildComponent`，闲置 3 秒后移除；`setOpaque(false)`、`setInterceptsMouseClicks(false,false)`、`setWantsKeyboardFocus(false)`；位于工作区之上、`TutorialOverlay` 之下 | 与 `TutorialOverlay` 同理，杜绝对 GL 合成层的常驻干扰 |
| **R11 坐标转换** | 面板局部坐标到 Editor 坐标只用 `editor.getLocalPoint(panel, pt)`，不经屏幕坐标或 native peer | `GPU_ARCHITECTURE_DESIGN.md` 坑4 |
| **R12 不缓存组件图像** | 彩蛋组件不调用 `setBufferedToImage`/`setCachedComponentImage` | 与 `CachedImage` 黑箱冲突（`PROJECT_OVERVIEW.md` §6.32） |
| **R13 浮动窗口** | 浮动态模块不在 Editor 组件树内，Editor 级叠加层画不到它；模块内 actor 照常在面板里画，"需要逃出模块"的效果在该态下裁剪到面板内 | F5 |
| **R14 不改分析** | 彩蛋对 `FrameSnapshot`、`AnalyserHub`、各模块的分析成员只读 | 红线 |

### 4.4 运行路径矩阵（验收必须全覆盖）

| 路径 | 说明 | L1 | L2 | 关注点 |
|---|---|:-:|:-:|---|
| P-A Windows GL 接管态 | 8 个 GL 模块 `glDirectActive=true` | ✅ | ✅（CPU 模块）| 透明洞上的叠加、脏矩形、拖影 |
| P-B Windows 未接管态 | 启动首帧前、浮动态、dock 回来后的过渡帧 | ✅ | ✅ | 状态翻转瞬间无残留 |
| P-C macOS 软件路径 | Editor 不挂 GL，全部 `paintLegacyContent` | ✅ | ✅ | emoji 字体 Apple Color Emoji；性能 |
| P-D 浮动窗口模块 | 独立顶层窗口 | ✅（裁剪）| ✅ | 叠加层缺席；监听器需另挂 |
| P-E 插件宿主（VST3/AU） | 宿主窗口内的 GL 子窗口 | ✅ | ✅ | 误触、DLL 卸载（FL Studio 先例） |

### 4.5 GL 模块逐一可执行性判定

| 模块 | 点击能否到达 | L1 可行性 | 需要的只读接口 | 结论 |
|---|---|---|---|---|
| Phase | 能（GL 画布不是子组件） | ✅ 环、气泡、幽灵指针、弹珠 | `areaGonio/areaDial/areaWidth/areaBalance`（已缓存）、`smoothedCorr/Width/Balance` | 可做 |
| StereoField | 能 | ✅ | 静态函数 `computeRadarGeometry(plot)`（已有） | 可做 |
| Spectrum | 能（顶部工具栏 26px 排除） | ✅ | `getCanvasBounds`、`xToFreq`、`dbToY`（均已有） | 可做；B 级曲线拨动延后 |
| Spectrogram | 能（右侧 42px 排除） | ✅ | 新增只读 `getScrollPixelsPerSecond()`、`rowToFreq(y)` | 可做 |
| Spectrogram3D | 能（右侧 42px 排除） | ✅ | 新增只读 `getProjectionSlant()`（`projSlantX/Y`） | 可做；"贴山脊"取消 |
| Waveform | 能（按钮条与右侧 42px 排除） | ✅ | `getCanvasBounds`（已有） | 可做；B 级列高调制延后 |
| Oscilloscope | 能（按钮条排除） | ✅ | `getCanvasBounds`（已有） | 可做 |
| OscilloscopeWave | 能（按钮条排除） | ✅ | 新增只读 `getWaveformForToy()`（返回 `snapshotL/R` 拷贝） | 可做；用数据画缩略图，不截屏 |

---

## 5. Emoji 方案

### 5.1 总策略

1. **来源**：系统彩色 emoji 字体（Windows `Segoe UI Emoji`，macOS `Apple Color Emoji`，Linux 尽力 `Noto Color Emoji`）。显式指定字体名，不走字体回退（F18）。
2. **使用方式**：启动后**烘焙成内存图集**，运行时只做 `drawImage`（F20）。
3. **只用"单码点（可带 U+FE0F）"emoji**：不使用 ZWJ 序列、肤色、旗帜、键帽，规避断词与整形差异。
4. **探测 + 降级**：每个 emoji 烘焙后做质量探测；通过的才进池；池不足则降级（§5.4）。
5. **风格**：支持 `Smooth`（原样）和 `Pixel`（缩到 16×16 网格再最近邻放大，贴合 Pink XP 像素风）两种；M0 出对比截图，评审二选一，未决前默认 `Smooth`。

### 5.2 烘焙流程

```
bake(emoji, logicalSize S, renderScale r):
    px   = ceil(S × r) + 2                        // 含 1px 边距
    img  = Image(ARGB, px, px, clear=true, SoftwareImageType())
    g(img); g.setFont(FontOptions(emojiFamily, px × 0.82, plain))
    g.setColour(white)                            // 彩色字形层忽略此色；单色回退时才起作用
    g.drawText(emoji, cell, centred, false)
    if style == Pixel:  img = downsample(img, 16×16) → nearestUpscale(img, px)
    return img                                    // 此后只读
```

- 尺寸分三档：S=18、M=24、L=34（逻辑像素，乘 `PinkXP::ui()` 与渲染缩放），按需烘焙。
- 总内存目标：约 80 个 emoji × 3 档 × (px²×4B) 小于 6 MB（150% DPI 下约 4 MB）。远小于 GL 纹理缓存的 96MB 上限。
- 时机：Editor 显示后约 1 秒起，每个 timer tick 烘焙 8 个，分摊到数帧，**不在启动路径和 GL 线程上做**。
- 重烘焙触发：`PinkXP::setUiScale` 变化、显示器 DPI 变化、主题变化时仅在"需要换池"时重烘焙。
- 旋转：`Smooth` 风格用 `drawImageTransformed`（≤200 精灵，成本可接受）；`Pixel` 风格改为 0/90/180/270 四向步进翻滚，避免像素图被双线性糊掉。

### 5.3 探测（双色差分法 + 空白字形检测）

| 步骤 | 判据 | 剔除的情形 |
|---|---|---|
| ① 覆盖率 | 不透明像素占比 ≥ 12% | 空白、缺字形 |
| ② **双色差分** | 用白色和黑色各渲染一次，两图逐像素差异 ≤ 阈值 | **单色回退**（字形被画成纯色块；包括 COLRv1-only 退化）。注意不能用"饱和度"判断，因为 💀、⬜ 这类合法 emoji 本身低饱和 |
| ③ **豆腐块检测** | 与"必定缺失码点"（如 `U+10FFFD`）的渲染结果对比，相似度 > 98% 视为缺字形 | 方框占位 |
| ④ 池充足度 | 每个主题池存活 ≥ 6 个；通用池存活 ≥ 8 个 | 不足则降级 |

### 5.4 Emoji 版本分级

| 级别 | 定义 | 规则 |
|---|---|---|
| Tier-1 | Unicode Emoji ≤ 5.0（2017） | 白名单主体，**必须全部来自此级** |
| Tier-2 | 6.0 到 11.0 | 可用，但**必须指定 Tier-1 备选**（如 📸→📷，🥳→😍） |
| Tier-3 | ≥ 12.0 | 仅作"探测通过才加入"的加成，绝不作为唯一选择（🪙🪐🫧🪩🧊🩸 等） |

分级依据 Unicode 官方 emoji 表，M0 复核；**不按操作系统版本号推断**，全部以运行时探测为准。

### 5.5 三级方案与 M0 闸门

| 方案 | 做法 | 启用条件 | 成本 |
|---|---|---|---|
| **Plan A（主方案）** | JUCE 直接用系统 emoji 字体渲染并烘焙（§5.2） | M0 实测在 Windows 10/11、macOS 上通过率 ≥ 90% 且视觉可接受 | 零资源、零依赖 |
| Plan B（Windows 补丁） | 仅当 Windows 上大面积落入 COLRv1 单色退化时：用 Direct2D 原生图像（JUCE 的 `tryDrawColourGlyphs` 已支持 COLR/位图/SVG）绘制并拷贝到 `SoftwareImageType`，随后立即释放原生图像 | M0 在 Windows 上 Plan A 失败 | 约 +1 到 2 天；需遵守 R6 |
| Plan C（保底） | 内置开源 emoji 位图集（候选：Noto Emoji、Twemoji、Fluent Emoji；约 60 个 64×64 PNG，几百 KB，经 `BinaryData` 打包） | A 与 B 均失败，或评审要求跨平台外观完全一致 | 需要**许可证核查与署名**；仍是 emoji，不属于自绘 |

> **M0 的产出是一份明确的 go/no-go 结论**。没有这份结论，不进入 M1，避免"做到最后 emoji 在某平台是黑块"。

### 5.6 文字约束

所有彩蛋文字（气泡、连击字幕、弹窗文案）只用 **ASCII 英文大写、数字、符号 + emoji**，走 PinkXP 像素字体；不使用中文，避免字体回退（F18）。

---

## 6. 交互规则（纯鼠标）

### 6.1 什么算"非交互区"

| 命中对象 | 处理 |
|---|---|
| 画布空白（`ModuleWorkspace::getCanvasArea()` 内、没有模块、没有拼豆贴画、不在 hit-test 洞里） | 画布玩法 |
| `ModulePanel` 内容区（`getContentBounds()`），且命中点不在子控件上 | 模块玩法 |
| 标题栏、关闭/弹出按钮、8px 边缘缩放热区、右下 CPU 小字区 | 排除 |
| `Button`/`Slider`/`ComboBox`/`TextEditor`/`ScrollBar`，以及各模块右侧 42px 控制条里的控件 | 排除（控件自己吃事件） |
| 整块排除：`VirtuPetModule`、`MilkdropModule`、`PerlerImageLayer`、`PalettePanel`、`ThemeSwatchBar`、`ToolbarOverlayBackdrop`、`TutorialOverlay`、窗口缩放手柄 | 排除。用组件属性 `toy.optOut` 标记，监听器沿祖先链检查，不增加头文件耦合 |
| 工具栏其余控件 | 排除；仅 `fpsLabel` 与标题栏文字作为彩蛋白名单 |
| 弹出菜单、模态弹窗、新手引导期间 | 全部静默 |

### 6.2 手势

| 手势 | 行为 |
|---|---|
| 左键单击 | 触发该处玩法 |
| 连击（间隔 < 1.2 秒） | 累计连击数（§8.4） |
| 长按 ≥ 0.5 秒（位移 < 4px） | 画布：蓄力喷发；EQ：烘烤 |
| 按住拖动（位移 ≥ 4px） | 画布：星屑拖尾；Spectrogram：连续盖章 |
| 右键 | 保留给"添加模块"菜单，**不触发** |
| 双击画布 | 保留给"添加模块"；第一下已触发的喷发保留，`getNumberOfClicks()>=2` 时不再触发 |
| 中键、滚轮、触摸 | 忽略 |
| 任何键盘事件与修饰键 | **忽略**（纯鼠标） |

### 6.3 冲突处理

- **布局锁定 L**：只是编辑闸门。由于监听器挂在 Editor 而不是 `ModulePanel::mouseDown`，锁定时玩法照常可玩（F9）。
- **Hide / Stick / MV / Only Milkdrop 布局**：画布越小，初速按窗口高度换算（§8.1），保证弧线可见。
- **模态态**：`Component::getCurrentlyModalComponent() != nullptr` 时静默。
- **窗口停用**：窗口未激活时点击不触发（避免 DAW 里"点一下窗口获取焦点"就喷表情）。

---

## 7. 总体架构

```
Y2KmeterAudioProcessorEditor
 ├─ ToyDirector（成员；juce::MouseListener；档位；连击；路由；emoji 池）
 │    └─ editor.addMouseListener(&director, true)   // 接收所有嵌套子组件事件（F11）
 ├─ ToyClock（AnalyserHub::FrameListener，Editor 生命周期内常驻注册；无 actor 时 onFrame 立即返回）
 ├─ EmojiAtlas（SoftwareImageType 图集；分档；只读）
 ├─ ToyOverlay（按需创建的全窗口叠加层；鼠标穿透；非不透明）   // 仅承载"逃出模块"的与画布 actor
 └─ ModuleWorkspace → ModulePanel × N
      └─ ModuleToyState（懒创建 unique_ptr；仅被点击过的面板才分配）
           └─ 模块内 actor：在 ModulePanel::paintOverChildren 里绘制，裁剪在内容区
```

### 7.1 数据流

1. `ToyDirector::mouseDown` 过闸门（档位、左键、无模态、窗口激活、不在 optOut 子树）。
2. 沿 `e.originalComponent` 向上分类：玩具自带控件（Clippo、TP 弹窗的 OK）→ 白名单彩蛋（标题栏文字、`fpsLabel`）→ `ModulePanel` → 画布。
3. `ModulePanel` 分支：`toyAccepts(p)` 排除标题栏/边缘 → `toyHitTest(p, hit)` 得到元素 → `toyPoke(hit, ctx)`；未命中元素则走**通用兜底**（§8.5）。
4. `ToyClock::onFrame(frame)`：`dt = frame.dtSec`；推进所有 actor；计算脏矩形；通过 R5 规定的入口重绘。
5. 面板销毁、模块关闭、模块缩放时，`ToyDirector` 通过 `Component::SafePointer` 发现并清理对应 actor；缩放或布局预设切换时取消该面板的全部 actor（几何已失效）。

### 7.2 actor 归属

| 类型 | 坐标 | 绘制位置 | 生命周期 |
|---|---|---|---|
| 模块内 actor | 面板局部 | `ModulePanel::paintOverChildren`，裁剪到内容区 | 随面板，随面板移动 |
| 自由 actor（画布精灵、掉落的拍立得、金币雨、Clippo、连击字幕） | Editor 坐标 | `ToyOverlay::paint` | 掉出窗口、TTL 到期（8 秒）或被点击销毁 |

模块内 actor 的"逃出"：玩法在适当时刻调用 `ctx.promoteToFree(actor)`，把面板局部坐标经 R11 转成 Editor 坐标，交给叠加层。浮动态下该调用被降级为"保持在面板内"。

### 7.3 线程模型

- 所有玩具状态**只在消息线程**修改（鼠标回调和 `onFrame`）。
- `paint` 在 GL 线程执行但**持消息线程锁**（F3），因此与状态修改天然互斥，不需要额外锁。
- 烘焙在消息线程分帧执行；图集发布后只读。
- 音频线程零参与。

### 7.4 音频联动（被动、可选）

- 读 `FrameSnapshot` 中已有效的字段（`has(kind)`）：`loudness`、`dynamics.rmsL/R`、`oscL/oscR`。
- 能量 = 可用字段的归一化 RMS；字段无效则固定为 0.5。
- Party 档位的节拍检测：低频能量跳变加 180ms 不应期；仅当 Oscilloscope 或 Loudness 已活跃时启用，否则静默降级。
- **不为彩蛋 retain 任何 Kind**（F17）。

---

## 8. 效果设计

> **修订提示（2026-10-24）**：第 1 阶段实测后，本节中以下内容已被附录 E 取代：§8.1 画布喷泉的触发与交互（改为按下即喷、按住持续喷、鼠标力场、点爆）；§8.2、§8.3 中除 Spectrum 与 OscilloscopeWave 以外的各模块玩法（改为"数据玩具"规则，见附录 D.6 与 E.4）；emoji 的选择与着色（改为按语义角色跟随主题，见附录 E.3）。

### 8.1 画布（无模块覆盖的桌面区）："表情喷泉"

**基础行为**

- 点击处喷出 7 到 14 个 emoji，锥角约 ±35°，带自旋。数量按档位与音频能量缩放（Chill 5 到 8，Normal 7 到 14，Party 10 到 20）。
- 初速按窗口高度换算：峰值高度取窗口高度的 18% 到 38%，即 `v0 = sqrt(2·g·H)`，小窗口也有完整弧线。
- 重力 g 初值 1100 px/s²（乘 `PinkXP::ui()`），轻微空气阻力；半隐式欧拉积分，`dt` 上限 50ms。
- **模块当平台**：只与各模块标题栏上沿碰撞，只在下落时，且上一帧位于平台上方才算；弹性 0.45，每个精灵最多弹 2 次，之后无视平台。浮动窗口里的模块不当平台。
- **销毁**：`y > 窗口高 + 精灵尺寸`，或 `x` 越界超过边距，或存活超过 8 秒。
- 同屏上限：画布 96，拖尾 32。

**主题池（Tier-1，完整清单见附录 A）**：按当前 `ThemeId` 选池；自定义主题与未覆盖主题使用通用池。

**桌面纹理"人格"**：同一引擎，不同参数表，不是 10 套代码。

| 桌面纹理 | 人格 | 参数与附加效果 |
|---|---|---|
| checker 棋盘 | Fall | 标准重力 |
| pixelStars | Twinkle | 低重力，alpha 闪烁，附加一颗斜向流星 ☄️ |
| scanGrid | Glitch | 位置按 2px 量化步进，加水平撕裂，双层错位绘制 |
| bigDots | Bubble Wrap | 点击处背景大圆点被戳成凹陷（叠加层画半透明凹陷圈），3 秒恢复 |
| bubbles | Float | 负重力上浮，左右摇摆，从顶部飞出 |
| diagStripes | Slide | 沿 45° 方向滑落，带摩擦 |
| hearts | Love | 上浮，附带 "+1" 小字 |
| zigzag | Lightning | 下落时 x 方向锯齿摆动 |
| crossHatch | Spark | 轨迹拖尾火花 |
| pixelFlower | Bloom | 点击处 3 帧开花，花瓣粒子飘落 |

**附加玩法**

- **长按蓄力**：长按时出现收缩环，松手按蓄力时长放大数量和速度。
- **拖动星屑**：空白画布上拖动，沿路径每 24px 抛一个 ✨ 并下落。
- **音频律动**（Party）：检测到节拍时，空中的精灵获得小的向上冲量。
- **宠物围观**：见 §8.4。

### 8.2 CPU 模块（纯 JUCE 绘制，无 GL 影响）

| 模块 | 命中区 | 玩法 | Emoji | 层级 | 需抽取/暴露 | 难度 |
|---|---|---|---|:-:|---|:-:|
| **Loudness** 五柱 | 亮格、刻度数字、峰值保持线 | **打砖块**：点中亮格，半径 2 格内的亮格炸成方块碎片（用该柱当前色）下落；峰值线被砸落弹跳；500ms 后回填 | 💥 | L1 | `cellRectAt(bar,row)`；`toyCoverFill()`（默认凹槽底色）；`bar*.peakValue` | S |
| **LUFS Realtime** | 大数字、右上小灯、进度条 | **老虎机**：点数字，逐位滚轮，每位错开 90ms 停下带回弹；3 秒内同一读数框连点 3 次出 "777" 金色闪烁加金币雨；点灯，灯变成萤火虫飞出；点进度条，亮度涟漪向两侧传播 | 🎰 💰 ✨ | L1 | `drawReadout` 拆出 `readoutParts(i)`（数字、灯、进度条矩形） | M |
| **True Peak** | 两个读数框 | **弹窗地狱**：弹出 Win95 风格小窗（复用 `PinkXP::drawRaised`/`drawPinkTitleBar`），内含 emoji 与英文文案与 OK 按钮；最多叠 5 个；点 OK 关闭，2 秒后自动关；叠满则 0.3 秒迷你蓝屏（亮度受限） | ⚠️ ❌ 💀 | L1 | 同上；OK 按钮由 `ToyDirector` 命中处理 | M |
| **Dynamics DR** | 三个读数框 | **压缩器压扁**：点击处向内压扁，阻尼弹簧过冲约 15%，数字同步缩放，气泡 "SQUISH" | 🗜️ | **L2** | `drawReadout` 增加 `squash` 参数（显示偏移） | M |
| **Phase Correlation** | 半圆表盘、指针、顶部数值 | **指针摆锤**：点指针（±6° 内）或表盘，指针受冲量摆动，撞 ±1 端点弹回并冒 "BONK" | 💫 | **L2** | 指针角度加 `toyAngleOffset`（`paintContent` 内） | S |
| **Phase Balance** | WIDTH 槽、BAL 槽 | **橡皮筋 / 弹珠**：点 WIDTH，填充条被拉到点击处再弹回；点 BAL，一颗 🔴 弹珠沿槽滚向点击点，撞壁反弹 2 到 3 次后消失（真实滑块不动） | 🔴 | L1 | 两个槽的矩形 | S |
| **Dynamics Meters** 四柱 | 柱体 | **幸运方块**：该柱最顶亮格向上顶起 4px，弹出 💰，抛物线后落回，柱底短暂显示 "x3" | 💰 | L1 | `meterRect(i)` | S |
| **Dynamics** 综合 | 四柱、DR 面板、Crest 区 | 按区域路由到上面的 Meters、DR、Crest 玩法 | — | 复用 | `areaMeters/areaDr/areaCrest`（已缓存） | S |
| **Dynamics Crest** | 历史曲线 plot | **Line Rider**：⚽ 从点击处落下，落在 crest 曲线上沿斜率滚动，随曲线左移被带走，滚出左边销毁 | ⚽ | L1 | 只读 `crestYAt(x)` | M |
| **VU 表** | 指针、表盘玻璃、LED | **拨针**（L2）：点指针，指针获得角速度弹簧摆动，撞针弹回；**玻璃裂**（L1）：点表盘空白，出现 5 到 7 条放射裂纹，4 秒淡出；**迪斯科灯**（L1）：点 LED，LED 轮换变色并环绕 ✨，换色不超过 3 次每秒 | 💥 ✨ | L2+L1 | `dialGeometry()`（圆心、半径、LED 矩形，现在是 `drawDial` 局部变量）；指针 `toyAngleOffset` | M |
| **EQ 拼豆** | 豆格 | **拼豆爆米花**：点中豆，该豆弹起（弹簧），邻近豆果冻式晃动，空位 700ms 后缩放回填；**烘烤**：长按 0.5 秒，🔥 扫过，经过的豆子熔成实心方块，1.5 秒后恢复 | 🍿 🔥 | **L2** | `PixelEqGraph` 暴露 `cellRectAt(col,row)`；`paint` 读取 `toyBeadOffset`/`meltMask`；其 `mouseDown` 已转发给 `EqModule` | M |

**CPU 模块通用说明**

- "遮盖法"仅用于平色凹槽背景：`toyCoverFill(g, rect)` 默认填 `PinkXP::content`，背景不同的模块覆写。
- 点击在模块**未命中任何元素**时走 §8.5 通用兜底，保证"点哪儿都有反馈"。

### 8.3 GL 直渲染模块（全部只用 L1）

| 模块 | 命中区 | 玩法 | Emoji | 需要的只读接口 | 难度 |
|---|---|---|---|---|:-:|
| **Phase** | Gonio、表盘、WIDTH、BAL | **Gonio 声呐**：点击处发出同心像素环，气泡显示该点 M/S（由 `px=cx+S·R, py=cy−M·R` 反算）；**表盘**：叠一根"幽灵指针"在真实指针之上摆动并冒 💫（真实指针不动）；**WIDTH/BAL**：复用橡皮筋与弹珠 | 📡 💫 🔴 | `areaGonio/areaDial/areaWidth/areaBalance`、`smoothedCorr/Width/Balance` | M |
| **StereoField** | 半圆雷达 | **入侵者**：点击发出声呐半环，生成 👾，步进式左右移动并逐行下降，气泡显示声像角与 dB（由 `rho/ang` 反算）；再点 👾 击落并爆炸 | 👾 💥 | `computeRadarGeometry(plot)`（已是静态函数） | M |
| **Spectrum** | 曲线区（顶部 26px 工具栏排除） | **拨弦与音名**：点击处出现拨弦涟漪（同心环加竖向余振线），气泡显示频率与音名，如 "A4 +12c 440Hz"（`xToFreq` 后纯函数换算） | 🎵 🎸 | `getCanvasBounds`、`xToFreq`、`dbToY`（均已有） | M |
| **Spectrogram** | 瀑布区（右侧 42px 排除） | **贴纸炸弹**：点击贴一枚 emoji 贴纸，按 `pixelsPerSecond` 随瀑布向左滚动，附带 Hz 小标签；按住拖动每 24px 连续盖章 | 通用池 | 新增 `getScrollPixelsPerSecond()`、`rowToFreq(y)` | M |
| **Spectrogram3D** | 山脉区（右侧 42px 排除） | **火山与旗帜**：点击处 🌋 喷发，≤24 个 🔥 粒子；同时插一面 🚩，沿深度斜线缓慢漂向山后并淡出 | 🌋 🔥 🚩 | 新增 `getProjectionSlant()` | M |
| **Waveform** | 波形区（按钮条、右侧 42px 排除） | **人浪**：以点击处为中心向两侧传播半透明高亮带，浪峰上 🙌 起伏 | 🙌 👋 | `getCanvasBounds`（已有） | M |
| **Oscilloscope** | 屏幕区（按钮条排除） | **拍电视**：一次白闪（alpha ≤ 0.35，≤120ms）、滚动暗带、若干水平撕裂线、雪花点、左下角 "TRACKING" 闪烁约 1.2 秒 | 📺 | `getCanvasBounds`（已有） | S |
| **OscilloscopeWave** | 波形区（按钮条排除） | **拍立得**：📸 与快门闪光；用 `snapshotL/R` 画成缩略折线放进白边相纸，相纸带自旋掉出模块并继续掉出窗口（浮动态裁剪在面板内）。**不截屏、不读回 GL** | 📸（备选 📷） | 新增只读 `getWaveformForToy()` | M |

**GL 模块的统一要求**

- 效果全部是半透明叠加或独立元素（R8），不遮盖、不假装还原背景。
- 点击坐标换算只用上表的几何接口；窗口缩放、dock/浮动往返时取消该面板 actor（R7）。
- 在 macOS 与未接管态，同样的 L1 效果原样生效（它们本来就不依赖 GL）。

### 8.4 全局系统

**连击**（Chill 档位关闭字幕）

| 条件 | 字幕 |
|---|---|
| 5 连击 | "NICE!" |
| 10 连击 | "COOL!" |
| 20 连击 | "WOW!" |
| 40 连击 | "Y2K MODE"，精灵彩虹着色，顶部下约 3 秒 🎉 彩带雨 |

间隔 < 1.2 秒累计，空闲超过 1.2 秒清零。字幕在点击位置上浮淡出。计数只在**当次运行的内存**中，不持久化。

**彩蛋**（Normal 与 Party 档位；全部纯鼠标）

| 彩蛋 | 触发 | 表现 |
|---|---|---|
| **Y2K BUG** | 3 秒内连点标题栏的软件名/版本号文字 5 次（需标题栏可见） | 全窗口约 1.5 秒故障：水平撕裂条、RGB 错位的 "Y2K BUG!!" 文字，闪烁 ≤ 3Hz、alpha ≤ 0.35；随后冒气泡 "just kidding :)" |
| **FPS 玩笑** | 点工具栏 `fpsLabel` | 叠加层在 Label 上盖一块底色并显示 "9999.9 fps (lol)" 约 1.5 秒，不修改 Label 文本 |
| **Clippo** | 本次运行累计互动 ≥ 40 次，且距上次出现 ≥ 10 分钟 | 📎 弹出带气泡，说一句**原创**英文短句（不使用微软 Clippy 的原文案）；点击它则飞走。Party 档位阈值降到 25 次 |

> Clippo 文案（≤8 条，英文大写，终稿待你确认，见 §15）示例："PSST... THAT KICK IS HOT"、"NICE POKING!"、"HAVE YOU TRIED THE OTHER MODULES?"。

**宠物联动（仅动画，决策 6）**

- 新增 `VirtuPetModule` 的**公开薄接口** `playToyReaction(ToyReaction)`，内部仅调用既有 `forceAnimation(startled/yell/angry)`；先用 `hasAnimation(id)` 判断，不存在则只冒表情气泡。
- **不修改** 饥饿、健康、运动模式、巡逻状态。
- 触发：画布一次喷发 ≥ 8 个精灵 → `startled`（冷却 8 秒）；连击 ≥ 20 → `yell`；连击 ≥ 40 → `angry`。
- 宠物自身不作为点击目标（它已整体排除）。

### 8.5 通用兜底

模块内点击若未命中任何元素：在点击处生成 3 到 6 个 ✨ 小火花（上抛、淡出）加一圈半透明扩散环。保证"所有非交互区点击都有反馈"，也让还没做专属玩法的模块有默认表现。

---

## 9. 性能预算与降级

> 以下数值是**目标值**，M1 用真实构建实测后校准。

| 项目 | 预算 |
|---|---|
| 同屏 actor | 画布精灵 96，拖尾 32，单模块 48，全局上限 256（Chill 96，Normal 192，Party 256） |
| 单帧 CPU | 100 粒子的 `onFrame`+`paint` 合计 < 0.3 ms（`paint` 在 GL 线程持锁） |
| 空闲开销 | 无 actor 时 `onFrame` 立即返回；叠加层 3 秒后移除；不持有 Timer |
| 脏矩形 | 每帧合并为 ≤ 4 个；包围盒 > 窗口面积 40% 才整窗重绘 |
| 帧率联动 | 随用户 FPS 上限；120Hz 档可隔帧推进（运动按 `dtSec` 积分，不受影响） |
| 自动降级 | 实测 FPS < 24 持续 1 秒降一档（Party→Normal→Chill）；< 15 暂停新生成 5 秒 |
| Milkdrop 在场 | 彩蛋不增加 GL 帧率，仅复用现有分发节拍（R9），因此不会额外触发 projectM 渲染 |
| 内存 | emoji 图集 < 6 MB；`ModuleToyState` 懒创建 |
| 生命周期安全 | 所有跨组件引用用 `Component::SafePointer`；主题切换、窗口缩放、布局预设切换、模块关闭、dock/浮动往返的中途都必须安全 |

---

## 10. 设置与持久化

### 10.1 档位

| 档位 | 内容 |
|---|---|
| Off | 完全关闭；叠加层不创建，监听器早退 |
| Chill | 画布基础喷发（5 到 8 个）+ 模块点击反馈；无连击字幕、无彩蛋、无音频联动；上限 96 |
| Normal | 全部玩法 + 连击字幕 + 彩蛋；音频只影响数量；上限 192 |
| Party | Normal + 节拍跳动、彩带雨、Clippo 低阈值；上限 256 |

默认：**Standalone = Normal，插件宿主 = Chill**（决策 1）。

### 10.2 入口

- **首选**：工具栏新增一个与 `fpsBtn` 同构的循环按钮 `TOY`（Off/Chill/Normal/Party）。工具栏窄时随现有控件一起隐藏（参考 `fpsLabel` 的可见性逻辑 `ModuleWorkspace.cpp:2672/2687`）。
- **备选**：`showAddMenu`（`ModuleWorkspace.cpp:1347`）末尾追加 Toys 子菜单。注意布局锁定时该菜单不可达。
- 具体采用哪个，由 M1 实测工具栏剩余宽度决定（§15）。

### 10.3 持久化

| 形态 | 键 | 接入点 |
|---|---|---|
| Standalone | `ui.toyLevel` | 读（约 `Y2KStandaloneApp.cpp:665`）、写（约 `:1299`）、加入 `managedKeys`（`:1057-1074`）三处 |
| 插件 | `<PBEQ_State toyLevel="…">` | `PluginProcessor.cpp` 的 `getStateInformation`/`setStateInformation`（约 `:664`） |

**不写入**布局 XML、`PBEQ_Layout`、布局预设；不保存任何统计或成就（决策 5）。

### 10.4 安全约束

- 单次闪光亮度 alpha ≤ 0.35，闪烁 ≤ 3Hz；全屏故障效果持续 ≤ 1.5 秒。
- 系统级"减少动态效果"联动不在 v3.0.0（列入 backlog）。

---

## 11. 代码落地清单

### 11.1 新增文件

| 文件 | 内容 |
|---|---|
| `source/ui/toys/ToyKit.h` | **合并为单头**（项目已有 MSVC include guard 串扰教训）：`ToyLevel`、`ToyHit`、`ToyContext`、`ModuleToyState`、`ToyDirector`、`ToyClock`、`EmojiAtlas` 接口 |
| `source/ui/toys/ToyPhysics.h` | 不依赖 JUCE 的纯 C++：粒子积分、弹簧、单向平台碰撞、一维波动（可选） |
| `source/ui/toys/ToyKit.cpp` | 监听器、分类路由、时钟、叠加层、连击、彩蛋 |
| `source/ui/toys/ToyEmoji.cpp` | 烘焙、探测、池、分级、降级 |
| `source/ui/toys/ToyCanvasFx.cpp` | 画布玩法与桌面纹理人格表 |
| `source/ui/toys/fx/ToyFx_*.cpp` | 逐模块玩法，按模块族分文件，通过窄只读接口访问模块，**减少对模块 .cpp 的侵入** |
| `tests/toy_physics_test.cpp` | 纯函数单测 + CMake target `Y2Kmeter_toy_test`（沿用 `Y2Kmeter_palette_test` 模式，F26） |

### 11.2 改动点

| 文件 | 改动 |
|---|---|
| `source/ui/ModuleWorkspace.h` | `ModulePanel` 一次性加齐：`toyAccepts`、`toyHitTest`、`toyPoke` 三个虚函数（默认空实现，旧模块零改动可编译）、`unique_ptr<ModuleToyState>`；`ModuleWorkspace` 加公开 `isEmptyCanvasPoint(Point)`（当前 `getCanvasArea()` 为内部接口）。**改头文件会触发几乎全量重编译，所以在 M1 一次改完，后续不再动此头** |
| `source/ui/ModulePanel.cpp` | `paintOverChildren` 里在三行标志清零之后、早退 `return` 之前调用 `toyState->paint(g)`；`~ModulePanel` 已是 out-of-line，需包含 `ToyKit.h` 以释放 `ModuleToyState` |
| `source/ui/ModuleWorkspace.cpp` | `hookPanel` 下发 `ToyDirector*`；`isEmptyCanvasPoint` 实现（排除模块、拼豆、hit-test 洞）；TOY 按钮或菜单项 |
| `PluginEditor.h/.cpp` | 增加 `ToyDirector`、`ToyClock` 成员（**声明位置必须在 `openGLContext` 之前**，后者须保持在类末尾，见 `PluginEditor.cpp:1798-1800`）；构造里注册 `FrameListener` 与 `addMouseListener`；`resized()` 同步叠加层 bounds；析构先注销监听与时钟、再释放图集（R6） |
| 浮动窗口创建处 | 浮动态时对该面板 `addMouseListener(director, true)`；dock 回来时移除 |
| 各模块（VU、LUFS/TP/DR、PhaseCorr、PhaseBalance、DynamicsMeters、Loudness、EQ、Crest、Phase、Stereo、Spectrum、Spectrogram、S3D、Waveform、Osc、OscWave） | 暴露只读几何/数据接口（§4.5、§8.2、§8.3）；CPU 模块的 L2 钩子；覆写 `toyHitTest`/`toyPoke` 并转发到 `ToyFx_*` |
| `VirtuPetModule.h/.cpp` | 新增公开 `playToyReaction`，仅动画（§8.4） |
| 会吞点击的装饰 Label（`spanLabel`、`speedLabel`、`gainLabel`、`cellSizeLabel`、`freqRangeLabel`） | 可选：`setInterceptsMouseClicks(false,false)`，让这些区域也参与玩法 |
| `Milkdrop` / `VirtuPet` / `PerlerImageLayer` / `PalettePanel` / `ThemeSwatchBar` | 设置 `toy.optOut` 属性 |
| `Y2KStandaloneApp.cpp`、`PluginProcessor.cpp` | 档位读写（§10.3） |
| `CMakeLists.txt` | 源文件是显式列表，需逐个加入；新增 `Y2Kmeter_toy_test` |
| `PROJECT_OVERVIEW.md` | 新增小节：Toybox 架构、命中规则、GL 约束 R1 到 R14、避坑 |

### 11.3 接口草图

```cpp
struct ToyHit { int kind = 0; int id = 0; juce::Rectangle<float> rect; juce::Point<float> pos; };

// ModulePanel 新增（默认空实现）
virtual bool toyAccepts (juce::Point<int> panelPos) const;                  // 默认：仅内容区且不在标题栏/边缘
virtual bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const { return false; }
virtual void toyPoke    (const ToyHit& hit, ToyContext& ctx) {}

// ToyContext：给玩法用的受限能力
struct ToyContext {
    void  spawnEmoji (EmojiId, juce::Point<float> at, juce::Vector2D<float> v, Params);
    void  addModuleActor (std::unique_ptr<ToyActor>);          // 画在面板里
    void  promoteToFree  (ToyActor&);                          // 逃出模块（浮动态降级）
    float audioEnergy() const;                                 // 0..1，无数据时 0.5
    float uiScale() const;                                     // PinkXP::ui 因子
};
```

### 11.4 构建与验证流程

- 每个里程碑完成后**实际编译并实测**，不只做静态检查。
- 命令（实施前先确认仓库现有的 `CMakePresets.json` 与构建脚本）：`cmake --build build/msvc --config Release --target Y2Kmeter_Standalone -j 8`。
- 实际参与编译的 JUCE 是 `D:/Code/_deps/JUCE`（8.0.12），不是 `third_party/JUCE`；本案**不修改 JUCE**，所以无需双处打补丁。
- 改 `ModuleWorkspace.h` 后做一次完整重编译验证（历史上出现过 obj 与头不一致的崩溃）。

---

## 12. 里程碑与工期（估算）

| 里程碑 | 内容 | 估算 |
|---|---|---|
| **M0 技术验证闸门** | ① emoji 在 Windows 10/11 与 macOS 的烘焙与探测通过率，输出 Plan A/B/C 结论，附 Smooth 与 Pixel 对比图；② 在真实 Editor 里用一个硬编码方块叠在 Oscilloscope（GL 接管态）上，验证叠加可见、脏矩形无拖影、dock/浮动往返无残留；③ 测量 `paint` 在 GL 线程的耗时；④ 验证 `FrameListener` 在无活跃 Kind 时仍被调用（`FpsFrameListener` 已依赖此行为）| 1.5 天 |
| M1 引擎骨架 | `ToyDirector`、分类路由、`ToyClock`、`EmojiAtlas`、`ToyOverlay`、画布喷泉（先 3 套池）、`ModuleWorkspace.h` 一次性改齐、档位设置与持久化 | 4 天 |
| M2 CPU 模块 | **先做 Loudness、VU、EQ**，再做 LUFS、TruePeak、DR、PhaseCorr、PhaseBalance、DynamicsMeters、Dynamics、Crest（共 11 个）| 7 天 |
| **MVP 切点** | M0 + M1 + M2 中的 Loudness、VU、EQ + M3 中的 **Oscilloscope（GL 代表，最简单的 L1）** | 约 8.5 天 |
| M3 GL 模块 | **先做 Oscilloscope**，再做 Phase、StereoField、Spectrum、Spectrogram、S3D、Waveform、OscWave（共 8 个）| 5.5 天 |
| M4 全局系统 | 连击、彩蛋、宠物动画联动、长按蓄力、拖尾、性能守卫、桌面纹理人格 | 2.5 天 |
| M5 打磨 | 手感参数、全矩阵回归、文档、构建验证 | 2.5 天 |
| 合计 | | **约 23 人日**（含 GL 与 emoji 两项风险缓冲）|

> MVP 必须包含一个 GL 模块，目的是在早期就暴露 GL 风险，而不是留到 M3。

---

## 13. 验收标准与测试矩阵

### 13.1 功能验收

- 每个在范围内的模块，点击当帧有反馈，且互不雷同（共 19 个模块类型 + 画布，Dynamics 综合复用 3 个子玩法）。
- 画布 emoji 一定掉出窗口并销毁，无残留。
- 按钮、滑条、标题栏拖动、边缘缩放、拼豆贴画操作**完全不受影响**。
- 右键、双击画布的既有行为不变。
- 全程**零键盘依赖**：无新增 `keyPressed`、无快捷键、无键盘焦点抢占；修饰键按下时行为不变。

### 13.2 GL 专项验收

| 编号 | 标准 |
|---|---|
| AC-GL1 | Windows GL 接管态下，8 个 GL 模块的 L1 效果位置正确、可见、无拖影、无闪烁 |
| AC-GL2 | 对任一 GL 模块执行"浮动 → dock"往返 10 次，期间持续点击，无残留、无崩溃 |
| AC-GL3 | macOS 软件路径下全部效果等价生效 |
| AC-GL4 | 同屏 Milkdrop + 全部 GL 模块 + 100 个活跃 actor，帧率下降不超过 15% |
| AC-GL5 | 机检：在 `source/ui/toys/` 下搜索 `juce::gl`、以 `gl` 加大写字母开头的函数调用、`OpenGLShaderProgram`、`OpenGLFrameBuffer`、`glReadPixels`，**零匹配** |
| AC-GL6 | 机检：`renderOpenGL`、`GlDirectModuleBase`、`VertexStream` 无 diff |
| AC-GL7 | 空闲（无 actor）时 CPU 与 v2.x 持平；叠加层不在子组件列表中 |

### 13.3 稳定性验收

- JUCE leak detector 无泄漏。
- 布局 XML、`.settings`、宿主状态在"玩前"与"玩后"**逐字节一致**（档位设置除外）。
- VST3 在 FL Studio 反复插入/删除并关闭编辑器，不卡死（图集在析构前已释放，R6）。
- 连续狂点 10 秒，帧率下降不超过 15%，且无内存增长。

### 13.4 测试矩阵

| 维度 | 取值 |
|---|---|
| 形态 | Standalone、VST3、AU（mac） |
| 路径 | 对应 §4.4 的 P-A 到 P-E |
| DPI | 100%、150%、200% |
| 主题 | 深色与浅色各至少 3 个，含自定义主题；播放中切换 |
| 窗口状态 | L 锁定、Hide、Stick、全屏、MV 与 Only Milkdrop 预设 |
| 模块状态 | 浮动、动画中途关闭模块、动画中途缩放模块、动画中途切布局预设 |
| 画布 | 双击弹菜单、右键、拼豆贴画上点击、窗口缩放、`bigDots` 等 10 种桌面纹理 |
| FPS 档位 | 30、60、120、无上限 |
| 音频 | 有信号、无信号、无输入设备 |
| 单测 | 弹簧能量衰减、碰撞次数上限、粒子必然销毁、脏矩形并集正确 |

---

## 14. 风险登记

| 风险 | 概率 | 影响 | 缓解 |
|---|:-:|:-:|---|
| Emoji 在某平台渲染成单色/豆腐块 | 中 | 高 | **M0 闸门** + 双色差分探测 + Plan B/C |
| 不同系统 emoji 外观不一致 | 高 | 低 | 接受（Windows 与 Apple 风格不同）；需要一致则用 Plan C |
| GL 叠加出现拖影/残影 | 中 | 高 | R3 脏矩形；M0 专项验证；AC-GL1/2 |
| `paint` 在 GL 线程持锁导致卡顿 | 中 | 中 | R4；预算与实测；自动降级 |
| 彩蛋重绘被 `ModulePanel` 合并标志吞掉 | 中 | 中 | R5：走 `ModulePanel*::repaint`，绘制位置在标志清零之后 |
| `Direct2DPixelData` 图集导致 DLL 卸载卡死 | 低 | 高 | R6：`SoftwareImageType` + 析构先释放；FL Studio 回归 |
| 改 `ModuleWorkspace.h` 引起全量重编译与 obj 不一致崩溃 | 中 | 中 | M1 一次改齐；完整重编译验证 |
| 误触：DAW 里点插件窗口获取焦点就触发 | 中 | 中 | 窗口未激活不触发；插件默认 Chill；可关闭 |
| 光敏风险 | 低 | 高 | §10.4 硬性上限 |
| 彩蛋文案或形象涉及第三方 IP | 低 | 中 | Clippo 全部原创文案；仅使用系统 emoji 📎；Plan C 做许可证核查 |
| 浮动窗口监听器漏挂 | 中 | 低 | §11.2 专项条目；P-D 测试 |
| VirtuPet 公开接口改动影响宠物逻辑 | 低 | 中 | 只调用既有 `forceAnimation`，不改状态机；单独回归宠物 |

---

## 15. 遗留待确认项（均带默认假设，不阻塞开发）

| # | 事项 | 默认假设 | 何时定 |
|---|---|---|---|
| 1 | emoji 渲染风格：`Smooth` 还是 `Pixel` | 默认 `Smooth` | M0 出对比图后评审 |
| 2 | 档位入口：工具栏 `TOY` 按钮还是右键菜单 | 默认工具栏按钮，宽度不足时回退菜单 | M1 实测工具栏剩余宽度 |
| 3 | Plan C 的 emoji 位图集来源与许可证 | 不启用 | 仅当 M0 判定 Plan A、B 均失败时再启动核查 |
| 4 | Clippo 文案终稿（英文，≤ 8 条） | 使用 §8.4 示例风格 | M4 前 |

---

## 16. 后续版本 Backlog（不在 v3.0.0）

- 统计与成就（点击数、连击峰值、拼豆爆破数、徽章）及其持久化。
- 系统"减少动态效果"联动（Windows `SPI_GETCLIENTAREAANIMATION`，macOS `accessibilityDisplayShouldReduceMotion`）。
- B 级数据钩子：Spectrum 曲线被真实拨动、Waveform 列高调制、Phase 指针偏移、S3D 旗帜贴山脊（均在 `buildGlSnapshot()` 内实现，并为 macOS 的 `paintLegacyContent` 单独适配）。
- Milkdrop 与 VirtuPet 的内部互动（需先解决点击语义占用）。
- 真正改变 GL 内容的效果（如瀑布纹理盖章），需单独立项评估 GL 风险。
- 拼豆贴画上的互动。

---

## 附录 A：Emoji 白名单

> 全部为单码点（可带 `U+FE0F`）。分级以 Unicode 官方表为准，M0 复核。Tier-2 项必须有 Tier-1 备选。

### A.1 通用池（Tier-1，≥ 8 个存活才启用）

❤️ 💖 ⭐ 🌟 ✨ 🎵 🎶 😎 😍 💫 🔥 🎉 💎
Tier-2 加成：🥳（备选 😍）

### A.2 主题池（Tier-1）

| 主题 | 池 |
|---|---|
| blackPink（默认） | ❤️ 💖 ⭐ 🦋 ⚡ 🍒 📼 ✨ 💿 🖤 |
| bubblegum | 💖 🍓 🎀 🍭 🍬 🌸 ⭐ |
| starlight | ⭐ 🌙 🌍 ✨ 💫 ☄️ 🌠 |
| cyberLilac | 👽 ⚡ 💿 🔮 🎧 🕹️ 👾 |
| tangerinePop | 🍊 ☀️ 🍦 😎 🍹 🏖️ |
| aquaPearl | 🐚 💧 🐟 🌊 🐬 🦀 |
| matchaSoda | 🥤 🍃 🍏 🍵 🌿 🍀 |
| jungle | 🍃 🐸 🦋 🍄 🌴 🦎 |
| winXP | 💾 💿 🖱️ ⌛ 📁 🖥️ 💻 |
| crimsonNoir | 💀 🦇 🌹 🥀 🕷️ 🖤 |
| voidGrey / paperGrey | ⬛ ⬜ 🔲 🔳 ➕ ✔️ |
| custom | 通用池 |

### A.3 模块玩法用 emoji

| 模块 | Emoji | 备选（Tier-1） |
|---|---|---|
| Loudness | 💥 | — |
| LUFS | 🎰 💰 ✨ | — |
| TruePeak | ⚠️ ❌ 💀 | — |
| DR | 🗜️ | 💥 |
| PhaseCorr / Phase 表盘 | 💫 | ✨ |
| PhaseBalance / Phase BAL | 🔴 | — |
| DynamicsMeters | 💰 | — |
| Crest | ⚽ | — |
| VU | 💥 ✨ | — |
| EQ | 🍿 🔥 | 💥 |
| Phase Gonio | 📡 | 💫 |
| StereoField | 👾 💥 | 👽 |
| Spectrum | 🎵 🎸 | 🎶 |
| S3D | 🌋 🔥 🚩 | — |
| Waveform | 🙌 👋 | — |
| Oscilloscope | 📺 | — |
| OscilloscopeWave | 📸（Tier-2） | 📷 |
| Clippo | 📎 | — |

### A.4 Tier-3 加成（探测通过才加入池，绝不作为唯一选择）

🪙 🪐 🫧 🪩 🧊 🩸

---

## 附录 B：关键初值（M1 实测后校准）

| 参数 | 初值 |
|---|---|
| 重力 g | 1100 px/s²（乘 `PinkXP::ui()`） |
| 空气阻力 k | 0.15 /s |
| 平台弹性 | 0.45，每精灵最多 2 次 |
| 精灵尺寸 S/M/L | 18/24/34 逻辑像素 |
| 精灵 TTL | 8 秒 |
| 连击间隔 | 1.2 秒 |
| 长按阈值 / 位移阈值 | 0.5 秒 / 4px |
| 拖尾间距 | 24px |
| 闪光 | alpha ≤ 0.35，≤ 3Hz |
| 叠加层闲置销毁 | 3 秒 |
| 积分步长上限 | 50ms（`dtSec` 已被 Hub 钳制到 0.1） |

---

## 附录 C：开发进度记录

> 按轮次追加；每一轮记录"完成内容 / 遇到的问题与解决 / 剩余待办"，方便回溯与交接。

### 第 1 轮（M0 + M1 + M3-Oscilloscope + M4 全局系统）

**完成**
- M0 引擎骨架：`ToyKit.h`（单头接口）、`ToyKit.cpp`、`ToyEmoji.cpp`、`ToyCanvasFx.cpp`、`ToyPhysics.h`（纯函数）、`fx/ToyFx_Oscilloscope.cpp`。
- `ToyDirector`（Editor 全局鼠标监听 + 路由）/ `ToyClock`（FrameListener）/ `ToyOverlay`（鼠标穿透叠加层）。
- `ModulePanel` 新增虚拟接口 `toyAccepts/toyHitTest/toyPoke` + 懒创建 `ModuleToyState`。
- 画布"表情喷泉" + 主题池 + 桌面纹理人格表（§8.1）。
- Oscilloscope"拍电视"（GL 代表 L1，§8.3）。
- 全局系统：连击字幕（NICE!/COOL!/WOW!/Y2K MODE）、Y2K BUG、FPS 玩笑、Clippo、宠物联动（`VirtuPetModule::playToyReaction`）。
- 档位：工具栏 TOY 循环按钮 + `ui.toyLevel`（Standalone）/ `PBEQ_State toyLevel`（插件）持久化。
- 排除项：`VirtuPet`/`Milkdrop`/`TutorialOverlay` 用 `toy.optOut`；控件自动过滤。
- 物理单测 `Y2Kmeter_toy_test`（通过）。

**问题**
1. `juce::Vector2D<float>` 在 JUCE 8 不存在 → 全部改用 `juce::Point<float>`。
2. `juce::Component::isActiveWindow()` 不存在 → 改用 `editor.getPeer()->isFocused()`。
3. `ModuleToyState::paint` 需要 director → 改为在构造时缓存 `ToyDirector*`。
4. `EmojiId` 枚举值 `Wave` 与 `Wave` 重名 → 拆成 `OceanWave` / `WaveHand`。

### 第 2 轮（emoji 彩色渲染修复 + M2/M3 模块接入）

**问题（用户实测反馈）**
1. Oscilloscope 白闪正常；其它模块点击只看到"小圆点"（✨ 通用兜底），**看不到 emoji**。
2. 画布喷泉有一个"乱码字符"飞出。

**根因**
- JUCE 的 `SoftwareImageType` 走 `LowLevelGraphicsSoftwareRenderer`，只用单色 `getEdgeTableForGlyph`；Windows 的 Segoe UI Emoji 是 COLR 彩色字体，`GetGlyphRunOutline` 对彩色 glyph 返回空轮廓 → 图集探测 `probe()` 覆盖率=0 全失败 → 返回空 Image → 喷泉不画。
- 需自建 DirectWrite/D2D 彩色渲染路径。

**修复**
- `ToyEmoji.cpp` 新增 `renderEmojiWin()`：DirectWrite + D2D1.1（WARP 软件设备，纯 CPU）+ `D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT` 渲染彩色 emoji，PBGRA→ARGB 反预乘拷贝。设备/上下文 static 缓存，位图按尺寸每次建。
- `bakeOne()` 优先走 D2D 彩色路径（`imageHasContent` 覆盖率 ≥1.5% 判定成功），失败回退 JUCE 软件单色渲染（旧 probe）。
- 编译问题：`juce::ComSmartPtr` 非公共 API → 改用 WRL `Microsoft::WRL::ComPtr`；`juce::CharPointer_wchar_t` 不存在 → 改用 `wcslen`；`drawRect(int,int,int,int,float)` 重载歧义 → 线宽改 `int`。

**M3（GL 模块）全部接入完成（均 L1，纯半透明叠加，不读回 GL）**
- Spectrum：拨弦涟漪 + 频率/音名气泡（`xToFreq` 纯函数换算）。
- Waveform：人浪（高亮带 + 🙌 起伏）。
- StereoField：👾 入侵者步进移动。
- OscilloscopeWave：拍立得（📸 闪光 + `snapshotL/R` 缩略折线相纸，**不读回 GL**）。
- Spectrogram：emoji 贴纸随瀑布左移。
- Spectrogram3D：🌋 火山 + 🔥 粒子 + 🚩。
- Phase：Gonio 声呐环 + 表盘幽灵指针。

**M2（CPU 模块）本轮接入 2 个代表（MVP 优先）**
- Loudness：打砖块（点中柱 → 该柱当前色方块碎片下落，§8.2）。
- VU：玻璃裂（表盘空白放射裂纹）+ 迪斯科灯（LED 换色 ≤3Hz + 环绕 ✨）。

**新增文件**：`ToyModuleFx.cpp`（所有模块 actor 集中实现）+ `CMakeLists.txt` 注册。

**剩余待办（下一轮）**
- M2 其余 CPU 模块：LUFS 老虎机、TruePeak 弹窗地狱、DR 压扁（L2）、PhaseCorr 摆锤（L2）、PhaseBalance 橡皮筋/弹珠、DynamicsMeters 幸运方块、Dynamics 综合路由、Crest Line Rider、EQ 爆米花（L2）。
- L2 钩子类效果（EQ/VU 拨针/PhaseCorr/DR）需要改模块 paint，风险较高，单独一轮做。
- 拍立得的"逃出模块继续掉出窗口"（`promoteToFree`）本轮未做，暂裁剪在面板内。

### 第 3 轮（emoji 彩色渲染根因定位 + 修复）

**问题（用户二次反馈）**
- emoji 仍显示为"乱码"，无法测试。

**根因（用独立诊断程序 `tests/emoji_diag.cpp` 定位）**
- 第 2 轮的 D2D1.1 方案里，`ID2D1DeviceContext::CreateBitmap` 传了 `D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_TARGET` 组合 —— 这是**非法组合**，返回 `E_INVALIDARG (0x80070057)`；且 `CPU_READ` 位图在**硬件 D2D 目标上不支持**。
- 结果 `renderEmojiWin()` 返回空 → `bakeOne()` 回退到 JUCE 软件渲染 → COLR 彩色 glyph 无单色轮廓 → 渲染成 `.notdef` 豆腐块 → 探测把豆腐块当成"有内容"存进图集 → 用户看到"乱码"。

**修复**
- `renderEmojiWin()` 改用 **D2D 1.0 的 WIC 软件渲染目标**（`CreateWicBitmapRenderTarget`）：
  - 软件目标支持 `ENABLE_COLOR_FONT`（COLR 彩色 emoji），且 WIC bitmap 可直接 `Lock` 读像素，无需 `CPU_READ` 位图。
  - 不再需要 D3D11 / D2D1.1 / DXGI，依赖降为 `dwrite` + `d2d1` + `windowscodecs`（新增 `#pragma comment(lib,"windowscodecs.lib")`）。
- locale 由 `L""` 改为 `L"en-US"`（避免 fallback 链异常）。
- 新增 `imageIsColorful()` 豆腐块检测（≥3 种显著颜色才算成功），单色退化/豆腐块一律判失败并回退，杜绝"乱码"被当有效 emoji。
- 诊断日志改用 `OutputDebugStringW`（DebugView/调试器可见，Release 下也能看）。

**验证**
- 独立诊断程序实测：`Segoe UI Emoji exists=1`，`coverage=44.18%`，`colors=32`（💖 彩色渲染成功），BMP 落盘可肉眼核验。

### 第 4 轮（"乱码字符"关键线索定位 + 确定性编码修复）

**用户反馈截图的关键线索**
- StereoField 点击后显示的是一个"ð"样单色字符。`ð` 是 Latin-1 的 `0xF0` —— **正是 👾 的 UTF-8 首字节**。说明某一环节把 UTF-8 字节序列按 ANSI/Latin-1 逐字节映射成了 Unicode 码点再渲染（`F0 9F 91 BE` → `ð Ÿ ' ¾`），而非按 UTF-8 解码。

**修复（消除全部编码中间层）**
- `ToyEmoji.cpp` 新增 `utf8ToUtf16()`：直接从 `kSpecs[]` 的 UTF-8 字节手工解码成 UTF-16 码元数组（含 surrogate pair 与 VS16），**不再经过 `juce::String::fromUTF8` / `toWideCharPointer`**。
- `renderEmojiWin()` 改为直接接收 UTF-16 码元 + 长度（与独立诊断程序完全一致的调用形态）。
- JUCE fallback 渲染同样用该 UTF-16 数组经 `CharPointer_UTF16` 构造字符串，两条路径共用同一份数据。
- 诊断日志增强：`bake` 时输出 utf16 码元 hex + content/colorful（`OutputDebugStringW`，DebugView/调试器可抓）。

**验证（独立诊断程序，与项目内路径逐字节一致）**
- `utf8ToUtf16: D83D DC7E`（👾 的正确 surrogate pair）
- `via-utf8 conversion: coverage=34.16% colors=11` → **彩色渲染成功**
- 附带防呆：`imageIsColorful` 保证任何单色退化/豆腐块一律判失败，最差显示通用兜底圆点，不会再出现乱码。

### 第 5 轮（UTF-16 硬编码，彻底终结编码问题）

**日志定位（桌面 toy_emoji_log.txt）**
- `renderer ready (WIC software target)` → D2D/WIC 初始化成功。
- `bake id=8 utf16=[f0 9f 98 8d 0] content=1 colorful=0` → 传给 DirectWrite 的"UTF-16"其实是**原始 UTF-8 字节** `F0 9F 98 8D`（应为 `D83D DE0D`）。`utf8ToUtf16()` 的转换逻辑在项目编译产物里未生效（逻辑本身正确、独立诊断程序验证过，属编译产物不一致），导致 D2D 把 UTF-8 字节当 Latin-1 渲染成 `ðŸ˜` 单色字符。

**修复（根除一切运行时编码转换）**
- `kSpecs[]` 的 emoji 由 `u8"\x..."` 改为 **`u"\U0001F496"`** 字面量（`char16_t`，UTF-16 编码固定，编译器直接把完整码点编码成 surrogate pair，零运行时转换、零歧义）。
- `EmojiSpec.utf8` 字段改为 `utf16`（`const char16_t*`）。
- `emojiUtf8()` 用 `CharPointer_UTF16(reinterpret_cast<...>)` 构造；`renderEmojiWin()` 直接接收 `const WCHAR*` 码元 + 长度。
- 删除 `utf8ToUtf16()`（不再需要）。
- 附：`\uXXXX` 不能表示 surrogate 半区（C3850），必须用 `\UXXXXXXXX` 完整码点。

**验证**
- 诊断日志现在应显示 `utf16=[d83d dc96]`（正确 surrogate pair）且 `colorful=1`。
- 独立诊断程序此前已实证：同样的 UTF-16 码元经 WIC 软件目标渲染出彩色（coverage 44%、colors 32）。

### 第 6 轮（一次性补齐剩余全部模块玩法 + 宠物联动）

**完成内容**
- `ToyModuleFx.cpp` 新增 9 个模块 actor 工厂：`makeSlotRollerActor`（老虎机滚轮）、`makeFireflyActor`（萤火虫）、`makePopUpWindowActor`（Win95 弹窗）、`makeSquashActor`（压扁）、`makeBallActor`（弹珠）、`makeRubberBandActor`（橡皮筋）、`makeCoinPopActor`（幸运方块）、`makeCrestBallActor`（Line Rider）、`makeBeadPopActor`（EQ 爆米花）。
- **M2 CPU 模块全部接入**（§8.2）：
  - LUFS Realtime：老虎机滚轮 + 萤火虫
  - True Peak：Win95 弹窗地狱
  - Phase Correlation：指针摆锤（幽灵指针上半圆摆动）
  - Phase Balance：WIDTH 橡皮筋 / BAL 弹珠
  - Dynamics Meters：幸运方块（💰 x3）
  - Dynamics DR：压缩器压扁（阻尼弹簧 + "SQUISH"）
  - Dynamics Crest：Line Rider（⚽ 下落左移）
  - Dynamics 综合：按 areaMeters/areaDr/areaCrest 路由复用
  - EQ：拼豆爆米花（🍿 弹起 + 邻近豆晃动）
- **宠物联动补齐**：画布喷发触发 `startled`（33，冷却 8 秒）——此前仅连击 `yell`（31）/`angry`（8）已实现。

**简化决策（用户指示"先不用调整细节，等后续统一调整"）**
- L2 钩子类效果（EQ 长按烘烤、VU 拨针真实指针冲量、PhaseCorr 真实指针冲量、DR 真实 squash 参数）本轮以**纯 actor 叠加**近似（幽灵指针/压扁动画），未改模块 paint。
- LUFS "3 秒连点 3 次出 777 + 金币雨"、TruePeak "叠满蓝屏"、Spectrogram "拖动连续盖章" 等联动细节暂以基础效果呈现。
- 拍立得"逃出模块继续掉出窗口"（`promoteToFree`）仍裁剪在面板内。

**剩余（后续版本统一调整，非本轮阻塞项）**
- L2 精确钩子：EQ 烘烤 / VU 拨针 / PhaseCorr 冲量 / DR 真实 squash。
- `promoteToFree` 逃出模块机制。
- 需求案明确 backlog：统计、成就、B 级数据钩子（真实改变 GL 内容）。
- 真机视觉/GL 拖影矩阵回归。

---

## 附录 D：第 1 阶段实测评审（2026-10-24）

> 本附录只做评审，不含实现。依据：用户实测反馈 + 对 `source/ui/toys/` 全部代码、各模块 `toyHitTest/toyPoke` 的逐文件阅读。行号以 2026-10-24 的代码为准。

### D.1 用户实测反馈

| # | 反馈 |
|---|---|
| 1 | 点击没有被模块覆盖的画布空白处，**每次都有很多 emoji 从软件左上角迅速飞出**，很奇怪，互动性也不强 |
| 2 | 部分模块的互动让人迷惑，例如 Dynamics DR，看不出这个效果想表达什么 |
| 3 | **满意**：Spectrum、OscilloscopeWave。原因："有一定功能性，还很好玩" |
| 4 | 模块互动里出现的 emoji **没有跟随用户的调色风格** |

### D.2 结论

问题不在 emoji 资源本身，而在三件事：

1. **一个渲染变换 bug**，直接造成"从左上角飞出"（B-01）。
2. **多数模块的玩法没有"真实数据"这个锚点**。用户满意的两个模块恰好都有：Spectrum 读出了点击处的频率和音名，OscilloscopeWave 把当前真实波形定格成相纸。其余模块多数是"在点击处放一个与数据无关的动画"，所以看不懂（D.5、D.6）。
3. **主题化只做在了画布喷泉的 emoji 池上**，模块玩法的 emoji 和颜色全部写死（B-07）。

### D.3 缺陷清单

严重度：P0 = 用户已直接感知的错误，必须先修；P1 = 偏离需求或会引发问题；P2 = 小问题或隐患。

| 编号 | 级别 | 现象 | 根因与证据 | 修复要求 |
|---|:-:|---|---|---|
| B-01 | P0 | 画布 emoji 从左上角飞出 | `ToyCanvasFx.cpp:200-204`：粒子 `spin≠0` 时走 `g.drawImageTransformed (img, AffineTransform::rotation (spin, pos.x, pos.y))`。该接口把**图像左上角当作 (0,0)** 再整体变换，所以图像被甩到"以粒子位置为圆心、以 (0,0) 到粒子的距离为半径"的圆弧上，而不是画在粒子处。同一分支还**没有按 `drawSize` 缩放**，按烘焙原尺寸（逻辑尺寸×2+2）绘制，约大一倍。`spin` 初值为 0，每帧累加，所以首帧位置正确，第二帧起就"飞向左上角" | 变换改为：缩放到 `drawSize` → 平移 `(-s/2,-s/2)` → 旋转 → 平移到 `pos`。把"绘制一个 emoji（位置、尺寸、角度、透明度）"收敛成**唯一的** `drawEmoji()`，禁止各 actor 自己拼变换。变换系数写成纯函数放进 `ToyPhysics.h`，加单测：任意 `spin` 下图像中心必须映射到 `pos` |
| B-02 | P0 | 点击后不是当帧喷，而是松开鼠标才喷 | 画布喷发在 `mouseUp`（`ToyKit.cpp:671-684`），违反 G1（当帧反馈）。另外按住拖动超过 4px 后 `charging_=false`（`:654-668`），松开不会喷，只留一串 ✨ | 改为 `mouseDown` 立即喷（见 E.2 的 C-1、C-2） |
| B-03 | P0 | 空中出现"空气墙"：emoji 在没有模块的地方凭空反弹 | 平台只保存 y 坐标：`platformTops` 是 `std::vector<float>`（`ToyKit.cpp:343-353`），碰撞只比较 y（`ToyCanvasFx.cpp:155-166`），**不判断 x 是否在模块范围内**。只要和任一模块标题栏同高，任何 x 处都会反弹 | 平台改存 `{x0, x1, y}`，命中需 `x∈[x0, x1]`；浮动窗口里的模块不当平台 |
| B-04 | P1 | 缩放窗口时，新增区域内的 emoji 不显示 | `ensureOverlay` 只在叠加层**首次挂载**时 `setBounds`（`ToyKit.cpp:491-500`）；`PluginEditor.cpp` 里 `toyDirector` 仅 7 处引用（创建、档位、浮动监听），没有 `resized()` 同步。仅在叠加层已挂载期间（最后一次活动后 3 秒内）缩放窗口时出现 | Editor 的 `resized()` 里，叠加层已挂载则同步 `setBounds (getLocalBounds())` |
| B-05 | P1 | 首次点击可能卡顿；COM 在渲染线程初始化有风险 | `EmojiAtlas::image()` 未烘焙时直接 `bakeOne()`（`ToyEmoji.cpp:555-573`），而它由 `paint` 调用，即在 GL 线程、持消息线程锁时做 DirectWrite/D2D/WIC 初始化和渲染，违反 R4 与 §5.2 | 预烘焙放消息线程分帧做（启动约 1 秒后每 tick 8 个；主题、DPI 变化后重做）。`paint` 只读，未就绪就返回空图，调用方跳过绘制 |
| B-06 | P1 | 桌面出现 `toy_emoji_log.txt` | `toyLogFile` 向**用户桌面**写日志（`ToyEmoji.cpp:183-195`），烘焙时调用（`:433-445`） | 发布构建移除；调试期仅在 `JUCE_DEBUG` 下写应用数据目录 |
| B-07 | P1 | emoji 与颜色不跟随主题 | (a) 模块玩法的 `EmojiId` 全部写死（🎰💰⚠️🔴⚽👾🌋🙌🍿 等，见 `ToyModuleFx.cpp`），Spectrogram 贴纸池也写死且含 💀（`SpectrogramModule.cpp:185-188`）；只有画布喷泉用 `poolFor(theme.id)`。(b) 非 emoji 绘制写死 `juce::Colours::*`，`source/ui/toys/` 共 4 个文件 25 处。(c) toys 目录没有任何 `subscribeThemeChanged`，切主题后图集和调色没有响应 | 见 E.3 |
| B-08 | P1 | Clippo 提前出现；画布点击永不触发 Clippo | `routeGlobal` 与 `spawnCanvasFountain` 各 `++totalPokes_`（`ToyKit.cpp:643`、`ToyCanvasFx.cpp:283`），画布每次点击计 2 次；而画布分支不调用 `maybeSpawnClippo()` | 每次点击只计一次，且各路径统一走一个入口 |
| B-09 | P1 | 玩法在小模块里溢出、在大模块里位置不对；缩放界面后比例失调 | InvaderActor 边界写死 360×340（`ToyModuleFx.cpp:123-127`）；各 actor 内部大量字面量尺寸（24/18/34、150×64 等），没有乘 `PinkXP::ui()`，也不随模块尺寸变化 | 所有尺寸用 `ctx.ui(x)` 换算，位置与边界一律取自模块自身几何 |
| B-10 | P1 | 部分效果飞到一半突然消失 | actor 的 `getBounds()` 与实际运动范围不一致：BrickShards 固定 120×240，而碎片 1.4 秒内可下落数百像素；CoinPop、CrestBall 同理。脏矩形之外的绘制会被裁掉 | actor 自行维护运动包围盒；Debug 构建断言 `paint` 不越界 |
| B-11 | P2 | 若干"假功能" | SlotRoller 的 `digits` 参数未使用，"777" 永远不出现（`ToyModuleFx.cpp:404-426`）；PopUpWindow 的 OK 是假按钮，也不叠加；Crest 的小球不读取曲线；Phase 的声呐没有读数；EQ 命中范围含滑条区（`EqModule.cpp:536`），应收窄到 `eqGraph.getBounds()` | 按附录 E.4 重做 |
| B-12 | P2 | 需求里写的"可点击击落入侵者"未做 | Invader 没有任何点击响应 | 引入 `ToyActor::onClick` |
| B-13 | P2 | 空闲时仍每帧遍历模块 | `ToyClock::onFrame` 先刷新 `platformTops`（遍历所有模块）再判断有无 actor（`ToyKit.cpp:343-353`） | 先判断有无活动 actor，再刷新平台 |
| B-14 | P2 | 通用兜底仍是纯色圆点 | `SparkBurstActor` 画的是圆点（`ToyKit.cpp:837-852`），与 §8.5 写的"✨ 小火花"不一致 | 改为小号 Burst 角色 emoji + 主题色圈 |

### D.4 顺带发现的既有问题（不属于 Toybox，请你确认）

Phase 模块的 Goniometer 左右标签与点云方向看起来相反。按代码推导：点云映射为 `px = cx + S·R`，`S = (L−R)/√2`（`PhaseModule.cpp:583-587`、`:367-370`），所以**只有左声道**的信号落在**右上**；而标签 "L" 画在左上、"R" 画在右上（`:563-566`、`:173-180`）。常见 goniometer 是左声道偏左。

这是静态推导，建议用"只在左声道的正弦波"实测确认。本案不处理，也不影响 Toybox；但 Toybox 的 Gonio 读数文案要按实际映射写，你确认后再决定是否单独立项修正。

### D.5 逐模块评价与处置

| 模块 | 现状互动 | 评价 | 处置 |
|---|---|---|---|
| Spectrum | 拨弦涟漪 + 频率音名气泡 | 满意：有读数、反馈清晰 | **保留**，加 dB 读数、主题化 |
| OscilloscopeWave | 拍立得（定格真实波形） | 满意：定格真实数据 | **保留**，加图注、真正掉出模块、主题化 |
| Loudness | 同色方块碎片 | 好玩但无信息 | 加读数；增加"敲落峰值线" |
| LUFS Realtime | 单个随机数字滚动 | 不显示真实值 | **重做**：数字滚到真实值，并对标 -14 LUFS |
| True Peak | 固定 "TOO MUCH PEAK!" 弹窗 | 与真实电平无关，会误导 | **重做**：弹窗内容如实反映状态 |
| Phase Correlation | 幽灵指针 | 与真实指针无关 | **重做**：真实指针被拨动 + 读数 |
| Phase Balance | 橡皮筋 / 弹珠 | 与真实值无关 | **重做** |
| Dynamics Meters | 💰 + "x3" | "x3" 无意义 | 重做：顶砖 + 读数 |
| Dynamics DR | 粉色圆角块 + "SQUISH" | **看不懂**（用户点名） | **重做**：数字被压扁 + 白话判语 |
| Dynamics Crest | 小球直线下落 | 没用到曲线，名不副实 | 重做为真正的 Line Rider |
| VU | 玻璃裂 / 迪斯科灯 | 配色写死；"拨针"未做 | 加真实拨针；配色主题化 |
| EQ 拼豆 | 🍿 + 4 个粉方块 | 与豆子无关；命中范围错 | **重做**：弹豆 + 频率读数 |
| Phase | 声呐环 / 幽灵指针 | 声呐无读数 | 加 M/S/角度读数；表盘做追踪迷你曲线 |
| StereoField | 👾 入侵者 | 边界写死、无读数、不可击落 | 重做：扇区能量读数 + 在雷达内玩 |
| Spectrogram | 随机贴纸 | 无信息，含 💀 | 贴纸带时间频率读数；主题化 |
| Spectrogram3D | 🌋🚩 | 无读数 | 加频率、dB 读数 |
| Waveform | 平铺高亮带 + 🙌 | 与波形无关 | 读数 + 人浪沿真实包络 |
| Oscilloscope | 拍电视 | 纯视觉 | 保留视觉；"TRACKING" 换成真实读数 |

### D.6 设计原则升级："数据玩具"五条

两个满意模块的共同点是：**点击位置能读到真实数据，或把真实数据留住**。后续所有模块玩法必须同时满足：

| # | 原则 | 要求 |
|---|---|---|
| 1 | 锚点（Anchor） | 至少绑定该模块**真实在显示的一个量**（读数、历史或形状） |
| 2 | 手感（Feel） | 点击当帧出现物理反馈（弹簧、重力、碰撞），0.1 秒内可见 |
| 3 | 诚实（Honest） | 不显示与真实状态矛盾的信息。例：电平正常时不能弹 "TOO MUCH PEAK" |
| 4 | 主题（Theme） | emoji 按语义角色取，颜色走 `ToyPalette`，不写死 |
| 5 | 自解释（Self-explanatory） | 不需要说明书：文字不超过 3 个英文单词，或"一个数值 + 单位" |

---

## 附录 E：第 2 阶段（Round 2）开发需求

> 目标：把 Toybox 从"能跑"提升到"好玩、有信息、跟随主题"。
> 优先级：P0 = 必须先修；P1 = 本轮主体；P2 = 有余量再做。
> 不做：统计与成就、B 级数据钩子（真实改变 GL 画面）、任何键盘操作、任何声音。这几条沿用 §1、§2.2 的决定。

### E.1 P0：缺陷修复（先做，约 0.5 天）

| 编号 | 内容 | 验收 |
|---|---|---|
| FIX-1 | 修 B-01：统一 `drawEmoji()`，变换系数写成纯函数并加单测 | 1000 次随机点击，任意一帧的 emoji 绘制中心与逻辑坐标偏差 ≤ 1px；肉眼不再有"飞向左上角" |
| FIX-2 | 修 B-02：改为 `mouseDown` 立即喷 | 按下后首帧即可见 emoji |
| FIX-3 | 修 B-03：平台带 x 范围 | 在没有模块的 x 范围内，任意高度都不发生反弹（单测 + 实测） |
| FIX-4 | 修 B-04、B-08、B-13 | 缩放窗口后新区域可见；Clippo 计数只算一次且画布点击能触发；空闲时不遍历模块 |

做完 FIX-1 到 FIX-4 即可先出一个体验版，让你确认"左上角飞出"已消失，再进入后面的工作。

### E.2 画布 2.0："表情喷泉"的互动升级（P1）

现状的问题：喷出去就不管了，用户无法再影响它们。目标：让 emoji 像一把可以玩的小玩具。

| 编号 | 内容 | 细则 |
|---|---|---|
| C-1 | 按下即喷 | 见 FIX-2。喷口位置 = 鼠标位置 |
| C-2 | 按住持续喷（取代原"蓄力环"） | 按下后 0.35 秒内为一次爆发；之后每 70ms 喷 1 到 2 个，喷口**跟随鼠标移动**（花洒）；松开即停；总量受档位上限约束；按住越久初速越大（×1.0 到 ×1.5） |
| C-3 | 鼠标是力场 | 鼠标移动经过 emoji 时把它推开：半径 48px×ui，冲量正比于鼠标速度并设上限。只在存在自由 actor 时处理 `mouseMove` |
| C-4 | 点爆 | 点击空中的 emoji（命中半径取绘制尺寸的 0.6 倍）：它爆成 4 到 6 个主题色火花，并给邻近 emoji 一个冲量。命中后**本次点击不再触发喷发或模块玩法**。判定放在 `gateAllows` 之后、路由之前 |
| C-5 | 左右墙反弹 | 左右窗口边界各反弹一次（弹性 0.6），再次越界才销毁；底部出界销毁不变；顶部不限制。这样 emoji 更容易停留在可玩区域，同时仍会掉出窗口 |
| C-6 | 连击递进喷口 | 连击 5/10/20 时喷口形态依次变为：扇形 → 双向对喷 → 360° 环形爆发，与现有字幕联动 |
| C-7 | 落地回弹动画 | 碰到平台时做一次纵向挤压回弹（scaleY 0.8 到 1），小而有手感 |
| C-8（P2） | emoji 之间弹性碰撞 | 圆形碰撞，弹性 0.5，仅 Normal 及以上档位，粒子数不超过 96，每帧不超过约 4600 对检测 |

验收：按住鼠标 2 秒能持续喷出并随鼠标移动；用鼠标在空中扫过能推开 emoji；点爆有效；窗口缩放后仍正常；所有 emoji 最终掉出窗口并销毁。

### E.3 主题化系统（P1，对应反馈 4 与 B-07）

分三层，从简单到复杂，可逐层交付。

#### E.3.1 第一层：按语义角色取 emoji（`ThemeFlavor`）

玩法不再直接写 `EmojiId`，而是写**角色**，由当前主题决定用哪个 emoji。

| 角色 | 用途 | 是否随主题变化 |
|---|---|---|
| Reward | 奖励（LUFS 命中、Meters 顶砖、贴纸） | 是 |
| Burst | 爆裂（碎片、点爆、EQ 弹豆、通用兜底） | 是 |
| Orb | 小球（弹珠、Line Rider 球） | 是 |
| Foe | 入侵者 | 是 |
| Cheer | 欢呼（人浪） | 是 |
| Pin | 标记（贴纸、旗帜） | 是 |
| Alert | 警告（True Peak 警告态） | 否，固定 ⚠️（语义不能变） |
| Clear | 放心（True Peak 通过态） | 否，固定 ✔️ |
| Flame | 火焰（火山余烬） | 否，固定 🔥 |

**功能性图标**（形状本身就是功能）不换形状，改走 E.3.2 的着色：🎰（LUFS）、📸（OscWave）、📺（Oscilloscope）、📎（Clippo）。

默认值：Reward 💰、Burst 💥、Orb 🔴、Foe 👾、Cheer 🙌、Pin 🚩。下表只列出各主题的覆盖项，**草案，终稿由你确认**。所列 emoji 均已在现有 `EmojiId` 中，无需新增。

| 主题 | Reward | Burst | Orb | Foe | Cheer | Pin |
|---|---|---|---|---|---|---|
| blackPink | 💎 | 💥 | 🍒 | 👾 | 🦋 | 🖤 |
| bubblegum | 🍬 | 🌸 | 🍓 | 👾 | 🎀 | 🍭 |
| starlight | 🌟 | ☄️ | 🌙 | 👽 | ✨ | ⭐ |
| cyberLilac | 💿 | ⚡ | 🔮 | 👾 | 🎧 | 🕹️ |
| tangerinePop | 🍹 | ☀️ | 🍊 | 👾 | 🍦 | 🍊 |
| aquaPearl | 🐚 | 💧 | 💧 | 🦀 | 🐬 | 🌊 |
| matchaSoda | 🥤 | 🍃 | 🍏 | 🍄 | 🍀 | 🌿 |
| jungle | 🦋 | 🍃 | 🍄 | 🦎 | 🐸 | 🌴 |
| winXP | 💾 | 💥 | 💿 | 🖱️ | 💻 | 📁 |
| crimsonNoir | 🌹 | 🦇 | 🖤 | 🕷️ | 🦇 | 🥀 |
| voidGrey / paperGrey | ✔️ | ➕ | ⬛ | 🔲 | 🔳 | ⬜ |
| custom | 默认值 | 默认值 | 默认值 | 默认值 | 默认值 | 默认值 |

Spectrogram 的贴纸池也改为取 Pin 角色，不再写死。

#### E.3.2 第二层：着色与统一"贴纸"外观

系统 emoji 的颜色固定，仅靠换 emoji 不足以"跟随调色风格"。烘焙后增加两步处理，生成**主题版本**：

| 处理 | 做法 | 目的 |
|---|---|---|
| 双色映射（duotone） | 取每个像素的亮度 L，映射到主题三色渐变（`pink700` → `accent` → `pink100`），再与原色按强度 s 混合：`out = lerp(原色, 渐变(L), s)`，保留 alpha | 让不同 emoji 都带上当前主题的色调，同时保持可识别 |
| 贴纸描边 | 对 alpha 做 1 个逻辑像素的膨胀，描边色取主题 `ink`（浅色画布）或 `hl`（深色画布） | 统一成 Y2K 贴纸风格，也提升在复杂背景上的可读性 |

强度 s 的初值（评审时出对比图再定）：默认 0.55；winXP 0.35；voidGrey / paperGrey 1.0（灰度）；custom 0.6。

实现要求：
- 原图烘焙一次后保持只读（R6）；主题版本是**另一张**新图，在消息线程按主题变化分帧生成，`paint` 里有主题版本就用，没有就用原图。
- 图集内存预算由 §5.2 的 6 MB 放宽到 10 MB（原图约 4 MB + 主题版本约 4 MB）。
- 主题版本只对已烘焙过的条目生成，新条目在预烘焙时直接生成。

#### E.3.3 第三层：非 emoji 绘制的颜色走 `ToyPalette`

新增 `ToyPalette`，由主题变化回调刷新，提供语义色：`ink`、`paper`、`accent`、`accent2`、`glow`、`shadow`、`danger`、`warn`、`ok`，以及 `ramp(t)`。

现有 25 处 `juce::Colours::*` 的替换规则：

| 现有 | 替换为 |
|---|---|
| yellow、orange | `warn` |
| red | `danger` |
| lime | `ok` |
| cyan | `accent2` |
| white | `glow` |
| black | `dark` |
| lightgrey / darkgrey | `face` / `shdw` |

故障类效果（Y2K BUG、TV 雪花）允许保留"RGB 错位"的视觉语言，但两色必须取 `accent` 与 `accent2`，不写死纯红纯青。

验收：
- `source/ui/toys/` 内不再出现 `juce::Colours::`（`transparentBlack` 除外）。
- 13 个主题 × 所有模块互动各截一张图，逐一目检没有脱离主题的颜色；灰度主题下没有彩色刺眼元素。
- 切换主题后无需重启，下一次点击即使用新主题。

### E.4 逐模块重设计（P1）

约定：层级 L1 = 只在叠加层绘制；L2 = 需要模块在自己的 `paintContent` 里读一个显示层偏移量。GL 模块一律只用 L1。所有读数必须**复用模块自己的坐标映射**，不得复制 hover 的近似公式（见 E.9 风险）。

#### E.4.1 保留并增强

**Spectrum**（GL，L1，难度 S）
- 触发：点击曲线区。
- 表现：涟漪（`accent`）+ 气泡。气泡由 `A4 +12c 440Hz` 增加为 **`A4 +12c 440Hz -23.5dB`**。
- 数据：频率沿用 `toyXToFreq`；dB 取该列的 `smoothedDb[col] - slopeOffsetDb[col]`（Slope 打开时扣掉补偿才是真实 dBFS），`col = round((x - inner.x) / inner.w × (N-1))`。需新增只读 `toyReadDbAtX (x)`。

**OscilloscopeWave**（GL，L1 + 逃出模块，难度 M）
- 触发：点击波形区。
- 表现：📸 快门闪光 → 相纸掉出。相纸边框走 `face/hl`，折线走 `accent/accent2`；下沿增加**图注**：`L+R  RMS -18.2dB  PK -6.4dB  ~220Hz`。
- 相纸**真正掉出模块**继续落到窗口外（实现 `promoteToFree`，坐标转换按 R11）；浮动窗口里降级为裁剪在模块内。同屏最多 3 张。
- 数据：`snapshotL/R`（已有 public getter）本地计算 RMS、峰值、过零估频。

#### E.4.2 重做（CPU 模块）

**Loudness**（L1，难度 M）
- 点击亮格或暗格：气泡显示该行的刻度值（如 `-14.0 LUFS`）；亮格同时炸出该柱当前颜色的碎片 + 1 到 2 个 Burst 角色小 emoji。
- 点击峰值保持线附近（±4px）："敲落"：叠加层画一条下落的细条，气泡显示 `PEAK -9.3`。
- 点击右侧刻度尺：气泡显示该高度对应的 dB。
- 数据：行号公式 `row = (y - offsetY) / 7`，`rowDb = -60 + (1 - row/(numRows-1)) × 60`；峰值取 `barX.peakValue`。需把"点→行"抽成 const 函数供 `paint` 与 toy 共用。

**LUFS Realtime**（L2，难度 M）
- 点击读数框（M/S/I 之一）：数字**逐位滚动**约 0.9 秒，每位错开 90ms 停下，**最终停在点击瞬间的真实值**。停下后气泡显示与 -14 LUFS 的差，如 `vs -14: +2.1`。差值绝对值 ≤ 0.5 时触发"命中"：Reward 角色掉落 5 个。
- 彩蛋：3 秒内连点同一个框 3 次，数字显示金色 `777.7`，Reward 雨（替代原来从未出现的 "777"）。
- 点击右上小灯：萤火虫改为 `glow` 色光点，保留。点击进度条：气泡显示该位置对应的 LUFS 值。
- 实现：模块增加 `toyTextOverride[3]`（非空时 `drawReadout` 画这个字符串）；`drawReadout` 的矩形计算抽成共享函数 `readoutRects(i)`（数字区、小灯、进度条）。关闭档位时覆盖串强制清空。

**True Peak**（L1，难度 M）
- 点击读数框：弹出 Win95 风格对话框（复用 `drawRaised/drawPinkTitleBar`），**内容如实反映状态**：

| 当前 `tpDisplay` | 图标 | 文案 |
|---|---|---|
| ≥ -1.0 dBTP | ⚠️ | `INTER-SAMPLE PEAK +0.4 dBTP` |
| -6 到 -1 dBTP | ⚠️ | `CLOSE TO CEILING -2.3 dBTP` |
| < -6 dBTP | ✔️ | `ALL CLEAR -9.1 dBTP` |

- 对话框的 OK 按钮真实可点（带按下动画）；最多叠 3 层，每层错位 14px；第 4 次触发时最早的一层抖动后消失，并冒出 `TOO MANY WINDOWS` 的小玩笑。取消原先"叠满蓝屏"。
- 阈值写成常量；这是娱乐性提示，不是限幅判定。
- 实现：给 `ToyActor` 增加 `hitTest/onClick`，`ToyDirector` 在路由前先检查自带可点击元素（Clippo 已有同类特例，一并收敛）。

**Phase Correlation**（L2，难度 M）
- 点击表盘：**真实指针**被拨动。用 `y2ktoy::Spring` 驱动 `toyNeedleOffsetRad`，冲量方向远离点击侧、大小正比于点击点到指针尖的距离；阻尼回正，撞到 ±1 端点会弹回并冒 Burst 角色 emoji。
- 气泡显示点击处角度对应的相关度及判语：≥ 0.7 `MONO SAFE`；0.2 到 0.7 `WIDE`；< 0 `OUT OF PHASE`（带 Alert）。
- 数据：`corr`；表盘几何 `cx, cy, R` 抽成 `dialGeometry()`；`θ → corr` 反算与 `Phase` 模块一致。

**Phase Balance**（L1，难度 S）
- 点击 WIDTH 条：在点击处画一段"假设值"填充（ghost，从左到点击处）+ 气泡 `WIDTH 80%`，随后 ghost 用弹簧回落到真实值（会过冲再回），结束时气泡 `NOW 63%`。
- 点击 BAL 条：Orb 角色的弹珠从点击处出发，沿槽滚向**真实 balance 位置**，来回摆几次最终停在真实位置，像碗里的珠子找到中心；气泡 `BAL L12` / `R5` / `CENTER`。
- 数据：`width ∈ [0,1]`、`balance ∈ [-1,1]`、两条槽的矩形（抽取 `wBar/bBar`）。

**Dynamics Meters**（L1，难度 S）
- 点击某根柱：该柱最顶一格"顶砖"，弹出 Reward 角色，抛物线后落回柱顶；气泡按柱显示 `PK L -6.2` / `RMS R -18.0`（含该柱的峰值保持值）。去掉无意义的 "x3"。
- 数据：`meterDb[4]`、`bPeakL/R.peakHold`、柱矩形（抽取 `meterRect(i)`）。

**Dynamics DR**（L2，难度 M；用户点名"看不懂"）
- 点击读数框（Short DR / Integrated DR / Crest）：框内**数字被压扁**（纵向缩放 1 → 0.45 → 过冲 1.15 → 1，阻尼弹簧），同时在框上方冒出**白话判语**：

| DR 值 | 判语 | 角色 |
|---|---|---|
| ≥ 14 | `DYNAMIC` | Cheer |
| 10 到 14 | `BALANCED` | 无 |
| 6 到 10 | `COMPRESSED` | 无 |
| < 6 | `SQUASHED` | Burst |

  Crest 框显示 `CREST 12.3 dB`。
- 删除现有的粉色圆角块和 "SQUISH" 字样。
- 数据：`smoothedShortDR/IntegDR/Crest`。实现：模块增加 `toyDigitScaleY[3]`，`drawRow` 绘制数字时读取。
- 降级方案（若 L2 来不及）：保留判语气泡 + 读数框边框脉冲（L1）。这样也比现状好懂。

**Dynamics Crest**（L1，难度 M）
- 点击曲线区：气泡 `CF 11.2 dB @ -3.4s`；一颗 Orb 角色的小球在点击 x 处落到曲线上，沿曲线斜率滚动（下坡加速），同时随曲线向左滚动被带走，球上方标注该处 crest 值；滚出左边界时留 `REACHED -Ns` 气泡后销毁。
- 数据：`crestHist`（3600 格，30 格/秒）、`writePos`、`spanSeconds`、`plot`。需新增只读 `toyCrestAt (xNorm)`。
- 注意：`Dynamics` 综合模块的 crest 环只有 120 格，按 120Hz 写入，约 1 秒，时间轴要按它自己的尺度算，不能套用 Crest 模块的。

**VU 表**（L2 + L1，难度 M）
- 点击表盘：与 Phase Correlation 相同的弹簧拨针（真实指针被拨动，`toyNeedleOffsetRad`），气泡 `VU -6.2`（取 `currentMonoDbfs()`）。
- 点击玻璃（表盘空白处）：裂纹**条数随当前电平**增加（5 到 9 条），颜色走 `glow/ink`，裂纹中心放 1 个 Burst 角色 emoji，4 秒淡出。
- 点击 LED：颜色序列固定为 `ok → warn → danger → accent`（不再写死 RGB），换色不超过 3 次每秒；气泡写出阈值 `SIGNAL > -25 dBFS` / `CLIP >= 0`。
- 数据：`displayedL/R`、`ledLevelDb`；表盘几何抽成 `dialGeometry()`。

**EQ 拼豆**（L1 + 只读 getter，难度 M）
- 命中范围收窄为 `eqGraph.getBounds()`（现状含滑条与标签）。
- 点击某一列：该列所有亮豆**自下而上依次弹跳**（每格错开 25ms 的弹簧）；最顶一颗被弹飞成 Burst 角色 emoji（物理抛出，`promoteToFree`）；气泡 `1.2 kHz  -34 dB`。
- 点击暗豆：该处短暂亮起一颗 `accent` 色幽灵豆（0.6 秒）。
- 长按 0.5 秒（P2）：熨斗扫过，经过的豆子熔成实心方块，1.5 秒后恢复。
- 数据：列 → 频率 `f = specPos^2.3 × nyquist`；亮格数 → dB `= -80 + level × 80`。`PixelEqGraph` 是嵌套类，需新增 `GridGeom`（`offsetX/Y, step, numCols, numRows, cs`）和 `smoothedSpectrum` 的 const getter，并让 `paint` 与 toy 共用同一份几何。

**Dynamics 综合模块**（路由复用，难度 S）：按 `areaMeters / areaDr / areaCrest` 分别复用 Meters、DR、Crest 的 actor 与几何。

#### E.4.3 重做与增强（GL 模块，全部 L1）

**Phase 综合**（难度 M）
- Gonio：声呐环 + 气泡 `M +0.42  S -0.10  ANGLE 12°`，由 `m = (cy - y)/R, s = (x - cx)/R` 反算。**文案按代码实际映射写**（见 D.4）。
- 表盘：**追踪迷你曲线**：从点击起记录 3 秒的 `smoothedCorr`，在表盘下方画成一条 sparkline，结束时气泡 `MIN -0.12  AVG 0.71`。取代原来与数据无关的幽灵指针。
- WIDTH / BAL：复用 Phase Balance 的 ghost 与弹珠，几何取 `areaWidth / areaBalance`。
- 需要给 actor 一个读当前值的方式：以 `SafePointer<ModulePanel>` 捕获，actor 随模块存活，安全。

**StereoField**（难度 M）
- 点击雷达：声呐半环扩散 + 气泡 `PAN L32°  LEVEL -12dB  SECTOR 23%`。扇区占比 = 点击方向 ±10° 内 `snapshotL/R` 点数占总点数的比例。
- 入侵者：在点击扇区生成 Foe 角色，**只在半圆雷达内**逐行下降（边界用 `computeRadarGeometry`，取代写死的 360×340），**可点击击落**（命中半径取精灵尺寸），击落后 Burst + Reward；10 秒未击落则落到基线，显示 `LANDED` 后淡出。
- 数据：`computeRadarGeometry`（现为 private static，改为 public 或加 getter）、`snapshotL/R`。

**Spectrogram**（难度 M）
- 点击：贴一枚 Pin 角色贴纸 + 小标签 `1.2kHz -41dB`；标签 2 秒后收起，仅留贴纸；贴纸按 `pixelsPerSecond` 随瀑布向左滚动，滚出左边消失。
- 按住拖动：每 24px 盖一枚，标签省略。
- 数据：`gridData` 最右列该行的值（`dB = -90 + t × 90`）；行 → 频率 `f = 20·(fMax/20)^((rows-1-r)/(rows-1))`，其中 `fMax = min(20k, 0.95·nyq)`（hover 现用 nyquist，有 5% 偏差，toy 用正确的）。需新增只读 `toyReadAt (x, y)`。

**Spectrogram3D**（M 档 难度 M，L 档 难度 L）
- M 档：点击处 Flame 余烬，数量正比于该频点的当前幅度（`historyRing[最新][bin]`）；插一面 Pin 旗，沿 `projSlantX/Y` 的深度斜线缓慢漂远并淡出；气泡 `1.2kHz -34dB`（仅用 x → bin，与 hover 一致）。
- L 档（可选）：用投影公式反算命中最前层的 `(d, bin)`，旗插在山体表面并随历史流动。若工期紧，只做 M 档。
- 数据：`historyRing`、`projSlantX/Y`、`numBins = 128`。需新增只读 `toyGetProjection()`。

**Waveform**（难度 M）
- 点击：竖向标尺线 + 气泡 `-1.8s  PK -3.1  RMS -14.2dB`（取该列 `columnHistory` 的 max / rms）。
- 人浪（Cheer 角色）**沿真实包络上沿**从点击处向两侧传播，位置 `y = cy - maxV·halfH`；删除现有铺满画布的整块 `fillRect`。
- 点在包络之外（静音区）：气泡 `SILENCE`。
- 数据：`columnHistory`、`validCols`、`displaySeconds`、`gainDb`。已知问题：切换 3s/6s 后旧列时间尺度混用，读数标 `≈`。

**Oscilloscope**（难度 S）
- 保留"拍电视"视觉（闪光 alpha ≤ 0.35、撕裂线、雪花），配色走 `ToyPalette`（雪花 `glow`，撕裂线 `accent`，暗带 `dark`）。
- 左下角 "TRACKING" 改为真实读数 `440Hz  RMS -18dB`（过零估频 + RMS，取自快照）；XY 模式显示点击处反算的 `L +0.42 R -0.10`。

#### E.4.4 通用兜底

模块内未命中任何元素的点击：生成 3 到 5 个小号 Burst 角色 emoji（上抛、淡出）+ 一圈 `accent` 色扩散环，取代现在的纯色圆点。

### E.5 共用基础设施（新增与改动）

| 项 | 内容 |
|---|---|
| `drawEmoji()` | 唯一的 emoji 绘制入口（中心、尺寸、角度、透明度），变换系数为纯函数，进入单测 |
| 读数气泡 `ReadoutBubble` | 统一的像素风气泡：Win95 凸起框，尾巴指向点击点，自动避让模块边缘，1.6 秒上浮淡出。接口 `ctx.bubble (at, text, role)`。所有模块共用，保证"读数"体验一致 |
| `ToyHit` 扩展 | 增加 `float v[3]`（带给 actor 的物理量）、`int row, col` |
| `ToyActor::onClick` | 可点击 actor（True Peak 的 OK、Invader、Clippo）统一机制 |
| `ToyContext` 扩展 | `ui(x)`（乘 `PinkXP::ui()`）、`bubble(...)`、`role(Role)`、`palette()` |
| 主题模块 | 新增 `ToyTheme.cpp`：`ThemeFlavor` 角色表、`ToyPalette`、订阅 `PinkXP::subscribeThemeChanged` |
| `EmojiAtlas` | 预烘焙（消息线程分帧）、主题版本（duotone + 描边）、删除桌面日志、未就绪返回空图 |
| `promoteToFree` | 真正实现：面板局部坐标 → Editor 坐标（R11），浮动窗口降级为面板内 |
| L2 钩子 | `toyNeedleOffsetRad`（VU、Phase Correlation）；`toyTextOverride[3]`（LUFS）；`toyDigitScaleY[3]`（DR）。默认 0 / 空，关闭档位时强制清零 |
| 只读 getter 清单 | Spectrum：`toyReadDbAtX`；Spectrogram：`toyReadAt`；Spectrogram3D：`toyGetProjection`；StereoField：`computeRadarGeometry` 公开；EQ：`GridGeom` 与频谱数组；Crest：`toyCrestAt`；FineSplit：`readoutRects(i)`、`dialGeometry()`、`wBar/bBar`、`meterRect(i)`；Loudness：`rowAt(bar, y)` |
| 几何抽取 | 原先写在 `paintContent` 里的局部变量要抽成成员函数，`paint` 与 toy 共用同一份，避免显示与命中不一致 |
| 单测 | 在 `toy_physics_test` 里增加：`emojiTransform` 中心映射、带 x 范围的 `platformHit`、Spring 稳定性、点爆命中判定 |
| 文件组织 | 按原 §11.1 把 `ToyModuleFx.cpp` 拆为 `toys/fx/ToyFx_*.cpp`（按模块族），新增文件需逐个加入 `CMakeLists.txt` |

### E.6 里程碑与工期（估算）

| 里程碑 | 内容 | 估算 |
|---|---|---|
| R2-M0 | FIX-1 到 FIX-4（P0），出体验版 | 0.5 天 |
| R2-M1 | 共用基础设施：`drawEmoji`、气泡、`ToyPalette`、角色表、预烘焙、duotone 与描边、去日志、几何抽取与 getter | 1.5 天 |
| R2-M2 | 画布 2.0：C-1 到 C-7（C-8 视余量） | 1.5 天 |
| R2-M3 | GL 模块：Spectrum 增强、OscWave、Oscilloscope、Phase、StereoField、Spectrogram、S3D（M 档）、Waveform | 3 天 |
| R2-M4 | CPU 模块：Loudness、LUFS、True Peak、PhaseCorr、PhaseBalance、DynamicsMeters、DR、Crest、VU、EQ、Dynamics 综合 | 3.5 天 |
| R2-M5 | 主题截图矩阵、全矩阵回归、文档、版本号 | 1 天 |
| 合计 | | **约 11 人日** |

建议的交付顺序：R2-M0 → 你确认左上角问题消失 → R2-M1 → R2-M2（先让画布好玩）→ R2-M3（GL 模块里两个满意模块先升级）→ R2-M4。每个里程碑完成后都按项目规则实际编译并告知结果。

### E.7 验收标准

**画布**
- 1000 次随机点击，emoji 绘制中心与逻辑坐标偏差 ≤ 1px，截图抽检没有出现在逻辑位置之外的 emoji。
- 按下首帧可见；按住 2 秒持续喷出并随鼠标移动；鼠标扫过能推开；点爆有效。
- 无模块的 x 范围内没有空气墙反弹。
- 窗口缩放后喷泉在新区域可见；所有 emoji 最终掉出窗口并销毁。

**主题**
- E.3.3 的验收全部通过；图集总内存 < 10 MB。
- 13 个主题下，所有模块互动出现的 emoji 要么来自该主题的角色表，要么是做过着色的功能性图标。

**模块**
- 每个模块互动满足"数据玩具五条"（D.6）。
- 自测清单：无信号或电平正常时不得出现警告类文案；读数与该模块自己的显示读数一致（频率误差 ≤ 1 个音分量级，电平误差 ≤ 0.5 dB）。
- Spectrum 与 OscilloscopeWave 的体验不退化。
- 所有 GL 模块无拖影，AC-GL5、AC-GL6 继续成立。

**性能与稳定**
- 烘焙不在 `paint` 线程；首次点击不出现超过 16ms 的卡帧。
- L2 钩子关闭档位时，被改动的模块静态画面与改动前**逐像素一致**。
- 原 §9 的预算与降级策略不变。

### E.8 风险

| 风险 | 缓解 |
|---|---|
| L2 钩子触及模块的 `paintContent`，可能误改显示 | 钩子默认值为 0 / 空；关闭档位强制清零；回归 VU、PhaseCorr、LUFS、DR 的静态画面对比 |
| duotone 后 emoji 可识别度下降 | 强度可调，默认 0.55；评审时先看对比图再定 |
| 读数准确性争议 | 读数函数必须复用模块自身映射：Spectrum 的 Slope 补偿与 [1,2,1] 模糊；Spectrogram 的 `fMax` 与 hover 差 5%；Waveform 的 3s/6s 混用；Dynamics 综合的 crest 时间尺度。每个读数写一条对照用例 |
| 判语阈值（DR、True Peak）被误当成专业判定 | 文案保持玩笑口吻；阈值写成常量并在代码注释标明"娱乐性提示" |
| 不同系统的 emoji 外观不一致 | 接受；着色与描边能缩小差异 |
| 工期估算偏乐观 | 可砍项：S3D L 档、C-8、EQ 熨斗、DR 的 L2 版本（有降级方案） |

### E.9 待你确认（均带默认值，不阻塞开工）

| # | 事项 | 默认 |
|---|---|---|
| 1 | 画布"按住 = 持续喷泉"取代原蓄力环 | 是 |
| 2 | 左右墙反弹一次，取代越界即销毁 | 是 |
| 3 | emoji 着色：默认强度 0.55 + 描边；不提供用户设置项 | 是 |
| 4 | True Peak 阈值 -1 / -6 dBTP | 如 E.4.2 |
| 5 | LUFS 对标值固定为 -14 LUFS，不做可选 | 是 |
| 6 | S3D 做到 M 档还是 L 档 | M 档 |
| 7 | 各主题的角色 emoji 表（E.3.1） | 按草案，可逐项修改 |
| 8 | Phase Gonio 的 L/R 标签与点云方向（D.4）是否单独立项 | 你实测后决定 |
| 9 | Clippo 的触发阈值与文案是否保持 | 保持 |

---

### E.10 R2 进度记录

#### R2-M0（P0 缺陷修复）—— 已完成 ✅

| 编号 | 修复内容 | 落地位置 |
|---|---|---|
| FIX-1（B-01） | 统一 `drawEmoji()`（中心对齐 pos + 缩放 + 绕中心旋转），变换系数 `y2ktoy::emojiTransform` 纯函数进 `ToyPhysics.h`，`EmojiParticle::paint` 改用；加单测 `testEmojiTransformCentre`（1000 次随机 spin 中心映射偏差 ≤1px） | `ToyPhysics.h` / `ToyEmoji.cpp` / `ToyCanvasFx.cpp` / `toy_physics_test.cpp` |
| FIX-2（B-02） | 画布喷发从 `mouseUp` 移到 `mouseDown`（`routeGlobal`），当帧触发；`mouseUp` 清空 | `ToyKit.cpp` |
| FIX-3（B-03） | `platformTops` 由 `vector<float>` 改为 `vector<Platform{x0,x1,y}>`，碰撞走 `platformHitX`（x∈[x0,x1]）；加单测 `testPlatformHitXRange` | `ToyKit.h` / `ToyKit.cpp` / `ToyCanvasFx.cpp` / `ToyPhysics.h` |
| FIX-4（B-04/B-08/B-13） | 缩放时同步叠加层 bounds（`PluginEditor::resized`）；画布点击只计一次 `totalPokes_` 且会触发 Clippo；`ToyClock::onFrame` 仅在存在活动自由 actor 时才遍历模块刷新平台 | `PluginEditor.cpp` / `ToyKit.cpp` |

**验证**：Release Standalone 编译 + LTO 链接通过；`Y2Kmeter_toy_test` 全绿（`ALL_OK`）。

#### R2-M1 ~ R2-M4 —— 已完成 ✅（一次性交付，约 10.5 人日范围）

**R2-M1 共用基础设施**
- `drawEmoji()`：唯一 emoji 绘制入口（FIX-1 已落地），所有 actor 的 emoji 绘制改走 `themedImage`（duotone + 描边主题版本）。
- `ToyTheme.cpp`（新）：`ToyRole` 语义角色 + 各主题角色表（E.3.1 草案）+ `ToyPalette` 语义色（实时映射 PinkXP）。
- 读数气泡 `ReadoutBubble`：Win95 凸起框 + 尾巴 + 主题色，`ctx.bubble(at, text, role)` 统一入口。
- `ToyActor::onClick`：可点击 actor 统一机制（Clippo / Invader / TruePeak OK 均收敛），`ToyDirector::mouseDown` 在路由前统一检查。
- `ToyHit` 扩展 `v[3] / row / col`；`ToyContext` 扩展 `ui(x) / bubble / role`。
- `promoteToFree` 真正实现：`OffsetActor` 包装做面板局部 → Editor 坐标转换（R11）。
- `EmojiAtlas`：预烘焙（消息线程 Timer 分帧，启动延迟 1s，每 tick ≤8）+ 主题版本（duotone 亮度→`pink700/sel/pink100` 三色渐变按强度混合 + 1 逻辑像素 ink 描边）；`image()` 改只读，未就绪返回空图（B-05）；桌面日志已删除（B-06）；主题切换 `invalidateThemed()` 重新生成。
- 强度表：默认 0.55 / winXP 0.35 / 灰度 1.0（E.3.2）。

**R2-M2 画布 2.0**
- C-1/C-2：mouseDown 当帧喷 + 按住持续喷（花洒，0.35s 后每 70ms 1~2 个，初速 ×1.0→1.5，喷口跟随鼠标）。
- C-3：鼠标力场（半径 48px×ui，冲量正比鼠标速度，仅 Normal/Party，仅存在自由 actor 时处理）。
- C-4：点爆（命中半径 0.3×包围盒，爆成 4~6 个 Burst + 邻近冲量，消费本次点击）。
- C-5：左右墙各反弹一次（弹性 0.6），底部销毁，顶部不限。
- C-6：连击 5/10/20 喷口形态（宽扇形 → 双向对喷 → 360° 环形）。
- C-7：落地纵向挤压回弹（scaleY 0.8→1）。
- C-8（emoji 间碰撞，P2）：**未做**（留待余量）。

**R2-M3 / R2-M4 模块"数据玩具"化**（D.6 五条）
- 全部模块补齐**真实读数气泡**：Spectrum（+dB）、OscilloscopeWave（RMS/PK/估频图注 + 真掉出模块）、Oscilloscope（RMS/估频取代 TRACKING）、Phase（Gonio M/S、CORR）、StereoField（PAN 角度 + Foe 入侵者可击落）、Spectrogram（频率/dB + Pin 贴纸）、S3D（频率）、Waveform（时间）、Loudness（柱值）、LUFS（真实滚轮 + vs -14）、TruePeak（如实三态文案 + 可点 OK）、PhaseCorr（角度反算 + 判语）、PhaseBalance（真实 WIDTH/BAL）、DynamicsMeters（柱名 + 值）、DR（白话判语）、Crest（CF 值）、VU（真实 VU / LED 阈值）、EQ（频率）、Dynamics 综合。
- 全部 emoji 按语义角色（Reward/Burst/Orb/Foe/Cheer/Pin/Alert/Clear/Flame）+ duotone 主题版本；非 emoji 颜色走 `ToyPalette`（`source/ui/toys/` 已无 `juce::Colours::*`，除探测渲染与 `transparentBlack`）。
- 通用兜底改小号 Burst emoji + accent 扩散环（B-14）。

**本轮简化项（诚实记录，留待后续统一调整）**
- L2 精确钩子未做真实指针/数字变化：VU/PhaseCorr 拨针为幽灵指针叠加、LUFS 数字滚动为 actor 层、DR 压扁为 actor 动画（模块 `paintContent` 未加 `toyNeedleOffsetRad/toyTextOverride/toyDigitScaleY` 钩子）。
- Crest Line Rider 为"下落 + 左移"近似（未沿真实曲线斜率）；Waveform 人浪为高亮带（未沿真实包络）。
- LUFS "连点 3 次 777 彩蛋"、TruePeak "叠 3 层错位 + TOO MANY WINDOWS"、Spectrogram "拖动连续盖章"、拍立得"同屏最多 3 张"、EQ 熨斗（P2）、S3D L 档——均未做。
- `ToyModuleFx.cpp` 未按 §11.1 拆分为 `fx/ToyFx_*.cpp`（文件组织，不影响功能）。

**验证**：Release Standalone 编译 + LTO 链接通过；`Y2Kmeter_toy_test` 全绿。

---

## 附录 F：全量测试用例（逐项验收清单）

> 覆盖 R1（M0/M1/M3/M4）+ R2（P0 修复/基础设施/画布 2.0/模块数据玩具化）的全部开发内容。
> 用法：按编号逐条实测，把问题和修改意见直接写在该行的「修改意见」列；一行一条意见，多行可复制该行编号在表后追加。
> 状态建议标记：✅ 通过 / ❌ 不符 / ⚠️ 有差异但可接受。
> 默认测试档位 **NORMAL**，主题 **blackPink**（涉及主题/档位的条目除外）。

### F-A 画布（表情喷泉 · 画布 2.0）

| 编号 | 操作步骤 | 预想效果 | 修改意见 |
|---|---|---|---|
| A1 | 画布空白处**单击**（注意：按下即触发，不是松开） | 当帧从鼠标位置向上呈扇形（±35°）喷出 NORMAL 档 7~14 个当前主题池 emoji；带重力下落、随机旋转；碰到模块标题栏上沿反弹（每颗最多 2 次，仅在该模块 x 范围内）；落地瞬间有纵向挤压回弹（scaleY 0.8→1）；最终掉出窗口底部销毁 |只看到了模块标题栏上沿反弹，落地瞬间纵向挤压回弹没看到，不知道你描述的效果是什么样的|
| A2 | 画布空白处**按住不放**并慢慢移动鼠标 2 秒 | 首次爆发后，约 0.35 秒起每 70ms 喷 1~2 个**小号** emoji，喷口跟随鼠标移动（花洒）；按住越久初速越大（×1.0→×1.5 封顶）；松开立即停止 | 没有问题|
| A3 | NORMAL 档，快速甩动鼠标**扫过**空中的 emoji | 鼠标 48px×ui 半径内的 emoji 被沿鼠标移动方向推开，鼠标越快推力越大（有上限）；CHILL 档应无此效果 | 没有问题|
| A4 | 直接**点击**空中正在飞行的 emoji | 该 emoji 爆成 4~6 个 Burst 角色小 emoji（当前主题 duotone 色）四散；邻近 emoji 被冲开；本次点击不再触发喷发（不会同时喷一簇） | 没有问题|
| A5 | 1.2 秒内连续快速点击画布 5 / 10 / 20 次 | 喷口形态逐级变化：5 连击起变宽扇形（±70°）→ 10 连击起上下双向对喷 → 20 连击起 360° 环形爆发 | 连续点击会触发添加新模块弹窗，修改一下逻辑，不要双击触发添加新模块弹窗了|
| A6 | 观察 emoji 碰左右窗口边缘 | 左右墙各反弹一次（弹性 0.6），再次越界才销毁；底部出界直接销毁；顶部不限 | 没有在碰到的时候立刻反弹，而是已经飞出窗口边缘了，然后才反弹|

### F-B 全局系统

| 编号 | 操作步骤 | 预想效果 | 修改意见 |
|---|---|---|---|
| B1 | 1.2 秒内快速点击任意可互动位置 5 / 10 / 20 / 40 次 | 分别在点击处浮现上浮字幕 **NICE! / COOL! / WOW! / Y2K MODE**（约 1.2 秒淡出）；40 连击额外触发 🎉 彩带雨（30 个从窗口顶部落下，PARTY 档彩带为彩虹光晕）+ 宠物 angry 动画 | 40连击触发的彩带雨可以更夸张一些，尤其是party模式，现在掉下来的emoji太少了。|
| B2 | 3 秒内连续点击标题栏**标题文字** 5 次 | 全屏故障约 1.5 秒：accent/accent2 色水平撕裂条（闪烁 ≤3Hz）+ "Y2K BUG!!" 大字 RGB 错位（错位两色取主题 accent/accent2）→ 结尾冒泡 "JUST KIDDING :)" | 这个效果没意思，而且双击抬头会触发最大化软件，操作逻辑撞了，删除吧|
| B3 | 点击底部工具栏 **FPS 标签** | FPS 标签约 1.5 秒显示 "9999.9 FPS (LOL)"，之后恢复真实 FPS 读数 | 没意思，删除吧|
| B4 | 累计点击达阈值（NORMAL/CHILL 40 次、PARTY 25 次），距上次出现 ≥10 分钟 | 窗口右下角出现 📎 Clippo + 随机文案气泡（"PSST... THAT KICK IS HOT" / "NICE POKING!" 等 5 句之一）；**点击 Clippo** 它飞走消失（约 1.2 秒向上抛物线飞出） | 没意思，删除吧|
| B5 | ① NORMAL 档点击画布喷发一次；② 1.2 秒内连击达 20；③ 连击达 40 | VirtuPet 宠物分别播放 **startled / yell / angry** 动画（startled 冷却 8 秒，短时间内连续喷发只触发一次）；宠物饥饿/健康/运动状态不变 | 1.2秒内点20下太难了，因为现在双击还是会触发添加模块，等修删除掉这个逻辑之后我再测试|

### F-C M3 GL 模块

| 编号 | 模块 | 操作步骤 | 预想效果 | 修改意见 |
|---|---|---|---|---|
| C1 | Oscilloscope | 点击示波器屏幕区（顶部按钮条之外） | 约 1.2 秒"拍电视"：120ms 白闪 → 自上而下滚动暗带 → 4 条水平撕裂线 → 60 个雪花点；左下角真实读数（如 "440Hz  RMS -18.2dB"，无信号时 "NO SIGNAL"）；雪花 glow 色 / 撕裂线 accent 色 / 暗带 shadow 色 | 不好玩，感觉没意思，改为跟OscilloscopeWave模块一样的效果，打印一张当前波形或者散点图的快照然后飞下去。|
| C2 | Spectrum | 点击频谱曲线区（顶部 26px 工具栏之外） | 点击处 accent 色同心环涟漪扩散（约 0.8 秒）+ 气泡 "A4 +12c 440Hz -23.5dB"（音名/音分/频率/电平；Slope 打开时 dB 已扣补偿；电平与该模块显示一致，误差 ≤0.5dB）；气泡 1.6 秒上浮淡出 | 气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| C3 | Waveform | 点击波形显示区（顶部工具栏、右侧 GAIN 面板之外） | 高亮带 + Cheer 角色 🙌 从点击处向两侧传播（约 1.4 秒）+ 气泡时间读数 "≈-1.8s"（最旧在左、最新在右） | 气泡只显示了点击位置的时间参数，应该加上点击的坐标的电平参数，然后也是有气泡超出模块边界就不显示的问题|
| C4 | StereoField | 点击半圆雷达区 | 声呐环扩散 + 气泡 "PAN L32°"（按点击位置反算声像角，含 L/R 方向）；Foe 角色（👾 或主题替代）从点击处**在雷达内**步进移动（每 0.5 秒一步）、逐行下降；**点击入侵者**可击落（爆 Burst 火花）；10 秒未击落则落到基线显示 "LANDED" 后淡出 | 气泡中的文字只写一个“Pan L32”角度就行，现在后边还有一个“DEG”，然后同样也有气泡超过模块边界不显示的问题，Foe角色emoji的运动方向增加随机参数，现在全是点击生成后立刻向右移动。|
| C5 | OscilloscopeWave | 点击波形区（顶部工具栏之外） | 📸 快门白闪 → 相纸（当前真实波形缩略折线，accent/accent2 色）**掉出模块**并继续掉出窗口；相纸下沿图注 "L+R RMS -18.2dB  PK -6.4dB  ~220Hz"，静音时 "SILENCE"；约 5 秒消失；模块为浮动态时相纸裁剪在模块内 | 相纸不要下方的图注字了，改为纯当前波形的快照，然后相纸的比例可以改变一下，把波形放大一些，让用户可以看清当前的快照，掉落的时候慢一点飘出去，增加一个保护防止用户连续多次点击|
| C6 | Spectrogram | 点击瀑布显示区（右侧滑条之外） | Pin 角色贴纸（🚩 或主题替代）+ 气泡 "1.2kHz -41dB"；贴纸随瀑布向左滚动，滚出左边界消失（最长 12 秒） | 气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| C7 | Spectrogram3D | 点击 3D 山脉区（右侧滑条之外） | 🌋 火山 + Flame 角色（🔥）余烬粒子 + Pin 旗帜沿深度斜线缓慢漂远淡出（约 3 秒）+ 气泡频率读数 "1.2kHz" | 不要气泡频率读数了|
| C8a | Phase | 点击 Gonio（李萨如图）区 | 声呐环 + 气泡 "M +0.42  S -0.10"（按点击位置反算中/侧分量，与点云实际映射一致） | 没什么问题|
| C8b | Phase | 点击表盘区 | 幽灵指针从当前 corr 对应角度来回摆动（**真实指针不动**，约 2.5 秒）+ 气泡 "CORR 0.42" | 幽灵指针的效果不好玩，改成指针出现一些虚影吧（就像眼花了一样）|
| C8c | Phase | 点击 WIDTH / BAL 显示条 | 暂无专属互动，应走 F-E1 通用兜底（Burst 小 emoji + 扩散环） | 没什么问题|

### F-D M2 CPU 模块

| 编号 | 模块 | 操作步骤 | 预想效果 | 修改意见 |
|---|---|---|---|---|
| D1 | Loudness | 点击任一柱（M / S / I / L / R） | 该柱当前颜色的方块碎片（8 个）下落弹跳 + 气泡真实读数（M/S/I 柱 "xx.x LUFS"、L/R 柱 "xx.x dBFS"） | 没什么问题|
| D2a | VU | 点击右上角 LED 灯 | 迪斯科灯 3 秒：颜色按 ok→warn→danger→accent 循环（每秒换色 ≤3 次）+ 环绕 ✨；气泡阈值文案（"SIGNAL > -25 dBFS" / "CLIP >= 0" / "NO SIGNAL"） | 迪斯科灯的效果没出来，现在显示的是一个方块色块变色，并不是led灯颜色变化，环绕的emoji字符有点奇怪删掉，好像还发出了一些小细线，细线中心应该是灯，环绕的效果应该是发射原点不变，现在是发射原点在动|
| D2b | VU | 点击表盘空白处 | glow 色玻璃裂（5~7 条放射裂纹，4 秒淡出）+ 气泡 "VU -6.2"（真实合成电平） | 气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| D3 | LUFS Realtime | 点击读数框（LUFS-M / S / I 之一） | 🎰 老虎机滚轮约 2 秒：前 0.6 秒数字随机滚动、之后停在**点击瞬间的真实值**；同时一个 warn 色萤火虫光点飞出；气泡 "vs -14: +2.1"（与 -14 LUFS 的差值） | 删除气泡|
| D4 | TruePeak | 点击读数框 | Win95 风格弹窗约 2.2 秒（标题 "Y2K TRUE PEAK"），内容如实三态：**≥-1.0 dBTP** ⚠️ "INTER-SAMPLE PEAK +0.4 dBTP" / **-6~-1** ⚠️ "CLOSE TO CEILING -2.3 dBTP" / **<-6** ✔️ "ALL CLEAR -9.1 dBTP"；OK 按钮可点击提前关闭；电平正常时**不得**出现警告文案（娱乐性提示，非限幅判定） | 删除气泡，小弹窗超出模块边界的部分不显示了|
| D5 | Phase Correlation | 点击半圆表盘 | 幽灵指针在上半圆摆动（约 2.5 秒）+ 气泡按点击角度反算的相关度与判语：**≥0.7** "MONO SAFE"（Clear 角色）/ **0.2~0.7** "WIDE" / **<0** "OUT OF PHASE"（Alert 角色） | 删除幽灵指针的效果，气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| D6a | Phase Balance | 点击 WIDTH 条 | 橡皮筋填充拉到点击处后弹簧回弹（约 1.5 秒，有回弹过冲）+ 气泡 "WIDTH 80%"（真实 width 值） | 气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| D6b | Phase Balance | 点击 BAL 条 | Orb 角色弹珠从点击处沿槽滚动、撞壁反弹（最多 3 次反弹后消失）+ 气泡 "BAL L12" / "BAL R5" / "BAL CENTER"（真实 balance 值） | 气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| D7 | Dynamics Meters | 点击任一根柱（PK L / PK R / RMS L / RMS R） | Reward 角色（💰 或主题替代）从柱顶弹起做抛物线后落回（约 1.8 秒）+ 气泡 "PK L -6.2"（柱名 + 真实值）；**无**无意义的 "x3" 字样 | 删除无意义的"x3" 字样，气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| D8 | Dynamics DR | 点击读数框（DR SHORT / DR INTEG / CREST） | 压扁动画（横向挤压 + 回弹过冲，约 1.2 秒）+ "SQUISH" 字样 + 气泡白话判语（按该框真实值）：**≥14** "DYNAMIC"（Cheer 角色）/ **10~14** "BALANCED" / **6~10** "COMPRESSED" / **<6** "SQUASHED"（Burst 角色） | 压扁动画的效果不好玩，改为一个简单的emoji弹出，气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| D9 | Dynamics Crest | 点击曲线显示区（右侧滑条之外） | Orb 角色小球（⚽ 或主题替代）从点击处下落并向左漂移（约 3.5 秒）+ 气泡 "CF 11.2 dB"（点击处 crest 值反算） | 气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|
| D10 | Dynamics 综合 | 分别点击左（Meters）/ 中（DR）/ 右（Crest）三区 | 三区分别复用 D7 / D8 / D9 的效果 + 气泡 "PK / RMS" / "DYNAMIC RANGE" / "CREST" | 同子模块一样的问题|
| D11 | EQ | 点击拼豆格区 | Burst 角色（🍿 或主题替代）从点击处弹起 + 邻近 4 颗豆晃动（约 1.2 秒）+ 气泡 "1.2 kHz"（列反算频率） | 气泡最左边的emoji被文字盖住了，把emoji移到文字左边，同时放大一点气泡边框大小，气泡超过模块边界的部分不显示了|

### F-E 通用兜底与排除

| 编号 | 操作步骤 | 预想效果 | 修改意见 |
|---|---|---|---|
| E1 | 点击任一模块内容区内、未命中上述任何专属元素的位置 | 3~5 个小号 Burst 角色 emoji 上抛淡出 + 一圈 accent 色扩散环（约 0.9 秒） | 没什么问题|
| E2 | ① 点击 VirtuPet / Milkdrop 模块；② 拖动滑条 / 点击按钮 / 操作下拉框；③ 教程覆盖层显示期间点击任意位置 | 均不触发任何 Toybox 互动（宠物只响应 B5 联动动画；Milkdrop 整块排除；控件正常完成自身功能不被劫持） | 没什么问题|

### F-F 主题与档位

| 编号 | 操作步骤 | 预想效果 | 修改意见 |
|---|---|---|---|
| F1 | 切换主题（13 个任选，含灰度 voidGrey / paperGrey），**无需重启**，再点击画布或任一模块 | 互动 emoji 按该主题角色表变化（blackPink Reward=💎、tangerinePop Reward=🍹、starlight Foe=👽 等）；emoji 带该主题 duotone 着色（灰度主题下接近纯灰度、无彩色刺眼元素）；气泡/裂纹/灯光等非 emoji 颜色随 ToyPalette 变化；功能性图标（🎰📸📎⚠️）不换形状只跟着色 | 没什么问题|
| F2 | 点击底部工具栏 **TOY 按钮**循环切到 Off / Chill / Normal / Party，逐档点画布 | **OFF**：所有互动完全静默；**CHILL**：喷泉 5~8 个、无鼠标力场、Clippo 阈值 40；**NORMAL**：喷泉 7~14 个、力场开启、Clippo 阈值 40；**PARTY**：喷泉 10~20 个、emoji 带彩虹光晕、Clippo 阈值 25 | 没什么问题|
| F3 | 设为 PARTY 后关闭应用重新打开 | 工具栏 TOY 显示 PARTY（Standalone 存 `ui.toyLevel`；插件宿主存工程状态 `PBEQ_State toyLevel`） | 没什么问题|

### F-G 工程验收（回归项）

| 编号 | 操作步骤 | 预想效果 | 修改意见 |
|---|---|---|---|
| G1 | 应用启动约 1 秒后第一次点击画布 | 无超过 16ms 的可感知卡帧（emoji 图集在消息线程分帧预烘焙，paint 线程不做烘焙，首次点击不触发烘焙卡顿） | 软件现在掉帧比之前严重很多，不过可以后边再优化|
| G2 | 喷泉进行中拖拽缩放窗口 | emoji 在窗口新增区域正常显示；左右墙反弹位置随新窗口边界；悬浮窗口形态下其模块内 actor 裁剪正常 | 没什么问题|
| G3 | 观察 emoji 下落穿过空白区域 | 仅在有模块标题栏的 x 范围内反弹；没有模块的 x 范围直接穿过下落（无"空气墙"） | 没什么问题|
| G4 | 全部用例跑完，观察桌面/日志 | 桌面不出现 `toy_emoji_log.txt`；无崩溃、无挂起；所有 actor 最终销毁不残留 | 没什么问题|

### 追加意见（超出单行空间的意见在此按编号补充）

> A1:aqua pearl和black pink颜色预设下，软件内的emoji图标的重力是反的，所有emoji向上飞出，需要修复
> A2:所有的气泡下边都有一个小细线，感觉是想连接气泡和鼠标点击的位置，但是它现在会跟着气泡一样向上飞出，删除这个细线吧

### 第 7 轮 · 反馈处理记录（全部落地，编译通过）

| 编号 | 反馈摘要 | 处理 |
|---|---|---|
| A1 | 落地纵向挤压回弹看不到 | squash 动画幅度 0.2 太小、衰减过快。改为 scaleY 0.65、衰减 5/s，经 `drawEmoji(scaleY)` 渲染，肉眼可见 |
| A5 | 连点触发"添加模块"弹窗 | `ModuleWorkspace::mouseDoubleClick`：Toybox 档位非 Off 时禁用双击弹菜单（右键菜单入口保留） |
| A6 | 左右墙反弹滞后（飞出边缘才反弹） | 反弹判定由 `±drawSize` 余量改为贴窗口边界（0 / w），命中即反弹并钳回 |
| B1 | 40 连击彩带雨太少 | 数量 30 → 60（PARTY 120）；横向初速随机、大小三档 |
| B2/B3/B4 | Y2K BUG / FPS 玩笑 / Clippo 删除 | 全部删除（触发逻辑、actor、工厂、`titleTextBoundsFn` 接线、Clippo 档位阈值） |
| C1 | 拍电视改为拍立得快照 | Oscilloscope 改为 PolaroidActor（当前真实 L/R 波形快照，`promoteToFree` 飞下），1.5s 防连点冷却 |
| C2/C3/C6/D2b/D5/D6a/D6b/D7/D8/D9/D11 | 气泡：emoji 被文字盖住、边框太小、超出模块边界、尾巴细线 | `ReadoutBubbleActor` 重做：**删除尾巴线**；emoji 左、文字右互不遮挡；高度 18→22；整体钳制在模块内容区内（`ctx.bubble` 传入 panel content bounds） |
| C3 | Waveform 气泡加电平参数 | 新增只读 `toyReadColumnPeak(x)`，气泡 "≈-1.8s  -12.3dB"，静音区 "SILENCE" |
| C4 | 气泡只写 "PAN L32"；入侵者方向随机 | 气泡去 "deg"；InvaderActor 初始移动方向随机化 |
| C5 | 相纸去图注、波形放大、慢飘、防连点 | PolaroidActor 重做：纯快照无图注；L/R 分上下半各占相纸、波形放大；重力 700→220 慢速飘落 + 横向漂移；1.5s 冷却 |
| C7 | 删气泡频率读数 | 已删（仅保留火山+余烬+旗帜） |
| C8b | 幽灵指针改"眼花"虚影 | GhostNeedleActor 重做：主指针 + 4 组随年龄向外扩散的偏移残影 |
| D2a | 迪斯科灯：LED 变色没出来、删环绕 emoji/细线、发射原点不动 | DiscoLedActor 重做：LED 本体变色（椭圆 + 高亮描边）+ 从 LED 中心扩散的 3 圈光晕环；无 emoji、无移动轨迹 |
| D3/D4 | LUFS / TruePeak 删气泡 | 已删；TruePeak 弹窗位置钳制在读数框区内（超界部分不显示） |
| D5 | PhaseCorr 删幽灵指针 | 已删，仅保留判语气泡 |
| D7 | DynamicsMeters 删 "x3" | CoinPopActor 已删 "x3" 字样 |
| D8 | DR 压扁动画改 emoji 弹出 | SquashActor 重做为 Burst 角色 emoji 弹起落下（判语气泡保留） |
| 追加 A1 | aqua pearl / black pink 主题下 emoji 向上飞 | 根因：桌面纹理人格 `bubbles/hearts` 设 `fall=false`。喷泉统一强制 `fall=true`（上浮人格仅保留给桌面纹理） |
| 追加 A2 | 气泡尾巴细线 | 已随 ReadoutBubbleActor 重做删除 |
| G1 | 掉帧比之前严重 | 记录，**留待后续专项优化**（本轮不改） |
| B5 | 连击 20/40 待 A5 修复后重测 | A5 已修复，可重新测试 |

### 第 8 轮 · 第二轮反馈

| 编号 | 反馈摘要 | 处理结果 |
|---|---|---|
| A1 | 气泡文字显示不全（VU 显示 "VU -6." 无小数；StereoField 右侧不显示角度） | ① 气泡宽度改为按 `Font::getStringWidthFloat` 真实像素计算（原来按字符数×7 估算，11px 加粗字实际更宽被截断）；② VU 气泡加单位 "VU -6.2 dB"、DynamicsMeters 加 " dB"；③ StereoField 修正：正上方 ±2° 显示 "PAN CENTER"（原来显示 "PAN R0"），其余 "PAN L32"/"PAN R45" |
| A2 | Oscilloscope X-Y / Liss 模式快照应为散点图 | PolaroidActor 增加 scatter 模式：XY/Lissajous 下画 (L→x, R→y) 李萨如散点（每 2 样本一点），波形模式仍画折线 |
| A3 | Phase corr 表针虚影角度错误（朝下）；PhaseCorr 模块也要同样虚影 | 根因：GhostNeedleActor 原来用屏幕坐标角（sin 向下），Phase 传的基准角公式错误。重构：GhostNeedleActor 改用与真实指针一致的**数学角度约定**（0=最左、π/2=正上、π=最右），Phase 按 drawCorrDial 的 thetaFor(corr) 传角、几何（圆心/半径）完全复刻；PhaseCorr 模块加回同样虚影（真实 corr 角度） |
| A4 | Spectrogram 贴纸左移一抖一抖 | StickerActor 滚动速度改一阶低通平滑（`expSmooth` 吸收 dt 抖动）+ 位置改用浮点 `drawEmoji` 绘制（原来整数取整每帧跳变） |
| A5 | 喷泉初速太快、重力太大，头晕 | 峰值高度 18%..38% → **12%..24%**；重力统一 ×0.75（主喷泉与持续喷） |
| A6 | VU 不要碎屏，改表针虚影 | VU 表盘空白处点击改为指针虚影（pivot/半径按 drawDial 同约定，theta = vuToAngle+π/2），玻璃裂移除；气泡保留 "VU -6.2 dB" |
| A7 | 同 A5 | 同 A5 |
| A8 | PhaseCorr 虚影轴位置偏上、未与真实表针重合 | 根因：toyHitTest 的 rect 已是 content.reduced(6)，toyPoke 里又 reduced(10)，比 paintContent 的 reduced(10) 多缩 6px → 圆心被抬高。改为 reduced(4)，几何与 paintContent 完全一致 |
| A9 | TOY 按钮移到 Stick 左边；TOY↔Stick、Stick↔Load 之间各画一条分割线 | 工具栏布局调整：TOY 从 FPS 左侧移到 stick 按钮组左侧；新增 `toolbarDividerXToy` / `toolbarDividerXStick` 两条分隔线（与现有分隔线同视觉）；stick 隐藏/极简态下分隔线自动不画 |

### 第 9 轮 · 性能优化第 1 轮（卡顿专项）

**性能日志基础设施（新增 `source/ui/toys/ToyPerf.h`）**
- 日志写入 **exe 目录** `toy_perf_log.txt`（不写桌面），每次软件启动追加一条 session 分隔行，供多轮优化比对。
- 打点内容：每秒一行 `frame: fps=.. freeActors=.. moduleActors=.. frameMs=..`；快照烘焙耗时（`osc snapshot (xy)` / `oscwave snapshot`）；喷泉粒子堆积告警（freeActors>80 时每秒一条）；图集预烘焙完成行。
- 线程安全（CriticalSection），低频写入，开销可忽略。

**优化项（针对用户报告的两个严重卡顿）**
| 编号 | 卡顿场景 | 根因 | 优化 |
|---|---|---|---|
| P1 | 大量 emoji 掉帧 ~20fps | 每粒子每帧 `drawImageTransformed` 逐像素旋转插值 | **旋转角度分桶**：`EmojiAtlas::rotated()` 把 spin 量化到 12 桶（30°），首次按需烘焙（主题 duotone 版），paint 改为整数坐标 `drawImage` blit；挤压/无旋转才走慢路径 |
| P2 | Oscilloscope 快照（尤其 X-Y/Liss）拖垮帧率 | PolaroidActor 的 paint 每帧重建：散点 1024 次 `fillRect`+`setColour` / 波形 2×2048 点 Path | **快照一次性烘焙**：构造时把相纸（白底+边框+波形/散点）烘焙到离屏 Image，paint 每帧仅 1 次整数 blit |
| P3 | 每帧遍历模块刷新平台 | 平台几何每帧重建（模块不动也重建） | **平台缓存 0.5s**：`platformCacheTime_` 节流，模块移动后 0.5s 内收敛 |
| P4 | 持续点击粒子无限堆积 | 无粒子上限 | **软上限**：freeActors 超 Normal 150 / Party 200 后，新喷发数量按 `limit/(alive+limit)` 比例削减 |
| — | toyPoke 数据拷贝 | juce::Array 逐个 add 2048 次 | `ensureStorageAllocated` 预分配 |

**验证方式**：运行 exe 后按用户报告的两组操作复现，读取 exe 目录 `toy_perf_log.txt`，对比 session 间 `fps` / `frameMs` / `freeActors` 变化。

### 第 9 轮紧急修复（点击画布卡死）

**现象**：打开软件后点击未覆盖模块的空白区域，UI 完全卡死（消息循环停摆），只能结束进程。

**排查与修复**（本轮优化引入，按风险从高到低逐层消除）：
1. **paint 回调内烘焙 + 引用悬空**：原 `rotated()` 在 paint 中 miss 时执行 `juce::Graphics`+`drawImageAt` 烘焙，且返回 `unordered_map` 内部引用（rehash 后悬空）。→ 重构为 **paint 零烘焙**：`rotatedCached()` 只做无锁快查 + **值传递**，miss 直接走慢路径；烘焙全部移到 **step 阶段** `ensureRotated()`。
2. **D2D 工厂并发风险**：`ensureRotated` 若触发 `bakeOne`（D2D 是 `SINGLE_THREADED` 工厂，仅消息线程 timer 可独占使用）会造成并发死锁。→ 加 `isBaked()` 守卫：原图未烘焙完成时**绝不触发烘焙**；paint 慢路径同样守卫，未烘焙该帧跳过。
3. **旋转烘焙路径不可控**：`juce::Graphics` + `addTransform(rotation)` + `drawImageAt` 的软件渲染路径不在可控范围内。→ 改为**自写最邻近采样旋转**（固定 w×w 次循环 + `BitmapData` 读写，24~48px 小图百微秒级），从机制上杜绝渲染层死循环。
4. 附：rotCache 加 4096 容量上限（超出整体清空，防内存失控）。

**结果**：编译 + LTO 链接通过、单测全绿。待用户复测画布点击与 X-Y 快照两个场景。

### 第 9 轮 · 性能优化第 2 轮（日志定位 + 渲染瓶颈专项）

**日志分析结论（用户实测 toy_perf_log.txt）**
- `frameMs` 全程 0.01~0.18ms → **CPU 物理/step 几乎免费**，瓶颈全在渲染。
- fps 随粒子数线性崩塌：14→35fps、62→30fps、127→22fps、164→17fps、166→10.6fps。
- 根因：`onFrame` 把所有粒子 bbox **union 成一个大 dirty** 再 `overlay->repaint` —— 粒子散布全屏时每帧重绘大半个窗口的透明 overlay（连带底层组件重绘）。
- 启动初期 fps 跌到 31：预烘焙 timer 60Hz × 每 tick 8 个 D2D 烘焙抢帧（烘焙完成后回升）。
- 快照烘焙本身仅 1~3ms（一次性），无问题。

**本轮优化**
| 编号 | 优化 | 说明 |
|---|---|---|
| P5 | 每粒子独立 repaint | 去掉 union 大 dirty，每个粒子只 repaint 自己 prev+cur 小块（JUCE 消息循环自动合并重叠），重绘面积与粒子实际覆盖成正比 |
| P6 | 粒子堆积渲染降级 | freeActors > 120 时 S 档粒子改画 4px 色点（`fillRect`），消除 170×24px blit 压力，视觉仍是彩色粒子雨 |
| P7 | 预烘焙放缓 | timer 60Hz→30Hz、每 tick 烘焙 8→3 个，启动期抢帧更平缓 |

**验证方式**：同一组操作复现后对比两轮日志：持续点击时 fps 应明显高于上一轮（166 粒子 10.6fps → 目标 >30fps）；启动后 2 秒内 fps 波动减小。

### 第 9 轮 · 性能优化第 3 轮（P5 反噬回退 + 降级组合）

**现象**：用户自编译版"稍微多一些 emoji 帧率就崩"，远不如上一轮。

**根因**：P5"每粒子独立 repaint"方向错误——粒子多时每帧产生数百次 `repaint()` 调用（每次遍历组件父链 + 消息循环处理海量小 invalid region），实测比 union 单次重绘的开销大得多。

**修复（P5 回退 + 组合降级）**
| 编号 | 措施 | 说明 |
|---|---|---|
| P8 | 回退 P5 | 恢复 union 单次 `repaint (dirty)`（与上一轮行为一致） |
| P6v2 | 色点降级两档 | >80 粒子：S 档画 4px 色点；>160 粒子：M 档画 5px 色点（只有 L 档保持 emoji blit） |
| P9 | 动画降频 | 粒子数 >100 时自由粒子每 2 帧 step 一次（半帧率动画，视觉仍流畅，step+repaint 压力减半）；跳帧路径保持 overlay 存活 |
| — | 构造期日志 + IO 防御 | 上轮已加：`[toy]` 前缀构造日志、性能日志 session 行延迟到首帧、appendText 失败静默（启动黑屏问题随之解决） |

**验证方式**：持续点击堆积 80~170 粒子，对比 toy_perf_log.txt 的 fps：P6v2+P9 组合下 100 粒子应 ≥45fps、170 粒子应 ≥30fps。

### 第 9 轮 · 性能优化第 4 轮（日志精读 + 残余瓶颈）

**日志分析结论（第三轮复测）**
- P6v2+P9 生效：高帧率段（120Hz+ 限制）82 粒子 145fps、157 粒子 147fps、173 粒子 150fps——粒子绘制本身已不是瓶颈。
- 残余瓶颈 1：**粒子散布时 union 大 dirty 底层重绘**——60Hz 段 48~150 粒子仅 22~30fps；高帧率段粒子集中在点击处（dirty 小）所以快。
- 残余瓶颈 2：**frameMs 尖峰 2~4ms**——主题切换清空旋转分桶缓存后，step 里 `ensureRotated` 一帧烘焙多张。

**本轮优化**
| 编号 | 优化 | 说明 |
|---|---|---|
| P10 | tile 分块 repaint | 粒子 prev+cur 累积进 `RectangleList`（自动合并），按 128px tile 切分，仅 repaint 与粒子相交的 tile——重绘面积与粒子总覆盖成正比，不再全屏重绘 |
| P11 | 分桶烘焙节流 | `ensureRotated` 每 16ms 窗口最多烘焙 2 张，消除主题切换/粒子风暴时单帧多张烘焙的 frameMs 尖峰 |
| P9v2 | 降频阈值 100→80 | 80 粒子以上即进入半帧率动画 |

**验证方式**：60Hz 限制下，粒子点击位置分散（全屏各处）堆积 100+ 粒子，目标 fps 从 22~30 提升到 ≥40；主题切换后 frameMs 尖峰应消失。

### 第 9 轮紧急修复（连续点击崩溃）

**现象**：连续点击空白时程序异常报错，堆栈停在 `ToyDirector::tryPopEmoji` 的 `std::exchange`（vector erase 内部）。

**根因**：`tryPopEmoji` 点爆命中后**先 push_back 爆裂火花、再 erase 命中粒子**——push_back 触发 vector 扩容使迭代器悬空，随后 `erase(it)` 解引用悬空迭代器崩溃。连续点击时点爆（A4）命中即触发。

**修复**：调整顺序为「邻近冲量（只读）→ erase 命中粒子 → 再 push_back 火花」，并注释说明约束。同时排查了其余 `push_back`/`erase` 共存的循环路径（onFrame 的 sprayTick 在循环外、mouseDown 的 onClick 消费 push 的是另一个容器），无同类隐患。

### 第 9 轮 · 性能优化第 5 轮（残余掉帧）

**日志分析（第四轮复测）**
- 整体大幅好转：350 粒子 32~40fps（此前 166 粒子 10.6fps）；50~150 粒子场景 53~84fps。
- 残余 1：粒子**无限堆积**——软上限只削减新增数量，持续点击仍堆到 351 个。
- 残余 2：tile 分块在粒子遍布全屏时退化（128px tile 铺满 2560×1366 约 220 块 → 数百次 repaint，重蹈 P5 覆辙）。

**本轮优化**
| 编号 | 优化 | 说明 |
|---|---|---|
| P12 | 粒子硬上限 250 | `onFrame` 裁剪：超过 250 按 age 保留最新 250 个（每帧 sort + erase），彻底杜绝无限堆积 |
| P13 | tile 退化回退 | tile 数 >80（粒子遍布全屏）时回退 union 单次 repaint；局部聚集仍走 tile 分块 |

**验证方式**：持续点击把粒子堆到极限，fps 应稳定在 45~60（不再因堆积无限下跌）；分散点击与集中点击的 fps 差距应缩小。

### 第 9 轮 · 性能优化第 6 轮（FIFO 淘汰 + 显示层算法）

**用户诉求**：① 每次点击喷发量恒定（不要"越点越少"的软上限削减）；② 超限时删除**最先创建**的粒子；③ 显示层继续挖算法空间，尽可能容纳更多粒子。

**改动**
| 编号 | 内容 | 说明 |
|---|---|---|
| P4 删除 | 移除 `spawnFountain` 软上限 | 每次点击喷发量恒定（档位 + 能量 + 蓄力），堆积控制交给 FIFO 硬上限 |
| P12v2 | FIFO 硬上限 400 | 修正上一版方向性错误（原实现 sort 降序删尾部 = **删最新留最老**，与预期相反）；改为 `nth_element`（O(n)）按 age 升序分区，保留最新 400 个，删除最老 |
| P14 | 旋转分桶 step 缓存 | `EmojiParticle` 记住上次 bucket，bucket 不变时零调用 `ensureRotated`（省掉每帧每粒子的锁 + hash 查找）；`EmojiAtlas::bucketOf` 纯函数无锁 |
| P15 | 1:1 blit 快路径 | 目标尺寸 == 源图尺寸时 `drawImage` 改 `drawImageAt`（无缩放路径，跳过缩放参数处理） |
| — | 上限 250 → 400 | 配合色点降级 + 半帧率动画，容纳更多粒子 |

**验证方式**：持续点击到 400 粒子封顶——每次点击喷发数量应保持不变（约 7~14 个/NORMAL），最老的粒子先消失、最新的保留；fps 观察是否仍 ≥30；若 400 仍卡可下调上限（单常量 `kFreeActorLimit`）。

### 第 9 轮 · 性能优化第 7 轮（多模块布局掉帧定位）

**日志分析（用户"增加更多模块后特别卡"）**
- 空闲（freeActors=0，frameMs=0.00）从 79~145fps 跌到 **47fps** —— Toybox 零开销，掉帧来自**模块渲染本身**（布局模块越多，每帧模块 paint + GL 渲染越贵），非 Toybox 责任。
- 粒子场景放大效应：同样 41 粒子，多模块布局 126fps → **18.3fps**（7 倍）。机制：粒子 repaint 的 tile 会触发底下模块区域重绘，模块越多每个 tile 的重绘成本越高，粒子 × 模块双重放大。

**Toybox 侧响应（阈值前移，压缩重绘频率）**
| 编号 | 改动 | 说明 |
|---|---|---|
| P9v3 | 动画降频阈值 80 → 40 | 粒子 >40 即半帧率动画，重绘频率尽早减半 |
| P6v3 | 色点降级阈值 80/160 → 40/120 | 提前色点化，减少 overlay 自身 blit |

**根本建议（超出 Toybox 范围）**：多模块布局的空闲 47fps 是应用层模块渲染成本（GL 模块尤甚），建议减少同时展示的模块数量、或把 FPS 限制调低（工具栏 FPS 按钮 60 档），Toybox 侧无法替模块买单。