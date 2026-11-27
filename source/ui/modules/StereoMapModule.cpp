#include "source/ui/modules/StereoMapModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"

#include <cmath>

namespace
{
    // ==========================================================
    // 亮度曲线（只作用于亮度，全程不参与声像计算）
    // ==========================================================
    constexpr float kSlopeDbPerOct = 4.5f;   // 与 Spectrum 的 slope 观感一致（基准 1kHz）
    constexpr float kHfLiftDb      = 7.0f;   // 高频亮度补偿上限
    constexpr float kHfLiftOct     = 3.3f;   // 1kHz 起 3.3 个八度（≈10kHz）到顶

    // 频率 → 亮度补偿 dB。
    //   4.5dB/oct 倾斜解决"频谱天然向右下坠"，但音乐素材在 8~15kHz 常比 1kHz
    //   低 20dB 以上，只靠倾斜时 HF 的 strength 会掉到 0.2 附近（曲线近乎不可见）。
    //   这里再叠一段只作用于亮度的高频提升，让 HF 与中频的观感强度对齐。
    float visibilityDb (float freqHz) noexcept
    {
        const float oct  = std::log2 (juce::jmax (20.0f, freqHz) / 1000.0f);
        const float lift = kHfLiftDb * juce::jlimit (0.0f, 1.0f, oct / kHfLiftOct);
        return kSlopeDbPerOct * oct + lift;
    }

    // 对数轴位置 0..1 → strength 黑场（strength = 0 对应的 dB）。
    //   低频 -90dB / 高频 -66dB。高频增益叠加后若黑场不动，FFT 噪声底会被
    //   一起点亮成雾状曲线，因此高频侧把黑场同步抬高。
    float blackFloorDb (float norm01) noexcept
    {
        return juce::jmap (std::sqrt (juce::jlimit (0.0f, 1.0f, norm01)), -90.0f, -66.0f);
    }

    // ==========================================================
    // 颜色梯度（accent = L 侧 / accent2 = R 侧）
    //   每侧是一条 3 锚点 OKLab 色阶：暗（混向画布）→ 本色 → 亮（混向
    //   ink 或 hl）。画布明暗决定"高值端"方向：亮画布加深以提高对比，
    //   暗画布提亮，保证两端在所有主题下都能读出层次。
    // ==========================================================
    struct SideRamp
    {
        juce::Colour lo, mid, hi;
    };

    SideRamp makeSideRamp (juce::Colour anchor)
    {
        const bool darkCanvas = PinkXP::content.getPerceivedBrightness() < 0.5f;

        SideRamp r;
        r.lo  = PinkXP::interpolateOkLab (anchor, PinkXP::content, 0.60f);
        r.mid = anchor;
        r.hi  = PinkXP::interpolateOkLab (anchor,
                                          darkCanvas ? PinkXP::hl : PinkXP::ink,
                                          0.45f);
        return r;
    }

    // value 0..1 → 该侧色阶上的颜色
    juce::Colour sampleRamp (const SideRamp& r, float value)
    {
        const float v = juce::jlimit (0.0f, 1.0f, value);
        if (v <= 0.5f)
            return PinkXP::interpolateOkLab (r.lo, r.mid, v * 2.0f);
        return PinkXP::interpolateOkLab (r.mid, r.hi, (v - 0.5f) * 2.0f);
    }

    // pan: +1 = 全左（accent），-1 = 全右（accent2）；value: 0..1 能量。
    //   中间区域两侧颜色按 OKLab 混合，声像越偏越接近该侧的本色。
    juce::Colour stereoMapColour (const SideRamp& leftRamp, const SideRamp& rightRamp,
                                  float pan, float value)
    {
        const juce::Colour colL = sampleRamp (leftRamp,  value);
        const juce::Colour colR = sampleRamp (rightRamp, value);
        const float w = juce::jlimit (0.0f, 1.0f,
                                      0.5f + juce::jlimit (-1.0f, 1.0f, pan) * 1.4f);
        return PinkXP::interpolateOkLab (colR, colL, w);
    }

    // ==========================================================
    // MONO 抵消模式的色阶：0（兼容）→ 0.5（部分损失）→ 1（严重抵消）
    //   低端混向画布（贴底的"没问题"频段自然沉入背景），高端落到主题的
    //   danger 色（语义色：过载/危险），并按画布明暗做加深/提亮。
    // ==========================================================
    SideRamp makeSeverityRamp()
    {
        const bool darkCanvas = PinkXP::content.getPerceivedBrightness() < 0.5f;

        SideRamp r;
        r.lo  = PinkXP::interpolateOkLab (PinkXP::accent2, PinkXP::content, 0.62f);
        r.mid = PinkXP::interpolateOkLab (PinkXP::accent2, PinkXP::warn, 0.50f);
        r.hi  = PinkXP::interpolateOkLab (PinkXP::danger,
                                          darkCanvas ? PinkXP::hl : PinkXP::ink,
                                          0.22f);
        return r;
    }

    // 填充梯形（底边基线 → 两个顶点），顶点格式与 VertexStream 一致（x,y,r,g,b,a）
    void appendFilledBand (std::vector<float>& v,
                           float xA, float yA, float xB, float yB, float baseY,
                           juce::Colour c)
    {
        const float r = c.getFloatRed(), g = c.getFloatGreen(),
                    b = c.getFloatBlue(), a = c.getFloatAlpha();

        const float pts[6][2] = { { xA, baseY }, { xB, baseY }, { xB, yB },
                                  { xA, baseY }, { xB, yB },   { xA, yA } };
        for (auto& p : pts)
        {
            v.push_back (p[0]); v.push_back (p[1]);
            v.push_back (r);    v.push_back (g);
            v.push_back (b);    v.push_back (a);
        }
    }
}

StereoMapModule::StereoMapModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::stereoMap), hub (h)
{
    hub.retain (AnalyserHub::Kind::Spectrum);
    hub.addFrameListener (this);

    setMinSize (96, 80);
    setDefaultSize (384, 240);
    setTitleText ("Stereo Map");
    setControlPanelEnabled (true);   // v3.1：PAN/MONO 模式切换与 FFT scale 都在参数面板里

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        buildGlSnapshot();
        repaint();
    });
}

StereoMapModule::~StereoMapModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Spectrum);
}

void StereoMapModule::setMode (Mode m)
{
    if (mode == m)
        return;

    mode = m;

    // 切入 MONO 时丢掉旧缓存，第一帧直接对齐目标值：
    //   否则曲线会从上一轮（可能是几分钟前）的遗留值慢慢爬过来。
    if (mode == Mode::cancel)
        cancelByBand.clear();

    buildGlSnapshot();
    repaint();
}

void StereoMapModule::setFftScale (y2k::fscale::Scale s)
{
    if (fftScale == s)
        return;

    fftScale = s;

    // 每个频段对应的频率变了：平滑缓存全部作废，第一帧直接对齐目标
    panByBand.clear();
    strengthByBand.clear();
    levelDbByBand.clear();
    cancelByBand.clear();
    peakPanByBand.clear();
    peakCancelByBand.clear();

    buildGlSnapshot();
    repaint();
}

float StereoMapModule::activeMinFreqHz() const noexcept
{
    switch (freqRange)
    {
        case FreqRange::music: return 30.0f;
        case FreqRange::vocal: return 80.0f;
        case FreqRange::bass:  return 20.0f;
        case FreqRange::full:
        default:               return kMinFreq;
    }
}

float StereoMapModule::activeMaxFreqHz() const noexcept
{
    switch (freqRange)
    {
        case FreqRange::music: return 12000.0f;
        case FreqRange::vocal: return 8000.0f;
        case FreqRange::bass:  return 1000.0f;
        case FreqRange::full:
        default:               return kMaxFreq;
    }
}

void StereoMapModule::setVisualStyle (VisualStyle s)
{
    if (visualStyle == s) return;
    visualStyle = s;
    buildGlSnapshot();
    repaint();
}

void StereoMapModule::setFreqRange (FreqRange r)
{
    if (freqRange == r) return;
    freqRange = r;
    panByBand.clear();
    strengthByBand.clear();
    levelDbByBand.clear();
    cancelByBand.clear();
    peakPanByBand.clear();
    peakCancelByBand.clear();
    buildGlSnapshot();
    repaint();
}

void StereoMapModule::setSmoothing (float mult)
{
    smoothingScale = juce::jlimit (0.2f, 3.0f, mult);
}

void StereoMapModule::setPeakHold (bool on)
{
    if (peakHoldEnabled == on) return;
    peakHoldEnabled = on;
    if (! peakHoldEnabled)
    {
        peakPanByBand.clear();
        peakCancelByBand.clear();
    }
    buildGlSnapshot();
    repaint();
}

void StereoMapModule::setCancelScalePct (float pct)
{
    cancelFullScale = juce::jlimit (0.20f, 1.00f, pct * 0.01f);
    buildGlSnapshot();
    repaint();
}

void StereoMapModule::setShowGrid (bool on)
{
    if (showGrid == on) return;
    showGrid = on;
    buildGlSnapshot();
    repaint();
}

