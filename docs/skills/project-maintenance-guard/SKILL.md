---
name: project-maintenance-guard
description: |
  Y2Kmeter 项目的通用工程维护 Skill。用于指导对主工程代码、构建系统、Standalone 外壳、分析层、UI 框架层、模块层、资源分发与版本文件的日常维护。
  包含项目知识速览、关键约束、构建验证流程、增量/全量编译判断、以及常见缓存与工具链排错经验。
  触发场景：修改 CMakeLists.txt、PluginProcessor/PluginEditor、source/analysis、source/ui、source/standalone、安装脚本、资源拷贝逻辑、主题系统、模块注册与布局持久化等。
license: internal
compatibility: |
  Windows 10/11 x64；Visual Studio 2026 (MSVC 14.51)；CMake ≥ 3.22；
  CLion 或命令行 Git Bash / cmd；默认验证构建类型 RelWithDebInfo；默认生成器 NMake Makefiles。
metadata:
  author: y2kmeter-team
  version: "1.0.0"
  data-classification: internal
  audit-level: medium
  scope:
    project: Y2Kmeter
    module: general
---

# Project Maintenance Guard

Y2Kmeter 项目的通用工程维护 Skill。它不绑定某一个模块，而是帮助 AI 在修改主工程时快速理解项目结构、避开历史坑，并以一致的方法完成构建验证。

## 1. 触发条件

命中以下任一条件时，应该优先参考本 Skill：

- 修改 `CMakeLists.txt`、`CMakePresets.json`、安装脚本、资源复制逻辑。
- 修改 `PluginProcessor.*`、`PluginEditor.*`、`source/analysis/**`、`source/ui/**`、`source/standalone/**`。
- 新增 / 删除模块类型，或改动 `ModuleWorkspace`、模块工厂、布局持久化。
- 调整主题系统、`PinkXP` 配色、顶层窗口行为、Loopback 音频接入。
- 处理编译缓存导致的“代码改了但行为没变”、MSVC / CMake / JUCE 工具链问题。
- 需要在不污染用户 CLion 构建目录的前提下完成独立验证构建。

## 2. 前置检查

开始修改前，优先确认以下事实：

1. 项目使用 **C++17**，并遵循 JUCE + CMake 的现有组织方式。
2. `Y2Kmeter` 同时存在插件态与 Standalone 态，改动顶层窗口、Loopback、标题栏、OpenGL 上下文时必须区分两种运行形态。
3. `source/analysis/AnalyserHub.h` 与 `source/ui/ModuleWorkspace.h` 存在“多类合并进同一头”的历史约定，不要轻易拆分。
4. 头文件、CMake、宏、模板、内联函数发生变更时，不能只依赖增量构建结果。
5. Windows 下默认依赖 VS 2026 + MSVC 14.51 + 静态 CRT；调试构建与发布构建的 CRT 组合不同。

详细项目知识见 [references/project-knowledge.md](references/project-knowledge.md)。

## 3. 推荐执行流程

1. 先读项目知识与约束，确认改动是否会影响 `Processor / Editor / Workspace / Standalone` 之间的数据流。
2. 优先在现有文件内修改，保持目录结构、命名风格与职责边界不扩散。
3. 若涉及模块注册、布局 XML、资源分发或版本信息，做成成套修改，不留半套状态。
4. 代码改完后，先做静态检查，再做独立构建验证。
5. 若修改涉及头文件 / CMake / 构建参数，按“全量构建 + 必要时清理 CLion 构建目录”的规则处理。

## 4. 核心维护规则

### 4.1 结构与架构规则

- `Y2KmeterAudioProcessor` 是分析入口；`Y2KmeterAudioProcessorEditor` 是 UI 壳；`AnalyserHub` 是分析调度中心；`ModuleWorkspace` 承担模块容器与布局持久化。
- 新增模块类型时，不要只改工厂，还要同步检查：枚举、字符串映射、默认布局、可用列表、持久化与构建源文件列表。
- Standalone 音频采集通过 `WasapiLoopbackCapture`（Windows）或 `MacDesktopAudioCapture`（macOS）走独立路径，不经过宿主 `processBlock`。

### 4.2 代码与工程约束

- 不要拆散为规避 MSVC include-guard 串扰而合并的头文件，尤其是 `AnalyserHub.h` 与 `ModuleWorkspace.h`。
- `Processor` 与 `Editor` 内对大成员使用 pimpl / 前向声明时，不要把实现细节重新泄漏回头文件。
- `processBlock`、`pushStereo`、`registerLoopbackRenderTime` 这类音频线程路径，保持无锁、无重型分配、无系统调用。
- `juce::OpenGLContext` 若作为成员存在，应保持既有生命周期顺序：构造后挂载、析构前先分离。
- 不要随意修改 `BUNDLE_ID = cn.iisaacbeats.Y2Kmeter`，否则会影响用户宿主内已有插件实例识别。

### 4.3 构建与缓存规则

- 只改 `.cpp` 且 `CMakeCache.txt` 已存在时，可做增量验证构建。
- 改了 `.h`、`CMakeLists.txt`、宏、模板、内联函数、目标链接选项时，必须做全量验证构建。
- 若用户反馈“验证构建通过但实际运行还是旧行为”，优先考虑 CLion 构建缓存残留，而不是先怀疑源码没生效。

详细说明见 [references/compile-verify.md](references/compile-verify.md)。

## 5. 构建验证

本 Skill 保留了已验证过的完整编译经验，但已改写为模块无关：

- 完整构建脚本：`scripts/build_skill_verify.bat`
- 增量构建脚本：`scripts/_incremental_build.bat`
- 后台完整构建启动器：`scripts/_bg_build.bat`
- 后台增量构建启动器：`scripts/_bg_incremental_build.bat`

默认日志文件：`build/skill-verify/_build_log.txt`

### 5.1 何时用哪种构建

- **只改文档**：跳过构建。
- **只改 `.cpp`**：优先增量构建。
- **改 `.h` / `CMakeLists.txt` / 构建参数**：必须完整构建。

### 5.2 为什么保留独立构建目录

验证构建始终写到 `build/skill-verify/`，与用户 CLion 的构建目录隔离，避免污染本地 IDE 缓存。

## 6. 常见问题

- 改头文件后行为没变：清理对应构建目录后重新 configure + build。
- CRT / 链接异常：优先检查 `CMAKE_MSVC_RUNTIME_LIBRARY` 与 JUCE helper target 的覆盖关系。
- 新模块加了但菜单 / 布局 / 持久化不完整：说明只改了一半注册链路。
- 顶层窗口 / 标题栏 / 全屏行为异常：优先核对 `PluginEditor` 与 Standalone 路径是否混淆。

## 7. 关联资源

- [项目知识速览](references/project-knowledge.md)
- [构建验证与排错](references/compile-verify.md)
- [后台完整构建脚本](scripts/build_skill_verify.bat)
- [后台完整启动器](scripts/_bg_build.bat)

---

**最后更新**：2026-08-06（v1.0.0：建立通用项目维护 Skill，保留完整编译经验并接入 PROJECT_OVERVIEW 的通用工程知识）