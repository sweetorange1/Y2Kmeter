#include "source/ui/modules/DynamicsModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// DynamicsModule
// ==========================================================

DynamicsModule::DynamicsModule(AnalyserHub& h)
    : ModulePanel(ModuleType::dynamics),
      hub(h)
{
    // Phase F：订阅 Dynamics + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Dynamics);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // dynamics 6×4 大格

    bPeakL.label = "PK L";
    bPeakR.label = "PK R";
    bRmsL .label = "RMS L";
    bRmsR .label = "RMS R";

    crestHist.fill(0.0f);
    lastTickTime = juce::Time::getCurrentTime();
}

DynamicsModule::~DynamicsModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Dynamics);
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调（UI 线程）
// ----------------------------------------------------------
void DynamicsModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Dynamics)) return;

    // v2.9：使用分发器提供的高精度帧间隔（旧实现用 Time::getCurrentTime 整数毫秒，
    //   120Hz 下在 8/9ms 间跳动，平滑会抖）
    const float dtSec = frame.dtSec;
    lastTickTime = juce::Time::getCurrentTime();

    const auto& s = frame.dynamics;
    bPeakL.update(s.peakL, dtSec);
    bPeakR.update(s.peakR, dtSec);
    bRmsL .update(s.rmsL,  dtSec);
    bRmsR .update(s.rmsR,  dtSec);

    auto smoothNum = [dtSec](float cur, float target, float a)
    {
        if (! std::isfinite(target)) target = 0.0f;
        return cur + y2k::pacing::alphaForDt (a, dtSec) * (target - cur);
    };
    smoothedShortDR = smoothNum(smoothedShortDR, s.shortDR,      0.2f);
    smoothedIntegDR = smoothNum(smoothedIntegDR, s.integratedDR, 0.05f);
    smoothedCrest   = smoothNum(smoothedCrest,   s.crest,        0.3f);

    // 历史带写入（按时间推进，帧率无关）
    crestHistAccum += dtSec * y2k::pacing::kRefHz;
    int pushes = juce::jmin ((int) crestHistAccum, crestHistLen);
    crestHistAccum -= (float) (int) crestHistAccum;
    const float crestVal = juce::jlimit(0.0f, 40.0f, s.crest);
    while (pushes-- > 0)
    {
        crestHist[(size_t) crestHistWrite] = crestVal;
        crestHistWrite = (crestHistWrite + 1) % crestHistLen;
    }

    // v2.9.2：读数类模块 30Hz 重绘（数据每帧更新，仅重绘分频）
    if ((frame.tickCount & 1ull) != 0)
        return;
    repaint();
}

// ----------------------------------------------------------
// layoutContent
// ----------------------------------------------------------
void DynamicsModule::layoutContent(juce::Rectangle<int> content)
{
    // 布局优先级（窗口拉大时）：
    //   · 左侧 Meters 固定 160px（四柱表够用即可）
    //   · 中间 DR 数字区固定约 180px（容纳 22pt 大数字 + 单位，再大没有视觉收益）
    //   · 右侧 CREST HISTORY 图吃掉所有剩余宽度 —— 这样拉宽窗口时优先放大右侧图
    //   · 两处 6px 间隙
    const int metersW = 160;
    const int drW     = 180;
    constexpr int gap = 6;

    // 兜底：窗口极窄时至少给 Crest 120px
    const int minCrestW = 120;
    int crestW = content.getWidth() - metersW - drW - gap * 2;
    if (crestW < minCrestW)
    {
        // 窗口不够宽时退化：按原"DR 吸收剩余"策略，但已被 setMinSize 保障不太会触发
        crestW = juce::jmax(minCrestW, content.getWidth() * 3 / 10);
    }

    areaMeters = content.withWidth(metersW);
    areaCrest  = content.withX(content.getRight() - crestW).withWidth(crestW);
    areaDr     = content.withTrimmedLeft(metersW + gap)
                        .withTrimmedRight(crestW + gap);
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
    drawDrPanel   (g, areaDr);
    drawCrestBand (g, areaCrest);

    // 鼠标悬停标尺（Crest 历史带）
    if (hoverActive)
        drawHoverRuler(g);
}

