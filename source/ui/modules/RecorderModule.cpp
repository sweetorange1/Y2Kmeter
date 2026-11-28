#include "source/ui/modules/RecorderModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"

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

    durationSlider.setTooltip (y2k::help::make ("Clip length",
        "How many seconds SAVE LAST grabs from the rolling 60 s buffer (1 - 60 s). "
        "Double-click for 10 s."));

    addAndMakeVisible (durationSlider);

    // ---- 导出按钮 ----
    saveButton.setButtonText ("SAVE LAST 10 s");
    saveButton.setTooltip (y2k::help::make ("Save last N seconds",
        "Missed a great moment? Y2Kmeter always keeps the last 60 s of input in memory. "
        "Click to write the last N seconds to a WAV file (32-bit float, before gain)."));
    saveButton.onClick = [this] { saveNow(); };
    addAndMakeVisible (saveButton);

    startTimerHz (kTimerHz);
    syncChildChromeVisibility();
    y2k::ui::setGlSafeImageCache (*this, ! PinkXP::isTransparentBackdrop());

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        syncChildChromeVisibility();
        y2k::ui::setGlSafeImageCache (*this, ! PinkXP::isTransparentBackdrop());
        repaint();
    });

    chromaRevealToken = PinkXP::subscribeChromaRevealChanged ([this] (juce::Component* prev, juce::Component* next)
    {
        if (prev == this || next == this || next == nullptr)
        {
            syncChildChromeVisibility();
            repaint();
        }
    });
}

RecorderModule::~RecorderModule()
{
    if (chromaRevealToken >= 0)
    {
        PinkXP::unsubscribeChromaRevealChanged (chromaRevealToken);
        chromaRevealToken = -1;
    }

    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    stopTimer();
}

void RecorderModule::syncChildChromeVisibility()
{
    const bool hideChildren = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);
    const float alpha = hideChildren ? 0.0f : 1.0f;
    if (std::abs (durationSlider.getAlpha() - alpha) > 0.001f)
        durationSlider.setAlpha (alpha);
    if (std::abs (saveButton.getAlpha() - alpha) > 0.001f)
        saveButton.setAlpha (alpha);
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

juce::Rectangle<int> RecorderModule::getChromaKeyCanvas()
{
    const auto content = getContentBounds();
    const auto r = computeRects (content);
    if (r.header.isEmpty() || (r.time.isEmpty() && r.dot.isEmpty()))
        return {};
    const int bottomY = (r.time.getHeight() >= 12) ? r.time.getBottom() : r.dot.getBottom();
    return content.withTop (r.header.getY()).withBottom (bottomY);
}

