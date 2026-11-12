#include "source/ui/ModuleControlDrawer.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"

// ==========================================================
// ModuleControlDrawer
// ==========================================================

namespace
{
    constexpr int kPad       = 6;    // 抽屉内边距
    constexpr int kRowH      = 20;   // 一行控件高度（未缩放）
    constexpr int kRowGap    = 4;
    constexpr int kHeaderH   = 14;   // 分组标题高度
    constexpr int kLabelMinW = 54;   // 标签列最小宽
    constexpr int kLabelMaxW = 120;  // 标签列最大宽

    void markNoToy (juce::Component& c)
    {
        c.getProperties().set ("toy.optOut", true);
    }

    int labelWidthFor (const std::vector<std::unique_ptr<juce::Component>>*, int total)
    {
        // 窄模块（<260）标签列收窄，避免控件被挤没
        if (total < 200) return kLabelMinW;
        if (total < 300) return 78;
        return kLabelMaxW;
    }
}

void ModuleControlDrawer::Inner::paint (juce::Graphics& g)
{
    // 分组标题 + 参数标签
    for (const auto& h : drawer.headers)
    {
        g.setColour (PinkXP::pink600.withAlpha (0.9f));
        g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
        g.drawText (y2k::tr (h.text), h.rect, juce::Justification::centredLeft, true);
        g.setColour (PinkXP::shdw.withAlpha (0.5f));
        g.fillRect (h.rect.getX(), h.rect.getBottom() - 1, h.rect.getWidth(), 1);
    }

    for (const auto& r : drawer.rows)
    {
        if (! r->visible) continue;
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (11.0f, juce::Font::plain));
        g.drawText (y2k::tr (r->def.label), r->labelRect, juce::Justification::centredLeft, true);
    }
}

