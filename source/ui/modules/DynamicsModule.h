#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <array>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"

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

    // 鼠标悬停标尺：Crest 历史带内十字线 + 时间/crest 读数
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    // ---- Toybox（v3.0.0）：综合路由（Meters/DR/Crest，§8.2）----
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

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

        // v2.9：dtSec 驱动（120Hz 标定），帧率无关
        void update(float newVal, float dtSec)
        {
            if (! std::isfinite(newVal)) newVal = -144.0f;
            const float alpha = y2k::pacing::alphaForDt ((newVal > smoothed) ? 0.5f : 0.08f, dtSec);
            smoothed = smoothed + alpha * (newVal - smoothed);
            smoothed = juce::jlimit(-144.0f, 60.0f, smoothed);

            if (smoothed > peakHold)
            {
                peakHold = smoothed;
                peakHoldMs = 0.0f;
            }
            else
            {
                peakHoldMs += dtSec * 1000.0f;
                if (peakHoldMs > 2000.0f) peakHold -= y2k::pacing::stepForDt (0.05f, dtSec);
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
        if (db > -3.0f)  return PinkXP::sel;
        if (db > -9.0f)  return PinkXP::pink500;
        return PinkXP::pink200;
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

    // DR 面板读数位图缓存（v2.9.6 性能：100ms 节流刷新，见 drawDrPanel）
    mutable juce::Image  drValueCache_[3];
    mutable juce::String drValueText_[3];
    mutable double       drValueTime_[3] = { 0.0, 0.0, 0.0 };

    // Crest 历史带（120 格；v2.9 起按时间推进，每 1/120s 一格 ≈ 1s）
    static constexpr int crestHistLen = 120;
    std::array<float, crestHistLen> crestHist {};
    int crestHistWrite = 0;
    float crestHistAccum = 0.0f;

    // 布局缓存
    juce::Rectangle<int> areaMeters;
    juce::Rectangle<int> areaDr;
    juce::Rectangle<int> areaCrest;

    // ---- 鼠标悬停标尺 ----
    void drawHoverRuler(juce::Graphics& g);
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsModule)
};
