#include "source/ui/modules/FineSplitModules.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"

#include <cmath>

namespace
{
    struct VuCoreStop
    {
        float        pos;
        juce::Colour colour;
    };

    juce::Colour sampleVuCoreStops (const VuCoreStop* stops, int count, float t) noexcept
    {
        if (count <= 0)
            return PinkXP::sel;
        t = juce::jlimit (0.0f, 1.0f, t);
        if (t <= stops[0].pos)
            return stops[0].colour;
        for (int i = 0; i < count - 1; ++i)
        {
            if (t <= stops[i + 1].pos)
            {
                const float span = stops[i + 1].pos - stops[i].pos;
                const float frac = (span > 1.0e-5f) ? ((t - stops[i].pos) / span) : 0.0f;
                return PinkXP::interpolateOkLab (stops[i].colour, stops[i + 1].colour, frac);
            }
        }
        return stops[count - 1].colour;
    }
}

// ==========================================================
// VuMeterModule —— 模拟指针 / 横向条形 双大类专业 VU 电平表
//
// 数据来源：AnalyserHub::Kind::Oscilloscope → 从 2048 样本原始波形尾部
//         取 rmsWindowMs 窗口自算瞬时 RMS，后端音频线程零新增分析计算。
// ==========================================================
VuMeterModule::VuMeterModule (AnalyserHub& h)
    : ModulePanel (ModuleType::vuMeter), hub (h)
{
    hub.retain (AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener (this);

    setMinSize (96, 80);
    setDefaultSize (320, 224);
    setTitleText ("VU Meter");
    setControlPanelEnabled (true);

    // 主路径改为 onFrame 推进弹道 + 重绘；Timer 仅作 10Hz 兜底。
    lastTickMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (10);

    themeSubToken = PinkXP::subscribeThemeChanged ([this]
    {
        repaint();
    });
}

VuMeterModule::~VuMeterModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }
    stopTimer();
    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Oscilloscope);
}

void VuMeterModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    const int bufLen = (int) frame.oscL.size();
    if (bufLen <= 0) return;

    const double sr = (hub.getSampleRate() > 0.0) ? hub.getSampleRate() : 48000.0;
    const int windowSamples = juce::jlimit (64, bufLen, juce::roundToInt (sr * rmsWindowMs * 0.001));
    const int n = juce::jmin (bufLen, windowSamples);
    const int startIdx = bufLen - n;

    double sumSqL = 0.0;
    double sumSqR = 0.0;
    for (int i = startIdx; i < bufLen; ++i)
    {
        const float sL = frame.oscL[(size_t) i];
        const float sR = frame.oscR[(size_t) i];
        sumSqL += (double) sL * (double) sL;
        sumSqR += (double) sR * (double) sR;
    }
    const double meanSqL = sumSqL / (double) n;
    const double meanSqR = sumSqR / (double) n;

    auto msToDb = [this] (double meanSq) -> float
    {
        if (meanSq <= 1.0e-10) return -144.0f;
        return (float) (10.0 * std::log10 (meanSq)) + trimDb;
    };
    targetL = msToDb (meanSqL);
    targetR = msToDb (meanSqR);

    // 读数类 30Hz 重绘 —— 隔帧推进弹道（dt 翻倍保持时间常数一致）。
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    lastOnFrameMs = nowMs;
    lastTickMs    = nowMs;
    if ((frame.tickCount & 1ull) == 0)
        advanceBallistics (frame.dtSec * 2000.0f);
}

void VuMeterModule::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs - lastOnFrameMs < 100.0)
    {
        lastTickMs = nowMs;
        return;
    }

    double dtMs = nowMs - lastTickMs;
    lastTickMs  = nowMs;
    if (dtMs < 0.0)   dtMs = 16.0;
    if (dtMs > 100.0) dtMs = 100.0;
    const float dt = (float) dtMs;

    // 防御性 pull 兜底：onFrame 暂未到达时主动拉最新帧
    if (auto frame = hub.getLatestFrame())
    {
        if (frame->has (AnalyserHub::Kind::Oscilloscope))
        {
            const int bufLen = (int) frame->oscL.size();
            if (bufLen > 0)
            {
                const double sr = (hub.getSampleRate() > 0.0) ? hub.getSampleRate() : 48000.0;
                const int windowSamples = juce::jlimit (64, bufLen, juce::roundToInt (sr * rmsWindowMs * 0.001));
                const int n = juce::jmin (bufLen, windowSamples);
                const int startIdx = bufLen - n;

                double sumSqL = 0.0;
                double sumSqR = 0.0;
                for (int i = startIdx; i < bufLen; ++i)
                {
                    const float sL = frame->oscL[(size_t) i];
                    const float sR = frame->oscR[(size_t) i];
                    sumSqL += (double) sL * (double) sL;
                    sumSqR += (double) sR * (double) sR;
                }
                const double meanSqL = sumSqL / (double) n;
                const double meanSqR = sumSqR / (double) n;

                auto msToDb = [this] (double ms) -> float
                {
                    if (ms <= 1.0e-10) return -144.0f;
                    return (float) (10.0 * std::log10 (ms)) + trimDb;
                };
                targetL = msToDb (meanSqL);
                targetR = msToDb (meanSqR);
            }
        }
    }

    advanceBallistics (dt);
}

void VuMeterModule::advanceBallistics (float dt)
{
    dt = juce::jlimit (0.0f, 100.0f, dt);

    // 按 ballistics 档位选择上升 / 下降 / 静音时间常数（1 = Classic 沿用经典常量）
    float tRise = tauRiseMs, tFall = tauFallMs, tMute = tauMuteFallMs;
    switch (ballistics)
    {
        case 0: tRise = 160.0f; tFall = 1800.0f; tMute = 2400.0f; break; // Slow (vintage)
        case 2: tRise =  35.0f; tFall =  500.0f; tMute =  800.0f; break; // Fast
        case 3: tRise =  15.0f; tFall =  220.0f; tMute =  400.0f; break; // Snappy
        default: break;                                                   // Classic
    }
    const float aUp       = 1.0f - std::exp (-dt / tRise);
    const float aDown     = 1.0f - std::exp (-dt / tFall);
    const float aMuteDown = 1.0f - std::exp (-dt / tMute);

    auto smoothOne = [&] (float& disp, float tgt)
    {
        if (! std::isfinite (disp)) disp = -144.0f;
        if (! std::isfinite (tgt))  tgt  = -144.0f;
        const float a = (tgt > disp) ? aUp
                      : (tgt <= -143.0f ? aMuteDown : aDown);
        disp += a * (tgt - disp);
    };
    smoothOne (displayedL, targetL);
    smoothOne (displayedR, targetR);

    // 峰值保持追踪：按 peakHoldSec_ 保持后以 6 dB/s 平缓回落
    const float monoNow = currentMonoDbfs();
    const float holdMs  = juce::jlimit (500.0f, 4000.0f, peakHoldSec_ * 1000.0f);
    auto updatePeak = [&] (float cur, float& pk, float& timerMs)
    {
        if (cur >= pk)
        {
            pk = cur;
            timerMs = 0.0f;
        }
        else
        {
            timerMs += dt;
            if (timerMs > holdMs)
                pk = juce::jmax (cur, pk - 6.0f * (dt * 0.001f));
        }
    };
    if (peakHold)
    {
        updatePeak (displayedL, peakHoldL,    peakHoldTimerL);
        updatePeak (displayedR, peakHoldR,    peakHoldTimerR);
        updatePeak (monoNow,    peakHoldMono, peakHoldTimerM);
    }

    // LED 跟随（单声道/立体声追踪 L+R 合成，单左/单右严格追踪对应声道），非对称：上升立即跟、下降快速衰减
    const float ledSrcDb = (channelMode_ == ChannelMode::left)  ? displayedL
                         : (channelMode_ == ChannelMode::right) ? displayedR
                         : monoNow;
    if (ledSrcDb > ledLevelDb)
    {
        ledLevelDb = ledSrcDb;
    }
    else
    {
        constexpr float tauLedMs = 150.0f;
        const float aLed = 1.0f - std::exp (-dt / tauLedMs);
        ledLevelDb += aLed * (ledSrcDb - ledLevelDb);
    }

    // 脉动相位（~0.9 秒一圈）
    constexpr float omegaRadPerMs = juce::MathConstants<float>::twoPi / 900.0f;
    pulsePhase += omegaRadPerMs * dt;
    if (pulsePhase > juce::MathConstants<float>::twoPi)
        pulsePhase -= juce::MathConstants<float>::twoPi;

    repaintFrameContent();
}

