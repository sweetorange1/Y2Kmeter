#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <algorithm>
#include "source/perf/PerformanceCounterSystem.h"
#include "source/perf/FramePerfLog.h"

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

    // 内部 Timer 回退路径（无 vblank 时）
    void timerCallback() override
    {
        dispatch (juce::Time::getMillisecondCounterHiRes() * 0.001);
    }

    // v2.9：一帧完整分发（Timer 或外部 vblank 时钟共用）
    void dispatch (double nowSec)
    {
        Y2K_PERF_FRAME_SCOPE ("dispatch");

        // 防重入：fanout 用成员 scratch vector，嵌套 dispatch 会破坏正在迭代的列表
        if (dispatching)
            return;
        const juce::ScopedValueSetter<bool> dispatchingScope (dispatching, true);

#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::ui, "UI-FrameDispatcher");
        y2k::perf::ScopedPerfTimer frameTimer(y2k::perf::FunctionId::uiFrameDispatcher,
                                              y2k::perf::Partition::uiRendering,
                                              y2k::perf::ThreadRole::ui);
#endif

        // 0) 帧间隔：时钟源切换（Timer ↔ vblank）或长时间停顿时视为不连续，
        //    回落到名义周期，避免模块拿到异常 dt 做一次巨大的衰减/滚动。
        const int    hzNow     = juce::jmax (1, owner.currentFrameDispatcherHz.load (std::memory_order_relaxed));
        const double nominalDt = 1.0 / (double) hzNow;
        double dt = (lastDispatchSec > 0.0) ? (nowSec - lastDispatchSec) : nominalDt;
        if (! (dt > 0.0) || dt > 0.25)
            dt = nominalDt;
        lastDispatchSec = nowSec;

        // 1) 组装活跃 mask
        juce::uint32 mask = 0;
        for (int i = 0; i < static_cast<int> (Kind::NumKinds); ++i)
            if (owner.refCounts[static_cast<size_t> (i)].load (std::memory_order_relaxed) > 0)
                mask |= static_cast<juce::uint32> (1u << i);

        if (mask == 0)
            return; // 所有路都无人订阅 —— 不生成帧

        // v2.9：快照池复用。FrameSnapshot ≈ 37KB，120Hz 下每 tick make_shared
        //   等于每秒 4.5MB 的 malloc + 清零；池里找一个"只被池持有"的旧快照复用。
        //   未激活 Kind 的字段可能残留旧值，但消费方一律先 has(kind) 判定，不受影响。
        auto frame = acquireSnapshot();
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordMemoryAlloc(sizeof(FrameSnapshot));
#endif
        frame->activeMask = mask;
        frame->tickCount  = ++dispatchTickCounter; // UI 侧模块自行分频
        frame->timeSec    = nowSec;
        frame->dtSec      = (float) juce::jlimit (0.001, 0.1, dt);

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
                         static_cast<size_t> (firstChunk) * sizeof (float));
            std::memcpy (frame->oscR.data(),              owner.oscBufR.data() + owner.oscWritePos,
                         static_cast<size_t> (firstChunk) * sizeof (float));
            if (owner.oscWritePos > 0)
            {
                std::memcpy (frame->oscL.data() + firstChunk, owner.oscBufL.data(),
                             static_cast<size_t> (owner.oscWritePos) * sizeof (float));
                std::memcpy (frame->oscR.data() + firstChunk, owner.oscBufR.data(),
                             static_cast<size_t> (owner.oscWritePos) * sizeof (float));
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
            // 反馈 #7：magData 尺寸随主路 FFT size 变化（1024..8192 bin）。
            //   spectrumMag 为固定 1024 的兼容数组：取前 min(1024, size) bin，
            //   不足部分清零（快照池复用，避免残留旧值）。
            {
                const size_t n = juce::jmin ((size_t) frame->spectrumMag.size(), owner.magData.size());
                if (n > 0)
                    std::memcpy (frame->spectrumMag.data(), owner.magData.data(),
                                 sizeof (float) * n);
                if (n < frame->spectrumMag.size())
                    std::fill (frame->spectrumMag.begin() + (std::ptrdiff_t) n,
                               frame->spectrumMag.end(), 0.0f);
            }
            std::memcpy (frame->spectrumData.data(), owner.specData.data(),
                         sizeof (float) * frame->spectrumData.size());
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
        //    拷贝一份 vector 避免在回调里触发 add/remove 导致迭代失效。
        //    v2.9：两个 vector 改为成员复用（clear 不释放容量），fanout 零分配。
        auto& localCopy       = listenerScratch;
        auto& activeListeners = activeScratch;
        localCopy.clear();
        activeListeners.clear();
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
            localCopy.assign (owner.frameListeners.begin(), owner.frameListeners.end());
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

        // 用 reserved 水位避免反复扩容；push后不会 shrink 回去。
        activeListeners.reserve (static_cast<size_t> (juce::jmax (owner.frameListenersReserved,
                                                                   static_cast<int> (localCopy.size()))));
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
        y2k::perf::PerformanceCounterSystem::instance().recordFrameListenerCount(static_cast<int> (activeListeners.size()));
        y2k::perf::PerformanceCounterSystem::instance().recordEvent(
            y2k::perf::FunctionId::uiFrameListenerFanout,
            y2k::perf::Partition::uiRendering,
            y2k::perf::ThreadRole::ui,
            static_cast<juce::int64> (activeListeners.size()));
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
    // 快照池：只在 UI 线程访问。use_count()==1 表示只有池本身持有
    //   （latestFrame 持有的那份 use_count>=2，绝不会被复用改写）。
    //   getLatestFrame() 的所有调用方都在 UI 线程，因此这里的判定没有跨线程竞争。
    std::shared_ptr<FrameSnapshot> acquireSnapshot()
    {
        for (auto& s : snapshotPool)
            if (s != nullptr && s.use_count() == 1)
                return s;

        for (auto& s : snapshotPool)
        {
            if (s == nullptr)
            {
                s = std::make_shared<FrameSnapshot>();
                return s;
            }
        }

        // 池满且都被外部持有：轮换替换一个槽（外部持有者仍保有旧对象，安全）
        auto& slot = snapshotPool[evictCursor++ % snapshotPool.size()];
        slot = std::make_shared<FrameSnapshot>();
        return slot;
    }

    AnalyserHub& owner;

    // 单调递增的 tick 序号，每次 timerCallback 自增 1 并写入 FrameSnapshot，
    //   供 UI 侧各模块自行分频（如 Spectrum/Spectrogram 每 2 帧实际 repaint 一次）。
    juce::uint64 dispatchTickCounter = 0;

    double lastDispatchSec = 0.0;
    bool   dispatching     = false;

    std::array<std::shared_ptr<FrameSnapshot>, 4> snapshotPool {};
    size_t evictCursor = 0;

    std::vector<FrameListener*> listenerScratch;
    std::vector<FrameListener*> activeScratch;
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

    // 主路 FFT 初始化（默认 2048，反馈 #7 起运行时可调）
    rebuildMainFft (mainFftOrder);
    const size_t magN = (size_t) (mainFftSize / 2);
    magData.assign (magN, 0.0f);
    specRe.assign (magN, 0.0f);
    specIm.assign (magN, 0.0f);
    prevSpecRe.assign (magN, 0.0f);
    prevSpecIm.assign (magN, 0.0f);
}

