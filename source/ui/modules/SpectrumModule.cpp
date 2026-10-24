#include "source/ui/modules/SpectrumModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"
#include <cmath>

// ==========================================================
// SpectrumModule
// ==========================================================

SpectrumModule::SpectrumModule(AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::spectrum),
      hub(h)
{
    // Phase F：本模块只需要 Spectrum 一路 + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Spectrum);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // spectrum 6×4 大格
    setTitleText("Spectrum");

    btnPeak .setClickingTogglesState(true);
    btnSlope.setClickingTogglesState(true);
    btnPeak .setToggleState(peakHoldEnabled, juce::dontSendNotification);
    btnSlope.setToggleState(slopeEnabled,    juce::dontSendNotification);

    btnPeak .onClick = [this]() { setPeakHoldEnabled(btnPeak.getToggleState()); };
    btnSlope.onClick = [this]() { setSlopeEnabled   (btnSlope.getToggleState()); };

    addAndMakeVisible(btnPeak);
    addAndMakeVisible(btnSlope);

    rawMags.ensureStorageAllocated (256);
    smoothedDb .reserve (256);
    peakDb     .reserve (256);
    blurredDb  .reserve (256);
    slopeOffsetDb.reserve (256);
    curvePts     .reserve (256);
    peakCurvePts .reserve (256);

    lastTickTime = juce::Time::getCurrentTime();
}

SpectrumModule::~SpectrumModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Spectrum);
}

juce::ValueTree SpectrumModule::saveModuleSpecificState() const
{
    juce::ValueTree s("state");
    s.setProperty("peakHold", peakHoldEnabled, nullptr);
    s.setProperty("slope", slopeEnabled, nullptr);
    return s;
}

void SpectrumModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
    if (state.hasProperty("peakHold"))
        setPeakHoldEnabled((bool) state.getProperty("peakHold"));
    if (state.hasProperty("slope"))
        setSlopeEnabled((bool) state.getProperty("slope"));
}

