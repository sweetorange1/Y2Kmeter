#ifndef PBEQ_ANALYSER_HUB_H_INCLUDED
#define PBEQ_ANALYSER_HUB_H_INCLUDED

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <deque>

// ============================================================
// 为规避 MSVC 多文件同进程编译时 include guard 跨 TU 串扰，
// 将 LoudnessMeter 与 AnalyserHub 合并到同一个头文件。
// 两者实现分别在 LoudnessMeter.cpp 与 AnalyserHub.cpp 中。
// ============================================================

// ==========================================================
// LoudnessMeter —— ITU-R BS.1770-4 响度计
//
// 实现：
//   - K-weighting 两级 IIR 滤波（pre-filter + RLB-weighting）
//   - 400ms 块均方（Momentary LUFS）
//   - 3s 滑动窗口（Short-term LUFS）
//   - 全程积分（Integrated LUFS，含 -70 LUFS 绝对门限 + 相对门限）
//   - 每通道 RMS（100ms 滑动窗口）
//   - True Peak（4× 过采样插值）
//
// 线程安全：
//   pushStereo() 在音频线程调用；
//   getSnapshot() 在 UI 线程调用，内部用 SpinLock 保护快照拷贝。
// ==========================================================

class LoudnessMeter
{
public:
    // ---- 对外快照结构 ----
    struct Snapshot
    {
        float lufsM     = -144.0f;   // Momentary LUFS（400ms）
        float lufsS     = -144.0f;   // Short-term LUFS（3s）
        float lufsI     = -144.0f;   // Integrated LUFS（全程）
        float rmsL      = -144.0f;   // 左声道 RMS dBFS（100ms）
        float rmsR      = -144.0f;   // 右声道 RMS dBFS（100ms）
        float truePeakL = -144.0f;   // 左声道 True Peak dBTP
        float truePeakR = -144.0f;   // 右声道 True Peak dBTP
        bool  integrated = false;    // 是否已有足够数据计算 Integrated
    };

    LoudnessMeter();
    ~LoudnessMeter() = default;

    // 在 prepareToPlay 时调用
    void prepare(double sampleRate, int samplesPerBlock);

    // 在音频线程调用，推送立体声数据
    void pushStereo(const float* left, const float* right, int numSamples);

    // 重置所有积分器（如 DAW 停止播放时）
    void reset();

    // UI 线程调用，获取当前快照
    Snapshot getSnapshot() const;

private:
    // ---- K-weighting 滤波器系数（每通道两级 biquad）----
    struct BiquadCoeffs
    {
        double b0, b1, b2, a1, a2;
    };

