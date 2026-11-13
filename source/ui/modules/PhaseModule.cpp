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
    constexpr int barLabelW   = 44;   // 纯文字列（不含右侧间隙）
    constexpr int barLabelGap = 4;
    // 条形本体的左边界 = 标签的居中范围（文字列 + 间隙一起参与居中）
    constexpr int barLeftInset = barLabelW + barLabelGap;
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
    return s;
}

void PhaseModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    // 缺键 = 保持当前值；构造默认 = 旧 Phase 外观
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
    out.push_back (ParamDef::choice ("barStyle", "Bar style",
        { "Segmented", "Solid" },
        "Segmented: pixel cells. Solid: one continuous filled bar.")
        .in ("Style").when ([this] { return items.width || items.bal; }));
}

double PhaseModule::getParamValue (const juce::String& id) const
{
    if (id == "itemGonio")   return items.gonio ? 1.0 : 0.0;
    if (id == "itemCorr")    return items.corr ? 1.0 : 0.0;
    if (id == "itemWidth")   return items.width ? 1.0 : 0.0;
    if (id == "itemBal")     return items.bal ? 1.0 : 0.0;
    if (id == "gonioStyle")  return (double) (int) gonioStyle;
    if (id == "barStyle")    return (double) (int) barStyle;
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
        const float a = y2k::pacing::alphaForDt (0.25f, frame.dtSec);
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
        out.kind = 2;                 // 声呐
        out.rect = areaGonio.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }
    if (areaDial.contains (panelPos))
    {
        out.kind = 3;                 // 幽灵指针
        out.rect = areaDial.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }
    return false;
}

void PhaseModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    if (hit.kind == 2)
    {
        ctx.addModuleActor (makeSonarRingActor (hit.pos));
        // M/S 反算（与 drawGonio 的映射一致，E.4.3）
        const float R = juce::jmin (hit.rect.getWidth(), hit.rect.getHeight()) * 0.5f;
        const float cx = hit.rect.getCentreX();
        const float cy = hit.rect.getCentreY();
        const float m = (cy - hit.pos.y) / juce::jmax (1.0f, R);
        const float s = (hit.pos.x - cx) / juce::jmax (1.0f, R);
        ctx.bubble (hit.pos.translated (0.0f, -22.0f),
                    "M " + juce::String (m, 2) + "  S " + juce::String (s, 2), ToyRole::Reward);
    }
    else if (hit.kind == 3)
    {
        // 反馈 A3：虚影围绕真实指针生成 —— 几何与角度完全复刻 drawCorrDial
        const auto inner = hit.rect.toNearestInt().reduced (6);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getBottom();
        const float R  = juce::jmin ((float) inner.getWidth() * 0.46f,
                                     (float) inner.getHeight() - 28.0f);
        const float th = (juce::jlimit (-1.0f, 1.0f, smoothedCorr) * 0.5f + 0.5f)
                         * juce::MathConstants<float>::pi;
        ctx.addModuleActor (makeGhostNeedleActor ({ cx, cy }, juce::jmax (4.0f, R), th));
        ctx.bubble (hit.pos.translated (0.0f, -22.0f),
                    "CORR " + juce::String (smoothedCorr, 2), ToyRole::Reward);
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
    //   WIDTH/BAL 标签与数值。
    const auto areas = computeAreas (canvas);
    const auto areaGonio = areas.gonio;
    const auto areaDial = areas.dial;
    const auto areaWidth = areas.width;
    const auto areaBalance = areas.balance;

    // ---- Gonio 轴标签 ----
    if (! areaGonio.isEmpty())
    {
        auto inner = areaGonio.reduced (6);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getCentreY();
        const float R  = (float) juce::jmin (inner.getWidth(), inner.getHeight()) * 0.48f;
        const auto axisStrong = PinkXP::ink.withAlpha (0.85f);
        const auto axisWeak   = PinkXP::ink.withAlpha (0.55f);
        PinkXP::drawCachedText (g, "M", juce::Rectangle<int> ((int) cx - 10, (int) (cy - R) - 12, 20, 12),
                                juce::Justification::centred, 9.0f, juce::Font::bold, axisStrong, true);
        PinkXP::drawCachedText (g, "S", juce::Rectangle<int> ((int) (cx + R) + 2, (int) cy - 6, 12, 12),
                                juce::Justification::centredLeft, 9.0f, juce::Font::bold, axisStrong, true);
        PinkXP::drawCachedText (g, "L", juce::Rectangle<int> ((int) (cx - R) - 12, (int) (cy - R) - 12, 12, 12),
                                juce::Justification::centredRight, 9.0f, juce::Font::bold, axisWeak, true);
        PinkXP::drawCachedText (g, "R", juce::Rectangle<int> ((int) (cx + R), (int) (cy - R) - 12, 12, 12),
                                juce::Justification::centredLeft, 9.0f, juce::Font::bold, axisWeak, true);
    }

    // ---- Dial 刻度 + CORR 读数 ----
    if (! areaDial.isEmpty())
    {
        auto inner = areaDial.reduced (6);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getBottom();
        const int readoutReserve = 28;
        const float R = juce::jmin ((float) inner.getWidth() * 0.46f,
                                    (float) inner.getHeight() - (float) readoutReserve);
        if (R >= 10.0f)
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
                PinkXP::drawCachedText (g, m.s, juce::Rectangle<int> ((int) px - 12, (int) py - 6, 24, 12),
                                        juce::Justification::centred, 8.0f, juce::Font::plain,
                                        PinkXP::ink.withAlpha (0.75f), true);
            }

            const juce::Colour needleCol = (smoothedCorr < 0.0f) ? PinkXP::sel : PinkXP::pink600;
            PinkXP::drawCachedText (g, "CORR",
                                    juce::Rectangle<int> (inner.getX(), inner.getY() + 2, inner.getWidth(), 12),
                                    juce::Justification::centred, 10.0f, juce::Font::bold, PinkXP::ink);
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
    //   标签在条形左侧的空列内水平居中（列宽 = 条形左边界 barLeftInset）
    {
        if (! areaWidth.isEmpty())
        {
            PinkXP::drawCachedText (g, "WIDTH", areaWidth.withWidth (barLeftInset), juce::Justification::centred,
                                    9.0f, juce::Font::bold, PinkXP::ink);
            g.setFont (PinkXP::getFont (9.0f, juce::Font::plain));
            g.drawText (y2k::tr (juce::String ((int) std::round (smoothedWidth * 100.0f)) + "%"),
                        areaWidth.withTrimmedLeft (barLeftInset).withTrimmedRight (4),
                        juce::Justification::centredRight, false);
        }

        if (! areaBalance.isEmpty())
        {
            PinkXP::drawCachedText (g, "BAL", areaBalance.withWidth (barLeftInset), juce::Justification::centred,
                                    9.0f, juce::Font::bold, PinkXP::ink);
            g.setFont (PinkXP::getFont (9.0f, juce::Font::plain));

            const float b = juce::jlimit (-1.0f, 1.0f, smoothedBalance);
            juce::String txt;
            if (std::abs (b) < 0.02f)      txt = "C";
            else if (b < 0)                txt = "L" + juce::String ((int) std::round (-b * 100.0f));
            else                           txt = "R" + juce::String ((int) std::round ( b * 100.0f));
            g.drawText (y2k::tr (txt),
                        areaBalance.withTrimmedLeft (barLeftInset).withTrimmedRight (4),
                        juce::Justification::centredRight, false);
        }
    }
}

void PhaseModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    rebuildStaticLayerIfNeeded(content);
    drawStaticLayer(g, content);

    drawGonio     (g, areaGonio);
    drawCorrDial  (g, areaDial);
    drawWidthBar  (g, areaWidth);
    drawBalanceBar(g, areaBalance);
}

// ----------------------------------------------------------
// buildGlSnapshot —— GL 顶点流（v2.9.4）
//   全部几何（凹陷/菱形/十字/弧形轨/刻度/单元条）+ 动态（点云/指针/亮块）
//   由 GL 绘制；文字由 paintGlOverlay 画在组件缓存。顶点为模块本地坐标。
// ----------------------------------------------------------
void PhaseModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto content = getContentBounds();
    if (content.getWidth() <= 8 || content.getHeight() <= 8)
        return;

    // 子区域（与 layoutContent 相同划分）
    const auto areas = computeAreas (content);
    const auto areaGonio = areas.gonio;
    const auto areaDial = areas.dial;
    const auto areaWidth = areas.width;
    const auto areaBalance = areas.balance;

    auto snap = beginGlSnapshot();
    snap->renderToLogical = true;
    snap->logicalW = content.getWidth();
    snap->logicalH = content.getHeight();
    snap->canvasLocal = content;

    auto pushSunken = [&] (juce::Rectangle<int> canvas, juce::Colour contentCol)
    {
        auto cr = canvas.toFloat();
        appendQuad (snap->vertices, cr, contentCol);
        appendQuad (snap->vertices, { cr.getX(), cr.getY(), (float) cr.getWidth(), 2.0f }, PinkXP::dark);
        appendQuad (snap->vertices, { cr.getX(), cr.getY(), 2.0f, (float) cr.getHeight() }, PinkXP::dark);
        appendQuad (snap->vertices, { cr.getX(), cr.getBottom() - 2.0f, (float) cr.getWidth(), 2.0f }, PinkXP::hl);
        appendQuad (snap->vertices, { cr.getRight() - 2.0f, cr.getY(), 2.0f, (float) cr.getHeight() }, PinkXP::hl);
        appendQuad (snap->vertices, { cr.getX() + 2.0f, cr.getY() + 2.0f, (float) cr.getWidth() - 4.0f, 1.0f }, PinkXP::shdw);
        appendQuad (snap->vertices, { cr.getX() + 2.0f, cr.getY() + 2.0f, 1.0f, (float) cr.getHeight() - 4.0f }, PinkXP::shdw);
        appendQuad (snap->vertices, { cr.getX() + 2.0f, cr.getBottom() - 3.0f, (float) cr.getWidth() - 4.0f, 1.0f }, juce::Colours::white);
        appendQuad (snap->vertices, { cr.getRight() - 3.0f, cr.getY() + 2.0f, 1.0f, (float) cr.getHeight() - 4.0f }, juce::Colours::white);
    };

    // 主题底色铺底：WIDTH/BAL 标签区、子区域之间的间隙没有各自的
    //   sunken 填充，GL FBO 未覆盖像素会透出黑色（浅色主题下尤其明显）。
    //   整个画布一次铺满，覆盖 upper/lower 之间的 6px 间隙。
    appendQuad (snap->vertices, content.toFloat(), PinkXP::content);

    // ---- Goniometer ----
    if (! areaGonio.isEmpty())
    {
        pushSunken (areaGonio, PinkXP::content);
        auto inner = areaGonio.reduced (6);
        if (inner.getWidth() > 4 && inner.getHeight() > 4)
        {
            const float cx = (float) inner.getCentreX();
            const float cy = (float) inner.getCentreY();
            const float R  = (float) juce::jmin (inner.getWidth(), inner.getHeight()) * 0.48f;

            // 菱形边界
            const auto diamondC = PinkXP::pink300.withAlpha (0.55f);
            appendLineSegment (snap->vertices, cx, cy - R, cx + R, cy, 0.5f, diamondC);
            appendLineSegment (snap->vertices, cx + R, cy, cx, cy + R, 0.5f, diamondC);
            appendLineSegment (snap->vertices, cx, cy + R, cx - R, cy, 0.5f, diamondC);
            appendLineSegment (snap->vertices, cx - R, cy, cx, cy - R, 0.5f, diamondC);

            // 中心十字
            const auto crossC = PinkXP::pink300.withAlpha (0.5f);
            appendLineSegment (snap->vertices, cx - R, cy, cx + R, cy, 0.5f, crossC);
            appendLineSegment (snap->vertices, cx, cy - R, cx, cy + R, 0.5f, crossC);

            // L/R 对角虚线（2×2 点）
            const auto diagC = PinkXP::pink200.withAlpha (0.6f);
            for (int d = 0; d < (int) R; d += 5)
            {
                appendQuad (snap->vertices, { cx - R + d, cy - R + d, 2.0f, 2.0f }, diagC);
                appendQuad (snap->vertices, { cx + R - d - 2, cy - R + d, 2.0f, 2.0f }, diagC);
            }

            // 样本点云：L/R → M/S
            const int N = juce::jmin (oscL.size(), oscR.size());
            if (N > 1)
            {
                const int targetPoints = juce::jmax (96, inner.getWidth() * 2);
                const int stride = juce::jmax (1, N / targetPoints);
                const auto dotC = PinkXP::pink500;
                const float dr = dotC.getFloatRed(), dg = dotC.getFloatGreen(), db = dotC.getFloatBlue();
                const float denom = (float) juce::jmax (1, N - 1);
                const auto innerF = inner.toFloat().expanded (1.0f);

                if (gonioStyle == GonioStyle::trace)
                {
                    // 轨迹样式：相邻样本连成细线，越新越亮
                    float lastX = 0.0f, lastY = 0.0f;
                    bool haveLast = false;
                    for (int i = 0; i < N; i += stride)
                    {
                        const float L = oscL.getUnchecked (i);
                        const float Rs = oscR.getUnchecked (i);
                        if (! std::isfinite (L) || ! std::isfinite (Rs)) { haveLast = false; continue; }
                        const float Lc = juce::jlimit (-1.0f, 1.0f, L);
                        const float Rc = juce::jlimit (-1.0f, 1.0f, Rs);
                        const float px = cx + (Lc - Rc) * 0.7071067f * R;
                        const float py = cy - (Lc + Rc) * 0.7071067f * R;
                        if (! innerF.contains (px, py)) { haveLast = false; continue; }
                        if (haveLast)
                        {
                            const float a = 0.2f + 0.6f * ((float) i / denom);
                            appendLineSegment (snap->vertices, lastX, lastY, px, py, 0.5f, dotC.withAlpha (a));
                        }
                        lastX = px; lastY = py; haveLast = true;
                    }
                }
                else
                for (int i = 0; i < N; i += stride)
                {
                    const float L = oscL.getUnchecked (i);
                    const float Rs = oscR.getUnchecked (i);
                    if (! std::isfinite (L) || ! std::isfinite (Rs)) continue;
                    const float Lc = juce::jlimit (-1.0f, 1.0f, L);
                    const float Rc = juce::jlimit (-1.0f, 1.0f, Rs);
                    const float M = (Lc + Rc) * 0.7071067f;
                    const float S = (Lc - Rc) * 0.7071067f;
                    const float px = cx + S * R;
                    const float py = cy - M * R;
                    if (! innerF.contains (px, py)) continue;
                    const float a = 0.2f + 0.6f * ((float) i / denom);
                    const float q[36] = {
                        px - 0.5f, py - 0.5f, dr, dg, db, a,
                        px + 1.5f, py - 0.5f, dr, dg, db, a,
                        px - 0.5f, py + 1.5f, dr, dg, db, a,
                        px + 1.5f, py - 0.5f, dr, dg, db, a,
                        px + 1.5f, py + 1.5f, dr, dg, db, a,
                        px - 0.5f, py + 1.5f, dr, dg, db, a,
                    };
                    snap->vertices.insert (snap->vertices.end(), q, q + 36);
                }
            }
        }
    }

    // ---- Correlation Dial ----
    if (! areaDial.isEmpty())
    {
        pushSunken (areaDial, PinkXP::content);
        auto inner = areaDial.reduced (6);
        if (inner.getWidth() > 8 && inner.getHeight() > 8)
        {
            const float cx = (float) inner.getCentreX();
            const float cy = (float) inner.getBottom();
            const int readoutReserve = 28;
            const float R = juce::jmin ((float) inner.getWidth() * 0.46f,
                                        (float) inner.getHeight() - (float) readoutReserve);
            if (R >= 10.0f)
            {
                auto thetaFor = [] (float corr) -> float
                {
                    const float t = juce::jlimit (-1.0f, 1.0f, corr) * 0.5f + 0.5f;
                    return t * juce::MathConstants<float>::pi;
                };
                auto pointAt = [&] (float theta, float r) -> juce::Point<float>
                {
                    return { cx - std::cos (theta) * r, cy - std::sin (theta) * r };
                };

                // 弧形轨（上半圆，64 段）：粗 6px（半宽 3）+ 细 2px（半宽 1）
                //   θ ∈ [0, π]（θ=0 正左 → θ=π 正右，θ=π/2 正上），与旧版
                //   pointAt 的 "cx - cos / cy - sin" 公式一致（上一版误用标准
                //   数学系 cos/sin，只画出了 0→+1 的右半段）。
                constexpr int arcSegs = 64;
                const auto trackWide = PinkXP::pink300.withAlpha (0.45f);
                const auto trackThin = PinkXP::pink200;
                for (int s = 0; s < arcSegs; ++s)
                {
                    const float th0 = juce::MathConstants<float>::pi * (float) s / (float) arcSegs;
                    const float th1 = juce::MathConstants<float>::pi * (float) (s + 1) / (float) arcSegs;
                    const float x0 = cx - std::cos (th0) * R, y0 = cy - std::sin (th0) * R;
                    const float x1 = cx - std::cos (th1) * R, y1 = cy - std::sin (th1) * R;
                    appendLineSegment (snap->vertices, x0, y0, x1, y1, 3.0f, trackWide);
                    appendLineSegment (snap->vertices, x0, y0, x1, y1, 1.0f, trackThin);
                }

                // 刻度短线（5 条）
                const float markVals[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
                const auto markC = PinkXP::pink400;
                for (float v : markVals)
                {
                    const float th = thetaFor (v);
                    const auto p1 = pointAt (th, R - 3.0f);
                    const auto p2 = pointAt (th, R + 1.0f);
                    appendLineSegment (snap->vertices, p1.x, p1.y, p2.x, p2.y, 0.6f, markC);
                }

                // 指针（阴影 + 本体 + 轴心）
                const float th = thetaFor (smoothedCorr);
                const auto tip = pointAt (th, R - 4.0f);
                appendLineSegment (snap->vertices, cx + 1, cy + 1, tip.x + 1, tip.y + 1, 1.5f,
                                   PinkXP::pink700.withAlpha (0.35f));
                const juce::Colour needleCol = (smoothedCorr < 0.0f) ? PinkXP::sel : PinkXP::pink600;
                appendLineSegment (snap->vertices, cx, cy, tip.x, tip.y, 1.0f, needleCol);
                appendDisc (snap->vertices, cx, cy, 3.5f, PinkXP::dark, 16);
                appendDisc (snap->vertices, cx, cy, 2.0f, PinkXP::pink500, 16);
            }
        }
    }

    // ---- Width bar ----
    if (! areaWidth.isEmpty())
    {
        auto barArea = areaWidth.withTrimmedLeft (barLeftInset);
        pushSunken (barArea, PinkXP::content.darker (0.05f));
        auto innerBar = barArea.reduced (2);
        if (! innerBar.isEmpty() && barStyle == BarStyle::solid)
        {
            // 实心样式：一条连续填充，颜色随长度分段（与像素格同一色阶）
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
                    appendQuad (snap->vertices, { x0 + a, y, b - a, h }, cols[s]);
            }
        }
        else if (! innerBar.isEmpty())
        {
            const int cellSize = 6, cellGap = 1, step = cellSize + cellGap;
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
                appendQuad (snap->vertices,
                            { (float) (innerBar.getX() + i * step), (float) y,
                              (float) cellSize, (float) h },
                            i < litN ? base : base.withAlpha (0.18f));
            }
        }
    }

    // ---- Balance bar ----
    if (! areaBalance.isEmpty())
    {
        auto barArea = areaBalance.withTrimmedLeft (barLeftInset);
        pushSunken (barArea, PinkXP::content.darker (0.05f));
        auto innerBar = barArea.reduced (2);
        if (! innerBar.isEmpty())
        {
            const int cx = innerBar.getCentreX();
            appendLineSegment (snap->vertices,
                               (float) cx, (float) innerBar.getY(),
                               (float) cx, (float) innerBar.getBottom(),
                               0.5f, PinkXP::pink300.withAlpha (0.6f));

            const float b = juce::jlimit (-1.0f, 1.0f, smoothedBalance);

            if (barStyle == BarStyle::solid)
            {
                // 实心样式：从中线向偏向的一侧连续填充
                const float halfW = (float) innerBar.getWidth() * 0.5f;
                const float len = std::abs (b) * halfW;
                const float y = (float) innerBar.getY() + 2.0f, h = (float) innerBar.getHeight() - 4.0f;
                if (b < 0.0f)      appendQuad (snap->vertices, { (float) cx - len, y, len, h }, PinkXP::pink500);
                else if (b > 0.0f) appendQuad (snap->vertices, { (float) cx, y, len, h }, PinkXP::pink600);
            }
            else
            {
                const int cellSize = 4, cellGap = 1, step = cellSize + cellGap;
                const int halfCells = juce::jmax (1, (innerBar.getWidth() / 2) / step);
                const int litCells = (int) std::round (std::abs (b) * halfCells);
                const int y = innerBar.getY() + 2;
                const int h = innerBar.getHeight() - 4;
                const auto offC = PinkXP::pink200.withAlpha (0.25f);
                for (int i = 0; i < halfCells; ++i)
                {
                    const int xL = cx - (i + 1) * step;
                    const int xR = cx + i * step + cellGap;
                    appendQuad (snap->vertices, { (float) xL, (float) y, (float) cellSize, (float) h },
                                (b < 0 && i < litCells) ? PinkXP::pink500 : offC);
                    appendQuad (snap->vertices, { (float) xR, (float) y, (float) cellSize, (float) h },
                                (b > 0 && i < litCells) ? PinkXP::pink600 : offC);
                }
            }
        }
    }

    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// Goniometer