// ==========================================================
// 绘制
// ==========================================================
void RecorderModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto& tokens      = PinkXP::getSkinTokens();
    const bool chromaKey    = tokens.isChromaKey;
    const bool hideControls = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);

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
            PinkXP::Craft::drawPanelWell (g, contentBounds.reduced (3), PinkXP::content, PinkXP::WellKind::card);
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
            PinkXP::Craft::drawPanelWell (g, contentBounds.reduced (3), PinkXP::content, PinkXP::WellKind::card);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            g.setColour (PinkXP::face.interpolatedWith (juce::Colours::black, isDark ? 0.16f : 0.07f));
            g.fillRect (contentBounds);
            PinkXP::Craft::drawPanelWell (g, contentBounds.reduced (3), PinkXP::content, PinkXP::WellKind::card);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
        {
            g.setColour (PinkXP::face);
            g.fillRect (contentBounds);
            PinkXP::Craft::drawPanelWell (g, contentBounds.reduced (3), PinkXP::content, PinkXP::WellKind::card);
        }
        else
        {
            PinkXP::Craft::drawPanelWell (g, contentBounds, PinkXP::content, PinkXP::WellKind::canvas);
        }
    }

    const auto r = computeRects (contentBounds);
    if (r.header.getWidth() <= 0 || r.header.getHeight() <= 0)
        return;

    const bool taking  = recorder.isTaking();
    const auto status  = recorder.getStatusText();

    // 字号全部跟随各自行高：模块被压缩时文字一起变小，而不只是被裁掉
    const float headerFont = juce::jlimit (5.5f, 9.0f,  (float) r.header.getHeight() * 0.64f);
    const float timeFont   = juce::jlimit (6.0f, 11.0f, (float) r.time.getHeight()   * 0.58f);
    const float footerFont = juce::jlimit (5.0f, 8.0f,  (float) r.footer.getHeight() * 0.66f);
    const float valueFont  = juce::jlimit (6.0f, 11.0f, (float) r.value.getHeight()  * 0.62f);

    // ---- 顶部状态行 ----
    auto header = r.header;

    g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (headerFont, juce::Font::bold),
                                        (float) juce::jmax (6, header.getHeight())));
    g.setColour (PinkXP::ink.withAlpha (0.75f));
    g.drawText (y2k::tr ("RECORDER"), header, juce::Justification::centredLeft, false);

    // 状态灯：录制中 → 亮红 + 500 ms 闪烁；未录制 → 暗灰
    juce::String stateLabel  = "IDLE";
    juce::Colour stateColour = PinkXP::ink.withAlpha (0.4f);
    bool         ledLit      = false;

    if (taking)
    {
        const bool blinkOn = ((juce::Time::getMillisecondCounter() / 500) % 2) == 0;
        stateLabel  = "REC";
        stateColour = blinkOn ? kRecRed : kRecRed.withAlpha (0.3f);
        ledLit      = blinkOn;
    }

    if (status.startsWithIgnoreCase ("ERROR"))
    {
        stateLabel  = "ERROR";
        stateColour = PinkXP::danger;
        ledLit      = true;
    }
    else if (status == "NO AUDIO CAPTURED")
    {
        stateLabel  = "EMPTY";
        stateColour = PinkXP::danger;
        ledLit      = true;
    }
    else if (! taking && recorder.didHitTakeLimit())
    {
        // 到达 1 小时上限：采集线程已自动收尾保存，状态灯给出明确反馈
        stateLabel  = "SAVED 1H";
        stateColour = PinkXP::ok;
        ledLit      = true;
    }

    const int stateWanted = juce::jmax (PinkXP::ui (30),
                                        (int) (headerFont * 1.3f * (float) stateLabel.length()));
    auto stateArea = header.removeFromRight (juce::jmin (header.getWidth() * 2 / 3,
                                                         juce::jmax (PinkXP::ui (24), stateWanted)));
    g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (headerFont, juce::Font::bold),
                                        (float) juce::jmax (6, stateArea.getHeight())));
    g.setColour (stateColour);
    g.drawText (y2k::tr (stateLabel), stateArea, juce::Justification::centredRight, true);

    // 状态灯：Pixel XP 保留标志性 2×2 像素方块；其余风格走 Craft::drawStatusLed
    if (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel)
    {
        const int px = juce::jmax (2, PinkXP::ui (2));
        const int cx = stateArea.getX() - PinkXP::ui (7);
        const int cy = stateArea.getCentreY() - px / 2;
        g.setColour (stateColour);
        g.fillRect (cx, cy, px, px);
    }
    else
    {
        const int sz = juce::jlimit (5, 7, r.header.getHeight() - 4);
        const int cx = stateArea.getX() - PinkXP::ui (8);
        const int cy = stateArea.getCentreY();
        const juce::Rectangle<int> ledRect (cx - sz / 2, cy - sz / 2, sz, sz);
        PinkXP::Craft::drawStatusLed (g, ledRect, ledLit ? stateColour : PinkXP::ink.withAlpha (0.5f),
                                      ledLit, true);
    }

    // 顶部状态行分割线（Chrome 金属沟槽 / Glass 柔透丝 / Neon 霓虹激光 / CRT 磷光线）
    if (! chromaKey && contentBounds.getHeight() >= 120)
    {
        const bool isDark = PinkXP::content.getPerceivedBrightness() < 0.45f;
        const int ruleY = r.header.getBottom() + 1;
        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            g.setColour (PinkXP::dark.withAlpha (isDark ? 0.50f : 0.30f));
            g.fillRect (r.header.getX(), ruleY, r.header.getWidth(), 1);
            g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f)
                                   .withAlpha (isDark ? 0.22f : 0.62f));
            g.fillRect (r.header.getX(), ruleY + 1, r.header.getWidth(), 1);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            g.setColour (isDark ? juce::Colours::white.withAlpha (0.14f)
                                : juce::Colours::black.withAlpha (0.09f));
            g.fillRect (r.header.getX(), ruleY, r.header.getWidth(), 1);
            g.setColour (juce::Colours::white.withAlpha (isDark ? 0.05f : 0.45f));
            g.fillRect (r.header.getX(), ruleY + 1, r.header.getWidth(), 1);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            if (! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (PinkXP::sel.withAlpha (0.18f));
                g.fillRect (r.header.getX(), ruleY - 1, r.header.getWidth(), 3);
            }
            g.setColour (PinkXP::sel.withAlpha (0.58f));
            g.fillRect (r.header.getX(), ruleY, r.header.getWidth(), 1);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            g.setColour (PinkXP::sel.withAlpha (0.45f));
            g.fillRect (r.header.getX(), ruleY, r.header.getWidth(), 1);
        }
    }

    // ---- 录音圆点（Pixel XP 11×11 像素盘 / Chrome CNC / Glass 宝石 / Neon 霓虹环 / CRT 磷光靶盘 / Modern）----
    paintPixelDot (g, r.dot, dotPressed, taking);

    // ---- 本次录制时长 + 进度凹槽 ----
    if (r.time.getHeight() >= 12)
    {
        PinkXP::Craft::drawPanelWell (g, r.time, PinkXP::content.darker (0.04f), PinkXP::WellKind::trough);

        auto timeInner = r.time.reduced (4, 1);
        if (taking && timeInner.getHeight() >= 10)
        {
            const float prog = juce::jlimit (0.0f, 1.0f,
                (float) (recorder.getTakeSeconds() / AudioRecorder::maxTakeSeconds));
            auto progBar = timeInner.removeFromBottom (2);
            if (prog > 0.001f)
                PinkXP::Craft::drawContinuousBar (g, progBar.toFloat(),
                                                  progBar.toFloat().withWidth ((float) progBar.getWidth() * prog),
                                                  kRecRed, false, false);
        }

        g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (timeFont, juce::Font::bold),
                                            (float) juce::jmax (6, timeInner.getHeight())));
        g.setColour (taking ? kRecRed : PinkXP::ink.withAlpha (0.4f));
        g.drawText (y2k::tr (formatElapsed (taking ? recorder.getTakeSeconds() : 0.0)),
                    timeInner, juce::Justification::centred, false);

        // 录制中在计时右侧提示单段上限（到点自动保存并停止）
        if (taking)
        {
            g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (juce::jlimit (5.0f, 8.0f,
                                                                               (float) timeInner.getHeight() * 0.45f)),
                                                (float) juce::jmax (5, timeInner.getHeight())));
            g.setColour (PinkXP::ink.withAlpha (0.35f));
            g.drawText (y2k::tr ("MAX " + formatElapsed (AudioRecorder::maxTakeSeconds)),
                        timeInner, juce::Justification::centredRight, false);
        }
    }

    // ---- 拖动条数值 + 底部状态（透明未悬停态自动隐藏，与绿幕裁切口径一致）----
    if (! hideControls)
    {
        g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (valueFont, juce::Font::bold),
                                            (float) juce::jmax (6, r.value.getHeight())));
        g.setColour (PinkXP::ink.withAlpha (0.8f));
        g.drawText (y2k::tr (juce::String ((int) selectedSeconds) + " s"),
                    r.value, juce::Justification::centredRight, false);

        juce::String fileLabel;
        if (const auto saved = recorder.getLastSavedFile(); saved != juce::File())
            fileLabel = y2k::trf ("SAVED: {0}", saved.getFileName());
        else
            fileLabel = "AUTO-CAPTURE: INPUT SIGNAL";

        g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (footerFont),
                                            (float) juce::jmax (5, r.footer.getHeight())));
        g.setColour (PinkXP::ink.withAlpha (0.5f));
        g.drawText (y2k::tr (fileLabel), r.footer, juce::Justification::centred, true);
    }

    // ---- 像素角标（仅 Pixel XP 等开启 pixelCornerAccents 的风格）----
    if (tokens.pixelCornerAccents && ! chromaKey
        && contentBounds.getWidth() > PinkXP::ui (60)
        && contentBounds.getHeight() > PinkXP::ui (60))
    {
        PinkXP::drawPixelCorners (g, contentBounds.reduced (2), PinkXP::pink300, PinkXP::ui (5), 1);
    }

    PinkXP::Craft::drawCanvasFinishOverlay (g, contentBounds.reduced (2));
}

