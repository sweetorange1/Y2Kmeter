// ==========================================================
// PalettePanel.cpp —— 调色面板实现
// ==========================================================
#include "source/ui/palette/PalettePanel.h"
#include "source/ui/palette/ThemeLibrary.h"
#include "source/ui/PinkXPStyle.h"

namespace
{
    // ---- hex 工具 ----
    juce::String rgbToHex(juce::Colour c)
    {
        return juce::String::toHexString((int) c.getARGB()).substring(2).toUpperCase();
    }

    juce::String rgb8ToHex(y2kpal::Rgb8 c)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X", c.r, c.g, c.b);
        return juce::String(buf);
    }

    bool parseHex(juce::String s, juce::Colour& out)
    {
        s = s.trimStart().removeCharacters("# ").trim();
        if (s.length() == 6)
        {
            const juce::Colour c = juce::Colour::fromString("ff" + s);
            if (c.isOpaque()) { out = c; return true; }
        }
        return false;
    }

    // 标题图标候选（UTF-8 转义字节 + fromUTF8，与 PinkXPStyle 主题图标同路径；
    //   直接字面量在本项目的像素字体渲染链下会乱码）
    const char* kTitleIcons[] = {
        "\xe2\x99\xa1",   // ♡
        "\xe2\x98\x85",   // ★
        "\xe2\x97\x86",   // ◆
        "\xe2\x97\x8f",   // ●
        "\xe2\x9c\xa6",   // ✦
        "\xe2\x8c\x81",   // ⌁
        "\xe2\x99\xa5",   // ♥
        "\xe2\x96\xb2"    // ▲
    };
    const char* kPatternNames[] = { "Checker", "Pixel Stars", "Scan Grid", "Big Dots",
                                    "Bubbles", "Diag Stripes", "Hearts",
                                    "Zigzag", "Cross Hatch", "Flower Dots" };
    const char* kHarmonySymbols[] = { "M", "A", "C", "S", "T" };
    const char* kHarmonyNames[]  = { "Mono", "Analogous", "Complementary", "Split", "Triad" };

    juce::Colour toJuce(y2kpal::Rgb8 c) { return juce::Colour(c.r, c.g, c.b); }
}

// ==========================================================
// HueRing
// ==========================================================
HueRing::HueRing()
{
    setInterceptsMouseClicks(true, false);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void HueRing::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds();
    const int cx = area.getCentreX();
    const int cy = area.getCentreY();
    const int rOuter = juce::jmin(area.getWidth(), area.getHeight()) / 2 - 4;
    const int rInner = rOuter - 14;

    // 环：每 1° 一小段，颜色 = 该色相在当前明暗/vibe 下的 accent（所见即所得）
    const float accentL = (brightness_ == 1) ? 0.575f : (brightness_ == 2 ? 0.72f : 0.74f);
    const float accentC = 0.08f + 0.18f * vibe_;

    for (int deg = 0; deg < 360; ++deg)
    {
        // 0° 在顶部（12 点），顺时针——与 mouseDown 的 atan2(dx, -dy) 对齐
        const float rad = (float) (deg - 90) * (3.14159265358979323846f / 180.0f);
        const float ca = std::cos(rad);
        const float sa = std::sin(rad);
        const float r1 = (float) rInner;
        const float r2 = (float) rOuter;

        juce::Path seg;
        seg.startNewSubPath(cx + r1 * ca, cy + r1 * sa);
        seg.lineTo(cx + r2 * ca, cy + r2 * sa);
        seg.lineTo(cx + r2 * std::cos(rad + 0.018f), cy + r2 * std::sin(rad + 0.018f));
        seg.lineTo(cx + r1 * std::cos(rad + 0.018f), cy + r1 * std::sin(rad + 0.018f));
        seg.closeSubPath();

        const y2kpal::Rgb8 col = y2kpal::okLchToRgb8(
            y2kpal::gamutClamp({ accentL, accentC, (float) deg }));
        g.setColour(juce::Colour(col.r, col.g, col.b));
        g.fillPath(seg);
    }

    // 环中心：当前 accent 色块 + hue 数字
    g.setColour(PinkXP::dark);
    g.drawEllipse(cx - rOuter, cy - rOuter, rOuter * 2, rOuter * 2, 1.0f);
    g.drawEllipse(cx - rInner, cy - rInner, rInner * 2, rInner * 2, 1.0f);

    const y2kpal::Rgb8 accentNow = y2kpal::okLchToRgb8(
        y2kpal::gamutClamp({ accentL, accentC, hue_ }));
    g.setColour(juce::Colour(accentNow.r, accentNow.g, accentNow.b));
    g.fillEllipse((float) cx - 10.0f, (float) cy - 10.0f, 20.0f, 20.0f);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(juce::String((int) std::lroundf(hue_)), cx - 12, cy - 8, 24, 16,
               juce::Justification::centred, false);

    // 拖点：环上小圆（与环颜色同方向：顶部 0°，顺时针）
    const float rad = (hue_ - 90.0f) * (3.14159265358979323846f / 180.0f);
    const float rr = (float) (rInner + rOuter) / 2.0f;
    const int px = cx + (int) std::lroundf(rr * std::cos(rad));
    const int py = cy + (int) std::lroundf(rr * std::sin(rad));
    g.setColour(PinkXP::ink);
    g.fillEllipse((float) px - 5.0f, (float) py - 5.0f, 10.0f, 10.0f);
    g.setColour(PinkXP::hl);
    g.fillEllipse((float) px - 3.0f, (float) py - 3.0f, 6.0f, 6.0f);
}

void HueRing::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = true;
    if (onHueDragStart) onHueDragStart();
    const auto c = getLocalBounds().getCentre();
    const float dy = (float) (e.getPosition().y - c.y);
    const float dx = (float) (e.getPosition().x - c.x);
    hue_ = y2kpal::wrapHue(std::atan2(dx, -dy) * (180.0f / 3.14159265358979323846f));
    repaint();
    if (onHueChanged) onHueChanged(hue_);
}

void HueRing::mouseDrag(const juce::MouseEvent& e)
{
    const auto c = getLocalBounds().getCentre();
    const float dy = (float) (e.getPosition().y - c.y);
    const float dx = (float) (e.getPosition().x - c.x);
    hue_ = y2kpal::wrapHue(std::atan2(dx, -dy) * (180.0f / 3.14159265358979323846f));
    repaint();
    if (onHueChanged) onHueChanged(hue_);
}

void HueRing::mouseUp(const juce::MouseEvent&)
{
    if (!dragging_)
        return;
    dragging_ = false;
    if (onHueDragEnd) onHueDragEnd();
}

// ==========================================================
// MiniPreviewCard
// ==========================================================
void MiniPreviewCard::paint(juce::Graphics& g)
{
    // 用面板的 lastGen_（拖动中实时）绘制；gen_ 为空时回退 PinkXP 全局。
    const bool live = (gen_ != nullptr);
    const auto L = [&](y2kpal::Rgb8 c, juce::Colour fallback)
    {
        return live ? juce::Colour(c.r, c.g, c.b) : fallback;
    };

    const auto a = getLocalBounds();
    PinkXP::drawSunken(g, a, live ? L(gen_->face, {}) : PinkXP::face);
    auto in = a.reduced(3);

    // 迷你标题栏
    auto bar = in.removeFromTop(18);
    g.setColour(L(gen_->accent, PinkXP::sel));
    g.fillRect(bar);
    g.setColour(L(gen_->selInk, PinkXP::selInk));
    g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
    g.drawText("MODULE 01", bar, juce::Justification::centred, false);

    auto body = in.reduced(4, 2);

    // 按钮 + 推子
    auto btn = body.removeFromTop(14).removeFromLeft(34);
    PinkXP::drawRaised(g, btn, L(gen_->btnFace, PinkXP::btnFace));
    g.setColour(L(gen_->ink, PinkXP::ink));
    g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
    g.drawText("OK", btn, juce::Justification::centred, false);

    auto fader = body.removeFromTop(14);
    g.setColour(L(gen_->face, PinkXP::face));
    g.fillRect(fader);
    g.setColour(L(gen_->shdw, PinkXP::shdw));
    g.fillRect(fader.getX(), fader.getBottom() - 1, fader.getWidth(), 1);
    const int trackW = fader.getWidth() * 2 / 3;
    g.setColour(L(gen_->accent, PinkXP::sel));
    g.fillRect(fader.getX() + (fader.getWidth() - trackW) / 2, fader.getY() + 1, trackW, 3);

    body.removeFromTop(6);

    // 频谱渐变（chartRamp 8 段竖条）
    auto ramp = body.removeFromTop(20);
    const int n = 8;
    const int w = ramp.getWidth() / n;
    for (int i = 0; i < n; ++i)
    {
        const int h = ramp.getHeight() * (i + 2) / (n + 2);
        g.setColour(live ? juce::Colour(gen_->chartRamp[i].r,
                                        gen_->chartRamp[i].g,
                                        gen_->chartRamp[i].b)
                         : PinkXP::chartRamp[i]);
        g.fillRect(ramp.getX() + i * w, ramp.getBottom() - h, w - 1, h);
    }

    // 文字行
    body.removeFromTop(4);
    g.setColour(L(gen_->ink, PinkXP::ink));
    g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
    g.drawText(juce::String::fromUTF8("Aa 123 \xC2\xB7 Y2K"), body, juce::Justification::centredLeft, false);
}

