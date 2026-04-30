#include "source/ui/modules/FineSplitModules.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"

namespace
{
    float mapDbToNorm(float db, float minDb = -60.0f, float maxDb = 0.0f)
    {
        return juce::jlimit(0.0f, 1.0f, juce::jmap(db, minDb, maxDb, 0.0f, 1.0f));
    }

    juce::Colour meterColour(float db)
    {
        if (db > -3.0f)  return juce::Colour(0xffec4d85);
        if (db > -9.0f)  return juce::Colour(0xffffcc44);
        return juce::Colour(0xff66cc88);
    }

    // 画一条横向像素风进度条（Y2K XP 风）：sunken 槽 + 像素格填充 + 右端高亮像素
    //   · tNorm 已归一化到 [0,1]
    //   · 改用"像素格"填充 + 脱离格流动高光——让用户明显看到"数据在跑"
    void drawReadoutBar(juce::Graphics& g, juce::Rectangle<int> bar, float tNorm,
                        float pulsePhase = 0.0f)
    {
        if (bar.getWidth() <= 4 || bar.getHeight() <= 3) return;
        tNorm = juce::jlimit(0.0f, 1.0f, tNorm);

        // 槽（下沉效果）
        PinkXP::drawSunken(g, bar, PinkXP::content.darker(0.04f));
        auto inner = bar.reduced(2);
        if (inner.getWidth() <= 2 || inner.getHeight() <= 1) return;

        // 像素格填充：每格 5px 宽 1px 间隙，达到"像素 LED 条"视觉
        const int cellW = 5;
        const int gap   = 1;
        const int step  = cellW + gap;
        const int totalCells = juce::jmax(1, (inner.getWidth() + gap) / step);
        const int litCells   = juce::jlimit(0, totalCells,
                                             (int) std::round(tNorm * (float) totalCells));

        for (int i = 0; i < totalCells; ++i)
        {
            const int x = inner.getX() + i * step;
            const float t = (float) i / (float) juce::jmax(1, totalCells - 1);
            // 颜色：低值→浅粉，高值→深粉 / 红（广播表观感）
            juce::Colour base;
            if (t < 0.7f)       base = PinkXP::pink300.interpolatedWith(PinkXP::pink600, t / 0.7f);
            else if (t < 0.9f)  base = juce::Colour(0xffffcc44);
            else                base = juce::Colour(0xffec4d85);

            if (i < litCells)
            {
                // 最右一个点亮的格叠加脂点亮度（每帧 pulsePhase 变化→身活跳动感）
                if (i == litCells - 1)
                {
                    const float pulse = 0.75f + 0.25f * std::sin(pulsePhase);
                    g.setColour(base.brighter(0.15f * pulse).withAlpha(1.0f));
                }
                else
                {
                    g.setColour(base);
                }
            }
            else
            {
                g.setColour(base.withAlpha(0.15f));
            }
            g.fillRect(x, inner.getY(), cellW, inner.getHeight());
        }
    }

    // 重载：带"进度条"的读数面板。tNorm ∈ [0,1]；<0 表示不画条
    //   flash ∈ [0,1]: 数值刚变化时走 1 然后衰减，用于数值文字的"闪光"高亮
    //   pulsePhase ∈ [0, 2π]: 进度条右端心跳脂点、以及右上角活性方块
    void drawReadout(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label,
                     float v, const juce::String& unit, float tNorm,
                     float flash = 0.0f, float pulsePhase = 0.0f)
    {
        PinkXP::drawSunken(g, r, PinkXP::content);
        auto inner = r.reduced(6);

        // 底部预留 8px 画进度条（比之前 5px 高）
        juce::Rectangle<int> barArea;
        if (tNorm >= 0.0f && inner.getHeight() >= 24)
        {
            constexpr int barH = 8;
            barArea = inner.removeFromBottom(barH);
            inner.removeFromBottom(2);
        }

        // 标题行：标签 + 右上角"活性心跳"方块（闪烁抖动→证明数据在跑）
        auto titleRow = inner.removeFromTop(12);
        g.setColour(PinkXP::ink.withAlpha(0.85f));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawText(label, titleRow, juce::Justification::centredLeft, false);

        // 右上角活活方块 6x6：心跳亮度脉动
        {
            const int sz = 6;
            auto dot = juce::Rectangle<int>(titleRow.getRight() - sz - 2,
                                             titleRow.getY() + (titleRow.getHeight() - sz) / 2,
                                             sz, sz);
            const float pulse = 0.5f + 0.5f * std::sin(pulsePhase);
            g.setColour(PinkXP::pink500.withAlpha(0.35f + 0.55f * pulse));
            g.fillRect(dot);
            g.setColour(PinkXP::pink700.withAlpha(0.6f));
            g.drawRect(dot, 1);
        }

        // 数值：刚变化时颜色闪白 + 高亮（flash 从 1 衰减到 0）
        const auto baseNumCol = PinkXP::pink600;
        const auto flashCol   = juce::Colour(0xffffffff); // 纯白高光
        g.setColour(baseNumCol.interpolatedWith(flashCol, juce::jlimit(0.0f, 1.0f, flash) * 0.65f));
        g.setFont(PinkXP::getFont(20.0f, juce::Font::bold));
        g.drawText(juce::String(v, 1), inner, juce::Justification::centredLeft, false);

        g.setColour(PinkXP::ink.withAlpha(0.7f));
        g.setFont(PinkXP::getFont(10.0f, juce::Font::plain));
        g.drawText(unit, inner, juce::Justification::centredRight, false);

        if (! barArea.isEmpty())
            drawReadoutBar(g, barArea, tNorm, pulsePhase);
    }

    // 兼容旧签名（其它模块若不需要进度条仍可直接调用）
    void drawReadout(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label, float v, const juce::String& unit)
    {
        drawReadout(g, r, label, v, unit, -1.0f, 0.0f, 0.0f);
    }
}

LufsRealtimeModule::LufsRealtimeModule(AnalyserHub& h)
    : ModulePanel(ModuleType::lufsRealtime), hub(h)
{
    // Phase F：订阅 Loudness + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Loudness);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(320, 256); // LufsRealtime → loudness 5×4 大格
    setTitleText("LUFS Real-time");
}

LufsRealtimeModule::~LufsRealtimeModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Loudness);
}

void LufsRealtimeModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Loudness)) return;

    const auto& s = frame.loudness;

    // 记录上一帧值用于"显著变化"检测→吸打闪烁
    prevM = lufsM; prevS = lufsS; prevI = lufsI;
    lufsM = lufsM + 0.30f * (s.lufsM - lufsM);
    lufsS = lufsS + 0.20f * (s.lufsS - lufsS);
    lufsI = lufsI + 0.08f * (s.lufsI - lufsI);

    // 变化 > 0.3 LUFS 就触发闪烁；flash 按每帧 0.08 衰减
    if (std::abs(lufsM - prevM) > 0.3f) flashM = 1.0f;
    if (std::abs(lufsS - prevS) > 0.3f) flashS = 1.0f;
    if (std::abs(lufsI - prevI) > 0.2f) flashI = 1.0f;
    flashM = juce::jmax(0.0f, flashM - 0.08f);
    flashS = juce::jmax(0.0f, flashS - 0.08f);
    flashI = juce::jmax(0.0f, flashI - 0.06f);

    // 心跳相位每帧推进（30fps 下约 0.86s 一个脉动周期）
    pulsePhase += 0.24f;
    if (pulsePhase > juce::MathConstants<float>::twoPi)
        pulsePhase -= juce::MathConstants<float>::twoPi;

    repaint();
}

void LufsRealtimeModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    auto area = content.reduced(6);
    const int h = juce::jmax(44, area.getHeight() / 3 - 3);

    auto lufsToNorm = [](float l) { return juce::jlimit(0.0f, 1.0f, juce::jmap(l, -40.0f, 0.0f, 0.0f, 1.0f)); };
    drawReadout(g, area.removeFromTop(h), "LUFS-M", lufsM, "LUFS", lufsToNorm(lufsM), flashM, pulsePhase);
    area.removeFromTop(3);
    drawReadout(g, area.removeFromTop(h), "LUFS-S", lufsS, "LUFS", lufsToNorm(lufsS), flashS, pulsePhase + 2.0f);
    area.removeFromTop(3);
    drawReadout(g, area, "LUFS-I", lufsI, "LUFS", lufsToNorm(lufsI), flashI, pulsePhase + 4.0f);
}

TruePeakModule::TruePeakModule(AnalyserHub& h)
    : ModulePanel(ModuleType::truePeak), hub(h)
{
    // Phase F：订阅 Loudness（True Peak / RMS 都在 LoudnessMeter里）+ 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Loudness);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(256, 192); // truepeak 4×3 大格
    setTitleText("True Peak");
}

TruePeakModule::~TruePeakModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Loudness);
}

void TruePeakModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Loudness)) return;

    const auto& s = frame.loudness;
    const float prevTpMax  = juce::jmax(tpL,  tpR);
    const float prevRmsMax = juce::jmax(rmsL, rmsR);
    prevTp = prevTpMax; prevRms = prevRmsMax;

    tpL  = tpL  + 0.30f * (s.truePeakL - tpL);
    tpR  = tpR  + 0.30f * (s.truePeakR - tpR);
    rmsL = rmsL + 0.12f * (s.rmsL - rmsL);
    rmsR = rmsR + 0.12f * (s.rmsR - rmsR);

    const float tpMax  = juce::jmax(tpL,  tpR);
    const float rmsMax = juce::jmax(rmsL, rmsR);
    if (std::abs(tpMax  - prevTp)  > 0.3f) flashTp  = 1.0f;
    if (std::abs(rmsMax - prevRms) > 0.3f) flashRms = 1.0f;
    flashTp  = juce::jmax(0.0f, flashTp  - 0.08f);
    flashRms = juce::jmax(0.0f, flashRms - 0.08f);

    pulsePhase += 0.24f;
    if (pulsePhase > juce::MathConstants<float>::twoPi)
        pulsePhase -= juce::MathConstants<float>::twoPi;

    repaint();
}

void TruePeakModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    auto area = content.reduced(6);
    auto top = area.removeFromTop(area.getHeight() / 2 - 2);
    area.removeFromTop(4);

    auto dbToNorm = [](float d) { return juce::jlimit(0.0f, 1.0f, juce::jmap(d, -40.0f, 0.0f, 0.0f, 1.0f)); };
    const float tpMax  = juce::jmax(tpL,  tpR);
    const float rmsMax = juce::jmax(rmsL, rmsR);
    drawReadout(g, top,  "TP-L / TP-R",   tpMax,  "dBTP", dbToNorm(tpMax),  flashTp,  pulsePhase);
    drawReadout(g, area, "RMS-L / RMS-R", rmsMax, "dBFS", dbToNorm(rmsMax), flashRms, pulsePhase + 3.0f);
}

OscilloscopeChannelModule::OscilloscopeChannelModule(AnalyserHub& h, bool leftChannel)
    : ModulePanel(leftChannel ? ModuleType::oscilloscopeLeft : ModuleType::oscilloscopeRight),
      hub(h),
      useLeft(leftChannel)
{
    // Phase F：订阅 Oscilloscope + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // oscilloscope L/R 6×4 大格
    setTitleText(leftChannel ? "Oscilloscope L" : "Oscilloscope R");
}

OscilloscopeChannelModule::~OscilloscopeChannelModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Oscilloscope);
}

void OscilloscopeChannelModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    // Phase F 优化：std::array → juce::Array 改 memcpy 批量拷贝
    const int n = (int) frame.oscL.size();
    oscL.resize(n);
    oscR.resize(n);
    std::memcpy (oscL.getRawDataPointer(), frame.oscL.data(), (size_t) n * sizeof (float));
    std::memcpy (oscR.getRawDataPointer(), frame.oscR.data(), (size_t) n * sizeof (float));
    repaint();
}

void OscilloscopeChannelModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    PinkXP::drawSunken(g, content, PinkXP::content);
    auto inner = content.reduced(6);
    if (inner.getWidth() <= 8 || inner.getHeight() <= 8)
        return;

    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    g.drawHorizontalLine(inner.getCentreY(), (float) inner.getX(), (float) inner.getRight());

    const auto& src = useLeft ? oscL : oscR;
    const int N = src.size();
    if (N <= 1) return;

    juce::Path p;
    const float xStep = (float) inner.getWidth() / (float) juce::jmax(1, N - 1);
    for (int i = 0; i < N; ++i)
    {
        const float s = juce::jlimit(-1.0f, 1.0f, src.getUnchecked(i));
        const float x = (float) inner.getX() + (float) i * xStep;
        const float y = (float) inner.getCentreY() - s * ((float) inner.getHeight() * 0.42f);
        if (i == 0) p.startNewSubPath(x, y);
        else        p.lineTo(x, y);
    }

    g.setColour(PinkXP::pink500.withAlpha(0.35f));
    g.strokePath(p, juce::PathStrokeType(3.0f));
    g.setColour(useLeft ? PinkXP::pink400 : PinkXP::pink600);
    g.strokePath(p, juce::PathStrokeType(1.4f));
}

