// ==========================================================
// music_analysis_test —— 节拍 / 调性 DSP 核心的离线验证
//
// 覆盖范围（全部为合成信号，不依赖 JUCE、不依赖音频设备）：
//   1. 已知速度的点击音轨 → BPM 精度（多个采样率 / 多个输入块长）
//   2. 静音 → 不输出 BPM
//   3. 启动阶段 → 数据不足时不提前给出结果
//   4. 中途变速 → 稳定后跟随新速度
//   5. 已知调性的和弦进行 → 调性（允许关系大小调歧义，见下方说明）
//   6. 中途转调 → 窗口滑出旧调性后跟随新调性
//   7. 长时间运行 → 滑动窗口帧数有界（内存不随时间增长）
//   8. reset / resetAccumulation 语义
//
// 明确不做的：
//   · 纯点击音轨不用于验证调性（点击没有调性内容）；
//   · 不做准确率统计（样本是合成信号，不能代表真实混音）。
//
// 运行：直接执行可执行文件，全部通过时打印 ALL_OK（返回 0）。
// ==========================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "source/analysis/music/KeyEstimator.h"
#include "source/analysis/music/MusicAnalysisTypes.h"
#include "source/analysis/music/TempoEstimator.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;

void check (bool condition, const std::string& what)
{
    if (! condition)
    {
        std::printf ("  FAIL: %s\n", what.c_str());
        ++gFailures;
    }
}

void checkNear (double value, double target, double tolerance, const std::string& what)
{
    const bool ok = std::abs (value - target) <= tolerance;

    if (! ok)
    {
        std::printf ("  FAIL: %s (got %.3f, expected %.3f ± %.3f)\n",
                     what.c_str(), value, target, tolerance);
        ++gFailures;
    }
}

double midiToHz (int midi) noexcept
{
    return 440.0 * std::pow (2.0, (midi - 69) / 12.0);
}

bool isRelativePair (int rootA, bool minorA, int rootB, bool minorB) noexcept
{
    if (rootA < 0 || rootB < 0 || minorA == minorB)
        return false;

    const int majorRoot = minorA ? rootB : rootA;
    const int minorRoot = minorA ? rootA : rootB;

    return ((majorRoot + 9) % 12) == minorRoot;
}

// 显示用：按调式选常用拼写（大调 Db 而不是 C#，小调 Eb 而不是 D#）
const char* primaryKeyName (int root, y2k::music::Mode mode) noexcept
{
    const auto sp = y2k::music::keySpelling (root, mode);
    return sp.name != nullptr ? sp.name : "--";
}

// ----------------------------------------------------------
// 合成：点击音轨（30ms 衰减正弦，周期性出现）
// ----------------------------------------------------------
std::vector<float> makeClickTrack (double sampleRate, double bpm, double seconds)
{
    const int total = (int) (sampleRate * seconds);
    std::vector<float> out ((size_t) total, 0.0f);

    const double period = 60.0 / bpm;
    const int clickLen  = (int) (0.03 * sampleRate);

    for (double t = 0.05; t < seconds - 0.1; t += period)
    {
        const int start = (int) (t * sampleRate);

        for (int i = 0; i < clickLen && start + i < total; ++i)
        {
            const double tt = (double) i / sampleRate;
            const double env = std::exp (-tt * 70.0);
            out[(size_t) (start + i)] += (float) (0.7 * env * std::sin (2.0 * kPi * 1000.0 * tt));
        }
    }

    return out;
}

