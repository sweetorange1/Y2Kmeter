#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// WaveformModule —— Pink XP 像素风"瀑布式"持续滚动波形
//
//   · 持续向左移动的波形瀑布图：新样本从右边进来，旧样本从左边淡出
//   · 每像素列绘制一根垂直柱，柱高=该列覆盖范围内样本的 min/max 包络
//   · 柱的颜色由该列的 RMS 幅度映射到主题粉色渐变：
//        静音 → pink100  轻柔 → pink300  中等 → pink500  强 → pink700  clip → sel
//   · 数据来源：复用 AnalyserHub::Kind::Oscilloscope 一路（与 OscilloscopeModule 共享）
//     本模块自身**不新增任何后端计算**，仅在 UI 线程中消费已产出的样本
//   · 按需计算：构造时 retain(Oscilloscope)；析构时 release；关闭模块 → 引用计数归零 →
//     AnalyserHub::pushStereo 不再推送 oscilloscope 数据
//
// 顶部工具栏：
//   ┌───────────────────────────────────────────┐
//   │  Range:[3s] [6s]           [Freeze]        │
//   ├───────────────────────────────────────────┤
//   │           瀑布画布（凹陷）                 │
//   └───────────────────────────────────────────┘
// ==========================================================

class WaveformModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit WaveformModule(AnalyserHub& hub);
    ~WaveformModule() override;

    // Hub 分发器回调（每帧把新样本追加到内部环形历史缓冲）
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // 显示时长（秒）：缓冲容量按此 × 采样率分配
    void setDisplaySeconds (float seconds);
    float getDisplaySeconds() const noexcept { return displaySeconds; }

    void setFrozen(bool b);
    bool isFrozen() const noexcept { return frozen; }

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // ---- 内部辅助 ----
    void drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawWaveform  (juce::Graphics& g, juce::Rectangle<int> canvas) const;

    // 幅度 [0,1] → Pink XP 主题色
    juce::Colour mapAmplitudeToColour (float amp01) const;

    // 从 oscilloscope 快照末尾追加 numSamples 个新样本到"采样累加器"；
    // 每当累加器里的样本数 >= samplesPerColumn 时，就产出一个新的像素列并
    // 追加到 columnHistory。核心目的：让屏幕滚动始终按"整数像素列"推进，
    // 彻底消除子像素抖动。
    void pushHistorySamples (const float* L, const float* R,
                             int snapshotLen, int numToAppend);

    // 根据当前 canvasWidth + displaySeconds 重建列缓冲 & samplesPerColumn
    void rebuildColumnBuffer (int newCanvasWidth);

    juce::Rectangle<int> getToolbarBounds (juce::Rectangle<int> content) const;
    juce::Rectangle<int> getCanvasBounds  (juce::Rectangle<int> content) const;

    // ---- 成员 ----
    AnalyserHub& hub;

    // 每个像素列的聚合数据（min/max 包络 + RMS，颜色由 RMS 决定）
    struct Column
    {
        float minV = 0.0f;
        float maxV = 0.0f;
        float rms  = 0.0f;
    };

    // 列环形缓冲：每个元素 = 1 个屏幕像素列的已聚合数据
    //   writeCol 指向"下一个要写的位置"，最新列在 (writeCol - 1) 处
    //   数据有效区间：最近 validCols 个列（<= columnHistory.size()）
    std::vector<Column> columnHistory;
    int   writeCol  = 0;
    int   validCols = 0;

    // 当前正在累加的列（原始样本先进这里，凑够 samplesPerColumn 个样本才
    // 产出一个 Column 并入 columnHistory）
    float accMin  =  1.0f;
    float accMax  = -1.0f;
    float accSumSq = 0.0f;
    int   accCount = 0;

    // 每列应容纳的样本数（固定：displaySeconds × sampleRate / canvasWidth）
    float samplesPerColumn = 0.0f;
    int   cachedCanvasWidth = 0;

    // 显示参数
    float displaySeconds = 3.0f;    // 默认显示最近 3 秒
    bool  frozen         = false;

    // 上次 onFrame 的系统时间（用于估算该 append 多少新样本）
    double lastFrameTimeMs = 0.0;
    double cachedSampleRate = 0.0;  // 缓存 hub 的采样率（避免每帧查询）
    double lastRepaintMs = 0.0;

    // 顶部按钮
    juce::TextButton btnRange3  { "3s"   };
    juce::TextButton btnRange6  { "6s"   };
    juce::TextButton btnFreeze  { "Freeze" };

    // 右侧增益滑条（垂直，0 ~ +36 dB 显示放大；默认 0 dB 即无放大）
    //   · 只作用于 UI 绘制阶段的"柱高缩放系数"，绝对不碰任何后端音频数据
    //   · 视觉样式与 EqModule 的 SIZE 滑条保持一致（顶部 Label + 垂直滑条）
    juce::Slider gainSlider;
    juce::Label  gainLabel;
    float        gainDb = 0.0f;  // 默认 0 dB（无放大）

    static constexpr int toolbarH = 26;

    // 主题订阅 token：切换主题时重新下发 gainLabel 的 textColourId，
    //   避免 Label 缓存的旧 ink 颜色在主题切换后与标题栏不一致。
    int themeSubToken = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformModule)
};
