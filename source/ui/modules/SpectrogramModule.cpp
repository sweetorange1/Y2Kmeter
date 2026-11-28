#include "source/ui/modules/SpectrogramModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/palette/OkLchUtils.h"
#include <cmath>
#include <algorithm>

// ==========================================================
// SpectrogramModule
// ==========================================================

namespace
{
    inline juce::Colour sampleColourStops (const juce::Colour* stops, int count, float t) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        const float pos = t * (float) (count - 1);
        const int   i   = juce::jlimit (0, count - 2, (int) pos);
        return PinkXP::interpolateOkLab (stops[i], stops[i + 1], pos - (float) i);
    }
}

SpectrogramModule::SpectrogramModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::spectrogram), hub (h)
{
    hub.retain (AnalyserHub::Kind::Spectrum);
    hub.addFrameListener (this);

    setMinSize     (96, 80);
    setDefaultSize (384, 224);
    setTitleText   ("Spectrogram");
    setControlPanelEnabled (true);

    invDbSpan_ = 1.0f / juce::jmax (1.0f, ceilDb - floorDb);

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        lastAppearanceGen_ = PinkXP::getAppearanceGeneration();
        lastFontGen_       = PinkXP::getFontGeneration();
        refreshSpectrogramColours();
        invalidateVisualGrading (true, false);
    });

    refreshSpectrogramColours();
    rebuildPaletteLut();
    lastAppearanceGen_ = PinkXP::getAppearanceGeneration();
    lastFontGen_       = PinkXP::getFontGeneration();
}

SpectrogramModule::~SpectrogramModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Spectrum);
}

float SpectrogramModule::activeMinFreqHz() const noexcept
{
    switch (freqRange)
    {
        case FreqRange::Music: return 30.0f;
        case FreqRange::Vocal: return 80.0f;
        case FreqRange::Bass:  return 20.0f;
        case FreqRange::Full:
        default:               return 20.0f;
    }
}

float SpectrogramModule::activeMaxFreqHz() const noexcept
{
    switch (freqRange)
    {
        case FreqRange::Music: return 12000.0f;
        case FreqRange::Vocal: return  8000.0f;
        case FreqRange::Bass:  return  1000.0f;
        case FreqRange::Full:
        default:               return 20000.0f;
    }
}

void SpectrogramModule::setCellPx (int px)
{
    const int clamped = juce::jlimit (1, 8, px);
    if (cellPx == clamped) return;
    cellPx = clamped;
    lastCanvasW = lastCanvasH = 0;
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBuf = juce::Image();
    imageBufDirty_ = true;
    repaint();
}

// ----------------------------------------------------------
// 持久化 + 控制面板
//   旧键（displayMode / speed / direction / colour / fftScale）完全兼容；
//   新键缺省时保留优雅默认值。
// ----------------------------------------------------------
juce::ValueTree SpectrogramModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("displayMode", (int) displayMode, nullptr);
    s.setProperty ("speed",       (double) pixelsPerSecond, nullptr);
    s.setProperty ("direction",   (int) direction, nullptr);
    s.setProperty ("colour",      (int) colourScheme, nullptr);
    s.setProperty ("fftScale",    (int) fftScale, nullptr);
    s.setProperty ("floorDb",     (double) floorDb, nullptr);
    s.setProperty ("ceilDb",      (double) ceilDb, nullptr);
    s.setProperty ("gamma",       (double) contrastGamma, nullptr);
    s.setProperty ("slope",       (double) slopeDbPerOct, nullptr);
    s.setProperty ("timeSmooth",  (double) timeSmoothPct, nullptr);
    s.setProperty ("smooth",      smoothPixels, nullptr);
    s.setProperty ("pixelSize",   cellPx, nullptr);
    s.setProperty ("freqRange",   (int) freqRange, nullptr);
    s.setProperty ("showGrid",    showGrid, nullptr);
    s.setProperty ("showLabels",  showLabels, nullptr);
    return s;
}

void SpectrogramModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    bool gridInvalid    = false;
    bool paletteDirty   = false;
    bool tiltDirty      = false;
    bool gradingChanged = false;
    bool visualTouched  = false;

    if (state.hasProperty ("displayMode"))
    {
        const auto m = (DisplayMode) juce::jlimit (0, 2, (int) state.getProperty ("displayMode"));
        if (displayMode != m)
        {
            displayMode = m;
            std::fill (smoothColDb.begin(), smoothColDb.end(), kSilenceDb);
            visualTouched = true;
        }
    }
    if (state.hasProperty ("speed"))
        setScrollSpeed ((float) (double) state.getProperty ("speed"));
    if (state.hasProperty ("direction"))
    {
        const auto d = (Direction) juce::jlimit (0, 3, (int) state.getProperty ("direction"));
        if (direction != d) { direction = d; gridInvalid = true; }
    }
    if (state.hasProperty ("colour"))
    {
        const auto c = (ColourScheme) juce::jlimit (0, 7, (int) state.getProperty ("colour"));
        if (colourScheme != c) { colourScheme = c; paletteDirty = true; gradingChanged = true; }
    }
    if (state.hasProperty ("fftScale"))
    {
        const auto s = y2k::fscale::fromIndex ((int) state.getProperty ("fftScale"));
        if (fftScale != s) { fftScale = s; gridInvalid = true; tiltDirty = true; }
    }
    if (state.hasProperty ("floorDb"))
    {
        const float f = juce::jlimit (-110.0f, -30.0f, (float) (double) state.getProperty ("floorDb"));
        if (std::abs (floorDb - f) >= 1.0e-4f) { floorDb = f; gradingChanged = true; }
    }
    if (state.hasProperty ("ceilDb"))
    {
        const float c = juce::jlimit (-36.0f, 0.0f, (float) (double) state.getProperty ("ceilDb"));
        if (std::abs (ceilDb - c) >= 1.0e-4f) { ceilDb = c; gradingChanged = true; }
    }
    if (ceilDb < floorDb + 6.0f)
    {
        ceilDb  = juce::jlimit (-36.0f, 0.0f, floorDb + 6.0f);
        floorDb = juce::jmin (floorDb, ceilDb - 6.0f);
        gradingChanged = true;
    }
    if (state.hasProperty ("gamma"))
    {
        const float g = juce::jlimit (0.40f, 2.50f, (float) (double) state.getProperty ("gamma"));
        if (std::abs (contrastGamma - g) >= 1.0e-4f) { contrastGamma = g; paletteDirty = true; gradingChanged = true; }
    }
    if (state.hasProperty ("slope"))
    {
        const float sl = juce::jlimit (0.0f, 9.0f, (float) (double) state.getProperty ("slope"));
        if (std::abs (slopeDbPerOct - sl) >= 1.0e-4f) { slopeDbPerOct = sl; tiltDirty = true; gradingChanged = true; }
    }
    if (state.hasProperty ("timeSmooth"))
        setTimeSmooth ((float) (double) state.getProperty ("timeSmooth"));
    if (state.hasProperty ("smooth"))
    {
        const bool sm = (bool) state.getProperty ("smooth");
        if (smoothPixels != sm) { smoothPixels = sm; visualTouched = true; }
    }
    if (state.hasProperty ("pixelSize"))
    {
        const int px = juce::jlimit (1, 8, (int) state.getProperty ("pixelSize"));
        if (cellPx != px) { cellPx = px; gridInvalid = true; }
    }
    if (state.hasProperty ("freqRange"))
    {
        const auto fr = (FreqRange) juce::jlimit (0, 3, (int) state.getProperty ("freqRange"));
        if (freqRange != fr) { freqRange = fr; gridInvalid = true; tiltDirty = true; }
    }
    if (state.hasProperty ("showGrid"))
    {
        const bool sg = (bool) state.getProperty ("showGrid");
        if (showGrid != sg) { showGrid = sg; visualTouched = true; }
    }
    if (state.hasProperty ("showLabels"))
    {
        const bool sl = (bool) state.getProperty ("showLabels");
        if (showLabels != sl) { showLabels = sl; visualTouched = true; }
    }

    if (gridInvalid)
    {
        lastCanvasW = lastCanvasH = 0;
        texCacheValid_ = false;
        lastBuiltWriteCol_ = -1;
        imageBuf = juce::Image();
        imageBufDirty_ = true;
    }

    if (gradingChanged || paletteDirty || tiltDirty)
    {
        invalidateVisualGrading (paletteDirty, tiltDirty);
    }
    else if (gridInvalid || visualTouched)
    {
        buildGlSnapshot();
        repaint();
    }

    refreshControlPanel();
}

void SpectrogramModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    // ---- 色彩与动态范围 ----
    out.push_back (ParamDef::choice ("colour", "Colours",
        { "Theme", "Heat", "Grey", "Ice", "Magma", "Inferno", "Plasma", "Viridis" },
        "Theme adapts to the current skin with multi-stop glow. Magma, Inferno, Plasma and Viridis "
        "are perceptually uniform studio colormaps. Heat, Grey and Ice are classic ramps.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("floorDb", "Floor", -110.0, -30.0, 2.0, " dB",
        "Noise floor cutoff. Raise it to keep quiet background clean and dark; lower it to reveal faint reverb tails.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("ceilDb", "Ceiling", -36.0, 0.0, 2.0, " dB",
        "Level that maps to peak brightness. Lowering Ceiling makes music harmonics glow brighter and richer.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("gamma", "Contrast", 0.40, 2.50, 0.05, "",
        "Brightness curve. Higher values sharpen contrast and darken faint haze; lower values lift quiet detail.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("slope", "Tilt", 0.0, 9.0, 0.5, " dB/oct",
        "Boosts higher frequencies around a 1 kHz pivot so hi-hats and upper harmonics shine evenly with bass.")
        .in ("Colour"));

    // ---- 清晰度与画质 ----
    out.push_back (ParamDef::choice ("shaper", "Sharpening",
        { "Classic", "Sharp", "Sharper" },
        "Classic: smooth raw FFT magnitude. Sharp: frequency reassignment blended with classic body for continuous note lines. "
        "Sharper: pure frequency reassignment that collapses tones into razor-thin particle beams.")
        .in ("Image"));
    out.push_back (ParamDef::slider ("timeSmooth", "Time smooth", 0.0, 80.0, 5.0, " %",
        "Smooths rapid horizontal flicker across columns while keeping note attacks crisp. 0% is raw frame-by-frame.")
        .in ("Image"));
    out.push_back (ParamDef::toggle ("smooth", "Smooth pixels",
        "ON uses silky linear filtering for a modern HD look. OFF uses sharp nearest-neighbour pixels for a Y2K retro look.")
        .in ("Image"));
    out.push_back (ParamDef::choice ("pixelSize", "Pixel size",
        { "1 px", "2 px", "3 px", "4 px" },
        "Grid cell size. 1 px gives maximum detail; larger cells create a chunky pixel-art waterfall.")
        .in ("Image"));

    // ---- 频率轴与标尺 ----
    out.push_back (ParamDef::choice ("fftScale", "FFT scale",
        { "Log", "Mel", "Linear" },
        "How frequency is laid out along the frequency axis. Log gives every octave equal room, "
        "Mel follows pitch perception, Linear gives equal Hz per pixel.")
        .in ("Axis"));
    out.push_back (ParamDef::choice ("freqRange", "Freq range",
        { "Full", "Music", "Vocal", "Bass" },
        "Zooms the frequency view: Full (20-20k Hz), Music (30-12k Hz), Vocal (80-8k Hz), or Bass (20-1k Hz).")
        .in ("Axis"));
    out.push_back (ParamDef::toggle ("showGrid", "Grid",
        "Overlay subtle reference lines along major frequency ticks.")
        .in ("Axis"));
    out.push_back (ParamDef::toggle ("showLabels", "Labels",
        "Show frequency ticks and older/newer direction hints. Turn OFF for a clean borderless waterfall.")
        .in ("Axis"));

    // ---- 滚动 ----
    out.push_back (ParamDef::choice ("direction", "Scroll direction",
        { "Leftwards", "Rightwards", "Upwards", "Downwards" },
        "Which way the picture moves. Leftwards: new sound enters on the right (classic). "
        "Upwards and Downwards turn the picture so time runs vertically and frequency runs left to right.")
        .in ("Scrolling"));
    out.push_back (ParamDef::slider ("speed", "Speed", 5.0, 120.0, 1.0, " px/s",
        "Scroll speed in pixels per second. Slower keeps a longer history on screen.")
        .in ("Scrolling"));
}

double SpectrogramModule::getParamValue (const juce::String& id) const
{
    if (id == "direction")  return (double) (int) direction;
    if (id == "speed")      return (double) pixelsPerSecond;
    if (id == "shaper")     return (double) (int) displayMode;
    if (id == "colour")     return (double) (int) colourScheme;
    if (id == "fftScale")   return (double) (int) fftScale;
    if (id == "floorDb")    return (double) floorDb;
    if (id == "ceilDb")     return (double) ceilDb;
    if (id == "gamma")      return (double) contrastGamma;
    if (id == "slope")      return (double) slopeDbPerOct;
    if (id == "timeSmooth") return (double) timeSmoothPct;
    if (id == "smooth")     return smoothPixels ? 1.0 : 0.0;
    if (id == "pixelSize")  return (double) juce::jlimit (0, 3, cellPx - 1);
    if (id == "freqRange")  return (double) (int) freqRange;
    if (id == "showGrid")   return showGrid ? 1.0 : 0.0;
    if (id == "showLabels") return showLabels ? 1.0 : 0.0;
    return 0.0;
}

void SpectrogramModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "direction")  setDirection ((Direction) juce::jlimit (0, 3, juce::roundToInt (v)));
    else if (id == "speed")      setScrollSpeed ((float) v);
    else if (id == "shaper")     setDisplayMode ((DisplayMode) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "colour")     setColourScheme ((ColourScheme) juce::jlimit (0, 7, juce::roundToInt (v)));
    else if (id == "fftScale")   setFftScale (y2k::fscale::fromIndex (juce::roundToInt (v)));
    else if (id == "floorDb")    setFloorDb ((float) v);
    else if (id == "ceilDb")     setCeilDb ((float) v);
    else if (id == "gamma")      setContrast ((float) v);
    else if (id == "slope")      setSlopeDbPerOct ((float) v);
    else if (id == "timeSmooth") setTimeSmooth ((float) v);
    else if (id == "smooth")     setSmoothPixels (v >= 0.5);
    else if (id == "pixelSize")  setCellPx (juce::jlimit (1, 4, juce::roundToInt (v) + 1));
    else if (id == "freqRange")  setFreqRange ((FreqRange) juce::jlimit (0, 3, juce::roundToInt (v)));
    else if (id == "showGrid")   setShowGrid (v >= 0.5);
    else if (id == "showLabels") setShowLabels (v >= 0.5);
}

void SpectrogramModule::setScrollSpeed (float pxPerSec)
{
    pixelsPerSecond = juce::jlimit (5.0f, 120.0f, pxPerSec);
}

void SpectrogramModule::setDirection (Direction d)
{
    if (direction == d) return;
    direction = d;
    lastCanvasW = lastCanvasH = 0;
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBuf = juce::Image();
    imageBufDirty_ = true;
    repaint();
}

void SpectrogramModule::setFftScale (y2k::fscale::Scale s)
{
    if (fftScale == s) return;
    fftScale = s;
    lastCanvasW = lastCanvasH = 0;
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBuf = juce::Image();
    imageBufDirty_ = true;
    repaint();
}

void SpectrogramModule::setFreqRange (FreqRange r)
{
    if (freqRange == r) return;
    freqRange = r;
    lastCanvasW = lastCanvasH = 0;
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBuf = juce::Image();
    imageBufDirty_ = true;
    repaint();
}

void SpectrogramModule::setColourScheme (ColourScheme c)
{
    if (colourScheme == c) return;
    colourScheme = c;
    invalidateVisualGrading (true, false);
}

