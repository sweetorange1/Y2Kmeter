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
- **点击别处自动收起**：抽屉打开时把 `DrawerDismissListener` 挂到当前窗口的 topLevel（`addMouseListener(..., true)`，窗口内任何组件的鼠标按下都会通知到），命中规则在 `ModulePanel::closeOpenControlDrawers(source, pos)`：点在抽屉内部（含子控件）或任一模块的齿轮按钮上不关，其余位置一律收起；**同时只允许开一个抽屉**（`setControlPanelOpen(true)` 会先关掉别人的）。抽屉全关时监听自动摘掉，平时零开销。
- **Toybox**：抽屉及其子控件标记 `toy.optOut`，点击不触发彩蛋；按住慢放的 `mouseDown` 也排除抽屉区域。
- **绘制层级：抽屉 > emoji**。抽屉是模块子组件，而彩蛋有两处绘制都在子组件之上 —— 模块内的 `ModulePanel::paintOverChildren` 与飞出模块的全窗口 `ToyOverlay::paint`。两处都用 `Graphics::excludeClipRegion` 扣掉 `getControlDrawerBounds()`（overlay 侧用 `getLocalPoint` 把各模块的抽屉换算到叠加层坐标），emoji / 粒子经过抽屉时自动"钻到面板后面"，不会再盖住设置界面。
- **声明方式**（`ModuleWorkspace.h` 里的 `ParamDef` / `ModulePanel`）：模块覆写 `buildParamDefs / getParamValue / setParamValue`，抽屉据此生成控件（`choice` 互斥按钮、`toggle` ON/OFF、`slider` 横向滑条）。支持分组（`.in("组名")`）、条件可见（`.when(lambda)`）、悬停提示（`y2k::help::make`）。
- **`ParamDef.id` 同时是存档键，改名即破坏兼容。** 外部改了参数（`restoreModuleSpecificState` / 预设）后要调 `refreshControlPanel()`。
- 所有面板文案都进 `i18n/zh-CN.lang`（v3.1 追加 135 条，位于文件末尾的「v3.1 模块控制面板」段）。

### 7.2 模块合并映射

`ModuleType` 枚举值**只追加不重排**，退役枚举保留（`isRetiredModuleType`）。退役类型不进「添加模块」菜单，不再写出；旧存档里的退役字符串仍由 `stringToModuleType` 解析。

| 存活类型 | 吸收的旧类型 | 迁移种子（`resolveRetiredModuleType`，见 `ModuleWorkspace.cpp`） |
| --- | --- | --- |
| `loudness` | `lufs_realtime` | `style=1`（数字读数），仅 M / S / I |
| | `true_peak` | `style=1`，仅 TP + RMS |
| `oscilloscope` | `oscilloscope_wave`（含 `oscilloscope_left/right`） | `displayMode=0`（波形）、`waveChannel=2`（双） |
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
| VU `vuMeter` | `dialStyle`（**六种表盘**：Half circle / Vintage fan / Bar scale / LED ladder / Minimal / Mirror，0..5）、`needleStyle`（Thin line / Wedge / Stereo L/R；横向式表盘里改为指示器形态：发丝 / 填充楔形 / L、R 分轨）、`showLed` |
| 示波器 `oscilloscope` | `displayMode`（波形 / X-Y / 李萨如）、`waveChannel`（L / R / 双）、`bandColour`（三路点云染色，仅 X-Y / 李萨如）、`crossLow` `crossHigh`（默认 200 / 2000 Hz）、`dotSize`（1–6 px，默认 2）、`fadeSpeed`（0.2–4×，默认 1）、`autoGain`、`showRings`（波形配色固定跟随主题 `chartRamp`；旧存档的 `waveColour` 键被忽略） |
| 频谱 `spectrum` | `peakHold`、`sharp`、`slope`（0–12 dB/oct，默认 4.5 = 旧 SLOPE 开关"开"；0 = 关；旧存档的布尔 `slope` 读档时迁移为 4.5 / 0）、`curve`（Smooth / Semi / Raw）、`channel`（Mid / Mid+Side / L+R）、`fftScale` |
| 相位 `phase` | `itemGonio` `itemCorr` `itemWidth` `itemBal`、`gonioStyle`（点阵 / 轨迹）、`barStyle`（像素格 / 实心） |
| 动态 `dynamics` | `itemPeak` `itemRms` `itemDrShort` `itemDrInt` `itemCrest` `itemHist`、`style`（大数字 / 进度条+数字）、`histSpan`（1–35 s，原先不持久化） |
| 语谱图 `spectrogram` | `direction`（左 / 右 / 上 / 下）、`speed`（5–120 px/s，原先不持久化）、`shaper`（Classic / Sharp / Sharper）、`colour`（Theme / Heat / Grey / Ice）、`fftScale` |
| 语谱图 3D `spectrogram3d` | `colourMode`（Theme / Heat / Grey / Ice）、`depthDir`（右后方 / 左后方）、`speed` |
| 波形历史 `waveform` | `colorSource`（Pitch / Loudness / Sibilance）、`sibThreshold`（-60…-10 dB，默认 -35）、`speed`、`dual`（上 L / 下 R 双声道） |
| 立体声场 `stereoField` | `dotSize`（1–6 px，默认 2）、`fadeSpeed`（0.2–4×，默认 1）、`bandColour`（三路独立轨迹）、`crossLow` `crossHigh`（默认 200 / 2000 Hz） |
| 立体声频段图 `stereoMap` | `mode`（Pan / Mono check）、`fftScale` |

