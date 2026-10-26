#include "source/ui/PinkXPStyle.h"
#include "BinaryData.h"
#include "source/perf/PerformanceCounterSystem.h"
#include "source/ui/palette/PaletteGenerator.h"
#include "source/Y2KLogging.h"

#include <unordered_map>

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

        // 全平台字体一致性注入：
        //   让所有走 JUCE 默认字体路径的控件（未显式 setFont 的 Label/Menu 等）
        //   也统一使用自定义 Typeface。macOS 下若不注入会回退系统字体；Windows
        //   下同样收益（彻底避免 JUCE 8 新字体栈在某些控件上回退到系统字体）。
        juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface (ptr);

        // 主界面实际使用的是 PinkXPLookAndFeel，需同步注入同一 Typeface；
        // 否则会出现"弹窗字体已变更，但主界面仍回退系统字体"的不一致。
        getPinkXPLookAndFeel().setDefaultSansSerifTypeface (ptr);
    }

    juce::Typeface::Ptr loadActiveTypeface()
    {
        return juce::Typeface::createSystemTypefaceFor(
            BinaryData::SilkscreenRegular_ttf,
            BinaryData::SilkscreenRegular_ttfSize);
    }

    // ==========================================================
    // UI 密度缩放（低分辨率适配）
    // ==========================================================
    static float gUiScale = 1.0f;

    // stick 驻留态（Windows AppBar）：供 LnF 调整 PopupMenu / ComboBox 的弹出锚点
    static bool gStickDockActive = false;
    static bool gStickDockBottom = false;

    void setStickDockState (bool active, bool bottom) noexcept
    {
        gStickDockActive = active;
        gStickDockBottom = active && bottom;
    }

    bool isStickDockActive() noexcept { return gStickDockActive; }
    bool isStickDockBottom() noexcept { return gStickDockBottom; }

    float uiScale() noexcept { return gUiScale; }

    void setUiScale (float s) noexcept
    {
        gUiScale = juce::jlimit (0.75f, 1.3f, s);
    }

    int ui (int px) noexcept
    {
        return juce::jmax (1, juce::roundToInt ((float) px * uiScale()));
    }

    float uiScaleForDisplay (const juce::Rectangle<int>& displayArea) noexcept
    {
        const int w = displayArea.getWidth();
        const int h = displayArea.getHeight();
        const int minDim = juce::jmin (w, h);

        // 按"显示屏逻辑分辨率"分级（传 totalArea —— 不要传 userArea：
        //   后者会被任务栏 / 自己的 AppBar 驻留压缩，stick 条拉高时会误判为矮屏）。
        // JUCE 的显示器尺寸已是 DPI 缩放后的逻辑像素：
        //   · ≤720 / ≤768 矮屏：1280×720、1024×768 等 → 0.75 紧凑
        //   · ≤800：1366×768、1280×800 等 → 0.8
        //   · ≤900：1440×900、1600×900 等 → 0.9
        //   · 更高分辨率 → 1.0 原始
        if (h <= 720 || minDim <= 768)  return 0.75f;
        if (h <= 800)                   return 0.80f;
        if (h <= 900)                   return 0.90f;
        return 1.0f;
    }

    // 内部工厂：按原始 height 生成字体
    //
    // 关键说明（JUCE 8 + macOS 字体回退问题修复）：
    //   旧的 juce::Font(typeface) + setBold/setItalic 路径，在 JUCE 8 的
    //   新字体栈下会按"family name + bold/italic"再去系统里查字型变体。
    //   Silkscreen 根本没有 bold/italic 变体，于是 macOS 下就回退到系统字体，
    //   只有 plain 样式（如抬头 iisaacbeats.cn 那一行）仍能命中自定义 Typeface，
    //   其余 bold/italic 文本全部变成系统字体——这正是本次字体异常的根因。
    //
    //   修复：改用 juce::FontOptions(gTypeface).withHeight(height)，
    //   直接锁定 Typeface 实例，不走 family name 再查询的回退路径；
    //   并忽略 styleFlags（Silkscreen 只有一种字形，bold/italic 仅靠
    //   调用方做伪粗/伪斜即可，不应触发 Typeface 替换）。
    //
    // 踩坑记录（Debug 崩溃）：
    //   最初写法 juce::FontOptions(height).withTypeface(gTypeface) 在 Debug 下会
    //   触发 juce_FontOptions.h:126 附近的 jassert：
    //       jassert (x == nullptr || style.isEmpty());
    //   原因是 FontOptions(float) 会级联调用 (String&, String&, float) 版本，
    //   把 style 字段先塞成 "Regular"（Font::plain 转出来的字体样式名），此时
    //   再 withTypeface(非空指针) 就会命中 assert。Release 版 jassert 是空操作，
    //   看似正常；Debug 下 int3 直接卡死在启动阶段（软件窗口都没弹出来）。
    //   正确写法：使用 FontOptions(Typeface::Ptr) 构造函数，让 name/style
    //   从一开始就和 typeface 保持一致（withTypeface 内部同样是这样做的），
    //   然后 withHeight() 覆盖字号即可。
    static juce::Font makeFontRaw(float height, int styleFlags)
    {
        if (gTypeface != nullptr)
        {
            // 单一自定义 Typeface 兜底所有样式，避免 bold/italic 场景回退系统字体
            juce::ignoreUnused (styleFlags);
            return juce::Font (juce::FontOptions (gTypeface).withHeight (height));
        }
        return juce::Font (juce::FontOptions (height, styleFlags));
    }

    // 通用字体：高度 >= 8 时做 1.5x 放大（UI 主字号），极小字 (<8) 保持原样
    // 坐标轴/刻度数字请显式使用 getAxisFont 以不放大
    juce::Font getFont(float height, int styleFlags)
    {
        const float scaled = (height >= 8.0f) ? height * 1.5f : height;
        return makeFontRaw(scaled * uiScale(), styleFlags);
    }

    // ======================================================
    // 静态文本位图缓存（v2.9.6 性能）
    //   mac 上 Editor GL（CPE）渲染下，drawText 每帧触发 HarfBuzz 塑形 +
    //   逐 glyph 的 GL 调用，NSOpenGL 驱动（Metal 转译层）下开销是
    //   Windows 的约 10 倍。标题栏、轴标签等「内容不变但组件每帧全量
    //   重绘」的静态文本是 GL 线程最大热点。
    //   这里把静态文本一次性 CPU 光栅化成 ARGB Image，之后每帧只需
    //   一次 drawImageAt（单纹理 quad），热点降一个数量级。
    //   缓存 key 含文本/字号/颜色/缩放，主题切换后 key 变化自然失效；
    //   容量上限保护避免极端主题组合下内存失控。
    // ======================================================
    juce::Image cachedStaticTextImage (const juce::String& text,
                                       float fontHeight,
                                       int styleFlags,
                                       juce::Colour colour,
                                       juce::Colour shadowColour,
                                       bool axisFont)
    {
        if (text.isEmpty())
            return {};

        const juce::String key = text + "|" + juce::String (fontHeight) + "|"
                               + juce::String (styleFlags) + "|"
                               + colour.toString() + "|"
                               + shadowColour.toString() + "|"
                               + juce::String (uiScale()) + "|"
                               + juce::String (axisFont ? 1 : 0);

        static std::unordered_map<juce::String, juce::Image> cache;

        const auto it = cache.find (key);
        if (it != cache.end())
            return it->second;

        const juce::Font font = axisFont ? getAxisFont (fontHeight, styleFlags)
                                         : getFont (fontHeight, styleFlags);
        // +2 为阴影偏移与字模边界留白
        const int w = juce::roundToInt (font.getStringWidthFloat (text)) + 2;
        const int h = juce::roundToInt (font.getHeight()) + 2;
        if (w <= 2 || h <= 2)
            return {};

        juce::Image img (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
        juce::Graphics g2 (img);
        g2.setFont (font);
        if (! shadowColour.isTransparent())
        {
            g2.setColour (shadowColour);
            g2.drawText (text, 1, 1, w - 2, h - 2, juce::Justification::left, false);
        }
        g2.setColour (colour);
        g2.drawText (text, 0, 0, w - 2, h - 2, juce::Justification::left, false);

        // 容量保护：超限整体清空（静态文本数量有限，正常远达不到）
        if (cache.size() > 512)
            cache.clear();
        cache.emplace (key, img);
        return img;
    }

    // 坐标轴/刻度专用字体：不放大，保持原始尺寸
    juce::Font getAxisFont(float height, int styleFlags)
    {
        return makeFontRaw(height, styleFlags);
    }

    juce::String formatFreqHz(float hz)
    {
        if (hz >= 1000.0f)
            return juce::String(hz / 1000.0f, 1) + " kHz";
        return juce::String((int) std::lround(hz)) + " Hz";
    }

    void drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas,
                        juce::Point<int> pos, const juce::String& readout)
    {
        if (readout.isEmpty())
            return;

        const int x = pos.x;
        const int y = pos.y;

        // 十字线（竖线 + 横线），半透明墨色，覆盖整个仪表区域
        g.setColour(ink.withAlpha(0.55f));
        g.drawVerticalLine(x, (float) canvas.getY(), (float) canvas.getBottom());
        g.drawHorizontalLine(y, (float) canvas.getX(), (float) canvas.getRight());

        // 读数框：绘制在鼠标右上方，尺寸按文本估算。
        //   反馈修复（外网 #5）：旧版硬编码 9px 太小；改为带 1.5x UI 放大的
        //   getFont，框高随字体实际高度自适应（10 号再微调收敛）。
        const int   fontH = 10;
        const int   padX  = 6;
        const int   padY  = 4;
        const auto  readoutFont = getFont ((float) fontH, juce::Font::plain);
        const int   textH = juce::jmax (16, (int) std::ceil (readoutFont.getHeight()));
        const int   boxH  = textH + padY * 2;
        const int   textW = juce::jmax(40, (int) std::ceil(readoutFont.getStringWidth(readout)) + padX * 2);

        int bx = x + 12;
        int by = y - boxH - 8;
        // 避免读数框超出仪表区域：右侧/顶部越界时回退到鼠标左/下侧
        if (bx + textW > canvas.getRight())
            bx = x - textW - 12;
        if (by < canvas.getY())
            by = y + 8;
        bx = juce::jlimit(canvas.getX(), canvas.getRight() - textW, bx);
        by = juce::jlimit(canvas.getY(), canvas.getBottom() - boxH, by);

        g.setColour(ink.withAlpha(0.85f));
        g.fillRect(bx, by, textW, boxH);
        g.setColour(hl);
        g.setFont(readoutFont);
        g.drawText(readout, bx, by, textW, boxH, juce::Justification::centred, false);
    }

    void drawRaised(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill)
    {
        g.setColour(fill);
        g.fillRect(r);
        drawRaisedFrame (g, r);
    }

    void drawRaisedFrame(juce::Graphics& g, juce::Rectangle<int> r)
    {
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

    // ==========================================================
    // 反馈修复（外网 #3 第二轮）：主题自带的 desktop2 在极端明暗主题下
    // 与 desktop 的明度差过小（voidGrey 黑对黑 / paperGrey 白对白 /
    // crimsonNoir 近黑对近黑），纹理完全不可见。
    //
    // 这里不再直接使用 desktop2，而是在 OKLab 空间重算纹理色：
    //   · 明度：从 desktop 的 L 向反方向推离 kMinDeltaL —— 深色桌面提亮、
    //     浅色桌面压暗，保证感知对比；
    //   · 色相：优先沿用 desktop2；desktop2 接近中性灰时借用强调色 sel，
    //     让纹理始终带主题气质而非隐形灰。
    // ==========================================================
    juce::Colour makeDesktopTextureColour()
    {
        using namespace y2kpal;

        const Rgb8 bg8 { desktop.getRed(),  desktop.getGreen(),  desktop.getBlue()  };
        const Rgb8 tn8 { desktop2.getRed(), desktop2.getGreen(), desktop2.getBlue() };

        const OkLab bgLab = linearToOkLab (rgbToLinear (fromRgb8 (bg8)));
        const OkLab tnLab = linearToOkLab (rgbToLinear (fromRgb8 (tn8)));

        // 1) 目标明度：远离背景明度（kMinDeltaL 的 OKLab 明度差肉眼可辨）
        constexpr float kMinDeltaL = 0.24f;
        const float targetL = clamp01 (bgLab.L >= 0.5f
                                        ? bgLab.L - kMinDeltaL
                                        : bgLab.L + kMinDeltaL);

        // 2) 色相/色度：desktop2 有色相则沿用，近中性则借 sel
        const float tnC = std::sqrt (tnLab.a * tnLab.a + tnLab.b * tnLab.b);
        float C, H;
        if (tnC >= 0.03f)
        {
            C = tnC;
            H = wrapHue (std::atan2 (tnLab.b, tnLab.a) * (180.0f / 3.14159265358979323846f));
        }
        else
        {
            const OkLab acLab = linearToOkLab (rgbToLinear (fromRgb8 ({ sel.getRed(),
                                                                       sel.getGreen(),
                                                                       sel.getBlue() })));
            C = std::sqrt (acLab.a * acLab.a + acLab.b * acLab.b);
            H = wrapHue (std::atan2 (acLab.b, acLab.a) * (180.0f / 3.14159265358979323846f));
        }

        OkLch out { targetL, C, H };
        out = gamutClamp (out);
        const Rgb8 rgb = okLchToRgb8 (out);
        return juce::Colour ((juce::uint32) 0xff000000u
                           | ((juce::uint32) rgb.r << 16)
                           | ((juce::uint32) rgb.g << 8)
                           |  (juce::uint32) rgb.b);
    }

    void drawDesktop(juce::Graphics& g, juce::Rectangle<int> r)
    {
        // y2k::perf::ScopedPerfTimer desktopTimer(y2k::perf::FunctionId::uiDrawDesktop,
        //                                         y2k::perf::Partition::uiRendering,
        //                                         y2k::perf::ThreadRole::ui);

        // 1) 底色
        g.setColour(desktop);
        g.fillRect(r);

        // 2) 纹理色（反馈 #3：按背景明度保证对比）+ 高光点缀色
        const juce::Colour tex = makeDesktopTextureColour();
        const bool bgIsDark = desktop.getBrightness() < 0.5f;
        const juce::Colour texHi = bgIsDark ? juce::Colours::white : juce::Colours::black;

        // 3) 按主题纹理绘制
        const auto& th = getCurrentTheme();
        switch (th.desktopPattern)
        {
            case DesktopPattern::checker:
            default:
            {
                // 棋盘格（经典 XP 风，v2.10 增大色块让纹理更醒目）
                g.setColour(tex);
                for (int y = r.getY(); y < r.getBottom(); y += 8)
                    for (int x = r.getX() + ((y / 8) % 2) * 8; x < r.getRight(); x += 16)
                        g.fillRect(x, y, 4, 4);
                break;
            }

            case DesktopPattern::pixelStars:
            {
                // 像素星星（"+" 字 + 对角小臂，v2.10 更大更密）
                const int step = 18;
                for (int y = r.getY() + 8; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        g.setColour(tex);
                        g.fillRect(x,     y - 2, 1, 5);          // 竖臂
                        g.fillRect(x - 2, y,     5, 1);          // 横臂
                        g.fillRect(x - 1, y - 1, 1, 1);          // 对角
                        g.fillRect(x + 1, y + 1, 1, 1);
                        g.fillRect(x + 1, y - 1, 1, 1);
                        g.fillRect(x - 1, y + 1, 1, 1);
                        // 次级小点
                        g.setColour(tex.withAlpha(0.7f));
                        g.fillRect(x + step / 3, y + step / 3, 1, 1);
                        g.fillRect(x - step / 3, y + step / 3, 1, 1);
                    }
                }
                break;
            }

            case DesktopPattern::scanGrid:
            {
                // 网格扫描线：水平淡线 + 竖直淡线 + 每 N 行一条亮线
                g.setColour(tex.withAlpha(0.85f));
                for (int y = r.getY(); y < r.getBottom(); y += 6)
                    g.fillRect(r.getX(), y, r.getWidth(), 1);
                g.setColour(tex.withAlpha(0.55f));
                for (int x = r.getX(); x < r.getRight(); x += 6)
                    g.fillRect(x, r.getY(), 1, r.getHeight());
                // 扫描高亮条（每 48px）
                g.setColour(texHi.withAlpha(0.9f));
                for (int y = r.getY(); y < r.getBottom(); y += 48)
                    g.fillRect(r.getX(), y, r.getWidth(), 1);
                break;
            }

            case DesktopPattern::bigDots:
            {
                // 大圆点（2003 MSN / 复古 iPod 感，v2.10 圆点更大 + 高光点）
                const int step = 30;
                const int radius = 7;
                for (int y = r.getY() + step / 2; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        g.setColour(tex);
                        g.fillEllipse((float)(x - radius), (float)(y - radius),
                                      (float)(radius * 2), (float)(radius * 2));
                        // 左上高光小点，增加立体感
                        g.setColour(texHi.withAlpha(0.45f));
                        g.fillEllipse((float)(x - radius + 3), (float)(y - radius + 3),
                                      3.0f, 3.0f);
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
                    g.setColour(tex.withAlpha(0.8f));
                    g.fillRect(r.getX(), y, r.getWidth(), 2);

                    // 同一横条上稀疏的珍珠泡泡
                    for (int x = r.getX() + 8; x < r.getRight(); x += 28)
                    {
                        const int h = (x * 2654435761u + y * 40503u) & 0xff;
                        if (h < 140)
                        {
                            const int rad = 2 + (h & 3);
                            g.setColour(texHi.withAlpha(0.65f));
                            g.fillEllipse((float)(x - rad), (float)(y - rad),
                                          (float)(rad * 2), (float)(rad * 2));
                        }
                    }
                }
                break;
            }

            case DesktopPattern::diagStripes:
            {
                // 斜条纹（45°，v2.10 加粗 + 双色深浅交替）
                const int step = 16;
                for (int k = -r.getHeight(); k < r.getWidth(); k += step)
                {
                    const juce::Line<float> line(
                        (float)(r.getX() + k),                (float) r.getY(),
                        (float)(r.getX() + k + r.getHeight()), (float) r.getBottom());
                    g.setColour(tex);
                    g.drawLine(line, 4.0f);
                    const juce::Line<float> line2(
                        (float)(r.getX() + k + 2),                (float) r.getY(),
                        (float)(r.getX() + k + 2 + r.getHeight()), (float) r.getBottom());
                    g.setColour(texHi.withAlpha(0.35f));
                    g.drawLine(line2, 1.0f);
                }
                break;
            }

            case DesktopPattern::hearts:
            {
                // 像素爱心：7x7 小桃心均匀平铺
                //    shape (每个 X 为一个 1px 填充点)：
                //      . X . . . X .
                //      X X X X X X X
                //      X X X X X X X
                //      . X X X X X .
                //      . . X X X . .
                //      . . . X . . .
                constexpr int kStep = 18;
                auto drawHeart = [&](int cx, int cy) {
                  // Row 0（top）
                  g.fillRect(cx - 2, cy - 3, 1, 1);
                  g.fillRect(cx + 2, cy - 3, 1, 1);
                  // Row 1
                  for (int i = -3; i <= 3; ++i) g.fillRect(cx + i, cy - 2, 1, 1);
                  // Row 2
                  for (int i = -3; i <= 3; ++i) g.fillRect(cx + i, cy - 1, 1, 1);
                  // Row 3
                  for (int i = -2; i <= 2; ++i) g.fillRect(cx + i, cy,     1, 1);
                  // Row 4
                  for (int i = -1; i <= 1; ++i) g.fillRect(cx + i, cy + 1, 1, 1);
                  // Row 5（bottom tip）
                  g.fillRect(cx, cy + 2, 1, 1);
                };
                g.setColour(tex);
                const int offset = kStep / 2;
                for (int y = r.getY() + 6; y < r.getBottom(); y += kStep) {
                  const int xPhase = ((y / kStep) % 2) * offset;
                  for (int x = r.getX() + 6 + xPhase; x < r.getRight(); x += kStep) {
                    drawHeart(x, y);
                  }
                }
                break;
            }

            case DesktopPattern::zigzag:
            {
                // 锯齿波纹：水平折线，每行错相位，形成波浪感
                const int period = 48;
                const int amp = 8;
                for (int y = r.getY() + 4; y < r.getBottom(); y += 26)
                {
                    g.setColour(tex.withAlpha(0.9f));
                    for (int x = r.getX(); x < r.getRight(); x += 1)
                    {
                        const int phase = (x / period) % 2;
                        const int zy = (phase == 0)
                            ? y + ((x % period) * amp / (period / 2))
                            : y + amp - (((x % period) - period / 2) * amp / (period / 2));
                        if (zy >= r.getY() && zy < r.getBottom())
                            g.fillRect(x, zy, 1, 2);
                    }
                }
                break;
            }

            case DesktopPattern::crossHatch:
            {
                // 交叉斜线网：两组 45° 线交织，形成菱形格网
                const int step = 22;
                for (int k = -r.getHeight(); k < r.getWidth(); k += step)
                {
                    g.setColour(tex.withAlpha(0.75f));
                    const juce::Line<float> a(
                        (float)(r.getX() + k),                (float) r.getY(),
                        (float)(r.getX() + k + r.getHeight()), (float) r.getBottom());
                    g.drawLine(a, 2.0f);
                    const juce::Line<float> b(
                        (float)(r.getX() + k),                (float) r.getBottom(),
                        (float)(r.getX() + k + r.getHeight()), (float) r.getY());
                    g.drawLine(b, 2.0f);
                }
                break;
            }

            case DesktopPattern::pixelFlower:
            {
                // 像素小花：中心点 + 上下左右四花瓣（5x5 十字花），错位平铺
                const int step = 26;
                for (int y = r.getY() + 8; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + 8 + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        g.setColour(tex);
                        g.fillRect(x - 1, y - 3, 3, 2);      // 上花瓣
                        g.fillRect(x - 1, y + 2, 3, 2);      // 下花瓣
                        g.fillRect(x - 3, y - 1, 2, 3);      // 左花瓣
                        g.fillRect(x + 2, y - 1, 2, 3);      // 右花瓣
                        g.setColour(texHi.withAlpha(0.8f));
                        g.fillRect(x, y, 1, 1);              // 花蕊
                    }
                }
                break;
            }
        }
    }

    // ------------------------------------------------------
    // 跨实例共享桌面纹理缓存（P6）
    //   · key：当前主题 id + (width, height)
    //   · 最多保留少量尺寸的条目（常见情况是用户开 N 个相同尺寸的 Editor
    //     时，所有实例都命中同一个共享 Image）
    //   · 不使用 JUCE ImageCache：那是一个进程级 static，插件 DLL 卸载时
    //     容易发生像素内存跨 DLL 悬垂；这里改为本 .cpp TU 内部 static，
    //     生命周期与 PinkXPStyle 所在的模块一致。
    // ------------------------------------------------------
    namespace
    {
        struct DesktopTextureEntry
        {
            ThemeId       theme;
            int           width;
            int           height;
            int           patternVersion;
            juce::Image   image;
        };

        // 底纹绘制算法/参数版本：改动 drawDesktop 的视觉参数时 +1，
        // 旧版本条目视为未命中，强制重新烘焙（避免升级后仍显示旧纹理）。
        static constexpr int kDesktopPatternVersion = 3;

        // 采用普通 vector + 简单 LRU 淘汰；N 不会很大（通常 1~3 种尺寸）
        static std::vector<DesktopTextureEntry>& getDesktopTextureCache()
        {
            static std::vector<DesktopTextureEntry> cache;
            return cache;
        }

        static constexpr size_t kMaxDesktopTextureEntries = 4;
    }

    juce::Image getSharedDesktopTexture(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return {};

        auto& cache = getDesktopTextureCache();
        const auto currentTheme = getCurrentThemeId();

        // 查找既存条目
        for (size_t i = 0; i < cache.size(); ++i)
        {
            auto& e = cache[i];
            if (e.theme == currentTheme
                && e.width == width
                && e.height == height
                && e.patternVersion == kDesktopPatternVersion
                && e.image.isValid())
            {
                // 命中：把条目移到末尾（简单 LRU）
                if (i + 1 != cache.size())
                {
                    DesktopTextureEntry hit = std::move(e);
                    cache.erase(cache.begin() + (std::ptrdiff_t) i);
                    cache.push_back(std::move(hit));
                }
                return cache.back().image;
            }
        }

        // 未命中：烘焙一张新的（左上角原点 = (0,0)，尺寸 = width × height）
        Y2K_LOG ("[palette] bake desktop texture: theme=" + juce::String ((int) currentTheme)
                 + " desktopGlobal=" + desktop.toString()
                 + " cacheEntries=" + juce::String ((int) cache.size()));
        juce::Image img(juce::Image::RGB, width, height, true);
        {
            juce::Graphics gg(img);
            drawDesktop(gg, juce::Rectangle<int>(0, 0, width, height));
        }

        // LRU 容量控制：超出时弹出最旧一条
        if (cache.size() >= kMaxDesktopTextureEntries)
            cache.erase(cache.begin());

        cache.push_back({ currentTheme, width, height, kDesktopPatternVersion, img });
        return cache.back().image;
    }

    void invalidateDesktopTextureCache()
    {
        getDesktopTextureCache().clear();
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
                           juce::Colour shadowColour,
                           float fontHeight)
    {
        if (iconArea.getWidth() <= 2 || iconArea.getHeight() <= 2)
            return;

        g.setFont(getFont(fontHeight, juce::Font::bold));
        g.setColour(shadowColour.withAlpha(0.55f));
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
        drawTitleIconText(g, iconRect, juce::String::fromUTF8(th.titleIconText), selInk, sel.darker(0.50f), fontHeight + 0.6f);

        // 标题文字（跟随主题 selInk）—— 静态文本位图缓存（v2.9.6 性能：
        //   组件每帧全量重绘时标题不再逐帧塑形/逐 glyph 上传，单次 quad 绘制）
        auto textRect = bounds.reduced(6, 0);
        textRect.removeFromLeft(19); // 给左侧图标留空间

        const juce::Image titleImg = cachedStaticTextImage (
            title, fontHeight, juce::Font::bold, selInk, sel.darker(0.50f).withAlpha(0.55f));
        if (titleImg.isValid())
        {
            g.drawImageAt (titleImg,
                           textRect.getX(),
                           textRect.getY() + (textRect.getHeight() - titleImg.getHeight()) / 2,
                           false);
        }
        else
        {
            // 兜底（缓存失败）：保持旧绘制路径
            g.setFont(getFont(fontHeight, juce::Font::bold));
            g.setColour(sel.darker(0.50f).withAlpha(0.55f));
            g.drawText(title, textRect.translated(1, 1), juce::Justification::centredLeft, false);
            g.setColour(selInk);
            g.drawText(title, textRect, juce::Justification::centredLeft, false);
        }

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
            // ---- 🌿 Jungle（荧光绿丛林）----
            //   · 荧光霓虹绿：暗黑丛林底 + 高饱和亮绿阶 → 经典荧光绿 #39FF14
            //   · 桌面 pixelStars 模拟暗夜雨林中的萤火虫/孢子光点
            //   · btnFace 暗绿底 + 亮绿 ink，确保深色主题下按钮文字清晰可读
            {
                ThemeId::jungle,
                "Jungle",
                "Neon / Rainforest / Fluorescent",
                juce::Colour(0xff081002), juce::Colour(0xff102006), // pink50/100 近黑→深暗绿
                juce::Colour(0xff1d3810), juce::Colour(0xff2a5218), // pink200/300 暗绿→暗翠绿
                juce::Colour(0xff39ff14), juce::Colour(0xff60ff40), // pink400/500 ★荧光绿→亮柠绿
                juce::Colour(0xff80ff6a), juce::Colour(0xffa0ff90), // pink600/700 亮霓虹→浅霓虹
                juce::Colour(0xff204a0e),                           // hl（暗绿高光）
                juce::Colour(0xff0a1a04),                           // face（深暗绿按钮面）
                juce::Colour(0xff040c01),                           // shdw
                juce::Colour(0xff010301),                           // dark（几乎纯黑）
                juce::Colour(0xffc0ffa0),                           // ink（亮浅绿文字，暗底高对比）
                juce::Colour(0xff39ff14),                           // sel（荧光绿标题栏）
                juce::Colour(0xff050d02),                           // selInk（暗绿字 on 荧光绿标题）
                juce::Colour(0xff040901),                           // desktop（暗黑丛林底）
                juce::Colour(0xff39ff14),                           // desktop2（荧光绿纹理点，萤火虫感）
                juce::Colour(0xff060e02),                           // content（暗黑绿画布）
                juce::Colour(0xff122808),                           // btnFace（暗绿按钮底，与亮 ink 对比强）
                juce::Colour(0xff39ff14),                           // swatch（荧光绿色票）
                DesktopPattern::pixelStars,
                "\xe2\x9c\xa6" // ✦（荧光星芒）
            },
            // ---- 🪟 Windows XP Luna（经典 Bliss 蓝天绿草）----
            //   pink50..pink700 在此主题下被"重新解读"为一组 Luna 蓝色阶
            //   （从极浅 sky → 经典 Luna 深蓝），所有使用 pink* 色的 UI
            //   控件（EQ 格子、Meter、曲线等）自动获得 Luna 蓝配色。
            //   桌面纹理用 bigDots 模拟 XP 经典 Bliss 天空蓝的波点。
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
                juce::Colour(0xff3a649c),                           // desktop (天空蓝)
                juce::Colour(0xff6e97c8),                           // desktop2 (淡天空蓝)
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
            // ---- 🖤💗 Black Pink（黑粉 Y2K）----
            //   · K-pop / 千禧年复古：纯黑主基调 + 亮粉强调色
            //   · desktop/content/face 全部使用暗黑色系，pink50→300 为暗粉→中粉过渡，
            //     pink400→700 为鲜艳粉→浅粉，供图表线条逐级高亮。
            //   · sel（标题栏）用签名亮粉 #ec4d85，搭配白色标题文字，黑底高对比。
            //   · ink 用浅粉白 #ffe6f0，在深色画布上清晰可读且保持粉色气质。
            //   · btnFace 用暗灰 #1e1e1e，保证按钮/控件在黑底上可见但不刺眼。
            //   · desktopPattern 用 checker 经典棋盘格，保持千禧年 XP 感。
            {
                ThemeId::blackPink,
                "Black Pink",
                "K-pop / Y2K / Dark + Neon pink",
                juce::Colour(0xff220a14), juce::Colour(0xff3a1428), // pink50/100  近黑深粉（提亮）
                juce::Colour(0xff5a203a), juce::Colour(0xff8a3058), // pink200/300 暗粉→玫瑰粉（提亮）
                juce::Colour(0xfff0508a), juce::Colour(0xffff78aa), // pink400/500 亮粉（Y2K 签名色，更亮）
                juce::Colour(0xffff96c0), juce::Colour(0xffffbddd), // pink600/700 浅粉→极浅粉（提亮）
                juce::Colour(0xff2e2e2e),                           // hl（暗灰高光，克制）
                juce::Colour(0xff1a1a1a),                           // face（深黑灰按钮面）
                juce::Colour(0xff0a0a0a),                           // shdw（近黑阴影）
                juce::Colour(0xff000000),                           // dark（纯黑边框）
                juce::Colour(0xffffe6f0),                           // ink（浅粉白文字，暗底高对比）
                juce::Colour(0xfff0508a),                           // sel（标题栏：亮粉，与 pink400 同步提亮）
                juce::Colour(0xff050d02),                           // selInk（深色文字 on 亮粉标题栏，复刻 Jungle 黑字风格）
                juce::Colour(0xff0e0e0e),                           // desktop（暗黑底，较之前调亮以显示纹理）
                juce::Colour(0xff4d1b2f),                           // desktop2（更深的爱心颜色，与底色形成微弱反差）
                juce::Colour(0xff121212),                           // content（深灰黑画布）
                juce::Colour(0xff1e1e1e),                           // btnFace（暗灰按钮面）
                juce::Colour(0xffec4d85),                           // swatch（色票：亮粉）
                DesktopPattern::hearts,
                "\xe2\x99\xa5" // ♥（黑桃心，千禧年符号）
            },
        };
        return themes;
    }

    const std::vector<Theme>& getAllThemes()
    {
        return buildThemes();
    }

    // 当前主题状态
    static ThemeId gCurrentThemeId = ThemeId::blackPink;

    // 主题变更回调表
    struct ThemeSub { int token; ThemeChangedCallback cb; };
    static std::vector<ThemeSub> gThemeSubs;
    static int gNextThemeSubToken = 1;

    // 自定义主题状态
    static juce::Colour gCustomPrimary { 0xffec4d85 };     // 默认粉色
    static juce::Colour gCustomSecondary { 0xffffffff };    // 默认白色
    // 调色板重构：生成器主题的原始 ThemeSpec + 动态显示名
    static y2kpal::ThemeSpec gCustomSpec;
    static juce::String gCustomThemeName;
    static Theme gCustomTheme;                              // 运行时动态构建的完整Theme

    juce::Colour getCustomPrimary()  { return gCustomPrimary; }
    juce::Colour getCustomSecondary() { return gCustomSecondary; }
    bool isCustomThemeActive() { return gCurrentThemeId == ThemeId::custom; }

    ThemeId getCurrentThemeId() { return gCurrentThemeId; }

    const Theme& getCurrentTheme()
    {
        if (gCurrentThemeId == ThemeId::custom)
            return gCustomTheme;
        const auto& all = getAllThemes();
        for (const auto& t : all)
            if (t.id == gCurrentThemeId)
                return t;
        return all.front();
    }

    // 把 Theme 的全部槽位写入全局调色板并通知订阅者
    //   （applyTheme / applyThemeFromSpec 共用，保证写入 + 通知流程一致）
    void commitTheme(const Theme& t)
    {
        pink50 = t.pink50;   pink100 = t.pink100; pink200 = t.pink200; pink300 = t.pink300;
        pink400 = t.pink400; pink500 = t.pink500; pink600 = t.pink600; pink700 = t.pink700;

        hl  = t.hl;   face = t.face; shdw = t.shdw; dark = t.dark;
        ink = t.ink;  sel  = t.sel;  selInk = t.selInk;
        desktop  = t.desktop;
        desktop2 = t.desktop2;
        content  = t.content;
        btnFace  = t.btnFace;

        // 调色板重构扩展槽位（S3）：预设主题未填时回退到 pink 阶梯/占位色
        accent2 = t.accent2.isTransparent() ? t.swatch : t.accent2;
        danger  = t.danger.isTransparent()  ? sel : t.danger;
        warn    = t.warn.isTransparent()    ? pink500 : t.warn;
        ok      = t.ok.isTransparent()      ? pink200 : t.ok;

        // 预设主题无 chartRamp 数据（默认透明）→ 回退 pink50..pink700 阶梯
        if (t.chartRamp[0].isTransparent())
        {
            chartRamp[0] = t.pink50;  chartRamp[1] = t.pink100;
            chartRamp[2] = t.pink200; chartRamp[3] = t.pink300;
            chartRamp[4] = t.pink400; chartRamp[5] = t.pink500;
            chartRamp[6] = t.pink600; chartRamp[7] = t.pink700;
        }
        else
        {
            for (int i = 0; i < 8; ++i)
                chartRamp[i] = t.chartRamp[(size_t) i];
        }

        // 刷新 LookAndFeel 内部 ColourScheme 缓存 —— 让 ComboBox 的
        //   arrowColourId/textColourId 等通过 findColour() 读取的控件颜色
        //   立刻跟随新主题（否则会保留构造时的旧配色）。
        //   注：ComboBox 内嵌 Label 的显式颜色残留由 PalettePanel 在
        //   应用主题后单独刷新（全局 sendLookAndFeelChange 会递归整棵
        //   组件树，拖动滑条高频触发时是明显的卡顿源，不能放在这里）。
        syncPinkXPColours (getPinkXPLookAndFeel());

        // P6: 主题切换时让"跨实例共享桌面纹理"全部失效，下一帧各 Editor
        //     paint 会拿到 key=(newThemeId,size) 的全新共享 Image。
        invalidateDesktopTextureCache();

        // 通知所有订阅者。每个回调单独 try/catch 隔离：任一订阅者抛异常
        //   都不能中断后续订阅者（Editor 的桌面缓存失效回调排在队列末尾，
        //   若被前面模块的异常截断，桌面纹理就会永远画旧缓存图）。
        for (auto& s : gThemeSubs)
        {
            if (!s.cb)
                continue;
            try
            {
                s.cb();
            }
            catch (const std::exception& e)
            {
                Y2K_LOG ("[palette] theme sub callback #" + juce::String (s.token)
                         + " threw: " + e.what());
            }
            catch (...)
            {
                Y2K_LOG ("[palette] theme sub callback #" + juce::String (s.token)
                         + " threw (unknown)");
            }
        }
    }

    void applyTheme(ThemeId id)
    {
        gCurrentThemeId = id;
        commitTheme(getCurrentTheme());
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

    // DesktopPattern ↔ y2kpal::Pattern（ThemeSpec 存储的是生成器枚举）
    DesktopPattern patternFromSpec(y2kpal::Pattern p)
    {
        switch (p)
        {
            case y2kpal::Pattern::PixelStars:  return DesktopPattern::pixelStars;
            case y2kpal::Pattern::ScanGrid:    return DesktopPattern::scanGrid;
            case y2kpal::Pattern::BigDots:     return DesktopPattern::bigDots;
            case y2kpal::Pattern::Bubbles:     return DesktopPattern::bubbles;
            case y2kpal::Pattern::DiagStripes: return DesktopPattern::diagStripes;
            case y2kpal::Pattern::Hearts:      return DesktopPattern::hearts;
            case y2kpal::Pattern::Zigzag:      return DesktopPattern::zigzag;
            case y2kpal::Pattern::CrossHatch:  return DesktopPattern::crossHatch;
            case y2kpal::Pattern::PixelFlower: return DesktopPattern::pixelFlower;
            case y2kpal::Pattern::Checker:
            default:                           return DesktopPattern::checker;
        }
    }

    y2kpal::Pattern patternFromSpecInv(DesktopPattern p)
    {
        switch (p)
        {
            case DesktopPattern::pixelStars:  return y2kpal::Pattern::PixelStars;
            case DesktopPattern::scanGrid:    return y2kpal::Pattern::ScanGrid;
            case DesktopPattern::bigDots:     return y2kpal::Pattern::BigDots;
            case DesktopPattern::bubbles:     return y2kpal::Pattern::Bubbles;
            case DesktopPattern::diagStripes: return y2kpal::Pattern::DiagStripes;
            case DesktopPattern::hearts:      return y2kpal::Pattern::Hearts;
            case DesktopPattern::zigzag:      return y2kpal::Pattern::Zigzag;
            case DesktopPattern::crossHatch:  return y2kpal::Pattern::CrossHatch;
            case DesktopPattern::pixelFlower: return y2kpal::Pattern::PixelFlower;
            case DesktopPattern::checker:
            default:                          return y2kpal::Pattern::Checker;
        }
    }

    void applyCustomTheme(juce::Colour primary, juce::Colour secondary)
    {
        gCustomPrimary   = primary;
        gCustomSecondary = secondary;

        // v2.10 调色板重构（R20 迁移）：双色 fit 成 ThemeSpec → OKLCH 生成器。
        //   accent/background 锁定为旧双色（视觉接近旧外观），其余角色由
        //   配方生成并受护栏保护（允许因质量提升略有差异）。
        y2kpal::ThemeSpec spec;
        y2kpal::fitSpecFromThemeColors(
            { primary.getRed(),   primary.getGreen(),   primary.getBlue()   },
            { secondary.getRed(), secondary.getGreen(), secondary.getBlue() },
            spec);
        applyThemeFromSpec(spec, "Custom (migrated)");
    }

    // ==================================================
    // 调色板重构（S2）：ThemeSpec → PaletteGenerator → Theme
    // ==================================================
    void applyThemeFromSpec(const y2kpal::ThemeSpec& spec, const juce::String& displayName)
    {
        gCustomSpec = spec;
        gCustomThemeName = displayName.isNotEmpty() ? displayName : juce::String("Custom");

        const y2kpal::GeneratedTheme gen = y2kpal::generate(spec);

        // 诊断日志（RelWithDebInfo）：桌面底色不随明暗切换的问题定位。
        //   bg/surface/control 的角色锁定会让 desktop/content/btnFace 槽位
        //   恒等于锁定色 —— mode 切换自然"看起来无效"。
        Y2K_LOG ("[palette] applyThemeFromSpec: bright=" + juce::String ((int) spec.brightness)
                 + " hue=" + juce::String (spec.hue, 1)
                 + " vibe=" + juce::String (spec.vibe, 2)
                 + " bgLock=" + (spec.overrides[(int) y2kpal::Role::Background].set ? "YES" : "no")
                 + " sfLock=" + (spec.overrides[(int) y2kpal::Role::Surface].set ? "YES" : "no")
                 + " ctlLock=" + (spec.overrides[(int) y2kpal::Role::Control].set ? "YES" : "no")
                 + " desktop=" + [&gen]() {
                       auto byte = [](uint8_t v) {
                           const juce::String s = juce::String::toHexString ((int) v);
                           return (s.length() < 2 ? "0" : "") + s;
                       };
                       return "#" + byte (gen.desktop.r) + byte (gen.desktop.g)
                            + byte (gen.desktop.b);
                   }());
        const auto toColour = [](y2kpal::Rgb8 c)
        {
            return juce::Colour(c.r, c.g, c.b);
        };

        // 标题图标候选（R14：5-8 个，与预设图标风格一致）
        static const char* const kTitleIcons[] = { "♡", "★", "◆", "●", "✦", "⌁", "♥", "▲" };
        const char* icon = kTitleIcons[spec.titleIcon % 8];

        // 逐字段赋值（MSVC 对含 std::array 成员的嵌套聚合初始化较挑剔）
        Theme t;
        t.id          = ThemeId::custom;
        t.displayName = gCustomThemeName;
        t.keywordHint = "Custom palette";
        t.pink50 = toColour(gen.pink[0]); t.pink100 = toColour(gen.pink[1]);
        t.pink200 = toColour(gen.pink[2]); t.pink300 = toColour(gen.pink[3]);
        t.pink400 = toColour(gen.pink[4]); t.pink500 = toColour(gen.pink[5]);
        t.pink600 = toColour(gen.pink[6]); t.pink700 = toColour(gen.pink[7]);
        t.hl = toColour(gen.hl);        t.face = toColour(gen.face);
        t.shdw = toColour(gen.shdw);    t.dark = toColour(gen.dark);
        t.ink = toColour(gen.ink);      t.sel = toColour(gen.sel);
        t.selInk = toColour(gen.selInk);
        t.desktop = toColour(gen.desktop); t.desktop2 = toColour(gen.desktop2);
        t.content = toColour(gen.content); t.btnFace = toColour(gen.btnFace);
        t.swatch = toColour(gen.accent);
        t.desktopPattern = patternFromSpec(spec.pattern);
        t.titleIconText = icon;
        t.accent2 = toColour(gen.accent2);
        t.danger = toColour(gen.danger);
        t.warn   = toColour(gen.warn);
        t.ok     = toColour(gen.ok);
        for (int i = 0; i < 8; ++i)
            t.chartRamp[(size_t) i] = toColour(gen.chartRamp[(size_t) i]);

        gCustomTheme = t;

        // 预览方块/色票条使用的双色快照
        gCustomPrimary   = gCustomTheme.sel;
        gCustomSecondary = gCustomTheme.desktop;

        gCurrentThemeId = ThemeId::custom;
        commitTheme(gCustomTheme);
    }

    y2kpal::ThemeSpec getCurrentThemeSpec()
    {
        if (gCurrentThemeId == ThemeId::custom)
            return gCustomSpec;

        // 预设主题：从 Accent(swatch)/Background(desktop) 反推意图参数
        //   （R10"基于预设新建"入口）。清除 fit 产生的覆盖锁定，
        //   保证色相环/氛围滑条等意图控件立即可用。
        const Theme& t = getCurrentTheme();
        y2kpal::ThemeSpec spec;
        y2kpal::fitSpecFromThemeColors(
            { t.swatch.getRed(),   t.swatch.getGreen(),   t.swatch.getBlue()   },
            { t.desktop.getRed(),  t.desktop.getGreen(),  t.desktop.getBlue()  },
            spec);
        for (auto& ov : spec.overrides)
            ov.set = false;
        spec.pattern = patternFromSpecInv(t.desktopPattern);
        // 氛围/染色从中间值起步（fit 反推会把它们算到接近 0，用户期望默认居中）
        spec.vibe   = 1.0f;
        spec.bgTint = 1.0f;
        return spec;
    }

    void applyUserTheme(const y2kpal::ThemeSpec& spec, const juce::String& name)
    {
        applyThemeFromSpec(spec, name);
    }

    juce::String getCustomThemeName()
    {
        return gCustomThemeName;
    }

    // ==================================================
    // 图表渐变采样（A3/S4）：OKLab 空间插值替代 sRGB 线性插值
    // ==================================================
    juce::Colour sampleChartRamp(float t)
    {
        t = juce::jlimit(0.0f, 1.0f, t);
        const float pos = t * 7.0f;
        const int   i = juce::jmin(6, (int) pos);
        const float u = pos - (float) i;
        return interpolateOkLab(chartRamp[i], chartRamp[i + 1], u);
    }

    juce::Colour interpolateOkLab(juce::Colour a, juce::Colour b, float t)
    {
        t = juce::jlimit(0.0f, 1.0f, t);
        const y2kpal::Rgb8 c = y2kpal::interpolateOkLab(
            { a.getRed(), a.getGreen(), a.getBlue() },
            { b.getRed(), b.getGreen(), b.getBlue() },
            t);
        return juce::Colour(c.r, c.g, c.b);
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
    // 组件显式覆盖的文字色优先（RoleRow 的 Auto 开关等需要自适应对比）；
    // 未覆盖时回退 PinkXP::ink（与历史行为一致）。TextButton 的文字色 id
    // 是 textColourOffId / textColourOnId（按下/选中态用 On）。
    const bool toggled = shouldDrawButtonAsDown || button.getToggleState();
    const juce::Colour tc = button.findColour(toggled ? juce::TextButton::textColourOnId
                                                      : juce::TextButton::textColourOffId);
    g.setColour(tc.isOpaque() ? tc : PinkXP::ink);
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
                                         float sliderPos, float minSliderPos, float maxSliderPos,
                                         const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::ignoreUnused(slider);

    const bool isVertical = (style == juce::Slider::LinearVertical
                             || style == juce::Slider::TwoValueVertical);

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

    const int thumbW = isVertical ? juce::jmin(w - 4, 18) : 12;
    const int thumbH = isVertical ? 12 : juce::jmin(h - 4, 18);

    const bool isTwoValue = (style == juce::Slider::TwoValueHorizontal
                             || style == juce::Slider::TwoValueVertical);

    if (isTwoValue)
    {
        // 双滑块：填充 min↔max 之间的选中区，并绘制两个 thumb。
        //   JUCE 对 TwoValue 只调用一次本函数，两个 thumb 位置分别由
        //   minSliderPos / maxSliderPos 传入（sliderPos 仅是当前被拖动的 thumb）。
        if (isVertical)
        {
            // 垂直方向：value 越大 thumb 越靠上，故 max 在上、min 在下
            const int top    = juce::jlimit(track.getY() + 2, track.getBottom() - 2, (int) maxSliderPos);
            const int bottom = juce::jlimit(track.getY() + 2, track.getBottom() - 2, (int) minSliderPos);
            g.setColour(PinkXP::sel);
            g.fillRect(track.getX() + 2, top, track.getWidth() - 4, juce::jmax(0, bottom - top));
        }
        else
        {
            const int left  = juce::jlimit(track.getX() + 2, track.getRight() - 2, (int) minSliderPos);
            const int right = juce::jlimit(track.getX() + 2, track.getRight() - 2, (int) maxSliderPos);
            g.setColour(PinkXP::sel);
            g.fillRect(left, track.getY() + 2, juce::jmax(0, right - left), track.getHeight() - 4);
        }

        auto drawThumb = [&](float pos)
        {
            juce::Rectangle<int> thumb;
            if (isVertical)
                thumb = juce::Rectangle<int>(x + (w - thumbW) / 2, (int) pos - thumbH / 2, thumbW, thumbH);
            else
                thumb = juce::Rectangle<int>((int) pos - thumbW / 2, y + (h - thumbH) / 2, thumbW, thumbH);
            PinkXP::drawRaised(g, thumb, PinkXP::btnFace);
        };
        drawThumb(minSliderPos);
        drawThumb(maxSliderPos);
        return;
    }

    // ---- 单值滑块（保持原逻辑）----
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

    juce::Rectangle<int> thumb;
    if (isVertical)
        thumb = juce::Rectangle<int>(x + (w - thumbW) / 2, (int) sliderPos - thumbH / 2, thumbW, thumbH);
    else
        thumb = juce::Rectangle<int>((int) sliderPos - thumbW / 2, y + (h - thumbH) / 2, thumbW, thumbH);

    PinkXP::drawRaised(g, thumb, PinkXP::btnFace);
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

// 全局字体兜底：任何通过 LookAndFeel 请求 Typeface 的控件都优先使用 PinkXP
// 当前激活的自定义字体（Silkscreen）。这能覆盖未显式 setFont 的组件路径，
// 避免"局部生效、整体回退系统字体"的混用风格。Windows / macOS 均受益。
juce::Typeface::Ptr PinkXPLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    juce::ignoreUnused (f);
    // 使用已缓存的 Typeface 而非每次调用 loadActiveTypeface()。
    // loadActiveTypeface() 内部会创建 SharedResourcePointer<Direct2DFactories>，
    // 每次调用开销大且在 DWrite 未就绪时可能崩溃。
    if (PinkXP::gTypeface != nullptr)
        return PinkXP::gTypeface;
    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}

// Popup 菜单条目理想尺寸：
//   drawPopupMenuItem 中 textArea 的左右内边距为：
//     - area.reduced(8, 0) 左右各 8px
//     - textArea.removeFromLeft(14) 再吃掉 14px（tick / icon 占位）
//     - 右侧若 hasSubMenu 还会 removeFromRight(12)
//   所以这里预留 8 + 14 + 8(右侧余量) = 30px 给 padding，并用
//   getPopupMenuFont() 的实际宽度测量文字像素宽，再 +2 作为小量余量。
void PinkXPLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                                  int standardMenuItemHeight,
                                                  int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth  = 50;
        idealHeight = (standardMenuItemHeight > 0 ? standardMenuItemHeight : 10) / 10 + 4;
        return;
    }

    const auto font     = getPopupMenuFont();
    const int  textPx   = juce::GlyphArrangement::getStringWidthInt (font, text);
    constexpr int hPad  = 8 + 14 + 8;   // 见上方注释：与 drawPopupMenuItem 保持一致

    idealWidth  = textPx + hPad + 2;
    idealHeight = (standardMenuItemHeight > 0)
                      ? standardMenuItemHeight
                      : juce::roundToInt (font.getHeight() * 1.3f);
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