// ==========================================================
// 共享量程 / 坐标 / 刻度 / 色谱 / 文本辅助（供 .inl 渲染器共用）
// ==========================================================
float VuMeterModule::minDisplayDb() const noexcept
{
    switch (scaleRange_)
    {
        case ScaleRange::r24: return -24.0f;
        case ScaleRange::r36: return -36.0f;
        case ScaleRange::r48: return -48.0f;
        case ScaleRange::r60: return -60.0f;
        default:              return -36.0f;
    }
}

float VuMeterModule::maxDisplayDb() const noexcept
{
    return (scaleRange_ == ScaleRange::r60) ? 0.0f : +3.0f;
}

float VuMeterModule::cautionDbfs() const noexcept
{
    return juce::jmax (minDisplayDb() + 3.0f, warnDbfs - 6.0f);
}

float VuMeterModule::currentMonoDbfs() const noexcept
{
    auto dbToPower = [] (float db) -> float
    {
        if (db <= -143.0f || ! std::isfinite (db)) return 0.0f;
        return std::pow (10.0f, db * 0.1f);
    };
    const float pL   = dbToPower (displayedL);
    const float pR   = dbToPower (displayedR);
    const float pAvg = (pL + pR) * 0.5f;
    return (pAvg > 1.0e-14f) ? 10.0f * std::log10 (pAvg) : -144.0f;
}

float VuMeterModule::activeSingleDbfs() const noexcept
{
    switch (channelMode_)
    {
        case ChannelMode::left:   return displayedL;
        case ChannelMode::right:  return displayedR;
        case ChannelMode::stereo: return juce::jmax (displayedL, displayedR);
        case ChannelMode::mono:
        default:                  return currentMonoDbfs();
    }
}

float VuMeterModule::activeSinglePeakDbfs() const noexcept
{
    switch (channelMode_)
    {
        case ChannelMode::left:   return peakHoldL;
        case ChannelMode::right:  return peakHoldR;
        case ChannelMode::stereo: return juce::jmax (peakHoldL, peakHoldR);
        case ChannelMode::mono:
        default:                  return peakHoldMono;
    }
}

const char* VuMeterModule::activeChannelTag() const noexcept
{
    switch (channelMode_)
    {
        case ChannelMode::left:  return "L";
        case ChannelMode::right: return "R";
        default:                 return nullptr;
    }
}

float VuMeterModule::dialTFor (float db) const noexcept
{
    const float lo = minDisplayDb();
    const float hi = maxDisplayDb();
    if (! std::isfinite (db) || hi <= lo)
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, juce::jmap (db, lo, hi, 0.0f, 1.0f));
}

float VuMeterModule::needleAngleFor (float db) const noexcept
{
    if (isLinearStyle())
        return 0.0f;
    return juce::jmap (dialTFor (db), 0.0f, 1.0f, dialGeom.angleStart, dialGeom.angleEnd);
}

float VuMeterModule::needleLengthFor (float db) const noexcept
{
    juce::ignoreUnused (db);
    if (isLinearStyle())
        return juce::jmax (8.0f, dialGeom.trackBottom - dialGeom.trackTop);
    if (dialGeom.needleLen > 0.0f)
        return juce::jmax (8.0f, dialGeom.needleLen);
    return juce::jmax (8.0f, dialGeom.radius);
}

float VuMeterModule::linearXFor (float db) const noexcept
{
    return juce::jmap (dialTFor (db), 0.0f, 1.0f, dialGeom.leftX, dialGeom.rightX);
}

float VuMeterModule::linearDbForX (float x) const noexcept
{
    const float lo = minDisplayDb();
    const float hi = maxDisplayDb();
    if (dialGeom.rightX <= dialGeom.leftX + 1.0f)
        return lo;
    const float t = juce::jlimit (0.0f, 1.0f,
                                  (x - dialGeom.leftX) / (dialGeom.rightX - dialGeom.leftX));
    return juce::jmap (t, lo, hi);
}

