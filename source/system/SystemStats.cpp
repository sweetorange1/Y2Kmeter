#include "source/system/SystemStats.h"

// ==========================================================
// SystemStats.cpp —— 采集线程 / 历史环形缓冲 / 格式化辅助
//   平台相关的查询全部在 SystemStatsWin.cpp / SystemStatsMac.cpp。
// ==========================================================

namespace y2k::sys
{
    float Snapshot::metricValue (Metric m) const noexcept
    {
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();

        auto pct = [] (double used, double total) -> float
        {
            if (! isValid (used) || ! (total > 0.0)) return std::numeric_limits<float>::quiet_NaN();
            return (float) juce::jlimit (0.0, 100.0, used / total * 100.0);
        };

        switch (m)
        {
            case Metric::cpu:     return isValid (cpuLoad)   ? cpuLoad  : nan;
            case Metric::cpuTemp: return hasTemp (cpuTempC)  ? cpuTempC : nan;
            case Metric::gpu:     return isValid (gpuLoad)   ? gpuLoad  : nan;
            case Metric::gpuTemp: return hasTemp (gpuTempC)  ? gpuTempC : nan;
            case Metric::ram:     return pct (ramUsed, ramTotal);
            case Metric::vram:    return pct (vramUsed, vramTotal);
            case Metric::disk:    return isValid (diskActive) ? diskActive : nan;
            case Metric::netDown: return isValid (netDownBps) ? (float) netDownBps : nan;
            case Metric::netUp:   return isValid (netUpBps)   ? (float) netUpBps   : nan;
            case Metric::battery: return (hasBattery && isValid (batteryPct)) ? batteryPct : nan;
            case Metric::numMetrics: break;
        }

        return nan;
    }

    // ------------------------------------------------------
    // Sampler
    // ------------------------------------------------------
    Sampler::Sampler()
        : juce::Thread ("Y2K SystemStats")
    {
        for (auto& row : history)
            row.fill (std::numeric_limits<float>::quiet_NaN());
    }

    Sampler::~Sampler()
    {
        signalThreadShouldExit();
        wakeEvent.signal();
        stopThread (4000);
    }

    void Sampler::subscribe (const void* who, double intervalSec)
    {
        {
            const juce::ScopedLock sl (lock);
            subscribers[who] = juce::jlimit (0.5, 10.0, intervalSec);
            numSubscribers.store ((int) subscribers.size());
        }

        recomputeInterval();

        if (! isThreadRunning())
            startThread (juce::Thread::Priority::low);

        wakeEvent.signal();
    }

    void Sampler::unsubscribe (const void* who)
    {
        {
            const juce::ScopedLock sl (lock);
            subscribers.erase (who);
            numSubscribers.store ((int) subscribers.size());
        }

        recomputeInterval();
    }

    void Sampler::recomputeInterval()
    {
        const juce::ScopedLock sl (lock);

        double best = 10.0;
        for (const auto& kv : subscribers)
            best = juce::jmin (best, kv.second);

        currentInterval.store (subscribers.empty() ? 1.0 : best);
    }

    Snapshot Sampler::getLatest() const
    {
        const juce::ScopedLock sl (lock);
        return latest;
    }

    void Sampler::copyHistory (Metric m, std::vector<float>& out, int maxCount) const
    {
        const juce::ScopedLock sl (lock);

        const int n = juce::jlimit (0, historyCount, maxCount);
        out.resize ((size_t) n);

        const auto& row = history[(size_t) m];
        int idx = (historyWrite - n + kHistoryLen) % kHistoryLen;

        for (int i = 0; i < n; ++i)
        {
            out[(size_t) i] = row[(size_t) idx];
            idx = (idx + 1) % kHistoryLen;
        }
    }

    void Sampler::run()
    {
        std::unique_ptr<PlatformBackend> backend;

        // 静态信息只取一次（getCpuModel 在 Windows 上读注册表）
        const juce::String cpuName = juce::SystemStats::getCpuModel().trim();
        const juce::String osName  = juce::SystemStats::getOperatingSystemName();

       #if JUCE_WINDOWS
        const juce::File sysVolume = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory);
       #else
        const juce::File sysVolume ("/");
       #endif

