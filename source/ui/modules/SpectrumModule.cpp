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

    rawMags.ensureStorageAllocated (256);
    smoothedDb .reserve (256);
    peakDb     .reserve (256);
    peakHoldMs .reserve (256);
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
// 坐标换算
//
//   新设计：X 轴 = 132 个等宽半音格（C0~B10）。
//     index ∈ [0, 131] ··· 0 = C0, 12 = C1, ..., 131 = B10
//     不再使用 "freqToX/log10" 坐标 —— 同一个音符占 1 格，方便扚谱。
// ----------------------------------------------------------
float SpectrumModule::noteToX (int note, juce::Rectangle<int> canvas)
{
    const int N = AnalyserHub::kNoteBinCount;
    const float t = (float) (note + 0.5f) / (float) N;
    return (float) canvas.getX() + t * (float) canvas.getWidth();
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
// onFrame —— 拿 132 半音格数据
// ----------------------------------------------------------
void SpectrumModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    // 分频 repaint（与原设计一致）
    const bool skipRepaintThisTick = ((frame.tickCount % 2ull) != 0ull);

    // ★★ 直接从 FrameSnapshot 拿fame·spectrumByNote·—— 132 个半音格的线性幅度。
    //    不再调用 hub·getSpectrumMagnitudesBlended，不再做任何重采样。
    constexpr int N = AnalyserHub::kNoteBinCount;
    rawMags.resize (N);
    std::memcpy (rawMags.getRawDataPointer(), frame.spectrumByNote.data(),
                 sizeof (float) * (size_t) N);

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

    // repaint 节流
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float  scale  = (float) juce::jmax (1.0, (double) juce::Component::getApproximateScaleFactorForComponent (this));
    const double minRepaintIntervalMs = 16.0 * (double) juce::jmin (1.8f, scale);
    if ((nowMs - lastRepaintMs) < minRepaintIntervalMs)
        return;

    if (skipRepaintThisTick)
        return;

    lastRepaintMs = nowMs;
    repaint();
}

// ----------------------------------------------------------
// rebuildDisplay —— 132 个半音格线性幅度 → 132 个 dB 值（可Slope）。
//   不再做频率重采样。“列间平滑"不再需要（每格本身就是一个独立音符，
//   横向平滑会让邙近音符能量互相渗透，反而损害扚谱体验）——这里只保留上升快下降慢的时间平滑。
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
        blurredDb  .assign(N, minDb);
    }

    ensureDisplayCache (N);

    for (int col = 0; col < N; ++col)
    {
        const float mag = std::abs (rawMags.getUnchecked (col));
        float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));

        // Slope 补偿：4.5 dB/oct 高频提升（基准 1kHz）
        if (slopeEnabled && (int) slopeOffsetDb.size() == N)
            db += slopeOffsetDb[(size_t) col];

        // 时间平滑：上升快，下降慢
        const float prev  = smoothedDb[(size_t) col];
        const float alpha = (db > prev) ? 0.55f : 0.12f;
        float sm = prev + alpha * (db - prev);
        sm = juce::jlimit (minDb, maxDb + 12.0f, sm);
        smoothedDb[(size_t) col] = sm;
    }
    // 注意：不再做列间 [1,2,1] 平滑——保持每个音符独立起伏，扚谱更清晰。
}

void SpectrumModule::ensureDisplayCache (int numPoints)
{
    numPoints = juce::jmax (2, numPoints);

    if (slopeCacheSize == numPoints && (int) slopeOffsetDb.size() == numPoints)
        return;

    slopeOffsetDb.resize ((size_t) numPoints);

    // 按半音格索引计算 Slope：每个半音 = (1/12) 八度，基准 1kHz ≈ A5(MIDI 81) 附近。
    // 3.0 dB/oct 高频提升（原本是 4.5 dB/oct，但在三段多分辨率 FFT 下，
    //   过强的高频提升会把高音区残留的窗函数旁瓣放大成肉眼可见的暗亮线，
    //   这里调到 3.0 dB/oct 折中：仍然抬高高频可读性，但不再放大伪影）。
    // 1000Hz 对应的 noteIndex：log2(1000/16.3516)*12 ≈ 71.4 (介B5/C6)
    constexpr float refNoteIndex = 71.4f;
    for (int n = 0; n < numPoints; ++n)
    {
        const float octaves = ((float) n - refNoteIndex) / 12.0f;
        slopeOffsetDb[(size_t) n] = 3.0f * octaves;
    }

    slopeCacheSize = numPoints;
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
// 网格：每 20dB 一条横线，以及每个 C 音（n%12==0）一条纵线
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

    // 纵线：每个 C 音（八度分界）画一条；中央 A4 附近画一条稍亮的参考线
    const int N = AnalyserHub::kNoteBinCount;
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    for (int n = 0; n < N; n += 12) // C0, C1, ..., C10
    {
        const int x = (int) std::round (noteToX (n, canvas));
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
    g.setColour (PinkXP::pink300.withAlpha (0.25f));
    g.fillPath (fillPath);

    // 2) 主曲线（双层描边：浅底 + 深顶，视觉厚度）
    curvePath.clear();
    buildSmoothPath (curvePath, curvePts, /*closeToBottom*/ false);
    g.setColour (PinkXP::pink500.withAlpha (0.35f));
    g.strokePath (curvePath, juce::PathStrokeType (3.0f));
    g.setColour (PinkXP::pink600);
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
        g.setColour (PinkXP::pink700.withAlpha (0.75f));
        g.fillPath (dashedPeakPath);
    }
}

// ----------------------------------------------------------
// 坐标轴标签（左侧 dB / 底部音符名）
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

    // 底部音符名标签：每个 C 音画 C0 / C1 / ... / C10
    const int N = AnalyserHub::kNoteBinCount;
    for (int n = 0; n < N; n += 12)
    {
        const int octave = n / 12;          // 0→10
        const int x = (int) std::round (noteToX (n, canvas));
        if (x < canvas.getX() - 4 || x > canvas.getRight() + 4) continue;
        const juce::String label = "C" + juce::String (octave);
        g.drawText (label, x - 14, canvas.getBottom() + 2, 28, 12,
                    juce::Justification::centred, false);
    }
}
