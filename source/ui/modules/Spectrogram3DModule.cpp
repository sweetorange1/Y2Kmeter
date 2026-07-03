#include "source/ui/modules/Spectrogram3DModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <algorithm>
#include <array>

// ==========================================================
// Spectrogram3DModule
// ==========================================================

Spectrogram3DModule::Spectrogram3DModule (AnalyserHub& h)
    : ModulePanel (ModuleType::spectrogram3d), hub (h)
{
    hub.retain (AnalyserHub::Kind::Spectrum);
    hub.addFrameListener (this);

    setMinSize     (128, 96);
    setDefaultSize (384, 256);
    setTitleText   ("Spectrogram 3D");

    // ---------- 右侧 SPEED 滑条（样式对齐 SpectrogramModule）----------
    speedSlider.setSliderStyle (juce::Slider::LinearVertical);
    speedSlider.setRange (10.0, 200.0, 1.0);
    speedSlider.setValue ((double) pixelsPerSecond, juce::dontSendNotification);
    speedSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 40, 16);
    speedSlider.setColour (juce::Slider::textBoxTextColourId,       PinkXP::ink);
    speedSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    speedSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    speedSlider.onValueChange = [this]()
    {
        pixelsPerSecond = (float) speedSlider.getValue();
    };

    speedLabel.setJustificationType (juce::Justification::centred);
    speedLabel.setColour (juce::Label::textColourId, PinkXP::ink);
    speedLabel.setFont   (PinkXP::getFont (11.0f, juce::Font::bold));
    speedLabel.setText   ("SPEED", juce::dontSendNotification);

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        speedLabel.setColour  (juce::Label::textColourId,          PinkXP::ink);
        speedSlider.setColour (juce::Slider::textBoxTextColourId,  PinkXP::ink);
        speedLabel.repaint();
        speedSlider.repaint();
    });

    addAndMakeVisible (speedSlider);
    addAndMakeVisible (speedLabel);

    // 预分配历史缓冲
    ensureHistory (defaultHistoryLen);
}

Spectrogram3DModule::~Spectrogram3DModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Spectrum);
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> Spectrogram3DModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    return content.withTrimmedRight (sliderPanelW);
}

void Spectrogram3DModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    auto area = contentBounds;
    auto rightPanel = area.removeFromRight (sliderPanelW);
    juce::ignoreUnused (area);

    auto labelArea = rightPanel.removeFromTop (14);
    speedLabel.setBounds  (labelArea);
    speedSlider.setBounds (rightPanel);
}

void Spectrogram3DModule::resized()
{
    ModulePanel::resized();
}

// ----------------------------------------------------------
// 环形历史缓冲
// ----------------------------------------------------------
void Spectrogram3DModule::ensureHistory (int newLength)
{
    newLength = juce::jmax (32, juce::jmin (400, newLength));

    if (newLength == historyLen
        && (int) historyRing.size() == historyLen
        && ! historyRing.empty()
        && (int) historyRing[0].size() == numBins)
        return;

    historyLen = newLength;
    historyRing.resize ((size_t) historyLen);
    for (auto& row : historyRing)
        row.assign ((size_t) numBins, 0.0f);

    writeIdx   = 0;
    frameCount = 0;
    columnAccumulator = 0.0f;
    lastFrameMs       = 0.0;
}

void Spectrogram3DModule::pushFrame (const juce::Array<float>& mags)
{
    if (historyLen <= 0 || numBins <= 0)
        return;

    auto& row = historyRing[(size_t) writeIdx];
    for (int i = 0; i < numBins; ++i)
        row[(size_t) i] = (i < mags.size()) ? std::abs (mags.getUnchecked (i)) : 0.0f;

    writeIdx   = (writeIdx + 1) % historyLen;
    frameCount = juce::jmin (frameCount + 1, historyLen);
}

