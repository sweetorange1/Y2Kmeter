#include "source/ui/modules/UtilityTools.h"

#include "source/ui/modules/ScreenColourSampler.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"

#include <array>
#include <cmath>

// ==========================================================
// UtilityTools.cpp —— 计算器 / 取色器 / 秒表 / 倒计时 / 音频换算 / 时钟 / 便签
// ==========================================================

namespace y2k::util
{
namespace
{
    // ------------------------------------------------------
    // 通用绘制辅助
    // ------------------------------------------------------
    juce::Font fitFont (const juce::String& text, float maxW, float maxH, float minH, int flags)
    {
        float h = juce::jmax (minH, maxH);

        for (int pass = 0; pass < 5; ++pass)
        {
            const auto f = PinkXP::getAxisFont (h, flags);
            const float w = juce::GlyphArrangement::getStringWidth (f, text);

            if (w <= maxW || h <= minH)
                return f;

            h = juce::jmax (minH, h * maxW / w * 0.97f);
        }

        return PinkXP::getAxisFont (h, flags);
    }

    void drawFitted (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                     juce::Justification j, float maxH, float minH = 6.0f, int flags = juce::Font::plain)
    {
        if (r.isEmpty() || text.isEmpty())
            return;

        g.setFont (PinkXP::fitFontToHeight (fitFont (text, (float) r.getWidth() - 2.0f, maxH, minH, flags),
                                            (float) juce::jmax (1, r.getHeight())));
        g.drawText (text, r, j, true);
    }

    juce::Colour inkOnContent()
    {
        return PinkXP::readableInkOn (PinkXP::content);
    }

    // 工具内嵌显示框：Modern / Glass 圆角卡片，Chrome 拉丝金属面板，CRT 0px 磷光终端卡片；Pixel XP / Neon 为凹槽
    PinkXP::WellKind displayWellKind() noexcept
    {
        const auto fs = PinkXP::getSkinTokens().frameStyle;
        return (fs == PinkXP::FrameStyle::modernStudio
                || fs == PinkXP::FrameStyle::frostedGlass
                || fs == PinkXP::FrameStyle::machinedChrome
                || fs == PinkXP::FrameStyle::crtPhosphor)
                   ? PinkXP::WellKind::card
                   : PinkXP::WellKind::canvas;
    }

    // 仪器内水平分割线：Chrome 刻蚀 2px 金属沟槽 / Glass 柔透双细线 / Neon 发光激光线 / CRT 磷光线 / 其余走 Craft::drawGridLine
    void drawInstrumentDivider (juce::Graphics& g, float x0, float y, float x1, juce::Colour fallbackInk)
    {
        const auto& tokens = PinkXP::getSkinTokens();
        if (tokens.isChromaKey)
            return;
        const bool isDark = PinkXP::content.getPerceivedBrightness() < 0.45f;
        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            g.setColour (PinkXP::dark.withAlpha (isDark ? 0.52f : 0.32f));
            g.fillRect (x0, y, x1 - x0, 1.0f);
            g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f)
                                   .withAlpha (isDark ? 0.22f : 0.64f));
            g.fillRect (x0, y + 1.0f, x1 - x0, 1.0f);
            return;
        }
        if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            g.setColour (isDark ? juce::Colours::white.withAlpha (0.16f)
                                : juce::Colours::black.withAlpha (0.10f));
            g.fillRect (x0, y, x1 - x0, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (isDark ? 0.05f : 0.45f));
            g.fillRect (x0, y + 1.0f, x1 - x0, 1.0f);
            return;
        }
        if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            if (! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (PinkXP::sel.withAlpha (0.18f));
                g.fillRect (x0, y - 1.0f, x1 - x0, 3.0f);
            }
            g.setColour (PinkXP::sel.withAlpha (0.62f));
            g.fillRect (x0, y, x1 - x0, 1.0f);
            return;
        }
        if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            g.setColour (PinkXP::sel.withAlpha (0.48f));
            g.fillRect (x0, y, x1 - x0, 1.0f);
            return;
        }
        PinkXP::Craft::drawGridLine (g, x0, y, x1, y, false, fallbackInk);
    }

    void styleEditor (juce::TextEditor& ed, const juce::Font& font)
    {
        const auto fg = inkOnContent();
        const auto& tokens = PinkXP::getSkinTokens();
        const auto outline = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                              || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                                 ? PinkXP::skinHairlineOn (PinkXP::content, 0.24f)
                             : (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
                                 ? PinkXP::dark.withAlpha (0.68f)
                             : (tokens.frameStyle == PinkXP::FrameStyle::neonTube
                                || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                                 ? PinkXP::sel.withAlpha (0.58f)
                                 : PinkXP::shdw;

        ed.setColour (juce::TextEditor::backgroundColourId, PinkXP::content);
        ed.setColour (juce::TextEditor::textColourId, fg);
        ed.setColour (juce::CaretComponent::caretColourId, fg);
        ed.setColour (juce::TextEditor::outlineColourId, outline);
        ed.setColour (juce::TextEditor::focusedOutlineColourId, PinkXP::sel);
        ed.setColour (juce::TextEditor::highlightColourId, PinkXP::sel.withAlpha (0.45f));
        ed.setColour (juce::TextEditor::highlightedTextColourId, fg);

        // TextEditor 的文字颜色 / 字体逐字符保存：存量文本也要改
        ed.applyColourToAllText (fg, true);
        ed.setFont (font);
        ed.applyFontToAllText (font, true);
    }

    void prepButton (juce::Button& b)
    {
        b.setWantsKeyboardFocus (false);
        b.setMouseClickGrabsKeyboardFocus (false);
    }

    juce::String trimNumber (double v, int maxDecimals)
    {
        if (! std::isfinite (v))
            return "Error";

        if (std::abs (v) >= 1.0e15 || (std::abs (v) < 1.0e-9 && v != 0.0))
            return juce::String (v, 6, true);

        auto s = juce::String (v, maxDecimals);
        if (s.containsChar ('.'))
        {
            s = s.trimCharactersAtEnd ("0");
            if (s.endsWithChar ('.'))
                s = s.dropLastCharacters (1);
        }
        if (s == "-0")
            s = "0";
        return s;
    }

    juce::String two (int v) { return juce::String (v).paddedLeft ('0', 2); }

    // 毫秒 → "MM:SS.cc" / "H:MM:SS"
    juce::String formatDuration (double ms, bool hundredths)
    {
        if (ms < 0.0) ms = 0.0;

        const auto total = (juce::int64) ms;
        const int h  = (int) (total / 3600000);
        const int m  = (int) ((total / 60000) % 60);
        const int s  = (int) ((total / 1000) % 60);
        const int cs = (int) ((total / 10) % 100);

        juce::String out = h > 0 ? juce::String (h) + ":" + two (m) + ":" + two (s)
                                 : two (m) + ":" + two (s);
        if (hundredths && h == 0)
            out << "." << two (cs);
        return out;
    }

    juce::int64 nowMs() { return juce::Time::currentTimeMillis(); }

    // 像素风小吸管图标（取色按钮用）
    void drawPipetteIcon (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour c)
    {
        const int s = juce::jmax (1, juce::jmin (r.getWidth(), r.getHeight()) / 7);
        const int x0 = r.getCentreX() - s * 7 / 2;
        const int y0 = r.getCentreY() - s * 7 / 2;

        g.setColour (c);
        for (int i = 0; i < 5; ++i)
            g.fillRect (x0 + i * s, y0 + (6 - i) * s, s, s);
        g.fillRect (x0 + 4 * s, y0, s * 3, s * 3);
        g.fillRect (x0 + 3 * s, y0 + 3 * s, s, s);
    }

    // ------------------------------------------------------
    // 参数 id（同时是工具子节点里的存档键）
    // ------------------------------------------------------
    constexpr const char* kCalcTape      = "calcTape";
    constexpr const char* kPickFormat    = "pickFormat";
    constexpr const char* kPickAutoCopy  = "pickAutoCopy";
    constexpr const char* kSwHundredths  = "swHundredths";
    constexpr const char* kTmSound       = "tmSound";
    constexpr const char* kTmFlash       = "tmFlash";
    constexpr const char* kCvA4          = "cvA4";
    constexpr const char* kClkStyle      = "clkStyle";
    constexpr const char* kClk24h        = "clk24h";
    constexpr const char* kClkSeconds    = "clkSeconds";
    constexpr const char* kClkDate       = "clkDate";
    constexpr const char* kNoteSize      = "noteSize";
}

const char* toolStateName (ToolKind k) noexcept
{
    switch (k)
    {
        case ToolKind::calculator:   return "calculator";
        case ToolKind::colourPicker: return "colourPicker";
        case ToolKind::stopwatch:    return "stopwatch";
        case ToolKind::timer:        return "timer";
        case ToolKind::converter:    return "converter";
        case ToolKind::clock:        return "clock";
        case ToolKind::notes:        return "notes";
        case ToolKind::numTools:     break;
    }
    return "calculator";
}

const char* toolDisplayName (ToolKind k) noexcept
{
    switch (k)
    {
        case ToolKind::calculator:   return "Calculator";
        case ToolKind::colourPicker: return "Colour Picker";
        case ToolKind::stopwatch:    return "Stopwatch";
        case ToolKind::timer:        return "Timer";
        case ToolKind::converter:    return "Audio Calc";
        case ToolKind::clock:        return "Clock";
        case ToolKind::notes:        return "Notes";
        case ToolKind::numTools:     break;
    }
    return "Calculator";
}

void buildToolParams (ToolKind k, std::vector<ParamDef>& out)
{
    switch (k)
    {
        case ToolKind::calculator:
            out.push_back (ParamDef::toggle (kCalcTape, "History tape",
                "Keep a list of the last results above the display."));
            break;

        case ToolKind::colourPicker:
            out.push_back (ParamDef::choice (kPickFormat, "Copy format", { "HEX", "RGB", "HSL" },
                "Format used by the COPY button and auto-copy."));
            out.push_back (ParamDef::toggle (kPickAutoCopy, "Copy after picking",
                "Put the colour on the clipboard as soon as you pick it."));
            break;

        case ToolKind::stopwatch:
            out.push_back (ParamDef::toggle (kSwHundredths, "Show hundredths",
                "Show 1/100 s digits while under one hour."));
            break;

        case ToolKind::timer:
            out.push_back (ParamDef::toggle (kTmSound, "Alert sound",
                "Play the system alert sound when the countdown ends."));
            out.push_back (ParamDef::toggle (kTmFlash, "Flash when done",
                "Flash the display when the countdown ends, until you click it."));
            break;

        case ToolKind::converter:
            out.push_back (ParamDef::slider (kCvA4, "A4 reference", 400.0, 480.0, 1.0, " Hz",
                "Concert pitch used by the note / frequency converter."));
            break;

        case ToolKind::clock:
            out.push_back (ParamDef::choice (kClkStyle, "Clock style", { "Digital", "Analogue" },
                "Big pixel digits or a pixel clock face."));
            out.push_back (ParamDef::toggle (kClk24h, "24-hour", "24-hour time instead of AM / PM."));
            out.push_back (ParamDef::toggle (kClkSeconds, "Show seconds", "Show seconds / the second hand."));
            out.push_back (ParamDef::toggle (kClkDate, "Show date", "Show the date and weekday."));
            break;

        case ToolKind::notes:
            out.push_back (ParamDef::slider (kNoteSize, "Text size", 10.0, 32.0, 1.0, " px",
                "Font size of the note text."));
            break;

        case ToolKind::numTools:
            break;
    }
}

// ==========================================================
// 计算器
// ==========================================================
class CalculatorView : public ToolView
{
public:
    CalculatorView()
    {
        setWantsKeyboardFocus (true);

        static const char* labels[] = { "C", "<-", "+/-", "/",
                                        "7", "8",  "9",   "x",
                                        "4", "5",  "6",   "-",
                                        "1", "2",  "3",   "+",
                                        "%", "0",  ".",   "=" };

        for (auto* l : labels)
        {
            auto b = std::make_unique<juce::TextButton> (l);
            prepButton (*b);
            // 纯拉丁/符号标签：整颗按钮用英文 UI 像素字体（与英文界面完全一致）。
            //   否则中文界面下 "C"/数字/"x" 会落到没有粗体的中文字体上，与
            //   "+ / - / = / %" 这些符号按钮（走 iconFont）粗细不一致（bug2）。
            b->getProperties().set ("y2k.iconFont", true);
            const juce::String key (l);
            b->onClick = [this, key] { grabKeyboardFocus(); press (key); };
            addAndMakeVisible (*b);
            buttons.push_back (std::move (b));
        }

        buttons[0]->setTooltip (y2k::help::make ("C", "Clear everything (Esc)."));
        buttons[1]->setTooltip (y2k::help::make ("<-", "Delete the last digit (Backspace)."));
        buttons[2]->setTooltip (y2k::help::make ("+/-", "Change the sign of the current number."));
        buttons[16]->setTooltip (y2k::help::make ("%", "Percent: with a pending + / - it takes that percentage of the first number."));
    }

    bool getParam (const juce::String& id, double& out) const override
    {
        if (id == kCalcTape) { out = showTape ? 1.0 : 0.0; return true; }
        return false;
    }

    bool setParam (const juce::String& id, double v) override
    {
        if (id != kCalcTape) return false;
        showTape = v >= 0.5;
        resized();
        repaint();
        return true;
    }

    void save (juce::ValueTree& s) const override
    {
        s.setProperty ("entry", entry, nullptr);
        s.setProperty ("acc", acc, nullptr);
        s.setProperty ("op", op == 0 ? juce::String() : juce::String::charToString (op), nullptr);
        s.setProperty ("expr", expr, nullptr);
        s.setProperty ("fresh", fresh, nullptr);
        s.setProperty ("tape", tape.joinIntoString ("\n"), nullptr);
        s.setProperty (kCalcTape, showTape, nullptr);
    }

    void restore (const juce::ValueTree& s) override
    {
        if (s.hasProperty ("entry")) entry = s.getProperty ("entry").toString();
        if (s.hasProperty ("acc"))   acc   = (double) s.getProperty ("acc");
        if (s.hasProperty ("op"))
        {
            const auto o = s.getProperty ("op").toString();
            op = o.isEmpty() ? 0 : o[0];
        }
        if (s.hasProperty ("expr"))  expr  = s.getProperty ("expr").toString();
        if (s.hasProperty ("fresh")) fresh = (bool) s.getProperty ("fresh");
        if (s.hasProperty ("tape"))  tape  = juce::StringArray::fromLines (s.getProperty ("tape").toString());
        tape.removeEmptyStrings();
        if (s.hasProperty (kCalcTape)) showTape = (bool) s.getProperty (kCalcTape);
        if (entry.isEmpty()) entry = "0";
        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int gap = 3;

        int displayH = juce::jlimit (26, 64, r.getHeight() / 4);
        tapeH = (showTape && r.getHeight() >= 190) ? juce::jlimit (0, 90, r.getHeight() / 5) : 0;

        displayRect = r.removeFromTop (displayH + tapeH);
        r.removeFromTop (4);

        const int cols = 4, rows = 5;
        const int bw = (r.getWidth()  - gap * (cols - 1)) / cols;
        const int bh = (r.getHeight() - gap * (rows - 1)) / rows;

        for (int i = 0; i < (int) buttons.size(); ++i)
        {
            const int c = i % cols, row = i / cols;
            const int x = r.getX() + c * (bw + gap);
            const int y = r.getY() + row * (bh + gap);
            const int w = (c == cols - 1) ? r.getRight() - x : bw;
            const int h = (row == rows - 1) ? r.getBottom() - y : bh;
            buttons[(size_t) i]->setBounds (x, y, w, h);
        }
    }

    juce::Rectangle<int> getChromaKeyRect() const override { return displayRect; }

    void setBackdropChromeHidden (bool hidden) override
    {
        const float a = hidden ? 0.0f : 1.0f;
        for (auto& b : buttons)
            if (b != nullptr && std::abs (b->getAlpha() - a) > 0.001f)
                b->setAlpha (a);
    }

    void paint (juce::Graphics& g) override
    {
        PinkXP::Craft::drawPanelWell (g, displayRect, PinkXP::content, displayWellKind());
        auto inner = displayRect.reduced (6, 4);
        const auto ink = inkOnContent();

        if (tapeH > 0)
        {
            auto tapeArea = inner.removeFromTop (tapeH);
            const int lineH = juce::jlimit (9, 13, tapeH / 4);
            const int maxLines = juce::jmax (1, tapeArea.getHeight() / lineH);

            g.setColour (ink.withAlpha (0.45f));
            int y = tapeArea.getBottom() - lineH;
            for (int i = tape.size() - 1, shown = 0; i >= 0 && shown < maxLines; --i, ++shown)
            {
                drawFitted (g, tape[i], { tapeArea.getX(), y, tapeArea.getWidth(), lineH },
                            juce::Justification::centredRight, (float) lineH - 1.0f);
                y -= lineH;
            }

            drawInstrumentDivider (g, (float) inner.getX(), (float) (inner.getY() - 1),
                                   (float) inner.getRight(), ink);
        }

        const int exprH = juce::jlimit (8, 13, inner.getHeight() / 3);
        auto exprRow = inner.removeFromTop (exprH);

        g.setColour (ink.withAlpha (0.5f));
        drawFitted (g, expr, exprRow, juce::Justification::centredRight, (float) exprH);

        if (juce::Time::getMillisecondCounter() < copiedUntil)
        {
            g.setColour (PinkXP::pink500);
            drawFitted (g, y2k::tr ("COPIED"), exprRow, juce::Justification::centredLeft, (float) exprH, 6.0f, juce::Font::bold);
        }

        g.setColour (error ? PinkXP::danger : ink);
        drawFitted (g, error ? y2k::tr (errorText) : entry, inner, juce::Justification::centredRight,
                    (float) inner.getHeight() * 0.92f, 8.0f, juce::Font::bold);

        PinkXP::Craft::drawCanvasFinishOverlay (g, displayRect.reduced (2));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (displayRect.contains (e.getPosition()) && ! error)
        {
            juce::SystemClipboard::copyTextToClipboard (entry);
            copiedUntil = juce::Time::getMillisecondCounter() + 900;
            repaint (displayRect);
            juce::Timer::callAfterDelay (950, [safe = juce::Component::SafePointer<CalculatorView> (this)]
            {
                if (safe != nullptr) safe->repaint();
            });
        }
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const auto c = k.getTextCharacter();

        if (c >= '0' && c <= '9')                   { press (juce::String::charToString (c)); return true; }
        if (c == '.' || c == ',')                   { press ("."); return true; }
        if (c == '+')                               { press ("+"); return true; }
        if (c == '-')                               { press ("-"); return true; }
        if (c == '*' || c == 'x' || c == 'X')       { press ("x"); return true; }
        if (c == '/')                               { press ("/"); return true; }
        if (c == '%')                               { press ("%"); return true; }
        if (c == '=' || k == juce::KeyPress::returnKey) { press ("="); return true; }
        if (k == juce::KeyPress::backspaceKey)      { press ("<-"); return true; }
        if (k == juce::KeyPress::escapeKey || k == juce::KeyPress::deleteKey) { press ("C"); return true; }

        if (k == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0))
        {
            juce::SystemClipboard::copyTextToClipboard (entry);
            return true;
        }
        return false;
    }

private:
    static juce::String opText (juce::juce_wchar o) { return juce::String::charToString (o); }

    static juce::String fmt (double v) { return trimNumber (v, 10); }

    double entryValue() const { return entry.getDoubleValue(); }

    void setError (const juce::String& text)
    {
        error = true;
        errorText = text;
        entry = "0";
        op = 0;
        acc = 0.0;
        fresh = true;
    }

    void clearAll()
    {
        entry = "0"; acc = 0.0; op = 0; expr.clear(); fresh = true; error = false;
    }

    static double apply (double a, juce::juce_wchar o, double b, bool& err)
    {
        switch (o)
        {
            case '+': return a + b;
            case '-': return a - b;
            case 'x': return a * b;
            case '/':
                if (b == 0.0) { err = true; return 0.0; }
                return a / b;
            default:  return b;
        }
    }

    void addTape (const juce::String& line)
    {
        tape.add (line);
        while (tape.size() > 20)
            tape.remove (0);
    }

    void press (const juce::String& key)
    {
        if (error)
        {
            clearAll();
            if (key == "C")
            {
                repaint();
                notifyChanged();
                return;
            }
        }

        const auto k0 = key[0];

        if (key.length() == 1 && (juce::CharacterFunctions::isDigit (k0) || k0 == '.'))
        {
            if (fresh)
            {
                entry = (k0 == '.') ? "0." : key;
                fresh = false;
                if (op == 0) expr.clear();
            }
            else
            {
                if (k0 == '.' && entry.containsChar ('.'))
                    return;
                if (entry.length() >= 16)
                    return;
                entry = (entry == "0" && k0 != '.') ? key : entry + key;
            }
        }
        else if (key == "+" || key == "-" || key == "x" || key == "/")
        {
            if (op != 0 && ! fresh)
            {
                bool err = false;
                const double r = apply (acc, op, entryValue(), err);
                if (err) { setError ("Cannot divide by zero"); repaint(); notifyChanged(); return; }
                acc = r;
                entry = fmt (r);
            }
            else if (op == 0)
            {
                acc = entryValue();
            }

            op = k0;
            fresh = true;
            expr = fmt (acc) + " " + opText (op);
        }
        else if (key == "=")
        {
            if (op != 0)
            {
                const double b = entryValue();
                bool err = false;
                const double r = apply (acc, op, b, err);
                expr = fmt (acc) + " " + opText (op) + " " + fmt (b) + " =";

                if (err) { setError ("Cannot divide by zero"); repaint(); notifyChanged(); return; }

                addTape (expr + " " + fmt (r));
                entry = fmt (r);
                acc = r;
                op = 0;
                fresh = true;
            }
        }
        else if (key == "C")
        {
            clearAll();
        }
        else if (key == "<-")
        {
            if (! fresh)
            {
                entry = entry.dropLastCharacters (1);
                if (entry.isEmpty() || entry == "-")
                    entry = "0";
            }
        }
        else if (key == "+/-")
        {
            if (entry != "0")
                entry = entry.startsWithChar ('-') ? entry.substring (1) : "-" + entry;
            if (fresh && op == 0)
                acc = entryValue();
        }
        else if (key == "%")
        {
            const double v = (op == '+' || op == '-') ? acc * entryValue() / 100.0 : entryValue() / 100.0;
            entry = fmt (v);
            fresh = false;
        }

        repaint();
        notifyChanged();
    }

    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    juce::Rectangle<int> displayRect;
    int tapeH = 0;

    juce::String entry { "0" };
    double acc = 0.0;
    juce::juce_wchar op = 0;
    juce::String expr;
    bool fresh = true;
    bool error = false;
    juce::String errorText;
    juce::StringArray tape;
    bool showTape = true;
    juce::uint32 copiedUntil = 0;
};

