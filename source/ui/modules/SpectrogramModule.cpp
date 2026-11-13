#include "source/ui/modules/SpectrogramModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/palette/OkLchUtils.h"
#include <cmath>
#include <algorithm>

// ==========================================================
// SpectrogramModule
// ==========================================================

SpectrogramModule::SpectrogramModule (AnalyserHub& h)
    : GlDirectModuleBase (ModuleType::spectrogram), hub (h)
{
    // 订阅 Spectrum 路 —— 与 SpectrumModule 共享同一路 1024 bin FFT 幅度，
    // 后端零新增计算。
    hub.retain (AnalyserHub::Kind::Spectrum);
    hub.addFrameListener (this);

    setMinSize     (96, 80);
    setDefaultSize (384, 224); // 6×3.5 大格，宽胜于高——强调"时间轴"铺展
    setTitleText   ("Spectrogram");
    setControlPanelEnabled (true);   // v3.1：右侧 SPEED 滑条与显示模式按钮搬进控制面板

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        // 频谱 base/accent 色 + GL 快照底色跟随主题
        refreshSpectrogramColours();
        buildGlSnapshot();

        // 刷新 Spectrogram 瀑布图 base/accent 颜色并失效 Image 缓存，
        // 下一帧 paint 会全量重绘为新配色。
        refreshSpectrogramColours();
        texCacheValid_ = false;   // v2.9.5：纹理缓存随配色失效
        imageBuf = juce::Image();
    });

    // 根据初始主题设置 Spectrogram 瀑布图配色
    refreshSpectrogramColours();
}

SpectrogramModule::~SpectrogramModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Spectrum);
}

void SpectrogramModule::setCellPx (int px)
{
    cellPx = juce::jlimit (2, 32, px);
    // 强制下一次 ensureGrid 重建
    lastCanvasW = lastCanvasH = 0;
    texCacheValid_ = false;      // v2.9.5：网格分辨率变化 → 纹理缓存失效
    lastBuiltWriteCol_ = -1;
    repaint();
}

// ----------------------------------------------------------
// 持久化 + 控制面板（v3.1）
//   键：displayMode（旧，Classic / Sharp / Sharper）· speed · direction · colour · fftScale
//   缺键 = 保持当前值（构造默认 = 旧外观）。
// ----------------------------------------------------------
juce::ValueTree SpectrogramModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("displayMode", (int) displayMode, nullptr);
    s.setProperty ("speed",       (double) pixelsPerSecond, nullptr);
    s.setProperty ("direction",   (int) direction, nullptr);
    s.setProperty ("colour",      (int) colourScheme, nullptr);
    s.setProperty ("fftScale",    (int) fftScale, nullptr);
    return s;
}

void SpectrogramModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    if (state.hasProperty ("displayMode"))
        setDisplayMode ((DisplayMode) juce::jlimit (0, 2, (int) state.getProperty ("displayMode")));
    if (state.hasProperty ("speed"))
        setScrollSpeed ((float) (double) state.getProperty ("speed"));
    if (state.hasProperty ("direction"))
        setDirection ((Direction) juce::jlimit (0, 3, (int) state.getProperty ("direction")));
    if (state.hasProperty ("colour"))
        setColourScheme ((ColourScheme) juce::jlimit (0, 3, (int) state.getProperty ("colour")));
    if (state.hasProperty ("fftScale"))
        setFftScale (y2k::fscale::fromIndex ((int) state.getProperty ("fftScale")));

    refreshControlPanel();
}

void SpectrogramModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("direction", "Scroll direction",
        { "Leftwards", "Rightwards", "Upwards", "Downwards" },
        "Which way the picture moves. Leftwards: new sound enters on the right (classic). "
        "Upwards and Downwards turn the picture so time runs vertically and frequency runs left to right.")
        .in ("Scrolling"));
    out.push_back (ParamDef::slider ("speed", "Speed", 5.0, 120.0, 1.0, " px/s",
        "Scroll speed in pixels per second. Slower keeps a longer history on screen.")
        .in ("Scrolling"));

    out.push_back (ParamDef::choice ("shaper", "Sharpening",
        { "Classic", "Sharp", "Sharper" },
        "Classic: raw FFT magnitude. Sharp: frequency reassignment - thinner, clearer note lines. "
        "Sharper: Sharp plus tighter transients.").in ("Image"));
    out.push_back (ParamDef::choice ("colour", "Colours",
        { "Theme", "Heat", "Grey", "Ice" },
        "Theme follows the current colour theme. Heat runs black - red - yellow - white. "
        "Grey is black to white. Ice runs dark blue to white.").in ("Image"));
    out.push_back (ParamDef::choice ("fftScale", "FFT scale",
        { "Log", "Mel", "Linear" },
        "How frequency is laid out along the frequency axis. Log gives every octave equal room, "
        "Mel follows pitch perception, Linear gives equal Hz per pixel.").in ("Image"));
}

double SpectrogramModule::getParamValue (const juce::String& id) const
{
    if (id == "direction") return (double) (int) direction;
    if (id == "speed")     return (double) pixelsPerSecond;
    if (id == "shaper")    return (double) (int) displayMode;
    if (id == "colour")    return (double) (int) colourScheme;
    if (id == "fftScale")  return (double) (int) fftScale;
    return 0.0;
}

void SpectrogramModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "direction") setDirection ((Direction) juce::jlimit (0, 3, juce::roundToInt (v)));
    else if (id == "speed")     setScrollSpeed ((float) v);
    else if (id == "shaper")    setDisplayMode ((DisplayMode) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "colour")    setColourScheme ((ColourScheme) juce::jlimit (0, 3, juce::roundToInt (v)));
    else if (id == "fftScale")  setFftScale (y2k::fscale::fromIndex (juce::roundToInt (v)));
}

