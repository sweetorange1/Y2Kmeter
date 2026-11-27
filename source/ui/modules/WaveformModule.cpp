#include "source/ui/modules/WaveformModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <cstring>

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

    // 音高染色的音乐音域范围（MIDI 音高）：C1 (32.7 Hz) … C7 (2093 Hz) → 主题高亮色阶
    constexpr float kPitchLowNote  = 24.0f;
    constexpr float kPitchHighNote = 96.0f;

    juce::String windowHintText (float seconds, bool rightwards)
    {
        if (rightwards)
            return y2k::trf (juce::String::fromUTF8 ("(\xe2\x86\x90 newest)  {0} s"), juce::String (seconds, 1));
        // 界面语言：模板翻译（zh 里 "（最新 →）"），数值保持原格式
        return y2k::trf (juce::String::fromUTF8 ("{0} s  (newest \xe2\x86\x92)"), juce::String (seconds, 1));
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

    // 预乘 BGRA 打包（小端 0xAARRGGBB = OpenGL GL_BGRA 与 JUCE SoftwareImageType PixelARGB 完全一致）
    inline juce::uint32 packPremulBGRA (int r, int g, int b, int a) noexcept
    {
        a = juce::jlimit (0, 255, a);
        const juce::uint32 ua = (juce::uint32) a;
        const juce::uint32 ur = ((juce::uint32) juce::jlimit (0, 255, r) * ua + 127u) / 255u;
        const juce::uint32 ug = ((juce::uint32) juce::jlimit (0, 255, g) * ua + 127u) / 255u;
        const juce::uint32 ub = ((juce::uint32) juce::jlimit (0, 255, b) * ua + 127u) / 255u;
        return (ua << 24) | (ur << 16) | (ug << 8) | ub;
    }

    inline juce::uint32 packPremulColour (juce::Colour c) noexcept
    {
        return packPremulBGRA ((int) c.getRed(), (int) c.getGreen(), (int) c.getBlue(), (int) c.getAlpha());
    }

    // Porter-Duff SrcOver 预乘合成：dst = src + dst * (1 - srcA)
    inline void blendPremulOver (juce::uint32& dst, juce::uint32 src) noexcept
    {
        const juce::uint32 sa = src >> 24;
        if (sa == 0u) return;
        if (sa == 255u || (dst >> 24) == 0u) { dst = src; return; }
        const juce::uint32 invA = 255u - sa;
        const juce::uint32 da = dst >> 24;
        const juce::uint32 dr = (dst >> 16) & 0xFFu;
        const juce::uint32 dg = (dst >>  8) & 0xFFu;
        const juce::uint32 db =  dst        & 0xFFu;
        const juce::uint32 sr = (src >> 16) & 0xFFu;
        const juce::uint32 sg = (src >>  8) & 0xFFu;
        const juce::uint32 sb =  src        & 0xFFu;
        const juce::uint32 oa = juce::jmin (255u, sa + (da * invA + 127u) / 255u);
        const juce::uint32 or_ = juce::jmin (255u, sr + (dr * invA + 127u) / 255u);
        const juce::uint32 og = juce::jmin (255u, sg + (dg * invA + 127u) / 255u);
        const juce::uint32 ob = juce::jmin (255u, sb + (db * invA + 127u) / 255u);
        dst = (oa << 24) | (or_ << 16) | (og << 8) | ob;
    }

    inline void fillColumnSolid (juce::uint32* col, int h, int y0, int y1, juce::Colour c, bool blend) noexcept
    {
        y0 = juce::jlimit (0, h, y0);
        y1 = juce::jlimit (0, h, y1);
        if (y1 <= y0) return;
        const juce::uint32 px = packPremulColour (c);
        if (! blend)
        {
            for (int y = y0; y < y1; ++y)
                col[y] = px;
        }
        else
        {
            for (int y = y0; y < y1; ++y)
                blendPremulOver (col[y], px);
        }
    }

    inline void fillColumnGrad (juce::uint32* col, int h, int y0, int y1,
                                juce::Colour cTop, juce::Colour cBot, bool blend) noexcept
    {
        const int cy0 = juce::jlimit (0, h, y0);
        const int cy1 = juce::jlimit (0, h, y1);
        if (cy1 <= cy0) return;
        const int span = juce::jmax (1, y1 - y0 - 1);
        const float invSpan = 1.0f / (float) span;
        const float r0 = (float) cTop.getRed(),   g0 = (float) cTop.getGreen(),
                    b0 = (float) cTop.getBlue(),  a0 = (float) cTop.getAlpha();
        const float dr = (float) cBot.getRed()   - r0, dg = (float) cBot.getGreen() - g0,
                    db = (float) cBot.getBlue()  - b0, da = (float) cBot.getAlpha() - a0;
        for (int y = cy0; y < cy1; ++y)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (float) (y - y0) * invSpan);
            const juce::uint32 px = packPremulBGRA ((int) (r0 + dr * t + 0.5f),
                                                    (int) (g0 + dg * t + 0.5f),
                                                    (int) (b0 + db * t + 0.5f),
                                                    (int) (a0 + da * t + 0.5f));
            if (blend) blendPremulOver (col[y], px);
            else       col[y] = px;
        }
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

    // 主题切换时环形像素与 GL 快照均须重建（颜色烘焙在像素缓冲里）
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        invalidateRingVisuals();
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
    s.setProperty("symmetric", symmetric, nullptr);
    s.setProperty("gainDb", (double) gainDb, nullptr);
    s.setProperty("floorDb", (double) floorDb, nullptr);
    s.setProperty("showRms", showRms, nullptr);
    s.setProperty("direction", scrollDirection, nullptr);
    s.setProperty("showGuides", showGuides, nullptr);
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
    if (state.hasProperty("symmetric"))
        setSymmetric ((bool) state.getProperty("symmetric"));
    else if (state.hasProperty("style"))
        setSymmetric ((int) state.getProperty("style") == 2); // 兼容旧存档：Mirrored(2) → 对称，Solid/Outline → 非对称
    if (state.hasProperty("gainDb"))
        setGainDb ((float) (double) state.getProperty("gainDb"));
    if (state.hasProperty("floorDb"))
        setFloorDb ((float) (double) state.getProperty("floorDb"));
    if (state.hasProperty("showRms"))
        setShowRms ((bool) state.getProperty("showRms"));
    if (state.hasProperty("direction"))
        setScrollDirection ((int) state.getProperty("direction"));
    if (state.hasProperty("showGuides"))
        setShowGuides ((bool) state.getProperty("showGuides"));

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
    out.push_back (ParamDef::toggle ("showRms", "RMS core",
        "Overlay a brighter inner RMS loudness band inside the outer peak waveform envelope.")
        .in ("Colours"));

    out.push_back (ParamDef::toggle ("symmetric", "Symmetric",
        "Mirror waveform peaks symmetrically above and below the zero line.")
        .in ("Wave"));
    out.push_back (ParamDef::slider ("gainDb", "Vertical zoom", -12.0, 18.0, 1.0, " dB",
        "Boosts or attenuates visual waveform height without changing audio. Handy for quiet acoustic passages.")
        .in ("Wave"));
    out.push_back (ParamDef::slider ("floorDb", "Noise gate", -90.0, -30.0, 2.0, " dB",
        "Columns quieter than this threshold fade out completely so silent gaps stay crisp and clean.")
        .in ("Wave"));
    out.push_back (ParamDef::toggle ("dual", "Dual channel",
        "Draw the left and right channel as two separate waveforms, left on top and right below.")
        .in ("Wave"));

    out.push_back (ParamDef::choice ("direction", "Scroll direction",
        { "Leftwards", "Rightwards" },
        "Leftwards: new audio enters on the right (classic). Rightwards: new audio enters on the left.")
        .in ("Scrolling"));
    out.push_back (ParamDef::slider ("speed", "Speed", (double) kMinPixelsPerSecond, (double) kMaxPixelsPerSecond,
        1.0, " px/s",
        "Scroll speed in pixels per second. Faster shows more detail, slower shows a longer history.",
        80.0).in ("Scrolling"));
    out.push_back (ParamDef::toggle ("showGuides", "Guides & labels",
        "Show the zero line, quarter time dividers, and window duration label.")
        .in ("Scrolling"));
}

