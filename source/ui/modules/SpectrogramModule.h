#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>
#include "source/ui/GlDirectModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoldSlowMo.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// SpectrogramModule —— 高可调实时频谱瀑布图（v3.5 视觉重构版）
//
//   核心特性：
//     * 订阅 AnalyserHub::Kind::Spectrum（复用 SpectrumModule 同一路 FFT 快照），
//       后端音频线程零新增计算。
//     * gridData 存原始 dB，使得调色盘 / 底噪门限（Floor）/ 峰值上限（Ceiling）/
//       对比度（Contrast）/ 高频倾斜（Tilt）都能**实时重绘整屏历史**（含冻结态）。
//     * 3 档清晰度：Classic（平滑原谱）/ Sharp（频率重分配叠加经典底衬）/
//       Sharper（纯频率重分配粒子束，低频 bassline 完全收束）+ 可调时间平滑（Time smooth）。
//     * 8 套色谱（增强多段 Theme / Heat / Grey / Ice / Magma / Inferno / Plasma / Viridis）、
//       平滑插值（GL_LINEAR）与像素块粒度（1..4 px）自由切换，兼顾专业高清与 Y2K 像素风。
//     * 4 档频率视野（Full / Music / Vocal / Bass）+ 可开关参考网格与刻度标签；
//       悬停即显示频率 + 相对时间 + 音名，按住画布冻结画面。
// ==========================================================

