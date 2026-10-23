#include "source/ui/modules/OscilloscopeWaveModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// OscilloscopeWaveModule —— 波形示波器（L / R / Both 通道选择）
// ==========================================================

OscilloscopeWaveModule::OscilloscopeWaveModule(AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::oscilloscopeWave),
      hub(h)
{
    // Phase F：订阅 Oscilloscope + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256);
    setTitleText("Oscilloscope Wave");

    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        invalidateStaticLayer();
        invalidateDynamicLayer();
        buildGlSnapshot();   // GL 快照含底色/几何颜色，主题切换必须重建
        repaint();
    });

    auto setupChannelBtn = [this](juce::TextButton& b)
    {
        b.setClickingTogglesState(true);
        b.setRadioGroupId(0x4F534357); // "OSCW" —— 与 OscilloscopeModule 的 "PBEQ" 区分
        addAndMakeVisible(b);
    };
    setupChannelBtn(btnL);
    setupChannelBtn(btnR);
    setupChannelBtn(btnBoth);

    btnL.onClick    = [this]() { setChannelMode(ChannelMode::left);  };
    btnR.onClick    = [this]() { setChannelMode(ChannelMode::right); };
    btnBoth.onClick = [this]() { setChannelMode(ChannelMode::both);  };

    btnBoth.setToggleState(true, juce::dontSendNotification);
}

OscilloscopeWaveModule::~OscilloscopeWaveModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Oscilloscope);
}

juce::ValueTree OscilloscopeWaveModule::saveModuleSpecificState() const
{
    juce::ValueTree s("state");
    s.setProperty("channelMode", (int) channelMode, nullptr);
    return s;
}

void OscilloscopeWaveModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
    if (state.hasProperty("channelMode"))
        setChannelMode((ChannelMode) (int) state.getProperty("channelMode"));
}

void OscilloscopeWaveModule::setChannelMode(ChannelMode m)
{
    if (channelMode == m) return;
    channelMode = m;
    refreshChannelButtons();
    invalidateDynamicLayer();
    repaint();
}

void OscilloscopeWaveModule::refreshChannelButtons()
{
    btnL.setToggleState   (channelMode == ChannelMode::left,  juce::dontSendNotification);
    btnR.setToggleState   (channelMode == ChannelMode::right, juce::dontSendNotification);
    btnBoth.setToggleState(channelMode == ChannelMode::both,  juce::dontSendNotification);
}

// ----------------------------------------------------------
// onFrame：Hub 分发器回调
// ----------------------------------------------------------
void OscilloscopeWaveModule::onFrame(const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has(AnalyserHub::Kind::Oscilloscope)) return;

    const int n = (int) frame.oscL.size();
    snapshotL.resize(n);
    snapshotR.resize(n);
    std::memcpy(snapshotL.getRawDataPointer(), frame.oscL.data(), (size_t) n * sizeof(float));
    std::memcpy(snapshotR.getRawDataPointer(), frame.oscR.data(), (size_t) n * sizeof(float));

    if (snapshotChangedSinceLastDraw())
        dynamicLayerDirty = true;

    // v2.9：去掉 15ms×DPI 硬编码节流；波形没变化时跳过重绘
    // v2.9.3：同步生成 GL 直渲染快照
    buildGlSnapshot();
    if (dynamicLayerDirty)
        repaint();
}