// ==========================================================
// 取色器
// ==========================================================
class EyedropperButton : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    std::function<void()> onDown, onUp;
    bool active = false;

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds();
        const auto state = ! isEnabled() ? PinkXP::ControlState::disabled
                         : active        ? PinkXP::ControlState::pressed
                         : isMouseOver() ? PinkXP::ControlState::hover
                                         : PinkXP::ControlState::normal;
        PinkXP::Craft::drawControlChrome (g, r, PinkXP::btnFace, state);

        auto inner = r.reduced (4, 2);
        if (active) inner.translate (1, 1);

        const auto ink = PinkXP::readableInkOn (PinkXP::btnFace);
        const int iconW = juce::jmin (inner.getHeight(), 16);
        drawPipetteIcon (g, inner.removeFromLeft (iconW), ink);
        inner.removeFromLeft (3);

        g.setColour (ink);
        drawFitted (g, y2k::tr ("PICK"), inner, juce::Justification::centred,
                    juce::jmin (12.0f, (float) inner.getHeight() * 0.8f), 6.0f, juce::Font::bold);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { repaint(); }
    void mouseDown  (const juce::MouseEvent&) override { if (onDown) onDown(); }
    void mouseUp    (const juce::MouseEvent&) override { if (onUp) onUp(); }
};