    struct BiquadState
    {
        double z1 = 0.0, z2 = 0.0;
        double process(double x, const BiquadCoeffs& c) noexcept
        {
            double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    // 每声道两级滤波器状态
    struct ChannelFilter
    {
        BiquadState stage1, stage2;
        void reset() { stage1.reset(); stage2.reset(); }
    };

    // 计算 K-weighting 系数（依赖采样率）
    static void calcKWeightingCoeffs(double sr,
                                     BiquadCoeffs& preFilter,
                                     BiquadCoeffs& rlbFilter);

    // 对单样本应用 K-weighting，返回加权后的值
    double applyKWeighting(double sample, ChannelFilter& ch) const noexcept;

    // ---- True Peak（4× 过采样，FIR 插值）----
    static constexpr int tpOversample = 4;
    static constexpr int tpFirLen     = 12; // 每相位 FIR 长度
    // 4 相位 × 12 抽头的 FIR 系数（Sinc + Hann 窗）
    static const float tpFirCoeffs[tpOversample][tpFirLen];

    struct TruePeakState
    {
        std::array<float, tpFirLen> history {};
        float peak = 0.0f;
        void reset() { history.fill(0.0f); peak = 0.0f; }
        void push(float x) noexcept
        {
            // 移位
            for (int i = tpFirLen - 1; i > 0; --i)
                history[(size_t)i] = history[(size_t)(i - 1)];
            history[0] = x;
        }
        float interpolate(int phase) const noexcept;
    };

    // ---- 内部状态 ----
    double sampleRate = 44100.0;

    BiquadCoeffs preFilterCoeffs  {};
    BiquadCoeffs rlbFilterCoeffs  {};

    ChannelFilter filterL, filterR;
    TruePeakState tpL, tpR;

    // 400ms 块（Momentary）
    int momentaryBlockSize  = 0;   // 采样数
    int momentaryWriteCount = 0;
    double momentarySumL    = 0.0;
    double momentarySumR    = 0.0;

    // 3s 短期（Short-term）：用 400ms 块的滑动队列（最多 7.5 块）
    static constexpr int shortTermBlocks = 8; // 覆盖 ~3.2s
    std::deque<double> shortTermBlocksL;
    std::deque<double> shortTermBlocksR;

    // 积分（Integrated）：所有通过门限的 400ms 块
    double integratedSumL   = 0.0;
    double integratedSumR   = 0.0;
    int    integratedCount  = 0;
    // 第一轮门限（-70 LUFS 绝对门）
    static constexpr double absGateThreshold = 1.0e-7; // -70 LUFS 线性均方

    // RMS（100ms 滑动窗口）
    int rmsWindowSize  = 0;
    std::deque<float> rmsHistL, rmsHistR;
    double rmsSumL = 0.0, rmsSumR = 0.0;

    // 快照（SpinLock 保护）
    mutable juce::SpinLock snapshotLock;
    Snapshot snapshot;

    // 更新快照（在音频线程每 400ms 块结束时调用）
    void updateSnapshot();

    // 计算 LUFS（从线性均方值）
    static float linearToLUFS(double meanSquare) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessMeter)
};

// ==========================================================
// PhaseCorrelator —— 立体声相位相关性 & 宽度
//
// 计算内容（基于 EMA 滑动窗）：
//   correlation = E[L·R] / sqrt(E[L²] · E[R²])     ∈ [-1, +1]
//       +1 完全同相 / 0 不相关 / -1 反相 (单声)
//   width       = E[side²] / (E[mid²] + E[side²])  ∈ [0, 1]
//       0 = 纯 mono / 1 = 纯 side
//   balance     = (E[R²] - E[L²]) / (E[L²] + E[R²])  ∈ [-1, +1]
//
// 线程安全：pushStereo 音频线程；getSnapshot UI 线程（SpinLock）
// ==========================================================
class PhaseCorrelator
{
public:
    struct Snapshot
    {
        float correlation = 0.0f;  // [-1, +1]
        float width       = 0.0f;  // [0, 1]
        float balance     = 0.0f;  // [-1, +1]  负=偏L 正=偏R
    };

    PhaseCorrelator() = default;
    ~PhaseCorrelator() = default;

    void prepare(double sampleRate);
    void reset();
    void pushStereo(const float* left, const float* right, int numSamples);
    Snapshot getSnapshot() const;

private:
    // EMA 系数（时间常数 ~50ms）
    double alpha = 0.0f;

    // 滑动均方与互相关
    double sLL = 0.0; // E[L²]
    double sRR = 0.0; // E[R²]
    double sLR = 0.0; // E[L·R]
    double sMM = 0.0; // E[mid²]
    double sSS = 0.0; // E[side²]

    mutable juce::SpinLock snapLock;
    Snapshot snap;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaseCorrelator)
};

// ==========================================================
// DynamicRangeMeter —— 实时动态范围检测
//
// 简化版 DR 检测（参考 TT Dynamic Range）：
//   对于立体声，每 100ms 块计算：
//     - blockPeak  = max(|L|, |R|) (dBFS)
//     - blockRms   = sqrt(meanSquare(L+R)/2) (dBFS)
//   维护最近 N=30 块的分布：
//     - shortPeakTop20  = Peak 前 20% 分位数
//     - shortRmsTop20   = RMS  前 20% 分位数
//     - shortDR         = shortPeakTop20 - shortRmsTop20  (dB)
//   全程统计：
//     - integratedDR = 所有块 PeakTop20 - RmsTop20
//   Crest Factor：
//     - crest = peakInstant - rmsInstant (dB)
//
// 线程安全：pushStereo 音频线程；getSnapshot UI 线程（SpinLock）
// ==========================================================
class DynamicRangeMeter
{
public:
    struct Snapshot
    {
        float peakL       = -144.0f; // 当前 100ms 块 peak (dBFS)
        float peakR       = -144.0f;
        float rmsL        = -144.0f;
        float rmsR        = -144.0f;
        float crest       = 0.0f;    // peak - rms (dB)
        float shortDR     = 0.0f;    // 最近 3s 窗口 DR (dB)
        float integratedDR = 0.0f;   // 全程 DR (dB)
    };

    DynamicRangeMeter() = default;
    ~DynamicRangeMeter() = default;

    void prepare(double sampleRate);
    void reset();
    void pushStereo(const float* left, const float* right, int numSamples);
    Snapshot getSnapshot() const;

private:
    double sampleRate = 44100.0;

