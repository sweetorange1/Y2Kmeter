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
// Toybox（v3.0.0）：拍电视（§8.3）
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
    // 防连点：短时间冷却
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (now - lastToyPokeTime_ < 1.5)
        return;
    lastToyPokeTime_ = now;

    TOYPERF_SCOPED_TIMER ("osc snapshot (" + juce::String (displayMode == DisplayMode::waveform ? "wave" : "xy") + ")");

    // 反馈 C1：与 OscilloscopeWave 一致的"拍立得快照"——定格当前真实波形/散点。
    juce::Array<float> l, r;
    if (auto f = hub.getLatestFrame())
    {
        if (f->has (AnalyserHub::Kind::Oscilloscope))
        {
            const int n = (int) f->oscL.size();
            l.ensureStorageAllocated (n);
            r.ensureStorageAllocated (n);
            for (size_t i = 0; i < f->oscL.size(); ++i) l.add (f->oscL[i]);
            for (size_t i = 0; i < f->oscR.size(); ++i) r.add (f->oscR[i]);
        }
    }

    // 反馈 A2：XY / Lissajous 模式画散点图，波形模式画折线
    const bool scatter = (displayMode != DisplayMode::waveform);
    ctx.promoteToFree (makePolaroidActor (hit.pos, l, r, {}, scatter));
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
    s.setProperty("autoGain",    autoGain, nullptr);
    s.setProperty("showRings",   showRings, nullptr);
    s.setProperty("dotSize",     (double) dotSize, nullptr);
    s.setProperty("fadeSpeed",   (double) fadeSpeed, nullptr);
    return s;
}

void OscilloscopeModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
    // 缺键 = 保持当前值（构造默认 = 旧 Oscilloscope 外观）。
    //   frozen 键（v3.0 及以前的 Freeze 按钮）被忽略：没有锁存式冻结了，按未冻结加载。
    if (state.hasProperty ("displayMode"))
        displayMode = (DisplayMode) juce::jlimit (0, 2, (int) state.getProperty ("displayMode"));

    // oscilloscope_wave 旧存档的 channelMode 与 waveChannel 同义（0 左 / 1 右 / 2 双）
    if (state.hasProperty ("waveChannel"))
        waveChannel = juce::jlimit (0, 2, (int) state.getProperty ("waveChannel"));
    else if (state.hasProperty ("channelMode"))
        waveChannel = juce::jlimit (0, 2, (int) state.getProperty ("channelMode"));

    // 旧存档的 waveColour（0 Classic / 1 Pink）已废弃：Pink 选项删除，一律按主题配色
    if (state.hasProperty ("bandColour")) bandColour = (bool) state.getProperty ("bandColour");
    if (state.hasProperty ("crossLow"))   crossLowHz  = juce::jlimit (80.0f, 1000.0f, (float) (double) state.getProperty ("crossLow"));
    if (state.hasProperty ("crossHigh"))  crossHighHz = juce::jlimit (500.0f, 10000.0f, (float) (double) state.getProperty ("crossHigh"));
    if (state.hasProperty ("autoGain"))   autoGain    = (bool) state.getProperty ("autoGain");
    if (state.hasProperty ("showRings"))  showRings   = (bool) state.getProperty ("showRings");
    if (state.hasProperty ("dotSize"))    dotSize     = juce::jlimit (1.0f, 6.0f, (float) (double) state.getProperty ("dotSize"));
    if (state.hasProperty ("fadeSpeed"))  fadeSpeed   = juce::jlimit (0.2f, 4.0f, (float) (double) state.getProperty ("fadeSpeed"));

    if (! autoGain) autoGainFactor = 1.0f;
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

    out.push_back (ParamDef::choice ("waveChannel", "Channels",
        { "Left", "Right", "Both" },
        "Which channel traces to draw.").in ("Waveform").when (isWave));

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
    out.push_back (ParamDef::toggle ("showRings", "Level rings",
        "Concentric rings with dB labels behind the dots.").in ("Scatter").when (isScatter));
}

