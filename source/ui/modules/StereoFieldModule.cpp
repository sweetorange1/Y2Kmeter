#include "source/ui/modules/StereoFieldModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <cstring>

// ==========================================================
// StereoFieldModule —— Pink XP 像素风"声相指示"（半圆雷达）
// ==========================================================

StereoFieldModule::StereoFieldModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::stereoField), hub (h)
{
    hub.retain (AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener (this);

    setMinSize     (64, 64);
    setDefaultSize (320, 220);
    setTitleText   ("Stereo Field");

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        // 点颜色跟随主题 → 切换主题时清空残影并重绘
        invalidateTrail();
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

    // v2.9.5：GPU trail 累积 —— 衰减在 GPU 每帧执行（decay 按累计 dt 换算，
    //   残影时长与帧率无关）；点云每帧画进 trail FBO（GPU 光栅化，不再需要
    //   旧版"隔帧 + CPU 全图 multiplyAlpha"）。
    trailDtAccumSec = juce::jmin (0.5f, trailDtAccumSec + frame.dtSec);

    // 浮动窗口没有 Editor 的 GL 渲染通道（wantsGlSnapshot=false），绘制回退
    //   legacy CPU 路径 —— legacy 的 trail 只在 trailNeedsUpdate 置位时重建，
    //   而 GL 接管期间该标志从未置位，脱离后 trailImage 停在旧图（画面定格）。
    //   浮动期间每帧强制重建 CPU trail，恢复动态。
    if (isFloating())
        trailNeedsUpdate = true;

    buildGlSnapshot();

    // v2.9：去掉 15ms×DPI 硬编码节流，每个分发帧重绘
    repaint();
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
    // 接管时只保留文字：能量 dB 标签 + L/C/R 声像方位标签
    auto plot = canvas.reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    const auto geo = computeRadarGeometry (plot);
    const int cx = geo.cx, baseY = geo.baseY, R = geo.radius;
    const float labelFontSize = geo.labelFontSize;

    g.setColour (PinkXP::ink.withAlpha (0.55f));
    g.setFont (PinkXP::getAxisFont (juce::jmax (6.0f, labelFontSize - 1.5f), juce::Font::plain));
    const float frac[4]   = { 0.25f, 0.50f, 0.75f, 1.00f };
    const char* labels[4] = { "-12", "-6", "-2.5", "0 dB" };
    for (int ri = 0; ri < 4; ++ri)
    {
        const float rr = (float) R * frac[ri];
        if (rr < 8.0f || rr > (float) R) continue;
        g.drawText (labels[ri], cx + 4, baseY - (int) rr - 6, 26, 12,
                    juce::Justification::centredLeft, false);
    }

    const int actualMargin = plot.getWidth() / 2 - R;
    const int labelH = juce::jmax (10, (int) std::ceil (labelFontSize) + 4);
    const int y = baseY - labelH;
    g.setColour (PinkXP::ink.withAlpha (0.7f));
    g.setFont (PinkXP::getAxisFont (labelFontSize, juce::Font::bold));
    g.drawText ("C", cx - 8, y, 16, labelH, juce::Justification::centred, false);
    g.setFont (PinkXP::getAxisFont (juce::jmin (labelFontSize, 11.0f), juce::Font::bold));
    g.drawText ("L", plot.getX(), y, actualMargin, labelH, juce::Justification::centredRight, false);
    g.drawText ("R", plot.getRight() - actualMargin, y, actualMargin, labelH, juce::Justification::centredLeft, false);
}

void StereoFieldModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    auto canvas = contentBounds.reduced (2);
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    drawBackground (g, canvas);

    if (trailNeedsUpdate)
    {
        updateTrail (canvas);
        trailNeedsUpdate = false;
    }

    if (trailImage.isValid())
    {
        g.drawImage (trailImage,
                     canvas.getX(), canvas.getY(), canvas.getWidth(), canvas.getHeight(),
                     0, 0, trailImage.getWidth(), trailImage.getHeight());
    }
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

    auto snap = beginGlSnapshot();
    snap->canvasLocal = canvas;

    // ---- trail 通道参数 ----
    snap->trailFbo = true;
    snap->trailDecay = std::pow (kDecayPerFrame, juce::jmax (0.0f, trailDtAccumSec) * 60.0f);
    trailDtAccumSec = 0.0f;
    snap->texW = canvas.getWidth();
    snap->texH = canvas.getHeight();
    snap->texTargetRect = canvas;

    constexpr float kPi = juce::MathConstants<float>::pi;
    auto plot = canvas.reduced (6);

    // ---- 背景段（segments[0]）：凹陷（框架 helper）+ 雷达几何 ----
    {
        appendSunkenFrame (snap->vertices, canvas);

        if (plot.getWidth() > 8 && plot.getHeight() > 8)
        {
            const auto geo = computeRadarGeometry (plot);
            const float cx = (float) geo.cx, baseY = (float) geo.baseY, R = (float) geo.radius;
            const float r45 = R * 0.70710678f;

            // 半圆外轮廓弧（64 段，半宽 0.6）
            const auto arcC = PinkXP::pink300.withAlpha (0.65f);
            constexpr int arcSegs = 64;
            for (int s = 0; s < arcSegs; ++s)
            {
                const float th0 = -kPi * 0.5f + kPi * (float) s / (float) arcSegs;
                const float th1 = -kPi * 0.5f + kPi * (float) (s + 1) / (float) arcSegs;
                // addCentredArc：0=顶部、顺时针；屏幕坐标 x = cx + R·sin(th), y = baseY - R·cos(th)
                const float x0 = cx + R * std::sin (th0), y0 = baseY - R * std::cos (th0);
                const float x1 = cx + R * std::sin (th1), y1 = baseY - R * std::cos (th1);
                appendLineSegment (snap->vertices, x0, y0, x1, y1, 0.6f, arcC);
            }

            // 直径线
            appendLineSegment (snap->vertices, cx - R, baseY, cx + R, baseY, 0.5f,
                               PinkXP::pink300.withAlpha (0.5f));

            // 45° 参考线 ×2 + 顶部中线
            const auto refC = PinkXP::pink200.withAlpha (0.45f);
            appendLineSegment (snap->vertices, cx, baseY, cx - r45, baseY - r45, 0.5f, refC);
            appendLineSegment (snap->vertices, cx, baseY, cx + r45, baseY - r45, 0.5f, refC);
            appendLineSegment (snap->vertices, cx, baseY, cx, baseY - R, 0.5f, refC);

            // 能量环（4 个半圆，半宽 0.5）
            const auto ringC = PinkXP::pink200.withAlpha (0.30f);
            const float frac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = R * frac[ri];
                if (rr < 2.0f || rr > R) continue;
                for (int s = 0; s < arcSegs; ++s)
                {
                    const float th0 = -kPi * 0.5f + kPi * (float) s / (float) arcSegs;
                    const float th1 = -kPi * 0.5f + kPi * (float) (s + 1) / (float) arcSegs;
                    const float x0 = cx + rr * std::sin (th0), y0 = baseY - rr * std::cos (th0);
                    const float x1 = cx + rr * std::sin (th1), y1 = baseY - rr * std::cos (th1);
                    appendLineSegment (snap->vertices, x0, y0, x1, y1, 0.5f, ringC);
                }
            }
        }
    }
    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    snap->trailSegStart = (int) snap->segments.size();

    // ---- trail 点段（segments[1..]）：当前帧新点 ----
    {
        const int N = juce::jmin (snapshotL.size(), snapshotR.size());
        if (N > 1 && plot.getWidth() > 8 && plot.getHeight() > 8)
        {
            const auto geo = computeRadarGeometry (plot);
            const float cx = (float) geo.cx, baseY = (float) geo.baseY, R = (float) geo.radius;

            constexpr int kMaxPoints = 512;
            const int stride = juce::jmax (1, N / kMaxPoints);
            const juce::Rectangle<float> plotF = plot.toFloat();
            const auto dotC = PinkXP::pink500.withAlpha (0.9f);
            const float dr = dotC.getFloatRed(), dg = dotC.getFloatGreen(),
                        db = dotC.getFloatBlue(), da = dotC.getFloatAlpha();

            const int trailStartIdx = (int) snap->vertices.size() / 6;
            for (int i = 0; i < N; i += stride)
            {
                const float rawL = snapshotL.getUnchecked (i);
                const float rawR = snapshotR.getUnchecked (i);
                if (! std::isfinite (rawL) || ! std::isfinite (rawR)) continue;
                const float l = std::abs (rawL);
                const float r = std::abs (rawR);
                const float sum = l + r;
                if (sum < 1.0e-4f) continue;
                const float balance = (r - l) / sum;
                const float peak = juce::jmax (l, r);
                const float rho = peak * R;
                const float ang = balance * (kPi * 0.5f);
                const float px = cx + rho * std::sin (ang);
                const float py = baseY - rho * std::cos (ang);
                if (! plotF.contains (px, py)) continue;
                const float q[36] = {
                    px - 1.0f, py - 1.0f, dr, dg, db, da,
                    px + 1.0f, py - 1.0f, dr, dg, db, da,
                    px - 1.0f, py + 1.0f, dr, dg, db, da,
                    px + 1.0f, py - 1.0f, dr, dg, db, da,
                    px + 1.0f, py + 1.0f, dr, dg, db, da,
                    px - 1.0f, py + 1.0f, dr, dg, db, da,
                };
                snap->vertices.insert (snap->vertices.end(), q, q + 36);
            }
            const int count = (int) snap->vertices.size() / 6 - trailStartIdx;
            if (count > 0)
                snap->segments.push_back ({ trailStartIdx, count, Primitive::Triangles });
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
    ctx.addModuleActor (makeSonarRingActor (hit.pos));
    ctx.addModuleActor (makeInvaderActor (hit.pos, hit.rect));

    // 声像角反算（半圆雷达：角度由点击位置决定，E.4.3）
    const float cx = hit.rect.getCentreX();
    const float baseY = hit.rect.getBottom();
    const float dx = hit.pos.x - cx;
    const float dy = baseY - hit.pos.y;
    const float angDeg = std::atan2 (dx, dy) * 180.0f / 3.14159f;
    const int adeg = juce::roundToInt (std::abs (angDeg));

    // 反馈 A1：文案合理化——接近正上方显示 CENTER，其余只写 "PAN L32"
    juce::String text = (adeg < 2)
        ? "PAN CENTER"
        : "PAN " + juce::String ((angDeg < 0.0f) ? "L" : "R") + juce::String (adeg);
    ctx.bubble (hit.pos.translated (0.0f, -22.0f), text, ToyRole::Reward);
}

// ----------------------------------------------------------
// 绘制半圆底衬
// ----------------------------------------------------------
void StereoFieldModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);

    auto plot = canvas.reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    const auto geo = computeRadarGeometry (plot);
    const int  cx            = geo.cx;
    const int  baseY         = geo.baseY;
    const int  R             = geo.radius;
    const float labelFontSize = geo.labelFontSize;

    constexpr float kPi = juce::MathConstants<float>::pi;
    const float r45 = (float) R * 0.70710678f;  // 45° 处的弦长分量

    // ---- 方位参考（固定半径 R，不随 autoGain 缩放）----

    // 半圆外轮廓（更亮） + 直径线
    {
        juce::Path arc;
        arc.addCentredArc ((float) cx, (float) baseY, (float) R, (float) R, 0.0f,
                           -kPi * 0.5f, kPi * 0.5f, true);
        g.setColour (PinkXP::pink300.withAlpha (0.65f));
        g.strokePath (arc, juce::PathStrokeType (1.2f));
        g.setColour (PinkXP::pink300.withAlpha (0.5f));
        g.drawLine ((float) (cx - R), (float) baseY, (float) (cx + R), (float) baseY, 1.0f);
    }

    // 45° 参考线（左 / 右）与顶部中线
    {
        g.setColour (PinkXP::pink200.withAlpha (0.45f));
        g.drawLine ((float) cx, (float) baseY, (float) cx - r45, (float) baseY - r45, 1.0f);
        g.drawLine ((float) cx, (float) baseY, (float) cx + r45, (float) baseY - r45, 1.0f);
        g.drawLine ((float) cx, (float) baseY, (float) cx,        (float) baseY - R,   1.0f);
    }

    // ---- 能量刻度（固定比例尺）----
    //   effR = R 即满幅 0 dB 对应的半径；能量环 + dB 标签固定在外轮廓内，
    //   不随信号强度做实时缩放。
    {
        const float effR      = (float) R;
        const float frac[4]   = { 0.25f, 0.50f, 0.75f, 1.00f };
        const char* labels[4] = { "-12", "-6", "-2.5", "0 dB" };

        g.setColour (PinkXP::pink200.withAlpha (0.30f));
        for (int ri = 0; ri < 4; ++ri)
        {
            const float rr = effR * frac[ri];
            if (rr < 2.0f || rr > (float) R) continue;
            juce::Path arc;
            // JUCE 角度：0 = 顶部中心，顺时针为正；
            // 上半圆 = 从左(-π/2) 顺时针经顶部(0) 到右(π/2)。
            // startAsNewSubPath=true：从圆弧左端点开启新子路径。
            arc.addCentredArc ((float) cx, (float) baseY, rr, rr, 0.0f,
                               -kPi * 0.5f, kPi * 0.5f, true);
            g.strokePath (arc, juce::PathStrokeType (1.0f));
        }

        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.setFont (PinkXP::getAxisFont (juce::jmax (6.0f, labelFontSize - 1.5f),
                                        juce::Font::plain));
        for (int ri = 0; ri < 4; ++ri)
        {
            const float rr = effR * frac[ri];
            if (rr < 8.0f || rr > (float) R) continue;
            const int ly = baseY - (int) rr - 6;
            g.drawText (labels[ri], cx + 4, ly, 26, 12,
                        juce::Justification::centredLeft, false);
        }
    }

    // ---- 声像方位刻度：L / C / R（固定，动态字号）----
    {
        const int actualMargin = plot.getWidth() / 2 - R;  // 实际预留的每侧宽度（≥0）
        const int labelH = juce::jmax (10, (int) std::ceil (labelFontSize) + 4);
        const int y      = baseY - labelH;

        g.setColour (PinkXP::ink.withAlpha (0.7f));
        g.setFont (PinkXP::getAxisFont (labelFontSize, juce::Font::bold));
        g.drawText ("C", cx - 8, y, 16, labelH,
                    juce::Justification::centred, false);

        // L/R 标签位于 10px 侧边间隙内，字号单独收紧，确保单字符完整显示
        g.setFont (PinkXP::getAxisFont (juce::jmin (labelFontSize, 11.0f),
                                        juce::Font::bold));
        g.drawText ("L", plot.getX(), y, actualMargin, labelH,
                    juce::Justification::centredRight, false);
        g.drawText ("R", plot.getRight() - actualMargin, y, actualMargin, labelH,
                    juce::Justification::centredLeft, false);
    }
}

