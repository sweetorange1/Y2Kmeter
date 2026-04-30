#include "source/ui/modules/SpectrumModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// SpectrumModule
// ==========================================================

SpectrumModule::SpectrumModule(AnalyserHub& h)
    : ModulePanel(ModuleType::spectrum),
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

    lastTickTime = juce::Time::getCurrentTime();
}

SpectrumModule::~SpectrumModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Spectrum);
}

void SpectrumModule::setPeakHoldEnabled(bool b)
{
    if (peakHoldEnabled == b) return;
    peakHoldEnabled = b;
    // 关闭时清空峰值，再次开启从当前值重新开始
    if (! peakHoldEnabled)
    {
        std::fill(peakDb.begin(),     peakDb.end(),     minDb);
        std::fill(peakHoldMs.begin(), peakHoldMs.end(), 0.0f);
    }
    btnPeak.setToggleState(peakHoldEnabled, juce::dontSendNotification);
    repaint();
}

void SpectrumModule::setSlopeEnabled(bool b)
{
    if (slopeEnabled == b) return;
    slopeEnabled = b;
    btnSlope.setToggleState(slopeEnabled, juce::dontSendNotification);
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
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调（从 Hub 拿已经 "双路合并" 的幅度）
// ----------------------------------------------------------
void SpectrumModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

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

    const auto now = juce::Time::getCurrentTime();
    const float deltaMs = (float)(now - lastTickTime).inMilliseconds();
    lastTickTime = now;

    rebuildDisplay();

    // 峰值保持 / 下降更新
    if (peakHoldEnabled && ! peakDb.empty())
    {
        for (size_t i = 0; i < peakDb.size(); ++i)
        {
            if (smoothedDb[i] > peakDb[i])
            {
                peakDb[i] = smoothedDb[i];
                peakHoldMs[i] = 0.0f;
            }
            else
            {
                peakHoldMs[i] += deltaMs;
                if (peakHoldMs[i] > peakHoldDuration)
                    peakDb[i] -= peakFallRate;
            }
            peakDb[i] = juce::jmax(minDb, peakDb[i]);
        }
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float scale  = (float) juce::jmax (1.0, (double) juce::Component::getApproximateScaleFactorForComponent (this));
    const double minRepaintIntervalMs = 16.0 * (double) juce::jmin (1.8f, scale);
    if ((nowMs - lastRepaintMs) < minRepaintIntervalMs)
        return;

    lastRepaintMs = nowMs;
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
        smoothedDb .assign(N, minDb);
        peakDb     .assign(N, minDb);
        peakHoldMs .assign(N, 0.0f);
    }

    const double fMin   = (double) minFreqHz;
    const double fMax   = juce::jmin ((double) maxFreqHz, hub.getSampleRate() * 0.5);
    const double logMin = std::log10 (fMin);
    const double logMax = std::log10 (fMax);

    for (int col = 0; col < N; ++col)
    {
        const float mag = std::abs (rawMags.getUnchecked (col));
        float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));

        // Slope 补偿：4.5 dB/oct 高频提升（基准 1kHz）
        if (slopeEnabled)
        {
            const double t = (double) col / (double) juce::jmax (1, N - 1);
            const double f = std::pow (10.0, logMin + t * (logMax - logMin));
            const double octaves = std::log2 (juce::jmax (20.0, f) / 1000.0);
            db += (float) (4.5 * octaves);
        }

        // 平滑：上升快，下降慢
        const float prev  = smoothedDb[(size_t) col];
        const float alpha = (db > prev) ? 0.55f : 0.12f;
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
        std::vector<float> blurred (smoothedDb.size());
        blurred[0]     = smoothedDb[0];
        blurred[N - 1] = smoothedDb[(size_t)(N - 1)];
        for (int i = 1; i < N - 1; ++i)
        {
            blurred[(size_t) i] = 0.25f * smoothedDb[(size_t)(i - 1)]
                                + 0.50f * smoothedDb[(size_t) i]
                                + 0.25f * smoothedDb[(size_t)(i + 1)];
        }
        smoothedDb.swap(blurred);
    }
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
void SpectrumModule::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    auto canvas = getCanvasBounds(contentBounds);
    drawBackground(g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    drawGrid      (g, canvas);
    drawCurves    (g, canvas);
    drawAxisLabels(g, canvas);
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
    std::vector<juce::Point<float>> pts;
    pts.reserve ((size_t) N);
    const float x0   = (float) inner.getX();
    const float xLen = (float) inner.getWidth();
    const float invMax = 1.0f / (float) juce::jmax (1, N - 1);
    for (int i = 0; i < N; ++i)
    {
        const float x = x0 + (float) i * invMax * xLen;
        const float y = dbToY (smoothedDb[(size_t) i], canvas);
        pts.push_back ({ x, y });
    }

    // ---------- Catmull-Rom → cubicBezier 构建 Path ----------
    const float yTop = (float) inner.getY();
    const float yBot = (float) inner.getBottom();

    auto buildSmoothPath = [&] (juce::Path& path,
                                const std::vector<juce::Point<float>>& P,
                                bool closeToBottom)
    {
        const int n = (int) P.size();
        if (n < 2) return;

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

            // Catmull-Rom (tension=0.5) → Bezier 控制点
            const float c1x = p1.x + (p2.x - p0.x) / 6.0f;
            const float c2x = p2.x - (p3.x - p1.x) / 6.0f;
            float c1y       = p1.y + (p2.y - p0.y) / 6.0f;
            float c2y       = p2.y - (p3.y - p1.y) / 6.0f;

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
    juce::Path fill;
    buildSmoothPath (fill, pts, /*closeToBottom*/ true);
    g.setColour (PinkXP::pink300.withAlpha (0.25f));
    g.fillPath (fill);

    // 2) 主曲线（双层描边：浅底 + 深顶，视觉厚度）
    juce::Path curve;
    buildSmoothPath (curve, pts, /*closeToBottom*/ false);
    g.setColour (PinkXP::pink500.withAlpha (0.35f));
    g.strokePath (curve, juce::PathStrokeType (3.0f));
    g.setColour (PinkXP::pink600);
    g.strokePath (curve, juce::PathStrokeType (1.4f));

    // 3) 峰值保持（虚线）—— 同样做 Catmull-Rom 平滑
    if (peakHoldEnabled && (int) peakDb.size() == N)
    {
        std::vector<juce::Point<float>> peakPts;
        peakPts.reserve ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const float x = x0 + (float) i * invMax * xLen;
            const float y = dbToY (peakDb[(size_t) i], canvas);
            peakPts.push_back ({ x, y });
        }
        juce::Path peak;
        buildSmoothPath (peak, peakPts, /*closeToBottom*/ false);

        juce::Path dashed;
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashed, peak, dashes, 2);
        g.setColour (PinkXP::pink700.withAlpha (0.75f));
        g.fillPath (dashed);
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
