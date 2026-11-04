#include "source/ui/modules/RecorderModule.h"
#include "source/ui/PinkXPStyle.h"

#include <cmath>

namespace
{
constexpr int kTimerHz = 10;

const juce::Colour kRecRed { 0xffe8403c };

constexpr int kDotCells = 11;        // 红点圆盘网格
constexpr float kDotRadius = 5.2f;   // 外圈描边半径（格）
constexpr float kDotRim = 4.05f;     // 内部填充半径（格）
constexpr int kDotSpan = 14;         // 含光晕的总直径（格）：11 + 左右各 1.5 格光晕

// HH:MM:SS
juce::String formatElapsed (double seconds)
{
    const auto total = (juce::int64) juce::jmax (0.0, seconds);
    const int h = (int) (total / 3600);
    const int m = (int) ((total / 60) % 60);
    const int s = (int) (total % 60);

    return juce::String::formatted ("%02d:%02d:%02d", h, m, s);
}
} // namespace

// ==========================================================
// 构造 / 析构
// ==========================================================
RecorderModule::RecorderModule (AnalyserHub& hub)
    : ModulePanel (ModuleType::recorder),
      recorder (hub.getRecorder())
{
    setMinSize (160, 170);
    setDefaultSize (300, 228);

    // ---- 拖动条：1–60 s ----
    //   不用 Slider 自带的数值框：它的字体/配色由 LNF 固定（不随模块缩放、也不跟主题），
    //   这里改成 NoTextBox + 自己在右侧画「N s」，字号随行高缩放。
    durationSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    durationSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    durationSlider.setRange (1.0, AudioRecorder::maxHistorySeconds, 1.0);
    durationSlider.setValue (selectedSeconds, juce::dontSendNotification);
    durationSlider.setNumDecimalPlacesToDisplay (0);
    durationSlider.setDoubleClickReturnValue (true, 10.0);

    durationSlider.onValueChange = [this]
    {
        selectedSeconds = durationSlider.getValue();
        updateButtonText();
        repaint();          // 右侧「N s」由 paintContent 画，需要主动重绘
    };

    addAndMakeVisible (durationSlider);

    // ---- 导出按钮 ----
    saveButton.setButtonText ("SAVE LAST 10 s");
    saveButton.onClick = [this] { saveNow(); };
    addAndMakeVisible (saveButton);

    startTimerHz (kTimerHz);
}

RecorderModule::~RecorderModule()
{
    stopTimer();
}

// ==========================================================
// 布局
// ==========================================================
RecorderModule::Rects RecorderModule::computeRects (juce::Rectangle<int> contentBounds) const
{
    Rects r;

    auto area = contentBounds.reduced (PinkXP::ui (8));
    if (area.getWidth() <= 0 || area.getHeight() <= 0)
        return r;

    r.header = area.removeFromTop (PinkXP::ui (14));
    r.footer = area.removeFromBottom (PinkXP::ui (12));

    int buttonH = PinkXP::ui (26);
    int gapH    = PinkXP::ui (6);
    int sliderH = PinkXP::ui (20);
    int timeH   = PinkXP::ui (18);

    // 红点必须始终可见且有最小尺寸：空间不够时按比例压缩其它行，优先保红点区
    const int others  = buttonH + gapH + sliderH + timeH;
    const int minDotH = juce::jmin (area.getHeight(), juce::jmax (1, PinkXP::ui (48)));

    if (others > 0 && area.getHeight() < others + minDotH)
    {
        const float k = (float) juce::jmax (0, area.getHeight() - minDotH) / (float) others;
        buttonH = (int) (buttonH * k);
        gapH    = (int) (gapH * k);
        sliderH = (int) (sliderH * k);
        timeH   = (int) (timeH * k);
    }

    r.button = area.removeFromBottom (buttonH);
    area.removeFromBottom (gapH);
    r.slider = area.removeFromBottom (sliderH);
    r.time   = area.removeFromBottom (timeH);

    // 拖动条右侧留出「N s」显示区（自己画，字号随行高缩放）
    const int valueW = juce::jlimit (PinkXP::ui (22), PinkXP::ui (54),
                                     juce::jmax (0, r.slider.getWidth() / 4));
    r.value = r.slider.removeFromRight (valueW);
    r.slider.removeFromRight (PinkXP::ui (2));

    // 剩余空间给红点区，然后收紧到「红点 + 光晕」的实际高度：
    //   多出来的空白留在上方，红点贴住下面的计时器，而不是悬在中间。
    r.dot = area;

    const int cell = computeDotCell (r.dot);
    if (cell > 0)
    {
        const int need = cell * kDotSpan + cell;
        if (r.dot.getHeight() > need)
            r.dot = r.dot.withTop (r.dot.getBottom() - need);
    }

    return r;
}