// 需要显式定义（std::unique_ptr<不完整类型 FrameDispatcher> 在头里无法自动析构）
AnalyserHub::~AnalyserHub()
{
    if (frameDispatcher != nullptr)
        frameDispatcher->stopTimer();
}

void AnalyserHub::prepare(double sampleRate, int samplesPerBlock)
{
    cachedSampleRate.store (sampleRate, std::memory_order_relaxed);

    // 示波器缓冲清零
    {
        const juce::SpinLock::ScopedLockType sl(oscLock);
        oscBufL.fill(0.0f);
        oscBufR.fill(0.0f);
        oscWritePos = 0;
    }

    // 频谱工作缓冲清零（prepare 与音频 push 串行调用）
    std::fill (fftFifo.begin(), fftFifo.end(), 0.0f);
    std::fill (fftData.begin(), fftData.end(), 0.0f);
    specDataWork.fill(0.0f);
    std::fill (magDataWork.begin(), magDataWork.end(), 0.0f);
    std::fill (specReWork.begin(), specReWork.end(), 0.0f);
    std::fill (specImWork.begin(), specImWork.end(), 0.0f);
    std::fill (specReWorkPrev.begin(), specReWorkPrev.end(), 0.0f);
    std::fill (specImWorkPrev.begin(), specImWorkPrev.end(), 0.0f);
    fftFifoIndex = 0;

    // 发布给 UI 的频谱快照清零
    {
        const juce::SpinLock::ScopedLockType sl(specLock);
        specData.fill(0.0f);
        std::fill (magData.begin(), magData.end(), 0.0f);
        std::fill (specRe.begin(), specRe.end(), 0.0f);
        std::fill (specIm.begin(), specIm.end(), 0.0f);
        std::fill (prevSpecRe.begin(), prevSpecRe.end(), 0.0f);
        std::fill (prevSpecIm.begin(), prevSpecIm.end(), 0.0f);
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
    const bool wantOsc      = refCounts[static_cast<size_t> (Kind::Oscilloscope)].load (std::memory_order_relaxed) > 0;
    const bool wantSpec     = refCounts[static_cast<size_t> (Kind::Spectrum    )].load (std::memory_order_relaxed) > 0;
    const bool wantLoud     = refCounts[static_cast<size_t> (Kind::Loudness    )].load (std::memory_order_relaxed) > 0;
    const bool wantPhase    = refCounts[static_cast<size_t> (Kind::Phase       )].load (std::memory_order_relaxed) > 0;
    const bool wantDynamics = refCounts[static_cast<size_t> (Kind::Dynamics    )].load (std::memory_order_relaxed) > 0;

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
    const auto us = static_cast<juce::int64> (juce::Time::highResolutionTicksToSeconds(t1 - t0) * 1.0e6);
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
    const auto idx = static_cast<size_t> (kind);
    jassert (idx < static_cast<size_t> (Kind::NumKinds));
    refCounts[idx].fetch_add (1, std::memory_order_relaxed);
}

void AnalyserHub::release(Kind kind) noexcept
{
    const auto idx = static_cast<size_t> (kind);
    jassert (idx < static_cast<size_t> (Kind::NumKinds));
    const auto prev = refCounts[idx].fetch_sub (1, std::memory_order_relaxed);
    jassert (prev >= 1); // 每次 release 前都必须有对应的 retain
    (void) prev;
}

bool AnalyserHub::isActive(Kind kind) const noexcept
{
    const auto idx = static_cast<size_t> (kind);
    jassert (idx < static_cast<size_t> (Kind::NumKinds));
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

        std::memcpy (oscBufL.data() + oscWritePos, left  + srcPos, static_cast<size_t> (chunk) * sizeof (float));
        std::memcpy (oscBufR.data() + oscWritePos, right + srcPos, static_cast<size_t> (chunk) * sizeof (float));

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
                 static_cast<size_t> (firstChunk) * sizeof (float));
    std::memcpy (destR.getRawDataPointer(),               oscBufR.data() + oscWritePos,
                 static_cast<size_t> (firstChunk) * sizeof (float));
    if (oscWritePos > 0)
    {
        std::memcpy (destL.getRawDataPointer() + firstChunk, oscBufL.data(),
                     static_cast<size_t> (oscWritePos) * sizeof (float));
        std::memcpy (destR.getRawDataPointer() + firstChunk, oscBufR.data(),
                     static_cast<size_t> (oscWritePos) * sizeof (float));
    }
}