class ColourPickerView : public ToolView,
                         private juce::Timer,
                         private juce::ChangeListener
{
public:
    ColourPickerView()
    {
        const bool live = y2k::screencolour::supportsLiveSampling();
        const bool sys  = y2k::screencolour::supportsSystemSampler();

        pickBtn.setTooltip (y2k::help::make ("Pick",
            live ? "Press and hold, drag anywhere on the screen, release to pick that colour."
                 : "Click, then click anywhere on the screen to pick a colour."));
        pickBtn.onDown = [this, live] { if (live) beginLivePick(); };
        pickBtn.onUp   = [this, live, sys]
        {
            if (live)     endLivePick();
            else if (sys) beginSystemPick();
        };
        pickBtn.setEnabled (live || sys);
        addAndMakeVisible (pickBtn);

        prepButton (copyBtn);
        copyBtn.setTooltip (y2k::help::make ("Copy", "Copy the colour in the format chosen in the gear menu."));
        copyBtn.onClick = [this] { copyCurrent(); };
        addAndMakeVisible (copyBtn);

        prepButton (editBtn);
        editBtn.setTooltip (y2k::help::make ("Edit", "Fine-tune the colour with sliders."));
        editBtn.onClick = [this] { openEditor(); };
        addAndMakeVisible (editBtn);
    }

    ~ColourPickerView() override
    {
        stopTimer();
        if (editorSel != nullptr)
            editorSel->removeChangeListener (this);
    }

    bool getParam (const juce::String& id, double& out) const override
    {
        if (id == kPickFormat)   { out = (double) format; return true; }
        if (id == kPickAutoCopy) { out = autoCopy ? 1.0 : 0.0; return true; }
        return false;
    }

    bool setParam (const juce::String& id, double v) override
    {
        if (id == kPickFormat)   { format = juce::jlimit (0, 2, juce::roundToInt (v)); repaint(); return true; }
        if (id == kPickAutoCopy) { autoCopy = v >= 0.5; return true; }
        return false;
    }

    void save (juce::ValueTree& s) const override
    {
        s.setProperty ("colour", current.toString(), nullptr);

        juce::StringArray h;
        for (auto c : history) h.add (c.toString());
        s.setProperty ("history", h.joinIntoString (","), nullptr);

        s.setProperty (kPickFormat, format, nullptr);
        s.setProperty (kPickAutoCopy, autoCopy, nullptr);
    }

    void restore (const juce::ValueTree& s) override
    {
        if (s.hasProperty ("colour"))
            current = juce::Colour::fromString (s.getProperty ("colour").toString()).withAlpha (1.0f);

        if (s.hasProperty ("history"))
        {
            history.clear();
            for (auto& t : juce::StringArray::fromTokens (s.getProperty ("history").toString(), ",", {}))
                if (t.isNotEmpty())
                    history.add (juce::Colour::fromString (t).withAlpha (1.0f));
        }

        if (s.hasProperty (kPickFormat))   format   = juce::jlimit (0, 2, (int) s.getProperty (kPickFormat));
        if (s.hasProperty (kPickAutoCopy)) autoCopy = (bool) s.getProperty (kPickAutoCopy);
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();

        historyRow = {};
        if (r.getHeight() >= 110)
        {
            historyRow = r.removeFromBottom (juce::jlimit (14, 22, r.getHeight() / 8));
            r.removeFromBottom (4);
        }

        auto buttons = r.removeFromBottom (juce::jlimit (18, 26, r.getHeight() / 5));
        r.removeFromBottom (4);

        const int gap = 3;
        const int pickW = buttons.getWidth() * 2 / 5;
        pickBtn.setBounds (buttons.removeFromLeft (pickW));
        buttons.removeFromLeft (gap);
        const int half = (buttons.getWidth() - gap) / 2;
        copyBtn.setBounds (buttons.removeFromLeft (half));
        buttons.removeFromLeft (gap);
        editBtn.setBounds (buttons);

        const int sw = juce::jmin (r.getHeight(), (int) ((float) r.getWidth() * 0.42f));
        swatchRect = r.removeFromLeft (sw);
        r.removeFromLeft (6);
        infoRect = r;
    }

    void setBackdropChromeHidden (bool hidden) override
    {
        if (chromeHidden == hidden)
            return;
        chromeHidden = hidden;
        const float a = hidden ? 0.0f : 1.0f;
        pickBtn.setAlpha (a);
        copyBtn.setAlpha (a);
        editBtn.setAlpha (a);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (chromeHidden)
            return;

        const auto& tokens   = PinkXP::getSkinTokens();
        const bool chromaKey = tokens.isChromaKey;

        const bool roundedSwatch = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                                    || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);

        // ---- 色块 / 放大镜 ----
        if (picking && loupeValid)
        {
            PinkXP::Craft::drawPanelWell (g, swatchRect, PinkXP::content, displayWellKind());
            auto inner = swatchRect.reduced (2);
            const int n = kLoupeSize;
            const float cw = (float) inner.getWidth() / (float) n;
            const float ch = (float) inner.getHeight() / (float) n;

            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    const auto& p = loupe[(size_t) (y * n + x)];
                    g.setColour (juce::Colour (p.r, p.g, p.b));
                    g.fillRect (juce::Rectangle<float> ((float) inner.getX() + (float) x * cw,
                                                        (float) inner.getY() + (float) y * ch,
                                                        cw + 0.5f, ch + 0.5f));
                }

            const auto centre = juce::Rectangle<float> ((float) inner.getX() + (float) (n / 2) * cw,
                                                        (float) inner.getY() + (float) (n / 2) * ch, cw, ch);
            g.setColour (juce::Colours::black);
            g.drawRect (centre.expanded (1.0f), 1.0f);
            g.setColour (juce::Colours::white);
            g.drawRect (centre, 1.0f);
        }
        else if (chromaKey)
        {
            g.setColour (current);
            g.fillRect (swatchRect);
        }
        else if (roundedSwatch)
        {
            const auto rf  = swatchRect.toFloat();
            const float rad = juce::jmin (tokens.cardRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.25f);
            g.setColour (current);
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::skinHairlineOn (current, 0.24f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
        }
        else
        {
            PinkXP::Craft::drawPanelWell (g, swatchRect, PinkXP::content, PinkXP::WellKind::recessed);
            g.setColour (current);
            g.fillRect (swatchRect.reduced (2));
        }

        // ---- 文本读数（点击任意一行复制该格式）----
        const auto shown = shownColour();
        const juce::String values[3] = { hexText (shown), rgbText (shown, false), hslText (shown, false) };
        const char* labels[3] = { "HEX", "RGB", "HSL" };

        auto info = infoRect;
        const int lineH = juce::jlimit (10, 24, info.getHeight() / 3);
        const int top = info.getY() + juce::jmax (0, (info.getHeight() - lineH * 3) / 2);

        for (int i = 0; i < 3; ++i)
        {
            lineRects[i] = { info.getX(), top + i * lineH, info.getWidth(), lineH };
            auto line = lineRects[i];

            if (i == format && ! chromaKey)
            {
                if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                    || tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                {
                    g.setColour (PinkXP::pink500.withAlpha (tokens.frameStyle == PinkXP::FrameStyle::neonTube ? 0.18f : 0.14f));
                    g.fillRect (line);
                    g.setColour (PinkXP::sel);
                    g.fillRect (line.getX(), line.getY() + 1, 2, juce::jmax (1, line.getHeight() - 2));
                }
                else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.20f));
                    g.fillRect (line);
                    g.setColour (PinkXP::sel.withAlpha (0.58f));
                    g.drawRect (line, 1);
                }
                else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                {
                    const float rad = juce::jmin (4.0f, (float) line.getHeight() * 0.35f);
                    g.setColour (PinkXP::pink500.withAlpha (0.16f));
                    g.fillRoundedRectangle (line.toFloat(), rad);
                    g.setColour (juce::Colours::white.withAlpha (0.30f));
                    g.drawRoundedRectangle (line.toFloat().reduced (0.5f), juce::jmax (0.5f, rad - 0.5f), 0.8f);
                }
                else
                {
                    g.setColour (PinkXP::pink500.withAlpha (0.14f));
                    const float rad = juce::jmin (tokens.badgeRadius, (float) line.getHeight() * 0.35f);
                    if (rad > 0.5f) g.fillRoundedRectangle (line.toFloat(), rad);
                    else            g.fillRect (line);
                }
            }

            const int labelW = juce::jlimit (22, 40, line.getWidth() / 4);
            g.setColour (PinkXP::ink.withAlpha (0.55f));
            drawFitted (g, labels[i], line.removeFromLeft (labelW).reduced (2, 0), juce::Justification::centredLeft,
                        (float) lineH * 0.6f, 6.0f, juce::Font::bold);

            g.setColour (PinkXP::ink);
            drawFitted (g, values[i], line.reduced (2, 0), juce::Justification::centredLeft,
                        (float) lineH * 0.75f, 6.0f, juce::Font::bold);
        }

        if (juce::Time::getMillisecondCounter() < copiedUntil && copiedLine >= 0)
        {
            g.setColour (PinkXP::pink500);
            drawFitted (g, y2k::tr ("COPIED"), lineRects[copiedLine].reduced (2, 0),
                        juce::Justification::centredRight, (float) lineH * 0.6f, 6.0f, juce::Font::bold);
        }

        // ---- 历史色 ----
        historyRects.clear();
        if (! historyRow.isEmpty())
        {
            const int s = historyRow.getHeight();
            int x = historyRow.getX();

            if (history.isEmpty())
            {
                g.setColour (PinkXP::ink.withAlpha (0.35f));
                drawFitted (g, y2k::tr ("Picked colours appear here"), historyRow,
                            juce::Justification::centredLeft, (float) s * 0.6f);
            }

            for (int i = 0; i < history.size() && x + s <= historyRow.getRight(); ++i)
            {
                const juce::Rectangle<int> sq (x, historyRow.getY(), s, s);
                if (chromaKey)
                {
                    g.setColour (history[i]);
                    g.fillRect (sq);
                }
                else if (roundedSwatch)
                {
                    const auto rf   = sq.toFloat();
                    const float rad = juce::jmin (3.5f, (float) s * 0.25f);
                    g.setColour (history[i]);
                    g.fillRoundedRectangle (rf, rad);
                    g.setColour (PinkXP::skinHairlineOn (history[i], 0.25f));
                    g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                }
                else
                {
                    PinkXP::Craft::drawPanelWell (g, sq, PinkXP::content, PinkXP::WellKind::recessed);
                    g.setColour (history[i]);
                    g.fillRect (s >= 6 ? sq.reduced (2) : sq.reduced (1));
                }
                historyRects.add (sq);
                x += s + 3;
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = e.getPosition();

        for (int i = 0; i < 3; ++i)
            if (lineRects[i].contains (p))
            {
                copyFormat (i);
                return;
            }

        for (int i = 0; i < historyRects.size(); ++i)
            if (historyRects[i].contains (p))
            {
                if (e.mods.isPopupMenu() || e.mods.isShiftDown())
                    history.remove (i);
                else
                    current = history[i];
                repaint();
                notifyChanged();
                return;
            }

        if (swatchRect.contains (p) && e.getNumberOfClicks() >= 2)
            openEditor();
    }

private:
    static constexpr int kLoupeRadius = 5;
    static constexpr int kLoupeSize   = kLoupeRadius * 2 + 1;

    juce::Colour shownColour() const
    {
        if (picking && loupeValid)
        {
            const auto& p = loupe[(size_t) (kLoupeRadius * kLoupeSize + kLoupeRadius)];
            return juce::Colour (p.r, p.g, p.b);
        }
        return current;
    }

    static juce::String hexText (juce::Colour c) { return "#" + c.toDisplayString (false); }

    static juce::String rgbText (juce::Colour c, bool css)
    {
        const auto body = juce::String (c.getRed()) + ", " + juce::String (c.getGreen()) + ", " + juce::String (c.getBlue());
        return css ? "rgb(" + body + ")" : body;
    }

    static juce::String hslText (juce::Colour c, bool css)
    {
        const auto body = juce::String (juce::roundToInt (c.getHue() * 360.0f) % 360) + ", "
                        + juce::String (juce::roundToInt (c.getSaturationHSL() * 100.0f)) + "%, "
                        + juce::String (juce::roundToInt (c.getLightness() * 100.0f)) + "%";
        return css ? "hsl(" + body + ")" : body;
    }

    juce::String formatText (int f, juce::Colour c) const
    {
        return f == 1 ? rgbText (c, true) : f == 2 ? hslText (c, true) : hexText (c);
    }

    void copyFormat (int f)
    {
        juce::SystemClipboard::copyTextToClipboard (formatText (f, current));
        copiedLine = f;
        copiedUntil = juce::Time::getMillisecondCounter() + 900;
        repaint();
        juce::Timer::callAfterDelay (950, [safe = juce::Component::SafePointer<ColourPickerView> (this)]
        {
            if (safe != nullptr) safe->repaint();
        });
    }

    void copyCurrent() { copyFormat (format); }

    void commitColour (juce::Colour c)
    {
        current = c.withAlpha (1.0f);

        for (int i = history.size(); --i >= 0;)
            if (history[i] == current)
                history.remove (i);

        history.insert (0, current);
        while (history.size() > 16)
            history.removeLast();

        if (autoCopy)
            copyCurrent();

        repaint();
        notifyChanged();
    }

    // ---- Windows：按住拖动实时取色 ----
    void beginLivePick()
    {
        picking = true;
        pickBtn.active = true;
        pickBtn.setMouseCursor (juce::MouseCursor::CrosshairCursor);
        sampleNow();
        startTimerHz (30);
        repaint();
    }

    void endLivePick()
    {
        if (! picking)
            return;

        sampleNow();
        picking = false;
        pickBtn.active = false;
        pickBtn.setMouseCursor (juce::MouseCursor::NormalCursor);

        if (loupeValid)
        {
            const auto& p = loupe[(size_t) (kLoupeRadius * kLoupeSize + kLoupeRadius)];
            commitColour (juce::Colour (p.r, p.g, p.b));
        }

        loupeValid = false;
        repaint();
    }

    void sampleNow()
    {
        loupeValid = y2k::screencolour::sampleAroundCursor (kLoupeRadius, loupe.data());
    }

    // ---- macOS：系统取色放大镜 ----
    void beginSystemPick()
    {
        juce::Component::SafePointer<ColourPickerView> safe (this);

        y2k::screencolour::pickWithSystemSampler ([safe] (bool ok, y2k::screencolour::Rgb c)
        {
            juce::MessageManager::callAsync ([safe, ok, c]
            {
                if (safe != nullptr && ok)
                    safe->commitColour (juce::Colour (c.r, c.g, c.b));
            });
        });
    }

    // ---- 手动微调（ColourSelector 弹出框）----
    void openEditor()
    {
        if (editorSel != nullptr)
            return;

        auto sel = std::make_unique<juce::ColourSelector> (juce::ColourSelector::showColourAtTop
                                                         | juce::ColourSelector::showSliders
                                                         | juce::ColourSelector::showColourspace);
        sel->setCurrentColour (current, juce::dontSendNotification);
        sel->setSize (260, 300);
        sel->addChangeListener (this);
        editorSel = sel.get();
        editStartColour = current;
        editing = true;

        juce::CallOutBox::launchAsynchronously (std::move (sel), localAreaToGlobal (swatchRect), nullptr);
        startTimerHz (5);   // 轮询弹出框关闭 → 把编辑结果记入历史
    }

    void changeListenerCallback (juce::ChangeBroadcaster* src) override
    {
        if (auto* s = dynamic_cast<juce::ColourSelector*> (src))
        {
            current = s->getCurrentColour().withAlpha (1.0f);
            repaint();
        }
    }

    void timerCallback() override
    {
        if (picking)
        {
            sampleNow();
            repaint (swatchRect);
            repaint (infoRect);
        }

        if (editing && editorSel == nullptr)
        {
            editing = false;
            if (current != editStartColour)
                commitColour (current);
        }

        if (! picking && editorSel == nullptr)
            stopTimer();
    }

    EyedropperButton pickBtn;
    juce::TextButton copyBtn { "COPY" }, editBtn { "EDIT" };

    juce::Colour current { 0xffec4d85 };
    juce::Array<juce::Colour> history;
    int  format = 0;
    bool autoCopy = false;

    bool picking = false;
    bool loupeValid = false;
    std::array<y2k::screencolour::Rgb, (size_t) (kLoupeSize * kLoupeSize)> loupe {};

    juce::Rectangle<int> swatchRect, infoRect, historyRow;
    juce::Rectangle<int> lineRects[3];
    juce::Array<juce::Rectangle<int>> historyRects;

    int copiedLine = -1;
    juce::uint32 copiedUntil = 0;

    juce::Component::SafePointer<juce::ColourSelector> editorSel;
    juce::Colour editStartColour;
    bool editing = false;
    bool chromeHidden = false;
};

