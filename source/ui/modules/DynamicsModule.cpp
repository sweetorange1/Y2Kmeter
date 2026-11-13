#include "source/ui/modules/DynamicsModule.h"
#include <cmath>

#include "source/ui/modules/MeterWidgets.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// DynamicsModule（动态模块）
// ==========================================================

juce::Colour DynamicsModule::dbToColour (float db)
{
    return y2k::meter::dbColour (db);
}

DynamicsModule::DynamicsModule(AnalyserHub& h)
    : ModulePanel(ModuleType::dynamics),
      hub(h)
{
    // Phase F：订阅 Dynamics + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Dynamics);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // dynamics 6×4 大格
    setControlPanelEnabled (true);

    bPeakL.label = "PK L";
    bPeakR.label = "PK R";
    bRmsL .label = "RMS L";
    bRmsR .label = "RMS R";

    crestHist.assign ((size_t) crestHistLen, 0.0f);
}

DynamicsModule::~DynamicsModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Dynamics);
}

// ----------------------------------------------------------
// 持久化 / 控制面板
// ----------------------------------------------------------
juce::ValueTree DynamicsModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("itemPeak",    items.peak,    nullptr);
    s.setProperty ("itemRms",     items.rms,     nullptr);
    s.setProperty ("itemDrShort", items.drShort, nullptr);
    s.setProperty ("itemDrInt",   items.drInteg, nullptr);
    s.setProperty ("itemCrest",   items.crest,   nullptr);
    s.setProperty ("itemHist",    items.hist,    nullptr);
    s.setProperty ("style",       (int) readoutStyle, nullptr);
    s.setProperty ("histSpan",    (double) histSpanSec, nullptr);
    return s;
}

void DynamicsModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    // 缺键 = 保持当前值（构造默认 = 旧 Dynamics 外观；种子只覆盖它关心的键）
    auto getB = [&] (const char* k, bool& dst)
    {
        if (state.hasProperty (k)) dst = (bool) state.getProperty (k);
    };

    Items it = items;
    getB ("itemPeak", it.peak);       getB ("itemRms", it.rms);
    getB ("itemDrShort", it.drShort); getB ("itemDrInt", it.drInteg);
    getB ("itemCrest", it.crest);     getB ("itemHist", it.hist);
    if (! it.any()) it = Items();   // 防御：存档里全关 → 回到默认

    if (it.hist && ! items.hist)
    {
        std::fill (crestHist.begin(), crestHist.end(), 0.0f);
        crestHistWrite = 0;
        crestHistAccum = 0.0f;
        crestHistTotal = 0;
    }
    items = it;

    if (state.hasProperty ("style"))
        readoutStyle = (int) state.getProperty ("style") == 1 ? ReadoutStyle::barReadout
                                                              : ReadoutStyle::bigDigits;
    if (state.hasProperty ("histSpan"))
        histSpanSec = juce::jlimit (1.0f, kMaxSpanSec, (float) (double) state.getProperty ("histSpan"));

    layoutContent (getContentBounds());
    refreshControlPanel();
    repaint();
}

void DynamicsModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::toggle ("itemPeak", "Peak bars",
        "Sample-peak level bars for the left and right channel, with a slowly falling hold line.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemRms", "RMS bars",
        "Average (RMS) level bars for the left and right channel.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemDrShort", "DR short-term",
        "Dynamic range of the recent programme: loudest 20% against the average.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemDrInt", "DR integrated",
        "Dynamic range over the whole programme. Resets after a long silence.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemCrest", "Crest factor",
        "Peak minus RMS in dB right now. Low = squashed, high = punchy.").in ("Show"));
    out.push_back (ParamDef::toggle ("itemHist", "Crest history graph",
        "Scrolling graph of the crest factor over time.").in ("Show"));

    out.push_back (ParamDef::choice ("style", "DR readout style",
        { "Big digits", "Bars + digits" },
        "Big digits: large numbers that fill the cell. Bars + digits: smaller numbers with a progress bar.")
        .in ("Style").when ([this] { return items.drCount() > 0; }));
    out.push_back (ParamDef::slider ("histSpan", "History span", 1.0, kMaxSpanSec, 1.0, " s",
        "How much time the crest history graph covers.")
        .in ("Style").when ([this] { return items.hist; }));
}

