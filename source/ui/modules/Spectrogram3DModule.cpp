#include "source/ui/modules/Spectrogram3DModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include <juce_opengl/juce_opengl.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>

namespace
{
    struct RidgeStyle
    {
        juce::Colour colour;
        float        baseAlpha = 0.0f;
        float        cpuWidth  = 0.6f;
    };

    inline RidgeStyle resolveRidgeStyle (const PinkXP::SkinTokens& tokens,
                                         bool showRidges,
                                         bool darkScene) noexcept
    {
        if (! showRidges)
            return { juce::Colours::white, 0.0f, 0.0f };

        // 清晰克制的 3D 等高线山脊轮廓：深底浅色高光、浅底深色描边、霓虹/CRT 辉光描边，
        //   开关切换对比一目了然且始终保持平滑抗锯齿（绝无锯齿虚线/碎点）
        const juce::Colour darkCrest   = PinkXP::pink50.interpolatedWith (juce::Colours::white, 0.50f);
        const juce::Colour lightCrest  = PinkXP::pink700.interpolatedWith (PinkXP::ink, 0.50f);
        const juce::Colour neonGlow    = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.42f);
        const juce::Colour crtPhosphor = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.34f);
        const juce::Colour glassCrest  = darkScene
            ? PinkXP::pink50.interpolatedWith (juce::Colours::white, 0.58f)
            : PinkXP::pink700.interpolatedWith (PinkXP::ink, 0.45f);
        const juce::Colour chromeCrest = darkScene
            ? PinkXP::hl.interpolatedWith (juce::Colours::white, 0.62f)
            : PinkXP::dark.interpolatedWith (PinkXP::ink, 0.55f);

        const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
        const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
        const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);

        // 抠像 / 透明模式：用户显式开启山脊线时不抑制，渲染柔和清晰的 0.38 轮廓（静音底仍为 alpha=0）
        if (tokens.isChromaKey)
        {
            const juce::Colour col = isNeon   ? neonGlow
                                   : isCrt    ? crtPhosphor
                                   : isGlass  ? glassCrest
                                   : isChrome ? chromeCrest
                                              : (darkScene ? darkCrest : lightCrest);
            return { col, 0.38f, 1.15f };
        }

        if (isNeon)
            return { neonGlow, 0.65f, 1.35f * tokens.strokeWidthScale };

        if (isCrt)
            return { crtPhosphor, 0.60f, 1.30f * tokens.strokeWidthScale };

        if (isGlass)
            return { glassCrest, darkScene ? 0.50f : 0.46f, 1.20f * tokens.strokeWidthScale };

        if (isChrome)
            return { chromeCrest, darkScene ? 0.54f : 0.50f, 1.22f * tokens.strokeWidthScale };

        return {
            darkScene ? darkCrest : lightCrest,
            darkScene ? 0.56f : 0.52f,
            1.25f
        };
    }
}

// ==========================================================
// Spectrogram3DModule
// (force rebuild: v1.9.4 P7 canvas bg cache)
// ==========================================================

Spectrogram3DModule::Spectrogram3DModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::spectrogram3d), hub (h)
{
    hub.retain (AnalyserHub::Kind::Spectrum);
    hub.addFrameListener (this);

    setMinSize     (128, 96);
    setDefaultSize (384, 256);
    setTitleText   ("Spectrogram 3D");
    setControlPanelEnabled (true);   // v3.1：SPEED 滑条与 TINT 按钮搬进控制面板

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        // 先标脏色板并失效离屏图像缓存，再重建 GL 快照，避免快照继续消费旧主题色板
        depthPalettesDirty   = true;
        imageCacheDirty      = true;
        cached3DImage        = juce::Image();
        cachedAppearanceGen_ = 0;
        cachedFontGen_       = 0;
        buildGlSnapshot();
        repaint();
    });

    // 预分配历史缓冲 + 幅度→色板 LUT
    buildMagLut();
    rebuildBinTiltGain();
    ensureHistory (defaultHistoryLen);
    depthPalettes.resize (visibleRows);
}

Spectrogram3DModule::~Spectrogram3DModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Spectrum);
}

// ----------------------------------------------------------
// 持久化 + 控制面板（v3.1）
//   旧存档只有 useThemeColours（TINT 按钮）：true → Theme，false → Heat。
// ----------------------------------------------------------
juce::ValueTree Spectrogram3DModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("useThemeColours", useThemeColours(), nullptr);   // 保留旧键，降级打开仍能还原着色
    s.setProperty ("colourMode",  colourMode, nullptr);
    s.setProperty ("depthDir",    depthDir, nullptr);
    s.setProperty ("speed",       (double) pixelsPerSecond, nullptr);
    s.setProperty ("floorDb",     (double) floorDb, nullptr);
    s.setProperty ("ceilDb",      (double) ceilDb, nullptr);
    s.setProperty ("slope",       (double) slopeDbPerOct, nullptr);
    s.setProperty ("depthFade",   (double) depthFadePct, nullptr);
    s.setProperty ("heightScale", (double) heightScale, nullptr);
    s.setProperty ("showRidges",  showRidges, nullptr);
    s.setProperty ("shaper",      shaperMode, nullptr);
    s.setProperty ("fftScale",    (int) fftScale, nullptr);
    s.setProperty ("freqRange",   freqRange, nullptr);
    s.setProperty ("showLabels",  showLabels, nullptr);
    return s;
}

void Spectrogram3DModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    if (state.hasProperty ("colourMode"))
        colourMode = juce::jlimit (0, 7, (int) state.getProperty ("colourMode"));
    else if (state.hasProperty ("useThemeColours"))
        colourMode = (bool) state.getProperty ("useThemeColours") ? 0 : 1;

    if (state.hasProperty ("depthDir"))    depthDir = juce::jlimit (0, 1, (int) state.getProperty ("depthDir"));
    if (state.hasProperty ("speed"))
        pixelsPerSecond = juce::jlimit (10.0f, 200.0f, (float) (double) state.getProperty ("speed"));
    if (state.hasProperty ("floorDb"))     floorDb = juce::jlimit (-110.0f, -40.0f, (float) (double) state.getProperty ("floorDb"));
    if (state.hasProperty ("ceilDb"))      ceilDb  = juce::jlimit (-30.0f, 0.0f, (float) (double) state.getProperty ("ceilDb"));
    if (floorDb > ceilDb - 10.0f)          floorDb = ceilDb - 10.0f;
    if (state.hasProperty ("slope"))       slopeDbPerOct = juce::jlimit (0.0f, 9.0f, (float) (double) state.getProperty ("slope"));
    if (state.hasProperty ("depthFade"))   depthFadePct  = juce::jlimit (0.0f, 100.0f, (float) (double) state.getProperty ("depthFade"));
    if (state.hasProperty ("heightScale")) heightScale   = juce::jlimit (0.3f, 2.0f, (float) (double) state.getProperty ("heightScale"));
    if (state.hasProperty ("showRidges"))  showRidges    = (bool) state.getProperty ("showRidges");
    if (state.hasProperty ("shaper"))      shaperMode    = juce::jlimit (0, 2, (int) state.getProperty ("shaper"));
    if (state.hasProperty ("fftScale"))    fftScale      = y2k::fscale::fromIndex ((int) state.getProperty ("fftScale"));
    if (state.hasProperty ("freqRange"))   freqRange     = juce::jlimit (0, 3, (int) state.getProperty ("freqRange"));
    if (state.hasProperty ("showLabels"))  showLabels    = (bool) state.getProperty ("showLabels");

    buildMagLut();
    rebuildBinTiltGain();
    depthPalettesDirty = true;
    imageCacheDirty    = true;
    lastCanvasW = -1;
    refreshControlPanel();
    buildGlSnapshot();
    repaint();
}

