#include "source/ui/modules/DynamicsModule.h"
#include "source/ui/PinkXPStyle.h"
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

    const auto now = juce::Time::getCurrentTime();
    const float dMs = (float)(now - lastTickTime).inMilliseconds();
    lastTickTime = now;

    const auto& s = frame.dynamics;
    bPeakL.update(s.peakL, dMs);
    bPeakR.update(s.peakR, dMs);
    bRmsL .update(s.rmsL,  dMs);
    bRmsR .update(s.rmsR,  dMs);

    auto smoothNum = [](float cur, float target, float a)
    {
        if (! std::isfinite(target)) target = 0.0f;
        return cur + a * (target - cur);
    };
    smoothedShortDR = smoothNum(smoothedShortDR, s.shortDR,      0.2f);
    smoothedIntegDR = smoothNum(smoothedIntegDR, s.integratedDR, 0.05f);
    smoothedCrest   = smoothNum(smoothedCrest,   s.crest,        0.3f);

    // 历史带写入
    crestHist[(size_t) crestHistWrite] = juce::jlimit(0.0f, 40.0f, s.crest);
    crestHistWrite = (crestHistWrite + 1) % crestHistLen;

    // 性能优化（阶段1）：节流 repaint。
    //   Hub 可能以 60~100Hz 回调 onFrame，但 Dynamics 显示的峰值/DR 数字
    //   肉眼 ~30Hz 已足够。这里用最小刷新间隔（高DPI适度放大）避免每次都刷。
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float  scale  = (float) juce::jmax (1.0, (double) juce::Component::getApproximateScaleFactorForComponent (this));
    const double minRepaintIntervalMs = 20.0 * (double) juce::jmin (1.8f, scale);
    if ((nowMs - lastRepaintMs) < minRepaintIntervalMs)
        return;

    lastRepaintMs = nowMs;
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
// paintContent
// ----------------------------------------------------------
void DynamicsModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    drawMeterGroup(g, areaMeters);
    drawDrPanel   (g, areaDr);
    drawCrestBand (g, areaCrest);
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
        // 标题
        g.setColour(PinkXP::ink.withAlpha(0.85f));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawText(title, row.withHeight(12), juce::Justification::topLeft, false);

        // 数值 / 单位：把数值区域收紧到"行内去掉右侧单位宽度"，
        //   避免大号数值与单位右侧 "dB" 在窄窗口下视觉重叠
        auto numArea  = row.withTrimmedTop(12);
        constexpr int unitW = 28;
        auto unitArea = numArea.removeFromRight(unitW);

        // 数值（大号，左对齐，数值右侧保留 unitW 给单位）
        g.setColour(PinkXP::pink600);
        g.setFont(PinkXP::getFont(big, juce::Font::bold));
        g.drawText(juce::String(value, 1),
                   numArea,
                   juce::Justification::centredLeft, false);

        // 单位（小号，左对齐贴在数值右侧 unitArea）
        g.setColour(PinkXP::ink.withAlpha(0.75f));
        g.setFont(PinkXP::getFont(10.0f, juce::Font::plain));
        g.drawText(unit,
                   unitArea,
                   juce::Justification::centredLeft, false);
    };

    drawRow(0, "DR SHORT",    smoothedShortDR, "dB", 22.0f);
    drawRow(1, "DR INTEG",    smoothedIntegDR, "dB", 22.0f);
    drawRow(2, "CREST FACT.", smoothedCrest,   "dB", 16.0f);
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
