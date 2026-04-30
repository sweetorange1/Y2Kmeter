#include "source/ui/modules/OscilloscopeModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// OscilloscopeModule —— Pink XP 像素风立体声示波器
// ==========================================================

OscilloscopeModule::OscilloscopeModule(AnalyserHub& h)
    : ModulePanel(ModuleType::oscilloscope),
      hub(h)
{
    // Phase F：本模块只需要 Oscilloscope 一路 + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // oscilloscope 6×4 大格

    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        invalidateStaticLayer();
        invalidateDynamicLayer();
        repaint();
    });
    auto setupModeBtn = [this](juce::TextButton& b)
    {
        b.setClickingTogglesState(true);
        b.setRadioGroupId(0x50424551); // "PBEQ"
        addAndMakeVisible(b);
    };
    setupModeBtn(btnWave);
    setupModeBtn(btnXY);
    setupModeBtn(btnLiss);

    btnWave.onClick = [this]() { setDisplayMode(DisplayMode::waveform);  };
    btnXY  .onClick = [this]() { setDisplayMode(DisplayMode::xy);        };
    btnLiss.onClick = [this]() { setDisplayMode(DisplayMode::lissajous); };

    btnFreeze.setClickingTogglesState(true);
    btnFreeze.onClick = [this]() { setFrozen(btnFreeze.getToggleState()); };
    addAndMakeVisible(btnFreeze);

    btnWave.setToggleState(true, juce::dontSendNotification);
}

OscilloscopeModule::~OscilloscopeModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Oscilloscope);
}

void OscilloscopeModule::setDisplayMode(DisplayMode m)
{
    if (displayMode == m) return;
    displayMode = m;
    refreshModeButtons();
    invalidateStaticLayer();
    invalidateDynamicLayer();
    repaint();
}

void OscilloscopeModule::setFrozen(bool b)
{
    if (frozen == b) return;
    frozen = b;
    btnFreeze.setToggleState(frozen, juce::dontSendNotification);
    invalidateDynamicLayer(); // 冻结状态右上角红点 + FROZEN 文本需要重绘
    repaint();
}

void OscilloscopeModule::refreshModeButtons()
{
    btnWave.setToggleState(displayMode == DisplayMode::waveform,  juce::dontSendNotification);
    btnXY  .setToggleState(displayMode == DisplayMode::xy,        juce::dontSendNotification);
    btnLiss.setToggleState(displayMode == DisplayMode::lissajous, juce::dontSendNotification);
}

// ----------------------------------------------------------
// onFrame：Hub 分发器回调（除非冻结）
// ----------------------------------------------------------
void OscilloscopeModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    if (! frozen)
    {
        // Phase F 优化：std::array → juce::Array 改 memcpy 批量拷贝
        const int n = (int) frame.oscL.size();
        snapshotL.resize(n);
        snapshotR.resize(n);
        std::memcpy (snapshotL.getRawDataPointer(), frame.oscL.data(), (size_t) n * sizeof (float));
        std::memcpy (snapshotR.getRawDataPointer(), frame.oscR.data(), (size_t) n * sizeof (float));

        // 方案 A：只有数据真正变化才标脏动态层，避免每帧 4 次 strokePath
        if (snapshotChangedSinceLastDraw())
            dynamicLayerDirty = true;
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float scale  = (float) juce::jmax (1.0, (double) juce::Component::getApproximateScaleFactorForComponent (this));
    const double minRepaintIntervalMs = 15.0 * (double) juce::jmin (2.0f, scale);
    if ((nowMs - lastRepaintMs) < minRepaintIntervalMs)
        return;

    lastRepaintMs = nowMs;
    repaint();
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> OscilloscopeModule::getToolbarBounds(juce::Rectangle<int> content) const
{
    return content.withHeight(toolbarH);
}

juce::Rectangle<int> OscilloscopeModule::getCanvasBounds(juce::Rectangle<int> content) const
{
    return content.withTrimmedTop(toolbarH + 4);
}

void OscilloscopeModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto tb = getToolbarBounds(contentBounds).reduced(2);
    const int gap = 4;
    const int modeBtnW = 60;

    btnWave  .setBounds(tb.getX(),                        tb.getY(), modeBtnW, tb.getHeight());
    btnXY    .setBounds(tb.getX() + (modeBtnW + gap),     tb.getY(), modeBtnW, tb.getHeight());
    btnLiss  .setBounds(tb.getX() + (modeBtnW + gap) * 2, tb.getY(), modeBtnW, tb.getHeight());

    const int freezeW = 64;
    btnFreeze.setBounds(tb.getRight() - freezeW, tb.getY(), freezeW, tb.getHeight());
}