void SpectrumModule::setPeakHoldEnabled(bool b)
{
    if (peakHoldEnabled == b) return;
    peakHoldEnabled = b;
    // 关闭时清空峰值，再次开启从当前值重新开始
    if (! peakHoldEnabled)
        std::fill(peakDb.begin(), peakDb.end(), minDb);
    btnPeak.setToggleState(peakHoldEnabled, juce::dontSendNotification);
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setSlopeEnabled(bool b)
{
    if (slopeEnabled == b) return;
    slopeEnabled = b;
    btnSlope.setToggleState(slopeEnabled, juce::dontSendNotification);
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// 频率/dB 坐标换算
// ----------------------------------------------------------
float SpectrumModule::freqToX(float freqHz, juce::Rectangle<int> canvas)
{
    const float f   = juce::jlimit(minFreqHz, maxFreqHz, freqHz);
    const float t   = (std::log10(f) - std::log10(minFreqHz))
                    / (std::log10(maxFreqHz) - std::log10(minFreqHz));
    return (float) canvas.getX() + t * (float) canvas.getWidth();
}

float SpectrumModule::xToFreq(float x, juce::Rectangle<int> canvas)
{
    const float t = (x - (float) canvas.getX()) / juce::jmax(1.0f, (float) canvas.getWidth());
    return std::pow(10.0f, std::log10(minFreqHz)
                         + juce::jlimit(0.0f, 1.0f, t)
                           * (std::log10(maxFreqHz) - std::log10(minFreqHz)));
}

float SpectrumModule::dbToY(float db, juce::Rectangle<int> canvas) const
{
    const float t = (db - minDb) / (maxDb - minDb);
    const float tc = juce::jlimit(0.0f, 1.0f, t);
    return (float) canvas.getBottom() - tc * (float) canvas.getHeight();
}

// ----------------------------------------------------------
// Toybox：拨弦与音名 + dB 读数（E.4.1）
// ----------------------------------------------------------
float SpectrumModule::toyReadDbAtX (float x) const
{
    const auto canvas = getToyCanvasBounds();
    const int N = (int) smoothedDb.size();
    if (N <= 0 || canvas.getWidth() <= 0)
        return -144.0f;

    const int col = juce::jlimit (0, N - 1,
        juce::roundToInt ((x - (float) canvas.getX()) / (float) canvas.getWidth() * (float) (N - 1)));
    float db = smoothedDb[(size_t) col];
    // Slope 打开时扣掉补偿才是真实 dBFS
    if (slopeEnabled && col < (int) slopeOffsetDb.size())
        db -= slopeOffsetDb[(size_t) col];
    return db;
}

bool SpectrumModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void SpectrumModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const float freq = toyXToFreq (hit.pos.x);
    const float db   = toyReadDbAtX (hit.pos.x);
    const juce::String note = toyFreqToNoteName (freq);

    ctx.addModuleActor (makeSonarRingActor (hit.pos));
    if (note.isNotEmpty())
    {
        juce::String text = note;
        if (db > -144.0f)
            text << " " << juce::String (db, 1) << "dB";
        ctx.bubble (hit.pos.translated (0.0f, -24.0f), text, ToyRole::Reward);
    }
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> SpectrumModule::getToolbarBounds(juce::Rectangle<int> content) const
{
    return content.withHeight(toolbarH);
}

juce::Rectangle<int> SpectrumModule::getCanvasBounds(juce::Rectangle<int> content) const
{
    // 左侧留 36 给 dB 标签，底部留 16 给 Hz 标签
    return content.withTrimmedTop(toolbarH + 4)
                  .withTrimmedLeft(32)
                  .withTrimmedBottom(16)
                  .withTrimmedRight(6);
}

void SpectrumModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto tb = getToolbarBounds(contentBounds).reduced(2);
    const int btnW = 60;
    const int gap = 4;
    btnPeak .setBounds(tb.getX(),                 tb.getY(), btnW, tb.getHeight());
    btnSlope.setBounds(tb.getX() + btnW + gap,    tb.getY(), btnW, tb.getHeight());

    // v2.9.5：尺寸守卫（防 layoutContent 每帧触发导致的交互卡顿）
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
        buildGlSnapshot();
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调（从 Hub 拿已经 "双路合并" 的幅度）
// ----------------------------------------------------------
void SpectrumModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    // v2.9：取消 "每 2 tick 才 repaint" 的分频与 16ms×DPI 节流（两者叠加后
    //   本模块实际只有 ~30fps）。帧分发已与 vblank 同步并受 FPS 上限约束，
    //   每个分发帧都重绘才能跑满 120fps；曲线本身走离屏缓存，成本可控。

    // 使用 Hub 的双路合并接口：低频走 8192 点（Δf≈5.9Hz），高频走 2048 点（Δf≈23Hz），
    // 在 500Hz 附近做等功率交叉淡化。UI 侧零额外重采样，直接拿到"每列一个线性幅度"。
    //
    // 关于 frame：
    //   ·frame.spectrumMag / spectrumMagLo 仍然保留（其他模块可能用），本模块不再使用；
    //   · blended 接口走 Hub 内部锁，成本极低（N 次查表 + 一次 cos 交叉）。
    constexpr int N = 256;
    hub.getSpectrumMagnitudesBlended (rawMags, N,
                                      minFreqHz,
                                      juce::jmin (maxFreqHz, (float) (hub.getSampleRate() * 0.5)));

    // v2.9：高精度帧间隔（旧实现 Time::getCurrentTime 整数毫秒，120Hz 下抖动）
    frameDtSec = frame.dtSec;
    const float deltaMs = frame.dtSec * 1000.0f;
    lastTickTime = juce::Time::getCurrentTime();

    rebuildDisplay();

    // 峰值下降更新（持续下落，无 hold / 无阈值）
    //   · 任一列信号超过当前 peak → 顶上去（刷新该列峰值）；
    //   · 否则该列以 peakFallRatePerSec 持续下降，直到 minDb。
    //   不再有"保持"阶段：无论信号强度如何，peak 线始终处于缓慢下落的
    //   动态——信号顶上来才抬升，否则一直回落，形成贴着信号、带下落惯性的虚线。
    if (peakHoldEnabled && ! peakDb.empty())
    {
        const float fallDelta = peakFallRatePerSec * (deltaMs / 1000.0f);

        for (size_t i = 0; i < peakDb.size(); ++i)
        {
            if (smoothedDb[i] > peakDb[i])
                peakDb[i] = smoothedDb[i];                                  // 信号超过峰值 → 顶上去
            else
                peakDb[i] = juce::jmax (minDb, peakDb[i] - fallDelta);      // 否则持续下落
        }
    }

    curve_cache_dirty_ = true;
    // v2.9.4：同步生成 GL 直渲染快照
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// rebuildDisplay —— 已合并的 N 列线性幅度 → N 列 dBFS 值（Slope 可选）
//
//   变更说明：
//     旧实现里本函数把 1024 bin 的 FFT 幅度**在 UI 侧**按对数频率轴重采样到 N=256 列，
//     并且费大力气处理"低频一个 bin 覆盖多列 → 阶梯/平滑"。
//     现在 Hub 已直接返回 N 列对数频率轴上的合并线性幅度（低频路 8192FFT + 高频路 2048FFT），
//     本函数只需：
//       1) 线性 → dB；
//       2) 可选 Slope 补偿；
//       3) 上升快 / 下降慢平滑；
//       4) 末端轻量 3 列模糊磨圆拐点。
// ----------------------------------------------------------
void SpectrumModule::rebuildDisplay()
{
    const int N = rawMags.size();
    if (N <= 1) return;

    if ((int) smoothedDb.size() != N)
    {
        smoothedDb  .assign(N, minDb);
        peakDb      .assign(N, minDb);
        blurredDb   .assign(N, minDb);
    }

    ensureDisplayCache (N);

    // v2.9：平滑系数按 dt 换算（120Hz 标定 = 旧版每 tick 系数）
    const float alphaUp   = y2k::pacing::alphaForDt (0.55f, frameDtSec);
    const float alphaDown = y2k::pacing::alphaForDt (0.12f, frameDtSec);

    for (int col = 0; col < N; ++col)
    {
        const float mag = std::abs (rawMags.getUnchecked (col));
        float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));

        // Slope 补偿：4.5 dB/oct 高频提升（基准 1kHz）
        if (slopeEnabled && (int) slopeOffsetDb.size() == N)
            db += slopeOffsetDb[(size_t) col];

        // 平滑：上升快，下降慢
        const float prev  = smoothedDb[(size_t) col];
        const float alpha = (db > prev) ? alphaUp : alphaDown;
        float sm = prev + alpha * (db - prev);
        sm = juce::jlimit (minDb, maxDb + 12.0f, sm);
        smoothedDb[(size_t) col] = sm;
    }

    // ----------------------------------------------------------
    //  额外的列间平滑（一维小核卷积）：磨圆列间拐点
    //   · 仅对已经求好的 smoothedDb 做轻量 [1,2,1]/4 低通，核宽 3 列
    //   · 纯视觉磨平，完全不影响频率/幅度精度
    // ----------------------------------------------------------
    if (N >= 3)
    {
        if ((int) blurredDb.size() != N)
            blurredDb.assign (N, minDb);

        blurredDb[0]     = smoothedDb[0];
        blurredDb[N - 1] = smoothedDb[(size_t)(N - 1)];
        for (int i = 1; i < N - 1; ++i)
        {
            blurredDb[(size_t) i] = 0.25f * smoothedDb[(size_t)(i - 1)]
                                   + 0.50f * smoothedDb[(size_t) i]
                                   + 0.25f * smoothedDb[(size_t)(i + 1)];
        }
        smoothedDb.swap(blurredDb);
    }
}