// ----------------------------------------------------------
// 投影计算 —— 根据 canvas 尺寸自适配
//
//   坐标系：
//     · 最新切片（depth=0）放在屏幕右下，称为"前方"
//     · 旧切片（depth↑）向左上方偏移，形成 45° 俯视纵深感
//     · 频率轴（低频→高频）从左到右
//     · 幅度映射为竖直高度
//
//   自适应：频率轴占 canvas 宽的 82%，高度占 canvas 高的 40%，
//   斜角偏移填满剩余空间，与历史帧数解耦。
// ----------------------------------------------------------
void Spectrogram3DModule::recomputeProjection (int canvasW, int canvasH)
{
    canvasW = juce::jmax (8, canvasW);
    canvasH = juce::jmax (8, canvasH);

    if (canvasW == lastCanvasW && canvasH == lastCanvasH
        && frameCount == lastCachedFrameCount)
        return;

    const int effLen = juce::jmax (1, frameCount);
    const int depthRows = juce::jmax (1, juce::jmin (effLen, visibleRows));

    // ---- 独立缩放而非 uniform scale ----
    //   频率轴宽度占 canvas 的大部分；高度固定占 canvas 的 heightRatio；
    //   斜角偏移占用剩余空间，确保深度>0 时始终有纵深感。
    constexpr float heightRatio    = 0.40f;   // 幅度高度占 canvas 高的 40%
    constexpr float freqWidthRatio = 0.82f;   // 频率轴占 canvas 宽的 82%，剩余给深度偏移

    const float padL = 8.0f;
    const float padR = 8.0f;

    // 频率轴和深度偏移共享宽度
    const float freqTotalW = ((float) canvasW - padL - padR) * freqWidthRatio;
    const float binWidth   = (numBins > 1) ? freqTotalW / (float) (numBins - 1) : 0.0f;

    // 深度偏移：固定按 visibleRows 计算间距，而非 frameCount。
    //   当 buffer 填满后 500 层中只有最新 150 层落入可见区域，
    //   旧层自然滚动出屏幕顶部，避免被裁剪在画布中间。
    const float depthSpanX = (float) juce::jmax (1, depthRows - 1);
    const float availableForSlantX = ((float) canvasW - padL - padR) - freqTotalW;
    const float slantX = availableForSlantX / depthSpanX;

    const float depthSpanY = (float) juce::jmax (1, depthRows - 1);
    // 地板覆盖近乎全画布高度：从 originY（底）到 0（顶），
    //   无信号时灰色平行四边形将填满整个可见区域；
    //   有信号时峰值堆叠在地板之上，超出顶部由 clipRegion 裁剪。
    const float slantY = (float) (canvasH - 4) / depthSpanY;

    // 幅度高度
    const float maxH = (float) canvasH * heightRatio;

    // 原点（最新切片左下角）
    const float originX = padL;
    const float originY = (float) canvasH - 4.0f;  // 底部留 4px 给标签

    // 写入成员
    projBinWidth = juce::jmax (0.5f, binWidth);
    projSlantX   = juce::jmax (0.3f, slantX);
    projSlantY   = juce::jmax (0.1f, slantY);
    projMaxH     = juce::jmax (1.0f, maxH);
    projOriginX  = originX;
    projOriginY  = originY;

    lastCanvasW = canvasW;
    lastCanvasH = canvasH;
    lastCachedFrameCount = frameCount;
}

// ----------------------------------------------------------
// 频率 → 屏幕 X 坐标
// ----------------------------------------------------------
float Spectrogram3DModule::freqToScreenX (int binIndex, int totalBins) const
{
    if (totalBins <= 1) return projOriginX;
    return projOriginX + (float) binIndex * projBinWidth;
}

// ----------------------------------------------------------
// 强度 t∈[0,1] → 热力图颜色（低→蓝，高→红）
//
//   t=0.00 → 深蓝（无信号）
//   t=0.25 → 青色
//   t=0.50 → 绿色
//   t=0.75 → 黄色
//   t=1.00 → 红色（满幅爆点）
//
//   色彩不依赖主题（蓝→红为固定热力图配色），
//   但 depthFade 会在 paint 侧叠加——旧切片整体向深色 fade。
// ----------------------------------------------------------
juce::Colour Spectrogram3DModule::valueToColour (float t, float depthFade) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    // 六段蓝→青→绿→黄→红热力图
    struct Stop { float t; juce::Colour c; };
    const Stop stops[] = {
        { 0.00f, juce::Colour (4,   4,   36)  },  // 深蓝黑
        { 0.15f, juce::Colour (8,   20,  100) },  // 深蓝
        { 0.30f, juce::Colour (0,   100, 180) },  // 蓝
        { 0.45f, juce::Colour (0,   180, 160) },  // 青
        { 0.60f, juce::Colour (20,  210, 40)  },  // 绿
        { 0.78f, juce::Colour (230, 230, 0)   },  // 黄
        { 0.92f, juce::Colour (240, 80,  0)   },  // 橙
        { 1.00f, juce::Colour (240, 10,  10)  },  // 红
    };
    constexpr int N = (int) (sizeof (stops) / sizeof (stops[0]));

    juce::Colour c = stops[N - 1].c;
    for (int i = 0; i < N - 1; ++i)
    {
        if (t <= stops[i + 1].t)
        {
            const float span = juce::jmax (1.0e-6f, stops[i + 1].t - stops[i].t);
            const float u    = juce::jlimit (0.0f, 1.0f, (t - stops[i].t) / span);
            c = stops[i].c.interpolatedWith (stops[i + 1].c, u);
            break;
        }
    }

    // 深度 fade：旧切片向深色融合（用深蓝黑作为 fade 目标）
    const float fade = juce::jlimit (0.0f, 1.0f, depthFade);
    return c.interpolatedWith (juce::Colour (8, 8, 24), fade * 0.65f);
}