void Spectrogram3DModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("colourMode", "Colours",
        { "Theme", "Heat", "Grey", "Ice", "Magma", "Inferno", "Plasma", "Viridis" },
        "Theme follows the current colour theme. Magma, Inferno, Plasma and Viridis are perceptually uniform "
        "studio colormaps. Heat, Grey and Ice are classic ramps.").in ("Colour"));
    out.push_back (ParamDef::slider ("floorDb", "Floor", -110.0, -40.0, 2.0, " dB",
        "Lowest level shown on the 3D floor. Raise it to flatten quiet background haze; lower it to lift soft tails.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("ceilDb", "Ceiling", -30.0, 0.0, 2.0, " dB",
        "Level that reaches full height and peak colour. Lowering Ceiling makes quieter mixes rise taller.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("slope", "Tilt", 0.0, 9.0, 0.5, " dB/oct",
        "Boosts higher frequencies around a 1 kHz pivot so hi-hats and upper harmonics rise evenly with bass.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("depthFade", "Depth fog", 0.0, 100.0, 5.0, " %",
        "How strongly distant slices fade into the background. 0% keeps far history bright and crisp.")
        .in ("Colour"));

    out.push_back (ParamDef::choice ("depthDir", "Depth direction",
        { "Back-right", "Back-left" },
        "Which way older slices recede. Back-right is the classic view; Back-left mirrors the landscape.")
        .in ("3D View"));
    out.push_back (ParamDef::slider ("heightScale", "Peak height", 0.3, 2.0, 0.1, " x",
        "Vertical elevation multiplier for the 3D spectrum ridges.")
        .in ("3D View"));
    out.push_back (ParamDef::toggle ("showRidges", "Ridge lines",
        "Draw crisp highlight outlines along the crest of each 3D slice. Turn OFF for a smooth shaded terrain.")
        .in ("3D View"));
    out.push_back (ParamDef::choice ("shaper", "Sharpening",
        { "Classic", "Sharp", "Sharper" },
        "Classic: smooth raw FFT magnitude. Sharp: frequency reassignment blended with classic body for continuous note lines. "
        "Sharper: pure frequency reassignment that collapses tones into razor-thin particle beams.")
        .in ("3D View"));
    out.push_back (ParamDef::slider ("speed", "Speed", 10.0, 200.0, 1.0, " px/s",
        "How fast the landscape rolls back. Slower keeps a longer history.").in ("3D View"));

    out.push_back (ParamDef::choice ("fftScale", "FFT scale",
        { "Log", "Mel", "Linear" },
        "How frequency is laid out along the frequency axis. Log gives every octave equal room, "
        "Mel follows pitch perception, Linear gives equal Hz per pixel.")
        .in ("Axis"));
    out.push_back (ParamDef::choice ("freqRange", "Freq range",
        { "Full", "Music", "Vocal", "Bass" },
        "Zooms the frequency view: Full (20-20k Hz), Music (30-12k Hz), Vocal (80-8k Hz), or Bass (20-1k Hz).")
        .in ("Axis"));
    out.push_back (ParamDef::toggle ("showLabels", "Labels",
        "Show frequency ticks and older/newer direction hints. Turn OFF for a clean borderless waterfall.")
        .in ("Axis"));
}

double Spectrogram3DModule::getParamValue (const juce::String& id) const
{
    if (id == "colourMode")  return (double) colourMode;
    if (id == "floorDb")     return (double) floorDb;
    if (id == "ceilDb")      return (double) ceilDb;
    if (id == "slope")       return (double) slopeDbPerOct;
    if (id == "depthFade")   return (double) depthFadePct;
    if (id == "depthDir")    return (double) depthDir;
    if (id == "heightScale") return (double) heightScale;
    if (id == "showRidges")  return showRidges ? 1.0 : 0.0;
    if (id == "shaper")      return (double) shaperMode;
    if (id == "speed")       return (double) pixelsPerSecond;
    if (id == "fftScale")    return (double) (int) fftScale;
    if (id == "freqRange")   return (double) freqRange;
    if (id == "showLabels")  return showLabels ? 1.0 : 0.0;
    return 0.0;
}

void Spectrogram3DModule::setParamValue (const juce::String& id, double v)
{
    if (id == "colourMode")       { colourMode = juce::jlimit (0, 7, juce::roundToInt (v)); depthPalettesDirty = true; }
    else if (id == "floorDb")     { floorDb = juce::jlimit (-110.0f, ceilDb - 10.0f, (float) v); buildMagLut(); }
    else if (id == "ceilDb")      { ceilDb  = juce::jlimit (floorDb + 10.0f, 0.0f, (float) v); buildMagLut(); }
    else if (id == "slope")       { slopeDbPerOct = juce::jlimit (0.0f, 9.0f, (float) v); rebuildBinTiltGain(); }
    else if (id == "depthFade")   { depthFadePct = juce::jlimit (0.0f, 100.0f, (float) v); depthPalettesDirty = true; }
    else if (id == "depthDir")    { depthDir = juce::jlimit (0, 1, juce::roundToInt (v)); lastCanvasW = -1; }
    else if (id == "heightScale") { heightScale = juce::jlimit (0.3f, 2.0f, (float) v); lastCanvasW = -1; }
    else if (id == "showRidges")  { showRidges = (v >= 0.5); }
    else if (id == "shaper")      { shaperMode = juce::jlimit (0, 2, juce::roundToInt (v)); }
    else if (id == "speed")       { pixelsPerSecond = juce::jlimit (10.0f, 200.0f, (float) v); return; }
    else if (id == "fftScale")    { fftScale = y2k::fscale::fromIndex (juce::roundToInt (v)); rebuildBinTiltGain(); }
    else if (id == "freqRange")   { freqRange = juce::jlimit (0, 3, juce::roundToInt (v)); rebuildBinTiltGain(); }
    else if (id == "showLabels")  { showLabels = (v >= 0.5); }
    else return;

    imageCacheDirty = true;
    buildGlSnapshot();
    repaint();
}

float Spectrogram3DModule::activeMinFreqHz() const noexcept
{
    switch (freqRange)
    {
        case 1: return 30.0f;
        case 2: return 80.0f;
        case 3: return 20.0f;
        default: return minFreqHz;
    }
}

float Spectrogram3DModule::activeMaxFreqHz() const noexcept
{
    switch (freqRange)
    {
        case 1: return 12000.0f;
        case 2: return 8000.0f;
        case 3: return 1000.0f;
        default: return maxFreqHz;
    }
}

void Spectrogram3DModule::rebuildBinTiltGain()
{
    binTiltGain_.assign ((size_t) juce::jmax (1, numBins), 1.0f);
    const double sr = hub.getSampleRate();
    const double nyquist = (sr > 0.0) ? sr * 0.5 : 24000.0;
    tiltSampleRate_ = sr;
    if (slopeDbPerOct <= 1.0e-5f)
        return;

    const double fMin = (double) activeMinFreqHz();
    const double fMax = juce::jmin ((double) activeMaxFreqHz(), nyquist * 0.95);
    if (fMax <= fMin)
        return;

    const double denom = (double) juce::jmax (1, numBins - 1);
    for (int i = 0; i < numBins; ++i)
    {
        const double u = (double) i / denom;
        const double f = y2k::fscale::fromUnit (fftScale, u, fMin, fMax);
        const float tiltDb = slopeDbPerOct * (float) std::log2 (juce::jmax (20.0, f) / 1000.0);
        binTiltGain_[(size_t) i] = std::pow (10.0f, tiltDb * 0.05f);
    }
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> Spectrogram3DModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    return content.withTrimmedRight (sliderPanelW);
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：火山与旗帜（§8.3）
// ----------------------------------------------------------
bool Spectrogram3DModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void Spectrogram3DModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const auto canvas = getToyCanvasBounds();
    juce::Point<float> depthVec { projSlantX, -projSlantY };
    if (std::hypot (depthVec.x, depthVec.y) < 1.0e-4f)
        depthVec = { depthDir == 1 ? -0.45f : 0.45f, -0.89f };

    ctx.addModuleActorCapped (makeSpec3DPulseActor (hit.pos, canvas, depthVec), 6);
}

void Spectrogram3DModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    // v2.9.5：尺寸守卫 —— 拖动窗口/dock 时 layoutContent 每帧调用，
    //   无条件重建（S3D ~2ms）会把消息线程打满导致交互卡顿
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
        buildGlSnapshot();
}

void Spectrogram3DModule::resized()
{
    ModulePanel::resized();
}