void SpectrumModule::ensureDisplayCache (int numPoints)
{
    numPoints = juce::jmax (2, numPoints);

    const double sampleRate = hub.getSampleRate();
    if (slopeCacheSize == numPoints
        && std::abs (slopeCacheSampleRate - sampleRate) <= 0.5
        && (int) slopeOffsetDb.size() == numPoints)
    {
        return;
    }

    slopeOffsetDb.resize ((size_t) numPoints);

    const double fMin   = (double) minFreqHz;
    const double fMax   = juce::jmin ((double) maxFreqHz, sampleRate * 0.5);
    const double logMin = std::log10 (fMin);
    const double logMax = std::log10 (juce::jmax (fMin, fMax));

    for (int col = 0; col < numPoints; ++col)
    {
        const double t = (double) col / (double) juce::jmax (1, numPoints - 1);
        const double f = std::pow (10.0, logMin + t * (logMax - logMin));
        const double octaves = std::log2 (juce::jmax (20.0, f) / 1000.0);
        slopeOffsetDb[(size_t) col] = (float) (4.5 * octaves);
    }

    slopeCacheSize = numPoints;
    slopeCacheSampleRate = sampleRate;
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
// ---- GlDirectModuleBase 钩子（v2.9.4 迁移）----
juce::Rectangle<int> SpectrumModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void SpectrumModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 接管时只保留轴标签与悬停标尺（GL 内容之上）
    drawAxisLabels (g, canvas);
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void SpectrumModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const int cw = contentBounds.getWidth();
    const int ch = contentBounds.getHeight();
    if (cw <= 0 || ch <= 0) return;

    // 1) 静态层：尺寸 / DPI / 主题配色变化时才重建，否则直接贴（GL 复用纹理）
    const float physScale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto  colourKey = computeThemeColourKey();
    if (! staticLayer_.isValid()
        || staticLayerBounds_ != contentBounds
        || std::abs (staticLayerScale_ - physScale) > 0.01f
        || staticLayerColourKey_ != colourKey)
    {
        renderStaticLayer (contentBounds, physScale);
        staticLayerColourKey_ = colourKey;
    }

    g.drawImageTransformed (staticLayer_,
                            juce::AffineTransform::scale (1.0f / staticLayerScale_)
                                .translated ((float) contentBounds.getX(), (float) contentBounds.getY()));

    // 2) 动态曲线：每帧直接画
    auto canvas = getCanvasBounds(contentBounds);
    if (canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (canvas.reduced (2));
        drawCurves(g, canvas);
    }
    curve_cache_dirty_ = false;

    // 鼠标悬停标尺
    if (hoverActive)
        drawHoverRuler(g, canvas);
}

