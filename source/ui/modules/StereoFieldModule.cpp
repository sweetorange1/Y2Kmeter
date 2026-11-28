#include "source/ui/modules/StereoFieldModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <cstring>

// ==========================================================
// StereoFieldModule —— Pink XP 像素风"声相指示"（半圆雷达）
// ==========================================================

namespace
{
// 能量环标签：环半径占 R 的比例 frac 在当前自动增益下的**真实电平**
//   level = frac / gain → dB = 20·log10(level)
//   gain = 1 时正好回落到原来的 -12 / -6 / -2.5 / 0 dB。
juce::String ringDbLabel (float frac, float gain)
{
    if (frac <= 0.0f || gain <= 0.0f)
        return {};

    const float db = 20.0f * std::log10 (frac / gain);

    if (std::abs (db) < 0.05f)
        return "0 dB";

    return (std::abs (db - std::round (db)) < 0.05f)
        ? juce::String (juce::roundToInt (db)) + " dB"
        : juce::String (db, 1) + " dB";
}
} // namespace

StereoFieldModule::StereoFieldModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::stereoField), hub (h)
{
    hub.retain (AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener (this);

    setMinSize     (64, 64);
    setDefaultSize (320, 220);
    setTitleText   ("Stereo Field");
    setControlPanelEnabled (true);

    // 自动缩放：首个结算周期从"现在"起算，用满 1 s 的真实数据再定增益
    lastPeriodicGainUpdateMs = juce::Time::getMillisecondCounterHiRes();

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        // 点颜色与视觉风格跟随主题/皮肤 → 清空残影、重建 GL 快照并重绘（静音态也立即刷新）
        invalidateTrail();
        buildGlSnapshot();
        repaint();
    });
}

StereoFieldModule::~StereoFieldModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Oscilloscope);
}

// ----------------------------------------------------------
// 持久化 + 控制面板（v3.1）
// ----------------------------------------------------------
juce::ValueTree StereoFieldModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("dotSize",      (double) dotSize, nullptr);
    s.setProperty ("fadeSpeed",    (double) fadeSpeed, nullptr);
    s.setProperty ("autoGain",     autoGain, nullptr);
    s.setProperty ("manualGainDb", (double) manualGainDb, nullptr);
    s.setProperty ("dotShape",     dotShape, nullptr);
    s.setProperty ("showRings",    showRings, nullptr);
    s.setProperty ("showLabels",   showLabels, nullptr);
    s.setProperty ("bandColour",   bandColour, nullptr);
    s.setProperty ("crossLow",     (double) crossLowHz, nullptr);
    s.setProperty ("crossHigh",    (double) crossHighHz, nullptr);
    return s;
}

void StereoFieldModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    // 缺键 = 保持当前值（构造默认 = 旧外观）
    if (state.hasProperty ("dotSize"))      dotSize      = juce::jlimit (1.0f, 6.0f, (float) (double) state.getProperty ("dotSize"));
    if (state.hasProperty ("fadeSpeed"))    fadeSpeed    = juce::jlimit (0.2f, 4.0f, (float) (double) state.getProperty ("fadeSpeed"));
    if (state.hasProperty ("autoGain"))     autoGain     = (bool) state.getProperty ("autoGain");
    if (state.hasProperty ("manualGainDb")) manualGainDb = juce::jlimit (-12.0f, 18.0f, (float) (double) state.getProperty ("manualGainDb"));
    if (state.hasProperty ("dotShape"))     dotShape     = juce::jlimit (0, 1, (int) state.getProperty ("dotShape"));
    if (state.hasProperty ("showRings"))    showRings    = (bool) state.getProperty ("showRings");
    if (state.hasProperty ("showLabels"))   showLabels   = (bool) state.getProperty ("showLabels");
    if (state.hasProperty ("bandColour"))   bandColour   = (bool) state.getProperty ("bandColour");
    if (state.hasProperty ("crossLow"))     crossLowHz   = juce::jlimit (80.0f, 1000.0f, (float) (double) state.getProperty ("crossLow"));
    if (state.hasProperty ("crossHigh"))    crossHighHz  = juce::jlimit (500.0f, 10000.0f, (float) (double) state.getProperty ("crossHigh"));

    if (! autoGain)
        autoGainFactor = std::pow (10.0f, manualGainDb * 0.05f);
    periodicMaxAccum         = 0.0f;
    lastPeriodicGainUpdateMs = autoGain ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    splitter.setCrossover (crossLowHz, crossHighHz);

    invalidateTrail();
    buildGlSnapshot();
    refreshControlPanel();
    repaint();
}

void StereoFieldModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("dotShape", "Dot style",
        { "Square", "Glow" },
        "Square: crisp Y2K pixel dots. Glow: soft luminous halo around each radar point.")
        .in ("Dots"));
    out.push_back (ParamDef::slider ("dotSize", "Dot size", 1.0, 6.0, 1.0, " px",
        "Side length of each dot.").in ("Dots"));
    out.push_back (ParamDef::slider ("fadeSpeed", "Fade speed", 0.2, 4.0, 0.1, "x",
        "How fast old dots disappear. 1.0 is the classic trail; higher is shorter, lower leaves long tails.")
        .in ("Dots"));
    out.push_back (ParamDef::toggle ("autoGain", "Auto zoom",
        "Scales the picture so the loudest point stays inside the screen.")
        .in ("Dots"));
    out.push_back (ParamDef::slider ("manualGainDb", "Manual zoom", -12.0, 18.0, 1.0, " dB",
        "Fixed radar radius zoom when Auto zoom is OFF.")
        .in ("Dots").when ([this] { return ! autoGain; }));

    out.push_back (ParamDef::toggle ("bandColour", "Colour by frequency",
        "Audio is split into three bands and each band's dots are drawn in its own colour: "
        "red for lows, purple-blue for mids, cyan for highs.")
        .in ("Colour"));
    out.push_back (ParamDef::slider ("crossLow", "Low / mid split", 80.0, 1000.0, 10.0, " Hz",
        "Low band: below this frequency (red dot cloud).", 200.0)
        .in ("Colour").when ([this] { return bandColour; }));
    out.push_back (ParamDef::slider ("crossHigh", "Mid / high split", 500.0, 10000.0, 100.0, " Hz",
        "High band: above this frequency (cyan dot cloud).", 2000.0)
        .in ("Colour").when ([this] { return bandColour; }));

    out.push_back (ParamDef::toggle ("showRings", "Level rings",
        "Show concentric dB rings and 45-degree radial guide lines.")
        .in ("Radar"));
    out.push_back (ParamDef::toggle ("showLabels", "Labels",
        "Show L/C/R pan labels and dB ring readouts.")
        .in ("Radar"));
}

double StereoFieldModule::getParamValue (const juce::String& id) const
{
    if (id == "dotShape")     return (double) dotShape;
    if (id == "dotSize")      return (double) dotSize;
    if (id == "fadeSpeed")    return (double) fadeSpeed;
    if (id == "autoGain")     return autoGain ? 1.0 : 0.0;
    if (id == "manualGainDb") return (double) manualGainDb;
    if (id == "bandColour")   return bandColour ? 1.0 : 0.0;
    if (id == "crossLow")     return (double) crossLowHz;
    if (id == "crossHigh")    return (double) crossHighHz;
    if (id == "showRings")    return showRings ? 1.0 : 0.0;
    if (id == "showLabels")   return showLabels ? 1.0 : 0.0;
    return 0.0;
}

void StereoFieldModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "dotShape")   { dotShape = juce::jlimit (0, 1, juce::roundToInt (v)); invalidateTrail(); buildGlSnapshot(); }
    else if (id == "dotSize")    dotSize    = juce::jlimit (1.0f, 6.0f, (float) v);
    else if (id == "fadeSpeed")  fadeSpeed  = juce::jlimit (0.2f, 4.0f, (float) v);
    else if (id == "autoGain")
    {
        const bool next = v >= 0.5;
        if (autoGain != next)
        {
            autoGain = next;
            if (! autoGain)
                autoGainFactor = std::pow (10.0f, manualGainDb * 0.05f);
            periodicMaxAccum         = 0.0f;
            lastPeriodicGainUpdateMs = 0.0;
            invalidateTrail();
            buildGlSnapshot();
        }
    }
    else if (id == "manualGainDb")
    {
        manualGainDb = juce::jlimit (-12.0f, 18.0f, (float) v);
        if (! autoGain)
        {
            autoGainFactor = std::pow (10.0f, manualGainDb * 0.05f);
            invalidateTrail();
            buildGlSnapshot();
        }
    }
    else if (id == "bandColour")
    {
        const bool turningOn = (v >= 0.5) && ! bandColour;
        bandColour = v >= 0.5;
        trailNeedsClear = true;
        invalidateTrail();

        if (turningOn)
        {
            crossLowHz  = y2k::bands::BandSplitter::kDefaultLowCross;
            crossHighHz = y2k::bands::BandSplitter::kDefaultHighCross;
            splitter.setCrossover (crossLowHz, crossHighHz);
        }
    }
    else if (id == "crossLow")   { crossLowHz  = (float) v; splitter.setCrossover (crossLowHz, crossHighHz); trailNeedsClear = true; invalidateTrail(); }
    else if (id == "crossHigh")  { crossHighHz = (float) v; splitter.setCrossover (crossLowHz, crossHighHz); trailNeedsClear = true; invalidateTrail(); }
    else if (id == "showRings")  { showRings  = (v >= 0.5); buildGlSnapshot(); }
    else if (id == "showLabels") { showLabels = (v >= 0.5); }
    else return;
    repaint();
}

// 把当前快照映射成点（GL 与 CPU 回退共用）：几何 / 过滤 / 抽稀 / 配色只有这一份
void StereoFieldModule::collectPoints (const juce::Rectangle<int>& plot, std::vector<Pt>& out) const
{
    out.clear();
    constexpr float kPi = juce::MathConstants<float>::pi;

    const int N = juce::jmin (snapshotL.size(), snapshotR.size());
    if (N <= 1 || plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    const auto geo = computeRadarGeometry (plot);
    const float cx = (float) geo.cx, baseY = (float) geo.baseY, R = (float) geo.radius;
    const float effR = R * autoGainFactor;   // 自动缩放：半径乘当前增益，瞬时超峰压到圆弧上

    constexpr int kMaxPoints = 512;
    const int stride = juce::jmax (1, N / kMaxPoints);
    const juce::Rectangle<float> plotF = plot.toFloat();
    const auto baseCol = PinkXP::pink500.withAlpha (0.9f);
    const bool useBand = bandColour && (int) lowL.size() == N;

    // 单层点云：把 (L,R) 映射到雷达坐标（角度 = 声像，半径 = 电平）
    auto pushCloud = [&] (const float* srcL, const float* srcR, juce::Colour col, int layer)
    {
        for (int i = 0; i < N; i += stride)
        {
            const float rawL = srcL[i];
            const float rawR = srcR[i];
            if (! std::isfinite (rawL) || ! std::isfinite (rawR)) continue;

            const float l = std::abs (rawL);
            const float r = std::abs (rawR);
            const float sum = l + r;
            if (sum < 1.0e-4f) continue;   // 静音点不画，避免在圆心堆积

            const float balance = (r - l) / sum;   // [-1,1]：-1 纯左，+1 纯右
            const float rho = juce::jmin (juce::jmax (l, r) * effR, R);
            const float ang = balance * (kPi * 0.5f);
            const float px = cx + rho * std::sin (ang);
            const float py = baseY - rho * std::cos (ang);
            if (! plotF.contains (px, py)) continue;

            out.push_back ({ px, py, col, layer });
        }
    };

    if (useBand)
    {
        const auto toCol = [] (const y2k::bands::Rgb& c)
        {
            return juce::Colour::fromFloatRGBA (c.r, c.g, c.b, 0.9f);
        };
        out.reserve ((size_t) (3 * (N / stride + 1)));
        pushCloud (lowL.data(),  lowR.data(),  toCol (y2k::bands::kLowRgb),  0);
        pushCloud (midL.data(),  midR.data(),  toCol (y2k::bands::kMidRgb),  1);
        pushCloud (highL.data(), highR.data(), toCol (y2k::bands::kHighRgb), 2);
    }
    else
    {
        out.reserve ((size_t) (N / stride + 1));
        pushCloud (snapshotL.getRawDataPointer(), snapshotR.getRawDataPointer(), baseCol, 0);
    }
}

// ----------------------------------------------------------
// onFrame：Hub 分发器回调
// ----------------------------------------------------------
void StereoFieldModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    // 批量拷贝立体声样本
    const int n = (int) frame.oscL.size();
    snapshotL.resize (n);
    snapshotR.resize (n);
    std::memcpy (snapshotL.getRawDataPointer(), frame.oscL.data(), (size_t) n * sizeof (float));
    std::memcpy (snapshotR.getRawDataPointer(), frame.oscR.data(), (size_t) n * sizeof (float));

    // 频段染色：只在开启时才付费把 L/R 滤成低 / 中 / 高三路（与示波器 X-Y 共用 BandSplitter）
    if (bandColour && n > 1)
    {
        const double fs = hub.getSampleRate();
        if (fs != splitterRate)
        {
            splitter.prepare (fs, crossLowHz, crossHighHz);
            splitterRate = fs;
        }

        lowL.resize ((size_t) n);  lowR.resize ((size_t) n);
        midL.resize ((size_t) n);  midR.resize ((size_t) n);
        highL.resize ((size_t) n); highR.resize ((size_t) n);

        splitter.split (snapshotL.getRawDataPointer(), snapshotR.getRawDataPointer(), n,
                        lowL.data(), lowR.data(), midL.data(), midR.data(),
                        highL.data(), highR.data());
    }

    // 自动缩放：累积本帧峰值，每 1 s 结算一次增益（与 XY / Lissajous 同策略）
    if (autoGain)
        updateAutoGain (snapshotL.getRawDataPointer(), snapshotR.getRawDataPointer(),
                        juce::jmin (snapshotL.size(), snapshotR.size()));

    // v2.9.5：GPU trail 累积 —— 衰减在 GPU 每帧执行（decay 按累计 dt 换算，
    //   残影时长与帧率无关）；点云每帧画进 trail FBO（GPU 光栅化，不再需要
    //   旧版"隔帧 + CPU 全图 multiplyAlpha"）。
    //   CPU 回退通道保持独立累计 dt，避免 docked + !glDirectActive 时
    //   buildGlSnapshot() 清零 trailDtAccumSec 导致 CPU 残影停止衰减。
    trailDtAccumSec    = juce::jmin (0.5f, trailDtAccumSec + frame.dtSec);
    cpuTrailDtAccumSec = juce::jmin (0.5f, cpuTrailDtAccumSec + frame.dtSec);

    // 仅在 GL 未接管（首帧 / 回退态 / macOS 透明模式）时触发 CPU 残影更新；
    //   停靠态与独立 GL 浮窗均走 GPU 3 层 trailFbo，零 CPU 光栅化开销。
    if (! glDirectActive.load (std::memory_order_acquire))
        trailNeedsUpdate = true;

    buildGlSnapshot();

    // v2.9：去掉 15ms×DPI 硬编码节流，每个分发帧重绘
    repaintFrameContent();
}

// ----------------------------------------------------------
// updateAutoGain —— 自动缩放（参考 OscilloscopeModule::onFrame）
//   · 峰值驱动：统计 max(|L|,|R|)（散点半径的驱动量），累积到 1 s 周期
//   · gain = kAutoGainTarget / 周期峰值 → 峰值落在半径的 90%
//   · 与当前 gain 差 < 10% 不更新（死区），避免画面持续缩放抖动
//   · 静音周期不重算：否则 gain 会顶到上限，下一段声音进来时突然炸开
// ----------------------------------------------------------
void StereoFieldModule::updateAutoGain (const float* left, const float* right, int numSamples)
{
    if (! autoGain || left == nullptr || right == nullptr || numSamples <= 0)
        return;

    float frameMax = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float l = std::abs (left[i]);
        const float r = std::abs (right[i]);
        const float peak = juce::jmax (l, r);

        if (std::isfinite (peak) && peak > frameMax)
            frameMax = peak;
    }

    if (frameMax > periodicMaxAccum)
        periodicMaxAccum = frameMax;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    constexpr double kPeriodMs = 1000.0;

    if (nowMs - lastPeriodicGainUpdateMs < kPeriodMs)
        return;

    lastPeriodicGainUpdateMs = nowMs;

    if (periodicMaxAccum > 1.0e-3f)
    {
        const float targetGain = juce::jlimit (0.1f, 10.0f, kAutoGainTarget / periodicMaxAccum);

        if (std::abs (targetGain - autoGainFactor) > autoGainFactor * kAutoGainDeadZone)
            autoGainFactor = targetGain;
    }

    periodicMaxAccum = 0.0f;
}