double WaveformModule::getParamValue (const juce::String& id) const
{
    if (id == "colorSource")  return (double) (int) colorSource;
    if (id == "sibThreshold") return (double) sibThresholdDb;
    if (id == "showRms")      return showRms ? 1.0 : 0.0;
    if (id == "symmetric")    return symmetric ? 1.0 : 0.0;
    if (id == "style")        return symmetric ? 2.0 : 0.0;
    if (id == "gainDb")       return (double) gainDb;
    if (id == "floorDb")      return (double) floorDb;
    if (id == "speed")        return (double) pixelsPerSecond;
    if (id == "dual")         return dualChannel ? 1.0 : 0.0;
    if (id == "direction")    return (double) scrollDirection;
    if (id == "showGuides")   return showGuides ? 1.0 : 0.0;
    return 0.0;
}

void WaveformModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "colorSource")  setColorSource ((ColorSource) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "sibThreshold") setSibilanceThresholdDb ((float) v);
    else if (id == "showRms")      setShowRms (v >= 0.5);
    else if (id == "symmetric")    setSymmetric (v >= 0.5);
    else if (id == "style")        setSymmetric (v >= 1.5);
    else if (id == "gainDb")       setGainDb ((float) v);
    else if (id == "floorDb")      setFloorDb ((float) v);
    else if (id == "speed")        setPixelsPerSecond ((float) v);
    else if (id == "dual")         setDualChannel (v >= 0.5);
    else if (id == "direction")    setScrollDirection (juce::roundToInt (v));
    else if (id == "showGuides")   setShowGuides (v >= 0.5);
}

void WaveformModule::setSymmetric (bool on)
{
    if (symmetric == on) return;
    symmetric = on;
    invalidateRingVisuals();
    buildGlSnapshot();
    repaint();
}

void WaveformModule::setGainDb (float db)
{
    db = juce::jlimit (-12.0f, 18.0f, db);
    if (std::abs (db - gainDb) < 0.05f) return;
    gainDb = db;
    invalidateRingVisuals();
    buildGlSnapshot();
    repaint();
}

void WaveformModule::setFloorDb (float db)
{
    db = juce::jlimit (-90.0f, -30.0f, db);
    if (std::abs (db - floorDb) < 0.05f) return;
    floorDb = db;
    invalidateRingVisuals();
    buildGlSnapshot();
    repaint();
}

void WaveformModule::setShowRms (bool on)
{
    if (showRms == on) return;
    showRms = on;
    invalidateRingVisuals();
    buildGlSnapshot();
    repaint();
}

void WaveformModule::setScrollDirection (int dir)
{
    dir = juce::jlimit (0, 1, dir);
    if (scrollDirection == dir) return;
    scrollDirection = dir;
    buildGlSnapshot();
    repaint();
}

void WaveformModule::setShowGuides (bool on)
{
    if (showGuides == on) return;
    showGuides = on;
    glBgDirty_ = true;
    buildGlSnapshot();
    repaint();
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
    invalidateRingVisuals();
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
        invalidateRingVisuals();
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
    accSumSq2 = 0.0f;
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

    invalidateRingVisuals();
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
    const int drained = drainPendingColumns (columnBudgetForDt (frame.dtSec));
    if (drained <= 0)
        return;   // 本帧画面没有任何变化，不必重建快照 / 重绘

    updateRingAfterDrain (drained);

    if (wantsGlSnapshot())
        buildGlSnapshot();

    // GL 已接管且未悬停标尺时直接唤醒宿主 GL 线程（停靠与独立 GL 浮窗均免软件 CachedImage 重绘）
    if (hoverActive || ! requestGlFrameOnly())
        repaintFrameContent();
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
            accSumSq  += lc * lc;
            accSumSq2 += rc * rc;
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
            col.rms2  = std::sqrt (accSumSq2 * inv);
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
        accSumSq = 0.0f; accSumSq2 = 0.0f; accSibMax = 0.0f; accSibMax2 = 0.0f; accCount = 0;

        samplesUntilNextColumn += samplesPerColumn;
        if (samplesUntilNextColumn <= 0.0f)   // samplesPerColumn < 1 的保护
            samplesUntilNextColumn = samplesPerColumn;
    }
}

// ----------------------------------------------------------
// 颜色映射
// ----------------------------------------------------------

