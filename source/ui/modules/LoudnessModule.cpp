#include "source/ui/modules/LoudnessModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// LoudnessModule
// ==========================================================
LoudnessModule::LoudnessModule(AnalyserHub& h)
    : ModulePanel(ModuleType::loudness),
      hub(h)
{
    // Phase F：订阅 Loudness + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Loudness);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(320, 256); // loudness 5×4 大格

    // 初始化标签
    barM.label = "M";
    barS.label = "S";
    barI.label = "I";
    barL.label = "L";
    barR.label = "R";

    lastTickTime = juce::Time::getCurrentTime();
}

LoudnessModule::~LoudnessModule()
{
    // Phase F：先解绑 FrameListener（避免在析构过程中再被回调）
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Loudness);
}

// ==========================================================
// onFrame —— Hub 分发器 30Hz UI 线程回调
// ==========================================================
void LoudnessModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Loudness))
        return; // 本帧 Loudness 路没有活跃

    const auto now = juce::Time::getCurrentTime();
    const float deltaMs = (float)(now - lastTickTime).inMilliseconds();
    lastTickTime = now;

    const auto& snap = frame.loudness;

    barM.update(snap.lufsM, deltaMs);
    barS.update(snap.lufsS, deltaMs);
    barI.update(snap.lufsI, deltaMs);
    barL.update(snap.rmsL,  deltaMs);
    barR.update(snap.rmsR,  deltaMs);

    repaint();
}

// ==========================================================
// valueToNorm —— 值 → 柱高比例 [0,1]
// LUFS 范围：-60 ~ 0 LUFS
// dBFS 范围：-60 ~ 0 dBFS
// ==========================================================
float LoudnessModule::valueToNorm(float val, bool /*isLUFS*/) noexcept
{
    // 统一映射到 -60 ~ 0 dB 范围
    return juce::jlimit(0.0f, 1.0f, juce::jmap(val, -60.0f, 0.0f, 0.0f, 1.0f));
}

// ==========================================================
// 颜色分区
// ==========================================================
juce::Colour LoudnessModule::lufsToColour(float lufs) noexcept
{
    if (lufs > -9.0f)  return juce::Colour(0xffec4d85);  // 红（过响）
    if (lufs > -14.0f) return juce::Colour(0xffffcc44);  // 黄（适中）
    return juce::Colour(0xff66cc88);                      // 绿（正常）
}

juce::Colour LoudnessModule::dbToColour(float db) noexcept
{
    if (db > -3.0f)  return juce::Colour(0xffec4d85);
    if (db > -9.0f)  return juce::Colour(0xffffcc44);
    return juce::Colour(0xff66cc88);
}

// ==========================================================
// drawMeterBar —— 绘制单条像素柱状表
// ==========================================================
void LoudnessModule::drawMeterBar(juce::Graphics& g,
                                   juce::Rectangle<int> barArea,
                                   const MeterBar& bar,
                                   bool isLUFS) const
{
    // 凹陷背景
    PinkXP::drawSunken(g, barArea, PinkXP::content.darker(0.05f));

    const int padding = 3;
    auto inner = barArea.reduced(padding);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int step = cellSize + cellGap;
    const int numRows = juce::jmax(1, (inner.getHeight() + cellGap) / step);

    // 计算点亮行数
    const float norm = valueToNorm(bar.smoothed, isLUFS);
    const int litRows = (int)std::round(norm * (float)numRows);

    // 计算峰值行
    const float peakNorm = valueToNorm(bar.peakValue, isLUFS);
    const int peakRow = (int)std::round(peakNorm * (float)numRows);

    // 居中绘制
    const int totalH = numRows * step - cellGap;
    const int offsetY = inner.getY() + (inner.getHeight() - totalH) / 2;
    // 灯带宽度：随柱容器宽度自适应（不再固定为 cellSize*3=18px）。
    //   · 保留左右各 2px 安全边距，避免贴死凹陷描边
    //   · 不再硬钳制，窗口越宽灯带越宽
    const int colW = juce::jmax(1, inner.getWidth() - 4);
    const int offsetX = inner.getX() + (inner.getWidth() - colW) / 2;

    for (int row = 0; row < numRows; ++row)
    {
        const int y = offsetY + row * step;
        const bool lit = (numRows - 1 - row) < litRows;

        // 对应的 dB 值（用于颜色分区）
        const float rowNorm = 1.0f - (float)row / (float)juce::jmax(1, numRows - 1);
        const float rowDb   = juce::jmap(rowNorm, -60.0f, 0.0f);

        // 与 DynamicsModule::drawSingleBar 对齐：未点亮格子用同色 0.15 alpha
        //   （而不是 pink50 单色），这样视觉上是"色带随电平从底向上点亮"
        const auto base = isLUFS ? lufsToColour(rowDb) : dbToColour(rowDb);
        g.setColour(lit ? base : base.withAlpha(0.15f));
        g.fillRect(offsetX, y, colW, cellSize);
    }

    // 缓降峰值线（与 DynamicsModule 一致：当前 peak 值对应颜色 brighter 0.2）
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
void LoudnessModule::drawScale(juce::Graphics& g,
                                juce::Rectangle<int> scaleArea,
                                bool /*isLUFS*/) const
{
    g.setFont(PinkXP::getAxisFont(9.0f));
    g.setColour(PinkXP::ink.withAlpha(0.75f));

    const float top    = (float)scaleArea.getY();
    const float bottom = (float)scaleArea.getBottom();
    const float height = bottom - top;

    // 刻度：0, -6, -12, -18, -24, -36, -48, -60
    const int marks[] = { 0, -6, -12, -18, -24, -36, -48, -60 };
    for (int db : marks)
    {
        const float norm = juce::jmap((float)db, -60.0f, 0.0f, 0.0f, 1.0f);
        const float y    = bottom - norm * height;

        // 刻度线
        g.setColour(PinkXP::pink600.withAlpha(0.5f));
        g.drawHorizontalLine((int)y, (float)scaleArea.getX(),
                             (float)(scaleArea.getX() + 4));

        // 标签
        g.setColour(PinkXP::ink.withAlpha(0.8f));
        juce::String label = (db == 0) ? "0" : juce::String(db);
        g.drawText(label,
                   scaleArea.getX() + 5, (int)y - 5,
                   scaleArea.getWidth() - 5, 10,
                   juce::Justification::centredLeft, false);
    }
}

// ==========================================================
// layoutContent
// ==========================================================
void LoudnessModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto area = contentBounds.reduced(6);

    // 右侧刻度尺（30px）
    areaScale = area.removeFromRight(30);
    area.removeFromRight(4);

    // 底部标签 + 读数区（labelH + readoutH）
    const int bottomH = labelH + readoutH;
    auto bottomArea = area.removeFromBottom(bottomH);
    juce::ignoreUnused(bottomArea);

    // 5 条柱等宽分布
    const int barCount = 5;
    const int gap = 4;
    const int barW = (area.getWidth() - gap * (barCount - 1)) / barCount;

    auto makeBar = [&](int idx) -> juce::Rectangle<int>
    {
        return juce::Rectangle<int>(
            area.getX() + idx * (barW + gap),
            area.getY(),
            barW,
            area.getHeight());
    };

    areaM = makeBar(0);
    areaS = makeBar(1);
    areaI = makeBar(2);
    areaL = makeBar(3);
    areaR = makeBar(4);
}