// ==========================================================
// SvHuePicker
// ==========================================================
SvHuePicker::SvHuePicker()
{
    setSize(190, 216);
    setCurrentColour(juce::Colours::hotpink);
}

void SvHuePicker::setCurrentColour(juce::Colour c)
{
    current_ = c;
    hue_ = c.getHue();            // JUCE getHue 返回 [0,1]（不是度数）
    sat_ = c.getSaturation();
    val_ = c.getBrightness();
    repaint();
}

void SvHuePicker::setColourToEdit(juce::Colour c) { setCurrentColour(c); }

void SvHuePicker::paint(juce::Graphics& g)
{
    PinkXP::drawSunken(g, getLocalBounds(), PinkXP::btnFace);
    auto in = getLocalBounds().reduced(3);

    // SV 方块：像素风 12x12 大格（X=S, Y=V）
    svRect_ = in.removeFromTop(150).reduced(2);
    g.setColour(PinkXP::dark);
    g.drawRect(svRect_, 1);
    const int cells = 12;
    const int cw = svRect_.getWidth() / cells;
    const int ch = svRect_.getHeight() / cells;
    for (int y = 0; y < cells; ++y)
    {
        for (int x = 0; x < cells; ++x)
        {
            const float s = (float) x / (float) (cells - 1);
            const float v = 1.0f - (float) y / (float) (cells - 1);
            // hue_ 是 [0,1]（JUCE getHue 体系），直接用于 fromHSV
            g.setColour(juce::Colour::fromHSV(hue_, s, v, 1.0f));
            g.fillRect(svRect_.getX() + x * cw, svRect_.getY() + y * ch,
                       cw + 1, ch + 1);
        }
    }
    // 选点标记
    const int mx = svRect_.getX() + (int) std::lroundf(sat_ * (svRect_.getWidth() - 1));
    const int my = svRect_.getY() + (int) std::lroundf((1.0f - val_) * (svRect_.getHeight() - 1));
    g.setColour(PinkXP::ink);
    g.drawRect(mx - 3, my - 3, 6, 6, 2);
    g.setColour(juce::Colours::white);
    g.drawRect(mx - 4, my - 4, 8, 8, 1);

    // 色相条
    in.removeFromTop(4);
    hueRect_ = in.removeFromTop(16).reduced(2);
    const int segments = 36;
    const int sw = hueRect_.getWidth() / segments;
    for (int i = 0; i < segments; ++i)
    {
        // fromHSV 的 hue 范围是 [0,1]（不是度数）
        g.setColour(juce::Colour::fromHSV((float) i / (float) segments, 1.0f, 1.0f, 1.0f));
        g.fillRect(hueRect_.getX() + i * sw, hueRect_.getY(), sw + 1, hueRect_.getHeight());
    }
    const int hx = hueRect_.getX() + (int) std::lroundf(hue_ * (hueRect_.getWidth() - 1));
    g.setColour(PinkXP::ink);
    g.fillRect(hx - 1, hueRect_.getY() - 2, 3, hueRect_.getHeight() + 4);

    // 当前色 + OK
    in.removeFromTop(8);
    auto row = in.removeFromTop(22);
    g.setColour(current_);
    g.fillRect(row.removeFromLeft(30));
    g.setColour(PinkXP::dark);
    g.drawRect(row.getX() - 30, row.getY(), 30, row.getHeight(), 1);
    okRect_ = row.removeFromRight(44).reduced(0, 2);
    PinkXP::drawRaised(g, okRect_, PinkXP::btnFace);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    g.drawText("OK", okRect_, juce::Justification::centred, false);
}

void SvHuePicker::resized() { repaint(); }

void SvHuePicker::updateFromMouse(juce::Point<int> p)
{
    if (svRect_.contains(p))
    {
        sat_ = (float) (p.x - svRect_.getX()) / (float) juce::jmax(1, svRect_.getWidth());
        val_ = 1.0f - (float) (p.y - svRect_.getY()) / (float) juce::jmax(1, svRect_.getHeight());
        sat_ = y2kpal::clamp01(sat_);
        val_ = y2kpal::clamp01(val_);
        commitColour();
    }
    else if (hueRect_.contains(p))
    {
        hue_ = (float) (p.x - hueRect_.getX()) / (float) juce::jmax(1, hueRect_.getWidth());
        hue_ = y2kpal::clamp01(hue_);
        commitColour();
    }
    else if (okRect_.contains(p))
    {
        if (onAccepted) onAccepted(current_);
    }
}

void SvHuePicker::commitColour()
{
    // hue_ 全程 [0,1]（JUCE getHue/fromHSV 统一体系），无需换算
    current_ = juce::Colour::fromHSV(hue_, sat_, val_, 1.0f);
    repaint();
    if (onColourChanged) onColourChanged(current_);
}

void SvHuePicker::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = true;
    updateFromMouse(e.getPosition());
}

void SvHuePicker::mouseDrag(const juce::MouseEvent& e)
{
    updateFromMouse(e.getPosition());
}

void SvHuePicker::mouseUp(const juce::MouseEvent&)
{
    if (!dragging_)
        return;
    dragging_ = false;
    if (onDragEnd) onDragEnd(current_);
}

// ==========================================================
// RoleRow
// ==========================================================
RoleRow::RoleRow()
{
    nameLabel_.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    addAndMakeVisible(nameLabel_);

    // 色块按钮：按钮底色 = 该角色颜色，中心画 "▶" 提示可点击取色
    swatchBtn_.setButtonText(juce::String::fromUTF8("\xE2\x96\xB6"));
    swatchBtn_.onClick = [this] { if (onPick_) onPick_(role_); };
    addAndMakeVisible(swatchBtn_);

    hexEditor_.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    hexEditor_.setInputRestrictions(7, "0123456789abcdefABCDEF# ");
    // PinkXP 样式（与命名输入框一致：浅底深边 + 主题文字色）
    hexEditor_.setColour(juce::TextEditor::backgroundColourId, PinkXP::content);
    hexEditor_.setColour(juce::TextEditor::textColourId, PinkXP::ink);
    hexEditor_.setColour(juce::TextEditor::outlineColourId, PinkXP::shdw);
    hexEditor_.setColour(juce::TextEditor::focusedOutlineColourId, PinkXP::sel);
    hexEditor_.setColour(juce::CaretComponent::caretColourId, PinkXP::ink);
    hexEditor_.onTextChange = [this]
    {
        if (updating_ || !onManualSet_) return;
        juce::Colour c;
        if (parseHex(hexEditor_.getText(), c))
            onManualSet_(role_, { c.getRed(), c.getGreen(), c.getBlue() });
    };
    addAndMakeVisible(hexEditor_);

    // Auto 开关：TextButton + toggle 态（按下=自动，弹起=手动锁定）。
    //   之前用 ToggleButton，其勾选框/文字绘制在深浅两种主题下都无法稳定
    //   可见（V4 的 drawTickBox/drawToggleButton 颜色链难以覆盖）；
    //   换成 TextButton 后完全走项目统一的按钮样式（btnFace/pink100 底 +
    //   ink 文字），与面板其他按钮观感一致且绝对可见。
    autoToggle_.setButtonText("Auto");
    autoToggle_.setClickingTogglesState(true);
    autoToggle_.setTooltip("Auto: colour follows the palette recipe. Click off to lock a manual colour.");
    autoToggle_.onClick = [this]
    {
        if (updating_) return;
        if (autoToggle_.getToggleState())
        {
            if (onAutoReset_) onAutoReset_(role_);
        }
        else if (onManualSet_)
        {
            onManualSet_(role_, lastShown_);
        }
    };
    addAndMakeVisible(autoToggle_);

    swatchBtn_.setTooltip("Pick colour with eyedropper-style picker");
}