// ----------------------------------------------------------
// buildGlSnapshot —— 频谱曲线 GL 顶点流（v2.9.4）
//   布局：[凹陷背景 + dB 横线 + 频率纵线]
//         [曲线填充（梯形条到画布底）]
//         [主曲线 glow strip][主曲线主线 strip]
//         [峰值保持虚线（3px 段 / 3px 空，与旧版 dash 一致）]
//   曲线为 1px 间距采样点直接折线（Catmull-Rom 平滑在 1px 间距下不可分辨）。
// ----------------------------------------------------------
void SpectrumModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto canvas = getCanvasBounds (getContentBounds());
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    const int N = (int) smoothedDb.size();
    if (N <= 1)
        return;

    auto snap = beginGlSnapshot();
    snap->renderToLogical = true;
    snap->logicalW = canvas.getWidth();
    snap->logicalH = canvas.getHeight();
    snap->canvasLocal = canvas;

    // ---- 背景（凹陷，框架 helper）----
    appendSunkenFrame (snap->vertices, canvas);

    auto inner = canvas.reduced (2);

    // ---- 网格（与 drawGrid 逐笔对应）----
    {
        // 横线（每 20dB，0dB 高亮）
        for (int db = (int) minDb; db <= (int) maxDb; db += 20)
        {
            const int y = (int) std::round (dbToY ((float) db, canvas));
            if (y < inner.getY() || y > inner.getBottom()) continue;
            const auto c = (db == 0) ? PinkXP::pink400.withAlpha (0.7f)
                                     : PinkXP::pink200.withAlpha (0.45f);
            appendLineSegment (snap->vertices,
                               (float) inner.getX(), (float) y,
                               (float) inner.getRight(), (float) y,
                               0.5f, c);
        }

        // 纵线（次 2/5 + 主 decade）
        const float minorFreqs[] = { 30.0f, 50.0f, 200.0f, 300.0f, 500.0f,
                                     2000.0f, 3000.0f, 5000.0f, 15000.0f };
        const float majorFreqs[] = { 100.0f, 1000.0f, 10000.0f };
        const auto minorC = PinkXP::pink200.withAlpha (0.3f);
        const auto majorC = PinkXP::pink300.withAlpha (0.6f);
        for (float f : minorFreqs)
        {
            const int x = (int) std::round (freqToX (f, canvas));
            if (x > inner.getX() && x < inner.getRight())
                appendLineSegment (snap->vertices,
                                   (float) x, (float) inner.getY(),
                                   (float) x, (float) inner.getBottom(),
                                   0.5f, minorC);
        }
        for (float f : majorFreqs)
        {
            const int x = (int) std::round (freqToX (f, canvas));
            if (x > inner.getX() && x < inner.getRight())
                appendLineSegment (snap->vertices,
                                   (float) x, (float) inner.getY(),
                                   (float) x, (float) inner.getBottom(),
                                   0.5f, majorC);
        }
    }

    // ---- 曲线采样点（等距 x）→ Catmull-Rom 样条细分折线（v2.9.4 修正）----
    //   旧版用 Catmull-Rom（tension 0.5）→ 三次贝塞尔画曲线；直接折线化时
    //   256 采样点分布在 ~400px 画布（点距 1.6px），dB 台阶清晰可见（"小线段"）。
    //   这里复刻旧版控制点公式，把每段贝塞尔细分 4 段（0.4px 间隔），
    //   折线在亚像素尺度逼近曲线，视觉与旧版一致。
    const float x0   = (float) inner.getX();
    const float xLen = (float) inner.getWidth();
    const float invMax = 1.0f / (float) juce::jmax (1, N - 1);
    const float yTop = (float) inner.getY();
    const float yBot = (float) inner.getBottom();

    auto samplePts = [&] (const std::vector<float>& dbVals,
                          std::vector<float>& outX, std::vector<float>& outY)
    {
        const int n = (int) dbVals.size();
        outX.clear();
        outY.clear();
        if (n < 2) return;

        std::vector<float> px ((size_t) n), py ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            px[(size_t) i] = x0 + (float) i * invMax * xLen;
            py[(size_t) i] = dbToY (dbVals[(size_t) i], canvas);
        }

        constexpr float floorPixelTol = 0.75f;
        constexpr int kSubdiv = 4;   // 每段贝塞尔细分 4 段

        auto atFloor = [&] (float y) noexcept { return (yBot - y) <= floorPixelTol; };

        outX.reserve ((size_t) (n * kSubdiv + 4));
        outY.reserve ((size_t) (n * kSubdiv + 4));
        outX.push_back (px[0]);
        outY.push_back (py[0]);

        for (int i = 0; i < n - 1; ++i)
        {
            const float p0x = (i == 0)     ? px[0]     : px[(size_t) i - 1];
            const float p0y = (i == 0)     ? py[0]     : py[(size_t) i - 1];
            const float p1x = px[(size_t) i];
            const float p1y = py[(size_t) i];
            const float p2x = px[(size_t) i + 1];
            const float p2y = py[(size_t) i + 1];
            const float p3x = (i + 2 >= n) ? px[(size_t) n - 1] : px[(size_t) i + 2];
            const float p3y = (i + 2 >= n) ? py[(size_t) n - 1] : py[(size_t) i + 2];

            // 两端贴底 → 直线贴底（与旧版一致）
            if (atFloor (p1y) && atFloor (p2y))
            {
                outX.push_back (p2x);
                outY.push_back (yBot);
                continue;
            }

            const float c1x = p1x + (p2x - p0x) / 6.0f;
            const float c2x = p2x - (p3x - p1x) / 6.0f;
            float c1y = p1y + (p2y - p0y) / 6.0f;
            float c2y = p2y - (p3y - p1y) / 6.0f;
            if (atFloor (p1y)) c1y = p1y;
            if (atFloor (p2y)) c2y = p2y;
            c1y = juce::jlimit (yTop, yBot, c1y);
            c2y = juce::jlimit (yTop, yBot, c2y);

            for (int s = 1; s <= kSubdiv; ++s)
            {
                const float t = (float) s / (float) kSubdiv;
                const float u = 1.0f - t;
                const float w0 = u * u * u;
                const float w1 = 3.0f * u * u * t;
                const float w2 = 3.0f * u * t * t;
                const float w3 = t * t * t;
                outX.push_back (w0 * p1x + w1 * c1x + w2 * c2x + w3 * p2x);
                outY.push_back (w0 * p1y + w1 * c1y + w2 * c2y + w3 * p2y);
            }
        }
    };

    std::vector<float> sx, sy, px, py;
    samplePts (smoothedDb, sx, sy);
    const int M = (int) sx.size();

    // ---- 1) 曲线填充（梯形条到画布底）----
    if (M > 1)
    {
        const auto fillC = PinkXP::chartRamp[3].withAlpha (0.25f);
        const float fr = fillC.getFloatRed(), fg = fillC.getFloatGreen(),
                    fb = fillC.getFloatBlue(), fa = fillC.getFloatAlpha();
        snap->vertices.reserve (snap->vertices.size() + (size_t) (M - 1) * 36);
        for (int i = 0; i + 1 < M; ++i)
        {
            const float q[36] = {
                sx[i],     sy[i],     fr, fg, fb, fa,
                sx[i + 1], sy[i + 1], fr, fg, fb, fa,
                sx[i],     yBot,      fr, fg, fb, fa,
                sx[i + 1], sy[i + 1], fr, fg, fb, fa,
                sx[i + 1], yBot,      fr, fg, fb, fa,
                sx[i],     yBot,      fr, fg, fb, fa,
            };
            snap->vertices.insert (snap->vertices.end(), q, q + 36);
        }
    }

    // ---- 2) 主曲线（glow 1.5px 半透明 + 主线 0.7px）----
    //   v2.9.4 修正：改用"连续折线带"（内部点法线取相邻两段法线的平均），
    //   相邻段共享端点顶点、无重叠无缝隙 —— 之前的逐段独立 quad 在端点
    //   重叠，端点亮度翻倍（"很多小亮点、亮度不一致"的根因）。
    if (M > 1)
    {
        // 预计算每个点的平均法线（单位向量）
        std::vector<float> nx ((size_t) M), ny ((size_t) M);
        for (int i = 0; i < M; ++i)
        {
            float ax = 0.0f, ay = 0.0f;
            if (i > 0)
            {
                const float dx = sx[i] - sx[i - 1];
                const float dy = sy[i] - sy[i - 1];
                const float l = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-6f) { ax += -dy / l; ay += dx / l; }
            }
            if (i + 1 < M)
            {
                const float dx = sx[i + 1] - sx[i];
                const float dy = sy[i + 1] - sy[i];
                const float l = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-6f) { ax += -dy / l; ay += dx / l; }
            }
            const float l = std::sqrt (ax * ax + ay * ay);
            nx[(size_t) i] = (l > 1.0e-6f) ? ax / l : 0.0f;
            ny[(size_t) i] = (l > 1.0e-6f) ? ay / l : 0.0f;
        }

        auto appendStrip = [&] (float halfW, juce::Colour c)
        {
            const float r = c.getFloatRed(), g = c.getFloatGreen(),
                        b = c.getFloatBlue(), a = c.getFloatAlpha();
            for (int i = 0; i + 1 < M; ++i)
            {
                const float x0 = sx[i]     + nx[(size_t) i] * halfW;
                const float y0 = sy[i]     + ny[(size_t) i] * halfW;
                const float x1 = sx[i + 1] + nx[(size_t) i + 1] * halfW;
                const float y1 = sy[i + 1] + ny[(size_t) i + 1] * halfW;
                const float x2 = sx[i]     - nx[(size_t) i] * halfW;
                const float y2 = sy[i]     - ny[(size_t) i] * halfW;
                const float x3 = sx[i + 1] - nx[(size_t) i + 1] * halfW;
                const float y3 = sy[i + 1] - ny[(size_t) i + 1] * halfW;
                const float q[36] = {
                    x0, y0, r, g, b, a,  x1, y1, r, g, b, a,  x2, y2, r, g, b, a,
                    x1, y1, r, g, b, a,  x3, y3, r, g, b, a,  x2, y2, r, g, b, a,
                };
                snap->vertices.insert (snap->vertices.end(), q, q + 36);
            }
        };

        appendStrip (1.5f, PinkXP::chartRamp[5].withAlpha (0.35f));
        appendStrip (0.7f, PinkXP::chartRamp[6]);
    }

    // ---- 3) 峰值保持虚线（弧长 3px 段 / 3px 空，与旧版 createDashedStroke 一致）----
    //   v2.9.4 修正：dash 边界精确插值 —— 峰值"信号消失边缘"的相邻列落差
    //   巨大（单段弧长可达几十像素），旧逻辑"画完整段再计数"会把超长段
    //   整段画出（线被拉长）。现在沿路径以 3px 为预算推进，超长段在 3px
    //   处截断，每根虚线严格 ≤3px。
    if (peakHoldEnabled && (int) peakDb.size() == N)
    {
        samplePts (peakDb, px, py);
        const int Mp = (int) px.size();
        if (Mp > 1)
        {
            // peak 虚线用 ink：light 下深灰、dark 下浅白，任何画布上都清晰
            const auto dashC = PinkXP::ink.withAlpha (0.75f);
            float curX = px[0], curY = py[0];
            float arcPos = 0.0f;      // 当前 dash 周期内已走的弧长
            bool drawing = true;
            for (int i = 0; i + 1 < Mp; ++i)
            {
                const float dx = px[i + 1] - curX;
                const float dy = py[i + 1] - curY;
                const float segLen = std::sqrt (dx * dx + dy * dy);
                if (segLen < 1.0e-6f) continue;
                const float ux = dx / segLen, uy = dy / segLen;

                float travelled = 0.0f;
                while (travelled < segLen - 1.0e-6f)
                {
                    const float remain = 3.0f - arcPos;
                    const float step = std::min (remain, segLen - travelled);
                    const float nx = curX + ux * step;
                    const float ny = curY + uy * step;
                    if (drawing)
                        appendLineSegment (snap->vertices, curX, curY, nx, ny, 0.6f, dashC);
                    curX = nx;
                    curY = ny;
                    travelled += step;
                    arcPos += step;
                    if (arcPos >= 3.0f - 1.0e-6f) { arcPos = 0.0f; drawing = !drawing; }
                }
            }
        }
    }

    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

