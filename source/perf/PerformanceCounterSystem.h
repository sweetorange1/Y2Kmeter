#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>

#ifndef Y2K_ENABLE_PERF_COUNTERS
#define Y2K_ENABLE_PERF_COUNTERS 1
#endif

namespace y2k::perf
{

enum class Partition : int
{
    audioAnalysis = 0,
    uiRendering,
    dataCommunication,
    memoryManagement,
    numPartitions
};

enum class ThreadRole : int
{
    unknown = 0,
    audio,
    ui,
    worker,
    numRoles
};

enum class FunctionId : int
{
    processBlockTotal = 0,
    analyserPushTotal,
    analyserOscilloscope,
    analyserSpectrum,
    analyserLoudness,
    analyserPhase,
    analyserDynamics,

    uiFrameDispatcher,
    uiFrameAssemble,
    uiOnFrameDispatch,
    uiModulePanelPaint,
    uiRepaintRequest,
    uiRepaintSkippedInvisible,
    uiFrameListenerFanout,

    dataPublishLatestFrame,
    dataGetLatestFrame,
    dataListenerListCopy,

    lockOsc,
    lockSpec,
    lockLatestFrame,
    lockFrameListeners,

    memoryFrameSnapshotAlloc,
    memoryFrameSnapshotRecycle,

    lowFreqAudioSourceSwitch,
    lowFreqThemeOrUiStateChange,

    numFunctions
};

class PerformanceCounterSystem
{
public:
    static PerformanceCounterSystem& instance();

    PerformanceCounterSystem(const PerformanceCounterSystem&) = delete;
    PerformanceCounterSystem& operator=(const PerformanceCounterSystem&) = delete;

    static juce::int64 nowNs() noexcept;

    void markCurrentThreadRole(ThreadRole role, const char* threadNameHint = nullptr) noexcept;

    void recordDuration(FunctionId fn,
                        Partition partition,
                        ThreadRole role,
                        juce::int64 durationNs,
                        juce::int64 lockWaitNs = 0) noexcept;

    void recordEvent(FunctionId fn,
                     Partition partition,
                     ThreadRole role,
                     juce::int64 amount = 1) noexcept;

    void recordMemoryAlloc(std::size_t bytes, FunctionId fn = FunctionId::memoryFrameSnapshotAlloc) noexcept;
    void recordMemoryFree (std::size_t bytes, FunctionId fn = FunctionId::memoryFrameSnapshotRecycle) noexcept;

    void recordUiModulePaint(int moduleTypeId, juce::int64 durationNs) noexcept;
    void recordUiRepaintRequest(int moduleTypeId) noexcept;
    void recordUiRepaintSkippedInvisible(int moduleTypeId) noexcept;
    void recordUiRepaintCoalesced(int moduleTypeId) noexcept;
    void recordUiRepaintDroppedOffscreen(int moduleTypeId) noexcept;
    void recordUiDirtyAreaSample(int moduleTypeId, double ratio01) noexcept;
    void recordFrameListenerCount(int listenerCount) noexcept;

    juce::File exportNow();
    void setAutoExportEnabled(bool enabled) noexcept;
    bool isAutoExportEnabled() const noexcept { return autoExportEnabled.load(std::memory_order_relaxed); }

private:
    PerformanceCounterSystem();
    ~PerformanceCounterSystem();

    struct AtomicFnStat
    {
        std::atomic<juce::uint64> callCount     { 0 };
        std::atomic<juce::uint64> totalNs       { 0 };
        std::atomic<juce::uint64> maxNs         { 0 };
        std::atomic<juce::uint64> totalLockWait { 0 };
        std::atomic<juce::uint64> maxLockWait   { 0 };
    };

    struct AtomicPartitionStat
    {
        std::atomic<juce::uint64> totalNs { 0 };
    };

    struct AtomicThreadStat
    {
        std::atomic<juce::uint64> totalNs { 0 };
        std::atomic<juce::uint64> calls   { 0 };
    };

    struct AtomicMemStat
    {
        std::atomic<juce::uint64> allocCount { 0 };
        std::atomic<juce::uint64> freeCount  { 0 };
        std::atomic<juce::uint64> allocBytes { 0 };
        std::atomic<juce::uint64> freeBytes  { 0 };
    };

    struct AtomicUiModulePaintStat
    {
        std::atomic<juce::uint64> callCount    { 0 };
        std::atomic<juce::uint64> totalNs      { 0 };
        std::atomic<juce::uint64> maxNs        { 0 };
        std::atomic<juce::uint64> repaintCount { 0 };
        std::atomic<juce::uint64> repaintSkippedInvisible { 0 };
        std::atomic<juce::uint64> repaintCoalesced { 0 };
        std::atomic<juce::uint64> repaintDroppedOffscreen { 0 };
        std::atomic<juce::uint64> paintOver8msCount { 0 };
        std::atomic<juce::uint64> paintOver16msCount { 0 };
        std::atomic<juce::uint64> dirtyAreaSampleCount { 0 };
        std::atomic<double>       dirtyAreaRatioSum { 0.0 };
    };

