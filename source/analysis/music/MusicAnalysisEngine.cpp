#include "source/analysis/music/MusicAnalysisEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace y2k::music
{

namespace
{
    constexpr float  kSilenceDb        = -58.0f;   // 低于此电平视为静音
    constexpr double kNoSignalSeconds  = 1.2;      // 超过此时长 → NO SIGNAL
    constexpr double kAutoResetSeconds = 3.0;      // 超过此时长 → 自动 reset（清空读数）
    constexpr double kPublishIntervalMs = 200.0;
}

MusicAnalysisEngine::MusicAnalysisEngine()
    : juce::Thread ("Y2K-MusicAnalysis")
{
    // 构造时不分配缓冲、不创建线程。
    //   只有真有模块订阅（setActive(true)）时才分配 + 启动，
    //   这样"没加载 Tempo/Key 模块"的实例完全不占后台资源。
    published.status = Status::idle;
    lastPublished    = published;
}

MusicAnalysisEngine::~MusicAnalysisEngine()
{
    active.store (false, std::memory_order_release);
    enabled.store (false, std::memory_order_relaxed);
    stopThread (2000);
}

// ==========================================================
// 订阅 / 启停
// ==========================================================
void MusicAnalysisEngine::setActive (bool shouldBeActive) noexcept
{
    if (! shouldBeActive)
    {
        if (! active.exchange (false, std::memory_order_acq_rel))
            return;

        stopWorker();
        return;
    }

    if (active.load (std::memory_order_acquire))
        return;

    // 关键顺序：先把缓冲准备好，再发布 active=true。
    //   否则音频线程可能看到 active=true 而 ring 还在分配（数据竞争）。
    if (! ensureBuffers())
        return;

    // 重新订阅：先清空对外结果，避免模块重新出现时先闪一帧旧读数
    resetRequested.store (true, std::memory_order_relaxed);

    {
        const juce::SpinLock::ScopedLockType sl (resultLock);
        published = Result {};
        published.status = enabled.load (std::memory_order_relaxed) ? Status::analysing
                                                                    : Status::idle;
        published.publishedMs = juce::Time::currentTimeMillis();
        lastPublished = published;
    }

    active.store (true, std::memory_order_release);

    if (enabled.load (std::memory_order_relaxed))
        startWorker();
}

bool MusicAnalysisEngine::ensureBuffers() noexcept
{
    if (! ring.empty())
        return true;

    try
    {
        std::vector<float> r ((size_t) ringCapacity, 0.0f);
        std::vector<float> s (8192, 0.0f);
        ring.swap (r);
        scratch.swap (s);
    }
    catch (...)
    {
        return false;
    }

    return true;
}

void MusicAnalysisEngine::startWorker() noexcept
{
    if (isThreadRunning())
        return;

    startThread (juce::Thread::Priority::low);
}

void MusicAnalysisEngine::stopWorker() noexcept
{
    if (! isThreadRunning())
        return;

    // stopThread = signalThreadShouldExit + notify + waitForThreadToExit
    stopThread (1000);
}

void MusicAnalysisEngine::prepare (double newSampleRate, int maxBlockSamples) noexcept
{
    juce::ignoreUnused (maxBlockSamples);

    // 只记录请求；真正的 FFT 重建在工作线程做，避免阻塞采集线程。
    //   注意：这里不置位 resetRequested —— 仅"块长变化"不应清空 BPM/调性历史，
    //   采样率真正变化时 applyPendingFormat() 会自己重建并清空状态。
    requestedSampleRate.store (std::clamp (newSampleRate, 8000.0, 384000.0),
                               std::memory_order_relaxed);
}

void MusicAnalysisEngine::setEnabled (bool shouldRun) noexcept
{
    const bool wasRunning = enabled.exchange (shouldRun, std::memory_order_relaxed);

    if (wasRunning == shouldRun)
        return;

    if (! shouldRun)
    {
        // STOP：音频线程立即早退，工作线程也直接退出（不是空转轮询）。
        stopWorker();

        // 线程已停，此时写状态不存在并发写者 → 立刻把界面切成 STOPPED，
        // 而不是让最后一条 TRACKING 一直挂着。
        {
            const juce::SpinLock::ScopedLockType sl (resultLock);
            published.status        = Status::idle;
            published.bpmValid      = false;
            published.keyValid      = false;
            published.bpmConfidence = 0.0f;
            published.keyConfidence = 0.0f;
            published.publishedMs   = juce::Time::currentTimeMillis();
            lastPublished           = published;
            lastPublishMs           = 0.0;
        }

        return;
    }

    resetRequested.store (true, std::memory_order_relaxed);

    if (active.load (std::memory_order_acquire))
        startWorker();
}

void MusicAnalysisEngine::reset() noexcept
{
    resetRequested.store (true, std::memory_order_relaxed);
}

void MusicAnalysisEngine::discardQueuedAudio() noexcept
{
    readIndex.store (writeIndex.load (std::memory_order_acquire),
                     std::memory_order_release);
}

void MusicAnalysisEngine::clearAccumulation()
{
    tempo.resetAccumulation();
    key.resetAccumulation();

    fedSamples       = 0.0;
    silentSamples    = 0.0;
    silenceResetDone = false;
}

void MusicAnalysisEngine::autoResetForSilence()
{
    // 与用户按 RESET 等效（但不重新启动线程）：
    //   清空已发布的 BPM / 调性与全部累积窗口，界面回到 "--"。
    // 注意：这里刻意不调用 clearAccumulation() —— 它会把 silentSamples 清零，
    //   导致"静音 3 秒"的判定每 3 秒重新触发一次；静音计时必须继续累加。
    tempo.reset();
    key.reset();

    fedSamples = 0.0;

    const bool running = enabled.load (std::memory_order_relaxed);

    {
        const juce::SpinLock::ScopedLockType sl (resultLock);
        published = Result {};
        published.status = running ? Status::noSignal : Status::idle;
        published.publishedMs = juce::Time::currentTimeMillis();
        lastPublished = published;
    }

    lastPublishMs = 0.0;
}

void MusicAnalysisEngine::applyPendingFormat()
{
    const double req = requestedSampleRate.load (std::memory_order_relaxed);

    if (std::abs (req - appliedSampleRate) < 0.5 && appliedSampleRate > 0.0)
        return;

    appliedSampleRate = req;

    tempo.prepare (appliedSampleRate);
    key.prepare (appliedSampleRate);

    discardQueuedAudio();
    clearAccumulation();

    const juce::SpinLock::ScopedLockType sl (resultLock);
    published     = Result {};
    published.status = Status::analysing;
    lastPublished = published;
    lastPublishMs = 0.0;
}

void MusicAnalysisEngine::pushStereo (const float* left, const float* right, int numSamples) noexcept
{
    // active 用 acquire 读：保证看到 active==true 时环形缓冲已分配完成
    if (! active.load (std::memory_order_acquire))
        return;

    if (! enabled.load (std::memory_order_relaxed))
        return;

    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    if (ring.empty())
        return;

    const uint64_t w = writeIndex.load (std::memory_order_relaxed);
    const uint64_t r = readIndex.load (std::memory_order_acquire);

    if (w < r)
        return; // 防御：索引不应倒挂

    const int used = (int) (w - r);
    const int freeSlots = ringCapacity - used;

    if (freeSlots <= 0)
    {
        droppedSamples.fetch_add ((uint64_t) numSamples, std::memory_order_relaxed);
        gapEpoch.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    const int n = juce::jmin (numSamples, freeSlots);
    uint64_t idx = w & ringMask;

    if (left == right)
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) idx] = left[i];
            idx = (idx + 1) & ringMask;
        }
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) idx] = 0.5f * (left[i] + right[i]);
            idx = (idx + 1) & ringMask;
        }
    }

    writeIndex.store (w + (uint64_t) n, std::memory_order_release);

    if (n < numSamples)
    {
        droppedSamples.fetch_add ((uint64_t) (numSamples - n), std::memory_order_relaxed);
        gapEpoch.fetch_add (1, std::memory_order_relaxed);
    }
}

