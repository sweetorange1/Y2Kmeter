# Y2KMeter 音乐分析内核 —— Standalone 形态改造总报告

- 日期：2026-11-03
- 前置文档：本仓库 `PROJECT_OVERVIEW.md` 的「v2.9.9 补充」章节（算法、线程模型与已知限制）
- **本报告修正了前置文档的优先级排序** —— 因为产品形态不同，同一批技术项的性价比完全不同。

---

## 0. 结论摘要

| # | 结论 |
|---|---|
| 1 | **你面对的是同一个问题的更难版本。** 商用同类插件面向 DAW 插入场景（输入是音乐轨、有宿主时钟）；你永远在 live 路径上，而且没有宿主给你干净的输入和时钟 |
| 2 | **standalone 下最大的功能缺口不是"识别准不准"，是"没有拍相位"。** 你现在只输出一个 BPM 数字；而你的核心价值是视觉同步。`MilkdropModule.cpp:4716` 的拍同步只吃宿主 `ppqPosition`，standalone 下 `PluginProcessor.cpp:175` 的注释已经写明 `getPlayHead()` 返回 nullptr → **拍同步在 standalone 形态下是死的** |
| 3 | **需要一套"样本索引 ↔ 墙钟"的锚点基础设施。** 采集端目前**完全没有使用** `GetStreamLatency` / `GetDevicePeriod` / `GetBuffer` 的 QPC 时间戳（`WasapiLoopbackCapture.cpp:344` 传的是 `nullptr, nullptr`）。没有它，任何拍相位都锚不住，视觉同步必然漂 |
| 4 | **延迟不影晌同步精度，只影响"多久跟对"。** 只要拍相位锚定在绝对时间上，400ms 的数值延迟不会让视觉错位。反过来说，**绝不能**用"BPM 数值发布时刻 + 累加周期"去推算当前拍——那会引入系统性偏移 |
| 5 | **商用同类插件的若干设计在 standalone 下应该反向做**：60–240 硬范围 + 手动 ×2/÷2、30s 收敛、假设输入是纯音乐 —— 这些在系统音频捕获场景里都是错的 |

---

## 1. 形态差异对照

| 维度 | 商用同类插件（VST 插入） | Y2KMeter（standalone 系统音频捕获） |
|---|---|---|
| 音频来源 | 宿主轨道，用户在插件前挂了什么就是什么 | **默认播放端点的系统混音**：Spotify / YouTube / 游戏 / 电影 / OBS / 会议 |
| 声道数 | 宿主决定的 1–2 声道 | **系统 mix format**：可能是 5.1 / 7.1 |
| 采样率 | 宿主工程采样率，稳定 | 系统混音器格式，**用户换音频设备就会变** |
| 时钟 | **宿主提供 ppqPosition / BPM / isPlaying**，精确 | **完全没有**，只有一个 Sleep(5) 轮询循环 |
| 输入连续性 | 连续，音频线程驱动 | 有 gap：静音、切设备、切 app、笔记本合盖 |
| 输入内容 | 用户主动挂上去的"音乐" | **不保证是音乐**：对白、枪声、爆炸、系统提示音 |
| 会话长度 | 用户按播放/停止 | **持续数小时**，中间切无数首歌 |
| 收敛预期 | 用户愿意等 30s 看结果 | 用户切换视频后 **3–5s 内就要有数** |
| CPU 预算 | 独占宿主的一个核也没关系 | 与 Milkdrop（GPU）、频谱、各种表头**共享** |
| 分析路径 | 离线整段 或 实时 | **永久实时** |


---

## 2. 你的 standalone 完整链路与延迟账

```
[系统混音] 
  → WASAPI loopback（缓冲区时长由引擎周期决定，实测约 10ms；你传的 100ms 在 shared 模式下不可依赖）
  → captureThread 轮询 Sleep(5)                              +0~5ms
  → onAudio → pushStereo → 无锁 SPSC 环形缓冲（131072 样本 ≈ 2.7s @48k）
  → 分析线程 wait(5)/wait(40) 轮询                            +0~5ms（常态）
  → TempoEstimator：46ms Hann 窗（中心延迟 ~23ms）+ 8s 包络 + 每 0.5s 估计一次
  → KeyEstimator：186ms 窗（~93ms）+ 20s 滑动窗 + 每 1.5s 更新一次
  → publishResult：200ms 节流
  → AnalyserHub 30Hz Timer 分发                                +16~33ms
```

