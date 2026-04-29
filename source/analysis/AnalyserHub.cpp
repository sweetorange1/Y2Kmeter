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
        y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::ui, "UI-FrameDispatcher");
        y2k::perf::ScopedPerfTimer frameTimer(y2k::perf::FunctionId::uiFrameDispatcher,
                                              y2k::perf::Partition::uiRendering,
                                              y2k::perf::ThreadRole::ui);

        // 1) 组装活跃 mask
        juce::uint32 mask = 0;
        for (int i = 0; i < (int) Kind::NumKinds; ++i)
            if (owner.refCounts[(size_t) i].load (std::memory_order_relaxed) > 0)
                mask |= (juce::uint32) (1u << i);

        if (mask == 0)
            return; // 所有路都无人订阅 —— 不生成帧

        auto frame = std::make_shared<FrameSnapshot>();
        y2k::perf::PerformanceCounterSystem::instance().recordMemoryAlloc(sizeof(FrameSnapshot));
        frame->activeMask = mask;

        y2k::perf::ScopedPerfTimer assembleTimer(y2k::perf::FunctionId::uiFrameAssemble,
                                                 y2k::perf::Partition::uiRendering,
                                                 y2k::perf::ThreadRole::ui);

        // 2) 按需填充各路数据
        if (frame->has (Kind::Oscilloscope))
        {
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
            const juce::SpinLock::ScopedLockType sl (owner.oscLock);
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockOsc,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
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
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
            const juce::SpinLock::ScopedLockType sl (owner.specLock);
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockSpec,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
            std::memcpy (frame->spectrumMag .data(), owner.magData .data(),
                         sizeof (float) * frame->spectrumMag .size());
            std::memcpy (frame->spectrumData.data(), owner.specData.data(),
                         sizeof (float) * frame->spectrumData.size());
            std::memcpy (frame->spectrumMagLo.data(), owner.magDataLo.data(),
                         sizeof (float) * frame->spectrumMagLo.size());
        }

        if (frame->has (Kind::Loudness))
            frame->loudness = owner.loudnessMeter.getSnapshot();

        if (frame->has (Kind::Phase))
            frame->phase    = owner.phaseCorrelator.getSnapshot();

        if (frame->has (Kind::Dynamics))
            frame->dynamics = owner.dynamicsMeter.getSnapshot();

        // 3) 原子发布到 latestFrame
        {
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
            const juce::SpinLock::ScopedLockType sl (owner.latestFrameLock);
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockLatestFrame,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
            owner.latestFrame = frame;
        }

        y2k::perf::PerformanceCounterSystem::instance().recordEvent(
            y2k::perf::FunctionId::dataPublishLatestFrame,
            y2k::perf::Partition::dataCommunication,
            y2k::perf::ThreadRole::ui,
            1);

        // 4) 通知所有监听器（UI 线程）
        //    拷贝一份 vector 避免在回调里触发 add/remove 导致迭代失效
        std::vector<FrameListener*> localCopy;
        {
            const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
            const juce::ScopedLock sl (owner.frameListenersLock);
            const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
            y2k::perf::PerformanceCounterSystem::instance().recordDuration(
                y2k::perf::FunctionId::lockFrameListeners,
                y2k::perf::Partition::dataCommunication,
                y2k::perf::ThreadRole::ui,
                0,
                lockNs - waitStart);
            localCopy = owner.frameListeners;
        }

        y2k::perf::PerformanceCounterSystem::instance().recordDuration(
            y2k::perf::FunctionId::dataListenerListCopy,
            y2k::perf::Partition::dataCommunication,
            y2k::perf::ThreadRole::ui,
            0,
            0);

        std::vector<FrameListener*> activeListeners;
        activeListeners.reserve(localCopy.size());
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

        y2k::perf::PerformanceCounterSystem::instance().recordFrameListenerCount((int) activeListeners.size());
        y2k::perf::PerformanceCounterSystem::instance().recordEvent(
            y2k::perf::FunctionId::uiFrameListenerFanout,
            y2k::perf::Partition::uiRendering,
            y2k::perf::ThreadRole::ui,
            (juce::int64) activeListeners.size());
        for (auto* lst : activeListeners)
        {
            y2k::perf::ScopedPerfTimer onFrameTimer(y2k::perf::FunctionId::uiOnFrameDispatch,
                                                    y2k::perf::Partition::uiRendering,
                                                    y2k::perf::ThreadRole::ui);
            lst->onFrame (*frame);
        }

        y2k::perf::PerformanceCounterSystem::instance().recordMemoryFree(sizeof(FrameSnapshot));
    }