    // 100ms 块
    int    blockSize     = 0;
    int    blockCounter  = 0;
    float  blockPeakL    = 0.0f;
    float  blockPeakR    = 0.0f;
    double blockSumSqL   = 0.0;
    double blockSumSqR   = 0.0;

    // 短时块队列（30 块≈ 3s）
    static constexpr int shortBlockCount = 30;
    std::deque<float> shortPeakDb;   // 每块 peak（max(L,R)） dB
    std::deque<float> shortRmsDb;    // 每块 rms dB

    // 全程累积（没有上限，使用 vector + 滑动代替逻辑：只保留 peak/rms 的 top有序列表太重，
    // 简化做法：保留 integratedPeakSum / RmsSum 用于粗略全程 DR，以及最大 peak / 均值 rms）
    double integratedSumSq     = 0.0;
    long long integratedSamples = 0;
    float integratedPeakDb     = -144.0f;

    // 快照
    mutable juce::SpinLock snapLock;
    Snapshot snap;

    // 实时 peak/rms（上一块的结果，用于 crest）
    float lastPeakL = -144.0f;
    float lastPeakR = -144.0f;
    float lastRmsL  = -144.0f;
    float lastRmsR  = -144.0f;

    void finishBlock();
    static float percentileTop20(const std::deque<float>& v);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicRangeMeter)
};

// ==========================================================
// AnalyserHub —— 音频分析中心
// ==========================================================
class AnalyserHub
{
public:
    AnalyserHub();
    ~AnalyserHub();

    // ---- 音频线程调用 ----

    // 在 prepareToPlay 时调用，初始化所有采集器
    void prepare(double sampleRate, int samplesPerBlock);

    // 在 processBlock 中调用，推送立体声数据
    // left / right 可以是同一指针（mono 降级）
    void pushStereo(const float* left, const float* right, int numSamples);

    // ======================================================
    // 按需计算（Phase-F 引入）—— 5 路独立的引用计数
    //
    //   目的：不加载的模块不计算。例如界面上只摆了一个 TruePeak 模块，
    //   那 FFT / 示波器 / PhaseCorrelator / DynamicRangeMeter 四路都应该跳过。
    //
    //   模块构造时 retain(Kind::xxx)，析构时 release(Kind::xxx)。
    //   所有 5 路 Kind 的引用计数全 == 0 时，pushStereo 可以（但不会硬性）什么
    //   都不干；任一路 > 0 则对应那一路会跑。
    //
    //   线程模型：retain/release 在 UI 线程调用；pushStereo 在音频线程调用。
    //   通过 std::atomic<int> 保证无锁。
    // ======================================================

    // ---- 常量（FrameSnapshot / 引用计数 / 快照接口都会用到，必须在它们之前可见）----
    static constexpr int oscilloscopeBufferSize = 2048;
    static constexpr int spectrumBins           = 160;

    // ============================================================
    // 三段多分辨率 FFT（基准采样率 = 44.1 kHz）—— 让最终结果精准落到
    //   每一个半音（C0~B10，共 132 格），并尽量减少高频区的旁瓣伪影。
    //
    //   · 高频路 Hi  : 原始采样率（≤44.1k 时为 SR；>44.1k 时先重采样到 44.1k）
    //                  fftSizeHi = 1024，Δf ≈ 43.07 Hz @44.1k
    //                  hop = fftSizeHi / 2（50% overlap）→ 帧间不连续抑制
    //                  目标音域：C8 (4186 Hz) ~ B10 (31609 Hz)
    //   · 中频路 Md  : 抗混叠低通后下采样 D_M=4 倍 → 11025 Hz（覆盖到 ~5 kHz）
    //                  fftSizeMd = 2048，Δf ≈ 5.39 Hz @44.1k
    //                  hop = fftSizeMd / 8（87.5% overlap）
    //                  目标音域：C3 (130.8 Hz) ~ B7 (3951 Hz)
    //   · 低频路 Lo  : 抗混叠低通后下采样 D_L=32 倍 → 1378 Hz
    //                  fftSizeLo = 4096，Δf ≈ 0.336 Hz @44.1k
    //                  hop = 64（≈98.4% overlap）→ 刷新率 ≈ 21.5 Hz @44.1k
    //                  目标音域：C0 (16.35 Hz) ~ B2 (123.5 Hz)
    //
    //   抗混叠滤波：每级降采样前接一个 N 阶 Butterworth 低通；
    //     截止频率 = (新 Nyquist) × kAntiAliasCutoffRatio。
    //
    //   ============================================================
    //   ★★★ Q3：调整抗混叠阶数的唯一位置 ★★★
    //     如果你觉得效果不理想，把 kAntiAliasOrder 改成 12 即可（必须为偶数；
    //     建议 8 / 12 / 16 三档之一）。中频/低频路同时生效。
    //   ============================================================
    static constexpr int   kAntiAliasOrder       = 8;
    static constexpr float kAntiAliasCutoffRatio = 0.85f;
    // ============================================================

