---
name: project-maintenance-guard
description: |
  Y2Kmeter 项目的通用工程维护 Skill。用于指导对主工程代码、构建系统、Standalone 外壳、分析层、UI 框架层、模块层、资源分发与版本文件的日常维护。
  包含项目知识速览、关键约束、参数系统与 i18n 约定、构建验证流程、增量/全量编译判断、发版流程，以及常见缓存与工具链排错经验。
  触发场景：修改 CMakeLists.txt、PluginProcessor/PluginEditor、source/analysis、source/ui、source/standalone、安装脚本、资源拷贝逻辑、主题系统、模块注册与布局持久化、模块控制面板参数、i18n 文案、发版升版本号。
license: internal
compatibility: |
  Windows 10/11 x64；Visual Studio 2026 (MSVC 14.51)；CMake ≥ 3.22；
  CLion 或命令行 Git Bash / cmd；默认验证构建类型 RelWithDebInfo；默认生成器 NMake Makefiles。
metadata:
  author: y2kmeter-team
  version: "1.1.0"
  data-classification: internal
  audit-level: medium
  scope:
    project: Y2Kmeter
    module: general
---

# Project Maintenance Guard

Y2Kmeter 项目的通用工程维护 Skill。它不绑定某一个模块，而是帮助 AI 在修改主工程时快速理解项目结构、避开历史坑，并以一致的方法完成构建验证与发版。

## 0. 动手前先读这几份（权威来源，优先于本 Skill）

| 文档 | 何时必读 |
|---|---|
| [PROJECT_OVERVIEW.md](../../../PROJECT_OVERVIEW.md) | 任何改动前的项目全景（分层、类清单、约定、版本规则） |
| [docs/INDEX.md](../../INDEX.md) | 不知道该看哪份文档时 |
| [docs/analysis-modules.md](../../analysis-modules.md) | 动分析层 / 模块 / 参数面板 / 按住交互；**§7 是 v3.1~v3.2 的约定与已知坑** |
| [docs/build-toolchain.md](../../build-toolchain.md) | 构建、版本号、`ModuleType` 枚举、全量构建判定、MSVC 坑、CRT |
| [docs/rendering-gl.md](../../rendering-gl.md) | 动 GL 直渲染、Milkdrop、文本绘制 |

本 Skill 只保留"流程与判断"，事实性细节以上述文档为准；两边冲突时**以文档为准并顺手修正本 Skill**。

## 1. 触发条件

命中以下任一条件时，应该优先参考本 Skill：

- 修改 `CMakeLists.txt`、安装脚本、资源复制逻辑、版本号相关文件。
- 修改 `PluginProcessor.*`、`PluginEditor.*`、`source/analysis/**`、`source/ui/**`、`source/standalone/**`。
- 新增 / 删除模块类型，或改动 `ModuleWorkspace`、模块工厂、布局持久化。
- **增删改模块控制面板参数**（`buildParamDefs / getParamValue / setParamValue`、`ModuleControlDrawer`）。
- **改 i18n 文案**（`i18n/zh-CN.lang`、`y2k::help` 悬停说明、参数名 / 参数提示）。
- 改模块绘制（GL 直渲染多路径 / CPU 回退、残影通道、悬停标尺）。
- 发版：升版本号、写文档、提交推送。
- 处理编译缓存导致的“代码改了但行为没变”、MSVC / CMake / JUCE 工具链问题。

## 2. 前置检查（v3.2 现状）

1. **版本**：当前 `3.2.0`；C++17；JUCE 8.0.12 vendored（`third_party/JUCE`，带 0001~0006 补丁）。
2. **模块形态**：分析模块 = `ModulePanel` 子类；多数同时继承 `GlDirectModuleBase` + `AnalyserHub::FrameListener`。**GL 与 CPU 回退是两条绘制路径，改外观必须两边都改**（`buildGlSnapshot` / `paintGlOverlay` 与 `paintLegacyContent`）。
3. **参数系统（v3.1+）**：抽屉由 `ModulePanel::buildParamDefs` 生成，值走 `getParamValue / setParamValue`，持久化走 `save / restoreModuleSpecificState`。
4. **i18n**：界面上出现的英文都是**翻译键**，必须在 `i18n/zh-CN.lang` 补中文（逐行键，非整段）。
5. **头文件合并约定**：`AnalyserHub.h`、`ModuleWorkspace.h` 是多类合并头，**不要拆**（MSVC include-guard 串扰历史原因）。
6. **音频线程约束**：`processBlock` / `pushStereo` / loopback 推数据路径保持无锁、无动态分配、无系统调用。
7. **文件编码**：含中文的源文件必须 UTF-8 **with BOM**（否则 MSVC 按 GBK 解析报 C2xxx）。