// ----------------------------------------------------------
// paintContent —— 底衬 + 残影叠加
// ----------------------------------------------------------
// ---- GlDirectModuleBase 钩子（v2.9.5 迁移，trail 累积通道）----
juce::Rectangle<int> StereoFieldModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return contentBounds.reduced (2);
}

void StereoFieldModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    auto plot = canvas.reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    // 绿幕：隐藏所有静态刻度/方位/增益标签，只保留活体散点（悬停标尺仍可用）
    if (! PinkXP::isChromaKeySkin() && showLabels)
    {
        const auto geo = computeRadarGeometry (plot);
        const int cx = geo.cx, baseY = geo.baseY, R = geo.radius;
        const float labelFontSize = geo.labelFontSize;

        if (showRings)
        {
            g.setColour (PinkXP::ink.withAlpha (0.55f));
            g.setFont (PinkXP::getAxisFont (juce::jmax (6.0f, labelFontSize - 1.5f), juce::Font::plain));
            const float frac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = (float) R * frac[ri];
                if (rr < 8.0f || rr > (float) R) continue;
                g.drawText (y2k::tr (ringDbLabel (frac[ri], autoGainFactor)), cx + 4, baseY - (int) rr - 6, 34, 12,
                            juce::Justification::centredLeft, false);
            }
        }

        const int actualMargin = plot.getWidth() / 2 - R;
        const int labelH = juce::jmax (10, (int) std::ceil (labelFontSize) + 4);
        const int y = baseY - labelH;
        const auto lcrColour = PinkXP::ink.withAlpha (0.7f);
        const float lrFontSize = juce::jmin (labelFontSize, 11.0f);
        PinkXP::drawCachedText (g, "C", juce::Rectangle<int> (cx - 8, y, 16, labelH), juce::Justification::centred,
                                labelFontSize, juce::Font::bold, lcrColour, true);
        g.setFont (PinkXP::getAxisFont (lrFontSize, juce::Font::bold));
        PinkXP::drawCachedText (g, "L", juce::Rectangle<int> (plot.getX(), y, actualMargin, labelH), juce::Justification::centredRight,
                                lrFontSize, juce::Font::bold, lcrColour, true);
        PinkXP::drawCachedText (g, "R", juce::Rectangle<int> (plot.getRight() - actualMargin, y, actualMargin, labelH), juce::Justification::centredLeft,
                                lrFontSize, juce::Font::bold, lcrColour, true);

        if (std::abs (autoGainFactor - 1.0f) > 0.05f)
        {
            g.setColour (PinkXP::ink.withAlpha (0.6f));
            g.setFont (PinkXP::getAxisFont (8.0f, juce::Font::plain));
            g.drawText (y2k::tr (juce::String (autoGainFactor, 1) + "x"),
                        canvas.getRight() - 48, canvas.getY() + 3, 44, 12,
                        juce::Justification::centredRight, false);
        }
    }

    // 悬停十字辅助线（反馈 #10）
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void StereoFieldModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto& tokens = PinkXP::getSkinTokens();
    auto canvas = contentBounds.reduced (2);

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

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    drawBackground (g, canvas);

    if (trailNeedsUpdate)
    {
        updateTrail (canvas);
        trailNeedsUpdate = false;
    }

    // 逐层合成（低 → 中 → 高）；未染色的层为空图，直接跳过
    for (auto& img : trailImage)
    {
        if (! img.isValid()) continue;
        g.drawImage (img,
                     canvas.getX(), canvas.getY(), canvas.getWidth(), canvas.getHeight(),
                     0, 0, img.getWidth(), img.getHeight());
    }

    PinkXP::Craft::drawCanvasFinishOverlay (g, canvas);

    // 悬停十字辅助线（反馈 #10）
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

// ----------------------------------------------------------
// 悬停处理（反馈 #10）：与 Spectrum/Waveform 一致的十字辅助线
// ----------------------------------------------------------
void StereoFieldModule::mouseMove (const juce::MouseEvent& e)
{
    ModulePanel::mouseMove (e);

    const auto canvas = getContentBounds().reduced (2);
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains (pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (canvas);
    }
}

void StereoFieldModule::mouseExit (const juce::MouseEvent& e)
{
    ModulePanel::mouseExit (e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在画布内，擦除只需失效画布（不走浮动态限流）
        repaint (getContentBounds().reduced (2));
    }
}

void StereoFieldModule::drawHoverRuler (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (! hoverActive || ! canvas.contains (hoverPos))
        return;

    const auto plot = canvas.reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    const auto geo = computeRadarGeometry (plot);
    const float cx = (float) geo.cx, baseY = (float) geo.baseY, R = (float) geo.radius;

    const float dx = (float) hoverPos.x - cx;
    const float dy = baseY - (float) hoverPos.y;

    // 半径比例 → 电平：rho = peak × R × gain → peak = rho / (R × gain)
    //   （自动缩放下"满半径"不再是 0 dB，必须除掉当前增益才是真实电平）
    const float rho  = juce::jlimit (0.0f, R, std::sqrt (dx * dx + dy * dy));
    const float frac = rho / (juce::jmax (1.0f, R) * juce::jmax (0.01f, autoGainFactor));
    const float db   = 20.0f * std::log10 (juce::jmax (1.0e-4f, frac));

    // 角度 → 声像：-90° = 纯左，+90° = 纯右，顶部 0° = 居中
    const float angDeg  = std::atan2 (dx, dy) * (180.0f / juce::MathConstants<float>::pi);
    const float balance = juce::jlimit (-1.0f, 1.0f, angDeg / 90.0f);

    juce::String readout;
    if (std::abs (balance) < 0.02f)
        readout = "PAN CENTER";
    else
        readout = "PAN " + juce::String (balance < 0.0f ? "L" : "R")
                + juce::String (std::abs (juce::roundToInt (balance * 100.0f)));
    readout << "  " << juce::String (db, 1) << "dB";

    PinkXP::drawHoverRuler (g, canvas, hoverPos, readout);
}

