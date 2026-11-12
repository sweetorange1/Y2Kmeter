# 分析后端 / 计量口径 / 新模块 / 已知限制与待办

## 1. 分析后端（详见 PROJECT_OVERVIEW.md §3.2 / §4.2）

- `AnalyserHub` 6 路 Kind（`Oscilloscope/Spectrum/Loudness/Phase/Dynamics/MusicAnalysis`），引用计数按需计算；`pushStereo` 只算计数 > 0 的路径。
- `MusicAnalysis`（Tempo/Key）：音频线程只降混 + 写入 SPSC 环形缓冲（131072 采样），FFT/自相关/chroma 在独立后台线程；**首次 `retain` 才建线程/缓冲**，无模块时零资源。
- **三路 FFT**（Stereo Map 引入）：主 FFT 由单路（mid）变三路（mid+L+R），复用同一 `fft` 实例 + 窗函数，**常开**（不做按需开关，否则 `fftFifoIndex` 相位错位）。`rebuildMainFft()` 与 `applyMainFftSizeChange()` 两处都要同步 resize 6 个 vector，漏一处即越界读。

## 2. 计量口径关键事实（详见 PROJECT_OVERVIEW.md §1.2 / §3.2）

- 响度 ITU-R BS.1770-4：K-weighting 双级 IIR + 400ms 动量；-70 LUFS 绝对门限 + 相对门限（二阶段）+ **静音 5s 自动重置**。
- True Peak：4× 过采样插值。
- DR：Short-term 与 Integrated 口径统一；Integrated DR 静音 5s 重置。
- VU 表底 -36 dB（LED 阈值解耦）。
- 分析前置增益 -10~+36 dB，只作用分析路径，不改透传输出。

## 3. 新模块注册清单（8 处，缺一不可）

| # | 文件 | 内容 |
| --- | --- | --- |
| ① | `source/ui/ModuleWorkspace.h` | `ModuleType` 枚举（**只追加不重排**，插在合适位置） |
| ② | 同文件 | `availableTypes` 数组追加 |
| ③ | `source/ui/ModulePanel.cpp` | `getModuleDisplayName()` 加 case |
| ④ | `source/ui/ModuleWorkspace.cpp` | `moduleTypeToString()` 加字符串 |
| ⑤ | 同文件 | `stringToModuleType()` 加映射（删除旧类型时要保留旧字符串→新类型） |
| ⑥ | `PluginEditor.cpp` | `#include` + 可用列表（顺序）+ `createModule()` 工厂 case |
| ⑦ | `source/perf/PerformanceCounterSystem.cpp` | `moduleTypeNameById()` 追加 ID（按枚举序数；删除则重编号） |
| ⑧ | `CMakeLists.txt` | 加新 `.h`/`.cpp` |

- 枚举插入中间 → 后续值 +1 → **必须全量构建**（见 build-toolchain.md §5）。
- v3.1 起新模块如需参数面板，另见 §7.11；退役类型的「旧字符串 → 存活类型 + 外观种子」登记在 `resolveRetiredModuleType`（§7.2）。
- 删除模块类时确认 `hub.retain(Kind)` / `hub.release(Kind)` 配对在新模块中完整（否则 Kind 引用计数泄漏 → 后端永久浪费算力）。
- `PerformanceCounterSystem` 仅在 `Y2K_ENABLE_PERF_COUNTERS=1` 时启用（发布版为 0）。

## 4. Tempo / Key 已知限制

1. chroma 用 8192 点 FFT：@44.1k bin 宽 5.38Hz，**低于 ~91Hz 相邻半音 <1 bin**，贝斯重的素材调性易糊；采样率越高窗越长，不自动缓解。
2. 无 beat tracking（只有周期、无拍点相位），无法判断节拍层级。
3. **半速 / 3:2 歧义**：真实拍点在 onset 包络无支撑时纯周期判据会选错层级；直方图投票只让输出稳定、不能造证据；换低频 onset 特征才有可能修。
4. 调性在真实混音（鼓/808/密集编曲）上偏弱（内部自测 4 首商用伴奏 3 首判错）。
5. 静音判定 -58 dBFS；连续 3s 无信号自动 reset 成 `--`。
6. 关系大小调纯音频不可完全消歧，靠迟滞保持稳定。