// 音高染色：音乐音域（C1..C7）映射到主题 chartRamp 的高可读鲜亮段，
//   避免暗色主题低端 chartRamp[0..2]（近黑）或浅色主题低端（近白）在真实低频/中频乐段显得暗淡发透。
juce::Colour WaveformModule::mapPitchToColour (float noteNumber, float confidence, float level01) const
{
    const float t = juce::jlimit (0.0f, 1.0f,
                                  (noteNumber - kPitchLowNote) / (kPitchHighNote - kPitchLowNote));
    const bool darkCanvas = (PinkXP::content.getPerceivedBrightness() < 0.50f);

    // 跳过最贴近画布底色的低阶 chartRamp[0..2]，从高可见度中亮段展开音高色域
    const float rampT = 0.42f + 0.58f * t;
    juce::Colour rampCol = PinkXP::sampleChartRamp (rampT);

    // 若主题配有区分度明显的 accent2，在中高音区引入温和的和谐色相过渡，音高层次更立体
    if (! PinkXP::accent2.isTransparent())
    {
        const float accentMix = 0.35f * juce::jlimit (0.0f, 1.0f, (t - 0.15f) / 0.85f);
        rampCol = PinkXP::interpolateOkLab (rampCol, PinkXP::accent2, accentMix);
    }

    // 置信度低（鼓点 / 噪声）：落向同可见度的中性主色，不发灰变暗
    const juce::Colour neutral = PinkXP::sampleChartRamp (0.62f).withMultipliedSaturation (0.58f);
    const float w = juce::jlimit (0.0f, 1.0f, (confidence - 0.05f) / 0.40f);
    juce::Colour c = PinkXP::interpolateOkLab (neutral, rampCol, 0.40f + 0.60f * w);

    // 电平微调 + 相对画布底色的清晰亮度保底（暗底不发黑、浅底不发白发透）
    const float level = juce::jlimit (0.0f, 1.0f, level01);
    c = c.withMultipliedBrightness (0.92f + 0.14f * level);
    if (darkCanvas)
    {
        const float minBr = 0.62f + 0.12f * level;
        if (c.getBrightness() < minBr)
            c = c.withBrightness (minBr);
    }
    else
    {
        const float maxBr = 0.72f - 0.06f * level;
        if (c.getBrightness() > maxBr)
            c = c.withBrightness (maxBr);
    }
    return c;
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

    const int k = (scrollDirection == 0) ? (w - 1 - xi) : xi;
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
            c.minV = c.minV2; c.maxV = c.maxV2; c.rms = c.rms2; c.db = c.db2; c.sib = c.sib2;
        }
        return c;
    };

    const Column c0 = sampleCol (juce::jmax (0, k - 1));
    const Column c1 = sampleCol (k);
    const Column c2 = sampleCol (juce::jmin (validCols - 1, k + 1));

    const float ampGain = (std::abs (gainDb) > 0.05f) ? std::pow (10.0f, gainDb * 0.05f) : 1.0f;
    float minV = (c0.minV + c1.minV + c2.minV) * (1.0f / 3.0f) * ampGain;
    float maxV = (c0.maxV + c1.maxV + c2.maxV) * (1.0f / 3.0f) * ampGain;
    const float rmsV = (c0.rms + c1.rms + c2.rms) * (1.0f / 3.0f) * ampGain;
    const float db   = (c0.db   + c1.db   + c2.db  ) * (1.0f / 3.0f);

    if (symmetric)
    {
        const float sym = juce::jmax (std::abs (minV), std::abs (maxV));
        maxV =  sym;
        minV = -sym;
    }

    const float alphaFullDb = floorDb + 21.0f;
    const float alpha = juce::jlimit (0.0f, 1.0f,
                                      (db - floorDb) / juce::jmax (1.0f, alphaFullDb - floorDb));
    if (alpha < 0.02f)
        return false;                     // 静音列整列不画

    const float level01 = juce::jlimit (0.0f, 1.0f,
                                        (db - kLoudFloorDb) / -kLoudFloorDb);

    const float halfH = (float) innerH * 0.48f;
    const int   cy    = centreY;

    const int yTopRaw = cy - (int) std::round (juce::jlimit (-1.0f, 1.0f, maxV) * halfH);
    const int yBotRaw = cy - (int) std::round (juce::jlimit (-1.0f, 1.0f, minV) * halfH);

    const int yMin = juce::jlimit (innerY, innerY + innerH, juce::jmin (yTopRaw, yBotRaw));
    const int yMax = juce::jlimit (innerY, innerY + innerH, juce::jmax (yTopRaw, yBotRaw));

    out.yMin  = yMin;
    out.hLine = juce::jmax (1, yMax - yMin);

    const auto style = columnStyle (c1, level01, alpha);
    out.colour = style.colour.withAlpha (juce::jlimit (0.0f, 1.0f, style.alpha));

    if (showRms && rmsV > 1.0e-5f)
    {
        const float rc = juce::jlimit (0.0f, 1.0f, rmsV);
        const int ryTop = juce::jlimit (innerY, innerY + innerH, cy - (int) std::round (rc * halfH));
        const int ryBot = juce::jlimit (innerY, innerY + innerH, cy + (int) std::round (rc * halfH));
        out.yRmsMin   = ryTop;
        out.hRmsLine  = juce::jmax (1, ryBot - ryTop);
        out.rmsColour = style.colour.interpolatedWith (PinkXP::hl, 0.48f)
                                    .withAlpha (juce::jlimit (0.0f, 1.0f, style.alpha * 0.95f));
    }
    else
    {
        out.hRmsLine = 0;
    }
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
    const auto canvas = getToyCanvasBounds();
    const auto inner  = canvas.reduced (4);
    if (inner.getWidth() < 8 || inner.getHeight() < 8)
        return;

    // 命中哪一条泳道（单声道 1 条 / 双声道上下 2 条）
    Lane lanes[2];
    const int nLanes = computeLanes (inner, lanes);
    int chosen = 0;
    if (nLanes == 2 && hit.pos.y >= (float) lanes[1].y)
        chosen = 1;
    const auto& ln = lanes[chosen];
    const juce::Rectangle<int> laneRect (inner.getX(), ln.y, inner.getWidth(), ln.h);

    // 采样该泳道的真实上下包络 Y（96 点，复用 resolveColumn 口径）
    constexpr int kBins = 96;
    std::vector<float> envTop ((size_t) kBins, (float) ln.cy);
    std::vector<float> envBot ((size_t) kBins, (float) ln.cy);
    const int colCap = (int) columnHistory.size();
    const int w = juce::jmin (inner.getWidth(), colCap);
    if (w > 1 && validCols > 0)
    {
        for (int i = 0; i < kBins; ++i)
        {
            const int xi = juce::jlimit (0, w - 1, juce::roundToInt ((float) i / (float) (kBins - 1) * (float) (w - 1)));
            ColumnPaint cp;
            if (resolveColumn (xi, w, ln.y, ln.h, ln.cy, cp, ln.chanB))
            {
                envTop[(size_t) i] = (float) cp.yMin;
                envBot[(size_t) i] = (float) (cp.yMin + cp.hLine);
            }
        }
    }

    juce::Component::SafePointer<WaveformModule> safe (this);
    auto isSlowMo = [safe]() -> bool
    {
        return safe != nullptr && safe->hold.active;
    };

    ctx.addModuleActorCapped (makeWaveformShockActor (hit.pos, laneRect, envTop, envBot, std::move (isSlowMo)), 6);
}

void WaveformModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    // 画布宽度变化（含首次布局）时重建列缓冲 —— 列数严格 = 画布内部宽
    auto canvas = getCanvasBounds (contentBounds).reduced (4);
    const int newW = juce::jmax (1, canvas.getWidth());
    const int newH = juce::jmax (1, canvas.getHeight());
    rebuildColumnBuffer (newW);
    ensureRingSurface (newW, newH);

    // 几何变化 → 立即重建 GL 快照（尺寸守卫防 layout 风暴）
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
    {
        glBgDirty_ = true;
        buildGlSnapshot();
    }
}