private:
    AnalyserHub& owner;
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

    // 示波器缓冲清零
    {
        const juce::SpinLock::ScopedLockType sl(oscLock);
        oscBufL.fill(0.0f);
        oscBufR.fill(0.0f);
        oscWritePos = 0;
    }

    // 频谱缓冲清零
    {
        const juce::SpinLock::ScopedLockType sl(specLock);

        fftFifo.fill(0.0f);

        fftData.fill(0.0f);
        specData.fill(0.0f);
        magData.fill(0.0f);
        fftFifoIndex = 0;

        // 低频路
        fftFifoLo.fill(0.0f);
        fftDataLo.fill(0.0f);
        magDataLo.fill(0.0f);
        fftFifoIndexLo = 0;
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

    y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::audio, "Audio-AnalyserHub");
    y2k::perf::ScopedPerfTimer pushTimer(y2k::perf::FunctionId::analyserPushTotal,
                                         y2k::perf::Partition::audioAnalysis,
                                         y2k::perf::ThreadRole::audio);

    const auto t0 = juce::Time::getHighResolutionTicks();

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
        y2k::perf::ScopedPerfTimer t(y2k::perf::FunctionId::analyserLoudness,
                                     y2k::perf::Partition::audioAnalysis,
                                     y2k::perf::ThreadRole::audio);
        loudnessMeter.pushStereo (left, right, numSamples);
    }

    if (wantPhase)
    {
        y2k::perf::ScopedPerfTimer t(y2k::perf::FunctionId::analyserPhase,
                                     y2k::perf::Partition::audioAnalysis,
                                     y2k::perf::ThreadRole::audio);
        phaseCorrelator.pushStereo(left, right, numSamples);
    }

    if (wantDynamics)
    {
        y2k::perf::ScopedPerfTimer t(y2k::perf::FunctionId::analyserDynamics,
                                     y2k::perf::Partition::audioAnalysis,
                                     y2k::perf::ThreadRole::audio);
        dynamicsMeter.pushStereo (left, right, numSamples);
    }

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
    y2k::perf::ScopedPerfTimer stageTimer(y2k::perf::FunctionId::analyserOscilloscope,
                                          y2k::perf::Partition::audioAnalysis,
                                          y2k::perf::ThreadRole::audio);
    const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
    const juce::SpinLock::ScopedLockType sl(oscLock);
    const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
    stageTimer.addLockWait(lockNs - waitStart);
    y2k::perf::PerformanceCounterSystem::instance().recordDuration(
        y2k::perf::FunctionId::lockOsc,
        y2k::perf::Partition::dataCommunication,
        y2k::perf::ThreadRole::audio,
        0,
        lockNs - waitStart);

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
// 频谱（L+R 混合，FFT）
// ==========================================================
void AnalyserHub::pushSamplesToSpectrum(const float* left, const float* right, int numSamples)
{
    y2k::perf::ScopedPerfTimer stageTimer(y2k::perf::FunctionId::analyserSpectrum,
                                          y2k::perf::Partition::audioAnalysis,
                                          y2k::perf::ThreadRole::audio);
    // Phase F 优化：之前是"每个样本都 trylock / 不成就 continue 丢样本"，
    //   导致：
    //     1) 每样本一次 lock/unlock 开销非常贵（48kHz × stereo → 一秒 96000 次上锁）；
    //     2) UI 在 getSpectrumMagnitudes 里拿 scopedLock 的几十微秒期间，
    //        音频线程会把这段时间的所有样本直接丢掉 → 频谱实际比真实信号少一截。
    //   改造：外层一次 scopedLock，内部紧凑 FIFO；FFT 仍在持锁期间执行，
    //   这段时间 UI 侧 getSpectrumMagnitudes 会短暂自旋，但因为 FFT 只在 fftSize
    //   个样本攒满时才触发，一次 pushStereo 最多触发 1~2 次，UI 等待时间完全可接受。
    const auto waitStart = y2k::perf::PerformanceCounterSystem::nowNs();
    const juce::SpinLock::ScopedLockType sl(specLock);
    const auto lockNs = y2k::perf::PerformanceCounterSystem::nowNs();
    stageTimer.addLockWait(lockNs - waitStart);
    y2k::perf::PerformanceCounterSystem::instance().recordDuration(
        y2k::perf::FunctionId::lockSpec,
        y2k::perf::Partition::dataCommunication,
        y2k::perf::ThreadRole::audio,
        0,
        lockNs - waitStart);

    // 低频路的 hop（75% overlap）：每 fftSizeLo/4 样本跑一次 FFT，

    // 兼顾"更新速度"与"CPU 成本"。
    constexpr int hopLo = fftSizeLo / 4;

    for (int i = 0; i < numSamples; ++i)
    {
        // 混合为 mid（L+R）/2
        const float mid = (left[i] + right[i]) * 0.5f;
        const float s   = juce::jlimit(-1.0f, 1.0f, mid);

        // ---------- 主路（2048）：非重叠，攒满就跑 ----------
        fftFifo[(size_t) fftFifoIndex] = s;
        ++fftFifoIndex;

        if (fftFifoIndex >= fftSize)
        {
            // 执行 FFT
            std::fill(fftData.begin(), fftData.end(), 0.0f);
            std::copy(fftFifo.begin(), fftFifo.end(), fftData.begin());
            window.multiplyWithWindowingTable(fftData.data(), fftSize);
            fft.performFrequencyOnlyForwardTransform(fftData.data());

            const int maxBin = fftSize / 2;

            // 1) 拷贝高精度幅度数组（供 SpectrumModule 使用）
            //    将幅度归一化到 [0,1]（用 FFT 大小归一 + 汉宁窗补偿）
            //    简化做法：除以 fftSize/2 作为粗略归一化，后续 UI 自己转 dB
            {
                const float invMax = 2.0f / (float) fftSize;
                for (int iBin = 0; iBin < maxBin; ++iBin)
                    magData[(size_t) iBin] = fftData[(size_t) iBin] * invMax;
            }

            // 2) 像素 EQ 用的粗粒度平滑频谱（保留原有逻辑）
            for (int iBin = 0; iBin < spectrumBins; ++iBin)
            {
                const float norm  = (float) iBin / (float) juce::jmax(1, spectrumBins - 1);
                const float skew  = std::pow(norm, 2.3f);
                const int   fBin  = juce::jlimit(0, maxBin - 1,
                                        (int) std::round(skew * (float)(maxBin - 1)));

                const float mag    = fftData[(size_t) fBin];
                const float db     = juce::Decibels::gainToDecibels(juce::jmax(1.0e-6f, mag));
                const float mapped = juce::jlimit(0.0f, 1.0f,
                                        juce::jmap(db, -50.0f, 50.0f, 0.0f, 1.0f));

                const float prev  = specData[(size_t) iBin];
                const float alpha = (mapped > prev) ? 0.6f : 0.15f;
                specData[(size_t) iBin] = prev + alpha * (mapped - prev);
            }

            fftFifoIndex = 0;
        }

        // ---------- 低频路（8192）：环形缓冲 + 75% overlap ----------
        //   存入当前写位置，再把"写位置"前进。当写位置每次跨过 hopLo 的整数倍时，
        //   就把"从 writePos 往回看的 fftSizeLo 个样本"打包跑一次 FFT。
        //   数据从最老到最新的排列为：fftFifoLo[writePos..end] + fftFifoLo[0..writePos-1]
        fftFifoLo[(size_t) fftFifoIndexLo] = s;
        fftFifoIndexLo = (fftFifoIndexLo + 1) % fftSizeLo;

        // 每 hopLo 个样本触发一次
        //   注意：initial 填充期（累计样本 < fftSizeLo）也会跑，但 FIFO 的零值部分
        //   等价于补零，幅度只会偏小不会炸。实际运行几秒后就 warm-up 完毕。
        if ((fftFifoIndexLo % hopLo) != 0)
            continue;

        // 将环形 FIFO 展平到 fftDataLo（时序：oldest → newest）
        const int firstChunk = fftSizeLo - fftFifoIndexLo;
        std::memcpy (fftDataLo.data(),              fftFifoLo.data() + fftFifoIndexLo,
                     (size_t) firstChunk * sizeof (float));
        if (fftFifoIndexLo > 0)
            std::memcpy (fftDataLo.data() + firstChunk, fftFifoLo.data(),
                         (size_t) fftFifoIndexLo * sizeof (float));
        // fftDataLo 后半段（fftSizeLo .. fftSizeLo*2）用作 FFT 内部缓冲，填 0
        std::fill (fftDataLo.begin() + fftSizeLo, fftDataLo.end(), 0.0f);

        windowLo.multiplyWithWindowingTable(fftDataLo.data(), fftSizeLo);
        fftLo.performFrequencyOnlyForwardTransform(fftDataLo.data());

        // 归一化（与主路保持一致的公式：2 / fftSize）
        {
            const int   maxBinLo = fftSizeLo / 2;
            const float invMaxLo = 2.0f / (float) fftSizeLo;
            for (int iBin = 0; iBin < maxBinLo; ++iBin)
                magDataLo[(size_t) iBin] = fftDataLo[(size_t) iBin] * invMaxLo;
        }
    }
}

