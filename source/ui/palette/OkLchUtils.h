// ==========================================================
// OkLchUtils.h —— 纯 C++ OKLCH 色彩工具（不依赖 JUCE，便于单元测试）
//
//   · sRGB ↔ OKLab ↔ OKLCH 转换（Björn Ottosson 2020 感知均匀色彩空间）
//   · 色域裁剪：OKLCH 超出 sRGB 时保 L、H 降 C，避免通道截断偏色（需求 A2）
//   · OKLab 空间插值（替代 sRGB 线性插值，解决渐变中段发灰，需求 A3）
//   · WCAG 对比度（基于线性 RGB 相对亮度，用于护栏 Q1/Q2）
//
//   对应需求案 §5.1。
// ==========================================================
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace y2kpal
{
    // sRGB 浮点色（0..1，gamma 编码）
    struct Rgb
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };

    // 8bit sRGB 色（0..255）
    struct Rgb8
    {
        uint8_t r = 0, g = 0, b = 0;

        bool operator==(const Rgb8& o) const { return r == o.r && g == o.g && b == o.b; }
        bool operator!=(const Rgb8& o) const { return !(*this == o); }
    };

    struct OkLab
    {
        float L = 0.0f, a = 0.0f, b = 0.0f;
    };

    // OKLCH（H 单位：度 [0, 360)）
    struct OkLch
    {
        float L = 0.0f, C = 0.0f, H = 0.0f;
    };

    // ---- 基本工具 ----
    inline float clamp01(float x) noexcept { return std::max(0.0f, std::min(1.0f, x)); }

    inline float wrapHue(float h) noexcept
    {
        h = std::fmod(h, 360.0f);
        if (h < 0.0f) h += 360.0f;
        return h;
    }

    inline Rgb8 toRgb8(const Rgb& c) noexcept
    {
        Rgb8 o;
        o.r = (uint8_t) std::lroundf(clamp01(c.r) * 255.0f);
        o.g = (uint8_t) std::lroundf(clamp01(c.g) * 255.0f);
        o.b = (uint8_t) std::lroundf(clamp01(c.b) * 255.0f);
        return o;
    }

    inline Rgb fromRgb8(const Rgb8& c) noexcept
    {
        return { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f };
    }

    // ---- sRGB ↔ linear ----
    inline float srgbToLinear(float c) noexcept
    {
        return (c <= 0.04045f) ? c / 12.92f
                               : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    inline float linearToSrgb(float c) noexcept
    {
        return (c <= 0.0031308f) ? c * 12.92f
                                 : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    }

    inline Rgb rgbToLinear(const Rgb& c) noexcept
    {
        return { srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b) };
    }

    inline Rgb linearToRgb(const Rgb& c) noexcept
    {
        return { linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b) };
    }

    // ---- linear RGB → OKLab ----
    inline OkLab linearToOkLab(const Rgb& lin) noexcept
    {
        const float l = 0.4122214708f * lin.r + 0.5363325363f * lin.g + 0.0514459929f * lin.b;
        const float m = 0.2119034982f * lin.r + 0.6806995451f * lin.g + 0.1073969566f * lin.b;
        const float s = 0.0883024619f * lin.r + 0.2817188376f * lin.g + 0.6299787005f * lin.b;

        const float l_ = std::cbrt(l);
        const float m_ = std::cbrt(m);
        const float s_ = std::cbrt(s);

        OkLab o;
        o.L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
        o.a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
        o.b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
        return o;
    }

    // ---- OKLab → linear RGB ----
    inline Rgb okLabToLinear(const OkLab& o) noexcept
    {
        const float l_ = o.L + 0.3963377774f * o.a + 0.2158037573f * o.b;
        const float m_ = o.L - 0.1055613458f * o.a - 0.0638541728f * o.b;
        const float s_ = o.L - 0.0894841775f * o.a - 1.2914855480f * o.b;

        const float l = l_ * l_ * l_;
        const float m = m_ * m_ * m_;
        const float s = s_ * s_ * s_;

        Rgb o2;
        o2.r = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
        o2.g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
        o2.b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
        return o2;
    }

    // ---- sRGB → OKLCH ----
    inline OkLch rgbToOkLch(const Rgb8& c) noexcept
    {
        const OkLab lab = linearToOkLab(rgbToLinear(fromRgb8(c)));
        OkLch o;
        o.L = lab.L;
        o.C = std::sqrt(lab.a * lab.a + lab.b * lab.b);
        o.H = wrapHue(std::atan2(lab.b, lab.a) * (180.0f / 3.14159265358979323846f));
        return o;
    }

    inline OkLch rgbToOkLch(const Rgb& c) noexcept { return rgbToOkLch(toRgb8(c)); }

    // ---- OKLCH → sRGB ----
    inline Rgb okLchToRgb(const OkLch& o) noexcept
    {
        const float ha = o.H * (3.14159265358979323846f / 180.0f);
        OkLab lab;
        lab.L = o.L;
        lab.a = o.C * std::cos(ha);
        lab.b = o.C * std::sin(ha);
        return linearToRgb(okLabToLinear(lab));
    }

    inline Rgb8 okLchToRgb8(const OkLch& o) noexcept { return toRgb8(okLchToRgb(o)); }

    // ---- 色域检查：给定 OKLCH 能否直接转成合法 sRGB ----
    inline bool isInGamut(const OkLch& o, float eps = 0.0005f) noexcept
    {
        const Rgb c = okLchToRgb(o);
        return c.r >= -eps && c.r <= 1.0f + eps &&
               c.g >= -eps && c.g <= 1.0f + eps &&
               c.b >= -eps && c.b <= 1.0f + eps;
    }

    // ---- 色域裁剪（A2）：保 L、H，从 C 向下二分找最大合法 C ----
    //   返回被裁剪后的 OkLch（若原本就在色域内则原样返回）。
    inline OkLch gamutClamp(const OkLch& o) noexcept
    {
        if (isInGamut(o))
            return o;

        float lo = 0.0f, hi = o.C;
        for (int i = 0; i < 28; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            if (isInGamut({ o.L, mid, o.H }))
                lo = mid;
            else
                hi = mid;
        }
        return { o.L, lo, o.H };
    }

    // ---- OKLab 空间插值（A3）：把 C1/C2 映射到 OKLab 后线性插值 ----
    inline Rgb8 interpolateOkLab(const Rgb8& c1, const Rgb8& c2, float t) noexcept
    {
        t = clamp01(t);
        const OkLab a = linearToOkLab(rgbToLinear(fromRgb8(c1)));
        const OkLab b = linearToOkLab(rgbToLinear(fromRgb8(c2)));
        const OkLab m{ a.L + (b.L - a.L) * t,
                       a.a + (b.a - a.a) * t,
                       a.b + (b.b - a.b) * t };
        return toRgb8(linearToRgb(okLabToLinear(m)));
    }

    // ---- WCAG 对比度（基于线性 RGB 相对亮度）----
    inline float relativeLuminance(const Rgb8& c) noexcept
    {
        const Rgb lin = rgbToLinear(fromRgb8(c));
        return 0.2126f * lin.r + 0.7152f * lin.g + 0.0722f * lin.b;
    }

    // 返回值 ≥ 1（按亮/暗自动取大比值）
    inline float wcagContrast(const Rgb8& a, const Rgb8& b) noexcept
    {
        const float la = relativeLuminance(a);
        const float lb = relativeLuminance(b);
        const float hi = std::max(la, lb);
        const float lo = std::min(la, lb);
        return (hi + 0.05f) / (lo + 0.05f);
    }

    // OKLab 明度差（护栏 Q3 用）
    inline float okLchDeltaL(const Rgb8& a, const Rgb8& b) noexcept
    {
        return std::fabs(rgbToOkLch(a).L - rgbToOkLch(b).L);
    }
}
