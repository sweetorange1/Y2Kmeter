#include "source/ui/modules/DynamicsModule.h"
#include <cmath>

#include "source/ui/modules/MeterWidgets.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// DynamicsModule（动态模块）
// ==========================================================

juce::Colour DynamicsModule::dbToColour (float db)
{
    return y2k::meter::dbColour (db);
}

DynamicsModule::DynamicsModule(AnalyserHub& h)
    : ModulePanel(ModuleType::dynamics),
      hub(h)
{
    // Phase F：订阅 Dynamics + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Dynamics);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // dynamics 6×4 大格
    setControlPanelEnabled (true);

    bPeakL.label = "PK L";
    bPeakR.label = "PK R";
    bRmsL .label = "RMS L";
    bRmsR .label = "RMS R";

    crestHist.assign ((size_t) crestHistLen, 0.0f);

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        for (int i = 0; i < 3; ++i)
        {
            drValueCache_[i] = {};
            drValueText_[i].clear();
            drValueTime_[i] = 0.0;
        }
        repaint();
    });
}

DynamicsModule::~DynamicsModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Dynamics);
}

// ----------------------------------------------------------
// 持久化 / 控制面板
// ----------------------------------------------------------
juce::ValueTree DynamicsModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("itemPeak",    items.peak,    nullptr);
    s.setProperty ("itemRms",     items.rms,     nullptr);
    s.setProperty ("itemDrShort", items.drShort, nullptr);
    s.setProperty ("itemDrInt",   items.drInteg, nullptr);
    s.setProperty ("itemCrest",   items.crest,   nullptr);
    s.setProperty ("itemHist",    items.hist,    nullptr);
    s.setProperty ("style",       (int) readoutStyle, nullptr);
    s.setProperty ("histSpan",    (double) histSpanSec, nullptr);
    s.setProperty ("barFloorDb",  (double) barFloorDb, nullptr);
    s.setProperty ("peakHold",    showPeakHold, nullptr);
    s.setProperty ("histStyle",   histStyle, nullptr);
    s.setProperty ("histMaxDb",   (double) histMaxDb, nullptr);
    return s;
}

void DynamicsModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    // 缺键 = 保持当前值（构造默认 = 旧 Dynamics 外观；种子只覆盖它关心的键）
    auto getB = [&] (const char* k, bool& dst)
    {
        if (state.hasProperty (k)) dst = (bool) state.getProperty (k);
    };

    Items it = items;
    getB ("itemPeak", it.peak);       getB ("itemRms", it.rms);
    getB ("itemDrShort", it.drShort); getB ("itemDrInt", it.drInteg);
    getB ("itemCrest", it.crest);     getB ("itemHist", it.hist);
    if (! it.any()) it = Items();   // 防御：存档里全关 → 回到默认

    if (it.hist && ! items.hist)
    {
        std::fill (crestHist.begin(), crestHist.end(), 0.0f);
        crestHistWrite = 0;
        crestHistAccum = 0.0f;
        crestHistTotal = 0;
    }
    items = it;

    if (state.hasProperty ("style"))
        readoutStyle = (int) state.getProperty ("style") == 1 ? ReadoutStyle::barReadout
                                                              : ReadoutStyle::bigDigits;
    if (state.hasProperty ("histSpan"))
        histSpanSec = juce::jlimit (1.0f, kMaxSpanSec, (float) (double) state.getProperty ("histSpan"));
    if (state.hasProperty ("barFloorDb"))
        barFloorDb = juce::jlimit (-72.0f, -36.0f, (float) (double) state.getProperty ("barFloorDb"));
    if (state.hasProperty ("peakHold"))
        showPeakHold = (bool) state.getProperty ("peakHold");
    if (state.hasProperty ("histStyle"))
        histStyle = juce::jlimit (0, 1, (int) state.getProperty ("histStyle"));
    if (state.hasProperty ("histMaxDb"))
        histMaxDb = juce::jlimit (15.0f, 40.0f, (float) (double) state.getProperty ("histMaxDb"));

    layoutContent (getContentBounds());
    refreshControlPanel();
    repaint();
}

void DynamicsModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::toggle ("itemPeak", "Peak bars",
        "Sample-peak level bars for the left and right channel, with a slowly falling hold line.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemRms", "RMS bars",
        "Average (RMS) level bars for the left and right channel.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemDrShort", "DR short-term",
        "Dynamic range of the recent programme: loudest 20% against the average.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemDrInt", "DR integrated",
        "Dynamic range over the whole programme. Resets after a long silence.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemCrest", "Crest factor",
        "Peak minus RMS in dB right now. Low = squashed, high = punchy.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemHist", "Crest history graph",
        "Scrolling graph of the crest factor over time.").in ("Show"));

    out.push_back (ParamDef::choice ("barFloorDb", "Meter floor",
        { "-72 dB", "-60 dB", "-48 dB", "-36 dB" },
        "Lowest level shown at the bottom of the Peak and RMS bars.")
        .in ("Level bars").when ([this] { return items.barCount() > 0; }));
    out.push_back (ParamDef::toggle ("peakHold", "Peak hold line",
        "Show the slowly falling peak-hold marker on the level bars.")
        .in ("Level bars").when ([this] { return items.barCount() > 0; }));

    out.push_back (ParamDef::choice ("style", "DR readout style",
        { "Big digits", "Bars + digits" },
        "Big digits: large numbers that fill the cell. Bars + digits: smaller numbers with a progress bar.")
        .in ("Style").when ([this] { return items.drCount() > 0; }));

    out.push_back (ParamDef::slider ("histSpan", "History span", 1.0, kMaxSpanSec, 1.0, " s",
        "How much time the crest history graph covers.")
        .in ("History").when ([this] { return items.hist; }));
    out.push_back (ParamDef::choice ("histStyle", "History style",
        { "Filled", "Curve" },
        "Filled: shaded area under the crest curve. Curve: clean line only.")
        .in ("History").when ([this] { return items.hist; }));
    out.push_back (ParamDef::slider ("histMaxDb", "Crest max", 15.0, 40.0, 5.0, " dB",
        "Top of the vertical crest scale. Lower zooms in on squashed masters; higher fits sharp transients.")
        .in ("History").when ([this] { return items.hist; }));
}