// ==========================================================
// paintContent
// ==========================================================
void LoudnessModule::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    // 内容区底色（深色主题下也保持浅色）
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    // 绘制 5 条柱
    drawMeterBar(g, areaM, barM, true);
    drawMeterBar(g, areaS, barS, true);
    drawMeterBar(g, areaI, barI, true);
    drawMeterBar(g, areaL, barL, false);
    drawMeterBar(g, areaR, barR, false);

    // 绘制刻度尺
    drawScale(g, areaScale, true);

    // 底部标签 + 数字读数
    auto area = contentBounds.reduced(6);
    area.removeFromRight(34);  // 刻度尺宽度

    const int barCount = 5;
    const int gap = 4;
    const int barW = juce::jmax(1, (area.getWidth() - gap * (barCount - 1)) / barCount);

    // 无穷符号字符串（UTF-8：E2 88 9E）作为常量，避免每次进 lambda 重新构造
    static const juce::String kMinusInf = juce::String::fromUTF8("-\xe2\x88\x9e");

    const auto drawLabel = [&](int idx, const MeterBar& bar, bool isLUFS)
    {
        const int x = area.getX() + idx * (barW + gap);
        const int labelY = area.getBottom() - labelH - readoutH;

        // 标签
        g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
        g.setColour(PinkXP::ink);
        g.drawText(bar.label,
                   x, labelY, barW, labelH,
                   juce::Justification::centred, false);

        // 数字读数（防御：NaN / Inf / 过小 → 显示 -∞；其余数值做 clamp）
        const float raw = bar.smoothed;
        const bool  isFinite = std::isfinite(raw);
        const float val = isFinite ? raw : -144.0f;

        juce::String readout;
        if (!isFinite || val <= -60.0f)
            readout = kMinusInf;
        else
            readout = juce::String(juce::jlimit(-60.0f, 60.0f, val), 1);

        g.setFont(PinkXP::getFont(9.0f));
        g.setColour(isLUFS ? lufsToColour(val) : dbToColour(val));
        g.drawText(readout,
                   x, labelY + labelH, barW, readoutH,
                   juce::Justification::centred, false);

        juce::ignoreUnused(isLUFS);
    };

    drawLabel(0, barM, true);
    drawLabel(1, barS, true);
    drawLabel(2, barI, true);
    drawLabel(3, barL, false);
    drawLabel(4, barR, false);

    // 分隔线：LUFS 组 / RMS 组
    {
        const int sepX = area.getX() + 3 * (barW + gap) - gap / 2;
        g.setColour(PinkXP::shdw.withAlpha(0.4f));
        g.drawVerticalLine(sepX, (float)contentBounds.getY() + 6,
                           (float)contentBounds.getBottom() - 6);
    }
}
