#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"


// ==========================================================
// VuMeterModule —— 模拟指针 / 横向条形 双大类专业 VU 电平表
//
//   · 两大顶级形态（MeterType）：
//       1. Needle（指针式，4 种表盘）：
//            · Studio     —— 180° 经典演播室半圆模拟表（宽屏立体声自动双表并排）
//            · Vintage    —— ±47° 窄扇形复古硬件表窗 + 百分比副刻度 + 机械盖板
//            · Dual arc   —— 240° 发光环弧现代仪表 + 实时弧尾辉光
//            · Retro warm —— 经典背光暖色广播级 VU 机头表
//       2. Bar（横向条形，5 种样式）：
//            · Studio bar —— 顺滑 / 刻度分格 / 霓虹辉光演播室水平条
//            · LED blocks —— 分段硬件 LED 方块条（三档密度）
//            · LED ladder —— 逐格加高阶梯 LED 柱（立体声上下镜像）
//            · Mirrored   —— 中脊 dB 刻度 + 上下对称镜像条
//            · Minimal    —— 大号数字读数 + 极简精密水平丝带
//   · 5 套色谱（ColorMap）：Theme（默认跟随当前皮肤）/ Studio / Warm VU / MiniMeter / Ice
//   · 4 档量程（ScaleRange）：-24..+3 / -36..+3 / -48..+3 / -60..0 dBFS
//   · 数据源：订阅 AnalyserHub::Kind::Oscilloscope 2048 样本原始波形自算瞬时 RMS，
//     后端音频线程零新增计算。
// ==========================================================
class VuMeterModule : public ModulePanel,
                      public AnalyserHub::FrameListener,
                      private juce::Timer
{
public:
    enum class MeterType       { needle = 0, bar = 1 };
    enum class NeedleDialStyle { studio = 0, vintage = 1, dualArc = 2, retroWarm = 3 };
    enum class BarDialStyle    { studioBar = 0, ledBlocks = 1, ledLadder = 2, mirrored = 3, minimal = 4 };
    enum class ColorMap        { theme = 0, studio = 1, warmVu = 2, miniMeter = 3, ice = 4 };
    enum class NeedleShape     { thin = 0, tapered = 1, glow = 2 };
    enum class BarDensity      { chunky = 0, medium = 1, fine = 2 };
    enum class BarFinish       { smooth = 0, notched = 1, glow = 2 };
    enum class ChannelMode     { mono = 0, stereo = 1, left = 2, right = 3 };
    enum class ScaleRange      { r24 = 0, r36 = 1, r48 = 2, r60 = 3 };

    explicit VuMeterModule (AnalyserHub& hub);
    ~VuMeterModule() override;

    MeterType       getMeterType()       const noexcept { return meterType_; }
    NeedleDialStyle getNeedleDialStyle() const noexcept { return needleDialStyle_; }
    BarDialStyle    getBarDialStyle()    const noexcept { return barDialStyle_; }
    ColorMap        getColorMap()        const noexcept { return colorMap_; }
    NeedleShape     getNeedleShape()     const noexcept { return needleShape_; }
    BarDensity      getBarDensity()      const noexcept { return barDensity_; }
    BarFinish       getBarFinish()       const noexcept { return barFinish_; }
    ChannelMode     getChannelMode()     const noexcept { return channelMode_; }
    ScaleRange      getScaleRange()      const noexcept { return scaleRange_; }
    bool            isPeakHoldEnabled()  const noexcept { return peakHold; }
    bool            isBacklightEnabled() const noexcept { return backlight_; }
    bool            isReadoutEnabled()   const noexcept { return showReadout_; }

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // ---- 持久化 + 4 标签页控制面板（Style / Meter / Ballistics / Scale）----
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    // ---- Toybox：迪斯科灯 + 指针虚影（支持单表/双表）+ 5 种横向条形专属互动 ----
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    juce::Rectangle<int> getChromaKeyCanvas() override { return getContentBounds(); }

private:
    void timerCallback() override;

    // ---- 共享刻度描述符（供指针式与条形渲染器共用）----
    struct VuScaleTick
    {
        float       db;
        bool        major;
        const char* label;
    };

    // ---- 控制面板参数与运行期峰值状态（.inl 渲染器可直接访问）----
    MeterType       meterType_       = MeterType::needle;
    NeedleDialStyle needleDialStyle_ = NeedleDialStyle::studio;
    BarDialStyle    barDialStyle_    = BarDialStyle::studioBar;
    ColorMap        colorMap_        = ColorMap::theme;
    NeedleShape     needleShape_     = NeedleShape::tapered;
    BarDensity      barDensity_      = BarDensity::medium;
    BarFinish       barFinish_       = BarFinish::smooth;
    ChannelMode     channelMode_     = ChannelMode::mono;
    ScaleRange      scaleRange_      = ScaleRange::r36; // -36..+3 dB
    bool            showLed          = true;
    bool            showReadout_     = true;
    bool            backlight_       = true;
    bool            peakHold         = false;
    float           peakHoldSec_     = 1.6f;  // 0.5..4.0 s hold before falling
    float           trimDb           = 0.0f;
    int             ballistics       = 1;     // 0 Slow / 1 Classic / 2 Fast / 3 Snappy
    float           rmsWindowMs      = 20.0f;
    float           warnDbfs         = 0.0f;
    float           peakHoldL        = -144.0f;
    float           peakHoldR        = -144.0f;
    float           peakHoldMono     = -144.0f;
    float           peakHoldTimerL   = 0.0f;
    float           peakHoldTimerR   = 0.0f;
    float           peakHoldTimerM   = 0.0f;

    // ---- 最近一帧渲染产出的几何（指针叠层 / 宽屏双表 / 条形 / Toybox 共用）----
    struct DialGeom
    {
        bool  valid       = false;
        bool  dualSide    = false;
        float angleStart  = -juce::MathConstants<float>::halfPi;
        float angleEnd    = +juce::MathConstants<float>::halfPi;
        juce::Point<float> pivot { 0.0f, 0.0f };
        float radius     = 0.0f;
        float needleLen  = 0.0f;
        float tailLen    = 6.0f;
        float capR       = 0.0f;
        juce::Point<float> pivotR { 0.0f, 0.0f };
        float radiusR    = 0.0f;
        float needleLenR = 0.0f;
        float capRR      = 0.0f;
        float leftX       = 0.0f;
        float rightX      = 0.0f;
        float trackTop    = 0.0f;
        float trackBottom = 0.0f;
        float bandTop     = 0.0f;
        float bandBottom  = 0.0f;
        int   ladderRows  = 0;
        float rowTop[2]   { 0.0f, 0.0f };
        float rowBot[2]   { 0.0f, 0.0f };
        bool  rowHang[2]  { false, false };
    };
    mutable DialGeom dialGeom;

    // ---- 共享辅助 API（FineSplitModules.cpp 实现，供 VuNeedleRenderer.inl / VuBarRenderer.inl 调用）----
    bool  isLinearStyle()       const noexcept { return meterType_ == MeterType::bar; }
    bool  isStereoMode()        const noexcept { return channelMode_ == ChannelMode::stereo; }
    bool  isSingleChannelMode() const noexcept { return channelMode_ == ChannelMode::left || channelMode_ == ChannelMode::right; }
    float minDisplayDb()  const noexcept;
    float maxDisplayDb()  const noexcept;
    float cautionDbfs()   const noexcept;
    float dialTFor        (float db) const noexcept;
    float needleAngleFor  (float db) const noexcept;
    float needleLengthFor (float db) const noexcept;
    float linearXFor      (float db) const noexcept;
    float linearDbForX    (float x)  const noexcept;
    float currentMonoDbfs()      const noexcept;
    float activeSingleDbfs()     const noexcept;
    float activeSinglePeakDbfs() const noexcept;
    const char* activeChannelTag() const noexcept;
    void  getScaleTicks   (std::vector<VuScaleTick>& out) const;
    juce::Colour vuPaletteColour (float t01, float dbVal, bool darkCanvas) const noexcept;
    juce::Colour vuAlertColour() const noexcept;
    juce::String formatDbReadout (float db) const;
    juce::Rectangle<float> linearBody (juce::Rectangle<int> dialArea) const;
    juce::Rectangle<int>   ledBounds  (juce::Rectangle<int> area) const;

    // ---- 指针式渲染器（实现于 VuNeedleRenderer.inl）----
    void drawNeedleStudioDial    (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawNeedleVintageDial   (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawNeedleDualArcDial   (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawNeedleRetroWarmDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawNeedlePointer       (juce::Graphics& g, juce::Point<float> pivot, float length, float tailLen,
                                  float angleRad, juce::Colour colour, float thickness,
                                  NeedleShape shape, float alphaScale = 1.0f) const;
    void drawNeedleOverlay       (juce::Graphics& g, juce::Rectangle<int> dialArea) const;

    // ---- 横向条形渲染器（实现于 VuBarRenderer.inl）----
    void drawBarStudioDial    (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawBarLedBlocksDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawBarLedLadderDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawBarMirrorDial    (juce::Graphics& g, juce::Rectangle<int> dialArea) const;
    void drawBarMinimalDial   (juce::Graphics& g, juce::Rectangle<int> dialArea) const;

    // ---- LED 信号灯 & 横向 Toybox ----
    void drawLed       (juce::Graphics& g, juce::Rectangle<int> ledArea) const;
    void toyPokeLinear (const ToyHit& hit, ToyContext& ctx);

    // ---- 音频分析与弹道状态 ----
    AnalyserHub& hub;
    int    themeSubToken = -1;

    float  targetL       = -144.0f;
    float  targetR       = -144.0f;
    float  displayedL    = -144.0f;
    float  displayedR    = -144.0f;
    float  ledLevelDb    = -144.0f;
    double lastTickMs    = 0.0;
    double lastOnFrameMs = 0.0;
    float  pulsePhase    = 0.0f;

    void advanceBallistics (float dtMs);

    static constexpr float fanHalfAngle       = 0.82f;
    static constexpr float instantRmsWindowMs = 20.0f;
    static constexpr float tauRiseMs          = 80.0f;
    static constexpr float tauFallMs          = 1200.0f;
    static constexpr float tauMuteFallMs      = 1800.0f;
    static constexpr float ledSignalDbfs      = -25.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VuMeterModule)
};