// ==========================================================
// 秒表
// ==========================================================
class StopwatchView : public ToolView,
                      private juce::Timer
{
public:
    StopwatchView()
    {
        prepButton (startBtn);
        prepButton (lapBtn);
        startBtn.onClick = [this] { toggleRun(); };
        lapBtn.onClick   = [this] { lapOrReset(); };
        startBtn.setTooltip (y2k::help::make ("Start / Stop", "Start or pause the stopwatch. Space also works."));
        lapBtn.setTooltip (y2k::help::make ("Lap / Reset", "While running: record a lap. While stopped: reset to zero."));
        addAndMakeVisible (startBtn);
        addAndMakeVisible (lapBtn);
        setWantsKeyboardFocus (true);
        refreshButtons();
    }

    bool getParam (const juce::String& id, double& out) const override
    {
        if (id == kSwHundredths) { out = hundredths ? 1.0 : 0.0; return true; }
        return false;
    }

    bool setParam (const juce::String& id, double v) override
    {
        if (id != kSwHundredths) return false;
        hundredths = v >= 0.5;
        repaint();
        return true;
    }

    void save (juce::ValueTree& s) const override
    {
        s.setProperty ("running", running, nullptr);
        s.setProperty ("startEpoch", (juce::int64) startEpoch, nullptr);
        s.setProperty ("accum", accumulated, nullptr);

        juce::StringArray l;
        for (auto v : laps) l.add (juce::String ((juce::int64) v));
        s.setProperty ("laps", l.joinIntoString (","), nullptr);
        s.setProperty (kSwHundredths, hundredths, nullptr);
    }

    void restore (const juce::ValueTree& s) override
    {
        if (s.hasProperty ("running"))    running = (bool) s.getProperty ("running");
        if (s.hasProperty ("startEpoch")) startEpoch = (juce::int64) s.getProperty ("startEpoch");
        if (s.hasProperty ("accum"))      accumulated = (double) s.getProperty ("accum");
        if (s.hasProperty ("laps"))
        {
            laps.clear();
            for (auto& t : juce::StringArray::fromTokens (s.getProperty ("laps").toString(), ",", {}))
                if (t.isNotEmpty())
                    laps.push_back ((double) t.getLargeIntValue());
        }
        if (s.hasProperty (kSwHundredths)) hundredths = (bool) s.getProperty (kSwHundredths);

        if (running) startTimerHz (30);
        refreshButtons();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int displayH = juce::jlimit (28, 90, r.getHeight() / 2);
        displayRect = r.removeFromTop (displayH);
        r.removeFromTop (4);

        auto row = r.removeFromTop (juce::jlimit (18, 26, r.getHeight() / 3));
        const int half = (row.getWidth() - 3) / 2;
        startBtn.setBounds (row.removeFromLeft (half));
        row.removeFromLeft (3);
        lapBtn.setBounds (row);

        r.removeFromTop (4);
        lapsRect = r;
    }

    juce::Rectangle<int> getChromaKeyRect() const override { return displayRect; }

    void setBackdropChromeHidden (bool hidden) override
    {
        if (chromeHidden == hidden)
            return;
        chromeHidden = hidden;
        const float a = hidden ? 0.0f : 1.0f;
        startBtn.setAlpha (a);
        lapBtn.setAlpha (a);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        PinkXP::Craft::drawReadoutCard (g, displayRect, PinkXP::content);
        g.setColour (inkOnContent());
        drawFitted (g, formatDuration (elapsedMs(), hundredths), displayRect.reduced (6, 4),
                    juce::Justification::centred, (float) displayRect.getHeight() * 0.7f, 8.0f, juce::Font::bold);
        PinkXP::Craft::drawCanvasFinishOverlay (g, displayRect.reduced (2));

        if (chromeHidden || lapsRect.getHeight() < 10 || laps.empty())
            return;

        {
            const auto fs = PinkXP::getSkinTokens().frameStyle;
            if (fs == PinkXP::FrameStyle::machinedChrome
                || fs == PinkXP::FrameStyle::frostedGlass
                || fs == PinkXP::FrameStyle::neonTube
                || fs == PinkXP::FrameStyle::crtPhosphor)
            {
                drawInstrumentDivider (g, (float) lapsRect.getX(), (float) (lapsRect.getY() - 2),
                                       (float) lapsRect.getRight(), PinkXP::ink);
            }
        }

        const int lineH = juce::jlimit (10, 16, lapsRect.getHeight() / 5);
        int y = lapsRect.getY();

        for (int i = (int) laps.size() - 1; i >= 0 && y + lineH <= lapsRect.getBottom(); --i)
        {
            const double total = laps[(size_t) i];
            const double split = total - (i > 0 ? laps[(size_t) i - 1] : 0.0);
            juce::Rectangle<int> line (lapsRect.getX(), y, lapsRect.getWidth(), lineH);

            g.setColour (PinkXP::ink.withAlpha (0.5f));
            drawFitted (g, y2k::trf ("LAP {0}", i + 1), line.removeFromLeft (line.getWidth() / 4),
                        juce::Justification::centredLeft, (float) lineH * 0.75f);

            g.setColour (PinkXP::ink);
            drawFitted (g, "+" + formatDuration (split, true), line.removeFromLeft (line.getWidth() / 2),
                        juce::Justification::centredLeft, (float) lineH * 0.8f, 6.0f, juce::Font::bold);

            g.setColour (PinkXP::ink.withAlpha (0.6f));
            drawFitted (g, formatDuration (total, true), line, juce::Justification::centredRight, (float) lineH * 0.75f);

            y += lineH;
        }
    }

    void mouseDown (const juce::MouseEvent&) override { grabKeyboardFocus(); }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)  { toggleRun(); return true; }
        if (k.getTextCharacter() == 'l' || k.getTextCharacter() == 'L' || k == juce::KeyPress::returnKey)
        {
            lapOrReset();
            return true;
        }
        return false;
    }

private:
    double elapsedMs() const
    {
        return accumulated + (running ? (double) (nowMs() - startEpoch) : 0.0);
    }

    void toggleRun()
    {
        if (running)
        {
            accumulated = elapsedMs();
            running = false;
            stopTimer();
        }
        else
        {
            startEpoch = nowMs();
            running = true;
            startTimerHz (30);
        }
        refreshButtons();
        repaint();
        notifyChanged();
    }

    void lapOrReset()
    {
        if (running)
        {
            laps.push_back (elapsedMs());
            if (laps.size() > 99)
                laps.erase (laps.begin());
        }
        else
        {
            accumulated = 0.0;
            laps.clear();
        }
        refreshButtons();
        repaint();
        notifyChanged();
    }

    void refreshButtons()
    {
        startBtn.setButtonText (running ? "STOP" : "START");
        lapBtn.setButtonText (running ? "LAP" : "RESET");
    }

    void timerCallback() override
    {
        if (! running) { stopTimer(); return; }
        repaint (displayRect);
    }

    juce::TextButton startBtn, lapBtn;
    juce::Rectangle<int> displayRect, lapsRect;

    bool running = false;
    juce::int64 startEpoch = 0;
    double accumulated = 0.0;
    std::vector<double> laps;
    bool hundredths = true;
    bool chromeHidden = false;
};

