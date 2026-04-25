#include "source/ui/PinkXPStyle.h"
#include "BinaryData.h"

// ==========================================================
// PinkXPStyle.cpp —— 像素风配色/绘制/LookAndFeel 的实现
// ==========================================================

// 前向声明：syncPinkXPColours 的完整定义在文件下方的匿名 namespace 里，
//   但 applyTheme 在它之前就需要调用它，这里先给一个局部声明供链接。
namespace
{
    void syncPinkXPColours (juce::LookAndFeel& lf);
}

// ==========================================================
// 字体配置：统一使用 Silkscreen-Regular.ttf
// ==========================================================
namespace PinkXP
{
    static juce::Typeface::Ptr gTypeface;

    void initCustomTypeface(juce::Typeface::Ptr ptr)
    {
        gTypeface = ptr;
    }

    juce::Typeface::Ptr loadActiveTypeface()
    {
        return juce::Typeface::createSystemTypefaceFor(
            BinaryData::SilkscreenRegular_ttf,
            BinaryData::SilkscreenRegular_ttfSize);
    }

    // 内部工厂：按原始 height 生成字体
    static juce::Font makeFontRaw(float height, int styleFlags)
    {
        if (gTypeface != nullptr)
        {
            juce::Font f(gTypeface);
            f.setHeight(height);
            if (styleFlags & juce::Font::bold)   f.setBold(true);
            if (styleFlags & juce::Font::italic) f.setItalic(true);
            return f;
        }
        return juce::Font(height, styleFlags);
    }

    // 通用字体：高度 >= 8 时做 1.5x 放大（UI 主字号），极小字 (<8) 保持原样
    // 坐标轴/刻度数字请显式使用 getAxisFont 以不放大
    juce::Font getFont(float height, int styleFlags)
    {
        const float scaled = (height >= 8.0f) ? height * 1.5f : height;
        return makeFontRaw(scaled, styleFlags);
    }

    // 坐标轴/刻度专用字体：不放大，保持原始尺寸
    juce::Font getAxisFont(float height, int styleFlags)
    {
        return makeFontRaw(height, styleFlags);
    }

    void drawRaised(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill)
    {
        g.setColour(fill);
        g.fillRect(r);

        // 外层 2px 硬边框：左+上 白高光，右+下 深色
        g.setColour(hl);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 2);
        g.fillRect(r.getX(), r.getY(), 2, r.getHeight());
        g.setColour(dark);
        g.fillRect(r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        g.fillRect(r.getRight() - 2, r.getY(), 2, r.getHeight());

        // 内层 1px 高光/阴影
        g.setColour(juce::Colour(0xfffff4f9));
        g.fillRect(r.getX() + 2, r.getY() + 2, r.getWidth() - 4, 1);
        g.fillRect(r.getX() + 2, r.getY() + 2, 1, r.getHeight() - 4);
        g.setColour(shdw);
        g.fillRect(r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 1);
        g.fillRect(r.getRight() - 3, r.getY() + 2, 1, r.getHeight() - 4);
    }

    void drawSunken(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill)
    {
        g.setColour(fill);
        g.fillRect(r);

        g.setColour(dark);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 2);
        g.fillRect(r.getX(), r.getY(), 2, r.getHeight());
        g.setColour(hl);
        g.fillRect(r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        g.fillRect(r.getRight() - 2, r.getY(), 2, r.getHeight());

        g.setColour(shdw);
        g.fillRect(r.getX() + 2, r.getY() + 2, r.getWidth() - 4, 1);
        g.fillRect(r.getX() + 2, r.getY() + 2, 1, r.getHeight() - 4);
        g.setColour(juce::Colours::white);
        g.fillRect(r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 1);
        g.fillRect(r.getRight() - 3, r.getY() + 2, 1, r.getHeight() - 4);
    }

    void drawPressed(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill)
    {
        g.setColour(fill);
        g.fillRect(r);

        g.setColour(dark);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 2);
        g.fillRect(r.getX(), r.getY(), 2, r.getHeight());
        g.setColour(hl);
        g.fillRect(r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        g.fillRect(r.getRight() - 2, r.getY(), 2, r.getHeight());

        g.setColour(shdw);
        g.fillRect(r.getX() + 2, r.getY() + 2, r.getWidth() - 4, 1);
        g.fillRect(r.getX() + 2, r.getY() + 2, 1, r.getHeight() - 4);
    }

    void drawHardShadow(juce::Graphics& g, juce::Rectangle<int> r, int offset)
    {
        g.setColour(dark);
        g.fillRect(r.getRight(), r.getY() + offset, offset, r.getHeight());
        g.fillRect(r.getX() + offset, r.getBottom(), r.getWidth(), offset);
    }

