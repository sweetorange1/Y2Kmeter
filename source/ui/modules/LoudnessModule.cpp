#include "source/ui/modules/LoudnessModule.h"
#include "source/ui/modules/MeterWidgets.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"
#include "source/steam/Y2KAchievements.h"
#include <cmath>

// ==========================================================
// LoudnessModule（响度模块）
// ==========================================================
LoudnessModule::LoudnessModule(AnalyserHub& h)
    : ModulePanel(ModuleType::loudness),
      hub(h)
{
    // 订阅 Loudness + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Loudness);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(320, 256); // loudness 5×4 大格
    setControlPanelEnabled (true);

    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        invalidateStaticLayer();
        repaint();
    });

    barM.label = "M";
    barS.label = "S";
    barI.label = "I";
    barTpL.label = "TL";
    barTpR.label = "TR";

    rebuildBarSlots();
}

LoudnessModule::~LoudnessModule()
{
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = -1;
    }

    // 先解绑 FrameListener（避免在析构过程中再被回调）
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Loudness);
}

// ==========================================================
// 持久化 / 控制面板
// ==========================================================
juce::ValueTree LoudnessModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("style",   (int) style, nullptr);
    s.setProperty ("itemM",   items.m,   nullptr);
    s.setProperty ("itemS",   items.s,   nullptr);
    s.setProperty ("itemI",   items.i,   nullptr);
    s.setProperty ("itemTp",       items.tp,  nullptr);
    s.setProperty ("itemRms",      items.rms, nullptr);
    s.setProperty ("targetLufs",   targetPreset, nullptr);
    s.setProperty ("floorDb",      (double) floorDb, nullptr);
    s.setProperty ("peakHold",     showPeakHold, nullptr);
    s.setProperty ("peakHoldSec",  (double) peakHoldSec, nullptr);
    return s;
}

void LoudnessModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    auto getB = [&] (const char* k, bool& dst)
    {
        if (state.hasProperty (k)) dst = (bool) state.getProperty (k);
    };

    Items it = items;
    getB ("itemM", it.m); getB ("itemS", it.s); getB ("itemI", it.i);
    getB ("itemTp", it.tp); getB ("itemRms", it.rms);
    if (! it.any()) it = Items();

    items = it;
    if (state.hasProperty ("style"))
        style = (int) state.getProperty ("style") == 1 ? Style::readouts : Style::ledBars;
    if (state.hasProperty ("targetLufs"))
        targetPreset = juce::jlimit (0, 5, (int) state.getProperty ("targetLufs"));
    if (state.hasProperty ("floorDb"))
        floorDb = juce::jlimit (-72.0f, -36.0f, (float) (double) state.getProperty ("floorDb"));
    if (state.hasProperty ("peakHold"))
        showPeakHold = (bool) state.getProperty ("peakHold");
    if (state.hasProperty ("peakHoldSec"))
        peakHoldSec = juce::jlimit (0.5f, 5.0f, (float) (double) state.getProperty ("peakHoldSec"));

    rebuildBarSlots();
    invalidateStaticLayer();
    layoutContent (getContentBounds());
    refreshControlPanel();
    repaint();
}

void LoudnessModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("style", "Display style",
        { "LED bars", "Number readouts" },
        "LED bars: pixel meters with a dB scale. Number readouts: big numbers with a progress bar.")
        .in ("Style"));

    out.push_back (ParamDef::toggle ("itemM", "Momentary",
        "Loudness over the last 400 ms - reacts fastest.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemS", "Short-term",
        "Loudness over the last 3 s - the usual main number.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemI", "Integrated",
        "Gated average over the whole programme. Resets after a long silence.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemTp", "True Peak",
        "Inter-sample peak (4x oversampled). LED bars show L and R, readouts show the louder one.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemRms", "RMS",
        "Average level of the left and right channel. Readouts show the louder one.").in ("Show"));

    out.push_back (ParamDef::choice ("targetLufs", "Target LUFS",
        { "Off", "-23 EBU", "-16 Apple", "-14 Stream", "-9 Club", "-6 Loud" },
        "Draws a mastering loudness target reference line across LUFS meters.")
        .in ("Scale"));
    out.push_back (ParamDef::choice ("floorDb", "Meter floor",
        { "-72 dB", "-60 dB", "-48 dB", "-36 dB" },
        "Bottom of the meter scale. Higher floor zooms into mastering headroom; lower floor reveals quiet ambience.")
        .in ("Scale"));
    out.push_back (ParamDef::toggle ("peakHold", "Peak hold",
        "Show the slowly falling peak-hold marker on each LED bar.")
        .in ("Scale").when ([this] { return style == Style::ledBars; }));
    out.push_back (ParamDef::slider ("peakHoldSec", "Peak hold time", 0.5, 5.0, 0.5, " s",
        "How long the peak marker stays before falling.")
        .in ("Scale").when ([this] { return style == Style::ledBars && showPeakHold; }));
}