void VuMeterModule::getScaleTicks (std::vector<VuScaleTick>& out) const
{
    out.clear();

    switch (scaleRange_)
    {
        case ScaleRange::r24:
        {
            static constexpr VuScaleTick kTicks24[] = {
                { -24.0f, true,  "-24" }, { -23.0f, false, nullptr }, { -22.0f, false, nullptr },
                { -21.0f, false, nullptr }, { -20.0f, true,  "-20" }, { -19.0f, false, nullptr },
                { -18.0f, false, nullptr }, { -17.0f, false, nullptr }, { -16.0f, false, nullptr },
                { -15.0f, true,  "-15" }, { -14.0f, false, nullptr }, { -13.0f, false, nullptr },
                { -12.0f, false, nullptr }, { -11.0f, false, nullptr }, { -10.0f, true,  "-10" },
                {  -9.0f, false, nullptr }, {  -8.0f, false, nullptr }, {  -7.0f, false, nullptr },
                {  -6.0f, true,  "-6"  }, {  -5.0f, false, nullptr }, {  -4.0f, false, nullptr },
                {  -3.0f, true,  "-3"  }, {  -2.0f, false, nullptr }, {  -1.0f, false, nullptr },
                {   0.0f, true,   "0"  }, {  +1.0f, false, nullptr }, {  +2.0f, false, nullptr },
                {  +3.0f, true,  "+3"  },
            };
            out.assign (std::begin (kTicks24), std::end (kTicks24));
            break;
        }
        case ScaleRange::r48:
        {
            static constexpr VuScaleTick kTicks48[] = {
                { -48.0f, true,  "-48" }, { -46.0f, false, nullptr }, { -44.0f, false, nullptr },
                { -42.0f, true,  nullptr }, { -40.0f, false, nullptr }, { -38.0f, false, nullptr },
                { -36.0f, true,  "-36" }, { -34.0f, false, nullptr }, { -32.0f, false, nullptr },
                { -30.0f, true,  nullptr }, { -28.0f, false, nullptr }, { -26.0f, false, nullptr },
                { -24.0f, true,  "-24" }, { -22.0f, false, nullptr }, { -20.0f, false, nullptr },
                { -18.0f, true,  "-18" }, { -16.0f, false, nullptr }, { -14.0f, false, nullptr },
                { -12.0f, true,  "-12" }, { -11.0f, false, nullptr }, { -10.0f, false, nullptr },
                {  -9.0f, false, nullptr }, {  -8.0f, false, nullptr }, {  -7.0f, false, nullptr },
                {  -6.0f, true,  "-6"  }, {  -5.0f, false, nullptr }, {  -4.0f, false, nullptr },
                {  -3.0f, true,  "-3"  }, {  -2.0f, false, nullptr }, {  -1.0f, false, nullptr },
                {   0.0f, true,   "0"  }, {  +1.0f, false, nullptr }, {  +2.0f, false, nullptr },
                {  +3.0f, true,  "+3"  },
            };
            out.assign (std::begin (kTicks48), std::end (kTicks48));
            break;
        }
        case ScaleRange::r60:
        {
            static constexpr VuScaleTick kTicks60[] = {
                { -60.0f, true,  "-60" }, { -57.0f, false, nullptr }, { -54.0f, true,  nullptr },
                { -51.0f, false, nullptr }, { -48.0f, true,  "-48" }, { -45.0f, false, nullptr },
                { -42.0f, true,  nullptr }, { -39.0f, false, nullptr }, { -36.0f, true,  "-36" },
                { -33.0f, false, nullptr }, { -30.0f, true,  nullptr }, { -27.0f, false, nullptr },
                { -24.0f, true,  "-24" }, { -22.0f, false, nullptr }, { -20.0f, false, nullptr },
                { -18.0f, true,  "-18" }, { -16.0f, false, nullptr }, { -14.0f, false, nullptr },
                { -12.0f, true,  "-12" }, { -11.0f, false, nullptr }, { -10.0f, false, nullptr },
                {  -9.0f, false, nullptr }, {  -8.0f, false, nullptr }, {  -7.0f, false, nullptr },
                {  -6.0f, true,  "-6"  }, {  -5.0f, false, nullptr }, {  -4.0f, false, nullptr },
                {  -3.0f, true,  "-3"  }, {  -2.0f, false, nullptr }, {  -1.0f, false, nullptr },
                {   0.0f, true,   "0"  },
            };
            out.assign (std::begin (kTicks60), std::end (kTicks60));
            break;
        }
        case ScaleRange::r36:
        default:
        {
            static constexpr VuScaleTick kTicks36[] = {
                { -36.0f, true,  "-36" }, { -35.0f, false, nullptr }, { -34.0f, false, nullptr },
                { -33.0f, false, nullptr }, { -32.0f, false, nullptr }, { -31.0f, false, nullptr },
                { -30.0f, true,  "-30" }, { -29.0f, false, nullptr }, { -28.0f, false, nullptr },
                { -27.0f, false, nullptr }, { -26.0f, false, nullptr }, { -25.0f, false, nullptr },
                { -24.0f, true,  "-24" }, { -23.0f, false, nullptr }, { -22.0f, false, nullptr },
                { -21.0f, false, nullptr }, { -20.0f, false, nullptr }, { -19.0f, false, nullptr },
                { -18.0f, true,  "-18" }, { -17.0f, false, nullptr }, { -16.0f, false, nullptr },
                { -15.0f, false, nullptr }, { -14.0f, false, nullptr }, { -13.0f, false, nullptr },
                { -12.0f, true,  "-12" }, { -11.0f, false, nullptr }, { -10.0f, false, nullptr },
                {  -9.0f, false, nullptr }, {  -8.0f, false, nullptr }, {  -7.0f, false, nullptr },
                {  -6.0f, true,  "-6"  }, {  -5.0f, false, nullptr }, {  -4.0f, false, nullptr },
                {  -3.0f, true,  "-3"  }, {  -2.0f, false, nullptr }, {  -1.0f, false, nullptr },
                {   0.0f, true,   "0"  }, {  +1.0f, false, nullptr }, {  +2.0f, false, nullptr },
                {  +3.0f, true,  "+3"  },
            };
            out.assign (std::begin (kTicks36), std::end (kTicks36));
            break;
        }
    }
}

juce::Colour VuMeterModule::vuAlertColour() const noexcept
{
    switch (colorMap_)
    {
        case ColorMap::studio:    return juce::Colour (0xffe8263a);
        case ColorMap::warmVu:    return juce::Colour (0xffe03228);
        case ColorMap::miniMeter: return juce::Colour (0xffff2468);
        case ColorMap::ice:       return juce::Colour (0xffff4b5c);
        case ColorMap::theme:
        default:                  return PinkXP::danger;
    }
}

juce::Colour VuMeterModule::vuPaletteColour (float t01, float dbVal, bool darkCanvas) const noexcept
{
    const float t      = juce::jlimit (0.0f, 1.0f, t01);
    const float maxDb  = maxDisplayDb();
    const float cautDb = cautionDbfs();
    const float overT  = (maxDb > warnDbfs + 0.05f)
                             ? juce::jlimit (0.0f, 1.0f, (dbVal - warnDbfs) / (maxDb - warnDbfs))
                             : 0.0f;

    switch (colorMap_)
    {
        case ColorMap::studio:
        {
            if (dbVal >= warnDbfs)
            {
                const auto lo = darkCanvas ? juce::Colour (0xffe8263a) : juce::Colour (0xffd61e32);
                const auto hi = darkCanvas ? juce::Colour (0xffff5262) : juce::Colour (0xffe82c40);
                return PinkXP::interpolateOkLab (lo, hi, overT);
            }
            if (dbVal >= cautDb && warnDbfs > cautDb + 0.05f)
            {
                const float ct = juce::jlimit (0.0f, 1.0f, (dbVal - cautDb) / (warnDbfs - cautDb));
                const VuCoreStop darkCaut[3] = {
                    { 0.00f, juce::Colour (0xff9ad928) },
                    { 0.50f, juce::Colour (0xfff4ae20) },
                    { 1.00f, juce::Colour (0xfff66c18) },
                };
                const VuCoreStop lightCaut[3] = {
                    { 0.00f, juce::Colour (0xff82b614) },
                    { 0.50f, juce::Colour (0xffdf9010) },
                    { 1.00f, juce::Colour (0xffe25a10) },
                };
                return sampleVuCoreStops (darkCanvas ? darkCaut : lightCaut, 3, ct);
            }
            const float loDb = minDisplayDb();
            const float gt = (cautDb > loDb + 0.1f)
                                 ? juce::jlimit (0.0f, 1.0f, (dbVal - loDb) / (cautDb - loDb))
                                 : t;
            const VuCoreStop darkGreen[3] = {
                { 0.00f, juce::Colour (0xff12884c) },
                { 0.55f, juce::Colour (0xff20c464) },
                { 1.00f, juce::Colour (0xff56e87c) },
            };
            const VuCoreStop lightGreen[3] = {
                { 0.00f, juce::Colour (0xff167f44) },
                { 0.55f, juce::Colour (0xff1ea654) },
                { 1.00f, juce::Colour (0xff36be60) },
            };
            return sampleVuCoreStops (darkCanvas ? darkGreen : lightGreen, 3, gt);
        }

        case ColorMap::warmVu:
        {
            if (dbVal >= warnDbfs)
            {
                const auto lo = darkCanvas ? juce::Colour (0xffe03228) : juce::Colour (0xffc9241c);
                const auto hi = darkCanvas ? juce::Colour (0xffff6248) : juce::Colour (0xffe03628);
                return PinkXP::interpolateOkLab (lo, hi, overT);
            }
            const VuCoreStop darkWarm[4] = {
                { 0.00f, juce::Colour (0xff58340a) },
                { 0.35f, juce::Colour (0xffa26614) },
                { 0.72f, juce::Colour (0xffd89624) },
                { 1.00f, juce::Colour (0xffffd068) },
            };
            const VuCoreStop lightWarm[4] = {
                { 0.00f, juce::Colour (0xff633b0c) },
                { 0.35f, juce::Colour (0xff9e6212) },
                { 0.72f, juce::Colour (0xffc98820) },
                { 1.00f, juce::Colour (0xffe29e2a) },
            };
            return sampleVuCoreStops (darkCanvas ? darkWarm : lightWarm, 4, t);
        }

        case ColorMap::miniMeter:
        {
            if (dbVal >= warnDbfs)
            {
                const auto lo = darkCanvas ? juce::Colour (0xffff2468) : juce::Colour (0xffe01852);
                const auto hi = darkCanvas ? juce::Colour (0xffff6490) : juce::Colour (0xfff42a64);
                return PinkXP::interpolateOkLab (lo, hi, overT);
            }
            const VuCoreStop darkMini[5] = {
                { 0.00f, juce::Colour (0xff2b0b62) },
                { 0.32f, juce::Colour (0xff6e129c) },
                { 0.64f, juce::Colour (0xffc80c94) },
                { 0.86f, juce::Colour (0xffff1ea0) },
                { 1.00f, juce::Colour (0xffff72ce) },
            };
            const VuCoreStop lightMini[4] = {
                { 0.00f, juce::Colour (0xff351478) },
                { 0.36f, juce::Colour (0xff7618a2) },
                { 0.72f, juce::Colour (0xffbf108c) },
                { 1.00f, juce::Colour (0xffe81884) },
            };
            return sampleVuCoreStops (darkCanvas ? darkMini : lightMini, darkCanvas ? 5 : 4, t);
        }

        case ColorMap::ice:
        {
            if (dbVal >= warnDbfs)
            {
                const auto lo = darkCanvas ? juce::Colour (0xffff4b5c) : juce::Colour (0xffde3244);
                const auto hi = darkCanvas ? juce::Colour (0xffff828e) : juce::Colour (0xfff04658);
                return PinkXP::interpolateOkLab (lo, hi, overT);
            }
            const VuCoreStop darkIce[4] = {
                { 0.00f, juce::Colour (0xff0e3472) },
                { 0.38f, juce::Colour (0xff167ac0) },
                { 0.74f, juce::Colour (0xff3ac6ee) },
                { 1.00f, juce::Colour (0xffa8f2ff) },
            };
            const VuCoreStop lightIce[4] = {
                { 0.00f, juce::Colour (0xff123c80) },
                { 0.40f, juce::Colour (0xff1270b0) },
                { 0.76f, juce::Colour (0xff189ad0) },
                { 1.00f, juce::Colour (0xff32b4de) },
            };
            return sampleVuCoreStops (darkCanvas ? darkIce : lightIce, 4, t);
        }

        case ColorMap::theme:
        default:
        {
            if (dbVal >= warnDbfs)
            {
                const auto hi = darkCanvas
                                    ? PinkXP::interpolateOkLab (PinkXP::danger, juce::Colour (0xffff8a7a), 0.32f)
                                    : PinkXP::danger.darker (0.08f);
                return PinkXP::interpolateOkLab (PinkXP::danger, hi, overT);
            }

            if (darkCanvas)
            {
                const VuCoreStop stops[4] = {
                    { 0.00f, PinkXP::interpolateOkLab (PinkXP::chartRamp[2], PinkXP::chartRamp[4], 0.45f) },
                    { 0.38f, PinkXP::chartRamp[5] },
                    { 0.76f, PinkXP::interpolateOkLab (PinkXP::chartRamp[6], PinkXP::sel, 0.62f) },
                    { 1.00f, PinkXP::interpolateOkLab (PinkXP::sel, PinkXP::hl, 0.46f) },
                };
                return sampleVuCoreStops (stops, 4, t);
            }

            const VuCoreStop stops[4] = {
                { 0.00f, PinkXP::chartRamp[3] },
                { 0.38f, PinkXP::chartRamp[5] },
                { 0.76f, PinkXP::interpolateOkLab (PinkXP::chartRamp[6], PinkXP::sel, 0.55f) },
                { 1.00f, PinkXP::interpolateOkLab (PinkXP::chartRamp[7], PinkXP::sel, 0.35f) },
            };
            return sampleVuCoreStops (stops, 4, t);
        }
    }
}

