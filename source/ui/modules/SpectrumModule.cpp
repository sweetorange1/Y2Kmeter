#include "source/ui/modules/SpectrumModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"
#include <cmath>
#include <initializer_list>
#include <limits>

// ==========================================================
// SpectrumModule
// ==========================================================

SpectrumModule::SpectrumModule(AnalyserHub& h)
    : GlDirectModuleBase(ModuleType::spectrum),
      hub(h)
{
    // Phase F：本模块只需要 Spectrum 一路 + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Spectrum);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // spectrum 6×4 大格
    setTitleText("Spectrum");
    setControlPanelEnabled (true);   // v3.1：Peak / Sharp / 曲线 / 通道 / FFT scale 都在参数面板里

    rawMags.ensureStorageAllocated (256);
    rawMagsSide.ensureStorageAllocated (256);
    smoothedDb .reserve (256);
    secondaryDb.reserve (256);
    peakDb     .reserve (256);
    blurredDb  .reserve (256);
    secondaryBlurDb.reserve (256);
    slopeOffsetDb.reserve (256);
    curvePts     .reserve (256);
    secondaryPts .reserve (256);
    peakCurvePts .reserve (256);

    lastTickTime = juce::Time::getCurrentTime();
}

SpectrumModule::~SpectrumModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Spectrum);
}

juce::ValueTree SpectrumModule::saveModuleSpecificState() const
{
    juce::ValueTree s("state");
    s.setProperty("peakHold", peakHoldEnabled, nullptr);
    s.setProperty("sharp", sharpEnabled, nullptr);
    s.setProperty("curve", juce::String (curveMode == CurveMode::smooth ? "smooth"
                                        : curveMode == CurveMode::semi   ? "semi"
                                                                         : "raw"), nullptr);
    s.setProperty("channel", juce::String (channelMode == ChannelMode::mid       ? "mid"
                                         : channelMode == ChannelMode::midSide   ? "midside"
                                                                                 : "lr"), nullptr);
    // slope：数值（dB/oct）。旧版是布尔开关，读档时迁移（见 restore）
    s.setProperty("slope", (double) slopeDbPerOct, nullptr);
    s.setProperty("fftScale", juce::String (fftScale == y2k::fscale::Scale::mel    ? "mel"
                                          : fftScale == y2k::fscale::Scale::linear ? "linear"
                                                                                   : "log"), nullptr);
    return s;
}

void SpectrumModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
    if (state.hasProperty("peakHold"))
        setPeakHoldEnabled((bool) state.getProperty("peakHold"));
    if (state.hasProperty("sharp"))
        setSharpEnabled((bool) state.getProperty("sharp"));

    // 曲线模式：优先读新的 "curve"；旧存档里的布尔 "smooth" 做一次性迁移
    const juce::String cm = state.getProperty("curve").toString();
    if (cm == "smooth" || cm == "semi" || cm == "raw")
        setCurveMode (cm == "semi" ? CurveMode::semi
                    : cm == "raw"  ? CurveMode::raw
                                   : CurveMode::smooth);
    else if (state.hasProperty("smooth"))
        setCurveMode ((bool) state.getProperty("smooth") ? CurveMode::smooth : CurveMode::raw);

    const juce::String ch = state.getProperty("channel", "mid").toString();
    setChannelMode (ch == "midside" ? ChannelMode::midSide
                  : ch == "lr"      ? ChannelMode::leftRight
                                    : ChannelMode::mid);

    // FFT scale：缺键 = 保持当前（旧存档没有这个键 → 构造默认的 Log = 旧外观）
    if (state.hasProperty("fftScale"))
    {
        const juce::String fs = state.getProperty("fftScale").toString();
        setFftScale (fs == "mel" ? y2k::fscale::Scale::mel
                   : fs == "linear" ? y2k::fscale::Scale::linear
                                    : y2k::fscale::Scale::log);
    }

    // slope：新存档是数值；旧存档是布尔开关（true = 当年的 4.5 dB/oct，false = 关）
    if (state.hasProperty("slope"))
    {
        const auto v = state.getProperty("slope");
        setSlopeDbPerOct (v.isBool() ? ((bool) v ? 4.5f : 0.0f)
                                     : (float) (double) v);
    }

    refreshControlPanel();
}

void SpectrumModule::setCurveMode (CurveMode m)
{
    if (curveMode == m) return;
    curveMode = m;

    // 切换列数会重置平滑/峰值状态（尺寸变化），立刻重建一次快照
    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setChannelMode (ChannelMode m)
{
    if (channelMode == m) return;
    channelMode = m;

    // 切换模式时清空次曲线状态，避免上一模式（如 side）的旧值淡入新曲线
    secondaryDb.clear();
    secondaryBlurDb.clear();

    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setFftScale (y2k::fscale::Scale s)
{
    if (fftScale == s) return;
    fftScale = s;

    // 横轴映射变了：列频率不同 → 平滑状态、斜率补偿缓存、静态层（网格 + 轴标签）全部作废
    smoothedDb.clear();
    secondaryDb.clear();
    peakDb.clear();
    slopeCacheSize = 0;
    staticLayerBounds_ = {};
    curve_cache_dirty_ = true;
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// 控制面板参数
// ----------------------------------------------------------
void SpectrumModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::toggle ("peakHold", "Peak hold",
        "Keep a slowly falling line at the highest level each frequency has reached.").in ("Display"));
    out.push_back (ParamDef::toggle ("sharp", "Sharpening",
        "Reassign spectral energy to its true frequency so notes and harmonics collapse "
        "into crisp lines instead of wide humps.").in ("Display"));
    out.push_back (ParamDef::slider ("slope", "Slope", 0.0, 12.0, 0.5, " dB/oct",
        "Tilts the curve up towards the highs, 1 kHz is the pivot. 4.5 dB/oct (default) makes "
        "pink noise look flat; 0 turns the tilt off and shows the raw level.")
        .in ("Display"));
    out.push_back (ParamDef::choice ("curve", "Curve",
        { "Smooth", "Semi", "Raw" },
        "Smooth: 256 columns with blur - calm. Semi: one point per FFT bin with a spline. "
        "Raw: one point per bin joined by straight lines - maximum spike detail.").in ("Display"));
    out.push_back (ParamDef::choice ("channel", "Channels",
        { "Mid", "Mid + Side", "Left + Right" },
        "Mid: (L+R)/2, what a mono speaker hears. Mid + Side: solid mid, faint side (L-R)/2. "
        "Left + Right: solid left, faint right.").in ("Display"));
    out.push_back (ParamDef::choice ("fftScale", "FFT scale",
        { "Log", "Mel", "Linear" },
        "How frequency is laid out left to right. Log: every octave is equally wide. "
        "Mel: follows pitch perception. Linear: equal Hz per pixel, so highs get most of the room.")
        .in ("Axis"));
}

double SpectrumModule::getParamValue (const juce::String& id) const
{
    if (id == "peakHold") return peakHoldEnabled ? 1.0 : 0.0;
    if (id == "sharp")    return sharpEnabled ? 1.0 : 0.0;
    if (id == "slope")    return (double) slopeDbPerOct;
    if (id == "curve")    return (double) (int) curveMode;
    if (id == "channel")  return (double) (int) channelMode;
    if (id == "fftScale") return (double) (int) fftScale;
    return 0.0;
}

void SpectrumModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "peakHold") setPeakHoldEnabled (v >= 0.5);
    else if (id == "sharp")    setSharpEnabled (v >= 0.5);
    else if (id == "slope")    setSlopeDbPerOct ((float) v);
    else if (id == "curve")    setCurveMode ((CurveMode) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "channel")  setChannelMode ((ChannelMode) juce::jlimit (0, 2, juce::roundToInt (v)));
    else if (id == "fftScale") setFftScale (y2k::fscale::fromIndex (juce::roundToInt (v)));
}

void SpectrumModule::setSharpEnabled(bool b)
{
    if (sharpEnabled == b) return;
    sharpEnabled = b;

    // 关闭时清空谱线；开启时从下一帧开始重新追踪（避免陈旧线残留）
    if (! sharpEnabled)
        sharpLines.clear();

    buildGlSnapshot();
    repaint();
}