        juce::uint32 count = 0;

        while (! threadShouldExit())
        {
            if (numSubscribers.load() == 0)
            {
                wakeEvent.wait (1000);
                continue;
            }

            if (backend == nullptr)
                backend = createPlatformBackend();

            const double t0 = juce::Time::getMillisecondCounterHiRes();

            Snapshot s;
            s.intervalSec = currentInterval.load();
            s.cpuName     = cpuName;
            s.osName      = osName;
            s.numCores    = juce::jlimit (0, kMaxCores, juce::SystemStats::getNumCpus());

            if (backend != nullptr)
                backend->sample (s);

            if (! Snapshot::isValid (s.ramTotal))
                s.ramTotal = (double) juce::SystemStats::getMemorySizeInMegabytes() * 1024.0 * 1024.0;

            s.sysDriveTotal = (double) sysVolume.getVolumeTotalSize();
            s.sysDriveFree  = (double) sysVolume.getBytesFreeOnVolume();

            if (threadShouldExit())
                break;

            {
                const juce::ScopedLock sl (lock);

                s.serial = ++count;
                latest = s;

                for (int m = 0; m < kNumMetrics; ++m)
                    history[(size_t) m][(size_t) historyWrite] = s.metricValue ((Metric) m);

                historyWrite = (historyWrite + 1) % kHistoryLen;
                historyCount = juce::jmin (historyCount + 1, kHistoryLen);
            }

            serial.store (count, std::memory_order_release);

            const double elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
            const int waitMs = juce::jmax (50, (int) (currentInterval.load() * 1000.0 - elapsed));
            wakeEvent.wait (waitMs);
        }

        // 后端在采集线程上析构（COM / PDH 句柄与创建线程一致）
        backend.reset();
    }

    // ------------------------------------------------------
    // 格式化
    // ------------------------------------------------------
    juce::String formatBytes (double bytes, int decimals)
    {
        if (! Snapshot::isValid (bytes))
            return "--";

        static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        int u = 0;
        while (bytes >= 1024.0 && u < 4) { bytes /= 1024.0; ++u; }

        // 注意：juce::String (double, 0) 是"默认格式"而不是四舍五入到整数
        const bool whole = (u == 0 || bytes >= 100.0 || decimals <= 0);
        return (whole ? juce::String (juce::roundToInt (bytes)) : juce::String (bytes, decimals)) + " " + units[u];
    }

    juce::String formatBytesShort (double bytes)
    {
        if (! Snapshot::isValid (bytes))
            return "--";

        static const char* units[] = { "B", "K", "M", "G", "T" };
        int u = 0;
        while (bytes >= 1024.0 && u < 4) { bytes /= 1024.0; ++u; }

        const bool whole = (u == 0 || bytes >= 10.0);
        return (whole ? juce::String (juce::roundToInt (bytes)) : juce::String (bytes, 1)) + units[u];
    }

    juce::String formatRate (double bps)
    {
        if (! Snapshot::isValid (bps))
            return "--";

        return formatBytes (bps, 1) + "/s";
    }

    juce::String formatUptime (double seconds)
    {
        if (! Snapshot::isValid (seconds))
            return "--";

        const auto total = (juce::int64) seconds;
        const int days  = (int) (total / 86400);
        const int hours = (int) ((total / 3600) % 24);
        const int mins  = (int) ((total / 60) % 60);

        auto two = [] (int v) { return juce::String (v).paddedLeft ('0', 2); };

        if (days > 0)
            return juce::String (days) + "d " + two (hours) + ":" + two (mins);

        return two (hours) + ":" + two (mins) + ":" + two ((int) (total % 60));
    }

   #if ! JUCE_WINDOWS && ! JUCE_MAC
    // 其它平台：只提供 JUCE 能给的内存总量，其余 N/A
    std::unique_ptr<PlatformBackend> createPlatformBackend()
    {
        struct NullBackend : PlatformBackend
        {
            void sample (Snapshot&) override {}
        };
        return std::make_unique<NullBackend>();
    }
   #endif
}