void SpectrogramModule::setScrollSpeed (float pxPerSec)
{
    pixelsPerSecond = juce::jlimit (5.0f, 120.0f, pxPerSec);
}

// 方向 / FFT scale 变化会改变网格的行列含义：清历史重建（与改变模块尺寸时一致）
void SpectrogramModule::setDirection (Direction d)
{
    if (direction == d) return;
    direction = d;
    lastCanvasW = lastCanvasH = 0;      // 强制 ensureGrid 重建
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBuf = juce::Image();
    repaint();
}

void SpectrogramModule::setFftScale (y2k::fscale::Scale s)
{
    if (fftScale == s) return;
    fftScale = s;
    lastCanvasW = lastCanvasH = 0;      // 旧列按旧刻度采样，不可复用
    texCacheValid_ = false;
    lastBuiltWriteCol_ = -1;
    imageBuf = juce::Image();
    repaint();
}

void SpectrogramModule::setColourScheme (ColourScheme c)
{
    if (colourScheme == c) return;
    colourScheme = c;
    texCacheValid_ = false;
    imageBuf = juce::Image();
    repaint();
}

void SpectrogramModule::setDisplayMode (DisplayMode m)
{
    if (displayMode == m) return;

    displayMode = m;

    // v2.9.5：纹理缓存失效（颜色调色随模式/主题变化）
    texCacheValid_ = false;

    // 切换模式后，grid 里已有的旧列仍是旧模式的数据（无法重算——重分配依赖
    // 相位历史），会随滚动自然过渡到新模式；这里仅重绘当前 image，新数据在
    // 下一帧 onFrame 按新模式数据源写入。
    if (imageBuf.isValid())
        redrawFullImage();

    repaint();
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> SpectrogramModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    // v3.1：右侧 SPEED 滑条面板取消，整块内容区都是画布。
    //   Hz 刻度文字直接浮绘在画布内部（半透明在频谱色块上），
    //   older/newer 提示同样浮绘在画布内部，无需抠占画布空间。
    return content;
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：贴纸炸弹（§8.3）
// ----------------------------------------------------------
bool SpectrogramModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void SpectrogramModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    // Pin 角色贴纸（E.3.1，不再写死池）
    ctx.addModuleActor (makeStickerActor (ctx.role (ToyRole::Pin), hit.pos, getToyScrollPxPerSec()));
    // 行 → 频率 + dB（E.4.3）
    const float freq = 20.0f * std::pow (0.95f * 20000.0f / 20.0f,
                                         1.0f - (hit.pos.y - hit.rect.getY()) / juce::jmax (1.0f, hit.rect.getHeight()));
    const float db = -90.0f + 60.0f * ((hit.pos.y - hit.rect.getY()) / juce::jmax (1.0f, hit.rect.getHeight()));
    ctx.bubble (hit.pos.translated (0.0f, -20.0f),
                juce::String (freq / 1000.0f, 1) + "kHz " + juce::String (db, 0) + "dB",
                ToyRole::Pin);
}

void SpectrogramModule::layoutContent (juce::Rectangle<int>)
{
    // v3.1：没有侧栏控件，画布 = 整个内容区
}

void SpectrogramModule::resized()
{
    ModulePanel::resized();
    // grid 尺寸的同步延迟到 paintContent / onFrame 里按需做
}

// ----------------------------------------------------------
// 网格尺寸管理
//
//   规则：
//     · 默认（画布尺寸足够）情况下：rows = canvasH / cellPx，
//       cols = canvasW / cellPx ——— 每格都是 cellPx × cellPx 正方形。
//     · 用户拉伸模块时：rows/cols 会按新的画布尺寸整除 cellPx 重算。
//       绘制时每格的实际像素尺寸 cellW = canvasW / cols、cellH = canvasH / rows
//       —— 两者整数相同值时格子保持正方形；不同值时所有格子统一变成
//       同一种长方形（符合"用户拉伸模块的时候可能根据窗口变成长方形，
//       但全部格子仍然一致"的需求）。
//     · 如果画布尺寸特别小（不足一个 cellPx），至少保留 1 行 1 列。
// ----------------------------------------------------------
void SpectrogramModule::ensureGrid (int canvasW, int canvasH)
{
    canvasW = juce::jmax (1, canvasW);
    canvasH = juce::jmax (1, canvasH);

    // 横向滚动（Left / Right）：频率纵向 → rows 跟随高度，cols（时间）跟随宽度；
    // 纵向滚动（Up / Down）：频率横向 → rows 跟随宽度，cols（时间）跟随高度。
    const int newCols = juce::jmax (1, (isVertical() ? canvasH : canvasW) / cellPx);
    const int newRows = juce::jmax (1, (isVertical() ? canvasW : canvasH) / cellPx);

    // 画布尺寸 & cellPx 都没变时不重建
    if (canvasW == lastCanvasW && canvasH == lastCanvasH
        && newRows == rows && newCols == cols
        && (int) gridData.size() == rows * cols)
        return;

    rows     = newRows;
    cols     = newCols;
    gridData.assign ((size_t) rows * (size_t) cols, 0.0f);
    writeCol = 0;

    lastCanvasW = canvasW;
    lastCanvasH = canvasH;
}

