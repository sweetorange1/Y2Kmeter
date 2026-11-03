#include "source/ui/modules/StereoMapModule.h"
#include "source/ui/PinkXPStyle.h"
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

    // 模式开关：按钮文字显示当前模式，切到 MONO 时按钮呈按下态
    //   （PinkXP 的 drawButtonBackground 对 toggle 态画凹陷 + 抖动点阵）
    btnMode.setClickingTogglesState (true);
    btnMode.onClick = [this]()
    {
        setMode (btnMode.getToggleState() ? Mode::cancel : Mode::pan);
    };
    addAndMakeVisible (btnMode);
    applyModeToButton();
}

StereoMapModule::~StereoMapModule()
{
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

    applyModeToButton();
    buildGlSnapshot();
    repaint();
}

void StereoMapModule::applyModeToButton()
{
    btnMode.setToggleState (mode == Mode::cancel, juce::dontSendNotification);
    btnMode.setButtonText (mode == Mode::pan ? "PAN" : "MONO");
    btnMode.setTooltip (mode == Mode::pan
        ? juce::String ("Mode: Pan - per-band L/R position (up = left, down = right). Click for Mono.")
        : juce::String ("Mode: Mono - level retained after summing to mono. Top = 100%, bottom = "
                        + formatPercent (1.0f - kCancelFullScale)
                        + " (full-scale cancellation). Lower = worse. Click for Pan."));
}

// ==========================================================
// 工具条右侧的模式说明
//   与模式按钮同一行、靠右对齐；字号先按行高取，再按实测字宽收缩，
//   模块被拉窄时不裁字（窄到放不下就整体不画）。
// ==========================================================
void StereoMapModule::drawToolbarHint (juce::Graphics& g) const
{
    const auto tb = getToolbarBounds (getContentBounds());

    if (tb.isEmpty() || tb.getHeight() < 8)
        return;

    const int btnW = juce::jmin (tb.getWidth(), PinkXP::ui (58));
    auto area = tb.withTrimmedLeft (btnW + PinkXP::ui (6)).reduced (PinkXP::ui (2), 0);

    if (area.getWidth() < 36 || area.getHeight() < 6)
        return;

    // 纯 ASCII：Silkscreen 只保证拉丁字形，中文/箭头会落到缺字方框
    const juce::String text = (mode == Mode::pan)
        ? juce::String ("L/R PER BAND   UP=L  DOWN=R")
        : juce::String ("MONO SUM   UP=CLEAN  DOWN=LOSS");

    float h = juce::jlimit (6.5f, 9.0f, (float) area.getHeight() * 0.58f);
    const float avail = (float) area.getWidth() - 2.0f;

    for (int pass = 0; pass < 4; ++pass)
    {
        const juce::Font f = PinkXP::getAxisFont (h, juce::Font::plain);
        const float w = juce::GlyphArrangement::getStringWidth (f, text);

        if (w <= avail || h <= 6.0f)
            break;

        h = juce::jmax (6.0f, h * avail / w);
    }

    g.setFont (PinkXP::getAxisFont (h, juce::Font::plain));
    g.setColour (PinkXP::ink.withAlpha (0.62f));
    g.drawText (text, area, juce::Justification::centredRight, false);
}

juce::ValueTree StereoMapModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("mode", mode == Mode::cancel ? "cancel" : "pan", nullptr);
    return s;
}

void StereoMapModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    const auto m = state.getProperty ("mode").toString();
    setMode (m == "cancel" ? Mode::cancel : Mode::pan);
}

void StereoMapModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    const auto tb = getToolbarBounds (contentBounds);
    btnMode.setVisible (! tb.isEmpty());
    if (! tb.isEmpty())
        btnMode.setBounds (tb.withWidth (juce::jmin (tb.getWidth(), PinkXP::ui (58))));

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
    repaint();
}

