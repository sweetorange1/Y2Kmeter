#pragma once

#include <JuceHeader.h>
#include <array>
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
    // 按住拖动时 JUCE 只派发 mouseDrag（不再发 mouseMove）—— 标尺要跟着走
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    // ---- 子弹时间（Bullet Time）----
    //   鼠标按住频谱画布期间，频谱 release（曲线回落 / 峰值下落）速度放慢
    //   50 倍（等价时间膨胀：dt/50），松手立即恢复正常。
    //   同时曲线切换为"残影色"（按住时满值，松手后按 ghostFadeSec 渐隐恢复原配色）。
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    bool isBulletTimeActive() const noexcept { return bulletTimeActive; }

    void setPeakHoldEnabled(bool b);
    bool isPeakHoldEnabled() const noexcept { return peakHoldEnabled; }

    // ---- 曲线模式（v3.1，按钮循环 Smooth → Semi → Raw）----
    //   smooth：256 列对数轴 + 频段保峰聚合 + 3 列模糊 + Catmull-Rom（最平滑）
    //   semi  ：逐 FFT bin（每个 bin 一个点，放在 bin 中心频率上）+ 无模糊
    //           + 样条连接 —— 低频点少自然平滑，高频点密保留尖刺
    //   raw   ：同样逐 bin，但用折线直接连各 bin 中点，尖刺角度最锐利
    //   逐 bin 取数是为了消除"多列映射到同一个 bin"造成的低频水平台阶。
    enum class CurveMode { smooth = 0, semi = 1, raw = 2 };
    void setCurveMode (CurveMode m);
    CurveMode getCurveMode() const noexcept { return curveMode; }
    bool isSmoothEnabled() const noexcept { return curveMode != CurveMode::raw; }

    // ---- 通道模式（v3.1）：MID（现状）/ MID+SIDE / LEFT+RIGHT ----
    //   MID       ：单条 mid 曲线（(L+R)/2）
    //   MID+SIDE  ：主曲线 mid + 次曲线 side（(L-R)/2，弱色细线）
    //   LEFT+RIGHT：主曲线 L + 次曲线 R（弱色细线），用于看左右差异
    enum class ChannelMode { mid = 0, midSide = 1, leftRight = 2 };
    void setChannelMode (ChannelMode m);
    ChannelMode getChannelMode() const noexcept { return channelMode; }

    // ---- Sharper 模式（重分配谱线）：乐音收束成竖线 ----
    void setSharpEnabled(bool b);
    bool isSharpEnabled() const noexcept { return sharpEnabled; }

    // ---- Slope（频谱倾斜补偿，dB/oct，基准 1 kHz）----
    //   旧版是标题栏的 SLOPE 开关按钮（已删）；v3.1 起以参数形式回到控制面板：
    //   0 = 关闭（显示原始电平），4.5 = 旧版"开"的效果（粉噪看起来接近平直）。
    void  setSlopeDbPerOct (float v);
    float getSlopeDbPerOct() const noexcept { return slopeDbPerOct; }

    // 持久化：peakHold / sharp / curve / channel / fftScale / slope
    //   "slope" 旧存档是布尔（true/false），读档时按 true→4.5、false→0 迁移。
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState(const juce::ValueTree& state) override;

    // ---- 控制面板（v3.1）：原顶部四个按钮 + FFT scale 都搬进参数面板 ----
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    // ---- 显示风格与频率视野（v3.5 / v3.6）----
    //   注意：fill=0, line=1, bars=2, glow=3 保持原枚举序数向后兼容，新增项依次追加。
    enum class FillStyle { fill = 0, line = 1, bars = 2, glow = 3, energy = 4, led = 5, mirror = 6 };
    enum class ColorMap  { theme = 0, miniMeter = 1, magma = 2, inferno = 3, plasma = 4, ice = 5, rainbow = 6 };
    enum class BarBands  { b31 = 0, b64 = 1, b96 = 2, b128 = 3 };
    enum class FreqRange { full = 0, music = 1, vocal = 2, bass = 3 };

    void      setFillStyle      (FillStyle s);
    FillStyle getFillStyle() const noexcept { return fillStyle; }
    void      setColorMap       (ColorMap m);
    ColorMap  getColorMap() const noexcept { return colorMap; }
    void      setEnergyContrast (float c);
    float     getEnergyContrast() const noexcept { return energyContrast; }
    void      setBarBands       (BarBands b);
    BarBands  getBarBands() const noexcept { return barBands; }
    int       getBarBandCount() const noexcept;
    void      setFreqRange      (FreqRange r);
    FreqRange getFreqRange() const noexcept { return freqRange; }
    void      setFloorDb        (float db);
    void      setCeilDb         (float db);
    void      setReleaseScale   (float mult);
    void      setPeakFallRate   (float dbPerSec);
    void      setShowGrid       (bool on);
    void      setShowLabels     (bool on);

    // ---- FFT scale（横坐标刻度）：Log（默认，旧外观）/ Mel / Linear ----
    void setFftScale (y2k::fscale::Scale s);
    y2k::fscale::Scale getFftScale() const noexcept { return fftScale; }

    // ---- Toybox（v3.0.0）：拨弦与音名（§8.3 / E.4.1）----
    juce::Rectangle<int> getToyCanvasBounds() const { return getCanvasBounds (getContentBounds()); }
    float toyXToFreq (float x) const { return xToFreq (x, getToyCanvasBounds()); }
    float toyReadDbAtX (float x) const;
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;

    // ---- GlDirectModuleBase 钩子（v2.9.4 迁移）----
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    // 绿幕：保留频谱画布（曲线 / 峰值 / 悬停读数），刻度列与外壳盖绿
    juce::Rectangle<int> getChromaKeyCanvas() override { return getCanvasBounds (getContentBounds()); }