    // 三路 FFT 大小（公开常量，便于 FrameSnapshot / UI 复用）
    static constexpr int spectrumMagSizeHi = 1 << 9;   // 1024 / 2 = 512  Hi
    static constexpr int spectrumMagSizeMd = 1 << 10;  // 2048 / 2 = 1024 Md
    static constexpr int spectrumMagSizeLo = 1 << 11;  // 4096 / 2 = 2048 Lo

    // 兼容旧 API：spectrumMagSize 现在指向高频路的 bin 数（旧代码不再被两个频谱模块使用）
    static constexpr int spectrumMagSize   = spectrumMagSizeHi;

    // 中/低频路降采样比（基准 44.1k；其他采样率自适应缩放，比例不变）
    //   D_M=4 让中频流 Nyquist ≈ 5.5k，足以覆盖 C8 (4186Hz) 以下
    //   D_L=32 让低频流 Nyquist ≈ 689Hz，足以覆盖 C5 以下
    static constexpr int kDecimMd = 4;
    static constexpr int kDecimLo = 32;

    // 三路 FFT hop 大小（单位 = 各路自身采样后的样本数）
    //   Hi 路 50% overlap → 减少帧间断点伪影；
    //   Md 路 87.5% overlap → 高刷新率（中频区域是扒谱主战场）；
    //   Lo 路 hop=64 ≈ 98.4% overlap → 把低频路的刷新率从原本 ~0.7 Hz
    //     提升到 ~21.5 Hz @44.1k，避免低音条 "每秒才动一次"。
    static constexpr int kFftHopHi = (1 << 10) / 2;   // 512
    static constexpr int kFftHopMd = (1 << 11) / 8;   // 256
    static constexpr int kFftHopLo = 64;              // 96.875% overlap

    // 132 半音格：C0 (MIDI=12, 16.3516 Hz) ~ B10 (MIDI=143, 31608.5 Hz)
    //   noteBins[n] 对应 MIDI 12+n，频率 = 440 * 2^((MIDI-69)/12)
    static constexpr int   kNoteBinCount = 132;
    static constexpr int   kNoteMidiBase = 12;     // C0
    static constexpr float kNoteRefHz    = 440.0f; // A4
    static constexpr int   kNoteRefMidi  = 69;     // A4

    // 段间分界（半音索引；用于决定每个音符从哪一路取值 + 1 半音 crossfade）
    //   n < kNoteSplitLoMd  → 纯低频路
    //   kNoteSplitLoMd .. kNoteSplitMdHi-1 → 中频路（边缘 1 格与低频/高频路 crossfade）
    //   n >= kNoteSplitMdHi → 纯高频路
    //
    //   注：kNoteSplitMdHi=96 把 C5/C6/C7（最常用的人声/乐器音区）全部交给
    //   分辨率更高的中频路，从而避免 Hi 路 Δf=43Hz 旁瓣污染高半音格。
    static constexpr int   kNoteSplitLoMd = 36;  // C3：低 → 中
    static constexpr int   kNoteSplitMdHi = 96;  // C8：中 → 高

    // 兼容旧字段（getSpectrumMagnitudesBlended 仍在用）
    static constexpr float spectrumXoverHz = 500.0f;

    enum class Kind : int
    {
        Oscilloscope = 0,  // 2×2048 样本环形缓冲
        Spectrum     = 1,  // FFT + magData[1024] + specData[160]
        Loudness     = 2,  // LUFS-M/S/I + RMS L/R + TruePeak L/R
        Phase        = 3,  // correlation + width + balance
        Dynamics     = 4,  // peak/rms/crest/shortDR/integratedDR
        NumKinds     = 5
    };

    // 引用计数 +1。UI 线程调用；多次 retain 同一 Kind 也合法（相当于多个模块共享一路）。
    void retain (Kind kind) noexcept;
    // 引用计数 -1。UI 线程调用；不允许在 count 为 0 时再调（带 jassert）。
    void release(Kind kind) noexcept;

    // 查询某一路是否处于活跃（refCount > 0）。主要供调试 / UI 诊断。
    bool isActive(Kind kind) const noexcept;