void SpectrogramModule::setFloorDb (float db)
{
    const float clamped = juce::jlimit (-110.0f, juce::jmin (-30.0f, ceilDb - 6.0f), db);
    if (std::abs (floorDb - clamped) < 1.0e-4f) return;
    floorDb = clamped;
    invalidateVisualGrading (false, false);
}

void SpectrogramModule::setCeilDb (float db)
{
    const float clamped = juce::jlimit (juce::jmax (-36.0f, floorDb + 6.0f), 0.0f, db);
    if (std::abs (ceilDb - clamped) < 1.0e-4f) return;
    ceilDb = clamped;
    invalidateVisualGrading (false, false);
}

void SpectrogramModule::setContrast (float g)
{
    const float clamped = juce::jlimit (0.40f, 2.50f, g);
    if (std::abs (contrastGamma - clamped) < 1.0e-4f) return;
    contrastGamma = clamped;
    invalidateVisualGrading (true, false);
}

void SpectrogramModule::setSlopeDbPerOct (float dbPerOct)
{
    const float clamped = juce::jlimit (0.0f, 9.0f, dbPerOct);
    if (std::abs (slopeDbPerOct - clamped) < 1.0e-4f) return;
    slopeDbPerOct = clamped;
    invalidateVisualGrading (false, true);
}

void SpectrogramModule::setTimeSmooth (float pct0To80)
{
    timeSmoothPct = juce::jlimit (0.0f, 80.0f, pct0To80);
}

void SpectrogramModule::setSmoothPixels (bool on)
{
    if (smoothPixels == on) return;
    smoothPixels = on;
    buildGlSnapshot();
    repaint();
}

void SpectrogramModule::setShowGrid (bool on)
{
    if (showGrid == on) return;
    showGrid = on;
    repaint();
}

void SpectrogramModule::setShowLabels (bool on)
{
    if (showLabels == on) return;
    showLabels = on;
    repaint();
}

void SpectrogramModule::setDisplayMode (DisplayMode m)
{
    if (displayMode == m) return;
    displayMode = m;
    std::fill (smoothColDb.begin(), smoothColDb.end(), kSilenceDb);
    repaint();
}

void SpectrogramModule::invalidateVisualGrading (bool paletteDirty, bool tiltDirty)
{
    invDbSpan_ = 1.0f / juce::jmax (1.0f, ceilDb - floorDb);
    if (paletteDirty)
        rebuildPaletteLut();
    if (tiltDirty)
        rebuildRowTiltLut();
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBufDirty_ = true;
    // GL 直绘已接管时不重绘 CPU 软件位图（脱离/回退到软绘时由 ensureImage 按需重建）
    if (! glDirectActive.load (std::memory_order_acquire) && imageBuf.isValid())
    {
        redrawFullImage();
        imageBufDirty_ = false;
    }
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> SpectrogramModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    return content;
}

// ----------------------------------------------------------
// Toybox：四方向十字声呐浮标（按住冻结时 0x 原地静止并柔淡退让）
// ----------------------------------------------------------
bool SpectrogramModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void SpectrogramModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const auto canvas = getToyCanvasBounds();
    const float spd = getToyScrollPxPerSec();
    juce::Point<float> driftVel { -spd, 0.0f };
    switch (direction)
    {
        case Direction::Left:  driftVel = { -spd,  0.0f }; break;
        case Direction::Right: driftVel = {  spd,  0.0f }; break;
        case Direction::Up:    driftVel = {  0.0f, -spd }; break;
        case Direction::Down:  driftVel = {  0.0f,  spd }; break;
    }

    juce::Component::SafePointer<SpectrogramModule> safe (this);
    auto isFrozen = [safe]() -> bool
    {
        return safe != nullptr && safe->hold.active;
    };

    ctx.addModuleActorCapped (makeSpectrogramBuoyActor (hit.pos, canvas, driftVel, std::move (isFrozen)), 6);
}

void SpectrogramModule::layoutContent (juce::Rectangle<int>)
{
}

void SpectrogramModule::resized()
{
    ModulePanel::resized();
}

// ----------------------------------------------------------
// 网格尺寸管理（存储原始 dB 值）
// ----------------------------------------------------------
void SpectrogramModule::ensureGrid (int canvasW, int canvasH)
{
    canvasW = juce::jmax (1, canvasW);
    canvasH = juce::jmax (1, canvasH);

    const int newCols = juce::jmax (1, (isVertical() ? canvasH : canvasW) / cellPx);
    const int newRows = juce::jmax (1, (isVertical() ? canvasW : canvasH) / cellPx);

    if (canvasW == lastCanvasW && canvasH == lastCanvasH
        && newRows == rows && newCols == cols
        && (int) gridData.size() == rows * cols)
        return;

    rows = newRows;
    cols = newCols;
    gridData.assign ((size_t) rows * (size_t) cols, kSilenceDb);
    colDbBuf.assign ((size_t) rows, kSilenceDb);
    smoothColDb.assign ((size_t) rows, kSilenceDb);
    writeCol = 0;

    lastCanvasW = canvasW;
    lastCanvasH = canvasH;
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBufDirty_ = true;

    rebuildRowTiltLut();
}

// ----------------------------------------------------------
// 色彩体系：8 套多段感知色阶 + LUT 预烘焙
// ----------------------------------------------------------
void SpectrogramModule::refreshSpectrogramColours()
{
    if (PinkXP::isCustomThemeActive())
    {
        spectrogramBaseColour_   = PinkXP::getCustomSecondary();
        spectrogramAccentColour_ = PinkXP::getCustomPrimary();
    }
    else
    {
        const auto& theme = PinkXP::getCurrentTheme();
        spectrogramBaseColour_   = theme.content;
        spectrogramAccentColour_ = theme.swatch;
    }
}

bool SpectrogramModule::syncAppearanceIfChanged()
{
    const auto appGen  = PinkXP::getAppearanceGeneration();
    const auto fontGen = PinkXP::getFontGeneration();
    if (appGen == lastAppearanceGen_ && fontGen == lastFontGen_)
        return false;

    lastAppearanceGen_ = appGen;
    lastFontGen_       = fontGen;
    refreshSpectrogramColours();
    rebuildPaletteLut();
    texCacheValid_     = false;
    lastBuiltWriteCol_ = -1;
    imageBufDirty_     = true;
    return true;
}

bool SpectrogramModule::isDarkPalette() const noexcept
{
    if (colourScheme != ColourScheme::Theme)
        return true;
    return spectrogramBaseColour_.getPerceivedBrightness() <= 0.55f;
}

