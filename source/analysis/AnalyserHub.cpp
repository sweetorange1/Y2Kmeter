#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include "source/perf/PerformanceCounterSystem.h"

// ==========================================================
// Phase F —— FrameDispatcher 内部实现类
//
//   UI 线程上的 juce::Timer；每 33ms：
//     1) 读取当前 refCounts → 决定哪些字段要填；
//     2) 从 loudness / phase / dynamics / osc / spec 拉快照，组装一个
//        新的 FrameSnapshot（immutable）；
//     3) 原子 swap 到 AnalyserHub::latestFrame；
//     4) 依次调用所有注册的 FrameListener::onFrame()。
//
//   把 juce::Timer 定义放在 .cpp 里实现 pimpl，避免 AnalyserHub.h 依赖
//   juce_events / juce_gui_basics。
// ==========================================================
class AnalyserHub::FrameDispatcher : public juce::Timer
{
public:
    explicit FrameDispatcher (AnalyserHub& ownerRef) : owner (ownerRef) {}

    ~FrameDispatcher() override { stopTimer(); }

    void timerCallback() override
    {
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::ui, "UI-FrameDispatcher");
        y2k::perf::ScopedPerfTimer frameTimer(y2k::perf::FunctionId::uiFrameDispatcher,
                                              y2k::perf::Partition::uiRendering,
                                              y2k::perf::ThreadRole::ui);
#endif

        // 1) 组装活跃 mask
        juce::uint32 mask = 0;
        for (int i = 0; i < (int) Kind::NumKinds; ++i)
            if (owner.refCounts[(size_t) i].load (std::memory_order_relaxed) > 0)
                mask |= (juce::uint32) (1u << i);

        if (mask == 0)
            return; // 所有路都无人订阅 —— 不生成帧

        auto frame = std::make_shared<FrameSnapshot>();
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordMemoryAlloc(sizeof(FrameSnapshot));
#endif
        frame->activeMask = mask;
        frame->tickCount  = ++dispatchTickCounter; // UI 侧模块自行分频

#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::ScopedPerfTimer assembleTimer(y2k::perf::FunctionId::uiFrameAssemble,
                                                 y2k::perf::Partition::uiRendering,
                                                 y2k::perf::ThreadRole::ui);
#endif

        // 2) 按需填充各路数据
        if (frame->has (Kind::Oscilloscope))
        {
#if Y2K_ENABLE_PERF_COUNTERS
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
#endif
            const juce::SpinLock::ScopedLockType sl (owner.oscLock);
#if Y2K_ENABLE_PERF_COUNTERS
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockOsc,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
#endif
            // Phase F 优化：直接对 frame->oscL/oscR 的 std::array 做分段 memcpy，
            //   省掉"getOscilloscopeSnapshot → juce::Array → std::array"这一层中转。
            const int bufSize    = oscilloscopeBufferSize;
            const int firstChunk = bufSize - owner.oscWritePos;
            std::memcpy (frame->oscL.data(),              owner.oscBufL.data() + owner.oscWritePos,
                         (size_t) firstChunk * sizeof (float));
            std::memcpy (frame->oscR.data(),              owner.oscBufR.data() + owner.oscWritePos,
                         (size_t) firstChunk * sizeof (float));
            if (owner.oscWritePos > 0)
            {
                std::memcpy (frame->oscL.data() + firstChunk, owner.oscBufL.data(),
                             (size_t) owner.oscWritePos * sizeof (float));
                std::memcpy (frame->oscR.data() + firstChunk, owner.oscBufR.data(),
                             (size_t) owner.oscWritePos * sizeof (float));
            }
        }

        if (frame->has (Kind::Spectrum))
        {
            // Phase F 优化：直接 memcpy（尺寸 + 类型都与 std::array 内部缓冲一致）
#if Y2K_ENABLE_PERF_COUNTERS
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
#endif
            const juce::SpinLock::ScopedLockType sl (owner.specLock);
#if Y2K_ENABLE_PERF_COUNTERS
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockSpec,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
#endif
            std::memcpy (frame->spectrumData .data(), owner.specData.data(),
                         sizeof (float) * frame->spectrumData.size());
            std::memcpy (frame->spectrumMagHi.data(), owner.magDataHi.data(),
                         sizeof (float) * frame->spectrumMagHi.size());
            std::memcpy (frame->spectrumMagMd.data(), owner.magDataMd.data(),
                         sizeof (float) * frame->spectrumMagMd.size());
            std::memcpy (frame->spectrumMagLo.data(), owner.magDataLo.data(),
                         sizeof (float) * frame->spectrumMagLo.size());
            std::memcpy (frame->spectrumByNote.data(), owner.noteMagPublished.data(),
                         sizeof (float) * frame->spectrumByNote.size());
        }

        if (frame->has (Kind::Loudness))
            frame->loudness = owner.loudnessMeter.getSnapshot();

        if (frame->has (Kind::Phase))
            frame->phase    = owner.phaseCorrelator.getSnapshot();

        if (frame->has (Kind::Dynamics))
            frame->dynamics = owner.dynamicsMeter.getSnapshot();

        // 3) 原子发布到 latestFrame
        {
#if Y2K_ENABLE_PERF_COUNTERS
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
#endif
            const juce::SpinLock::ScopedLockType sl (owner.latestFrameLock);
#if Y2K_ENABLE_PERF_COUNTERS
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockLatestFrame,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
#endif
            owner.latestFrame = frame;
        }

#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordEvent(
            y2k::perf::FunctionId::dataPublishLatestFrame,
            y2k::perf::Partition::dataCommunication,
            y2k::perf::ThreadRole::ui,
            1);
#endif

        // 4) 通知所有监听器（UI 线程）
        //    拷贝一份 vector 避免在回调里触发 add/remove 导致迭代失效
        std::vector<FrameListener*> localCopy;
        {
#if Y2K_ENABLE_PERF_COUNTERS
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
#endif
            const juce::ScopedLock sl (owner.frameListenersLock);
#if Y2K_ENABLE_PERF_COUNTERS
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockFrameListeners,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
#endif
            localCopy = owner.frameListeners;
        }

        // 空 listener 列表直接返回，避免后续 reserve/活跳 component 检查等无谓开销。
        if (localCopy.empty())
            return;