void RoleRow::setup(int role,
                    std::function<void(int, y2kpal::Rgb8)> onManualSet,
                    std::function<void(int)> onAutoReset,
                    std::function<void(int)> onPick)
{
    role_ = role;
    onManualSet_ = std::move(onManualSet);
    onAutoReset_ = std::move(onAutoReset);
    onPick_ = std::move(onPick);
}

void RoleRow::paint(juce::Graphics& g)
{
    g.setColour(PinkXP::face);
    g.fillRect(getLocalBounds());
    g.setColour(PinkXP::shdw);
    g.fillRect(0, getHeight() - 1, getWidth(), 1);

    // 当前颜色展示块（hex 与 Auto 之间）
    if (!swatchViewRect_.isEmpty())
    {
        g.setColour(juce::Colour(displayColour_.r, displayColour_.g, displayColour_.b));
        g.fillRect(swatchViewRect_);
        g.setColour(PinkXP::dark);
        g.drawRect(swatchViewRect_, 1);
    }
}

void RoleRow::resized()
{
    auto a = getLocalBounds().reduced(4, 3);
    nameLabel_.setBounds(a.removeFromLeft(86));
    swatchBtn_.setBounds(a.removeFromLeft(26).reduced(1));
    a.removeFromLeft(6);
    hexEditor_.setBounds(a.removeFromLeft(82).reduced(0, 2));
    a.removeFromLeft(8);
    swatchViewRect_ = a.removeFromLeft(16).reduced(0, 2);   // 当前色展示块
    a.removeFromLeft(10);
    autoToggle_.setBounds(a.removeFromLeft(52).reduced(0, 2));
}

void RoleRow::refresh(const y2kpal::ThemeSpec& spec, const y2kpal::GeneratedTheme& gen)
{
    updating_ = true;

    nameLabel_.setText(y2kpal::roleName((y2kpal::Role) role_), juce::dontSendNotification);

    const auto& ov = spec.overrides[(size_t) role_];
    y2kpal::Rgb8 shown = ov.set ? ov.rgb
                                : [&]() -> y2kpal::Rgb8 {
        switch ((y2kpal::Role) role_)
        {
            case y2kpal::Role::Accent:     return gen.accent;
            case y2kpal::Role::Accent2:    return gen.accent2;
            case y2kpal::Role::Background: return gen.desktop;
            case y2kpal::Role::Surface:    return gen.content;
            case y2kpal::Role::Control:    return gen.btnFace;
            case y2kpal::Role::Text:       return gen.ink;
            case y2kpal::Role::OnAccent:   return gen.selInk;
            default:                       return gen.accent;
        }
    }();

    swatchBtn_.setColour(juce::TextButton::buttonColourId, toJuce(shown));
    hexEditor_.setText(rgb8ToHex(shown), juce::dontSendNotification);
    autoToggle_.setToggleState(!ov.set, juce::dontSendNotification);
    lastShown_    = shown;
    displayColour_ = shown;

    updating_ = false;
    repaint();
}

// ==========================================================
// MyThemesList
// ==========================================================
MyThemesList::MyThemesList()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks(true, false);
}

void MyThemesList::refreshLayout()
{
    const int rows = PinkXP::ThemeLibrary::instance().size();
    setSize(juce::jmax(getWidth(), 100), juce::jmax(1, rows) * rowHeight);
    repaint();
}

juce::Rectangle<int> MyThemesList::rowBounds(int index) const
{
    return { 0, index * rowHeight, getWidth(), rowHeight - 2 };
}

int MyThemesList::hitTestRow(juce::Point<int> p) const
{
    const int idx = p.y / rowHeight;
    if (p.y < 0 || idx >= PinkXP::ThemeLibrary::instance().size())
        return -1;
    return idx;
}

void MyThemesList::paint(juce::Graphics& g)
{
    g.setColour(PinkXP::content);
    g.fillAll();

    const auto& lib = PinkXP::ThemeLibrary::instance();
    const int w = getWidth();

    for (int i = 0; i < lib.size(); ++i)
    {
        auto r = rowBounds(i);
        if (r.getBottom() < 0 || r.getY() > getHeight()) continue;

        g.setColour(PinkXP::face);
        g.fillRect(r);

        const auto& t = lib.themes()[(size_t) i];
        const y2kpal::GeneratedTheme gen = y2kpal::generate(t.spec);

        // 色票（对角双色：accent + desktop）
        auto sw = r.removeFromLeft(18).reduced(2, 2);
        g.setColour(toJuce(gen.accent));
        g.fillRect(sw);
        juce::Path tri;
        tri.startNewSubPath((float) sw.getX(), (float) sw.getY());
        tri.lineTo((float) sw.getRight(), (float) sw.getY());
        tri.lineTo((float) sw.getX(), (float) sw.getBottom());
        tri.closeSubPath();
        g.setColour(toJuce(gen.desktop));
        g.fillPath(tri);
        g.setColour(PinkXP::dark);
        g.drawRect(sw, 1);

        // 名字
        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
        g.drawText(t.name, r.withTrimmedLeft(4), juce::Justification::centredLeft, true);

        // 右侧按钮区（Apply ↑ ↓ ✎ ×）
        auto btnArea = r.removeFromRight(148);
        auto applyR  = btnArea.removeFromLeft(36).reduced(1, 2);
        auto upR     = btnArea.removeFromLeft(22).reduced(1, 2);
        auto dnR     = btnArea.removeFromLeft(22).reduced(1, 2);
        auto rnR     = btnArea.removeFromLeft(22).reduced(1, 2);
        auto delR    = btnArea.removeFromLeft(22).reduced(1, 2);

        PinkXP::drawRaised(g, applyR, PinkXP::btnFace);
        PinkXP::drawRaised(g, upR,    PinkXP::btnFace);
        PinkXP::drawRaised(g, dnR,    PinkXP::btnFace);
        PinkXP::drawRaised(g, rnR,    PinkXP::btnFace);
        PinkXP::drawRaised(g, delR,   PinkXP::btnFace);

        g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
        g.setColour(PinkXP::ink);
        g.drawText("Use", applyR, juce::Justification::centred, false);
        g.drawText(juce::String::fromUTF8("\xE2\x86\x91"), upR, juce::Justification::centred, false);   // ↑
        g.drawText(juce::String::fromUTF8("\xE2\x86\x93"), dnR, juce::Justification::centred, false);   // ↓
        g.drawText(juce::String::fromUTF8("\xE2\x9C\x8E"), rnR, juce::Justification::centred, false);   // ✎
        g.drawText(juce::String::fromUTF8("\xC3\x97"), delR, juce::Justification::centred, false);      // ×
    }
}

void MyThemesList::mouseMove(const juce::MouseEvent& e)
{
    const int idx = hitTestRow(e.getPosition());
    if (idx != hoverRow_) { hoverRow_ = idx; repaint(); }
}

