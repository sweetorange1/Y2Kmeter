#include "source/ui/modules/OscilloscopeModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// OscilloscopeModule —— Pink XP 像素风立体声示波器（示波器模块）
// ==========================================================

namespace
{
    void pushQuadVerts (std::vector<float>& v, float x0, float y0, float x1, float y1, juce::Colour c)
    {
        const float r = c.getFloatRed(), g = c.getFloatGreen(), b = c.getFloatBlue(), a = c.getFloatAlpha();
        const float q[36] = {
            x0, y0, r, g, b, a,  x1, y0, r, g, b, a,  x0, y1, r, g, b, a,
            x1, y0, r, g, b, a,  x1, y1, r, g, b, a,  x0, y1, r, g, b, a,
        };
        v.insert (v.end(), q, q + 36);
    }

    // Liang–Barsky：把线段裁剪到矩形内。全部在界外返回 false，否则就地改写端点。
    //   GL 直渲染没有 scissor，超出画布的几何会画到别的 GL 模块上 ——
    //   X-Y / 李萨如的标尺环在自动缩放（autoGain > 1）时半径会超出画布，
    //   必须在进顶点流之前裁掉。
    bool clipSegmentToRect (float& x0, float& y0, float& x1, float& y1,
                            const juce::Rectangle<float>& r) noexcept
    {
        const float dx = x1 - x0, dy = y1 - y0;
        float t0 = 0.0f, t1 = 1.0f;

        const float p[4] = { -dx, dx, -dy, dy };
        const float q[4] = { x0 - r.getX(), r.getRight()  - x0,
                             y0 - r.getY(), r.getBottom() - y0 };

        for (int i = 0; i < 4; ++i)
        {
            if (std::abs (p[i]) < 1.0e-9f)
            {
                if (q[i] < 0.0f)
                    return false;       // 与该边平行且在界外
                continue;
            }

            const float t = q[i] / p[i];
            if (p[i] < 0.0f) { if (t > t1) return false; if (t > t0) t0 = t; }
            else             { if (t < t0) return false; if (t < t1) t1 = t; }
        }

        const float cx0 = x0 + t0 * dx, cy0 = y0 + t0 * dy;
        const float cx1 = x0 + t1 * dx, cy1 = y0 + t1 * dy;
        x0 = cx0; y0 = cy0; x1 = cx1; y1 = cy1;
        return true;
    }
}

OscilloscopeModule::OscilloscopeModule(AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::oscilloscope),
      hub(h)
{
    // Phase F：本模块只需要 Oscilloscope 一路 + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // oscilloscope 6×4 大格
    setControlPanelEnabled (true);

    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        invalidateStaticLayer();
        invalidateDynamicLayer();
        trailNeedsClear = true;
        glForceTrailClear.store (true, std::memory_order_release);
        buildGlSnapshot();   // GL 快照含底色/几何颜色，主题切换必须重建
        repaint();
    });
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

// ----------------------------------------------------------
// Toybox：双态荧光电子玩具（波形冲浪珠 / X-Y 轨道电子）
//   · 完全取代旧的拍立得打印纸，零不透明遮挡块、零全屏闪白；
//   · 按住慢放观察期间自动进入 0.10x 子弹时间并柔淡退让，松手恢复全速。
// ----------------------------------------------------------
bool OscilloscopeModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;

    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void OscilloscopeModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    // 轻量连点节流（支持畅快连点，同时上限 6 个并发 actor 防止叠加过多干扰视线）
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (now - lastToyPokeTime_ < 0.14)
        return;
    lastToyPokeTime_ = now;

    TOYPERF_SCOPED_TIMER ("osc electron (" + juce::String (displayMode == DisplayMode::waveform ? "surfer" : "orbit") + ")");

    if (ctx.state != nullptr && ctx.state->actors.size() >= 6)
        ctx.state->actors.erase (ctx.state->actors.begin());

    juce::Component::SafePointer<OscilloscopeModule> safe (this);
    auto isSlowMo = [safe]() -> bool
    {
        return safe != nullptr && (safe->hold.active || safe->holdPendingStart);
    };

    const auto canvas = getToyCanvasBounds();
    if (displayMode == DisplayMode::waveform)
    {
        // 直接沿当前画面正在显示的波形声道冲浪（Right 单声道用 R，其余用 L）
        const auto& wave = (waveChannel == 1 && ! snapshotR.isEmpty()) ? snapshotR : snapshotL;
        ctx.addModuleActor (makeOscWaveSurferActor (hit.pos, canvas, wave, std::move (isSlowMo)));
    }
    else
    {
        ctx.addModuleActor (makeOscOrbitElectronActor (hit.pos, canvas, std::move (isSlowMo)));
    }
}

// ----------------------------------------------------------
// 持久化 / 控制面板
// ----------------------------------------------------------
juce::ValueTree OscilloscopeModule::saveModuleSpecificState() const
{
    juce::ValueTree s("state");
    s.setProperty("displayMode", (int) displayMode, nullptr);
    s.setProperty("waveChannel", waveChannel, nullptr);
    s.setProperty("bandColour",  bandColour, nullptr);
    s.setProperty("crossLow",    (double) crossLowHz, nullptr);
    s.setProperty("crossHigh",   (double) crossHighHz, nullptr);
    s.setProperty("autoGain",      autoGain, nullptr);
    s.setProperty("showRings",     showRings, nullptr);
    s.setProperty("dotSize",       (double) dotSize, nullptr);
    s.setProperty("fadeSpeed",     (double) fadeSpeed, nullptr);
    s.setProperty("timeScale",     (double) timeScale, nullptr);
    s.setProperty("waveGainDb",    (double) waveGainDb, nullptr);
    s.setProperty("lineWidth",     (double) lineWidth, nullptr);
    s.setProperty("fillWave",      fillWave, nullptr);
    s.setProperty("showGrid",      showGrid, nullptr);
    s.setProperty("scatterGainDb", (double) scatterGainDb, nullptr);
    return s;
}

void OscilloscopeModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
    if (state.hasProperty ("displayMode"))
        displayMode = (DisplayMode) juce::jlimit (0, 2, (int) state.getProperty ("displayMode"));

    if (state.hasProperty ("waveChannel"))
        waveChannel = juce::jlimit (0, 2, (int) state.getProperty ("waveChannel"));
    else if (state.hasProperty ("channelMode"))
        waveChannel = juce::jlimit (0, 2, (int) state.getProperty ("channelMode"));

    if (state.hasProperty ("bandColour"))    bandColour    = (bool) state.getProperty ("bandColour");
    if (state.hasProperty ("crossLow"))      crossLowHz    = juce::jlimit (80.0f, 1000.0f, (float) (double) state.getProperty ("crossLow"));
    if (state.hasProperty ("crossHigh"))     crossHighHz   = juce::jlimit (500.0f, 10000.0f, (float) (double) state.getProperty ("crossHigh"));
    if (state.hasProperty ("autoGain"))      autoGain      = (bool) state.getProperty ("autoGain");
    if (state.hasProperty ("showRings"))     showRings     = (bool) state.getProperty ("showRings");
    if (state.hasProperty ("dotSize"))       dotSize       = juce::jlimit (1.0f, 6.0f, (float) (double) state.getProperty ("dotSize"));
    if (state.hasProperty ("fadeSpeed"))     fadeSpeed     = juce::jlimit (0.2f, 4.0f, (float) (double) state.getProperty ("fadeSpeed"));
    if (state.hasProperty ("timeScale"))     timeScale     = juce::jlimit (0.25f, 1.0f, (float) (double) state.getProperty ("timeScale"));
    if (state.hasProperty ("waveGainDb"))    waveGainDb    = juce::jlimit (-12.0f, 18.0f, (float) (double) state.getProperty ("waveGainDb"));
    if (state.hasProperty ("lineWidth"))     lineWidth     = juce::jlimit (1.0f, 4.0f, (float) (double) state.getProperty ("lineWidth"));
    if (state.hasProperty ("fillWave"))      fillWave      = (bool) state.getProperty ("fillWave");
    if (state.hasProperty ("showGrid"))      showGrid      = (bool) state.getProperty ("showGrid");
    if (state.hasProperty ("scatterGainDb")) scatterGainDb = juce::jlimit (-12.0f, 18.0f, (float) (double) state.getProperty ("scatterGainDb"));

    if (! autoGain)
    {
        autoGainFactor           = std::pow (10.0f, scatterGainDb * 0.05f);
        periodicMaxAccum         = 0.0f;
        prevPeriodicMax          = 1.0f;
        lastPeriodicGainUpdateMs = 0.0;
    }
    splitter.setCrossover (crossLowHz, crossHighHz);
    trailNeedsClear = true;

    invalidateStaticLayer();
    invalidateDynamicLayer();
    buildGlSnapshot();
    refreshControlPanel();
    repaint();
}

void OscilloscopeModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    const auto isWave = [this] { return displayMode == DisplayMode::waveform; };
    const auto isScatter = [this] { return displayMode != DisplayMode::waveform; };

    out.push_back (ParamDef::choice ("displayMode", "Display type",
        { "Waveform", "X-Y", "Lissajous" },
        "Waveform: signal against time. X-Y: left against right. Lissajous: the same turned 45 degrees (mid / side).")
        .in ("View"));
    out.push_back (ParamDef::toggle ("showGrid", "Grid",
        "Show the dotted background grid, centre crosshairs, and quarter dividers.")
        .in ("View"));

    out.push_back (ParamDef::choice ("waveChannel", "Channels",
        { "Left", "Right", "Both" },
        "Which channel traces to draw.").in ("Waveform").when (isWave));
    out.push_back (ParamDef::slider ("timeScale", "Time window", 0.25, 1.0, 0.05, " x",
        "Zooms the horizontal time span. Lower values magnify single waveform cycles.")
        .in ("Waveform").when (isWave));
    out.push_back (ParamDef::slider ("waveGainDb", "Vertical zoom", -12.0, 18.0, 1.0, " dB",
        "Boosts or attenuates waveform display height without changing audio.")
        .in ("Waveform").when (isWave));
    out.push_back (ParamDef::slider ("lineWidth", "Line thickness", 1.0, 4.0, 0.2, " px",
        "Stroke thickness of the oscilloscope trace.")
        .in ("Waveform").when (isWave));
    out.push_back (ParamDef::toggle ("fillWave", "Fill under wave",
        "Add a subtle translucent fill between the waveform trace and the centre zero line.")
        .in ("Waveform").when (isWave));

    out.push_back (ParamDef::toggle ("bandColour", "Colour by frequency",
        "Audio is split into three bands and each band's dots are drawn in its own colour: "
        "red for lows, purple-blue for mids, cyan for highs.")
        .in ("Scatter").when (isScatter));
    out.push_back (ParamDef::slider ("crossLow", "Low / mid split", 80.0, 1000.0, 10.0, " Hz",
        "Low band: below this frequency (red dot cloud).", 200.0)
        .in ("Scatter").when ([this] { return xyBandActive(); }));
    out.push_back (ParamDef::slider ("crossHigh", "Mid / high split", 500.0, 10000.0, 100.0, " Hz",
        "High band: above this frequency (cyan dot cloud).", 2000.0)
        .in ("Scatter").when ([this] { return xyBandActive(); }));
    out.push_back (ParamDef::slider ("dotSize", "Dot size", 1.0, 6.0, 1.0, " px",
        "Side length of each dot.").in ("Scatter").when (isScatter));
    out.push_back (ParamDef::slider ("fadeSpeed", "Fade speed", 0.2, 4.0, 0.1, "x",
        "How fast old dots disappear. 1.0 is the classic trail; higher is shorter, lower leaves long tails.")
        .in ("Scatter").when (isScatter));
    out.push_back (ParamDef::toggle ("autoGain", "Auto zoom",
        "Scales the picture so the loudest point stays inside the screen.").in ("Scatter").when (isScatter));
    out.push_back (ParamDef::slider ("scatterGainDb", "Manual zoom", -12.0, 18.0, 1.0, " dB",
        "Fixed zoom level for X-Y / Lissajous when Auto zoom is turned OFF.")
        .in ("Scatter").when ([this] { return displayMode != DisplayMode::waveform && ! autoGain; }));
    out.push_back (ParamDef::toggle ("showRings", "Level rings",
        "Concentric rings with dB labels behind the dots.").in ("Scatter").when (isScatter));
}

double OscilloscopeModule::getParamValue (const juce::String& id) const
{
    if (id == "displayMode")   return (double) (int) displayMode;
    if (id == "showGrid")      return showGrid ? 1.0 : 0.0;
    if (id == "waveChannel")   return (double) waveChannel;
    if (id == "timeScale")     return (double) timeScale;
    if (id == "waveGainDb")    return (double) waveGainDb;
    if (id == "lineWidth")     return (double) lineWidth;
    if (id == "fillWave")      return fillWave ? 1.0 : 0.0;
    if (id == "bandColour")    return bandColour ? 1.0 : 0.0;
    if (id == "crossLow")      return (double) crossLowHz;
    if (id == "crossHigh")     return (double) crossHighHz;
    if (id == "autoGain")      return autoGain ? 1.0 : 0.0;
    if (id == "scatterGainDb") return (double) scatterGainDb;
    if (id == "showRings")     return showRings ? 1.0 : 0.0;
    if (id == "dotSize")       return (double) dotSize;
    if (id == "fadeSpeed")     return (double) fadeSpeed;
    return 0.0;
}

void OscilloscopeModule::setParamValue (const juce::String& id, double v)
{
    if (id == "displayMode")
    {
        setDisplayMode ((DisplayMode) juce::jlimit (0, 2, juce::roundToInt (v)));
        return;
    }

    if      (id == "waveChannel") waveChannel = juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "showGrid")    { showGrid = v >= 0.5; invalidateStaticLayer(); }
    else if (id == "timeScale")   timeScale   = juce::jlimit (0.25f, 1.0f, (float) v);
    else if (id == "waveGainDb")  waveGainDb  = juce::jlimit (-12.0f, 18.0f, (float) v);
    else if (id == "lineWidth")   lineWidth   = juce::jlimit (1.0f, 4.0f, (float) v);
    else if (id == "fillWave")    fillWave    = v >= 0.5;
    else if (id == "bandColour")
    {
        const bool turningOn = (v >= 0.5) && ! bandColour;
        bandColour = v >= 0.5;
        trailNeedsClear = true;

        if (turningOn)
        {
            crossLowHz  = y2k::bands::BandSplitter::kDefaultLowCross;
            crossHighHz = y2k::bands::BandSplitter::kDefaultHighCross;
            splitter.setCrossover (crossLowHz, crossHighHz);
        }
    }
    else if (id == "crossLow")    { crossLowHz  = (float) v; splitter.setCrossover (crossLowHz, crossHighHz); trailNeedsClear = true; }
    else if (id == "crossHigh")   { crossHighHz = (float) v; splitter.setCrossover (crossLowHz, crossHighHz); trailNeedsClear = true; }
    else if (id == "autoGain")
    {
        const bool next = v >= 0.5;
        if (autoGain != next)
        {
            autoGain = next;
            if (! autoGain)
                autoGainFactor = std::pow (10.0f, scatterGainDb * 0.05f);
            periodicMaxAccum         = 0.0f;
            prevPeriodicMax          = 1.0f;
            lastPeriodicGainUpdateMs = 0.0;
            trailNeedsClear          = true;
            invalidateStaticLayer();
        }
    }
    else if (id == "scatterGainDb")
    {
        scatterGainDb = juce::jlimit (-12.0f, 18.0f, (float) v);
        if (! autoGain)
        {
            autoGainFactor  = std::pow (10.0f, scatterGainDb * 0.05f);
            trailNeedsClear = true;
        }
    }
    else if (id == "showRings")   showRings = v >= 0.5;
    else if (id == "dotSize")     dotSize   = juce::jlimit (1.0f, 6.0f, (float) v);
    else if (id == "fadeSpeed")   fadeSpeed = juce::jlimit (0.2f, 4.0f, (float) v);
    else return;

    invalidateDynamicLayer();
    buildGlSnapshot();
    repaint();
}

void OscilloscopeModule::setDisplayMode(DisplayMode m)
{
    if (displayMode == m) return;
    displayMode = m;
    trailNeedsClear = true;
    invalidateStaticLayer();
    invalidateDynamicLayer();
    // v2.9.3：三种模式都支持 GL 直渲染；切换后立即重建快照，避免旧模式闪一帧
    buildGlSnapshot();
    refreshControlPanel();
    repaint();
}

// ----------------------------------------------------------
// 按住慢放（参照 SpectrumModule 的子弹时间，状态机见 HoldSlowMo）
// ----------------------------------------------------------
void OscilloscopeModule::mouseDown (const juce::MouseEvent& e)
{
    ModulePanel::mouseDown (e);   // 保留基类行为（toFront / 右键菜单 / 边缘拖拽）

    // 只在"左键按在画布内部（避开边缘缩放热区）"时进入慢放；抽屉是子组件，
    //   点击它不会走到这里。
    if (e.mods.isLeftButtonDown()
        && getCanvasBounds (getContentBounds()).reduced (6).contains (e.getPosition())
        && ! getControlDrawerBounds().contains (e.getPosition()))
    {
        hold.press();
        holdPendingStart = true;   // 下一帧到达时用当前帧窗口作为历史起点
        invalidateDynamicLayer();
        buildGlSnapshot();
        repaint();
    }
}

void OscilloscopeModule::mouseUp (const juce::MouseEvent& e)
{
    ModulePanel::mouseUp (e);

    if (hold.active || holdPendingStart)
        endHold();
}

void OscilloscopeModule::beginHold (const AnalyserHub::FrameSnapshot& frame)
{
    history.begin (frame);
    holdPlayPos = (double) history.totalWritten();   // 读指针从"实时"出发
    holdPendingStart = false;
}

void OscilloscopeModule::endHold()
{
    // 松手直接回到实时：下一帧 onFrame 走实时分支；残影色由 HoldSlowMo::tick 渐隐
    hold.release();
    holdPendingStart = false;
    history.end();
    invalidateDynamicLayer();
}