double LoudnessModule::getParamValue (const juce::String& id) const
{
    if (id == "style")       return (double) (int) style;
    if (id == "itemM")       return items.m ? 1.0 : 0.0;
    if (id == "itemS")       return items.s ? 1.0 : 0.0;
    if (id == "itemI")       return items.i ? 1.0 : 0.0;
    if (id == "itemTp")      return items.tp ? 1.0 : 0.0;
    if (id == "itemRms")     return items.rms ? 1.0 : 0.0;
    if (id == "targetLufs")  return (double) targetPreset;
    if (id == "floorDb")
    {
        if (floorDb <= -66.0f) return 0.0;
        if (floorDb <= -54.0f) return 1.0;
        if (floorDb <= -42.0f) return 2.0;
        return 3.0;
    }
    if (id == "peakHold")    return showPeakHold ? 1.0 : 0.0;
    if (id == "peakHoldSec") return (double) peakHoldSec;
    return 0.0;
}

void LoudnessModule::setParamValue (const juce::String& id, double v)
{
    if (id == "style")
    {
        style = juce::roundToInt (v) == 1 ? Style::readouts : Style::ledBars;
        layoutContent (getContentBounds());
        repaint();
        return;
    }
    if (id == "targetLufs")
    {
        targetPreset = juce::jlimit (0, 5, juce::roundToInt (v));
        invalidateStaticLayer();
        repaint();
        return;
    }
    if (id == "floorDb")
    {
        static constexpr float kFloors[4] = { -72.0f, -60.0f, -48.0f, -36.0f };
        floorDb = kFloors[juce::jlimit (0, 3, juce::roundToInt (v))];
        invalidateStaticLayer();
        repaint();
        return;
    }
    if (id == "peakHold")
    {
        showPeakHold = (v >= 0.5);
        repaint();
        return;
    }
    if (id == "peakHoldSec")
    {
        peakHoldSec = juce::jlimit (0.5f, 5.0f, (float) v);
        return;
    }

    const bool on = v >= 0.5;
    Items it = items;
    if      (id == "itemM")   it.m = on;
    else if (id == "itemS")   it.s = on;
    else if (id == "itemI")   it.i = on;
    else if (id == "itemTp")  it.tp = on;
    else if (id == "itemRms") it.rms = on;
    else return;

    if (! it.any())
        return;   // 至少保留一项

    items = it;
    rebuildBarSlots();
    invalidateStaticLayer();
    layoutContent (getContentBounds());
    repaint();
}

void LoudnessModule::rebuildBarSlots()
{
    slots.clear();
    if (items.m) slots.push_back ({ &barM, true });
    if (items.s) slots.push_back ({ &barS, true });
    if (items.i) slots.push_back ({ &barI, true });
    if (items.tp)
    {
        slots.push_back ({ &barTpL, false });
        slots.push_back ({ &barTpR, false });
    }
    if (items.rms)
    {
        // 开着 True Peak 时 RMS 改叫 RL / RR，避免和 TL / TR 混淆；单独 RMS 时沿用旧版的 L / R
        barRmsL.label = items.tp ? "RL" : "L";
        barRmsR.label = items.tp ? "RR" : "R";
        slots.push_back ({ &barRmsL, false });
        slots.push_back ({ &barRmsR, false });
    }
}

// ==========================================================
// onFrame —— Hub 分发器 UI 线程回调
// ==========================================================
void LoudnessModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Loudness))
        return; // 本帧 Loudness 路没有活跃

    const float dt = frame.dtSec;
    const auto& snap = frame.loudness;

    if (style == Style::ledBars)
    {
        const float deltaMs = dt * 1000.0f;
        const float holdMs  = peakHoldSec * 1000.0f;
        if (items.m)   barM.update(snap.lufsM, deltaMs, holdMs);
        if (items.s)   barS.update(snap.lufsS, deltaMs, holdMs);
        if (items.i)   barI.update(snap.lufsI, deltaMs, holdMs);
        if (items.tp)  { barTpL.update(snap.truePeakL, deltaMs, holdMs); barTpR.update(snap.truePeakR, deltaMs, holdMs); }
        if (items.rms) { barRmsL.update(snap.rmsL, deltaMs, holdMs);     barRmsR.update(snap.rmsR, deltaMs, holdMs); }
    }
    else
    {
        using namespace y2k::pacing;
        const float prev[numReadRows] = { rdValue[rM], rdValue[rS], rdValue[rI], rdValue[rTp], rdValue[rRms] };

        auto sm = [&] (float cur, float target, float a)
        {
            if (! std::isfinite (target)) target = -144.0f;
            return cur + alphaForDt (a, dt) * (target - cur);
        };
        rdValue[rM] = sm (rdValue[rM], snap.lufsM, 0.30f);
        rdValue[rS] = sm (rdValue[rS], snap.lufsS, 0.20f);
        rdValue[rI] = sm (rdValue[rI], snap.lufsI, 0.08f);

        rmsSmoothL = sm (rmsSmoothL, snap.rmsL, 0.12f);
        rmsSmoothR = sm (rmsSmoothR, snap.rmsR, 0.12f);
        rdValue[rRms] = juce::jmax (rmsSmoothL, rmsSmoothR);

        // True Peak：短保持 + 自动回落（0.5 s 保持，之后 0.28 dB / 120Hz 帧）
        const float tpInstantMax = juce::jmax (snap.truePeakL, snap.truePeakR);
        constexpr float holdSec = framesToSec (60.0f);
        if (tpInstantMax >= rdValue[rTp] - 0.0001f)
        {
            rdValue[rTp] = tpInstantMax;
            tpHoldSec = holdSec;
        }
        else if (tpHoldSec > 0.0f)
        {
            tpHoldSec -= dt;
        }
        else
        {
            rdValue[rTp] = juce::jmax (tpInstantMax, rdValue[rTp] - stepForDt (0.28f, dt));
        }

        const float th = juce::jmax (0.05f, dt * kRefHz);
        const float thr[numReadRows] = { 0.3f, 0.3f, 0.2f, 0.3f, 0.3f };
        const float dec[numReadRows] = { 0.08f, 0.08f, 0.06f, 0.08f, 0.08f };
        for (int r = 0; r < numReadRows; ++r)
        {
            if (std::abs (rdValue[r] - prev[r]) > thr[r] * th) rdFlash[r] = 1.0f;
            rdFlash[r] = juce::jmax (0.0f, rdFlash[r] - stepForDt (dec[r], dt));
        }

        pulsePhase += stepForDt (0.24f, dt);
        while (pulsePhase > juce::MathConstants<float>::twoPi)
            pulsePhase -= juce::MathConstants<float>::twoPi;
    }

    // 成就：响度达标的判定在 AnalyserHub::dispatch()（fanout 之前统一判一次）。
    //   ⚠️ 这里不要再调用，否则会重复累计。

    // v2.9.2：读数类模块 30Hz 重绘（数据每帧更新，仅重绘分频）
    if ((frame.tickCount & 1ull) != 0)
        return;
    repaintFrameContent();
}

