# Y2Kmeter AI 知识库（索引）

> 本文件夹是**面向 AI 助手**的项目知识库，与根目录 `PROJECT_OVERVIEW.md`（现状导航）配套。
> 两者职责划分：
> - `PROJECT_OVERVIEW.md` —— **现状**：产品是什么、架构分层、代码结构、关键类、业务流、核心约定。改动前先读它。
> - `docs/` —— **开发须知与易错点**：从历史开发过程中提炼出的、对后续开发仍有指导意义的规则 / 约束 / 已知限制 / 待办。按主题拆分，按需查阅。
>
> **阅读顺序建议**：先 `PROJECT_OVERVIEW.md` 定位"改哪个文件、调哪个类"，再按改动主题进对应文档查"有哪些坑、有哪些硬约束、有哪些未完成事项"。

## 文件清单

| 文件 | 内容 | 何时读 |
| --- | --- | --- |
| [build-toolchain.md](build-toolchain.md) | 构建目录/命令、vendored JUCE 补丁管理、版本号同步、`ModuleType` 枚举规则、全量构建触发条件、MSVC 特有坑、CRT | 加/删文件、改枚举、升级版本、升级 JUCE、遇链接/增量编译问题 |
| [rendering-gl.md](rendering-gl.md) | GL/OpenGL 渲染架构、GL Image 消费规则、Milkdrop(projectM) 避坑、shader 文本、文本绘制坑 | 改任何模块绘制、GL 上下文、Milkdrop、Spectrogram3D、着色器 |
| [window-layout.md](window-layout.md) | 窗口行为模型（L/Hide/Stick）、布局锁定、auto-hide 机制、多屏混合 DPI、窗口恢复 | 改窗口几何、拖拽、全屏、预设、多显示器 |
| [analysis-modules.md](analysis-modules.md) | 分析后端、计量口径、新模块注册清单、Tempo/Key/Recorder/StereoMap 关键设计与已知限制/待办；**§7 v3.1 模块控制面板 / 参数系统 / 模块合并映射 / 存档迁移 / 按住慢放 / FFT scale / 已知偏差** | 动分析后端、新增模块、给模块加参数、改存档键、改计量口径、续做 Stereo Map |
| [ui-features.md](ui-features.md) | VirtuPet / Toybox / 新手引导 / 悬停提示 / i18n 的约定与注意点 | 改这些子系统的行为 |
| [engineering-lessons.md](engineering-lessons.md) | 跨主题通用工程教训（Debug 校验、资源判空、线程解耦、模块排他等） | 任何改动前的通读/复查 |

## 关键事实速查（版本 / 目录）

- 当前版本：`3.1.0`（CMakeLists `project(Y2Kmeter VERSION 3.1.0)`）。
- 构建目录：Release = `cmake-build-release-visual-studio`；诊断包 RelWithDebInfo = `cmake-build-relwithdebinfo-visual-studio-ninja`。
- vendored JUCE：`third_party/JUCE`（补丁 0001~0006 已打入源码，diff 在 `third_party/juce_patches/`）。
- 未发布/开发中的模块：`Stereo Map`（`ModuleType::stereoMap`）。