「Display style / 项目开关」类参数的默认值都是该模块旧外观；颜色相关的固定配色（Heat / Grey / Ice、频段染色红绿蓝）不随主题变化。

### 7.5 FFT scale（`source/analysis/FreqScale.h`）

- `y2k::fscale::{Scale, toUnit, fromUnit, fromIndex}`：log / mel / linear 三种频率轴映射，**唯一事实来源**，纯 C++。
- Hub 的 `getSpectrumMagnitudesBlended / LR / Side / ReassignedBlended` 新增末位参数 `scale`（默认 `Scale::log`，旧调用点零改动），UI 坐标换算（`freqToX` / `xToFreq` / 网格线 / 轴标签 / 悬停读数）也用同一套 —— 保证「画在哪 = 取哪里的数据」。
- **只改横轴排布，不改 FFT 尺寸**；FFT size 仍是 Hub 全局唯一，由工具栏 `FFT Size` 控制。
- 改 scale 会让频谱的平滑缓存、斜率补偿缓存、语谱图网格全部作废并重建（列对应的频率变了，旧列不可复用）。

### 7.6 按住交互（示波器慢放 / 波形历史、语谱图冻结，取代 Freeze 按钮）

- `source/ui/HoldSlowMo.h`：共享状态机，`kSlowdown = 50`（与 `SpectrumModule::bulletTimeSlowdown` 一致）；**松手直接回实时**。慢放 / 冻结本身不改变配色（残影变色已取消，画面始终用模块正常配色）。频谱模块保留自己的实现（它的「慢放」只是回落沿拉长，语义不同）。
- **语谱图同样是「完全冻结」**：按住画布左键时 `onFrame` 只对齐 `lastFrameMs`（避免松手后 dt 攒成一大坨、一次补好几列），不采样、不推进新列；按下瞬间把 `columnAccumulator` 清零，画面当场停住。松手从当前时刻继续滚动，冻结期间的频谱不补播。
- `source/analysis/OscHistory.h`：示波器样本的长环形历史，容量 2^18（48 kHz ≈ 5.5 s，96 kHz ≈ 2.7 s）。用 `oscTotalSamples` 做差在 UI 线程接续新增样本；**只在按住期间分配并录制**，不按住零成本。慢放读指针只落后实时，超容量时被夹回最老可读位置。
- **波形历史是「完全冻结」而不是慢放**：按住期间 `onFrame` 只同步实时游标（`lastOscTotal`）、不消费任何样本；按下瞬间还会清空待写列与半列累加，保证画面当场停住。松手后从最新位置继续滚动，冻结期间的样本直接跳过（不补播）。因此波形历史不再使用 `OscHistory`（长历史只服务示波器）。
- **示波器抽点必须按绝对样本索引对齐**（`snapshotBaseIndex`）：波形折线是「每 stride 个样本取 1 点」画的（stride ≈ 窗口样本数 / 画布宽度，2048 px 窗口下约 5）。慢放时窗口每帧只前进十几个样本，若按「窗口第 0 个样本」开始抽点，抽到的样本每帧换一批，同一波形被抽成两三种不同形状 —— 现场表现为「大量毛刺 + 两个状态来回闪」。把抽点集合锁到样本流上（`(base + i) % stride == 0`）后只剩平移，形状不再跳。X-Y / 李萨如走全点散点，不受影响。
- **悬停读数里的音名**：语谱图在冻结时、频谱在按住（子弹时间）时，读数末尾追加鼠标所在频率的音名（`PinkXP::noteNameForHz`，十二平均律 A4 = 440 Hz）。不按住时语谱图不显示音名 —— 画面在滚动，同一点的数值会一直变，这是和波形历史保持一致的口径。三个模块的悬停都重写了 `mouseDrag`，按住拖动时十字线与读数持续跟随鼠标（JUCE 在按住期间只派发 `mouseDrag`）。
- 老存档里的 `frozen=1` 丢弃，加载后为未冻结（新设计没有锁存式冻结）。