// ----------------------------------------------------------
void PhaseModule::drawGonio(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    PinkXP::drawSunken(g, area, PinkXP::content);

    auto inner = area.reduced(6);
    if (inner.getWidth() <= 4 || inner.getHeight() <= 4) return;

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getCentreY();
    const float R  = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.48f;

    // 菱形边界（M/S 模式标准显示：↑M，↓M，←S，→S，45°就是L/R）
    juce::Path diamond;
    diamond.startNewSubPath(cx,       cy - R);
    diamond.lineTo         (cx + R,   cy);
    diamond.lineTo         (cx,       cy + R);
    diamond.lineTo         (cx - R,   cy);
    diamond.closeSubPath();
    g.setColour(PinkXP::pink300.withAlpha(0.55f));
    g.strokePath(diamond, juce::PathStrokeType(1.0f));

    // 中心十字
    g.setColour(PinkXP::pink300.withAlpha(0.5f));
    g.drawHorizontalLine((int) cy, (float)(cx - R), (float)(cx + R));
    g.drawVerticalLine  ((int) cx, (float)(cy - R), (float)(cy + R));

    // L/R 对角虚线（45°）
    g.setColour(PinkXP::pink200.withAlpha(0.6f));
    for (int d = 0; d < (int) R; d += 5)
    {
        g.fillRect((int)(cx - R + d),     (int)(cy - R + d),     2, 2);
        g.fillRect((int)(cx + R - d - 2), (int)(cy - R + d),     2, 2);
    }

    // 轴标签
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr ("M"), juce::Rectangle<int>((int) cx - 10, (int)(cy - R) - 12, 20, 12),
               juce::Justification::centred, false);
    g.drawText(y2k::tr ("S"), juce::Rectangle<int>((int)(cx + R) + 2, (int) cy - 6, 12, 12),
               juce::Justification::centredLeft, false);
    g.setColour(PinkXP::ink.withAlpha(0.55f));
    g.drawText(y2k::tr ("L"), juce::Rectangle<int>((int)(cx - R) - 12, (int)(cy - R) - 12, 12, 12),
               juce::Justification::centredRight, false);
    g.drawText(y2k::tr ("R"), juce::Rectangle<int>((int)(cx + R), (int)(cy - R) - 12, 12, 12),
               juce::Justification::centredLeft, false);

    // 样本点云：L/R → M=(L+R)/√2, S=(L-R)/√2
    const int N = juce::jmin(oscL.size(), oscR.size());
    if (N <= 1) return;

    const int targetPoints = juce::jmax(96, inner.getWidth() * 2);
    const int stride = juce::jmax(1, N / targetPoints);

    if (gonioStyle == GonioStyle::trace)
    {
        float lastX = 0.0f, lastY = 0.0f;
        bool haveLast = false;
        const auto innerF = inner.toFloat().expanded(1.0f);
        for (int i = 0; i < N; i += stride)
        {
            const float L = oscL.getUnchecked(i);
            const float Rs = oscR.getUnchecked(i);
            if (! std::isfinite(L) || ! std::isfinite(Rs)) { haveLast = false; continue; }
            const float Lc = juce::jlimit(-1.0f, 1.0f, L);
            const float Rc = juce::jlimit(-1.0f, 1.0f, Rs);
            const float px = cx + (Lc - Rc) * 0.7071067f * R;
            const float py = cy - (Lc + Rc) * 0.7071067f * R;
            if (! innerF.contains(px, py)) { haveLast = false; continue; }
            if (haveLast)
            {
                const float alpha = 0.2f + 0.6f * ((float) i / (float) juce::jmax(1, N - 1));
                g.setColour(PinkXP::pink500.withAlpha(alpha));
                g.drawLine(lastX, lastY, px, py, 1.0f);
            }
            lastX = px; lastY = py; haveLast = true;
        }
        return;
    }

    for (int i = 0; i < N; i += stride)
    {
        const float L = oscL.getUnchecked(i);
        const float Rs = oscR.getUnchecked(i);
        if (! std::isfinite(L) || ! std::isfinite(Rs)) continue;

        const float Lc = juce::jlimit(-1.0f, 1.0f, L);
        const float Rc = juce::jlimit(-1.0f, 1.0f, Rs);
        const float M  = (Lc + Rc) * 0.7071067f;
        const float S  = (Lc - Rc) * 0.7071067f;

        const float px = cx + S * R;
        const float py = cy - M * R;
        if (! inner.toFloat().expanded(1.0f).contains(px, py))
            continue;

        const float alpha = 0.2f + 0.6f * ((float) i / (float) juce::jmax(1, N - 1));
        g.setColour(PinkXP::pink500.withAlpha(alpha));
        g.fillRect(px - 0.5f, py - 0.5f, 2.0f, 2.0f);
    }
}