double OscilloscopeModule::getParamValue (const juce::String& id) const
{
    if (id == "displayMode") return (double) (int) displayMode;
    if (id == "waveChannel") return (double) waveChannel;
    if (id == "bandColour")  return bandColour ? 1.0 : 0.0;
    if (id == "crossLow")    return (double) crossLowHz;
    if (id == "crossHigh")   return (double) crossHighHz;
    if (id == "autoGain")    return autoGain ? 1.0 : 0.0;
    if (id == "showRings")   return showRings ? 1.0 : 0.0;
    if (id == "dotSize")     return (double) dotSize;
    if (id == "fadeSpeed")   return (double) fadeSpeed;
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
    else if (id == "bandColour")
    {
        const bool turningOn = (v >= 0.5) && ! bandColour;
        bandColour = v >= 0.5;
        trailNeedsClear = true;

        // 每次开启染色都把两个分频点复位到默认（200 / 2000，与 MiniMeters 一致）：
        //   抽屉里的滑条由 refreshControlPanel 回读，会同步显示新值。
        if (turningOn)
        {
            crossLowHz  = y2k::bands::BandSplitter::kDefaultLowCross;
            crossHighHz = y2k::bands::BandSplitter::kDefaultHighCross;
            splitter.setCrossover (crossLowHz, crossHighHz);
        }
    }
    else if (id == "crossLow")    { crossLowHz  = (float) v; splitter.setCrossover (crossLowHz, crossHighHz); trailNeedsClear = true; }
    else if (id == "crossHigh")   { crossHighHz = (float) v; splitter.setCrossover (crossLowHz, crossHighHz); trailNeedsClear = true; }
    else if (id == "autoGain")    { autoGain = v >= 0.5; if (! autoGain) autoGainFactor = 1.0f; }
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
            const float safeMax = juce::jmax (periodicMaxAccum, 0.001f);
            const float targetGain = juce::jlimit (0.1f, 10.0f, kTargetFraction / safeMax);

            if (std::abs (targetGain - autoGainFactor) > autoGainFactor * kDeadZone)
            {
                autoGainFactor = targetGain;
                dynamicLayerDirty = true;
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

    const int N = snapshotL.size();
    if (N <= 1)
        return;

    const bool scatterMode = (displayMode == DisplayMode::xy || displayMode == DisplayMode::lissajous);

    auto snap = beginGlSnapshot();
    snap->canvasLocal = canvas;

    if (scatterMode)
    {
        // 散点：点云走持久残影通道（点大小 / 消失速度可调，三路点云颜色不互相污染）
        snap->trailFbo   = true;
        snap->trailLayers = 1;
        snap->trailDecay = std::pow (kTrailDecayPerFrame,
                                     juce::jmax (0.0f, trailDtAccumSec) * 60.0f * fadeSpeed);
        snap->trailClear = trailNeedsClear;
        trailNeedsClear  = false;
        trailDtAccumSec  = 0.0f;

        snap->texW = canvas.getWidth();
        snap->texH = canvas.getHeight();
        snap->texTargetRect = canvas;
    }
    else
    {
        // v2.9.4：逻辑 FBO 渲染（规避 dock/聚焦时的透明与拖影问题，见框架注释）
        snap->renderToLogical = true;
        snap->logicalW = canvas.getWidth();
        snap->logicalH = canvas.getHeight();
    }

    const int maxPts = juce::jmax (64, inner.getWidth());
    snap->vertices.reserve ((size_t) (1500 * 6 * 6 + (size_t) 4 * maxPts * 6 * 6));

    auto pushRect = [&] (juce::Rectangle<float> r, juce::Colour c)
    {
        pushQuadVerts (snap->vertices, r.getX(), r.getY(), r.getRight(), r.getBottom(), c);
    };

    // ---- 背景：凹陷底（框架 helper）+ 点阵 + 十字 + 虚线 ----
    {
        appendSunkenFrame (snap->vertices, canvas);

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

    // ---- 波形折线 strip ----
    const float h     = (float) inner.getHeight();
    const float cy    = (float) inner.getCentreY();
    const float halfH = h * 0.45f;
    const int   targetPoints = juce::jmax (64, inner.getWidth());
    const int   stride = juce::jmax (1, N / targetPoints);

    // 抽点相位锁定：按「绝对样本索引 % stride」选集，而不是从窗口第 0 个样本数起。
    //   慢放时读窗口每帧只前进十几个样本（实时约 800 个），若按窗口相对下标抽点，
    //   抽到的样本逐帧换一批 —— 同一个波形被抽成几种不同形状，画面就在这些形状
    //   之间来回闪（看到的就是"大量毛刺 + 两个状态切换"）。锁到样本流之后，
    //   抽点集合只随窗口平移，形状保持不变。
    const juce::int64 baseIdx = juce::jmax<juce::int64> (0, snapshotBaseIndex);
    const int phase0 = (int) ((stride - (int) (baseIdx % (juce::int64) stride)) % stride);

    auto pushWave = [&] (const juce::Array<float>& samples,
                         juce::Colour glowCol, float glowHalfW,
                         juce::Colour mainCol, float mainHalfW)
    {
        std::vector<float> xs, ys;
        xs.reserve ((size_t) (N / stride + 4));
        ys.reserve ((size_t) (N / stride + 4));
        for (int i = phase0; i < N; i += stride)
        {
            const float raw = samples.getUnchecked (i);
            const float s = std::isfinite (raw) ? juce::jlimit (-1.0f, 1.0f, raw) : 0.0f;
            const float norm = (float) i / (float) juce::jmax (1, N - 1);
            xs.push_back ((float) inner.getX() + norm * (float) inner.getWidth());
            ys.push_back (cy - s * halfH);
        }
        if (phase0 < N && ((N - 1 - phase0) % stride) != 0)
        {
            const float raw = samples.getUnchecked (N - 1);
            const float s = std::isfinite (raw) ? juce::jlimit (-1.0f, 1.0f, raw) : 0.0f;
            xs.push_back ((float) inner.getRight());
            ys.push_back (cy - s * halfH);
        }

        const size_t ptCount = xs.size();
        for (int layer = 0; layer < 2; ++layer)
        {
            const float hw = (layer == 0) ? glowHalfW : mainHalfW;
            const auto col = (layer == 0) ? glowCol : mainCol;
            const float r = col.getFloatRed(), g2 = col.getFloatGreen(),
                        b = col.getFloatBlue(), a = col.getFloatAlpha();

            for (size_t i = 0; i + 1 < ptCount; ++i)
            {
                const float dx = xs[i + 1] - xs[i];
                const float dy = ys[i + 1] - ys[i];
                const float len = std::sqrt (dx * dx + dy * dy);
                if (len < 1.0e-6f) continue;
                const float nx = -dy / len * hw;
                const float ny =  dx / len * hw;

                const float q[36] = {
                    xs[i] + nx,     ys[i] + ny,     r, g2, b, a,
                    xs[i + 1] + nx, ys[i + 1] + ny, r, g2, b, a,
                    xs[i] - nx,     ys[i] - ny,     r, g2, b, a,
                    xs[i + 1] + nx, ys[i + 1] + ny, r, g2, b, a,
                    xs[i + 1] - nx, ys[i + 1] - ny, r, g2, b, a,
                    xs[i] - nx,     ys[i] - ny,     r, g2, b, a,
                };
                snap->vertices.insert (snap->vertices.end(), q, q + 36);
            }
        }
    };

    // ---- XY / Lissajous：标尺环 + 点云 ----
    if (displayMode == DisplayMode::xy || displayMode == DisplayMode::lissajous)
    {
        const bool rotate45 = (displayMode == DisplayMode::lissajous);
        auto innerXY = canvas.reduced (6);
        const float cxx = (float) innerXY.getCentreX();
        const float cyy = (float) innerXY.getCentreY();
        const float baseRadius = (float) juce::jmin (innerXY.getWidth(), innerXY.getHeight()) * 0.45f;
        const float gain = autoGainFactor;

        // 标尺环：4 个同心 1px 圆环 → 64 段细 quad
        if (showRings)
        {
            const auto ringCol = PinkXP::pink200.withAlpha (0.30f);
            const float rr0 = ringCol.getFloatRed(), rg0 = ringCol.getFloatGreen(),
                        rb0 = ringCol.getFloatBlue(), ra0 = ringCol.getFloatAlpha();
            const float ringFrac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
            constexpr int ringSegs = 64;

            // 环可能超出画布（autoGain 放大时）→ 逐段裁到画布内，
            //   否则 GL 直渲染会把线画到旁边的 GL 模块上。
            const auto ringClip = canvas.toFloat().reduced (1.0f);

            for (int ri = 0; ri < 4; ++ri)
            {
                const float rr = baseRadius * gain * ringFrac[ri];
                if (rr < 2.0f) continue;
                for (int s = 0; s < ringSegs; ++s)
                {
                    const float a0 = juce::MathConstants<float>::twoPi * (float) s       / (float) ringSegs;
                    const float a1 = juce::MathConstants<float>::twoPi * (float) (s + 1) / (float) ringSegs;
                    float x0 = cxx + rr * std::cos (a0), y0 = cyy + rr * std::sin (a0);
                    float x1 = cxx + rr * std::cos (a1), y1 = cyy + rr * std::sin (a1);

                    if (! clipSegmentToRect (x0, y0, x1, y1, ringClip))
                        continue;

                    const float dx = x1 - x0, dy = y1 - y0;
                    const float len = std::sqrt (dx * dx + dy * dy);
                    if (len < 1.0e-6f) continue;
                    const float nx = -dy / len * 0.5f;
                    const float ny =  dx / len * 0.5f;
                    const float q[36] = {
                        x0 + nx, y0 + ny, rr0, rg0, rb0, ra0,
                        x1 + nx, y1 + ny, rr0, rg0, rb0, ra0,
                        x0 - nx, y0 - ny, rr0, rg0, rb0, ra0,
                        x1 + nx, y1 + ny, rr0, rg0, rb0, ra0,
                        x1 - nx, y1 - ny, rr0, rg0, rb0, ra0,
                        x0 - nx, y0 - ny, rr0, rg0, rb0, ra0,
                    };
                    snap->vertices.insert (snap->vertices.end(), q, q + 36);
                }
            }
        }

        // 背景段（凹陷 / 网格 / 标尺环）直接画后缓冲；之后的点云进残影层
        snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
        snap->trailSegStart = (int) snap->segments.size();

        // 点云：点大小可调；三路点云各用自己的固定色，靠残影衰减留尾
        std::vector<XYPoint> pts;
        collectXYPoints (canvas, rotate45, pts);
        if (! pts.empty())
        {
            const float hs = juce::jmax (0.5f, dotSize * 0.5f);
            const int startIdx = (int) snap->vertices.size() / 6;
            for (const auto& p : pts)
                pushQuadVerts (snap->vertices, p.x - hs, p.y - hs, p.x + hs, p.y + hs,
                               p.col.withAlpha (p.alpha));
            snap->segments.push_back ({ startIdx,
                                        (int) (snap->vertices.size() / 6) - startIdx,
                                        Primitive::Triangles, 0 });
        }
    }
    else
    {
        // L 声道：glow 2.2px 半透明 → main 1.4px 实线；R 声道同理（与 drawWaveform 同序）
        const auto wc = waveColours();
        if (drawsL())
            pushWave (snapshotL, wc.lGlow, 1.1f, wc.lMain, 0.7f);
        if (drawsR() && snapshotR.size() == N)
            pushWave (snapshotR, wc.rGlow, 1.1f, wc.rMain, 0.7f);

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
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    auto canvas = getCanvasBounds(contentBounds);
    rebuildStaticLayerIfNeeded(contentBounds);
    drawStaticLayer(g, contentBounds);

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
}

// ----------------------------------------------------------
// 背景：凹陷画布 + 像素网格
// ----------------------------------------------------------
void OscilloscopeModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken(g, canvas, PinkXP::content);

    auto inner = canvas.reduced(2);
    if (inner.isEmpty()) return;

    g.setColour(PinkXP::pink200.withAlpha(0.35f));
    const int dotStep = 8;
    for (int y = inner.getY(); y < inner.getBottom(); y += dotStep)
        for (int x = inner.getX() + ((y / dotStep) % 2) * (dotStep / 2);
             x < inner.getRight(); x += dotStep)
            g.fillRect(x, y, 1, 1);

    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    const int cx = inner.getCentreX();
    const int cy = inner.getCentreY();
    g.drawHorizontalLine(cy, (float) inner.getX(), (float) inner.getRight());
    g.drawVerticalLine  (cx, (float) inner.getY(), (float) inner.getBottom());

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
// Waveform 模式：时域 L / R 叠加（声道、配色可选）
// ----------------------------------------------------------
void OscilloscopeModule::drawWaveform(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    const int N = snapshotL.size();
    if (N <= 1) return;

    auto inner = canvas.reduced(4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const float h  = (float) inner.getHeight();
    const float cy = (float) inner.getCentreY();
    const float halfH = h * 0.45f;
    const auto wc = waveColours();

    if (drawsL())
    {
        auto pathL = buildWaveformPath(snapshotL, inner, cy, halfH);
        g.setColour(wc.lGlow);
        g.strokePath(pathL, juce::PathStrokeType(2.2f));
        g.setColour(wc.lMain);
        g.strokePath(pathL, juce::PathStrokeType(1.4f));
    }

    if (drawsR() && snapshotR.size() == N)
    {
        auto pathR = buildWaveformPath(snapshotR, inner, cy, halfH);
        g.setColour(wc.rGlow);
        g.strokePath(pathR, juce::PathStrokeType(2.2f));
        g.setColour(wc.rMain);
        g.strokePath(pathR, juce::PathStrokeType(1.4f));
    }

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

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getCentreY();
    const float baseRadius = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.45f;
    const float gain = autoGainFactor;

    // ---- 标尺环 ----
    if (showRings)
    {
        const float ringFrac[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
        g.setColour(PinkXP::pink200.withAlpha(0.30f));
        for (int ri = 0; ri < 4; ++ri)
        {
            const float rr = baseRadius * gain * ringFrac[ri];
            if (rr < 2.0f) continue;
            g.drawEllipse(cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, 1.0f);
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

    const float ds = juce::jmax (1.0f, dotSize);
    const float hs = ds * 0.5f;

    if (xyBandActive())
    {
        // 三路点云：按层分组成 3 个 RectangleList，各一次 fillRectList
        //   （CPU 回退没有残影，直接画瞬时点；点色固定为频段色）
        juce::RectangleList<float> lists[3];
        for (auto& l : lists)
            l.ensureStorageAllocated ((int) pts.size() / 3 + 8);

        for (const auto& p : pts)
            lists[juce::jlimit (0, 2, p.layer)].addWithoutMerging ({ p.x - hs, p.y - hs, ds, ds });

        const auto bandCol = [] (const y2k::bands::Rgb& c)
        {
            return juce::Colour::fromFloatRGBA (c.r, c.g, c.b, 0.9f);
        };
        const juce::Colour cols[3] = { bandCol (y2k::bands::kLowRgb),
                                       bandCol (y2k::bands::kMidRgb),
                                       bandCol (y2k::bands::kHighRgb) };
        for (int li = 0; li < 3; ++li)
        {
            if (lists[li].isEmpty()) continue;
            g.setColour (cols[li]);
            g.fillRectList (lists[li]);
        }
    }
    else
    {
        // 单色点云：合并成一次 fillRectList
        juce::RectangleList<float> all;
        all.ensureStorageAllocated ((size_t) pts.size());
        for (const auto& p : pts)
            all.addWithoutMerging ({ p.x - hs, p.y - hs, ds, ds });

        g.setColour (PinkXP::chartRamp[5].withAlpha (0.9f));
        g.fillRectList (all);
    }

    // 左下角模式说明 + 当前增益
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

    const int targetPoints = juce::jmax(64, inner.getWidth());
    const int stride = juce::jmax(1, len / targetPoints);

    // 与 GL 路径同一套相位锁定（见 buildGlSnapshot 的 pushWave）
    const juce::int64 baseIdx = juce::jmax<juce::int64> (0, snapshotBaseIndex);
    const int phase0 = (int) ((stride - (int) (baseIdx % (juce::int64) stride)) % stride);

    bool started = false;
    for (int i = phase0; i < len; i += stride)
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

void OscilloscopeModule::rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds)
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
}

// ----------------------------------------------------------
// 方案 A：动态波形 / XY 点图缓存层
// ----------------------------------------------------------
void OscilloscopeModule::invalidateDynamicLayer()
{
    dynamicLayer = juce::Image();
    dynamicLayerCanvasBounds = {};
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

    const bool sizeChanged = (! dynamicLayer.isValid())
                          || dynamicLayer.getWidth()  != canvas.getWidth()
                          || dynamicLayer.getHeight() != canvas.getHeight();

    const bool modeChanged  = (lastDrawnMode != displayMode);
    const bool gainChanged  = (std::abs(autoGainFactor - lastDrawnAutoGain) > 0.005f);

    if (sizeChanged)
    {
        dynamicLayer = juce::Image(juce::Image::ARGB,
                                   canvas.getWidth(),
                                   canvas.getHeight(),
                                   true);
        dynamicLayerDirty = true;
    }

    if (modeChanged || gainChanged)
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
    lastDrawnMode     = displayMode;
    lastDrawnAutoGain = autoGainFactor;
    dynamicLayerDirty = false;
}