void SpectrumModule::setSlopeDbPerOct (float v)
{
    v = juce::jlimit (0.0f, 12.0f, v);
    if (std::abs (v - slopeDbPerOct) < 0.01f)
        return;

    slopeDbPerOct = v;

    // 补偿量变了：斜率缓存、平滑状态、峰值、谱线全部作废，从下一帧按新补偿重算
    //   （不清的话旧值是按旧斜率算出来的，会慢慢"漂"到新位置）
    slopeCacheSize = 0;
    smoothedDb.clear();
    secondaryDb.clear();
    peakDb.clear();
    sharpLines.clear();
    curve_cache_dirty_ = true;

    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// Sharper 模式（重分配谱线）
// ----------------------------------------------------------
float SpectrumModule::sharpSlopeDb (float freqHz) const noexcept
{
    // 与曲线 Slope 补偿同款（基准 1 kHz，量值来自参数），按精确频率计算
    return slopeDbPerOct * std::log2 (juce::jmax (20.0f, freqHz) / 1000.0f);
}

void SpectrumModule::updateSharpLines (const std::vector<AnalyserHub::ReassignedPeak>& peaks,
                                       float frameDtSec,
                                       double sampleRate)
{
    juce::ignoreUnused (sampleRate);
    using namespace y2k::pacing;

    // 平滑系数（按真实 dt 换算，帧率无关；120Hz 标定）
    const float aPos   = alphaForDt (0.45f, frameDtSec);   // 位置 EMA（线频率本身极稳，跟踪要跟得上换音）
    const float aMagUp = alphaForDt (0.60f, frameDtSec);   // 幅度上升快（攻击）
    // 子弹时间：幅度回落（release）做 1/50 时间膨胀，与主曲线行为一致
    const float aMagDn = alphaForDt (0.14f, bulletTimeActive
                                            ? frameDtSec / bulletTimeSlowdown
                                            : frameDtSec);   // 幅度回落慢（避免闪烁）
    const float aAlpha = alphaForDt (0.40f, frameDtSec);   // 淡入

    // 帧间匹配阈值：1/8 八度内视为同一根谱线（> 半音间距 0.083 八度，能区分相邻半音）
    constexpr float kMatchOctaves = 0.125f;
    // 反馈修订：新线创建门槛拉高 —— 弱峰（<-64dB）不进入显示，只参与已有线匹配
    constexpr float kNewLineMinDb = -64.0f;

    std::vector<bool> matched (sharpLines.size(), false);

    for (const auto& pk : peaks)
    {
        // 重分配谱线与主曲线用同一套 Slope 补偿（量值取自参数）
        const float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, pk.mag))
                       + sharpSlopeDb ((float) pk.freqHz);

        // 在未匹配的旧线中找频率最近邻
        int best = -1;
        float bestDist = kMatchOctaves;
        for (size_t i = 0; i < sharpLines.size(); ++i)
        {
            if (matched[i]) continue;
            const float d = std::abs (std::log2 (
                (float) (pk.freqHz / juce::jmax (1.0, sharpLines[i].freqHz))));
            if (d < bestDist)
            {
                bestDist = d;
                best = (int) i;
            }
        }

        if (best >= 0)
        {
            SharpLine& l = sharpLines[(size_t) best];
            matched[(size_t) best] = true;
            l.freqHz += aPos * (pk.freqHz - l.freqHz);
            const float aMag = (db > l.magDb) ? aMagUp : aMagDn;
            l.magDb += aMag * (db - l.magDb);
            l.alpha += aAlpha * (1.0f - l.alpha);
        }
        else if (sharpLines.size() < 64 && db >= kNewLineMinDb)
        {
            // 新谱线：淡入（alpha 从 0 爬升，避免"啪"地出现）
            SharpLine l;
            l.freqHz = pk.freqHz;
            l.magDb  = db;
            l.alpha  = 0.0f;
            sharpLines.push_back (l);
            matched.push_back (true);
        }
    }

    // 未匹配的谱线快速渐隐（反馈修订：0.45s → 0.12s 时间常数，
    //   音停止后竖线约 0.5s 内消失，不再残留大片幽灵细线）。
    const float fade = std::exp (-juce::jmax (0.0f, frameDtSec) / 0.12f);
    for (size_t i = 0; i < sharpLines.size();)
    {
        if (matched[i])
        {
            ++i;
            continue;
        }

        SharpLine& l = sharpLines[i];
        l.alpha *= fade;
        if (l.alpha <= 0.015f)
            sharpLines.erase (sharpLines.begin() + (std::ptrdiff_t) i);
        else
            ++i;
    }
}

void SpectrumModule::setPeakHoldEnabled(bool b)
{
    if (peakHoldEnabled == b) return;
    peakHoldEnabled = b;
    // 关闭时清空峰值，再次开启从当前值重新开始
    if (! peakHoldEnabled)
        std::fill(peakDb.begin(), peakDb.end(), minDb);
    buildGlSnapshot();
    repaint();
}

// ----------------------------------------------------------
// 频率/dB 坐标换算
// ----------------------------------------------------------
float SpectrumModule::freqToX(float freqHz, juce::Rectangle<int> canvas) const
{
    return (float) canvas.getX() + unitOfHz(freqHz) * (float) canvas.getWidth();
}

float SpectrumModule::xToFreq(float x, juce::Rectangle<int> canvas) const
{
    const float t = (x - (float) canvas.getX()) / juce::jmax(1.0f, (float) canvas.getWidth());
    return (float) y2k::fscale::fromUnit (fftScale, (double) juce::jlimit(0.0f, 1.0f, t),
                                          (double) minFreqHz, (double) maxFreqHz);
}

// 纵向网格频率：Log 沿用旧的 decade + 2/5 次线；Mel / Linear 换成各自刻度下好读的位置
void SpectrumModule::gridFreqs (std::vector<float>& major, std::vector<float>& minor) const
{
    switch (fftScale)
    {
        case y2k::fscale::Scale::mel:
            major = { 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
            minor = { 100.0f, 200.0f, 300.0f, 700.0f, 1500.0f, 3000.0f, 7000.0f, 15000.0f };
            break;
        case y2k::fscale::Scale::linear:
            major = { 5000.0f, 10000.0f, 15000.0f };
            minor = { 1000.0f, 2000.0f, 3000.0f, 4000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f,
                      11000.0f, 12000.0f, 13000.0f, 14000.0f, 16000.0f, 17000.0f, 18000.0f, 19000.0f };
            break;
        case y2k::fscale::Scale::log:
        default:
            major = { 100.0f, 1000.0f, 10000.0f };
            minor = { 30.0f, 50.0f, 200.0f, 300.0f, 500.0f, 2000.0f, 3000.0f, 5000.0f, 15000.0f };
            break;
    }
}

float SpectrumModule::dbToY(float db, juce::Rectangle<int> canvas) const
{
    const float t = (db - minDb) / (maxDb - minDb);
    const float tc = juce::jlimit(0.0f, 1.0f, t);
    return (float) canvas.getBottom() - tc * (float) canvas.getHeight();
}

// ----------------------------------------------------------
// Toybox：拨弦与音名 + dB 读数（E.4.1）
// ----------------------------------------------------------
float SpectrumModule::toyReadDbAtX (float x) const
{
    const auto canvas = getToyCanvasBounds();
    const int N = (int) smoothedDb.size();
    if (N <= 0 || canvas.getWidth() <= 0)
        return -144.0f;

    // 逐 bin 模式（Semi/Raw）：按频率找最近的点；smooth 模式：等距列
    const bool freqAxis = ((int) smoothedHz.size() == N);
    int col = 0;
    float slopeDb = 0.0f;

    if (freqAxis)
    {
        const float freq = xToFreq (x, canvas);
        float best = std::numeric_limits<float>::max();
        for (int i = 0; i < N; ++i)
        {
            const float d = std::abs (smoothedHz[(size_t) i] - freq);
            if (d < best) { best = d; col = i; }
        }
        slopeDb = slopeDbPerOct * std::log2 (juce::jmax (20.0f, smoothedHz[(size_t) col]) / 1000.0f);
    }
    else
    {
        col = juce::jlimit (0, N - 1,
            juce::roundToInt ((x - (float) canvas.getX())
                              / (float) canvas.getWidth() * (float) (N - 1)));
        if (col < (int) slopeOffsetDb.size())
            slopeDb = slopeOffsetDb[(size_t) col];
    }

    // 读数要扣掉 slope 补偿才是真实 dBFS
    return smoothedDb[(size_t) col] - slopeDb;
}

// ----------------------------------------------------------
// 残影着色：基础色 → 残影色的交叉混合（GL / CPU 两条路径共用）
// ----------------------------------------------------------
juce::Colour SpectrumModule::ghostTint (juce::Colour base, juce::Colour ghost) const
{
    if (ghostMix <= 0.0f)
        return base;
    return base.interpolatedWith (ghost, juce::jlimit (0.0f, 1.0f, ghostMix));
}

bool SpectrumModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto canvas = getToyCanvasBounds();
    if (! canvas.contains (panelPos))
        return false;
    out.kind = 1;
    out.rect = canvas.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void SpectrumModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const float freq = toyXToFreq (hit.pos.x);
    const float db   = toyReadDbAtX (hit.pos.x);
    const juce::String note = toyFreqToNoteName (freq);

    // 残影不再以"冻结曲线副本"的形式打印：按下时曲线本体直接切换为残影色
    //   （见 mouseDown / ghostMix），松手后渐隐恢复原配色。
    ctx.addModuleActor (makeSonarRingActor (hit.pos));
    if (note.isNotEmpty())
    {
        juce::String text = note;
        if (db > -144.0f)
            text << " " << juce::String (db, 1) << "dB";
        ctx.bubble (hit.pos.translated (0.0f, -24.0f), text, ToyRole::Reward);
    }
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> SpectrumModule::getCanvasBounds(juce::Rectangle<int> content) const
{
    // v3.1：顶部工具栏取消（参数搬进控制面板），整块内容区都是画布。
    //   左侧留 32 给 dB 标签，底部留 16 给 Hz 标签
    return content.withTrimmedTop(4)
                  .withTrimmedLeft(32)
                  .withTrimmedBottom(16)
                  .withTrimmedRight(6);
}

void SpectrumModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    // v2.9.5：尺寸守卫（防 layoutContent 每帧触发导致的交互卡顿）
    if (glCanvasSizeChanged (getGlCanvasBounds (contentBounds)))
        buildGlSnapshot();
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调（从 Hub 拿已经 "双路合并" 的幅度）
// ----------------------------------------------------------
void SpectrumModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace()) return;
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    // v2.9：取消 "每 2 tick 才 repaint" 的分频与 16ms×DPI 节流（两者叠加后
    //   本模块实际只有 ~30fps）。帧分发已与 vblank 同步并受 FPS 上限约束，
    //   每个分发帧都重绘才能跑满 120fps；曲线本身走离屏缓存，成本可控。

    // v2.9.6 单路 FFT：直接读主路（用户所选 size，75% overlap）的对数采样结果，
    // 分辨率与刷新率完全由 FFT size 档位决定。UI 侧零额外重采样，
    // 直接拿到"每列一个线性幅度"。
    //
    // 关于 frame：
    //   ·frame.spectrumMag 仍然保留（其他模块可能用），本模块不再使用；
    //   · blended 接口走 Hub 内部锁，成本极低（N 次查表）。
    const float fHi = juce::jmin (maxFreqHz, (float) (hub.getSampleRate() * 0.5));

    // 列数与采样方式（三档）：
    //   smooth：256 列 + 频段保峰聚合（最平滑、成本最低）
    //   semi  ：512 列 + 频段保峰聚合（更多窄峰，仍走样条）
    //   semi / raw：逐 FFT bin 取数（bins 模式）
    //   低频列宽（~2.7%×f，20Hz 处 ≈0.54Hz）远小于 bin 宽（48k/2048 ≈23.4Hz），
    //   若仍按"列→bin"映射，几十列会重复同一个 bin 的值 → 正是那些水平小台阶。
    //   bins 模式下每个 bin 只出一个点（放在该 bin 的中心频率上）再连线，
    //   低频因此自然稀疏、接近 smoot 的观感；高频 bin 密集 → 针状纹理。
    const bool binMode = (curveMode != CurveMode::smooth);

    if (binMode)
    {
        // 通道模式取数（主/次两路同一帧抓取，轴与分辨率完全一致）：
        //   MID       —— 主 = mid；MID+SIDE —— 主 = mid，次 = side
        //   LEFT+RIGHT—— 主 = L，次 = R
        double hz = 0.0;
        switch (channelMode)
        {
            case ChannelMode::mid:
                hub.copySpectrumBins (rawBins, AnalyserHub::SpectrumSource::mid, hz);
                break;

            case ChannelMode::midSide:
                hub.copySpectrumBins (rawBins,     AnalyserHub::SpectrumSource::mid,  hz);
                hub.copySpectrumBins (rawBinsSide, AnalyserHub::SpectrumSource::side, hz);
                break;

            case ChannelMode::leftRight:
                hub.copySpectrumBins (rawBins,     AnalyserHub::SpectrumSource::left,  hz);
                hub.copySpectrumBins (rawBinsSide, AnalyserHub::SpectrumSource::right, hz);
                break;
        }
        binHz = hz;
    }
    else
    {
        // 平滑模式：256 列对数轴 + 频段保峰聚合（现状）
        constexpr int N = 256;
        switch (channelMode)
        {
            case ChannelMode::mid:
                hub.getSpectrumMagnitudesBlended (rawMags, N, minFreqHz, fHi, false, fftScale);
                break;

            case ChannelMode::midSide:
                hub.getSpectrumMagnitudesBlended (rawMags, N, minFreqHz, fHi, false, fftScale);
                hub.getSpectrumMagnitudesBlendedSide (rawMagsSide, N, minFreqHz, fHi, false, fftScale);
                break;

            case ChannelMode::leftRight:
                hub.getSpectrumMagnitudesBlendedLR (rawMags, rawMagsSide, N, minFreqHz, fHi, false, fftScale);
                break;
        }
        binHz = 0.0;
    }

    // Sharper：重分配谱峰 → 谱线追踪（乐音收束成竖线）
    //   反馈修订：上限收敛到 28 根（基频 + 主要谐波 + 少数背景音足够），
    //   避免弱成分把画面铺满细线。
    if (sharpEnabled)
    {
        sharpPeaksScratch.clear();
        hub.getSpectrumReassignedPeaks (sharpPeaksScratch,
                                        minFreqHz,
                                        fHi,
                                        28);
        updateSharpLines (sharpPeaksScratch, frame.dtSec, hub.getSampleRate());
    }

    // v2.9：高精度帧间隔（旧实现 Time::getCurrentTime 整数毫秒，120Hz 下抖动）
    frameDtSec = frame.dtSec;
    const float deltaMs = frame.dtSec * 1000.0f;
    lastTickTime = juce::Time::getCurrentTime();

    rebuildDisplay();

    // 峰值下降更新（持续下落，无 hold / 无阈值）
    //   · 任一列信号超过当前 peak → 顶上去（刷新该列峰值）；
    //   · 否则该列以 peakFallRatePerSec 持续下降，直到 minDb。
    //   不再有"保持"阶段：无论信号强度如何，peak 线始终处于缓慢下落的
    //   动态——信号顶上来才抬升，否则一直回落，形成贴着信号、带下落惯性的虚线。
    if (peakHoldEnabled && ! peakDb.empty())
    {
        // 子弹时间：峰值下落速率同步放慢 50 倍（顶上去不受影响，只慢回落）
        const float fallRatePerSec = bulletTimeActive
                                       ? peakFallRatePerSec / bulletTimeSlowdown
                                       : peakFallRatePerSec;
        const float fallDelta = fallRatePerSec * (deltaMs / 1000.0f);

        for (size_t i = 0; i < peakDb.size(); ++i)
        {
            if (smoothedDb[i] > peakDb[i])
                peakDb[i] = smoothedDb[i];                                  // 信号超过峰值 → 顶上去
            else
                peakDb[i] = juce::jmax (minDb, peakDb[i] - fallDelta);      // 否则持续下落
        }
    }

    // 残影色消散：按住时保持满值（ghostMix=1），松手后按 ghostFadeSec 线性降到 0，
    //   曲线随之从残影色渐变回正常配色。放在 buildGlSnapshot 之前，
    //   本帧快照即用新的混合比例。
    if (! bulletTimeActive && ghostMix > 0.0f)
        ghostMix = juce::jmax (0.0f, ghostMix - frame.dtSec / ghostFadeSec);

    curve_cache_dirty_ = true;
    // v2.9.4：同步生成 GL 直渲染快照
    buildGlSnapshot();
    repaintFrameContent();
}