// ----------------------------------------------------------
// onFrame
// ----------------------------------------------------------
void Spectrogram3DModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto plot   = canvas.reduced (2);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    // 让 Hub 直接按 numBins 个对数频率点返回双路合并的线性幅度
    const double nyquist = hub.getSampleRate() * 0.5;
    const float  fMin    = minFreqHz;
    const float  fMax    = juce::jmin (maxFreqHz, (float) (nyquist * 0.95));

    hub.getSpectrumMagnitudesBlended (rowMagBuf, numBins, fMin, fMax);

    // ---- 速度解耦 ----
    const double now = juce::Time::getMillisecondCounterHiRes();
    float dtMs = 0.0f;
    if (lastFrameMs > 0.0)
        dtMs = (float) (now - lastFrameMs);
    lastFrameMs = now;
    dtMs = juce::jlimit (0.0f, 500.0f, dtMs);

    columnAccumulator += dtMs * pixelsPerSecond * 0.001f;
    int nCols = (int) columnAccumulator;
    if (historyLen > 0 && nCols > historyLen) nCols = historyLen;
    if (nCols < 0) nCols = 0;
    columnAccumulator -= (float) nCols;

    if (nCols == 0)
        return;

    // 连续 push nCols 帧（都是当前幅度快照）
    for (int k = 0; k < nCols; ++k)
        pushFrame (rowMagBuf);

    // repaint 节流（P1-1）：限制最短重绘间隔，降低宿主消息线程压力。
    //   默认 384×256 模块下，每帧 150×127≈19k 次 fillRect 即使优化后
    //   在 60fps 仍然吃 CPU。限制到 ~30fps 对视觉流动感几乎无影响。
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if ((nowMs - lastRepaintMs) >= 33.0)
    {
        lastRepaintMs = nowMs;
        repaint();
    }
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
void Spectrogram3DModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    const auto canvas = getCanvasBounds (contentBounds);
    drawBackground (g, canvas);

    const auto plot = canvas.reduced (2);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    // 只有至少 1 帧才值得绘制
    const int effLen = juce::jmax (1, frameCount);
    if (effLen <= 0) return;

    recomputeProjection (plot.getWidth(), plot.getHeight());

    // 原点 offset（投影坐标是相对于 plot 的）
    const float ox = (float) plot.getX();
    const float oy = (float) plot.getY();

    // ---- 从远到近逐层绘制（画家算法）----
    //   最旧的切片在最上方（屏幕 Y 最小），最新切片在最下方（屏幕 Y 最大）。
    //   从上往下画：旧数据先画（被新数据遮挡），新数据后画（盖住后方）。
    //   这样形成「从上方俯视曲面」的视觉。

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (plot);

    const int effRows = juce::jmin (historyLen, juce::jmin (effLen, visibleRows));

    // ---- 预计算 256 级热力图调色板（depthFade=0 基准色）----
    //   每帧计算一次，消除内层循环 19,000 次 valueToColour 调用。
    const juce::Colour fadeTarget (8, 8, 24);
    std::array<juce::Colour, 256> palette;
    for (int i = 0; i < 256; ++i)
        palette[(size_t) i] = valueToColour ((float) i * (1.0f / 255.0f), 0.0f);

    for (int d = effRows - 1; d >= 0; --d)
    {
        // d=effRows-1 → 最旧（上方），d=0 → 最新（下方）
        const int readIdx = (writeIdx - 1 - d + historyLen * 2) % historyLen;
        const auto& row = historyRing[(size_t) readIdx];

        // 深度 fade: 0 = 最新, 1 = 最旧
        const float depthFade = (effRows > 1) ? (float) d / (float) (effRows - 1) : 0.0f;

        // 该层基线的 Y 坐标
        const float baseY = oy + projOriginY - (float) d * projSlantY;

        // 该层基线的 X 偏移
        const float depthX = (float) d * projSlantX;

        // 预计算每个 bin 的 X / topY / 颜色下标（单次遍历，避免重复 gainToDecibels）
        std::array<float, 256> binX, binTopY;
        std::array<int,   256> colIdx;
        jassert (numBins <= 256);
        for (int i = 0; i < numBins; ++i)
        {
            const float mag = row[(size_t) i];
            const float db  = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));
            const float t01 = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
            const int   idx = juce::jlimit (0, 255, (int) std::lround (t01 * 255.0f));

            binX[(size_t) i]    = ox + projOriginX + depthX + (float) i * projBinWidth;
            binTopY[(size_t) i] = baseY - t01 * projMaxH;
            colIdx[(size_t) i]  = idx;
        }

        // ---- 逐段绘制矩形条（fillRect 替代 fillPath，消除 Path 分配开销）----
        //   每个 bin i → i+1 的梯形近似为矩形条：
        //     x = binX[i], yTop = binTopY[i], w = projBinWidth, h = baseY - binTopY[i]
        //   相邻 bin 顶边高度差异仅几像素，用矩形近似在视觉上几乎无差异。
        for (int i = 0; i < numBins - 1; ++i)
        {
            const auto c = palette[(size_t) colIdx[(size_t) i]]
                              .interpolatedWith (fadeTarget, depthFade * 0.65f);
            g.setColour (c);
            const float top = binTopY[(size_t) i];
            const float h   = juce::jmax (0.5f, baseY - top);
            g.fillRect (binX[(size_t) i], top, projBinWidth, h);
        }

        // ---- 绘制顶部轮廓线（强化"曲面脊线"视觉）----
        {
            juce::Path outline;
            outline.startNewSubPath (binX[0], baseY);
            outline.lineTo          (binX[0], binTopY[0]);
            for (int i = 0; i < numBins - 1; ++i)
                outline.lineTo (binX[(size_t) i + 1], binTopY[(size_t) i + 1]);
            outline.lineTo (binX[(size_t) (numBins - 1)], baseY);

            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.strokePath (outline, juce::PathStrokeType (0.6f));
        }
    }

    // 绘制轴标签（浮绘在 3D 视图上方）
    drawAxisLabels (g, plot);
}