int RecorderModule::computeDotCell (juce::Rectangle<int> area) const
{
    if (area.getWidth() <= 0 || area.getHeight() <= 0)
        return 0;

    // 以「红点 + 光晕」的总直径（14 格）去适配可用短边，红点随模块缩放；
    // 单元上限 ui(16)（红点 176 px / 光晕 224 px），避免超大模块里出现巨型圆饼；
    // 单元下限 2 格（红点 22 px），模块被压到很小时红点也不会缩成一个点。
    const int avail = juce::jmin (area.getWidth(), area.getHeight());
    const int cap   = juce::jmax (1, PinkXP::ui (16));
    const int floor = juce::jmin (2, cap);

    return juce::jlimit (floor, cap, avail / kDotSpan);
}

juce::Rectangle<int> RecorderModule::computeDotSquare (juce::Rectangle<int> area) const
{
    const int cell = computeDotCell (area);
    if (cell <= 0)
        return {};

    return juce::Rectangle<int> (cell * kDotCells, cell * kDotCells).withCentre (area.getCentre());
}

juce::Rectangle<int> RecorderModule::getDotHitBounds() const
{
    const auto r  = computeRects (getContentBounds());
    const auto sq = computeDotSquare (r.dot);
    if (sq.isEmpty())
        return {};

    const int cell = computeDotCell (r.dot);
    return sq.expanded (cell * 2);
}

void RecorderModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    const auto r = computeRects (contentBounds);

    durationSlider.setBounds (r.slider);
    saveButton.setBounds (r.button);
}

