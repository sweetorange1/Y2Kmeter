#pragma once

#include <JuceHeader.h>
#include <vector>

#include "source/analysis/AnalyserHub.h"
#include "source/analysis/SibilanceDetector.h"
#include "source/ui/GlDirectModule.h"

// ==========================================================
// WaveformModule —— Pink XP 像素风"滚动波形历史"
//
//   · 持续向左推进的波形包络瀑布：新样本从右边进来，旧样本从左边被覆盖
//   · SPEED 滑条 = 滚动速度（像素列/秒，与 SpectrogramModule 同口径）：
//       每列聚合 sampleRate / pxPerSec 个样本，与模块宽度无关
//       —— 拉宽模块只是看到更长的历史，画面推进速度不变
//   · 每个屏幕像素列 = 一根 1px 竖柱（该列覆盖样本的 min/max 包络）
//   · 颜色来源（工具栏 PITCH / LOUD / SIB 三选一，默认 PITCH）：
//       PITCH —— 真正的音高染色：chroma 折叠 → 12 音级 → 色相
//       LOUD  —— 响度（dBFS）映射到主题 chartRamp 渐变
//       SIB   —— 齿音（5 kHz 以上能量占比）
//   · 透明度：列峰值 dBFS 越低越透（clamp((db + 80) / 21)），
//     静音列整列不画 —— 干净观感的关键
//   · 数据来源：AnalyserHub::Kind::Oscilloscope（波形 / 齿音）
//                AnalyserHub::Kind::Spectrum（PITCH 模式的音高估计）
//                —— 仅 PITCH 模式下 retain，切走即 release
//
// 顶部工具栏：
//   ┌──────────────────────────────────────────────┐
//   │ [PITCH][LOUD][SIB]                [Freeze]   │  [SPEED]
//   ├──────────────────────────────────────────────┤  滑条
//   │                瀑布画布（凹陷）               │  (1024…
//   └──────────────────────────────────────────────┘   16384)
// ==========================================================

