#include "source/ui/modules/WaveformModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// WaveformModule —— 滚动波形历史
//
// 设计要点：
//   · 零后端新增：波形复用 AnalyserHub::Kind::Oscilloscope；
//     PITCH 染色复用 Kind::Spectrum（只有该模式才订阅，切走即释放）
//   · 每像素列一根 1px 竖柱：min/max 包络 + 峰值 dBFS
//   · 透明度由 dBFS 决定（静音不画）—— 干净观感的关键
//   · 颜色三选一：PITCH（真音高）/ LOUD（响度）/ SIB（齿音）
// ==========================================================

namespace
{
    // 透明度门：低于 kAlphaFloorDb 整列不画，高于 kAlphaFullDb 完全不透明
    constexpr float kAlphaFloorDb = -80.0f;
    constexpr float kAlphaFullDb  = -59.0f;

    // 响度配色的下界（-60 dBFS → 色阶最暗端）
    constexpr float kLoudFloorDb  = -60.0f;

    // 音高染色的音域范围（MIDI 音高）：C0 … C7 → 主题色阶 0 … 1
    constexpr float kPitchLowNote  = 12.0f;
    constexpr float kPitchHighNote = 96.0f;

    juce::String windowHintText (float seconds)
    {
        // 界面语言：模板翻译（zh 里 "（最新 →）"），数值保持原格式
        return y2k::trf ("{0} s  (newest \xe2\x86\x92)", juce::String (seconds, 1));
    }

    juce::String formatRelativeTime (float seconds)
    {
        return (std::abs (seconds) >= 1.0f)
            ? juce::String (seconds, 2) + " s"
            : juce::String (seconds * 1000.0f, 0) + " ms";
    }

    juce::String midiNoteName (float noteNumber)
    {
        if (! std::isfinite (noteNumber) || noteNumber < 12.0f || noteNumber > 132.0f)
            return {};

        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
        const int n   = juce::roundToInt (noteNumber);
        const int pc  = ((n % 12) + 12) % 12;
        const int oct = n / 12 - 1;
        return juce::String (names[pc]) + juce::String (oct);
    }
}

WaveformModule::WaveformModule (AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::waveform),
      hub(h)
{
    // 按需启用 Oscilloscope 计算（与其它模块一致）
    hub.retain (AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener (this);

    setMinSize(64, 64);
    setDefaultSize(384, 256);

    cachedSampleRate = hub.getSampleRate();
    if (cachedSampleRate <= 0.0) cachedSampleRate = 48000.0;

    // 齿音检测器（带通 + 包络 + 迟滞门限）
    sibDetector.prepare (cachedSampleRate);

    // ---- 颜色来源：单个按钮循环 PITCH → LOUD → SIB ----
    //   旧的三个互斥按钮占掉了大半个工具栏；合成一个后按钮文字就是当前模式，
    //   单击即切到下一档（默认 PITCH），tooltip 里写明完整循环顺序。
    btnColourMode.setClickingTogglesState (false);
    btnColourMode.setTooltip (y2k::help::make ("Colour source",
        "Click to cycle what the colours mean:\n"
        "- PITCH: hue follows the musical note (chroma)\n"
        "- LOUD: colour ramps with level (dBFS)\n"
        "- SIB: highlights sibilance (5 - 12 kHz 's' sounds)"));
    btnColourMode.onClick = [this]()
    {
        setColorSource (nextColorSource (colorSource));
    };
    addAndMakeVisible (btnColourMode);

    btnFreeze.setClickingTogglesState (true);
    btnFreeze.setTooltip (y2k::help::make ("Freeze", "Stop scrolling to inspect the history. Click again to resume."));
    btnFreeze.onClick = [this]() { setFrozen (btnFreeze.getToggleState()); };
    addAndMakeVisible (btnFreeze);

    // ---- SPEED：滚动速度（像素列/秒），与 SpectrogramModule 同一套口径 ----
    //   顶 = 快（400 px/s），底 = 慢（10 px/s）；默认 120 px/s。
    //   skew 把行程往低速端倾斜：10..400 跨 40 倍，线性的话 10~40 只占 7% 行程。
    speedSlider.setSliderStyle (juce::Slider::LinearVertical);
    speedSlider.setRange ((double) kMinPixelsPerSecond, (double) kMaxPixelsPerSecond, 1.0);
    speedSlider.setSkewFactorFromMidPoint (80.0);
    speedSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, true, speedPanelW - 2, 16);
    speedSlider.setColour (juce::Slider::textBoxTextColourId,       PinkXP::ink);
    speedSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    speedSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    speedSlider.setValue ((double) pixelsPerSecond, juce::dontSendNotification);
    speedSlider.onValueChange = [this]()
    {
        setPixelsPerSecond ((float) speedSlider.getValue());
    };
    addAndMakeVisible (speedSlider);

    speedLabel.setJustificationType (juce::Justification::centred);
    speedLabel.setColour (juce::Label::textColourId, PinkXP::ink);
    speedLabel.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
    speedLabel.setText ("SPEED", juce::dontSendNotification);
    speedSlider.setTooltip (y2k::help::make ("Speed",
        "Scroll speed in pixels per second (10 - 400). Faster shows more detail, "
        "slower shows a longer history."));
    speedLabel.setTooltip (speedSlider.getTooltip());
    addAndMakeVisible (speedLabel);

    // 主题切换时重新下发文字颜色（Label / TextBox 都缓存颜色）
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        speedLabel.setColour (juce::Label::textColourId, PinkXP::ink);
        speedSlider.setColour (juce::Slider::textBoxTextColourId, PinkXP::ink);
        speedLabel.repaint();
        speedSlider.repaint();
    });

    refreshColourButton();

    // 默认 PITCH：需要频谱一路来驱动 chroma 跟踪
    hub.retain (AnalyserHub::Kind::Spectrum);
    spectrumRetained = true;

    speedSlider.updateText();
}

