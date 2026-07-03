#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// Spectrogram3DModule —— Pink XP 像素风"3D 频谱瀑布图"
//
// 功能概述：
//   * 订阅 AnalyserHub::Kind::Spectrum（复用 SpectrumModule 同一路
//     FFT 幅度快照）—— 后端音频线程零新增计算。
//   * 视觉效果：斜四十五度俯视观察频谱瀑布。新一帧 FFT 数据出现在
//     画面"前方"（右下），旧数据随时间向远后方（左上）消退。
//     每个频率点的幅度映射为高度，形成类似三维曲面图的立体效果。
//   * 深度方向绘制 historyLength 层时间切片，由后向前（远→近）
//     逐层 fillPath，实现正确的画家算法遮挡关系。
//   * 颜色跟随当前主题：低幅度→深色，高幅度→亮色（pink 单色渐变）。
//     深度方向叠加 fade-out：旧切片逐渐向背景色过渡。
//   * 右侧 SPEED 滑条控制瀑布滚动速度（像素/秒），与 UI 帧率解耦。
//
// 数据源：AnalyserHub::getSpectrumMagnitudesBlended（低频 8192FFT + 高频 2048FFT）
// ==========================================================

class Spectrogram3DModule : public ModulePanel,
                             public AnalyserHub::FrameListener
{
public:
    explicit Spectrogram3DModule (AnalyserHub& hub);
    ~Spectrogram3DModule() override;

    // AnalyserHub::FrameListener
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void layoutContent (juce::Rectangle<int> contentBounds) override;
    void paintContent  (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    void resized() override;

private:
    // ---- 布局 ----
    juce::Rectangle<int> getCanvasBounds (juce::Rectangle<int> content) const;

    // ---- 环形历史缓冲 ----
    void ensureHistory (int newLength);
    void pushFrame (const juce::Array<float>& mags);

    // ---- 投影与坐标 ----
    // 根据当前 canvas 尺寸计算 isometric 投影参数（自适配缩放）
    void recomputeProjection (int canvasW, int canvasH);

    // 强度 t∈[0,1] + 深度 d∈[0,1]（0=最新,1=最旧）→ 蓝→红热力图颜色
    static juce::Colour valueToColour (float t, float depthFade) noexcept;

    // 频率 → 屏幕 X（等距线性映射）
    float freqToScreenX (int binIndex, int totalBins) const;

    // 绘制底衬 / 轴标签
    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawAxisLabels (juce::Graphics& g, juce::Rectangle<int> canvas) const;

    // ---- 成员 ----
    AnalyserHub& hub;

    // 环形历史缓冲：每行是一个时间切片（numBins 个频率点的线性幅度）
    std::vector<std::vector<float>> historyRing;

    int  numBins    = 128;          // 频率 bin 数量
    int  historyLen = 0;            // 当前历史长度（= historyRing.size()）
    int  writeIdx   = 0;            // 环形写入指针
    int  frameCount = 0;            // 已写入的总帧数（用于判断是否填满）

    // ---- 投影参数（由 recomputeProjection 按 canvas 大小计算）----
    float projOriginX  = 0.0f;      // 最新切片左下角 X
    float projOriginY  = 0.0f;      // 最新切片左下角 Y
    float projSlantX   = 0.0f;      // 每层深度向右的偏移（px）
    float projSlantY   = 0.0f;      // 每层深度向上的偏移（px）
    float projBinWidth = 0.0f;      // 每频率 bin 的屏幕宽度（px）
    float projMaxH     = 0.0f;      // 最大幅度对应的视觉高度（px）

    // ---- 滚动速度解耦 ----
    float  pixelsPerSecond  = 60.0f;  // 默认 60 px/s
    double lastFrameMs      = 0.0;
    float  columnAccumulator = 0.0f;

    // ---- 右侧控制条 ----
    juce::Slider speedSlider;
    juce::Label  speedLabel;
    static constexpr int sliderPanelW = 42;

    int themeSubToken = -1;

    // 画布尺寸缓存（判断是否需要重新计算投影）
    int lastCanvasW = -1;
    int lastCanvasH = -1;
    int lastCachedFrameCount = -1;

    // repaint 节流（P1-1 风格）：限制最短重绘间隔，降低宿主消息线程压力
    double lastRepaintMs = 0.0;

    // 缓冲区复用
    juce::Array<float> rowMagBuf;

    // 显示参数
    static constexpr float minFreqHz = 20.0f;
    static constexpr float maxFreqHz = 20000.0f;
    static constexpr float minDb     = -90.0f;
    static constexpr float maxDb     = 0.0f;
    static constexpr int   defaultHistoryLen = 500;  // 环形缓冲总容量（远大于可见层数，旧层滚出屏幕后延迟覆盖）
    static constexpr int   visibleRows      = 300;  // 屏幕可见层数：投影计算只考虑这 150 层，其余在画布外

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Spectrogram3DModule)
};