详细项目知识见 [references/project-knowledge.md](references/project-knowledge.md)。

## 3. 推荐执行流程

1. **先搜后写**：`search_content` 找现有实现 / 同类参数 / 旧常量，不要凭空新建一套；历史实现可用 `git log -S"关键字"` 找回（例如已被删除的 Slope 开关）。
2. 在现有文件内修改，保持目录结构、命名风格与职责边界不扩散。
3. 涉及模块注册、布局 XML、资源分发、版本信息时做成**成套修改**，不留半套状态。
4. 改完先看 lints，再按 §5 选择增量 / 全量构建。
5. 同步文档与 i18n（见 §4.3），否则下一轮又会出现"文档说 A 代码是 B"。
6. 发版走 §6 的固定清单。

## 4. 核心维护规则

### 4.1 结构与架构

- `Y2KmeterAudioProcessor` 是分析入口；`Y2KmeterAudioProcessorEditor` 是 UI 壳；`AnalyserHub` 是分析调度中心（6 路分析、按引用计数启用）；`ModuleWorkspace` 是模块容器 + 布局持久化。
- 新增模块类型：`ModuleType` **只追加不重排**；同步枚举、字符串映射、工厂、默认布局、持久化、`CMakeLists.txt` 源文件列表、性能计数映射。
- 旧类型（`lufs_realtime` / `true_peak` / `oscilloscope_wave` / `phase_correlation` / `phase_balance` / `dynamics_meters` / `dynamics_dr` / `dynamics_crest`）已合并到新模块，**字符串映射必须保留**，否则旧存档模块会静默丢失。
- Standalone 采集走 `WasapiLoopbackCapture` / `MacDesktopAudioCapture`，不经过宿主 `processBlock`。

### 4.2 参数与存档（最容易踩）

- `ParamDef.id` **同时是存档键**：改名 = 破坏旧存档；确需改就走"双读旧键 + 写新键"迁移，并在 `restoreModuleSpecificState` 里用 `hasProperty` 判断（**缺键不覆盖**）。
- 新增参数的固定六步：
  1. `buildParamDefs` 加定义（`toggle / choice / slider`，分组 `.in("组名")`，可见性 `.when(lambda)`）；
  2. `getParamValue` / `setParamValue` 加分支（**setter 里夹紧范围**）；
  3. `saveModuleSpecificState` 写键、`restoreModuleSpecificState` 读键（老值迁移）；
  4. 参数会改变网格 / 缓存 / 颜色时，**让对应缓存失效**并从下一帧重建；
  5. 抽屉控件回读：`refreshControlPanel()`（外部改参数后也要调）；
  6. 文案 + 中文 + 文档 `analysis-modules.md` §7.4 参数表三处同步。
- 注意 `ParamDef::slider` 的第 8 个参数是 **skewMid（映射中点）**，不是默认值；默认值来自成员初始值。
- 滑条范围与读档 `jlimit` 范围必须一致，否则老存档会被夹到边界。

### 4.3 文案与 i18n

- 所有面向用户的英文（参数名、参数提示、悬停说明、气泡、状态文本）都要进 `i18n/zh-CN.lang`，**逐行**建键。
- `y2k::help::moduleHelp` 的正文按 `\n` 分行，建议每行一条独立语义（便于翻译与换行）。
- 删改帮助文案后，顺手删掉语言包里对应的死键并检查是否被其它文案复用（键是全局的）。
- 自检：脚本比对"帮助文案每一行 → lang 是否有译文"，以及 lang 是否出现重复键。

### 4.4 绘制相关硬约束

- **GL 直渲染没有 scissor**：顶点是模块本地坐标，渲染器只设 viewport + 平移 uniform。画布外的几何会画到邻居模块上 —— **自己裁**（参考示波器标尺环的 Liang–Barsky 裁剪）。
- **滚动类图形不要整列跳**：低频刷新时用"子列偏移（小数相位）"连续滚动，否则会出现"每动一次闪一下"（语谱图 §7.12）。
- **移动窗口上做列统计要锁绝对相位**：列边界对齐绝对样本序号，否则峰值会在相邻列之间跳 → 毛刺闪烁（Crest 历史 §7.13、示波器抽点）。
- **抽屉是子组件，而 emoji 画在 paintOverChildren / 全窗口 overlay 之上**：两处都要 `excludeClipRegion(抽屉)`，否则彩蛋盖住设置面板（§7.1）。
- **按住期间的鼠标事件只有 `mouseDrag`**：悬停标尺类模块必须重写它，否则读数停在按下位置。
- 每帧重建 GL 快照的分支，注意 `repaintFrameContent()` 的失效区域与静态层缓存（`staticLayer` / `staticLayerBounds_`）的失效条件。

### 4.5 构建缓存与全量判定