void SpectrumModule::mouseMove(const juce::MouseEvent& e)
{
    ModulePanel::mouseMove(e);

    const auto canvas = getCanvasBounds(getContentBounds());
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains(pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaint();
    }
}

void SpectrumModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint();
    }
}

void SpectrumModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const float freqHz = xToFreq((float) hoverPos.x, canvas);
    const float t = (float)(canvas.getBottom() - hoverPos.y)
                  / juce::jmax(1.0f, (float) canvas.getHeight());
    const float db = minDb + juce::jlimit(0.0f, 1.0f, t) * (maxDb - minDb);

    const juce::String readout = PinkXP::formatFreqHz(freqHz)
                               + "  " + juce::String(db, 1) + " dBFS";
    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
}

// ----------------------------------------------------------
// renderStaticLayer —— 底衬 / 画布 / 网格 / 坐标轴标签渲染到离屏 Image（物理像素分辨率）
// ----------------------------------------------------------
juce::uint32 SpectrumModule::computeThemeColourKey() noexcept
{
    juce::uint32 k = 2166136261u;
    for (auto c : { PinkXP::btnFace, PinkXP::content, PinkXP::pink200, PinkXP::pink400, PinkXP::ink })
        k = (k ^ c.getARGB()) * 16777619u;
    return k;
}