WaveformModule::~WaveformModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Oscilloscope);

    if (spectrumRetained)
    {
        hub.release (AnalyserHub::Kind::Spectrum);
        spectrumRetained = false;
    }
}

// ----------------------------------------------------------
// 持久化
//   新字段：pixelsPerSecond / colorSource
//   旧状态只有 displaySeconds（秒），按名义宽度换算成 px/s，保持向后兼容
// ----------------------------------------------------------
juce::ValueTree WaveformModule::saveModuleSpecificState() const
{
    juce::ValueTree s("state");
    s.setProperty("pixelsPerSecond", (double) pixelsPerSecond, nullptr);
    s.setProperty("colorSource", (int) colorSource, nullptr);
    s.setProperty("frozen", frozen, nullptr);
    return s;
}

void WaveformModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
    if (state.hasProperty("pixelsPerSecond"))
    {
        setPixelsPerSecond ((float) (double) state.getProperty("pixelsPerSecond"));
    }
    else if (state.hasProperty("displaySeconds"))
    {
        // 旧存档只有"窗口秒数"（3s / 6s 档）：按名义 384 px 宽换算成 px/s，
        //   3 s → 128 px/s、6 s → 64 px/s，尽量沿用用户原来的观感。
        const double secs = juce::jmax (0.05, (double) state.getProperty("displaySeconds"));
        setPixelsPerSecond ((float) (384.0 / secs));
    }

    if (state.hasProperty("colorSource"))
        setColorSource ((ColorSource) juce::jlimit (0, 2, (int) state.getProperty("colorSource")));
    else if (state.hasProperty("sibilance") && (bool) state.getProperty("sibilance"))
        setColorSource (ColorSource::sibilance);

    if (state.hasProperty("frozen"))
        setFrozen ((bool) state.getProperty("frozen"));
}

// ----------------------------------------------------------
// 颜色来源切换：PITCH 模式才订阅频谱（切换即时释放，避免空跑 FFT）
// ----------------------------------------------------------
void WaveformModule::setColorSource (ColorSource s)
{
    const bool wantSpectrum = (s == ColorSource::pitch);

    if (wantSpectrum != spectrumRetained)
    {
        if (wantSpectrum) hub.retain (AnalyserHub::Kind::Spectrum);
        else              hub.release (AnalyserHub::Kind::Spectrum);

        spectrumRetained = wantSpectrum;
    }

    if (colorSource != s)
    {
        colorSource = s;
        buildGlSnapshot();
        repaint();
    }

    refreshColourButton();
}

WaveformModule::ColorSource WaveformModule::nextColorSource (ColorSource s) noexcept
{
    switch (s)
    {
        case ColorSource::pitch:      return ColorSource::loudness;
        case ColorSource::loudness:   return ColorSource::sibilance;
        case ColorSource::sibilance:  return ColorSource::pitch;
    }

    return ColorSource::pitch;
}