// ----------------------------------------------------------
// 色彩：从 base 色（无信号/低幅度）到 accent 色（满信号）的线性渐变
//   · t = 0 → base   → 无信号底色（custom: secondary 基色；预设: content）
//   · t = 1 → accent → 满信号主色（custom: primary 强调色；预设: swatch）
//
//   切换主题时 refreshSpectrogramColours() 会更新 base/accent 成员，
//   同时 invalidate imageBuf 强制全量重绘，确保颜色立即切换。
// ----------------------------------------------------------
void SpectrogramModule::refreshSpectrogramColours()
{
    if (PinkXP::isCustomThemeActive())
    {
        spectrogramBaseColour_   = PinkXP::getCustomSecondary();
        spectrogramAccentColour_ = PinkXP::getCustomPrimary();
    }
    else
    {
        const auto& theme = PinkXP::getCurrentTheme();
        spectrogramBaseColour_   = theme.content;
        spectrogramAccentColour_ = theme.swatch;
    }
}

juce::Colour SpectrogramModule::intensityToColour (float t) const noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    // v3.1 固定色阶（颜色参数 ≠ Theme）：不随主题变化
    switch (colourScheme)
    {
        case ColourScheme::Heat:
        {
            // 黑 → 深红 → 橙黄 → 白
            const juce::Colour stops[] = { juce::Colour (0xff080406), juce::Colour (0xffb01818),
                                           juce::Colour (0xfff6b020), juce::Colour (0xfffffff0) };
            const float pos = t * 3.0f;
            const int   i   = juce::jmin (2, (int) pos);
            return stops[i].interpolatedWith (stops[i + 1], pos - (float) i);
        }
        case ColourScheme::Grey:
            return juce::Colour::fromFloatRGBA (t, t, t, 1.0f);
        case ColourScheme::Ice:
        {
            const juce::Colour stops[] = { juce::Colour (0xff04081c), juce::Colour (0xff1c4aa0),
                                           juce::Colour (0xff4cc0e8), juce::Colour (0xfff2fbff) };
            const float pos = t * 3.0f;
            const int   i   = juce::jmin (2, (int) pos);
            return stops[i].interpolatedWith (stops[i + 1], pos - (float) i);
        }
        case ColourScheme::Theme:
        default:
            break;
    }

    // 调色板重构（A3/S4）：生成器主题的强度色阶与 Waveform 保持完全一致的
    //   色谱映射 —— chartRamp 8 阶本身横跨"主题色相 → accent2 色相"，
    //   能量越高越亮且色相随阶渐变（蓝→粉），而非单一亮度的同色加深：
    //   · t ∈ [0, 0.15]：画布底色 content 平滑融入色谱起点 ramp[0]；
    //   · t ∈ (0.15, 1]：与 Waveform 相同的 sampleChartRamp 全阶渐变。
    //   预设主题保持 base→accent 数据驱动（S7 保真），插值空间同样用 OKLab。
    if (PinkXP::isCustomThemeActive())
    {
        constexpr float kBlend = 0.15f;
        if (t < kBlend)
            return PinkXP::interpolateOkLab (PinkXP::content,
                                             PinkXP::chartRamp[0],
                                             t / kBlend);
        return PinkXP::sampleChartRamp ((t - kBlend) / (1.0f - kBlend));
    }
    return PinkXP::interpolateOkLab (spectrogramBaseColour_, spectrogramAccentColour_, t);
}

// ----------------------------------------------------------
// 频率 → 网格行号（对数；row=0 顶=高频，rows-1 底=低频）
// ----------------------------------------------------------
int SpectrogramModule::freqToGridRow (double freqHz, double minHz, double maxHz, int rows) noexcept
{
    const double f    = juce::jlimit (minHz, maxHz, freqHz);
    const double logA = std::log10 (minHz);
    const double logB = std::log10 (maxHz);
    const double t    = (std::log10 (f) - logA) / juce::jmax (1.0e-9, logB - logA); // 0..1，0=低频
    const int    row  = (int) std::round ((1.0 - t) * (double) (rows - 1));
    return juce::jlimit (0, rows - 1, row);
}

// ----------------------------------------------------------
// 一帧 → 一列网格值
//
//   变更说明：
//     旧实现里本函数在 UI 侧对 1024 bin 的 FFT 幅度做"对数频率分段 + 每段取最大"。
//     现在 Hub 直接给出 rows 个对数频率点上的合并幅度（低频 8192 FFT + 高频 2048 FFT，
//     500Hz 附近做等功率交叉），行数 → 点数一一对应，本函数只需：
//       1) 每行线性 → dB；
//       2) dB → 归一化到 [0,1]；
//       3) 写入 grid 的 writeCol 列；writeCol 环形前进。
// ----------------------------------------------------------
void SpectrogramModule::pushColumn (const juce::Array<float>& rowsMags)
{
    if (rows <= 0 || cols <= 0) return;
    if (gridData.empty())       return;
    if (rowsMags.size() != rows) return;

    // Hub 返回的顺序是 "低频 → 高频"（r=0 是最低频 f_0），
    // 而 grid 约定 r=0 在"顶部 = 高频"、r=rows-1 在"底部 = 低频"，
    // 所以写入时需要翻转：gridRow = rows - 1 - i。
    for (int i = 0; i < rows; ++i)
    {
        const float mag = std::abs (rowsMags.getUnchecked (i));
        const float db  = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));
        const float t01 = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));

        set (rows - 1 - i, writeCol, t01);
    }

    writeCol = (writeCol + 1) % cols;
}

