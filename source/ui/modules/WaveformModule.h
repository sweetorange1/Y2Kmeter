#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

#include "source/analysis/AnalyserHub.h"
#include "source/analysis/SibilanceDetector.h"
#include "source/ui/GlDirectModule.h"
#include "source/ui/HoldSlowMo.h"

// ==========================================================
// WaveformModule —— Pink XP 像素风"滚动波形历史"
//
//   · 持续向左推进的波形包络瀑布：新样本从右边进来，旧样本从左边被覆盖
//   · SPEED 滑条 = 滚动速度（像素列/秒，与 SpectrogramModule 同口径）：
//       每列聚合 sampleRate / pxPerSec 个样本，与模块宽度无关
//       —— 拉宽模块只是看到更长的历史，画面推进速度不变
//   · 每个屏幕像素列 = 一根 1px 竖柱（该列覆盖样本的 min/max 包络）
//   · 颜色来源（工具栏单个按钮循环 PITCH → LOUD → SIB，默认 PITCH）：
//       PITCH —— 真正的音高染色：chroma 折叠 → 12 音级 → 色相
//       LOUD  —— 响度（dBFS）映射到主题 chartRamp 渐变
//       SIB   —— 齿音（5 kHz 以上能量占比）
//   · 透明度：列峰值 dBFS 越低越透（clamp((db + 80) / 21)），
//     静音列整列不画 —— 干净观感的关键
//   · 数据来源：AnalyserHub::Kind::Oscilloscope（波形 / 齿音）
//                AnalyserHub::Kind::Spectrum（PITCH 模式的音高估计）
//                —— 仅 PITCH 模式下 retain，切走即 release
//
// 布局（v3.1）：顶部工具栏 / SPEED 滑条取消，整块内容区都是瀑布画布；
//   着色来源、速度、双声道分轨、齿音阈值都在标题栏齿轮的控制面板里。
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
    void mouseDrag (const juce::MouseEvent& e) override;   // 按住拖动时读数也要跟随鼠标
    void mouseExit (const juce::MouseEvent& e) override;

    // ---- 滚动速度（SPEED 滑条，像素列/秒）----
    void  setPixelsPerSecond (float pxPerSec);
    float getPixelsPerSecond() const noexcept { return pixelsPerSecond; }

    // 派生量：当前可见窗口的时长 = canvasWidth / pixelsPerSecond
    //   （悬停标尺、Toybox 读数、左下角提示用；会随模块宽度变化）
    float getDisplaySeconds() const noexcept;

    void setColorSource (ColorSource s);
    ColorSource getColorSource() const noexcept { return colorSource; }

    // ---- v3.1：按住冻结（取代 Freeze 按钮）----
    //   按住画布左键 → 完全静止：不消费任何样本、不写新列（按下瞬间清空待写列与
    //   半列累加）；松手直接跳回实时，冻结期间的样本丢弃不补播（见 HoldSlowMo.h）。
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

    // 持久化：pixelsPerSecond + colorSource + dual + sibThreshold（旧存档的 frozen 键被忽略）
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;

    // ---- 控制面板（v3.1）----
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;
    void setDualChannel (bool b);
    void setSibilanceThresholdDb (float db);

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
    //   单声道模式：A 组 = (L+R)/2 混音（旧外观）。
    //   双声道模式：A 组 = 左声道，B 组 = 右声道（minV2/maxV2/db2/sib2）。
    struct Column
    {
        float minV  = 0.0f;
        float maxV  = 0.0f;
        float rms   = 0.0f;
        float db    = -96.0f;     // 本列峰值 dBFS（透明度与响度配色用）
        float sib   = 0.0f;       // 本列齿音强度 0..1（带通 + 包络 + 迟滞门限检测器输出）
        float pitchNote = 60.0f;  // 产出该列时的平滑 MIDI 音高（音域 → 主题色阶位置）
        float pitchConf = 0.0f;   // 产出该列时的音高置信度
        float minV2 = 0.0f, maxV2 = 0.0f, db2 = -96.0f, sib2 = 0.0f;   // 双声道 B 组（右声道）
    };

    // 单列的绘制参数 —— GL 与 legacy 两条路径共用同一套解算，保证观感一致
    struct ColumnPaint
    {
        int          yMin  = 0;
        int          hLine = 1;
        juce::Colour colour { juce::Colours::transparentBlack };
    };

    // chanB = true 取 B 组（双声道的右声道）
    bool resolveColumn (int xi, int w, int innerY, int innerH, int centreY,
                        ColumnPaint& out, bool chanB = false) const;

    // 把 inner 区域划分成要绘制的声道区域（单声道 = 1 个，双声道 = 上下 2 个）
    struct Lane { int y; int h; int cy; bool chanB; };
    int computeLanes (juce::Rectangle<int> inner, Lane (&lanes)[2]) const;

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

    // ---- 列写入节流（突发平滑）----
    //   数据是按块到达的：主机块大小、loopback 补零块、甚至"静音后突然补齐"，
    //   一次可能带来几十毫秒的样本。若在同一个分发帧里把它们全写成列，画面会
    //   从"每帧 1px 顺滑左移"变成"几十毫秒不动，然后一下跳好几像素"的闪现。
    //   所以 pushHistorySamples 只负责把完整的列放进待写队列，真正写入环形缓冲
    //   由 drainPendingColumns() 按每帧预算（≈ pixelsPerSecond × dt + 1 列）
    //   分几帧吃掉 —— 稳态下预算从不成为瓶颈（列率仍严格由样本数决定），
    //   突发时则把"跳好几像素"摊成 1~2 px 的连续推进。
    static constexpr int kPendingColumnCap = 128;
    std::array<Column, (size_t) kPendingColumnCap> pendingColumns {};
    int pendingHead  = 0;   // 最旧待写列
    int pendingCount = 0;   // 待写列数

    void queueColumn (const Column& c);
    int  drainPendingColumns (int maxColumns);
    int  columnBudgetForDt (float dtSec) const;
    void clearPendingColumns();

    void rebuildColumnBuffer (int newCanvasWidth);
    void updateSamplesPerColumn();
    void resetAccumulator();

    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawWaveform  (juce::Graphics& g, juce::Rectangle<int> canvas) const;

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
    float accMin2 = 1.0f, accMax2 = -1.0f, accSibMax2 = 0.0f;   // 双声道 B 组

    // 齿音检测（带通 + 包络 + 迟滞门限）
    //   逐样本推进，时间常数固定 —— 不随 SPEED / 列宽变化。
    //   单声道模式用立体声联动的 sibDetector（旧外观）；双声道模式左右各一个，
    //   各自只看自己的声道。
    y2k::sibilance::Detector sibDetector;
    y2k::sibilance::Detector sibDetectorL, sibDetectorR;
    float sibThresholdDb = y2k::sibilance::Detector::kDefaultThresholdDb;

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
    bool        dualChannel      = false;   // 双声道：上 L / 下 R 两条波形

    // 最近一帧的音高估计（新产出的列会带上它）
    float currentPitchNote = 60.0f;
    float currentPitchConf = 0.0f;

    // 按住冻结（v3.1：不再慢放滚动，按住期间画面完全静止）
    HoldSlowMo hold;

    double cachedSampleRate = 48000.0;   // 缓存 hub 的采样率（避免每帧查询）

    // ---- 鼠标悬停标尺 ----
    void drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas);
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    // 主题订阅 token（保留：切换主题时 GL 快照要重建）
    int themeSubToken = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformModule)
};