// ----------------------------------------------------------
// 环形历史缓冲
// ----------------------------------------------------------
void Spectrogram3DModule::ensureHistory (int newLength)
{
    newLength = juce::jmax (32, juce::jmin (400, newLength));

    if (newLength == historyLen
        && (int) historyRing.size() == historyLen
        && ! historyRing.empty()
        && (int) historyRing[0].size() == numBins)
        return;

    historyLen = newLength;
    historyRing.resize ((size_t) historyLen);
    for (auto& row : historyRing)
        row.assign ((size_t) numBins, 0.0f);

    writeIdx   = 0;
    frameCount = 0;
    columnAccumulator = 0.0f;
    lastFrameMs       = 0.0f;
    depthPalettesDirty = true;
}

void Spectrogram3DModule::pushFrame (const juce::Array<float>& mags)
{
    if (historyLen <= 0 || numBins <= 0)
        return;

    auto& row = historyRing[(size_t) writeIdx];
    for (int i = 0; i < numBins; ++i)
        row[(size_t) i] = (i < mags.size()) ? std::abs (mags.getUnchecked (i)) : 0.0f;

    writeIdx   = (writeIdx + 1) % historyLen;
    frameCount = juce::jmin (frameCount + 1, historyLen);
}

// ----------------------------------------------------------
// 投影计算 —— 根据 canvas 尺寸自适配
//
//   坐标系：
//     · 最新切片（depth=0）放在屏幕右下，称为"前方"
//     · 旧切片（depth↑）向左上方偏移，形成 45° 俯视纵深感
//     · 频率轴（低频→高频）从左到右
//     · 幅度映射为竖直高度
//
//   自适应：频率轴占 canvas 宽的 82%，高度占 canvas 高的 40%，
//   斜角偏移填满剩余空间，与历史帧数解耦。
// ----------------------------------------------------------
void Spectrogram3DModule::recomputeProjection (int canvasW, int canvasH)
{
    canvasW = juce::jmax (8, canvasW);
    canvasH = juce::jmax (8, canvasH);

    if (canvasW == lastCanvasW && canvasH == lastCanvasH
        && frameCount == lastCachedFrameCount)
        return;

    const int effLen = juce::jmax (1, frameCount);
    const int depthRows = juce::jmax (1, juce::jmin (effLen, visibleRows));

    // ---- 独立缩放而非 uniform scale ----
    //   频率轴宽度占 canvas 的大部分；高度固定占 canvas 的 heightRatio；
    //   斜角偏移占用剩余空间，确保深度>0 时始终有纵深感。
    constexpr float heightRatio    = 0.40f;   // 幅度高度占 canvas 高的 40%
    constexpr float freqWidthRatio = 0.82f;   // 频率轴占 canvas 宽的 82%，剩余给深度偏移

    const float padL = 8.0f;
    const float padR = 8.0f;

    // 频率轴和深度偏移共享宽度
    const float freqTotalW = ((float) canvasW - padL - padR) * freqWidthRatio;
    const float binWidth   = (numBins > 1) ? freqTotalW / (float) (numBins - 1) : 0.0f;

    // 深度偏移：固定按 visibleRows 计算间距，而非 frameCount。
    //   当 buffer 填满后 500 层中只有最新 150 层落入可见区域，
    //   旧层自然滚动出屏幕顶部，避免被裁剪在画布中间。
    const float depthSpanX = (float) juce::jmax (1, depthRows - 1);
    const float availableForSlantX = ((float) canvasW - padL - padR) - freqTotalW;
    const float slantX = availableForSlantX / depthSpanX;

    const float depthSpanY = (float) juce::jmax (1, depthRows - 1);
    // 地板覆盖近乎全画布高度：从 originY（底）到 0（顶），
    //   无信号时灰色平行四边形将填满整个可见区域；
    //   有信号时峰值堆叠在地板之上，超出顶部由 clipRegion 裁剪。
    const float slantY = (float) (canvasH - 4) / depthSpanY;

    // 幅度高度（受 heightScale 调节）
    const float maxH = (float) canvasH * heightRatio * heightScale;

    // 原点（最新切片左下角）
    const float originX = padL;
    const float originY = (float) canvasH - 4.0f;  // 底部留 4px 给标签

    // 写入成员
    //   Back-left：最新切片靠右、深度向左后方退远 —— 原点右移整段斜移量、斜移取负，
    //   其余用到 projSlantX / projOriginX 的绘制（GL / Image / 悬停标尺）无需改动。
    const bool  mirrored = (depthDir == 1);
    const float slantAbs = juce::jmax (0.3f, slantX);
    projBinWidth = juce::jmax (0.5f, binWidth);
    projSlantX   = mirrored ? -slantAbs : slantAbs;
    projSlantY   = juce::jmax (0.1f, slantY);
    projMaxH     = juce::jmax (1.0f, maxH);
    projOriginX  = mirrored ? originX + slantAbs * depthSpanX : originX;
    projOriginY  = originY;

    lastCanvasW = canvasW;
    lastCanvasH = canvasH;
    lastCachedFrameCount = frameCount;
    depthPalettesDirty   = true; // 投影变化 → 深度色板需要重建
}

// ----------------------------------------------------------
// 频率 → 屏幕 X 坐标
// ----------------------------------------------------------
float Spectrogram3DModule::freqToScreenX (int binIndex, int totalBins) const
{
    if (totalBins <= 1) return projOriginX;
    return projOriginX + (float) binIndex * projBinWidth;
}

// ----------------------------------------------------------
// 强度 t∈[0,1] → 山峰颜色（按 useThemeColours 选择热力图或主题色阶）
// ----------------------------------------------------------
juce::Colour Spectrogram3DModule::valueToColour (float t) const noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);
    auto sampleRamp = [] (const juce::Colour* stops, int n, float u) noexcept -> juce::Colour
    {
        const float pos = juce::jlimit (0.0f, 1.0f, u) * (float) (n - 1);
        const int idx = juce::jmin (n - 2, (int) pos);
        return stops[idx].interpolatedWith (stops[idx + 1], pos - (float) idx);
    };

    switch (colourMode)
    {
        case 2: return juce::Colour::fromFloatRGBA (t, t, t, 1.0f);   // Grey
        case 3:                                                        // Ice
        {
            const juce::Colour stops[] = { juce::Colour (0xff04081c), juce::Colour (0xff1c4aa0),
                                           juce::Colour (0xff4cc0e8), juce::Colour (0xfff2fbff) };
            return sampleRamp (stops, 4, t);
        }
        case 4:                                                        // Magma
        {
            const juce::Colour stops[] = {
                juce::Colour (0xff050410), juce::Colour (0xff2b115f),
                juce::Colour (0xff721f81), juce::Colour (0xffb73779),
                juce::Colour (0xfff1605d), juce::Colour (0xfffeb078),
                juce::Colour (0xfffcfdbf)
            };
            return sampleRamp (stops, 7, t);
        }
        case 5:                                                        // Inferno
        {
            const juce::Colour stops[] = {
                juce::Colour (0xff04030b), juce::Colour (0xff280b53),
                juce::Colour (0xff65156e), juce::Colour (0xff9f2a63),
                juce::Colour (0xffd44842), juce::Colour (0xfff57d15),
                juce::Colour (0xfffac228), juce::Colour (0xfffcffa4)
            };
            return sampleRamp (stops, 8, t);
        }
        case 6:                                                        // Plasma
        {
            const juce::Colour stops[] = {
                juce::Colour (0xff0d0887), juce::Colour (0xff46039f),
                juce::Colour (0xff7201a8), juce::Colour (0xff9c179e),
                juce::Colour (0xffbd3786), juce::Colour (0xffd8576b),
                juce::Colour (0xffed7953), juce::Colour (0xfffb9f3a),
                juce::Colour (0xfff0f921)
            };
            return sampleRamp (stops, 9, t);
        }
        case 7:                                                        // Viridis
        {
            const juce::Colour stops[] = {
                juce::Colour (0xff440154), juce::Colour (0xff482878),
                juce::Colour (0xff3e4989), juce::Colour (0xff31688e),
                juce::Colour (0xff26828e), juce::Colour (0xff1f9e89),
                juce::Colour (0xff35b779), juce::Colour (0xff6ece58),
                juce::Colour (0xfffde725)
            };
            return sampleRamp (stops, 9, t);
        }
        case 1:  return heatValueToColour (t);
        default: return themeValueToColour (t);
    }
}