void RecorderModule::paintPixelDot (juce::Graphics& g, juce::Rectangle<int> area,
                                    bool pressed, bool taking) const
{
    const int cell = computeDotCell (area);
    if (cell <= 0)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const juce::Rectangle<int> square (cell * kDotCells, cell * kDotCells);
    const auto origin = square.withCentre (area.getCentre()).getPosition();

    // ---- 三种状态（按压优先于录制）----
    //   未录制   ：发光 + 正常亮度 + 不下沉
    //   按钮按下 ：不发光 + 变暗 + 下沉一格
    //   录制中   ：轻微发光 + 轻微变暗 + 下沉半格
    const int   sink = pressed ? cell : (taking ? juce::jmax (1, cell / 2) : 0);
    const float glow = pressed ? 0.0f : (taking ? 0.45f : 1.0f);

    juce::Colour body = kRecRed;
    if (pressed)     body = body.darker (0.45f);
    else if (taking) body = body.darker (0.18f);

    const bool greenScreen = PinkXP::isGreenScreenBackdrop();

    // CRT Terminal：Tektronix 磷光双环靶盘 REC 指示灯（平滑矢量磷光圆 + 十字准星刻度 + 微扫描线圆盘）
    if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
    {
        const float dia = (float) (cell * kDotCells) * 0.84f;
        const juce::Rectangle<float> baseCirc ((float) area.getCentreX() - dia * 0.5f,
                                               (float) area.getCentreY() - dia * 0.5f,
                                               dia, dia);
        const float ringGap   = juce::jlimit (2.5f, 5.0f, dia * 0.11f);
        const auto outerRing  = baseCirc.expanded (ringGap + 2.2f);
        const auto innerRing  = baseCirc.expanded (ringGap * 0.45f);
        const float cx        = baseCirc.getCentreX();
        const float cy        = baseCirc.getCentreY();

        if (! greenScreen)
        {
            g.setColour (PinkXP::sel.withAlpha (0.20f));
            g.drawEllipse (outerRing, 2.8f);
            if (glow > 0.01f)
            {
                g.setColour (kRecRed.withAlpha ((taking ? 0.24f : 0.14f) * glow));
                g.fillEllipse (baseCirc.expanded (dia * 0.14f));
            }
        }

        // 双线磷光目标环
        g.setColour (PinkXP::sel.withAlpha (0.85f));
        g.drawEllipse (outerRing, 1.15f);
        g.setColour (PinkXP::sel.withAlpha (0.48f));
        g.drawEllipse (innerRing, 0.85f);

        // 四方向十字准星刻度 + 四角瞄准框
        const float tickLen = juce::jlimit (3.0f, 6.0f, dia * 0.12f);
        const float rOut    = outerRing.getWidth() * 0.5f;
        g.setColour (PinkXP::sel.withAlpha (0.78f));
        g.drawLine (cx, cy - rOut - tickLen, cx, cy - rOut + 1.0f, 1.0f);
        g.drawLine (cx, cy + rOut - 1.0f,    cx, cy + rOut + tickLen, 1.0f);
        g.drawLine (cx - rOut - tickLen, cy, cx - rOut + 1.0f, cy, 1.0f);
        g.drawLine (cx + rOut - 1.0f,    cy, cx + rOut + tickLen, cy, 1.0f);

        if (! greenScreen)
        {
            const auto br = outerRing.expanded (2.0f).toNearestInt();
            PinkXP::drawPixelCorners (g, br, PinkXP::sel.withAlpha (0.65f), juce::jlimit (4, 7, (int) (dia * 0.14f)), 1);
        }

        // 磷光发光 REC 核心圆盘 + 细微扫描线纹理
        const auto discCirc = baseCirc.translated (0.0f, (float) sink * 0.55f);
        {
            juce::ColourGradient phGrad (body.interpolatedWith (juce::Colours::white, pressed ? 0.18f : 0.42f),
                                         discCirc.getCentreX(), discCirc.getCentreY(),
                                         body.darker (0.28f),
                                         discCirc.getRight(), discCirc.getBottom(), true);
            g.setGradientFill (phGrad);
            g.fillEllipse (discCirc);
        }
        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addEllipse (discCirc);
            g.reduceClipRegion (clip);
            g.setColour (juce::Colours::black.withAlpha (0.16f));
            for (int py = (int) discCirc.getY() + 1; py < (int) discCirc.getBottom(); py += 2)
                g.fillRect ((int) discCirc.getX(), py, (int) discCirc.getWidth() + 1, 1);
        }
        g.setColour (body.interpolatedWith (juce::Colours::white, 0.48f));
        g.drawEllipse (discCirc.reduced (0.5f), 1.0f);
        return;
    }

    // 非像素风格（Chrome / Glass / Neon / Modern）：绿幕与透明模式均完整继承
    if (! tokens.pixelSnapped)
    {
        if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            // Frutiger Aero 宝石红玻璃 REC 实体晶珠按键：磨砂晶环 + 凸面红宝石球 + 穹顶高光 + 底部折光反弹
            const float dia    = (float) (cell * kDotCells) * 0.86f;
            const float bezelW = juce::jlimit (2.6f, 5.5f, dia * 0.115f);
            const juce::Rectangle<float> baseCirc ((float) area.getCentreX() - dia * 0.5f,
                                                   (float) area.getCentreY() - dia * 0.5f,
                                                   dia, dia);
            const auto bezelOuter = baseCirc.expanded (bezelW);
            const bool isDark     = PinkXP::content.getPerceivedBrightness() < 0.45f;

            if (! greenScreen)
            {
                g.setColour (juce::Colours::black.withAlpha (isDark ? 0.30f : 0.15f));
                g.fillEllipse (bezelOuter.translated (0.0f, 1.8f));

                if (glow > 0.01f)
                {
                    g.setColour (kRecRed.withAlpha ((taking ? 0.18f : 0.10f) * glow));
                    g.fillEllipse (bezelOuter.expanded (dia * 0.14f));
                }

                // 磨砂玻璃表环（上下渐变折射 + 顶沿纯白镜面高光）
                juce::ColourGradient ringGrad (juce::Colours::white.withAlpha (isDark ? 0.26f : 0.72f),
                                               bezelOuter.getCentreX(), bezelOuter.getY(),
                                               PinkXP::face.interpolatedWith (juce::Colours::white, isDark ? 0.10f : 0.25f)
                                                           .withAlpha (isDark ? 0.35f : 0.55f),
                                               bezelOuter.getCentreX(), bezelOuter.getBottom(), false);
                g.setGradientFill (ringGrad);
                g.fillEllipse (bezelOuter);
                g.setColour (PinkXP::dark.withAlpha (0.30f));
                g.drawEllipse (bezelOuter.reduced (0.5f), 1.0f);
                g.setColour (juce::Colours::white.withAlpha (isDark ? 0.42f : 0.85f));
                g.drawEllipse (bezelOuter.reduced (1.4f), 0.9f);
            }
            else
            {
                g.setColour (PinkXP::hl.interpolatedWith (PinkXP::face, 0.40f));
                g.drawEllipse (bezelOuter.reduced (bezelW * 0.5f), bezelW * 0.85f);
            }

            // 凸面通透宝石红晶珠主体 + 底部折光反弹光（Aero Caustic Bounce）
            const auto discCirc = baseCirc.translated (0.0f, (float) sink * 0.65f);
            {
                juce::ColourGradient jewelGrad (body.darker (0.28f),
                                                discCirc.getCentreX(), discCirc.getY(),
                                                body.interpolatedWith (juce::Colours::white, pressed ? 0.14f : 0.42f),
                                                discCirc.getCentreX(), discCirc.getBottom(), false);
                jewelGrad.addColour (0.48, body);
                jewelGrad.addColour (0.82, body.brighter (0.22f));
                g.setGradientFill (jewelGrad);
                g.fillEllipse (discCirc);
            }

            // 顶部 42% 晶莹穹顶玻璃高光 + 清晰镜面月牙弧
            if (! pressed && dia >= 14.0f)
            {
                const float shineH = juce::jmax (3.5f, dia * 0.40f);
                const auto shine = discCirc.reduced (dia * 0.14f, dia * 0.08f).withHeight (shineH);
                juce::ColourGradient domeSpec (juce::Colours::white.withAlpha (0.64f),
                                               shine.getCentreX(), shine.getY(),
                                               juce::Colours::white.withAlpha (0.04f),
                                               shine.getCentreX(), shine.getBottom(), false);
                g.setGradientFill (domeSpec);
                g.fillEllipse (shine);

                juce::Path specArc;
                const float arcR = dia * 0.37f;
                specArc.addCentredArc (discCirc.getCentreX(), discCirc.getCentreY(),
                                       arcR, arcR, 0.0f, -0.68f, 0.68f, true);
                g.setColour (juce::Colours::white.withAlpha (0.58f));
                g.strokePath (specArc, juce::PathStrokeType (juce::jmax (1.1f, dia * 0.042f),
                                                             juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
            }

            g.setColour (juce::Colours::white.withAlpha (0.34f));
            g.drawEllipse (discCirc.reduced (1.1f), 0.85f);
            g.setColour (body.darker (0.45f).withAlpha (0.65f));
            g.drawEllipse (discCirc.reduced (0.4f), 0.95f);
            return;
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            // Cyber Neon 霓虹双环 REC 按键：四角 HUD 瞄准支架 + 暗烟熏基座 + 双层发光霓虹外环 + 高亮白芯红核
            const float dia    = (float) (cell * kDotCells) * 0.84f;
            const float bezelW = juce::jlimit (2.6f, 5.2f, dia * 0.115f);
            const juce::Rectangle<float> baseCirc ((float) area.getCentreX() - dia * 0.5f,
                                                   (float) area.getCentreY() - dia * 0.5f,
                                                   dia, dia);
            const auto bezelOuter = baseCirc.expanded (bezelW);

            if (! greenScreen)
            {
                // 四角赛博 HUD 支架
                const auto hudBox = bezelOuter.expanded (3.0f).toNearestInt();
                PinkXP::drawPixelCorners (g, hudBox, PinkXP::sel.withAlpha (0.72f),
                                          juce::jlimit (4, 8, (int) (dia * 0.16f)), 1);

                // 外层彩色霓虹辉光
                g.setColour (PinkXP::sel.withAlpha (0.18f));
                g.drawEllipse (bezelOuter, 3.8f);
                g.setColour (kRecRed.withAlpha ((taking ? 0.34f : 0.22f) * juce::jmax (0.35f, glow)));
                g.drawEllipse (bezelOuter.reduced (0.5f), 4.6f);
            }

            // 暗色烟熏圆形基座
            g.setColour (PinkXP::dark.darker (0.55f));
            g.fillEllipse (bezelOuter);

            // 双层发光霓虹外环（主灯管 + 白芯）
            g.setColour (body);
            g.drawEllipse (bezelOuter.reduced (0.8f), 1.65f);
            g.setColour (body.interpolatedWith (juce::Colours::white, 0.58f));
            g.drawEllipse (bezelOuter.reduced (0.8f), 0.8f);

            // 中心高亮霓虹红核
            const auto coreCirc = baseCirc.reduced (dia * 0.13f).translated (0.0f, (float) sink * 0.55f);
            if (! greenScreen && glow > 0.01f)
            {
                g.setColour (kRecRed.withAlpha (0.28f * glow));
                g.fillEllipse (coreCirc.expanded (dia * 0.10f));
            }
            {
                juce::ColourGradient coreGrad (body.interpolatedWith (juce::Colours::white, pressed ? 0.22f : 0.65f),
                                               coreCirc.getCentreX(), coreCirc.getCentreY(),
                                               body.darker (0.32f),
                                               coreCirc.getRight(), coreCirc.getBottom(), true);
                coreGrad.addColour (0.48, body);
                g.setGradientFill (coreGrad);
                g.fillEllipse (coreCirc);
            }
            g.setColour (body.interpolatedWith (juce::Colours::white, 0.55f));
            g.drawEllipse (coreCirc.reduced (0.5f), 0.95f);
            return;
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            // CNC 机械拉丝金属 REC 实体按键：固定拉丝铝外圈 + 凹槽内壁 + 可下沉凸面宝石红按键帽
            const float dia    = (float) (cell * kDotCells) * 0.86f;
            const float bezelW = juce::jlimit (2.5f, 5.5f, dia * 0.115f);
            const juce::Rectangle<float> baseCirc ((float) area.getCentreX() - dia * 0.5f,
                                                   (float) area.getCentreY() - dia * 0.5f,
                                                   dia, dia);
            const auto bezelOuter = baseCirc.expanded (bezelW);
            const bool isDark     = PinkXP::content.getPerceivedBrightness() < 0.45f;

            // 1. 外部投影与录制微光（绿幕跳过半透明外晕避免绿边）
            if (! greenScreen)
            {
                g.setColour (juce::Colours::black.withAlpha (isDark ? 0.36f : 0.22f));
                g.fillEllipse (bezelOuter.translated (0.0f, 1.5f));

                if (glow > 0.01f && taking)
                {
                    g.setColour (kRecRed.withAlpha (0.14f * glow));
                    g.fillEllipse (bezelOuter.expanded (dia * 0.10f));
                }
            }

            // 2. 固定拉丝铝 CNC 环形表圈（对角四段金属拉丝渐变 + 2px 镜面倒角）
            {
                const auto hiChrome  = PinkXP::hl.interpolatedWith (juce::Colours::white, isDark ? 0.55f : 0.82f);
                const auto midChrome = PinkXP::face.interpolatedWith (PinkXP::shdw, isDark ? 0.18f : 0.26f);
                const auto loChrome  = PinkXP::shdw.interpolatedWith (PinkXP::dark, isDark ? 0.52f : 0.38f);
                juce::ColourGradient bezelGrad (hiChrome, bezelOuter.getX(), bezelOuter.getY(),
                                                loChrome, bezelOuter.getRight(), bezelOuter.getBottom(), false);
                bezelGrad.addColour (0.38, midChrome.interpolatedWith (juce::Colours::white, isDark ? 0.16f : 0.34f));
                bezelGrad.addColour (0.62, midChrome.interpolatedWith (juce::Colours::black, isDark ? 0.14f : 0.08f));
                bezelGrad.addColour (0.92, PinkXP::hl.interpolatedWith (juce::Colours::white, 0.28f));
                g.setGradientFill (bezelGrad);
                g.fillEllipse (bezelOuter);

                // 外沿 1px 暗切边 + 内 1px 镜面倒角高光环
                g.setColour (PinkXP::dark.withAlpha (0.80f));
                g.drawEllipse (bezelOuter.reduced (0.5f), 1.05f);

                juce::ColourGradient bevelRim (juce::Colours::white.withAlpha (isDark ? 0.52f : 0.85f),
                                               bezelOuter.getX(), bezelOuter.getY(),
                                               PinkXP::dark.withAlpha (isDark ? 0.58f : 0.36f),
                                               bezelOuter.getRight(), bezelOuter.getBottom(), false);
                g.setGradientFill (bevelRim);
                g.drawEllipse (bezelOuter.reduced (1.5f), 1.0f);
            }

            // 3. 铣削内陷灯座（位于不透明表圈内部，绿幕同样零绿边）
            const auto socket = baseCirc.expanded (1.2f);
            g.setColour (PinkXP::dark.darker (0.40f));
            g.fillEllipse (socket);
            g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.45f).withAlpha (0.28f));
            g.drawEllipse (socket.reduced (0.4f), 0.75f);

            // 4. 凸面宝石红 REC 按钮主体（按压/录制时在固定表圈内下沉）
            const float pressShift = (float) sink * 0.60f;
            const auto discCirc = baseCirc.translated (0.0f, pressShift);
            {
                juce::ColourGradient rubyGrad (body.interpolatedWith (juce::Colours::white, pressed ? 0.12f : 0.34f),
                                               discCirc.getX() + dia * 0.36f, discCirc.getY() + dia * 0.28f,
                                               body.darker (0.44f),
                                               discCirc.getRight(), discCirc.getBottom(), true);
                rubyGrad.addColour (0.52, body);
                g.setGradientFill (rubyGrad);
                g.fillEllipse (discCirc);
            }

            // 5. 按钮内部精致车削细沟槽（双环暗槽 + 微亮沿）
            if (dia >= 18.0f)
            {
                const auto grooveR = discCirc.reduced (dia * 0.15f);
                g.setColour (juce::Colours::black.withAlpha (0.26f));
                g.drawEllipse (grooveR, 0.85f);
                g.setColour (juce::Colours::white.withAlpha (0.18f));
                g.drawEllipse (grooveR.reduced (0.8f), 0.7f);
            }

            // 6. 上沿镜面月牙高光弧 + 柔和表镜穹顶反光
            if (! pressed && dia >= 14.0f)
            {
                const float shineH = juce::jmax (3.0f, dia * 0.32f);
                const auto shine = discCirc.reduced (dia * 0.18f, dia * 0.10f).withHeight (shineH);
                juce::ColourGradient domeSpec (juce::Colours::white.withAlpha (0.26f),
                                               shine.getCentreX(), shine.getY(),
                                               juce::Colours::white.withAlpha (0.0f),
                                               shine.getCentreX(), shine.getBottom(), false);
                g.setGradientFill (domeSpec);
                g.fillEllipse (shine);

                juce::Path specArc;
                const float arcR = dia * 0.36f;
                specArc.addCentredArc (discCirc.getCentreX(), discCirc.getCentreY(),
                                       arcR, arcR, 0.0f, -0.72f, 0.72f, true);
                g.setColour (juce::Colours::white.withAlpha (0.52f));
                g.strokePath (specArc, juce::PathStrokeType (juce::jmax (1.1f, dia * 0.042f),
                                                             juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
            }

            g.setColour (body.darker (0.58f));
            g.drawEllipse (discCirc.reduced (0.4f), 1.0f);
            return;
        }

        const float dia = (float) (cell * kDotCells) * 0.90f;
        const juce::Rectangle<float> circ ((float) area.getCentreX() - dia * 0.5f,
                                           (float) area.getCentreY() - dia * 0.5f + (float) sink,
                                           dia, dia);

        // 外圈演播室刻度环（绿幕跳过避免绿边；正常与透明模式完整呈现）
        if (! greenScreen)
        {
            const float ringPad = juce::jmax (3.0f, dia * 0.08f);
            const auto outerRing = circ.expanded (ringPad);
            if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
            {
                g.setColour (taking ? kRecRed.withAlpha (0.42f) : PinkXP::skinHairlineOn (PinkXP::content, 0.24f));
                g.drawEllipse (outerRing, 1.25f);
            }
            else if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
            {
                g.setColour (PinkXP::dark.withAlpha (0.55f));
                g.drawEllipse (outerRing, 1.4f);
                g.setColour (PinkXP::hl.withAlpha (0.55f));
                g.drawEllipse (outerRing.reduced (1.2f), 1.0f);
            }

            if (glow > 0.01f)
            {
                g.setColour (kRecRed.withAlpha ((taking ? 0.14f : 0.08f) * glow));
                g.fillEllipse (circ.expanded (dia * 0.14f));
                if (tokens.frameStyle == PinkXP::FrameStyle::neonTube
                    || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                {
                    g.setColour (kRecRed.withAlpha (0.07f * glow));
                    g.fillEllipse (circ.expanded (dia * 0.26f));
                }
            }

            // 微阴影（未按下时悬浮微抬升）
            if (! pressed && tokens.frameStyle != PinkXP::FrameStyle::neonTube)
            {
                g.setColour (juce::Colours::black.withAlpha (0.18f));
                g.fillEllipse (circ.translated (0.0f, 1.5f));
            }
        }

        // 按钮主体（精致纵渐变 + 顶沿细高光 + 清晰描边）
        {
            juce::ColourGradient discGrad (body.brighter (pressed ? 0.02f : 0.14f),
                                           circ.getCentreX(), circ.getY(),
                                           body.darker (0.12f),
                                           circ.getCentreX(), circ.getBottom(), false);
            g.setGradientFill (discGrad);
            g.fillEllipse (circ);
        }

        if (! pressed && dia >= 14.0f)
        {
            const float shineH = juce::jmax (3.0f, dia * 0.34f);
            const auto shine = circ.reduced (dia * 0.18f, dia * 0.10f).withHeight (shineH);
            juce::ColourGradient spec (juce::Colours::white.withAlpha (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass ? 0.42f : 0.24f),
                                       shine.getCentreX(), shine.getY(),
                                       juce::Colours::white.withAlpha (0.0f),
                                       shine.getCentreX(), shine.getBottom(), false);
            g.setGradientFill (spec);
            g.fillEllipse (shine);
        }

        g.setColour (PinkXP::dark.withAlpha (tokens.frameStyle == PinkXP::FrameStyle::modernStudio ? 0.24f : 0.48f));
        g.drawEllipse (circ.reduced (0.5f), 1.1f);
        return;
    }

    // Pixel XP（及 CRT）：保留标志性 11×11 像素圆盘网格（绿幕/透明模式均完整继承）
    const juce::Colour edge = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor) ? PinkXP::sel : PinkXP::dark;
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

    // 像素阶梯光晕：绿幕下跳过避免半透明红绿混合边，正常与透明模式下正常呈现
    if (glow > 0.01f && ! greenScreen)
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

ModulePanel::HoverHelp RecorderModule::getHoverHelp (juce::Point<int> localPos) const
{
    const auto hit = getDotHitBounds();
    if (! hit.isEmpty() && hit.contains (localPos))
    {
        HoverHelp h;
        h.text = recorder.isTaking()
            ? y2k::help::make ("Stop recording",
                               "Click to stop and save this take as a WAV file. "
                               "The folder opens automatically.")
            : y2k::help::make ("Record",
                               "Click to start recording the raw input (before gain) to a "
                               "32-bit float WAV. Click again to stop and save. Up to 1 hour per take.");
        return h;
    }

    return ModulePanel::getHoverHelp (localPos);
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