// ==========================================================
// 值 → 柱高 / 颜色
// ==========================================================
float LoudnessModule::valueToNorm(float val) const noexcept
{
    return juce::jlimit(0.0f, 1.0f, juce::jmap(val, floorDb, 0.0f, 0.0f, 1.0f));
}

float LoudnessModule::targetLufsDb() const noexcept
{
    static constexpr float kTargets[6] = { -999.0f, -23.0f, -16.0f, -14.0f, -9.0f, -6.0f };
    return kTargets[juce::jlimit (0, 5, targetPreset)];
}

juce::Colour LoudnessModule::lufsToColour(float lufs) noexcept { return y2k::meter::lufsColour (lufs); }
juce::Colour LoudnessModule::dbToColour(float db) noexcept     { return y2k::meter::dbColour (db); }

// ==========================================================
// drawMeterBar —— 绘制单条像素柱状表
// ==========================================================
void LoudnessModule::drawMeterBar(juce::Graphics& g,
                                   juce::Rectangle<int> barArea,
                                   const MeterBar& bar,
                                   bool isLUFS) const
{
    const auto& tokens    = PinkXP::getSkinTokens();
    const bool  chromaKey = tokens.isChromaKey;
    const bool  hasStatic = (! chromaKey && staticLayer.isValid());

    // 非静态层路径（无缓冲回退）：绘制凹槽；有静态层时静态层已画完凹槽 + 暗格，避免双画
    if (! hasStatic)
        PinkXP::Craft::drawPanelWell (g, barArea, PinkXP::content.darker (0.05f), PinkXP::WellKind::trough);

    const int padding = 3;
    auto inner = barArea.reduced(padding);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int step = cellSize + cellGap;
    const int numRows = juce::jmax(1, (inner.getHeight() + cellGap) / step);

    const float norm = valueToNorm(bar.smoothed);
    const int litRows = juce::jlimit (0, numRows, (int)std::round(norm * (float)numRows));

    const float peakNorm = valueToNorm(bar.peakValue);
    const int peakRow = (int)std::round(peakNorm * (float)numRows);

    const int totalH = numRows * step - cellGap;
    const int offsetY = inner.getY() + (inner.getHeight() - totalH) / 2;
    // 灯带宽度随柱容器宽度自适应：保留左右各 2px 安全边距
    const int colW = juce::jmax(1, inner.getWidth() - 4);
    const int offsetX = inner.getX() + (inner.getWidth() - colW) / 2;

    if (hasStatic || chromaKey)
    {
        // 静态层已绘制所有暗格，绿幕跳过暗格 → 仅绘制点亮格，零双画
        const int firstLitRow = juce::jmax (0, numRows - litRows);
        for (int row = firstLitRow; row < numRows; ++row)
        {
            const int y = offsetY + row * step;
            const float rowNorm = 1.0f - (float)row / (float)juce::jmax(1, numRows - 1);
            const float rowDb   = juce::jmap(rowNorm, floorDb, 0.0f);
            const auto base = isLUFS ? lufsToColour(rowDb) : dbToColour(rowDb);
            PinkXP::Craft::drawMeterCell (g, juce::Rectangle<int> (offsetX, y, colW, cellSize), base, true);
        }
    }
    else
    {
        for (int row = 0; row < numRows; ++row)
        {
            const int y = offsetY + row * step;
            const bool lit = (numRows - 1 - row) < litRows;
            const float rowNorm = 1.0f - (float)row / (float)juce::jmax(1, numRows - 1);
            const float rowDb   = juce::jmap(rowNorm, floorDb, 0.0f);
            const auto base = isLUFS ? lufsToColour(rowDb) : dbToColour(rowDb);
            PinkXP::Craft::drawMeterCell (g, juce::Rectangle<int> (offsetX, y, colW, cellSize), base, lit);
        }
    }

    // 目标响度参考线（仅 LUFS 柱绘制）
    const float tgt = targetLufsDb();
    if (isLUFS && tgt >= floorDb && tgt <= 0.0f && ! chromaKey)
    {
        const float tgtNorm = valueToNorm (tgt);
        const int tgtY = offsetY + juce::roundToInt ((1.0f - tgtNorm) * (float) totalH);
        if ((tokens.frameStyle == PinkXP::FrameStyle::neonTube
             || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor) && ! PinkXP::isGreenScreenBackdrop())
        {
            g.setColour (PinkXP::sel.withAlpha (0.28f));
            g.fillRect (offsetX - 2, tgtY - 2, colW + 4, 4);
        }
        g.setColour (PinkXP::sel.withAlpha (0.90f));
        if (! tokens.hardPixelSteps && tokens.barCellRadius > 0.5f)
        {
            g.fillRoundedRectangle ((float) (offsetX - 2), (float) tgtY - 0.75f, (float) (colW + 4), 1.5f, 0.75f);
            if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
            {
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f));
                g.fillRect (offsetX - 1, tgtY, colW + 2, 1);
            }
        }
        else
        {
            g.fillRect (offsetX - 2, tgtY - 1, colW + 4, 2);
            if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
            {
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f));
                g.fillRect (offsetX - 2, tgtY - 1, colW + 4, 1);
            }
        }
    }

    // 缓降峰值线
    if (showPeakHold && peakRow > 0 && peakRow <= numRows)
    {
        const int peakY = offsetY + (numRows - peakRow) * step;
        const auto peakCol = (isLUFS ? lufsToColour(bar.peakValue)
                                     : dbToColour (bar.peakValue)).brighter(0.2f);
        const juce::Rectangle<int> peakRect (offsetX - 1, peakY, colW + 2, 2);
        if (tokens.peakCapGlow && ! PinkXP::isGreenScreenBackdrop())
        {
            g.setColour (peakCol.withAlpha (0.28f));
            g.fillRect (peakRect.expanded (1));
        }
        g.setColour (peakCol);
        if (! tokens.hardPixelSteps && tokens.barCellRadius > 0.5f)
        {
            g.fillRoundedRectangle (peakRect.toFloat(), juce::jmin (1.0f, tokens.barCellRadius * 0.5f));
            if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass && ! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (juce::Colours::white.withAlpha (0.48f));
                g.fillRect ((float) peakRect.getX() + 0.5f, (float) peakRect.getY(),
                            juce::jmax (1.0f, (float) peakRect.getWidth() - 1.0f), 1.0f);
            }
            else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
            {
                g.setColour (peakCol.interpolatedWith (juce::Colours::white, 0.58f));
                g.fillRect (peakRect.getX() + 1, peakRect.getY(), juce::jmax (1, peakRect.getWidth() - 2), 1);
            }
        }
        else
        {
            g.fillRect (peakRect);
            if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
            {
                g.setColour (peakCol.interpolatedWith (juce::Colours::white, 0.55f));
                g.fillRect (peakRect.getX(), peakRect.getY(), peakRect.getWidth(), 1);
            }
        }
    }

    PinkXP::Craft::drawCanvasFinishOverlay (g, barArea.reduced (2));
}