// ----------------------------------------------------------
// buildGlSnapshot —— GL 直渲染快照（v2.9.3，与 OscilloscopeModule 同构；
//   v2.9.4 迁移到统一框架）
// ----------------------------------------------------------
void OscilloscopeWaveModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto canvas = getCanvasBounds (getContentBounds());
    auto inner = canvas.reduced (4);
    if (inner.getWidth() <= 8 || inner.getHeight() <= 8)
        return;

    const bool hasL = (snapshotL.size() > 1);
    const bool hasR = (snapshotR.size() > 1);
    const bool drawL = (channelMode == ChannelMode::left || channelMode == ChannelMode::both) && hasL;
    const bool drawR = (channelMode == ChannelMode::right || channelMode == ChannelMode::both) && hasR;
    if (! drawL && ! drawR)
        return;

    auto snap = beginGlSnapshot();
    // v2.9.4：逻辑 FBO 渲染（规避 dock/聚焦时的透明与拖影问题，见框架注释）
    snap->renderToLogical = true;
    snap->logicalW = canvas.getWidth();
    snap->logicalH = canvas.getHeight();
    snap->canvasLocal = canvas;

    const int maxPts = juce::jmax (64, inner.getWidth());
    snap->vertices.reserve ((size_t) (1500 * 6 * 6 + (size_t) 4 * maxPts * 6 * 6));

    auto pushRect = [&] (juce::Rectangle<float> r, juce::Colour c)
    {
        const float x0 = r.getX(), y0 = r.getY(), x1 = r.getRight(), y1 = r.getBottom();
        const float rr = c.getFloatRed(), gg = c.getFloatGreen(),
                    bb = c.getFloatBlue(), aa = c.getFloatAlpha();
        const float quad[36] = {
            x0, y0, rr, gg, bb, aa,  x1, y0, rr, gg, bb, aa,  x0, y1, rr, gg, bb, aa,
            x1, y0, rr, gg, bb, aa,  x1, y1, rr, gg, bb, aa,  x0, y1, rr, gg, bb, aa,
        };
        snap->vertices.insert (snap->vertices.end(), quad, quad + 36);
    };

    // 背景（与 drawBackground 逐笔对应）
    {
        auto cr = canvas.toFloat();
        pushRect (cr, PinkXP::content);
        pushRect ({ cr.getX(), cr.getY(), (float) cr.getWidth(), 2.0f }, PinkXP::dark);
        pushRect ({ cr.getX(), cr.getY(), 2.0f, (float) cr.getHeight() }, PinkXP::dark);
        pushRect ({ cr.getX(), cr.getBottom() - 2.0f, (float) cr.getWidth(), 2.0f }, PinkXP::hl);
        pushRect ({ cr.getRight() - 2.0f, cr.getY(), 2.0f, (float) cr.getHeight() }, PinkXP::hl);
        pushRect ({ cr.getX() + 2.0f, cr.getY() + 2.0f, (float) cr.getWidth() - 4.0f, 1.0f }, PinkXP::shdw);
        pushRect ({ cr.getX() + 2.0f, cr.getY() + 2.0f, 1.0f, (float) cr.getHeight() - 4.0f }, PinkXP::shdw);
        pushRect ({ cr.getX() + 2.0f, cr.getBottom() - 3.0f, (float) cr.getWidth() - 4.0f, 1.0f }, juce::Colours::white);
        pushRect ({ cr.getRight() - 3.0f, cr.getY() + 2.0f, 1.0f, (float) cr.getHeight() - 4.0f }, juce::Colours::white);

        const auto dot = PinkXP::pink200.withAlpha (0.35f);
        constexpr int dotStep = 8;
        for (int y = inner.getY(); y < inner.getBottom(); y += dotStep)
            for (int x = inner.getX() + ((y / dotStep) % 2) * (dotStep / 2);
                 x < inner.getRight(); x += dotStep)
                pushRect ({ (float) x, (float) y, 1.0f, 1.0f }, dot);

        const auto cross = PinkXP::pink300.withAlpha (0.6f);
        const int cx = inner.getCentreX();
        const int cy = inner.getCentreY();
        pushRect ({ (float) inner.getX(), (float) cy, (float) inner.getWidth(), 1.0f }, cross);
        pushRect ({ (float) cx, (float) inner.getY(), 1.0f, (float) inner.getHeight() }, cross);

        const auto dash = PinkXP::pink200.withAlpha (0.5f);
        const int q1x = inner.getX() + inner.getWidth() / 4;
        const int q3x = inner.getX() + inner.getWidth() * 3 / 4;
        for (int y = inner.getY(); y < inner.getBottom(); y += 4)
        {
            pushRect ({ (float) q1x, (float) y, 1.0f, 2.0f }, dash);
            pushRect ({ (float) q3x, (float) y, 1.0f, 2.0f }, dash);
        }
    }

    // 波形折线 strip（glow + main）
    const float h     = (float) inner.getHeight();
    const float cy    = (float) inner.getCentreY();
    const float halfH = h * 0.45f;

    auto pushWave = [&] (const juce::Array<float>& samples,
                         juce::Colour glowCol, float glowHalfW,
                         juce::Colour mainCol, float mainHalfW)
    {
        const int N = samples.size();
        const int targetPoints = juce::jmax (64, inner.getWidth());
        const int stride = juce::jmax (1, N / targetPoints);

        std::vector<float> xs, ys;
        xs.reserve ((size_t) (N / stride + 4));
        ys.reserve ((size_t) (N / stride + 4));
        for (int i = 0; i < N; i += stride)
        {
            const float raw = samples.getUnchecked (i);
            const float s = std::isfinite (raw) ? juce::jlimit (-1.0f, 1.0f, raw) : 0.0f;
            xs.push_back ((float) inner.getX() + (float) i / (float) juce::jmax (1, N - 1) * (float) inner.getWidth());
            ys.push_back (cy - s * halfH);
        }
        if ((N - 1) % stride != 0)
        {
            const float raw = samples.getUnchecked (N - 1);
            const float s = std::isfinite (raw) ? juce::jlimit (-1.0f, 1.0f, raw) : 0.0f;
            xs.push_back ((float) inner.getRight());
            ys.push_back (cy - s * halfH);
        }

        const float gr = glowCol.getFloatRed(),   gg = glowCol.getFloatGreen(),
                    gb = glowCol.getFloatBlue(),  ga = glowCol.getFloatAlpha();
        const float mr = mainCol.getFloatRed(),   mg = mainCol.getFloatGreen(),
                    mb = mainCol.getFloatBlue(),  ma = mainCol.getFloatAlpha();

        const size_t ptCount = xs.size();
        for (int layer = 0; layer < 2; ++layer)
        {
            const float hw = (layer == 0) ? glowHalfW : mainHalfW;
            const float r  = (layer == 0) ? gr : mr;
            const float gg2= (layer == 0) ? gg : mg;
            const float b  = (layer == 0) ? gb : mb;
            const float a  = (layer == 0) ? ga : ma;

            for (size_t i = 0; i + 1 < ptCount; ++i)
            {
                const float dx = xs[i + 1] - xs[i];
                const float dy = ys[i + 1] - ys[i];
                const float len = std::sqrt (dx * dx + dy * dy);
                if (len < 1.0e-6f) continue;
                const float nx = -dy / len * hw;
                const float ny =  dx / len * hw;

                const float q[36] = {
                    xs[i] + nx,     ys[i] + ny,     r, gg2, b, a,
                    xs[i + 1] + nx, ys[i + 1] + ny, r, gg2, b, a,
                    xs[i] - nx,     ys[i] - ny,     r, gg2, b, a,
                    xs[i + 1] + nx, ys[i + 1] + ny, r, gg2, b, a,
                    xs[i + 1] - nx, ys[i + 1] - ny, r, gg2, b, a,
                    xs[i] - nx,     ys[i] - ny,     r, gg2, b, a,
                };
                snap->vertices.insert (snap->vertices.end(), q, q + 36);
            }
        }
    };

    if (drawL)
        pushWave (snapshotL, PinkXP::pink300.withAlpha (0.35f), 1.1f, PinkXP::pink400, 0.7f);
    if (drawR)
        pushWave (snapshotR, PinkXP::pink500.withAlpha (0.35f), 1.1f, PinkXP::pink600, 0.7f);

    // 单段：全部为 GL_TRIANGLES
    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });

    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> OscilloscopeWaveModule::getToolbarBounds(juce::Rectangle<int> content) const
{
    return content.withHeight(toolbarH);
}

