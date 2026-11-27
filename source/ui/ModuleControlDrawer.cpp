#include "source/ui/ModuleControlDrawer.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"

#include <cmath>
#include <limits>

// ==========================================================
// ModuleControlDrawer —— 模块外部弹出式参数面板（分类标签页 + 卡片化）
// ==========================================================

namespace
{
    constexpr int kPad               = 6;    // 内容区外边距
    constexpr int kCardPad           = 6;    // 卡片内边距
    constexpr int kCardGap           = 6;    // 非标签模式下多卡片之间的间距
    constexpr int kRowH              = 22;   // 一行控件高度（未缩放）
    constexpr int kChoiceLabelH      = 18;   // 按钮网格上方独立标签行高度（未缩放）
    constexpr int kRowGap            = 4;
    constexpr int kStackedLabelGap   = 2;    // 标签与控件上下堆叠时的间距
    constexpr int kHeaderH           = 16;   // 非标签模式下的分组标题高度
    constexpr int kHeaderToCardGap   = 3;    // 分组标题到卡片顶边的间距
    constexpr int kTitleBarH         = 18;   // 顶部迷你标题条高度（未缩放）
    constexpr int kCloseBtnSz        = 13;   // 迷你关闭按钮边长（未缩放）
    constexpr int kLabelGap          = 8;    // 标签与控件列间距
    constexpr int kPairColGap        = 10;   // 双列开关之间的列间距
    constexpr int kPairLblGap        = 6;    // 双列开关内标签与按钮的间距
    constexpr int kPairMinCardInnerW = 248;  // 启用双列开关所需的最小卡片内宽
    constexpr int kScrollBarW        = 10;   // 纵向滚动条宽（未缩放）
    constexpr int kTabH              = 22;   // 分类标签高度（未缩放）
    constexpr int kTabGap            = 2;    // 标签间距
    constexpr int kTabPadX           = 5;    // 标签条左右内缩
    constexpr int kTabPadTop         = 4;    // 标签条顶部呼吸距
    constexpr int kSliderTrackMinW   = 76;   // 滑块轨道最小可用宽度
    constexpr int kSliderTrackIdealW = 112;  // 滑块轨道理想宽度
    constexpr int kChoiceGridPrefCap = 360;  // 均衡按钮网格在常规宽度下的目标上限
    constexpr int kChoiceGridWideCap = 420;  // 高度受限时允许拉宽的网格上限
    constexpr int kShortChoiceSideW  = 236;  // 2~3 项短选择行保持并排单行的最大按钮总宽
    constexpr int kPairPrefMaxW      = 356;  // 优先拉宽配合双列开关的卡片内宽上限
    constexpr int kPrefOuterMinW     = 288;  // 理想外框最小宽（确保常见双列开关与标签舒展）
    constexpr int kPrefOuterMaxW     = 500;  // 理想外框最大宽
    constexpr int kHardOuterMaxW     = 560;  // 拉宽避滚时的外框硬上限
    constexpr int kTabThresholdRows  = 6;    // 可见行数 > 6 且分组 >= 2 时启用分类标签条
    constexpr juce::uint32 kHighlightPulseMs = 900;

    void markNoToy (juce::Component& c)
    {
        c.getProperties().set ("toy.optOut", true);
    }

    // 卡片底色：在任意深浅主题下都与 PinkXP::face 形成细腻分层，且保持高文字对比度
    juce::Colour getCardFillColour() noexcept
    {
        const auto& tokens = PinkXP::getSkinTokens();
        const bool isDark  = PinkXP::face.getPerceivedBrightness() < 0.45f;
        if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
        {
            // Modern Minimal：深色主题（blackPink/voidGrey）清晰微抬升，浅色主题（bubblegum/paperGrey）通透卡片分层
            return isDark ? PinkXP::face.interpolatedWith (juce::Colours::white, 0.048f)
                          : PinkXP::face.interpolatedWith (PinkXP::content, 0.56f);
        }
        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            // Millennium Chrome：拉丝铝机架面板（深色阳极枪灰微提亮，浅色拉丝铝通透分层）
            return isDark ? PinkXP::face.interpolatedWith (juce::Colours::white, 0.085f)
                          : PinkXP::face.interpolatedWith (PinkXP::content, 0.44f);
        }
        if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            // Frosted Glass：深色烟熏黑曜水晶 / 浅色霜冰亚克力清晰分层
            return isDark ? PinkXP::face.interpolatedWith (juce::Colours::white, 0.075f)
                          : PinkXP::face.interpolatedWith (juce::Colours::white, 0.42f);
        }
        if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            // Neon Cyber：深色主题压深暗烟熏底凸显霓虹灯管；浅色主题保持高对比通透卡片
            return isDark ? PinkXP::face.interpolatedWith (juce::Colours::black, 0.24f)
                          : PinkXP::face.interpolatedWith (PinkXP::content, 0.48f);
        }
        if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            // CRT Terminal：0px 荧光终端机箱卡片底色
            return isDark ? PinkXP::content.interpolatedWith (PinkXP::face, 0.24f)
                          : PinkXP::content.interpolatedWith (PinkXP::face, 0.36f);
        }
        return isDark ? PinkXP::face.brighter (0.09f)
                      : PinkXP::face.interpolatedWith (PinkXP::content, 0.34f);
    }

    // 绿幕 / 透明背景模式下 Craft::drawPanelWell 会为仪表画布跳过底板；
    //   主动打开的设置抽屉属于交互弹窗，需按当前 SkinId（tokens.frameStyle）绘制原汁原味的分组卡片。
    void drawKeyedDrawerCard (juce::Graphics& g,
                              juce::Rectangle<int> rect,
                              juce::Colour cardFill,
                              const PinkXP::SkinTokens& tokens)
    {
        if (rect.isEmpty())
            return;

        juce::Graphics::ScopedSaveState ss (g);
        const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;

        switch (tokens.frameStyle)
        {
            case PinkXP::FrameStyle::pixelBevel:
                g.setColour (cardFill);
                g.fillRect (rect);
                g.setColour (PinkXP::shdw.withAlpha (isDark ? 0.62f : 0.48f));
                g.drawRect (rect, 1);
                if (rect.getWidth() >= 4 && rect.getHeight() >= 4)
                {
                    g.setColour (PinkXP::hl.withAlpha (isDark ? 0.16f : 0.65f));
                    g.fillRect (rect.getX() + 1, rect.getY() + 1, rect.getWidth() - 2, 1);
                    g.fillRect (rect.getX() + 1, rect.getY() + 1, 1, rect.getHeight() - 2);
                }
                break;

            case PinkXP::FrameStyle::machinedChrome:
            {
                // 4-stop 拉丝金属折痕渐变（38% 处形成标志性拉丝高光折痕）
                juce::ColourGradient grad (cardFill.interpolatedWith (juce::Colours::white, isDark ? 0.14f : 0.24f),
                                           0.0f, (float) rect.getY(),
                                           cardFill.interpolatedWith (juce::Colours::black, isDark ? 0.12f : 0.08f),
                                           0.0f, (float) rect.getBottom(), false);
                grad.addColour (0.38, cardFill.interpolatedWith (juce::Colours::white, isDark ? 0.05f : 0.10f));
                grad.addColour (0.42, cardFill.interpolatedWith (juce::Colours::black, isDark ? 0.05f : 0.03f));
                g.setGradientFill (grad);
                g.fillRect (rect);
                g.setColour (PinkXP::dark.withAlpha (isDark ? 0.88f : 0.72f));
                g.drawRect (rect, 1);
                if (rect.getWidth() >= 4 && rect.getHeight() >= 4)
                {
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.45f)
                                           .withAlpha (isDark ? 0.28f : 0.72f));
                    g.fillRect (rect.getX() + 1, rect.getY() + 1, rect.getWidth() - 2, 1);
                    g.fillRect (rect.getX() + 1, rect.getY() + 1, 1, rect.getHeight() - 2);
                }
                break;
            }

            case PinkXP::FrameStyle::frostedGlass:
            {
                // Frosted Glass：6px 圆角半透明磨砂亚克力卡片 + 顶部 40% 穹顶反光 + 1px 亮白顶沿
                const auto rf = rect.toFloat();
                const float rad = juce::jmin (tokens.cardRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.5f);
                g.setColour (cardFill);
                g.fillRoundedRectangle (rf, rad);
                if (rf.getHeight() >= 10.0f)
                {
                    auto sheen = rf.reduced (1.5f);
                    sheen.setHeight (sheen.getHeight() * 0.40f);
                    juce::ColourGradient dome (juce::Colours::white.withAlpha (isDark ? 0.14f : 0.34f),
                                               0.0f, sheen.getY(),
                                               juce::Colours::white.withAlpha (0.02f),
                                               0.0f, sheen.getBottom(), false);
                    g.setGradientFill (dome);
                    g.fillRoundedRectangle (sheen, juce::jmax (1.0f, rad - 1.5f));
                }
                g.setColour (PinkXP::dark.withAlpha (isDark ? 0.40f : 0.32f));
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                if (rect.getWidth() >= 10 && rect.getHeight() >= 8)
                {
                    const int inset = juce::jmax (3, (int) std::round (rad * 0.65f));
                    g.setColour (juce::Colours::white.withAlpha (isDark ? 0.28f : 0.72f));
                    g.fillRect (rect.getX() + inset, rect.getY() + 1, juce::jmax (1, rect.getWidth() - inset * 2), 1);
                }
                break;
            }

            case PinkXP::FrameStyle::neonTube:
            {
                // Neon Cyber：2px 科技微角暗烟熏 HUD 卡片 + 双通道发光霓虹灯管边 + 四角微光刻标
                const auto rf = rect.toFloat();
                const float rad = juce::jmin (2.0f, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.25f);
                g.setColour (cardFill);
                g.fillRoundedRectangle (rf, rad);
                g.setColour (PinkXP::sel.withAlpha (0.30f));
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 2.0f);
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.28f).withAlpha (0.86f));
                g.drawRoundedRectangle (rf.reduced (1.0f), juce::jmax (0.5f, rad - 0.5f), 1.0f);
                PinkXP::drawPixelCorners (g, rect.reduced (1),
                                          PinkXP::sel.interpolatedWith (juce::Colours::white, 0.45f), 4, 1);
                break;
            }

            case PinkXP::FrameStyle::crtPhosphor:
                // CRT Terminal：0px 方形荧光终端卡片 + 1px 锐利荧光边框 + 内层微荧光细线
                g.setColour (cardFill);
                g.fillRect (rect);
                g.setColour (PinkXP::sel.withAlpha (0.76f));
                g.drawRect (rect, 1);
                if (rect.getWidth() >= 6 && rect.getHeight() >= 6)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.18f));
                    g.drawRect (rect.reduced (1), 1);
                }
                break;

            case PinkXP::FrameStyle::modernStudio:
            default:
            {
                const auto rf = rect.toFloat();
                const float rad = juce::jmin (tokens.cardRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.5f);
                g.setColour (cardFill);
                g.fillRoundedRectangle (rf, rad);
                g.setColour (PinkXP::skinHairlineOn (PinkXP::face, isDark ? 0.16f : 0.20f));
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                break;
            }
        }
    }

    // 与 PinkXPLookAndFeel::drawButtonText 保持完全一致的字号与测宽口径
    int measureButtonWidth (const juce::String& rawText, int rowH)
    {
        const juce::String shown = y2k::tr (rawText);
        const bool symbolsOnly = PinkXP::SymbolGlyphs::isSymbolsOnly (shown);
        const float nominal = symbolsOnly ? 14.0f : 12.0f;
        const auto probe = symbolsOnly
                               ? PinkXP::SymbolGlyphs::iconFont (nominal, juce::Font::bold)
                               : PinkXP::SymbolGlyphs::buttonFont (shown, nominal, juce::Font::bold);
        const float maxH = (float) juce::jmax (1, rowH);
        const float fontSize = (probe.getHeight() > maxH && probe.getHeight() > 0.0f)
                                   ? nominal * maxH / probe.getHeight()
                                   : nominal;
        const auto font = symbolsOnly
                              ? PinkXP::SymbolGlyphs::iconFont (fontSize, juce::Font::bold)
                              : PinkXP::SymbolGlyphs::buttonFont (shown, fontSize, juce::Font::bold);
        const float textW = PinkXP::SymbolGlyphs::getTextWidth (shown, font);
        // 2px 左右边框 + 1px 按下位移 + 舒适左右留白
        const int chromePad = juce::jmax (14, PinkXP::ui (16));
        return juce::jmax (PinkXP::ui (30), (int) std::ceil (textW) + chromePad);
    }

    // 与 paintTabStrip 完全一致的标签字体与测宽口径
    juce::Font makeTabFont (const juce::String& shown, int tabH)
    {
        const float nominal = 10.5f;
        const auto probe = PinkXP::SymbolGlyphs::buttonFont (shown, nominal, juce::Font::bold);
        const float maxH = (float) juce::jmax (1, tabH - 4);
        const float fontSize = (probe.getHeight() > maxH && probe.getHeight() > 0.0f)
                                   ? nominal * maxH / probe.getHeight()
                                   : nominal;
        return PinkXP::SymbolGlyphs::buttonFont (shown, fontSize, juce::Font::bold);
    }

    int measureTabWidth (const juce::String& rawGroup, int tabH)
    {
        const juce::String shown = y2k::tr (rawGroup);
        const auto font = makeTabFont (shown, tabH);
        const float textW = PinkXP::SymbolGlyphs::getTextWidth (shown, font);
        const int hPad = juce::jmax (14, PinkXP::ui (16));
        return juce::jmax (PinkXP::ui (42), (int) std::ceil (textW) + hPad);
    }

    // 与 Inner::paint 绘制参数标签完全一致的字体测宽
    int measureLabelWidth (const juce::String& rawLabel, int rowH)
    {
        const juce::String shown = y2k::tr (rawLabel);
        const auto font = PinkXP::fitFontToHeight (PinkXP::getFont (11.0f, juce::Font::plain),
                                                   (float) juce::jmax (1, rowH));
        return (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown, font)) + 4;
    }

    // 与 Inner::paint 绘制分组标题完全一致的字体测宽
    int measureHeaderWidth (const juce::String& rawGroup, int headH)
    {
        const juce::String shown = y2k::tr (rawGroup);
        const auto font = PinkXP::fitFontToHeight (PinkXP::getFont (10.0f, juce::Font::bold),
                                                   (float) juce::jmax (1, headH));
        return (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown, font)) + 4;
    }

    // 顶部迷你标题栏所需的最小外框宽度（左图标 + 标题文字 + 右关闭按钮）
    int measureTitleMinOuterWidth (const juce::String& rawTitle)
    {
        const juce::String shown = y2k::tr (rawTitle);
        const auto font = PinkXP::getFont (10.5f, juce::Font::bold);
        const int textW = (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown, font));
        return 4 + 25 + textW + PinkXP::ui (kCloseBtnSz) + 14;
    }

    // 与 PinkXPLookAndFeel::drawLabel 保持完全一致的滑块读数框测宽
    int measureSliderBoxWidth (juce::Slider& slider, const ParamDef& def, int rowH)
    {
        const int textAreaH = juce::jmax (1, rowH - 2);
        const auto font = PinkXP::fitFontToHeight (PinkXP::getFont (11.0f, juce::Font::bold),
                                                   (float) textAreaH + 1.0f);
        float maxW = 0.0f;
        auto sample = [&] (double v)
        {
            const double clamped = juce::jlimit (def.minV, def.maxV, v);
            const auto shown = y2k::tr (slider.getTextFromValue (clamped));
            maxW = juce::jmax (maxW, PinkXP::SymbolGlyphs::getTextWidth (shown, font));
            // 比例字体里 '1' 偏窄，按同位宽替换成 '8' 兜底，避免拖动到任意中间值时被截断
            const auto wideDigits = shown.replaceCharacter ('1', '8');
            if (wideDigits != shown)
                maxW = juce::jmax (maxW, PinkXP::SymbolGlyphs::getTextWidth (wideDigits, font));
        };

        sample (def.minV);
        sample (def.maxV);
        if (def.step > 0.0)
        {
            sample (def.minV + def.step);
            sample (def.maxV - def.step);
            const double mid = def.minV + std::floor ((def.maxV - def.minV) * 0.5 / def.step) * def.step;
            sample (mid);
            sample (mid + def.step);
        }
        sample (slider.getValue());

        // JUCE Label 默认左右边界各 5px（共 10px）+ 6px 呼吸余量
        return juce::jmax (PinkXP::ui (52), (int) std::ceil (maxW) + 16);
    }

    // 注水式宽度均衡分配：保证每项不小于 minWidths[i] 且总宽严丝合缝填满 availW
    std::vector<int> distributeBalancedWidths (const std::vector<int>& minWidths, int gap, int availW)
    {
        const size_t n = minWidths.size();
        std::vector<int> widths (n, 1);
        if (n == 0)
            return widths;

        const int totalGaps = (int) (n - 1) * gap;
        const int budget = juce::jmax ((int) n, availW - totalGaps);

        int sumMin = 0;
        for (size_t i = 0; i < n; ++i)
        {
            widths[i] = juce::jmax (1, minWidths[i]);
            sumMin += widths[i];
        }

        if (sumMin > budget)
        {
            // 极端超窄兜底：按比例缩回 budget 内，避免越出容器
            int used = 0;
            for (size_t i = 0; i < n; ++i)
            {
                widths[i] = (i + 1 == n)
                                ? juce::jmax (1, budget - used)
                                : juce::jmax (1, (widths[i] * budget) / juce::jmax (1, sumMin));
                used += widths[i];
            }
            return widths;
        }

        int rem = budget - sumMin;
        while (rem > 0)
        {
            int lowest = std::numeric_limits<int>::max();
            int nextLowest = std::numeric_limits<int>::max();
            for (int w : widths)
            {
                if (w < lowest)
                {
                    nextLowest = lowest;
                    lowest = w;
                }
                else if (w > lowest && w < nextLowest)
                {
                    nextLowest = w;
                }
            }

            int countAtLowest = 0;
            for (int w : widths)
                if (w == lowest)
                    ++countAtLowest;

            if (countAtLowest <= 0)
                break;

            if (nextLowest == std::numeric_limits<int>::max())
            {
                const int addEach = rem / countAtLowest;
                int extra = rem % countAtLowest;
                for (int& w : widths)
                {
                    w += addEach + (extra > 0 ? 1 : 0);
                    if (extra > 0) --extra;
                }
                rem = 0;
            }
            else
            {
                const int delta = nextLowest - lowest;
                const int maxStep = rem / countAtLowest;
                if (maxStep >= delta)
                {
                    for (int& w : widths)
                        if (w == lowest)
                            w += delta;
                    rem -= delta * countAtLowest;
                }
                else
                {
                    int extra = rem % countAtLowest;
                    for (int& w : widths)
                    {
                        if (w == lowest)
                        {
                            w += maxStep + (extra > 0 ? 1 : 0);
                            if (extra > 0) --extra;
                        }
                    }
                    rem = 0;
                }
            }
        }
        return widths;
    }

    void drawSingleLineTextSafe (juce::Graphics& g,
                                 const juce::String& shown,
                                 juce::Rectangle<int> area,
                                 juce::Justification justification,
                                 const juce::Font& font)
    {
        const auto runs = PinkXP::SymbolGlyphs::splitRuns (shown);
        const bool needsRunLayout = runs.size() > 1 || (runs.size() == 1 && runs[0].symbol);
        if (needsRunLayout)
        {
            PinkXP::SymbolGlyphs::drawText (g, shown, area, justification, font);
        }
        else
        {
            g.setFont (font);
            g.drawText (shown,
                        area.toFloat().translated (0.0f, PinkXP::opticalNudge (font)),
                        justification, true);
        }
    }
}

