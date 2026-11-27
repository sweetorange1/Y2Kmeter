#include "source/ui/SkinCraft.h"
#include "source/i18n/Localisation.h"

#include <algorithm>
#include <cmath>

// ==========================================================
// SkinCraft.cpp —— 视觉风格令牌表 + CPU 工艺原语实现
// ==========================================================

namespace PinkXP
{
    namespace
    {
        constexpr SkinTokens makePinkXPTokens() noexcept
        {
            SkinTokens t {};
            t.frameStyle            = FrameStyle::pixelBevel;
            t.isChromaKey           = false;
            t.pixelSnapped          = true;
            t.preferLinearGl        = false;
            t.useFlatControls       = false;
            t.useTextShadow         = true;

            t.controlRadius         = 0.0f;
            t.cardRadius            = 0.0f;
            t.badgeRadius           = 0.0f;
            t.barCellRadius         = 0.0f;
            t.tabRadius             = 0.0f;
            t.sliderTrackRadius     = 0.0f;
            t.sliderThumbRadius     = 0.0f;

            t.borderStrength        = 1.0f;
            t.surfaceSheenAlpha     = 0.0f;
            t.innerShadowAlpha      = 0.0f;
            t.scanlineAlpha         = 0.0f;
            t.scanlinePitch         = 0;
            t.vignetteAlpha         = 0.0f;
            t.glowIntensity         = 0.0f;

            t.cellFinish            = CellFinish::flatPixel;
            t.defaultCellGapPx      = 1;
            t.unlitCellAlpha        = 0.15f;
            t.cellHighlightAlpha    = 0.36f;
            t.peakCapGlow           = false;
            t.hardPixelSteps        = true;

            t.strokeWidthScale      = 1.0f;
            t.strokeGlowPass        = false;
            t.strokeGlowWidthMul    = 1.0f;
            t.strokeGlowAlpha       = 0.0f;
            t.areaFillAlphaScale    = 1.0f;
            t.antiAliasStrokes      = true;

            t.gridStyle             = GridStyle::dottedPixel;
            t.gridMajorAlpha        = 0.28f;
            t.gridMinorAlpha        = 0.14f;

            t.dialFinish            = DialFinish::pixelHardware;
            t.dialGlossAlpha        = 0.0f;
            t.needleShadow          = true;
            t.needleHardPixelShadow = true;
            t.needleGlow            = false;
            t.statusDotCircular     = false;
            t.statusDotGlow         = false;
            t.pixelCornerAccents    = true;
            return t;
        }

        constexpr SkinTokens makeChromeTokens() noexcept
        {
            SkinTokens t {};
            t.frameStyle            = FrameStyle::machinedChrome;
            t.isChromaKey           = false;
            t.pixelSnapped          = false;
            t.preferLinearGl        = true;
            t.useFlatControls       = false;
            t.useTextShadow         = true;

            t.controlRadius         = 0.0f;
            t.cardRadius            = 0.0f;
            t.badgeRadius           = 0.0f;
            t.barCellRadius         = 0.0f;
            t.tabRadius             = 0.0f;
            t.sliderTrackRadius     = 0.0f;
            t.sliderThumbRadius     = 0.0f;

            t.borderStrength        = 0.85f;
            t.surfaceSheenAlpha     = 0.0f;
            t.innerShadowAlpha      = 0.24f;
            t.scanlineAlpha         = 0.0f;
            t.scanlinePitch         = 0;
            t.vignetteAlpha         = 0.0f;
            t.glowIntensity         = 0.0f;

            t.cellFinish            = CellFinish::metallicBevel;
            t.defaultCellGapPx      = 1;
            t.unlitCellAlpha        = 0.16f;
            t.cellHighlightAlpha    = 0.46f;
            t.peakCapGlow           = false;
            t.hardPixelSteps        = false;

            t.strokeWidthScale      = 1.05f;
            t.strokeGlowPass        = false;
            t.strokeGlowWidthMul    = 1.0f;
            t.strokeGlowAlpha       = 0.0f;
            t.areaFillAlphaScale    = 1.0f;
            t.antiAliasStrokes      = true;

            t.gridStyle             = GridStyle::solidHairline;
            t.gridMajorAlpha        = 0.20f;
            t.gridMinorAlpha        = 0.10f;

            t.dialFinish            = DialFinish::brushedMetal;
            t.dialGlossAlpha        = 0.14f;
            t.needleShadow          = true;
            t.needleHardPixelShadow = false;
            t.needleGlow            = false;
            t.statusDotCircular     = true;
            t.statusDotGlow         = true;
            t.pixelCornerAccents    = false;
            return t;
        }

        constexpr SkinTokens makeModernTokens() noexcept
        {
            SkinTokens t {};
            t.frameStyle            = FrameStyle::modernStudio;
            t.isChromaKey           = false;
            t.pixelSnapped          = false;
            t.preferLinearGl        = true;
            t.useFlatControls       = true;
            t.useTextShadow         = false;

            t.controlRadius         = 4.5f;
            t.cardRadius            = 5.0f;
            t.badgeRadius           = 4.0f;
            t.barCellRadius         = 1.75f;
            t.tabRadius             = 4.5f;
            t.sliderTrackRadius     = 2.0f;
            t.sliderThumbRadius     = 6.0f;

            t.borderStrength        = 0.16f;
            t.surfaceSheenAlpha     = 0.0f;
            t.innerShadowAlpha      = 0.08f;
            t.scanlineAlpha         = 0.0f;
            t.scanlinePitch         = 0;
            t.vignetteAlpha         = 0.0f;
            t.glowIntensity         = 0.0f;

            t.cellFinish            = CellFinish::cleanRounded;
            t.defaultCellGapPx      = 2;
            t.unlitCellAlpha        = 0.11f;
            t.cellHighlightAlpha    = 0.08f;
            t.peakCapGlow           = false;
            t.hardPixelSteps        = false;

            t.strokeWidthScale      = 1.0f;
            t.strokeGlowPass        = false;
            t.strokeGlowWidthMul    = 1.0f;
            t.strokeGlowAlpha       = 0.0f;
            t.areaFillAlphaScale    = 0.96f;
            t.antiAliasStrokes      = true;

            t.gridStyle             = GridStyle::solidHairline;
            t.gridMajorAlpha        = 0.16f;
            t.gridMinorAlpha        = 0.08f;

            t.dialFinish            = DialFinish::studioSmooth;
            t.dialGlossAlpha        = 0.0f;
            t.needleShadow          = true;
            t.needleHardPixelShadow = false;
            t.needleGlow            = false;
            t.statusDotCircular     = true;
            t.statusDotGlow         = false;
            t.pixelCornerAccents    = false;
            return t;
        }

        constexpr SkinTokens makeGlassTokens() noexcept
        {
            SkinTokens t {};
            t.frameStyle            = FrameStyle::frostedGlass;
            t.isChromaKey           = false;
            t.pixelSnapped          = false;
            t.preferLinearGl        = true;
            t.useFlatControls       = true;
            t.useTextShadow         = true;

            t.controlRadius         = 6.0f;
            t.cardRadius            = 6.0f;
            t.badgeRadius           = 6.0f;
            t.barCellRadius         = 3.0f;
            t.tabRadius             = 5.5f;
            t.sliderTrackRadius     = 4.0f;
            t.sliderThumbRadius     = 6.5f;

            t.borderStrength        = 0.34f;
            t.surfaceSheenAlpha     = 0.12f;
            t.innerShadowAlpha      = 0.16f;
            t.scanlineAlpha         = 0.0f;
            t.scanlinePitch         = 0;
            t.vignetteAlpha         = 0.0f;
            t.glowIntensity         = 0.20f;

            t.cellFinish            = CellFinish::glossyCapsule;
            t.defaultCellGapPx      = 2;
            t.unlitCellAlpha        = 0.14f;
            t.cellHighlightAlpha    = 0.40f;
            t.peakCapGlow           = true;
            t.hardPixelSteps        = false;

            t.strokeWidthScale      = 1.10f;
            t.strokeGlowPass        = false;
            t.strokeGlowWidthMul    = 1.0f;
            t.strokeGlowAlpha       = 0.0f;
            t.areaFillAlphaScale    = 1.08f;
            t.antiAliasStrokes      = true;

            t.gridStyle             = GridStyle::solidHairline;
            t.gridMajorAlpha        = 0.20f;
            t.gridMinorAlpha        = 0.10f;

            t.dialFinish            = DialFinish::glassDome;
            t.dialGlossAlpha        = 0.26f;
            t.needleShadow          = true;
            t.needleHardPixelShadow = false;
            t.needleGlow            = false;
            t.statusDotCircular     = true;
            t.statusDotGlow         = true;
            t.pixelCornerAccents    = false;
            return t;
        }

        constexpr SkinTokens makeNeonTokens() noexcept
        {
            SkinTokens t {};
            t.frameStyle            = FrameStyle::neonTube;
            t.isChromaKey           = false;
            t.pixelSnapped          = false;
            t.preferLinearGl        = true;
            t.useFlatControls       = true;
            t.useTextShadow         = false;

            t.controlRadius         = 2.0f;
            t.cardRadius            = 2.0f;
            t.badgeRadius           = 2.0f;
            t.barCellRadius         = 1.5f;
            t.tabRadius             = 2.0f;
            t.sliderTrackRadius     = 1.5f;
            t.sliderThumbRadius     = 2.0f;

            t.borderStrength        = 0.68f;
            t.surfaceSheenAlpha     = 0.0f;
            t.innerShadowAlpha      = 0.18f;
            t.scanlineAlpha         = 0.0f;
            t.scanlinePitch         = 0;
            t.vignetteAlpha         = 0.0f;
            t.glowIntensity         = 0.65f;

            t.cellFinish            = CellFinish::neonLit;
            t.defaultCellGapPx      = 2;
            t.unlitCellAlpha        = 0.14f;
            t.cellHighlightAlpha    = 0.52f;
            t.peakCapGlow           = true;
            t.hardPixelSteps        = false;

            t.strokeWidthScale      = 1.05f;
            t.strokeGlowPass        = true;
            t.strokeGlowWidthMul    = 2.8f;
            t.strokeGlowAlpha       = 0.30f;
            t.areaFillAlphaScale    = 0.85f;
            t.antiAliasStrokes      = true;

            t.gridStyle             = GridStyle::dashedTech;
            t.gridMajorAlpha        = 0.22f;
            t.gridMinorAlpha        = 0.11f;

            t.dialFinish            = DialFinish::neonRings;
            t.dialGlossAlpha        = 0.12f;
            t.needleShadow          = false;
            t.needleHardPixelShadow = false;
            t.needleGlow            = true;
            t.statusDotCircular     = true;
            t.statusDotGlow         = true;
            t.pixelCornerAccents    = false;
            return t;
        }

        constexpr SkinTokens makeCrtTokens() noexcept
        {
            SkinTokens t {};
            t.frameStyle            = FrameStyle::crtPhosphor;
            t.isChromaKey           = false;
            t.pixelSnapped          = true;
            t.preferLinearGl        = false;
            t.useFlatControls       = false;
            t.useTextShadow         = true;

            t.controlRadius         = 0.0f;
            t.cardRadius            = 0.0f;
            t.badgeRadius           = 0.0f;
            t.barCellRadius         = 0.0f;
            t.tabRadius             = 0.0f;
            t.sliderTrackRadius     = 0.0f;
            t.sliderThumbRadius     = 0.0f;

            t.borderStrength        = 0.62f;
            t.surfaceSheenAlpha     = 0.0f;
            t.innerShadowAlpha      = 0.14f;
            t.scanlineAlpha         = 0.085f;
            t.scanlinePitch         = 2;
            t.vignetteAlpha         = 0.24f;
            t.glowIntensity         = 0.44f;

            t.cellFinish            = CellFinish::phosphorBar;
            t.defaultCellGapPx      = 1;
            t.unlitCellAlpha        = 0.13f;
            t.cellHighlightAlpha    = 0.32f;
            t.peakCapGlow           = true;
            t.hardPixelSteps        = true;

            t.strokeWidthScale      = 1.0f;
            t.strokeGlowPass        = true;
            t.strokeGlowWidthMul    = 2.4f;
            t.strokeGlowAlpha       = 0.24f;
            t.areaFillAlphaScale    = 0.78f;
            t.antiAliasStrokes      = true;

            t.gridStyle             = GridStyle::phosphorCrosshair;
            t.gridMajorAlpha        = 0.24f;
            t.gridMinorAlpha        = 0.10f;

            t.dialFinish            = DialFinish::crtVector;
            t.dialGlossAlpha        = 0.16f;
            t.needleShadow          = false;
            t.needleHardPixelShadow = false;
            t.needleGlow            = true;
            t.statusDotCircular     = false;
            t.statusDotGlow         = true;
            t.pixelCornerAccents    = false;
            return t;
        }

        constexpr SkinTokens makeKeyedVariant (SkinTokens t) noexcept
        {
            // 绿幕 / 透明背景模式：100% 继承所属视觉风格的材质、圆角、LED 形态与表盘工艺，
            //   仅置 isChromaKey = true 并清零画布后处理（scanline / vignette / sheen），
            //   保证不污染纯绿或透明背景像素。
            t.isChromaKey       = true;
            t.scanlineAlpha     = 0.0f;
            t.scanlinePitch     = 0;
            t.vignetteAlpha     = 0.0f;
            t.surfaceSheenAlpha = 0.0f;
            return t;
        }

        static constexpr SkinTokens kNormalTable[6] = {
            makePinkXPTokens(),
            makeChromeTokens(),
            makeModernTokens(),
            makeGlassTokens(),
            makeNeonTokens(),
            makeCrtTokens()
        };

        static constexpr SkinTokens kKeyedTable[6] = {
            makeKeyedVariant (kNormalTable[0]),
            makeKeyedVariant (kNormalTable[1]),
            makeKeyedVariant (kNormalTable[2]),
            makeKeyedVariant (kNormalTable[3]),
            makeKeyedVariant (kNormalTable[4]),
            makeKeyedVariant (kNormalTable[5])
        };

        // 编译期自检：确保 hasCanvasFx() 与 keyed 继承表严格符合产品规格
        static_assert (! kNormalTable[0].hasCanvasFx(), "pinkXP must not trigger canvas post-FX");
        static_assert (! kNormalTable[1].hasCanvasFx(), "chrome must not trigger canvas post-FX");
        static_assert (! kNormalTable[2].hasCanvasFx(), "modern must not trigger canvas post-FX");
        static_assert (  kNormalTable[3].hasCanvasFx(), "glass requires dome sheen post-FX");
        static_assert (  kNormalTable[4].hasCanvasFx(), "neon requires inner rim post-FX");
        static_assert (  kNormalTable[5].hasCanvasFx(), "crt requires scanline+vignette post-FX");
        static_assert (kKeyedTable[0].isChromaKey && ! kKeyedTable[0].hasCanvasFx() && kKeyedTable[0].frameStyle == FrameStyle::pixelBevel, "keyed pinkXP inherits pixelBevel");
        static_assert (kKeyedTable[1].isChromaKey && ! kKeyedTable[1].hasCanvasFx() && kKeyedTable[1].frameStyle == FrameStyle::machinedChrome, "keyed chrome inherits machinedChrome");
        static_assert (kKeyedTable[2].isChromaKey && ! kKeyedTable[2].hasCanvasFx() && kKeyedTable[2].frameStyle == FrameStyle::modernStudio, "keyed modern inherits modernStudio");
        static_assert (kKeyedTable[3].isChromaKey && ! kKeyedTable[3].hasCanvasFx() && kKeyedTable[3].frameStyle == FrameStyle::frostedGlass, "keyed glass inherits frostedGlass");
        static_assert (kKeyedTable[4].isChromaKey && ! kKeyedTable[4].hasCanvasFx() && kKeyedTable[4].frameStyle == FrameStyle::neonTube, "keyed neon inherits neonTube");
        static_assert (kKeyedTable[5].isChromaKey && ! kKeyedTable[5].hasCanvasFx() && kKeyedTable[5].frameStyle == FrameStyle::crtPhosphor, "keyed crt inherits crtPhosphor");