class SpectrogramModule : public GlDirectModuleBase,
                          public AnalyserHub::FrameListener
{
public:
    explicit SpectrogramModule (AnalyserHub& hub);
    ~SpectrogramModule() override;

    // AnalyserHub::FrameListener
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // 鼠标悬停标尺：仪表区域内十字线 + 频率 / 时间 / 音名读数
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    // ---- 按住冻结：按住画布左键 → 停止推进新列（画面完全静止），松手继续滚动 ----
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

    // 像素块粒度（1 = 精细高清，2..4 = 复古像素块）
    void  setCellPx (int px);
    int   getCellPx() const noexcept { return cellPx; }

    // ---- 显示模式（Classic / Sharp / Sharper）----
    enum class DisplayMode : int { Classic = 0, Sharp = 1, Sharper = 2 };
    void        setDisplayMode (DisplayMode m);
    DisplayMode getDisplayMode() const noexcept { return displayMode; }

    // 滚动方向：Left（默认）新数据从右侧进入；Up/Down 时间纵向、频率横向
    enum class Direction : int { Left = 0, Right = 1, Up = 2, Down = 3 };

    // 颜色色谱（0..3 保持旧存档索引兼容；4..7 为新增专业感知均匀色谱）
    enum class ColourScheme : int
    {
        Theme   = 0,
        Heat    = 1,
        Grey    = 2,
        Ice     = 3,
        Magma   = 4,
        Inferno = 5,
        Plasma  = 6,
        Viridis = 7
    };

    // 频率视野预设：全频 / 音乐主频 / 人声 / 低频重低音
    enum class FreqRange : int
    {
        Full  = 0,   // 20 Hz - 20 kHz
        Music = 1,   // 30 Hz - 12 kHz
        Vocal = 2,   // 80 Hz - 8 kHz
        Bass  = 3    // 20 Hz - 1 kHz
    };

    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;

    // ---- 控制面板参数 ----
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    void setDirection     (Direction d);
    void setColourScheme  (ColourScheme c);
    void setFftScale      (y2k::fscale::Scale s);
    void setScrollSpeed   (float pxPerSec);
    void setFloorDb       (float db);
    void setCeilDb        (float db);
    void setContrast      (float g);
    void setSlopeDbPerOct (float dbPerOct);
    void setTimeSmooth    (float pct0To80);
    void setSmoothPixels  (bool on);
    void setFreqRange     (FreqRange r);
    void setShowGrid      (bool on);
    void setShowLabels    (bool on);

    // ---- Toybox ----
    juce::Rectangle<int> getToyCanvasBounds() const { return getCanvasBounds (getContentBounds()); }
    float getToyScrollPxPerSec() const { return pixelsPerSecond; }
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

protected:
    void layoutContent (juce::Rectangle<int> contentBounds) override;
    void resized() override;

    // ---- GlDirectModuleBase 钩子（纹理通道）----
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    juce::Rectangle<int> getChromaKeyCanvas() override { return getCanvasBounds (getContentBounds()); }

private:
    static constexpr float kSilenceDb = -144.0f;

    juce::Rectangle<int> getCanvasBounds (juce::Rectangle<int> content) const;

    float activeMinFreqHz() const noexcept;
    float activeMaxFreqHz() const noexcept;

    void ensureGrid (int canvasW, int canvasH);
    void pushColumn (const std::vector<float>& rowsDb);
    void writeLatestColumnToImage();
    void ensureImage();
    void redrawFullImage();

    // 重建 256 级调色 LUT（含 Contrast 幂曲线与绿幕抠像）及每行频率倾斜补偿表
    void rebuildPaletteLut();
    void rebuildRowTiltLut();
    void invalidateVisualGrading (bool paletteDirty = true, bool tiltDirty = true);   // 调色/量程/倾斜改变：保留历史，按需重绘整屏

    juce::Colour intensityToColour (float t) const noexcept;
    inline juce::uint32 dbRowToArgb (float rawDb, int r) const noexcept
    {
        const float tilt = (r >= 0 && r < (int) rowTiltDb_.size()) ? rowTiltDb_[(size_t) r] : 0.0f;
        const float t = juce::jlimit (0.0f, 1.0f, (rawDb + tilt - floorDb) * invDbSpan_);
        const int idx = juce::jlimit (0, 255, (int) (t * 255.0f + 0.5f));
        return paletteLut_[(size_t) idx];
    }

    bool isDarkPalette() const noexcept;

    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawAxisLabels (juce::Graphics& g, juce::Rectangle<int> canvas) const;

    inline float at (int r, int c) const noexcept
    {
        return gridData[(size_t) (r * cols + c)];
    }
    inline void set (int r, int c, float v) noexcept
    {
        gridData[(size_t) (r * cols + c)] = v;
    }

    void buildUnrolledTexture (std::vector<juce::uint32>& out, int& outW, int& outH) const;
    void hoverToFreqAndAge (juce::Rectangle<int> canvas, juce::Point<int> p,
                            float& freqHz, float& secondsAgo) const;

    void refreshSpectrogramColours();
    bool syncAppearanceIfChanged();
    void drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas);

    // ---- 成员 ----
    AnalyserHub& hub;

    int cellPx = 1;
    int rows   = 0;
    int cols   = 0;

    // 行×列 原始 dB 历史网格（r=0 顶=高频，r=rows-1 底=低频），无信号初始化为 kSilenceDb
    std::vector<float> gridData;
    int writeCol = 0;

    juce::Image imageBuf;
    bool imageBufDirty_ = true;
    int lastCanvasW = 0;
    int lastCanvasH = 0;

    // 每帧复用工作缓冲（零堆分配）
    juce::Array<float> rowMagBuf;
    juce::Array<float> classicMagBuf;
    std::vector<float> colDbBuf;
    std::vector<float> smoothColDb;     // Time smooth 状态

    // 256 级 ARGB 查表 + 每行高频倾斜补偿（dB）
    std::array<juce::uint32, 256> paletteLut_ {};
    std::vector<float>            rowTiltDb_;
    double                        tiltSampleRate_ = 0.0;
    float                         invDbSpan_ = 1.0f / 76.0f;

    // GL 增量纹理像素缓存（Left 环形双拷贝 / Right·Up·Down 展开缓存共用）
    std::vector<juce::uint32> texPixelsCache_;
    bool         texCacheValid_      = false;
    int          lastBuiltWriteCol_  = -1;
    int          texCacheW_          = 0;
    int          texCacheH_          = 0;
    Direction    lastBuiltDir_       = Direction::Left;
    juce::uint64 texDataGen_         = 1;     // 纹理内容/过滤/尺寸代次（永不为 0，0 保留为未上传/已重置态）
    bool         lastUploadedLinear_ = true;  // 最近一次触发上传时的 smoothPixels 状态

    // 滚动速度（与 UI 帧率解耦）
    float  pixelsPerSecond   = 60.0f;
    double lastFrameMs       = 0.0;
    float  columnAccumulator = 0.0f;

    // ---- 用户可调参数 ----
    Direction          direction     = Direction::Left;
    ColourScheme       colourScheme  = ColourScheme::Theme;
    DisplayMode        displayMode   = DisplayMode::Sharp;
    y2k::fscale::Scale fftScale      = y2k::fscale::Scale::log;
    FreqRange          freqRange     = FreqRange::Full;

    float floorDb       = -84.0f;   // 底噪门限（-110 .. -30 dB）
    float ceilDb        =  -8.0f;   // 峰值上限（-36 .. 0 dB）
    float contrastGamma =   1.15f;  // 对比度幂曲线（0.40 .. 2.50）
    float slopeDbPerOct =   3.0f;   // 高频倾斜补偿（0.0 .. 9.0 dB/oct，1 kHz 为支点）
    float timeSmoothPct =  15.0f;   // 时间平滑（0 .. 80 %）
    bool  smoothPixels  =  true;    // GL_LINEAR / 中等采样平滑
    bool  showGrid      =  true;    // 细频率参考线
    bool  showLabels    =  true;    // 刻度标签与 older/newer 提示

    bool isVertical() const noexcept { return direction == Direction::Up || direction == Direction::Down; }

    int themeSubToken = -1;
    juce::uint32 lastAppearanceGen_ = 0;
    juce::uint32 lastFontGen_       = 0;
    juce::Colour spectrogramBaseColour_;
    juce::Colour spectrogramAccentColour_;

    // 鼠标悬停标尺
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    // 按住冻结
    HoldSlowMo hold;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramModule)
};