### 端到端延迟（估算）

| 输出 | 典型 | 最坏 |
|---|---|---|
| BPM 数值变化可见 | **~410 ms** | ~800 ms |
| Key 数值变化可见 | **~980 ms** | ~1.9 s |
| 拍相位（若实现） | **~40 ms**（只受采集 + 轮询影响） | ~60 ms |

**这张表最重要的一行是第三行。** 拍相位可以在很低的延迟下拿到（因为它只需要"现在处理到哪个样本"，不需要等估计周期），而 BPM 数值必然滞后。

> **核心设计原则**：拍相位锚定绝对时间，BPM 数值滞后多少都无所谓。
> `beatPhaseAtWallClock(now)` 必须是 `fmod(now − beatAnchorWallSec, period)`，
> 而**不是** `fmod(now − bpmPublishedTime, period)`。后者会随每次 BPM 更新而跳变。

---

## 3. standalone 独有、商用同类插件根本不需要面对的 6 个问题

### S1 🔴 没有时钟锚点 → 一切同步都建在沙上

**现状**：`WasapiLoopbackCapture.cpp:344`
```cpp
hr = captureClient->GetBuffer (&data, &frames, &flags, nullptr, nullptr);
//                                             设备位置 ↑      QPC ↑  全是 nullptr
```
采集端从不记录"这段音频是几点几分被听到的"。分析线程只知道自己处理了多少个样本。

**为什么致命**：样本计数器和墙钟之间有两条裂缝——
1. **注入静音**：`WasapiLoopbackCapture.cpp:323-331`，无 packet 超过 100ms 就主动喂 100ms 全 0。这些零不是真实采样时序，累计后样本数 ≠ 真实经过时间
2. **丢样**：环形缓冲溢出时丢新数据并递增 `gapEpoch`（`MusicAnalysisEngine.cpp:256-258`），样本计数器直接断裂

**后果**：任何"从某个起点按周期累加"的相位推算，会以每首歌几十到几百毫秒的速度累积误差。视觉在 3 分钟内必然肉眼可见的偏移。

**修法**：见 §5.1。

---

### S2 🔴 换轨/换内容无法感知 → 旧结果会污染新内容

**现状**：只在**静音超过 3 秒**时清窗口（`MusicAnalysisEngine.cpp:14` `kClearAfterSilence = 3.0`）。

**问题场景**（standalone 下天天发生）：

| 场景 | 有没有静音 | 后果 |
|---|---|---|
| 手动切歌，中间停顿 | 有（>3s） | ✅ 正常清理 |
| **YouTube 自动连播 / Spotify 无缝播放** | **没有** | 🔴 8s onset 包络 + 20s chroma 窗口里同时装着两首歌 → 自相关拿到垃圾周期，chroma 拿到两个调的混合 → **会稳定输出一个错误答案，而且因为"3 代稳定"判定还会给较高置信度** |
| 看视频中途插播广告 | 通常有短暂停顿 | 🟡 若 <3s 则不清，广告的人声会污染 chroma |
| 游戏里从音乐切到战斗音效 | 没有 | 🔴 同上 |

**这是 standalone 形态下"结果明显离谱"最常见的成因**，而且它不会自己恢复——要等 8/20 秒窗口自然滑过去。

**修法**：见 §5.2。

---

### S3 🟡 多声道只取前两路 → 中央声道（人声/主旋律）被丢掉

**现状**：`WasapiLoopbackCapture.cpp:118-127`
```cpp
// 每帧 channels 个样本，取 ch0 / ch1（若 mono 则 L=R）
if (channels >= 2) {
    L[f] = readSample (frame);
    R[f] = readSample (frame + (bps / 8));
}
```
**只读 ch0 / ch1，其余声道直接丢弃。**

**问题**：`GetMixFormat()`（`:253`）拿到的是端点 mix format。如果用户把输出设备设成 5.1（家庭影院、部分游戏本、外接声卡），你会得到 6 声道流：

| 声道 | 内容 | 丢掉它的后果 |
|---|---|---|
| L / R | 伴奏、环境声 | — |
| **C** | **人声、主旋律、游戏中大部分对白与 Boss 语音** | 调性检测失去主旋律 → chroma 偏向伴奏的根音与五度 |
| LFE | 低频 | 影响不大（低频不贡献 chroma，反而减少误判） |
| Ls / Rs | 环绕效果 | 影响不大 |