// ----------------------------------------------------------
// 合成：和弦进行（正弦 + 2/3 次谐波 + 低八度根音）
// ----------------------------------------------------------
void addTone (std::vector<float>& buffer, double sampleRate,
              double startSec, double durSec, double freq, float amp)
{
    const int start = (int) (startSec * sampleRate);
    const int len   = (int) (durSec * sampleRate);

    for (int i = 0; i < len; ++i)
    {
        const int idx = start + i;

        if (idx < 0 || idx >= (int) buffer.size())
            break;

        const double t = (double) i / sampleRate;
        const double attack  = std::min (1.0, t / 0.02);
        const double release = std::min (1.0, (durSec - t) / 0.08);
        const double env = attack * release;

        const double v = std::sin (2.0 * kPi * freq * t)
                       + 0.40 * std::sin (2.0 * kPi * 2.0 * freq * t)
                       + 0.20 * std::sin (2.0 * kPi * 3.0 * freq * t);

        buffer[(size_t) idx] += (float) (amp * env * v);
    }
}

// ----------------------------------------------------------
// 合成：富谐波音符（模拟真实乐器 / 合成器，1/h 谐波列）
//   用来验证 K3 谐波感知 chroma：纯正弦的测试素材频谱太干净，
//   测不出谐波泄漏问题。
// ----------------------------------------------------------
void addRichTone (std::vector<float>& buffer, double sampleRate,
                  double startSec, double durSec, double freq, float amp,
                  int harmonics = 8)
{
    const int start = (int) (startSec * sampleRate);
    const int len   = (int) (durSec * sampleRate);

    for (int i = 0; i < len; ++i)
    {
        const int idx = start + i;

        if (idx < 0 || idx >= (int) buffer.size())
            break;

        const double t = (double) i / sampleRate;
        const double attack  = std::min (1.0, t / 0.02);
        const double release = std::min (1.0, (durSec - t) / 0.08);
        const double env = attack * release;

        double v = 0.0;

        for (int h = 1; h <= harmonics; ++h)
        {
            const double fh = freq * (double) h;

            if (fh > sampleRate * 0.45)
                break;

            v += std::sin (2.0 * kPi * fh * t) / (double) h;
        }

        buffer[(size_t) idx] += (float) (amp * env * v * 0.5);
    }
}

// 一个"主音和弦占主导"的 I–IV–V–I 进行（小调用 i–iv–v–i）
//   majorDominant = true 时，属和弦（degree 7）改用大三和弦 —— 这是和声小调
//   最典型的特征（A 小调里的 E 大三和弦带 G#）。
std::vector<float> makeProgression (double sampleRate, int rootPc, bool minor, double seconds,
                                    bool majorDominant = false, double centsOffset = 0.0)
{
    const int total = (int) (sampleRate * seconds);
    std::vector<float> out ((size_t) total, 0.0f);

    // 整体调音偏移（验证 K1 调音基准检测）
    const double tuningFactor = std::pow (2.0, centsOffset / 1200.0);

    const int rootMidi = 60 + rootPc;   // C4 附近

    struct Step { int degree; double dur; };
    const Step pattern[4] = { { 0, 2.0 }, { 5, 1.5 }, { 7, 1.5 }, { 0, 2.0 } };

    double t = 0.0;

    while (t < seconds - 0.1)
    {
        for (const auto& step : pattern)
        {
            if (t >= seconds - 0.1)
                break;

            const int chordRoot = rootMidi + step.degree;
            const int third     = (minor && ! (majorDominant && step.degree == 7)) ? 3 : 4;

            addTone (out, sampleRate, t, step.dur, midiToHz (chordRoot - 12) * tuningFactor, 0.16f);
            addTone (out, sampleRate, t, step.dur, midiToHz (chordRoot) * tuningFactor,      0.14f);
            addTone (out, sampleRate, t, step.dur, midiToHz (chordRoot + third) * tuningFactor, 0.12f);
            addTone (out, sampleRate, t, step.dur, midiToHz (chordRoot + 7) * tuningFactor,  0.12f);

            t += step.dur;
        }
    }

    return out;
}

void feedTempo (y2k::music::TempoEstimator& est, const std::vector<float>& audio, int blockSize)
{
    for (size_t i = 0; i < audio.size();)
    {
        const int n = std::min (blockSize, (int) (audio.size() - i));
        est.pushAudio (audio.data() + i, n);
        i += (size_t) n;
    }
}

