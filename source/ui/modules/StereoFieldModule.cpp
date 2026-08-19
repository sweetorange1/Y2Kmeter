#include "source/ui/modules/StereoFieldModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <cstring>

// ==========================================================
// StereoFieldModule —— Pink XP 像素风"声相指示"（半圆雷达）
// ==========================================================

StereoFieldModule::StereoFieldModule (AnalyserHub& h)
    : ModulePanel (ModuleType::stereoField), hub (h)
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

    trailNeedsUpdate = true;

    // repaint 节流：最短 15ms（~60fps）
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float scale = (float) juce::jmax (1.0, (double) juce::Component::getApproximateScaleFactorForComponent (this));
    const double minRepaintIntervalMs = 15.0 * (double) juce::jmin (2.0f, scale);
    if ((nowMs - lastRepaintMs) < minRepaintIntervalMs)
        return;
    lastRepaintMs = nowMs;
    repaint();
}

// ----------------------------------------------------------
// paintContent —— 底衬 + 残影叠加
// ----------------------------------------------------------
void StereoFieldModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
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

    // 1) 衰减旧点：所有像素 alpha *= kDecayPerFrame（跳过透明像素加速）
    {
        juce::Image::BitmapData bd (trailImage, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < bd.height; ++y)
        {
            auto* line = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer (y));
            for (int x = 0; x < bd.width; ++x)
            {
                auto& p = line[x];
                if (p.getAlpha() == 0) continue;
                p.multiplyAlpha (kDecayPerFrame);
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

    tg.setColour (PinkXP::pink500.withAlpha (0.9f));

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

        tg.fillRect (px - 1.0f, py - 1.0f, 2.0f, 2.0f);
    }
}

void StereoFieldModule::invalidateTrail()
{
    trailImage = juce::Image();
    trailNeedsUpdate = true;
}