## 5. Recorder 模块

- 后端启动即常驻采集，内存滚动保留最近 60s；拖动条选 1~60s，导出"按下前 N 秒"为 WAV。
- **单段上限 1 小时**（v3.0.0 起，到点自动保存并停止）。

已知限制：
1. 采集期间切换音频设备/采样率 → 缓冲按新率重建，之前的 N 秒丢弃。
2. 环形缓冲满（采集线程被挂起 >1.3s）丢样本，无提示。
3. 导出在消息线程同步落盘（60s ≈ 23MB@48k），慢盘上最多几百 ms 停顿。
4. 滚动历史常驻内存（60s×2ch×4B ≈ 23MB@48k，96k 约 46MB）；有输入才分配。
5. 录制中移除模块/关界面会立即收尾并保存，不留半截文件。

## 6. Stereo Map（`ModuleType::stereoMap`，未发布）

**现状**：已入库但**版本未 bump、未进任何布局预设**。两个模式 PAN（按频率声像 ±1）/ MONO（切单声道相位抵消量，上下倒置）。GL 直渲染 + CPU 回退双路径。v3.1 起模式切换与 FFT scale 在控制面板里（顶部工具条与说明文字已取消），见 §7。

**待办 / 已知缺口**：
1. 三路 FFT 的 CPU 成本未测量 —— 用 `PerformanceCounterSystem`（RelWithDebInfo + `Y2K_ENABLE_PERF_COUNTERS=1`）对比「有/无 Stereo Map」的 CPU 与帧率；超标则改"仅 stereoMap 被 retain 时才跑 L/R"。
2. 未进布局预设 / stick 变体，要不要给预设位待产品定。
3. `PerformanceCounterSystem::moduleTypeNameById` 已滞后（缺 `stereoField`/`milkdrop`/`stereoMap`），只在 perf 调试构建显示 `unknown(N)`。
4. `bandCount = plot 宽度`，上限 512、下限 96（窄窗相邻频点重复取同 bin），是否改"固定 bandCount + 像素插值"待定。
5. 无右键自定义项（锁定量程/节能模式等）。
6. MONO 抵消是**逐频段峰值近似**（非逐 bin 复数比），强/弱带混时抵消量偏乐观；要精确需 Hub 逐 bin 相干接口（成本低但动 ABI）。
7. `kCancelFullScale = 0.5`（"抵消一半即满"）听感标定待实机确认，改这一个常量即可调灵敏度。

**换环境续做注意**：改了 `AnalyserHub.h` 成员布局（新增 6 vector）→ 全量重编；`stereoMap` 枚举插在中间 → `virtuPet`/`milkdrop` 值 +1 → 全量构建。

## 7. 模块控制面板与参数系统（v3.1）

v3.1 把各模块零散暴露的按钮 / 滑条统一收进「控制面板」，并合并功能重叠的模块。**纯工程重构 + 新增面板**：面板默认收起，收起时模块只剩仪表内容，旧存档与布局预设加载后外观与升级前一致（已知偏差见 7.9）。

### 7.1 控制面板框架

- **入口**：标题栏 `[齿轮][弹出][关闭]`。仅 `setControlPanelEnabled(true)` 的模块显示齿轮（不含速度/调性、录音、ProjectM、VirtuPet）。
- **抽屉**：`ModuleControlDrawer`（`source/ui/ModuleControlDrawer.h/.cpp`）首次点开时懒创建，**覆盖式**盖在内容区顶部，高度上限 = 内容区 80%（超出内部滚动）。不改变画布尺寸 —— Spectrogram 改尺寸会 `ensureGrid` 清历史，所以抽屉绝不挤占画布。
- **展开状态不进存档**，加载后一律收起；布局锁定时仍可调（不是布局操作）；浮动窗口里同样可用。
- **Toybox**：抽屉及其子控件标记 `toy.optOut`，点击不触发彩蛋；按住慢放的 `mouseDown` 也排除抽屉区域。
- **声明方式**（`ModuleWorkspace.h` 里的 `ParamDef` / `ModulePanel`）：模块覆写 `buildParamDefs / getParamValue / setParamValue`，抽屉据此生成控件（`choice` 互斥按钮、`toggle` ON/OFF、`slider` 横向滑条）。支持分组（`.in("组名")`）、条件可见（`.when(lambda)`）、悬停提示（`y2k::help::make`）。
- **`ParamDef.id` 同时是存档键，改名即破坏兼容。** 外部改了参数（`restoreModuleSpecificState` / 预设）后要调 `refreshControlPanel()`。
- 所有面板文案都进 `i18n/zh-CN.lang`（v3.1 追加 135 条，位于文件末尾的「v3.1 模块控制面板」段）。