// ----------------------------------------------------------
// GlDirectModuleBase 钩子
// ----------------------------------------------------------
juce::Rectangle<int> WaveformModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void WaveformModule::drawOverlayGuides (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    if (PinkXP::isChromaKeySkin() || ! showGuides)
        return;

    const auto& tokens   = PinkXP::getSkinTokens();
    const bool  isModern = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
    const bool  isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool  isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool  isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool  isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const float axisPt   = isNeon ? 7.8f : (isCrt ? 8.5f : 9.0f);
    const auto  axisFont = PinkXP::getAxisFont (axisPt, juce::Font::plain);
    const juce::String hint = windowHintText (getDisplaySeconds(), scrollDirection == 1);

    if (isModern || isChrome || isGlass || isNeon || isCrt)
    {
        const bool darkCanvas = (PinkXP::content.getPerceivedBrightness() < 0.50f);
        juce::Colour chipFill;
        juce::Colour chipBorder;
        if (isChrome)
        {
            chipFill   = (darkCanvas ? PinkXP::face.interpolatedWith (PinkXP::content, 0.30f)
                                     : PinkXP::face.interpolatedWith (PinkXP::hl, 0.25f)).withAlpha (0.90f);
            chipBorder = darkCanvas ? PinkXP::hl.withAlpha (0.34f) : PinkXP::dark.withAlpha (0.44f);
        }
        else if (isGlass)
        {
            chipFill   = (darkCanvas ? PinkXP::content.interpolatedWith (juce::Colours::white, 0.14f)
                                     : PinkXP::content.interpolatedWith (PinkXP::face, 0.45f)).withAlpha (0.76f);
            chipBorder = darkCanvas ? juce::Colours::white.withAlpha (0.28f) : PinkXP::dark.withAlpha (0.24f);
        }
        else if (isNeon)
        {
            chipFill   = PinkXP::content.interpolatedWith (juce::Colours::black, 0.52f).withAlpha (0.90f);
            chipBorder = PinkXP::sel.withAlpha (0.84f);
        }
        else if (isCrt)
        {
            chipFill   = PinkXP::content.interpolatedWith (juce::Colours::black, 0.44f).withAlpha (0.92f);
            chipBorder = PinkXP::sel.withAlpha (0.86f);
        }
        else
        {
            chipFill   = PinkXP::content.interpolatedWith (PinkXP::face, 0.35f).withAlpha (0.78f);
            chipBorder = PinkXP::skinHairlineOn (PinkXP::content, 0.20f);
        }
        const juce::Colour textCol = PinkXP::readableInkOn (chipFill.withAlpha (1.0f))
                                         .withAlpha ((isNeon || isCrt) ? 0.95f : (isChrome ? 0.92f : 0.88f));

        const int hintW = juce::jlimit (44, juce::jmax (44, canvas.getWidth() - 10),
                                        (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (hint, axisFont)) + ((isCrt || isNeon) ? 12 : 10));
        const juce::Rectangle<int> hintRect (canvas.getX() + 4, canvas.getBottom() - 16, hintW, 13);
        PinkXP::Craft::drawBadgePill (g, hintRect, chipFill, chipBorder);
        g.setColour (textCol);
        PinkXP::SymbolGlyphs::drawText (g, hint, hintRect.reduced (4, 0),
                                        juce::Justification::centredLeft, axisFont);

        if (dualChannel)
        {
            const int lrW = (isCrt || isNeon) ? 17 : 15;
            const juce::Rectangle<int> lRect (canvas.getX() + 4, canvas.getY() + 4, lrW, 13);
            const juce::Rectangle<int> rRect (canvas.getX() + 4, canvas.getCentreY() + 3, lrW, 13);
            PinkXP::Craft::drawBadgePill (g, lRect, chipFill, chipBorder);
            PinkXP::Craft::drawBadgePill (g, rRect, chipFill, chipBorder);
            g.setColour (textCol);
            g.setFont (axisFont);
            g.drawText (y2k::tr ("L"), lRect, juce::Justification::centred, false);
            g.drawText (y2k::tr ("R"), rRect, juce::Justification::centred, false);
        }
        return;
    }

    const juce::Colour labelCol = PinkXP::readableInkOn (PinkXP::content).withAlpha (0.82f);
    g.setColour (labelCol);
    PinkXP::SymbolGlyphs::drawText (g, hint,
                                    juce::Rectangle<int> (canvas.getX() + 4, canvas.getBottom() - 14, 220, 12),
                                    juce::Justification::centredLeft, axisFont);

    if (dualChannel)
    {
        g.setFont (axisFont);
        g.drawText (y2k::tr ("L"), canvas.getX() + 4, canvas.getY() + 4, 16, 12,
                    juce::Justification::centredLeft, false);
        g.drawText (y2k::tr ("R"), canvas.getX() + 4, canvas.getCentreY() + 3, 16, 12,
                    juce::Justification::centredLeft, false);
    }
}

void WaveformModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    drawOverlayGuides (g, canvas);

    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void WaveformModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaHidden = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);
    if (! (chromaHidden && PinkXP::isTransparentBackdrop()))
    {
        g.setColour (chromaHidden
                         ? PinkXP::chromaKeyGreen()
                         : (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                                ? PinkXP::btnFace
                                : PinkXP::face));
        g.fillRect (contentBounds);
    }

    auto canvas = getCanvasBounds (contentBounds);
    drawBackground (g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    drawWaveform (g, canvas);
    PinkXP::Craft::drawCanvasFinishOverlay (g, canvas);
    drawOverlayGuides (g, canvas);

    if (hoverActive)
        drawHoverRuler (g, canvas);
}

// ----------------------------------------------------------
// 增量环形像素光栅化（GL 直绘与浮窗软绘共享同一套像素结果，O(新增列数)）
//   · 彻底消除所有风格下 1x1 白色边缘噪点（采用连贯的柱体内渐变与中轴柔光）
//   · 彻底消除大窗口下的全宽 VBO 重建（94K 顶点 → ~40 静态背景顶点 + ~4 KB 增量纹理补丁）
//   · 彻底消除浮窗软绘逐列 ColourGradient / fillRect 导致的 Direct2D/消息线程卡顿
// ----------------------------------------------------------
void WaveformModule::invalidateRingVisuals()
{
    ringFullRebuild_ = true;
    swImgDirty_      = true;
    glBgDirty_       = true;
    ++ringFullGen_;
    glTexUploadedGen.store (0, std::memory_order_release);
}

void WaveformModule::ensureRingSurface (int innerW, int innerH)
{
    innerW = juce::jmax (1, innerW);
    innerH = juce::jmax (1, innerH);
    const size_t needed = (size_t) innerW * (size_t) innerH;
    if (ringW_ == innerW && ringH_ == innerH && ringColMajor_.size() == needed)
        return;

    ringW_ = innerW;
    ringH_ = innerH;
    ringColMajor_.assign (needed, 0u);
    invalidateRingVisuals();
}