**对调性检测是实质性伤害**：主旋律往往是调性最强的载体，把它丢了，剩下的伴奏更容易判成关系调或五度调。

**修法**：见 §5.3。

---

### S4 🟡 输入不保证是音乐 → 需要"是否值得给结果"的门控

**商用同类插件永远不需要想这个**：用户把它挂在音乐轨道上，输入就是音乐。

**你的输入可能是**：游戏对白、枪声连发、爆炸、系统提示音、会议人声、纯环境音、静音。

**现状**：只有电平门限（`kSilenceDb = -58dBFS`）。够响的非音乐内容照样会走到 `updateKey()`，得到 chroma 后强行和 KK 轮廓做相关，可能凑巧过 0.30 门限就发布一个"调性"。

**用户体验问题**：用户正在打游戏，看到"Meter 显示 D minor"，这是负价值——它损害了产品在所有其他场景下的可信度。

**修法**：见 §5.4。

---

### S5 🟡 收敛速度与准确率的取舍，standalone 更偏向"快"

你现在的设计：

| 参数 | 值 | 依据 | standalone 是否合适 |
|---|---|---|---|
| Tempo 起算 | 4s（`kMinEnvelopeSeconds`） | 出数要快 | 🟢 合适 |
| Tempo 稳定判定 | ±4%，3 代，2 轮确认 | 防抖 | 🟡 约 5–6s 才发布，可以 |
| Key 起算 | 8s（`minWindowSec`） | 准确率 | 🟡 有点慢 |
| Key 窗口 | 20s | 转调跟随 | 🟢 合适 |
| Key 2 票确认 | 2 次 × 1.5s = 3s | 防抖 | 🟡 首次发布要 ~11s，偏慢 |
| 置信度数据充分度 | 按 20s 计 | 诚实 | 🟢 **设计正确**（商用同类插件是黑盒概率） |

**关键**：离线整段分析的常见口径是 30 秒以上才给出可靠的初始 tempo。你在 live 路径上 5s 出数、用置信度表达不确定性，**这个设计是对的，不要为了"追平准确率"把它改慢**。

---

### S6 🟢 CPU 预算：比 VST 紧，但你的算法没问题

standalone 下同时跑：Milkdrop（GPU + 参数更新）、频谱、示波器、各种电平表、以及 60/120Hz UI 重绘。

**好消息**：音乐分析在独立线程，且现有开销很低。

| 环节 | 每秒运算量估算（@48k） |
|---|---|
| Tempo FFT（2048 点 @ 93.75 fps） | ~0.2 M butterfly/s |
| Tempo ACF（8s 包络，230 lag，每 0.5s） | 可忽略 |
| Key FFT（8192 点 @ 21.5 fps） | ~2.3 M butterfly/s |
| **合计** | **远低于 Milkdrop 的 GPU 负载** |

**即使加上本报告推荐的 HPCP 谐波求和**（69 音 × 6 谐波 × 3 bin × 3 迭代 × 21.5 fps ≈ **8 万次读/秒**），仍然可忽略。

**结论：不要因为"怕卡"而放弃算法升级，瓶颈不在这里。** 真正的风险在 GPU 侧（`docs/Spectrogram3D_Performance_Optimization.md` 已经在处理）。

---

## 4. 改造清单（按 standalone 形态重排）

> 前置文档按"纯算法差距"排序；这里按"standalone 产品价值"重排。

| 新优先级 | 项 | 归类 | 解决什么 | 工作量 |
|---|---|---|---|---|
| **A1** | **样本↔墙钟锚点基础设施** | S1 | 一切同步的前提 | ~120 行 |
| **A2** | **拍相位 + 拍格输出** | T1 | Milkdrop standalone 拍同步，**产品核心价值** | ~90 行 |
| **A3** | **换轨/新颖度检测** | S2 | 消灭"稳定输出错误答案" | ~100 行 |
| **B1** | 调音基准检测 | K2 | 变速/磁带/Suno 素材调性 | ~80 行 |
| **B2** | 谱白化 | K3 | chroma 被音色主导 | ~25 行 |
| **B3** | 谐波感知 HPCP | K1 | **关系大小调混淆的根因** | ~120 行 |
| **C1** | 音乐性门控 | S4 | 游戏/对白场景不瞎报 | ~60 行 |
| **C2** | 多声道正确降混 | S3 | 5.1 下中央声道不丢 | ~40 行 |
| **C3** | DP 拍跟踪（Ellis） | T2/T3/T4 | 切分音/无鼓/变速鲁棒性 | ~150 行 |
| **D1** | 轮廓表升级 | K5 | 现代流行/电子更准 | ~40 行 |
| ~~X~~ | 神经网络 | — | **不建议现在做**（见 §6） | — |

