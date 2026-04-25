#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "source/ui/ModuleWorkspace.h"
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

class PhaseModule : public ModulePanel,
                    public AnalyserHub::FrameListener
{
public:
    explicit PhaseModule(AnalyserHub& hub);
    ~PhaseModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:

    // 绘制
    void drawGonio  (juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawCorrDial(juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawWidthBar(juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawBalanceBar(juce::Graphics& g, juce::Rectangle<int> area) const;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaseModule)
};
