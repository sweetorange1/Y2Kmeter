#include "source/ui/modules/SpectrumModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace
{
    // ---- 坐标轴标签留白（画布边距与标签绘制共用的单一事实来源）----
    //   底部 Hz 行与左侧 dB 列用同一口径：文字厚度 + 与画布边缘的间隙。
    //     · 底部：轴字体行高 12 + 间隙 2 + 余量 2 = 16
    //     · 左侧：最宽刻度 "-80" 在 9px 轴字体下实测约 15px（Silkscreen 14.9 /
    //       ChillBitmap 14.6）+ 间隙 2 + 余量 3 = 20
    //   旧值 32（28 文字盒 + 4 间隙）比文字实际所需宽了近一倍，白白吃掉画布宽度。
    constexpr int axisLabelGap   = 2;   // 文字与画布边缘的间隙
    constexpr int dbLabelColumnW = 20;  // 左侧 dB 刻度列宽
    constexpr int hzLabelRowH    = 16;  // 底部 Hz 刻度行高

    struct WeightedColourStop
    {
        float        pos;
        juce::Colour colour;
    };

    inline juce::Colour sampleWeightedStops (const WeightedColourStop* stops, int count, float t) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        if (count <= 0) return juce::Colours::black;
        if (count == 1 || t <= stops[0].pos) return stops[0].colour;
        for (int i = 0; i + 1 < count; ++i)
        {
            if (t <= stops[i + 1].pos)
            {
                const float span = juce::jmax (1.0e-5f, stops[i + 1].pos - stops[i].pos);
                const float frac = juce::jlimit (0.0f, 1.0f, (t - stops[i].pos) / span);
                return PinkXP::interpolateOkLab (stops[i].colour, stops[i + 1].colour, frac);
            }
        }
        return stops[count - 1].colour;
    }

    // 能量柱底部深度衰减色（~0.78-0.84x，保持强峰垂直光幕通透发光的同时带出纵深层次）
    inline juce::Colour makeEnergyDepthColour (juce::Colour topCol, float energy, float alpha) noexcept
    {
        const float taper = 0.78f + 0.06f * juce::jlimit (0.0f, 1.0f, energy);
        return juce::Colour::fromFloatRGBA (topCol.getFloatRed()   * taper,
                                            topCol.getFloatGreen() * taper,
                                            topCol.getFloatBlue()  * taper,
                                            alpha);
    }

    // 4 角独立 RGBA 顶点色四边形（2 三角形 = 36 floats）
    //   顶点序：(x0,y0)=TL, (x1,y1)=TR, (x2,y2)=BL, (x3,y3)=BR
    inline void appendGradientQuad (std::vector<float>& v,
                                    float x0, float y0, juce::Colour c0,
                                    float x1, float y1, juce::Colour c1,
                                    float x2, float y2, juce::Colour c2,
                                    float x3, float y3, juce::Colour c3)
    {
        const float q[36] = {
            x0, y0, c0.getFloatRed(), c0.getFloatGreen(), c0.getFloatBlue(), c0.getFloatAlpha(),
            x1, y1, c1.getFloatRed(), c1.getFloatGreen(), c1.getFloatBlue(), c1.getFloatAlpha(),
            x2, y2, c2.getFloatRed(), c2.getFloatGreen(), c2.getFloatBlue(), c2.getFloatAlpha(),
            x1, y1, c1.getFloatRed(), c1.getFloatGreen(), c1.getFloatBlue(), c1.getFloatAlpha(),
            x3, y3, c3.getFloatRed(), c3.getFloatGreen(), c3.getFloatBlue(), c3.getFloatAlpha(),
            x2, y2, c2.getFloatRed(), c2.getFloatGreen(), c2.getFloatBlue(), c2.getFloatAlpha(),
        };
        v.insert (v.end(), q, q + 36);
    }

    // CPU 逐列能量着色采样：推进有序光标 cur，返回覆盖 [pxLeft, pxRight] 像素列的最高峰 Y（最小 y）
    //   高频多点挤在同一像素列内时保峰取最高点，低频稀疏段线性插值，确保细尖峰不丢光柱。
    inline float sampleColumnPeakY (const std::vector<float>& xs,
                                    const std::vector<float>& ys,
                                    int& cur,
                                    float pxLeft,
                                    float pxRight,
                                    float yBot) noexcept
    {
        const int M = (int) xs.size();
        if (M <= 0) return yBot;
        if (M == 1) return ys[0];

        const float cx = 0.5f * (pxLeft + pxRight);
        while (cur + 1 < M && xs[(size_t) (cur + 1)] < pxLeft)
            ++cur;

        // 先求像素中心 cx 处的线性插值 Y
        int interpIdx = cur;
        while (interpIdx + 1 < M && xs[(size_t) (interpIdx + 1)] < cx)
            ++interpIdx;

        float yInterp = ys[(size_t) interpIdx];
        if (cx <= xs[0])
        {
            yInterp = ys[0];
        }
        else if (cx >= xs[(size_t) (M - 1)])
        {
            yInterp = ys[(size_t) (M - 1)];
        }
        else if (interpIdx + 1 < M)
        {
            const float dx = xs[(size_t) (interpIdx + 1)] - xs[(size_t) interpIdx];
            if (dx > 1.0e-5f)
            {
                const float frac = juce::jlimit (0.0f, 1.0f, (cx - xs[(size_t) interpIdx]) / dx);
                yInterp = ys[(size_t) interpIdx] + frac * (ys[(size_t) (interpIdx + 1)] - ys[(size_t) interpIdx]);
            }
        }

        // 若像素列 [pxLeft, pxRight] 内有多个窄峰顶点，取最高峰（最小 Y）以保留垂直能量丝线
        float bestY = yInterp;
        for (int k = cur; k < M && xs[(size_t) k] <= pxRight; ++k)
        {
            if (xs[(size_t) k] >= pxLeft)
                bestY = juce::jmin (bestY, ys[(size_t) k]);
        }
        return bestY;
    }
}

// ==========================================================
// SpectrumModule
// ==========================================================

SpectrumModule::SpectrumModule(AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::spectrum),
      hub(h)
{
    // Phase F：本模块只需要 Spectrum 一路 + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Spectrum);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // spectrum 6×4 大格
    setTitleText("Spectrum");
    setControlPanelEnabled (true);   // v3.1：Peak / Sharp / 曲线 / 通道 / FFT scale 都在参数面板里

    rawMags.ensureStorageAllocated (256);
    rawMagsSide.ensureStorageAllocated (256);
    smoothedDb .reserve (256);
    secondaryDb.reserve (256);
    peakDb     .reserve (256);
    blurredDb  .reserve (256);
    secondaryBlurDb.reserve (256);
    slopeOffsetDb.reserve (256);
    curvePts     .reserve (256);
    secondaryPts .reserve (256);
    peakCurvePts .reserve (256);

    lastTickTime = juce::Time::getCurrentTime();

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        staticLayerBounds_ = {};
        energyLutValid_    = false;
        buildGlSnapshot();
        repaint();
    });
}

SpectrumModule::~SpectrumModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Spectrum);
}

juce::ValueTree SpectrumModule::saveModuleSpecificState() const
{
    juce::ValueTree s("state");
    s.setProperty("peakHold", peakHoldEnabled, nullptr);
    s.setProperty("sharp", sharpEnabled, nullptr);
    s.setProperty("curve", juce::String (curveMode == CurveMode::smooth ? "smooth"
                                        : curveMode == CurveMode::semi   ? "semi"
                                                                         : "raw"), nullptr);
    s.setProperty("channel", juce::String (channelMode == ChannelMode::mid       ? "mid"
                                         : channelMode == ChannelMode::midSide   ? "midside"
                                                                                 : "lr"), nullptr);
    // slope：数值（dB/oct）。旧版是布尔开关，读档时迁移（见 restore）
    s.setProperty("slope", (double) slopeDbPerOct, nullptr);
    s.setProperty("fftScale", juce::String (fftScale == y2k::fscale::Scale::mel    ? "mel"
                                          : fftScale == y2k::fscale::Scale::linear ? "linear"
                                                                                   : "log"), nullptr);
    s.setProperty("fillStyle",      (int) fillStyle, nullptr);
    s.setProperty("colorMap",       (int) colorMap, nullptr);
    s.setProperty("energyContrast", (double) energyContrast, nullptr);
    s.setProperty("barBands",       (int) barBands, nullptr);
    s.setProperty("release",        (double) releaseScale, nullptr);
    s.setProperty("peakFall",       (double) peakFallRate, nullptr);
    s.setProperty("floorDb",        (double) floorDb, nullptr);
    s.setProperty("ceilDb",         (double) ceilDb, nullptr);
    s.setProperty("freqRange",      (int) freqRange, nullptr);
    s.setProperty("showGrid",       showGrid, nullptr);
    s.setProperty("showLabels",     showLabels, nullptr);
    return s;
}

void SpectrumModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
    if (state.hasProperty("peakHold"))
        setPeakHoldEnabled((bool) state.getProperty("peakHold"));
    if (state.hasProperty("sharp"))
        setSharpEnabled((bool) state.getProperty("sharp"));

    // 曲线模式：优先读新的 "curve"；旧存档里的布尔 "smooth" 做一次性迁移
    const juce::String cm = state.getProperty("curve").toString();
    if (cm == "smooth" || cm == "semi" || cm == "raw")
        setCurveMode (cm == "semi" ? CurveMode::semi
                    : cm == "raw"  ? CurveMode::raw
                                   : CurveMode::smooth);
    else if (state.hasProperty("smooth"))
        setCurveMode ((bool) state.getProperty("smooth") ? CurveMode::smooth : CurveMode::raw);

    const juce::String ch = state.getProperty("channel", "mid").toString();
    setChannelMode (ch == "midside" ? ChannelMode::midSide
                  : ch == "lr"      ? ChannelMode::leftRight
                                    : ChannelMode::mid);

    // FFT scale：缺键 = 保持当前（旧存档没有这个键 → 构造默认的 Log = 旧外观）
    if (state.hasProperty("fftScale"))
    {
        const juce::String fs = state.getProperty("fftScale").toString();
        setFftScale (fs == "mel" ? y2k::fscale::Scale::mel
                   : fs == "linear" ? y2k::fscale::Scale::linear
                                    : y2k::fscale::Scale::log);
    }

    // slope：新存档是数值；旧存档是布尔开关（true = 当年的 4.5 dB/oct，false = 关）
    if (state.hasProperty("slope"))
    {
        const auto v = state.getProperty("slope");
        setSlopeDbPerOct (v.isBool() ? ((bool) v ? 4.5f : 0.0f)
                                     : (float) (double) v);
    }

    // 旧存档兼容：若存档缺少 fillStyle / colorMap，升级首次打开默认启用能量幕 + 主题色谱
    if (state.hasProperty("fillStyle"))
        setFillStyle ((FillStyle) juce::jlimit (0, 6, (int) state.getProperty("fillStyle")));
    else
        setFillStyle (FillStyle::energy);

    if (state.hasProperty("colorMap"))
        setColorMap ((ColorMap) juce::jlimit (0, 6, (int) state.getProperty("colorMap")));
    else
        setColorMap (ColorMap::theme);
    if (state.hasProperty("energyContrast"))
        setEnergyContrast ((float) (double) state.getProperty("energyContrast"));
    if (state.hasProperty("barBands"))
        setBarBands ((BarBands) juce::jlimit (0, 3, (int) state.getProperty("barBands")));
    if (state.hasProperty("release"))
        setReleaseScale ((float) (double) state.getProperty("release"));
    if (state.hasProperty("peakFall"))
        setPeakFallRate ((float) (double) state.getProperty("peakFall"));
    if (state.hasProperty("floorDb"))
        setFloorDb ((float) (double) state.getProperty("floorDb"));
    if (state.hasProperty("ceilDb"))
        setCeilDb ((float) (double) state.getProperty("ceilDb"));
    if (state.hasProperty("freqRange"))
        setFreqRange ((FreqRange) juce::jlimit (0, 3, (int) state.getProperty("freqRange")));
    if (state.hasProperty("showGrid"))
        setShowGrid ((bool) state.getProperty("showGrid"));
    if (state.hasProperty("showLabels"))
        setShowLabels ((bool) state.getProperty("showLabels"));

    refreshControlPanel();
}

void SpectrumModule::setCurveMode (CurveMode m)
{
    if (curveMode == m) return;
    curveMode = m;

    // 切换列数会重置平滑/峰值状态（尺寸变化），立刻重建一次快照
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setChannelMode (ChannelMode m)
{
    if (channelMode == m) return;
    channelMode = m;

    // 切换模式时清空次曲线状态，避免上一模式（如 side）的旧值淡入新曲线
    secondaryDb.clear();
    secondaryBlurDb.clear();
    barPeakDb.clear();

    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setFftScale (y2k::fscale::Scale s)
{
    if (fftScale == s) return;
    fftScale = s;

    // 横轴映射变了：列频率不同 → 平滑状态、斜率补偿缓存、静态层（网格 + 轴标签）全部作废
    smoothedDb.clear();
    secondaryDb.clear();
    peakDb.clear();
    barPeakDb.clear();
    slopeCacheSize = 0;
    staticLayerBounds_ = {};
    curve_cache_dirty_ = true;
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// 控制面板参数（4 组：Style / Signal / Range / Axis）
// ----------------------------------------------------------
void SpectrumModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    // ---- Group 1: Style（视觉呈现）----
    out.push_back (ParamDef::choice ("fillStyle", "Display mode",
        { "Fill", "Outline", "Bar chart", "Glow", "Energy", "LED", "Mirrored" },
        "Fill: translucent curve fill. Outline: crisp trace with no fill. "
        "Bars: studio band bar chart with peak caps. Glow: neon double-halo curve. "
        "Energy: MiniMeter-style vertical frequency-energy pillars. "
        "LED: segmented retro LED brick stacks. Mirror: symmetrical vertical energy mirror.")
        .in ("Style"));
    out.push_back (ParamDef::choice ("colorMap", "Color map",
        { "Theme", "MiniMeter", "Magma", "Inferno", "Plasma", "Ice", "Rainbow" },
        "Color palette used for Energy, Bars, LED and Mirror modes. "
        "MiniMeter matches the deep-indigo to blazing neon-magenta energy curtain.")
        .in ("Style").when ([this]
        {
            return fillStyle == FillStyle::energy
                || fillStyle == FillStyle::bars
                || fillStyle == FillStyle::led
                || fillStyle == FillStyle::mirror;
        }));
    out.push_back (ParamDef::slider ("energyContrast", "Energy contrast", 0.5, 2.5, 0.1, {},
        "Shapes how sharply high-energy peaks stand out from quiet valleys in Energy and Mirror modes.")
        .in ("Style").when ([this]
        {
            return fillStyle == FillStyle::energy
                || fillStyle == FillStyle::mirror;
        }));
    out.push_back (ParamDef::choice ("barBands", "Bar bands",
        { "31", "64", "96", "128" },
        "Number of discrete frequency bands for Bars and LED modes "
        "(31 is classic 1/3-octave; 128 gives fine studio resolution).")
        .in ("Style").when ([this]
        {
            return fillStyle == FillStyle::bars
                || fillStyle == FillStyle::led;
        }));
    out.push_back (ParamDef::choice ("curve", "Curve",
        { "Smooth", "Semi", "Raw" },
        "Smooth: 256 columns with blur - calm. Semi: one point per FFT bin with a spline. "
        "Raw: one point per bin joined by straight lines - maximum spike detail.")
        .in ("Style").when ([this]
        {
            return fillStyle != FillStyle::bars
                && fillStyle != FillStyle::led;
        }));

    // ---- Group 2: Signal（分析与动态）----
    out.push_back (ParamDef::choice ("channel", "Channels",
        { "Mid", "Mid + Side", "Left + Right" },
        "Mid: (L+R)/2, what a mono speaker hears. Mid + Side: solid mid, faint side (L-R)/2. "
        "Left + Right: solid left, faint right.")
        .in ("Signal"));
    out.push_back (ParamDef::slider ("release", "Release speed", 0.2, 3.0, 0.1, " x",
        "How fast the spectrum falls back after a peak. Lower gives a calm averaged curve; higher tracks rapid transients.")
        .in ("Signal"));
    out.push_back (ParamDef::toggle ("sharp", "Sharpening",
        "Reassign spectral energy to its true frequency so notes and harmonics collapse "
        "into crisp lines instead of wide humps.")
        .in ("Signal").when ([this]
        {
            return fillStyle != FillStyle::bars
                && fillStyle != FillStyle::led;
        }));
    out.push_back (ParamDef::toggle ("peakHold", "Peak hold",
        "Keep a slowly falling line at the highest level each frequency has reached.")
        .in ("Signal"));
    out.push_back (ParamDef::slider ("peakFall", "Peak fall rate", 2.0, 24.0, 1.0, " dB/s",
        "How quickly the peak-hold line drops back down.")
        .in ("Signal").when ([this] { return peakHoldEnabled; }));

    // ---- Group 3: Range（动态量程与倾斜）----
    out.push_back (ParamDef::slider ("floorDb", "Floor", -100.0, -40.0, 5.0, " dB",
        "Bottom of the vertical dB scale. Raise it to zoom into loud program material; lower it to inspect quiet tails.")
        .in ("Range"));
    out.push_back (ParamDef::slider ("ceilDb", "Ceiling", -24.0, 6.0, 2.0, " dB",
        "Top of the vertical dB scale. Lowering Ceiling zooms in vertically on quieter signals.")
        .in ("Range"));
    out.push_back (ParamDef::slider ("slope", "Slope", 0.0, 12.0, 0.5, " dB/oct",
        "Tilts the curve up towards the highs, 1 kHz is the pivot. 4.5 dB/oct (default) makes "
        "pink noise look flat; 0 turns the tilt off and shows the raw level.")
        .in ("Range"));

    // ---- Group 4: Axis（频率轴与标尺）----
    out.push_back (ParamDef::choice ("fftScale", "FFT scale",
        { "Log", "Mel", "Linear" },
        "How frequency is laid out left to right. Log: every octave is equally wide. "
        "Mel: follows pitch perception. Linear: equal Hz per pixel, so highs get most of the room.")
        .in ("Axis"));
    out.push_back (ParamDef::choice ("freqRange", "Freq range",
        { "Full", "Music", "Vocal", "Bass" },
        "Zooms the frequency view: Full (20-20k Hz), Music (30-12k Hz), Vocal (80-8k Hz), or Bass (20-1k Hz).")
        .in ("Axis"));
    out.push_back (ParamDef::toggle ("showGrid", "Grid",
        "Show horizontal dB and vertical frequency reference lines.")
        .in ("Axis"));
    out.push_back (ParamDef::toggle ("showLabels", "Labels",
        "Show dB and Hz axis labels. Turn OFF for a larger borderless spectrum.")
        .in ("Axis"));
}

double SpectrumModule::getParamValue (const juce::String& id) const
{
    if (id == "fillStyle")      return (double) (int) fillStyle;
    if (id == "colorMap")       return (double) (int) colorMap;
    if (id == "energyContrast") return (double) energyContrast;
    if (id == "barBands")       return (double) (int) barBands;
    if (id == "peakHold")       return peakHoldEnabled ? 1.0 : 0.0;
    if (id == "peakFall")       return (double) peakFallRate;
    if (id == "release")        return (double) releaseScale;
    if (id == "sharp")          return sharpEnabled ? 1.0 : 0.0;
    if (id == "slope")          return (double) slopeDbPerOct;
    if (id == "floorDb")        return (double) floorDb;
    if (id == "ceilDb")         return (double) ceilDb;
    if (id == "curve")          return (double) (int) curveMode;
    if (id == "channel")        return (double) (int) channelMode;
    if (id == "fftScale")       return (double) (int) fftScale;
    if (id == "freqRange")      return (double) (int) freqRange;
    if (id == "showGrid")       return showGrid ? 1.0 : 0.0;
    if (id == "showLabels")     return showLabels ? 1.0 : 0.0;
    return 0.0;
}

void SpectrumModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "fillStyle")      setFillStyle ((FillStyle) juce::jlimit (0, 6, juce::roundToInt (v)));
    else if (id == "colorMap")       setColorMap ((ColorMap) juce::jlimit (0, 6, juce::roundToInt (v)));
    else if (id == "energyContrast") setEnergyContrast ((float) v);
    else if (id == "barBands")       setBarBands ((BarBands) juce::jlimit (0, 3, juce::roundToInt (v)));
    else if (id == "peakHold")       setPeakHoldEnabled (v >= 0.5);
    else if (id == "peakFall")       setPeakFallRate ((float) v);
    else if (id == "release")        setReleaseScale ((float) v);
    else if (id == "sharp")          setSharpEnabled (v >= 0.5);
    else if (id == "slope")          setSlopeDbPerOct ((float) v);
    else if (id == "floorDb")        setFloorDb ((float) v);
    else if (id == "ceilDb")         setCeilDb ((float) v);
    else if (id == "curve")          setCurveMode ((CurveMode) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "channel")        setChannelMode ((ChannelMode) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "fftScale")       setFftScale (y2k::fscale::fromIndex (juce::roundToInt (v)));
    else if (id == "freqRange")      setFreqRange ((FreqRange) juce::jlimit (0, 3, juce::roundToInt (v)));
    else if (id == "showGrid")       setShowGrid (v >= 0.5);
    else if (id == "showLabels")     setShowLabels (v >= 0.5);
}

