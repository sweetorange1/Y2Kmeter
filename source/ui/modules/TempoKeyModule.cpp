#include "source/ui/modules/TempoKeyModule.h"

#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"

#include <cmath>

namespace
{
    int buttonRowHeight (juce::Rectangle<int> content) noexcept
    {
        return juce::jlimit (16, 24, content.getHeight() / 7);
    }

    float clampFont (float v, float lo, float hi) noexcept
    {
        return juce::jlimit (lo, hi, v);
    }

    // 求"能放进 maxWidth × maxHeight 的最大字号参数"。
    //   注意：PinkXP::getFont(h) 内部会 ×1.5（正文放大）再乘 uiScale，
    //   所以这里的 h 是字体参数而不是最终像素高度；测量和绘制用同一个
    //   font 对象，因此宽度判定是准确的。
    //   像素字体（Silkscreen）的宽度与字号近似线性，迭代 4 次足够收敛。
    float fitTextHeight (const juce::String& text,
                         float maxWidth,
                         float maxHeight,
                         float minHeight,
                         int styleFlags)
    {
        if (text.isEmpty() || maxWidth <= 4.0f)
            return minHeight;

        float h = juce::jmax (minHeight, maxHeight);

        for (int pass = 0; pass < 4; ++pass)
        {
            const juce::Font f = PinkXP::getFont (h, styleFlags);
            const float w = juce::GlyphArrangement::getStringWidth (f, y2k::tr (text));

            if (w <= maxWidth || h <= minHeight)
                break;

            h = juce::jmax (minHeight, h * maxWidth / w);
        }

        return h;
    }

    // 置信度 / 电平进度条：Pixel XP / CRT 为分段像素 LED 格，平滑风格为连续进度条
    void drawTempoProgressBar (juce::Graphics& g, juce::Rectangle<int> bar, float frac, juce::Colour col)
    {
        if (bar.getWidth() < 4 || bar.getHeight() < 2)
            return;

        const auto& tokens = PinkXP::getSkinTokens();
        const float norm   = juce::jlimit (0.0f, 1.0f, frac);

        if (tokens.hardPixelSteps)
        {
            const bool isCrt = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
            if (isCrt && ! tokens.isChromaKey && bar.getHeight() >= 5)
            {
                g.setColour (PinkXP::sel.withAlpha (0.38f));
                g.drawRect (bar, 1);
                bar = bar.reduced (1);
                if (bar.getWidth() < 2 || bar.getHeight() < 1)
                    return;
            }
            const int cellW = juce::jlimit (3, 5, bar.getHeight());
            const int gap   = 1;
            const int step  = cellW + gap;
            const int total = juce::jmax (1, (bar.getWidth() + gap) / step);
            const int litN  = juce::jlimit (0, total, (int) std::lround ((double) total * (double) norm));
            for (int i = 0; i < total; ++i)
            {
                const int x = bar.getX() + i * step;
                const int w = juce::jmin (cellW, bar.getRight() - x);
                if (w <= 0) break;
                PinkXP::Craft::drawMeterCell (g, juce::Rectangle<int> (x, bar.getY(), w, bar.getHeight()),
                                              col, i < litN);
            }
            return;
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            const bool isDark = PinkXP::content.getPerceivedBrightness() < 0.45f;
            const auto bf     = bar.toFloat();
            const float rad   = juce::jmin (3.0f, bf.getHeight() * 0.5f);
            if (! tokens.isChromaKey)
            {
                g.setColour (isDark ? juce::Colours::white.withAlpha (0.10f)
                                    : juce::Colours::black.withAlpha (0.09f));
                if (rad > 0.5f) g.fillRoundedRectangle (bf, rad);
                else            g.fillRect (bar);
                g.setColour (juce::Colours::white.withAlpha (isDark ? 0.16f : 0.52f));
                if (rad > 0.5f) g.drawRoundedRectangle (bf.reduced (0.5f), juce::jmax (0.5f, rad - 0.5f), 0.8f);
                else            g.drawRect (bar, 1);
            }
            const float fillW = bf.getWidth() * norm;
            if (fillW > 0.25f)
                PinkXP::Craft::drawContinuousBar (g, bf, bf.withWidth (fillW), col, false, false);
            return;
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            if (! tokens.isChromaKey)
            {
                g.setColour (juce::Colours::black.withAlpha (0.42f));
                g.fillRect (bar);
                g.setColour (PinkXP::sel.withAlpha (0.34f));
                g.drawRect (bar, 1);
            }
            const auto innerBar = (! tokens.isChromaKey && bar.getHeight() >= 4) ? bar.reduced (1) : bar;
            const float fillW = (float) innerBar.getWidth() * norm;
            if (fillW > 0.25f)
                PinkXP::Craft::drawContinuousBar (g, innerBar.toFloat(), innerBar.toFloat().withWidth (fillW),
                                                  col, false, false);
            return;
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            const bool isDark = PinkXP::content.getPerceivedBrightness() < 0.45f;
            const bool useWell = (bar.getHeight() >= 6);
            if (! tokens.isChromaKey)
            {
                if (useWell)
                {
                    PinkXP::Craft::drawPanelWell (g, bar, PinkXP::content.darker (0.06f), PinkXP::WellKind::trough);
                }
                else
                {
                    g.setColour (PinkXP::dark.withAlpha (isDark ? 0.50f : 0.24f));
                    g.fillRect (bar);
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f)
                                           .withAlpha (isDark ? 0.22f : 0.58f));
                    g.fillRect (bar.getX(), bar.getBottom() - 1, bar.getWidth(), 1);
                }
            }