// ==========================================================
// 绘制
// ==========================================================
void RecorderModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    PinkXP::drawSunken (g, contentBounds, PinkXP::content);

    const auto r = computeRects (contentBounds);
    if (r.header.getWidth() <= 0 || r.header.getHeight() <= 0)
        return;

    const bool taking  = recorder.isTaking();
    const auto status  = recorder.getStatusText();

    // 字号全部跟随各自行高：模块被压缩时文字一起变小，而不只是被裁掉
    const float headerFont = juce::jlimit (5.5f, 9.0f,  (float) r.header.getHeight() * 0.64f);
    const float timeFont   = juce::jlimit (6.0f, 12.0f, (float) r.time.getHeight()   * 0.66f);
    const float footerFont = juce::jlimit (5.0f, 8.0f,  (float) r.footer.getHeight() * 0.66f);
    const float valueFont  = juce::jlimit (6.0f, 11.0f, (float) r.value.getHeight()  * 0.62f);

    // ---- 顶部状态行 ----
    auto header = r.header;

    g.setFont (PinkXP::getFont (headerFont, juce::Font::bold));
    g.setColour (PinkXP::ink.withAlpha (0.75f));
    g.drawText ("RECORDER", header, juce::Justification::centredLeft, false);

    // 状态灯：录制中 → 亮红 + 500 ms 闪烁；未录制 → 暗灰
    juce::String stateLabel  = "IDLE";
    juce::Colour stateColour = PinkXP::ink.withAlpha (0.4f);

    if (taking)
    {
        const bool blinkOn = ((juce::Time::getMillisecondCounter() / 500) % 2) == 0;
        stateLabel  = "REC";
        stateColour = blinkOn ? kRecRed : kRecRed.withAlpha (0.3f);
    }

    if (status.startsWithIgnoreCase ("ERROR"))
    {
        stateLabel  = "ERROR";
        stateColour = PinkXP::danger;
    }
    else if (status == "NO AUDIO CAPTURED")
    {
        stateLabel  = "EMPTY";
        stateColour = PinkXP::danger;
    }
    else if (! taking && recorder.didHitTakeLimit())
    {
        // 到达 1 小时上限：采集线程已自动收尾保存，状态灯给出明确反馈
        stateLabel  = "SAVED 1H";
        stateColour = PinkXP::ok;
    }

    const int stateWanted = juce::jmax (PinkXP::ui (30),
                                        (int) (headerFont * 1.3f * (float) stateLabel.length()));
    auto stateArea = header.removeFromRight (juce::jmin (header.getWidth() * 2 / 3,
                                                         juce::jmax (PinkXP::ui (24), stateWanted)));
    g.setFont (PinkXP::getFont (headerFont, juce::Font::bold));
    g.setColour (stateColour);
    g.drawText (stateLabel, stateArea, juce::Justification::centredRight, true);

    // 状态灯：2×2 像素方块（不是圆球，保持像素风）
    {
        const int px = juce::jmax (2, PinkXP::ui (2));
        const int cx = stateArea.getX() - PinkXP::ui (7);
        const int cy = stateArea.getCentreY() - px / 2;
        g.setColour (stateColour);
        g.fillRect (cx, cy, px, px);
    }

    // ---- 像素红点（点击 = 开始 / 停止录制）----
    paintPixelDot (g, r.dot, dotPressed, taking);

    // ---- 本次录制时长 ----
    g.setFont (PinkXP::getFont (timeFont, juce::Font::bold));
    g.setColour (taking ? kRecRed : PinkXP::ink.withAlpha (0.4f));
    g.drawText (formatElapsed (taking ? recorder.getTakeSeconds() : 0.0),
                r.time, juce::Justification::centred, false);

    // 录制中在计时右侧提示单段上限（到点自动保存并停止）
    if (taking)
    {
        g.setFont (PinkXP::getFont (juce::jlimit (5.0f, 8.0f,
                                                  (float) r.time.getHeight() * 0.45f)));
        g.setColour (PinkXP::ink.withAlpha (0.35f));
        g.drawText ("MAX " + formatElapsed (AudioRecorder::maxTakeSeconds),
                    r.time, juce::Justification::centredRight, false);
    }

    // ---- 拖动条数值（自己画，字号随行高缩放）----
    g.setFont (PinkXP::getFont (valueFont, juce::Font::bold));
    g.setColour (PinkXP::ink.withAlpha (0.8f));
    g.drawText (juce::String ((int) selectedSeconds) + " s",
                r.value, juce::Justification::centredRight, false);

    // ---- 底部：最近保存文件 ----
    {
        juce::String fileLabel;
        if (const auto saved = recorder.getLastSavedFile(); saved != juce::File())
            fileLabel = "SAVED: " + saved.getFileName();
        else
            fileLabel = "AUTO-CAPTURE: INPUT SIGNAL";

        g.setFont (PinkXP::getFont (footerFont));
        g.setColour (PinkXP::ink.withAlpha (0.5f));
        g.drawText (fileLabel, r.footer, juce::Justification::centred, true);
    }

    // ---- 像素装饰 ----
    if (contentBounds.getWidth() > PinkXP::ui (60) && contentBounds.getHeight() > PinkXP::ui (60))
        PinkXP::drawPixelCorners (g, contentBounds.reduced (2), PinkXP::pink300, PinkXP::ui (5), 1);
}