double DynamicsModule::getParamValue (const juce::String& id) const
{
    if (id == "itemPeak")    return items.peak ? 1.0 : 0.0;
    if (id == "itemRms")     return items.rms ? 1.0 : 0.0;
    if (id == "itemDrShort") return items.drShort ? 1.0 : 0.0;
    if (id == "itemDrInt")   return items.drInteg ? 1.0 : 0.0;
    if (id == "itemCrest")   return items.crest ? 1.0 : 0.0;
    if (id == "itemHist")    return items.hist ? 1.0 : 0.0;
    if (id == "barFloorDb")
    {
        if (barFloorDb <= -66.0f) return 0.0;
        if (barFloorDb <= -54.0f) return 1.0;
        if (barFloorDb <= -42.0f) return 2.0;
        return 3.0;
    }
    if (id == "peakHold")    return showPeakHold ? 1.0 : 0.0;
    if (id == "style")       return (double) (int) readoutStyle;
    if (id == "histSpan")    return (double) histSpanSec;
    if (id == "histStyle")   return (double) histStyle;
    if (id == "histMaxDb")   return (double) histMaxDb;
    return 0.0;
}

void DynamicsModule::setParamValue (const juce::String& id, double v)
{
    const bool on = v >= 0.5;
    Items it = items;

    if      (id == "itemPeak")    it.peak = on;
    else if (id == "itemRms")     it.rms = on;
    else if (id == "itemDrShort") it.drShort = on;
    else if (id == "itemDrInt")   it.drInteg = on;
    else if (id == "itemCrest")   it.crest = on;
    else if (id == "itemHist")    it.hist = on;
    else if (id == "barFloorDb")
    {
        static constexpr float kFloors[4] = { -72.0f, -60.0f, -48.0f, -36.0f };
        barFloorDb = kFloors[juce::jlimit (0, 3, juce::roundToInt (v))];
        repaint();
        return;
    }
    else if (id == "peakHold")
    {
        showPeakHold = on;
        repaint();
        return;
    }
    else if (id == "style")
    {
        readoutStyle = juce::roundToInt (v) == 1 ? ReadoutStyle::barReadout : ReadoutStyle::bigDigits;
        repaint();
        return;
    }
    else if (id == "histSpan")
    {
        histSpanSec = juce::jlimit (1.0f, kMaxSpanSec, (float) v);
        repaint();
        return;
    }
    else if (id == "histStyle")
    {
        histStyle = juce::jlimit (0, 1, juce::roundToInt (v));
        repaint();
        return;
    }
    else if (id == "histMaxDb")
    {
        histMaxDb = juce::jlimit (15.0f, 40.0f, (float) v);
        repaint();
        return;
    }
    else return;

    if (! it.any())
        return;   // 至少保留一项，避免出现空白模块

    if (it.hist && ! items.hist)
    {
        std::fill (crestHist.begin(), crestHist.end(), 0.0f);
        crestHistWrite = 0;
        crestHistAccum = 0.0f;
        crestHistTotal = 0;
    }
    items = it;
    layoutContent (getContentBounds());
    repaint();
}

bool DynamicsModule::drRowEnabled (int row) const noexcept
{
    return row == rowShort ? items.drShort : row == rowInteg ? items.drInteg : items.crest;
}