void WaveformModule::rasterizeColumnSlot (int slot)
{
    const int colCap = (int) columnHistory.size();
    if (ringW_ <= 0 || ringH_ <= 0 || colCap <= 0 || slot < 0 || slot >= ringW_)
        return;

    juce::uint32* colDst = ringColMajor_.data() + (size_t) slot * (size_t) ringH_;
    std::memset (colDst, 0, (size_t) ringH_ * sizeof (juce::uint32));

    if (validCols <= 0 || slot >= colCap)
        return;

    const int age = (writeCol - 1 - slot + colCap) % colCap;
    if (age >= validCols)
        return;

    const int prevSlot = (age + 1 < validCols) ? ((slot - 1 + colCap) % colCap) : slot;
    const int nextSlot = (age > 0)             ? ((slot + 1) % colCap)          : slot;

    Lane lanes[2];
    const int nLanes = computeLanes ({ 0, 0, ringW_, ringH_ }, lanes);

    const auto& tokens       = PinkXP::getSkinTokens();
    const bool greenBackdrop = PinkXP::isGreenScreenBackdrop();
    const bool isPixelXp     = (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel);
    const bool isGlowSkin    = tokens.strokeGlowPass;
    const bool isNeon        = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isChrome      = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass       = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isSmoothSkin  = ! isPixelXp && ! isGlowSkin && ! isChrome && ! isGlass;

    const float ampGain     = (std::abs (gainDb) > 0.05f) ? std::pow (10.0f, gainDb * 0.05f) : 1.0f;
    const float alphaFullDb = floorDb + 21.0f;
    const float invAlphaSpan = 1.0f / juce::jmax (1.0f, alphaFullDb - floorDb);

    auto pickCol = [&] (int s, bool chanB) -> Column
    {
        Column c = columnHistory[(size_t) s];
        if (chanB)
        {
            c.minV = c.minV2; c.maxV = c.maxV2; c.rms = c.rms2; c.db = c.db2; c.sib = c.sib2;
        }
        return c;
    };

    for (int li = 0; li < nLanes; ++li)
    {
        const auto& ln = lanes[li];
        const Column c0 = pickCol (nextSlot, ln.chanB);
        const Column c1 = pickCol (slot,     ln.chanB);
        const Column c2 = pickCol (prevSlot, ln.chanB);

        float minV = (c0.minV + c1.minV + c2.minV) * (1.0f / 3.0f) * ampGain;
        float maxV = (c0.maxV + c1.maxV + c2.maxV) * (1.0f / 3.0f) * ampGain;
        const float rmsV = (c0.rms + c1.rms + c2.rms) * (1.0f / 3.0f) * ampGain;
        const float db   = (c0.db  + c1.db  + c2.db ) * (1.0f / 3.0f);

        if (symmetric)
        {
            const float sym = juce::jmax (std::abs (minV), std::abs (maxV));
            maxV =  sym;
            minV = -sym;
        }

        const float alpha = juce::jlimit (0.0f, 1.0f, (db - floorDb) * invAlphaSpan);
        if (alpha < 0.02f)
            continue;

        const float level01 = juce::jlimit (0.0f, 1.0f, (db - kLoudFloorDb) / -kLoudFloorDb);
        const float halfH   = (float) ln.h * 0.48f;
        const int   cy      = ln.cy;
        const int   laneTop = ln.y;
        const int   laneBot = ln.y + ln.h;

        const int yTopRaw = cy - (int) std::round (juce::jlimit (-1.0f, 1.0f, maxV) * halfH);
        const int yBotRaw = cy - (int) std::round (juce::jlimit (-1.0f, 1.0f, minV) * halfH);
        const int yTop    = juce::jlimit (laneTop, laneBot, juce::jmin (yTopRaw, yBotRaw));
        const int yBotMax = juce::jlimit (laneTop, laneBot, juce::jmax (yTopRaw, yBotRaw));
        const int hCol    = juce::jmax (1, yBotMax - yTop);
        const int yBot    = juce::jmin (laneBot, yTop + hCol);

        const auto style   = columnStyle (c1, level01, alpha);
        const auto baseCol = style.colour.withAlpha (juce::jlimit (0.0f, 1.0f, style.alpha));
        const bool darkCanvas = (PinkXP::content.getPerceivedBrightness() < 0.50f);
        const auto hiTarget   = darkCanvas ? juce::Colours::white : PinkXP::hl;

        if (isPixelXp)
        {
            // Pixel XP：纯粹利落的像素实心波柱（无边缘白点噪点）
            fillColumnSolid (colDst, ringH_, yTop, yBot, baseCol, false);
        }
        else if (isGlowSkin)
        {
            // Neon Cyber / CRT Terminal：彩色柔光外晕 + 饱满灯管主体 + 沿中轴 cy 的柔滑荧光芯
            if (! greenBackdrop)
            {
                const int haloReach = isNeon ? 3 : 2;
                const float gInt = tokens.glowIntensity;
                static constexpr float kNeonFalloff[3] = { 0.30f, 0.16f, 0.06f };
                static constexpr float kCrtFalloff[2]  = { 0.22f, 0.08f };
                for (int d = 1; d <= haloReach; ++d)
                {
                    const float aMul = (isNeon ? kNeonFalloff[d - 1] : kCrtFalloff[d - 1]) * gInt;
                    const auto hc = baseCol.withMultipliedAlpha (aMul);
                    if (yTop - d >= laneTop)    fillColumnSolid (colDst, ringH_, yTop - d, yTop - d + 1, hc, true);
                    if (yBot + d - 1 < laneBot) fillColumnSolid (colDst, ringH_, yBot + d - 1, yBot + d, hc, true);
                }
            }

            const float bodyMul = greenBackdrop ? 1.0f : (isNeon ? 0.94f : 0.96f);
            const auto  bodyCol = baseCol.withMultipliedAlpha (bodyMul);
            fillColumnSolid (colDst, ringH_, yTop, yBot, bodyCol, false);

            if (hCol >= 6 && cy > yTop + 1 && cy < yBot - 1)
            {
                const int coreHalf = juce::jlimit (1, isNeon ? 6 : 4, hCol / (isNeon ? 4 : 5));
                const auto coreCol = bodyCol.interpolatedWith (juce::Colours::white, isNeon ? 0.30f : 0.20f);
                const int cTop = juce::jmax (yTop + 1, cy - coreHalf);
                const int cBot = juce::jmin (yBot - 1, cy + coreHalf + 1);
                fillColumnGrad (colDst, ringH_, cTop, cy + 1, bodyCol, coreCol, false);
                fillColumnGrad (colDst, ringH_, cy, cBot, coreCol, bodyCol, false);
            }
        }
        else if (isChrome)
        {
            // Millennium Chrome：金属缎面拉丝渐变 —— 亮银高光带 + 饱满主体（暗主题下不向暗灰 hl 压暗）
            const auto cTip    = baseCol;
            const auto cSheen  = baseCol.interpolatedWith (hiTarget, darkCanvas ? 0.28f : 0.24f);
            const auto cCore   = baseCol.darker (0.10f);
            if (hCol >= 6 && cy > yTop + 2 && cy < yBot - 2)
            {
                const int ySheen = yTop + juce::jmax (1, (int) std::round ((float) (cy - yTop) * 0.42f));
                fillColumnGrad (colDst, ringH_, yTop,   ySheen + 1, cTip,   cSheen, false);
                fillColumnGrad (colDst, ringH_, ySheen, cy + 1,     cSheen, cCore,  false);
                fillColumnGrad (colDst, ringH_, cy,     yBot,       cCore,  cTip,   false);
            }
            else
            {
                fillColumnGrad (colDst, ringH_, yTop, yBot, cSheen, cCore, false);
            }
        }
        else if (isGlass)
        {
            // Frosted Glass：液态水晶波柱 —— 柔和顶部玻璃过渡反光 + 饱满通透渐变柱体
            const auto cTop  = baseCol.interpolatedWith (juce::Colours::white, 0.16f)
                                      .withMultipliedAlpha (greenBackdrop ? 1.0f : 0.96f);
            const auto cCore = baseCol.withMultipliedAlpha (greenBackdrop ? 1.0f : 0.88f);
            const auto cBot  = baseCol.withMultipliedAlpha (greenBackdrop ? 1.0f : 0.93f);
            if (hCol >= 4 && cy > yTop + 1 && cy < yBot - 1)
            {
                fillColumnGrad (colDst, ringH_, yTop, cy + 1, cTop,  cCore, false);
                fillColumnGrad (colDst, ringH_, cy,   yBot,   cCore, cBot,  false);
            }
            else
            {
                fillColumnGrad (colDst, ringH_, yTop, yBot, cTop, cCore, false);
            }
            if (! greenBackdrop && hCol >= 8)
            {
                const int sheenH = juce::jmax (2, (int) std::round ((float) hCol * 0.30f));
                const float aBase = baseCol.getFloatAlpha();
                fillColumnGrad (colDst, ringH_, yTop, juce::jmin (yBot, yTop + sheenH),
                                juce::Colours::white.withAlpha (0.16f * aBase),
                                juce::Colours::white.withAlpha (0.0f), true);
            }
        }
        else if (isSmoothSkin)
        {
            // Modern Minimal：丝滑对称渐变，饱满清晰不发暗
            const auto cOuter = baseCol;
            const auto cCore  = baseCol.interpolatedWith (hiTarget, darkCanvas ? 0.16f : 0.14f);
            if (hCol >= 4 && cy > yTop + 1 && cy < yBot - 1)
            {
                fillColumnGrad (colDst, ringH_, yTop, cy + 1, cOuter, cCore,  false);
                fillColumnGrad (colDst, ringH_, cy,   yBot,   cCore,  cOuter, false);
            }
            else
            {
                fillColumnGrad (colDst, ringH_, yTop, yBot, cOuter, cCore, false);
            }
        }
        else
        {
            fillColumnSolid (colDst, ringH_, yTop, yBot, baseCol, false);
        }

        // 内层 RMS 核心包络（暗色主题用纯白提亮而非深灰 PinkXP::hl，保证清晰明亮）
        if (showRms && rmsV > 1.0e-5f)
        {
            const float rc = juce::jlimit (0.0f, 1.0f, rmsV);
            const int ryTop = juce::jlimit (laneTop, laneBot, cy - (int) std::round (rc * halfH));
            const int ryBot = juce::jlimit (laneTop, laneBot, cy + (int) std::round (rc * halfH));
            const int rh    = juce::jmax (1, ryBot - ryTop);
            const auto rmsCol = style.colour.interpolatedWith (hiTarget, darkCanvas ? 0.38f : 0.44f)
                                            .withAlpha (juce::jlimit (0.0f, 1.0f, style.alpha * 0.94f));
            fillColumnSolid (colDst, ringH_, ryTop, juce::jmin (laneBot, ryTop + rh), rmsCol, true);
        }
    }
}