        constexpr int skinToTableIndex (SkinId id) noexcept
        {
            switch (id)
            {
                case SkinId::chrome: return 1;
                case SkinId::modern:
                case SkinId::fluent: return 2;
                case SkinId::glass:  return 3;
                case SkinId::neon:   return 4;
                case SkinId::crt:    return 5;
                case SkinId::pinkXP:
                default:             return 0;
            }
        }

        inline juce::Colour neonTubeCore (bool hot) noexcept
        {
            return sel.interpolatedWith (juce::Colours::white, hot ? 0.55f : 0.30f);
        }

        inline bool allowSmallRounding (juce::Rectangle<int> r) noexcept
        {
            return juce::jmin (r.getWidth(), r.getHeight()) <= 48
                && juce::jmax (r.getWidth(), r.getHeight()) <= 320
                && r.getWidth() >= 6 && r.getHeight() >= 6;
        }

        inline void fillRectFrame1Px (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour c)
        {
            if (r.getWidth() < 2 || r.getHeight() < 2)
                return;
            g.setColour (c);
            g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
            g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);
            g.fillRect (r.getX(), r.getY() + 1, 1, r.getHeight() - 2);
            g.fillRect (r.getRight() - 1, r.getY() + 1, 1, r.getHeight() - 2);
        }

        inline void drawModernSurface (juce::Graphics& g,
                                       juce::Rectangle<int> r,
                                       juce::Colour fill,
                                       float radius,
                                       bool elevatedLip,
                                       bool pressedLook,
                                       bool activeLook,
                                       bool hoverLook,
                                       bool sunkenLook)
        {
            if (r.getWidth() < 2 || r.getHeight() < 2)
                return;

            const auto  rf    = r.toFloat();
            const float rad   = (radius > 0.5f)
                                  ? juce::jmin (radius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.5f)
                                  : 0.0f;
            const bool  round = (rad > 0.5f);

            const auto refBase = fill.isTransparent() ? face : fill;
            const bool isDark  = refBase.getPerceivedBrightness() < 0.45f;

            juce::Colour base = fill;
            if (! base.isTransparent())
            {
                const float fa = fill.getFloatAlpha();
                if (activeLook)
                    base = base.interpolatedWith (sel, isDark ? 0.22f : 0.14f).withAlpha (fa);
                else if (pressedLook)
                    base = base.interpolatedWith (sel, isDark ? 0.28f : 0.20f).withAlpha (fa);
                else if (hoverLook)
                    base = base.interpolatedWith (isDark ? juce::Colours::white : juce::Colours::black,
                                                  isDark ? 0.07f : 0.045f).withAlpha (fa);

                if (! sunkenLook && ! pressedLook)
                {
                    // 极轻的演播室顶面受光梯度（暗色主题克制提亮，亮色主题柔和清透）
                    const float topLift = isDark ? (hoverLook ? 0.055f : 0.035f)
                                                 : (hoverLook ? 0.065f : 0.040f);
                    juce::ColourGradient grad (base.interpolatedWith (juce::Colours::white, topLift).withAlpha (fa),
                                               0.0f, rf.getY(),
                                               base,
                                               0.0f, rf.getBottom(), false);
                    g.setGradientFill (grad);
                }
                else
                {
                    g.setColour (base);
                }

                if (round) g.fillRoundedRectangle (rf, rad);
                else       g.fillRect (r);
            }

            const auto ref = fill.isTransparent() ? face : base;
            const auto border = (activeLook || pressedLook)
                                  ? sel.withAlpha (activeLook ? 0.88f : 1.0f)
                                  : (isDark
                                       ? ref.withAlpha (1.0f).interpolatedWith (juce::Colours::white,
                                                                                 hoverLook ? 0.20f : (sunkenLook ? 0.10f : 0.13f))
                                       : ref.withAlpha (1.0f).interpolatedWith (juce::Colours::black,
                                                                                 hoverLook ? 0.20f : (sunkenLook ? 0.13f : 0.14f)));

            g.setColour (border);
            if (round) g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            else       fillRectFrame1Px (g, r, border);

            const int inset = round ? juce::jmax (2, (int) std::ceil (rad * 0.72f)) : 1;
            const int spanW = r.getWidth() - inset * 2;

            if (spanW > 2 && r.getHeight() >= 6)
            {
                if (sunkenLook)
                {
                    // 内凹层次：顶部极轻内阴影 + 底部微弱反光沿
                    g.setColour (juce::Colours::black.withAlpha (isDark ? 0.26f : 0.08f));
                    g.fillRect (r.getX() + inset, r.getY() + 1, spanW, 1);
                    g.setColour (juce::Colours::white.withAlpha (isDark ? 0.07f : 0.42f));
                    g.fillRect (r.getX() + inset, r.getBottom() - 1, spanW, 1);
                }
                else if (elevatedLip && ! activeLook && ! pressedLook)
                {
                    // 抬升层次：顶沿微弱受光高光线 + 底沿克制暗影抬升边（暗/亮主题均朝下压暗）
                    g.setColour (juce::Colours::white.withAlpha (isDark ? 0.09f : 0.50f));
                    g.fillRect (r.getX() + inset, r.getY() + 1, spanW, 1);
                    g.setColour (isDark ? ref.withAlpha (1.0f).interpolatedWith (juce::Colours::black, 0.45f)
                                        : ref.withAlpha (1.0f).interpolatedWith (juce::Colours::black, 0.22f));
                    g.fillRect (r.getX() + inset, r.getBottom() - 1, spanW, 1);
                }
            }

            // 激活态底部 2px 锐利圆角强调短条
            if (activeLook && r.getWidth() >= 14 && r.getHeight() >= 10)
            {
                const int bw = juce::jlimit (10, r.getWidth() - 6, r.getWidth() * 2 / 5);
                g.setColour (sel);
                g.fillRoundedRectangle ((float) (r.getCentreX() - bw / 2),
                                        (float) r.getBottom() - 3.0f,
                                        (float) bw, 2.0f, 1.0f);
            }
        }

        inline void drawChromeSurface (juce::Graphics& g,
                                       juce::Rectangle<int> r,
                                       juce::Colour fill,
                                       bool pressedLook,
                                       bool hoverLook,
                                       bool sunkenLook)
        {
            if (r.getWidth() < 2 || r.getHeight() < 2)
                return;

            const float y0    = (float) r.getY();
            const float y1    = (float) r.getBottom();
            const auto  white = juce::Colours::white;
            const auto  black = juce::Colours::black;
            const auto  ref0  = fill.isTransparent() ? face : fill;
            const bool  isDark = ref0.getPerceivedBrightness() < 0.45f;

            if (! fill.isTransparent())
            {
                const float fa = fill.getFloatAlpha();
                const auto  f  = hoverLook
                                   ? fill.interpolatedWith (white, isDark ? 0.09f : 0.12f).withAlpha (fa)
                                   : fill;
                if (sunkenLook)
                {
                    // CNC 铣削深槽：略压暗槽底 + 顶部 3px 倒角投射内阴影 + 底沿 1px 金属反光
                    const auto wellBg = f.interpolatedWith (black, isDark ? 0.12f : 0.04f).withAlpha (fa);
                    g.setColour (wellBg);
                    g.fillRect (r);
                    const auto in = r.reduced (2);
                    if (! in.isEmpty())
                    {
                        const float alphas[3] = { isDark ? 0.36f : 0.24f,
                                                  isDark ? 0.20f : 0.13f,
                                                  isDark ? 0.08f : 0.05f };
                        for (int i = 0; i < 3 && i < in.getHeight(); ++i)
                        {
                            g.setColour (black.withAlpha (alphas[i] * fa));
                            g.fillRect (in.getX(), in.getY() + i, in.getWidth(), 1);
                        }
                        if (in.getHeight() >= 4)
                        {
                            g.setColour (white.withAlpha ((isDark ? 0.12f : 0.42f) * fa));
                            g.fillRect (in.getX(), in.getBottom() - 1, in.getWidth(), 1);
                        }
                    }
                }
                else if (pressedLook)
                {
                    // 按下态：内凹铣削金属渐变（上暗下微回光）
                    juce::ColourGradient grad (f.interpolatedWith (black, isDark ? 0.22f : 0.16f).withAlpha (fa), 0.0f, y0,
                                               f.interpolatedWith (white, isDark ? 0.08f : 0.14f).withAlpha (fa), 0.0f, y1, false);
                    grad.addColour (0.50, f.interpolatedWith (black, isDark ? 0.08f : 0.05f).withAlpha (fa));
                    g.setGradientFill (grad);
                    g.fillRect (r);
                }
                else if (r.getHeight() <= 56)
                {
                    // 4-Stop 拉丝金属折痕渐变（38% 处锐利镜面折痕，暗色=阳极氧化枪灰、亮色=拉丝银铝）
                    juce::ColourGradient grad (f.interpolatedWith (white, isDark ? 0.14f : 0.44f).withAlpha (fa), 0.0f, y0,
                                               f.interpolatedWith (white, isDark ? 0.06f : 0.14f).withAlpha (fa), 0.0f, y1, false);
                    grad.addColour (0.38, f.interpolatedWith (white, isDark ? 0.22f : 0.26f).withAlpha (fa));
                    grad.addColour (0.42, f.interpolatedWith (black, isDark ? 0.10f : 0.08f).withAlpha (fa));
                    g.setGradientFill (grad);
                    g.fillRect (r);
                }
                else
                {
                    // 大面板 4-Stop 柔和拉丝金属光泽（无中段硬折痕，保持 WCAG 可读性）
                    juce::ColourGradient grad (f.interpolatedWith (white, isDark ? 0.10f : 0.24f).withAlpha (fa), 0.0f, y0,
                                               f.interpolatedWith (black, isDark ? 0.12f : 0.08f).withAlpha (fa), 0.0f, y1, false);
                    grad.addColour (0.36, f.interpolatedWith (white, isDark ? 0.04f : 0.10f).withAlpha (fa));
                    grad.addColour (0.68, f.interpolatedWith (black, isDark ? 0.04f : 0.03f).withAlpha (fa));
                    g.setGradientFill (grad);
                    g.fillRect (r);
                }
            }

            // 1px CNC 锐利深色外切边
            const auto outerEdge = isDark ? ref0.withAlpha (1.0f).interpolatedWith (black, 0.72f)
                                          : dark;
            fillRectFrame1Px (g, r, outerEdge);

            // 1px 内层镜面铬质倒角（凸起左上锐光/右下深边；凹陷反之）
            const auto in = r.reduced (1);
            if (! in.isEmpty())
            {
                const auto lit = isDark
                                   ? ref0.withAlpha (1.0f).interpolatedWith (white, hoverLook ? 0.44f : 0.34f)
                                   : hl.interpolatedWith (white, hoverLook ? 0.72f : 0.56f);
                const auto shad = isDark
                                    ? ref0.withAlpha (1.0f).interpolatedWith (black, 0.46f)
                                    : shdw;
                const bool inv = sunkenLook || pressedLook;
                g.setColour (inv ? shad : lit);
                g.fillRect (in.getX(), in.getY(), in.getWidth(), 1);
                g.fillRect (in.getX(), in.getY(), 1, in.getHeight());
                g.setColour (inv ? lit : shad);
                g.fillRect (in.getX(), in.getBottom() - 1, in.getWidth(), 1);
                g.fillRect (in.getRight() - 1, in.getY(), 1, in.getHeight());
            }
        }

        inline void drawGlassSurface (juce::Graphics& g,
                                      juce::Rectangle<int> r,
                                      juce::Colour fill,
                                      float radius,
                                      bool pressedLook,
                                      bool hoverLook,
                                      bool sunkenLook,
                                      bool activeLook = false)
        {
            if (r.getWidth() < 3 || r.getHeight() < 3)
                return;

            const auto rf     = r.toFloat();
            const float rad   = (radius > 0.5f)
                                  ? juce::jmin (radius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.5f)
                                  : 0.0f;
            const bool round  = (rad > 0.5f);
            const auto white  = juce::Colours::white;
            const auto black  = juce::Colours::black;
            const auto ref0   = fill.isTransparent() ? face : fill;
            const bool isDark = ref0.getPerceivedBrightness() < 0.45f;

            if (! fill.isTransparent())
            {
                const float a = fill.getFloatAlpha();
                const auto f  = pressedLook ? fill.interpolatedWith (sel, isDark ? 0.24f : 0.18f)
                              : activeLook  ? fill.interpolatedWith (sel, isDark ? 0.18f : 0.13f)
                              : hoverLook   ? fill.interpolatedWith (white, isDark ? 0.08f : 0.12f)
                                            : fill;
                if (sunkenLook)
                {
                    // 磨砂凹槽：深色烟熏黑曜晶 / 浅色冰雾亚克力微纵渐变
                    juce::ColourGradient wellGrad (f.interpolatedWith (black, isDark ? 0.16f : 0.05f).withAlpha (a),
                                                   0.0f, rf.getY(),
                                                   f.interpolatedWith (white, isDark ? 0.03f : 0.05f).withAlpha (a),
                                                   0.0f, rf.getBottom(), false);
                    g.setGradientFill (wellGrad);
                    if (round) g.fillRoundedRectangle (rf, rad);
                    else       g.fillRect (r);
                }
                else
                {
                    // 自适应明暗 3-Stop 液态玻璃渐变（暗主题走烟熏黑曜晶，保证 WCAG 对比；亮主题走冰雾亚克力）
                    if (isDark)
                    {
                        juce::ColourGradient grad (f.interpolatedWith (white, pressedLook ? 0.04f : (hoverLook ? 0.18f : 0.14f)).withAlpha (a),
                                                   0.0f, rf.getY(),
                                                   f.interpolatedWith (black, pressedLook ? 0.06f : 0.08f).withAlpha (a),
                                                   0.0f, rf.getBottom(), false);
                        if (! pressedLook)
                            grad.addColour (0.46, f.interpolatedWith (white, 0.04f).withAlpha (a));
                        g.setGradientFill (grad);
                    }
                    else
                    {
                        juce::ColourGradient grad (f.interpolatedWith (white, pressedLook ? 0.08f : (hoverLook ? 0.44f : 0.38f)).withAlpha (a),
                                                   0.0f, rf.getY(),
                                                   f.interpolatedWith (white, pressedLook ? 0.18f : 0.06f).withAlpha (a),
                                                   0.0f, rf.getBottom(), false);
                        if (! pressedLook)
                            grad.addColour (0.48, f.interpolatedWith (white, 0.16f).withAlpha (a));
                        g.setGradientFill (grad);
                    }
                    if (round) g.fillRoundedRectangle (rf, rad);
                    else       g.fillRect (r);

                    // 顶部 40% 凸面玻璃穹顶镜面高光带（弧面渐变而非平板色块）
                    if (! pressedLook && r.getHeight() >= 8)
                    {
                        auto sheen = rf.reduced (1.5f, 1.0f);
                        sheen.setHeight (juce::jmax (2.0f, rf.getHeight() * 0.40f));
                        const float topA = (isDark ? (hoverLook ? 0.17f : 0.12f)
                                                   : (hoverLook ? 0.26f : 0.20f)) * a;
                        const float botA = (isDark ? 0.02f : 0.04f) * a;
                        juce::ColourGradient dome (white.withAlpha (topA), 0.0f, sheen.getY(),
                                                   white.withAlpha (botA), 0.0f, sheen.getBottom(), false);
                        g.setGradientFill (dome);
                        if (round) g.fillRoundedRectangle (sheen, juce::jmax (1.0f, rad - 1.25f));
                        else       g.fillRect (sheen);
                    }

                    // 激活/按下态底部强调色玻璃内折射光晕（精准贴合圆角，零溢边）
                    if ((activeLook || pressedLook) && r.getHeight() >= 10 && ! isGreenScreenBackdrop())
                    {
                        juce::ColourGradient belly (sel.withAlpha (0.0f), 0.0f, rf.getY(),
                                                    sel.withAlpha ((isDark ? 0.28f : 0.20f) * a), 0.0f, rf.getBottom(), false);
                        belly.addColour (0.50, sel.withAlpha (0.0f));
                        g.setGradientFill (belly);
                        if (round) g.fillRoundedRectangle (rf, rad);
                        else       g.fillRect (r);
                    }
                }
            }

            // 稳定通透的外框（0.34-0.40f 暗边；激活态透出主题强调光）
            const auto outerRim = (activeLook || pressedLook)
                                    ? sel.withAlpha (0.72f)
                                    : (isDark ? black.withAlpha (sunkenLook ? 0.54f : 0.40f)
                                              : dark.withAlpha (sunkenLook ? 0.40f : 0.34f));
            g.setColour (outerRim);
            if (round) g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            else       fillRectFrame1Px (g, r, outerRim);

            // 深色烟熏水晶：极细 0.8px 内冰光轮廓，让暗色主题下玻璃厚度清晰可辨
            if (isDark && ! sunkenLook && r.getWidth() >= 6 && r.getHeight() >= 6)
            {
                g.setColour (white.withAlpha (hoverLook ? 0.20f : 0.12f));
                if (round) g.drawRoundedRectangle (rf.reduced (1.25f), juce::jmax (0.5f, rad - 0.75f), 0.8f);
                else       fillRectFrame1Px (g, r.reduced (1), white.withAlpha (hoverLook ? 0.16f : 0.10f));
            }

            // 锐利 1px 内顶冰光沿 + 底部微弱折光反光沿
            const auto in    = r.reduced (1);
            const int  inset = round ? juce::jmax (2, (int) std::round (rad * 0.65f)) : 1;
            const int  w     = in.getWidth() - inset * 2;
            if (w > 1)
            {
                if (sunkenLook)
                {
                    g.setColour (black.withAlpha (isDark ? 0.28f : 0.14f));
                    g.fillRect (in.getX() + inset, in.getY(), w, 1);
                    if (in.getHeight() >= 6)
                    {
                        g.setColour (black.withAlpha (isDark ? 0.14f : 0.06f));
                        g.fillRect (in.getX() + inset, in.getY() + 1, w, 1);
                    }
                    g.setColour (white.withAlpha (isDark ? 0.22f : 0.52f));
                    g.fillRect (in.getX() + inset, in.getBottom() - 1, w, 1);
                }
                else
                {
                    g.setColour (white.withAlpha (isDark ? (hoverLook ? 0.52f : 0.36f)
                                                         : (hoverLook ? 0.88f : 0.74f)));
                    g.fillRect (in.getX() + inset, in.getY(), w, 1);
                    if (r.getHeight() >= 10)
                    {
                        const auto botRim = activeLook ? sel.interpolatedWith (white, 0.50f) : white;
                        g.setColour (botRim.withAlpha (isDark ? 0.16f : 0.32f));
                        g.fillRect (in.getX() + inset, in.getBottom() - 1, w, 1);
                    }
                }
            }
        }

