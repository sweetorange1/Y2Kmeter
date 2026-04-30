#include "source/ui/modules/SpectrogramModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <algorithm>

// ==========================================================
// SpectrogramModule
// ==========================================================

SpectrogramModule::SpectrogramModule (AnalyserHub& h)
    : ModulePanel (ModuleType::spectrogram), hub (h)
{
    // 订阅 Spectrum 路 —— 与 SpectrumModule 共享同一路 1024 bin FFT 幅度，
    // 后端零新增计算。
    hub.retain (AnalyserHub::Kind::Spectrum);
    hub.addFrameListener (this);

    setMinSize     (96, 80);
    setDefaultSize (384, 224); // 6×3.5 大格，宽胜于高——强调"时间轴"铺展
    setTitleText   ("Spectrogram");

    // ---------- 右侧 SPEED 滑条（样式与 EqModule 的 SIZE 滑条完全一致）----------
    //   · 垂直滑条，范围 [5, 120] px/s，步长 1；默认 30 px/s
    //   · TextBox 只读（与 EqModule 一致），数字只用于展示
    //   · 切换主题时重新下发 textColour（与 EqModule 一致）
    speedSlider.setSliderStyle (juce::Slider::LinearVertical);
    speedSlider.setRange (5.0, 120.0, 1.0);
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
    repaint();
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> SpectrogramModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    // 布局风格对齐 WaveformModule（去掉顶部工具栏后的情形）：
    //   · 画布贴着 content 的左/上/下边框，只扣掉右侧 SPEED 滑条面板
    //   · 不再做对称 pad 和 axisLeft/axisBottom 切割
    //     —— Hz 刻度文字会直接浮绘在画布内部左侧（半透明在频谱上色块上），
    //        older/newer 提示同样浮绘在画布内部底部，无需抠占画布空间。
    return content.withTrimmedRight (sliderPanelW);
}

void SpectrogramModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    // 右侧 SPEED 滑条面板（样式与 EqModule、DynamicsCrestModule 完全一致）
    auto area = contentBounds;
    auto rightPanel = area.removeFromRight (sliderPanelW);
    // sliderPanelGap 像素留白在画布右缘与面板左缘之间（由 getCanvasBounds 顺带扣掉）
    juce::ignoreUnused (area);

    auto labelArea = rightPanel.removeFromTop (14);
    speedLabel.setBounds  (labelArea);
    speedSlider.setBounds (rightPanel);
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

    const int newCols = juce::jmax (1, canvasW / cellPx);
    const int newRows = juce::jmax (1, canvasH / cellPx);

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
// 色彩：跟随当前主题，从深 (pink700) 到浅 (pink100) 的 7 段线性渐变
//   · t = 0 → 低电平 → 深
//   · t = 1 → 高电平 → 浅
//   · 每次 paint 都重新取 PinkXP::pinkXXX，切换主题后自然跟随。
// ----------------------------------------------------------
juce::Colour SpectrogramModule::intensityToColour (float t) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    // ============================================================
    // 单色色谱 —— 完全跟随当前主题主色调，绝不混入其他强调色
    //
    // 为什么不用 pinkXXX？
    //   · Matcha Soda 等多个主题把 pink500/pink300 留给"强调桃粉"，
    //     只有 pink700/dark 才是主色调的深色。
    //   · Crimson Noir / Void Grey 等深色主题把 pink50..pink700 反向排序
    //     （暗→亮），用 pink50 作"近白"会在某些主题里变成深色——不可靠。
    //
    // 在所有主题下语义稳定的只有这几个符号：
    //   dark     → 最外层深色边框色（所有主题都是主色调的极深值）
    //   shdw     → 内层阴影色（中深主色调）
    //   swatch   → 主色调"色票"（每个主题最具代表性的一色）
    //   hl       → 高光白（或深色主题里的类亮色）
    //
    // 例：
    //   Matcha Soda :  dark=墨绿  shdw=深绿  swatch=主绿  → 全绿单色
    //   Bubblegum   :  dark=深紫红 shdw=阴影粉 swatch=主粉 → 全粉单色
    //   Tangerine   :  dark=墨棕  shdw=深橘  swatch=主橘  → 全橘单色
    //   Crimson     :  dark=纯黑  shdw=近黑  swatch=血红  → 黑-红单色
    // ============================================================

    // swatch 不是 PinkXP 命名空间的全局变量，而是 Theme 结构体里的字段
    //   —— 通过 getCurrentTheme() 取当前主题的主色票，切换主题自动跟随
    const juce::Colour themeSwatch = PinkXP::getCurrentTheme().swatch;

    const juce::Colour cDark   = PinkXP::dark;
    const juce::Colour cShdw   = PinkXP::shdw;
    const juce::Colour cMain   = themeSwatch;
    const juce::Colour cBright = themeSwatch.brighter (0.5f);
    const juce::Colour cTop    = PinkXP::hl;

    // ----- 非线性映射：0.00~0.50 均为极深，0.80 附近开始突变亮 -----
    //   stops 按 t 阈值分段，每段内 RGB 线性插值
    //   0.00 → dark       · 无信号背景
    //   0.50 → dark       · 同色——这一段线性插值“从dark到dark”仍是纯 dark
    //   0.70 → shdw       · 开始出现深主色调
    //   0.82 → swatch     · 主色登场（0.8 附近突变亮）
    //   0.95 → swatch.brighter(0.5)  · 再提亮
    //   1.00 → hl                   · 白色爆点
    struct Stop { float t; juce::Colour c; };
    const Stop stops[] = {
        { 0.00f, cDark   },
        { 0.50f, cDark   },   // 0~0.5 均为极深
        { 0.70f, cShdw   },
        { 0.82f, cMain   },   // 0.8 附近突变亮
        { 0.95f, cBright },
        { 1.00f, cTop    }
    };
    constexpr int N = (int) (sizeof (stops) / sizeof (stops[0]));

    for (int i = 0; i < N - 1; ++i)
    {
        if (t <= stops[i + 1].t)
        {
            const float span = juce::jmax (1.0e-6f, stops[i + 1].t - stops[i].t);
            const float u    = juce::jlimit (0.0f, 1.0f, (t - stops[i].t) / span);
            return stops[i].c.interpolatedWith (stops[i + 1].c, u);
        }
    }
    return stops[N - 1].c;
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

    hub.getSpectrumMagnitudesBlended (rowMagBuf, rows, fMin, fMax);

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

    if (nCols == 0)
    {
        // 这一帧不推进：不改变 writeCol、不改变 image，也不需要 repaint。
        return;
    }

    ensureImage();

    // 连续 push nCols 列（都是当前帧最新的幅度快照 —— 低 fps 下这会
    // ‘复制同一列形成连续横条’，视觉上则是滚动速度视帧插补）。
    for (int k = 0; k < nCols; ++k)
    {
        pushColumn (rowMagBuf);
        writeLatestColumnToImage();
    }

    repaint();
}