void StereoMapModule::computeStereoMap (float dtSec)
{
    const int bandCount = juce::jlimit (96, 512, cachedBandCount > 0 ? cachedBandCount : 256);
    hub.getSpectrumMagnitudesBlendedLR (magsL, magsR, bandCount, kMinFreq, kMaxFreq);
    if (magsL.size() != bandCount || magsR.size() != bandCount)
        return;

    // MONO 模式才需要 mid 路（= (L+R)/2 的单声道 FFT）：
    //   同一帧、同一窗、同一 hop，所以 |mid|*2 就是该帧的 |L+R|。
    //   不在 PAN 模式跑这一路，省掉一次逐频段聚合。
    const bool wantCancel = (mode == Mode::cancel);
    if (wantCancel)
    {
        hub.getSpectrumMagnitudesBlended (magsM, bandCount, kMinFreq, kMaxFreq);
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

    const float alpha = juce::jlimit (0.10f, 0.58f, juce::jmax (0.0f, dtSec) * 12.0f);
    // 抵消比是"两路比值的比值"，天然比幅度更抖；单独用一条更慢的平滑。
    const float alphaCancel = juce::jlimit (0.08f, 0.40f, juce::jmax (0.0f, dtSec) * 8.0f);
    for (int i = 0; i < bandCount; ++i)
    {
        panByBand[(size_t) i] += (targetPan[(size_t) i] - panByBand[(size_t) i]) * alpha;
        strengthByBand[(size_t) i] += (targetStrength[(size_t) i] - strengthByBand[(size_t) i]) * alpha;
        levelDbByBand[(size_t) i] += (targetLevelDb[(size_t) i] - levelDbByBand[(size_t) i]) * alpha;
        if (wantCancel)
            cancelByBand[(size_t) i] += (targetCancel[(size_t) i] - cancelByBand[(size_t) i]) * alphaCancel;
    }
}

int StereoMapModule::toolbarHeightFor (juce::Rectangle<int> contentBounds) noexcept
{
    const int h = PinkXP::ui (kToolbarH);

    // 模块被拉得很矮时隐藏工具条：否则按钮会吃掉曲线一半的高度。
    if (contentBounds.getHeight() < h * 5)
        return 0;
    return h;
}

juce::Rectangle<int> StereoMapModule::getToolbarBounds (juce::Rectangle<int> contentBounds) const
{
    const int h = toolbarHeightFor (contentBounds);
    if (h <= 0)
        return {};

    return contentBounds.reduced (2).withHeight (h);
}

juce::Rectangle<int> StereoMapModule::canvasFromContent (juce::Rectangle<int> contentBounds) noexcept
{
    auto c = contentBounds.reduced (2);

    // 工具条在凹陷画布之上（与 Spectrum 的 Peak/Slope/Sharp 同一套布局语言）
    const int h = toolbarHeightFor (contentBounds);
    if (h > 0)
        c = c.withTrimmedTop (h + 2);

    return c;
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
    return canvas.reduced (26, 12).withTrimmedBottom (14);
}

void StereoMapModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;

    const auto canvas = canvasBounds();
    if (canvas.getWidth() < 40 || canvas.getHeight() < 30)
        return;

    auto snap = beginGlSnapshot();
    snap->canvasLocal = canvas;

    appendSunkenFrame (snap->vertices, canvas);

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

        // 上下边界线：PAN 模式用对应侧色相（上 = L / 下 = R）；
        //   MONO 模式已倒置：上 = 兼容（100%）/ 下 = 满幅抵消，所以顶部用色阶低端。
        if (mode == Mode::pan)
        {
            const SideRamp edgeL = makeSideRamp (PinkXP::sel);
            const SideRamp edgeR = makeSideRamp (PinkXP::accent2);

            appendLineSegment (snap->vertices, x0, yTop, x1, yTop, 0.5f,
                               sampleRamp (edgeL, 0.62f).withAlpha (0.38f));
            appendLineSegment (snap->vertices, x0, yMid, x1, yMid, 0.6f, PinkXP::ink.withAlpha (0.35f));
            appendLineSegment (snap->vertices, x0, yBot, x1, yBot, 0.5f,
                               sampleRamp (edgeR, 0.62f).withAlpha (0.38f));
        }
        else
        {
            const SideRamp sev = makeSeverityRamp();

            appendLineSegment (snap->vertices, x0, yTop, x1, yTop, 0.5f,
                               sampleRamp (sev, 0.10f).withAlpha (0.42f));
            appendLineSegment (snap->vertices, x0, yMid, x1, yMid, 0.6f, PinkXP::ink.withAlpha (0.35f));
            appendLineSegment (snap->vertices, x0, yBot, x1, yBot, 0.5f,
                               sampleRamp (sev, 0.98f).withAlpha (0.35f));
        }

        for (const float f : { 100.0f, 1000.0f, 10000.0f })
        {
            const float x = xForNorm (normFromFreq (f), plot);
            appendLineSegment (snap->vertices, x, yTop, x, yBot, 0.5f, PinkXP::pink100.withAlpha (0.25f));
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

                // 颜色梯度：L 侧 = accent（PinkXP::sel），R 侧 = accent2。
                //   每侧明度随 strength 走色阶；横向由声像决定 accent/accent2 的混合比
                //   —— 越靠上越接近 accent，越靠下越接近 accent2，中线是两者的混合。
                const SideRamp leftRamp  = makeSideRamp (PinkXP::sel);
                const SideRamp rightRamp = makeSideRamp (PinkXP::accent2);

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
                    appendLineSegment (snap->vertices, xA, yA, xB, yB, 0.9f,
                                       stereoMapColour (leftRamp, rightRamp, panSeg, s).withAlpha (a));

                    if ((i % 3) == 0)
                        appendDisc (snap->vertices, xA, yA, 1.15f,
                                    stereoMapColour (leftRamp, rightRamp, panSeg,
                                                     juce::jmin (1.0f, s + 0.20f)).withAlpha (a),
                                    8);
                }

                const int count = (int) snap->vertices.size() / 6 - start;
                if (count > 0)
                    snap->segments.push_back ({ start, count, Primitive::Triangles });
            }
        }
        else
        {
            // MONO 模式：面积图（倒置）。基线在**顶部**，曲线越往下 = 抵消越严重，
            //   顶部贴边 = 完全兼容。颜色仍走抵消色阶（红 = 严重）。
            const int n = (int) cancelByBand.size();
            if (n > 1)
            {
                const int start = (int) snap->vertices.size() / 6;
                const SideRamp sev = makeSeverityRamp();
                const float baseY = (float) plot.getY() + 1.0f;
                const float spanH = (float) juce::jmax (1, plot.getHeight() - 1);

                for (int i = 0; i < n - 1; ++i)
                {
                    const float t0 = (float) i / (float) (n - 1);
                    const float t1 = (float) (i + 1) / (float) (n - 1);
                    const float xA = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
                    const float xB = juce::jmap (t1, (float) plot.getX(), (float) plot.getRight());

                    // cancelByBand 存真实值，绘制前换算成显示值（满幅 = 50% 抵消）
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
                                      col.withAlpha (a * 0.45f));
                    appendLineSegment (snap->vertices, xA, yA, xB, yB, 0.9f,
                                       col.withAlpha (juce::jmin (1.0f, a * 1.25f)));
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
    // 工具条右侧的模式说明（两条绘制路径共用：paintLegacyContent 末尾也会调本函数）
    drawToolbarHint (g);

    const auto plot = getPlotBounds (canvas);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    if (mode == Mode::pan)
    {
        // L / R 标签沿用曲线的侧向配色（取样色阶靠"亮"端，保证在浅色画布上可读），
        //   让"哪条颜色代表左/右"不需要图例也能读出来。
        const juce::Colour labelL = sampleRamp (makeSideRamp (PinkXP::sel),     0.72f);
        const juce::Colour labelR = sampleRamp (makeSideRamp (PinkXP::accent2), 0.72f);

        g.setFont (PinkXP::getAxisFont (10.0f, juce::Font::bold));
        g.setColour (labelL.withAlpha (0.85f));
        g.drawText ("L", plot.getX() - 18, plot.getY() - 2, 14, 12, juce::Justification::centredRight, false);
        g.setColour (PinkXP::ink.withAlpha (0.72f));
        g.drawText ("C", plot.getX() - 18, plot.getCentreY() - 6, 14, 12, juce::Justification::centredRight, false);
        g.setColour (labelR.withAlpha (0.85f));
        g.drawText ("R", plot.getX() - 18, plot.getBottom() - 10, 14, 12, juce::Justification::centredRight, false);
    }
    else
    {
        // MONO 模式纵轴已倒置：刻度是"单声道保留量"（= 1 - 真实抵消量），
        //   顶 = 100%（没有抵消）/ 中 = 75% / 底 = 50%（抵消达到满幅标定）。
        const SideRamp sev = makeSeverityRamp();

        g.setFont (PinkXP::getAxisFont (8.0f, juce::Font::plain));

        g.setColour (sampleRamp (sev, 0.10f).withAlpha (0.80f));
        g.drawText ("100%", plot.getX() - 25, plot.getY() - 5, 24, 11,
                    juce::Justification::centredRight, false);
        g.setColour (PinkXP::ink.withAlpha (0.62f));
        g.drawText ("75%", plot.getX() - 25, plot.getCentreY() - 5, 24, 11,
                    juce::Justification::centredRight, false);
        g.setColour (sampleRamp (sev, 0.90f).withAlpha (0.85f));
        g.drawText ("50%", plot.getX() - 25, plot.getBottom() - 6, 24, 11,
                    juce::Justification::centredRight, false);
    }

    g.setColour (PinkXP::ink.withAlpha (0.58f));
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
    for (const float f : { 20.0f, 100.0f, 1000.0f, 10000.0f, 20000.0f })
    {
        const float x = xForNorm (normFromFreq (f), plot);
        juce::String label;
        if (f >= 1000.0f)
            label = juce::String (f / 1000.0f, (f == 1000.0f || f == 20000.0f) ? 0 : 1) + "k";
        else
            label = juce::String ((int) f);

        g.drawText (label,
                    juce::roundToInt (x) - 14,
                    plot.getBottom() + 2,
                    28,
                    12,
                    juce::Justification::centred,
                    false);
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
        repaint();
    }
}

void StereoMapModule::mouseExit (const juce::MouseEvent& e)
{
    ModulePanel::mouseExit (e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint();
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

    return PinkXP::formatFreqHz (freq) + "  " + yText;
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
    const juce::String readout = makeDataReadout (hit.pos.x, plot);
    if (readout.isNotEmpty())
        ctx.bubble (hit.pos.translated (0.0f, -22.0f), readout, ToyRole::Reward);
}

void StereoMapModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);

    const auto plot = getPlotBounds (canvas);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    if (mode == Mode::pan)
    {
        const SideRamp edgeL = makeSideRamp (PinkXP::sel);
        const SideRamp edgeR = makeSideRamp (PinkXP::accent2);

        g.setColour (sampleRamp (edgeL, 0.62f).withAlpha (0.38f));
        g.drawHorizontalLine (plot.getY(), (float) plot.getX(), (float) plot.getRight());
        g.setColour (sampleRamp (edgeR, 0.62f).withAlpha (0.38f));
        g.drawHorizontalLine (plot.getBottom(), (float) plot.getX(), (float) plot.getRight());
    }
    else
    {
        const SideRamp sev = makeSeverityRamp();

        // 倒置后：顶 = 兼容（色阶低端）/ 底 = 满幅抵消（色阶高端）
        g.setColour (sampleRamp (sev, 0.10f).withAlpha (0.42f));
        g.drawHorizontalLine (plot.getY(), (float) plot.getX(), (float) plot.getRight());
        g.setColour (sampleRamp (sev, 0.98f).withAlpha (0.35f));
        g.drawHorizontalLine (plot.getBottom(), (float) plot.getX(), (float) plot.getRight());
    }

    g.setColour (PinkXP::ink.withAlpha (0.35f));
    g.drawHorizontalLine (plot.getCentreY(), (float) plot.getX(), (float) plot.getRight());

    g.setColour (PinkXP::pink100.withAlpha (0.25f));
    for (const float f : { 100.0f, 1000.0f, 10000.0f })
    {
        const float x = xForNorm (normFromFreq (f), plot);
        g.drawVerticalLine (juce::roundToInt (x), (float) plot.getY(), (float) plot.getBottom());
    }
}

