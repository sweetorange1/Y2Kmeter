# Lessons Learned (v6~v22 一句话教训)

> 本文档为 [SKILL.md](../SKILL.md) §7 之外的教训索引。每条对应 [`docs/MilkDrop3_Integration.md`](../../../MilkDrop3_Integration.md) 附录 D / E 中的一轮或多轮记录。

---

## 编译期教训（v6 ~ v14）

- **MSVC x64 严格模式拒绝 K&R 风格 `for(i=0;...)`** —— 上游 milkdrop2 源自 VC6 时代。所有隐式 `int i;` 都要显式补声明。
- **`__asm` NSEEL 代码在 x64 不可用** —— `asm-nseel-x86-msvc.c` 必须用 `#ifdef _M_IX86 ... #else DECL_STUB(fn)` 包裹，为 x64 提供 stub。
- **DirectXMath 相比 D3DX9 数学库有 API 差异** —— 引擎侧 `D3DXVec3` 等已被替换或保留 D3DX9 依赖。**不要**在 v20 之后再动这些替换点。
- **`_CRT_SECURE_NO_WARNINGS` 只在引擎 target 上定义** —— Y2Kmeter 侧勿开启，否则会掩盖真实的过时 API 使用。

## D3D9 窗口层级教训（v10 ~ v18）

- **`WS_CHILD` HWND 会被 JUCE 顶层绘制盖住** —— 已否决。
- **unowned `WS_POPUP` 会脱离主窗口拖动** —— 已否决。
- **`HWND_TOP` / `WS_EX_TOOLWINDOW` 不能改变 popup 与 JUCE 的层级** —— 已否决。
- **`SWP_FRAMECHANGED` 每帧调用会增加 DWM 负担** —— 只在尺寸真正变化时调用。
- **唯一有效组合**：owned `WS_POPUP` + PMv2 + `logicalToPhysical`。见铁律 2/3。

## DPI 与坐标教训（v15 ~ v19）

- **进程 PMv2 后 Win32 API 严格区分逻辑与物理像素** —— 这是 v15 前后偏移量从固定变成"随距离缩放"的直接原因。
- **`GetDpiForWindow(parent_hwnd)` 与 JUCE displays 缩放不一定一致** —— 特别是跨屏时。**必须**用 JUCE 的 `logicalToPhysical`。
- **调试日志的 `MonitorFromPoint + GetDpiForMonitor` 曾误导 18 轮** —— 因为它报告的是"父窗口所在监视器的 DPI"，而目标窗口可能在另一屏。禁止再引入这类"只写日志"的探测。

## 音频接入教训（v20）

- **`Milkdrop3Module` 曾 14 个版本未调用 `addFrameListener(this)`** —— 引擎一直用零向量渲染，视觉与音频完全脱钩。修复只需一行。
- **`AnalyzeNewSound` 是引擎侧唯一合理的注入点** —— `time_to_frequency_domain` 之后短路 FFT 并 memcpy 外部谱。
- **consume-once 语义必须严格** —— 消费后立即置 `m_bY2kExternalSpectrumValid = false`，否则后续帧继续用陈旧谱。

## 交互层教训（v20 ~ v21）

- **右键处理会与 workspace 的"添加模块菜单"冲突** —— milkdrop3 模块**只用左键**。
- **`PostMessage(GetParent(hwnd), ...)` 转发偶发失效** —— 直接在 popup 侧 `MessageManager::callAsync` 处理。
- **`enterModalState` 会锁死软件全局交互** —— 用 in-place 面板替代。
- **独立子组件浮层会被 D3D9 HWND 遮挡** —— 用引擎 `m_bShowPresetInfo` + `LaunchSongTitleAnim`。

## 死锁教训（v21 四轮 + v22）

- **`getDefaultSizeForType` 内 `factory(milkdrop3)` 拿实例算尺寸 → 死锁** —— 尺寸表必须静态。
- **`getHoverPreviewImage` 内渲染真实模块 → 死锁** —— 预览用静态占位图。
- **`renderOpenGL` 内 `measureMax` 之类的 GDI 字体调用 → GL 线程堆锁 vs 主线程堆锁** —— 字体测量结果必须缓存到主线程。
- **`Editor` 构造函数末尾同步 `openGLContext.attachTo(*this)` → 初始化死锁** —— 改成 `attachOpenGLContext()` + `MessageManager::callAsync`，让 Editor 构造完成、消息队列跑起来后再启动 GL 线程。

## 启动序列与加载器锁教训（v22）