int ModuleControlDrawer::headerBarHeight() noexcept
{
    return PinkXP::ui (kTitleBarH);
}

void ModuleControlDrawer::Inner::paint (juce::Graphics& g)
{
    const auto& tokens = PinkXP::getSkinTokens();
    const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;
    const auto cardFill = getCardFillColour();
    const auto labelInk = PinkXP::readableInkOn (cardFill);
    juce::Colour headerInk;
    if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
    {
        // Modern Minimal：克制演播室标题层级，任意深浅主题皆保持高可读对比
        headerInk = PinkXP::readableInkOn (PinkXP::face).withAlpha (isDark ? 0.88f : 0.82f);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
    {
        // Millennium Chrome：拉丝铝面板丝印高对比分组标题
        headerInk = PinkXP::readableInkOn (PinkXP::face).withAlpha (isDark ? 0.92f : 0.88f);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
    {
        // Frosted Glass：清透磨砂高可读标题
        headerInk = PinkXP::readableInkOn (PinkXP::face).withAlpha (isDark ? 0.92f : 0.86f);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
    {
        // Neon Cyber：暗主题发光霓虹抬头 / 浅主题高对比可读墨色
        headerInk = isDark ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.42f)
                           : PinkXP::readableInkOn (PinkXP::face);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
    {
        // CRT Terminal：暗主题纯净荧光抬头 / 浅主题高对比可读墨色
        headerInk = isDark ? PinkXP::sel : PinkXP::readableInkOn (PinkXP::face);
    }
    else
    {
        headerInk = isDark ? PinkXP::pink300 : PinkXP::pink600.withAlpha (0.94f);
        if (headerInk.getPerceivedBrightness() - PinkXP::face.getPerceivedBrightness() < 0.12f
            && PinkXP::face.getPerceivedBrightness() - headerInk.getPerceivedBrightness() < 0.12f)
        {
            headerInk = PinkXP::readableInkOn (PinkXP::face);
        }
    }

    // 1. 非标签模式下的分组抬头（带主题强调竖条 + 风格化延伸细线）
    for (const auto& h : drawer.headers)
    {
        const juce::String shown = y2k::tr (h.text);
        const auto font = PinkXP::fitFontToHeight (PinkXP::getFont (10.0f, juce::Font::bold),
                                                   (float) juce::jmax (1, h.rect.getHeight()));

        const int barW = 3;
        const int barH = juce::jmax (8, h.rect.getHeight() - 4);
        const int barY = h.rect.getY() + (h.rect.getHeight() - barH) / 2;
        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            // CNC 金属嵌条 + 顶沿反光点 + 右侧微刻影
            g.setColour (PinkXP::sel);
            g.fillRect (h.rect.getX(), barY, barW, barH);
            g.setColour (juce::Colours::white.withAlpha (0.50f));
            g.fillRect (h.rect.getX(), barY, barW, 1);
            g.setColour (PinkXP::dark.withAlpha (isDark ? 0.65f : 0.42f));
            g.fillRect (h.rect.getX() + barW, barY, 1, barH);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            // 晶透圆胶囊竖条 + 顶部亮白玻璃反光点
            g.setColour (PinkXP::sel.withAlpha (0.88f));
            g.fillRoundedRectangle ((float) h.rect.getX(), (float) barY, (float) barW, (float) barH, 1.5f);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillRoundedRectangle ((float) h.rect.getX() + 0.5f, (float) barY + 0.5f,
                                    (float) barW - 1.0f, juce::jmax (2.0f, (float) barH * 0.38f), 1.0f);
        }
        else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            // 激光灯管竖条（外柔光 + 白芯灯丝）
            g.setColour (PinkXP::sel.withAlpha (0.28f));
            g.fillRect (h.rect.getX() - 1, barY - 1, barW + 2, barH + 2);
            g.setColour (PinkXP::sel);
            g.fillRect (h.rect.getX(), barY, barW, barH);
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.45f));
            g.fillRect (h.rect.getX() + 1, barY + 1, 1, juce::jmax (2, barH - 2));
        }
        else
        {
            g.setColour (PinkXP::sel);
            if (! tokens.pixelSnapped && tokens.controlRadius > 0.5f)
                g.fillRoundedRectangle ((float) h.rect.getX(), (float) barY, (float) barW, (float) barH, 1.5f);
            else
                g.fillRect (h.rect.getX(), barY, barW, barH);
        }

        const int textX = h.rect.getX() + barW + 5;
        const int textW = juce::jmin (h.rect.getRight() - textX,
                                      (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown, font)) + 4);
        const juce::Rectangle<int> textRect (textX, h.rect.getY(), juce::jmax (1, textW), h.rect.getHeight());

        g.setColour (headerInk);
        drawSingleLineTextSafe (g, shown, textRect, juce::Justification::centredLeft, font);

        const int ruleX = textRect.getRight() + 5;
        if (ruleX < h.rect.getRight() - 2)
        {
            const int ruleW = h.rect.getRight() - ruleX;
            const int ruleY = h.rect.getCentreY();
            switch (tokens.frameStyle)
            {
                case PinkXP::FrameStyle::pixelBevel:
                    g.setColour (PinkXP::shdw.withAlpha (isDark ? 0.50f : 0.38f));
                    g.fillRect (ruleX, ruleY, ruleW, 1);
                    g.setColour (PinkXP::hl.withAlpha (isDark ? 0.14f : 0.55f));
                    g.fillRect (ruleX, ruleY + 1, ruleW, 1);
                    break;

                case PinkXP::FrameStyle::machinedChrome:
                    // 2px 金属机加工蚀刻槽（1px 暗槽 + 1px 亮铬反光沿）
                    g.setColour (PinkXP::dark.withAlpha (isDark ? 0.68f : 0.46f));
                    g.fillRect (ruleX, ruleY, ruleW, 1);
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.48f)
                                           .withAlpha (isDark ? 0.26f : 0.72f));
                    g.fillRect (ruleX, ruleY + 1, ruleW, 1);
                    break;

                case PinkXP::FrameStyle::frostedGlass:
                    // 磨砂玻璃双半透明细线（暗折射 + 白高光）
                    g.setColour (PinkXP::dark.withAlpha (isDark ? 0.28f : 0.14f));
                    g.fillRect (ruleX, ruleY, ruleW, 1);
                    g.setColour (juce::Colours::white.withAlpha (isDark ? 0.14f : 0.62f));
                    g.fillRect (ruleX, ruleY + 1, ruleW, 1);
                    break;

                case PinkXP::FrameStyle::neonTube:
                    // 激光双通道分隔线（亮激光芯 + 柔光底晕 + 起始端微光点）
                    g.setColour (PinkXP::sel.withAlpha (0.65f));
                    g.fillRect (ruleX, ruleY, ruleW, 1);
                    g.setColour (PinkXP::sel.withAlpha (0.20f));
                    g.fillRect (ruleX, ruleY + 1, ruleW, 1);
                    g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.45f).withAlpha (0.88f));
                    g.fillRect (ruleX, ruleY, juce::jmin (4, ruleW), 1);
                    break;

                case PinkXP::FrameStyle::crtPhosphor:
                    // 1px 锐利荧光终端分隔线 + 末端竖标
                    g.setColour (PinkXP::sel.withAlpha (0.62f));
                    g.fillRect (ruleX, ruleY, ruleW, 1);
                    if (ruleW >= 4)
                        g.fillRect (ruleX + ruleW - 1, ruleY - 1, 1, 3);
                    break;

                case PinkXP::FrameStyle::modernStudio:
                default:
                    g.setColour (PinkXP::skinHairlineOn (PinkXP::face, isDark ? 0.14f : 0.16f));
                    g.fillRect (ruleX, ruleY, ruleW, 1);
                    break;
            }
        }
    }

    // 2. 分组卡片（全风格统一经由 Craft::drawPanelWell(..., WellKind::card)；键控模式下仍忠实遵循当前 SkinId）
    for (const auto& card : drawer.cards_)
    {
        if (card.rect.isEmpty())
            continue;

        if (tokens.isChromaKey)
            drawKeyedDrawerCard (g, card.rect, cardFill, tokens);
        else
            PinkXP::Craft::drawPanelWell (g, card.rect, cardFill, PinkXP::WellKind::card);
    }

    // 3. 双列开关之间的微弱中缝细线
    if (! drawer.pairDividers_.empty())
    {
        for (const auto& div : drawer.pairDividers_)
        {
            switch (tokens.frameStyle)
            {
                case PinkXP::FrameStyle::pixelBevel:
                    g.setColour (PinkXP::shdw.withAlpha (isDark ? 0.34f : 0.26f));
                    g.fillRect (div);
                    g.setColour (PinkXP::hl.withAlpha (isDark ? 0.12f : 0.45f));
                    g.fillRect (div.getX() + 1, div.getY(), 1, div.getHeight());
                    break;

                case PinkXP::FrameStyle::machinedChrome:
                    // 2px 金属机加工中缝（1px 暗刻槽 + 1px 亮铬反光沿）
                    g.setColour (PinkXP::dark.withAlpha (isDark ? 0.56f : 0.38f));
                    g.fillRect (div);
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.48f)
                                           .withAlpha (isDark ? 0.22f : 0.62f));
                    g.fillRect (div.getX() + 1, div.getY(), 1, div.getHeight());
                    break;

                case PinkXP::FrameStyle::frostedGlass:
                    g.setColour (PinkXP::dark.withAlpha (isDark ? 0.24f : 0.12f));
                    g.fillRect (div);
                    g.setColour (juce::Colours::white.withAlpha (isDark ? 0.12f : 0.52f));
                    g.fillRect (div.getX() + 1, div.getY(), 1, div.getHeight());
                    break;

                case PinkXP::FrameStyle::neonTube:
                    g.setColour (PinkXP::sel.withAlpha (0.48f));
                    g.fillRect (div);
                    g.setColour (PinkXP::sel.withAlpha (0.16f));
                    g.fillRect (div.getX() + 1, div.getY(), 1, div.getHeight());
                    break;

                case PinkXP::FrameStyle::crtPhosphor:
                    g.setColour (PinkXP::sel.withAlpha (0.50f));
                    g.fillRect (div);
                    break;

                case PinkXP::FrameStyle::modernStudio:
                default:
                    g.setColour (PinkXP::skinHairlineOn (cardFill, isDark ? 0.13f : 0.15f));
                    g.fillRect (div);
                    break;
            }
        }
    }

    // 4. 当前页各可见参数标签
    for (const auto& r : drawer.rows)
    {
        if (! r->shownOnActivePage || r->labelRect.isEmpty())
            continue;

        const auto font = PinkXP::fitFontToHeight (PinkXP::getFont (11.0f, juce::Font::plain),
                                                   (float) juce::jmax (1, r->labelRect.getHeight()));
        g.setColour (labelInk);
        drawSingleLineTextSafe (g, y2k::tr (r->def.label), r->labelRect, juce::Justification::centredLeft, font);
    }
}