        inline void drawNeonSurface (juce::Graphics& g,
                                     juce::Rectangle<int> r,
                                     juce::Colour fill,
                                     float radius,
                                     bool hot,
                                     bool hoverLook,
                                     bool sunkenLook)
        {
            if (r.getWidth() < 3 || r.getHeight() < 3)
                return;

            const auto rf     = r.toFloat();
            const float rad   = (radius > 0.5f)
                                  ? juce::jmin (radius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.35f)
                                  : 0.0f;
            const bool round  = (rad > 0.5f);
            const auto black  = juce::Colours::black;
            const auto ref0   = fill.isTransparent() ? face : fill;
            const bool isDark = ref0.getPerceivedBrightness() < 0.45f;

            if (! fill.isTransparent())
            {
                const float a = fill.getFloatAlpha();
                // 烟熏暗井底色（暗色主题更深邃，浅色主题克制微压暗，确保深/浅主题均达 WCAG 对比）
                const auto smoked = fill.interpolatedWith (black, isDark ? (sunkenLook ? 0.28f : 0.20f)
                                                                         : (sunkenLook ? 0.11f : 0.07f));
                const auto body   = hot       ? smoked.interpolatedWith (sel, isDark ? 0.24f : 0.14f)
                                  : hoverLook ? smoked.interpolatedWith (sel, isDark ? 0.13f : 0.08f)
                                              : smoked;
                if (! sunkenLook && r.getHeight() >= 8)
                {
                    const auto botCol = (hot || hoverLook)
                                          ? body.interpolatedWith (sel, isDark ? 0.12f : 0.07f)
                                          : body.interpolatedWith (black, isDark ? 0.08f : 0.03f);
                    juce::ColourGradient grad (body.withAlpha (a), 0.0f, rf.getY(),
                                               botCol.withAlpha (a), 0.0f, rf.getBottom(), false);
                    g.setGradientFill (grad);
                }
                else
                {
                    g.setColour (body.withAlpha (a));
                }
                if (round) g.fillRoundedRectangle (rf, rad);
                else       g.fillRect (r);
            }

            // 双通道霓虹灯管边框：外圈柔光晕（sel / accent2）+ 内圈 1px 炽热白芯灯管
            if (sunkenLook)
            {
                const auto outerCol = dark.withAlpha (isDark ? 0.90f : 0.78f);
                const auto innerCol = sel.withAlpha (0.55f);
                if (round)
                {
                    g.setColour (outerCol);
                    g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                    if (r.getWidth() >= 5 && r.getHeight() >= 5)
                    {
                        g.setColour (innerCol);
                        g.drawRoundedRectangle (rf.reduced (1.5f), juce::jmax (0.5f, rad - 0.75f), 0.9f);
                    }
                }
                else
                {
                    fillRectFrame1Px (g, r, outerCol);
                    if (r.getWidth() >= 5 && r.getHeight() >= 5)
                        fillRectFrame1Px (g, r.reduced (1), innerCol);
                }
            }
            else
            {
                const auto auraBase = hot ? sel : sel.interpolatedWith (accent2, 0.25f);
                const auto auraCol  = auraBase.withAlpha (hot ? 0.58f : (hoverLook ? 0.48f : 0.34f));
                const auto coreCol  = neonTubeCore (hot || hoverLook).withAlpha (hot ? 0.98f : (hoverLook ? 0.92f : 0.78f));
                if (round)
                {
                    g.setColour (auraCol);
                    g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.35f);
                    if (r.getWidth() >= 5 && r.getHeight() >= 5)
                    {
                        g.setColour (coreCol);
                        g.drawRoundedRectangle (rf.reduced (1.25f), juce::jmax (0.5f, rad - 0.75f), 0.95f);
                    }
                }
                else
                {
                    fillRectFrame1Px (g, r, auraCol);
                    if (r.getWidth() >= 5 && r.getHeight() >= 5)
                        fillRectFrame1Px (g, r.reduced (1), coreCol);
                }
            }
        }

        inline void drawCrtSurface (juce::Graphics& g,
                                    juce::Rectangle<int> r,
                                    juce::Colour fill,
                                    bool hot,
                                    bool hoverLook,
                                    bool sunkenLook)
        {
            if (r.getWidth() < 2 || r.getHeight() < 2)
                return;

            const auto ref0   = fill.isTransparent() ? face : fill;
            const bool isDark = ref0.getPerceivedBrightness() < 0.45f;

            if (! fill.isTransparent())
            {
                const float a = fill.getFloatAlpha();
                const auto body = sunkenLook ? fill.interpolatedWith (juce::Colours::black, isDark ? 0.14f : 0.04f)
                                : hot        ? fill.interpolatedWith (sel, isDark ? 0.24f : 0.18f)
                                : hoverLook  ? fill.interpolatedWith (sel, isDark ? 0.12f : 0.09f)
                                             : fill;
                g.setColour (body.withAlpha (a));
                g.fillRect (r);
            }

            // 0px 锐利磷光单色机箱外框 + 内凹/激活磷光细线
            fillRectFrame1Px (g, r, sunkenLook ? sel.withAlpha (0.62f)
                                               : sel.withAlpha (hot || hoverLook ? 1.0f : 0.85f));
            if (r.getWidth() >= 4 && r.getHeight() >= 4)
            {
                if (hot)
                {
                    fillRectFrame1Px (g, r.reduced (1), sel.withAlpha (0.76f));
                    if (r.getWidth() >= 10 && r.getHeight() >= 10)
                    {
                        g.setColour (sel.interpolatedWith (juce::Colours::white, 0.25f));
                        g.fillRect (r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 1);
                    }
                }
                else if (sunkenLook)
                {
                    fillRectFrame1Px (g, r.reduced (1), sel.withAlpha (0.18f));
                }
                else
                {
                    fillRectFrame1Px (g, r.reduced (1), sel.withAlpha (hoverLook ? 0.42f : 0.20f));
                }
            }
        }
    }

    const SkinTokens& getSkinTokens (SkinId id) noexcept
    {
        if (id == SkinId::greenScreen)
            return kKeyedTable[skinToTableIndex (getCurrentSkin())];

        const int idx = skinToTableIndex (id);
        return isChromaKeySkin() ? kKeyedTable[idx] : kNormalTable[idx];
    }

    namespace Craft
    {
        void drawPanelWell (juce::Graphics& g,
                            juce::Rectangle<int> r,
                            juce::Colour fill,
                            WellKind kind)
        {
            if (r.isEmpty())
                return;

            const auto& t = getSkinTokens();
            if (t.isChromaKey)
                return;   // Green Screen：画布/卡片/槽/徽章一律跳过，零绿幕污染

            juce::Graphics::ScopedSaveState savedState (g);

            if (kind == WellKind::canvas || kind == WellKind::recessed)
            {
                drawSunken (g, r, fill);
                return;
            }

            if (kind == WellKind::trough)
            {
                switch (t.frameStyle)
                {
                    case FrameStyle::pixelBevel:
                        drawSunken (g, r, fill);
                        return;

                    case FrameStyle::machinedChrome:
                        drawChromeSurface (g, r, fill, false, false, true);
                        return;

                    case FrameStyle::modernStudio:
                    {
                        const bool darkFill = fill.getPerceivedBrightness() < 0.45f;
                        const auto troughFill = fill.interpolatedWith (juce::Colours::black,
                                                                       darkFill ? 0.16f : 0.045f)
                                                    .withAlpha (fill.getFloatAlpha());
                        const float rad = (r.getWidth() >= 6 && r.getHeight() >= 6)
                                            ? juce::jmin (t.barCellRadius + 1.75f,
                                                          (float) juce::jmin (r.getWidth(), r.getHeight()) * 0.35f)
                                            : 0.0f;
                        drawModernSurface (g, r, troughFill, rad, false, false, false, false, true);
                        return;
                    }

                    case FrameStyle::frostedGlass:
                    {
                        const float rad = allowSmallRounding (r) ? t.barCellRadius + 1.5f : 0.0f;
                        drawGlassSurface (g, r, fill, rad, false, false, true, false);
                        return;
                    }

                    case FrameStyle::neonTube:
                    {
                        const float rad = allowSmallRounding (r) ? t.cardRadius : 0.0f;
                        drawNeonSurface (g, r, fill, rad, false, false, true);
                        return;
                    }

                    case FrameStyle::crtPhosphor:
                        drawCrtSurface (g, r, fill, false, false, true);
                        return;

                    case FrameStyle::chromaNone:
                    default:
                        return;
                }
            }

            if (kind == WellKind::card)
            {
                const bool isDark = fill.getPerceivedBrightness() < 0.45f;
                switch (t.frameStyle)
                {
                    case FrameStyle::pixelBevel:
                    {
                        // 0px 锐利像素蚀刻卡片（Win95/XP 分组面板）
                        g.setColour (fill);
                        g.fillRect (r);
                        if (r.getWidth() >= 4 && r.getHeight() >= 4)
                        {
                            g.setColour (hl.withAlpha (isDark ? 0.16f : 0.55f));
                            g.fillRect (r.getX() + 1, r.getY() + 1, r.getWidth() - 2, 1);
                            g.fillRect (r.getX() + 1, r.getY() + 1, 1, r.getHeight() - 2);
                        }
                        fillRectFrame1Px (g, r, shdw.withAlpha (isDark ? 0.52f : 0.42f));
                        return;
                    }

                    case FrameStyle::machinedChrome:
                    {
                        // 拉丝铝面板卡片：4-Stop 金属拉丝光泽 + 1px 锐利左上镜面高光 + 1px 铣削深底边
                        const float fa = fill.getFloatAlpha();
                        const auto white = juce::Colours::white;
                        const auto black = juce::Colours::black;
                        juce::ColourGradient grad (fill.interpolatedWith (white, isDark ? 0.08f : 0.22f).withAlpha (fa),
                                                   (float) r.getX(), (float) r.getY(),
                                                   fill.interpolatedWith (black, isDark ? 0.10f : 0.06f).withAlpha (fa),
                                                   (float) r.getRight(), (float) r.getBottom(), false);
                        grad.addColour (0.38, fill.interpolatedWith (white, isDark ? 0.12f : 0.14f).withAlpha (fa));
                        grad.addColour (0.44, fill.interpolatedWith (black, isDark ? 0.03f : 0.02f).withAlpha (fa));
                        g.setGradientFill (grad);
                        g.fillRect (r);

                        fillRectFrame1Px (g, r, isDark ? fill.withAlpha (1.0f).interpolatedWith (black, 0.65f)
                                                       : dark.withAlpha (0.82f));
                        if (r.getWidth() >= 4 && r.getHeight() >= 4)
                        {
                            const auto in = r.reduced (1);
                            g.setColour (isDark ? white.withAlpha (0.20f)
                                                : hl.interpolatedWith (white, 0.55f).withAlpha (0.72f));
                            g.fillRect (in.getX(), in.getY(), in.getWidth(), 1);
                            g.fillRect (in.getX(), in.getY(), 1, in.getHeight());
                            g.setColour (isDark ? black.withAlpha (0.38f) : shdw.withAlpha (0.45f));
                            g.fillRect (in.getX(), in.getBottom() - 1, in.getWidth(), 1);
                            g.fillRect (in.getRight() - 1, in.getY(), 1, in.getHeight());
                        }
                        return;
                    }

                    case FrameStyle::modernStudio:
                        drawModernSurface (g, r, fill, t.cardRadius, true, false, false, false, false);
                        return;

                    case FrameStyle::frostedGlass:
                        drawGlassSurface (g, r, fill, t.cardRadius, false, false, false, false);
                        return;

                    case FrameStyle::neonTube:
                    {
                        const float rad = juce::jmin (t.cardRadius,
                                                      juce::jmin ((float) r.getWidth(), (float) r.getHeight()) * 0.25f);
                        drawNeonSurface (g, r, fill, rad, false, false, false);
                        if (r.getWidth() >= 14 && r.getHeight() >= 14)
                            drawPixelCorners (g, r, accent2.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.85f), 4, 1);
                        return;
                    }

                    case FrameStyle::crtPhosphor:
                    {
                        drawCrtSurface (g, r, fill, false, false, false);
                        if (r.getWidth() >= 10 && r.getHeight() >= 10)
                            drawPixelCorners (g, r, sel.withAlpha (0.90f), 4, 1);
                        return;
                    }

                    case FrameStyle::chromaNone:
                    default:
                        return;
                }
            }

            // WellKind::badge
            drawBadgePill (g, r, fill, {});
        }

        void drawControlChrome (juce::Graphics& g,
                                juce::Rectangle<int> r,
                                juce::Colour fill,
                                ControlState state,
                                float radiusOverride)
        {
            if (r.isEmpty())
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const auto& t = getSkinTokens();
            const bool isHover    = (state == ControlState::hover);
            const bool isPressed  = (state == ControlState::pressed);
            const bool isActive   = (state == ControlState::active);
            const bool isDisabled = (state == ControlState::disabled);

            const juce::Colour baseFill = isDisabled ? fill.withMultipliedAlpha (0.65f) : fill;

            switch (t.frameStyle)
            {
                case FrameStyle::pixelBevel:
                {
                    if (isDisabled)
                    {
                        g.setColour (baseFill);
                        g.fillRect (r);
                        fillRectFrame1Px (g, r, shdw.withAlpha (0.55f));
                        return;
                    }
                    if (isPressed || isActive)
                    {
                        drawPressed (g, r, baseFill);
                        if (isActive && r.getWidth() >= 8 && r.getHeight() >= 8)
                        {
                            g.setColour (pink200.withAlpha (0.65f));
                            for (int py = r.getY() + 3; py < r.getBottom() - 2; py += 4)
                                for (int px = r.getX() + 3 + ((py / 4) % 2) * 2; px < r.getRight() - 2; px += 4)
                                    g.fillRect (px, py, 2, 2);
                        }
                        return;
                    }
                    drawRaised (g, r, isHover ? baseFill.brighter (0.06f) : baseFill);
                    return;
                }

                case FrameStyle::machinedChrome:
                {
                    drawChromeSurface (g, r, baseFill, isPressed || isActive, isHover && ! isDisabled, false);
                    if (isActive && r.getWidth() >= 10 && r.getHeight() >= 8)
                    {
                        // 硬件机架激活态：顶部内沿 2px 发光强调 LED 条 + 1px 亮芯
                        g.setColour (sel);
                        g.fillRect (r.getX() + 2, r.getY() + 2, r.getWidth() - 4, 2);
                        g.setColour (sel.interpolatedWith (juce::Colours::white, 0.45f));
                        g.fillRect (r.getX() + 3, r.getY() + 2, juce::jmax (1, r.getWidth() - 6), 1);
                    }
                    return;
                }

                case FrameStyle::modernStudio:
                case FrameStyle::chromaNone:   // 绿幕悬停显露态：使用现代简约质感
                {
                    const float defaultRad = allowSmallRounding (r) ? t.controlRadius : 0.0f;
                    const float rad = (radiusOverride >= 0.0f) ? radiusOverride : defaultRad;
                    drawModernSurface (g, r, baseFill, rad, true, isPressed, isActive, isHover && ! isDisabled, false);
                    return;
                }

                case FrameStyle::frostedGlass:
                {
                    const float defaultRad = allowSmallRounding (r) ? t.controlRadius : 0.0f;
                    const float rad = (radiusOverride >= 0.0f) ? radiusOverride : defaultRad;
                    drawGlassSurface (g, r, baseFill, rad, isPressed, isHover && ! isDisabled, false, isActive);
                    if (isActive && r.getWidth() >= 12 && r.getHeight() >= 8)
                    {
                        const int bw = juce::jlimit (8, r.getWidth() - 6, r.getWidth() * 2 / 5);
                        const float bx = (float) (r.getCentreX() - bw / 2);
                        const float by = (float) r.getBottom() - 3.5f;
                        g.setColour (sel.withAlpha (0.92f));
                        g.fillRoundedRectangle (bx, by, (float) bw, 2.2f, 1.1f);
                        g.setColour (juce::Colours::white.withAlpha (0.55f));
                        g.fillRect (bx + 1.0f, by, juce::jmax (1.0f, (float) bw - 2.0f), 0.75f);
                    }
                    return;
                }

                case FrameStyle::neonTube:
                {
                    const float defaultRad = allowSmallRounding (r) ? t.controlRadius : 0.0f;
                    const float rad = (radiusOverride >= 0.0f) ? juce::jmin (radiusOverride, t.controlRadius) : defaultRad;
                    drawNeonSurface (g, r, baseFill, rad, isPressed || isActive, isHover && ! isDisabled, false);
                    if (isActive && r.getWidth() >= 8 && r.getHeight() >= 6)
                    {
                        if (! isGreenScreenBackdrop())
                        {
                            g.setColour (sel.withAlpha (0.36f));
                            g.fillRect (r.getX() + 2, r.getBottom() - 4, r.getWidth() - 4, 3);
                        }
                        g.setColour (neonTubeCore (true));
                        g.fillRect (r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 2);
                    }
                    return;
                }

                case FrameStyle::crtPhosphor:
                {
                    drawCrtSurface (g, r, baseFill, isPressed || isActive, isHover && ! isDisabled, false);
                    return;
                }
            }
        }

        void drawTabButton (juce::Graphics& g,
                            juce::Rectangle<int> r,
                            bool active,
                            bool hover,
                            const juce::String& text,
                            float fontHeight)
        {
            if (r.isEmpty())
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const auto& t = getSkinTokens();
            juce::Colour textCol = readableInkOn (btnFace);

            switch (t.frameStyle)
            {
                case FrameStyle::pixelBevel:
                {
                    // Win95 经典文件夹页签：选中项完整凸起并贴底无下框 + 顶部 2px 强调条
                    auto tabR = active ? r : r.withTrimmedTop (1);
                    const auto fill = active ? face : (hover ? btnFace.brighter (0.05f) : btnFace.darker (0.04f));
                    g.setColour (fill);
                    g.fillRect (tabR);

                    // 左 / 上 高光，右 深边
                    g.setColour (hl);
                    g.fillRect (tabR.getX(), tabR.getY(), tabR.getWidth(), 1);
                    g.fillRect (tabR.getX(), tabR.getY(), 1, tabR.getHeight());
                    g.setColour (dark);
                    g.fillRect (tabR.getRight() - 1, tabR.getY(), 1, tabR.getHeight());
                    if (! active)
                    {
                        g.setColour (shdw);
                        g.fillRect (tabR.getX(), tabR.getBottom() - 1, tabR.getWidth(), 1);
                    }
                    else
                    {
                        g.setColour (sel);
                        g.fillRect (tabR.getX() + 1, tabR.getY() + 1, juce::jmax (0, tabR.getWidth() - 2), 2);
                    }
                    textCol = readableInkOn (fill);
                    break;
                }

                case FrameStyle::machinedChrome:
                {
                    // 拉丝铝机架页签：非激活态内凹钢灰；激活态抬升 + 顶部 2px 发光强调灯条 + 底沿无接缝融入卡片
                    const bool isDark = face.getPerceivedBrightness() < 0.45f;
                    const auto tabR   = active ? r : r.withTrimmedTop (1);
                    const auto fill   = active
                                          ? (isDark ? face.interpolatedWith (juce::Colours::white, 0.08f) : face)
                                          : (isDark ? btnFace.interpolatedWith (juce::Colours::black, 0.14f)
                                                    : btnFace.interpolatedWith (juce::Colours::black, 0.06f));
                    drawChromeSurface (g, tabR, fill, false, hover && ! active, ! active);
                    if (active)
                    {
                        // 擦除底沿闭合边使其顺畅衔接到下方卡片
                        g.setColour (fill);
                        g.fillRect (tabR.getX() + 1, tabR.getBottom() - 1, juce::jmax (0, tabR.getWidth() - 2), 1);
                        // 顶部 2px 强调色硬件指示灯条
                        g.setColour (sel);
                        g.fillRect (tabR.getX() + 2, tabR.getY() + 1, juce::jmax (0, tabR.getWidth() - 4), 2);
                        g.setColour (sel.interpolatedWith (juce::Colours::white, 0.45f));
                        g.fillRect (tabR.getX() + 3, tabR.getY() + 1, juce::jmax (0, tabR.getWidth() - 6), 1);
                        textCol = readableInkOn (fill);
                    }
                    else
                    {
                        textCol = readableInkOn (fill).withAlpha (hover ? 0.94f : 0.74f);
                    }
                    break;
                }

                case FrameStyle::modernStudio:
                case FrameStyle::chromaNone:
                {
                    const auto pillR = r.reduced (1, 1);
                    const auto rf    = pillR.toFloat();
                    const float rad  = juce::jmin (t.tabRadius, rf.getHeight() * 0.45f);
                    const bool isDark = face.getPerceivedBrightness() < 0.45f;

                    if (active)
                    {
                        // 选中页签：相对底条形成清晰微抬升 + 底部 2px 锐利强调条
                        const auto activeFill = isDark
                            ? face.interpolatedWith (juce::Colours::white, 0.09f)
                            : content.interpolatedWith (juce::Colours::white, 0.35f);
                        drawModernSurface (g, pillR, activeFill, rad, true, false, false, false, false);
                        if (pillR.getWidth() >= 12 && pillR.getHeight() >= 8)
                        {
                            const int bw = juce::jlimit (10, pillR.getWidth() - 6, pillR.getWidth() * 3 / 5);
                            g.setColour (sel);
                            g.fillRoundedRectangle ((float) (pillR.getCentreX() - bw / 2),
                                                    (float) pillR.getBottom() - 2.5f,
                                                    (float) bw, 2.0f, 1.0f);
                        }
                        textCol = readableInkOn (activeFill);
                    }
                    else
                    {
                        if (hover)
                        {
                            g.setColour (isDark ? juce::Colours::white.withAlpha (0.07f)
                                                : juce::Colours::black.withAlpha (0.055f));
                            g.fillRoundedRectangle (rf, rad);
                        }
                        textCol = readableInkOn (face).withAlpha (hover ? 0.94f : 0.70f);
                    }
                    break;
                }

                case FrameStyle::frostedGlass:
                {
                    const auto pillR  = r.reduced (1, 1);
                    const float rad   = juce::jmin (t.tabRadius, (float) pillR.getHeight() * 0.48f);
                    const bool isDark = face.getPerceivedBrightness() < 0.45f;
                    if (active)
                    {
                        const auto activeFill = isDark
                            ? face.interpolatedWith (juce::Colours::white, 0.10f)
                            : content.interpolatedWith (juce::Colours::white, 0.30f);
                        drawGlassSurface (g, pillR, activeFill, rad, false, false, false, true);
                        const int bw = juce::jlimit (10, juce::jmax (10, pillR.getWidth() - 6), pillR.getWidth() * 3 / 5);
                        const float bx = (float) (pillR.getCentreX() - bw / 2);
                        const float by = (float) pillR.getBottom() - 3.25f;
                        g.setColour (sel);
                        g.fillRoundedRectangle (bx, by, (float) bw, 2.2f, 1.1f);
                        g.setColour (juce::Colours::white.withAlpha (0.55f));
                        g.fillRect (bx + 1.0f, by, juce::jmax (1.0f, (float) bw - 2.0f), 0.75f);
                        textCol = readableInkOn (activeFill);
                    }
                    else
                    {
                        drawGlassSurface (g, pillR, btnFace.withAlpha (hover ? 0.78f : 0.42f), rad,
                                          false, hover, false, false);
                        textCol = readableInkOn (face).withAlpha (hover ? 0.94f : 0.74f);
                    }
                    break;
                }

                case FrameStyle::neonTube:
                {
                    const auto tabR   = r.reduced (1, 1);
                    const bool isDark = face.getPerceivedBrightness() < 0.45f;
                    const auto fill   = active
                                          ? (isDark ? content.interpolatedWith (sel, 0.14f)
                                                    : face.interpolatedWith (sel, 0.10f))
                                          : face;
                    drawNeonSurface (g, tabR, fill, t.tabRadius, active, hover && ! active, ! active);
                    if (active && tabR.getWidth() >= 10 && tabR.getHeight() >= 8)
                    {
                        if (! isGreenScreenBackdrop())
                        {
                            g.setColour (sel.withAlpha (0.38f));
                            g.fillRect (tabR.getX() + 2, tabR.getBottom() - 4, tabR.getWidth() - 4, 3);
                        }
                        g.setColour (neonTubeCore (true));
                        g.fillRect (tabR.getX() + 2, tabR.getBottom() - 2, tabR.getWidth() - 4, 2);
                        // 顶部四角炽热强调光点
                        g.setColour (accent2.interpolatedWith (juce::Colours::white, 0.40f));
                        g.fillRect (tabR.getX() + 1, tabR.getY() + 1, 3, 1);
                        g.fillRect (tabR.getRight() - 4, tabR.getY() + 1, 3, 1);
                    }
                    const auto effBg = fill.interpolatedWith (juce::Colours::black, isDark ? 0.20f : 0.07f);
                    textCol = active ? readableInkOn (effBg)
                                     : readableInkOn (effBg).withAlpha (hover ? 0.92f : 0.72f);
                    break;
                }

                case FrameStyle::crtPhosphor:
                {
                    if (active)
                    {
                        g.setColour (sel);
                        g.fillRect (r);
                        g.setColour (juce::Colours::black.withAlpha (0.08f));
                        for (int py = r.getY() + 1; py < r.getBottom(); py += 2)
                            g.fillRect (r.getX(), py, r.getWidth(), 1);
                        g.setColour (sel.interpolatedWith (juce::Colours::white, 0.36f));
                        g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                        textCol = selInk;
                    }
                    else
                    {
                        const auto tabFill = hover ? content.interpolatedWith (sel, 0.14f) : content;
                        g.setColour (tabFill);
                        g.fillRect (r);
                        fillRectFrame1Px (g, r, sel.withAlpha (hover ? 0.80f : 0.48f));
                        // 经典终端 [ ] 内方括号角标
                        if (r.getWidth() >= 24 && r.getHeight() >= 12)
                        {
                            g.setColour (sel.withAlpha (hover ? 0.88f : 0.55f));
                            const int by0 = r.getY() + 3;
                            const int bh  = r.getHeight() - 6;
                            g.fillRect (r.getX() + 2, by0, 1, bh);
                            g.fillRect (r.getX() + 2, by0, 3, 1);
                            g.fillRect (r.getX() + 2, by0 + bh - 1, 3, 1);
                            g.fillRect (r.getRight() - 3, by0, 1, bh);
                            g.fillRect (r.getRight() - 5, by0, 3, 1);
                            g.fillRect (r.getRight() - 5, by0 + bh - 1, 3, 1);
                        }
                        textCol = readableInkOn (tabFill).withAlpha (hover ? 0.95f : 0.78f);
                    }
                    break;
                }
            }

            if (text.isNotEmpty())
            {
                const juce::String shown = y2k::tr (text);
                const auto textArea = r.reduced (3, 1);
                const float maxH = (float) juce::jmax (1, textArea.getHeight());
                const auto probe = SymbolGlyphs::buttonFont (shown, fontHeight, juce::Font::bold);
                const float fitted = (probe.getHeight() > maxH && probe.getHeight() > 0.0f)
                                       ? fontHeight * maxH / probe.getHeight()
                                       : fontHeight;
                drawCachedSymbolText (g, shown, textArea, juce::Justification::centred,
                                      fitted, juce::Font::bold, textCol,
                                      StaticTextFont::button, juce::Colours::transparentBlack, false);
            }
        }

        void drawMeterCell (juce::Graphics& g,
                            juce::Rectangle<float> r,
                            juce::Colour col,
                            bool lit,
                            float pulse)
        {
            if (r.getWidth() <= 0.25f || r.getHeight() <= 0.25f)
                return;

            const auto& t = getSkinTokens();
            if (! lit && t.isChromaKey)
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const float p = juce::jlimit (0.0f, 1.0f, pulse);
            const juce::Colour litCol = (p > 0.001f) ? col.brighter (0.18f * p) : col;

            switch (t.cellFinish)
            {
                case CellFinish::flatPixel:
                {
                    const auto ri = r.toNearestInt();
                    if (ri.isEmpty())
                        return;
                    if (! lit)
                    {
                        g.setColour (col.withMultipliedAlpha (t.unlitCellAlpha));
                        g.fillRect (ri);
                        return;
                    }
                    g.setColour (litCol);
                    g.fillRect (ri);
                    // 1px 左上硬件像素高光（经典 1998-2002 LED 颗粒感）
                    if (ri.getWidth() >= 3 && ri.getHeight() >= 3)
                    {
                        g.setColour (juce::Colours::white.withAlpha (t.cellHighlightAlpha * litCol.getFloatAlpha()));
                        g.fillRect (ri.getX(), ri.getY(), ri.getWidth(), 1);
                        g.fillRect (ri.getX(), ri.getY() + 1, 1, juce::jmax (1, ri.getHeight() - 2));
                    }
                    if (ri.getWidth() >= 4 && ri.getHeight() >= 5)
                    {
                        g.setColour (juce::Colours::black.withAlpha (0.18f * litCol.getFloatAlpha()));
                        g.fillRect (ri.getX(), ri.getBottom() - 1, ri.getWidth(), 1);
                    }
                    return;
                }

                case CellFinish::metallicBevel:
                {
                    const bool isDark = content.getPerceivedBrightness() < 0.45f;
                    if (! lit)
                    {
                        // 暗凹金属灯座 + 1px 蚀刻底边（绿幕/透明由入口跳过）
                        g.setColour (isDark ? juce::Colours::black.withAlpha (0.28f)
                                            : col.withMultipliedAlpha (t.unlitCellAlpha * 0.85f));
                        g.fillRect (r);
                        if (isDark)
                        {
                            g.setColour (col.withMultipliedAlpha (t.unlitCellAlpha));
                            g.fillRect (r);
                        }
                        if (r.getWidth() >= 3.0f && r.getHeight() >= 3.0f)
                        {
                            g.setColour (isDark ? juce::Colours::white.withAlpha (0.12f)
                                                : dark.withAlpha (0.20f));
                            g.fillRect (r.getX(), r.getBottom() - 1.0f, r.getWidth(), 1.0f);
                        }
                        return;
                    }

                    // 3-Stop 宝石/金属折光 LED：顶高光折痕 → 饱满核心 → 底部微暗倒角
                    const bool horizontal = (r.getWidth() >= r.getHeight());
                    juce::ColourGradient grad = horizontal
                        ? juce::ColourGradient (litCol.interpolatedWith (juce::Colours::white, t.cellHighlightAlpha),
                                                r.getX(), r.getY(),
                                                litCol.darker (0.24f),
                                                r.getX(), r.getBottom(), false)
                        : juce::ColourGradient (litCol.interpolatedWith (juce::Colours::white, t.cellHighlightAlpha),
                                                r.getX(), r.getY(),
                                                litCol.darker (0.24f),
                                                r.getRight(), r.getY(), false);
                    grad.addColour (0.38, litCol);
                    g.setGradientFill (grad);
                    g.fillRect (r);

                    // 1px 锐利顶高光折线
                    if (r.getWidth() >= 3.0f && r.getHeight() >= 3.0f)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.40f * litCol.getFloatAlpha()));
                        if (horizontal)
                            g.fillRect (r.getX() + 0.5f, r.getY() + 0.5f, juce::jmax (1.0f, r.getWidth() - 1.0f), 1.0f);
                        else
                            g.fillRect (r.getX() + 0.5f, r.getY() + 0.5f, 1.0f, juce::jmax (1.0f, r.getHeight() - 1.0f));
                    }
                    // 0.5px 精密外切边
                    if (r.getWidth() >= 4.0f && r.getHeight() >= 4.0f)
                    {
                        g.setColour (dark.withAlpha (0.38f));
                        g.drawRect (r, 0.75f);
                    }
                    return;
                }

                case CellFinish::cleanRounded:
                {
                    const float rad = juce::jmin (t.barCellRadius, juce::jmin (r.getWidth(), r.getHeight()) * 0.44f);
                    if (! lit)
                    {
                        g.setColour (col.withMultipliedAlpha (t.unlitCellAlpha));
                        if (rad > 0.4f) g.fillRoundedRectangle (r, rad);
                        else            g.fillRect (r);
                        // 浅色画布上为未点亮圆角格补一条极克制 0.10f 轮廓线，增强可读性
                        if (! t.isChromaKey && content.getPerceivedBrightness() >= 0.55f
                            && r.getWidth() >= 3.0f && r.getHeight() >= 3.0f)
                        {
                            g.setColour (skinHairlineOn (content, 0.10f));
                            if (rad > 0.4f)
                                g.drawRoundedRectangle (r.reduced (0.5f), juce::jmax (0.5f, rad - 0.25f), 0.85f);
                            else
                                g.drawRect (r, 1.0f);
                        }
                        return;
                    }
                    if (! isGreenScreenBackdrop() && r.getHeight() >= 4.0f && r.getWidth() >= 4.0f)
                    {
                        juce::ColourGradient grad (litCol.interpolatedWith (juce::Colours::white, t.cellHighlightAlpha),
                                                   r.getX(), r.getY(),
                                                   litCol,
                                                   r.getX(), r.getBottom(), false);
                        g.setGradientFill (grad);
                    }
                    else
                    {
                        g.setColour (litCol);
                    }
                    if (rad > 0.4f) g.fillRoundedRectangle (r, rad);
                    else            g.fillRect (r);
                    return;
                }

                case CellFinish::glossyCapsule:
                {
                    const float rad = juce::jmin (t.barCellRadius, juce::jmin (r.getWidth(), r.getHeight()) * 0.5f);
                    const bool isDark = content.getPerceivedBrightness() < 0.45f;
                    if (! lit)
                    {
                        // 磨砂玻璃未点亮灯槽（绿幕/透明由入口跳过）
                        g.setColour (col.withMultipliedAlpha (t.unlitCellAlpha));
                        if (rad > 0.5f) g.fillRoundedRectangle (r, rad);
                        else            g.fillRect (r);
                        if (r.getWidth() >= 4.0f && r.getHeight() >= 4.0f)
                        {
                            auto frost = r.withHeight (r.getHeight() * 0.42f).reduced (0.5f, 0.25f);
                            g.setColour (juce::Colours::white.withAlpha (isDark ? 0.06f : 0.14f));
                            if (rad > 0.5f) g.fillRoundedRectangle (frost, juce::jmax (0.5f, rad * 0.7f));
                            else            g.fillRect (frost);
                            g.setColour (juce::Colours::white.withAlpha (isDark ? 0.10f : 0.20f));
                            if (rad > 0.5f) g.drawRoundedRectangle (r.reduced (0.4f), rad, 0.75f);
                            else            g.drawRect (r, 0.75f);
                        }
                        return;
                    }

                    const float fa = litCol.getFloatAlpha();
                    if (! isGreenScreenBackdrop() && p > 0.12f)
                    {
                        g.setColour (litCol.withMultipliedAlpha (0.20f * p));
                        if (rad > 0.5f) g.fillRoundedRectangle (r.expanded (0.75f), rad + 0.5f);
                        else            g.fillRect (r.expanded (0.75f));
                    }

                    // 3-Stop 晶莹胶囊：顶透光 → 饱满主体 → 底部内折射回光
                    juce::ColourGradient bodyGrad (litCol.interpolatedWith (juce::Colours::white, 0.20f).withAlpha (fa),
                                                   r.getX(), r.getY(),
                                                   litCol.interpolatedWith (juce::Colours::white, 0.24f).withAlpha (fa),
                                                   r.getX(), r.getBottom(), false);
                    bodyGrad.addColour (0.52, litCol);
                    g.setGradientFill (bodyGrad);
                    if (rad > 0.5f) g.fillRoundedRectangle (r, rad);
                    else            g.fillRect (r);

                    // 顶部 44% 水晶穹顶高光弧
                    if (r.getHeight() >= 3.2f)
                    {
                        auto sheen = r.withHeight (r.getHeight() * 0.44f).reduced (0.5f, 0.2f);
                        juce::ColourGradient dome (juce::Colours::white.withAlpha (t.cellHighlightAlpha * fa),
                                                   sheen.getX(), sheen.getY(),
                                                   juce::Colours::white.withAlpha (0.04f * fa),
                                                   sheen.getX(), sheen.getBottom(), false);
                        g.setGradientFill (dome);
                        if (rad > 0.5f) g.fillRoundedRectangle (sheen, juce::jmax (0.5f, rad * 0.72f));
                        else            g.fillRect (sheen);
                    }
                    return;
                }

                case CellFinish::neonLit:
                {
                    const float rad = juce::jmin (t.barCellRadius, juce::jmin (r.getWidth(), r.getHeight()) * 0.42f);
                    if (! lit)
                    {
                        g.setColour (col.withMultipliedAlpha (t.unlitCellAlpha));
                        if (rad > 0.35f) g.fillRoundedRectangle (r, rad);
                        else             g.fillRect (r);
                        if (r.getWidth() >= 4.0f && r.getHeight() >= 4.0f)
                        {
                            g.setColour (col.withMultipliedAlpha (0.22f));
                            if (rad > 0.35f) g.drawRoundedRectangle (r.reduced (0.4f), rad, 0.75f);
                            else             g.drawRect (r, 0.75f);
                        }
                        return;
                    }
                    // 外辉光（仅在纯绿幕下跳过避免绿边；Normal 与 Transparent 均保留真实辉光）
                    if (! isGreenScreenBackdrop())
                    {
                        g.setColour (litCol.withMultipliedAlpha (0.24f + 0.10f * p));
                        if (rad > 0.35f) g.fillRoundedRectangle (r.expanded (1.0f), rad + 0.75f);
                        else             g.fillRect (r.expanded (1.0f));
                    }
                    g.setColour (litCol);
                    if (rad > 0.35f) g.fillRoundedRectangle (r, rad);
                    else             g.fillRect (r);
                    // 炽热白芯灯丝（沿长轴居中贯穿）
                    if (r.getWidth() >= 3.0f && r.getHeight() >= 3.0f)
                    {
                        const bool horizontal = (r.getWidth() >= r.getHeight());
                        juce::Rectangle<float> core;
                        if (horizontal)
                            core = r.reduced (juce::jmin (1.5f, r.getWidth() * 0.14f), r.getHeight() * 0.28f);
                        else
                            core = r.reduced (r.getWidth() * 0.28f, juce::jmin (1.5f, r.getHeight() * 0.14f));
                        g.setColour (litCol.interpolatedWith (juce::Colours::white, t.cellHighlightAlpha));
                        if (rad > 0.35f) g.fillRoundedRectangle (core, juce::jmax (0.5f, rad * 0.5f));
                        else             g.fillRect (core);
                    }
                    return;
                }

                case CellFinish::phosphorBar:
                {
                    const auto ri = r.toNearestInt();
                    if (ri.isEmpty())
                        return;
                    if (! lit)
                    {
                        g.setColour (col.withMultipliedAlpha (t.unlitCellAlpha));
                        g.fillRect (ri);
                        return;
                    }
                    if (! isGreenScreenBackdrop())
                    {
                        g.setColour (litCol.withMultipliedAlpha (0.20f));
                        g.fillRect (ri.expanded (1));
                    }
                    g.setColour (litCol);
                    g.fillRect (ri);
                    // 中心微弱磷光高亮细芯
                    if (ri.getWidth() >= 3 && ri.getHeight() >= 3)
                    {
                        g.setColour (litCol.interpolatedWith (juce::Colours::white, t.cellHighlightAlpha));
                        if (ri.getWidth() >= ri.getHeight())
                            g.fillRect (ri.getX() + 1, ri.getCentreY(), juce::jmax (1, ri.getWidth() - 2), 1);
                        else
                            g.fillRect (ri.getCentreX(), ri.getY() + 1, 1, juce::jmax (1, ri.getHeight() - 2));
                    }
                    if (ri.getHeight() >= 5)
                    {
                        g.setColour (juce::Colours::black.withAlpha (0.10f));
                        for (int py = ri.getY() + 1; py < ri.getBottom(); py += 2)
                            g.fillRect (ri.getX(), py, ri.getWidth(), 1);
                    }
                    return;
                }
            }
        }

        void drawContinuousBar (juce::Graphics& g,
                                juce::Rectangle<float> r,
                                juce::Rectangle<float> fillRect,
                                juce::Colour col,
                                bool vertical,
                                bool drawTrough)
        {
            if (r.isEmpty())
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const auto& t = getSkinTokens();
            if (drawTrough && ! t.isChromaKey)
                drawPanelWell (g, r.toNearestInt(), content.darker (0.04f), WellKind::trough);

            const auto clamped = fillRect.getIntersection (r);
            if (clamped.getWidth() <= 0.25f || clamped.getHeight() <= 0.25f)
                return;

            switch (t.cellFinish)
            {
                case CellFinish::flatPixel:
                {
                    const auto ri = clamped.toNearestInt();
                    if (ri.isEmpty()) return;
                    g.setColour (col);
                    g.fillRect (ri);
                    if (ri.getWidth() >= 3 && ri.getHeight() >= 3)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.28f * col.getFloatAlpha()));
                        if (vertical) g.fillRect (ri.getX(), ri.getY(), 1, ri.getHeight());
                        else          g.fillRect (ri.getX(), ri.getY(), ri.getWidth(), 1);
                    }
                    return;
                }

                case CellFinish::metallicBevel:
                {
                    // 3-Stop 宝石金属连续条：亮顶折痕 + 饱满核心 + 微暗下倒角
                    juce::ColourGradient grad = vertical
                        ? juce::ColourGradient (col.interpolatedWith (juce::Colours::white, 0.44f), clamped.getX(), 0.0f,
                                                col.darker (0.22f), clamped.getRight(), 0.0f, false)
                        : juce::ColourGradient (col.interpolatedWith (juce::Colours::white, 0.44f), 0.0f, clamped.getY(),
                                                col.darker (0.22f), 0.0f, clamped.getBottom(), false);
                    grad.addColour (0.38, col);
                    g.setGradientFill (grad);
                    g.fillRect (clamped);
                    if (clamped.getWidth() >= 3.0f && clamped.getHeight() >= 3.0f)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.38f * col.getFloatAlpha()));
                        if (vertical)
                            g.fillRect (clamped.getX() + 0.5f, clamped.getY(), 1.0f, clamped.getHeight());
                        else
                            g.fillRect (clamped.getX(), clamped.getY() + 0.5f, clamped.getWidth(), 1.0f);
                    }
                    return;
                }

                case CellFinish::cleanRounded:
                {
                    const float rad = juce::jmin (t.barCellRadius + 0.5f,
                                                  juce::jmin (clamped.getWidth(), clamped.getHeight()) * 0.48f);
                    if (! isGreenScreenBackdrop() && clamped.getWidth() >= 4.0f && clamped.getHeight() >= 4.0f)
                    {
                        juce::ColourGradient grad = vertical
                            ? juce::ColourGradient (col.interpolatedWith (juce::Colours::white, 0.09f), clamped.getX(), 0.0f,
                                                    col, clamped.getRight(), 0.0f, false)
                            : juce::ColourGradient (col.interpolatedWith (juce::Colours::white, 0.09f), 0.0f, clamped.getY(),
                                                    col, 0.0f, clamped.getBottom(), false);
                        g.setGradientFill (grad);
                    }
                    else
                    {
                        g.setColour (col);
                    }
                    if (rad > 0.4f) g.fillRoundedRectangle (clamped, rad);
                    else            g.fillRect (clamped);
                    return;
                }

                case CellFinish::glossyCapsule:
                {
                    const float rad = juce::jmin (t.barCellRadius, juce::jmin (clamped.getWidth(), clamped.getHeight()) * 0.5f);
                    const float fa  = col.getFloatAlpha();
                    juce::ColourGradient body = vertical
                        ? juce::ColourGradient (col.interpolatedWith (juce::Colours::white, 0.22f).withAlpha (fa), clamped.getX(), 0.0f,
                                                col.interpolatedWith (juce::Colours::white, 0.24f).withAlpha (fa), clamped.getRight(), 0.0f, false)
                        : juce::ColourGradient (col.interpolatedWith (juce::Colours::white, 0.22f).withAlpha (fa), 0.0f, clamped.getY(),
                                                col.interpolatedWith (juce::Colours::white, 0.24f).withAlpha (fa), 0.0f, clamped.getBottom(), false);
                    body.addColour (0.50, col);
                    g.setGradientFill (body);
                    if (rad > 0.5f) g.fillRoundedRectangle (clamped, rad);
                    else            g.fillRect (clamped);

                    auto sheen = vertical
                                   ? clamped.withWidth (clamped.getWidth() * 0.44f)
                                   : clamped.withHeight (clamped.getHeight() * 0.44f);
                    juce::ColourGradient dome = vertical
                        ? juce::ColourGradient (juce::Colours::white.withAlpha (0.36f * fa), sheen.getX(), 0.0f,
                                                juce::Colours::white.withAlpha (0.04f * fa), sheen.getRight(), 0.0f, false)
                        : juce::ColourGradient (juce::Colours::white.withAlpha (0.36f * fa), 0.0f, sheen.getY(),
                                                juce::Colours::white.withAlpha (0.04f * fa), 0.0f, sheen.getBottom(), false);
                    g.setGradientFill (dome);
                    if (rad > 0.5f) g.fillRoundedRectangle (sheen, juce::jmax (0.5f, rad * 0.72f));
                    else            g.fillRect (sheen);
                    return;
                }

                case CellFinish::neonLit:
                {
                    const float rad = juce::jmin (t.barCellRadius, juce::jmin (clamped.getWidth(), clamped.getHeight()) * 0.42f);
                    if (! isGreenScreenBackdrop())
                    {
                        g.setColour (col.withMultipliedAlpha (0.24f));
                        if (rad > 0.35f) g.fillRoundedRectangle (clamped.expanded (1.0f), rad + 0.75f);
                        else             g.fillRect (clamped.expanded (1.0f));
                    }
                    g.setColour (col);
                    if (rad > 0.35f) g.fillRoundedRectangle (clamped, rad);
                    else             g.fillRect (clamped);
                    g.setColour (col.interpolatedWith (juce::Colours::white, 0.56f));
                    if (vertical)
                        g.fillRect (clamped.getCentreX() - 0.6f, clamped.getY(), 1.2f, clamped.getHeight());
                    else
                        g.fillRect (clamped.getX(), clamped.getCentreY() - 0.6f, clamped.getWidth(), 1.2f);
                    return;
                }

                case CellFinish::phosphorBar:
                {
                    const auto ri = clamped.toNearestInt();
                    if (ri.isEmpty()) return;
                    if (! isGreenScreenBackdrop())
                    {
                        g.setColour (col.withMultipliedAlpha (0.18f));
                        g.fillRect (ri.expanded (1));
                    }
                    g.setColour (col);
                    g.fillRect (ri);
                    if (ri.getWidth() >= 3 && ri.getHeight() >= 3)
                    {
                        g.setColour (col.interpolatedWith (juce::Colours::white, 0.32f));
                        if (vertical) g.fillRect (ri.getCentreX(), ri.getY(), 1, ri.getHeight());
                        else          g.fillRect (ri.getX(), ri.getCentreY(), ri.getWidth(), 1);
                    }
                    if (ri.getHeight() >= 5)
                    {
                        g.setColour (juce::Colours::black.withAlpha (0.10f));
                        for (int py = ri.getY() + 1; py < ri.getBottom(); py += 2)
                            g.fillRect (ri.getX(), py, ri.getWidth(), 1);
                    }
                    return;
                }
            }
        }

        void drawReadoutCard (juce::Graphics& g,
                              juce::Rectangle<int> r,
                              juce::Colour fill)
        {
            drawPanelWell (g, r, fill, WellKind::card);
        }

        void drawBadgePill (juce::Graphics& g,
                            juce::Rectangle<int> r,
                            juce::Colour fill,
                            juce::Colour borderCol)
        {
            if (r.isEmpty())
                return;

            const auto& t = getSkinTokens();
            if (t.isChromaKey)
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const bool hasBorder = ! borderCol.isTransparent();

            switch (t.frameStyle)
            {
                case FrameStyle::pixelBevel:
                {
                    // 0px 锐利像素徽章标签（绝无现代圆角胶囊）
                    g.setColour (fill);
                    g.fillRect (r);
                    if (r.getWidth() >= 4 && r.getHeight() >= 4)
                    {
                        g.setColour (hl.withAlpha (0.45f));
                        g.fillRect (r.getX() + 1, r.getY() + 1, r.getWidth() - 2, 1);
                    }
                    fillRectFrame1Px (g, r, hasBorder ? borderCol : dark.withAlpha (0.75f));
                    return;
                }

                case FrameStyle::machinedChrome:
                {
                    const bool isDark = fill.getPerceivedBrightness() < 0.45f;
                    const float fa = fill.getFloatAlpha();
                    juce::ColourGradient grad (fill.interpolatedWith (juce::Colours::white, isDark ? 0.14f : 0.36f).withAlpha (fa),
                                               0.0f, (float) r.getY(),
                                               fill.interpolatedWith (juce::Colours::black, isDark ? 0.12f : 0.08f).withAlpha (fa),
                                               0.0f, (float) r.getBottom(), false);
                    grad.addColour (0.38, fill.interpolatedWith (juce::Colours::white, isDark ? 0.20f : 0.22f).withAlpha (fa));
                    grad.addColour (0.42, fill.interpolatedWith (juce::Colours::black, isDark ? 0.08f : 0.06f).withAlpha (fa));
                    g.setGradientFill (grad);
                    g.fillRect (r);
                    fillRectFrame1Px (g, r, hasBorder ? borderCol
                                                      : (isDark ? fill.withAlpha (1.0f).interpolatedWith (juce::Colours::black, 0.68f)
                                                                : dark.withAlpha (0.85f)));
                    if (r.getWidth() >= 4 && r.getHeight() >= 4)
                    {
                        g.setColour (juce::Colours::white.withAlpha (isDark ? 0.24f : 0.58f));
                        g.fillRect (r.getX() + 1, r.getY() + 1, r.getWidth() - 2, 1);
                        g.fillRect (r.getX() + 1, r.getY() + 1, 1, r.getHeight() - 2);
                    }
                    return;
                }

                case FrameStyle::modernStudio:
                {
                    const auto rf   = r.toFloat();
                    const float rad = juce::jmin (t.badgeRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.5f);
                    const bool darkFill = fill.getPerceivedBrightness() < 0.45f;
                    juce::ColourGradient grad (fill.interpolatedWith (juce::Colours::white, darkFill ? 0.04f : 0.06f)
                                                   .withAlpha (fill.getFloatAlpha()),
                                               0.0f, rf.getY(),
                                               fill, 0.0f, rf.getBottom(), false);
                    g.setGradientFill (grad);
                    g.fillRoundedRectangle (rf, rad);
                    const auto edge = hasBorder
                                        ? borderCol
                                        : (darkFill ? fill.withAlpha (1.0f).interpolatedWith (juce::Colours::white, 0.16f)
                                                    : fill.withAlpha (1.0f).interpolatedWith (juce::Colours::black, 0.16f));
                    g.setColour (edge);
                    g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                    return;
                }

                case FrameStyle::frostedGlass:
                {
                    const auto rf = r.toFloat();
                    const float rad = juce::jmin (t.badgeRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.5f);
                    drawGlassSurface (g, r, fill, rad, false, false, false, false);
                    if (hasBorder)
                    {
                        g.setColour (borderCol);
                        g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                    }
                    return;
                }

                case FrameStyle::neonTube:
                {
                    const auto rf     = r.toFloat();
                    const float rad   = juce::jmin (t.badgeRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.35f);
                    const bool isDark = fill.getPerceivedBrightness() < 0.45f;
                    const auto edge   = hasBorder ? borderCol : sel;
                    g.setColour (fill.interpolatedWith (juce::Colours::black, isDark ? 0.24f : 0.08f)
                                     .withAlpha (fill.getFloatAlpha()));
                    g.fillRoundedRectangle (rf, rad);
                    g.setColour (edge.withAlpha (0.34f));
                    g.drawRoundedRectangle (rf.reduced (0.4f), rad, 1.3f);
                    g.setColour (edge.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.90f));
                    g.drawRoundedRectangle (rf.reduced (1.0f), juce::jmax (0.5f, rad - 0.5f), 0.85f);
                    return;
                }

                case FrameStyle::crtPhosphor:
                {
                    const auto edge = hasBorder ? borderCol : sel.withAlpha (0.80f);
                    g.setColour (fill);
                    g.fillRect (r);
                    fillRectFrame1Px (g, r, edge);
                    if (r.getWidth() >= 12 && r.getHeight() >= 10)
                        drawPixelCorners (g, r, edge.interpolatedWith (juce::Colours::white, 0.25f), 3, 1);
                    return;
                }

                case FrameStyle::chromaNone:
                default:
                    return;
            }
        }

        void drawStatusLed (juce::Graphics& g,
                            juce::Rectangle<float> bounds,
                            juce::Colour col,
                            bool lit,
                            bool useGlow)
        {
            if (bounds.getWidth() < 2.0f || bounds.getHeight() < 2.0f)
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const auto& t = getSkinTokens();
            if (t.isChromaKey)
            {
                if (! lit)
                    return;
                // 绿幕 / 透明背景：完整继承所属视觉风格的 LED 几何（方形像素 LED vs 圆形 LED），
                //   纯绿幕下跳过外辉光以免污染抠像边缘；透明背景允许真实 alpha 辉光
                if (! t.statusDotCircular)
                {
                    const auto ri = bounds.toNearestInt();
                    if (ri.isEmpty())
                        return;
                    if (t.frameStyle == FrameStyle::crtPhosphor)
                    {
                        if (useGlow && ! isGreenScreenBackdrop())
                        {
                            g.setColour (col.withAlpha (0.24f));
                            g.fillRect (ri.expanded (1));
                        }
                        g.setColour (col);
                        g.fillRect (ri);
                        fillRectFrame1Px (g, ri, sel.withAlpha (0.90f));
                        if (ri.getWidth() >= 5 && ri.getHeight() >= 5)
                        {
                            g.setColour (col.interpolatedWith (juce::Colours::white, 0.50f));
                            g.fillRect (ri.getCentreX(), ri.getCentreY(), 1, 1);
                        }
                        return;
                    }
                    g.setColour (col);
                    g.fillRect (ri);
                    fillRectFrame1Px (g, ri, dark.withAlpha (0.72f));
                    if (ri.getWidth() >= 4 && ri.getHeight() >= 4)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.70f));
                        g.fillRect (ri.getX() + 1, ri.getY() + 1, 1, 1);
                    }
                }
                else
                {
                    const float dia = juce::jmin (bounds.getWidth(), bounds.getHeight());
                    const juce::Rectangle<float> circ (bounds.getCentreX() - dia * 0.5f,
                                                       bounds.getCentreY() - dia * 0.5f,
                                                       dia, dia);
                    if (t.frameStyle == FrameStyle::machinedChrome && dia >= 5.0f)
                    {
                        juce::ColourGradient bezel (juce::Colours::white.withAlpha (0.72f), circ.getX(), circ.getY(),
                                                    dark.withAlpha (0.90f), circ.getRight(), circ.getBottom(), false);
                        g.setGradientFill (bezel);
                        g.fillEllipse (circ);
                        const auto jewel = circ.reduced (juce::jlimit (1.0f, 1.6f, dia * 0.16f));
                        g.setColour (col);
                        g.fillEllipse (jewel);
                        g.setColour (juce::Colours::white.withAlpha (0.80f));
                        g.fillEllipse (jewel.getX() + jewel.getWidth() * 0.16f,
                                       jewel.getY() + jewel.getHeight() * 0.14f,
                                       juce::jmax (1.4f, jewel.getWidth() * 0.36f),
                                       juce::jmax (1.2f, jewel.getHeight() * 0.24f));
                        return;
                    }
                    if (t.frameStyle == FrameStyle::frostedGlass)
                    {
                        if (useGlow && ! isGreenScreenBackdrop())
                        {
                            g.setColour (col.withAlpha (0.24f));
                            g.fillEllipse (circ.expanded (dia * 0.30f));
                        }
                        juce::ColourGradient bead (col.darker (0.14f), 0.0f, circ.getY(),
                                                   col.interpolatedWith (juce::Colours::white, 0.45f), 0.0f, circ.getBottom(), false);
                        bead.addColour (0.50, col);
                        g.setGradientFill (bead);
                        g.fillEllipse (circ);
                        if (dia >= 5.0f)
                        {
                            const auto dome = juce::Rectangle<float> (circ.getX() + dia * 0.18f,
                                                                      circ.getY() + dia * 0.10f,
                                                                      dia * 0.64f, dia * 0.38f);
                            juce::ColourGradient dg (juce::Colours::white.withAlpha (0.84f), 0.0f, dome.getY(),
                                                     juce::Colours::white.withAlpha (0.08f), 0.0f, dome.getBottom(), false);
                            g.setGradientFill (dg);
                            g.fillEllipse (dome);
                        }
                        g.setColour (col.darker (0.32f).withAlpha (0.65f));
                        g.drawEllipse (circ.reduced (0.4f), 0.8f);
                        return;
                    }
                    if (t.frameStyle == FrameStyle::neonTube)
                    {
                        if (useGlow && ! isGreenScreenBackdrop())
                        {
                            g.setColour (col.withAlpha (0.34f));
                            g.fillEllipse (circ.expanded (dia * 0.32f));
                            g.setColour (col.withAlpha (0.15f));
                            g.fillEllipse (circ.expanded (dia * 0.58f));
                        }
                        g.setColour (col.interpolatedWith (juce::Colours::black, 0.35f));
                        g.fillEllipse (circ);
                        g.setColour (col);
                        g.drawEllipse (circ.reduced (0.6f), 1.2f);
                        const float coreD = juce::jmax (2.0f, dia * 0.48f);
                        g.setColour (col.interpolatedWith (juce::Colours::white, 0.62f));
                        g.fillEllipse (circ.getCentreX() - coreD * 0.5f, circ.getCentreY() - coreD * 0.5f, coreD, coreD);
                        return;
                    }
                    g.setColour (col);
                    g.fillEllipse (circ);
                    if (dia >= 5.0f)
                    {
                        const float spec = juce::jmax (1.4f, dia * 0.26f);
                        g.setColour (juce::Colours::white.withAlpha (0.50f));
                        g.fillEllipse (circ.getX() + dia * 0.22f, circ.getY() + dia * 0.18f, spec, spec);
                    }
                }
                return;
            }

            if (! t.statusDotCircular)
            {
                // Pixel XP & CRT：整数对齐锐利方形像素 LED（带 1px 左上反光点 / 磷光核心）
                const auto ri = bounds.toNearestInt();
                if (ri.isEmpty())
                    return;

                if (t.frameStyle == FrameStyle::crtPhosphor)
                {
                    if (lit && useGlow && t.statusDotGlow)
                    {
                        g.setColour (col.withAlpha (0.26f));
                        g.fillRect (ri.expanded (1));
                    }
                    g.setColour (lit ? col : col.withAlpha (0.14f));
                    g.fillRect (ri);
                    fillRectFrame1Px (g, ri, sel.withAlpha (lit ? 0.90f : 0.45f));
                    if (lit && ri.getWidth() >= 5 && ri.getHeight() >= 5)
                    {
                        g.setColour (col.interpolatedWith (juce::Colours::white, 0.48f));
                        g.fillRect (ri.getCentreX() - 1, ri.getCentreY() - 1, 2, 2);
                    }
                    return;
                }

                // Pixel XP：硬边凹框 + 方块像素 LED + 1px 白色高光点
                g.setColour (lit ? col : col.withAlpha (0.12f));
                g.fillRect (ri);
                fillRectFrame1Px (g, ri, dark.withAlpha (0.72f));
                if (lit && ri.getWidth() >= 4 && ri.getHeight() >= 4)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.70f));
                    g.fillRect (ri.getX() + 1, ri.getY() + 1, 1, 1);
                }
                return;
            }

            // 圆形 LED（Chrome / Modern / Glass / Neon）
            const float dia = juce::jmin (bounds.getWidth(), bounds.getHeight());
            const juce::Rectangle<float> circ (bounds.getCentreX() - dia * 0.5f,
                                               bounds.getCentreY() - dia * 0.5f,
                                               dia, dia);

            if (t.frameStyle == FrameStyle::machinedChrome)
            {
                // CNC 车削铬环 + 宝石透镜 LED + 左上镜面月牙高光
                if (lit && useGlow && t.statusDotGlow)
                {
                    g.setColour (col.withAlpha (0.24f));
                    g.fillEllipse (circ.expanded (dia * 0.30f));
                }

                // 外层车削金属斜边环（上左亮、下右暗）
                juce::ColourGradient bezel (juce::Colours::white.withAlpha (0.72f), circ.getX(), circ.getY(),
                                            dark.withAlpha (0.90f), circ.getRight(), circ.getBottom(), false);
                bezel.addColour (0.48, face.interpolatedWith (juce::Colours::white, 0.18f));
                g.setGradientFill (bezel);
                g.fillEllipse (circ);

                const float rimW = juce::jlimit (1.0f, 1.8f, dia * 0.16f);
                const auto jewel = circ.reduced (rimW);
                if (jewel.getWidth() >= 1.5f)
                {
                    if (lit)
                    {
                        juce::ColourGradient jg (col.brighter (0.22f), jewel.getX(), jewel.getY(),
                                                 col.darker (0.26f), jewel.getRight(), jewel.getBottom(), false);
                        g.setGradientFill (jg);
                        g.fillEllipse (jewel);
                        // 左上锐利宝石月牙反光
                        if (jewel.getWidth() >= 4.0f)
                        {
                            const float specW = juce::jmax (1.5f, jewel.getWidth() * 0.38f);
                            const float specH = juce::jmax (1.2f, jewel.getHeight() * 0.24f);
                            g.setColour (juce::Colours::white.withAlpha (0.82f));
                            g.fillEllipse (jewel.getX() + jewel.getWidth() * 0.16f,
                                           jewel.getY() + jewel.getHeight() * 0.14f,
                                           specW, specH);
                        }
                    }
                    else
                    {
                        g.setColour (dark.interpolatedWith (col, 0.16f).withAlpha (0.78f));
                        g.fillEllipse (jewel);
                    }
                }
                g.setColour (dark.withAlpha (0.72f));
                g.drawEllipse (circ.reduced (0.4f), 0.8f);
                return;
            }

            if (t.frameStyle == FrameStyle::frostedGlass)
            {
                // 水晶玻璃珠 LED：柔和折射晕 + 底部亮肚折射 + 顶部弧形玻璃穹顶高光
                if (lit && useGlow && t.statusDotGlow)
                {
                    g.setColour (col.withAlpha (0.24f));
                    g.fillEllipse (circ.expanded (dia * 0.32f));
                }

                if (lit)
                {
                    juce::ColourGradient bead (col.darker (0.16f), 0.0f, circ.getY(),
                                               col.interpolatedWith (juce::Colours::white, 0.46f), 0.0f, circ.getBottom(), false);
                    bead.addColour (0.50, col);
                    g.setGradientFill (bead);
                    g.fillEllipse (circ);

                    if (dia >= 5.0f)
                    {
                        const auto dome = juce::Rectangle<float> (circ.getX() + dia * 0.17f,
                                                                  circ.getY() + dia * 0.10f,
                                                                  dia * 0.66f, dia * 0.38f);
                        juce::ColourGradient dg (juce::Colours::white.withAlpha (0.86f), 0.0f, dome.getY(),
                                                 juce::Colours::white.withAlpha (0.08f), 0.0f, dome.getBottom(), false);
                        g.setGradientFill (dg);
                        g.fillEllipse (dome);
                    }
                    g.setColour (col.darker (0.34f).withAlpha (0.65f));
                    g.drawEllipse (circ.reduced (0.4f), 0.85f);
                }
                else
                {
                    g.setColour (dark.interpolatedWith (col, 0.14f).withAlpha (0.44f));
                    g.fillEllipse (circ);
                    if (dia >= 5.0f)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.24f));
                        g.fillEllipse (circ.getX() + dia * 0.22f, circ.getY() + dia * 0.14f, dia * 0.52f, dia * 0.28f);
                    }
                    g.setColour (juce::Colours::white.withAlpha (0.32f));
                    g.drawEllipse (circ.reduced (0.4f), 0.8f);
                }
                return;
            }

            if (t.frameStyle == FrameStyle::neonTube)
            {
                // 霓虹双环发光灯：暗井灯座 + 发光外环 + 炽热白芯核心点
                if (lit && useGlow && t.statusDotGlow)
                {
                    g.setColour (col.withAlpha (0.34f));
                    g.fillEllipse (circ.expanded (dia * 0.34f));
                    g.setColour (col.withAlpha (0.15f));
                    g.fillEllipse (circ.expanded (dia * 0.62f));
                }

                g.setColour (lit ? col.interpolatedWith (juce::Colours::black, 0.32f)
                                 : juce::Colours::black.withAlpha (0.48f));
                g.fillEllipse (circ);
                g.setColour (lit ? col : col.withAlpha (0.34f));
                g.drawEllipse (circ.reduced (0.6f), lit ? 1.25f : 0.9f);

                if (lit)
                {
                    const float coreD = juce::jmax (2.0f, dia * 0.48f);
                    g.setColour (col.interpolatedWith (juce::Colours::white, 0.62f));
                    g.fillEllipse (circ.getCentreX() - coreD * 0.5f, circ.getCentreY() - coreD * 0.5f, coreD, coreD);
                }
                return;
            }

            if (lit && useGlow && (t.statusDotGlow || t.frameStyle == FrameStyle::modernStudio))
            {
                const float glowAlpha = (t.frameStyle == FrameStyle::neonTube)     ? 0.38f
                                      : (t.frameStyle == FrameStyle::modernStudio) ? 0.20f
                                                                                   : 0.22f;
                const float expand    = (t.frameStyle == FrameStyle::modernStudio) ? (dia * 0.25f) : (dia * 0.35f);
                g.setColour (col.withAlpha (glowAlpha));
                g.fillEllipse (circ.expanded (expand));
                if (t.frameStyle == FrameStyle::neonTube)
                {
                    g.setColour (col.withAlpha (0.18f));
                    g.fillEllipse (circ.expanded (dia * 0.65f));
                }
            }

            if (t.frameStyle == FrameStyle::modernStudio && ! lit)
            {
                const bool isDark = content.getPerceivedBrightness() < 0.45f;
                g.setColour (isDark ? juce::Colours::black.withAlpha (0.38f) : content.darker (0.14f));
                g.fillEllipse (circ);
                g.setColour (isDark ? juce::Colours::white.withAlpha (0.14f) : juce::Colours::black.withAlpha (0.16f));
                g.drawEllipse (circ.reduced (0.5f), 0.9f);
                return;
            }

            g.setColour (lit ? col : col.withAlpha (0.16f));
            g.fillEllipse (circ);

            if (t.frameStyle == FrameStyle::machinedChrome)
            {
                g.setColour (dark.withAlpha (0.65f));
                g.drawEllipse (circ.reduced (0.5f), 1.0f);
            }
            else if (t.frameStyle == FrameStyle::modernStudio)
            {
                g.setColour (col.darker (0.28f).withAlpha (0.55f));
                g.drawEllipse (circ.reduced (0.5f), 0.85f);
            }

            if (lit && dia >= 5.0f)
            {
                const float spec = juce::jmax (1.4f, dia * 0.26f);
                g.setColour (juce::Colours::white.withAlpha (t.frameStyle == FrameStyle::frostedGlass ? 0.78f : 0.48f));
                g.fillEllipse (circ.getX() + dia * 0.22f, circ.getY() + dia * 0.18f, spec, spec);
            }
        }

        void drawGridLine (juce::Graphics& g,
                           float x0, float y0,
                           float x1, float y1,
                           bool major,
                           juce::Colour col)
        {
            const auto& t = getSkinTokens();
            if (t.isChromaKey || t.gridStyle == GridStyle::none)
                return;

            const float alpha = col.getFloatAlpha() * (major ? t.gridMajorAlpha : t.gridMinorAlpha);
            if (alpha <= 0.002f)
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const auto c = col.withAlpha (alpha);
            g.setColour (c);

            switch (t.gridStyle)
            {
                case GridStyle::dottedPixel:
                {
                    // 整数像素点阵线：主线 2on/2off，次线 1on/3off
                    const int ix0 = juce::roundToInt (x0);
                    const int iy0 = juce::roundToInt (y0);
                    const int ix1 = juce::roundToInt (x1);
                    const int iy1 = juce::roundToInt (y1);
                    const int onLen  = major ? 2 : 1;
                    const int period = 4;

                    if (iy0 == iy1)
                    {
                        const int a = juce::jmin (ix0, ix1);
                        const int b = juce::jmax (ix0, ix1);
                        for (int x = a; x <= b; x += period)
                            g.fillRect (x, iy0, juce::jmin (onLen, b - x + 1), 1);
                        return;
                    }
                    if (ix0 == ix1)
                    {
                        const int a = juce::jmin (iy0, iy1);
                        const int b = juce::jmax (iy0, iy1);
                        for (int y = a; y <= b; y += period)
                            g.fillRect (ix0, y, 1, juce::jmin (onLen, b - y + 1));
                        return;
                    }

                    // 任意斜线：整数 Bresenham 点阵
                    int x = ix0, y = iy0;
                    const int dx = std::abs (ix1 - ix0);
                    const int dy = std::abs (iy1 - iy0);
                    const int sx = (ix0 < ix1) ? 1 : -1;
                    const int sy = (iy0 < iy1) ? 1 : -1;
                    int err = dx - dy;
                    int step = 0;
                    for (;;)
                    {
                        if ((step % period) < onLen)
                            g.fillRect (x, y, 1, 1);
                        if (x == ix1 && y == iy1)
                            break;
                        const int e2 = err * 2;
                        if (e2 > -dy) { err -= dy; x += sx; }
                        if (e2 <  dx) { err += dx; y += sy; }
                        ++step;
                    }
                    return;
                }

                case GridStyle::dashedTech:
                {
                    // 霓虹激光虚线网格：严格 4px on / 4px off（与 GL 侧 100% 同步）
                    const float dashes[2] = { 4.0f, 4.0f };
                    g.drawDashedLine (juce::Line<float> (x0, y0, x1, y1), dashes, 2, major ? 1.0f : 0.85f);
                    return;
                }

                case GridStyle::phosphorCrosshair:
                {
                    const int ix0 = juce::roundToInt (x0);
                    const int iy0 = juce::roundToInt (y0);
                    const int ix1 = juce::roundToInt (x1);
                    const int iy1 = juce::roundToInt (y1);
                    if (major)
                    {
                        // 1982 Tektronix 示波器磷光主轴 + 垂直微刻度标尺（每 16px 一道 3px 刻度）
                        const auto tickCol = col.withAlpha (juce::jmin (1.0f, alpha * 1.25f));
                        if (iy0 == iy1)
                        {
                            const int a = juce::jmin (ix0, ix1);
                            const int b = juce::jmax (ix0, ix1);
                            g.fillRect (a, iy0, b - a + 1, 1);
                            g.setColour (tickCol);
                            for (int tx = a + 16; tx <= b - 8; tx += 16)
                                g.fillRect (tx, iy0 - 1, 1, 3);
                        }
                        else if (ix0 == ix1)
                        {
                            const int a = juce::jmin (iy0, iy1);
                            const int b = juce::jmax (iy0, iy1);
                            g.fillRect (ix0, a, 1, b - a + 1);
                            g.setColour (tickCol);
                            for (int ty = a + 16; ty <= b - 8; ty += 16)
                                g.fillRect (ix0 - 1, ty, 3, 1);
                        }
                        else
                        {
                            g.drawLine (x0, y0, x1, y1, 1.0f);
                        }
                    }
                    else
                    {
                        const float dashes[2] = { 2.0f, 4.0f };
                        g.drawDashedLine (juce::Line<float> ((float) ix0 + 0.5f, (float) iy0 + 0.5f,
                                                             (float) ix1 + 0.5f, (float) iy1 + 0.5f),
                                          dashes, 2, 0.9f);
                    }
                    return;
                }

                case GridStyle::solidHairline:
                default:
                    g.drawLine (x0, y0, x1, y1, major ? 1.0f : 0.75f);
                    return;
            }
        }

        void drawStyledStroke (juce::Graphics& g,
                               const juce::Path& path,
                               juce::Colour col,
                               float width,
                               juce::PathStrokeType::JointStyle joint,
                               juce::PathStrokeType::EndCapStyle cap)
        {
            if (path.isEmpty() || width <= 0.05f || col.isTransparent())
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const auto& t = getSkinTokens();
            const float scaledW = juce::jmax (0.5f, width * t.strokeWidthScale);

            if (t.strokeGlowPass && ! isGreenScreenBackdrop())
            {
                g.setColour (col.withMultipliedAlpha (t.strokeGlowAlpha));
                g.strokePath (path, juce::PathStrokeType (scaledW * t.strokeGlowWidthMul,
                                                          juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            }

            // Restrained Materiality：所有风格的曲线描边保持干净抗锯齿（Pixel XP 不强加 mitered 尖角锯齿）
            g.setColour (col);
            g.strokePath (path, juce::PathStrokeType (scaledW, joint, cap));

            if ((t.frameStyle == FrameStyle::neonTube || t.frameStyle == FrameStyle::crtPhosphor) && scaledW >= 1.8f)
            {
                const float mix = (t.frameStyle == FrameStyle::neonTube) ? 0.45f : 0.30f;
                g.setColour (col.interpolatedWith (juce::Colours::white, mix).withMultipliedAlpha (0.72f));
                g.strokePath (path, juce::PathStrokeType (juce::jmax (0.75f, scaledW * 0.38f), joint, cap));
            }
        }

        // ----------------------------------------------------------
        // 像素级几何原语（Bresenham 线 / 像素粗线 / 扫描线像素圆弧 / 圆盘 / Bayer 抖动）
        // ----------------------------------------------------------
        void drawPixelLine (juce::Graphics& g,
                            int x0, int y0,
                            int x1, int y1,
                            juce::Colour col,
                            int thickness)
        {
            if (thickness <= 0 || col.isTransparent())
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            g.setColour (col);
            const int half = thickness / 2;

            if (x0 == x1)
            {
                const int top = juce::jmin (y0, y1);
                const int len = std::abs (y1 - y0) + 1;
                g.fillRect (x0 - half, top - half, thickness, len + thickness - 1);
                return;
            }
            if (y0 == y1)
            {
                const int left = juce::jmin (x0, x1);
                const int len  = std::abs (x1 - x0) + 1;
                g.fillRect (left - half, y0 - half, len + thickness - 1, thickness);
                return;
            }

            const int dx = std::abs (x1 - x0);
            const int dy = std::abs (y1 - y0);
            const int sx = (x0 < x1) ? 1 : -1;
            const int sy = (y0 < y1) ? 1 : -1;

            if (dx >= dy)
            {
                // 主轴 X：合并同一 y 行的连续像素为一个 fillRect
                int x = x0, y = y0;
                int err = dx / 2;
                int runX0 = x, runX1 = x, runY = y;

                auto flushRow = [&]()
                {
                    const int left = juce::jmin (runX0, runX1);
                    const int span = std::abs (runX1 - runX0) + 1;
                    g.fillRect (left - half, runY - half, span + thickness - 1, thickness);
                };

                for (int i = 0; i < dx; ++i)
                {
                    x += sx;
                    err -= dy;
                    if (err < 0)
                    {
                        flushRow();
                        y += sy;
                        err += dx;
                        runX0 = runX1 = x;
                        runY = y;
                    }
                    else
                    {
                        runX1 = x;
                    }
                }
                flushRow();
            }
            else
            {
                // 主轴 Y：合并同一 x 列的连续像素为一个 fillRect
                int x = x0, y = y0;
                int err = dy / 2;
                int runY0 = y, runY1 = y, runX = x;

                auto flushCol = [&]()
                {
                    const int top  = juce::jmin (runY0, runY1);
                    const int span = std::abs (runY1 - runY0) + 1;
                    g.fillRect (runX - half, top - half, thickness, span + thickness - 1);
                };

                for (int i = 0; i < dy; ++i)
                {
                    y += sy;
                    err -= dx;
                    if (err < 0)
                    {
                        flushCol();
                        x += sx;
                        err += dy;
                        runY0 = runY1 = y;
                        runX = x;
                    }
                    else
                    {
                        runY1 = y;
                    }
                }
                flushCol();
            }
        }

        void drawPixelThickLine (juce::Graphics& g,
                                 float x0, float y0,
                                 float x1, float y1,
                                 juce::Colour col,
                                 int thickness,
                                 bool hardShadow)
        {
            const int ix0 = juce::roundToInt (x0);
            const int iy0 = juce::roundToInt (y0);
            const int ix1 = juce::roundToInt (x1);
            const int iy1 = juce::roundToInt (y1);

            if (hardShadow)
                drawPixelLine (g, ix0 + 1, iy0 + 1, ix1 + 1, iy1 + 1,
                               dark.withAlpha (0.72f * col.getFloatAlpha()), thickness);

            drawPixelLine (g, ix0, iy0, ix1, iy1, col, thickness);
        }

        void drawPixelArc (juce::Graphics& g,
                           float cx, float cy,
                           float radius,
                           float startAngleRad,
                           float endAngleRad,
                           juce::Colour col,
                           int thickness,
                           int stepPx)
        {
            if (radius < 0.5f || thickness <= 0 || col.isTransparent())
                return;
            if (! std::isfinite (cx) || ! std::isfinite (cy) || ! std::isfinite (radius)
                || ! std::isfinite (startAngleRad) || ! std::isfinite (endAngleRad))
                return;

            float a0 = startAngleRad;
            float a1 = endAngleRad;
            if (a1 < a0)
                std::swap (a0, a1);

            const float span = a1 - a0;
            if (span < 1.0e-5f)
                return;

            constexpr float kTwoPi = juce::MathConstants<float>::twoPi;
            juce::Graphics::ScopedSaveState savedState (g);
            g.setColour (col);

            // 步进刻度弧模式（stepPx > 1）：沿弧长每隔 stepPx 像素落一个锐利整数方块
            if (stepPx > 1)
            {
                const float arcLen = radius * span;
                const int steps = juce::jmax (1, (int) std::floor (arcLen / (float) stepPx));
                const int half  = thickness / 2;
                for (int i = 0; i <= steps; ++i)
                {
                    const float t   = (float) i / (float) steps;
                    const float ang = a0 + span * t;
                    const int px = juce::roundToInt (cx + radius * std::sin (ang));
                    const int py = juce::roundToInt (cy - radius * std::cos (ang));
                    g.fillRect (px - half, py - half, thickness, thickness);
                }
                return;
            }

            // 连续整数扫描线环形扇区光栅化：零断缝、零重复混合、全角度等宽
            const bool fullCircle = (span >= kTwoPi - 1.0e-4f);
            const float rIn  = juce::jmax (0.0f, radius - (float) thickness * 0.5f);
            const float rOut = radius + (float) thickness * 0.5f;
            const float rIn2  = rIn * rIn;
            const float rOut2 = rOut * rOut;

            float a0Norm = std::fmod (a0, kTwoPi);
            if (a0Norm < 0.0f)
                a0Norm += kTwoPi;

            const int yMin = (int) std::floor (cy - rOut);
            const int yMax = (int) std::ceil  (cy + rOut);

            auto scanSpan = [&] (int py, int xFrom, int xTo, float dy, float dy2)
            {
                int runStart = 0;
                bool inRun = false;

                for (int px = xFrom; px <= xTo; ++px)
                {
                    const float dx = (float) px - cx;
                    const float d2 = dx * dx + dy2;
                    bool hit = (d2 <= rOut2) && (rIn <= 0.0f || d2 >= rIn2);
                    if (hit && ! fullCircle)
                    {
                        float ang = std::atan2 (dx, -dy);
                        if (ang < 0.0f)
                            ang += kTwoPi;
                        float rel = ang - a0Norm;
                        if (rel < 0.0f)
                            rel += kTwoPi;
                        hit = (rel <= span + 1.0e-4f);
                    }

                    if (hit)
                    {
                        if (! inRun) { runStart = px; inRun = true; }
                    }
                    else if (inRun)
                    {
                        g.fillRect (runStart, py, px - runStart, 1);
                        inRun = false;
                    }
                }

                if (inRun)
                    g.fillRect (runStart, py, xTo - runStart + 1, 1);
            };

            for (int py = yMin; py <= yMax; ++py)
            {
                const float dy  = (float) py - cy;
                const float dy2 = dy * dy;
                if (dy2 > rOut2)
                    continue;

                const float maxDx = std::sqrt (juce::jmax (0.0f, rOut2 - dy2));
                const int xMin = (int) std::floor (cx - maxDx);
                const int xMax = (int) std::ceil  (cx + maxDx);

                if (rIn > 0.0f && dy2 < rIn2)
                {
                    const float minDx = std::sqrt (rIn2 - dy2);
                    const int leftEnd    = juce::jmin (xMax, (int) std::ceil  (cx - minDx));
                    const int rightStart = juce::jmax (leftEnd + 1, (int) std::floor (cx + minDx));
                    if (xMin <= leftEnd)
                        scanSpan (py, xMin, leftEnd, dy, dy2);
                    if (rightStart <= xMax)
                        scanSpan (py, rightStart, xMax, dy, dy2);
                }
                else
                {
                    scanSpan (py, xMin, xMax, dy, dy2);
                }
            }
        }

        void drawPixelDisc (juce::Graphics& g,
                            int cx, int cy,
                            int radius,
                            juce::Colour fill,
                            juce::Colour border)
        {
            if (radius < 0)
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            if (radius == 0)
            {
                g.setColour (! fill.isTransparent() ? fill : border);
                g.fillRect (cx, cy, 1, 1);
                return;
            }

            const bool hasBorder = ! border.isTransparent();
            const int outerR2 = radius * radius + radius;
            const int innerR  = juce::jmax (0, radius - 1);
            const int innerR2 = hasBorder ? (innerR * innerR + innerR) : outerR2;

            for (int dy = -radius; dy <= radius; ++dy)
            {
                const int dy2 = dy * dy;
                if (dy2 > outerR2)
                    continue;

                const int outerDx = (int) std::floor (std::sqrt ((float) (outerR2 - dy2)));
                if (! hasBorder || radius <= 1)
                {
                    if (! fill.isTransparent())
                    {
                        g.setColour (fill);
                        g.fillRect (cx - outerDx, cy + dy, outerDx * 2 + 1, 1);
                    }
                    if (hasBorder && radius == 1)
                    {
                        g.setColour (border);
                        if (dy == -1 || dy == 1)
                            g.fillRect (cx - outerDx, cy + dy, outerDx * 2 + 1, 1);
                        else
                        {
                            g.fillRect (cx - outerDx, cy + dy, 1, 1);
                            g.fillRect (cx + outerDx, cy + dy, 1, 1);
                        }
                    }
                    continue;
                }

                if (dy2 > innerR2)
                {
                    g.setColour (border);
                    g.fillRect (cx - outerDx, cy + dy, outerDx * 2 + 1, 1);
                }
                else
                {
                    const int innerDx = (int) std::floor (std::sqrt ((float) (innerR2 - dy2)));
                    if (! fill.isTransparent())
                    {
                        g.setColour (fill);
                        g.fillRect (cx - innerDx, cy + dy, innerDx * 2 + 1, 1);
                    }
                    const int leftW = outerDx - innerDx;
                    g.setColour (border);
                    if (leftW > 0)
                    {
                        g.fillRect (cx - outerDx, cy + dy, leftW, 1);
                        g.fillRect (cx + innerDx + 1, cy + dy, leftW, 1);
                    }
                    else
                    {
                        g.fillRect (cx - outerDx, cy + dy, 1, 1);
                        g.fillRect (cx + outerDx, cy + dy, 1, 1);
                    }
                }
            }
        }

        void drawPixelDitherBand (juce::Graphics& g,
                                  juce::Rectangle<int> r,
                                  juce::Colour colA,
                                  juce::Colour colB,
                                  int bands)
        {
            if (r.isEmpty())
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            const int numBands = juce::jlimit (1, juce::jmax (1, r.getHeight()), bands);
            if (numBands == 1)
            {
                g.setColour (colA);
                g.fillRect (r);
                return;
            }

            for (int i = 0; i < numBands; ++i)
            {
                const int y0 = r.getY() + (r.getHeight() * i) / numBands;
                const int y1 = r.getY() + (r.getHeight() * (i + 1)) / numBands;
                const int h  = y1 - y0;
                if (h <= 0)
                    continue;

                const float t = (float) i / (float) (numBands - 1);
                const auto bandCol = colA.interpolatedWith (colB, t);
                g.setColour (bandCol);
                g.fillRect (r.getX(), y0, r.getWidth(), h);

                // 色带交界处 2 行 2x2 Bayer 棋盘格抖动过渡（经典 1998 256 色显示感）
                if (i + 1 < numBands && h >= 4)
                {
                    const auto nextCol = colA.interpolatedWith (colB, (float) (i + 1) / (float) (numBands - 1));
                    g.setColour (nextCol);
                    const int seamY = y1 - 2;
                    for (int row = 0; row < 2; ++row)
                    {
                        const int py = seamY + row;
                        const int phase = (row & 1);
                        const int step  = (row == 0) ? 4 : 2;
                        for (int px = r.getX() + phase; px < r.getRight(); px += step)
                            g.fillRect (px, py, 1, 1);
                    }
                }
            }
        }

        void drawCanvasFinishOverlay (juce::Graphics& g, juce::Rectangle<int> r)
        {
            if (r.getWidth() < 4 || r.getHeight() < 4)
                return;

            const auto& t = getSkinTokens();
            if (! t.hasCanvasFx())
                return;

            juce::Graphics::ScopedSaveState savedState (g);

            // 1. Glass：顶部 40% 玻璃镜面穹顶反光带 + 1px 细腻顶冰光沿（绝不冲淡暗色图谱）
            if (t.surfaceSheenAlpha > 0.0f)
            {
                const auto rf = r.toFloat();
                const float sheenH = juce::jmax (4.0f, rf.getHeight() * 0.40f);
                juce::ColourGradient sheen (juce::Colours::white.withAlpha (t.surfaceSheenAlpha),
                                            0.0f, rf.getY(),
                                            juce::Colours::white.withAlpha (0.0f),
                                            0.0f, rf.getY() + sheenH, false);
                sheen.addColour (0.45, juce::Colours::white.withAlpha (t.surfaceSheenAlpha * 0.35f));
                g.setGradientFill (sheen);
                g.fillRect (rf.getX(), rf.getY(), rf.getWidth(), sheenH);

                g.setColour (juce::Colours::white.withAlpha (juce::jmin (0.22f, t.surfaceSheenAlpha * 1.6f)));
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
            }

            // 2. Neon：克制的四周内沿霓虹灯管微光边（中心图谱保持 100% 清澈）
            if (t.frameStyle == FrameStyle::neonTube)
            {
                const auto rf  = r.toFloat();
                const float rim = juce::jmin (7.0f, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.10f);
                const float a0  = 0.28f * t.glowIntensity;  // ~0.18f
                const auto botCol = interpolateOkLab (sel, accent2, 0.35f);

                juce::ColourGradient topG (sel.withAlpha (a0), 0.0f, rf.getY(),
                                           sel.withAlpha (0.0f), 0.0f, rf.getY() + rim, false);
                g.setGradientFill (topG);
                g.fillRect (rf.getX(), rf.getY(), rf.getWidth(), rim);

                juce::ColourGradient botG (botCol.withAlpha (a0 * 0.90f), 0.0f, rf.getBottom(),
                                           botCol.withAlpha (0.0f), 0.0f, rf.getBottom() - rim, false);
                g.setGradientFill (botG);
                g.fillRect (rf.getX(), rf.getBottom() - rim, rf.getWidth(), rim);

                juce::ColourGradient leftG (sel.withAlpha (a0 * 0.55f), rf.getX(), 0.0f,
                                            sel.withAlpha (0.0f), rf.getX() + rim, 0.0f, false);
                g.setGradientFill (leftG);
                g.fillRect (rf.getX(), rf.getY(), rim, rf.getHeight());

                juce::ColourGradient rightG (botCol.withAlpha (a0 * 0.55f), rf.getRight(), 0.0f,
                                             botCol.withAlpha (0.0f), rf.getRight() - rim, 0.0f, false);
                g.setGradientFill (rightG);
                g.fillRect (rf.getRight() - rim, rf.getY(), rim, rf.getHeight());
            }

            // 3. CRT：2px 扫描线 + 柔和径向暗角
            if (t.scanlineAlpha > 0.0f && t.scanlinePitch > 0)
            {
                g.setColour (juce::Colours::black.withAlpha (t.scanlineAlpha));
                for (int y = r.getY() + 1; y < r.getBottom(); y += t.scanlinePitch)
                    g.fillRect (r.getX(), y, r.getWidth(), 1);
            }

            if (t.vignetteAlpha > 0.0f)
            {
                const auto rf = r.toFloat();
                const float rad = juce::jmax (rf.getWidth(), rf.getHeight()) * 0.72f;
                juce::ColourGradient vig (juce::Colours::black.withAlpha (0.0f), rf.getCentreX(), rf.getCentreY(),
                                          juce::Colours::black.withAlpha (t.vignetteAlpha),
                                          rf.getCentreX() + rad, rf.getCentreY(), true);
                vig.addColour (0.58, juce::Colours::black.withAlpha (0.0f));
                g.setGradientFill (vig);
                g.fillRect (r);
            }
        }
    }
}