juce::String VuMeterModule::formatDbReadout (float db) const
{
    if (! std::isfinite (db) || db < minDisplayDb())
        return "--.-";
    if (std::abs (db) < 0.05f)
        return "0.0";
    return (db > 0.0f ? "+" : "") + juce::String (db, 1);
}

juce::Rectangle<int> VuMeterModule::ledBounds (juce::Rectangle<int> area) const
{
    const int ledSz = juce::jlimit (10, 18,
                                    juce::jmin (area.getWidth(), area.getHeight()) / 12);
    return { area.getRight() - ledSz - 6, area.getY() + 6, ledSz, ledSz };
}

juce::Rectangle<float> VuMeterModule::linearBody (juce::Rectangle<int> dialArea) const
{
    const auto  led = ledBounds (dialArea);
    const float top = (float) led.getBottom() + 2.0f;
    return { (float) dialArea.getX() + 8.0f, top,
             juce::jmax (0.0f, (float) dialArea.getWidth() - 16.0f),
             juce::jmax (0.0f, (float) dialArea.getBottom() - 8.0f - top) };
}

// ==========================================================
// Toybox：迪斯科灯 + 指针虚影（支持单表/立体声双表）+ 5 种条形专属互动
// ==========================================================
bool VuMeterModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto area = getContentBounds().reduced (6);
    if (! area.contains (panelPos))
        return false;

    const auto ledArea = ledBounds (area);

    // 1 = 迪斯科灯；2 = 指针虚影（指针式）；3 = 横向条形专属互动
    out.kind = (showLed && ledArea.contains (panelPos)) ? 1 : (isLinearStyle() ? 3 : 2);
    out.rect = area.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void VuMeterModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    if (hit.kind == 1)
    {
        const auto ledRect = ledBounds (hit.rect.getSmallestIntegerContainer());
        ctx.addModuleActorCapped (makeDiscoLedActor (ledRect.toFloat()), 6);
    }
    else if (isLinearStyle() && dialGeom.valid)
    {
        toyPokeLinear (hit, ctx);
    }
    else
    {
        // 从点击角度弹拨荧光虚影指针，欠阻尼弹簧回弹到当前真实指针角度
        const bool haveGeom = dialGeom.valid && ! isLinearStyle() && dialGeom.radius > 4.0f;
        const bool useRight = haveGeom && dialGeom.dualSide && dialGeom.radiusR > 4.0f
                              && hit.pos.x >= 0.5f * (dialGeom.pivot.x + dialGeom.pivotR.x);

        const float liveDb = (haveGeom && dialGeom.dualSide)
                                 ? (useRight ? displayedR : displayedL)
                                 : (isStereoMode() ? displayedL : activeSingleDbfs());
        const float restTh = juce::jlimit (0.04f, juce::MathConstants<float>::pi - 0.04f,
                                           needleAngleFor (liveDb) + juce::MathConstants<float>::halfPi);
        const juce::Point<float> piv = haveGeom
                                           ? (useRight ? dialGeom.pivotR : dialGeom.pivot)
                                           : juce::Point<float> (hit.rect.getCentreX(), hit.rect.getBottom() - 8.0f);
        const float R = haveGeom
                            ? (useRight ? dialGeom.radiusR : dialGeom.radius)
                            : juce::jmax (6.0f, juce::jmin (hit.rect.getWidth() * 0.5f,
                                                            hit.rect.getHeight()) - 14.0f);
        const float clickTh = juce::jlimit (0.04f, juce::MathConstants<float>::pi - 0.04f,
                                            std::atan2 (juce::jmax (1.0f, piv.y - hit.pos.y), piv.x - hit.pos.x));
        ctx.addModuleActorCapped (makeGhostNeedleActor (piv, R, restTh, clickTh), 6);
    }
}