// ----------------------------------------------------------
// updateTrail —— 衰减旧点 + 绘制新点
//
//   trailImage 为 ARGB（预乘 alpha）离屏图，尺寸按画布对角线动态降采样。
//   每帧先对所有像素 multiplyAlpha(kDecayPerFrame) 实现渐隐，再用
//   AffineTransform 把新样本点画到图上（点用主题色，source-over 叠加）。
// ----------------------------------------------------------
void StereoFieldModule::updateTrail (juce::Rectangle<int> canvas)
{
    const int cw = canvas.getWidth();
    const int ch = canvas.getHeight();
    if (cw <= 0 || ch <= 0) return;

    // 动态分辨率：对角线 ≤ 700px 时 1:1，超出反比降采样（下限 25%）。
    //   相比旧的 900px/35%，大窗口下离屏图更小，每帧 multiplyAlpha 遍历的
    //   像素量大幅下降，缓解拖拽/脱离模式下窗口拉大后的卡顿。
    const float diag  = std::sqrt ((float) (cw * cw + ch * ch));
    const float scale = juce::jlimit (0.25f, 1.0f, 700.0f / juce::jmax (700.0f, diag));
    const int   rw    = juce::jmax (8, (int) std::lround ((float) cw * scale));
    const int   rh    = juce::jmax (8, (int) std::lround ((float) ch * scale));

    if (trailImage.isNull()
        || trailImage.getWidth()  != rw
        || trailImage.getHeight() != rh)
    {
        trailImage = juce::Image (juce::Image::ARGB, rw, rh, true);
        trailImage.clear (trailImage.getBounds());
    }

    // 1) 衰减旧点：所有像素 alpha *= decay（跳过透明像素加速）
    //    v2.9：decay 由累计 dt 换算（60Hz 标定），残影时长与帧率无关
    const float decay = std::pow (kDecayPerFrame, juce::jmax (0.0f, trailDtAccumSec) * 60.0f);
    trailDtAccumSec = 0.0f;
    {
        juce::Image::BitmapData bd (trailImage, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < bd.height; ++y)
        {
            auto* line = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer (y));
            for (int x = 0; x < bd.width; ++x)
            {
                auto& p = line[x];
                if (p.getAlpha() == 0) continue;
                p.multiplyAlpha (decay);
            }
        }
    }

    // 2) 绘制新点（canvas 局部坐标系，经 scale 变换映射到 trailImage）
    juce::Graphics tg (trailImage);
    tg.addTransform (juce::AffineTransform::scale (scale));

    auto plot = juce::Rectangle<int> (0, 0, cw, ch).reduced (6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    constexpr float kPi = juce::MathConstants<float>::pi;
    const auto geo   = computeRadarGeometry (plot);
    const int  cx    = geo.cx;
    const int  baseY = geo.baseY;
    const int  R     = geo.radius;
    const float effR = (float) R;

    const int N = juce::jmin (snapshotL.size(), snapshotR.size());
    if (N <= 1) return;

    // 固定点数量上限：无论画布多大，每帧最多绘制 kMaxPoints 个点，
    //   避免模块拉大后每帧绘制上千个点（旧的 targetPoints = cw*2 随宽度
    //   线性增长）导致拖拽/脱离模式下严重卡顿。
    constexpr int kMaxPoints = 512;
    const int stride = juce::jmax (1, N / kMaxPoints);

    const juce::Rectangle<float> plotF = plot.toFloat();

    // v2.9.2：所有点同色同 alpha —— 收集进 RectangleList 后一次 fillRectList 提交，
    //   省掉最多 512 次 fillRect 调用 + 软光栅状态切换（每帧确定性的微省）。
    juce::RectangleList<float> rects;
    rects.ensureStorageAllocated (kMaxPoints);

    for (int i = 0; i < N; i += stride)
    {
        const float rawL = snapshotL.getUnchecked (i);
        const float rawR = snapshotR.getUnchecked (i);
        if (! std::isfinite (rawL) || ! std::isfinite (rawR))
            continue;

        const float l = std::abs (rawL);
        const float r = std::abs (rawR);
        const float sum = l + r;
        if (sum < 1.0e-4f) continue;  // 静音点不画，避免在圆心堆积

        const float balance = (r - l) / sum;   // [-1,1]：-1 纯左，+1 纯右
        // 半径由"主导声道幅度"驱动（max(|L|,|R|)），使散点最大包络
        // 正好落在背景半圆（半径 R）上：
        //   · 纯左/纯右满幅 → rho=R，落在左右直径端点
        //   · 居中满幅       → rho=R，落在顶部
        //   · 任意声像满幅   → rho=R，落在半圆弧上（不再收缩成三角形）
        const float peak = juce::jmax (l, r);
        const float rho  = peak * effR;
        // 角度范围 ±90°：balance=-1 纯左 → 左水平线（直径线左端）；
        // balance=+1 纯右 → 右水平线（直径线右端）；0 = 顶部。覆盖完整上半圆。
        const float ang  = balance * (kPi * 0.5f);

        const float px = (float) cx + rho * std::sin (ang);
        const float py = (float) baseY - rho * std::cos (ang);

        if (! plotF.contains (px, py))
            continue;

        rects.addWithoutMerging ({ px - 1.0f, py - 1.0f, 2.0f, 2.0f });
    }

    if (! rects.isEmpty())
    {
        tg.setColour (PinkXP::pink500.withAlpha (0.9f));
        tg.fillRectList (rects);
    }
}

void StereoFieldModule::invalidateTrail()
{
    trailImage = juce::Image();
    trailNeedsUpdate = true;
}