private:

    // ---- 数据处理 ----
    // 把 Hub 返回的 N 列线性幅度 → N 列 dBFS（Slope 补偿 + 上升快/下降慢平滑
    //   + 可选 3 列模糊），结果写进 state，scratch 为列间模糊的临时缓冲。
    void processSeries (const juce::Array<float>& mags,
                        std::vector<float>& state,
                        std::vector<float>& scratch);
    // 逐 bin 版本（Semi / Raw）：freqs 输出每个点的 center frequency（Hz）。
    //   只保留 [20Hz, min(20k, sr/2)] 内的 bin；相邻 bin 是否合并**按实际像素
    //   间距动态决定**（间距 < 线宽 + 余量就并入同一组，组内取最大保峰）。
    //   因此模块拉得越宽 → 合并越少 → 细节越多；线宽恒定不随宽度变化。
    void processBinSeries (const juce::Array<float>& mags, double binHz,
                           std::vector<float>& state,
                           std::vector<float>& scratch,
                           std::vector<float>& freqs);
    // 主曲线 + 次曲线（side 或 R）一起刷新
    void rebuildDisplay();
    void ensureDisplayCache (int numPoints);

    // ---- 能量色谱与频段聚合辅助（GL / CPU 双路径共用）----
    void         ensureEnergyLut() const;
    juce::Colour sampleEnergyPalette   (float t) const noexcept;
    juce::Colour energyContourColour   (bool darkCanvas) const noexcept;
    juce::Colour energySecondaryColour (bool darkCanvas) const noexcept;
    void         buildSubdividedCurve  (const std::vector<float>& dbVals,
                                        const std::vector<float>& freqs,
                                        juce::Rectangle<int> canvas,
                                        juce::Rectangle<int> inner,
                                        bool smooth, int subdiv,
                                        std::vector<float>& outX,
                                        std::vector<float>& outY) const;
    void         computeBandLevels     (std::vector<float>& outMainDb,
                                        std::vector<float>* outPeakDb,
                                        std::vector<float>* outSecondaryDb) const;

    // ---- 绘制 ----
    void drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawGrid      (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawCurves    (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawAxisLabels(juce::Graphics& g, juce::Rectangle<int> canvas) const;

    // ---- 布局 ----
    juce::Rectangle<int> getCanvasBounds (juce::Rectangle<int> content) const;

    // 频率 → X 像素坐标（按 fftScale：Log / Mel / Linear）
    float freqToX(float freqHz, juce::Rectangle<int> canvas) const;
    // X 像素坐标 → 频率
    float xToFreq(float x, juce::Rectangle<int> canvas) const;
    float activeMinFreqHz() const noexcept;
    float activeMaxFreqHz() const noexcept;

    // 频率 → [0,1] 的横向位置（freqToX / 曲线定位共用）
    float unitOfHz(float freqHz) const noexcept
    {
        return (float) y2k::fscale::toUnit (fftScale, (double) freqHz,
                                            (double) activeMinFreqHz(), (double) activeMaxFreqHz());
    }
    // 当前刻度下的纵向网格频率（major 带实线/标签，minor 为淡线）
    void gridFreqs (std::vector<float>& major, std::vector<float>& minor) const;
    // dBFS → Y 像素坐标
    float dbToY(float db, juce::Rectangle<int> canvas) const;

    // ---- 成员 ----
    AnalyserHub& hub;

    // 原始幅度快照（N 列线性）：主曲线源
    juce::Array<float> rawMags;
    // 次曲线源：midSide 模式 = side（(L-R)/2）；leftRight 模式 = R；mid 模式不用
    juce::Array<float> rawMagsSide;

    // ------------------------------------------------------------------
    // Semi / Raw 模式：逐 bin 数据（每个 FFT bin 一个点，按 bin 中心频率摆放）
    //   rawBins / rawBinsSide 为 FFT 幅度原序列（索引 i ↔ 频率 i*binHz）；
    //   smoothedHz / secondaryHz 是对应点的中心频率，绘制时按对数轴定位。
    //   smooth 模式下这两个频率数组为空 → 沿用等距列布局。
    // ------------------------------------------------------------------
    juce::Array<float> rawBins;
    juce::Array<float> rawBinsSide;
    double             binHz = 0.0;
    std::vector<float> smoothedHz;
    std::vector<float> secondaryHz;
    // 每个 bin 的像素 x 缓存（每帧复用，避免逐帧堆分配）
    std::vector<float> binXScratch;

    // 平滑后的 dB 值（按显示列数）
    std::vector<float> smoothedDb;
    // 次曲线（side / R）平滑后的 dB 值，与 smoothedDb 同尺寸
    std::vector<float> secondaryDb;
    // 峰值（dB）—— 持续下落：信号超过就顶上去，否则以 peakFallRatePerSec 回落
    std::vector<float> peakDb;

    // 每帧复用的显示层缓冲，避免 rebuild/paint 中反复分配。
    std::vector<float> blurredDb;
    std::vector<float> secondaryBlurDb;
    std::vector<float> slopeOffsetDb;
    int    slopeCacheSize = 0;
    double slopeCacheSampleRate = 0.0;
    y2k::fscale::Scale slopeCacheScale = y2k::fscale::Scale::log;
    float  slopeCacheDbPerOct = -1.0f;   // 缓存也跟 slope 参数绑定

    // paintContent 是 const 路径，使用 mutable 缓存承接曲线点数组。
    mutable std::vector<juce::Point<float>> curvePts;
    mutable std::vector<juce::Point<float>> secondaryPts;   // 次曲线采样点
    mutable std::vector<juce::Point<float>> peakCurvePts;
    mutable juce::Path fillPath;
    mutable juce::Path curvePath;
    mutable juce::Path secondaryPath;
    mutable juce::Path peakPath;
    mutable juce::Path dashedPeakPath;

    // 是否需要绘制次曲线（midSide / leftRight 模式）
    bool hasSecondaryCurve() const noexcept
    {
        return channelMode != ChannelMode::mid && ! secondaryDb.empty();
    }

    // 显示参数（默认与旧外观逐像素一致）
    static constexpr float minFreqHz = 20.0f;
    static constexpr float maxFreqHz = 20000.0f;
    float floorDb          = -80.0f;
    float ceilDb           =   0.0f;
    float peakFallRate     =   6.0f;    // dB/s 持续下降速率
    float releaseScale     =   1.0f;    // 曲线回落速度倍率（0.2..3.0）
    FillStyle fillStyle    = FillStyle::energy;
    ColorMap  colorMap     = ColorMap::theme;
    float     energyContrast = 1.25f;   // 能量着色反差幂次（0.5..2.5）
    BarBands  barBands     = BarBands::b64;
    FreqRange freqRange    = FreqRange::full;
    bool  showGrid         = true;
    bool  showLabels       = true;

    // Bars / LED 专用分频段峰值保持（仅在 peakHoldEnabled 开启时追踪并绘制浮空峰帽 / 峰砖）
    std::vector<float> barPeakDb;
    mutable std::vector<float> bandLevelScratch_;
    mutable std::vector<float> bandPeakScratch_;
    mutable std::vector<float> bandSecScratch_;

    // 能量着色 256 阶预烘焙 LUT（按 colorMap + 主题键懒更新，GL / CPU 逐像素一致）
    mutable std::array<juce::Colour, 256> energyLut_ {};
    mutable ColorMap      energyLutMap_      = ColorMap::theme;
    mutable juce::uint32  energyLutThemeKey_ = 0;
    mutable bool          energyLutValid_    = false;

    // GL / CPU 共用的细分折线临时缓冲（每帧复用，避免堆分配）
    mutable std::vector<float> subX_, subY_, subX2_, subY2_, subPx_, subPy_;
    mutable std::vector<juce::Point<float>> polyPtsScratch_;
    mutable juce::Image energyCpuCache_; // 浮动态/软绘能量幕直写位图（SoftwareImageType，避免逐列 ColourGradient 卡顿）

    // 新模块默认：Peak 关（不画峰值虚线）+ Semi 曲线（细节与平滑折中）
    bool peakHoldEnabled = false;
    // Slope 补偿量（dB/oct，基准 1 kHz）：默认 4.5 = 旧版 SLOPE 开关"开"的效果；
    //   0 = 关闭（与旧版"关"等价）。
    float slopeDbPerOct = 4.5f;
    CurveMode   curveMode   = CurveMode::semi;    // 曲线模式（平滑 / 半平滑 / 折线）
    ChannelMode channelMode = ChannelMode::mid;   // 通道模式

    // 每个模式的实现开关（保持单一事实来源，绘制 / 数据处理共用）
    bool useSpline() const noexcept { return curveMode != CurveMode::raw; }   // 是否做样条细分
    bool useColumnBlur() const noexcept { return curveMode == CurveMode::smooth; }
    int  splineSubdiv() const noexcept { return curveMode == CurveMode::semi ? 2 : 4; }

    juce::Time lastTickTime;
    float      frameDtSec = 1.0f / 120.0f; // v2.9：本帧间隔（rebuildDisplay 平滑换算用）

    // ---- 子弹时间（Bullet Time）----
    //   按住频谱画布 → release 回落速度放慢 bulletTimeSlowdown 倍。
    //   实现为"时间膨胀"：所有回落路径的 dt 换算为 dt/倍率，上升沿不受影响。
    bool bulletTimeActive = false;
    static constexpr float bulletTimeSlowdown = 50.0f;

    // ---- 残影着色（Ghost Tint）----
    //   按住频谱画布 → ghostMix 置 1，曲线整体混向残影色（PinkXP::sel，与原
    //   SpectrumGhostActor 同色）；松手后按 ghostFadeSec 线性消散回原配色。
    //   ghostMix = 1 纯残影色 / 0 正常配色，两条绘制路径（GL + CPU 回退）共用。
    float ghostMix = 0.0f;
    static constexpr float ghostFadeSec = 0.6f;   // 松手后残影色消散时长（秒）
    // 基础色 → 残影色的交叉混合（ghostMix 为 0 时原样返回）
    juce::Colour ghostTint (juce::Colour base, juce::Colour ghost) const;



    // v2.9：静态层缓存（底衬 + 凹陷画布 + 网格 + 坐标轴文字），仅在尺寸 / DPI /
    //   主题配色变化时重建；GL 渲染器对未修改的 Image 复用纹理，不再每帧上传。
    //   曲线（填充 + 双层描边 + 峰值虚线）每帧直接画到 GL 上下文。
    //   旧实现每帧"新建整张 ARGB Image → 软件光栅化全部内容 → 整图上传纹理"，
    //   在 120Hz 下是本模块最大的开销。
    mutable juce::Image  staticLayer_;
    mutable juce::Rectangle<int> staticLayerBounds_;
    mutable float        staticLayerScale_         = 0.0f;
    mutable juce::uint32 staticLayerColourKey_     = 0;
    mutable juce::uint32 staticLayerAppearanceGen_ = 0;
    mutable juce::uint32 staticLayerFontGen_       = 0;
    mutable bool         staticLayerChromaHidden_  = false;
    mutable bool         curve_cache_dirty_        = true; // 兼容保留：onFrame 标脏（不再触发重建）
    void renderStaticLayer (juce::Rectangle<int> content, float physicalScale) const;
    static juce::uint32 computeThemeColourKey() noexcept;
    int themeSubToken = -1;

    // ---- 控制面板参数 ----
    y2k::fscale::Scale fftScale = y2k::fscale::Scale::log;   // 构造默认 = 旧外观

    // ---- 鼠标悬停标尺 ----
    void drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas);
    juce::Point<int> hoverPos;
    bool             hoverActive = false;

    // ---- Sharper 模式（重分配谱线，反馈：参考 Spectrogram Sharp）----
    //   谱线追踪：Hub 每帧给重分配谱峰（瞬时频率 + 凝聚能量），
    //   这里做帧间匹配 + 位置/幅度 EMA 平滑 + 淡入淡出，
    //   避免谱线在帧间跳闪 —— 与专业频谱仪的重分配线一致。
    bool sharpEnabled = false;

    struct SharpLine
    {
        double freqHz   = 0.0;      // 平滑后的瞬时频率
        float  magDb    = -144.0f;  // 平滑后的幅度（dB，含 slope 补偿）
        float  alpha    = 0.0f;     // 淡入淡出 [0,1]
    };
    std::vector<SharpLine> sharpLines;
    std::vector<AnalyserHub::ReassignedPeak> sharpPeaksScratch;   // 每帧复用，避免分配

    void updateSharpLines (const std::vector<AnalyserHub::ReassignedPeak>& peaks,
                           float frameDtSec,
                           double sampleRate);
    // 谱线的 4.5dB/oct 斜率补偿（与曲线 slope 一致，按精确频率计算）
    float sharpSlopeDb (float freqHz) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumModule)
};