// ----------------------------------------------------------
// buildGlSnapshot —— trail 累积通道快照（v2.9.5）
//   背景（凹陷/半圆/45°线/能量环）为顶点段，直接画后缓冲；
//   点云为 trail 段，画进持久 trail FBO（Editor 每帧衰减 + 累积 + 合成）。
// ----------------------------------------------------------
void StereoFieldModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto canvas = getContentBounds().reduced (2);
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaKey = tokens.isChromaKey;

    auto snap = beginGlSnapshot();
    snap->canvasLocal  = canvas;
    snap->fxRect       = canvas;
    snap->snapToPixels = false;

    // ---- trail 通道参数 ----
    //   三频段染色时每路一条独立轨迹（颜色不互相污染），否则只用一层
    const int layerCount = bandColour ? kTrailLayers : 1;
    snap->trailFbo      = true;
    snap->trailLayers   = layerCount;
    snap->trailClear    = trailNeedsClear;
    trailNeedsClear     = false;
    snap->trailDecay    = std::pow (kDecayPerFrame, juce::jmax (0.0f, trailDtAccumSec) * 60.0f * fadeSpeed);
    trailDtAccumSec     = 0.0f;
    snap->texW          = canvas.getWidth();
    snap->texH          = canvas.getHeight();
    snap->texTargetRect = canvas;
    snap->texLinear     = true;

    constexpr float kPi = juce::MathConstants<float>::pi;
    auto plot = canvas.reduced (6);

    // ---- 背景段（segments[0]）：凹槽 + 平滑反走样雷达几何（绿幕只铺纯绿；透明模式 0 顶点保持 alpha=0）----
    //   注意：透明模式静音时即使本段 count==0 也保留空段占位，确保 snap->segments 非空，
    //   从而 Editor 仍执行 trailFbo 的衰减/清零/合成 pass，让残影平滑淡出到 (0,0,0,0)。
    {
        appendWellFrame (snap->vertices, canvas, PinkXP::content);

        if (! chromaKey && plot.getWidth() > 8 && plot.getHeight() > 8)
        {
            const auto geo = computeRadarGeometry (plot);
            const float cx = (float) geo.cx, baseY = (float) geo.baseY, R = (float) geo.radius;
            const float r45 = R * 0.70710678f;

            const bool isChrome = (tokens.dialFinish == PinkXP::DialFinish::brushedMetal);
            const bool isModern = (tokens.dialFinish == PinkXP::DialFinish::studioSmooth);
            const bool isGlass  = (tokens.dialFinish == PinkXP::DialFinish::glassDome);
            const bool isNeon   = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
            const bool isCrt    = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
            const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;

            // 半圆外轮廓弧 + 直径线（全风格平滑反走样圆弧）
            const auto arcC = isChrome ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.42f).withAlpha (darkBg ? 0.74f : 0.66f)
                            : isGlass  ? PinkXP::pink300.interpolatedWith (juce::Colours::white, 0.25f).withAlpha (darkBg ? 0.56f : 0.62f)
                            : isNeon   ? PinkXP::pink300.withAlpha (0.88f)
                            : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.86f)
                            : isModern ? PinkXP::pink300.withAlpha (0.48f)
                                       : PinkXP::pink300.withAlpha (0.65f);
            const auto chromeOuterCut  = PinkXP::dark.withAlpha (darkBg ? 0.38f : 0.20f);
            const auto outerSpecRim    = juce::Colours::white.withAlpha (darkBg ? 0.18f : 0.30f);
            const auto neonHotCore     = PinkXP::pink200.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.88f);
            const float arcHalf = isModern ? 0.50f : ((isChrome || isGlass) ? 0.58f : 0.60f);
            constexpr int arcSegs = 64;
            for (int s = 0; s < arcSegs; ++s)
            {
                const float th0 = -kPi * 0.5f + kPi * (float) s / (float) arcSegs;
                const float th1 = -kPi * 0.5f + kPi * (float) (s + 1) / (float) arcSegs;
                const float sn0 = std::sin (th0), cs0 = std::cos (th0);
                const float sn1 = std::sin (th1), cs1 = std::cos (th1);
                const float x0 = cx + R * sn0, y0 = baseY - R * cs0;
                const float x1 = cx + R * sn1, y1 = baseY - R * cs1;
                if (isChrome)
                    appendLineSegment (snap->vertices, cx + (R + 1.0f) * sn0, baseY - (R + 1.0f) * cs0,
                                       cx + (R + 1.0f) * sn1, baseY - (R + 1.0f) * cs1, 0.52f, chromeOuterCut);
                if (isNeon || (isCrt && tokens.glowIntensity > 0.0f))
                    appendLineSegment (snap->vertices, x0, y0, x1, y1, isNeon ? 2.0f : 1.55f,
                                       PinkXP::sel.withAlpha (isNeon ? 0.28f : 0.22f * tokens.glowIntensity));
                appendLineSegment (snap->vertices, x0, y0, x1, y1, arcHalf, arcC);
                if (isNeon)
                    appendLineSegment (snap->vertices, x0, y0, x1, y1, 0.36f, neonHotCore);
                if (isChrome || isGlass)
                {
                    const float rimOff = isGlass ? 1.4f : 1.7f;
                    appendLineSegment (snap->vertices, cx + (R + rimOff) * sn0, baseY - (R + rimOff) * cs0,
                                       cx + (R + rimOff) * sn1, baseY - (R + rimOff) * cs1, 0.38f, outerSpecRim);
                }
            }

            if ((isChrome || isCrt) && R >= 24.0f)
            {
                // Millennium Chrome / CRT Terminal：半圆外沿 15° 精密方位刻度（45° 主刻 + 15° 次刻）
                const auto majC = isCrt
                                      ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.25f).withAlpha (0.76f)
                                      : PinkXP::ink.interpolatedWith (PinkXP::hl, darkBg ? 0.35f : 0.18f).withAlpha (0.58f);
                const auto minC = isCrt
                                      ? PinkXP::sel.withAlpha (0.40f)
                                      : PinkXP::ink.withAlpha (darkBg ? 0.28f : 0.24f);
                for (int deg = -75; deg <= 75; deg += 15)
                {
                    const bool major = (deg % 45 == 0);
                    const float th   = (float) deg * (kPi / 180.0f);
                    const float sn   = std::sin (th), cs = std::cos (th);
                    const float len  = major ? 4.5f : 2.2f;
                    appendLineSegment (snap->vertices,
                                       cx + (R - len) * sn, baseY - (R - len) * cs,
                                       cx + R * sn,         baseY - R * cs,
                                       major ? 0.52f : 0.38f, major ? majC : minC);
                }
            }
            else if (isNeon && R >= 24.0f)
            {
                // Neon Cyber：45° 发光外沿角标刻度
                const auto nGlow = PinkXP::sel.withAlpha (0.30f);
                const auto nCore = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.50f).withAlpha (0.90f);
                for (int deg : { -45, 0, 45 })
                {
                    const float th = (float) deg * (kPi / 180.0f);
                    const float sn = std::sin (th), cs = std::cos (th);
                    appendLineSegment (snap->vertices, cx + (R - 4.5f) * sn, baseY - (R - 4.5f) * cs,
                                       cx + (R + 1.2f) * sn, baseY - (R + 1.2f) * cs, 1.25f, nGlow);
                    appendLineSegment (snap->vertices, cx + (R - 4.5f) * sn, baseY - (R - 4.5f) * cs,
                                       cx + (R + 1.2f) * sn, baseY - (R + 1.2f) * cs, 0.48f, nCore);
                }
            }

            if (isNeon || (isCrt && tokens.glowIntensity > 0.0f))
                appendLineSegment (snap->vertices, cx - R, baseY, cx + R, baseY, 1.3f,
                                   PinkXP::sel.withAlpha (isNeon ? 0.22f : 0.18f * tokens.glowIntensity));
            appendLineSegment (snap->vertices, cx - R, baseY, cx + R, baseY, isModern ? 0.45f : 0.50f,
                               isChrome ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.36f).withAlpha (darkBg ? 0.56f : 0.50f)
                             : isGlass  ? PinkXP::pink300.interpolatedWith (juce::Colours::white, 0.22f).withAlpha (darkBg ? 0.46f : 0.52f)
                             : isNeon   ? PinkXP::pink300.withAlpha (0.76f)
                             : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.74f)
                             : isModern ? PinkXP::pink300.withAlpha (0.36f)
                                        : PinkXP::pink300.withAlpha (0.50f));
            if (isChrome || isGlass)
                appendLineSegment (snap->vertices, cx - R, baseY + 1.0f, cx + R, baseY + 1.0f, 0.36f,
                                   juce::Colours::white.withAlpha (darkBg ? 0.12f : 0.22f));

            // 45° 参考线 ×2 + 顶部中线 + 同心能量环（受 showRings 控制）
            if (showRings)
            {
                if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware)
                {
                    // Pixel XP：干净平滑低对比 45° 射线与中线（避免斜线锯齿）
                    const auto refC = PinkXP::pink200.withAlpha (0.45f);
                    appendLineSegment (snap->vertices, cx, baseY, cx - r45, baseY - r45, 0.5f, refC);
                    appendLineSegment (snap->vertices, cx, baseY, cx + r45, baseY - r45, 0.5f, refC);
                    appendLineSegment (snap->vertices, cx, baseY, cx, baseY - R, 0.5f, refC);
                }
                else
                {
                    appendGridLine (snap->vertices, cx, baseY, cx - r45, baseY - r45, false, PinkXP::pink300);
                    appendGridLine (snap->vertices, cx, baseY, cx + r45, baseY - r45, false, PinkXP::pink300);
                    appendGridLine (snap->vertices, cx, baseY, cx, baseY - R, true, PinkXP::pink300);
                }

                const float frac[3] = { 0.25f, 0.50f, 0.75f };
                const bool dashed = (tokens.gridStyle == PinkXP::GridStyle::dashedTech);
                const auto ringC = isChrome
                                       ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.38f).withAlpha (darkBg ? 0.30f : 0.25f)
                                       : isGlass
                                             ? PinkXP::pink300.interpolatedWith (juce::Colours::white, 0.20f).withAlpha (darkBg ? 0.24f : 0.28f)
                                             : isNeon
                                                   ? PinkXP::pink300.withAlpha (0.42f)
                                                   : isCrt
                                                         ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.42f)
                                                         : isModern
                                                               ? PinkXP::pink300.withAlpha (juce::jlimit (0.14f, 0.26f, tokens.gridMajorAlpha * 1.10f))
                                                               : PinkXP::pink200.withAlpha (0.34f);
                for (int ri = 0; ri < 3; ++ri)
                {
                    const float rr = R * frac[ri];
                    if (rr < 2.0f || rr >= R) continue;
                    for (int s = 0; s < arcSegs; ++s)
                    {
                        if (dashed && (s & 1) != 0) continue;
                        const float th0 = -kPi * 0.5f + kPi * (float) s / (float) arcSegs;
                        const float th1 = -kPi * 0.5f + kPi * (float) (s + 1) / (float) arcSegs;
                        const float x0 = cx + rr * std::sin (th0), y0 = baseY - rr * std::cos (th0);
                        const float x1 = cx + rr * std::sin (th1), y1 = baseY - rr * std::cos (th1);
                        if (isCrt && tokens.glowIntensity > 0.0f)
                            appendLineSegment (snap->vertices, x0, y0, x1, y1, 1.15f,
                                               ringC.withMultipliedAlpha (0.28f));
                        else if (isNeon)
                            appendLineSegment (snap->vertices, x0, y0, x1, y1, 1.15f,
                                               PinkXP::sel.withAlpha (0.18f));
                        appendLineSegment (snap->vertices, x0, y0, x1, y1, isModern ? 0.44f : 0.48f, ringC);
                    }
                }
            }
        }
    }
    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    snap->trailSegStart = (int) snap->segments.size();

    // ---- trail 点段（segments[1..]）：当前帧新点（GL / CPU 完全同构）----
    {
        std::vector<Pt> pts;
        collectPoints (plot, pts);
        if (! pts.empty())
        {
            const float hs        = juce::jmax (0.5f, dotSize * 0.5f);
            const bool greenScreen = PinkXP::isGreenScreenBackdrop();
            const bool pixelDots  = tokens.pixelSnapped;
            const bool userGlow   = (dotShape == 1) && ! greenScreen;
            const bool skinNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube) && ! greenScreen;
            const bool skinCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor) && ! greenScreen;
            const bool skinGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass) && ! greenScreen;
            const bool chromePin  = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome) && ! greenScreen && dotSize >= 1.5f;
            const float snapSize  = juce::jmax (1.0f, std::round (dotSize));
            const float haloR     = hs * (userGlow ? 2.1f : 1.75f) + (userGlow ? 0.8f : 0.6f);
            const float haloAlphaScale = (userGlow && skinNeon) ? 0.32f
                                       : (userGlow ? 0.24f : 0.24f * juce::jmax (0.4f, tokens.glowIntensity));
            const int discSegs = (dotSize >= 3.5f) ? 10 : 8;
            const float pinR   = juce::jmax (0.42f, hs * 0.42f);
            const float pinOff = hs * 0.18f;

            // 每个残影层一个段：低 / 中 / 高各自累积，颜色不互相污染
            for (int layer = 0; layer < layerCount; ++layer)
            {
                const int trailStartIdx = (int) snap->vertices.size() / 6;
                for (const auto& p : pts)
                {
                    if (p.layer != layer) continue;
                    const auto coreCol = greenScreen ? p.col.withAlpha (1.0f) : p.col;

                    if (pixelDots)
                    {
                        // Pixel XP / CRT：锐利整数对齐像素方块点（CRT 附加磷光外晕与亮芯）
                        const float qx = std::floor (p.x - hs);
                        const float qy = std::floor (p.y - hs);
                        if (skinCrt || userGlow)
                        {
                            const float pad = (skinCrt && userGlow) ? 2.0f : 1.0f;
                            appendQuad (snap->vertices,
                                        { qx - pad, qy - pad, snapSize + pad * 2.0f, snapSize + pad * 2.0f },
                                        coreCol.withMultipliedAlpha (skinCrt ? 0.28f : 0.22f));
                        }
                        appendQuad (snap->vertices, { qx, qy, snapSize, snapSize }, coreCol);
                        if (skinCrt && snapSize >= 2.0f)
                        {
                            const float cw = juce::jmax (1.0f, snapSize * 0.5f);
                            appendQuad (snap->vertices,
                                        { qx + snapSize * 0.25f, qy + snapSize * 0.25f, cw, cw },
                                        coreCol.interpolatedWith (juce::Colours::white, 0.52f)
                                               .withAlpha (coreCol.getFloatAlpha() * 0.60f));
                        }
                    }
                    else
                    {
                        // Modern / Chrome / Glass / Neon / Transparent：平滑圆点（Chrome/Glass 镜面高光芯；Neon/Glow 带圆晕；绿幕无外晕）
                        if ((userGlow || skinNeon) && ! greenScreen)
                            appendDisc (snap->vertices, p.x, p.y, haloR,
                                        coreCol.withMultipliedAlpha (haloAlphaScale), 8);
                        else if (skinGlass)
                            appendDisc (snap->vertices, p.x, p.y, hs * 1.45f + 0.4f,
                                        coreCol.withMultipliedAlpha (0.16f), 8);

                        if (dotSize >= 1.5f)
                            appendDisc (snap->vertices, p.x, p.y, hs, coreCol, discSegs);
                        else
                            appendQuad (snap->vertices, { p.x - hs, p.y - hs, dotSize, dotSize }, coreCol);

                        if (chromePin || (skinGlass && dotSize >= 1.5f))
                            appendDisc (snap->vertices, p.x - pinOff, p.y - pinOff, pinR,
                                        (skinGlass ? juce::Colours::white
                                                   : coreCol.interpolatedWith (juce::Colours::white, 0.72f))
                                            .withAlpha (coreCol.getFloatAlpha() * (skinGlass ? 0.62f : 0.85f)), 6);
                        else if (skinNeon && dotSize >= 2.0f)
                            appendDisc (snap->vertices, p.x, p.y, pinR,
                                        coreCol.interpolatedWith (juce::Colours::white, 0.72f)
                                               .withAlpha (coreCol.getFloatAlpha() * 0.78f), 6);
                    }
                }
                const int count = (int) snap->vertices.size() / 6 - trailStartIdx;
                if (count > 0)
                    snap->segments.push_back ({ trailStartIdx, count, Primitive::Triangles, layer });
            }
        }
    }

    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// 计算雷达几何