float DynamicsModule::drRowValue (int row) const noexcept
{
    return row == rowShort ? smoothedShortDR : row == rowInteg ? smoothedIntegDR : smoothedCrest;
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调（UI 线程）
// ----------------------------------------------------------
void DynamicsModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Dynamics)) return;

    // v2.9：使用分发器提供的高精度帧间隔
    const float dtSec = frame.dtSec;

    const auto& s = frame.dynamics;
    if (items.peak) { bPeakL.update(s.peakL, dtSec); bPeakR.update(s.peakR, dtSec); }
    if (items.rms)  { bRmsL .update(s.rmsL,  dtSec); bRmsR .update(s.rmsR,  dtSec); }

    auto smoothNum = [dtSec](float cur, float target, float a)
    {
        if (! std::isfinite(target)) target = 0.0f;
        return cur + y2k::pacing::alphaForDt (a, dtSec) * (target - cur);
    };
    smoothedShortDR = smoothNum(smoothedShortDR, s.shortDR,      0.2f);
    smoothedIntegDR = smoothNum(smoothedIntegDR, s.integratedDR, 0.05f);
    smoothedCrest   = smoothNum(smoothedCrest,   s.crest,        0.3f);

    // 带进度条读数样式的闪白 / 心跳（数值刚变化时闪一下）
    if (readoutStyle == ReadoutStyle::barReadout && items.drCount() > 0)
    {
        const float th = juce::jmax (0.05f, dtSec * y2k::pacing::kRefHz);
        for (int r = 0; r < numDrRows; ++r)
        {
            const float cur = drRowValue (r);
            if (std::abs (cur - prevDr[r]) > 0.3f * th) flash[r] = 1.0f;
            prevDr[r] = cur;
            flash[r] = juce::jmax (0.0f, flash[r] - y2k::pacing::stepForDt (0.08f, dtSec));
        }
        pulsePhase += y2k::pacing::stepForDt (0.24f, dtSec);
        while (pulsePhase > juce::MathConstants<float>::twoPi)
            pulsePhase -= juce::MathConstants<float>::twoPi;
    }

    // 历史带写入（按时间推进，帧率无关；仅历史开启时）
    if (items.hist)
    {
        crestHistAccum += dtSec * (float) kHistRate;
        int pushes = juce::jmin ((int) crestHistAccum, crestHistLen);
        crestHistAccum -= (float) (int) crestHistAccum;
        const float crestVal = juce::jlimit(0.0f, 40.0f, s.crest);
        while (pushes-- > 0)
        {
            crestHist[(size_t) crestHistWrite] = crestVal;
            crestHistWrite = (crestHistWrite + 1) % crestHistLen;
            ++crestHistTotal;
        }
    }

    // v2.9.2：读数类模块 30Hz 重绘（数据每帧更新，仅重绘分频）
    if ((frame.tickCount & 1ull) != 0)
        return;
    repaintFrameContent();
}

// ----------------------------------------------------------
// layoutContent —— 只排启用的区段（四项全开 = 旧 Dynamics 布局）
// ----------------------------------------------------------
void DynamicsModule::layoutContent(juce::Rectangle<int> content)
{
    areaMeters = areaDr = areaCrest = {};

    const bool hasM = items.barCount() > 0;
    const bool hasD = items.drCount() > 0;
    const bool hasH = items.hist;
    const int  n    = (hasM ? 1 : 0) + (hasD ? 1 : 0) + (hasH ? 1 : 0);
    if (n == 0) return;

    constexpr int gap = 6;

    if (n == 1)
    {
        (hasM ? areaMeters : hasD ? areaDr : areaCrest) = content;
        return;
    }

    // 每根柱 40px（四柱 = 旧版 160px），DR 区 180px
    const int metersW = 40 * juce::jmax (1, items.barCount());
    const int drW     = 180;

    if (hasH)
    {
        // 与旧 Dynamics 同一套：历史带吃剩余宽度，窗口过窄时兜底 120px 或 30%
        const int leftW = (hasM ? metersW : 0) + (hasD ? drW : 0) + ((hasM && hasD) ? gap : 0);
        const int minCrestW = 120;
        int crestW = content.getWidth() - leftW - gap;
        if (crestW < minCrestW)
            crestW = juce::jmax (minCrestW, content.getWidth() * 3 / 10);

        areaCrest = content.withX (content.getRight() - crestW).withWidth (crestW);

        if (hasM && hasD)
        {
            areaMeters = content.withWidth (metersW);
            areaDr     = content.withTrimmedLeft (metersW + gap).withTrimmedRight (crestW + gap);
        }
        else
        {
            auto left = content.withTrimmedRight (crestW + gap);
            (hasM ? areaMeters : areaDr) = left;
        }
        return;
    }

    // 没有历史带：柱组与 DR 区按权重瓜分
    const int total = content.getWidth() - gap;
    const int mW = juce::jmax (30, total * metersW / (metersW + drW));
    areaMeters = content.withWidth (mW);
    areaDr     = content.withTrimmedLeft (mW + gap);
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：综合路由（Meters/DR/Crest，§8.2）
// ----------------------------------------------------------
bool DynamicsModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    if (areaCrest.contains (panelPos))      { out.kind = 3; out.rect = areaCrest.toFloat(); }
    else if (areaDr.contains (panelPos))    { out.kind = 2; out.rect = areaDr.toFloat(); }
    else if (areaMeters.contains (panelPos)){ out.kind = 1; out.rect = areaMeters.toFloat(); }
    else return false;
    out.pos = panelPos.toFloat();
    return true;
}

void DynamicsModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    switch (hit.kind)
    {
        case 1:
        {
            // 精确定位被点击的单根 Peak / RMS 柱体（与 drawMeterGroup 同口径）
            const int barCount = items.barCount();
            if (barCount <= 0 || areaMeters.isEmpty())
                return;
            constexpr int gap = 4;
            constexpr int labelH = 14;
            const auto barRow = areaMeters.withTrimmedBottom (labelH);
            const int barW = juce::jmax (8, (barRow.getWidth() - gap * (barCount - 1)) / barCount);
            int idx = juce::jlimit (0, barCount - 1,
                (int) std::floor ((hit.pos.x - (float) barRow.getX()) / (float) (barW + gap)));
            const juce::Rectangle<float> barRect ((float) (barRow.getX() + idx * (barW + gap)),
                                                  (float) barRow.getY(),
                                                  (float) barW,
                                                  (float) barRow.getHeight());
            const bool isRmsBar = items.rms && (! items.peak || idx >= 2);
            const juce::Colour col = isRmsBar ? PinkXP::accent2 : PinkXP::sel;
            ctx.addModuleActorCapped (makeMeterBarLaunchActor (barRect, hit.pos.y, col), 6);
            break;
        }
        case 2:
        {
            // 精确定位被点击的 DR 读数子框（兼容 bigDigits / barReadout 两种样式）
            const int n = items.drCount();
            if (n <= 0 || areaDr.isEmpty())
                return;
            juce::Rectangle<int> cell = areaDr;
            if (readoutStyle == ReadoutStyle::barReadout)
            {
                auto a = areaDr;
                const int h = juce::jmax (44, a.getHeight() / n - 3);
                for (int slot = 0; slot < n; ++slot)
                {
                    const bool last = (slot == n - 1);
                    auto c = last ? a : a.removeFromTop (h);
                    if (! last) a.removeFromTop (3);
                    if (c.contains (hit.pos.toInt()) || last) { cell = c; break; }
                }
            }
            else
            {
                const auto inner = areaDr.reduced (6);
                const int rowH = juce::jmax (1, inner.getHeight() / n);
                const int slot = juce::jlimit (0, n - 1, (int) ((hit.pos.y - (float) inner.getY()) / (float) rowH));
                cell = { inner.getX(), inner.getY() + slot * rowH, inner.getWidth(), rowH };
            }
            ctx.addModuleActorCapped (makeCompressorClampActor (cell.toFloat(), hit.pos), 6);
            break;
        }
        default:
        {
            // 采样当前 Crest 历史折线（与 drawCrestBand 同口径）供双荧光光珠冲浪
            const auto inner = areaCrest.reduced (4);
            const auto plot  = inner.withTrimmedTop (14);
            if (plot.getWidth() < 8 || plot.getHeight() < 8)
                return;

            constexpr int kBins = 96;
            std::vector<float> curveY ((size_t) kBins, (float) plot.getBottom());
            const int N = juce::jlimit (8, crestHistLen, juce::roundToInt (histSpanSec * (float) kHistRate));
            const juce::int64 winStartAbs = (juce::int64) crestHistTotal - (juce::int64) N;
            auto ringAt = [this] (juce::int64 absIdx) noexcept
            {
                if (absIdx < 0) return 0.0f;
                return crestHist[(size_t) (absIdx % (juce::int64) crestHistLen)];
            };

            const float h = (float) plot.getHeight();
            for (int i = 0; i < kBins; ++i)
            {
                const juce::int64 s0 = winStartAbs + (juce::int64) std::llround ((double) i / (double) kBins * (double) N);
                const juce::int64 s1 = juce::jmax (s0 + 1, winStartAbs + (juce::int64) std::llround ((double) (i + 1) / (double) kBins * (double) N));
                const juce::int64 a0 = juce::jmax (s0, winStartAbs);
                const juce::int64 a1 = juce::jmin (s1, (juce::int64) crestHistTotal);
                float v = 0.0f;
                for (juce::int64 a = a0; a < a1; ++a)
                    v = juce::jmax (v, ringAt (a));
                const float norm = juce::jlimit (0.0f, 1.0f, v / histMaxDb);
                curveY[(size_t) i] = (float) plot.getBottom() - norm * h;
            }
            ctx.addModuleActorCapped (makeCrestSurferActor (hit.pos, plot, curveY), 6);
            break;
        }
    }
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
void DynamicsModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    const auto& tokens = PinkXP::getSkinTokens();
    if (PinkXP::isGreenScreenBackdrop())
    {
        g.setColour (PinkXP::chromaKeyGreen());
        g.fillRect (content);
    }
    else if (! PinkXP::isTransparentBackdrop())
    {
        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            juce::ColourGradient grad (PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.05f : 0.11f),
                                       0.0f, (float) content.getY(),
                                       PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.06f : 0.04f),
                                       0.0f, (float) content.getBottom(), false);
            grad.addColour (0.38, PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.03f : 0.06f));
            grad.addColour (0.52, PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.03f : 0.02f));
            g.setGradientFill (grad);
            g.fillRect (content);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            juce::ColourGradient grad (PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.06f : 0.15f),
                                       0.0f, (float) content.getY(),
                                       PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.05f : 0.03f),
                                       0.0f, (float) content.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRect (content);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube
                 || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            const float darken = (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                                     ? (isDark ? 0.16f : 0.07f)
                                     : (isDark ? 0.10f : 0.03f);
            g.setColour (PinkXP::face.interpolatedWith (juce::Colours::black, darken));
            g.fillRect (content);
        }
        else
        {
            g.setColour (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel ? PinkXP::btnFace : PinkXP::face);
            g.fillRect (content);
        }
    }

    drawMeterGroup(g, areaMeters);
    if (readoutStyle == ReadoutStyle::barReadout) drawDrReadouts (g, areaDr);
    else                                          drawDrBigDigits (g, areaDr);
    drawCrestBand (g, areaCrest);

    // 鼠标悬停标尺（Crest 历史带）
    if (hoverActive)
        drawHoverRuler(g);
}

