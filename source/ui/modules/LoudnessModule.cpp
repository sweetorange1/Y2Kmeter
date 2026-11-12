#include "source/ui/modules/LoudnessModule.h"
#include "source/ui/modules/MeterWidgets.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
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
    s.setProperty ("itemTp",  items.tp,  nullptr);
    s.setProperty ("itemRms", items.rms, nullptr);
    return s;
}

void LoudnessModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    // 缺键 = 保持当前值；构造默认 = 旧 Loudness 外观（灯条 + M S I RMS）
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

    out.push_back (ParamDef::toggle ("itemM", "Momentary (LUFS-M)",
        "Loudness over the last 400 ms - reacts fastest.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemS", "Short-term (LUFS-S)",
        "Loudness over the last 3 s - the usual main number.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemI", "Integrated (LUFS-I)",
        "Gated average over the whole programme. Resets after a long silence.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemTp", "True Peak",
        "Inter-sample peak (4x oversampled). LED bars show L and R, readouts show the louder one.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemRms", "RMS",
        "Average level of the left and right channel. Readouts show the louder one.").in ("Show"));
}

double LoudnessModule::getParamValue (const juce::String& id) const
{
    if (id == "style")   return (double) (int) style;
    if (id == "itemM")   return items.m ? 1.0 : 0.0;
    if (id == "itemS")   return items.s ? 1.0 : 0.0;
    if (id == "itemI")   return items.i ? 1.0 : 0.0;
    if (id == "itemTp")  return items.tp ? 1.0 : 0.0;
    if (id == "itemRms") return items.rms ? 1.0 : 0.0;
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
        if (items.m)   barM.update(snap.lufsM, deltaMs);
        if (items.s)   barS.update(snap.lufsS, deltaMs);
        if (items.i)   barI.update(snap.lufsI, deltaMs);
        if (items.tp)  { barTpL.update(snap.truePeakL, deltaMs); barTpR.update(snap.truePeakR, deltaMs); }
        if (items.rms) { barRmsL.update(snap.rmsL, deltaMs);     barRmsR.update(snap.rmsR, deltaMs); }
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
float LoudnessModule::valueToNorm(float val) noexcept
{
    // LUFS / dBFS 统一映射到 -60 ~ 0 dB
    return juce::jlimit(0.0f, 1.0f, juce::jmap(val, -60.0f, 0.0f, 0.0f, 1.0f));
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
    PinkXP::drawSunken(g, barArea, PinkXP::content.darker(0.05f));

    const int padding = 3;
    auto inner = barArea.reduced(padding);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int step = cellSize + cellGap;
    const int numRows = juce::jmax(1, (inner.getHeight() + cellGap) / step);

    const float norm = valueToNorm(bar.smoothed);
    const int litRows = (int)std::round(norm * (float)numRows);

    const float peakNorm = valueToNorm(bar.peakValue);
    const int peakRow = (int)std::round(peakNorm * (float)numRows);

    const int totalH = numRows * step - cellGap;
    const int offsetY = inner.getY() + (inner.getHeight() - totalH) / 2;
    // 灯带宽度随柱容器宽度自适应：保留左右各 2px 安全边距
    const int colW = juce::jmax(1, inner.getWidth() - 4);
    const int offsetX = inner.getX() + (inner.getWidth() - colW) / 2;

    const int firstRow = firstVisibleRowForValue(bar.smoothed, numRows);
    for (int row = firstRow; row < numRows; ++row)
    {
        const int y = offsetY + row * step;
        const bool lit = (numRows - 1 - row) < litRows;

        const float rowNorm = 1.0f - (float)row / (float)juce::jmax(1, numRows - 1);
        const float rowDb   = juce::jmap(rowNorm, -60.0f, 0.0f);

        // 未点亮格子用同色 0.15 alpha：视觉上是"色带随电平从底向上点亮"
        const auto base = isLUFS ? lufsToColour(rowDb) : dbToColour(rowDb);
        g.setColour(lit ? base : base.withAlpha(0.15f));
        g.fillRect(offsetX, y, colW, cellSize);
    }

    // 缓降峰值线
    if (peakRow > 0 && peakRow <= numRows)
    {
        const int peakY = offsetY + (numRows - peakRow) * step;
        const auto peakCol = (isLUFS ? lufsToColour(bar.peakValue)
                                     : dbToColour (bar.peakValue)).brighter(0.2f);
        g.setColour(peakCol);
        g.fillRect(offsetX - 1, peakY, colW + 2, 2);
    }
}

// ==========================================================
// drawScale —— 绘制右侧 dB 刻度尺
// ==========================================================
void LoudnessModule::drawScale(juce::Graphics& g, juce::Rectangle<int> scaleArea) const
{
    g.setFont(PinkXP::getAxisFont(9.0f));
    g.setColour(PinkXP::ink.withAlpha(0.75f));

    const float top    = (float)scaleArea.getY();
    const float bottom = (float)scaleArea.getBottom();
    const float height = bottom - top;

    const int marks[] = { 0, -6, -12, -18, -24, -36, -48, -60 };
    for (int db : marks)
    {
        const float norm = juce::jmap((float)db, -60.0f, 0.0f, 0.0f, 1.0f);
        const float y    = bottom - norm * height;

        g.setColour(PinkXP::pink600.withAlpha(0.5f));
        g.drawHorizontalLine((int)y, (float)scaleArea.getX(),
                             (float)(scaleArea.getX() + 4));

        g.setColour(PinkXP::ink.withAlpha(0.8f));
        juce::String label = (db == 0) ? "0" : juce::String(db);
        PinkXP::drawCachedText(g, label,
                               juce::Rectangle<int>(scaleArea.getX() + 5, (int)y - 5,
                                                    scaleArea.getWidth() - 5, 10),
                               juce::Justification::centredLeft, 9.0f, juce::Font::plain,
                               PinkXP::ink.withAlpha(0.8f), true);
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
    float val = 0.0f;
    juce::String unit;
    juce::Colour c;

    if (style == Style::ledBars)
    {
        const int i = juce::jlimit (0, juce::jmax (0, (int) slots.size() - 1), hit.kind);
        if (slots.empty()) return;
        val  = slots[(size_t) i].bar->smoothed;
        unit = slots[(size_t) i].isLufs ? "LUFS" : "dBFS";
        c    = slots[(size_t) i].isLufs ? lufsToColour (val) : dbToColour (val);
    }
    else
    {
        const int r = juce::jlimit (0, (int) numReadRows - 1, hit.kind);
        val  = rdValue[r];
        unit = (r <= rI) ? "LUFS" : (r == rTp ? "dBTP" : "dBFS");
        c    = (r <= rI) ? lufsToColour (val) : dbToColour (val);
    }

    ctx.addModuleActor (makeBrickShardsActor (hit.pos, c, 8));
    ctx.bubble (hit.pos.translated (0.0f, -24.0f),
                juce::String (val, 1) + " " + unit, ToyRole::Burst);
}

// ==========================================================
// 布局
// ==========================================================
void LoudnessModule::computeBarGeometry(juce::Rectangle<int> content,
                                        std::vector<juce::Rectangle<int>>& bars,
                                        juce::Rectangle<int>& scale) const
{
    auto area = content.reduced(6);

    scale = area.removeFromRight(30);
    area.removeFromRight(4);
    area.removeFromBottom(labelH + readoutH);

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
    // 内容区底色（深色主题下也保持浅色）
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    if (style == Style::ledBars) paintLedBars (g, contentBounds);
    else                         paintReadouts (g, contentBounds);
}

void LoudnessModule::paintReadouts(juce::Graphics& g, juce::Rectangle<int>)
{
    static const char* titles[numReadRows] = { "LUFS-M", "LUFS-S", "LUFS-I", "TRUE PEAK", "RMS" };
    static const char* units [numReadRows] = { "LUFS", "LUFS", "LUFS", "dBTP", "dBFS" };

    int slot = 0;
    for (const auto& rr : readoutRects)
    {
        const int r = rr.first;
        const float v = rdValue[r];
        const float tNorm = juce::jlimit (0.0f, 1.0f, juce::jmap (v, -40.0f, 0.0f, 0.0f, 1.0f));
        y2k::meter::drawReadout (g, rr.second, titles[r], v, units[r], tNorm,
                                 rdFlash[r], pulsePhase + 2.0f * (float) slot);
        ++slot;
    }
}

void LoudnessModule::paintLedBars(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    rebuildStaticLayerIfNeeded(contentBounds);
    drawStaticLayer(g, contentBounds);

    for (size_t i = 0; i < slots.size() && i < barAreas.size(); ++i)
        drawMeterBar(g, barAreas[i], *slots[i].bar, slots[i].isLufs);

    drawScale(g, areaScale);

    // 底部标签 + 数字读数
    const int bottom = contentBounds.reduced(6).getBottom();

    for (int idx = 0; idx < (int) slots.size() && idx < (int) barAreas.size(); ++idx)
    {
        const auto& bar = *slots[(size_t) idx].bar;
        const bool isLUFS = slots[(size_t) idx].isLufs;
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
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const juce::Colour rc = isLUFS ? lufsToColour(val) : dbToColour(val);
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
    PinkXP::drawSunken(g, barArea, PinkXP::content.darker(0.05f));

    const int padding = 3;
    auto inner = barArea.reduced(padding);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int step = cellSize + cellGap;
    const int numRows = juce::jmax(1, (inner.getHeight() + cellGap) / step);

    const int totalH = numRows * step - cellGap;
    const int offsetY = inner.getY() + (inner.getHeight() - totalH) / 2;
    const int colW = juce::jmax(1, inner.getWidth() - 4);
    const int offsetX = inner.getX() + (inner.getWidth() - colW) / 2;

    for (int row = 0; row < numRows; ++row)
    {
        const int y = offsetY + row * step;
        const float rowNorm = 1.0f - (float)row / (float)juce::jmax(1, numRows - 1);
        const float rowDb   = juce::jmap(rowNorm, -60.0f, 0.0f);
        const auto base = isLUFS ? lufsToColour(rowDb) : dbToColour(rowDb);
        g.setColour(base.withAlpha(0.12f));
        g.fillRect(offsetX, y, colW, cellSize);
    }
}

int LoudnessModule::firstVisibleRowForValue(float value, int numRows) noexcept
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
        return;
    }

    if (staticLayer.isValid()
        && staticLayerContentBounds == contentBounds
        && staticLayerBarCount == (int) slots.size()
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
    staticLayerBarCount = (int) slots.size();

    juce::Graphics sg(staticLayer);
    sg.setColour(PinkXP::btnFace);
    sg.fillRect(staticLayer.getBounds());

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
}