#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordDuration(
            y2k::perf::FunctionId::dataListenerListCopy,
            y2k::perf::Partition::dataCommunication,
            y2k::perf::ThreadRole::ui,
            0,
            0);
#endif

        std::vector<FrameListener*> activeListeners;
        // 用 reserved 水位避免反复扩容；push后不会 shrink 回去。
        activeListeners.reserve ((size_t) juce::jmax (owner.frameListenersReserved,
                                                       (int) localCopy.size()));
        for (auto* lst : localCopy)
        {
            if (lst == nullptr)
                continue;

            bool active = true;
            if (auto* comp = dynamic_cast<juce::Component*>(lst))
            {
                if (! comp->isShowing() || comp->getWidth() <= 0 || comp->getHeight() <= 0)
                {
                    active = false;
                }
                else if (auto* p = comp->getParentComponent())
                {
                    if (comp->getBounds().getIntersection(p->getLocalBounds()).isEmpty())
                        active = false;
                }
            }

            if (active)
                activeListeners.push_back(lst);
        }

#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordFrameListenerCount((int) activeListeners.size());
        y2k::perf::PerformanceCounterSystem::instance().recordEvent(
            y2k::perf::FunctionId::uiFrameListenerFanout,
            y2k::perf::Partition::uiRendering,
            y2k::perf::ThreadRole::ui,
            (juce::int64) activeListeners.size());
#endif
        for (auto* lst : activeListeners)
        {
#if Y2K_ENABLE_PERF_COUNTERS
            y2k::perf::ScopedPerfTimer onFrameTimer(y2k::perf::FunctionId::uiOnFrameDispatch,
                                                    y2k::perf::Partition::uiRendering,
                                                    y2k::perf::ThreadRole::ui);
#endif
            lst->onFrame (*frame);
        }

#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordMemoryFree(sizeof(FrameSnapshot));
#endif
    }

private:
    AnalyserHub& owner;

    // 单调递增的 tick 序号，每次 timerCallback 自增 1 并写入 FrameSnapshot，
    //   供 UI 侧各模块自行分频（如 Spectrum/Spectrogram 每 2 帧实际 repaint 一次）。
    juce::uint64 dispatchTickCounter = 0;
};

// ==========================================================
// AnalyserHub
// ==========================================================
AnalyserHub::AnalyserHub()
{
    // 构造即起一个 30Hz 的 UI 分发器；若某段时间没有任何模块订阅，
    // timerCallback 会直接 return，开销可忽略。
    frameDispatcher = std::make_unique<FrameDispatcher> (*this);
    // 真正的 startTimer 延后到 startFrameDispatcher() 显式调用，
    // 避免"构造期间触发 Timer 回调，但 AnalyserHub 还没完全构造好"。
}

// 需要显式定义（std::unique_ptr<不完整类型 FrameDispatcher> 在头里无法自动析构）
AnalyserHub::~AnalyserHub()
{
    if (frameDispatcher != nullptr)
        frameDispatcher->stopTimer();
}

void AnalyserHub::prepare(double sampleRate, int samplesPerBlock)
{
    cachedSampleRate = sampleRate;

    // ====================================================================
    // 输入重采样器 + 三路 FFT 抗混叠链 初始化
    // ====================================================================
    inputResampleEnabled = (sampleRate > (double) kAnalysisTargetSR + 1.0);
    inputDecimRatio      = juce::jmax (1.0, sampleRate / (double) kAnalysisTargetSR);
    inputDecimPhase      = 0.0;
    inputResampleZ1      = 0.0f;

    // 输入端 anti-image LP（仅 SR > 44.1k 时必要）：4 阶 Butterworth，截止 = 44100/2 * ratio
    inputPreLpStages = 0;
    if (inputResampleEnabled)
    {
        const double sr        = sampleRate;
        const double cutoffHz  = 0.5 * (double) kAnalysisTargetSR * (double) kAntiAliasCutoffRatio;
        const auto   coeffs    = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (
                                    (float) cutoffHz, (float) sr, /*order*/ 4);
        // designIIRLowpassHighOrderButterworthMethod 返回一组 biquad（阶数/2个）
        const int n = juce::jmin ((int) inputPreLp.size(), coeffs.size());
        for (int i = 0; i < n; ++i)
        {
            inputPreLp[(size_t) i].coefficients = coeffs[i];
            inputPreLp[(size_t) i].reset();
        }
        inputPreLpStages = n;
    }

    // 中/低频路的抗混叠链 —— 根据 kAntiAliasOrder 个级数
    aaActiveStages = juce::jlimit (1, (int) kMaxBiquadStages, kAntiAliasOrder / 2);

    // 中频路：在 "高频流 SR_hi" 上 LP。
    //   SR_hi = inputResampleEnabled ? kAnalysisTargetSR : sampleRate
    const double srHi   = inputResampleEnabled ? (double) kAnalysisTargetSR : sampleRate;
    const double srMd   = srHi / (double) kDecimMd;            // 中频路输出采样率
    const double srLo   = srHi / (double) kDecimLo;            // 低频路输出采样率

    const double cutoffMd = 0.5 * srMd * (double) kAntiAliasCutoffRatio;
    const double cutoffLo = 0.5 * srLo * (double) kAntiAliasCutoffRatio;

    {
        const auto coeffs = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (
                                (float) cutoffMd, (float) srHi, kAntiAliasOrder);
        const int n = juce::jmin ((int) aaFilterMd.size(), coeffs.size());
        for (int i = 0; i < n; ++i)
        {
            aaFilterMd[(size_t) i].coefficients = coeffs[i];
            aaFilterMd[(size_t) i].reset();
        }
        // 未使用的级也 reset，以免负载历史状态
        for (int i = n; i < (int) aaFilterMd.size(); ++i) aaFilterMd[(size_t) i].reset();
    }
    {
        // 低频路的 LP 接在中频输出上运作（采样率 = srMd），截止 = cutoffLo
        const auto coeffs = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (
                                (float) cutoffLo, (float) srMd, kAntiAliasOrder);
        const int n = juce::jmin ((int) aaFilterLo.size(), coeffs.size());
        for (int i = 0; i < n; ++i)
        {
            aaFilterLo[(size_t) i].coefficients = coeffs[i];
            aaFilterLo[(size_t) i].reset();
        }
        for (int i = n; i < (int) aaFilterLo.size(); ++i) aaFilterLo[(size_t) i].reset();
    }

    // 示波器缓冲清零
    {
        const juce::SpinLock::ScopedLockType sl(oscLock);
        oscBufL.fill(0.0f);
        oscBufR.fill(0.0f);
        oscWritePos = 0;
    }

    // 频谱工作缓冲清零（prepare 与音频 push 串行调用）
    fftFifoHi.fill(0.0f);  fftDataHi.fill(0.0f);  fftFifoIndexHi = 0;  hopAccumHi = 0;
    fftFifoMd.fill(0.0f);  fftDataMd.fill(0.0f);  fftFifoIndexMd = 0;  decimCounterMd = 0;
    fftFifoLo.fill(0.0f);  fftDataLo.fill(0.0f);  fftFifoIndexLo = 0;  decimCounterLo = 0;

    magDataHiWork.fill(0.0f);
    magDataMdWork.fill(0.0f);
    magDataLoWork.fill(0.0f);
    specDataWork.fill(0.0f);

    // 发布给 UI 的频谱快照清零
    {
        const juce::SpinLock::ScopedLockType sl(specLock);
        specData.fill(0.0f);
        magDataHi.fill(0.0f);
        magDataMd.fill(0.0f);
        magDataLo.fill(0.0f);
        noteMagPublished.fill(0.0f);
    }

    // 响度计初始化
    loudnessMeter.prepare(sampleRate, samplesPerBlock);

    // 相位相关仪初始化
    phaseCorrelator.prepare(sampleRate);

    // 动态范围计初始化
    dynamicsMeter.prepare(sampleRate);
}