double DynamicsModule::getParamValue (const juce::String& id) const
{
    if (id == "itemPeak")    return items.peak ? 1.0 : 0.0;
    if (id == "itemRms")     return items.rms ? 1.0 : 0.0;
    if (id == "itemDrShort") return items.drShort ? 1.0 : 0.0;
    if (id == "itemDrInt")   return items.drInteg ? 1.0 : 0.0;
    if (id == "itemCrest")   return items.crest ? 1.0 : 0.0;
    if (id == "itemHist")    return items.hist ? 1.0 : 0.0;
    if (id == "style")       return (double) (int) readoutStyle;
    if (id == "histSpan")    return (double) histSpanSec;
    return 0.0;
}

void DynamicsModule::setParamValue (const juce::String& id, double v)
{
    const bool on = v >= 0.5;
    Items it = items;

    if      (id == "itemPeak")    it.peak = on;
    else if (id == "itemRms")     it.rms = on;
    else if (id == "itemDrShort") it.drShort = on;
    else if (id == "itemDrInt")   it.drInteg = on;
    else if (id == "itemCrest")   it.crest = on;
    else if (id == "itemHist")    it.hist = on;
    else if (id == "style")
    {
        readoutStyle = juce::roundToInt (v) == 1 ? ReadoutStyle::barReadout : ReadoutStyle::bigDigits;
        repaint();
        return;
    }
    else if (id == "histSpan")
    {
        histSpanSec = juce::jlimit (1.0f, kMaxSpanSec, (float) v);
        repaint();
        return;
    }
    else return;

    if (! it.any())
        return;   // 至少保留一项，避免出现空白模块

    if (it.hist && ! items.hist)
    {
        std::fill (crestHist.begin(), crestHist.end(), 0.0f);
        crestHistWrite = 0;
        crestHistAccum = 0.0f;
        crestHistTotal = 0;
    }
    items = it;
    layoutContent (getContentBounds());
    repaint();
}

bool DynamicsModule::drRowEnabled (int row) const noexcept
{
    return row == rowShort ? items.drShort : row == rowInteg ? items.drInteg : items.crest;
}

float DynamicsModule::drRowValue (int row) const noexcept
{
    return row == rowShort ? smoothedShortDR : row == rowInteg ? smoothedIntegDR : smoothedCrest;
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调（UI 线程）
// ----------------------------------------------------------
void DynamicsModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Dynamics)) return;

    // v2.9：使用分发器提供的高精度帧间隔
    const float dtSec = frame.dtSec;

    const auto& s = frame.dynamics;
    if (items.peak) { bPeakL.update(s.peakL, dtSec); bPeakR.update(s.peakR, dtSec); }
    if (items.rms)  { bRmsL .update(s.rmsL,  dtSec); bRmsR .update(s.rmsR,  dtSec); }

    auto smoothNum = [dtSec](float cur, float target, float a)
    {
        if (! std::isfinite(target)) target = 0.0f;
        return cur + y2k::pacing::alphaForDt (a, dtSec) * (target - cur);
    };
    smoothedShortDR = smoothNum(smoothedShortDR, s.shortDR,      0.2f);
    smoothedIntegDR = smoothNum(smoothedIntegDR, s.integratedDR, 0.05f);
    smoothedCrest   = smoothNum(smoothedCrest,   s.crest,        0.3f);

    // 带进度条读数样式的闪白 / 心跳（数值刚变化时闪一下）
    if (readoutStyle == ReadoutStyle::barReadout && items.drCount() > 0)
    {
        const float th = juce::jmax (0.05f, dtSec * y2k::pacing::kRefHz);
        for (int r = 0; r < numDrRows; ++r)
        {
            const float cur = drRowValue (r);
            if (std::abs (cur - prevDr[r]) > 0.3f * th) flash[r] = 1.0f;
            prevDr[r] = cur;
            flash[r] = juce::jmax (0.0f, flash[r] - y2k::pacing::stepForDt (0.08f, dtSec));
        }
        pulsePhase += y2k::pacing::stepForDt (0.24f, dtSec);
        while (pulsePhase > juce::MathConstants<float>::twoPi)
            pulsePhase -= juce::MathConstants<float>::twoPi;
    }

    // 历史带写入（按时间推进，帧率无关；仅历史开启时）
    if (items.hist)
    {
        crestHistAccum += dtSec * (float) kHistRate;
        int pushes = juce::jmin ((int) crestHistAccum, crestHistLen);
        crestHistAccum -= (float) (int) crestHistAccum;
        const float crestVal = juce::jlimit(0.0f, 40.0f, s.crest);
        while (pushes-- > 0)
        {
            crestHist[(size_t) crestHistWrite] = crestVal;
            crestHistWrite = (crestHistWrite + 1) % crestHistLen;
            ++crestHistTotal;
        }
    }

    // v2.9.2：读数类模块 30Hz 重绘（数据每帧更新，仅重绘分频）
    if ((frame.tickCount & 1ull) != 0)
        return;
    repaintFrameContent();
}

