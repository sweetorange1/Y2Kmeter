#pragma once

#include <JuceHeader.h>
#include <cmath>

// ==========================================================
// FramePacing —— v2.9 帧率无关的 UI 动画辅助
//
// 背景：
//   帧分发改为 vblank 同步后，onFrame 的调用频率 = min(FPS 上限, 显示器刷新率)，
//   可能是 30/60/120/144/165Hz 任意值。历史代码里大量"每帧固定系数"的平滑/衰减/
//   计数（如 x += 0.25 * (t - x)、peak -= 0.008、--holdFrames）会随帧率变快变慢。
//
// 约定：
//   所有"每帧常数"都按 kRefHz = 120Hz 标定 —— 这正是旧 FrameDispatcher 的默认
//   分发频率（125Hz Timer），因此换算后在 120Hz 下视觉行为与旧版本完全一致，
//   在其它帧率下保持相同的"每秒"变化速度。
// ==========================================================
namespace y2k::pacing
{
    constexpr float kRefHz = 120.0f;

    // 一阶指数平滑系数：alphaAtRef 是 120Hz 下每帧系数，换算到实际 dt。
    //   cur += alphaForDt(a, dt) * (target - cur)
    inline float alphaForDt (float alphaAtRef, float dtSec) noexcept
    {
        const float a = juce::jlimit (0.0f, 1.0f, alphaAtRef);
        if (a >= 1.0f) return 1.0f;
        return 1.0f - std::pow (1.0f - a, juce::jmax (0.0f, dtSec) * kRefHz);
    }

    // 线性步进量（衰减/下落/相位推进）：stepAtRef 是 120Hz 下每帧步长。
    inline float stepForDt (float stepAtRef, float dtSec) noexcept
    {
        return stepAtRef * juce::jmax (0.0f, dtSec) * kRefHz;
    }

    // "按帧计数"换算为秒：framesAtRef 帧（120Hz 下）对应的时长。
    constexpr float framesToSec (float framesAtRef) noexcept
    {
        return framesAtRef / kRefHz;
    }
}