void MusicAnalysisEngine::publishResult (bool force)
{
    const double nowMs = juce::Time::currentTimeMillis();

    if (! force && lastPublishMs > 0.0 && (nowMs - lastPublishMs) < kPublishIntervalMs)
        return;

    Result r;

    const bool running = enabled.load (std::memory_order_relaxed);
    const bool silent  = appliedSampleRate > 0.0
                      && silentSamples > appliedSampleRate * kNoSignalSeconds;

    if (! running)
        r.status = Status::idle;
    else if (silent)
        r.status = Status::noSignal;
    else if (tempo.hasTempo() && key.hasKey())
        r.status = Status::tracking;
    else
        r.status = Status::analysing;

    // 静音时保留上一次的数值用于显示（UI 会变暗并标注 stale），
    // 但不再宣称它是"当前有效结果"。
    r.bpm      = tempo.bpm();
    r.bpmValid = running && ! silent && tempo.hasTempo();

    r.keyRoot     = key.root();
    r.keyMode     = key.mode();
    r.keyValid    = running && ! silent && key.hasKey();
    r.tuningCents = key.tuningCents();

    // 置信度只在结果"当前有效"时对外呈现；stale 的旧读数不附带置信度，
    // 避免把"很久以前的 80%"当成现在的 80%。
    r.bpmConfidence = r.bpmValid ? tempo.confidence() : 0.0f;
    r.keyConfidence = r.keyValid ? key.confidence() : 0.0f;

    r.levelDb         = tempo.currentLevelDb();
    r.analysedSeconds = appliedSampleRate > 0.0 ? fedSamples / appliedSampleRate : 0.0;
    r.publishedMs     = nowMs;

    // 结果更新时间：只在"数值真的变了"时刷新，用来区分旧结果与当前结果。
    const bool bpmChanged = r.bpmValid != lastPublished.bpmValid
                         || std::abs (r.bpm - lastPublished.bpm) > 0.05f;

    const bool keyChanged = r.keyValid != lastPublished.keyValid
                         || r.keyRoot != lastPublished.keyRoot
                         || r.keyMode != lastPublished.keyMode;

    r.bpmUpdatedMs = bpmChanged ? nowMs : lastPublished.bpmUpdatedMs;
    r.keyUpdatedMs = keyChanged ? nowMs : lastPublished.keyUpdatedMs;

    lastPublished = r;
    lastPublishMs = nowMs;

    const juce::SpinLock::ScopedLockType sl (resultLock);
    published = r;
}