// ----------------------------------------------------------
// processSeries —— 一路 N 列线性幅度 → N 列 dBFS 显示值
//   1) 线性 → dB；
//   2) Slope 补偿（dB/oct 可调，默认 4.5，0 = 关闭）；
//   3) 上升快 / 下降慢 EMA（子弹时间只拖慢下降沿）；
//   4) 圆滑模式：末端轻量 3 列模糊磨圆拐点；
//      Semi / Raw 模式跳过这一步 —— 该卷积会把孤立尖峰
//      按 [1,2,1]/4 削掉一半以上，是"高频尖刺被磨没"的来源之一。
// ----------------------------------------------------------
void SpectrumModule::processSeries (const juce::Array<float>& mags,
                                    std::vector<float>& state,
                                    std::vector<float>& scratch)
{
    const int N = mags.size();
    if (N <= 1)
        return;

    if ((int) state.size() != N)
        state.assign (N, minDb);
    if ((int) scratch.size() != N)
        scratch.assign (N, minDb);

    // v2.9：平滑系数按 dt 换算（120Hz 标定 = 旧版每 tick 系数）
    //   子弹时间：下降沿（release）的 dt 做 1/50 时间膨胀 → 回落慢 50 倍；
    //   上升沿（attack）保持原速，频谱仍能实时跟上信号。
    const float releaseDtSec = bulletTimeActive
                                   ? frameDtSec / bulletTimeSlowdown
                                   : frameDtSec;
    const float alphaUp   = y2k::pacing::alphaForDt (0.55f, frameDtSec);
    const float alphaDown = y2k::pacing::alphaForDt (0.12f, releaseDtSec);

    for (int col = 0; col < N; ++col)
    {
        const float mag = std::abs (mags.getUnchecked (col));
        float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));

        // Slope 补偿：dB/oct 可调（默认 4.5，基准 1 kHz；0 = 关闭）
        if ((int) slopeOffsetDb.size() == N)
            db += slopeOffsetDb[(size_t) col];

        // 平滑：上升快，下降慢
        const float prev  = state[(size_t) col];
        const float alpha = (db > prev) ? alphaUp : alphaDown;
        float sm = prev + alpha * (db - prev);
        sm = juce::jlimit (minDb, maxDb + 12.0f, sm);
        state[(size_t) col] = sm;
    }

    // 列间低通：只在 smooth 模式做（semi / raw 跳过，保住窄峰高度）
    if (! useColumnBlur() || N < 3)
        return;

    scratch[0]     = state[0];
    scratch[N - 1] = state[(size_t) (N - 1)];
    for (int i = 1; i < N - 1; ++i)
    {
        scratch[(size_t) i] = 0.25f * state[(size_t)(i - 1)]
                            + 0.50f * state[(size_t) i]
                            + 0.25f * state[(size_t)(i + 1)];
    }
    state.swap (scratch);
}

