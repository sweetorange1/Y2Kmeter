#include "source/ui/modules/OscilloscopeModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// OscilloscopeModule —— Pink XP 像素风立体声示波器
// ==========================================================

OscilloscopeModule::OscilloscopeModule(AnalyserHub& h)
    : ModulePanel(ModuleType::oscilloscope),
      hub(h)
{
    // Phase F：本模块只需要 Oscilloscope 一路 + 注册 FrameListener
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener(this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // oscilloscope 6×4 大格

    auto setupModeBtn = [this](juce::TextButton& b)
    {
        b.setClickingTogglesState(true);
        b.setRadioGroupId(0x50424551); // "PBEQ"
        addAndMakeVisible(b);
    };
    setupModeBtn(btnWave);
    setupModeBtn(btnXY);
    setupModeBtn(btnLiss);

    btnWave.onClick = [this]() { setDisplayMode(DisplayMode::waveform);  };
    btnXY  .onClick = [this]() { setDisplayMode(DisplayMode::xy);        };
    btnLiss.onClick = [this]() { setDisplayMode(DisplayMode::lissajous); };

    btnFreeze.setClickingTogglesState(true);
    btnFreeze.onClick = [this]() { setFrozen(btnFreeze.getToggleState()); };
    addAndMakeVisible(btnFreeze);

    btnWave.setToggleState(true, juce::dontSendNotification);
}

OscilloscopeModule::~OscilloscopeModule()
{
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Oscilloscope);
}

void OscilloscopeModule::setDisplayMode(DisplayMode m)
{
    if (displayMode == m) return;
    displayMode = m;
    refreshModeButtons();
    repaint();
}

void OscilloscopeModule::setFrozen(bool b)
{
    if (frozen == b) return;
    frozen = b;
    btnFreeze.setToggleState(frozen, juce::dontSendNotification);
}

void OscilloscopeModule::refreshModeButtons()
{
    btnWave.setToggleState(displayMode == DisplayMode::waveform,  juce::dontSendNotification);
    btnXY  .setToggleState(displayMode == DisplayMode::xy,        juce::dontSendNotification);
    btnLiss.setToggleState(displayMode == DisplayMode::lissajous, juce::dontSendNotification);
}

// ----------------------------------------------------------
// onFrame：Hub 分发器回调（除非冻结）
// ----------------------------------------------------------
void OscilloscopeModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    if (! frozen)
    {
        // Phase F 优化：std::array → juce::Array 改 memcpy 批量拷贝
        const int n = (int) frame.oscL.size();
        snapshotL.resize(n);
        snapshotR.resize(n);
        std::memcpy (snapshotL.getRawDataPointer(), frame.oscL.data(), (size_t) n * sizeof (float));
        std::memcpy (snapshotR.getRawDataPointer(), frame.oscR.data(), (size_t) n * sizeof (float));
    }
    repaint();
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> OscilloscopeModule::getToolbarBounds(juce::Rectangle<int> content) const
{
    return content.withHeight(toolbarH);
}

juce::Rectangle<int> OscilloscopeModule::getCanvasBounds(juce::Rectangle<int> content) const
{
    return content.withTrimmedTop(toolbarH + 4);
}

void OscilloscopeModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto tb = getToolbarBounds(contentBounds).reduced(2);
    const int gap = 4;
    const int modeBtnW = 60;

    btnWave  .setBounds(tb.getX(),                        tb.getY(), modeBtnW, tb.getHeight());
    btnXY    .setBounds(tb.getX() + (modeBtnW + gap),     tb.getY(), modeBtnW, tb.getHeight());
    btnLiss  .setBounds(tb.getX() + (modeBtnW + gap) * 2, tb.getY(), modeBtnW, tb.getHeight());

    const int freezeW = 64;
    btnFreeze.setBounds(tb.getRight() - freezeW, tb.getY(), freezeW, tb.getHeight());
}

// ----------------------------------------------------------
// 绘制
// ----------------------------------------------------------
void OscilloscopeModule::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    // 内容区底色（深色主题下也保持浅色，保证按钮文字可读）
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);

    // 画布区域（凹陷）
    auto canvas = getCanvasBounds(contentBounds);
    drawBackground(g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    switch (displayMode)
    {
        case DisplayMode::waveform:  drawWaveform(g, canvas);        break;
        case DisplayMode::xy:        drawXY(g, canvas, false);       break;
        case DisplayMode::lissajous: drawXY(g, canvas, true);        break;
    }

    // 冻结标记（右上角小红点）
    if (frozen)
    {
        auto dot = juce::Rectangle<int>(canvas.getRight() - 12, canvas.getY() + 6, 8, 8);
        g.setColour(juce::Colour(0xffec4d85));
        g.fillEllipse(dot.toFloat());
        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawText("FROZEN", canvas.getRight() - 60, canvas.getY() + 4,
                   42, 12, juce::Justification::centredRight, false);
    }
}