void DynamicsModule::mouseMove(const juce::MouseEvent& e)
{
    ModulePanel::mouseMove(e);

    const auto pos    = e.getPosition();
    const bool inside = areaCrest.contains(pos);
    if (inside != hoverActive || (inside && pos != hoverPos))
    {
        hoverActive = inside;
        hoverPos    = pos;
        repaint();
    }
}

void DynamicsModule::mouseExit(const juce::MouseEvent& e)
{
    ModulePanel::mouseExit(e);
    if (hoverActive)
    {
        hoverActive = false;
        repaint();
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

    // X → 相对时间（120 帧 × 30Hz ≈ 4s，最右=当前，负=过去）
    const float t = (float)(hoverPos.x - plot.getX())
                  / juce::jmax(1, plot.getWidth());
    const float timeSec = (t - 1.0f) * 4.0f;

    // Y → crest 值（0 ~ 30 dB）
    const float norm = juce::jlimit(0.0f, 1.0f,
        (float)(plot.getBottom() - hoverPos.y) / juce::jmax(1, plot.getHeight()));
    const float crestDb = norm * 30.0f;

    const juce::String readout = juce::String(timeSec, 1) + " s  "
                               + juce::String(crestDb, 1) + " dB";
    PinkXP::drawHoverRuler(g, plot, hoverPos, readout);
}

// ----------------------------------------------------------
// 四柱 Meter
// ----------------------------------------------------------
void DynamicsModule::drawMeterGroup(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    const int gap = 4;
    const int barCount = 4;
    const int labelH = 14;

    auto barRow = area.withTrimmedBottom(labelH);
    const int barW = juce::jmax(8, (barRow.getWidth() - gap * (barCount - 1)) / barCount);

    const MeterBar* bars[4] = { &bPeakL, &bPeakR, &bRmsL, &bRmsR };

    for (int i = 0; i < barCount; ++i)
    {
        juce::Rectangle<int> r(barRow.getX() + i * (barW + gap),
                               barRow.getY(), barW, barRow.getHeight());
        drawSingleBar(g, r, *bars[i]);

        // 标签
        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawText(bars[i]->label,
                   r.getX(), area.getBottom() - labelH, r.getWidth(), labelH,
                   juce::Justification::centred, false);
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
        // row=0 在屏幕顶部（高电平区）→ 0 dB → 红色；
        // row=rows-1 在屏幕底部（低电平区）→ -60 dB → 绿色。
        // 之前 rowDb 的 jmap 方向写反了，导致上绿下红，这里翻转一下。
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
// DR 面板：SHORT / INTEG 两个大数字 + CREST 小数字
// ----------------------------------------------------------
void DynamicsModule::drawDrPanel(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    PinkXP::drawSunken(g, area, PinkXP::content);

    auto inner = area.reduced(8);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int rowH = inner.getHeight() / 3;

    auto drawRow = [&](int rowIdx, const juce::String& title, float value,
                       const juce::String& unit, float big)
    {
        juce::Rectangle<int> row(inner.getX(), inner.getY() + rowIdx * rowH,
                                 inner.getWidth(), rowH);

        // ① 标题：DR 面板默认只有 ~92px 宽（layoutContent 的兜底分支），标题按
        //    getFont(9) 放大后的字宽会越过本行右边界，越过部分随后被绘制顺序更晚
        //    的 CREST 面板整段盖掉（表现为 "DR SHORT" 只看到 "DR SHOR"）。
        //    改用 drawFittedText 自动横向压缩，标题永远收在本行宽度内。
        const int titleH = juce::jlimit(8, 12, row.getHeight() / 4);
        g.setColour(PinkXP::ink.withAlpha(0.85f));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawFittedText(title, row.removeFromTop(titleH),
                         juce::Justification::topLeft, 1, 0.8f);

        // ② 单位 "dB" 移到数值下方（原实现占用右侧 28px，把大数字挤得只剩整数位）。
        //    面板被拖得很矮时不再单独留单位行，优先保证数值本身还有高度可用。
        const int unitH = (row.getHeight() >= 26) ? juce::jlimit(8, 12, row.getHeight() / 4)
                                                  : 0;
        auto unitRect = (unitH > 0) ? row.removeFromBottom(unitH)
                                    : juce::Rectangle<int>();

        // ③ 数值：整行宽度都可用来画大数字（1 位小数），字号按可用宽度收缩。
        //    · 固定样本 "88.8" 推算字号，避免读数位数变化时字号/基线抖动；
        //    · axisFont=true 走不放大路径，位图高度 ≈ px+2，可直接按行高约束。
        const juce::String numText = juce::String(value, 1);

        float px = big;
        {
            const juce::Font probeFont = PinkXP::getAxisFont(px, juce::Font::bold);
            // 以固定样本决定字号（稳定、不随位数抖动）；万一实际读数比样本更长，
            // 再按实际宽度补一次收缩，保证末位小数不被裁掉。
            const float needW = juce::jmax(probeFont.getStringWidthFloat("88.8"),
                                           probeFont.getStringWidthFloat(numText));
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
            drValueCache_[rowIdx] = PinkXP::cachedStaticTextImage (
                numText, px, juce::Font::bold, PinkXP::pink600,
                juce::Colours::transparentBlack, true);
        }

        // 裁剪到数值区：即使字号推算有偏差，也绝不会溢出去压到单位/标题/别的面板
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (row);

            const auto& numImg = drValueCache_[rowIdx];
            if (numImg.isValid())
            {
                g.drawImageAt (numImg,
                               row.getX(),
                               row.getY() + juce::jmax(0, (row.getHeight() - numImg.getHeight()) / 2),
                               false);
            }
            else
            {
                g.setColour(PinkXP::pink600);
                g.setFont(PinkXP::getAxisFont(px, juce::Font::bold));
                g.drawText(numText, row, juce::Justification::centredLeft, false);
            }
        }

        // ④ 单位（静态 → 位图缓存，紧贴数值下方的空余空间，左对齐）
        if (unitRect.isEmpty())
            return;

        const juce::Image unitImg = PinkXP::cachedStaticTextImage (
            unit, 9.0f, juce::Font::plain, PinkXP::ink.withAlpha(0.75f),
            juce::Colours::transparentBlack, true);
        if (unitImg.isValid())
            g.drawImageAt (unitImg, unitRect.getX(), unitRect.getY(), false);
        else
        {
            g.setColour(PinkXP::ink.withAlpha(0.75f));
            g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
            g.drawText(unit, unitRect, juce::Justification::topLeft, false);
        }
    };

    // 三行读数用同一字号（getAxisFont 口径，不再 1.5× 放大）
    drawRow(0, "DR SHORT", smoothedShortDR, "dB", 28.0f);
    drawRow(1, "DR INTEG", smoothedIntegDR, "dB", 28.0f);
    drawRow(2, "CREST",    smoothedCrest,   "dB", 28.0f);
}

// ----------------------------------------------------------
// Crest 历史带
// ----------------------------------------------------------
void DynamicsModule::drawCrestBand(juce::Graphics& g, juce::Rectangle<int> area) const
{
    if (area.isEmpty()) return;

    PinkXP::drawSunken(g, area, PinkXP::content);
    auto inner = area.reduced(4);
    if (inner.getWidth() <= 4 || inner.getHeight() <= 4) return;

    // 标题
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText("CREST HISTORY",
               inner.withHeight(12), juce::Justification::topLeft, false);

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

    // 数据路径
    juce::Path path;
    const int N = crestHistLen;
    const float w = (float) plot.getWidth();
    const float h = (float) plot.getHeight();

    for (int i = 0; i < N; ++i)
    {
        const int idx = (crestHistWrite + i) % N;
        const float v = crestHist[(size_t) idx];
        const float t = (float) i / (float) juce::jmax(1, N - 1);
        const float x = (float) plot.getX() + t * w;
        const float norm = juce::jlimit(0.0f, 1.0f, v / 30.0f);
        const float y = (float) plot.getBottom() - norm * h;

        if (i == 0) path.startNewSubPath(x, y);
        else        path.lineTo(x, y);
    }

    // 填充
    juce::Path fill = path;
    fill.lineTo((float) plot.getRight(),  (float) plot.getBottom());
    fill.lineTo((float) plot.getX(),      (float) plot.getBottom());
    fill.closeSubPath();
    g.setColour(PinkXP::pink300.withAlpha(0.3f));
    g.fillPath(fill);

    // 主曲线
    g.setColour(PinkXP::pink500.withAlpha(0.4f));
    g.strokePath(path, juce::PathStrokeType(2.5f));
    g.setColour(PinkXP::pink600);
    g.strokePath(path, juce::PathStrokeType(1.3f));
}
