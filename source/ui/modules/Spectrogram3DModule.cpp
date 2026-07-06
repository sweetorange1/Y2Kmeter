#include "source/ui/modules/Spectrogram3DModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <algorithm>
#include <array>

// ==========================================================
// Spectrogram3DModule
// (force rebuild: v1.9.4 P7 canvas bg cache)
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

    // 预分配历史缓冲 + 幅度→色板 LUT
    buildMagLut();
    ensureHistory (defaultHistoryLen);
    depthPalettes.resize (visibleRows);
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
    lastFrameMs       = 0.0f;
    depthPalettesDirty = true;
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
    depthPalettesDirty   = true; // 投影变化 → 深度色板需要重建
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
    imageCacheDirty = true;

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
// paintContent —— 离屏 Image 缓存版（P2+P4）
//
//   P2 性能优化：将 3D 视图渲染到离屏 juce::Image，paintContent
//   只需一次 g.drawImage。无论 macOS CoreGraphics 软光栅还是
//   Windows OpenGL，单次位图 blit 都远快于逐层 38,100 次 fillRect
//   + 300 次 strokePath 的分散绘制。
//
//   P4 动态分辨率：当 canvas 对角线超过 900px 时，renderToImage
//   以低于 1:1 的分辨率渲染，paintContent 用 drawImage 放大到屏上
//   尺寸。像素风 UI 对此类上采样高度宽容，主观质量几乎无损。
// ----------------------------------------------------------
void Spectrogram3DModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    const auto canvas = getCanvasBounds (contentBounds);
    const auto plot   = canvas.reduced (2);

    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    const int effLen = juce::jmax (1, frameCount);
    if (effLen <= 0) return;

    // 离屏 Image 缓存：仅在数据脏或 canvas 尺寸变化时重建
    if (imageCacheDirty || cached3DImage.isNull()
        || cachedCanvasW != canvas.getWidth()
        || cachedCanvasH != canvas.getHeight())
    {
        renderToImage (canvas);
        imageCacheDirty = false;
    }

    // P4: 将降分辨率 Image 放大到 canvas 尺寸（单次 drawImage 完成上采样 blit）
    g.drawImage (cached3DImage,
                 canvas.getX(), canvas.getY(), canvas.getWidth(), canvas.getHeight(),
                 0, 0, cached3DImage.getWidth(), cached3DImage.getHeight());

    // 轴标签浮绘在 3D 视图上方（不进入 Image，保证文字清晰度）
    drawAxisLabels (g, plot);
}

// ----------------------------------------------------------
// buildMagLut —— 预计算 4096 级 mag → 色板下标
//
//   每帧原本需要 128 bins × visibleRows 次 gainToDecibels (log10)
//   + jlimit + lround，共 ~19K 次昂贵浮点运算。改为查表后仅剩
//   一次整数索引，在 Apple Silicon / x86 上均快到可忽略。
// ----------------------------------------------------------
void Spectrogram3DModule::buildMagLut()
{
    for (int i = 0; i < 4096; ++i)
    {
        const float mag = (float) i * (1.0f / 4095.0f);
        const float db  = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));
        const float t01 = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        magToIdx[(size_t) i] = (uint8_t) juce::jlimit (0, 255, (int) std::lround (t01 * 255.0f));
    }
}

// ----------------------------------------------------------
// rebuildDepthPalettes —— 预计算逐层深度 fade 色板
//
//   基准色板（256 级）已由 valueToColour 生成；本函数为每层
//   叠加深度 fade 后存为 depthPalettes[d][256]。
//   仅在 effRows 变化（投影 recalc 引起）时调用一次，
//   之后每帧直接 depthPalettes[d][colIdx] 取值即可。
// ----------------------------------------------------------
void Spectrogram3DModule::rebuildDepthPalettes (int nRows)
{
    nRows = juce::jmax (1, juce::jmin (nRows, visibleRows));
    if (nRows == depthPalettesRows && ! depthPalettesDirty)
        return;

    const juce::Colour fadeTarget (8, 8, 24);
    // 先算基准色板（depthFade=0）
    std::array<juce::Colour, 256> basePalette;
    for (int i = 0; i < 256; ++i)
        basePalette[(size_t) i] = valueToColour ((float) i * (1.0f / 255.0f), 0.0f);

    for (int d = 0; d < nRows; ++d)
    {
        const float depthFade = (nRows > 1) ? (float) d / (float) (nRows - 1) : 0.0f;
        const float fadeAmt   = depthFade * 0.65f;
        for (int i = 0; i < 256; ++i)
            depthPalettes[(size_t) d][(size_t) i] =
                basePalette[(size_t) i].interpolatedWith (fadeTarget, fadeAmt);
    }

    depthPalettesRows  = nRows;
    depthPalettesDirty = false;
}