// ----------------------------------------------------------
// 绘制
// ----------------------------------------------------------
void OscilloscopeModule::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    // 内容区底色（深色主题下也保持浅色，保证按钮文字可读）
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    rebuildStaticLayerIfNeeded(contentBounds);
    drawStaticLayer(g, contentBounds);

    // 画布区域（凹陷）
    auto canvas = getCanvasBounds(contentBounds);
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    // 方案 A：动态层缓存
    //   · 只有尺寸变化 / 模式变化 / 冻结状态变化 / snapshot 有新数据时才重绘动态层
    //   · 否则直接 drawImageAt，避开 4 次 strokePath 与上千次 fillRect
    redrawDynamicLayerIfNeeded(canvas);
    if (dynamicLayer.isValid())
        g.drawImageAt(dynamicLayer, canvas.getX(), canvas.getY());
}

// ----------------------------------------------------------
// 背景：凹陷画布 + 像素网格
// ----------------------------------------------------------
void OscilloscopeModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken(g, canvas, PinkXP::content);

    auto inner = canvas.reduced(2);
    if (inner.isEmpty()) return;

    // 像素点阵（8px 间距淡粉点阵）
    g.setColour(PinkXP::pink200.withAlpha(0.35f));
    const int dotStep = 8;
    for (int y = inner.getY(); y < inner.getBottom(); y += dotStep)
        for (int x = inner.getX() + ((y / dotStep) % 2) * (dotStep / 2);
             x < inner.getRight(); x += dotStep)
            g.fillRect(x, y, 1, 1);

    // 中心十字线
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    const int cx = inner.getCentreX();
    const int cy = inner.getCentreY();
    g.drawHorizontalLine(cy, (float) inner.getX(), (float) inner.getRight());
    g.drawVerticalLine  (cx, (float) inner.getY(), (float) inner.getBottom());

    // 1/4 / 3/4 虚线（垂直）
    g.setColour(PinkXP::pink200.withAlpha(0.5f));
    const int q1x = inner.getX() + inner.getWidth() / 4;
    const int q3x = inner.getX() + inner.getWidth() * 3 / 4;
    for (int y = inner.getY(); y < inner.getBottom(); y += 4)
    {
        g.fillRect(q1x, y, 1, 2);
        g.fillRect(q3x, y, 1, 2);
    }
}

