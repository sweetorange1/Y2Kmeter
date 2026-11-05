// =============================================================================
// source/diag/Y2KDiagLog.h
//   运行时诊断日志（现场排查用）—— 与 Y2KLogging.h 的区别：
//
//   · Y2KLogging.h 的 Y2K_LOG 只在 RelWithDebInfo 编译期开启，正式发布包
//     完全不含日志代码。用户现场出问题时**无法**让用户自己产日志。
//   · 本模块全构建可用、**运行时开关**：
//       - 编译期默认开启：-DY2K_DIAG_LOG_DEFAULT_ON=ON（发给用户的诊断包）
//       - 环境变量 Y2K_DIAG_LOG=1（0/off 可强制关闭）
//       - 标记文件 <exe目录>/Y2Kmeter_diag_on.txt 或
//         <用户数据目录>/Y2Kmeter/diag_on.txt 存在
//     未开启时每个宏只做一次 relaxed 原子读并跳过参数求值，
//     字符串拼接/构造全部不发生（等同零开销）。
//
//   主要能力：
//     1. 落盘：<桌面>/Y2Kmeter_Diag/Y2Kmeter_diag_<时间戳>.log
//        （桌面不可写时回退到文档/用户数据目录），自动写 README 说明；
//     2. 线程阶段标记（setPhase）——卡死时能直接看到"卡在哪个函数/哪次 GL 调用"；
//     3. 心跳通道 + 看门狗线程 —— 某条线程长时间不心跳时自动输出
//        STALL 报告（各线程阶段、Editor/模块状态、5s 统计），
//        并在 Windows 上额外落一份 Minidump 供事后用 PDB 看调用栈；
//     4. 环境信息（OS/CPU/内存/显示器/DPI/远程桌面/GL 版本）；
//     5. exportSnapshot()：随时导出"当前状态快照"txt（快捷键 Ctrl+Shift+D）。
//
//   编码约定：
//     · 宏参数必须是纯表达式，禁止带副作用（关闭时整段不求值）；
//     · 高频路径（每帧/每个音频块）只允许 heartbeat / setPhase 这类原子写，
//       禁止直接 write()（会拼字符串 + 落盘）。
// =============================================================================
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

namespace y2k::diag
{
    // =========================================================================
    // 开关 / 生命周期
    // =========================================================================
    bool isEnabled() noexcept;      // 一次 relaxed 原子读
    bool isVerbose() noexcept;      // Y2K_DIAG_VERBOSE=1

    // 幂等；可在任意线程最早期调用（内部自行判断开关，关闭时不做任何事）
    void ensureInitialised() noexcept;
    // 刷盘 + 写会话结束行（可重复调用；之后日志仍可继续写）
    void shutdown() noexcept;

    juce::File   getLogDirectory();
    juce::File   getLogFile();
    juce::String exportSnapshot();  // 写一份状态快照，返回文件路径（失败返回空）

    // =========================================================================
    // 写日志
    // =========================================================================
    // 普通行（进缓冲，看门狗每 250ms 刷盘）
    // 关键行（立即 flush；用于上下文创建/销毁、STALL 报告等"崩了也要留下"的事件）
    //
    // ⚠️ 为什么每个函数都有两个重载：
    //   juce::String 的 `String (const char*)` 构造把 8-bit 数据当 **ASCII** 处理
    //   （见 juce_String.cpp 里 CharPointer_ASCII 那条），源码里的中文（UTF-8）
    //   会被逐字节当成独立码点，再按 UTF-8 重新编码 → 日志里全是 "åä¸æ" 这种乱码。
    //   而 `operator<< (String&, const char*)` 走的是 UTF-8 路径，所以只有
    //   "字面量隐式转 String" 这一条链有问题。
    //   → 字面量走 const char* 重载（内部按 UTF-8 解析），拼接表达式走 String 重载。
    void write (const char* tag, const char* utf8Message);
    void write (const char* tag, const juce::String& message);
    void writeCritical (const char* tag, const char* utf8Message);
    void writeCritical (const char* tag, const juce::String& message);