ModuleControlDrawer::ModuleControlDrawer (ModulePanel& ownerPanel)
    : owner (ownerPanel)
{
    markNoToy (*this);
    markNoToy (viewport);
    markNoToy (inner);

    viewport.setViewedComponent (&inner, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (PinkXP::ui (8));
    addAndMakeVisible (viewport);

    // 抽屉本身吃掉鼠标：既不让点击穿透到模块（避免触发 Toybox / 悬停标尺 /
    //   按住冻结），也不让边缘缩放热区抢事件。
    setInterceptsMouseClicks (true, true);
    setOpaque (false);

    rebuild();
}

ModuleControlDrawer::~ModuleControlDrawer() = default;

void ModuleControlDrawer::rebuild()
{
    rows.clear();
    headers.clear();
    inner.removeAllChildren();

    std::vector<ParamDef> defs;
    owner.buildParamDefs (defs);

    for (auto& d : defs)
    {
        auto row = std::make_unique<Row>();
        row->def = std::move (d);
        const size_t idx = rows.size();

        const juce::String tipText = y2k::help::make (row->def.label, row->def.tip);
        auto tip = [&] (juce::SettableTooltipClient& c) { c.setTooltip (tipText); };

        switch (row->def.kind)
        {
            case ParamDef::Kind::choice:
            {
                for (int i = 0; i < row->def.choices.size(); ++i)
                {
                    auto b = std::make_unique<juce::TextButton> (row->def.choices[i]);
                    b->setClickingTogglesState (false);
                    b->onClick = [this, idx, i] { onParamChanged (idx, (double) i); };
                    tip (*b);
                    markNoToy (*b);
                    inner.addAndMakeVisible (*b);
                    row->btns.push_back (std::move (b));
                }
                break;
            }
            case ParamDef::Kind::toggle:
            {
                auto b = std::make_unique<juce::TextButton> ("OFF");
                b->setClickingTogglesState (true);
                b->onClick = [this, idx, bp = b.get()] { onParamChanged (idx, bp->getToggleState() ? 1.0 : 0.0); };
                tip (*b);
                markNoToy (*b);
                inner.addAndMakeVisible (*b);
                row->btns.push_back (std::move (b));
                break;
            }
            case ParamDef::Kind::slider:
            {
                auto s = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
                s->setRange (row->def.minV, row->def.maxV, row->def.step);
                if (row->def.skewMid > row->def.minV && row->def.skewMid < row->def.maxV)
                    s->setSkewFactorFromMidPoint (row->def.skewMid);
                s->setTextBoxStyle (juce::Slider::TextBoxRight, false, PinkXP::ui (52), PinkXP::ui (kRowH));
                s->setTextValueSuffix (row->def.suffix);
                s->setColour (juce::Slider::textBoxTextColourId, PinkXP::ink);
                s->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
                s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                s->onValueChange = [this, idx, sp = s.get()] { onParamChanged (idx, sp->getValue()); };
                tip (*s);
                markNoToy (*s);
                inner.addAndMakeVisible (*s);
                row->slider = std::move (s);
                break;
            }
        }

        rows.push_back (std::move (row));
    }

    lastInnerWidth = -1;
    syncFromModule();
}

void ModuleControlDrawer::onParamChanged (size_t rowIndex, double value)
{
    if (syncing || rowIndex >= rows.size())
        return;

    owner.setParamValue (rows[rowIndex]->def.id, value);
    // 模块可能联动改了别的参数（互斥 / 可见性），整体回读一遍
    syncFromModule();
    owner.refreshControlPanel();
    if (owner.onParamChangedByUser)
        owner.onParamChangedByUser (owner);
}

void ModuleControlDrawer::syncFromModule()
{
    const juce::ScopedValueSetter<bool> guard (syncing, true);

    bool visibilityChanged = false;

    for (auto& r : rows)
    {
        const bool vis = ! r->def.visibleIf || r->def.visibleIf();
        if (vis != r->visible)
        {
            r->visible = vis;
            visibilityChanged = true;
        }

        const double v = owner.getParamValue (r->def.id);

        switch (r->def.kind)
        {
            case ParamDef::Kind::choice:
                for (int i = 0; i < (int) r->btns.size(); ++i)
                    r->btns[(size_t) i]->setToggleState (i == juce::roundToInt (v), juce::dontSendNotification);
                break;
            case ParamDef::Kind::toggle:
                if (! r->btns.empty())
                {
                    r->btns[0]->setToggleState (v >= 0.5, juce::dontSendNotification);
                    r->btns[0]->setButtonText (v >= 0.5 ? "ON" : "OFF");
                }
                break;
            case ParamDef::Kind::slider:
                if (r->slider != nullptr)
                    r->slider->setValue (v, juce::dontSendNotification);
                break;
        }
    }

    if (visibilityChanged)
    {
        lastInnerWidth = -1;
        resized();
    }
    inner.repaint();
}

int ModuleControlDrawer::layoutInner (int width)
{
    headers.clear();

    const int rowH   = PinkXP::ui (kRowH);
    const int gap    = PinkXP::ui (kRowGap);
    const int pad    = PinkXP::ui (kPad);
    const int headH  = PinkXP::ui (kHeaderH);
    const int labelW = juce::jmin (PinkXP::ui (labelWidthFor (nullptr, width)), juce::jmax (30, width / 2));

    int y = pad;
    juce::String lastGroup;

    for (auto& r : rows)
    {
        for (auto& b : r->btns) b->setVisible (r->visible);
        if (r->slider != nullptr) r->slider->setVisible (r->visible);
        if (! r->visible) continue;

        if (r->def.group.isNotEmpty() && r->def.group != lastGroup)
        {
            lastGroup = r->def.group;
            headers.push_back ({ r->def.group, { pad, y, width - pad * 2, headH } });
            y += headH + gap;
        }

        const int ctlX = pad + labelW;
        const int ctlW = juce::jmax (20, width - pad - ctlX);
        r->labelRect = { pad, y, labelW - 4, rowH };

        if (r->def.kind == ParamDef::Kind::slider)
        {
            r->slider->setBounds (ctlX, y, ctlW, rowH);
            y += rowH + gap;
        }
        else if (r->def.kind == ParamDef::Kind::toggle)
        {
            r->btns[0]->setBounds (ctlX, y, juce::jmin (ctlW, PinkXP::ui (52)), rowH);
            y += rowH + gap;
        }
        else
        {
            // choice：按钮宽 = 文本宽 + 边距，放不下自动换行
            int x = ctlX;
            int lineY = y;
            for (auto& b : r->btns)
            {
                const auto shown = y2k::tr (b->getButtonText());
                const int bw = juce::jmax (PinkXP::ui (26),
                    (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown,
                        PinkXP::SymbolGlyphs::buttonFont (shown, 11.0f, juce::Font::bold))) + PinkXP::ui (14));

                if (x + bw > width - pad && x > ctlX)
                {
                    x = ctlX;
                    lineY += rowH + gap;
                }
                b->setBounds (x, lineY, juce::jmin (bw, width - pad - x), rowH);
                x += bw + gap;
            }
            y = lineY + rowH + gap;
        }
    }

    return y + pad - gap;
}

int ModuleControlDrawer::getContentHeight (int width)
{
    return layoutInner (juce::jmax (40, width - viewport.getScrollBarThickness()));
}

void ModuleControlDrawer::resized()
{
    viewport.setBounds (getLocalBounds().reduced (2));

    const int innerW = juce::jmax (40, viewport.getMaximumVisibleWidth());
    const int innerH = layoutInner (innerW);
    lastInnerWidth  = innerW;
    lastInnerHeight = innerH;
    inner.setSize (innerW, juce::jmax (innerH, viewport.getHeight()));
    inner.repaint();
}

void ModuleControlDrawer::paint (juce::Graphics& g)
{
    // 凸起面板底 + 粉色外框；抽屉自己不透明，盖住下方（含 GL 画布）
    PinkXP::drawRaised (g, getLocalBounds(), PinkXP::face);
}