// ----------------------------------------------------------
// 绘制底衬
// ----------------------------------------------------------
void Spectrogram3DModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);
}

// ----------------------------------------------------------
// 轴标签 —— 频率标签在底部，older/newer 提示在顶部/左边
// ----------------------------------------------------------
void Spectrogram3DModule::drawAxisLabels (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));

    // 频率标签（浮绘在底部）
    struct L { float hz; const char* label; };
    const L labels[] = {
        {  100.0f, "100" },
        { 1000.0f,  "1k" },
        {10000.0f, "10k" },
    };

    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) minFreqHz;
    const double fMax       = juce::jmin ((double) maxFreqHz, nyquist);

    for (auto& l : labels)
    {
        if ((double) l.hz < fMin || (double) l.hz > fMax) continue;

        const double logA = std::log10 (fMin);
        const double logB = std::log10 (fMax);
        const double t    = (std::log10 ((double) l.hz) - logA) / juce::jmax (1.0e-9, logB - logA);

        // 鼠标位置 = canvas 左边缘 + t * canvasW
        const int x = canvas.getX() + (int) std::round (t * (double) canvas.getWidth());
        const int y = canvas.getBottom() - 13;

        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.fillRect (x - 12, y, 24, 11);
        g.setColour (PinkXP::hl);
        g.drawText (l.label, x - 12, y, 24, 11, juce::Justification::centred, false);
    }

    // older/newer 提示（浮绘在左上和右下）
    {
        g.setColour (PinkXP::ink.withAlpha (0.45f));

        const int ow = 56, oh = 11;
        g.fillRect (canvas.getX() + 2,                canvas.getY() + 2, ow, oh);
        g.fillRect (canvas.getRight() - ow - 2,       canvas.getBottom() - oh - 2, ow, oh);

        g.setColour (PinkXP::hl);
        g.drawText (juce::String::fromUTF8 ("\xe2\x86\x96 older"),
                    canvas.getX() + 2, canvas.getY() + 2, ow, oh,
                    juce::Justification::centred, false);
        g.drawText (juce::String::fromUTF8 ("newer \xe2\x86\x98"),
                    canvas.getRight() - ow - 2, canvas.getBottom() - oh - 2, ow, oh,
                    juce::Justification::centred, false);
    }
}