void WaveformModule::rasterizeAllRingColumns()
{
    if (ringW_ <= 0 || ringH_ <= 0)
        return;

    std::memset (ringColMajor_.data(), 0, ringColMajor_.size() * sizeof (juce::uint32));
    const int colCap = (int) columnHistory.size();
    const int n = juce::jmin (validCols, juce::jmin (ringW_, colCap));
    for (int age = 0; age < n; ++age)
    {
        const int slot = (writeCol - 1 - age + colCap) % colCap;
        if (slot >= 0 && slot < ringW_)
            rasterizeColumnSlot (slot);
    }

    ringFullRebuild_ = false;
    lastRasterAppearanceGen_ = PinkXP::getAppearanceGeneration();
    if (++colSeq_ == 0)
        colSeq_ = 1;
}

void WaveformModule::updateRingAfterDrain (int drainedCount)
{
    if (drainedCount <= 0)
        return;

    const auto inner = getCanvasBounds (getContentBounds()).reduced (4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0)
        return;

    if (PinkXP::getAppearanceGeneration() != lastRasterAppearanceGen_)
        invalidateRingVisuals();

    ensureRingSurface (inner.getWidth(), inner.getHeight());

    if (ringFullRebuild_)
    {
        rasterizeAllRingColumns();
        return;
    }

    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || ringW_ <= 0)
        return;

    // 重绘本帧新增的 drainedCount 列 + 紧邻其前的 1 列（3-tap 平滑右邻居刚更新）
    const int toUpdate = juce::jmin (ringW_, juce::jmin (validCols, drainedCount + 1));
    for (int k = 0; k < toUpdate; ++k)
    {
        const int slot = (writeCol - 1 - k + colCap) % colCap;
        if (slot >= 0 && slot < ringW_)
            rasterizeColumnSlot (slot);
    }

    colSeq_ += (juce::uint64) drainedCount;
    if (colSeq_ == 0)
        colSeq_ = 1;
}