void ModuleControlDrawer::Inner::paintOverChildren (juce::Graphics& g)
{
    const auto& tokens = PinkXP::getSkinTokens();
    const auto accent  = PinkXP::sel;

    // 1. 激活状态微强调底条：
    //    · Pixel XP 下 PinkXPLookAndFeel 画 Win95 按下抖动底纹，此处叠 0px 锐利强调底条；
    //    · 其余风格已由 Craft::drawControlChrome(..., ControlState::active) 自带原生激活标记，
    //      不再叠加生硬直角方条，避免盖住 Modern/Glass 圆胶囊或 Neon/CRT 边框。
    if (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel)
    {
        for (const auto& r : drawer.rows)
        {
            if (! r->shownOnActivePage)
                continue;

            if (r->def.kind == ParamDef::Kind::choice || r->def.kind == ParamDef::Kind::toggle)
            {
                for (const auto& b : r->btns)
                {
                    if (b != nullptr && b->isVisible() && b->getToggleState())
                    {
                        const auto bb = b->getBounds();
                        if (bb.getWidth() >= 10 && bb.getHeight() >= 8)
                        {
                            g.setColour (accent.withAlpha (0.92f));
                            g.fillRect (bb.getX() + 3, bb.getBottom() - 3, bb.getWidth() - 6, 2);
                        }
                    }
                }
            }
        }
    }

    // 2. 撤回 / 重做自动定位柔光脉冲（~900ms 自动消退；圆角与质感完全跟随 SkinTokens）
    if (drawer.highlightedParamId_.isNotEmpty() && drawer.highlightAlpha_ > 0.01f)
    {
        for (const auto& r : drawer.rows)
        {
            if (r->shownOnActivePage && r->def.id == drawer.highlightedParamId_ && ! r->highlightRect.isEmpty())
            {
                const float a   = juce::jlimit (0.0f, 1.0f, drawer.highlightAlpha_);
                const auto  hr  = r->highlightRect.toFloat();
                const float rad = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                                   || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                                    ? tokens.controlRadius
                                    : 0.0f;

                if (rad > 0.5f)
                {
                    g.setColour (accent.withAlpha (0.14f * a));
                    g.fillRoundedRectangle (hr, rad);

                    if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass && hr.getHeight() >= 8.0f)
                    {
                        auto dome = hr.reduced (1.5f);
                        dome.setHeight (dome.getHeight() * 0.40f);
                        g.setColour (juce::Colours::white.withAlpha (0.18f * a));
                        g.fillRoundedRectangle (dome, juce::jmax (1.0f, rad - 1.5f));
                    }

                    g.setColour (accent.withAlpha ((tokens.frameStyle == PinkXP::FrameStyle::frostedGlass ? 0.36f : 0.28f) * a));
                    g.drawRoundedRectangle (hr.expanded (1.0f), rad + 1.0f, 1.0f);

                    g.setColour ((tokens.frameStyle == PinkXP::FrameStyle::frostedGlass
                                      ? accent.interpolatedWith (juce::Colours::white, 0.26f)
                                      : accent).withAlpha (0.92f * a));
                    g.drawRoundedRectangle (hr.reduced (0.5f), rad, 1.75f);
                }
                else
                {
                    // 0-2px 锐利脉冲（Pixel XP / Chrome / Neon / CRT）
                    g.setColour (accent.withAlpha (0.16f * a));
                    g.fillRect (r->highlightRect);

                    if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                    {
                        // Neon Cyber：耀眼双通道霓虹脉冲 + 4px 发光角标
                        const float nRad = juce::jmin (2.0f, juce::jmin (hr.getWidth(), hr.getHeight()) * 0.25f);
                        g.setColour (accent.withAlpha (0.34f * a));
                        g.drawRoundedRectangle (hr.expanded (1.5f), nRad + 1.0f, 2.0f);
                        g.setColour (accent.interpolatedWith (juce::Colours::white, 0.48f).withAlpha (0.96f * a));
                        g.drawRoundedRectangle (hr.reduced (0.5f), nRad, 1.5f);
                        PinkXP::drawPixelCorners (g, r->highlightRect,
                                                  accent.interpolatedWith (juce::Colours::white, 0.65f).withAlpha (a), 4, 1);
                    }
                    else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                    {
                        // CRT Terminal：0px 荧光终端脉冲 + 锐利角标
                        g.setColour (accent.withAlpha (0.36f * a));
                        g.drawRect (r->highlightRect.expanded (1), 1);
                        g.setColour (accent.withAlpha (0.96f * a));
                        g.drawRect (r->highlightRect, 1);
                        PinkXP::drawPixelCorners (g, r->highlightRect, accent.withAlpha (a), 4, 1);
                    }
                    else if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
                    {
                        // Millennium Chrome：CNC 金属高光定位脉冲（暗槽外沿 + 主题金属边 + 顶/左镜面反光）
                        g.setColour (PinkXP::dark.withAlpha (0.42f * a));
                        g.drawRect (r->highlightRect.expanded (1), 1);
                        g.setColour (accent.withAlpha (0.95f * a));
                        g.drawRect (r->highlightRect, 2);
                        if (r->highlightRect.getWidth() >= 6 && r->highlightRect.getHeight() >= 6)
                        {
                            g.setColour (accent.interpolatedWith (juce::Colours::white, 0.58f).withAlpha (0.85f * a));
                            g.fillRect (r->highlightRect.getX() + 1, r->highlightRect.getY() + 1,
                                        r->highlightRect.getWidth() - 2, 1);
                        }
                    }
                    else
                    {
                        g.setColour (PinkXP::pink400.withAlpha (0.38f * a));
                        g.drawRect (r->highlightRect.expanded (1), 1);
                        g.setColour (accent.withAlpha (0.95f * a));
                        g.drawRect (r->highlightRect, 2);
                    }
                }
                break;
            }
        }
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
    viewport.setScrollBarThickness (PinkXP::ui (kScrollBarW));
    viewport.setSingleStepSizes (PinkXP::ui (16), PinkXP::ui (kRowH + kRowGap));
    addAndMakeVisible (viewport);

    // 面板本身吃掉鼠标：不穿透到底层模块或画布，并支持 Esc 快捷收起
    setInterceptsMouseClicks (true, true);
    setWantsKeyboardFocus (true);
    setOpaque (true);

    // 挂 GL 安全位图缓存：当外部弹出面板覆盖在 60fps 的 GL 模块/Milkdrop 上方时，
    //   避免每帧重绘所有滑块和按钮（桌面浮动态下该缓存会自动透传，零额外开销）。
    y2k::ui::setGlSafeImageCache (*this, true);

    themeSubToken_ = PinkXP::subscribeThemeChanged ([this]()
    {
        refreshThemeColours();
    });

    rebuild();
}

ModuleControlDrawer::~ModuleControlDrawer()
{
    stopTimer();
    detachSliderCallbacks();
    if (activeDragRow_ >= 0)
    {
        activeDragRow_ = -1;
        dragHadValueChange_ = false;
        if (owner.onParamGestureEnded)
            owner.onParamGestureEnded (owner);
    }

    if (themeSubToken_ >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken_);
        themeSubToken_ = -1;
    }
    setDesktopFloating (false, nullptr);
}

void ModuleControlDrawer::refreshThemeColours()
{
    const auto textInk = PinkXP::readableInkOn (getCardFillColour());
    for (auto& r : rows)
    {
        if (r->slider != nullptr)
            r->slider->setColour (juce::Slider::textBoxTextColourId, textInk);
    }

    viewport.setScrollBarThickness (PinkXP::ui (kScrollBarW));
    viewport.setSingleStepSizes (PinkXP::ui (16), PinkXP::ui (kRowH + kRowGap));
    layoutDirty_ = true;

    sendLookAndFeelChange();

    // 切换语言 / 视觉风格（换字体）会直接改变标签、按钮和数值框宽度：
    //   面板若当前正打开，立即通知宿主重新匹配外框宽高与内部排布。
    if (! refreshingLayout_ && ! syncing && owner.isControlPanelOpen())
    {
        const juce::ScopedValueSetter<bool> guard (refreshingLayout_, true);
        owner.refreshControlPanel();
    }
    else if (getWidth() > 0 && getHeight() > 0)
    {
        applyLayout();
    }

    repaint();
    inner.repaint();
}