    void drawDesktop(juce::Graphics& g, juce::Rectangle<int> r)
    {
        // 1) 底色
        g.setColour(desktop);
        g.fillRect(r);

        // 2) 按主题纹理绘制
        const auto& th = getCurrentTheme();
        switch (th.desktopPattern)
        {
            case DesktopPattern::checker:
            default:
            {
                // 棋盘格（经典 XP 风）
                g.setColour(desktop2);
                for (int y = r.getY(); y < r.getBottom(); y += 4)
                    for (int x = r.getX() + ((y / 4) % 2) * 4; x < r.getRight(); x += 8)
                        g.fillRect(x, y, 2, 2);
                break;
            }

            case DesktopPattern::pixelStars:
            {
                // 像素小星星（"+" 字形，稀疏分布）
                const int step = 22;
                for (int y = r.getY() + 6; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        // 黄色小星
                        g.setColour(desktop2);
                        g.fillRect(x,     y - 1, 1, 3);
                        g.fillRect(x - 1, y,     3, 1);
                        // 次级小点
                        g.setColour(desktop2.withAlpha(0.5f));
                        g.fillRect(x + (step / 3), y + (step / 3), 1, 1);
                    }
                }
                break;
            }

            case DesktopPattern::scanGrid:
            {
                // 网格扫描线：水平淡线 + 竖直淡线 + 每 N 行一条亮线
                g.setColour(desktop2.withAlpha(0.4f));
                for (int y = r.getY(); y < r.getBottom(); y += 6)
                    g.fillRect(r.getX(), y, r.getWidth(), 1);
                g.setColour(desktop2.withAlpha(0.25f));
                for (int x = r.getX(); x < r.getRight(); x += 6)
                    g.fillRect(x, r.getY(), 1, r.getHeight());
                // 扫描高亮条（每 48px）
                g.setColour(desktop2.withAlpha(0.6f));
                for (int y = r.getY(); y < r.getBottom(); y += 48)
                    g.fillRect(r.getX(), y, r.getWidth(), 1);
                break;
            }

            case DesktopPattern::bigDots:
            {
                // 大圆点（2003 MSN / 复古 iPod 感）
                const int step = 26;
                const int radius = 5;
                for (int y = r.getY() + step / 2; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        g.setColour(desktop2);
                        g.fillEllipse((float)(x - radius), (float)(y - radius),
                                      (float)(radius * 2), (float)(radius * 2));
                    }
                }
                break;
            }

            case DesktopPattern::bubbles:
            {
                // 横条泡泡：每 20px 一条细横带 + 带上随机小泡泡（基于位置哈希稳定）
                const int stripeStep = 20;
                for (int y = r.getY() + stripeStep / 2; y < r.getBottom(); y += stripeStep)
                {
                    g.setColour(desktop2.withAlpha(0.35f));
                    g.fillRect(r.getX(), y, r.getWidth(), 2);

                    // 同一横条上稀疏的珍珠泡泡
                    for (int x = r.getX() + 8; x < r.getRight(); x += 28)
                    {
                        const int h = (x * 2654435761u + y * 40503u) & 0xff;
                        if (h < 140)
                        {
                            const int rad = 2 + (h & 3);
                            g.setColour(juce::Colours::white.withAlpha(0.55f));
                            g.fillEllipse((float)(x - rad), (float)(y - rad),
                                          (float)(rad * 2), (float)(rad * 2));
                        }
                    }
                }
                break;
            }

