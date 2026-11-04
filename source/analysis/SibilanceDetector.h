#pragma once

#include <array>

// ==========================================================
// SibilanceDetector —— 齿音检测（专业 de-esser 的侧链结构）
//
// 业内成熟的 de-esser 检测链：
//   输入/侧链 → HP + LP 带通（通常 2 kHz…20 kHz，可调）
//             → 立体声联动
//             → 电平包络 + 迟滞门限
//
// 本类实现其中与"识别"相关的部分（不含增益计算 / 处理）：
//   1) 带通：HP 5 kHz + LP 12 kHz，各两级（Q=0.7071）级联 = 24 dB/oct（LR4）
//      —— 齿音集中 6–10 kHz，必须把 5 kHz 以下的中频和 12 kHz 以上的空气声挡掉；
//   2) 每声道独立包络：peak follower，attack 1 ms / release 50 ms；
//   3) 立体声联动：env = max(envL, envR)（等价 Stereo Link 100%），
//      不会因为 (L+R)/2 降混把侧声道齿音抵消掉；
//   4) 迟滞门限：绝对值 dBFS（开门 / 关门两个阈值：
//      开 = exp(thr*ln10/20)，关 = exp((thr-hys)*ln10/20)），
//      过门限后 kRangeDb dB 内线性映射到 0..1。
//
// 与旧实现（一阶高通 + hfRms/rms 占比 × 3.5）的区别：
//   · 有低通 → 踩镲/空气声不再等同齿音；
//   · 24 dB/oct 而非 6 dB/oct → 2 kHz 人声主体不会再漏进来；
//   · 绝对电平 + 迟滞 → 与电平无关的"占比"不再让安静嘶声满色；
//   · 时间常数固定（不随 SPEED / 窗口宽度变化）→ 换速度不会改变判定。
//
// 线程模型：逐样本调用（UI 线程，在 Waveform 的样本消费循环里），无锁、无分配。
// 纯 C++（不依赖 JUCE），可被 tests/ 直接单测。
// ==========================================================

namespace y2k::sibilance
{

class Detector
{
public:
    void prepare (double newSampleRate) noexcept;
    void reset() noexcept;

    // 逐样本推进（立体声），返回该样本的齿音强度 0..1
    float pushSample (float left, float right) noexcept;

    float getDrive() const noexcept   { return drive; }
    float getBandDb() const noexcept  { return bandDb; }   // 带内电平（dBFS），调试/读数用
    bool  isGateOpen() const noexcept { return gateOpen; }

private:
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;

        void reset() noexcept { z1 = 0.0f; z2 = 0.0f; }

        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    struct Channel
    {
        Biquad hp1, hp2;   // 两级 Butterworth → LR4 高通
        Biquad lp1, lp2;   // 两级 Butterworth → LR4 低通
        float  env = 0.0f;

        void reset() noexcept
        {
            hp1.reset(); hp2.reset();
            lp1.reset(); lp2.reset();
            env = 0.0f;
        }
    };

    void designFilters() noexcept;

    double sampleRate = 48000.0;

    Channel ch[2];

    // 包络 / 平滑系数
    float attackCoeff  = 0.0f;
    float releaseCoeff = 0.0f;
    float smoothCoeff  = 0.0f;

    float drive    = 0.0f;
    float bandDb   = -120.0f;
    bool  gateOpen = false;
};

} // namespace y2k::sibilance
