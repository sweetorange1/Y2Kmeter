#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"

// ==========================================================
// LoudnessModule —— 响度模块（v3.1：合并了 Loudness / LUFS Real-time / True Peak）
//
//   同一路 Kind::Loudness 分析结果，用「项」+「样式」还原每一种旧外观：
//     项：M（瞬时 LUFS）· S（短时 LUFS）· I（积分 LUFS）· True Peak · RMS
//     样式：
//       LED 灯条     —— 旧 Loudness：像素柱 + 右侧 dB 刻度尺 + 底部标签/读数
//       数字读数     —— 旧 LUFS Real-time / True Peak：大数字 + 进度条
//
//   旧外观对应的配置：
//     loudness     → 灯条，M S I RMS（构造默认）
//     lufs_realtime→ 读数，M S I
//     true_peak    → 读数，True Peak + RMS
//
//   灯条样式里 RMS / True Peak 各占 L、R 两根柱；读数样式里各占一行（取 L/R 较大者）。
// ==========================================================

class LoudnessModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit LoudnessModule(AnalyserHub& hub);
    ~LoudnessModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // ---- 持久化 + 控制面板 ----
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    // ---- Toybox（v3.0.0）：打砖块（§8.2）----
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

    enum class Style { ledBars = 0, readouts = 1 };

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // ---- 单条像素柱状表 ----
    struct MeterBar
    {
        juce::String label;       // 标签文字
        float        value       = -144.0f;
        float        peakValue   = -144.0f;  // 峰值保持
        float        peakHoldMs  = 0.0f;
        float        smoothed    = -144.0f;  // 平滑后的显示值

        static constexpr float peakHoldDuration = 3000.0f; // 3s
        static constexpr float peakFallRate     = 0.02f;   // 每帧衰减量（dB）

        void update(float newValue, float deltaMs)
        {
            if (! std::isfinite(newValue))
                newValue = -144.0f;

            // 平滑：上升快，下降慢（按 dt 换算，120Hz 标定）
            const float dtSec = deltaMs * 0.001f;
            const float alpha = y2k::pacing::alphaForDt ((newValue > smoothed) ? 0.5f : 0.05f, dtSec);
            smoothed = smoothed + alpha * (newValue - smoothed);
            smoothed = juce::jlimit(-200.0f, 60.0f, smoothed);

            if (smoothed > peakValue)
            {
                peakValue  = smoothed;
                peakHoldMs = 0.0f;
            }
            else
            {
                peakHoldMs += deltaMs;
                if (peakHoldMs > peakHoldDuration)
                    peakValue -= y2k::pacing::stepForDt (peakFallRate, dtSec);
            }
            peakValue = juce::jmax(-144.0f, peakValue);
        }
    };

    // 灯条样式的一根柱：指向 MeterBar + 是否 LUFS 配色
    struct BarSlot { MeterBar* bar; bool isLufs; };

    struct Items
    {
        bool m = true, s = true, i = true, tp = false, rms = true;
        bool any() const noexcept { return m || s || i || tp || rms; }
    };

    // ---- 灯条样式 ----
    void rebuildBarSlots();
    // 把 content 区拆成各柱区域 + 刻度尺（layoutContent 与静态层共用同一份几何）
    void computeBarGeometry(juce::Rectangle<int> content,
                            std::vector<juce::Rectangle<int>>& bars,
                            juce::Rectangle<int>& scale) const;
    void paintLedBars(juce::Graphics& g, juce::Rectangle<int> contentBounds);
    void paintReadouts(juce::Graphics& g, juce::Rectangle<int> contentBounds);

    void drawMeterBar(juce::Graphics& g, juce::Rectangle<int> barArea, const MeterBar& bar, bool isLUFS) const;
    void drawScale(juce::Graphics& g, juce::Rectangle<int> scaleArea) const;
    void drawMeterBarStaticLayer(juce::Graphics& g, juce::Rectangle<int> barArea, bool isLUFS) const;
    static int firstVisibleRowForValue(float value, int numRows) noexcept;

    void rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds);
    void drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const;
    void invalidateStaticLayer();

    static float valueToNorm(float val) noexcept;
    static juce::Colour lufsToColour(float lufs) noexcept;
    static juce::Colour dbToColour(float db) noexcept;

    // ---- 成员 ----
    AnalyserHub& hub;

    Items items;
    Style style = Style::ledBars;

    // 灯条：M S I TP-L TP-R RMS-L RMS-R（按项过滤后排进 slots）
    MeterBar barM, barS, barI, barTpL, barTpR, barRmsL, barRmsR;
    std::vector<BarSlot> slots;

    // 读数：平滑值 + 闪白 + 心跳
    enum ReadRow { rM = 0, rS, rI, rTp, rRms, numReadRows };
    float rdValue[numReadRows] = { -144.0f, -144.0f, -144.0f, -144.0f, -144.0f };
    float rdFlash[numReadRows] = {};
    float rdPrev [numReadRows] = {};
    float rmsSmoothL = -144.0f, rmsSmoothR = -144.0f;
    float tpHoldSec = 0.0f;
    float pulsePhase = 0.0f;

    // 灯条读数位图缓存（v2.9.6 性能：100ms 节流刷新）
    mutable PinkXP::StaticText readoutCache_[7];
    mutable juce::String readoutCacheText_[7];
    mutable double       readoutCacheTimeMs_[7] = {};

    // 布局缓存（layoutContent 计算，paintContent 使用）
    std::vector<juce::Rectangle<int>> barAreas;
    juce::Rectangle<int> areaScale;
    std::vector<std::pair<int, juce::Rectangle<int>>> readoutRects;   // 读数样式：(行号, 区域)

    juce::Image staticLayer;
    juce::Rectangle<int> staticLayerContentBounds;
    int staticLayerBarCount = -1;
    int themeSubToken = -1;

    static constexpr int cellSize  = 6;
    static constexpr int cellGap   = 1;
    static constexpr int labelH    = 14;  // 底部标签高度
    static constexpr int readoutH  = 14;  // 底部数字读数高度

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessModule)
};