            case DesktopPattern::diagStripes:
            {
                // 斜条纹（45°）
                const int step = 10;
                g.setColour(desktop2.withAlpha(0.45f));
                for (int k = -r.getHeight(); k < r.getWidth(); k += step)
                {
                    const juce::Line<float> line(
                        (float)(r.getX() + k),                (float) r.getY(),
                        (float)(r.getX() + k + r.getHeight()), (float) r.getBottom());
                    g.drawLine(line, 2.0f);
                }
                break;
            }
        }
    }

    // ------------------------------------------------------
    // drawLogo —— 在 area 中央绘制插件 logo 图片（由调用方提供 Image）
    //   半透明渲染；Image 由 Editor 成员缓存（避免 ImageCache 跨 DLL 悬垂）
    // ------------------------------------------------------
    void drawLogo(juce::Graphics& g, juce::Rectangle<int> area, const juce::Image& logo)
    {
        if (area.getWidth() < 80 || area.getHeight() < 80) return;
        if (logo.isNull()) return;

        // 目标尺寸：取 area 较小边的 80%，保持图片原始宽高比
        // （从 0.40 → 0.80，视觉面积变为原来的 ~4 倍，但边长刚好 2 倍，符合"放大 2 倍"的需求）
        const float targetSide = juce::jmin((float) area.getWidth(),
                                             (float) area.getHeight()) * 0.80f;

        const float imgAspect = (float) logo.getWidth() / (float) logo.getHeight();
        float drawW, drawH;
        if (imgAspect >= 1.0f) {  // 宽 >= 高
            drawW = targetSide;
            drawH = targetSide / imgAspect;
        } else {
            drawH = targetSide;
            drawW = targetSide * imgAspect;
        }

        const float cx = (float) area.getCentreX();
        const float cy = (float) area.getCentreY();
        juce::Rectangle<float> dst(cx - drawW * 0.5f, cy - drawH * 0.5f, drawW, drawH);

        // 40% 透明度绘制（避免喧宾夺主）
        g.setOpacity(0.40f);
        g.drawImage(logo, dst,
                    juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                    false);
        g.setOpacity(1.0f);
    }

    void drawTitleIconText(juce::Graphics& g,
                           juce::Rectangle<int> iconArea,
                           const juce::String& iconText,
                           juce::Colour colour,
                           float fontHeight)
    {
        if (iconArea.getWidth() <= 2 || iconArea.getHeight() <= 2)
            return;

        g.setFont(getFont(fontHeight, juce::Font::bold));
        g.setColour(colour.contrasting().withAlpha(0.55f));
        g.drawText(iconText, iconArea.translated(1, 1), juce::Justification::centred, false);

        g.setColour(colour);
        g.drawText(iconText, iconArea, juce::Justification::centred, false);
    }

    juce::Rectangle<int> drawPinkTitleBar(juce::Graphics& g,
                                          juce::Rectangle<int> bounds,
                                          const juce::String& title,
                                          float fontHeight)
    {
        // 主题底色
        g.setColour(sel);
        g.fillRect(bounds);

        // 顶部 1px 高光 + 底部 1px 暗线 —— 旧版本这里写死了 0xffff9dbf / pink700，
        //   在非粉色主题（Starlight 蓝、Tangerine 橙、Aqua 蓝、Matcha 绿、CyberLilac 紫）
        //   下就会出现"一道粉色横线"的 bug。改为基于当前主题的 `sel` 色做亮/暗推演，
        //   保证任何主题下顶/底线都与标题栏底色同色系。
        g.setColour(sel.brighter(0.35f));
        g.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(), 1);
        g.setColour(sel.darker(0.45f));
        g.fillRect(bounds.getX(), bounds.getBottom() - 1, bounds.getWidth(), 1);

        const auto& th = getCurrentTheme();

        // 左侧主题文本图标
        auto iconRect = juce::Rectangle<int>(bounds.getX() + 5,
                                             bounds.getY() + (bounds.getHeight() - 14) / 2,
                                             16, 14);
        drawTitleIconText(g, iconRect, juce::String::fromUTF8(th.titleIconText), selInk, fontHeight + 0.6f);

        // 标题文字（跟随主题 selInk）
        g.setFont(getFont(fontHeight, juce::Font::bold));
        auto textRect = bounds.reduced(6, 0);
        textRect.removeFromLeft(19); // 给左侧图标留空间

        g.setColour(selInk.contrasting().withAlpha(0.55f));
        g.drawText(title, textRect.translated(1, 1), juce::Justification::centredLeft, false);

        g.setColour(selInk);
        g.drawText(title, textRect, juce::Justification::centredLeft, false);

        return bounds;
    }

    // ======================================================
    // Y2K / 像素装饰工具
    // ======================================================
    void drawScanlines(juce::Graphics& g, juce::Rectangle<int> area,
                       juce::Colour colour, float alpha)
    {
        if (area.isEmpty() || alpha <= 0.0f) return;
        g.setColour(colour.withAlpha(alpha));
        for (int y = area.getY(); y < area.getBottom(); y += 2)
            g.fillRect(area.getX(), y, area.getWidth(), 1);
    }

    void drawPixelCorners(juce::Graphics& g, juce::Rectangle<int> area,
                          juce::Colour colour, int armLen, int thickness)
    {
        if (area.getWidth() < armLen * 2 || area.getHeight() < armLen * 2) return;
        g.setColour(colour);
        const int L = armLen, T = thickness;

        // 左上 L
        g.fillRect(area.getX(), area.getY(), L, T);
        g.fillRect(area.getX(), area.getY(), T, L);
        // 右上 L
        g.fillRect(area.getRight() - L, area.getY(), L, T);
        g.fillRect(area.getRight() - T, area.getY(), T, L);
        // 左下 L
        g.fillRect(area.getX(), area.getBottom() - T, L, T);
        g.fillRect(area.getX(), area.getBottom() - L, T, L);
        // 右下 L
        g.fillRect(area.getRight() - L, area.getBottom() - T, L, T);
        g.fillRect(area.getRight() - T, area.getBottom() - L, T, L);
    }

    void drawCheckerBorder(juce::Graphics& g, juce::Rectangle<int> area,
                           juce::Colour a, juce::Colour b, int cellSize)
    {
        if (area.isEmpty() || cellSize <= 0) return;

        const int x0 = area.getX(), x1 = area.getRight();
        const int y0 = area.getY(), y1 = area.getBottom();

        auto drawRow = [&](int y, bool flip)
        {
            int i = 0;
            for (int x = x0; x < x1; x += cellSize, ++i)
            {
                const bool useA = ((i % 2) == 0) ^ flip;
                g.setColour(useA ? a : b);
                g.fillRect(x, y,
                           juce::jmin(cellSize, x1 - x),
                           juce::jmin(cellSize, y1 - y));
            }
        };
        auto drawCol = [&](int x, bool flip)
        {
            int i = 0;
            for (int y = y0; y < y1; y += cellSize, ++i)
            {
                const bool useA = ((i % 2) == 0) ^ flip;
                g.setColour(useA ? a : b);
                g.fillRect(x, y,
                           juce::jmin(cellSize, x1 - x),
                           juce::jmin(cellSize, y1 - y));
            }
        };

        drawRow(y0,             false);
        drawRow(y1 - cellSize,  true);
        drawCol(x0,             false);
        drawCol(x1 - cellSize,  true);
    }

    void drawPixelStars(juce::Graphics& g, juce::Rectangle<int> area,
                        int phase, int count, juce::Colour colour)
    {
        if (area.getWidth() < 8 || area.getHeight() < 8) return;
        // 简单 LCG 伪随机（不依赖 <random>，保证每帧可复现）
        unsigned int seed = (unsigned int)(phase * 2654435761u);
        auto nextU = [&seed]() -> unsigned int
        {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        for (int i = 0; i < count; ++i)
        {
            const int x = area.getX() + (int)(nextU() % (unsigned int) area.getWidth());
            const int y = area.getY() + (int)(nextU() % (unsigned int) area.getHeight());
            const int brightness = (int)(nextU() % 3u); // 0/1/2 三档亮度
            const float alpha = 0.35f + 0.25f * (float) brightness;

            g.setColour(colour.withAlpha(alpha));
            // Y2K 典型 "+" 字像素星
            g.fillRect(x,     y - 1, 1, 3);
            g.fillRect(x - 1, y,     3, 1);
        }
    }

    void drawDashedBorder(juce::Graphics& g, juce::Rectangle<int> area,
                          juce::Colour colour, int dash)
    {
        if (area.isEmpty() || dash <= 0) return;
        g.setColour(colour);
        const int x0 = area.getX(), x1 = area.getRight() - 1;
        const int y0 = area.getY(), y1 = area.getBottom() - 1;

        // 顶/底
        for (int x = x0; x < x1; x += dash * 2)
            g.fillRect(x, y0, juce::jmin(dash, x1 - x), 1);
        for (int x = x0 + dash; x < x1; x += dash * 2)
            g.fillRect(x, y1, juce::jmin(dash, x1 - x), 1);
        // 左/右
        for (int y = y0; y < y1; y += dash * 2)
            g.fillRect(x0, y, 1, juce::jmin(dash, y1 - y));
        for (int y = y0 + dash; y < y1; y += dash * 2)
            g.fillRect(x1, y, 1, juce::jmin(dash, y1 - y));
    }

    void drawY2KGradient(juce::Graphics& g, juce::Rectangle<int> area,
                         juce::Colour top, juce::Colour bottom, int bands)
    {
        if (area.isEmpty() || bands <= 0) return;
        const int h = area.getHeight();
        for (int i = 0; i < bands; ++i)
        {
            const float t = (float) i / (float) juce::jmax(1, bands - 1);
            const int y1 = area.getY() + (i * h) / bands;
            const int y2 = area.getY() + ((i + 1) * h) / bands;
            g.setColour(top.interpolatedWith(bottom, t));
            g.fillRect(area.getX(), y1, area.getWidth(), juce::jmax(1, y2 - y1));
        }
    }

    // ======================================================
    // 主题系统实现
    // ======================================================
    static const std::vector<Theme>& buildThemes()
    {
        static const std::vector<Theme> themes = {
            // ---- 🌸 Bubblegum（糖果粉，当前默认主题）----
            {
                ThemeId::bubblegum,
                "Bubblegum",
                "Sweet / Girly / Painter",
                juce::Colour(0xfffff3f9), juce::Colour(0xffffe6f1),
                juce::Colour(0xffffd1e6), juce::Colour(0xffffb8d8),
                juce::Colour(0xffff9cc9), juce::Colour(0xffff7bb3),
                juce::Colour(0xffe65e96), juce::Colour(0xffbf3f72),
                juce::Colour(0xffffffff), juce::Colour(0xffffe6f1),
                juce::Colour(0xffe65e96), juce::Colour(0xff5a1a35),
                juce::Colour(0xff3a0f24), juce::Colour(0xffff7bb3),
                juce::Colour(0xffffffff),
                juce::Colour(0xffd977ac), juce::Colour(0xffbf5a91),
                juce::Colour(0xfffffbfe),   // content (更亮粉白)
                juce::Colour(0xffffe6f1),   // btnFace (更浅奶粉)
                juce::Colour(0xffff7bb3),
                DesktopPattern::checker,
                "♡"
            },
            // ---- 🌌 Starlight（星空深蓝 + 黄小星）----
            {
                ThemeId::starlight,
                "Starlight",
                "Midnight / Wish / Stars",
                juce::Colour(0xffe7ecff), juce::Colour(0xffc8d2ff),
                juce::Colour(0xff9fb0f5), juce::Colour(0xff7083e0),
                juce::Colour(0xff4658b5), juce::Colour(0xfff8d74a), // 霓虹黄作为"强调"
                juce::Colour(0xff1c2450), juce::Colour(0xff10163a),
                juce::Colour(0xffffffff), juce::Colour(0xff2a3465),
                juce::Colour(0xff10163a), juce::Colour(0xff060824),
                juce::Colour(0xff10163a), juce::Colour(0xfff8d74a),
                juce::Colour(0xff10163a),
                juce::Colour(0xff10163a), juce::Colour(0xfff8d74a),
                juce::Colour(0xffe7ecff),   // content (淡蓝白)
                juce::Colour(0xffc8d2ff),   // btnFace (淡蓝白，深主题下按钮面)
                juce::Colour(0xff3f51b5),
                DesktopPattern::pixelStars,
                "★"
            },
            // ---- 🪩 Cyber Lilac（赛博紫 + 青）----
            {
                ThemeId::cyberLilac,
                "Cyber Lilac",
                "Cyber / e-Pet / DVD menu",
                juce::Colour(0xfff1e7ff), juce::Colour(0xffd8c2ff),
                juce::Colour(0xffb89aff), juce::Colour(0xff9870ff),
                juce::Colour(0xff8151ff), juce::Colour(0xffff45c6), // 霓虹粉
                juce::Colour(0xff5432b5), juce::Colour(0xff2e1873),
                juce::Colour(0xffffffff), juce::Colour(0xff2a1e5a),
                juce::Colour(0xff6a3cdb), juce::Colour(0xff1a0f44),
                juce::Colour(0xff2e1873), juce::Colour(0xffff45c6),
                juce::Colour(0xff1a0f44),   // selInk (\u6df1\u7d2b\uff1a\u7edf\u4e00\u6df1\u8272\u6587\u5b57\u903b\u8f91)
                juce::Colour(0xff1a0f44), juce::Colour(0xff6affd5), // \u8584\u8377\u9752\u7f51\u683c
                juce::Colour(0xfff1e7ff),   // content (淡紫白)
                juce::Colour(0xffd8c2ff),   // btnFace (淡紫白，深主题下按钮面)
                juce::Colour(0xff8151ff),
                DesktopPattern::scanGrid,
                "⌁"
            },
            // ---- 🍊 Tangerine Pop（橘色波普 + 奶油）----
            {
                ThemeId::tangerinePop,
                "Tangerine Pop",
                "2003 MSN / retro iPod",
                juce::Colour(0xfffff6e8), juce::Colour(0xffffe4bc),
                juce::Colour(0xffffc48a), juce::Colour(0xffffa85d),
                juce::Colour(0xffff8b33), juce::Colour(0xffff6a1f),
                juce::Colour(0xffc04e15), juce::Colour(0xff7a2f08),
                juce::Colour(0xffffffff), juce::Colour(0xfffff2d6),
                juce::Colour(0xffc04e15), juce::Colour(0xff4a1b05),
                juce::Colour(0xff2b1003), juce::Colour(0xffff6a1f),
                juce::Colour(0xffffffff),
                juce::Colour(0xffffb558), juce::Colour(0xfffff2d6),
                juce::Colour(0xfffff6e8),   // content (奶油白)
                juce::Colour(0xffffe4bc),   // btnFace (浅奶油按钮面)
                juce::Colour(0xffff6a1f),
                DesktopPattern::bigDots,
                "●"
            },
            // ---- 🪞 Aqua Pearl（水蓝珠光）----
            {
                ThemeId::aquaPearl,
                "Aqua Pearl",
                "Ocean / early Mac / water fae",
                juce::Colour(0xffe9f6ff), juce::Colour(0xffcae8ff),
                juce::Colour(0xff9cd1f8), juce::Colour(0xff6fb6ea),
                juce::Colour(0xff4a98d0), juce::Colour(0xff2a7aa8),
                juce::Colour(0xff1b5480), juce::Colour(0xff0d3050),
                juce::Colour(0xffffffff), juce::Colour(0xffd7eeff),
                juce::Colour(0xff1b5480), juce::Colour(0xff0a2742),
                juce::Colour(0xff061a2e), juce::Colour(0xff2a7aa8),
                juce::Colour(0xffffffff),
                juce::Colour(0xff4a98d0), juce::Colour(0xffc0d8ea), // 珍珠白
                juce::Colour(0xffe9f6ff),   // content (水白)
                juce::Colour(0xffcae8ff),   // btnFace (浅水蓝按钮面)
                juce::Colour(0xff6fb6ea),
                DesktopPattern::bubbles,
                "◎"
            },
            // ---- 🫧 Matcha Soda（苏打绿 + 粉）----
            {
                ThemeId::matchaSoda,
                "Matcha Soda",
                "Fresh / K-Y2K / stickers",
                juce::Colour(0xfff0fbe6), juce::Colour(0xffd5f1b8),
                juce::Colour(0xffb4e38a), juce::Colour(0xff8fd45c),
                juce::Colour(0xff69bf36), juce::Colour(0xffff8fb5), // 桃粉强调
                juce::Colour(0xff3c8f2c), juce::Colour(0xff245a1a),
                juce::Colour(0xffffffff), juce::Colour(0xffe6f7d4),
                juce::Colour(0xff3c8f2c), juce::Colour(0xff123e0a),
                juce::Colour(0xff0a2405), juce::Colour(0xffff8fb5),
                juce::Colour(0xffffffff),
                juce::Colour(0xff8fd45c), juce::Colour(0xfffff6e8), // 米白斜条
                juce::Colour(0xfff0fbe6),   // content (嫩绿白)
                juce::Colour(0xffd5f1b8),   // btnFace (浅嫩绿按钮面)
                juce::Colour(0xff8fd45c),
                DesktopPattern::diagStripes,
                "\\"
            },
            // ---- 🪟 Windows XP Luna（经典 Bliss 蓝天绿草）----
            //   pink50..pink700 在此主题下被"重新解读"为一组 Luna 蓝色阶
            //   （从极浅 sky → 经典 Luna 深蓝），所有使用 pink* 色的 UI
            //   控件（EQ 格子、Meter、曲线等）自动获得 Luna 蓝配色。
            //   桌面纹理用 bigDots 模拟 XP 经典 Bliss 草地/蓝天的绿色波点。
            {
                ThemeId::winXP,
                "Win XP Luna",
                "Bliss / taskbar / Clearlooks",
                juce::Colour(0xffeaf4ff), juce::Colour(0xffcfe4ff), // pink50/100 淡天蓝
                juce::Colour(0xffa8cdf0), juce::Colour(0xff7db4e3),
                juce::Colour(0xff4a8ed4), juce::Colour(0xff2d6fcd), // pink400/500 Luna 蓝
                juce::Colour(0xff17549c), juce::Colour(0xff0d3b7a),
                juce::Colour(0xffffffff),                           // hl
                juce::Colour(0xffe6effa),                           // face (XP 任务栏浅蓝灰)
                juce::Colour(0xff486fad),                           // shdw
                juce::Colour(0xff0a2a5e),                           // dark
                juce::Colour(0xff0a1e3a),                           // ink
                juce::Colour(0xff0a66b0),                           // sel (XP 标题栏深 Luna 蓝)
                juce::Colour(0xffffffff),                           // selInk
                juce::Colour(0xff3a7d3a),                           // desktop (草地绿)
                juce::Colour(0xff6aa85f),                           // desktop2 (淡草绿)
                juce::Colour(0xfff7fbff),                           // content (near-white 画布)
                juce::Colour(0xffe6effa),                           // btnFace (XP 按钮灰蓝)
                juce::Colour(0xff2d6fcd),                           // swatch (色票 Luna 蓝)
                DesktopPattern::bigDots,
                "\xe2\x96\xa0" // ■（实心方块作为"开始按钮"风图标）
            },
            // ---- 🩸 Crimson Noir（红黑暗夜）----
            //   · pink50..pink700 重新解读为"带淡红调的亮灰阶"（暗→亮），
            //     让仪表图线条（oscilloscope/spectrum 等常用 pink300/400/500）
            //     在黑底上清晰可见；红色气质改由 sel/desktop 承担
            //   · desktopPattern 选 diagStripes：黑底斜纹，经典暗夜感
            {
                ThemeId::crimsonNoir,
                "Crimson Noir",
                "Gothic / Vampire / Late-night mix",
                juce::Colour(0xff3a2a2a), juce::Colour(0xff5a4040), // pink50/100  深灰带红
                juce::Colour(0xff7f6060), juce::Colour(0xffa68888), // pink200/300 中灰带红
                juce::Colour(0xffc9b2b2), juce::Colour(0xffe4d4d4), // pink400/500 亮灰（仪表主线）
                juce::Colour(0xfff2e8e8), juce::Colour(0xfffaf5f5), // pink600/700 近白
                juce::Colour(0xff6d2b2b),                           // hl（高光：暗红）
                juce::Colour(0xff2a0f0f),                           // face（按钮面：近黑深红）
                juce::Colour(0xff1a0606),                           // shdw
                juce::Colour(0xff000000),                           // dark（最外层：黑）
                juce::Colour(0xfff7dada),                           // ink（浅血红文字，在暗底上可读）
                juce::Colour(0xff8a1a1a),                           // sel（标题栏深血红）
                juce::Colour(0xfffff0f0),                           // selInk
                juce::Colour(0xff0a0303),                           // desktop（近纯黑）
                juce::Colour(0xff2a0808),                           // desktop2（暗红纹理）
                juce::Colour(0xff120505),                           // content（画布：深黑红）
                juce::Colour(0xff2a0f0f),                           // btnFace（按钮底）
                juce::Colour(0xff8a1a1a),                           // swatch（色票：血红）
                DesktopPattern::diagStripes,
                "\xe2\x97\x86" // ◆（深色菱形）
            },
            // ---- ⬛ Void Grey（纯黑灰）----
            //   · OLED 友好的极简暗色：纯黑背景 + 中性灰阶 + 微弱白色点缀
            //   · 适合长时间监听 / 深夜工作；不带任何强调色
            //   · pink 色阶按"暗→亮"排列，仪表图线用 pink300/400/500 在黑底上如同白线
            {
                ThemeId::voidGrey,
                "Void Grey",
                "Pure dark / OLED / minimal",
                juce::Colour(0xff2a2a2a), juce::Colour(0xff4a4a4a), // pink50/100  深灰
                juce::Colour(0xff707070), juce::Colour(0xff9a9a9a), // pink200/300 中灰→中亮灰
                juce::Colour(0xffc2c2c2), juce::Colour(0xffe0e0e0), // pink400/500 亮灰（仪表主线）
                juce::Colour(0xfff0f0f0), juce::Colour(0xfffafafa), // pink600/700 近白
                juce::Colour(0xff505050),                           // hl
                juce::Colour(0xff2a2a2a),                           // face
                juce::Colour(0xff1a1a1a),                           // shdw
                juce::Colour(0xff000000),                           // dark（纯黑）
                juce::Colour(0xffe6e6e6),                           // ink（浅灰文字）
                juce::Colour(0xff3d3d3d),                           // sel（标题栏中灰）
                juce::Colour(0xffffffff),                           // selInk
                juce::Colour(0xff000000),                           // desktop（纯黑桌面）
                juce::Colour(0xff1a1a1a),                           // desktop2
                juce::Colour(0xff181818),                           // content（画布：接近纯黑）
                juce::Colour(0xff2a2a2a),                           // btnFace
                juce::Colour(0xff5a5a5a),                           // swatch（色票：中灰）
                DesktopPattern::scanGrid,
                "\xe2\x97\x8f" // ●（黑色实心圆）
            },
            // ---- ⬜ Paper Grey（纯白灰）----
            //   · 纸面极简：纯白背景 + 浅灰阶 + 深灰文字
            //   · 亮色模式、打印机友好、清晰易读
            {
                ThemeId::paperGrey,
                "Paper Grey",
                "Pure light / paper / minimal",
                juce::Colour(0xfffafafa), juce::Colour(0xfff0f0f0), // pink50/100 近白
                juce::Colour(0xffe0e0e0), juce::Colour(0xffc8c8c8),
                juce::Colour(0xffa8a8a8), juce::Colour(0xff7d7d7d), // pink400/500 中灰
                juce::Colour(0xff5a5a5a), juce::Colour(0xff333333),
                juce::Colour(0xffffffff),                           // hl
                juce::Colour(0xffeeeeee),                           // face（浅灰按钮面）
                juce::Colour(0xffbdbdbd),                           // shdw
                juce::Colour(0xff6a6a6a),                           // dark
                juce::Colour(0xff1a1a1a),                           // ink（深灰文字）
                juce::Colour(0xff7d7d7d),                           // sel（中灰标题栏）
                juce::Colour(0xffffffff),                           // selInk
                juce::Colour(0xffe8e8e8),                           // desktop（浅灰桌面）
                juce::Colour(0xfff5f5f5),                           // desktop2
                juce::Colour(0xffffffff),                           // content（画布：纯白）
                juce::Colour(0xffeeeeee),                           // btnFace
                juce::Colour(0xff7d7d7d),                           // swatch（色票：中灰）
                DesktopPattern::checker,
                "\xe2\x97\x8b" // ○（空心圆）
            },
        };
        return themes;
    }

    const std::vector<Theme>& getAllThemes()
    {
        return buildThemes();
    }

    // 当前主题状态
    static ThemeId gCurrentThemeId = ThemeId::bubblegum;

    // 主题变更回调表
    struct ThemeSub { int token; ThemeChangedCallback cb; };
    static std::vector<ThemeSub> gThemeSubs;
    static int gNextThemeSubToken = 1;

    ThemeId getCurrentThemeId() { return gCurrentThemeId; }

    const Theme& getCurrentTheme()
    {
        const auto& all = getAllThemes();
        for (const auto& t : all)
            if (t.id == gCurrentThemeId)
                return t;
        return all.front();
    }

    void applyTheme(ThemeId id)
    {
        gCurrentThemeId = id;
        const Theme& t  = getCurrentTheme();

        // 写入全局调色板
        pink50 = t.pink50;   pink100 = t.pink100; pink200 = t.pink200; pink300 = t.pink300;
        pink400 = t.pink400; pink500 = t.pink500; pink600 = t.pink600; pink700 = t.pink700;

        hl  = t.hl;   face = t.face; shdw = t.shdw; dark = t.dark;
        ink = t.ink;  sel  = t.sel;  selInk = t.selInk;
        desktop  = t.desktop;
        desktop2 = t.desktop2;
        content  = t.content;
        btnFace  = t.btnFace;

        // 刷新 LookAndFeel 内部 ColourScheme 缓存 —— 让 ComboBox 的
        //   arrowColourId/textColourId 等通过 findColour() 读取的控件颜色
        //   立刻跟随新主题（否则会保留构造时的旧配色）。
        syncPinkXPColours (getPinkXPLookAndFeel());

        // 通知所有订阅者
        for (auto& s : gThemeSubs)
            if (s.cb) s.cb();
    }

    int subscribeThemeChanged(ThemeChangedCallback cb)
    {
        const int token = gNextThemeSubToken++;
        gThemeSubs.push_back({ token, std::move(cb) });
        return token;
    }

    void unsubscribeThemeChanged(int token)
    {
        for (auto it = gThemeSubs.begin(); it != gThemeSubs.end(); ++it)
        {
            if (it->token == token) { gThemeSubs.erase(it); return; }
        }
    }
}