// ----------------------------------------------------------
// 绘制
// ----------------------------------------------------------
void SpectrogramModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    const auto canvas = getCanvasBounds (contentBounds);
    drawBackground (g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    // ===========================================================
    // 绘图区 = canvas.reduced(2)
    //   · 外围 2px 是 drawSunken 画的凹陷边框（上下左右对称）—— 与 Waveform 风格一致
    //   · image 只贴到内部，永不覆盖边框也不被边框覆盖
    //   · 之前 "最顶一条恒深色带" 的根源是：drawSunken 顶部画了 2 像素 dark、
    //     drawImage 贴全 canvas 时因缩放插值留下一条 1-2px 的深色重合条
    //     → 改成“image 只贴 canvas.reduced(2)”之后彻底消除。
    // ===========================================================
    const auto plot = canvas.reduced (2);
    if (plot.getWidth() <= 4 || plot.getHeight() <= 4) return;

    // paint 期间兼底同步（窗口刚 resize、还没 onFrame 进来时）
    auto* self = const_cast<SpectrogramModule*> (this);
    self->ensureGrid (plot.getWidth(), plot.getHeight());
    self->ensureImage();
    if (rows <= 0 || cols <= 0 || ! imageBuf.isValid()) return;

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (plot);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

    // ===========================================================
    // 单次 drawImage 贴屏（方案 C 配套改动）
    //
    //   imageBuf 是 2*cols 宽的"双拷贝环形瀑布图"，写入时对 c 和 c+cols
    //   两列同步写，任何时刻截取 [writeCol, writeCol+cols) 的一段都是
    //   "从老到新"的完整瀑布。
    //
    //   这里用 AffineTransform 把这段"子区窗口"映射到屏幕 plot 区：
    //     1. 缩放：image 源像素尺寸 (cols × rows) → 屏幕目标尺寸 (plot.w × plot.h)
    //     2. 平移：image 的 writeCol 列要贴到 plot 最左 (plot.x)，
    //              且要先把 image 的 y=0 贴到 plot.y 顶部
    //
    //   与旧实现（两次 g.drawImage(..., sx, sy, sw, sh, ...)）的关键区别：
    //   · 旧实现每次带 src 矩形 → JUCE 内部 new 临时 SubsectionPixelData →
    //     OpenGLRendering::CachedImageList 以 ImagePixelData* 为键频繁失效 →
    //     每帧 glDeleteTextures + glTexImage2D 重传全图。
    //   · 新实现：始终把整张 imageBuf 的 ImagePixelData 指针（不变）交给
    //     CachedImageList，纹理命中缓存，不再销毁/重建。超出 plot 的左右
    //     多余部分由 reduceClipRegion(plot) 裁掉，结果像素上完全一致。
    // ===========================================================
    const int cw = plot.getWidth();
    const int ch = plot.getHeight();

    const float sx = (float) cw / (float) cols;   // 每列源像素 → 屏幕像素宽
    const float sy = (float) ch / (float) rows;   // 每行源像素 → 屏幕像素高

    // 先在 image 坐标里把 writeCol 移到原点，再缩放到屏幕尺寸，最后平移到 plot
    const auto xform = juce::AffineTransform::translation (- (float) writeCol, 0.0f)
                          .scaled (sx, sy)
                          .translated ((float) plot.getX(), (float) plot.getY());

    g.drawImageTransformed (imageBuf, xform, false);

    drawAxisLabels (g, plot);
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

    juce::Image::BitmapData bmp (imageBuf, juce::Image::BitmapData::writeOnly);

    for (int r = 0; r < rows; ++r)
    {
        auto* row = (juce::PixelARGB*) bmp.getLinePointer (r);
        for (int c = 0; c < cols; ++c)
        {
            const float t01 = at (r, c);
            const auto col  = intensityToColour (t01);
            const juce::PixelARGB px ((juce::uint8) 255,
                                       col.getRed(), col.getGreen(), col.getBlue());
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

    // 用 BitmapData::readWrite 直接拿到原始像素指针批量写，避免每像素 setPixelColour
    // 的函数调用和像素格式转换开销
    juce::Image::BitmapData bmp (imageBuf, juce::Image::BitmapData::readWrite);

    for (int r = 0; r < rows; ++r)
    {
        auto* row = (juce::PixelARGB*) bmp.getLinePointer (r);
        const float t01 = at (r, justWrittenCol);
        const auto col  = intensityToColour (t01);
        const juce::PixelARGB px ((juce::uint8) 255,
                                   col.getRed(), col.getGreen(), col.getBlue());
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
    const L labels[] = {
        {   100.0f, "100" },
        {  1000.0f,  "1k" },
        { 10000.0f, "10k" },
    };

    // Hz 刻度文字直接浮绘在画布内部左侧（不再从画布外留白区取空间）：
    //   · 文字上面给一小块半透明的暗色背板（不被频谱色块干扰可读）
    //   · 文字用浅色 hl（反白）保证在深色背景上高对比
    for (auto& l : labels)
    {
        if ((double) l.hz < fMin || (double) l.hz > fMax) continue;

        const double logA = std::log10 (fMin);
        const double logB = std::log10 (fMax);
        const double t    = (std::log10 ((double) l.hz) - logA) / juce::jmax (1.0e-9, logB - logA);
        const int    y    = canvas.getY() + (int) std::round ((1.0 - t) * (double) canvas.getHeight());

        // 文字小背板（半透明墨色）—— 让文字从纷杂色块里跳出来
        const int bx = canvas.getX() + 2;
        const int by = y - 6;
        const int bw = 22;
        const int bh = 12;
        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.fillRect (bx, by, bw, bh);
        g.setColour (PinkXP::hl);
        g.drawText (l.label, bx, by, bw, bh, juce::Justification::centred, false);
    }

    // 底部 older/newer 提示：同样浮绘在画布内部底边，带半透明暗背板
    {
        const int by = canvas.getBottom() - 13;
        const int bh = 11;
        g.setColour (PinkXP::ink.withAlpha (0.45f));
        g.fillRect (canvas.getX() + 2,              by, 56, bh);
        g.fillRect (canvas.getRight() - 58,         by, 56, bh);

        g.setColour (PinkXP::hl);
        g.drawText (juce::String::fromUTF8 ("\xe2\x86\x90 older"),
                    canvas.getX() + 2, by, 56, bh,
                    juce::Justification::centred, false);
        g.drawText (juce::String::fromUTF8 ("newer \xe2\x86\x92"),
                    canvas.getRight() - 58, by, 56, bh,
                    juce::Justification::centred, false);
    }
}