// ----------------------------------------------------------
// Correlation Dial —— 半圆指针仪（-1 ↔ +1）
// ----------------------------------------------------------
void PhaseModule::drawCorrDial(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    PinkXP::drawSunken(g, area, PinkXP::content);

    auto inner = area.reduced(6);
    if (inner.getWidth() <= 8 || inner.getHeight() <= 8) return;

    // ------------------------------------------------------
    // 规范化：标准的 0°~180° 上半圆仪表
    //   · 圆心 = 模块显示窗口的"正下方中点"（底边中点）
    //   · corr = -1  → 指针指向正左  (θ = 0°)
    //   · corr =  0  → 指针指向正上  (θ = 90°)
    //   · corr = +1  → 指针指向正右  (θ = 180°)
    //   · 使用数学角度系（X+ 正右，Y+ 向上）→ 屏幕 Y 翻转
    // ------------------------------------------------------
    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getBottom();                 // 圆心：底边中点
    const int   readoutReserve = 28;                            // 顶部预留给 CORR 文字和数值
    const float R  = juce::jmin((float) inner.getWidth() * 0.46f,
                                 (float) inner.getHeight() - (float) readoutReserve);

    if (R < 10.0f) return;

    // corr ∈ [-1,+1]  →  theta ∈ [0°, 180°]
    auto thetaFor = [&](float corr) -> float
    {
        const float t = juce::jlimit(-1.0f, 1.0f, corr) * 0.5f + 0.5f; // 0..1
        return t * juce::MathConstants<float>::pi; // 0..π
    };

    // 根据 theta 求半径为 r 的点（θ=0→最左，θ=π/2→最上，θ=π→最右）
    auto pointAt = [&](float theta, float r) -> juce::Point<float>
    {
        return { cx - std::cos(theta) * r, cy - std::sin(theta) * r };
    };

    // ------- 弧形轨（上半圆） -------
    //   JUCE addCentredArc：0 指向正上、顺时针递增
    //   正左 = -π/2（= 3π/2）；正右 = +π/2
    const float arcStart = -juce::MathConstants<float>::halfPi; // 正左
    const float arcEnd   =  juce::MathConstants<float>::halfPi; // 正右
    juce::Path arcTrack;
    arcTrack.addCentredArc(cx, cy, R, R, 0.0f, arcStart, arcEnd, true);
    g.setColour(PinkXP::pink300.withAlpha(0.45f));
    g.strokePath(arcTrack, juce::PathStrokeType(6.0f));
    g.setColour(PinkXP::pink200);
    g.strokePath(arcTrack, juce::PathStrokeType(2.0f));

    // ------- 刻度：-1 / -0.5 / 0 / +0.5 / +1 -------
    g.setColour(PinkXP::ink.withAlpha(0.75f));
    g.setFont(PinkXP::getAxisFont(8.0f, juce::Font::plain));
    const struct { float v; const char* s; } marks[] = {
        { -1.0f, "-1" }, { -0.5f, "-.5" }, { 0.0f, "0" },
        {  0.5f, "+.5"}, {  1.0f, "+1" }
    };
    for (auto& m : marks)
    {
        const float th = thetaFor(m.v);
        const auto  pOut = pointAt(th, R + 10.0f);
        g.drawText(y2k::tr (m.s), juce::Rectangle<int>((int) pOut.x - 12, (int) pOut.y - 6, 24, 12),
                   juce::Justification::centred, false);

        // 刻度短线
        const auto p1 = pointAt(th, R - 3.0f);
        const auto p2 = pointAt(th, R + 1.0f);
        g.setColour(PinkXP::pink400);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, 1.2f);
        g.setColour(PinkXP::ink.withAlpha(0.75f));
    }

    // ------- 指针（长度严格 < R，避免越界） -------
    const float th = thetaFor(smoothedCorr);
    const auto  tip = pointAt(th, R - 4.0f);

    // 指针阴影
    g.setColour(PinkXP::pink700.withAlpha(0.35f));
    g.drawLine(cx + 1, cy + 1, tip.x + 1, tip.y + 1, 3.0f);

    // 指针本体
    juce::Colour needleCol = (smoothedCorr < 0.0f)
                              ? PinkXP::sel       // accent：反相
                              : PinkXP::pink600;
    g.setColour(needleCol);
    g.drawLine(cx, cy, tip.x, tip.y, 2.0f);
    g.setColour(PinkXP::dark);
    g.fillEllipse(cx - 3.5f, cy - 3.5f, 7.0f, 7.0f);
    g.setColour(PinkXP::pink500);
    g.fillEllipse(cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);

    // ------- 顶部读数 -------
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    g.drawText(y2k::tr ("CORR"),
               juce::Rectangle<int>(inner.getX(), inner.getY() + 2, inner.getWidth(), 12),
               juce::Justification::centred, false);

    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    g.setColour(needleCol);
    juce::String numTxt = juce::String(smoothedCorr, 2);
    if (smoothedCorr >= 0.0f) numTxt = "+" + numTxt;
    g.drawText(y2k::tr (numTxt),
               juce::Rectangle<int>(inner.getX(), inner.getY() + 14, inner.getWidth(), 14),
               juce::Justification::centred, false);
}