// ==========================================================
// PinkXPLookAndFeel
// ==========================================================
namespace
{
    // 将当前 PinkXP 全局调色板（pink*/ink/btnFace/sel/selInk 等）同步写入
    // 指定 LookAndFeel 的 ColourScheme 缓存里。
    //   · 构造时调用一次：建立初始配色
    //   · applyTheme 切主题后再调一次：更新缓存，让通过 findColour() 取色的
    //     控件（ComboBox 的文字/箭头、PopupMenu、Slider textbox 等）立刻跟随
    //     新主题，无需各处重新 setColour。
    void syncPinkXPColours (juce::LookAndFeel& lf)
    {
        lf.setColour (juce::Slider::backgroundColourId,        PinkXP::btnFace);
        lf.setColour (juce::Slider::trackColourId,             PinkXP::sel);
        lf.setColour (juce::Slider::thumbColourId,             PinkXP::btnFace);
        lf.setColour (juce::Slider::textBoxTextColourId,       PinkXP::ink);
        lf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
        lf.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);

        lf.setColour (juce::Label::textColourId,               PinkXP::ink);
        lf.setColour (juce::Label::backgroundColourId,         juce::Colours::transparentBlack);

        lf.setColour (juce::TextButton::buttonColourId,        PinkXP::btnFace);
        lf.setColour (juce::TextButton::buttonOnColourId,      PinkXP::pink200);
        lf.setColour (juce::TextButton::textColourOffId,       PinkXP::ink);
        lf.setColour (juce::TextButton::textColourOnId,        PinkXP::ink);