**A 组是产品性的（没它功能就没有），B 组是准确率的，C 组是场景鲁棒性的，D 组是打磨。**

建议顺序：**A1 → A2 → A3 → B1 → B2 → B3 → C1 → C2 → C3 → D1**。

A1 和 A2 必须连着做——单独做 A2 会得到一个"看着对但慢慢漂"的功能，比没有更糟。

---

## 5. 关键实现要点

### 5.1 样本 ↔ 墙钟锚点（A1）

**目标**：给定"处理到第 S 个样本"，能回答"这个样本是几点几分被听到的"。

#### 5.1.1 采集端要先拿到设备延迟

`WasapiLoopbackCapture` 在 `Initialize` 成功后加：

```cpp
// GetStreamLatency 返回 100ns 单位，表示"数据到端点 → 可被听到"的流延迟
REFERENCE_TIME streamLatency = 0;
if (SUCCEEDED (audioClient->GetStreamLatency (&streamLatency)))
    captureLatencySec = (double) streamLatency / 1.0e7;
else
    captureLatencySec = 0.010;   // 保守回退 10ms

// 同时记录引擎周期（用于诊断，不直接参与计算）
REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
audioClient->GetDevicePeriod (&defaultPeriod, &minPeriod);
```

并且把 `GetBuffer` 的 QPC 参数用起来（这是最准的方式）：

```cpp
UINT64 devPos = 0, qpcPos = 0;
hr = captureClient->GetBuffer (&data, &frames, &flags, &devPos, &qpcPos);
// qpcPos 是这段首样本对应的性能计数器位置（100ns）
// → 直接得到"样本 ↔ QPC"的精确锚点，无需估算
```

**建议优先用 `qpcPos`**：它是驱动给出的真实硬件时间戳，比自己估算的 `steady_clock − latency` 准得多。`GetStreamLatency` 作为回退。

回调签名需要扩展：

```cpp
using AudioCallback = std::function<void (const float* left, const float* right,
                                          int numSamples, double sampleRate,
                                          uint64_t qpc100ns)>;   // 新增
```

#### 5.1.2 引擎里维护锚点环

```cpp
// MusicAnalysisEngine 私有
struct TimeAnchor
{
    uint64_t sampleIndex = 0;   // 全局已写样本号
    double   wallSec     = 0.0; // steady_clock 秒
};

static constexpr int kAnchorCount = 8;      // 2 的幂，简单索引
std::array<TimeAnchor, kAnchorCount> anchors {};
std::atomic<uint32_t> anchorWritePos { 0 };

// pushStereo 里（采集线程），写入成功之后：
TimeAnchor a;
a.sampleIndex = writeIndex.load (std::memory_order_relaxed);   // 写入后的位置
a.wallSec     = qpcToSteadySec (qpc100ns);                     // 或 now - captureLatencySec
anchors[anchorWritePos.load() & (kAnchorCount - 1)] = a;
anchorWritePos.fetch_add (1, std::memory_order_release);
```

#### 5.1.3 消费端插值

```cpp
// 分析线程：把样本号映射成墙钟秒
bool sampleIndexToWallSec (uint64_t sampleIndex, double& outWallSec) const noexcept
{
    const uint32_t n = anchorWritePos.load (std::memory_order_acquire);
    if (n == 0) return false;

    // 找最近的、且 sampleIndex >= 目标 的锚点，与其前一个做线性插值
    for (int k = 0; k < (int) juce::jmin (n, (uint32_t) kAnchorCount); ++k)
    {
        const auto& cur  = anchors[(n - 1 - (uint32_t) k) & (kAnchorCount - 1)];
        const auto& prev = anchors[(n - 2 - (uint32_t) k) & (kAnchorCount - 1)];

        if (cur.sampleIndex < sampleIndex) break;
        if (prev.sampleIndex > sampleIndex) continue;

        const double span = (double) (cur.sampleIndex - prev.sampleIndex);
        if (span < 1.0) { outWallSec = cur.wallSec; return true; }

        const double f = (double) (sampleIndex - prev.sampleIndex) / span;
        outWallSec = prev.wallSec + f * (cur.wallSec - prev.wallSec);
        return true;
    }
    return false;
}
```