float SpectrumModule::activeMinFreqHz() const noexcept
{
    switch (freqRange)
    {
        case FreqRange::music: return 30.0f;
        case FreqRange::vocal: return 80.0f;
        case FreqRange::bass:  return 20.0f;
        case FreqRange::full:
        default:               return minFreqHz;
    }
}

float SpectrumModule::activeMaxFreqHz() const noexcept
{
    switch (freqRange)
    {
        case FreqRange::music: return 12000.0f;
        case FreqRange::vocal: return 8000.0f;
        case FreqRange::bass:  return 1000.0f;
        case FreqRange::full:
        default:               return maxFreqHz;
    }
}

int SpectrumModule::getBarBandCount() const noexcept
{
    switch (barBands)
    {
        case BarBands::b31:  return 31;
        case BarBands::b96:  return 96;
        case BarBands::b128: return 128;
        case BarBands::b64:
        default:             return 64;
    }
}

void SpectrumModule::setFillStyle (FillStyle s)
{
    if (fillStyle == s) return;
    fillStyle = s;
    if (fillStyle == FillStyle::bars || fillStyle == FillStyle::led)
    {
        barPeakDb.clear();
        sharpLines.clear();
    }
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setColorMap (ColorMap m)
{
    if (colorMap == m) return;
    colorMap = m;
    energyLutValid_ = false;
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setEnergyContrast (float c)
{
    c = juce::jlimit (0.5f, 2.5f, c);
    if (std::abs (c - energyContrast) < 0.005f) return;
    energyContrast = c;
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setBarBands (BarBands b)
{
    if (barBands == b) return;
    barBands = b;
    barPeakDb.clear();
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setFreqRange (FreqRange r)
{
    if (freqRange == r) return;
    freqRange = r;
    smoothedDb.clear();
    secondaryDb.clear();
    peakDb.clear();
    barPeakDb.clear();
    slopeCacheSize = 0;
    staticLayerBounds_ = {};
    curve_cache_dirty_ = true;
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setFloorDb (float db)
{
    db = juce::jlimit (-100.0f, ceilDb - 12.0f, db);
    if (std::abs (db - floorDb) < 0.05f) return;
    floorDb = db;
    staticLayerBounds_ = {};
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setCeilDb (float db)
{
    db = juce::jlimit (floorDb + 12.0f, 6.0f, db);
    if (std::abs (db - ceilDb) < 0.05f) return;
    ceilDb = db;
    staticLayerBounds_ = {};
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setReleaseScale (float mult)
{
    releaseScale = juce::jlimit (0.2f, 3.0f, mult);
}

void SpectrumModule::setPeakFallRate (float dbPerSec)
{
    peakFallRate = juce::jlimit (2.0f, 24.0f, dbPerSec);
}

void SpectrumModule::setShowGrid (bool on)
{
    if (showGrid == on) return;
    showGrid = on;
    staticLayerBounds_ = {};
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setShowLabels (bool on)
{
    if (showLabels == on) return;
    showLabels = on;
    staticLayerBounds_ = {};
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setSharpEnabled(bool b)
{
    if (sharpEnabled == b) return;
    sharpEnabled = b;

    // 关闭时清空谱线；开启时从下一帧开始重新追踪（避免陈旧线残留）
    if (! sharpEnabled)
        sharpLines.clear();

    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setSlopeDbPerOct (float v)
{
    v = juce::jlimit (0.0f, 12.0f, v);
    if (std::abs (v - slopeDbPerOct) < 0.01f)
        return;

    slopeDbPerOct = v;

    // 补偿量变了：斜率缓存、平滑状态、峰值、谱线全部作废，从下一帧按新补偿重算
    //   （不清的话旧值是按旧斜率算出来的，会慢慢"漂"到新位置）
    slopeCacheSize = 0;
    smoothedDb.clear();
    secondaryDb.clear();
    peakDb.clear();
    barPeakDb.clear();
    sharpLines.clear();
    curve_cache_dirty_ = true;

    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// Sharper 模式（重分配谱线）
// ----------------------------------------------------------
float SpectrumModule::sharpSlopeDb (float freqHz) const noexcept
{
    // 与曲线 Slope 补偿同款（基准 1 kHz，量值来自参数），按精确频率计算
    return slopeDbPerOct * std::log2 (juce::jmax (20.0f, freqHz) / 1000.0f);
}

void SpectrumModule::updateSharpLines (const std::vector<AnalyserHub::ReassignedPeak>& peaks,
                                       float frameDtSec,
                                       double sampleRate)
{
    juce::ignoreUnused (sampleRate);
    using namespace y2k::pacing;

    // 平滑系数（按真实 dt 换算，帧率无关；120Hz 标定）
    const float aPos   = alphaForDt (0.45f, frameDtSec);   // 位置 EMA（线频率本身极稳，跟踪要跟得上换音）
    const float aMagUp = alphaForDt (0.60f, frameDtSec);   // 幅度上升快（攻击）
    // 子弹时间：幅度回落（release）做 1/50 时间膨胀，与主曲线行为一致
    const float aMagDn = alphaForDt (0.14f, (bulletTimeActive
                                            ? frameDtSec / bulletTimeSlowdown
                                            : frameDtSec) * releaseScale);   // 幅度回落慢（避免闪烁）
    const float aAlpha = alphaForDt (0.40f, frameDtSec);   // 淡入

    // 帧间匹配阈值：1/8 八度内视为同一根谱线（> 半音间距 0.083 八度，能区分相邻半音）
    constexpr float kMatchOctaves = 0.125f;
    // 反馈修订：新线创建门槛拉高 —— 弱峰（<-64dB）不进入显示，只参与已有线匹配
    constexpr float kNewLineMinDb = -64.0f;

    std::vector<bool> matched (sharpLines.size(), false);

    for (const auto& pk : peaks)
    {
        // 重分配谱线与主曲线用同一套 Slope 补偿（量值取自参数）
        const float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, pk.mag))
                       + sharpSlopeDb ((float) pk.freqHz);

        // 在未匹配的旧线中找频率最近邻
        int best = -1;
        float bestDist = kMatchOctaves;
        for (size_t i = 0; i < sharpLines.size(); ++i)
        {
            if (matched[i]) continue;
            const float d = std::abs (std::log2 (
                (float) (pk.freqHz / juce::jmax (1.0, sharpLines[i].freqHz))));
            if (d < bestDist)
            {
                bestDist = d;
                best = (int) i;
            }
        }

        if (best >= 0)
        {
            SharpLine& l = sharpLines[(size_t) best];
            matched[(size_t) best] = true;
            l.freqHz += aPos * (pk.freqHz - l.freqHz);
            const float aMag = (db > l.magDb) ? aMagUp : aMagDn;
            l.magDb += aMag * (db - l.magDb);
            l.alpha += aAlpha * (1.0f - l.alpha);
        }
        else if (sharpLines.size() < 64 && db >= kNewLineMinDb)
        {
            // 新谱线：淡入（alpha 从 0 爬升，避免"啪"地出现）
            SharpLine l;
            l.freqHz = pk.freqHz;
            l.magDb  = db;
            l.alpha  = 0.0f;
            sharpLines.push_back (l);
            matched.push_back (true);
        }
    }

    // 未匹配的谱线快速渐隐（反馈修订：0.45s → 0.12s 时间常数，
    //   音停止后竖线约 0.5s 内消失，不再残留大片幽灵细线）。
    const float fade = std::exp (-juce::jmax (0.0f, frameDtSec) / 0.12f);
    for (size_t i = 0; i < sharpLines.size();)
    {
        if (matched[i])
        {
            ++i;
            continue;
        }

        SharpLine& l = sharpLines[i];
        l.alpha *= fade;
        if (l.alpha <= 0.015f)
            sharpLines.erase (sharpLines.begin() + (std::ptrdiff_t) i);
        else
            ++i;
    }
}

void SpectrumModule::setPeakHoldEnabled(bool b)
{
    if (peakHoldEnabled == b) return;
    peakHoldEnabled = b;
    // 关闭时清空曲线峰值与柱图/LED 频段峰值，再次开启从当前值重新开始
    if (! peakHoldEnabled)
    {
        std::fill(peakDb.begin(), peakDb.end(), floorDb);
        barPeakDb.clear();
    }
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// 频率/dB 坐标换算
// ----------------------------------------------------------
float SpectrumModule::freqToX(float freqHz, juce::Rectangle<int> canvas) const
{
    return (float) canvas.getX() + unitOfHz(freqHz) * (float) canvas.getWidth();
}

float SpectrumModule::xToFreq(float x, juce::Rectangle<int> canvas) const
{
    const float t = (x - (float) canvas.getX()) / juce::jmax(1.0f, (float) canvas.getWidth());
    return (float) y2k::fscale::fromUnit (fftScale, (double) juce::jlimit(0.0f, 1.0f, t),
                                          (double) activeMinFreqHz(), (double) activeMaxFreqHz());
}

// 纵向网格频率：按 fftScale 与 freqRange 自适应筛选可见主次线
void SpectrumModule::gridFreqs (std::vector<float>& major, std::vector<float>& minor) const
{
    const float fLo = activeMinFreqHz();
    const float fHi = activeMaxFreqHz();
    auto filterPush = [&] (std::vector<float>& dst, std::initializer_list<float> src)
    {
        dst.clear();
        for (float f : src)
            if (f > fLo * 1.02f && f < fHi * 0.98f)
                dst.push_back (f);
    };

    if (freqRange == FreqRange::bass)
    {
        filterPush (major, { 50.0f, 100.0f, 200.0f, 500.0f });
        filterPush (minor, { 30.0f, 40.0f, 60.0f, 80.0f, 150.0f, 300.0f, 400.0f, 700.0f, 800.0f });
        return;
    }

    switch (fftScale)
    {
        case y2k::fscale::Scale::mel:
            filterPush (major, { 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f });
            filterPush (minor, { 100.0f, 200.0f, 300.0f, 700.0f, 1500.0f, 3000.0f, 7000.0f, 15000.0f });
            break;
        case y2k::fscale::Scale::linear:
            filterPush (major, { 2000.0f, 5000.0f, 10000.0f, 15000.0f });
            filterPush (minor, { 1000.0f, 3000.0f, 4000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f,
                                 11000.0f, 12000.0f, 13000.0f, 14000.0f, 16000.0f, 17000.0f, 18000.0f, 19000.0f });
            break;
        case y2k::fscale::Scale::log:
        default:
            filterPush (major, { 100.0f, 1000.0f, 10000.0f });
            filterPush (minor, { 30.0f, 50.0f, 200.0f, 300.0f, 500.0f, 2000.0f, 3000.0f, 5000.0f, 15000.0f });
            break;
    }
}

float SpectrumModule::dbToY(float db, juce::Rectangle<int> canvas) const
{
    const float span = juce::jmax (1.0f, ceilDb - floorDb);
    const float t = (db - floorDb) / span;
    const float tc = juce::jlimit(0.0f, 1.0f, t);
    return (float) canvas.getBottom() - tc * (float) canvas.getHeight();
}

// ----------------------------------------------------------
// Toybox：拨弦与音名 + dB 读数（E.4.1）
// ----------------------------------------------------------
float SpectrumModule::toyReadDbAtX (float x) const
{
    const auto canvas = getToyCanvasBounds();
    const int N = (int) smoothedDb.size();
    if (N <= 0 || canvas.getWidth() <= 0)
        return -144.0f;

    // 逐 bin 模式（Semi/Raw）：按频率找最近的点；smooth 模式：等距列
    const bool freqAxis = ((int) smoothedHz.size() == N);
    int col = 0;
    float slopeDb = 0.0f;

    if (freqAxis)
    {
        const float freq = xToFreq (x, canvas);
        auto it = std::lower_bound (smoothedHz.begin(), smoothedHz.end(), freq);
        if (it == smoothedHz.begin())
            col = 0;
        else if (it == smoothedHz.end())
            col = N - 1;
        else
        {
            const int hi = (int) (it - smoothedHz.begin());
            const int lo = hi - 1;
            col = (std::abs (smoothedHz[(size_t) lo] - freq) <= std::abs (smoothedHz[(size_t) hi] - freq)) ? lo : hi;
        }
        slopeDb = slopeDbPerOct * std::log2 (juce::jmax (20.0f, smoothedHz[(size_t) col]) / 1000.0f);
    }
    else
    {
        col = juce::jlimit (0, N - 1,
            juce::roundToInt ((x - (float) canvas.getX())
                              / (float) canvas.getWidth() * (float) (N - 1)));
        if (col < (int) slopeOffsetDb.size())
            slopeDb = slopeOffsetDb[(size_t) col];
    }

    // 读数要扣掉 slope 补偿才是真实 dBFS
    return smoothedDb[(size_t) col] - slopeDb;
}

// ----------------------------------------------------------
// 残影着色：基础色 → 残影色的交叉混合（GL / CPU 两条路径共用）
// ----------------------------------------------------------
juce::Colour SpectrumModule::ghostTint (juce::Colour base, juce::Colour ghost) const
{
    if (ghostMix <= 0.0f)
        return base;
    return base.interpolatedWith (ghost, juce::jlimit (0.0f, 1.0f, ghostMix));
}

bool SpectrumModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void SpectrumModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const auto canvas = getToyCanvasBounds();
    const auto inner  = canvas.reduced (3);
    if (inner.getWidth() < 8 || inner.getHeight() < 8)
        return;

    // 按当前画布采集 96 点真实频谱轮廓 Y（smoothedDb 已含当前斜率补偿，与屏幕所见曲线一致）
    auto sampleDisplayY = [&](float x) -> float
    {
        const int N = (int) smoothedDb.size();
        if (N <= 0 || canvas.getWidth() <= 0)
            return (float) inner.getBottom();
        const bool freqAxis = ((int) smoothedHz.size() == N);
        int col = 0;
        if (freqAxis)
        {
            const float f = xToFreq (x, canvas);
            auto it = std::lower_bound (smoothedHz.begin(), smoothedHz.end(), f);
            if (it == smoothedHz.begin())
                col = 0;
            else if (it == smoothedHz.end())
                col = N - 1;
            else
            {
                const int hi = (int) (it - smoothedHz.begin());
                const int lo = hi - 1;
                col = (std::abs (smoothedHz[(size_t) lo] - f) <= std::abs (smoothedHz[(size_t) hi] - f)) ? lo : hi;
            }
        }
        else
        {
            col = juce::jlimit (0, N - 1,
                juce::roundToInt ((x - (float) canvas.getX())
                                  / (float) canvas.getWidth() * (float) (N - 1)));
        }
        return dbToY (smoothedDb[(size_t) col], canvas);
    };

    constexpr int kBins = 96;
    std::vector<float> curveY ((size_t) kBins);
    for (int i = 0; i < kBins; ++i)
    {
        const float x = (float) inner.getX() + (float) i / (float) (kBins - 1) * (float) inner.getWidth();
        curveY[(size_t) i] = sampleDisplayY (x);
    }

    // 计算真实 2f / 3f / 4f 泛音节点（严格尊重当前 fftScale 频率轴刻度）
    const float baseFreq = toyXToFreq (hit.pos.x);
    std::vector<juce::Point<float>> harmonics;
    harmonics.reserve (3);
    for (int m = 2; m <= 4; ++m)
    {
        const float hf = baseFreq * (float) m;
        if (hf > activeMaxFreqHz()) break;
        const float hx = freqToX (hf, canvas);
        if (hx > (float) inner.getX() + 2.0f && hx < (float) inner.getRight() - 2.0f)
            harmonics.push_back ({ hx, sampleDisplayY (hx) });
    }

    juce::Component::SafePointer<SpectrumModule> safe (this);
    auto isSlowMo = [safe]() -> bool
    {
        return safe != nullptr && safe->bulletTimeActive;
    };

    ctx.addModuleActorCapped (makeSpectrumPluckActor (hit.pos, canvas, curveY, harmonics, std::move (isSlowMo)), 6);
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> SpectrumModule::getCanvasBounds(juce::Rectangle<int> content) const
{
    if (! showLabels)
        return content.reduced (4);
    // 左侧留 dbLabelColumnW 给 dB 标签，底部留 hzLabelRowH 给 Hz 标签
    return content.withTrimmedTop(4)
                  .withTrimmedLeft(dbLabelColumnW)
                  .withTrimmedBottom(hzLabelRowH)
                  .withTrimmedRight(6);
}

void SpectrumModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    // v2.9.5：尺寸守卫（防 layoutContent 每帧触发导致的交互卡顿）
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
        buildGlSnapshot();
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调（从 Hub 拿已经 "双路合并" 的幅度）
// ----------------------------------------------------------
void SpectrumModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    // v2.9：取消 "每 2 tick 才 repaint" 的分频与 16ms×DPI 节流（两者叠加后
    //   本模块实际只有 ~30fps）。帧分发已与 vblank 同步并受 FPS 上限约束，
    //   每个分发帧都重绘才能跑满 120fps；曲线本身走离屏缓存，成本可控。

    // v2.9.6 单路 FFT：直接读主路（用户所选 size，75% overlap）的对数采样结果，
    // 分辨率与刷新率完全由 FFT size 档位决定。UI 侧零额外重采样，
    // 直接拿到"每列一个线性幅度"。
    //
    // 关于 frame：
    //   ·frame.spectrumMag 仍然保留（其他模块可能用），本模块不再使用；
    //   · blended 接口走 Hub 内部锁，成本极低（N 次查表）。
    const float fLo = activeMinFreqHz();
    const float fHi = juce::jmin (activeMaxFreqHz(), (float) (hub.getSampleRate() * 0.5));
    if (fHi <= fLo) return;

    // 列数与采样方式（三档）：
    //   smooth：256 列 + 频段保峰聚合（最平滑、成本最低）
    //   semi  ：512 列 + 频段保峰聚合（更多窄峰，仍走样条）
    //   semi / raw：逐 FFT bin 取数（bins 模式）
    //   低频列宽（~2.7%×f，20Hz 处 ≈0.54Hz）远小于 bin 宽（48k/2048 ≈23.4Hz），
    //   若仍按"列→bin"映射，几十列会重复同一个 bin 的值 → 正是那些水平小台阶。
    //   bins 模式下每个 bin 只出一个点（放在该 bin 的中心频率上）再连线，
    //   低频因此自然稀疏、接近 smoot 的观感；高频 bin 密集 → 针状纹理。
    const bool binMode = (curveMode != CurveMode::smooth);

    if (binMode)
    {
        // 通道模式取数（主/次两路同一帧抓取，轴与分辨率完全一致）：
        //   MID       —— 主 = mid；MID+SIDE —— 主 = mid，次 = side
        //   LEFT+RIGHT—— 主 = L，次 = R
        double hz = 0.0;
        switch (channelMode)
        {
            case ChannelMode::mid:
                hub.copySpectrumBins (rawBins, AnalyserHub::SpectrumSource::mid, hz);
                break;

            case ChannelMode::midSide:
                hub.copySpectrumBins (rawBins,     AnalyserHub::SpectrumSource::mid,  hz);
                hub.copySpectrumBins (rawBinsSide, AnalyserHub::SpectrumSource::side, hz);
                break;

            case ChannelMode::leftRight:
                hub.copySpectrumBins (rawBins,     AnalyserHub::SpectrumSource::left,  hz);
                hub.copySpectrumBins (rawBinsSide, AnalyserHub::SpectrumSource::right, hz);
                break;
        }
        binHz = hz;
    }
    else
    {
        // 平滑模式：256 列对数轴 + 频段保峰聚合（现状）
        constexpr int N = 256;
        switch (channelMode)
        {
            case ChannelMode::mid:
                hub.getSpectrumMagnitudesBlended (rawMags, N, fLo, fHi, false, fftScale);
                break;

            case ChannelMode::midSide:
                hub.getSpectrumMagnitudesBlended (rawMags, N, fLo, fHi, false, fftScale);
                hub.getSpectrumMagnitudesBlendedSide (rawMagsSide, N, fLo, fHi, false, fftScale);
                break;

            case ChannelMode::leftRight:
                hub.getSpectrumMagnitudesBlendedLR (rawMags, rawMagsSide, N, fLo, fHi, false, fftScale);
                break;
        }
        binHz = 0.0;
    }

    // Sharper：重分配谱峰 → 谱线追踪（柱形图 / LED 砖柱模式下不启用锐化）
    const bool allowSharp = sharpEnabled
                            && fillStyle != FillStyle::bars
                            && fillStyle != FillStyle::led;
    if (allowSharp)
    {
        sharpPeaksScratch.clear();
        hub.getSpectrumReassignedPeaks (sharpPeaksScratch,
                                        fLo,
                                        fHi,
                                        28);
        updateSharpLines (sharpPeaksScratch, frame.dtSec, hub.getSampleRate());
    }

    // v2.9：高精度帧间隔（旧实现 Time::getCurrentTime 整数毫秒，120Hz 下抖动）
    frameDtSec = frame.dtSec;
    const float deltaMs = frame.dtSec * 1000.0f;
    lastTickTime = juce::Time::getCurrentTime();

    rebuildDisplay();

    // 峰值下降更新（持续下落，无 hold / 无阈值）
    //   · 任一列信号超过当前 peak → 顶上去（刷新该列峰值）；
    //   · 否则该列以 peakFallRatePerSec 持续下降，直到 minDb。
    //   不再有"保持"阶段：无论信号强度如何，peak 线始终处于缓慢下落的
    //   动态——信号顶上来才抬升，否则一直回落，形成贴着信号、带下落惯性的虚线。
    if (peakHoldEnabled && ! peakDb.empty())
    {
        // 子弹时间：峰值下落速率同步放慢 50 倍（顶上去不受影响，只慢回落）
        const float fallRatePerSec = bulletTimeActive
                                       ? peakFallRate / bulletTimeSlowdown
                                       : peakFallRate;
        const float fallDelta = fallRatePerSec * (deltaMs / 1000.0f);

        for (size_t i = 0; i < peakDb.size(); ++i)
        {
            if (smoothedDb[i] > peakDb[i])
                peakDb[i] = smoothedDb[i];                                  // 信号超过峰值 → 顶上去
            else
                peakDb[i] = juce::jmax (floorDb, peakDb[i] - fallDelta);    // 否则持续下落
        }
    }

    // Bars / LED 模式的浮空峰帽 / 峰砖（仅在开启「峰值保持」时追踪与显示）
    if (peakHoldEnabled && (fillStyle == FillStyle::bars || fillStyle == FillStyle::led) && smoothedDb.size() > 1)
    {
        const int numBands = getBarBandCount();
        if ((int) barPeakDb.size() != numBands)
            barPeakDb.assign ((size_t) numBands, floorDb);

        computeBandLevels (bandLevelScratch_, nullptr, nullptr);

        const float fallRate = bulletTimeActive ? peakFallRate / bulletTimeSlowdown : peakFallRate;
        const float fallDelta = fallRate * (deltaMs / 1000.0f);

        for (int b = 0; b < numBands; ++b)
        {
            const float curDb = bandLevelScratch_[(size_t) b];
            if (curDb > barPeakDb[(size_t) b])
                barPeakDb[(size_t) b] = curDb;
            else
                barPeakDb[(size_t) b] = juce::jmax (floorDb, barPeakDb[(size_t) b] - fallDelta);
        }
    }

    // 残影色消散：按住时保持满值（ghostMix=1），松手后按 ghostFadeSec 线性降到 0，
    //   曲线随之从残影色渐变回正常配色。放在 buildGlSnapshot 之前，
    //   本帧快照即用新的混合比例。
    if (! bulletTimeActive && ghostMix > 0.0f)
        ghostMix = juce::jmax (0.0f, ghostMix - frame.dtSec / ghostFadeSec);

    curve_cache_dirty_ = true;
    // v2.9.4：同步生成 GL 直渲染快照
    buildGlSnapshot();
    if (hoverActive || ! requestGlFrameOnly())
        repaintFrameContent();
}

// ----------------------------------------------------------
// processSeries —— 一路 N 列线性幅度 → N 列 dBFS 显示值
//   1) 线性 → dB；
//   2) Slope 补偿（dB/oct 可调，默认 4.5，0 = 关闭）；
//   3) 上升快 / 下降慢 EMA（子弹时间只拖慢下降沿）；
//   4) 圆滑模式：末端轻量 3 列模糊磨圆拐点；
//      Semi / Raw 模式跳过这一步 —— 该卷积会把孤立尖峰
//      按 [1,2,1]/4 削掉一半以上，是"高频尖刺被磨没"的来源之一。
// ----------------------------------------------------------
void SpectrumModule::processSeries (const juce::Array<float>& mags,
                                    std::vector<float>& state,
                                    std::vector<float>& scratch)
{
    const int N = mags.size();
    if (N <= 1)
        return;

    if ((int) state.size() != N)
        state.assign (N, floorDb);
    if ((int) scratch.size() != N)
        scratch.assign (N, floorDb);

    const float releaseDtSec = (bulletTimeActive
                                    ? frameDtSec / bulletTimeSlowdown
                                    : frameDtSec) * releaseScale;
    const float alphaUp   = y2k::pacing::alphaForDt (0.55f, frameDtSec);
    const float alphaDown = y2k::pacing::alphaForDt (0.12f, releaseDtSec);

    for (int col = 0; col < N; ++col)
    {
        const float mag = std::abs (mags.getUnchecked (col));
        float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));

        // Slope 补偿：dB/oct 可调（默认 4.5，基准 1 kHz；0 = 关闭）
        if ((int) slopeOffsetDb.size() == N)
            db += slopeOffsetDb[(size_t) col];

        // 平滑：上升快，下降慢
        const float prev  = state[(size_t) col];
        const float alpha = (db > prev) ? alphaUp : alphaDown;
        float sm = prev + alpha * (db - prev);
        sm = juce::jlimit (floorDb, ceilDb + 12.0f, sm);
        state[(size_t) col] = sm;
    }

    // 列间低通：只在 smooth 模式做（semi / raw 跳过，保住窄峰高度）
    if (! useColumnBlur() || N < 3)
        return;

    scratch[0]     = state[0];
    scratch[N - 1] = state[(size_t) (N - 1)];
    for (int i = 1; i < N - 1; ++i)
    {
        scratch[(size_t) i] = 0.25f * state[(size_t)(i - 1)]
                            + 0.50f * state[(size_t) i]
                            + 0.25f * state[(size_t)(i + 1)];
    }
    state.swap (scratch);
}

// ----------------------------------------------------------
// processBinSeries —— 逐 bin 幅度 → 显示点（Semi / Raw）
//   · 只取 [20Hz, min(20k, sr/2)] 内的 bin；
//   · 合并规则按**实际像素间距**动态决定：从当前 bin 开始向后吃进邻居，
//     直到它的 x 距本组起点 ≥ minGapPx（= 线宽 + 一点余量）为止。
//     于是：
//       - 模块越宽 → 每 bin 占的像素越多 → 合并越少、细节越多；
//       - 高频 bin 天然密集 → 自动合并成 1 点/线宽，消除"点距 < 线宽"
//         造成的噪点/锯齿；
//       - 低频 bin 天然稀疏 → 基本不合并，每个 bin 独立成点。
//     组内取**最大**幅度（保峰），频率取组中心，尖峰不会被平均掉。
//   · 每个点带自己的中心频率（freqs），绘制时按对数轴定位 —— 不再出现
//     "多列共用同一个 bin 值"的水平台阶。
// ----------------------------------------------------------
void SpectrumModule::processBinSeries (const juce::Array<float>& mags, double hzPerBin,
                                       std::vector<float>& state,
                                       std::vector<float>& scratch,
                                       std::vector<float>& freqs)
{
    juce::ignoreUnused (scratch);
    freqs.clear();

    const int total = mags.size();
    if (total < 2 || hzPerBin <= 0.0)
    {
        state.clear();
        return;
    }

    const double nyq  = hub.getSampleRate() * 0.5;
    const double fLoD = (double) activeMinFreqHz();
    const double fHiD = juce::jmin ((double) activeMaxFreqHz(), nyq);
    if (fHiD <= fLoD)
    {
        state.clear();
        return;
    }

    const int firstBin = juce::jmax (1, (int) std::ceil  (fLoD / hzPerBin));
    const int lastBin  = juce::jmin (total - 1, (int) std::floor (fHiD / hzPerBin));
    if (lastBin <= firstBin)
    {
        state.clear();
        return;
    }

    // ---- 画布像素映射（与绘制时 inner = canvas.reduced(2) 一致）----
    const auto  canvas  = getCanvasBounds (getContentBounds());
    const float innerX0 = (float) canvas.getX() + 2.0f;
    const float innerW  = (float) juce::jmax (1, canvas.getWidth() - 4);

    auto xOfHz = [&] (double f) noexcept
    {
        return innerX0 + unitOfHz ((float) f) * innerW;
    };

    // 相邻输出点的最小像素间距：线带全宽 + 0.15px 余量。
    //   线宽是固定值（不随模块宽度变化），所以这个阈值也只随"曲线模式 × 是否
    //   有填充"变化 —— 无填充时线更粗（见绘制分支），阈值同步放大。
    const bool  noFill      = (channelMode != ChannelMode::mid);
    const float lineWidthPx = (curveMode == CurveMode::raw)
                                ? (noFill ? 1.4f : 1.1f)
                                : (noFill ? 1.9f : 1.4f);
    const float minGapPx    = lineWidthPx + 0.15f;

    // 每个 bin 的 x 位置只算一次（两次遍历共用；16k FFT 时也避免重复 log10）
    const int count = lastBin - firstBin + 1;
    binXScratch.resize ((size_t) count);
    for (int i = 0; i < count; ++i)
        binXScratch[(size_t) i] = xOfHz ((double) (firstBin + i) * hzPerBin);
    const std::vector<float>& binX = binXScratch;

    // 从组起点 i 向后吃到"与起点间距 ≥ minGapPx"为止，返回组末下标
    auto groupEnd = [&] (int i) noexcept
    {
        const float xStart = binX[(size_t) i];
        int j = i;
        while (j + 1 < count && (binX[(size_t) (j + 1)] - xStart) < minGapPx)
            ++j;
        return j;
    };

    // 第一遍：数出合并后的点数（组边界只依赖宽度/FFT，不依赖音频）
    int pts = 0;
    for (int i = 0; i < count; i = groupEnd (i) + 1)
        ++pts;

    // 最低可用 bin 的频率通常 > 20Hz（例如 1024FFT/48k 时是 46.9Hz）：
    //   在最左侧补一个"延伸到 20Hz"的点（值取第一个分组），避免左下角空白。
    const bool extendLeft = (firstBin > 1);
    const int  off        = extendLeft ? 1 : 0;
    const int  totalPts   = pts + off;

    if ((int) state.size() != totalPts)
        state.assign ((size_t) totalPts, floorDb);
    freqs.resize ((size_t) totalPts);

    // 与 processSeries 完全相同的攻/放时间常数（子弹时间只拖慢释放）
    const float releaseDt = (bulletTimeActive ? frameDtSec / bulletTimeSlowdown : frameDtSec) * releaseScale;
    const float alphaUp   = y2k::pacing::alphaForDt (0.55f, frameDtSec);
    const float alphaDown = y2k::pacing::alphaForDt (0.12f, releaseDt);

    auto feedPoint = [&] (int idx, float mag, float fCenter)
    {
        freqs[(size_t) idx] = fCenter;

        float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));
        db += slopeDbPerOct * std::log2 (juce::jmax (20.0f, fCenter) / 1000.0f);   // slope

        const float prev  = state[(size_t) idx];
        const float alpha = (db > prev) ? alphaUp : alphaDown;
        state[(size_t) idx] = juce::jlimit (floorDb, ceilDb + 12.0f,
                                            prev + alpha * (db - prev));
    };

    // 第二遍：按同样的分组规则填值
    int idx = 0;
    for (int i = 0; i < count; )
    {
        const int j = groupEnd (i);
        const int b0 = firstBin + i;
        const int b1 = firstBin + j;

        float mag = 0.0f;
        for (int k = b0; k <= b1; ++k)
            mag = juce::jmax (mag, std::abs (mags.getUnchecked (k)));

        const float fAnchor = (float) ((double) b0 * hzPerBin);

        // 左侧延伸点：与第一组同值，频率放到当前视野下限
        if (extendLeft && idx == 0)
            feedPoint (idx++, mag, activeMinFreqHz());

        feedPoint (idx++, mag, fAnchor);
        i = j + 1;
    }
}

// ----------------------------------------------------------
// rebuildDisplay —— 主曲线 + 次曲线（side / R）一起刷新
// ----------------------------------------------------------
void SpectrumModule::rebuildDisplay()
{
    if (curveMode == CurveMode::smooth)
    {
        const int N = rawMags.size();
        if (N <= 1)
            return;

        ensureDisplayCache (N);
        smoothedHz.clear();
        secondaryHz.clear();

        processSeries (rawMags, smoothedDb, blurredDb);

        // 次曲线：midSide → side；leftRight → R；mid → 不画
        if (channelMode != ChannelMode::mid && (int) rawMagsSide.size() == N)
            processSeries (rawMagsSide, secondaryDb, secondaryBlurDb);
        else
            secondaryDb.clear();
    }
    else
    {
        if (rawBins.size() < 2)
            return;

        processBinSeries (rawBins, binHz, smoothedDb, blurredDb, smoothedHz);

        if (channelMode != ChannelMode::mid && rawBinsSide.size() >= 2)
            processBinSeries (rawBinsSide, binHz, secondaryDb, secondaryBlurDb, secondaryHz);
        else
        {
            secondaryDb.clear();
            secondaryHz.clear();
        }
    }

    // 峰值数组跟随主曲线尺寸（尺寸变化时重置，避免旧尺度残留）
    if ((int) peakDb.size() != (int) smoothedDb.size())
        peakDb.assign (smoothedDb.size(), floorDb);
}

void SpectrumModule::ensureDisplayCache (int numPoints)
{
    numPoints = juce::jmax (2, numPoints);

    const double sampleRate = hub.getSampleRate();
    if (slopeCacheSize == numPoints
        && slopeCacheScale == fftScale
        && std::abs (slopeCacheSampleRate - sampleRate) <= 0.5
        && std::abs (slopeCacheDbPerOct - slopeDbPerOct) < 0.01f
        && (int) slopeOffsetDb.size() == numPoints)
    {
        return;
    }

    slopeOffsetDb.resize ((size_t) numPoints);

    const double fMin   = (double) activeMinFreqHz();
    const double fMax   = juce::jmax (fMin, juce::jmin ((double) activeMaxFreqHz(), sampleRate * 0.5));

    for (int col = 0; col < numPoints; ++col)
    {
        const double t = (double) col / (double) juce::jmax (1, numPoints - 1);
        const double f = y2k::fscale::fromUnit (fftScale, t, fMin, fMax);
        const double octaves = std::log2 (juce::jmax (20.0, f) / 1000.0);
        slopeOffsetDb[(size_t) col] = (float) (slopeDbPerOct * octaves);
    }

    slopeCacheSize = numPoints;
    slopeCacheScale = fftScale;
    slopeCacheSampleRate = sampleRate;
    slopeCacheDbPerOct = slopeDbPerOct;
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
// ---- GlDirectModuleBase 钩子（v2.9.4 迁移）----
juce::Rectangle<int> SpectrumModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void SpectrumModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 接管时只保留轴标签与悬停标尺（GL 内容之上）
    drawAxisLabels (g, canvas);
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void SpectrumModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const int cw = contentBounds.getWidth();
    const int ch = contentBounds.getHeight();
    if (cw <= 0 || ch <= 0) return;

    // 1) 静态层：尺寸 / DPI / 主题配色 / 外观代次 / 字体代次 / 绿幕显露态变化时才重建
    const float physScale       = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto  colourKey       = computeThemeColourKey();
    const auto  appearanceGen   = PinkXP::getAppearanceGeneration();
    const auto  fontGen         = PinkXP::getFontGeneration();
    const bool  chromaHidden    = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);
    if (! staticLayer_.isValid()
        || staticLayerBounds_ != contentBounds
        || std::abs (staticLayerScale_ - physScale) > 0.01f
        || staticLayerColourKey_ != colourKey
        || staticLayerAppearanceGen_ != appearanceGen
        || staticLayerFontGen_ != fontGen
        || staticLayerChromaHidden_ != chromaHidden)
    {
        renderStaticLayer (contentBounds, physScale);
        staticLayerColourKey_     = colourKey;
        staticLayerAppearanceGen_ = appearanceGen;
        staticLayerFontGen_       = fontGen;
        staticLayerChromaHidden_  = chromaHidden;
    }

    g.setOpacity (1.0f);
    g.drawImageTransformed (staticLayer_,
                            juce::AffineTransform::scale (1.0f / staticLayerScale_)
                                .translated ((float) contentBounds.getX(), (float) contentBounds.getY()));

    // 2) 动态曲线：每帧直接画
    auto canvas = getCanvasBounds(contentBounds);
    if (canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (canvas.reduced (2));
            drawCurves (g, canvas);
        }
        PinkXP::Craft::drawCanvasFinishOverlay (g, canvas);
    }
    curve_cache_dirty_ = false;

    // 鼠标悬停标尺
    if (hoverActive)
        drawHoverRuler(g, canvas);
}

// ----------------------------------------------------------
// 能量色谱 LUT 与频段聚合辅助（GL / CPU 双路径共用）
// ----------------------------------------------------------
void SpectrumModule::ensureEnergyLut() const
{
    const auto themeKey = computeThemeColourKey();
    if (energyLutValid_
        && energyLutMap_ == colorMap
        && (colorMap != ColorMap::theme || energyLutThemeKey_ == themeKey))
        return;

    const bool lightThemeCanvas = (PinkXP::content.getPerceivedBrightness() > 0.55f);

    auto evalColour = [&] (float t) -> juce::Colour
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        switch (colorMap)
        {
            case ColorMap::miniMeter:
            {
                // 还原 MiniMeter 垂直频谱光幕：暗夜靛黑 → 深紫罗兰 → 浓郁紫红 → 炽烈霓虹品红 → 炽白粉光顶
                const WeightedColourStop stops[7] = {
                    { 0.00f, juce::Colour (0xff070112) },
                    { 0.20f, juce::Colour (0xff200338) },
                    { 0.42f, juce::Colour (0xff5f0375) },
                    { 0.62f, juce::Colour (0xffba008d) },
                    { 0.78f, juce::Colour (0xffff0096) },
                    { 0.92f, juce::Colour (0xffff42bc) },
                    { 1.00f, juce::Colour (0xffffaae6) },
                };
                return sampleWeightedStops (stops, 7, t);
            }
            case ColorMap::magma:
            {
                const WeightedColourStop stops[5] = {
                    { 0.00f, juce::Colour (0xff05040d) },
                    { 0.26f, juce::Colour (0xff3b0f70) },
                    { 0.54f, juce::Colour (0xffb63679) },
                    { 0.80f, juce::Colour (0xfffb8861) },
                    { 1.00f, juce::Colour (0xfffcfdbf) },
                };
                return sampleWeightedStops (stops, 5, t);
            }
            case ColorMap::inferno:
            {
                const WeightedColourStop stops[5] = {
                    { 0.00f, juce::Colour (0xff04030c) },
                    { 0.26f, juce::Colour (0xff420a68) },
                    { 0.54f, juce::Colour (0xffbc3754) },
                    { 0.80f, juce::Colour (0xfff98e09) },
                    { 1.00f, juce::Colour (0xfffcffa4) },
                };
                return sampleWeightedStops (stops, 5, t);
            }
            case ColorMap::plasma:
            {
                const WeightedColourStop stops[5] = {
                    { 0.00f, juce::Colour (0xff0d0887) },
                    { 0.26f, juce::Colour (0xff6a00a8) },
                    { 0.54f, juce::Colour (0xffcb4679) },
                    { 0.80f, juce::Colour (0xfff89540) },
                    { 1.00f, juce::Colour (0xfff0f921) },
                };
                return sampleWeightedStops (stops, 5, t);
            }
            case ColorMap::ice:
            {
                const WeightedColourStop stops[5] = {
                    { 0.00f, juce::Colour (0xff030816) },
                    { 0.26f, juce::Colour (0xff0d2f6b) },
                    { 0.54f, juce::Colour (0xff1888c8) },
                    { 0.80f, juce::Colour (0xff6be2f8) },
                    { 1.00f, juce::Colour (0xfff2fcff) },
                };
                return sampleWeightedStops (stops, 5, t);
            }
            case ColorMap::rainbow:
            {
                const WeightedColourStop stops[7] = {
                    { 0.00f, juce::Colour (0xff080418) },
                    { 0.18f, juce::Colour (0xff271680) },
                    { 0.36f, juce::Colour (0xff0e6bd6) },
                    { 0.54f, juce::Colour (0xff06c4b0) },
                    { 0.72f, juce::Colour (0xff6be846) },
                    { 0.86f, juce::Colour (0xffffcc24) },
                    { 1.00f, juce::Colour (0xffff3888) },
                };
                return sampleWeightedStops (stops, 7, t);
            }
            case ColorMap::theme:
            default:
            {
                if (lightThemeCanvas)
                {
                    const WeightedColourStop stops[5] = {
                        { 0.00f, PinkXP::interpolateOkLab (PinkXP::content, PinkXP::chartRamp[1], 0.35f) },
                        { 0.26f, PinkXP::chartRamp[2] },
                        { 0.54f, PinkXP::chartRamp[4] },
                        { 0.80f, PinkXP::chartRamp[6] },
                        { 1.00f, PinkXP::interpolateOkLab (PinkXP::chartRamp[7], PinkXP::sel, 0.45f) },
                    };
                    return sampleWeightedStops (stops, 5, t);
                }
                const WeightedColourStop stops[5] = {
                    { 0.00f, PinkXP::interpolateOkLab (juce::Colour (0xff07050c), PinkXP::content, 0.45f) },
                    { 0.26f, PinkXP::interpolateOkLab (PinkXP::content, PinkXP::chartRamp[3], 0.62f) },
                    { 0.56f, PinkXP::chartRamp[5] },
                    { 0.82f, PinkXP::interpolateOkLab (PinkXP::chartRamp[6], PinkXP::sel, 0.55f) },
                    { 1.00f, PinkXP::interpolateOkLab (PinkXP::sel, PinkXP::hl, 0.68f) },
                };
                return sampleWeightedStops (stops, 5, t);
            }
        }
    };

    for (int i = 0; i < 256; ++i)
        energyLut_[(size_t) i] = evalColour ((float) i / 255.0f);

    energyLutMap_      = colorMap;
    energyLutThemeKey_ = themeKey;
    energyLutValid_    = true;
}

juce::Colour SpectrumModule::sampleEnergyPalette (float t) const noexcept
{
    const int idx = juce::jlimit (0, 255, juce::roundToInt (juce::jlimit (0.0f, 1.0f, t) * 255.0f));
    return energyLut_[(size_t) idx];
}

juce::Colour SpectrumModule::energyContourColour (bool darkCanvas) const noexcept
{
    if (colorMap == ColorMap::theme)
    {
        return darkCanvas
                   ? PinkXP::interpolateOkLab (PinkXP::chartRamp[6], juce::Colours::white, 0.58f)
                   : PinkXP::interpolateOkLab (PinkXP::chartRamp[7], PinkXP::ink, 0.35f);
    }

    switch (colorMap)
    {
        case ColorMap::magma:
        case ColorMap::inferno: return juce::Colour (0xfffef2cf);
        case ColorMap::plasma:  return juce::Colour (0xfff5f8ca);
        case ColorMap::ice:     return juce::Colour (0xffe8faff);
        case ColorMap::rainbow: return juce::Colour (0xfff0f4ff);
        case ColorMap::miniMeter:
        default:                return juce::Colour (0xffd2e0fc);   // MiniMeter 标志性冷冰白轮廓线
    }
}

juce::Colour SpectrumModule::energySecondaryColour (bool darkCanvas) const noexcept
{
    if (darkCanvas)
        return juce::Colour (0xff9aa8c7).withAlpha (0.78f);
    return PinkXP::ink.withAlpha (0.58f);
}

void SpectrumModule::buildSubdividedCurve (const std::vector<float>& dbVals,
                                           const std::vector<float>& freqs,
                                           juce::Rectangle<int> canvas,
                                           juce::Rectangle<int> inner,
                                           bool smooth, int subdiv,
                                           std::vector<float>& outX,
                                           std::vector<float>& outY) const
{
    const int n = (int) dbVals.size();
    outX.clear();
    outY.clear();
    if (n < 2) return;

    const float x0     = (float) inner.getX();
    const float xLen   = (float) inner.getWidth();
    const float invMax = 1.0f / (float) juce::jmax (1, n - 1);
    const float yTop   = (float) inner.getY();
    const float yBot   = (float) inner.getBottom();

    const bool useFreqAxis = ((int) freqs.size() == n);

    std::vector<float> px ((size_t) n), py ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        px[(size_t) i] = useFreqAxis ? (x0 + unitOfHz (freqs[(size_t) i]) * xLen)
                                     : (x0 + (float) i * invMax * xLen);
        py[(size_t) i] = dbToY (dbVals[(size_t) i], canvas);
    }

    if (! smooth)
    {
        outX = std::move (px);
        outY = std::move (py);
        return;
    }

    constexpr float floorPixelTol = 0.75f;
    const int kSubdiv = juce::jlimit (1, 8, subdiv);

    auto atFloor = [&] (float y) noexcept { return (yBot - y) <= floorPixelTol; };

    outX.reserve ((size_t) (n * kSubdiv + 4));
    outY.reserve ((size_t) (n * kSubdiv + 4));
    outX.push_back (px[0]);
    outY.push_back (py[0]);

    for (int i = 0; i < n - 1; ++i)
    {
        const float p0x = (i == 0)     ? px[0]              : px[(size_t) i - 1];
        const float p0y = (i == 0)     ? py[0]              : py[(size_t) i - 1];
        const float p1x = px[(size_t) i];
        const float p1y = py[(size_t) i];
        const float p2x = px[(size_t) i + 1];
        const float p2y = py[(size_t) i + 1];
        const float p3x = (i + 2 >= n) ? px[(size_t) n - 1] : px[(size_t) i + 2];
        const float p3y = (i + 2 >= n) ? py[(size_t) n - 1] : py[(size_t) i + 2];

        if (atFloor (p1y) && atFloor (p2y))
        {
            outX.push_back (p2x);
            outY.push_back (yBot);
            continue;
        }

        const float c1x = p1x + (p2x - p0x) / 6.0f;
        const float c2x = p2x - (p3x - p1x) / 6.0f;
        float c1y = p1y + (p2y - p0y) / 6.0f;
        float c2y = p2y - (p3y - p1y) / 6.0f;
        if (atFloor (p1y)) c1y = p1y;
        if (atFloor (p2y)) c2y = p2y;
        c1y = juce::jlimit (yTop, yBot, c1y);
        c2y = juce::jlimit (yTop, yBot, c2y);

        const int segSubdiv = (std::abs (p2x - p1x) < 1.0f) ? 1 : kSubdiv;
        for (int s = 1; s <= segSubdiv; ++s)
        {
            const float t  = (float) s / (float) segSubdiv;
            const float u  = 1.0f - t;
            const float w0 = u * u * u;
            const float w1 = 3.0f * u * u * t;
            const float w2 = 3.0f * u * t * t;
            const float w3 = t * t * t;
            outX.push_back (w0 * p1x + w1 * c1x + w2 * c2x + w3 * p2x);
            outY.push_back (w0 * p1y + w1 * c1y + w2 * c2y + w3 * p2y);
        }
    }
}

void SpectrumModule::computeBandLevels (std::vector<float>& outMainDb,
                                        std::vector<float>* outPeakDb,
                                        std::vector<float>* outSecondaryDb) const
{
    const int numBands = getBarBandCount();
    outMainDb.assign ((size_t) numBands, floorDb);
    if (outPeakDb != nullptr)
        outPeakDb->assign ((size_t) numBands, floorDb);
    if (outSecondaryDb != nullptr)
        outSecondaryDb->assign ((size_t) numBands, floorDb);

    const int N = (int) smoothedDb.size();
    if (N < 2)
        return;

    auto aggregateSeries = [&] (const std::vector<float>& srcDb,
                                const std::vector<float>& srcHz,
                                std::vector<float>& dst)
    {
        const int n = (int) srcDb.size();
        if (n < 2) return;
        const bool useFreq = ((int) srcHz.size() == n);
        const float invNm1 = 1.0f / (float) (n - 1);
        auto uOf = [&] (int i) noexcept -> float
        {
            return useFreq ? unitOfHz (srcHz[(size_t) i]) : (float) i * invNm1;
        };

        int cursor = 0;
        for (int b = 0; b < numBands; ++b)
        {
            const float u0   = (float) b / (float) numBands;
            const float u1   = (float) (b + 1) / (float) numBands;
            const float uMid = 0.5f * (u0 + u1);

            while (cursor < n && uOf (cursor) < u0)
                ++cursor;

            float best = -1000.0f;
            bool  hit  = false;
            int   k    = cursor;
            while (k < n)
            {
                const float uk = uOf (k);
                if (uk > u1 && (b + 1 < numBands))
                    break;
                if (uk >= u0)
                {
                    best = juce::jmax (best, srcDb[(size_t) k]);
                    hit = true;
                }
                ++k;
            }

            if (! hit)
            {
                const int hi = juce::jlimit (0, n - 1, cursor);
                const int lo = juce::jmax (0, hi - 1);
                const float uLo = uOf (lo);
                const float uHi = uOf (hi);
                if (hi > lo && uHi > uLo + 1.0e-5f)
                {
                    const float frac = juce::jlimit (0.0f, 1.0f, (uMid - uLo) / (uHi - uLo));
                    best = srcDb[(size_t) lo] + frac * (srcDb[(size_t) hi] - srcDb[(size_t) lo]);
                }
                else
                {
                    best = srcDb[(size_t) hi];
                }
            }
            dst[(size_t) b] = juce::jlimit (floorDb, ceilDb + 12.0f, best);
        }
    };

    aggregateSeries (smoothedDb, smoothedHz, outMainDb);

    if (outPeakDb != nullptr)
    {
        *outPeakDb = outMainDb;
        if (peakHoldEnabled && (int) peakDb.size() == N)
        {
            std::vector<float> tmpPeak ((size_t) numBands, floorDb);
            aggregateSeries (peakDb, smoothedHz, tmpPeak);
            for (int b = 0; b < numBands; ++b)
                (*outPeakDb)[(size_t) b] = juce::jmax ((*outPeakDb)[(size_t) b], tmpPeak[(size_t) b]);
        }
        if ((int) barPeakDb.size() == numBands)
        {
            for (int b = 0; b < numBands; ++b)
                (*outPeakDb)[(size_t) b] = juce::jmax ((*outPeakDb)[(size_t) b], barPeakDb[(size_t) b]);
        }
    }

    if (outSecondaryDb != nullptr && hasSecondaryCurve() && (int) secondaryDb.size() >= 2)
        aggregateSeries (secondaryDb, secondaryHz, *outSecondaryDb);
}

// ----------------------------------------------------------
// buildGlSnapshot —— 频谱曲线 GL 顶点流（v2.9.4 / v3.6 7 模态）
// ----------------------------------------------------------
void SpectrumModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto canvas = getCanvasBounds (getContentBounds());
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const int N = (int) smoothedDb.size();

    auto snap = beginGlSnapshot();
    // 曲线与斜线轨迹保持平滑抗锯齿（Pixel XP 工艺体现在 0px 边框、锐利柱/LED 砖块与字体上）
    snap->snapToPixels    = false;
    snap->renderToLogical = true;
    snap->logicalW        = canvas.getWidth();
    snap->logicalH        = canvas.getHeight();
    snap->canvasLocal     = canvas;
    snap->fxRect          = canvas;

    // ---- 背景（令牌驱动凹槽底板；绿幕下为纯绿）----
    appendWellFrame (snap->vertices, canvas, PinkXP::content);

    auto inner = canvas.reduced (2);

    // ---- 网格（与 drawGrid 逐笔对应，全部经由 appendGridLine 耦合 SkinTokens）----
    if (! tokens.isChromaKey && showGrid)
    {
        const bool isModern = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
        const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
        const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
        const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
        const juce::Colour etchedMetal = PinkXP::skinHairlineOn (PinkXP::content, 0.80f)
                                             .interpolatedWith (PinkXP::hl, 0.25f);
        const auto zeroGridCol = isModern ? PinkXP::pink400.withAlpha (0.85f)
                               : isChrome ? PinkXP::pink400.interpolatedWith (PinkXP::hl, 0.35f).withAlpha (0.90f)
                               : isGlass  ? PinkXP::pink400.withAlpha (0.82f)
                               : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.35f).withAlpha (0.95f)
                               : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::hl, 0.25f)
                                          : PinkXP::pink500;
        const auto dbGridCol   = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.72f)
                               : isChrome ? etchedMetal.withAlpha (0.82f)
                               : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.70f)
                               : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                               : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                          : PinkXP::pink400.withAlpha (0.75f);
        const auto freqGridCol = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.72f)
                               : isChrome ? etchedMetal.withAlpha (0.85f)
                               : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.70f)
                               : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                               : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                          : PinkXP::pink400;

        const int dbStep = ((ceilDb - floorDb) <= 44.0f) ? 10 : 20;
        const int startDb = ((int) std::ceil (floorDb / (float) dbStep)) * dbStep;
        for (int db = startDb; db <= (int) std::floor (ceilDb); db += dbStep)
        {
            const int y = (int) std::round (dbToY ((float) db, canvas));
            if (y < inner.getY() || y > inner.getBottom()) continue;
            appendGridLine (snap->vertices,
                            (float) inner.getX(), (float) y,
                            (float) inner.getRight(), (float) y,
                            true, (db == 0) ? zeroGridCol : dbGridCol);
        }

        std::vector<float> majorFreqs, minorFreqs;
        gridFreqs (majorFreqs, minorFreqs);
        for (float f : minorFreqs)
        {
            const int x = (int) std::round (freqToX (f, canvas));
            if (x > inner.getX() && x < inner.getRight())
                appendGridLine (snap->vertices,
                                (float) x, (float) inner.getY(),
                                (float) x, (float) inner.getBottom(),
                                false, freqGridCol);
        }
        for (float f : majorFreqs)
        {
            const int x = (int) std::round (freqToX (f, canvas));
            if (x > inner.getX() && x < inner.getRight())
                appendGridLine (snap->vertices,
                                (float) x, (float) inner.getY(),
                                (float) x, (float) inner.getBottom(),
                                true, freqGridCol);
        }
    }

    if (N <= 1)
    {
        const int bgVerts = (int) (snap->vertices.size() / 6);
        if (bgVerts > 0)
            snap->segments.push_back ({ 0, bgVerts, Primitive::Triangles });
        publishGlSnapshot (std::move (snap));
        return;
    }

    const float x0       = (float) inner.getX();
    const float xLen     = (float) inner.getWidth();
    const float yTop     = (float) inner.getY();
    const float yBot     = (float) inner.getBottom();
    const float spanH    = juce::jmax (1.0f, yBot - yTop);
    const float yMid     = 0.5f * (yTop + yBot);
    const float halfSpan = 0.5f * spanH;

    const bool rawMode      = (curveMode == CurveMode::raw);
    const bool isBarsMode   = (fillStyle == FillStyle::bars);
    const bool isLedMode    = (fillStyle == FillStyle::led);
    const bool isGlowMode   = (fillStyle == FillStyle::glow);
    const bool isEnergyMode = (fillStyle == FillStyle::energy);
    const bool isMirrorMode = (fillStyle == FillStyle::mirror);
    const bool isBandMode   = (isBarsMode || isLedMode);
    const bool wantsFill    = (channelMode == ChannelMode::mid)
                              && (fillStyle == FillStyle::fill || fillStyle == FillStyle::glow);

    const bool  darkCanvas    = (PinkXP::content.getPerceivedBrightness() < 0.50f);
    const bool  greenBackdrop = PinkXP::isGreenScreenBackdrop();
    const bool  isChrome      = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool  isGlass       = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const float energyTopA    = greenBackdrop ? 1.0f : juce::jlimit (0.0f, 1.0f, 0.92f * tokens.areaFillAlphaScale);
    const float energyBotA    = greenBackdrop ? 1.0f : juce::jlimit (0.0f, 1.0f, 0.84f * tokens.areaFillAlphaScale);

    if (isEnergyMode || isMirrorMode || isBandMode)
        ensureEnergyLut();

    // 仅在需要曲线路径时细分主曲线
    if (! isBandMode)
        buildSubdividedCurve (smoothedDb, smoothedHz, canvas, inner,
                              useSpline(), splineSubdiv(), subX_, subY_);
    const int M = (int) subX_.size();

    // ---- 风格化曲线折线（经由 appendStyledPolyline：Pixel XP 像素对齐无雾；Neon/CRT 自动辉光底描边）----
    auto appendStyledCurve = [&] (const std::vector<float>& xs,
                                  const std::vector<float>& ys,
                                  float width, juce::Colour c)
    {
        const int n = (int) xs.size();
        if (n < 2)
            return;
        polyPtsScratch_.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
            polyPtsScratch_[(size_t) i] = { xs[(size_t) i], ys[(size_t) i] };
        appendStyledPolyline (snap->vertices, polyPtsScratch_.data(), n, c, width);
    };

    // 模式专属外晕带（仅用于平滑皮肤柔光层或 Glow 模态外晕，不叠加重复 glow pass）
    auto appendPlainRibbon = [&] (const std::vector<float>& xs,
                                  const std::vector<float>& ys,
                                  float halfW, juce::Colour c)
    {
        const int n = (int) xs.size();
        if (n < 2 || halfW <= 0.0f || c.getFloatAlpha() <= 0.001f)
            return;

        const float r = c.getFloatRed(), g = c.getFloatGreen(),
                    b = c.getFloatBlue(), a = c.getFloatAlpha();
        std::vector<float> nx ((size_t) n), ny ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            float ax = 0.0f, ay = 0.0f;
            if (i > 0)
            {
                const float dx = xs[(size_t) i] - xs[(size_t) i - 1];
                const float dy = ys[(size_t) i] - ys[(size_t) i - 1];
                const float l = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-6f) { ax += -dy / l; ay += dx / l; }
            }
            if (i + 1 < n)
            {
                const float dx = xs[(size_t) i + 1] - xs[(size_t) i];
                const float dy = ys[(size_t) i + 1] - ys[(size_t) i];
                const float l = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-6f) { ax += -dy / l; ay += dx / l; }
            }
            const float l = std::sqrt (ax * ax + ay * ay);
            nx[(size_t) i] = (l > 1.0e-6f) ? ax / l : 0.0f;
            ny[(size_t) i] = (l > 1.0e-6f) ? ay / l : 0.0f;
        }

        snap->vertices.reserve (snap->vertices.size() + (size_t) (n - 1) * 36);
        for (int i = 0; i + 1 < n; ++i)
        {
            const float p0x = xs[(size_t) i]     + nx[(size_t) i]     * halfW;
            const float p0y = ys[(size_t) i]     + ny[(size_t) i]     * halfW;
            const float p1x = xs[(size_t) i + 1] + nx[(size_t) i + 1] * halfW;
            const float p1y = ys[(size_t) i + 1] + ny[(size_t) i + 1] * halfW;
            const float p2x = xs[(size_t) i]     - nx[(size_t) i]     * halfW;
            const float p2y = ys[(size_t) i]     - ny[(size_t) i]     * halfW;
            const float p3x = xs[(size_t) i + 1] - nx[(size_t) i + 1] * halfW;
            const float p3y = ys[(size_t) i + 1] - ny[(size_t) i + 1] * halfW;
            const float q[36] = {
                p0x, p0y, r, g, b, a,  p1x, p1y, r, g, b, a,  p2x, p2y, r, g, b, a,
                p1x, p1y, r, g, b, a,  p3x, p3y, r, g, b, a,  p2x, p2y, r, g, b, a,
            };
            snap->vertices.insert (snap->vertices.end(), q, q + 36);
        }
    };

    auto appendDashedPolyline = [&] (const std::vector<float>& xs,
                                     const std::vector<float>& ys,
                                     juce::Colour dashC)
    {
        const int Mp = (int) xs.size();
        if (Mp <= 1) return;
        const float halfW = juce::jmax (0.5f, 0.6f * tokens.strokeWidthScale);
        float curX = xs[0], curY = ys[0];
        float arcPos = 0.0f;
        bool drawing = true;
        for (int i = 0; i + 1 < Mp; ++i)
        {
            const float dx = xs[(size_t) i + 1] - curX;
            const float dy = ys[(size_t) i + 1] - curY;
            const float segLen = std::sqrt (dx * dx + dy * dy);
            if (segLen < 1.0e-6f) continue;
            const float ux = dx / segLen, uy = dy / segLen;

            float travelled = 0.0f;
            while (travelled < segLen - 1.0e-6f)
            {
                const float remain = 3.0f - arcPos;
                const float step = std::min (remain, segLen - travelled);
                const float nx = curX + ux * step;
                const float ny = curY + uy * step;
                if (drawing)
                {
                    if (tokens.strokeGlowPass && ! greenBackdrop && tokens.strokeGlowAlpha > 0.0f)
                        appendLineSegment (snap->vertices, curX, curY, nx, ny,
                                           halfW * juce::jmax (1.6f, tokens.strokeGlowWidthMul * 0.85f),
                                           dashC.withMultipliedAlpha (tokens.strokeGlowAlpha));
                    appendLineSegment (snap->vertices, curX, curY, nx, ny, halfW, dashC);
                }
                curX = nx;
                curY = ny;
                travelled += step;
                arcPos += step;
                if (arcPos >= 3.0f - 1.0e-6f) { arcPos = 0.0f; drawing = ! drawing; }
            }
        }
    };

    auto appendChromeCrestGlint = [&] (const std::vector<float>& xs,
                                       const std::vector<float>& ys,
                                       juce::Colour baseCol,
                                       float alpha)
    {
        if ((! isChrome && ! isGlass) || rawMode || greenBackdrop)
            return;
        const int n = (int) xs.size();
        if (n < 2)
            return;
        std::vector<float> glintY ((size_t) n);
        for (int i = 0; i < n; ++i)
            glintY[(size_t) i] = juce::jmax (yTop, ys[(size_t) i] - 0.55f);
        const float glintAlpha = isGlass ? (alpha * 0.72f) : alpha;
        appendPlainRibbon (xs, glintY, isGlass ? 0.48f : 0.42f,
                           baseCol.interpolatedWith (juce::Colours::white, isGlass ? 0.72f : 0.65f).withAlpha (glintAlpha));
    };

    auto appendEnergyContour = [&] (const std::vector<float>& xs,
                                    const std::vector<float>& ys)
    {
        const auto contourCol = ghostTint (energyContourColour (darkCanvas), PinkXP::sel);
        const float coreW     = rawMode ? 1.30f : 1.64f;
        if (darkCanvas || colorMap == ColorMap::theme)
        {
            // Pixel XP：零柔焦雾；Neon/CRT：由 appendStyledPolyline 自动叠加辉光底带（绿幕下抑止外晕防绿边）
            if (! rawMode && tokens.antiAliasStrokes && ! tokens.strokeGlowPass && ! greenBackdrop)
                appendPlainRibbon (xs, ys, 1.45f, contourCol.withAlpha (0.24f));
            appendStyledCurve (xs, ys, coreW, contourCol);
            appendChromeCrestGlint (xs, ys, contourCol, darkCanvas ? 0.54f : 0.40f);
        }
        else
        {
            // 浅色画布配深色能量色谱：细暗边托底 + 高亮内芯，保证在白底和彩柱上都清晰锐利
            const auto rimCol = ghostTint (juce::Colour (0xff160824).withAlpha (0.60f),
                                           PinkXP::sel.darker (0.4f).withAlpha (0.55f));
            appendStyledCurve (xs, ys, coreW + 1.1f, rimCol);
            appendStyledCurve (xs, ys, coreW, contourCol);
            appendChromeCrestGlint (xs, ys, contourCol, 0.42f);
        }
    };

    // ---- 1) 能量光幕 / 镜像 / 专业柱图 / LED 砖柱 / 经典填充 ----
    if (isEnergyMode && M > 1)
    {
        // MiniMeter 风格：每列按自身能量垂直着色，宽频段自动按 ~3px 细分保持 OKLab 色阶顺滑过渡
        snap->vertices.reserve (snap->vertices.size() + (size_t) M * 48);
        for (int i = 0; i + 1 < M; ++i)
        {
            const float xa = subX_[(size_t) i],     ya = subY_[(size_t) i];
            const float xb = subX_[(size_t) i + 1], yb = subY_[(size_t) i + 1];
            const float segDx = xb - xa;
            if (segDx < 1.0e-4f) continue;

            const float naCheck = (yBot - ya) / spanH;
            const float nbCheck = (yBot - yb) / spanH;
            if (naCheck <= 0.002f && nbCheck <= 0.002f) continue;

            const int subSteps = juce::jlimit (1, 32, (int) std::ceil (segDx / 3.0f));
            for (int s = 0; s < subSteps; ++s)
            {
                const float t0 = (float) s / (float) subSteps;
                const float t1 = (float) (s + 1) / (float) subSteps;
                const float sx0 = xa + segDx * t0;
                const float sx1 = xa + segDx * t1;
                const float sy0 = ya + (yb - ya) * t0;
                const float sy1 = ya + (yb - ya) * t1;

                const float n0 = juce::jlimit (0.0f, 1.0f, (yBot - sy0) / spanH);
                const float n1 = juce::jlimit (0.0f, 1.0f, (yBot - sy1) / spanH);
                const float e0 = std::pow (n0, energyContrast);
                const float e1 = std::pow (n1, energyContrast);

                const auto cTop0 = ghostTint (sampleEnergyPalette (e0), PinkXP::sel).withAlpha (energyTopA);
                const auto cTop1 = ghostTint (sampleEnergyPalette (e1), PinkXP::sel).withAlpha (energyTopA);
                const auto cBot0 = makeEnergyDepthColour (cTop0, e0, energyBotA);
                const auto cBot1 = makeEnergyDepthColour (cTop1, e1, energyBotA);

                appendGradientQuad (snap->vertices,
                                    sx0, sy0,  cTop0,
                                    sx1, sy1,  cTop1,
                                    sx0, yBot, cBot0,
                                    sx1, yBot, cBot1);
            }
        }
    }
    else if (isMirrorMode && M > 1)
    {
        // 中轴细参考线（走风格化网格线）
        appendGridLine (snap->vertices, x0, yMid, x0 + xLen, yMid, true,
                        ghostTint (energyContourColour (darkCanvas), PinkXP::sel));

        snap->vertices.reserve (snap->vertices.size() + (size_t) M * 96);
        for (int i = 0; i + 1 < M; ++i)
        {
            const float xa = subX_[(size_t) i],     ya = subY_[(size_t) i];
            const float xb = subX_[(size_t) i + 1], yb = subY_[(size_t) i + 1];
            const float segDx = xb - xa;
            if (segDx < 1.0e-4f) continue;

            const float naCheck = (yBot - ya) / spanH;
            const float nbCheck = (yBot - yb) / spanH;
            if (naCheck <= 0.002f && nbCheck <= 0.002f) continue;

            const int subSteps = juce::jlimit (1, 32, (int) std::ceil (segDx / 3.0f));
            for (int s = 0; s < subSteps; ++s)
            {
                const float t0 = (float) s / (float) subSteps;
                const float t1 = (float) (s + 1) / (float) subSteps;
                const float sx0 = xa + segDx * t0;
                const float sx1 = xa + segDx * t1;
                const float sy0 = ya + (yb - ya) * t0;
                const float sy1 = ya + (yb - ya) * t1;

                const float n0 = juce::jlimit (0.0f, 1.0f, (yBot - sy0) / spanH);
                const float n1 = juce::jlimit (0.0f, 1.0f, (yBot - sy1) / spanH);
                const float e0 = std::pow (n0, energyContrast);
                const float e1 = std::pow (n1, energyContrast);

                const float dy0 = n0 * halfSpan;
                const float dy1 = n1 * halfSpan;

                const auto cOut0 = ghostTint (sampleEnergyPalette (e0), PinkXP::sel).withAlpha (energyTopA);
                const auto cOut1 = ghostTint (sampleEnergyPalette (e1), PinkXP::sel).withAlpha (energyTopA);
                const auto cMid0 = makeEnergyDepthColour (cOut0, e0, energyBotA);
                const auto cMid1 = makeEnergyDepthColour (cOut1, e1, energyBotA);

                // 上半镜像幕
                appendGradientQuad (snap->vertices,
                                    sx0, yMid - dy0, cOut0,
                                    sx1, yMid - dy1, cOut1,
                                    sx0, yMid,       cMid0,
                                    sx1, yMid,       cMid1);
                // 下半镜像幕
                appendGradientQuad (snap->vertices,
                                    sx0, yMid,       cMid0,
                                    sx1, yMid,       cMid1,
                                    sx0, yMid + dy0, cOut0,
                                    sx1, yMid + dy1, cOut1);
            }
        }
    }
    else if (isBarsMode)
    {
        // 专业频段柱形图（经由 appendStyledBarCell 耦合 7 种皮肤材质；浮空峰帽严格受 peakHoldEnabled 门控）
        computeBandLevels (bandLevelScratch_, peakHoldEnabled ? &bandPeakScratch_ : nullptr, nullptr);
        const int   numBands = getBarBandCount();
        const float stride   = xLen / (float) numBands;
        const float gap      = tokens.pixelSnapped
                                   ? ((stride >= 3.0f) ? 1.0f : 0.0f)
                                   : ((stride >= 6.0f) ? juce::jmin ((float) juce::jmax (1, tokens.defaultCellGapPx), stride * 0.24f)
                                    : (stride >= 3.0f) ? 1.0f
                                                       : juce::jmax (0.4f, stride * 0.20f));
        const float barW     = juce::jmax (1.0f, stride - gap);
        const float barAlpha = greenBackdrop ? 1.0f : 0.94f;

        snap->vertices.reserve (snap->vertices.size() + (size_t) numBands * 180);
        for (int b = 0; b < numBands; ++b)
        {
            float bx    = x0 + (float) b * stride + gap * 0.5f;
            float bw    = barW;
            const float curDb = bandLevelScratch_[(size_t) b];
            const float yBar  = dbToY (curDb, canvas);
            float barTop = yBar;
            float h      = yBot - yBar;

            if (tokens.pixelSnapped)
            {
                bx     = std::floor (bx);
                bw     = juce::jmax (1.0f, std::floor (bw));
                barTop = std::floor (barTop);
                h      = std::floor (yBot) - barTop;
            }

            if (h > 0.5f)
            {
                const float norm = juce::jlimit (0.0f, 1.0f, h / spanH);
                const auto cBar  = ghostTint (sampleEnergyPalette (0.16f + 0.84f * norm), PinkXP::sel).withAlpha (barAlpha);
                appendStyledBarCell (snap->vertices, { bx, barTop, bw, h }, cBar, true, norm * 0.22f);
            }

            // 浮空回落峰帽（严格仅在开启「峰值保持」时绘制）
            if (peakHoldEnabled)
            {
                const float pkDb = bandPeakScratch_[(size_t) b];
                if (pkDb > floorDb + 0.5f)
                {
                    float pyPk = juce::jmin (dbToY (pkDb, canvas), yBar);
                    if (tokens.pixelSnapped)
                        pyPk = std::floor (pyPk);
                    const float pkNorm = juce::jlimit (0.0f, 1.0f, (yBot - pyPk) / spanH);
                    const auto pkCol = ghostTint (
                        PinkXP::interpolateOkLab (sampleEnergyPalette (juce::jmax (0.60f, pkNorm)),
                                                  energyContourColour (darkCanvas), 0.50f),
                        PinkXP::sel);
                    appendStyledBarCell (snap->vertices, { bx, pyPk, bw, 2.0f }, pkCol, true,
                                         tokens.peakCapGlow ? 0.70f : 0.25f);
                }
            }
        }
    }
    else if (isLedMode)
    {
        // 分格 LED 砖柱分析仪（经由 appendStyledBarCell 耦合 7 种皮肤材质；Pixel XP 锐利 0px + 1px 左上高光 + 1px 隙）
        computeBandLevels (bandLevelScratch_, peakHoldEnabled ? &bandPeakScratch_ : nullptr, nullptr);
        const int   numBands = getBarBandCount();
        const float stride   = xLen / (float) numBands;
        const float gap      = tokens.pixelSnapped
                                   ? ((stride >= 3.0f) ? 1.0f : 0.0f)
                                   : ((stride >= 5.0f) ? juce::jmin ((float) juce::jmax (1, tokens.defaultCellGapPx), stride * 0.25f)
                                    : (stride >= 3.0f) ? 1.0f
                                                       : 0.5f);
        const float brickW   = tokens.pixelSnapped ? juce::jmax (1.0f, std::floor (stride - gap))
                                                   : juce::jmax (1.2f, stride - gap);
        const float segGap   = tokens.pixelSnapped ? 1.0f
                                                   : ((spanH >= 130.0f) ? (float) juce::jmax (1, tokens.defaultCellGapPx) : 1.0f);
        const float segH     = juce::jlimit (2.0f, 5.0f, std::floor (spanH / 34.0f) - segGap + 1.0f);
        const float segPitch = segH + segGap;
        const int   maxSegs  = juce::jmax (1, (int) std::floor ((spanH + segGap) / segPitch));
        const float ledAlpha = greenBackdrop ? 1.0f : 0.94f;
        const float yBotRef  = tokens.pixelSnapped ? std::floor (yBot) : yBot;

        for (int b = 0; b < numBands; ++b)
        {
            const float bx    = tokens.pixelSnapped
                                    ? std::floor (x0 + (float) b * stride + gap * 0.5f)
                                    : (x0 + (float) b * stride + gap * 0.5f);
            const float curDb = bandLevelScratch_[(size_t) b];
            const float norm  = juce::jlimit (0.0f, 1.0f, (yBot - dbToY (curDb, canvas)) / spanH);
            const int litSegs = (curDb <= floorDb + 0.35f)
                                    ? 0
                                    : juce::jlimit (1, maxSegs, (int) std::floor (norm * (float) maxSegs + 0.25f));

            for (int s = 0; s < litSegs; ++s)
            {
                const float segTop = yBotRef - (float) s * segPitch - segH;
                if (segTop < yTop - 0.5f) break;
                const float segT = ((float) s + 0.5f) / (float) maxSegs;
                const auto cBrick = sampleEnergyPalette (0.14f + 0.86f * segT);
                const float pulse = (s == litSegs - 1) ? 0.30f : 0.0f;
                appendStyledBarCell (snap->vertices, { bx, segTop, brickW, segH },
                                     ghostTint (cBrick, PinkXP::sel).withAlpha (ledAlpha), true, pulse);
            }

            // 浮空峰值 LED 砖（严格仅在开启「峰值保持」时绘制）
            if (peakHoldEnabled)
            {
                const float pkDb = bandPeakScratch_[(size_t) b];
                if (pkDb > floorDb + 0.5f)
                {
                    const float pkNorm = juce::jlimit (0.0f, 1.0f, (yBot - dbToY (pkDb, canvas)) / spanH);
                    const int pkSeg = juce::jlimit (0, maxSegs - 1,
                                                    (int) std::floor (pkNorm * (float) (maxSegs - 1) + 0.5f));
                    if (pkSeg >= litSegs || (litSegs > 0 && pkSeg == litSegs - 1))
                    {
                        const float pkTop = yBotRef - (float) pkSeg * segPitch - segH;
                        if (pkTop >= yTop - 0.5f)
                        {
                            const auto pkCol = ghostTint (
                                PinkXP::interpolateOkLab (sampleEnergyPalette (juce::jmax (0.65f, pkNorm)),
                                                          energyContourColour (darkCanvas), 0.55f),
                                PinkXP::sel);
                            appendStyledBarCell (snap->vertices, { bx, pkTop, brickW, segH }, pkCol, true,
                                                 tokens.peakCapGlow ? 0.75f : 0.35f);
                        }
                    }
                }
            }
        }
    }
    else if (wantsFill && M > 1)
    {
        const float fillBaseA = isGlowMode ? 0.38f : 0.25f;
        const float fillA     = greenBackdrop ? fillBaseA : juce::jlimit (0.0f, 1.0f, fillBaseA * tokens.areaFillAlphaScale);
        const auto fillC = ghostTint (PinkXP::chartRamp[3].withAlpha (fillA),
                                      PinkXP::sel.withAlpha (isGlowMode ? 0.32f : 0.22f));
        snap->vertices.reserve (snap->vertices.size() + (size_t) (M - 1) * 36);
        if (isGlass && ! greenBackdrop)
        {
            const auto fillTop = ghostTint (
                PinkXP::chartRamp[4].interpolatedWith (juce::Colours::white, 0.22f)
                    .withAlpha (juce::jlimit (0.0f, 0.90f, fillA * 1.24f)),
                PinkXP::sel.interpolatedWith (juce::Colours::white, 0.24f).withAlpha (0.36f));
            const auto fillBot = fillC.withMultipliedAlpha (0.38f);
            for (int i = 0; i + 1 < M; ++i)
                appendGradientQuad (snap->vertices,
                                    subX_[(size_t) i],     subY_[(size_t) i],     fillTop,
                                    subX_[(size_t) i + 1], subY_[(size_t) i + 1], fillTop,
                                    subX_[(size_t) i],     yBot,                  fillBot,
                                    subX_[(size_t) i + 1], yBot,                  fillBot);
        }
        else
        {
            const float fr = fillC.getFloatRed(), fg = fillC.getFloatGreen(),
                        fb = fillC.getFloatBlue(), fa = fillC.getFloatAlpha();
            for (int i = 0; i + 1 < M; ++i)
            {
                const float q[36] = {
                    subX_[(size_t) i],     subY_[(size_t) i],     fr, fg, fb, fa,
                    subX_[(size_t) i + 1], subY_[(size_t) i + 1], fr, fg, fb, fa,
                    subX_[(size_t) i],     yBot,                  fr, fg, fb, fa,
                    subX_[(size_t) i + 1], subY_[(size_t) i + 1], fr, fg, fb, fa,
                    subX_[(size_t) i + 1], yBot,                  fr, fg, fb, fa,
                    subX_[(size_t) i],     yBot,                  fr, fg, fb, fa,
                };
                snap->vertices.insert (snap->vertices.end(), q, q + 36);
            }
        }
    }

    // ---- 1.5) 次曲线（MID+SIDE 的 side / LEFT+RIGHT 的 R）----
    if (hasSecondaryCurve() && (int) secondaryDb.size() >= 2)
    {
        buildSubdividedCurve (secondaryDb, secondaryHz, canvas, inner,
                              useSpline(), splineSubdiv(), subX2_, subY2_);
        if ((int) subX2_.size() > 1)
        {
            if (isMirrorMode)
            {
                std::vector<float> secUp (subY2_.size()), secDn (subY2_.size());
                for (size_t i = 0; i < subY2_.size(); ++i)
                {
                    const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY2_[i]) / spanH) * halfSpan;
                    secUp[i] = yMid - dy;
                    secDn[i] = yMid + dy;
                }
                const auto secC = ghostTint (energySecondaryColour (darkCanvas), PinkXP::sel.withAlpha (0.82f));
                appendStyledCurve (subX2_, secUp, rawMode ? 1.3f : 1.55f, secC);
                appendStyledCurve (subX2_, secDn, rawMode ? 1.3f : 1.55f, secC);
            }
            else if (isEnergyMode || isBandMode)
            {
                const auto secC = ghostTint (energySecondaryColour (darkCanvas), PinkXP::sel.withAlpha (0.82f));
                appendStyledCurve (subX2_, subY2_, rawMode ? 1.35f : 1.6f, secC);
            }
            else
            {
                if (! rawMode && tokens.antiAliasStrokes && ! tokens.strokeGlowPass && ! greenBackdrop)
                    appendPlainRibbon (subX2_, subY2_, 1.4f,
                                       ghostTint (PinkXP::chartRamp[4].withAlpha (0.34f),
                                                  PinkXP::sel.withAlpha (0.34f)));
                const auto secondaryCore =
                    PinkXP::interpolateOkLab (PinkXP::chartRamp[5], PinkXP::ink, 0.22f).withAlpha (0.88f);
                appendStyledCurve (subX2_, subY2_, rawMode ? 1.4f : 1.7f,
                                   ghostTint (secondaryCore,
                                              PinkXP::sel.withAlpha (0.85f)));
            }
        }
    }

    // ---- 2) 主轮廓曲线 ----
    if (isEnergyMode && M > 1)
    {
        appendEnergyContour (subX_, subY_);
    }
    else if (isMirrorMode && M > 1)
    {
        std::vector<float> yUp ((size_t) M), yDn ((size_t) M);
        for (int i = 0; i < M; ++i)
        {
            const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY_[(size_t) i]) / spanH) * halfSpan;
            yUp[(size_t) i] = yMid - dy;
            yDn[(size_t) i] = yMid + dy;
        }
        appendEnergyContour (subX_, yUp);
        appendEnergyContour (subX_, yDn);
    }
    else if (! isBandMode && M > 1)
    {
        const bool noFill = ! wantsFill;
        if (! rawMode && ! greenBackdrop && (isGlowMode || (tokens.antiAliasStrokes && ! tokens.strokeGlowPass)))
        {
            float glowHalf  = noFill ? 1.8f
                                     : (curveMode == CurveMode::semi ? 1.05f : 1.5f);
            float glowAlpha = noFill ? 0.42f
                                     : (curveMode == CurveMode::semi ? 0.26f : 0.35f);
            if (isGlowMode) { glowHalf *= 1.75f; glowAlpha = 0.52f; }
            appendPlainRibbon (subX_, subY_, glowHalf,
                               ghostTint (PinkXP::chartRamp[5].withAlpha (glowAlpha),
                                          PinkXP::sel.withAlpha (0.60f)));
        }
        const float coreW = rawMode ? (noFill ? 1.4f : 1.1f)
                                    : (noFill ? 1.9f : (isGlowMode ? 1.8f : 1.4f));
        const auto mainCurveCol = ghostTint (PinkXP::chartRamp[6], PinkXP::sel);
        appendStyledCurve (subX_, subY_, coreW, mainCurveCol);
        appendChromeCrestGlint (subX_, subY_, mainCurveCol, darkCanvas ? 0.58f : 0.44f);
    }

    // ---- 3) 峰值保持虚线（Bars / LED 已用浮空峰帽 / 峰砖原生表达峰值保持）----
    if (peakHoldEnabled && ! isBandMode && (int) peakDb.size() == N)
    {
        buildSubdividedCurve (peakDb, smoothedHz, canvas, inner,
                              useSpline(), splineSubdiv(), subPx_, subPy_);
        const int Mp = (int) subPx_.size();
        if (Mp > 1)
        {
            const auto dashC = ((isEnergyMode || isMirrorMode) && darkCanvas)
                                   ? energyContourColour (true).withAlpha (0.70f)
                                   : PinkXP::ink.withAlpha (0.75f);
            if (isMirrorMode)
            {
                std::vector<float> pkUp ((size_t) Mp), pkDn ((size_t) Mp);
                for (int i = 0; i < Mp; ++i)
                {
                    const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subPy_[(size_t) i]) / spanH) * halfSpan;
                    pkUp[(size_t) i] = yMid - dy;
                    pkDn[(size_t) i] = yMid + dy;
                }
                appendDashedPolyline (subPx_, pkUp, dashC);
                appendDashedPolyline (subPx_, pkDn, dashC);
            }
            else
            {
                appendDashedPolyline (subPx_, subPy_, dashC);
            }
        }
    }

    // ---- 4) Sharper：重分配谱线（柱形图 / LED 砖柱模式下不显示锐化谱线）----
    if (sharpEnabled && ! isBandMode && ! sharpLines.empty())
    {
        const float yBotF = (float) inner.getBottom();
        for (const auto& l : sharpLines)
        {
            const float x = freqToX ((float) l.freqHz, canvas);
            if (x < (float) inner.getX() - 2.0f || x > (float) inner.getRight() + 2.0f)
                continue;

            const float a     = juce::jlimit (0.0f, 1.0f, l.alpha);
            const float strength = juce::jlimit (0.0f, 1.0f,
                                                  (l.magDb - floorDb) / juce::jmax (1.0f, ceilDb - floorDb));
            const auto lineColour = PinkXP::pink200.interpolatedWith (PinkXP::sel, strength);
            const float yTopF = (float) inner.getY();
            const float h = yBotF - yTopF;

            if (tokens.pixelSnapped && ! tokens.strokeGlowPass)
            {
                appendQuad (snap->vertices,
                            { std::floor (x), yTopF, 1.0f, h },
                            lineColour.withAlpha (0.85f * a));
            }
            else
            {
                if (! greenBackdrop)
                    appendQuad (snap->vertices,
                                { x - 1.5f, yTopF, 3.0f, h },
                                lineColour.withAlpha ((tokens.strokeGlowPass ? 0.28f : 0.18f) * a));
                appendQuad (snap->vertices,
                            { x - 0.6f, yTopF, 1.2f, h },
                            lineColour.withAlpha (0.85f * a));
            }
        }
    }

    const int totalVerts = (int) (snap->vertices.size() / 6);
    if (totalVerts > 0)
        snap->segments.push_back ({ 0, totalVerts, Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

void SpectrumModule::mouseMove(const juce::MouseEvent& e)
{
    ModulePanel::mouseMove(e);

    const auto canvas = getCanvasBounds(getContentBounds());
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains(pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (canvas);
    }
}

void SpectrumModule::mouseDrag (const juce::MouseEvent& e)
{
    ModulePanel::mouseDrag (e);

    // 按住拖动期间 JUCE 只派发 mouseDrag（mouseMove 会被抑制），若不同步更新
    //   hoverPos，十字线与频率/dB 读数就会停在按下那一刻的位置不动。
    const auto canvas = getCanvasBounds (getContentBounds());
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains (pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (canvas);
    }
}

// ----------------------------------------------------------
// 子弹时间：按住频谱画布 → release 回落放慢 50 倍 + 曲线转为残影色；
//   松手 → 恢复原速，残影色按 ghostFadeSec 渐隐
// ----------------------------------------------------------
void SpectrumModule::mouseDown (const juce::MouseEvent& e)
{
    ModulePanel::mouseDown (e);   // 保留基类行为（toFront / 右键菜单 / 边缘拖拽）

    // 只在"左键按在频谱画布内"时进入子弹时间：
    //   工具栏按钮 / 标题栏 / 边缘的按下交给基类逻辑（拖动、resize），
    //   不触发减速（JUCE 的按钮点击先于本组件的 mouseDown，互不干扰）。
    if (e.mods.isLeftButtonDown()
        && getCanvasBounds (getContentBounds()).contains (e.getPosition()))
    {
        bulletTimeActive = true;
        ghostMix = 1.0f;          // 曲线立即切换为残影色

        // 立刻重建快照并重绘画布内容，不等下一帧（按住瞬间就要看到变色）
        buildGlSnapshot();
        repaintFrameContent();
    }
}

void SpectrumModule::mouseUp (const juce::MouseEvent& e)
{
    ModulePanel::mouseUp (e);

    // 无论松手位置在哪（按住后拖出画布再松开），都恢复正常速度。
    //   若按下的位置不在画布（未进入子弹时间），这里只是无害地写一次 false。
    //   残影色不在这里清零：由 onFrame 按 ghostFadeSec 逐帧消散。
    if (bulletTimeActive)
        bulletTimeActive = false;
}

void SpectrumModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在画布内：擦除只需要失效画布（且不走浮动态限流，必须立刻生效）
        repaint (getCanvasBounds (getContentBounds()));
    }
}

void SpectrumModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const float freqHz = xToFreq((float) hoverPos.x, canvas);
    float t = (float)(canvas.getBottom() - hoverPos.y)
            / juce::jmax(1.0f, (float) canvas.getHeight());
    if (fillStyle == FillStyle::mirror)
    {
        const float yMid  = 0.5f * (float) (canvas.getY() + canvas.getBottom());
        const float halfH = juce::jmax (1.0f, 0.5f * (float) canvas.getHeight());
        t = std::abs ((float) hoverPos.y - yMid) / halfH;
    }
    const float db = floorDb + juce::jlimit(0.0f, 1.0f, t) * (ceilDb - floorDb);

    juce::String readout = PinkXP::formatFreqHz(freqHz)
                         + "  " + juce::String(db, 1) + " dBFS";

    // 悬停即显示当前频率坐标对应的音名
    const juce::String note = PinkXP::noteNameForHz(freqHz);
    if (note.isNotEmpty())
        readout += "  " + note;

    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
}

// ----------------------------------------------------------
// renderStaticLayer —— 底衬 / 画布 / 网格 / 坐标轴标签渲染到离屏 Image（物理像素分辨率）
// ----------------------------------------------------------
juce::uint32 SpectrumModule::computeThemeColourKey() noexcept
{
    juce::uint32 k = 2166136261u;
    for (auto c : { PinkXP::btnFace, PinkXP::face, PinkXP::content, PinkXP::pink200, PinkXP::pink400,
                    PinkXP::ink, PinkXP::sel, PinkXP::hl,
                    PinkXP::chartRamp[3], PinkXP::chartRamp[6] })
        k = (k ^ c.getARGB()) * 16777619u;
    // 视觉风格 + 基底模式 + 外观代次并入键
    k = (k ^ (juce::uint32) PinkXP::getCurrentSkin()) * 16777619u;
    k = (k ^ (juce::uint32) PinkXP::getBackdropMode()) * 16777619u;
    k = (k ^ PinkXP::getAppearanceGeneration()) * 16777619u;
    return k;
}

