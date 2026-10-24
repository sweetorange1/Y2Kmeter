#include "source/ui/modules/Spectrogram3DModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include <juce_opengl/juce_opengl.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>

// ==========================================================
// Spectrogram3DModule
// (force rebuild: v1.9.4 P7 canvas bg cache)
// ==========================================================

Spectrogram3DModule::Spectrogram3DModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::spectrogram3d), hub (h)
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

    // ---------- 山峰着色按钮（默认按下：跟随主题着色）----------
    themeColourBtn.setClickingTogglesState (true);
    themeColourBtn.setToggleState (true, juce::dontSendNotification);
    themeColourBtn.setTooltip ("山峰着色跟随当前主题（关闭则使用固定蓝→红热力图）");
    themeColourBtn.onClick = [this]()
    {
        useThemeColours = themeColourBtn.getToggleState();
        depthPalettesDirty = true;
        imageCacheDirty    = true;
        repaint();
    };

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        speedLabel.setColour  (juce::Label::textColourId,          PinkXP::ink);
        speedSlider.setColour (juce::Slider::textBoxTextColourId,  PinkXP::ink);
        speedLabel.repaint();
        speedSlider.repaint();
        // GL 快照底色跟随主题（瀑布图基色走下方 depthPalettesDirty 重建）
        buildGlSnapshot();
        themeColourBtn.repaint();
        // 主题切换 → 山峰主题色与背景 fade 目标变化 → 重建色板 + 重绘
        depthPalettesDirty = true;
        imageCacheDirty    = true;
        repaint();
    });

    addAndMakeVisible (speedSlider);
    addAndMakeVisible (speedLabel);
    addAndMakeVisible (themeColourBtn);

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
// v2.9.3 持久化：TINT（山峰着色模式）按钮状态
// ----------------------------------------------------------
juce::ValueTree Spectrogram3DModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("useThemeColours", useThemeColours, nullptr);
    return s;
}

void Spectrogram3DModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    if (! state.hasProperty ("useThemeColours"))
        return;

    useThemeColours = (bool) state.getProperty ("useThemeColours");
    themeColourBtn.setToggleState (useThemeColours, juce::dontSendNotification);
    // 着色模式变化 → 色板与缓存重建
    depthPalettesDirty = true;
    imageCacheDirty    = true;
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> Spectrogram3DModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    return content.withTrimmedRight (sliderPanelW);
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：火山与旗帜（§8.3）
// ----------------------------------------------------------
bool Spectrogram3DModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void Spectrogram3DModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    // 反馈 C7：不要气泡频率读数，只保留火山 + 余烬 + 旗帜
    ctx.addModuleActor (makeVolcanoActor (hit.pos, { 6.0f, 10.0f }));
}

void Spectrogram3DModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    auto area = contentBounds;
    auto rightPanel = area.removeFromRight (sliderPanelW);
    juce::ignoreUnused (area);

    auto labelArea = rightPanel.removeFromTop (14);
    speedLabel.setBounds  (labelArea);
    auto btnArea = rightPanel.removeFromBottom (18);
    speedSlider.setBounds (rightPanel);
    themeColourBtn.setBounds (btnArea);

    // v2.9.5：尺寸守卫 —— 拖动窗口/dock 时 layoutContent 每帧调用，
    //   无条件重建（S3D ~2ms）会把消息线程打满导致交互卡顿
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
        buildGlSnapshot();
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
// 强度 t∈[0,1] → 山峰颜色（按 useThemeColours 选择热力图或主题色阶）
// ----------------------------------------------------------
juce::Colour Spectrogram3DModule::valueToColour (float t) const noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);
    return useThemeColours ? themeValueToColour (t) : heatValueToColour (t);
}

// ----------------------------------------------------------
// 蓝→红固定热力图（低=深蓝，高=红），不依赖主题
//
//   t=0.00 → 深蓝（无信号）
//   t=0.25 → 青色
//   t=0.50 → 绿色
//   t=0.75 → 黄色
//   t=1.00 → 红色（满幅爆点）
// ----------------------------------------------------------
juce::Colour Spectrogram3DModule::heatValueToColour (float t) noexcept
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
    return c;
}

