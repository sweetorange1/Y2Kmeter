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