            const auto innerBar = (useWell && ! tokens.isChromaKey) ? bar.reduced (1) : bar;
            const float fillW = (float) innerBar.getWidth() * norm;
            if (fillW > 0.25f)
            {
                const auto fillR = innerBar.toFloat().withWidth (fillW);
                PinkXP::Craft::drawContinuousBar (g, innerBar.toFloat(), fillR, col, false, false);
                if (fillR.getHeight() >= 2.0f)
                {
                    g.setColour (col.interpolatedWith (juce::Colours::white, 0.52f).withAlpha (0.65f));
                    g.fillRect (fillR.getX(), fillR.getY(), fillR.getWidth(), 1.0f);
                }
            }
            return;
        }

        const bool drawTrough = (bar.getHeight() >= 7);
        if (! drawTrough && ! tokens.isChromaKey)
        {
            const auto trackCol = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                                   || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                                      ? PinkXP::skinHairlineOn (PinkXP::content, 0.14f)
                                      : PinkXP::ink.withAlpha (0.14f);
            g.setColour (trackCol);
            const float rad = juce::jmin (tokens.barCellRadius, (float) bar.getHeight() * 0.5f);
            if (rad > 0.5f) g.fillRoundedRectangle (bar.toFloat(), rad);
            else            g.fillRect (bar);
        }

        const float fillW = (float) bar.getWidth() * norm;
        if (fillW > 0.25f)
        {
            auto fillR = bar.toFloat().withWidth (fillW);
            if (drawTrough)
                fillR = fillR.reduced (1.0f);
            PinkXP::Craft::drawContinuousBar (g, bar.toFloat(), fillR, col, false, drawTrough);
        }
        else if (drawTrough)
        {
            PinkXP::Craft::drawPanelWell (g, bar, PinkXP::content.darker (0.04f), PinkXP::WellKind::trough);
        }
    }
}

TempoKeyModule::TempoKeyModule (AnalyserHub& h)
    : ModulePanel (ModuleType::tempoKey),
      hub (h)
{
    hub.retain (AnalyserHub::Kind::MusicAnalysis);
    hub.addFrameListener (this);

    setMinSize (160, 96);
    setDefaultSize (352, 200);
    setTitleText ("Tempo / Key");
    setControlPanelEnabled (true);

    btnStartStop.setTooltip (y2k::help::make ("Start / Stop",
        "Start or pause the tempo + key analysis. It runs in a background thread, "
        "so it never affects the audio."));
    btnStartStop.onClick = [this]() { onStartStopClicked(); };
    addAndMakeVisible (btnStartStop);

    // 若分析尚未启动，RESET 会顺手按下 START（见 onResetClicked）。
    btnReset.setTooltip (y2k::help::make ("Reset",
        "Forget what has been heard so far and start a fresh BPM and key estimate - "
        "use it when the song changes. Starts the analysis if it is stopped."));
    btnReset.onClick = [this]() { onResetClicked(); };
    addAndMakeVisible (btnReset);

    // 分析引擎默认就是 enabled（是否真的收到 PCM 由 Hub 的 Kind 引用计数决定），
    // 所以这里不主动 setEnabled(true) —— 否则"添加模块菜单的 hover 预览"
    // 会构造一个临时模块并顺手清空正在运行的分析。
    latest = hub.getMusicAnalysisResult();
    refreshButtons();
    syncChildChromeVisibility();

    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        syncChildChromeVisibility();
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

TempoKeyModule::~TempoKeyModule()
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

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::MusicAnalysis);

    // 注意：这里不强制 setEnabled(false)。
    //   · Hub 的 pushStereo 由 Kind 引用计数决定是否喂数据，模块全部卸载后
    //     引擎即使仍是 enabled 也拿不到新样本；
    //   · 由用户显式 STOP 才停止，避免"关闭一个模块顺手停掉另一个模块"的行为。
}

// ----------------------------------------------------------
// 持久化 / 控制面板
// ----------------------------------------------------------
juce::ValueTree TempoKeyModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("showBpm",        showBpm,        nullptr);
    s.setProperty ("showKey",        showKey,        nullptr);
    s.setProperty ("keyNotation",    keyNotation,    nullptr);
    s.setProperty ("showRelative",   showRelative,   nullptr);
    s.setProperty ("bpmPrecision",   bpmPrecision,   nullptr);
    s.setProperty ("showConfidence", showConfidence, nullptr);
    s.setProperty ("showStatus",     showStatus,     nullptr);
    return s;
}

void TempoKeyModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    bool nextBpm = showBpm;
    bool nextKey = showKey;
    if (state.hasProperty ("showBpm"))        nextBpm        = (bool) state.getProperty ("showBpm");
    if (state.hasProperty ("showKey"))        nextKey        = (bool) state.getProperty ("showKey");
    if (! nextBpm && ! nextKey)               { nextBpm = true; nextKey = true; }
    showBpm = nextBpm;
    showKey = nextKey;

    if (state.hasProperty ("keyNotation"))    keyNotation    = juce::jlimit (0, 2, (int) state.getProperty ("keyNotation"));
    if (state.hasProperty ("showRelative"))   showRelative   = (bool) state.getProperty ("showRelative");
    if (state.hasProperty ("bpmPrecision"))   bpmPrecision   = juce::jlimit (0, 1, (int) state.getProperty ("bpmPrecision"));
    if (state.hasProperty ("showConfidence")) showConfidence = (bool) state.getProperty ("showConfidence");
    if (state.hasProperty ("showStatus"))     showStatus     = (bool) state.getProperty ("showStatus");

    refreshControlPanel();
    repaint();
}

void TempoKeyModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::toggle ("showBpm", "BPM column",
        "Show the detected tempo (BPM) readout.").in ("Show"));
    out.push_back (ParamDef::toggle ("showKey", "Key column",
        "Show the detected musical key readout.").in ("Show"));

    out.push_back (ParamDef::choice ("keyNotation", "Key notation",
        { "Standard", "Camelot", "Std + Camelot" },
        "Standard: classic musical key (C major / A minor). Camelot: DJ wheel code (8B / 8A). Std + Camelot: show both together.")
        .in ("Key").when ([this] { return showKey; }));
    out.push_back (ParamDef::toggle ("showRelative", "Relative key",
        "Show the relative major/minor key subtitle beneath the detected key.")
        .in ("Key").when ([this] { return showKey; }));

    out.push_back (ParamDef::choice ("bpmPrecision", "BPM precision",
        { "0.1 BPM", "Integer" },
        "Show BPM with one decimal place or rounded to the nearest whole number.")
        .in ("Display").when ([this] { return showBpm; }));
    out.push_back (ParamDef::toggle ("showConfidence", "Confidence bar",
        "Show the stability bar and percentage beneath each readout.").in ("Display"));
    out.push_back (ParamDef::toggle ("showStatus", "Status bar",
        "Show the bottom tracking status LED and input signal bar.").in ("Display"));
}

double TempoKeyModule::getParamValue (const juce::String& id) const
{
    if (id == "showBpm")        return showBpm ? 1.0 : 0.0;
    if (id == "showKey")        return showKey ? 1.0 : 0.0;
    if (id == "keyNotation")    return (double) keyNotation;
    if (id == "showRelative")   return showRelative ? 1.0 : 0.0;
    if (id == "bpmPrecision")   return (double) bpmPrecision;
    if (id == "showConfidence") return showConfidence ? 1.0 : 0.0;
    if (id == "showStatus")     return showStatus ? 1.0 : 0.0;
    return 0.0;
}

void TempoKeyModule::setParamValue (const juce::String& id, double v)
{
    const bool on = v >= 0.5;
    if (id == "showBpm")
    {
        if (! on && ! showKey) return;
        showBpm = on;
    }
    else if (id == "showKey")
    {
        if (! on && ! showBpm) return;
        showKey = on;
    }
    else if (id == "keyNotation")    keyNotation    = juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "showRelative")   showRelative   = on;
    else if (id == "bpmPrecision")   bpmPrecision   = juce::jlimit (0, 1, juce::roundToInt (v));
    else if (id == "showConfidence") showConfidence = on;
    else if (id == "showStatus")     showStatus     = on;
    else return;

    repaint();
}

// ----------------------------------------------------------
// onFrame（UI 线程）
// ----------------------------------------------------------
void TempoKeyModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing() || ! isVisuallyActiveInWorkspace())
        return;

    if (! frame.has (AnalyserHub::Kind::MusicAnalysis))
        return;

    latest = frame.musicAnalysis;

    refreshButtons();

    // 结果每 200ms 才可能变化，UI 5Hz 刷新足够，避免无谓重绘。
    const double nowMs = juce::Time::currentTimeMillis();

    if (lastRepaintMs > 0.0 && (nowMs - lastRepaintMs) < 200.0)
        return;

    lastRepaintMs = nowMs;
    repaintFrameContent();
}

void TempoKeyModule::onStartStopClicked()
{
    auto& engine = hub.getMusicAnalysisEngine();
    engine.setEnabled (! engine.isEnabled());

    lastRepaintMs = 0.0;
    refreshButtons();
    repaint();
}

void TempoKeyModule::onResetClicked()
{
    auto& engine = hub.getMusicAnalysisEngine();

    hub.resetMusicAnalysis();

    // RESET 的语义是"清空并重新开始"：如果分析还没启动（按钮仍是 START），
    //   顺手替用户按下 START，否则重置完只会停在那里，还得再点一次才开始。
    if (! engine.isEnabled())
        engine.setEnabled (true);

    lastRepaintMs = 0.0;
    refreshButtons();
    repaint();
}

void TempoKeyModule::refreshButtons()
{
    const bool running = hub.getMusicAnalysisEngine().isEnabled();

    const juce::String wanted = running ? "STOP" : "START";

    if (btnStartStop.getButtonText() != wanted)
        btnStartStop.setButtonText (wanted);

    if (btnStartStop.getToggleState() != running)
        btnStartStop.setToggleState (running, juce::dontSendNotification);
}

void TempoKeyModule::syncChildChromeVisibility()
{
    const bool hideChildren = PinkXP::isTransparentBackdrop() && ! PinkXP::isChromaRevealed (*this);
    const float alpha = hideChildren ? 0.0f : 1.0f;
    if (std::abs (btnStartStop.getAlpha() - alpha) > 0.001f)
        btnStartStop.setAlpha (alpha);
    if (std::abs (btnReset.getAlpha() - alpha) > 0.001f)
        btnReset.setAlpha (alpha);
}