// ----------------------------------------------------------
StereoFieldModule::RadarGeometry
    StereoFieldModule::computeRadarGeometry (const juce::Rectangle<int>& plot)
{
    // 基准尺寸：取宽高较小者，用于推导随模块缩放的字号
    const float baseDim = (float) juce::jmin (plot.getWidth(), plot.getHeight());
    // 标签字号随模块尺寸线性缩放（小模块用小字，大模块用大字）
    const float labelFontSize = juce::jlimit (7.0f, 16.0f, baseDim * 0.06f);
    // 侧边留白固定 10px：让半圆直径端点（左下/右下角）贴近仪表区左右边界。
    const int sideMargin = 10;

    RadarGeometry g;
    g.cx            = plot.getCentreX();
    g.baseY         = plot.getBottom();
    g.labelFontSize = labelFontSize;
    g.sideMargin    = sideMargin;

    const int maxRx = plot.getWidth() / 2 - sideMargin;
    g.radius = juce::jmax (8, juce::jmin (maxRx, plot.getHeight()));
    return g;
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：入侵者（§8.3）
// ----------------------------------------------------------
bool StereoFieldModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto area = getContentBounds().reduced (4);
    if (! area.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = area.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void StereoFieldModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const auto canvas = getContentBounds().reduced (4);
    const auto plot   = canvas.reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    const auto geo = computeRadarGeometry (plot);
    ctx.addModuleActorCapped (
        makeStereoRadarSweepActor (hit.pos, canvas,
                                   { (float) geo.cx, (float) geo.baseY },
                                   (float) geo.radius),
        6);
}

// ----------------------------------------------------------
// 绘制半圆底衬
// ----------------------------------------------------------
void StereoFieldModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const auto& tokens = PinkXP::getSkinTokens();
    if (PinkXP::isTransparentBackdrop())
        return;
    if (PinkXP::isGreenScreenBackdrop())
    {
        g.setColour (PinkXP::chromaKeyGreen());
        g.fillRect (canvas);
        return;
    }

    PinkXP::Craft::drawPanelWell (g, canvas, PinkXP::content, PinkXP::WellKind::canvas);

    auto plot = canvas.reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    const auto geo = computeRadarGeometry (plot);
    const int  cx            = geo.cx;
    const int  baseY         = geo.baseY;
    const int  R             = geo.radius;
    const float labelFontSize = geo.labelFontSize;

    constexpr float kPi     = juce::MathConstants<float>::pi;
    constexpr float kHalfPi = juce::MathConstants<float>::halfPi;
    const float r45 = (float) R * 0.70710678f;  // 45° 处的弦长分量

    // ---- 方位参考（固定半径 R，不随 autoGain 缩放；全风格平滑反走样圆弧）----
    const bool isChrome = (tokens.dialFinish == PinkXP::DialFinish::brushedMetal);
    const bool isModern = (tokens.dialFinish == PinkXP::DialFinish::studioSmooth);
    const bool isGlass  = (tokens.dialFinish == PinkXP::DialFinish::glassDome);
    const bool isNeon   = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
    const bool isCrt    = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
    const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;

    {
        juce::Path arc;
        arc.addCentredArc ((float) cx, (float) baseY, (float) R, (float) R, 0.0f, -kHalfPi, kHalfPi, true);
        if (isChrome || isGlass)
        {
            if (isChrome)
            {
                juce::Path outerCut;
                outerCut.addCentredArc ((float) cx, (float) baseY, (float) R + 1.0f, (float) R + 1.0f, 0.0f, -kHalfPi, kHalfPi, true);
                g.setColour (PinkXP::dark.withAlpha (darkBg ? 0.38f : 0.20f));
                g.strokePath (outerCut, juce::PathStrokeType (1.05f));
            }
            g.setColour (isChrome
                             ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.42f).withAlpha (darkBg ? 0.74f : 0.66f)
                             : PinkXP::pink300.interpolatedWith (juce::Colours::white, 0.25f).withAlpha (darkBg ? 0.56f : 0.62f));
            g.strokePath (arc, juce::PathStrokeType (1.15f));

            const float rimOff = isGlass ? 1.4f : 1.7f;
            juce::Path outerRim;
            outerRim.addCentredArc ((float) cx, (float) baseY, (float) R + rimOff, (float) R + rimOff, 0.0f, -kHalfPi, kHalfPi, true);
            g.setColour (juce::Colours::white.withAlpha (darkBg ? 0.18f : 0.30f));
            g.strokePath (outerRim, juce::PathStrokeType (0.75f));

            if (isChrome && R >= 24)
            {
                const auto majC = PinkXP::ink.interpolatedWith (PinkXP::hl, darkBg ? 0.35f : 0.18f).withAlpha (0.58f);
                const auto minC = PinkXP::ink.withAlpha (darkBg ? 0.28f : 0.24f);
                for (int deg = -75; deg <= 75; deg += 15)
                {
                    const bool major = (deg % 45 == 0);
                    const float th   = (float) deg * (kPi / 180.0f);
                    const float sn   = std::sin (th), cs = std::cos (th);
                    const float len  = major ? 4.5f : 2.2f;
                    g.setColour (major ? majC : minC);
                    g.drawLine ((float) cx + ((float) R - len) * sn, (float) baseY - ((float) R - len) * cs,
                                (float) cx + (float) R * sn,         (float) baseY - (float) R * cs,
                                major ? 1.05f : 0.75f);
                }
            }
        }
        else
        {
            if (isNeon || (isCrt && tokens.glowIntensity > 0.0f))
            {
                g.setColour (PinkXP::sel.withAlpha (isNeon ? 0.28f : 0.22f * tokens.glowIntensity));
                g.strokePath (arc, juce::PathStrokeType (isNeon ? 4.0f : 3.1f));
            }
            g.setColour (isNeon   ? PinkXP::pink300.withAlpha (0.88f)
                       : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.86f)
                       : isModern ? PinkXP::pink300.withAlpha (0.48f)
                                  : PinkXP::pink300.withAlpha (0.65f));
            g.strokePath (arc, juce::PathStrokeType (isModern ? 1.0f : 1.2f));
            if (isNeon)
            {
                g.setColour (PinkXP::pink200.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.88f));
                g.strokePath (arc, juce::PathStrokeType (0.72f));
                if (R >= 24)
                {
                    const auto nGlow = PinkXP::sel.withAlpha (0.30f);
                    const auto nCore = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.50f).withAlpha (0.90f);
                    for (int deg : { -45, 0, 45 })
                    {
                        const float th = (float) deg * (kPi / 180.0f);
                        const float sn = std::sin (th), cs = std::cos (th);
                        g.setColour (nGlow);
                        g.drawLine ((float) cx + ((float) R - 4.5f) * sn, (float) baseY - ((float) R - 4.5f) * cs,
                                    (float) cx + ((float) R + 1.2f) * sn, (float) baseY - ((float) R + 1.2f) * cs, 2.5f);
                        g.setColour (nCore);
                        g.drawLine ((float) cx + ((float) R - 4.5f) * sn, (float) baseY - ((float) R - 4.5f) * cs,
                                    (float) cx + ((float) R + 1.2f) * sn, (float) baseY - ((float) R + 1.2f) * cs, 0.96f);
                    }
                }
            }
            else if (isCrt && R >= 24)
            {
                const auto majC = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.25f).withAlpha (0.76f);
                const auto minC = PinkXP::sel.withAlpha (0.40f);
                for (int deg = -75; deg <= 75; deg += 15)
                {
                    const bool major = (deg % 45 == 0);
                    const float th   = (float) deg * (kPi / 180.0f);
                    const float sn   = std::sin (th), cs = std::cos (th);
                    const float len  = major ? 4.5f : 2.2f;
                    g.setColour (major ? majC : minC);
                    g.drawLine ((float) cx + ((float) R - len) * sn, (float) baseY - ((float) R - len) * cs,
                                (float) cx + (float) R * sn,         (float) baseY - (float) R * cs,
                                major ? 1.05f : 0.75f);
                }
            }
        }

        if (isNeon || (isCrt && tokens.glowIntensity > 0.0f))
        {
            g.setColour (PinkXP::sel.withAlpha (isNeon ? 0.22f : 0.18f * tokens.glowIntensity));
            g.drawLine ((float) (cx - R), (float) baseY, (float) (cx + R), (float) baseY, 2.6f);
        }
        g.setColour (isChrome ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.36f).withAlpha (darkBg ? 0.56f : 0.50f)
                   : isGlass  ? PinkXP::pink300.interpolatedWith (juce::Colours::white, 0.22f).withAlpha (darkBg ? 0.46f : 0.52f)
                   : isNeon   ? PinkXP::pink300.withAlpha (0.76f)
                   : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.74f)
                   : isModern ? PinkXP::pink300.withAlpha (0.36f)
                              : PinkXP::pink300.withAlpha (0.50f));
        g.drawLine ((float) (cx - R), (float) baseY, (float) (cx + R), (float) baseY, isModern ? 0.9f : 1.0f);
        if (isChrome || isGlass)
        {
            g.setColour (juce::Colours::white.withAlpha (darkBg ? 0.12f : 0.22f));
            g.drawLine ((float) (cx - R), (float) baseY + 1.0f, (float) (cx + R), (float) baseY + 1.0f, 0.72f);
        }
    }

    // 45° 参考线（左 / 右）、顶部中线与能量环（受 showRings 控制）
    if (showRings)
    {
        if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware)
        {
            g.setColour (PinkXP::pink200.withAlpha (0.45f));
            g.drawLine ((float) cx, (float) baseY, (float) cx - r45, (float) baseY - r45, 1.0f);
            g.drawLine ((float) cx, (float) baseY, (float) cx + r45, (float) baseY - r45, 1.0f);
            g.drawLine ((float) cx, (float) baseY, (float) cx, (float) baseY - (float) R, 1.0f);
        }
        else
        {
            PinkXP::Craft::drawGridLine (g, (float) cx, (float) baseY, (float) cx - r45, (float) baseY - r45, false, PinkXP::pink300);
            PinkXP::Craft::drawGridLine (g, (float) cx, (float) baseY, (float) cx + r45, (float) baseY - r45, false, PinkXP::pink300);
            PinkXP::Craft::drawGridLine (g, (float) cx, (float) baseY, (float) cx, (float) baseY - (float) R, true, PinkXP::pink300);
        }

        const float innerFrac[3] = { 0.25f, 0.50f, 0.75f };
        const auto ringC = isChrome
                               ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.38f).withAlpha (darkBg ? 0.30f : 0.25f)
                               : isGlass
                                     ? PinkXP::pink300.interpolatedWith (juce::Colours::white, 0.20f).withAlpha (darkBg ? 0.24f : 0.28f)
                                     : isNeon
                                           ? PinkXP::pink300.withAlpha (0.42f)
                                           : isCrt
                                                 ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.42f)
                                                 : isModern
                                                       ? PinkXP::pink300.withAlpha (juce::jlimit (0.14f, 0.26f, tokens.gridMajorAlpha * 1.10f))
                                                       : PinkXP::pink200.withAlpha (0.34f);
        for (int ri = 0; ri < 3; ++ri)
        {
            const float rr = (float) R * innerFrac[ri];
            if (rr < 2.0f || rr >= (float) R) continue;

            if (tokens.gridStyle == PinkXP::GridStyle::dashedTech)
            {
                constexpr int arcSegs = 64;
                for (int s = 0; s < arcSegs; s += 2)
                {
                    const float th0 = -kHalfPi + kPi * (float) s / (float) arcSegs;
                    const float th1 = -kHalfPi + kPi * (float) (s + 1) / (float) arcSegs;
                    const float x0 = (float) cx + rr * std::sin (th0), y0 = (float) baseY - rr * std::cos (th0);
                    const float x1 = (float) cx + rr * std::sin (th1), y1 = (float) baseY - rr * std::cos (th1);
                    if (isNeon)
                    {
                        g.setColour (PinkXP::sel.withAlpha (0.18f));
                        g.drawLine (x0, y0, x1, y1, 2.3f);
                    }
                    g.setColour (ringC);
                    g.drawLine (x0, y0, x1, y1, 0.95f);
                }
            }
            else
            {
                juce::Path arc;
                arc.addCentredArc ((float) cx, (float) baseY, rr, rr, 0.0f, -kHalfPi, kHalfPi, true);
                if (isCrt && tokens.glowIntensity > 0.0f)
                {
                    g.setColour (ringC.withMultipliedAlpha (0.28f));
                    g.strokePath (arc, juce::PathStrokeType (2.3f));
                }
                g.setColour (ringC);
                g.strokePath (arc, juce::PathStrokeType (isModern ? 0.88f : 0.95f));
            }
        }

        if (showLabels)
        {
            const float frac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
            g.setColour (PinkXP::ink.withAlpha (0.55f));
            g.setFont (PinkXP::getAxisFont (juce::jmax (6.0f, labelFontSize - 1.5f),
                                            juce::Font::plain));
            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = (float) R * frac[ri];
                if (rr < 8.0f || rr > (float) R) continue;
                const int ly = baseY - (int) rr - 6;
                g.drawText (y2k::tr (ringDbLabel (frac[ri], autoGainFactor)), cx + 4, ly, 34, 12,
                            juce::Justification::centredLeft, false);
            }
        }
    }

    // ---- 声像方位刻度：L / C / R 与缩放倍率（受 showLabels 控制）----
    if (showLabels)
    {
        const int actualMargin = plot.getWidth() / 2 - R;
        const int labelH = juce::jmax (10, (int) std::ceil (labelFontSize) + 4);
        const int y      = baseY - labelH;

        g.setColour (PinkXP::ink.withAlpha (0.7f));
        g.setFont (PinkXP::getAxisFont (labelFontSize, juce::Font::bold));
        g.drawText (y2k::tr ("C"), cx - 8, y, 16, labelH,
                    juce::Justification::centred, false);

        g.setFont (PinkXP::getAxisFont (juce::jmin (labelFontSize, 11.0f),
                                        juce::Font::bold));
        g.drawText (y2k::tr ("L"), plot.getX(), y, actualMargin, labelH,
                    juce::Justification::centredRight, false);
        g.drawText (y2k::tr ("R"), plot.getRight() - actualMargin, y, actualMargin, labelH,
                    juce::Justification::centredLeft, false);

        if (std::abs (autoGainFactor - 1.0f) > 0.05f)
        {
            g.setColour (PinkXP::ink.withAlpha (0.6f));
            g.setFont (PinkXP::getAxisFont (8.0f, juce::Font::plain));
            g.drawText (y2k::tr (juce::String (autoGainFactor, 1) + "x"),
                        canvas.getRight() - 48, canvas.getY() + 3, 44, 12,
                        juce::Justification::centredRight, false);
        }
    }
}

