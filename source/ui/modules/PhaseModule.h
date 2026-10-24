#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "source/ui/GlDirectModule.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// PhaseModule —— Pink XP 像素风立体声相位相关仪
//
// 布局：
//   ┌────────────────────────────────────────┐
//   │  Goniometer (M/S)    |   Correlation   │
//   │   椭圆样本云          |   弧形指针仪    │
//   │                      |                 │
//   ├────────────────────────────────────────┤
//   │ Width:  [====●====]  Balance: [--●--]  │
//   └────────────────────────────────────────┘
//
// 数据：
//   - 样本点云：AnalyserHub::getOscilloscopeSnapshot（立体声）
//   - 相关/宽度/平衡：AnalyserHub::getPhaseSnapshot
//
// 刷新率：30Hz
// ==========================================================

class PhaseModule : public GlDirectModuleBase,
                    public AnalyserHub::FrameListener
{
public:
    explicit PhaseModule(AnalyserHub& hub);
    ~PhaseModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // ---- Toybox（v3.0.0）：Gonio 声呐 + 幽灵指针（§8.3）----
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;

    // ---- GlDirectModuleBase 钩子（v2.9.4 迁移）----
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:

    // 绘制
    void drawGonio  (juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawCorrDial(juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawWidthBar(juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawBalanceBar(juce::Graphics& g, juce::Rectangle<int> area) const;

    void rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds);
    void drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const;
    void invalidateStaticLayer();

    // ---- 成员 ----
    AnalyserHub& hub;

    juce::Array<float> oscL, oscR;

    // 平滑快照
    float smoothedCorr    = 0.0f;
    float smoothedWidth   = 0.0f;
    float smoothedBalance = 0.0f;

    // 子区域缓存
    juce::Rectangle<int> areaGonio;
    juce::Rectangle<int> areaDial;
    juce::Rectangle<int> areaWidth;
    juce::Rectangle<int> areaBalance;

    juce::Image staticLayer;
    juce::Rectangle<int> staticLayerContentBounds;
    int themeSubToken = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaseModule)
};