// ----------------------------------------------------------
// 布局 / 绘制
// ----------------------------------------------------------
void TempoKeyModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    auto area = contentBounds.reduced (4);

    if (area.getWidth() <= 0 || area.getHeight() <= 0)
    {
        btnStartStop.setVisible (false);
        btnReset.setVisible (false);
        return;
    }

    const int btnH = buttonRowHeight (contentBounds);
    auto row = area.removeFromBottom (btnH);

    const int btnW = juce::jlimit (40, 56, area.getWidth() / 4);

    btnReset.setVisible (true);
    btnStartStop.setVisible (true);

    btnReset.setBounds (row.removeFromRight (btnW).withSizeKeepingCentre (btnW, btnH));

    if (row.getWidth() > btnW + 8)
        row.removeFromRight (4);

    btnStartStop.setBounds (row.removeFromRight (btnW).withSizeKeepingCentre (btnW, btnH));
}

TempoKeyModule::LayoutRects TempoKeyModule::computeLayout (juce::Rectangle<int> contentBounds) const
{
    LayoutRects out;

    auto area = contentBounds.reduced (4);
    const int btnH = buttonRowHeight (contentBounds);
    area.removeFromBottom (btnH + 4);

    auto inner = area.reduced (7, 5);

    if (showStatus)
    {
        const int statusH = juce::jlimit (10, 14, inner.getHeight() / 7);
        out.statusRow = inner.removeFromBottom (statusH);
        inner.removeFromBottom (2);
    }

    if (showBpm && showKey)
    {
        out.keyCol = inner.removeFromRight (inner.getWidth() / 2);
        out.bpmCol = inner;
        out.keyCol.removeFromLeft (6);
    }
    else if (showBpm)
    {
        out.bpmCol = inner;
    }
    else if (showKey)
    {
        out.keyCol = inner;
    }

    return out;
}

// ==========================================================
// Toybox：点击 BPM / KEY 列 → 数据气泡
//   命中这两列时不再走 ModulePanel 的通用 ✨ 兜底，
//   因此也不会出现"火花掉到置信度条附近卡住"的现象。
// ==========================================================
bool TempoKeyModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto content = getContentBounds();

    if (! content.contains (panelPos))
        return false;

    const auto layout = computeLayout (content);

    if (layout.bpmCol.contains (panelPos))
    {
        out.kind = 1;
        out.rect = layout.bpmCol.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }

    if (layout.keyCol.contains (panelPos))
    {
        out.kind = 2;
        out.rect = layout.keyCol.toFloat();
        out.pos  = panelPos.toFloat();
        return true;
    }

    return false;
}

void TempoKeyModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    const auto bubbleAt = hit.pos.translated (0.0f, -22.0f);

    if (hit.kind == 1)
    {
        // 严格按当前真实 BPM 周期律动的节拍脉冲环 + 底边摆动节拍光点
        ctx.addModuleActorCapped (
            makeBpmMetronomePulseActor (hit.rect.toNearestInt(), hit.pos, latest.bpm),
            6);

        if (latest.bpm <= 0.0f)
        {
            // 还没抓到 BPM：也给出反馈，而不是点击无反应
            ctx.bubble (bubbleAt, y2k::tr ("No BPM yet - play some music and wait a moment."),
                        ToyRole::Reward);
            return;
        }

        // 纯 ASCII：Silkscreen 只保证拉丁字形，"×/÷" 可能落到缺字方框
        const juce::String text = juce::String ("x2 ")
                                + juce::String (latest.bpm * 2.0f, 1)
                                + "    /2 "
                                + juce::String (latest.bpm * 0.5f, 1);

        ctx.bubble (bubbleAt, text, ToyRole::Reward);
        return;
    }

    if (hit.kind == 2)
    {
        int modeIdx = 0;
        switch (latest.keyMode)
        {
            case y2k::music::Mode::major:         modeIdx = 0; break;
            case y2k::music::Mode::naturalMinor:  modeIdx = 1; break;
            case y2k::music::Mode::harmonicMinor: modeIdx = 2; break;
            case y2k::music::Mode::melodicMinor:  modeIdx = 3; break;
            case y2k::music::Mode::mixolydian:    modeIdx = 4; break;
        }
        ctx.addModuleActorCapped (
            makeKeyConstellationActor (hit.rect.toNearestInt(), hit.pos, latest.keyRoot, modeIdx),
            6);

        const juce::String text = scaleNoteNames (latest.keyRoot, latest.keyMode);

        if (text.isNotEmpty())
        {
            ctx.bubble (bubbleAt, text, ToyRole::Pin);
            return;
        }

        // 还没识别出调性：同样给反馈
        ctx.bubble (bubbleAt, y2k::tr ("No key detected yet - play some music and wait a moment."),
                    ToyRole::Pin);
    }
}

