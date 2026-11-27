#include "source/ui/modules/PhaseModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"

namespace
{
    // Width / Balance 条左侧的文字列（GL 快照 / overlay / 旧绘制路径共用）：
    //   标签在整列内水平居中，列右缘再留 barLabelGap 给凹陷条形的起点。
    //   列宽取「最宽标签 "WIDTH" 在 9px bold 下实测约 36px（Silkscreen 35.6 /
    //   ChillBitmap 36.3）+ 两侧各 6px 呼吸」= 48 —— 旧值 60 时左右各空 12px，
    //   文字看着离两侧都太远；缩小后条形顺带加宽 12px。
    constexpr int barLabelW     = 44;   // 纯文字列（不含右侧间隙）
    constexpr int barLabelGap   = 4;
    // 条形本体的左边界 = 标签的居中范围（文字列 + 间隙一起参与居中）
    constexpr int barLeftInset  = barLabelW + barLabelGap;
    // 右侧独立读数列（供 "100%" / "L100" / "R100" 显示，避免与 LED 格及 Frosted/Neon 边框重叠）
    constexpr int barReadoutW   = 36;
    constexpr int barReadoutGap = 4;
    constexpr int barRightInset = barReadoutW + barReadoutGap;
    // Correlation Dial 顶部给 "CORR" + 数值读数（y+14..28）+ "0" 刻度标签预留的安全净空
    constexpr int kDialReadoutReserve = 48;

    struct BarRowLayout
    {
        juce::Rectangle<int> labelRect;
        juce::Rectangle<int> barRect;
        juce::Rectangle<int> valueRect;
    };

    inline BarRowLayout layoutBarRow (juce::Rectangle<int> row) noexcept
    {
        BarRowLayout out;
        out.labelRect = row.withWidth (juce::jmin (barLeftInset, row.getWidth()));
        const int rightReserve = (row.getWidth() >= barLeftInset + barRightInset + 24)
                                     ? barRightInset
                                     : 0;
        out.barRect = row.withTrimmedLeft (barLeftInset).withTrimmedRight (rightReserve);
        if (rightReserve > 0)
            out.valueRect = row.withTrimmedLeft (row.getWidth() - barReadoutW).withTrimmedRight (2);
        else
            out.valueRect = out.barRect.withTrimmedRight (4);
        return out;
    }

    inline juce::Rectangle<int> clampLabelRect (juce::Rectangle<int> r,
                                                juce::Rectangle<int> bounds) noexcept
    {
        const int x = juce::jlimit (bounds.getX(), juce::jmax (bounds.getX(), bounds.getRight() - r.getWidth()), r.getX());
        const int y = juce::jlimit (bounds.getY(), juce::jmax (bounds.getY(), bounds.getBottom() - r.getHeight()), r.getY());
        return r.withPosition (x, y);
    }

    inline juce::Colour phaseBackdropColour() noexcept
    {
        const auto& tokens = PinkXP::getSkinTokens();
        if (tokens.isChromaKey)
            return PinkXP::chromaKeyBackdropColour();
        return (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel)
                   ? PinkXP::btnFace
                   : PinkXP::face;
    }

    // GL 侧渐细指针（现代 / 金属 / 玻璃 / 霓虹 / 像素表盘共用平滑反走样指针）
    void appendTaperedNeedleGl (std::vector<float>& v,
                                float x0, float y0, float x1, float y1,
                                float baseHalfW, float tipHalfW, juce::Colour c)
    {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt (dx * dx + dy * dy);
        if (len < 1.0e-5f || c.getFloatAlpha() <= 0.001f)
            return;

        const float nx = -dy / len, ny = dx / len;
        const float r = c.getFloatRed(), g = c.getFloatGreen(),
                    b = c.getFloatBlue(), a = c.getFloatAlpha();
        const float q[36] = {
            x0 + nx * baseHalfW, y0 + ny * baseHalfW, r, g, b, a,
            x1 + nx * tipHalfW,  y1 + ny * tipHalfW,  r, g, b, a,
            x0 - nx * baseHalfW, y0 - ny * baseHalfW, r, g, b, a,
            x1 + nx * tipHalfW,  y1 + ny * tipHalfW,  r, g, b, a,
            x1 - nx * tipHalfW,  y1 - ny * tipHalfW,  r, g, b, a,
            x0 - nx * baseHalfW, y0 - ny * baseHalfW, r, g, b, a,
        };
        v.insert (v.end(), q, q + 36);
    }

    // CPU 侧渐细指针路径
    void drawTaperedNeedleCpu (juce::Graphics& g,
                               float x0, float y0, float x1, float y1,
                               float baseHalfW, float tipHalfW, juce::Colour c)
    {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt (dx * dx + dy * dy);
        if (len < 1.0e-5f || c.isTransparent())
            return;

        const float nx = -dy / len, ny = dx / len;
        juce::Path p;
        p.startNewSubPath (x0 + nx * baseHalfW, y0 + ny * baseHalfW);
        p.lineTo          (x1 + nx * tipHalfW,  y1 + ny * tipHalfW);
        p.lineTo          (x1 - nx * tipHalfW,  y1 - ny * tipHalfW);
        p.lineTo          (x0 - nx * baseHalfW, y0 - ny * baseHalfW);
        p.closeSubPath();
        g.setColour (c);
        g.fillPath (p);
    }
}

// ==========================================================
// PhaseModule
// ==========================================================

PhaseModule::PhaseModule(AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::phase),
      hub(h)
{
    // 订阅按项开关：相位图需要 Oscilloscope（点云），相关/宽度/平衡需要 Phase
    updateSubscriptions();
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(320, 192); // phase 5×3 大格
    setControlPanelEnabled (true);

    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        invalidateStaticLayer();
        buildGlSnapshot();   // GL 快照含底色/几何颜色，主题切换必须重建
        repaint();
    });
}

PhaseModule::~PhaseModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = -1;
    }

    // Phase F：先解绑 FrameListener
    hub.removeFrameListener(this);
    if (phaseRetained) hub.release(AnalyserHub::Kind::Phase);
    if (oscRetained)   hub.release(AnalyserHub::Kind::Oscilloscope);
}

// 按当前启用项增减 Hub 订阅（引用计数为 0 的分析路径 pushStereo 会直接跳过）
void PhaseModule::updateSubscriptions()
{
    const bool wantOsc   = items.gonio;
    const bool wantPhase = items.corr || items.width || items.bal;

    if (wantOsc != oscRetained)
    {
        if (wantOsc) hub.retain(AnalyserHub::Kind::Oscilloscope);
        else         hub.release(AnalyserHub::Kind::Oscilloscope);
        oscRetained = wantOsc;
    }
    if (wantPhase != phaseRetained)
    {
        if (wantPhase) hub.retain(AnalyserHub::Kind::Phase);
        else           hub.release(AnalyserHub::Kind::Phase);
        phaseRetained = wantPhase;
    }
}

// ----------------------------------------------------------
// 持久化 / 控制面板
// ----------------------------------------------------------
juce::ValueTree PhaseModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("itemGonio", items.gonio, nullptr);
    s.setProperty ("itemCorr",  items.corr,  nullptr);
    s.setProperty ("itemWidth", items.width, nullptr);
    s.setProperty ("itemBal",   items.bal,   nullptr);
    s.setProperty ("gonioStyle", (int) gonioStyle, nullptr);
    s.setProperty ("barStyle",   (int) barStyle,   nullptr);
    s.setProperty ("gonioGain",  (double) gonioGain, nullptr);
    s.setProperty ("gonioSize",  (double) gonioSize, nullptr);
    s.setProperty ("smoothing",  (double) smoothingScale, nullptr);
    s.setProperty ("showGrid",   showGonioGrid, nullptr);
    return s;
}

void PhaseModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    auto getB = [&] (const char* k, bool& dst)
    {
        if (state.hasProperty (k)) dst = (bool) state.getProperty (k);
    };

    Items it = items;
    getB ("itemGonio", it.gonio); getB ("itemCorr", it.corr);
    getB ("itemWidth", it.width); getB ("itemBal", it.bal);
    if (! it.any()) it = Items();
    items = it;

    if (state.hasProperty ("gonioStyle"))
        gonioStyle = (int) state.getProperty ("gonioStyle") == 1 ? GonioStyle::trace : GonioStyle::dots;
    if (state.hasProperty ("barStyle"))
        barStyle = (int) state.getProperty ("barStyle") == 1 ? BarStyle::solid : BarStyle::segmented;
    if (state.hasProperty ("gonioGain"))
        gonioGain = juce::jlimit (0.5f, 3.0f, (float) (double) state.getProperty ("gonioGain"));
    if (state.hasProperty ("gonioSize"))
        gonioSize = juce::jlimit (1.0f, 4.0f, (float) (double) state.getProperty ("gonioSize"));
    if (state.hasProperty ("smoothing"))
        smoothingScale = juce::jlimit (0.2f, 3.0f, (float) (double) state.getProperty ("smoothing"));
    if (state.hasProperty ("showGrid"))
        showGonioGrid = (bool) state.getProperty ("showGrid");

    updateSubscriptions();
    invalidateStaticLayer();
    layoutContent (getContentBounds());
    buildGlSnapshot();
    refreshControlPanel();
    repaint();
}

void PhaseModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::toggle ("itemGonio", "Goniometer",
        "Left against right as a mid/side vector scope. A tall thin shape is mono, a wide cloud is stereo.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemCorr", "Correlation dial",
        "+1 = mono-safe, 0 = very wide, -1 = out of phase. A needle below 0 means parts cancel in mono.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemWidth", "Width bar",
        "How much of the signal lives in the side channel, 0 - 100%.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemBal", "Balance bar",
        "Left / right level balance. C = centred.").in ("Show"));

    out.push_back (ParamDef::choice ("gonioStyle", "Goniometer style",
        { "Dots", "Trace" },
        "Dots: each sample is a pixel. Trace: neighbouring samples are joined by a thin line.")
        .in ("Style").when ([this] { return items.gonio; }));
    out.push_back (ParamDef::slider ("gonioGain", "Goniometer zoom", 0.5, 3.0, 0.1, " x",
        "Scale the M/S vector plot so quieter mixes fill the diamond clearly.")
        .in ("Style").when ([this] { return items.gonio; }));
    out.push_back (ParamDef::slider ("gonioSize", "Dot / line size", 1.0, 4.0, 0.5, " px",
        "Pixel size for goniometer dots or stroke thickness for trace lines.")
        .in ("Style").when ([this] { return items.gonio; }));
    out.push_back (ParamDef::toggle ("showGrid", "Goniometer grid",
        "Show the diamond frame, M/S crosshairs, L/R diagonals, and axis labels.")
        .in ("Style").when ([this] { return items.gonio; }));
    out.push_back (ParamDef::choice ("barStyle", "Bar style",
        { "Segmented", "Solid" },
        "Segmented: pixel cells. Solid: one continuous filled bar.")
        .in ("Style").when ([this] { return items.width || items.bal; }));
    out.push_back (ParamDef::slider ("smoothing", "Response speed", 0.2, 3.0, 0.1, " x",
        "How quickly the correlation needle, width bar, and balance bar react.")
        .in ("Style"));
}

double PhaseModule::getParamValue (const juce::String& id) const
{
    if (id == "itemGonio")   return items.gonio ? 1.0 : 0.0;
    if (id == "itemCorr")    return items.corr ? 1.0 : 0.0;
    if (id == "itemWidth")   return items.width ? 1.0 : 0.0;
    if (id == "itemBal")     return items.bal ? 1.0 : 0.0;
    if (id == "gonioStyle")  return (double) (int) gonioStyle;
    if (id == "gonioGain")   return (double) gonioGain;
    if (id == "gonioSize")   return (double) gonioSize;
    if (id == "showGrid")    return showGonioGrid ? 1.0 : 0.0;
    if (id == "barStyle")    return (double) (int) barStyle;
    if (id == "smoothing")   return (double) smoothingScale;
    return 0.0;
}