**为什么用插值而不是"起点 + 样本数/sr"**：注入静音（`S1`）和丢样（`gapEpoch`）会让样本数与真实时间脱钩。锚点环自动吸收这些误差，只要两次锚点之间的漂移足够小（毫秒级），插值精度就是毫秒级。

---

### 5.2 拍相位 + 拍格（A2）

你已经从 ACF 拿到了最佳 `refinedLag`，再做两件事：**求相位**、**锚定绝对时间**。

#### 5.2.1 TempoEstimator 增加相位求解

在 `estimateTempo()` 里，抛物线插值算出 `refinedLag` 之后（现 `TempoEstimator.cpp:410` 附近）：

```cpp
const int P = std::max (1, (int) std::lround (refinedLag));

// 相位直方图：把包络按 i % P 归并，能量最大处就是拍点相位
std::vector<double> phaseAcc ((size_t) P, 0.0);
for (int i = 0; i < n; ++i)
    phaseAcc[(size_t) (i % P)] += (double) envLinear[(size_t) i];

int phasePeak = 0;
for (int i = 1; i < P; ++i)
    if (phaseAcc[(size_t) i] > phaseAcc[(size_t) phasePeak])
        phasePeak = i;

// envLinear[n-1] 是"最新一帧"，对应样本号 = totalSamples
// 第 phasePeak 帧相对最新一帧回退 (n - 1 - phasePeak) 个 hop
const long long beatSampleIndex =
    (long long) totalSamples - (long long) (n - 1 - phasePeak) * hopSize;

beatPeriodSec = refinedLag / frameRate;
pendingBeatSampleIndex = beatSampleIndex;   // 交给引擎换算成墙钟
```

#### 5.2.2 引擎负责把样本号换成墙钟并发布

```cpp
// MusicAnalysisEngine::publishResult 里
if (tempo.hasBeatGrid())
{
    double wallSec = 0.0;
    if (sampleIndexToWallSec (tempo.beatAnchorSample(), wallSec))
    {
        // 归一化到一个周期内，避免长时间运行后数值过大
        const double period = tempo.beatPeriodSec();
        r.beatPeriodSec   = period;
        r.beatAnchorWallSec = wallSec - std::floor (wallSec / period) * period;
        r.beatPhaseValid  = true;
    }
}
```

#### 5.2.3 消费端（Milkdrop / 任何视觉模块）

`FrameSnapshot` 加三个字段：`beatPeriodSec`、`beatAnchorWallSec`、`beatPhaseValid`。

```cpp
// 任意模块里，本帧渲染时
const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;

double phase01 = 0.0;
if (snap.beatPhaseValid && snap.beatPeriodSec > 0.0)
{
    const double ph = std::fmod (nowSec - snap.beatAnchorWallSec, snap.beatPeriodSec);
    phase01 = (ph < 0.0 ? ph + snap.beatPeriodSec : ph) / snap.beatPeriodSec;
}
// phase01 就是"当前处在两拍之间的哪个位置"，0 = 正拍
```

Milkdrop 的 `autoModeIsBeat_` 分支（`MilkdropModule.cpp:4714`）改成：

```cpp
// 优先用宿主 ppq（有宿主时最准）
if (snap.valid && snap.isPlaying && snap.bpm > 0.0) { /* 现有逻辑 */ return; }

// 回退：用自己的拍相位（standalone）
if (! snap.beatPhaseValid) return;
const int beatsPerSwitch = std::max (1, autoBeats_);
const long long bucket = (long long) std::floor ((nowSec - snap.beatAnchorWallSec)
                                                 / snap.beatPeriodSec);
const long long bucketIndex = bucket / beatsPerSwitch;
if (! beatBucketInitialized_) { lastBeatBucket_ = bucketIndex; beatBucketInitialized_ = true; return; }
if (bucketIndex > lastBeatBucket_) { lastBeatBucket_ = bucketIndex; randomPreset(); }
```

**这一步做完，standalone 下的拍同步就活了**——而且是从系统音频里自己检测的拍，不依赖任何宿主。

#### 5.2.4 优雅降级

相位会在这些情况下失效：置信度掉下来、换轨、长时间静音。建议分级：