juce::Colour SpectrogramModule::intensityToColour (float t) const noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    const auto base = [&]() noexcept -> juce::Colour
    {
        switch (colourScheme)
        {
            case ColourScheme::Heat:
            {
                const juce::Colour stops[5] = {
                    juce::Colour (0xff070308), juce::Colour (0xff5c0a24),
                    juce::Colour (0xffd4261c), juce::Colour (0xfff9a825),
                    juce::Colour (0xfffffde7)
                };
                return sampleColourStops (stops, 5, t);
            }
            case ColourScheme::Grey:
            {
                const juce::Colour stops[5] = {
                    juce::Colour (0xff060608), juce::Colour (0xff34363c),
                    juce::Colour (0xff7a7e88), juce::Colour (0xffc4c8d0),
                    juce::Colour (0xffffffff)
                };
                return sampleColourStops (stops, 5, t);
            }
            case ColourScheme::Ice:
            {
                const juce::Colour stops[5] = {
                    juce::Colour (0xff030816), juce::Colour (0xff0d2f6b),
                    juce::Colour (0xff1888c8), juce::Colour (0xff6be2f8),
                    juce::Colour (0xfff2fcff)
                };
                return sampleColourStops (stops, 5, t);
            }
            case ColourScheme::Magma:
            {
                const juce::Colour stops[5] = {
                    juce::Colour (0xff05040d), juce::Colour (0xff3b0f70),
                    juce::Colour (0xffb63679), juce::Colour (0xfffb8861),
                    juce::Colour (0xfffcfdbf)
                };
                return sampleColourStops (stops, 5, t);
            }
            case ColourScheme::Inferno:
            {
                const juce::Colour stops[5] = {
                    juce::Colour (0xff04030c), juce::Colour (0xff420a68),
                    juce::Colour (0xffbc3754), juce::Colour (0xfff98e09),
                    juce::Colour (0xfffcffa4)
                };
                return sampleColourStops (stops, 5, t);
            }
            case ColourScheme::Plasma:
            {
                const juce::Colour stops[5] = {
                    juce::Colour (0xff0d0887), juce::Colour (0xff6a00a8),
                    juce::Colour (0xffcb4679), juce::Colour (0xfff89540),
                    juce::Colour (0xfff0f921)
                };
                return sampleColourStops (stops, 5, t);
            }
            case ColourScheme::Viridis:
            {
                const juce::Colour stops[5] = {
                    juce::Colour (0xff0b0d1f), juce::Colour (0xff3b528b),
                    juce::Colour (0xff21918c), juce::Colour (0xff5ec962),
                    juce::Colour (0xfffde725)
                };
                return sampleColourStops (stops, 5, t);
            }
            case ColourScheme::Theme:
            default:
                break;
        }

        // 增强版 Theme 多段主题色阶：
        //   · 自定义主题：content → chartRamp 全色谱 → 极高能量轻度提亮至 hl
        //   · 预设主题：区分深底/浅底做 5 段感知平滑色阶，既有层次又有峰值高光
        if (PinkXP::isCustomThemeActive())
        {
            constexpr float kBlendIn  = 0.12f;
            constexpr float kBloomOut = 0.90f;
            if (t < kBlendIn)
                return PinkXP::interpolateOkLab (PinkXP::content, PinkXP::chartRamp[0], t / kBlendIn);
            if (t <= kBloomOut)
                return PinkXP::sampleChartRamp ((t - kBlendIn) / (kBloomOut - kBlendIn));
            const auto peakTarget = (PinkXP::content.getPerceivedBrightness() > 0.55f)
                                        ? PinkXP::ink
                                        : PinkXP::hl;
            return PinkXP::interpolateOkLab (PinkXP::sampleChartRamp (1.0f), peakTarget,
                                             (t - kBloomOut) / (1.0f - kBloomOut) * 0.65f);
        }

        const bool lightBase = spectrogramBaseColour_.getPerceivedBrightness() > 0.55f;
        if (lightBase)
        {
            const juce::Colour stops[5] = {
                spectrogramBaseColour_,
                PinkXP::interpolateOkLab (spectrogramBaseColour_, PinkXP::pink300, 0.65f),
                spectrogramAccentColour_,
                PinkXP::interpolateOkLab (spectrogramAccentColour_, PinkXP::pink700, 0.55f),
                PinkXP::interpolateOkLab (PinkXP::pink700, PinkXP::ink, 0.45f)
            };
            return sampleColourStops (stops, 5, t);
        }
        else
        {
            const juce::Colour stops[5] = {
                spectrogramBaseColour_,
                PinkXP::interpolateOkLab (spectrogramBaseColour_, spectrogramAccentColour_, 0.42f),
                spectrogramAccentColour_,
                PinkXP::interpolateOkLab (spectrogramAccentColour_, PinkXP::accent2, 0.55f),
                PinkXP::interpolateOkLab (PinkXP::accent2, PinkXP::hl, 0.72f)
            };
            return sampleColourStops (stops, 5, t);
        }
    }();

    return PinkXP::isChromaKeySkin() ? PinkXP::chromaKeyFade (base, t) : base;
}

void SpectrogramModule::rebuildPaletteLut()
{
    for (int i = 0; i < 256; ++i)
    {
        const float lin = (float) i * (1.0f / 255.0f);
        const float curved = std::pow (lin, contrastGamma);
        paletteLut_[(size_t) i] = intensityToColour (curved).getARGB();
    }
}

void SpectrogramModule::rebuildRowTiltLut()
{
    rowTiltDb_.assign ((size_t) juce::jmax (0, rows), 0.0f);
    if (rows <= 0) return;

    const double sr      = hub.getSampleRate();
    const double nyquist = (sr > 0.0) ? sr * 0.5 : 24000.0;
    tiltSampleRate_      = sr;

    const double fMin = (double) activeMinFreqHz();
    const double fMax = juce::jmin ((double) activeMaxFreqHz(), nyquist * 0.95);
    if (fMax <= fMin || slopeDbPerOct <= 1.0e-5f)
        return;

    // r = 0 顶 = 高频，r = rows-1 底 = 低频
    const double denom = (double) juce::jmax (1, rows - 1);
    for (int r = 0; r < rows; ++r)
    {
        const double u = 1.0 - (double) r / denom;
        const double f = y2k::fscale::fromUnit (fftScale, u, fMin, fMax);
        rowTiltDb_[(size_t) r] = slopeDbPerOct * (float) std::log2 (juce::jmax (20.0, f) / 1000.0);
    }
}

// ----------------------------------------------------------
// 一帧 dB → 写入环形列
// ----------------------------------------------------------
void SpectrogramModule::pushColumn (const std::vector<float>& rowsDb)
{
    if (rows <= 0 || cols <= 0 || gridData.empty()) return;
    if ((int) rowsDb.size() != rows) return;

    // rowsDb 顺序为 低频(0) → 高频(rows-1)，写入翻转到 r=0 高频、r=rows-1 低频
    for (int i = 0; i < rows; ++i)
        set (rows - 1 - i, writeCol, rowsDb[(size_t) i]);

    writeCol = (writeCol + 1) % cols;
}