void ModuleControlDrawer::updateTimerState()
{
    const bool needHighlight = highlightedParamId_.isNotEmpty() && highlightAlpha_ > 0.0f && isVisible();
    if (needHighlight)
        startTimer (33);
    else if (desktopFloating_)
        startTimer (120);
    else
        stopTimer();
}

void ModuleControlDrawer::setDesktopFloating (bool shouldFloat, juce::Component* mainWindow)
{
    if (shouldFloat)
    {
        floatingMainWindow_ = mainWindow;
        floatingMainWindowBounds_ = (mainWindow != nullptr) ? mainWindow->getScreenBounds()
                                                            : juce::Rectangle<int>();

        if (desktopFloating_)
        {
            updateTimerState();
            return;
        }

        const bool wasVisible = isVisible();
        if (auto* p = getParentComponent())
            p->removeChildComponent (this);

        // 脱离父链前钉住当前 PinkXP LookAndFeel，避免回退到系统默认外观
        setLookAndFeel (&owner.getLookAndFeel());
        lookAndFeelPinned_ = true;

        desktopFloating_ = true;
        setOpaque (true);
        setAlwaysOnTop (true);
        addToDesktop (juce::ComponentPeer::windowIsTemporary);
        updateTimerState();

        if (wasVisible)
            setVisible (true);
        return;
    }

    floatingMainWindow_ = nullptr;
    floatingMainWindowBounds_ = {};

    if (! desktopFloating_)
    {
        updateTimerState();
        return;
    }

    desktopFloating_ = false;
    removeFromDesktop();
    setAlwaysOnTop (false);

    if (lookAndFeelPinned_)
    {
        setLookAndFeel (nullptr);
        lookAndFeelPinned_ = false;
    }
    updateTimerState();
}

void ModuleControlDrawer::timerCallback()
{
    if (highlightedParamId_.isNotEmpty())
    {
        const juce::uint32 elapsed = juce::Time::getMillisecondCounter() - highlightStartMs_;
        if (elapsed >= kHighlightPulseMs)
        {
            highlightedParamId_.clear();
            highlightAlpha_ = 0.0f;
            inner.repaint();
            updateTimerState();
        }
        else
        {
            const float t = (float) elapsed / (float) kHighlightPulseMs;
            highlightAlpha_ = (t < 0.25f) ? 1.0f : std::pow (1.0f - (t - 0.25f) / 0.75f, 1.6f);
            inner.repaint();
        }
    }

    if (! desktopFloating_)
    {
        if (highlightedParamId_.isEmpty())
            stopTimer();
        return;
    }

    auto* hostWin = floatingMainWindow_.getComponent();
    if (hostWin == nullptr || ! hostWin->isVisible() || ! owner.isVisible())
    {
        finishActiveSliderDrag (true);
        owner.setControlPanelOpen (false);
        return;
    }

    const auto cur = hostWin->getScreenBounds();
    if (! floatingMainWindowBounds_.isEmpty() && cur != floatingMainWindowBounds_)
    {
        finishActiveSliderDrag (true);
        owner.setControlPanelOpen (false);
    }
}

juce::Rectangle<int> ModuleControlDrawer::getHeaderBarBounds() const
{
    return { 2, 2, juce::jmax (0, getWidth() - 4), headerBarHeight() };
}

juce::Rectangle<int> ModuleControlDrawer::getCloseButtonBounds() const
{
    const auto tb = getHeaderBarBounds();
    const int s = PinkXP::ui (kCloseBtnSz);
    const int y = tb.getY() + (tb.getHeight() - s) / 2;
    return { tb.getRight() - 3 - s, y, s, s };
}

int ModuleControlDrawer::hitTestTabIndex (juce::Point<int> pos) const noexcept
{
    if (! useTabs_)
        return -1;
    for (const auto& t : tabStripLayout_.tabs)
        if (t.bounds.contains (pos))
            return t.groupIndex;
    return -1;
}

void ModuleControlDrawer::detachSliderCallbacks()
{
    for (auto& r : rows)
    {
        if (r != nullptr && r->slider != nullptr)
        {
            r->slider->onDragStart   = nullptr;
            r->slider->onValueChange = nullptr;
            r->slider->onDragEnd     = nullptr;
        }
    }
}

void ModuleControlDrawer::finishActiveSliderDrag (bool commitIfChanged)
{
    if (activeDragRow_ < 0)
        return;

    const int  rowIdx  = activeDragRow_;
    const bool changed = dragHadValueChange_;
    activeDragRow_      = -1;
    dragHadValueChange_ = false;

    if (commitIfChanged && changed && rowIdx < (int) rows.size())
    {
        owner.setLastChangedParamCoalesceKey ({});
        owner.setLastChangedParamFocusHint ("drawer:" + rows[(size_t) rowIdx]->def.id);
        if (owner.onParamChangedByUser)
            owner.onParamChangedByUser (owner);
        owner.setLastChangedParamCoalesceKey ({});
        owner.setLastChangedParamFocusHint ({});
    }

    if (owner.onParamGestureEnded)
        owner.onParamGestureEnded (owner);
}

void ModuleControlDrawer::visibilityChanged()
{
    if (! isVisible())
    {
        if (activeDragRow_ >= 0)
            finishActiveSliderDrag (true);
        if (highlightedParamId_.isNotEmpty())
        {
            highlightedParamId_.clear();
            highlightAlpha_ = 0.0f;
        }
        hoveredTabIndex_ = -1;
    }
    updateTimerState();
}

void ModuleControlDrawer::rebuild()
{
    finishActiveSliderDrag (true);
    detachSliderCallbacks();
    rows.clear();
    groups_.clear();
    headers.clear();
    cards_.clear();
    pairDividers_.clear();
    inner.removeAllChildren();

    std::vector<ParamDef> defs;
    owner.buildParamDefs (defs);

    const auto textInk = PinkXP::readableInkOn (getCardFillColour());

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
                s->setTextBoxStyle (juce::Slider::TextBoxRight, false, PinkXP::ui (68), PinkXP::ui (kRowH));
                s->setTextValueSuffix (row->def.suffix);
                s->setColour (juce::Slider::textBoxTextColourId, textInk);
                s->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
                s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                s->onDragStart = [this, idx]
                {
                    if (syncing || idx >= rows.size())
                        return;
                    if (activeDragRow_ >= 0 && activeDragRow_ != (int) idx)
                        finishActiveSliderDrag (true);
                    if (activeDragRow_ < 0)
                    {
                        activeDragRow_ = (int) idx;
                        dragHadValueChange_ = false;
                        if (owner.onWillChangeParamByUser)
                            owner.onWillChangeParamByUser (owner);
                    }
                };
                s->onValueChange = [this, idx, sp = s.get()] { onParamChanged (idx, sp->getValue()); };
                s->onDragEnd     = [this, idx]
                {
                    if (activeDragRow_ == (int) idx)
                        finishActiveSliderDrag (true);
                    else if (owner.onParamGestureEnded)
                        owner.onParamGestureEnded (owner);
                };
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
    layoutDirty_ = true;
    syncFromModule();
}

void ModuleControlDrawer::onParamChanged (size_t rowIndex, double value)
{
    if (syncing || rowIndex >= rows.size())
        return;

    if (activeDragRow_ >= 0 && activeDragRow_ != (int) rowIndex)
        finishActiveSliderDrag (true);

    owner.setParamValue (rows[rowIndex]->def.id, value);
    // 模块可能联动改了别的参数（互斥 / 可见性），整体回读一遍
    const bool visChanged = syncFromModuleInternal();
    if (visChanged || layoutDirty_)
        owner.refreshControlPanel();

    // 连续滑块拖拽期间仅即时更新模块与画面，跳过逐帧 saveLayoutTree()；
    // 松手时在 onDragEnd 从 onDragStart 捕获的变更前快照提交 1 条原子撤回记录。
    if (activeDragRow_ == (int) rowIndex && rows[rowIndex]->def.kind == ParamDef::Kind::slider)
    {
        dragHadValueChange_ = true;
        return;
    }

    // 仅连续滑块设置合并键，离散按钮/开关每次点击都单独成一条撤回记录
    owner.setLastChangedParamCoalesceKey (
        rows[rowIndex]->def.kind == ParamDef::Kind::slider
            ? ("param:" + owner.getModuleId() + ":" + rows[rowIndex]->def.id)
            : juce::String());
    owner.setLastChangedParamFocusHint ("drawer:" + rows[rowIndex]->def.id);
    if (owner.onParamChangedByUser)
        owner.onParamChangedByUser (owner);
    owner.setLastChangedParamCoalesceKey ({});
    owner.setLastChangedParamFocusHint ({});
}

bool ModuleControlDrawer::rebuildGroups()
{
    std::vector<GroupBucket> nextGroups;
    int nextTotalVisible = 0;

    for (size_t idx = 0; idx < rows.size(); ++idx)
    {
        if (! rows[idx]->visible)
            continue;
        ++nextTotalVisible;

        const auto& grpName = rows[idx]->def.group;
        auto it = nextGroups.end();
        for (auto cur = nextGroups.begin(); cur != nextGroups.end(); ++cur)
        {
            if (cur->name == grpName)
            {
                it = cur;
                break;
            }
        }
        if (it == nextGroups.end())
            nextGroups.push_back ({ grpName, { idx } });
        else
            it->rowIndices.push_back (idx);
    }

    bool allNamed = ! nextGroups.empty();
    for (const auto& g : nextGroups)
    {
        if (g.name.isEmpty())
        {
            allNamed = false;
            break;
        }
    }

    const bool nextUseTabs = allNamed && (nextGroups.size() >= 2) && (nextTotalVisible > kTabThresholdRows);

    int nextActiveTab = 0;
    if (nextUseTabs && ! nextGroups.empty())
    {
        int matched = -1;
        if (activeGroupName_.isNotEmpty())
        {
            for (int i = 0; i < (int) nextGroups.size(); ++i)
            {
                if (nextGroups[(size_t) i].name == activeGroupName_)
                {
                    matched = i;
                    break;
                }
            }
        }
        nextActiveTab = (matched >= 0) ? matched
                                       : juce::jlimit (0, (int) nextGroups.size() - 1, activeTabIndex_);
        activeGroupName_ = nextGroups[(size_t) nextActiveTab].name;
    }

    bool structureChanged = (nextUseTabs != useTabs_)
                            || (nextActiveTab != activeTabIndex_)
                            || (nextTotalVisible != totalVisibleRows_)
                            || (nextGroups.size() != groups_.size());
    if (! structureChanged)
    {
        for (size_t i = 0; i < nextGroups.size(); ++i)
        {
            if (nextGroups[i].name != groups_[i].name
                || nextGroups[i].rowIndices != groups_[i].rowIndices)
            {
                structureChanged = true;
                break;
            }
        }
    }

    groups_           = std::move (nextGroups);
    totalVisibleRows_ = nextTotalVisible;
    useTabs_          = nextUseTabs;
    activeTabIndex_   = nextActiveTab;

    if (! useTabs_ || hoveredTabIndex_ >= (int) groups_.size())
        hoveredTabIndex_ = -1;

    return structureChanged;
}

void ModuleControlDrawer::syncFromModule()
{
    syncFromModuleInternal();
}

bool ModuleControlDrawer::syncFromModuleInternal()
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

    const bool groupsChanged = rebuildGroups();

    if (visibilityChanged || groupsChanged)
    {
        layoutDirty_ = true;
        if (getWidth() > 0 && getHeight() > 0)
            applyLayout();
    }
    inner.repaint();
    return visibilityChanged || groupsChanged;
}

bool ModuleControlDrawer::hasTabs() const noexcept
{
    return useTabs_;
}

int ModuleControlDrawer::getTabCount() const noexcept
{
    return useTabs_ ? (int) groups_.size() : 0;
}

int ModuleControlDrawer::getActiveTabIndex() const noexcept
{
    return useTabs_ ? activeTabIndex_ : 0;
}

juce::String ModuleControlDrawer::getTabName (int index) const
{
    if (! useTabs_ || index < 0 || index >= (int) groups_.size())
        return {};
    return groups_[(size_t) index].name;
}

void ModuleControlDrawer::selectTab (int index)
{
    if (! useTabs_ || groups_.empty())
        return;

    const int clamped = juce::jlimit (0, (int) groups_.size() - 1, index);
    if (clamped == activeTabIndex_ && ! layoutDirty_)
        return;

    finishActiveSliderDrag (true);
    activeTabIndex_  = clamped;
    activeGroupName_ = groups_[(size_t) clamped].name;
    layoutDirty_     = true;
    viewport.setViewPosition (0, 0);

    if (! refreshingLayout_ && owner.isControlPanelOpen())
    {
        const juce::ScopedValueSetter<bool> guard (refreshingLayout_, true);
        owner.refreshControlPanel();
    }
    else if (getWidth() > 0 && getHeight() > 0)
    {
        applyLayout();
    }

    repaint();
    inner.repaint();
}