// ==========================================================
// 倒计时
// ==========================================================
class CountdownView : public ToolView,
                      private juce::Timer
{
public:
    CountdownView()
    {
        static const int presets[] = { 1, 5, 10, 25 };
        for (int m : presets)
        {
            auto b = std::make_unique<juce::TextButton> (juce::String (m) + "M");
            prepButton (*b);
            b->setTooltip (y2k::help::make (juce::String (m) + "M", "Set the countdown to this many minutes."));
            b->onClick = [this, m] { setDuration (m * 60000.0); };
            addAndMakeVisible (*b);
            presetBtns.push_back (std::move (b));
        }

        prepButton (startBtn);
        prepButton (resetBtn);
        startBtn.onClick = [this] { running ? pause() : start(); };
        resetBtn.onClick = [this] { reset(); };
        startBtn.setTooltip (y2k::help::make ("Start / Pause", "Start or pause the countdown."));
        resetBtn.setTooltip (y2k::help::make ("Reset", "Back to the full duration."));
        addAndMakeVisible (startBtn);
        addAndMakeVisible (resetBtn);
        refreshButtons();
    }

    bool getParam (const juce::String& id, double& out) const override
    {
        if (id == kTmSound) { out = sound ? 1.0 : 0.0; return true; }
        if (id == kTmFlash) { out = flash ? 1.0 : 0.0; return true; }
        return false;
    }

    bool setParam (const juce::String& id, double v) override
    {
        if (id == kTmSound) { sound = v >= 0.5; return true; }
        if (id == kTmFlash) { flash = v >= 0.5; repaint(); return true; }
        return false;
    }

    void save (juce::ValueTree& s) const override
    {
        s.setProperty ("duration", durationMs, nullptr);
        s.setProperty ("running", running, nullptr);
        s.setProperty ("endEpoch", (juce::int64) endEpoch, nullptr);
        s.setProperty ("remaining", remainingMs, nullptr);
        s.setProperty (kTmSound, sound, nullptr);
        s.setProperty (kTmFlash, flash, nullptr);
    }

    void restore (const juce::ValueTree& s) override
    {
        if (s.hasProperty ("duration"))  durationMs  = juce::jlimit (5000.0, 86400000.0, (double) s.getProperty ("duration"));
        if (s.hasProperty ("remaining")) remainingMs = juce::jlimit (0.0, durationMs, (double) s.getProperty ("remaining"));
        if (s.hasProperty ("running"))   running     = (bool) s.getProperty ("running");
        if (s.hasProperty ("endEpoch"))  endEpoch    = (juce::int64) s.getProperty ("endEpoch");
        if (s.hasProperty (kTmSound))    sound       = (bool) s.getProperty (kTmSound);
        if (s.hasProperty (kTmFlash))    flash       = (bool) s.getProperty (kTmFlash);

        if (running)
        {
            if (remaining() <= 0.0)
            {
                // 软件关闭期间已经到点：直接显示"到点"，但不补播提示音
                running = false;
                finished = true;
                remainingMs = 0.0;
            }
            startTimerHz (20);
        }
        refreshButtons();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int gap = 3;

        const int rowH = juce::jlimit (16, 24, r.getHeight() / 6);
        auto bottom = r.removeFromBottom (rowH);
        r.removeFromBottom (gap);
        auto presetsRow = r.removeFromBottom (rowH);
        r.removeFromBottom (4);
        displayRect = r;

        const int half = (bottom.getWidth() - gap) / 2;
        startBtn.setBounds (bottom.removeFromLeft (half));
        bottom.removeFromLeft (gap);
        resetBtn.setBounds (bottom);

        const int n = (int) presetBtns.size();
        const int w = (presetsRow.getWidth() - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i)
        {
            const int x = presetsRow.getX() + i * (w + gap);
            presetBtns[(size_t) i]->setBounds (x, presetsRow.getY(), i == n - 1 ? presetsRow.getRight() - x : w, rowH);
        }
    }

    juce::Rectangle<int> getChromaKeyRect() const override { return displayRect; }

    void setBackdropChromeHidden (bool hidden) override
    {
        const float a = hidden ? 0.0f : 1.0f;
        for (auto& b : presetBtns)
            if (b != nullptr && std::abs (b->getAlpha() - a) > 0.001f)
                b->setAlpha (a);
        if (std::abs (startBtn.getAlpha() - a) > 0.001f) startBtn.setAlpha (a);
        if (std::abs (resetBtn.getAlpha() - a) > 0.001f) resetBtn.setAlpha (a);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& tokens = PinkXP::getSkinTokens();
        const bool flashOn = finished && flash && ((juce::Time::getMillisecondCounter() / 400) % 2 == 0);
        const auto bg = flashOn ? PinkXP::pink500 : PinkXP::content;
        PinkXP::Craft::drawReadoutCard (g, displayRect, bg);

        auto inner = displayRect.reduced (6, 4);
        const auto ink = tokens.isChromaKey ? inkOnContent() : PinkXP::readableInkOn (bg);

        // 进度条（剩余比例）：走 Craft::drawContinuousBar（Pixel XP / Chrome / Glass / Neon / CRT / Modern / 绿幕跳过底槽）
        const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
        const bool isGlass  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const bool isNeon   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
        const bool isCrt    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
        const int barH = (isChrome || isGlass || isNeon || isCrt)
                             ? juce::jlimit (4, 7, inner.getHeight() / 12)
                             : juce::jlimit (2, 5, inner.getHeight() / 14);
        auto bar = inner.removeFromBottom (barH);
        inner.removeFromBottom (2);
        const float frac = juce::jlimit (0.0f, 1.0f, durationMs > 0.0 ? (float) (remaining() / durationMs) : 0.0f);
        const auto barCol = finished ? PinkXP::danger : PinkXP::pink500;
        if (isChrome)
        {
            const bool isDark = bg.getPerceivedBrightness() < 0.45f;
            const bool useWell = (bar.getHeight() >= 6);
            if (! tokens.isChromaKey)
            {
                if (useWell)
                {
                    PinkXP::Craft::drawPanelWell (g, bar, bg.darker (0.06f), PinkXP::WellKind::trough);
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
            const float fillW = (float) innerBar.getWidth() * frac;
            if (fillW > 0.25f)
            {
                const auto fillR = innerBar.toFloat().withWidth (fillW);
                PinkXP::Craft::drawContinuousBar (g, innerBar.toFloat(), fillR, barCol, false, false);
                if (fillR.getHeight() >= 2.0f)
                {
                    g.setColour (barCol.interpolatedWith (juce::Colours::white, 0.52f).withAlpha (0.65f));
                    g.fillRect (fillR.getX(), fillR.getY(), fillR.getWidth(), 1.0f);
                }
            }
        }
        else if (isGlass)
        {
            const bool isDark = bg.getPerceivedBrightness() < 0.45f;
            const auto bf     = bar.toFloat();
            const float rad   = juce::jmin (3.5f, bf.getHeight() * 0.5f);
            if (! tokens.isChromaKey)
            {
                g.setColour (isDark ? juce::Colours::white.withAlpha (0.10f)
                                    : juce::Colours::black.withAlpha (0.09f));
                g.fillRoundedRectangle (bf, rad);
                g.setColour (juce::Colours::white.withAlpha (isDark ? 0.18f : 0.55f));
                g.drawRoundedRectangle (bf.reduced (0.5f), juce::jmax (0.5f, rad - 0.5f), 0.8f);
            }
            const float fillW = bf.getWidth() * frac;
            if (fillW > 0.25f)
                PinkXP::Craft::drawContinuousBar (g, bf, bf.withWidth (fillW), barCol, false, false);
        }
        else if (isNeon)
        {
            if (! tokens.isChromaKey)
            {
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                g.fillRect (bar);
                g.setColour (PinkXP::sel.withAlpha (0.38f));
                g.drawRect (bar, 1);
            }
            const auto innerBar = (! tokens.isChromaKey && bar.getHeight() >= 4) ? bar.reduced (1) : bar;
            const float fillW = (float) innerBar.getWidth() * frac;
            if (fillW > 0.25f)
                PinkXP::Craft::drawContinuousBar (g, innerBar.toFloat(), innerBar.toFloat().withWidth (fillW),
                                                  barCol, false, false);
        }
        else if (isCrt)
        {
            if (! tokens.isChromaKey)
            {
                g.setColour (PinkXP::sel.withAlpha (0.14f));
                g.fillRect (bar);
                g.setColour (PinkXP::sel.withAlpha (0.48f));
                g.drawRect (bar, 1);
            }
            const auto innerBar = (! tokens.isChromaKey && bar.getHeight() >= 4) ? bar.reduced (1) : bar;
            const float fillW = (float) innerBar.getWidth() * frac;
            if (fillW > 0.25f)
                PinkXP::Craft::drawContinuousBar (g, innerBar.toFloat(), innerBar.toFloat().withWidth (fillW),
                                                  barCol, false, false);
        }
        else
        {
            const bool drawBarTrough = (bar.getHeight() >= 6);
            if (! drawBarTrough && ! tokens.isChromaKey)
            {
                g.setColour (ink.withAlpha (0.12f));
                const float rad = juce::jmin (tokens.barCellRadius, (float) bar.getHeight() * 0.45f);
                if (rad > 0.5f) g.fillRoundedRectangle (bar.toFloat(), rad);
                else            g.fillRect (bar);
            }
            const float fillW = (float) bar.getWidth() * frac;
            if (fillW > 0.25f)
                PinkXP::Craft::drawContinuousBar (g, bar.toFloat(), bar.toFloat().withWidth (fillW),
                                                  barCol, false, drawBarTrough);
            else if (drawBarTrough)
                PinkXP::Craft::drawPanelWell (g, bar, PinkXP::content.darker (0.04f), PinkXP::WellKind::trough);
        }

        juce::Rectangle<int> labelRow;
        if (inner.getHeight() >= 40)
            labelRow = inner.removeFromTop (juce::jlimit (8, 12, inner.getHeight() / 6));

        g.setColour (ink.withAlpha (0.55f));
        drawFitted (g, finished ? y2k::tr ("TIME UP - click to dismiss")
                                : (running ? y2k::tr ("RUNNING") : y2k::tr ("Scroll here to adjust")),
                    labelRow, juce::Justification::centred, (float) labelRow.getHeight());

        g.setColour (finished ? (flashOn ? (tokens.isChromaKey ? PinkXP::pink500 : ink) : PinkXP::danger) : ink);
        drawFitted (g, formatDuration (std::ceil (remaining() / 1000.0) * 1000.0, false), inner,
                    juce::Justification::centred, (float) inner.getHeight() * 0.85f, 8.0f, juce::Font::bold);

        PinkXP::Craft::drawCanvasFinishOverlay (g, displayRect.reduced (2));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (finished && displayRect.contains (e.getPosition()))
            reset();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (running || ! displayRect.contains (e.getPosition()))
            return;

        const float dy = std::abs (w.deltaY) > 0.0f ? w.deltaY : w.deltaX;
        if (dy == 0.0f)
            return;

        // 左半边按分钟、右半边按 5 秒调整
        const double step = e.x < displayRect.getCentreX() ? 60000.0 : 5000.0;
        setDuration (durationMs + (dy > 0.0f ? step : -step));
    }

private:
    double remaining() const
    {
        return running ? juce::jmax (0.0, (double) (endEpoch - nowMs())) : remainingMs;
    }

    void setDuration (double ms)
    {
        if (running)
            return;

        durationMs = juce::jlimit (5000.0, 86400000.0, ms);
        remainingMs = durationMs;
        finished = false;
        stopTimer();
        refreshButtons();
        repaint();
        notifyChanged();
    }

    void start()
    {
        if (finished || remainingMs <= 0.0)
            remainingMs = durationMs;

        finished = false;
        endEpoch = nowMs() + (juce::int64) remainingMs;
        running = true;
        startTimerHz (20);
        refreshButtons();
        repaint();
        notifyChanged();
    }

    void pause()
    {
        remainingMs = remaining();
        running = false;
        stopTimer();
        refreshButtons();
        repaint();
        notifyChanged();
    }

    void reset()
    {
        running = false;
        finished = false;
        alertsLeft = 0;
        remainingMs = durationMs;
        stopTimer();
        refreshButtons();
        repaint();
        notifyChanged();
    }

    void refreshButtons()
    {
        startBtn.setButtonText (running ? "PAUSE" : "START");
    }

    void timerCallback() override
    {
        if (running && remaining() <= 0.0)
        {
            running = false;
            finished = true;
            remainingMs = 0.0;
            alertsLeft = sound ? 3 : 0;
            nextAlertMs = 0;
            refreshButtons();
            notifyChanged();
        }

        if (finished && alertsLeft > 0 && juce::Time::getMillisecondCounter() >= nextAlertMs)
        {
            --alertsLeft;
            nextAlertMs = juce::Time::getMillisecondCounter() + 700;
            getLookAndFeel().playAlertSound();
        }

        if (! running && ! finished)
        {
            stopTimer();
            return;
        }

        if (finished && ! flash && alertsLeft == 0)
            stopTimer();

        repaint (displayRect);
    }

    std::vector<std::unique_ptr<juce::TextButton>> presetBtns;
    juce::TextButton startBtn, resetBtn { "RESET" };
    juce::Rectangle<int> displayRect;

    double durationMs  = 5.0 * 60000.0;
    double remainingMs = 5.0 * 60000.0;
    juce::int64 endEpoch = 0;
    bool running  = false;
    bool finished = false;
    bool sound = true;
    bool flash = true;
    int  alertsLeft = 0;
    juce::uint32 nextAlertMs = 0;
};

// ==========================================================
// 音频换算：节拍 ↔ 毫秒 / 音名 ↔ 频率 / dB ↔ 倍数 / 采样 ↔ 毫秒
// ==========================================================
class ConverterView : public ToolView
{
public:
    ConverterView()
    {
        static const char* modeNames[] = { "TEMPO", "NOTE", "GAIN", "TIME" };
        static const char* modeTips[]  =
        {
            "BPM to delay / reverb times for each note length (straight, dotted, triplet). TAP to measure a tempo.",
            "Note name to frequency and back, with MIDI number, cents, period and wavelength.",
            "Decibels to amplitude / power ratio. Type a ratio as x0.5 to go the other way.",
            "Samples to milliseconds at common sample rates. Add ms or s to go the other way."
        };

        for (int i = 0; i < 4; ++i)
        {
            auto b = std::make_unique<juce::TextButton> (modeNames[i]);
            prepButton (*b);
            b->setClickingTogglesState (true);
            b->setRadioGroupId (0x5d17);
            b->setTooltip (y2k::help::make (modeNames[i], modeTips[i]));
            b->onClick = [this, i] { setMode (i); };
            addAndMakeVisible (*b);
            tabs.push_back (std::move (b));
        }

        input.setMultiLine (false);
        input.setSelectAllWhenFocused (true);
        input.onTextChange = [this] { inputs[(size_t) mode] = input.getText(); repaint(); notifyChanged(); };
        addAndMakeVisible (input);

        prepButton (tapBtn);
        tapBtn.setTooltip (y2k::help::make ("Tap", "Click along with the beat; the BPM is averaged over the last taps."));
        tapBtn.onClick = [this] { tap(); };
        addAndMakeVisible (tapBtn);

        inputs = { "120", "A4", "-6", "512" };
        setMode (0);
        themeChanged();
    }

    bool getParam (const juce::String& id, double& out) const override
    {
        if (id == kCvA4) { out = a4; return true; }
        return false;
    }

    bool setParam (const juce::String& id, double v) override
    {
        if (id != kCvA4) return false;
        a4 = juce::jlimit (400.0, 480.0, v);
        repaint();
        return true;
    }

    void save (juce::ValueTree& s) const override
    {
        s.setProperty ("mode", mode, nullptr);
        for (int i = 0; i < 4; ++i)
            s.setProperty ("in" + juce::String (i), inputs[(size_t) i], nullptr);
        s.setProperty (kCvA4, a4, nullptr);
    }

    void restore (const juce::ValueTree& s) override
    {
        for (int i = 0; i < 4; ++i)
            if (s.hasProperty ("in" + juce::String (i)))
                inputs[(size_t) i] = s.getProperty ("in" + juce::String (i)).toString();
        if (s.hasProperty (kCvA4)) a4 = juce::jlimit (400.0, 480.0, (double) s.getProperty (kCvA4));
        setMode (s.hasProperty ("mode") ? juce::jlimit (0, 3, (int) s.getProperty ("mode")) : 0);
    }

    void themeChanged() override
    {
        styleEditor (input, PinkXP::getAxisFont (13.0f, juce::Font::bold));
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int gap = 3;

        auto tabRow = r.removeFromTop (juce::jlimit (16, 22, r.getHeight() / 7));
        const int tw = (tabRow.getWidth() - gap * 3) / 4;
        for (int i = 0; i < 4; ++i)
        {
            const int x = tabRow.getX() + i * (tw + gap);
            tabs[(size_t) i]->setBounds (x, tabRow.getY(), i == 3 ? tabRow.getRight() - x : tw, tabRow.getHeight());
        }

        r.removeFromTop (4);
        auto inRow = r.removeFromTop (juce::jlimit (18, 24, r.getHeight() / 5));
        inputLabelRect = inRow.removeFromLeft (juce::jlimit (36, 80, inRow.getWidth() / 4));
        inRow.removeFromLeft (4);

        if (mode == 0)
        {
            tapBtn.setVisible (true);
            tapBtn.setBounds (inRow.removeFromRight (juce::jlimit (34, 56, inRow.getWidth() / 4)));
            inRow.removeFromRight (gap);
        }
        else
        {
            tapBtn.setVisible (false);
        }

        input.setBounds (inRow);
        r.removeFromTop (4);
        resultRect = r;
    }

    void setBackdropChromeHidden (bool hidden) override
    {
        if (chromeHidden == hidden)
            return;
        chromeHidden = hidden;
        const float a = hidden ? 0.0f : 1.0f;
        for (auto& t : tabs)
            if (t != nullptr) t->setAlpha (a);
        input.setAlpha (a);
        tapBtn.setAlpha (a);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (chromeHidden)
            return;

        static const char* inputLabels[] = { "BPM", "NOTE / Hz", "dB", "SAMPLES" };
        g.setColour (PinkXP::ink.withAlpha (0.65f));
        drawFitted (g, y2k::tr (inputLabels[mode]), inputLabelRect, juce::Justification::centredLeft,
                    11.0f, 6.0f, juce::Font::bold);

        if (resultRect.getHeight() < 14)
            return;

        PinkXP::Craft::drawPanelWell (g, resultRect, PinkXP::content, displayWellKind());
        auto area = resultRect.reduced (6, 4);

        std::vector<std::array<juce::String, 4>> rows;
        int columns = 2;
        buildRows (rows, columns);

        if (rows.empty())
        {
            g.setColour (inkOnContent().withAlpha (0.5f));
            drawFitted (g, y2k::tr ("Type a value above"), area, juce::Justification::centred, 12.0f);
            PinkXP::Craft::drawCanvasFinishOverlay (g, resultRect.reduced (2));
            return;
        }

        const int lineH = juce::jlimit (9, 20, area.getHeight() / (int) rows.size());
        const int shownRows = juce::jmin ((int) rows.size(), area.getHeight() / lineH);
        const auto ink = inkOnContent();

        for (int i = 0; i < shownRows; ++i)
        {
            juce::Rectangle<int> line (area.getX(), area.getY() + i * lineH, area.getWidth(), lineH);
            const auto& row = rows[(size_t) i];
            const bool header = row[0].startsWithChar ('#');

            if (columns == 2)
            {
                g.setColour (ink.withAlpha (0.55f));
                drawFitted (g, row[0], line.removeFromLeft (line.getWidth() * 2 / 5), juce::Justification::centredLeft,
                            (float) lineH * 0.7f);
                g.setColour (ink);
                drawFitted (g, row[1], line, juce::Justification::centredRight, (float) lineH * 0.8f, 6.0f, juce::Font::bold);
            }
            else
            {
                const int cw = line.getWidth() / columns;
                for (int c = 0; c < columns; ++c)
                {
                    auto cell = c == columns - 1 ? line : line.removeFromLeft (cw);
                    auto text = row[(size_t) c];
                    if (header && c == 0) text = text.substring (1);

                    g.setColour (header ? ink.withAlpha (0.5f) : (c == 0 ? ink.withAlpha (0.65f) : ink));
                    drawFitted (g, text, cell.reduced (1, 0),
                                c == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight,
                                (float) lineH * (header ? 0.65f : 0.8f), 6.0f,
                                (header || c == 0) ? juce::Font::plain : juce::Font::bold);
                }
                if (header)
                    drawInstrumentDivider (g, (float) area.getX(), (float) (area.getY() + (i + 1) * lineH - 1),
                                           (float) area.getRight(), ink);
            }
        }

        PinkXP::Craft::drawCanvasFinishOverlay (g, resultRect.reduced (2));
    }

private:
    void setMode (int m)
    {
        mode = juce::jlimit (0, 3, m);
        for (int i = 0; i < 4; ++i)
            tabs[(size_t) i]->setToggleState (i == mode, juce::dontSendNotification);
        input.setText (inputs[(size_t) mode], juce::dontSendNotification);
        resized();
        repaint();
    }

    void tap()
    {
        const double t = juce::Time::getMillisecondCounterHiRes();
        if (! taps.empty() && t - taps.back() > 2000.0)
            taps.clear();
        taps.push_back (t);
        while (taps.size() > 8)
            taps.erase (taps.begin());

        if (taps.size() >= 2)
        {
            const double avg = (taps.back() - taps.front()) / (double) (taps.size() - 1);
            if (avg > 0.0)
            {
                inputs[0] = juce::String (60000.0 / avg, 1);
                input.setText (inputs[0], juce::dontSendNotification);
                notifyChanged();
            }
        }
        repaint();
    }

    static juce::String ms (double v)
    {
        if (v >= 10000.0) return juce::String (v / 1000.0, 2) + " s";
        return juce::String (v, v < 10.0 ? 2 : 1) + " ms";
    }

    static bool parseNote (const juce::String& text, double& midiOut)
    {
        auto t = text.trim().toUpperCase().replace (juce::String::fromUTF8 ("\xe2\x99\xaf"), "#")
                                          .replace (juce::String::fromUTF8 ("\xe2\x99\xad"), "B");
        if (t.isEmpty())
            return false;

        static const int pcs[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        const auto letter = t[0];
        if (letter < 'A' || letter > 'G')
            return false;

        int pc = pcs[letter - 'A'];
        int i = 1;
        if (i < t.length() && t[i] == '#') { ++pc; ++i; }
        else if (i < t.length() && t[i] == 'B' && i + 1 < t.length()) { --pc; ++i; }   // "Bb4"：第二个 B 是降号

        const auto octStr = t.substring (i).trim();
        if (octStr.isEmpty() || ! octStr.containsOnly ("-0123456789"))
            return false;

        midiOut = 12.0 * (octStr.getIntValue() + 1) + pc;
        return true;
    }

    static juce::String noteName (int midi)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int pc = ((midi % 12) + 12) % 12;
        return juce::String (names[pc]) + juce::String ((int) std::floor (midi / 12.0) - 1);
    }

    void buildRows (std::vector<std::array<juce::String, 4>>& rows, int& columns) const
    {
        const auto text = inputs[(size_t) mode].trim();

        switch (mode)
        {
            case 0:   // TEMPO
            {
                const double bpm = text.getDoubleValue();
                if (bpm < 1.0 || bpm > 999.0)
                    return;

                columns = 4;
                rows.push_back ({ "#" + y2k::tr ("NOTE"), y2k::tr ("STRAIGHT"), y2k::tr ("DOTTED"), y2k::tr ("TRIPLET") });

                static const char* names[] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
                const double whole = 240000.0 / bpm;
                for (int i = 0; i < 6; ++i)
                {
                    const double v = whole / std::pow (2.0, i);
                    rows.push_back ({ names[i], ms (v), ms (v * 1.5), ms (v * 2.0 / 3.0) });
                }
                rows.push_back ({ "1/4 Hz", juce::String (bpm / 60.0, 3), "", "" });
                break;
            }

            case 1:   // NOTE
            {
                double midi = 0.0, hz = 0.0;
                if (parseNote (text, midi))
                {
                    hz = a4 * std::pow (2.0, (midi - 69.0) / 12.0);
                }
                else
                {
                    hz = text.upToFirstOccurrenceOf ("hz", false, true).trim().getDoubleValue();
                    if (hz < 1.0 || hz > 100000.0)
                        return;
                    midi = 69.0 + 12.0 * std::log2 (hz / a4);
                }

                const int nearest = (int) std::lround (midi);
                const int cents = (int) std::lround ((midi - nearest) * 100.0);
                const juce::String centsText = (cents >= 0 ? "+" : "") + juce::String (cents) + " c";

                rows.push_back ({ y2k::tr ("Note"), noteName (nearest) + "  " + centsText, "", "" });
                rows.push_back ({ y2k::tr ("Frequency"), juce::String (hz, hz < 100.0 ? 3 : 2) + " Hz", "", "" });
                rows.push_back ({ y2k::tr ("MIDI"), std::abs (midi - nearest) < 0.005 ? juce::String (nearest)
                                                                                      : juce::String (midi, 2), "", "" });
                rows.push_back ({ y2k::tr ("Period"), ms (1000.0 / hz), "", "" });
                rows.push_back ({ y2k::tr ("Wavelength"), juce::String (343.0 / hz, 3) + " m", "", "" });
                break;
            }

            case 2:   // GAIN
            {
                double db = 0.0;
                auto t = text.toLowerCase().removeCharacters (" ");

                if (t.startsWithChar ('x') || t.endsWithChar ('x'))
                {
                    const double ratio = t.removeCharacters ("x").getDoubleValue();
                    if (ratio <= 0.0)
                        return;
                    db = 20.0 * std::log10 (ratio);
                }
                else
                {
                    if (t.isEmpty() || ! t.removeCharacters ("db").containsAnyOf ("0123456789"))
                        return;
                    db = t.removeCharacters ("db").getDoubleValue();
                }

                const double amp = std::pow (10.0, db / 20.0);
                rows.push_back ({ y2k::tr ("Level"), juce::String (db, 2) + " dB", "", "" });
                rows.push_back ({ y2k::tr ("Amplitude"), "x" + trimNumber (amp, amp < 0.01 ? 6 : 4), "", "" });
                rows.push_back ({ y2k::tr ("Power"), "x" + trimNumber (amp * amp, amp * amp < 0.01 ? 6 : 4), "", "" });
                rows.push_back ({ y2k::tr ("Percent"), trimNumber (amp * 100.0, 2) + " %", "", "" });
                break;
            }

            case 3:   // TIME
            {
                auto t = text.toLowerCase().removeCharacters (" ");
                if (t.isEmpty())
                    return;

                const bool fromMs  = t.endsWith ("ms");
                const bool fromSec = ! fromMs && t.endsWithChar ('s') && ! t.endsWith ("smp");
                const double v = t.retainCharacters ("0123456789.").getDoubleValue();
                if (v <= 0.0)
                    return;

                static const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
                for (double sr : rates)
                {
                    const auto label = trimNumber (sr / 1000.0, 1) + " kHz";
                    if (fromMs || fromSec)
                    {
                        const double msVal = fromSec ? v * 1000.0 : v;
                        rows.push_back ({ label, trimNumber (msVal * sr / 1000.0, 1) + " smp", "", "" });
                    }
                    else
                    {
                        rows.push_back ({ label, ms (v / sr * 1000.0), "", "" });
                    }
                }
                break;
            }

            default: break;
        }
    }

    std::vector<std::unique_ptr<juce::TextButton>> tabs;
    juce::TextEditor input;
    juce::TextButton tapBtn { "TAP" };
    juce::Rectangle<int> inputLabelRect, resultRect;

    int mode = 0;
    std::array<juce::String, 4> inputs;
    double a4 = 440.0;
    std::vector<double> taps;
    bool chromeHidden = false;
};

// ==========================================================
// 时钟
// ==========================================================
class ClockView : public ToolView,
                  private juce::Timer
{
public:
    ClockView() { startTimer (200); }
    ~ClockView() override { stopTimer(); }

    bool getParam (const juce::String& id, double& out) const override
    {
        if (id == kClkStyle)   { out = analogue ? 1.0 : 0.0; return true; }
        if (id == kClk24h)     { out = h24 ? 1.0 : 0.0; return true; }
        if (id == kClkSeconds) { out = seconds ? 1.0 : 0.0; return true; }
        if (id == kClkDate)    { out = showDate ? 1.0 : 0.0; return true; }
        return false;
    }

    bool setParam (const juce::String& id, double v) override
    {
        const bool on = v >= 0.5;
        if      (id == kClkStyle)   analogue = juce::roundToInt (v) == 1;
        else if (id == kClk24h)     h24 = on;
        else if (id == kClkSeconds) seconds = on;
        else if (id == kClkDate)    showDate = on;
        else return false;
        repaint();
        return true;
    }

    void save (juce::ValueTree& s) const override
    {
        s.setProperty (kClkStyle, analogue ? 1 : 0, nullptr);
        s.setProperty (kClk24h, h24, nullptr);
        s.setProperty (kClkSeconds, seconds, nullptr);
        s.setProperty (kClkDate, showDate, nullptr);
    }

    void restore (const juce::ValueTree& s) override
    {
        if (s.hasProperty (kClkStyle))   analogue = (int) s.getProperty (kClkStyle) == 1;
        if (s.hasProperty (kClk24h))     h24      = (bool) s.getProperty (kClk24h);
        if (s.hasProperty (kClkSeconds)) seconds  = (bool) s.getProperty (kClkSeconds);
        if (s.hasProperty (kClkDate))    showDate = (bool) s.getProperty (kClkDate);
        repaint();
    }

    juce::Rectangle<int> getChromaKeyRect() const override { return getLocalBounds(); }

    void paint (juce::Graphics& g) override
    {
        const auto now = juce::Time::getCurrentTime();
        const auto fullBounds = getLocalBounds();
        auto r = fullBounds;
        PinkXP::Craft::drawPanelWell (g, r, PinkXP::content, displayWellKind());
        r = r.reduced (6, 4);
        const auto ink = inkOnContent();

        juce::Rectangle<int> dateRow;
        if (showDate && r.getHeight() >= 40)
            dateRow = r.removeFromBottom (juce::jlimit (9, 16, r.getHeight() / 6));

        if (analogue) paintAnalogue (g, r, now, ink);
        else          paintDigital (g, r, now, ink);

        if (! dateRow.isEmpty())
        {
            static const char* days[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
            const auto text = juce::String (now.getYear()) + "-" + two (now.getMonth() + 1) + "-" + two (now.getDayOfMonth())
                            + "  " + y2k::tr (days[now.getDayOfWeek()]);
            g.setColour (ink.withAlpha (0.6f));
            drawFitted (g, text, dateRow, juce::Justification::centred, (float) dateRow.getHeight());
        }

        PinkXP::Craft::drawCanvasFinishOverlay (g, fullBounds.reduced (2));
    }

private:
    void paintDigital (juce::Graphics& g, juce::Rectangle<int> r, const juce::Time& now, juce::Colour ink)
    {
        int hour = now.getHours();
        juce::String suffix;
        if (! h24)
        {
            suffix = hour >= 12 ? "PM" : "AM";
            hour = hour % 12;
            if (hour == 0) hour = 12;
        }

        const bool blink = (now.getMilliseconds() < 500) || seconds;
        const auto mainText = (h24 ? two (hour) : juce::String (hour)) + (blink ? ":" : " ") + two (now.getMinutes());

        // 注意：不要用 small 作变量名（Windows rpcndr.h 把它 #define 成 char）
        juce::String smallText;
        if (seconds) smallText = two (now.getSeconds());
        if (suffix.isNotEmpty()) smallText = smallText.isEmpty() ? suffix : smallText + " " + suffix;

        auto smallArea = smallText.isNotEmpty() ? r.removeFromRight (r.getWidth() / 4) : juce::Rectangle<int>();

        g.setColour (ink);
        drawFitted (g, mainText, r, juce::Justification::centredRight, (float) r.getHeight() * 0.8f, 8.0f, juce::Font::bold);

        if (! smallArea.isEmpty())
        {
            g.setColour (PinkXP::pink500);
            drawFitted (g, smallText, smallArea.reduced (3, 0), juce::Justification::centredLeft,
                        (float) r.getHeight() * 0.35f, 6.0f, juce::Font::bold);
        }
    }

    void paintAnalogue (juce::Graphics& g, juce::Rectangle<int> r, const juce::Time& now, juce::Colour ink)
    {
        const float d = (float) juce::jmin (r.getWidth(), r.getHeight());
        if (d < 20.0f)
            return;

        const auto& tokens = PinkXP::getSkinTokens();
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        const float sec  = (float) now.getSeconds();
        const float min  = (float) now.getMinutes() + sec / 60.0f;
        const float hr   = (float) (now.getHours() % 12) + min / 60.0f;
        const float hrA  = twoPi * (hr / 12.0f);
        const float minA = twoPi * (min / 60.0f);
        const float secA = twoPi * (sec / 60.0f);

        const auto  c    = r.toFloat().getCentre();
        const float rad  = d * 0.5f - 2.0f;
        const float unit = juce::jmax (1.5f, d / 44.0f);
        const auto  dialRect = juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (c);

        const bool isPixelXP   = (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel);
        const bool isChrome    = (tokens.dialFinish == PinkXP::DialFinish::brushedMetal);
        const bool isGlass     = (tokens.dialFinish == PinkXP::DialFinish::glassDome);
        const bool isNeon      = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
        const bool isCrt       = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
        const bool isModern    = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
        const bool isDarkDial  = PinkXP::content.getPerceivedBrightness() < 0.45f;
        const bool greenScreen = PinkXP::isGreenScreenBackdrop();
        const float chromeBezelW = isChrome ? juce::jlimit (2.4f, 5.2f, rad * 0.105f) : 0.0f;
        const float glassBezelW  = isGlass  ? juce::jlimit (2.4f, 4.8f, rad * 0.095f) : 0.0f;
        const float ringInset    = isChrome ? chromeBezelW : (isGlass ? glassBezelW : (isNeon ? 3.5f : 0.0f));

        // ---- 1. 表盘外圈（全风格统一平滑抗锯齿圆环，完整继承当前视觉风格；透明模式内盘严格保持 alpha=0）----
        if (! greenScreen)
        {
            if (isGlass)
            {
                // Frutiger Aero 磨砂晶莹表圈 + 上 40% 球面玻璃穹顶反光（透明模式仅描晶环，中心 alpha=0）
                g.setColour (juce::Colours::black.withAlpha (isDarkDial ? 0.30f : 0.13f));
                g.drawEllipse (dialRect.reduced (glassBezelW * 0.45f).translated (0.0f, 1.5f),
                               glassBezelW * 0.90f);

                juce::ColourGradient ringGrad (juce::Colours::white.withAlpha (isDarkDial ? 0.28f : 0.78f),
                                               c.x, dialRect.getY(),
                                               PinkXP::face.interpolatedWith (juce::Colours::white, isDarkDial ? 0.10f : 0.22f)
                                                           .withAlpha (isDarkDial ? 0.36f : 0.54f),
                                               c.x, dialRect.getBottom(), false);
                const auto faceRect = dialRect.reduced (glassBezelW);
                if (! tokens.isChromaKey)
                {
                    g.setGradientFill (ringGrad);
                    g.fillEllipse (dialRect);

                    juce::ColourGradient faceGrad (PinkXP::content.interpolatedWith (juce::Colours::white, isDarkDial ? 0.05f : 0.14f),
                                                   c.x, faceRect.getY(),
                                                   PinkXP::content.interpolatedWith (juce::Colours::black, isDarkDial ? 0.06f : 0.02f),
                                                   c.x, faceRect.getBottom(), false);
                    g.setGradientFill (faceGrad);
                    g.fillEllipse (faceRect);

                    // 上 40% 弯曲玻璃穹顶镜面高光
                    {
                        juce::Graphics::ScopedSaveState ss (g);
                        juce::Path clipCircle;
                        clipCircle.addEllipse (faceRect);
                        g.reduceClipRegion (clipCircle);
                        const auto domeBox = faceRect.withHeight (faceRect.getHeight() * 0.44f)
                                                     .expanded (faceRect.getWidth() * 0.12f, 0.0f);
                        juce::ColourGradient dome (juce::Colours::white.withAlpha (isDarkDial ? 0.22f : 0.42f),
                                                   c.x, faceRect.getY(),
                                                   juce::Colours::white.withAlpha (0.0f),
                                                   c.x, faceRect.getY() + faceRect.getHeight() * 0.44f, false);
                        g.setGradientFill (dome);
                        g.fillEllipse (domeBox);
                    }
                }
                else
                {
                    g.setGradientFill (ringGrad);
                    g.drawEllipse (dialRect.reduced (glassBezelW * 0.5f), glassBezelW);
                }

                g.setColour (PinkXP::dark.withAlpha (0.30f));
                g.drawEllipse (dialRect.reduced (0.5f), 1.0f);
                g.setColour (juce::Colours::white.withAlpha (isDarkDial ? 0.45f : 0.88f));
                g.drawEllipse (dialRect.reduced (1.4f), 0.9f);
                g.setColour (PinkXP::dark.withAlpha (isDarkDial ? 0.34f : 0.18f));
                g.drawEllipse (faceRect, 0.85f);
            }
            else if (isNeon)
            {
                // Neon Cyber 同心双发光霓虹表环 + 暗烟熏盘底（透明模式盘底不填，保持 alpha=0）
                if (! tokens.isChromaKey)
                {
                    g.setColour (PinkXP::content.interpolatedWith (juce::Colours::black, isDarkDial ? 0.22f : 0.10f));
                    g.fillEllipse (dialRect.reduced (1.0f));
                }
                g.setColour (PinkXP::sel.withAlpha (0.26f));
                g.drawEllipse (dialRect.reduced (0.8f), 3.8f);
                g.setColour (PinkXP::sel.withAlpha (0.86f));
                g.drawEllipse (dialRect.reduced (0.8f), 1.45f);
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.82f));
                g.drawEllipse (dialRect.reduced (0.8f), 0.7f);

                if (rad >= 20.0f)
                {
                    g.setColour (PinkXP::accent2.withAlpha (0.48f));
                    g.drawEllipse (dialRect.reduced (4.0f), 0.85f);
                }
            }
            else if (isCrt)
            {
                // Tektronix CRT 矢量示波器磷光表盘：平滑磷光外环 + 内同心微刻度环 + 暗十字准星
                g.setColour (PinkXP::sel.withAlpha (0.22f));
                g.drawEllipse (dialRect.reduced (0.6f), 3.2f);
                g.setColour (PinkXP::sel.withAlpha (0.86f));
                g.drawEllipse (dialRect.reduced (0.6f), 1.25f);

                if (! tokens.isChromaKey && rad >= 24.0f)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.18f));
                    g.drawEllipse (dialRect.reduced (rad * 0.24f), 0.7f);
                    g.setColour (PinkXP::sel.withAlpha (0.13f));
                    g.drawLine (c.x - rad * 0.78f, c.y, c.x + rad * 0.78f, c.y, 0.7f);
                    g.drawLine (c.x, c.y - rad * 0.78f, c.x, c.y + rad * 0.78f, 0.7f);
                }
            }
            else if (isChrome)
            {
                // CNC 拉丝金属时钟外圈（金属对角拉丝渐变环 + 镜面倒角 + 内凹表盘沿；透明模式下内盘严格保持 alpha=0）
                g.setColour (juce::Colours::black.withAlpha (isDarkDial ? 0.36f : 0.16f));
                g.drawEllipse (dialRect.reduced (chromeBezelW * 0.45f).translated (0.0f, 1.4f),
                               chromeBezelW * 0.92f);

                const auto hiChrome  = PinkXP::hl.interpolatedWith (juce::Colours::white, isDarkDial ? 0.56f : 0.84f);
                const auto midChrome = PinkXP::face.interpolatedWith (PinkXP::shdw, isDarkDial ? 0.16f : 0.24f);
                const auto loChrome  = PinkXP::shdw.interpolatedWith (PinkXP::dark, isDarkDial ? 0.50f : 0.38f);
                juce::ColourGradient bezelGrad (hiChrome, dialRect.getX(), dialRect.getY(),
                                                loChrome, dialRect.getRight(), dialRect.getBottom(), false);
                bezelGrad.addColour (0.38, midChrome.interpolatedWith (juce::Colours::white, isDarkDial ? 0.16f : 0.34f));
                bezelGrad.addColour (0.62, midChrome.interpolatedWith (juce::Colours::black, isDarkDial ? 0.14f : 0.08f));
                bezelGrad.addColour (0.92, PinkXP::hl.interpolatedWith (juce::Colours::white, 0.30f));

                const auto faceRect = dialRect.reduced (chromeBezelW);
                if (! tokens.isChromaKey)
                {
                    g.setGradientFill (bezelGrad);
                    g.fillEllipse (dialRect);

                    // 表盘内面：克制拉丝纵向金属渐变 + 内凹顶沿阴影
                    juce::ColourGradient faceGrad (PinkXP::content.interpolatedWith (juce::Colours::white, isDarkDial ? 0.04f : 0.10f),
                                                   c.x, faceRect.getY(),
                                                   PinkXP::content.interpolatedWith (juce::Colours::black, isDarkDial ? 0.07f : 0.04f),
                                                   c.x, faceRect.getBottom(), false);
                    g.setGradientFill (faceGrad);
                    g.fillEllipse (faceRect);

                    g.setColour (PinkXP::dark.withAlpha (isDarkDial ? 0.46f : 0.22f));
                    g.drawEllipse (faceRect.reduced (0.5f), 1.3f);
                }
                else
                {
                    // 透明叠加模式：仅描拉丝外环，表盘中心保持 alpha=0 透出桌面
                    g.setGradientFill (bezelGrad);
                    g.drawEllipse (dialRect.reduced (chromeBezelW * 0.5f), chromeBezelW);
                }

                // 外切暗边 + 镜面倒角高光 + 内壁切边
                g.setColour (PinkXP::dark.withAlpha (0.78f));
                g.drawEllipse (dialRect.reduced (0.5f), 1.05f);

                juce::ColourGradient bevelRim (juce::Colours::white.withAlpha (isDarkDial ? 0.50f : 0.85f),
                                               dialRect.getX(), dialRect.getY(),
                                               PinkXP::dark.withAlpha (isDarkDial ? 0.56f : 0.34f),
                                               dialRect.getRight(), dialRect.getBottom(), false);
                g.setGradientFill (bevelRim);
                g.drawEllipse (dialRect.reduced (1.45f), 0.95f);

                g.setColour (PinkXP::dark.withAlpha (0.56f));
                g.drawEllipse (faceRect, 0.95f);
            }
            else if (isModern)
            {
                g.setColour (tokens.isChromaKey ? ink.withAlpha (0.36f)
                                                : PinkXP::skinHairlineOn (PinkXP::content, 0.22f));
                g.drawEllipse (dialRect, 1.0f);
            }
            else
            {
                // Pixel XP：干净平滑的抗锯齿外圈（保留 0px Win95 框与方形小时刻度作像素标识）
                g.setColour (ink.withAlpha (0.42f));
                g.drawEllipse (dialRect, 1.25f);
            }
        }
        else
        {
            if (isChrome || isGlass)
            {
                const float ringW = juce::jmax (1.8f, ringInset * 0.68f);
                g.setColour (PinkXP::hl.interpolatedWith (ink, 0.35f));
                g.drawEllipse (dialRect.reduced (ringW * 0.5f), ringW);
                g.setColour (PinkXP::dark.withAlpha (0.72f));
                g.drawEllipse (dialRect.reduced (0.5f), 1.0f);
            }
            else
            {
                g.setColour ((isNeon || isCrt) ? PinkXP::sel : ink.withAlpha (0.55f));
                g.drawEllipse (dialRect, isModern ? 1.0f : 1.25f);
            }
        }

        // ---- 2. 刻度：Pixel XP 方形整点块；Chrome 刻蚀条；Glass 纤细圆头刻度；Neon 发光刻度；CRT Tektronix 标尺刻度 ----
        if (isPixelXP)
        {
            const int px = juce::jmax (2, juce::roundToInt (d / 42.0f));
            for (int i = 0; i < 12; ++i)
            {
                const float a = twoPi * (float) i / 12.0f;
                const bool cardinal = (i % 3 == 0);
                const int s = cardinal ? juce::jmax (3, px * 2) : juce::jmax (2, px);
                const float tickR = juce::jmax (4.0f, rad - (float) s * 0.85f - 2.0f);
                const int tx = juce::roundToInt (c.x + std::sin (a) * tickR);
                const int ty = juce::roundToInt (c.y - std::cos (a) * tickR);
                g.setColour (cardinal ? ink : ink.withAlpha (0.55f));
                g.fillRect (tx - s / 2, ty - s / 2, s, s);
            }
        }
        else
        {
            const float tickOuterR = (ringInset > 0.0f) ? (rad - ringInset - 1.8f) : (rad - 2.4f);

            // 60 分钟细微短刻度（Modern / Chrome / Glass / Neon / CRT；正常与透明模式完整继承）
            if (! greenScreen && ((isModern && rad >= 34.0f)
                                  || (isGlass && rad >= 30.0f)
                                  || ((isChrome || isNeon || isCrt) && rad >= 26.0f)))
            {
                const auto minCol = isCrt  ? PinkXP::sel.withAlpha (0.34f)
                                  : isNeon ? PinkXP::sel.withAlpha (0.28f)
                                  : isChrome ? ink.withAlpha (isDarkDial ? 0.28f : 0.22f)
                                             : ink.withAlpha (0.18f);
                g.setColour (minCol);
                const float outerR = tickOuterR;
                const float innerR = outerR - juce::jmax (1.8f, rad * ((isChrome || isCrt) ? 0.052f : 0.045f));
                for (int m = 0; m < 60; ++m)
                {
                    if (m % 5 == 0) continue;
                    const float a = twoPi * (float) m / 60.0f;
                    const float sx = std::sin (a), cyCos = std::cos (a);
                    g.drawLine (c.x + sx * innerR, c.y - cyCos * innerR,
                                c.x + sx * outerR, c.y - cyCos * outerR, 0.75f);
                }
            }

            for (int i = 0; i < 12; ++i)
            {
                const float a = twoPi * (float) i / 12.0f;
                const bool cardinal = (i % 3 == 0);
                const float outerR = tickOuterR;
                const float innerR = outerR - (cardinal ? juce::jmax (4.5f, rad * ((isChrome || isCrt) ? 0.15f : 0.14f))
                                                        : juce::jmax (2.5f, rad * ((isChrome || isCrt) ? 0.095f : 0.085f)));
                const float sx = std::sin (a), cyCos = std::cos (a);

                juce::Path tick;
                tick.startNewSubPath (c.x + sx * innerR, c.y - cyCos * innerR);
                tick.lineTo          (c.x + sx * outerR, c.y - cyCos * outerR);
                const float tickW = (isChrome || isNeon)
                                        ? (cardinal ? juce::jmax (1.8f, unit * 0.95f) : juce::jmax (1.1f, unit * 0.56f))
                                    : isCrt
                                        ? (cardinal ? juce::jmax (1.5f, unit * 0.80f) : juce::jmax (0.95f, unit * 0.50f))
                                    : (isModern || isGlass)
                                        ? (cardinal ? juce::jmax (1.4f, unit * 0.78f) : juce::jmax (0.9f, unit * 0.45f))
                                        : (cardinal ? juce::jmax (1.6f, unit * 0.90f) : juce::jmax (1.0f, unit * 0.55f));
                const auto cap = (isChrome || isNeon || isCrt) ? juce::PathStrokeType::butt : juce::PathStrokeType::rounded;

                if (isChrome && ! greenScreen)
                {
                    // 刻蚀金属反光沿（轻微偏移营造 CNC 切槽感）
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.52f)
                                           .withAlpha (isDarkDial ? 0.24f : 0.56f));
                    g.strokePath (tick, juce::PathStrokeType (tickW, juce::PathStrokeType::mitered, cap),
                                  juce::AffineTransform::translation (0.55f, 0.70f));
                }
                else if (isNeon && cardinal && ! greenScreen)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.34f));
                    g.strokePath (tick, juce::PathStrokeType (tickW * 2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }

                const auto tickCol = ((isNeon || isCrt) && cardinal)
                                         ? PinkXP::sel
                                         : (cardinal ? ink.withAlpha (isChrome ? 0.92f : 0.88f)
                                                     : ink.withAlpha ((isChrome || isNeon || isCrt) ? 0.62f : 0.42f));
                g.setColour (tickCol);
                g.strokePath (tick, juce::PathStrokeType (tickW, juce::PathStrokeType::curved, cap));

                if (isNeon && cardinal)
                {
                    g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.60f));
                    g.strokePath (tick, juce::PathStrokeType (juce::jmax (0.75f, tickW * 0.36f),
                                                              juce::PathStrokeType::mitered, cap));
                }
            }
        }

        // ---- 3. 时针 / 分针 / 秒针（全风格平滑抗锯齿指针）----
        auto drawTaperedHand = [&] (float angle, float len, float tail, float baseW, float tipW, juce::Colour col)
        {
            const float dx = std::sin (angle), dy = -std::cos (angle);
            const float nx = -dy, ny = dx;

            juce::Path p;
            p.startNewSubPath (c.x - dx * tail + nx * baseW * 0.5f, c.y - dy * tail + ny * baseW * 0.5f);
            p.lineTo          (c.x + dx * len  + nx * tipW  * 0.5f, c.y + dy * len  + ny * tipW  * 0.5f);
            p.lineTo          (c.x + dx * len  - nx * tipW  * 0.5f, c.y + dy * len  - ny * tipW  * 0.5f);
            p.lineTo          (c.x - dx * tail - nx * baseW * 0.5f, c.y - dy * tail - ny * baseW * 0.5f);
            p.closeSubPath();

            if (tokens.needleShadow && ! greenScreen)
            {
                g.setColour (juce::Colours::black.withAlpha (isChrome ? 0.24f : (isPixelXP ? 0.24f : 0.18f)));
                g.fillPath (p, juce::AffineTransform::translation (isChrome ? 1.15f : 1.0f,
                                                                   isPixelXP ? 1.0f : 1.55f));
            }
            if (tokens.needleGlow && ! greenScreen)
            {
                g.setColour ((isNeon ? PinkXP::sel : col).withAlpha (isNeon ? 0.34f : 0.26f));
                g.strokePath (p, juce::PathStrokeType (baseW * (isNeon ? 1.75f : 1.45f),
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            }
            g.setColour (col);
            g.fillPath (p);

            if (isChrome)
            {
                // 抛光钢指针中脊高光切面（半侧镜面反光）
                juce::Path facet;
                facet.startNewSubPath (c.x - dx * tail,                      c.y - dy * tail);
                facet.lineTo          (c.x + dx * len,                       c.y + dy * len);
                facet.lineTo          (c.x + dx * len  - nx * tipW  * 0.5f,  c.y + dy * len  - ny * tipW  * 0.5f);
                facet.lineTo          (c.x - dx * tail - nx * baseW * 0.5f,  c.y - dy * tail - ny * baseW * 0.5f);
                facet.closeSubPath();
                g.setColour (col.interpolatedWith (juce::Colours::white, isDarkDial ? 0.44f : 0.54f)
                                .withAlpha (greenScreen ? 0.50f : 0.58f));
                g.fillPath (facet);
            }
            else if (isNeon || isCrt)
            {
                // 霓虹 / 磷光指针高亮白芯中轴
                g.setColour (col.interpolatedWith (juce::Colours::white, isNeon ? 0.68f : 0.48f)
                                .withAlpha (0.88f));
                g.drawLine (c.x - dx * tail * 0.6f, c.y - dy * tail * 0.6f,
                            c.x + dx * len  * 0.94f, c.y + dy * len  * 0.94f,
                            juce::jmax (0.75f, tipW * 0.70f));
            }
            else if (isGlass && ! greenScreen)
            {
                // 玻璃指针纤细晶亮反光丝
                g.setColour (juce::Colours::white.withAlpha (0.42f));
                g.drawLine (c.x, c.y, c.x + dx * len * 0.86f, c.y + dy * len * 0.86f,
                            juce::jmax (0.7f, tipW * 0.55f));
            }
        };

        const float handReach = (ringInset > 0.0f) ? (rad - ringInset * 0.85f) : rad;
        if (isPixelXP)
        {
            // Pixel XP：干净抗锯齿直边指针 + 清晰微影（不锯齿、保持复古硬件利落感）
            drawTaperedHand (hrA,  rad * 0.48f, rad * 0.08f, juce::jmax (2.4f, unit * 1.40f), juce::jmax (1.8f, unit * 1.05f), ink);
            drawTaperedHand (minA, rad * 0.72f, rad * 0.10f, juce::jmax (1.8f, unit * 1.05f), juce::jmax (1.3f, unit * 0.75f), ink);
        }
        else if (isChrome)
        {
            drawTaperedHand (hrA,  handReach * 0.52f, handReach * 0.12f, juce::jmax (2.8f, unit * 1.65f), juce::jmax (1.0f, unit * 0.55f), ink);
            drawTaperedHand (minA, handReach * 0.76f, handReach * 0.14f, juce::jmax (2.1f, unit * 1.22f), juce::jmax (0.8f, unit * 0.42f), ink);
        }
        else
        {
            drawTaperedHand (hrA,  handReach * 0.50f, handReach * 0.10f, juce::jmax (2.5f, unit * 1.50f), juce::jmax (1.2f, unit * 0.65f), ink);
            drawTaperedHand (minA, handReach * 0.74f, handReach * 0.12f, juce::jmax (1.9f, unit * 1.10f), juce::jmax (0.9f, unit * 0.50f), ink);
        }

        if (seconds)
        {
            juce::Path secPath;
            secPath.startNewSubPath (c.x - std::sin (secA) * (handReach * 0.18f), c.y + std::cos (secA) * (handReach * 0.18f));
            secPath.lineTo          (c.x + std::sin (secA) * (handReach * 0.82f), c.y - std::cos (secA) * (handReach * 0.82f));
            if ((isChrome || isGlass) && ! greenScreen)
            {
                g.setColour (juce::Colours::black.withAlpha (0.20f));
                g.strokePath (secPath, juce::PathStrokeType (juce::jmax (1.0f, unit * 0.50f)),
                              juce::AffineTransform::translation (0.9f, 1.2f));
            }
            PinkXP::Craft::drawStyledStroke (g, secPath,
                                             (isChrome || isNeon || isCrt) ? PinkXP::sel : PinkXP::pink500,
                                             juce::jmax (1.0f, unit * 0.48f));
        }

        // ---- 4. 轴心：Pixel XP 方形 / Chrome CNC 金属帽 / Glass 晶珠 / Neon 发光环 / CRT 矢量空心环 / Modern 圆轴心 ----
        if (isPixelXP)
        {
            const int px    = juce::jmax (2, juce::roundToInt (d / 42.0f));
            const int pivSz = juce::jmax (4, px * 2);
            const juce::Rectangle<int> piv (juce::roundToInt (c.x) - pivSz / 2,
                                            juce::roundToInt (c.y) - pivSz / 2,
                                            pivSz, pivSz);
            g.setColour (PinkXP::pink500);
            g.fillRect (piv);
            g.setColour (PinkXP::dark.withAlpha (0.75f));
            g.drawRect (piv, 1);
            if (pivSz >= 4)
            {
                g.setColour (juce::Colours::white.withAlpha (0.65f));
                g.fillRect (piv.getX() + 1, piv.getY() + 1, 1, 1);
            }
        }
        else if (isChrome)
        {
            const float hubR = juce::jmax (3.2f, unit * 1.55f);
            const auto hubRect = juce::Rectangle<float> (hubR * 2.0f, hubR * 2.0f).withCentre (c);
            if (! greenScreen)
            {
                g.setColour (juce::Colours::black.withAlpha (0.28f));
                g.fillEllipse (hubRect.translated (0.6f, 1.0f));
            }
            juce::ColourGradient hubGrad (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.72f),
                                          hubRect.getX(), hubRect.getY(),
                                          PinkXP::shdw.interpolatedWith (PinkXP::dark, 0.42f),
                                          hubRect.getRight(), hubRect.getBottom(), false);
            g.setGradientFill (hubGrad);
            g.fillEllipse (hubRect);

            g.setColour (PinkXP::dark.withAlpha (0.78f));
            g.drawEllipse (hubRect.reduced (0.4f), 0.95f);

            if (hubR >= 3.4f)
            {
                g.setColour (juce::Colours::white.withAlpha (0.55f));
                g.drawEllipse (hubRect.reduced (1.25f), 0.75f);
            }

            const auto pinRect = hubRect.reduced (hubR * 0.54f);
            g.setColour (PinkXP::sel);
            g.fillEllipse (pinRect);
            if (pinRect.getWidth() >= 2.2f)
            {
                g.setColour (juce::Colours::white.withAlpha (0.68f));
                g.fillEllipse (pinRect.getX() + 0.5f, pinRect.getY() + 0.4f,
                               juce::jmax (1.0f, pinRect.getWidth() * 0.36f),
                               juce::jmax (1.0f, pinRect.getHeight() * 0.36f));
            }
        }
        else if (isGlass)
        {
            // 晶莹宝石玻璃中心帽
            const float hubR = juce::jmax (3.2f, unit * 1.50f);
            const auto hubRect = juce::Rectangle<float> (hubR * 2.0f, hubR * 2.0f).withCentre (c);
            if (! greenScreen)
            {
                g.setColour (juce::Colours::black.withAlpha (0.20f));
                g.fillEllipse (hubRect.translated (0.0f, 1.0f));
            }
            juce::ColourGradient jewel (PinkXP::sel.darker (0.22f), hubRect.getCentreX(), hubRect.getY(),
                                        PinkXP::sel.interpolatedWith (juce::Colours::white, 0.52f),
                                        hubRect.getCentreX(), hubRect.getBottom(), false);
            g.setGradientFill (jewel);
            g.fillEllipse (hubRect);
            g.setColour (juce::Colours::white.withAlpha (0.76f));
            g.fillEllipse (hubRect.getX() + hubR * 0.35f, hubRect.getY() + hubR * 0.25f,
                           hubR * 0.85f, hubR * 0.50f);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawEllipse (hubRect.reduced (0.5f), 0.85f);
        }
        else if (isNeon)
        {
            // 赛博霓虹发光轴环
            const float hubR = juce::jmax (3.0f, unit * 1.45f);
            const auto hubRect = juce::Rectangle<float> (hubR * 2.0f, hubR * 2.0f).withCentre (c);
            if (! greenScreen)
            {
                g.setColour (PinkXP::sel.withAlpha (0.36f));
                g.drawEllipse (hubRect, 3.0f);
            }
            g.setColour (PinkXP::dark.darker (0.45f));
            g.fillEllipse (hubRect);
            g.setColour (PinkXP::sel);
            g.drawEllipse (hubRect.reduced (0.3f), 1.35f);
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.65f));
            g.drawEllipse (hubRect.reduced (0.3f), 0.65f);
        }
        else if (isCrt)
        {
            // Tektronix 极简空心矢量环 + 中心准星点
            const float hubR = juce::jmax (3.0f, unit * 1.45f);
            const auto hubRect = juce::Rectangle<float> (hubR * 2.0f, hubR * 2.0f).withCentre (c);
            if (! greenScreen)
            {
                g.setColour (PinkXP::sel.withAlpha (0.28f));
                g.drawEllipse (hubRect, 2.6f);
            }
            g.setColour (PinkXP::sel);
            g.drawEllipse (hubRect, 1.25f);
            g.fillEllipse (c.x - 1.0f, c.y - 1.0f, 2.0f, 2.0f);
        }
        else
        {
            const float hubR = juce::jmax (2.5f, unit * 1.35f);
            const auto hubRect = juce::Rectangle<float> (hubR * 2.0f, hubR * 2.0f).withCentre (c);
            g.setColour (PinkXP::pink500);
            g.fillEllipse (hubRect);
            g.setColour (PinkXP::content);
            g.fillEllipse (hubRect.reduced (hubR * 0.52f));
        }
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getCurrentTime();
        const int key = now.getSeconds() * 2 + (now.getMilliseconds() >= 500 ? 1 : 0);
        if (key != lastKey)
        {
            lastKey = key;
            repaint();
        }
    }

    bool analogue = false, h24 = true, seconds = true, showDate = true;
    int lastKey = -1;
};

