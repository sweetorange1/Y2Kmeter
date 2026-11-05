#pragma once
// ==========================================================
// ToyPerf —— Toybox 性能日志（**仅 RelWithDebInfo 构建**）
//
//   门控：Y2K_ENABLE_LOGGING（见 source/Y2KLogging.h 与 CMakeLists 里的
//   $<$<CONFIG:RelWithDebInfo>:Y2K_ENABLE_LOGGING=1>）。
//     · RelWithDebInfo：保留完整性能日志，供多轮优化比对；
//     · 正式 Release：整段实现不编译，调用点用 TOYPERF_LOG 宏展开为空，
//       **连字符串拼接都不会发生**（与 Y2K_LOG 同一套约定）。
//   历史教训：v2.9.x 曾无条件输出，正式安装包会在 Program Files 下
//   持续追加一个几 MB 的 toy_perf_log.txt（每秒一次 open+append+close），
//   既浪费算力又污染安装目录，故收敛为编译期门控。
//
//   · 日志位置：exe 所在目录 / toy_perf_log.txt（不写桌面）
//   · session 分隔行延迟到消息循环稳定后（onFrame 首帧）才写，
//     构造期绝不碰文件 IO（黑屏卡死防御）。
//   · 线程安全（CriticalSection）；IO 一旦失败即静默降级，绝不阻塞重试。
// ==========================================================
#include <juce_core/juce_core.h>

// __LINE__ 拼接（两个宏必须都是单行定义，见下方 TOYPERF_SCOPED_TIMER）
#define TOYPERF_CONCAT_INNER(a, b) a##b
#define TOYPERF_CONCAT(a, b)       TOYPERF_CONCAT_INNER (a, b)

#if defined (Y2K_ENABLE_LOGGING)

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

  #define TOYPERF_LOG(msg)              ::toyperf::log (msg)
  #define TOYPERF_BEGIN_SESSION()       ::toyperf::beginSession()
  // 作用域计时：TOYPERF_SCOPED_TIMER("xxx")  →  const scope 变量，析构时打点
  #define TOYPERF_SCOPED_TIMER(msgExpr) ::toyperf::ScopedTimer TOYPERF_CONCAT (toyPerfTimer_, __LINE__) (msgExpr)

#else   // 正式 Release：调用点完全消失，参数表达式不求值

  #define TOYPERF_LOG(msg)              ((void) 0)
  #define TOYPERF_BEGIN_SESSION()       ((void) 0)
  #define TOYPERF_SCOPED_TIMER(msgExpr) ((void) 0)

#endif