### 7.7 频段染色与齿音阈值

- `source/analysis/BandSplit.h`（`y2k::bands::BandSplitter`）：LR4 三频段分离（默认 200 Hz / 2 kHz，面板可调）→ **输出三路独立的 L/R 信号**（低 = LP(f1)、中 = HP(f1)→LP(f2)、高 = HP(f2)，三路互斥）。显示层不再按能量混色，而是像 MiniMeters Stereometer 那样**每路画一层纯色点云**：低 = 红 `#E60045`、中 = 蓝紫 `#665AFF`、高 = 青 `#00FFFF`。**仅开启 `bandColour` 时才计算**，示波器 X-Y / 李萨如与立体声场共用；窗口式无状态调用，在 UI 线程分发器里算，不碰音频线程。**每次把染色开关从关拨到开，两个分频点都会复位成默认的 200 / 2000 Hz**（避免上次调过的分频点让三色分布看起来"不对"）；读档不触发复位，仍按存档值恢复。
- 三路点云的残影：示波器散点用 1 层持久 trail（点大小 / 消失速度可调）；立体声场 `trailLayers = 3`，每层独立 FBO（GL）或独立离屏图（CPU 回退），颜色不会互相污染。
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

### 7.12 语谱图的低速滚动（子列偏移）

滚动速度 = `pixelsPerSecond`（px/s），而新列只在「累计够一整列（`cellPx` 像素）」时才写入。低速度时一列要好几百毫秒才凑够，若画面只在写列那一帧整列跳一次，视觉上就是「每动一下闪一下」（< 30 px/s 尤其明显，约 5–7 Hz）。

做法：`columnAccumulator` 减掉整数列后剩下的**小数**当作子列偏移（0..1），喂给滚动窗口 —— GL 走 `snap->texScrollU = (writeCol + frac) / (cols*2)`，CPU 回退走图像变换原点 `wc = writeCol + frac`。因此 `onFrame` **每帧都要重建快照并 `repaintFrameContent()`**（不能再 `nCols == 0` 时直接 return），新列仍只在整数时刻写入。冻结时保留这段小数偏移，按下瞬间不会跳一格。

GL 的 Right / Up / Down 走「展开纹理」路径（每帧重排整张纹理、没有窗口偏移），暂时还是整列步进 —— 只有 Left（经典方向）享受子列偏移。CPU 回退四个方向都是连续的。

### 7.13 动态模块的「峰值因数历史」列相位

同一类问题的第三个现场：历史环按 120 点/秒写入，跨度为 1–35 s，绘制时把窗口 `N` 个样本按画布宽度分成若干列、每列取**最大值**。旧实现把列边界钉在「窗口起点」上 —— 窗口每推进 1 个样本，同一列统计的样本集合就整体平移一次，峰值会在相邻列之间来回跳。跨度调大（每列覆盖十几个样本）后，肉眼就是「毛刺 + 闪烁」。

修法与示波器抽点一致：新增 `crestHistTotal`（绝对写入序号），**列边界对齐到绝对样本序号** `floor(abs / spc)`，再按窗口左边缘的小数相位 `frac` 平移整条曲线 —— 画面连续滚动，同一列永远是同一批样本。右边缘那半列（超出写入位置）直接跳过，避免曲线尾部掉到 0。

### 7.14 GL 直渲染没有 scissor —— 几何必须自己裁

GL 直渲染模块的顶点是**模块本地坐标**，渲染器只设 `glViewport(0,0,vw,vh)` + 一个平移 / 翻转的 uniform，**没有按模块矩形开 `glScissor`**。所以任何超出画布的几何都会照常画出来，压到旁边的 GL 模块上。