void LoudnessModule::drawScale(juce::Graphics& g, juce::Rectangle<int> scaleArea) const
{
    if (PinkXP::isChromaKeySkin() || scaleArea.getWidth() < 8 || scaleArea.getHeight() < 12)
        return;

    g.setFont(PinkXP::getAxisFont(9.0f));
    g.setColour(PinkXP::ink.withAlpha(0.75f));

    // 与 LED 柱体内部（barArea.reduced(3)）对齐，使 0 dB 与底部 floorDb 精准贴齐灯带上下缘
    const auto aligned = (scaleArea.getHeight() >= 16) ? scaleArea.reduced (0, 3) : scaleArea;
    const float top    = (float) aligned.getY();
    const float bottom = (float) aligned.getBottom();
    const float height = juce::jmax (1.0f, bottom - top);
    const int   minLabelY = scaleArea.getY();
    const int   maxLabelY = juce::jmax (minLabelY, scaleArea.getBottom() - 10);

    const auto& tokens = PinkXP::getSkinTokens();
    const bool  modernScale = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                               || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool  chromeScale = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool  neonScale   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool  crtScale    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool  isDarkFace  = PinkXP::face.getPerceivedBrightness() < 0.45f;
    const float labelAlpha  = modernScale ? 0.68f : (chromeScale ? 0.82f : ((neonScale || crtScale) ? 0.86f : 0.80f));

    if (chromeScale)
    {
        // 激光刻蚀垂直刻度导轨（1px 深槽 + 1px 镜面亮沿）
        g.setColour (PinkXP::dark.withAlpha (isDarkFace ? 0.50f : 0.32f));
        g.fillRect (scaleArea.getX(), aligned.getY(), 1, aligned.getHeight());
        g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f)
                               .withAlpha (isDarkFace ? 0.22f : 0.58f));
        g.fillRect (scaleArea.getX() + 1, aligned.getY(), 1, aligned.getHeight());
    }
    else if (neonScale || crtScale)
    {
        // 霓虹 / Tektronix CRT 垂直标尺轨
        g.setColour (PinkXP::sel.withAlpha (crtScale ? 0.48f : 0.36f));
        g.fillRect (scaleArea.getX(), aligned.getY(), 1, aligned.getHeight());
    }

    const int marks[] = { 0, -6, -12, -18, -24, -36, -48, -60, -72 };
    for (int db : marks)
    {
        if ((float) db < floorDb - 0.5f) continue;
        const float norm = juce::jmap((float)db, floorDb, 0.0f, 0.0f, 1.0f);
        const int   tickY = juce::jlimit (scaleArea.getY(),
                                          juce::jmax (scaleArea.getY(), scaleArea.getBottom() - 1),
                                          juce::roundToInt (bottom - norm * height));
        const bool major = (db == 0 || db == -12 || db == -24 || db == -36 || db == -48 || db == -60);

        if (modernScale)
        {
            g.setColour (PinkXP::ink.withAlpha (major ? 0.36f : 0.20f));
            g.fillRect (scaleArea.getX(), tickY, major ? 4 : 3, 1);
        }
        else if (chromeScale)
        {
            const int tickW = major ? 5 : 3;
            g.setColour (PinkXP::ink.withAlpha (major ? 0.68f : 0.38f));
            g.fillRect (scaleArea.getX(), tickY, tickW, 1);
            if (tickY + 1 < scaleArea.getBottom())
            {
                g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f)
                                       .withAlpha (isDarkFace ? (major ? 0.28f : 0.16f)
                                                              : (major ? 0.68f : 0.42f)));
                g.fillRect (scaleArea.getX() + 1, tickY + 1, juce::jmax (1, tickW - 1), 1);
            }
        }
        else if (neonScale || crtScale)
        {
            const int tickW = major ? 5 : 3;
            g.setColour (PinkXP::sel.withAlpha (major ? 0.85f : 0.45f));
            g.fillRect (scaleArea.getX(), tickY, tickW, 1);
            if (neonScale && major)
            {
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.85f));
                g.fillRect (scaleArea.getX(), tickY, 2, 1);
            }
        }
        else
        {
            PinkXP::Craft::drawGridLine (g, (float) scaleArea.getX(), (float) tickY,
                                         (float) (scaleArea.getX() + 4), (float) tickY,
                                         major, PinkXP::pink600);
            g.setColour (PinkXP::pink600.withAlpha (major ? 0.65f : 0.40f));
            g.fillRect (scaleArea.getX(), tickY, 2, 1);
        }

        g.setColour(PinkXP::ink.withAlpha(labelAlpha));
        const int labelY = juce::jlimit (minLabelY, maxLabelY, tickY - 5);
        juce::String label = (db == 0) ? "0" : juce::String(db);
        PinkXP::drawCachedText(g, label,
                               juce::Rectangle<int>(scaleArea.getX() + 5, labelY,
                                                    scaleArea.getWidth() - 5, 10),
                               juce::Justification::centredLeft, 9.0f, juce::Font::plain,
                               PinkXP::ink.withAlpha(labelAlpha), true);
    }

    const float tgt = targetLufsDb();
    if (tgt >= floorDb && tgt <= 0.0f)
    {
        const float norm = juce::jmap (tgt, floorDb, 0.0f, 0.0f, 1.0f);
        const float y    = juce::jlimit ((float) scaleArea.getY() + 1.0f,
                                         (float) juce::jmax (scaleArea.getY() + 1, scaleArea.getBottom() - 1),
                                         bottom - norm * height);
        g.setColour (PinkXP::sel);
        g.fillRect ((float) scaleArea.getX(), y - 1.0f, 5.0f, 2.0f);
        if (chromeScale || neonScale || crtScale)
        {
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f));
            g.fillRect ((float) scaleArea.getX(), y - 1.0f, 5.0f, 1.0f);
        }
    }
}