    // ======================================================
    // Phase F 聚合帧（Frame Snapshot） —— 计算/显示分离
    //
    //   目的：把"所有模块各自拉快照"统一成"Hub 一帧一份，所有模块共享"。
    //
    //   FrameSnapshot = "本帧"的所有分析值打包（按 activeMask 决定哪些字段有效）。
    //   Hub 内部开一个 30Hz 的 UI 线程 Timer，每帧：
    //     1) 按当前活跃 Kind 把底层计算器里的最新快照拷进一个新的 FrameSnapshot；
    //     2) 以 shared_ptr 原子 swap 进 latestFrame（任何模块都能 getLatestFrame()）；
    //     3) 回调所有注册的 FrameListener::onFrame()（也在 UI 线程）。
    //
    //   这样做的好处：
    //     · osc L/R 2048 样本每帧只拷贝一次（不是 N 个模块各拷一次）；
    //     · Loudness / Phase / Dynamics 的锁每帧只获取一次；
    //     · 模块不再需要各自起 Timer —— 显著降低 wait_for_single_object 开销。
    //
    //   FrameSnapshot 本身不可变：拿到 shared_ptr<const> 的模块可以随意读，
    //   不怕并发。新一帧来时旧的引用自然随 shared_ptr 释放。
    // ======================================================
    struct FrameSnapshot
    {
        // 本帧"哪些 Kind 的字段是有效的"。bit = 1 << (int) Kind::xxx
        juce::uint32 activeMask = 0;

        // 单调递增的 tick 序号（由 FrameDispatcher 每次 timerCallback 自增 1）。
        //   · UI 侧各模块可以据此自行分频：例如 Spectrum/Spectrogram 只在
        //     (tickCount % 2) == 0 时才 repaint，把 60 Hz 分发拆成 30 Hz 实际重绘；
        //     Waveform/Oscilloscope 等需要丝滑滚动的模块仍保持 60 Hz。
        //   · 分频选择放在 listener 侧而不是 dispatcher 侧，是因为不同 module
        //     对刷新率的敏感度差异很大，集中式调度难以普适。
        juce::uint64 tickCount = 0;

        // Oscilloscope：立体声 2048 样本（时间从旧到新）。仅在 activeMask 含 Oscilloscope 时有效。
        std::array<float, oscilloscopeBufferSize> oscL {};
        std::array<float, oscilloscopeBufferSize> oscR {};

        // Spectrum：粗粒度归一化数组（保留给可能的旧消费者）
        std::array<float, spectrumBins>      spectrumData {};

        // 三路 FFT 幅度快照（线性，已 2/N 归一化）
        std::array<float, spectrumMagSizeHi> spectrumMagHi {};
        std::array<float, spectrumMagSizeMd> spectrumMagMd {};
        std::array<float, spectrumMagSizeLo> spectrumMagLo {};

        // 132 半音格幅度（线性；每格 = 该半音 ±50 cent 内峰值）
        //   ★ Spectrum / Spectrogram 模块的最终消费来源 ★
        std::array<float, kNoteBinCount>     spectrumByNote {};
    

        // Loudness / Phase / Dynamics 的快照
        LoudnessMeter::Snapshot      loudness {};
        PhaseCorrelator::Snapshot    phase    {};
        DynamicRangeMeter::Snapshot  dynamics {};

        bool has (Kind kind) const noexcept
        {
            return (activeMask & (juce::uint32) (1u << (int) kind)) != 0;
        }
    };

    // UI 线程调用 FrameListener —— 订阅每帧的聚合快照
    class FrameListener
    {
    public:
        virtual ~FrameListener() = default;
        virtual void onFrame (const FrameSnapshot& frame) = 0;
    };

    // 注册 / 注销（仅 UI 线程；内部用 MessageManagerLock 或 ScopedLock 保护）
    void addFrameListener    (FrameListener* listener);
    void removeFrameListener (FrameListener* listener);

    // Pull 风格：拿到当前最新一帧的共享指针（可能为空 —— 第一帧还未产出时）
    // shared_ptr<const FrameSnapshot>：任何模块都可以安全持有，不怕并发修改。
    std::shared_ptr<const FrameSnapshot> getLatestFrame() const noexcept;

    // 让外部（Editor）可以启停分发器。默认 Hub 构造时就已启动 30Hz Timer。
    void startFrameDispatcher (int hz = 30);
    void stopFrameDispatcher();

    // 查询当前分发频率（Hz）；0 表示尚未启动。
    //   · 供 Editor 端的自适应帧率策略避免重复 startTimerHz 同频。
    int  getFrameDispatcherHz() const noexcept
    {
        return currentFrameDispatcherHz.load (std::memory_order_relaxed);
    }