const char* WaveformModule::colorSourceName (ColorSource s) noexcept
{
    switch (s)
    {
        case ColorSource::pitch:      return "PITCH";
        case ColorSource::loudness:   return "LOUD";
        case ColorSource::sibilance:  return "SIB";
    }

    return "PITCH";
}

void WaveformModule::refreshColourButton()
{
    btnColourMode.setButtonText (colorSourceName (colorSource));
}

// ----------------------------------------------------------
// 滚动速度：pixelsPerSecond（像素列/秒）
//   列推进 = 每秒 pixelsPerSecond 个像素列，**与模块宽度无关**：
//   拉宽模块只是窗口里装下更长的历史，画面推进速度不变。
//   每列聚合的样本数 = sampleRate / pixelsPerSecond。
// ----------------------------------------------------------
void WaveformModule::setPixelsPerSecond (float pxPerSec)
{
    pxPerSec = juce::jlimit (kMinPixelsPerSecond, kMaxPixelsPerSecond, pxPerSec);
    if (std::abs (pxPerSec - pixelsPerSecond) < 0.01f)
        return;

    pixelsPerSecond = pxPerSec;
    updateSamplesPerColumn();
    resetAccumulator();

    speedSlider.setValue ((double) pixelsPerSecond, juce::dontSendNotification);
    speedSlider.updateText();

    buildGlSnapshot();
    repaint();
}

float WaveformModule::getDisplaySeconds() const noexcept
{
    const int w = (cachedCanvasWidth > 0) ? cachedCanvasWidth : 384;
    return (pixelsPerSecond > 0.0f) ? (float) w / pixelsPerSecond : 3.0f;
}

void WaveformModule::updateSamplesPerColumn()
{
    if (cachedSampleRate <= 0.0 || pixelsPerSecond <= 0.0f)
        return;

    // 下限 1 样本/列：极慢速度 + 极宽模块时避免出现空列
    samplesPerColumn = juce::jmax (1.0f, (float) (cachedSampleRate / (double) pixelsPerSecond));
}

void WaveformModule::resetAccumulator()
{
    accMin = 1.0f;
    accMax = -1.0f;
    accSumSq = 0.0f;
    accSibMax = 0.0f;
    accCount = 0;
    samplesUntilNextColumn = samplesPerColumn;
}

// ----------------------------------------------------------
// 画布宽度变化时重建列缓冲（保留最近 min(validCols, newWidth) 列）
// ----------------------------------------------------------
void WaveformModule::rebuildColumnBuffer (int newCanvasWidth)
{
    newCanvasWidth = juce::jmax (1, newCanvasWidth);
    if (newCanvasWidth == cachedCanvasWidth && ! columnHistory.empty())
        return;

    std::vector<Column> newBuf ((size_t) newCanvasWidth);

    // 拷贝最近 keep 列到新缓冲的末尾（保持"最新在右端"）
    const int keep = juce::jmin (validCols, newCanvasWidth);
    const int oldSize = (int) columnHistory.size();
    for (int i = 0; i < keep; ++i)
    {
        const int k = keep - 1 - i;
        const int srcIdx = (writeCol - 1 - k + oldSize) % juce::jmax (1, oldSize);
        if (srcIdx >= 0 && srcIdx < oldSize)
            newBuf[(size_t) (newCanvasWidth - keep + i)] = columnHistory[(size_t) srcIdx];
    }

    columnHistory   = std::move (newBuf);
    writeCol        = newCanvasWidth % (int) columnHistory.size();
    validCols       = keep;
    cachedCanvasWidth = newCanvasWidth;

    updateSamplesPerColumn();
    resetAccumulator();
}