// ==========================================================
// 频谱（L+R 混合，FFT）
// ==========================================================
// ---- 主路 FFT size 动态调整（反馈 #7）----
void AnalyserHub::setMainFftSize (int size) noexcept
{
    // 仅接受 [1024, 16384] 内的 2 的幂
    int s = 1, order = 0;
    while (s < size && order < 16) { s <<= 1; ++order; }
    if (s != size || s < 1024 || s > 16384)
        return;

    mainFftSizeRequest.store (s, std::memory_order_relaxed);
}

void AnalyserHub::rebuildMainFft (int order)
{
    fft    = std::make_unique<juce::dsp::FFT> (order);
    window = std::make_unique<juce::dsp::WindowingFunction<float>> (
        (size_t) (1 << order), juce::dsp::WindowingFunction<float>::hann);

    mainHop = (1 << order) / 4;   // 75% overlap：hop = size/4

    const size_t magN = (size_t) (1 << order) / 2;
    fftFifo.assign ((size_t) (1 << order), 0.0f);
    fftData.assign ((size_t) (1 << order) * 2, 0.0f);
    magDataWork.assign (magN, 0.0f);
    specReWork.assign (magN, 0.0f);
    specImWork.assign (magN, 0.0f);
    specReWorkPrev.assign (magN, 0.0f);
    specImWorkPrev.assign (magN, 0.0f);
    fftFifoIndex = 0;
}

void AnalyserHub::applyMainFftSizeChange()
{
    const int want = mainFftSizeRequest.load (std::memory_order_relaxed);
    if (want == mainFftSize)
        return;

    int s = 1, order = 0;
    while (s < want && order < 16) { s <<= 1; ++order; }
    if (s != want || s < 1024 || s > 16384)
    {
        // 非法请求（防御）：回退为当前尺寸，避免音频线程反复重试
        mainFftSizeRequest.store (mainFftSize, std::memory_order_relaxed);
        return;
    }

    mainFftSize  = s;
    mainFftOrder = order;
    rebuildMainFft (order);

    // 发布侧缓冲尺寸同步（UI 线程读 magData/specRe/specIm 均在 specLock 内）
    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        const size_t magN = (size_t) (mainFftSize / 2);
        magData.assign (magN, 0.0f);
        specRe.assign (magN, 0.0f);
        specIm.assign (magN, 0.0f);
        prevSpecRe.assign (magN, 0.0f);
        prevSpecIm.assign (magN, 0.0f);
    }
}

void AnalyserHub::pushSamplesToSpectrum(const float* left, const float* right, int numSamples)
{
#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::ScopedPerfTimer stageTimer(y2k::perf::FunctionId::analyserSpectrum,
                                          y2k::perf::Partition::audioAnalysis,
                                          y2k::perf::ThreadRole::audio);
