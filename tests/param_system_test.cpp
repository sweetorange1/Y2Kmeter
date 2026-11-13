// ==========================================================
// param_system_test —— v3.1 参数系统新增纯算法的离线验证（不依赖 JUCE / 音频设备）
//
// 覆盖：
//   1. FreqScale：log / mel / linear 的 toUnit ↔ fromUnit 互逆、端点、单调；
//      Log 与旧实现（20 Hz ~ 20 kHz 对数插值）逐点一致（"旧外观"回归保证）。
//   2. BandSplitter：纯低频 / 中频 / 高频正弦 → 三路滤波输出的能量分别落在
//      对应频段（互斥）；静音三路都无能量；分频点可调；三色为 MiniMeters 默认。
//
// 运行：直接执行，全部通过时打印 ALL_OK（返回 0）。
// ==========================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "source/analysis/BandSplit.h"
#include "source/analysis/FreqScale.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
int gFailures = 0;

void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        std::printf ("  FAIL: %s\n", what.c_str());
        ++gFailures;
    }
}

void checkNear (double v, double target, double tol, const std::string& what)
{
    if (std::abs (v - target) > tol)
    {
        std::printf ("  FAIL: %s (got %.6f, want %.6f)\n", what.c_str(), v, target);
        ++gFailures;
    }
}

void testFreqScale()
{
    using namespace y2k::fscale;
    std::printf ("[FreqScale]\n");

    const double lo = 20.0, hi = 20000.0;

    for (Scale s : { Scale::log, Scale::mel, Scale::linear })
    {
        checkNear (toUnit (s, lo, lo, hi), 0.0, 1e-9, "low end maps to 0");
        checkNear (toUnit (s, hi, lo, hi), 1.0, 1e-9, "high end maps to 1");

        double prev = -1.0;
        for (int i = 0; i <= 100; ++i)
        {
            const double t = i / 100.0;
            const double f = fromUnit (s, t, lo, hi);
            check (f >= lo - 1e-6 && f <= hi + 1e-6, "fromUnit stays in range");
            checkNear (toUnit (s, f, lo, hi), t, 1e-6, "toUnit(fromUnit(t)) == t");
            check (f > prev, "fromUnit monotonic");
            prev = f;
        }
    }

    // Log 必须与旧实现逐点一致：10^(log10(lo) + t*(log10(hi)-log10(lo)))
    for (int i = 0; i <= 50; ++i)
    {
        const double t = i / 50.0;
        const double legacy = std::pow (10.0, std::log10 (lo) + t * (std::log10 (hi) - std::log10 (lo)));
        checkNear (fromUnit (Scale::log, t, lo, hi), legacy, legacy * 1e-12, "log scale == legacy formula");
    }

    // Linear 中点 = 算术中点；Mel 中点远低于算术中点、高于对数中点
    checkNear (fromUnit (Scale::linear, 0.5, lo, hi), 10010.0, 1e-6, "linear midpoint");
    const double melMid = fromUnit (Scale::mel, 0.5, lo, hi);
    const double logMid = fromUnit (Scale::log, 0.5, lo, hi);
    check (melMid > logMid && melMid < 10010.0, "mel midpoint lies between log and linear");

    check (fromIndex (0) == Scale::log && fromIndex (1) == Scale::mel && fromIndex (2) == Scale::linear,
           "fromIndex mapping");
}

// 生成 n 个样本的正弦（L = R = 单声道）
std::vector<float> sine (double fs, double freq, int n, float amp = 0.5f)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = amp * (float) std::sin (2.0 * kPi * freq * i / fs);
    return x;
}

void testBandSplit()
{
    std::printf ("[BandSplitter]\n");

    constexpr double fs = 48000.0;
    constexpr int n = 2048;

    y2k::bands::BandSplitter sp;
    sp.prepare (fs);   // 默认 200 / 2000 Hz（MiniMeters 一致）

    std::vector<float> loL, loR, miL, miR, hiL, hiR;

    // 跳过滤波器起振段，取后半窗口的 RMS
    auto rms = [] (const std::vector<float>& v)
    {
        const size_t start = v.size() / 2;
        double s = 0.0;
        for (size_t i = start; i < v.size(); ++i)
            s += (double) v[i] * v[i];
        const double cnt = (double) std::max<size_t> (1, v.size() - start);
        return std::sqrt (s / cnt);
    };

    auto splitRms = [&] (const std::vector<float>& x, double* out)
    {
        loL.resize ((size_t) n); loR.resize ((size_t) n);
        miL.resize ((size_t) n); miR.resize ((size_t) n);
        hiL.resize ((size_t) n); hiR.resize ((size_t) n);
        sp.split (x.data(), x.data(), n,
                  loL.data(), loR.data(), miL.data(), miR.data(), hiL.data(), hiR.data());
        out[0] = rms (loL); out[1] = rms (miL); out[2] = rms (hiL);
    };

    struct Case { double f; int dominant; const char* name; };   // 0 低 1 中 2 高
    const Case cases[] = { { 80.0, 0, "80 Hz -> low" }, { 1000.0, 1, "1 kHz -> mid" },
                           { 9000.0, 2, "9 kHz -> high" } };

    for (const auto& c : cases)
    {
        const auto x = sine (fs, c.f, n);
        double a[3] = { 0, 0, 0 };
        splitRms (x, a);

        std::printf ("  %-14s low=%.3f mid=%.3f high=%.3f\n", c.name, a[0], a[1], a[2]);
        check (a[c.dominant] > 0.2, std::string (c.name) + ": dominant band has energy");

        for (int b = 0; b < 3; ++b)
            if (b != c.dominant)
                check (a[c.dominant] > a[b] * 2.0, std::string (c.name) + ": dominant band wins");
    }

    // 静音 → 三路都没有能量
    {
        std::vector<float> z ((size_t) n, 0.0f);
        double a[3] = { 0, 0, 0 };
        splitRms (z, a);
        check (a[0] < 1e-6 && a[1] < 1e-6 && a[2] < 1e-6, "silence -> no band energy");
    }

    // 分频点可调：2 kHz 在 (200,4000) 属中频，在 (200,1000) 属高频
    {
        const auto x = sine (fs, 2000.0, n);
        double a[3] = { 0, 0, 0 };

        sp.setCrossover (200.0f, 4000.0f);
        splitRms (x, a);
        check (a[1] > a[2], "crossover change moves 2 kHz into the mid band");

        sp.setCrossover (200.0f, 1000.0f);
        splitRms (x, a);
        check (a[2] > a[1], "crossover change moves 2 kHz into the high band");
    }

    // 固定三色（MiniMeters 主题默认）：低 = 红、中 = 蓝紫、高 = 青
    {
        const auto& lo = y2k::bands::kLowRgb;
        const auto& mi = y2k::bands::kMidRgb;
        const auto& hi = y2k::bands::kHighRgb;
        check (lo.r > lo.g && lo.r > lo.b, "low = red");
        check (mi.b > mi.r && mi.b > mi.g, "mid = purple-blue");
        check (hi.g > 0.9f && hi.b > 0.9f && hi.r < 0.1f, "high = cyan");
    }
}
} // namespace

int main()
{
    testFreqScale();
    testBandSplit();

    if (gFailures == 0)
    {
        std::printf ("ALL_OK\n");
        return 0;
    }
    std::printf ("%d FAILURES\n", gFailures);
    return 1;
}