// ----------------------------------------------------------
// 主题色阶（低=深色主题色，高=亮色主题色），跟随当前颜色预设
//
//   自定义主题：
//     t=0.00 → primary 深色（左侧强调色的暗部）
//     t=1.00 → secondary   （右侧基色，满幅峰值）
//   预设主题：
//     t=0.00 → pink700（最深）
//     t=0.30 → pink500
//     t=0.60 → pink300
//     t=0.85 → pink50 （最亮）
//     t=1.00 → sel    （满幅爆点高亮）
// ----------------------------------------------------------
juce::Colour Spectrogram3DModule::themeValueToColour (float t) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    // 自定义主题：高能量（满幅）使用右侧选择的基色（secondary），
    //   低能量（无信号）直接使用左侧强调色（accent / primary），
    //   形成 accent → secondary 的渐变（不做变深，避免低能量处发脏）。
    if (PinkXP::isCustomThemeActive())
    {
        const juce::Colour low  = PinkXP::getCustomPrimary();
        const juce::Colour high = PinkXP::getCustomSecondary();
        return low.interpolatedWith (high, t);
    }

    struct Stop { float t; juce::Colour c; };
    const Stop stops[] = {
        { 0.00f, PinkXP::pink700 },
        { 0.30f, PinkXP::pink500 },
        { 0.60f, PinkXP::pink300 },
        { 0.85f, PinkXP::pink50  },
        { 1.00f, PinkXP::sel     },
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
    return c;
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

    // v2.9.2：滚动更新按分发 tick 隔帧降频。列累计按 dt 连续进行（跳过期间
    //   nCols 自动翻倍），滚动速度不变；离屏图重建成本减半。用 tick 而非
    //   33ms 门控 —— 低帧率（GL 26fps 时帧间隔 38ms）下 33ms 门控会失效。
    if ((frame.tickCount & 1ull) != 0)
        return;

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

    // v2.9.3：更新 GL 直渲染快照（UI 线程，推列后数据已变化）
    buildGlSnapshot();

    // v2.9：去掉 20ms（≈50fps）硬编码节流；仅在本帧确有新列推入时重绘
    repaint();
}

