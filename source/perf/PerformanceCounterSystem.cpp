#include "source/perf/PerformanceCounterSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

namespace y2k::perf
{

namespace
{
thread_local ThreadRole gThreadRole = ThreadRole::unknown;
thread_local bool gThreadRoleMarked = false;

struct FnSnapshot
{
    FunctionId fn = FunctionId::processBlockTotal;
    juce::uint64 callCount = 0;
    juce::uint64 totalNs = 0;
    juce::uint64 maxNs = 0;
    juce::uint64 totalLockWait = 0;
    juce::uint64 maxLockWait = 0;
};

std::mutex& exportMutex()
{
    static std::mutex m;
    return m;
}

juce::String escapeJson(const juce::String& s)
{
    juce::String out;
    out.preallocateBytes(s.getNumBytesAsUTF8() + 16);

    for (auto c : s)
    {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += juce::String::charToString(c);
    }

    return out;
}

juce::String percentStr(double x)
{
    return juce::String(x, 4);
}

juce::String moduleTypeNameById(int id)
{
    switch (id)
    {
        case 0: return "eq";
        case 1: return "loudness";
        case 2: return "oscilloscope";
        case 3: return "spectrum";
        case 4: return "phase";
        case 5: return "dynamics";
        case 6: return "lufsRealtime";
        case 7: return "truePeak";
        case 8: return "oscilloscopeLeft";
        case 9: return "oscilloscopeRight";
        case 10: return "phaseCorrelation";
        case 11: return "phaseBalance";
        case 12: return "dynamicsMeters";
        case 13: return "dynamicsDr";
        case 14: return "dynamicsCrest";
        case 15: return "waveform";
        case 16: return "vuMeter";
        case 17: return "spectrogram";
        case 18: return "tamagotchi";
        default: return juce::String("unknown(") + juce::String(id) + ")";
    }
}

} // namespace

PerformanceCounterSystem& PerformanceCounterSystem::instance()
{
    static PerformanceCounterSystem s;
    return s;
}

juce::int64 PerformanceCounterSystem::nowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

PerformanceCounterSystem::PerformanceCounterSystem()
{
    windowStartNs.store(nowNs(), std::memory_order_relaxed);
    maybeStartWorker();
}

PerformanceCounterSystem::~PerformanceCounterSystem()
{
    stopWorker.store(true, std::memory_order_relaxed);
    if (workerThread.joinable())
        workerThread.join();
}

void PerformanceCounterSystem::markCurrentThreadRole(ThreadRole role, const char* threadNameHint) noexcept
{
    juce::ignoreUnused(threadNameHint);
    gThreadRole = role;
    gThreadRoleMarked = true;
}

void PerformanceCounterSystem::recordDuration(FunctionId fn,
                                              Partition partition,
                                              ThreadRole role,
                                              juce::int64 durationNs,
                                              juce::int64 lockWaitNs) noexcept
{
    if (durationNs < 0) durationNs = 0;
    if (lockWaitNs < 0) lockWaitNs = 0;

    const auto fi = (size_t) fn;
    const auto pi = (size_t) partition;

    auto& f = fnStats[fi];
    f.callCount.fetch_add(1, std::memory_order_relaxed);
    const auto d = (juce::uint64) durationNs;
    const auto w = (juce::uint64) lockWaitNs;
    f.totalNs.fetch_add(d, std::memory_order_relaxed);
    f.totalLockWait.fetch_add(w, std::memory_order_relaxed);

    auto curMax = f.maxNs.load(std::memory_order_relaxed);
    while (d > curMax && ! f.maxNs.compare_exchange_weak(curMax, d,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {}

    auto curWaitMax = f.maxLockWait.load(std::memory_order_relaxed);
    while (w > curWaitMax && ! f.maxLockWait.compare_exchange_weak(curWaitMax, w,
                                                                   std::memory_order_relaxed,
                                                                   std::memory_order_relaxed)) {}

    partitionStats[pi].totalNs.fetch_add(d, std::memory_order_relaxed);

    ThreadRole effectiveRole = role;
    if (effectiveRole == ThreadRole::unknown && gThreadRoleMarked)
        effectiveRole = gThreadRole;

    const auto ri = (size_t) effectiveRole;
    threadStats[ri].totalNs.fetch_add(d, std::memory_order_relaxed);
    threadStats[ri].calls.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceCounterSystem::recordEvent(FunctionId fn,
                                           Partition partition,
                                           ThreadRole role,
                                           juce::int64 amount) noexcept
{
    if (amount <= 0)
        return;

    const auto fi = (size_t) fn;
    fnStats[fi].callCount.fetch_add((juce::uint64) amount, std::memory_order_relaxed);

    ThreadRole effectiveRole = role;
    if (effectiveRole == ThreadRole::unknown && gThreadRoleMarked)
        effectiveRole = gThreadRole;

    const auto ri = (size_t) effectiveRole;
    threadStats[ri].calls.fetch_add((juce::uint64) amount, std::memory_order_relaxed);

    juce::ignoreUnused(partition);
}

void PerformanceCounterSystem::recordMemoryAlloc(std::size_t bytes, FunctionId fn) noexcept
{
    memStats.allocCount.fetch_add(1, std::memory_order_relaxed);
    memStats.allocBytes.fetch_add((juce::uint64) bytes, std::memory_order_relaxed);
    recordEvent(fn, Partition::memoryManagement, ThreadRole::unknown, 1);
}

void PerformanceCounterSystem::recordMemoryFree(std::size_t bytes, FunctionId fn) noexcept
{
    memStats.freeCount.fetch_add(1, std::memory_order_relaxed);
    memStats.freeBytes.fetch_add((juce::uint64) bytes, std::memory_order_relaxed);
    recordEvent(fn, Partition::memoryManagement, ThreadRole::unknown, 1);
}

juce::File PerformanceCounterSystem::exportNow()
{
    return doExportSnapshot(true);
}

void PerformanceCounterSystem::setAutoExportEnabled(bool enabled) noexcept
{
    autoExportEnabled.store(enabled, std::memory_order_relaxed);
}

juce::File PerformanceCounterSystem::doExportSnapshot(bool resetAfterExport)
{
    const std::lock_guard<std::mutex> lk(exportMutex());

    const auto now = nowNs();
    const auto start = windowStartNs.load(std::memory_order_relaxed);
    const auto elapsedNs = juce::jmax<juce::int64>(1, now - start);
    const double windowSec = (double) elapsedNs / 1.0e9;

    const auto json = buildJson(now, windowSec);
    const auto csv  = buildCsv(now, windowSec);

    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Y2Kmeter")
                    .getChildFile("perf_counters");
    dir.createDirectory();

    const auto time = juce::Time::getCurrentTime();
    const auto stamp = time.formatted("%Y%m%d_%H%M%S");

    const auto jsonFile = dir.getChildFile("perf_" + stamp + ".json");
    const auto csvFile  = dir.getChildFile("perf_" + stamp + ".csv");

    jsonFile.replaceWithText(json, false, false, "UTF-8");
    csvFile.replaceWithText(csv, false, false, "UTF-8");

    if (resetAfterExport)
        resetWindow(now);

    return jsonFile;
}

juce::String PerformanceCounterSystem::buildJson(juce::int64 nowNsValue, double windowSeconds) const
{
    juce::ignoreUnused(nowNsValue);

    std::vector<FnSnapshot> list;
    list.reserve((size_t) FunctionId::numFunctions);

    juce::uint64 totalCpuNs = 0;
    for (size_t i = 0; i < (size_t) ThreadRole::numRoles; ++i)
        totalCpuNs += threadStats[i].totalNs.load(std::memory_order_relaxed);

    for (size_t i = 0; i < (size_t) FunctionId::numFunctions; ++i)
    {
        FnSnapshot s;
        s.fn = (FunctionId) i;
        s.callCount = fnStats[i].callCount.load(std::memory_order_relaxed);
        s.totalNs   = fnStats[i].totalNs.load(std::memory_order_relaxed);
        s.maxNs     = fnStats[i].maxNs.load(std::memory_order_relaxed);
        s.totalLockWait = fnStats[i].totalLockWait.load(std::memory_order_relaxed);
        s.maxLockWait   = fnStats[i].maxLockWait.load(std::memory_order_relaxed);
        list.push_back(s);
    }

    std::sort(list.begin(), list.end(), [] (const FnSnapshot& a, const FnSnapshot& b)
    {
        if (a.totalNs != b.totalNs) return a.totalNs > b.totalNs;
        return a.callCount > b.callCount;
    });

    juce::String out;
    out << "{\n";
    out << "  \"timestamp\": \"" << escapeJson(juce::Time::getCurrentTime().toISO8601(true)) << "\",\n";
    out << "  \"windowSeconds\": " << juce::String(windowSeconds, 6) << ",\n";

    out << "  \"partitionCpuShare\": [\n";
    for (size_t i = 0; i < (size_t) Partition::numPartitions; ++i)
    {
        const auto ns = partitionStats[i].totalNs.load(std::memory_order_relaxed);
        const double share = (totalCpuNs > 0) ? (double) ns / (double) totalCpuNs : 0.0;
        out << "    {\"partition\": \"" << partitionName((Partition) i)
            << "\", \"totalNs\": " << juce::String((juce::int64) ns)
            << ", \"cpuShare\": " << percentStr(share) << "}";
        out << (i + 1 < (size_t) Partition::numPartitions ? ",\n" : "\n");
    }
    out << "  ],\n";

    out << "  \"threadCpuShare\": [\n";
    for (size_t i = 0; i < (size_t) ThreadRole::numRoles; ++i)
    {
        const auto ns = threadStats[i].totalNs.load(std::memory_order_relaxed);
        const auto calls = threadStats[i].calls.load(std::memory_order_relaxed);
        const double share = (totalCpuNs > 0) ? (double) ns / (double) totalCpuNs : 0.0;
        out << "    {\"threadRole\": \"" << threadRoleName((ThreadRole) i)
            << "\", \"totalNs\": " << juce::String((juce::int64) ns)
            << ", \"calls\": " << juce::String((juce::int64) calls)
            << ", \"cpuShare\": " << percentStr(share) << "}";
        out << (i + 1 < (size_t) ThreadRole::numRoles ? ",\n" : "\n");
    }
    out << "  ],\n";

    out << "  \"hotFunctions\": [\n";
    bool firstFn = true;
    for (const auto& s : list)
    {
        if (s.callCount == 0)
            continue;

        if (! firstFn) out << ",\n";
        firstFn = false;

        const double avgNs = (s.callCount > 0) ? (double) s.totalNs / (double) s.callCount : 0.0;
        const double hz = (windowSeconds > 1.0e-6) ? (double) s.callCount / windowSeconds : 0.0;
        const bool highFreq = hz >= 10.0;

        out << "    {\"function\": \"" << functionName(s.fn)
            << "\", \"calls\": " << juce::String((juce::int64) s.callCount)
            << ", \"totalNs\": " << juce::String((juce::int64) s.totalNs)
            << ", \"avgNs\": " << juce::String(avgNs, 3)
            << ", \"maxNs\": " << juce::String((juce::int64) s.maxNs)
            << ", \"callHz\": " << juce::String(hz, 3)
            << ", \"frequencyClass\": \"" << (highFreq ? "high" : "low")
            << "\", \"lockWaitTotalNs\": " << juce::String((juce::int64) s.totalLockWait)
            << ", \"lockWaitMaxNs\": " << juce::String((juce::int64) s.maxLockWait)
            << "}";
    }
    out << "\n  ],\n";

    out << "  \"uiModulePaint\": [\n";
    for (int i = 0; i < (int) uiModulePaintStats.size(); ++i)
    {
        const auto calls = uiModulePaintStats[(size_t) i].callCount.load(std::memory_order_relaxed);
        const auto total = uiModulePaintStats[(size_t) i].totalNs.load(std::memory_order_relaxed);
        const auto maxv  = uiModulePaintStats[(size_t) i].maxNs.load(std::memory_order_relaxed);
        const auto rep   = uiModulePaintStats[(size_t) i].repaintCount.load(std::memory_order_relaxed);
        const auto repSkipInvisible = uiModulePaintStats[(size_t) i].repaintSkippedInvisible.load(std::memory_order_relaxed);
        const auto repCoalesced = uiModulePaintStats[(size_t) i].repaintCoalesced.load(std::memory_order_relaxed);
        const auto repDropped = uiModulePaintStats[(size_t) i].repaintDroppedOffscreen.load(std::memory_order_relaxed);
        const auto over8 = uiModulePaintStats[(size_t) i].paintOver8msCount.load(std::memory_order_relaxed);
        const auto over16 = uiModulePaintStats[(size_t) i].paintOver16msCount.load(std::memory_order_relaxed);
        const auto dirtySamples = uiModulePaintStats[(size_t) i].dirtyAreaSampleCount.load(std::memory_order_relaxed);
        const auto dirtyRatioSum = uiModulePaintStats[(size_t) i].dirtyAreaRatioSum.load(std::memory_order_relaxed);
        if (calls == 0 && rep == 0 && repSkipInvisible == 0 && repCoalesced == 0 && repDropped == 0)
            continue;

        const double avgNs = (calls > 0) ? (double) total / (double) calls : 0.0;
        const double dirtyAvg = (dirtySamples > 0) ? (dirtyRatioSum / (double) dirtySamples) : 0.0;
        out << "    {\"moduleTypeId\": " << juce::String(i)
            << ", \"module\": \"" << escapeJson(moduleTypeNameById(i)) << "\""
            << ", \"paintCalls\": " << juce::String((juce::int64) calls)
            << ", \"paintTotalNs\": " << juce::String((juce::int64) total)
            << ", \"paintAvgNs\": " << juce::String(avgNs, 3)
            << ", \"paintMaxNs\": " << juce::String((juce::int64) maxv)
            << ", \"paintOver8msCount\": " << juce::String((juce::int64) over8)
            << ", \"paintOver16msCount\": " << juce::String((juce::int64) over16)
            << ", \"repaintRequests\": " << juce::String((juce::int64) rep)
            << ", \"repaintSkippedInvisible\": " << juce::String((juce::int64) repSkipInvisible)
            << ", \"repaintCoalescedCount\": " << juce::String((juce::int64) repCoalesced)
            << ", \"repaintDroppedOffscreenCount\": " << juce::String((juce::int64) repDropped)
            << ", \"dirtyAreaSampleCount\": " << juce::String((juce::int64) dirtySamples)
            << ", \"dirtyAreaRatioAvg\": " << juce::String(dirtyAvg, 6)
            << "},\n";
    }

    if (out.endsWith(",\n"))
        out = out.dropLastCharacters(2) + "\n";
    out << "  ],\n";

    const auto allocCount = memStats.allocCount.load(std::memory_order_relaxed);
    const auto freeCount  = memStats.freeCount .load(std::memory_order_relaxed);
    const auto allocBytes = memStats.allocBytes.load(std::memory_order_relaxed);
    const auto freeBytes  = memStats.freeBytes .load(std::memory_order_relaxed);

    out << "  \"memoryStats\": {"
        << "\"allocCount\": " << juce::String((juce::int64) allocCount)
        << ", \"freeCount\": " << juce::String((juce::int64) freeCount)
        << ", \"allocBytes\": " << juce::String((juce::int64) allocBytes)
        << ", \"freeBytes\": " << juce::String((juce::int64) freeBytes)
        << ", \"allocHz\": " << juce::String(windowSeconds > 1.0e-6 ? allocCount / windowSeconds : 0.0, 3)
        << ", \"freeHz\": " << juce::String(windowSeconds > 1.0e-6 ? freeCount / windowSeconds : 0.0, 3)
        << "},\n";

    juce::uint64 lockWaitTotal = 0;
    juce::uint64 lockWaitMax = 0;
    for (size_t i = 0; i < (size_t) FunctionId::numFunctions; ++i)
    {
        lockWaitTotal += fnStats[i].totalLockWait.load(std::memory_order_relaxed);
        lockWaitMax = juce::jmax(lockWaitMax, fnStats[i].maxLockWait.load(std::memory_order_relaxed));
    }

    const auto fanSamples = frameListenerDistStats.sampleCount.load(std::memory_order_relaxed);
    const auto fanTotal = frameListenerDistStats.totalCount.load(std::memory_order_relaxed);
    const auto fanMinRaw = frameListenerDistStats.minCount.load(std::memory_order_relaxed);
    const auto fanMax = frameListenerDistStats.maxCount.load(std::memory_order_relaxed);

    juce::uint64 fanMin = (fanSamples > 0) ? fanMinRaw : 0;
    if (fanMin == (juce::uint64) std::numeric_limits<juce::uint32>::max())
        fanMin = 0;

    juce::uint64 fanP95 = 0;
    if (fanSamples > 0)
    {
        const juce::uint64 target = (juce::uint64) std::ceil((double) fanSamples * 0.95);
        juce::uint64 acc = 0;
        for (size_t i = 0; i < frameListenerDistStats.histogram.size(); ++i)
        {
            acc += frameListenerDistStats.histogram[i].load(std::memory_order_relaxed);
            if (acc >= target)
            {
                fanP95 = (juce::uint64) i;
                break;
            }
        }
    }

    out << "  \"frameListenerCountDistribution\": {"
        << "\"sampleCount\": " << juce::String((juce::int64) fanSamples)
        << ", \"min\": " << juce::String((juce::int64) fanMin)
        << ", \"avg\": " << juce::String(fanSamples > 0 ? (double) fanTotal / (double) fanSamples : 0.0, 6)
        << ", \"p95\": " << juce::String((juce::int64) fanP95)
        << ", \"max\": " << juce::String((juce::int64) fanMax)
        << "},\n";

    out << "  \"lockContention\": {"
        << "\"totalWaitNs\": " << juce::String((juce::int64) lockWaitTotal)
        << ", \"maxSingleWaitNs\": " << juce::String((juce::int64) lockWaitMax)
        << "}\n";

    out << "}\n";
    return out;
}

juce::String PerformanceCounterSystem::buildCsv(juce::int64 nowNsValue, double windowSeconds) const
{
    juce::ignoreUnused(nowNsValue);

    juce::String out;
    out << "section,name,calls,total_ns,avg_ns,max_ns,call_hz,frequency_class,lock_wait_total_ns,lock_wait_max_ns\n";

    for (size_t i = 0; i < (size_t) FunctionId::numFunctions; ++i)
    {
        const auto calls = fnStats[i].callCount.load(std::memory_order_relaxed);
        const auto total = fnStats[i].totalNs.load(std::memory_order_relaxed);
        const auto maxv  = fnStats[i].maxNs.load(std::memory_order_relaxed);
        const auto lwt   = fnStats[i].totalLockWait.load(std::memory_order_relaxed);
        const auto lwm   = fnStats[i].maxLockWait.load(std::memory_order_relaxed);

        if (calls == 0)
            continue;

        const double avgNs = (calls > 0) ? (double) total / (double) calls : 0.0;
        const double hz    = (windowSeconds > 1.0e-6) ? (double) calls / windowSeconds : 0.0;
        const auto klass   = (hz >= 10.0 ? "high" : "low");

        out << "function," << functionName((FunctionId) i)
            << "," << juce::String((juce::int64) calls)
            << "," << juce::String((juce::int64) total)
            << "," << juce::String(avgNs, 3)
            << "," << juce::String((juce::int64) maxv)
            << "," << juce::String(hz, 3)
            << "," << klass
            << "," << juce::String((juce::int64) lwt)
            << "," << juce::String((juce::int64) lwm)
            << "\n";
    }

    out << "\nsection,name,total_ns,cpu_share\n";

    juce::uint64 totalCpuNs = 0;
    for (size_t i = 0; i < (size_t) ThreadRole::numRoles; ++i)
        totalCpuNs += threadStats[i].totalNs.load(std::memory_order_relaxed);

    for (size_t i = 0; i < (size_t) Partition::numPartitions; ++i)
    {
        const auto ns = partitionStats[i].totalNs.load(std::memory_order_relaxed);
        const double share = (totalCpuNs > 0) ? (double) ns / (double) totalCpuNs : 0.0;
        out << "partition," << partitionName((Partition) i)
            << "," << juce::String((juce::int64) ns)
            << "," << juce::String(share, 6)
            << "\n";
    }

    out << "\nsection,name,total_ns,calls,cpu_share\n";
    for (size_t i = 0; i < (size_t) ThreadRole::numRoles; ++i)
    {
        const auto ns = threadStats[i].totalNs.load(std::memory_order_relaxed);
        const auto calls = threadStats[i].calls.load(std::memory_order_relaxed);
        const double share = (totalCpuNs > 0) ? (double) ns / (double) totalCpuNs : 0.0;
        out << "thread," << threadRoleName((ThreadRole) i)
            << "," << juce::String((juce::int64) ns)
            << "," << juce::String((juce::int64) calls)
            << "," << juce::String(share, 6)
            << "\n";
    }

    out << "\nsection,name,alloc_count,free_count,alloc_bytes,free_bytes,alloc_hz,free_hz\n";
    const auto allocCount = memStats.allocCount.load(std::memory_order_relaxed);
    const auto freeCount  = memStats.freeCount .load(std::memory_order_relaxed);
    const auto allocBytes = memStats.allocBytes.load(std::memory_order_relaxed);
    const auto freeBytes  = memStats.freeBytes .load(std::memory_order_relaxed);
    const double allocHz = (windowSeconds > 1.0e-6) ? (double) allocCount / windowSeconds : 0.0;
    const double freeHz  = (windowSeconds > 1.0e-6) ? (double) freeCount  / windowSeconds : 0.0;

    out << "memory,global"
        << "," << juce::String((juce::int64) allocCount)
        << "," << juce::String((juce::int64) freeCount)
        << "," << juce::String((juce::int64) allocBytes)
        << "," << juce::String((juce::int64) freeBytes)
        << "," << juce::String(allocHz, 3)
        << "," << juce::String(freeHz, 3)
        << "\n";

    out << "\nsection,module_type_id,module,paint_calls,paint_total_ns,paint_avg_ns,paint_max_ns,paint_over_8ms_count,paint_over_16ms_count,repaint_requests,repaint_skipped_invisible,repaint_coalesced_count,repaint_dropped_offscreen_count,dirty_area_sample_count,dirty_area_ratio_avg\n";
    for (int i = 0; i < (int) uiModulePaintStats.size(); ++i)
    {
        const auto calls = uiModulePaintStats[(size_t) i].callCount.load(std::memory_order_relaxed);
        const auto total = uiModulePaintStats[(size_t) i].totalNs.load(std::memory_order_relaxed);
        const auto maxv  = uiModulePaintStats[(size_t) i].maxNs.load(std::memory_order_relaxed);
        const auto rep   = uiModulePaintStats[(size_t) i].repaintCount.load(std::memory_order_relaxed);
        const auto repSkipInvisible = uiModulePaintStats[(size_t) i].repaintSkippedInvisible.load(std::memory_order_relaxed);
        const auto repCoalesced = uiModulePaintStats[(size_t) i].repaintCoalesced.load(std::memory_order_relaxed);
        const auto repDropped = uiModulePaintStats[(size_t) i].repaintDroppedOffscreen.load(std::memory_order_relaxed);
        const auto over8 = uiModulePaintStats[(size_t) i].paintOver8msCount.load(std::memory_order_relaxed);
        const auto over16 = uiModulePaintStats[(size_t) i].paintOver16msCount.load(std::memory_order_relaxed);
        const auto dirtySamples = uiModulePaintStats[(size_t) i].dirtyAreaSampleCount.load(std::memory_order_relaxed);
        const auto dirtyRatioSum = uiModulePaintStats[(size_t) i].dirtyAreaRatioSum.load(std::memory_order_relaxed);
        if (calls == 0 && rep == 0 && repSkipInvisible == 0 && repCoalesced == 0 && repDropped == 0)
            continue;

        const double avgNs = (calls > 0) ? (double) total / (double) calls : 0.0;
        const double dirtyAvg = (dirtySamples > 0) ? (dirtyRatioSum / (double) dirtySamples) : 0.0;

        out << "ui_module," << juce::String(i)
            << "," << moduleTypeNameById(i)
            << "," << juce::String((juce::int64) calls)
            << "," << juce::String((juce::int64) total)
            << "," << juce::String(avgNs, 3)
            << "," << juce::String((juce::int64) maxv)
            << "," << juce::String((juce::int64) over8)
            << "," << juce::String((juce::int64) over16)
            << "," << juce::String((juce::int64) rep)
            << "," << juce::String((juce::int64) repSkipInvisible)
            << "," << juce::String((juce::int64) repCoalesced)
            << "," << juce::String((juce::int64) repDropped)
            << "," << juce::String((juce::int64) dirtySamples)
            << "," << juce::String(dirtyAvg, 6)
            << "\n";
    }

    out << "\nsection,name,sample_count,min,avg,p95,max\n";
    {
        const auto fanSamples = frameListenerDistStats.sampleCount.load(std::memory_order_relaxed);
        const auto fanTotal = frameListenerDistStats.totalCount.load(std::memory_order_relaxed);
        const auto fanMinRaw = frameListenerDistStats.minCount.load(std::memory_order_relaxed);
        const auto fanMax = frameListenerDistStats.maxCount.load(std::memory_order_relaxed);

        juce::uint64 fanMin = (fanSamples > 0) ? fanMinRaw : 0;
        if (fanMin == (juce::uint64) std::numeric_limits<juce::uint32>::max())
            fanMin = 0;

        juce::uint64 fanP95 = 0;
        if (fanSamples > 0)
        {
            const juce::uint64 target = (juce::uint64) std::ceil((double) fanSamples * 0.95);
            juce::uint64 acc = 0;
            for (size_t i = 0; i < frameListenerDistStats.histogram.size(); ++i)
            {
                acc += frameListenerDistStats.histogram[i].load(std::memory_order_relaxed);
                if (acc >= target)
                {
                    fanP95 = (juce::uint64) i;
                    break;
                }
            }
        }

        out << "frame_listener_dist,frameListenerCount"
            << "," << juce::String((juce::int64) fanSamples)
            << "," << juce::String((juce::int64) fanMin)
            << "," << juce::String(fanSamples > 0 ? (double) fanTotal / (double) fanSamples : 0.0, 6)
            << "," << juce::String((juce::int64) fanP95)
            << "," << juce::String((juce::int64) fanMax)
            << "\n";
    }

    return out;
}

void PerformanceCounterSystem::maybeStartWorker()
{
    workerThread = std::thread([this]() { workerLoop(); });
}

void PerformanceCounterSystem::workerLoop()
{
    while (! stopWorker.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (! autoExportEnabled.load(std::memory_order_relaxed))
            continue;

        const auto now = nowNs();
        const auto start = windowStartNs.load(std::memory_order_relaxed);
        if (now - start >= autoExportWindowNs)
            doExportSnapshot(true);
    }
}

void PerformanceCounterSystem::resetWindow(juce::int64 newWindowStartNs)
{
    for (auto& f : fnStats)
    {
        f.callCount.store(0, std::memory_order_relaxed);
        f.totalNs.store(0, std::memory_order_relaxed);
        f.maxNs.store(0, std::memory_order_relaxed);
        f.totalLockWait.store(0, std::memory_order_relaxed);
        f.maxLockWait.store(0, std::memory_order_relaxed);
    }

    for (auto& p : partitionStats)
        p.totalNs.store(0, std::memory_order_relaxed);

    for (auto& t : threadStats)
    {
        t.totalNs.store(0, std::memory_order_relaxed);
        t.calls.store(0, std::memory_order_relaxed);
    }

    for (auto& m : uiModulePaintStats)
    {
        m.callCount.store(0, std::memory_order_relaxed);
        m.totalNs.store(0, std::memory_order_relaxed);
        m.maxNs.store(0, std::memory_order_relaxed);
        m.repaintCount.store(0, std::memory_order_relaxed);
        m.repaintSkippedInvisible.store(0, std::memory_order_relaxed);
        m.repaintCoalesced.store(0, std::memory_order_relaxed);
        m.repaintDroppedOffscreen.store(0, std::memory_order_relaxed);
        m.paintOver8msCount.store(0, std::memory_order_relaxed);
        m.paintOver16msCount.store(0, std::memory_order_relaxed);
        m.dirtyAreaSampleCount.store(0, std::memory_order_relaxed);
        m.dirtyAreaRatioSum.store(0.0, std::memory_order_relaxed);
    }

    frameListenerDistStats.sampleCount.store(0, std::memory_order_relaxed);
    frameListenerDistStats.totalCount.store(0, std::memory_order_relaxed);
    frameListenerDistStats.minCount.store((juce::uint64) std::numeric_limits<juce::uint32>::max(), std::memory_order_relaxed);
    frameListenerDistStats.maxCount.store(0, std::memory_order_relaxed);
    for (auto& h : frameListenerDistStats.histogram)
        h.store(0, std::memory_order_relaxed);

    memStats.allocCount.store(0, std::memory_order_relaxed);
    memStats.freeCount .store(0, std::memory_order_relaxed);
    memStats.allocBytes.store(0, std::memory_order_relaxed);
    memStats.freeBytes .store(0, std::memory_order_relaxed);

    windowStartNs.store(newWindowStartNs, std::memory_order_relaxed);
}

const char* PerformanceCounterSystem::functionName(FunctionId fn) noexcept
{
    switch (fn)
    {
        case FunctionId::processBlockTotal: return "processBlockTotal";
        case FunctionId::analyserPushTotal: return "analyserPushTotal";
        case FunctionId::analyserOscilloscope: return "analyserOscilloscope";
        case FunctionId::analyserSpectrum: return "analyserSpectrum";
        case FunctionId::analyserLoudness: return "analyserLoudness";
        case FunctionId::analyserPhase: return "analyserPhase";
        case FunctionId::analyserDynamics: return "analyserDynamics";

        case FunctionId::uiFrameDispatcher: return "uiFrameDispatcher";
        case FunctionId::uiFrameAssemble: return "uiFrameAssemble";
        case FunctionId::uiOnFrameDispatch: return "uiOnFrameDispatch";
        case FunctionId::uiModulePanelPaint: return "uiModulePanelPaint";
        case FunctionId::uiRepaintRequest: return "uiRepaintRequest";
        case FunctionId::uiRepaintSkippedInvisible: return "uiRepaintSkippedInvisible";
        case FunctionId::uiFrameListenerFanout: return "uiFrameListenerFanout";

        case FunctionId::dataPublishLatestFrame: return "dataPublishLatestFrame";
        case FunctionId::dataGetLatestFrame: return "dataGetLatestFrame";
        case FunctionId::dataListenerListCopy: return "dataListenerListCopy";

        case FunctionId::lockOsc: return "lockOsc";
        case FunctionId::lockSpec: return "lockSpec";
        case FunctionId::lockLatestFrame: return "lockLatestFrame";
        case FunctionId::lockFrameListeners: return "lockFrameListeners";

        case FunctionId::memoryFrameSnapshotAlloc: return "memoryFrameSnapshotAlloc";
        case FunctionId::memoryFrameSnapshotRecycle: return "memoryFrameSnapshotRecycle";

        case FunctionId::lowFreqAudioSourceSwitch: return "lowFreqAudioSourceSwitch";
        case FunctionId::lowFreqThemeOrUiStateChange: return "lowFreqThemeOrUiStateChange";

        case FunctionId::numFunctions: break;
    }
    return "unknown";
}

const char* PerformanceCounterSystem::partitionName(Partition p) noexcept
{
    switch (p)
    {
        case Partition::audioAnalysis: return "audioAnalysis";
        case Partition::uiRendering: return "uiRendering";
        case Partition::dataCommunication: return "dataCommunication";
        case Partition::memoryManagement: return "memoryManagement";
        case Partition::numPartitions: break;
    }
    return "unknown";
}

const char* PerformanceCounterSystem::threadRoleName(ThreadRole r) noexcept
{
    switch (r)
    {
        case ThreadRole::unknown: return "unknown";
        case ThreadRole::audio: return "audio";
        case ThreadRole::ui: return "ui";
        case ThreadRole::worker: return "worker";
        case ThreadRole::numRoles: break;
    }
    return "unknown";
}

void PerformanceCounterSystem::recordUiModulePaint(int moduleTypeId, juce::int64 durationNs) noexcept
{
    if (durationNs < 0) durationNs = 0;
    if (moduleTypeId < 0 || moduleTypeId >= (int) uiModulePaintStats.size())
        moduleTypeId = 63;

    auto& s = uiModulePaintStats[(size_t) moduleTypeId];
    s.callCount.fetch_add(1, std::memory_order_relaxed);
    const auto d = (juce::uint64) durationNs;
    s.totalNs.fetch_add(d, std::memory_order_relaxed);

    if (d >= 8ULL * 1000ULL * 1000ULL)
        s.paintOver8msCount.fetch_add(1, std::memory_order_relaxed);
    if (d >= 16ULL * 1000ULL * 1000ULL)
        s.paintOver16msCount.fetch_add(1, std::memory_order_relaxed);

    auto curMax = s.maxNs.load(std::memory_order_relaxed);
    while (d > curMax && ! s.maxNs.compare_exchange_weak(curMax, d,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {}
}

void PerformanceCounterSystem::recordUiRepaintRequest(int moduleTypeId) noexcept
{
    if (moduleTypeId < 0 || moduleTypeId >= (int) uiModulePaintStats.size())
        moduleTypeId = 63;

    uiModulePaintStats[(size_t) moduleTypeId].repaintCount.fetch_add(1, std::memory_order_relaxed);
    recordEvent(FunctionId::uiRepaintRequest, Partition::uiRendering, ThreadRole::ui, 1);
}

void PerformanceCounterSystem::recordUiRepaintSkippedInvisible(int moduleTypeId) noexcept
{
    if (moduleTypeId < 0 || moduleTypeId >= (int) uiModulePaintStats.size())
        moduleTypeId = 63;

    uiModulePaintStats[(size_t) moduleTypeId].repaintSkippedInvisible.fetch_add(1, std::memory_order_relaxed);
    recordEvent(FunctionId::uiRepaintSkippedInvisible, Partition::uiRendering, ThreadRole::ui, 1);
}

void PerformanceCounterSystem::recordUiRepaintCoalesced(int moduleTypeId) noexcept
{
    if (moduleTypeId < 0 || moduleTypeId >= (int) uiModulePaintStats.size())
        moduleTypeId = 63;

    uiModulePaintStats[(size_t) moduleTypeId].repaintCoalesced.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceCounterSystem::recordUiRepaintDroppedOffscreen(int moduleTypeId) noexcept
{
    if (moduleTypeId < 0 || moduleTypeId >= (int) uiModulePaintStats.size())
        moduleTypeId = 63;

    uiModulePaintStats[(size_t) moduleTypeId].repaintDroppedOffscreen.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceCounterSystem::recordUiDirtyAreaSample(int moduleTypeId, double ratio01) noexcept
{
    if (moduleTypeId < 0 || moduleTypeId >= (int) uiModulePaintStats.size())
        moduleTypeId = 63;

    if (! std::isfinite(ratio01))
        return;

    ratio01 = juce::jlimit(0.0, 1.0, ratio01);
    auto& s = uiModulePaintStats[(size_t) moduleTypeId];
    s.dirtyAreaSampleCount.fetch_add(1, std::memory_order_relaxed);

    auto cur = s.dirtyAreaRatioSum.load(std::memory_order_relaxed);
    while (! s.dirtyAreaRatioSum.compare_exchange_weak(cur, cur + ratio01,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {}
}

void PerformanceCounterSystem::recordFrameListenerCount(int listenerCount) noexcept
{
    if (listenerCount < 0)
        listenerCount = 0;

    auto& s = frameListenerDistStats;
    const auto lc = (juce::uint64) listenerCount;
    s.sampleCount.fetch_add(1, std::memory_order_relaxed);
    s.totalCount.fetch_add(lc, std::memory_order_relaxed);

    auto curMin = s.minCount.load(std::memory_order_relaxed);
    while (lc < curMin && ! s.minCount.compare_exchange_weak(curMin, lc,
                                                             std::memory_order_relaxed,
                                                             std::memory_order_relaxed)) {}

    auto curMax = s.maxCount.load(std::memory_order_relaxed);
    while (lc > curMax && ! s.maxCount.compare_exchange_weak(curMax, lc,
                                                             std::memory_order_relaxed,
                                                             std::memory_order_relaxed)) {}

    const int bucket = juce::jlimit(0, 128, listenerCount);
    s.histogram[(size_t) bucket].fetch_add(1, std::memory_order_relaxed);
}

} // namespace y2k::perf