// ----------------------------------------------------------
// processBinSeries —— 逐 bin 幅度 → 显示点（Semi / Raw）
//   · 只取 [20Hz, min(20k, sr/2)] 内的 bin；
//   · 合并规则按**实际像素间距**动态决定：从当前 bin 开始向后吃进邻居，
//     直到它的 x 距本组起点 ≥ minGapPx（= 线宽 + 一点余量）为止。
//     于是：
//       - 模块越宽 → 每 bin 占的像素越多 → 合并越少、细节越多；
//       - 高频 bin 天然密集 → 自动合并成 1 点/线宽，消除"点距 < 线宽"
//         造成的噪点/锯齿；
//       - 低频 bin 天然稀疏 → 基本不合并，每个 bin 独立成点。
//     组内取**最大**幅度（保峰），频率取组中心，尖峰不会被平均掉。
//   · 每个点带自己的中心频率（freqs），绘制时按对数轴定位 —— 不再出现
//     "多列共用同一个 bin 值"的水平台阶。
// ----------------------------------------------------------
void SpectrumModule::processBinSeries (const juce::Array<float>& mags, double hzPerBin,
                                       std::vector<float>& state,
                                       std::vector<float>& scratch,
                                       std::vector<float>& freqs)
{
    juce::ignoreUnused (scratch);
    freqs.clear();

    const int total = mags.size();
    if (total < 2 || hzPerBin <= 0.0)
    {
        state.clear();
        return;
    }

    const double nyq  = hub.getSampleRate() * 0.5;
    const double fLoD = (double) minFreqHz;
    const double fHiD = juce::jmin ((double) maxFreqHz, nyq);
    if (fHiD <= fLoD)
    {
        state.clear();
        return;
    }

    const int firstBin = juce::jmax (1, (int) std::ceil  (fLoD / hzPerBin));
    const int lastBin  = juce::jmin (total - 1, (int) std::floor (fHiD / hzPerBin));
    if (lastBin <= firstBin)
    {
        state.clear();
        return;
    }

    // ---- 画布像素映射（与绘制时 inner = canvas.reduced(2) 一致）----
    const auto  canvas  = getCanvasBounds (getContentBounds());
    const float innerX0 = (float) canvas.getX() + 2.0f;
    const float innerW  = (float) juce::jmax (1, canvas.getWidth() - 4);

    auto xOfHz = [&] (double f) noexcept
    {
        return innerX0 + unitOfHz ((float) f) * innerW;
    };

    // 相邻输出点的最小像素间距：线带全宽 + 0.15px 余量。
    //   线宽是固定值（不随模块宽度变化），所以这个阈值也只随"曲线模式 × 是否
    //   有填充"变化 —— 无填充时线更粗（见绘制分支），阈值同步放大。
    const bool  noFill      = (channelMode != ChannelMode::mid);
    const float lineWidthPx = (curveMode == CurveMode::raw)
                                ? (noFill ? 1.4f : 1.1f)
                                : (noFill ? 1.9f : 1.4f);
    const float minGapPx    = lineWidthPx + 0.15f;

    // 每个 bin 的 x 位置只算一次（两次遍历共用；16k FFT 时也避免重复 log10）
    const int count = lastBin - firstBin + 1;
    binXScratch.resize ((size_t) count);
    for (int i = 0; i < count; ++i)
        binXScratch[(size_t) i] = xOfHz ((double) (firstBin + i) * hzPerBin);
    const std::vector<float>& binX = binXScratch;

    // 从组起点 i 向后吃到"与起点间距 ≥ minGapPx"为止，返回组末下标
    auto groupEnd = [&] (int i) noexcept
    {
        const float xStart = binX[(size_t) i];
        int j = i;
        while (j + 1 < count && (binX[(size_t) (j + 1)] - xStart) < minGapPx)
            ++j;
        return j;
    };

    // 第一遍：数出合并后的点数（组边界只依赖宽度/FFT，不依赖音频）
    int pts = 0;
    for (int i = 0; i < count; i = groupEnd (i) + 1)
        ++pts;

    // 最低可用 bin 的频率通常 > 20Hz（例如 1024FFT/48k 时是 46.9Hz）：
    //   在最左侧补一个"延伸到 20Hz"的点（值取第一个分组），避免左下角空白。
    const bool extendLeft = (firstBin > 1);
    const int  off        = extendLeft ? 1 : 0;
    const int  totalPts   = pts + off;

    if ((int) state.size() != totalPts)
        state.assign ((size_t) totalPts, minDb);
    freqs.resize ((size_t) totalPts);

    // 与 processSeries 完全相同的攻/放时间常数（子弹时间只拖慢释放）
    const float releaseDt = bulletTimeActive ? frameDtSec / bulletTimeSlowdown : frameDtSec;
    const float alphaUp   = y2k::pacing::alphaForDt (0.55f, frameDtSec);
    const float alphaDown = y2k::pacing::alphaForDt (0.12f, releaseDt);

    auto feedPoint = [&] (int idx, float mag, float fCenter)
    {
        freqs[(size_t) idx] = fCenter;

        float db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-7f, mag));
        db += slopeDbPerOct * std::log2 (juce::jmax (20.0f, fCenter) / 1000.0f);   // slope

        const float prev  = state[(size_t) idx];
        const float alpha = (db > prev) ? alphaUp : alphaDown;
        state[(size_t) idx] = juce::jlimit (minDb, maxDb + 12.0f,
                                            prev + alpha * (db - prev));
    };

    // 第二遍：按同样的分组规则填值
    int idx = 0;
    for (int i = 0; i < count; )
    {
        const int j = groupEnd (i);
        const int b0 = firstBin + i;
        const int b1 = firstBin + j;

        float mag = 0.0f;
        for (int k = b0; k <= b1; ++k)
            mag = juce::jmax (mag, std::abs (mags.getUnchecked (k)));

        // 绘制频率取**组起点**的 bin 频率：
        //   若取组中心，相邻输出点的中心间距可能只有 minGap 的一半（组本身
        //   有宽度），又会掉回"点距 < 线宽"的重叠噪声；取起点则保证任意两个
        //   输出点的 x 间距 ≥ minGapPx。偏移不超过一个组宽（<1px，肉眼不可见）。
        const float fAnchor = (float) ((double) b0 * hzPerBin);

        // 左侧延伸点：与第一组同值，频率放到 20Hz
        if (extendLeft && idx == 0)
            feedPoint (idx++, mag, (float) minFreqHz);

        feedPoint (idx++, mag, fAnchor);
        i = j + 1;
    }
}

// ----------------------------------------------------------
// rebuildDisplay —— 主曲线 + 次曲线（side / R）一起刷新
// ----------------------------------------------------------
void SpectrumModule::rebuildDisplay()
{
    if (curveMode == CurveMode::smooth)
    {
        const int N = rawMags.size();
        if (N <= 1)
            return;

        ensureDisplayCache (N);
        smoothedHz.clear();
        secondaryHz.clear();

        processSeries (rawMags, smoothedDb, blurredDb);

        // 次曲线：midSide → side；leftRight → R；mid → 不画
        if (channelMode != ChannelMode::mid && (int) rawMagsSide.size() == N)
            processSeries (rawMagsSide, secondaryDb, secondaryBlurDb);
        else
            secondaryDb.clear();
    }
    else
    {
        if (rawBins.size() < 2)
            return;

        processBinSeries (rawBins, binHz, smoothedDb, blurredDb, smoothedHz);

        if (channelMode != ChannelMode::mid && rawBinsSide.size() >= 2)
            processBinSeries (rawBinsSide, binHz, secondaryDb, secondaryBlurDb, secondaryHz);
        else
        {
            secondaryDb.clear();
            secondaryHz.clear();
        }
    }

    // 峰值数组跟随主曲线尺寸（尺寸变化时重置，避免旧尺度残留）
    if ((int) peakDb.size() != (int) smoothedDb.size())
        peakDb.assign (smoothedDb.size(), minDb);
}

void SpectrumModule::ensureDisplayCache (int numPoints)
{
    numPoints = juce::jmax (2, numPoints);

    const double sampleRate = hub.getSampleRate();
    if (slopeCacheSize == numPoints
        && slopeCacheScale == fftScale
        && std::abs (slopeCacheSampleRate - sampleRate) <= 0.5
        && std::abs (slopeCacheDbPerOct - slopeDbPerOct) < 0.01f
        && (int) slopeOffsetDb.size() == numPoints)
    {
        return;
    }

    slopeOffsetDb.resize ((size_t) numPoints);

    const double fMin   = (double) minFreqHz;
    const double fMax   = juce::jmax (fMin, juce::jmin ((double) maxFreqHz, sampleRate * 0.5));

    for (int col = 0; col < numPoints; ++col)
    {
        const double t = (double) col / (double) juce::jmax (1, numPoints - 1);
        const double f = y2k::fscale::fromUnit (fftScale, t, fMin, fMax);
        const double octaves = std::log2 (juce::jmax (20.0, f) / 1000.0);
        slopeOffsetDb[(size_t) col] = (float) (slopeDbPerOct * octaves);
    }

    slopeCacheSize = numPoints;
    slopeCacheScale = fftScale;
    slopeCacheSampleRate = sampleRate;
    slopeCacheDbPerOct = slopeDbPerOct;
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
// ---- GlDirectModuleBase 钩子（v2.9.4 迁移）----
juce::Rectangle<int> SpectrumModule::getGlCanvasBounds (juce::Rectangle<int> contentBounds)
{
    return getCanvasBounds (contentBounds);
}

void SpectrumModule::paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas)
{
    // 接管时只保留轴标签与悬停标尺（GL 内容之上）
    drawAxisLabels (g, canvas);
    if (hoverActive)
        drawHoverRuler (g, canvas);
}

void SpectrumModule::paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const int cw = contentBounds.getWidth();
    const int ch = contentBounds.getHeight();
    if (cw <= 0 || ch <= 0) return;

    // 1) 静态层：尺寸 / DPI / 主题配色变化时才重建，否则直接贴（GL 复用纹理）
    const float physScale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto  colourKey = computeThemeColourKey();
    if (! staticLayer_.isValid()
        || staticLayerBounds_ != contentBounds
        || std::abs (staticLayerScale_ - physScale) > 0.01f
        || staticLayerColourKey_ != colourKey)
    {
        renderStaticLayer (contentBounds, physScale);
        staticLayerColourKey_ = colourKey;
    }

    g.drawImageTransformed (staticLayer_,
                            juce::AffineTransform::scale (1.0f / staticLayerScale_)
                                .translated ((float) contentBounds.getX(), (float) contentBounds.getY()));

    // 2) 动态曲线：每帧直接画
    auto canvas = getCanvasBounds(contentBounds);
    if (canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (canvas.reduced (2));
        drawCurves(g, canvas);
    }
    curve_cache_dirty_ = false;

    // 鼠标悬停标尺
    if (hoverActive)
        drawHoverRuler(g, canvas);
}