// ==========================================================
// pushStereo —— 音频线程
// ==========================================================
void AnalyserHub::pushStereo(const float* left, const float* right, int numSamples)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::audio, "Audio-AnalyserHub");
    y2k::perf::ScopedPerfTimer pushTimer(y2k::perf::FunctionId::analyserPushTotal,
                                         y2k::perf::Partition::audioAnalysis,
                                         y2k::perf::ThreadRole::audio);

    const auto t0 = juce::Time::getHighResolutionTicks();
#endif

    // Phase F 按需计算：只跑被订阅（refCount > 0）的那几路。
    //   · memory_order_relaxed 已足够：这里只要求原子读写不撕裂，不需要跨线程同步其他数据。
    //   · 若某个 Kind 被短暂 retain 之后又 release，本帧可能仍旧跑或跑半路就停，
    //     都不会造成数据一致性问题，因为每一路都是"能算就算"，不依赖上一帧活跃状态。
    const bool wantOsc      = refCounts[(size_t) Kind::Oscilloscope].load (std::memory_order_relaxed) > 0;
    const bool wantSpec     = refCounts[(size_t) Kind::Spectrum    ].load (std::memory_order_relaxed) > 0;
    const bool wantLoud     = refCounts[(size_t) Kind::Loudness    ].load (std::memory_order_relaxed) > 0;
    const bool wantPhase    = refCounts[(size_t) Kind::Phase       ].load (std::memory_order_relaxed) > 0;
    const bool wantDynamics = refCounts[(size_t) Kind::Dynamics    ].load (std::memory_order_relaxed) > 0;

    if (wantOsc)      pushSamplesToOscilloscope(left, right, numSamples);
    if (wantSpec)     pushSamplesToSpectrum    (left, right, numSamples);

    if (wantLoud)
    {
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::ScopedPerfTimer t(y2k::perf::FunctionId::analyserLoudness,
                                     y2k::perf::Partition::audioAnalysis,
                                     y2k::perf::ThreadRole::audio);
#endif
        loudnessMeter.pushStereo (left, right, numSamples);
    }

    if (wantPhase)
    {
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::ScopedPerfTimer t(y2k::perf::FunctionId::analyserPhase,
                                     y2k::perf::Partition::audioAnalysis,
                                     y2k::perf::ThreadRole::audio);
#endif
        phaseCorrelator.pushStereo(left, right, numSamples);
    }

    if (wantDynamics)
    {
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::ScopedPerfTimer t(y2k::perf::FunctionId::analyserDynamics,
                                     y2k::perf::Partition::audioAnalysis,
                                     y2k::perf::ThreadRole::audio);
#endif
        dynamicsMeter.pushStereo (left, right, numSamples);
    }

#if Y2K_ENABLE_PERF_COUNTERS
    const auto t1 = juce::Time::getHighResolutionTicks();
    const auto us = (juce::int64) (juce::Time::highResolutionTicksToSeconds(t1 - t0) * 1.0e6);
    perfPushCount.fetch_add(1, std::memory_order_relaxed);
    perfPushTotalUs.fetch_add(us, std::memory_order_relaxed);
    // CAS-like max 更新
    juce::int64 curMax = perfPushMaxUs.load(std::memory_order_relaxed);
    while (us > curMax
           && ! perfPushMaxUs.compare_exchange_weak(curMax, us,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed))
        ; // spin
#endif
}

// ==========================================================
// Phase F 按需计算 —— 引用计数 API
// ==========================================================
void AnalyserHub::retain(Kind kind) noexcept
{
    const auto idx = (size_t) kind;
    jassert (idx < (size_t) Kind::NumKinds);
    refCounts[idx].fetch_add (1, std::memory_order_relaxed);
}

void AnalyserHub::release(Kind kind) noexcept
{
    const auto idx = (size_t) kind;
    jassert (idx < (size_t) Kind::NumKinds);
    const auto prev = refCounts[idx].fetch_sub (1, std::memory_order_relaxed);
    jassert (prev >= 1); // 每次 release 前都必须有对应的 retain
    (void) prev;
}

bool AnalyserHub::isActive(Kind kind) const noexcept
{
    const auto idx = (size_t) kind;
    jassert (idx < (size_t) Kind::NumKinds);
    return refCounts[idx].load (std::memory_order_relaxed) > 0;
}

// ==========================================================
// 示波器（立体声环形缓冲）
// ==========================================================
void AnalyserHub::pushSamplesToOscilloscope(const float* left, const float* right, int numSamples)
{
#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::ScopedPerfTimer stageTimer(y2k::perf::FunctionId::analyserOscilloscope,
                                          y2k::perf::Partition::audioAnalysis,
                                          y2k::perf::ThreadRole::audio);
    const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