void VuMeterModule::toyPokeLinear (const ToyHit& hit, ToyContext& ctx)
{
    if (dialGeom.rightX <= dialGeom.leftX + 2.0f)
        return;

    const float clickX  = juce::jlimit (dialGeom.leftX, dialGeom.rightX, hit.pos.x);
    const float clickDb = linearDbForX (clickX);
    const float monoDb  = activeSingleDbfs();
    const float restX   = linearXFor (monoDb);
    const float minDb   = minDisplayDb();
    const float maxDb   = maxDisplayDb();
    const bool  darkCanvas = (PinkXP::content.getPerceivedBrightness() < 0.45f);

    switch (barDialStyle_)
    {
        case BarDialStyle::studioBar:
        {
            const float topY = (dialGeom.bandBottom > dialGeom.bandTop) ? dialGeom.bandTop : dialGeom.trackTop;
            const float botY = (dialGeom.bandBottom > dialGeom.bandTop) ? dialGeom.bandBottom : dialGeom.trackBottom;
            ctx.addModuleActorCapped (makeSliderFlickActor (topY - 5.0f, botY + 5.0f, clickX, restX), 6);
            break;
        }
        case BarDialStyle::ledBlocks:
        case BarDialStyle::ledLadder:
        {
            const bool isLadder = (barDialStyle_ == BarDialStyle::ledLadder);
            int row = 0;
            if (dialGeom.ladderRows == 2 && hit.pos.y > dialGeom.rowBot[0])
                row = 1;

            const bool  hasRow = dialGeom.ladderRows > 0 && dialGeom.rowBot[row] > dialGeom.rowTop[row] + 1.0f;
            const float rTop   = hasRow ? dialGeom.rowTop[row] : dialGeom.bandTop;
            const float rBot   = hasRow ? dialGeom.rowBot[row] : dialGeom.bandBottom;
            const bool  hang   = hasRow ? dialGeom.rowHang[row] : false;
            const float hMax   = rBot - rTop;
            if (hMax < 2.0f)
                break;

            int nCols = 36;
            if (barDensity_ == BarDensity::chunky)
                nCols = 22;
            else if (barDensity_ == BarDensity::fine)
                nCols = isLadder ? 54 : 58;
            else
                nCols = isLadder ? 36 : 38;

            const float spanX = dialGeom.rightX - dialGeom.leftX;
            const float pitch = spanX / (float) nCols;
            const float gap   = pitch >= 3.0f ? juce::jlimit (1.0f, 3.0f, pitch * 0.25f) : 0.0f;
            const float colW  = juce::jmax (1.0f, pitch - gap);
            const float hMin  = isLadder ? (hMax * 0.28f) : hMax;
            const float clickT = dialTFor (clickDb);
            const int   last   = juce::jlimit (0, nCols - 1, (int) std::floor (clickT * (float) nCols));

            std::vector<juce::Rectangle<float>> cols;
            std::vector<juce::Colour>           colours;
            cols.reserve ((size_t) last + 1);
            colours.reserve ((size_t) last + 1);
            for (int i = 0; i <= last; ++i)
            {
                const float frac = (nCols > 1) ? ((float) i / (float) (nCols - 1)) : 0.0f;
                const float db0  = juce::jmap (frac, minDb, maxDb);
                const float h    = hMin + (hMax - hMin) * frac;
                const float x    = dialGeom.leftX + (float) i * pitch + gap * 0.5f;
                cols.push_back ({ x, hang ? rTop : (rBot - h), colW, h });
                colours.push_back (vuPaletteColour (frac, db0, darkCanvas));
            }
            ctx.addModuleActorCapped (makeLedChaseActor (std::move (cols), std::move (colours), hang ? 1.0f : -1.0f), 6);
            break;
        }
        case BarDialStyle::minimal:
        {
            const float fromDb = (std::isfinite (monoDb) && monoDb >= minDb) ? monoDb : minDb;
            const float delta  = clickDb - fromDb;
            const juce::String label = (std::abs (delta) < 0.05f)
                                           ? juce::String ("= 0.0 dB")
                                           : (delta > 0.0f ? "+" : "") + juce::String (delta, 1) + " dB";
            ctx.addModuleActorCapped (makeMeasureTapeActor (dialGeom.bandTop - 20.0f, restX, clickX, label), 6);
            break;
        }
        case BarDialStyle::mirrored:
        default:
        {
            const float spineY = (dialGeom.bandTop + dialGeom.bandBottom) * 0.5f;
            const float topY   = (dialGeom.rowBot[0] > dialGeom.rowTop[0] + 1.0f) ? dialGeom.rowTop[0] : dialGeom.trackTop;
            const float botY   = (dialGeom.rowBot[0] > dialGeom.rowTop[0] + 1.0f) ? dialGeom.rowBot[0] : dialGeom.trackBottom;
            ctx.addModuleActorCapped (makeMirrorRippleActor (clickX, spineY, topY, botY,
                                                             dialGeom.leftX, dialGeom.rightX), 6);
            break;
        }
    }
}

// ==========================================================
// 右上角 LED 信号灯（按 SkinCraft 形态自动切换像素方灯 / 现代圆灯 / 绿幕抠像）
// ==========================================================
void VuMeterModule::drawLed (juce::Graphics& g, juce::Rectangle<int> ledArea) const
{
    if (ledArea.getWidth() < 6 || ledArea.getHeight() < 6) return;

    const auto& tokens   = PinkXP::getSkinTokens();
    const bool  hasSignal = ledLevelDb > ledSignalDbfs;
    const bool  danger    = ledLevelDb >= warnDbfs;

    if (tokens.isChromaKey && ! hasSignal)
        return;

    const int sz = juce::jmin (ledArea.getWidth(), ledArea.getHeight());
    auto square = juce::Rectangle<int> (ledArea.getRight() - sz,
                                        ledArea.getY(),
                                        sz, sz).reduced (2);
    if (square.getWidth() < 4 || square.getHeight() < 4)
        return;

    juce::Colour coreCol;
    float pulse = 0.5f;

    if (! hasSignal)
    {
        coreCol = juce::Colour (0xff30d86a);
    }
    else if (danger)
    {
        const float phaseFast = pulsePhase * 2.0f;
        pulse = 0.55f + 0.45f * std::sin (phaseFast);
        const auto alertBase = vuAlertColour();
        coreCol = alertBase.interpolatedWith (juce::Colour (0xffffc8d0), 0.18f * pulse);
    }
    else
    {
        pulse = 0.6f + 0.4f * std::sin (pulsePhase);
        coreCol = juce::Colour (0xff30d86a).interpolatedWith (juce::Colour (0xffc8ffd4), 0.15f * pulse);
    }

    if (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel)
    {
        if (! tokens.isChromaKey)
        {
            // Win95 像素硬件嵌入式方灯凹槽
            const auto bezel = square.expanded (1);
            g.setColour (PinkXP::shdw);
            g.fillRect (bezel.getX(), bezel.getY(), bezel.getWidth(), 1);
            g.fillRect (bezel.getX(), bezel.getY(), 1, bezel.getHeight());
            g.setColour (PinkXP::hl);
            g.fillRect (bezel.getX(), bezel.getBottom() - 1, bezel.getWidth(), 1);
            g.fillRect (bezel.getRight() - 1, bezel.getY(), 1, bezel.getHeight());

            if (! hasSignal)
            {
                g.setColour (juce::Colour (0xff1d2024));
                g.fillRect (square);
            }
        }
        // 锐利像素方形 LED（在绿幕 / 透明模式下仍继承 Pixel XP 方形像素工艺，仅跳过外凹槽）
        PinkXP::Craft::drawStatusLed (g, square, coreCol, hasSignal, false);
        return;
    }

    if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
    {
        const float cx = (float) square.getCentreX();
        const float cy = (float) square.getCentreY();
        const float r  = juce::jmax (2.0f, (float) square.getWidth() * 0.5f - 1.0f);
        const juce::Rectangle<float> ledRect (cx - r, cy - r, r * 2.0f, r * 2.0f);

        if (! tokens.isChromaKey)
        {
            // CNC 倒角铬银外圈（左上镜面亮沿 + 右下暗切边）
            const float rimR = r + 1.4f;
            const juce::Rectangle<float> rimRect (cx - rimR, cy - rimR, rimR * 2.0f, rimR * 2.0f);
            juce::ColourGradient rimGrad (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f),
                                          rimRect.getX(), rimRect.getY(),
                                          PinkXP::dark,
                                          rimRect.getRight(), rimRect.getBottom(), false);
            rimGrad.addColour (0.48, PinkXP::face.interpolatedWith (PinkXP::shdw, 0.35f));
            g.setGradientFill (rimGrad);
            g.fillEllipse (rimRect);
            g.setColour (PinkXP::dark.withAlpha (0.72f));
            g.drawEllipse (rimRect, 0.85f);

            if (! hasSignal)
            {
                g.setColour (juce::Colour (0xff1c1e24));
                g.fillEllipse (ledRect);
                g.setColour (juce::Colours::white.withAlpha (0.14f));
                g.drawEllipse (ledRect.reduced (0.4f), 0.7f);
                return;
            }
        }

        PinkXP::Craft::drawStatusLed (g, ledRect, coreCol, hasSignal, ! PinkXP::isGreenScreenBackdrop());
        if (! tokens.isChromaKey)
        {
            g.setColour (PinkXP::dark.withAlpha (0.65f));
            g.drawEllipse (ledRect, 0.85f);
        }
        return;
    }

    if (tokens.dialFinish == PinkXP::DialFinish::studioSmooth && ! tokens.isChromaKey)
    {
        // 保持 Modern Minimal 经典圆形高光信号灯（零回归）
        const float cx = (float) square.getCentreX();
        const float cy = (float) square.getCentreY();
        const float r  = (float) square.getWidth() * 0.5f - 1.0f;
        if (r > 0.5f)
        {
            const juce::Rectangle<float> ledRect (cx - r, cy - r, r * 2.0f, r * 2.0f);
            if (! hasSignal)
            {
                g.setColour (juce::Colour (0xff2a2a2a));
                g.fillEllipse (ledRect);
                g.setColour (PinkXP::ink);
                g.drawEllipse (ledRect, 1.2f);
            }
            else
            {
                PinkXP::Craft::drawStatusLed (g, ledRect, coreCol, true, false);
                g.setColour (PinkXP::ink.withAlpha (0.78f));
                g.drawEllipse (ledRect, 1.0f);
            }
        }
        return;
    }

    const bool allowGlow = ! PinkXP::isGreenScreenBackdrop();
    if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass
     || tokens.frameStyle == PinkXP::FrameStyle::neonTube)
    {
        PinkXP::Craft::drawStatusLed (g, square.toFloat().reduced (0.5f), coreCol, hasSignal, allowGlow);
        return;
    }

    PinkXP::Craft::drawStatusLed (g, square, coreCol, hasSignal, allowGlow);
}