// ==========================================================
// Toybox（v3.0.0）：打砖块（§8.2）
// ==========================================================
bool LoudnessModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    if (style == Style::ledBars)
    {
        for (int i = 0; i < (int) barAreas.size(); ++i)
        {
            if (barAreas[(size_t) i].contains (panelPos))
            {
                out.kind = i;
                out.rect = barAreas[(size_t) i].toFloat();
                out.pos  = panelPos.toFloat();
                return true;
            }
        }
        return false;
    }

    for (const auto& rr : readoutRects)
    {
        if (rr.second.contains (panelPos))
        {
            out.kind = rr.first;
            out.rect = rr.second.toFloat();
            out.pos  = panelPos.toFloat();
            return true;
        }
    }
    return false;
}

void LoudnessModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    if (style == Style::ledBars)
    {
        if (slots.empty()) return;
        const int i = juce::jlimit (0, (int) slots.size() - 1, hit.kind);
        const float val = slots[(size_t) i].bar->smoothed;
        const juce::Colour c = slots[(size_t) i].isLufs ? lufsToColour (val) : dbToColour (val);
        ctx.addModuleActorCapped (makeMeterBarLaunchActor (hit.rect, hit.pos.y, c), 6);
    }
    else
    {
        const int r = juce::jlimit (0, (int) numReadRows - 1, hit.kind);
        const float val = rdValue[r];
        const juce::Colour c = (r <= rI) ? lufsToColour (val) : dbToColour (val);
        ctx.addModuleActorCapped (makePerimeterSweepActor (hit.rect, hit.pos, c), 6);
    }
}