void DynamicsModule::mouseMove(const juce::MouseEvent& e)
{
    ModulePanel::mouseMove(e);

    const auto pos    = e.getPosition();
    const bool inside = items.hist && areaCrest.contains(pos) && ! getControlDrawerBounds().contains (pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (areaCrest);
    }
}

void DynamicsModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在 Crest 历史带内，擦除只需失效该区域（不走浮动态限流）
        repaint (areaCrest);
    }
}

void DynamicsModule::drawHoverRuler(juce::Graphics& g)
{
    if (!hoverActive)
        return;

    // Crest 历史带内部绘图区（与 drawCrestBand 一致）
    const auto inner = areaCrest.reduced(4);
    const auto plot  = inner.withTrimmedTop(14);
    if (plot.isEmpty() || !plot.contains(hoverPos))
        return;

    // X → 相对时间（最右=当前，负=过去；横轴跨度 = 历史跨度参数）
    const float t = (float)(hoverPos.x - plot.getX())
                  / juce::jmax(1, plot.getWidth());
    const float timeSec = (t - 1.0f) * histSpanSec;

    // Y → crest 值（0 ~ histMaxDb dB）
    const float norm = juce::jlimit(0.0f, 1.0f,
        (float)(plot.getBottom() - hoverPos.y) / juce::jmax(1, plot.getHeight()));
    const float crestDb = norm * histMaxDb;

    const juce::String readout = juce::String(timeSec, 1) + " s  "
                               + juce::String(crestDb, 1) + " dB";
    PinkXP::drawHoverRuler(g, plot, hoverPos, readout);
}

// ----------------------------------------------------------
// 柱组（按启用项取 PK L/R、RMS L/R）
// ----------------------------------------------------------
void DynamicsModule::drawMeterGroup(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    const MeterBar* bars[4];
    int barCount = 0;
    if (items.peak) { bars[barCount++] = &bPeakL; bars[barCount++] = &bPeakR; }
    if (items.rms)  { bars[barCount++] = &bRmsL;  bars[barCount++] = &bRmsR;  }
    if (barCount == 0) return;

    const int gap = 4;
    const int labelH = 14;

    auto barRow = area.withTrimmedBottom(labelH);
    const int barW = juce::jmax(8, (barRow.getWidth() - gap * (barCount - 1)) / barCount);

    for (int i = 0; i < barCount; ++i)
    {
        juce::Rectangle<int> r(barRow.getX() + i * (barW + gap),
                               barRow.getY(), barW, barRow.getHeight());
        drawSingleBar(g, r, *bars[i]);

        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        PinkXP::drawCachedText(g, bars[i]->label,
                               juce::Rectangle<int>(r.getX(), area.getBottom() - labelH, r.getWidth(), labelH),
                               juce::Justification::centred, 9.0f, juce::Font::bold, PinkXP::ink);
    }
}

void DynamicsModule::drawSingleBar(juce::Graphics& g, juce::Rectangle<int> r, const MeterBar& bar) const
{
    const auto& tokens = PinkXP::getSkinTokens();
    PinkXP::Craft::drawPanelWell (g, r, PinkXP::content.darker (0.05f), PinkXP::WellKind::trough);
    auto inner = r.reduced(3);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int cellH   = 4;
    const int cellGap = (tokens.defaultCellGapPx > 1 && inner.getHeight() >= 64) ? tokens.defaultCellGapPx : 1;
    const int step    = cellH + cellGap;
    const int rows    = juce::jmax(1, (inner.getHeight() + cellGap) / step);

    const float norm = dbToNorm(bar.smoothed);
    const int litRows = (int) std::round(norm * rows);

    for (int row = 0; row < rows; ++row)
    {
        // row=0 在屏幕顶部（高电平区）→ 0 dB → 红色；row=rows-1 → barFloorDb → 绿色
        const float rowNorm = (float) row / (float) juce::jmax(1, rows - 1);
        const float rowDb   = juce::jmap(rowNorm, 0.0f, 1.0f, 0.0f, barFloorDb);
        const bool  lit     = (rows - 1 - row) < litRows;

        const int y = inner.getY() + row * step;
        const auto base = dbToColour(rowDb);
        PinkXP::Craft::drawMeterCell (g, juce::Rectangle<int> (inner.getX(), y, inner.getWidth(), cellH), base, lit);
    }

    // 峰值保持线
    if (showPeakHold)
    {
        const float peakNorm = dbToNorm(bar.peakHold);
        const int   peakY = inner.getBottom() - (int) std::round(peakNorm * inner.getHeight());
        if (peakY >= inner.getY() && peakY <= inner.getBottom())
        {
            const auto peakCol = dbToColour(bar.peakHold).brighter(0.2f);
            const juce::Rectangle<int> peakRect (inner.getX(), peakY - 1, inner.getWidth(), 2);
            if (tokens.peakCapGlow && ! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (peakCol.withAlpha (0.28f));
                g.fillRect (peakRect.expanded (1));
            }
            g.setColour (peakCol);
            if (! tokens.hardPixelSteps && tokens.barCellRadius > 0.5f)
            {
                g.fillRoundedRectangle (peakRect.toFloat(), juce::jmin (1.0f, tokens.barCellRadius * 0.5f));
                if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass && ! PinkXP::isGreenScreenBackdrop())
                {
                    g.setColour (juce::Colours::white.withAlpha (0.48f));
                    g.fillRect ((float) peakRect.getX() + 0.5f, (float) peakRect.getY(),
                                juce::jmax (1.0f, (float) peakRect.getWidth() - 1.0f), 1.0f);
                }
                else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                {
                    g.setColour (peakCol.interpolatedWith (juce::Colours::white, 0.58f));
                    g.fillRect (peakRect.getX() + 1, peakRect.getY(), juce::jmax (1, peakRect.getWidth() - 2), 1);
                }
            }
            else
            {
                g.fillRect (peakRect);
                if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                    || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                {
                    g.setColour (peakCol.interpolatedWith (juce::Colours::white, 0.55f));
                    g.fillRect (peakRect.getX(), peakRect.getY(), peakRect.getWidth(), 1);
                }
            }
        }
    }

    PinkXP::Craft::drawCanvasFinishOverlay (g, r.reduced (2));
}