// ----------------------------------------------------------
// onFrame：Hub 分发器回调
// ----------------------------------------------------------
void OscilloscopeModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    const int n = (int) frame.oscL.size();

    if (hold.active)
    {
        // ---- 慢放：从长历史里按"被拉长 50 倍的时间"推进读指针 ----
        if (holdPendingStart || ! history.isRecording())
            beginHold (frame);
        else
            history.feed (frame);

        holdPlayPos += (double) hold.dilate (frame.dtSec) * hub.getSampleRate();
        const double hi = (double) history.totalWritten();
        const double lo = (double) history.oldestReadable() + (double) n;
        holdPlayPos = juce::jlimit (lo < hi ? lo : hi, hi, holdPlayPos);

        readBufL.resize ((size_t) n);
        readBufR.resize ((size_t) n);
        history.read ((std::uint64_t) holdPlayPos, n, readBufL.data(), readBufR.data());

        snapshotL.resize (n);
        snapshotR.resize (n);
        std::memcpy (snapshotL.getRawDataPointer(), readBufL.data(), (size_t) n * sizeof (float));
        std::memcpy (snapshotR.getRawDataPointer(), readBufR.data(), (size_t) n * sizeof (float));
        snapshotBaseIndex = (juce::int64) (std::uint64_t) holdPlayPos - (juce::int64) n;
    }
    else
    {
        // Phase F 优化：std::array → juce::Array 改 memcpy 批量拷贝
        snapshotL.resize(n);
        snapshotR.resize(n);
        std::memcpy (snapshotL.getRawDataPointer(), frame.oscL.data(), (size_t) n * sizeof (float));
        std::memcpy (snapshotR.getRawDataPointer(), frame.oscR.data(), (size_t) n * sizeof (float));
        snapshotBaseIndex = (juce::int64) frame.oscTotalSamples - (juce::int64) n;
    }

    // 方案 A：只有数据真正变化才标脏动态层，避免每帧 4 次 strokePath
    if (snapshotChangedSinceLastDraw())
        dynamicLayerDirty = true;

    const bool scatter = (displayMode == DisplayMode::xy || displayMode == DisplayMode::lissajous);

    // v1.8.5 XY/Lissajous 自动缩放：峰值驱动（避免 RMS 过度补偿）
    //   · 每帧取样本最大欧氏距离 sqrt(L²+R²)，累积到 periodicMaxAccum
    //   · 每秒结算一次：gain = 1.0 / periodicMaxAccum → 峰值落在边界
    //   · 与上周期的 gain 差 <10% 则不缩放，避免频繁抖动
    if (scatter && autoGain && n > 0)
    {
        float frameMax = 0.0f;
        const int nr = juce::jmin (snapshotL.size(), snapshotR.size());
        for (int i = 0; i < nr; ++i)
        {
            const float l = snapshotL.getUnchecked(i);
            const float r = snapshotR.getUnchecked(i);
            const float dist = std::sqrt (l * l + r * r);
            if (dist > frameMax) frameMax = dist;
        }
        if (frameMax > periodicMaxAccum)
            periodicMaxAccum = frameMax;

        const double nowMs2 = juce::Time::getMillisecondCounterHiRes();
        constexpr double kPeriodMs       = 1000.0;
        constexpr float  kTargetFraction = 1.00f;
        constexpr float  kDeadZone       = 0.10f;

        if ((nowMs2 - lastPeriodicGainUpdateMs) >= kPeriodMs)
        {
            // 静音周期不重算：与 StereoFieldModule 一致，避免静音时把增益顶到上限 10x，
            //   导致下一段声音刚进来时画面突然炸出画布。
            if (periodicMaxAccum > 1.0e-3f)
            {
                const float targetGain = juce::jlimit (0.1f, 10.0f, kTargetFraction / periodicMaxAccum);

                if (std::abs (targetGain - autoGainFactor) > autoGainFactor * kDeadZone)
                {
                    autoGainFactor = targetGain;
                    dynamicLayerDirty = true;
                }
            }

            prevPeriodicMax            = periodicMaxAccum;
            periodicMaxAccum           = 0.0f;
            lastPeriodicGainUpdateMs   = nowMs2;
        }
    }

    // 频段染色：把当前窗口的 L/R 滤成低 / 中 / 高三路（仅 X-Y / 李萨如 + 开启时才付费）
    if (xyBandActive() && snapshotL.size() > 1 && snapshotR.size() == snapshotL.size())
    {
        const int nb = snapshotL.size();
        const double fs = hub.getSampleRate();
        if (fs != splitterRate)
        {
            splitter.prepare (fs, crossLowHz, crossHighHz);
            splitterRate = fs;
        }

        lowL.resize ((size_t) nb);  lowR.resize ((size_t) nb);
        midL.resize ((size_t) nb);  midR.resize ((size_t) nb);
        highL.resize ((size_t) nb); highR.resize ((size_t) nb);

        splitter.split (snapshotL.getRawDataPointer(), snapshotR.getRawDataPointer(), nb,
                        lowL.data(), lowR.data(), midL.data(), midR.data(),
                        highL.data(), highR.data());
    }

    // 散点残影：累计 dt，供 buildGlSnapshot 换算与帧率无关的衰减系数
    if (scatter)
        trailDtAccumSec = juce::jmin (0.5f, trailDtAccumSec + frame.dtSec);

    // v2.9：去掉 15ms×DPI 硬编码节流（高 DPI 下会锁到 ≤33fps），每个分发帧重绘
    // v2.9.3：同步生成 GL 直渲染快照（waveform / XY / Lissajous 三种模式）
    buildGlSnapshot();
    repaintFrameContent();
}

// ----------------------------------------------------------
// 配色 / 散点几何（GL 与 CPU 回退共用）
// ----------------------------------------------------------
OscilloscopeModule::WaveColours OscilloscopeModule::waveColours() const
{
    WaveColours c;
    // 主题 chartRamp 阶梯（S4 调色板重构）：波形配色不再提供 Pink 备选
    c.lGlow = PinkXP::chartRamp[3].withAlpha (0.35f); c.lMain = PinkXP::chartRamp[4];
    c.rGlow = PinkXP::chartRamp[5].withAlpha (0.35f); c.rMain = PinkXP::chartRamp[6];
    return c;
}

void OscilloscopeModule::collectXYPoints (juce::Rectangle<int> canvas, bool rotate45,
                                          std::vector<XYPoint>& out) const
{
    out.clear();
    const int N = juce::jmin (snapshotL.size(), snapshotR.size());
    if (N <= 1) return;

    auto inner = canvas.reduced (6);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getCentreY();
    const float baseRadius = (float) juce::jmin (inner.getWidth(), inner.getHeight()) * 0.45f;
    const float effectiveRadius = baseRadius * autoGainFactor;
    const auto innerF = inner.toFloat().expanded (1.0f);

    const bool useBand = xyBandActive() && (int) lowL.size() == N;

    // 三路时每路降密度：3 层点云总量控制在 ~3k 点（GL 顶点流别爆）
    const int stride = juce::jmax (1, N / (useBand ? 1024 : 2048));

    // 单层点云：把 (L,R) 映射到相位仪坐标
    auto pushCloud = [&] (const float* srcL, const float* srcR, juce::Colour col, int layer)
    {
        for (int i = 0; i < N; i += stride)
        {
            const float rawL = srcL[i];
            const float rawR = srcR[i];
            if (! std::isfinite (rawL) || ! std::isfinite (rawR))
                continue;

            // v1.8.5：样本直接限幅到 ±1，autoGain 在像素映射阶段生效
            const float lx = juce::jlimit (-1.0f, 1.0f, rawL);
            const float ry = juce::jlimit (-1.0f, 1.0f, rawR);

            float px, py;
            if (rotate45)
            {
                const float mid  = (lx + ry) * 0.7071067f;
                const float side = (lx - ry) * 0.7071067f;
                px = cx + side * effectiveRadius;
                py = cy - mid  * effectiveRadius;
            }
            else
            {
                px = cx + lx * effectiveRadius;
                py = cy - ry * effectiveRadius;
            }

            if (! innerF.contains (px, py))
                continue;

            // 残影本身负责衰减，点统一用固定 alpha
            out.push_back ({ px, py, 0.9f, col, layer });
        }
    };

    if (useBand)
    {
        // MiniMeters 风格：三路各自的点云 + 固定色，不混色
        const auto bandCol = [] (const y2k::bands::Rgb& c)
        {
            return juce::Colour::fromFloatRGBA (c.r, c.g, c.b, 1.0f);
        };
        out.reserve ((size_t) (N / stride * 3 + 3));
        pushCloud (lowL.data(),  lowR.data(),  bandCol (y2k::bands::kLowRgb),  0);
        pushCloud (midL.data(),  midR.data(),  bandCol (y2k::bands::kMidRgb),  1);
        pushCloud (highL.data(), highR.data(), bandCol (y2k::bands::kHighRgb), 2);
    }
    else
    {
        out.reserve ((size_t) (N / stride + 1));
        pushCloud (snapshotL.getRawDataPointer(), snapshotR.getRawDataPointer(),
                   PinkXP::chartRamp[5], 0);
    }
}