// ----------------------------------------------------------
// layoutContent —— 只排启用的区段（四项全开 = 旧 Dynamics 布局）
// ----------------------------------------------------------
void DynamicsModule::layoutContent(juce::Rectangle<int> content)
{
    areaMeters = areaDr = areaCrest = {};

    const bool hasM = items.barCount() > 0;
    const bool hasD = items.drCount() > 0;
    const bool hasH = items.hist;
    const int  n    = (hasM ? 1 : 0) + (hasD ? 1 : 0) + (hasH ? 1 : 0);
    if (n == 0) return;

    constexpr int gap = 6;

    if (n == 1)
    {
        (hasM ? areaMeters : hasD ? areaDr : areaCrest) = content;
        return;
    }

    // 每根柱 40px（四柱 = 旧版 160px），DR 区 180px
    const int metersW = 40 * juce::jmax (1, items.barCount());
    const int drW     = 180;

    if (hasH)
    {
        // 与旧 Dynamics 同一套：历史带吃剩余宽度，窗口过窄时兜底 120px 或 30%
        const int leftW = (hasM ? metersW : 0) + (hasD ? drW : 0) + ((hasM && hasD) ? gap : 0);
        const int minCrestW = 120;
        int crestW = content.getWidth() - leftW - gap;
        if (crestW < minCrestW)
            crestW = juce::jmax (minCrestW, content.getWidth() * 3 / 10);

        areaCrest = content.withX (content.getRight() - crestW).withWidth (crestW);

        if (hasM && hasD)
        {
            areaMeters = content.withWidth (metersW);
            areaDr     = content.withTrimmedLeft (metersW + gap).withTrimmedRight (crestW + gap);
        }
        else
        {
            auto left = content.withTrimmedRight (crestW + gap);
            (hasM ? areaMeters : areaDr) = left;
        }
        return;
    }

    // 没有历史带：柱组与 DR 区按权重瓜分
    const int total = content.getWidth() - gap;
    const int mW = juce::jmax (30, total * metersW / (metersW + drW));
    areaMeters = content.withWidth (mW);
    areaDr     = content.withTrimmedLeft (mW + gap);
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：综合路由（Meters/DR/Crest，§8.2）
// ----------------------------------------------------------
bool DynamicsModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    if (areaCrest.contains (panelPos))      { out.kind = 3; out.rect = areaCrest.toFloat(); }
    else if (areaDr.contains (panelPos))    { out.kind = 2; out.rect = areaDr.toFloat(); }
    else if (areaMeters.contains (panelPos)){ out.kind = 1; out.rect = areaMeters.toFloat(); }
    else return false;
    out.pos = panelPos.toFloat();
    return true;
}

void DynamicsModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    switch (hit.kind)
    {
        case 1:
            ctx.addModuleActor (makeCoinPopActor (hit.pos));
            ctx.bubble (hit.pos.translated (0.0f, -24.0f), "PK / RMS", ToyRole::Reward);
            break;
        case 2:
            ctx.addModuleActor (makeSquashActor (hit.pos));
            ctx.bubble (hit.pos.translated (0.0f, -24.0f), "DYNAMIC RANGE", ToyRole::Reward);
            break;
        default:
            ctx.addModuleActor (makeCrestBallActor (hit.pos));
            ctx.bubble (hit.pos.translated (0.0f, -20.0f), "CREST", ToyRole::Orb);
            break;
    }
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
void DynamicsModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    drawMeterGroup(g, areaMeters);
    if (readoutStyle == ReadoutStyle::barReadout) drawDrReadouts (g, areaDr);
    else                                          drawDrBigDigits (g, areaDr);
    drawCrestBand (g, areaCrest);

    // 鼠标悬停标尺（Crest 历史带）
    if (hoverActive)
        drawHoverRuler(g);
}

void DynamicsModule::mouseMove(const juce::MouseEvent& e)
{
    ModulePanel::mouseMove(e);

    const auto pos    = e.getPosition();
    const bool inside = items.hist && areaCrest.contains(pos) && ! getControlDrawerBounds().contains (pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaintHover (areaCrest);
    }
}

void DynamicsModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        // 标尺只画在 Crest 历史带内，擦除只需失效该区域（不走浮动态限流）
        repaint (areaCrest);
    }
}

void DynamicsModule::drawHoverRuler(juce::Graphics& g)
{
    if (!hoverActive)
        return;

    // Crest 历史带内部绘图区（与 drawCrestBand 一致）
    const auto inner = areaCrest.reduced(4);
    const auto plot  = inner.withTrimmedTop(14);
    if (plot.isEmpty() || !plot.contains(hoverPos))
        return;

    // X → 相对时间（最右=当前，负=过去；横轴跨度 = 历史跨度参数）
    const float t = (float)(hoverPos.x - plot.getX())
                  / juce::jmax(1, plot.getWidth());
    const float timeSec = (t - 1.0f) * histSpanSec;

    // Y → crest 值（0 ~ 30 dB）
    const float norm = juce::jlimit(0.0f, 1.0f,
        (float)(plot.getBottom() - hoverPos.y) / juce::jmax(1, plot.getHeight()));
    const float crestDb = norm * 30.0f;

    const juce::String readout = juce::String(timeSec, 1) + " s  "
                               + juce::String(crestDb, 1) + " dB";
    PinkXP::drawHoverRuler(g, plot, hoverPos, readout);
}

// ----------------------------------------------------------
// 柱组（按启用项取 PK L/R、RMS L/R）
// ----------------------------------------------------------
void DynamicsModule::drawMeterGroup(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    const MeterBar* bars[4];
    int barCount = 0;
    if (items.peak) { bars[barCount++] = &bPeakL; bars[barCount++] = &bPeakR; }
    if (items.rms)  { bars[barCount++] = &bRmsL;  bars[barCount++] = &bRmsR;  }
    if (barCount == 0) return;

    const int gap = 4;
    const int labelH = 14;

    auto barRow = area.withTrimmedBottom(labelH);
    const int barW = juce::jmax(8, (barRow.getWidth() - gap * (barCount - 1)) / barCount);

    for (int i = 0; i < barCount; ++i)
    {
        juce::Rectangle<int> r(barRow.getX() + i * (barW + gap),
                               barRow.getY(), barW, barRow.getHeight());
        drawSingleBar(g, r, *bars[i]);

        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        PinkXP::drawCachedText(g, bars[i]->label,
                               juce::Rectangle<int>(r.getX(), area.getBottom() - labelH, r.getWidth(), labelH),
                               juce::Justification::centred, 9.0f, juce::Font::bold, PinkXP::ink);
    }
}