void WaveformModule::syncSoftwareRingImage (int innerW, int innerH) const
{
    if (innerW <= 0 || innerH <= 0 || ringW_ != innerW || ringH_ != innerH)
        return;

    const bool needCreate = (! waveSwImg_.isValid()
                             || waveSwImg_.getWidth() != innerW
                             || waveSwImg_.getHeight() != innerH);
    if (needCreate)
    {
        waveSwImg_ = juce::Image (juce::Image::ARGB, innerW, innerH, true, juce::SoftwareImageType());
        swImgDirty_ = true;
    }

    const bool needFull = swImgDirty_
                       || (swImgSyncedFullGen_ != ringFullGen_)
                       || (colSeq_ < swImgSyncedSeq_)
                       || ((colSeq_ - swImgSyncedSeq_) >= (juce::uint64) ringW_);

    if (! needFull && colSeq_ == swImgSyncedSeq_)
        return;

    juce::Image::BitmapData bd (waveSwImg_, juce::Image::BitmapData::writeOnly);

    auto copyColumn = [&] (int slot)
    {
        const juce::uint32* srcCol = ringColMajor_.data() + (size_t) slot * (size_t) ringH_;
        for (int y = 0; y < ringH_; ++y)
            reinterpret_cast<juce::uint32*> (bd.getLinePointer (y))[slot] = srcCol[y];
    };

    if (needFull)
    {
        for (int s = 0; s < ringW_; ++s)
            copyColumn (s);
    }
    else
    {
        const int delta = (int) (colSeq_ - swImgSyncedSeq_);
        const int count = juce::jmin (ringW_, delta + 1);
        for (int k = 0; k < count; ++k)
        {
            const int slot = (writeCol - 1 - k + ringW_) % ringW_;
            copyColumn (slot);
        }
    }

    swImgDirty_         = false;
    swImgSyncedSeq_     = colSeq_;
    swImgSyncedFullGen_ = ringFullGen_;
}

// ----------------------------------------------------------
// buildGlSnapshot —— 静态凹槽/参考线顶点 + 列主序增量环形纹理
//   每帧仅上传 1~4 列增量补丁（~4 KB），彻底消除 94K 顶点 VBO 与 2x SSAA 过绘
// ----------------------------------------------------------
void WaveformModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;

    const auto canvas = getCanvasBounds (getContentBounds());
    const auto inner  = canvas.reduced (4);
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8
        || inner.getWidth() <= 0 || inner.getHeight() <= 0)
        return;

    const juce::uint32 appGen = PinkXP::getAppearanceGeneration();
    if (appGen != lastRasterAppearanceGen_)
        invalidateRingVisuals();

    ensureRingSurface (inner.getWidth(), inner.getHeight());
    if (ringFullRebuild_)
        rasterizeAllRingColumns();

    const auto& tokens = PinkXP::getSkinTokens();

    // 静态背景与参考线缓存（仅尺寸 / 皮肤 / 参考线 / 双声道变化时重建）
    if (glBgDirty_
        || glBgCachedCanvas_ != canvas
        || glBgCachedAppearanceGen_ != appGen
        || glBgCachedGuides_ != showGuides
        || glBgCachedDual_ != dualChannel)
    {
        glBgVertsCache_.clear();
        appendWellFrame (glBgVertsCache_, canvas, PinkXP::content);
        if (! tokens.isChromaKey && showGuides)
        {
            auto guideInner = canvas.reduced (2);

            const bool isModern = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
            const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
            const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
            const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
            const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
            const juce::Colour etchedMetal = PinkXP::skinHairlineOn (PinkXP::content, 0.82f)
                                                 .interpolatedWith (PinkXP::hl, 0.25f);
            const auto zeroCol  = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.85f)
                                : isChrome ? PinkXP::pink400.interpolatedWith (PinkXP::hl, 0.35f).withAlpha (0.92f)
                                : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.78f)
                                : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.35f).withAlpha (0.95f)
                                : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::hl, 0.25f)
                                           : PinkXP::pink500;
            const auto divCol   = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.72f)
                                : isChrome ? etchedMetal.withAlpha (0.86f)
                                : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.68f)
                                : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                                : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                           : PinkXP::pink400;
            const auto qtrCol   = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.68f)
                                : isChrome ? etchedMetal.withAlpha (0.76f)
                                : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.62f)
                                : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                                : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                           : PinkXP::pink400;

            Lane bgLanes[2];
            const int nBg = computeLanes (guideInner, bgLanes);
            for (int li = 0; li < nBg; ++li)
                appendGridLine (glBgVertsCache_,
                                (float) guideInner.getX(), (float) bgLanes[li].cy,
                                (float) guideInner.getRight(), (float) bgLanes[li].cy,
                                true, zeroCol);
            if (nBg == 2)
                appendGridLine (glBgVertsCache_,
                                (float) guideInner.getX(), (float) (bgLanes[0].y + bgLanes[0].h),
                                (float) guideInner.getRight(), (float) (bgLanes[0].y + bgLanes[0].h),
                                true, divCol);
            for (int i = 1; i <= 3; ++i)
            {
                const int x = guideInner.getX() + (guideInner.getWidth() * i) / 4;
                appendGridLine (glBgVertsCache_,
                                (float) x, (float) guideInner.getY(),
                                (float) x, (float) guideInner.getBottom(),
                                false, qtrCol);
            }
        }

        glBgCachedCanvas_        = canvas;
        glBgCachedAppearanceGen_ = appGen;
        glBgCachedGuides_        = showGuides;
        glBgCachedDual_          = dualChannel;
        glBgDirty_               = false;
    }

    auto snap = beginGlSnapshot();
    snap->snapToPixels     = false;
    snap->renderToLogical  = false;
    snap->canvasLocal      = canvas;
    snap->fxRect           = canvas;
    snap->vertices         = glBgVertsCache_;
    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });

    // 列主序环形纹理参数（texW = ringH_, texH = ringW_, GL_REPEAT 无缝卷绕）
    snap->texW             = ringH_;
    snap->texH             = ringW_;
    snap->texLinear        = true;
    snap->texTransposed    = true;
    snap->texPremultiplied = true;
    snap->texWrapRepeatT   = true;
    snap->texTargetRect    = inner;
    snap->texDataGen       = colSeq_;

    const float invW = 1.0f / (float) juce::jmax (1, ringW_);
    const float spanCols = (float) juce::jmax (1, ringW_ - 1);
    if (scrollDirection == 0)
    {
        snap->texScrollU = ((float) writeCol + 0.5f) * invW;
        snap->texWinW    = spanCols * invW;
    }
    else
    {
        snap->texScrollU = ((float) writeCol + spanCols + 0.5f) * invW;
        snap->texWinW    = -spanCols * invW;
    }

    const juce::uint64 uploaded = glTexUploadedGen.load (std::memory_order_acquire);
    const bool needFull = (! glDirectActive.load (std::memory_order_acquire))
                       || (uploaded == 0)
                       || (colSeq_ < uploaded)
                       || ((colSeq_ - uploaded) >= (juce::uint64) ringW_);

    if (needFull)
    {
        snap->texDirty  = true;
        snap->texPixels = ringColMajor_;
    }
    else if (colSeq_ > uploaded)
    {
        snap->texDirty = false;
        const int delta = (int) (colSeq_ - uploaded);
        const int count = juce::jmin (ringW_, delta + 1);
        const int start = (writeCol - count + ringW_) % ringW_;
        const int firstRun = juce::jmin (count, ringW_ - start);
        const int secondRun = count - firstRun;

        auto pushPatch = [&] (int rowStart, int rowCount)
        {
            if (rowCount <= 0) return;
            VertexStream::TexRowPatch patch;
            patch.rowStart = rowStart;
            patch.rowCount = rowCount;
            const size_t offset = (size_t) rowStart * (size_t) ringH_;
            const size_t nPix   = (size_t) rowCount * (size_t) ringH_;
            patch.pixels.assign (ringColMajor_.begin() + (ptrdiff_t) offset,
                                 ringColMajor_.begin() + (ptrdiff_t) (offset + nPix));
            snap->texRowPatches.push_back (std::move (patch));
        };

        pushPatch (start, firstRun);
        if (secondRun > 0)
            pushPatch (0, secondRun);
    }

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

    // X → 相对时间（秒，负 = 过去）
    const float t = (scrollDirection == 0)
        ? (float) (hoverPos.x - inner.getRight()) / (float) juce::jmax (1, inner.getWidth())
        : (float) (inner.getX() - hoverPos.x)     / (float) juce::jmax (1, inner.getWidth());
    const float timeSec = t * getDisplaySeconds();

    // Y → 真实幅度（扣除纵向缩放增益 gainDb）→ dBFS
    const float ampGain = (std::abs (gainDb) > 0.05f) ? std::pow (10.0f, gainDb * 0.05f) : 1.0f;
    const float amp = (cy - (float) hoverPos.y) / juce::jmax (1.0f, halfH) / ampGain;
    const float mag = juce::jlimit (0.0f, 1.0f, std::abs (amp));
    const float db  = juce::Decibels::gainToDecibels (juce::jmax (1.0e-4f, mag));

    juce::String readout = formatRelativeTime (timeSec) + "  "
                         + juce::String (db, 1) + " dBFS";

    if (colorSource == ColorSource::pitch && hold.active)
    {
        const int colCap = (int) columnHistory.size();
        const int w = juce::jmin (inner.getWidth(), colCap);
        if (colCap > 0 && validCols > 0 && w > 0)
        {
            const int xi = juce::jlimit (0, w - 1, hoverPos.x - inner.getX());
            const int k  = (scrollDirection == 0) ? (w - 1 - xi) : xi;
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
    if (PinkXP::isTransparentBackdrop())
        return;
    if (PinkXP::isGreenScreenBackdrop())
    {
        g.setColour (PinkXP::chromaKeyGreen());
        g.fillRect (canvas);
        return;
    }

    PinkXP::Craft::drawPanelWell (g, canvas, PinkXP::content, PinkXP::WellKind::canvas);
    if (! showGuides) return;

    auto inner = canvas.reduced (2);
    if (inner.isEmpty()) return;

    const auto& tokens  = PinkXP::getSkinTokens();
    const bool isModern = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
    const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const juce::Colour etchedMetal = PinkXP::skinHairlineOn (PinkXP::content, 0.82f)
                                         .interpolatedWith (PinkXP::hl, 0.25f);
    const auto zeroCol  = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.85f)
                        : isChrome ? PinkXP::pink400.interpolatedWith (PinkXP::hl, 0.35f).withAlpha (0.92f)
                        : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.78f)
                        : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.35f).withAlpha (0.95f)
                        : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::hl, 0.25f)
                                   : PinkXP::pink500;
    const auto divCol   = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.72f)
                        : isChrome ? etchedMetal.withAlpha (0.86f)
                        : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.68f)
                        : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                        : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                   : PinkXP::pink400;
    const auto qtrCol   = isModern ? PinkXP::skinHairlineOn (PinkXP::content, 0.68f)
                        : isChrome ? etchedMetal.withAlpha (0.76f)
                        : isGlass  ? PinkXP::skinHairlineOn (PinkXP::content, 0.62f)
                        : isNeon   ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.45f)
                        : isCrt    ? PinkXP::sel.interpolatedWith (PinkXP::skinHairlineOn (PinkXP::content, 0.85f), 0.35f)
                                   : PinkXP::pink400;

    // 零幅度基准线（双声道时每个声道带一条，经由 Craft::drawGridLine）
    Lane bgLanes[2];
    const int nBg = computeLanes (inner, bgLanes);
    for (int li = 0; li < nBg; ++li)
        PinkXP::Craft::drawGridLine (g, (float) inner.getX(), (float) bgLanes[li].cy,
                                     (float) inner.getRight(), (float) bgLanes[li].cy,
                                     true, zeroCol);
    if (nBg == 2)
        PinkXP::Craft::drawGridLine (g, (float) inner.getX(), (float) (bgLanes[0].y + bgLanes[0].h),
                                     (float) inner.getRight(), (float) (bgLanes[0].y + bgLanes[0].h),
                                     true, divCol);

    // 3 条竖向分区线
    for (int i = 1; i <= 3; ++i)
    {
        const int x = inner.getX() + (inner.getWidth() * i) / 4;
        PinkXP::Craft::drawGridLine (g, (float) x, (float) inner.getY(),
                                     (float) x, (float) inner.getBottom(),
                                     false, qtrCol);
    }
}