// ----------------------------------------------------------
// Waveform 模式：时域 L（粉色）+ R（深粉）叠加
// ----------------------------------------------------------
void OscilloscopeModule::drawWaveform(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    const int N = snapshotL.size();
    if (N <= 1) return;

    auto inner = canvas.reduced(4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const float h  = (float) inner.getHeight();
    const float cy = (float) inner.getCentreY();
    const float halfH = h * 0.45f;

    // L 声道：浅粉描边（先淡描 glow，再实线）
    auto pathL = buildWaveformPath(snapshotL, inner, cy, halfH);
    g.setColour(PinkXP::pink300.withAlpha(0.35f));
    g.strokePath(pathL, juce::PathStrokeType(3.0f));
    g.setColour(PinkXP::pink400);
    g.strokePath(pathL, juce::PathStrokeType(1.4f));

    // R 声道：深粉描边
    if (snapshotR.size() == N)
    {
        auto pathR = buildWaveformPath(snapshotR, inner, cy, halfH);
        g.setColour(PinkXP::pink500.withAlpha(0.35f));
        g.strokePath(pathR, juce::PathStrokeType(3.0f));
        g.setColour(PinkXP::pink600);
        g.strokePath(pathR, juce::PathStrokeType(1.4f));
    }

    // 左下角说明
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    g.drawText("L", canvas.getX() + 4, canvas.getBottom() - 14,
               24, 12, juce::Justification::centredLeft, false);
    g.setColour(PinkXP::pink400);
    g.fillRect(canvas.getX() + 14, canvas.getBottom() - 9, 8, 2);

    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.drawText("R", canvas.getX() + 32, canvas.getBottom() - 14,
               24, 12, juce::Justification::centredLeft, false);
    g.setColour(PinkXP::pink600);
    g.fillRect(canvas.getX() + 42, canvas.getBottom() - 9, 8, 2);
}

// ----------------------------------------------------------
// XY / Lissajous 模式：散点云
// ----------------------------------------------------------
void OscilloscopeModule::drawXY(juce::Graphics& g, juce::Rectangle<int> canvas, bool rotate45)
{
    const int N = juce::jmin(snapshotL.size(), snapshotR.size());
    if (N <= 1) return;

    auto inner = canvas.reduced(6);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getCentreY();
    const float radius = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.45f;

    // 边界圆（轻）
    g.setColour(PinkXP::pink300.withAlpha(0.45f));
    g.drawEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.0f);

    const int targetPoints = juce::jmax(96, inner.getWidth() * 2);
    const int stride = juce::jmax(1, N / targetPoints);

    // 方案 C：按 alpha 分桶，合并 setColour + fillRect 调用
    //   原本每个点都要 setColour + fillRect，600px 宽画布 ~1200 次状态切换；
    //   按 8 档 alpha 把点汇总到 RectangleList，一次 fillRectList 提交 → 最多 8 次状态切换。
    constexpr int kAlphaBuckets = 8;
    juce::RectangleList<float> buckets[kAlphaBuckets];
    for (int b = 0; b < kAlphaBuckets; ++b)
        buckets[b].ensureStorageAllocated(targetPoints / kAlphaBuckets + 8);

    const float denom = (float) juce::jmax(1, N - 1);
    const juce::Rectangle<float> innerF = inner.toFloat().expanded(1.0f);

    for (int i = 0; i < N; i += stride)
    {
        const float rawL = snapshotL.getUnchecked(i);
        const float rawR = snapshotR.getUnchecked(i);
        if (! std::isfinite(rawL) || ! std::isfinite(rawR))
            continue;

        const float lx = juce::jlimit(-1.0f, 1.0f, rawL);
        const float ry = juce::jlimit(-1.0f, 1.0f, rawR);

        float px, py;
        if (rotate45)
        {
            const float mid  = (lx + ry) * 0.7071067f;
            const float side = (lx - ry) * 0.7071067f;
            px = cx + side * radius;
            py = cy - mid  * radius;
        }
        else
        {
            px = cx + lx * radius;
            py = cy - ry * radius;
        }

        if (! innerF.contains(px, py))
            continue;

        // 越新的点（i 越大）越亮 —— 0.25f ~ 0.85f 之间映射到 8 档桶
        const float normAlpha = (float) i / denom;               // 0..1
        int bucket = (int) (normAlpha * (float) kAlphaBuckets);
        bucket = juce::jlimit(0, kAlphaBuckets - 1, bucket);

        buckets[bucket].addWithoutMerging({ px - 0.5f, py - 0.5f, 2.0f, 2.0f });
    }

    for (int b = 0; b < kAlphaBuckets; ++b)
    {
        if (buckets[b].isEmpty()) continue;
        const float bucketAlpha = 0.25f + 0.6f * (((float) b + 0.5f) / (float) kAlphaBuckets);
        g.setColour(PinkXP::pink500.withAlpha(bucketAlpha));
        g.fillRectList(buckets[b]);
    }

    // 左下角模式说明
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    const juce::String hint = rotate45 ? "M/S" : "X=L  Y=R";
    g.drawText(hint, canvas.getX() + 4, canvas.getBottom() - 14,
               80, 12, juce::Justification::centredLeft, false);
}