// ----------------------------------------------------------
// buildGlSnapshot —— GL 直渲染快照（v2.9.3，覆盖三种显示模式；
//   v2.9.4 迁移到统一框架，单段 GL_TRIANGLES 顶点流）
// ----------------------------------------------------------
void OscilloscopeModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过（拖动浮动窗口 resize 会每帧触发重建）

    const auto canvas = getCanvasBounds (getContentBounds());
    auto inner = canvas.reduced (4);
    if (inner.getWidth() <= 8 || inner.getHeight() <= 8)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool chromaKey   = tokens.isChromaKey;
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();
    const int N = snapshotL.size();
    const bool scatterMode = (displayMode == DisplayMode::xy || displayMode == DisplayMode::lissajous);
    const bool useBandLayers = scatterMode && (N > 1) && xyBandActive() && (int) lowL.size() == N;
    const int layerCount = useBandLayers ? 3 : 1;

    auto snap = beginGlSnapshot();
    snap->canvasLocal  = canvas;
    snap->fxRect       = canvas;
    snap->snapToPixels = false;

    if (scatterMode)
    {
        // 散点：点云走持久残影通道（3 频段染色时每频段独占一层 FBO，颜色互不污染）
        snap->trailFbo    = true;
        snap->trailLayers = layerCount;
        snap->trailDecay  = std::pow (kTrailDecayPerFrame,
                                      juce::jmax (0.0f, trailDtAccumSec) * 60.0f * fadeSpeed);
        snap->trailClear  = trailNeedsClear;
        trailNeedsClear   = false;
        trailDtAccumSec   = 0.0f;

        snap->texW          = canvas.getWidth();
        snap->texH          = canvas.getHeight();
        snap->texTargetRect = canvas;
        snap->texLinear     = true;
    }
    else
    {
        // v2.9.4：逻辑 FBO 渲染（规避 dock/聚焦时的透明与拖影问题，见框架注释）
        snap->renderToLogical = true;
        snap->logicalW        = canvas.getWidth();
        snap->logicalH        = canvas.getHeight();
    }

    const int maxPts = juce::jmax (64, inner.getWidth());
    snap->vertices.reserve ((size_t) (1500 * 6 * 6 + (size_t) 4 * maxPts * 6 * 6));

    // ---- 背景：凹槽底（SkinCraft 令牌驱动）+ 风格化网格（绿幕：只铺纯绿）----
    appendWellFrame (snap->vertices, canvas, PinkXP::content);
    if (! chromaKey && showGrid)
    {
        const int cx = inner.getCentreX();
        const int cy = inner.getCentreY();
        const int q1x = inner.getX() + inner.getWidth() / 4;
        const int q3x = inner.getX() + inner.getWidth() * 3 / 4;

        if (tokens.gridStyle == PinkXP::GridStyle::dottedPixel)
        {
            // Pixel XP 标志性 8px 错格像素点阵底纹 + 像素点阵十字/四分线
            const auto dot = PinkXP::pink200.withAlpha (0.35f);
            constexpr int dotStep = 8;
            for (int y = inner.getY(); y < inner.getBottom(); y += dotStep)
                for (int x = inner.getX() + ((y / dotStep) % 2) * (dotStep / 2);
                     x < inner.getRight(); x += dotStep)
                    pushQuadVerts (snap->vertices, (float) x, (float) y, (float) x + 1.0f, (float) y + 1.0f, dot);
        }

        // 中心十字主线（自动按 SkinTokens：Pixel XP 点阵 / Modern 细实线 / Neon 虚线 / CRT 磷光标尺）
        appendGridLine (snap->vertices, (float) inner.getX(), (float) cy,
                        (float) inner.getRight(), (float) cy, true, PinkXP::pink400);
        appendGridLine (snap->vertices, (float) cx, (float) inner.getY(),
                        (float) cx, (float) inner.getBottom(), true, PinkXP::pink400);

        // 四分辅助线（次线）
        appendGridLine (snap->vertices, (float) q1x, (float) inner.getY(),
                        (float) q1x, (float) inner.getBottom(), false, PinkXP::pink300);
        appendGridLine (snap->vertices, (float) q3x, (float) inner.getY(),
                        (float) q3x, (float) inner.getBottom(), false, PinkXP::pink300);

        if (tokens.gridStyle != PinkXP::GridStyle::dottedPixel)
        {
            const int q1y = inner.getY() + inner.getHeight() / 4;
            const int q3y = inner.getY() + inner.getHeight() * 3 / 4;
            appendGridLine (snap->vertices, (float) inner.getX(), (float) q1y,
                            (float) inner.getRight(), (float) q1y, false, PinkXP::pink300);
            appendGridLine (snap->vertices, (float) inner.getX(), (float) q3y,
                            (float) inner.getRight(), (float) q3y, false, PinkXP::pink300);
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            // Millennium Chrome：示波器主十字激光蚀刻分度微刻度（1/8 步进）
            const bool darkBg = PinkXP::content.getPerceivedBrightness() < 0.5f;
            const auto tickC  = PinkXP::hl.interpolatedWith (PinkXP::pink400, 0.45f)
                                    .withAlpha (darkBg ? 0.34f : 0.28f);
            for (int k = 1; k < 8; ++k)
            {
                if (k == 4) continue;
                const float tx = (float) inner.getX() + (float) inner.getWidth() * ((float) k / 8.0f);
                const float ty = (float) inner.getY() + (float) inner.getHeight() * ((float) k / 8.0f);
                appendQuad (snap->vertices, { std::floor (tx), (float) cy - 2.0f, 1.0f, 5.0f }, tickC);
                appendQuad (snap->vertices, { (float) cx - 2.0f, std::floor (ty), 5.0f, 1.0f }, tickC);
            }
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            // CRT Terminal：Tektronix 经典磷光示波器标尺微刻度（横轴 1/10，纵轴 1/8）
            const auto crtTickC = PinkXP::sel.interpolatedWith (PinkXP::pink400, 0.45f).withAlpha (0.50f);
            for (int k = 1; k < 10; ++k)
            {
                if (k == 5) continue;
                const float tx = std::floor ((float) inner.getX() + (float) inner.getWidth() * ((float) k / 10.0f));
                const float hLen = (k % 5 == 0) ? 4.0f : 2.5f;
                appendQuad (snap->vertices, { tx, (float) cy - hLen, 1.0f, hLen * 2.0f + 1.0f }, crtTickC);
            }
            for (int k = 1; k < 8; ++k)
            {
                if (k == 4) continue;
                const float ty = std::floor ((float) inner.getY() + (float) inner.getHeight() * ((float) k / 8.0f));
                const float wLen = (k % 2 == 0) ? 4.0f : 2.5f;
                appendQuad (snap->vertices, { (float) cx - wLen, ty, wLen * 2.0f + 1.0f, 1.0f }, crtTickC);
            }
        }
    }

    // ---- XY / Lissajous：标尺环 + 点云 ----
    if (scatterMode)
    {
        const bool rotate45 = (displayMode == DisplayMode::lissajous);
        auto innerXY = canvas.reduced (6);
        const float cxx = (float) innerXY.getCentreX();
        const float cyy = (float) innerXY.getCentreY();
        const float baseRadius = (float) juce::jmin (innerXY.getWidth(), innerXY.getHeight()) * 0.45f;
        const float gain = autoGainFactor;

        // 标尺环（绿幕不画；全风格平滑反走样圆环，Chrome 激光蚀刻 / Glass 轻反光 / Neon 虚线辉光 / CRT 磷光 / Modern 细实线）
        if (showRings && ! chromaKey && tokens.gridStyle != PinkXP::GridStyle::none)
        {
            const float ringFrac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
            const auto ringClip = canvas.toFloat().reduced (1.0f);
            constexpr int ringSegs = 64;
            const bool dashed   = (tokens.gridStyle == PinkXP::GridStyle::dashedTech);
            const bool phosphor = (tokens.gridStyle == PinkXP::GridStyle::phosphorCrosshair);
            const bool isModern = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
            const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
            const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
            const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
            const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;
            const auto ringCol  = isChrome
                                      ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.38f)
                                            .withAlpha (darkBg ? 0.30f : 0.26f)
                                      : isNeon
                                            ? PinkXP::pink300.withAlpha (0.36f)
                                            : phosphor
                                                  ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.38f)
                                                  : PinkXP::pink300.withAlpha (
                                                        isModern ? juce::jlimit (0.12f, 0.24f, tokens.gridMajorAlpha * 1.05f)
                                                                 : juce::jlimit (0.16f, 0.38f, tokens.gridMajorAlpha * 1.10f));
            const auto outerSpecRim = juce::Colours::white.withAlpha (darkBg ? 0.14f : 0.24f);

            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = baseRadius * gain * ringFrac[ri];
                if (rr < 2.0f) continue;

                for (int s = 0; s < ringSegs; ++s)
                {
                    if (dashed && (s & 1) != 0)
                        continue;

                    const float a0 = juce::MathConstants<float>::twoPi * (float) s       / (float) ringSegs;
                    const float a1 = juce::MathConstants<float>::twoPi * (float) (s + 1) / (float) ringSegs;
                    float x0 = cxx + rr * std::cos (a0), y0 = cyy + rr * std::sin (a0);
                    float x1 = cxx + rr * std::cos (a1), y1 = cyy + rr * std::sin (a1);

                    if (! clipSegmentToRect (x0, y0, x1, y1, ringClip))
                        continue;

                    if (phosphor && tokens.glowIntensity > 0.0f)
                        appendLineSegment (snap->vertices, x0, y0, x1, y1, 1.25f,
                                           ringCol.withMultipliedAlpha (0.28f));
                    else if (isNeon)
                        appendLineSegment (snap->vertices, x0, y0, x1, y1, 1.25f,
                                           PinkXP::sel.withAlpha (0.18f));
                    appendLineSegment (snap->vertices, x0, y0, x1, y1, isModern ? 0.45f : (isChrome ? 0.48f : 0.50f), ringCol);
                    if ((isChrome || isGlass) && ri == 3)
                    {
                        float rx0 = cxx + (rr + 0.85f) * std::cos (a0), ry0 = cyy + (rr + 0.85f) * std::sin (a0);
                        float rx1 = cxx + (rr + 0.85f) * std::cos (a1), ry1 = cyy + (rr + 0.85f) * std::sin (a1);
                        if (clipSegmentToRect (rx0, ry0, rx1, ry1, ringClip))
                            appendLineSegment (snap->vertices, rx0, ry0, rx1, ry1, 0.36f, outerSpecRim);
                    }
                }
            }
        }

        // 背景段（凹陷 / 网格 / 标尺环）直接画后缓冲；之后的点云按层进残影 FBO
        snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
        snap->trailSegStart = (int) snap->segments.size();

        // 点云：点大小可调；三频段染色时按 layer=0..2 独立分段，各写独立 FBO
        if (N > 1)
        {
            std::vector<XYPoint> pts;
            collectXYPoints (canvas, rotate45, pts);
            if (! pts.empty())
            {
                const float hs       = juce::jmax (0.5f, dotSize * 0.5f);
                const bool pixelDots = tokens.pixelSnapped;
                const bool crtHalo   = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor) && ! greenScreen && tokens.glowIntensity > 0.0f;
                const bool neonHalo  = (tokens.frameStyle == PinkXP::FrameStyle::neonTube) && ! greenScreen && tokens.glowIntensity > 0.0f;
                const bool glassDot  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass) && ! greenScreen;
                const bool chromePin = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome) && ! greenScreen && dotSize >= 1.5f;
                const float snapSize = juce::jmax (1.0f, std::round (dotSize));
                const int discSegs   = (dotSize >= 3.5f) ? 10 : 8;
                const float pinR     = juce::jmax (0.42f, hs * 0.42f);
                const float pinOff   = hs * 0.18f;

                for (int layer = 0; layer < layerCount; ++layer)
                {
                    const int startIdx = (int) snap->vertices.size() / 6;
                    for (const auto& p : pts)
                    {
                        if (useBandLayers && p.layer != layer)
                            continue;

                        const float pa = greenScreen ? 1.0f : p.alpha;
                        if (pixelDots)
                        {
                            const float qx = std::floor (p.x - hs);
                            const float qy = std::floor (p.y - hs);
                            if (crtHalo)
                                pushQuadVerts (snap->vertices, qx - 1.0f, qy - 1.0f, qx + snapSize + 1.0f, qy + snapSize + 1.0f,
                                               p.col.withAlpha (pa * 0.28f * tokens.glowIntensity));
                            pushQuadVerts (snap->vertices, qx, qy, qx + snapSize, qy + snapSize, p.col.withAlpha (pa));
                            if (crtHalo && snapSize >= 2.0f)
                            {
                                const float cx0 = qx + snapSize * 0.25f;
                                const float cy0 = qy + snapSize * 0.25f;
                                const float cw  = juce::jmax (1.0f, snapSize * 0.5f);
                                pushQuadVerts (snap->vertices, cx0, cy0, cx0 + cw, cy0 + cw,
                                               p.col.interpolatedWith (juce::Colours::white, 0.52f).withAlpha (pa * 0.60f));
                            }
                        }
                        else
                        {
                            if (neonHalo)
                                appendDisc (snap->vertices, p.x, p.y, hs * 2.15f + 0.9f,
                                            p.col.withAlpha (pa * 0.26f * tokens.glowIntensity), 8);
                            else if (glassDot)
                                appendDisc (snap->vertices, p.x, p.y, hs * 1.45f + 0.4f,
                                            p.col.withAlpha (pa * 0.16f), 8);
                            if (dotSize >= 2.0f)
                                appendDisc (snap->vertices, p.x, p.y, hs, p.col.withAlpha (pa), discSegs);
                            else
                                pushQuadVerts (snap->vertices, p.x - hs, p.y - hs, p.x + hs, p.y + hs, p.col.withAlpha (pa));
                            if (chromePin || (glassDot && dotSize >= 1.5f))
                                appendDisc (snap->vertices, p.x - pinOff, p.y - pinOff, pinR,
                                            (glassDot ? juce::Colours::white
                                                      : p.col.interpolatedWith (juce::Colours::white, 0.72f))
                                                .withAlpha (pa * (glassDot ? 0.62f : 0.82f)), 6);
                            else if (neonHalo && dotSize >= 2.0f)
                                appendDisc (snap->vertices, p.x, p.y, pinR,
                                            p.col.interpolatedWith (juce::Colours::white, 0.72f).withAlpha (pa * 0.78f), 6);
                        }
                    }

                    const int segCount = (int) (snap->vertices.size() / 6) - startIdx;
                    if (segCount > 0)
                        snap->segments.push_back ({ startIdx, segCount, Primitive::Triangles, layer });
                }
            }
        }
    }
    else
    {
        // ---- Waveform 模式：通过 appendStyledPolyline 自动实现 Pixel XP 锐利 / Modern 平滑 / Neon+CRT 辉光 ----
        if (N > 1)
        {
            const float h     = (float) inner.getHeight();
            const float cy    = (float) inner.getCentreY();
            const float halfH = h * 0.45f;
            const int   visN  = juce::jlimit (32, N, juce::roundToInt ((float) N * timeScale));
            const int   visStart = N - visN;
            const float ampGain  = (std::abs (waveGainDb) > 0.05f) ? std::pow (10.0f, waveGainDb * 0.05f) : 1.0f;
            const int   targetPoints = juce::jmax (64, inner.getWidth());
            const int   stride = juce::jmax (1, visN / targetPoints);

            const juce::int64 baseIdx = juce::jmax<juce::int64> (0, snapshotBaseIndex + visStart);
            const int phase0 = (int) ((stride - (int) (baseIdx % (juce::int64) stride)) % stride);

            auto pushWave = [&] (const juce::Array<float>& samples, juce::Colour mainCol)
            {
                std::vector<juce::Point<float>> pts;
                pts.reserve ((size_t) (visN / stride + 4));
                for (int j = phase0; j < visN; j += stride)
                {
                    const float raw = samples.getUnchecked (visStart + j);
                    const float s = std::isfinite (raw) ? juce::jlimit (-1.0f, 1.0f, raw * ampGain) : 0.0f;
                    const float norm = (float) j / (float) juce::jmax (1, visN - 1);
                    pts.push_back ({ (float) inner.getX() + norm * (float) inner.getWidth(), cy - s * halfH });
                }
                if (phase0 < visN && ((visN - 1 - phase0) % stride) != 0)
                {
                    const float raw = samples.getUnchecked (N - 1);
                    const float s = std::isfinite (raw) ? juce::jlimit (-1.0f, 1.0f, raw * ampGain) : 0.0f;
                    pts.push_back ({ (float) inner.getRight(), cy - s * halfH });
                }

                const size_t ptCount = pts.size();
                if (fillWave && ptCount > 1)
                {
                    const auto fc = mainCol.withAlpha (0.20f * tokens.areaFillAlphaScale);
                    const float fr = fc.getFloatRed(), fg = fc.getFloatGreen(), fb = fc.getFloatBlue(), fa = fc.getFloatAlpha();
                    for (size_t i = 0; i + 1 < ptCount; ++i)
                    {
                        const float q[36] = {
                            pts[i].x, cy, fr, fg, fb, fa,      pts[i + 1].x, cy, fr, fg, fb, fa,      pts[i].x, pts[i].y, fr, fg, fb, fa,
                            pts[i + 1].x, cy, fr, fg, fb, fa,  pts[i + 1].x, pts[i + 1].y, fr, fg, fb, fa,  pts[i].x, pts[i].y, fr, fg, fb, fa,
                        };
                        snap->vertices.insert (snap->vertices.end(), q, q + 36);
                    }
                }

                if (ptCount >= 2)
                {
                    appendStyledPolyline (snap->vertices, pts.data(), (int) ptCount, mainCol, lineWidth);
                    if (! greenScreen)
                    {
                        const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
                        const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
                        const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
                        const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
                        if (isChrome || isGlass || isNeon || isCrt)
                        {
                            const float whMix  = isNeon ? 0.64f : (isGlass ? 0.45f : 0.52f);
                            const float coreA  = isNeon ? 0.82f : (isCrt ? 0.74f : (isGlass ? 0.46f : 0.62f));
                            const float wScale = isNeon ? 0.34f : (isGlass ? 0.40f : 0.36f);
                            appendStyledPolyline (snap->vertices, pts.data(), (int) ptCount,
                                                  mainCol.interpolatedWith (juce::Colours::white, whMix).withAlpha (coreA),
                                                  juce::jmax (0.55f, lineWidth * wScale));
                        }
                    }
                }
            };

            const auto wc = waveColours();
            if (drawsL())
                pushWave (snapshotL, wc.lMain);
            if (drawsR() && snapshotR.size() == N)
                pushWave (snapshotR, wc.rMain);
        }

        snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    }

    publishGlSnapshot (std::move (snap));
}

