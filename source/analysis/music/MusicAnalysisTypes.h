#pragma once

// ==========================================================
// MusicAnalysisTypes —— 节拍（BPM）/ 调性（Key）分析的结果数据结构
//
// 设计约束：
//   1. 纯 C++（不 include JUCE），这样 tests/music_analysis_test.cpp 可以
//      直接链接 DSP 核心做单元测试，不需要 JUCE 运行时。
//   2. 只放"结果"与"状态"，不放任何 DSP 状态。
//   3. 置信度字段是**可解释的稳定性指标**（0..1），不是标定过的概率：
//      计算公式写在 TempoEstimator::updateConfidence() /
//      KeyEstimator::updateKey() 里，UI 只做展示，不做二次编造。
//
// 线程模型：
//   后台分析线程写 → MusicAnalysisEngine 用 SpinLock 发布一份 Result 拷贝
//   → UI 线程（AnalyserHub::FrameDispatcher）整体拷进 FrameSnapshot 读取。
// ==========================================================

namespace y2k::music
{

enum class Status
{
    idle = 0,   // 用户点了 STOP（分析未运行）
    analysing,  // 正在采集，数据不足或结果尚未稳定
    tracking,   // BPM 与调性都已有稳定结果
    noSignal    // 输入为静音 / 电平过低，当前没有可用信号
};

// 调式：大调 / 自然小调 / 和声小调 / 旋律小调（上行）
enum class Mode
{
    major = 0,
    naturalMinor,
    harmonicMinor,
    melodicMinor
};

inline bool isMinorMode (Mode m) noexcept
{
    return m != Mode::major;
}

inline const char* modeName (Mode m) noexcept
{
    switch (m)
    {
        case Mode::major:         return "major";
        case Mode::naturalMinor:  return "natural minor";
        case Mode::harmonicMinor: return "harmonic minor";
        case Mode::melodicMinor:  return "melodic minor";
    }

    return "major";
}

// 只取调式限定词（"natural" / "harmonic" / "melodic"），大调返回空串
inline const char* modeQualifier (Mode m) noexcept
{
    switch (m)
    {
        case Mode::major:         return "";
        case Mode::naturalMinor:  return "natural";
        case Mode::harmonicMinor: return "harmonic";
        case Mode::melodicMinor:  return "melodic";
    }

    return "";
}

struct Result
{
    Status status = Status::idle;

    // ---- BPM ----
    float bpm      = 0.0f;   // 仅在 bpmValid == true 时具有意义
    bool  bpmValid = false;

    // ---- Key ----
    int   keyRoot  = -1;     // 0 = C, 1 = C#, ... 11 = B；-1 = 尚无结果
    Mode  keyMode  = Mode::major;
    bool  keyValid = false;  // 当前滑动窗口是否仍支持这个调性

    // ---- 调音基准（K1）----
    //   A4 相对 440 Hz 的偏移，单位音分（cent），范围约 [-50, +50]。
    //   0 = 标准音高。仅用于诊断显示，不参与 BPM / Key 判定。
    float tuningCents = 0.0f;

    // ---- 置信度（0..1，启发式稳定性指标，不是概率）----
    //   BPM：梳状自相关显著度、候选离散度、最佳/次佳峰间隔、分析时长
    //   调性：KK 轮廓相关度、与"不同主音"候选的差距、窗口覆盖时长
    //   特意乘了"数据充分度"系数，所以刚开始听的时候数值天然偏低。
    float bpmConfidence = 0.0f;
    float keyConfidence = 0.0f;

    // ---- 诊断 / 显示 ----
    float  levelDb         = -100.0f;  // 当前分析帧电平（dBFS，未加权）
    double bpmUpdatedMs    = 0.0;      // BPM 数值最后一次变化的时间（wall clock）
    double keyUpdatedMs    = 0.0;      // 调性最后一次变化的时间（wall clock）
    double publishedMs     = 0.0;      // 本次结果发布时间（wall clock）
    double analysedSeconds = 0.0;      // 自 START 以来实际分析过的音频时长
};

// ==========================================================
// 调性拼写 / 关系调（显示用，不影响识别）
//
// 拼写：同一组音在不同调号下写法不同（F# / Gb）。这里只决定"怎么称呼"，
//      主写法取调号更简单、更常用的那个（Db 而不是 C#，Eb 而不是 D#）。
//
// 关系调（relative key）：与当前调共享**完全相同的一组音**、只是主音不同的调。
//   · 大调     → 关系小调（主音 +9 半音），例如 C major ↔ A minor
//   · 自然小调 → 关系大调（主音 +3 半音）
//   · 和声/旋律小调没有关系调（音级集合与任何大调/自然小调都不同）
//   关系调的拼写跟随主调的调号侧：Eb minor → Gb major，而不是 F# major。
// ==========================================================
struct KeySpelling
{
    const char* name     = nullptr;  // 常用写法，正常调性下非空
    bool        flatSide = false;    // 该写法属于降号侧（用于关系调拼写一致）
};

inline KeySpelling keySpelling (int root, Mode mode) noexcept
{
    //                         C      C#     D      D#     E     F      F#     G      G#     A      A#     B
    static const char* majorName[12] =
        { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    static const bool majorFlat[12] =
        { false, true, false, true, false, true, false, false, true, false, true, false };

    static const char* minorName[12] =
        { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };
    static const bool minorFlat[12] =
        { true, false, true, true, false, true, false, true, false, false, true, false };

    KeySpelling out;

    if (root < 0 || root > 11)
        return out;

    const bool minor = isMinorMode (mode);

    out.name     = minor ? minorName[root] : majorName[root];
    out.flatSide = minor ? minorFlat[root] : majorFlat[root];

    return out;
}

// 按调号侧取拼写（升号侧 / 降号侧）。只在"关系调"里用，
// 保证 Eb minor 的关系大调写成 Gb major 而不是 F# major。
inline const char* keyNameForSide (int root, Mode mode, bool flatSide) noexcept
{
    static const char* majorSharp[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* majorFlat[12] =
        { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };

    static const char* minorSharp[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* minorFlat[12] =
        { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };

    if (root < 0 || root > 11)
        return nullptr;

    const bool minor = isMinorMode (mode);

    if (minor)
        return flatSide ? minorFlat[root] : minorSharp[root];

    return flatSide ? majorFlat[root] : majorSharp[root];
}

struct RelativeKey
{
    const char* name  = nullptr;  // 关系调主音名；nullptr = 该调式没有关系调
    bool        minor = false;
    int         root  = -1;
};

inline RelativeKey relativeKeyOf (int root, Mode mode) noexcept
{
    RelativeKey out;

    if (root < 0 || root > 11)
        return out;

    if (mode == Mode::major)
    {
        out.root  = (root + 9) % 12;   // 大调 → 关系小调
        out.minor = true;
    }
    else if (mode == Mode::naturalMinor)
    {
        out.root  = (root + 3) % 12;   // 自然小调 → 关系大调
        out.minor = false;
    }
    else
    {
        return out;                    // 和声/旋律小调：无关系调
    }

    const bool flatSide = keySpelling (root, mode).flatSide;

    out.name = keyNameForSide (out.root, out.minor ? Mode::naturalMinor : Mode::major,
                               flatSide);
    return out;
}

inline const char* statusText (Status s) noexcept
{
    switch (s)
    {
        case Status::idle:      return "STOPPED";
        case Status::analysing: return "ANALYSING";
        case Status::tracking:  return "TRACKING";
        case Status::noSignal:  return "NO SIGNAL";
    }

    return "STOPPED";
}

} // namespace y2k::music
