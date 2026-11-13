# Project Knowledge（项目知识速览）

> 本文件只做"索引 + 心智模型"。事实细节以 [PROJECT_OVERVIEW.md](../../../PROJECT_OVERVIEW.md) 与 [docs/](../../INDEX.md) 为准，冲突时改这里。

## 1. 项目定位

| 项 | 值 |
|---|---|
| 产品名 | `Y2Kmeter` |
| 当前版本 | `3.2.0`（版本号同步点见 SKILL.md §6） |
| 形态 | VST3 / AU / Standalone（纯分析，不产生音频输出） |
| 技术栈 | C++17、JUCE 8.0.12（vendored + 0001~0006 补丁）、CMake ≥ 3.22 |
| 平台 | Windows（静态 CRT）/ macOS |
| Bundle ID | `cn.iisaacbeats.Y2Kmeter`（**不要改**，会丢宿主里的插件实例） |

## 2. 核心分层

| 层 | 关键文件 | 职责 |
|---|---|---|
| 插件入口 | `PluginProcessor.*` | 音频回调、参数、状态持久化 |
| UI 壳 | `PluginEditor.*` | 标题栏、窗口交互、GL 合成、工作区承载 |
| 分析层 | `source/analysis/**` | `AnalyserHub`（6 路分析、引用计数、帧分发）；FFT / RMS / LUFS / 调性 / 录音等 |
| UI 框架 | `source/ui/**` | `ModuleWorkspace`（容器 + 布局持久化）、`ModulePanel`（外壳 + 参数面板 + 悬停帮助）、`GlDirectModuleBase`（GL 直渲染基类）、`PinkXPStyle`（主题 / LookAndFeel / 悬停标尺） |
| 模块层 | `source/ui/modules/**` | 各分析模块的绘制与交互 |
| Standalone | `source/standalone/**` | Loopback 采集、平台差异、独立窗口 |

## 3. 重要架构事实

### 3.1 数据流

- 插件态：`processBlock → AnalyserHub::pushStereo → FrameSnapshot → 各模块 onFrame`
- Standalone：系统音频经 `WasapiLoopbackCapture` / `MacDesktopAudioCapture` 直接推入 `AnalyserHub`
- 模块 = `ModulePanel`（UI）+ `AnalyserHub::FrameListener`（数据）；GL 模块再加 `GlDirectModuleBase`

### 3.2 合并头（不要拆）

`source/analysis/AnalyserHub.h` 与 `source/ui/ModuleWorkspace.h` 都是**多类合并头**，历史上为规避 MSVC include-guard / 多 TU 串扰而合并。拆分要连带重构大量 include，收益低风险高。

### 3.3 两条绘制路径（改外观必看）

| 路径 | 入口 | 何时走 |
|---|---|---|
| GL 直渲染 | `buildGlSnapshot()` / `paintGlOverlay()` | 编辑器 GL 合成可用时（绝大多数情况） |
| CPU 回退 | `paintLegacyContent()` | GL 不可用 / 浮动态降级 |

两份实现必须视觉一致。改颜色、边距、刻度都要**两边都改**。

### 3.4 参数面板（v3.1+）

- 标题栏齿轮 → `ModuleControlDrawer`（覆盖式抽屉，高度上限 = 内容区 80%，内部 `Viewport` 滚动）。
- 参数声明：`buildParamDefs(std::vector<ParamDef>&)`；读：`getParamValue`；写：`setParamValue`。
- `ParamDef.id` = 存档键；`slider` 第 8 参是 **skewMid**，不是默认值。
- 抽屉"点外部自动收起"由 `ModulePanel::closeOpenControlDrawers` + 顶层 `MouseListener` 实现，同时只允许开一个。
- 详见 [analysis-modules.md §7.1](../../analysis-modules.md)。

### 3.5 按住交互（v3.1+）

`source/ui/HoldSlowMo.h`：示波器 = 慢放 50×（只拖慢回落沿），波形历史 / 语谱图 = 完全冻结；松手回实时。悬停读数在按住期间靠 `mouseDrag` 跟随。

### 3.6 GL 与顶层窗口

- `PluginEditor` 的 `juce::OpenGLContext` 有生命周期约束：构造后挂载、析构前分离。
- Standalone 与插件态的标题栏 / 全屏 / 拖拽行为不同，不能混为一谈。
- GL 直渲染**没有 scissor**：画布外几何会画到邻居模块，必须自己裁（analysis-modules §7.14）。

## 4. 常见维护点

### 4.1 新增 / 删除模块类型

同步清单：`ModuleType` 枚举（只追加）、字符串映射（旧类型保留）、工厂、默认布局 / 预设、持久化、`CMakeLists.txt` 源文件列表、性能计数映射。详见 [build-toolchain.md §4](../../build-toolchain.md)。

### 4.2 布局与持久化

- `ModuleWorkspace` 负责布局、主题、FPS 等状态；写回有 debounce / flush。
- 抽屉展开状态**不进存档**（加载后一律收起）。
- 布局锁定 / 隐藏 chrome / 窗口边界恢复相关改动，注意构造期与 `visibilityChanged` 的时序。

### 4.3 资源与打包

- 字体、图标、安装资源复制由 CMake 控制；Windows / macOS 路径不同。
- 版本号改动后 `*_resources.rc` 不会自动重生成（见 build-toolchain §1）。

### 4.4 文案与 i18n

- 所有界面英文都是翻译键，中文在 `i18n/zh-CN.lang`（逐行键）。
- 悬停说明在 `source/ui/HoverHelp.cpp`（`y2k::help::moduleHelp`），正文按 `\n` 分行。
- 改文案后要删死键、补新键，并确认键没有被其它文案复用。

## 5. 历史经验提炼

- 头文件改了但行为不变 → 先怀疑构建缓存，再怀疑代码。
- 视觉异常分三类先分类：**缓存没失效** / **两条绘制路径不一致** / **GL 越界没裁**。
- 滚动、历史类图形出现"闪烁 + 毛刺" → 先查列相位与子列偏移（analysis-modules §7.12 / §7.13）。
- 音频线程路径的改动必须最保守：无锁、无分配、无阻塞。
- Skill 与文档两套说法同时存在时，以 `docs/` 为准，并回头修 Skill。

## 6. 推荐阅读顺序

1. `PROJECT_OVERVIEW.md`（全景 + 约定 + 版本规则）
2. `docs/INDEX.md` → 按主题跳到具体文档
3. `AnalyserHub.*` → `ModuleWorkspace.*` / `ModulePanel.*`
4. 具体模块或平台代码
