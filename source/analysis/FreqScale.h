#pragma once

#include <algorithm>
#include <cmath>

// ==========================================================
// FreqScale —— 频率轴刻度（log / mel / linear）的正反映射（v3.1，"FFT scale"）
//
//   频谱、语谱图、立体声频段图的横轴（或纵轴）都是"频率 → [0,1] 的位置"：
//     · Log    ：对数（现状，20 Hz ~ 20 kHz 每倍频等宽）
//     · Mel    ：梅尔刻度 m = 2595·log10(1 + f/700)，贴近听感
//     · Linear ：线性，高频占的面积大
//   这个头文件是这些映射的唯一事实来源：UI 的坐标换算与 Hub 的取样函数
//   （AnalyserHub::getSpectrumMagnitudesBlended*）都用它，保证"画在哪 = 取哪里的数据"。
//
//   纯 C++，无 JUCE 依赖。
// ==========================================================
namespace y2k::fscale
{
    enum class Scale { log = 0, mel = 1, linear = 2 };

    inline double toMel (double f) noexcept { return 2595.0 * std::log10 (1.0 + f / 700.0); }
    inline double fromMel (double m) noexcept { return 700.0 * (std::pow (10.0, m / 2595.0) - 1.0); }

    // 频率 → [0,1]（fLo → 0，fHi → 1），区间外夹到端点
    inline double toUnit (Scale s, double f, double fLo, double fHi) noexcept
    {
        f = std::clamp (f, fLo, fHi);
        switch (s)
        {
            case Scale::mel:
            {
                const double a = toMel (fLo), b = toMel (fHi);
                return b > a ? (toMel (f) - a) / (b - a) : 0.0;
            }
            case Scale::linear:
                return fHi > fLo ? (f - fLo) / (fHi - fLo) : 0.0;
            case Scale::log:
            default:
            {
                const double a = std::log10 (std::max (fLo, 1.0e-9));
                const double b = std::log10 (std::max (fHi, 1.0e-9));
                return b > a ? (std::log10 (std::max (f, 1.0e-9)) - a) / (b - a) : 0.0;
            }
        }
    }

    // [0,1] → 频率
    inline double fromUnit (Scale s, double t, double fLo, double fHi) noexcept
    {
        switch (s)
        {
            case Scale::mel:
            {
                const double a = toMel (fLo), b = toMel (fHi);
                return fromMel (a + t * (b - a));
            }
            case Scale::linear:
                return fLo + t * (fHi - fLo);
            case Scale::log:
            default:
            {
                const double a = std::log10 (std::max (fLo, 1.0e-9));
                const double b = std::log10 (std::max (fHi, 1.0e-9));
                return std::pow (10.0, a + t * (b - a));
            }
        }
    }

    inline Scale fromIndex (int i) noexcept
    {
        return i == 1 ? Scale::mel : (i == 2 ? Scale::linear : Scale::log);
    }
}
