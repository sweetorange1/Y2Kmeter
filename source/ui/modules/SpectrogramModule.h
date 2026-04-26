#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// SpectrogramModule —— Pink XP 像素风"实时频谱瀑布图"
//
// 功能概述：
//   * 订阅 AnalyserHub::Kind::Spectrum（复用 SpectrumModule 用的同一路
//     1024 bin FFT 幅度快照）—— 后端音频线程零新增计算。
//   * 把画布切成一张【方格网】：行数 rows × 列数 cols。默认每格是
//     cellPx × cellPx 的正方形像素块；当用户拉伸模块时，格子数量
//     保持不变，每格的 cellW / cellH 自动按画布尺寸重算 —— 这时
//     格子变成统一的长方形（所有格子仍然一致）。
//   * 每帧根据当前 FFT 幅度算一列新的网格值（对数频率纵轴、dBFS →
//     归一化强度 t∈[0,1]），环形写入；paint 时用 fillRect 画出整张网格。
//   * 颜色：跟随当前主题，从 pink700 (深) → pink100 (浅) 线性渐变，
//     对应 低电平 → 高电平。切换主题时全局调色板会自动刷新。
//
// 数据源：AnalyserHub::getSpectrumMagnitudesBlended（低频 8192FFT + 高频 2048FFT 双路合并）
// ==========================================================

class SpectrogramModule : public ModulePanel,
                          public AnalyserHub::FrameListener
{
public:
    explicit SpectrogramModule (AnalyserHub& hub);
    ~SpectrogramModule() override;

    // AnalyserHub::FrameListener
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // 默认每格像素尺寸（正方形），决定"默认状态下"方格是多大像素
    void  setCellPx (int px);
    int   getCellPx() const noexcept { return cellPx; }

protected:
    void layoutContent (juce::Rectangle<int> contentBounds) override;
    void paintContent  (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    void resized() override;

private:
    // ---- 布局 ----
    juce::Rectangle<int> getCanvasBounds (juce::Rectangle<int> content) const;

    // 确保 grid 的 rows/cols 与 canvas 尺寸 + cellPx 一致；变化则重建
    void ensureGrid (int canvasW, int canvasH);

    // 把当前已合并到「rows 个对数频率点」的线性幅度写到 grid 的 writeCol 列
    void pushColumn (const juce::Array<float>& rowsMags);

    // 方案 B：把 grid 的"最新一列强度"写入离屏 Image 的对应像素列。
    //   · Image 的每个像素 = grid 的一格（按 Image 尺寸等比映射），
    //     paint 时一次性 drawImage 贴屏，开销 O(canvas_pixels) 但常数极低，
    //     远低于 rows×cols 次 fillRect。
    //   · 只重绘"新进来的一列"，其它列完全不动。
    void writeLatestColumnToImage();

    // 方案 B：根据当前 rows / cols 重建离屏 Image（仅在尺寸变化时）
    void ensureImage();

    // 方案 B：把整个 gridData 重新画到 Image 上（尺寸变化/首帧时用）
    void redrawFullImage();

    // 左侧滚动的推进速度控制
    //   pixelsPerSecond = 目标"滚动速度"（像素列/秒），与 UI 帧率解耦
    //   —— 不论 Hub 30/60 Hz 分发，画面横向推进速度始终目标押到
    //   pixelsPerSecond 列/秒。

    // 强度 t∈[0,1] → 色彩：跟随主题的 pink700..pink100 深→浅渐变
    static juce::Colour intensityToColour (float t) noexcept;

    // 频率 → 网格行号（0 = 顶 = 高频；rows-1 = 底 = 低频）
    static int freqToGridRow (double freqHz, double minHz, double maxHz, int rows) noexcept;

    // 绘制底衬、轴标签
    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawAxisLabels (juce::Graphics& g, juce::Rectangle<int> canvas) const;

    // 取 grid 里第 c 列第 r 行的强度（环形列）
    inline float at (int r, int c) const noexcept
    {
        return gridData[(size_t) (r * cols + c)];
    }
    inline void set (int r, int c, float v) noexcept
    {
        gridData[(size_t) (r * cols + c)] = v;
    }

    // ---- 成员 ----
    AnalyserHub& hub;

    // 每格默认边长（像素）。默认状态下格子是 cellPx × cellPx 的正方形；
    // 拉伸模块时格子数量保持不变，像素尺寸会统一变成长方形。
    int cellPx = 1;

    // 网格尺寸（rows = 纵向频率数，cols = 横向时间数）
    int rows = 0;
    int cols = 0;

    // 行×列 的强度网格，值域 [0,1]。按行优先布局：index = r*cols + c
    std::vector<float> gridData;

    // 环形列写入指针：下一列要写的位置
    int writeCol = 0;

    // ---- 方案 B：离屏 Image 缓存 ----
    //   imageBuf 的宽 = cols，高 = rows，一像素对应一格。
    //   paint 时 drawImage(imageBuf, canvas, ...) 直接缩放贴屏，
    //   不再每帧 rows×cols 次 fillRect。
    juce::Image imageBuf;

    // 画布尺寸缓存（仅用于判断是否需要重建 grid）
    int lastCanvasW = 0;
    int lastCanvasH = 0;

    // 每帧复用的"rows 长度线性幅度"缓冲，避免 onFrame 里反复分配
    juce::Array<float> rowMagBuf;

    // ---- 滚动速度解耦 —— 按实时时间推进列数，清除 "帧率×1 列"的耦合 ----
    float  pixelsPerSecond  = 60.0f;   // 默认 60 px/s（滚动速度，与 UI 帧率解耦）
    double lastFrameMs      = 0.0;     // 上一次 onFrame 时刻（毫秒）
    float  columnAccumulator = 0.0f;   // 累计的小数列值，⏲外部当 1 列抽走

    // ---- 右侧控制条（样式与 EqModule 的 SIZE 滑条一致）----
    juce::Slider speedSlider;
    juce::Label  speedLabel;
    static constexpr int sliderPanelW = 42;  // 右侧滑条面板宽度（与 WaveformModule 的 GAIN 面板一致）

    // 主题订阅 token：切换主题时重新下发 Label / Slider textBox 的墨色
    int themeSubToken = -1;

    // 显示参数
    static constexpr float minFreqHz = 20.0f;
    static constexpr float maxFreqHz = 20000.0f;
    static constexpr float minDb     = -90.0f; // 最暗端
    static constexpr float maxDb     =   0.0f; // 最亮端

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramModule)
};