void DynamicsModule::drawSingleBar(juce::Graphics& g, juce::Rectangle<int> r, const MeterBar& bar) const
{
    PinkXP::drawSunken(g, r, PinkXP::content.darker(0.05f));
    auto inner = r.reduced(3);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int cellH = 4;
    const int cellGap = 1;
    const int step = cellH + cellGap;
    const int rows = juce::jmax(1, (inner.getHeight() + cellGap) / step);

    const float norm = dbToNorm(bar.smoothed);
    const int litRows = (int) std::round(norm * rows);

    for (int row = 0; row < rows; ++row)
    {
        // row=0 在屏幕顶部（高电平区）→ 0 dB → 红色；row=rows-1 → -60 dB → 绿色
        const float rowNorm = (float) row / (float) juce::jmax(1, rows - 1);
        const float rowDb   = juce::jmap(rowNorm, 0.0f, 1.0f, 0.0f, -60.0f);
        const bool  lit     = (rows - 1 - row) < litRows;

        const int y = inner.getY() + row * step;
        const auto base = dbToColour(rowDb);
        g.setColour(lit ? base : base.withAlpha(0.15f));
        g.fillRect(inner.getX(), y, inner.getWidth(), cellH);
    }

    // 峰值保持线
    const float peakNorm = dbToNorm(bar.peakHold);
    const int   peakY = inner.getBottom() - (int) std::round(peakNorm * inner.getHeight());
    if (peakY >= inner.getY() && peakY <= inner.getBottom())
    {
        g.setColour(dbToColour(bar.peakHold).brighter(0.2f));
        g.fillRect(inner.getX(), peakY - 1, inner.getWidth(), 2);
    }
}