void MyThemesList::mouseExit(const juce::MouseEvent&)
{
    if (hoverRow_ != -1) { hoverRow_ = -1; repaint(); }
}

void MyThemesList::mouseDown(const juce::MouseEvent& e)
{
    const int idx = hitTestRow(e.getPosition());
    if (idx < 0) return;

    const int w = getWidth();
    const int x = e.getPosition().x;
    const int r = rowBounds(idx).getRight();

    if      (x >= r - 46)  { if (onDelete)   onDelete(idx); }
    else if (x >= r - 68)  { if (onRename)   onRename(idx); }
    else if (x >= r - 90)  { if (onMoveDown) onMoveDown(idx); }
    else if (x >= r - 112) { if (onMoveUp)   onMoveUp(idx); }
    else                   { if (onApply)    onApply(idx); }
}

// ==========================================================
// PalettePanel
// ==========================================================

// 全局点击外部关闭监听器
class PalettePanel::DismissListener : public juce::MouseListener
{
public:
    explicit DismissListener(PalettePanel& ownerRef) : owner_(ownerRef) {}

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!owner_.getScreenBounds().contains(e.getScreenPosition()))
            owner_.dismiss();
    }

private:
    PalettePanel& owner_;
};

PalettePanel::PalettePanel()
    : dismissListener_(std::make_unique<DismissListener>(*this))
{
    // 注意：不在此处 setSize —— resized() 会访问下方创建的控件，
    //   尺寸设置在构造末尾（所有成员创建完成后）统一进行。
    setVisible(false);
    setWantsKeyboardFocus(true);
    setInterceptsMouseClicks(true, true);

    // ---- 色相环 ----
    hueRing_ = std::make_unique<HueRing>();
    // 色相环拖动：拖动中只更新面板内预览（色环/预览卡/角色行），
    //   松手才把主题提交给工作区——避免拖动期间每帧重烘焙桌面纹理。
    hueRing_->onHueDragStart = [this] { hueDragging_ = true; };
    hueRing_->onHueChanged = [this](float h)
    {
        spec_.hue = h;
        regenerateAndRefresh();
        if (!hueDragging_)
            applyCurrentSpec();
    };
    hueRing_->onHueDragEnd = [this]
    {
        hueDragging_ = false;
        applyCurrentSpec();
    };
    addAndMakeVisible(*hueRing_);

    // ---- 明暗两段（R5；Auto 已删除）----
    modeDarkBtn_.setButtonText("Dark");
    modeLightBtn_.setButtonText("Light");
    for (auto* b : { &modeDarkBtn_, &modeLightBtn_ })
    {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(101);
        addAndMakeVisible(*b);
    }
    // 切明暗时清除"与明暗强绑定的角色"的手动锁定：Background/Surface/
    //   Control/Text 由明暗配方决定，若保持锁定则桌面底色/模块画布不会
    //   随 mode 变化（用户看到的"底色卡死"）。Accent/Accent2/OnAccent
    //   是用户主动挑的颜色，保留其锁定。
    auto applyModeChange = [this](y2kpal::Brightness b)
    {
        spec_.brightness = b;
        for (int role : { (int) y2kpal::Role::Background,
                          (int) y2kpal::Role::Surface,
                          (int) y2kpal::Role::Control,
                          (int) y2kpal::Role::Text })
            spec_.overrides[(size_t) role].set = false;
        specChanged();
    };
    modeDarkBtn_.onClick  = [this, applyModeChange] { if (modeDarkBtn_.getToggleState())  applyModeChange (y2kpal::Brightness::Dark);  };
    modeLightBtn_.onClick = [this, applyModeChange] { if (modeLightBtn_.getToggleState()) applyModeChange (y2kpal::Brightness::Light); };

    // ---- 氛围 / 背景染色滑条（R5）----
    vibeLabel_.setText("Vibe", juce::dontSendNotification);
    tintLabel_.setText("Tint", juce::dontSendNotification);
    for (auto* l : { &vibeLabel_, &tintLabel_, &modeLabel_, &harmonyLabel_, &patternLabel_, &iconLabel_ })
    {
        l->setFont(PinkXP::getFont(10.0f, juce::Font::bold));
        addAndMakeVisible(*l);
    }
    modeLabel_.setText("Mode", juce::dontSendNotification);
    harmonyLabel_.setText("Harmony", juce::dontSendNotification);
    patternLabel_.setText("Desktop", juce::dontSendNotification);
    iconLabel_.setText("Icon", juce::dontSendNotification);

    // 滑条拖动：与色相环一致——拖动中只刷新面板内预览，松手提交。
    auto commitIfNotDragging = [this] { if (!sliderDragging_) applyCurrentSpec(); };

    vibeSlider_.setRange(0.0, 1.0, 0.0);
    vibeSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    vibeSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    vibeSlider_.onDragStart = [this] { sliderDragging_ = true; };
    vibeSlider_.onValueChange = [this, commitIfNotDragging]
    {
        spec_.vibe = (float) vibeSlider_.getValue();
        regenerateAndRefresh();
        commitIfNotDragging();
    };
    vibeSlider_.onDragEnd = [this]
    {
        sliderDragging_ = false;
        applyCurrentSpec();
    };
    addAndMakeVisible(vibeSlider_);

    tintSlider_.setRange(0.0, 1.0, 0.0);
    tintSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    tintSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    tintSlider_.onDragStart = [this] { sliderDragging_ = true; };
    tintSlider_.onValueChange = [this, commitIfNotDragging]
    {
        spec_.bgTint = (float) tintSlider_.getValue();
        regenerateAndRefresh();
        commitIfNotDragging();
    };
    tintSlider_.onDragEnd = [this]
    {
        sliderDragging_ = false;
        applyCurrentSpec();
    };
    addAndMakeVisible(tintSlider_);

    // ---- 和谐方式（R5，默认显示，用户决策）----
    for (int i = 0; i < 5; ++i)
    {
        harmonyBtns_[i].setButtonText(kHarmonySymbols[i]);
        harmonyBtns_[i].setTooltip(kHarmonyNames[i]);
        harmonyBtns_[i].setClickingTogglesState(true);
        harmonyBtns_[i].setRadioGroupId(102);
        harmonyBtns_[i].onClick = [this, i]
        {
            if (harmonyBtns_[i].getToggleState())
            {
                spec_.harmony = (y2kpal::Harmony) i;
                specChanged();
            }
        };
        addAndMakeVisible(harmonyBtns_[i]);
    }

    // ---- 桌面纹理（R14）----
    for (int i = 0; i < (int) (sizeof(kPatternNames) / sizeof(kPatternNames[0])); ++i)
        patternCombo_.addItem(kPatternNames[i], i + 1);
    patternCombo_.onChange = [this]
    {
        spec_.pattern = (y2kpal::Pattern) (patternCombo_.getSelectedId() - 1);
        specChanged();
    };
    addAndMakeVisible(patternCombo_);

    // ---- 标题图标（R14：8 个候选）----
    for (int i = 0; i < 8; ++i)
    {
        iconBtns_[i].setButtonText(juce::String::fromUTF8(kTitleIcons[i]));
        iconBtns_[i].setClickingTogglesState(true);
        iconBtns_[i].setRadioGroupId(103);
        iconBtns_[i].onClick = [this, i]
        {
            if (iconBtns_[i].getToggleState())
            {
                spec_.titleIcon = (uint8_t) i;
                specChanged();
            }
        };
        addAndMakeVisible(iconBtns_[i]);
    }

    // ---- 底部动作按钮 ----
    // 不带 emoji：🎲 在 Silkscreen 像素字体下无字形且超出按钮高度
    randomBtn_.setButtonText("Random");
    randomBtn_.setTooltip("Random recipe from the curated library");
    randomBtn_.onClick = [this] { randomize(); };
    addAndMakeVisible(randomBtn_);

    compareBtn_.setButtonText("A/B");
    compareBtn_.setTooltip("Hold to see the theme from before opening the panel");
    compareBtn_.addListener(this);   // Button::Listener（按下/松开）
    addAndMakeVisible(compareBtn_);

    revertBtn_.setButtonText("Revert");
    revertBtn_.setTooltip("Back to the theme when the panel was opened");
    revertBtn_.onClick = [this] { revertToSnapshot(); };
    addAndMakeVisible(revertBtn_);

    saveBtn_.setButtonText("SAVE");
    saveBtn_.setTooltip("Apply current palette and save it to My Themes");
    saveBtn_.onClick = [this] { saveCurrentAsUserTheme(); };
    addAndMakeVisible(saveBtn_);

    closeBtn_.setButtonText("X");
    closeBtn_.onClick = [this] { dismiss(); };
    addAndMakeVisible(closeBtn_);

    // ---- 预览卡 / 徽章 / 提示 ----
    previewCard_.setGen(&lastGen_);   // 预览卡直接读面板生成结果（拖动中实时）
    addAndMakeVisible(previewCard_);
    badgeLabel_.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    badgeLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(badgeLabel_);
    noteLabel_.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
    noteLabel_.setColour(juce::Label::textColourId, PinkXP::ink);
    addAndMakeVisible(noteLabel_);

    // ---- 高级折叠（P3）----
    advancedToggle_.setButtonText(juce::String::fromUTF8("Advanced \xE2\x96\xB8"));   // Advanced ▸
    advancedToggle_.onClick = [this]
    {
        advancedOpen_ = !advancedOpen_;
        advancedToggle_.setButtonText(juce::String::fromUTF8(
            advancedOpen_ ? "Advanced \xE2\x96\xBE" : "Advanced \xE2\x96\xB8"));
        // 折叠 Advanced 时若取色器还开着，一并收起（避免浮在折叠区上方）
        if (!advancedOpen_ && pickingRole_ >= 0)
        {
            pickingRole_ = -1;
            resized();
        }
        updatePanelHeight();
    };
    addAndMakeVisible(advancedToggle_);

    for (int i = 0; i < y2kpal::kRoleCount; ++i)
    {
        auto row = std::make_unique<RoleRow>();
        row->setup(i,
            [this](int role, y2kpal::Rgb8 c)
            {
                spec_.overrides[(size_t) role].set = true;
                spec_.overrides[(size_t) role].rgb = c;
                regenerateAndRefresh();
                applyCurrentSpec();
            },
            [this](int role)
            {
                spec_.overrides[(size_t) role].set = false;
                regenerateAndRefresh();
                applyCurrentSpec();
                // 若取色器正开着该角色：同步到回归后的自动色，避免停留在旧色
                if (pickingRole_ == role)
                    svPicker_.setCurrentColour(toJuce(roleColourFor(role)));
            },
            [this](int role)
            {
                pickingRole_ = role;
                // 以当前角色色作为取色器起点
                const y2kpal::Rgb8 cur = roleColourFor(role);
                juce::Colour start = toJuce(cur);
                if (start.getSaturation() < 0.05f)
                {
                    // 无色角色（Surface/Control/Text 的灰白黑）：getHue()=0 会让
                    //   取色器显示纯红。用当前主题 accent 的色相作为取色起点，
                    //   保持亮度不变——打开时更贴近"当前颜色"的观感。
                    const juce::Colour accent(lastGen_.accent.r,
                                              lastGen_.accent.g,
                                              lastGen_.accent.b);
                    start = juce::Colour::fromHSV(accent.getHue(),
                                                  start.getSaturation(),
                                                  start.getBrightness(), 1.0f);
                }
                svPicker_.setColourToEdit(start);
                updatePanelHeight();
                // 显式 resized：取色器显隐在 resized 里处理，而 updatePanelHeight
                //   在高度未变时不会触发 resized → 取色器可能不显示。
                resized();
            });
        roleRows_[(size_t) i] = std::move(row);
        addChildComponent(*roleRows_[(size_t) i]);
    }

    // ---- 轻量取色器（R9）----
    //   拖动中：只刷新面板内预览（角色行/预览卡实时），不提交全局主题；
    //   松手：一次性提交（与色环/滑条一致，避免拖动期间每帧桌面重烘焙卡顿）。
    svPicker_.onColourChanged = [this](juce::Colour c)
    {
        if (pickingRole_ < 0 || pickingRole_ >= y2kpal::kRoleCount)
            return;
        spec_.overrides[(size_t) pickingRole_].set = true;
        spec_.overrides[(size_t) pickingRole_].rgb = { c.getRed(), c.getGreen(), c.getBlue() };
        regenerateAndRefresh();
        roleRows_[(size_t) pickingRole_]->refresh(spec_, lastGen_);
    };
    svPicker_.onDragEnd = [this](juce::Colour)
    {
        applyCurrentSpec();
    };
    svPicker_.onAccepted = [this](juce::Colour c)
    {
        if (pickingRole_ >= 0 && pickingRole_ < y2kpal::kRoleCount)
        {
            spec_.overrides[(size_t) pickingRole_].set = true;
            spec_.overrides[(size_t) pickingRole_].rgb = { c.getRed(), c.getGreen(), c.getBlue() };
            regenerateAndRefresh();
            applyCurrentSpec();
        }
        pickingRole_ = -1;
        resized();
    };
    addChildComponent(svPicker_);

    // ---- 我的主题区（P4）----
    myThemesToggle_.setButtonText(juce::String::fromUTF8("My Themes \xE2\x96\xB8"));
    myThemesToggle_.onClick = [this]
    {
        myThemesOpen_ = !myThemesOpen_;
        myThemesToggle_.setButtonText(juce::String::fromUTF8(
            myThemesOpen_ ? "My Themes \xE2\x96\xBE" : "My Themes \xE2\x96\xB8"));
        updatePanelHeight();
    };
    addAndMakeVisible(myThemesToggle_);

    myThemesViewport_ = std::make_unique<juce::Viewport>("myThemesView");
    myThemesViewport_->setViewedComponent(&myThemesList_, false);
    myThemesViewport_->setScrollBarsShown(true, false, true, false);
    addChildComponent(*myThemesViewport_);

    myThemesHint_.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
    myThemesHint_.setColour(juce::Label::textColourId, PinkXP::ink);
    myThemesHint_.setText(juce::String::fromUTF8(
                              "Use=apply   \xE2\x86\x91\xE2\x86\x93=sort   \xE2\x9C\x8E=rename   \xC3\x97=delete"),
                          juce::dontSendNotification);
    addChildComponent(myThemesHint_);

    myThemesList_.onApply = [this](int i)
    {
        auto& lib = PinkXP::ThemeLibrary::instance();
        if (i < 0 || i >= lib.size()) return;
        spec_ = lib.themes()[(size_t) i].spec;
        loadSpecIntoUi(spec_);
        regenerateAndRefresh();
        applyCurrentSpec();
    };
    myThemesList_.onRename = [this](int i)
    {
        auto& lib = PinkXP::ThemeLibrary::instance();
        if (i < 0 || i >= lib.size()) return;
        promptForName(lib.themes()[(size_t) i].name,
                      [this, &lib, i](const juce::String& newName)
        {
            lib.renameTheme(i, newName);
        });
    };
    myThemesList_.onDelete = [this](int i)
    {
        PinkXP::ThemeLibrary::instance().removeTheme(i);
    };
    myThemesList_.onMoveUp = [this](int i)
    {
        PinkXP::ThemeLibrary::instance().moveTheme(i, i - 1);
    };
    myThemesList_.onMoveDown = [this](int i)
    {
        PinkXP::ThemeLibrary::instance().moveTheme(i, i + 1);
    };

    copyCodeBtn_.setButtonText("Copy");
    copyCodeBtn_.setTooltip("Copy share code of current palette");
    copyCodeBtn_.onClick = [this] { copyShareCode(); };
    addChildComponent(copyCodeBtn_);

    pasteCodeBtn_.setButtonText("Paste");
    pasteCodeBtn_.setTooltip("Paste share code and apply");
    pasteCodeBtn_.onClick = [this] { pasteShareCode(); };
    addChildComponent(pasteCodeBtn_);

    exportBtn_.setButtonText("Export");
    exportBtn_.onClick = [this] { exportCurrentToFile(); };
    addChildComponent(exportBtn_);

    importBtn_.setButtonText("Import");
    importBtn_.onClick = [this] { importFromFile(); };
    addChildComponent(importBtn_);

    // ---- 内嵌命名输入栏（PinkXP 样式：浅底 + 深边 + 主题文字色）----
    nameEditor_.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    nameEditor_.setInputRestrictions(32, {});
    nameEditor_.setTextToShowWhenEmpty("theme name", juce::Colours::grey);
    nameEditor_.setColour(juce::TextEditor::backgroundColourId, PinkXP::content);
    nameEditor_.setColour(juce::TextEditor::textColourId, PinkXP::ink);
    nameEditor_.setColour(juce::TextEditor::outlineColourId, PinkXP::shdw);
    nameEditor_.setColour(juce::TextEditor::focusedOutlineColourId, PinkXP::sel);
    nameEditor_.setColour(juce::CaretComponent::caretColourId, PinkXP::ink);
    nameEditor_.onReturnKey = [this] { closeNamePrompt(true); };
    addChildComponent(nameEditor_);

    nameOkBtn_.setButtonText("OK");
    nameOkBtn_.onClick = [this] { closeNamePrompt(true); };
    addChildComponent(nameOkBtn_);

    nameCancelBtn_.setButtonText("Cancel");
    nameCancelBtn_.onClick = [this] { closeNamePrompt(false); };
    addChildComponent(nameCancelBtn_);

    // ---- 面板内轻提示 toast（替代 JUCE 默认 AlertWindow 弹窗）----
    toastLabel_.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    toastLabel_.setColour(juce::Label::textColourId, PinkXP::ink);
    toastLabel_.setColour(juce::Label::backgroundColourId, PinkXP::btnFace);
    toastLabel_.setColour(juce::Label::outlineColourId, PinkXP::shdw);
    toastLabel_.setJustificationType(juce::Justification::centred);
    addChildComponent(toastLabel_);

    // ---- 主题库订阅 ----
    libToken_ = PinkXP::ThemeLibrary::instance().subscribe([this]
    {
        myThemesList_.refreshLayout();
        myThemesList_.repaint();
    });

    // ---- 16ms 节流（S6）----
    startTimerHz(60);

    // 所有成员创建完成后统一设置初始尺寸（触发首次 resized 布局）
    setSize(660, 300);
}