// ----------------------------------------------------------
// buildGlSnapshot —— GL 直渲染快照（v2.9.3 引入，v2.9.4 迁移到统一框架）
//   与 renderToImage 共用同一套投影/色板逻辑；顶点为模块本地坐标，
//   Editor 侧负责模块屏幕偏移 + DPI + Y-flip 变换。
// ----------------------------------------------------------
void Spectrogram3DModule::buildGlSnapshot()
{
    // 浮动态无 GL 渲染目标，跳过快照构建（v2.9.5：拖动浮动窗口 resize 时
    //   若仍每帧全量重建 ~2ms，消息线程被打满 → 交互卡死）
    if (! wantsGlSnapshot())
        return;

    // v2.9.3 诊断开关：环境变量 Y2K_NO_S3D_GL 非空时禁用 GL 直渲染（两端），
    //   用于二分定位启动崩溃（旧软件渲染路径不受影响）。
    static const bool glDisabled = juce::SystemStats::getEnvironmentVariable ("Y2K_NO_S3D_GL", "").isNotEmpty();
    if (glDisabled)
        return;

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto plot   = canvas.reduced (2);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    recomputeProjection (plot.getWidth(), plot.getHeight());
    const int effRows = juce::jmin (historyLen, juce::jmin (juce::jmax (1, frameCount), visibleRows));
    if (depthPalettesDirty || depthPalettesRows != effRows)
        rebuildDepthPalettes (effRows);

    auto snap = beginGlSnapshot();
    // v2.9.4：逻辑 FBO 渲染（与 Waveform 同路径 —— 内容自包含在独立 FBO，
    //   与组件缓存的交互和旧版"逻辑纹理 + 放大"一致，规避后缓冲直渲染在
    //   dock/聚焦时的透明与拖影问题）
    snap->renderToLogical = true;
    snap->logicalW = canvas.getWidth();
    snap->logicalH = canvas.getHeight();
    snap->canvasLocal = canvas;

    const int strips = numBins - 1;

    // 单一顶点流布局（条带 triangles 与轮廓 lines 混排在同一 VBO，
    //   逐层 drawArrays 用 offset 区分）：[背景 9 矩形][层0 条带+轮廓]...
    //   层序 = 旧→新（新层后画覆盖旧层，3D 前后层次正确）。
    constexpr int kBgRectCount = 9;
    constexpr int kBgVertices  = kBgRectCount * 6;
    const int vertsPerLayer = strips * 6;
    const int linesPerLayer = numBins;
    snap->vertices.resize ((size_t) (kBgVertices + effRows * (vertsPerLayer + linesPerLayer)) * 6);

    {
        float* bv = snap->vertices.data();
        auto pushRect = [&] (juce::Rectangle<float> r, juce::Colour c)
        {
            const float x0 = r.getX(), y0 = r.getY(), x1 = r.getRight(), y1 = r.getBottom();
            const float rr = c.getFloatRed(), gg = c.getFloatGreen(),
                        bb = c.getFloatBlue(), aa = c.getFloatAlpha();
            const float quad[36] = {
                x0, y0, rr, gg, bb, aa,  x1, y0, rr, gg, bb, aa,  x0, y1, rr, gg, bb, aa,
                x1, y0, rr, gg, bb, aa,  x1, y1, rr, gg, bb, aa,  x0, y1, rr, gg, bb, aa,
            };
            std::memcpy (bv, quad, sizeof (quad));
            bv += 36;
        };
        // 与 PinkXP::drawSunken(g, canvas, PinkXP::content) 逐笔对应
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
    }

    // 绘制段描述：背景 + 逐层[条带 + 轮廓]
    snap->segments.push_back ({ 0, kBgVertices, Primitive::Triangles });
    {
        int offset = kBgVertices;
        for (int s = 0; s < effRows; ++s)
        {
            snap->segments.push_back ({ offset, vertsPerLayer, Primitive::Triangles });
            offset += vertsPerLayer;
            snap->segments.push_back ({ offset, linesPerLayer, Primitive::Lines });
            offset += linesPerLayer;
        }
    }

    const float inv255 = 1.0f / 255.0f;
    const float ox = (float) plot.getX();
    const float oy = (float) plot.getY();
    float* v = snap->vertices.data() + (size_t) kBgVertices * 6;  // 条带顶点从背景之后开始

    // v2.9.3：按"旧层→新层"顺序构建顶点流。GPU 按提交顺序光栅化，
    //   与旧软件路径（renderToImage 的 d 从 effRows-1 递减到 0）一致 ——
    //   新层后画、覆盖旧层，3D 前后层次正确（旧层不会盖住新层）。
    for (int seg = 0; seg < effRows; ++seg)
    {
        const int d = effRows - 1 - seg;   // d 仍是深度索引（色板/投影按 d 取），仅提交顺序按 seg
        const int readIdx = (writeIdx - 1 - d + historyLen * 2) % historyLen;
        const auto& row = historyRing[(size_t) readIdx];

        const float baseY  = oy + projOriginY - (float) d * projSlantY;
        const float depthX = (float) d * projSlantX;

        // 该层颜色：magToIdx → depthPalettes（ARGB → RGBA floats）
        std::array<float, 256 * 4> rgba;
        for (int i = 0; i < numBins; ++i)
        {
            const float mag = row[(size_t) i];
            const int   mi  = juce::jlimit (0, 4095, (int) (mag * 4095.0f));
            const int   idx = (int) magToIdx[(size_t) mi];
            const juce::Colour c = depthPalettes[(size_t) d][(size_t) idx];
            rgba[(size_t) i * 4 + 0] = c.getFloatRed();
            rgba[(size_t) i * 4 + 1] = c.getFloatGreen();
            rgba[(size_t) i * 4 + 2] = c.getFloatBlue();
            rgba[(size_t) i * 4 + 3] = c.getFloatAlpha();
        }

        // 条带三角形（GL_TRIANGLES，6 顶点/条带，颜色与顶部 y 取自该列幅度）
        //   —— 同层内先画条带，轮廓线随后（与旧软件路径一致）
        for (int i = 0; i < strips; ++i)
        {
            const float mag = row[(size_t) i];
            const int   mi  = juce::jlimit (0, 4095, (int) (mag * 4095.0f));
            const int   idx = (int) magToIdx[(size_t) mi];
            const float x0  = ox + projOriginX + depthX + (float) i * projBinWidth;
            const float x1  = x0 + projBinWidth;
            const float top = baseY - (float) idx * inv255 * projMaxH;
            const float yb  = baseY;

            const float r = rgba[(size_t) i * 4 + 0];
            const float g = rgba[(size_t) i * 4 + 1];
            const float b = rgba[(size_t) i * 4 + 2];
            const float a = rgba[(size_t) i * 4 + 3];

            // 三角形 1: (x0,top) (x1,top) (x0,yb)
            *v++ = x0; *v++ = top; *v++ = r; *v++ = g; *v++ = b; *v++ = a;
            *v++ = x1; *v++ = top; *v++ = r; *v++ = g; *v++ = b; *v++ = a;
            *v++ = x0; *v++ = yb;  *v++ = r; *v++ = g; *v++ = b; *v++ = a;
            // 三角形 2: (x1,top) (x1,yb) (x0,yb)
            *v++ = x1; *v++ = top; *v++ = r; *v++ = g; *v++ = b; *v++ = a;
            *v++ = x1; *v++ = yb;  *v++ = r; *v++ = g; *v++ = b; *v++ = a;
            *v++ = x0; *v++ = yb;  *v++ = r; *v++ = g; *v++ = b; *v++ = a;
        }

        // 轮廓线顶点（GL_LINES，白色 25% alpha）：顶部折线（画在本层条带之后）
        for (int i = 0; i < numBins; ++i)
        {
            const float mag = row[(size_t) i];
            const int   mi  = juce::jlimit (0, 4095, (int) (mag * 4095.0f));
            const int   idx = (int) magToIdx[(size_t) mi];
            const float x   = ox + projOriginX + depthX + (float) i * projBinWidth;
            const float top = baseY - (float) idx * inv255 * projMaxH;
            *v++ = x;
            *v++ = top;
            *v++ = 1.0f; *v++ = 1.0f; *v++ = 1.0f; *v++ = 0.25f;
        }
    }

    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// GlDirectModuleBase 钩子（v2.9.4 迁移自 v2.9.3 直渲染样板）
// ----------------------------------------------------------
juce::Rectangle<int> Spectrogram3DModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void Spectrogram3DModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 轴标签 / 悬停标尺画在组件缓存，合成后位于 GL 内容之上（层级正确）
    drawAxisLabels (g, canvas.reduced (2));
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

// 旧软件路径（未接管时）：离屏 Image 缓存版（P2+P4）
//
//   P2：将 3D 视图渲染到离屏 juce::Image，paintContent 只需一次 drawImage。
//   P4：canvas 对角线超过 900px 时降分辨率渲染，像素风 UI 对此高度宽容。
void Spectrogram3DModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto canvas = getCanvasBounds (contentBounds);
    const auto plot   = canvas.reduced (2);

    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    const int effLen = juce::jmax (1, frameCount);
    if (effLen <= 0) return;

    // 离屏 Image 缓存：数据脏 / canvas 尺寸变化 / GL 上下文变化时重建。
    //   每层全量 strokePath（配 Path 复用）已消除 P6 降频的视觉回退。
    if (imageCacheDirty || cached3DImage.isNull()
        || glImageContext != static_cast<void*> (juce::OpenGLContext::getCurrentContext())
        || cachedCanvasW != canvas.getWidth()
        || cachedCanvasH != canvas.getHeight())
    {
        renderToImage(canvas);
        imageCacheDirty = false;
    }

    // P4: 将降分辨率 Image 放大到 canvas 尺寸（单次 drawImage 完成上采样 blit）
    g.drawImage (cached3DImage,
                 canvas.getX(), canvas.getY(), canvas.getWidth(), canvas.getHeight(),
                 0, 0, cached3DImage.getWidth(), cached3DImage.getHeight());

    // 轴标签浮绘在 3D 视图上方（不进入 Image，保证文字清晰度）
    drawAxisLabels (g, plot);

    // 鼠标悬停标尺（实时绘制在缓存之上）
    if (hoverActive)
        drawHoverRuler(g, canvas);
}