// SpectrumOverviewModule 已在 2026-04 版本移除（完整的 SpectrumModule 已足够）

PhaseCorrelationModule::PhaseCorrelationModule(AnalyserHub& h)
    : ModulePanel(ModuleType::phaseCorrelation), hub(h)
{
    // Phase F：订阅 Phase + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Phase);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(192, 128); // phase correlation 3×2 大格
    setTitleText("Phase Correlation");
}

PhaseCorrelationModule::~PhaseCorrelationModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Phase);
}

void PhaseCorrelationModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Phase)) return;

    const auto& s = frame.phase;
    corr = corr + 0.25f * (s.correlation - corr);
    repaint();
}

void PhaseCorrelationModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    PinkXP::drawSunken(g, content, PinkXP::content);
    auto inner = content.reduced(10);
    if (inner.getWidth() <= 8 || inner.getHeight() <= 8) return;

    // 规范化：标准 0°~180° 上半圆仪表
    //   · 圆心 = 模块显示窗口的"正下方中点"（底边中点）
    //   · corr = -1  → 指针指向正左  (θ = 0°)
    //   · corr =  0  → 指针指向正上  (θ = 90°)
    //   · corr = +1  → 指针指向正右  (θ = 180°)
    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getBottom();
    const int   readoutReserve = 24;
    const float R  = juce::jmin((float) inner.getWidth() * 0.44f,
                                 (float) inner.getHeight() - (float) readoutReserve);
    if (R < 8.0f) return;

    // 弧形轨（上半圆）
    const float arcStart = -juce::MathConstants<float>::halfPi;
    const float arcEnd   =  juce::MathConstants<float>::halfPi;
    juce::Path arc;
    arc.addCentredArc(cx, cy, R, R, 0.0f, arcStart, arcEnd, true);
    g.setColour(PinkXP::pink300.withAlpha(0.55f));
    g.strokePath(arc, juce::PathStrokeType(5.0f));

    // 指针
    const float t  = juce::jlimit(-1.0f, 1.0f, corr) * 0.5f + 0.5f;
    const float th = t * juce::MathConstants<float>::pi; // 0..π
    const float nx = cx - std::cos(th) * (R - 4.0f);
    const float ny = cy - std::sin(th) * (R - 4.0f);

    g.setColour(corr < 0.0f ? juce::Colour(0xffec4d85) : PinkXP::pink600);
    g.drawLine(cx, cy, nx, ny, 2.0f);
    g.setColour(PinkXP::dark);
    g.fillEllipse(cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);

    // 顶部数值读数
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(16.0f, juce::Font::bold));
    auto txt = juce::String(corr, 2);
    if (corr >= 0.0f) txt = "+" + txt;
    g.drawText(txt, inner.removeFromTop(20), juce::Justification::centred, false);
}

PhaseBalanceModule::PhaseBalanceModule(AnalyserHub& h)
    : ModulePanel(ModuleType::phaseBalance), hub(h)
{
    // Phase F：订阅 Phase + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Phase);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(192, 128); // phase balance 3×2 大格
    setTitleText("Phase Width / Balance");
}

PhaseBalanceModule::~PhaseBalanceModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Phase);
}

void PhaseBalanceModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Phase)) return;

    const auto& s = frame.phase;
    width = width + 0.20f * (s.width - width);
    balance = balance + 0.20f * (s.balance - balance);
    repaint();
}

void PhaseBalanceModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    auto area = content.reduced(8);
    auto wBar = area.removeFromTop(area.getHeight() / 2 - 2);
    area.removeFromTop(4);

    PinkXP::drawSunken(g, wBar, PinkXP::content);
    auto wi = wBar.reduced(4);
    const int ww = (int) std::round(width * wi.getWidth());
    g.setColour(PinkXP::pink400.withAlpha(0.35f));
    g.fillRect(wi.withWidth(ww));
    g.setColour(PinkXP::pink600);
    g.drawRect(wi, 1);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    g.drawText("WIDTH " + juce::String((int) std::round(width * 100.0f)) + "%", wBar.reduced(4), juce::Justification::centredLeft, false);

    PinkXP::drawSunken(g, area, PinkXP::content);
    auto bi = area.reduced(4);
    const int cx = bi.getCentreX();
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    g.drawVerticalLine(cx, (float) bi.getY(), (float) bi.getBottom());
    const int x = cx + (int) std::round(balance * (float) bi.getWidth() * 0.5f);
    g.setColour(PinkXP::pink600);
    g.fillRect(x - 2, bi.getY(), 4, bi.getHeight());

    juce::String btxt;
    if (std::abs(balance) < 0.02f) btxt = "BAL C";
    else if (balance < 0.0f) btxt = "BAL L" + juce::String((int) std::round(-balance * 100.0f));
    else btxt = "BAL R" + juce::String((int) std::round(balance * 100.0f));

    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    g.drawText(btxt, area.reduced(4), juce::Justification::centredLeft, false);
}

DynamicsMetersModule::DynamicsMetersModule(AnalyserHub& h)
    : ModulePanel(ModuleType::dynamicsMeters), hub(h)
{
    // Phase F：订阅 Dynamics + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Dynamics);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(256, 256); // dynamics meters 4×4 大格
    setTitleText("Dynamics Meters");
}

DynamicsMetersModule::~DynamicsMetersModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Dynamics);
}

void DynamicsMetersModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Dynamics)) return;

    const auto& s = frame.dynamics;
    meterDb[0] = meterDb[0] + 0.25f * (s.peakL - meterDb[0]);
    meterDb[1] = meterDb[1] + 0.25f * (s.peakR - meterDb[1]);
    meterDb[2] = meterDb[2] + 0.18f * (s.rmsL  - meterDb[2]);
    meterDb[3] = meterDb[3] + 0.18f * (s.rmsR  - meterDb[3]);
    repaint();
}

void DynamicsMetersModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    auto area = content.reduced(8);
    const int n = 4;
    const int gap = 5;
    const int w = juce::jmax(12, (area.getWidth() - gap * (n - 1)) / n);
    const int labelH = 14;
    static const char* labels[] = { "PK-L", "PK-R", "RMS-L", "RMS-R" };

    // ---- 与 DynamicsModule::drawSingleBar 对齐的像素方格柱 ----
    //   · 凹陷槽 + 每 4px 一行像素格（cellGap=1）
    //   · 底部绿 / 中段黄 / 顶部红（通过行 dB → meterColour 插值）
    //   · 未点亮的格子：同色 withAlpha(0.15) 保留底色提示（而不是全空白）
    //   · 峰值保持线：当前值色彩 brighter(0.2) 2px 亮线
    constexpr int cellH   = 4;
    constexpr int cellGap = 1;
    constexpr int step    = cellH + cellGap;

    for (int i = 0; i < n; ++i)
    {
        auto bar = juce::Rectangle<int>(area.getX() + i * (w + gap),
                                         area.getY(), w, area.getHeight() - labelH);

        // 凹陷槽（底色比 content 再深一点，突出灯带）
        PinkXP::drawSunken(g, bar, PinkXP::content.darker(0.05f));
        auto inner = bar.reduced(3);
        if (inner.getWidth() > 0 && inner.getHeight() > 0)
        {
            const int rows    = juce::jmax(1, (inner.getHeight() + cellGap) / step);
            const float db    = meterDb[(size_t) i];
            const float norm  = mapDbToNorm(db);
            const int   litRows = (int) std::round(norm * (float) rows);

            for (int row = 0; row < rows; ++row)
            {
                // row=0 屏幕顶（0 dB 红色），row=rows-1 屏幕底（-60 dB 绿色）
                const float rowNorm = (float) row / (float) juce::jmax(1, rows - 1);
                const float rowDb   = juce::jmap(rowNorm, 0.0f, 1.0f, 0.0f, -60.0f);
                const bool  lit     = (rows - 1 - row) < litRows;

                const int y = inner.getY() + row * step;
                const auto base = meterColour(rowDb);
                g.setColour(lit ? base : base.withAlpha(0.15f));
                g.fillRect(inner.getX(), y, inner.getWidth(), cellH);
            }

            // 缓降峰值线：这里 DynamicsMetersModule 没有单独 peakHold 状态，
            //   直接用 smoothed db 作为"瞬时峰值"绘制一条 2px 亮线，视觉上
            //   与 DynamicsModule 的 peakHold 线保持一致。
            const int peakY = inner.getBottom() - (int) std::round(norm * inner.getHeight());
            if (peakY >= inner.getY() && peakY <= inner.getBottom())
            {
                g.setColour(meterColour(db).brighter(0.2f));
                g.fillRect(inner.getX(), peakY - 1, inner.getWidth(), 2);
            }
        }

        // 标签（跟随主题 ink 色，已由 PinkXPStyle 全局主题控制）
        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getFont(8.5f, juce::Font::bold));
        g.drawText(labels[i], area.getX() + i * (w + gap), area.getBottom() - (labelH - 2), w, 12,
                   juce::Justification::centred, false);
    }
}

DynamicsDrModule::DynamicsDrModule(AnalyserHub& h)
    : ModulePanel(ModuleType::dynamicsDr), hub(h)
{
    // Phase F：订阅 Dynamics + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Dynamics);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(320, 256); // dynamics DR 5×4 大格
    setTitleText("Dynamics DR");
}

DynamicsDrModule::~DynamicsDrModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Dynamics);
}

void DynamicsDrModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Dynamics)) return;

    const auto& s = frame.dynamics;
    prevShort = shortDR; prevInteg = integDR; prevCrest = crest;
    shortDR = shortDR + 0.20f * (s.shortDR - shortDR);
    integDR = integDR + 0.08f * (s.integratedDR - integDR);
    crest   = crest   + 0.25f * (s.crest - crest);

    if (std::abs(shortDR - prevShort) > 0.3f) flashShort = 1.0f;
    if (std::abs(integDR - prevInteg) > 0.2f) flashInteg = 1.0f;
    if (std::abs(crest   - prevCrest) > 0.3f) flashCrest = 1.0f;
    flashShort = juce::jmax(0.0f, flashShort - 0.08f);
    flashInteg = juce::jmax(0.0f, flashInteg - 0.07f);
    flashCrest = juce::jmax(0.0f, flashCrest - 0.08f);

    pulsePhase += 0.24f;
    if (pulsePhase > juce::MathConstants<float>::twoPi)
        pulsePhase -= juce::MathConstants<float>::twoPi;

    repaint();
}

void DynamicsDrModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    auto area = content.reduced(6);
    const int h = juce::jmax(42, area.getHeight() / 3 - 3);

    auto drToNorm = [](float d) { return juce::jlimit(0.0f, 1.0f, juce::jmap(d, 0.0f, 24.0f, 0.0f, 1.0f)); };
    drawReadout(g, area.removeFromTop(h), "DR SHORT", shortDR, "dB", drToNorm(shortDR), flashShort, pulsePhase);
    area.removeFromTop(3);
    drawReadout(g, area.removeFromTop(h), "DR INTEG", integDR, "dB", drToNorm(integDR), flashInteg, pulsePhase + 2.0f);
    area.removeFromTop(3);
    drawReadout(g, area, "CREST", crest, "dB", drToNorm(crest), flashCrest, pulsePhase + 4.0f);
}

DynamicsCrestModule::DynamicsCrestModule(AnalyserHub& h)
    : ModulePanel(ModuleType::dynamicsCrest), hub(h)
{
    // Phase F：订阅 Dynamics + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Dynamics);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // dynamics crest 6×4 大格
    setTitleText("Dynamics Crest History");

    // 初始化：默认 8 秒跨度，缓冲按 histMaxLen 点 × "每几帧采样一次"
    crestHist.assign((size_t) histMaxLen, 0.0f);

    // ---- 右侧颗粒度滑条（风格与 EqModule 的 SIZE 滑条保持一致）----
    //   · 垂直滑条，范围 [2, 120] 秒（常用动态范围场景够用）
    //   · 现在方案：滑条只控制显示窗口大小，不再改变"几帧采样一次"
    //     → 调大颜粒度时图像仍按每帧 1 像素流畅推进，不会卡顿
    spanSlider.setSliderStyle(juce::Slider::LinearVertical);
    spanSlider.setRange(1.0, 35.0, 1.0);
    spanSlider.setValue((double) spanSeconds, juce::dontSendNotification);
    // TextBox 只读：拿掉"点数字改值"的交互入口，数字只做展示
    spanSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 40, 16);
    // 数字文字颜色跟随主题 ink（浅色主题下是深色 → 可读）
    spanSlider.setColour (juce::Slider::textBoxTextColourId,       PinkXP::ink);
    spanSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    spanSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    spanSlider.setTextValueSuffix("s");
    spanSlider.onValueChange = [this]()
    {
        spanSeconds = juce::jlimit(1, 35, (int) spanSlider.getValue());
        // 无需再重算 framesPerSample：始终保持每帧写入；
        //   窗口变小时只是"显示更少的点"，数据不需清零，
        //   窗口变大时旧点仍可用（不足部分显示为 0）
        repaint();
    };

    spanLabel.setJustificationType(juce::Justification::centred);
    spanLabel.setColour(juce::Label::textColourId, PinkXP::ink);
    spanLabel.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    spanLabel.setText("SPAN", juce::dontSendNotification);

    addAndMakeVisible(spanSlider);
    addAndMakeVisible(spanLabel);

    // 主题切换时重新下发 "SPAN" 标签的 textColourId —— 与标题栏墨色保持一致。
    //   Label 的 textColour 是缓存值，主题切换后需要手动刷新。
    //   Slider 的 textBoxTextColourId 同样需要显式刷新（同样的缓存语义）。
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        spanLabel.setColour (juce::Label::textColourId, PinkXP::ink);
        spanSlider.setColour (juce::Slider::textBoxTextColourId, PinkXP::ink);
        spanLabel.repaint();
        spanSlider.repaint();
    });

    // 固定每帧写入：保持 framesPerSample = 1 / frameCounter = 0
    framesPerSample = 1;
    frameCounter    = 0;
}