// ----------------------------------------------------------
// buildGlSnapshot —— 频谱曲线 GL 顶点流（v2.9.4）
//   布局：[凹陷背景 + dB 横线 + 频率纵线]
//         [曲线填充（梯形条到画布底）]
//         [主曲线 glow strip][主曲线主线 strip]
//         [峰值保持虚线（3px 段 / 3px 空，与旧版 dash 一致）]
//   曲线为 1px 间距采样点直接折线（Catmull-Rom 平滑在 1px 间距下不可分辨）。
// ----------------------------------------------------------
void SpectrumModule::buildGlSnapshot()
{
    if (! wantsGlSnapshot())
        return;   // v2.9.5：浮动态跳过

    const auto canvas = getCanvasBounds (getContentBounds());
    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    const int N = (int) smoothedDb.size();
    if (N <= 1)
        return;

    auto snap = beginGlSnapshot();
    snap->renderToLogical = true;
    snap->logicalW = canvas.getWidth();
    snap->logicalH = canvas.getHeight();
    snap->canvasLocal = canvas;

    // ---- 背景（凹陷，框架 helper）----
    appendSunkenFrame (snap->vertices, canvas);

    auto inner = canvas.reduced (2);

    // ---- 网格（与 drawGrid 逐笔对应）----
    {
        // 横线（每 20dB，0dB 高亮）
        for (int db = (int) minDb; db <= (int) maxDb; db += 20)
        {
            const int y = (int) std::round (dbToY ((float) db, canvas));
            if (y < inner.getY() || y > inner.getBottom()) continue;
            const auto c = (db == 0) ? PinkXP::pink400.withAlpha (0.7f)
                                     : PinkXP::pink200.withAlpha (0.45f);
            appendLineSegment (snap->vertices,
                               (float) inner.getX(), (float) y,
                               (float) inner.getRight(), (float) y,
                               0.5f, c);
        }

        // 纵线（次 + 主；Log 为 2/5 次线 + decade，Mel / Linear 见 gridFreqs）
        std::vector<float> majorFreqs, minorFreqs;
        gridFreqs (majorFreqs, minorFreqs);
        const auto minorC = PinkXP::pink200.withAlpha (0.3f);
        const auto majorC = PinkXP::pink300.withAlpha (0.6f);
        for (float f : minorFreqs)
        {
            const int x = (int) std::round (freqToX (f, canvas));
            if (x > inner.getX() && x < inner.getRight())
                appendLineSegment (snap->vertices,
                                   (float) x, (float) inner.getY(),
                                   (float) x, (float) inner.getBottom(),
                                   0.5f, minorC);
        }
        for (float f : majorFreqs)
        {
            const int x = (int) std::round (freqToX (f, canvas));
            if (x > inner.getX() && x < inner.getRight())
                appendLineSegment (snap->vertices,
                                   (float) x, (float) inner.getY(),
                                   (float) x, (float) inner.getBottom(),
                                   0.5f, majorC);
        }
    }

    // ---- 曲线采样点（等距 x）→ Catmull-Rom 样条细分折线（v2.9.4 修正）----
    //   旧版用 Catmull-Rom（tension 0.5）→ 三次贝塞尔画曲线；直接折线化时
    //   256 采样点分布在 ~400px 画布（点距 1.6px），dB 台阶清晰可见（"小线段"）。
    //   这里复刻旧版控制点公式，把每段贝塞尔细分 4 段（0.4px 间隔），
    //   折线在亚像素尺度逼近曲线，视觉与旧版一致。
    const float x0   = (float) inner.getX();
    const float xLen = (float) inner.getWidth();
    const float invMax = 1.0f / (float) juce::jmax (1, N - 1);
    const float yTop = (float) inner.getY();
    const float yBot = (float) inner.getBottom();

    // 频率 → 画布内 x（按 fftScale）。Semi/Raw 的采样点在频率上**不等距**
    // （每个 FFT bin 一个点，低频稀疏、高频密集），必须按频率定位。
    auto xForFreq = [&] (float f) noexcept
    {
        return (float) inner.getX() + unitOfHz (f) * (float) inner.getWidth();
    };

    auto samplePts = [&] (const std::vector<float>& dbVals,
                          const std::vector<float>& freqs,
                          std::vector<float>& outX, std::vector<float>& outY,
                          bool smooth, int subdiv)
    {
        const int n = (int) dbVals.size();
        outX.clear();
        outY.clear();
        if (n < 2) return;

        const bool useFreqAxis = ((int) freqs.size() == n);

        std::vector<float> px ((size_t) n), py ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            px[(size_t) i] = useFreqAxis ? xForFreq (freqs[(size_t) i])
                                         : x0 + (float) i * invMax * xLen;
            py[(size_t) i] = dbToY (dbVals[(size_t) i], canvas);
        }

        // 折线模式（Smooth 关闭）：直接连采样点，不做任何插值/贴底夹取 ——
        //   高频窄峰与尖刺按原始 dB 值出角，便于精确读谱。
        if (! smooth)
        {
            outX = px;
            outY = py;
            return;
        }

        constexpr float floorPixelTol = 0.75f;
        const int kSubdiv = juce::jlimit (1, 8, subdiv);   // 每段贝塞尔细分段数

        auto atFloor = [&] (float y) noexcept { return (yBot - y) <= floorPixelTol; };

        outX.reserve ((size_t) (n * kSubdiv + 4));
        outY.reserve ((size_t) (n * kSubdiv + 4));
        outX.push_back (px[0]);
        outY.push_back (py[0]);

        for (int i = 0; i < n - 1; ++i)
        {
            const float p0x = (i == 0)     ? px[0]     : px[(size_t) i - 1];
            const float p0y = (i == 0)     ? py[0]     : py[(size_t) i - 1];
            const float p1x = px[(size_t) i];
            const float p1y = py[(size_t) i];
            const float p2x = px[(size_t) i + 1];
            const float p2y = py[(size_t) i + 1];
            const float p3x = (i + 2 >= n) ? px[(size_t) n - 1] : px[(size_t) i + 2];
            const float p3y = (i + 2 >= n) ? py[(size_t) n - 1] : py[(size_t) i + 2];

            // 两端贴底 → 直线贴底（与旧版一致）
            if (atFloor (p1y) && atFloor (p2y))
            {
                outX.push_back (p2x);
                outY.push_back (yBot);
                continue;
            }

            const float c1x = p1x + (p2x - p0x) / 6.0f;
            const float c2x = p2x - (p3x - p1x) / 6.0f;
            float c1y = p1y + (p2y - p0y) / 6.0f;
            float c2y = p2y - (p3y - p1y) / 6.0f;
            if (atFloor (p1y)) c1y = p1y;
            if (atFloor (p2y)) c2y = p2y;
            c1y = juce::jlimit (yTop, yBot, c1y);
            c2y = juce::jlimit (yTop, yBot, c2y);

            // 自适应细分：段的横向跨度不足 1px 时（高频 bin 极密）细分没有视觉
            //   收益，只增加顶点数 —— 直接取一段，省 2~4 倍顶点。
            const int segSubdiv = (std::abs (p2x - p1x) < 1.0f)
                                    ? 1 : kSubdiv;
            for (int s = 1; s <= segSubdiv; ++s)
            {
                const float t = (float) s / (float) segSubdiv;
                const float u = 1.0f - t;
                const float w0 = u * u * u;
                const float w1 = 3.0f * u * u * t;
                const float w2 = 3.0f * u * t * t;
                const float w3 = t * t * t;
                outX.push_back (w0 * p1x + w1 * c1x + w2 * c2x + w3 * p2x);
                outY.push_back (w0 * p1y + w1 * c1y + w2 * c2y + w3 * p2y);
            }
        }
    };

    std::vector<float> sx, sy, px, py, sx2, sy2;
    samplePts (smoothedDb, smoothedHz, sx, sy, useSpline(), splineSubdiv());
    const int M = (int) sx.size();

    // 折线模式（raw）专用：逐段独立法线，避免尖刺处"平均法线"把线带压成 0 宽
    //   （平均法线在 180° 折角处互相抵消 → 线在尖峰顶部断掉/忽然变细）。
    const bool rawMode = (curveMode == CurveMode::raw);

    // ---- 曲线折线带（可复用：主线 / 次曲线）----
    //   v2.9.4 修正：改用"连续折线带"（内部点法线取相邻两段法线的平均），
    //   相邻段共享端点顶点、无重叠无缝隙 —— 之前的逐段独立 quad 在端点
    //   重叠，端点亮度翻倍（"很多小亮点、亮度不一致"的根因）。
    auto appendCurveStrip = [&] (const std::vector<float>& xs,
                                 const std::vector<float>& ys,
                                 float halfW, juce::Colour c, bool perSegment)
    {
        const int n = (int) xs.size();
        if (n < 2)
            return;

        const float r = c.getFloatRed(), g = c.getFloatGreen(),
                    b = c.getFloatBlue(), a = c.getFloatAlpha();

        // ---- 逐段独立法线（折线模式）----
        //   每段用自己的垂直方向、宽度恒定；相邻段在端点重叠，但主线是
        //   不透明纯色，重叠不会出现亮度翻倍，反而保证尖峰处不掉线。
        if (perSegment)
        {
            snap->vertices.reserve (snap->vertices.size() + (size_t) (n - 1) * 36);
            for (int i = 0; i + 1 < n; ++i)
            {
                const float dx = xs[i + 1] - xs[i];
                const float dy = ys[i + 1] - ys[i];
                const float l  = std::sqrt (dx * dx + dy * dy);
                if (l < 1.0e-6f)
                    continue;
                const float nx = -dy / l * halfW;
                const float ny =  dx / l * halfW;
                const float q[36] = {
                    xs[i]     + nx, ys[i]     + ny, r, g, b, a,
                    xs[i + 1] + nx, ys[i + 1] + ny, r, g, b, a,
                    xs[i]     - nx, ys[i]     - ny, r, g, b, a,
                    xs[i + 1] + nx, ys[i + 1] + ny, r, g, b, a,
                    xs[i + 1] - nx, ys[i + 1] - ny, r, g, b, a,
                    xs[i]     - nx, ys[i]     - ny, r, g, b, a,
                };
                snap->vertices.insert (snap->vertices.end(), q, q + 36);
            }
            return;
        }

        // 预计算每个点的平均法线（单位向量）
        std::vector<float> nx ((size_t) n), ny ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            float ax = 0.0f, ay = 0.0f;
            if (i > 0)
            {
                const float dx = xs[i] - xs[i - 1];
                const float dy = ys[i] - ys[i - 1];
                const float l = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-6f) { ax += -dy / l; ay += dx / l; }
            }
            if (i + 1 < n)
            {
                const float dx = xs[i + 1] - xs[i];
                const float dy = ys[i + 1] - ys[i];
                const float l = std::sqrt (dx * dx + dy * dy);
                if (l > 1.0e-6f) { ax += -dy / l; ay += dx / l; }
            }
            const float l = std::sqrt (ax * ax + ay * ay);
            nx[(size_t) i] = (l > 1.0e-6f) ? ax / l : 0.0f;
            ny[(size_t) i] = (l > 1.0e-6f) ? ay / l : 0.0f;
        }

        snap->vertices.reserve (snap->vertices.size() + (size_t) (n - 1) * 36);
        for (int i = 0; i + 1 < n; ++i)
        {
            const float x0 = xs[i]     + nx[(size_t) i] * halfW;
            const float y0 = ys[i]     + ny[(size_t) i] * halfW;
            const float x1 = xs[i + 1] + nx[(size_t) i + 1] * halfW;
            const float y1 = ys[i + 1] + ny[(size_t) i + 1] * halfW;
            const float x2 = xs[i]     - nx[(size_t) i] * halfW;
            const float y2 = ys[i]     - ny[(size_t) i] * halfW;
            const float x3 = xs[i + 1] - nx[(size_t) i + 1] * halfW;
            const float y3 = ys[i + 1] - ny[(size_t) i + 1] * halfW;
            const float q[36] = {
                x0, y0, r, g, b, a,  x1, y1, r, g, b, a,  x2, y2, r, g, b, a,
                x1, y1, r, g, b, a,  x3, y3, r, g, b, a,  x2, y2, r, g, b, a,
            };
            snap->vertices.insert (snap->vertices.end(), q, q + 36);
        }
    };

    // ---- 1) 曲线填充（梯形条到画布底）----
    //   反馈修订：Sharper 模式下频谱曲线保持原样（主体），谱线仅叠加辅助
    //   残影态：填充 / 光晕 / 主线三层一起混向 PinkXP::sel（原残影 actor 同色）
    //   是否着色**只看通道模式**：mid 一律着色；MID+SIDE / LEFT+RIGHT 不着色
    //   （底色会盖住下方更细的次曲线）。与 Smooth/Semi/Raw 无关。
    if (channelMode == ChannelMode::mid && M > 1)
    {
        const auto fillC = ghostTint (PinkXP::chartRamp[3].withAlpha (0.25f),
                                      PinkXP::sel.withAlpha (0.22f));
        const float fr = fillC.getFloatRed(), fg = fillC.getFloatGreen(),
                    fb = fillC.getFloatBlue(), fa = fillC.getFloatAlpha();
        snap->vertices.reserve (snap->vertices.size() + (size_t) (M - 1) * 36);
        for (int i = 0; i + 1 < M; ++i)
        {
            const float q[36] = {
                sx[i],     sy[i],     fr, fg, fb, fa,
                sx[i + 1], sy[i + 1], fr, fg, fb, fa,
                sx[i],     yBot,      fr, fg, fb, fa,
                sx[i + 1], sy[i + 1], fr, fg, fb, fa,
                sx[i + 1], yBot,      fr, fg, fb, fa,
                sx[i],     yBot,      fr, fg, fb, fa,
            };
            snap->vertices.insert (snap->vertices.end(), q, q + 36);
        }
    }

    // ---- 1.5) 次曲线（MID+SIDE 的 side / LEFT+RIGHT 的 R）----
    //   只画线不填充。没有主曲线的填充托底时，细/浅的线在浅色画布上几乎
    //   看不见 —— 这里改用更饱和的 chartRamp[5] 作核心色、提高不透明度并
    //   加宽线带，同时保留一条光晕把它从背景里"抬"出来。
    if (hasSecondaryCurve() && (int) secondaryDb.size() >= 2)
    {
        samplePts (secondaryDb, secondaryHz, sx2, sy2, useSpline(), splineSubdiv());
        if ((int) sx2.size() > 1)
        {
            // raw 模式点距小，光晕会连成宽带：只保留加粗主线
            if (! rawMode)
                appendCurveStrip (sx2, sy2, 1.4f,
                                  ghostTint (PinkXP::chartRamp[4].withAlpha (0.34f),
                                             PinkXP::sel.withAlpha (0.34f)), false);
            // 核心色朝 ink 混 22%：自定义浅色主题下 chartRamp 可能很淡，
            //   混到前景色方向可保证在明/暗画布上都有对比度（色相仍保留）。
            const auto secondaryCore =
                PinkXP::interpolateOkLab (PinkXP::chartRamp[5], PinkXP::ink, 0.22f).withAlpha (0.88f);
            appendCurveStrip (sx2, sy2, rawMode ? 0.7f : 0.85f,
                              ghostTint (secondaryCore,
                                         PinkXP::sel.withAlpha (0.85f)), rawMode);
        }
    }

    // ---- 2) 主曲线（glow + 主线）----
    //   光晕加粗放亮 + 主线换成残影纯色，按住时整条曲线明显"发光"
    //   无填充模式（M+S / L+R）：主线与光晕一并加粗提亮，避免"线太细没存在感"；
    //   raw 模式：点距极小，光晕会把相邻针连成一片 —— 只加粗主线。
    if (M > 1)
    {
        const bool noFill = (channelMode != ChannelMode::mid);
        if (! rawMode)
        {
            const float glowHalf  = noFill ? 1.8f
                                           : (curveMode == CurveMode::semi ? 1.05f : 1.5f);
            const float glowAlpha = noFill ? 0.42f
                                           : (curveMode == CurveMode::semi ? 0.26f : 0.35f);
            appendCurveStrip (sx, sy, glowHalf,
                              ghostTint (PinkXP::chartRamp[5].withAlpha (glowAlpha),
                                         PinkXP::sel.withAlpha (0.60f)), false);
        }
        const float coreHalf = rawMode ? (noFill ? 0.7f : 0.55f)
                                       : (noFill ? 0.95f : 0.7f);
        appendCurveStrip (sx, sy, coreHalf,
                          ghostTint (PinkXP::chartRamp[6], PinkXP::sel), rawMode);
    }

    // ---- 3) 峰值保持虚线（弧长 3px 段 / 3px 空，与旧版 createDashedStroke 一致）----
    //   v2.9.4 修正：dash 边界精确插值 —— 峰值"信号消失边缘"的相邻列落差
    //   巨大（单段弧长可达几十像素），旧逻辑"画完整段再计数"会把超长段
    //   整段画出（线被拉长）。现在沿路径以 3px 为预算推进，超长段在 3px
    //   处截断，每根虚线严格 ≤3px。
    if (peakHoldEnabled && (int) peakDb.size() == N)
    {
        // 峰值虚线跟随主曲线的插值模式（raw 下同样是折线）
        samplePts (peakDb, smoothedHz, px, py, useSpline(), splineSubdiv());
        const int Mp = (int) px.size();
        if (Mp > 1)
        {
            // peak 虚线用 ink：light 下深灰、dark 下浅白，任何画布上都清晰
            const auto dashC = PinkXP::ink.withAlpha (0.75f);
            float curX = px[0], curY = py[0];
            float arcPos = 0.0f;      // 当前 dash 周期内已走的弧长
            bool drawing = true;
            for (int i = 0; i + 1 < Mp; ++i)
            {
                const float dx = px[i + 1] - curX;
                const float dy = py[i + 1] - curY;
                const float segLen = std::sqrt (dx * dx + dy * dy);
                if (segLen < 1.0e-6f) continue;
                const float ux = dx / segLen, uy = dy / segLen;

                float travelled = 0.0f;
                while (travelled < segLen - 1.0e-6f)
                {
                    const float remain = 3.0f - arcPos;
                    const float step = std::min (remain, segLen - travelled);
                    const float nx = curX + ux * step;
                    const float ny = curY + uy * step;
                    if (drawing)
                        appendLineSegment (snap->vertices, curX, curY, nx, ny, 0.6f, dashC);
                    curX = nx;
                    curY = ny;
                    travelled += step;
                    arcPos += step;
                    if (arcPos >= 3.0f - 1.0e-6f) { arcPos = 0.0f; drawing = !drawing; }
                }
            }
        }
    }

    // ---- 4) Sharper：重分配谱线（全高竖线；峰值强度映射颜色）----
    if (sharpEnabled && ! sharpLines.empty())
    {
        const float yBotF = (float) inner.getBottom();
        for (const auto& l : sharpLines)
        {
            const float x = freqToX ((float) l.freqHz, canvas);
            if (x < (float) inner.getX() - 2.0f || x > (float) inner.getRight() + 2.0f)
                continue;

            const float a     = juce::jlimit (0.0f, 1.0f, l.alpha);
            const float strength = juce::jlimit (0.0f, 1.0f,
                                                  (l.magDb - minDb) / (maxDb - minDb));
            const auto lineColour = PinkXP::pink200.interpolatedWith (PinkXP::sel, strength);
            const float yTopF = (float) inner.getY();
            const float h = yBotF - yTopF;

            appendQuad (snap->vertices,
                        { x - 1.5f, yTopF, 3.0f, h },
                        lineColour.withAlpha (0.18f * a));
            appendQuad (snap->vertices,
                        { x - 0.6f, yTopF, 1.2f, h },
                        lineColour.withAlpha (0.85f * a));
        }
    }

    snap->segments.push_back ({ 0, (int) (snap->vertices.size() / 6), Primitive::Triangles });
    publishGlSnapshot (std::move (snap));
}