// ----------------------------------------------------------
// onFrame
// ----------------------------------------------------------
void SpectrogramModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    // 按住 = 画面完全静止：不采样、不推进新列；只把帧时钟对齐，
    //   否则松手后 dt 会攒成一大坨、一次性补好几列。
    if (hold.active)
    {
        lastFrameMs = juce::Time::getMillisecondCounterHiRes();
        return;
    }

    const auto canvas = getCanvasBounds (getContentBounds());
    // 与 paintContent 一致：内部绘图区 = canvas.reduced(2)，外围 2px 留给凹陷边框
    const auto plot   = canvas.reduced (2);
    ensureGrid (plot.getWidth(), plot.getHeight());
    if (rows <= 0) return;

    // 让 Hub 直接按 rows 个对数频率点返回"双路合并"的线性幅度数组，
    // 低频段自动走 8192FFT（Δf≈5.86Hz @48k）—— 本模块低频分辨率显著提升。
    //
    // 注意：fMax 不直接取到 Nyquist。Nyquist 附近的 bin 会被抗混叠滤波
    //       压成极低能量，若 rows 的最顶点落到 Nyquist，最顶那一行
    //       几乎恒为 ≈0，视觉上呈现为"一条常态深色横线"。这里取 0.95×Nyquist
    //       作为上限，既保留可用高频带，又规避这条恒黑线。
    const double nyquist = hub.getSampleRate() * 0.5;
    const float  fMin    = minFreqHz;
    const float  fMax    = juce::jmin (maxFreqHz, (float) (nyquist * 0.95));

    // 显示模式选择数据源：
    //   Classic → 原始幅度谱；Sharp/Sharper → 频率重分配（粒子凝聚）
    if (displayMode == DisplayMode::Classic)
        hub.getSpectrumMagnitudesBlended (rowMagBuf, rows, fMin, fMax, false, fftScale);
    else
        hub.getSpectrumReassignedBlended (rowMagBuf, rows, fMin, fMax, fftScale);

    // ---------- 速度解耦：按"真实流逝时间"累计推进列数 ----------
    //   旧行为：每帧 push 1 列 → 滚动速度 = 分发 fps（Hub 30/60Hz 直接体现在速度上）。
    //   新行为：columnAccumulator += dtMs * pixelsPerSecond / 1000，抽取整数部分 → 推进列数。
    //   这样高 fps 时会"有几帧推 0 列"，低 fps 时会"一帧推多列"，
    //   视觉滚动速度始终目标定在 pixelsPerSecond 列/秒。
    const double now = juce::Time::getMillisecondCounterHiRes();
    float dtMs = 0.0f;
    if (lastFrameMs > 0.0)
        dtMs = (float) (now - lastFrameMs);
    lastFrameMs = now;
    dtMs = juce::jlimit (0.0f, 500.0f, dtMs); // 显著卡顿/首帧时夹住，避免一次性满屏

    columnAccumulator += dtMs * pixelsPerSecond * 0.001f;
    int nCols = (int) columnAccumulator;
    // 卡上限：即使大拖动/黑屏后恢复，最多一次补全满屏列
    if (cols > 0 && nCols > cols) nCols = cols;
    if (nCols < 0) nCols = 0;
    columnAccumulator -= (float) nCols;

    // 子列偏移（0..1）：剩下的 `columnAccumulator` 就是"离下一列还差多少"。
    //   低速度时一列要好几帧才推进，如果只按整列跳，画面会每 100~200ms 跳
    //   cellPx 个像素 —— 现场表现就是"每动一次闪一下"（尤其 <30 px/s 时）。
    //   把这段小数喂给滚动窗口（GL 的 texScrollU / CPU 的变换原点），滚动就是
    //   连续的；新列仍然只在整数时刻写入。因此这里必须**每帧**重建快照并重绘，
    //   不能再像以前那样 nCols==0 就直接 return。
    ensureImage();

    // 连续 push nCols 列（都是当前帧最新的幅度快照 —— 低 fps 下这会
    // ‘复制同一列形成连续横条’，视觉上则是滚动速度视帧插补）。
    for (int k = 0; k < nCols; ++k)
    {
        pushColumn (rowMagBuf);
        writeLatestColumnToImage();
    }

    // v2.9.5：同步生成 GL 纹理快照（新列已写入）
    buildGlSnapshot();

    repaintFrameContent();
}