PalettePanel::~PalettePanel()
{
    detachDismissListener();
    PinkXP::ThemeLibrary::instance().unsubscribe(libToken_);
}

// ==================================================
// 实时预览（S6：16ms 节流）
// ==================================================
void PalettePanel::timerCallback()
{
    // 仅处理 toast 超时。主题应用已改为同步（生成器重算 <1ms，AC9），
    //   旧 16ms Timer 节流链路存在断链风险（specDirty_ 置位后未消费）。
    if (toastLabel_.isVisible()
        && juce::Time::getMillisecondCounter() > toastUntil_)
        toastLabel_.setVisible(false);
}

void PalettePanel::applyCurrentSpec()      { applySpecImmediately(); }

void PalettePanel::applySpecImmediately()
{
    if (compareHeld_) return;
    PinkXP::applyThemeFromSpec(spec_, "Custom");

    // ComboBox 内嵌 Label 不自动跟随 LnF —— 单独刷新面板内的下拉框，
    //   避免全局 sendLookAndFeelChange 递归整棵组件树（拖动卡顿源）。
    patternCombo_.lookAndFeelChanged();
}

void PalettePanel::specChanged()
{
    regenerateAndRefresh();
    applyCurrentSpec();
}

void PalettePanel::regenerateAndRefresh()
{
    lastGen_ = y2kpal::generate(spec_);
    refreshRoleRows();
    refreshBadge();
    previewCard_.repaint();
    hueRing_->setContext(spec_.vibe, (int) spec_.brightness);
    hueRing_->repaint();
}