void WaveformModule::setFrozen (bool b)
{
    if (frozen == b) return;
    frozen = b;
    btnFreeze.setToggleState (frozen, juce::dontSendNotification);
    buildGlSnapshot();   // 解冻时数据可能已过期，立即重建快照
    repaint();
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调
// ----------------------------------------------------------
void WaveformModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    // 采样率变了（宿主换设备 / 换工程）：重算滤波器系数、每列样本数与时间读数。
    //   samplesPerColumn = sampleRate / pxPerSec，所以采样率变化必须跟着改。
    const double srNow = hub.getSampleRate();
    if (srNow > 0.0 && std::abs (srNow - cachedSampleRate) > 0.5)
    {
        cachedSampleRate = srNow;

        sibDetector.prepare (cachedSampleRate);

        // 每列样本数 = sampleRate / pxPerSec，采样率变了要跟着改
        updateSamplesPerColumn();
        resetAccumulator();

        speedSlider.updateText();
    }

    // 音高（只在 PITCH 模式下订阅了 Spectrum，frame 里才会有新值）
    if (frame.has (AnalyserHub::Kind::Spectrum))
    {
        currentPitchNote = frame.pitch.noteNumber;
        currentPitchConf = frame.pitch.confidence;
    }

    // 冻结态不再推数据，也不再强制 repaint（setFrozen 已触发一次重绘）
    if (frozen)
        return;

    if (columnHistory.empty() || samplesPerColumn <= 0.0f)
        return;

    const int snapshotLen = (int) frame.oscL.size();
    if (snapshotLen <= 0)
        return;

    // 本帧新增了多少**真实**样本：直接用音频线程的累计写入计数做差。
    //   旧做法用帧时钟估（dt × sampleRate）会和音频实际产出对不齐，周期性
    //   重复消费同一段样本 —— 同一像素列里混进两个相位的波形，画面上就是
    //   "线突然变粗"的疙瘩。
    const juce::uint64 total = frame.oscTotalSamples;

    if (! oscTotalValid || total < lastOscTotal)
    {
        // 首帧 / Hub 刚 prepare 过（计数归零）：只对齐游标，不追加
        lastOscTotal  = total;
        oscTotalValid = true;
        return;
    }

    const juce::uint64 available = total - lastOscTotal;
    lastOscTotal = total;

    if (available == 0)
        return;

    // 环形缓冲只保留最近 snapshotLen 个样本：超出的部分只能丢弃（否则会
    //   落后于实时）。正常帧率下 available 远小于 2048，不会触发。
    const int numToAppend = (int) juce::jmin<juce::uint64> (available, (juce::uint64) snapshotLen);

    pushHistorySamples (frame.oscL.data(), frame.oscR.data(), snapshotLen, numToAppend);

    buildGlSnapshot();

    // 每个分发帧只重绘 canvas 区域
    repaint (getCanvasBounds (getContentBounds()));
}

// ----------------------------------------------------------
// 把 oscilloscope 快照末尾的 numToAppend 个样本合并成 mono，
// 每 samplesPerColumn 个样本聚合成 1 列写入 columnHistory。
//
// 列推进用"倒计时"：
//   samplesUntilNextColumn 递减到 0 就产出一列，然后 += samplesPerColumn。
//   samplesPerColumn = sampleRate / pixelsPerSecond，不必是整数，
//   长期平均列率严格等于 pixelsPerSecond 列/秒，且**每个样本只被统计一次**。
// ----------------------------------------------------------
void WaveformModule::pushHistorySamples (const float* L, const float* R,
                                         int snapshotLen, int numToAppend)
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || samplesPerColumn <= 0.0f) return;

    // 快照里最新样本在 snapshotLen-1，最旧在 0；取最末 numToAppend 个
    const int srcStart = snapshotLen - numToAppend;
    if (srcStart < 0) return;

    for (int i = 0; i < numToAppend; ++i)
    {
        const float l = L[srcStart + i];
        const float r = R[srcStart + i];
        float m = 0.5f * (l + r);
        if (! std::isfinite (m)) m = 0.0f;
        m = juce::jlimit (-1.0f, 1.0f, m);

        // 齿音：逐样本推进检测器（立体声独立处理后再联动，不做 (L+R)/2 降混）。
        //   状态跨列连续，时间常数固定 —— 与 SPEED / 列宽无关。
        const float sib = sibDetector.pushSample (l, r);
        if (sib > accSibMax) accSibMax = sib;

        if (m < accMin) accMin = m;
        if (m > accMax) accMax = m;
        accSumSq += m * m;
        ++accCount;

        samplesUntilNextColumn -= 1.0f;
        if (samplesUntilNextColumn > 0.0f)
            continue;

        Column col;
        col.minV  = accMin;
        col.maxV  = accMax;

        const float inv = 1.0f / (float) juce::jmax (1, accCount);
        col.rms   = std::sqrt (accSumSq * inv);
        col.sib   = accSibMax;                    // 本列齿音峰值（包络已经很平滑）

        const float peak = juce::jmax (std::abs (accMin), std::abs (accMax));
        col.db = juce::jlimit (-96.0f, 0.0f,
                               20.0f * std::log10 (juce::jmax (1.0e-6f, peak)));

        col.pitchNote = currentPitchNote;
        col.pitchConf = currentPitchConf;

        columnHistory[(size_t) writeCol] = col;
        writeCol = (writeCol + 1) % colCap;
        if (validCols < colCap) ++validCols;

        accMin = 1.0f; accMax = -1.0f;
        accSumSq = 0.0f; accSibMax = 0.0f; accCount = 0;

        samplesUntilNextColumn += samplesPerColumn;
        if (samplesUntilNextColumn <= 0.0f)   // samplesPerColumn < 1 的保护
            samplesUntilNextColumn = samplesPerColumn;
    }
}