void SpectrumModule::mouseMove(const juce::MouseEvent& e)
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

void SpectrumModule::mouseDrag (const juce::MouseEvent& e)
{
    ModulePanel::mouseDrag (e);

    // 按住拖动期间 JUCE 只派发 mouseDrag（mouseMove 会被抑制），若不同步更新
    //   hoverPos，十字线与频率/dB 读数就会停在按下那一刻的位置不动。
    const auto canvas = getCanvasBounds (getContentBounds());
    const auto pos    = e.getPosition();
    const bool inside = canvas.contains (pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaint();
    }
}

// ----------------------------------------------------------
// 子弹时间：按住频谱画布 → release 回落放慢 50 倍 + 曲线转为残影色；
//   松手 → 恢复原速，残影色按 ghostFadeSec 渐隐
// ----------------------------------------------------------
void SpectrumModule::mouseDown (const juce::MouseEvent& e)
{
    ModulePanel::mouseDown (e);   // 保留基类行为（toFront / 右键菜单 / 边缘拖拽）

    // 只在"左键按在频谱画布内"时进入子弹时间：
    //   工具栏按钮 / 标题栏 / 边缘的按下交给基类逻辑（拖动、resize），
    //   不触发减速（JUCE 的按钮点击先于本组件的 mouseDown，互不干扰）。
    if (e.mods.isLeftButtonDown()
        && getCanvasBounds (getContentBounds()).contains (e.getPosition()))
    {
        bulletTimeActive = true;
        ghostMix = 1.0f;          // 曲线立即切换为残影色

        // 立刻重建快照并重绘，不等下一帧（按住瞬间就要看到变色）
        buildGlSnapshot();
        repaint();
    }
}