// ----------------------------------------------------------
// 蓝→红固定热力图（低=深蓝，高=红），不依赖主题
//
//   t=0.00 → 深蓝（无信号）
//   t=0.25 → 青色
//   t=0.50 → 绿色
//   t=0.75 → 黄色
//   t=1.00 → 红色（满幅爆点）
// ----------------------------------------------------------
juce::Colour Spectrogram3DModule::heatValueToColour (float t) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    // 六段蓝→青→绿→黄→红热力图
    struct Stop { float t; juce::Colour c; };
    const Stop stops[] = {
        { 0.00f, juce::Colour (4,   4,   36)  },  // 深蓝黑
        { 0.15f, juce::Colour (8,   20,  100) },  // 深蓝
        { 0.30f, juce::Colour (0,   100, 180) },  // 蓝
        { 0.45f, juce::Colour (0,   180, 160) },  // 青
        { 0.60f, juce::Colour (20,  210, 40)  },  // 绿
        { 0.78f, juce::Colour (230, 230, 0)   },  // 黄
        { 0.92f, juce::Colour (240, 80,  0)   },  // 橙
        { 1.00f, juce::Colour (240, 10,  10)  },  // 红
    };
    constexpr int N = (int) (sizeof (stops) / sizeof (stops[0]));

    juce::Colour c = stops[N - 1].c;
    for (int i = 0; i < N - 1; ++i)
    {
        if (t <= stops[i + 1].t)
        {
            const float span = juce::jmax (1.0e-6f, stops[i + 1].t - stops[i].t);
            const float u    = juce::jlimit (0.0f, 1.0f, (t - stops[i].t) / span);
            c = stops[i].c.interpolatedWith (stops[i + 1].c, u);
            break;
        }
    }
    return c;
}

// ----------------------------------------------------------
// 主题色阶（低=深色主题色，高=亮色主题色），跟随当前颜色预设
//
//   自定义主题：
//     t=0.00 → primary 深色（左侧强调色的暗部）
//     t=1.00 → secondary   （右侧基色，满幅峰值）
//   预设主题：
//     t=0.00 → pink700（最深）
//     t=0.30 → pink500
//     t=0.60 → pink300
//     t=0.85 → pink50 （最亮）
//     t=1.00 → sel    （满幅爆点高亮）
// ----------------------------------------------------------
juce::Colour Spectrogram3DModule::themeValueToColour (float t) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    // 自定义主题：高能量（满幅）使用右侧选择的基色（secondary），
    //   低能量（无信号）直接使用左侧强调色（accent / primary），
    //   形成 accent → secondary 的渐变（不做变深，避免低能量处发脏）。
    if (PinkXP::isCustomThemeActive())
    {
        const juce::Colour low  = PinkXP::getCustomPrimary();
        const juce::Colour high = PinkXP::getCustomSecondary();
        return low.interpolatedWith (high, t);
    }

    struct Stop { float t; juce::Colour c; };
    const Stop stops[] = {
        { 0.00f, PinkXP::pink700 },
        { 0.30f, PinkXP::pink500 },
        { 0.60f, PinkXP::pink300 },
        { 0.85f, PinkXP::pink50  },
        { 1.00f, PinkXP::sel     },
    };
    constexpr int N = (int) (sizeof (stops) / sizeof (stops[0]));

    juce::Colour c = stops[N - 1].c;
    for (int i = 0; i < N - 1; ++i)
    {
        if (t <= stops[i + 1].t)
        {
            const float span = juce::jmax (1.0e-6f, stops[i + 1].t - stops[i].t);
            const float u    = juce::jlimit (0.0f, 1.0f, (t - stops[i].t) / span);
            c = stops[i].c.interpolatedWith (stops[i + 1].c, u);
            break;
        }
    }
    return c;
}

// ----------------------------------------------------------
// onFrame
// ----------------------------------------------------------
void Spectrogram3DModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto plot   = canvas.reduced (2);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    // ---- 速度解耦（先判断是否需要推新列，跳过奇数 tick / nCols==0 帧的无用频谱重采样）----
    const double now = juce::Time::getMillisecondCounterHiRes();
    float dtMs = 0.0f;
    if (lastFrameMs > 0.0)
        dtMs = (float) (now - lastFrameMs);
    lastFrameMs = now;
    dtMs = juce::jlimit (0.0f, 500.0f, dtMs);

    columnAccumulator += dtMs * pixelsPerSecond * 0.001f;

    // v2.9.2：滚动更新按分发 tick 隔帧降频。列累计按 dt 连续进行（跳过期间
    //   nCols 自动翻倍），滚动速度不变；离屏图重建成本减半。用 tick 而非
    //   33ms 门控 —— 低帧率（GL 26fps 时帧间隔 38ms）下 33ms 门控会失效。
    if ((frame.tickCount & 1ull) != 0)
        return;

    int nCols = (int) columnAccumulator;
    if (historyLen > 0 && nCols > historyLen) nCols = historyLen;
    if (nCols < 0) nCols = 0;
    columnAccumulator -= (float) nCols;

    if (nCols == 0)
        return;

    // 按当前 freqRange / fftScale / shaper 获取线性幅度（仅在确需推列时执行）
    const double sr      = hub.getSampleRate();
    const double nyquist = (sr > 0.0) ? sr * 0.5 : 24000.0;
    if (std::abs (sr - tiltSampleRate_) > 1.0)
        rebuildBinTiltGain();

    const float fMin = activeMinFreqHz();
    const float fMax = juce::jmin (activeMaxFreqHz(), (float) (nyquist * 0.95));
    if (fMax <= fMin) return;

    if (shaperMode == 0)
    {
        hub.getSpectrumMagnitudesBlended (rowMagBuf, numBins, fMin, fMax, false, fftScale);
    }
    else if (shaperMode == 1)
    {
        hub.getSpectrumReassignedBlended (rowMagBuf, numBins, fMin, fMax, fftScale);
        hub.getSpectrumMagnitudesBlended (classicMagBuf, numBins, fMin, fMax, false, fftScale);
        constexpr float kClassicFloorScale = 0.35481339f; // -9 dB
        for (int i = 0; i < numBins && i < rowMagBuf.size() && i < classicMagBuf.size(); ++i)
            rowMagBuf.setUnchecked (i, juce::jmax (std::abs (rowMagBuf.getUnchecked (i)),
                                                   std::abs (classicMagBuf.getUnchecked (i)) * kClassicFloorScale));
    }
    else
    {
        hub.getSpectrumReassignedBlended (rowMagBuf, numBins, fMin, fMax, fftScale);
    }

    // 连续 push nCols 帧（都是当前幅度快照）
    for (int k = 0; k < nCols; ++k)
        pushFrame (rowMagBuf);
    imageCacheDirty = true;

    // v2.9.3：更新 GL 直渲染快照（UI 线程，推列后数据已变化）
    buildGlSnapshot();

    // GL 已接管且无悬停标尺时直接唤醒宿主 GL 线程，免除软件 CachedImage 重绘
    if (hoverActive || ! requestGlFrameOnly())
        repaintFrameContent();
}