// ==========================================================
// 布局
// ==========================================================
juce::Rectangle<int> LoudnessModule::barLayoutArea(juce::Rectangle<int> content) const noexcept
{
    auto area = content.reduced(6);
    // 右侧 / 下方原来各留 6px，刻度文本与读数离模块边界太远；
    //   收紧到 2px，只留一条小空隙。
    area.setRight  (content.getRight()  - 2);
    area.setBottom (content.getBottom() - 2);
    return area;
}

void LoudnessModule::computeBarGeometry(juce::Rectangle<int> content,
                                        std::vector<juce::Rectangle<int>>& bars,
                                        juce::Rectangle<int>& scale) const
{
    auto area = barLayoutArea(content);

    area.removeFromBottom(labelH + readoutH);
    scale = area.removeFromRight(22);   // 刻度列：与 LED 柱体等高对齐，右边不再留大片空白
    area.removeFromRight(3);

    const int barCount = juce::jmax (1, (int) slots.size());
    const int gap = 4;
    const int barW = juce::jmax (1, (area.getWidth() - gap * (barCount - 1)) / barCount);

    bars.clear();
    for (int idx = 0; idx < (int) slots.size(); ++idx)
        bars.emplace_back (area.getX() + idx * (barW + gap), area.getY(), barW, area.getHeight());
}

void LoudnessModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    readoutRects.clear();

    if (style == Style::ledBars)
    {
        computeBarGeometry (contentBounds, barAreas, areaScale);
        return;
    }

    // 读数样式：启用行垂直堆叠（先 M S I，再 True Peak，再 RMS）
    barAreas.clear();
    areaScale = {};

    bool rowOn[numReadRows] = { items.m, items.s, items.i, items.tp, items.rms };
    int n = 0;
    for (bool b : rowOn) n += b ? 1 : 0;
    if (n == 0) return;

    auto area = contentBounds.reduced(6);
    const int h = juce::jmax(44, area.getHeight() / n - 3);
    int slot = 0;
    for (int r = 0; r < numReadRows; ++r)
    {
        if (! rowOn[r]) continue;
        const bool last = (slot == n - 1);
        auto cell = last ? area : area.removeFromTop (h);
        readoutRects.emplace_back (r, cell);
        if (! last) area.removeFromTop (3);
        ++slot;
    }
}

// ==========================================================
// paintContent
// ==========================================================
void LoudnessModule::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    // 内容区底色：绿幕纯绿 / 透明模式跳过不透明填充（保持 alpha=0）/ 正常模式按风格填充
    const auto& tokens = PinkXP::getSkinTokens();
    if (PinkXP::isGreenScreenBackdrop())
    {
        g.setColour (PinkXP::chromaKeyGreen());
        g.fillRect (contentBounds);
    }
    else if (! PinkXP::isTransparentBackdrop())
    {
        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            juce::ColourGradient grad (PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.05f : 0.11f),
                                       0.0f, (float) contentBounds.getY(),
                                       PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.06f : 0.04f),
                                       0.0f, (float) contentBounds.getBottom(), false);
            grad.addColour (0.38, PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.03f : 0.06f));
            grad.addColour (0.52, PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.03f : 0.02f));
            g.setGradientFill (grad);
            g.fillRect (contentBounds);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            juce::ColourGradient grad (PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.06f : 0.15f),
                                       0.0f, (float) contentBounds.getY(),
                                       PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.05f : 0.03f),
                                       0.0f, (float) contentBounds.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRect (contentBounds);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube
                 || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            const float darken = (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                                     ? (isDark ? 0.16f : 0.07f)
                                     : (isDark ? 0.10f : 0.03f);
            g.setColour (PinkXP::face.interpolatedWith (juce::Colours::black, darken));
            g.fillRect (contentBounds);
        }
        else
        {
            g.setColour (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel ? PinkXP::btnFace : PinkXP::face);
            g.fillRect (contentBounds);
        }
    }

    if (style == Style::ledBars) paintLedBars (g, contentBounds);
    else                         paintReadouts (g, contentBounds);
}

void LoudnessModule::paintReadouts(juce::Graphics& g, juce::Rectangle<int>)
{
    static const char* titles[numReadRows] = { "LUFS-M", "LUFS-S", "LUFS-I", "TRUE PEAK", "RMS" };
    static const char* units [numReadRows] = { "LUFS", "LUFS", "LUFS", "dBTP", "dBFS" };

    int slot = 0;
    const float tgt = targetLufsDb();
    for (const auto& rr : readoutRects)
    {
        const int r = rr.first;
        const float v = rdValue[r];
        const float tNorm = juce::jlimit (0.0f, 1.0f, juce::jmap (v, floorDb, 0.0f, 0.0f, 1.0f));
        y2k::meter::drawReadout (g, rr.second, titles[r], v, units[r], tNorm,
                                 rdFlash[r], pulsePhase + 2.0f * (float) slot);

        // 在 LUFS 读数卡片底部进度条位置绘制目标响度参考刻度
        if (r <= rI && tgt >= floorDb && tgt <= 0.0f && ! PinkXP::isChromaKeySkin())
        {
            const float tgtNorm = juce::jlimit (0.0f, 1.0f, juce::jmap (tgt, floorDb, 0.0f, 0.0f, 1.0f));
            const int x = rr.second.getX() + 6 + juce::roundToInt (tgtNorm * (float) juce::jmax (1, rr.second.getWidth() - 12));
            g.setColour (PinkXP::sel);
            g.fillRect (x - 1, rr.second.getBottom() - 12, 2, 9);
            if (PinkXP::getSkinTokens().frameStyle == PinkXP::FrameStyle::machinedChrome)
            {
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f));
                g.fillRect (x - 1, rr.second.getBottom() - 12, 1, 9);
            }
        }
        ++slot;
    }
}