DynamicsCrestModule::~DynamicsCrestModule()
{
    // 解绑主题订阅，避免析构后仍然回调到已销毁的 this
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Dynamics);
}

void DynamicsCrestModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Dynamics)) return;

    const auto& s = frame.dynamics;

    // 固定每帧都写入缓冲 → 图像始终按 30fps 的速率流畅流动
    //   后端分析量零新增（dynamics 原本就会每帧产出 frame.dynamics.crest）
    crestHist[(size_t) writePos] = juce::jlimit(0.0f, 40.0f, s.crest);
    writePos = (writePos + 1) % histMaxLen;

    repaint();
}

void DynamicsCrestModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto area = contentBounds.reduced(6);

    // 右侧 SPAN 滑条（样式与 EqModule 的 SIZE 滑条相同）
    const int sliderWidth = 42;
    auto rightPanel = area.removeFromRight(sliderWidth);
    area.removeFromRight(6);

    auto labelArea = rightPanel.removeFromTop(14);
    spanLabel.setBounds(labelArea);
    spanSlider.setBounds(rightPanel);
    // 注：area 是左侧绘图区，不再作为子组件 bounds，由 paintContent 自己计算
    //    我们通过成员不存，paintContent 每次重算
    juce::ignoreUnused(area);
}

void DynamicsCrestModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    // 与 layoutContent 保持同步：从内容区裁掉右侧滑条栏和间距
    auto area = content.reduced(6);
    const int sliderWidth = 42;
    area.removeFromRight(sliderWidth + 6);

    PinkXP::drawSunken(g, area, PinkXP::content);
    auto plot = area.reduced(6);
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8)
        return;

    // 横线网格：0/10/20/30 dB
    g.setColour(PinkXP::pink200.withAlpha(0.45f));
    for (int db = 0; db <= 30; db += 10)
    {
        const float t = 1.0f - (float) db / 30.0f;
        const int y = plot.getY() + (int) std::round(t * plot.getHeight());
        g.drawHorizontalLine(y, (float) plot.getX(), (float) plot.getRight());
    }

    // 曲线：最旧在左、最新在右
    //   • 缓冲是 histMaxLen = 3600 点（足够装下 120s × 30fps）
    //   • span 只决定"取最新 N 点显示"： N = min(spanSeconds × 30, histMaxLen)
    //   • 最新点 = crestHist[(writePos - 1 + histMaxLen) % histMaxLen]
    //     最旧点 = crestHist[(writePos - N + histMaxLen) % histMaxLen]
    const int N = juce::jmin (spanSeconds * 30, histMaxLen);
    const int denom = juce::jmax (1, N - 1);
    juce::Path p;
    for (int i = 0; i < N; ++i)
    {
        const int idx = ((writePos - N + i) % histMaxLen + histMaxLen) % histMaxLen;
        const float v = crestHist[(size_t) idx];
        const float t = (float) i / (float) denom;
        const float x = (float) plot.getX() + t * (float) plot.getWidth();
        const float y = (float) plot.getBottom() - juce::jlimit(0.0f, 1.0f, v / 30.0f) * (float) plot.getHeight();
        if (i == 0) p.startNewSubPath(x, y);
        else        p.lineTo(x, y);
    }

    juce::Path fill = p;
    fill.lineTo((float) plot.getRight(), (float) plot.getBottom());
    fill.lineTo((float) plot.getX(),     (float) plot.getBottom());
    fill.closeSubPath();

    g.setColour(PinkXP::pink300.withAlpha(0.28f));
    g.fillPath(fill);
    g.setColour(PinkXP::pink600);
    g.strokePath(p, juce::PathStrokeType(1.4f));

    // 左下角跨度提示
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    g.drawText(juce::String(spanSeconds) + "s  history",
               plot.getX() + 2, plot.getBottom() - 12, 120, 12,
               juce::Justification::centredLeft, false);
}

// ==========================================================
// VuMeterModule —— 指针式模拟 VU 表
//
// 视觉差异化：
//   · 采用**半圆扇形表盘 + 双指针**的经典模拟 VU 表造型，区别于项目里
//     其他基于方块柱 / 数字读数 / 示波器曲线的模块
//   · 圆心位于表盘底部中点，扇形跨度约 140° (左下 -> 正上 -> 右下)
//   · 左指针 (L) = pink400；右指针 (R) = pink700
//   · 刻度：-20 / -10 / -7 / -5 / -3 / 0 / +3 VU
//   · 右上角 LED 圆灯：dim / 绿 (有信号) / 红 (>= -3 dBFS 危险)
//
// 数据来源：AnalyserHub::Kind::Loudness → 复用已算好的 RMS L/R
//         → 无任何新增的音频线程分析计算量
// ==========================================================
VuMeterModule::VuMeterModule(AnalyserHub& h)
    : ModulePanel(ModuleType::vuMeter), hub(h)
{
    // 订阅 Oscilloscope —— 复用 Waveform 模块用的同一份 2048 样本原始波形，
    //   这是目前后端最"快"的电平数据源（每 33ms 帧到达时里面就有最新样本）。
    //   不订阅 Loudness：它的 rmsL/R 只在 400ms 块边界刷新一次，滞后严重。
    //   → 后端音频线程零新增计算。
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener(this);

    setMinSize(96, 80);
    setDefaultSize(320, 224);
    setTitleText("VU Meter");

    // 启动 60Hz 自有 Timer 做补帧插值。按真实 dt 把 displayed 连续推进到 target，
    // 视觉上是丝滑扫动。
    lastTickMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(60);
}

VuMeterModule::~VuMeterModule()
{
    stopTimer();
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Oscilloscope);
}

void VuMeterModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    // 从 oscL/R 尾部（即最新的一段样本）取 ~20ms 窗口算瞬时 RMS
    //   · 波形缓冲是 2048 样本、"时间从旧到新"，末尾就是最近的样本
    //   · 采样率通过 AudioProcessor 拿不到最方便的引用，这里用启发式估计：
    //     典型 44.1/48 kHz → 20ms ≈ 880/960 样本；我们取 960 作为上限，
    //     若实际 sr 偏低（比如 44.1k）窗口略超 20ms 也无碍。
    const int bufLen = (int) frame.oscL.size(); // = 2048
    if (bufLen <= 0) return;

    constexpr int windowSamples = 960; // ≈ 20ms @ 48kHz，≈ 21.8ms @ 44.1kHz
    const int n = juce::jmin(bufLen, windowSamples);
    const int startIdx = bufLen - n; // 从末尾向前 n 个样本

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

    // 线性 RMS → dBFS；低于门限（接近数字底噪）直接报 -144 让指针回到左端
    auto msToDb = [](double meanSq) -> float
    {
        if (meanSq <= 1.0e-10) return -144.0f;
        return (float)(10.0 * std::log10(meanSq));
    };
    targetL = msToDb(meanSqL);
    targetR = msToDb(meanSqR);
}

void VuMeterModule::timerCallback()
{
    // 按真实 dt（毫秒）推进 —— 不假设 60Hz 准确
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    double dtMs = nowMs - lastTickMs;
    lastTickMs  = nowMs;
    if (dtMs < 0.0)   dtMs = 16.0;
    if (dtMs > 100.0) dtMs = 100.0;
    const float dt = (float) dtMs;

    // ======================================================================
    // 防御性 pull 兜底：
    //
    //   主路径依赖 AnalyserHub::FrameDispatcher 的 fanout 调用 VuMeterModule::
    //   onFrame —— 但 Hub 的 fanout 里有个 `if (!comp->isShowing() ...) skip` 的
    //   活跃检查（见 AnalyserHub.cpp 里对 FrameListener 做 dynamic_cast<Component*>
    //   后调 isShowing 的那段）。在以下场景会触发跳过：
    //
    //     · 插件宿主挂起 / 隐藏 Editor 窗口瞬间 isShowing()=false；
    //     · 切换布局预设 / 新建模块 的一两帧里组件尺寸为 0；
    //     · workspace 子组件 hierarchy 在重排中间态 isShowing() 暂短为 false；
    //     · OpenGLContext attachTo 初始化的首帧 Component 仍未 showing；
    //
    //   以上任何一种瞬间出现，VU 的 onFrame 都拿不到新的 frame 数据，targetL/R
    //   保持 -144，而 60Hz Timer 的 smoothOne 让 displayedL/R 也收敛到 -144，
    //   指针就"贴死在 -20"（minDisplayDb）并很难恢复。
    //
    //   修复：每个 60Hz tick 主动 hub.getLatestFrame() 拉一次最新帧，自己重算
    //   targetL/R。这一路和 onFrame push 路径完全独立，即便 fanout 出于任何
    //   原因跳过本模块，Pull 路径仍能把数据灌进来。
    //
    //   成本：每帧一次 SpinLock + shared_ptr 拷贝（Hub::getLatestFrame 内部已
    //   做原子发布），在 60Hz 下开销可忽略。
    // ======================================================================
    if (auto frame = hub.getLatestFrame())
    {
        if (frame->has (AnalyserHub::Kind::Oscilloscope))
        {
            const int bufLen = (int) frame->oscL.size(); // 2048
            if (bufLen > 0)
            {
                constexpr int windowSamples = 960; // ≈ 20ms @ 48kHz
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

                auto msToDb = [](double ms) -> float
                {
                    if (ms <= 1.0e-10) return -144.0f;
                    return (float) (10.0 * std::log10 (ms));
                };
                targetL = msToDb (meanSqL);
                targetR = msToDb (meanSqR);
            }
        }
    }

    // 非对称弹道：上升 τ=tauRiseMs（抓瞬态），下降 τ=tauFallMs（模拟 VU 回落）。
    //   · 连续瞬态情形：每个瞬态都通过上升项立刻把指针抬到新高，之间的回落
    //     按 fall 时间常数执行 —— 指针不会被锁死在顶端，会"踩着节拍"往上
    //     走，节拍停的瞬间也会继续按 fall 缓慢下行。
    //   · 静音（target -> -144）时：由下降项主导，指针平缓归位。
    const float aUp   = 1.0f - std::exp(-dt / tauRiseMs);
    const float aDown = 1.0f - std::exp(-dt / tauFallMs);

    auto smoothOne = [&](float& disp, float tgt)
    {
        if (! std::isfinite(disp)) disp = -144.0f;
        if (! std::isfinite(tgt))  tgt  = -144.0f;
        const float a = (tgt > disp) ? aUp : aDown;
        disp += a * (tgt - disp);
    };
    smoothOne(displayedL, targetL);
    smoothOne(displayedR, targetR);

    // LED 跟随（基于 displayed 的单声道合成），非对称：上升立即跟、下降快速衰减
    const float monoNow = currentMonoDbfs();
    if (monoNow > ledLevelDb)
    {
        ledLevelDb = monoNow;
    }
    else
    {
        constexpr float tauLedMs = 150.0f; // 略快于指针回落
        const float aLed = 1.0f - std::exp(-dt / tauLedMs);
        ledLevelDb += aLed * (monoNow - ledLevelDb);
    }

    // 脉动相位（~0.9 秒一圈）
    constexpr float omegaRadPerMs = juce::MathConstants<float>::twoPi / 900.0f;
    pulsePhase += omegaRadPerMs * dt;
    if (pulsePhase > juce::MathConstants<float>::twoPi)
        pulsePhase -= juce::MathConstants<float>::twoPi;

    repaint();
}

float VuMeterModule::currentMonoDbfs() const noexcept
{
    // 把 displayed L/R dBFS 转线性功率、平均、再转 dBFS
    //   等价于 10*log10((10^(L/10) + 10^(R/10)) / 2)
    auto dbToPower = [](float db) -> float
    {
        if (db <= -143.0f || ! std::isfinite(db)) return 0.0f;
        return std::pow(10.0f, db * 0.1f);
    };
    const float pL = dbToPower(displayedL);
    const float pR = dbToPower(displayedR);
    const float pAvg = (pL + pR) * 0.5f;
    return (pAvg > 1.0e-14f) ? 10.0f * std::log10(pAvg) : -144.0f;
}

