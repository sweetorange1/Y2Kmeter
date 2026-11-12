#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// ==========================================================
// BandSplit —— 时域三频段权重（低 / 中 / 高）
//
//   用途：示波器 X-Y / 李萨如、立体声场的"频段染色"。每个样本点得到三个
//   权重 (wLow, wMid, wHigh)，和为 1；显示层按权重把固定的红 / 绿 / 蓝混色
//   （低频偏红、中频偏绿、高频偏蓝）。
//
//   做法（纯 C++，不依赖 JUCE，可单测）：
//     1) 取单声道和 (L+R)/2；
//     2) 两个 Linkwitz-Riley 4 阶分频点（每点 2 级同参 Butterworth 级联）
//        分成 低 / 中 / 高 三路：低 = LP(f1)，中 = HP(f1)→LP(f2)，高 = HP(f1)→HP(f2)；
//     3) 每路整流 + 峰值包络（立即起、按频段取不同释放时间：低频慢、高频快）；
//     4) 包络归一化 → 权重。静音时权重落到"中"。
//
//   线程 / 状态：窗口式无状态调用（每帧对 2048 样本窗口从零状态跑一遍），
//   窗口最早的约 100 个样本带滤波器起振瞬态，它们 alpha 最低，肉眼不可辨。
//   成本：每帧 ≈ 2048 × 8 个 biquad，微秒级。
// ==========================================================
namespace y2k::bands
{
    class BandSplitter
    {
    public:
        void prepare (double sampleRate, float lowCrossHz = 250.0f, float highCrossHz = 4000.0f) noexcept
        {
            fs = sampleRate > 0.0 ? sampleRate : 48000.0;
            setCrossover (lowCrossHz, highCrossHz);
        }

        void setCrossover (float lowCrossHz, float highCrossHz) noexcept
        {
            const float lo = std::clamp (lowCrossHz, 40.0f, 2000.0f);
            const float hi = std::clamp (highCrossHz, lo * 2.0f, 16000.0f);
            f1 = lo;
            f2 = hi;
            design();
        }

        float getLowCross()  const noexcept { return f1; }
        float getHighCross() const noexcept { return f2; }

        // L / R：n 个样本（时间从旧到新）。输出三个长度 n 的权重数组（和为 1）。
        void compute (const float* L, const float* R, int n,
                      std::vector<float>& wLow, std::vector<float>& wMid, std::vector<float>& wHigh)
        {
            wLow.resize ((size_t) n);
            wMid.resize ((size_t) n);
            wHigh.resize ((size_t) n);
            if (n <= 0) return;

            Chain c = proto;   // 每次调用从零状态开始
            c.reset();

            // 包络释放系数：低频慢、高频快（时间常数约 1.5 个周期的量级）
            const float relLow  = releaseCoeff (8.0e-3);
            const float relMid  = releaseCoeff (1.5e-3);
            const float relHigh = releaseCoeff (3.0e-4);
            float eLo = 0.0f, eMi = 0.0f, eHi = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                const float l = std::isfinite (L[i]) ? L[i] : 0.0f;
                const float r = std::isfinite (R[i]) ? R[i] : 0.0f;
                const float x = 0.5f * (l + r);

                float lo, mi, hi;
                c.process (x, lo, mi, hi);

                eLo = follow (eLo, std::abs (lo), relLow);
                eMi = follow (eMi, std::abs (mi), relMid);
                eHi = follow (eHi, std::abs (hi), relHigh);

                const float sum = eLo + eMi + eHi;
                if (sum < 1.0e-5f)
                {
                    wLow[(size_t) i] = 0.0f; wMid[(size_t) i] = 1.0f; wHigh[(size_t) i] = 0.0f;
                }
                else
                {
                    const float inv = 1.0f / sum;
                    wLow[(size_t) i] = eLo * inv; wMid[(size_t) i] = eMi * inv; wHigh[(size_t) i] = eHi * inv;
                }
            }
        }

    private:
        struct Biquad
        {
            float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            float process (float x) noexcept
            {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
            void reset() noexcept { z1 = z2 = 0.0f; }
        };

        struct Chain
        {
            Biquad lp1a, lp1b;       // 低通 f1 ×2
            Biquad hp1a, hp1b;       // 高通 f1 ×2
            Biquad lp2a, lp2b;       // 低通 f2 ×2（作用在 hp1 之后 → 中频）
            Biquad hp2a, hp2b;       // 高通 f2 ×2（作用在 hp1 之后 → 高频）

            void reset() noexcept
            {
                lp1a.reset(); lp1b.reset(); hp1a.reset(); hp1b.reset();
                lp2a.reset(); lp2b.reset(); hp2a.reset(); hp2b.reset();
            }

            void process (float x, float& low, float& mid, float& high) noexcept
            {
                low = lp1b.process (lp1a.process (x));
                const float rest = hp1b.process (hp1a.process (x));
                mid  = lp2b.process (lp2a.process (rest));
                high = hp2b.process (hp2a.process (rest));
            }
        };

        static float follow (float env, float rect, float rel) noexcept
        {
            return rect > env ? rect : rect + rel * (env - rect);
        }

        float releaseCoeff (double seconds) const noexcept
        {
            return (float) std::exp (-1.0 / (fs * seconds));
        }

        static void designOne (Biquad& bq, double fs, float freq, bool highPass) noexcept
        {
            constexpr double kPi = 3.14159265358979323846;
            const double f0 = std::min ((double) freq, fs * 0.45);
            const double w0 = 2.0 * kPi * f0 / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * 0.70710678118654752);
            const double a0 = 1.0 + alpha;
            const double b0 = highPass ? (1.0 + cw) * 0.5 : (1.0 - cw) * 0.5;
            const double b1 = highPass ? -(1.0 + cw)      : (1.0 - cw);
            bq.b0 = (float) (b0 / a0);
            bq.b1 = (float) (b1 / a0);
            bq.b2 = (float) (b0 / a0);
            bq.a1 = (float) ((-2.0 * cw) / a0);
            bq.a2 = (float) ((1.0 - alpha) / a0);
        }

        void design() noexcept
        {
            designOne (proto.lp1a, fs, f1, false); designOne (proto.lp1b, fs, f1, false);
            designOne (proto.hp1a, fs, f1, true);  designOne (proto.hp1b, fs, f1, true);
            designOne (proto.lp2a, fs, f2, false); designOne (proto.lp2b, fs, f2, false);
            designOne (proto.hp2a, fs, f2, true);  designOne (proto.hp2b, fs, f2, true);
            proto.reset();
        }

        double fs = 48000.0;
        float f1 = 250.0f, f2 = 4000.0f;
        Chain proto;   // 系数原型；compute() 拷贝一份并清零状态
    };

    // 固定的低 / 中 / 高三色（红 / 绿 / 蓝），不随主题变化
    struct Rgb { float r, g, b; };
    inline constexpr Rgb kLowRgb  { 0.93f, 0.23f, 0.23f };   // 红
    inline constexpr Rgb kMidRgb  { 0.23f, 0.83f, 0.35f };   // 绿
    inline constexpr Rgb kHighRgb { 0.23f, 0.48f, 1.00f };   // 蓝

    inline Rgb mix (float wLow, float wMid, float wHigh) noexcept
    {
        return { wLow * kLowRgb.r + wMid * kMidRgb.r + wHigh * kHighRgb.r,
                 wLow * kLowRgb.g + wMid * kMidRgb.g + wHigh * kHighRgb.g,
                 wLow * kLowRgb.b + wMid * kMidRgb.b + wHigh * kHighRgb.b };
    }
}
