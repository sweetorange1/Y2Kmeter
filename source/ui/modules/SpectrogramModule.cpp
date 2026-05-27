#include "source/ui/modules/SpectrogramModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <vector>

namespace
{
constexpr double kA4Hz = 440.0;
constexpr int    kA4Midi = 69;
constexpr int    kBpo = 12;

static double midiToHz (int midi) noexcept
{
    return kA4Hz * std::pow (2.0, ((double) midi - (double) kA4Midi) / (double) kBpo);
}

static juce::String midiToNote (int midi)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int pc = (midi % 12 + 12) % 12;
    const int oct = midi / 12 - 1;
    return juce::String (names[pc]) + juce::String (oct);
}
}

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

    auto setupModeBtn = [this] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (0x53475048); // "SGPH"
        addAndMakeVisible (b);
    };
    setupModeBtn (btnClassic);
    setupModeBtn (btnSharp);
    btnClassic.onClick = [this]() { setDisplayMode (DisplayMode::classic); };
    btnSharp  .onClick = [this]() { setDisplayMode (DisplayMode::sharp);   };
    btnClassic.setToggleState (true, juce::dontSendNotification);

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

void SpectrogramModule::setDisplayMode (DisplayMode mode)
{
    if (displayMode == mode)
        return;

    displayMode = mode;
    refreshModeButtons();

    // 模式切换时强制重建网格/离屏缓存，避免 classic<->sharp 尺寸语义冲突。
    rows = cols = 0;
    lastCanvasW = lastCanvasH = 0;
    writeCol = 0;
    gridData.clear();
    imageBuf = juce::Image();
    columnAccumulator = 0.0f;

    repaint();
}

void SpectrogramModule::refreshModeButtons()
{
    btnClassic.setToggleState (displayMode == DisplayMode::classic, juce::dontSendNotification);
    btnSharp  .setToggleState (displayMode == DisplayMode::sharp,   juce::dontSendNotification);
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
    return content.withTrimmedTop (toolbarH + 3)
                  .withTrimmedRight (sliderPanelW);
}

void SpectrogramModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    auto top = contentBounds.removeFromTop (toolbarH).reduced (2, 1);
    const int modeW = 62;
    const int gap   = 4;
    btnClassic.setBounds (top.getX(),                  top.getY(), modeW, top.getHeight());
    btnSharp  .setBounds (top.getX() + modeW + gap,    top.getY(), modeW, top.getHeight());

    // 右侧 SPEED 滑条面板（样式与 EqModule、DynamicsCrestModule 完全一致）
    auto area = contentBounds.withTrimmedTop (3);
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

    const int divisor = (displayMode == DisplayMode::sharp) ? 1 : cellPx;
    const int newCols = juce::jmax (1, canvasW / divisor);
    const int newRows = juce::jmax (1, canvasH / divisor);

    // 画布尺寸 & cellPx 都没变时不重建
    if (canvasW == lastCanvasW && canvasH == lastCanvasH
        && newRows == rows && newCols == cols
        && (int) gridData.size() == rows * cols)
        return;

    rows     = newRows;
    cols     = newCols;
    gridData.assign ((size_t) rows * (size_t) cols, 0.0f);
    sharpColumn.assign ((size_t) rows, 0.0f);
    rowBinDensity.assign ((size_t) rows, 0.0f);
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