// ----------------------------------------------------------
// buildGlSnapshot —— GL 直渲染快照（v2.9.3 引入，v2.9.4 迁移到统一框架）
//   与 renderToImage 共用同一套投影/色板逻辑；顶点为模块本地坐标，
//   Editor 侧负责模块屏幕偏移 + DPI + Y-flip 变换。
// ----------------------------------------------------------
void Spectrogram3DModule::buildGlSnapshot()
{
    // 浮动态无 GL 渲染目标，跳过快照构建（v2.9.5：拖动浮动窗口 resize 时
    //   若仍每帧全量重建 ~2ms，消息线程被打满 → 交互卡死）
    if (! wantsGlSnapshot())
        return;

    // v2.9.3 诊断开关：环境变量 Y2K_NO_S3D_GL 非空时禁用 GL 直渲染（两端），
    //   用于二分定位启动崩溃（旧软件渲染路径不受影响）。
    static const bool glDisabled = juce::SystemStats::getEnvironmentVariable ("Y2K_NO_S3D_GL", "").isNotEmpty();
    if (glDisabled)
        return;

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto plot   = canvas.reduced (2);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    recomputeProjection (plot.getWidth(), plot.getHeight());
    const auto curAppGen = PinkXP::getAppearanceGeneration();
    if (curAppGen != paletteAppearanceGen_)
    {
        depthPalettesDirty    = true;
        paletteAppearanceGen_ = curAppGen;
    }

    const int effRows = juce::jmin (historyLen, juce::jmin (juce::jmax (1, frameCount), visibleRows));
    if (depthPalettesDirty || depthPalettesRows != effRows)
        rebuildDepthPalettes (effRows);

    const auto& tokens    = PinkXP::getSkinTokens();
    const bool  darkScene = (! useThemeColours()) || (PinkXP::content.getPerceivedBrightness() <= 0.55f);
    const auto  ridge     = resolveRidgeStyle (tokens, showRidges, darkScene);

    auto snap = beginGlSnapshot();
    // v2.9.4：逻辑 FBO 渲染；3D 曲面与山脊始终保持平滑抗锯齿（snapToPixels = false）
    snap->snapToPixels    = false;
    snap->renderToLogical = true;
    snap->logicalW        = canvas.getWidth();
    snap->logicalH        = canvas.getHeight();
    snap->canvasLocal     = canvas;
    snap->fxRect          = canvas;

    const int  strips        = numBins - 1;
    const bool drawRidges    = (ridge.baseAlpha > 0.001f);
    const int  maxLayerVerts = strips * 6 + (drawRidges ? strips * 6 : 0);

    // 单一三角形顶点流布局：[令牌驱动凹槽背景][层0 条带(+山脊丝带)]...
    //   层序 = 旧→新（新层后画覆盖旧层，3D 前后层次正确），全三角化实现 Windows / macOS Core Profile 3.2
    //   一致的 ~1.25px 抗锯齿等高线山脊，并合并为单次 GL draw call。
    snap->vertices.reserve ((size_t) (96 + effRows * maxLayerVerts) * 6);
    appendWellFrame (snap->vertices, canvas, PinkXP::content);

    const float inv255 = 1.0f / 255.0f;
    const float ox = (float) plot.getX();
    const float oy = (float) plot.getY();
    const float halfW = 0.5f * ridge.cpuWidth;
    const float rr = ridge.colour.getFloatRed();
    const float rg = ridge.colour.getFloatGreen();
    const float rb = ridge.colour.getFloatBlue();
    const int   ridgeFloorIdx = tokens.isChromaKey ? 15 : 2;
    const float ridgeRampInv  = tokens.isChromaKey ? (1.0f / 14.0f) : (1.0f / 10.0f);

    // v2.9.3：按"旧层→新层"顺序构建顶点流。GPU 按提交顺序光栅化，
    //   与旧软件路径（renderToImage 的 d 从 effRows-1 递减到 0）一致 ——
    //   新层后画、覆盖旧层，3D 前后层次正确（旧层不会盖住新层）。
    for (int seg = 0; seg < effRows; ++seg)
    {
        const int d = effRows - 1 - seg;   // d 仍是深度索引（色板/投影按 d 取），仅提交顺序按 seg
        const int readIdx = (writeIdx - 1 - d + historyLen * 2) % historyLen;
        const auto& row = historyRing[(size_t) readIdx];

        const float baseY  = oy + projOriginY - (float) d * projSlantY;
        const float depthX = (float) d * projSlantX;

        // 该层颜色与几何：mag * binTiltGain → magToIdx → depthPalettes
        std::array<float, 256 * 4> rgba;
        std::array<float, 256>     binX, binTopY;
        std::array<int,   256>     binIdx;
        jassert (numBins <= 256);
        for (int i = 0; i < numBins; ++i)
        {
            const float gain = (i < (int) binTiltGain_.size()) ? binTiltGain_[(size_t) i] : 1.0f;
            const float mag  = juce::jlimit (0.0f, 1.0f, row[(size_t) i] * gain);
            const int   mi   = juce::jlimit (0, 4095, (int) (mag * 4095.0f));
            const int   idx  = (int) magToIdx[(size_t) mi];
            binIdx[(size_t) i]  = idx;
            binX[(size_t) i]    = ox + projOriginX + depthX + (float) i * projBinWidth;
            binTopY[(size_t) i] = baseY - (float) idx * inv255 * projMaxH;
            const juce::Colour c = depthPalettes[(size_t) d][(size_t) idx];
            rgba[(size_t) i * 4 + 0] = c.getFloatRed();
            rgba[(size_t) i * 4 + 1] = c.getFloatGreen();
            rgba[(size_t) i * 4 + 2] = c.getFloatBlue();
            rgba[(size_t) i * 4 + 3] = c.getFloatAlpha();
        }

        // 条带三角形（GL_TRIANGLES，6 顶点/条带，颜色与顶部 y 取自该列幅度）
        //   —— 同层内先画条带，山脊丝带随后（与旧软件路径严格同序）
        for (int i = 0; i < strips; ++i)
        {
            const float a = rgba[(size_t) i * 4 + 3];
            if (a <= 0.001f)
                continue;

            const float x0  = binX[(size_t) i];
            const float x1  = x0 + projBinWidth;
            const float top = binTopY[(size_t) i];
            const float yb  = baseY;

            const float r = rgba[(size_t) i * 4 + 0];
            const float g = rgba[(size_t) i * 4 + 1];
            const float b = rgba[(size_t) i * 4 + 2];

            const float q[36] = {
                x0, top, r, g, b, a,  x1, top, r, g, b, a,  x0, yb, r, g, b, a,
                x1, top, r, g, b, a,  x1, yb,  r, g, b, a,  x0, yb, r, g, b, a,
            };
            snap->vertices.insert (snap->vertices.end(), q, q + 36);
        }

        // 山脊等高线丝带（GL_TRIANGLES 限幅斜接带，~1.25px 抗锯齿，静音底保持 alpha=0）
        if (! drawRidges)
            continue;

        const float depthFrac    = (effRows > 1) ? (float) d / (float) (effRows - 1) : 0.0f;
        const float outlineAlpha = ridge.baseAlpha * (1.0f - 0.70f * depthFrac);

        std::array<float, 256> offX, offY, elevA;
        for (int i = 0; i < numBins; ++i)
        {
            const int idx = binIdx[(size_t) i];
            elevA[(size_t) i] = (idx <= ridgeFloorIdx)
                                    ? 0.0f
                                    : juce::jlimit (0.0f, 1.0f, (float) (idx - ridgeFloorIdx) * ridgeRampInv);
        }

        for (int i = 0; i < numBins; ++i)
        {
            float n0x = 0.0f, n0y = 1.0f;
            float n1x = 0.0f, n1y = 1.0f;
            bool has0 = false, has1 = false;
            if (i > 0)
            {
                const float dx = binX[(size_t) i]    - binX[(size_t) i - 1];
                const float dy = binTopY[(size_t) i] - binTopY[(size_t) i - 1];
                const float l  = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-5f) { n0x = -dy / l; n0y = dx / l; has0 = true; }
            }
            if (i + 1 < numBins)
            {
                const float dx = binX[(size_t) i + 1]    - binX[(size_t) i];
                const float dy = binTopY[(size_t) i + 1] - binTopY[(size_t) i];
                const float l  = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-5f) { n1x = -dy / l; n1y = dx / l; has1 = true; }
            }

            float ax = 0.0f, ay = 1.0f;
            float miterScale = 1.0f;
            if (has0 && has1)
            {
                ax = n0x + n1x;
                ay = n0y + n1y;
                const float l = std::sqrt (ax * ax + ay * ay);
                if (l > 1.0e-5f)
                {
                    ax /= l;
                    ay /= l;
                    const float dot = n0x * ax + n0y * ay;
                    miterScale = 1.0f / juce::jmax (0.65f, dot);
                }
                else
                {
                    ax = n0x;
                    ay = n0y;
                }
            }
            else if (has0) { ax = n0x; ay = n0y; }
            else if (has1) { ax = n1x; ay = n1y; }

            offX[(size_t) i] = ax * halfW * miterScale;
            offY[(size_t) i] = ay * halfW * miterScale;
        }

        for (int i = 0; i < strips; ++i)
        {
            const float a0 = outlineAlpha * elevA[(size_t) i];
            const float a1 = outlineAlpha * elevA[(size_t) i + 1];
            if (a0 <= 0.001f && a1 <= 0.001f)
                continue;

            const float p0x = binX[(size_t) i]        + offX[(size_t) i];
            const float p0y = binTopY[(size_t) i]     + offY[(size_t) i];
            const float p1x = binX[(size_t) i + 1]    + offX[(size_t) i + 1];
            const float p1y = binTopY[(size_t) i + 1] + offY[(size_t) i + 1];
            const float p2x = binX[(size_t) i]        - offX[(size_t) i];
            const float p2y = binTopY[(size_t) i]     - offY[(size_t) i];
            const float p3x = binX[(size_t) i + 1]    - offX[(size_t) i + 1];
            const float p3y = binTopY[(size_t) i + 1] - offY[(size_t) i + 1];

            const float q[36] = {
                p0x, p0y, rr, rg, rb, a0,  p1x, p1y, rr, rg, rb, a1,  p2x, p2y, rr, rg, rb, a0,
                p1x, p1y, rr, rg, rb, a1,  p3x, p3y, rr, rg, rb, a1,  p2x, p2y, rr, rg, rb, a0,
            };
            snap->vertices.insert (snap->vertices.end(), q, q + 36);
        }
    }

    const int totalVerts = (int) (snap->vertices.size() / 6);
    if (totalVerts > 0)
        snap->segments.push_back ({ 0, totalVerts, Primitive::Triangles });

    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// GlDirectModuleBase 钩子（v2.9.4 迁移自 v2.9.3 直渲染样板）