void LoudnessModule::paintLedBars(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    // 绿幕：静态层（凹槽 + 暗格 + 刻度）整层不画
    const bool chromaKey = PinkXP::isChromaKeySkin();
    if (! chromaKey)
    {
        rebuildStaticLayerIfNeeded(contentBounds);
        drawStaticLayer(g, contentBounds);
    }

    for (size_t i = 0; i < slots.size() && i < barAreas.size(); ++i)
        drawMeterBar(g, barAreas[i], *slots[i].bar, slots[i].isLufs);

    if (! chromaKey && ! staticLayer.isValid())
        drawScale(g, areaScale);

    // 底部标签 + 数字读数（与 computeBarGeometry 共用同一份布局区）
    const int bottom = barLayoutArea(contentBounds).getBottom();

    for (int idx = 0; idx < (int) slots.size() && idx < (int) barAreas.size(); ++idx)
    {
        const auto& bar = *slots[(size_t) idx].bar;
        const int x = barAreas[(size_t) idx].getX();
        const int barW = barAreas[(size_t) idx].getWidth();
        const int labelY = bottom - labelH - readoutH;

        // 标签：直接居中绘制（只有几个单字符，逐帧开销可忽略）
        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getAxisFont(11.0f, juce::Font::bold));
        PinkXP::drawCachedText(g, bar.label, juce::Rectangle<int>(x, labelY, barW, labelH),
                               juce::Justification::centred, 11.0f, juce::Font::bold,
                               PinkXP::ink, true);

        // 数字读数（防御：NaN / Inf / 过小 → -144；其余做 clamp）
        const float raw = bar.smoothed;
        const bool  isFinite = std::isfinite(raw);
        const float val = isFinite ? raw : -144.0f;

        juce::String readout;
        if (!isFinite || val <= -60.0f)
            readout = "-144";
        else
            readout = juce::String(juce::jlimit(-60.0f, 60.0f, val), 1);

        // 读数位图缓存：值变化且距上次 ≥ 100ms 才重光栅化（10Hz 刷新）
        //   颜色不用电平分区色（pink200 这类中间调在深色主题下对比太弱），
        //   统一用"数字读数"样式里大数字的颜色 pink600 —— 深色主题下它就是浅色端。
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const juce::Colour rc = PinkXP::pink600;
        if (readout != readoutCacheText_[idx]
            || nowMs - readoutCacheTimeMs_[idx] >= 100.0
            || ! readoutCache_[idx].isValid())
        {
            readoutCacheText_[idx]   = readout;
            readoutCacheTimeMs_[idx] = nowMs;
            readoutCache_[idx] = PinkXP::cachedStaticText (
                g, readout, 9.0f, juce::Font::plain, rc);
        }

        if (readoutCache_[idx].isValid())
        {
            // 位图比 14px 读数带高时向上溢出的部分夹回带内，避免压到上方标签
            const int readoutY = labelY + labelH
                    + juce::jmax(0, (readoutH - readoutCache_[idx].getHeight()) / 2);
            readoutCache_[idx].drawAt (g,
                                       (float) (x + (barW - readoutCache_[idx].getWidth()) / 2),
                                       (float) readoutY);
        }
        else
        {
            g.setFont(PinkXP::getFont(9.0f));
            g.setColour(rc);
            g.drawText(y2k::tr (readout), x, labelY + labelH, barW, readoutH,
                       juce::Justification::centred, false);
        }
    }
}

void LoudnessModule::drawMeterBarStaticLayer(juce::Graphics& g,
                                             juce::Rectangle<int> barArea,
                                             bool isLUFS) const
{
    PinkXP::Craft::drawPanelWell (g, barArea, PinkXP::content.darker (0.05f), PinkXP::WellKind::trough);

    const int padding = 3;
    auto inner = barArea.reduced(padding);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int step = cellSize + cellGap;
    const int numRows = juce::jmax(1, (inner.getHeight() + cellGap) / step);

    const int totalH = numRows * step - cellGap;
    const int offsetY = inner.getY() + (inner.getHeight() - totalH) / 2;
    const int colW = juce::jmax(1, inner.getWidth() - 4);
    const int offsetX = inner.getX() + (inner.getWidth() - colW) / 2;

    // 凹槽内标尺网格线（点阵 / 细线 / 虚线 / 磷光准星，绿幕自动跳过）
    const int refMarks[] = { -12, -24, -36, -48, -60 };
    for (int db : refMarks)
    {
        if ((float) db <= floorDb + 1.0f) continue;
        const float norm = valueToNorm ((float) db);
        const float y    = std::round ((float) offsetY + (1.0f - norm) * (float) totalH);
        PinkXP::Craft::drawGridLine (g, (float) inner.getX(), y, (float) inner.getRight(), y,
                                     (db == -12 || db == -24), PinkXP::ink);
    }

    for (int row = 0; row < numRows; ++row)
    {
        const int y = offsetY + row * step;
        const float rowNorm = 1.0f - (float)row / (float)juce::jmax(1, numRows - 1);
        const float rowDb   = juce::jmap(rowNorm, floorDb, 0.0f);
        const auto base = isLUFS ? lufsToColour(rowDb) : dbToColour(rowDb);
        PinkXP::Craft::drawMeterCell (g, juce::Rectangle<int> (offsetX, y, colW, cellSize), base, false);
    }
}