// ----------------------------------------------------------
// renderToImage —— 将完整 3D 视图渲染到离屏 Image（P3+P4 优化版）
//
//   P3 优化（已内化）：
//     1) magToIdx LUT     → 消除 19,200 次 gainToDecibels/log10
//     2) depthPalettes    → 消除 19,050 次 interpolatedWith
//     3) Image 复用       → 不再每帧 malloc
//
//   P4 动态分辨率：
//     canvas 对角线 ≤ 900px → 1:1 渲染（零质量损失）
//     超出则 scale = 900/diag，下限 35%，大幅降低大窗口 fillRect 像素量
//     所有坐标通过 AffineTransform::scale 自动缩放到实际 Image，代码无需改动
// ----------------------------------------------------------
void Spectrogram3DModule::renderToImage (juce::Rectangle<int> canvas)
{
    const int cw = canvas.getWidth();
    const int ch = canvas.getHeight();
    if (cw <= 0 || ch <= 0) return;

    const int effLen = juce::jmax (1, frameCount);
    if (effLen <= 0) return;

    // P4: 动态渲染分辨率 —— 对角线 ≤ 900px 时 1:1，超出反比降采样
    const float diag  = std::sqrt ((float) (cw * cw + ch * ch));
    const float scale = juce::jlimit (0.35f, 1.0f, 900.0f / juce::jmax (900.0f, diag));
    cachedRenderScale = scale;
    cachedCanvasW     = cw;
    cachedCanvasH     = ch;

    const int rw = juce::jmax (8, (int) ((float) cw * scale));
    const int rh = juce::jmax (8, (int) ((float) ch * scale));

    // 复用 Image：尺寸匹配时只清空，避免 malloc
    if (cached3DImage.isNull()
        || cached3DImage.getWidth()  != rw
        || cached3DImage.getHeight() != rh)
    {
        cached3DImage = juce::Image (juce::Image::ARGB, rw, rh, true);
    }
    else
    {
        cached3DImage.clear (cached3DImage.getBounds());
    }

    juce::Graphics ig (cached3DImage);
    // P4: 缩放变换 —— 后续所有 fillRect/strokePath 用全尺寸 cw×ch 坐标，
    //     自动等比缩放到 rw×rh 的实际 Image 像素
    ig.addTransform (juce::AffineTransform::scale (scale));

    // 底衬（Image-local 坐标 → 由变换转为 scaled 坐标）
    juce::Rectangle<int> imgCanvas (0, 0, cw, ch);
    drawBackground (ig, imgCanvas);

    const auto plot = imgCanvas.reduced (2);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    recomputeProjection (plot.getWidth(), plot.getHeight());

    const int effRows = juce::jmin (historyLen, juce::jmin (effLen, visibleRows));

    // 深度色板只在投影/行数变化时重建
    if (depthPalettesDirty || depthPalettesRows != effRows)
        rebuildDepthPalettes (effRows);

    const float ox = (float) plot.getX();
    const float oy = (float) plot.getY();

    juce::Graphics::ScopedSaveState ss (ig);
    ig.reduceClipRegion (plot);

    for (int d = effRows - 1; d >= 0; --d)
    {
        const int readIdx = (writeIdx - 1 - d + historyLen * 2) % historyLen;
        const auto& row = historyRing[(size_t) readIdx];

        const float baseY  = oy + projOriginY - (float) d * projSlantY;
        const float depthX = (float) d * projSlantX;

        std::array<float, 256> binX, binTopY;
        std::array<int,   256> colIdx;
        jassert (numBins <= 256);
        for (int i = 0; i < numBins; ++i)
        {
            // P3: magToIdx LUT 替代 gainToDecibels + jlimit + lround
            const float mag = row[(size_t) i];
            const int   mi  = juce::jlimit (0, 4095, (int) (mag * 4095.0f));
            const int   idx = (int) magToIdx[(size_t) mi];

            binX[(size_t) i]    = ox + projOriginX + depthX + (float) i * projBinWidth;
            binTopY[(size_t) i] = baseY - (float) idx * (1.0f / 255.0f) * projMaxH;
            colIdx[(size_t) i]  = idx;
        }

        for (int i = 0; i < numBins - 1; ++i)
        {
            // P3: 深度色板直接查表，替代 interpolatedWith
            ig.setColour (depthPalettes[(size_t) d][(size_t) colIdx[(size_t) i]]);
            const float top = binTopY[(size_t) i];
            const float h   = juce::jmax (0.5f, baseY - top);
            ig.fillRect (binX[(size_t) i], top, projBinWidth, h);
        }

        // 顶部轮廓线
        {
            juce::Path outline;
            outline.startNewSubPath (binX[0], baseY);
            outline.lineTo          (binX[0], binTopY[0]);
            for (int i = 0; i < numBins - 1; ++i)
                outline.lineTo (binX[(size_t) i + 1], binTopY[(size_t) i + 1]);
            outline.lineTo (binX[(size_t) (numBins - 1)], baseY);

            ig.setColour (juce::Colours::white.withAlpha (0.25f));
            ig.strokePath (outline, juce::PathStrokeType (0.6f));
        }
    }
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

        // 频率轴在屏幕上的实际范围：
        //   起点 = canvas.getX() + projOriginX
        //   宽度 = projBinWidth × (numBins-1)（与 recomputeProjection 的 freqTotalW 等价）
        // 不使用 canvas 全宽，因为深度偏移占用了剩余 ~18% 宽度
        const float freqAxisW = projBinWidth * (float) (numBins - 1);
        const int x = canvas.getX() + (int) std::round ((double) projOriginX + t * (double) freqAxisW);
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
        g.drawText (juce::String::fromUTF8 ("\xe2\x86\x97 older"),
                    canvas.getX() + 2, canvas.getY() + 2, ow, oh,
                    juce::Justification::centred, false);
        g.drawText (juce::String::fromUTF8 ("newer \xe2\x86\x99"),
                    canvas.getRight() - ow - 2, canvas.getBottom() - oh - 2, ow, oh,
                    juce::Justification::centred, false);
    }
}