void StereoMapModule::setShowLabels (bool on)
{
    if (showLabels == on) return;
    showLabels = on;
    buildGlSnapshot();
    repaint();
}

juce::ValueTree StereoMapModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("mode", mode == Mode::cancel ? "cancel" : "pan", nullptr);
    s.setProperty ("fftScale", (int) fftScale, nullptr);
    s.setProperty ("style", (int) visualStyle, nullptr);
    s.setProperty ("freqRange", (int) freqRange, nullptr);
    s.setProperty ("smoothing", (double) smoothingScale, nullptr);
    s.setProperty ("peakHold", peakHoldEnabled, nullptr);
    s.setProperty ("cancelScale", (double) (cancelFullScale * 100.0f), nullptr);
    s.setProperty ("showGrid", showGrid, nullptr);
    s.setProperty ("showLabels", showLabels, nullptr);
    return s;
}

void StereoMapModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    if (state.hasProperty ("mode"))
        setMode (state.getProperty ("mode").toString() == "cancel" ? Mode::cancel : Mode::pan);
    if (state.hasProperty ("fftScale"))
        setFftScale (y2k::fscale::fromIndex ((int) state.getProperty ("fftScale")));
    if (state.hasProperty ("style"))
        setVisualStyle ((VisualStyle) juce::jlimit (0, 2, (int) state.getProperty ("style")));
    if (state.hasProperty ("freqRange"))
        setFreqRange ((FreqRange) juce::jlimit (0, 3, (int) state.getProperty ("freqRange")));
    if (state.hasProperty ("smoothing"))
        setSmoothing ((float) (double) state.getProperty ("smoothing"));
    if (state.hasProperty ("peakHold"))
        setPeakHold ((bool) state.getProperty ("peakHold"));
    if (state.hasProperty ("cancelScale"))
        setCancelScalePct ((float) (double) state.getProperty ("cancelScale"));
    if (state.hasProperty ("showGrid"))
        setShowGrid ((bool) state.getProperty ("showGrid"));
    if (state.hasProperty ("showLabels"))
        setShowLabels ((bool) state.getProperty ("showLabels"));

    refreshControlPanel();
}

void StereoMapModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("mode", "Map mode",
        { "Pan", "Mono check" },
        "Pan: where each frequency band sits in the stereo field - up = left, middle = centre, down = right. "
        "Mono check: how much of each band survives when summed to mono - top is safe, bottom is heavy cancellation.")
        .in ("View"));
    out.push_back (ParamDef::choice ("style", "Visual style",
        { "Curve", "Fill", "Columns" },
        "Curve: classic clean stereo line. Fill: translucent ribbon from the baseline. "
        "Columns: discrete per-band vertical bars.")
        .in ("View"));
    out.push_back (ParamDef::slider ("smoothing", "Response speed", 0.2, 3.0, 0.1, " x",
        "Controls how fast the stereo curve responds. Lower smooths jitter; higher catches fast panning moves.")
        .in ("View"));
    out.push_back (ParamDef::toggle ("peakHold", "Peak envelope",
        "Keep a slowly decaying outline of the widest stereo spread or worst mono cancellation per band.")
        .in ("View"));
    out.push_back (ParamDef::slider ("cancelScale", "Mono sensitivity", 20.0, 100.0, 5.0, " %",
        "Cancellation depth that reaches the bottom of the chart. Lower zooms in on subtle phase loss.")
        .in ("View").when ([this] { return mode == Mode::cancel; }));

    out.push_back (ParamDef::choice ("fftScale", "FFT scale",
        { "Log", "Mel", "Linear" },
        "How frequency is laid out left to right. Log gives every octave equal room, "
        "Mel follows pitch perception, Linear gives equal Hz per pixel.").in ("Axis"));
    out.push_back (ParamDef::choice ("freqRange", "Freq range",
        { "Full", "Music", "Vocal", "Bass" },
        "Zooms the frequency view: Full (20-20k Hz), Music (30-12k Hz), Vocal (80-8k Hz), or Bass (20-1k Hz).")
        .in ("Axis"));
    out.push_back (ParamDef::toggle ("showGrid", "Grid",
        "Show horizontal reference baselines and vertical frequency grid lines.")
        .in ("Axis"));
    out.push_back (ParamDef::toggle ("showLabels", "Labels",
        "Show L/C/R, mono %, and frequency labels. Turn OFF for a larger borderless stereo map.")
        .in ("Axis"));
}

double StereoMapModule::getParamValue (const juce::String& id) const
{
    if (id == "mode")        return mode == Mode::cancel ? 1.0 : 0.0;
    if (id == "style")       return (double) (int) visualStyle;
    if (id == "smoothing")   return (double) smoothingScale;
    if (id == "peakHold")    return peakHoldEnabled ? 1.0 : 0.0;
    if (id == "cancelScale") return (double) (cancelFullScale * 100.0f);
    if (id == "fftScale")    return (double) (int) fftScale;
    if (id == "freqRange")   return (double) (int) freqRange;
    if (id == "showGrid")    return showGrid ? 1.0 : 0.0;
    if (id == "showLabels")  return showLabels ? 1.0 : 0.0;
    return 0.0;
}

void StereoMapModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "mode")        setMode (juce::roundToInt (v) == 1 ? Mode::cancel : Mode::pan);
    else if (id == "style")       setVisualStyle ((VisualStyle) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "smoothing")   setSmoothing ((float) v);
    else if (id == "peakHold")    setPeakHold (v >= 0.5);
    else if (id == "cancelScale") setCancelScalePct ((float) v);
    else if (id == "fftScale")    setFftScale (y2k::fscale::fromIndex (juce::roundToInt (v)));
    else if (id == "freqRange")   setFreqRange ((FreqRange) juce::jlimit (0, 3, juce::roundToInt (v)));
    else if (id == "showGrid")    setShowGrid (v >= 0.5);
    else if (id == "showLabels")  setShowLabels (v >= 0.5);
}

void StereoMapModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    // 尺寸守卫：只在画布尺寸变化时重建快照（避免 layout 每帧触发重建）
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
        buildGlSnapshot();
}

void StereoMapModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    computeStereoMap (frame.dtSec);
    buildGlSnapshot();
    if (hoverActive || ! requestGlFrameOnly())
        repaintFrameContent();
}