| 置信度 | 行为 |
|---|---|
| conf ≥ 0.6 且 `beatPhaseValid` | 正常打拍，视觉硬对齐 |
| 0.3 ≤ conf < 0.6 | 继续用旧周期自由运行（free-run），视觉不断 |
| conf < 0.3 或换轨检测触发 | **暂停打拍**，视觉退回时间模式，UI 显示 "RE-SYNC" |

**宁可停，也不要打错拍。** 错拍的视觉比不对拍的视觉更破坏产品可信度。

---

### 5.3 换轨 / 新颖度检测（A3）

不用新信号，复用你已有的 chroma 就够。

#### 5.3.1 方案：短窗 chroma 的余弦距离

```cpp
// TempoEstimator / KeyEstimator 之外，引擎里维护一个轻量的"当前 1s 频谱指纹"
// 直接用 KeyEstimator 已有的 chroma 也可以，但为了独立性，建议在引擎层做：

static constexpr int kFingerprintBins = 12;   // 用 12 维 chroma 最省
std::array<float, 12> fpFast {};   // 最近 1s 加权平均
std::array<float, 12> fpSlow {};   // 最近 8s 加权平均
double fpFastLastUpdateSec = 0.0;

void updateNovelty (const Result& r, double nowSec)
{
    // 用 tempo 的 24 带能量做指纹也行；这里假设 KeyEstimator 能导出短窗 chroma
    // 计算余弦距离
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        dot += (double) fpFast[i] * (double) fpSlow[i];
        na  += (double) fpFast[i] * (double) fpFast[i];
        nb  += (double) fpSlow[i] * (double) fpSlow[i];
    }
    const double cos = dot / (std::sqrt (na * nb) + 1.0e-9);

    if (cos < 0.90)          // 阈值待调，0.88~0.93 之间
    {
        if (++noveltyStreak >= 2)   // 连续 2 次（约 2s）才认定换轨
        {
            tempo.resetAccumulation();
            key.resetAccumulation();
            noveltyStreak = 0;
        }
    }
    else noveltyStreak = 0;
}
```

#### 5.3.2 更省事的替代方案

如果不想加指纹，可以直接**降低清理门限 + 结合置信度**：

```cpp
// 现有：静音 > 3s 才清
constexpr double kClearAfterSilence = 3.0;

// 改成：静音 > 0.8s 就清（切歌、切视频通常有 >1s 间隙）
constexpr double kClearAfterSilence = 0.8;
```

代价：玩游戏中短暂静音（爆炸后的瞬间）会误清，导致频繁重新收敛。
**建议还是做指纹方案**，它同时能处理无缝播放这个主场景。

#### 5.3.3 还要注意

`TempoEstimator::resetAccumulation()` 会保留 `published`/`publishedBpm`（`:136-143` 的 `reset()` 才会清）。这是有意设计（静音时不闪），但**换轨时应该用 `reset()` 而不是 `resetAccumulation()`**，否则新歌会沿用旧 BPM 一直到新结果发布——中间那段是明确的错值。

**建议区分两种清理**：
- 静音 → `resetAccumulation()`（保留显示，UI 标 stale）
- **换轨 → `reset()` + 立刻发一帧 `Status::analysing`**（明确告诉用户"重新开始分析了"）

---

### 5.4 音乐性门控（C1）

目的是"**不确定是音乐时，宁可不给调性**"。

#### 5.4.1 廉价特征

三个都在现有 FFT 结果上直接算，无需额外 FFT：

| 特征 | 计算 | 判据 |
|---|---|---|
| **谱平坦度**（spectral flatness） | 几何均值 / 算术均值 | 对白/噪声接近 1，音乐通常 < 0.3 |
| **色度集中度** | `max(chroma) / mean(chroma)` | 音乐通常 > 3，噪声接近 1.2 |
| **调性贴合度** | 现有 `lastBestScore`（KK 相关 r） | 已存在，`kMinKeyCorrelation = 0.30` |

#### 5.4.2 组合判定

```cpp
// KeyEstimator::updateKey() 里，发布前加一道
const bool looksMusical = (chromaPeakRatio > 1.8f)
                       && (lastBestScore  > kMinKeyCorrelation);

if (! looksMusical)
{
    confident     = false;
    keyConfidence = 0.0f;
    // 不要动 published —— 保留上次结果，让 UI 标 stale
    return;
}
```

