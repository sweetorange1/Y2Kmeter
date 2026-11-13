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
    sibDetectorL.prepare (cachedSampleRate);
    sibDetectorR.prepare (cachedSampleRate);
    setControlPanelEnabled (true);   // v3.1：颜色来源 / 速度 / 双声道 / 齿音阈值都在参数面板里

    // 主题切换时 GL 快照要重建（颜色在快照里）
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        buildGlSnapshot();
        repaint();
    });

    // 默认 PITCH：需要频谱一路来驱动 chroma 跟踪
    hub.retain (AnalyserHub::Kind::Spectrum);
    spectrumRetained = true;
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
// 持久化 + 控制面板（v3.1）
//   键：pixelsPerSecond / colorSource（旧）· dual · sibThreshold（新，缺键 = 旧外观）
//   旧状态只有 displaySeconds（秒），按名义宽度换算成 px/s，保持向后兼容；
//   旧的 frozen 键被忽略（没有锁存式冻结了，按未冻结加载）。
// ----------------------------------------------------------
juce::ValueTree WaveformModule::saveModuleSpecificState() const
{
    juce::ValueTree s("state");
    s.setProperty("pixelsPerSecond", (double) pixelsPerSecond, nullptr);
    s.setProperty("colorSource", (int) colorSource, nullptr);
    s.setProperty("dual", dualChannel, nullptr);
    s.setProperty("sibThreshold", (double) sibThresholdDb, nullptr);
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

    if (state.hasProperty("dual"))
        setDualChannel ((bool) state.getProperty("dual"));
    if (state.hasProperty("sibThreshold"))
        setSibilanceThresholdDb ((float) (double) state.getProperty("sibThreshold"));

    refreshControlPanel();
}

void WaveformModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("colorSource", "Colour by",
        { "Pitch", "Loudness", "Sibilance" },
        "Pitch: hue follows the musical note. Loudness: colour ramps with level (dBFS). "
        "Sibilance: highlights 's' sounds (5 - 12 kHz).").in ("Colours"));
    out.push_back (ParamDef::slider ("sibThreshold", "Sibilance threshold", -60.0, -10.0, 1.0, " dB",
        "How loud the 5 - 12 kHz band must be before it counts as sibilance. "
        "Lower catches fainter 's' sounds, higher keeps only strong ones.")
        .in ("Colours").when ([this] { return colorSource == ColorSource::sibilance; }));

    out.push_back (ParamDef::slider ("speed", "Speed", (double) kMinPixelsPerSecond, (double) kMaxPixelsPerSecond,
        1.0, " px/s",
        "Scroll speed in pixels per second. Faster shows more detail, slower shows a longer history.",
        80.0).in ("Scrolling"));
    out.push_back (ParamDef::toggle ("dual", "Dual channel",
        "Draw the left and right channel as two separate waveforms, left on top and right below.")
        .in ("Scrolling"));
}

double WaveformModule::getParamValue (const juce::String& id) const
{
    if (id == "colorSource")  return (double) (int) colorSource;
    if (id == "sibThreshold") return (double) sibThresholdDb;
    if (id == "speed")        return (double) pixelsPerSecond;
    if (id == "dual")         return dualChannel ? 1.0 : 0.0;
    return 0.0;
}

void WaveformModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "colorSource")  setColorSource ((ColorSource) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "sibThreshold") setSibilanceThresholdDb ((float) v);
    else if (id == "speed")        setPixelsPerSecond ((float) v);
    else if (id == "dual")         setDualChannel (v >= 0.5);
}

void WaveformModule::setSibilanceThresholdDb (float db)
{
    sibThresholdDb = juce::jlimit (-60.0f, -10.0f, db);
    sibDetector.setThresholdDb (sibThresholdDb);
    sibDetectorL.setThresholdDb (sibThresholdDb);
    sibDetectorR.setThresholdDb (sibThresholdDb);
}

// 单 / 双声道切换：已有列的含义不同（A 组 = 混音 or 左声道），清空历史重新开始
void WaveformModule::setDualChannel (bool b)
{
    if (dualChannel == b) return;
    dualChannel = b;
    validCols = 0;
    writeCol  = 0;
    resetAccumulator();
    sibDetector.reset();
    sibDetectorL.reset();
    sibDetectorR.reset();
    buildGlSnapshot();
    repaint();
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
    accMin2 = 1.0f;
    accMax2 = -1.0f;
    accSibMax2 = 0.0f;
    accCount = 0;
    samplesUntilNextColumn = samplesPerColumn;
    clearPendingColumns();
}