void AnalyserHub::getSpectrumSnapshot(juce::Array<float>& dest)
{
    dest.resize(spectrumBins);
    const juce::SpinLock::ScopedLockType sl(specLock);
    for (int i = 0; i < spectrumBins; ++i)
        dest.set(i, specData[(size_t) i]);
}

void AnalyserHub::getSpectrumMagnitudes(juce::Array<float>& dest)
{
    const auto t0 = juce::Time::getHighResolutionTicks();

    dest.resize(spectrumMagSize);
    {
        const juce::SpinLock::ScopedLockType sl(specLock);
        for (int i = 0; i < spectrumMagSize; ++i)
            dest.set(i, magData[(size_t) i]);
    }

    const auto t1 = juce::Time::getHighResolutionTicks();
    const auto us = (juce::int64) (juce::Time::highResolutionTicksToSeconds(t1 - t0) * 1.0e6);
    perfSnapCount.fetch_add(1,  std::memory_order_relaxed);
    perfSnapTotalUs.fetch_add(us, std::memory_order_relaxed);
}

// ==========================================================
// 低频路高精度幅度快照（4096 bin）
// ==========================================================
void AnalyserHub::getSpectrumMagnitudesLo(juce::Array<float>& dest)
{
    dest.resize(spectrumMagSizeLo);
    const juce::SpinLock::ScopedLockType sl(specLock);
    for (int i = 0; i < spectrumMagSizeLo; ++i)
        dest.set(i, magDataLo[(size_t) i]);
}

