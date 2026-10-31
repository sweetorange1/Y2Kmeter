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
//   - 约 3.2s 滑动窗口（Short-term LUFS，8×400ms 块）
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
        float lufsS     = -144.0f;   // Short-term LUFS（约 3.2s）
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

    // 400ms 块（Momentary，供 Short-term / Integrated 做块级累积）
    int momentaryBlockSize  = 0;   // 采样数
    int momentaryWriteCount = 0;
    double momentarySumL    = 0.0;
    double momentarySumR    = 0.0;

    // Momentary 实时滑动窗口（400ms，逐样本滑动）：LUFS-M 用它做实时响度，
    //   信号停止后旧样本被逐样本替换，最多 400ms 内归零（不再依赖块凑满）。
    int    momentaryWindowSize = 0;
    std::deque<double> momentaryHistL;
    std::deque<double> momentaryHistR;
    double momentarySlideSumL = 0.0;
    double momentarySlideSumR = 0.0;

    // 短期（Short-term）：用 400ms 块的滑动队列（8 块 ≈ 3.2s）
    static constexpr int shortTermBlocks = 8;
    std::deque<double> shortTermBlocksL;
    std::deque<double> shortTermBlocksR;

    // 积分（Integrated）：BS.1770-4 两阶段门限
    //   · 绝对门限：blockMean >= absGateThreshold（-70 LUFS）
    //   · 相对门限：对通过绝对门限的所有块求平均，再 -10 LU（×0.1）
    //   · 最终 Integrated = 对 blockMean >= 相对门限 的块求平均
    std::deque<double> gatedBlocks; // 通过绝对门限的 400ms 块能量（左右合成均方）
    // 绝对门限（-45 LUFS 线性均方 ≈ 10^(-45/10)）。原 -70 LUFS 太深，会导致
    //   loopback / 输入本底噪声（约 -55 dB）也被计入 Integrated，I 无法归零。
    static constexpr double absGateThreshold = 3.162e-5;

    // ---- Integrated 静音自动重置 ----
    //   连续 silenceResetSeconds 秒无音频信号（合成 RMS 均方 < silenceMeanSqThreshold）
    //   时，自动清空 integrated 积分，下次出现信号时重新开始累积。
    //   目的：DAW 停止播放 / loopback 静音后，旧的节目响度不应继续污染新段落。
    static constexpr double silenceMeanSqThreshold = 3.162e-5; // -45 dBFS 线性均方
    static constexpr double silenceResetSeconds    = 2.0;       // 静音 2s 后重置 Integrated
    double silenceSamples = 0.0; // 连续静音累计样本数

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

    // ---- 全程 DR 统计：固定分箱直方图 ----
    //   旧实现把每块 peak/rms 塞进两个 std::deque，并在每个 100ms 块里拷进
    //   std::vector 全量排序（percentileTop20）——音频线程上既分配又 O(N log N)，
    //   且 deque 无上限（10 块/秒 → 每小时 3.6 万个元素），长时间运行会持续变慢。
    //   改为 0.5 dB 一箱、覆盖 [-144, 0] dB 的定长直方图：恒定内存、零分配、
    //   每块 O(箱数) 统计，结果与全量排序的 top-20% 均值一致（误差 ≤ 0.25 dB）。
    static constexpr float drHistBinDb = 0.5f;
    static constexpr float drHistMinDb = -144.0f;
    static constexpr float drHistMaxDb = 0.0f;
    static constexpr int   drHistBins  = 289;   // (0 - (-144)) / 0.5 + 1
    std::array<int, drHistBins> allPeakHist {};
    std::array<int, drHistBins> allRmsHist  {};

    // ---- Integrated DR 静音自动重置 ----
    //   连续 silenceResetSeconds 秒无音频信号（块合成 RMS 均方 < silenceMeanSqThreshold）
    //   时清空 integrated 累积，下次有信号重新累积。
    static constexpr double silenceMeanSqThreshold = 1.0e-6; // -60 dBFS 线性均方
    static constexpr double silenceResetSeconds    = 5.0;
    double silenceSamples = 0.0; // 连续静音累计样本数

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

    // 直方图写入 / 上尾 20% 均值（integrated DR 用，零分配）
    static void  addToHist (std::array<int, drHistBins>& h, float db) noexcept;
    static float percentileTop20FromHist (const std::array<int, drHistBins>& h) noexcept;

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
    // 兼容快照尺寸（固定 1024）：FrameSnapshot.spectrumMag / GpuUniforms / getSpectrumMagnitudes
    // 仍按此尺寸发布。主路 FFT 调大后（反馈 #7），该数组只取前 1024 bin（兼容层），
    // 全分辨率数据由 getSpectrumMagnitudesBlended 直接读动态尺寸的 magData。
    static constexpr int spectrumMagSize        = 1 << 10;

    // ======================================================
    // 单路自适应 FFT（v2.9.6 合并低频路）：
    //   · 唯一一路 FFT：尺寸用户可选（1024/2048/4096/8192/16384，setMainFftSize）
    //   · 75% overlap（hop = size/4）：大尺寸下仍保持较快刷新，且高低频段
    //     刷新节奏天然一致；同时 hop=size/4 保证重分配谱图的无折叠范围
    //     恒为 ±2 bin（覆盖 Hann 主瓣，Sharp/Sharper 无折叠伪影）
    //   · 一次 FFT 同时产出：幅度（blended）、复频谱帧对（重分配谱图）、
    //     160 bin 粗粒度谱（EqModule）
    // ======================================================

    enum class Kind : int
    {
        Oscilloscope = 0,  // 2×2048 样本环形缓冲
        Spectrum     = 1,  // FFT + magData + specData[160] + 复频谱帧对
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

        // v2.9 帧时钟：本帧呈现时间（秒，单调递增）与距上一帧的间隔（秒）。
        //   · vblank 驱动时 timeSec 取显示器 vblank 时间戳，Timer 回退时取高精度时钟；
        //   · dtSec 已钳制到 [1/1000, 0.1]，模块做"与帧率无关"的平滑/衰减/滚动时
        //     直接用它（例如 a = 1 - pow(1 - a60, dtSec * 60)），避免 60→120Hz 后
        //     动画速度/衰减时间常数随帧率变化。
        double timeSec = 0.0;
        float  dtSec   = 1.0f / 60.0f;

        // Oscilloscope：立体声 2048 样本（时间从旧到新）。仅在 activeMask 含 Oscilloscope 时有效。
        std::array<float, oscilloscopeBufferSize> oscL {};
        std::array<float, oscilloscopeBufferSize> oscR {};

        // Spectrum：1024 个高精度 mag（线性）+ 160 个粗粒度归一化 specData
        std::array<float, spectrumMagSize>  spectrumMag  {};
        std::array<float, spectrumBins>     spectrumData {};

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

    // ---- v2.9 外部帧时钟（vblank 同步）----
    //   Editor 挂 juce::VBlankAttachment 后调用 setExternalFrameClock(true)：
    //   内部 Timer 停止，改由 Editor 在每个（按 FPS 上限抽稀后的）vblank 回调里
    //   调用 dispatchFrameNow()。这样分发节拍与显示器刷新严格同相，消除
    //   "8ms Timer(125Hz) vs 120Hz 刷新"之间的拍频抖动（周期性一帧双更/一帧空更）。
    //   vblank 长时间不来（显示器休眠 / 远程桌面 / 无 peer）时 Editor 会切回
    //   setExternalFrameClock(false)，内部 Timer 以 startFrameDispatcher 记录的频率接管。
    //   均只能在 UI 线程调用。
    void setExternalFrameClock (bool shouldUseExternalClock);
    bool isUsingExternalFrameClock() const noexcept { return externalFrameClock; }
    // timestampSec < 0 表示使用当前高精度时钟。
    void dispatchFrameNow (double timestampSec = -1.0);

    // ---- UI 线程调用（快照拷贝，线程安全）----

    // 示波器：返回 L/R 各 oscilloscopeBufferSize 个采样（时间从旧到新）
    void getOscilloscopeSnapshot(juce::Array<float>& destL,
                                 juce::Array<float>& destR);

    // 频谱：返回 spectrumBins 个归一化 [0,1] 幅度值
    void getSpectrumSnapshot(juce::Array<float>& dest);

    // 高精度频谱幅度快照：返回 fftSize/2 = 1024 个原始幅度值
    // （未做对数/平滑，供 SpectrumModule 自行处理）
    void getSpectrumMagnitudes(juce::Array<float>& dest);

    // ======================================================
    // 对数频率轴幅度快照 —— 推荐 UI 频谱/瀑布图直接使用。
    //   v2.9.6 单路化：直接在主路 magData 上做对数采样，不再交叉合并双路。
    //
    //   内部按 numPoints 个等对数间隔的频率中心 f_i，在 [fMin, fMax] 之间采样；
    //   返回的 dest[i] 是"以 f_i 为中心的那一段频率"的最大线性幅度（保峰）。
    //   分辨率完全由当前主路 FFT size 决定（分辨率 vs 刷新率由用户权衡）。
    // ======================================================
    void getSpectrumMagnitudesBlended (juce::Array<float>& dest,
                                       int   numPoints,
                                       float fMin,
                                       float fMax);

    // 双通道对数频率轴幅度快照（L/R 分开）：
    //   与 getSpectrumMagnitudesBlended 使用同一频率轴与带宽聚合逻辑，
    //   仅数据源改为左/右声道各自的 magData。
    void getSpectrumMagnitudesBlendedLR (juce::Array<float>& destL,
                                         juce::Array<float>& destR,
                                         int   numPoints,
                                         float fMin,
                                         float fMax);

    // ======================================================
    // 重分配谱图（频率重分配 / reassigned spectrogram）——
    //   推荐 Spectrogram 的 Sharp / Sharper 模式使用。
    //
    //   与 getSpectrumMagnitudesBlended 相同的对数频率轴，但能量按
    //   "瞬时频率"重定位（相位差法，phase-vocoder 风格）：
    //     ω̂_k = ω_k + Δφ_k / hopTime,  Δφ_k = arg(X_k · conj(X_prev,k))
    //   把每个 bin 的能量 |X_k|² 从"bin 中心频率"搬到"瞬时频率"，
    //   落到对应对数行 —— 产生"能量凝聚成粒子"的效果。
    //
    //   v2.9.6 单路化：改用主路复频谱帧对（主路 75% overlap，hop=size/4，
    //   无折叠范围恒为 ±2 bin，覆盖 Hann 主瓣，无折叠伪影）。
    // ======================================================
    void getSpectrumReassignedBlended (juce::Array<float>& dest,
                                       int   numPoints,
                                       float fMin,
                                       float fMax);

    // ======================================================
    // 重分配谱峰（Spectrum 模块 Sharper 模式用）——
    //   把当前帧的乐音成分收束成"谱线列表"。
    //
    //   与 getSpectrumReassignedBlended 同一套相位差重分配，但输出不是
    //   对数行能量，而是逐根谱峰：
    //     · freqHz：相干段内按能量加权的瞬时频率（亚 bin 精度）；
    //     · mag：该谱峰凝聚的线性幅度（与 blended 同量纲，2/N 归一化）。
    //   算法：逐 bin 瞬时频率 → 绝对/相对门限 → 相邻 bin 相干性聚合为段
    //     → 按能量取前 maxPeaks 个。
    //   共享主路复频谱帧对，零新增 FFT 计算。
    // ======================================================
    struct ReassignedPeak
    {
        double freqHz = 0.0;
        float  mag    = 0.0f;
    };
    void getSpectrumReassignedPeaks (std::vector<ReassignedPeak>& out,
                                     float fMinHz,
                                     float fMaxHz,
                                     int   maxPeaks);

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
    double getSampleRate() const noexcept { return cachedSampleRate.load (std::memory_order_relaxed); }

    // ======================================================
    // 单路 FFT size 选择（反馈 #7）
    //   UI 线程调用 setMainFftSize：档位 1024/2048/4096/8192/16384（2 的幂）。
    //   音频线程在下一轮 pushSamplesToSpectrum 开始时按新 size 重建
    //   FFT / 窗口 / 环形 FIFO / magData / 复频谱帧对，真正作用到后端。
    // ======================================================
    void setMainFftSize (int size) noexcept;
    int  getMainFftSize() const noexcept { return mainFftSizeRequest.load (std::memory_order_relaxed); }

private:
    // ---- 单路 FFT 尺寸（运行时可变）----
    static constexpr int mainFftSizeDefault = 2048;
    std::atomic<int>     mainFftSizeRequest { mainFftSizeDefault };  // UI 线程写 / 音频线程读
    int mainFftSize  = mainFftSizeDefault;   // 音频线程实际生效的尺寸
    int mainFftOrder = 11;
    int mainHop      = mainFftSizeDefault / 4;   // 75% overlap：hop = size/4

    // ---- 示波器（立体声环形缓冲）----
    void pushSamplesToOscilloscope(const float* left, const float* right, int numSamples);

    juce::SpinLock oscLock;
    std::array<float, oscilloscopeBufferSize> oscBufL {};
    std::array<float, oscilloscopeBufferSize> oscBufR {};
    int oscWritePos = 0;

    // ---- 频谱（单声道混合，FFT）----
    void pushSamplesToSpectrum(const float* left, const float* right, int numSamples);

    // 只保护发布给 UI 的频谱快照数组；FFT/FIFO 工作状态由音频线程单写。
    juce::SpinLock specLock;
    // 单路 FFT：尺寸运行时可调（反馈 #7），故 FFT/窗口/缓冲全部动态分配
    std::unique_ptr<juce::dsp::FFT> fft;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
    std::vector<float> fftFifo;     // mainFftSize（环形，75% overlap）
    std::vector<float> fftData;     // mainFftSize * 2
    std::vector<float> fftFifoL;    // 左声道环形 FIFO（mainFftSize）
    std::vector<float> fftFifoR;    // 右声道环形 FIFO（mainFftSize）
    std::vector<float> fftDataL;    // 左声道 FFT 工作区（mainFftSize * 2）
    std::vector<float> fftDataR;    // 右声道 FFT 工作区（mainFftSize * 2）
    std::array<float, spectrumBins> specData {};
    std::array<float, spectrumBins> specDataWork {};
    // 高精度 FFT 幅度缓冲（尺寸 = mainFftSize/2，随 FFT size 变化）
    std::vector<float> magData;
    std::vector<float> magDataWork;
    std::vector<float> magDataL;
    std::vector<float> magDataR;
    std::vector<float> magDataWorkL;
    std::vector<float> magDataWorkR;
    int fftFifoIndex = 0;

    // 主路复频谱帧对（供重分配谱图：hop = size/4 → 无折叠范围恒 ±2 bin，
    // 覆盖 Hann 主瓣，无折叠伪影）。
    //   音频线程私有的"帧对"：一个音频块可能触发 ≥2 次 FFT，发布时若只滚一次
    //   会跳帧（帧对时间差 ≠ hop，瞬时频率系数就错了）。提取新帧时先把旧
    //   workCur 挪到 workPrev，保证发布出去的 prev/current 永远相邻。
    std::vector<float> specReWork;       // 工作区：当前帧 re（mainFftSize/2）
    std::vector<float> specImWork;       // 工作区：当前帧 im
    std::vector<float> specReWorkPrev;   // 工作区：上一帧 re
    std::vector<float> specImWorkPrev;   // 工作区：上一帧 im
    std::vector<float> specRe;           // 发布区（specLock 保护）
    std::vector<float> specIm;
    std::vector<float> prevSpecRe;
    std::vector<float> prevSpecIm;

    // 按指定 order 重建主路 FFT / 窗口 / 工作缓冲（音频线程）
    void rebuildMainFft (int order);
    // 检查 mainFftSizeRequest 是否变化，变化则重建（音频线程，push 开头调用）
    void applyMainFftSizeChange();

    // ---- 响度计 ----
    LoudnessMeter loudnessMeter;

    // ---- 相位相关仪 ----
    PhaseCorrelator phaseCorrelator;

    // ---- 动态范围计 ----
    DynamicRangeMeter dynamicsMeter;

    // ---- 采样率缓存 ----
    //   prepare() 在音频线程准备阶段写入，getSampleRate()/频谱混合在 UI 线程读取，
    //   用 std::atomic<double> 消除跨线程数据竞争（无锁，读写均 relaxed 即可）。
    std::atomic<double> cachedSampleRate { 44100.0 };

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

    // 外部帧时钟是否接管（UI 线程读写）
    bool                                 externalFrameClock = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalyserHub)
};

#endif // PBEQ_ANALYSER_HUB_H_INCLUDED