void SpectrumModule::mouseUp (const juce::MouseEvent& e)
{
    ModulePanel::mouseUp (e);

    // 无论松手位置在哪（按住后拖出画布再松开），都恢复正常速度。
    //   若按下的位置不在画布（未进入子弹时间），这里只是无害地写一次 false。
    //   残影色不在这里清零：由 onFrame 按 ghostFadeSec 逐帧消散。
    if (bulletTimeActive)
        bulletTimeActive = false;
}

void SpectrumModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint();
    }
}

void SpectrumModule::drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas)
{
    if (!hoverActive || !canvas.contains(hoverPos))
        return;

    const float freqHz = xToFreq((float) hoverPos.x, canvas);
    const float t = (float)(canvas.getBottom() - hoverPos.y)
                  / juce::jmax(1.0f, (float) canvas.getHeight());
    const float db = minDb + juce::jlimit(0.0f, 1.0f, t) * (maxDb - minDb);

    juce::String readout = PinkXP::formatFreqHz(freqHz)
                         + "  " + juce::String(db, 1) + " dBFS";

    // 按住（子弹时间）时补一个音名：曲线慢下来，读数才对得上这个频点的音高
    if (bulletTimeActive)
    {
        const juce::String note = PinkXP::noteNameForHz(freqHz);
        if (note.isNotEmpty())
            readout += "  " + note;
    }

    PinkXP::drawHoverRuler(g, canvas, hoverPos, readout);
}

// ----------------------------------------------------------
// renderStaticLayer —— 底衬 / 画布 / 网格 / 坐标轴标签渲染到离屏 Image（物理像素分辨率）
// ----------------------------------------------------------
juce::uint32 SpectrumModule::computeThemeColourKey() noexcept
{
    juce::uint32 k = 2166136261u;
    for (auto c : { PinkXP::btnFace, PinkXP::content, PinkXP::pink200, PinkXP::pink400, PinkXP::ink })
        k = (k ^ c.getARGB()) * 16777619u;
    return k;
}

void SpectrumModule::renderStaticLayer (juce::Rectangle<int> contentBounds, float physicalScale) const
{
    const int pw = juce::jmax (1, juce::roundToInt ((float) contentBounds.getWidth()  * physicalScale));
    const int ph = juce::jmax (1, juce::roundToInt ((float) contentBounds.getHeight() * physicalScale));

    if (! staticLayer_.isValid() || staticLayer_.getWidth() != pw || staticLayer_.getHeight() != ph)
        staticLayer_ = juce::Image (juce::Image::ARGB, pw, ph, true);
    else
        staticLayer_.clear (staticLayer_.getBounds());

    staticLayerBounds_ = contentBounds;
    staticLayerScale_  = physicalScale;

    juce::Graphics g (staticLayer_);
    g.addTransform (juce::AffineTransform::translation ((float) -contentBounds.getX(),
                                                        (float) -contentBounds.getY())
                        .scaled (physicalScale));

    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    auto canvas = getCanvasBounds(contentBounds);
    drawBackground(g, canvas);

    if (canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        drawGrid(g, canvas);
        drawAxisLabels(g, canvas);
    }
}

void SpectrumModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken(g, canvas, PinkXP::content);
}

// ----------------------------------------------------------
// 网格：每 20dB 一条横线，以及 100/1k/10k 纵线
// ----------------------------------------------------------
void SpectrumModule::drawGrid(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    auto inner = canvas.reduced(2);
    if (inner.isEmpty()) return;

    // 横线（每 20dB）
    g.setColour(PinkXP::pink200.withAlpha(0.45f));
    for (int db = (int) minDb; db <= (int) maxDb; db += 20)
    {
        const int y = (int) std::round(dbToY((float) db, canvas));
        if (y < inner.getY() || y > inner.getBottom()) continue;
        if (db == 0)
            g.setColour(PinkXP::pink400.withAlpha(0.7f));
        else
            g.setColour(PinkXP::pink200.withAlpha(0.45f));
        g.drawHorizontalLine(y, (float) inner.getX(), (float) inner.getRight());
    }

    // 纵线（主 + 次，刻度相关）
    std::vector<float> majorFreqs, minorFreqs;
    gridFreqs (majorFreqs, minorFreqs);
    g.setColour(PinkXP::pink200.withAlpha(0.3f));
    for (float f : minorFreqs)
    {
        const int x = (int) std::round(freqToX(f, canvas));
        if (x > inner.getX() && x < inner.getRight())
            g.drawVerticalLine(x, (float) inner.getY(), (float) inner.getBottom());
    }
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    for (float f : majorFreqs)
    {
        const int x = (int) std::round(freqToX(f, canvas));
        if (x > inner.getX() && x < inner.getRight())
            g.drawVerticalLine(x, (float) inner.getY(), (float) inner.getBottom());
    }
}

