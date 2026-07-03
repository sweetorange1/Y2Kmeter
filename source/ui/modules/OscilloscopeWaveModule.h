#pragma once

#include <JuceHeader.h>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// OscilloscopeWaveModule —— Pink XP 像素风波形示波器（精简版）
//
// 与 OscilloscopeModule（三模式示波器）不同，本模块**专精波形**：
//   · 没有 XY / Lissajous 模式，只有波形模式
//   · 顶部工具栏提供通道选择：L（仅左声道）/ R（仅右声道）/ Both（双声道叠加）
//   · 所有数据复用 AnalyserHub::Kind::Oscilloscope，后端零新增计算
//
// 顶部工具栏：
//   ┌──────────────────────────────────────┐
//   │  [ L ]  [ R ]  [Both]               │
//   ├──────────────────────────────────────┤
//   │                                      │
//   │           波形画布（凹陷）           │
//   │                                      │
//   └──────────────────────────────────────┘
//
// 设计背景：
//   旧版提供了三个独立模块：Oscilloscope（L+R）、Oscilloscope L、Oscilloscope R。
//   三者功能严重重叠，v1.8.4 合并为 OscilloscopeWave（L/R/Both 一键切换）。
// ==========================================================

class OscilloscopeWaveModule : public ModulePanel,
                               public AnalyserHub::FrameListener
{
public:
    explicit OscilloscopeWaveModule(AnalyserHub& hub);
    ~OscilloscopeWaveModule() override;

    // Phase F：Hub 分发器回调
    void onFrame(const AnalyserHub::FrameSnapshot& frame) override;

    enum class ChannelMode { left, right, both };

    void setChannelMode(ChannelMode m);
    ChannelMode getChannelMode() const noexcept { return channelMode; }

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // ---- 绘制 ----
    void drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawWaveform(juce::Graphics& g, juce::Rectangle<int> canvas);
    juce::Path buildWaveformPath(const juce::Array<float>& samples,
                                 juce::Rectangle<int> inner,
                                 float yCenter,
                                 float halfHeight) const;

    // 静态层（背景网格/十字线）
    void rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds);
    void drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const;
    void invalidateStaticLayer();

    // 动态层缓存（波形 Path）
    void redrawDynamicLayerIfNeeded(juce::Rectangle<int> canvas);
    void invalidateDynamicLayer();
    bool snapshotChangedSinceLastDraw() const noexcept;

    // 布局辅助
    juce::Rectangle<int> getToolbarBounds(juce::Rectangle<int> content) const;
    juce::Rectangle<int> getCanvasBounds(juce::Rectangle<int> content) const;
    void refreshChannelButtons();

    // ---- 成员 ----
    AnalyserHub& hub;

    ChannelMode channelMode = ChannelMode::both;

    // 快照（UI 线程独占使用）
    juce::Array<float> snapshotL;
    juce::Array<float> snapshotR;

    // 性能优化：repaint 节流
    double lastRepaintMs = 0.0;

    // 静态层（背景）
    juce::Image staticLayer;
    juce::Rectangle<int> staticLayerContentBounds;
    int themeSubToken = -1;

    // 动态层（波形）
    juce::Image dynamicLayer;
    juce::Rectangle<int> dynamicLayerCanvasBounds;
    bool dynamicLayerDirty = true;

    // 指纹（判断 snapshot 是否变化）
    int   lastDrawnSampleCount = -1;
    float lastDrawnFingerprint[6] { 0, 0, 0, 0, 0, 0 };
    ChannelMode lastDrawnMode = ChannelMode::both;

    // 顶部工具栏按钮
    juce::TextButton btnL    { "L"    };
    juce::TextButton btnR    { "R"    };
    juce::TextButton btnBoth { "Both" };

    static constexpr int toolbarH = 26;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscilloscopeWaveModule)
};
