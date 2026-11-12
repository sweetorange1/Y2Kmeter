#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include "source/ui/GlDirectModule.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// PhaseModule —— Pink XP 像素风立体声相位模块（v3.1：相位模块）
//
//   合并了旧的 Phase / Phase Correlation / Phase Balance：
//   同一路 Kind::Phase（相关/宽度/平衡）+ 可选 Kind::Oscilloscope（相位图点云）。
//
//   项（控制面板里可开关）：
//     · 相位图 Goniometer（M/S 矢量示波）
//     · 相关度 Correlation（半圆指针）
//     · 宽度 Width 条
//     · 平衡 Balance 条
//   样式：
//     · 相位图：点云 Dots / 轨迹 Trace（相邻样本连线）
//     · 条形：像素格 Segmented / 实心 Solid
//
//   布局：
//   ┌────────────────────────────────────────┐
//   │  Goniometer (M/S)    |   Correlation   │   ← 上排：相位图 + 相关度
//   ├────────────────────────────────────────┤
//   │ Width:  [====●====]  Balance: [--●--]  │   ← 下排：宽度 / 平衡条
//   └────────────────────────────────────────┘
//   只排启用的区段；四项全开时与旧 Phase 逐像素相同。
//
//   旧存档兼容：无 state 键时 = 旧 Phase 外观；phase_correlation / phase_balance
//   经 resolveRetiredModuleType 注入种子（见 ModuleWorkspace.cpp）。
// ==========================================================

class PhaseModule : public GlDirectModuleBase,
                    public AnalyserHub::FrameListener
{
public:
    explicit PhaseModule(AnalyserHub& hub);
    ~PhaseModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // ---- 持久化 + 控制面板 ----
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    // ---- Toybox（v3.0.0）：Gonio 声呐 + 幽灵指针（§8.3）----
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

    enum class GonioStyle { dots = 0, trace = 1 };
    enum class BarStyle   { segmented = 0, solid = 1 };

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;

    // ---- GlDirectModuleBase 钩子（v2.9.4 迁移）----
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    struct Items
    {
        bool gonio = true, corr = true, width = true, bal = true;
        bool any() const noexcept { return gonio || corr || width || bal; }
    };

    // 四个子区域；某项关闭 = 对应区域为空
    struct Areas { juce::Rectangle<int> gonio, dial, width, balance; };
    Areas computeAreas (juce::Rectangle<int> content) const;

    void updateSubscriptions();

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

    Items items;
    GonioStyle gonioStyle = GonioStyle::dots;
    BarStyle   barStyle   = BarStyle::segmented;
    bool oscRetained = false, phaseRetained = false;

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