// ==========================================================
// 便签
// ==========================================================
class NotesView : public ToolView,
                  private juce::Timer
{
public:
    NotesView()
    {
        editor.setMultiLine (true, true);
        editor.setReturnKeyStartsNewLine (true);
        editor.setScrollbarsShown (true);
        editor.setTabKeyUsedAsCharacter (false);
        editor.onTextChange = [this] { startTimer (700); };   // 停止输入 0.7 s 后再存档
        addAndMakeVisible (editor);
        themeChanged();
    }

    ~NotesView() override
    {
        // 不在这里通知存档：模块切换 / 存档时会直接读取当前文本（save），
        //   析构期间再回调模块反而可能访问到正在销毁的视图。
        stopTimer();
    }

    bool getParam (const juce::String& id, double& out) const override
    {
        if (id == kNoteSize) { out = fontSize; return true; }
        return false;
    }

    bool setParam (const juce::String& id, double v) override
    {
        if (id != kNoteSize) return false;
        fontSize = juce::jlimit (10.0f, 32.0f, (float) v);
        themeChanged();
        return true;
    }

    void save (juce::ValueTree& s) const override
    {
        s.setProperty ("text", editor.getText(), nullptr);
        s.setProperty (kNoteSize, fontSize, nullptr);
    }

    void restore (const juce::ValueTree& s) override
    {
        if (s.hasProperty (kNoteSize)) fontSize = juce::jlimit (10.0f, 32.0f, (float) (double) s.getProperty (kNoteSize));
        if (s.hasProperty ("text"))    editor.setText (s.getProperty ("text").toString(), juce::dontSendNotification);
        themeChanged();
    }

    void themeChanged() override
    {
        // 便签可能含中文等字符：用系统字体，而不是只有拉丁字形的像素字体
        styleEditor (editor, juce::Font (juce::FontOptions (fontSize)));
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor.setTextToShowWhenEmpty (y2k::tr ("Type a note..."), inkOnContent().withAlpha (0.35f));
        resized();
        repaint();
    }

    void setBackdropChromeHidden (bool hidden) override
    {
        const float a = hidden ? 0.0f : 1.0f;
        if (std::abs (editor.getAlpha() - a) > 0.001f)
            editor.setAlpha (a);
    }

    void paint (juce::Graphics& g) override
    {
        PinkXP::Craft::drawPanelWell (g, getLocalBounds(), PinkXP::content, displayWellKind());
    }

    void resized() override
    {
        const auto fs = PinkXP::getSkinTokens().frameStyle;
        const int inset = (fs == PinkXP::FrameStyle::modernStudio || fs == PinkXP::FrameStyle::frostedGlass) ? 3 : 2;
        editor.setBounds (getLocalBounds().reduced (inset));
    }

private:
    void timerCallback() override
    {
        stopTimer();
        notifyChanged();
    }

    juce::TextEditor editor;
    float fontSize = 15.0f;
};

// ==========================================================
// 工厂
// ==========================================================
std::unique_ptr<ToolView> createToolView (ToolKind kind)
{
    switch (kind)
    {
        case ToolKind::calculator:   return std::make_unique<CalculatorView>();
        case ToolKind::colourPicker: return std::make_unique<ColourPickerView>();
        case ToolKind::stopwatch:    return std::make_unique<StopwatchView>();
        case ToolKind::timer:        return std::make_unique<CountdownView>();
        case ToolKind::converter:    return std::make_unique<ConverterView>();
        case ToolKind::clock:        return std::make_unique<ClockView>();
        case ToolKind::notes:        return std::make_unique<NotesView>();
        case ToolKind::numTools:     break;
    }
    return std::make_unique<CalculatorView>();
}

} // namespace y2k::util