void PhaseModule::setParamValue (const juce::String& id, double v)
{
    if (id == "gonioStyle")
    {
        gonioStyle = juce::roundToInt (v) == 1 ? GonioStyle::trace : GonioStyle::dots;
        buildGlSnapshot(); repaint();
        return;
    }
    if (id == "gonioGain")
    {
        gonioGain = juce::jlimit (0.5f, 3.0f, (float) v);
        buildGlSnapshot(); repaint();
        return;
    }
    if (id == "gonioSize")
    {
        gonioSize = juce::jlimit (1.0f, 4.0f, (float) v);
        buildGlSnapshot(); repaint();
        return;
    }
    if (id == "showGrid")
    {
        showGonioGrid = (v >= 0.5);
        invalidateStaticLayer(); buildGlSnapshot(); repaint();
        return;
    }
    if (id == "smoothing")
    {
        smoothingScale = juce::jlimit (0.2f, 3.0f, (float) v);
        return;
    }
    if (id == "barStyle")
    {
        barStyle = juce::roundToInt (v) == 1 ? BarStyle::solid : BarStyle::segmented;
        buildGlSnapshot(); repaint();
        return;
    }

    const bool on = v >= 0.5;
    Items it = items;
    if      (id == "itemGonio") it.gonio = on;
    else if (id == "itemCorr")  it.corr = on;
    else if (id == "itemWidth") it.width = on;
    else if (id == "itemBal")   it.bal = on;
    else return;

    if (! it.any())
        return;   // 至少保留一项

    items = it;
    updateSubscriptions();
    invalidateStaticLayer();
    layoutContent (getContentBounds());
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// 区域划分（layoutContent / GL 快照 / overlay / 静态层共用同一份几何）
//   四项全开：与旧 Phase 逐像素相同。
// ----------------------------------------------------------
PhaseModule::Areas PhaseModule::computeAreas (juce::Rectangle<int> content) const
{
    Areas a;

    const int barH    = 20;
    const int barGap  = 2;
    const bool hasUpper = items.gonio || items.corr;
    const int  barRows  = (items.width ? 1 : 0) + (items.bal ? 1 : 0);

    juce::Rectangle<int> upper, lower;
    if (hasUpper && barRows > 0)
    {
        const int footer = barH * barRows + barGap * (barRows - 1);
        upper = content.withTrimmedBottom(footer + 6);
        lower = content.withTop(upper.getBottom() + 6);
    }
    else if (hasUpper)
    {
        upper = content;
    }
    else
    {
        // 只有条形：每条拉高一些并整体垂直居中，避免两条细线贴在顶端
        const int rowH = juce::jlimit (barH, 44, (content.getHeight() - barGap * (barRows - 1)) / juce::jmax (1, barRows));
        const int totalH = rowH * barRows + barGap * (barRows - 1);
        lower = content.withTrimmedTop (juce::jmax (0, (content.getHeight() - totalH) / 2)).withHeight (totalH);
    }

    if (items.gonio && items.corr)
    {
        const int gonioSize = juce::jmin(upper.getWidth() * 5 / 9, upper.getHeight());
        a.gonio = upper.withWidth(gonioSize);
        a.dial  = upper.withTrimmedLeft(gonioSize + 8);
    }
    else if (items.gonio) a.gonio = upper;
    else if (items.corr)  a.dial  = upper;

    if (barRows > 0)
    {
        const int rowH = hasUpper ? barH
                                  : juce::jlimit (barH, 44, (content.getHeight() - barGap * (barRows - 1)) / barRows);
        int y = lower.getY();
        if (items.width)   { a.width   = lower.withY (y).withHeight (rowH); y += rowH + barGap; }
        if (items.bal)     { a.balance = lower.withY (y).withHeight (rowH); }
    }

    return a;
}

// ----------------------------------------------------------
// onFrame
// ----------------------------------------------------------
void PhaseModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;

    // Oscilloscope 点云数据（从聚合帧内的 oscL/oscR 直接 memcpy 入 juce::Array）
    if (items.gonio && frame.has (AnalyserHub::Kind::Oscilloscope))
    {
        const int n = (int) frame.oscL.size();
        oscL.resize(n);
        oscR.resize(n);
        std::memcpy (oscL.getRawDataPointer(), frame.oscL.data(), (size_t) n * sizeof (float));
        std::memcpy (oscR.getRawDataPointer(), frame.oscR.data(), (size_t) n * sizeof (float));
    }

    if (frame.has (AnalyserHub::Kind::Phase))
    {
        const auto& snap = frame.phase;
        const float a = y2k::pacing::alphaForDt (0.25f, frame.dtSec * smoothingScale);
        auto smooth = [a](float cur, float target)
        {
            if (! std::isfinite(target)) target = 0.0f;
            return cur + a * (target - cur);
        };
        smoothedCorr    = smooth(smoothedCorr,    snap.correlation);
        smoothedWidth   = smooth(smoothedWidth,   snap.width);
        smoothedBalance = smooth(smoothedBalance, snap.balance);
    }

    // v2.9：去掉 16ms×DPI 硬编码节流（会把本模块锁在 ≤60fps），每个分发帧重绘
    // v2.9.4：同步生成 GL 直渲染快照
    buildGlSnapshot();
    repaintFrameContent();
}


// ----------------------------------------------------------
// Toybox（v3.0.0）：Gonio 声呐 + 幽灵指针（§8.3）
// ----------------------------------------------------------
bool PhaseModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    if (areaGonio.contains (panelPos))
    {
        out.kind = 2;                 // M/S 菱形荧光轨道电子
        out.rect = areaGonio.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }
    if (areaDial.contains (panelPos))
    {
        out.kind = 3;                 // 指针弹拨虚影
        out.rect = areaDial.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }
    if (areaWidth.contains (panelPos))
    {
        out.kind = 4;                 // WIDTH 条形荧光弹珠
        out.rect = areaWidth.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }
    if (areaBalance.contains (panelPos))
    {
        out.kind = 5;                 // BAL 条形荧光弹珠
        out.rect = areaBalance.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }
    return false;
}

void PhaseModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    if (hit.kind == 2)
    {
        ctx.addModuleActorCapped (makePhaseGonioOrbiterActor (hit.pos, areaGonio), 6);
    }
    else if (hit.kind == 3)
    {
        // 从点击角度弹拨虚影指针，欠阻尼弹簧弹回真实 smoothedCorr 角度
        const auto inner = hit.rect.toNearestInt().reduced (6);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getBottom();
        const float R  = juce::jmin ((float) inner.getWidth() * 0.46f,
                                     (float) inner.getHeight() - (float) kDialReadoutReserve);
        const float restTh = (juce::jlimit (-1.0f, 1.0f, smoothedCorr) * 0.5f + 0.5f)
                             * juce::MathConstants<float>::pi;
        const float clickTh = juce::jlimit (0.04f, juce::MathConstants<float>::pi - 0.04f,
                                            std::atan2 (juce::jmax (1.0f, cy - hit.pos.y), cx - hit.pos.x));
        ctx.addModuleActorCapped (makeGhostNeedleActor ({ cx, cy }, juce::jmax (6.0f, R), restTh, clickTh), 6);
    }
    else if (hit.kind == 4 || hit.kind == 5)
    {
        const auto barArea = layoutBarRow (hit.kind == 4 ? areaWidth : areaBalance).barRect.reduced (2);
        if (barArea.getWidth() >= 10 && barArea.getHeight() >= 4)
            ctx.addModuleActorCapped (makeTrackOrbActor (barArea.toFloat(), hit.pos, hit.kind == 5), 6);
    }
}

// ----------------------------------------------------------
// layoutContent
// ----------------------------------------------------------
void PhaseModule::layoutContent(juce::Rectangle<int> content)
{
    const auto a = computeAreas (content);
    areaGonio   = a.gonio;
    areaDial    = a.dial;
    areaWidth   = a.width;
    areaBalance = a.balance;

    // v2.9.5：尺寸守卫（防 layoutContent 每帧触发导致的交互卡顿）
    if (glCanvasSizeChanged (getGlCanvasBounds (content)))
        buildGlSnapshot();
}

// ----------------------------------------------------------
// GlDirectModuleBase 钩子（v2.9.4 迁移）
// ----------------------------------------------------------
juce::Rectangle<int> PhaseModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return contentBounds;
}

void PhaseModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 接管时只画文字（几何全部由 GL 快照绘制）：轴标签、CORR 读数与刻度、
    //   WIDTH/BAL 标签与数值。绿幕下跳过静态刻度/标题，只留实时数值读数。
    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaKey = tokens.isChromaKey;
    const auto labelInk = PinkXP::readableInkOn (phaseBackdropColour());

    const auto areas = computeAreas (canvas);
    const auto areaGonio = areas.gonio;
    const auto areaDial = areas.dial;
    const auto areaWidth = areas.width;
    const auto areaBalance = areas.balance;

    // ---- Gonio 轴标签（绿幕不画，夹紧在凹槽内框里防贴边裁切）----
    if (! chromaKey && ! areaGonio.isEmpty() && showGonioGrid)
    {
        auto inner = areaGonio.reduced (6);
        const auto safe = areaGonio.reduced (3);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getCentreY();
        const float R  = (float) juce::jmin (inner.getWidth(), inner.getHeight()) * 0.48f;
        const auto axisStrong = PinkXP::ink.withAlpha (0.85f);
        const auto axisWeak   = PinkXP::ink.withAlpha (0.55f);
        PinkXP::drawCachedText (g, "M",
                                clampLabelRect ({ juce::roundToInt (cx) - 10, juce::roundToInt (cy - R) - 12, 20, 12 }, safe),
                                juce::Justification::centred, 9.0f, juce::Font::bold, axisStrong, true);
        PinkXP::drawCachedText (g, "S",
                                clampLabelRect ({ juce::roundToInt (cx + R) + 2, juce::roundToInt (cy) - 6, 12, 12 }, safe),
                                juce::Justification::centred, 9.0f, juce::Font::bold, axisStrong, true);
        PinkXP::drawCachedText (g, "L",
                                clampLabelRect ({ juce::roundToInt (cx - R) - 12, juce::roundToInt (cy - R) - 12, 12, 12 }, safe),
                                juce::Justification::centred, 9.0f, juce::Font::bold, axisWeak, true);
        PinkXP::drawCachedText (g, "R",
                                clampLabelRect ({ juce::roundToInt (cx + R), juce::roundToInt (cy - R) - 12, 12, 12 }, safe),
                                juce::Justification::centred, 9.0f, juce::Font::bold, axisWeak, true);
    }

    // ---- Dial 刻度 + CORR 读数 ----
    if (! areaDial.isEmpty())
    {
        auto inner = areaDial.reduced (6);
        const auto dialSafe = areaDial.reduced (3);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getBottom();
        const float R = juce::jmin ((float) inner.getWidth() * 0.46f,
                                    (float) inner.getHeight() - (float) kDialReadoutReserve);
        if (R >= 10.0f)
        {
            if (! chromaKey)
            {
                auto thetaFor = [] (float corr) -> float
                {
                    const float t = juce::jlimit (-1.0f, 1.0f, corr) * 0.5f + 0.5f;
                    return t * juce::MathConstants<float>::pi;
                };
                const struct { float v; const char* s; } marks[] = {
                    { -1.0f, "-1" }, { -0.5f, "-.5" }, { 0.0f, "0" },
                    {  0.5f, "+.5"}, {  1.0f, "+1" }
                };
                for (auto& m : marks)
                {
                    const float th = thetaFor (m.v);
                    const float px = cx - std::cos (th) * (R + 10.0f);
                    const float py = cy - std::sin (th) * (R + 10.0f);
                    const int lx = tokens.pixelSnapped ? (juce::roundToInt (px) - 12) : ((int) px - 12);
                    const int ly = tokens.pixelSnapped ? (juce::roundToInt (py) - 6)  : ((int) py - 6);
                    PinkXP::drawCachedText (g, m.s, clampLabelRect ({ lx, ly, 24, 12 }, dialSafe),
                                            juce::Justification::centred, 8.0f, juce::Font::plain,
                                            PinkXP::ink.withAlpha (0.75f), true);
                }

                PinkXP::drawCachedText (g, "CORR",
                                        juce::Rectangle<int> (inner.getX(), inner.getY() + 2, inner.getWidth(), 12),
                                        juce::Justification::centred, 10.0f, juce::Font::bold, PinkXP::ink);
            }

            const juce::Colour needleCol = (smoothedCorr < 0.0f) ? PinkXP::sel : PinkXP::pink600;
            g.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
            g.setColour (needleCol);
            juce::String numTxt = juce::String (smoothedCorr, 2);
            if (smoothedCorr >= 0.0f) numTxt = "+" + numTxt;
            g.drawText (y2k::tr (numTxt),
                        juce::Rectangle<int> (inner.getX(), inner.getY() + 14, inner.getWidth(), 14),
                        juce::Justification::centred, false);
        }
    }

    // ---- Width / Balance 标签与数值 ----
    //   左列显示 WIDTH/BAL 标签，中段为凹槽与 LED 格，右列独立显示数值（避免与格子/边框重叠）
    {
        const auto valueInk = chromaKey ? PinkXP::ink : labelInk;

        if (! areaWidth.isEmpty())
        {
            const auto row = layoutBarRow (areaWidth);
            if (! chromaKey)
                PinkXP::drawCachedText (g, "WIDTH", row.labelRect, juce::Justification::centred,
                                        9.0f, juce::Font::bold, labelInk);
            g.setColour (valueInk);
            g.setFont (PinkXP::getFont (9.0f, juce::Font::plain));
            g.drawFittedText (y2k::tr (juce::String ((int) std::round (smoothedWidth * 100.0f)) + "%"),
                              row.valueRect, juce::Justification::centredRight, 1, 0.80f);
        }

        if (! areaBalance.isEmpty())
        {
            const auto row = layoutBarRow (areaBalance);
            if (! chromaKey)
                PinkXP::drawCachedText (g, "BAL", row.labelRect, juce::Justification::centred,
                                        9.0f, juce::Font::bold, labelInk);
            g.setColour (valueInk);
            g.setFont (PinkXP::getFont (9.0f, juce::Font::plain));

            const float b = juce::jlimit (-1.0f, 1.0f, smoothedBalance);
            juce::String txt;
            if (std::abs (b) < 0.02f)      txt = "C";
            else if (b < 0)                txt = "L" + juce::String ((int) std::round (-b * 100.0f));
            else                           txt = "R" + juce::String ((int) std::round ( b * 100.0f));
            g.drawFittedText (y2k::tr (txt), row.valueRect, juce::Justification::centredRight, 1, 0.80f);
        }
    }
}

void PhaseModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> content)
{
    const auto areas = computeAreas (content);

    if (PinkXP::isTransparentBackdrop())
    {
        // 透明模式：跳过静态层与不透明底色填充，保持底图纯 alpha=0，仅浮绘动态数据与读数
    }
    else if (PinkXP::isGreenScreenBackdrop())
    {
        // 绿幕：静态层（凹槽 / 网格 / 刻度）整层跳过，只铺纯绿并绘制动态数据图形与读数
        g.setColour (PinkXP::chromaKeyGreen());
        g.fillRect (content);
    }
    else
    {
        // 静态层已完整绘制底衬、四个凹槽框、Gonio 网格与轴标签、Dial 弧轨与刻度、
        // WIDTH/BAL 标签及平衡中线 —— 下方四个动态绘制函数不再重复 drawSunken。
        rebuildStaticLayerIfNeeded (content);
        drawStaticLayer (g, content);
    }

    drawGonio     (g, areas.gonio);
    drawCorrDial  (g, areas.dial);
    drawWidthBar  (g, areas.width);
    drawBalanceBar(g, areas.balance);

    PinkXP::Craft::drawCanvasFinishOverlay (g, content);
}