// ----------------------------------------------------------
// 列写入节流（突发平滑）
//   pushHistorySamples 只把完整列塞进待写队列，drainPendingColumns 每帧
//   按预算写入环形缓冲。这样"一大块样本一次到达"不会变成"一次跳好几像素"。
// ----------------------------------------------------------
void WaveformModule::clearPendingColumns()
{
    pendingHead  = 0;
    pendingCount = 0;
}

void WaveformModule::queueColumn (const Column& c)
{
    // 队列满：丢最旧的（与"落后太多就放弃旧数据"同语义，避免延迟无限增长）
    if (pendingCount >= kPendingColumnCap)
    {
        pendingHead = (pendingHead + 1) % kPendingColumnCap;
        --pendingCount;
    }

    pendingColumns[(size_t) ((pendingHead + pendingCount) % kPendingColumnCap)] = c;
    ++pendingCount;
}

int WaveformModule::drainPendingColumns (int maxColumns)
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || maxColumns <= 0 || pendingCount <= 0)
        return 0;

    const int n = juce::jmin (maxColumns, pendingCount);

    for (int i = 0; i < n; ++i)
    {
        columnHistory[(size_t) writeCol] = pendingColumns[(size_t) pendingHead];
        pendingHead = (pendingHead + 1) % kPendingColumnCap;
        --pendingCount;

        writeCol = (writeCol + 1) % colCap;
        if (validCols < colCap) ++validCols;
    }

    return n;
}