void SpectrumModule::renderStaticLayer (juce::Rectangle<int> contentBounds, float physicalScale) const
{
    const int pw = juce::jmax (1, juce::roundToInt ((float) contentBounds.getWidth()  * physicalScale));
    const int ph = juce::jmax (1, juce::roundToInt ((float) contentBounds.getHeight() * physicalScale));

    if (! staticLayer_.isValid() || staticLayer_.getWidth() != pw || staticLayer_.getHeight() != ph)
        staticLayer_ = juce::Image (juce::Image::ARGB, pw, ph, true, juce::SoftwareImageType());
    else
        staticLayer_.clear (staticLayer_.getBounds(), juce::Colours::transparentBlack);

    staticLayerBounds_ = contentBounds;
    staticLayerScale_  = physicalScale;

    juce::Graphics g (staticLayer_);
    g.addTransform (juce::AffineTransform::translation ((float) -contentBounds.getX(),
                                                        (float) -contentBounds.getY())
                        .scaled (physicalScale));

    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaHidden = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);
    if (! (chromaHidden && PinkXP::isTransparentBackdrop()))
    {
        const juce::Colour collarCol = chromaHidden
                                           ? PinkXP::chromaKeyGreen()
                                           : (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                                                  ? PinkXP::btnFace
                                                  : PinkXP::face);
        g.setColour (collarCol);
        g.fillRect (contentBounds);
    }

    auto canvas = getCanvasBounds(contentBounds);
    drawBackground(g, canvas);

    if (canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        drawGrid(g, canvas);
        drawAxisLabels(g, canvas);
    }
}

void SpectrumModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    if (PinkXP::isTransparentBackdrop())
        return;
    if (PinkXP::isGreenScreenBackdrop())
    {
        g.setColour (PinkXP::chromaKeyGreen());
        g.fillRect (canvas);
        return;
    }
    PinkXP::Craft::drawPanelWell (g, canvas, PinkXP::content, PinkXP::WellKind::canvas);
}

// ----------------------------------------------------------
// 网格：每 20dB 一条横线，以及 100/1k/10k 纵线（全经由 Craft::drawGridLine）
// ----------------------------------------------------------
void SpectrumModule::drawGrid(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    if (PinkXP::isChromaKeySkin() || ! showGrid)
        return;   // 绿幕或关闭网格：只留曲线与读数

    auto inner = canvas.reduced(2);
    if (inner.isEmpty()) return;

    const auto& tokens     = PinkXP::getSkinTokens();
    const bool  isModern   = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
    const bool  isChrome   = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool  isGlass    = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool  isNeon     = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool  isCrt      = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const juce::Colour etchedMetal = PinkXP::skinHairlineOn (PinkXP::content, 0.80f)
                                         .interpolatedWith (PinkXP::hl, 0.25f);
    const auto zeroGridCol = isModern ? PinkXP::pink400.withAlpha (0.85f)
                           : isChrome ? PinkXP::pink400.interpolatedWith (PinkXP::hl, 0.35f).withAlpha (0.90f)
                           : isGlass  ? PinkXP::pink400.withAlpha (0.82f)
                           : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.35f).withAlpha (0.95f)
                           : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::hl, 0.25f)
                                      : PinkXP::pink500;
    const auto dbGridCol   = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.72f)
                           : isChrome ? etchedMetal.withAlpha (0.82f)
                           : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.70f)
                           : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                           : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                      : PinkXP::pink400.withAlpha (0.75f);
    const auto freqGridCol = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.72f)
                           : isChrome ? etchedMetal.withAlpha (0.85f)
                           : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.70f)
                           : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                           : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                      : PinkXP::pink400;

    // 横线（自适应 10/20 dB 步进）
    const int dbStep = ((ceilDb - floorDb) <= 44.0f) ? 10 : 20;
    const int startDb = ((int) std::ceil (floorDb / (float) dbStep)) * dbStep;
    for (int db = startDb; db <= (int) std::floor (ceilDb); db += dbStep)
    {
        const int y = (int) std::round(dbToY((float) db, canvas));
        if (y < inner.getY() || y > inner.getBottom()) continue;
        PinkXP::Craft::drawGridLine (g, (float) inner.getX(), (float) y,
                                     (float) inner.getRight(), (float) y, true,
                                     (db == 0) ? zeroGridCol : dbGridCol);
    }

    // 纵线（主 + 次，刻度相关）
    std::vector<float> majorFreqs, minorFreqs;
    gridFreqs (majorFreqs, minorFreqs);
    for (float f : minorFreqs)
    {
        const int x = (int) std::round(freqToX(f, canvas));
        if (x > inner.getX() && x < inner.getRight())
            PinkXP::Craft::drawGridLine (g, (float) x, (float) inner.getY(),
                                         (float) x, (float) inner.getBottom(), false, freqGridCol);
    }
    for (float f : majorFreqs)
    {
        const int x = (int) std::round(freqToX(f, canvas));
        if (x > inner.getX() && x < inner.getRight())
            PinkXP::Craft::drawGridLine (g, (float) x, (float) inner.getY(),
                                         (float) x, (float) inner.getBottom(), true, freqGridCol);
    }
}