    struct AtomicFrameListenerDistStat
    {
        std::atomic<juce::uint64> sampleCount { 0 };
        std::atomic<juce::uint64> totalCount  { 0 };
        std::atomic<juce::uint64> minCount    { (juce::uint64) std::numeric_limits<juce::uint32>::max() };
        std::atomic<juce::uint64> maxCount    { 0 };
        std::array<std::atomic<juce::uint64>, 129> histogram {};
    };

    std::array<AtomicFnStat, (size_t) FunctionId::numFunctions> fnStats;
    std::array<AtomicPartitionStat, (size_t) Partition::numPartitions> partitionStats;
    std::array<AtomicThreadStat, (size_t) ThreadRole::numRoles> threadStats;
    AtomicMemStat memStats;
    std::array<AtomicUiModulePaintStat, 64> uiModulePaintStats;
    AtomicFrameListenerDistStat frameListenerDistStats;

    std::atomic<bool> autoExportEnabled { false };
    std::atomic<bool> stopWorker        { false };
    std::thread workerThread;

    std::atomic<juce::int64> windowStartNs { 0 };
    static constexpr juce::int64 autoExportWindowNs = 60LL * 1000LL * 1000LL * 1000LL;

    juce::File doExportSnapshot(bool resetAfterExport);
    juce::String buildJson(juce::int64 nowNsValue, double windowSeconds) const;
    juce::String buildCsv (juce::int64 nowNsValue, double windowSeconds) const;

    void maybeStartWorker();
    void workerLoop();
    void resetWindow(juce::int64 newWindowStartNs);

    static const char* functionName(FunctionId fn) noexcept;
    static const char* partitionName(Partition p) noexcept;
    static const char* threadRoleName(ThreadRole r) noexcept;
};

class ScopedPerfTimer
{
public:
    ScopedPerfTimer(FunctionId fn,
                    Partition partition,
                    ThreadRole role) noexcept
        : function(fn),
          part(partition),
          threadRole(role)
    {
#if Y2K_ENABLE_PERF_COUNTERS
        startNs = PerformanceCounterSystem::nowNs();
#endif
    }

    ~ScopedPerfTimer()
    {
#if Y2K_ENABLE_PERF_COUNTERS
        const auto endNs = PerformanceCounterSystem::nowNs();
        PerformanceCounterSystem::instance().recordDuration(function, part, threadRole, endNs - startNs, lockWaitNs);
#endif
    }

    void addLockWait(juce::int64 ns) noexcept
    {
#if Y2K_ENABLE_PERF_COUNTERS
        lockWaitNs += ns;
#else
        juce::ignoreUnused(ns);
#endif
    }

private:
    FunctionId function;
    Partition  part;
    ThreadRole threadRole;
    juce::int64 startNs = 0;
    juce::int64 lockWaitNs = 0;
};

class ScopedLockWaitMeasure
{
public:
    ScopedLockWaitMeasure(FunctionId fn,
                          Partition partition,
                          ThreadRole role) noexcept
        : function(fn),
          part(partition),
          threadRole(role)
    {
#if Y2K_ENABLE_PERF_COUNTERS
        waitStart = PerformanceCounterSystem::nowNs();
#endif
    }

    template <typename LockType>
    void lock(LockType& lockObj) noexcept
    {
        lockObj.enter();
#if Y2K_ENABLE_PERF_COUNTERS
        lockAcquired = PerformanceCounterSystem::nowNs();
        lockAcquiredValid = true;
#endif
    }

    ~ScopedLockWaitMeasure()
    {
#if Y2K_ENABLE_PERF_COUNTERS
        if (! lockAcquiredValid)
            return;

        const auto unlockNs = PerformanceCounterSystem::nowNs();
        const auto waitNs = juce::jmax<juce::int64>(0, lockAcquired - waitStart);
        const auto holdNs = juce::jmax<juce::int64>(0, unlockNs - lockAcquired);
        PerformanceCounterSystem::instance().recordDuration(function, part, threadRole, holdNs, waitNs);
#endif
    }

    void markAcquired() noexcept
    {
#if Y2K_ENABLE_PERF_COUNTERS
        lockAcquiredValid = true;
#endif
    }

    void setLockAcquiredNow() noexcept
    {
#if Y2K_ENABLE_PERF_COUNTERS
        lockAcquired = PerformanceCounterSystem::nowNs();
        lockAcquiredValid = true;
#endif
    }

private:
    FunctionId function;
    Partition  part;
    ThreadRole threadRole;
    juce::int64 waitStart = 0;
    juce::int64 lockAcquired = 0;
    bool lockAcquiredValid = false;
};

} // namespace y2k::perf