### 7.2 模块合并映射

`ModuleType` 枚举值**只追加不重排**，退役枚举保留（`isRetiredModuleType`）。退役类型不进「添加模块」菜单，不再写出；旧存档里的退役字符串仍由 `stringToModuleType` 解析。

| 存活类型 | 吸收的旧类型 | 迁移种子（`resolveRetiredModuleType`，见 `ModuleWorkspace.cpp`） |
| --- | --- | --- |
| `loudness` | `lufs_realtime` | `style=1`（数字读数），仅 M / S / I |
| | `true_peak` | `style=1`，仅 TP + RMS |
| `oscilloscope` | `oscilloscope_wave`（含 `oscilloscope_left/right`） | `displayMode=0`（波形）、`waveChannel=2`（双）、`waveColour=1`（粉色，复刻原配色） |
| `phase` | `phase_correlation` | 仅 `itemCorr` |
| | `phase_balance` | 仅 `itemWidth` + `itemBal`，`barStyle=1`（实心） |
| `dynamics` | `dynamics_meters` | 仅 `itemPeak` + `itemRms` |
| | `dynamics_dr` | 仅 DR short / int / Crest，`style=1` |
| | `dynamics_crest` | 仅 `itemHist`，`histSpan=20` |

不合并、只加面板：`vuMeter`、`spectrum`、`waveform`、`spectrogram`、`spectrogram3d`、`stereoField`、`stereoMap`。

### 7.3 存档兼容与迁移链路

1. `stringToModuleType`（3 处解析点：布局加载、浮动模块加载、旧版浮动导入）仍返回**退役枚举**。
2. `Editor::createModule`（`PluginEditor.cpp`，**唯一漏斗**，预设表 `FullScreenTile` / stick 变体等全部经过它，无需改写）发现 `isRetiredModuleType` 时：`resolveRetiredModuleType` 换成存活类型，创建模块，再把「外观种子」交给 `restoreModuleSpecificState`。
3. 随后 workspace 用存档里该节点自带的 `<state>` 再覆盖一次。
4. **所有 `restoreModuleSpecificState` 都是「缺键 = 保持当前值」**，而构造默认值 = 存活类型自己的旧外观。所以旧存档缺哪个新键都会落回旧外观。新增参数必须沿用这一写法（Spectrum 的 `channel`、StereoMap 的 `mode` 以前是缺键即重置，现已改成缺键不覆盖）。
5. 旧键 → 新参数：
   - Oscilloscope `displayMode` 不变；`frozen` 丢弃（按未冻结加载）。
   - Spectrum `peakHold / sharp / curve / channel` 键名不变（含更早的布尔 `smooth` 迁移）；`slope` 忽略。
   - Spectrogram `displayMode` 键保留，对应面板「Sharpening」。
   - Spectrogram3D 旧 `useThemeColours`：true → `colourMode=0`（Theme），false → `colourMode=1`（Heat）；保存时仍同时写 `useThemeColours`，降级打开可还原着色。
   - Waveform `frozen` 丢弃；旧 `displaySeconds` → `pixelsPerSecond`（沿用 v3.0 前的换算）。
   - StereoMap `mode` "pan"/"cancel" 不变。
6. 降级安全：新存档只写存活类型字符串，沿用旧键名并另加新键；老版本打开不丢模块，只是外观回到默认。

### 7.4 各模块参数（`ParamDef.id` = 存档键）