float VuMeterModule::vuToAngle(float valueDb) const noexcept
{
    // 指针扫过 0°~180° 的上半圆（水平左 → 水平右）
    const float leftRad  = -juce::MathConstants<float>::halfPi;   // -90°
    const float rightRad = +juce::MathConstants<float>::halfPi;   // +90°

    // dBFS 线性映射到 [0,1] —— 表盘上的 dB 刻度是等距线性的，符合
    //   "-60 / -40 / -20 / -6 / 0" 这些整数 dB 在表盘上的常规感知。
    const float t = juce::jlimit(0.0f, 1.0f,
                                 juce::jmap(valueDb, minDisplayDb, maxDisplayDb, 0.0f, 1.0f));
    return juce::jmap(t, 0.0f, 1.0f, leftRad, rightRad);
}

void VuMeterModule::drawDial(juce::Graphics& g,
                             juce::Rectangle<int> dialArea,
                             juce::Point<float>&  outPivot,
                             float&               outRadius) const
{
    // 注意：sunken 卡片背景（pink50）已由 paintContent 在外层统一绘制，
    //   这里不再重复 drawSunken，避免出现双层凹陷边框。
    //   dialArea 即 sunken 卡片的整体区域，我们只需从中留出一点边距
    //   供圆弧/刻度/文字使用即可。
    auto inner = dialArea.reduced(10);
    if (inner.getWidth() < 20 || inner.getHeight() < 20) return;

    // 圆心 = 内框底边中点（半圆 0°-180° 以底边水平为直径）
    //   · 指针从水平左(-90°) 扫到水平右(+90°)，所以圆心必须在底边上
    //   · cy 稍微上移 2px，给底部的圆心轴 + 描边留一点边距
    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getBottom() - 2.0f;

    // 半径约束：
    //   · 横向 —— 半圆直径 = 2R，所以 R <= width/2
    //   · 纵向 —— 半圆高度 = R，所以 R <= height
    //   再各留 4px 呼吸空间防止贴到 drawSunken 内沿
    const float R = juce::jmin((float) inner.getWidth()  * 0.5f - 4.0f,
                                (float) inner.getHeight()        - 4.0f);
    if (R < 10.0f) return;

    outPivot  = { cx, cy };
    outRadius = R;

    // 表盘深色前景统一用主题 ink（在浅粉底上有良好对比度）
    const juce::Colour inkCol = PinkXP::ink;

    // ---- 弧形主刻度轨（0°~180° 上半圆） ----
    juce::Path arc;
    const float leftRad  = -juce::MathConstants<float>::halfPi;
    const float rightRad = +juce::MathConstants<float>::halfPi;
    // JUCE addCentredArc 的角度 0 = 12 点方向、顺时针为正
    arc.addCentredArc(cx, cy, R, R, 0.0f, leftRad, rightRad, true);
    g.setColour(inkCol.withAlpha(0.85f));
    g.strokePath(arc, juce::PathStrokeType(1.8f));

    // ---- 内弧（红色警戒段：warnDbfs 到 0 dBFS） ----
    juce::Path arcRed;
    const float redStart = vuToAngle(warnDbfs);
    arcRed.addCentredArc(cx, cy, R, R, 0.0f, redStart, rightRad, true);
    g.setColour(juce::Colour(0xffd21f3a).withAlpha(0.9f));
    g.strokePath(arcRed, juce::PathStrokeType(3.0f));

    // ---- 刻度与数字（dBFS，-25 .. +3 均匀等距） ----
    //   线性映射（vuToAngle 用 jmap），所以每个整数 dB 在表盘上间隔均等。
    //   策略：-25 .. +3 共 28 dB
    //     · 每 1 dB 画一个微刻度（短刻度）
    //     · 每 5 dB + 关键整数位（-3/0/+3）画长刻度并带数字标签
    struct Tick { float db; const char* label; bool major; };
    static const Tick ticks[] = {
        { -25.0f, "-25", true  },
        { -24.0f, nullptr,false},
        { -23.0f, nullptr,false},
        { -22.0f, nullptr,false},
        { -21.0f, nullptr,false},
        { -20.0f, "-20", true  },
        { -19.0f, nullptr,false},
        { -18.0f, nullptr,false},
        { -17.0f, nullptr,false},
        { -16.0f, nullptr,false},
        { -15.0f, "-15", true  },
        { -14.0f, nullptr,false},
        { -13.0f, nullptr,false},
        { -12.0f, nullptr,false},
        { -11.0f, nullptr,false},
        { -10.0f, "-10", true  },
        {  -9.0f, nullptr,false},
        {  -8.0f, nullptr,false},
        {  -7.0f, nullptr,false},
        {  -6.0f, nullptr,false},
        {  -5.0f,  "-5", true  },
        {  -4.0f, nullptr,false},
        {  -3.0f,  "-3", true  },
        {  -2.0f, nullptr,false},
        {  -1.0f, nullptr,false},
        {   0.0f,   "0", true  },
        {  +1.0f, nullptr,false},
        {  +2.0f, nullptr,false},
        {  +3.0f,  "+3", true  },
    };

    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::bold));
    for (const auto& t : ticks)
    {
        const float ang = vuToAngle(t.db);
        const float sinA = std::sin(ang);
        const float cosA = std::cos(ang);
        // 刻度线从半径 R 向内延伸 (长刻度 7px，短刻度 4px)
        const float tickLen = t.major ? 7.0f : 4.0f;
        const float x1 = cx + sinA * R;
        const float y1 = cy - cosA * R;
        const float x2 = cx + sinA * (R - tickLen);
        const float y2 = cy - cosA * (R - tickLen);

        // 红色警戒段：达到 warnDbfs 开始变红
        const bool red = t.db >= warnDbfs;
        g.setColour(red ? juce::Colour(0xffb61325) : inkCol);
        g.drawLine(x1, y1, x2, y2, t.major ? 1.8f : 1.2f);

        // 数字标签 (贴在刻度外侧内圈 R-14)
        if (t.label != nullptr)
        {
            const float tx = cx + sinA * (R - 16.0f);
            const float ty = cy - cosA * (R - 16.0f);
            juce::Rectangle<float> labRect (tx - 12.0f, ty - 6.0f, 24.0f, 12.0f);
            g.setColour(red ? juce::Colour(0xffb61325) : inkCol);
            g.drawText(t.label, labRect, juce::Justification::centred, false);
        }
    }

    // ---- 表盘面文字 "dBFS" ----
    {
        g.setColour(inkCol.withAlpha(0.75f));
        g.setFont(PinkXP::getFont(12.0f, juce::Font::bold));
        // 圆心在底边之上一点，"dBFS" 放在圆心偏上
        const float ly = cy - R * 0.42f;
        g.drawText("dBFS", juce::Rectangle<float>(cx - 30.0f, ly - 8.0f, 60.0f, 16.0f),
                   juce::Justification::centred, false);
    }

    // ---- 圆心轴 (主题色小凸起 + ink 描边) ----
    g.setColour(PinkXP::pink300);
    g.fillEllipse(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    g.setColour(inkCol);
    g.drawEllipse(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, 1.2f);
}

