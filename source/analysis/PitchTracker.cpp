#include "source/analysis/PitchTracker.h"

#include <algorithm>
#include <cmath>

namespace y2k::pitch
{

namespace
{
    // ---- 可调参数（都在这里，UI 观感不对时只改这一处）----
    // 下界取 30 Hz（C1 附近）：更低的音（如 C0 = 16.35 Hz）需要更大 FFT 才分辨得出，
    //   默认 2048 点 FFT 的 bin 宽 ~23 Hz，本来也测不准 —— 此时音高沿用上一次有效值。
    constexpr float  kMinHz     = 30.0f;
    constexpr float  kMaxHz     = 5000.0f;   // 再高基本是泛音 / 齿音，只稀释音级
    constexpr float  kTiltRefHz = 440.0f;    // 高频倾斜的参考频率
    constexpr float  kTiltExp   = 0.35f;     // 0 = 不倾斜；越大越偏向中高频
    constexpr float  kSmoothTau = 0.10f;     // 音级平滑时间常数（秒）
    constexpr float  kNoteTau   = 0.15f;     // 音高读数（音域染色用）平滑时间常数
    constexpr float  kSilenceDb = -78.0f;    // 低于此电平不出音高
    constexpr float  kFullDb    = -24.0f;    // 高于此电平 confidence 满档
    constexpr float  kPeakFloor = 0.06f;     // 最强桶占比低于此值 → 无明确音高
    constexpr float  kPeakFull  = 0.55f;     // 达到此占比 → confidence = 1
    constexpr double kTwoPi     = 6.283185307179586476925286766559;
}

void Tracker::reset() noexcept
{
    chroma.fill (0.0f);
    published    = Info {};
    smoothedNote = 60.0f;
    noteInit     = false;
}

// 频带 / 音级 / 倾斜只与 (numBins, binHz) 有关：只在两者变化时重算一次。
//   bin → MIDI 音高：midi = 69 + 12·log2(f / 440)，音级 = midi mod 12。
void Tracker::rebuildBinLut (int numBins, double binHz) noexcept
{
    const int n = std::min (numBins, kMaxBins);

    lutBins  = numBins;
    lutBinHz = binHz;

    firstBin = std::max (1, (int) std::ceil ((double) kMinHz / binHz));
    lastBin  = std::min (n, (int) std::floor ((double) kMaxHz / binHz) + 1);

    for (int k = 0; k < n; ++k)
    {
        const double f = (double) k * binHz;

        BinInfo& b = binLut[(size_t) k];

        if (f <= 0.0)
        {
            b = BinInfo {};
            continue;
        }

        const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
        double pc = std::fmod (midi, 12.0);
        if (pc < 0.0)
            pc += 12.0;

        b.pc   = ((int) pc) % kPitchClasses;
        b.frac = (float) (pc - std::floor (pc));
        b.tilt = std::pow ((float) (f / (double) kTiltRefHz), kTiltExp);
    }
}

Info Tracker::pushSpectrum (const float* mag, int numBins, double binHz, float dtSec) noexcept
{
    if (mag == nullptr || numBins < 8 || binHz <= 0.0)
        return published;

    if (numBins != lutBins || binHz != lutBinHz)
        rebuildBinLut (numBins, binHz);

    const float dt = (dtSec > 0.0f && dtSec < 0.5f) ? dtSec : (1.0f / 60.0f);

    // ---------- 1) 12 音级折叠（线性插值，避免半音边界跳变）----------
    std::array<float, (size_t) kPitchClasses> raw {};
    float peakMag = 0.0f;

    for (int k = firstBin; k < lastBin; ++k)
    {
        const float m = mag[(size_t) k];
        if (! (m > 0.0f) || ! std::isfinite (m))
            continue;

        peakMag = std::max (peakMag, m);

        const BinInfo& b = binLut[(size_t) k];
        const float w = m * m * b.tilt;

        raw[(size_t) b.pc]                          += w * (1.0f - b.frac);
        raw[(size_t) ((b.pc + 1) % kPitchClasses)]  += w * b.frac;
    }

    float sum = 0.0f;
    for (float v : raw)
        sum += v;

    if (sum > 0.0f)
        for (float& v : raw)
            v /= sum;

    // ---------- 2) 时间平滑（换音时色相滑动而不是硬切）----------
    const float a = 1.0f - std::exp (-dt / kSmoothTau);
    for (int i = 0; i < kPitchClasses; ++i)
        chroma[(size_t) i] += a * (raw[(size_t) i] - chroma[(size_t) i]);

    float total = 0.0f;
    for (float v : chroma)
        total += v;

    if (total < 1.0e-4f)
    {
        published.valid      = false;
        published.confidence *= 0.85f;   // 静音时置信度衰减，色相保留
        return published;
    }

    // ---------- 3) 锐化后找主音级（平方让主音而不是平均音决定色相）----------
    std::array<float, (size_t) kPitchClasses> w {};
    int   peak = 0;
    float wSum = 0.0f;

    for (int i = 0; i < kPitchClasses; ++i)
    {
        w[(size_t) i] = chroma[(size_t) i] * chroma[(size_t) i];
        wSum += w[(size_t) i];
        if (w[(size_t) i] > w[(size_t) peak])
            peak = i;
    }

    // ---------- 4) 主音级 ±1 桶内的圆周重心 → 连续色相 ----------
    double cx = 0.0, cy = 0.0, localSum = 0.0;
    for (int d = -1; d <= 1; ++d)
    {
        const int    idx = ((peak + d) % kPitchClasses + kPitchClasses) % kPitchClasses;
        const double ang = kTwoPi * (double) idx / (double) kPitchClasses;
        const double ww  = (double) w[(size_t) idx];

        cx += ww * std::cos (ang);
        cy += ww * std::sin (ang);
        localSum += ww;
    }

    double hue = (double) peak / (double) kPitchClasses;
    if (localSum > 1.0e-9)
    {
        hue = std::atan2 (cy, cx) / kTwoPi;
        if (hue < 0.0)
            hue += 1.0;
    }

    // ---------- 5) 主峰频率（读数用）：带内最大 bin + 对数抛物线插值 ----------
    int   bestBin = -1;
    float bestMag = 0.0f;
    for (int k = firstBin; k < lastBin; ++k)
    {
        const float m = mag[(size_t) k];
        if (m > bestMag)
        {
            bestMag = m;
            bestBin = k;
        }
    }

    double hz = 0.0;
    if (bestBin > 0 && bestBin + 1 < numBins && bestMag > 0.0f)
    {
        const double l = std::log ((double) std::max (1.0e-9f, mag[(size_t) (bestBin - 1)]));
        const double c = std::log ((double) std::max (1.0e-9f, mag[(size_t) bestBin]));
        const double r = std::log ((double) std::max (1.0e-9f, mag[(size_t) (bestBin + 1)]));

        const double denom = l - 2.0 * c + r;
        const double delta = (std::abs (denom) > 1.0e-12) ? (0.5 * (l - r) / denom) : 0.0;

        hz = ((double) bestBin + std::clamp (delta, -0.5, 0.5)) * binHz;
    }

    // ---------- 6) 置信度 = 最强桶占比 × 电平门 ----------
    const float levelDb   = 20.0f * std::log10 (std::max (peakMag, 1.0e-9f));
    const float levelGate = std::clamp ((levelDb - kSilenceDb) / (kFullDb - kSilenceDb), 0.0f, 1.0f);
    const float share     = (wSum > 0.0f) ? (w[(size_t) peak] / wSum) : 0.0f;
    const float shareConf = std::clamp ((share - kPeakFloor) / (kPeakFull - kPeakFloor), 0.0f, 1.0f);

    published.hue01      = (float) hue;
    published.confidence = std::clamp (shareConf * levelGate, 0.0f, 1.0f);
    published.valid      = published.confidence > 0.02f;

    if (hz > 0.0)
    {
        const float target = (float) (69.0 + 12.0 * std::log2 (hz / 440.0));

        if (! noteInit)
        {
            smoothedNote = target;
            noteInit     = true;
        }
        else
        {
            const float an = 1.0f - std::exp (-dt / kNoteTau);
            smoothedNote += an * (target - smoothedNote);
        }

        published.hz         = (float) hz;
        published.noteNumber = smoothedNote;
    }

    return published;
}

} // namespace y2k::pitch