#endif
    const juce::SpinLock::ScopedLockType sl(oscLock);
#if Y2K_ENABLE_PERF_COUNTERS
    const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
    stageTimer.addLockWait(lockNs - waitStart);
    y2k::perf::PerformanceCounterSystem::instance().recordDuration(
        y2k::perf::FunctionId::lockOsc,
        y2k::perf::Partition::dataCommunication,
        y2k::perf::ThreadRole::audio,
        0,
        lockNs - waitStart);
#endif

    // Phase F 优化：逐样本循环 → 分段 memcpy
    //   若 numSamples 跨越环形缓冲末尾，最多只需要两次 memcpy 就能写完整块。
    int samplesRemaining = numSamples;
    int srcPos = 0;
    while (samplesRemaining > 0)
    {
        const int spaceTillEnd = oscilloscopeBufferSize - oscWritePos;
        const int chunk        = juce::jmin (samplesRemaining, spaceTillEnd);

        std::memcpy (oscBufL.data() + oscWritePos, left  + srcPos, (size_t) chunk * sizeof (float));
        std::memcpy (oscBufR.data() + oscWritePos, right + srcPos, (size_t) chunk * sizeof (float));

        oscWritePos = (oscWritePos + chunk) % oscilloscopeBufferSize;
        srcPos            += chunk;
        samplesRemaining  -= chunk;
    }
}

void AnalyserHub::getOscilloscopeSnapshot(juce::Array<float>& destL,
                                           juce::Array<float>& destR)
{
    destL.resize(oscilloscopeBufferSize);
    destR.resize(oscilloscopeBufferSize);

    const juce::SpinLock::ScopedLockType sl(oscLock);

    // Phase F 优化：逐样本循环 → 分段 memcpy
    //   环形缓冲里"从旧到新"的数据是 [oscWritePos .. end] 接 [0 .. oscWritePos-1]，
    //   最多两段 memcpy 就能拷完；destL/R 内部 raw 数组由 juce::Array 连续分配。
    const int firstChunk = oscilloscopeBufferSize - oscWritePos;
    std::memcpy (destL.getRawDataPointer(),               oscBufL.data() + oscWritePos,
                 (size_t) firstChunk * sizeof (float));
    std::memcpy (destR.getRawDataPointer(),               oscBufR.data() + oscWritePos,
                 (size_t) firstChunk * sizeof (float));
    if (oscWritePos > 0)
    {
        std::memcpy (destL.getRawDataPointer() + firstChunk, oscBufL.data(),
                     (size_t) oscWritePos * sizeof (float));
        std::memcpy (destR.getRawDataPointer() + firstChunk, oscBufR.data(),
                     (size_t) oscWritePos * sizeof (float));
    }
}

// ==========================================================
// 频谱：三路多分辨率 FFT（Hi/Md/Lo）+ 132 半音格输出
//
//   总流程：
//     1) 立体声 → mid（(L+R)/2）
//     2) 若 SR > 44.1k：4 阶 Butterworth LP → 多相位线性插值降到 44.1k 形成 highStream
//        若 SR ≤ 44.1k：highStream = mid 原样
//     3) highStream:
//        a) 环形 FIFO 送 fftFifoHi（1024，hop=kFftHopHi=512，50% overlap）
//        b) 经 aaFilterMd LP → 每 kDecimMd 个样本抽 1 个 → 形成 midStream
//        c) midStream 经 aaFilterLo LP → 每 (kDecimLo/kDecimMd) 个样本抽 1 个 → lowStream
//     4) midStream → fftFifoMd（2048，hop=kFftHopMd=256，87.5% overlap）
//     5) lowStream → fftFifoLo（4096，hop=kFftHopLo=64，≈98.4% overlap）
//     6) 任一路 FFT 完成 → 写 magXxxWork
//     7) 末尾用 magXxxWork 合成 132 半音格 → publish 到 specLock 保护的 magXxx + noteMagPublished
// ==========================================================
void AnalyserHub::pushSamplesToSpectrum(const float* left, const float* right, int numSamples)
{
#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::ScopedPerfTimer stageTimer(y2k::perf::FunctionId::analyserSpectrum,
                                          y2k::perf::Partition::audioAnalysis,
                                          y2k::perf::ThreadRole::audio);