// ----------------------------------------------------------
// DR 面板·大数字样式（旧 Dynamics）：启用行等分高度
// ----------------------------------------------------------
void DynamicsModule::drawDrBigDigits(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty() || items.drCount() == 0) return;

    PinkXP::Craft::drawReadoutCard (g, area, PinkXP::content);

    auto inner = area.reduced(8);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int rowH = inner.getHeight() / items.drCount();

    auto drawRow = [&](int slot, int rowIdx, const juce::String& title, float value,
                       const juce::String& unit, float big)
    {
        juce::Rectangle<int> row(inner.getX(), inner.getY() + slot * rowH,
                                 inner.getWidth(), rowH);

        // ① 标题：窄面板下按行宽压缩，避免被右侧区段盖掉
        const int titleH = juce::jlimit(8, 12, row.getHeight() / 4);
        g.setColour(PinkXP::ink.withAlpha(0.85f));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawFittedText(y2k::tr (title), row.removeFromTop(titleH),
                         juce::Justification::topLeft, 1, 0.8f);

        // ② 单位 "dB" 在数值下方；面板被拖得很矮时不再单独留单位行
        const int unitH = (row.getHeight() >= 26) ? juce::jlimit(8, 12, row.getHeight() / 4) : 0;
        auto unitRect = (unitH > 0) ? row.removeFromBottom(unitH) : juce::Rectangle<int>();

        // ③ 数值：整行宽度都可用来画大数字，字号按可用宽度收缩（样本 "88.8" 保证稳定）
        const juce::String numText = juce::String(value, 1);

        float px = big;
        {
            const juce::Font probeFont = PinkXP::getAxisFont(px, juce::Font::bold);
            const float needW = juce::jmax(probeFont.getStringWidthFloat(y2k::tr ("88.8")),
                                           probeFont.getStringWidthFloat(y2k::tr (numText)));
            if (needW > 1.0f && needW > (float) row.getWidth())
                px *= (float) row.getWidth() / needW;
        }
        px = juce::jmin(px, (float) juce::jmax(6, row.getHeight() - 2));
        {
            const auto hProbe = PinkXP::getAxisFont (px, juce::Font::bold);
            const float maxH  = (float) juce::jmax (6, row.getHeight());
            if (hProbe.getHeight() > maxH && hProbe.getHeight() > 0.0f)
                px *= maxH / hProbe.getHeight();
        }
        px = juce::jmax(6.0f, px);

        const juce::String cacheKey = numText + "|" + juce::String(px, 1);
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (cacheKey != drValueText_[rowIdx]
            || nowMs - drValueTime_[rowIdx] >= 100.0
            || ! drValueCache_[rowIdx].isValid())
        {
            drValueText_[rowIdx] = cacheKey;
            drValueTime_[rowIdx] = nowMs;
            drValueCache_[rowIdx] = PinkXP::cachedStaticText (
                g, numText, px, juce::Font::bold, PinkXP::pink600,
                juce::Colours::transparentBlack, true);
        }

        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (row);

            const auto& numImg = drValueCache_[rowIdx];
            if (numImg.isValid())
            {
                numImg.drawAt (g,
                               (float) row.getX(),
                               (float) (row.getY() + juce::jmax(0, (row.getHeight() - numImg.getHeight()) / 2)));
            }
            else
            {
                g.setColour(PinkXP::pink600);
                g.setFont(PinkXP::getAxisFont(px, juce::Font::bold));
                g.drawText(y2k::tr (numText), row, juce::Justification::centredLeft, false);
            }
        }

        // ④ 单位（位图缓存，紧贴数值下方，左对齐）
        if (unitRect.isEmpty())
            return;

        const auto unitImg = PinkXP::cachedStaticText (
            g, unit, 9.0f, juce::Font::plain, PinkXP::ink.withAlpha(0.75f),
            juce::Colours::transparentBlack, true);
        if (unitImg.isValid())
            unitImg.drawAt (g, (float) unitRect.getX(), (float) unitRect.getY());
        else
        {
            g.setColour(PinkXP::ink.withAlpha(0.75f));
            g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
            g.drawText(y2k::tr (unit), unitRect, juce::Justification::topLeft, false);
        }
    };

    const auto& cardTokens = PinkXP::getSkinTokens();
    const bool modernDividers = (cardTokens.frameStyle == PinkXP::FrameStyle::modernStudio
                                 || cardTokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool chromeDividers = (cardTokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                                 && ! cardTokens.isChromaKey);
    const bool neonDividers   = (cardTokens.frameStyle == PinkXP::FrameStyle::neonTube
                                 && ! cardTokens.isChromaKey);
    const bool crtDividers    = (cardTokens.frameStyle == PinkXP::FrameStyle::crtPhosphor
                                 && ! cardTokens.isChromaKey);
    const bool isDarkCard     = PinkXP::content.getPerceivedBrightness() < 0.45f;

    static const char* titles[numDrRows] = { "DR SHORT", "DR INTEG", "CREST" };
    int slot = 0;
    for (int r = 0; r < numDrRows; ++r)
    {
        if (! drRowEnabled (r))
            continue;
        if (slot > 0)
        {
            if (chromeDividers)
            {
                const int divY = inner.getY() + slot * rowH - 2;
                g.setColour (PinkXP::dark.withAlpha (isDarkCard ? 0.52f : 0.30f));
                g.fillRect (inner.getX(), divY, inner.getWidth(), 1);
                g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f)
                                       .withAlpha (isDarkCard ? 0.22f : 0.62f));
                g.fillRect (inner.getX(), divY + 1, inner.getWidth(), 1);
            }
            else if (neonDividers)
            {
                const int divY = inner.getY() + slot * rowH - 1;
                if (! PinkXP::isGreenScreenBackdrop())
                {
                    g.setColour (PinkXP::sel.withAlpha (0.18f));
                    g.fillRect (inner.getX(), divY - 1, inner.getWidth(), 3);
                }
                g.setColour (PinkXP::sel.withAlpha (0.58f));
                g.fillRect (inner.getX(), divY, inner.getWidth(), 1);
            }
            else if (crtDividers)
            {
                const int divY = inner.getY() + slot * rowH - 1;
                g.setColour (PinkXP::sel.withAlpha (0.44f));
                g.fillRect (inner.getX(), divY, inner.getWidth(), 1);
                g.fillRect (inner.getCentreX(), divY - 1, 1, 3);
            }
            else if (modernDividers)
            {
                const float divY = (float) (inner.getY() + slot * rowH) - 1.0f;
                PinkXP::Craft::drawGridLine (g, (float) inner.getX(), divY, (float) inner.getRight(), divY,
                                             false, PinkXP::ink);
            }
        }
        drawRow (slot++, r, titles[r], drRowValue (r), "dB", 28.0f);
    }

    PinkXP::Craft::drawCanvasFinishOverlay (g, area.reduced (2));
}