// ----------------------------------------------------------
// buildGlSnapshot —— GL 顶点流（SkinCraft 令牌驱动）
//   全部几何（凹槽/菱形/十字/弧形轨/刻度/单元条）+ 动态（点云/指针/亮块）
//   由 GL 绘制；文字由 paintGlOverlay 画在组件缓存。顶点为模块本地坐标。
// ----------------------------------------------------------
void PhaseModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto content = getContentBounds();
    if (content.getWidth() <= 8 || content.getHeight() <= 8)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaKey   = tokens.isChromaKey;
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool transparent = PinkXP::isTransparentBackdrop();

    // 子区域（与 layoutContent 相同划分）
    const auto areas = computeAreas (content);
    const auto areaGonio = areas.gonio;
    const auto areaDial = areas.dial;
    const auto areaWidth = areas.width;
    const auto areaBalance = areas.balance;

    auto snap = beginGlSnapshot();
    snap->renderToLogical = true;
    snap->logicalW        = content.getWidth();
    snap->logicalH        = content.getHeight();
    snap->canvasLocal     = content;
    snap->fxRect          = content;
    snap->snapToPixels    = false;

    // 主题底色铺底：透明模式跳过以保持纯 alpha=0；绿幕铺纯绿 #00FF00；正常模式铺主题底色
    if (! transparent)
        appendQuad (snap->vertices, content.toFloat(), phaseBackdropColour());

    // ---- Goniometer ----
    if (! areaGonio.isEmpty())
    {
        appendWellFrame (snap->vertices, areaGonio, PinkXP::content);
        auto inner = areaGonio.reduced (6);
        if (inner.getWidth() > 4 && inner.getHeight() > 4)
        {
            const float cx = (float) inner.getCentreX();
            const float cy = (float) inner.getCentreY();
            const float R  = (float) juce::jmin (inner.getWidth(), inner.getHeight()) * 0.48f;

            if (! chromaKey && showGonioGrid)
            {
                const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
                const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
                const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
                const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;
                const auto diamondC = isChrome
                                          ? PinkXP::pink400.interpolatedWith (PinkXP::hl, darkBg ? 0.32f : 0.22f)
                                          : isCrt
                                                ? PinkXP::sel.interpolatedWith (PinkXP::pink400, 0.45f)
                                                : PinkXP::pink400;
                appendGridLine (snap->vertices, cx, cy - R, cx + R, cy, true, diamondC);
                appendGridLine (snap->vertices, cx + R, cy, cx, cy + R, true, diamondC);
                appendGridLine (snap->vertices, cx, cy + R, cx - R, cy, true, diamondC);
                appendGridLine (snap->vertices, cx - R, cy, cx, cy - R, true, diamondC);

                appendGridLine (snap->vertices, cx - R, cy, cx + R, cy, true, diamondC);
                appendGridLine (snap->vertices, cx, cy - R, cx, cy + R, true, diamondC);

                appendGridLine (snap->vertices, cx - R, cy - R, cx, cy, false, PinkXP::pink300);
                appendGridLine (snap->vertices, cx + R, cy - R, cx, cy, false, PinkXP::pink300);

                if (isChrome)
                {
                    // Millennium Chrome：激光蚀刻微准星刻度（M/S 主轴 1/2 处）
                    const auto etchTickC = PinkXP::hl.interpolatedWith (PinkXP::ink, darkBg ? 0.30f : 0.45f)
                                               .withAlpha (darkBg ? 0.34f : 0.28f);
                    const float halfR = R * 0.5f;
                    appendLineSegment (snap->vertices, cx - 2.5f, cy - halfR, cx + 2.5f, cy - halfR, 0.45f, etchTickC);
                    appendLineSegment (snap->vertices, cx - 2.5f, cy + halfR, cx + 2.5f, cy + halfR, 0.45f, etchTickC);
                    appendLineSegment (snap->vertices, cx - halfR, cy - 2.5f, cx - halfR, cy + 2.5f, 0.45f, etchTickC);
                    appendLineSegment (snap->vertices, cx + halfR, cy - 2.5f, cx + halfR, cy + 2.5f, 0.45f, etchTickC);
                }
                else if (isCrt)
                {
                    // CRT Terminal：Tektronix 磷光十字准星微刻度（M/S 主轴 ±0.25/0.50/0.75R）
                    const auto crtTickC = PinkXP::sel.interpolatedWith (PinkXP::pink400, 0.40f).withAlpha (0.48f);
                    const float fracs[3] = { 0.25f, 0.50f, 0.75f };
                    for (int fi = 0; fi < 3; ++fi)
                    {
                        const float d = R * fracs[fi];
                        const float halfLen = (fi == 1) ? 3.5f : 2.0f;
                        appendLineSegment (snap->vertices, cx - halfLen, cy - d, cx + halfLen, cy - d, 0.42f, crtTickC);
                        appendLineSegment (snap->vertices, cx - halfLen, cy + d, cx + halfLen, cy + d, 0.42f, crtTickC);
                        appendLineSegment (snap->vertices, cx - d, cy - halfLen, cx - d, cy + halfLen, 0.42f, crtTickC);
                        appendLineSegment (snap->vertices, cx + d, cy - halfLen, cx + d, cy + halfLen, 0.42f, crtTickC);
                    }
                }
                else if (isNeon)
                {
                    // Neon Cyber：菱形四顶点发光微角标
                    const auto vGlow = PinkXP::sel.withAlpha (0.34f);
                    const auto vCore = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.45f).withAlpha (0.85f);
                    const juce::Point<float> corners[4] = { { cx, cy - R }, { cx + R, cy }, { cx, cy + R }, { cx - R, cy } };
                    for (const auto& pt : corners)
                    {
                        appendDisc (snap->vertices, pt.x, pt.y, 2.4f, vGlow, 8);
                        appendDisc (snap->vertices, pt.x, pt.y, 1.0f, vCore, 6);
                    }
                }
            }

            // 样本点云：L/R → M/S（受 gonioGain 与 gonioSize 控制）
            const int N = juce::jmin (oscL.size(), oscR.size());
            if (N > 1)
            {
                const int targetPoints = juce::jmax (96, inner.getWidth() * 2);
                const int stride = juce::jmax (1, N / targetPoints);
                const auto dotC = PinkXP::pink500;
                const float denom = (float) juce::jmax (1, N - 1);
                const auto innerF = inner.toFloat().expanded (1.0f);
                const float scaledR = R * gonioGain;
                const float hs = juce::jmax (0.5f, gonioSize * 0.5f);
                const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
                const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
                const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
                const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);

                if (gonioStyle == GonioStyle::trace)
                {
                    float lastX = 0.0f, lastY = 0.0f;
                    bool haveLast = false;
                    const float lineHalf = juce::jmax (0.4f, gonioSize * 0.35f * tokens.strokeWidthScale);
                    const bool glowTrace = tokens.strokeGlowPass && ! greenScreen && tokens.strokeGlowAlpha > 0.0f;
                    const float glowHalf = lineHalf * juce::jmax (1.25f, tokens.strokeGlowWidthMul);
                    const bool glassSoft = isGlass && ! greenScreen;
                    const bool coreSheen = (isChrome || isNeon || isCrt) && ! greenScreen;
                    const auto sheenCol  = dotC.interpolatedWith (juce::Colours::white, isNeon ? 0.62f : 0.48f);
                    const float sheenMul = isNeon ? 0.72f : (isCrt ? 0.62f : 0.52f);

                    for (int i = 0; i < N; i += stride)
                    {
                        const float L = oscL.getUnchecked (i);
                        const float Rs = oscR.getUnchecked (i);
                        if (! std::isfinite (L) || ! std::isfinite (Rs)) { haveLast = false; continue; }
                        const float Lc = juce::jlimit (-1.0f, 1.0f, L);
                        const float Rc = juce::jlimit (-1.0f, 1.0f, Rs);
                        float px = cx + (Lc - Rc) * 0.7071067f * scaledR;
                        float py = cy - (Lc + Rc) * 0.7071067f * scaledR;
                        if (! innerF.contains (px, py)) { haveLast = false; continue; }
                        if (haveLast)
                        {
                            const float frac = (float) i / denom;
                            const float a = greenScreen ? (0.55f + 0.45f * frac)
                                          : transparent ? (0.35f + 0.65f * frac)
                                                        : (0.20f + 0.60f * frac);
                            if (glowTrace)
                                appendLineSegment (snap->vertices, lastX, lastY, px, py, glowHalf,
                                                   dotC.withAlpha (a * tokens.strokeGlowAlpha));
                            else if (glassSoft)
                                appendLineSegment (snap->vertices, lastX, lastY, px, py, lineHalf * 1.65f,
                                                   dotC.withAlpha (a * 0.16f));
                            appendLineSegment (snap->vertices, lastX, lastY, px, py, lineHalf, dotC.withAlpha (a));
                            if (coreSheen)
                                appendLineSegment (snap->vertices, lastX, lastY, px, py,
                                                   juce::jmax (0.28f, lineHalf * 0.36f),
                                                   sheenCol.withAlpha (a * sheenMul));
                        }
                        lastX = px; lastY = py; haveLast = true;
                    }
                }
                else
                {
                    const bool pixelDots = tokens.pixelSnapped;
                    const bool crtHalo   = isCrt && ! greenScreen && tokens.glowIntensity > 0.0f;
                    const bool neonHalo  = isNeon && ! greenScreen && tokens.glowIntensity > 0.0f;
                    const bool glassDot  = isGlass && ! greenScreen;
                    const bool chromePin = isChrome && ! greenScreen && gonioSize >= 2.0f;
                    const float snapSize = juce::jmax (1.0f, std::round (gonioSize));
                    const int discSegs   = (gonioSize >= 2.5f) ? 8 : 6;
                    const auto pinCol    = dotC.interpolatedWith (juce::Colours::white, 0.70f);
                    const float pinR     = juce::jmax (0.42f, hs * 0.42f);
                    const float pinOff   = hs * 0.18f;

                    for (int i = 0; i < N; i += stride)
                    {
                        const float L = oscL.getUnchecked (i);
                        const float Rs = oscR.getUnchecked (i);
                        if (! std::isfinite (L) || ! std::isfinite (Rs)) continue;
                        const float Lc = juce::jlimit (-1.0f, 1.0f, L);
                        const float Rc = juce::jlimit (-1.0f, 1.0f, Rs);
                        const float M = (Lc + Rc) * 0.7071067f;
                        const float S = (Lc - Rc) * 0.7071067f;
                        const float px = cx + S * scaledR;
                        const float py = cy - M * scaledR;
                        if (! innerF.contains (px, py)) continue;

                        const float frac = (float) i / denom;
                        const float a = greenScreen ? 1.0f
                                      : transparent ? (0.35f + 0.65f * frac)
                                                    : (0.20f + 0.60f * frac);

                        if (pixelDots)
                        {
                            const float qx = std::floor (px - hs);
                            const float qy = std::floor (py - hs);
                            if (crtHalo)
                                appendQuad (snap->vertices, { qx - 1.0f, qy - 1.0f, snapSize + 2.0f, snapSize + 2.0f },
                                            dotC.withAlpha (a * 0.26f * tokens.glowIntensity));
                            appendQuad (snap->vertices, { qx, qy, snapSize, snapSize }, dotC.withAlpha (a));
                            if (crtHalo && snapSize >= 2.0f)
                                appendQuad (snap->vertices,
                                            { qx + snapSize * 0.25f, qy + snapSize * 0.25f,
                                              juce::jmax (1.0f, snapSize * 0.5f), juce::jmax (1.0f, snapSize * 0.5f) },
                                            pinCol.withAlpha (a * 0.55f));
                        }
                        else
                        {
                            if (neonHalo)
                                appendDisc (snap->vertices, px, py, hs * 2.1f + 0.9f,
                                            dotC.withAlpha (a * 0.26f * tokens.glowIntensity), 8);
                            else if (glassDot)
                                appendDisc (snap->vertices, px, py, hs * 1.45f + 0.4f,
                                            dotC.withAlpha (a * 0.16f), 8);
                            if (gonioSize >= 1.5f)
                                appendDisc (snap->vertices, px, py, hs, dotC.withAlpha (a), discSegs);
                            else
                                appendQuad (snap->vertices, { px - hs, py - hs, gonioSize, gonioSize }, dotC.withAlpha (a));
                            if (chromePin || (glassDot && gonioSize >= 2.0f))
                                appendDisc (snap->vertices, px - pinOff, py - pinOff, pinR,
                                            (glassDot ? juce::Colours::white : pinCol).withAlpha (a * (glassDot ? 0.62f : 0.78f)), 6);
                            else if (neonHalo && gonioSize >= 2.0f)
                                appendDisc (snap->vertices, px, py, pinR, pinCol.withAlpha (a * 0.75f), 6);
                        }
                    }
                }
            }
        }
    }

    // ---- Correlation Dial ----
    if (! areaDial.isEmpty())
    {
        appendWellFrame (snap->vertices, areaDial, PinkXP::content);
        auto inner = areaDial.reduced (6);
        if (inner.getWidth() > 8 && inner.getHeight() > 8)
        {
            const float cx = (float) inner.getCentreX();
            const float cy = (float) inner.getBottom();
            const float R = juce::jmin ((float) inner.getWidth() * 0.46f,
                                        (float) inner.getHeight() - (float) kDialReadoutReserve);
            if (R >= 10.0f)
            {
                constexpr float kPi = juce::MathConstants<float>::pi;

                auto thetaFor = [] (float corr) -> float
                {
                    const float t = juce::jlimit (-1.0f, 1.0f, corr) * 0.5f + 0.5f;
                    return t * juce::MathConstants<float>::pi;
                };
                auto pointAt = [&] (float theta, float r) -> juce::Point<float>
                {
                    return { cx - std::cos (theta) * r, cy - std::sin (theta) * r };
                };

                const float markVals[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };

                // 弧形轨 + 刻度（全风格平滑反走样弧线，绝不制造锯齿阶梯；绿幕隐藏静态刻度）
                const bool isChrome = (tokens.dialFinish == PinkXP::DialFinish::brushedMetal);
                const bool isModern = (tokens.dialFinish == PinkXP::DialFinish::studioSmooth);
                const bool isGlass  = (tokens.dialFinish == PinkXP::DialFinish::glassDome);
                const bool isNeon   = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
                const bool isCrt    = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
                const bool isPixel  = (tokens.dialFinish == PinkXP::DialFinish::pixelHardware);
                const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;

                if (! chromaKey)
                {
                    constexpr int arcSegs = 64;

                    if (isChrome)
                    {
                        // Millennium Chrome：CNC 拉丝金属内凹弧槽（外暗切边 + 拉丝凹槽 + 镜面外/内反光唇）
                        const auto outerCut = PinkXP::dark.withAlpha (darkBg ? 0.54f : 0.26f);
                        const auto grooveC  = PinkXP::content.interpolatedWith (PinkXP::dark, darkBg ? 0.34f : 0.14f)
                                                  .interpolatedWith (PinkXP::shdw, 0.38f)
                                                  .withAlpha (darkBg ? 0.72f : 0.44f);
                        const auto negArcC  = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.14f).withAlpha (0.70f);
                        const auto posArcC  = PinkXP::hl.interpolatedWith (PinkXP::pink300, 0.42f)
                                                  .withAlpha (darkBg ? 0.78f : 0.68f);
                        const auto outerRim = juce::Colours::white.withAlpha (darkBg ? 0.28f : 0.42f);
                        const auto innerLip = PinkXP::hl.withAlpha (darkBg ? 0.16f : 0.26f);

                        for (int s = 0; s < arcSegs; ++s)
                        {
                            const float th0 = kPi * (float) s / (float) arcSegs;
                            const float th1 = kPi * (float) (s + 1) / (float) arcSegs;
                            const float c0 = std::cos (th0), s0 = std::sin (th0);
                            const float c1 = std::cos (th1), s1 = std::sin (th1);

                            appendLineSegment (snap->vertices, cx - c0 * (R + 2.6f), cy - s0 * (R + 2.6f),
                                               cx - c1 * (R + 2.6f), cy - s1 * (R + 2.6f), 0.55f, outerCut);
                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 2.8f, grooveC);
                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 0.85f,
                                               (s < arcSegs / 2) ? negArcC : posArcC);
                            appendLineSegment (snap->vertices, cx - c0 * (R + 1.8f), cy - s0 * (R + 1.8f),
                                               cx - c1 * (R + 1.8f), cy - s1 * (R + 1.8f), 0.45f, outerRim);
                            appendLineSegment (snap->vertices, cx - c0 * (R - 2.3f), cy - s0 * (R - 2.3f),
                                               cx - c1 * (R - 2.3f), cy - s1 * (R - 2.3f), 0.40f, innerLip);
                        }

                        // 精雕金属主刻度（-1/-0.5/0/+0.5/+1）+ 4 道细次刻度（-0.75/-0.25/+0.25/+0.75）
                        const auto majorTickC = PinkXP::ink.interpolatedWith (PinkXP::hl, darkBg ? 0.36f : 0.16f).withAlpha (0.76f);
                        const auto majorGlint = juce::Colours::white.withAlpha (darkBg ? 0.20f : 0.34f);
                        const auto minorTickC = PinkXP::ink.withAlpha (darkBg ? 0.32f : 0.28f);
                        const float minorVals[4] = { -0.75f, -0.25f, 0.25f, 0.75f };
                        for (float mv : minorVals)
                        {
                            const float th = thetaFor (mv);
                            const auto p1 = pointAt (th, R - 2.4f);
                            const auto p2 = pointAt (th, R + 0.7f);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.40f, minorTickC);
                        }
                        for (float v : markVals)
                        {
                            const float th = thetaFor (v);
                            const auto p1 = pointAt (th, R - 4.2f);
                            const auto p2 = pointAt (th, R + 1.6f);
                            appendLineSegment (snap->vertices, p1.x + 0.55f, p1.y + 0.55f,
                                               p2.x + 0.55f, p2.y + 0.55f, 0.38f, majorGlint);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.62f, majorTickC);
                        }
                    }
                    else if (isGlass)
                    {
                        // Frosted Glass：表盘上沿柔和玻璃穹顶反光带 + 磨砂半透明弧轨 + 顶/底折光高光边
                        const auto domeBox = areaDial.reduced (2).toFloat();
                        const float domeH1 = domeBox.getHeight() * 0.20f;
                        const float domeH2 = domeBox.getHeight() * 0.16f;
                        const float domeA  = darkBg ? 0.075f : 0.16f;
                        appendQuad (snap->vertices, { domeBox.getX(), domeBox.getY(), domeBox.getWidth(), domeH1 },
                                    juce::Colours::white.withAlpha (domeA));
                        appendQuad (snap->vertices, { domeBox.getX(), domeBox.getY() + domeH1, domeBox.getWidth(), domeH2 },
                                    juce::Colours::white.withAlpha (domeA * 0.42f));

                        const auto frostWide = PinkXP::hl.interpolatedWith (PinkXP::pink300, 0.45f)
                                                   .withAlpha (darkBg ? 0.28f : 0.36f);
                        const auto negGlass  = PinkXP::sel.withAlpha (0.62f);
                        const auto posGlass  = PinkXP::pink300.interpolatedWith (juce::Colours::white, 0.30f).withAlpha (0.75f);
                        const auto outerRim  = juce::Colours::white.withAlpha (darkBg ? 0.26f : 0.42f);
                        const auto innerRim  = juce::Colours::white.withAlpha (darkBg ? 0.10f : 0.20f);

                        for (int s = 0; s < arcSegs; ++s)
                        {
                            const float th0 = kPi * (float) s / (float) arcSegs;
                            const float th1 = kPi * (float) (s + 1) / (float) arcSegs;
                            const float c0 = std::cos (th0), s0 = std::sin (th0);
                            const float c1 = std::cos (th1), s1 = std::sin (th1);

                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 3.0f, frostWide);
                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 0.90f,
                                               (s < arcSegs / 2) ? negGlass : posGlass);
                            appendLineSegment (snap->vertices, cx - c0 * (R + 1.8f), cy - s0 * (R + 1.8f),
                                               cx - c1 * (R + 1.8f), cy - s1 * (R + 1.8f), 0.45f, outerRim);
                            appendLineSegment (snap->vertices, cx - c0 * (R - 2.2f), cy - s0 * (R - 2.2f),
                                               cx - c1 * (R - 2.2f), cy - s1 * (R - 2.2f), 0.38f, innerRim);
                        }

                        const auto markC = PinkXP::ink.withAlpha (0.55f);
                        for (float v : markVals)
                        {
                            const float th = thetaFor (v);
                            const auto p1 = pointAt (th, R - 3.4f);
                            const auto p2 = pointAt (th, R + 1.2f);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.52f, markC);
                        }
                    }
                    else if (isNeon)
                    {
                        // Neon Cyber：双层发光霓虹弧轨 + 内层科技参考虚弧 + 发光径向刻度
                        const auto auraCol   = PinkXP::sel.withAlpha (0.22f);
                        const auto negTube   = PinkXP::sel.withAlpha (0.68f);
                        const auto posTube   = PinkXP::accent2.interpolatedWith (PinkXP::pink300, 0.40f).withAlpha (0.62f);
                        const auto hotCore   = PinkXP::pink200.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.92f);
                        const auto innerGuide = PinkXP::sel.withAlpha (0.22f);
                        const float rIn = R * 0.72f;

                        for (int s = 0; s < arcSegs; ++s)
                        {
                            const float th0 = kPi * (float) s / (float) arcSegs;
                            const float th1 = kPi * (float) (s + 1) / (float) arcSegs;
                            const float c0 = std::cos (th0), s0 = std::sin (th0);
                            const float c1 = std::cos (th1), s1 = std::sin (th1);

                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 4.2f, auraCol);
                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 1.85f,
                                               (s < arcSegs / 2) ? negTube : posTube);
                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 0.65f, hotCore);
                            if ((s & 1) == 0 && rIn >= 8.0f)
                                appendLineSegment (snap->vertices, cx - c0 * rIn, cy - s0 * rIn,
                                                   cx - c1 * rIn, cy - s1 * rIn, 0.40f, innerGuide);
                        }

                        const float minorVals[4] = { -0.75f, -0.25f, 0.25f, 0.75f };
                        const auto minorC = PinkXP::sel.withAlpha (0.45f);
                        for (float mv : minorVals)
                        {
                            const float th = thetaFor (mv);
                            const auto p1 = pointAt (th, R - 2.6f);
                            const auto p2 = pointAt (th, R + 0.8f);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.42f, minorC);
                        }
                        const auto tickGlow = PinkXP::sel.withAlpha (0.28f);
                        const auto tickCore = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.45f).withAlpha (0.92f);
                        for (float v : markVals)
                        {
                            const float th = thetaFor (v);
                            const auto p1 = pointAt (th, R - 4.0f);
                            const auto p2 = pointAt (th, R + 1.6f);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 1.4f, tickGlow);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.55f, tickCore);
                        }
                    }
                    else if (isCrt)
                    {
                        // CRT Terminal：Tektronix 平滑磷光矢量弧 + 内参考弧 + 9 档精密矢量刻度
                        const auto glowC  = PinkXP::sel.withAlpha (0.22f * juce::jmax (0.4f, tokens.glowIntensity));
                        const auto coreC  = PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.90f);
                        const auto innerC = PinkXP::sel.withAlpha (0.24f);
                        const float rSub  = juce::jmax (6.0f, R - 4.5f);

                        for (int s = 0; s < arcSegs; ++s)
                        {
                            const float th0 = kPi * (float) s / (float) arcSegs;
                            const float th1 = kPi * (float) (s + 1) / (float) arcSegs;
                            const float c0 = std::cos (th0), s0 = std::sin (th0);
                            const float c1 = std::cos (th1), s1 = std::sin (th1);

                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 2.6f, glowC);
                            appendLineSegment (snap->vertices, cx - c0 * R, cy - s0 * R,
                                               cx - c1 * R, cy - s1 * R, 0.75f, coreC);
                            if (R >= 16.0f)
                                appendLineSegment (snap->vertices, cx - c0 * rSub, cy - s0 * rSub,
                                                   cx - c1 * rSub, cy - s1 * rSub, 0.38f, innerC);
                        }

                        const float minorVals[4] = { -0.75f, -0.25f, 0.25f, 0.75f };
                        const auto minorC = PinkXP::sel.withAlpha (0.48f);
                        for (float mv : minorVals)
                        {
                            const float th = thetaFor (mv);
                            const auto p1 = pointAt (th, R - 3.0f);
                            const auto p2 = pointAt (th, R + 0.5f);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.38f, minorC);
                        }
                        const auto majorC = PinkXP::sel.withAlpha (0.88f);
                        for (float v : markVals)
                        {
                            const float th = thetaFor (v);
                            const auto p1 = pointAt (th, R - 5.0f);
                            const auto p2 = pointAt (th, R + 1.5f);
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.55f, majorC);
                        }
                    }
                    else
                    {
                        const auto trackWide = isModern ? PinkXP::pink300.withAlpha (0.24f)
                                                        : PinkXP::pink300.withAlpha (0.45f);
                        const auto trackThin = isModern ? PinkXP::pink400.withAlpha (0.58f)
                                                        : PinkXP::pink200;
                        const float wideHalf = isModern ? 1.8f : 2.8f;
                        const float thinHalf = isModern ? 0.65f : 0.95f;

                        for (int s = 0; s < arcSegs; ++s)
                        {
                            const float th0 = kPi * (float) s / (float) arcSegs;
                            const float th1 = kPi * (float) (s + 1) / (float) arcSegs;
                            const float x0 = cx - std::cos (th0) * R, y0 = cy - std::sin (th0) * R;
                            const float x1 = cx - std::cos (th1) * R, y1 = cy - std::sin (th1) * R;
                            appendLineSegment (snap->vertices, x0, y0, x1, y1, wideHalf, trackWide);
                            const auto segCore = (isModern && s < arcSegs / 2)
                                                     ? PinkXP::sel.withAlpha (0.46f)
                                                     : trackThin;
                            appendLineSegment (snap->vertices, x0, y0, x1, y1, thinHalf, segCore);
                        }

                        const auto markC = isModern ? PinkXP::ink.withAlpha (0.38f) : PinkXP::pink400;
                        const float tickHalf = isModern ? 0.48f : 0.60f;
                        for (float v : markVals)
                        {
                            const float th = thetaFor (v);
                            const auto p1 = pointAt (th, R - (isModern ? 3.2f : 3.5f));
                            const auto p2 = pointAt (th, R + (isModern ? 1.0f : 1.2f));
                            appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, tickHalf, markC);
                        }
                    }
                }

                // 指针与轴心（全风格平滑反走样；按材质令牌区分阴影/辉光/CNC 金属/玻璃穹顶/霓虹/磷光）
                const float th = thetaFor (smoothedCorr);
                const auto tip = pointAt (th, R - 4.0f);
                const juce::Colour needleCol = (smoothedCorr < 0.0f) ? PinkXP::sel : PinkXP::pink600;

                if (tokens.needleGlow && ! greenScreen)
                {
                    if (isNeon)
                    {
                        appendTaperedNeedleGl (snap->vertices, cx, cy, tip.x, tip.y, 3.4f, 1.4f,
                                               needleCol.withAlpha (0.26f));
                        appendTaperedNeedleGl (snap->vertices, cx, cy, tip.x, tip.y, 2.0f, 0.85f,
                                               needleCol.withAlpha (0.42f));
                    }
                    else
                    {
                        appendTaperedNeedleGl (snap->vertices, cx, cy, tip.x, tip.y, 2.6f, 1.0f,
                                               needleCol.withAlpha (0.24f));
                    }
                }

                if (tokens.needleShadow && ! chromaKey)
                {
                    const float shOffX = isPixel ? 1.0f : (isChrome ? 1.3f : 1.2f);
                    const float shOffY = isPixel ? 1.0f : ((isChrome || isGlass) ? 1.8f : 1.6f);
                    const float shAlpha = isPixel ? 0.35f : (isModern ? 0.18f : (isGlass ? 0.20f : (isChrome ? 0.28f : 0.24f)));
                    appendTaperedNeedleGl (snap->vertices, cx + shOffX, cy + shOffY,
                                           tip.x + shOffX, tip.y + shOffY,
                                           isModern ? 1.55f : 1.75f, isChrome ? 0.48f : 0.55f,
                                           PinkXP::dark.withAlpha (shAlpha));
                }

                appendTaperedNeedleGl (snap->vertices, cx, cy, tip.x, tip.y,
                                       isModern ? 1.45f : (isChrome ? 1.65f : (isCrt ? 1.35f : 1.55f)),
                                       isChrome ? 0.42f : (isModern || isGlass ? 0.45f : 0.48f), needleCol);

                if ((isChrome || isGlass) && ! greenScreen)
                {
                    const auto sp0 = pointAt (th, 3.5f);
                    const auto sp1 = pointAt (th, juce::jmax (5.0f, R - 5.2f));
                    appendLineSegment (snap->vertices, sp0.x - 0.30f, sp0.y - 0.30f,
                                       sp1.x - 0.30f, sp1.y - 0.30f, 0.36f,
                                       needleCol.interpolatedWith (juce::Colours::white, isGlass ? 0.50f : 0.58f)
                                                .withAlpha (isGlass ? 0.55f : 0.78f));
                }
                else if ((isNeon || isCrt) && ! greenScreen)
                {
                    appendLineSegment (snap->vertices, cx, cy, tip.x, tip.y, 0.40f,
                                       needleCol.interpolatedWith (juce::Colours::white, isNeon ? 0.72f : 0.52f));
                }

                if (isChrome)
                {
                    // CNC 圆形拉丝铬合金轴帽：外深切圈 + 亮镀铬环 + 阳极金属芯 + 镜面轴销
                    appendDisc (snap->vertices, cx, cy, 4.4f, PinkXP::dark, 24);
                    appendDisc (snap->vertices, cx, cy, 3.4f,
                                PinkXP::hl.interpolatedWith (juce::Colours::white, darkBg ? 0.30f : 0.45f), 20);
                    appendDisc (snap->vertices, cx, cy, 2.25f,
                                PinkXP::shdw.interpolatedWith (PinkXP::pink500, 0.55f), 16);
                    const float pinOff = greenScreen ? 0.0f : -0.25f;
                    appendDisc (snap->vertices, cx + pinOff, cy + pinOff, 1.05f,
                                juce::Colours::white.interpolatedWith (PinkXP::hl, 0.32f), 12);
                }
                else if (isGlass)
                {
                    // Frosted Glass：晶莹水晶穹顶轴帽（半透明外圈 + 水晶球体 + 底部折光 + 左上球面高光）
                    appendDisc (snap->vertices, cx, cy, 4.4f,
                                PinkXP::dark.withAlpha (greenScreen ? 1.0f : (darkBg ? 0.48f : 0.30f)), 24);
                    appendDisc (snap->vertices, cx, cy, 3.6f,
                                PinkXP::pink500.interpolatedWith (PinkXP::hl, 0.25f), 20);
                    if (! greenScreen)
                    {
                        appendDisc (snap->vertices, cx + 0.35f, cy + 0.45f, 2.3f,
                                    PinkXP::pink200.withAlpha (0.55f), 16);
                        appendDisc (snap->vertices, cx - 0.95f, cy - 0.95f, 1.35f,
                                    juce::Colours::white.withAlpha (0.82f), 12);
                    }
                }
                else if (isNeon)
                {
                    // Neon Cyber：发光霓虹环轴心（外晕 + 深烟底 + 亮霓虹环 + 炽白中心点）
                    if (! greenScreen)
                        appendDisc (snap->vertices, cx, cy, 5.4f, PinkXP::sel.withAlpha (0.26f), 20);
                    appendDisc (snap->vertices, cx, cy, 4.0f, PinkXP::dark, 20);
                    appendDisc (snap->vertices, cx, cy, 3.2f, PinkXP::sel.interpolatedWith (juce::Colours::white, 0.25f), 18);
                    appendDisc (snap->vertices, cx, cy, 2.0f, PinkXP::dark, 16);
                    appendDisc (snap->vertices, cx, cy, 0.95f, PinkXP::sel.interpolatedWith (juce::Colours::white, 0.68f), 10);
                }
                else if (isCrt)
                {
                    // CRT Terminal：极简空心磷光矢量环轴心
                    if (! greenScreen)
                        appendDisc (snap->vertices, cx, cy, 4.6f, PinkXP::sel.withAlpha (0.20f), 20);
                    appendDisc (snap->vertices, cx, cy, 3.5f, PinkXP::sel.interpolatedWith (juce::Colours::white, 0.22f), 18);
                    appendDisc (snap->vertices, cx, cy, 2.2f, chromaKey ? PinkXP::dark : PinkXP::content, 16);
                    appendDisc (snap->vertices, cx, cy, 0.70f, PinkXP::sel.interpolatedWith (juce::Colours::white, 0.50f), 10);
                }
                else
                {
                    const float hubOuter = isModern ? 3.4f : 3.8f;
                    const float hubInner = isModern ? 2.0f : 2.3f;
                    appendDisc (snap->vertices, cx, cy, hubOuter, PinkXP::dark, 20);
                    appendDisc (snap->vertices, cx, cy, hubInner, PinkXP::pink500, 16);
                    if (! greenScreen)
                        appendDisc (snap->vertices, cx - 0.7f, cy - 0.7f, 0.75f,
                                    juce::Colours::white.withAlpha (isModern ? 0.55f : 0.68f), 10);
                }
            }
        }
    }

    // ---- Width bar ----
    if (! areaWidth.isEmpty())
    {
        auto barArea = layoutBarRow (areaWidth).barRect;
        appendWellFrame (snap->vertices, barArea, PinkXP::content.darker (0.05f));
        auto innerBar = barArea.reduced (2);
        if (! innerBar.isEmpty() && barStyle == BarStyle::solid)
        {
            const float fillW = (float) innerBar.getWidth() * juce::jlimit (0.0f, 1.0f, smoothedWidth);
            const float y = (float) innerBar.getY() + 1.0f, h = (float) innerBar.getHeight() - 2.0f;
            const float x0 = (float) innerBar.getX();
            const float seg[4] = { 0.0f, 0.6f, 0.85f, 1.0f };
            const juce::Colour cols[3] = { PinkXP::pink200, PinkXP::pink500, PinkXP::sel };
            for (int s = 0; s < 3; ++s)
            {
                const float a = seg[s] * (float) innerBar.getWidth();
                const float b = juce::jmin (fillW, seg[s + 1] * (float) innerBar.getWidth());
                if (b > a)
                    appendStyledBarCell (snap->vertices, { x0 + a, y, b - a, h }, cols[s], true, 0.0f);
            }
        }
        else if (! innerBar.isEmpty())
        {
            const int cellSize = 6;
            const int cellGap  = tokens.defaultCellGapPx;
            const int step = cellSize + cellGap;
            const int N = juce::jmax (1, (innerBar.getWidth() + cellGap) / step);
            const int litN = juce::jlimit (0, N, (int) std::round (smoothedWidth * N));
            const int y = innerBar.getY() + 1;
            const int h = innerBar.getHeight() - 2;
            for (int i = 0; i < N; ++i)
            {
                const float t = (float) i / (float) juce::jmax (1, N - 1);
                juce::Colour base;
                if (t < 0.6f)      base = PinkXP::pink200;
                else if (t < 0.85f) base = PinkXP::pink500;
                else                base = PinkXP::sel;
                const bool lit = (i < litN);
                const float pulse = (lit && i == litN - 1) ? 0.45f : 0.0f;
                appendStyledBarCell (snap->vertices,
                                     { (float) (innerBar.getX() + i * step), (float) y,
                                       (float) cellSize, (float) h },
                                     base, lit, pulse);
            }
        }
    }

    // ---- Balance bar ----
    if (! areaBalance.isEmpty())
    {
        auto barArea = layoutBarRow (areaBalance).barRect;
        appendWellFrame (snap->vertices, barArea, PinkXP::content.darker (0.05f));
        auto innerBar = barArea.reduced (2);
        if (! innerBar.isEmpty())
        {
            const int cx = innerBar.getCentreX();
            appendGridLine (snap->vertices,
                            (float) cx, (float) innerBar.getY(),
                            (float) cx, (float) innerBar.getBottom(),
                            true, PinkXP::pink400);

            const float b = juce::jlimit (-1.0f, 1.0f, smoothedBalance);

            if (barStyle == BarStyle::solid)
            {
                const float halfW = (float) innerBar.getWidth() * 0.5f;
                const float len = std::abs (b) * halfW;
                const float y = (float) innerBar.getY() + 2.0f, h = (float) innerBar.getHeight() - 4.0f;
                if (b < 0.0f && len > 0.5f)
                    appendStyledBarCell (snap->vertices, { (float) cx - len, y, len, h }, PinkXP::pink500, true, 0.0f);
                else if (b > 0.0f && len > 0.5f)
                    appendStyledBarCell (snap->vertices, { (float) cx, y, len, h }, PinkXP::pink600, true, 0.0f);
            }
            else
            {
                const int cellSize = 4;
                const int cellGap  = tokens.defaultCellGapPx;
                const int step = cellSize + cellGap;
                const int halfCells = juce::jmax (1, (innerBar.getWidth() / 2) / step);
                const int litCells = (int) std::round (std::abs (b) * halfCells);
                const int y = innerBar.getY() + 2;
                const int h = innerBar.getHeight() - 4;
                for (int i = 0; i < halfCells; ++i)
                {
                    const int xL = cx - (i + 1) * step;
                    const int xR = cx + i * step + cellGap;
                    const bool litL = (b < 0.0f && i < litCells);
                    const bool litR = (b > 0.0f && i < litCells);
                    appendStyledBarCell (snap->vertices, { (float) xL, (float) y, (float) cellSize, (float) h },
                                         litL ? PinkXP::pink500 : PinkXP::pink200, litL,
                                         (litL && i == litCells - 1) ? 0.45f : 0.0f);
                    appendStyledBarCell (snap->vertices, { (float) xR, (float) y, (float) cellSize, (float) h },
                                         litR ? PinkXP::pink600 : PinkXP::pink200, litR,
                                         (litR && i == litCells - 1) ? 0.45f : 0.0f);
                }
            }
        }
    }

    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// Goniometer（CPU 回退动态层：静态凹槽/网格已在 staticLayer 绘制）
