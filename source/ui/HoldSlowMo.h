#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include "source/ui/PinkXPStyle.h"

// ==========================================================
// HoldSlowMo —— "按住鼠标 = 慢放"的共享状态机（v3.1）
//
//   参照 SpectrumModule 的子弹时间（bulletTimeActive / ghostMix），抽成可复用件，
//   供示波器、波形历史使用：
//     · 按住画布左键 → active = true，时间被拉长 kSlowdown 倍，曲线混向残影色；
//     · 松手 → active = false，**直接回到实时**；残影色按 kGhostFadeSec 线性渐隐。
//   频谱模块仍保留自己的实现（它的"慢放"是回落沿拉长，语义不同），常量保持一致。
// ==========================================================
struct HoldSlowMo
{
    static constexpr float kSlowdown     = 50.0f;   // 与 SpectrumModule::bulletTimeSlowdown 一致
    static constexpr float kGhostFadeSec = 0.6f;    // 与 SpectrumModule::ghostFadeSec 一致

    bool  active   = false;
    float ghostMix = 0.0f;     // 1 = 纯残影色，0 = 正常配色

    void press()   noexcept { active = true; ghostMix = 1.0f; }
    void release() noexcept { active = false; }

    // 每帧调用：松手后残影色渐隐。返回 true 表示 ghostMix 本帧仍在变化（需要重建快照）
    bool tick (float dtRealSec) noexcept
    {
        if (! active && ghostMix > 0.0f)
        {
            ghostMix = std::max (0.0f, ghostMix - dtRealSec / kGhostFadeSec);
            return true;
        }
        return false;
    }

    // 真实帧间隔 → 模块内部时间间隔（按住时被拉长 kSlowdown 倍）
    float dilate (float dtRealSec) const noexcept
    {
        return active ? dtRealSec / kSlowdown : dtRealSec;
    }

    // 基础色 → 残影色（PinkXP::sel，与频谱同色）
    juce::Colour tint (juce::Colour base) const noexcept
    {
        if (ghostMix <= 0.0f) return base;
        return base.interpolatedWith (PinkXP::sel, juce::jlimit (0.0f, 1.0f, ghostMix));
    }

    bool visuallyActive() const noexcept { return active || ghostMix > 0.0f; }
};
