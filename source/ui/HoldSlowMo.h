#pragma once

#include <JuceHeader.h>
#include "source/ui/PinkXPStyle.h"

// ==========================================================
// HoldSlowMo —— "按住鼠标 = 慢放"的共享状态机（v3.1）
//
//   参照 SpectrumModule 的子弹时间（bulletTimeActive），抽成可复用件：
//     · 示波器：按住画布左键 → active = true，时间被拉长 kSlowdown 倍；
//     · 波形历史：只用 active 表示"冻结"（按住期间不消费样本，画面完全静止）。
//   两者都是松手 → active = false，**直接回到实时**。
//   慢放 / 冻结本身**不改变配色**（残影变色已取消），画面始终用模块自己的正常配色。
//   频谱模块仍保留自己的实现（它的"慢放"是回落沿拉长，语义不同），常量保持一致。
// ==========================================================
struct HoldSlowMo
{
    static constexpr float kSlowdown = 50.0f;   // 与 SpectrumModule::bulletTimeSlowdown 一致

    bool active = false;

    void press()   noexcept { active = true; }
    void release() noexcept { active = false; }

    // 真实帧间隔 → 模块内部时间间隔（按住时被拉长 kSlowdown 倍）
    float dilate (float dtRealSec) const noexcept
    {
        return active ? dtRealSec / kSlowdown : dtRealSec;
    }
};