void Spectrogram3DModule::mouseMove(const juce::MouseEvent& e)
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

void Spectrogram3DModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint();
    }
}

void Spectrogram3DModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) minFreqHz;
    const double fMax       = juce::jmin((double) maxFreqHz, nyquist);

    // X → 频率（对数；频率轴宽度 = projBinWidth × (numBins-1)）
    const float freqAxisW = projBinWidth * (float)(numBins - 1);
    const float t = (float)(hoverPos.x - canvas.getX() - projOriginX)
                  / juce::jmax(1.0f, freqAxisW);
    const double logA   = std::log10(fMin);
    const double logB   = std::log10(fMax);
    const float  freqHz = (float) std::pow(10.0,
        logA + juce::jlimit(0.0, 1.0, (double) t) * (logB - logA));

    const juce::String readout = PinkXP::formatFreqHz(freqHz);
    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
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

    // v2.9.3 修复：depthPalettes 的 resize 在构造函数末尾才执行，但构造中
    //   addAndMakeVisible 会提前触发 layoutContent → updateGlSnapshot →
    //   本函数 —— 此前对空 vector 越界写（[d][i]），导致启动堆损坏崩溃。
    //   这里显式保证容量，任何调用时机都安全。
    if ((int) depthPalettes.size() < nRows)
        depthPalettes.resize ((size_t) nRows);

    // 深度 fade 目标：主题模式向画布底色融合，热力图模式向深蓝黑融合
    const juce::Colour fadeTarget = useThemeColours ? PinkXP::content : juce::Colour (8, 8, 24);
    // 先算基准色板（无深度 fade）
    std::array<juce::Colour, 256> basePalette;
    for (int i = 0; i < 256; ++i)
        basePalette[(size_t) i] = valueToColour ((float) i * (1.0f / 255.0f));

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

    // v2.9.2：GL 上下文可用时使用 FBO 驻留 Image —— 12600 次 fillRect 由
    //   GPU 光栅化、drawImage 直接复用 FBO 纹理（零上传），绕开 CPU 软光栅
    //   与每帧整图纹理重传两大成本。上下文不可用（软光栅回退 / 浮动窗口
    //   过渡态）时回退软件 Image。
    if (auto* ctx = juce::OpenGLContext::getCurrentContext())
    {
        if (cached3DImage.isNull()
            || glImageContext != static_cast<void*> (ctx)
            || cached3DImage.getWidth()  != rw
            || cached3DImage.getHeight() != rh)
        {
            cached3DImage = juce::Image (juce::OpenGLImageType().create (juce::Image::ARGB, rw, rh, false));
            glImageContext = static_cast<void*> (ctx);
        }
    }
    else
    {
        glImageContext = nullptr;
        if (cached3DImage.isNull()
            || cached3DImage.getWidth()  != rw
            || cached3DImage.getHeight() != rh)
        {
            cached3DImage = juce::Image (juce::Image::ARGB, rw, rh, true);
        }
    }

    juce::Graphics ig (cached3DImage);
    // P4: 缩放变换 —— 后续所有 fillRect/strokePath 用全尺寸 cw×ch 坐标，
    //     自动等比缩放到 rw×rh 的实际 Image 像素
    ig.addTransform (juce::AffineTransform::scale (scale));

    // 底衬（Image-local 坐标 → 由变换转为 scaled 坐标）
    //   先整图铺底（FBO Image 无初始内容且避免每帧 BitmapData clear 的昂贵路径）
    juce::Rectangle<int> imgCanvas (0, 0, cw, ch);
    ig.setColour (PinkXP::content);
    ig.fillRect (imgCanvas.toFloat());
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

    juce::Graphics::ScopedSaveState ss(ig);
    ig.reduceClipRegion(plot);

    // Path 对象复用：clear() 替代每层重建，避免 VTune 报告的 HeapAlloc 热点
    juce::Path outline;
    const juce::PathStrokeType outlineStroke(0.6f);

    // v2.9.2：恢复旧版单遍全高绘制。上一版"楔形裁剪"虽减少像素量，但轮廓线
    //   超出楔形部分不再被后续层覆盖，露出逐列竖线（立方体 + 分界线观感），
    //   被确认视觉回归。性能改用 onFrame 的 30Hz 滚动更新降频换取。
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

        // 顶部轮廓线：复用循环外 Path 对象，clear() 后重建，
        //   避免每层分配/销毁的堆开销（VTune 显示 HeapAlloc 位居热点前列）
        outline.clear();
        outline.startNewSubPath(binX[0], baseY);
        outline.lineTo         (binX[0], binTopY[0]);
        for (int i = 0; i < numBins - 1; ++i)
            outline.lineTo(binX[(size_t)i + 1], binTopY[(size_t)i + 1]);
        outline.lineTo(binX[(size_t)(numBins - 1)], baseY);

        ig.setColour(juce::Colours::white.withAlpha(0.25f));
        ig.strokePath(outline, outlineStroke);
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