    // 把源码里的 UTF-8 字面量安全转成 juce::String。
    //   只用于"中文必须起头"的拼接：`"中文" + juce::String(x)` 会走
    //   operator+ (const char*, const String&) → String(const char*) → 乱码；
    //   写成 `u8("中文") + juce::String(x)` 即正确。
    inline juce::String u8 (const char* utf8Text)
    {
        return utf8Text != nullptr ? juce::String (juce::CharPointer_UTF8 (utf8Text))
                                   : juce::String();
    }

    // =========================================================================
    // 线程标记（卡死报告用）
    // =========================================================================
    void setThreadTag (const char* tag) noexcept;        // "UI" / "GL" / "AUDIO"
    void setPhase (const char* phase) noexcept;           // 字符串字面量
    void setPhase (const juce::String& phase) noexcept;   // 动态名（内部拷进 TLS 定长缓冲）

    // =========================================================================
    // 心跳通道
    // =========================================================================
    enum class Channel { GlFrame = 0, UiMessage, Dispatch, Audio, Count };

    // active=false 时该通道不参与卡死判定（如窗口最小化、GL 已 detach）
    void setChannelActive (Channel c, bool active) noexcept;
    void heartbeat (Channel c) noexcept;

    // =========================================================================
    // 状态镜像（只写原子量；看门狗线程只读，避免跨线程碰组件树）
    // =========================================================================
    void setEditorState (int logicalW, int logicalH, float dpiScale,
                         bool glAttached, bool glContextCreated,
                         bool milkdropReady, int moduleCount,
                         juce::int64 glFrames) noexcept;

    void setGlInfo (const juce::String& info);   // GL_VENDOR/RENDERER/VERSION 等

    // 上下文建立过程监测：attach 之后长时间没有 newOpenGLContextCreated，
    // 说明驱动/窗口系统在创建 GL 上下文时卡住（这是"界面全黑/全卡"的典型现场）。
    void setGlAttached (bool attached) noexcept;
    void markGlContextCreated() noexcept;

    // 每帧采样（GL 线程调用，纯原子累加，不落盘）
    void addFrameSample (double totalMs, double milkdropMs, double directMs) noexcept;
    void addUiTick() noexcept;
    void addDispatchTick() noexcept;

    // 直渲染模块状态提供者（GlDirectModuleBase 注册；内部只读原子量）
    void setModuleStateProvider (juce::String (*fn)()) noexcept;

    // 手动触发一次完整报告（快捷键 / 用户反馈"卡了一下"）
    void reportNow (const char* reason);

    // =========================================================================
    // 计时器：只在超过阈值时写一行（避免逐帧刷屏）
    // =========================================================================
    class ScopedTimer
    {
    public:
        ScopedTimer (const char* tag, const char* what, double slowThresholdMs = 0.0) noexcept;
        ~ScopedTimer();

        ScopedTimer (const ScopedTimer&) = delete;
        ScopedTimer& operator= (const ScopedTimer&) = delete;

    private:
        const char* tag_ = nullptr;
        const char* what_ = nullptr;
        double startMs_ = 0.0;
        double thresholdMs_ = 0.0;
        bool   armed_ = false;
    };
}

// =============================================================================
// 宏
// =============================================================================
#define Y2K_DIAG_ENABLED() (::y2k::diag::isEnabled())

// 普通日志：未开启时 msg 表达式完全不求值
#define Y2K_DIAG(tag, msg) \
    do { if (::y2k::diag::isEnabled()) ::y2k::diag::write ((tag), (msg)); } while (false)

// 关键日志：立即刷盘
#define Y2K_DIAGC(tag, msg) \
    do { if (::y2k::diag::isEnabled()) ::y2k::diag::writeCritical ((tag), (msg)); } while (false)

// 阶段标记（卡死报告会打印每个线程最后一次设置的阶段）
#define Y2K_DIAG_PHASE(phase) \
    do { if (::y2k::diag::isEnabled()) ::y2k::diag::setPhase (phase); } while (false)

#define Y2K_DIAG_CONCAT_INNER(a, b) a##b
#define Y2K_DIAG_CONCAT(a, b)       Y2K_DIAG_CONCAT_INNER(a, b)

// 慢操作计时：超过 thresholdMs 才写一行 "SLOW ..."
#define Y2K_DIAG_SLOW(tag, what, thresholdMs) \
    ::y2k::diag::ScopedTimer Y2K_DIAG_CONCAT (y2kDiagTimer_, __LINE__) ((tag), (what), (thresholdMs))