// ----------------------------------------------------------
// 布局：v3.1 起没有工具栏，整块内容区都是画布
// ----------------------------------------------------------
juce::Rectangle<int> OscilloscopeModule::getCanvasBounds(juce::Rectangle<int> content) const
{
    return content;
}

void OscilloscopeModule::layoutContent(juce::Rectangle<int>)
{
}

// ----------------------------------------------------------
// 绘制
// ----------------------------------------------------------
//
// P5 平台分流：
//   · Windows：保留动态层 Image 缓存（方案 A），dirty 时才重绘。
//   · macOS：CoreGraphics Path stroke 由系统硬件加速，直接画到屏幕 Graphics。
//
namespace
{
#if JUCE_MAC
    constexpr bool kOscUseImageCache = false;
#else
    constexpr bool kOscUseImageCache = true;
#endif
}

// ---- GlDirectModuleBase 钩子 ----
juce::Rectangle<int> OscilloscopeModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void OscilloscopeModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 绿幕：图例 / 模式说明 / 标尺环 dB 标签都不画
    if (PinkXP::isChromaKeySkin())
        return;

    // 接管时只画图例/说明文字（GL 内容之上的 overlay）
    g.setColour (PinkXP::ink.withAlpha (0.85f));
    g.setFont   (PinkXP::getAxisFont (9.0f, juce::Font::plain));

    if (displayMode == DisplayMode::waveform)
    {
        const bool legend = (waveChannel == 2);
        if (legend)
        {
            // L / R 双色图例：色块取当前波形实际配色（跟随主题 chartRamp）
            const auto wc = waveColours();
            PinkXP::drawCachedText (g, "L", juce::Rectangle<int> (canvas.getX() + 4, canvas.getBottom() - 14, 24, 12),
                                    juce::Justification::centredLeft, 9.0f, juce::Font::plain,
                                    PinkXP::ink.withAlpha (0.85f), true);
            g.setColour (wc.lMain);
            g.fillRect  (canvas.getX() + 14, canvas.getBottom() - 9, 8, 2);
            PinkXP::drawCachedText (g, "R", juce::Rectangle<int> (canvas.getX() + 32, canvas.getBottom() - 14, 24, 12),
                                    juce::Justification::centredLeft, 9.0f, juce::Font::plain,
                                    PinkXP::ink.withAlpha (0.85f), true);
            g.setColour (wc.rMain);
            g.fillRect  (canvas.getX() + 42, canvas.getBottom() - 9, 8, 2);
        }
        else
        {
            // 旧 Oscilloscope Wave 的通道文字标签
            const juce::String modeLabel = (waveChannel == 0) ? "L" : (waveChannel == 1) ? "R" : "L+R";
            PinkXP::drawCachedText (g, modeLabel,
                                    juce::Rectangle<int> (canvas.getX() + 4, canvas.getBottom() - 14, 40, 12),
                                    juce::Justification::centredLeft, 9.0f, juce::Font::plain,
                                    PinkXP::ink.withAlpha (0.85f), true);
        }
    }
    else
    {
        const juce::String hint = (displayMode == DisplayMode::lissajous) ? "M/S" : "X=L  Y=R";
        PinkXP::drawCachedText (g, hint, juce::Rectangle<int> (canvas.getX() + 4, canvas.getBottom() - 14, 80, 12),
                                juce::Justification::centredLeft, 9.0f, juce::Font::plain,
                                PinkXP::ink.withAlpha (0.85f), true);
        if (std::abs (autoGainFactor - 1.0f) > 0.05f)
        {
            const juce::String gainText = juce::String (autoGainFactor, 1) + "x";
            g.drawText (y2k::tr (gainText), canvas.getRight() - 46, canvas.getBottom() - 14,
                        42, 12, juce::Justification::centredRight, false);
        }

        // 标尺环 dB 标签
        auto innerXY = canvas.reduced (6);
        const float baseRadius = (float) juce::jmin (innerXY.getWidth(), innerXY.getHeight()) * 0.45f;
        if (showRings && baseRadius * autoGainFactor >= 20.0f)
        {
            const float ringFrac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
            const char* labels[4] = { "-12", "-6", "-2.5", "0 dB" };
            const float cx = (float) innerXY.getCentreX();
            const float cy = (float) innerXY.getCentreY();
            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = baseRadius * autoGainFactor * ringFrac[ri];
                if (rr < 8.0f) continue;
                PinkXP::drawCachedText (g, labels[ri],
                                        juce::Rectangle<int> ((int) (cx + rr + 3), (int) (cy - 6), 26, 12),
                                        juce::Justification::centredLeft, 7.5f, juce::Font::plain,
                                        PinkXP::ink.withAlpha (0.55f), true);
            }
        }
    }
}

void OscilloscopeModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto& tokens = PinkXP::getSkinTokens();
    auto canvas = getCanvasBounds(contentBounds);

    if (PinkXP::isTransparentBackdrop())
    {
        // 透明模式：不铺任何不透明底色，也不画静态层（保持纯 alpha=0），仅绘波形/点云
    }
    else
    {
        const juce::Colour backdrop = PinkXP::isGreenScreenBackdrop()
                                          ? PinkXP::chromaKeyGreen()
                                          : (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                                                 ? PinkXP::btnFace
                                                 : PinkXP::face);
        g.setColour(backdrop);
        g.fillRect(contentBounds);

        rebuildStaticLayerIfNeeded(contentBounds);
        drawStaticLayer(g, contentBounds);
    }

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    if (kOscUseImageCache)
    {
        redrawDynamicLayerIfNeeded(canvas);
        if (dynamicLayer.isValid())
            g.drawImageAt(dynamicLayer, canvas.getX(), canvas.getY());
    }
    else
    {
        switch (displayMode)
        {
            case DisplayMode::waveform:  drawWaveform(g, canvas);        break;
            case DisplayMode::xy:        drawXY      (g, canvas, false); break;
            case DisplayMode::lissajous: drawXY      (g, canvas, true);  break;
        }
    }

    PinkXP::Craft::drawCanvasFinishOverlay(g, canvas);
}

// ----------------------------------------------------------
// 背景：凹陷画布 + SkinCraft 风格化网格
// ----------------------------------------------------------
void OscilloscopeModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const auto& tokens = PinkXP::getSkinTokens();
    if (PinkXP::isTransparentBackdrop())
        return;
    if (PinkXP::isGreenScreenBackdrop())
    {
        g.setColour (PinkXP::chromaKeyGreen());
        g.fillRect (canvas);
        return;
    }

    PinkXP::Craft::drawPanelWell(g, canvas, PinkXP::content, PinkXP::WellKind::canvas);
    if (! showGrid) return;

    auto inner = canvas.reduced(4);
    if (inner.isEmpty()) return;

    const int cx = inner.getCentreX();
    const int cy = inner.getCentreY();
    const int q1x = inner.getX() + inner.getWidth() / 4;
    const int q3x = inner.getX() + inner.getWidth() * 3 / 4;

    if (tokens.gridStyle == PinkXP::GridStyle::dottedPixel)
    {
        g.setColour(PinkXP::pink200.withAlpha(0.35f));
        const int dotStep = 8;
        for (int y = inner.getY(); y < inner.getBottom(); y += dotStep)
            for (int x = inner.getX() + ((y / dotStep) % 2) * (dotStep / 2);
                 x < inner.getRight(); x += dotStep)
                g.fillRect(x, y, 1, 1);
    }

    PinkXP::Craft::drawGridLine(g, (float) inner.getX(), (float) cy,
                                (float) inner.getRight(), (float) cy, true, PinkXP::pink400);
    PinkXP::Craft::drawGridLine(g, (float) cx, (float) inner.getY(),
                                (float) cx, (float) inner.getBottom(), true, PinkXP::pink400);

    PinkXP::Craft::drawGridLine(g, (float) q1x, (float) inner.getY(),
                                (float) q1x, (float) inner.getBottom(), false, PinkXP::pink300);
    PinkXP::Craft::drawGridLine(g, (float) q3x, (float) inner.getY(),
                                (float) q3x, (float) inner.getBottom(), false, PinkXP::pink300);

    if (tokens.gridStyle != PinkXP::GridStyle::dottedPixel)
    {
        const int q1y = inner.getY() + inner.getHeight() / 4;
        const int q3y = inner.getY() + inner.getHeight() * 3 / 4;
        PinkXP::Craft::drawGridLine(g, (float) inner.getX(), (float) q1y,
                                    (float) inner.getRight(), (float) q1y, false, PinkXP::pink300);
        PinkXP::Craft::drawGridLine(g, (float) inner.getX(), (float) q3y,
                                    (float) inner.getRight(), (float) q3y, false, PinkXP::pink300);
    }

    if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
    {
        const bool darkBg = PinkXP::content.getPerceivedBrightness() < 0.5f;
        g.setColour(PinkXP::hl.interpolatedWith(PinkXP::pink400, 0.45f)
                        .withAlpha(darkBg ? 0.34f : 0.28f));
        for (int k = 1; k < 8; ++k)
        {
            if (k == 4) continue;
            const int tx = inner.getX() + (inner.getWidth() * k) / 8;
            const int ty = inner.getY() + (inner.getHeight() * k) / 8;
            g.fillRect(tx, cy - 2, 1, 5);
            g.fillRect(cx - 2, ty, 5, 1);
        }
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
    {
        g.setColour(PinkXP::sel.interpolatedWith(PinkXP::pink400, 0.45f).withAlpha(0.50f));
        for (int k = 1; k < 10; ++k)
        {
            if (k == 5) continue;
            const int tx = inner.getX() + (inner.getWidth() * k) / 10;
            const int hLen = (k % 5 == 0) ? 4 : 2;
            g.fillRect(tx, cy - hLen, 1, hLen * 2 + 1);
        }
        for (int k = 1; k < 8; ++k)
        {
            if (k == 4) continue;
            const int ty = inner.getY() + (inner.getHeight() * k) / 8;
            const int wLen = (k % 2 == 0) ? 4 : 2;
            g.fillRect(cx - wLen, ty, wLen * 2 + 1, 1);
        }
    }
}

