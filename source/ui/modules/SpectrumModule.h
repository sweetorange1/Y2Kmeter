#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// SpectrumModule —— Pink XP 像素风高精度频谱分析仪
//
// 特性：
//   * 对数频率坐标（20Hz ~ 20kHz）
//   * dBFS 纵轴（默认 -80 ~ 0 dBFS）
//   * 三层显示：
//       - 实时（半透明浅色）
//       - 平滑（主曲线）
//       - 峰值保持（虚线，可关）
//   * Slope 补偿：4.5 dB/oct 高频提升，抵消粉噪谱下沉观感
//   * 频率刻度（100 / 1k / 10k）+ dB 刻度（每 20dB）
//
// 数据来源：AnalyserHub::getSpectrumMagnitudes（1024 bin）
// 刷新率：30Hz
// ==========================================================

class SpectrumModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit SpectrumModule(AnalyserHub& hub);
    ~SpectrumModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    void setPeakHoldEnabled(bool b);
    bool isPeakHoldEnabled() const noexcept { return peakHoldEnabled; }

    void setSlopeEnabled(bool b);
    bool isSlopeEnabled() const noexcept { return slopeEnabled; }

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:

    // ---- 数据处理 ----
    // 把 1024 bin 的线性幅度 → N_cols 列的 dBFS 值（按对数频率+Slope 补偿）
    void rebuildDisplay();
    void ensureDisplayCache (int numPoints);

    // ---- 绘制 ----
    void drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawGrid      (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawCurves    (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawAxisLabels(juce::Graphics& g, juce::Rectangle<int> canvas) const;

    // ---- 布局 ----
    juce::Rectangle<int> getToolbarBounds(juce::Rectangle<int> content) const;
    juce::Rectangle<int> getCanvasBounds (juce::Rectangle<int> content) const;

    // 频率 → X 像素坐标（对数）
    static float freqToX(float freqHz, juce::Rectangle<int> canvas);
    // X 像素坐标 → 频率
    static float xToFreq(float x, juce::Rectangle<int> canvas);
    // dBFS → Y 像素坐标
    float dbToY(float db, juce::Rectangle<int> canvas) const;

    // ---- 成员 ----
    AnalyserHub& hub;

    // 原始幅度快照（1024 个线性）
    juce::Array<float> rawMags;

    // 平滑后的 dB 值（按显示列数）
    std::vector<float> smoothedDb;
    // 峰值保持（dB）+ 保持计时
    std::vector<float> peakDb;
    std::vector<float> peakHoldMs;

    // 每帧复用的显示层缓冲，避免 rebuild/paint 中反复分配。
    std::vector<float> blurredDb;
    std::vector<float> slopeOffsetDb;
    int    slopeCacheSize = 0;
    double slopeCacheSampleRate = 0.0;

    // paintContent 是 const 路径，使用 mutable 缓存承接曲线点数组。
    mutable std::vector<juce::Point<float>> curvePts;
    mutable std::vector<juce::Point<float>> peakCurvePts;
    mutable juce::Path fillPath;
    mutable juce::Path curvePath;
    mutable juce::Path peakPath;
    mutable juce::Path dashedPeakPath;

    // 显示参数
    static constexpr float minFreqHz = 20.0f;
    static constexpr float maxFreqHz = 20000.0f;
    static constexpr float minDb     = -80.0f;
    static constexpr float maxDb     = 0.0f;
    static constexpr float peakHoldDuration = 2000.0f; // 2s
    static constexpr float peakFallRate     = 0.5f;     // dB/frame 下降率

    bool peakHoldEnabled = true;
    bool slopeEnabled    = true;

    juce::Time lastTickTime;

    // 性能优化（阶段1）：按 UI 缩放动态限制最短重绘间隔，降低宿主消息线程压力。
    double lastRepaintMs = 0.0;

    // ---- 顶部工具栏 ----
    juce::TextButton btnPeak  { "Peak"  };
    juce::TextButton btnSlope { "Slope" };
    static constexpr int toolbarH = 26;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumModule)
};