// ----------------------------------------------------------
// onFrame
// ----------------------------------------------------------
void SpectrogramModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    const bool appearanceChanged = syncAppearanceIfChanged();

    if (hold.active)
    {
        lastFrameMs = juce::Time::getMillisecondCounterHiRes();
        if (appearanceChanged)
        {
            buildGlSnapshot();
            repaint();
        }
        return;
    }

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto plot   = canvas.reduced (2);
    ensureGrid (plot.getWidth(), plot.getHeight());
    if (rows <= 0) return;

    const double sr      = hub.getSampleRate();
    const double nyquist = (sr > 0.0) ? sr * 0.5 : 24000.0;
    if (std::abs (sr - tiltSampleRate_) > 1.0)
        rebuildRowTiltLut();

    const float fMin = activeMinFreqHz();
    const float fMax = juce::jmin (activeMaxFreqHz(), (float) (nyquist * 0.95));
    if (fMax <= fMin) return;

    // 1) 先算速度解耦列数；若无时间平滑且本帧 nCols==0，跳过本帧频谱重采样与对数转换
    const double now = juce::Time::getMillisecondCounterHiRes();
    float dtMs = 0.0f;
    if (lastFrameMs > 0.0)
        dtMs = (float) (now - lastFrameMs);
    lastFrameMs = now;
    dtMs = juce::jlimit (0.0f, 500.0f, dtMs);

    columnAccumulator += dtMs * pixelsPerSecond * 0.001f / (float) cellPx;
    int nCols = (int) columnAccumulator;
    if (cols > 0 && nCols > cols) nCols = cols;
    if (nCols < 0) nCols = 0;
    columnAccumulator -= (float) nCols;

    const float relAlpha = juce::jlimit (0.0f, 0.80f, timeSmoothPct * 0.01f);
    const bool  needSample = (nCols > 0) || (relAlpha > 0.001f);

    if (needSample)
    {
        if ((int) colDbBuf.size() != rows)    colDbBuf.assign ((size_t) rows, kSilenceDb);
        if ((int) smoothColDb.size() != rows) smoothColDb.assign ((size_t) rows, kSilenceDb);

        if (displayMode == DisplayMode::Classic)
        {
            hub.getSpectrumMagnitudesBlended (rowMagBuf, rows, fMin, fMax, false, fftScale);
            for (int i = 0; i < rows; ++i)
                colDbBuf[(size_t) i] = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, std::abs (rowMagBuf.getUnchecked (i))));
        }
        else if (displayMode == DisplayMode::Sharp)
        {
            // Sharp：重分配叠加 -9 dB 经典底衬；在线性域合并后仅做一次 log10，与 dB 域 max(rDb, cDb - 9) 等价
            hub.getSpectrumReassignedBlended (rowMagBuf, rows, fMin, fMax, fftScale);
            hub.getSpectrumMagnitudesBlended (classicMagBuf, rows, fMin, fMax, false, fftScale);

            constexpr float kClassicFloorScale = 0.35481339f; // 10^(-9/20)
            for (int i = 0; i < rows; ++i)
            {
                const float rMag = std::abs (rowMagBuf.getUnchecked (i));
                const float cMag = std::abs (classicMagBuf.getUnchecked (i)) * kClassicFloorScale;
                colDbBuf[(size_t) i] = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, juce::jmax (rMag, cMag)));
            }
        }
        else
        {
            // Sharper：纯频率重分配粒子束（复原原版凝聚效果，低频 bassline 完全收束为纤细光束，不叠加经典底衬）
            hub.getSpectrumReassignedBlended (rowMagBuf, rows, fMin, fMax, fftScale);
            for (int i = 0; i < rows; ++i)
                colDbBuf[(size_t) i] = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, std::abs (rowMagBuf.getUnchecked (i))));
        }

        // 2) 非对称时间平滑（起音保持利落、拖尾滤除横向噪点颗粒）
        if (relAlpha > 0.001f)
        {
            const float atkAlpha = relAlpha * 0.45f;
            for (int i = 0; i < rows; ++i)
            {
                const float cur = colDbBuf[(size_t) i];
                const float prv = smoothColDb[(size_t) i];
                const float a   = (cur > prv) ? atkAlpha : relAlpha;
                smoothColDb[(size_t) i] = (prv <= -135.0f) ? cur : (prv * a + cur * (1.0f - a));
            }
        }
    }

    const auto& activeColDb = (relAlpha > 0.001f) ? smoothColDb : colDbBuf;

    // 3) 写入历史列：GL 直绘态跳过冗余的 CPU 软件位图（imageBuf）写入并释放其内存
    const bool glActive = glDirectActive.load (std::memory_order_acquire);
    if (glActive)
    {
        if (imageBuf.isValid())
            imageBuf = juce::Image();
        if (nCols > 0)
        {
            for (int k = 0; k < nCols; ++k)
                pushColumn (activeColDb);
            if (nCols >= cols)
                texCacheValid_ = false;
            imageBufDirty_ = true;
        }
    }
    else
    {
        ensureImage();
        for (int k = 0; k < nCols; ++k)
        {
            pushColumn (activeColDb);
            writeLatestColumnToImage();
        }
        if (nCols >= cols)
            texCacheValid_ = false;
    }

    // Right / Up / Down 方向无子列连续滚动，nCols==0 且外观未变、GPU 纹理已同步时免去全图重传
    if (glActive && ! appearanceChanged && direction != Direction::Left && nCols == 0
        && texCacheValid_ && lastBuiltDir_ == direction && lastBuiltWriteCol_ == writeCol
        && texDataGen_ != 0
        && glTexUploadedGen.load (std::memory_order_acquire) == texDataGen_
        && smoothPixels == lastUploadedLinear_)
        return;

    buildGlSnapshot();
    if (hoverActive || ! requestGlFrameOnly())
        repaintFrameContent();
}