void PalettePanel::refreshRoleRows()
{
    for (int i = 0; i < y2kpal::kRoleCount; ++i)
        if (roleRows_[(size_t) i] != nullptr)
            roleRows_[(size_t) i]->refresh(spec_, lastGen_);
}

void PalettePanel::refreshBadge()
{
    const bool clean = lastGen_.notes.empty();
    // ✓ / ⚠ 用转义字节 + fromUTF8（项目已验证的像素字体渲染路径）
    badgeLabel_.setText(juce::String::fromUTF8(clean ? "\xE2\x9C\x93 QUALITY"
                                                     : "\xE2\x9A\xA0 AUTO-FIXED"),
                        juce::dontSendNotification);
    badgeLabel_.setColour(juce::Label::textColourId, clean ? juce::Colour(0xff2e7d32)
                                                          : juce::Colour(0xffb26a00));

    juce::String tip = clean ? juce::String("All contrast guardrails pass (WCAG >= 4.5:1)")
                             : juce::String("Some values were auto-adjusted:");
    for (const auto& n : lastGen_.notes)
        tip += "\n- " + juce::String::fromUTF8(n.c_str(), (int) n.size());
    badgeLabel_.setTooltip(tip);

    noteLabel_.setText(lastGen_.notes.empty()
                          ? juce::String()
                          : juce::String::fromUTF8(lastGen_.notes.front().c_str(),
                                                   (int) lastGen_.notes.front().size()),
                       juce::dontSendNotification);
}

// ==================================================
// 随机（R4 表 / A7 精选库；用户决策：允许改变明暗）
// ==================================================
void PalettePanel::randomize()
{
    const auto& recipes = y2kpal::getCuratedRecipes();
    const auto& r = recipes[(size_t) (std::rand() % (int) recipes.size())];

    if (!spec_.lockHue)        spec_.hue = r.hue;
    if (!spec_.lockBrightness) spec_.brightness = r.brightness;
    if (!spec_.lockVibe)       spec_.vibe = r.vibe;
    if (!spec_.lockHarmony)    spec_.harmony = r.harmony;

    loadSpecIntoUi(spec_);
    regenerateAndRefresh();
    applyCurrentSpec();
}

// ==================================================
// A/B 对比（R4）
// ==================================================
void PalettePanel::buttonStateChanged(juce::Button* b)
{
    if (b == &compareBtn_)
    {
        if (compareBtn_.isDown()) enterCompare();
        else                      exitCompare();
    }
}

void PalettePanel::enterCompare()
{
    if (compareHeld_) return;
    compareHeld_ = true;
    if (snapshot_.wasCustom)
        PinkXP::applyThemeFromSpec(snapshot_.spec, PinkXP::getCustomThemeName());
    else
        PinkXP::applyTheme((PinkXP::ThemeId) snapshot_.themeId);
}

void PalettePanel::exitCompare()
{
    if (!compareHeld_) return;
    compareHeld_ = false;
    applySpecImmediately();
}

// ==================================================
// 还原（R3）
// ==================================================
void PalettePanel::revertToSnapshot()
{
    if (snapshot_.wasCustom)
        PinkXP::applyThemeFromSpec(snapshot_.spec, PinkXP::getCustomThemeName());
    else
        PinkXP::applyTheme((PinkXP::ThemeId) snapshot_.themeId);

    spec_ = snapshot_.spec;
    loadSpecIntoUi(spec_);
    regenerateAndRefresh();
}

// ==================================================
// 保存/分享/导入导出（R11/R12）
// ==================================================
void PalettePanel::showToast(const juce::String& msg)
{
    toastLabel_.setText(msg, juce::dontSendNotification);
    toastLabel_.setVisible(true);
    toastLabel_.toFront(false);
    toastUntil_ = juce::Time::getMillisecondCounter() + 2000;

    // 按文本宽度调整 toast 尺寸
    const int w = juce::jmin(getWidth() - 60,
        PinkXP::getFont(9.0f, juce::Font::bold).getStringWidth(msg) + 28);
    toastLabel_.setSize(w, 22);
    toastLabel_.setTopLeftPosition((getWidth() - w) / 2,
                                   getHeight() - 60);
}