void VuMeterModule::drawNeedle(juce::Graphics& g,
                               juce::Point<float> pivot,
                               float              radius,
                               float              vuDb,
                               juce::Colour       colour,
                               float              thickness) const
{
    const float ang = vuToAngle(vuDb);
    const float sinA = std::sin(ang);
    const float cosA = std::cos(ang);

    // 指针尖端：稍短于表盘弧线 (R - 12)，避免盖住刻度数字
    const float tipR = radius - 10.0f;
    // 指针根部：圆心后方伸出一点 (配重效果)
    const float tailR = 8.0f;

    const float x1 = pivot.x - sinA * tailR;
    const float y1 = pivot.y + cosA * tailR;
    const float x2 = pivot.x + sinA * tipR;
    const float y2 = pivot.y - cosA * tipR;

    // 阴影 (在指针下方偏移 1.5 px 画一根半透明深色)
    g.setColour(juce::Colour(0xff2a1a08).withAlpha(0.35f));
    g.drawLine(x1 + 1.0f, y1 + 1.5f, x2 + 1.0f, y2 + 1.5f, thickness);

    // 指针本体
    g.setColour(colour);
    g.drawLine(x1, y1, x2, y2, thickness);

    // 指针尖端小圆点
    g.setColour(colour.darker(0.2f));
    g.fillEllipse(x2 - 1.5f, y2 - 1.5f, 3.0f, 3.0f);
}

void VuMeterModule::drawLed(juce::Graphics& g, juce::Rectangle<int> ledArea) const
{
    if (ledArea.getWidth() < 6 || ledArea.getHeight() < 6) return;

    const int sz = juce::jmin(ledArea.getWidth(), ledArea.getHeight());
    auto square = juce::Rectangle<int>(ledArea.getRight() - sz,
                                        ledArea.getY(),
                                        sz, sz).reduced(2);

    const float cx = (float) square.getCentreX();
    const float cy = (float) square.getCentreY();
    const float r  = (float) square.getWidth() * 0.5f - 1.0f;

    // 状态判定（使用 onFrame 里维护的实时 ledLevelDb — peak-follower，
    //   基于 currentMonoDbfs，和指针视觉同步）
    //   · ledLevelDb < -60 dBFS → 视为无信号（暗灭，不发光）
    //   · ledLevelDb >= warnDbfs → 危险（红灯 + 快速脉动），和表盘红色段一致
    //   · 否则                    → 绿灯（慢速呼吸）
    const bool hasSignal = ledLevelDb > minDisplayDb;
    const bool danger    = ledLevelDb >= warnDbfs;

    juce::Colour coreCol, glowCol;
    float pulse = 0.5f;

    if (! hasSignal)
    {
        // 无信号：不亮 —— 用一个非常暗的灰色，且无晕光
        coreCol = juce::Colour(0xff2a2a2a);
        glowCol = juce::Colour(0x00000000);
    }
    else if (danger)
    {
        // 危险：红色 + 快速脉动 (每帧 2× 加速)
        const float phaseFast = pulsePhase * 2.0f;
        pulse = 0.55f + 0.45f * std::sin(phaseFast);
        coreCol = juce::Colour(0xffff2040).interpolatedWith(juce::Colour(0xffffc0c8), 0.15f * pulse);
        glowCol = juce::Colour(0xffff2040).withAlpha(0.45f + 0.45f * pulse);
    }
    else
    {
        // 正常：绿色 + 慢速呼吸
        pulse = 0.6f + 0.4f * std::sin(pulsePhase);
        coreCol = juce::Colour(0xff30d86a).interpolatedWith(juce::Colour(0xffc8ffd4), 0.15f * pulse);
        glowCol = juce::Colour(0xff30d86a).withAlpha(0.35f + 0.45f * pulse);
    }

    // 不使用外围泛光 —— 仅画 LED 本体 + 高光 + 描边
    juce::ignoreUnused(glowCol);

    // LED 本体
    g.setColour(coreCol);
    g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);

    // 高光（仅在有信号时显示，凸显球面立体感）
    if (hasSignal)
    {
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.fillEllipse(cx - r * 0.6f, cy - r * 0.75f,
                      r * 0.6f,      r * 0.5f);
    }

    // 外圈金属边 (统一用主题 ink)
    g.setColour(PinkXP::ink);
    g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 1.2f);
}

void VuMeterModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    // 外层面板背景：统一用主题 btnFace，保证和其它模块留白风格一致
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    // 参照 PhaseModule / PhaseCorrelation：只留很小的边距，直接把整块内容
    //   下沉成一个 sunken 卡片（底色为当前主题浅色 pink50），不再在模块边框
    //   上独占一条 LED band。这样模块外边框跟 Phase 模块一致的紧凑效果。
    auto area = content.reduced(6);
    if (area.getWidth() < 40 || area.getHeight() < 40) return;

    PinkXP::drawSunken(g, area, PinkXP::pink50);

    // 表盘区域 = sunken 卡片内部（再减去一点内描边余量，和 drawDial 保持一致）
    auto dialArea = area;

    // 画表盘 (获取圆心 + 半径)
    juce::Point<float> pivot;
    float radius = 0.0f;
    drawDial(g, dialArea, pivot, radius);

    if (radius > 10.0f)
    {
        // 指针和 LED 共用的"单声道总电平"（dBFS）
        const float monoDb = currentMonoDbfs();

        // 单一指针（总电平），主题粉色系 pink600，稍粗一点
        drawNeedle(g, pivot, radius, monoDb, PinkXP::pink600, 2.2f);
    }

    // ---- LED 信号灯：放到表盘旁边（sunken 卡片内的右上角空白） ----
    //   VU 表盘是 0°~180° 的上半圆，所以卡片的左上角和右上角天然是空白，
    //   把 LED 塞在右上角可避免和指针/刻度重叠，也不再占用模块边框。
    const int ledSz = juce::jlimit(10, 18,
                                    juce::jmin(area.getWidth(), area.getHeight()) / 12);
    // 相对 sunken 内沿再缩 6px，给 drawSunken 的阴影/内描边一点呼吸
    auto ledArea = juce::Rectangle<int>(area.getRight() - ledSz - 6,
                                         area.getY()      + 6,
                                         ledSz, ledSz);
    drawLed(g, ledArea);
}