// ==========================================================
// 合并（对数频率轴）幅度快照 —— Hub 内部完成双路拼接
//
//   核心做法：
//     1) 在 [fMin, fMax] 上以 log10 均匀采样 numPoints 个中心频率 f_i；
//     2) 对每个 f_i 算出它对应的"覆盖频率带宽"（相邻两个采样点的几何平均之差）；
//     3) 根据 f_i 所在频段，从主路 magData（1024 bin，Δf≈23Hz @48k）
//        或低频路 magDataLo（4096 bin，Δf≈5.86Hz @48k）取"带宽内最大幅度"；
//     4) 在过渡带内（以 spectrumXoverHz 为中心、±半八度），
//        对两路按能量线性交叉淡化（power-preserving crossfade）拼合；
//     5) 低频路超出其有效频段（> ~2×spectrumXoverHz）时强制只用主路，
//        避免取到低通之外的 bin。
// ==========================================================
void AnalyserHub::getSpectrumMagnitudesBlended (juce::Array<float>& dest,
                                                int   numPoints,
                                                float fMin,
                                                float fMax)
{
    numPoints = juce::jmax (2, numPoints);
    dest.resize (numPoints);

    const double sr      = (cachedSampleRate > 0.0) ? cachedSampleRate : 48000.0;
    const double nyquist = sr * 0.5;

    const double fLo2 = juce::jmax (1.0, (double) fMin);
    const double fHi2 = juce::jmin (nyquist, (double) fMax);
    if (fHi2 <= fLo2) { for (int i = 0; i < numPoints; ++i) dest.set (i, 0.0f); return; }

    const double logMin = std::log10 (fLo2);
    const double logMax = std::log10 (fHi2);

    // 主路 / 低频路的 "Hz → bin" 因子
    const double hzToBinHi = (double) (spectrumMagSize   - 1) / nyquist;
    const double hzToBinLo = (double) (spectrumMagSizeLo - 1) / nyquist;

    // 过渡带（对数中心 = spectrumXoverHz，±0.5 八度 → ratio = sqrt(2)）
    const double xover     = (double) spectrumXoverHz;
    const double xoverLo   = xover / std::sqrt (2.0);   // ~354 Hz
    const double xoverHi   = xover * std::sqrt (2.0);   // ~707 Hz
    // 低频路 FFT 低通之外禁用（取 2× xover 作为硬上限兜底）
    const double loHardMax = xover * 2.0;

    const juce::SpinLock::ScopedLockType sl (specLock);

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
        const double t   = (double) i / (double) (numPoints - 1);
        const double f   = std::pow (10.0, logMin + t * (logMax - logMin));

        const double tPrev = (double) (i - 1) / (double) (numPoints - 1);
        const double tNext = (double) (i + 1) / (double) (numPoints - 1);
        const double fPrev = (i == 0)               ? f : std::pow (10.0, logMin + tPrev * (logMax - logMin));
        const double fNext = (i == numPoints - 1)   ? f : std::pow (10.0, logMin + tNext * (logMax - logMin));
        const double f0    = std::sqrt (fPrev * f);
        const double f1    = std::sqrt (f * fNext);

        // ---- 主路取值 ----
        const int binHiLo  = (int) std::floor (f0 * hzToBinHi);
        const int binHiUp  = (int) std::ceil  (f1 * hzToBinHi);
        const float magHi  = peakInRange (magData.data(), spectrumMagSize, binHiLo, binHiUp);

        // ---- 低频路取值（只在有效频段内查询）----
        float magLo = 0.0f;
        if (f < loHardMax)
        {
            const int binLoLo = (int) std::floor (f0 * hzToBinLo);
            const int binLoUp = (int) std::ceil  (f1 * hzToBinLo);
            magLo = peakInRange (magDataLo.data(), spectrumMagSizeLo, binLoLo, binLoUp);
        }

        // ---- 交叉淡化（频率在 [xoverLo, xoverHi] 的八度内）----
        //   w = 0 → 纯低频路；w = 1 → 纯主路
        float w;
        if      (f <= xoverLo) w = 0.0f;
        else if (f >= xoverHi) w = 1.0f;
        else
        {
            const double u = (std::log (f) - std::log (xoverLo))
                           / (std::log (xoverHi) - std::log (xoverLo));
            // 等功率（cos²/sin²）交叉：w²(Hi) + (1-w)²(Lo) 之和在能量域平滑
            w = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * u));
        }

        // 能量域线性混合后再开方（避免两峰重合时幅度偏高）
        const float eHi = magHi * magHi;
        const float eLo = magLo * magLo;
        const float eMix = w * eHi + (1.0f - w) * eLo;
        const float out  = std::sqrt (juce::jmax (0.0f, eMix));

        dest.set (i, out);
    }
}

// ==========================================================
// 响度快照
// ==========================================================
LoudnessMeter::Snapshot AnalyserHub::getLoudnessSnapshot()
{
    return loudnessMeter.getSnapshot();
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
            return;
        }
    }
}

std::shared_ptr<const AnalyserHub::FrameSnapshot> AnalyserHub::getLatestFrame() const noexcept
{
    const juce::SpinLock::ScopedLockType sl (latestFrameLock);
    y2k::perf::PerformanceCounterSystem::instance().recordEvent(
        y2k::perf::FunctionId::dataGetLatestFrame,
        y2k::perf::Partition::dataCommunication,
        y2k::perf::ThreadRole::unknown,
        1);
    return latestFrame; // 返回共享副本，调用方可自由持有
}

void AnalyserHub::startFrameDispatcher (int hz)
{
    // 必须在 UI 线程（juce::Timer 要求）
    if (frameDispatcher != nullptr)
        frameDispatcher->startTimerHz (hz);
}

void AnalyserHub::stopFrameDispatcher()
{
    if (frameDispatcher != nullptr)
        frameDispatcher->stopTimer();
}