// ----------------------------------------------------------
// Width bar
// ----------------------------------------------------------
void PhaseModule::drawWidthBar(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    // 左侧标签（在条形左侧的空列内居中）
    auto labelArea = area.withWidth(barLeftInset);
    auto barArea   = area.withTrimmedLeft(barLeftInset);

    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr ("WIDTH"), labelArea, juce::Justification::centred, false);

    // 凹陷背景
    PinkXP::drawSunken(g, barArea, PinkXP::content.darker(0.05f));
    auto innerBar = barArea.reduced(2);
    if (innerBar.isEmpty()) return;

    // 像素单元（cell=6, gap=1），从左到右填充 width（0..1）；实心样式则连续填充
    const int cellSize = 6;
    const int cellGap  = 1;
    const int step = cellSize + cellGap;
    const int N = juce::jmax(1, (innerBar.getWidth() + cellGap) / step);
    const int litN = juce::jlimit(0, N, (int) std::round(smoothedWidth * N));

    if (barStyle == BarStyle::solid)
    {
        const float fillW = (float) innerBar.getWidth() * juce::jlimit(0.0f, 1.0f, smoothedWidth);
        const float seg[4] = { 0.0f, 0.6f, 0.85f, 1.0f };
        const juce::Colour cols[3] = { PinkXP::pink200, PinkXP::pink500, PinkXP::sel };
        for (int s = 0; s < 3; ++s)
        {
            const float a = seg[s] * (float) innerBar.getWidth();
            const float b = juce::jmin(fillW, seg[s + 1] * (float) innerBar.getWidth());
            if (b > a)
            {
                g.setColour(cols[s]);
                g.fillRect((float) innerBar.getX() + a, (float) innerBar.getY() + 1.0f, b - a,
                           (float) innerBar.getHeight() - 2.0f);
            }
        }
    }
    else
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
        g.setColour(i < litN ? base : base.withAlpha(0.18f));
        g.fillRect(x, y, cellSize, h);
    }

    // 数值读数
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText(y2k::tr (juce::String((int) std::round(smoothedWidth * 100.0f)) + "%"),
               barArea.withTrimmedRight(4),
               juce::Justification::centredRight, false);
}