// ----------------------------------------------------------
void PhaseModule::drawGonio(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    auto inner = area.reduced(6);
    if (inner.getWidth() <= 4 || inner.getHeight() <= 4) return;

    const int N = juce::jmin(oscL.size(), oscR.size());
    if (N <= 1) return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool transparent = PinkXP::isTransparentBackdrop();

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getCentreY();
    const float R  = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.48f;

    const int targetPoints = juce::jmax(96, inner.getWidth() * 2);
    const int stride = juce::jmax(1, N / targetPoints);
    const float scaledR = R * gonioGain;
    const float hs = juce::jmax(0.5f, gonioSize * 0.5f);
    const float denom = (float) juce::jmax(1, N - 1);
    const auto innerF = inner.toFloat().expanded(1.0f);

    const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);

    if (gonioStyle == GonioStyle::trace)
    {
        float lastX = 0.0f, lastY = 0.0f;
        bool haveLast = false;
        const float lw = juce::jmax(0.8f, gonioSize * 0.65f * tokens.strokeWidthScale);
        const bool glowTrace  = tokens.strokeGlowPass && ! greenScreen && tokens.strokeGlowAlpha > 0.0f;
        const float glowW     = lw * juce::jmax(1.25f, tokens.strokeGlowWidthMul);
        const bool glassSoft  = isGlass && ! greenScreen;
        const bool coreSheen  = (isChrome || isNeon || isCrt) && ! greenScreen;
        const auto sheenCol   = PinkXP::pink500.interpolatedWith(juce::Colours::white, isNeon ? 0.62f : 0.48f);
        const float sheenMul  = isNeon ? 0.72f : (isCrt ? 0.62f : 0.52f);

        for (int i = 0; i < N; i += stride)
        {
            const float L = oscL.getUnchecked(i);
            const float Rs = oscR.getUnchecked(i);
            if (! std::isfinite(L) || ! std::isfinite(Rs)) { haveLast = false; continue; }
            const float Lc = juce::jlimit(-1.0f, 1.0f, L);
            const float Rc = juce::jlimit(-1.0f, 1.0f, Rs);
            const float px = cx + (Lc - Rc) * 0.7071067f * scaledR;
            const float py = cy - (Lc + Rc) * 0.7071067f * scaledR;
            if (! innerF.contains(px, py)) { haveLast = false; continue; }
            if (haveLast)
            {
                const float frac = (float) i / denom;
                const float alpha = greenScreen ? (0.55f + 0.45f * frac)
                                  : transparent ? (0.35f + 0.65f * frac)
                                                : (0.20f + 0.60f * frac);
                if (glowTrace)
                {
                    g.setColour(PinkXP::pink500.withAlpha(alpha * tokens.strokeGlowAlpha));
                    g.drawLine(lastX, lastY, px, py, glowW);
                }
                else if (glassSoft)
                {
                    g.setColour(PinkXP::pink500.withAlpha(alpha * 0.16f));
                    g.drawLine(lastX, lastY, px, py, lw * 1.65f);
                }
                g.setColour(PinkXP::pink500.withAlpha(alpha));
                g.drawLine(lastX, lastY, px, py, lw);
                if (coreSheen)
                {
                    g.setColour(sheenCol.withAlpha(alpha * sheenMul));
                    g.drawLine(lastX, lastY, px, py, juce::jmax(0.55f, lw * 0.36f));
                }
            }
            lastX = px; lastY = py; haveLast = true;
        }
        return;
    }

    const bool pixelDots = tokens.pixelSnapped;
    const bool crtHalo   = isCrt && ! greenScreen && tokens.glowIntensity > 0.0f;
    const bool neonHalo  = isNeon && ! greenScreen && tokens.glowIntensity > 0.0f;
    const bool glassDot  = isGlass && ! greenScreen;
    const bool chromePin = isChrome && ! greenScreen && gonioSize >= 2.0f;
    const float snapSize = juce::jmax(1.0f, std::round(gonioSize));
    const auto pinCol    = PinkXP::pink500.interpolatedWith(juce::Colours::white, 0.70f);
    const float pinR     = juce::jmax(0.42f, hs * 0.42f);
    const float pinOff   = hs * 0.18f;

    for (int i = 0; i < N; i += stride)
    {
        const float L = oscL.getUnchecked(i);
        const float Rs = oscR.getUnchecked(i);
        if (! std::isfinite(L) || ! std::isfinite(Rs)) continue;

        const float Lc = juce::jlimit(-1.0f, 1.0f, L);
        const float Rc = juce::jlimit(-1.0f, 1.0f, Rs);
        const float M  = (Lc + Rc) * 0.7071067f;
        const float S  = (Lc - Rc) * 0.7071067f;

        const float px = cx + S * scaledR;
        const float py = cy - M * scaledR;
        if (! innerF.contains(px, py))
            continue;

        const float frac = (float) i / denom;
        const float alpha = greenScreen ? 1.0f
                          : transparent ? (0.35f + 0.65f * frac)
                                        : (0.20f + 0.60f * frac);

        if (pixelDots)
        {
            const float qx = std::floor(px - hs);
            const float qy = std::floor(py - hs);
            if (crtHalo)
            {
                g.setColour(PinkXP::pink500.withAlpha(alpha * 0.26f * tokens.glowIntensity));
                g.fillRect(qx - 1.0f, qy - 1.0f, snapSize + 2.0f, snapSize + 2.0f);
            }
            g.setColour(PinkXP::pink500.withAlpha(alpha));
            g.fillRect(qx, qy, snapSize, snapSize);
            if (crtHalo && snapSize >= 2.0f)
            {
                g.setColour(pinCol.withAlpha(alpha * 0.55f));
                g.fillRect(qx + snapSize * 0.25f, qy + snapSize * 0.25f,
                           juce::jmax(1.0f, snapSize * 0.5f), juce::jmax(1.0f, snapSize * 0.5f));
            }
        }
        else
        {
            if (neonHalo)
            {
                const float hr = hs * 2.1f + 0.9f;
                g.setColour(PinkXP::pink500.withAlpha(alpha * 0.26f * tokens.glowIntensity));
                g.fillEllipse(px - hr, py - hr, hr * 2.0f, hr * 2.0f);
            }
            else if (glassDot)
            {
                const float hr = hs * 1.45f + 0.4f;
                g.setColour(PinkXP::pink500.withAlpha(alpha * 0.16f));
                g.fillEllipse(px - hr, py - hr, hr * 2.0f, hr * 2.0f);
            }
            g.setColour(PinkXP::pink500.withAlpha(alpha));
            if (gonioSize >= 1.5f)
                g.fillEllipse(px - hs, py - hs, gonioSize, gonioSize);
            else
                g.fillRect(px - hs, py - hs, gonioSize, gonioSize);
            if (chromePin || (glassDot && gonioSize >= 2.0f))
            {
                g.setColour((glassDot ? juce::Colours::white : pinCol).withAlpha(alpha * (glassDot ? 0.62f : 0.78f)));
                g.fillEllipse(px - pinOff - pinR, py - pinOff - pinR, pinR * 2.0f, pinR * 2.0f);
            }
            else if (neonHalo && gonioSize >= 2.0f)
            {
                g.setColour(pinCol.withAlpha(alpha * 0.75f));
                g.fillEllipse(px - pinR, py - pinR, pinR * 2.0f, pinR * 2.0f);
            }
        }
    }
}