#endif
    // 反馈 #7：UI 请求的新 FFT size 在本轮开始时生效
    applyMainFftSizeChange();
    // 频谱 DSP 状态只由音频线程写；specLock 只包住发布给 UI 的快照拷贝。
    // 这样 UI 读快照不会等 FFT，音频线程也不会因 UI 的频谱合成计算而自旋。

    // v2.9.6 单路化：唯一一路 FFT，75% overlap（hop = size/4）。
    //   · 大尺寸档位下仍保持较快刷新，且高低频刷新节奏天然一致；
    //   · hop=size/4 保证重分配谱图无折叠范围恒为 ±2 bin。
    const int hop = mainHop;

    auto publishUnderSpecLock = [&] (auto&& publish)
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
        publish();
    };

    bool publish = false;

    for (int i = 0; i < numSamples; ++i)
    {
        // 混合为 mid（L+R）/2
        const float mid = (left[i] + right[i]) * 0.5f;
        const float s   = juce::jlimit(-1.0f, 1.0f, mid);

        // ---------- 单路 FFT：环形缓冲 + 75% overlap ----------
        //   存入当前写位置，再把"写位置"前进。当写位置每次跨过 hop 的整数倍时，
        //   就把"从 writePos 往回看的 mainFftSize 个样本"打包跑一次 FFT。
        //   数据从最老到最新的排列为：fftFifo[writePos..end] + fftFifo[0..writePos-1]
        fftFifo[static_cast<size_t> (fftFifoIndex)] = s;
        fftFifoIndex = (fftFifoIndex + 1) % mainFftSize;

        // 每 hop 个样本触发一次
        //   注意：initial 填充期（累计样本 < mainFftSize）也会跑，但 FIFO 的零值部分
        //   等价于补零，幅度只会偏小不会炸。实际运行几秒后就 warm-up 完毕。
        if ((fftFifoIndex % hop) != 0)
            continue;

        // 将环形 FIFO 展平到 fftData（时序：oldest → newest）
        const int firstChunk = mainFftSize - fftFifoIndex;
        std::memcpy (fftData.data(),              fftFifo.data() + fftFifoIndex,
                     static_cast<size_t> (firstChunk) * sizeof (float));
        if (fftFifoIndex > 0)
            std::memcpy (fftData.data() + firstChunk, fftFifo.data(),
                         static_cast<size_t> (fftFifoIndex) * sizeof (float));
        // fftData 后半段（mainFftSize .. mainFftSize*2）用作 FFT 内部缓冲，填 0
        std::fill (fftData.begin() + mainFftSize, fftData.end(), 0.0f);

        window->multiplyWithWindowingTable(fftData.data(), mainFftSize);
        fft->performRealOnlyForwardTransform(fftData.data(), true);

        const int maxBin = mainFftSize / 2;
        // RealOnly(onlyNonNegative=true) 输出 maxBin+1 个复数、re/im 交错：
        //   fftData[2k]=re[k]、fftData[2k+1]=im[k]。
        //   归一化 2/fftSize（与原 FrequencyOnly 一致），后续 UI 自己转 dB。
        const float invMax = 2.0f / static_cast<float> (mainFftSize);

        // 1) 高精度幅度 + 复频谱帧对（一次 FFT 产出，幅度供 Spectrum/瀑布图，
        //    复频谱供重分配谱图；帧对滚动保证发布出去的 prev/current 恒相邻）
        {
            std::memcpy (specReWorkPrev.data(), specReWork.data(),
                         sizeof (float) * specReWork.size());
            std::memcpy (specImWorkPrev.data(), specImWork.data(),
                         sizeof (float) * specImWork.size());
            for (int iBin = 0; iBin < maxBin; ++iBin)
            {
                const float re = fftData[static_cast<size_t> (2 * iBin)];
                const float im = fftData[static_cast<size_t> (2 * iBin + 1)];
                magDataWork[static_cast<size_t> (iBin)] = std::sqrt (re * re + im * im) * invMax;
                specReWork[static_cast<size_t> (iBin)] = re * invMax;
                specImWork[static_cast<size_t> (iBin)] = im * invMax;
            }
        }

        // 2) 像素 EQ 用的粗粒度平滑频谱
        for (int iBin = 0; iBin < spectrumBins; ++iBin)
        {
            const float norm  = static_cast<float> (iBin) / static_cast<float> (juce::jmax(1, spectrumBins - 1));
            const float skew  = std::pow(norm, 2.3f);
            const int   fBin  = juce::jlimit(0, maxBin - 1,
                                    static_cast<int> (std::round(skew * static_cast<float> (maxBin - 1))));

            // fftData 现为 re/im 交错（RealOnly 输出），须用 re/im 算幅度，
            // 否则会读到错位的 re/im 值（FFT 改造后的回归点）。
            const float re    = fftData[static_cast<size_t> (2 * fBin)];
            const float im    = fftData[static_cast<size_t> (2 * fBin + 1)];
            // 与高精度 magData 保持一致：乘 2/fftSize 归一化（满幅正弦 → -6dB）。
            const float mag   = std::sqrt (re * re + im * im) * invMax;
            float db = juce::Decibels::gainToDecibels(juce::jmax(1.0e-6f, mag));

            // Slope 补偿：+4.5dB/oct（基准 1kHz），与 SpectrumModule 的 slope 一致。
            //   音乐/音频高频能量天然远低于低频，不做抬升时高频几乎贴底看不到。
            const double sr   = cachedSampleRate.load (std::memory_order_relaxed);
            const double nyq  = (sr > 0.0) ? sr * 0.5 : 24000.0;
            const double freq = static_cast<double> (fBin) * nyq / static_cast<double> (maxBin - 1);
            db += 4.5f * static_cast<float> (std::log2 (juce::jmax (20.0, freq) / 1000.0));

            // 满幅到顶：dB 映射 [-80, 0]（对齐 SpectrumModule 的 minDb/maxDb）。
            const float mapped = juce::jlimit(0.0f, 1.0f,
                                    juce::jmap(db, -80.0f, 0.0f, 0.0f, 1.0f));

            const float prev  = specDataWork[static_cast<size_t> (iBin)];
            const float alpha = (mapped > prev) ? 0.6f : 0.15f;
            specDataWork[static_cast<size_t> (iBin)] = prev + alpha * (mapped - prev);
        }

        publish = true;
    }

    if (publish)
    {
        publishUnderSpecLock ([&]
        {
            std::memcpy (magData.data(), magDataWork.data(),
                         sizeof (float) * magData.size());
            std::memcpy (specData.data(), specDataWork.data(),
                         sizeof (float) * specData.size());
            // 复频谱发布：prev ← workPrev，cur ← workCur（相邻帧对）
            std::memcpy (prevSpecRe.data(), specReWorkPrev.data(), sizeof (float) * prevSpecRe.size());
            std::memcpy (prevSpecIm.data(), specImWorkPrev.data(), sizeof (float) * prevSpecIm.size());
            std::memcpy (specRe.data(), specReWork.data(), sizeof (float) * specRe.size());
            std::memcpy (specIm.data(), specImWork.data(), sizeof (float) * specIm.size());
        });
    }
}

void AnalyserHub::getSpectrumSnapshot(juce::Array<float>& dest)
{
    dest.resize(spectrumBins);

    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        std::memcpy (dest.getRawDataPointer(), specData.data(),
                     sizeof (float) * static_cast<size_t> (spectrumBins));
    }
}

void AnalyserHub::getSpectrumMagnitudes(juce::Array<float>& dest)
{
#if Y2K_ENABLE_PERF_COUNTERS
    const auto t0 = juce::Time::getHighResolutionTicks();
#endif

    dest.resize(spectrumMagSize);
    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        // 反馈 #7：magData 尺寸动态变化，兼容层取前 1024 bin，不足清零
        const size_t n = juce::jmin ((size_t) spectrumMagSize, magData.size());
        if (n > 0)
            std::memcpy (dest.getRawDataPointer(), magData.data(),
                         sizeof (float) * n);
        if (n < (size_t) spectrumMagSize)
            std::fill (dest.getRawDataPointer() + (std::ptrdiff_t) n,
                       dest.getRawDataPointer() + spectrumMagSize, 0.0f);
    }