class WaveformModule : public GlDirectModuleBase,
                       public AnalyserHub::FrameListener
{
public:
    // 颜色来源
    enum class ColorSource
    {
        pitch     = 0,   // 真正的音高染色（默认）
        loudness  = 1,   // 响度映射
        sibilance = 2    // 齿音（保留的旧模式）
    };

    // SPEED 滑条：滚动速度（像素列 / 秒），与 SpectrogramModule 同一套口径。
    //   · 直接决定"每像素列聚合多少个音频样本"：samplesPerColumn = sampleRate / pxPerSec
    //     —— 与模块宽度**无关**：拉宽模块只是看到更长的历史，画面推进速度不变。
    //   · 默认 120 px/s：384 px 宽 ≈ 3.2 s 窗口（与旧版 3s 档观感接近）。
    static constexpr float kMinPixelsPerSecond     = 10.0f;
    static constexpr float kMaxPixelsPerSecond     = 800.0f;
    static constexpr float kDefaultPixelsPerSecond = 120.0f;

    explicit WaveformModule (AnalyserHub& hub);
    ~WaveformModule() override;

    // Hub 分发器回调（每帧把新样本追加到内部列缓冲）
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // 鼠标悬停标尺：仪表区域内十字线 + 时间/响度（PITCH 模式追加音名）读数
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    // ---- 滚动速度（SPEED 滑条，像素列/秒）----
    void  setPixelsPerSecond (float pxPerSec);
    float getPixelsPerSecond() const noexcept { return pixelsPerSecond; }

    // 派生量：当前可见窗口的时长 = canvasWidth / pixelsPerSecond
    //   （悬停标尺、Toybox 读数、左下角提示用；会随模块宽度变化）
    float getDisplaySeconds() const noexcept;

    void setColorSource (ColorSource s);
    ColorSource getColorSource() const noexcept { return colorSource; }

    void setFrozen (bool b);
    bool isFrozen() const noexcept { return frozen; }

    // 持久化：pixelsPerSecond + colorSource + frozen
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;

    // ---- Toybox（v3.0.0）：人浪（§8.3 / 反馈 C3 加电平读数）----
    juce::Rectangle<int> getToyCanvasBounds() const { return getCanvasBounds (getContentBounds()); }
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

    // 读点击处列的电平峰值（波形幅度 -1..1 的绝对最大值；无数据返回 0）
    float toyReadColumnPeak (float x) const
    {
        const auto canvas = getCanvasBounds (getContentBounds());
        if (canvas.getWidth() <= 0 || validCols <= 0 || columnHistory.empty())
            return 0.0f;
        const int N = (int) columnHistory.size();
        const int colIdx = juce::jlimit (0, validCols - 1,
            juce::roundToInt ((x - (float) canvas.getX()) / (float) canvas.getWidth() * (float) (validCols - 1)));
        const int idx = ((writeCol - validCols + colIdx) % N + N) % N;
        const auto& col = columnHistory[(size_t) idx];
        return juce::jmax (std::abs (col.minV), std::abs (col.maxV));
    }

protected:
    void layoutContent (juce::Rectangle<int> contentBounds) override;

    // ---- GlDirectModuleBase 钩子 ----
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // 每个像素列的聚合数据
    struct Column
    {
        float minV  = 0.0f;
        float maxV  = 0.0f;
        float rms   = 0.0f;
        float db    = -96.0f;     // 本列峰值 dBFS（透明度与响度配色用）
        float sib   = 0.0f;       // 本列齿音强度 0..1（带通 + 包络 + 迟滞门限检测器输出）
        float pitchNote = 60.0f;  // 产出该列时的平滑 MIDI 音高（音域 → 主题色阶位置）
        float pitchConf = 0.0f;   // 产出该列时的音高置信度
    };

    // 单列的绘制参数 —— GL 与 legacy 两条路径共用同一套解算，保证观感一致
    struct ColumnPaint
    {
        int          yMin  = 0;
        int          hLine = 1;
        juce::Colour colour { juce::Colours::transparentBlack };
    };

    bool resolveColumn (int xi, int w, int innerY, int innerH, int centreY,
                        ColumnPaint& out) const;

    // ---- 颜色映射 ----
    //   PITCH / LOUD 的透明度由电平 dB 决定；SIB 的"存在感"由齿音强度决定
    //   （没有齿音的列压成低存在感灰、有齿音的列用红色），所以颜色与透明度
    //   必须一起返回。
    struct ColumnStyle
    {
        juce::Colour colour;
        float        alpha = 1.0f;
    };

    ColumnStyle columnStyle (const Column& c, float level01, float levelAlpha) const;

    // 音高染色 = 把平滑后的 MIDI 音高映射到主题色阶（C0 … C7 → 色阶 0 … 1），
    //   与 LOUD 模式同一套 chartRamp，只是驱动量换成音高。
    juce::Colour mapPitchToColour (float noteNumber, float confidence, float level01) const;
    juce::Colour mapLoudnessToColour (float db) const;

    // ---- 列推进 ----
    // 从 oscilloscope 快照末尾追加 numToAppend 个新样本；用"倒计时"推进列，
    // 保证长期平均列率精确且不丢样本（旧实现的余数回填会被直接丢弃）
    void pushHistorySamples (const float* L, const float* R, int snapshotLen, int numToAppend);

    void rebuildColumnBuffer (int newCanvasWidth);
    void updateSamplesPerColumn();
    void resetAccumulator();

    void refreshColourButtons();

    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawWaveform  (juce::Graphics& g, juce::Rectangle<int> canvas) const;

    juce::Rectangle<int> getToolbarBounds (juce::Rectangle<int> content) const;
    juce::Rectangle<int> getCanvasBounds  (juce::Rectangle<int> content) const;


    // ---- 成员 ----
    AnalyserHub& hub;

    // 列环形缓冲：每个元素 = 1 个屏幕像素列的已聚合数据
    std::vector<Column> columnHistory;
    int   writeCol  = 0;
    int   validCols = 0;

    // 当前正在累加的列
    float accMin  =  1.0f;
    float accMax  = -1.0f;
    float accSumSq = 0.0f;
    float accSibMax = 0.0f;      // 本列内齿音强度的峰值保持
    int   accCount = 0;

    // 齿音检测（带通 + 包络 + 迟滞门限）
    //   逐样本推进，时间常数固定 —— 不随 SPEED / 列宽变化。
    y2k::sibilance::Detector sibDetector;

    // 列推进
    float samplesPerColumn       = 0.0f;   // = sampleRate / pixelsPerSecond（≥ 1）
    float samplesUntilNextColumn = 0.0f;   // 距下一列还差多少样本（倒计时）
    int   cachedCanvasWidth      = 0;      // 只用于把"可见时长"换算出来

    // 按"音频线程累计写入样本数"推进，而不是按帧时钟估算
    //   —— 估算会重复/跳过样本，同一列混进两个相位 → 波形局部变粗。
    juce::uint64 lastOscTotal = 0;
    bool         oscTotalValid = false;

    float pixelsPerSecond = kDefaultPixelsPerSecond;   // SPEED（像素列/秒）

    // 颜色来源 + Spectrum 订阅状态（PITCH 模式才需要频谱）
    ColorSource colorSource      = ColorSource::pitch;
    bool        spectrumRetained = false;

    // 最近一帧的音高估计（新产出的列会带上它）
    float currentPitchNote = 60.0f;
    float currentPitchConf = 0.0f;

    bool   frozen = false;
    double cachedSampleRate = 48000.0;   // 缓存 hub 的采样率（避免每帧查询）

    // 顶部按钮
    juce::TextButton btnPitch  { "PITCH" };
    juce::TextButton btnLoud   { "LOUD"  };
    juce::TextButton btnSib    { "SIB"   };
    juce::TextButton btnFreeze { "Freeze" };

    // 右侧 SPEED 滑条（垂直）：单位 px/s（像素列/秒），顶 = 快，底 = 慢
    juce::Slider speedSlider;
    juce::Label  speedLabel;

    static constexpr int toolbarH       = 26;
    static constexpr int speedPanelW    = 48;
    static constexpr int speedLabelH    = 14;

    // ---- 鼠标悬停标尺 ----
    void drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas);
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    // 主题订阅 token：切换主题时重新下发 SPEED 标签 / 滑条文字颜色
    int themeSubToken = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformModule)
};