// ----------------------------------------------------------
// 绘制曲线（平滑 + 峰值保持）
//
//   · 低频段 20~100Hz 在对数坐标下挤在左侧 ~40% 像素内，而那段仅覆盖少数 FFT bin，
//     直接用 lineTo 折线会看到明显尖角。
//   · 这里用 Catmull-Rom 样条（tension 0.5）→ 转成三次贝塞尔 cubicTo：
//       - 曲线严格经过所有采样点（不改变频谱数值，纯视觉插值）
//       - 纯前端开销：对每个 x 点额外算两次减法/除法，完全不碰后端分析
//       - 端点重复处理避免首/末段被截掉
//       - 对控制点 y 做 jlimit 防止抛物线过冲跌出画布
// ----------------------------------------------------------
void SpectrumModule::drawCurves(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const int N = (int) smoothedDb.size();
    if (N <= 1) return;

    auto inner = canvas.reduced(2);
    if (inner.getWidth() <= 2) return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool snapPx  = tokens.pixelSnapped;

    // ---------- 先把采样点映射到像素坐标（曲线保持浮点平滑，不做强制阶梯量化）----------
    //   smooth 模式：等距列；Semi / Raw：按每个 bin 的中心频率定位（不等距）。
    curvePts.resize ((size_t) N);
    const float x0   = (float) inner.getX();
    const float xLen = (float) inner.getWidth();
    const float invMax = 1.0f / (float) juce::jmax (1, N - 1);

    auto xAt = [&] (int i, const std::vector<float>& hz) noexcept
    {
        if ((int) hz.size() == N)
            return (float) inner.getX() + unitOfHz (hz[(size_t) i]) * (float) inner.getWidth();
        return x0 + (float) i * invMax * xLen;
    };

    for (int i = 0; i < N; ++i)
        curvePts[(size_t) i] = { xAt (i, smoothedHz), dbToY (smoothedDb[(size_t) i], canvas) };

    // 次曲线采样点（MID+SIDE 的 side / LEFT+RIGHT 的 R）
    const bool drawSecondary = hasSecondaryCurve() && (int) secondaryDb.size() == N;
    if (drawSecondary)
    {
        secondaryPts.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
            secondaryPts[(size_t) i] = { xAt (i, secondaryHz),
                                         dbToY (secondaryDb[(size_t) i], canvas) };
    }

    // ---------- Catmull-Rom → cubicBezier 构建 Path ----------
    const float yTop = (float) inner.getY();
    const float yBot = (float) inner.getBottom();

    constexpr float floorPixelTol = 0.75f;

    auto buildSmoothPath = [&] (juce::Path& path,
                                const std::vector<juce::Point<float>>& P,
                                bool closeToBottom)
    {
        const int n = (int) P.size();
        if (n < 2) return;

        auto atFloor = [yBot, floorPixelTol] (const juce::Point<float>& p) noexcept
        {
            return (yBot - p.y) <= floorPixelTol;
        };

        if (closeToBottom) path.startNewSubPath (P[0].x, yBot);
        else               path.startNewSubPath (P[0]);

        if (closeToBottom) path.lineTo (P[0]);

        for (int i = 0; i < n - 1; ++i)
        {
            const auto& p0 = (i == 0)     ? P[0]     : P[i - 1];
            const auto& p1 = P[i];
            const auto& p2 = P[i + 1];
            const auto& p3 = (i + 2 >= n) ? P[n - 1] : P[i + 2];

            if (! useSpline())
            {
                path.lineTo (p2.x, p2.y);
                continue;
            }

            if (atFloor (p1) && atFloor (p2))
            {
                path.lineTo (p2.x, yBot);
                continue;
            }

            const float c1x = p1.x + (p2.x - p0.x) / 6.0f;
            const float c2x = p2.x - (p3.x - p1.x) / 6.0f;
            float c1y       = p1.y + (p2.y - p0.y) / 6.0f;
            float c2y       = p2.y - (p3.y - p1.y) / 6.0f;

            if (atFloor (p1)) c1y = p1.y;
            if (atFloor (p2)) c2y = p2.y;

            c1y = juce::jlimit (yTop, yBot, c1y);
            c2y = juce::jlimit (yTop, yBot, c2y);

            path.cubicTo (c1x, c1y, c2x, c2y, p2.x, p2.y);
        }

        if (closeToBottom)
        {
            path.lineTo (P[n - 1].x, yBot);
            path.closeSubPath();
        }
    };

    // 1) 7 模态绘制（Fill / Outline / Bars / Glow / Energy / LED / Mirror）
    const bool rawMode      = (curveMode == CurveMode::raw);
    const bool isBarsMode   = (fillStyle == FillStyle::bars);
    const bool isLedMode    = (fillStyle == FillStyle::led);
    const bool isGlowMode   = (fillStyle == FillStyle::glow);
    const bool isEnergyMode = (fillStyle == FillStyle::energy);
    const bool isMirrorMode = (fillStyle == FillStyle::mirror);
    const bool isBandMode   = (isBarsMode || isLedMode);
    const bool wantsFill    = (channelMode == ChannelMode::mid)
                              && (fillStyle == FillStyle::fill || fillStyle == FillStyle::glow);

    const float spanH         = juce::jmax (1.0f, yBot - yTop);
    const float yMid          = 0.5f * (yTop + yBot);
    const float halfSpan      = 0.5f * spanH;
    const bool  darkCanvas    = (PinkXP::content.getPerceivedBrightness() < 0.50f);
    const bool  greenBackdrop = PinkXP::isGreenScreenBackdrop();
    const bool  isChrome      = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool  isGlass       = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const float energyTopA    = greenBackdrop ? 1.0f : juce::jlimit (0.0f, 1.0f, 0.92f * tokens.areaFillAlphaScale);
    const float energyBotA    = greenBackdrop ? 1.0f : juce::jlimit (0.0f, 1.0f, 0.84f * tokens.areaFillAlphaScale);

    if (isEnergyMode || isMirrorMode || isBandMode)
        ensureEnergyLut();

    auto strokeChromeCrestGlintCpu = [&] (const juce::Path& p, juce::Colour baseCol, float alpha)
    {
        if ((! isChrome && ! isGlass) || rawMode || greenBackdrop)
            return;
        juce::Graphics::ScopedSaveState ssGlint (g);
        g.addTransform (juce::AffineTransform::translation (0.0f, -0.55f));
        const float glintAlpha = isGlass ? (alpha * 0.72f) : alpha;
        g.setColour (baseCol.interpolatedWith (juce::Colours::white, isGlass ? 0.72f : 0.65f).withAlpha (glintAlpha));
        g.strokePath (p, juce::PathStrokeType (isGlass ? 0.95f : 0.85f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    };

    auto strokeEnergyContourCpu = [&] (const juce::Path& p)
    {
        const auto contourCol = ghostTint (energyContourColour (darkCanvas), PinkXP::sel);
        const float coreW     = rawMode ? 1.30f : 1.65f;
        if (darkCanvas || colorMap == ColorMap::theme)
        {
            if (! rawMode && tokens.antiAliasStrokes && ! tokens.strokeGlowPass && ! greenBackdrop)
            {
                g.setColour (contourCol.withAlpha (0.24f));
                g.strokePath (p, juce::PathStrokeType (2.8f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            }
            PinkXP::Craft::drawStyledStroke (g, p, contourCol, coreW);
            strokeChromeCrestGlintCpu (p, contourCol, darkCanvas ? 0.54f : 0.40f);
        }
        else
        {
            const auto rimCol = ghostTint (juce::Colour (0xff160824).withAlpha (0.60f),
                                           PinkXP::sel.darker (0.4f).withAlpha (0.55f));
            PinkXP::Craft::drawStyledStroke (g, p, rimCol, coreW + 1.1f);
            PinkXP::Craft::drawStyledStroke (g, p, contourCol, coreW);
            strokeChromeCrestGlintCpu (p, contourCol, 0.42f);
        }
    };

    if (isEnergyMode)
    {
        fillPath.clear();
        buildSmoothPath (fillPath, curvePts, /*closeToBottom*/ true);
        buildSubdividedCurve (smoothedDb, smoothedHz, canvas, inner,
                              useSpline(), splineSubdiv(), subX_, subY_);

        const int iw = inner.getWidth();
        const int ih = inner.getHeight();
        if ((int) subX_.size() > 1 && iw > 0 && ih > 0)
        {
            if (! energyCpuCache_.isValid() || energyCpuCache_.getWidth() != iw || energyCpuCache_.getHeight() != ih)
                energyCpuCache_ = juce::Image (juce::Image::ARGB, iw, ih, true, juce::SoftwareImageType());

            {
                juce::Image::BitmapData bd (energyCpuCache_, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < ih; ++y)
                    std::memset (bd.getLinePointer (y), 0, (size_t) iw * 4u);

                const int xStart = inner.getX();
                const int yStart = inner.getY();
                int cur = 0;
                for (int col = 0; col < iw; ++col)
                {
                    const int pxCol = xStart + col;
                    const float y = sampleColumnPeakY (subX_, subY_, cur,
                                                       (float) pxCol, (float) pxCol + 1.0f, yBot);
                    const float h = yBot - y;
                    if (h <= 0.35f) continue;

                    const float norm  = juce::jlimit (0.0f, 1.0f, h / spanH);
                    const float e     = std::pow (norm, energyContrast);
                    const auto  cTop  = ghostTint (sampleEnergyPalette (e), PinkXP::sel).withAlpha (energyTopA);
                    const auto  cBot  = makeEnergyDepthColour (cTop, e, energyBotA);
                    const int y0 = juce::jlimit (0, ih - 1, (int) std::floor (juce::jmax (yTop, y - 1.0f)) - yStart);
                    const int y1 = juce::jlimit (y0 + 1, ih, (int) std::ceil (yBot) - yStart);
                    const int span = juce::jmax (1, y1 - y0 - 1);
                    const float r0 = (float) cTop.getRed(),   g0 = (float) cTop.getGreen(),
                                b0 = (float) cTop.getBlue(),  a0 = (float) cTop.getAlpha();
                    const float dr = (float) cBot.getRed()   - r0, dg = (float) cBot.getGreen() - g0,
                                db = (float) cBot.getBlue()  - b0, da = (float) cBot.getAlpha() - a0;
                    const float invSpan = 1.0f / (float) span;

                    for (int iy = y0; iy < y1; ++iy)
                    {
                        const float t = (float) (iy - y0) * invSpan;
                        const juce::uint32 a = (juce::uint32) juce::jlimit (0, 255, (int) (a0 + da * t + 0.5f));
                        const juce::uint32 r = ((juce::uint32) juce::jlimit (0, 255, (int) (r0 + dr * t + 0.5f)) * a + 127u) / 255u;
                        const juce::uint32 gr = ((juce::uint32) juce::jlimit (0, 255, (int) (g0 + dg * t + 0.5f)) * a + 127u) / 255u;
                        const juce::uint32 b = ((juce::uint32) juce::jlimit (0, 255, (int) (b0 + db * t + 0.5f)) * a + 127u) / 255u;
                        reinterpret_cast<juce::uint32*> (bd.getLinePointer (iy))[col] = (a << 24) | (r << 16) | (gr << 8) | b;
                    }
                }
            }

            juce::Graphics::ScopedSaveState clipState (g);
            g.reduceClipRegion (fillPath);
            g.setOpacity (1.0f);
            g.drawImageAt (energyCpuCache_, inner.getX(), inner.getY());
        }
    }
    else if (isMirrorMode)
    {
        // 中轴细参考线
        PinkXP::Craft::drawGridLine (g, x0, yMid, x0 + xLen, yMid, true,
                                     ghostTint (energyContourColour (darkCanvas), PinkXP::sel));

        buildSubdividedCurve (smoothedDb, smoothedHz, canvas, inner,
                              useSpline(), splineSubdiv(), subX_, subY_);
        const int M = (int) subX_.size();
        const int iw = inner.getWidth();
        const int ih = inner.getHeight();
        if (M > 1 && iw > 0 && ih > 0)
        {
            fillPath.clear();
            const float dy0 = juce::jlimit (0.0f, 1.0f, (yBot - subY_[0]) / spanH) * halfSpan;
            fillPath.startNewSubPath (subX_[0], yMid - dy0);
            for (int i = 1; i < M; ++i)
            {
                const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY_[(size_t) i]) / spanH) * halfSpan;
                fillPath.lineTo (subX_[(size_t) i], yMid - dy);
            }
            for (int i = M - 1; i >= 0; --i)
            {
                const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY_[(size_t) i]) / spanH) * halfSpan;
                fillPath.lineTo (subX_[(size_t) i], yMid + dy);
            }
            fillPath.closeSubPath();

            if (! energyCpuCache_.isValid() || energyCpuCache_.getWidth() != iw || energyCpuCache_.getHeight() != ih)
                energyCpuCache_ = juce::Image (juce::Image::ARGB, iw, ih, true, juce::SoftwareImageType());

            {
                juce::Image::BitmapData bd (energyCpuCache_, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < ih; ++y)
                    std::memset (bd.getLinePointer (y), 0, (size_t) iw * 4u);

                const int xStart = inner.getX();
                const int yStart = inner.getY();
                const float yMidLocal = yMid - (float) yStart;
                int cur = 0;
                for (int col = 0; col < iw; ++col)
                {
                    const int pxCol = xStart + col;
                    const float y = sampleColumnPeakY (subX_, subY_, cur,
                                                       (float) pxCol, (float) pxCol + 1.0f, yBot);
                    const float norm = juce::jlimit (0.0f, 1.0f, (yBot - y) / spanH);
                    const float dy   = norm * halfSpan;
                    if (dy <= 0.35f) continue;

                    const float e    = std::pow (norm, energyContrast);
                    const auto  cOut = ghostTint (sampleEnergyPalette (e), PinkXP::sel).withAlpha (energyTopA);
                    const auto  cMid = makeEnergyDepthColour (cOut, e, energyBotA);
                    const int y0 = juce::jlimit (0, ih - 1, (int) std::floor (juce::jmax (yTop, yMid - dy - 1.0f)) - yStart);
                    const int y1 = juce::jlimit (y0 + 1, ih, (int) std::ceil (juce::jmin (yBot, yMid + dy + 1.0f)) - yStart);
                    const float invHalf = 1.0f / juce::jmax (1.0f, dy + 1.0f);
                    const float rM = (float) cMid.getRed(),   gM = (float) cMid.getGreen(),
                                bM = (float) cMid.getBlue(),  aM = (float) cMid.getAlpha();
                    const float dr = (float) cOut.getRed()   - rM, dg = (float) cOut.getGreen() - gM,
                                db = (float) cOut.getBlue()  - bM, da = (float) cOut.getAlpha() - aM;

                    for (int iy = y0; iy < y1; ++iy)
                    {
                        const float t = juce::jlimit (0.0f, 1.0f, std::abs ((float) iy - yMidLocal) * invHalf);
                        const juce::uint32 a  = (juce::uint32) juce::jlimit (0, 255, (int) (aM + da * t + 0.5f));
                        const juce::uint32 r  = ((juce::uint32) juce::jlimit (0, 255, (int) (rM + dr * t + 0.5f)) * a + 127u) / 255u;
                        const juce::uint32 gr = ((juce::uint32) juce::jlimit (0, 255, (int) (gM + dg * t + 0.5f)) * a + 127u) / 255u;
                        const juce::uint32 b  = ((juce::uint32) juce::jlimit (0, 255, (int) (bM + db * t + 0.5f)) * a + 127u) / 255u;
                        reinterpret_cast<juce::uint32*> (bd.getLinePointer (iy))[col] = (a << 24) | (r << 16) | (gr << 8) | b;
                    }
                }
            }

            juce::Graphics::ScopedSaveState clipState (g);
            g.reduceClipRegion (fillPath);
            g.setOpacity (1.0f);
            g.drawImageAt (energyCpuCache_, inner.getX(), inner.getY());
        }
    }
    else if (isBarsMode)
    {
        computeBandLevels (bandLevelScratch_, peakHoldEnabled ? &bandPeakScratch_ : nullptr, nullptr);
        const int   numBands = getBarBandCount();
        const float stride   = xLen / (float) numBands;
        const float gap      = snapPx
                                   ? ((stride >= 3.0f) ? 1.0f : 0.0f)
                                   : ((stride >= 6.0f) ? juce::jmin ((float) juce::jmax (1, tokens.defaultCellGapPx), stride * 0.24f)
                                    : (stride >= 3.0f) ? 1.0f
                                                       : juce::jmax (0.4f, stride * 0.20f));
        const float barW     = juce::jmax (1.0f, stride - gap);
        const float barAlpha = greenBackdrop ? 1.0f : 0.94f;

        for (int b = 0; b < numBands; ++b)
        {
            float bx    = x0 + (float) b * stride + gap * 0.5f;
            float bw    = barW;
            const float curDb = bandLevelScratch_[(size_t) b];
            const float yBar  = dbToY (curDb, canvas);
            float barTop = yBar;
            float h      = yBot - yBar;

            if (snapPx)
            {
                bx     = std::floor (bx);
                bw     = juce::jmax (1.0f, std::floor (bw));
                barTop = std::floor (barTop);
                h      = std::floor (yBot) - barTop;
            }

            if (h > 0.5f)
            {
                const float norm = juce::jlimit (0.0f, 1.0f, h / spanH);
                const auto cBar  = ghostTint (sampleEnergyPalette (0.16f + 0.84f * norm), PinkXP::sel).withAlpha (barAlpha);
                PinkXP::Craft::drawMeterCell (g, juce::Rectangle<float> (bx, barTop, bw, h), cBar, true, norm * 0.22f);
            }

            if (peakHoldEnabled)
            {
                const float pkDb = bandPeakScratch_[(size_t) b];
                if (pkDb > floorDb + 0.5f)
                {
                    float pyPk = juce::jmin (dbToY (pkDb, canvas), yBar);
                    if (snapPx)
                        pyPk = std::floor (pyPk);
                    const float pkNorm = juce::jlimit (0.0f, 1.0f, (yBot - pyPk) / spanH);
                    const auto pkCol = ghostTint (
                        PinkXP::interpolateOkLab (sampleEnergyPalette (juce::jmax (0.60f, pkNorm)),
                                                  energyContourColour (darkCanvas), 0.50f),
                        PinkXP::sel);
                    PinkXP::Craft::drawMeterCell (g, juce::Rectangle<float> (bx, pyPk, bw, 2.0f), pkCol, true,
                                                  tokens.peakCapGlow ? 0.70f : 0.25f);
                }
            }
        }
    }
    else if (isLedMode)
    {
        computeBandLevels (bandLevelScratch_, peakHoldEnabled ? &bandPeakScratch_ : nullptr, nullptr);
        const int   numBands = getBarBandCount();
        const float stride   = xLen / (float) numBands;
        const float gap      = snapPx
                                   ? ((stride >= 3.0f) ? 1.0f : 0.0f)
                                   : ((stride >= 5.0f) ? juce::jmin ((float) juce::jmax (1, tokens.defaultCellGapPx), stride * 0.25f)
                                    : (stride >= 3.0f) ? 1.0f
                                                       : 0.5f);
        const float brickW   = snapPx ? juce::jmax (1.0f, std::floor (stride - gap))
                                      : juce::jmax (1.2f, stride - gap);
        const float segGap   = snapPx ? 1.0f
                                      : ((spanH >= 130.0f) ? (float) juce::jmax (1, tokens.defaultCellGapPx) : 1.0f);
        const float segH     = juce::jlimit (2.0f, 5.0f, std::floor (spanH / 34.0f) - segGap + 1.0f);
        const float segPitch = segH + segGap;
        const int   maxSegs  = juce::jmax (1, (int) std::floor ((spanH + segGap) / segPitch));
        const float ledAlpha = greenBackdrop ? 1.0f : 0.94f;
        const float yBotRef  = snapPx ? std::floor (yBot) : yBot;

        for (int b = 0; b < numBands; ++b)
        {
            const float bx    = snapPx ? std::floor (x0 + (float) b * stride + gap * 0.5f)
                                       : (x0 + (float) b * stride + gap * 0.5f);
            const float curDb = bandLevelScratch_[(size_t) b];
            const float norm  = juce::jlimit (0.0f, 1.0f, (yBot - dbToY (curDb, canvas)) / spanH);
            const int litSegs = (curDb <= floorDb + 0.35f)
                                    ? 0
                                    : juce::jlimit (1, maxSegs, (int) std::floor (norm * (float) maxSegs + 0.25f));

            for (int s = 0; s < litSegs; ++s)
            {
                const float segTop = yBotRef - (float) s * segPitch - segH;
                if (segTop < yTop - 0.5f) break;
                const float segT = ((float) s + 0.5f) / (float) maxSegs;
                const auto cBrick = sampleEnergyPalette (0.14f + 0.86f * segT);
                const float pulse = (s == litSegs - 1) ? 0.30f : 0.0f;
                PinkXP::Craft::drawMeterCell (g, juce::Rectangle<float> (bx, segTop, brickW, segH),
                                              ghostTint (cBrick, PinkXP::sel).withAlpha (ledAlpha), true, pulse);
            }

            if (peakHoldEnabled)
            {
                const float pkDb = bandPeakScratch_[(size_t) b];
                if (pkDb > floorDb + 0.5f)
                {
                    const float pkNorm = juce::jlimit (0.0f, 1.0f, (yBot - dbToY (pkDb, canvas)) / spanH);
                    const int pkSeg = juce::jlimit (0, maxSegs - 1,
                                                    (int) std::floor (pkNorm * (float) (maxSegs - 1) + 0.5f));
                    if (pkSeg >= litSegs || (litSegs > 0 && pkSeg == litSegs - 1))
                    {
                        const float pkTop = yBotRef - (float) pkSeg * segPitch - segH;
                        if (pkTop >= yTop - 0.5f)
                        {
                            const auto pkCol = ghostTint (
                                PinkXP::interpolateOkLab (sampleEnergyPalette (juce::jmax (0.65f, pkNorm)),
                                                          energyContourColour (darkCanvas), 0.55f),
                                PinkXP::sel);
                            PinkXP::Craft::drawMeterCell (g, juce::Rectangle<float> (bx, pkTop, brickW, segH), pkCol, true,
                                                          tokens.peakCapGlow ? 0.75f : 0.35f);
                        }
                    }
                }
            }
        }
    }
    else if (wantsFill)
    {
        fillPath.clear();
        buildSmoothPath (fillPath, curvePts, /*closeToBottom*/ true);
        const float fillBaseA = isGlowMode ? 0.38f : 0.25f;
        const float fillA     = greenBackdrop ? fillBaseA : juce::jlimit (0.0f, 1.0f, fillBaseA * tokens.areaFillAlphaScale);
        const auto fillC = ghostTint (PinkXP::chartRamp[3].withAlpha (fillA),
                                      PinkXP::sel.withAlpha (isGlowMode ? 0.32f : 0.22f));
        if (isGlass && ! greenBackdrop)
        {
            const auto fillTop = ghostTint (
                PinkXP::chartRamp[4].interpolatedWith (juce::Colours::white, 0.22f)
                    .withAlpha (juce::jlimit (0.0f, 0.90f, fillA * 1.24f)),
                PinkXP::sel.interpolatedWith (juce::Colours::white, 0.24f).withAlpha (0.36f));
            g.setGradientFill (juce::ColourGradient::vertical (fillTop, yTop, fillC.withMultipliedAlpha (0.38f), yBot));
        }
        else
        {
            g.setColour (fillC);
        }
        g.fillPath (fillPath);
    }

    // 1.5) 次曲线（side / R）
    if (drawSecondary)
    {
        if (isMirrorMode)
        {
            buildSubdividedCurve (secondaryDb, secondaryHz, canvas, inner,
                                  useSpline(), splineSubdiv(), subX2_, subY2_);
            const int M2 = (int) subX2_.size();
            if (M2 > 1)
            {
                const auto secC = ghostTint (energySecondaryColour (darkCanvas), PinkXP::sel.withAlpha (0.85f));
                const float secW = rawMode ? 1.3f : 1.55f;
                const float dy0 = juce::jlimit (0.0f, 1.0f, (yBot - subY2_[0]) / spanH) * halfSpan;

                secondaryPath.clear();
                secondaryPath.startNewSubPath (subX2_[0], yMid - dy0);
                for (int i = 1; i < M2; ++i)
                {
                    const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY2_[(size_t) i]) / spanH) * halfSpan;
                    secondaryPath.lineTo (subX2_[(size_t) i], yMid - dy);
                }
                PinkXP::Craft::drawStyledStroke (g, secondaryPath, secC, secW);

                secondaryPath.clear();
                secondaryPath.startNewSubPath (subX2_[0], yMid + dy0);
                for (int i = 1; i < M2; ++i)
                {
                    const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY2_[(size_t) i]) / spanH) * halfSpan;
                    secondaryPath.lineTo (subX2_[(size_t) i], yMid + dy);
                }
                PinkXP::Craft::drawStyledStroke (g, secondaryPath, secC, secW);
            }
        }
        else if (isEnergyMode || isBandMode)
        {
            secondaryPath.clear();
            buildSmoothPath (secondaryPath, secondaryPts, /*closeToBottom*/ false);
            PinkXP::Craft::drawStyledStroke (g, secondaryPath,
                                             ghostTint (energySecondaryColour (darkCanvas), PinkXP::sel.withAlpha (0.85f)),
                                             rawMode ? 1.35f : 1.6f);
        }
        else
        {
            secondaryPath.clear();
            buildSmoothPath (secondaryPath, secondaryPts, /*closeToBottom*/ false);
            if (! rawMode && tokens.antiAliasStrokes && ! tokens.strokeGlowPass && ! greenBackdrop)
            {
                g.setColour (ghostTint (PinkXP::chartRamp[4].withAlpha (0.34f),
                                        PinkXP::sel.withAlpha (0.34f)));
                g.strokePath (secondaryPath, juce::PathStrokeType (2.8f, juce::PathStrokeType::curved,
                                                                   juce::PathStrokeType::rounded));
            }
            PinkXP::Craft::drawStyledStroke (g, secondaryPath,
                                             ghostTint (PinkXP::interpolateOkLab (PinkXP::chartRamp[5], PinkXP::ink, 0.22f)
                                                            .withAlpha (0.88f),
                                                        PinkXP::sel.withAlpha (0.88f)),
                                             rawMode ? 1.4f : 1.7f);
        }
    }

    // 2) 主轮廓曲线
    if (isEnergyMode)
    {
        curvePath.clear();
        buildSmoothPath (curvePath, curvePts, /*closeToBottom*/ false);
        strokeEnergyContourCpu (curvePath);
    }
    else if (isMirrorMode)
    {
        const int M = (int) subX_.size();
        if (M > 1)
        {
            curvePath.clear();
            const float dy0 = juce::jlimit (0.0f, 1.0f, (yBot - subY_[0]) / spanH) * halfSpan;
            curvePath.startNewSubPath (subX_[0], yMid - dy0);
            for (int i = 1; i < M; ++i)
            {
                const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY_[(size_t) i]) / spanH) * halfSpan;
                curvePath.lineTo (subX_[(size_t) i], yMid - dy);
            }
            strokeEnergyContourCpu (curvePath);

            curvePath.clear();
            curvePath.startNewSubPath (subX_[0], yMid + dy0);
            for (int i = 1; i < M; ++i)
            {
                const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subY_[(size_t) i]) / spanH) * halfSpan;
                curvePath.lineTo (subX_[(size_t) i], yMid + dy);
            }
            strokeEnergyContourCpu (curvePath);
        }
    }
    else if (! isBandMode)
    {
        const bool noFillCpu = ! wantsFill;
        curvePath.clear();
        buildSmoothPath (curvePath, curvePts, /*closeToBottom*/ false);
        if (! rawMode && ! greenBackdrop && (isGlowMode || (tokens.antiAliasStrokes && ! tokens.strokeGlowPass)))
        {
            float glowW = noFillCpu ? 3.6f
                                    : ((curveMode == CurveMode::semi) ? 2.1f : 3.0f);
            float glowA = noFillCpu ? 0.42f : 0.32f;
            if (isGlowMode) { glowW *= 1.75f; glowA = 0.52f; }
            g.setColour (ghostTint (PinkXP::chartRamp[5].withAlpha (glowA),
                                    PinkXP::sel.withAlpha (0.60f)));
            g.strokePath (curvePath, juce::PathStrokeType (glowW, juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
        }
        const float coreW = rawMode ? (noFillCpu ? 1.4f : 1.1f)
                                    : (noFillCpu ? 1.9f : (isGlowMode ? 1.8f : 1.4f));
        const auto mainCurveCol = ghostTint (PinkXP::chartRamp[6], PinkXP::sel);
        PinkXP::Craft::drawStyledStroke (g, curvePath, mainCurveCol, coreW);
        strokeChromeCrestGlintCpu (curvePath, mainCurveCol, darkCanvas ? 0.58f : 0.44f);
    }

    // 3) 峰值保持（虚线；Bars / LED 已用浮空峰帽 / 峰砖原生表达）
    if (peakHoldEnabled && ! isBandMode && (int) peakDb.size() == N)
    {
        const float dashes[] = { 3.0f, 3.0f };
        const auto dashC = ((isEnergyMode || isMirrorMode) && darkCanvas)
                               ? energyContourColour (true).withAlpha (0.70f)
                               : PinkXP::ink.withAlpha (0.75f);
        const float dashW = juce::jmax (1.0f, 1.2f * tokens.strokeWidthScale);

        auto paintPeakPath = [&] (const juce::Path& src)
        {
            if (tokens.strokeGlowPass && ! greenBackdrop && tokens.strokeGlowAlpha > 0.0f)
            {
                dashedPeakPath.clear();
                juce::PathStrokeType (dashW * juce::jmax (1.6f, tokens.strokeGlowWidthMul * 0.85f))
                    .createDashedStroke (dashedPeakPath, src, dashes, 2);
                g.setColour (dashC.withMultipliedAlpha (tokens.strokeGlowAlpha));
                g.fillPath (dashedPeakPath);
            }
            dashedPeakPath.clear();
            juce::PathStrokeType (dashW).createDashedStroke (dashedPeakPath, src, dashes, 2);
            g.setColour (dashC);
            g.fillPath (dashedPeakPath);
        };

        if (isMirrorMode)
        {
            buildSubdividedCurve (peakDb, smoothedHz, canvas, inner,
                                  useSpline(), splineSubdiv(), subPx_, subPy_);
            const int Mp = (int) subPx_.size();
            if (Mp > 1)
            {
                const float dy0 = juce::jlimit (0.0f, 1.0f, (yBot - subPy_[0]) / spanH) * halfSpan;

                peakPath.clear();
                peakPath.startNewSubPath (subPx_[0], yMid - dy0);
                for (int i = 1; i < Mp; ++i)
                {
                    const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subPy_[(size_t) i]) / spanH) * halfSpan;
                    peakPath.lineTo (subPx_[(size_t) i], yMid - dy);
                }
                paintPeakPath (peakPath);

                peakPath.clear();
                peakPath.startNewSubPath (subPx_[0], yMid + dy0);
                for (int i = 1; i < Mp; ++i)
                {
                    const float dy = juce::jlimit (0.0f, 1.0f, (yBot - subPy_[(size_t) i]) / spanH) * halfSpan;
                    peakPath.lineTo (subPx_[(size_t) i], yMid + dy);
                }
                paintPeakPath (peakPath);
            }
        }
        else
        {
            peakCurvePts.resize ((size_t) N);
            for (int i = 0; i < N; ++i)
            {
                const float y = dbToY (peakDb[(size_t) i], canvas);
                peakCurvePts[(size_t) i] = { xAt (i, smoothedHz), y };
            }
            peakPath.clear();
            buildSmoothPath (peakPath, peakCurvePts, /*closeToBottom*/ false);
            paintPeakPath (peakPath);
        }
    }

    // 4) Sharper：重分配谱线（柱形图 / LED 砖柱模式下不显示锐化谱线）
    if (sharpEnabled && ! isBandMode && ! sharpLines.empty())
    {
        for (const auto& l : sharpLines)
        {
            const float x = freqToX ((float) l.freqHz, canvas);
            if (x < (float) inner.getX() - 2.0f || x > (float) inner.getRight() + 2.0f)
                continue;

            const float a     = juce::jlimit (0.0f, 1.0f, l.alpha);
            const float strength = juce::jlimit (0.0f, 1.0f,
                                                  (l.magDb - floorDb) / juce::jmax (1.0f, ceilDb - floorDb));
            const auto lineColour = PinkXP::pink200.interpolatedWith (PinkXP::sel, strength);
            const int yTopI = inner.getY();
            const int h     = juce::jmax (1, inner.getHeight());

            if (snapPx && ! tokens.strokeGlowPass)
            {
                g.setColour (lineColour.withAlpha (0.85f * a));
                g.fillRect ((int) std::floor (x), yTopI, 1, h);
            }
            else
            {
                if (! greenBackdrop)
                {
                    g.setColour (lineColour.withAlpha ((tokens.strokeGlowPass ? 0.28f : 0.18f) * a));
                    g.fillRect ((int) std::floor (x - 1.5f), yTopI, 3, h);
                }
                g.setColour (lineColour.withAlpha (0.85f * a));
                g.fillRect ((int) std::floor (x - 0.6f), yTopI, 1, h);
            }
        }
    }
}

