# 通用工程教训（跨主题）

> 这些是从多轮开发中反复出现、跨模块通用、且对未来改动仍有指导意义的规则。

## 1. Debug 与断言

- **`jassert` 只在 Debug 生效**（Release 里 `jassertfalse` 是 no-op）。"Release 正常、Debug 启动 int3 崩溃"是典型"假冒的正常"—— 一定要偶尔跑 Debug 暴露逻辑错误。
- 遇"Debug 启动就 int3、Release 正常"，第一反应是 grep JUCE 源码那一行的 `jassert`（多是"字段互斥/边界"没满足）。

## 2. 资源与初始化

- **资源"存在 ≠ 有效"**：判空要基于内容有效性（子文件数、扩展名匹配），不是目录存在性。旧版本残留空目录会屏蔽新内置资源（升级软件后 `%APPDATA%\Y2Kmeter\` 残留空目录同样翻车）。
- **共享大资源 seed 一次到 AppData**：三端（Standalone/VST3/AU）读共享 AppData；只装插件不装主程序时资源不会自动 seed（需文档/installer 提示）。
- **磁盘 IO / 扫盘与 GL 线程关键路径解耦**：无 GL 依赖的准备工作（扫盘、读小配置）上移到主线程构造函数，别在 `newOpenGLContextCreated` 里做 9000+ 文件遍历。
- **空资源就地兜底**：热路径依赖资源数组的随机访问先判空返回默认，不崩（用户宁看"不动的宠物"也不要"打开就崩"）。

## 3. 模块排他与状态

- **模块排他性判断要含脱离态缓存**：模块脱离后从 `modules` 数组移入 floatingCache，判断"是否已存在某类模块"必须同时看两个来源，否则单例资源（GL context/dylib handle）打架。
- 状态位变更需同步 UI：关键 flag 的 setter 应同时刷新依赖它的 UI 文案（`setChromeVisible` 的 early return 会吞掉文案刷新）。

## 4. 事件驱动与线程

- **事件驱动不可靠时加兜底**：mouseEnter/Exit 可能因焦点切换/多屏/DPI 变化丢失，叠加 timer 轮询、多层订阅。
- **异步事件需要计时器窗口保护**：`setBounds()` 产生的异步 WM_SIZE/mouse 事件在回调返回后才抵达，同步 `bool` 挡不住，用计数器 + timer 递减覆盖窗口期。
- **浮动态（pop-out）模块 GL 化与软件回退限流（v3.5.0 更新）**：v3.5.0 起非 MilkDrop 浮动态模块均托管独立 `OpenGLContext` + `GlDirectRenderer`（`floatingGlHosted_ == true`），不限流且外观/帧率与嵌入态完全统一；仅当 GL 不可用退回消息线程软件光栅化时，`ModulePanel::repaintFrameContent / repaintHover` 仍保持 30Hz 限流以防高频鼠标拖拽打满消息线程。新写跟随鼠标的绘制仍应统一走 `repaintHover()` 而非直接 `repaint()`。
- **绝不在正在执行的 `std::function` 回调里自覆写该回调或同步销毁顶层窗口（v3.5.0）**：如果 `panel->onPopOutClicked()` 或 `window->onDock_()` 内部立刻重新赋值同一 `std::function` 或 `erase` 销毁窗口，小对象优化（SBO）缓冲会被当场覆盖，回调返回析构局部捕获即触发 UAF / `0xc0000409 / abort()`；同时在 `WM_LBUTTONUP` 栈帧内同步销毁当前 `HWND` 也会让 JUCE `ComponentPeer` 悬空。规范：`mouseUp` / `closeButtonPressed` 先将回调按值拷贝到栈再调用；脱离窗口的停靠/关闭一律通过 `MessageManager::callAsync` + `SafePointer` 延迟到事件返回后执行，且对重复触发保持幂等。
- **跨 GL 上下文模块严禁持有 `OpenGLImageType` 缓存（v3.5.0）**：模块脱离/停靠会在 Editor 与 `FloatingModuleWindow` 两个独立 `OpenGLContext` 间迁移，`OpenGLImageType` 持有裸 `OpenGLContext*`，跨上下文析构或读写必炸。所有模块离屏回退图一律固定 `SoftwareImageType`。
- **不要给非平凡 UI 状态切换函数标注 `noexcept`**：`ModulePanel::setFloating` / `GlDirectModuleBase::setFloating` 等方法涉及组件状态/回调/资源切换，标记 `noexcept` 会让任何异常瞬间升级为 `std::terminate() → abort()`（无法被诊断层捕获）。
- **节流必须带"最后一帧补齐"**：只按时间窗口丢弃重绘，拖拽停下的瞬间会丢掉最后一次事件，UI 定格在上一帧位置（悬停十字线尤其明显）。做法是累积区域 + 一次 `juce::Timer::callAfterDelay` 补发（见 `ModulePanel::repaintHover`）。
- **头文件合并避免 MSVC include-guard 串扰**：多类合并到同一头（`AnalyserHub.h`、`ModuleWorkspace.h`），不要拆散（详见 PROJECT_OVERVIEW.md §6.1）。
- **音频线程约束**：`pushStereo`/`processBlock`/`registerLoopbackRenderTime` 必须无锁/无堆分配/无系统调用（详见 PROJECT_OVERVIEW.md §6.3）。

## 5. 全量构建 / 枚举 / 类布局

- 枚举重编号、类布局变更、头文件时间戳未变（git 分支切换）都要求全量构建（详见 build-toolchain.md §5）。

## 6. 版本号同步

- 升版本要同步 CMakeLists（3 处）+ `.iss` + `PluginEditor.cpp`（3 处字面量）+ README 徽章 + 文档（详见 build-toolchain.md §3）。

## 7. 文本与字体

- **JUCE 的 `getBounds()` 是"行框"不是"墨迹"**：`GlyphArrangement::getBoundingBox()` / `PositionedGlyph::getBounds()` 返回 `advance 宽 × (ascent + descent)`。用它做垂直居中等于按行框居中 —— `_` 永远贴底、`*` 永远顶天。要真实墨迹必须 `PositionedGlyph::createPath(path)` 后取 `path.getBounds()`。
- **像素级手调偏移只在"单一字体"前提下成立**：`translate(0, -1)` 这类补偿是按某一套字体调的，换字体/换皮肤就变成新的偏差。跨字体统一走实测字形带（`PinkXP::FontOptics`，见 rendering-gl.md §5）。
- **"默认皮肤零回归"的做法**：把基准字体（Silkscreen）的现行视觉位置当成 0 点，其它字体向它对齐，而不是给所有字体（含默认）重新定一套绝对值。