void feedKey (y2k::music::KeyEstimator& est, const std::vector<float>& audio, int blockSize)
{
    for (size_t i = 0; i < audio.size();)
    {
        const int n = std::min (blockSize, (int) (audio.size() - i));
        est.pushAudio (audio.data() + i, n);
        i += (size_t) n;
    }
}

// ----------------------------------------------------------
// 1. BPM 精度
// ----------------------------------------------------------
void testTempoAccuracy()
{
    std::printf ("[1] BPM accuracy\n");

    struct Case { double sr; double bpm; int block; };
    const Case cases[] =
    {
        { 44100.0, 120.0,  512 },
        { 48000.0,  90.0, 1024 },
        { 96000.0, 140.0, 4096 },
        { 44100.0, 140.0,   64 },
        { 44100.0,  90.0, 4096 },
        { 48000.0, 120.0,   37 },
        { 48000.0, 120.0, 4096 },
    };

    for (const auto& c : cases)
    {
        y2k::music::TempoEstimator est;
        est.prepare (c.sr);

        const auto audio = makeClickTrack (c.sr, c.bpm, 16.0);
        feedTempo (est, audio, c.block);

        const std::string tag = "sr=" + std::to_string ((int) c.sr)
                              + " block=" + std::to_string (c.block)
                              + " target=" + std::to_string ((int) c.bpm);

        check (est.hasTempo(), "no BPM published: " + tag);

        if (est.hasTempo())
        {
            std::printf ("  %s -> %.2f BPM\n", tag.c_str(), est.bpm());
            checkNear (est.bpm(), c.bpm, c.bpm * 0.03, "BPM out of tolerance: " + tag);
        }
    }
}

// ----------------------------------------------------------
// 2. 静音
// ----------------------------------------------------------
void testSilence()
{
    std::printf ("[2] Silence\n");

    y2k::music::TempoEstimator est;
    est.prepare (48000.0);

    std::vector<float> silence ((size_t) (48000 * 10), 0.0f);
    feedTempo (est, silence, 480);

    check (! est.hasTempo(), "BPM published on silence");
    check (est.currentLevelDb() < -55.0f, "level not reported as silence");
}

// ----------------------------------------------------------
// 3. 启动阶段不提前出结果
// ----------------------------------------------------------
void testStartupDelay()
{
    std::printf ("[3] Startup delay\n");

    y2k::music::TempoEstimator est;
    est.prepare (48000.0);

    const auto audio = makeClickTrack (48000.0, 120.0, 3.0);
    feedTempo (est, audio, 512);

    check (! est.hasTempo(), "BPM published before enough audio was analysed");
}

// ----------------------------------------------------------
// 4. 中途变速
// ----------------------------------------------------------
void testTempoChange()
{
    std::printf ("[4] Tempo change (90 -> 140)\n");

    y2k::music::TempoEstimator est;
    est.prepare (48000.0);

    const auto first  = makeClickTrack (48000.0, 90.0, 10.0);
    const auto second = makeClickTrack (48000.0, 140.0, 22.0);

    feedTempo (est, first, 512);
    feedTempo (est, second, 512);

    check (est.hasTempo(), "no BPM after tempo change");

    if (est.hasTempo())
    {
        std::printf ("  final -> %.2f BPM (expected 140)\n", est.bpm());
        checkNear (est.bpm(), 140.0, 4.0, "BPM did not follow the tempo change");
    }
}