// ----------------------------------------------------------
// GlDirectModuleBase 钩子（v2.9.5 迁移，纹理通道）
// ----------------------------------------------------------
juce::Rectangle<int> SpectrogramModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void SpectrogramModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    drawAxisLabels (g, canvas.reduced (2));
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void SpectrogramModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    const auto canvas = getCanvasBounds (contentBounds);
    drawBackground (g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    // 绘图区 = canvas.reduced(2)：外围 2px 是凹陷边框，image 只贴内部
    const auto plot = canvas.reduced (2);
    if (plot.getWidth() <= 4 || plot.getHeight() <= 4) return;

    auto* self = const_cast<SpectrogramModule*> (this);
    self->ensureGrid (plot.getWidth(), plot.getHeight());
    self->ensureImage();
    if (rows <= 0 || cols <= 0 || ! imageBuf.isValid()) return;

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (plot);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

    const int cw = plot.getWidth();
    const int ch = plot.getHeight();

    // 方向变换：图像坐标 (u = 时间列 0..2*cols，v = 频率行 0..rows，行 0 = 高频) → 屏幕。
    //   Left（旧外观）：u→x 向右增长，截取 [writeCol, writeCol+cols) 窗口，新数据在右。
    //   其余方向见下，窗口同样是 [writeCol, writeCol+cols)，只是轴与朝向不同。
    // 与 GL 路径一致：窗口起点 = 已写列 + 子列偏移（低速时连续滚动，不再整列跳）
    const float wc = (float) writeCol + juce::jlimit (0.0f, 1.0f, columnAccumulator);
    const float px = (float) plot.getX(), py = (float) plot.getY();
    juce::AffineTransform xform;
    if (! isVertical())
    {
        const float sx = (float) cw / (float) cols;
        const float sy = (float) ch / (float) rows;
        if (direction == Direction::Left)
            xform = juce::AffineTransform (sx, 0.0f, px - wc * sx,
                                           0.0f, sy, py);
        else   // Right：新数据在左，时间轴反向
            xform = juce::AffineTransform (-sx, 0.0f, px + (wc + (float) cols) * sx,
                                           0.0f, sy, py);
    }
    else
    {
        const float sx = (float) cw / (float) rows;   // 频率沿 x（低频在左）
        const float sy = (float) ch / (float) cols;   // 时间沿 y
        if (direction == Direction::Up)   // 新数据在底部，画面向上移动
            xform = juce::AffineTransform (0.0f, -sx, px + (float) rows * sx,
                                           sy, 0.0f, py - wc * sy);
        else                               // Down：新数据在顶部，画面向下移动
            xform = juce::AffineTransform (0.0f, -sx, px + (float) rows * sx,
                                           -sy, 0.0f, py + (wc + (float) cols) * sy);
    }
    g.drawImageTransformed (imageBuf, xform, false);

    drawAxisLabels (g, plot);
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

// ----------------------------------------------------------
// buildGlSnapshot —— 纹理通道快照（v2.9.5）
//   双拷贝环形纹理（2*cols × rows 像素，与旧版 imageBuf 同布局）：
//   texScrollU/texWinW 把 [writeCol, writeCol+cols) 子区映射到 plot ——
//   零 memmove 滚动；背景（凹陷边框）为顶点流；轴标签/hover 为 overlay。
// ----------------------------------------------------------
void SpectrogramModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto plot   = canvas.reduced (2);
    if (plot.getWidth() <= 4 || plot.getHeight() <= 4)
        return;

    if (rows <= 0 || cols <= 0)
        return;

    auto snap = beginGlSnapshot();
    snap->canvasLocal = canvas;

    if (direction != Direction::Left)
    {
        // ---- Right / Up / Down：展开成无环形的纹理（每帧重排；用 256 级调色板查表）----
        snap->texDirty = true;
        snap->texScrollU = 0.0f;
        snap->texWinW = 1.0f;
        snap->texTargetRect = plot;
        buildUnrolledTexture (snap->texPixels, snap->texW, snap->texH);
        texCacheValid_ = false;   // 环形路径的缓存与此无关，切回 Left 时需要重建

        appendSunkenFrame (snap->vertices, canvas);
        snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
        publishGlSnapshot (std::move (snap));
        return;
    }

    // ---- 纹理：gridData 着色（双拷贝环形）----
    snap->texW = cols * 2;
    snap->texH = rows;
    snap->texDirty = true;
    // 窗口起点 = 已写列 + 未走完的那一列（子列偏移）→ 低速时滚动也是连续的
    snap->texScrollU = ((float) writeCol + juce::jlimit (0.0f, 1.0f, columnAccumulator))
                            / (float) (cols * 2);
    snap->texWinW = 0.5f;
    snap->texTargetRect = plot;
    {
        // v2.9.5：增量着色 —— 只有新列重新调色（12 万像素全量 interpolatedWith
        //   每帧 ~2.4ms，是消息线程卡顿主因）；其余列复用缓存。
        //   行序翻转：内存行 0 = 低频（纹理 v=0 = 底，屏幕顶配 uv(0,1)）。
        const size_t totalPix = (size_t) snap->texW * (size_t) snap->texH;
        if (texPixelsCache_.size() != totalPix)
            texCacheValid_ = false;

        if (! texCacheValid_)
        {
            texPixelsCache_.resize (totalPix);
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                {
                    const juce::uint32 argb = intensityToColour (gridData[(size_t) (r * cols + c)]).getARGB();
                    const int rOut = rows - 1 - r;   // 内存行 0 = 低频
                    texPixelsCache_[(size_t) rOut * (size_t) snap->texW + (size_t) c] = argb;
                    texPixelsCache_[(size_t) rOut * (size_t) snap->texW + (size_t) (c + cols)] = argb;
                }
            texCacheValid_ = true;
        }
        else
        {
            // v2.9.5：增量更新整个"自上次构建以来写入的环形区间" ——
            //   低帧率一帧 push 多列（nCols>1），只更新最后一列会让中间列
            //   残留该环形位置的几十秒前历史数据 → 竖直黑线。
            int pending = (lastBuiltWriteCol_ < 0)
                              ? cols
                              : ((writeCol - lastBuiltWriteCol_ + cols) % cols);
            for (int k = 0; k < pending; ++k)
            {
                const int c = (writeCol - 1 - k + cols) % cols;
                for (int r = 0; r < rows; ++r)
                {
                    const juce::uint32 argb = intensityToColour (gridData[(size_t) (r * cols + c)]).getARGB();
                    const int rOut = rows - 1 - r;
                    texPixelsCache_[(size_t) rOut * (size_t) snap->texW + (size_t) c] = argb;
                    texPixelsCache_[(size_t) rOut * (size_t) snap->texW + (size_t) (c + cols)] = argb;
                }
            }
        }
        lastBuiltWriteCol_ = writeCol;
        snap->texPixels = texPixelsCache_;   // 拷贝（480KB memcpy ~30μs）
    }

    // ---- 背景顶点：凹陷边框（框架 helper）----
    appendSunkenFrame (snap->vertices, canvas);

    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// 展开纹理（Right / Up / Down）
//   时间序 τ：0 = 最老，cols-1 = 最新；环形列 c = (writeCol + τ) % cols。
//   纹理内存行 0 = 画面底部（v=0 = 底）。gridData 行 0 = 高频。
//     Right：W = cols, H = rows。x = cols-1-τ（最新在左），m = rows-1-r。
//     Up   ：W = rows, H = cols。x = rows-1-r（低频在左），m = cols-1-τ（最新在底）。
//     Down ：W = rows, H = cols。x = rows-1-r，m = τ（最新在顶）。
// ----------------------------------------------------------
void SpectrogramModule::buildUnrolledTexture (std::vector<juce::uint32>& out, int& outW, int& outH) const
{
    const bool vertical = isVertical();
    outW = vertical ? rows : cols;
    outH = vertical ? cols : rows;
    out.assign ((size_t) outW * (size_t) outH, 0u);

    std::array<juce::uint32, 256> palette;
    for (int i = 0; i < 256; ++i)
        palette[(size_t) i] = intensityToColour ((float) i * (1.0f / 255.0f)).getARGB();

    auto idxOf = [] (float t) { return juce::jlimit (0, 255, (int) std::lround (t * 255.0f)); };

    for (int tau = 0; tau < cols; ++tau)
    {
        const int c = (writeCol + tau) % cols;
        for (int r = 0; r < rows; ++r)
        {
            const juce::uint32 argb = palette[(size_t) idxOf (gridData[(size_t) (r * cols + c)])];
            int x = 0, m = 0;
            switch (direction)
            {
                case Direction::Right: x = cols - 1 - tau; m = rows - 1 - r; break;
                case Direction::Up:    x = rows - 1 - r;   m = cols - 1 - tau; break;
                case Direction::Down:  x = rows - 1 - r;   m = tau; break;
                case Direction::Left:  default: break;
            }
            out[(size_t) m * (size_t) outW + (size_t) x] = argb;
        }
    }
}

// 悬停点 → 频率 + 距今秒数（方向 / FFT scale 感知）
void SpectrogramModule::hoverToFreqAndAge (juce::Rectangle<int> canvas, juce::Point<int> p,
                                           float& freqHz, float& secondsAgo) const
{
    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) minFreqHz;
    const double fMax       = juce::jmin ((double) maxFreqHz, nyquist);

    float freqT = 0.0f, ageFrac = 0.0f;   // freqT: 0 = 低频；ageFrac: 0 = 最新
    switch (direction)
    {
        case Direction::Left:
            freqT   = 1.0f - (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            ageFrac = (float) (canvas.getRight() - p.x) / (float) juce::jmax (1, canvas.getWidth());
            break;
        case Direction::Right:
            freqT   = 1.0f - (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            ageFrac = (float) (p.x - canvas.getX()) / (float) juce::jmax (1, canvas.getWidth());
            break;
        case Direction::Up:
            freqT   = (float) (p.x - canvas.getX()) / (float) juce::jmax (1, canvas.getWidth());
            ageFrac = (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            ageFrac = 1.0f - ageFrac;
            break;
        case Direction::Down:
            freqT   = (float) (p.x - canvas.getX()) / (float) juce::jmax (1, canvas.getWidth());
            ageFrac = (float) (p.y - canvas.getY()) / (float) juce::jmax (1, canvas.getHeight());
            break;
    }

    freqHz = (float) y2k::fscale::fromUnit (fftScale, juce::jlimit (0.0, 1.0, (double) freqT), fMin, fMax);
    const float spanPx = isVertical() ? (float) canvas.getHeight() : (float) canvas.getWidth();
    secondsAgo = juce::jlimit (0.0f, 1.0f, ageFrac) * spanPx / juce::jmax (1.0f, pixelsPerSecond);
}

void SpectrogramModule::mouseMove(const juce::MouseEvent& e)
{
    ModulePanel::mouseMove(e);

    const auto canvas = getCanvasBounds(getContentBounds());
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains(pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (canvas);
    }
}

void SpectrogramModule::mouseDrag(const juce::MouseEvent& e)
{
    ModulePanel::mouseDrag(e);

    // 按住拖动期间 JUCE 只派发 mouseDrag（mouseMove 被抑制），若不同步更新
    //   hoverPos，十字线与频率 / 时间 / 音名读数就会停在按下那一刻的位置不动。
    const auto canvas = getCanvasBounds(getContentBounds());
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains(pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (canvas);
    }
}

void SpectrogramModule::mouseDown(const juce::MouseEvent& e)
{
    ModulePanel::mouseDown(e);   // 保留基类行为（toFront / 右键菜单 / 边缘拖拽）

    // 只在"左键按在画布内部（避开边缘缩放热区）"时冻结；抽屉是子组件，点击不会走到这里。
    if (e.mods.isLeftButtonDown()
        && getCanvasBounds(getContentBounds()).reduced (6).contains (e.getPosition())
        && ! getControlDrawerBounds().contains (e.getPosition()))
    {
        hold.press();
        // 保留 columnAccumulator（子列偏移），冻结时画面停在当前位置，不会跳一格
        repaint();
    }
}

void SpectrogramModule::mouseUp(const juce::MouseEvent& e)
{
    ModulePanel::mouseUp(e);

    // 松手直接恢复滚动：下一帧用新的 lastFrameMs 重新累计列
    if (hold.active)
    {
        hold.release();
        repaint();   // 去掉"冻结时才有"的音名读数，不等下一帧
    }
}

void SpectrogramModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在画布内，擦除只需失效画布（不走浮动态限流）
        repaint (getCanvasBounds (getContentBounds()));
    }
}

void SpectrogramModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    float freqHz = 0.0f, secondsAgo = 0.0f;
    hoverToFreqAndAge (canvas, hoverPos, freqHz, secondsAgo);

    juce::String readout = PinkXP::formatFreqHz(freqHz)
                         + "  " + juce::String(-secondsAgo, 1) + " s";

    // 音名只在按住冻结时显示（与波形历史一致）：滚动中同一点的读数参考意义不大，
    //   冻结后鼠标所在频率的音高才是确定值。
    if (hold.active)
    {
        const juce::String note = PinkXP::noteNameForHz(freqHz);
        if (note.isNotEmpty())
            readout += "  " + note;
    }

    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
}