void StereoMapModule::drawCurve (juce::Graphics& g, juce::Rectangle<int> plot) const
{
    if (mode == Mode::cancel)
    {
        // MONO 模式（CPU 回退）：与 GL 路径一致的倒置面积图（基线在顶部，
        //   曲线越往下 = 抵消越严重）
        const int n = (int) cancelByBand.size();
        if (n <= 1)
            return;

        const SideRamp sev = makeSeverityRamp();
        const float baseY = (float) plot.getY() + 1.0f;
        const float spanH = (float) juce::jmax (1, plot.getHeight() - 1);

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

            g.setColour (col.withAlpha (a * 0.45f));
            g.fillPath (p);
            g.setColour (col.withAlpha (juce::jmin (1.0f, a * 1.25f)));
            g.drawLine (xA, yA, xB, yB, 1.1f);
        }
        return;
    }

    const int n = (int) panByBand.size();
    if (n <= 1)
        return;

    const float cy = (float) plot.getCentreY();
    const float halfH = (float) juce::jmax (1, plot.getHeight() / 2);

    // 与 GL 路径同一套颜色梯度（L = accent / R = accent2）。
    const SideRamp leftRamp  = makeSideRamp (PinkXP::sel);
    const SideRamp rightRamp = makeSideRamp (PinkXP::accent2);

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

        g.setColour (stereoMapColour (leftRamp, rightRamp, 0.5f * (panA + panB), s).withAlpha (a));
        g.drawLine (xA, yA, xB, yB, 1.1f);
    }

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
        g.setColour (stereoMapColour (leftRamp, rightRamp, pan,
                                      juce::jmin (1.0f, s + 0.20f)).withAlpha (a));
        g.fillEllipse (x - 1.5f, y - 1.5f, 3.0f, 3.0f);
    }
}

void StereoMapModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    const auto canvas = canvasFromContent (contentBounds);
    drawBackground (g, canvas);

    const auto plot = getPlotBounds (canvas);
    cachedBandCount = juce::jmax (64, plot.getWidth());
    drawCurve (g, plot);

    paintGlOverlay (g, canvas);
}

float StereoMapModule::freqAt (float norm01) noexcept
{
    norm01 = juce::jlimit (0.0f, 1.0f, norm01);
    return kMinFreq * std::pow (kMaxFreq / kMinFreq, norm01);
}

float StereoMapModule::normFromFreq (float freqHz) noexcept
{
    const float f = juce::jlimit (kMinFreq, kMaxFreq, freqHz);
    return std::log (f / kMinFreq) / std::log (kMaxFreq / kMinFreq);
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
float StereoMapModule::compatForY (float y, juce::Rectangle<int> plot) noexcept
{
    const float h = (float) juce::jmax (1, plot.getHeight() - 1);
    const float display = juce::jlimit (0.0f, 1.0f,
                                        ((float) y - (float) plot.getY() - 1.0f) / h);

    return 1.0f - display * kCancelFullScale;
}

// 真实抵消量 → 显示值：抵消一半（kCancelFullScale）即顶到天花板。
float StereoMapModule::cancelToDisplay (float cancel) noexcept
{
    return juce::jlimit (0.0f, 1.0f, cancel / kCancelFullScale);
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