// ----------------------------------------------------------
// 5. 已知调性
// ----------------------------------------------------------
void testKeyDetection()
{
    std::printf ("[5] Key detection\n");

    struct Case { int rootPc; bool minor; const char* name; };
    const Case cases[] =
    {
        { 0, false, "C major"  },
        { 6, false, "F# major" },
        { 9, true,  "A minor"  },
    };

    for (const auto& c : cases)
    {
        y2k::music::KeyEstimator est;
        est.prepare (44100.0);

        const auto audio = makeProgression (44100.0, c.rootPc, c.minor, 26.0);
        feedKey (est, audio, 1024);

        const std::string tag = std::string ("target=") + c.name;

        check (est.hasKey(), "no key published: " + tag);

        if (! est.hasKey())
            continue;

        std::printf ("  %s -> %s %s (r=%.2f, conf=%.2f, tuning=%+.0fc)\n", tag.c_str(),
                     primaryKeyName (est.root(), est.mode()),
                     y2k::music::modeName (est.mode()),
                     (double) est.lastScore(),
                     (double) est.confidence(),
                     (double) est.tuningCents());

        const bool exact = (est.root() == c.rootPc && est.minor() == c.minor);

        // 关系大小调（C major / A minor 等）的 KK 轮廓高度相关，是公认歧义；
        // 这里允许"精确命中"或"命中其关系调"，但绝不允许真正错误的调性。
        const bool relative = isRelativePair (c.rootPc, c.minor, est.root(), est.minor());

        // 本次改造的量化指标：三个已知调性必须精确命中（不再接受关系调歧义）
        check (exact, "wrong key: " + tag);
    }
}

// ----------------------------------------------------------
// 6. 中途转调
// ----------------------------------------------------------
void testKeyChange()
{
    std::printf ("[6] Key change (C major -> F# major)\n");

    y2k::music::KeyEstimator est;
    est.prepare (44100.0);

    const auto before = makeProgression (44100.0, 0, false, 22.0);
    const auto after  = makeProgression (44100.0, 6, false, 26.0);

    feedKey (est, before, 1024);
    feedKey (est, after, 1024);

    check (est.hasKey(), "no key after modulation");

    if (est.hasKey())
    {
        std::printf ("  final -> %s %s (r=%.2f, conf=%.2f)\n",
                     primaryKeyName (est.root(), est.mode()),
                     y2k::music::modeName (est.mode()),
                     (double) est.lastScore(),
                     (double) est.confidence());

        const bool exact    = (est.root() == 6 && ! est.minor());
        const bool relative = isRelativePair (6, false, est.root(), est.minor());

        check (exact || relative, "key did not follow the modulation");
    }
}

// ----------------------------------------------------------
// 7. 长时间运行：窗口帧数有界
// ----------------------------------------------------------
void testLongRunBoundedMemory()
{
    std::printf ("[7] Long run (4 min @ 8 kHz) — window stays bounded\n");

    y2k::music::KeyEstimator est;
    est.prepare (8000.0);

    const auto chunk = makeProgression (8000.0, 0, false, 20.0);

    int framesAfterOneMinute = 0;

    for (int minute = 0; minute < 4; ++minute)
    {
        for (int i = 0; i < 3; ++i)   // 3 × 20s = 60s
            feedKey (est, chunk, 333);

        if (minute == 0)
            framesAfterOneMinute = est.debugFrameCount();
    }

    const int finalFrames = est.debugFrameCount();

    std::printf ("  frames after 1 min = %d, after 4 min = %d (capacity %d)\n",
                 framesAfterOneMinute, finalFrames, est.debugFrameCapacity());

    check (finalFrames <= est.debugFrameCapacity(), "chroma window exceeded its capacity");

    // 窗口按 updateIntervalSec(=1.5s) 的节拍裁剪，因此帧数会在一个更新周期内
    //   上下波动（不是随时间增长）。这里允许一个更新周期的余量（约 32 帧 @8k），
    //   仍然能证明"4 分钟不增长"。
    check (finalFrames <= framesAfterOneMinute + 32, "chroma window keeps growing over time");
}

