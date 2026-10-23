#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/GlDirectModule.h"
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

class SpectrumModule : public GlDirectModuleBase,
                       public AnalyserHub::FrameListener
{
public:
    explicit SpectrumModule(AnalyserHub& hub);
    ~SpectrumModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // 鼠标悬停标尺：仪表区域内十字线 + 频率/响度读数
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    void setPeakHoldEnabled(bool b);
    bool isPeakHoldEnabled() const noexcept { return peakHoldEnabled; }

    void setSlopeEnabled(bool b);
    bool isSlopeEnabled() const noexcept { return slopeEnabled; }

    // v1.8.4 持久化：保存/恢复 peakHoldEnabled + slopeEnabled
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState(const juce::ValueTree& state) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;

    // ---- GlDirectModuleBase 钩子（v2.9.4 迁移）----
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

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
    // 峰值（dB）—— 持续下落：信号超过就顶上去，否则以 peakFallRatePerSec 回落
    std::vector<float> peakDb;

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
    static constexpr float peakFallRatePerSec = 6.0f;    // dB/s 持续下降速率（无 hold/阈值，peak 线始终缓慢下落）

    bool peakHoldEnabled = true;
    bool slopeEnabled    = true;

    juce::Time lastTickTime;
    float      frameDtSec = 1.0f / 120.0f; // v2.9：本帧间隔（rebuildDisplay 平滑换算用）



    // v2.9：静态层缓存（底衬 + 凹陷画布 + 网格 + 坐标轴文字），仅在尺寸 / DPI /
    //   主题配色变化时重建；GL 渲染器对未修改的 Image 复用纹理，不再每帧上传。
    //   曲线（填充 + 双层描边 + 峰值虚线）每帧直接画到 GL 上下文。
    //   旧实现每帧"新建整张 ARGB Image → 软件光栅化全部内容 → 整图上传纹理"，
    //   在 120Hz 下是本模块最大的开销。
    mutable juce::Image  staticLayer_;
    mutable juce::Rectangle<int> staticLayerBounds_;
    mutable float        staticLayerScale_     = 0.0f;
    mutable juce::uint32 staticLayerColourKey_ = 0;
    mutable bool         curve_cache_dirty_    = true; // 兼容保留：onFrame 标脏（不再触发重建）
    void renderStaticLayer (juce::Rectangle<int> content, float physicalScale) const;
    static juce::uint32 computeThemeColourKey() noexcept;

    // ---- 顶部工具栏 ----
    juce::TextButton btnPeak  { "Peak"  };
    juce::TextButton btnSlope { "Slope" };
    static constexpr int toolbarH = 26;

    // ---- 鼠标悬停标尺 ----
    void drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas);
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumModule)
};