void SpectrumModule::renderStaticLayer (juce::Rectangle<int> contentBounds, float physicalScale) const
{
    const int pw = juce::jmax (1, juce::roundToInt ((float) contentBounds.getWidth()  * physicalScale));
    const int ph = juce::jmax (1, juce::roundToInt ((float) contentBounds.getHeight() * physicalScale));

    if (! staticLayer_.isValid() || staticLayer_.getWidth() != pw || staticLayer_.getHeight() != ph)
        staticLayer_ = juce::Image (juce::Image::ARGB, pw, ph, true);
    else
        staticLayer_.clear (staticLayer_.getBounds());

    staticLayerBounds_ = contentBounds;
    staticLayerScale_  = physicalScale;

    juce::Graphics g (staticLayer_);
    g.addTransform (juce::AffineTransform::translation ((float) -contentBounds.getX(),
                                                        (float) -contentBounds.getY())
                        .scaled (physicalScale));

    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    auto canvas = getCanvasBounds(contentBounds);
    drawBackground(g, canvas);

    if (canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        drawGrid(g, canvas);
        drawAxisLabels(g, canvas);
    }
}

void SpectrumModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken(g, canvas, PinkXP::content);
}

// ----------------------------------------------------------
// 网格：每 20dB 一条横线，以及 100/1k/10k 纵线
// ----------------------------------------------------------
void SpectrumModule::drawGrid(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    auto inner = canvas.reduced(2);
    if (inner.isEmpty()) return;

    // 横线（每 20dB）
    g.setColour(PinkXP::pink200.withAlpha(0.45f));
    for (int db = (int) minDb; db <= (int) maxDb; db += 20)
    {
        const int y = (int) std::round(dbToY((float) db, canvas));
        if (y < inner.getY() || y > inner.getBottom()) continue;
        if (db == 0)
            g.setColour(PinkXP::pink400.withAlpha(0.7f));
        else
            g.setColour(PinkXP::pink200.withAlpha(0.45f));
        g.drawHorizontalLine(y, (float) inner.getX(), (float) inner.getRight());
    }

    // 纵线（主 decade + 次 2/5）
    const float majorFreqs[]   = { 100.0f, 1000.0f, 10000.0f };
    const float minorFreqs[]   = { 30.0f, 50.0f, 200.0f, 300.0f, 500.0f,
                                   2000.0f, 3000.0f, 5000.0f, 15000.0f };

    g.setColour(PinkXP::pink200.withAlpha(0.3f));
    for (float f : minorFreqs)
    {
        const int x = (int) std::round(freqToX(f, canvas));
        if (x > inner.getX() && x < inner.getRight())
            g.drawVerticalLine(x, (float) inner.getY(), (float) inner.getBottom());
    }
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    for (float f : majorFreqs)
    {
        const int x = (int) std::round(freqToX(f, canvas));
        if (x > inner.getX() && x < inner.getRight())
            g.drawVerticalLine(x, (float) inner.getY(), (float) inner.getBottom());
    }
}