// ----------------------------------------------------------
// 方案 C：双拷贝环形 Image —— 彻底消除每帧 glDeleteTextures / glTexImage2D 抖动
//
// 背景（GPU 采样诊断）：
//   旧实现在 paintContent 里每帧调用两次
//     g.drawImage (imageBuf, dstX, dstY, dstW, dstH, srcX, srcY, srcW, srcH, false);
//   这种带 src 矩形的重载会在 JUCE 内部 new 出一个临时的 SubsectionPixelData 包装
//   对象；而 juce::OpenGLRendering::CachedImageList 把"每个 ImagePixelData 指针"当作
//   纹理缓存键 —— 临时对象析构 → glDeleteTextures → 下一帧又是新的临时对象 →
//   glTexImage2D 重传 —— GPU 线程采样里 glDeleteTextures / intelSubmitCommands 堆
//   到 35% CPU 占用的根本原因。
//
// 解决：
//   · imageBuf 宽度改为 2 * cols（高 rows 不变）。每次写入"最新一列"时，
//     同时写到第 writeCol 列 和 writeCol+cols 列（双拷贝）。这样任何时候
//     截取一段长度为 cols、起点为 writeCol 的滑动窗口，都能拿到完整且顺序
//     正确的"从老到新"瀑布图。
//   · paintContent 改为只调用一次 drawImage，不带 src 矩形；贴图前用
//     AffineTransform 把 imageBuf 的 [writeCol .. writeCol+cols) 这段子区
//     缩放并平移到 plot 区域，再用 reduceClipRegion(plot) 把窗口外多余部分
//     裁掉。全程使用同一个 imageBuf pixelData 指针，CachedImageList 命中纹理
//     缓存，纹理不再销毁/重建。
// ----------------------------------------------------------
void SpectrogramModule::ensureImage()
{
    if (rows <= 0 || cols <= 0) return;

    const int desiredW = cols * 2;

    if (! imageBuf.isValid()
        || imageBuf.getWidth()  != desiredW
        || imageBuf.getHeight() != rows)
    {
        imageBuf = juce::Image (juce::Image::ARGB, desiredW, rows, true);
        redrawFullImage();   // 尺寸变化：把现有 grid 全量刷回 image（双拷贝）
    }
}