#if Y2K_ENABLE_PERF_COUNTERS
    const auto t1 = juce::Time::getHighResolutionTicks();
    const auto us = static_cast<juce::int64> (juce::Time::highResolutionTicksToSeconds(t1 - t0) * 1.0e6);
    perfSnapCount.fetch_add(1,  std::memory_order_relaxed);
    perfSnapTotalUs.fetch_add(us, std::memory_order_relaxed);
#endif
}

// ==========================================================
// ==========================================================
// 对数频率轴幅度快照（v2.9.6 单路化）
//
//   核心做法：
//     1) 在 [fMin, fMax] 上以 log10 均匀采样 numPoints 个中心频率 f_i；
//     2) 对每个 f_i 算出它对应的"覆盖频率带宽"（相邻两个采样点的几何平均之差）；
//     3) 直接在主路 magData 上取"带宽内最大幅度"（保峰）。
//   分辨率完全由当前主路 FFT size 决定。
// ==========================================================
void AnalyserHub::getSpectrumMagnitudesBlended (juce::Array<float>& dest,
                                                int   numPoints,
                                                float fMin,
                                                float fMax)
{
    numPoints = juce::jmax (2, numPoints);
    dest.resize (numPoints);

    const double cachedSr = cachedSampleRate.load (std::memory_order_relaxed);
    const double sr       = (cachedSr > 0.0) ? cachedSr : 48000.0;
    const double nyquist = sr * 0.5;

    const double fLo2 = juce::jmax (1.0, static_cast<double> (fMin));
    const double fHi2 = juce::jmin (nyquist, static_cast<double> (fMax));
    if (fHi2 <= fLo2)
    {
        auto* outData = dest.getRawDataPointer();
        std::fill (outData, outData + numPoints, 0.0f);
        return;
    }

    // P2-1：UI 线程里的每个 Spectrum/Spectrogram 实例每帧都会调一次本函数。
    //   thread_local 静态缓冲：函数调用零分配，跨实例复用同一块 TLS 内存。
    //   线程安全性：specLock 保护的只是"拷贝瞬间"，本地 snapshot 仅当前线程自读自写。
    //   反馈 #7：magData 尺寸随 FFT size 动态变化（1024/2 .. 16384/2 bin），
    //   快照为动态 vector，FFT size 直接决定显示分辨率。
    thread_local std::vector<float> magSnapshot;

    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        magSnapshot.resize (magData.size());
        if (! magSnapshot.empty())
            std::memcpy (magSnapshot.data(), magData.data(),
                         sizeof (float) * magSnapshot.size());
    }

    const double logMin = std::log10 (fLo2);
    const double logMax = std::log10 (fHi2);

    // "Hz → bin" 因子（bin 数 = 当前主路 FFT size / 2）
    const double hzToBin = (magSnapshot.size() > 1)
        ? (static_cast<double> (magSnapshot.size() - 1) / nyquist)
        : 0.0;

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
        // 以点 i 为中心的频率带宽 [f0, f1]（几何平均法）
        const double t   = static_cast<double> (i) / static_cast<double> (numPoints - 1);
        const double f   = std::pow (10.0, logMin + t * (logMax - logMin));

        const double tPrev = static_cast<double> (i - 1) / static_cast<double> (numPoints - 1);
        const double tNext = static_cast<double> (i + 1) / static_cast<double> (numPoints - 1);
        const double fPrev = (i == 0)               ? f : std::pow (10.0, logMin + tPrev * (logMax - logMin));
        const double fNext = (i == numPoints - 1)   ? f : std::pow (10.0, logMin + tNext * (logMax - logMin));
        const double f0    = std::sqrt (fPrev * f);
        const double f1    = std::sqrt (f * fNext);

        // ---- 主路取值 ----
        float mag = 0.0f;
        if (hzToBin > 0.0 && ! magSnapshot.empty())
        {
            const int binLo = static_cast<int> (std::floor (f0 * hzToBin));
            const int binHi = static_cast<int> (std::ceil  (f1 * hzToBin));
            mag = peakInRange (magSnapshot.data(), (int) magSnapshot.size(), binLo, binHi);
        }

        outData[i] = mag;
    }
}