// ----------------------------------------------------------
// drawWaveform —— 瀑布主体（浮窗 / 无 GL 回退路径）
//   复用与 GL 完全一致的增量环形像素缓冲（SoftwareImageType），
//   每帧仅同步新增列 + 最多 2 次 g.drawImage 贴图，彻底消除逐列渐变卡顿。
// ----------------------------------------------------------
void WaveformModule::drawWaveform (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || validCols <= 0) return;

    auto inner = canvas.reduced (4);
    const int iw = inner.getWidth();
    const int ih = inner.getHeight();
    if (iw <= 0 || ih <= 0) return;

    auto* self = const_cast<WaveformModule*> (this);
    if (PinkXP::getAppearanceGeneration() != lastRasterAppearanceGen_)
        self->invalidateRingVisuals();
    self->ensureRingSurface (iw, ih);
    if (ringFullRebuild_)
        self->rasterizeAllRingColumns();

    syncSoftwareRingImage (iw, ih);
    if (! waveSwImg_.isValid())
        return;

    const int ix = inner.getX();
    const int iy = inner.getY();
    const int wc = juce::jlimit (0, ringW_, writeCol % juce::jmax (1, ringW_));
    const int olderCount = ringW_ - wc;

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (inner);
    g.setOpacity (1.0f);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

    if (scrollDirection == 1)
    {
        // 向右推进（最新在左、最旧在右）：对两段环形切片做水平镜像映射
        g.addTransform (juce::AffineTransform::scale (-1.0f, 1.0f)
                            .translated ((float) (ix * 2 + ringW_), 0.0f));
    }

    if (olderCount > 0)
        g.drawImage (waveSwImg_, ix, iy, olderCount, ringH_, wc, 0, olderCount, ringH_);
    if (wc > 0)
        g.drawImage (waveSwImg_, ix + olderCount, iy, wc, ringH_, 0, 0, wc, ringH_);
}