// ==========================================================
// 音阶音名：按调号的字母顺序 + 必要升降号拼写
//   · 主音字母由 keySpelling() 决定（Db / Eb / F# …）
//   · 每个级数用相邻的下一个字母（D E F G A B C …）
//   · 该字母的自然音级与目标音级的差 = 需要的升降号
//   → Eb minor: Eb F Gb Ab Bb Cb Db
//   → F# major: F# G# A# B C# D# E#
// ==========================================================
juce::String TempoKeyModule::scaleNoteNames (int root, y2k::music::Mode mode)
{
    if (root < 0 || root > 11)
        return {};

    const auto spelling = y2k::music::keySpelling (root, mode);

    if (spelling.name == nullptr || spelling.name[0] == '\0')
        return {};

    static const char kLetters[7]  = { 'C', 'D', 'E', 'F', 'G', 'A', 'B' };
    static const int  kLetterPc[7] = { 0, 2, 4, 5, 7, 9, 11 };

    static const int kMajorSteps[7]    = { 0, 2, 4, 5, 7, 9, 11 };
    static const int kNatMinorSteps[7] = { 0, 2, 3, 5, 7, 8, 10 };
    static const int kHarmSteps[7]     = { 0, 2, 3, 5, 7, 8, 11 };
    static const int kMelSteps[7]      = { 0, 2, 3, 5, 7, 9, 11 };

    const int* steps = kMajorSteps;

    switch (mode)
    {
        case y2k::music::Mode::major:         steps = kMajorSteps;    break;
        case y2k::music::Mode::naturalMinor:  steps = kNatMinorSteps; break;
        case y2k::music::Mode::harmonicMinor: steps = kHarmSteps;     break;
        case y2k::music::Mode::melodicMinor:  steps = kMelSteps;      break;
    }

    int letterIndex = 0;

    for (int i = 0; i < 7; ++i)
        if (kLetters[i] == spelling.name[0])
        {
            letterIndex = i;
            break;
        }

    juce::String out;

    for (int degree = 0; degree < 7; ++degree)
    {
        const int targetPc = (root + steps[degree]) % 12;
        const int li       = (letterIndex + degree) % 7;

        int accidental = targetPc - kLetterPc[li];
        while (accidental > 6)   accidental -= 12;
        while (accidental < -6)  accidental += 12;

        if (degree > 0)
            out += " ";

        out += juce::String::charToString ((juce::juce_wchar) kLetters[li]);

        for (int k = 0; k < accidental; ++k)
            out += "#";

        for (int k = 0; k < -accidental; ++k)
            out += "b";
    }

    return out;
}

juce::Rectangle<int> TempoKeyModule::getChromaKeyCanvas()
{
    auto area = getContentBounds().reduced (4);
    if (area.getWidth() < 60 || area.getHeight() < 40)
        return {};

    const int btnH = buttonRowHeight (getContentBounds());
    area.removeFromBottom (btnH + 4);
    return area;
}

void TempoKeyModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    // 绿幕纯绿 / 透明模式跳过不透明底色（保持 alpha=0）/ 正常模式按风格填充
    const auto& tokens = PinkXP::getSkinTokens();
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
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube
                 || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
            const float darken = (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                                     ? (isDark ? 0.16f : 0.07f)
                                     : (isDark ? 0.10f : 0.03f);
            g.setColour (PinkXP::face.interpolatedWith (juce::Colours::black, darken));
            g.fillRect (contentBounds);
        }
        else
        {
            g.setColour (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel ? PinkXP::btnFace : PinkXP::face);
            g.fillRect (contentBounds);
        }
    }

    auto area = contentBounds.reduced (4);

    if (area.getWidth() < 60 || area.getHeight() < 40)
        return;

    const int btnH = buttonRowHeight (contentBounds);
    area.removeFromBottom (btnH + 4);

    PinkXP::Craft::drawReadoutCard (g, area, PinkXP::content);

    // ---- 两列：BPM | KEY（与 toyHitTest 共用同一套几何）----
    const auto layout = computeLayout (contentBounds);

    auto statusRow = layout.statusRow;
    auto bpmCol    = layout.bpmCol;
    auto keyCol    = layout.keyCol;

    const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool isDarkCard = PinkXP::content.getPerceivedBrightness() < 0.45f;

    if (showBpm && showKey)
    {
        if (isChrome && ! tokens.isChromaKey)
        {
            // 刻蚀 2px 金属中缝（1px 暗槽 + 1px 镜面高光）
            const int divX = keyCol.getX() - 3;
            const int divY = keyCol.getY() + 2;
            const int divH = juce::jmax (2, keyCol.getHeight() - 4);
            g.setColour (PinkXP::dark.withAlpha (isDarkCard ? 0.56f : 0.34f));
            g.fillRect (divX, divY, 1, divH);
            g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f)
                                   .withAlpha (isDarkCard ? 0.24f : 0.68f));
            g.fillRect (divX + 1, divY, 1, divH);
        }
        else if (isGlass && ! tokens.isChromaKey)
        {
            const int divX = keyCol.getX() - 3;
            const int divY = keyCol.getY() + 4;
            const int divH = juce::jmax (2, keyCol.getHeight() - 8);
            g.setColour (isDarkCard ? juce::Colours::white.withAlpha (0.16f)
                                    : juce::Colours::black.withAlpha (0.10f));
            g.fillRect (divX, divY, 1, divH);
            g.setColour (juce::Colours::white.withAlpha (isDarkCard ? 0.06f : 0.48f));
            g.fillRect (divX + 1, divY, 1, divH);
        }
        else if (isNeon && ! tokens.isChromaKey)
        {
            // 发光霓虹激光中线
            const int divX = keyCol.getX() - 3;
            const int divY = keyCol.getY() + 2;
            const int divH = juce::jmax (2, keyCol.getHeight() - 4);
            if (! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (PinkXP::sel.withAlpha (0.22f));
                g.fillRect (divX - 1, divY, 3, divH);
            }
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.85f));
            g.fillRect (divX, divY, 1, divH);
        }
        else if (isCrt && ! tokens.isChromaKey)
        {
            // Tektronix 磷光中轴标尺线 + 微型水平刻度齿
            const int divX = keyCol.getX() - 3;
            const int divY = keyCol.getY() + 1;
            const int divH = juce::jmax (2, keyCol.getHeight() - 2);
            g.setColour (PinkXP::sel.withAlpha (0.56f));
            g.fillRect (divX, divY, 1, divH);
            const int step = juce::jmax (10, divH / 5);
            for (int ty = divY + step; ty < divY + divH - 2; ty += step)
                g.fillRect (divX - 2, ty, 5, 1);
        }
        else
        {
            const float divX = (float) (keyCol.getX() - 3);
            const float padY = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio) ? 4.0f : 0.0f;
            PinkXP::Craft::drawGridLine (g, divX, (float) keyCol.getY() + padY,
                                         divX, (float) keyCol.getBottom() - padY,
                                         true, PinkXP::pink600);
        }
    }

    // ---- 行高（两列共用，保证基线对齐）----
    const int colH    = (showBpm ? bpmCol : keyCol).getHeight();
    const int statusH = statusRow.getHeight();
    const int labelPx = juce::jlimit (7, 11, (int) std::lround ((double) colH * 0.13));
    const int agePx   = juce::jlimit (6, 9,  (int) std::lround ((double) colH * 0.11));
    const int confPx  = showConfidence ? juce::jlimit (7, 11, (int) std::lround ((double) colH * 0.11)) : 0;

    const int bodyH = colH - labelPx - agePx - confPx;

    // 关系调副行：约占主数值区域的 2/5（至少 9px），两列预留同样高度。
    //   比例取 0.40 是为了让副行字号能达到主数值的 ~0.62 而不挤压主数值。
    const int subPx = (bodyH >= 34)
                          ? juce::jlimit (9, 34, (int) std::lround ((double) bodyH * 0.40))
                          : 0;

    // 三种视觉状态：
    //   stale       —— 当前信号不再支持该读数（变暗 + 标注 stale）
    //   provisional —— 有读数但置信度 < 50%（半暗 + 标注 provisional）
    //   normal      —— 正常
    const bool bpmStale = ! latest.bpmValid && latest.bpm > 0.0f;
    const bool keyStale = ! latest.keyValid && latest.keyRoot >= 0;
    const bool bpmProv  = latest.bpmValid && latest.bpmConfidence < 0.50f;
    const bool keyProv  = latest.keyValid && latest.keyConfidence < 0.50f;

    auto drawColumn = [&] (juce::Rectangle<int> col,
                           const juce::String& labelText,
                           const juce::String& valueText,
                           const juce::String& subText,
                           const juce::String& ageText,
                           float confidence,
                           bool valid,
                           bool stale,
                           bool provisional)
    {
        if (col.getWidth() <= 12 || col.getHeight() <= 18)
            return;

        auto labelArea = col.removeFromTop (labelPx);
        auto ageArea   = col.removeFromBottom (agePx);
        auto confArea  = (confPx > 0) ? col.removeFromBottom (confPx) : juce::Rectangle<int>();

        auto subArea = juce::Rectangle<int>();

        if (subPx > 0)
            subArea = col.removeFromBottom (subPx);

        // 标签（"KEY (provisional)" 这类长文本同样按宽度收缩，窄模块不裁字）
        float labelH = clampFont ((float) labelPx * 0.92f, 7.0f, 10.5f);

        {
            const float avail = (float) labelArea.getWidth() - 2.0f;
            const float w = juce::GlyphArrangement::getStringWidth (
                PinkXP::getAxisFont (labelH, juce::Font::bold), y2k::tr (labelText));

            if (w > avail && w > 0.0f)
                labelH = juce::jmax (6.0f, labelH * avail / w);
        }

        g.setFont (PinkXP::fitFontToHeight (PinkXP::getAxisFont (labelH, juce::Font::bold),
                                            (float) juce::jmax (6, labelArea.getHeight())));
        g.setColour (PinkXP::ink.withAlpha (stale ? 0.30f : (provisional ? 0.45f : 0.65f)));
        g.drawText (y2k::tr (labelText), labelArea, juce::Justification::centredLeft, false);

        // 主数值：按"剩余高度 + 实际文字宽度"求最大可用字号，
        //   宽度用字体真实测量（不再靠字符数估算），任何尺寸都不会裁掉末尾字母。
        const float valueH = fitTextHeight (valueText,
                                            (float) col.getWidth() - 4.0f,
                                            (float) col.getHeight() * 0.62f,
                                            8.0f,
                                            juce::Font::bold);

        if (col.getWidth() > 0 && col.getHeight() > 0)
        {
            g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (valueH, juce::Font::bold),
                                                (float) col.getHeight()));
            g.setColour (stale ? PinkXP::ink.withAlpha (0.28f)
                               : (provisional ? PinkXP::pink700.withAlpha (0.55f)
                                              : PinkXP::pink700));
            g.drawText (y2k::tr (valueText), col, juce::Justification::centred, false);
        }

        // 关系调副行：字号约为主数值的 0.62（"gently smaller"），
        //   同样按副行宽度收缩，避免在窄模块里被裁掉。
        if (! subArea.isEmpty() && subText.isNotEmpty())
        {
            const float subMax = juce::jmin ((float) subArea.getHeight() * 0.60f,
                                             valueH * 0.62f);

            const float subH = fitTextHeight (subText,
                                              (float) subArea.getWidth() - 4.0f,
                                              subMax,
                                              8.0f,
                                              juce::Font::plain);

            g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (subH, juce::Font::plain),
                                                (float) subArea.getHeight()));
            g.setColour (PinkXP::ink.withAlpha (stale ? 0.25f : 0.60f));

            // 作为大字读数的"副标题"居中显示，视觉上隶属于上方调性
            g.drawText (y2k::tr (subText), subArea, juce::Justification::centred, false);
        }

        // 时效
        g.setFont (PinkXP::fitFontToHeight (PinkXP::getAxisFont (clampFont ((float) agePx * 0.92f, 6.5f, 9.5f),
                                                                 juce::Font::plain),
                                            (float) juce::jmax (6, ageArea.getHeight())));
        g.setColour (PinkXP::ink.withAlpha (0.45f));
        g.drawText (y2k::tr (ageText), ageArea, juce::Justification::centredLeft, false);

        // 置信度：细条 + 百分比（只在结果有效时显示）
        if (valid && confArea.getHeight() > 0)
        {
            auto pctArea = confArea.removeFromRight (juce::jmin (34, confArea.getWidth() / 2));
            auto bar = confArea.reduced (0, juce::jmax (1, confArea.getHeight() / 4));

            drawTempoProgressBar (g, bar, confidence, provisional ? PinkXP::warn : PinkXP::pink500);

            g.setFont (PinkXP::getAxisFont (clampFont ((float) confPx * 0.72f, 6.0f, 8.5f),
                                            juce::Font::bold));
            g.setColour (PinkXP::ink.withAlpha (0.55f));
            g.drawText (y2k::tr (juce::String ((int) std::lround ((double) confidence * 100.0)) + "%"),
                        pctArea, juce::Justification::centredRight, false);
        }
    };

    // ---- BPM 列 ----
    if (showBpm)
        drawColumn (bpmCol,
                    bpmStale ? "BPM (stale)" : (bpmProv ? "BPM (provisional)" : "BPM"),
                    formatBpm (latest),
                    {},
                    latest.bpm > 0.0f ? formatAge (latest.bpmUpdatedMs) : juce::String(),
                    latest.bpmConfidence,
                    latest.bpmValid,
                    bpmStale,
                    bpmProv);

    // ---- KEY 列 ----
    if (showKey)
    {
        juce::String keySub;

        if (latest.keyRoot >= 0)
        {
            // 自然小调是默认值，不再重复写 "natural"；只有和声/旋律小调才标注限定词，
            // 这样副行留给关系调，且能放大到"略小于主数值"的字号而不被宽度裁掉。
            if (latest.keyMode == y2k::music::Mode::harmonicMinor)
                keySub = y2k::tr ("harmonic");
            else if (latest.keyMode == y2k::music::Mode::melodicMinor)
                keySub = y2k::tr ("melodic");

            // 关系调：与当前调共享**完全相同的 7 个音**，只是主音不同。
            //   大调 ↔ 关系小调；自然小调 ↔ 关系大调；
            //   和声/旋律小调没有关系调（name == nullptr，不显示）。
            if (showRelative)
            {
                const auto rel = y2k::music::relativeKeyOf (latest.keyRoot, latest.keyMode);

                if (rel.name != nullptr)
                {
                    // 界面语言：模板 + 调式词分别翻译（音名本身不翻译）
                    const juce::String r = y2k::trf ("rel. {0} {1}", rel.name,
                                                     y2k::tr (rel.minor ? "minor" : "major"));

                    keySub = keySub.isEmpty() ? r : (keySub + " · " + r);
                }
            }
        }

        drawColumn (keyCol,
                    keyStale ? "KEY (stale)" : (keyProv ? "KEY (provisional)" : "KEY"),
                    formatKey (latest),
                    keySub,
                    latest.keyRoot >= 0 ? formatAge (latest.keyUpdatedMs) : juce::String(),
                    latest.keyConfidence,
                    latest.keyValid,
                    keyStale,
                    keyProv);
    }

    // ---- 状态行 ----
    if (showStatus && ! statusRow.isEmpty())
    {
        if (isChrome && ! tokens.isChromaKey)
        {
            const int ruleY = statusRow.getY() - 2;
            g.setColour (PinkXP::dark.withAlpha (isDarkCard ? 0.52f : 0.30f));
            g.fillRect (statusRow.getX(), ruleY, statusRow.getWidth(), 1);
            g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f)
                                   .withAlpha (isDarkCard ? 0.22f : 0.62f));
            g.fillRect (statusRow.getX(), ruleY + 1, statusRow.getWidth(), 1);
        }
        else if (isNeon && ! tokens.isChromaKey)
        {
            const int ruleY = statusRow.getY() - 2;
            if (! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (PinkXP::sel.withAlpha (0.18f));
                g.fillRect (statusRow.getX(), ruleY - 1, statusRow.getWidth(), 3);
            }
            g.setColour (PinkXP::sel.withAlpha (0.58f));
            g.fillRect (statusRow.getX(), ruleY, statusRow.getWidth(), 1);
        }
        else if (isCrt && ! tokens.isChromaKey)
        {
            const int ruleY = statusRow.getY() - 2;
            g.setColour (PinkXP::sel.withAlpha (0.48f));
            g.fillRect (statusRow.getX(), ruleY, statusRow.getWidth(), 1);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio || isGlass)
        {
            const float ruleY = (float) (statusRow.getY() - 2);
            PinkXP::Craft::drawGridLine (g, (float) statusRow.getX(), ruleY,
                                         (float) statusRow.getRight(), ruleY,
                                         false, PinkXP::ink);
        }

        const int ledSz = juce::jlimit ((isChrome || isGlass || isNeon) ? 6 : 5, 9,
                                        statusRow.getHeight() - ((isChrome || isGlass || isNeon) ? 3 : 4));
        auto ledSlot    = statusRow.removeFromLeft (statusRow.getHeight());
        const auto led  = ledSlot.withSizeKeepingCentre (ledSz, ledSz);
        const bool ledLit = (latest.status != y2k::music::Status::idle);
        PinkXP::Craft::drawStatusLed (g, led, statusColour (latest.status), ledLit, true);

        auto levelArea = statusRow.removeFromRight (juce::jmin (56, statusRow.getWidth() / 3));
        statusRow.removeFromRight (4);

        g.setColour (PinkXP::ink.withAlpha (0.60f));
        g.setFont (PinkXP::fitFontToHeight (PinkXP::getAxisFont (clampFont ((float) statusH * 0.85f, 7.5f, 10.0f),
                                                                 juce::Font::bold),
                                            (float) juce::jmax (6, statusRow.getHeight())));
        g.drawText (y2k::tr (y2k::music::statusText (latest.status)), statusRow,
                    juce::Justification::centredLeft, false);

        // 电平条（仅作输入是否有信号的辅助提示）
        if (! levelArea.isEmpty())
        {
            auto bar = levelArea.reduced (0, 2);
            const float norm = juce::jlimit (0.0f, 1.0f, (latest.levelDb + 60.0f) / 60.0f);
            const auto barCol = (latest.status == y2k::music::Status::noSignal)
                                    ? PinkXP::ink.withAlpha (0.35f)
                                    : PinkXP::pink500;
            drawTempoProgressBar (g, bar, norm, barCol);
        }
    }

    PinkXP::Craft::drawCanvasFinishOverlay (g, area.reduced (2));
}