// ----------------------------------------------------------
// Balance bar（中心为 0，-1=L / +1=R）
// ----------------------------------------------------------
void PhaseModule::drawBalanceBar(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    // 左侧标签（在条形左侧的空列内居中）
    auto labelArea = area.withWidth(barLeftInset);
    auto barArea   = area.withTrimmedLeft(barLeftInset);

    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr ("BAL"), labelArea, juce::Justification::centred, false);

    PinkXP::drawSunken(g, barArea, PinkXP::content.darker(0.05f));
    auto innerBar = barArea.reduced(2);
    if (innerBar.isEmpty()) return;

    // 中线
    const int cx = innerBar.getCentreX();
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    g.drawVerticalLine(cx, (float) innerBar.getY(), (float) innerBar.getBottom());

    const float b = juce::jlimit(-1.0f, 1.0f, smoothedBalance);

    if (barStyle == BarStyle::solid)
    {
        const float len = std::abs(b) * (float) innerBar.getWidth() * 0.5f;
        const float y = (float) innerBar.getY() + 2.0f, h = (float) innerBar.getHeight() - 4.0f;
        if (b < 0.0f)
        {
            g.setColour(PinkXP::pink500);
            g.fillRect((float) cx - len, y, len, h);
        }
        else if (b > 0.0f)
        {
            g.setColour(PinkXP::pink600);
            g.fillRect((float) cx, y, len, h);
        }
    }

    // 像素指示块：从中心向偏移方向填充
    const int cellSize = 4;
    const int cellGap  = 1;
    const int step = cellSize + cellGap;
    const int halfCells = juce::jmax(1, (innerBar.getWidth() / 2) / step);
    const int litCells = (int) std::round(std::abs(b) * halfCells);

    for (int i = 0; i < halfCells && barStyle == BarStyle::segmented; ++i)
    {
        const int xL = cx - (i + 1) * step;
        const int xR = cx + i * step + cellGap;
        const int y = innerBar.getY() + 2;
        const int h = innerBar.getHeight() - 4;

        const juce::Colour colL = (b < 0 && i < litCells)
                                  ? PinkXP::pink500
                                  : PinkXP::pink200.withAlpha(0.25f);
        const juce::Colour colR = (b > 0 && i < litCells)
                                  ? PinkXP::pink600
                                  : PinkXP::pink200.withAlpha(0.25f);
        g.setColour(colL);
        g.fillRect(xL, y, cellSize, h);
        g.setColour(colR);
        g.fillRect(xR, y, cellSize, h);
    }

    // 数值：L30 / CENTER / R40
    juce::String txt;
    if (std::abs(b) < 0.02f)       txt = "C";
    else if (b < 0)                txt = "L" + juce::String((int) std::round(-b * 100.0f));
    else                           txt = "R" + juce::String((int) std::round( b * 100.0f));

    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText(y2k::tr (txt), barArea.withTrimmedRight(4),
               juce::Justification::centredRight, false);
}