**注意**：你已经有 `kMinKeyCorrelation = 0.30` 这道门，`looksMusical` 主要是把"凑巧过线的非音乐"挡掉。这条改动很小，但能显著减少"打游戏时看到奇怪调性"的负体验。

#### 5.4.3 UI 表达

`Status` 枚举已有 `noSignal`。建议加一个 `nonMusic`：
- `NO SIGNAL` —— 没声音
- `NOT MUSIC` —— 有声音但不像音乐（对白/音效）

在 standalone 场景下，这两种状态频繁出现，**把它们讲清楚比硬给一个答案更有价值**。

---

### 5.5 多声道正确降混（C2）

替换 `WasapiLoopbackCapture.cpp:118-134` 的 `ch0/ch1` 逻辑。

```cpp
// 按声道数决定降混权重（ITU-R BS.775 / Dolby 常用系数）
//   stereo : L, R
//   5.1    : L + 0.707·C + 0.707·Ls → L ;  R + 0.707·C + 0.707·Rs → R ; LFE 丢弃
//   7.1    : 额外 0.5·Lb / 0.5·Rb
for (int f = 0; f < framesAvailable; ++f)
{
    const BYTE* frame = src + (size_t) f * (size_t) bytesPerFr;
    const int stride = bps / 8;

    auto ch = [&] (int idx) -> float
    {
        return (idx < channels) ? readSample (frame + (size_t) idx * stride) : 0.0f;
    };

    float l = 0.0f, r = 0.0f;

    if (channels == 1)
    {
        l = r = ch (0);
    }
    else if (channels == 2)
    {
        l = ch (0); r = ch (1);
    }
    else if (channels == 6)        // 5.1
    {
        const float c = ch (2), lfe = ch (3), ls = ch (4), rs = ch (5);
        (void) lfe;                // LFE 丢弃，低频不贡献 chroma，还会干扰 onset
        l = ch (0) + 0.707f * c + 0.707f * ls;
        r = ch (1) + 0.707f * c + 0.707f * rs;
    }
    else if (channels == 8)        // 7.1
    {
        const float c = ch (2), lfe = ch (3), ls = ch (4), rs = ch (5);
        const float lb = ch (6), rb = ch (7);
        (void) lfe;
        l = ch (0) + 0.707f * c + 0.707f * ls + 0.5f * lb;
        r = ch (1) + 0.707f * c + 0.707f * rs + 0.5f * rb;
    }
    else                           // 其他配置：保守取前两路
    {
        l = ch (0); r = ch (1);
    }

    // 5.1 叠加后峰值可能超过 ±1，做一次软限幅，避免后续 dB 计算溢出
    L[(size_t) f] = std::clamp (l, -1.0f, 1.0f);
    R[(size_t) f] = std::clamp (r, -1.0f, 1.0f);
}
```

⚠️ **注意声道顺序**：WASAPI 的 5.1 顺序是 `FL, FR, FC, LFE, BL, BR`（对应 `SPEAKER_*` 位掩码）。但不同驱动有时会用 `FL, FR, FC, LFE, SL, SR`。稳妥做法是检查 `WAVEFORMATEXTENSIBLE.dwChannelMask`，按掩码取而不是按索引猜。

**macOS 侧同理但位置不同**：两条路径最终都调 `AnalyserHub::pushStereo(L, R, n)`（`Y2KStandaloneApp.cpp:1766/1779/1972/1986`），降混发生在**更上游**。macOS 用 ScreenCaptureKit / 桌面音频 tap 时 `mChannelsPerFrame` 同样可能 > 2（`MacDesktopAudioCapture.mm:370` 已经在读这个字段），需要在喂进 `pushStereo` 之前做同样的降混。

---

## 6. 不建议照搬的部分

| 常见商用的做法 | 为什么你不能照搬 |
|---|---|
| **BPM 硬范围 60–240** | 你是游戏/影视音频分析工具，慢速配乐（40–60）和双踩/碎拍（250+）都常见。你现在的 **45–220 反而更合适**；如果扩，建议 40–240 并配合下面的自动八度 |
| **×2 / ÷2 手动按钮** | standalone 没有用户盯着去点。你需要**自动八度判定**（结合"哪个八度更符合 4/4 拍强弱的周期性"），或者在 UI 上**同时显示两个候选**（如 `128 (64)`），把选择权交给用户但不用他操作 |
| **最少 30s 才可靠** | 这是它离线路径的口径。你切换视频后 30s 才出数=不可用。保持你现在 4–6s 的策略，用置信度表达不确定性 |
| **整段离线分析** | 你没有文件输入（也没有必要做） |
| **假设输入是音乐** | 见 §5.4，这是你必须自己解决的问题 |
| **frugally-deep + 内嵌 NN** | 见前置文档 §4：模型只有 42.8K 参数，真实成本是标注数据。且 ONNX/fdeep 会破坏你"纯 C++ 可单测"的设计 |

