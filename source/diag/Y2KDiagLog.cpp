// =============================================================================
// source/diag/Y2KDiagLog.cpp
//   运行时诊断日志实现。设计要点：
//
//   1. 关闭态零开销：所有写入都经过 gEnabled 原子检查（宏里先判断，
//      连参数表达式都不会求值）。
//   2. 打开态也不拖慢 GL/UI 线程：普通行先进内存缓冲（String），
//      由独立看门狗线程每 250ms 刷盘一次；只有关键事件（上下文创建、
//      STALL 报告等）同步 flush。
//   3. 卡死可定位：每帧只做几个原子写（阶段标记 + 心跳 + 统计累加），
//      真正拼字符串/落盘只发生在看门狗判定异常时。
//   4. 进程被强杀也不丢：日志目录固定放在桌面（不可写时才回退），
//      缓冲最多滞后 250ms。
// =============================================================================
#include "source/diag/Y2KDiagLog.h"

#include <JuceHeader.h>

#include <algorithm>
#include <cstring>
#include <memory>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace y2k::diag
{
namespace
{
    // -------------------------------------------------------------------------
    // 常量
    // -------------------------------------------------------------------------
    constexpr int    kMaxThreadSlots   = 12;
    constexpr int    kWriterTickMs     = 250;      // 缓冲刷盘周期
    constexpr double kSummaryInterval  = 5000.0;   // 统计摘要周期
    constexpr double kStallRepeat      = 15000.0;  // 持续卡死时的重复报告周期
    constexpr int    kMaxDumpsPerRun   = 3;        // 单次会话最多写几份 Minidump
    constexpr int    kMaxMessageBytes  = 8000;     // 单行上限（防病态长串）

    // 全进程唯一的开关原子量（constant initialization，任何静态初始化顺序都能读）
    std::atomic<bool> gEnabled { false };
    std::atomic<bool> gVerbose { false };

    struct ChannelDef
    {
        const char* name;
        double      thresholdMs;      // 超过该空闲时间判定为异常
        bool        canTriggerStall;  // 是否触发 STALL 报告（其余仅记录在摘要里）
    };

    const ChannelDef kChannelDefs[] =
    {
        { "GL-FRAME", 5000.0, true  },
        { "UI-MSG",   4000.0, true  },
        { "DISPATCH", 6000.0, false },
        { "AUDIO",    8000.0, false },
    };

    constexpr int kChannelCount = (int) Channel::Count;

    // -------------------------------------------------------------------------
    // 线程局部数据（阶段标记 / 线程别名）
    //   阶段字符串必须是常驻内存：动态字符串拷进 tls 定长缓冲，
    //   发布给看门狗的是缓冲地址（不会随 String 重分配而失效）。
    // -------------------------------------------------------------------------
    struct ThreadLocalData
    {
        char tagBuf[32]   {};
        char phaseBuf[192] {};
        const char* lastPhasePtr = nullptr;   // 快路径：指针不变则跳过拷贝
        int  slot = -1;
    };

    thread_local ThreadLocalData tls;

    struct ThreadSlot
    {
        std::atomic<bool>        used  { false };
        std::atomic<const char*> tag   { nullptr };
        std::atomic<const char*> phase { nullptr };
    };

    // -------------------------------------------------------------------------
    // 内核状态（故意 new 出来不释放：看门狗线程可能在静态析构阶段仍在跑，
    //   走静态对象析构会 UAF。进程退出由系统回收）
    // -------------------------------------------------------------------------
    struct Core
    {
        std::atomic<bool> initAttempted { false };
        std::atomic<bool> shuttingDown  { false };

        double        startMs = 0.0;
        juce::String  startWall;

        juce::File    logDir;
        juce::File    logFile;

        juce::CriticalSection                     ioLock;
        std::unique_ptr<juce::FileOutputStream>   fileStream;
        // 自研输出缓冲（JUCE 8 没有 BufferedOutputStream）：
        //   write() 只追加到这个 String，看门狗每 250ms 或积压 32KB 时真正落盘，
        //   既不阻塞 GL/UI 线程，进程被强杀时也最多丢最后 250ms 的日志。
        juce::String  pendingText;
        juce::int64   writtenBytes = 0;

        juce::String  glInfo;           // 受 ioLock 保护
        juce::String  sessionInfo;      // 受 ioLock 保护

        ThreadSlot    threads[kMaxThreadSlots];

        std::atomic<double> lastBeat[kChannelCount] {};
        std::atomic<bool>   channelActive[kChannelCount] {};

        // 每 5s 汇总的统计量
        std::atomic<juce::int64> frames     { 0 };
        std::atomic<juce::int64> frameUsSum { 0 };
        std::atomic<juce::int64> frameUsMax { 0 };
        std::atomic<juce::int64> milkUsSum  { 0 };
        std::atomic<juce::int64> directUsSum{ 0 };
        std::atomic<juce::int64> uiTicks    { 0 };
        std::atomic<juce::int64> dispatchTicks { 0 };

        // Editor 状态镜像（只读原子量，看门狗不碰组件树）
        std::atomic<int>         edW { 0 }, edH { 0 }, edDpi100 { 0 }, edModules { 0 };
        std::atomic<bool>        edAttached { false }, edCreated { false }, edMilkdrop { false };
        std::atomic<juce::int64> edGlFrames { 0 };

        // GL 上下文建立监测
        std::atomic<double> glAttachedAt { 0.0 };
        std::atomic<bool>   glCreatedFlag { false };
        std::atomic<bool>   glAttachWarned { false };

        // 启动阻塞监测
        std::atomic<bool>   uiEverBeat { false };
        std::atomic<bool>   startupWarned { false };

        std::atomic<juce::String (*)()> moduleProvider { nullptr };

        class Watchdog* watchdog = nullptr;
        juce::uint32    processId = 0;
        int             dumpsWritten = 0;
    };

    Core& core()
    {
        // C++11 起函数内静态初始化线程安全
        static Core* instance = new Core();
        return *instance;
    }

    double nowMs() noexcept
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    juce::String makeTimestampForFile()
    {
        const auto t = juce::Time::getCurrentTime();
        return t.formatted ("%Y-%m-%d_%H-%M-%S");
    }

    juce::String environmentSummary();   // 前置声明

    // -------------------------------------------------------------------------
    // 开关判定
    // -------------------------------------------------------------------------
    juce::Array<juce::File> markerFiles()
    {
        juce::Array<juce::File> files;
        const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                .getParentDirectory();
        files.add (exeDir.getChildFile ("Y2Kmeter_diag_on.txt"));
        files.add (exeDir.getChildFile ("diag_on.txt"));

        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                 .getChildFile ("Y2Kmeter");
        files.add (appData.getChildFile ("diag_on.txt"));
        files.add (appData.getChildFile ("Y2Kmeter_diag_on.txt"));
        return files;
    }

    bool shouldEnable()
    {
        const auto env = juce::SystemStats::getEnvironmentVariable ("Y2K_DIAG_LOG", juce::String());
        if (env.isNotEmpty())
        {
            const auto v = env.trim().toLowerCase();
            if (v == "0" || v == "off" || v == "false" || v == "no")
                return false;
            return true;
        }

        for (const auto& f : markerFiles())
            if (f.existsAsFile())
                return true;

       #if defined (Y2K_DIAG_LOG_DEFAULT) && Y2K_DIAG_LOG_DEFAULT
        return true;
       #elif defined (Y2K_ENABLE_LOGGING)
        // RelWithDebInfo（本工程约定的"带日志"配置，见 Y2KLogging.h）：
        //   诊断日志默认开启 —— 打 RelWithDebInfo 包即自动带日志，
        //   Release 下该宏不存在，保持纯净。
        return true;
       #else
        return false;
       #endif
    }

    bool probeWritable (const juce::File& dir)
    {
        if (dir == juce::File())
            return false;
        if (! dir.isDirectory() && ! dir.createDirectory().wasOk())
            return false;

        auto probe = dir.getChildFile (".y2k_write_probe");
        const bool ok = probe.replaceWithText ("ok");
        probe.deleteFile();
        return ok;
    }

    juce::File pickLogDirectory()
    {
        const auto desktop = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                 .getChildFile ("Y2Kmeter_Diag");
        const auto documents = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                   .getChildFile ("Y2Kmeter_Diag");
        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                 .getChildFile ("Y2Kmeter").getChildFile ("Diag");

       #if JUCE_WINDOWS
        // Windows：优先桌面 —— 用户最容易找到（卡死被强杀后也能自己翻出来）。
        if (probeWritable (desktop))   return desktop;
        if (probeWritable (documents)) return documents;
        if (probeWritable (appData))   return appData;
       #else
        // macOS：桌面/文档属于 TCC 受保护目录，启动即写会弹权限框，因此先走
        //   ~/Library/Application Support（与 settings 同处，无权限问题），
        //   再退回桌面。
        if (probeWritable (appData))   return appData;
        if (probeWritable (desktop))   return desktop;
        if (probeWritable (documents)) return documents;
       #endif

        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("Y2Kmeter_Diag");
    }

    void writeReadmeIfMissing (const juce::File& dir)
    {
        auto readme = dir.getChildFile ("README_请把这个文件夹发给开发者.txt");
        if (readme.existsAsFile())
            return;

        juce::String t;
        t << "Y2Kmeter 诊断日志\n"
          << "=================\n\n"
          << "这个文件夹是软件在【诊断模式】下自动生成的运行日志，用来定位问题。\n\n"
          << "出现问题时请这样做：\n"
          << "  1. 复现问题（例如画面卡住）；如果程序卡死无响应，直接用任务管理器结束它；\n"
          << "  2. 把整个 \"Y2Kmeter_Diag\" 文件夹压缩成 zip；\n"
          << "  3. 发给开发者。\n\n"
          << "文件夹里可能出现：\n"
          << "  Y2Kmeter_diag_*.log    运行日志（主要文件）\n"
          << "  Y2Kmeter_hang_*.dmp    卡死瞬间的程序内存转储（开发者用，可选）\n"
          << "  Y2Kmeter_snapshot_*.txt 手动导出的状态快照\n\n"
          << "日志只记录运行状态（界面/显卡/耗时），不包含音频内容。\n";
        readme.replaceWithText (t);
    }

    // -------------------------------------------------------------------------
    // 底层写入
    // -------------------------------------------------------------------------
    // 调用方必须已持有 ioLock
    void flushLocked (Core& c) noexcept
    {
        if (c.fileStream == nullptr || c.pendingText.isEmpty())
            return;

        try
        {
            const auto numBytes = (size_t) c.pendingText.getNumBytesAsUTF8();
            c.fileStream->write (c.pendingText.toRawUTF8(), numBytes);
            c.fileStream->flush();
        }
        catch (...)
        {
        }

        c.pendingText.clear();
    }

    void appendLine (const char* tag, const juce::String& message, bool flushNow) noexcept
    {
        if (! gEnabled.load (std::memory_order_relaxed))
            return;

        auto& c = core();

        try
        {
            juce::String msg = message;
            if (msg.getNumBytesAsUTF8() > kMaxMessageBytes)
                msg = msg.substring (0, kMaxMessageBytes / 2) + "...(truncated)";

            const double elapsed = (nowMs() - c.startMs) * 0.001;

            const char* threadTag = tls.tagBuf[0] != 0 ? tls.tagBuf : "-";
            juce::String tagPadded (threadTag);
            while (tagPadded.length() < 4)
                tagPadded << " ";

            juce::String line;
            line.preallocateBytes (msg.getNumBytesAsUTF8() + 96);
            line << juce::String::formatted ("%9.3f", elapsed) << "s  "
                 << tagPadded << " [" << tag << "] " << msg << juce::newLine;

            const juce::ScopedLock sl (c.ioLock);
            if (c.fileStream == nullptr || c.shuttingDown.load (std::memory_order_relaxed))
                return;

            c.pendingText << line;
            c.writtenBytes += line.getNumBytesAsUTF8();

            // 关键行立即落盘；普通行积压到 32KB 或由看门狗 250ms 刷一次
            if (flushNow || c.pendingText.getNumBytesAsUTF8() > 32 * 1024)
                flushLocked (c);
        }
        catch (...)
        {
            // 日志绝不允许影响主流程
        }
    }

    void flushBuffer() noexcept
    {
        auto& c = core();
        const juce::ScopedLock sl (c.ioLock);
        flushLocked (c);
    }

    // -------------------------------------------------------------------------
    // Minidump（Windows）
    //   动态加载 dbghelp，不引入链接依赖；dump 里含所有线程的调用栈，
    //   事后配合构建产生的 PDB 在 Visual Studio / WinDbg 里可直接定位死锁点。
    // -------------------------------------------------------------------------
   #if JUCE_WINDOWS
    bool writeMiniDump (const juce::File& target)
    {
        using MiniDumpWriteDumpFn = BOOL (WINAPI*) (HANDLE, DWORD, HANDLE, int, void*, void*, void*);

        static MiniDumpWriteDumpFn fn = nullptr;
        static bool resolved = false;
        if (! resolved)
        {
            resolved = true;
            if (auto* dll = ::LoadLibraryW (L"dbghelp.dll"))
                fn = (MiniDumpWriteDumpFn) (void*) ::GetProcAddress (dll, "MiniDumpWriteDump");
        }
        if (fn == nullptr)
            return false;

        const auto path = target.getFullPathName();
        HANDLE h = ::CreateFileW (path.toWideCharPointer(),
                                  GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;

        // MiniDumpNormal(0) | MiniDumpWithThreadInfo(0x1000)：体积小但含全部线程栈
        constexpr int kDumpType = 0x00000000 | 0x00001000;
        const BOOL ok = fn (::GetCurrentProcess(), ::GetCurrentProcessId(), h,
                            kDumpType, nullptr, nullptr, nullptr);
        ::CloseHandle (h);
        return ok != FALSE;
    }
   #endif

    // -------------------------------------------------------------------------
    // 状态报告
    // -------------------------------------------------------------------------
    juce::String threadSection()
    {
        auto& c = core();
        juce::String s;
        s << "  threads:\n";
        int usedCount = 0;
        for (int i = 0; i < kMaxThreadSlots; ++i)
        {
            if (! c.threads[i].used.load())
                continue;
            ++usedCount;
            const char* tag   = c.threads[i].tag.load();
            const char* phase = c.threads[i].phase.load();
            s << "    [" << (tag != nullptr ? tag : "?") << "] "
              << (phase != nullptr ? phase : "(no phase set)") << "\n";
        }
        if (usedCount == 0)
            s << "    (none)\n";
        return s;
    }

    juce::String channelSection()
    {
        auto& c = core();
        juce::String s;
        s << "  channels:\n";
        const double now = nowMs();
        for (int i = 0; i < kChannelCount; ++i)
        {
            const bool   active = c.channelActive[i].load();
            const double last   = c.lastBeat[i].load();
            s << "    " << juce::String (kChannelDefs[i].name).paddedRight (' ', 9)
              << (active ? "active " : "idle   ")
              << (last > 0.0 ? juce::String ((now - last) / 1000.0, 2) + "s since last beat"
                             : juce::String ("no beat yet"))
              << "\n";
        }
        return s;
    }

    juce::String editorSection()
    {
        auto& c = core();
        juce::String s;
        s << "  editor: size=" << c.edW.load() << "x" << c.edH.load()
          << " dpi=" << juce::String (c.edDpi100.load() / 100.0, 2)
          << " glAttached=" << (c.edAttached.load() ? 1 : 0)
          << " glContextCreated=" << (c.edCreated.load() ? 1 : 0)
          << " milkdropReady=" << (c.edMilkdrop.load() ? 1 : 0)
          << " modules=" << c.edModules.load()
          << " glFrames=" << (long long) c.edGlFrames.load() << "\n";

        const juce::String gl = [&]
        {
            const juce::ScopedLock sl (c.ioLock);
            return c.glInfo;
        }();

        if (gl.isNotEmpty())
            s << "  gl: " << gl << "\n";
        return s;
    }

    juce::String buildStateReport (const char* reason)
    {
        juce::String s;
        s << "\n==================== STATE REPORT: " << reason << " ====================\n"
          << "  uptime=" << juce::String ((nowMs() - core().startMs) / 1000.0, 1) << "s"
          << "  wall=" << juce::Time::getCurrentTime().toString (true, true, true, true) << "\n";

        s << threadSection();
        s << channelSection();
        s << editorSection();

        if (auto* provider = core().moduleProvider.load())
        {
            s << "  modules:\n";
            try { s << provider(); } catch (...) {}
        }

        s << "========================================================================\n";
        return s;
    }

    void writeDumpForStall (const juce::String& reason)
    {
       #if JUCE_WINDOWS
        auto& c = core();
        if (c.dumpsWritten >= kMaxDumpsPerRun)
            return;

        const auto target = c.logDir.getChildFile ("Y2Kmeter_hang_" + makeTimestampForFile() + ".dmp");
        const bool ok = writeMiniDump (target);

        if (ok)
        {
            ++c.dumpsWritten;
            writeCritical ("DUMP", "minidump written: " + target.getFullPathName()
                                      + " (" + juce::String (target.getSize() / 1024) + " KB) for " + reason);
        }
        else
        {
            write ("DUMP", "minidump write FAILED (dbghelp unavailable or file not writable) for " + reason);
        }
       #else
        juce::ignoreUnused (reason);
        write ("DUMP", "minidump 仅 Windows 支持（当前平台跳过）；状态报告已足够定位线程阶段");
       #endif
    }

    void emitStallReport (const char* reason)
    {
        flushBuffer();
        writeCritical ("STALL", "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        writeCritical ("STALL", u8 ("!! 卡死/停摆检测: ") + reason);

        // 逐行写：多行字符串一次性写进文件时只有首行带时间戳，读起来会很乱
        juce::StringArray lines;
        lines.addLines (buildStateReport (reason));
        for (const auto& l : lines)
            writeCritical ("STALL", l);

        writeCritical ("STALL", "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        writeDumpForStall (reason);
    }

    // -------------------------------------------------------------------------
    // 看门狗线程：刷盘 + 卡死判定 + 5s 统计
    // -------------------------------------------------------------------------
    class Watchdog : public juce::Thread
    {
    public:
        Watchdog() : juce::Thread ("Y2K-DiagWatchdog") {}

        void run() override
        {
            double lastSummary = nowMs();
            const double startTime = lastSummary;

            while (! threadShouldExit())
            {
                wait (kWriterTickMs);
                if (threadShouldExit())
                    break;

                try
                {
                    tick (lastSummary, startTime);
                }
                catch (...)
                {
                }
            }
        }

    private:
        bool  stalled[kChannelCount] {};
        double lastReportMs = 0.0;

        void tick (double& lastSummary, double startTime)
        {
            flushBuffer();

            const double now = nowMs();

            // ---- GL 上下文建立超时（驱动卡在 wglCreateContext 的典型现场）----
            {
                auto& c = core();
                const double attachedAt = c.glAttachedAt.load();
                if (attachedAt > 0.0 && ! c.glCreatedFlag.load() && ! c.glAttachWarned.load()
                    && now - attachedAt > 10000.0)
                {
                    c.glAttachWarned.store (true);
                    writeCritical ("GL", u8 ("!! attach 已 ")
                                           + juce::String ((now - attachedAt) / 1000.0, 1)
                                           + "s 仍未创建 GL 上下文"
                                             "（newOpenGLContextCreated 未回调）——驱动/远程桌面高危");
                }
            }

            // ---- 启动阻塞判定 ----
            //   开关打开后 30s 内消息线程一次心跳都没有 = 卡在启动流程里
            //   （典型：驱动创建 GL 上下文 / 扫描预设 / 音频设备初始化）。
            {
                auto& c = core();
                if (! c.uiEverBeat.load() && now - startTime > 30000.0 && ! c.startupWarned.load())
                {
                    c.startupWarned.store (true);
                    writeCritical ("STALL", "!! 启动后 30s 内消息线程从未心跳 —— 启动流程疑似被阻塞");
                    juce::StringArray lines;
                    lines.addLines (buildStateReport ("startup blocked"));
                    for (const auto& l : lines)
                        writeCritical ("STALL", l);
                }
            }

            // ---- 卡死判定 ----
            for (int i = 0; i < kChannelCount; ++i)
            {
                if (! core().channelActive[i].load())
                {
                    stalled[i] = false;
                    continue;
                }

                const double last = core().lastBeat[i].load();
                if (last <= 0.0)
                    continue;

                const double idleMs = now - last;
                if (idleMs < kChannelDefs[i].thresholdMs)
                {
                    if (stalled[i])
                    {
                        stalled[i] = false;
                        write ("RECOVER", juce::String (kChannelDefs[i].name) + " recovered");
                    }
                    continue;
                }

                if (! kChannelDefs[i].canTriggerStall)
                    continue;

                if (! stalled[i])
                {
                    stalled[i] = true;
                    lastReportMs = now;
                    emitStallReport ((juce::String (kChannelDefs[i].name)
                                      + " 无心跳 " + juce::String (idleMs / 1000.0, 1) + "s").toRawUTF8());
                }
                else if (now - lastReportMs > kStallRepeat)
                {
                    lastReportMs = now;
                    emitStallReport ((juce::String (kChannelDefs[i].name)
                                      + " 仍然无心跳 " + juce::String (idleMs / 1000.0, 1) + "s").toRawUTF8());
                }
            }

            // ---- 5s 统计 ----
            if (now - lastSummary >= kSummaryInterval)
            {
                const double windowSec = juce::jmax (0.1, (now - lastSummary) / 1000.0);
                const double uptimeSec = (now - startTime) / 1000.0;
                lastSummary = now;
                writeSummary (windowSec, uptimeSec);
            }
        }

        static void writeSummary (double windowSec, double uptimeSec)
        {
            auto& c = core();

            const auto frames = c.frames.exchange (0);
            const auto frameUs = c.frameUsSum.exchange (0);
            const auto frameMaxUs = c.frameUsMax.exchange (0);
            const auto milkUs = c.milkUsSum.exchange (0);
            const auto directUs = c.directUsSum.exchange (0);
            const auto uiTicks = c.uiTicks.exchange (0);
            const auto dispatchTicks = c.dispatchTicks.exchange (0);

            juce::String s;
            // 注意：String (double, 0) 在 JUCE 里等于"最短表示"，会输出 5.01099 这种，
            //   要整数得显式转 int。
            s << juce::String (windowSec, 1) << "s window (uptime " << (int) uptimeSec << "s)";

            if (frames > 0)
            {
                const double fps = (double) frames / windowSec;
                s << " | GL " << (int) frames << "f (" << juce::String (fps, 1) << "/s)"
                  << " avg " << juce::String ((double) frameUs / (double) frames / 1000.0, 2) << "ms"
                  << " max " << juce::String ((double) frameMaxUs / 1000.0, 2) << "ms";

                if (milkUs > 0)
                    s << " | md " << juce::String ((double) milkUs / (double) frames / 1000.0, 2) << "ms";
                if (directUs > 0)
                    s << " | direct " << juce::String ((double) directUs / (double) frames / 1000.0, 2) << "ms";
            }
            else
            {
                s << " | GL 0f (无出帧)";
            }

            s << " | UI " << (int) ((double) uiTicks / windowSec) << "/s"
              << " | dispatch " << (int) ((double) dispatchTicks / windowSec) << "/s";

            write ("STATS", s);

            if (gVerbose.load())
            {
                if (auto* provider = c.moduleProvider.load())
                {
                    try { write ("MODULES", provider()); } catch (...) {}
                }
            }
        }
    };

    // -------------------------------------------------------------------------
    // 环境信息
    // -------------------------------------------------------------------------
    juce::String environmentSummary()
    {
        juce::String s;
        s << "os=" << juce::SystemStats::getOperatingSystemName()
          << " (" << (juce::SystemStats::isOperatingSystem64Bit() ? "64bit" : "32bit") << ")";
      #if JUCE_WINDOWS
        s << " remoteSession=" << ((::GetSystemMetrics (SM_REMOTESESSION) != 0) ? 1 : 0);
      #endif
        s << "\n  cpu=" << juce::SystemStats::getCpuModel()
          << " / " << juce::String ((int) juce::SystemStats::getNumCpus()) << " cores"
          << " / mem=" << juce::String ((int) juce::SystemStats::getMemorySizeInMegabytes()) << "MB";

        s << "\n  juce=" << juce::String (JUCE_MAJOR_VERSION) << "." << JUCE_MINOR_VERSION
          << "." << JUCE_BUILDNUMBER;

        // 构建配置标识：诊断包是 RelWithDebInfo，必须能在日志里一眼确认
        //   （Y2K_ENABLE_LOGGING 只在本工程的 RelWithDebInfo 配置下定义，见 Y2KLogging.h）
       #if defined (Y2K_BUILD_CONFIG)
        s << " | build=" << Y2K_BUILD_CONFIG;
       #elif defined (Y2K_ENABLE_LOGGING)
        s << " | build=RelWithDebInfo";
       #elif JUCE_DEBUG
        s << " | build=Debug";
       #else
        s << " | build=Release";
       #endif

      #if defined (JucePlugin_Name)
        s << " | app=" << JucePlugin_Name << " " << JucePlugin_VersionString;
      #endif

        s << "\n  exe=" << juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
        s << "\n  pid=" << (int) core().processId;

        // 显示器 / DPI —— 多屏 + 高 DPI 是 GL 布局问题的高发区
        auto& displays = juce::Desktop::getInstance().getDisplays();
        s << "\n  displays=" << displays.displays.size();
        for (const auto& d : displays.displays)
        {
            s << "\n    " << (d.isMain ? "*" : "-")
              << " " << d.totalArea.getWidth() << "x" << d.totalArea.getHeight()
              << " scale=" << juce::String (d.scale, 2)
              << " dpi=" << juce::String (d.dpi, 0);
        }

        return s;
    }

    void writeSessionHeader()
    {
        auto& c = core();
        c.startMs = nowMs();
        c.startWall = juce::Time::getCurrentTime().toString (true, true, true, true);

        writeCritical ("BOOT", "================ Y2Kmeter diagnostics session ================");
        writeCritical ("BOOT", "start=" + c.startWall);
        writeCritical ("BOOT", "logdir=" + c.logDir.getFullPathName());

        // 环境信息本身是多行文本，逐行写（否则只有首行有时间戳，可读性很差）
        juce::StringArray envLines;
        envLines.addLines (environmentSummary());
        for (const auto& l : envLines)
            writeCritical ("ENV", l);

        const auto info = (c.sessionInfo.isNotEmpty() ? c.sessionInfo : juce::String());
        if (info.isNotEmpty())
            writeCritical ("BOOT", info);

        writeCritical ("BOOT", "=============================================================");
    }
} // namespace

// =============================================================================
// 公共接口
// =============================================================================
bool isEnabled() noexcept
{
    return gEnabled.load (std::memory_order_relaxed);
}

bool isVerbose() noexcept
{
    return gVerbose.load (std::memory_order_relaxed);
}

void ensureInitialised() noexcept
{
    auto& c = core();

    bool expected = false;
    if (! c.initAttempted.compare_exchange_strong (expected, true))
        return;

    try
    {
        if (! shouldEnable())
            return;

        auto dir = pickLogDirectory();
        if (! dir.isDirectory() && ! dir.createDirectory().wasOk())
            return;

        c.logDir = dir;
        c.logFile = dir.getChildFile ("Y2Kmeter_diag_" + makeTimestampForFile() + ".log");

        c.fileStream.reset (c.logFile.createOutputStream().release());
        if (c.fileStream == nullptr)
            return;

       #if JUCE_WINDOWS
        c.processId = (juce::uint32) ::GetCurrentProcessId();
       #else
        c.processId = 0;
       #endif

        const auto verboseEnv = juce::SystemStats::getEnvironmentVariable ("Y2K_DIAG_VERBOSE", juce::String());
        gVerbose.store (verboseEnv.isNotEmpty()
                            && verboseEnv.trim().toLowerCase() != "0", std::memory_order_relaxed);

        // 先置位再写头（write 内部会检查开关）
        gEnabled.store (true, std::memory_order_relaxed);

        writeReadmeIfMissing (dir);
        writeSessionHeader();

        c.watchdog = new Watchdog();   // 故意泄漏；进程退出时由系统回收
        c.watchdog->startThread (juce::Thread::Priority::low);
    }
    catch (...)
    {
        gEnabled.store (false, std::memory_order_relaxed);
    }
}

void shutdown() noexcept
{
    if (! gEnabled.load (std::memory_order_relaxed))
        return;

    try
    {
        auto& c = core();
        writeCritical ("BOOT", "session end");
        flushBuffer();

        c.shuttingDown.store (true, std::memory_order_relaxed);

        if (c.watchdog != nullptr)
        {
            c.watchdog->signalThreadShouldExit();
            c.watchdog->notify();
            c.watchdog->stopThread (2000);
            c.watchdog = nullptr;
        }

        {
            const juce::ScopedLock sl (c.ioLock);
            flushLocked (c);
            c.fileStream.reset();
            c.pendingText.clear();
        }
    }
    catch (...)
    {
    }

    gEnabled.store (false, std::memory_order_relaxed);
}

juce::File getLogDirectory()
{
    return core().logDir;
}

juce::File getLogFile()
{
    return core().logFile;
}

juce::String exportSnapshot()
{
    if (! isEnabled())
        return {};

    try
    {
        auto& c = core();
        const auto text = buildStateReport ("manual snapshot");
        const auto file = c.logDir.getChildFile ("Y2Kmeter_snapshot_" + makeTimestampForFile() + ".txt");
        if (file.replaceWithText (text))
        {
            writeCritical ("SNAPSHOT", "written: " + file.getFullPathName());
            return file.getFullPathName();
        }
    }
    catch (...)
    {
    }
    return {};
}

void write (const char* tag, const char* utf8Message)
{
    appendLine (tag, u8 (utf8Message), false);
}

void write (const char* tag, const juce::String& message)
{
    appendLine (tag, message, false);
}

void writeCritical (const char* tag, const char* utf8Message)
{
    appendLine (tag, u8 (utf8Message), true);
}

void writeCritical (const char* tag, const juce::String& message)
{
    appendLine (tag, message, true);
}

// -----------------------------------------------------------------------------
// 线程标记
// -----------------------------------------------------------------------------
namespace
{
    int claimThreadSlot() noexcept
    {
        auto& c = core();
        for (int i = 0; i < kMaxThreadSlots; ++i)
        {
            bool expected = false;
            if (c.threads[i].used.compare_exchange_strong (expected, true))
                return i;
        }
        return -1;
    }
}

void setThreadTag (const char* tag) noexcept
{
    if (! isEnabled() || tag == nullptr)
        return;

    if (tls.slot < 0)
        tls.slot = claimThreadSlot();

    std::strncpy (tls.tagBuf, tag, sizeof (tls.tagBuf) - 1);
    tls.tagBuf[sizeof (tls.tagBuf) - 1] = 0;

    if (tls.slot >= 0)
        core().threads[tls.slot].tag.store (tls.tagBuf);
}

void setPhase (const char* phase) noexcept
{
    if (! isEnabled() || phase == nullptr)
        return;

    if (tls.slot < 0)
        tls.slot = claimThreadSlot();
    if (tls.slot < 0)
        return;

    // 关键：一律拷进**本线程**的定长缓冲再发布指针。
    //   绝不能直接保存调用方指针 —— 阶段名可能来自模块成员（std::string），
    //   模块析构后看门狗再读就是悬垂指针（诊断代码反而把程序搞崩）。
    //   指针未变（同一字符串字面量/同一模块的常驻成员）时走快路径跳过拷贝。
    if (phase == tls.lastPhasePtr)
        return;

    std::strncpy (tls.phaseBuf, phase, sizeof (tls.phaseBuf) - 1);
    tls.phaseBuf[sizeof (tls.phaseBuf) - 1] = 0;
    tls.lastPhasePtr = phase;

    core().threads[tls.slot].phase.store (tls.phaseBuf);
}

void setPhase (const juce::String& phase) noexcept
{
    if (! isEnabled())
        return;

    if (tls.slot < 0)
        tls.slot = claimThreadSlot();
    if (tls.slot < 0)
        return;

    try
    {
        // 动态字符串：强制拷贝（内容可能每次不同，不能走指针快路径）
        std::strncpy (tls.phaseBuf, phase.toRawUTF8(), sizeof (tls.phaseBuf) - 1);
        tls.phaseBuf[sizeof (tls.phaseBuf) - 1] = 0;
        tls.lastPhasePtr = nullptr;
        core().threads[tls.slot].phase.store (tls.phaseBuf);
    }
    catch (...)
    {
    }
}

// -----------------------------------------------------------------------------
// 心跳
// -----------------------------------------------------------------------------
void setChannelActive (Channel c, bool active) noexcept
{
    const int idx = (int) c;
    if (idx < 0 || idx >= kChannelCount)
        return;

    const bool was = core().channelActive[idx].exchange (active);
    // 由 idle → active 时刷新基准，避免刚激活就误判
    if (active && ! was)
        core().lastBeat[idx].store (nowMs());
}

void heartbeat (Channel c) noexcept
{
    if (! isEnabled())
        return;

    const int idx = (int) c;
    if (idx < 0 || idx >= kChannelCount)
        return;

    auto& coreRef = core();
    coreRef.lastBeat[idx].store (nowMs(), std::memory_order_relaxed);

    if (c == Channel::UiMessage)
        coreRef.uiEverBeat.store (true, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// 状态镜像
// -----------------------------------------------------------------------------
void setEditorState (int logicalW, int logicalH, float dpiScale,
                     bool glAttached, bool glContextCreated,
                     bool milkdropReady, int moduleCount,
                     juce::int64 glFrames) noexcept
{
    if (! isEnabled())
        return;

    auto& c = core();
    c.edW.store (logicalW);
    c.edH.store (logicalH);
    c.edDpi100.store ((int) (dpiScale * 100.0f + 0.5f));
    c.edAttached.store (glAttached);
    c.edCreated.store (glContextCreated);
    c.edMilkdrop.store (milkdropReady);
    c.edModules.store (moduleCount);
    c.edGlFrames.store (glFrames);
}

void setGlInfo (const juce::String& info)
{
    if (! isEnabled())
        return;

    {
        auto& c = core();
        const juce::ScopedLock sl (c.ioLock);
        c.glInfo = info;
    }
    writeCritical ("GL", "context info: " + info);
}

void setGlAttached (bool attached) noexcept
{
    if (! isEnabled())
        return;

    auto& c = core();
    if (attached)
    {
        if (c.glAttachedAt.load() <= 0.0)
            c.glAttachedAt.store (nowMs());
    }
    else
    {
        c.glAttachedAt.store (0.0);
        c.glCreatedFlag.store (false);
        c.glAttachWarned.store (false);
    }
}

void markGlContextCreated() noexcept
{
    if (! isEnabled())
        return;

    auto& c = core();
    c.glCreatedFlag.store (true);
    c.glAttachWarned.store (false);
}

void addFrameSample (double totalMs, double milkdropMs, double directMs) noexcept
{
    if (! isEnabled())
        return;

    auto& c = core();
    c.frames.fetch_add (1, std::memory_order_relaxed);

    const auto totalUs = (juce::int64) (juce::jmax (0.0, totalMs) * 1000.0);
    c.frameUsSum.fetch_add (totalUs, std::memory_order_relaxed);
    c.milkUsSum.fetch_add ((juce::int64) (juce::jmax (0.0, milkdropMs) * 1000.0), std::memory_order_relaxed);
    c.directUsSum.fetch_add ((juce::int64) (juce::jmax (0.0, directMs) * 1000.0), std::memory_order_relaxed);

    auto prevMax = c.frameUsMax.load (std::memory_order_relaxed);
    while (totalUs > prevMax
           && ! c.frameUsMax.compare_exchange_weak (prevMax, totalUs, std::memory_order_relaxed))
    {
    }
}

void addUiTick() noexcept
{
    if (isEnabled())
        core().uiTicks.fetch_add (1, std::memory_order_relaxed);
}

void addDispatchTick() noexcept
{
    if (isEnabled())
        core().dispatchTicks.fetch_add (1, std::memory_order_relaxed);
}

void setModuleStateProvider (juce::String (*fn)()) noexcept
{
    core().moduleProvider.store (fn);
}

void reportNow (const char* reason)
{
    if (! isEnabled())
        return;

    emitStallReport (reason != nullptr ? reason : "manual");
}

// -----------------------------------------------------------------------------
// ScopedTimer
// -----------------------------------------------------------------------------
ScopedTimer::ScopedTimer (const char* tag, const char* what, double slowThresholdMs) noexcept
{
    if (! isEnabled())
        return;

    tag_ = tag;
    what_ = what;
    thresholdMs_ = slowThresholdMs;
    startMs_ = nowMs();
    armed_ = true;
}

ScopedTimer::~ScopedTimer()
{
    if (! armed_)
        return;

    const double elapsed = nowMs() - startMs_;
    if (elapsed >= thresholdMs_)
    {
        try
        {
            write (tag_ != nullptr ? tag_ : "SLOW",
                   "SLOW " + juce::String (what_ != nullptr ? what_ : "?")
                       + " " + juce::String (elapsed, 1) + "ms");
        }
        catch (...)
        {
        }
    }
}

} // namespace y2k::diag