y2kpal::Rgb8 PalettePanel::roleColourFor(int role) const
{
    const auto& ov = spec_.overrides[(size_t) role];
    if (ov.set)
        return ov.rgb;
    switch ((y2kpal::Role) role)
    {
        case y2kpal::Role::Accent:     return lastGen_.accent;
        case y2kpal::Role::Accent2:    return lastGen_.accent2;
        case y2kpal::Role::Background: return lastGen_.desktop;
        case y2kpal::Role::Surface:    return lastGen_.content;
        case y2kpal::Role::Control:    return lastGen_.btnFace;
        case y2kpal::Role::Text:       return lastGen_.ink;
        case y2kpal::Role::OnAccent:   return lastGen_.selInk;
        default:                       return lastGen_.accent;
    }
}

// 自动生成一个未被占用的"My Palette N"名字
juce::String PalettePanel::nextAutoThemeName()
{
    auto& lib = PinkXP::ThemeLibrary::instance();
    for (int i = 1; i <= 999; ++i)
    {
        const juce::String name = "My Palette " + juce::String(i);
        if (lib.indexOfName(name) < 0)
            return name;
    }
    return "My Palette " + juce::Time::getCurrentTime().formatted("%H%M%S");
}

void PalettePanel::saveCurrentAsUserTheme()
{
    auto& lib = PinkXP::ThemeLibrary::instance();
    const juce::String name = nextAutoThemeName();

    if (lib.saveTheme(name, spec_))
    {
        // 应用为命名主题；落盘由 Standalone 退出时统一 persistToSettings，
        // VST3 在 getStateInformation 时序列化 themeLib 随宿主工程保存。
        PinkXP::applyUserTheme(spec_, name);
        myThemesList_.refreshLayout();
        myThemesList_.repaint();
        showToast("Saved: " + name);
    }
    else
    {
        showToast("Library full (max 12). Delete a theme first.");
    }
}

void PalettePanel::promptForName(const juce::String& initial,
                                 std::function<void(const juce::String&)> onOk)
{
    namePromptOnOk_ = std::move(onOk);
    nameEditor_.setText(initial);
    namePromptVisible_ = true;
    nameEditor_.setVisible(true);
    nameOkBtn_.setVisible(true);
    nameCancelBtn_.setVisible(true);
    nameEditor_.toFront(true);
    nameOkBtn_.toFront(false);
    nameCancelBtn_.toFront(false);
    nameEditor_.grabKeyboardFocus();
    resized();
}

void PalettePanel::closeNamePrompt(bool commit)
{
    if (!namePromptVisible_)
        return;

    if (commit && namePromptOnOk_)
    {
        const juce::String name = nameEditor_.getText().trim();
        if (name.isNotEmpty())
            namePromptOnOk_(name);
    }

    namePromptOnOk_ = nullptr;
    namePromptVisible_ = false;
    nameEditor_.setVisible(false);
    nameOkBtn_.setVisible(false);
    nameCancelBtn_.setVisible(false);
}

void PalettePanel::copyShareCode()
{
    const juce::String code = PinkXP::ThemeLibrary::specToShareCode(spec_);
    juce::SystemClipboard::copyTextToClipboard(code);
    showToast("Code copied: " + code.substring(0, 12) + "...");
}

void PalettePanel::pasteShareCode()
{
    const juce::String code = juce::SystemClipboard::getTextFromClipboard();
    y2kpal::ThemeSpec spec;
    if (!PinkXP::ThemeLibrary::shareCodeToSpec(code, spec))
    {
        showToast("Clipboard has no valid palette code");
        return;
    }

    auto& lib = PinkXP::ThemeLibrary::instance();
    const juce::String name = nextAutoThemeName();
    if (!lib.saveTheme(name, spec))
    {
        showToast("Library full (max 12). Delete a theme first.");
        return;
    }

    // 应用为当前预览（要放弃可 Revert）
    spec_ = spec;
    loadSpecIntoUi(spec_);
    regenerateAndRefresh();
    applyCurrentSpec();
    myThemesList_.refreshLayout();
    myThemesList_.repaint();
}

void PalettePanel::exportCurrentToFile()
{
    // 插件模式下 JUCE_MODAL_LOOPS_PERMITTED=0，必须用 launchAsync（与工具栏
    //   Save/Load 预设一致的异步非模态写法）
    auto chooser = std::make_shared<juce::FileChooser>(
        "Export palette...",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("my-palette.y2kpalette"),
        "*.y2kpalette");

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<PalettePanel> safe(this);
    chooser->launchAsync(flags, [safe, chooser](const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        juce::File f = fc.getResult();
        if (f == juce::File{}) return;                    // 用户取消
        if (f.getFileExtension().isEmpty())
            f = f.withFileExtension("y2kpalette");

        juce::XmlElement root("Y2KPALETTE");
        root.setAttribute("name", PinkXP::getCustomThemeName());
        root.setAttribute("spec", juce::String(safe->spec_.shareCode()));
        f.replaceWithText(root.toString());
    });
}

void PalettePanel::importFromFile()
{
    auto chooser = std::make_shared<juce::FileChooser>(
        "Import palette...",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.y2kpalette");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;

    juce::Component::SafePointer<PalettePanel> safe(this);
    chooser->launchAsync(flags, [safe, chooser](const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        const juce::File result = fc.getResult();
        if (result == juce::File{} || !result.existsAsFile()) return;

        juce::String name;
        y2kpal::ThemeSpec spec;
        if (!PinkXP::ThemeLibrary::instance().importFromFile(result, name, spec))
        {
            safe->showToast("Not a valid .y2kpalette file");
            return;
        }

        safe->spec_ = spec;
        safe->loadSpecIntoUi(spec);
        safe->regenerateAndRefresh();
        safe->applyCurrentSpec();
    });
}

// ==================================================
// show / dismiss（R2/R3）
// ==================================================
void PalettePanel::show()
{
    // 快照当前主题（还原点）
    snapshot_.wasCustom = PinkXP::isCustomThemeActive();
    snapshot_.themeId = (int) PinkXP::getCurrentThemeId();
    snapshot_.spec = PinkXP::getCurrentThemeSpec();

    // 编辑起点 = 当前主题的 spec
    spec_ = PinkXP::getCurrentThemeSpec();

    pickingRole_ = -1;
    compareHeld_ = false;
    svPicker_.setVisible(false);

    loadSpecIntoUi(spec_);
    regenerateAndRefresh();
    updatePanelHeight();

    visible_ = true;
    setVisible(true);
    toFront(false);
    grabKeyboardFocus();

    // 延迟挂载外部关闭监听（与旧 CustomThemePicker 同样的防误触处理）
    juce::Component::SafePointer<PalettePanel> safe(this);
    juce::MessageManager::callAsync([safe]()
    {
        if (safe != nullptr && safe->visible_)
            safe->attachDismissListener();
    });
}

void PalettePanel::dismiss()
{
    // R3：关闭面板默认保留当前效果（spec 已实时应用），仅标记为未命名
    visible_ = false;
    setVisible(false);
    detachDismissListener();
    pickingRole_ = -1;
    svPicker_.setVisible(false);
    if (onClosed) onClosed();
}

bool PalettePanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (namePromptVisible_)   // 命名栏打开时 Esc 只取消命名，不关面板
        {
            closeNamePrompt(false);
            return true;
        }
        dismiss();
        return true;
    }
    return false;
}

void PalettePanel::attachDismissListener()
{
    if (auto* top = getTopLevelComponent())
        top->addMouseListener(dismissListener_.get(),
                              /*wantsEventsForAllNestedChildComponents=*/true);
}

void PalettePanel::detachDismissListener()
{
    if (auto* top = getTopLevelComponent())
        top->removeMouseListener(dismissListener_.get());
}