// ----------------------------------------------------------
// 8. reset 语义
// ----------------------------------------------------------
void testReset()
{
    std::printf ("[8] Reset\n");

    y2k::music::TempoEstimator tempo;
    tempo.prepare (48000.0);

    const auto audio = makeClickTrack (48000.0, 120.0, 14.0);
    feedTempo (tempo, audio, 512);

    check (tempo.hasTempo(), "no BPM before reset");

    tempo.reset();
    check (! tempo.hasTempo(), "BPM survived reset()");

    const auto shortAudio = makeClickTrack (48000.0, 120.0, 2.0);
    feedTempo (tempo, shortAudio, 512);
    check (! tempo.hasTempo(), "BPM republished too early after reset()");

    y2k::music::KeyEstimator key;
    key.prepare (44100.0);

    const auto chords = makeProgression (44100.0, 0, false, 26.0);
    feedKey (key, chords, 1024);

    check (key.hasKey(), "no key before reset");

    key.resetAccumulation();
    check (! key.hasKey(), "key survived resetAccumulation()");
    check (key.debugFrameCount() == 0, "chroma frames survived resetAccumulation()");
}

// ----------------------------------------------------------
// 9. 置信度：刚开始听时低，听得久之后升高
// ----------------------------------------------------------
void testConfidenceRamp()
{
    std::printf ("[9] Confidence ramps with listening time\n");

    y2k::music::TempoEstimator tempo;
    tempo.prepare (48000.0);

    feedTempo (tempo, makeClickTrack (48000.0, 120.0, 9.0), 512);

    check (tempo.hasTempo(), "no BPM at 9s");

    const float bpmEarly = tempo.confidence();

    feedTempo (tempo, makeClickTrack (48000.0, 120.0, 21.0), 512);

    const float bpmLate = tempo.confidence();

    std::printf ("  BPM confidence @9s = %.2f, @30s = %.2f\n",
                 (double) bpmEarly, (double) bpmLate);

    check (bpmEarly < 0.80f, "early BPM confidence is unrealistically high");
    check (bpmLate > bpmEarly + 0.05f, "BPM confidence did not grow with more audio");
    check (bpmLate > 0.50f, "BPM confidence stayed low after 30s");

    y2k::music::KeyEstimator key;
    key.prepare (44100.0);

    feedKey (key, makeProgression (44100.0, 0, false, 14.0), 1024);

    check (key.hasKey(), "no key at 14s");

    const float keyEarly = key.confidence();

    feedKey (key, makeProgression (44100.0, 0, false, 20.0), 1024);

    const float keyLate = key.confidence();

    std::printf ("  Key confidence @14s = %.2f, @34s = %.2f\n",
                 (double) keyEarly, (double) keyLate);

    check (keyLate > keyEarly + 0.03f, "key confidence did not grow with more audio");
}

// ----------------------------------------------------------
// 10. 小调调式：和声小调（属和弦带升高的 7 级）
// ----------------------------------------------------------
void testMinorModes()
{
    std::printf ("[10] Minor modes (A harmonic minor)\n");

    y2k::music::KeyEstimator est;
    est.prepare (44100.0);

    // i – iv – V（大三，带 G#）– i
    feedKey (est, makeProgression (44100.0, 9, true, 28.0, true), 1024);

    check (est.hasKey(), "no key for A harmonic minor progression");

    if (est.hasKey())
    {
        std::printf ("  -> %s %s\n",
                     primaryKeyName (est.root(), est.mode()),
                     y2k::music::modeName (est.mode()));

        check (est.root() == 9, "root is not A");
        check (y2k::music::isMinorMode (est.mode()), "not classified as a minor mode");

        // 和声小调轮廓由自然小调构造而来，两者只差 6/7 级；
        // 允许保守退化为 natural minor（这是可解释的结果）。
        check (est.mode() == y2k::music::Mode::harmonicMinor
                   || est.mode() == y2k::music::Mode::naturalMinor,
               "unexpected mode for harmonic-minor material");
    }
}