第一个踩到的现场：示波器 X-Y / 李萨如的**标尺环**。半径 = `baseRadius * autoGainFactor`，自动缩放放大（>1）后圆环会超出画布 → 环线画到了邻近模块。修法是在进顶点流之前用 Liang–Barsky 把每个环段裁到 `canvas` 内（`clipSegmentToRect`，裁掉后重新算法线，避免半个 quad 探出边界）。

新写 GL 几何时记住这条：**画布外的部分自己裁**，别指望渲染器兜底；点云这类已经在生成阶段做过 `innerF.contains` 过滤的不受影响。

### 7.15 v3.2 视觉与交互收尾

- **VU 六种表盘**（`dialStyle` 0..5，上限 = `VuMeterModule::numDialStyles - 1`；读档 / setter 的 `jlimit` 都用它）：
  - `0` 半圆：底边为直径的 180° 上半圆（原经典外观，旧存档默认，**不要动**）；
  - `1` 复古扇形：**窄开口小扇形**（±`fanHalfAngle` ≈ ±47°，不是半圆）—— 先画一块扇形表窗（两条放射直边 + 外弧 + 盖板处内弧，淡底 + 墨色轮廓），再画粗弧轨（刻度朝外、dB 数字在最外圈）+ 内侧细副刻度弧挂百分比圆点（100% = 0 dBFS），0 dB 之后弧轨 / 副弧 / 刻度整段 `PinkXP::danger`；"VU" 字标默认左上角，与扇形相交（窄高模块）时挪进扇形中部；扇形按可用宽高的 0.92 缩放后居中；根部盖半圆盖板 `drawFanCap`（画在指针之后）；
  - `2` 横向刻度条：粗横杆按 `ok / warn / danger` 分段（-6 / 0 dBFS 分界，`kVuCautionDb`），上排斜刻度（向两侧外撇）+ 数字，下排百分比小圆点（数字从右往左贪心排、挤不下跳过），0 dB 竖直分界线贯穿三排；
  - `3` LED 梯柱：1 dB 一格，柱高从左到右逐格加高，三色分段，未亮格子留淡底 + 段色细框；
  - `4` 极简游标：大号读数（动态文本直接 `drawText`，不进静态文本缓存）+ 发丝刻度线 + 5 个稀疏刻度；
  - `5` 镜像刻度：dB 数字走中脊，上下对称刻度，外侧上下两条填充条（未亮部分淡色显示三色分区）。
  - 所有样式共用 `dialTFor` 的 dBFS 线性映射（-36 … +3），横向式的"-20 … +3 / 百分比"只借经典 VU 的视觉形式，量纲不变。
  - 指针式（0 / 1）几何由 `drawDial` 写进 `dialGeom`，`vuToAngle` 按样式分支（半圆 ±90°、复古扇形 ±`fanHalfAngle`）；横向式（`isLinearStyle()` = 2..5）用 `linearXFor` / `linearDbForX`，指示器在各自 `draw*Dial` 里画完，`paintContent` 不再画针；`needleAngleFor` 对横向式返回 0、`needleLengthFor` 返回指示区高度。
  - **`needleStyle` = 指示器形态**，每种表盘三档都要一眼可分（半圆保持旧线宽不动）：

    | 表盘 | Thin line | Wedge | Stereo L/R |
    |---|---|---|---|
    | 复古扇形 | 1.2px 发丝长针 | 矛形宽针（根宽 ≈ 1.4 × 盖板半径） | 两根细针 + 针身 L / R 圆标（错开半径） |
    | 横向刻度条 | 横杆常亮 + 贯穿三排的发丝针 + 骑杆圆珠 | 横杆变填充条（之后淡色）+ 上下大三角箭头 + 3px 前沿线 | 横杆劈成 L / R 两条轨道，各自填充 + 滑块 |
    | LED 梯柱 | 细针 LED（格宽 ≈ 间距 38%） | 实心楔形（每 3 dB 一道细缝）+ 前沿线与小三角 | 上下镜像两排（L 往上长、R 往下挂） |
    | 极简游标 | 2px 进度线 + 发丝游标 | 越往右越厚的实心楔形（0 dB 后转红）+ 大三角游标 | 两根独立发丝线（上 L 下 R）+ 左右两格读数 |
    | 镜像刻度 | 两条贴刻度的窄条 + 贯穿中脊的发丝针 | 两条楔形条（外沿张开成喇叭口）+ 前沿线 + 指向中脊的三角 | 满高矩形条 + 声道色粗端帽 |

    三色分区填充统一走 `fillZonedShape`（竖条裁剪），任意形状（矩形 / 楔形 / 梯形）共用。
  - **Toybox**：LED 处点击 = 迪斯科灯（六种共用）；指针式（0 / 1）其余区域 = 指针虚影（未改）；横向式走 `toyPokeLinear`，每种表盘一个专属玩具（几何取 `dialGeom.bandTop/bandBottom/rowTop/rowBot`，实现在 `ToyModuleFx.cpp`）：
    - 横向刻度条 = **弹簧游标** `makeSliderFlickActor`：幽灵游标被拽到点击处，拖一根锯齿弹簧、带过冲弹回真实电平，身后残影；气泡 `dB + %`；
    - LED 梯柱 = **跑马冲顶** `makeLedChaseActor`：从最左一格跑马点亮到点击那格，顶帽弹飞后落回弹三下（倒挂行往下飞）；气泡 `PEAK x dB`；
    - 极简游标 = **卷尺** `makeMeasureTapeActor`：从当前游标拉出卷尺量到点击处，标出相差多少 dB，最后卷回；
    - 镜像刻度 = **水滴涟漪** `makeMirrorRippleActor`：水滴落到中脊，以中脊为水面上下对称荡开三圈（裁在两条填充条外沿之间）；气泡 `dB + %`。
  - LED 六种样式统一右上角（`ledBounds` 不再按样式分支）；横向式由 `linearBody` 在顶部让出"标题小字 + LED"一行。
  - 新样式颜色全部取 `PinkXP::` 主题变量（`ink / ok / warn / danger / pink500~700 / btnFace / hl`）；旧存档里的 `dialStyle = 1 / 2`（原弧形 / 梯形）原地显示为复古扇形 / 横向刻度条。