        lf.setColour (juce::PopupMenu::backgroundColourId,            PinkXP::btnFace);
        lf.setColour (juce::PopupMenu::textColourId,                  PinkXP::ink);
        lf.setColour (juce::PopupMenu::highlightedBackgroundColourId, PinkXP::sel);
        lf.setColour (juce::PopupMenu::highlightedTextColourId,       PinkXP::selInk);

        // ComboBox —— 跟随 Pink XP 主题（消除默认的 LookAndFeel_V4 浅蓝配色）
        lf.setColour (juce::ComboBox::backgroundColourId,     PinkXP::btnFace);
        lf.setColour (juce::ComboBox::textColourId,           PinkXP::ink);
        lf.setColour (juce::ComboBox::outlineColourId,        juce::Colours::transparentBlack);
        lf.setColour (juce::ComboBox::buttonColourId,         PinkXP::btnFace);
        lf.setColour (juce::ComboBox::arrowColourId,          PinkXP::ink);
        lf.setColour (juce::ComboBox::focusedOutlineColourId, juce::Colours::transparentBlack);
    }
}

PinkXPLookAndFeel::PinkXPLookAndFeel()
{
    syncPinkXPColours (*this);
}

void PinkXPLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                             const juce::Colour&, bool, bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds();
    const bool toggled = button.getToggleState();
    const bool pressed = shouldDrawButtonAsDown || toggled;

    if (pressed)
    {
        PinkXP::drawPressed(g, bounds, PinkXP::pink100);
        g.setColour(PinkXP::pink200);
        for (int py = bounds.getY() + 3; py < bounds.getBottom() - 2; py += 4)
            for (int px = bounds.getX() + 3 + ((py / 4) % 2) * 2; px < bounds.getRight() - 2; px += 4)
                g.fillRect(px, py, 2, 2);
    }
    else
    {
        PinkXP::drawRaised(g, bounds, PinkXP::btnFace);
    }
}

void PinkXPLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                       bool, bool shouldDrawButtonAsDown)
{
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(12.0f, juce::Font::bold));

    auto area = button.getLocalBounds();
    if (shouldDrawButtonAsDown || button.getToggleState())
        area.translate(1, 1);

    g.drawText(button.getButtonText(), area, juce::Justification::centred, false);
}

juce::Font PinkXPLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return PinkXP::getFont(juce::jmin(14.0f, buttonHeight * 0.55f), juce::Font::bold);
}

void PinkXPLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                                         float sliderPos, float, float,
                                         const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const bool isVertical = (style == juce::Slider::LinearVertical);

    juce::Rectangle<int> track;
    if (isVertical)
    {
        const int tw = 8;
        track = juce::Rectangle<int>(x + (w - tw) / 2, y, tw, h);
    }
    else
    {
        const int th = 8;
        track = juce::Rectangle<int>(x, y + (h - th) / 2, w, th);
    }
    PinkXP::drawSunken(g, track, juce::Colours::white);

    if (isVertical)
    {
        int fillTop = juce::jlimit(track.getY() + 2, track.getBottom() - 2, (int) sliderPos);
        g.setColour(PinkXP::sel);
        g.fillRect(track.getX() + 2, fillTop, track.getWidth() - 4, track.getBottom() - 2 - fillTop);
    }
    else
    {
        int fillRight = juce::jlimit(track.getX() + 2, track.getRight() - 2, (int) sliderPos);
        g.setColour(PinkXP::sel);
        g.fillRect(track.getX() + 2, track.getY() + 2, fillRight - (track.getX() + 2), track.getHeight() - 4);
    }

    const int thumbW = isVertical ? juce::jmin(w - 4, 18) : 12;
    const int thumbH = isVertical ? 12 : juce::jmin(h - 4, 18);
    juce::Rectangle<int> thumb;
    if (isVertical)
        thumb = juce::Rectangle<int>(x + (w - thumbW) / 2, (int) sliderPos - thumbH / 2, thumbW, thumbH);
    else
        thumb = juce::Rectangle<int>((int) sliderPos - thumbW / 2, y + (h - thumbH) / 2, thumbW, thumbH);

    PinkXP::drawRaised(g, thumb, PinkXP::btnFace);

    juce::ignoreUnused(slider);
}