juce::String ModuleControlDrawer::getHighlightedParamId() const noexcept
{
    return (highlightAlpha_ > 0.0f) ? highlightedParamId_ : juce::String();
}

int ModuleControlDrawer::getVisibleRowCount() const noexcept
{
    if (useTabs_)
    {
        if (activeTabIndex_ >= 0 && activeTabIndex_ < (int) groups_.size())
            return (int) groups_[(size_t) activeTabIndex_].rowIndices.size();
        return 0;
    }
    return totalVisibleRows_;
}

void ModuleControlDrawer::revealParam (const juce::String& paramId)
{
    if (paramId.isEmpty())
        return;

    size_t targetRowIdx = rows.size();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i] != nullptr && rows[i]->def.id == paramId)
        {
            targetRowIdx = i;
            break;
        }
    }

    if (targetRowIdx >= rows.size() || ! rows[targetRowIdx]->visible)
        return;

    if (useTabs_)
    {
        int targetTab = -1;
        for (int t = 0; t < (int) groups_.size(); ++t)
        {
            for (size_t ri : groups_[(size_t) t].rowIndices)
            {
                if (ri == targetRowIdx)
                {
                    targetTab = t;
                    break;
                }
            }
            if (targetTab >= 0)
                break;
        }

        if (targetTab >= 0 && targetTab != activeTabIndex_)
        {
            finishActiveSliderDrag (true);
            activeTabIndex_  = targetTab;
            activeGroupName_ = groups_[(size_t) targetTab].name;
            layoutDirty_     = true;

            if (! refreshingLayout_ && owner.isControlPanelOpen())
            {
                const juce::ScopedValueSetter<bool> guard (refreshingLayout_, true);
                owner.refreshControlPanel();
            }
            else if (getWidth() > 0 && getHeight() > 0)
            {
                applyLayout();
            }
        }
    }

    ensureLayoutCurrent();

    const auto& hr = rows[targetRowIdx]->highlightRect;
    if (! hr.isEmpty() && viewport.getViewHeight() > 0)
    {
        const int margin = PinkXP::ui (8);
        const int viewY  = viewport.getViewPositionY();
        const int viewH  = viewport.getViewHeight();
        if (hr.getY() - margin < viewY)
            viewport.setViewPosition (0, juce::jmax (0, hr.getY() - margin));
        else if (hr.getBottom() + margin > viewY + viewH)
            viewport.setViewPosition (0, juce::jmax (0, hr.getBottom() + margin - viewH));
    }

    highlightedParamId_ = paramId;
    highlightStartMs_   = juce::Time::getMillisecondCounter();
    highlightAlpha_     = 1.0f;
    updateTimerState();
    repaint();
    inner.repaint();
}

ModuleControlDrawer::TabStripLayout ModuleControlDrawer::computeTabStripLayout (int outerWidth, const DrawerMetrics& m) const
{
    TabStripLayout out;
    if (! useTabs_ || groups_.empty())
        return out;

    const int stripX = 2;
    const int stripY = 2 + headerBarHeight() + 1;
    const int stripW = juce::jmax (40, outerWidth - 4);
    const int availW = juce::jmax (24, stripW - m.tabPadX * 2);
    const int n      = (int) groups_.size();

    std::vector<int> minWs ((size_t) n, PinkXP::ui (42));
    int singleRowSum = 0;
    for (int i = 0; i < n; ++i)
    {
        minWs[(size_t) i] = measureTabWidth (groups_[(size_t) i].name, m.tabH);
        singleRowSum += minWs[(size_t) i] + (i > 0 ? m.tabGap : 0);
    }

    // 正常宽度下全部 2~4 个标签单行平铺；仅在极端窄面板时优雅折为 2 行
    std::vector<std::vector<int>> tabRows;
    if (singleRowSum <= availW)
    {
        std::vector<int> r;
        r.reserve ((size_t) n);
        for (int i = 0; i < n; ++i)
            r.push_back (i);
        tabRows.push_back (std::move (r));
    }
    else
    {
        // 4 个标签优先尝试对称 2 + 2 折行
        bool balanced2x2 = false;
        if (n == 4)
        {
            const int r0 = minWs[0] + m.tabGap + minWs[1];
            const int r1 = minWs[2] + m.tabGap + minWs[3];
            if (r0 <= availW && r1 <= availW)
            {
                tabRows.push_back ({ 0, 1 });
                tabRows.push_back ({ 2, 3 });
                balanced2x2 = true;
            }
        }

        if (! balanced2x2)
        {
            std::vector<int> curRow;
            int curW = 0;
            for (int i = 0; i < n; ++i)
            {
                const int w = minWs[(size_t) i];
                if (! curRow.empty() && curW + m.tabGap + w > availW)
                {
                    tabRows.push_back (std::move (curRow));
                    curRow.clear();
                    curW = 0;
                }
                curW += (curRow.empty() ? 0 : m.tabGap) + w;
                curRow.push_back (i);
            }
            if (! curRow.empty())
                tabRows.push_back (std::move (curRow));
        }
    }

    out.numRows = (int) tabRows.size();
    out.height  = m.tabPadTop + out.numRows * m.tabH + (out.numRows - 1) * m.tabGap + m.tabPadBot;
    out.stripBounds = { stripX, stripY, stripW, out.height };
    out.tabs.reserve ((size_t) n);

    for (int rIdx = 0; rIdx < out.numRows; ++rIdx)
    {
        const auto& rowItems = tabRows[(size_t) rIdx];
        std::vector<int> rowMinWs;
        rowMinWs.reserve (rowItems.size());
        for (int gIdx : rowItems)
            rowMinWs.push_back (minWs[(size_t) gIdx]);

        const auto finalWs = distributeBalancedWidths (rowMinWs, m.tabGap, availW);
        int x = stripX + m.tabPadX;
        const int y = stripY + m.tabPadTop + rIdx * (m.tabH + m.tabGap);

        for (size_t k = 0; k < rowItems.size(); ++k)
        {
            const int gIdx = rowItems[k];
            const int w    = finalWs[k];
            out.tabs.push_back ({ gIdx, rIdx, groups_[(size_t) gIdx].name, { x, y, w, m.tabH } });
            x += w + m.tabGap;
        }
    }

    return out;
}

ModuleControlDrawer::DrawerMetrics ModuleControlDrawer::computeMetrics()
{
    DrawerMetrics m;
    m.rowH             = PinkXP::ui (kRowH);
    m.choiceLabelH     = PinkXP::ui (kChoiceLabelH);
    m.gap              = PinkXP::ui (kRowGap);
    m.stackedGap       = PinkXP::ui (kStackedLabelGap);
    m.pad              = PinkXP::ui (kPad);
    m.cardPad          = PinkXP::ui (kCardPad);
    m.cardGap          = PinkXP::ui (kCardGap);
    m.headH            = PinkXP::ui (kHeaderH);
    m.headerToCardGap  = PinkXP::ui (kHeaderToCardGap);
    m.labelGap         = PinkXP::ui (kLabelGap);
    m.pairColGap       = PinkXP::ui (kPairColGap);
    m.pairLblGap       = PinkXP::ui (kPairLblGap);
    m.scrollW          = PinkXP::ui (kScrollBarW);
    m.tabH             = PinkXP::ui (kTabH);
    m.tabGap           = PinkXP::ui (kTabGap);
    m.tabPadX          = PinkXP::ui (kTabPadX);
    m.tabPadTop        = PinkXP::ui (kTabPadTop);
    m.tabPadBot        = 1;

    m.toggleW = juce::jmax (PinkXP::ui (48),
                            juce::jmax (measureButtonWidth ("ON", m.rowH),
                                        measureButtonWidth ("OFF", m.rowH)));
    m.sliderMinTrack   = juce::jmax (44, PinkXP::ui (kSliderTrackMinW));
    m.sliderIdealTrack = juce::jmax (m.sliderMinTrack + 20, PinkXP::ui (kSliderTrackIdealW));

    const int gridPrefCap    = PinkXP::ui (kChoiceGridPrefCap);
    const int gridWideCap    = PinkXP::ui (kChoiceGridWideCap);
    const int shortSideCap   = PinkXP::ui (kShortChoiceSideW);
    const int pairPrefCap    = PinkXP::ui (kPairPrefMaxW);
    const int pairMinInner   = PinkXP::ui (kPairMinCardInnerW);

    int globalMinCardInnerW  = 0;
    int globalPrefCardInnerW = 0;
    int globalWideCardInnerW = 0;
    int globalMaxHeaderW     = 0;

    // 遍历【全部】可见分组（与当前 activeTabIndex_ 无关），确保切换标签时宽度 100% 恒定零抖动
    for (const auto& grp : groups_)
    {
        if (! useTabs_ && grp.name.isNotEmpty())
            globalMaxHeaderW = juce::jmax (globalMaxHeaderW, measureHeaderWidth (grp.name, m.headH) + PinkXP::ui (12));

        int grpSliderBoxW = 0;
        for (size_t idx : grp.rowIndices)
        {
            const auto& r = *rows[idx];
            if (r.def.kind == ParamDef::Kind::slider && r.slider != nullptr)
                grpSliderBoxW = juce::jmax (grpSliderBoxW, measureSliderBoxWidth (*r.slider, r.def, m.rowH));
        }

        // 先标出本组内适合双列并排的相邻开关对
        std::vector<bool> inTogglePair (grp.rowIndices.size(), false);
        for (size_t p = 0; p + 1 < grp.rowIndices.size(); )
        {
            const auto& rA = *rows[grp.rowIndices[p]];
            const auto& rB = *rows[grp.rowIndices[p + 1]];
            if (rA.def.kind == ParamDef::Kind::toggle && rB.def.kind == ParamDef::Kind::toggle)
            {
                const int lwA = measureLabelWidth (rA.def.label, m.rowH);
                const int lwB = measureLabelWidth (rB.def.label, m.rowH);
                const int halfNeed = juce::jmax (lwA, lwB) + m.pairLblGap + m.toggleW;
                const int pairNeed = halfNeed * 2 + m.pairColGap;
                if (pairNeed <= pairPrefCap)
                {
                    inTogglePair[p]     = true;
                    inTogglePair[p + 1] = true;
                    const int targetW = juce::jmax (pairMinInner, pairNeed);
                    globalPrefCardInnerW = juce::jmax (globalPrefCardInnerW, targetW);
                    globalWideCardInnerW = juce::jmax (globalWideCardInnerW, targetW);
                    globalMinCardInnerW  = juce::jmax (globalMinCardInnerW, juce::jmax (lwA, lwB, m.toggleW));
                    p += 2;
                    continue;
                }
            }
            ++p;
        }

        int grpSideLabelMax = 0;
        int grpSideIdealCtl = 0;
        int grpSideWideCtl  = 0;

        for (size_t p = 0; p < grp.rowIndices.size(); ++p)
        {
            if (inTogglePair[p])
                continue;

            const auto& r = *rows[grp.rowIndices[p]];
            const int lw = measureLabelWidth (r.def.label, m.rowH);
            globalMinCardInnerW = juce::jmax (globalMinCardInnerW, lw);

            if (r.def.kind == ParamDef::Kind::toggle)
            {
                globalMinCardInnerW = juce::jmax (globalMinCardInnerW, m.toggleW);
                grpSideLabelMax     = juce::jmax (grpSideLabelMax, lw);
                grpSideIdealCtl     = juce::jmax (grpSideIdealCtl, m.toggleW);
                grpSideWideCtl      = juce::jmax (grpSideWideCtl,  m.toggleW);
            }
            else if (r.def.kind == ParamDef::Kind::slider && r.slider != nullptr)
            {
                const int boxW = (grpSliderBoxW > 0) ? grpSliderBoxW : measureSliderBoxWidth (*r.slider, r.def, m.rowH);
                globalMinCardInnerW = juce::jmax (globalMinCardInnerW, boxW + m.sliderMinTrack);
                grpSideLabelMax     = juce::jmax (grpSideLabelMax, lw);
                grpSideIdealCtl     = juce::jmax (grpSideIdealCtl, boxW + m.sliderIdealTrack);
                grpSideWideCtl      = juce::jmax (grpSideWideCtl,  boxW + m.sliderIdealTrack);
            }
            else if (r.def.kind == ParamDef::Kind::choice)
            {
                int maxBtnW = 0;
                int rowSum  = 0;
                for (size_t i = 0; i < r.btns.size(); ++i)
                {
                    const int bw = measureButtonWidth (r.btns[i]->getButtonText(), m.rowH);
                    maxBtnW = juce::jmax (maxBtnW, bw);
                    rowSum += bw + (i > 0 ? m.gap : 0);
                }
                globalMinCardInnerW = juce::jmax (globalMinCardInnerW, maxBtnW);

                const int n = (int) r.btns.size();
                if (n >= 4)
                {
                    // >=4 项使用标签置顶 + 全宽均衡按钮网格：4/7/8 项优选 4 列，5/6 项优选 3 列
                    const int targetCols = (n == 5 || n == 6) ? 3 : 4;
                    const int wTarget    = targetCols * maxBtnW + (targetCols - 1) * m.gap;
                    int gridPrefW = (n == 4 && rowSum <= gridPrefCap) ? juce::jmin (wTarget, rowSum) : wTarget;
                    if (gridPrefW > gridPrefCap)
                    {
                        const int fbCols = (n == 4) ? 2 : juce::jmax (2, targetCols - 1);
                        const int wFb    = fbCols * maxBtnW + (fbCols - 1) * m.gap;
                        gridPrefW = (wFb <= gridPrefCap)
                                        ? wFb
                                        : juce::jmax (maxBtnW, juce::jmin (gridPrefCap, 2 * maxBtnW + m.gap));
                    }
                    const int gridWideW = (wTarget <= gridWideCap) ? wTarget : gridPrefW;
                    globalPrefCardInnerW = juce::jmax (globalPrefCardInnerW, juce::jmax (lw, gridPrefW));
                    globalWideCardInnerW = juce::jmax (globalWideCardInnerW, juce::jmax (lw, gridWideW));
                }
                else if (rowSum <= shortSideCap)
                {
                    grpSideLabelMax = juce::jmax (grpSideLabelMax, lw);
                    grpSideIdealCtl = juce::jmax (grpSideIdealCtl, rowSum);
                    grpSideWideCtl  = juce::jmax (grpSideWideCtl,  rowSum);
                }
                else
                {
                    globalPrefCardInnerW = juce::jmax (globalPrefCardInnerW, juce::jmax (lw, rowSum));
                    globalWideCardInnerW = juce::jmax (globalWideCardInnerW, juce::jmax (lw, rowSum));
                }
            }
        }

        if (grpSideLabelMax > 0 && grpSideIdealCtl > 0)
        {
            globalPrefCardInnerW = juce::jmax (globalPrefCardInnerW, grpSideLabelMax + m.labelGap + grpSideIdealCtl);
            globalWideCardInnerW = juce::jmax (globalWideCardInnerW, grpSideLabelMax + m.labelGap + grpSideWideCtl);
        }
    }

    // 分类标签条所需宽度：常规宽度确保全部标签单行排开，最小宽度确保单标签零截断
    int tabsMinOuterW       = 0;
    int tabsSingleRowOuterW = 0;
    if (useTabs_ && ! groups_.empty())
    {
        int singleRowTabsW = 0;
        int maxSingleTabW  = 0;
        for (size_t i = 0; i < groups_.size(); ++i)
        {
            const int tw = measureTabWidth (groups_[i].name, m.tabH);
            maxSingleTabW = juce::jmax (maxSingleTabW, tw);
            singleRowTabsW += tw + (i > 0 ? m.tabGap : 0);
        }
        tabsMinOuterW       = 4 + m.tabPadX * 2 + maxSingleTabW;
        tabsSingleRowOuterW = 4 + m.tabPadX * 2 + singleRowTabsW;
    }

    const int chromeCardScroll = 4 + m.scrollW + m.pad * 2 + m.cardPad * 2;
    const int chromeHeadScroll = 4 + m.scrollW + m.pad * 2;
    const int titleMinW        = measureTitleMinOuterWidth (owner.getTitleText());

    const int minContentOuterW  = juce::jmax (chromeHeadScroll + globalMaxHeaderW,
                                              chromeCardScroll + globalMinCardInnerW);
    m.minOuterW = juce::jmax (PinkXP::ui (220),
                              juce::jmax (titleMinW, tabsMinOuterW),
                              minContentOuterW);

    const int prefContentOuterW = juce::jmax (chromeHeadScroll + globalMaxHeaderW,
                                              chromeCardScroll + globalPrefCardInnerW);
    const int rawPrefOuterW = juce::jmax (PinkXP::ui (kPrefOuterMinW),
                                          juce::jmax (titleMinW, tabsSingleRowOuterW),
                                          prefContentOuterW);
    m.prefOuterW = juce::jlimit (m.minOuterW,
                                 juce::jmax (m.minOuterW, PinkXP::ui (kPrefOuterMaxW)),
                                 rawPrefOuterW);

    const int rawMaxOuterW = juce::jmax (m.prefOuterW + PinkXP::ui (56),
                                         chromeCardScroll + juce::jmax (globalPrefCardInnerW, globalWideCardInnerW));
    m.maxOuterW = juce::jlimit (m.prefOuterW,
                                juce::jmax (m.prefOuterW, PinkXP::ui (kHardOuterMaxW)),
                                rawMaxOuterW);

    return m;
}