// ----------------------------------------------------------
juce::Rectangle<int> Spectrogram3DModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void Spectrogram3DModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 轴标签 / 悬停标尺画在组件缓存，合成后位于 GL 内容之上（层级正确）
    drawAxisLabels (g, canvas.reduced (2));
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

// 旧软件路径（未接管时）：离屏 Image 缓存版（P2+P4）
//
//   P2：将 3D 视图渲染到离屏 juce::Image，paintContent 只需一次 drawImage。
//   P4：canvas 对角线超过 900px 时降分辨率渲染，像素风 UI 对此高度宽容。
void Spectrogram3DModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto canvas = getCanvasBounds (contentBounds);
    const auto plot   = canvas.reduced (2);

    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaHidden = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);
    if (! (chromaHidden && PinkXP::isTransparentBackdrop()))
    {
        g.setColour (chromaHidden
                         ? PinkXP::chromaKeyGreen()
                         : (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                                ? PinkXP::btnFace
                                : PinkXP::face));
        g.fillRect (contentBounds);
    }

    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    const int effLen = juce::jmax (1, frameCount);
    if (effLen <= 0) return;

    const auto  curAppGen   = PinkXP::getAppearanceGeneration();
    const auto  curFontGen  = PinkXP::getFontGeneration();

    if (imageCacheDirty || cached3DImage.isNull()
        || cachedCanvasW != canvas.getWidth()
        || cachedCanvasH != canvas.getHeight()
        || cachedAppearanceGen_ != curAppGen
        || cachedFontGen_ != curFontGen)
    {
        if (cachedAppearanceGen_ != curAppGen)
            depthPalettesDirty = true;
        renderToImage (canvas);
        imageCacheDirty      = false;
        cachedAppearanceGen_ = curAppGen;
        cachedFontGen_       = curFontGen;
    }

    // P4: 将降分辨率 Image 放大到 canvas 尺寸（单次 drawImage 完成上采样 blit）
    g.setOpacity (1.0f);
    g.drawImage (cached3DImage,
                 canvas.getX(), canvas.getY(), canvas.getWidth(), canvas.getHeight(),
                 0, 0, cached3DImage.getWidth(), cached3DImage.getHeight());

    // 画布光学质感覆层（与 GL s3dSkinFxShader 对齐：CRT 扫描线+暗角 / Glass 镜面 / Neon 内沿辉光）
    PinkXP::Craft::drawCanvasFinishOverlay (g, canvas);

    // 轴标签浮绘在 3D 视图上方（不进入 Image，保证文字清晰度）
    drawAxisLabels (g, plot);

    // 鼠标悬停标尺（实时绘制在缓存之上）
    if (hoverActive)
        drawHoverRuler(g, canvas);
}

void Spectrogram3DModule::mouseMove(const juce::MouseEvent& e)
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

void Spectrogram3DModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在画布内，擦除只需失效画布（不走浮动态限流）
        repaint (getCanvasBounds (getContentBounds()));
    }
}

void Spectrogram3DModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) activeMinFreqHz();
    const double fMax       = juce::jmin ((double) activeMaxFreqHz(), nyquist * 0.95);

    // X → 频率（按当前 fftScale 换算）
    const float freqAxisW = projBinWidth * (float) (numBins - 1);
    const float t = juce::jlimit (0.0f, 1.0f,
        (float) (hoverPos.x - canvas.getX() - projOriginX) / juce::jmax (1.0f, freqAxisW));
    const float freqHz = (float) y2k::fscale::fromUnit (fftScale, (double) t, fMin, fMax);

    juce::String readout = PinkXP::formatFreqHz(freqHz);
    const juce::String note = PinkXP::noteNameForHz(freqHz);
    if (note.isNotEmpty())
        readout += "  " + note;
    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
}

// ----------------------------------------------------------
// buildMagLut —— 预计算 4096 级 mag → 色板下标
//
//   每帧原本需要 128 bins × visibleRows 次 gainToDecibels (log10)
//   + jlimit + lround，共 ~19K 次昂贵浮点运算。改为查表后仅剩
//   一次整数索引，在 Apple Silicon / x86 上均快到可忽略。
// ----------------------------------------------------------
void Spectrogram3DModule::buildMagLut()
{
    const float span = juce::jmax (1.0f, ceilDb - floorDb);
    for (int i = 0; i < 4096; ++i)
    {
        const float mag = (float) i * (1.0f / 4095.0f);
        const float db  = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));
        const float t01 = juce::jlimit (0.0f, 1.0f, (db - floorDb) / span);
        magToIdx[(size_t) i] = (uint8_t) juce::jlimit (0, 255, (int) std::lround (t01 * 255.0f));
    }
}

// ----------------------------------------------------------
// rebuildDepthPalettes —— 预计算逐层深度 fade 色板
//
//   基准色板（256 级）已由 valueToColour 生成；本函数为每层
//   叠加深度 fade 后存为 depthPalettes[d][256]。
//   仅在 effRows 变化（投影 recalc 引起）时调用一次，
//   之后每帧直接 depthPalettes[d][colIdx] 取值即可。
// ----------------------------------------------------------
void Spectrogram3DModule::rebuildDepthPalettes (int nRows)
{
    nRows = juce::jmax (1, juce::jmin (nRows, visibleRows));
    if (nRows == depthPalettesRows && ! depthPalettesDirty)
        return;

    // v2.9.3 修复：depthPalettes 的 resize 在构造函数末尾才执行，但构造中
    //   addAndMakeVisible 会提前触发 layoutContent → updateGlSnapshot →
    //   本函数 —— 此前对空 vector 越界写（[d][i]），导致启动堆损坏崩溃。
    //   这里显式保证容量，任何调用时机都安全。
    if ((int) depthPalettes.size() < nRows)
        depthPalettes.resize ((size_t) nRows);

    // 深度 fade 目标：主题模式向画布底色融合，热力图模式向深蓝黑融合
    //   · 绿幕：远处层与低能量条带融进纯绿（抠像后自然淡出）
    //   · 透明：保持山脊纯色 RGB，仅衰减 alpha 至 0（零绿边、零黑雾）
    const bool greenBackdrop       = PinkXP::isGreenScreenBackdrop();
    const bool transparentBackdrop = PinkXP::isTransparentBackdrop();
    const bool chromaKey           = PinkXP::isChromaKeySkin();
    const juce::Colour fadeTarget  = greenBackdrop ? PinkXP::chromaKeyGreen()
                                   : useThemeColours() ? PinkXP::content : juce::Colour (8, 8, 24);
    // 先算基准色板（无深度 fade）
    std::array<juce::Colour, 256> basePalette;
    for (int i = 0; i < 256; ++i)
    {
        const float t = (float) i * (1.0f / 255.0f);
        basePalette[(size_t) i] = chromaKey ? PinkXP::chromaKeyFade (valueToColour (t), t)
                                            : valueToColour (t);
    }

    const float maxFade = juce::jlimit (0.0f, 1.0f, depthFadePct * 0.01f);
    for (int d = 0; d < nRows; ++d)
    {
        const float depthFade = (nRows > 1) ? (float) d / (float) (nRows - 1) : 0.0f;
        const float fadeAmt   = depthFade * maxFade;
        for (int i = 0; i < 256; ++i)
            depthPalettes[(size_t) d][(size_t) i] = transparentBackdrop
                ? basePalette[(size_t) i].withMultipliedAlpha (1.0f - fadeAmt)
                : basePalette[(size_t) i].interpolatedWith (fadeTarget, fadeAmt);
    }

    depthPalettesRows  = nRows;
    depthPalettesDirty = false;
}