juce::Font PinkXPLookAndFeel::getLabelFont(juce::Label&)
{
    return PinkXP::getFont(11.0f, juce::Font::bold);
}

juce::Label* PinkXPLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox(slider);
    l->setFont(PinkXP::getFont(11.0f, juce::Font::plain));
    l->setColour(juce::Label::textColourId,       PinkXP::ink);
    l->setColour(juce::Label::backgroundColourId, PinkXP::btnFace);
    l->setColour(juce::Label::outlineColourId,    juce::Colours::transparentBlack);
    return l;
}

void PinkXPLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    PinkXP::drawRaised(g, { 0, 0, width, height }, PinkXP::btnFace);
}

void PinkXPLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                          bool isSeparator, bool isActive, bool isHighlighted,
                                          bool isTicked, bool hasSubMenu,
                                          const juce::String& text, const juce::String& /*shortcutKeyText*/,
                                          const juce::Drawable* /*icon*/, const juce::Colour* /*textColour*/)
{
    if (isSeparator)
    {
        g.setColour(PinkXP::shdw);
        g.fillRect(area.getX() + 6, area.getCentreY(), area.getWidth() - 12, 1);
        g.setColour(PinkXP::hl);
        g.fillRect(area.getX() + 6, area.getCentreY() + 1, area.getWidth() - 12, 1);
        return;
    }

    if (isHighlighted && isActive)
    {
        g.setColour(PinkXP::sel);
        g.fillRect(area);
        g.setColour(PinkXP::selInk);
    }
    else
    {
        g.setColour(isActive ? PinkXP::ink : PinkXP::ink.withAlpha(0.45f));
    }

    g.setFont(PinkXP::getFont(12.0f, juce::Font::bold));

    auto textArea = area.reduced(8, 0);
    if (isTicked)
    {
        auto tick = textArea.removeFromLeft(14);
        g.drawText(juce::String::fromUTF8("\xe2\x9c\x93"), tick, juce::Justification::centredLeft, false); // ✓
    }
    else
    {
        textArea.removeFromLeft(14);
    }

    g.drawText(text, textArea, juce::Justification::centredLeft, false);

    if (hasSubMenu)
    {
        auto arrow = area;
        g.drawText(">", arrow.removeFromRight(12), juce::Justification::centred, false);
    }
}