// ==========================================================
// 重分配谱图（频率重分配）—— 相位差法估计瞬时频率
//
//   原理（重分配谱图的相位差实现，phase-vocoder 风格）：
//     STFT 每个 bin 的"能量质心"并不在 bin 中心频率，而在"瞬时频率"
//       ω̂_k = ω_k + Δφ_k / hopTime
//     其中 Δφ_k = arg(X_k · conj(X_prev,k)) 是相邻帧的相位差。
//     把 |X_k|² 从 bin 中心 ω_k 搬到瞬时频率 ω̂_k，就能把因频谱泄漏
//     而扩散的能量重新"凝聚"到真实频率上 —— 视觉上从"脏热点"变成"粒子"。
//
//   v2.9.6 单路化：主路 75% overlap（hop = size/4）。相位差法的
//   "无折叠范围"是 |Δf| ≤ fs/(2·hop) = 2·binHz —— 恒为 ±2 bin，
//   覆盖 Hann 主瓣，任何 size 档位下都能干净收束、无折叠伪影
//   （旧版主路非重叠只有 ±0.5 bin，主瓣边缘 bin 全折叠成伪影线）。
//
//   折叠伪影的去除（关键！无此过滤则正弦波显示为等间隔 fs/hop 的多根线）：
//     无折叠范围之外的 bin，Δφ 被 atan2 折叠，散射到 f ± n·(fs/hop) 的
//     镜像位置。真实单频成分主瓣内相邻 bin 的瞬时频率"一致"（都指向
//     同一频率），折叠 bin 的瞬时频率与邻居"跳变" —— 这正是重分配谱图
//     标准的相干性过滤（coherence test）。再配合帧内相对门限压掉
//     旁瓣折叠组，正弦波才能像 MiniMeters 一样收束成一根线。
// ==========================================================
void AnalyserHub::getSpectrumReassignedBlended (juce::Array<float>& dest,
                                                int   numPoints,
                                                float fMin,
                                                float fMax)
{
    numPoints = juce::jmax (2, numPoints);
    dest.resize (numPoints);
    auto* outData = dest.getRawDataPointer();
    std::fill (outData, outData + numPoints, 0.0f);

    const double cachedSr = cachedSampleRate.load (std::memory_order_relaxed);
    const double sr       = (cachedSr > 0.0) ? cachedSr : 48000.0;
    const double nyquist  = sr * 0.5;

    const double fLo = juce::jmax (1.0, static_cast<double> (fMin));
    const double fHi = juce::jmin (nyquist, static_cast<double> (fMax));
    if (fHi <= fLo) return;

    // v2.9.6 单路化：主路 75% overlap（hop = size/4）。
    //   瞬时频率偏移 Δf = Δφ / (2π·hopTime) = Δφ · fs / (2π·hopSamples)。
    //   注意单位：dphi 是弧度，换算成 Hz 要乘 fs/(2π·hop)，而非除以 (2π/hopTime)
    //   —— 后者会把偏移缩小 hopTime² 倍，导致能量几乎不搬动。
    //   hop = size/4 → 无折叠范围 fs/(2·hop) = 2·binHz（±2 bin），覆盖 Hann 主瓣。
    const double hopSamples = static_cast<double> (mainHop);
    const double fftN       = static_cast<double> (mainFftSize);
    const double binHz      = sr / fftN;
    const double dphiToHz   = sr / (juce::MathConstants<double>::twoPi * hopSamples);

    // thread_local 快照（与 getSpectrumMagnitudesBlended 一致，零栈分配）
    thread_local std::vector<float> reSnap, imSnap, preSnap, pimSnap, fhat, mag2;
    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        reSnap.resize (specRe.size());
        imSnap.resize (specIm.size());
        preSnap.resize (prevSpecRe.size());
        pimSnap.resize (prevSpecIm.size());
        if (! reSnap.empty())
        {
            std::memcpy (reSnap.data(),  specRe.data(),     sizeof (float) * reSnap.size());
            std::memcpy (imSnap.data(),  specIm.data(),     sizeof (float) * imSnap.size());
            std::memcpy (preSnap.data(), prevSpecRe.data(), sizeof (float) * preSnap.size());
            std::memcpy (pimSnap.data(), prevSpecIm.data(), sizeof (float) * pimSnap.size());
        }
    }

    const int nBins = (int) reSnap.size();
    if (nBins <= 1)
        return;

    fhat.resize ((size_t) nBins);
    mag2.resize ((size_t) nBins);

    const double logMin   = std::log10 (fLo);
    const double logMax   = std::log10 (fHi);
    const double logRange = juce::jmax (1.0e-9, logMax - logMin);

    // 绝对能量阈值：对应约 -90 dBFS（与 UI 显示下限 minDb 一致）
    constexpr float kMag2Threshold = 1.0e-9f;
    // 帧内相对门限：比本帧最强 bin 低 30dB 以上的不散射。
    //   作用：压掉旁瓣的"折叠镜像组"（相邻旁瓣 bin 常折叠到同一镜像位置，
    //   单靠相干过滤拦不住）。对复音/音乐 = Sharp 模式动态范围 30dB，
    //   更弱的成分看 Classic。
    constexpr float kRelMag2FloorDb = 30.0f;

    // ---- Pass 1：逐 bin 瞬时频率 + 能量 ----
    float maxMag2 = 0.0f;
    for (int k = 0; k < nBins; ++k)
    {
        const float re   = reSnap[static_cast<size_t> (k)];
        const float im   = imSnap[static_cast<size_t> (k)];
        const float m2   = re * re + im * im;
        mag2[static_cast<size_t> (k)] = m2;

        if (m2 < kMag2Threshold)
        {
            fhat[static_cast<size_t> (k)] = static_cast<float> (static_cast<double> (k) * binHz);
            continue;
        }
        if (m2 > maxMag2) maxMag2 = m2;

        const float pre = preSnap[static_cast<size_t> (k)];
        const float pim = pimSnap[static_cast<size_t> (k)];

        // 相位差（含 heterodyne 修正）：
        //   Δφ = princarg( arg(X_cur·conj(X_prev)) − 2π·k·hop/N )
        //   其中 −2π·k·hop/N 是"bin k 固有频率 f_k 在 hop 样本内的自然相位
        //   推进"。JUCE 的 FFT 是切片式（窗口固定、帧信号为 x[n+t] 切片），
        //   其相邻帧相位差 = 2π·f0·hop/fs 与 bin 无关、且对任何
        //   f0 > fs/(2·hop) 全部折叠；必须减去这个固有推进项，才能得到真正
        //   的瞬时频率偏移（标准 phase-vocoder / Fitz-Haken 公式）。漏掉它
        //   会导致 f̂ 全部错误 → 相干过滤把一切拒掉（频谱全空）。
        const double expectedPhase = juce::MathConstants<double>::twoPi
                                   * static_cast<double> (k) * hopSamples
                                   / fftN;
        double dphi = static_cast<double> (std::atan2 (im * pre - re * pim, re * pre + im * pim))
                    - expectedPhase;
        dphi = std::atan2 (std::sin (dphi), std::cos (dphi));   // princarg 折叠到 [-π, π]

        // 瞬时频率：f̂ = f_k + Δφ · fs/(2π·hop)
        fhat[static_cast<size_t> (k)] = static_cast<float> (
            static_cast<double> (k) * binHz + dphi * dphiToHz);
    }

    // 帧内相对门限（-30dB，能量域）
    const float relMag2Floor = juce::jmax (kMag2Threshold,
        maxMag2 * std::pow (10.0f, -kRelMag2FloorDb / 10.0f));

    // ---- Pass 2：相干性过滤 + 散射 ----
    //   真实成分主瓣内相邻 bin 的 f̂ 一致；折叠/噪声 bin 的 f̂ 与邻居跳变。
    //   相干判定：与左或右任一邻居的 f̂ 差 ≤ 1 个 bin 宽即认为相干。
    for (int k = 0; k < nBins; ++k)
    {
        const float m2 = mag2[static_cast<size_t> (k)];
        if (m2 < relMag2Floor) continue;

        const float fk = fhat[static_cast<size_t> (k)];
        bool coherent = false;
        if (k > 0
            && std::fabs (fk - fhat[static_cast<size_t> (k - 1)]) <= binHz)
            coherent = true;
        if (! coherent && k < nBins - 1
            && std::fabs (fk - fhat[static_cast<size_t> (k + 1)]) <= binHz)
            coherent = true;
        if (! coherent) continue;   // 折叠/噪声 bin：弃

        // 落到对应对数行（clamp 到 [fLo, fHi]）
        //   dest 顺序与 getSpectrumMagnitudesBlended 一致：index 0=低频 → 末尾=高频
        const double f   = juce::jlimit (fLo, fHi, static_cast<double> (fk));
        const double t   = (std::log10 (f) - logMin) / logRange;   // 0=低频, 1=高频
        const int    row = static_cast<int> (std::round (t * static_cast<double> (numPoints - 1)));
        const int    rr  = juce::jlimit (0, numPoints - 1, row);

        // 能量累加：凝聚到同一行的多个 bin 求和
        outData[rr] += m2;
    }

    // 能量 → 幅度（与 Classic 幅度同量纲，UI 的 dB 映射无需改）
    for (int i = 0; i < numPoints; ++i)
        outData[i] = std::sqrt (juce::jmax (0.0f, outData[i]));
}