// ----------------------------------------------------------
// 背景：凹陷画布 + 像素网格
// ----------------------------------------------------------
void OscilloscopeModule::drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken(g, canvas, PinkXP::content);

    auto inner = canvas.reduced(2);
    if (inner.isEmpty()) return;

    // 像素点阵（8px 间距淡粉点阵）
    g.setColour(PinkXP::pink200.withAlpha(0.35f));
    const int dotStep = 8;
    for (int y = inner.getY(); y < inner.getBottom(); y += dotStep)
        for (int x = inner.getX() + ((y / dotStep) % 2) * (dotStep / 2);
             x < inner.getRight(); x += dotStep)
            g.fillRect(x, y, 1, 1);

    // 中心十字线
    g.setColour(PinkXP::pink300.withAlpha(0.6f));
    const int cx = inner.getCentreX();
    const int cy = inner.getCentreY();
    g.drawHorizontalLine(cy, (float) inner.getX(), (float) inner.getRight());
    g.drawVerticalLine  (cx, (float) inner.getY(), (float) inner.getBottom());

    // 1/4 / 3/4 虚线（垂直）
    g.setColour(PinkXP::pink200.withAlpha(0.5f));
    const int q1x = inner.getX() + inner.getWidth() / 4;
    const int q3x = inner.getX() + inner.getWidth() * 3 / 4;
    for (int y = inner.getY(); y < inner.getBottom(); y += 4)
    {
        g.fillRect(q1x, y, 1, 2);
        g.fillRect(q3x, y, 1, 2);
    }
}

// ----------------------------------------------------------
// Waveform 模式：时域 L（粉色）+ R（深粉）叠加
// ----------------------------------------------------------
void OscilloscopeModule::drawWaveform(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const int N = snapshotL.size();
    if (N <= 1) return;

    auto inner = canvas.reduced(4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const float w  = (float) inner.getWidth();
    const float h  = (float) inner.getHeight();
    const float cy = (float) inner.getCentreY();
    const float halfH = h * 0.45f;

    auto buildPath = [&](const juce::Array<float>& samples) -> juce::Path
    {
        juce::Path path;
        const int len = samples.size();
        if (len <= 1) return path;

        const float xStep = w / (float)(len - 1);
        for (int i = 0; i < len; ++i)
        {
            const float raw = samples.getUnchecked(i);
            const float s   = std::isfinite(raw) ? juce::jlimit(-1.0f, 1.0f, raw) : 0.0f;
            const float x   = (float) inner.getX() + (float) i * xStep;
            const float y   = cy - s * halfH;
            if (i == 0) path.startNewSubPath(x, y);
            else        path.lineTo(x, y);
        }
        return path;
    };

    // L 声道：浅粉描边（先淡描 glow，再实线）
    auto pathL = buildPath(snapshotL);
    g.setColour(PinkXP::pink300.withAlpha(0.35f));
    g.strokePath(pathL, juce::PathStrokeType(3.0f));
    g.setColour(PinkXP::pink400);
    g.strokePath(pathL, juce::PathStrokeType(1.4f));

    // R 声道：深粉描边
    if (snapshotR.size() == N)
    {
        auto pathR = buildPath(snapshotR);
        g.setColour(PinkXP::pink500.withAlpha(0.35f));
        g.strokePath(pathR, juce::PathStrokeType(3.0f));
        g.setColour(PinkXP::pink600);
        g.strokePath(pathR, juce::PathStrokeType(1.4f));
    }

    // 左下角说明
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    g.drawText("L", canvas.getX() + 4, canvas.getBottom() - 14,
               24, 12, juce::Justification::centredLeft, false);
    g.setColour(PinkXP::pink400);
    g.fillRect(canvas.getX() + 14, canvas.getBottom() - 9, 8, 2);

    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.drawText("R", canvas.getX() + 32, canvas.getBottom() - 14,
               24, 12, juce::Justification::centredLeft, false);
    g.setColour(PinkXP::pink600);
    g.fillRect(canvas.getX() + 42, canvas.getBottom() - 9, 8, 2);
}

// ----------------------------------------------------------
// XY / Lissajous 模式：散点云
// ----------------------------------------------------------
void OscilloscopeModule::drawXY(juce::Graphics& g, juce::Rectangle<int> canvas, bool rotate45) const
{
    const int N = juce::jmin(snapshotL.size(), snapshotR.size());
    if (N <= 1) return;

    auto inner = canvas.reduced(6);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getCentreY();
    const float radius = (float) juce::jmin(inner.getWidth(), inner.getHeight()) * 0.45f;

    // 边界圆（轻）
    g.setColour(PinkXP::pink300.withAlpha(0.45f));
    g.drawEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.0f);

    // 点绘：每 2 样本取 1，节省绘制，颜色用轨迹衰减
    for (int i = 0; i < N; i += 2)
    {
        const float rawL = snapshotL.getUnchecked(i);
        const float rawR = snapshotR.getUnchecked(i);
        if (! std::isfinite(rawL) || ! std::isfinite(rawR))
            continue;

        float lx = juce::jlimit(-1.0f, 1.0f, rawL);
        float ry = juce::jlimit(-1.0f, 1.0f, rawR);

        float px, py;
        if (rotate45)
        {
            // Mid/Side = L+R / L-R ，旋转 45°
            const float mid  = (lx + ry) * 0.7071067f;
            const float side = (lx - ry) * 0.7071067f;
            px = cx + side * radius;
            py = cy - mid  * radius;
        }
        else
        {
            px = cx + lx * radius;
            py = cy - ry * radius;
        }

        // 越新的点（i 越大）越亮
        const float alpha = 0.25f + 0.6f * ((float) i / (float) juce::jmax(1, N - 1));
        g.setColour(PinkXP::pink500.withAlpha(alpha));
        g.fillRect(px - 0.5f, py - 0.5f, 2.0f, 2.0f);
    }

    // 左下角模式说明
    g.setColour(PinkXP::ink.withAlpha(0.85f));
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));
    const juce::String hint = rotate45 ? "M/S" : "X=L  Y=R";
    g.drawText(hint, canvas.getX() + 4, canvas.getBottom() - 14,
               80, 12, juce::Justification::centredLeft, false);
}