| 模块 | 参数 |
| --- | --- |
| 响度 `loudness` | `style`（灯条 / 数字读数）、`itemM` `itemS` `itemI` `itemTp` `itemRms` |
| VU `vuMeter` | `dialStyle`（Classic / Dark / Minimal）、`needleStyle`（Line / Wedge / Stereo L/R）、`showLed` |
| 示波器 `oscilloscope` | `displayMode`（波形 / X-Y / 李萨如）、`waveChannel`（L / R / 双）、`waveColour`（Classic / Pink）、`bandColour`（频段染色，仅 X-Y / 李萨如）、`crossLow` `crossHigh`、`autoGain`、`showRings` |
| 频谱 `spectrum` | `peakHold`、`sharp`、`curve`（Smooth / Semi / Raw）、`channel`（Mid / Mid+Side / L+R）、`fftScale` |
| 相位 `phase` | `itemGonio` `itemCorr` `itemWidth` `itemBal`、`gonioStyle`（点阵 / 轨迹）、`barStyle`（像素格 / 实心） |
| 动态 `dynamics` | `itemPeak` `itemRms` `itemDrShort` `itemDrInt` `itemCrest` `itemHist`、`style`（大数字 / 进度条+数字）、`histSpan`（1–35 s，原先不持久化） |
| 语谱图 `spectrogram` | `direction`（左 / 右 / 上 / 下）、`speed`（5–120 px/s，原先不持久化）、`shaper`（Classic / Sharp / Sharper）、`colour`（Theme / Heat / Grey / Ice）、`fftScale` |
| 语谱图 3D `spectrogram3d` | `colourMode`（Theme / Heat / Grey / Ice）、`depthDir`（右后方 / 左后方）、`speed` |
| 波形历史 `waveform` | `colorSource`（Pitch / Loudness / Sibilance）、`sibThreshold`（-60…-10 dB，默认 -35）、`speed`、`dual`（上 L / 下 R 双声道） |
| 立体声场 `stereoField` | `dotSize`（1–6 px，默认 2）、`fadeSpeed`（0.2–4×，默认 1）、`bandColour`、`crossLow` `crossHigh` |
| 立体声频段图 `stereoMap` | `mode`（Pan / Mono check）、`fftScale` |

「Display style / 项目开关」类参数的默认值都是该模块旧外观；颜色相关的固定配色（Heat / Grey / Ice、频段染色红绿蓝）不随主题变化。

### 7.5 FFT scale（`source/analysis/FreqScale.h`）

- `y2k::fscale::{Scale, toUnit, fromUnit, fromIndex}`：log / mel / linear 三种频率轴映射，**唯一事实来源**，纯 C++。
- Hub 的 `getSpectrumMagnitudesBlended / LR / Side / ReassignedBlended` 新增末位参数 `scale`（默认 `Scale::log`，旧调用点零改动），UI 坐标换算（`freqToX` / `xToFreq` / 网格线 / 轴标签 / 悬停读数）也用同一套 —— 保证「画在哪 = 取哪里的数据」。
- **只改横轴排布，不改 FFT 尺寸**；FFT size 仍是 Hub 全局唯一，由工具栏 `FFT Size` 控制。
- 改 scale 会让频谱的平滑缓存、斜率补偿缓存、语谱图网格全部作废并重建（列对应的频率变了，旧列不可复用）。

### 7.6 按住慢放（示波器、波形历史，取代 Freeze 按钮）

- `source/ui/HoldSlowMo.h`：共享状态机，`kSlowdown = 50`（与 `SpectrumModule::bulletTimeSlowdown` 一致）、`kGhostFadeSec = 0.6`；按住时曲线混向残影色（`PinkXP::sel`，与频谱同色），**松手直接回实时**，残影色线性渐隐。频谱模块保留自己的实现（它的「慢放」只是回落沿拉长，语义不同）。
- `source/analysis/OscHistory.h`：示波器样本的长环形历史，容量 2^18（48 kHz ≈ 5.5 s，96 kHz ≈ 2.7 s）。用 `oscTotalSamples` 做差在 UI 线程接续新增样本；**只在按住期间分配并录制**，不按住零成本。慢放读指针只落后实时，超容量时被夹回最老可读位置。
- Waveform 里读指针按「被拉长 50 倍的时间」推进，喂给原有的列聚合（`pushHistorySamples`）；松手时样本游标对齐到最新位置，慢放期间未消费的样本被跳过。
- 老存档里的 `frozen=1` 丢弃，加载后为未冻结（新设计没有锁存式冻结）。