// ==========================================================
// 重分配谱峰（Spectrum Sharper 模式）—— 乐音收束成谱线
//
//   相位差重分配（与 Sharp 谱图同公式）→ 逐 bin 瞬时频率 f̂_k 与能量 m2_k；
//   门限过滤后，把"瞬时频率与邻居相干（Δf̂ ≤ binHz）"的连续 bin 聚合成
//   谱峰段 —— 每段即一根乐音谱线（含基频与谐波），频率取段内能量加权
//   平均（亚 bin 精度），幅度取段内凝聚能量开方。
// ==========================================================
void AnalyserHub::getSpectrumReassignedPeaks (std::vector<ReassignedPeak>& out,
                                              float fMinHz,
                                              float fMaxHz,
                                              int   maxPeaks)
{
    out.clear();
    if (maxPeaks <= 0)
        return;

    const double cachedSr = cachedSampleRate.load (std::memory_order_relaxed);
    const double sr       = (cachedSr > 0.0) ? cachedSr : 48000.0;
    const double nyquist  = sr * 0.5;

    const double fLo = juce::jmax (1.0, static_cast<double> (fMinHz));
    const double fHi = juce::jmin (nyquist, static_cast<double> (fMaxHz));
    if (fHi <= fLo)
        return;

    // 主路 75% overlap：hop = size/4，无折叠范围 ±2 bin
    const double hopSamples = static_cast<double> (mainHop);
    const double fftN       = static_cast<double> (mainFftSize);
    const double binHz      = sr / fftN;
    const double dphiToHz   = sr / (juce::MathConstants<double>::twoPi * hopSamples);

    // thread_local 快照（与 Sharp 谱图共用同一套帧对数据，零新增 FFT）
    thread_local std::vector<float> reSnap, imSnap, preSnap, pimSnap, fhat, mag2;
    {
        const juce::SpinLock::ScopedLockType sl (specLock);
        reSnap.resize (specRe.size());
        imSnap.resize (specIm.size());
        preSnap.resize (prevSpecRe.size());
        pimSnap.resize (prevSpecIm.size());
        if (! reSnap.empty())
        {
            std::memcpy (reSnap.data(),  specRe.data(),     sizeof (float) * reSnap.size());
            std::memcpy (imSnap.data(),  specIm.data(),     sizeof (float) * imSnap.size());
            std::memcpy (preSnap.data(), prevSpecRe.data(), sizeof (float) * preSnap.size());
            std::memcpy (pimSnap.data(), prevSpecIm.data(), sizeof (float) * pimSnap.size());
        }
    }

    const int nBins = (int) reSnap.size();
    if (nBins <= 1)
        return;

    fhat.resize ((size_t) nBins);
    mag2.resize ((size_t) nBins);

    // ---- Pass 1：逐 bin 瞬时频率 + 能量 ----
    // 绝对能量阈值：约 -90 dBFS（与 Sharp 谱图一致）
    constexpr float kMag2Threshold = 1.0e-9f;
    float maxMag2 = 0.0f;
    for (int k = 0; k < nBins; ++k)
    {
        const float re = reSnap[static_cast<size_t> (k)];
        const float im = imSnap[static_cast<size_t> (k)];
        const float m2 = re * re + im * im;
        mag2[static_cast<size_t> (k)] = m2;

        if (m2 < kMag2Threshold)
        {
            fhat[static_cast<size_t> (k)] = static_cast<float> (static_cast<double> (k) * binHz);
            continue;
        }
        if (m2 > maxMag2) maxMag2 = m2;

        const float pre = preSnap[static_cast<size_t> (k)];
        const float pim = pimSnap[static_cast<size_t> (k)];

        // heterodyne 修正后的相位差 → 瞬时频率（Fitz-Haken 公式）
        const double expectedPhase = juce::MathConstants<double>::twoPi
                                   * static_cast<double> (k) * hopSamples / fftN;
        double dphi = static_cast<double> (std::atan2 (im * pre - re * pim, re * pre + im * pim))
                    - expectedPhase;
        dphi = std::atan2 (std::sin (dphi), std::cos (dphi));
        fhat[static_cast<size_t> (k)] = static_cast<float> (
            static_cast<double> (k) * binHz + dphi * dphiToHz);
    }

    // 帧内相对门限：比最强 bin 低 24dB 以上不参与。
    //   反馈修订：原 30dB 过宽，弱泛音/噪声段大量进入谱线列表；
    //   收紧到 24dB 后只保留真正显著的音乐成分。
    const float relFloor = juce::jmax (kMag2Threshold,
        maxMag2 * std::pow (10.0f, -24.0f / 10.0f));

    // ---- Pass 2：相干段聚合（相邻 bin 的 f̂ 差 ≤ binHz 视为同一谱线）----
    //   一根稳态正弦在 Hann 主瓣内 2~3 个 bin 都超门限且 f̂ 一致 → 聚成一段。
    //   段内频率取能量加权平均（亚 bin 精度、抗旁瓣抖动）。
    struct Seg
    {
        double fSum = 0.0;
        double wSum = 0.0;
        float  energy = 0.0f;
        int    bins   = 0;
    };
    thread_local std::vector<Seg> segs;
    segs.clear();

    for (int k = 0; k < nBins; ++k)
    {
        const float m2 = mag2[static_cast<size_t> (k)];
        if (m2 < relFloor)
            continue;

        const double fk = static_cast<double> (fhat[static_cast<size_t> (k)]);
        if (k > 0 && ! segs.empty()
            && std::fabs (fk - static_cast<double> (fhat[static_cast<size_t> (k - 1)])) <= binHz)
        {
            Seg& s = segs.back();
            s.fSum += fk * m2;
            s.wSum += m2;
            s.energy += m2;
            ++s.bins;
        }
        else
        {
            segs.push_back ({ fk * m2, (double) m2, m2, 1 });
        }
    }

    // ---- Pass 3：段 → 谱峰（过滤频段 + 按能量排序取 top）----
    //   反馈修订（门限拉高）：
    //     · 孤立单 bin 段（噪声/旁瓣毛刺）丢弃，除非能量显著（≥4×相对门限）；
    //     · 绝对幅度下限 ≈ -74 dBFS，压掉纯噪声段。
    thread_local std::vector<ReassignedPeak> candidates;
    candidates.clear();

    for (const auto& s : segs)
    {
        if (s.energy <= 0.0f || s.wSum <= 0.0)
            continue;
        if (s.bins < 2 && s.energy < relFloor * 4.0f)
            continue;

        const double f = s.fSum / s.wSum;
        if (f < fLo || f > fHi)
            continue;

        const float mag = std::sqrt (s.energy);
        if (mag < 1.0e-4f)
            continue;

        ReassignedPeak p;
        p.freqHz = f;
        p.mag    = mag;
        candidates.push_back (p);
    }

    std::sort (candidates.begin(), candidates.end(),
               [] (const ReassignedPeak& a, const ReassignedPeak& b)
               {
                   return a.mag > b.mag;
               });

    const int nOut = juce::jmin (maxPeaks, (int) candidates.size());
    out.reserve ((size_t) nOut);
    for (int i = 0; i < nOut; ++i)
        out.push_back (candidates[(size_t) i]);
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
    out.avgPushUs = (pc > 0) ? static_cast<double> (pt) / static_cast<double> (pc) : 0.0;
    out.avgSnapUs = (sc > 0) ? static_cast<double> (st) / static_cast<double> (sc) : 0.0;
    out.maxPushUs = static_cast<double> (pm);
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
    frameListenersReserved = juce::jmax (frameListenersReserved, static_cast<int> (frameListeners.size()));
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
            if (static_cast<int> (frameListeners.size()) < frameListenersReserved / 2)
                frameListenersReserved = static_cast<int> (frameListeners.size());
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

    currentFrameDispatcherHz.store (safeHz, std::memory_order_relaxed);

    // 外部 vblank 时钟接管期间只记录目标频率（供回退时使用），不启动 Timer
    if (! externalFrameClock)
        frameDispatcher->startTimerHz (safeHz);
}

void AnalyserHub::stopFrameDispatcher()
{
    if (frameDispatcher != nullptr)
        frameDispatcher->stopTimer();
    currentFrameDispatcherHz.store (0, std::memory_order_relaxed);
    externalFrameClock = false;
}

void AnalyserHub::setExternalFrameClock (bool shouldUseExternalClock)
{
    if (frameDispatcher == nullptr || externalFrameClock == shouldUseExternalClock)
        return;

    externalFrameClock = shouldUseExternalClock;

    if (externalFrameClock)
    {
        frameDispatcher->stopTimer();
    }
    else
    {
        const int hz = currentFrameDispatcherHz.load (std::memory_order_relaxed);
        if (hz > 0)
            frameDispatcher->startTimerHz (hz);
    }
}

void AnalyserHub::dispatchFrameNow (double timestampSec)
{
    if (frameDispatcher == nullptr)
        return;

    frameDispatcher->dispatch (timestampSec >= 0.0 ? timestampSec
                                                   : juce::Time::getMillisecondCounterHiRes() * 0.001);
}