void PhaseModule::rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds)
{
    if (contentBounds.isEmpty())
    {
        staticLayer = juce::Image();
        staticLayerContentBounds = {};
        return;
    }

    if (staticLayer.isValid()
        && staticLayerContentBounds == contentBounds
        && staticLayer.getWidth() == contentBounds.getWidth()
        && staticLayer.getHeight() == contentBounds.getHeight())
    {
        return;
    }

    staticLayer = juce::Image(juce::Image::ARGB,
                              contentBounds.getWidth(),
                              contentBounds.getHeight(),
                              true);
    staticLayerContentBounds = contentBounds;

    juce::Graphics sg(staticLayer);
    sg.setColour(PinkXP::btnFace);
    sg.fillRect(staticLayer.getBounds());

    auto localContent = juce::Rectangle<int>(0, 0, contentBounds.getWidth(), contentBounds.getHeight());
    const auto staticAreas = computeAreas (localContent);
    const auto staticAreaGonio = staticAreas.gonio;
    const auto staticAreaDial  = staticAreas.dial;
    const auto staticAreaWidth = staticAreas.width;
    const auto staticAreaBalance = staticAreas.balance;

    if (! staticAreaGonio.isEmpty())
        PinkXP::drawSunken(sg, staticAreaGonio, PinkXP::content);

    auto inner = staticAreaGonio.reduced(6);
    if (! staticAreaGonio.isEmpty() && inner.getWidth() > 4 && inner.getHeight() > 4)
    {
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getCentreY();
        const float R  = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.48f;

        juce::Path diamond;
        diamond.startNewSubPath(cx,       cy - R);
        diamond.lineTo         (cx + R,   cy);
        diamond.lineTo         (cx,       cy + R);
        diamond.lineTo         (cx - R,   cy);
        diamond.closeSubPath();
        sg.setColour(PinkXP::pink300.withAlpha(0.55f));
        sg.strokePath(diamond, juce::PathStrokeType(1.0f));

        sg.setColour(PinkXP::pink300.withAlpha(0.5f));
        sg.drawHorizontalLine((int) cy, (float)(cx - R), (float)(cx + R));
        sg.drawVerticalLine  ((int) cx, (float)(cy - R), (float)(cy + R));

        sg.setColour(PinkXP::pink200.withAlpha(0.6f));
        for (int d = 0; d < (int) R; d += 5)
        {
            sg.fillRect((int)(cx - R + d),     (int)(cy - R + d),     2, 2);
            sg.fillRect((int)(cx + R - d - 2), (int)(cy - R + d),     2, 2);
        }

        sg.setColour(PinkXP::ink.withAlpha(0.85f));
        sg.setFont(PinkXP::getAxisFont(9.0f, juce::Font::bold));
        sg.drawText(y2k::tr ("M"), juce::Rectangle<int>((int) cx - 10, (int)(cy - R) - 12, 20, 12),
                    juce::Justification::centred, false);
        sg.drawText(y2k::tr ("S"), juce::Rectangle<int>((int)(cx + R) + 2, (int) cy - 6, 12, 12),
                    juce::Justification::centredLeft, false);
        sg.setColour(PinkXP::ink.withAlpha(0.55f));
        sg.drawText(y2k::tr ("L"), juce::Rectangle<int>((int)(cx - R) - 12, (int)(cy - R) - 12, 12, 12),
                    juce::Justification::centredRight, false);
        sg.drawText(y2k::tr ("R"), juce::Rectangle<int>((int)(cx + R), (int)(cy - R) - 12, 12, 12),
                    juce::Justification::centredLeft, false);
    }

    if (! staticAreaDial.isEmpty())
        PinkXP::drawSunken(sg, staticAreaDial, PinkXP::content);
    if (! staticAreaWidth.isEmpty())
        PinkXP::drawSunken(sg, staticAreaWidth.withTrimmedLeft(barLeftInset), PinkXP::content.darker(0.05f));
    if (! staticAreaBalance.isEmpty())
        PinkXP::drawSunken(sg, staticAreaBalance.withTrimmedLeft(barLeftInset), PinkXP::content.darker(0.05f));
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
}