// ----------------------------------------------------------
// 坐标轴标签（左侧 dB / 底部 Hz）
// ----------------------------------------------------------
void SpectrumModule::drawAxisLabels(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    if (PinkXP::isChromaKeySkin() || ! showLabels)
        return;   // 绿幕或关闭刻度标签：不画

    const auto& tokens = PinkXP::getSkinTokens();
    const bool  isModern      = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
    const bool  isChrome      = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool  isGlass       = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool  isNeon        = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool  isCrt         = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool  useAxisPlates = isModern || isChrome || isGlass || isNeon || isCrt;
    const juce::Colour collarCol = (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel)
                                       ? PinkXP::btnFace
                                       : PinkXP::face;
    const bool darkCollar = (collarCol.getPerceivedBrightness() < 0.50f);
    juce::Colour chipFill;
    juce::Colour chipBorder;
    if (isChrome)
    {
        chipFill   = collarCol.interpolatedWith (PinkXP::hl, darkCollar ? 0.14f : 0.26f).withAlpha (0.94f);
        chipBorder = darkCollar ? PinkXP::hl.withAlpha (0.34f) : PinkXP::dark.withAlpha (0.46f);
    }
    else if (isGlass)
    {
        chipFill   = collarCol.interpolatedWith (darkCollar ? juce::Colours::white : PinkXP::content,
                                                 darkCollar ? 0.14f : 0.45f).withAlpha (0.76f);
        chipBorder = darkCollar ? juce::Colours::white.withAlpha (0.28f) : PinkXP::dark.withAlpha (0.24f);
    }
    else if (isNeon)
    {
        chipFill   = PinkXP::content.interpolatedWith (juce::Colours::black, 0.50f).withAlpha (0.90f);
        chipBorder = PinkXP::sel.withAlpha (0.84f);
    }
    else if (isCrt)
    {
        chipFill   = PinkXP::content.interpolatedWith (juce::Colours::black, 0.44f).withAlpha (0.92f);
        chipBorder = PinkXP::sel.withAlpha (0.86f);
    }
    else
    {
        chipFill   = collarCol.interpolatedWith (PinkXP::content, 0.50f).withAlpha (0.76f);
        chipBorder = PinkXP::skinHairlineOn (collarCol, 0.16f);
    }
    const juce::Colour labelColour = PinkXP::readableInkOn (useAxisPlates ? chipFill.withAlpha (1.0f) : collarCol)
                                         .withAlpha ((isNeon || isCrt) ? 0.95f : (isChrome ? 0.92f : (useAxisPlates ? 0.89f : 0.85f)));
    const float axisPt = isNeon ? 7.5f : (isCrt ? 8.5f : 9.0f);

    // 左侧 dB 标签（右对齐，步进与网格一致；非 Pixel XP 风格用风格化微铭牌托底并防重叠/防裁切）
    const int dbLabelLeft = canvas.getX() - dbLabelColumnW;
    const int dbLabelW    = dbLabelColumnW - axisLabelGap;
    const int dbStep      = ((ceilDb - floorDb) <= 44.0f) ? 10 : 20;
    const int startDb     = ((int) std::ceil (floorDb / (float) dbStep)) * dbStep;
    int lastDbChipTop     = 999999;
    for (int db = startDb; db <= (int) std::floor (ceilDb); db += dbStep)
    {
        const int y = (int) std::round(dbToY((float) db, canvas));
        juce::String s = (db == 0) ? (useAxisPlates ? "0" : " 0") : juce::String (db);
        const auto img = PinkXP::cachedStaticText (
            g, s, axisPt, juce::Font::plain, labelColour,
            juce::Colours::transparentBlack, true);

        if (useAxisPlates)
        {
            const int chipH = isCrt ? 12 : 11;
            const int chipY = juce::jlimit (canvas.getY(),
                                            juce::jmax (canvas.getY(), canvas.getBottom() - chipH),
                                            y - chipH / 2);
            if (chipY + chipH <= lastDbChipTop - 1)
            {
                const juce::Rectangle<int> chipR (dbLabelLeft + 1, chipY, dbLabelW, chipH);
                PinkXP::Craft::drawBadgePill (g, chipR, chipFill, chipBorder);
                lastDbChipTop = chipR.getY();
                if (img.isValid() && img.getWidth() <= chipR.getWidth() - 4)
                {
                    g.setOpacity (1.0f);
                    img.drawAt (g,
                                (float) (chipR.getCentreX() - img.getWidth() / 2),
                                (float) (chipR.getCentreY() - img.getHeight() / 2));
                }
                else
                {
                    g.setFont (PinkXP::getAxisFont (axisPt, juce::Font::plain));
                    g.setColour (labelColour);
                    g.drawFittedText (y2k::tr (s), chipR.reduced (2, 0), juce::Justification::centred, 1, 0.65f);
                }
            }
            continue;
        }

        if (img.isValid())
        {
            g.setOpacity (1.0f);
            img.drawAt (g,
                        (float) (dbLabelLeft + dbLabelW - img.getWidth()),
                        (float) (y - img.getHeight() / 2));
        }
        else
        {
            g.setFont(PinkXP::getAxisFont(axisPt, juce::Font::plain));
            g.setColour(labelColour);
            g.drawText(y2k::tr (s), dbLabelLeft, y - 6, dbLabelW, 12,
                       juce::Justification::centredRight, false);
        }
    }

    // 底部 Hz 标签（当前视野两端 + 主网格线）
    auto hzLabel = [] (float hz) -> juce::String
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k" : juce::String ((int) hz);
    };

    std::vector<float> majorHz, minorHz;
    gridFreqs (majorHz, minorHz);
    std::vector<float> labelHz;
    labelHz.push_back (activeMinFreqHz());
    for (float f : majorHz) labelHz.push_back (f);
    labelHz.push_back (activeMaxFreqHz());

    const int hzLabelTop = canvas.getBottom() + axisLabelGap;
    int lastDrawnX = -9999;
    for (float hz : labelHz)
    {
        const int x = (int) std::round(freqToX(hz, canvas));
        if (x < canvas.getX() - 4 || x > canvas.getRight() + 4) continue;

        const juce::String text = hzLabel (hz);
        const auto img = PinkXP::cachedStaticText (
            g, text, axisPt, juce::Font::plain, labelColour,
            juce::Colours::transparentBlack, true);

        if (useAxisPlates)
        {
            const int tw = img.isValid() ? img.getWidth() : 16;
            const int chipW = juce::jlimit (18, 42, tw + (isNeon ? 10 : (isCrt ? 6 : 8)));
            const int chipH = 12;
            const int chipX = juce::jlimit (canvas.getX(),
                                            juce::jmax (canvas.getX(), canvas.getRight() - chipW),
                                            x - chipW / 2);
            if (chipX < lastDrawnX + 4) continue;
            lastDrawnX = chipX + chipW;

            const juce::Rectangle<int> chipR (chipX, hzLabelTop, chipW, chipH);
            PinkXP::Craft::drawBadgePill (g, chipR, chipFill, chipBorder);
            if (img.isValid() && img.getWidth() <= chipR.getWidth() - 4)
            {
                g.setOpacity (1.0f);
                img.drawAt (g,
                            (float) (chipR.getCentreX() - img.getWidth() / 2),
                            (float) (chipR.getCentreY() - img.getHeight() / 2));
            }
            else
            {
                g.setFont (PinkXP::getAxisFont (axisPt, juce::Font::plain));
                g.setColour (labelColour);
                g.drawFittedText (y2k::tr (text), chipR.reduced (2, 0), juce::Justification::centred, 1, 0.68f);
            }
            continue;
        }

        if (x - lastDrawnX < 24) continue;
        lastDrawnX = x;
        if (img.isValid())
        {
            g.setOpacity (1.0f);
            img.drawAt (g, (float) (x - img.getWidth() / 2), (float) hzLabelTop);
        }
        else
        {
            g.setFont(PinkXP::getAxisFont(axisPt, juce::Font::plain));
            g.setColour(labelColour);
            g.drawText(y2k::tr (text), x - 18, hzLabelTop, 36, 12,
                       juce::Justification::centred, false);
        }
    }
}
