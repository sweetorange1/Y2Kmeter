#pragma once

#include <JuceHeader.h>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// OscilloscopeModule —— Pink XP 像素风立体声示波器
//
// 支持三种显示模式：
//   * Waveform  ：时域波形（L 粉 / R 深粉 叠加）
//   * XY        ：李萨如 XY 模式（L→X，R→Y，点阵）
//   * Lissajous ：Mid/Side 模式（旋转 45° 的 XY）
//
// 顶部工具栏：
//   ┌──────────────────────────────────────┐
//   │ [Wave] [X-Y] [Liss]   [Freeze]       │
//   ├──────────────────────────────────────┤
//   │                                      │
//   │           波形画布（凹陷）           │
//   │                                      │
//   └──────────────────────────────────────┘
//
// 数据来源：AnalyserHub::getOscilloscopeSnapshot（立体声）
// 刷新率：30Hz（juce::Timer）
// 冻结时 Timer 继续跑，但不再拉取新快照，保留当前内容。
// ==========================================================

class OscilloscopeModule : public ModulePanel,
                           public AnalyserHub::FrameListener
{
public:
    explicit OscilloscopeModule(AnalyserHub& hub);
    ~OscilloscopeModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    enum class DisplayMode { waveform, xy, lissajous };

    void setDisplayMode(DisplayMode m);
    DisplayMode getDisplayMode() const noexcept { return displayMode; }

    void setFrozen(bool b);
    bool isFrozen() const noexcept { return frozen; }

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:

    // ---- 各模式绘制 ----
    void drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawWaveform  (juce::Graphics& g, juce::Rectangle<int> canvas);
    void drawXY        (juce::Graphics& g, juce::Rectangle<int> canvas, bool rotate45);
    juce::Path buildWaveformPath(const juce::Array<float>& samples,
                                 juce::Rectangle<int> inner,
                                 float yCenter,
                                 float halfHeight) const;

    void rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds);
    void drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const;
    void invalidateStaticLayer();

    // 方案 A：动态波形 / XY 点图缓存层
    //   · 只有数据变化（snapshot 有新内容）或尺寸变化时才重绘
    //   · paintContent 只做一次 drawImageAt，把 4 次 strokePath 的成本摊到"有新数据的帧"
    void redrawDynamicLayerIfNeeded(juce::Rectangle<int> canvas);
    void invalidateDynamicLayer();
    bool snapshotChangedSinceLastDraw() const noexcept;

    // 顶部工具栏布局
    juce::Rectangle<int> getToolbarBounds (juce::Rectangle<int> content) const;
    juce::Rectangle<int> getCanvasBounds  (juce::Rectangle<int> content) const;

    // 模式按钮点击 / 刷新外观
    void refreshModeButtons();

    // ---- 成员 ----
    AnalyserHub& hub;

    DisplayMode displayMode = DisplayMode::waveform;
    bool        frozen      = false;

    // 快照（UI 线程独占使用）
    juce::Array<float> snapshotL;
    juce::Array<float> snapshotR;

    // 性能优化（阶段1）：UI 侧 repaint 节流。
    double lastRepaintMs = 0.0;

    juce::Image staticLayer;
    juce::Rectangle<int> staticLayerContentBounds;
    int themeSubToken = -1;

    // 方案 A：动态层缓存
    juce::Image dynamicLayer;
    juce::Rectangle<int> dynamicLayerCanvasBounds; // 以 canvas 坐标系存
    bool        dynamicLayerDirty = true;

    // 粗粒度指纹：采样长度 + 首/中/尾元素 —— 用来判断 snapshot 是否真的变了
    int   lastDrawnSampleCount = -1;
    float lastDrawnFingerprint[6] { 0, 0, 0, 0, 0, 0 };
    DisplayMode lastDrawnMode = DisplayMode::waveform;
    bool        lastDrawnFrozen = false;

    // 顶部工具栏按钮
    juce::TextButton btnWave   { "Wave"   };
    juce::TextButton btnXY     { "X-Y"    };
    juce::TextButton btnLiss   { "Liss"   };
    juce::TextButton btnFreeze { "Freeze" };

    static constexpr int toolbarH = 26;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscilloscopeModule)
};