#endif

    // 三路 hop（全部取自公开常量，便于调参）
    constexpr int hopHi = kFftHopHi;   // 512
    constexpr int hopMd = kFftHopMd;   // 256
    constexpr int hopLo = kFftHopLo;   // 64
    // 第二级抽取比：低频路相对中频路多抽几倍
    constexpr int kDecimLoOverMd = kDecimLo / kDecimMd;  // 32/4 = 8

    bool publishAny = false;

    // ---- 局部 lambda：从环形 FIFO 跑一次 Hi-FFT ----
    auto runFftHi = [&]()
    {
        // 环形 FIFO 展平到 fftDataHi（oldest → newest）
        const int firstChunk = fftSizeHi - fftFifoIndexHi;
        std::memcpy (fftDataHi.data(),              fftFifoHi.data() + fftFifoIndexHi,
                     sizeof (float) * (size_t) firstChunk);
        if (fftFifoIndexHi > 0)
            std::memcpy (fftDataHi.data() + firstChunk, fftFifoHi.data(),
                         sizeof (float) * (size_t) fftFifoIndexHi);
        std::fill (fftDataHi.begin() + fftSizeHi, fftDataHi.end(), 0.0f);

        windowHi.multiplyWithWindowingTable (fftDataHi.data(), fftSizeHi);
        fftHi.performFrequencyOnlyForwardTransform (fftDataHi.data());

        const int   maxBin = fftSizeHi / 2;
        const float invMax = 2.0f / (float) fftSizeHi;
        for (int b = 0; b < maxBin; ++b)
            magDataHiWork[(size_t) b] = fftDataHi[(size_t) b] * invMax;

        // 旧 specData（粗粒度 160 bin） —— 还在用 FFT bin 做映射，读 fftDataHi
        for (int iBin = 0; iBin < spectrumBins; ++iBin)
        {
            const float norm = (float) iBin / (float) juce::jmax (1, spectrumBins - 1);
            const float skew = std::pow (norm, 2.3f);
            const int   fBin = juce::jlimit (0, maxBin - 1,
                                  (int) std::round (skew * (float) (maxBin - 1)));
            const float mag    = fftDataHi[(size_t) fBin];
            const float db     = juce::Decibels::gainToDecibels (juce::jmax (1.0e-6f, mag));
            const float mapped = juce::jlimit (0.0f, 1.0f,
                                  juce::jmap (db, -50.0f, 50.0f, 0.0f, 1.0f));
            const float prev   = specDataWork[(size_t) iBin];
            const float alpha  = (mapped > prev) ? 0.6f : 0.15f;
            specDataWork[(size_t) iBin] = prev + alpha * (mapped - prev);
        }
    };

    // ---- 局部 lambda：从环形 FIFO 跑一次 Md-FFT ----
    auto runFftMd = [&]()
    {
        // 环形 FIFO 展平到 fftDataMd（oldest → newest）
        const int firstChunk = fftSizeMd - fftFifoIndexMd;
        std::memcpy (fftDataMd.data(),              fftFifoMd.data() + fftFifoIndexMd,
                     sizeof (float) * (size_t) firstChunk);
        if (fftFifoIndexMd > 0)
            std::memcpy (fftDataMd.data() + firstChunk, fftFifoMd.data(),
                         sizeof (float) * (size_t) fftFifoIndexMd);
        std::fill (fftDataMd.begin() + fftSizeMd, fftDataMd.end(), 0.0f);

        windowMd.multiplyWithWindowingTable (fftDataMd.data(), fftSizeMd);
        fftMd.performFrequencyOnlyForwardTransform (fftDataMd.data());

        const int   maxBin = fftSizeMd / 2;
        const float invMax = 2.0f / (float) fftSizeMd;
        for (int b = 0; b < maxBin; ++b)
            magDataMdWork[(size_t) b] = fftDataMd[(size_t) b] * invMax;
    };

    // ---- 局部 lambda：从环形 FIFO 跑一次 Lo-FFT ----
    auto runFftLo = [&]()
    {
        const int firstChunk = fftSizeLo - fftFifoIndexLo;
        std::memcpy (fftDataLo.data(),              fftFifoLo.data() + fftFifoIndexLo,
                     sizeof (float) * (size_t) firstChunk);
        if (fftFifoIndexLo > 0)
            std::memcpy (fftDataLo.data() + firstChunk, fftFifoLo.data(),
                         sizeof (float) * (size_t) fftFifoIndexLo);
        std::fill (fftDataLo.begin() + fftSizeLo, fftDataLo.end(), 0.0f);

        windowLo.multiplyWithWindowingTable (fftDataLo.data(), fftSizeLo);
        fftLo.performFrequencyOnlyForwardTransform (fftDataLo.data());

        const int   maxBin = fftSizeLo / 2;
        const float invMax = 2.0f / (float) fftSizeLo;
        for (int b = 0; b < maxBin; ++b)
            magDataLoWork[(size_t) b] = fftDataLo[(size_t) b] * invMax;
    };

    // ---- 局部 lambda：把 highStream 的一个样本注入三路 FIFO，触发可能的 FFT ----
    auto feedHighSample = [&] (float s)
    {
        // 1) Hi 路（环形 FIFO + 50% overlap）
        fftFifoHi[(size_t) fftFifoIndexHi] = s;
        fftFifoIndexHi = (fftFifoIndexHi + 1) % fftSizeHi;
        ++hopAccumHi;
        if (hopAccumHi >= hopHi)
        {
            hopAccumHi = 0;
            runFftHi();
            publishAny = true;
        }

        // 2) Md 路：先过 aaFilterMd，再每 kDecimMd 个样本抽 1 个
        float sM = s;
        for (int k = 0; k < aaActiveStages; ++k)
            sM = aaFilterMd[(size_t) k].processSample (sM);

        ++decimCounterMd;
        if (decimCounterMd >= kDecimMd)
        {
            decimCounterMd = 0;

            // sM 是 midStream 的一个样本
            fftFifoMd[(size_t) fftFifoIndexMd] = sM;
            fftFifoIndexMd = (fftFifoIndexMd + 1) % fftSizeMd;
            if ((fftFifoIndexMd % hopMd) == 0)
            {
                runFftMd();
                publishAny = true;
            }

            // 3) Lo 路：把 midStream 样本继续过 aaFilterLo，再每 kDecimLoOverMd 个抽 1 个
            float sL = sM;
            for (int k = 0; k < aaActiveStages; ++k)
                sL = aaFilterLo[(size_t) k].processSample (sL);

            ++decimCounterLo;
            if (decimCounterLo >= kDecimLoOverMd)
            {
                decimCounterLo = 0;

                fftFifoLo[(size_t) fftFifoIndexLo] = sL;
                fftFifoIndexLo = (fftFifoIndexLo + 1) % fftSizeLo;
                if ((fftFifoIndexLo % hopLo) == 0)
                {
                    runFftLo();
                    publishAny = true;
                }
            }
        }
    };

    // ====================================================================
    // 主循环：读 left/right → 形成 mid → （可选）下采到 44.1k → highStream
    // ====================================================================
    if (! inputResampleEnabled)
    {
        // SR ≤ 44.1k：原样作为 highStream
        for (int i = 0; i < numSamples; ++i)
        {
            const float mid = (left[i] + right[i]) * 0.5f;
            const float s   = juce::jlimit (-1.0f, 1.0f, mid);
            feedHighSample (s);
        }
    }
    else
    {
        // SR > 44.1k：4 阶 LP + 多相位插值降采样到 44.1k
        //   多相位策略：每个输入样本先 LP，再用线性插值在 inputDecimRatio 步长上采样输出。
        const double ratio = inputDecimRatio;  // > 1.0
        for (int i = 0; i < numSamples; ++i)
        {
            float in = (left[i] + right[i]) * 0.5f;
            in = juce::jlimit (-1.0f, 1.0f, in);

            // 4 阶 anti-image LP
            float filt = in;
            for (int k = 0; k < inputPreLpStages; ++k)
                filt = inputPreLp[(size_t) k].processSample (filt);

            // 在 [prev, filt] 之间产出若干个等距输出
            // inputDecimPhase 表示"下一个输出在当前样本区间里的位置"，初始 0
            //   每个输入样本贡献 (1.0/ratio) 步长；当 phase < 1.0 时输出，phase += ratio
            inputDecimPhase += 1.0;  // 当前区间长度 +1 个输入样本
            while (inputDecimPhase >= ratio)
            {
                inputDecimPhase -= ratio;
                // 输出位置 t ∈ [0,1]：相对于"当前样本"的偏移；以 prev=z1 / cur=filt 线性插值
                const float t = (float) (1.0 - inputDecimPhase / 1.0);  // 越大越接近 cur
                const float out = inputResampleZ1 + (filt - inputResampleZ1) * t;
                feedHighSample (out);
            }

            inputResampleZ1 = filt;
        }
    }

    // ====================================================================
    // 任一路 FFT 完成 → 重新合成 132 半音格 → 一次性 publish
    // ====================================================================
    if (publishAny)
    {
        // 当前各路输入流采样率（用于 Hz→bin 计算）
        const double srSrc = (cachedSampleRate > 0.0) ? cachedSampleRate : 44100.0;
        const double srHi  = inputResampleEnabled ? (double) kAnalysisTargetSR : srSrc;
        const double srMd  = srHi / (double) kDecimMd;
        const double srLo  = srHi / (double) kDecimLo;

        const double nyqHi = srHi * 0.5;
        const double nyqMd = srMd * 0.5;
        const double nyqLo = srLo * 0.5;

        // 取以 f 为中心的±25 cent 频带峰值；同时限制最多只取距中心最近的 3 个 bin。
        //   原因：在高频区，一个半音 ±50 cent 跨 bin 多达 10+ 个，护带过宽
        //   会把邻近半音的能量以及 FFT 算法噪声全部 "卷" 进来，造成伪亮线。
        //   ±25 cent 同时限 3 bin 能准确化高音区的选择性，不影响低音区（在低音区
        //   一个半音本来就<1 个 bin）。
        auto peakInBand = [] (const float* buf, int bufN,
                              double fCenter, double fLow, double fHigh, double nyq) noexcept
        {
            if (nyq <= 0.0 || bufN <= 0) return 0.0f;
            const double hzToBin = (double) (bufN - 1) / nyq;
            int b0 = (int) std::floor (fLow  * hzToBin);
            int b1 = (int) std::ceil  (fHigh * hzToBin);
            b0 = juce::jlimit (0, bufN - 1, b0);
            b1 = juce::jlimit (0, bufN - 1, b1);
            if (b1 < b0) std::swap (b0, b1);

            // 超过 3 个 bin 时，只保留 "距中心 fCenter 最近的 3 bin"
            constexpr int kMaxBins = 3;
            if (b1 - b0 + 1 > kMaxBins)
            {
                const int center = juce::jlimit (b0, b1,
                                       (int) std::round (fCenter * hzToBin));
                b0 = juce::jmax (b0, center - 1);
                b1 = juce::jmin (b1, center + 1);
            }

            float m = 0.0f;
            for (int b = b0; b <= b1; ++b)
            {
                const float v = std::abs (buf[b]);
                if (std::isfinite (v) && v > m) m = v;
            }
            return m;
        };

        // 半音 → Hz；±25 cent = 频率 × 2^(±1/48)
        constexpr double centBandLo = 0.9856156709705; // 2^(-1/48)
        constexpr double centBandHi = 1.0145453349375; // 2^(+1/48)

        // 构造 132 个半音格（线性幅度）
        std::array<float, kNoteBinCount> noteWork {};
        for (int n = 0; n < kNoteBinCount; ++n)
        {
            const int    midi = kNoteMidiBase + n;
            const double f    = (double) kNoteRefHz * std::pow (2.0, (midi - kNoteRefMidi) / 12.0);
            const double fLo  = f * centBandLo;
            const double fHi  = f * centBandHi;

            // 选择路：硬切 + 1 半音 crossfade
            //   纯低频路：n < kNoteSplitLoMd-1
            //   crossfade 低↔中：n ∈ [kNoteSplitLoMd-1, kNoteSplitLoMd]
            //   纯中频路：n ∈ [kNoteSplitLoMd+1, kNoteSplitMdHi-2]
            //   crossfade 中↔高：n ∈ [kNoteSplitMdHi-1, kNoteSplitMdHi]
            //   纯高频路：n > kNoteSplitMdHi
            float magVal = 0.0f;

            // 各路在该频带的幅度（仅在频率落入对应路 Nyquist 内时取）
            float magLo = (f < nyqLo) ? peakInBand (magDataLoWork.data(), spectrumMagSizeLo, f, fLo, fHi, nyqLo) : 0.0f;
            float magMd = (f < nyqMd) ? peakInBand (magDataMdWork.data(), spectrumMagSizeMd, f, fLo, fHi, nyqMd) : 0.0f;
            float magHi = (f < nyqHi) ? peakInBand (magDataHiWork.data(), spectrumMagSizeHi, f, fLo, fHi, nyqHi) : 0.0f;

            if (n <= kNoteSplitLoMd - 2)
            {
                magVal = magLo;
            }
            else if (n == kNoteSplitLoMd - 1 || n == kNoteSplitLoMd)
            {
                // 2 个半音的等功率 crossfade（B2 / C3）
                const float w = (n == kNoteSplitLoMd - 1) ? 0.25f : 0.75f; // 中频路权重
                const float eL = magLo * magLo;
                const float eM = magMd * magMd;
                magVal = std::sqrt (juce::jmax (0.0f, w * eM + (1.0f - w) * eL));
            }
            else if (n <= kNoteSplitMdHi - 2)
            {
                magVal = magMd;
            }
            else if (n == kNoteSplitMdHi - 1 || n == kNoteSplitMdHi)
            {
                // 2 个半音的等功率 crossfade（B7 / C8）
                const float w = (n == kNoteSplitMdHi - 1) ? 0.25f : 0.75f; // 高频路权重
                const float eM = magMd * magMd;
                const float eH = magHi * magHi;
                magVal = std::sqrt (juce::jmax (0.0f, w * eH + (1.0f - w) * eM));
            }
            else
            {
                magVal = magHi;
            }

            noteWork[(size_t) n] = magVal;
        }

        // ---- 一次性 publish ----
        {
#if Y2K_ENABLE_PERF_COUNTERS
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
#endif
            const juce::SpinLock::ScopedLockType sl (specLock);
#if Y2K_ENABLE_PERF_COUNTERS
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            stageTimer.addLockWait(lockNs - waitStart);
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockSpec,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::audio,
                0,
                lockNs - waitStart);
#endif
            std::memcpy (magDataHi.data(), magDataHiWork.data(), sizeof (float) * magDataHi.size());
            std::memcpy (magDataMd.data(), magDataMdWork.data(), sizeof (float) * magDataMd.size());
            std::memcpy (magDataLo.data(), magDataLoWork.data(), sizeof (float) * magDataLo.size());
            std::memcpy (specData.data(),  specDataWork.data(),  sizeof (float) * specData.size());
            std::memcpy (noteMagPublished.data(), noteWork.data(),
                         sizeof (float) * noteMagPublished.size());
        }
    }
}