    // ---- UI 线程调用（快照拷贝，线程安全）----

    // 示波器：返回 L/R 各 oscilloscopeBufferSize 个采样（时间从旧到新）
    void getOscilloscopeSnapshot(juce::Array<float>& destL,
                                 juce::Array<float>& destR);

    // 频谱：返回 spectrumBins 个归一化 [0,1] 幅度值
    void getSpectrumSnapshot(juce::Array<float>& dest);

    // 高频路 1024-FFT 幅度快照（512 bin）—— 供旧代码用
    void getSpectrumMagnitudes(juce::Array<float>& dest);

    // 中频路 2048-FFT 幅度快照（1024 bin，输入流为 SR/D_M）
    void getSpectrumMagnitudesMd(juce::Array<float>& dest);

    // 低频路 4096-FFT 幅度快照（2048 bin，输入流为 SR/D_L）
    void getSpectrumMagnitudesLo(juce::Array<float>& dest);

    // ★★ 132 半音格幅度（C0~B10）—— 推荐 UI 直接使用 ★★
    //   每格输出对应音符的 ±50 cent 频带内峰值（线性幅度）。
    //   段间自动选择低/中/高路并在 1 个半音内 crossfade。
    void getSpectrumMagnitudesByNote (juce::Array<float>& dest);

    // ======================================================
    // 合并（对数频率轴）幅度快照 —— 推荐 UI 频谱/瀑布图直接使用。
    //
    //   内部按 numPoints 个等对数间隔的频率中心 f_i，在 [fMin, fMax] 之间采样：
    //     · f_i < spectrumXoverHz（八度内交叉淡化）         → 从 magDataLo 取
    //     · f_i > spectrumXoverHz（八度内交叉淡化）         → 从 magData   取
    //     · 在过渡带里：两路按能量域线性交叉，避免拼接断崖
    //   返回的 dest[i] 是"以 f_i 为中心的那一段频率"的最大线性幅度（保峰）。
    //
    //   该接口不改变现有 getSpectrumMagnitudes 的语义，老模块可逐步迁移。
    // ======================================================
    void getSpectrumMagnitudesBlended (juce::Array<float>& dest,
                                       int   numPoints,
                                       float fMin,
                                       float fMax);

    // 响度：返回当前 LoudnessMeter 快照
    LoudnessMeter::Snapshot getLoudnessSnapshot();

    // 音频线程调用：重置 Loudness 相关积分/峰值状态（含 LUFS-I / True Peak）
    void resetLoudness() noexcept;

    // 相位相关快照
    PhaseCorrelator::Snapshot getPhaseSnapshot();

    // 动态范围快照
    DynamicRangeMeter::Snapshot getDynamicsSnapshot();

    // ======================================================
    // Phase E —— 性能 profile（原子计数，UI 线程可随时读取）
    // ======================================================
    struct PerfCounters
    {
        double    avgPushUs = 0.0;   // 每次 pushStereo 的平均耗时 (μs)
        double    maxPushUs = 0.0;   // 峰值耗时
        juce::int64 pushCount = 0;   // 累计调用次数

        double    avgSnapUs = 0.0;   // getSpectrumMagnitudes 平均耗时
        juce::int64 snapCount = 0;
    };

    PerfCounters getPerfCounters() const noexcept;
    void         resetPerfCounters() noexcept;

    // ---- 参数查询 ----
    double getSampleRate() const noexcept { return cachedSampleRate; }

private:
    // ---- 内部常量：三路 FFT 阶数 ----
    static constexpr int fftOrderHi = 10;  // 1024
    static constexpr int fftSizeHi  = 1 << fftOrderHi;
    static constexpr int fftOrderMd = 11;  // 2048
    static constexpr int fftSizeMd  = 1 << fftOrderMd;
    static constexpr int fftOrderLo = 12;  // 4096
    static constexpr int fftSizeLo  = 1 << fftOrderLo;

    // 兼容旧名（仅本文件内部使用）
    static constexpr int fftSize  = fftSizeHi;
    static constexpr int fftOrder = fftOrderHi;

    // ---- 示波器（立体声环形缓冲）----
    void pushSamplesToOscilloscope(const float* left, const float* right, int numSamples);

    juce::SpinLock oscLock;
    std::array<float, oscilloscopeBufferSize> oscBufL {};
    std::array<float, oscilloscopeBufferSize> oscBufR {};
    int oscWritePos = 0;