void SpectrogramModule::pushSharpColumnFromBins (const float* magHi, int numHi,
                                                 const float* magLo, int numLo,
                                                 double sampleRate,
                                                 juce::uint32 seedBase)
{
    if (rows <= 0 || cols <= 0 || gridData.empty()) return;
    if (magHi == nullptr || numHi < 4 || sampleRate <= 0.0) return;

    if ((int) sharpColumn.size() != rows)  sharpColumn.assign ((size_t) rows, 0.0f);
    if ((int) rowBinDensity.size() != rows) rowBinDensity.assign ((size_t) rows, 0.0f);
    std::fill (sharpColumn.begin(), sharpColumn.end(), 0.0f);
    std::fill (rowBinDensity.begin(), rowBinDensity.end(), 0.0f);

    const double nyquist = sampleRate * 0.5;
    const double fLo     = (double) minFreqHz;
    const double fHi     = juce::jmin ((double) maxFreqHz, nyquist * 0.95);
    if (fHi <= fLo) return;

    const double xover     = 500.0;
    const double xoverLo   = xover / std::sqrt (2.0);
    const double xoverHi   = xover * std::sqrt (2.0);
    const double loHardMax = xover * 2.0;
    const double logA      = std::log10 (fLo);
    const double logB      = std::log10 (fHi);
    const double logSpan   = juce::jmax (1.0e-9, logB - logA);

    auto wHi = [xoverLo, xoverHi] (double f) noexcept -> float
    {
        if (f >= xoverHi) return 1.0f;
        if (f <= xoverLo) return 0.0f;
        const double u = (std::log (f) - std::log (xoverLo)) / juce::jmax (1.0e-9, (std::log (xoverHi) - std::log (xoverLo)));
        return (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * u));
    };
    auto wLo = [&wHi] (double f) noexcept -> float { return 1.0f - wHi (f); };

    // 轻量确定性 hash，避免 std::rand 带来的全局状态与线程问题。
    auto hash32 = [] (juce::uint32 x) noexcept -> juce::uint32
    {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    };
    auto rand01 = [&hash32] (juce::uint32 x) noexcept -> float
    {
        return (float) (hash32 (x) & 0x00ffffffu) * (1.0f / 16777215.0f);
    };

    std::vector<double> rowLoHz ((size_t) rows, fLo);
    std::vector<double> rowHiHz ((size_t) rows, fHi);
    std::vector<double> rowSpanHz ((size_t) rows, 1.0);
    for (int r = 0; r < rows; ++r)
    {
        const double tTop = 1.0 - (double) r / (double) rows;
        const double tBot = 1.0 - (double) (r + 1) / (double) rows;
        const double hiHz = std::pow (10.0, logA + tTop * logSpan);
        const double loHz = std::pow (10.0, logA + tBot * logSpan);
        rowHiHz[(size_t) r]   = juce::jmax (hiHz, loHz);
        rowLoHz[(size_t) r]   = juce::jmin (hiHz, loHz);
        rowSpanHz[(size_t) r] = juce::jmax (1.0e-9, rowHiHz[(size_t) r] - rowLoHz[(size_t) r]);
    }

    auto forEachBand = [&] (const float* mags, int nBin,
                            double binToHz,
                            auto&& weightOf,
                            double clipMin,
                            double clipMax,
                            auto&& fn)
    {
        if (mags == nullptr || nBin < 4 || binToHz <= 0.0) return;

        for (int b = 1; b < nBin - 1; ++b)
        {
            const double fCenter = (double) b * binToHz;
            if (fCenter < fLo || fCenter > fHi) continue;

            const float routeW = weightOf (fCenter);
            if (routeW <= 1.0e-4f) continue;

            const double fPrev = (double) (b - 1) * binToHz;
            const double fNext = (double) (b + 1) * binToHz;
            double bandLo = 0.5 * (fPrev + fCenter);
            double bandHi = 0.5 * (fCenter + fNext);

            bandLo = juce::jmax (bandLo, juce::jmax (fLo, clipMin));
            bandHi = juce::jmin (bandHi, juce::jmin (fHi, clipMax));
            if (bandHi <= bandLo) continue;

            fn (mags[b], b, fCenter, bandLo, bandHi, routeW);
        }
    };

    auto addBandDensity = [&] (double bandLo, double bandHi, float routeW)
    {
        const int rTop = juce::jmin (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));
        const int rBot = juce::jmax (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));

        for (int r = rTop; r <= rBot; ++r)
        {
            const double ovLo = juce::jmax (bandLo, rowLoHz[(size_t) r]);
            const double ovHi = juce::jmin (bandHi, rowHiHz[(size_t) r]);
            const double ovHz = ovHi - ovLo;
            if (ovHz <= 0.0) continue;

            const float cov = (float) juce::jlimit (0.0, 1.0, ovHz / rowSpanHz[(size_t) r]);
            rowBinDensity[(size_t) r] += cov * routeW;
        }
    };

    const double binToHzHi = nyquist / (double) (numHi - 1);
    const double binToHzLo = (numLo > 1) ? (nyquist / (double) (numLo - 1)) : 0.0;

    forEachBand (magHi, numHi, binToHzHi, wHi,
                 xoverLo * 0.5, fHi,
                 [&] (float, int, double, double bLo, double bHi, float w)
                 {
                     addBandDensity (bLo, bHi, w);
                 });

    forEachBand (magLo, numLo, binToHzLo, wLo,
                 fLo, loHardMax,
                 [&] (float, int, double, double bLo, double bHi, float w)
                 {
                     addBandDensity (bLo, bHi, w);
                 });

    float densityAvg = 1.0f;
    {
        float sumDensity = 0.0f;
        int nonZeroRows  = 0;
        for (int r = 0; r < rows; ++r)
        {
            if (rowBinDensity[(size_t) r] > 1.0e-6f)
            {
                sumDensity += rowBinDensity[(size_t) r];
                ++nonZeroRows;
            }
        }
        if (nonZeroRows > 0)
            densityAvg = sumDensity / (float) nonZeroRows;
    }

    auto emitBand = [&] (float mag, int sourceTag, int binIndex,
                         double bandLo, double bandHi,
                         float routeW)
    {
        if (! std::isfinite (mag) || mag <= 0.0f || routeW <= 1.0e-4f) return;

        const float db   = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag * std::sqrt (routeW)));
        const float tLin = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        const float tone = std::pow (tLin, 0.82f);
        if (tone <= 1.0e-4f) return;

        const int rTop = juce::jmin (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));
        const int rBot = juce::jmax (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));

        for (int r = rTop; r <= rBot; ++r)
        {
            const double ovLo = juce::jmax (bandLo, rowLoHz[(size_t) r]);
            const double ovHi = juce::jmin (bandHi, rowHiHz[(size_t) r]);
            const double ovHz = ovHi - ovLo;
            if (ovHz <= 0.0) continue;

            const float cov = (float) juce::jlimit (0.0, 1.0, ovHz / rowSpanHz[(size_t) r]);
            const float rowLoad = juce::jmax (1.0e-4f, rowBinDensity[(size_t) r]);
            const float densityComp = juce::jlimit (0.35f, 2.0f, densityAvg / rowLoad);

            const float fillRate = 0.92f;
            const float fireProb = juce::jlimit (0.0f, 1.0f,
                                                 (0.06f + 0.94f * tone)
                                                 * cov
                                                 * densityComp
                                                 * fillRate);

            const juce::uint32 key = seedBase
                                   ^ ((juce::uint32) sourceTag << 24)
                                   ^ (juce::uint32) (binIndex * 2654435761u)
                                   ^ (juce::uint32) (r * 40503);

            if (rand01 (key ^ 0x3c6ef372u) >= fireProb)
                continue;

            const float sparkle = 0.86f + 0.14f * rand01 (key ^ 0xa54ff53au);
            const float v = juce::jlimit (0.0f, 1.0f, (0.22f + 0.78f * tone) * sparkle);
            sharpColumn[(size_t) r] = juce::jmax (sharpColumn[(size_t) r], v);
        }
    };

    forEachBand (magHi, numHi, binToHzHi, wHi,
                 xoverLo * 0.5, fHi,
                 [&] (float mag, int binIndex, double, double bLo, double bHi, float w)
                 {
                     emitBand (mag, 1, binIndex, bLo, bHi, w);
                 });

    forEachBand (magLo, numLo, binToHzLo, wLo,
                 fLo, loHardMax,
                 [&] (float mag, int binIndex, double, double bLo, double bHi, float w)
                 {
                     emitBand (mag, 2, binIndex, bLo, bHi, w);
                 });

    for (int r = 0; r < rows; ++r)
        set (r, writeCol, sharpColumn[(size_t) r]);

    writeCol = (writeCol + 1) % cols;
}