// ----------------------------------------------------------
// DR 面板·带进度条读数样式（旧 Dynamics DR）：启用行垂直堆叠
// ----------------------------------------------------------
void DynamicsModule::drawDrReadouts(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty() || items.drCount() == 0) return;

    auto a = area;
    const int n = items.drCount();
    const int h = juce::jmax(44, a.getHeight() / n - 3);
    auto drToNorm = [](float d) { return juce::jlimit(0.0f, 1.0f, d / 24.0f); };

    static const char* titles[numDrRows] = { "DR SHORT", "DR INTEG", "CREST" };
    int slot = 0;
    for (int r = 0; r < numDrRows; ++r)
    {
        if (! drRowEnabled (r)) continue;
        const bool last = (slot == n - 1);
        auto cell = last ? a : a.removeFromTop (h);
        y2k::meter::drawReadout (g, cell, titles[r], drRowValue (r), "dB", drToNorm (drRowValue (r)),
                                 flash[r], pulsePhase + 2.0f * (float) slot);
        if (! last) a.removeFromTop (3);
        ++slot;
    }
}

// ----------------------------------------------------------
// Crest 历史带：取最近 span 秒，过密时按像素列取最大值
// ----------------------------------------------------------
void DynamicsModule::drawCrestBand(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty() || ! items.hist) return;

    const auto& tokens = PinkXP::getSkinTokens();
    const auto wellKind = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                           || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                              ? PinkXP::WellKind::card
                              : PinkXP::WellKind::canvas;
    PinkXP::Craft::drawPanelWell (g, area, PinkXP::content, wellKind);
    auto inner = area.reduced(6, 5);
    if (inner.getWidth() <= 4 || inner.getHeight() <= 4) return;

    if (! tokens.isChromaKey)
    {
        const float titleAlpha = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio) ? 0.72f : 0.90f;
        g.setColour(PinkXP::ink.withAlpha(titleAlpha));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        PinkXP::drawCachedText(g, "CREST HISTORY", inner.withHeight(12), juce::Justification::topLeft,
                               9.0f, juce::Font::bold, PinkXP::ink.withAlpha(titleAlpha));
    }

    auto plot = inner.withTrimmedTop(14);
    if (plot.isEmpty()) return;

    // 刻度线：每 10 dB 一条直到 histMaxDb（Pixel XP 像素点阵 / Modern 细线 / Chrome 刻蚀细线 / Neon 虚线 / CRT 准星 / 绿幕跳过）
    const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const int maxTick = (int) std::round(histMaxDb);
    for (int db = 0; db <= maxTick; db += 10)
    {
        const float t = 1.0f - (float) db / histMaxDb;
        const float y = (float) plot.getY() + std::round(t * (float) plot.getHeight());
        const bool major = (db == 0 || db == 20 || db == 40);
        PinkXP::Craft::drawGridLine (g, (float) plot.getX(), y, (float) plot.getRight(), y,
                                     major, (isNeon || isCrt) ? PinkXP::sel : PinkXP::pink500);
        if ((isChrome || isCrt) && major && ! tokens.isChromaKey)
        {
            g.setColour (isCrt ? PinkXP::sel.withAlpha (0.65f) : PinkXP::ink.withAlpha (0.45f));
            g.fillRect (plot.getX(), (int) y, 3, 1);
            if (isCrt)
                g.fillRect (plot.getRight() - 3, (int) y, 3, 1);
        }
    }

    const int N = juce::jlimit (8, crestHistLen, juce::roundToInt (histSpanSec * (float) kHistRate));
    const int drawN = juce::jmin (N, juce::jmax (2, plot.getWidth()));
    const float w = (float) plot.getWidth();
    const float h = (float) plot.getHeight();

    // ---- 列的相位锁定（与示波器抽点是同一类问题）----
    const float samplesPerCol = (float) N / (float) drawN;
    const juce::int64 winStartAbs = (juce::int64) crestHistTotal - (juce::int64) N;
    const double colPos = (double) winStartAbs / (double) juce::jmax (1.0e-6f, samplesPerCol);
    const juce::int64 firstBin = (juce::int64) std::floor (colPos);
    const float frac = (float) (colPos - (double) firstBin);
    const float pxPerCol = w / (float) drawN;

    auto ringAt = [this] (juce::int64 absIdx) noexcept
    {
        if (absIdx < 0) return 0.0f;
        return crestHist[(size_t) (absIdx % (juce::int64) crestHistLen)];
    };

    // 全风格统一平滑抗锯齿曲线（Pixel XP 保持 0px Win95 卡片与像素网格，曲线自身不强行锯齿化）
    juce::Path path;
    bool started = false;
    for (int j = 0; j <= drawN; ++j)
    {
        const juce::int64 bin = firstBin + (juce::int64) j;
        const juce::int64 s0 = (juce::int64) std::llround ((double) bin * (double) samplesPerCol);
        const juce::int64 s1 = juce::jmax (s0 + 1, (juce::int64) std::llround ((double) (bin + 1) * (double) samplesPerCol));

        const juce::int64 a0 = juce::jmax (s0, winStartAbs);
        const juce::int64 a1 = juce::jmin (s1, (juce::int64) crestHistTotal);
        if (a1 <= a0) continue;

        float v = 0.0f;
        for (juce::int64 a = a0; a < a1; ++a)
            v = juce::jmax (v, ringAt (a));

        const float x = (float) plot.getX() + ((float) j - frac) * pxPerCol;
        const float norm = juce::jlimit(0.0f, 1.0f, v / histMaxDb);
        const float y = (float) plot.getBottom() - norm * h;

        if (! started) { path.startNewSubPath(x, y); started = true; }
        else           path.lineTo(x, y);
    }

    if (started)
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (plot);

        if (histStyle == 0)
        {
            juce::Path fill = path;
            fill.lineTo((float) plot.getRight(),  (float) plot.getBottom());
            fill.lineTo((float) plot.getX(),      (float) plot.getBottom());
            fill.closeSubPath();

            if (! PinkXP::isGreenScreenBackdrop()
                && (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                    || isGlass || isChrome || isNeon || isCrt))
            {
                const auto topCol = isNeon ? PinkXP::sel : (isCrt ? PinkXP::pink500 : PinkXP::pink400);
                const float topA  = isNeon   ? 0.34f
                                  : isGlass  ? 0.32f
                                  : isChrome ? 0.32f
                                  : isCrt    ? 0.24f
                                             : 0.28f;
                juce::ColourGradient grad (topCol.withAlpha (topA * tokens.areaFillAlphaScale),
                                           0.0f, (float) plot.getY(),
                                           PinkXP::pink300.withAlpha (0.02f * tokens.areaFillAlphaScale),
                                           0.0f, (float) plot.getBottom(), false);
                if (isChrome || isGlass)
                    grad.addColour (0.45, PinkXP::pink300.withAlpha (0.15f * tokens.areaFillAlphaScale));
                else if (isNeon)
                    grad.addColour (0.45, PinkXP::pink400.withAlpha (0.12f * tokens.areaFillAlphaScale));
                g.setGradientFill (grad);
            }
            else
            {
                g.setColour(PinkXP::pink300.withAlpha(0.28f * tokens.areaFillAlphaScale));
            }
            g.fillPath(fill);
        }

        PinkXP::Craft::drawStyledStroke (g, path, PinkXP::pink600, (isChrome || isGlass) ? 1.6f : 1.5f);
        if (isChrome || (isGlass && ! PinkXP::isGreenScreenBackdrop()))
        {
            const auto glintCol = PinkXP::pink600.interpolatedWith (juce::Colours::white, isGlass ? 0.68f : 0.62f)
                                                 .withAlpha (PinkXP::isGreenScreenBackdrop() ? 0.60f : (isGlass ? 0.42f : 0.44f));
            g.setColour (glintCol);
            g.strokePath (path, juce::PathStrokeType (0.85f,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded),
                          juce::AffineTransform::translation (0.0f, -0.75f));
        }
    }

    PinkXP::Craft::drawCanvasFinishOverlay (g, area.reduced (2));
}