void StereoMapModule::computeStereoMap (float dtSec)
{
    const int bandCount = juce::jlimit (96, 512, cachedBandCount > 0 ? cachedBandCount : 256);
    const float fLo = activeMinFreqHz();
    const float fHi = activeMaxFreqHz();
    hub.getSpectrumMagnitudesBlendedLR (magsL, magsR, bandCount, fLo, fHi, false, fftScale);
    if (magsL.size() != bandCount || magsR.size() != bandCount)
        return;

    const bool wantCancel = (mode == Mode::cancel);
    if (wantCancel)
    {
        hub.getSpectrumMagnitudesBlended (magsM, bandCount, fLo, fHi, false, fftScale);
        if (magsM.size() != bandCount)
            return;
    }

    std::vector<float> targetPan ((size_t) bandCount, 0.0f);
    std::vector<float> targetStrength ((size_t) bandCount, 0.0f);
    std::vector<float> targetLevelDb ((size_t) bandCount, -100.0f);
    std::vector<float> targetCancel ((size_t) bandCount, 0.0f);

    // 相对门限基于原始幅度，不吃 slope，避免低频被视觉补偿反向压制。
    float framePeakRaw = 0.0f;
    for (int i = 0; i < bandCount; ++i)
    {
        const float lRaw = juce::jmax (0.0f, magsL.getUnchecked (i));
        const float rRaw = juce::jmax (0.0f, magsR.getUnchecked (i));
        framePeakRaw = juce::jmax (framePeakRaw, juce::jmax (lRaw, rRaw));
    }

    constexpr float kPanFullScaleDb = 6.0206f; // 2:1 比值（L=2R 或 R=2L）映射到满幅

    for (int i = 0; i < bandCount; ++i)
    {
        const float t = (bandCount > 1) ? (float) i / (float) (bandCount - 1) : 0.0f;
        const float freq = freqAt (t);
        const float lRaw = juce::jmax (0.0f, magsL.getUnchecked (i));
        const float rRaw = juce::jmax (0.0f, magsR.getUnchecked (i));
        const float signalRaw = juce::jmax (lRaw, rRaw);

        // 满值映射：L/R 或 R/L = 2 时到达图表上下边界。
        const float lrDb = 20.0f * std::log10 ((lRaw + 1.0e-8f) / (rRaw + 1.0e-8f));
        const float rawPan = juce::jlimit (-1.0f, 1.0f, lrDb / kPanFullScaleDb);

        // 可见度 = Spectrum 的 4.5dB/oct slope + 只作用于亮度的高频提升。
        //   直接在 dB 域相加，省掉一次 pow/log 往返。声像位置不受影响。
        const float dbVis = 20.0f * std::log10 (juce::jmax (1.0e-8f, signalRaw))
                          + visibilityDb (freq);
        // 黑场随频率抬高（低频 -90dB → 高频 -66dB），抵掉高频增益，
        // 避免把 FFT 噪声底点亮成雾状曲线。
        const float floorDb = blackFloorDb (t);
        float strength = juce::jlimit (0.0f, 1.0f, (dbVis - floorDb) / (0.0f - floorDb));
        // 高频侧再做一次轻微提亮：中低电平更接近满值，曲线不至于断成虚点。
        strength = std::pow (strength, juce::jmap (std::sqrt (t), 1.0f, 0.78f));

        // 非线性阻尼：频带达到 -25 dB 后直接取消阻尼。
        const float dbRaw = 20.0f * std::log10 (juce::jmax (1.0e-8f, signalRaw));
        float levelActivity = 1.0f;
        if (dbRaw < -25.0f)
        {
            const float x = juce::jlimit (0.0f, 1.0f, (dbRaw + 80.0f) / 55.0f); // -80..-25 -> 0..1
            const float exp = juce::jmap (std::sqrt (t), 1.05f, 0.70f);
            levelActivity = std::pow (x, exp);
        }

        // 相对主峰门（弱带降噪），不在强带上制造额外阻尼。
        float rel = (framePeakRaw > 1.0e-8f) ? (signalRaw / framePeakRaw) : 0.0f;
        const float gate = juce::jmap (std::sqrt (t), 0.006f, 0.002f);
        rel = juce::jlimit (0.0f, 1.0f, (rel - gate) / juce::jmax (1.0e-4f, 1.0f - gate));
        const float relActivity = std::pow (rel, juce::jmap (std::sqrt (t), 0.85f, 0.45f));

        // 方向可信度下限：当 L/R 差接近满幅（约 2:1）时，即使能量偏低也要明显偏上/偏下。
        const float panConfidence = juce::jlimit (0.0f, 1.0f, std::abs (lrDb) / kPanFullScaleDb);
        const float activityFloor = juce::jmap (std::sqrt (t), 0.42f, 0.18f) * panConfidence;

        float activityFinal = juce::jmax (levelActivity * relActivity, activityFloor);
        if (dbRaw >= -25.0f)
            activityFinal = 1.0f;

        // 高频略放大偏移响应，便于直接读出"哪些频段偏左/偏右"。
        const float panBoost = juce::jmap (std::sqrt (t), 1.02f, 1.25f);
        // 强度下限：相对主峰门限对音乐素材天然惩罚高频（HF 常比低频低 20~30dB），
        //   高频侧把下限从 0.32 抬到 0.74，让可听的 HF 不被压成暗线。
        const float strengthFloor = juce::jmap (std::sqrt (t), 0.32f, 0.74f);

        targetPan[(size_t) i] = juce::jlimit (-1.0f, 1.0f, rawPan * activityFinal * panBoost);
        targetStrength[(size_t) i] = strength * juce::jmax (strengthFloor, activityFinal);
        targetLevelDb[(size_t) i] = juce::jlimit (-100.0f, 0.0f, dbRaw);

        // ---- MONO 抵消量 ----
        //   mid 路是 (L+R)/2，所以 |L+R| = 2*|mid|；
        //   参考值是"两声道不相干时"的功率和 sqrt(L²+R²)。
        //     coh = |L+R| / sqrt(L²+R²)  →  完全同相 1.41 / 不相干 1 / 反相 0
        //   抵消量 = clamp(1 - coh)：0 = mono 完全兼容，1 = 完全抵消。
        if (wantCancel)
        {
            const float midMag = juce::jmax (0.0f, magsM.getUnchecked (i)) * 2.0f;
            const float refMag = std::sqrt (lRaw * lRaw + rRaw * rRaw);
            // 参考值≈0（该频段本来就没内容）时判为完全兼容，避免静音段全屏满值。
            const float coh = (refMag > 1.0e-7f) ? (midMag / refMag) : 1.0f;
            targetCancel[(size_t) i] = juce::jlimit (0.0f, 1.0f, 1.0f - coh);
        }
    }

    const bool sizeMismatch   = ((int) panByBand.size() != bandCount)
                             || ((int) strengthByBand.size() != bandCount)
                             || ((int) levelDbByBand.size() != bandCount);
    const bool cancelMismatch = ((int) cancelByBand.size() != bandCount);

    if (sizeMismatch)
    {
        panByBand      = targetPan;
        strengthByBand = targetStrength;
        levelDbByBand  = targetLevelDb;
    }
    if (cancelMismatch)
        cancelByBand = wantCancel ? targetCancel
                                  : std::vector<float> ((size_t) bandCount, 0.0f);

    if (sizeMismatch || cancelMismatch)
        return;

    const float dt = juce::jmax (0.0f, dtSec) * smoothingScale;
    const float dtRelease = bulletTimeActive ? dt / kBulletTimeSlowdown : dt;
    const float alphaUp   = juce::jlimit (0.04f, 0.78f, dt * 12.0f);
    const float alphaDown = bulletTimeActive ? juce::jlimit (0.002f, 0.58f, dtRelease * 12.0f) : alphaUp;
    const float alphaUpC   = juce::jlimit (0.03f, 0.65f, dt * 8.0f);
    const float alphaDownC = bulletTimeActive ? juce::jlimit (0.0015f, 0.40f, dtRelease * 8.0f) : alphaUpC;

    auto follow = [] (float cur, float target, float aUp, float aDown) noexcept
    {
        return (target > cur) ? cur + (target - cur) * aUp
                              : cur + (target - cur) * aDown;
    };

    const float alphaPan = bulletTimeActive ? juce::jlimit (0.002f, 0.58f, dtRelease * 12.0f) : alphaUp;

    if (peakHoldEnabled)
    {
        if ((int) peakPanByBand.size() != bandCount)    peakPanByBand.assign ((size_t) bandCount, 0.0f);
        if ((int) peakCancelByBand.size() != bandCount) peakCancelByBand.assign ((size_t) bandCount, 0.0f);
    }
    const float peakDecay = std::exp (-juce::jmax (0.0f, dtSec) / (bulletTimeActive ? 40.0f : 1.6f));

    for (int i = 0; i < bandCount; ++i)
    {
        panByBand[(size_t) i]      += (targetPan[(size_t) i] - panByBand[(size_t) i]) * alphaPan;
        strengthByBand[(size_t) i] = follow (strengthByBand[(size_t) i], targetStrength[(size_t) i], alphaUp,  alphaDown);
        levelDbByBand[(size_t) i]  = follow (levelDbByBand[(size_t) i],  targetLevelDb[(size_t) i],  alphaUp,  alphaDown);
        if (wantCancel)
            cancelByBand[(size_t) i] = follow (cancelByBand[(size_t) i], targetCancel[(size_t) i],   alphaUpC, alphaDownC);

        if (peakHoldEnabled)
        {
            const float curP = panByBand[(size_t) i];
            float& pkP = peakPanByBand[(size_t) i];
            pkP = (std::abs (curP) >= std::abs (pkP)) ? curP : (pkP * peakDecay);

            if (wantCancel)
            {
                const float curC = cancelByBand[(size_t) i];
                float& pkC = peakCancelByBand[(size_t) i];
                pkC = (curC >= pkC) ? curC : (pkC * peakDecay);
            }
        }
    }
}

juce::Rectangle<int> StereoMapModule::canvasFromContent (juce::Rectangle<int> contentBounds) noexcept
{
    // v3.1：顶部工具条（PAN/MONO 按钮 + 说明文字）取消，画布占满内容区
    return contentBounds.reduced (2);
}

juce::Rectangle<int> StereoMapModule::canvasBounds() const
{
    return canvasFromContent (getContentBounds());
}

juce::Rectangle<int> StereoMapModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return canvasFromContent (contentBounds);
}

juce::Rectangle<int> StereoMapModule::getPlotBounds (juce::Rectangle<int> canvas) const
{
    if (! showLabels)
        return canvas.reduced (6, 6);
    return canvas.reduced (26, 12).withTrimmedBottom (14);
}

void StereoMapModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;

    const auto canvas = canvasBounds();
    if (canvas.getWidth() < 40 || canvas.getHeight() < 30)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaKey   = tokens.isChromaKey;
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool pixelNodes  = tokens.pixelSnapped;
    const bool isChrome    = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass     = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeon      = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrt       = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool coreSheen   = (isChrome || isGlass || isNeon || isCrt) && ! greenScreen;
    const float sheenWhMix = isNeon ? 0.62f : (isGlass ? 0.46f : (isCrt ? 0.50f : 0.54f));
    const float sheenAlpha = isNeon ? 0.72f : (isCrt ? 0.64f : (isGlass ? 0.42f : 0.52f));
    const bool strokeGlow  = tokens.strokeGlowPass && ! greenScreen && tokens.strokeGlowAlpha > 0.0f;
    const bool crtGlowNode = isCrt && ! greenScreen && tokens.glowIntensity > 0.0f;
    const bool neonGlowNode = isNeon && ! greenScreen && tokens.glowIntensity > 0.0f;
    const float strokeHalf = 0.9f * tokens.strokeWidthScale;
    const float glowHalf   = strokeHalf * juce::jmax (1.25f, tokens.strokeGlowWidthMul);
    const float sheenHalf  = juce::jmax (0.30f, strokeHalf * 0.36f);

    auto snap = beginGlSnapshot();
    snap->canvasLocal  = canvas;
    snap->fxRect       = canvas;
    snap->snapToPixels = false;

    appendWellFrame (snap->vertices, canvas, PinkXP::content);

    const auto plot = getPlotBounds (canvas);
    cachedBandCount = juce::jmax (64, plot.getWidth());
    if (plot.getWidth() > 4 && plot.getHeight() > 4)
    {
        const int bgStart = 0;
        const float x0 = (float) plot.getX();
        const float x1 = (float) plot.getRight();
        const float yTop = (float) plot.getY();
        const float yMid = (float) plot.getCentreY();
        const float yBot = (float) plot.getBottom();

        if (showGrid && ! chromaKey)
        {
            if (mode == Mode::pan)
            {
                const SideRamp edgeL = makeSideRamp (PinkXP::sel);
                const SideRamp edgeR = makeSideRamp (PinkXP::accent2);

                appendGridLine (snap->vertices, x0, yTop, x1, yTop, true, sampleRamp (edgeL, 0.62f));
                appendGridLine (snap->vertices, x0, yMid, x1, yMid, true, PinkXP::ink);
                appendGridLine (snap->vertices, x0, yBot, x1, yBot, true, sampleRamp (edgeR, 0.62f));
            }
            else
            {
                const SideRamp sev = makeSeverityRamp();

                appendGridLine (snap->vertices, x0, yTop, x1, yTop, true, sampleRamp (sev, 0.10f));
                appendGridLine (snap->vertices, x0, yMid, x1, yMid, true, PinkXP::ink);
                appendGridLine (snap->vertices, x0, yBot, x1, yBot, true, sampleRamp (sev, 0.98f));
            }

            std::vector<float> gridLines, gridLabels;
            gridFreqs (gridLines, gridLabels);
            const bool darkBg = PinkXP::content.getPerceivedBrightness() < 0.5f;
            const auto etchTickC = PinkXP::hl.interpolatedWith (PinkXP::ink, darkBg ? 0.25f : 0.40f)
                                       .withAlpha (darkBg ? 0.36f : 0.28f);
            const auto crtTickC  = PinkXP::sel.interpolatedWith (PinkXP::pink400, 0.40f).withAlpha (0.48f);
            for (const float f : gridLines)
            {
                const float x = xForNorm (normFromFreq (f), plot);
                appendGridLine (snap->vertices, x, yTop, x, yBot, false, PinkXP::pink300);
                if (isChrome || isCrt)
                {
                    const float halfH = isCrt ? 3.0f : 2.5f;
                    const float ty = (mode == Mode::pan) ? (yMid - halfH) : yTop;
                    appendQuad (snap->vertices, { std::floor (x), ty, 1.0f, halfH * 2.0f },
                                isCrt ? crtTickC : etchTickC);
                }
            }
        }

        const int bgCount = (int) snap->vertices.size() / 6 - bgStart;
        if (bgCount > 0)
            snap->segments.push_back ({ bgStart, bgCount, Primitive::Triangles });

        if (mode == Mode::pan)
        {
            const int n = (int) panByBand.size();
            if (n > 1)
            {
                const int start = (int) snap->vertices.size() / 6;
                const float cy = (float) plot.getCentreY();
                const float halfH = (float) juce::jmax (1, plot.getHeight() / 2);

                const SideRamp leftRamp  = makeSideRamp (PinkXP::sel);
                const SideRamp rightRamp = makeSideRamp (PinkXP::accent2);

                if (visualStyle == VisualStyle::bars)
                {
                    const int step = juce::jmax (2, n / juce::jmax (16, plot.getWidth() / 4));
                    const float barW = juce::jmax (1.5f, (float) plot.getWidth() / (float) (n / step + 1) - (float) tokens.defaultCellGapPx);
                    for (int i = 0; i < n; i += step)
                    {
                        const float t0  = (float) i / (float) (n - 1);
                        const float x   = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
                        const float pan = juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]);
                        const float s   = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                        const float a   = std::pow (s, 0.92f);
                        if (a < 0.02f) continue;
                        const float y = cy - pan * halfH;
                        float bx = x - barW * 0.5f;
                        float bt = juce::jmin (cy, y);
                        float bw = barW;
                        float bh = juce::jmax (1.5f, std::abs (y - cy));
                        if (pixelNodes)
                        {
                            bx = std::floor (bx);
                            bt = std::floor (bt);
                            bw = juce::jmax (1.0f, std::round (bw));
                            bh = juce::jmax (2.0f, std::round (bh));
                        }
                        appendStyledBarCell (snap->vertices, { bx, bt, bw, bh },
                                             stereoMapColour (leftRamp, rightRamp, pan, s).withAlpha (chromaKey ? juce::jmin (1.0f, a + 0.25f) : a * 0.88f),
                                             true, 0.0f);
                    }
                }
                else
                {
                    for (int i = 0; i < n - 1; ++i)
                    {
                        const float t0 = (float) i / (float) (n - 1);
                        const float t1 = (float) (i + 1) / (float) (n - 1);

                        const float xA = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
                        const float xB = juce::jmap (t1, (float) plot.getX(), (float) plot.getRight());

                        const float panA = juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]);
                        const float panB = juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) (i + 1)]);
                        const float yA = cy - panA * halfH;
                        const float yB = cy - panB * halfH;

                        const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                        const float a = std::pow (s, 0.92f);
                        if (a < 0.02f)
                            continue;

                        const float panSeg = 0.5f * (panA + panB);
                        const auto segCol = stereoMapColour (leftRamp, rightRamp, panSeg, s);
                        if (visualStyle == VisualStyle::fill)
                            appendFilledBand (snap->vertices, xA, yA, xB, yB, cy,
                                              segCol.withAlpha (a * 0.42f * tokens.areaFillAlphaScale));

                        if (strokeGlow)
                            appendLineSegment (snap->vertices, xA, yA, xB, yB, glowHalf,
                                               segCol.withAlpha (a * tokens.strokeGlowAlpha));

                        appendLineSegment (snap->vertices, xA, yA, xB, yB, strokeHalf, segCol.withAlpha (a));
                        if (coreSheen)
                            appendLineSegment (snap->vertices, xA, yA, xB, yB, sheenHalf,
                                               segCol.interpolatedWith (juce::Colours::white, sheenWhMix).withAlpha (a * sheenAlpha));

                        if (visualStyle == VisualStyle::curve && (i % 3) == 0)
                        {
                            const auto nodeCol = stereoMapColour (leftRamp, rightRamp, panSeg,
                                                                  juce::jmin (1.0f, s + 0.20f)).withAlpha (chromaKey ? 1.0f : a);
                            if (pixelNodes)
                            {
                                const float nx = std::floor (xA) - 1.0f;
                                const float ny = std::floor (yA) - 1.0f;
                                if (crtGlowNode)
                                {
                                    appendQuad (snap->vertices, { nx - 1.0f, ny - 1.0f, 5.0f, 5.0f },
                                                nodeCol.withMultipliedAlpha (0.28f * tokens.glowIntensity));
                                    appendQuad (snap->vertices, { nx - 1.0f, ny + 1.0f, 5.0f, 1.0f },
                                                nodeCol.withMultipliedAlpha (0.55f));
                                    appendQuad (snap->vertices, { nx + 1.0f, ny - 1.0f, 1.0f, 5.0f },
                                                nodeCol.withMultipliedAlpha (0.55f));
                                }
                                appendQuad (snap->vertices, { nx, ny, 3.0f, 3.0f }, nodeCol);
                                if (crtGlowNode)
                                    appendQuad (snap->vertices, { nx + 1.0f, ny + 1.0f, 1.0f, 1.0f },
                                                nodeCol.interpolatedWith (juce::Colours::white, 0.58f));
                            }
                            else if (isChrome)
                            {
                                // Millennium Chrome：精雕深切金属座圈 + OKLab 宝石芯 + 亮镜面中心销
                                const float na = nodeCol.getFloatAlpha();
                                appendDisc (snap->vertices, xA, yA, 2.05f,
                                            PinkXP::dark.withAlpha (greenScreen ? 1.0f : na * 0.78f), 12);
                                appendDisc (snap->vertices, xA, yA, 1.55f, nodeCol, 10);
                                const float pinOff = greenScreen ? 0.0f : -0.25f;
                                appendDisc (snap->vertices, xA + pinOff, yA + pinOff, 0.62f,
                                            juce::Colours::white.withAlpha (greenScreen ? 1.0f : na * 0.88f), 8);
                            }
                            else if (isGlass)
                            {
                                // Frosted Glass：柔和晶莹水晶珠节点（柔光晕 + OKLab 水晶核 + 左上高光点）
                                const float na = nodeCol.getFloatAlpha();
                                if (! greenScreen)
                                    appendDisc (snap->vertices, xA, yA, 2.10f, nodeCol.withAlpha (na * 0.18f), 10);
                                appendDisc (snap->vertices, xA, yA, 1.48f, nodeCol, 10);
                                if (! greenScreen)
                                    appendDisc (snap->vertices, xA - 0.35f, yA - 0.35f, 0.55f,
                                                juce::Colours::white.withAlpha (na * 0.74f), 8);
                            }
                            else if (isNeon)
                            {
                                // Neon Cyber：发光霓虹空心环节点 + 炽白中心点
                                const float na = nodeCol.getFloatAlpha();
                                if (neonGlowNode)
                                    appendDisc (snap->vertices, xA, yA, 2.95f,
                                                nodeCol.withMultipliedAlpha (0.30f * tokens.glowIntensity), 10);
                                appendDisc (snap->vertices, xA, yA, 1.80f, nodeCol, 10);
                                if (! greenScreen)
                                    appendDisc (snap->vertices, xA, yA, 0.98f, PinkXP::dark.withAlpha (na * 0.90f), 8);
                                appendDisc (snap->vertices, xA, yA, 0.52f,
                                            nodeCol.interpolatedWith (juce::Colours::white, 0.72f), 8);
                            }
                            else
                            {
                                appendDisc (snap->vertices, xA, yA, 1.35f, nodeCol, 10);
                            }
                        }
                    }
                }

                if (peakHoldEnabled && (int) peakPanByBand.size() == n)
                {
                    std::vector<juce::Point<float>> pkPts;
                    pkPts.reserve ((size_t) n);
                    for (int i = 0; i < n; ++i)
                    {
                        const float px = juce::jmap ((float) i / (float) (n - 1), (float) plot.getX(), (float) plot.getRight());
                        const float py = cy - juce::jlimit (-1.0f, 1.0f, peakPanByBand[(size_t) i]) * halfH;
                        pkPts.push_back ({ px, py });
                    }
                    appendStyledPolyline (snap->vertices, pkPts.data(), (int) pkPts.size(),
                                          PinkXP::ink.withAlpha (0.55f), 1.1f);
                }

                const int count = (int) snap->vertices.size() / 6 - start;
                if (count > 0)
                    snap->segments.push_back ({ start, count, Primitive::Triangles });
            }
        }
        else
        {
            const int n = (int) cancelByBand.size();
            if (n > 1)
            {
                const int start = (int) snap->vertices.size() / 6;
                const SideRamp sev = makeSeverityRamp();
                const float baseY = (float) plot.getY() + 1.0f;
                const float spanH = (float) juce::jmax (1, plot.getHeight() - 1);

                if (visualStyle == VisualStyle::bars)
                {
                    const int step = juce::jmax (2, n / juce::jmax (16, plot.getWidth() / 4));
                    const float barW = juce::jmax (1.5f, (float) plot.getWidth() / (float) (n / step + 1) - (float) tokens.defaultCellGapPx);
                    for (int i = 0; i < n; i += step)
                    {
                        const float t0 = (float) i / (float) (n - 1);
                        const float x  = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
                        const float cA = cancelToDisplay (cancelByBand[(size_t) i]);
                        const float s  = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                        const float a  = std::pow (s, 0.88f);
                        if (a < 0.02f) continue;
                        float bx = x - barW * 0.5f;
                        float by = baseY;
                        float bw = barW;
                        float bh = juce::jmax (1.5f, cA * spanH);
                        if (pixelNodes)
                        {
                            bx = std::floor (bx);
                            by = std::floor (by);
                            bw = juce::jmax (1.0f, std::round (bw));
                            bh = juce::jmax (2.0f, std::round (bh));
                        }
                        appendStyledBarCell (snap->vertices, { bx, by, bw, bh },
                                             sampleRamp (sev, cA).withAlpha (chromaKey ? juce::jmin (1.0f, a + 0.25f) : a * 0.82f),
                                             true, 0.0f);
                    }
                }
                else
                {
                    const float fillAlpha = ((visualStyle == VisualStyle::fill) ? 0.68f : 0.45f) * tokens.areaFillAlphaScale;
                    for (int i = 0; i < n - 1; ++i)
                    {
                        const float t0 = (float) i / (float) (n - 1);
                        const float t1 = (float) (i + 1) / (float) (n - 1);
                        const float xA = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
                        const float xB = juce::jmap (t1, (float) plot.getX(), (float) plot.getRight());

                        const float cA = cancelToDisplay (cancelByBand[(size_t) i]);
                        const float cB = cancelToDisplay (cancelByBand[(size_t) (i + 1)]);
                        const float yA = baseY + cA * spanH;
                        const float yB = baseY + cB * spanH;

                        const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                        const float a = std::pow (s, 0.88f);
                        if (a < 0.02f)
                            continue;

                        const juce::Colour col = sampleRamp (sev, 0.5f * (cA + cB));
                        appendFilledBand (snap->vertices, xA, yA, xB, yB, baseY,
                                          col.withAlpha (a * fillAlpha));
                        const float lineA = juce::jmin (1.0f, a * 1.25f);
                        if (strokeGlow)
                            appendLineSegment (snap->vertices, xA, yA, xB, yB, glowHalf,
                                               col.withAlpha (lineA * tokens.strokeGlowAlpha));
                        appendLineSegment (snap->vertices, xA, yA, xB, yB, strokeHalf,
                                           col.withAlpha (lineA));
                        if (coreSheen)
                            appendLineSegment (snap->vertices, xA, yA, xB, yB, sheenHalf,
                                               col.interpolatedWith (juce::Colours::white, sheenWhMix).withAlpha (lineA * sheenAlpha));
                    }
                }

                if (peakHoldEnabled && (int) peakCancelByBand.size() == n)
                {
                    std::vector<juce::Point<float>> pkPts;
                    pkPts.reserve ((size_t) n);
                    for (int i = 0; i < n; ++i)
                    {
                        const float px = juce::jmap ((float) i / (float) (n - 1), (float) plot.getX(), (float) plot.getRight());
                        const float py = baseY + cancelToDisplay (peakCancelByBand[(size_t) i]) * spanH;
                        pkPts.push_back ({ px, py });
                    }
                    appendStyledPolyline (snap->vertices, pkPts.data(), (int) pkPts.size(),
                                          PinkXP::ink.withAlpha (0.62f), 1.1f);
                }

                const int count = (int) snap->vertices.size() / 6 - start;
                if (count > 0)
                    snap->segments.push_back ({ start, count, Primitive::Triangles });
            }
        }
    }

    if (snap->segments.empty())
        snap->segments.push_back ({ 0, (int) snap->vertices.size() / 6, Primitive::Triangles });

    publishGlSnapshot (std::move (snap));
}

void StereoMapModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    const auto plot = getPlotBounds (canvas);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    // 绿幕：隐藏所有静态坐标轴/频率标签（悬停标尺仍保留）
    if (! PinkXP::isChromaKeySkin() && showLabels)
    {
        if (mode == Mode::pan)
        {
            const juce::Colour labelL = sampleRamp (makeSideRamp (PinkXP::sel),     0.72f);
            const juce::Colour labelR = sampleRamp (makeSideRamp (PinkXP::accent2), 0.72f);

            g.setFont (PinkXP::getAxisFont (10.0f, juce::Font::bold));
            g.setColour (labelL.withAlpha (0.85f));
            g.drawText (y2k::tr ("L"), plot.getX() - 18, plot.getY() - 2, 14, 12, juce::Justification::centredRight, false);
            g.setColour (PinkXP::ink.withAlpha (0.72f));
            g.drawText (y2k::tr ("C"), plot.getX() - 18, plot.getCentreY() - 6, 14, 12, juce::Justification::centredRight, false);
            g.setColour (labelR.withAlpha (0.85f));
            g.drawText (y2k::tr ("R"), plot.getX() - 18, plot.getBottom() - 10, 14, 12, juce::Justification::centredRight, false);
        }
        else
        {
            const SideRamp sev = makeSeverityRamp();
            const int midPct = juce::roundToInt ((1.0f - cancelFullScale * 0.5f) * 100.0f);
            const int botPct = juce::roundToInt ((1.0f - cancelFullScale) * 100.0f);

            g.setFont (PinkXP::getAxisFont (8.0f, juce::Font::plain));

            g.setColour (sampleRamp (sev, 0.10f).withAlpha (0.80f));
            g.drawText (y2k::tr ("100%"), plot.getX() - 25, plot.getY() - 5, 24, 11,
                        juce::Justification::centredRight, false);
            g.setColour (PinkXP::ink.withAlpha (0.62f));
            g.drawText (juce::String (midPct) + "%", plot.getX() - 25, plot.getCentreY() - 5, 24, 11,
                        juce::Justification::centredRight, false);
            g.setColour (sampleRamp (sev, 0.90f).withAlpha (0.85f));
            g.drawText (juce::String (botPct) + "%", plot.getX() - 25, plot.getBottom() - 6, 24, 11,
                        juce::Justification::centredRight, false);
        }

        g.setColour (PinkXP::ink.withAlpha (0.58f));
        g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
        std::vector<float> gridLines, gridLabels;
        gridFreqs (gridLines, gridLabels);
        int lastDrawnX = -9999;
        for (const float f : gridLabels)
        {
            const float x = xForNorm (normFromFreq (f), plot);
            const int ix = juce::roundToInt (x);
            if (ix - lastDrawnX < 24) continue;
            lastDrawnX = ix;
            juce::String label;
            if (f >= 1000.0f)
                label = juce::String (f / 1000.0f, (std::fmod (f, 1000.0f) < 1.0f) ? 0 : 1) + "k";
            else
                label = juce::String ((int) f);

            g.drawText (y2k::tr (label),
                        ix - 14,
                        plot.getBottom() + 2,
                        28,
                        12,
                        juce::Justification::centred,
                        false);
        }
    }

    // 悬停标尺：paintGlOverlay 同时服务 GL 接管态与 CPU 回退态
    //   （paintLegacyContent 末尾会调用本函数），放在这里两条路径都生效。
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