juce::Font PinkXPLookAndFeel::getPopupMenuFont()
{
    return PinkXP::getFont(12.0f, juce::Font::bold);
}

// ==========================================================
// ComboBox —— Pink XP 像素风
//   · 整体框体沿用 drawRaised（按下时 drawPressed），与按钮/面板风格一致
//   · 右侧 buttonArea 单独画一条像素竖分隔线 + 像素 "▼" 箭头
//   · 字体与 Label 同步，使用 PinkXP::getFont（11.5pt）
// ==========================================================
void PinkXPLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height,
                                     bool isButtonDown,
                                     int buttonX, int buttonY, int buttonW, int buttonH,
                                     juce::ComboBox& box)
{
    juce::ignoreUnused(buttonW, buttonH);

    const juce::Rectangle<int> bounds(0, 0, width, height);

    // 1) 外壳：按下时 sunken，其余时候 raised（与按钮一致的像素风）
    if (isButtonDown || box.isPopupActive())
        PinkXP::drawPressed(g, bounds, PinkXP::pink100);
    else
        PinkXP::drawRaised(g, bounds, PinkXP::btnFace);

    // 2) 右侧"▼"按钮区：用一条像素竖线与文本区域分隔
    if (buttonX > 2)
    {
        g.setColour(PinkXP::shdw);
        g.fillRect(buttonX, 2, 1, height - 4);
        g.setColour(PinkXP::hl);
        g.fillRect(buttonX + 1, 2, 1, height - 4);
    }

    // 3) 像素三角箭头（用填充矩形阶梯组成，保持 8-bit 感）
    //    尺寸固定：7x4 像素；居中放在按钮区
    const int arrowW = 7;
    const int arrowH = 4;
    const int ax = buttonX + (width - buttonX - arrowW) / 2;
    const int ay = (height - arrowH) / 2 + (isButtonDown ? 1 : 0);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId)
                  .withAlpha(box.isEnabled() ? 1.0f : 0.4f));
    // 从宽到窄的 4 行
    g.fillRect(ax,     ay,     arrowW,     1); // 7
    g.fillRect(ax + 1, ay + 1, arrowW - 2, 1); // 5
    g.fillRect(ax + 2, ay + 2, arrowW - 4, 1); // 3
    g.fillRect(ax + 3, ay + 3, 1,          1); // 1
}

juce::Font PinkXPLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return PinkXP::getFont(11.5f, juce::Font::bold);
}

void PinkXPLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    // 让文本留出右侧箭头按钮宽度（与 LookAndFeel_V4 保持一致的布局，但加一点 padding）
    const int arrowButtonW = juce::jmin(20, box.getHeight());
    label.setBounds(4, 1,
                    box.getWidth() - arrowButtonW - 4,
                    box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId,
                    box.findColour(juce::ComboBox::textColourId));
}

PinkXPLookAndFeel& getPinkXPLookAndFeel()
{
    static PinkXPLookAndFeel lnf;
    return lnf;
}