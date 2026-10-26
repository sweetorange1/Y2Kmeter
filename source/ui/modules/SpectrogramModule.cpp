#include "source/ui/modules/SpectrogramModule.h"
#include "source/ui/PinkXPStyle.h"
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

    // ---------- 右下角显示模式切换按钮（样式对齐 Spectrogram3DModule 的 TINT）----------
    //   · 三态循环：Classic → Sharp → Sharper → Classic，默认 Sharp
    //   · 不是 toggle（三选一循环），点击一次切到下一档
    modeBtn.setClickingTogglesState (false);
    modeBtn.setButtonText (displayModeText (displayMode));
    modeBtn.setTooltip ("Spectrogram display mode: \"-\" Classic (raw magnitude), "
                        "\"+\" Sharp (frequency reassignment), "
                        "\"++\" Sharper (time + frequency reassignment).");
    modeBtn.onClick = [this]()
    {
        const int next = ((int) displayMode + 1) % 3;
        setDisplayMode ((DisplayMode) next);
    };

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        speedLabel.setColour  (juce::Label::textColourId,          PinkXP::ink);
        speedSlider.setColour (juce::Slider::textBoxTextColourId,  PinkXP::ink);
        speedLabel.repaint();
        speedSlider.repaint();
        // 频谱 base/accent 色 + GL 快照底色跟随主题
        refreshSpectrogramColours();
        buildGlSnapshot();
        modeBtn.repaint();

        // 刷新 Spectrogram 瀑布图 base/accent 颜色并失效 Image 缓存，
        // 下一帧 paint 会全量重绘为新配色。
        refreshSpectrogramColours();
        texCacheValid_ = false;   // v2.9.5：纹理缓存随配色失效
        imageBuf = juce::Image();
    });

    // 根据初始主题设置 Spectrogram 瀑布图配色
    refreshSpectrogramColours();

    addAndMakeVisible (speedSlider);
    addAndMakeVisible (speedLabel);
    addAndMakeVisible (modeBtn);
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

const char* SpectrogramModule::displayModeText (DisplayMode m) noexcept
{
    switch (m)
    {
        case DisplayMode::Classic:  return "-";
        case DisplayMode::Sharper:  return "++";
        case DisplayMode::Sharp:
        default:                    return "+";
    }
}

// ----------------------------------------------------------
// v2.9.3 持久化：显示模式（Classic / Sharp / Sharper）按钮状态
// ----------------------------------------------------------
juce::ValueTree SpectrogramModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("displayMode", (int) displayMode, nullptr);
    return s;
}

void SpectrogramModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    if (! state.hasProperty ("displayMode"))
        return;

    setDisplayMode ((DisplayMode) (int) state.getProperty ("displayMode"));
}

void SpectrogramModule::setDisplayMode (DisplayMode m)
{
    if (displayMode == m) return;

    displayMode = m;
    modeBtn.setButtonText (displayModeText (m));

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
    // 布局风格对齐 WaveformModule（去掉顶部工具栏后的情形）：
    //   · 画布贴着 content 的左/上/下边框，只扣掉右侧 SPEED 滑条面板
    //   · 不再做对称 pad 和 axisLeft/axisBottom 切割
    //     —— Hz 刻度文字会直接浮绘在画布内部左侧（半透明在频谱上色块上），
    //        older/newer 提示同样浮绘在画布内部底部，无需抠占画布空间。
    return content.withTrimmedRight (sliderPanelW);
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

void SpectrogramModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    // 右侧 SPEED 滑条面板（样式与 EqModule、DynamicsCrestModule 完全一致）
    auto area = contentBounds;
    auto rightPanel = area.removeFromRight (sliderPanelW);
    // sliderPanelGap 像素留白在画布右缘与面板左缘之间（由 getCanvasBounds 顺带扣掉）
    juce::ignoreUnused (area);

    auto labelArea = rightPanel.removeFromTop (14);
    speedLabel.setBounds  (labelArea);
    auto btnArea = rightPanel.removeFromBottom (18);   // 右下角模式按钮（与 3D TINT 一致）
    speedSlider.setBounds (rightPanel);
    modeBtn.setBounds    (btnArea);
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

juce::Colour SpectrogramModule::intensityToColour (float t) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

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
        hub.getSpectrumMagnitudesBlended (rowMagBuf, rows, fMin, fMax);
    else
        hub.getSpectrumReassignedBlended (rowMagBuf, rows, fMin, fMax);

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

    // v2.9.5：同步生成 GL 纹理快照（新列已写入）
    buildGlSnapshot();

    repaint();
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
    const float sx = (float) cw / (float) cols;
    const float sy = (float) ch / (float) rows;

    // 双拷贝环形瀑布图：截取 [writeCol, writeCol+cols) 子区映射到 plot
    const auto xform = juce::AffineTransform::translation (- (float) writeCol, 0.0f)
                          .scaled (sx, sy)
                          .translated ((float) plot.getX(), (float) plot.getY());
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

    // ---- 纹理：gridData 着色（双拷贝环形）----
    snap->texW = cols * 2;
    snap->texH = rows;
    snap->texDirty = true;
    snap->texScrollU = (float) writeCol / (float) (cols * 2);
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
        repaint();
    }
}

void SpectrogramModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint();
    }
}

void SpectrogramModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const double sampleRate = hub.getSampleRate();
    const double nyquist    = (sampleRate > 0.0) ? sampleRate * 0.5 : 24000.0;
    const double fMin       = (double) minFreqHz;
    const double fMax       = juce::jmin((double) maxFreqHz, nyquist);

    // Y → 频率（对数；顶=高频，底=低频）
    const float t = 1.0f - (float)(hoverPos.y - canvas.getY())
                  / juce::jmax(1, canvas.getHeight());
    const double logA   = std::log10(fMin);
    const double logB   = std::log10(fMax);
    const float  freqHz = (float) std::pow(10.0,
        logA + juce::jlimit(0.0, 1.0, (double) t) * (logB - logA));

    // X → 相对时间（过去为负，越右越新）
    const float secondsAgo = (float)(canvas.getRight() - hoverPos.x)
                           / juce::jmax(1.0f, pixelsPerSecond);

    const juce::String readout = PinkXP::formatFreqHz(freqHz)
                               + "  " + juce::String(-secondsAgo, 1) + " s";
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
        // 静态标签位图缓存（v2.9.6 性能，见 SpectrumModule::drawAxisLabels）
        const juce::Image img = PinkXP::cachedStaticTextImage (
            l.label, 9.0f, juce::Font::plain, PinkXP::hl,
            juce::Colours::transparentBlack, true);
        if (img.isValid())
        {
            g.drawImageAt (img, bx + (bw - img.getWidth()) / 2,
                           by + (bh - img.getHeight()) / 2, false);
        }
        else
        {
            g.setColour (PinkXP::hl);
            g.drawText (l.label, bx, by, bw, bh, juce::Justification::centred, false);
        }
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
