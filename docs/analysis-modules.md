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

**现状**：已入库但**版本未 bump、未进任何布局预设**。两个模式 PAN（按频率声像 ±1）/ MONO（切单声道相位抵消量，上下倒置）。GL 直渲染 + CPU 回退双路径。

**待办 / 已知缺口**：
1. 三路 FFT 的 CPU 成本未测量 —— 用 `PerformanceCounterSystem`（RelWithDebInfo + `Y2K_ENABLE_PERF_COUNTERS=1`）对比「有/无 Stereo Map」的 CPU 与帧率；超标则改"仅 stereoMap 被 retain 时才跑 L/R"。
2. 未进布局预设 / stick 变体，要不要给预设位待产品定。
3. `PerformanceCounterSystem::moduleTypeNameById` 已滞后（缺 `stereoField`/`milkdrop`/`stereoMap`），只在 perf 调试构建显示 `unknown(N)`。
4. `bandCount = plot 宽度`，上限 512、下限 96（窄窗相邻频点重复取同 bin），是否改"固定 bandCount + 像素插值"待定。
5. 无右键自定义项（锁定量程/节能模式等）。
6. MONO 抵消是**逐频段峰值近似**（非逐 bin 复数比），强/弱带混时抵消量偏乐观；要精确需 Hub 逐 bin 相干接口（成本低但动 ABI）。
7. `kCancelFullScale = 0.5`（"抵消一半即满"）听感标定待实机确认，改这一个常量即可调灵敏度。

**换环境续做注意**：改了 `AnalyserHub.h` 成员布局（新增 6 vector）→ 全量重编；`stereoMap` 枚举插在中间 → `virtuPet`/`milkdrop` 值 +1 → 全量构建。
