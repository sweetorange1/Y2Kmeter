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

- **事件驱动不可靠时加兜底**：mouseEnter/Exit 可能因焦点切换/多屏/DPI 变化丢失，叠加 timer 轮询、多层监听。
- **异步事件需要计时器窗口保护**：`setBounds()` 产生的异步 WM_SIZE/mouse 事件在回调返回后才抵达，同步 `bool` 挡不住，用计数器 + timer 递减覆盖窗口期。
- **头文件合并绕过 MSVC include-guard 串扰**：多类合并到同一头（`AnalyserHub.h`、`ModuleWorkspace.h`），不要拆散（详见 PROJECT_OVERVIEW.md §6.1）。
- **音频线程约束**：`pushStereo`/`processBlock`/`registerLoopbackRenderTime` 必须无锁/无堆分配/无系统调用（详见 PROJECT_OVERVIEW.md §6.3）。

## 5. 全量构建 / 枚举 / 类布局

- 枚举重编号、类布局变更、头文件时间戳未变（git 分支切换）都要求全量构建（详见 build-toolchain.md §5）。

## 6. 版本号同步

- 升版本要同步 CMakeLists（3 处）+ `.iss` + `PluginEditor.cpp`（3 处字面量）+ README 徽章 + 文档（详见 build-toolchain.md §3）。