// ----------------------------------------------------------
// 文本格式化
// ----------------------------------------------------------
juce::String TempoKeyModule::camelotCode (int root, y2k::music::Mode mode)
{
    if (root < 0 || root > 11)
        return {};
    static constexpr const char* kMajorCamelot[12] =
    {
        "8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B"
    };
    static constexpr const char* kMinorCamelot[12] =
    {
        "5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A"
    };
    return y2k::music::isMinorMode (mode) ? kMinorCamelot[root] : kMajorCamelot[root];
}

juce::String TempoKeyModule::formatBpm (const y2k::music::Result& r) const
{
    // 保留上一次数值：无效/静音时由 paint 变暗 + 标注 (stale)，
    // 而不是把读数清成 "--"，这样"旧结果"和"当前结果"在视觉上可区分。
    if (r.bpm <= 0.0f)
        return "--";

    if (bpmPrecision == 1)
        return juce::String (juce::roundToInt (r.bpm));

    return juce::String (r.bpm, 1);
}

juce::String TempoKeyModule::formatKey (const y2k::music::Result& r) const
{
    if (r.keyRoot < 0 || r.keyRoot > 11)
        return "--";

    const auto spelling = y2k::music::keySpelling (r.keyRoot, r.keyMode);

    if (spelling.name == nullptr)
        return "--";

    const juce::String cam = camelotCode (r.keyRoot, r.keyMode);
    if (keyNotation == 1 && cam.isNotEmpty())
        return cam;

    // 主写法按调式选（大调 Db / 小调 Eb）；关系调显示在副行。
    const juce::String stdKey = juce::String (spelling.name) + " "
                              + y2k::tr (y2k::music::isMinorMode (r.keyMode) ? "minor" : "major");
    if (keyNotation == 2 && cam.isNotEmpty())
        return stdKey + " (" + cam + ")";

    return stdKey;
}

juce::String TempoKeyModule::formatAge (double updatedMs)
{
    if (updatedMs <= 0.0)
        return {};

    const double ageMs = juce::Time::currentTimeMillis() - updatedMs;

    if (ageMs < 1500.0)
        return "now";

    if (ageMs < 60000.0)
        return juce::String ((int) (ageMs / 1000.0)) + "s ago";

    return juce::String ((int) (ageMs / 60000.0)) + "m ago";
}

juce::Colour TempoKeyModule::statusColour (y2k::music::Status s)
{
    switch (s)
    {
        case y2k::music::Status::tracking:  return PinkXP::ok;
        case y2k::music::Status::analysing: return PinkXP::warn;
        case y2k::music::Status::noSignal:  return PinkXP::danger.withAlpha (0.65f);
        case y2k::music::Status::idle:      return PinkXP::ink.withAlpha (0.35f);
    }

    return PinkXP::ink;
}