// ==================================================
// UI 回写（undo/redo/随机/载入主题后同步控件状态）
// ==================================================
void PalettePanel::loadSpecIntoUi(const y2kpal::ThemeSpec& s)
{
    spec_ = s;

    hueRing_->setHue(s.hue);
    hueRing_->setContext(s.vibe, (int) s.brightness);

    modeDarkBtn_.setToggleState(s.brightness == y2kpal::Brightness::Dark, juce::dontSendNotification);
    modeLightBtn_.setToggleState(s.brightness == y2kpal::Brightness::Light, juce::dontSendNotification);

    vibeSlider_.setValue(s.vibe, juce::dontSendNotification);
    tintSlider_.setValue(s.bgTint, juce::dontSendNotification);

    for (int i = 0; i < 5; ++i)
        harmonyBtns_[i].setToggleState(s.harmony == (y2kpal::Harmony) i, juce::dontSendNotification);

    patternCombo_.setSelectedId((int) s.pattern + 1, juce::dontSendNotification);

    for (int i = 0; i < 8; ++i)
        iconBtns_[i].setToggleState(s.titleIcon == (uint8_t) i, juce::dontSendNotification);

    refreshRoleRows();
}

void PalettePanel::updatePanelHeight()
{
    int h = 282;                                    // 标题栏 + 主体区 + 按钮行
    if (advancedOpen_) h += 216;
    if (myThemesOpen_) h += 148;
    h += 6;

    if (getHeight() != h)
        setSize(getWidth(), h);

    if (onHeightChanged)
        onHeightChanged();
}

// ==================================================
// 绘制与布局
// ==================================================
void PalettePanel::paint(juce::Graphics& g)
{
    PinkXP::drawSunken(g, getLocalBounds(), PinkXP::btnFace);
    auto inner = getLocalBounds().reduced(3);
    g.setColour(PinkXP::content);
    g.fillRect(inner);

    // 命名输入栏可见时：输入框四周画 PinkXP 凹陷边框（贴合项目像素风）
    if (namePromptVisible_ && nameEditor_.isVisible())
        PinkXP::drawSunken(g, nameEditor_.getBounds().expanded(3), PinkXP::btnFace);

    // 标题栏
    auto bar = inner.removeFromTop(26);
    g.setColour(PinkXP::sel);
    g.fillRect(bar);
    g.setColour(PinkXP::selInk);
    g.setFont(PinkXP::getFont(12.0f, juce::Font::bold));
    g.drawText("PALETTE STUDIO", bar.reduced(8, 0), juce::Justification::centredLeft, false);
}

void PalettePanel::resized()
{
    auto area = getLocalBounds().reduced(4, 4);

    // 标题栏 + 关闭按钮
    auto bar = area.removeFromTop(26);
    closeBtn_.setBounds(bar.removeFromRight(24).reduced(2));

    // ---- 主体区 ----
    auto main = area.removeFromTop(214);
    {
        // 左列
        auto left = main.removeFromLeft(198);
        modeLabel_.setBounds(left.getX() + 12, left.getY() + 2, 60, 16);
        modeDarkBtn_.setBounds(left.getX() + 12, left.getY() + 20, 56, 20);
        modeLightBtn_.setBounds(left.getX() + 76, left.getY() + 20, 56, 20);

        hueRing_->setBounds(left.getX() + 20, left.getY() + 46, 150, 150);
    }

    // 中列
    auto mid = main.removeFromLeft(202);
    {
        vibeLabel_.setBounds(mid.getX() + 2, mid.getY() + 14, 52, 16);
        vibeSlider_.setBounds(mid.getX() + 56, mid.getY() + 12, 140, 20);

        tintLabel_.setBounds(mid.getX() + 2, mid.getY() + 42, 52, 16);
        tintSlider_.setBounds(mid.getX() + 56, mid.getY() + 40, 140, 20);

        harmonyLabel_.setBounds(mid.getX() + 2, mid.getY() + 70, 62, 16);
        for (int i = 0; i < 5; ++i)
            harmonyBtns_[i].setBounds(mid.getX() + 2 + i * 32, mid.getY() + 88, 28, 20);

        patternLabel_.setBounds(mid.getX() + 2, mid.getY() + 118, 62, 16);
        patternCombo_.setBounds(mid.getX() + 2, mid.getY() + 136, 150, 20);

        iconLabel_.setBounds(mid.getX() + 2, mid.getY() + 164, 40, 16);
        for (int i = 0; i < 4; ++i)
            iconBtns_[i].setBounds(mid.getX() + 44 + i * 22, mid.getY() + 162, 20, 20);
        for (int i = 0; i < 4; ++i)
            iconBtns_[4 + i].setBounds(mid.getX() + 44 + i * 22, mid.getY() + 184, 20, 20);
    }

    // 右列：预览卡 + 徽章
    auto right = main;
    {
        previewCard_.setBounds(right.getX() + 6, right.getY() + 6, right.getWidth() - 20, 148);
        badgeLabel_.setBounds(right.getX() + 6, right.getY() + 160, right.getWidth() - 20, 18);
        noteLabel_.setBounds(right.getX() + 6, right.getY() + 180, right.getWidth() - 20, 16);
    }

    // ---- 按钮行 ----
    auto btnRow = area.removeFromTop(30);
    {
        int x = btnRow.getX() + 8;
        auto place = [&](juce::Component& c, int w)
        {
            c.setBounds(x, btnRow.getY() + 3, w, 24);
            x += w + 4;
        };
        place(advancedToggle_, 80);
        place(randomBtn_, 76);
        place(compareBtn_, 40);
        place(revertBtn_, 66);
        place(saveBtn_, 66);
        myThemesToggle_.setBounds(x, btnRow.getY() + 3, 90, 24);
    }

    // ---- 高级区（P3，折叠）----
    if (advancedOpen_)
    {
        auto adv = area.removeFromTop(216);
        for (int i = 0; i < y2kpal::kRoleCount; ++i)
        {
            roleRows_[(size_t) i]->setVisible(true);
            roleRows_[(size_t) i]->setBounds(adv.getX() + 6, adv.getY() + 20 + i * 27,
                                              adv.getWidth() - 12, 26);
        }
    }
    else
    {
        for (int i = 0; i < y2kpal::kRoleCount; ++i)
            roleRows_[(size_t) i]->setVisible(false);
    }

    // ---- 我的主题区（P4，折叠）----
    myThemesViewport_->setVisible(myThemesOpen_);
    copyCodeBtn_.setVisible(myThemesOpen_);
    pasteCodeBtn_.setVisible(myThemesOpen_);
    exportBtn_.setVisible(myThemesOpen_);
    importBtn_.setVisible(myThemesOpen_);
    myThemesHint_.setVisible(myThemesOpen_);

    if (myThemesOpen_)
    {
        auto my = area.removeFromTop(148);
        myThemesViewport_->setBounds(my.getX() + 6, my.getY() + 18, my.getWidth() - 12, 100);
        myThemesList_.setBounds(0, 0, my.getWidth() - 12 - 12, 0);
        myThemesList_.refreshLayout();

        int x = my.getX() + 6;
        auto place = [&](juce::Component& c, int w)
        {
            c.setBounds(x, my.getY() + 122, w, 20);
            x += w + 4;
        };
        place(copyCodeBtn_, 54);
        place(pasteCodeBtn_, 54);
        place(exportBtn_, 58);
        place(importBtn_, 58);
        myThemesHint_.setBounds(x, my.getY() + 124, my.getWidth() - x - my.getX() - 6, 16);
    }

    // ---- 取色器（覆盖在面板左上角，避开右侧预览卡，R9）----
    svPicker_.setVisible(pickingRole_ >= 0);
    if (pickingRole_ >= 0)
    {
        svPicker_.setTopLeftPosition(8, 32);
        svPicker_.toFront(false);
    }

    // ---- 内嵌命名输入栏（覆盖在底部按钮行上方）----
    if (namePromptVisible_)
    {
        auto row = getLocalBounds().removeFromBottom(32).reduced(10, 5);
        nameOkBtn_.setBounds(row.removeFromRight(60));
        nameCancelBtn_.setBounds(row.removeFromRight(64));
        nameEditor_.setBounds(row.removeFromRight(220));
    }
}