int LoudnessModule::firstVisibleRowForValue(float value, int numRows) const noexcept
{
    const float norm = valueToNorm(value);
    const int litRows = (int) std::round(norm * (float) numRows);
    const int tailRows = juce::jmax(6, litRows + 8);
    return juce::jmax(0, numRows - tailRows);
}

void LoudnessModule::rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds)
{
    if (contentBounds.isEmpty())
    {
        staticLayer = juce::Image();
        staticLayerContentBounds = {};
        staticLayerAppearanceGen = 0;
        staticLayerFontGen       = 0;
        return;
    }

    const auto appGen  = PinkXP::getAppearanceGeneration();
    const auto fontGen = PinkXP::getFontGeneration();

    if (staticLayer.isValid()
        && staticLayerContentBounds == contentBounds
        && staticLayerBarCount == (int) slots.size()
        && staticLayerAppearanceGen == appGen
        && staticLayerFontGen == fontGen
        && staticLayer.getWidth() == contentBounds.getWidth()
        && staticLayer.getHeight() == contentBounds.getHeight())
    {
        return;
    }

    staticLayer = juce::Image(juce::Image::ARGB,
                              contentBounds.getWidth(),
                              contentBounds.getHeight(),
                              true,
                              juce::SoftwareImageType());
    staticLayerContentBounds = contentBounds;
    staticLayerBarCount      = (int) slots.size();
    staticLayerAppearanceGen = appGen;
    staticLayerFontGen       = fontGen;

    if (PinkXP::isTransparentBackdrop())
    {
        staticLayer.clear (staticLayer.getBounds(), juce::Colours::transparentBlack);
        return;
    }

    juce::Graphics sg(staticLayer);
    const auto& tokens = PinkXP::getSkinTokens();
    if (PinkXP::isGreenScreenBackdrop())
    {
        sg.setColour (PinkXP::chromaKeyGreen());
        sg.fillRect (staticLayer.getBounds());
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
    {
        const auto b = staticLayer.getBounds();
        const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
        juce::ColourGradient grad (PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.05f : 0.11f),
                                   0.0f, (float) b.getY(),
                                   PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.06f : 0.04f),
                                   0.0f, (float) b.getBottom(), false);
        grad.addColour (0.38, PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.03f : 0.06f));
        grad.addColour (0.52, PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.03f : 0.02f));
        sg.setGradientFill (grad);
        sg.fillRect (b);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
    {
        const auto b = staticLayer.getBounds();
        const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
        juce::ColourGradient grad (PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.06f : 0.15f),
                                   0.0f, (float) b.getY(),
                                   PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.05f : 0.03f),
                                   0.0f, (float) b.getBottom(), false);
        sg.setGradientFill (grad);
        sg.fillRect (b);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube
             || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
    {
        const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
        const float darken = (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                                 ? (isDark ? 0.16f : 0.07f)
                                 : (isDark ? 0.10f : 0.03f);
        sg.setColour (PinkXP::face.interpolatedWith (juce::Colours::black, darken));
        sg.fillRect (staticLayer.getBounds());
    }
    else
    {
        sg.setColour (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel ? PinkXP::btnFace : PinkXP::face);
        sg.fillRect (staticLayer.getBounds());
    }

    std::vector<juce::Rectangle<int>> bars;
    juce::Rectangle<int> scale;
    computeBarGeometry (juce::Rectangle<int>(0, 0, contentBounds.getWidth(), contentBounds.getHeight()),
                        bars, scale);

    for (size_t i = 0; i < bars.size() && i < slots.size(); ++i)
        drawMeterBarStaticLayer(sg, bars[i], slots[i].isLufs);

    drawScale(sg, scale);
}

void LoudnessModule::drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const
{
    if (! staticLayer.isValid() || contentBounds.isEmpty())
        return;

    g.drawImageAt(staticLayer, contentBounds.getX(), contentBounds.getY());
}

void LoudnessModule::invalidateStaticLayer()
{
    staticLayer = juce::Image();
    staticLayerContentBounds = {};
    staticLayerBarCount = -1;
    staticLayerAppearanceGen = 0;
    staticLayerFontGen       = 0;
    for (int i = 0; i < 7; ++i)
    {
        readoutCache_[i] = {};
        readoutCacheText_[i].clear();
        readoutCacheTimeMs_[i] = 0.0;
    }
}
