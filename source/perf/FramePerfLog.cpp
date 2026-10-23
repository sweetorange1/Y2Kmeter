#include "source/perf/FramePerfLog.h"

#if Y2K_ENABLE_FRAME_PERF_LOG

#include <algorithm>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace y2k::perflog
{
    namespace
    {
        struct Agg
        {
            juce::int64 calls  = 0;
            juce::int64 totalUs = 0;
            juce::int64 maxUs  = 0;
        };

        // 所有埋点只来自 UI/GL 两条线程，频率低（<30 次/帧），临界区足够
        juce::CriticalSection gLock;
        std::unordered_map<std::string, Agg> gMap;

        double gLastDumpWallMs = 0.0;
        std::vector<std::string> gSessionLines;  // 全 session 累计，每次 dump 整体重写文件
    }

    ScopedSample::ScopedSample (const char* c)
        : category (c), startTicks (juce::Time::getHighResolutionTicks())
    {
    }

    ScopedSample::~ScopedSample()
    {
        const juce::int64 elapsedTicks = juce::Time::getHighResolutionTicks() - startTicks;
        const double sec = juce::Time::highResolutionTicksToSeconds (elapsedTicks);
        const juce::int64 us = (juce::int64) (juce::jmax (0.0, sec) * 1.0e6);

        const juce::ScopedLock sl (gLock);
        auto& a = gMap[category];
        ++a.calls;
        a.totalUs += us;
        if (us > a.maxUs) a.maxUs = us;
    }

    void dumpIfDue()
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (gLastDumpWallMs > 0.0 && nowMs - gLastDumpWallMs < 5000.0)
            return;
        gLastDumpWallMs = nowMs;

        // 取出并重置本窗口统计
        struct Row
        {
            std::string name;
            juce::int64 calls, totalUs, maxUs;
        };
        std::vector<Row> rows;
        {
            const juce::ScopedLock sl (gLock);
            rows.reserve (gMap.size());
            for (auto& kv : gMap)
            {
                rows.push_back ({ kv.first, kv.second.calls, kv.second.totalUs, kv.second.maxUs });
                kv.second.calls = 0;
                kv.second.totalUs = 0;
                kv.second.maxUs = 0;
            }
        }

        std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b)
        {
            return a.totalUs > b.totalUs;  // 总耗时降序：最贵的类别排最前
        });

        juce::int64 glCalls = 0, glTotalUs = 0;
        juce::int64 paintTotalUs = 0;
        juce::int64 dispatchCalls = 0, dispatchTotalUs = 0;
        for (const auto& r : rows)
        {
            if (r.name == "glFrame") { glCalls = r.calls; glTotalUs = r.totalUs; }
            else if (r.name == "dispatch") { dispatchCalls = r.calls; dispatchTotalUs = r.totalUs; }
            else paintTotalUs += r.totalUs;
        }

        const double windowSec = 5.0;
        std::string block;
        block.reserve (2048);
        block += "==== window @ " + std::to_string ((int) (nowMs / 1000.0)) + "s ====\n";

        if (glCalls > 0)
        {
            const double fps = (double) glCalls / windowSec;
            const double avgMs = (double) glTotalUs / (double) glCalls / 1000.0;
            const double paintPerFrameMs = (double) paintTotalUs / (double) glCalls / 1000.0;
            block += "  GL frames: " + std::to_string (glCalls)
                   + "  (" + std::to_string ((int) (fps + 0.5)) + " fps)"
                   + "  renderOpenGL avg " + std::to_string (avgMs) + " ms"
                   + "  module-paint-per-frame " + std::to_string (paintPerFrameMs) + " ms\n";
        }
        if (dispatchCalls > 0)
        {
            const double dFps = (double) dispatchCalls / windowSec;
            const double dAvg = (double) dispatchTotalUs / (double) dispatchCalls / 1000.0;
            block += "  dispatch: " + std::to_string (dispatchCalls)
                   + "  (" + std::to_string ((int) (dFps + 0.5)) + "/s)"
                   + "  avg " + std::to_string (dAvg) + " ms\n";
        }

        for (const auto& r : rows)
        {
            char line[160];
            snprintf (line, sizeof (line), "  %-20s calls=%6lld  total=%8.1fms  avg=%7.1fus  max=%7.1fus\n",
                      r.name.c_str(),
                      (long long) r.calls,
                      (double) r.totalUs / 1000.0,
                      r.calls > 0 ? (double) r.totalUs / (double) r.calls : 0.0,
                      (double) r.maxUs);
            block += line;
        }
        block += "\n";

        gSessionLines.push_back (block);

        // 每次整体重写（session 行数少，成本可忽略；避免 append 模式下的截断文件）
        auto f = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                    .getSiblingFile ("Y2Kmeter_perf_log.txt");
        if (auto out = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream()))
        {
            for (const auto& l : gSessionLines)
                out->write (l.data(), l.size());
            out->flush();
        }
    }
}

#endif