// ----------------------------------------------------------
// DR 面板·大数字样式（旧 Dynamics）：启用行等分高度
// ----------------------------------------------------------
void DynamicsModule::drawDrBigDigits(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty() || items.drCount() == 0) return;

    PinkXP::drawSunken(g, area, PinkXP::content);

    auto inner = area.reduced(8);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int rowH = inner.getHeight() / items.drCount();

    auto drawRow = [&](int slot, int rowIdx, const juce::String& title, float value,
                       const juce::String& unit, float big)
    {
        juce::Rectangle<int> row(inner.getX(), inner.getY() + slot * rowH,
                                 inner.getWidth(), rowH);

        // ① 标题：窄面板下按行宽压缩，避免被右侧区段盖掉
        const int titleH = juce::jlimit(8, 12, row.getHeight() / 4);
        g.setColour(PinkXP::ink.withAlpha(0.85f));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawFittedText(y2k::tr (title), row.removeFromTop(titleH),
                         juce::Justification::topLeft, 1, 0.8f);

        // ② 单位 "dB" 在数值下方；面板被拖得很矮时不再单独留单位行
        const int unitH = (row.getHeight() >= 26) ? juce::jlimit(8, 12, row.getHeight() / 4) : 0;
        auto unitRect = (unitH > 0) ? row.removeFromBottom(unitH) : juce::Rectangle<int>();

        // ③ 数值：整行宽度都可用来画大数字，字号按可用宽度收缩（样本 "88.8" 保证稳定）
        const juce::String numText = juce::String(value, 1);

        float px = big;
        {
            const juce::Font probeFont = PinkXP::getAxisFont(px, juce::Font::bold);
            const float needW = juce::jmax(probeFont.getStringWidthFloat(y2k::tr ("88.8")),
                                           probeFont.getStringWidthFloat(y2k::tr (numText)));
            if (needW > 1.0f && needW > (float) row.getWidth())
                px *= (float) row.getWidth() / needW;
        }
        px = juce::jmin(px, (float) juce::jmax(6, row.getHeight() - 2));
        px = juce::jmax(6.0f, px);

        const juce::String cacheKey = numText + "|" + juce::String(px, 1);
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (cacheKey != drValueText_[rowIdx]
            || nowMs - drValueTime_[rowIdx] >= 100.0
            || ! drValueCache_[rowIdx].isValid())
        {
            drValueText_[rowIdx] = cacheKey;
            drValueTime_[rowIdx] = nowMs;
            drValueCache_[rowIdx] = PinkXP::cachedStaticText (
                g, numText, px, juce::Font::bold, PinkXP::pink600,
                juce::Colours::transparentBlack, true);
        }

        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (row);

            const auto& numImg = drValueCache_[rowIdx];
            if (numImg.isValid())
            {
                numImg.drawAt (g,
                               (float) row.getX(),
                               (float) (row.getY() + juce::jmax(0, (row.getHeight() - numImg.getHeight()) / 2)));
            }
            else
            {
                g.setColour(PinkXP::pink600);
                g.setFont(PinkXP::getAxisFont(px, juce::Font::bold));
                g.drawText(y2k::tr (numText), row, juce::Justification::centredLeft, false);
            }
        }

        // ④ 单位（位图缓存，紧贴数值下方，左对齐）
        if (unitRect.isEmpty())
            return;

        const auto unitImg = PinkXP::cachedStaticText (
            g, unit, 9.0f, juce::Font::plain, PinkXP::ink.withAlpha(0.75f),
            juce::Colours::transparentBlack, true);
        if (unitImg.isValid())
            unitImg.drawAt (g, (float) unitRect.getX(), (float) unitRect.getY());
        else
        {
            g.setColour(PinkXP::ink.withAlpha(0.75f));
            g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
            g.drawText(y2k::tr (unit), unitRect, juce::Justification::topLeft, false);
        }
    };

    static const char* titles[numDrRows] = { "DR SHORT", "DR INTEG", "CREST" };
    int slot = 0;
    for (int r = 0; r < numDrRows; ++r)
        if (drRowEnabled (r))
            drawRow (slot++, r, titles[r], drRowValue (r), "dB", 28.0f);
}

// ----------------------------------------------------------
// DR 面板·带进度条读数样式（旧 Dynamics DR）：启用行垂直堆叠
// ----------------------------------------------------------
void DynamicsModule::drawDrReadouts(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty() || items.drCount() == 0) return;

    auto a = area;
    const int n = items.drCount();
    const int h = juce::jmax(44, a.getHeight() / n - 3);
    auto drToNorm = [](float d) { return juce::jlimit(0.0f, 1.0f, d / 24.0f); };

    static const char* titles[numDrRows] = { "DR SHORT", "DR INTEG", "CREST" };
    int slot = 0;
    for (int r = 0; r < numDrRows; ++r)
    {
        if (! drRowEnabled (r)) continue;
        const bool last = (slot == n - 1);
        auto cell = last ? a : a.removeFromTop (h);
        y2k::meter::drawReadout (g, cell, titles[r], drRowValue (r), "dB", drToNorm (drRowValue (r)),
                                 flash[r], pulsePhase + 2.0f * (float) slot);
        if (! last) a.removeFromTop (3);
        ++slot;
    }
}