void SpectrogramModule::redrawFullImage()
{
    if (! imageBuf.isValid()) return;
    if (rows <= 0 || cols <= 0) return;

    // 见 writeLatestColumnToImage 里的说明：这里也用本地 palette 缓存，
    //   原本 rows*cols 次 intensityToColour 调用（含 getCurrentTheme() + 分支判断）
    //   改为一次性预构 256 级 ARGB 表 + 单次下标查找，全量重绘大幅降低 CPU。
    std::array<juce::PixelARGB, 256> palette;
    for (int i = 0; i < 256; ++i)
    {
        const float t = (float) i * (1.0f / 255.0f);
        const auto c  = intensityToColour (t);
        palette[(size_t) i] = juce::PixelARGB ((juce::uint8) 255,
                                               c.getRed(), c.getGreen(), c.getBlue());
    }

    juce::Image::BitmapData bmp (imageBuf, juce::Image::BitmapData::readWrite);

    for (int r = 0; r < rows; ++r)
    {
        auto* row = (juce::PixelARGB*) bmp.getLinePointer (r);
        for (int c = 0; c < cols; ++c)
        {
            const float t01 = at (r, c);
            const int   idx = juce::jlimit (0, 255, (int) std::lround (t01 * 255.0f));
            const juce::PixelARGB px = palette[(size_t) idx];
            // 双拷贝：同时写到 c 和 c+cols 两列，形成无缝环形贴图
            row[c]        = px;
            row[c + cols] = px;
        }
    }
}

void SpectrogramModule::writeLatestColumnToImage()
{
    if (! imageBuf.isValid()) return;
    if (rows <= 0 || cols <= 0) return;

    // onFrame 里 pushColumn 刚让 writeCol 环形 +1，所以"刚写入的"列是 writeCol-1
    const int justWrittenCol = (writeCol - 1 + cols) % cols;

    // ----- P1-3：一次性构建 256 级调色板缓存 -----
    //   · 旧实现里 intensityToColour 内部会 getCurrentTheme() + 多段分支判断，
    //     rows 次循环就要调 rows 次 → 一张 400×300 Spectrogram 每帧多 400 次
    //     主题查找。缓存 256 级 ARGB 像素后，后续循环变成单次数组下标查找。
    //   · 缓存只在本次调用内存在（栈上 256*4=1KB），主题切换后下次调用
    //     自然会重建，无需额外 invalidate 逻辑。
    std::array<juce::PixelARGB, 256> palette;
    for (int i = 0; i < 256; ++i)
    {
        const float t = (float) i * (1.0f / 255.0f);
        const auto c  = intensityToColour (t);
        palette[(size_t) i] = juce::PixelARGB ((juce::uint8) 255,
                                               c.getRed(), c.getGreen(), c.getBlue());
    }

    // ----- P1-3：writeOnly 锁，避免驱动"把整张纹理读回 CPU"的开销 -----
    //   只写"新列"两处像素，不读任何旧像素；改用 writeOnly 避免 JUCE 的
    //   CachedImage 在某些后端触发 read-back。
    juce::Image::BitmapData bmp (imageBuf, juce::Image::BitmapData::writeOnly);

    for (int r = 0; r < rows; ++r)
    {
        auto* row = (juce::PixelARGB*) bmp.getLinePointer (r);
        const float t01 = at (r, justWrittenCol);
        const int   idx = juce::jlimit (0, 255, (int) std::lround (t01 * 255.0f));
        const juce::PixelARGB px = palette[(size_t) idx];
        // 双拷贝：分别写到 image 的 justWrittenCol 和 justWrittenCol+cols 两列
        row[justWrittenCol]        = px;
        row[justWrittenCol + cols] = px;
    }
}

void SpectrogramModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);
}

// ----------------------------------------------------------
// 轴标签
// ----------------------------------------------------------
void SpectrogramModule::drawAxisLabels (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));

    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) minFreqHz;
    const double fMax       = juce::jmin ((double) maxFreqHz, nyquist);

    struct L { float hz; const char* label; };
    static const L logLabels[]    = { { 100.0f, "100" }, { 1000.0f, "1k" }, { 10000.0f, "10k" } };
    static const L melLabels[]    = { { 500.0f, "500" }, { 1000.0f, "1k" }, { 2000.0f, "2k" },
                                      { 5000.0f, "5k" }, { 10000.0f, "10k" } };
    static const L linearLabels[] = { { 5000.0f, "5k" }, { 10000.0f, "10k" }, { 15000.0f, "15k" } };
    const L* labels = logLabels;
    int numLabels = (int) std::size (logLabels);
    if (fftScale == y2k::fscale::Scale::mel)    { labels = melLabels;    numLabels = (int) std::size (melLabels); }
    if (fftScale == y2k::fscale::Scale::linear) { labels = linearLabels; numLabels = (int) std::size (linearLabels); }

    // Hz 刻度文字直接浮绘在画布内部（不再从画布外留白区取空间）：
    //   · 横向滚动：贴左侧；纵向滚动：频率沿 x，贴顶部
    //   · 文字上面给一小块半透明的暗色背板（不被频谱色块干扰可读）
    //   · 文字用浅色 hl（反白）保证在深色背景上高对比
    for (int li = 0; li < numLabels; ++li)
    {
        const auto& l = labels[li];
        if ((double) l.hz < fMin || (double) l.hz > fMax) continue;

        const double t = y2k::fscale::toUnit (fftScale, (double) l.hz, fMin, fMax);

        const int bw = 22;
        const int bh = 12;
        int bx, by;
        if (isVertical())
        {
            bx = canvas.getX() + (int) std::round (t * (double) canvas.getWidth()) - bw / 2;
            by = canvas.getY() + 2;
        }
        else
        {
            const int y = canvas.getY() + (int) std::round ((1.0 - t) * (double) canvas.getHeight());
            bx = canvas.getX() + 2;
            by = y - 6;
        }

        // 文字小背板（半透明墨色）—— 让文字从纷杂色块里跳出来
        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.fillRect (bx, by, bw, bh);
        // 静态标签位图缓存（v2.9.6 性能，见 SpectrumModule::drawAxisLabels）
        const auto img = PinkXP::cachedStaticText (
            g, l.label, 9.0f, juce::Font::plain, PinkXP::hl,
            juce::Colours::transparentBlack, true);
        if (img.isValid())
        {
            img.drawAt (g, (float) (bx + (bw - img.getWidth()) / 2),
                        (float) (by + (bh - img.getHeight()) / 2));
        }
        else
        {
            g.setColour (PinkXP::hl);
            g.drawText (y2k::tr (l.label), bx, by, bw, bh, juce::Justification::centred, false);
        }
    }

    // older / newer 提示：同样浮绘在画布内部，带半透明暗背板。位置与箭头随滚动方向变化：
    //   Left（旧外观）左 "← older"、右 "newer →"；Right 对调；Up / Down 放在右侧上下两端。
    {
        const int bh = 11, bw = 56;
        auto slot = [&] (juce::Rectangle<int> r, const juce::String& text)
        {
            g.setColour (PinkXP::ink.withAlpha (0.45f));
            g.fillRect (r);
            PinkXP::drawCachedSymbolText (g, text, r, juce::Justification::centred,
                                          8.5f, juce::Font::plain, PinkXP::hl);
        };
        const juce::String older = juce::String::fromUTF8 ("older");
        const juce::String newer = juce::String::fromUTF8 ("newer");
        const juce::String arrL  = juce::String::fromUTF8 ("\xe2\x86\x90 ");
        const juce::String arrR  = juce::String::fromUTF8 (" \xe2\x86\x92");
        const juce::String arrU  = juce::String::fromUTF8 ("\xe2\x86\x91 ");
        const juce::String arrD  = juce::String::fromUTF8 ("\xe2\x86\x93 ");

        const int by = canvas.getBottom() - 13;
        const juce::Rectangle<int> left  (canvas.getX() + 2,      by, bw, bh);
        const juce::Rectangle<int> right (canvas.getRight() - 58, by, bw, bh);
        const juce::Rectangle<int> top   (canvas.getRight() - 58, canvas.getY() + 2, bw, bh);
        const juce::Rectangle<int> bot   (canvas.getRight() - 58, by, bw, bh);

        switch (direction)
        {
            case Direction::Left:  slot (left, arrL + older); slot (right, newer + arrR); break;
            case Direction::Right: slot (left, arrL + newer); slot (right, older + arrR); break;
            case Direction::Up:    slot (top,  arrU + older); slot (bot,   newer + juce::String (" ") + arrD.trim()); break;
            case Direction::Down:  slot (top,  arrU + newer); slot (bot,   older + juce::String (" ") + arrD.trim()); break;
        }
    }
}