void AnalyserHub::getSpectrumSnapshot(juce::Array<float>& dest)
{
    dest.resize(spectrumBins);

    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        std::memcpy (dest.getRawDataPointer(), specData.data(),
                     sizeof (float) * (size_t) spectrumBins);
    }
}

void AnalyserHub::getSpectrumMagnitudes(juce::Array<float>& dest)
{
#if Y2K_ENABLE_PERF_COUNTERS
    const auto t0 = juce::Time::getHighResolutionTicks();
#endif

    dest.resize(spectrumMagSizeHi);
    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        std::memcpy (dest.getRawDataPointer(), magDataHi.data(),
                     sizeof (float) * (size_t) spectrumMagSizeHi);
    }

#if Y2K_ENABLE_PERF_COUNTERS
    const auto t1 = juce::Time::getHighResolutionTicks();
    const auto us = (juce::int64) (juce::Time::highResolutionTicksToSeconds(t1 - t0) * 1.0e6);
    perfSnapCount.fetch_add(1,  std::memory_order_relaxed);
    perfSnapTotalUs.fetch_add(us, std::memory_order_relaxed);
#endif
}

// ==========================================================
// 中频路高精度幅度快照（1024 bin，输入流为 SR/D_M）
// ==========================================================
void AnalyserHub::getSpectrumMagnitudesMd(juce::Array<float>& dest)
{
    dest.resize(spectrumMagSizeMd);
    const juce::SpinLock::ScopedLockType sl (specLock);
    std::memcpy (dest.getRawDataPointer(), magDataMd.data(),
                 sizeof (float) * (size_t) spectrumMagSizeMd);
}