// ==========================================================
// 鼠标悬停标尺 / 点击读数
// ==========================================================
void StereoMapModule::mouseMove (const juce::MouseEvent& e)
{
    ModulePanel::mouseMove (e);

    const auto canvas = canvasBounds();
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains (pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (canvas);
    }
}

void StereoMapModule::mouseDrag (const juce::MouseEvent& e)
{
    ModulePanel::mouseDrag (e);

    // 按住拖动期间 JUCE 只派发 mouseDrag（mouseMove 被抑制），不同步的话
    //   十字线与读数会停在按下那一刻的位置不动。
    const auto canvas = canvasBounds();
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains (pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (canvas);
    }
}

void StereoMapModule::mouseDown (const juce::MouseEvent& e)
{
    ModulePanel::mouseDown (e);   // 保留基类行为（toFront / 右键菜单 / 边缘拖拽）

    // 只在"左键按在画布内部（避开边缘缩放热区与抽屉）"时进入子弹时间
    if (e.mods.isLeftButtonDown()
        && canvasBounds().contains (e.getPosition())
        && ! getControlDrawerBounds().contains (e.getPosition()))
    {
        bulletTimeActive = true;
        repaint();
    }
}

void StereoMapModule::mouseUp (const juce::MouseEvent& e)
{
    ModulePanel::mouseUp (e);

    // 松手即恢复（按下位置不在画布时这里只是无害地写一次 false）
    if (bulletTimeActive)
    {
        bulletTimeActive = false;
        repaint();   // 去掉"按住才有"的音名读数，不等下一帧
    }
}

void StereoMapModule::mouseExit (const juce::MouseEvent& e)
{
    ModulePanel::mouseExit (e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在画布内，擦除只需失效画布（不走浮动态限流）
        repaint (canvasBounds());
    }
}

void StereoMapModule::drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (! hoverActive || ! canvas.contains (hoverPos))
        return;

    const auto plot = getPlotBounds (canvas);
    const auto readout = makeCoordReadout (hoverPos, plot);
    if (readout.isEmpty())
        return;

    PinkXP::drawHoverRuler (g, canvas, hoverPos, readout);
}

// 悬停态读数：横轴频率 + 鼠标所在高度的纵轴坐标（与 Spectrum 的"频率 + dBFS"同构）
juce::String StereoMapModule::makeCoordReadout (juce::Point<int> pos, juce::Rectangle<int> plot) const
{
    if (plot.getWidth() <= 1 || plot.getHeight() <= 1)
        return {};

    const float freq = freqAt (normForX ((float) pos.x, plot));

    // 纵轴读数：PAN = 声像 ±1；MONO = 单声道兼容度（顶 100% / 底 0%，
    //   与倒置后的曲线高度同向 —— 越往下越糟）
    const juce::String yText = (mode == Mode::pan)
        ? formatPan (panForY ((float) pos.y, plot))
        : formatPercent (compatForY ((float) pos.y, plot));

    juce::String text = PinkXP::formatFreqHz (freq) + "  " + yText;

    // 悬停即显示当前频率坐标对应的音名
    const juce::String note = PinkXP::noteNameForHz (freq);
    if (note.isNotEmpty())
        text += "  " + note;

    return text;
}

// 点击态读数：该频率上的真实数据（平滑后的声像 / 抵消量 + 原始电平）
juce::String StereoMapModule::makeDataReadout (float x, juce::Rectangle<int> plot) const
{
    const int idx = bandIndexForX (x, plot);
    if (idx < 0)
        return {};

    const float freq = freqAt (normForX (x, plot));

    juce::String text = PinkXP::formatFreqHz (freq);
    if (mode == Mode::pan)
    {
        text << "  " << formatPan (juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) idx]));
    }
    else if ((int) cancelByBand.size() > idx)
    {
        // 与纵轴同一套刻度：mono 保留量 = 1 - 真实抵消量（100% = 完全兼容）
        const float keep = 1.0f - juce::jlimit (0.0f, 1.0f, cancelByBand[(size_t) idx]);
        text << "  " << formatPercent (keep) << " mono";
    }

    if ((int) levelDbByBand.size() > idx)
    {
        const float db = levelDbByBand[(size_t) idx];
        if (db > -99.0f)
            text << "  " << juce::String (db, 1) << " dB";
    }
    return text;
}

int StereoMapModule::bandIndexForX (float x, juce::Rectangle<int> plot) const
{
    const int n = (int) panByBand.size();
    if (n <= 0 || plot.getWidth() <= 1)
        return -1;

    const float t = normForX (x, plot);
    return juce::jlimit (0, n - 1, juce::roundToInt (t * (float) (n - 1)));
}

// ---- Toybox：点击绘图区 → 该频段的读数气泡 ----
bool StereoMapModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = canvasBounds();
    if (! canvas.contains (panelPos))
        return false;

    out.kind = 1;
    out.rect = getPlotBounds (canvas).toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void StereoMapModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const auto canvas = canvasBounds();
    const auto plot   = getPlotBounds (canvas);
    if (plot.getWidth() < 8 || plot.getHeight() < 8)
        return;

    juce::Component::SafePointer<StereoMapModule> safe (this);
    auto isSlowMo = [safe]() -> bool
    {
        return safe != nullptr && safe->bulletTimeActive;
    };

    ctx.addModuleActorCapped (
        makeStereoMapBeamActor (hit.pos, plot, mode == Mode::cancel, std::move (isSlowMo)),
        6);
}

void StereoMapModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
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
    if (! showGrid)
        return;

    const auto plot = getPlotBounds (canvas);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    const float x0 = (float) plot.getX();
    const float x1 = (float) plot.getRight();
    const float yTop = (float) plot.getY();
    const float yMid = (float) plot.getCentreY();
    const float yBot = (float) plot.getBottom();

    if (mode == Mode::pan)
    {
        const SideRamp edgeL = makeSideRamp (PinkXP::sel);
        const SideRamp edgeR = makeSideRamp (PinkXP::accent2);

        PinkXP::Craft::drawGridLine (g, x0, yTop, x1, yTop, true, sampleRamp (edgeL, 0.62f));
        PinkXP::Craft::drawGridLine (g, x0, yBot, x1, yBot, true, sampleRamp (edgeR, 0.62f));
    }
    else
    {
        const SideRamp sev = makeSeverityRamp();

        // 倒置后：顶 = 兼容（色阶低端）/ 底 = 满幅抵消（色阶高端）
        PinkXP::Craft::drawGridLine (g, x0, yTop, x1, yTop, true, sampleRamp (sev, 0.10f));
        PinkXP::Craft::drawGridLine (g, x0, yBot, x1, yBot, true, sampleRamp (sev, 0.98f));
    }

    PinkXP::Craft::drawGridLine (g, x0, yMid, x1, yMid, true, PinkXP::ink);

    std::vector<float> bgLines, bgLabels;
    gridFreqs (bgLines, bgLabels);
    const auto& tokens = PinkXP::getSkinTokens();
    const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;
    const auto etchTickC = PinkXP::hl.interpolatedWith (PinkXP::ink, darkBg ? 0.25f : 0.40f)
                               .withAlpha (darkBg ? 0.36f : 0.28f);
    const auto crtTickC  = PinkXP::sel.interpolatedWith (PinkXP::pink400, 0.40f).withAlpha (0.48f);
    for (const float f : bgLines)
    {
        const float x = xForNorm (normFromFreq (f), plot);
        PinkXP::Craft::drawGridLine (g, x, yTop, x, yBot, false, PinkXP::pink300);
        if (isChrome || isCrt)
        {
            const float halfH = isCrt ? 3.0f : 2.5f;
            const float ty = (mode == Mode::pan) ? (yMid - halfH) : yTop;
            g.setColour (isCrt ? crtTickC : etchTickC);
            g.fillRect (std::floor (x), ty, 1.0f, halfH * 2.0f);
        }
    }
}