ModuleControlDrawer::ComputedLayout ModuleControlDrawer::computeLayoutForTab (
    int innerWidth, const DrawerMetrics& m, int tabIndex) const
{
    ComputedLayout out;
    out.rows.resize (rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
        out.rows[i].rowIndex = i;

    if (groups_.empty())
    {
        out.contentHeight = m.pad * 2;
        return out;
    }

    const int outerW     = juce::jmax (40, innerWidth - m.pad * 2);
    const int cardX      = m.pad;
    const int cardInnerX = cardX + m.cardPad;
    const int cardInnerW = juce::jmax (24, outerW - m.cardPad * 2);
    const int pairMinW   = PinkXP::ui (kPairMinCardInnerW);

    std::vector<int> displayGroups;
    if (useTabs_)
        displayGroups.push_back (juce::jlimit (0, (int) groups_.size() - 1, tabIndex));
    else
        for (int g = 0; g < (int) groups_.size(); ++g)
            displayGroups.push_back (g);

    const bool showHeaders = ! useTabs_;

    int y = m.pad;
    bool hasAnyGroup = false;

    for (int gIdx : displayGroups)
    {
        const auto& grp = groups_[(size_t) gIdx];
        if (grp.rowIndices.empty())
            continue;

        if (hasAnyGroup)
            y += m.cardGap;

        if (showHeaders && grp.name.isNotEmpty())
        {
            out.headers.push_back ({ grp.name, { cardX, y, outerW, m.headH } });
            y += m.headH + m.headerToCardGap;
        }

        const int cardTop = y;
        y += m.cardPad;

        // 卡片内滑块统一读数框宽
        int cardSliderBoxW = 0;
        for (size_t idx : grp.rowIndices)
        {
            const auto& r = *rows[idx];
            if (r.def.kind == ParamDef::Kind::slider && r.slider != nullptr)
                cardSliderBoxW = juce::jmax (cardSliderBoxW, measureSliderBoxWidth (*r.slider, r.def, m.rowH));
        }

        const int colWL = (cardInnerW - m.pairColGap) / 2;
        const int rightX = cardInnerX + colWL + m.pairColGap;
        const int colWR = cardInnerW - colWL - m.pairColGap;

        auto canPairToggles = [&] (const Row& rA, const Row& rB) -> bool
        {
            if (cardInnerW < pairMinW)
                return false;
            const int lwA = measureLabelWidth (rA.def.label, m.rowH);
            const int lwB = measureLabelWidth (rB.def.label, m.rowH);
            return (lwA + m.pairLblGap + m.toggleW <= colWL)
                && (lwB + m.pairLblGap + m.toggleW <= colWR);
        };

        // 预扫描本卡片内可并排的相邻开关对
        std::vector<bool> pairLead (grp.rowIndices.size(), false);
        std::vector<bool> pairMember (grp.rowIndices.size(), false);
        bool cardHasAnyPair = false;
        for (size_t p = 0; p + 1 < grp.rowIndices.size(); )
        {
            const auto& rA = *rows[grp.rowIndices[p]];
            const auto& rB = *rows[grp.rowIndices[p + 1]];
            if (rA.def.kind == ParamDef::Kind::toggle && rB.def.kind == ParamDef::Kind::toggle
                && canPairToggles (rA, rB))
            {
                pairLead[p]       = true;
                pairMember[p]     = true;
                pairMember[p + 1] = true;
                cardHasAnyPair    = true;
                p += 2;
            }
            else
            {
                ++p;
            }
        }

        auto rowMinSideCtlW = [&] (const Row& r) -> int
        {
            if (r.def.kind == ParamDef::Kind::toggle)
                return m.toggleW;
            if (r.def.kind == ParamDef::Kind::slider)
                return (cardSliderBoxW > 0 ? cardSliderBoxW : PinkXP::ui (68)) + m.sliderMinTrack;
            int sum = 0;
            for (size_t i = 0; i < r.btns.size(); ++i)
                sum += measureButtonWidth (r.btns[i]->getButtonText(), m.rowH) + (i > 0 ? m.gap : 0);
            return sum;
        };

        // 计算本卡片内并排行（滑块 / 单列开关 / 2~3 项短选择）的统一标签列宽
        int allSideMaxLbl = 0;
        int allSideMaxCtl = 0;
        for (size_t p = 0; p < grp.rowIndices.size(); ++p)
        {
            if (pairMember[p])
                continue;
            const auto& r = *rows[grp.rowIndices[p]];
            if (r.def.kind == ParamDef::Kind::choice && r.btns.size() >= 4)
                continue;
            const int lw = measureLabelWidth (r.def.label, m.rowH);
            const int mc = rowMinSideCtlW (r);
            allSideMaxLbl = juce::jmax (allSideMaxLbl, lw);
            allSideMaxCtl = juce::jmax (allSideMaxCtl, mc);
        }

        int colLabelW = 0;
        if (allSideMaxLbl > 0 && allSideMaxLbl + m.labelGap + allSideMaxCtl <= cardInnerW)
        {
            colLabelW = allSideMaxLbl + m.labelGap;
        }
        else
        {
            int fitMaxLabelW = 0;
            for (size_t p = 0; p < grp.rowIndices.size(); ++p)
            {
                if (pairMember[p])
                    continue;
                const auto& r = *rows[grp.rowIndices[p]];
                if (r.def.kind == ParamDef::Kind::choice && r.btns.size() >= 4)
                    continue;
                const int lw = measureLabelWidth (r.def.label, m.rowH);
                if (lw + m.labelGap + rowMinSideCtlW (r) <= cardInnerW)
                    fitMaxLabelW = juce::jmax (fitMaxLabelW, lw);
            }
            if (fitMaxLabelW > 0)
            {
                const int cap = juce::jmax (PinkXP::ui (48), (int) ((float) cardInnerW * 0.52f));
                colLabelW = juce::jmin (fitMaxLabelW + m.labelGap, cap);
            }
        }

        // 逐行排布卡片内控件
        for (size_t p = 0; p < grp.rowIndices.size(); )
        {
            // ---- 双列并排开关 ----
            if (pairLead[p] && p + 1 < grp.rowIndices.size())
            {
                const size_t idxA = grp.rowIndices[p];
                const size_t idxB = grp.rowIndices[p + 1];

                auto& rlA = out.rows[idxA];
                rlA.visible       = true;
                rlA.labelRect     = { cardInnerX, y, juce::jmax (1, colWL - m.pairLblGap - m.toggleW), m.rowH };
                rlA.btnRects.push_back ({ cardInnerX + colWL - m.toggleW, y, m.toggleW, m.rowH });
                rlA.highlightRect = { cardInnerX - 2, y - 2, colWL + 4, m.rowH + 4 };

                auto& rlB = out.rows[idxB];
                rlB.visible       = true;
                rlB.labelRect     = { rightX, y, juce::jmax (1, colWR - m.pairLblGap - m.toggleW), m.rowH };
                rlB.btnRects.push_back ({ rightX + colWR - m.toggleW, y, m.toggleW, m.rowH });
                rlB.highlightRect = { rightX - 2, y - 2, colWR + 4, m.rowH + 4 };

                out.pairDividers.push_back ({ cardInnerX + colWL + m.pairColGap / 2,
                                              y + 3, 1, juce::jmax (1, m.rowH - 6) });

                y += m.rowH + m.gap;
                p += 2;
                continue;
            }

            const size_t idx = grp.rowIndices[p];
            const auto& r    = *rows[idx];
            auto& rl         = out.rows[idx];
            rl.visible       = true;

            const int rowTopY = y;
            const int lw      = measureLabelWidth (r.def.label, m.rowH);
            const int sideCtlW = cardInnerW - colLabelW;

            if (r.def.kind == ParamDef::Kind::slider)
            {
                const int rowBoxW = (cardSliderBoxW > 0) ? cardSliderBoxW : PinkXP::ui (68);
                const int minCtl  = rowBoxW + m.sliderMinTrack;
                const bool sideBySide = (colLabelW > 0)
                                        && (lw <= colLabelW - m.labelGap)
                                        && (sideCtlW >= minCtl);

                int ctlX = cardInnerX;
                int ctlW = cardInnerW;
                if (sideBySide)
                {
                    rl.labelRect = { cardInnerX, y, colLabelW - m.labelGap, m.rowH };
                    ctlX = cardInnerX + colLabelW;
                    ctlW = sideCtlW;
                }
                else
                {
                    rl.labelRect = { cardInnerX, y, cardInnerW, m.rowH };
                    y += m.rowH + m.stackedGap;
                }

                const int effBoxW = (ctlW >= rowBoxW + juce::jmax (38, PinkXP::ui (40)))
                                        ? rowBoxW
                                        : juce::jmin (rowBoxW, juce::jmax (30, ctlW / 2));
                rl.sliderBoxW    = effBoxW;
                rl.sliderRect    = { ctlX, y, ctlW, m.rowH };
                y += m.rowH + m.gap;
                rl.highlightRect = { cardInnerX - 2, rowTopY - 2, cardInnerW + 4, (y - m.gap) - rowTopY + 4 };
            }
            else if (r.def.kind == ParamDef::Kind::toggle)
            {
                // 若本卡片存在双列开关且单落左列完全放得下，让奇数末行开关严格对齐左列
                if (cardHasAnyPair && cardInnerW >= pairMinW && (lw + m.pairLblGap + m.toggleW <= colWL))
                {
                    rl.labelRect     = { cardInnerX, y, juce::jmax (1, colWL - m.pairLblGap - m.toggleW), m.rowH };
                    rl.btnRects.push_back ({ cardInnerX + colWL - m.toggleW, y, m.toggleW, m.rowH });
                    rl.highlightRect = { cardInnerX - 2, y - 2, colWL + 4, m.rowH + 4 };
                    y += m.rowH + m.gap;
                }
                else
                {
                    const bool sideBySide = (colLabelW > 0)
                                            && (lw <= colLabelW - m.labelGap)
                                            && (sideCtlW >= m.toggleW);
                    if (sideBySide)
                    {
                        rl.labelRect = { cardInnerX, y, colLabelW - m.labelGap, m.rowH };
                        rl.btnRects.push_back ({ cardInnerX + colLabelW, y, juce::jmin (sideCtlW, m.toggleW), m.rowH });
                    }
                    else
                    {
                        rl.labelRect = { cardInnerX, y, cardInnerW, m.rowH };
                        y += m.rowH + m.stackedGap;
                        rl.btnRects.push_back ({ cardInnerX, y, juce::jmin (cardInnerW, m.toggleW), m.rowH });
                    }
                    y += m.rowH + m.gap;
                    rl.highlightRect = { cardInnerX - 2, rowTopY - 2, cardInnerW + 4, (y - m.gap) - rowTopY + 4 };
                }
            }
            else // ParamDef::Kind::choice
            {
                std::vector<int> bws;
                bws.reserve (r.btns.size());
                int maxBtnW = 0;
                int rowSum  = 0;
                for (size_t i = 0; i < r.btns.size(); ++i)
                {
                    const int bw = measureButtonWidth (r.btns[i]->getButtonText(), m.rowH);
                    bws.push_back (bw);
                    maxBtnW = juce::jmax (maxBtnW, bw);
                    rowSum += bw + (i > 0 ? m.gap : 0);
                }

                const bool useStackedGrid = (r.btns.size() >= 4)
                                            || (colLabelW <= 0)
                                            || (lw > colLabelW - m.labelGap)
                                            || (sideCtlW < rowSum);

                rl.btnRects.reserve (r.btns.size());

                if (! useStackedGrid)
                {
                    // 2~3 项短选择：与标签列齐平单行均衡排布
                    rl.labelRect = { cardInnerX, y, colLabelW - m.labelGap, m.rowH };
                    const int ctlX = cardInnerX + colLabelW;
                    const int ctlW = sideCtlW;
                    const auto finalWs = distributeBalancedWidths (bws, m.gap, ctlW);
                    int x = ctlX;
                    for (size_t i = 0; i < r.btns.size(); ++i)
                    {
                        rl.btnRects.push_back ({ x, y, finalWs[i], m.rowH });
                        x += finalWs[i] + m.gap;
                    }
                    y += m.rowH + m.gap;
                }
                else
                {
                    // >=4 项或单行装不下：标签置顶 + 全宽均衡按钮网格
                    const int labelH = juce::jmax (m.choiceLabelH, m.rowH - PinkXP::ui (3));
                    rl.labelRect = { cardInnerX, y, cardInnerW, labelH };
                    y += labelH + m.stackedGap;

                    const int n = (int) r.btns.size();
                    const int maxFitCols = (maxBtnW + m.gap > 0) ? ((cardInnerW + m.gap) / (maxBtnW + m.gap)) : 1;

                    if (n >= 1 && n <= 4 && rowSum <= cardInnerW)
                    {
                        // 2~4 项可完整放进单行时，做全宽均衡单行排布（兼顾等宽与不等宽文案）
                        const auto finalWs = distributeBalancedWidths (bws, m.gap, cardInnerW);
                        int x = cardInnerX;
                        for (int i = 0; i < n; ++i)
                        {
                            rl.btnRects.push_back ({ x, y, finalWs[(size_t) i], m.rowH });
                            x += finalWs[(size_t) i] + m.gap;
                        }
                        y += m.rowH + m.gap;
                    }
                    else if (n >= 2 && maxFitCols >= 2)
                    {
                        const int capCols     = juce::jmin (n, juce::jmin (4, maxFitCols));
                        const int numGridRows = (n + capCols - 1) / capCols;
                        const int cols        = (n + numGridRows - 1) / numGridRows;
                        const int totalGaps   = (cols - 1) * m.gap;
                        const int baseW       = (cardInnerW - totalGaps) / cols;
                        const int extra       = (cardInnerW - totalGaps) % cols;

                        for (int i = 0; i < n; ++i)
                        {
                            const int gr = i / cols;
                            const int gc = i % cols;
                            const int bx = cardInnerX + gc * (baseW + m.gap) + juce::jmin (gc, extra);
                            const int bw = baseW + (gc < extra ? 1 : 0);
                            const int by = y + gr * (m.rowH + m.gap);
                            rl.btnRects.push_back ({ bx, by, bw, m.rowH });
                        }
                        y += numGridRows * m.rowH + numGridRows * m.gap;
                    }
                    else
                    {
                        // 极窄宽度兜底：贪心换行，确保不溢出不重叠
                        const int ctlRight = cardInnerX + cardInnerW;
                        int x = cardInnerX;
                        int lineY = y;
                        for (size_t i = 0; i < r.btns.size(); ++i)
                        {
                            const int bw = bws[i];
                            if (x + bw > ctlRight && x > cardInnerX)
                            {
                                x = cardInnerX;
                                lineY += m.rowH + m.gap;
                            }
                            const int finalW = juce::jmin (bw, juce::jmax (1, ctlRight - x));
                            rl.btnRects.push_back ({ x, lineY, finalW, m.rowH });
                            x += finalW + m.gap;
                        }
                        y = lineY + m.rowH + m.gap;
                    }
                }

                rl.highlightRect = { cardInnerX - 2, rowTopY - 2, cardInnerW + 4, (y - m.gap) - rowTopY + 4 };
            }

            ++p;
        }

        const int cardBottom = y - m.gap + m.cardPad;
        out.cards.push_back ({ { cardX, cardTop, outerW, juce::jmax (1, cardBottom - cardTop) } });
        y = cardBottom;
        hasAnyGroup = true;
    }

    out.contentHeight = hasAnyGroup ? (y + m.pad) : (m.pad * 2);
    return out;
}

int ModuleControlDrawer::getContentHeight (int width)
{
    const auto m = computeMetrics();
    return computeLayoutForTab (juce::jmax (40, width), m, activeTabIndex_).contentHeight;
}

int ModuleControlDrawer::getPreferredTotalHeight (int outerWidth)
{
    const auto m = computeMetrics();
    const auto tabLayout = computeTabStripLayout (outerWidth, m);
    const int chromeH = 2 + headerBarHeight() + 1 + tabLayout.height + 2;
    return chromeH + computeLayoutForTab (juce::jmax (48, outerWidth - 4), m, activeTabIndex_).contentHeight;
}

int ModuleControlDrawer::getTallestTabTotalHeight (int outerWidth)
{
    const auto m = computeMetrics();
    const auto tabLayout = computeTabStripLayout (outerWidth, m);
    const int chromeH = 2 + headerBarHeight() + 1 + tabLayout.height + 2;
    const int contentW = juce::jmax (48, outerWidth - 4);

    if (! useTabs_ || groups_.empty())
        return chromeH + computeLayoutForTab (contentW, m, 0).contentHeight;

    int maxContentH = 0;
    for (int t = 0; t < (int) groups_.size(); ++t)
        maxContentH = juce::jmax (maxContentH, computeLayoutForTab (contentW, m, t).contentHeight);
    return chromeH + maxContentH;
}

int ModuleControlDrawer::getMinimumOuterWidth()
{
    return computeMetrics().minOuterW;
}

int ModuleControlDrawer::getPreferredOuterWidth()
{
    return computeMetrics().prefOuterW;
}

int ModuleControlDrawer::getMaximumOuterWidth()
{
    return computeMetrics().maxOuterW;
}

void ModuleControlDrawer::applyLayout()
{
    const auto m = computeMetrics();
    tabStripLayout_ = computeTabStripLayout (getWidth(), m);

    const int topOffset = 2 + headerBarHeight() + 1 + tabStripLayout_.height;
    auto body = getLocalBounds().reduced (2);
    body.setTop (juce::jmin (body.getBottom(), topOffset));

    if (viewport.getScrollBarThickness() != m.scrollW)
        viewport.setScrollBarThickness (m.scrollW);
    viewport.setSingleStepSizes (PinkXP::ui (16), m.rowH + m.gap);

    viewport.setBounds (body);

    const int bodyW = juce::jmax (40, body.getWidth());
    const int bodyH = juce::jmax (1, body.getHeight());

    // 两阶段判定纵向滚动条：
    //   · 无滚动条时内容高度 <= bodyH → 用全宽 bodyW，不出现滚动条，右侧零空隙；
    //   · 内容超出 bodyH → 预先扣掉滚动条宽 m.scrollW 再排布，滚动条与右侧控件零重叠。
    auto layout = computeLayoutForTab (bodyW, m, activeTabIndex_);
    int innerW = bodyW;
    int innerH = bodyH;

    if (layout.contentHeight > bodyH)
    {
        innerW = juce::jmax (40, bodyW - m.scrollW);
        layout = computeLayoutForTab (innerW, m, activeTabIndex_);
        innerH = juce::jmax (layout.contentHeight, bodyH + 1);
    }

    headers       = std::move (layout.headers);
    cards_        = std::move (layout.cards);
    pairDividers_ = std::move (layout.pairDividers);

    {
        const juce::ScopedValueSetter<bool> guard (syncing, true);
        for (const auto& rl : layout.rows)
        {
            auto& r = rows[rl.rowIndex];
            r->shownOnActivePage = rl.visible;
            for (auto& b : r->btns) b->setVisible (rl.visible);
            if (r->slider != nullptr) r->slider->setVisible (rl.visible);
            if (! rl.visible)
            {
                r->labelRect     = {};
                r->highlightRect = {};
                continue;
            }

            r->labelRect     = rl.labelRect;
            r->highlightRect = rl.highlightRect;

            if (r->def.kind == ParamDef::Kind::slider && r->slider != nullptr)
            {
                if (r->slider->getTextBoxWidth() != rl.sliderBoxW
                    || r->slider->getTextBoxHeight() != m.rowH)
                {
                    r->slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, rl.sliderBoxW, m.rowH);
                }
                r->slider->setBounds (rl.sliderRect);
            }
            else
            {
                const size_t n = juce::jmin (r->btns.size(), rl.btnRects.size());
                for (size_t i = 0; i < n; ++i)
                    r->btns[i]->setBounds (rl.btnRects[i]);
            }
        }
    }

    lastInnerWidth      = innerW;
    lastInnerHeight     = innerH;
    lastOuterW_         = getWidth();
    lastOuterH_         = getHeight();
    lastBodyW_          = body.getWidth();
    lastBodyH_          = body.getHeight();
    lastAppliedTabIndex_ = activeTabIndex_;
    lastAppliedUseTabs_  = useTabs_;
    lastUiScale_        = PinkXP::uiScale();
    lastFontGen_        = PinkXP::getFontGeneration();
    layoutDirty_        = false;

    inner.setSize (innerW, innerH);
    const int maxScrollY = juce::jmax (0, innerH - bodyH);
    if (viewport.getViewPositionY() > maxScrollY)
        viewport.setViewPosition (0, maxScrollY);
    inner.repaint();
    repaint();
}