// ==========================================================
// 低频路高精度幅度快照（2048 bin，输入流为 SR/D_L）
// ==========================================================
void AnalyserHub::getSpectrumMagnitudesLo(juce::Array<float>& dest)
{
    dest.resize(spectrumMagSizeLo);

    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        std::memcpy (dest.getRawDataPointer(), magDataLo.data(),
                     sizeof (float) * (size_t) spectrumMagSizeLo);
    }
}

// ==========================================================
// 132 半音格幅度快照（C0~B10）—— UI 推荐入口
// ==========================================================
void AnalyserHub::getSpectrumMagnitudesByNote (juce::Array<float>& dest)
{
    dest.resize (kNoteBinCount);
    const juce::SpinLock::ScopedLockType sl (specLock);
    std::memcpy (dest.getRawDataPointer(), noteMagPublished.data(),
                 sizeof (float) * (size_t) kNoteBinCount);
}

// ==========================================================
// 合并（对数频率轴）幅度快照 —— 兼容旧 API
//
//   现在内部数据源已变更：
//     主路 magData → magDataHi（512 bin）；
//     低频路 magDataLo（2048 bin）。
//   输入流如果做了重采样，nyquist 取 44.1k/2。
// ==========================================================
void AnalyserHub::getSpectrumMagnitudesBlended (juce::Array<float>& dest,
                                                int   numPoints,
                                                float fMin,
                                                float fMax)
{
    numPoints = juce::jmax (2, numPoints);
    dest.resize (numPoints);

    const double srSrc   = (cachedSampleRate > 0.0) ? cachedSampleRate : 48000.0;
    const double srHi    = inputResampleEnabled ? (double) kAnalysisTargetSR : srSrc;
    const double nyquist = srHi * 0.5;
    const double nyqLo   = (srHi / (double) kDecimLo) * 0.5;

    const double fLo2 = juce::jmax (1.0, (double) fMin);
    const double fHi2 = juce::jmin (nyquist, (double) fMax);
    if (fHi2 <= fLo2)
    {
        auto* outData = dest.getRawDataPointer();
        std::fill (outData, outData + numPoints, 0.0f);
        return;
    }

    thread_local std::array<float, spectrumMagSizeHi> magHiSnapshot;
    thread_local std::array<float, spectrumMagSizeLo> magLoSnapshot;

    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        std::memcpy (magHiSnapshot.data(), magDataHi.data(),
                     sizeof (float) * magHiSnapshot.size());
        std::memcpy (magLoSnapshot.data(), magDataLo.data(),
                     sizeof (float) * magLoSnapshot.size());
    }

    const double logMin = std::log10 (fLo2);
    const double logMax = std::log10 (fHi2);

    const double hzToBinHi = (double) (spectrumMagSizeHi - 1) / nyquist;
    const double hzToBinLo = (double) (spectrumMagSizeLo - 1) / nyqLo;

    const double xover     = (double) spectrumXoverHz;
    const double xoverLo   = xover / std::sqrt (2.0);
    const double xoverHi   = xover * std::sqrt (2.0);
    const double loHardMax = nyqLo * 0.95; // 低频路有效上限
    auto* outData = dest.getRawDataPointer();

    auto peakInRange = [] (const float* buf, int bufN, int binLo, int binHi) noexcept
    {
        binLo = juce::jlimit (0, bufN - 1, binLo);
        binHi = juce::jlimit (0, bufN - 1, binHi);
        if (binHi < binLo) std::swap (binLo, binHi);
        float m = 0.0f;
        for (int b = binLo; b <= binHi; ++b)
        {
            const float v = std::abs (buf[b]);
            if (std::isfinite (v) && v > m) m = v;
        }
        return m;
    };

    for (int i = 0; i < numPoints; ++i)
    {
        const double t   = (double) i / (double) (numPoints - 1);
        const double f   = std::pow (10.0, logMin + t * (logMax - logMin));

        const double tPrev = (double) (i - 1) / (double) (numPoints - 1);
        const double tNext = (double) (i + 1) / (double) (numPoints - 1);
        const double fPrev = (i == 0)             ? f : std::pow (10.0, logMin + tPrev * (logMax - logMin));
        const double fNext = (i == numPoints - 1) ? f : std::pow (10.0, logMin + tNext * (logMax - logMin));
        const double f0    = std::sqrt (fPrev * f);
        const double f1    = std::sqrt (f * fNext);

        const int binHiLo  = (int) std::floor (f0 * hzToBinHi);
        const int binHiUp  = (int) std::ceil  (f1 * hzToBinHi);
        const float magHi  = peakInRange (magHiSnapshot.data(), spectrumMagSizeHi, binHiLo, binHiUp);

        float magLo = 0.0f;
        if (f < loHardMax)
        {
            const int binLoLo = (int) std::floor (f0 * hzToBinLo);
            const int binLoUp = (int) std::ceil  (f1 * hzToBinLo);
            magLo = peakInRange (magLoSnapshot.data(), spectrumMagSizeLo, binLoLo, binLoUp);
        }

        float w;
        if      (f <= xoverLo) w = 0.0f;
        else if (f >= xoverHi) w = 1.0f;
        else
        {
            const double u = (std::log (f) - std::log (xoverLo))
                           / (std::log (xoverHi) - std::log (xoverLo));
            w = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * u));
        }

        const float eHi = magHi * magHi;
        const float eLo = magLo * magLo;
        const float eMix = w * eHi + (1.0f - w) * eLo;
        const float out  = std::sqrt (juce::jmax (0.0f, eMix));

        outData[i] = out;
    }
}