// ----------------------------------------------------------
// GlDirectModuleBase 钩子（纹理通道）
// ----------------------------------------------------------
juce::Rectangle<int> SpectrogramModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void SpectrogramModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    drawAxisLabels (g, canvas.reduced (2));
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void SpectrogramModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
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

    const auto canvas = getCanvasBounds (contentBounds);
    drawBackground (g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    const auto plot = canvas.reduced (2);
    if (plot.getWidth() <= 4 || plot.getHeight() <= 4) return;

    auto* self = const_cast<SpectrogramModule*> (this);
    self->syncAppearanceIfChanged();
    self->ensureGrid (plot.getWidth(), plot.getHeight());
    self->ensureImage();
    if (rows <= 0 || cols <= 0 || ! imageBuf.isValid()) return;

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (plot);
        g.setImageResamplingQuality (smoothPixels ? juce::Graphics::mediumResamplingQuality
                                                  : juce::Graphics::lowResamplingQuality);

        const int cw = plot.getWidth();
        const int ch = plot.getHeight();
        const float wc = (float) writeCol + juce::jlimit (0.0f, 1.0f, columnAccumulator);
        const float px = (float) plot.getX(), py = (float) plot.getY();
        juce::AffineTransform xform;
        if (! isVertical())
        {
            const float sx = (float) cw / (float) cols;
            const float sy = (float) ch / (float) rows;
            if (direction == Direction::Left)
                xform = juce::AffineTransform (sx, 0.0f, px - wc * sx,
                                               0.0f, sy, py);
            else
                xform = juce::AffineTransform (-sx, 0.0f, px + (wc + (float) cols) * sx,
                                               0.0f, sy, py);
        }
        else
        {
            const float sx = (float) cw / (float) rows;
            const float sy = (float) ch / (float) cols;
            if (direction == Direction::Up)
                xform = juce::AffineTransform (0.0f, -sx, px + (float) rows * sx,
                                               sy, 0.0f, py - wc * sy);
            else
                xform = juce::AffineTransform (0.0f, -sx, px + (float) rows * sx,
                                               -sy, 0.0f, py + (wc + (float) cols) * sy);
        }
        g.setOpacity (1.0f);
        g.drawImageTransformed (imageBuf, xform, false);
        PinkXP::Craft::drawCanvasFinishOverlay (g, plot);
    }

    drawAxisLabels (g, plot);
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void SpectrogramModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;

    syncAppearanceIfChanged();

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto plot   = canvas.reduced (2);
    if (plot.getWidth() <= 4 || plot.getHeight() <= 4)
        return;

    if (rows <= 0 || cols <= 0)
        ensureGrid (plot.getWidth(), plot.getHeight());
    if (rows <= 0 || cols <= 0)
        return;

    auto snap = beginGlSnapshot();
    snap->canvasLocal = canvas;
    snap->texLinear   = smoothPixels;

    auto finalizeTexUpload = [&] (bool pixelsOrFilterChanged)
    {
        if (pixelsOrFilterChanged || texDataGen_ == 0)
        {
            if (++texDataGen_ == 0)
                texDataGen_ = 1;
        }

        const juce::uint64 uploadedGen = glTexUploadedGen.load (std::memory_order_acquire);
        const bool gpuUpToDate = glDirectActive.load (std::memory_order_acquire)
                              && (uploadedGen != 0)
                              && (uploadedGen == texDataGen_)
                              && (smoothPixels == lastUploadedLinear_);
        snap->texDataGen = texDataGen_;
        if (gpuUpToDate)
        {
            snap->texDirty = false;
            // snap->texPixels 保持空：零字节拷贝、零 GL 纹理上传
        }
        else
        {
            snap->texDirty      = true;
            lastUploadedLinear_ = smoothPixels;
            snap->texPixels     = texPixelsCache_;
        }
    };

    if (direction != Direction::Left)
    {
        const bool vertical = isVertical();
        const int  expectedW = vertical ? rows : cols;
        const int  expectedH = vertical ? cols : rows;
        const size_t expectedPix = (size_t) expectedW * (size_t) expectedH;

        const bool cacheHit = texCacheValid_
                           && lastBuiltDir_ == direction
                           && lastBuiltWriteCol_ == writeCol
                           && texCacheW_ == expectedW
                           && texCacheH_ == expectedH
                           && texPixelsCache_.size() == expectedPix;
        bool texChanged = (smoothPixels != lastUploadedLinear_);
        if (! cacheHit)
        {
            buildUnrolledTexture (texPixelsCache_, texCacheW_, texCacheH_);
            texCacheValid_     = true;
            lastBuiltDir_      = direction;
            lastBuiltWriteCol_ = writeCol;
            texChanged         = true;
        }

        snap->texW          = texCacheW_;
        snap->texH          = texCacheH_;
        snap->texScrollU    = 0.0f;
        snap->texWinW       = 1.0f;
        snap->texTargetRect = plot;
        snap->fxRect        = snap->texTargetRect;
        finalizeTexUpload (texChanged);

        // 透明模式下 appendWellFrame 不产出底板顶点（bgVerts==0），仍须压入占位段
        //   使 Editor 纹理通道（texW>0）正常上传并绘制直通透明瀑布图纹理。
        appendWellFrame (snap->vertices, canvas, PinkXP::content);
        snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
        publishGlSnapshot (std::move (snap));
        return;
    }

    snap->texW = cols * 2;
    snap->texH = rows;

    const float frac = juce::jlimit (0.0f, 0.999f, columnAccumulator);
    if (smoothPixels && cols >= 3)
    {
        // GL_LINEAR 模式下向内缩半个纹素且窗口取 cols-2，彻底杜绝双拷贝环形接缝处的 1px 混色拖影
        snap->texScrollU = ((float) writeCol + frac + 0.5f) / (float) (cols * 2);
        snap->texWinW    = (float) (cols - 2) / (float) (cols * 2);
    }
    else
    {
        snap->texScrollU = ((float) writeCol + frac) / (float) (cols * 2);
        snap->texWinW    = 0.5f;
    }
    snap->texTargetRect = plot;
    snap->fxRect        = snap->texTargetRect;

    {
        const size_t totalPix = (size_t) snap->texW * (size_t) snap->texH;
        if (lastBuiltDir_ != Direction::Left
            || texCacheW_ != snap->texW
            || texCacheH_ != snap->texH
            || texPixelsCache_.size() != totalPix)
            texCacheValid_ = false;

        const juce::uint32* const lut      = paletteLut_.data();
        const float               scale255 = invDbSpan_ * 255.0f;
        const int                 tiltSize = (int) rowTiltDb_.size();
        const float* const        tiltPtr  = rowTiltDb_.data();

        bool texChanged = (smoothPixels != lastUploadedLinear_);

        if (! texCacheValid_)
        {
            texPixelsCache_.resize (totalPix);
            for (int r = 0; r < rows; ++r)
            {
                const int rOut = rows - 1 - r;
                const float rowOffset = ((r < tiltSize) ? tiltPtr[r] : 0.0f) - floorDb;
                const float* const srcRow = gridData.data() + (size_t) r * (size_t) cols;
                juce::uint32* const dstRow = texPixelsCache_.data() + (size_t) rOut * (size_t) snap->texW;
                for (int c = 0; c < cols; ++c)
                {
                    const float t255 = (srcRow[c] + rowOffset) * scale255;
                    const int idx = (t255 <= 0.0f) ? 0 : (t255 >= 255.0f ? 255 : (int) (t255 + 0.5f));
                    const juce::uint32 argb = lut[idx];
                    dstRow[c]        = argb;
                    dstRow[c + cols] = argb;
                }
            }
            texCacheValid_ = true;
            texCacheW_     = snap->texW;
            texCacheH_     = snap->texH;
            lastBuiltDir_  = Direction::Left;
            texChanged     = true;
        }
        else
        {
            const int pending = (lastBuiltWriteCol_ < 0)
                                    ? cols
                                    : ((writeCol - lastBuiltWriteCol_ + cols) % cols);
            if (pending > 0)
            {
                for (int k = 0; k < pending; ++k)
                {
                    const int c = (writeCol - 1 - k + cols) % cols;
                    for (int r = 0; r < rows; ++r)
                    {
                        const float rowOffset = ((r < tiltSize) ? tiltPtr[r] : 0.0f) - floorDb;
                        const float t255 = (gridData[(size_t) (r * cols + c)] + rowOffset) * scale255;
                        const int idx = (t255 <= 0.0f) ? 0 : (t255 >= 255.0f ? 255 : (int) (t255 + 0.5f));
                        const juce::uint32 argb = lut[idx];
                        const int rOut = rows - 1 - r;
                        juce::uint32* const dstRow = texPixelsCache_.data() + (size_t) rOut * (size_t) snap->texW;
                        dstRow[c]        = argb;
                        dstRow[c + cols] = argb;
                    }
                }
                texChanged = true;
            }
        }
        lastBuiltWriteCol_ = writeCol;
        finalizeTexUpload (texChanged);
    }

    // 透明模式下 appendWellFrame 不产出底板顶点（0 顶点占位段保活 Editor 纹理通道绘制）
    appendWellFrame (snap->vertices, canvas, PinkXP::content);
    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

void SpectrogramModule::buildUnrolledTexture (std::vector<juce::uint32>& out, int& outW, int& outH) const
{
    const bool vertical = isVertical();
    outW = vertical ? rows : cols;
    outH = vertical ? cols : rows;
    out.resize ((size_t) outW * (size_t) outH);

    const juce::uint32* const lut      = paletteLut_.data();
    const float               scale255 = invDbSpan_ * 255.0f;
    const int                 tiltSize = (int) rowTiltDb_.size();
    const float* const        tiltPtr  = rowTiltDb_.data();
    juce::uint32* const       dstBase  = out.data();

    for (int r = 0; r < rows; ++r)
    {
        const float rowOffset = ((r < tiltSize) ? tiltPtr[r] : 0.0f) - floorDb;
        const float* const srcRow = gridData.data() + (size_t) r * (size_t) cols;

        if (direction == Direction::Right)
        {
            juce::uint32* const dstRow = dstBase + (size_t) (rows - 1 - r) * (size_t) outW;
            for (int tau = 0; tau < cols; ++tau)
            {
                const int c = (writeCol + tau) % cols;
                const float t255 = (srcRow[c] + rowOffset) * scale255;
                const int idx = (t255 <= 0.0f) ? 0 : (t255 >= 255.0f ? 255 : (int) (t255 + 0.5f));
                dstRow[cols - 1 - tau] = lut[idx];
            }
        }
        else if (direction == Direction::Up)
        {
            const int x = rows - 1 - r;
            for (int tau = 0; tau < cols; ++tau)
            {
                const int c = (writeCol + tau) % cols;
                const float t255 = (srcRow[c] + rowOffset) * scale255;
                const int idx = (t255 <= 0.0f) ? 0 : (t255 >= 255.0f ? 255 : (int) (t255 + 0.5f));
                dstBase[(size_t) (cols - 1 - tau) * (size_t) outW + (size_t) x] = lut[idx];
            }
        }
        else // Direction::Down
        {
            const int x = rows - 1 - r;
            for (int tau = 0; tau < cols; ++tau)
            {
                const int c = (writeCol + tau) % cols;
                const float t255 = (srcRow[c] + rowOffset) * scale255;
                const int idx = (t255 <= 0.0f) ? 0 : (t255 >= 255.0f ? 255 : (int) (t255 + 0.5f));
                dstBase[(size_t) tau * (size_t) outW + (size_t) x] = lut[idx];
            }
        }
    }
}

