#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/GlDirectModule.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// StereoMapModule
//   Compare L/R spectrum and map per-frequency stereo position:
//   * top   = left channel stronger
//   * center= balanced
//   * bottom= right channel stronger
//
//   v2.9.x 新增第二个显示模式（顶部按钮切换）：
//   * PAN  ：纵轴 ±1 声像位置（上面描述的语义）
//   * MONO ：切单声道后的相位抵消量，0 = 完全兼容（贴底），
//            1 = 完全抵消（贴顶）。数据取 Hub 的 mid 路（L+R）/2 频谱
//            与 L/R 两路做比值，抵消越严重值越高。
// ==========================================================
class StereoMapModule : public GlDirectModuleBase,
                        public AnalyserHub::FrameListener
{
public:
    // ---- 显示模式 ----
    enum class Mode
    {
        pan = 0,      // 逐频段 L/R 声像（上 L / 中 C / 下 R）
        cancel = 1    // 逐频段 mono 相位抵消量（越高越严重）
    };

    explicit StereoMapModule (AnalyserHub& hub);
    ~StereoMapModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    void setMode (Mode m);
    Mode getMode() const noexcept { return mode; }

    // 模式写入布局存档（"pan" / "cancel"）
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;

    // 鼠标悬停标尺：绘图区内十字线 + 频率/纵轴坐标读数
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

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
    void applyModeToButton();
    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawCurve (juce::Graphics& g, juce::Rectangle<int> plot) const;

    // 悬停标尺 / 点击读数
    void drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas);
    juce::String makeCoordReadout (juce::Point<int> pos, juce::Rectangle<int> plot) const;
    juce::String makeDataReadout (float x, juce::Rectangle<int> plot) const;
    int  bandIndexForX (float x, juce::Rectangle<int> plot) const;

    static float freqAt (float norm01) noexcept;
    static float normFromFreq (float freqHz) noexcept;
    static float xForNorm (float norm01, juce::Rectangle<int> plot) noexcept;
    static float normForX (float x, juce::Rectangle<int> plot) noexcept;
    static float panForY (float y, juce::Rectangle<int> plot) noexcept;
    static float severityForY (float y, juce::Rectangle<int> plot) noexcept;
    // 真实抵消量 → 显示值（0..1）：满幅只需 kCancelFullScale 的真实抵消
    static float cancelToDisplay (float cancel) noexcept;
    // 显示值 → 真实抵消量（读数用）
    static float displayToCancel (float display) noexcept;
    static juce::String formatPan (float pan);
    static juce::String formatPercent (float v);

    // 工具条：位于凹陷画布之上，画布太矮时隐藏（把空间留给曲线）
    static int toolbarHeightFor (juce::Rectangle<int> contentBounds) noexcept;
    juce::Rectangle<int> getToolbarBounds (juce::Rectangle<int> contentBounds) const;

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
    juce::TextButton btnMode { "PAN" };

    // 鼠标悬停态（标尺）
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    int cachedBandCount = 0;

    static constexpr int kToolbarH = 18;   // 逻辑像素（按 UI 密度缩放）

    // MONO 模式的满幅标定：真实抵消量达到这个比例时曲线就顶到天花板。
    //   0.5 = "抵消一半就满"。调小 → 更敏感（更容易顶满），调大 → 更迟钝。
    static constexpr float kCancelFullScale = 0.5f;

    static constexpr float kMinFreq = 20.0f;
    static constexpr float kMaxFreq = 20000.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoMapModule)
};