// ----------------------------------------------------------
// 颜色映射
// ----------------------------------------------------------

// 音高染色：音域（低音 → 高音）映射到主题 chartRamp（与 LOUD 模式同一套色阶）
//   置信度低（噪声 / 无明确音高）→ 往同亮度中性灰靠，避免满屏乱跳
juce::Colour WaveformModule::mapPitchToColour (float noteNumber, float confidence, float level01) const
{
    const float t = juce::jlimit (0.0f, 1.0f,
                                  (noteNumber - kPitchLowNote) / (kPitchHighNote - kPitchLowNote));

    const juce::Colour rampCol = PinkXP::sampleChartRamp (t);
    const juce::Colour neutral = PinkXP::ink.withMultipliedSaturation (0.25f);

    const float w = juce::jlimit (0.0f, 1.0f, (confidence - 0.05f) / 0.45f);
    const juce::Colour c = PinkXP::interpolateOkLab (neutral, rampCol, 0.30f + 0.70f * w);

    // 电平轻微提亮：弱信号不刺眼，强信号更"实"
    const float level = juce::jlimit (0.0f, 1.0f, level01);
    return c.withMultipliedBrightness (0.80f + 0.24f * level);
}

// 响度染色：dBFS → 主题 chartRamp（0.9 以上向 sel 过渡，接近 clip 更醒目）
juce::Colour WaveformModule::mapLoudnessToColour (float db) const
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - kLoudFloorDb) / -kLoudFloorDb);
    const float s = std::pow (t, 0.75f);

    if (s >= 0.9f)
    {
        const float u = (s - 0.9f) / 0.1f;
        return PinkXP::interpolateOkLab (PinkXP::chartRamp[7], PinkXP::sel, u);
    }

    return PinkXP::sampleChartRamp (s / 0.9f);
}

// 齿音模式：没有齿音的列压成"低存在感灰"，有齿音的列用红色（强度 → 亮度 / 不透明度）
//   透明度不再跟电平走 —— 齿音是短促爆发，安静也要能看见；真静音在 dB 门限那里
//   已经被整列丢弃，所以不会在无声处留下灰线。
WaveformModule::ColumnStyle WaveformModule::columnStyle (const Column& c,
                                                         float level01,
                                                         float levelAlpha) const
{
    switch (colorSource)
    {
        case ColorSource::pitch:
            return { mapPitchToColour (c.pitchNote, c.pitchConf, level01), levelAlpha };

        case ColorSource::loudness:
            return { mapLoudnessToColour (c.db), levelAlpha };

        case ColorSource::sibilance:
        {
            const float drive = juce::jlimit (0.0f, 1.0f, c.sib);

            // 无齿音：低存在感灰（能看出波形走向，但不抢注意力）
            if (drive < 0.02f)
            {
                const juce::Colour grey = PinkXP::ink.withMultipliedSaturation (0.18f)
                                                       .withMultipliedBrightness (0.85f);
                return { grey, juce::jmin (levelAlpha, 0.13f) };
            }

            // 有齿音：暗红 → 亮红，同时抬不透明度
            const float t = std::pow (drive, 0.7f);
            const juce::Colour red = PinkXP::interpolateOkLab (
                PinkXP::danger.withMultipliedBrightness (0.55f), PinkXP::danger, t);

            return { red, juce::jmax (levelAlpha * 0.5f, 0.30f + 0.70f * t) };
        }
    }

    return { PinkXP::pink500, levelAlpha };
}

