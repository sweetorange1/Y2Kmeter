#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"

// ==========================================================
// DynamicsModule —— Pink XP 像素风动态范围监测（v3.1：动态模块）
//
//   合并了旧的 Dynamics / Dynamics Meters / Dynamics DR / Dynamics Crest：
//   同一路 Kind::Dynamics 分析结果，不同"项"开关与样式即可还原每一种旧外观。
//
//   项（控制面板里可开关）：
//     · 峰值柱 PK L/R     · RMS 柱 RMS L/R
//     · DR short-term     · DR integrated     · Crest 当前值
//     · Crest 历史曲线（跨度 1~35 s 可调）
//   样式：DR/Crest 读数 = 大数字（旧 Dynamics）/ 带进度条读数（旧 Dynamics DR）
//
//   布局（从左到右，只排启用的区段）：
//     柱组 | DR 读数 | Crest 历史
//   四项全开时与旧 Dynamics 逐像素相同（柱组 160 + DR 180 + 余量给历史）。
//
//   旧存档兼容：无 state 键时 = 旧 Dynamics 外观；退役类型经
//   resolveRetiredModuleType 注入种子（见 ModuleWorkspace.cpp）。
// ==========================================================

class DynamicsModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit DynamicsModule(AnalyserHub& hub);
    ~DynamicsModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // 鼠标悬停标尺：Crest 历史带内十字线 + 时间/crest 读数
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    // ---- 持久化 + 控制面板 ----
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    // ---- Toybox（v3.0.0）：综合路由（Meters/DR/Crest，§8.2）----
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

    enum class ReadoutStyle { bigDigits = 0, barReadout = 1 };

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

    // 三个 DR 读数行
    enum DrRow { rowShort = 0, rowInteg = 1, rowCrest = 2, numDrRows = 3 };

    struct Items
    {
        bool peak = true, rms = true;
        bool drShort = true, drInteg = true, crest = true;
        bool hist = true;
        int  barCount() const noexcept { return (peak ? 2 : 0) + (rms ? 2 : 0); }
        int  drCount()  const noexcept { return (drShort ? 1 : 0) + (drInteg ? 1 : 0) + (crest ? 1 : 0); }
        bool any() const noexcept { return peak || rms || drShort || drInteg || crest || hist; }
    };

    void drawMeterGroup(juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawDrBigDigits(juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawDrReadouts (juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawCrestBand (juce::Graphics& g, juce::Rectangle<int> area) const;
    void drawSingleBar (juce::Graphics& g, juce::Rectangle<int> area, const MeterBar& bar) const;

    bool drRowEnabled (int row) const noexcept;
    float drRowValue (int row) const noexcept;

    // dBFS → [0,1] 条高
    static float dbToNorm(float db)
    {
        return juce::jlimit(0.0f, 1.0f, juce::jmap(db, -60.0f, 0.0f, 0.0f, 1.0f));
    }
    static juce::Colour dbToColour(float db);

    // ---- 成员 ----
    AnalyserHub& hub;

    Items items;
    ReadoutStyle readoutStyle = ReadoutStyle::bigDigits;
    float histSpanSec = 1.0f;     // 默认 1 s = 旧 Dynamics 的实际窗口（120 格 × 1/120 s）

    // 4 柱：PeakL, PeakR, RmsL, RmsR
    MeterBar bPeakL, bPeakR, bRmsL, bRmsR;

    // DR 值（平滑）
    float smoothedShortDR = 0.0f;
    float smoothedIntegDR = 0.0f;
    float smoothedCrest   = 0.0f;

    // 带进度条读数样式的动效（闪白 / 心跳）
    float flash[numDrRows] = { 0.0f, 0.0f, 0.0f };
    float prevDr[numDrRows] = { 0.0f, 0.0f, 0.0f };
    float pulsePhase = 0.0f;

    // DR 面板读数位图缓存（v2.9.6 性能：100ms 节流刷新，见 drawDrBigDigits）
    mutable PinkXP::StaticText drValueCache_[3];
    mutable juce::String drValueText_[3];
    mutable double       drValueTime_[3] = { 0.0, 0.0, 0.0 };

    // Crest 历史：120 点/秒 × 35 s（旧 Dynamics 的 120 格同为 120 点/秒）
    static constexpr int   kHistRate   = 120;
    static constexpr float kMaxSpanSec = 35.0f;
    static constexpr int   crestHistLen = (int) (kHistRate * kMaxSpanSec);
    std::vector<float> crestHist;
    int   crestHistWrite = 0;
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