void RecorderModule::paintPixelDot (juce::Graphics& g, juce::Rectangle<int> area,
                                    bool pressed, bool taking) const
{
    const int cell = computeDotCell (area);
    if (cell <= 0)
        return;

    const juce::Rectangle<int> square (cell * kDotCells, cell * kDotCells);
    const auto origin = square.withCentre (area.getCentre()).getPosition();

    // ---- 三种状态（按压优先于录制）----
    //   未录制   ：发光（像素阶梯光晕）+ 正常亮度 + 不下沉
    //   按钮按下 ：不发光 + 变暗 + 下沉一格
    //   录制中   ：轻微发光 + 轻微变暗 + 下沉半格
    const int   sink = pressed ? cell : (taking ? juce::jmax (1, cell / 2) : 0);
    const float glow = pressed ? 0.0f : (taking ? 0.45f : 1.0f);

    juce::Colour body = kRecRed;
    if (pressed)     body = body.darker (0.45f);
    else if (taking) body = body.darker (0.18f);

    const juce::Colour edge = PinkXP::dark;
    const int centreCell = kDotCells / 2;

    // 以圆心为原点、按格填充一个圆盘（硬边，无抗锯齿）
    auto drawDisc = [&] (float radiusCells, juce::Colour colour)
    {
        g.setColour (colour);

        const int r = (int) std::ceil (radiusCells);

        for (int gy = -r; gy <= r; ++gy)
        {
            for (int gx = -r; gx <= r; ++gx)
            {
                if ((float) (gx * gx + gy * gy) > radiusCells * radiusCells)
                    continue;

                g.fillRect (origin.x + (centreCell + gx) * cell,
                            origin.y + (centreCell + gy) * cell + sink,
                            cell, cell);
            }
        }
    };

    // 像素阶梯光晕：三层同心圆盘，由外到内 alpha 递增（重叠累积，无模糊）
    //   刻意做得很弱：靠近圆点处累积约 11% alpha，只是给圆点一圈极淡的红边
    if (glow > 0.01f)
    {
        drawDisc (6.3f,  kRecRed.withAlpha (0.02f * glow));
        drawDisc (5.85f, kRecRed.withAlpha (0.035f * glow));
        drawDisc (5.55f, kRecRed.withAlpha (0.06f * glow));
    }

    // 主体：外圈深色描边 + 内部纯色填充
    drawDisc (kDotRadius, edge);
    drawDisc (kDotRim, body);
}

// ==========================================================
// 交互
// ==========================================================
void RecorderModule::mouseDown (const juce::MouseEvent& e)
{
    ModulePanel::mouseDown (e);   // toFront / 关闭按钮 / 标题栏拖拽 等

    if (e.mods.isPopupMenu())
        return;

    const auto hit = getDotHitBounds();
    if (! hit.isEmpty() && hit.contains (e.getPosition()))
    {
        dotPressed = true;
        toggleTake();   // 按下即开始 / 结束录制（松手只负责恢复视觉状态）
        repaint (hit.expanded (PinkXP::ui (4)));
    }
}

void RecorderModule::mouseUp (const juce::MouseEvent& e)
{
    ModulePanel::mouseUp (e);

    if (! dotPressed)
        return;

    // 录制状态已在 mouseDown 切换；松手只回到「录制中 / 未录制」的常态外观
    dotPressed = false;

    const auto hit = getDotHitBounds();
    if (! hit.isEmpty())
        repaint (hit.expanded (PinkXP::ui (4)));
}

void RecorderModule::mouseMove (const juce::MouseEvent& e)
{
    ModulePanel::mouseMove (e);

    const auto hit = getDotHitBounds();
    if (! hit.isEmpty() && hit.contains (e.getPosition()))
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void RecorderModule::mouseExit (const juce::MouseEvent& e)
{
    ModulePanel::mouseExit (e);   // 三种视觉状态完全由「未录制 / 按下 / 录制中」决定
}

// ==========================================================
// 导出
// ==========================================================
void RecorderModule::updateButtonText()
{
    saveButton.setButtonText ("SAVE LAST " + juce::String ((int) selectedSeconds) + " s");
}

void RecorderModule::saveNow()
{
    recorder.exportLastSeconds (selectedSeconds);

    lastStatus.clear();
    repaint();
}

void RecorderModule::toggleTake()
{
    if (recorder.isTaking())
        recorder.stopTake();
    else
        recorder.startTake();

    lastTakeSeconds = -1.0;
    lastStatus.clear();
    repaint();
}

// ==========================================================
// 定时刷新
// ==========================================================
void RecorderModule::timerCallback()
{
    if (! isVisuallyActiveInWorkspace())
        return;

    const double takeSecs = recorder.getTakeSeconds();
    const auto   status   = recorder.getStatusText();
    const bool   taking   = recorder.isTaking();

    if (std::abs (takeSecs - lastTakeSeconds) > 0.05
        || status != lastStatus
        || taking != lastTaking)
    {
        lastTakeSeconds = takeSecs;
        lastStatus      = status;
        lastTaking      = taking;
        repaint();
    }
}
