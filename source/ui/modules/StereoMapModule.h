#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/GlDirectModule.h"
#include "source/analysis/AnalyserHub.h"
#include "source/analysis/FreqScale.h"

// ==========================================================
// StereoMapModule
//   Compare L/R spectrum and map per-frequency stereo position:
//   * top   = left channel stronger
//   * center= balanced
//   * bottom= right channel stronger
//
//   v2.9.x 新增第二个显示模式（顶部按钮切换）：
//   * PAN  ：纵轴 ±1 声像位置（上面描述的语义）
//   * MONO ：单声道保留量（**上下颠倒**）：顶边 = 100%（没有抵消），
//            底边 = 50%（抵消达到满幅标定 kCancelFullScale）。
//            曲线 / 填充从顶部基线向下生长 —— 抵消越严重，图形越低。
//            数据取 Hub 的 mid 路（L+R）/2 频谱与 L/R 两路做比值。
//   工具条右侧有一行随模式切换的说明文字（PAN：上=左 / 下=右；
//   MONO：上=干净 / 下=单声道相位损失）。
// ==========================================================
class StereoMapModule : public GlDirectModuleBase,
                        public AnalyserHub::FrameListener
{
public:
    // ---- 显示模式 ----
    enum class Mode
    {
        pan = 0,      // 逐频段 L/R 声像（上 L / 中 C / 下 R）
        cancel = 1    // 逐频段 mono 兼容度（上 = 干净 / 下 = 严重抵消）
    };

    explicit StereoMapModule (AnalyserHub& hub);
    ~StereoMapModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    void setMode (Mode m);
    Mode getMode() const noexcept { return mode; }

    // 模式 + FFT scale 写入布局存档（mode: "pan" / "cancel"；fftScale: 0 Log / 1 Mel / 2 Linear）
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;

    // ---- 控制面板（v3.1）：原顶部 PAN/MONO 按钮 + FFT scale ----
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;
    void setFftScale (y2k::fscale::Scale s);

    // 鼠标悬停标尺：绘图区内十字线 + 频率/纵轴坐标读数
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;   // 按住拖动时读数也要跟随鼠标
    void mouseExit (const juce::MouseEvent& e) override;

    // ---- 按住子弹时间（与 Spectrum 一致）：只拖慢回落沿 ----
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

    // ---- Toybox：点击绘图区 → 该频段的声像 / 抵消读数气泡 ----
    //   ToyHit / ToyContext 由 ModuleWorkspace.h 前向声明，实现文件引入 ToyKit.h。
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

protected:
    void layoutContent (juce::Rectangle<int> contentBounds) override;

    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    void computeStereoMap (float dtSec);
    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawCurve (juce::Graphics& g, juce::Rectangle<int> plot) const;

    // 悬停标尺 / 点击读数
    void drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas);
    juce::String makeCoordReadout (juce::Point<int> pos, juce::Rectangle<int> plot) const;
    juce::String makeDataReadout (float x, juce::Rectangle<int> plot) const;
    int  bandIndexForX (float x, juce::Rectangle<int> plot) const;

    // 频率轴（按 fftScale：Log / Mel / Linear，范围 kMinFreq ~ kMaxFreq）
    float freqAt (float norm01) const noexcept;
    float normFromFreq (float freqHz) const noexcept;
    // 纵向网格线频率 / 底部标签频率（随刻度变化；Log 下与旧版逐项一致）
    void gridFreqs (std::vector<float>& lines, std::vector<float>& labels) const;
    static float xForNorm (float norm01, juce::Rectangle<int> plot) noexcept;
    static float normForX (float x, juce::Rectangle<int> plot) noexcept;
    static float panForY (float y, juce::Rectangle<int> plot) noexcept;
    // MONO 纵轴（显示值 = 单声道保留量 = 1 - 真实抵消量）：
    //   顶边 1（无抵消）/ 底边 1 - kCancelFullScale（满幅抵消）
    static float compatForY (float y, juce::Rectangle<int> plot) noexcept;
    // 真实抵消量 → 显示值（0..1）：满幅只需 kCancelFullScale 的真实抵消。
    //   倒置后曲线 y = 顶边 + 显示值 × 高（显示值越大越往下 = 越糟）。
    static float cancelToDisplay (float cancel) noexcept;
    static juce::String formatPan (float pan);
    static juce::String formatPercent (float v);

    // v3.1：顶部工具条取消，画布 = 整个内容区（留 2px 边距）
    static juce::Rectangle<int> canvasFromContent (juce::Rectangle<int> contentBounds) noexcept;
    juce::Rectangle<int> canvasBounds() const;
    juce::Rectangle<int> getPlotBounds (juce::Rectangle<int> canvas) const;

    AnalyserHub& hub;

    juce::Array<float> magsL;
    juce::Array<float> magsR;
    juce::Array<float> magsM;          // mid = (L+R)/2 路（mono 抵消用）

    std::vector<float> panByBand;
    std::vector<float> strengthByBand;
    std::vector<float> levelDbByBand;  // 平滑后的原始电平（dBFS），仅用于读数
    std::vector<float> cancelByBand;   // 0 = mono 兼容 / 1 = 完全抵消（真实值，绘制时再放大）

    Mode mode = Mode::pan;
    y2k::fscale::Scale fftScale = y2k::fscale::Scale::log;   // 构造默认 = 旧外观

    // 鼠标悬停态（标尺）
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    // 按住子弹时间：只把下降沿（回落）的时间拉长 kBulletTimeSlowdown 倍，
    //   上升沿保持实时（与 SpectrumModule 的语义一致）
    bool bulletTimeActive = false;
    static constexpr float kBulletTimeSlowdown = 50.0f;

    int cachedBandCount = 0;

    // MONO 模式的满幅标定：真实抵消量达到这个比例时曲线就顶到天花板。
    //   0.5 = "抵消一半就满"。调小 → 更敏感（更容易顶满），调大 → 更迟钝。
    static constexpr float kCancelFullScale = 0.5f;

    static constexpr float kMinFreq = 20.0f;
    static constexpr float kMaxFreq = 20000.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoMapModule)
};