// ----------------------------------------------------------
// 11. 关系调（同一组音、不同主音）
// ----------------------------------------------------------
void testRelativeKeys()
{
    std::printf ("[11] Relative keys\n");

    using y2k::music::Mode;

    struct Case
    {
        int         root;
        Mode        mode;
        const char* expectedName;
        bool        expectedMinor;
    };

    const Case cases[] =
    {
        { 0,  Mode::major,        "A",  true  },  // C major  ↔ A minor
        { 9,  Mode::naturalMinor, "C",  false },  // A minor  ↔ C major
        { 6,  Mode::major,        "D#", true  },  // F# major ↔ D# minor（升号侧）
        { 3,  Mode::naturalMinor, "Gb", false },  // Eb minor ↔ Gb major（降号侧）
        { 10, Mode::major,        "G",  true  },  // Bb major ↔ G minor
        { 11, Mode::major,        "G#", true  },  // B major  ↔ G# minor
        { 1,  Mode::major,        "Bb", true  },  // Db major ↔ Bb minor
    };

    for (const auto& c : cases)
    {
        const auto rel = y2k::music::relativeKeyOf (c.root, c.mode);
        const std::string tag = std::string ("root=")
                              + y2k::music::keySpelling (c.root, c.mode).name
                              + (c.mode == Mode::major ? " major" : " minor");

        check (rel.name != nullptr, "no relative key for " + tag);

        if (rel.name == nullptr)
            continue;

        std::printf ("  %s -> rel. %s %s\n", tag.c_str(), rel.name,
                     rel.minor ? "minor" : "major");

        check (std::string (rel.name) == c.expectedName,
               "wrong relative tonic for " + tag);
        check (rel.minor == c.expectedMinor,
               "wrong relative mode for " + tag);
    }

    // 和声/旋律小调没有关系调：它们的音级集合与任何大调/自然小调都不同
    check (y2k::music::relativeKeyOf (9, Mode::harmonicMinor).name == nullptr,
           "harmonic minor should have no relative key");
    check (y2k::music::relativeKeyOf (9, Mode::melodicMinor).name == nullptr,
           "melodic minor should have no relative key");

    // 主写法修正（上一轮的显示瑕疵）仍然保留
    check (std::string (y2k::music::keySpelling (1, Mode::major).name) == "Db",
           "pc1 major primary should be Db");
    check (std::string (y2k::music::keySpelling (3, Mode::naturalMinor).name) == "Eb",
           "pc3 minor primary should be Eb");
    check (std::string (y2k::music::keySpelling (6, Mode::naturalMinor).name) == "F#",
           "pc6 minor primary should be F#");
}

// ----------------------------------------------------------
// 12. 调音基准检测（K1）
// ----------------------------------------------------------
void testTuningDetection()
{
    std::printf ("[12] Tuning detection (+30 cents)\n");

    y2k::music::KeyEstimator est;
    est.prepare (44100.0);

    // 整体升 30 音分（相当于 A4 ≈ 447.7 Hz）
    const auto audio = makeProgression (44100.0, 0, false, 26.0, false, 30.0);
    feedKey (est, audio, 1024);

    std::printf ("  detected tuning = %+.1f cents (expected +30)\n",
                 (double) est.tuningCents());

    // 10 音分一格，允许 ±2 格误差
    check (std::abs (est.tuningCents() - 30.0f) <= 12.0f,
           "tuning estimate out of range");

    // 调音偏移被测量出来之后，调性判定不应受影响
    check (est.hasKey(), "no key published on detuned material");

    if (est.hasKey())
    {
        std::printf ("  key -> %s (r=%.2f)\n",
                     primaryKeyName (est.root(), est.mode()),
                     (double) est.lastScore());
        check (est.root() == 0 && ! est.minor(), "wrong key on detuned material");
    }
}