juce::Path OscilloscopeModule::buildWaveformPath(const juce::Array<float>& samples,
                                                 juce::Rectangle<int> inner,
                                                 float yCenter,
                                                 float halfHeight) const
{
    juce::Path path;
    const int len = samples.size();
    if (len <= 1 || inner.getWidth() <= 1)
        return path;

    const int targetPoints = juce::jmax(64, inner.getWidth() * 2);
    const int stride = juce::jmax(1, len / targetPoints);

    bool started = false;
    for (int i = 0; i < len; i += stride)
    {
        const float raw = samples.getUnchecked(i);
        const float s = std::isfinite(raw) ? juce::jlimit(-1.0f, 1.0f, raw) : 0.0f;
        const float norm = (float) i / (float) juce::jmax(1, len - 1);
        const float x = (float) inner.getX() + norm * (float) inner.getWidth();
        const float y = yCenter - s * halfHeight;
        if (! started)
        {
            path.startNewSubPath(x, y);
            started = true;
        }
        else
        {
            path.lineTo(x, y);
        }
    }

    if ((len - 1) % stride != 0)
    {
        const float raw = samples.getUnchecked(len - 1);
        const float s = std::isfinite(raw) ? juce::jlimit(-1.0f, 1.0f, raw) : 0.0f;
        const float x = (float) inner.getRight();
        const float y = yCenter - s * halfHeight;
        if (! started)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }

    return path;
}

void OscilloscopeModule::rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds)
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

    auto canvas = getCanvasBounds(juce::Rectangle<int>(0, 0,
                                                       contentBounds.getWidth(),
                                                       contentBounds.getHeight()));
    drawBackground(sg, canvas);
}

void OscilloscopeModule::drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const
{
    if (! staticLayer.isValid() || contentBounds.isEmpty())
        return;

    g.drawImageAt(staticLayer, contentBounds.getX(), contentBounds.getY());
}

void OscilloscopeModule::invalidateStaticLayer()
{
    staticLayer = juce::Image();
    staticLayerContentBounds = {};
}

// ----------------------------------------------------------
// 方案 A：动态波形 / XY 点图缓存层
// ----------------------------------------------------------
void OscilloscopeModule::invalidateDynamicLayer()
{
    dynamicLayer = juce::Image();
    dynamicLayerCanvasBounds = {};
    dynamicLayerDirty = true;
    lastDrawnSampleCount = -1;
}

bool OscilloscopeModule::snapshotChangedSinceLastDraw() const noexcept
{
    const int n = snapshotL.size();
    if (n != lastDrawnSampleCount)
        return true;
    if (n <= 0)
        return false;

    // 取 6 个代表点（首、1/4、1/2、3/4、末 L + 末 R）做快速指纹
    const int i0 = 0;
    const int i1 = juce::jlimit(0, n - 1, n / 4);
    const int i2 = juce::jlimit(0, n - 1, n / 2);
    const int i3 = juce::jlimit(0, n - 1, (3 * n) / 4);
    const int i4 = n - 1;

    const float f0 = snapshotL.getUnchecked(i0);
    const float f1 = snapshotL.getUnchecked(i1);
    const float f2 = snapshotL.getUnchecked(i2);
    const float f3 = snapshotL.getUnchecked(i3);
    const float f4 = snapshotL.getUnchecked(i4);
    const float f5 = (snapshotR.size() > i4) ? snapshotR.getUnchecked(i4) : 0.0f;

    return f0 != lastDrawnFingerprint[0]
        || f1 != lastDrawnFingerprint[1]
        || f2 != lastDrawnFingerprint[2]
        || f3 != lastDrawnFingerprint[3]
        || f4 != lastDrawnFingerprint[4]
        || f5 != lastDrawnFingerprint[5];
}

