#pragma once

#include <JuceHeader.h>

// ==========================================================
// FramePerfLog —— 帧耗时统计日志（仅开发者诊断构建）
//
// 由编译宏 Y2K_ENABLE_FRAME_PERF_LOG（CMake option，默认 OFF）整段隔离：
//   · 宏关闭时所有头文件内容为空、埋点宏展开为无操作、零运行时开销；
//   · 宏开启时在关键帧路径埋针（模块 paint / GL 渲染 / 帧分发），
//     dumpIfDue() 每 5 秒把滚动窗口统计写入 exe 目录下的
//     "Y2Kmeter_perf_log.txt"（每次启动清空重写）。
//
// 用法：
//   cmake -S . -B build/skill-verify -G "NMake Makefiles" \
//         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DY2K_ENABLE_FRAME_PERF_LOG=ON
//   cmake --build build/skill-verify -j
//   运行 Standalone → 复现场景几分钟 → 发回 Y2Kmeter_perf_log.txt。
//
// 埋点宏：Y2K_PERF_FRAME_SCOPE("类别名") —— 放在函数/代码块起始处，
//   作用域结束自动记录耗时（高精度 QPC，微秒级）。
// ==========================================================

#if Y2K_ENABLE_FRAME_PERF_LOG

namespace y2k::perflog
{
    // 作用域计时器：进入记录起点，析构时把耗时写进对应类别
    class ScopedSample
    {
    public:
        explicit ScopedSample (const char* category);
        ~ScopedSample();

        ScopedSample (const ScopedSample&) = delete;
        ScopedSample& operator= (const ScopedSample&) = delete;

    private:
        const char* category;
        juce::int64 startTicks;
    };

    // 周期性落盘（调用方定时调用，内部按 5 秒窗口节流）
    void dumpIfDue();
}

#define Y2K_PERF_CONCAT_INNER(a, b) a##b
#define Y2K_PERF_CONCAT(a, b)       Y2K_PERF_CONCAT_INNER(a, b)
#define Y2K_PERF_FRAME_SCOPE(cat) \
    y2k::perflog::ScopedSample Y2K_PERF_CONCAT (y2kPerfScope_, __LINE__) (cat)

#else

#define Y2K_PERF_FRAME_SCOPE(cat)

#endif