- **响度灯条读数**：底部数字改用 `PinkXP::pink600`（与「数字读数」样式的大数字同色；深色主题下 pink 阶梯越靠后越亮，所以更清楚）。灯条布局由 `barLayoutArea()` 统一计算：左/上 6px、右/下 2px，刻度列 30 → 22px；静态层、柱区、刻度、标签共用同一份几何。
- **抽屉滚动条**：`PinkXPLookAndFeel` 补了 `drawScrollbar / drawScrollbarButton`（此前继承 V4 默认的蓝灰滑块 + V2 蓝灰箭头，不跟主题）。轨道 = content 略深 + ink 1px 内框；滑块 pink400 / 500 / 600（常态 / 悬停 / 按下）+ 左上 1px 高光；箭头按钮同色系。全局生效。
- **悬停说明**：`y2k::help::moduleHelp` 全面校对 —— 响度补 TP / RMS 两行，各模块补上 v3.1 新增参数与"都在齿轮面板里"的位置说明；已合并的旧类型条目保留但永不显示。语言包逐行对齐（91 行帮助文案 0 缺译、938 键 0 重复）。
- **轴刻度 / 标签留白走常量，别再写字面魔数**：
  - 频谱（`SpectrumModule.cpp` 顶层匿名 namespace）：`axisLabelGap / dbLabelColumnW / hzLabelRowH` 是画布边距与标签绘制的**唯一事实来源**。左侧 dB 列与底部 Hz 行同一口径（文字厚度 + 2px 间隙）：底部 16，左侧 20 —— 最宽刻度 `-80` 在 9px 轴字体下实测 ≈15px（Silkscreen 14.9 / ChillBitmap 14.6）。`getCanvasBounds()` 与 `drawAxisLabels()` 必须同时引用它；旧版 `withTrimmedLeft(32)` + `canvas.getX() - 32 + (28 - w)` 是同一数值手工抄了三遍，改边距漏改一处就会错位。
  - 相位（`PhaseModule.cpp` 顶层匿名 namespace）：`barLabelW(44) / barLabelGap(4) / barLeftInset` —— WIDTH / BAL 标签在条形左侧的整块空白列内**水平居中**，列宽按最宽标签实测 ≈36px + 两侧各 6px 取；GL overlay、GL 快照、CPU 回退绘制、静态层四条路径共用 `barLeftInset`（旧版 4 处 `labelW = 56` + 2 处 `withTrimmedLeft(60)` 各自抄一份）。
