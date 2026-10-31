#include "source/ui/modules/StereoMapModule.h"
#include "source/ui/PinkXPStyle.h"

#include <cmath>

StereoMapModule::StereoMapModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::stereoMap), hub (h)
{
    hub.retain (AnalyserHub::Kind::Spectrum);
    hub.addFrameListener (this);

    setMinSize (96, 80);
    setDefaultSize (384, 240);
    setTitleText ("Stereo Map");
}

StereoMapModule::~StereoMapModule()
{
    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Spectrum);
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

    std::vector<float> targetPan ((size_t) bandCount, 0.0f);
    std::vector<float> targetStrength ((size_t) bandCount, 0.0f);

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

        // 可见度沿用 Spectrum 的 slope 观感，仅用于亮度，不影响声像位置。
        const float slopeDb = 4.5f * std::log2 (juce::jmax (20.0f, freq) / 1000.0f);
        const float slopeLin = std::pow (10.0f, slopeDb / 20.0f);
        const float signalVis = signalRaw * slopeLin;
        const float dbVis = 20.0f * std::log10 (juce::jmax (1.0e-8f, signalVis));
        const float strength = juce::jlimit (0.0f, 1.0f, (dbVis + 90.0f) / 90.0f);

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
        targetPan[(size_t) i] = juce::jlimit (-1.0f, 1.0f, rawPan * activityFinal * panBoost);
        targetStrength[(size_t) i] = strength * juce::jmax (0.35f, activityFinal);
    }

    if ((int) panByBand.size() != bandCount)
    {
        panByBand = targetPan;
        strengthByBand = targetStrength;
        return;
    }

    const float alpha = juce::jlimit (0.10f, 0.58f, juce::jmax (0.0f, dtSec) * 12.0f);
    for (int i = 0; i < bandCount; ++i)
    {
        panByBand[(size_t) i] += (targetPan[(size_t) i] - panByBand[(size_t) i]) * alpha;
        strengthByBand[(size_t) i] += (targetStrength[(size_t) i] - strengthByBand[(size_t) i]) * alpha;
    }
}

juce::Rectangle<int> StereoMapModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return contentBounds.reduced (2);
}

juce::Rectangle<int> StereoMapModule::getPlotBounds (juce::Rectangle<int> canvas) const
{
    return canvas.reduced (26, 12).withTrimmedBottom (14);
}

void StereoMapModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;

    const auto canvas = getContentBounds().reduced (2);
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

        appendLineSegment (snap->vertices, x0, yTop, x1, yTop, 0.5f, PinkXP::pink200.withAlpha (0.35f));
        appendLineSegment (snap->vertices, x0, yMid, x1, yMid, 0.6f, PinkXP::ink.withAlpha (0.35f));
        appendLineSegment (snap->vertices, x0, yBot, x1, yBot, 0.5f, PinkXP::pink200.withAlpha (0.35f));

        for (const float f : { 100.0f, 1000.0f, 10000.0f })
        {
            const float x = xForNorm (normFromFreq (f), plot);
            appendLineSegment (snap->vertices, x, yTop, x, yBot, 0.5f, PinkXP::pink100.withAlpha (0.25f));
        }

        const int bgCount = (int) snap->vertices.size() / 6 - bgStart;
        if (bgCount > 0)
            snap->segments.push_back ({ bgStart, bgCount, Primitive::Triangles });

        const int n = (int) panByBand.size();
        if (n > 1)
        {
            const int start = (int) snap->vertices.size() / 6;
            const float cy = (float) plot.getCentreY();
            const float halfH = (float) juce::jmax (1, plot.getHeight() / 2);

            for (int i = 0; i < n - 1; ++i)
            {
                const float t0 = (float) i / (float) (n - 1);
                const float t1 = (float) (i + 1) / (float) (n - 1);

                const float xA = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
                const float xB = juce::jmap (t1, (float) plot.getX(), (float) plot.getRight());

                const float yA = cy - juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]) * halfH;
                const float yB = cy - juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) (i + 1)]) * halfH;

                const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
                const float a = std::pow (s, 0.95f);
                if (a < 0.02f)
                    continue;
                appendLineSegment (snap->vertices, xA, yA, xB, yB, 0.9f,
                                   PinkXP::pink500.withAlpha (a));

                if ((i % 3) == 0)
                    appendDisc (snap->vertices, xA, yA, 1.15f, PinkXP::pink700.withAlpha (a), 8);
            }

            const int count = (int) snap->vertices.size() / 6 - start;
            if (count > 0)
                snap->segments.push_back ({ start, count, Primitive::Triangles });
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

    g.setColour (PinkXP::ink.withAlpha (0.72f));
    g.setFont (PinkXP::getAxisFont (10.0f, juce::Font::bold));
    g.drawText ("L", plot.getX() - 18, plot.getY() - 2, 14, 12, juce::Justification::centredRight, false);
    g.drawText ("C", plot.getX() - 18, plot.getCentreY() - 6, 14, 12, juce::Justification::centredRight, false);
    g.drawText ("R", plot.getX() - 18, plot.getBottom() - 10, 14, 12, juce::Justification::centredRight, false);

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
}

void StereoMapModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);

    const auto plot = getPlotBounds (canvas);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    g.setColour (PinkXP::pink200.withAlpha (0.35f));
    g.drawHorizontalLine (plot.getY(), (float) plot.getX(), (float) plot.getRight());
    g.drawHorizontalLine (plot.getBottom(), (float) plot.getX(), (float) plot.getRight());

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
    const int n = (int) panByBand.size();
    if (n <= 1)
        return;

    const float cy = (float) plot.getCentreY();
    const float halfH = (float) juce::jmax (1, plot.getHeight() / 2);

    for (int i = 0; i < n - 1; ++i)
    {
        const float t0 = (float) i / (float) (n - 1);
        const float t1 = (float) (i + 1) / (float) (n - 1);
        const float xA = juce::jmap (t0, (float) plot.getX(), (float) plot.getRight());
        const float xB = juce::jmap (t1, (float) plot.getX(), (float) plot.getRight());
        const float yA = cy - juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]) * halfH;
        const float yB = cy - juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) (i + 1)]) * halfH;

        const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
        const float a = std::pow (s, 0.95f);
        if (a < 0.02f)
            continue;

        g.setColour (PinkXP::pink500.withAlpha (a));
        g.drawLine (xA, yA, xB, yB, 1.1f);
    }

    for (int i = 0; i < n; i += 3)
    {
        const float t = (float) i / (float) (n - 1);
        const float x = juce::jmap (t, (float) plot.getX(), (float) plot.getRight());
        const float y = cy - juce::jlimit (-1.0f, 1.0f, panByBand[(size_t) i]) * halfH;
        const float s = juce::jlimit (0.0f, 1.0f, strengthByBand[(size_t) i]);
        const float a = std::pow (s, 0.95f);
        if (a < 0.03f)
            continue;
        g.setColour (PinkXP::pink700.withAlpha (a));
        g.fillEllipse (x - 1.5f, y - 1.5f, 3.0f, 3.0f);
    }
}

void StereoMapModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    const auto canvas = contentBounds.reduced (2);
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