// ----------------------------------------------------------
// renderToImage —— 将完整 3D 视图渲染到离屏 Image（P3+P4 优化版）
//
//   P3 优化（已内化）：
//     1) magToIdx LUT     → 消除 19,200 次 gainToDecibels/log10
//     2) depthPalettes    → 消除 19,050 次 interpolatedWith
//     3) Image 复用       → 不再每帧 malloc
//
//   P4 动态分辨率：
//     canvas 对角线 ≤ 900px → 1:1 渲染（零质量损失）
//     超出则 scale = 900/diag，下限 35%，大幅降低大窗口 fillRect 像素量
//     所有坐标通过 AffineTransform::scale 自动缩放到实际 Image，代码无需改动
// ----------------------------------------------------------
void Spectrogram3DModule::renderToImage (juce::Rectangle<int> canvas)
{
    const int cw = canvas.getWidth();
    const int ch = canvas.getHeight();
    if (cw <= 0 || ch <= 0) return;

    const int effLen = juce::jmax (1, frameCount);
    if (effLen <= 0) return;

    // P4: 动态渲染分辨率 —— 对角线 ≤ 900px 时 1:1，超出反比降采样
    const float diag  = std::sqrt ((float) (cw * cw + ch * ch));
    const float scale = juce::jlimit (0.35f, 1.0f, 900.0f / juce::jmax (900.0f, diag));
    cachedRenderScale = scale;
    cachedCanvasW     = cw;
    cachedCanvasH     = ch;

    const int rw = juce::jmax (8, (int) ((float) cw * scale));
    const int rh = juce::jmax (8, (int) ((float) ch * scale));

    // 回退绘制路径恒使用 SoftwareImageType：
    //   · 正常运行时 Spectrogram3D 在停靠态与脱离态均走 GlDirectRenderer GPU 直绘，
    //     本函数仅在 GL 不可用或跨上下文迁移的首帧过渡期触发；
    //   · 严禁使用 OpenGLImageType（OpenGLFrameBufferImage 内部持有原 OpenGLContext 的裸引用，
    //     脱离窗口销毁后再次替换/析构 cached3DImage 会解引用已释放上下文，并在 Editor 活跃
    //     上下文里误删同名 FBO/纹理句柄，引发偶发闪退与画面损坏）。
    glImageContext = nullptr;
    if (cached3DImage.isNull()
        || cachedGlImage
        || cached3DImage.getWidth()  != rw
        || cached3DImage.getHeight() != rh)
    {
        cached3DImage = juce::Image (juce::Image::ARGB, rw, rh, true, juce::SoftwareImageType());
        cachedGlImage = false;
    }

    const bool transparentBackdrop = PinkXP::isTransparentBackdrop();
    if (transparentBackdrop)
        cached3DImage.clear (cached3DImage.getBounds(), juce::Colours::transparentBlack);

    juce::Graphics ig (cached3DImage);
    // P4: 缩放变换 —— 后续所有 fillRect/strokePath 用全尺寸 cw×ch 坐标，
    //     自动等比缩放到 rw×rh 的实际 Image 像素
    ig.addTransform (juce::AffineTransform::scale (scale));

    // 底衬（Image-local 坐标 → 由变换转为 scaled 坐标）
    //   · 透明模式已清空为纯 alpha=0，不铺不透明底色
    //   · 正常/绿幕先整图铺底（FBO Image 无初始内容且避免每帧 BitmapData clear 的昂贵路径）
    juce::Rectangle<int> imgCanvas (0, 0, cw, ch);
    if (! transparentBackdrop)
    {
        ig.setColour (PinkXP::isGreenScreenBackdrop() ? PinkXP::chromaKeyGreen() : PinkXP::content);
        ig.fillRect (imgCanvas.toFloat());
        drawBackground (ig, imgCanvas);
    }

    const auto plot = imgCanvas.reduced (2);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    recomputeProjection (plot.getWidth(), plot.getHeight());

    const int effRows = juce::jmin (historyLen, juce::jmin (effLen, visibleRows));

    const auto curAppGen = PinkXP::getAppearanceGeneration();
    if (curAppGen != paletteAppearanceGen_)
    {
        depthPalettesDirty    = true;
        paletteAppearanceGen_ = curAppGen;
    }

    // 深度色板只在投影/行数/外观代次变化时重建
    if (depthPalettesDirty || depthPalettesRows != effRows)
        rebuildDepthPalettes (effRows);

    const auto& tokens    = PinkXP::getSkinTokens();
    const bool  darkScene = (! useThemeColours()) || (PinkXP::content.getPerceivedBrightness() <= 0.55f);
    const auto  ridge     = resolveRidgeStyle (tokens, showRidges, darkScene);

    const float ox = (float) plot.getX();
    const float oy = (float) plot.getY();

    juce::Graphics::ScopedSaveState ss(ig);
    ig.reduceClipRegion(plot);

    // Path 对象复用：clear() 替代每层重建，避免 VTune 报告的 HeapAlloc 热点
    juce::Path outline;
    const juce::PathStrokeType outlineStroke (juce::jmax (0.5f, ridge.cpuWidth),
                                              juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded);

    // v2.9.2：恢复旧版单遍全高绘制。上一版"楔形裁剪"虽减少像素量，但轮廓线
    //   超出楔形部分不再被后续层覆盖，露出逐列竖线（立方体 + 分界线观感），
    //   被确认视觉回归。性能改用 onFrame 的 30Hz 滚动更新降频换取。
    for (int d = effRows - 1; d >= 0; --d)
    {
        const int readIdx = (writeIdx - 1 - d + historyLen * 2) % historyLen;
        const auto& row = historyRing[(size_t) readIdx];

        const float baseY  = oy + projOriginY - (float) d * projSlantY;
        const float depthX = (float) d * projSlantX;

        std::array<float, 256> binX, binTopY;
        std::array<int,   256> colIdx;
        jassert (numBins <= 256);
        for (int i = 0; i < numBins; ++i)
        {
            const float gain = (i < (int) binTiltGain_.size()) ? binTiltGain_[(size_t) i] : 1.0f;
            const float mag  = juce::jlimit (0.0f, 1.0f, row[(size_t) i] * gain);
            const int   mi   = juce::jlimit (0, 4095, (int) (mag * 4095.0f));
            const int   idx  = (int) magToIdx[(size_t) mi];

            binX[(size_t) i]    = ox + projOriginX + depthX + (float) i * projBinWidth;
            binTopY[(size_t) i] = baseY - (float) idx * (1.0f / 255.0f) * projMaxH;
            colIdx[(size_t) i]  = idx;
        }

        for (int i = 0; i < numBins - 1; ++i)
        {
            // P3: 深度色板直接查表，替代 interpolatedWith（透明模式跳过 alpha=0 静音底列）
            const auto col = depthPalettes[(size_t) d][(size_t) colIdx[(size_t) i]];
            if (col.getAlpha() == 0)
                continue;
            ig.setColour (col);
            const float top = binTopY[(size_t) i];
            const float h   = juce::jmax (0.5f, baseY - top);
            ig.fillRect (binX[(size_t) i], top, projBinWidth, h);
        }

        // 顶部等高线山脊轮廓：与 GL 路径 1:1 对齐（跳过静音底，平滑抗锯齿）
        if (ridge.baseAlpha <= 0.001f)
            continue;

        const int ridgeFloorIdx = tokens.isChromaKey ? 15 : 2;
        outline.clear();
        bool inRidge = false;
        for (int i = 0; i < numBins - 1; ++i)
        {
            const bool elevated = (colIdx[(size_t) i] > ridgeFloorIdx) || (colIdx[(size_t) i + 1] > ridgeFloorIdx);
            if (elevated)
            {
                if (! inRidge)
                {
                    outline.startNewSubPath (binX[(size_t) i], binTopY[(size_t) i]);
                    inRidge = true;
                }
                outline.lineTo (binX[(size_t) i + 1], binTopY[(size_t) i + 1]);
            }
            else
            {
                inRidge = false;
            }
        }

        if (! outline.isEmpty())
        {
            const float depthFrac  = (effRows > 1) ? (float) d / (float) (effRows - 1) : 0.0f;
            const float layerAlpha = ridge.baseAlpha * (1.0f - 0.70f * depthFrac);
            ig.setColour (ridge.colour.withAlpha (layerAlpha));
            ig.strokePath (outline, outlineStroke);
        }
    }
}