// ----------------------------------------------------------
// 13. 富谐波素材的调性（K3 验证）
// ----------------------------------------------------------
void testHarmonicRichKey()
{
    std::printf ("[13] Key with harmonic-rich timbre\n");

    struct Case { int rootPc; bool minor; const char* name; };
    const Case cases[] =
    {
        { 0, false, "C major" },
        { 9, true,  "A minor" },
    };

    for (const auto& c : cases)
    {
        y2k::music::KeyEstimator est;
        est.prepare (44100.0);

        // 手动造和弦：根音 + 三音 + 五音，全部 8 次谐波，并加一个低八度根音
        std::vector<float> audio ((size_t) (44100.0 * 26.0), 0.0f);
        const int rootMidi = 48 + c.rootPc;   // C3 附近

        double t = 0.0;
        const int degrees[4] = { 0, 5, 7, 0 };
        const double durs[4] = { 2.0, 1.5, 1.5, 2.0 };
        const int third = c.minor ? 3 : 4;

        while (t < 25.9)
        {
            for (int s = 0; s < 4 && t < 25.9; ++s)
            {
                const int chordRoot = rootMidi + degrees[s];
                const float tf = (float) (0.14 * std::pow (0.5, (double) degrees[s] / 12.0));

                addRichTone (audio, 44100.0, t, durs[s], midiToHz (chordRoot - 12), 0.12f);
                addRichTone (audio, 44100.0, t, durs[s], midiToHz (chordRoot),      tf);
                addRichTone (audio, 44100.0, t, durs[s], midiToHz (chordRoot + third), 0.10f);
                addRichTone (audio, 44100.0, t, durs[s], midiToHz (chordRoot + 7),  0.10f);
                t += durs[s];
            }
        }

        feedKey (est, audio, 1024);

        const std::string tag = std::string ("target=") + c.name;

        check (est.hasKey(), "no key on harmonic-rich material: " + tag);

        if (! est.hasKey())
            continue;

        std::printf ("  %s -> %s (r=%.2f, tuning=%+.0fc)\n", tag.c_str(),
                     primaryKeyName (est.root(), est.mode()),
                     (double) est.lastScore(), (double) est.tuningCents());

        check (est.root() == c.rootPc && est.minor() == c.minor,
               "wrong key on harmonic-rich material: " + tag);
    }
}

// ----------------------------------------------------------
// 14. 强低音不改变调性判定（K3 抗谐波泄漏）
// ----------------------------------------------------------
void testBassHeavyKey()
{
    std::printf ("[14] Key with dominant bass (harmonic leakage)\n");

    y2k::music::KeyEstimator est;
    est.prepare (44100.0);

    auto audio = makeProgression (44100.0, 0, false, 26.0);   // C major

    // 叠一条很强的低八度 C，模拟低频很重的电子乐
    const double rootHz = midiToHz (36);   // C2
    for (double t = 0.0; t < 25.9; t += 0.5)
        addRichTone (audio, 44100.0, t, 0.45, rootHz, 0.30f, 4);

    feedKey (est, audio, 1024);

    check (est.hasKey(), "no key with heavy bass");

    if (est.hasKey())
    {
        std::printf ("  -> %s (r=%.2f)\n",
                     primaryKeyName (est.root(), est.mode()),
                     (double) est.lastScore());
        check (est.root() == 0 && ! est.minor(),
               "heavy bass pulled the key away from C major");
    }
}

} // namespace

int main()
{
    std::printf ("=== music_analysis_test ===\n");

    testTempoAccuracy();
    testSilence();
    testStartupDelay();
    testTempoChange();
    testKeyDetection();
    testKeyChange();
    testLongRunBoundedMemory();
    testReset();
    testConfidenceRamp();
    testMinorModes();
    testRelativeKeys();
    testTuningDetection();
    testHarmonicRichKey();
    testBassHeavyKey();

    if (gFailures == 0)
    {
        std::printf ("ALL_OK\n");
        return 0;
    }

    std::printf ("FAILED: %d check(s)\n", gFailures);
    return 1;
}