// ==========================================================
// 响度快照
// ==========================================================
LoudnessMeter::Snapshot AnalyserHub::getLoudnessSnapshot()
{
    return loudnessMeter.getSnapshot();
}

void AnalyserHub::resetLoudness() noexcept
{
    loudnessMeter.reset();
}

// ==========================================================
// 相位相关快照
// ==========================================================
PhaseCorrelator::Snapshot AnalyserHub::getPhaseSnapshot()
{
    return phaseCorrelator.getSnapshot();
}

// ==========================================================
// 动态范围快照
// ==========================================================
DynamicRangeMeter::Snapshot AnalyserHub::getDynamicsSnapshot()
{
    return dynamicsMeter.getSnapshot();
}

// ==========================================================
// Phase E —— 性能计数
// ==========================================================
AnalyserHub::PerfCounters AnalyserHub::getPerfCounters() const noexcept
{
    PerfCounters out;
    const auto pc = perfPushCount.load (std::memory_order_relaxed);
    const auto pt = perfPushTotalUs.load(std::memory_order_relaxed);
    const auto pm = perfPushMaxUs.load (std::memory_order_relaxed);
    const auto sc = perfSnapCount.load (std::memory_order_relaxed);
    const auto st = perfSnapTotalUs.load(std::memory_order_relaxed);

    out.pushCount = pc;
    out.snapCount = sc;
    out.avgPushUs = (pc > 0) ? (double) pt / (double) pc : 0.0;
    out.avgSnapUs = (sc > 0) ? (double) st / (double) sc : 0.0;
    out.maxPushUs = (double) pm;
    return out;
}

void AnalyserHub::resetPerfCounters() noexcept
{
    perfPushCount  .store(0, std::memory_order_relaxed);
    perfPushTotalUs.store(0, std::memory_order_relaxed);
    perfPushMaxUs  .store(0, std::memory_order_relaxed);
    perfSnapCount  .store(0, std::memory_order_relaxed);
    perfSnapTotalUs.store(0, std::memory_order_relaxed);
}

// ==========================================================
// Phase F —— FrameDispatcher 对外 API
// ==========================================================
void AnalyserHub::addFrameListener (FrameListener* listener)
{
    if (listener == nullptr) return;
    const juce::ScopedLock sl (frameListenersLock);
    // 去重
    for (auto* l : frameListeners)
        if (l == listener) return;
    frameListeners.push_back (listener);
    frameListenersReserved = juce::jmax (frameListenersReserved, (int) frameListeners.size());
}

void AnalyserHub::removeFrameListener (FrameListener* listener)
{
    if (listener == nullptr) return;
    const juce::ScopedLock sl (frameListenersLock);
    for (auto it = frameListeners.begin(); it != frameListeners.end(); ++it)
    {
        if (*it == listener)
        {
            frameListeners.erase (it);
            // 容量下降到一半以下再缩水位，避免抖动；reserve 仍归 vector 管。
            if ((int) frameListeners.size() < frameListenersReserved / 2)
                frameListenersReserved = (int) frameListeners.size();
            return;
        }
    }
}

std::shared_ptr<const AnalyserHub::FrameSnapshot> AnalyserHub::getLatestFrame() const noexcept
{
    const juce::SpinLock::ScopedLockType sl (latestFrameLock);
#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().recordEvent(
        y2k::perf::FunctionId::dataGetLatestFrame,
        y2k::perf::Partition::dataCommunication,
        y2k::perf::ThreadRole::unknown,
        1);
#endif
    return latestFrame; // 返回共享副本，调用方可自由持有
}

void AnalyserHub::startFrameDispatcher (int hz)
{
    // 必须在 UI 线程（juce::Timer 要求）
    if (frameDispatcher == nullptr)
        return;

    // 限定合理范围，相同频率直接 return 避免重复 startTimerHz 抖动。
    const int safeHz = juce::jlimit (8, 120, hz);
    const int curHz  = currentFrameDispatcherHz.load (std::memory_order_relaxed);
    if (curHz == safeHz)
        return;

    frameDispatcher->startTimerHz (safeHz);
    currentFrameDispatcherHz.store (safeHz, std::memory_order_relaxed);
}

void AnalyserHub::stopFrameDispatcher()
{
    if (frameDispatcher != nullptr)
        frameDispatcher->stopTimer();
    currentFrameDispatcherHz.store (0, std::memory_order_relaxed);
}