void SpectrogramModule::hoverToFreqAndAge (juce::Rectangle<int> canvas, juce::Point<int> p,
                                           float& freqHz, float& secondsAgo) const
{
    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) activeMinFreqHz();
    const double fMax       = juce::jmin ((double) activeMaxFreqHz(), nyquist * 0.95);

    float freqT = 0.0f, ageFrac = 0.0f;
    switch (direction)
    {
        case Direction::Left:
            freqT   = 1.0f - (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            ageFrac = (float) (canvas.getRight() - p.x) / (float) juce::jmax (1, canvas.getWidth());
            break;
        case Direction::Right:
            freqT   = 1.0f - (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            ageFrac = (float) (p.x - canvas.getX()) / (float) juce::jmax (1, canvas.getWidth());
            break;
        case Direction::Up:
            freqT   = (float) (p.x - canvas.getX()) / (float) juce::jmax (1, canvas.getWidth());
            ageFrac = 1.0f - (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            break;
        case Direction::Down:
            freqT   = (float) (p.x - canvas.getX()) / (float) juce::jmax (1, canvas.getWidth());
            ageFrac = (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            break;
    }

    freqHz = (float) y2k::fscale::fromUnit (fftScale, juce::jlimit (0.0, 1.0, (double) freqT), fMin, fMax);
    const float spanPx = isVertical() ? (float) canvas.getHeight() : (float) canvas.getWidth();
    secondsAgo = juce::jlimit (0.0f, 1.0f, ageFrac) * spanPx / juce::jmax (1.0f, pixelsPerSecond);
}

void SpectrogramModule::mouseMove (const juce::MouseEvent& e)
{
    ModulePanel::mouseMove (e);

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

void SpectrogramModule::mouseDrag (const juce::MouseEvent& e)
{
    ModulePanel::mouseDrag (e);

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

void SpectrogramModule::mouseDown (const juce::MouseEvent& e)
{
    ModulePanel::mouseDown (e);

    if (e.mods.isLeftButtonDown()
        && getCanvasBounds (getContentBounds()).reduced (6).contains (e.getPosition())
        && ! getControlDrawerBounds().contains (e.getPosition()))
    {
        hold.press();
        repaint();
    }
}

void SpectrogramModule::mouseUp (const juce::MouseEvent& e)
{
    ModulePanel::mouseUp (e);

    if (hold.active)
    {
        hold.release();
        repaint();
    }
}

void SpectrogramModule::mouseExit (const juce::MouseEvent& e)
{
    ModulePanel::mouseExit (e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint (getCanvasBounds (getContentBounds()));
    }
}

void SpectrogramModule::drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (! hoverActive || ! canvas.contains (hoverPos))
        return;

    float freqHz = 0.0f, secondsAgo = 0.0f;
    hoverToFreqAndAge (canvas, hoverPos, freqHz, secondsAgo);

    juce::String readout = PinkXP::formatFreqHz (freqHz)
                         + "  " + juce::String (-secondsAgo, 1) + " s";

    // 悬停即显示当前频率坐标对应的音名
    const juce::String note = PinkXP::noteNameForHz (freqHz);
    if (note.isNotEmpty())
        readout += "  " + note;

    PinkXP::drawHoverRuler (g, canvas, hoverPos, readout);
}

// ----------------------------------------------------------
// 双拷贝环形 Image（CPU / 浮动态回退）
// ----------------------------------------------------------
void SpectrogramModule::ensureImage()
{
    if (rows <= 0 || cols <= 0) return;

    const int desiredW = cols * 2;
    if (! imageBuf.isValid()
        || imageBuf.getWidth()  != desiredW
        || imageBuf.getHeight() != rows)
    {
        imageBuf = juce::Image (juce::Image::ARGB, desiredW, rows, true, juce::SoftwareImageType());
        imageBufDirty_ = true;
    }
    if (imageBufDirty_)
    {
        redrawFullImage();
        imageBufDirty_ = false;
    }
}

void SpectrogramModule::redrawFullImage()
{
    if (! imageBuf.isValid() || rows <= 0 || cols <= 0) return;

    juce::Image::BitmapData bmp (imageBuf, juce::Image::BitmapData::readWrite);
    const juce::uint32* const lut      = paletteLut_.data();
    const float               scale255 = invDbSpan_ * 255.0f;
    const int                 tiltSize = (int) rowTiltDb_.size();
    const float* const        tiltPtr  = rowTiltDb_.data();

    for (int r = 0; r < rows; ++r)
    {
        auto* row = (juce::PixelARGB*) bmp.getLinePointer (r);
        const float rowOffset = ((r < tiltSize) ? tiltPtr[r] : 0.0f) - floorDb;
        const float* const srcRow = gridData.data() + (size_t) r * (size_t) cols;
        for (int c = 0; c < cols; ++c)
        {
            const float t255 = (srcRow[c] + rowOffset) * scale255;
            const int idx = (t255 <= 0.0f) ? 0 : (t255 >= 255.0f ? 255 : (int) (t255 + 0.5f));
            const juce::uint32 argb = lut[idx];
            juce::PixelARGB px ((juce::uint8) ((argb >> 24) & 0xff),
                                (juce::uint8) ((argb >> 16) & 0xff),
                                (juce::uint8) ((argb >>  8) & 0xff),
                                (juce::uint8) ( argb        & 0xff));
            px.premultiply();
            row[c]        = px;
            row[c + cols] = px;
        }
    }
}

void SpectrogramModule::writeLatestColumnToImage()
{
    if (! imageBuf.isValid() || rows <= 0 || cols <= 0) return;

    const int justWrittenCol = (writeCol - 1 + cols) % cols;
    juce::Image::BitmapData bmp (imageBuf, juce::Image::BitmapData::writeOnly);

    for (int r = 0; r < rows; ++r)
    {
        auto* row = (juce::PixelARGB*) bmp.getLinePointer (r);
        const juce::uint32 argb = dbRowToArgb (at (r, justWrittenCol), r);
        juce::PixelARGB px ((juce::uint8) ((argb >> 24) & 0xff),
                            (juce::uint8) ((argb >> 16) & 0xff),
                            (juce::uint8) ((argb >>  8) & 0xff),
                            (juce::uint8) ( argb        & 0xff));
        px.premultiply();
        row[justWrittenCol]        = px;
        row[justWrittenCol + cols] = px;
    }
}

void SpectrogramModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
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
// 参考网格 + 轴标签（根据当前 FreqRange / FFTScale / SkinTokens 自适应排布）
// ----------------------------------------------------------
void SpectrogramModule::drawAxisLabels (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    if (PinkXP::isChromaKeySkin())
        return;
    if (! showGrid && ! showLabels)
        return;

    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) activeMinFreqHz();
    const double fMax       = juce::jmin ((double) activeMaxFreqHz(), nyquist * 0.95);
    if (fMax <= fMin)
        return;

    struct Tick { float hz; const char* label; };
    static const Tick kLogTicks[] = {
        { 30.0f, "30" }, { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" },
        { 500.0f, "500" }, { 1000.0f, "1k" }, { 2000.0f, "2k" }, { 5000.0f, "5k" },
        { 10000.0f, "10k" }, { 15000.0f, "15k" }
    };
    static const Tick kMelTicks[] = {
        { 100.0f, "100" }, { 250.0f, "250" }, { 500.0f, "500" }, { 1000.0f, "1k" },
        { 2000.0f, "2k" }, { 4000.0f, "4k" }, { 8000.0f, "8k" }, { 12000.0f, "12k" },
        { 16000.0f, "16k" }
    };
    static const Tick kLinBassTicks[] = {
        { 200.0f, "200" }, { 400.0f, "400" }, { 600.0f, "600" }, { 800.0f, "800" }
    };
    static const Tick kLinMidTicks[] = {
        { 2000.0f, "2k" }, { 4000.0f, "4k" }, { 6000.0f, "6k" }, { 8000.0f, "8k" }, { 10000.0f, "10k" }
    };
    static const Tick kLinFullTicks[] = {
        { 5000.0f, "5k" }, { 10000.0f, "10k" }, { 15000.0f, "15k" }
    };

    const Tick* ticks = kLogTicks;
    int numTicks = (int) std::size (kLogTicks);
    if (fftScale == y2k::fscale::Scale::mel)
    {
        ticks = kMelTicks;
        numTicks = (int) std::size (kMelTicks);
    }
    else if (fftScale == y2k::fscale::Scale::linear)
    {
        if (fMax <= 1200.0)       { ticks = kLinBassTicks; numTicks = (int) std::size (kLinBassTicks); }
        else if (fMax <= 12500.0) { ticks = kLinMidTicks;  numTicks = (int) std::size (kLinMidTicks); }
        else                      { ticks = kLinFullTicks; numTicks = (int) std::size (kLinFullTicks); }
    }

    const auto& tokens   = PinkXP::getSkinTokens();
    const bool  darkBg   = isDarkPalette();
    const bool  isModern = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
    const bool  isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool  isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool  isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool  isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const juce::Colour gridBaseCol = isNeon
        ? PinkXP::sel.interpolatedWith (darkBg ? juce::Colours::white : PinkXP::ink, 0.30f).withAlpha (0.78f)
        : isCrt
            ? PinkXP::sel.interpolatedWith (darkBg ? juce::Colours::white : PinkXP::ink, 0.25f).withAlpha (0.76f)
            : darkBg
                ? (isChrome ? PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.62f)
                   : isGlass ? juce::Colours::white.withAlpha (0.48f)
                             : juce::Colours::white.withAlpha (isModern ? 0.55f : 0.65f))
                : (isChrome ? PinkXP::dark.interpolatedWith (PinkXP::ink, 0.40f).withAlpha (0.60f)
                   : isGlass ? PinkXP::ink.withAlpha (0.48f)
                             : PinkXP::ink.withAlpha (isModern ? 0.55f : 0.65f));

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
        badgeFill   = darkBg ? juce::Colour (0xd80c0a14) : PinkXP::btnFace.withAlpha (0.92f);
        badgeBorder = darkBg ? PinkXP::pink300.withAlpha (0.78f) : PinkXP::dark.withAlpha (0.85f);
    }
    else if (isChrome)
    {
        badgeFill   = darkBg
                          ? PinkXP::face.interpolatedWith (juce::Colour (0xff1a1d24), 0.52f).withAlpha (0.90f)
                          : PinkXP::face.interpolatedWith (PinkXP::hl, 0.28f).withAlpha (0.92f);
        badgeBorder = darkBg
                          ? PinkXP::hl.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.36f)
                          : PinkXP::dark.withAlpha (0.48f);
    }
    else if (isGlass)
    {
        badgeFill   = darkBg
                          ? juce::Colour (0xb2161c28)
                          : PinkXP::face.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.82f);
        badgeBorder = darkBg
                          ? juce::Colours::white.withAlpha (0.30f)
                          : PinkXP::dark.withAlpha (0.24f);
    }
    else if (isModern)
    {
        badgeFill   = darkBg ? juce::Colour (0xb8101218)
                             : PinkXP::content.interpolatedWith (PinkXP::face, 0.48f).withAlpha (0.88f);
        badgeBorder = darkBg ? juce::Colours::white.withAlpha (0.16f)
                             : PinkXP::skinHairlineOn (PinkXP::face, 0.22f);
    }
    else
    {
        badgeFill   = darkBg ? juce::Colour (0xc8101018) : PinkXP::face.withAlpha (0.86f);
        badgeBorder = darkBg ? juce::Colours::white.withAlpha (0.24f) : PinkXP::dark.withAlpha (0.30f);
    }
    const juce::Colour badgeText = PinkXP::readableInkOn (badgeFill.withAlpha (1.0f));
    const float labelPt = isNeon ? 7.6f : (isCrt ? 8.5f : 9.0f);

    g.setFont (PinkXP::getAxisFont (labelPt, juce::Font::plain));
    const int minSpacingPx = isVertical() ? ((isCrt || isNeon) ? 38 : 32) : 18;
    int lastPosPx = -10000;

    for (int li = 0; li < numTicks; ++li)
    {
        const auto& tk = ticks[li];
        if ((double) tk.hz <= fMin * 1.05 || (double) tk.hz >= fMax * 0.96)
            continue;

        const double t = y2k::fscale::toUnit (fftScale, (double) tk.hz, fMin, fMax);
        const int posPx = isVertical()
            ? canvas.getX() + (int) std::round (t * (double) canvas.getWidth())
            : canvas.getY() + (int) std::round ((1.0 - t) * (double) canvas.getHeight());

        if (std::abs (posPx - lastPosPx) < minSpacingPx)
            continue;
        lastPosPx = posPx;

        if (showGrid)
        {
            if (isVertical())
                PinkXP::Craft::drawGridLine (g, (float) posPx, (float) canvas.getY(),
                                             (float) posPx, (float) canvas.getBottom(), true, gridBaseCol);
            else
                PinkXP::Craft::drawGridLine (g, (float) canvas.getX(), (float) posPx,
                                             (float) canvas.getRight(), (float) posPx, true, gridBaseCol);
        }

        if (showLabels)
        {
            const auto img = PinkXP::cachedStaticText (
                g, tk.label, labelPt, juce::Font::plain, badgeText,
                juce::Colours::transparentBlack, true);
            const int minBw = (isCrt || isNeon) ? 32 : 26;
            const int bw = juce::jlimit (minBw, 44, img.isValid() ? (img.getWidth() + 8) : minBw);
            const int bh = 13;
            int bx = isVertical() ? (posPx - bw / 2) : (canvas.getX() + 2);
            int by = isVertical() ? (canvas.getY() + 2) : (posPx - bh / 2);
            bx = juce::jlimit (canvas.getX() + 1, juce::jmax (canvas.getX() + 1, canvas.getRight() - bw - 1), bx);
            by = juce::jlimit (canvas.getY() + 1, juce::jmax (canvas.getY() + 1, canvas.getBottom() - bh - 1), by);

            const juce::Rectangle<int> badgeRect (bx, by, bw, bh);
            PinkXP::Craft::drawBadgePill (g, badgeRect, badgeFill, badgeBorder);

            if (img.isValid() && img.getWidth() <= bw - 4)
            {
                g.setOpacity (1.0f);
                img.drawAt (g, (float) (bx + (bw - img.getWidth()) / 2),
                            (float) (by + (bh - img.getHeight()) / 2));
            }
            else
            {
                g.setColour (badgeText);
                g.drawFittedText (y2k::tr (tk.label), badgeRect.reduced (2, 0), juce::Justification::centred, 1, 0.70f);
            }
        }
    }

    if (! showLabels)
        return;

    // older / newer 方向提示（耦合 Craft::drawBadgePill）
    {
        const int bh = 13, bw = (isCrt || isNeon) ? 68 : 60;
        const float dirPt = isNeon ? 7.8f : 8.5f;
        auto slot = [&] (juce::Rectangle<int> r, const juce::String& text)
        {
            PinkXP::Craft::drawBadgePill (g, r, badgeFill, badgeBorder);
            PinkXP::drawCachedSymbolText (g, text, r, juce::Justification::centred,
                                          dirPt, juce::Font::plain, badgeText);
        };
        const juce::String older = juce::String::fromUTF8 ("older");
        const juce::String newer = juce::String::fromUTF8 ("newer");
        const juce::String arrL  = juce::String::fromUTF8 ("\xe2\x86\x90 ");
        const juce::String arrR  = juce::String::fromUTF8 (" \xe2\x86\x92");
        const juce::String arrU  = juce::String::fromUTF8 ("\xe2\x86\x91 ");
        const juce::String arrD  = juce::String::fromUTF8 ("\xe2\x86\x93 ");

        const int by = canvas.getBottom() - bh - 2;
        const juce::Rectangle<int> left  (canvas.getX() + 2,          by, bw, bh);
        const juce::Rectangle<int> right (canvas.getRight() - bw - 2, by, bw, bh);
        const juce::Rectangle<int> top   (canvas.getRight() - bw - 2, canvas.getY() + 2, bw, bh);
        const juce::Rectangle<int> bot   (canvas.getRight() - bw - 2, by, bw, bh);

        switch (direction)
        {
            case Direction::Left:  slot (left, arrL + older); slot (right, newer + arrR); break;
            case Direction::Right: slot (left, arrL + newer); slot (right, older + arrR); break;
            case Direction::Up:    slot (top,  arrU + older); slot (bot,   newer + juce::String (" ") + arrD.trim()); break;
            case Direction::Down:  slot (top,  arrU + newer); slot (bot,   older + juce::String (" ") + arrD.trim()); break;
        }
    }
}
