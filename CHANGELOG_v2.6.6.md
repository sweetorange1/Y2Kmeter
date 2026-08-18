# Y2Kmeter v2.6.6 开发总结

> 本文档记录 v2.6.6 版本相对 v2.6.5 的开发内容：为 Milkdrop 模块新增**简单波形（simple waveform）样式编辑面板**，通过预设文本注入实现 `wave_mode` 及样式参数运行时调整，并修复注入字段名映射 bug 与 mode stepper 交互动画。

---

## 1. 背景

v2.6.5 完成效果系统架构重构（38 个后处理效果 + 动态网格 effects 面板）后，本轮聚焦 Milkdrop 的**内置简单波形**样式调节能力。调研发现 libprojectM 4 的 C API 没有运行时修改 `wave_mode` 的接口，因此采用**预设文本注入**方案：在 `projectm_load_preset_data` 之前把覆盖值写入 `.milk` 文本，再交给 projectM 编译，实现不写磁盘、不破坏用户预设文件的样式调节。

## 2. 核心改动

### 2.1 新增 wave 样式编辑面板

- 控制栏新增 `wave` 按钮（位于 `effects` 左侧），点击展开/收起，与 auto / color / effects 面板互斥。
- 面板内容：
  - **mode stepper**：`< 模式名 >` 左右切换 16 种波形，带按下/悬停动画。
  - **7 个滑块**：X / Y / R / G / B / A / Mystery。
  - **4 个开关**：dots / thick / add / bright。
  - **Reset**：恢复默认并关闭覆盖。

### 2.2 新增 `MilkdropWaveState.h`（header-only）

- `MilkdropWaveState` 结构体：`enabled` + `mode/x/y/r/g/b/a/mystery/usedots/thick/additive/brighten` + `isNeutral()` + `reset()`。
- 16 种 `wave_mode` 显示名表 `GetWaveModeName()`。
- 预设文本注入函数 `ReplaceWaveKeyValue()` / `ApplyWaveParamsToPresetText()`。

### 2.3 预设文本注入机制

- 在 `projectm_load_preset_data` 之前，把被启用的覆盖值写入 `.milk` 文本（替换已有键值、缺失则追加），再交给 projectM 编译。
- `enabled=false` 时原样透传（零开销），全程不写磁盘。

### 2.4 字段名映射（关键坑修复）

simple waveform 的静态样式由 `.milk` 预设块 `[preset00]` 字段决定，与 per_frame 运行时变量名是两套命名：

| 面板语义参数 | 预设块真实字段 |
|---|---|
| mode | `nWaveMode` |
| X / Y | `wave_x` / `wave_y` |
| R / G / B | `wave_r` / `wave_g` / `wave_b` |
| A（不透明度） | `fWaveAlpha` |
| Mystery（形态参数） | `fWaveParam` |
| dots | `bWaveDots` |
| thick | `bWaveThick` |
| add（加性） | `bAdditiveWaves` |
| bright（提亮） | `bMaximizeWaveColor` |

### 2.5 持久化

- wave 状态全局共享，`SetMilkdropWaveState()` 写回 `Processor::setSavedMilkdropWaveState()`。
- 序列化到 host state 顶层属性：`milkdropWaveEnabled/Mode/X/Y/R/G/B/A/Mystery/Usedots/Thick/Additive/Brighten`，关闭重开软件后复原。

### 2.6 新增测试预设

- 新增 [`0000_test_single_wave.milk`](/I:/Y2KMeter/0000_test_single_wave.milk)：仅含单个 simple waveform，无自定义波形/形状干扰，供 wave 面板调参测试。

## 3. 每个参数的语义说明

| 面板参数 | 预设块字段 | 范围 | 作用与视觉效果 |
|---|---|---|---|
| mode | `nWaveMode` | 0~15 | 波形类型（切换绘制算法），视觉差异最大。0=Circle、1=Radial Blob、2=Blob2、3=Blob3、4=Derivative、5=Blob5、6=Line、7=Double Line（8~15 为 MilkDrop3 扩展，projectM 4.1 可能忽略） |
| X | `wave_x` | 0~1 | 波形中心水平位置，0=最左、1=最右 |
| Y | `wave_y` | 0~1 | 波形中心垂直位置，0=最底、1=最顶 |
| R | `wave_r` | 0~1 | 波形颜色红分量 |
| G | `wave_g` | 0~1 | 波形颜色绿分量 |
| B | `wave_b` | 0~1 | 波形颜色蓝分量 |
| A | `fWaveAlpha` | 0~∞ | 波形不透明度/亮度，0=透明（波形消失），越大越亮（预设默认常在 1~3） |
| Mys | `fWaveParam` | -1~1 | mystery 形态参数，含义随 mode 变化（团块凸起/线条偏移等） |
| dots | `bWaveDots` | 0/1 | 用离散点代替连续线条绘制 |
| thick | `bWaveThick` | 0/1 | 加粗波形线条 |
| add | `bAdditiveWaves` | 0/1 | 加性混合，多处叠加变亮发白、有发光感 |
| bright | `bMaximizeWaveColor` | 0/1 | 颜色等比放大到至少一通道为 1.0，避免颜色暗淡 |

## 4. 踩坑记录

1. **注入字段名用错导致一半参数失效**：最初注入用运行时变量名（`wave_mode`/`wave_a`/`wave_mystery`/`wave_usedots`/…），但 simple waveform 实际读取预设块字段（`nWaveMode`/`fWaveAlpha`/`fWaveParam`/`bWaveDots`/…）。巧合的是 `wave_x/y/r/g/b` 预设块同名，所以只有这几个生效——直接对应"只有 X/Y/R/G/B 有效"的现象。
2. **mode stepper 左右按钮缺少交互反馈**：原始绘制只有静态填充，未记录 pressed/hover 状态；补充 `waveModeStepperPressed_` / `waveModeStepperHover_` 后在 `mouseDown/Up/Move` 与 `paintWavePanel` 贯通三态绘制。

## 5. 版本号更新（v2.6.5 → v2.6.6）

- `CMakeLists.txt`：`project(Y2Kmeter VERSION 2.6.6 ...)`、`juce_add_plugin(... VERSION 2.6.6 ...)`。
- `Y2Kmeter_installer.iss`：`#define MyAppVersion "2.6.6"`。
- `PluginEditor.cpp`：关于页面标题/抬头 4 处 `"v2.6.5"` → `"v2.6.6"`。
- `PROJECT_OVERVIEW.md`：当前版本标识 `2.6.5` → `2.6.6`，并追加 v2.6.6 章节。
- `MACOS_ADAPTATION_DIFFS.md`：当前打包版本标识 `v2.6.5` → `v2.6.6`。

## 6. 编译建议

本次改动涉及头文件 `MilkdropWaveState.h`（新增）、`PluginProcessor.h`、`PluginEditor.h`、`MilkdropModule.h`，**发布前需执行一次 clean 全量构建**以刷新头文件依赖。
