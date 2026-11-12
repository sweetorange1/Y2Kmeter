#include "source/analysis/SibilanceDetector.h"

#include <algorithm>
#include <cmath>

namespace y2k::sibilance
{

namespace
{
    // ---- 检测参数（集中在这里便于调）----
    constexpr float kHighPassHz   = 5000.0f;    // 齿音下界
    constexpr float kLowPassHz    = 12000.0f;   // 齿音上界（挡掉 12k 以上的"空气声"）
    constexpr float kAttackSec    = 0.001f;     // 1 ms：抓住 's' / 't' 的起振
    constexpr float kReleaseSec   = 0.050f;     // 50 ms：齿音是 50–150 ms 的爆发
    constexpr float kHysteresisDb = 4.0f;       // 迟滞：开 thr / 关 thr-4，避免边界抖动
    constexpr float kRangeDb      = 12.0f;      // 过门限 12 dB 映射到满色
    constexpr float kSmoothSec    = 0.015f;     // 输出平滑（显示用，避免逐样本跳变）
    // 绝对门限（dBFS）不再是常量：见 Detector::setThresholdDb（默认 kDefaultThresholdDb = -35）

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kButterworthQ = 0.70710678118654752;
}

void Detector::designFilters() noexcept
{
    const double fs = (sampleRate > 0.0) ? sampleRate : 48000.0;

    // RBJ cookbook 二阶节；两级同参 Butterworth 级联 = LR4（24 dB/oct）
    auto design = [fs] (float freqHz, bool highPass, Biquad& bq) noexcept
    {
        const double f0 = (double) std::min (freqHz, (float) (fs * 0.45));
        const double w0 = 2.0 * kPi * f0 / fs;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double alpha = sw / (2.0 * kButterworthQ);
        const double a0 = 1.0 + alpha;

        const double b0 = highPass ? (1.0 + cw) * 0.5 : (1.0 - cw) * 0.5;
        const double b1 = highPass ? -(1.0 + cw)     : (1.0 - cw);
        const double b2 = b0;
        const double a1 = -2.0 * cw;
        const double a2 = 1.0 - alpha;

        bq.b0 = (float) (b0 / a0);
        bq.b1 = (float) (b1 / a0);
        bq.b2 = (float) (b2 / a0);
        bq.a1 = (float) (a1 / a0);
        bq.a2 = (float) (a2 / a0);
    };

    for (auto& c : ch)
    {
        design (kHighPassHz, true,  c.hp1);
        design (kHighPassHz, true,  c.hp2);
        design (kLowPassHz,  false, c.lp1);
        design (kLowPassHz,  false, c.lp2);
    }
}

void Detector::prepare (double newSampleRate) noexcept
{
    sampleRate = (newSampleRate > 0.0) ? newSampleRate : 48000.0;

    designFilters();

    attackCoeff  = (float) std::exp (-1.0 / (sampleRate * (double) kAttackSec));
    releaseCoeff = (float) std::exp (-1.0 / (sampleRate * (double) kReleaseSec));
    smoothCoeff  = (float) (1.0 - std::exp (-1.0 / (sampleRate * (double) kSmoothSec)));

    reset();
}

void Detector::reset() noexcept
{
    for (auto& c : ch)
        c.reset();

    drive    = 0.0f;
    bandDb   = -120.0f;
    gateOpen = false;
}

float Detector::pushSample (float left, float right) noexcept
{
    if (! std::isfinite (left))  left  = 0.0f;
    if (! std::isfinite (right)) right = 0.0f;

    const float in[2] = { left, right };
    float envMax = 0.0f;

    for (int i = 0; i < 2; ++i)
    {
        Channel& c = ch[i];

        // 带通：HP(5k) ×2 → LP(12k) ×2
        float x = c.hp1.process (in[i]);
        x = c.hp2.process (x);
        x = c.lp1.process (x);
        x = c.lp2.process (x);

        // 峰值包络（attack / release 两档系数）
        const float rect = std::abs (x);
        c.env = (rect > c.env)
            ? rect + attackCoeff  * (c.env - rect)
            : rect + releaseCoeff * (c.env - rect);

        envMax = std::max (envMax, c.env);
    }

    bandDb = 20.0f * std::log10 (std::max (envMax, 1.0e-9f));

    // 迟滞门限：开门 / 关门两个阈值
    if (! gateOpen)
    {
        if (bandDb > thresholdDb)
            gateOpen = true;
    }
    else if (bandDb < thresholdDb - kHysteresisDb)
    {
        gateOpen = false;
    }

    const float target = gateOpen
        ? std::clamp ((bandDb - thresholdDb) / kRangeDb, 0.0f, 1.0f)
        : 0.0f;

    drive += smoothCoeff * (target - drive);

    if (drive < 1.0e-4f)
        drive = 0.0f;

    return drive;
}

} // namespace y2k::sibilance