// ----------------------------------------------------------
// 单列解算（GL / legacy 共用）：
//   · min/max 做 3-tap 水平平滑（只糊包络，不糊色相）
//   · 峰值 dBFS → 透明度（静音列直接丢弃）
//   · 颜色按当前 ColorSource 决定
// 返回 false 表示该列不画（静音 / 无数据）
// ----------------------------------------------------------
bool WaveformModule::resolveColumn (int xi, int w, int innerY, int innerH, int centreY,
                                    ColumnPaint& out) const
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || validCols <= 0 || w <= 0 || innerH <= 0)
        return false;

    const int k = w - 1 - xi;            // xi = 0 最左（最旧）… w-1 最右（最新）
    if (k >= validCols)
        return false;

    auto sampleCol = [&] (int kk) -> Column
    {
        if (kk < 0 || kk >= validCols) return Column{};
        const int idx = (writeCol - 1 - kk + colCap) % colCap;
        return columnHistory[(size_t) idx];
    };

    const Column c0 = sampleCol (juce::jmax (0, k - 1));
    const Column c1 = sampleCol (k);
    const Column c2 = sampleCol (juce::jmin (validCols - 1, k + 1));

    const float minV = (c0.minV + c1.minV + c2.minV) * (1.0f / 3.0f);
    const float maxV = (c0.maxV + c1.maxV + c2.maxV) * (1.0f / 3.0f);
    const float db   = (c0.db   + c1.db   + c2.db  ) * (1.0f / 3.0f);

    const float alpha = juce::jlimit (0.0f, 1.0f,
                                      (db - kAlphaFloorDb) / (kAlphaFullDb - kAlphaFloorDb));
    if (alpha < 0.02f)
        return false;                     // 静音列整列不画

    const float level01 = juce::jlimit (0.0f, 1.0f,
                                        (db - kLoudFloorDb) / -kLoudFloorDb);

    const float halfH = (float) innerH * 0.48f;
    const int   cy    = centreY;

    const int yTopRaw = cy - (int) std::round (maxV * halfH);
    const int yBotRaw = cy - (int) std::round (minV * halfH);

    const int yMin = juce::jlimit (innerY, innerY + innerH, juce::jmin (yTopRaw, yBotRaw));
    const int yMax = juce::jlimit (innerY, innerY + innerH, juce::jmax (yTopRaw, yBotRaw));

    out.yMin  = yMin;
    out.hLine = juce::jmax (1, yMax - yMin);   // 静音附近也保留 1 px 细线

    // 颜色 + 透明度：SIB 模式下存在感由齿音强度决定，其余由电平决定
    //   色相不做跨列平均（圆周量线性平均会串色），直接用本列自己的估计
    const auto style = columnStyle (c1, level01, alpha);
    out.colour = style.colour.withAlpha (juce::jlimit (0.0f, 1.0f, style.alpha));
    return true;
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> WaveformModule::getToolbarBounds (juce::Rectangle<int> content) const
{
    return content.withHeight (toolbarH);
}

juce::Rectangle<int> WaveformModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    // 顶部扣掉工具栏，右侧扣掉 SPEED 面板
    return content.withTrimmedTop (toolbarH + 4)
                  .withTrimmedRight (speedPanelW);
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：人浪（§8.3）
// ----------------------------------------------------------
bool WaveformModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void WaveformModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    ctx.addModuleActor (makeWaveBandActor (hit.pos, hit.rect));

    // 时间 + 电平读数（反馈 C3：补上点击坐标的电平参数）
    const float t = (hit.pos.x - hit.rect.getX()) / juce::jmax (1.0f, hit.rect.getWidth());
    const float timeSec = -(1.0f - t) * getDisplaySeconds();
    const float pk = toyReadColumnPeak (hit.pos.x);

    juce::String text;
    if (pk < 0.003f)
        text = "SILENCE";
    else
    {
        const float db = 20.0f * std::log10 (pk + 1e-9f);
        text = formatRelativeTime (timeSec) + "  " + juce::String (db, 1) + "dB";
    }
    ctx.bubble (hit.pos.translated (0.0f, -20.0f), text, ToyRole::Reward);
}

void WaveformModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto tb = getToolbarBounds (contentBounds).reduced (2);
    const int gap = 4;

    // 右侧：Freeze（先算，左侧按钮按剩余宽度自适应，窄模块下不会互相压住）
    const int freezeW = juce::jmin (60, juce::jmax (0, tb.getWidth() / 2 - gap));
    btnFreeze.setBounds (tb.getRight() - freezeW, tb.getY(), freezeW, tb.getHeight());

    // 左侧：颜色来源循环按钮（文字在 PITCH / LOUD / SIB 之间切换）
    const int colourW = juce::jlimit (24, 64, tb.getWidth() - freezeW - gap);
    btnColourMode.setBounds (tb.getX(), tb.getY(), colourW, tb.getHeight());

    // 右侧 SPEED 面板（垂直滑条 + 顶部 Label）
    auto contentInner = contentBounds;
    contentInner.removeFromTop (toolbarH + 4);
    auto speedPanel = contentInner.removeFromRight (speedPanelW);
    speedLabel.setBounds (speedPanel.removeFromTop (speedLabelH));
    speedSlider.setBounds (speedPanel);

    // 画布宽度变化（含首次布局）时重建列缓冲 —— 列数严格 = 画布内部宽
    auto canvas = getCanvasBounds (contentBounds).reduced (4);
    rebuildColumnBuffer (juce::jmax (1, canvas.getWidth()));

    // 几何变化 → 立即重建 GL 快照（尺寸守卫防 layout 风暴）
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
        buildGlSnapshot();
}

// ----------------------------------------------------------
// GlDirectModuleBase 钩子
// ----------------------------------------------------------
juce::Rectangle<int> WaveformModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void WaveformModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 窗口样本数提示（GL 路径下文字必须画在 overlay 里）
    g.setColour (PinkXP::ink.withAlpha (0.8f));
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
    g.drawText (y2k::tr (windowHintText (getDisplaySeconds())),
                canvas.getX() + 4, canvas.getBottom() - 14, 220, 12,
                juce::Justification::centredLeft, false);

    // 右上角冻结标记
    if (frozen)
    {
        auto dot = juce::Rectangle<int> (canvas.getRight() - 12, canvas.getY() + 6, 8, 8);
        g.setColour (PinkXP::sel);
        g.fillEllipse (dot.toFloat());
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText (y2k::tr ("FROZEN"), canvas.getRight() - 60, canvas.getY() + 4,
                    42, 12, juce::Justification::centredRight, false);
    }

    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void WaveformModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    auto canvas = getCanvasBounds (contentBounds);
    drawBackground (g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    drawWaveform (g, canvas);

    if (frozen)
    {
        auto dot = juce::Rectangle<int> (canvas.getRight() - 12, canvas.getY() + 6, 8, 8);
        g.setColour (PinkXP::sel);
        g.fillEllipse (dot.toFloat());
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText (y2k::tr ("FROZEN"), canvas.getRight() - 60, canvas.getY() + 4,
                    42, 12, juce::Justification::centredRight, false);
    }

    if (hoverActive)
        drawHoverRuler (g, canvas);
}

// ----------------------------------------------------------
// buildGlSnapshot —— 波形 GL 顶点流
//   布局：[凹陷背景 + 中心线 + 时间分区虚线] + [每列 1 px 宽垂直柱]
// ----------------------------------------------------------
void WaveformModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;

    const auto canvas = getCanvasBounds (getContentBounds());
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    auto snap = beginGlSnapshot();
    // 逻辑分辨率 FBO 渲染：150% DPI 下 1 逻辑像素宽的柱在物理像素网格上无解，
    //   只有"逻辑域渲染 + 线性放大 blit"与旧版逐像素一致。
    snap->renderToLogical = true;
    snap->logicalW = canvas.getWidth();
    snap->logicalH = canvas.getHeight();
    snap->canvasLocal = canvas;

    // ---- 背景 ----
    {
        appendSunkenFrame (snap->vertices, canvas);

        auto inner = canvas.reduced (2);

        // 零幅度基准线
        appendQuad (snap->vertices,
                    { (float) inner.getX(), (float) inner.getCentreY(),
                      (float) inner.getWidth(), 1.0f },
                    PinkXP::pink300.withAlpha (0.55f));

        // 3 条竖向分区虚线（1/4、1/2、3/4）
        const auto dash = PinkXP::pink200.withAlpha (0.35f);
        for (int i = 1; i <= 3; ++i)
        {
            const int x = inner.getX() + (inner.getWidth() * i) / 4;
            for (int y = inner.getY(); y < inner.getBottom(); y += 4)
                appendQuad (snap->vertices, { (float) x, (float) y, 1.0f, 2.0f }, dash);
        }
    }

    // ---- 主体：每列 1 px 垂直柱 ----
    {
        const int colCap = (int) columnHistory.size();
        auto inner = canvas.reduced (4);

        if (colCap > 0 && validCols > 0 && inner.getWidth() > 0 && inner.getHeight() > 0)
        {
            const int w  = juce::jmin (inner.getWidth(), colCap);
            const int iy = inner.getY();
            const int ih = inner.getHeight();
            const int cy = iy + ih / 2;

            snap->vertices.reserve (snap->vertices.size() + (size_t) w * 36 + 64);

            for (int xi = 0; xi < w; ++xi)
            {
                ColumnPaint p;
                if (! resolveColumn (xi, w, iy, ih, cy, p))
                    continue;

                appendQuad (snap->vertices,
                            { (float) (inner.getX() + xi), (float) p.yMin,
                              1.0f, (float) p.hLine },
                            p.colour);
            }
        }
    }

    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// 鼠标悬停标尺