// ----------------------------------------------------------
// Waveform 模式：时域 L / R 叠加（Craft::drawStyledStroke 驱动）
// ----------------------------------------------------------
void OscilloscopeModule::drawWaveform(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    const int N = snapshotL.size();
    if (N <= 1) return;

    auto inner = canvas.reduced(4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const auto& tokens = PinkXP::getSkinTokens();
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool isChrome    = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass     = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeon      = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrt       = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const float h  = (float) inner.getHeight();
    const float cy = (float) inner.getCentreY();
    const float halfH = h * 0.45f;
    const auto wc = waveColours();

    auto paintChannelWave = [&] (const juce::Array<float>& samples, juce::Colour mainC)
    {
        auto path = buildWaveformPath (samples, inner, cy, halfH);
        if (fillWave && ! path.isEmpty())
        {
            juce::Path fillP (path);
            fillP.lineTo ((float) inner.getRight(), cy);
            fillP.lineTo ((float) inner.getX(), cy);
            fillP.closeSubPath();
            g.setColour (mainC.withAlpha (0.20f * tokens.areaFillAlphaScale));
            g.fillPath (fillP);
        }
        PinkXP::Craft::drawStyledStroke (g, path, mainC, lineWidth);
        if (! greenScreen && ! path.isEmpty() && (isChrome || isGlass || isNeon || isCrt))
        {
            const float whMix  = isNeon ? 0.64f : (isGlass ? 0.45f : 0.52f);
            const float coreA  = isNeon ? 0.82f : (isCrt ? 0.74f : (isGlass ? 0.46f : 0.62f));
            const float wScale = isNeon ? 0.34f : (isGlass ? 0.40f : 0.36f);
            g.setColour (mainC.interpolatedWith (juce::Colours::white, whMix).withAlpha (coreA));
            g.strokePath (path, juce::PathStrokeType (juce::jmax (0.55f, lineWidth * wScale) * tokens.strokeWidthScale,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }
    };

    if (drawsL())
        paintChannelWave (snapshotL, wc.lMain);

    if (drawsR() && snapshotR.size() == N)
        paintChannelWave (snapshotR, wc.rMain);

    if (tokens.isChromaKey)
        return;   // 绿幕：不画通道说明

    // 左下角说明（与 GL overlay 一致）
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    if (waveChannel == 2)
    {
        g.drawText(y2k::tr ("L"), canvas.getX() + 4, canvas.getBottom() - 14,
                   24, 12, juce::Justification::centredLeft, false);
        g.setColour(wc.lMain);
        g.fillRect(canvas.getX() + 14, canvas.getBottom() - 9, 8, 2);

        g.setColour(PinkXP::ink.withAlpha(0.85f));
        g.drawText(y2k::tr ("R"), canvas.getX() + 32, canvas.getBottom() - 14,
                   24, 12, juce::Justification::centredLeft, false);
        g.setColour(wc.rMain);
        g.fillRect(canvas.getX() + 42, canvas.getBottom() - 9, 8, 2);
    }
    else
    {
        const juce::String modeLabel = (waveChannel == 0) ? "L" : (waveChannel == 1) ? "R" : "L+R";
        g.drawText(y2k::tr (modeLabel), canvas.getX() + 4, canvas.getBottom() - 14,
                   40, 12, juce::Justification::centredLeft, false);
    }
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

    const auto& tokens = PinkXP::getSkinTokens();
    const bool  chromaKey = tokens.isChromaKey;
    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getCentreY();
    const float baseRadius = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.45f;
    const float gain = autoGainFactor;

    // ---- 标尺环（绿幕不画；全风格平滑反走样圆环，Chrome 激光蚀刻 / Glass 轻反光 / Neon 虚线辉光 / CRT 磷光 / Modern 细实线）----
    if (showRings && ! chromaKey && tokens.gridStyle != PinkXP::GridStyle::none)
    {
        const float ringFrac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
        const bool isModern = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
        const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
        const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
        const bool phosphor = (tokens.gridStyle == PinkXP::GridStyle::phosphorCrosshair);
        const bool darkBg   = PinkXP::content.getPerceivedBrightness() < 0.5f;
        const auto ringCol  = isChrome
                                  ? PinkXP::pink300.interpolatedWith (PinkXP::hl, 0.38f)
                                        .withAlpha (darkBg ? 0.30f : 0.26f)
                                  : isNeon
                                        ? PinkXP::pink300.withAlpha (0.36f)
                                        : phosphor
                                              ? PinkXP::sel.interpolatedWith (PinkXP::pink300, 0.45f).withAlpha (0.38f)
                                              : PinkXP::pink300.withAlpha (
                                                    isModern ? juce::jlimit (0.12f, 0.24f, tokens.gridMajorAlpha * 1.05f)
                                                             : juce::jlimit (0.16f, 0.38f, tokens.gridMajorAlpha * 1.10f));
        const auto outerSpecRim = juce::Colours::white.withAlpha (darkBg ? 0.14f : 0.24f);
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (canvas.reduced (1));

            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = baseRadius * gain * ringFrac[ri];
                if (rr < 2.0f) continue;

                if (tokens.gridStyle == PinkXP::GridStyle::dashedTech)
                {
                    constexpr int ringSegs = 64;
                    for (int s = 0; s < ringSegs; s += 2)
                    {
                        const float a0 = juce::MathConstants<float>::twoPi * (float) s / (float) ringSegs;
                        const float a1 = juce::MathConstants<float>::twoPi * (float) (s + 1) / (float) ringSegs;
                        const float x0 = cx + rr * std::cos (a0), y0 = cy + rr * std::sin (a0);
                        const float x1 = cx + rr * std::cos (a1), y1 = cy + rr * std::sin (a1);
                        if (isNeon)
                        {
                            g.setColour (PinkXP::sel.withAlpha (0.18f));
                            g.drawLine (x0, y0, x1, y1, 2.5f);
                        }
                        g.setColour (ringCol);
                        g.drawLine (x0, y0, x1, y1, 1.0f);
                    }
                }
                else
                {
                    if (phosphor && tokens.glowIntensity > 0.0f)
                    {
                        g.setColour (ringCol.withMultipliedAlpha (0.28f));
                        g.drawEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, 2.5f);
                    }
                    g.setColour (ringCol);
                    g.drawEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, isModern ? 0.9f : (isChrome ? 0.95f : 1.0f));
                    if ((isChrome || isGlass) && ri == 3)
                    {
                        const float rOut = rr + 0.85f;
                        g.setColour (outerSpecRim);
                        g.drawEllipse (cx - rOut, cy - rOut, rOut * 2.0f, rOut * 2.0f, 0.72f);
                    }
                }
            }
        }

        if (baseRadius * gain * 1.0f >= 20.0f)
        {
            g.setColour(PinkXP::ink.withAlpha(0.55f));
            g.setFont(PinkXP::getAxisFont(7.5f, juce::Font::plain));
            const char* labels[4] = { "-12", "-6", "-2.5", "0 dB" };
            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = baseRadius * gain * ringFrac[ri];
                if (rr < 8.0f) continue;
                g.drawText(y2k::tr (labels[ri]),
                           juce::Rectangle<int>((int)(cx + rr + 3), (int)(cy - 6), 26, 12),
                           juce::Justification::centredLeft, false);
            }
        }
    }

    std::vector<XYPoint> pts;
    collectXYPoints (canvas, rotate45, pts);

    const float ds          = juce::jmax (1.0f, dotSize);
    const float hs          = ds * 0.5f;
    const bool  greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool  pixelDots   = tokens.pixelSnapped;
    const bool  crtHalo     = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor) && ! greenScreen && tokens.glowIntensity > 0.0f;
    const bool  neonHalo    = (tokens.frameStyle == PinkXP::FrameStyle::neonTube) && ! greenScreen && tokens.glowIntensity > 0.0f;
    const bool  glassDot    = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass) && ! greenScreen;
    const bool  chromePin   = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome) && ! greenScreen && ds >= 1.5f;
    const float snapSize    = juce::jmax (1.0f, std::round (ds));
    const float dotAlpha    = greenScreen ? 1.0f : 0.9f;
    const float pinR        = juce::jmax (0.42f, hs * 0.42f);
    const float pinOff      = hs * 0.18f;

    auto paintCloud = [&] (const std::vector<XYPoint>& points, int filterLayer, juce::Colour baseCol)
    {
        const auto mainCol = baseCol.withAlpha (dotAlpha);
        if (pixelDots)
        {
            if (crtHalo)
            {
                juce::RectangleList<float> halos;
                halos.ensureStorageAllocated ((int) points.size());
                for (const auto& p : points)
                {
                    if (filterLayer >= 0 && p.layer != filterLayer) continue;
                    const float qx = std::floor (p.x - hs), qy = std::floor (p.y - hs);
                    halos.addWithoutMerging ({ qx - 1.0f, qy - 1.0f, snapSize + 2.0f, snapSize + 2.0f });
                }
                if (! halos.isEmpty())
                {
                    g.setColour (baseCol.withAlpha (dotAlpha * 0.28f * tokens.glowIntensity));
                    g.fillRectList (halos);
                }
            }

            juce::RectangleList<float> rects;
            rects.ensureStorageAllocated ((int) points.size());
            for (const auto& p : points)
            {
                if (filterLayer >= 0 && p.layer != filterLayer) continue;
                rects.addWithoutMerging ({ std::floor (p.x - hs), std::floor (p.y - hs), snapSize, snapSize });
            }
            if (! rects.isEmpty())
            {
                g.setColour (mainCol);
                g.fillRectList (rects);
            }

            if (crtHalo && snapSize >= 2.0f)
            {
                const float cw = juce::jmax (1.0f, snapSize * 0.5f);
                juce::RectangleList<float> cores;
                cores.ensureStorageAllocated ((int) points.size());
                for (const auto& p : points)
                {
                    if (filterLayer >= 0 && p.layer != filterLayer) continue;
                    const float qx = std::floor (p.x - hs), qy = std::floor (p.y - hs);
                    cores.addWithoutMerging ({ qx + snapSize * 0.25f, qy + snapSize * 0.25f, cw, cw });
                }
                if (! cores.isEmpty())
                {
                    g.setColour (baseCol.interpolatedWith (juce::Colours::white, 0.52f).withAlpha (dotAlpha * 0.60f));
                    g.fillRectList (cores);
                }
            }
            return;
        }

        if (neonHalo)
        {
            const float hr = hs * 2.15f + 0.9f;
            g.setColour (baseCol.withAlpha (dotAlpha * 0.26f * tokens.glowIntensity));
            for (const auto& p : points)
            {
                if (filterLayer >= 0 && p.layer != filterLayer) continue;
                g.fillEllipse (p.x - hr, p.y - hr, hr * 2.0f, hr * 2.0f);
            }
        }
        else if (glassDot)
        {
            const float hr = hs * 1.45f + 0.4f;
            g.setColour (baseCol.withAlpha (dotAlpha * 0.16f));
            for (const auto& p : points)
            {
                if (filterLayer >= 0 && p.layer != filterLayer) continue;
                g.fillEllipse (p.x - hr, p.y - hr, hr * 2.0f, hr * 2.0f);
            }
        }

        if (ds >= 2.0f)
        {
            g.setColour (mainCol);
            for (const auto& p : points)
            {
                if (filterLayer >= 0 && p.layer != filterLayer) continue;
                g.fillEllipse (p.x - hs, p.y - hs, ds, ds);
            }
        }
        else
        {
            juce::RectangleList<float> rects;
            rects.ensureStorageAllocated ((int) points.size());
            for (const auto& p : points)
            {
                if (filterLayer >= 0 && p.layer != filterLayer) continue;
                rects.addWithoutMerging ({ p.x - hs, p.y - hs, ds, ds });
            }
            if (! rects.isEmpty())
            {
                g.setColour (mainCol);
                g.fillRectList (rects);
            }
        }

        if (chromePin || (glassDot && ds >= 1.5f))
        {
            g.setColour ((glassDot ? juce::Colours::white
                                   : baseCol.interpolatedWith (juce::Colours::white, 0.72f))
                             .withAlpha (dotAlpha * (glassDot ? 0.62f : 0.82f)));
            for (const auto& p : points)
            {
                if (filterLayer >= 0 && p.layer != filterLayer) continue;
                g.fillEllipse (p.x - pinOff - pinR, p.y - pinOff - pinR, pinR * 2.0f, pinR * 2.0f);
            }
        }
        else if (neonHalo && ds >= 2.0f)
        {
            g.setColour (baseCol.interpolatedWith (juce::Colours::white, 0.72f).withAlpha (dotAlpha * 0.78f));
            for (const auto& p : points)
            {
                if (filterLayer >= 0 && p.layer != filterLayer) continue;
                g.fillEllipse (p.x - pinR, p.y - pinR, pinR * 2.0f, pinR * 2.0f);
            }
        }
    };

    if (xyBandActive())
    {
        const auto bandCol = [] (const y2k::bands::Rgb& c)
        {
            return juce::Colour::fromFloatRGBA (c.r, c.g, c.b, 1.0f);
        };
        const juce::Colour cols[3] = { bandCol (y2k::bands::kLowRgb),
                                       bandCol (y2k::bands::kMidRgb),
                                       bandCol (y2k::bands::kHighRgb) };
        for (int li = 0; li < 3; ++li)
            paintCloud (pts, li, cols[li]);
    }
    else
    {
        paintCloud (pts, -1, PinkXP::chartRamp[5]);
    }

    // 左下角模式说明 + 当前增益（绿幕不画）
    if (chromaKey)
        return;
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    const juce::String hint = rotate45 ? "M/S" : "X=L  Y=R";
    g.drawText(y2k::tr (hint), canvas.getX() + 4, canvas.getBottom() - 14,
               80, 12, juce::Justification::centredLeft, false);
    if (std::abs(gain - 1.0f) > 0.05f)
    {
        juce::String gainText = juce::String(gain, 1) + "x";
        g.drawText(y2k::tr (gainText), canvas.getRight() - 46, canvas.getBottom() - 14,
                   42, 12, juce::Justification::centredRight, false);
    }
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

    const int visN = juce::jlimit (32, len, juce::roundToInt ((float) len * timeScale));
    const int visStart = len - visN;
    const float ampGain = (std::abs (waveGainDb) > 0.05f) ? std::pow (10.0f, waveGainDb * 0.05f) : 1.0f;

    const int targetPoints = juce::jmax(64, inner.getWidth());
    const int stride = juce::jmax(1, visN / targetPoints);

    const juce::int64 baseIdx = juce::jmax<juce::int64> (0, snapshotBaseIndex + visStart);
    const int phase0 = (int) ((stride - (int) (baseIdx % (juce::int64) stride)) % stride);

    bool started = false;
    for (int j = phase0; j < visN; j += stride)
    {
        const float raw = samples.getUnchecked(visStart + j);
        const float s = std::isfinite(raw) ? juce::jlimit(-1.0f, 1.0f, raw * ampGain) : 0.0f;
        const float norm = (float) j / (float) juce::jmax(1, visN - 1);
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

    if (phase0 < visN && ((visN - 1 - phase0) % stride) != 0)
    {
        const float raw = samples.getUnchecked(len - 1);
        const float s = std::isfinite(raw) ? juce::jlimit(-1.0f, 1.0f, raw * ampGain) : 0.0f;
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
        staticLayerAppearanceGen = 0;
        staticLayerFontGen       = 0;
        return;
    }

    const auto curAppGen  = PinkXP::getAppearanceGeneration();
    const auto curFontGen = PinkXP::getFontGeneration();

    if (staticLayer.isValid()
        && staticLayerContentBounds == contentBounds
        && staticLayer.getWidth() == contentBounds.getWidth()
        && staticLayer.getHeight() == contentBounds.getHeight()
        && staticLayerAppearanceGen == curAppGen
        && staticLayerFontGen == curFontGen)
    {
        return;
    }

    staticLayer = juce::Image(juce::Image::ARGB,
                              contentBounds.getWidth(),
                              contentBounds.getHeight(),
                              true);
    staticLayer.clear(staticLayer.getBounds(), juce::Colours::transparentBlack);
    staticLayerContentBounds = contentBounds;
    staticLayerAppearanceGen = curAppGen;
    staticLayerFontGen       = curFontGen;

    const auto& tokens = PinkXP::getSkinTokens();
    juce::Graphics sg(staticLayer);
    if (! PinkXP::isTransparentBackdrop())
    {
        const juce::Colour backdrop = PinkXP::isGreenScreenBackdrop()
                                          ? PinkXP::chromaKeyGreen()
                                          : (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                                                 ? PinkXP::btnFace
                                                 : PinkXP::face);
        sg.setColour(backdrop);
        sg.fillRect(staticLayer.getBounds());
    }

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
    staticLayerAppearanceGen = 0;
    staticLayerFontGen       = 0;
}

