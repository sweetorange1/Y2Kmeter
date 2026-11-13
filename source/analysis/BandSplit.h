#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// ==========================================================
// BandSplit —— 时域三频段分离（低 / 中 / 高）
//
//   用途：示波器 X-Y / 李萨如、立体声场的"频段染色"。
//   做法参考 MiniMeters Stereometer 的 Multiband 模式：把 L / R 各自滤成
//   三路独立信号，三路各自画一层纯色点云叠在一起（颜色不混）。
//
//   实现（纯 C++，不依赖 JUCE，可单测）：
//     · 两个 Linkwitz-Riley 4 阶分频点（每点 2 级同参 Butterworth 级联）：
//         低 = LP(f1)
//         中 = HP(f1) → LP(f2)
//         高 = HP(f2)
//       三路互斥，相位在分频点处对齐（LR 特性）。
//     · 默认 200 Hz / 2000 Hz（与 MiniMeters 一致；面板上可调）。
//
//   线程 / 状态：窗口式无状态调用（每帧对整窗样本从零状态跑一遍），
//   窗口最早的约 100 个样本带滤波器起振瞬态，肉眼不可辨。
//   成本：每帧 ≈ 2 声道 × 2048 样本 × 8 biquad，微秒级。
// ==========================================================
namespace y2k::bands
{
    class BandSplitter
    {
    public:
        static constexpr float kDefaultLowCross  = 200.0f;
        static constexpr float kDefaultHighCross = 2000.0f;

        void prepare (double sampleRate,
                      float lowCrossHz  = kDefaultLowCross,
                      float highCrossHz = kDefaultHighCross) noexcept
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

        // L / R：n 个样本（时间从旧到新）。
        // 输出三路互斥的立体声信号，长度均为 n；调用方负责分配（可复用缓冲）。
        void split (const float* L, const float* R, int n,
                    float* lowL,  float* lowR,
                    float* midL,  float* midR,
                    float* highL, float* highR) const noexcept
        {
            if (n <= 0) return;

            Chain cl = proto, cr = proto;   // 每帧从零状态开始
            cl.reset();
            cr.reset();

            for (int i = 0; i < n; ++i)
            {
                const float l = std::isfinite (L[i]) ? L[i] : 0.0f;
                const float r = std::isfinite (R[i]) ? R[i] : 0.0f;

                float lo, mi, hi;
                cl.process (l, lo, mi, hi);
                lowL[(size_t) i] = lo; midL[(size_t) i] = mi; highL[(size_t) i] = hi;

                cr.process (r, lo, mi, hi);
                lowR[(size_t) i] = lo; midR[(size_t) i] = mi; highR[(size_t) i] = hi;
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
        float f1 = kDefaultLowCross, f2 = kDefaultHighCross;
        Chain proto;   // 系数原型；split() 拷贝一份并清零状态
    };

    // 三路固定配色（MiniMeters Stereometer 主题默认值）：
    //   低 #E60045（红）/ 中 #665AFF（蓝紫）/ 高 #00FFFF（青）
    struct Rgb { float r, g, b; };
    inline constexpr Rgb kLowRgb  { 0.902f, 0.000f, 0.271f };   // 230,  0, 69
    inline constexpr Rgb kMidRgb  { 0.400f, 0.353f, 1.000f };   // 102, 90,255
    inline constexpr Rgb kHighRgb { 0.000f, 1.000f, 1.000f };   //   0,255,255
}