### 7.7 频段染色与齿音阈值

- `source/analysis/BandSplit.h`（`y2k::bands::BandSplitter`）：LR4 三频段分离（默认 250 Hz / 4 kHz，面板可调）→ 逐样本能量权重 → 固定红（低）/ 绿（中）/ 蓝（高）按权重混色。**仅开启 `bandColour` 时才计算**，示波器 X-Y / 李萨如与立体声场共用。状态跨帧保持，在 UI 线程分发器里算，不碰音频线程。
- `SibilanceDetector`：`kThresholdDb` 常量改为可设成员 `setThresholdDb()`（默认仍 -35 dB，迟滞 -4 dB 不变），运行时改不重置滤波器 / 包络。波形历史双声道模式下左右各用一个检测器，各自只看自己的声道。

### 7.8 订阅生命周期与共享

- **按启用项动态 `retain/release`**：相位模块只开相位图才订 `Oscilloscope`，只开相关 / 宽度 / 平衡才订 `Phase`；Waveform 只有 Pitch 着色才订 `Spectrum`。「没显示就不算」。
- 响度 / 动态 / 相位的分析计算本身未改，同一路分析结果仍被多个显示项共享；响度 / 动态的订阅（`Loudness` / `Dynamics`）也未改。
- 响度 / 动态各有的 3 份相同的颜色与格柱绘制拷贝合并为 `source/ui/modules/MeterWidgets.h`。

### 7.9 已知偏差（旧外观并非逐像素一致的项）

1. `phase_correlation` / `phase_balance` 迁移后使用相位模块的表盘与条形几何，与原独立模块版式不同（原 PhaseBalance 是整条填充 + 标记块；原 PhaseCorrelation 是占满整块的大表盘 + 16pt 读数）。数据、配色、平滑口径一致。要完全还原，需给相位模块加 `Compact` 样式（涉及约 6 处绘制路径）。
2. 控制面板收起后，原顶部工具条 / 右侧 SPEED 栏让出的空间由画布占用（画布变大，内容不变）。「画面完全一致」与「原按钮不占空间」无法同时满足，这里取后者。
3. 老存档里 `frozen=1` 的示波器 / 波形历史加载后为未冻结。
4. 响度、动态等合并模块复用了原绘制代码，但没有做逐像素对比。
5. 未做（方案中的「补充项」）：响度目标线与 LUFS-I 手动复位、VU 刻度范围、立体声频段图频带数。
6. 待核实（代码与文档原本就有出入，未改行为）：`LoudnessModule` 的 L/R 柱实为 RMS 而 `HoverHelp` 写的是 Peak；Spectrogram 的 Sharper 与 Sharp 数据源相同；`DynamicsModule` 的 Crest 历史窗口约 1 s 而悬停标尺按 4 s 标注。

### 7.10 测试与验证

- `tests/param_system_test.cpp`（CMake 目标 `Y2Kmeter_param_test`，`ctest` 可跑，不依赖 JUCE）：刻度映射互逆 / 单调 / Log 与旧公式逐点一致，三频段分离（80 Hz→低、1 kHz→中、9 kHz→高，权重和为 1，静音落中频，分频点可调）。
- v3.1 交付时已验证：Release Standalone 全量构建通过，启动 8 秒无崩溃，上述单元测试通过。**尚未做**：真实旧存档 / 预设逐一加载比对、抽屉交互手测、按住慢放手感、频段染色效果、语谱图四方向实机观感。

### 7.11 给模块加参数的步骤

1. 构造里 `setControlPanelEnabled(true)`。
2. 覆写 `buildParamDefs / getParamValue / setParamValue`，`id` 即存档键；文案走 `y2k::tr`，并补 `i18n/zh-CN.lang`。
3. `saveModuleSpecificState` 写出新键，`restoreModuleSpecificState` 用 `hasProperty` 判断后再赋值（**缺键不覆盖**），末尾调 `refreshControlPanel()`。
4. 若参数会改变画布网格 / 缓存（如语谱图方向），在 setter 里让对应缓存失效并重建快照；GL 与 legacy 两条绘制路径都要跟着改。
5. 参数默认值必须等于该模块旧外观。