// ==========================================================
// 主绘制入口：外框 + 凹陷底板 + 分派至指针/条形渲染器 + 信号灯
// ==========================================================
void VuMeterModule::paintContent (juce::Graphics& g, juce::Rectangle<int> content)
{
    const auto& tokens   = PinkXP::getSkinTokens();
    const bool  chromaKey = tokens.isChromaKey;

    if (chromaKey)
    {
        if (PinkXP::isGreenScreenBackdrop())
        {
            g.setColour (PinkXP::chromaKeyGreen());
            g.fillRect (content);
        }
        // Transparent Overlay (PinkXP::isTransparentBackdrop()): skip opaque fill so live
        // needles, Dual Arc trails, lit LED/bar segments, peak markers & dB readouts float
        // cleanly over pure alpha = 0.
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
    {
        juce::ColourGradient plateGrad (PinkXP::face.interpolatedWith (juce::Colours::white, 0.10f),
                                        0.0f, (float) content.getY(),
                                        PinkXP::face.darker (0.06f),
                                        0.0f, (float) content.getBottom(), false);
        plateGrad.addColour (0.45, PinkXP::face);
        g.setGradientFill (plateGrad);
        g.fillRect (content);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
    {
        const bool darkFace = PinkXP::face.getPerceivedBrightness() < 0.45f;
        juce::ColourGradient glassGrad (PinkXP::face.interpolatedWith (juce::Colours::white, darkFace ? 0.10f : 0.28f),
                                        0.0f, (float) content.getY(),
                                        PinkXP::face.interpolatedWith (juce::Colours::black, darkFace ? 0.08f : 0.03f),
                                        0.0f, (float) content.getBottom(), false);
        g.setGradientFill (glassGrad);
        g.fillRect (content);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
    {
        const bool darkFace = PinkXP::face.getPerceivedBrightness() < 0.45f;
        g.setColour (darkFace ? PinkXP::face.darker (0.28f)
                              : PinkXP::face);
        g.fillRect (content);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
    {
        const bool darkFace = PinkXP::face.getPerceivedBrightness() < 0.45f;
        g.setColour (darkFace ? PinkXP::face.darker (0.18f) : PinkXP::face.darker (0.06f));
        g.fillRect (content);
    }
    else
    {
        g.setColour (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                         ? PinkXP::btnFace
                         : PinkXP::face);
        g.fillRect (content);
    }

    auto area = content.reduced (6);
    if (area.getWidth() < 40 || area.getHeight() < 40) return;

    if (! chromaKey)
        PinkXP::Craft::drawPanelWell (g, area, PinkXP::content, PinkXP::WellKind::canvas);

    auto dialArea = area;
    dialGeom = DialGeom{};

    if (! isLinearStyle())
    {
        switch (needleDialStyle_)
        {
            case NeedleDialStyle::vintage:
                dialGeom.angleStart = -fanHalfAngle;
                dialGeom.angleEnd   = +fanHalfAngle;
                break;
            case NeedleDialStyle::dualArc:
                dialGeom.angleStart = -2.0943951f;
                dialGeom.angleEnd   = +2.0943951f;
                break;
            case NeedleDialStyle::retroWarm:
                dialGeom.angleStart = -0.92f;
                dialGeom.angleEnd   = +0.92f;
                break;
            case NeedleDialStyle::studio:
            default:
                dialGeom.angleStart = -juce::MathConstants<float>::halfPi;
                dialGeom.angleEnd   = +juce::MathConstants<float>::halfPi;
                break;
        }

        switch (needleDialStyle_)
        {
            case NeedleDialStyle::vintage:   drawNeedleVintageDial   (g, dialArea); break;
            case NeedleDialStyle::dualArc:   drawNeedleDualArcDial   (g, dialArea); break;
            case NeedleDialStyle::retroWarm: drawNeedleRetroWarmDial (g, dialArea); break;
            case NeedleDialStyle::studio:
            default:                         drawNeedleStudioDial    (g, dialArea); break;
        }

        drawNeedleOverlay (g, dialArea);
    }
    else
    {
        switch (barDialStyle_)
        {
            case BarDialStyle::ledBlocks: drawBarLedBlocksDial (g, dialArea); break;
            case BarDialStyle::ledLadder: drawBarLedLadderDial (g, dialArea); break;
            case BarDialStyle::mirrored:  drawBarMirrorDial    (g, dialArea); break;
            case BarDialStyle::minimal:   drawBarMinimalDial   (g, dialArea); break;
            case BarDialStyle::studioBar:
            default:                      drawBarStudioDial    (g, dialArea); break;
        }
    }

    if (showLed)
        drawLed (g, ledBounds (area));

    if (! chromaKey)
        PinkXP::Craft::drawCanvasFinishOverlay (g, area);
}

// ==========================================================
// 持久化（含旧版 dialStyle/needleStyle 存档无缝迁移）
// ==========================================================
juce::ValueTree VuMeterModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("meterType",       (int) meterType_,       nullptr);
    s.setProperty ("needleDialStyle", (int) needleDialStyle_, nullptr);
    s.setProperty ("barDialStyle",    (int) barDialStyle_,    nullptr);
    s.setProperty ("colorMap",        (int) colorMap_,        nullptr);
    s.setProperty ("needleShape",     (int) needleShape_,     nullptr);
    s.setProperty ("barDensity",      (int) barDensity_,      nullptr);
    s.setProperty ("barFinish",       (int) barFinish_,       nullptr);
    s.setProperty ("channels",        (int) channelMode_,     nullptr);
    s.setProperty ("showLed",         showLed,                nullptr);
    s.setProperty ("showReadout",     showReadout_,           nullptr);
    s.setProperty ("peakHold",        peakHold,               nullptr);
    s.setProperty ("backlight",       backlight_,             nullptr);
    s.setProperty ("ballistics",      ballistics,             nullptr);
    s.setProperty ("rmsWindowMs",     (double) rmsWindowMs,   nullptr);
    s.setProperty ("peakHoldSec",     (double) peakHoldSec_,  nullptr);
    s.setProperty ("trimDb",          (double) trimDb,        nullptr);
    s.setProperty ("scaleRange",      (int) scaleRange_,      nullptr);
    s.setProperty ("warnDb",          (double) warnDbfs,      nullptr);
    return s;
}

void VuMeterModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    if (! state.isValid())
        return;

    if (! state.hasProperty ("meterType"))
    {
        // 旧版 XML 存档迁移（dialStyle 0..5 + needleStyle 0..2）
        if (state.hasProperty ("dialStyle"))
        {
            const int legacyDial = juce::jlimit (0, 5, (int) state.getProperty ("dialStyle"));
            switch (legacyDial)
            {
                case 0: meterType_ = MeterType::needle; needleDialStyle_ = NeedleDialStyle::studio; break;
                case 1: meterType_ = MeterType::needle; needleDialStyle_ = NeedleDialStyle::vintage; break;
                case 2: meterType_ = MeterType::bar;    barDialStyle_    = BarDialStyle::studioBar; break;
                case 3: meterType_ = MeterType::bar;    barDialStyle_    = BarDialStyle::ledLadder; break;
                case 4: meterType_ = MeterType::bar;    barDialStyle_    = BarDialStyle::minimal; break;
                case 5: meterType_ = MeterType::bar;    barDialStyle_    = BarDialStyle::mirrored; break;
                default: break;
            }
        }
        if (state.hasProperty ("needleStyle"))
        {
            const int legacyNeedle = juce::jlimit (0, 2, (int) state.getProperty ("needleStyle"));
            switch (legacyNeedle)
            {
                case 0: channelMode_ = ChannelMode::mono;   needleShape_ = NeedleShape::thin; break;
                case 1: channelMode_ = ChannelMode::mono;   needleShape_ = NeedleShape::tapered; break;
                case 2: channelMode_ = ChannelMode::stereo; needleShape_ = NeedleShape::thin; break;
                default: break;
            }
        }
    }
    else
    {
        meterType_ = (MeterType) juce::jlimit (0, 1, (int) state.getProperty ("meterType"));
    }

    if (state.hasProperty ("needleDialStyle"))
        needleDialStyle_ = (NeedleDialStyle) juce::jlimit (0, 3, (int) state.getProperty ("needleDialStyle"));
    if (state.hasProperty ("barDialStyle"))
        barDialStyle_ = (BarDialStyle) juce::jlimit (0, 4, (int) state.getProperty ("barDialStyle"));
    if (state.hasProperty ("colorMap"))
        colorMap_ = (ColorMap) juce::jlimit (0, 4, (int) state.getProperty ("colorMap"));
    if (state.hasProperty ("needleShape"))
        needleShape_ = (NeedleShape) juce::jlimit (0, 2, (int) state.getProperty ("needleShape"));
    if (state.hasProperty ("barDensity"))
        barDensity_ = (BarDensity) juce::jlimit (0, 2, (int) state.getProperty ("barDensity"));
    if (state.hasProperty ("barFinish"))
        barFinish_ = (BarFinish) juce::jlimit (0, 2, (int) state.getProperty ("barFinish"));
    if (state.hasProperty ("channels"))
        channelMode_ = (ChannelMode) juce::jlimit (0, 3, (int) state.getProperty ("channels"));
    if (state.hasProperty ("showLed"))
        showLed = (bool) state.getProperty ("showLed");
    if (state.hasProperty ("showReadout"))
        showReadout_ = (bool) state.getProperty ("showReadout");
    if (state.hasProperty ("peakHold"))
    {
        peakHold = (bool) state.getProperty ("peakHold");
        if (! peakHold)
        {
            peakHoldL = peakHoldR = peakHoldMono = -144.0f;
            peakHoldTimerL = peakHoldTimerR = peakHoldTimerM = 0.0f;
        }
    }
    if (state.hasProperty ("backlight"))
        backlight_ = (bool) state.getProperty ("backlight");
    if (state.hasProperty ("ballistics"))
        ballistics = juce::jlimit (0, 3, (int) state.getProperty ("ballistics"));
    if (state.hasProperty ("rmsWindowMs"))
        rmsWindowMs = juce::jlimit (10.0f, 40.0f, (float) (double) state.getProperty ("rmsWindowMs"));
    if (state.hasProperty ("peakHoldSec"))
        peakHoldSec_ = juce::jlimit (0.5f, 4.0f, (float) (double) state.getProperty ("peakHoldSec"));
    if (state.hasProperty ("trimDb"))
        trimDb = juce::jlimit (-12.0f, 12.0f, (float) (double) state.getProperty ("trimDb"));
    if (state.hasProperty ("scaleRange"))
        scaleRange_ = (ScaleRange) juce::jlimit (0, 3, (int) state.getProperty ("scaleRange"));
    if (state.hasProperty ("warnDb"))
        warnDbfs = juce::jlimit (-6.0f, 0.0f, (float) (double) state.getProperty ("warnDb"));

    refreshControlPanel();
    repaint();
}

// ==========================================================
// 控制面板参数（4 标签页：Style / Meter / Ballistics / Scale）
// ==========================================================
void VuMeterModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    // ---- Group 1: Style ----
    out.push_back (ParamDef::choice ("meterType", "Meter type",
        { "Needle", "Bar" },
        "Switch between analog swinging-needle VU dials and modern horizontal bar meters.")
        .in ("Style"));
    out.push_back (ParamDef::choice ("needleDialStyle", "Dial style",
        { "Studio", "Vintage", "Dual arc", "Retro warm" },
        "Studio: classic 180-degree semicircle dial. "
        "Vintage: narrow fan-shaped hardware window with percent sub-scale. "
        "Dual arc: wide 240-degree illuminated ring gauge. "
        "Retro warm: classic backlit broadcast VU unit.")
        .in ("Style").when ([this] { return meterType_ == MeterType::needle; }));
    out.push_back (ParamDef::choice ("barDialStyle", "Bar style",
        { "Studio bar", "LED blocks", "LED ladder", "Mirrored", "Minimal" },
        "Studio bar: continuous horizontal level bar. "
        "LED blocks: segmented hardware LED brick row. "
        "LED ladder: stepped rising-height LED columns. "
        "Mirrored: symmetrical dual bars flanking a center dB spine. "
        "Minimal: oversized digital dB readout over a slim ribbon.")
        .in ("Style").when ([this] { return meterType_ == MeterType::bar; }));
    out.push_back (ParamDef::choice ("colorMap", "Color map",
        { "Theme", "Studio", "Warm VU", "MiniMeter", "Ice" },
        "Color palette for arcs, bars and glow accents. "
        "Theme adapts to the active skin; Studio uses classic green-amber-red broadcast zones.")
        .in ("Style"));
    out.push_back (ParamDef::choice ("needleShape", "Needle",
        { "Thin", "Tapered", "Glow" },
        "Visual shape of the analog pointer: crisp hairline, classic tapered spear, or luminous neon glow.")
        .in ("Style").when ([this] { return meterType_ == MeterType::needle; }));
    out.push_back (ParamDef::choice ("barDensity", "Density",
        { "Chunky", "Medium", "Fine" },
        "LED segment density for LED blocks and LED ladder styles.")
        .in ("Style").when ([this]
        {
            return meterType_ == MeterType::bar
                && (barDialStyle_ == BarDialStyle::ledBlocks
                    || barDialStyle_ == BarDialStyle::ledLadder);
        }));
    out.push_back (ParamDef::choice ("barFinish", "Finish",
        { "Smooth", "Notched", "Glow" },
        "Surface styling for continuous horizontal bars: smooth gradient, broadcast notched divisions, or luminous neon bloom.")
        .in ("Style").when ([this]
        {
            return meterType_ == MeterType::bar
                && (barDialStyle_ == BarDialStyle::studioBar
                    || barDialStyle_ == BarDialStyle::mirrored);
        }));

    // ---- Group 2: Meter ----
    out.push_back (ParamDef::choice ("channels", "Channels",
        { "Mono (L+R)", "Stereo L/R", "Left (L)", "Right (R)" },
        "Mono (L+R): power-summed overall level. "
        "Stereo L/R: independent coaxial dual needles (or dual stereo bars) for L and R. "
        "Left (L) / Right (R): monitor a single stereo channel.")
        .in ("Meter"));
    out.push_back (ParamDef::toggle ("showLed", "Signal LED",
        "Round lamp in the corner: dark = no signal, green = signal, red = at or above the warning threshold.")
        .in ("Meter"));
    out.push_back (ParamDef::toggle ("showReadout", "dB readout",
        "Show a compact live digital dB value readout on the meter face.")
        .in ("Meter").when ([this]
        {
            return ! (meterType_ == MeterType::bar && barDialStyle_ == BarDialStyle::minimal);
        }));
    out.push_back (ParamDef::toggle ("peakHold", "Peak hold",
        "Show a slowly falling ghost needle or tick at the recent peak level.")
        .in ("Meter"));
    out.push_back (ParamDef::toggle ("backlight", "Backlight",
        "Soft illuminated dial plate glow and subtle glass sheen on analog needle meters.")
        .in ("Meter").when ([this] { return meterType_ == MeterType::needle; }));

    // ---- Group 3: Ballistics ----
    out.push_back (ParamDef::choice ("ballistics", "Response",
        { "Slow", "Classic", "Fast", "Snappy" },
        "Needle inertia and decay speed. Slow feels heavy and vintage; Classic is the default; Fast and Snappy chase drum transients.")
        .in ("Ballistics"));
    out.push_back (ParamDef::slider ("rmsWindowMs", "RMS window", 10.0, 40.0, 5.0, " ms",
        "Integration window length for the instant RMS detector. Shorter reacts crisper; longer smooths bass ripple.")
        .in ("Ballistics"));
    out.push_back (ParamDef::slider ("peakHoldSec", "Hold time", 0.5, 4.0, 0.5, " s",
        "How long peak-hold markers stay frozen before smoothly decaying.")
        .in ("Ballistics").when ([this] { return peakHold; }));

    // ---- Group 4: Scale ----
    out.push_back (ParamDef::slider ("trimDb", "Input trim", -12.0, 12.0, 1.0, " dB",
        "Visual calibration trim added to the VU reading so quieter mixes swing right in the sweet spot.")
        .in ("Scale"));
    out.push_back (ParamDef::choice ("scaleRange", "Scale range",
        { "-24..+3 dB", "-36..+3 dB", "-48..+3 dB", "-60..0 dB" },
        "Displayed dBFS dynamic range from the left floor to the right ceiling of the meter scale.")
        .in ("Scale"));
    out.push_back (ParamDef::choice ("warnDb", "Red zone",
        { "-6 dB", "-3 dB", "-1 dB", "0 dB" },
        "Threshold where the red overload zone and red warning LED activate.")
        .in ("Scale"));
}

double VuMeterModule::getParamValue (const juce::String& id) const
{
    if (id == "meterType")       return (double) (int) meterType_;
    if (id == "needleDialStyle") return (double) (int) needleDialStyle_;
    if (id == "barDialStyle")    return (double) (int) barDialStyle_;
    if (id == "colorMap")        return (double) (int) colorMap_;
    if (id == "needleShape")     return (double) (int) needleShape_;
    if (id == "barDensity")      return (double) (int) barDensity_;
    if (id == "barFinish")       return (double) (int) barFinish_;
    if (id == "channels")        return (double) (int) channelMode_;
    if (id == "showLed")         return showLed ? 1.0 : 0.0;
    if (id == "showReadout")     return showReadout_ ? 1.0 : 0.0;
    if (id == "peakHold")        return peakHold ? 1.0 : 0.0;
    if (id == "backlight")       return backlight_ ? 1.0 : 0.0;
    if (id == "ballistics")      return (double) ballistics;
    if (id == "rmsWindowMs")     return (double) rmsWindowMs;
    if (id == "peakHoldSec")     return (double) peakHoldSec_;
    if (id == "trimDb")          return (double) trimDb;
    if (id == "scaleRange")      return (double) (int) scaleRange_;
    if (id == "warnDb")
    {
        if (warnDbfs <= -4.5f) return 0.0;
        if (warnDbfs <= -2.0f) return 1.0;
        if (warnDbfs <= -0.5f) return 2.0;
        return 3.0;
    }
    return 0.0;
}

void VuMeterModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "meterType")       meterType_       = (MeterType)       juce::jlimit (0, 1, juce::roundToInt (v));
    else if (id == "needleDialStyle") needleDialStyle_ = (NeedleDialStyle) juce::jlimit (0, 3, juce::roundToInt (v));
    else if (id == "barDialStyle")    barDialStyle_    = (BarDialStyle)    juce::jlimit (0, 4, juce::roundToInt (v));
    else if (id == "colorMap")        colorMap_        = (ColorMap)        juce::jlimit (0, 4, juce::roundToInt (v));
    else if (id == "needleShape")     needleShape_     = (NeedleShape)     juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "barDensity")      barDensity_      = (BarDensity)      juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "barFinish")       barFinish_       = (BarFinish)       juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "channels")        channelMode_     = (ChannelMode)     juce::jlimit (0, 3, juce::roundToInt (v));
    else if (id == "showLed")         showLed          = v >= 0.5;
    else if (id == "showReadout")     showReadout_     = v >= 0.5;
    else if (id == "peakHold")
    {
        peakHold = v >= 0.5;
        if (! peakHold)
        {
            peakHoldL = peakHoldR = peakHoldMono = -144.0f;
            peakHoldTimerL = peakHoldTimerR = peakHoldTimerM = 0.0f;
        }
    }
    else if (id == "backlight")       backlight_       = v >= 0.5;
    else if (id == "ballistics")      ballistics       = juce::jlimit (0, 3, juce::roundToInt (v));
    else if (id == "rmsWindowMs")     rmsWindowMs      = juce::jlimit (10.0f, 40.0f, (float) v);
    else if (id == "peakHoldSec")     peakHoldSec_     = juce::jlimit (0.5f, 4.0f, (float) v);
    else if (id == "trimDb")          trimDb           = juce::jlimit (-12.0f, 12.0f, (float) v);
    else if (id == "scaleRange")      scaleRange_      = (ScaleRange)      juce::jlimit (0, 3, juce::roundToInt (v));
    else if (id == "warnDb")
    {
        static constexpr float kWarnOpts[4] = { -6.0f, -3.0f, -1.0f, 0.0f };
        warnDbfs = kWarnOpts[juce::jlimit (0, 3, juce::roundToInt (v))];
    }
    else
        return;

    repaint();
}

#include "source/ui/modules/VuNeedleRenderer.inl"
#include "source/ui/modules/VuBarRenderer.inl"