juce::PopupMenu::Options PinkXPLookAndFeel::getOptionsForComboBoxPopupMenu (juce::ComboBox& box,
                                                                            juce::Label& label)
{
    auto options = juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu (box, label);

    if (! PinkXP::isStickDockActive())
        return options;

    // stick 驻留态：JUCE 默认把菜单 clamp 到 display->userArea，而 AppBar 驻留
    //   正好把条带从工作区里挖掉了 —— 下拉框位于条带内（userArea 之外）时，
    //   菜单会被整体推到条带上方，离控件很远。这里把允许区域放宽到整个显示器，
    //   让菜单按默认逻辑贴着下拉框展开（可以盖在条带上）。
    const auto boxScreen = box.getScreenBounds();
    const auto display = juce::Desktop::getInstance().getDisplays()
                             .getDisplayForPoint (boxScreen.getCentre());
    if (display == nullptr)
        return options;

    // 工作区内的下拉（例如浮动窗口）不受影响，保持默认锚点与约束。
    if (display->userArea.contains (boxScreen.getCentre()))
        return options;

   #if JUCE_WINDOWS
    // withParentAreaOverride 是本仓库 JUCE 的本地扩展（见 juce_PopupMenu.h）。
    //   这里用平台宏包住，保证 macOS 端即使链接官方 JUCE 也能编译。
    return options.withParentAreaOverride (
        display->safeAreaInsets.subtractedFrom (display->totalArea));
   #else
    return options;
   #endif
}

PinkXPLookAndFeel& getPinkXPLookAndFeel()
{
    static PinkXPLookAndFeel lnf;
    return lnf;
}
