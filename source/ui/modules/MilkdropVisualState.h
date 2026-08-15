#pragma once

#include <cmath>

// ==========================================================
// MilkdropVisualState —— Milkdrop 模块整体后处理的全局视觉状态
//
// 由 MilkdropModule 的 color / effects 面板修改，经 Editor 全局共享给
// 所有 Milkdrop 模块，并持久化到 PluginProcessor 的 host state 中。
//
// 字段语义：
//   · tint_r/g/b  —— RGB 三通道加性偏移（master output colors），
//                    1.0 = 中性；>1 整体偏向该色（含黑色），<1 偏向补色。
//   · brightness  —— bright 增益（对齐 MilkDrop3 的 ret *= brightness），
//                    范围 0~8，1.0 = 中性；>1 提亮（暗部大幅抬升、高光溢出），
//                    <1 压暗。后处理阶段为纯线性增益 + 最终 clamp。
//   · invert      —— 反相 / 负片（对应 MilkDrop3 的 ret = 1 - ret）。
//   · shadows     —— 暗部针对性压暗并保留高光（亮度掩码 + 平方，而非全局平方）。
//
// 设计为纯数据 + 查询方法的轻量结构体，便于后续追加 solarize / darken /
// brighten 等更多效果字段，而不改动接口签名。
// ==========================================================
struct MilkdropVisualState
{
    float tint_r     = 1.0f;
    float tint_g     = 1.0f;
    float tint_b     = 1.0f;
    float brightness = 1.0f;
    bool  invert     = false;
    bool  shadows    = false;

    // 无任何染色 / 效果时返回 true，用于跳过零开销的 offscreen 后处理路径。
    bool isNeutral() const noexcept
    {
        return std::fabs (tint_r - 1.0f)     <= 1e-4f
            && std::fabs (tint_g - 1.0f)     <= 1e-4f
            && std::fabs (tint_b - 1.0f)     <= 1e-4f
            && std::fabs (brightness - 1.0f) <= 1e-4f
            && ! invert
            && ! shadows;
    }
};