    // ---- 频谱（单声道混合，FFT）----
    //
    //   总流程：
    //     audio mid → [SR>44.1k? 4× 多相重采样到 44.1k] → highStream
    //     highStream → 喂 fftFifoHi（1024，hop=kFftHopHi=50% overlap）
    //     highStream → AA-LP-Md → ÷kDecimMd → 喂 fftFifoMd（2048，hop=kFftHopMd=87.5% overlap）
    //     midStream  → AA-LP-Lo → ÷(kDecimLo/kDecimMd) → 喂 fftFifoLo（4096，hop=kFftHopLo）
    //
    //   注：每个 IIR 抗混叠链的阶数 = kAntiAliasOrder（见公开常量区注释）。
    //   一个 N 阶 Butterworth 在 juce::dsp::IIR 里 = (N/2) 个 biquad cascade。
    void pushSamplesToSpectrum (const float* left, const float* right, int numSamples);

    // 只保护发布给 UI 的频谱快照数组；FFT/FIFO 工作状态由音频线程单写。
    juce::SpinLock specLock;

    // ---- 三路 FFT 工作器 ----
    juce::dsp::FFT fftHi { fftOrderHi };
    juce::dsp::FFT fftMd { fftOrderMd };
    juce::dsp::FFT fftLo { fftOrderLo };

    juce::dsp::WindowingFunction<float> windowHi { fftSizeHi, juce::dsp::WindowingFunction<float>::hann };
    juce::dsp::WindowingFunction<float> windowMd { fftSizeMd, juce::dsp::WindowingFunction<float>::hann };
    juce::dsp::WindowingFunction<float> windowLo { fftSizeLo, juce::dsp::WindowingFunction<float>::hann };

    // FIFO（高频路非重叠；中/低频路环形 + 75% overlap）
    // Hi 路改为环形 FIFO + 50% overlap（解决 C5 等单音注入 C6~C9 的旁瓣伪影）
    std::array<float, fftSizeHi>     fftFifoHi {};
    std::array<float, fftSizeHi * 2> fftDataHi {};
    int fftFifoIndexHi = 0;   // 环形写指针（0..fftSizeHi-1）
    int hopAccumHi     = 0;   // 每收满 kFftHopHi 个新样本就跑一次 FFT

    std::array<float, fftSizeMd>     fftFifoMd {};
    std::array<float, fftSizeMd * 2> fftDataMd {};
    int fftFifoIndexMd = 0;   // 环形写指针
    int decimCounterMd = 0;   // 抽 D_M 个样本只往 fftFifoMd 写 1 个

    std::array<float, fftSizeLo>     fftFifoLo {};
    std::array<float, fftSizeLo * 2> fftDataLo {};
    int fftFifoIndexLo = 0;
    int decimCounterLo = 0;

    // 三路输出幅度（线性）—— work 是音频线程私有，magXxx 是发布给 UI 的快照
    std::array<float, spectrumMagSizeHi> magDataHi {};
    std::array<float, spectrumMagSizeHi> magDataHiWork {};
    std::array<float, spectrumMagSizeMd> magDataMd {};
    std::array<float, spectrumMagSizeMd> magDataMdWork {};
    std::array<float, spectrumMagSizeLo> magDataLo {};
    std::array<float, spectrumMagSizeLo> magDataLoWork {};

    // 132 半音格幅度（线性；从三路幅度合成；UI 直接消费）
    std::array<float, kNoteBinCount> noteMagPublished {};

    // 粗粒度归一化谱（保留给可能的旧消费者）
    std::array<float, spectrumBins>  specData {};
    std::array<float, spectrumBins>  specDataWork {};

    // 兼容旧名：FrameDispatcher 拷贝路径中使用 magData 变量名的地方后面会改成 magDataHi。
    // 这里不再保留别名，避免与 std::array 引用字段带来拷贝/初始化坑。

    // ---- 抗混叠 IIR 链（中/低频路）----
    //   每条链 = (kAntiAliasOrder / 2) 个 biquad；
    //   prepare() 里按当前采样率 + 目标 D_M / D_L 重新设计系数。
    //
    //   实现细节：
    //     · 中频路：在 highStream（≤44.1kHz）上做 LP（截止 ~ SR_hi/(2*D_M)*ratio），
    //       然后每 D_M 个样本抽 1。
    //     · 低频路：复用中频路的输出（已经 LP 到 ~Nyq/D_M），再做一级 LP（截止 ~ SR_md/(2*D_L/D_M)*ratio），
    //       然后每 (D_L/D_M) 个样本抽 1。两级级联可有效压低过渡带。
    //
    //   使用 juce::dsp::IIR::Filter 的 stage cascade。每路最多支持 16 阶 → 8 个 biquad。
    static constexpr int kMaxBiquadStages = 8;  // ≥ kAntiAliasOrder/2
    using BiquadF = juce::dsp::IIR::Filter<float>;
    std::array<BiquadF, kMaxBiquadStages> aaFilterMd;        // 中频路抗混叠（44.1k → 5.5k）
    std::array<BiquadF, kMaxBiquadStages> aaFilterLo;        // 低频路抗混叠（5.5k → 689）
    int  aaActiveStages = 0;  // = kAntiAliasOrder / 2