// ----------------------------------------------------------
// Crest 历史带：取最近 span 秒，过密时按像素列取最大值
// ----------------------------------------------------------
void DynamicsModule::drawCrestBand(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty() || ! items.hist) return;

    PinkXP::drawSunken(g, area, PinkXP::content);
    auto inner = area.reduced(4);
    if (inner.getWidth() <= 4 || inner.getHeight() <= 4) return;

    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    PinkXP::drawCachedText(g, "CREST HISTORY", inner.withHeight(12), juce::Justification::topLeft,
                           9.0f, juce::Font::bold, PinkXP::ink);

    auto plot = inner.withTrimmedTop(14);
    if (plot.isEmpty()) return;

    // 刻度线：0 / 10 / 20 / 30 dB
    g.setColour(PinkXP::pink200.withAlpha(0.5f));
    for (int db = 0; db <= 30; db += 10)
    {
        const float t = 1.0f - (float) db / 30.0f;
        const int y = plot.getY() + (int) std::round(t * plot.getHeight());
        g.drawHorizontalLine(y, (float) plot.getX(), (float) plot.getRight());
    }

    const int N = juce::jlimit (8, crestHistLen, juce::roundToInt (histSpanSec * (float) kHistRate));
    const int drawN = juce::jmin (N, juce::jmax (2, plot.getWidth()));
    const float w = (float) plot.getWidth();
    const float h = (float) plot.getHeight();

    // ---- 列的相位锁定（与示波器抽点是同一类问题）----
    //   旧实现把每列的样本区间钉在"窗口起点"上：窗口每推进 1 个样本，同一列
    //   统计的样本集合就整体平移，峰值会在相邻列之间来回跳 —— 历史跨度调大
    //   （每列覆盖十几个样本）时就是肉眼可见的"毛刺 + 闪烁"。
    //   现在列边界钉在**绝对样本序号**上，整条曲线再按窗口左边缘的小数相位
    //   平移：画面连续滚动，峰值不会再跳列。
    const float samplesPerCol = (float) N / (float) drawN;
    const juce::int64 winStartAbs = (juce::int64) crestHistTotal - (juce::int64) N;
    const double colPos = (double) winStartAbs / (double) juce::jmax (1.0e-6f, samplesPerCol);
    const juce::int64 firstBin = (juce::int64) std::floor (colPos);
    const float frac = (float) (colPos - (double) firstBin);
    const float pxPerCol = w / (float) drawN;

    auto ringAt = [this] (juce::int64 absIdx) noexcept
    {
        if (absIdx < 0) return 0.0f;
        return crestHist[(size_t) (absIdx % (juce::int64) crestHistLen)];
    };

    juce::Path path;
    bool started = false;
    for (int j = 0; j <= drawN; ++j)
    {
        // 该绘制列对应的绝对样本区间 [bin*spc, (bin+1)*spc)
        const juce::int64 bin = firstBin + (juce::int64) j;
        const juce::int64 s0 = (juce::int64) std::llround ((double) bin * (double) samplesPerCol);
        const juce::int64 s1 = juce::jmax (s0 + 1, (juce::int64) std::llround ((double) (bin + 1) * (double) samplesPerCol));

        // 只统计落在显示窗口内、且已经写入的部分
        const juce::int64 a0 = juce::jmax (s0, winStartAbs);
        const juce::int64 a1 = juce::jmin (s1, (juce::int64) crestHistTotal);
        if (a1 <= a0) continue;   // 该列全部超出写入位置（右边缘的那半列）

        float v = 0.0f;
        for (juce::int64 a = a0; a < a1; ++a)
            v = juce::jmax (v, ringAt (a));

        const float x = (float) plot.getX() + ((float) j - frac) * pxPerCol;
        const float norm = juce::jlimit(0.0f, 1.0f, v / 30.0f);
        const float y = (float) plot.getBottom() - norm * h;

        if (! started) { path.startNewSubPath(x, y); started = true; }
        else           path.lineTo(x, y);
    }
    if (! started) return;

    // 曲线向右会多出一列（子列偏移），统一裁到绘图区内
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (plot);

    juce::Path fill = path;
    fill.lineTo((float) plot.getRight(),  (float) plot.getBottom());
    fill.lineTo((float) plot.getX(),      (float) plot.getBottom());
    fill.closeSubPath();
    g.setColour(PinkXP::pink300.withAlpha(0.3f));
    g.fillPath(fill);

    g.setColour(PinkXP::pink500.withAlpha(0.4f));
    g.strokePath(path, juce::PathStrokeType(2.5f));
    g.setColour(PinkXP::pink600);
    g.strokePath(path, juce::PathStrokeType(1.3f));
}