int WaveformModule::columnBudgetForDt (float dtSec) const
{
    // 每帧允许写入的列数 = 本帧"本应推进的列数"向上取整（≥ 1）。
    //   · ceil 保证预算**永远不小于**名义供给速率，稳态下队列每帧清空，
    //     列率与滚动速度完全不变（只是写入被摊成 1px 步进）；
    //   · 突发（一大块样本一次到达，例如静音补零的第一段）多出来的列按
    //     1 列/帧推进 → 视觉上是连续滑动，而不是"一下跳好几像素"；
    //   · 上限 32：极端长帧（窗口刚从最小化恢复，dt 被钳到 0.1s）也不至于
    //     一帧写进几百列把缓冲刷掉。
    const float nominal = pixelsPerSecond * juce::jmax (0.0f, dtSec);
    int budget = juce::jmax (1, (int) std::ceil (nominal));

    // 积压保护：极端情况下（帧率骤降 / 采样率与列率解算不一致）不让延迟无限增长
    if (pendingCount > 8)
        budget += juce::jmin (4, pendingCount / 8);

    return juce::jlimit (1, 32, budget);
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

void WaveformModule::mouseDown (const juce::MouseEvent& e)
{
    ModulePanel::mouseDown (e);   // 保留基类行为（toFront / 右键菜单 / 边缘拖拽）

    // 只在"左键按在画布内部（避开边缘缩放热区）"时冻结；抽屉是子组件，点击不会走到这里。
    if (e.mods.isLeftButtonDown()
        && getCanvasBounds (getContentBounds()).reduced (6).contains (e.getPosition())
        && ! getControlDrawerBounds().contains (e.getPosition()))
    {
        hold.press();
        // 立即停住：丢掉还没写进环形的待写列和半列累加，按下瞬间画面就不再移动
        pendingHead  = 0;
        pendingCount = 0;
        resetAccumulator();
        repaint();
    }
}

void WaveformModule::mouseUp (const juce::MouseEvent& e)
{
    ModulePanel::mouseUp (e);

    // 松手直接回到实时：onFrame 会把样本游标对齐到最新位置，
    //   冻结期间产生的样本被直接跳过（画面从当前时刻继续滚动）。
    if (hold.active)
    {
        hold.release();
        repaint();   // 去掉"冻结时才有"的音名读数，不等下一帧
    }
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
        sibDetectorL.prepare (cachedSampleRate);
        sibDetectorR.prepare (cachedSampleRate);

        // 每列样本数 = sampleRate / pxPerSec，采样率变了要跟着改
        updateSamplesPerColumn();
        resetAccumulator();
    }

    // 音高（只在 PITCH 模式下订阅了 Spectrum，frame 里才会有新值）
    if (frame.has (AnalyserHub::Kind::Spectrum))
    {
        currentPitchNote = frame.pitch.noteNumber;
        currentPitchConf = frame.pitch.confidence;
    }

    if (columnHistory.empty() || samplesPerColumn <= 0.0f)
        return;

    const int snapshotLen = (int) frame.oscL.size();
    if (snapshotLen <= 0)
        return;

    const juce::uint64 total = frame.oscTotalSamples;

    if (hold.active)
    {
        // 按住 = 画面完全静止（冻结）：不消费任何样本，已写好的列保持原样。
        //   实时游标继续跟随（lastOscTotal），松手后从最新位置接着画 ——
        //   冻结期间产生的样本直接跳过，画面不会补播。
        lastOscTotal  = total;
        oscTotalValid = true;
    }
    else
    {
        // 本帧新增了多少**真实**样本：直接用音频线程的累计写入计数做差。
        //   旧做法用帧时钟估（dt × sampleRate）会和音频实际产出对不齐，周期性
        //   重复消费同一段样本 —— 同一像素列里混进两个相位的波形，画面上就是
        //   "线突然变粗"的疙瘩。
        if (! oscTotalValid || total < lastOscTotal)
        {
            // 首帧 / Hub 刚 prepare 过（计数归零）：只对齐游标，不追加
            lastOscTotal  = total;
            oscTotalValid = true;
            return;
        }

        const juce::uint64 available = total - lastOscTotal;
        lastOscTotal = total;

        if (available > 0)
        {
            // 环形缓冲只保留最近 snapshotLen 个样本：超出的部分只能丢弃（否则会
            //   落后于实时）。正常帧率下 available 远小于 2048，不会触发。
            const int numToAppend = (int) juce::jmin<juce::uint64> (available, (juce::uint64) snapshotLen);

            pushHistorySamples (frame.oscL.data(), frame.oscR.data(), snapshotLen, numToAppend);
        }
    }

    // 把待写列按本帧预算推进环形缓冲（突发数据分几帧摊开，见 columnBudgetForDt）。
    //   没有新样本时也要 drain：上一帧突发留下的积压要继续吃掉，否则会"停一拍再跳"。
    if (drainPendingColumns (columnBudgetForDt (frame.dtSec)) <= 0)
        return;   // 本帧画面没有任何变化，不必重建快照 / 重绘

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

        if (dualChannel)
        {
            // 双声道：A 组 = 左，B 组 = 右；齿音左右各自检测（只看自己的声道）
            const float lc = std::isfinite (l) ? juce::jlimit (-1.0f, 1.0f, l) : 0.0f;
            const float rc = std::isfinite (r) ? juce::jlimit (-1.0f, 1.0f, r) : 0.0f;

            const float sibL = sibDetectorL.pushSample (lc, 0.0f);
            const float sibR = sibDetectorR.pushSample (0.0f, rc);
            if (sibL > accSibMax)  accSibMax  = sibL;
            if (sibR > accSibMax2) accSibMax2 = sibR;

            if (lc < accMin)  accMin  = lc;
            if (lc > accMax)  accMax  = lc;
            if (rc < accMin2) accMin2 = rc;
            if (rc > accMax2) accMax2 = rc;
            accSumSq += lc * lc;
        }
        else
        {
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
        }
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

        if (dualChannel)
        {
            col.minV2 = accMin2;
            col.maxV2 = accMax2;
            col.sib2  = accSibMax2;
            const float peak2 = juce::jmax (std::abs (accMin2), std::abs (accMax2));
            col.db2 = juce::jlimit (-96.0f, 0.0f,
                                    20.0f * std::log10 (juce::jmax (1.0e-6f, peak2)));
        }

        col.pitchNote = currentPitchNote;
        col.pitchConf = currentPitchConf;

        // 不直接写环形缓冲：交给每帧预算摊开（见 drainPendingColumns）
        queueColumn (col);

        accMin = 1.0f; accMax = -1.0f;
        accMin2 = 1.0f; accMax2 = -1.0f;
        accSumSq = 0.0f; accSibMax = 0.0f; accSibMax2 = 0.0f; accCount = 0;

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
                                    ColumnPaint& out, bool chanB) const
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || validCols <= 0 || w <= 0 || innerH <= 0)
        return false;

    const int k = w - 1 - xi;            // xi = 0 最左（最旧）… w-1 最右（最新）
    if (k >= validCols)
        return false;

    // 取声道组：chanB 时把 B 组（右声道）换到 A 组的字段上，之后的解算逻辑完全一致
    auto sampleCol = [&] (int kk) -> Column
    {
        if (kk < 0 || kk >= validCols) return Column{};
        const int idx = (writeCol - 1 - kk + colCap) % colCap;
        Column c = columnHistory[(size_t) idx];
        if (chanB)
        {
            c.minV = c.minV2; c.maxV = c.maxV2; c.db = c.db2; c.sib = c.sib2;
        }
        return c;
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

// 把 inner 区域划成要画的"声道带"：单声道 1 条（整个 inner，旧外观），双声道上下各一条
int WaveformModule::computeLanes (juce::Rectangle<int> inner, Lane (&lanes)[2]) const
{
    if (! dualChannel)
    {
        lanes[0] = { inner.getY(), inner.getHeight(), inner.getY() + inner.getHeight() / 2, false };
        return 1;
    }

    const int gap = 2;
    const int h   = juce::jmax (1, (inner.getHeight() - gap) / 2);
    lanes[0] = { inner.getY(),               h, inner.getY() + h / 2,               false };
    lanes[1] = { inner.getY() + h + gap,     h, inner.getY() + h + gap + h / 2,     true  };
    return 2;
}

// ----------------------------------------------------------
// 布局：v3.1 起没有工具栏和右侧 SPEED 面板，整块内容区都是画布
// ----------------------------------------------------------
juce::Rectangle<int> WaveformModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    return content;
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

    // 双声道标注
    if (dualChannel)
    {
        g.drawText (y2k::tr ("L"), canvas.getX() + 4, canvas.getY() + 4, 16, 12,
                    juce::Justification::centredLeft, false);
        g.drawText (y2k::tr ("R"), canvas.getX() + 4, canvas.getCentreY() + 3, 16, 12,
                    juce::Justification::centredLeft, false);
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

    if (dualChannel)
    {
        g.setColour (PinkXP::ink.withAlpha (0.8f));
        g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
        g.drawText (y2k::tr ("L"), canvas.getX() + 4, canvas.getY() + 4, 16, 12,
                    juce::Justification::centredLeft, false);
        g.drawText (y2k::tr ("R"), canvas.getX() + 4, canvas.getCentreY() + 3, 16, 12,
                    juce::Justification::centredLeft, false);
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

        Lane bgLanes[2];
        const int nBg = computeLanes (inner, bgLanes);

        // 零幅度基准线（每个声道带一条）
        for (int li = 0; li < nBg; ++li)
            appendQuad (snap->vertices,
                        { (float) inner.getX(), (float) bgLanes[li].cy,
                          (float) inner.getWidth(), 1.0f },
                        PinkXP::pink300.withAlpha (0.55f));

        // 双声道：两条带之间一条分隔线
        if (nBg == 2)
            appendQuad (snap->vertices,
                        { (float) inner.getX(), (float) (bgLanes[0].y + bgLanes[0].h),
                          (float) inner.getWidth(), 1.0f },
                        PinkXP::pink400.withAlpha (0.5f));

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

            Lane lanes[2];
            const int nLanes = computeLanes (inner, lanes);

            snap->vertices.reserve (snap->vertices.size() + (size_t) w * 36 * (size_t) nLanes + 64);

            for (int li = 0; li < nLanes; ++li)
            {
                for (int xi = 0; xi < w; ++xi)
                {
                    ColumnPaint p;
                    if (! resolveColumn (xi, w, lanes[li].y, lanes[li].h, lanes[li].cy, p, lanes[li].chanB))
                        continue;

                    appendQuad (snap->vertices,
                                { (float) (inner.getX() + xi), (float) p.yMin,
                                  1.0f, (float) p.hLine },
                                p.colour);
                }
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
        repaintHover (canvas);
    }
}

void WaveformModule::mouseDrag(const juce::MouseEvent& e)
{
    ModulePanel::mouseDrag(e);

    // 按住拖动期间 JUCE 只派发 mouseDrag（mouseMove 被抑制），若不同步更新
    //   hoverPos，十字线与时间 / dBFS 读数就会停在按下那一刻的位置不动。
    //   按住冻结时画面本身不重绘，没有新列顺带刷新，这个问题尤其明显。
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

void WaveformModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在画布内，擦除只需失效画布（不走浮动态限流）
        repaint (getCanvasBounds (getContentBounds()));
    }
}

void WaveformModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const auto inner = canvas.reduced(4);
    if (inner.isEmpty())
        return;

    // 当前悬停点所在的声道带：单声道 = 整个 inner，双声道 = 上 / 下半
    Lane hoverLanes[2];
    const int nHover = computeLanes (inner, hoverLanes);
    int laneIdx = 0;
    if (nHover == 2 && hoverPos.y >= hoverLanes[1].y)
        laneIdx = 1;

    const float halfH = (float) hoverLanes[laneIdx].h * 0.48f;
    const float cy    = (float) hoverLanes[laneIdx].cy;

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

    // PITCH 模式追加音名：只在按住冻结时显示。
    //   · 冻结时画面静止，鼠标所在那一列（时间位置）的音高是确定值，读数跟着指针走；
    //   · 不冻结时列一直向左跑，同一 x 对应的音高每帧都在变，显示出来没有意义，
    //     干脆不显示。
    //   音名取自该列的 pitchNote（PITCH 模式下颜色的含义），纵坐标是电平，没有音名。
    if (colorSource == ColorSource::pitch && hold.active)
    {
        const int colCap = (int) columnHistory.size();
        const int w = juce::jmin (inner.getWidth(), colCap);
        if (colCap > 0 && validCols > 0 && w > 0)
        {
            const int xi = juce::jlimit (0, w - 1, hoverPos.x - inner.getX());
            const int k  = w - 1 - xi;                    // 0 = 最右（最新）
            if (k < validCols)
            {
                const int idx = (writeCol - 1 - k + colCap) % colCap;
                const auto& c = columnHistory[(size_t) idx];
                if (c.pitchConf > 0.06f)
                {
                    const juce::String note = midiNoteName (c.pitchNote);
                    if (note.isNotEmpty())
                        readout += "  " + note;
                }
            }
        }
    }

    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
}

void WaveformModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);

    auto inner = canvas.reduced (2);
    if (inner.isEmpty()) return;

    // 零幅度基准线（双声道时每个声道带一条）
    Lane bgLanes[2];
    const int nBg = computeLanes (inner, bgLanes);
    g.setColour (PinkXP::pink300.withAlpha (0.55f));
    for (int li = 0; li < nBg; ++li)
        g.drawHorizontalLine (bgLanes[li].cy, (float) inner.getX(), (float) inner.getRight());
    if (nBg == 2)
    {
        g.setColour (PinkXP::pink400.withAlpha (0.5f));
        g.drawHorizontalLine (bgLanes[0].y + bgLanes[0].h, (float) inner.getX(), (float) inner.getRight());
    }

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

    Lane lanes[2];
    const int nLanes = computeLanes (canvas.reduced (4), lanes);
    const int w  = juce::jmin (inner.getWidth(), colCap);
    const int ix = inner.getX();

    for (int li = 0; li < nLanes; ++li)
    {
        for (int xi = 0; xi < w; ++xi)
        {
            ColumnPaint p;
            if (! resolveColumn (xi, w, lanes[li].y, lanes[li].h, lanes[li].cy, p, lanes[li].chanB))
                continue;

            g.setColour (p.colour);
            g.fillRect (ix + xi, p.yMin, 1, p.hLine);
        }
    }

    // 左下角说明：当前窗口样本数 / 时长
    g.setColour (PinkXP::ink.withAlpha (0.85f));
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
    g.drawText (y2k::tr (windowHintText (getDisplaySeconds())),
                canvas.getX() + 4, canvas.getBottom() - 14, 220, 12,
                juce::Justification::centredLeft, false);
}