juce::Rectangle<int> OscilloscopeWaveModule::getCanvasBounds(juce::Rectangle<int> content) const
{
    return content.withTrimmedTop(toolbarH + 4);
}

void OscilloscopeWaveModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto tb = getToolbarBounds(contentBounds).reduced(2);
    const int gap = 4;
    const int btnW = 56;

    btnL.setBounds   (tb.getX(),                    tb.getY(), btnW, tb.getHeight());
    btnR.setBounds   (tb.getX() + (btnW + gap),     tb.getY(), btnW, tb.getHeight());
    btnBoth.setBounds(tb.getX() + (btnW + gap) * 2, tb.getY(), btnW, tb.getHeight());
}

// ----------------------------------------------------------
// 绘制
// ----------------------------------------------------------

// P5 平台分流：Windows 用 Image 缓存，macOS 直绘
namespace
{
#if JUCE_MAC
    constexpr bool kOscWaveUseImageCache = false;
#else
    constexpr bool kOscWaveUseImageCache = true;
#endif
}

// ---- GlDirectModuleBase 钩子（v2.9.4 迁移自 v2.9.3 直渲染样板）----
juce::Rectangle<int> OscilloscopeWaveModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void OscilloscopeWaveModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 接管时只保留通道模式标签（GL 波形之上的 overlay）
    g.setColour (PinkXP::ink.withAlpha (0.85f));
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
    const juce::String modeLabel = (channelMode == ChannelMode::left)  ? "L" :
                                   (channelMode == ChannelMode::right) ? "R" : "L+R";
    g.drawText (modeLabel, canvas.getX() + 4, canvas.getBottom() - 14,
                40, 12, juce::Justification::centredLeft, false);
}

void OscilloscopeWaveModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    auto canvas = getCanvasBounds(contentBounds);
    rebuildStaticLayerIfNeeded(contentBounds);
    drawStaticLayer(g, contentBounds);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    if (kOscWaveUseImageCache)
    {
        redrawDynamicLayerIfNeeded(canvas);
        if (dynamicLayer.isValid())
            g.drawImageAt(dynamicLayer, canvas.getX(), canvas.getY());
    }
    else
    {
        drawWaveform(g, canvas);
    }

    // 左下角通道模式标签
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    const juce::String modeLabel = (channelMode == ChannelMode::left)  ? "L" :
                                   (channelMode == ChannelMode::right) ? "R" : "L+R";
    g.drawText(modeLabel, canvas.getX() + 4, canvas.getBottom() - 14,
               40, 12, juce::Justification::centredLeft, false);
}

// ----------------------------------------------------------
// 背景：凹陷画布 + 像素网格
// ----------------------------------------------------------
void OscilloscopeWaveModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
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
// Waveform 绘制
// ----------------------------------------------------------
void OscilloscopeWaveModule::drawWaveform(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    auto inner = canvas.reduced(4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const float h  = (float) inner.getHeight();
    const float cy = (float) inner.getCentreY();
    const float halfH = h * 0.45f;

    if (channelMode == ChannelMode::left || channelMode == ChannelMode::both)
    {
        const int N = snapshotL.size();
        if (N > 1)
        {
            auto pathL = buildWaveformPath(snapshotL, inner, cy, halfH);
            g.setColour(PinkXP::pink300.withAlpha(0.35f));
            g.strokePath(pathL, juce::PathStrokeType(2.2f));
            g.setColour(PinkXP::pink400);
            g.strokePath(pathL, juce::PathStrokeType(1.4f));
        }
    }

    if (channelMode == ChannelMode::right || channelMode == ChannelMode::both)
    {
        const int N = snapshotR.size();
        if (N > 1)
        {
            auto pathR = buildWaveformPath(snapshotR, inner, cy, halfH);
            g.setColour(PinkXP::pink500.withAlpha(0.35f));
            g.strokePath(pathR, juce::PathStrokeType(2.2f));
            g.setColour(PinkXP::pink600);
            g.strokePath(pathR, juce::PathStrokeType(1.4f));
        }
    }

    // 左下角通道说明（仅在 Both 模式下显示图例）
    if (channelMode == ChannelMode::both)
    {
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
}

juce::Path OscilloscopeWaveModule::buildWaveformPath(const juce::Array<float>& samples,
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

// ----------------------------------------------------------
// 静态层（背景）
// ----------------------------------------------------------
void OscilloscopeWaveModule::rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds)
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

void OscilloscopeWaveModule::drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const
{
    if (! staticLayer.isValid() || contentBounds.isEmpty())
        return;

    g.drawImageAt(staticLayer, contentBounds.getX(), contentBounds.getY());
}

void OscilloscopeWaveModule::invalidateStaticLayer()
{
    staticLayer = juce::Image();
    staticLayerContentBounds = {};
}

// ----------------------------------------------------------
// 动态层（波形缓存）
// ----------------------------------------------------------
void OscilloscopeWaveModule::invalidateDynamicLayer()
{
    dynamicLayer = juce::Image();
    dynamicLayerCanvasBounds = {};
    dynamicLayerDirty = true;
    lastDrawnSampleCount = -1;
}

bool OscilloscopeWaveModule::snapshotChangedSinceLastDraw() const noexcept
{
    const int n = snapshotL.size();
    if (n != lastDrawnSampleCount)
        return true;
    if (n <= 0)
        return false;

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

void OscilloscopeWaveModule::redrawDynamicLayerIfNeeded(juce::Rectangle<int> canvas)
{
    if (canvas.getWidth() <= 0 || canvas.getHeight() <= 0)
    {
        invalidateDynamicLayer();
        return;
    }

    const bool sizeChanged = (! dynamicLayer.isValid())
                          || dynamicLayer.getWidth()  != canvas.getWidth()
                          || dynamicLayer.getHeight() != canvas.getHeight();

    const bool modeChanged = (lastDrawnMode != channelMode);

    if (sizeChanged)
    {
        dynamicLayer = juce::Image(juce::Image::ARGB,
                                   canvas.getWidth(),
                                   canvas.getHeight(),
                                   true);
        dynamicLayerDirty = true;
    }

    if (modeChanged)
        dynamicLayerDirty = true;

    dynamicLayerCanvasBounds = canvas;

    if (! dynamicLayerDirty)
        return;

    dynamicLayer.clear(dynamicLayer.getBounds(), juce::Colours::transparentBlack);

    juce::Graphics dg(dynamicLayer);
    const juce::Rectangle<int> localCanvas(0, 0, canvas.getWidth(), canvas.getHeight());

    drawWaveform(dg, localCanvas);

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
    lastDrawnMode     = channelMode;
    dynamicLayerDirty = false;
}