void MusicAnalysisEngine::run()
{
    while (! threadShouldExit())
    {
        applyPendingFormat();

        if (resetRequested.exchange (false, std::memory_order_relaxed))
        {
            tempo.reset();
            key.reset();
            discardQueuedAudio();
            clearAccumulation();

            {
                const juce::SpinLock::ScopedLockType sl (resultLock);
                published = Result {};
                published.status = enabled.load (std::memory_order_relaxed)
                                       ? Status::analysing
                                       : Status::idle;
                published.publishedMs = juce::Time::currentTimeMillis();
                lastPublished = published;
            }

            lastPublishMs = 0.0;
        }

        if (! enabled.load (std::memory_order_relaxed))
        {
            publishResult (false);
            wait (40);
            continue;
        }

        const uint64_t r = readIndex.load (std::memory_order_relaxed);
        const uint64_t w = writeIndex.load (std::memory_order_acquire);
        const uint64_t available = (w > r) ? (w - r) : 0;

        if (available == 0)
        {
            publishResult (false);

            // 刚处理完数据时用短等待（低延迟），长时间无数据（例如模块被
            // 卸载、宿主暂停）时退化为 40ms 轮询，避免无谓唤醒。
            if (++emptyPolls > 50)
                wait (40);
            else
                wait (5);

            continue;
        }

        emptyPolls = 0;

        const int take = (int) juce::jmin<uint64_t> (available, (uint64_t) scratch.size());
        const uint64_t start = r & ringMask;
        const int firstChunk = juce::jmin (take, ringCapacity - (int) start);

        std::memcpy (scratch.data(), ring.data() + start,
                     sizeof (float) * (size_t) firstChunk);

        if (take > firstChunk)
            std::memcpy (scratch.data() + firstChunk, ring.data(),
                         sizeof (float) * (size_t) (take - firstChunk));

        readIndex.store (r + (uint64_t) take, std::memory_order_release);

        // 环形缓冲曾经溢出 → 输入不连续，丢弃未凑满的帧。
        const uint32_t gap = gapEpoch.load (std::memory_order_relaxed);

        if (gap != consumedGapEpoch)
        {
            consumedGapEpoch = gap;
            tempo.dropPendingFrame();
            key.dropPendingFrame();
        }

        double sumSq = 0.0;

        for (int i = 0; i < take; ++i)
            sumSq += (double) scratch[(size_t) i] * (double) scratch[(size_t) i];

        const float db = (float) (10.0 * std::log10 (sumSq / (double) take + 1.0e-12));

        if (db < kSilenceDb)
        {
            silentSamples += (double) take;

            if (! silenceResetDone
                && appliedSampleRate > 0.0
                && silentSamples > appliedSampleRate * kAutoResetSeconds)
            {
                // 连续 3 秒没有任何信号：自动 reset（等价于用户按 RESET 按钮），
                // 同时清掉 onset / chroma 窗口，防止恢复播放后沿用旧片段。
                autoResetForSilence();
                silenceResetDone = true;
            }
        }
        else
        {
            silentSamples    = 0.0;
            silenceResetDone = false;
        }

        tempo.pushAudio (scratch.data(), take);
        key.pushAudio (scratch.data(), take);

        fedSamples += (double) take;

        publishResult (false);
    }
}

Result MusicAnalysisEngine::getResult() const noexcept
{
    const juce::SpinLock::ScopedLockType sl (resultLock);
    return published;
}

MusicAnalysisEngine::Diagnostics MusicAnalysisEngine::getDiagnostics() const noexcept
{
    Diagnostics d;
    d.sampleRate     = appliedSampleRate;
    d.droppedSamples = droppedSamples.load (std::memory_order_relaxed);
    d.gapEpoch       = gapEpoch.load (std::memory_order_relaxed);
    d.running        = isThreadRunning();

    const uint64_t w = writeIndex.load (std::memory_order_relaxed);
    const uint64_t r = readIndex.load (std::memory_order_relaxed);
    d.queuedSamples  = (w > r) ? (w - r) : 0;

    return d;
}

} // namespace y2k::music