// ----------------------------------------------------------
// 绘制底衬
// ----------------------------------------------------------
void Spectrogram3DModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
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
// 轴标签 —— 频率标签在底部，older/newer 提示在顶部/右下（耦合 Craft::drawBadgePill）
// ----------------------------------------------------------
void Spectrogram3DModule::drawAxisLabels (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    if (PinkXP::isChromaKeySkin() || ! showLabels)
        return;   // 绿幕或关闭刻度标签：不画

    const auto& tokens    = PinkXP::getSkinTokens();
    const bool  darkScene = (! useThemeColours()) || (PinkXP::content.getPerceivedBrightness() <= 0.55f);
    const bool  isNeon    = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool  isCrt     = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool  isGlass   = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);

    juce::Colour badgeFill;
    juce::Colour badgeBorder;
    if (isNeon)
    {
        badgeFill   = PinkXP::content.interpolatedWith (juce::Colours::black, 0.52f).withAlpha (0.90f);
        badgeBorder = PinkXP::sel.withAlpha (0.86f);
    }
    else if (isCrt)
    {
        badgeFill   = PinkXP::content.interpolatedWith (juce::Colours::black, 0.45f).withAlpha (0.92f);
        badgeBorder = PinkXP::sel.withAlpha (0.86f);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel)
    {
        badgeFill   = darkScene ? juce::Colour (0xd80c0a14) : PinkXP::btnFace.withAlpha (0.92f);
        badgeBorder = darkScene ? PinkXP::pink300.withAlpha (0.78f) : PinkXP::dark.withAlpha (0.85f);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
    {
        badgeFill   = darkScene
                          ? PinkXP::face.interpolatedWith (juce::Colour (0xff1a1d24), 0.52f).withAlpha (0.90f)
                          : PinkXP::face.interpolatedWith (PinkXP::hl, 0.28f).withAlpha (0.92f);
        badgeBorder = darkScene
                          ? PinkXP::hl.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.36f)
                          : PinkXP::dark.withAlpha (0.48f);
    }
    else if (isGlass)
    {
        badgeFill   = darkScene
                          ? juce::Colour (0xb2161c28)
                          : PinkXP::face.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.82f);
        badgeBorder = darkScene
                          ? juce::Colours::white.withAlpha (0.30f)
                          : PinkXP::dark.withAlpha (0.24f);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
    {
        badgeFill   = darkScene ? juce::Colour (0xb8101218)
                                : PinkXP::content.interpolatedWith (PinkXP::face, 0.48f).withAlpha (0.88f);
        badgeBorder = darkScene ? juce::Colours::white.withAlpha (0.16f)
                                : PinkXP::skinHairlineOn (PinkXP::face, 0.22f);
    }
    else
    {
        badgeFill   = darkScene ? juce::Colour (0xc8101018) : PinkXP::face.withAlpha (0.86f);
        badgeBorder = darkScene ? juce::Colours::white.withAlpha (0.24f) : PinkXP::dark.withAlpha (0.30f);
    }
    const juce::Colour badgeText = PinkXP::readableInkOn (badgeFill.withAlpha (1.0f));
    const float labelPt = isNeon ? 7.6f : (isCrt ? 8.5f : 9.0f);
    const float dirPt   = isNeon ? 7.8f : 8.5f;

    g.setFont (PinkXP::getAxisFont (labelPt, juce::Font::plain));

    const int ow = (isCrt || isNeon) ? 68 : 60, oh = 13;
    const juce::Rectangle<int> topBadge (canvas.getX() + 2,          canvas.getY() + 2,          ow, oh);
    const juce::Rectangle<int> botBadge (canvas.getRight() - ow - 2, canvas.getBottom() - oh - 2, ow, oh);

    // 频率标签（按 freqRange 自动筛可见刻度，并避让右下 newer 徽章）
    struct L { float hz; const char* label; };
    const L labels[] = {
        {    50.0f,  "50" },
        {   100.0f, "100" },
        {   250.0f, "250" },
        {   500.0f, "500" },
        {  1000.0f,  "1k" },
        {  4000.0f,  "4k" },
        { 10000.0f, "10k" },
    };

    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) activeMinFreqHz();
    const double fMax       = juce::jmin ((double) activeMaxFreqHz(), nyquist * 0.95);
    int lastDrawnRight = -9999;

    const int minBw = (isCrt || isNeon) ? 32 : 26;
    const int bh    = 13;
    for (auto& l : labels)
    {
        if ((double) l.hz < fMin || (double) l.hz > fMax) continue;

        const double t = y2k::fscale::toUnit (fftScale, (double) l.hz, fMin, fMax);

        const float freqAxisW = projBinWidth * (float) (numBins - 1);
        const int x = canvas.getX() + (int) std::round ((double) projOriginX + t * (double) freqAxisW);

        const auto img = PinkXP::cachedStaticText (
            g, l.label, labelPt, juce::Font::plain, badgeText,
            juce::Colours::transparentBlack, true);
        const int bw = juce::jlimit (minBw, 44, img.isValid() ? (img.getWidth() + 8) : minBw);
        const int bx = juce::jlimit (canvas.getX() + 1, juce::jmax (canvas.getX() + 1, canvas.getRight() - bw - 1), x - bw / 2);
        const int by = juce::jmax (canvas.getY() + 1, canvas.getBottom() - bh - 2);
        if (bx < lastDrawnRight + 4) continue;
        if (bx + bw > botBadge.getX() - 3)
            continue;
        lastDrawnRight = bx + bw;

        const juce::Rectangle<int> tagRect (bx, by, bw, bh);

        PinkXP::Craft::drawBadgePill (g, tagRect, badgeFill, badgeBorder);

        if (img.isValid() && img.getWidth() <= bw - 4)
        {
            g.setOpacity (1.0f);
            img.drawAt (g, (float) (bx + (bw - img.getWidth()) / 2), (float) (by + (bh - img.getHeight()) / 2));
        }
        else
        {
            g.setColour (badgeText);
            g.drawFittedText (y2k::tr (l.label), tagRect.reduced (2, 0), juce::Justification::centred, 1, 0.70f);
        }
    }

    // older/newer 提示（浮绘在左上和右下，耦合 Craft::drawBadgePill）
    {
        PinkXP::Craft::drawBadgePill (g, topBadge, badgeFill, badgeBorder);
        PinkXP::Craft::drawBadgePill (g, botBadge, badgeFill, badgeBorder);

        PinkXP::drawCachedSymbolText (g, juce::String::fromUTF8 ("\xe2\x86\x97 older"),
                                      topBadge, juce::Justification::centred, dirPt, juce::Font::plain, badgeText);
        PinkXP::drawCachedSymbolText (g, juce::String::fromUTF8 ("newer \xe2\x86\x99"),
                                      botBadge, juce::Justification::centred, dirPt, juce::Font::plain, badgeText);
    }
}