#pragma once
// ==========================================================
// ToyPerf —— Toybox 性能日志（每次启动追加写入同一文件，供多轮优化比对）
//   · 日志位置：exe 所在目录 / toy_perf_log.txt（不写桌面）
//   · session 分隔行延迟到消息循环稳定后（onFrame 首帧）才写，
//     构造期绝不碰文件 IO（黑屏卡死防御）。
//   · 线程安全（CriticalSection）；IO 一旦失败即静默降级，绝不阻塞重试。
// ==========================================================
#include <juce_core/juce_core.h>

namespace toyperf
{
    inline juce::File file()
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::invokedExecutableFile);
        return exe.getParentDirectory().getChildFile ("toy_perf_log.txt");
    }

    inline void log (const juce::String& msg)
    {
        static juce::CriticalSection cs;
        static bool ioFailed = false;
        const juce::ScopedLock sl (cs);
        if (ioFailed)
            return;   // IO 失败后静默，不再尝试（避免每次卡在文件系统）
        const double ms = juce::Time::getMillisecondCounterHiRes();
        if (! file().appendText (juce::String (ms, 3) + " | " + msg + "\n"))
            ioFailed = true;
    }

    // 只在消息循环稳定后调用一次（ToyClock 首帧），构造期不调用
    inline void beginSession()
    {
        static bool begun = false;
        if (begun)
            return;
        begun = true;
        log ("======================================================");
        log ("== session start " + juce::Time::getCurrentTime().toString (true, true)
             + "  exe=" + file().getFullPathName());
    }

    // 简易计时器：构造即计时，析构打点
    struct ScopedTimer
    {
        ScopedTimer (juce::String name_) : name (std::move (name_)), t0 (juce::Time::getMillisecondCounterHiRes()) {}
        ~ScopedTimer()
        {
            log (name + " took " + juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 2) + "ms");
        }
        juce::String name;
        double t0 = 0.0;
    };
}