// ----------------------------------------------------------
// Correlation Dial（CPU 回退动态层：凹槽/弧轨/刻度已在 staticLayer 绘制）
// ----------------------------------------------------------
void PhaseModule::drawCorrDial(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    auto inner = area.reduced(6);
    if (inner.getWidth() <= 8 || inner.getHeight() <= 8) return;

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getBottom();
    const float R  = juce::jmin((float) inner.getWidth() * 0.46f,
                                (float) inner.getHeight() - (float) kDialReadoutReserve);

    if (R < 10.0f) return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaKey   = tokens.isChromaKey;
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();

    auto thetaFor = [] (float corr) -> float
    {
        const float t = juce::jlimit(-1.0f, 1.0f, corr) * 0.5f + 0.5f;
        return t * juce::MathConstants<float>::pi;
    };
    auto pointAt = [&] (float theta, float r) -> juce::Point<float>
    {
        return { cx - std::cos(theta) * r, cy - std::sin(theta) * r };
    };

    const float th = thetaFor(smoothedCorr);
    const auto  tip = pointAt(th, R - 4.0f);
    const juce::Colour needleCol = (smoothedCorr < 0.0f) ? PinkXP::sel : PinkXP::pink600;
    const bool isChrome = (tokens.dialFinish == PinkXP::DialFinish::brushedMetal);
    const bool isModern = (tokens.dialFinish == PinkXP::DialFinish::studioSmooth);
    const bool isGlass  = (tokens.dialFinish == PinkXP::DialFinish::glassDome);
    const bool isNeon   = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
    const bool isCrt    = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
    const bool isPixel  = (tokens.dialFinish == PinkXP::DialFinish::pixelHardware);
    const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;

    if (tokens.needleGlow && ! greenScreen)
    {
        if (isNeon)
        {
            drawTaperedNeedleCpu(g, cx, cy, tip.x, tip.y, 3.4f, 1.4f, needleCol.withAlpha(0.26f));
            drawTaperedNeedleCpu(g, cx, cy, tip.x, tip.y, 2.0f, 0.85f, needleCol.withAlpha(0.42f));
        }
        else
        {
            drawTaperedNeedleCpu(g, cx, cy, tip.x, tip.y, 2.6f, 1.0f, needleCol.withAlpha(0.24f));
        }
    }

    if (tokens.needleShadow && ! chromaKey)
    {
        const float shOffX = isPixel ? 1.0f : (isChrome ? 1.3f : 1.2f);
        const float shOffY = isPixel ? 1.0f : ((isChrome || isGlass) ? 1.8f : 1.6f);
        const float shAlpha = isPixel ? 0.35f : (isModern ? 0.18f : (isGlass ? 0.20f : (isChrome ? 0.28f : 0.24f)));
        drawTaperedNeedleCpu(g, cx + shOffX, cy + shOffY, tip.x + shOffX, tip.y + shOffY,
                             isModern ? 1.55f : 1.75f, isChrome ? 0.48f : 0.55f, PinkXP::dark.withAlpha(shAlpha));
    }

    drawTaperedNeedleCpu(g, cx, cy, tip.x, tip.y,
                         isModern ? 1.45f : (isChrome ? 1.65f : (isCrt ? 1.35f : 1.55f)),
                         isChrome ? 0.42f : (isModern || isGlass ? 0.45f : 0.48f), needleCol);

    if ((isChrome || isGlass) && ! greenScreen)
    {
        const auto sp0 = pointAt(th, 3.5f);
        const auto sp1 = pointAt(th, juce::jmax(5.0f, R - 5.2f));
        g.setColour(needleCol.interpolatedWith(juce::Colours::white, isGlass ? 0.50f : 0.58f)
                             .withAlpha(isGlass ? 0.55f : 0.78f));
        g.drawLine(sp0.x - 0.30f, sp0.y - 0.30f, sp1.x - 0.30f, sp1.y - 0.30f, 0.72f);
    }
    else if ((isNeon || isCrt) && ! greenScreen)
    {
        g.setColour(needleCol.interpolatedWith(juce::Colours::white, isNeon ? 0.72f : 0.52f));
        g.drawLine(cx, cy, tip.x, tip.y, 0.8f);
    }

    if (isChrome)
    {
        g.setColour(PinkXP::dark);
        g.fillEllipse(cx - 4.4f, cy - 4.4f, 8.8f, 8.8f);
        g.setColour(PinkXP::hl.interpolatedWith(juce::Colours::white, darkBg ? 0.30f : 0.45f));
        g.fillEllipse(cx - 3.4f, cy - 3.4f, 6.8f, 6.8f);
        g.setColour(PinkXP::shdw.interpolatedWith(PinkXP::pink500, 0.55f));
        g.fillEllipse(cx - 2.25f, cy - 2.25f, 4.5f, 4.5f);
        const float pinOff = greenScreen ? 0.0f : -0.25f;
        g.setColour(juce::Colours::white.interpolatedWith(PinkXP::hl, 0.32f));
        g.fillEllipse(cx + pinOff - 1.05f, cy + pinOff - 1.05f, 2.1f, 2.1f);
    }
    else if (isGlass)
    {
        g.setColour(PinkXP::dark.withAlpha(greenScreen ? 1.0f : (darkBg ? 0.48f : 0.30f)));
        g.fillEllipse(cx - 4.4f, cy - 4.4f, 8.8f, 8.8f);
        g.setColour(PinkXP::pink500.interpolatedWith(PinkXP::hl, 0.25f));
        g.fillEllipse(cx - 3.6f, cy - 3.6f, 7.2f, 7.2f);
        if (! greenScreen)
        {
            g.setColour(PinkXP::pink200.withAlpha(0.55f));
            g.fillEllipse(cx + 0.35f - 2.3f, cy + 0.45f - 2.3f, 4.6f, 4.6f);
            g.setColour(juce::Colours::white.withAlpha(0.82f));
            g.fillEllipse(cx - 0.95f - 1.35f, cy - 0.95f - 1.35f, 2.7f, 2.7f);
        }
    }
    else if (isNeon)
    {
        if (! greenScreen)
        {
            g.setColour(PinkXP::sel.withAlpha(0.26f));
            g.fillEllipse(cx - 5.4f, cy - 5.4f, 10.8f, 10.8f);
        }
        g.setColour(PinkXP::dark);
        g.fillEllipse(cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        g.setColour(PinkXP::sel.interpolatedWith(juce::Colours::white, 0.25f));
        g.fillEllipse(cx - 3.2f, cy - 3.2f, 6.4f, 6.4f);
        g.setColour(PinkXP::dark);
        g.fillEllipse(cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        g.setColour(PinkXP::sel.interpolatedWith(juce::Colours::white, 0.68f));
        g.fillEllipse(cx - 0.95f, cy - 0.95f, 1.9f, 1.9f);
    }
    else if (isCrt)
    {
        if (! greenScreen)
        {
            g.setColour(PinkXP::sel.withAlpha(0.20f));
            g.fillEllipse(cx - 4.6f, cy - 4.6f, 9.2f, 9.2f);
        }
        g.setColour(PinkXP::sel.interpolatedWith(juce::Colours::white, 0.22f));
        g.fillEllipse(cx - 3.5f, cy - 3.5f, 7.0f, 7.0f);
        g.setColour(chromaKey ? PinkXP::dark : PinkXP::content);
        g.fillEllipse(cx - 2.2f, cy - 2.2f, 4.4f, 4.4f);
        g.setColour(PinkXP::sel.interpolatedWith(juce::Colours::white, 0.50f));
        g.fillEllipse(cx - 0.70f, cy - 0.70f, 1.4f, 1.4f);
    }
    else
    {
        const float hubOuter = isModern ? 3.4f : 3.8f;
        const float hubInner = isModern ? 2.0f : 2.3f;
        g.setColour(PinkXP::dark);
        g.fillEllipse(cx - hubOuter, cy - hubOuter, hubOuter * 2.0f, hubOuter * 2.0f);
        g.setColour(PinkXP::pink500);
        g.fillEllipse(cx - hubInner, cy - hubInner, hubInner * 2.0f, hubInner * 2.0f);
        if (! greenScreen)
        {
            g.setColour(juce::Colours::white.withAlpha(isModern ? 0.55f : 0.68f));
            g.fillEllipse(cx - 1.45f, cy - 1.45f, 1.5f, 1.5f);
        }
    }

    // 动态数值读数（绿幕下保留）
    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    g.setColour(needleCol);
    juce::String numTxt = juce::String(smoothedCorr, 2);
    if (smoothedCorr >= 0.0f) numTxt = "+" + numTxt;
    g.drawText(y2k::tr (numTxt),
               juce::Rectangle<int>(inner.getX(), inner.getY() + 14, inner.getWidth(), 14),
               juce::Justification::centred, false);
}

// ----------------------------------------------------------
// Width bar（CPU 回退动态层：凹槽与标签已在 staticLayer 绘制）
// ----------------------------------------------------------
void PhaseModule::drawWidthBar(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    const auto row = layoutBarRow (area);
    auto barArea  = row.barRect;
    auto innerBar = barArea.reduced(2);
    if (innerBar.isEmpty()) return;

    const auto& tokens = PinkXP::getSkinTokens();

    if (barStyle == BarStyle::solid)
    {
        const float fillW = (float) innerBar.getWidth() * juce::jlimit(0.0f, 1.0f, smoothedWidth);
        const float seg[4] = { 0.0f, 0.6f, 0.85f, 1.0f };
        const juce::Colour cols[3] = { PinkXP::pink200, PinkXP::pink500, PinkXP::sel };
        const auto barF = innerBar.toFloat();
        for (int s = 0; s < 3; ++s)
        {
            const float a = seg[s] * (float) innerBar.getWidth();
            const float b = juce::jmin(fillW, seg[s + 1] * (float) innerBar.getWidth());
            if (b > a)
            {
                const juce::Rectangle<float> segR ((float) innerBar.getX() + a,
                                                   (float) innerBar.getY() + 1.0f,
                                                   b - a,
                                                   (float) innerBar.getHeight() - 2.0f);
                PinkXP::Craft::drawContinuousBar(g, barF, segR, cols[s], false, false);
            }
        }
    }
    else
    {
        const int cellSize = 6;
        const int cellGap  = tokens.defaultCellGapPx;
        const int step = cellSize + cellGap;
        const int N = juce::jmax(1, (innerBar.getWidth() + cellGap) / step);
        const int litN = juce::jlimit(0, N, (int) std::round(smoothedWidth * N));

        for (int i = 0; i < N; ++i)
        {
            const int x = innerBar.getX() + i * step;
            const int y = innerBar.getY() + 1;
            const int h = innerBar.getHeight() - 2;
            const float t = (float) i / (float) juce::jmax(1, N - 1);
            juce::Colour base;
            if (t < 0.6f)      base = PinkXP::pink200;
            else if (t < 0.85f) base = PinkXP::pink500;
            else                base = PinkXP::sel;
            const bool lit = (i < litN);
            const float pulse = (lit && i == litN - 1) ? 0.45f : 0.0f;
            PinkXP::Craft::drawMeterCell(g, juce::Rectangle<int>(x, y, cellSize, h), base, lit, pulse);
        }
    }

    // 动态数值读数（独立右列，绿幕/透明下保留）
    g.setColour(tokens.isChromaKey ? PinkXP::ink : PinkXP::readableInkOn (phaseBackdropColour()));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawFittedText(y2k::tr (juce::String((int) std::round(smoothedWidth * 100.0f)) + "%"),
                     row.valueRect, juce::Justification::centredRight, 1, 0.80f);
}

// ----------------------------------------------------------
// Balance bar（中心为 0，-1=L / +1=R；凹槽/标签/中线已在 staticLayer 绘制）
// ----------------------------------------------------------
void PhaseModule::drawBalanceBar(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    const auto row = layoutBarRow (area);
    auto barArea  = row.barRect;
    auto innerBar = barArea.reduced(2);
    if (innerBar.isEmpty()) return;

    const auto& tokens = PinkXP::getSkinTokens();
    const int cx = innerBar.getCentreX();
    const float b = juce::jlimit(-1.0f, 1.0f, smoothedBalance);

    if (barStyle == BarStyle::solid)
    {
        const float len = std::abs(b) * (float) innerBar.getWidth() * 0.5f;
        const float y = (float) innerBar.getY() + 2.0f, h = (float) innerBar.getHeight() - 4.0f;
        const auto barF = innerBar.toFloat();
        if (b < 0.0f && len > 0.5f)
            PinkXP::Craft::drawContinuousBar(g, barF, juce::Rectangle<float>((float) cx - len, y, len, h), PinkXP::pink500, false, false);
        else if (b > 0.0f && len > 0.5f)
            PinkXP::Craft::drawContinuousBar(g, barF, juce::Rectangle<float>((float) cx, y, len, h), PinkXP::pink600, false, false);
    }
    else
    {
        const int cellSize = 4;
        const int cellGap  = tokens.defaultCellGapPx;
        const int step = cellSize + cellGap;
        const int halfCells = juce::jmax(1, (innerBar.getWidth() / 2) / step);
        const int litCells = (int) std::round(std::abs(b) * halfCells);
        const int y = innerBar.getY() + 2;
        const int h = innerBar.getHeight() - 4;

        for (int i = 0; i < halfCells; ++i)
        {
            const int xL = cx - (i + 1) * step;
            const int xR = cx + i * step + cellGap;
            const bool litL = (b < 0.0f && i < litCells);
            const bool litR = (b > 0.0f && i < litCells);
            PinkXP::Craft::drawMeterCell(g, juce::Rectangle<int>(xL, y, cellSize, h),
                                         litL ? PinkXP::pink500 : PinkXP::pink200, litL,
                                         (litL && i == litCells - 1) ? 0.45f : 0.0f);
            PinkXP::Craft::drawMeterCell(g, juce::Rectangle<int>(xR, y, cellSize, h),
                                         litR ? PinkXP::pink600 : PinkXP::pink200, litR,
                                         (litR && i == litCells - 1) ? 0.45f : 0.0f);
        }
    }

    // 动态数值读数（绿幕下保留）：L30 / C / R40
    juce::String txt;
    if (std::abs(b) < 0.02f)       txt = "C";
    else if (b < 0)                txt = "L" + juce::String((int) std::round(-b * 100.0f));
    else                           txt = "R" + juce::String((int) std::round( b * 100.0f));

    g.setColour(tokens.isChromaKey ? PinkXP::ink : PinkXP::readableInkOn (phaseBackdropColour()));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawFittedText(y2k::tr (txt), row.valueRect,
                     juce::Justification::centredRight, 1, 0.80f);
}

void PhaseModule::rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds)
{
    if (contentBounds.isEmpty())
    {
        staticLayer = juce::Image();
        staticLayerContentBounds = {};
        staticLayerAppearanceGen = 0;
        staticLayerFontGen       = 0;
        return;
    }

    const auto curAppGen  = PinkXP::getAppearanceGeneration();
    const auto curFontGen = PinkXP::getFontGeneration();

    if (staticLayer.isValid()
        && staticLayerContentBounds == contentBounds
        && staticLayer.getWidth() == contentBounds.getWidth()
        && staticLayer.getHeight() == contentBounds.getHeight()
        && staticLayerAppearanceGen == curAppGen
        && staticLayerFontGen == curFontGen)
    {
        return;
    }

    staticLayer = juce::Image(juce::Image::ARGB,
                              contentBounds.getWidth(),
                              contentBounds.getHeight(),
                              true);
    staticLayer.clear(staticLayer.getBounds(), juce::Colours::transparentBlack);
    staticLayerContentBounds = contentBounds;
    staticLayerAppearanceGen = curAppGen;
    staticLayerFontGen       = curFontGen;

    const auto& tokens = PinkXP::getSkinTokens();
    const auto backdropCol = phaseBackdropColour();
    const auto labelInk    = PinkXP::readableInkOn (backdropCol);

    juce::Graphics sg(staticLayer);
    if (! PinkXP::isTransparentBackdrop())
    {
        sg.setColour(backdropCol);
        sg.fillRect(staticLayer.getBounds());
    }

    auto localContent = juce::Rectangle<int>(0, 0, contentBounds.getWidth(), contentBounds.getHeight());
    const auto staticAreas = computeAreas (localContent);
    const auto staticAreaGonio = staticAreas.gonio;
    const auto staticAreaDial  = staticAreas.dial;
    const auto staticAreaWidth = staticAreas.width;
    const auto staticAreaBalance = staticAreas.balance;

    // ---- Goniometer 静态层（凹槽 + 菱形/十字/对角线 + 轴标签）----
    if (! staticAreaGonio.isEmpty())
    {
        PinkXP::Craft::drawPanelWell(sg, staticAreaGonio, PinkXP::content, PinkXP::WellKind::canvas);
        auto inner = staticAreaGonio.reduced(6);
        if (showGonioGrid && inner.getWidth() > 4 && inner.getHeight() > 4)
        {
            const float cx = (float) inner.getCentreX();
            const float cy = (float) inner.getCentreY();
            const float R  = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.48f;

            const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
            const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
            const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
            const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;
            const auto diamondC = isChrome
                                      ? PinkXP::pink400.interpolatedWith(PinkXP::hl, darkBg ? 0.32f : 0.22f)
                                      : isCrt
                                            ? PinkXP::sel.interpolatedWith(PinkXP::pink400, 0.45f)
                                            : PinkXP::pink400;

            PinkXP::Craft::drawGridLine(sg, cx, cy - R, cx + R, cy, true, diamondC);
            PinkXP::Craft::drawGridLine(sg, cx + R, cy, cx, cy + R, true, diamondC);
            PinkXP::Craft::drawGridLine(sg, cx, cy + R, cx - R, cy, true, diamondC);
            PinkXP::Craft::drawGridLine(sg, cx - R, cy, cx, cy - R, true, diamondC);

            PinkXP::Craft::drawGridLine(sg, cx - R, cy, cx + R, cy, true, diamondC);
            PinkXP::Craft::drawGridLine(sg, cx, cy - R, cx, cy + R, true, diamondC);

            PinkXP::Craft::drawGridLine(sg, cx - R, cy - R, cx, cy, false, PinkXP::pink300);
            PinkXP::Craft::drawGridLine(sg, cx + R, cy - R, cx, cy, false, PinkXP::pink300);

            if (isChrome)
            {
                const auto etchTickC = PinkXP::hl.interpolatedWith(PinkXP::ink, darkBg ? 0.30f : 0.45f)
                                           .withAlpha(darkBg ? 0.34f : 0.28f);
                const float halfR = R * 0.5f;
                sg.setColour(etchTickC);
                sg.drawLine(cx - 2.5f, cy - halfR, cx + 2.5f, cy - halfR, 0.9f);
                sg.drawLine(cx - 2.5f, cy + halfR, cx + 2.5f, cy + halfR, 0.9f);
                sg.drawLine(cx - halfR, cy - 2.5f, cx - halfR, cy + 2.5f, 0.9f);
                sg.drawLine(cx + halfR, cy - 2.5f, cx + halfR, cy + 2.5f, 0.9f);
            }
            else if (isCrt)
            {
                const auto crtTickC = PinkXP::sel.interpolatedWith(PinkXP::pink400, 0.40f).withAlpha(0.48f);
                const float fracs[3] = { 0.25f, 0.50f, 0.75f };
                sg.setColour(crtTickC);
                for (int fi = 0; fi < 3; ++fi)
                {
                    const float d = R * fracs[fi];
                    const float halfLen = (fi == 1) ? 3.5f : 2.0f;
                    sg.drawLine(cx - halfLen, cy - d, cx + halfLen, cy - d, 0.85f);
                    sg.drawLine(cx - halfLen, cy + d, cx + halfLen, cy + d, 0.85f);
                    sg.drawLine(cx - d, cy - halfLen, cx - d, cy + halfLen, 0.85f);
                    sg.drawLine(cx + d, cy - halfLen, cx + d, cy + halfLen, 0.85f);
                }
            }
            else if (isNeon)
            {
                const auto vGlow = PinkXP::sel.withAlpha(0.34f);
                const auto vCore = PinkXP::sel.interpolatedWith(juce::Colours::white, 0.45f).withAlpha(0.85f);
                const juce::Point<float> corners[4] = { { cx, cy - R }, { cx + R, cy }, { cx, cy + R }, { cx - R, cy } };
                for (const auto& pt : corners)
                {
                    sg.setColour(vGlow);
                    sg.fillEllipse(pt.x - 2.4f, pt.y - 2.4f, 4.8f, 4.8f);
                    sg.setColour(vCore);
                    sg.fillEllipse(pt.x - 1.0f, pt.y - 1.0f, 2.0f, 2.0f);
                }
            }

            const auto safe = staticAreaGonio.reduced(3);
            sg.setColour(PinkXP::ink.withAlpha(0.85f));
            sg.setFont(PinkXP::getAxisFont(9.0f, juce::Font::bold));
            sg.drawText(y2k::tr ("M"),
                        clampLabelRect({ juce::roundToInt(cx) - 10, juce::roundToInt(cy - R) - 12, 20, 12 }, safe),
                        juce::Justification::centred, false);
            sg.drawText(y2k::tr ("S"),
                        clampLabelRect({ juce::roundToInt(cx + R) + 2, juce::roundToInt(cy) - 6, 12, 12 }, safe),
                        juce::Justification::centred, false);
            sg.setColour(PinkXP::ink.withAlpha(0.55f));
            sg.drawText(y2k::tr ("L"),
                        clampLabelRect({ juce::roundToInt(cx - R) - 12, juce::roundToInt(cy - R) - 12, 12, 12 }, safe),
                        juce::Justification::centred, false);
            sg.drawText(y2k::tr ("R"),
                        clampLabelRect({ juce::roundToInt(cx + R), juce::roundToInt(cy - R) - 12, 12, 12 }, safe),
                        juce::Justification::centred, false);
        }
    }

    // ---- Correlation Dial 静态层（凹槽 + 弧形轨 + 刻度 + 刻度/标题文字）----
    if (! staticAreaDial.isEmpty())
    {
        PinkXP::Craft::drawPanelWell(sg, staticAreaDial, PinkXP::content, PinkXP::WellKind::canvas);
        auto inner = staticAreaDial.reduced(6);
        const auto dialSafe = staticAreaDial.reduced(3);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getBottom();
        const float R = juce::jmin((float) inner.getWidth() * 0.46f,
                                   (float) inner.getHeight() - (float) kDialReadoutReserve);
        if (inner.getWidth() > 8 && inner.getHeight() > 8 && R >= 10.0f)
        {
            constexpr float kHalfPi = juce::MathConstants<float>::halfPi;
            auto thetaFor = [] (float corr) -> float
            {
                const float t = juce::jlimit(-1.0f, 1.0f, corr) * 0.5f + 0.5f;
                return t * juce::MathConstants<float>::pi;
            };
            auto pointAt = [&] (float theta, float r) -> juce::Point<float>
            {
                return { cx - std::cos(theta) * r, cy - std::sin(theta) * r };
            };

            const struct { float v; const char* s; } marks[] = {
                { -1.0f, "-1" }, { -0.5f, "-.5" }, { 0.0f, "0" },
                {  0.5f, "+.5"}, {  1.0f, "+1" }
            };

            const bool isChrome = (tokens.dialFinish == PinkXP::DialFinish::brushedMetal);
            const bool isModern = (tokens.dialFinish == PinkXP::DialFinish::studioSmooth);
            const bool isNeon   = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
            const bool isCrt    = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
            const bool isGlass  = (tokens.dialFinish == PinkXP::DialFinish::glassDome);
            const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;

            if (isChrome)
            {
                juce::Path outerCut, arcTrack, negHalf, posHalf, outerRim, innerLip;
                outerCut.addCentredArc(cx, cy, R + 2.6f, R + 2.6f, 0.0f, -kHalfPi, kHalfPi, true);
                arcTrack.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, kHalfPi, true);
                negHalf.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, 0.0f, true);
                posHalf.addCentredArc(cx, cy, R, R, 0.0f, 0.0f, kHalfPi, true);
                outerRim.addCentredArc(cx, cy, R + 1.8f, R + 1.8f, 0.0f, -kHalfPi, kHalfPi, true);
                innerLip.addCentredArc(cx, cy, R - 2.3f, R - 2.3f, 0.0f, -kHalfPi, kHalfPi, true);

                sg.setColour(PinkXP::dark.withAlpha(darkBg ? 0.54f : 0.26f));
                sg.strokePath(outerCut, juce::PathStrokeType(1.1f));
                sg.setColour(PinkXP::content.interpolatedWith(PinkXP::dark, darkBg ? 0.34f : 0.14f)
                                 .interpolatedWith(PinkXP::shdw, 0.38f)
                                 .withAlpha(darkBg ? 0.72f : 0.44f));
                sg.strokePath(arcTrack, juce::PathStrokeType(5.6f));
                sg.setColour(PinkXP::sel.interpolatedWith(juce::Colours::white, 0.14f).withAlpha(0.70f));
                sg.strokePath(negHalf, juce::PathStrokeType(1.7f));
                sg.setColour(PinkXP::hl.interpolatedWith(PinkXP::pink300, 0.42f).withAlpha(darkBg ? 0.78f : 0.68f));
                sg.strokePath(posHalf, juce::PathStrokeType(1.7f));
                sg.setColour(juce::Colours::white.withAlpha(darkBg ? 0.28f : 0.42f));
                sg.strokePath(outerRim, juce::PathStrokeType(0.9f));
                sg.setColour(PinkXP::hl.withAlpha(darkBg ? 0.16f : 0.26f));
                sg.strokePath(innerLip, juce::PathStrokeType(0.8f));

                const float minorVals[4] = { -0.75f, -0.25f, 0.25f, 0.75f };
                sg.setColour(PinkXP::ink.withAlpha(darkBg ? 0.32f : 0.28f));
                for (float mv : minorVals)
                {
                    const float th = thetaFor(mv);
                    const auto p1 = pointAt(th, R - 2.4f);
                    const auto p2 = pointAt(th, R + 0.7f);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 0.8f);
                }

                const auto majorTickC = PinkXP::ink.interpolatedWith(PinkXP::hl, darkBg ? 0.36f : 0.16f).withAlpha(0.76f);
                const auto majorGlint = juce::Colours::white.withAlpha(darkBg ? 0.20f : 0.34f);
                for (auto& m : marks)
                {
                    const float th = thetaFor(m.v);
                    const auto p1 = pointAt(th, R - 4.2f);
                    const auto p2 = pointAt(th, R + 1.6f);
                    sg.setColour(majorGlint);
                    sg.drawLine(p1.x + 0.55f, p1.y + 0.55f, p2.x + 0.55f, p2.y + 0.55f, 0.75f);
                    sg.setColour(majorTickC);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 1.25f);
                }
            }
            else if (isGlass)
            {
                const auto domeBox = staticAreaDial.reduced(2).toFloat();
                const float domeH  = domeBox.getHeight() * 0.36f;
                const float domeA  = darkBg ? 0.085f : 0.17f;
                juce::ColourGradient domeGrad(juce::Colours::white.withAlpha(domeA), domeBox.getX(), domeBox.getY(),
                                              juce::Colours::white.withAlpha(0.0f),  domeBox.getX(), domeBox.getY() + domeH,
                                              false);
                sg.setGradientFill(domeGrad);
                sg.fillRoundedRectangle(domeBox.withHeight(domeH), juce::jmin(tokens.cardRadius, 5.0f));

                juce::Path arcTrack, negHalf, posHalf, outerRim, innerRim;
                arcTrack.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, kHalfPi, true);
                negHalf.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, 0.0f, true);
                posHalf.addCentredArc(cx, cy, R, R, 0.0f, 0.0f, kHalfPi, true);
                outerRim.addCentredArc(cx, cy, R + 1.8f, R + 1.8f, 0.0f, -kHalfPi, kHalfPi, true);
                innerRim.addCentredArc(cx, cy, R - 2.2f, R - 2.2f, 0.0f, -kHalfPi, kHalfPi, true);

                sg.setColour(PinkXP::hl.interpolatedWith(PinkXP::pink300, 0.45f).withAlpha(darkBg ? 0.28f : 0.36f));
                sg.strokePath(arcTrack, juce::PathStrokeType(6.0f));
                sg.setColour(PinkXP::sel.withAlpha(0.62f));
                sg.strokePath(negHalf, juce::PathStrokeType(1.8f));
                sg.setColour(PinkXP::pink300.interpolatedWith(juce::Colours::white, 0.30f).withAlpha(0.75f));
                sg.strokePath(posHalf, juce::PathStrokeType(1.8f));
                sg.setColour(juce::Colours::white.withAlpha(darkBg ? 0.26f : 0.42f));
                sg.strokePath(outerRim, juce::PathStrokeType(0.9f));
                sg.setColour(juce::Colours::white.withAlpha(darkBg ? 0.10f : 0.20f));
                sg.strokePath(innerRim, juce::PathStrokeType(0.75f));

                sg.setColour(PinkXP::ink.withAlpha(0.55f));
                for (auto& m : marks)
                {
                    const float th = thetaFor(m.v);
                    const auto p1 = pointAt(th, R - 3.4f);
                    const auto p2 = pointAt(th, R + 1.2f);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 1.05f);
                }
            }
            else if (isNeon)
            {
                juce::Path arcTrack, negHalf, posHalf;
                arcTrack.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, kHalfPi, true);
                negHalf.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, 0.0f, true);
                posHalf.addCentredArc(cx, cy, R, R, 0.0f, 0.0f, kHalfPi, true);

                sg.setColour(PinkXP::sel.withAlpha(0.22f));
                sg.strokePath(arcTrack, juce::PathStrokeType(8.4f));
                sg.setColour(PinkXP::sel.withAlpha(0.68f));
                sg.strokePath(negHalf, juce::PathStrokeType(3.7f));
                sg.setColour(PinkXP::accent2.interpolatedWith(PinkXP::pink300, 0.40f).withAlpha(0.62f));
                sg.strokePath(posHalf, juce::PathStrokeType(3.7f));
                sg.setColour(PinkXP::pink200.interpolatedWith(juce::Colours::white, 0.55f).withAlpha(0.92f));
                sg.strokePath(arcTrack, juce::PathStrokeType(1.3f));

                const float rIn = R * 0.72f;
                if (rIn >= 8.0f)
                {
                    sg.setColour(PinkXP::sel.withAlpha(0.22f));
                    constexpr int segs = 64;
                    for (int s = 0; s < segs; s += 2)
                    {
                        const float th0 = juce::MathConstants<float>::pi * (float) s / (float) segs;
                        const float th1 = juce::MathConstants<float>::pi * (float) (s + 1) / (float) segs;
                        sg.drawLine(cx - std::cos(th0) * rIn, cy - std::sin(th0) * rIn,
                                    cx - std::cos(th1) * rIn, cy - std::sin(th1) * rIn, 0.8f);
                    }
                }

                const float minorVals[4] = { -0.75f, -0.25f, 0.25f, 0.75f };
                sg.setColour(PinkXP::sel.withAlpha(0.45f));
                for (float mv : minorVals)
                {
                    const float th = thetaFor(mv);
                    const auto p1 = pointAt(th, R - 2.6f);
                    const auto p2 = pointAt(th, R + 0.8f);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 0.85f);
                }
                const auto tickGlow = PinkXP::sel.withAlpha(0.28f);
                const auto tickCore = PinkXP::sel.interpolatedWith(juce::Colours::white, 0.45f).withAlpha(0.92f);
                for (auto& m : marks)
                {
                    const float th = thetaFor(m.v);
                    const auto p1 = pointAt(th, R - 4.0f);
                    const auto p2 = pointAt(th, R + 1.6f);
                    sg.setColour(tickGlow);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 2.8f);
                    sg.setColour(tickCore);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 1.1f);
                }
            }
            else if (isCrt)
            {
                juce::Path arcTrack;
                arcTrack.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, kHalfPi, true);
                sg.setColour(PinkXP::sel.withAlpha(0.22f * juce::jmax(0.4f, tokens.glowIntensity)));
                sg.strokePath(arcTrack, juce::PathStrokeType(5.2f));
                sg.setColour(PinkXP::sel.interpolatedWith(PinkXP::pink300, 0.45f).withAlpha(0.90f));
                sg.strokePath(arcTrack, juce::PathStrokeType(1.5f));
                if (R >= 16.0f)
                {
                    juce::Path subArc;
                    subArc.addCentredArc(cx, cy, R - 4.5f, R - 4.5f, 0.0f, -kHalfPi, kHalfPi, true);
                    sg.setColour(PinkXP::sel.withAlpha(0.24f));
                    sg.strokePath(subArc, juce::PathStrokeType(0.75f));
                }

                const float minorVals[4] = { -0.75f, -0.25f, 0.25f, 0.75f };
                sg.setColour(PinkXP::sel.withAlpha(0.48f));
                for (float mv : minorVals)
                {
                    const float th = thetaFor(mv);
                    const auto p1 = pointAt(th, R - 3.0f);
                    const auto p2 = pointAt(th, R + 0.5f);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 0.76f);
                }
                sg.setColour(PinkXP::sel.withAlpha(0.88f));
                for (auto& m : marks)
                {
                    const float th = thetaFor(m.v);
                    const auto p1 = pointAt(th, R - 5.0f);
                    const auto p2 = pointAt(th, R + 1.5f);
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, 1.1f);
                }
            }
            else
            {
                juce::Path arcTrack;
                arcTrack.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, kHalfPi, true);
                const auto trackWide = isModern ? PinkXP::pink300.withAlpha(0.24f)
                                                : PinkXP::pink300.withAlpha(0.45f);
                sg.setColour(trackWide);
                sg.strokePath(arcTrack, juce::PathStrokeType(isModern ? 3.6f : 5.6f));

                if (isModern)
                {
                    juce::Path negHalf, posHalf;
                    negHalf.addCentredArc(cx, cy, R, R, 0.0f, -kHalfPi, 0.0f, true);
                    posHalf.addCentredArc(cx, cy, R, R, 0.0f, 0.0f, kHalfPi, true);
                    sg.setColour(PinkXP::sel.withAlpha(0.46f));
                    sg.strokePath(negHalf, juce::PathStrokeType(1.3f));
                    sg.setColour(PinkXP::pink400.withAlpha(0.58f));
                    sg.strokePath(posHalf, juce::PathStrokeType(1.3f));
                }
                else
                {
                    sg.setColour(PinkXP::pink200);
                    sg.strokePath(arcTrack, juce::PathStrokeType(1.9f));
                }

                const auto markC = isModern ? PinkXP::ink.withAlpha(0.38f) : PinkXP::pink400;
                sg.setColour(markC);
                for (auto& m : marks)
                {
                    const float th = thetaFor(m.v);
                    const auto p1 = pointAt(th, R - (isModern ? 3.2f : 3.5f));
                    const auto p2 = pointAt(th, R + (isModern ? 1.0f : 1.2f));
                    sg.drawLine(p1.x, p1.y, p2.x, p2.y, isModern ? 0.95f : 1.2f);
                }
            }

            sg.setColour(PinkXP::ink.withAlpha(0.75f));
            sg.setFont(PinkXP::getAxisFont(8.0f, juce::Font::plain));
            for (auto& m : marks)
            {
                const float th = thetaFor(m.v);
                const auto pOut = pointAt(th, R + 10.0f);
                const int lx = tokens.pixelSnapped ? (juce::roundToInt(pOut.x) - 12) : ((int) pOut.x - 12);
                const int ly = tokens.pixelSnapped ? (juce::roundToInt(pOut.y) - 6)  : ((int) pOut.y - 6);
                sg.drawText(y2k::tr (m.s), clampLabelRect({ lx, ly, 24, 12 }, dialSafe),
                            juce::Justification::centred, false);
            }

            sg.setColour(PinkXP::ink);
            sg.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
            sg.drawText(y2k::tr ("CORR"),
                        juce::Rectangle<int>(inner.getX(), inner.getY() + 2, inner.getWidth(), 12),
                        juce::Justification::centred, false);
        }
    }

    // ---- Width 静态层（标签 + 凹槽）----
    if (! staticAreaWidth.isEmpty())
    {
        const auto row = layoutBarRow (staticAreaWidth);
        sg.setColour(labelInk);
        sg.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        sg.drawText(y2k::tr ("WIDTH"), row.labelRect,
                    juce::Justification::centred, false);
        PinkXP::Craft::drawPanelWell(sg, row.barRect,
                                     PinkXP::content.darker(0.05f), PinkXP::WellKind::trough);
    }

    // ---- Balance 静态层（标签 + 凹槽 + 中线）----
    if (! staticAreaBalance.isEmpty())
    {
        const auto row = layoutBarRow (staticAreaBalance);
        sg.setColour(labelInk);
        sg.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        sg.drawText(y2k::tr ("BAL"), row.labelRect,
                    juce::Justification::centred, false);
        const auto balBar = row.barRect;
        PinkXP::Craft::drawPanelWell(sg, balBar, PinkXP::content.darker(0.05f), PinkXP::WellKind::trough);
        const auto innerBal = balBar.reduced(2);
        if (! innerBal.isEmpty())
            PinkXP::Craft::drawGridLine(sg,
                                        (float) innerBal.getCentreX(), (float) innerBal.getY(),
                                        (float) innerBal.getCentreX(), (float) innerBal.getBottom(),
                                        true, PinkXP::pink400);
    }
}

void PhaseModule::drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const
{
    if (! staticLayer.isValid() || contentBounds.isEmpty())
        return;

    g.drawImageAt(staticLayer, contentBounds.getX(), contentBounds.getY());
}

void PhaseModule::invalidateStaticLayer()
{
    staticLayer = juce::Image();
    staticLayerContentBounds = {};
    staticLayerAppearanceGen = 0;
    staticLayerFontGen       = 0;
}