void ModuleControlDrawer::ensureLayoutCurrent()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    if (layoutDirty_
        || lastOuterW_ != getWidth()
        || lastOuterH_ != getHeight()
        || lastAppliedTabIndex_ != activeTabIndex_
        || lastAppliedUseTabs_ != useTabs_
        || lastUiScale_ != PinkXP::uiScale()
        || lastFontGen_ != PinkXP::getFontGeneration())
    {
        applyLayout();
    }
}

void ModuleControlDrawer::resized()
{
    applyLayout();
}

void ModuleControlDrawer::paintTabStrip (juce::Graphics& g) const
{
    if (! useTabs_ || tabStripLayout_.stripBounds.isEmpty())
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const auto strip   = tabStripLayout_.stripBounds;
    const bool isDark  = PinkXP::face.getPerceivedBrightness() < 0.45f;

    // 1. 风格化标签条底槽与底部分割基线
    juce::Colour troughBg;
    juce::Colour baseLineCol;
    switch (tokens.frameStyle)
    {
        case PinkXP::FrameStyle::pixelBevel:
            troughBg    = isDark ? PinkXP::face.darker (0.20f) : PinkXP::face.darker (0.07f);
            baseLineCol = PinkXP::shdw.withAlpha (isDark ? 0.62f : 0.48f);
            break;

        case PinkXP::FrameStyle::machinedChrome:
            troughBg    = PinkXP::face.darker (0.10f);
            baseLineCol = PinkXP::dark.withAlpha (0.65f);
            break;

        case PinkXP::FrameStyle::frostedGlass:
            troughBg    = isDark ? PinkXP::face.darker (0.14f) : PinkXP::face.darker (0.05f);
            baseLineCol = PinkXP::dark.withAlpha (0.24f);
            break;

        case PinkXP::FrameStyle::neonTube:
            troughBg    = PinkXP::face.interpolatedWith (juce::Colours::black, 0.24f);
            baseLineCol = PinkXP::sel.withAlpha (0.48f);
            break;

        case PinkXP::FrameStyle::crtPhosphor:
            troughBg    = PinkXP::content;
            baseLineCol = PinkXP::sel.withAlpha (0.58f);
            break;

        case PinkXP::FrameStyle::modernStudio:
        default:
            troughBg    = isDark ? PinkXP::face.darker (0.16f) : PinkXP::face.darker (0.045f);
            baseLineCol = PinkXP::skinHairlineOn (PinkXP::face, isDark ? 0.15f : 0.17f);
            break;
    }

    if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
    {
        // Millennium Chrome：CNC 凹陷拉丝钢页签槽（微暗金属渐变 + 顶内阴影 + 底部机加工接缝）
        const auto topCol = PinkXP::face.darker (isDark ? 0.24f : 0.14f);
        const auto botCol = PinkXP::face.darker (isDark ? 0.12f : 0.06f);
        juce::ColourGradient grad (topCol, 0.0f, (float) strip.getY(),
                                   botCol, 0.0f, (float) strip.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRect (strip);

        g.setColour (juce::Colours::black.withAlpha (isDark ? 0.36f : 0.14f));
        g.fillRect (strip.getX(), strip.getY(), strip.getWidth(), 1);
        g.setColour (PinkXP::dark.withAlpha (isDark ? 0.85f : 0.68f));
        g.fillRect (strip.getX(), strip.getBottom() - 1, strip.getWidth(), 1);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
    {
        // Frosted Glass：半透明磨砂玻璃胶囊页签槽 + 顶沿微光 + 底部半透明细线
        juce::ColourGradient grad (troughBg.interpolatedWith (juce::Colours::white, isDark ? 0.05f : 0.18f),
                                   0.0f, (float) strip.getY(),
                                   troughBg, 0.0f, (float) strip.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRect (strip);
        g.setColour (juce::Colours::white.withAlpha (isDark ? 0.14f : 0.55f));
        g.fillRect (strip.getX(), strip.getY(), strip.getWidth(), 1);
        g.setColour (PinkXP::dark.withAlpha (isDark ? 0.34f : 0.22f));
        g.fillRect (strip.getX(), strip.getBottom() - 1, strip.getWidth(), 1);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
    {
        // Neon Cyber：暗烟熏霓虹灯箱页签槽 + 底部双通道发光霓虹轨
        g.setColour (isDark ? PinkXP::face.interpolatedWith (juce::Colours::black, 0.36f)
                            : PinkXP::face.interpolatedWith (juce::Colours::black, 0.10f));
        g.fillRect (strip);
        g.setColour (PinkXP::sel.withAlpha (0.24f));
        g.fillRect (strip.getX(), strip.getBottom() - 2, strip.getWidth(), 2);
        g.setColour (PinkXP::sel.withAlpha (0.76f));
        g.fillRect (strip.getX(), strip.getBottom() - 1, strip.getWidth(), 1);
    }
    else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
    {
        // CRT Terminal：0px 荧光终端页签槽 + 锐利 1px 荧光底轨
        g.setColour (isDark ? PinkXP::content : PinkXP::face.darker (0.08f));
        g.fillRect (strip);
        g.setColour (PinkXP::sel.withAlpha (0.72f));
        g.fillRect (strip.getX(), strip.getBottom() - 1, strip.getWidth(), 1);
    }
    else
    {
        g.setColour (troughBg);
        g.fillRect (strip);

        g.setColour (baseLineCol);
        g.fillRect (strip.getX(), strip.getBottom() - 1, strip.getWidth(), 1);
    }

    // 2. 逐个标签经由 Craft::drawTabButton 绘制
    for (const auto& t : tabStripLayout_.tabs)
    {
        const bool isActive    = (t.groupIndex == activeTabIndex_);
        const bool isHovered   = (! isActive && t.groupIndex == hoveredTabIndex_);
        const bool isBottomRow = (t.rowIndex == tabStripLayout_.numRows - 1);

        auto r = t.bounds;
        if ((tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
             || tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
            && isActive && isBottomRow)
        {
            // Win95 / Millennium Chrome 机架页签：最底行激活页签无缝向下打通基线，直连下方卡片区
            r.setBottom (strip.getBottom());
        }

        PinkXP::Craft::drawTabButton (g, r, isActive, isHovered, t.name, 10.5f);

        if ((tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
             || tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
            && isActive && ! isBottomRow)
        {
            // 多行折行时非最底行激活页签补齐底边
            g.setColour (PinkXP::sel.withAlpha (0.85f));
            g.fillRect (r.getX() + 1, r.getBottom() - 1, juce::jmax (2, r.getWidth() - 2), 1);
        }
    }
}

void ModuleControlDrawer::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    PinkXP::drawRaised (g, bounds, PinkXP::face);

    const auto tb = getHeaderBarBounds();
    if (tb.getWidth() > 4 && tb.getHeight() > 4)
    {
        PinkXP::drawPinkTitleBar (g, tb, owner.getTitleText(), 10.5f);
        g.setColour (PinkXP::titleBarDividerColour());
        g.fillRect (tb.getX(), tb.getBottom(), tb.getWidth(), 1);

        const auto cb = getCloseButtonBounds();
        const auto& tokens = PinkXP::getSkinTokens();
        if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
        {
            // Modern Minimal：与标题栏融为一体的微圆角演播室关闭键
            const auto rf  = cb.toFloat();
            const float rad = juce::jmin (3.0f, rf.getHeight() * 0.35f);
            if (closePressed_ || closeHovered_)
            {
                g.setColour (PinkXP::selInk.withAlpha (closePressed_ ? 0.24f : 0.15f));
                g.fillRoundedRectangle (rf, rad);
                g.setColour (PinkXP::selInk.withAlpha (0.28f));
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            }
            auto cbText = cb;
            if (closePressed_) cbText.translate (0, 1);
            PinkXP::SymbolGlyphs::drawIconGlyph (g, "x", cbText, 9.5f,
                                                 PinkXP::selInk.withAlpha (closeHovered_ || closePressed_ ? 1.0f : 0.86f),
                                                 0, -1);
        }
        else
        {
            const auto cbState = closePressed_ ? PinkXP::ControlState::pressed
                               : closeHovered_ ? PinkXP::ControlState::hover
                                               : PinkXP::ControlState::normal;
            const bool keepNativeHover = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                                          || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass
                                          || tokens.frameStyle == PinkXP::FrameStyle::neonTube
                                          || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
            const auto cbFill  = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor && (closePressed_ || closeHovered_))
                                   ? PinkXP::sel
                               : (tokens.frameStyle == PinkXP::FrameStyle::neonTube && closePressed_)
                                   ? PinkXP::sel
                               : (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass && closePressed_)
                                   ? PinkXP::sel
                               : closePressed_ ? PinkXP::pink100
                               : (closeHovered_ && ! keepNativeHover) ? PinkXP::pink200
                                                                      : PinkXP::btnFace;
            PinkXP::Craft::drawControlChrome (g, cb, cbFill, cbState);

            auto cbText = cb;
            if (closePressed_) cbText.translate (1, 1);
            PinkXP::SymbolGlyphs::drawIconGlyph (g, "x", cbText, 9.5f,
                                                 PinkXP::readableInkOn (cbFill), 0, -1);
        }
    }

    paintTabStrip (g);
}

void ModuleControlDrawer::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    const bool hov = getCloseButtonBounds().contains (pos);
    if (hov != closeHovered_)
    {
        closeHovered_ = hov;
        repaint (getHeaderBarBounds());
    }

    const int tabHov = hitTestTabIndex (pos);
    if (tabHov != hoveredTabIndex_)
    {
        hoveredTabIndex_ = tabHov;
        if (! tabStripLayout_.stripBounds.isEmpty())
            repaint (tabStripLayout_.stripBounds);
    }

    if (! hov && getHeaderBarBounds().contains (pos))
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else if (tabHov >= 0)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ModuleControlDrawer::mouseExit (const juce::MouseEvent&)
{
    if (closeHovered_)
    {
        closeHovered_ = false;
        repaint (getHeaderBarBounds());
    }
    if (hoveredTabIndex_ >= 0)
    {
        hoveredTabIndex_ = -1;
        if (! tabStripLayout_.stripBounds.isEmpty())
            repaint (tabStripLayout_.stripBounds);
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ModuleControlDrawer::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown())
        return;

    const auto pos = e.getPosition();
    if (getCloseButtonBounds().contains (pos))
    {
        closePressed_ = true;
        repaint (getHeaderBarBounds());
        return;
    }

    const int tabIdx = hitTestTabIndex (pos);
    if (tabIdx >= 0)
    {
        selectTab (tabIdx);
        return;
    }

    if (getHeaderBarBounds().contains (pos))
    {
        dragging_ = true;
        dragStartScreen_  = e.getScreenPosition();
        dragStartTopLeft_ = getPosition();
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }
}

void ModuleControlDrawer::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging_)
        return;

    const auto delta = e.getScreenPosition() - dragStartScreen_;
    auto next = dragStartTopLeft_ + delta;
    if (! dragClampBounds_.isEmpty())
    {
        const int maxX = juce::jmax (dragClampBounds_.getX(), dragClampBounds_.getRight()  - getWidth());
        const int maxY = juce::jmax (dragClampBounds_.getY(), dragClampBounds_.getBottom() - getHeight());
        next.x = juce::jlimit (dragClampBounds_.getX(), maxX, next.x);
        next.y = juce::jlimit (dragClampBounds_.getY(), maxY, next.y);
    }
    setTopLeftPosition (next);
    userMoved_ = true;
}

void ModuleControlDrawer::mouseUp (const juce::MouseEvent& e)
{
    if (closePressed_)
    {
        closePressed_ = false;
        const bool stillOn = getCloseButtonBounds().contains (e.getPosition());
        repaint (getHeaderBarBounds());
        if (stillOn)
        {
            finishActiveSliderDrag (true);
            owner.setControlPanelOpen (false);
        }
        return;
    }

    if (dragging_)
    {
        dragging_ = false;
        setMouseCursor (getHeaderBarBounds().contains (e.getPosition())
                            ? juce::MouseCursor::DraggingHandCursor
                            : juce::MouseCursor::NormalCursor);
    }
}

bool ModuleControlDrawer::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        finishActiveSliderDrag (true);
        owner.setControlPanelOpen (false);
        return true;
    }
    // 支持外部桌面弹窗焦点下的 Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y 撤回与重做
    if (auto* ws = owner.findParentComponentOfClass<ModuleWorkspace>())
    {
        const auto mods = key.getModifiers();
        const bool ctrlOrCmd = mods.isCommandDown() || mods.isCtrlDown();
        const int kc = key.getKeyCode();
        const bool isUndoRedoKey = ctrlOrCmd && ! mods.isAltDown()
                                   && (kc == 'Z' || kc == 'z' || (! mods.isShiftDown() && (kc == 'Y' || kc == 'y')));
        if (isUndoRedoKey && activeDragRow_ >= 0
            && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
        {
            finishActiveSliderDrag (true);
        }
        if (ws->handleUndoRedoKeyPress (key))
            return true;
    }
    return false;
}