    // ---- 输入端可选重采样：若来料 SR > 44.1k，则先降到 44.1k 再分析 ----
    //   仅作用于分析路径，主音频输出不动。
    //   实现：4 阶 Butterworth LP（截止 = 44100/2 * 0.85 ≈ 18743 Hz）→ 多相抽取。
    //   实际抽取比 = SR / 44100（可能是 2.0 / 4.0 / 4.35... 非整数 → 用 1D 线性插值的多项式相位采样）。
    static constexpr float kAnalysisTargetSR = 44100.0f;
    bool   inputResampleEnabled = false;       // SR > 44.1k 时为 true
    double inputDecimRatio      = 1.0;         // = SR / 44100
    double inputDecimPhase      = 0.0;         // 多相累加相位 [0,1)
    float  inputResampleZ1      = 0.0f;        // 上一个输入样本（线性插值用）
    std::array<BiquadF, 4> inputPreLp;         // 4 阶 anti-image LP
    int    inputPreLpStages     = 0;

    // 兼容旧名（getSpectrumMagnitudes 依然返回它）
    static constexpr int fftSizeLegacy = fftSizeHi;

    // ---- 响度计 ----
    LoudnessMeter loudnessMeter;

    // ---- 相位相关仪 ----
    PhaseCorrelator phaseCorrelator;

    // ---- 动态范围计 ----
    DynamicRangeMeter dynamicsMeter;

    // ---- 采样率缓存 ----
    double cachedSampleRate = 44100.0;

    // ---- Phase E 性能计数（音频线程写，UI 线程读；用 atomic 保证无锁）----
    std::atomic<juce::int64> perfPushCount  { 0 };
    std::atomic<juce::int64> perfPushTotalUs{ 0 };
    std::atomic<juce::int64> perfPushMaxUs  { 0 };
    std::atomic<juce::int64> perfSnapCount  { 0 };
    std::atomic<juce::int64> perfSnapTotalUs{ 0 };

    // ---- Phase F 按需计算：5 路独立引用计数 ----
    //   UI 线程 retain/release 时做 +/-1；音频线程 pushStereo 时 load()
    //   决定是否跳过某一路计算。std::atomic<int> 保证跨线程无锁安全。
    std::array<std::atomic<int>, (size_t) Kind::NumKinds> refCounts {};

    // ---- Phase F 聚合帧分发器 ----
    //   内部 Timer（UI 线程）每 33ms 聚合一次 FrameSnapshot，
    //   然后 swap 进 latestFrame + 依次调用 frameListeners。
    //
    //   为了把 juce::Timer 隐藏在 AnalyserHub.cpp 里（避免 .h 必须 include
    //   juce_events + juce_gui_basics），这里用 pimpl 持有。
    class FrameDispatcher;
    std::unique_ptr<FrameDispatcher> frameDispatcher;

    // 注册的 listeners —— 列表只在 UI 线程读写（由 dispatcher 保证）
    std::vector<FrameListener*> frameListeners;
    juce::CriticalSection        frameListenersLock;
    // fanout 时拷贝列表所需的目标容量（滚动水位）。
    //   · 在 add/remove Listener 时更新，避免 fanout 每帧 vector::reserve 重分配。
    int                          frameListenersReserved = 0;

    // 最新一帧 —— UI 线程写（dispatcher），任意线程读
    //   注：MSVC C++17 下 std::atomic<std::shared_ptr<T>> 不可用；
    //   改用 SpinLock + shared_ptr swap 实现原子发布。
    std::shared_ptr<const FrameSnapshot> latestFrame;
    mutable juce::SpinLock               latestFrameLock;

    // 当前分发频率（Hz）—— 供 UI 自适应策略查询，避免重复 startTimerHz。
    //   初值 0 表示尚未启动，首次 startFrameDispatcher 一定会生效。
    std::atomic<int>                     currentFrameDispatcherHz { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalyserHub)
};

#endif // PBEQ_ANALYSER_HUB_H_INCLUDED