- **“setVisible(true) 卡死”的堆栈地址 `0x00007ffc*d9f8` 是 `ntdll!LdrLockLoaderLock`**。一旦看到这个地址，不要去看 JUCE 自己的 setVisible / toFront 实现，也不要去怀疑 milkdrop3 自身——它一定是启动时主线程与 audio 线程为 `LoaderLock` 开战。
- **”预热 TimerThread”不能治本**。TimerThreadBoot 自己也会加载 DLL，反而因为提前启动了一个内部线程而加剧锁竞争。
- **进程级 DPI awareness 只能用 `SetProcessDpiAwarenessContext`，不可用线程级**。`SetThreadDpiAwarenessContext` 会在 audio/DWM 线程上与主线程产生 DPI 上下文不一致，比直接卡死更难排查。
- **“mainWindow 提前创建’以”预建 HWND”）”不会避开锁竞争**。`addToDesktop` 只会把 HWND 提前创建；真正危险的是 `setVisible(true) → toFront → SetForegroundWindow` 路径，它只会在”真正显示“那一瞬间才走。因此正确策略是把 `addToDesktop + setVisible(true)` **一起放到 initialise 末尾**，而不是分开。
- **不要“演化式重构”启动序列**。即使你只需要追加一行 `SetProcessDpiAwarenessContext`，也必须把它插到 initialise 首行，而**不变动** `pluginHolder / mainWindow / addToDesktop / setVisible` 四者的相对顺序与致密相邻关系。任何其它“优化”（如提前创建、分裂 setVisible、引入 workaround）都属于销毁性变更。

- **`Api::Initialize()`（单大函数）→ `Initialize_XX` 五段** —— 便于按需重试或复用。
- **`Api::CreateRenderWindow` 单句包装** —— 无价值，已删除。
- **`Api::SetPresetDir` 外部调用不生效** —— 引擎路径由 `Initialize_PreInit` 内固定，已删除。
- **`CycleRenderScale / ApplyRenderScale / render_scale_`** —— 从未在 UI 层暴露入口，已删除。
- **`hub_retained_` 冗余布尔** —— RAII + retain/release 已足够，已删除。
- **`MD3_BUILD_TAG` / `Md3BuildTagLogger` / `#pragma message`** —— 排障期噪音，已删除。

---

## v23（本轮：控件栏 z-order / 主题解耦 / 预设跳转 / JUCE 窗口误改）

- **控件栏 overlay 在与 D3D 交互后不可见** —— 根因是首次修复时把 `SetWindowPos` 的 `hWndInsertAfter` 传成了 `d3d_child_hwnd_`，Win32 语义是"置于其下"，把 overlay 塞到 D3D 之下了。正确做法：`HWND_TOP`，或依赖 owned popup 的"后创建的自动更靠上" z-order。
- **抬头颜色不应跟随主软件调色板** —— 该模块的定位是"黑色视频输出区上叠加控件"，与主软件预设色系解耦，`UpdateThemeColors()` 里全部用固定灰阶（`RGB(0x1E,0x1E,0x1E) / 0x33/0x66/0xCC/0xF0`）。**教训**：模块级样式应能"独立于全局主题存在"，避免和主题系统间接耦合。
- **单行 EDIT 中 Enter 键要在 `WM_KEYDOWN` 处理** —— 不能依赖 `WM_CHAR`。这条本轮反复踩坑三次，最终把 `VK_RETURN` 和 `VK_ESCAPE` 一起搬到 `WM_KEYDOWN` 里才稳定。
- **`Milkdrop3Module::paint` 里访问基类 `private` 成员** —— `closeButtonPressed` / `closeButtonHovered` 是 `ModulePanel::private`，子类不可用。做自定义标题栏时首选**保留基类默认 paint**，只 override `paintContent` / `paintOverChildren`。
- **改 `juce_Windowing_windows.cpp` 是禁区** —— 本轮用户报 D3D9 popup 偏移，AI 一度去改 UWPUIViewSettings / DwmSetWindowAttribute / renderer 后端，结果把全软件外框搞出黑边、标题栏文字变成小圆圈、右侧边不对称。**JUCE 是共享资源**：milkdrop3 相关 DPI 问题只能在 milkdrop3 内部用 `logicalToPhysical` 解决，不要碰 JUCE。
- **块作用域局部变量不能跨块使用** —— 复制"Go 按钮"代码得到"Cancel 按钮"时，两块的 `HPEN oldPn` / `HBRUSH oldBr2` 是各自 `if` 内的局部变量，跨块直接引用会 C2065。写完复用代码后必须重命名或提到外层块。
- **成员函数中的 `PaintJumpDialog` 类内定义要小心括号平衡** —— 一个多余的 `}` 就能让方法体脱离类作用域，导致后续所有成员变量"未声明"。写完大段绘制代码后必须用 `read_file` 从起始 `void PaintJumpDialog(...)` 到结束 `}` 逐段核对。
- **AI 不要在编译验证上试超时** —— 本轮末期 AI 反复 `cmd //c` 各种路径转义都失败，浪费了两轮。正确做法：一旦发现命令行编译不稳定（Git Bash 里 `cmd //c` 的 Windows 路径转义常炸），立刻把编译交回用户在 CLion 里做，AI 只做**静态自检**（`read_lints` + `check_init_sequence.py` + `check_forbidden_patterns.py`）。

---

## 附：如何快速定位教训详情

搜索项目根 `docs/MilkDrop3_Integration.md`：
- 附录 D：所有轮次总览（D.1 表格）+ 最终修复记录（D.2）+ v20 改动（D.3）。
- 附录 E：死锁问题的四轮排查记录（E.1 ~ E.3）。

搜索关键词示例：
```
grep -n "轮次\|Round\|Attempt\|试错" docs/MilkDrop3_Integration.md
```