// ----------------------------------------------------------
// updateTrail —— 衰减旧点 + 绘制新点
// ----------------------------------------------------------
void StereoFieldModule::updateTrail (juce::Rectangle<int> canvas)
{
    const int cw = canvas.getWidth();
    const int ch = canvas.getHeight();
    if (cw <= 0 || ch <= 0) return;

    const float diag  = std::sqrt ((float) (cw * cw + ch * ch));
    const float scale = juce::jlimit (0.25f, 1.0f, 700.0f / juce::jmax (700.0f, diag));
    const int   rw    = juce::jmax (8, (int) std::lround ((float) cw * scale));
    const int   rh    = juce::jmax (8, (int) std::lround ((float) ch * scale));

    const int   layers = bandColour ? kTrailLayers : 1;

    const float decay = std::pow (kDecayPerFrame, juce::jmax (0.0f, cpuTrailDtAccumSec) * 60.0f * fadeSpeed);
    cpuTrailDtAccumSec = 0.0f;

    for (int li = 0; li < kTrailLayers; ++li)
    {
        if (li >= layers)
        {
            trailImage[li] = juce::Image();
            continue;
        }

        auto& img = trailImage[li];
        if (img.isNull() || img.getWidth() != rw || img.getHeight() != rh)
        {
            img = juce::Image (juce::Image::ARGB, rw, rh, true, juce::SoftwareImageType());
            img.clear (img.getBounds(), juce::Colours::transparentBlack);
        }

        if (decay < 0.999f)
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
            for (int y = 0; y < bd.height; ++y)
            {
                auto* line = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer (y));
                for (int x = 0; x < bd.width; ++x)
                {
                    auto& p = line[x];
                    const auto a = p.getAlpha();
                    if (a == 0) continue;
                    if (a <= 2)
                    {
                        p.setARGB (0, 0, 0, 0);
                        continue;
                    }
                    p.multiplyAlpha (decay);
                    if (p.getAlpha() == a)
                        p.multiplyAlpha ((float) (a - 1) / (float) a);
                    if (p.getAlpha() <= 2)
                        p.setARGB (0, 0, 0, 0);
                }
            }
        }
    }

    auto plot = juce::Rectangle<int> (0, 0, cw, ch).reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    std::vector<Pt> pts;
    collectPoints (plot, pts);
    if (pts.empty()) return;

    const auto& tokens     = PinkXP::getSkinTokens();
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool pixelDots   = tokens.pixelSnapped;
    const bool userGlow    = (dotShape == 1) && ! greenScreen;
    const bool skinNeon    = (tokens.frameStyle == PinkXP::FrameStyle::neonTube) && ! greenScreen;
    const bool skinCrt     = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor) && ! greenScreen;
    const bool skinGlass   = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass) && ! greenScreen;
    const bool chromePin   = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome) && ! greenScreen && dotSize >= 1.5f;
    const float ds         = dotSize;
    const float hs         = juce::jmax (0.5f, ds * 0.5f);
    const float snapSize   = juce::jmax (1.0f, std::round (ds));
    const float haloR      = hs * (userGlow ? 2.1f : 1.75f) + (userGlow ? 0.8f : 0.6f);
    const float haloAlphaScale = (userGlow && skinNeon) ? 0.32f
                               : (userGlow ? 0.24f : 0.24f * juce::jmax (0.4f, tokens.glowIntensity));
    const float pinR       = juce::jmax (0.42f, hs * 0.42f);
    const float pinOff     = hs * 0.18f;

    for (int li = 0; li < layers; ++li)
    {
        if (! trailImage[li].isValid()) continue;

        juce::Graphics tg (trailImage[li]);
        tg.addTransform (juce::AffineTransform::scale (scale));

        if (pixelDots)
        {
            const float pad = (skinCrt && userGlow) ? 2.0f : 1.0f;
            const float cw  = juce::jmax (1.0f, snapSize * 0.5f);
            if (bandColour)
            {
                for (const auto& p : pts)
                {
                    if (p.layer != li) continue;
                    const auto coreCol = greenScreen ? p.col.withAlpha (1.0f) : p.col;
                    const float qx = std::floor (p.x - hs);
                    const float qy = std::floor (p.y - hs);
                    if (skinCrt || userGlow)
                    {
                        tg.setColour (coreCol.withMultipliedAlpha (skinCrt ? 0.28f : 0.22f));
                        tg.fillRect (qx - pad, qy - pad, snapSize + pad * 2.0f, snapSize + pad * 2.0f);
                    }
                    tg.setColour (coreCol);
                    tg.fillRect (qx, qy, snapSize, snapSize);
                    if (skinCrt && snapSize >= 2.0f)
                    {
                        tg.setColour (coreCol.interpolatedWith (juce::Colours::white, 0.52f)
                                             .withAlpha (coreCol.getFloatAlpha() * 0.60f));
                        tg.fillRect (qx + snapSize * 0.25f, qy + snapSize * 0.25f, cw, cw);
                    }
                }
            }
            else
            {
                const auto baseCol = PinkXP::pink500.withAlpha (greenScreen ? 1.0f : 0.9f);
                if (skinCrt || userGlow)
                {
                    juce::RectangleList<float> halos;
                    halos.ensureStorageAllocated ((int) pts.size());
                    for (const auto& p : pts)
                    {
                        const float qx = std::floor (p.x - hs), qy = std::floor (p.y - hs);
                        halos.addWithoutMerging ({ qx - pad, qy - pad, snapSize + pad * 2.0f, snapSize + pad * 2.0f });
                    }
                    tg.setColour (baseCol.withMultipliedAlpha (skinCrt ? 0.28f : 0.22f));
                    tg.fillRectList (halos);
                }
                juce::RectangleList<float> rects;
                rects.ensureStorageAllocated ((int) pts.size());
                for (const auto& p : pts)
                    rects.addWithoutMerging ({ std::floor (p.x - hs), std::floor (p.y - hs), snapSize, snapSize });
                tg.setColour (baseCol);
                tg.fillRectList (rects);
                if (skinCrt && snapSize >= 2.0f)
                {
                    juce::RectangleList<float> cores;
                    cores.ensureStorageAllocated ((int) pts.size());
                    for (const auto& p : pts)
                        cores.addWithoutMerging ({ std::floor (p.x - hs) + snapSize * 0.25f,
                                                   std::floor (p.y - hs) + snapSize * 0.25f, cw, cw });
                    tg.setColour (baseCol.interpolatedWith (juce::Colours::white, 0.52f)
                                         .withAlpha (baseCol.getFloatAlpha() * 0.60f));
                    tg.fillRectList (cores);
                }
            }
            continue;
        }

        // 平滑风格（Modern / Chrome / Glass / Neon）：圆形散点（与 GL 同构）
        if (bandColour)
        {
            for (const auto& p : pts)
            {
                if (p.layer != li) continue;
                const auto coreCol = greenScreen ? p.col.withAlpha (1.0f) : p.col;
                if ((userGlow || skinNeon) && ! greenScreen)
                {
                    tg.setColour (coreCol.withMultipliedAlpha (haloAlphaScale));
                    tg.fillEllipse (p.x - haloR, p.y - haloR, haloR * 2.0f, haloR * 2.0f);
                }
                else if (skinGlass)
                {
                    const float gh = hs * 1.45f + 0.4f;
                    tg.setColour (coreCol.withMultipliedAlpha (0.16f));
                    tg.fillEllipse (p.x - gh, p.y - gh, gh * 2.0f, gh * 2.0f);
                }
                tg.setColour (coreCol);
                if (ds >= 1.5f)
                    tg.fillEllipse (p.x - hs, p.y - hs, ds, ds);
                else
                    tg.fillRect (p.x - hs, p.y - hs, ds, ds);
                if (chromePin || (skinGlass && ds >= 1.5f))
                {
                    tg.setColour ((skinGlass ? juce::Colours::white
                                             : coreCol.interpolatedWith (juce::Colours::white, 0.72f))
                                      .withAlpha (coreCol.getFloatAlpha() * (skinGlass ? 0.62f : 0.85f)));
                    tg.fillEllipse (p.x - pinOff - pinR, p.y - pinOff - pinR, pinR * 2.0f, pinR * 2.0f);
                }
                else if (skinNeon && ds >= 2.0f)
                {
                    tg.setColour (coreCol.interpolatedWith (juce::Colours::white, 0.72f)
                                         .withAlpha (coreCol.getFloatAlpha() * 0.78f));
                    tg.fillEllipse (p.x - pinR, p.y - pinR, pinR * 2.0f, pinR * 2.0f);
                }
            }
        }
        else
        {
            const auto baseCol = PinkXP::pink500.withAlpha (greenScreen ? 1.0f : 0.9f);
            if ((userGlow || skinNeon) && ! greenScreen)
            {
                tg.setColour (baseCol.withMultipliedAlpha (haloAlphaScale));
                for (const auto& p : pts)
                    tg.fillEllipse (p.x - haloR, p.y - haloR, haloR * 2.0f, haloR * 2.0f);
            }
            else if (skinGlass)
            {
                const float gh = hs * 1.45f + 0.4f;
                tg.setColour (baseCol.withMultipliedAlpha (0.16f));
                for (const auto& p : pts)
                    tg.fillEllipse (p.x - gh, p.y - gh, gh * 2.0f, gh * 2.0f);
            }
            tg.setColour (baseCol);
            if (ds >= 1.5f)
            {
                for (const auto& p : pts)
                    tg.fillEllipse (p.x - hs, p.y - hs, ds, ds);
            }
            else
            {
                juce::RectangleList<float> rects;
                rects.ensureStorageAllocated ((int) pts.size());
                for (const auto& p : pts)
                    rects.addWithoutMerging ({ p.x - hs, p.y - hs, ds, ds });
                tg.fillRectList (rects);
            }
            if (chromePin || (skinGlass && ds >= 1.5f))
            {
                tg.setColour ((skinGlass ? juce::Colours::white
                                         : baseCol.interpolatedWith (juce::Colours::white, 0.72f))
                                  .withAlpha (baseCol.getFloatAlpha() * (skinGlass ? 0.62f : 0.85f)));
                for (const auto& p : pts)
                    tg.fillEllipse (p.x - pinOff - pinR, p.y - pinOff - pinR, pinR * 2.0f, pinR * 2.0f);
            }
            else if (skinNeon && ds >= 2.0f)
            {
                tg.setColour (baseCol.interpolatedWith (juce::Colours::white, 0.72f)
                                     .withAlpha (baseCol.getFloatAlpha() * 0.78f));
                for (const auto& p : pts)
                    tg.fillEllipse (p.x - pinR, p.y - pinR, pinR * 2.0f, pinR * 2.0f);
            }
        }
    }
}

void StereoFieldModule::invalidateTrail()
{
    for (auto& img : trailImage)
        img = juce::Image();
    trailNeedsUpdate   = true;
    trailNeedsClear    = true;   // GL 侧也要清：读档 / 改染色 / 切背景后不让旧点残留
    cpuTrailDtAccumSec = 0.0f;
    glForceTrailClear.store (true, std::memory_order_release);
}