// ----------------------------------------------------------
void WaveformModule::mouseMove(const juce::MouseEvent& e)
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

void WaveformModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint();
    }
}

void WaveformModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const auto inner = canvas.reduced(4);
    if (inner.isEmpty())
        return;

    const float halfH = (float) inner.getHeight() * 0.48f;
    const float cy    = (float) inner.getCentreY();

    // X → 相对时间（秒，负 = 过去，最右 = 当前）
    const float t = (float)(hoverPos.x - inner.getRight())
                  / juce::jmax(1, inner.getWidth());
    const float timeSec = t * getDisplaySeconds();

    // Y → 幅度（±1 满幅）→ dBFS
    const float amp = (cy - (float) hoverPos.y) / juce::jmax(1.0f, halfH);
    const float mag = juce::jlimit(0.0f, 1.0f, std::abs(amp));
    const float db  = juce::Decibels::gainToDecibels(juce::jmax(1.0e-4f, mag));

    juce::String readout = formatRelativeTime (timeSec) + "  "
                         + juce::String (db, 1) + " dBFS";

    // PITCH 模式追加当前音名，方便肉眼验证跟踪是否合理
    if (colorSource == ColorSource::pitch && currentPitchConf > 0.06f)
    {
        const juce::String note = midiNoteName (currentPitchNote);
        if (note.isNotEmpty())
            readout += "  " + note;
    }

    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
}

void WaveformModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);

    auto inner = canvas.reduced (2);
    if (inner.isEmpty()) return;

    // 零幅度基准线
    g.setColour (PinkXP::pink300.withAlpha (0.55f));
    g.drawHorizontalLine (inner.getCentreY(), (float) inner.getX(), (float) inner.getRight());

    // 3 条竖向分区虚线
    g.setColour (PinkXP::pink200.withAlpha (0.35f));
    for (int i = 1; i <= 3; ++i)
    {
        const int x = inner.getX() + (inner.getWidth() * i) / 4;
        for (int y = inner.getY(); y < inner.getBottom(); y += 4)
            g.fillRect (x, y, 1, 2);
    }
}

// ----------------------------------------------------------
// drawWaveform —— 瀑布主体（legacy / 无 GL 回退路径）
//   与 GL 路径共用 resolveColumn，保证两条路径观感一致。
//   旧实现的 32 档颜色分桶在这里不再需要：颜色已是逐列函数，
//   分桶只会把音高色相量化成 12 段之外的台阶。
// ----------------------------------------------------------
void WaveformModule::drawWaveform (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || validCols <= 0) return;

    auto inner = canvas.reduced (4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int w  = juce::jmin (inner.getWidth(), colCap);
    const int ix = inner.getX();
    const int iy = inner.getY();
    const int ih = inner.getHeight();
    const int cy = iy + ih / 2;

    for (int xi = 0; xi < w; ++xi)
    {
        ColumnPaint p;
        if (! resolveColumn (xi, w, iy, ih, cy, p))
            continue;

        g.setColour (p.colour);
        g.fillRect (ix + xi, p.yMin, 1, p.hLine);
    }

    // 左下角说明：当前窗口样本数 / 时长
    g.setColour (PinkXP::ink.withAlpha (0.85f));
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
    g.drawText (y2k::tr (windowHintText (getDisplaySeconds())),
                canvas.getX() + 4, canvas.getBottom() - 14, 220, 12,
                juce::Justification::centredLeft, false);
}