// ----------------------------------------------------------
// 绘制曲线（平滑 + 峰值保持）
//
//   · 低频段 20~100Hz 在对数坐标下挤在左侧 ~40% 像素内，而那段仅覆盖少数 FFT bin，
//     直接用 lineTo 折线会看到明显尖角。
//   · 这里用 Catmull-Rom 样条（tension 0.5）→ 转成三次贝塞尔 cubicTo：
//       - 曲线严格经过所有采样点（不改变频谱数值，纯视觉插值）
//       - 纯前端开销：对每个 x 点额外算两次减法/除法，完全不碰后端分析
//       - 端点重复处理避免首/末段被截掉
//       - 对控制点 y 做 jlimit 防止抛物线过冲跌出画布
// ----------------------------------------------------------
void SpectrumModule::drawCurves(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const int N = (int) smoothedDb.size();
    if (N <= 1) return;

    auto inner = canvas.reduced(2);
    if (inner.getWidth() <= 2) return;

    // ---------- 先把 N 个采样点映射到像素坐标 ----------
    //   (等距 x；y 由 dbToY 换算)
    curvePts.resize ((size_t) N);
    const float x0   = (float) inner.getX();
    const float xLen = (float) inner.getWidth();
    const float invMax = 1.0f / (float) juce::jmax (1, N - 1);
    for (int i = 0; i < N; ++i)
    {
        const float x = x0 + (float) i * invMax * xLen;
        const float y = dbToY (smoothedDb[(size_t) i], canvas);
        curvePts[(size_t) i] = { x, y };
    }

    // ---------- Catmull-Rom → cubicBezier 构建 Path ----------
    const float yTop = (float) inner.getY();
    const float yBot = (float) inner.getBottom();

    // "沉底"判定阈值（像素）：当采样点的 y 到画布底的距离小于该阈值时，视为贴底。
    //   用像素判据而非 dB 判据：
    //     1) 与 dbToY 对 minDb 的精确取值无关，天然免疫浮点误差；
    //     2) 只要视觉上已贴底，就不再让邻点 p3 的切线把本段抬出一个小弧。
    //   0.75 px ≈ 亚像素级贴底，不会把"刚离开底部一点点"的正常上升误判为沉底。
    constexpr float floorPixelTol = 0.75f;

    auto buildSmoothPath = [&] (juce::Path& path,
                                const std::vector<juce::Point<float>>& P,
                                bool closeToBottom)
    {
        const int n = (int) P.size();
        if (n < 2) return;

        auto atFloor = [yBot, floorPixelTol] (const juce::Point<float>& p) noexcept
        {
            return (yBot - p.y) <= floorPixelTol;
        };

        if (closeToBottom) path.startNewSubPath (P[0].x, yBot); // fill 版：从底部开始
        else               path.startNewSubPath (P[0]);

        if (closeToBottom) path.lineTo (P[0]); // 先拉一条竖线到第一个点

        // 端点处理：P[-1] = P[0], P[n] = P[n-1]（复制端点 → Catmull-Rom 自然收束）
        for (int i = 0; i < n - 1; ++i)
        {
            const auto& p0 = (i == 0)     ? P[0]     : P[i - 1];
            const auto& p1 = P[i];
            const auto& p2 = P[i + 1];
            const auto& p3 = (i + 2 >= n) ? P[n - 1] : P[i + 2];

            // ---------- 沉底直线段：p1、p2 都贴底 ----------
            //   Catmull-Rom 在"两端都贴底"的段里，仍会按 p0/p3 的斜率生成控制点，
            //   导致 p1→p2 间出现向上或向下的小弧（尤其当 p3 是陡升峰时）。
            //   这种情况下视觉上"就是一条水平底线"，直接 lineTo 到 (p2.x, yBot)
            //   可完全消除小弧，并让填充路径严格贴画布底沿。
            if (atFloor (p1) && atFloor (p2))
            {
                path.lineTo (p2.x, yBot);
                continue;
            }

            // Catmull-Rom (tension=0.5) → Bezier 控制点
            const float c1x = p1.x + (p2.x - p0.x) / 6.0f;
            const float c2x = p2.x - (p3.x - p1.x) / 6.0f;
            float c1y       = p1.y + (p2.y - p0.y) / 6.0f;
            float c2y       = p2.y - (p3.y - p1.y) / 6.0f;

            // 若本段的起点或终点贴底，则把对应端的切线分量强制归零，
            // 避免"从底抬起/跌入底"瞬间由对向邻点造成的鼓包/下凹。
            if (atFloor (p1)) c1y = p1.y;
            if (atFloor (p2)) c2y = p2.y;

            // 防止过冲到画布外（贝塞尔控制点 y 超出会让曲线短暂跌出可视区）
            c1y = juce::jlimit (yTop, yBot, c1y);
            c2y = juce::jlimit (yTop, yBot, c2y);

            path.cubicTo (c1x, c1y, c2x, c2y, p2.x, p2.y);
        }

        if (closeToBottom)
        {
            path.lineTo (P[n - 1].x, yBot);
            path.closeSubPath();
        }
    };

    // 1) 填充区域（从底部到曲线的 lightly tinted area）
    fillPath.clear();
    buildSmoothPath (fillPath, curvePts, /*closeToBottom*/ true);
    g.setColour (PinkXP::chartRamp[3].withAlpha (0.25f));
    g.fillPath (fillPath);

    // 2) 主曲线（双层描边：浅底 + 深顶，视觉厚度）
    curvePath.clear();
    buildSmoothPath (curvePath, curvePts, /*closeToBottom*/ false);
    g.setColour (PinkXP::chartRamp[5].withAlpha (0.35f));
    g.strokePath (curvePath, juce::PathStrokeType (3.0f));
    g.setColour (PinkXP::chartRamp[6]);
    g.strokePath (curvePath, juce::PathStrokeType (1.4f));

    // 3) 峰值保持（虚线）—— 同样做 Catmull-Rom 平滑
    if (peakHoldEnabled && (int) peakDb.size() == N)
    {
        peakCurvePts.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const float x = x0 + (float) i * invMax * xLen;
            const float y = dbToY (peakDb[(size_t) i], canvas);
            peakCurvePts[(size_t) i] = { x, y };
        }
        peakPath.clear();
        buildSmoothPath (peakPath, peakCurvePts, /*closeToBottom*/ false);

        dashedPeakPath.clear();
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashedPeakPath, peakPath, dashes, 2);
        g.setColour (PinkXP::ink.withAlpha (0.75f));
        g.fillPath (dashedPeakPath);
    }
}

// ----------------------------------------------------------
// 坐标轴标签（左侧 dB / 底部 Hz）
// ----------------------------------------------------------
void SpectrumModule::drawAxisLabels(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    g.setColour(PinkXP::ink.withAlpha(0.85f));

    // 左侧 dB 标签
    for (int db = (int) minDb; db <= (int) maxDb; db += 20)
    {
        const int y = (int) std::round(dbToY((float) db, canvas));
        juce::String s = (db == 0) ? " 0" : juce::String(db);
        g.drawText(s, canvas.getX() - 32, y - 6, 28, 12,
                   juce::Justification::centredRight, false);
    }

    // 底部 Hz 标签
    struct L { float hz; const char* label; };
    const L labels[] = {
        { 20.0f, "20" }, { 100.0f, "100" }, { 1000.0f, "1k" },
        { 10000.0f, "10k" }, { 20000.0f, "20k" }
    };
    for (auto& l : labels)
    {
        const int x = (int) std::round(freqToX(l.hz, canvas));
        if (x < canvas.getX() - 4 || x > canvas.getRight() + 4) continue;
        g.drawText(l.label, x - 18, canvas.getBottom() + 2, 36, 12,
                   juce::Justification::centred, false);
    }
}