// ----------------------------------------------------------
// 方案 A：动态波形 / XY 点图缓存层
// ----------------------------------------------------------
void OscilloscopeModule::invalidateDynamicLayer()
{
    dynamicLayer = juce::Image();
    dynamicLayerCanvasBounds = {};
    dynamicLayerAppearanceGen = 0;
    dynamicLayerFontGen       = 0;
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

    const auto curAppGen  = PinkXP::getAppearanceGeneration();
    const auto curFontGen = PinkXP::getFontGeneration();

    const bool sizeChanged = (! dynamicLayer.isValid())
                          || dynamicLayer.getWidth()  != canvas.getWidth()
                          || dynamicLayer.getHeight() != canvas.getHeight();

    const bool modeChanged       = (lastDrawnMode != displayMode);
    const bool gainChanged       = (std::abs(autoGainFactor - lastDrawnAutoGain) > 0.005f);
    const bool appearanceChanged = (dynamicLayerAppearanceGen != curAppGen
                                    || dynamicLayerFontGen != curFontGen);

    if (sizeChanged)
    {
        dynamicLayer = juce::Image(juce::Image::ARGB,
                                   canvas.getWidth(),
                                   canvas.getHeight(),
                                   true);
        dynamicLayerDirty = true;
    }

    if (modeChanged || gainChanged || appearanceChanged)
        dynamicLayerDirty = true;

    dynamicLayerCanvasBounds = canvas;

    if (! dynamicLayerDirty)
        return;

    dynamicLayer.clear(dynamicLayer.getBounds(), juce::Colours::transparentBlack);

    juce::Graphics dg(dynamicLayer);
    const juce::Rectangle<int> localCanvas(0, 0, canvas.getWidth(), canvas.getHeight());

    switch (displayMode)
    {
        case DisplayMode::waveform:  drawWaveform(dg, localCanvas);        break;
        case DisplayMode::xy:        drawXY      (dg, localCanvas, false); break;
        case DisplayMode::lissajous: drawXY      (dg, localCanvas, true);  break;
    }

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
    lastDrawnMode             = displayMode;
    lastDrawnAutoGain         = autoGainFactor;
    dynamicLayerAppearanceGen = curAppGen;
    dynamicLayerFontGen       = curFontGen;
    dynamicLayerDirty         = false;
}
