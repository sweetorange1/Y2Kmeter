#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// LoudnessModule —— Pink XP 像素风响度计模块
//
// 布局（内容区从左到右）：
//   ┌──────────────────────────────────────────────────────┐
//   │  [LUFS-M 柱] [LUFS-S 柱] [LUFS-I 柱]  [Peak 柱×2]  │
//   │   数字读数    数字读数    数字读数      L/R 数字读数  │
//   │                                                      │
//   │  右侧 dBFS 刻度尺（-60 ~ 0 dBFS）                   │
//   └──────────────────────────────────────────────────────┘
//
// 每条柱状表：
//   - 像素格子（cellSize = 6px，gap = 1px）
//   - 颜色分区：绿（< -14 LUFS）→ 黄（-14 ~ -9）→ 红（> -9）
//   - 峰值保持线（3s 衰减）
//   - 底部数字读数（Pink XP 字体，11px）
// ==========================================================

class LoudnessModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit LoudnessModule(AnalyserHub& hub);
    ~LoudnessModule() override;

    // Phase F：由 Hub 的 30Hz 分发器在 UI 线程调用
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:

    // ---- 单条像素柱状表 ----
    struct MeterBar
    {
        juce::String label;       // 标签文字（如 "M"、"S"、"I"、"L"、"R"）
        float        value       = -144.0f;  // 当前值（dBFS / LUFS）
        float        peakValue   = -144.0f;  // 峰值保持
        float        peakHoldMs  = 0.0f;     // 峰值保持计时（ms）
        float        smoothed    = -144.0f;  // 平滑后的显示值

        static constexpr float peakHoldDuration = 3000.0f; // 3s
        static constexpr float peakFallRate     = 0.02f;   // 每帧衰减量（dB）

        void update(float newValue, float deltaMs)
        {
            // 防御：非有限值（NaN / Inf）归为底噪 -144dB，避免污染 smoothed
            if (! std::isfinite(newValue))
                newValue = -144.0f;

            // 平滑：上升快，下降慢
            const float alpha = (newValue > smoothed) ? 0.5f : 0.05f;
            smoothed = smoothed + alpha * (newValue - smoothed);

            // 再夹一次，严防浮点累计误差飘出合理区间
            smoothed = juce::jlimit(-200.0f, 60.0f, smoothed);

            // 峰值保持
            if (smoothed > peakValue)
            {
                peakValue  = smoothed;
                peakHoldMs = 0.0f;
            }
            else
            {
                peakHoldMs += deltaMs;
                if (peakHoldMs > peakHoldDuration)
                    peakValue -= peakFallRate;
            }
            peakValue = juce::jmax(-144.0f, peakValue);
        }
    };

    // ---- 绘制辅助 ----
    void drawMeterBar(juce::Graphics& g,
                      juce::Rectangle<int> barArea,
                      const MeterBar& bar,
                      bool isLUFS) const;

    void drawScale(juce::Graphics& g,
                   juce::Rectangle<int> scaleArea,
                   bool isLUFS) const;

    void drawMeterBarStaticLayer(juce::Graphics& g,
                                 juce::Rectangle<int> barArea,
                                 bool isLUFS) const;

    static int firstVisibleRowForValue(float value, int numRows, bool isLUFS) noexcept;

    void rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds);
    void drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const;
    void invalidateStaticLayer();

    // dBFS / LUFS 值 → 柱高比例 [0,1]
    static float valueToNorm(float val, bool isLUFS) noexcept;

    // 根据 LUFS 值返回颜色
    static juce::Colour lufsToColour(float lufs) noexcept;
    // 根据 dBFS 值返回颜色
    static juce::Colour dbToColour(float db) noexcept;

    // ---- 成员 ----
    AnalyserHub& hub;

    // 5 条柱：M / S / I / L / R
    MeterBar barM, barS, barI, barL, barR;

    // 上次 tick 时间（用于峰值保持计时）
    juce::Time lastTickTime;

    // 布局缓存（在 layoutContent 中计算，在 paintContent 中使用）
    juce::Rectangle<int> areaM, areaS, areaI, areaL, areaR, areaScale;

    juce::Image staticLayer;
    juce::Rectangle<int> staticLayerContentBounds;
    int themeSubToken = -1;

    static constexpr int cellSize  = 6;
    static constexpr int cellGap   = 1;
    static constexpr int labelH    = 14;  // 底部标签高度
    static constexpr int readoutH  = 14;  // 底部数字读数高度

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessModule)
};
