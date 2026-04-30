#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <array>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// DynamicsModule —— Pink XP 像素风动态范围监测
//
// 布局：
//   ┌─────────────────────────────────────────────┐
//   │  Peak/RMS 四柱  │   DR 大字  │  Crest 历史 │
//   │  PL PR RL RR    │  SHORT  17 │   ~~~~~~~~  │
//   │  (像素柱)        │  INTEG  14 │   (滚动带)  │
//   └─────────────────────────────────────────────┘
//
// 数据：AnalyserHub::getDynamicsSnapshot
// 刷新率：30Hz
// ==========================================================

class DynamicsModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit DynamicsModule(AnalyserHub& hub);
    ~DynamicsModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:

    // ---- 柱状表 ----
    struct MeterBar
    {
        juce::String label;
        float value     = -144.0f;
        float smoothed  = -144.0f;
        float peakHold  = -144.0f;
        float peakHoldMs = 0.0f;

        void update(float newVal, float deltaMs)
        {
            if (! std::isfinite(newVal)) newVal = -144.0f;
            const float alpha = (newVal > smoothed) ? 0.5f : 0.08f;
            smoothed = smoothed + alpha * (newVal - smoothed);
            smoothed = juce::jlimit(-144.0f, 60.0f, smoothed);

            if (smoothed > peakHold)
            {
                peakHold = smoothed;
                peakHoldMs = 0.0f;
            }
            else
            {
                peakHoldMs += deltaMs;
                if (peakHoldMs > 2000.0f) peakHold -= 0.05f;
            }
            peakHold = juce::jmax(-144.0f, peakHold);
            value = newVal;
        }
    };

    void drawMeterGroup(juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawDrPanel   (juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawCrestBand (juce::Graphics& g, juce::Rectangle<int> area) const;

    void drawSingleBar (juce::Graphics& g, juce::Rectangle<int> area, const MeterBar& bar) const;

    // dBFS → [0,1] 条高
    static float dbToNorm(float db)
    {
        return juce::jlimit(0.0f, 1.0f, juce::jmap(db, -60.0f, 0.0f, 0.0f, 1.0f));
    }
    static juce::Colour dbToColour(float db)
    {
        if (db > -3.0f)  return juce::Colour(0xffec4d85);
        if (db > -9.0f)  return juce::Colour(0xffffcc44);
        return juce::Colour(0xff66cc88);
    }

    // ---- 成员 ----
    AnalyserHub& hub;
    juce::Time   lastTickTime;

    // 4 柱：PeakL, PeakR, RmsL, RmsR
    MeterBar bPeakL, bPeakR, bRmsL, bRmsR;

    // DR 值（平滑）
    float smoothedShortDR = 0.0f;
    float smoothedIntegDR = 0.0f;
    float smoothedCrest   = 0.0f;

    // Crest 历史带（120 帧 ≈ 4s）
    static constexpr int crestHistLen = 120;
    std::array<float, crestHistLen> crestHist {};
    int crestHistWrite = 0;

    // 布局缓存
    juce::Rectangle<int> areaMeters;
    juce::Rectangle<int> areaDr;
    juce::Rectangle<int> areaCrest;

    // 性能优化（阶段1）：UI 侧 repaint 节流。
    //   Hub 可能以 60~100Hz 回调 onFrame，但 Dynamics 显示的峰值/DR 数字
    //   肉眼 ~30Hz 已足够。用最小刷新间隔（高 DPI 适度放大）避免每次都刷。
    double lastRepaintMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsModule)
};