void OscilloscopeModule::redrawDynamicLayerIfNeeded(juce::Rectangle<int> canvas)
{
    if (canvas.getWidth() <= 0 || canvas.getHeight() <= 0)
    {
        invalidateDynamicLayer();
        return;
    }

    const bool sizeChanged = (! dynamicLayer.isValid())
                          || dynamicLayer.getWidth()  != canvas.getWidth()
                          || dynamicLayer.getHeight() != canvas.getHeight();

    const bool modeChanged   = (lastDrawnMode   != displayMode);
    const bool frozenChanged = (lastDrawnFrozen != frozen);

    if (sizeChanged)
    {
        dynamicLayer = juce::Image(juce::Image::ARGB,
                                   canvas.getWidth(),
                                   canvas.getHeight(),
                                   true);
        dynamicLayerDirty = true;
    }

    if (modeChanged || frozenChanged)
        dynamicLayerDirty = true;

    dynamicLayerCanvasBounds = canvas;

    if (! dynamicLayerDirty)
        return;

    // 清空整张动态层（ARGB 透明底），再在 "局部 canvas 坐标系" 里重绘波形
    dynamicLayer.clear(dynamicLayer.getBounds(), juce::Colours::transparentBlack);

    juce::Graphics dg(dynamicLayer);
    const juce::Rectangle<int> localCanvas(0, 0, canvas.getWidth(), canvas.getHeight());

    switch (displayMode)
    {
        case DisplayMode::waveform:  drawWaveform(dg, localCanvas);        break;
        case DisplayMode::xy:        drawXY      (dg, localCanvas, false); break;
        case DisplayMode::lissajous: drawXY      (dg, localCanvas, true);  break;
    }

    // 冻结标记（右上角小红点 + "FROZEN"）也画进动态层
    if (frozen)
    {
        auto dot = juce::Rectangle<int>(localCanvas.getRight() - 12, localCanvas.getY() + 6, 8, 8);
        dg.setColour(juce::Colour(0xffec4d85));
        dg.fillEllipse(dot.toFloat());
        dg.setColour(PinkXP::ink);
        dg.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        dg.drawText("FROZEN", localCanvas.getRight() - 60, localCanvas.getY() + 4,
                    42, 12, juce::Justification::centredRight, false);
    }

    // 更新指纹 / 状态缓存
    lastDrawnSampleCount = snapshotL.size();
    if (lastDrawnSampleCount > 0)
    {
        const int n = lastDrawnSampleCount;
        const int i1 = juce::jlimit(0, n - 1, n / 4);
        const int i2 = juce::jlimit(0, n - 1, n / 2);
        const int i3 = juce::jlimit(0, n - 1, (3 * n) / 4);
        lastDrawnFingerprint[0] = snapshotL.getUnchecked(0);
        lastDrawnFingerprint[1] = snapshotL.getUnchecked(i1);
        lastDrawnFingerprint[2] = snapshotL.getUnchecked(i2);
        lastDrawnFingerprint[3] = snapshotL.getUnchecked(i3);
        lastDrawnFingerprint[4] = snapshotL.getUnchecked(n - 1);
        lastDrawnFingerprint[5] = (snapshotR.size() > 0)
                                    ? snapshotR.getUnchecked(snapshotR.size() - 1)
                                    : 0.0f;
    }
    lastDrawnMode     = displayMode;
    lastDrawnFrozen   = frozen;
    dynamicLayerDirty = false;
}