void SpectrogramModule::pushSharpColumnFromCqt (const float* mags, int numBins,
                                                double sampleRate,
                                                juce::uint32 seedBase)
{
    if (rows <= 0 || cols <= 0 || gridData.empty()) return;
    if (mags == nullptr || numBins < 4 || sampleRate <= 0.0) return;

    if ((int) sharpColumn.size() != rows)  sharpColumn.assign ((size_t) rows, 0.0f);
    if ((int) rowBinDensity.size() != rows) rowBinDensity.assign ((size_t) rows, 0.0f);
    std::fill (sharpColumn.begin(), sharpColumn.end(), 0.0f);
    std::fill (rowBinDensity.begin(), rowBinDensity.end(), 0.0f);

    const double nyquist = sampleRate * 0.5;
    const double fLo     = (double) minFreqHz;
    const double fHi     = juce::jmin ((double) maxFreqHz, nyquist * 0.95);
    if (fHi <= fLo) return;

    const double logA      = std::log10 (fLo);
    const double logB      = std::log10 (fHi);
    const double logSpan   = juce::jmax (1.0e-9, logB - logA);
    constexpr double bpo   = 12.0;
    const double halfStep  = std::pow (2.0, 0.5 / bpo);
    const int midiStart = (int) std::ceil ((double) kA4Midi + bpo * std::log2 (fLo / kA4Hz));

    auto hash32 = [] (juce::uint32 x) noexcept -> juce::uint32
    {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    };
    auto rand01 = [&hash32] (juce::uint32 x) noexcept -> float
    {
        return (float) (hash32 (x) & 0x00ffffffu) * (1.0f / 16777215.0f);
    };

    std::vector<double> rowLoHz ((size_t) rows, fLo);
    std::vector<double> rowHiHz ((size_t) rows, fHi);
    std::vector<double> rowSpanHz ((size_t) rows, 1.0);
    for (int r = 0; r < rows; ++r)
    {
        const double tTop = 1.0 - (double) r / (double) rows;
        const double tBot = 1.0 - (double) (r + 1) / (double) rows;
        const double hiHz = std::pow (10.0, logA + tTop * logSpan);
        const double loHz = std::pow (10.0, logA + tBot * logSpan);
        rowHiHz[(size_t) r]   = juce::jmax (hiHz, loHz);
        rowLoHz[(size_t) r]   = juce::jmin (hiHz, loHz);
        rowSpanHz[(size_t) r] = juce::jmax (1.0e-9, rowHiHz[(size_t) r] - rowLoHz[(size_t) r]);
    }

    auto bandCenterHz = [midiStart] (int i) noexcept -> double
    {
        return midiToHz (midiStart + i);
    };

    for (int b = 0; b < numBins; ++b)
    {
        const double fc = bandCenterHz (b);
        if (fc < fLo || fc > fHi) continue;
        const double bandLo = juce::jmax (fLo, fc / halfStep);
        const double bandHi = juce::jmin (fHi, fc * halfStep);
        if (bandHi <= bandLo) continue;

        const int rTop = juce::jmin (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));
        const int rBot = juce::jmax (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));
        for (int r = rTop; r <= rBot; ++r)
        {
            const double ovLo = juce::jmax (bandLo, rowLoHz[(size_t) r]);
            const double ovHi = juce::jmin (bandHi, rowHiHz[(size_t) r]);
            const double ovHz = ovHi - ovLo;
            if (ovHz <= 0.0) continue;
            const float cov = (float) juce::jlimit (0.0, 1.0, ovHz / rowSpanHz[(size_t) r]);
            rowBinDensity[(size_t) r] += cov;
        }
    }

    float densityAvg = 1.0f;
    {
        float sumDensity = 0.0f;
        int nonZeroRows  = 0;
        for (int r = 0; r < rows; ++r)
        {
            if (rowBinDensity[(size_t) r] > 1.0e-6f)
            {
                sumDensity += rowBinDensity[(size_t) r];
                ++nonZeroRows;
            }
        }
        if (nonZeroRows > 0)
            densityAvg = sumDensity / (float) nonZeroRows;
    }

    for (int b = 0; b < numBins; ++b)
    {
        const float mag = mags[b];
        if (! std::isfinite (mag) || mag <= 0.0f) continue;

        const double fc = bandCenterHz (b);
        if (fc < fLo || fc > fHi) continue;
        const double bandLo = juce::jmax (fLo, fc / halfStep);
        const double bandHi = juce::jmin (fHi, fc * halfStep);
        if (bandHi <= bandLo) continue;

        const float db   = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));
        const float tLin = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        const float tone = std::pow (tLin, 0.82f);
        if (tone <= 1.0e-4f) continue;

        const int rTop = juce::jmin (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));
        const int rBot = juce::jmax (freqToGridRow (bandHi, fLo, fHi, rows),
                                     freqToGridRow (bandLo, fLo, fHi, rows));

        for (int r = rTop; r <= rBot; ++r)
        {
            const double ovLo = juce::jmax (bandLo, rowLoHz[(size_t) r]);
            const double ovHi = juce::jmin (bandHi, rowHiHz[(size_t) r]);
            const double ovHz = ovHi - ovLo;
            if (ovHz <= 0.0) continue;

            const float cov = (float) juce::jlimit (0.0, 1.0, ovHz / rowSpanHz[(size_t) r]);
            const float rowLoad = juce::jmax (1.0e-4f, rowBinDensity[(size_t) r]);
            const float densityComp = juce::jlimit (0.35f, 2.0f, densityAvg / rowLoad);
            const float fireProb = juce::jlimit (0.0f, 1.0f,
                                                 (0.06f + 0.94f * tone)
                                                 * cov
                                                 * densityComp
                                                 * 0.92f);

            const juce::uint32 key = seedBase
                                   ^ (juce::uint32) (b * 2654435761u)
                                   ^ (juce::uint32) (r * 40503);
            if (rand01 (key ^ 0x3c6ef372u) >= fireProb)
                continue;

            const float sparkle = 0.86f + 0.14f * rand01 (key ^ 0xa54ff53au);
            const float v = juce::jlimit (0.0f, 1.0f, (0.22f + 0.78f * tone) * sparkle);
            sharpColumn[(size_t) r] = juce::jmax (sharpColumn[(size_t) r], v);
        }
    }

    for (int r = 0; r < rows; ++r)
        set (r, writeCol, sharpColumn[(size_t) r]);

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

    const bool sharpMode = (displayMode == DisplayMode::sharp);
    const float* sharpHiPtr = nullptr;
    const float* sharpLoPtr = nullptr;
    int sharpHiSize = 0;
    int sharpLoSize = 0;
    if (sharpMode)
    {
        sharpHiPtr  = frame.spectrumMag.data();
        sharpLoPtr  = frame.spectrumMagLo.data();
        sharpHiSize = frame.spectrumMagCount;
        sharpLoSize = frame.spectrumMagLoCount;
        if (sharpHiSize <= 0) sharpHiSize = (int) frame.spectrumMag.size();
        if (sharpLoSize < 0)  sharpLoSize = 0;
    }
    else
    {
        // classic 模式保留原有逻辑：按 rows 个对数频率点拉双路合并幅度。
        const double nyquist = hub.getSampleRate() * 0.5;
        const float  fMin    = minFreqHz;
        const float  fMax    = juce::jmin (maxFreqHz, (float) (nyquist * 0.95));
        hub.getSpectrumMagnitudesBlended (rowMagBuf, rows, fMin, fMax);
    }

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
        if (sharpMode)
        {
            sharpNoiseSeed += 0x9e3779b9u;
            if (frame.fourierMode == AnalyserHub::FourierMode::cqt)
            {
                pushSharpColumnFromCqt (sharpHiPtr, sharpHiSize,
                                        hub.getSampleRate(),
                                        sharpNoiseSeed + (juce::uint32) (k * 131u));
            }
            else
            {
                pushSharpColumnFromBins (sharpHiPtr, sharpHiSize,
                                         sharpLoPtr, sharpLoSize,
                                         hub.getSampleRate(),
                                         sharpNoiseSeed + (juce::uint32) (k * 131u));
            }
        }
        else
        {
            pushColumn (rowMagBuf);
        }
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

    const bool isCqt = (hub.getFourierMode() == AnalyserHub::FourierMode::cqt);

    // 测试功能：CQT 模式下绘制按音名频带划分的辅助横线。
    //   每个音名频带由上下两根淡红线围成，右侧中间位置标注红色音名。
    if (isCqt)
    {
        const double logA = std::log10 (fMin);
        const double logB = std::log10 (fMax);
        const double halfStep = std::pow (2.0, 0.5 / (double) kBpo);
        const int midiMin = (int) std::ceil (kA4Midi + kBpo * std::log2 (juce::jmax (1.0, fMin) / kA4Hz));
        const int midiMax = (int) std::floor (kA4Midi + kBpo * std::log2 (juce::jmax (1.0, fMax) / kA4Hz));

        auto hzToY = [&] (double hz) -> int
        {
            const double t = (std::log10 (juce::jlimit (fMin, fMax, hz)) - logA)
                           / juce::jmax (1.0e-9, logB - logA);
            return canvas.getY() + (int) std::round ((1.0 - t) * (double) canvas.getHeight());
        };

        g.setColour (PinkXP::pink300.withAlpha (0.35f));
        for (int midi = midiMin; midi <= midiMax + 1; ++midi)
        {
            const double boundaryHz = midiToHz (midi) / halfStep;
            if (boundaryHz < fMin || boundaryHz > fMax) continue;
            const int y = hzToY (boundaryHz);
            if (y <= canvas.getY() || y >= canvas.getBottom()) continue;
            g.drawHorizontalLine (y, (float) canvas.getX(), (float) canvas.getRight());
        }

        g.setColour (PinkXP::pink600.withAlpha (0.95f));
        for (int midi = midiMin; midi <= midiMax; ++midi)
        {
            const double fc = midiToHz (midi);
            const double fLoBand = juce::jmax (fMin, fc / halfStep);
            const double fHiBand = juce::jmin (fMax, fc * halfStep);
            if (fHiBand <= fLoBand) continue;

            const int yTop = hzToY (fHiBand);
            const int yBot = hzToY (fLoBand);
            if (std::abs (yBot - yTop) < 8) continue;

            const int by = (yTop + yBot) / 2 - 5;
            g.drawText (midiToNote (midi),
                        canvas.getRight() - 34, by, 32, 10,
                        juce::Justification::centredRight, false);
        }
    }

    struct L { double hz; juce::String label; };
    std::vector<L> labels;
    if (isCqt)
    {
        const int midiMin = (int) std::ceil (kA4Midi + kBpo * std::log2 (juce::jmax (1.0, fMin) / kA4Hz));
        const int midiMax = (int) std::floor (kA4Midi + kBpo * std::log2 (juce::jmax (1.0, fMax) / kA4Hz));
        for (int midi = midiMin; midi <= midiMax; ++midi)
        {
            const int pc = (midi % 12 + 12) % 12;
            if (pc != 0 && pc != 9) // C 与 A（每八度 2 个标签，避免拥挤）
                continue;
            labels.push_back ({ midiToHz (midi), midiToNote (midi) });
        }
    }
    else
    {
        labels.push_back ({ 100.0,  "100" });
        labels.push_back ({ 1000.0, "1k"  });
        labels.push_back ({ 10000.0,"10k" });
    }

    // Hz 刻度文字直接浮绘在画布内部左侧（不再从画布外留白区取空间）：
    //   · 文字上面给一小块半透明的暗色背板（不被频谱色块干扰可读）
    //   · 文字用浅色 hl（反白）保证在深色背景上高对比
    for (const auto& l : labels)
    {
        if (l.hz < fMin || l.hz > fMax) continue;

        const double logA = std::log10 (fMin);
        const double logB = std::log10 (fMax);
        const double t    = (std::log10 (l.hz) - logA) / juce::jmax (1.0e-9, logB - logA);
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