**同时，"整段离线分析"是插件形态做不了、而 standalone 可以额外提供的能力**：你将来如果真要做"导入文件分析"功能（用户可能会想要），可以复用同一套 DSP 但把窗口拉长、把确认轮次加多，走"高精度慢速"模式。**这是你相对 商用同类插件能做的一个差异化功能**——它是插件，做不了"分析我这整个素材库"。

---

## 7. 路线图与验收指标

### 阶段一：让拍同步活过来（A1 + A2）

| 任务 | 文件 | 验收 |
|---|---|---|
| 采集端拿 QPC 时间戳 + `GetStreamLatency` | `WasapiLoopbackCapture.cpp/.h` | 回调能传出 `qpc100ns` |
| 锚点环 + 插值 | `MusicAnalysisEngine.cpp/.h` | 单元测试：构造已知样本↔时间序列，插值误差 < 2ms |
| 相位直方图 | `TempoEstimator.cpp/.h` | 用 `makeClickTrack` 生成 120 BPM，相位误差 < 1 个包络帧（11.6ms） |
| 发布 beat 字段 | `MusicAnalysisTypes.h`、`AnalyserHub.h` | `FrameSnapshot` 有 `beatPeriodSec/beatAnchorWallSec/beatPhaseValid` |
| Milkdrop 回退分支 | `MilkdropModule.cpp:4714` | **standalone 下拍切换正常工作** |
| 长时间漂移测试 | 新测试用例 | 连续跑 10 分钟，相位漂移 < 20ms |

**这一阶段做完，产品多了一个 商用同类插件给不了的能力**：不依赖 DAW 的、从系统音频实时检测的拍同步。

### 阶段二：消除"稳定输出错误答案"（A3 + C1）

| 任务 | 验收 |
|---|---|
| 短窗 chroma 指纹 + 新颖度 | 测试用例：拼接 120 BPM / 90 BPM 两段无缝音频，换轨后 **3s 内** 输出新 BPM |
| 换轨走 `reset()` 而非 `resetAccumulation()` | 换轨瞬间 UI 从 TRACKING → ANALYSING |
| 音乐性门控 | 测试用例：纯白噪声、纯人声，**不发布调性** |

### 阶段三：调性准确率（B1 + B2 + B3）

按前置文档的 P0→P1→P2 顺序（有依赖关系）。

**关键验收点**：把 `tests/music_analysis_test.cpp:335` 的
```cpp
check (exact || relative, "wrong key: " + tag);
```
收紧为
```cpp
check (exact, "wrong key: " + tag);
```
**这一步能把"关系大小调混淆降低到什么程度"变成可量化指标。**

### 阶段四：场景鲁棒性（C2 + C3）

### 阶段五：打磨（D1）

---

## 8. 一页总结

> **商用同类插件解决的是"给 DAW 里的音乐轨打标签"。**
> **你解决的是"给任何事情配一套会呼吸的视觉"。**
>
> 这两个问题的技术重叠只有前端那一段（FFT、chroma、onset）。往后全部不同：
> 它要一个准确的数字；你要一个**准时**的拍。
> 它有时钟、有干净输入、有 30 秒耐心；你什么都没有。
>
> 所以最该补的不是"识别更准"，而是：
> **① 把拍相位锚到绝对时间（A1+A2）② 换轨时别给错答案（A3）③ 不像音乐时闭嘴（C1）。**
> 这三件补完，你的 standalone 形态在**产品能力**上就超越了 商用同类插件在你场景里的可能性；
> 再补 B1–B3 的谐波/白化/调音，**识别精度**也基本追平。
>
> 而你已经有、它没有的东西——滑动窗口转调跟随、四种小调、可解释置信度、纯 C++ 可单测——
> 全部都是 standalone 形态下的正确选择，**不要在追平的过程中把它们丢掉**。
