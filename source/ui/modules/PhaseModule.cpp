#include "source/ui/modules/PhaseModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"

// ==========================================================
// PhaseModule
// ==========================================================

PhaseModule::PhaseModule(AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::phase),
      hub(h)
{
    // Phase F：本模块同时需要 Oscilloscope（Lissajous 点云）和 Phase（相关/宽度/平衡）
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.retain(AnalyserHub::Kind::Phase);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(320, 192); // phase 5×3 大格

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
    hub.release(AnalyserHub::Kind::Phase);
    hub.release(AnalyserHub::Kind::Oscilloscope);
}

// ----------------------------------------------------------
// onFrame
// ----------------------------------------------------------
void PhaseModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;

    // Oscilloscope 点云数据（从聚合帧内的 oscL/oscR 直接 memcpy 入 juce::Array）
    if (frame.has (AnalyserHub::Kind::Oscilloscope))
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
    repaint();
}

// ----------------------------------------------------------
// layoutContent
// ----------------------------------------------------------
void PhaseModule::layoutContent(juce::Rectangle<int> content)
{
    const int barH    = 20;
    const int barGap  = 2;
    const int footer  = barH * 2 + barGap;

    auto upper = content.withTrimmedBottom(footer + 6);
    auto lower = content.withTop(upper.getBottom() + 6);

    const int gonioSize = juce::jmin(upper.getWidth() * 5 / 9, upper.getHeight());
    areaGonio = upper.withWidth(gonioSize);
    areaDial  = upper.withTrimmedLeft(gonioSize + 8);

    areaWidth   = lower.withHeight(barH);
    areaBalance = lower.withTrimmedTop(barH + barGap).withHeight(barH);

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
    const int barH    = 20;
    const int barGap  = 2;
    const int footer  = barH * 2 + barGap;
    auto upper = canvas.withTrimmedBottom (footer + 6);
    auto lower = canvas.withTop (upper.getBottom() + 6);
    const int gonioSize = juce::jmin (upper.getWidth() * 5 / 9, upper.getHeight());
    const auto areaGonio = upper.withWidth (gonioSize);
    const auto areaDial  = upper.withTrimmedLeft (gonioSize + 8);
    const auto areaWidth = lower.withHeight (barH);
    const auto areaBalance = lower.withTrimmedTop (barH + barGap).withHeight (barH);

    // ---- Gonio 轴标签 ----
    {
        auto inner = areaGonio.reduced (6);
        const float cx = (float) inner.getCentreX();
        const float cy = (float) inner.getCentreY();
        const float R  = (float) juce::jmin (inner.getWidth(), inner.getHeight()) * 0.48f;
        g.setColour (PinkXP::ink.withAlpha (0.85f));
        g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::bold));
        g.drawText ("M", juce::Rectangle<int> ((int) cx - 10, (int) (cy - R) - 12, 20, 12),
                    juce::Justification::centred, false);
        g.drawText ("S", juce::Rectangle<int> ((int) (cx + R) + 2, (int) cy - 6, 12, 12),
                    juce::Justification::centredLeft, false);
        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.drawText ("L", juce::Rectangle<int> ((int) (cx - R) - 12, (int) (cy - R) - 12, 12, 12),
                    juce::Justification::centredRight, false);
        g.drawText ("R", juce::Rectangle<int> ((int) (cx + R), (int) (cy - R) - 12, 12, 12),
                    juce::Justification::centredLeft, false);
    }

    // ---- Dial 刻度 + CORR 读数 ----
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
            g.setColour (PinkXP::ink.withAlpha (0.75f));
            g.setFont (PinkXP::getAxisFont (8.0f, juce::Font::plain));
            for (auto& m : marks)
            {
                const float th = thetaFor (m.v);
                const float px = cx - std::cos (th) * (R + 10.0f);
                const float py = cy - std::sin (th) * (R + 10.0f);
                g.drawText (m.s, juce::Rectangle<int> ((int) px - 12, (int) py - 6, 24, 12),
                            juce::Justification::centred, false);
            }

            const juce::Colour needleCol = (smoothedCorr < 0.0f) ? PinkXP::sel : PinkXP::pink600;
            g.setColour (PinkXP::ink);
            g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
            g.drawText ("CORR",
                        juce::Rectangle<int> (inner.getX(), inner.getY() + 2, inner.getWidth(), 12),
                        juce::Justification::centred, false);
            g.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
            g.setColour (needleCol);
            juce::String numTxt = juce::String (smoothedCorr, 2);
            if (smoothedCorr >= 0.0f) numTxt = "+" + numTxt;
            g.drawText (numTxt,
                        juce::Rectangle<int> (inner.getX(), inner.getY() + 14, inner.getWidth(), 14),
                        juce::Justification::centred, false);
        }
    }

    // ---- Width / Balance 标签与数值 ----
    {
        const int labelW = 56;
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText ("WIDTH", areaWidth.withWidth (labelW), juce::Justification::centredLeft, false);
        g.drawText ("BAL", areaBalance.withWidth (labelW), juce::Justification::centredLeft, false);

        g.setFont (PinkXP::getFont (9.0f, juce::Font::plain));
        g.drawText (juce::String ((int) std::round (smoothedWidth * 100.0f)) + "%",
                    areaWidth.withTrimmedLeft (labelW + 4).withTrimmedRight (4),
                    juce::Justification::centredRight, false);

        const float b = juce::jlimit (-1.0f, 1.0f, smoothedBalance);
        juce::String txt;
        if (std::abs (b) < 0.02f)      txt = "C";
        else if (b < 0)                txt = "L" + juce::String ((int) std::round (-b * 100.0f));
        else                           txt = "R" + juce::String ((int) std::round ( b * 100.0f));
        g.drawText (txt,
                    areaBalance.withTrimmedLeft (labelW + 4).withTrimmedRight (4),
                    juce::Justification::centredRight, false);
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
    const int barH    = 20;
    const int barGap  = 2;
    const int footer  = barH * 2 + barGap;
    auto upper = content.withTrimmedBottom (footer + 6);
    auto lower = content.withTop (upper.getBottom() + 6);
    const int gonioSize = juce::jmin (upper.getWidth() * 5 / 9, upper.getHeight());
    const auto areaGonio = upper.withWidth (gonioSize);
    const auto areaDial  = upper.withTrimmedLeft (gonioSize + 8);
    const auto areaWidth = lower.withHeight (barH);
    const auto areaBalance = lower.withTrimmedTop (barH + barGap).withHeight (barH);

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
    {
        const int labelW = 56;
        auto barArea = areaWidth.withTrimmedLeft (labelW + 4);
        pushSunken (barArea, PinkXP::content.darker (0.05f));
        auto innerBar = barArea.reduced (2);
        if (! innerBar.isEmpty())
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
    {
        const int labelW = 56;
        auto barArea = areaBalance.withTrimmedLeft (labelW + 4);
        pushSunken (barArea, PinkXP::content.darker (0.05f));
        auto innerBar = barArea.reduced (2);
        if (! innerBar.isEmpty())
        {
            const int cx = innerBar.getCentreX();
            appendLineSegment (snap->vertices,
                               (float) cx, (float) innerBar.getY(),
                               (float) cx, (float) innerBar.getBottom(),
                               0.5f, PinkXP::pink300.withAlpha (0.6f));

            const int cellSize = 4, cellGap = 1, step = cellSize + cellGap;
            const int halfCells = juce::jmax (1, (innerBar.getWidth() / 2) / step);
            const float b = juce::jlimit (-1.0f, 1.0f, smoothedBalance);
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
    g.drawText("M", juce::Rectangle<int>((int) cx - 10, (int)(cy - R) - 12, 20, 12),
               juce::Justification::centred, false);
    g.drawText("S", juce::Rectangle<int>((int)(cx + R) + 2, (int) cy - 6, 12, 12),
               juce::Justification::centredLeft, false);
    g.setColour(PinkXP::ink.withAlpha(0.55f));
    g.drawText("L", juce::Rectangle<int>((int)(cx - R) - 12, (int)(cy - R) - 12, 12, 12),
               juce::Justification::centredRight, false);
    g.drawText("R", juce::Rectangle<int>((int)(cx + R), (int)(cy - R) - 12, 12, 12),
               juce::Justification::centredLeft, false);

    // 样本点云：L/R → M=(L+R)/√2, S=(L-R)/√2
    const int N = juce::jmin(oscL.size(), oscR.size());
    if (N <= 1) return;

    const int targetPoints = juce::jmax(96, inner.getWidth() * 2);
    const int stride = juce::jmax(1, N / targetPoints);

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
        g.drawText(m.s, juce::Rectangle<int>((int) pOut.x - 12, (int) pOut.y - 6, 24, 12),
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
    g.drawText("CORR",
               juce::Rectangle<int>(inner.getX(), inner.getY() + 2, inner.getWidth(), 12),
               juce::Justification::centred, false);

    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    g.setColour(needleCol);
    juce::String numTxt = juce::String(smoothedCorr, 2);
    if (smoothedCorr >= 0.0f) numTxt = "+" + numTxt;
    g.drawText(numTxt,
               juce::Rectangle<int>(inner.getX(), inner.getY() + 14, inner.getWidth(), 14),
               juce::Justification::centred, false);
}

// ----------------------------------------------------------
// Width bar
// ----------------------------------------------------------
void PhaseModule::drawWidthBar(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    // 左侧标签
    const int labelW = 56;
    auto labelArea = area.withWidth(labelW);
    auto barArea   = area.withTrimmedLeft(labelW + 4);

    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText("WIDTH", labelArea, juce::Justification::centredLeft, false);

    // 凹陷背景
    PinkXP::drawSunken(g, barArea, PinkXP::content.darker(0.05f));
    auto innerBar = barArea.reduced(2);
    if (innerBar.isEmpty()) return;

    // 像素单元（cell=6, gap=1），从左到右填充 width（0..1）
    const int cellSize = 6;
    const int cellGap  = 1;
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
        g.setColour(i < litN ? base : base.withAlpha(0.18f));
        g.fillRect(x, y, cellSize, h);
    }

    // 数值读数
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText(juce::String((int) std::round(smoothedWidth * 100.0f)) + "%",
               barArea.withTrimmedRight(4),
               juce::Justification::centredRight, false);
}

// ----------------------------------------------------------
// Balance bar（中心为 0，-1=L / +1=R）
// ----------------------------------------------------------
void PhaseModule::drawBalanceBar(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    const int labelW = 56;
    auto labelArea = area.withWidth(labelW);
    auto barArea   = area.withTrimmedLeft(labelW + 4);

    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText("BAL", labelArea, juce::Justification::centredLeft, false);

    PinkXP::drawSunken(g, barArea, PinkXP::content.darker(0.05f));
    auto innerBar = barArea.reduced(2);
    if (innerBar.isEmpty()) return;

    // 中线
    const int cx = innerBar.getCentreX();
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    g.drawVerticalLine(cx, (float) innerBar.getY(), (float) innerBar.getBottom());

    // 像素指示块：从中心向偏移方向填充
    const int cellSize = 4;
    const int cellGap  = 1;
    const int step = cellSize + cellGap;
    const int halfCells = juce::jmax(1, (innerBar.getWidth() / 2) / step);
    const float b = juce::jlimit(-1.0f, 1.0f, smoothedBalance);
    const int litCells = (int) std::round(std::abs(b) * halfCells);

    for (int i = 0; i < halfCells; ++i)
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
    g.drawText(txt, barArea.withTrimmedRight(4),
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
    const int barH    = 20;
    const int barGap  = 2;
    const int footer  = barH * 2 + barGap;

    auto upper = localContent.withTrimmedBottom(footer + 6);
    auto lower = localContent.withTop(upper.getBottom() + 6);

    const int gonioSize = juce::jmin(upper.getWidth() * 5 / 9, upper.getHeight());
    const auto staticAreaGonio = upper.withWidth(gonioSize);
    const auto staticAreaDial  = upper.withTrimmedLeft(gonioSize + 8);
    const auto staticAreaWidth = lower.withHeight(barH);
    const auto staticAreaBalance = lower.withTrimmedTop(barH + barGap).withHeight(barH);

    PinkXP::drawSunken(sg, staticAreaGonio, PinkXP::content);

    auto inner = staticAreaGonio.reduced(6);
    if (inner.getWidth() > 4 && inner.getHeight() > 4)
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
        sg.drawText("M", juce::Rectangle<int>((int) cx - 10, (int)(cy - R) - 12, 20, 12),
                    juce::Justification::centred, false);
        sg.drawText("S", juce::Rectangle<int>((int)(cx + R) + 2, (int) cy - 6, 12, 12),
                    juce::Justification::centredLeft, false);
        sg.setColour(PinkXP::ink.withAlpha(0.55f));
        sg.drawText("L", juce::Rectangle<int>((int)(cx - R) - 12, (int)(cy - R) - 12, 12, 12),
                    juce::Justification::centredRight, false);
        sg.drawText("R", juce::Rectangle<int>((int)(cx + R), (int)(cy - R) - 12, 12, 12),
                    juce::Justification::centredLeft, false);
    }

    PinkXP::drawSunken(sg, staticAreaDial, PinkXP::content);
    PinkXP::drawSunken(sg, staticAreaWidth.withTrimmedLeft(60), PinkXP::content.darker(0.05f));
    PinkXP::drawSunken(sg, staticAreaBalance.withTrimmedLeft(60), PinkXP::content.darker(0.05f));
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