// ----------------------------------------------------------
// 绘制曲线（平滑 + 峰值保持）
//
//   · 低频段 20~100Hz 在对数坐标下挤在左侧 ~40% 像素内，而那段仅覆盖少数 FFT bin，
//     直接用 lineTo 折线会看到明显尖角。
//   · 这里用 Catmull-Rom 样条（tension 0.5）→ 转成三次贝塞尔 cubicTo：
//       - 曲线严格经过所有采样点（不改变频谱数值，纯视觉插值）
//       - 纯前端开销：对每个 x 点额外算两次减法/除法，完全不碰后端分析
//       - 端点重复处理避免首/末段被截掉
//       - 对控制点 y 做 jlimit 防止抛物线过冲跌出画布
// ----------------------------------------------------------
void SpectrumModule::drawCurves(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const int N = (int) smoothedDb.size();
    if (N <= 1) return;

    auto inner = canvas.reduced(2);
    if (inner.getWidth() <= 2) return;

    // ---------- 先把采样点映射到像素坐标 ----------
    //   smooth 模式：等距列；Semi / Raw：按每个 bin 的中心频率定位（不等距）。
    curvePts.resize ((size_t) N);
    const float x0   = (float) inner.getX();
    const float xLen = (float) inner.getWidth();
    const float invMax = 1.0f / (float) juce::jmax (1, N - 1);

    auto xAt = [&] (int i, const std::vector<float>& hz) noexcept
    {
        if ((int) hz.size() == N)
            return (float) inner.getX() + unitOfHz (hz[(size_t) i]) * (float) inner.getWidth();
        return x0 + (float) i * invMax * xLen;
    };

    for (int i = 0; i < N; ++i)
        curvePts[(size_t) i] = { xAt (i, smoothedHz), dbToY (smoothedDb[(size_t) i], canvas) };

    // 次曲线采样点（MID+SIDE 的 side / LEFT+RIGHT 的 R）
    const bool drawSecondary = hasSecondaryCurve() && (int) secondaryDb.size() == N;
    if (drawSecondary)
    {
        secondaryPts.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
            secondaryPts[(size_t) i] = { xAt (i, secondaryHz),
                                         dbToY (secondaryDb[(size_t) i], canvas) };
    }

    // ---------- Catmull-Rom → cubicBezier 构建 Path ----------
    const float yTop = (float) inner.getY();
    const float yBot = (float) inner.getBottom();

    // "沉底"判定阈值（像素）：当采样点的 y 到画布底的距离小于该阈值时，视为贴底。
    //   用像素判据而非 dB 判据：
    //     1) 与 dbToY 对 minDb 的精确取值无关，天然免疫浮点误差；
    //     2) 只要视觉上已贴底，就不再让邻点 p3 的切线把本段抬出一个小弧。
    //   0.75 px ≈ 亚像素级贴底，不会把"刚离开底部一点点"的正常上升误判为沉底。
    constexpr float floorPixelTol = 0.75f;

    auto buildSmoothPath = [&] (juce::Path& path,
                                const std::vector<juce::Point<float>>& P,
                                bool closeToBottom)
    {
        const int n = (int) P.size();
        if (n < 2) return;

        auto atFloor = [yBot, floorPixelTol] (const juce::Point<float>& p) noexcept
        {
            return (yBot - p.y) <= floorPixelTol;
        };

        if (closeToBottom) path.startNewSubPath (P[0].x, yBot); // fill 版：从底部开始
        else               path.startNewSubPath (P[0]);

        if (closeToBottom) path.lineTo (P[0]); // 先拉一条竖线到第一个点

        // 端点处理：P[-1] = P[0], P[n] = P[n-1]（复制端点 → Catmull-Rom 自然收束）
        for (int i = 0; i < n - 1; ++i)
        {
            const auto& p0 = (i == 0)     ? P[0]     : P[i - 1];
            const auto& p1 = P[i];
            const auto& p2 = P[i + 1];
            const auto& p3 = (i + 2 >= n) ? P[n - 1] : P[i + 2];

            // ---------- 折线模式：不做任何插值 ----------
            //   Raw 模式保持原始采样点连线，高频窄峰/尖刺按真实 dB 出角。
            if (! useSpline())
            {
                path.lineTo (p2.x, p2.y);
                continue;
            }

            // ---------- 沉底直线段：p1、p2 都贴底 ----------
            //   Catmull-Rom 在"两端都贴底"的段里，仍会按 p0/p3 的斜率生成控制点，
            //   导致 p1→p2 间出现向上或向下的小弧（尤其当 p3 是陡升峰时）。
            //   这种情况下视觉上"就是一条水平底线"，直接 lineTo 到 (p2.x, yBot)
            //   可完全消除小弧，并让填充路径严格贴画布底沿。
            if (atFloor (p1) && atFloor (p2))
            {
                path.lineTo (p2.x, yBot);
                continue;
            }

            // Catmull-Rom (tension=0.5) → Bezier 控制点
            const float c1x = p1.x + (p2.x - p0.x) / 6.0f;
            const float c2x = p2.x - (p3.x - p1.x) / 6.0f;
            float c1y       = p1.y + (p2.y - p0.y) / 6.0f;
            float c2y       = p2.y - (p3.y - p1.y) / 6.0f;

            // 若本段的起点或终点贴底，则把对应端的切线分量强制归零，
            // 避免"从底抬起/跌入底"瞬间由对向邻点造成的鼓包/下凹。
            if (atFloor (p1)) c1y = p1.y;
            if (atFloor (p2)) c2y = p2.y;

            // 防止过冲到画布外（贝塞尔控制点 y 超出会让曲线短暂跌出可视区）
            c1y = juce::jlimit (yTop, yBot, c1y);
            c2y = juce::jlimit (yTop, yBot, c2y);

            path.cubicTo (c1x, c1y, c2x, c2y, p2.x, p2.y);
        }

        if (closeToBottom)
        {
            path.lineTo (P[n - 1].x, yBot);
            path.closeSubPath();
        }
    };

    // 1) 填充区域（从底部到曲线的 lightly tinted area）
    //   反馈修订：Sharper 模式下频谱曲线保持原样（主体），谱线仅叠加辅助
    //   残影态：填充 / 光晕 / 主线三层与 GL 路径同款混向 PinkXP::sel
    //   是否着色只看通道模式：mid 一律着色，M+S / L+R 不着色（与曲线模式无关）
    const bool rawMode = (curveMode == CurveMode::raw);
    if (channelMode == ChannelMode::mid)
    {
        fillPath.clear();
        buildSmoothPath (fillPath, curvePts, /*closeToBottom*/ true);
        g.setColour (ghostTint (PinkXP::chartRamp[3].withAlpha (0.25f),
                                PinkXP::sel.withAlpha (0.22f)));
        g.fillPath (fillPath);
    }

    // 1.5) 次曲线（side / R）：只画线不填充。没有填充托底时，原来的浅色
    //   细线在浅色画布上几乎不可见 —— 改用更饱和的颜色 + 加粗。
    if (drawSecondary)
    {
        secondaryPath.clear();
        buildSmoothPath (secondaryPath, secondaryPts, /*closeToBottom*/ false);
        g.setColour (ghostTint (PinkXP::interpolateOkLab (PinkXP::chartRamp[5], PinkXP::ink, 0.22f)
                                          .withAlpha (0.88f),
                                PinkXP::sel.withAlpha (0.88f)));
        g.strokePath (secondaryPath, juce::PathStrokeType (rawMode ? 1.4f : 1.7f,
                                                           juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
    }

    // 2) 主曲线（双层描边：浅底 + 深顶，视觉厚度）
    //   无填充模式（M+S / L+R）整体加粗提亮；raw 模式去掉浅底描边，
    //   接合样式用 rounded（默认 miter 在尖刺处会拉出长尖/断口）。
    const bool noFillCpu = (channelMode != ChannelMode::mid);
    curvePath.clear();
    buildSmoothPath (curvePath, curvePts, /*closeToBottom*/ false);
    if (! rawMode)
    {
        const float glowW = noFillCpu ? 3.6f
                                      : ((curveMode == CurveMode::semi) ? 2.1f : 3.0f);
        g.setColour (ghostTint (PinkXP::chartRamp[5].withAlpha (noFillCpu ? 0.42f : 0.32f),
                                PinkXP::sel.withAlpha (0.60f)));
        g.strokePath (curvePath, juce::PathStrokeType (glowW, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    }
    g.setColour (ghostTint (PinkXP::chartRamp[6], PinkXP::sel));
    const float coreW = rawMode ? (noFillCpu ? 1.4f : 1.1f)
                                : (noFillCpu ? 1.9f : 1.4f);
    g.strokePath (curvePath, juce::PathStrokeType (coreW,
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

    // 3) 峰值保持（虚线）—— 同样做 Catmull-Rom 平滑
    if (peakHoldEnabled && (int) peakDb.size() == N)
    {
        peakCurvePts.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const float y = dbToY (peakDb[(size_t) i], canvas);
            peakCurvePts[(size_t) i] = { xAt (i, smoothedHz), y };
        }
        peakPath.clear();
        buildSmoothPath (peakPath, peakCurvePts, /*closeToBottom*/ false);

        dashedPeakPath.clear();
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashedPeakPath, peakPath, dashes, 2);
        g.setColour (PinkXP::ink.withAlpha (0.75f));
        g.fillPath (dashedPeakPath);
    }

    // 4) Sharper：重分配谱线（全高竖线；峰值强度映射颜色）
    if (sharpEnabled && ! sharpLines.empty())
    {
        const float yBotF = (float) inner.getBottom();
        for (const auto& l : sharpLines)
        {
            const float x = freqToX ((float) l.freqHz, canvas);
            if (x < (float) inner.getX() - 2.0f || x > (float) inner.getRight() + 2.0f)
                continue;

            const float a     = juce::jlimit (0.0f, 1.0f, l.alpha);
            const float strength = juce::jlimit (0.0f, 1.0f,
                                                  (l.magDb - minDb) / (maxDb - minDb));
            const auto lineColour = PinkXP::pink200.interpolatedWith (PinkXP::sel, strength);
            const int yTop = inner.getY();
            const int h = juce::jmax (1, inner.getHeight());

            g.setColour (lineColour.withAlpha (0.18f * a));
            g.fillRect (juce::Rectangle<int> ((int) std::floor (x - 1.5f), yTop,
                                              3, (int) h));
            g.setColour (lineColour.withAlpha (0.85f * a));
            g.fillRect (juce::Rectangle<int> ((int) std::floor (x - 0.6f), yTop,
                                              1, (int) h));
        }
    }
}

// ----------------------------------------------------------
// 坐标轴标签（左侧 dB / 底部 Hz）
// ----------------------------------------------------------
void SpectrumModule::drawAxisLabels(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    // 轴标签是静态文本，但组件每帧全量重绘；用位图缓存避免每帧塑形 + 逐
    // glyph GL 调用（mac NSOpenGL 下开销是 Windows 约 10 倍）。
    const juce::Colour labelColour = PinkXP::ink.withAlpha(0.85f);

    // 左侧 dB 标签
    for (int db = (int) minDb; db <= (int) maxDb; db += 20)
    {
        const int y = (int) std::round(dbToY((float) db, canvas));
        juce::String s = (db == 0) ? " 0" : juce::String(db);
        const auto img = PinkXP::cachedStaticText (
            g, s, 9.0f, juce::Font::plain, labelColour,
            juce::Colours::transparentBlack, true);
        if (img.isValid())
        {
            img.drawAt (g,
                        (float) (canvas.getX() - 32 + (28 - img.getWidth())),
                        (float) (y - img.getHeight() / 2));
        }
        else
        {
            g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
            g.setColour(labelColour);
            g.drawText(y2k::tr (s), canvas.getX() - 32, y - 6, 28, 12,
                       juce::Justification::centredRight, false);
        }
    }

    // 底部 Hz 标签（20 / 20k 两端 + 当前刻度的主网格线；Log 下与旧版逐项一致）
    auto hzLabel = [] (float hz) -> juce::String
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k" : juce::String ((int) hz);
    };

    std::vector<float> majorHz, minorHz;
    gridFreqs (majorHz, minorHz);
    std::vector<float> labelHz;
    labelHz.push_back (20.0f);
    for (float f : majorHz) labelHz.push_back (f);
    labelHz.push_back (20000.0f);

    for (float hz : labelHz)
    {
        const int x = (int) std::round(freqToX(hz, canvas));
        if (x < canvas.getX() - 4 || x > canvas.getRight() + 4) continue;
        const juce::String text = hzLabel (hz);
        const auto img = PinkXP::cachedStaticText (
            g, text, 9.0f, juce::Font::plain, labelColour,
            juce::Colours::transparentBlack, true);
        if (img.isValid())
        {
            img.drawAt (g, (float) (x - img.getWidth() / 2), (float) (canvas.getBottom() + 2));
        }
        else
        {
            g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
            g.setColour(labelColour);
            g.drawText(y2k::tr (text), x - 18, canvas.getBottom() + 2, 36, 12,
                       juce::Justification::centred, false);
        }
    }
}