- 只改 `.cpp` 且 `CMakeCache.txt` 已存在 → 可增量验证构建。
- 改 `.h` / `CMakeLists.txt` / 宏 / 模板 / 内联函数 / 目标链接选项 / `ModuleType` 枚举序数 / 类成员布局 → **必须全量**（否则可能 `0x80000003` STATUS_BREAKPOINT）。
- 改版本号后增量构建**不会**重写 `JuceLibraryCode/*_resources.rc`，EXE/VST3 版本信息仍是旧值 —— 删 `build/*/JuceLibraryCode/*_resources.rc` 或重新 configure。
- 用户反馈"验证构建通过但运行还是旧行为"时，先怀疑 IDE 构建缓存，而不是源码没生效。

详细说明见 [references/compile-verify.md](references/compile-verify.md)。

## 5. 构建验证

- 完整构建脚本：`scripts/build_skill_verify.bat`（独立目录 `build/skill-verify/`，不污染 IDE 构建目录）
- 增量构建脚本：`scripts/_incremental_build.bat`
- 后台启动器：`scripts/_bg_build.bat` / `scripts/_bg_incremental_build.bat`
- 日志：`build/skill-verify/_build_log.txt`（轮询 `[PASS]` / `[FAIL]`）

正式 Release / 诊断包的构建命令见 [docs/build-toolchain.md](../../build-toolchain.md) §1（本 Skill 的脚本只用于"改完先验证能不能编过"）。

| 改动类型 | 处理 |
|---|---|
| 只改文档 | 跳过构建 |
| 只改 `.cpp` | 增量构建 |
| 改 `.h` / CMake / 构建参数 / 枚举序数 | 完整构建（必要时清目录重配） |

## 6. 发版流程（升版本号）

1. **版本号同步点（全部改到）**：
   - `CMakeLists.txt`：`project(Y2Kmeter VERSION x.y.z)` + 两个 `juce_add_plugin(... VERSION x.y.z)`（共 3 处）；
   - `Y2Kmeter_installer.iss`：`MyAppVersion`；
   - `PluginEditor.cpp`：`"vx.y.z"` 字面量 3 处（`getStringWidth` 含 debug/正式两变体 + `versionText` 两处）；
   - `README.md` 徽章、`PROJECT_OVERVIEW.md` §1.1 与 §6.8、`docs/INDEX.md`、`docs/build-toolchain.md` 示例。
2. **文档**：`PROJECT_OVERVIEW.md` 加本轮变更摘要；`analysis-modules.md` 的新约定 / 新坑归到 §7 对应小节。
3. **自查**：`read_lints` 全绿；`grep` 旧 API / 旧版本号无残留；lang 无缺译、无重复键。
4. **构建**：按 §5 全量验证（版本号/头文件改动 → 全量）。
5. **提交**：`git add -u` + 说明性 commit message（沿用 `vX.Y.Z：要点` 风格），推送 `origin main`。

## 7. 常见问题

| 现象 | 先查 |
|---|---|
| 改头文件后行为没变 / Release 启动崩 | 构建缓存、`ModuleType` 序数、类布局（build-toolchain §5） |
| 版本号改了但 EXE 属性还是旧的 | `*_resources.rc` 未重新生成（build-toolchain §1） |
| CRT / 链接异常 | `CMAKE_MSVC_RUNTIME_LIBRARY`、`juceaide` target 覆盖（build-toolchain §7） |
| 中文源文件报 C2xxx | 文件缺 BOM（build-toolchain §6） |
| 新模块加了但菜单 / 布局 / 存档不完整 | 注册链路只改了一半（§4.1） |
| 模块外观改了但抽屉收起时没变 | GL 与 CPU 回退两条路径没都改（§2 第 2 条） |
| 参数改了但界面没同步 | setter 没调 `refreshControlPanel()`，或缓存没失效（§4.2） |
| 彩蛋盖住设置面板 | 两处绘制都没排除抽屉区域（§4.4） |
| 滚动/历史图闪烁毛刺 | 子列偏移、列相位锁定（analysis-modules §7.12 / §7.13） |
| GL 画面画到邻居模块上 | 几何没自裁（analysis-modules §7.14） |

## 8. 关联资源

- [项目知识速览](references/project-knowledge.md)
- [构建验证与排错](references/compile-verify.md)
- [完整构建脚本](scripts/build_skill_verify.bat) · [增量](scripts/_incremental_build.bat) · [后台启动器](scripts/_bg_build.bat)

---

**最后更新**：2026-11-13（v1.1.0：对齐 v3.2.0 —— 补参数系统 / i18n / GL 绘制硬约束 / 发版流程；把事实性细节统一指向 `PROJECT_OVERVIEW.md` 与 `docs/`，避免 Skill 与文档两套说法）