void StereoMapModule::drawCurve (juce::Graphics& g, juce::Rectangle<int> plot) const
{
    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaKey   = tokens.isChromaKey;
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool pixelNodes  = tokens.pixelSnapped;
    const bool isChrome    = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass     = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeon      = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrt       = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool coreSheen   = (isChrome || isGlass || isNeon || isCrt) && ! greenScreen;
    const float sheenWhMix = isNeon ? 0.62f : (isGlass ? 0.46f : (isCrt ? 0.50f : 0.54f));
    const float sheenAlpha = isNeon ? 0.72f : (isCrt ? 0.64f : (isGlass ? 0.42f : 0.52f));
    const bool strokeGlow  = tokens.strokeGlowPass && ! greenScreen && tokens.strokeGlowAlpha > 0.0f;
    const bool crtGlowNode = isCrt && ! greenScreen && tokens.glowIntensity > 0.0f;
    const bool neonGlowNode = isNeon && ! greenScreen && tokens.glowIntensity > 0.0f;
    const float lineW  = 1.25f * tokens.strokeWidthScale;
    const float glowW  = lineW * juce::jmax (1.25f, tokens.strokeGlowWidthMul);
    const float sheenW = juce::jmax (0.55f, lineW * 0.36f);

    if (mode == Mode::cancel)
    {
        const int n = (int) cancelByBand.size();
        if (n <= 1)
            return;

        const SideRamp sev = makeSeverityRamp();
        const float baseY = (float) plot.getY() + 1.0f;
        const float spanH = (float) juce::jmax (1, plot.getHeight() - 1);

        if (visualStyle == VisualStyle::bars)
        {
            const int step = juce::jmax (2, n / juce::jmax (16, plot.getWidth() / 4));
            const float barW = juce::jmax (1.5f, (float) plot.getWidth() / (float) (n / step + 1) - (float) tokens.defaultCellGapPx);
            for (int i = 0; i < n; i += step)
            {
                const float x  = juce::jmap ((float) i / (float) (n - 1), (float) plot.getX(), (float) plot.getRight());
                const float cA = cancelToDisplay (cancelByBand[(size_t) i]);
                const float s  = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                const float a  = std::pow (s, 0.88f);
                if (a < 0.02f) continue;
                float bx = x - barW * 0.5f;
                float by = baseY;
                float bw = barW;
                float bh = juce::jmax (1.5f, cA * spanH);
                if (pixelNodes)
                {
                    bx = std::floor (bx);
                    by = std::floor (by);
                    bw = juce::jmax (1.0f, std::round (bw));
                    bh = juce::jmax (2.0f, std::round (bh));
                }
                PinkXP::Craft::drawMeterCell (g, juce::Rectangle<float> (bx, by, bw, bh),
                                              sampleRamp (sev, cA).withAlpha (chromaKey ? juce::jmin (1.0f, a + 0.25f) : a * 0.82f),
                                              true, 0.0f);
            }
        }
        else
        {
            const float fillAlpha = ((visualStyle == VisualStyle::fill) ? 0.68f : 0.45f) * tokens.areaFillAlphaScale;
            for (int i = 0; i < n - 1; ++i)
            {
                const float t0 = (float) i / (float) (n - 1);
                const float t1 = (float) (i + 1) / (float) (n - 1);
                const float xA = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
                const float xB = juce::jmap (t1, (float) plot.getX(), (float) plot.getRight());

                const float cA = cancelToDisplay (cancelByBand[(size_t) i]);
                const float cB = cancelToDisplay (cancelByBand[(size_t) (i + 1)]);
                const float yA = baseY + cA * spanH;
                const float yB = baseY + cB * spanH;

                const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                const float a = std::pow (s, 0.88f);
                if (a < 0.02f)
                    continue;

                const juce::Colour col = sampleRamp (sev, 0.5f * (cA + cB));

                juce::Path p;
                p.startNewSubPath (xA, baseY);
                p.lineTo (xB, baseY);
                p.lineTo (xB, yB);
                p.lineTo (xA, yA);
                p.closeSubPath();

                g.setColour (col.withAlpha (a * fillAlpha));
                g.fillPath (p);
                const float lineA = juce::jmin (1.0f, a * 1.25f);
                if (strokeGlow)
                {
                    g.setColour (col.withAlpha (lineA * tokens.strokeGlowAlpha));
                    g.drawLine (xA, yA, xB, yB, glowW);
                }
                g.setColour (col.withAlpha (lineA));
                g.drawLine (xA, yA, xB, yB, lineW);
                if (coreSheen)
                {
                    g.setColour (col.interpolatedWith (juce::Colours::white, sheenWhMix).withAlpha (lineA * sheenAlpha));
                    g.drawLine (xA, yA, xB, yB, sheenW);
                }
            }
        }

        if (peakHoldEnabled && (int) peakCancelByBand.size() == n)
        {
            juce::Path pkPath;
            for (int i = 0; i < n; ++i)
            {
                const float px = juce::jmap ((float) i / (float) (n - 1), (float) plot.getX(), (float) plot.getRight());
                const float py = baseY + cancelToDisplay (peakCancelByBand[(size_t) i]) * spanH;
                if (i == 0) pkPath.startNewSubPath (px, py);
                else        pkPath.lineTo (px, py);
            }
            PinkXP::Craft::drawStyledStroke (g, pkPath, PinkXP::ink.withAlpha (0.62f), 1.0f);
        }
        return;
    }

    const int n = (int) panByBand.size();
    if (n <= 1)
        return;

    const float cy = (float) plot.getCentreY();
    const float halfH = (float) juce::jmax (1, plot.getHeight() / 2);

    const SideRamp leftRamp  = makeSideRamp (PinkXP::sel);
    const SideRamp rightRamp = makeSideRamp (PinkXP::accent2);

    if (visualStyle == VisualStyle::bars)
    {
        const int step = juce::jmax (2, n / juce::jmax (16, plot.getWidth() / 4));
        const float barW = juce::jmax (1.5f, (float) plot.getWidth() / (float) (n / step + 1) - (float) tokens.defaultCellGapPx);
        for (int i = 0; i < n; i += step)
        {
            const float x   = juce::jmap ((float) i / (float) (n - 1), (float) plot.getX(), (float) plot.getRight());
            const float pan = juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]);
            const float s   = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
            const float a   = std::pow (s, 0.92f);
            if (a < 0.02f) continue;
            const float y = cy - pan * halfH;
            float bx = x - barW * 0.5f;
            float bt = juce::jmin (cy, y);
            float bw = barW;
            float bh = juce::jmax (1.5f, std::abs (y - cy));
            if (pixelNodes)
            {
                bx = std::floor (bx);
                bt = std::floor (bt);
                bw = juce::jmax (1.0f, std::round (bw));
                bh = juce::jmax (2.0f, std::round (bh));
            }
            PinkXP::Craft::drawMeterCell (g, juce::Rectangle<float> (bx, bt, bw, bh),
                                          stereoMapColour (leftRamp, rightRamp, pan, s).withAlpha (chromaKey ? juce::jmin (1.0f, a + 0.25f) : a * 0.88f),
                                          true, 0.0f);
        }
    }
    else
    {
        for (int i = 0; i < n - 1; ++i)
        {
            const float t0 = (float) i / (float) (n - 1);
            const float t1 = (float) (i + 1) / (float) (n - 1);
            const float xA = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
            const float xB = juce::jmap (t1, (float) plot.getX(), (float) plot.getRight());
            const float panA = juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]);
            const float panB = juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) (i + 1)]);
            const float yA = cy - panA * halfH;
            const float yB = cy - panB * halfH;

            const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
            const float a = std::pow (s, 0.92f);
            if (a < 0.02f)
                continue;

            const auto col = stereoMapColour (leftRamp, rightRamp, 0.5f * (panA + panB), s);
            if (visualStyle == VisualStyle::fill)
            {
                juce::Path p;
                p.startNewSubPath (xA, cy);
                p.lineTo (xB, cy);
                p.lineTo (xB, yB);
                p.lineTo (xA, yA);
                p.closeSubPath();
                g.setColour (col.withAlpha (a * 0.42f * tokens.areaFillAlphaScale));
                g.fillPath (p);
            }
            if (strokeGlow)
            {
                g.setColour (col.withAlpha (a * tokens.strokeGlowAlpha));
                g.drawLine (xA, yA, xB, yB, glowW);
            }
            g.setColour (col.withAlpha (a));
            g.drawLine (xA, yA, xB, yB, lineW);
            if (coreSheen)
            {
                g.setColour (col.interpolatedWith (juce::Colours::white, sheenWhMix).withAlpha (a * sheenAlpha));
                g.drawLine (xA, yA, xB, yB, sheenW);
            }
        }

        if (visualStyle == VisualStyle::curve)
        {
            for (int i = 0; i < n; i += 3)
            {
                const float t = (float) i / (float) (n - 1);
                const float x = juce::jmap (t, (float) plot.getX(), (float) plot.getRight());
                const float pan = juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]);
                const float y = cy - pan * halfH;
                const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                const float a = std::pow (s, 0.92f);
                if (a < 0.03f)
                    continue;
                const auto nodeCol = stereoMapColour (leftRamp, rightRamp, pan,
                                                      juce::jmin (1.0f, s + 0.20f)).withAlpha (chromaKey ? 1.0f : a);
                if (pixelNodes)
                {
                    const float nx = std::floor (x) - 1.0f;
                    const float ny = std::floor (y) - 1.0f;
                    if (crtGlowNode)
                    {
                        g.setColour (nodeCol.withMultipliedAlpha (0.28f * tokens.glowIntensity));
                        g.fillRect (nx - 1.0f, ny - 1.0f, 5.0f, 5.0f);
                        g.setColour (nodeCol.withMultipliedAlpha (0.55f));
                        g.fillRect (nx - 1.0f, ny + 1.0f, 5.0f, 1.0f);
                        g.fillRect (nx + 1.0f, ny - 1.0f, 1.0f, 5.0f);
                    }
                    g.setColour (nodeCol);
                    g.fillRect (nx, ny, 3.0f, 3.0f);
                    if (crtGlowNode)
                    {
                        g.setColour (nodeCol.interpolatedWith (juce::Colours::white, 0.58f));
                        g.fillRect (nx + 1.0f, ny + 1.0f, 1.0f, 1.0f);
                    }
                }
                else if (isChrome)
                {
                    const float na = nodeCol.getFloatAlpha();
                    g.setColour (PinkXP::dark.withAlpha (greenScreen ? 1.0f : na * 0.78f));
                    g.fillEllipse (x - 2.05f, y - 2.05f, 4.10f, 4.10f);
                    g.setColour (nodeCol);
                    g.fillEllipse (x - 1.55f, y - 1.55f, 3.10f, 3.10f);
                    const float pinOff = greenScreen ? 0.0f : -0.25f;
                    g.setColour (juce::Colours::white.withAlpha (greenScreen ? 1.0f : na * 0.88f));
                    g.fillEllipse (x + pinOff - 0.62f, y + pinOff - 0.62f, 1.24f, 1.24f);
                }
                else if (isGlass)
                {
                    const float na = nodeCol.getFloatAlpha();
                    if (! greenScreen)
                    {
                        g.setColour (nodeCol.withAlpha (na * 0.18f));
                        g.fillEllipse (x - 2.10f, y - 2.10f, 4.20f, 4.20f);
                    }
                    g.setColour (nodeCol);
                    g.fillEllipse (x - 1.48f, y - 1.48f, 2.96f, 2.96f);
                    if (! greenScreen)
                    {
                        g.setColour (juce::Colours::white.withAlpha (na * 0.74f));
                        g.fillEllipse (x - 0.35f - 0.55f, y - 0.35f - 0.55f, 1.10f, 1.10f);
                    }
                }
                else if (isNeon)
                {
                    const float na = nodeCol.getFloatAlpha();
                    if (neonGlowNode)
                    {
                        g.setColour (nodeCol.withMultipliedAlpha (0.30f * tokens.glowIntensity));
                        g.fillEllipse (x - 2.95f, y - 2.95f, 5.90f, 5.90f);
                    }
                    g.setColour (nodeCol);
                    g.fillEllipse (x - 1.80f, y - 1.80f, 3.60f, 3.60f);
                    if (! greenScreen)
                    {
                        g.setColour (PinkXP::dark.withAlpha (na * 0.90f));
                        g.fillEllipse (x - 0.98f, y - 0.98f, 1.96f, 1.96f);
                    }
                    g.setColour (nodeCol.interpolatedWith (juce::Colours::white, 0.72f));
                    g.fillEllipse (x - 0.52f, y - 0.52f, 1.04f, 1.04f);
                }
                else
                {
                    g.setColour (nodeCol);
                    g.fillEllipse (x - 1.35f, y - 1.35f, 2.7f, 2.7f);
                }
            }
        }
    }

    if (peakHoldEnabled && (int) peakPanByBand.size() == n)
    {
        juce::Path pkPath;
        for (int i = 0; i < n; ++i)
        {
            const float px = juce::jmap ((float) i / (float) (n - 1), (float) plot.getX(), (float) plot.getRight());
            const float py = cy - juce::jlimit (-1.0f, 1.0f, peakPanByBand[(size_t) i]) * halfH;
            if (i == 0) pkPath.startNewSubPath (px, py);
            else        pkPath.lineTo (px, py);
        }
        PinkXP::Craft::drawStyledStroke (g, pkPath, PinkXP::ink.withAlpha (0.55f), 1.0f);
    }
}

void StereoMapModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto& tokens = PinkXP::getSkinTokens();
    const auto canvas = canvasFromContent (contentBounds);

    if (PinkXP::isTransparentBackdrop())
    {
        if (PinkXP::isChromaRevealed (*this))
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.excludeClipRegion (canvas);
            g.setColour (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                             ? PinkXP::btnFace
                             : PinkXP::face);
            g.fillRect (contentBounds);
        }
    }
    else
    {
        const juce::Colour backdrop = PinkXP::isGreenScreenBackdrop()
                                          ? PinkXP::chromaKeyGreen()
                                          : (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                                                 ? PinkXP::btnFace
                                                 : PinkXP::face);
        g.setColour (backdrop);
        g.fillRect (contentBounds);
    }

    drawBackground (g, canvas);

    const auto plot = getPlotBounds (canvas);
    cachedBandCount = juce::jmax (64, plot.getWidth());
    drawCurve (g, plot);

    PinkXP::Craft::drawCanvasFinishOverlay (g, canvas);
    paintGlOverlay (g, canvas);
}

float StereoMapModule::freqAt (float norm01) const noexcept
{
    return (float) y2k::fscale::fromUnit (fftScale, (double) juce::jlimit (0.0f, 1.0f, norm01),
                                          (double) activeMinFreqHz(), (double) activeMaxFreqHz());
}

float StereoMapModule::normFromFreq (float freqHz) const noexcept
{
    return (float) y2k::fscale::toUnit (fftScale, (double) freqHz,
                                        (double) activeMinFreqHz(), (double) activeMaxFreqHz());
}

void StereoMapModule::gridFreqs (std::vector<float>& lines, std::vector<float>& labels) const
{
    const float fLo = activeMinFreqHz();
    const float fHi = activeMaxFreqHz();
    lines.clear();
    labels.clear();

    auto filterLines = [&] (std::initializer_list<float> src)
    {
        for (float f : src)
            if (f > fLo * 1.02f && f < fHi * 0.98f)
                lines.push_back (f);
    };

    if (freqRange == FreqRange::bass)
        filterLines ({ 50.0f, 100.0f, 200.0f, 500.0f });
    else if (fftScale == y2k::fscale::Scale::mel)
        filterLines ({ 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f });
    else if (fftScale == y2k::fscale::Scale::linear)
        filterLines ({ 2000.0f, 5000.0f, 10000.0f, 15000.0f });
    else
        filterLines ({ 100.0f, 1000.0f, 10000.0f });

    labels.push_back (fLo);
    for (float f : lines) labels.push_back (f);
    labels.push_back (fHi);
}

float StereoMapModule::xForNorm (float norm01, juce::Rectangle<int> plot) noexcept
{
    return juce::jmap (norm01,
                       (float) plot.getX(),
                       (float) plot.getRight());
}

// xForNorm 的逆运算：plot 左边界 = 20Hz，右边界 = 20kHz
float StereoMapModule::normForX (float x, juce::Rectangle<int> plot) noexcept
{
    const float w = (float) plot.getWidth();
    if (w <= 0.0f)
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, (x - (float) plot.getX()) / w);
}

// 纵轴：中线 = 居中（pan 0），上边界 = L 满幅（+1），下边界 = R 满幅（-1）
float StereoMapModule::panForY (float y, juce::Rectangle<int> plot) noexcept
{
    const float halfH = (float) juce::jmax (1, plot.getHeight() / 2);
    return juce::jlimit (-1.0f, 1.0f, ((float) plot.getCentreY() - y) / halfH);
}

// MONO 模式纵轴（倒置后基线在顶部）：
//   轴值 = 单声道保留量 = 1 - 真实抵消量
//   顶边 = 1.00（没有抵消），底边 = 1 - kCancelFullScale（抵消达到满幅标定）
//   所以从上到下读数是 100% → 75% → 50%，与曲线高度一致。
float StereoMapModule::compatForY (float y, juce::Rectangle<int> plot) const noexcept
{
    const float h = (float) juce::jmax (1, plot.getHeight() - 1);
    const float display = juce::jlimit (0.0f, 1.0f,
                                        ((float) y - (float) plot.getY() - 1.0f) / h);

    return 1.0f - display * cancelFullScale;
}

float StereoMapModule::cancelToDisplay (float cancel) const noexcept
{
    return juce::jlimit (0.0f, 1.0f, cancel / juce::jmax (0.1f, cancelFullScale));
}

juce::String StereoMapModule::formatPan (float pan)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);
    if (std::abs (pan) < 0.005f)
        return "C";
    return juce::String (pan > 0.0f ? "L " : "R ") + juce::String (std::abs (pan), 2);
}

juce::String StereoMapModule::formatPercent (float v)
{
    return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * 100.0f)) + "%";
}


