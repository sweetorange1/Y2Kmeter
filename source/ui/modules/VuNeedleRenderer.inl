// ============================================================================
// VuNeedleRenderer.inl
//   Included at the bottom of FineSplitModules.cpp (VuMeterModule scope).
//   Implements the 4 museum-grade analog Needle VU styles:
//     1. Studio    (180 deg classic studio semicircular meter + twin stereo)
//     2. Vintage   (+/-47 deg narrow-sector fan console meter + % sub-scale)
//     3. Dual arc  (+/-120 deg hi-fi illuminated 240 deg live-trail gauge)
//     4. Retro warm(+/-53 deg McIntosh/Nagra/Sony backlit rectangular VU unit)
//   plus precision drawNeedlePointer() and drawNeedleOverlay().
// ============================================================================

namespace VuNeedleDetail
{
    inline bool isDarkCanvas() noexcept
    {
        return PinkXP::content.getPerceivedBrightness() < 0.45f;
    }

    inline bool isPixelHardware() noexcept
    {
        return PinkXP::getSkinTokens().dialFinish == PinkXP::DialFinish::pixelHardware;
    }

    inline bool isBrushedMetal() noexcept
    {
        return PinkXP::getSkinTokens().dialFinish == PinkXP::DialFinish::brushedMetal;
    }

    inline bool isGlassDome() noexcept
    {
        return PinkXP::getSkinTokens().dialFinish == PinkXP::DialFinish::glassDome;
    }

    inline bool isNeonRings() noexcept
    {
        return PinkXP::getSkinTokens().dialFinish == PinkXP::DialFinish::neonRings;
    }

    inline bool isCrtVector() noexcept
    {
        return PinkXP::getSkinTokens().dialFinish == PinkXP::DialFinish::crtVector;
    }

    inline bool isChromaKey() noexcept
    {
        return PinkXP::getSkinTokens().isChromaKey;
    }

    inline bool isGreenScreen() noexcept
    {
        return PinkXP::isGreenScreenBackdrop();
    }

    // Crisp 3x3 pixel slotted screw head for 1999 rack-mount / Winamp hardware faceplates.
    inline void drawPixelScrew (juce::Graphics& g, int cx, int cy, bool darkBezel)
    {
        const juce::Colour hi   = darkBezel ? juce::Colour (0xffb8bec8) : PinkXP::hl;
        const juce::Colour mid  = darkBezel ? juce::Colour (0xff6c727e) : PinkXP::face.darker (0.12f);
        const juce::Colour low  = darkBezel ? juce::Colour (0xff3a3e48) : PinkXP::shdw;
        const juce::Colour slot = darkBezel ? juce::Colour (0xff121418) : PinkXP::dark;

        // 3x3 base block
        g.setColour (mid);
        g.fillRect (cx - 1, cy - 1, 3, 3);
        // Top-left catchlight & bottom-right bevel shade
        g.setColour (hi);
        g.fillRect (cx - 1, cy - 1, 1, 1);
        g.setColour (low);
        g.fillRect (cx + 1, cy,     1, 2);
        g.fillRect (cx,     cy + 1, 1, 1);
        // Diagonal 3px dark screw slot (bottom-left -> center -> top-right)
        g.setColour (slot);
        g.fillRect (cx - 1, cy + 1, 1, 1);
        g.fillRect (cx,     cy,     1, 1);
        g.fillRect (cx + 1, cy - 1, 1, 1);
    }

    // CNC machined recessed slotted bolt head for 2000-2004 brushed aluminum rack hardware.
    inline void drawCncBolt (juce::Graphics& g, float cx, float cy, float r, bool darkBezel)
    {
        if (r < 1.2f)
            return;

        // Recessed counterbore shadow ring
        const float outerR = r + 0.8f;
        g.setColour (juce::Colours::black.withAlpha (darkBezel ? 0.62f : 0.34f));
        g.fillEllipse (cx - outerR, cy - outerR, outerR * 2.0f, outerR * 2.0f);

        // Turned stainless steel bolt head gradient
        const juce::Colour hi  = darkBezel ? juce::Colour (0xffd6dce6) : juce::Colour (0xffffffff);
        const juce::Colour mid = darkBezel ? juce::Colour (0xff7e8694) : juce::Colour (0xffaeb6c2);
        const juce::Colour lo  = darkBezel ? juce::Colour (0xff323640) : juce::Colour (0xff5c6470);
        juce::ColourGradient headGrad (hi, cx - r, cy - r, lo, cx + r, cy + r, false);
        headGrad.addColour (0.48, mid);
        g.setGradientFill (headGrad);
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);

        // Machined diagonal screw slot + bright chamfer edge
        const float d = r * 0.62f;
        g.setColour (juce::Colours::black.withAlpha (0.72f));
        g.drawLine (cx - d, cy + d, cx + d, cy - d, juce::jmax (0.85f, r * 0.30f));
        g.setColour (juce::Colours::white.withAlpha (darkBezel ? 0.48f : 0.68f));
        g.drawLine (cx - d + 0.55f, cy + d + 0.55f, cx + d + 0.55f, cy - d + 0.55f, 0.65f);

        // Crisp chamfered outer hairline
        g.setColour (juce::Colours::black.withAlpha (darkBezel ? 0.55f : 0.35f));
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 0.7f);
    }

    // Restrained CNC horizontal brushed-aluminum micro-sheen (preserves high dark/light text contrast).
    inline void drawBrushedSheenOverlay (juce::Graphics& g, juce::Rectangle<float> bounds, bool darkFace)
    {
        if (bounds.getWidth() < 8.0f || bounds.getHeight() < 8.0f)
            return;

        // Subtle wide metallic sheen band across upper-middle
        juce::ColourGradient band (juce::Colours::transparentWhite, bounds.getX(), bounds.getY(),
                                   juce::Colours::transparentWhite, bounds.getRight(), bounds.getBottom(), false);
        band.addColour (0.28, juce::Colours::white.withAlpha (darkFace ? 0.045f : 0.14f));
        band.addColour (0.52, juce::Colours::black.withAlpha (darkFace ? 0.06f : 0.035f));
        band.addColour (0.78, juce::Colours::white.withAlpha (darkFace ? 0.03f : 0.09f));
        g.setGradientFill (band);
        g.fillRect (bounds);

        // Fine 2px horizontal anodized grain lines
        const auto bi = bounds.toNearestInt();
        g.setColour (darkFace ? juce::Colours::white.withAlpha (0.026f)
                              : juce::Colours::black.withAlpha (0.028f));
        for (int y = bi.getY() + 1; y < bi.getBottom(); y += 2)
            g.fillRect (bi.getX(), y, bi.getWidth(), 1);
    }

    // Crisp 0px CNC milled rectangular instrument bezel (matches SkinCraft machinedChrome).
    inline void drawMachinedPlateBezel (juce::Graphics& g, juce::Rectangle<int> rect,
                                        juce::Colour bg, bool darkCanvas)
    {
        if (rect.getWidth() < 6 || rect.getHeight() < 6)
            return;

        g.setColour (bg);
        g.fillRect (rect);

        const auto in = rect.reduced (2);
        if (! in.isEmpty())
        {
            drawBrushedSheenOverlay (g, in.toFloat(), darkCanvas);

            // 3px cast top inner recess shadow from milled bezel rim
            const float alphas[3] = { darkCanvas ? 0.42f : 0.22f,
                                      darkCanvas ? 0.22f : 0.11f,
                                      darkCanvas ? 0.10f : 0.05f };
            for (int i = 0; i < 3 && i < in.getHeight(); ++i)
            {
                g.setColour (juce::Colours::black.withAlpha (alphas[i]));
                g.fillRect (in.getX(), in.getY() + i, in.getWidth(), 1);
            }
        }

        // 1px dark outer cut
        g.setColour (PinkXP::dark);
        g.drawRect (rect, 1);

        // 1px inner machined bevel (shadow top/left recess + bright chrome bottom/right catchlight)
        const auto rim = rect.reduced (1);
        if (! rim.isEmpty())
        {
            const auto chromeHi = PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f);
            g.setColour (PinkXP::shdw.withAlpha (darkCanvas ? 0.85f : 0.72f));
            g.fillRect (rim.getX(), rim.getY(), rim.getWidth(), 1);
            g.fillRect (rim.getX(), rim.getY(), 1, rim.getHeight());
            g.setColour (chromeHi.withAlpha (darkCanvas ? 0.42f : 0.82f));
            g.fillRect (rim.getX(), rim.getBottom() - 1, rim.getWidth(), 1);
            g.fillRect (rim.getRight() - 1, rim.getY(), 1, rim.getHeight());
        }
    }

    // Frutiger Aero convex glass dome reflection wash across upper ~38% of a clipped dial bay.
    inline void drawGlassDomeReflection (juce::Graphics& g, juce::Rectangle<float> bounds, bool darkFace)
    {
        if (bounds.getWidth() < 12.0f || bounds.getHeight() < 12.0f)
            return;

        const float domeH = bounds.getHeight() * 0.40f;
        juce::Path dome;
        dome.startNewSubPath (bounds.getX(), bounds.getY());
        dome.lineTo (bounds.getRight(), bounds.getY());
        dome.lineTo (bounds.getRight(), bounds.getY() + domeH * 0.72f);
        dome.quadraticTo (bounds.getCentreX(), bounds.getY() + domeH * 1.24f,
                          bounds.getX(), bounds.getY() + domeH * 0.72f);
        dome.closeSubPath();

        juce::ColourGradient gloss (juce::Colours::white.withAlpha (darkFace ? 0.16f : 0.46f),
                                    bounds.getCentreX(), bounds.getY(),
                                    juce::Colours::white.withAlpha (darkFace ? 0.02f : 0.07f),
                                    bounds.getCentreX(), bounds.getY() + domeH * 1.15f, false);
        g.setGradientFill (gloss);
        g.fillPath (dome);

        // Crisp 1px bright white inner top specular refraction edge
        g.setColour (juce::Colours::white.withAlpha (darkFace ? 0.34f : 0.82f));
        g.fillRect (juce::Rectangle<float> (bounds.getX() + 4.0f, bounds.getY() + 0.5f,
                                            juce::jmax (4.0f, bounds.getWidth() - 8.0f), 1.0f));
    }

    inline juce::Colour neonSurfaceBg (bool darkCanvas) noexcept
    {
        return darkCanvas
                   ? PinkXP::content.interpolatedWith (juce::Colour (0xff090c16), 0.55f)
                   : PinkXP::content.interpolatedWith (PinkXP::pink100, 0.16f);
    }

    // Dark smoked obsidian (dark theme) or cyber-tinted light well (light theme) framed by double-pass glowing neon bezel + 4 corner ticks.
    inline void drawNeonDoubleBezel (juce::Graphics& g, juce::Rectangle<float> rect,
                                     juce::Colour accentCol, juce::Colour interiorBg)
    {
        if (rect.getWidth() < 8.0f || rect.getHeight() < 8.0f)
            return;

        constexpr float rad = 2.0f;
        g.setColour (interiorBg);
        g.fillRoundedRectangle (rect, rad);

        // Outer soft neon tube aura + sharp 1px hot white-tinted tube core
        if (! PinkXP::isGreenScreenBackdrop())
        {
            g.setColour (accentCol.withAlpha (0.28f));
            g.drawRoundedRectangle (rect.reduced (0.5f), rad, 3.2f);
        }
        g.setColour (accentCol.interpolatedWith (juce::Colours::white, 0.38f).withAlpha (0.92f));
        g.drawRoundedRectangle (rect.reduced (0.5f), rad, 1.1f);

        // Crisp 5px glowing corner techno L-brackets
        if (rect.getWidth() >= 30.0f && rect.getHeight() >= 24.0f)
        {
            const auto ri = rect.toNearestInt();
            const int arm = juce::jmin (6, juce::jmin (ri.getWidth(), ri.getHeight()) / 6);
            const auto hotCorner = PinkXP::accent2.interpolatedWith (juce::Colours::white, 0.40f);
            g.setColour (hotCorner);
            // Top-left
            g.fillRect (ri.getX(), ri.getY(), arm, 1);
            g.fillRect (ri.getX(), ri.getY(), 1, arm);
            // Top-right
            g.fillRect (ri.getRight() - arm, ri.getY(), arm, 1);
            g.fillRect (ri.getRight() - 1,   ri.getY(), 1, arm);
            // Bottom-left
            g.fillRect (ri.getX(), ri.getBottom() - 1,   arm, 1);
            g.fillRect (ri.getX(), ri.getBottom() - arm, 1, arm);
            // Bottom-right
            g.fillRect (ri.getRight() - arm, ri.getBottom() - 1,   arm, 1);
            g.fillRect (ri.getRight() - 1,   ri.getBottom() - arm, 1, arm);
        }
    }

    // 0px square Tektronix oscilloscope phosphor bezel + optional subtle precision crosshair graduation ticks.
    inline void drawCrtTektronixBezel (juce::Graphics& g, juce::Rectangle<int> rect,
                                       juce::Colour phosphorCol, juce::Colour bgCol,
                                       bool drawCrosshair = true)
    {
        if (rect.getWidth() < 8 || rect.getHeight() < 8)
            return;

        g.setColour (bgCol);
        g.fillRect (rect);

        const auto in = rect.reduced (2);
        if (drawCrosshair && in.getWidth() >= 28 && in.getHeight() >= 24)
        {
            const int cx = in.getCentreX();
            const int cy = in.getCentreY();
            const auto retCol = phosphorCol.withAlpha (0.16f);
            g.setColour (retCol);
            g.fillRect (in.getX(), cy, in.getWidth(), 1);
            g.fillRect (cx, in.getY(), 1, in.getHeight());

            // Tiny perpendicular graduation ticks along center axes
            const int step = juce::jmax (8, juce::jmin (in.getWidth(), in.getHeight()) / 8);
            g.setColour (phosphorCol.withAlpha (0.24f));
            for (int x = cx + step; x < in.getRight() - 2; x += step)
                g.fillRect (x, cy - 2, 1, 5);
            for (int x = cx - step; x > in.getX() + 2; x -= step)
                g.fillRect (x, cy - 2, 1, 5);
            for (int y = cy + step; y < in.getBottom() - 2; y += step)
                g.fillRect (cx - 2, y, 5, 1);
            for (int y = cy - step; y > in.getY() + 2; y -= step)
                g.fillRect (cx - 2, y, 5, 1);
        }

        // Sharp 0px 1px phosphor border + faint inner recessed hairline
        g.setColour (phosphorCol.withAlpha (0.82f));
        g.drawRect (rect, 1);
        if (rect.getWidth() >= 6 && rect.getHeight() >= 6)
        {
            g.setColour (phosphorCol.withAlpha (0.22f));
            g.drawRect (rect.reduced (1), 1);
        }
    }

    // Clean anti-aliased dashed ghost peak needle for Pixel XP (restrained materiality: crisp dashes, zero jagged stair-stepping).
    inline void drawCrispDashedNeedle (juce::Graphics& g,
                                       juce::Point<float> pivot,
                                       float length,
                                       float angleRad,
                                       juce::Colour colour,
                                       float thickness)
    {
        if (length <= 3.0f || colour.isTransparent() || ! std::isfinite (angleRad))
            return;

        const juce::Point<float> tip { pivot.x + std::sin (angleRad) * length,
                                       pivot.y - std::cos (angleRad) * length };
        const float dashes[2] = { 3.5f, 3.0f };
        g.setColour (colour);
        g.drawDashedLine (juce::Line<float> (pivot, tip), dashes, 2, juce::jmax (1.0f, thickness));

        // Crisp 3x3 tip marker pip
        g.setColour (colour.interpolatedWith (juce::Colours::white, 0.35f));
        g.fillRect (juce::roundToInt (tip.x) - 1, juce::roundToInt (tip.y) - 1, 3, 3);
    }

    inline juce::Point<float> arcPoint (juce::Point<float> c, float r, float angRad) noexcept
    {
        return { c.x + std::sin (angRad) * r, c.y - std::cos (angRad) * r };
    }

    inline juce::Path makeArcPath (float cx, float cy, float r, float a0, float a1)
    {
        juce::Path p;
        if (r > 1.0f && std::isfinite (a0) && std::isfinite (a1) && a1 > a0 + 1.0e-4f)
            p.addCentredArc (cx, cy, r, r, 0.0f, a0, a1, true);
        return p;
    }

    // Smooth graduated arc stroked in fine overlapping segments so OKLab/colormap
    // transitions are silky and seamlessly ignite into alert colour past warnDb.
    template <typename ColourFn>
    inline void strokeGraduatedArc (juce::Graphics& g,
                                    float cx, float cy, float r,
                                    float aStart, float aEnd,
                                    float drawFrom, float drawTo,
                                    float strokeW,
                                    int segments,
                                    ColourFn&& colourAtT,
                                    juce::PathStrokeType::EndCapStyle cap = juce::PathStrokeType::butt)
    {
        if (r <= 1.0f || strokeW <= 0.1f || drawTo <= drawFrom + 1.0e-4f || aEnd <= aStart + 1.0e-4f)
            return;

        const float span = aEnd - aStart;
        const float fracSpan = juce::jlimit (0.0f, 1.0f, (drawTo - drawFrom) / span);
        const int n = juce::jmax (1, juce::roundToInt ((float) segments * fracSpan));
        const float step = (drawTo - drawFrom) / (float) n;
        const float overlap = juce::jmin (step * 0.18f, 0.008f);

        for (int i = 0; i < n; ++i)
        {
            const float sa = drawFrom + (float) i * step;
            const float sb = (i == n - 1) ? drawTo : juce::jmin (drawTo, sa + step + overlap);
            const float midA = 0.5f * (sa + sb);
            const float t01 = juce::jlimit (0.0f, 1.0f, (midA - aStart) / span);

            const auto col = colourAtT (t01);
            if (col.getAlpha() == 0)
                continue;

            juce::Path seg;
            seg.addCentredArc (cx, cy, r, r, 0.0f, sa, sb, true);
            const auto endCap = (i == 0 || i == n - 1) ? cap : juce::PathStrokeType::butt;
            g.setColour (col);
            g.strokePath (seg, juce::PathStrokeType (strokeW, juce::PathStrokeType::curved, endCap));
        }
    }

    // Anti-collision label placement engine: places high-priority landmarks first
    // (0 dB, warn threshold, min/max bounds, round 10 dB steps) then remaining ticks,
    // skipping any label that would overlap a previously placed label or clip bounds.
    struct TickLabelCandidate
    {
        juce::String       text;
        juce::Point<float> centre;
        juce::Colour       colour;
        int                priority = 2; // 0 = highest, 1 = medium, 2 = standard
    };

    inline void drawNonOverlappingLabels (juce::Graphics& g,
                                          std::vector<TickLabelCandidate>& items,
                                          juce::Rectangle<float> safeBounds,
                                          float fontPx,
                                          float padX = 1.5f,
                                          float padY = 0.8f)
    {
        if (items.empty() || fontPx < 6.0f)
            return;

        std::stable_sort (items.begin(), items.end(),
            [] (const TickLabelCandidate& a, const TickLabelCandidate& b)
            {
                return a.priority < b.priority;
            });

        std::vector<juce::Rectangle<float>> placed;
        placed.reserve (items.size());

        const auto  axisFont = PinkXP::getAxisFont (fontPx, juce::Font::bold);
        const float boxH     = juce::jmax (fontPx + 2.5f, std::ceil (axisFont.getHeight()) + 1.0f);
        const bool  snapPx   = isPixelHardware();

        for (const auto& item : items)
        {
            if (item.text.isEmpty())
                continue;

            const float measuredW = juce::jmax (PinkXP::SymbolGlyphs::getTextWidth (item.text, axisFont),
                                                (float) item.text.length() * (fontPx * 0.64f));
            const float w = juce::jmax (13.0f, std::ceil (measuredW) + 4.0f);
            if (w > safeBounds.getWidth() || boxH > safeBounds.getHeight())
                continue;

            // Skip labels whose anchor centre lies far outside safeBounds; otherwise clamp
            // cleanly inside safeBounds so wide monospace fonts (e.g. Silkscreen "-24") never clip.
            if (item.centre.x < safeBounds.getX() - 2.0f || item.centre.x > safeBounds.getRight() + 2.0f
             || item.centre.y < safeBounds.getY() - 2.0f || item.centre.y > safeBounds.getBottom() + 2.0f)
                continue;

            juce::Rectangle<float> rect (item.centre.x - w * 0.5f,
                                         item.centre.y - boxH * 0.5f,
                                         w, boxH);

            if (rect.getX() < safeBounds.getX())           rect.setX (safeBounds.getX());
            if (rect.getRight() > safeBounds.getRight())   rect.setX (safeBounds.getRight() - rect.getWidth());
            if (rect.getY() < safeBounds.getY())           rect.setY (safeBounds.getY());
            if (rect.getBottom() > safeBounds.getBottom()) rect.setY (safeBounds.getBottom() - rect.getHeight());

            if (snapPx)
            {
                rect.setX (juce::jlimit (std::ceil (safeBounds.getX()),
                                         juce::jmax (std::ceil (safeBounds.getX()), std::floor (safeBounds.getRight() - rect.getWidth())),
                                         std::round (rect.getX())));
                rect.setY (juce::jlimit (std::ceil (safeBounds.getY()),
                                         juce::jmax (std::ceil (safeBounds.getY()), std::floor (safeBounds.getBottom() - rect.getHeight())),
                                         std::round (rect.getY())));
            }

            const auto testRect = rect.expanded (padX, padY);
            bool collides = false;
            for (const auto& prev : placed)
            {
                if (prev.intersects (testRect))
                {
                    collides = true;
                    break;
                }
            }
            if (collides)
                continue;

            placed.push_back (rect);
            PinkXP::drawCachedText (g, item.text, rect, juce::Justification::centred,
                                    fontPx, juce::Font::bold, item.colour, true);
        }
    }

    inline int tickLabelPriority (float db, float minDb, float maxDb, float warnDb) noexcept
    {
        if (std::abs (db) < 0.25f || std::abs (db - warnDb) < 0.25f) return 0;
        if (std::abs (db - maxDb) < 0.25f || std::abs (db - minDb) < 0.25f) return 0;
        const int idb = juce::roundToInt (db);
        if (idb % 10 == 0) return 1;
        return 2;
    }

    // Small crisp channel badge ("L" / "R") tucked cleanly at upper-left of a dial pane.
    inline void drawChannelBadge (juce::Graphics& g,
                                  juce::Rectangle<float> pane,
                                  const char* tag,
                                  juce::Colour accent,
                                  bool darkFace)
    {
        const auto& tokens = PinkXP::getSkinTokens();
        if (tokens.isChromaKey)
            return;

        const float sz = juce::jlimit (13.0f, 17.0f, juce::jmin (pane.getWidth(), pane.getHeight()) * 0.13f);
        const juce::Rectangle<float> box (pane.getX() + 4.0f, pane.getY() + 3.0f, sz + 3.0f, sz);
        const juce::Colour bg = darkFace ? juce::Colour (0xff0c0e12).withAlpha (0.86f)
                                         : PinkXP::content.interpolatedWith (accent, 0.16f).withAlpha (0.94f);

        if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware
         || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass
         || tokens.frameStyle == PinkXP::FrameStyle::neonTube
         || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor
         || tokens.badgeRadius < 0.5f)
        {
            const auto boxI = box.toNearestInt();
            const juce::Colour badgeBg = (tokens.frameStyle == PinkXP::FrameStyle::neonTube && darkFace)
                                             ? juce::Colour (0xff0a0d16).withAlpha (0.94f)
                                             : bg;
            const juce::Colour badgeBorder = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                                                 ? PinkXP::sel.withAlpha (0.85f)
                                                 : accent.withAlpha (darkFace ? 0.85f : 0.88f);
            PinkXP::Craft::drawBadgePill (g, boxI, badgeBg, badgeBorder);
            const bool darkBadge = badgeBg.getPerceivedBrightness() < 0.45f;
            const juce::Colour txtCol = darkBadge ? accent.interpolatedWith (juce::Colours::white, 0.42f)
                                                  : PinkXP::readableInkOn (badgeBg);
            const float fPx = juce::jlimit (7.2f, 9.2f, sz * (isCrtVector() ? 0.56f : 0.62f));
            PinkXP::drawCachedText (g, tag, boxI, juce::Justification::centred,
                                    fPx, juce::Font::bold, txtCol, true);
            return;
        }

        g.setColour (bg);
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (accent.withAlpha (darkFace ? 0.65f : 0.72f));
        g.drawRoundedRectangle (box, 3.0f, 1.0f);

        const juce::Colour txtCol = darkFace ? accent.interpolatedWith (juce::Colours::white, 0.35f)
                                             : accent.darker (0.35f);
        PinkXP::drawCachedText (g, tag, box, juce::Justification::centred,
                                juce::jlimit (7.5f, 9.5f, sz * 0.62f), juce::Font::bold,
                                txtCol, true);
    }

    // Subtle vertical brushed divider between twin stereo L/R dial faces.
    inline void drawTwinDivider (juce::Graphics& g, float x, float y0, float y1, bool darkCanvas)
    {
        const auto& tokens = PinkXP::getSkinTokens();
        if (tokens.isChromaKey)
            return;

        if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware)
        {
            // Classic Win95 2px etched vertical separator groove
            const int ix = juce::roundToInt (x);
            const int iy0 = juce::roundToInt (y0);
            const int h   = juce::jmax (1, juce::roundToInt (y1 - y0));
            g.setColour (PinkXP::shdw.withAlpha (darkCanvas ? 0.75f : 0.65f));
            g.fillRect (ix - 1, iy0, 1, h);
            g.setColour (PinkXP::hl.withAlpha (darkCanvas ? 0.22f : 0.72f));
            g.fillRect (ix,     iy0, 1, h);
            return;
        }

        const juce::Colour midCol = darkCanvas ? juce::Colours::white.withAlpha (0.14f)
                                               : PinkXP::ink.withAlpha (0.18f);
        juce::ColourGradient grad (midCol.withAlpha (0.0f), x, y0,
                                   midCol.withAlpha (0.0f), x, y1, false);
        grad.addColour (0.20, midCol);
        grad.addColour (0.80, midCol);
        g.setGradientFill (grad);
        g.fillRect (juce::Rectangle<float> (x - 0.5f, y0, 1.0f, juce::jmax (1.0f, y1 - y0)));
    }

    // Harmonized palette bundle for the Retro Warm hardware glass window so both
    // drawNeedleRetroWarmDial() and drawNeedleOverlay() agree 100% on contrast.
    struct RetroWarmFaceSpec
    {
        juce::Colour bezelTop;
        juce::Colour bezelBot;
        juce::Colour faceCenter;
        juce::Colour faceEdge;
        juce::Colour lampCol;
        juce::Colour scaleInk;
        juce::Colour dangerCol;
        bool         faceIsDark = false;
    };

    inline RetroWarmFaceSpec resolveRetroWarmFace (VuMeterModule::ColorMap map,
                                                   bool darkCanvas,
                                                   bool backlightOn,
                                                   juce::Colour alertCol) noexcept
    {
        RetroWarmFaceSpec s;
        s.dangerCol = alertCol;

        if (darkCanvas)
        {
            s.bezelTop = juce::Colour (0xff2e2b2f);
            s.bezelBot = juce::Colour (0xff121114);
        }
        else
        {
            s.bezelTop = PinkXP::face.interpolatedWith (juce::Colour (0xffd8d0c8), 0.35f);
            s.bezelBot = PinkXP::shdw.interpolatedWith (juce::Colour (0xff8a8078), 0.45f);
        }

        switch (map)
        {
            case VuMeterModule::ColorMap::warmVu:
                // Authentic Nagra / Sony warm amber parchment illuminated face (always warm light parchment)
                s.bezelTop   = juce::Colour (0xff322923);
                s.bezelBot   = juce::Colour (0xff15100d);
                s.faceCenter = backlightOn ? juce::Colour (0xfffbe7b5) : juce::Colour (0xffe0d3b4);
                s.faceEdge   = backlightOn ? juce::Colour (0xffd49f50) : juce::Colour (0xffb8a784);
                s.lampCol    = juce::Colour (0xffffd67a);
                s.scaleInk   = juce::Colour (0xff26170b);
                s.dangerCol  = juce::Colour (0xffc91c22);
                s.faceIsDark = false;
                break;

            case VuMeterModule::ColorMap::ice:
                // Iconic McIntosh illuminated sapphire-blue VU window (dark) / porcelain ice enamel (light)
                if (darkCanvas)
                {
                    s.bezelTop   = juce::Colour (0xff262c36);
                    s.bezelBot   = juce::Colour (0xff0e1218);
                    s.faceCenter = backlightOn ? juce::Colour (0xff153d73) : juce::Colour (0xff0f223d);
                    s.faceEdge   = backlightOn ? juce::Colour (0xff08162c) : juce::Colour (0xff070f1d);
                    s.lampCol    = juce::Colour (0xff4eb8ff);
                    s.scaleInk   = juce::Colour (0xffd8f0ff);
                    s.dangerCol  = juce::Colour (0xffff4d5e);
                    s.faceIsDark = true;
                }
                else
                {
                    s.faceCenter = backlightOn ? juce::Colour (0xffeaf6ff) : juce::Colour (0xffd8e6f2);
                    s.faceEdge   = backlightOn ? juce::Colour (0xffb9d9f4) : juce::Colour (0xffb5c8da);
                    s.lampCol    = juce::Colour (0xff8cd4ff);
                    s.scaleInk   = juce::Colour (0xff122c4a);
                    s.dangerCol  = juce::Colour (0xffcf2238);
                    s.faceIsDark = false;
                }
                break;

            case VuMeterModule::ColorMap::studio:
                if (darkCanvas)
                {
                    s.faceCenter = backlightOn ? juce::Colour (0xff262c22) : juce::Colour (0xff181c17);
                    s.faceEdge   = backlightOn ? juce::Colour (0xff121610) : juce::Colour (0xff0d100c);
                    s.lampCol    = juce::Colour (0xffd8e68c);
                    s.scaleInk   = juce::Colour (0xffeae8d8);
                    s.faceIsDark = true;
                }
                else
                {
                    s.faceCenter = backlightOn ? juce::Colour (0xfff6f4df) : juce::Colour (0xffe2e0cd);
                    s.faceEdge   = backlightOn ? juce::Colour (0xffd7d2ad) : juce::Colour (0xffc2bfaa);
                    s.lampCol    = juce::Colour (0xfffff0a6);
                    s.scaleInk   = juce::Colour (0xff1f261b);
                    s.faceIsDark = false;
                }
                break;

            case VuMeterModule::ColorMap::miniMeter:
                if (darkCanvas)
                {
                    s.faceCenter = backlightOn ? juce::Colour (0xff2d1629) : juce::Colour (0xff1b0e1a);
                    s.faceEdge   = backlightOn ? juce::Colour (0xff140814) : juce::Colour (0xff0d060d);
                    s.lampCol    = juce::Colour (0xffff82c8);
                    s.scaleInk   = juce::Colour (0xffffe2f4);
                    s.faceIsDark = true;
                }
                else
                {
                    s.faceCenter = backlightOn ? juce::Colour (0xffffeef5) : juce::Colour (0xffead5df);
                    s.faceEdge   = backlightOn ? juce::Colour (0xfff0c2d8) : juce::Colour (0xffcfb4c2);
                    s.lampCol    = juce::Colour (0xffffa8d6);
                    s.scaleInk   = juce::Colour (0xff38122a);
                    s.faceIsDark = false;
                }
                break;

            case VuMeterModule::ColorMap::theme:
            default:
                if (darkCanvas)
                {
                    // Warm backlit dark glass harmonizing with Black Pink / Void Grey / Starlight
                    const auto warmTint = juce::Colour (0xff352416);
                    s.faceCenter = PinkXP::content.interpolatedWith (warmTint, backlightOn ? 0.52f : 0.24f)
                                                  .interpolatedWith (PinkXP::pink300, backlightOn ? 0.10f : 0.04f);
                    s.faceEdge   = PinkXP::content.darker (0.32f);
                    s.lampCol    = PinkXP::pink400.interpolatedWith (juce::Colour (0xffffcc78), 0.55f);
                    s.scaleInk   = PinkXP::ink.interpolatedWith (juce::Colour (0xfffff2dc), 0.25f);
                    s.faceIsDark = true;
                }
                else
                {
                    // Warm luminous theme-tinted antique cream glass on Bubblegum / Paper Grey
                    s.faceCenter = PinkXP::content.interpolatedWith (juce::Colour (0xfffff1d0), backlightOn ? 0.56f : 0.26f);
                    s.faceEdge   = PinkXP::content.interpolatedWith (juce::Colour (0xffe8cb96), backlightOn ? 0.48f : 0.30f);
                    s.lampCol    = PinkXP::pink300.interpolatedWith (juce::Colour (0xffffd98e), 0.60f);
                    s.scaleInk   = PinkXP::ink;
                    s.faceIsDark = false;
                }
                break;
        }

        if (isNeonRings() && darkCanvas)
        {
            // Neon Cyber (dark theme): deep smoked cyber-backlit rack unit
            s.bezelTop   = juce::Colour (0xff151828);
            s.bezelBot   = juce::Colour (0xff080912);
            s.faceIsDark = true;
            switch (map)
            {
                case VuMeterModule::ColorMap::warmVu:
                    s.faceCenter = backlightOn ? juce::Colour (0xff26160a) : juce::Colour (0xff160d07);
                    s.faceEdge   = juce::Colour (0xff0c0704);
                    s.lampCol    = juce::Colour (0xffffaa44);
                    s.scaleInk   = juce::Colour (0xffffe4b5);
                    s.dangerCol  = juce::Colour (0xffff3852);
                    break;
                case VuMeterModule::ColorMap::ice:
                    s.faceCenter = backlightOn ? juce::Colour (0xff0b2244) : juce::Colour (0xff081529);
                    s.faceEdge   = juce::Colour (0xff050c18);
                    s.lampCol    = juce::Colour (0xff48d2ff);
                    s.scaleInk   = juce::Colour (0xffd8f6ff);
                    s.dangerCol  = juce::Colour (0xffff4864);
                    break;
                case VuMeterModule::ColorMap::studio:
                    s.faceCenter = backlightOn ? juce::Colour (0xff112218) : juce::Colour (0xff0a1510);
                    s.faceEdge   = juce::Colour (0xff060c09);
                    s.lampCol    = juce::Colour (0xff48ff9a);
                    s.scaleInk   = juce::Colour (0xffe0ffe8);
                    break;
                case VuMeterModule::ColorMap::miniMeter:
                    s.faceCenter = backlightOn ? juce::Colour (0xff240c28) : juce::Colour (0xff140818);
                    s.faceEdge   = juce::Colour (0xff0b040e);
                    s.lampCol    = juce::Colour (0xffff5cc8);
                    s.scaleInk   = juce::Colour (0xffffdcf4);
                    break;
                case VuMeterModule::ColorMap::theme:
                default:
                    s.faceCenter = juce::Colour (0xff0d101c).interpolatedWith (PinkXP::sel, backlightOn ? 0.16f : 0.06f);
                    s.faceEdge   = juce::Colour (0xff060810);
                    s.lampCol    = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.25f);
                    s.scaleInk   = juce::Colour (0xffeef4ff);
                    break;
            }
        }

        return s;
    }

    inline bool isNeedleFaceDark (VuMeterModule::NeedleDialStyle style,
                                  VuMeterModule::ColorMap map,
                                  bool darkCanvas,
                                  bool backlightOn,
                                  juce::Colour alertCol) noexcept
    {
        if (style == VuMeterModule::NeedleDialStyle::retroWarm)
            return resolveRetroWarmFace (map, darkCanvas, backlightOn, alertCol).faceIsDark;
        return darkCanvas;
    }

    inline juce::Colour resolveRightCoaxialAccent (VuMeterModule::ColorMap map,
                                                   bool faceIsDark,
                                                   juce::Colour leftBase) noexcept
    {
        auto hueDelta = [] (float h1, float h2) noexcept
        {
            float d = std::abs (h1 - h2);
            return d > 0.5f ? (1.0f - d) : d;
        };

        if (map == VuMeterModule::ColorMap::theme)
        {
            const auto a2 = PinkXP::accent2;
            if (a2.getSaturation() >= 0.25f && hueDelta (a2.getHue(), leftBase.getHue()) >= 0.14f)
            {
                return faceIsDark ? a2.interpolatedWith (juce::Colours::white, 0.24f)
                                  : a2.darker (0.22f);
            }
            const float lHue = leftBase.getHue();
            const bool leftIsCoolCyan = (leftBase.getSaturation() >= 0.20f && lHue >= 0.44f && lHue <= 0.72f);
            if (leftIsCoolCyan)
                return faceIsDark ? juce::Colour (0xffffc64c) : juce::Colour (0xffc2560c);
            return faceIsDark ? juce::Colour (0xff4ae6ff) : juce::Colour (0xff125eb2);
        }

        if (map == VuMeterModule::ColorMap::ice)
            return faceIsDark ? juce::Colour (0xffffcc5c) : juce::Colour (0xffb85012);

        return faceIsDark ? juce::Colour (0xff52e6ff) : juce::Colour (0xff165ca8);
    }

    inline juce::Colour resolveNeedleColour (VuMeterModule::NeedleDialStyle style,
                                             VuMeterModule::ColorMap map,
                                             VuMeterModule::NeedleShape shape,
                                             float db,
                                             float warnDb,
                                             bool isRightCoaxial,
                                             bool faceIsDark,
                                             juce::Colour paletteCol,
                                             juce::Colour alertCol) noexcept
    {
        const bool isHot = db >= warnDb;

        const juce::Colour primaryRef = (map == VuMeterModule::ColorMap::theme)
                                            ? (faceIsDark ? PinkXP::pink500 : PinkXP::pink700)
                                            : paletteCol;

        if (shape == VuMeterModule::NeedleShape::glow)
        {
            if (isRightCoaxial)
            {
                const juce::Colour sec = resolveRightCoaxialAccent (map, faceIsDark, primaryRef);
                return isHot ? alertCol.interpolatedWith (juce::Colour (0xffffb85c), 0.28f)
                             : sec;
            }
            if (isHot)
                return alertCol.interpolatedWith (juce::Colours::white, faceIsDark ? 0.18f : 0.0f);
            return faceIsDark ? paletteCol.interpolatedWith (juce::Colours::white, 0.22f)
                              : paletteCol.darker (0.28f);
        }

        if (isRightCoaxial)
        {
            if (isHot)
                return faceIsDark ? alertCol.interpolatedWith (juce::Colour (0xffffb048), 0.30f)
                                  : alertCol.interpolatedWith (juce::Colour (0xffc84e0a), 0.32f);
            return resolveRightCoaxialAccent (map, faceIsDark, primaryRef);
        }

        // Authentic classic dark bronze-black needle on light Retro Warm parchment
        if (style == VuMeterModule::NeedleDialStyle::retroWarm && ! faceIsDark
         && map == VuMeterModule::ColorMap::warmVu)
        {
            return isHot ? juce::Colour (0xffb8181e) : juce::Colour (0xff1d120b);
        }

        if (faceIsDark)
        {
            if (isHot)
                return alertCol.interpolatedWith (juce::Colours::white, 0.20f);

            switch (map)
            {
                case VuMeterModule::ColorMap::studio:    return juce::Colour (0xfff6efe2);
                case VuMeterModule::ColorMap::warmVu:    return juce::Colour (0xffffe2b0);
                case VuMeterModule::ColorMap::miniMeter: return juce::Colour (0xffff8ad8);
                case VuMeterModule::ColorMap::ice:       return juce::Colour (0xffccf4ff);
                case VuMeterModule::ColorMap::theme:
                default:
                    return PinkXP::pink500.interpolatedWith (juce::Colours::white, 0.30f);
            }
        }
        else
        {
            if (isHot)
                return alertCol.darker (0.12f);

            switch (map)
            {
                case VuMeterModule::ColorMap::studio:    return juce::Colour (0xffc42228);
                case VuMeterModule::ColorMap::warmVu:    return juce::Colour (0xff26150a);
                case VuMeterModule::ColorMap::miniMeter: return juce::Colour (0xff9b1676);
                case VuMeterModule::ColorMap::ice:       return juce::Colour (0xff0e4884);
                case VuMeterModule::ColorMap::theme:
                default:
                    return PinkXP::pink700.darker (0.18f);
            }
        }
    }

    // Machined pivot hub (circular anodized jewel cap when capR <= 0.5,
    // or machined half-moon fan housing cover when capR > 0.5).
    inline void drawMachinedPivotCap (juce::Graphics& g,
                                      juce::Point<float> pivot,
                                      float dialRadius,
                                      float capR,
                                      bool darkCanvas,
                                      bool isRetroWarm)
    {
        const auto& tokens = PinkXP::getSkinTokens();
        if (tokens.isChromaKey)
        {
            // Minimal solid needle anchor pin only (zero opaque bezel pollution; inherits square vs circular skin craft)
            g.setColour (PinkXP::ink);
            if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware || ! tokens.statusDotCircular)
            {
                const int ix = juce::roundToInt (pivot.x);
                const int iy = juce::roundToInt (pivot.y);
                g.fillRect (ix - 2, iy - 2, 5, 5);
            }
            else
            {
                const float pinR = 3.0f;
                g.fillEllipse (pivot.x - pinR, pivot.y - pinR, pinR * 2.0f, pinR * 2.0f);
            }
            return;
        }

        if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware && capR > 0.5f)
        {
            // Beveled Win95 rectangular pivot base block (Vintage & Retro Warm — crisp orthogonal hardware block)
            const int ix     = juce::roundToInt (pivot.x);
            const int iy     = juce::roundToInt (pivot.y);
            const int halfW  = juce::jmax (6, juce::roundToInt (capR));
            const int blockH = juce::jmax (6, juce::roundToInt (capR * 0.78f));
            const juce::Rectangle<int> block (ix - halfW, iy - blockH + 1, halfW * 2 + 1, blockH + 1);
            const juce::Colour blockFill = (darkCanvas || isRetroWarm)
                                               ? juce::Colour (0xff32343c)
                                               : PinkXP::face.darker (0.06f);
            PinkXP::drawRaised (g, block, blockFill);

            // Crisp 1px dark outer hairline + centered 3x3 pixel screw pin
            g.setColour (PinkXP::dark);
            g.drawRect (block, 1);
            if (blockH >= 7)
                drawPixelScrew (g, ix, block.getCentreY(), darkCanvas || isRetroWarm);
            return;
        }

        if (tokens.dialFinish == PinkXP::DialFinish::brushedMetal)
        {
            if (capR > 0.5f)
            {
                // Beveled CNC brushed-steel rectangular base plate (Vintage & Retro Warm)
                const int ix     = juce::roundToInt (pivot.x);
                const int iy     = juce::roundToInt (pivot.y);
                const int halfW  = juce::jmax (7, juce::roundToInt (capR));
                const int blockH = juce::jmax (7, juce::roundToInt (capR * 0.78f));
                const juce::Rectangle<int> block (ix - halfW, iy - blockH + 1, halfW * 2 + 1, blockH + 1);
                const bool darkPlate = (darkCanvas || isRetroWarm);

                // Drop shadow
                g.setColour (juce::Colours::black.withAlpha (darkPlate ? 0.48f : 0.24f));
                g.fillRect (block.translated (0, 1));

                const juce::Colour hi  = darkPlate ? juce::Colour (0xff6e7482) : juce::Colour (0xfff4f7fc);
                const juce::Colour mid = darkPlate ? juce::Colour (0xff3c404a) : juce::Colour (0xffcfd5e0);
                const juce::Colour lo  = darkPlate ? juce::Colour (0xff1c1f26) : juce::Colour (0xff8a92a0);
                juce::ColourGradient plateGrad (hi, (float) block.getX(), (float) block.getY(),
                                                lo, (float) block.getX(), (float) block.getBottom(), false);
                plateGrad.addColour (0.44, mid.interpolatedWith (juce::Colours::white, 0.16f));
                plateGrad.addColour (0.52, mid.darker (0.08f));
                g.setGradientFill (plateGrad);
                g.fillRect (block);

                // 1px dark outer cut + 1px bright top/left chrome chamfer
                g.setColour (PinkXP::dark);
                g.drawRect (block, 1);
                const auto in = block.reduced (1);
                if (! in.isEmpty())
                {
                    g.setColour (juce::Colours::white.withAlpha (darkPlate ? 0.42f : 0.78f));
                    g.fillRect (in.getX(), in.getY(), in.getWidth(), 1);
                    g.fillRect (in.getX(), in.getY(), 1, in.getHeight());
                    g.setColour (juce::Colours::black.withAlpha (darkPlate ? 0.50f : 0.28f));
                    g.fillRect (in.getX(), in.getBottom() - 1, in.getWidth(), 1);
                    g.fillRect (in.getRight() - 1, in.getY(), 1, in.getHeight());
                }

                if (blockH >= 8)
                    drawCncBolt (g, (float) ix, (float) block.getCentreY(),
                                 juce::jlimit (1.8f, 3.0f, (float) blockH * 0.24f), darkPlate);
                return;
            }

            // CNC machined circular brushed chrome pivot cap (Studio & Dual Arc)
            const float hubR = juce::jlimit (4.8f, 8.8f, dialRadius * 0.086f);
            g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.56f : 0.28f));
            g.fillEllipse (pivot.x - hubR + 0.8f, pivot.y - hubR + 1.3f, hubR * 2.0f, hubR * 2.0f);

            // Chamfered outer chrome ring
            const juce::Colour rimHi = darkCanvas ? juce::Colour (0xffb8c0ce) : juce::Colour (0xffffffff);
            const juce::Colour rimLo = darkCanvas ? juce::Colour (0xff242830) : juce::Colour (0xff565e6c);
            juce::ColourGradient outerG (rimHi, pivot.x - hubR, pivot.y - hubR,
                                         rimLo, pivot.x + hubR, pivot.y + hubR, false);
            outerG.addColour (0.48, darkCanvas ? juce::Colour (0xff666e7c) : juce::Colour (0xffc2cad6));
            g.setGradientFill (outerG);
            g.fillEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f);

            // Concentric turned-aluminum mid collar
            const float midR = hubR * 0.72f;
            juce::ColourGradient midG (darkCanvas ? juce::Colour (0xff484e5a) : juce::Colour (0xffdce2ec),
                                       pivot.x + midR, pivot.y - midR,
                                       darkCanvas ? juce::Colour (0xff16181e) : juce::Colour (0xff7e8694),
                                       pivot.x - midR, pivot.y + midR, false);
            g.setGradientFill (midG);
            g.fillEllipse (pivot.x - midR, pivot.y - midR, midR * 2.0f, midR * 2.0f);

            // Specular concentric highlight arc
            juce::Path rimArc;
            rimArc.addCentredArc (pivot.x, pivot.y, midR, midR, 0.0f, -2.2f, -0.35f, true);
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.52f : 0.78f));
            g.strokePath (rimArc, juce::PathStrokeType (0.95f));

            // Center CNC machined steel axle pin
            const float pinR = juce::jmax (1.5f, hubR * 0.30f);
            juce::ColourGradient pinG (juce::Colour (0xfff4f7fc), pivot.x - pinR * 0.5f, pivot.y - pinR * 0.5f,
                                       juce::Colour (0xff3c424e), pivot.x + pinR * 0.7f, pivot.y + pinR * 0.7f, false);
            g.setGradientFill (pinG);
            g.fillEllipse (pivot.x - pinR, pivot.y - pinR, pinR * 2.0f, pinR * 2.0f);

            g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.72f : 0.48f));
            g.drawEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f, 0.95f);
            return;
        }

        if (tokens.dialFinish == PinkXP::DialFinish::glassDome)
        {
            if (capR > 0.5f)
            {
                // Glossy translucent crystal half-moon dome cover (Vintage & Retro Warm)
                juce::Path dome;
                dome.addCentredArc (pivot.x, pivot.y, capR, capR, 0.0f,
                                    -juce::MathConstants<float>::halfPi,
                                    +juce::MathConstants<float>::halfPi, true);
                dome.lineTo (pivot.x + capR, pivot.y + 1.5f);
                dome.lineTo (pivot.x - capR, pivot.y + 1.5f);
                dome.closeSubPath();

                g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.42f : 0.18f));
                g.fillPath (dome, juce::AffineTransform::translation (0.0f, 1.2f));

                const juce::Colour topC = darkCanvas ? juce::Colour (0xff8a96b0).withAlpha (0.82f)
                                                     : juce::Colours::white.withAlpha (0.90f);
                const juce::Colour botC = darkCanvas ? juce::Colour (0xff1c2234).withAlpha (0.92f)
                                                     : PinkXP::pink200.interpolatedWith (juce::Colour (0xff8aa4c2), 0.45f).withAlpha (0.86f);
                juce::ColourGradient crystalG (topC, pivot.x, pivot.y - capR,
                                               botC, pivot.x, pivot.y + 1.5f, false);
                g.setGradientFill (crystalG);
                g.fillPath (dome);

                // Upper crystal specular crescent + crisp bright glass rim
                juce::Path hiArc;
                hiArc.addCentredArc (pivot.x, pivot.y, juce::jmax (2.0f, capR - 1.5f), juce::jmax (2.0f, capR - 1.5f),
                                     0.0f, -1.25f, 0.35f, true);
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.62f : 0.88f));
                g.strokePath (hiArc, juce::PathStrokeType (1.35f));

                g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.60f : 0.32f));
                g.strokePath (dome, juce::PathStrokeType (1.0f));
                return;
            }

            // Glossy translucent crystal dome circular pivot cap (Studio & Dual Arc)
            const float hubR = juce::jlimit (4.8f, 8.6f, dialRadius * 0.086f);
            g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.44f : 0.22f));
            g.fillEllipse (pivot.x - hubR + 0.6f, pivot.y - hubR + 1.3f, hubR * 2.0f, hubR * 2.0f);

            const juce::Colour domeHi  = juce::Colours::white.withAlpha (darkCanvas ? 0.80f : 0.94f);
            const juce::Colour domeMid = PinkXP::sel.interpolatedWith (juce::Colours::white, darkCanvas ? 0.25f : 0.50f).withAlpha (0.78f);
            const juce::Colour domeLo  = darkCanvas ? juce::Colour (0xff141826).withAlpha (0.92f)
                                                    : PinkXP::dark.interpolatedWith (PinkXP::sel, 0.40f).withAlpha (0.68f);
            juce::ColourGradient sphereG (domeHi, pivot.x - hubR * 0.42f, pivot.y - hubR * 0.48f,
                                          domeLo, pivot.x + hubR * 0.65f, pivot.y + hubR * 0.72f, true);
            sphereG.addColour (0.48, domeMid);
            g.setGradientFill (sphereG);
            g.fillEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f);

            // Crisp bright white glass rim + luminous top-left specular dot
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.56f : 0.86f));
            g.drawEllipse (pivot.x - hubR + 0.5f, pivot.y - hubR + 0.5f, (hubR - 0.5f) * 2.0f, (hubR - 0.5f) * 2.0f, 1.0f);
            const float dotR = juce::jmax (1.2f, hubR * 0.26f);
            g.setColour (juce::Colours::white.withAlpha (0.90f));
            g.fillEllipse (pivot.x - hubR * 0.36f - dotR * 0.5f,
                           pivot.y - hubR * 0.42f - dotR * 0.5f,
                           dotR * 2.0f, dotR * 1.5f);
            return;
        }

        if (tokens.dialFinish == PinkXP::DialFinish::neonRings)
        {
            const juce::Colour neonC = PinkXP::sel;
            const juce::Colour hotC  = darkCanvas ? neonC.interpolatedWith (juce::Colours::white, 0.52f)
                                                  : neonC.darker (0.14f);
            const juce::Colour hubBg = darkCanvas ? juce::Colour (0xff0a0c15)
                                                  : PinkXP::content.interpolatedWith (PinkXP::pink100, 0.24f);
            if (capR > 0.5f)
            {
                // Cyber half-moon housing framed by glowing neon tube rim
                juce::Path dome;
                dome.addCentredArc (pivot.x, pivot.y, capR, capR, 0.0f,
                                    -juce::MathConstants<float>::halfPi,
                                    +juce::MathConstants<float>::halfPi, true);
                dome.lineTo (pivot.x + capR, pivot.y + 1.5f);
                dome.lineTo (pivot.x - capR, pivot.y + 1.5f);
                dome.closeSubPath();

                g.setColour (hubBg);
                g.fillPath (dome);
                if (! PinkXP::isGreenScreenBackdrop())
                {
                    g.setColour (neonC.withAlpha (darkCanvas ? 0.34f : 0.26f));
                    g.strokePath (dome, juce::PathStrokeType (3.4f));
                }
                g.setColour (hotC);
                g.strokePath (dome, juce::PathStrokeType (1.2f));
                const float pinR = juce::jlimit (1.5f, 2.6f, capR * 0.18f);
                g.setColour (hotC);
                g.fillEllipse (pivot.x - pinR, pivot.y - capR * 0.34f - pinR, pinR * 2.0f, pinR * 2.0f);
                return;
            }

            // Glowing neon ring pivot hub (Studio & Dual Arc)
            const float hubR = juce::jlimit (4.6f, 8.2f, dialRadius * 0.082f);
            g.setColour (hubBg);
            g.fillEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f);
            if (! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (neonC.withAlpha (darkCanvas ? 0.34f : 0.26f));
                g.drawEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f, 3.2f);
            }
            g.setColour (hotC);
            g.drawEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f, 1.25f);
            const float pinR = juce::jmax (1.4f, hubR * 0.28f);
            g.fillEllipse (pivot.x - pinR, pivot.y - pinR, pinR * 2.0f, pinR * 2.0f);
            return;
        }

        if (tokens.dialFinish == PinkXP::DialFinish::crtVector)
        {
            const juce::Colour phos = darkCanvas ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.25f)
                                                 : PinkXP::sel.darker (0.18f);
            const juce::Colour tubeBg = darkCanvas ? PinkXP::content.darker (0.32f)
                                                   : PinkXP::content;
            if (capR > 0.5f)
            {
                // Minimalist technical open vector base arch + precision crosshair pin
                juce::Path arch;
                arch.addCentredArc (pivot.x, pivot.y, capR, capR, 0.0f,
                                    -juce::MathConstants<float>::halfPi,
                                    +juce::MathConstants<float>::halfPi, true);
                arch.lineTo (pivot.x + capR, pivot.y + 1.0f);
                arch.lineTo (pivot.x - capR, pivot.y + 1.0f);
                arch.closeSubPath();
                g.setColour (tubeBg);
                g.fillPath (arch);
                g.setColour (phos.withAlpha (0.86f));
                g.strokePath (arch, juce::PathStrokeType (1.15f));
                const float r = juce::jlimit (2.0f, 3.2f, capR * 0.24f);
                g.drawLine (pivot.x - r, pivot.y - capR * 0.34f, pivot.x + r, pivot.y - capR * 0.34f, 1.0f);
                g.drawLine (pivot.x, pivot.y - capR * 0.34f - r, pivot.x, pivot.y - capR * 0.34f + r, 1.0f);
                return;
            }

            // Minimalist open-ring vector pivot hub + central '+' reticle
            const float hubR = juce::jlimit (4.2f, 7.6f, dialRadius * 0.078f);
            g.setColour (tubeBg);
            g.fillEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f);
            if (! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (phos.withAlpha (0.24f));
                g.drawEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f, 2.8f);
            }
            g.setColour (phos);
            g.drawEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f, 1.25f);
            const float c = hubR * 0.48f;
            g.drawLine (pivot.x - c, pivot.y, pivot.x + c, pivot.y, 1.0f);
            g.drawLine (pivot.x, pivot.y - c, pivot.x, pivot.y + c, 1.0f);
            return;
        }

        if (capR > 0.5f)
        {
            // Half-moon machined housing cap covering the needle root
            juce::Path dome;
            dome.addCentredArc (pivot.x, pivot.y, capR, capR, 0.0f,
                                -juce::MathConstants<float>::halfPi,
                                +juce::MathConstants<float>::halfPi, true);
            dome.lineTo (pivot.x + capR, pivot.y + 1.5f);
            dome.lineTo (pivot.x - capR, pivot.y + 1.5f);
            dome.closeSubPath();

            // Subtle cast shadow
            g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.45f : 0.22f));
            g.fillPath (dome, juce::AffineTransform::translation (0.0f, 1.0f));

            const juce::Colour topMetal = (darkCanvas || isRetroWarm)
                                              ? juce::Colour (0xff48464c)
                                              : PinkXP::shdw.interpolatedWith (juce::Colour (0xff5c5458), 0.55f);
            const juce::Colour botMetal = (darkCanvas || isRetroWarm)
                                              ? juce::Colour (0xff141317)
                                              : PinkXP::dark.interpolatedWith (juce::Colour (0xff241e22), 0.50f);

            juce::ColourGradient metalGrad (topMetal, pivot.x - capR * 0.35f, pivot.y - capR,
                                            botMetal, pivot.x + capR * 0.35f, pivot.y + 1.0f, false);
            g.setGradientFill (metalGrad);
            g.fillPath (dome);

            // Specular catchlight arc along upper-left rim
            juce::Path rimHl;
            rimHl.addCentredArc (pivot.x, pivot.y, juce::jmax (2.0f, capR - 1.2f), juce::jmax (2.0f, capR - 1.2f),
                                 0.0f, -1.25f, 0.35f, true);
            g.setColour (juce::Colours::white.withAlpha (0.34f));
            g.strokePath (rimHl, juce::PathStrokeType (1.0f));

            // Outer crisp hairline
            g.setColour (juce::Colours::black.withAlpha (0.65f));
            g.strokePath (dome, juce::PathStrokeType (1.0f));

            // Tiny polished center screw pin
            const float pinR = juce::jlimit (1.6f, 3.2f, capR * 0.21f);
            const float pinY = pivot.y - capR * 0.32f;
            juce::ColourGradient pinGrad (juce::Colour (0xffd8d8de), pivot.x - pinR * 0.5f, pinY - pinR * 0.5f,
                                          juce::Colour (0xff424248), pivot.x + pinR * 0.6f, pinY + pinR * 0.6f, false);
            g.setGradientFill (pinGrad);
            g.fillEllipse (pivot.x - pinR, pinY - pinR, pinR * 2.0f, pinR * 2.0f);
            return;
        }

        // Machined circular hub cap (Studio & Dual Arc)
        const float hubR = juce::jlimit (4.2f, 8.2f, dialRadius * 0.082f);

        // Soft drop shadow
        g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.52f : 0.26f));
        g.fillEllipse (pivot.x - hubR + 0.8f, pivot.y - hubR + 1.2f, hubR * 2.0f, hubR * 2.0f);

        // Outer chamfered metallic bezel
        const juce::Colour outerHi = darkCanvas ? juce::Colour (0xff787a84)
                                                : PinkXP::hl.interpolatedWith (juce::Colour (0xffd8d8e0), 0.45f);
        const juce::Colour outerLo = darkCanvas ? juce::Colour (0xff1a1b20)
                                                : PinkXP::dark.interpolatedWith (juce::Colour (0xff3a3842), 0.55f);
        juce::ColourGradient outerGrad (outerHi, pivot.x - hubR, pivot.y - hubR,
                                        outerLo, pivot.x + hubR, pivot.y + hubR, false);
        g.setGradientFill (outerGrad);
        g.fillEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f);

        // Inner recessed jewel cap
        const float innerR = hubR * 0.68f;
        const juce::Colour inHi = darkCanvas ? juce::Colour (0xff3a3c46)
                                             : PinkXP::pink300.interpolatedWith (juce::Colour (0xff6a6672), 0.35f);
        const juce::Colour inLo = darkCanvas ? juce::Colour (0xff101116)
                                             : PinkXP::pink600.darker (0.30f);
        juce::ColourGradient inGrad (inHi, pivot.x - innerR, pivot.y - innerR,
                                     inLo, pivot.x + innerR, pivot.y + innerR, false);
        g.setGradientFill (inGrad);
        g.fillEllipse (pivot.x - innerR, pivot.y - innerR, innerR * 2.0f, innerR * 2.0f);

        // Tiny specular jewel highlight dot
        const float dotR = juce::jmax (0.9f, hubR * 0.20f);
        g.setColour (juce::Colours::white.withAlpha (0.58f));
        g.fillEllipse (pivot.x - innerR * 0.45f - dotR * 0.5f,
                       pivot.y - innerR * 0.45f - dotR * 0.5f,
                       dotR * 2.0f, dotR * 2.0f);

        g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.65f : 0.42f));
        g.drawEllipse (pivot.x - hubR, pivot.y - hubR, hubR * 2.0f, hubR * 2.0f, 0.9f);
    }

    // Crisp unobtrusive digital dB mini-readout pill anchored cleanly at the bottom.
    inline void drawDbReadoutPill (juce::Graphics& g,
                                   float centreX,
                                   float topY,
                                   float maxWidth,
                                   float pillH,
                                   const juce::String& text,
                                   bool isHot,
                                   bool darkCanvas,
                                   juce::Colour alertCol,
                                   juce::Colour dotCol = juce::Colours::transparentBlack)
    {
        if (maxWidth < 26.0f || pillH < 10.0f)
            return;

        const auto& tokens = PinkXP::getSkinTokens();
        const float rawFontPx = juce::jlimit (7.5f, 9.5f, pillH * 0.64f);
        const float fontPx = (tokens.dialFinish == PinkXP::DialFinish::crtVector)
                                 ? juce::jmax (7.0f, rawFontPx * 0.90f)
                                 : rawFontPx;
        const bool hasDot  = dotCol.getAlpha() > 0;
        const float dotPad = hasDot ? 8.0f : 0.0f;
        const auto  pillFont = PinkXP::getAxisFont (fontPx, juce::Font::bold);
        const float textW  = juce::jmax (PinkXP::SymbolGlyphs::getTextWidth (text, pillFont),
                                         (float) text.length() * (fontPx * 0.64f));
        const float estW   = std::ceil (textW) + 14.0f + dotPad;
        const float w      = juce::jlimit (juce::jmin (44.0f, maxWidth), maxWidth, estW);
        const juce::Rectangle<float> pill (centreX - w * 0.5f, topY, w, pillH);

        if (tokens.isChromaKey)
        {
            juce::Rectangle<float> textArea = pill.reduced (2.0f, 0.0f);
            if (hasDot)
            {
                g.setColour (dotCol);
                if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware || ! tokens.statusDotCircular)
                {
                    const int dx = juce::roundToInt (pill.getX() + 4.0f);
                    const int dy = juce::roundToInt (pill.getCentreY()) - 2;
                    g.fillRect (dx, dy, 4, 4);
                }
                else
                {
                    const float dr = 2.3f;
                    g.fillEllipse (pill.getX() + 4.0f, pill.getCentreY() - dr, dr * 2.0f, dr * 2.0f);
                }
                textArea.removeFromLeft (7.0f);
            }
            const juce::Colour txtCol = isHot ? alertCol : PinkXP::ink;
            PinkXP::drawCachedText (g, text, textArea, juce::Justification::centred,
                                    fontPx, juce::Font::bold, txtCol, true);
            return;
        }

        const bool isNeon  = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
        const bool isGlass = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const bool isCrt   = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);

        const juce::Colour bgCol = (isNeon && darkCanvas)
                                       ? juce::Colour (0xff0a0d16).withAlpha (0.94f)
                                       : (darkCanvas
                                              ? juce::Colour (0xff0b0d11).withAlpha (0.92f)
                                              : PinkXP::content.interpolatedWith (PinkXP::ink, 0.06f).withAlpha (0.96f));
        const juce::Colour borderCol = isHot
                                           ? alertCol.withAlpha (0.92f)
                                           : (isNeon
                                                  ? PinkXP::sel.withAlpha (0.85f)
                                                  : (isCrt
                                                         ? PinkXP::sel.withAlpha (0.82f)
                                                         : (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                                                                ? PinkXP::skinHairlineOn (bgCol, darkCanvas ? 0.22f : 0.24f)
                                                                : (darkCanvas ? juce::Colours::white.withAlpha (0.20f)
                                                                              : PinkXP::ink.withAlpha (0.30f)))));

        if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware || isGlass || isNeon || isCrt || tokens.badgeRadius < 0.5f)
        {
            const auto pillI = pill.toNearestInt();
            PinkXP::Craft::drawBadgePill (g, pillI, bgCol, isHot ? alertCol : borderCol);

            if (tokens.dialFinish == PinkXP::DialFinish::brushedMetal && pillI.getWidth() >= 6 && pillI.getHeight() >= 6)
            {
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.24f : 0.62f));
                g.fillRect (pillI.getX() + 1, pillI.getY() + 1, pillI.getWidth() - 2, 1);
            }

            juce::Rectangle<int> textArea = pillI.reduced (3, 0);
            if (hasDot)
            {
                if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware || ! tokens.statusDotCircular)
                {
                    const int dsz = 4;
                    const int dx  = pillI.getX() + 4;
                    const int dy  = pillI.getCentreY() - dsz / 2;
                    g.setColour (dotCol);
                    g.fillRect (dx, dy, dsz, dsz);
                }
                else
                {
                    const float dr = 2.3f;
                    const float dx = (float) pillI.getX() + 5.5f;
                    const float dy = (float) pillI.getCentreY();
                    g.setColour (dotCol);
                    g.fillEllipse (dx - dr, dy - dr, dr * 2.0f, dr * 2.0f);
                    g.setColour (juce::Colours::white.withAlpha (0.42f));
                    g.drawEllipse (dx - dr, dy - dr, dr * 2.0f, dr * 2.0f, 0.65f);
                }
                textArea.removeFromLeft (7);
            }

            const juce::Colour txtCol = isHot ? alertCol : PinkXP::readableInkOn (bgCol);
            PinkXP::drawCachedText (g, text, textArea, juce::Justification::centred,
                                    fontPx, juce::Font::bold, txtCol, true);
            return;
        }

        g.setColour (bgCol);
        g.fillRoundedRectangle (pill, pillH * 0.44f);
        g.setColour (borderCol);
        g.drawRoundedRectangle (pill, pillH * 0.44f, 0.95f);

        juce::Rectangle<float> textArea = pill.reduced (3.0f, 0.0f);
        if (hasDot)
        {
            const float dr = 2.3f;
            const float dx = pill.getX() + 5.5f;
            const float dy = pill.getCentreY();
            g.setColour (dotCol);
            g.fillEllipse (dx - dr, dy - dr, dr * 2.0f, dr * 2.0f);
            textArea.removeFromLeft (7.5f);
        }

        const juce::Colour txtCol = isHot ? alertCol : PinkXP::readableInkOn (bgCol);
        PinkXP::drawCachedText (g, text, textArea, juce::Justification::centred,
                                fontPx, juce::Font::bold, txtCol, true);
    }
} // namespace VuNeedleDetail

// ============================================================================
// 1. Studio Dial (180 deg Classic Studio Semicircular Analog Meter)
// ============================================================================
void VuMeterModule::drawNeedleStudioDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    auto inner = dialArea.reduced (6);
    if (inner.getWidth() < 36 || inner.getHeight() < 28)
        return;

    const bool darkCanvas = VuNeedleDetail::isDarkCanvas();
    const float aStart = -juce::MathConstants<float>::halfPi;
    const float aEnd   = +juce::MathConstants<float>::halfPi;
    dialGeom.angleStart = aStart;
    dialGeom.angleEnd   = aEnd;

    // Studio Dial: ALWAYS one unified dial (coaxial dual needles in Stereo L/R, single needle + badge in L/R).
    const float readoutReserve = (showReadout_ && dialArea.getHeight() >= 76) ? 22.0f : 4.0f;
    const float minDb  = minDisplayDb();
    const float maxDb  = maxDisplayDb();
    const float aWarn  = needleAngleFor (warnDbfs);
    const auto  alertC = vuAlertColour();

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    auto drawOneStudioFace = [&] (juce::Rectangle<float> pane,
                                  float liveDb,
                                  const char* channelTag,
                                  juce::Point<float>& outPivot,
                                  float& outR,
                                  float& outNeedleLen) -> bool
    {
        const float topPad = 5.0f;
        const float sidePad = 6.0f;
        const float availW = pane.getWidth() - sidePad * 2.0f;
        const float availH = pane.getHeight() - topPad - readoutReserve;
        const float R = juce::jmin (availW * 0.5f - 3.0f, availH - 5.0f);
        if (R < 18.0f)
            return false;

        const float cx = pane.getCentreX();
        const float cy = pane.getY() + topPad + (availH - R) * 0.5f + R;
        const juce::Point<float> pivot { cx, cy };
        const float majorLen = juce::jlimit (4.5f, 9.5f, R * 0.092f);

        if (VuNeedleDetail::isChromaKey())
        {
            outPivot     = pivot;
            outR         = R;
            outNeedleLen = juce::jmax (10.0f, R - majorLen * 0.35f);
            return true;
        }

        if (VuNeedleDetail::isPixelHardware())
        {
            // ---- Pixel XP Studio Face: Win95 recessed bay + stepped Bayer backlight + clean anti-aliased dial arcs ----
            const int icx = juce::roundToInt (cx);
            const int icy = juce::roundToInt (cy);
            const int iR  = juce::roundToInt (R);
            const int pad = 4;
            const int lip = juce::jlimit (3, 5, juce::roundToInt (R * 0.05f));
            const juce::Rectangle<int> plateRect (icx - iR - pad, icy - iR - pad,
                                                  (iR + pad) * 2, iR + pad + lip);
            const juce::Colour plateBg = darkCanvas
                                             ? PinkXP::content.darker (0.28f)
                                             : PinkXP::content.interpolatedWith (PinkXP::pink50, 0.25f);
            PinkXP::drawSunken (g, plateRect, plateBg);

            const auto innerWell = plateRect.reduced (2);
            const float fcx = (float) icx;
            const float fcy = (float) icy;
            const float railW    = juce::jlimit (2.0f, 3.4f, R * 0.031f);
            const float rRail    = (float) iR - 2.6f;
            const float minorLen = majorLen * 0.52f;

            if (backlight_ && ! innerWell.isEmpty())
            {
                const juce::Colour lampTint = vuPaletteColour (0.68f, -6.0f, darkCanvas)
                                                  .interpolatedWith (juce::Colour (0xffffebd0), darkCanvas ? 0.24f : 0.42f);
                const juce::Colour topCol = plateBg.interpolatedWith (lampTint, darkCanvas ? 0.24f : 0.18f);
                PinkXP::Craft::drawPixelDitherBand (g, innerWell, topCol, plateBg, 3);

                if (liveDb >= warnDbfs && aEnd > aWarn + 0.02f)
                {
                    // Clean overload highlight strictly behind the red danger arc (zero top bezel bleed)
                    auto redArc = VuNeedleDetail::makeArcPath (fcx, fcy, rRail, aWarn, aEnd);
                    g.setColour (alertC.withAlpha (darkCanvas ? 0.26f : 0.18f));
                    g.strokePath (redArc, juce::PathStrokeType (railW + 5.5f, juce::PathStrokeType::curved,
                                                                juce::PathStrokeType::butt));
                }
            }

            const juce::Colour inkCol = PinkXP::readableInkOn (plateBg);

            // Clean anti-aliased outer hairline rail
            g.setColour (inkCol.withAlpha (darkCanvas ? 0.30f : 0.34f));
            g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, (float) iR, aStart, aEnd),
                          juce::PathStrokeType (0.9f));

            // Smooth graduated primary color rail up to warnDbfs
            const float safeEnd = juce::jmin (aEnd, aWarn);
            VuNeedleDetail::strokeGraduatedArc (
                g, fcx, fcy, rRail, aStart, aEnd, aStart, safeEnd, railW, 40,
                [&] (float t) { return vuPaletteColour (t, juce::jmap (t, minDb, maxDb), darkCanvas); });

            // Bold clean red overload arc past warnDbfs
            if (aEnd > aWarn + 0.005f)
            {
                g.setColour (alertC);
                g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rRail, aWarn, aEnd),
                              juce::PathStrokeType (railW + 1.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::butt));
            }

            // Delicate inner framing arc
            if (R >= 44.0f)
            {
                g.setColour (inkCol.withAlpha (darkCanvas ? 0.16f : 0.20f));
                g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, (float) iR - majorLen - 2.0f, aStart, aEnd),
                              juce::PathStrokeType (0.85f));
            }

            // Smooth anti-aliased radial ticks + crisp pixel numerals
            const float rTickOuter = (float) iR - 0.8f;
            const float fontPx     = juce::jlimit (7.2f, 9.2f, R * 0.094f);
            const float rLabel     = (float) iR - majorLen - juce::jlimit (7.5f, 13.0f, R * 0.135f);

            std::vector<VuNeedleDetail::TickLabelCandidate> labels;
            labels.reserve (ticks.size());

            for (const auto& t : ticks)
            {
                if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                    continue;

                const float ang  = needleAngleFor (t.db);
                const bool  hot  = t.db >= warnDbfs - 0.01f;
                const float tLen = t.major ? majorLen : minorLen;

                const auto p1 = VuNeedleDetail::arcPoint ({ fcx, fcy }, rTickOuter, ang);
                const auto p2 = VuNeedleDetail::arcPoint ({ fcx, fcy }, rTickOuter - tLen, ang);

                g.setColour (hot ? alertC : inkCol.withAlpha (t.major ? 0.90f : 0.48f));
                g.drawLine (p1.x, p1.y, p2.x, p2.y, t.major ? 1.5f : 0.95f);

                if (t.label != nullptr && rLabel > 16.0f)
                {
                    auto lp = VuNeedleDetail::arcPoint ({ fcx, fcy }, rLabel, ang);
                    lp.x = std::round (lp.x);
                    lp.y = std::round (juce::jmin (lp.y, fcy - fontPx * 0.62f));
                    labels.push_back ({
                        juce::String (t.label),
                        lp,
                        hot ? alertC : inkCol.withAlpha (0.90f),
                        VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                    });
                }
            }

            const juce::Rectangle<float> labelSafe (fcx - (float) iR + 4.0f, fcy - (float) iR + 4.0f,
                                                    ((float) iR - 4.0f) * 2.0f, (float) iR - 4.0f);
            VuNeedleDetail::drawNonOverlappingLabels (g, labels, labelSafe, fontPx);

            if (R >= 44.0f)
            {
                const float vuSize = juce::jlimit (10.0f, 15.0f, R * 0.17f);
                const int   vuY    = juce::roundToInt (fcy - (float) iR * 0.42f);
                PinkXP::drawCachedText (g, "VU",
                                        juce::Rectangle<int> (icx - 26, vuY - juce::roundToInt (vuSize * 0.6f), 52, juce::roundToInt (vuSize) + 3),
                                        juce::Justification::centred, vuSize, juce::Font::bold,
                                        inkCol.withAlpha (0.72f), true);

                if (R >= 60.0f)
                {
                    const float subSize = juce::jlimit (7.2f, 9.0f, R * 0.095f);
                    PinkXP::drawCachedText (g, "dBFS",
                                            juce::Rectangle<int> (icx - 24, vuY + juce::roundToInt (vuSize * 0.50f), 48, juce::roundToInt (subSize) + 3),
                                            juce::Justification::centred, subSize, juce::Font::bold,
                                            inkCol.withAlpha (0.48f), true);
                }
            }

            if (channelTag != nullptr)
                VuNeedleDetail::drawChannelBadge (g, pane, channelTag,
                                                  vuPaletteColour (0.72f, -6.0f, darkCanvas), darkCanvas);

            outPivot     = { fcx, fcy };
            outR         = (float) iR;
            outNeedleLen = juce::jmax (10.0f, (float) iR - majorLen * 0.35f);
            return true;
        }

        // ---- Smooth, Chrome, Glass, Neon & CRT skins: Recessed dial plate ----
        const bool isChrome = VuNeedleDetail::isBrushedMetal();
        const bool isGlass  = VuNeedleDetail::isGlassDome();
        const bool isNeon   = VuNeedleDetail::isNeonRings();
        const bool isCrt    = VuNeedleDetail::isCrtVector();
        const bool faceDark = darkCanvas;

        const float plateR = R + 4.0f;
        const float lipDrop = juce::jlimit (2.5f, 4.5f, R * 0.045f);
        juce::Path plate;
        plate.addCentredArc (cx, cy, plateR, plateR, 0.0f, aStart, aEnd, true);
        plate.lineTo (cx + plateR, cy + lipDrop);
        plate.lineTo (cx - plateR, cy + lipDrop);
        plate.closeSubPath();

        juce::Colour inkCol = PinkXP::ink;
        const int icx  = juce::roundToInt (cx);
        const int icy  = juce::roundToInt (cy);
        const int iR   = juce::roundToInt (plateR);
        const int iLip = juce::jmax (3, juce::roundToInt (lipDrop) + 1);
        const juce::Rectangle<int> bayRect (icx - iR - 2, icy - iR - 2,
                                            (iR + 2) * 2, iR + 2 + iLip);

        if (isChrome)
        {
            // CNC milled rectangular brushed-metal instrument bay + engraved inner semicircle rim
            const juce::Colour bayBg = darkCanvas
                                           ? PinkXP::content.interpolatedWith (juce::Colour (0xff1c202a), 0.45f)
                                           : PinkXP::content.interpolatedWith (juce::Colour (0xffe8edf4), 0.40f);
            VuNeedleDetail::drawMachinedPlateBezel (g, bayRect, bayBg, darkCanvas);

            const auto bayIn = bayRect.reduced (2);
            if (backlight_ && ! bayIn.isEmpty())
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (bayIn);

                const juce::Colour lampTint = vuPaletteColour (0.70f, -6.0f, darkCanvas)
                                                  .interpolatedWith (juce::Colour (0xffdceeff), darkCanvas ? 0.20f : 0.32f);
                juce::ColourGradient lampGrad (lampTint.withAlpha (darkCanvas ? 0.22f : 0.14f),
                                               cx, cy - R * 0.12f,
                                               lampTint.withAlpha (0.0f),
                                               cx, cy - plateR * 1.08f, true);
                g.setGradientFill (lampGrad);
                g.fillRect (bayIn);

                if (liveDb >= warnDbfs && aEnd > aWarn + 0.02f)
                {
                    const auto redCentre = VuNeedleDetail::arcPoint (pivot, R * 0.72f, 0.5f * (aWarn + aEnd));
                    juce::ColourGradient redGlow (alertC.withAlpha (darkCanvas ? 0.24f : 0.16f),
                                                  redCentre.x, redCentre.y,
                                                  alertC.withAlpha (0.0f),
                                                  redCentre.x + R * 0.48f, redCentre.y, true);
                    g.setGradientFill (redGlow);
                    g.fillRect (bayIn);
                }
            }

            // Crisp machined semicircular chrome bezel engraving inside bay
            g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.22f)
                                    : PinkXP::shdw.withAlpha (0.48f));
            g.strokePath (plate, juce::PathStrokeType (1.0f));
        }
        else if (isGlass)
        {
            // Frosted Glass: rounded 6px acrylic instrument bay + convex glass dome reflection across upper ~38%
            const juce::Colour glassBg = darkCanvas
                                             ? PinkXP::content.interpolatedWith (juce::Colour (0xff1a2234), 0.48f)
                                             : PinkXP::content.interpolatedWith (juce::Colours::white, 0.38f);
            PinkXP::Craft::drawPanelWell (g, bayRect, glassBg, PinkXP::WellKind::card);
            inkCol = PinkXP::readableInkOn (glassBg);

            const auto bayF = bayRect.toFloat().reduced (1.5f);
            juce::Path glassClip;
            glassClip.addRoundedRectangle (bayF, 5.0f);
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (glassClip);

                if (backlight_)
                {
                    const juce::Colour lampTint = vuPaletteColour (0.68f, -6.0f, darkCanvas);
                    juce::ColourGradient lampGrad (lampTint.withAlpha (darkCanvas ? 0.22f : 0.15f),
                                                   cx, cy - R * 0.12f,
                                                   lampTint.withAlpha (0.0f),
                                                   cx, cy - plateR * 1.06f, true);
                    g.setGradientFill (lampGrad);
                    g.fillRect (bayF);
                }

                VuNeedleDetail::drawGlassDomeReflection (g, bayF, darkCanvas);
            }

            // Delicate semicircular inner crystal rim
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.22f : 0.62f));
            g.strokePath (plate, juce::PathStrokeType (1.0f));
        }
        else if (isNeon)
        {
            // Neon Cyber: theme-respecting HUD bay framed by double-pass glowing neon tube bezel
            const juce::Colour neonBg = VuNeedleDetail::neonSurfaceBg (darkCanvas);
            VuNeedleDetail::drawNeonDoubleBezel (g, bayRect.toFloat(), PinkXP::sel, neonBg);
            inkCol = PinkXP::readableInkOn (neonBg);

            if (backlight_)
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (bayRect.reduced (2));
                const juce::Colour aura = PinkXP::sel;
                juce::ColourGradient gAura (aura.withAlpha (darkCanvas ? 0.20f : 0.13f), cx, cy,
                                            aura.withAlpha (0.0f),  cx, cy - plateR * 1.05f, true);
                g.setGradientFill (gAura);
                g.fillRect (bayRect.reduced (2));
            }

            g.setColour (PinkXP::sel.withAlpha (darkCanvas ? 0.34f : 0.42f));
            g.strokePath (plate, juce::PathStrokeType (1.1f));
        }
        else if (isCrt)
        {
            // CRT Terminal: 0px square Tektronix oscilloscope bay + subtle crosshair reticle
            const juce::Colour tubeBg = darkCanvas ? PinkXP::content.darker (0.28f)
                                                   : PinkXP::content.interpolatedWith (PinkXP::sel, 0.05f);
            const juce::Colour phosC  = darkCanvas ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.20f)
                                                   : PinkXP::sel.darker (0.22f);
            VuNeedleDetail::drawCrtTektronixBezel (g, bayRect, phosC, tubeBg, true);
            inkCol = PinkXP::readableInkOn (tubeBg);

            g.setColour (phosC.withAlpha (0.36f));
            g.strokePath (plate, juce::PathStrokeType (0.95f));
        }
        else
        {
            const juce::Colour centerCol = darkCanvas
                                               ? PinkXP::content.interpolatedWith (juce::Colour (0xff262832), 0.35f)
                                               : PinkXP::content.brighter (0.14f);
            const juce::Colour edgeCol   = darkCanvas
                                               ? PinkXP::content.darker (0.42f)
                                               : PinkXP::content.interpolatedWith (PinkXP::pink200, 0.24f);
            juce::ColourGradient plateGrad (centerCol, cx, cy - R * 0.15f,
                                            edgeCol,   cx, cy - plateR, true);
            g.setGradientFill (plateGrad);
            g.fillPath (plate);

            // ---- Backlight radial lamp glow + upper glass crescent ----
            if (backlight_)
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (plate);

                const juce::Colour lampTint = vuPaletteColour (0.70f, -6.0f, darkCanvas)
                                                  .interpolatedWith (juce::Colour (0xffffebd0), darkCanvas ? 0.26f : 0.38f);
                juce::ColourGradient lampGrad (lampTint.withAlpha (darkCanvas ? 0.24f : 0.17f),
                                               cx, cy - R * 0.10f,
                                               lampTint.withAlpha (0.0f),
                                               cx, cy - plateR * 1.06f, true);
                lampGrad.addColour (0.55, lampTint.withAlpha (darkCanvas ? 0.10f : 0.07f));
                g.setGradientFill (lampGrad);
                g.fillPath (plate);

                // Live overload warm blush when clipping
                if (liveDb >= warnDbfs && aEnd > aWarn + 0.02f)
                {
                    const auto redCentre = VuNeedleDetail::arcPoint (pivot, R * 0.72f, 0.5f * (aWarn + aEnd));
                    juce::ColourGradient redGlow (alertC.withAlpha (darkCanvas ? 0.24f : 0.16f),
                                                  redCentre.x, redCentre.y,
                                                  alertC.withAlpha (0.0f),
                                                  redCentre.x + R * 0.48f, redCentre.y, true);
                    g.setGradientFill (redGlow);
                    g.fillPath (plate);
                }

                // Subtle curved glass highlight crescent along top-left arc
                const float rGlare = plateR * 0.91f;
                auto glareArc = VuNeedleDetail::makeArcPath (cx, cy, rGlare, -1.28f, -0.14f);
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.09f : 0.34f));
                g.strokePath (glareArc, juce::PathStrokeType (juce::jlimit (1.6f, 3.2f, R * 0.032f),
                                                              juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
            }

            // Subtle inner recessed rim border on the plate
            g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.14f)
                                    : PinkXP::ink.withAlpha (0.24f));
            g.strokePath (plate, juce::PathStrokeType (1.25f));
        }

        // ---- Double-rail arc scale ----
        const float minorLen = majorLen * 0.52f;
        const float railW    = juce::jlimit (1.9f, 3.5f, R * 0.031f);
        const float rRail    = R - 2.2f;

        // Outer hairline rail
        g.setColour (inkCol.withAlpha (faceDark ? 0.28f : 0.32f));
        g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, R, aStart, aEnd),
                      juce::PathStrokeType (0.85f));

        // Primary colored arc rail (smooth palette up to warnDbfs; glowing underlay in Neon/CRT)
        const float safeEnd = juce::jmin (aEnd, aWarn);
        if ((isNeon || isCrt) && ! VuNeedleDetail::isGreenScreen())
        {
            VuNeedleDetail::strokeGraduatedArc (
                g, cx, cy, rRail, aStart, aEnd, aStart, safeEnd, railW + (isNeon ? 4.4f : 2.8f), 32,
                [&] (float t) { return vuPaletteColour (t, juce::jmap (t, minDb, maxDb), faceDark).withAlpha (isNeon ? 0.28f : 0.20f); });
        }
        VuNeedleDetail::strokeGraduatedArc (
            g, cx, cy, rRail, aStart, aEnd, aStart, safeEnd, railW, 40,
            [&] (float t)
            {
                auto c = vuPaletteColour (t, juce::jmap (t, minDb, maxDb), faceDark);
                return isNeon ? c.interpolatedWith (juce::Colours::white, 0.24f) : c;
            });

        // Glowing overload arc past warnDbfs
        if (aEnd > aWarn + 0.005f)
        {
            auto redArc = VuNeedleDetail::makeArcPath (cx, cy, rRail, aWarn, aEnd);
            g.setColour (alertC.withAlpha (darkCanvas ? 0.28f : 0.16f));
            g.strokePath (redArc, juce::PathStrokeType (railW + 4.2f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::butt));
            g.setColour (alertC);
            g.strokePath (redArc, juce::PathStrokeType (railW + 1.1f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::butt));
        }

        // Delicate inner hairline framing tick roots
        if (R >= 44.0f)
        {
            g.setColour (inkCol.withAlpha (darkCanvas ? 0.12f : 0.15f));
            g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, R - majorLen - 2.0f, aStart, aEnd),
                          juce::PathStrokeType (0.75f));
        }

        // ---- Precision major/minor radial ticks + anti-collision dB labels ----
        const float rTickOuter = R - 0.8f;
        const float fontPx     = juce::jlimit (7.2f, 9.4f, R * 0.095f);
        const float rLabel     = R - majorLen - juce::jlimit (7.5f, 13.0f, R * 0.135f);

        std::vector<VuNeedleDetail::TickLabelCandidate> labels;
        labels.reserve (ticks.size());

        for (const auto& t : ticks)
        {
            if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                continue;

            const float ang  = needleAngleFor (t.db);
            const bool  hot  = t.db >= warnDbfs - 0.01f;
            const float tLen = t.major ? majorLen : minorLen;

            const auto p1 = VuNeedleDetail::arcPoint (pivot, rTickOuter, ang);
            const auto p2 = VuNeedleDetail::arcPoint (pivot, rTickOuter - tLen, ang);

            g.setColour (hot ? alertC : inkCol.withAlpha (t.major ? 0.88f : 0.45f));
            g.drawLine (p1.x, p1.y, p2.x, p2.y, t.major ? 1.55f : 0.95f);

            if (t.label != nullptr && rLabel > 16.0f)
            {
                auto lp = VuNeedleDetail::arcPoint (pivot, rLabel, ang);
                // Keep endpoint labels slightly lifted above horizontal base line
                lp.y = juce::jmin (lp.y, cy - fontPx * 0.62f);
                labels.push_back ({
                    juce::String (t.label),
                    lp,
                    hot ? alertC : inkCol.withAlpha (0.86f),
                    VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                });
            }
        }

        const juce::Rectangle<float> labelSafe (cx - R + 4.0f, cy - R + 4.0f,
                                                (R - 4.0f) * 2.0f, R - 4.0f);
        VuNeedleDetail::drawNonOverlappingLabels (g, labels, labelSafe, fontPx);

        // ---- Subtle "VU" / "dBFS" studio markings above pivot ----
        if (R >= 44.0f)
        {
            const float vuSize = juce::jlimit (10.0f, 16.0f, R * 0.175f);
            const float vuY    = cy - R * 0.42f;
            PinkXP::drawCachedText (g, "VU",
                                    juce::Rectangle<float> (cx - 26.0f, vuY - vuSize * 0.6f, 52.0f, vuSize + 3.0f),
                                    juce::Justification::centred, vuSize, juce::Font::bold,
                                    inkCol.withAlpha (darkCanvas ? 0.62f : 0.68f), true);

            if (R >= 60.0f)
            {
                const float subSize = juce::jlimit (7.2f, 9.0f, R * 0.095f);
                PinkXP::drawCachedText (g, "dBFS",
                                        juce::Rectangle<float> (cx - 24.0f, vuY + vuSize * 0.50f, 48.0f, subSize + 3.0f),
                                        juce::Justification::centred, subSize, juce::Font::bold,
                                        inkCol.withAlpha (0.42f), true);
            }
        }

        if (channelTag != nullptr)
            VuNeedleDetail::drawChannelBadge (g, pane, channelTag,
                                              vuPaletteColour (0.72f, -6.0f, darkCanvas), darkCanvas);

        outPivot     = pivot;
        outR         = R;
        outNeedleLen = juce::jmax (10.0f, R - majorLen * 0.35f);
        return true;
    };

    {
        juce::Point<float> piv;
        float R = 0.0f, nLen = 0.0f;
        const float liveDb = activeSingleDbfs();
        if (drawOneStudioFace (inner.toFloat(), liveDb, activeChannelTag(), piv, R, nLen))
        {
            dialGeom.valid     = true;
            dialGeom.dualSide  = false;
            dialGeom.pivot     = piv;
            dialGeom.radius    = R;
            dialGeom.needleLen = nLen;
            dialGeom.tailLen   = juce::jlimit (3.5f, 7.0f, R * 0.072f);
            dialGeom.capR      = 0.0f;
        }
    }
}

// ============================================================================
// 2. Vintage Dial (+/-47 deg Narrow-Sector Console Hardware VU Meter)
// ============================================================================
void VuMeterModule::drawNeedleVintageDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    auto inner = dialArea.reduced (6);
    if (inner.getWidth() < 44 || inner.getHeight() < 34)
        return;

    const bool darkCanvas = VuNeedleDetail::isDarkCanvas();
    constexpr float halfA = 0.82f; // ~47 deg
    dialGeom.angleStart = -halfA;
    dialGeom.angleEnd   = +halfA;

    const float readoutReserve = (showReadout_ && dialArea.getHeight() >= 76) ? 18.0f : 4.0f;
    const float minDb  = minDisplayDb();
    const float maxDb  = maxDisplayDb();
    const float aWarn  = needleAngleFor (warnDbfs);
    const auto  alertC = vuAlertColour();

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    auto drawOneVintageFace = [&] (juce::Rectangle<float> pane,
                                   float liveDb,
                                   const char* channelTag,
                                   juce::Point<float>& outPivot,
                                   float& outR,
                                   float& outNeedleLen,
                                   float& outCapR) -> bool
    {
        const float availW = pane.getWidth() - 10.0f;
        const float availH = pane.getHeight() - readoutReserve - 6.0f;
        if (availW < 36.0f || availH < 28.0f)
            return false;

        // Solve fan radius from width (pie wings at +/- (halfA + pad)) and height
        const float maxWingSin = std::sin (halfA + 0.19f); // ~0.847
        const float R = 0.93f * juce::jmin ((availW * 0.5f - 8.0f) / maxWingSin,
                                             availH - 11.0f);
        if (R < 26.0f)
            return false;

        const float rimExtra = juce::jlimit (7.0f, 11.0f, R * 0.11f);
        const float Rw       = R + rimExtra;
        const float capR     = juce::jlimit (6.5f, 15.5f, R * 0.135f);
        const float pad      = juce::jlimit (0.12f, 0.21f, 14.0f / R);

        const float cx = pane.getCentreX();
        const float cy = pane.getY() + 3.0f + (availH - Rw) * 0.5f + Rw;
        const juce::Point<float> pivot { cx, cy };

        const float fontPx   = juce::jlimit (7.2f, 9.2f, R * 0.095f);
        const float rLabel   = R - 0.5f;
        const float tickMaj  = juce::jlimit (4.2f, 7.0f, R * 0.070f);
        const float tickMin  = tickMaj * 0.50f;
        const float rTick0   = rLabel - fontPx * 0.65f - tickMaj - 2.0f;

        if (VuNeedleDetail::isChromaKey())
        {
            outPivot     = pivot;
            outR         = R;
            outNeedleLen = juce::jmax (12.0f, rTick0 + tickMaj * 0.65f);
            outCapR      = capR;
            return true;
        }

        if (VuNeedleDetail::isPixelHardware())
        {
            // ---- Pixel XP Vintage Fan Window: Win95 sunken rectangular bezel + Bayer LCD well + clean anti-aliased fan arcs ----
            const int icx = juce::roundToInt (cx);
            const int icy = juce::roundToInt (cy);
            const float fcx = (float) icx;
            const float fcy = (float) icy;

            const int halfWinW = juce::jmin (juce::roundToInt (pane.getWidth() * 0.5f) - 2,
                                             juce::roundToInt (Rw * std::sin (halfA + pad)) + 4);
            const int winTop   = juce::jmax (juce::roundToInt (pane.getY()) + 1,
                                             icy - juce::roundToInt (Rw) - 2);
            const int winBot   = juce::jmin (juce::roundToInt (pane.getBottom() - readoutReserve) + 1,
                                             icy + 2);
            const juce::Rectangle<int> winRect (icx - halfWinW, winTop,
                                                juce::jmax (12, halfWinW * 2),
                                                juce::jmax (12, winBot - winTop));

            const bool warmTint = (colorMap_ == ColorMap::warmVu);
            const juce::Colour lcdBg = darkCanvas
                                           ? (warmTint ? juce::Colour (0xff271e13)
                                                       : PinkXP::content.interpolatedWith (juce::Colour (0xff24282e), 0.38f))
                                           : (warmTint ? juce::Colour (0xfff6ebd0)
                                                       : PinkXP::content.interpolatedWith (juce::Colour (0xfff9f5e4), 0.42f));
            PinkXP::drawSunken (g, winRect, lcdBg);

            const auto winInner = winRect.reduced (2);
            if (! winInner.isEmpty())
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (winInner);

                if (backlight_)
                {
                    const juce::Colour lampTint = vuPaletteColour (0.65f, -6.0f, darkCanvas)
                                                      .interpolatedWith (juce::Colour (0xffffd68a),
                                                                         warmTint ? 0.68f : 0.40f);
                    const juce::Colour topBand = lcdBg.interpolatedWith (lampTint, darkCanvas ? 0.24f : 0.22f);
                    PinkXP::Craft::drawPixelDitherBand (g, winInner, topBand, lcdBg, 4);

                    if (liveDb >= warnDbfs && halfA > aWarn + 0.01f)
                    {
                        auto redArc = VuNeedleDetail::makeArcPath (fcx, fcy, R * 0.80f, aWarn, halfA);
                        g.setColour (alertC.withAlpha (darkCanvas ? 0.24f : 0.16f));
                        g.strokePath (redArc, juce::PathStrokeType (juce::jmax (4.0f, R * 0.08f),
                                                                    juce::PathStrokeType::curved,
                                                                    juce::PathStrokeType::butt));
                    }
                }

                // Clean anti-aliased fan mask wing lines framing +/- (halfA + pad)
                const auto wingCol = PinkXP::readableInkOn (lcdBg).withAlpha (0.22f);
                g.setColour (wingCol);
                for (float wingA : { -halfA - pad * 0.65f, +halfA + pad * 0.65f })
                {
                    const auto wp0 = VuNeedleDetail::arcPoint ({ fcx, fcy }, capR + 2.0f, wingA);
                    const auto wp1 = VuNeedleDetail::arcPoint ({ fcx, fcy }, Rw, wingA);
                    g.drawLine (wp0.x, wp0.y, wp1.x, wp1.y, 1.0f);
                }

                const juce::Colour inkCol = PinkXP::readableInkOn (lcdBg);
                const float railW = juce::jlimit (2.6f, 5.2f, R * 0.050f);
                const float rRail = rTick0 - railW * 0.5f;
                const float rSub  = rRail - railW * 0.5f - juce::jlimit (5.5f, 10.5f, R * 0.092f);

                // Smooth graduated main scale arc
                const float safeEnd = juce::jmin (halfA, aWarn);
                VuNeedleDetail::strokeGraduatedArc (
                    g, fcx, fcy, rRail, -halfA, halfA, -halfA, safeEnd, railW, 34,
                    [&] (float t)
                    {
                        const float db = juce::jmap (t, minDb, maxDb);
                        return (colorMap_ == ColorMap::theme)
                                   ? inkCol.interpolatedWith (vuPaletteColour (t, db, darkCanvas), 0.68f)
                                   : vuPaletteColour (t, db, darkCanvas);
                    });

                // Bold clean red overload block past warnDbfs
                if (halfA > aWarn + 0.005f)
                {
                    g.setColour (alertC);
                    g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rRail, aWarn, halfA),
                                  juce::PathStrokeType (railW + 1.4f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::butt));
                }

                // Clean anti-aliased radial ticks + crisp pixel numerals
                std::vector<VuNeedleDetail::TickLabelCandidate> labels;
                labels.reserve (ticks.size());

                for (const auto& t : ticks)
                {
                    if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                        continue;

                    const float ang = needleAngleFor (t.db);
                    const bool  hot = t.db >= warnDbfs - 0.01f;
                    const auto  p0  = VuNeedleDetail::arcPoint ({ fcx, fcy }, rTick0, ang);
                    const auto  p1  = VuNeedleDetail::arcPoint ({ fcx, fcy }, rTick0 + (t.major ? tickMaj : tickMin), ang);

                    g.setColour (hot ? alertC : inkCol.withAlpha (t.major ? 0.90f : 0.50f));
                    g.drawLine (p0.x, p0.y, p1.x, p1.y, t.major ? 1.45f : 0.95f);

                    if (t.label != nullptr)
                    {
                        auto lp = VuNeedleDetail::arcPoint ({ fcx, fcy }, rLabel, ang);
                        lp.x = std::round (lp.x);
                        lp.y = std::round (lp.y);
                        labels.push_back ({
                            juce::String (t.label),
                            lp,
                            hot ? alertC : inkCol.withAlpha (0.90f),
                            VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                        });
                    }
                }

                VuNeedleDetail::drawNonOverlappingLabels (g, labels, winInner.toFloat().reduced (2.0f), fontPx);

                // Smooth inner % modulation sub-scale with crisp square 2x2/3x3 pips
                if (rSub > capR + 12.0f)
                {
                    const float aZero = needleAngleFor (juce::jlimit (minDb, maxDb, 0.0f));
                    g.setColour (inkCol.withAlpha (0.50f));
                    g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rSub, -halfA, juce::jmin (halfA, aZero)),
                                  juce::PathStrokeType (1.0f));
                    if (halfA > aZero + 0.005f)
                    {
                        g.setColour (alertC.withAlpha (0.90f));
                        g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rSub, aZero, halfA),
                                      juce::PathStrokeType (1.05f));
                    }

                    const bool withPctText = (R >= 74.0f && rSub - 13.0f > capR + 14.0f);
                    for (int p = 0; p <= 100; p += 10)
                    {
                        const float pctDb = (p == 0) ? minDb : 20.0f * std::log10 ((float) p / 100.0f);
                        if (pctDb < minDb - 0.1f || pctDb > maxDb + 0.1f)
                            continue;

                        const float ang = needleAngleFor (pctDb);
                        const auto  dp  = VuNeedleDetail::arcPoint ({ fcx, fcy }, rSub - 3.5f, ang);
                        const int   bsz = (p % 50 == 0) ? 3 : 2;
                        const bool  hot = pctDb >= warnDbfs - 0.01f;
                        g.setColour (hot ? alertC : inkCol.withAlpha (p % 50 == 0 ? 0.80f : 0.50f));
                        g.fillRect (juce::roundToInt (dp.x) - bsz / 2,
                                    juce::roundToInt (dp.y) - bsz / 2, bsz, bsz);

                        if (withPctText && (p == 0 || p == 50 || p == 100))
                        {
                            const auto tp = VuNeedleDetail::arcPoint ({ fcx, fcy }, rSub - 11.5f, ang);
                            const juce::String pctTxt = (p == 100) ? juce::String ("100%") : juce::String (p);
                            PinkXP::drawCachedText (g, pctTxt,
                                                    juce::Rectangle<int> (juce::roundToInt (tp.x) - 14,
                                                                          juce::roundToInt (tp.y) - 5, 28, 10),
                                                    juce::Justification::centred, 7.2f, juce::Font::plain,
                                                    inkCol.withAlpha (0.65f), true);
                        }
                    }
                }

                // Clean "VU" / "dBFS" emblem centered inside fan
                const float emblemTopLimit = (rSub > capR + 12.0f) ? (rSub - (R >= 74.0f ? 19.0f : 7.0f)) : (rRail - 6.0f);
                const float emblemSpan     = emblemTopLimit - (capR + 4.0f);
                if (emblemSpan >= 16.0f)
                {
                    const float midR   = capR + 4.0f + emblemSpan * 0.50f;
                    const float vuSize = juce::jlimit (10.0f, 15.0f, emblemSpan * 0.46f);
                    const int   my     = juce::roundToInt (fcy - midR);
                    const bool  showUnit = emblemSpan >= 28.0f;

                    PinkXP::drawCachedText (g, "VU",
                                            juce::Rectangle<int> (icx - 24,
                                                                  my - juce::roundToInt (showUnit ? vuSize * 0.78f : vuSize * 0.55f),
                                                                  48, juce::roundToInt (vuSize) + 3),
                                            juce::Justification::centred, vuSize, juce::Font::bold,
                                            inkCol.withAlpha (0.80f), true);
                    if (showUnit)
                    {
                        PinkXP::drawCachedText (g, "dBFS",
                                                juce::Rectangle<int> (icx - 22, my + juce::roundToInt (vuSize * 0.28f), 44, 10),
                                                juce::Justification::centred, 7.5f, juce::Font::bold,
                                                inkCol.withAlpha (0.50f), true);
                    }
                }
            }

            if (channelTag != nullptr)
                VuNeedleDetail::drawChannelBadge (g, pane, channelTag,
                                                  vuPaletteColour (0.68f, -6.0f, darkCanvas), darkCanvas);

            outPivot     = { fcx, fcy };
            outR         = R;
            outNeedleLen = juce::jmax (12.0f, rTick0 + tickMaj * 0.65f);
            outCapR      = capR;
            return true;
        }

        // ---- Smooth, Chrome, Glass, Neon & CRT skins: Framed pie-window bezel ----
        const bool isChrome = VuNeedleDetail::isBrushedMetal();
        const bool isGlass  = VuNeedleDetail::isGlassDome();
        const bool isNeon   = VuNeedleDetail::isNeonRings();
        const bool isCrt    = VuNeedleDetail::isCrtVector();
        const bool faceDark = darkCanvas;

        const float innerRatio = juce::jlimit (0.05f, 0.45f, (capR + 1.5f) / Rw);
        juce::Path win;
        win.addPieSegment (cx - Rw, cy - Rw, Rw * 2.0f, Rw * 2.0f,
                           -halfA - pad, +halfA + pad, innerRatio);

        const int halfWinW = juce::jmin (juce::roundToInt (pane.getWidth() * 0.5f) - 2,
                                         juce::roundToInt (Rw * std::sin (halfA + pad)) + 5);
        const int winTop   = juce::jmax (juce::roundToInt (pane.getY()) + 1,
                                         juce::roundToInt (cy - Rw) - 3);
        const int winBot   = juce::jmin (juce::roundToInt (pane.getBottom() - readoutReserve) + 1,
                                         juce::roundToInt (cy) + 2);
        const juce::Rectangle<int> frameRect (juce::roundToInt (cx) - halfWinW, winTop,
                                              juce::jmax (16, halfWinW * 2),
                                              juce::jmax (16, winBot - winTop));

        if (isChrome)
        {
            const juce::Colour metalBg = darkCanvas
                                             ? PinkXP::face.interpolatedWith (juce::Colour (0xff1d2028), 0.55f)
                                             : PinkXP::face.interpolatedWith (juce::Colour (0xffdce2ec), 0.35f);
            VuNeedleDetail::drawMachinedPlateBezel (g, frameRect, metalBg, darkCanvas);
        }
        else if (isGlass)
        {
            const juce::Colour glassFrame = darkCanvas
                                                ? PinkXP::face.interpolatedWith (juce::Colour (0xff1a2234), 0.50f)
                                                : PinkXP::face.interpolatedWith (juce::Colours::white, 0.42f);
            PinkXP::Craft::drawPanelWell (g, frameRect, glassFrame, PinkXP::WellKind::card);
        }
        else if (isNeon)
        {
            VuNeedleDetail::drawNeonDoubleBezel (g, frameRect.toFloat(), PinkXP::sel, VuNeedleDetail::neonSurfaceBg (darkCanvas));
        }
        else if (isCrt)
        {
            const juce::Colour tubeBg = darkCanvas ? PinkXP::content.darker (0.28f)
                                                   : PinkXP::content.interpolatedWith (PinkXP::sel, 0.05f);
            const juce::Colour phosC  = darkCanvas ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.20f)
                                                   : PinkXP::sel.darker (0.22f);
            VuNeedleDetail::drawCrtTektronixBezel (g, frameRect, phosC, tubeBg, true);
        }

        // Outer recessed bezel border halo
        g.setColour (faceDark ? juce::Colours::black.withAlpha (0.55f)
                              : PinkXP::ink.withAlpha (0.18f));
        g.strokePath (win, juce::PathStrokeType (3.2f, juce::PathStrokeType::mitered,
                                                 juce::PathStrokeType::butt));

        // Dial face background plate (subtle warm vintage tint on warmVu; smoked dark in Neon; phosphor tube in CRT)
        juce::Colour inkCol = PinkXP::ink;
        {
            const bool warmTint = (colorMap_ == ColorMap::warmVu);
            juce::Colour faceHi, faceLo;
            if (isNeon)
            {
                faceHi = darkCanvas
                             ? juce::Colour (0xff101424).interpolatedWith (PinkXP::sel, backlight_ ? 0.14f : 0.05f)
                             : PinkXP::content.interpolatedWith (PinkXP::sel, backlight_ ? 0.08f : 0.03f);
                faceLo = darkCanvas
                             ? juce::Colour (0xff070910)
                             : PinkXP::content.interpolatedWith (PinkXP::pink200, 0.22f);
                inkCol = PinkXP::readableInkOn (faceHi);
            }
            else if (isCrt)
            {
                faceHi = darkCanvas ? PinkXP::content.darker (0.24f)
                                    : PinkXP::content.interpolatedWith (PinkXP::sel, 0.05f);
                faceLo = darkCanvas ? PinkXP::content.darker (0.38f)
                                    : PinkXP::content.interpolatedWith (PinkXP::sel, 0.10f);
                inkCol = PinkXP::readableInkOn (faceHi);
            }
            else
            {
                faceHi = darkCanvas
                             ? (warmTint ? juce::Colour (0xff2b2115)
                                         : PinkXP::content.interpolatedWith (juce::Colour (0xff252730), 0.35f))
                             : (warmTint ? juce::Colour (0xfffaf0d4)
                                         : PinkXP::content.interpolatedWith (juce::Colour (0xfffffaf0), 0.35f));
                faceLo = darkCanvas
                             ? PinkXP::content.darker (0.45f)
                             : (warmTint ? juce::Colour (0xffe5d3ab)
                                         : PinkXP::content.interpolatedWith (PinkXP::pink200, 0.20f));
                inkCol = PinkXP::readableInkOn (faceHi);
            }
            juce::ColourGradient faceGrad (faceHi, cx, cy - R * 0.35f,
                                           faceLo, cx, cy - Rw, true);
            g.setGradientFill (faceGrad);
            g.fillPath (win);
        }

        // ---- Vintage lamp backlight + recessed inner window shadow + red-zone blush ----
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (win);

            if (isChrome)
                VuNeedleDetail::drawBrushedSheenOverlay (g, win.getBounds(), darkCanvas);
            else if (isGlass)
                VuNeedleDetail::drawGlassDomeReflection (g, win.getBounds(), darkCanvas);

            if (backlight_)
            {
                const juce::Colour lampCol = vuPaletteColour (0.65f, -6.0f, faceDark)
                                                 .interpolatedWith (juce::Colour (0xffffd68a),
                                                                    colorMap_ == ColorMap::warmVu ? 0.65f : 0.36f);
                juce::ColourGradient lampGrad (lampCol.withAlpha (faceDark ? 0.25f : 0.20f),
                                               cx, cy - R * 0.26f,
                                               lampCol.withAlpha (0.0f),
                                               cx, cy - Rw * 1.04f, true);
                lampGrad.addColour (0.52, lampCol.withAlpha (faceDark ? 0.11f : 0.08f));
                g.setGradientFill (lampGrad);
                g.fillPath (win);

                // Warm red blush in the red overload zone when live signal >= warnDbfs
                if (liveDb >= warnDbfs && halfA > aWarn + 0.01f)
                {
                    const auto redSpot = VuNeedleDetail::arcPoint (pivot, R * 0.78f, 0.5f * (aWarn + halfA));
                    juce::ColourGradient redBlush (alertC.withAlpha (faceDark ? 0.28f : 0.19f),
                                                   redSpot.x, redSpot.y,
                                                   alertC.withAlpha (0.0f),
                                                   redSpot.x + R * 0.45f, redSpot.y, true);
                    g.setGradientFill (redBlush);
                    g.fillPath (win);
                }

                // Subtle diagonal vintage glass streak
                juce::Path glare;
                glare.addCentredArc (cx, cy, Rw * 0.90f, Rw * 0.90f, 0.0f, -halfA + 0.05f, -0.05f, true);
                g.setColour (juce::Colours::white.withAlpha (faceDark ? 0.08f : 0.30f));
                g.strokePath (glare, juce::PathStrokeType (juce::jlimit (1.5f, 3.0f, R * 0.03f),
                                                           juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
            }

            // Inner recessed bezel shadow (cast downward from top rim)
            g.setColour (juce::Colours::black.withAlpha (faceDark ? 0.46f : 0.14f));
            g.strokePath (win, juce::PathStrokeType (3.8f), juce::AffineTransform::translation (0.0f, 1.4f));
        }

        // Crisp inner window rim
        if (isChrome)
        {
            g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.34f)
                                    : PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f));
            g.strokePath (win, juce::PathStrokeType (1.35f));
            g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.72f : 0.52f));
            g.strokePath (win, juce::PathStrokeType (0.75f));
        }
        else if (isGlass)
        {
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.44f : 0.84f));
            g.strokePath (win, juce::PathStrokeType (1.25f));
        }
        else if (isNeon)
        {
            if (! VuNeedleDetail::isGreenScreen())
            {
                g.setColour (PinkXP::sel.withAlpha (0.32f));
                g.strokePath (win, juce::PathStrokeType (3.2f));
            }
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.36f));
            g.strokePath (win, juce::PathStrokeType (1.15f));
        }
        else if (isCrt)
        {
            const juce::Colour phosC = darkCanvas ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.20f)
                                                  : PinkXP::sel.darker (0.20f);
            g.setColour (phosC.withAlpha (0.80f));
            g.strokePath (win, juce::PathStrokeType (1.15f));
        }
        else
        {
            g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.20f)
                                    : inkCol.withAlpha (0.48f));
            g.strokePath (win, juce::PathStrokeType (1.15f));
        }

        // ---- Thick graduated main scale arc + outward ticks + outer dB labels ----
        const float railW    = juce::jlimit (2.6f, 5.4f, R * 0.052f);
        const float rRail    = rTick0 - railW * 0.5f;
        const float rSub     = rRail - railW * 0.5f - juce::jlimit (5.5f, 10.5f, R * 0.092f);

        const float safeEnd = juce::jmin (halfA, aWarn);
        if ((isNeon || isCrt) && ! VuNeedleDetail::isGreenScreen())
        {
            VuNeedleDetail::strokeGraduatedArc (
                g, cx, cy, rRail, -halfA, halfA, -halfA, safeEnd, railW + (isNeon ? 4.2f : 2.6f), 28,
                [&] (float t)
                {
                    const float db = juce::jmap (t, minDb, maxDb);
                    return vuPaletteColour (t, db, faceDark).withAlpha (isNeon ? 0.28f : 0.20f);
                });
        }
        VuNeedleDetail::strokeGraduatedArc (
            g, cx, cy, rRail, -halfA, halfA, -halfA, safeEnd, railW, 34,
            [&] (float t)
            {
                const float db = juce::jmap (t, minDb, maxDb);
                auto c = (colorMap_ == ColorMap::theme)
                             ? inkCol.interpolatedWith (vuPaletteColour (t, db, faceDark), 0.65f)
                             : vuPaletteColour (t, db, faceDark);
                return isNeon ? c.interpolatedWith (juce::Colours::white, 0.22f) : c;
            });

        // Bold solid red overload arc past warnDbfs
        if (halfA > aWarn + 0.005f)
        {
            auto redArc = VuNeedleDetail::makeArcPath (cx, cy, rRail, aWarn, halfA);
            g.setColour (alertC.withAlpha (darkCanvas ? 0.28f : 0.16f));
            g.strokePath (redArc, juce::PathStrokeType (railW + 3.8f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::butt));
            g.setColour (alertC);
            g.strokePath (redArc, juce::PathStrokeType (railW + 1.0f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::butt));
        }

        std::vector<VuNeedleDetail::TickLabelCandidate> labels;
        labels.reserve (ticks.size());

        for (const auto& t : ticks)
        {
            if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                continue;

            const float ang = needleAngleFor (t.db);
            const bool  hot = t.db >= warnDbfs - 0.01f;
            const auto  p0  = VuNeedleDetail::arcPoint (pivot, rTick0, ang);
            const auto  p1  = VuNeedleDetail::arcPoint (pivot, rTick0 + (t.major ? tickMaj : tickMin), ang);

            g.setColour (hot ? alertC : inkCol.withAlpha (t.major ? 0.90f : 0.48f));
            g.drawLine (p0.x, p0.y, p1.x, p1.y, t.major ? 1.45f : 0.95f);

            if (t.label != nullptr)
            {
                labels.push_back ({
                    juce::String (t.label),
                    VuNeedleDetail::arcPoint (pivot, rLabel, ang),
                    hot ? alertC : inkCol.withAlpha (0.88f),
                    VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                });
            }
        }

        const juce::Rectangle<float> labelSafe (pane.getX() + 2.0f, pane.getY() + 2.0f,
                                                pane.getWidth() - 4.0f, availH);
        VuNeedleDetail::drawNonOverlappingLabels (g, labels, labelSafe, fontPx);

        // ---- Delicate inner % modulation sub-scale (0%..100%) ----
        if (rSub > capR + 12.0f)
        {
            const float aZero = needleAngleFor (juce::jlimit (minDb, maxDb, 0.0f));
            g.setColour (inkCol.withAlpha (0.45f));
            g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, rSub, -halfA, juce::jmin (halfA, aZero)),
                          juce::PathStrokeType (0.95f));
            if (halfA > aZero + 0.005f)
            {
                g.setColour (alertC.withAlpha (0.85f));
                g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, rSub, aZero, halfA),
                              juce::PathStrokeType (1.0f));
            }

            const bool withPctText = (R >= 74.0f && rSub - 13.0f > capR + 14.0f);
            for (int p = 0; p <= 100; p += 10)
            {
                const float pctDb = (p == 0) ? minDb : 20.0f * std::log10 ((float) p / 100.0f);
                if (pctDb < minDb - 0.1f || pctDb > maxDb + 0.1f)
                    continue;

                const float ang = needleAngleFor (pctDb);
                const auto  dp  = VuNeedleDetail::arcPoint (pivot, rSub - 3.2f, ang);
                const float dr  = (p % 50 == 0) ? 1.55f : 0.95f;
                const bool  hot = pctDb >= warnDbfs - 0.01f;
                g.setColour (hot ? alertC.withAlpha (0.88f) : inkCol.withAlpha (p % 50 == 0 ? 0.72f : 0.45f));
                g.fillEllipse (dp.x - dr, dp.y - dr, dr * 2.0f, dr * 2.0f);

                if (withPctText && (p == 0 || p == 50 || p == 100))
                {
                    const auto tp = VuNeedleDetail::arcPoint (pivot, rSub - 11.0f, ang);
                    const juce::String pctTxt = (p == 100) ? juce::String ("100%") : juce::String (p);
                    PinkXP::drawCachedText (g, pctTxt,
                                            juce::Rectangle<float> (tp.x - 14.0f, tp.y - 4.5f, 28.0f, 9.0f),
                                            juce::Justification::centred, 7.2f, juce::Font::plain,
                                            inkCol.withAlpha (0.58f), true);
                }
            }
        }

        // ---- Clean "VU" + "dBFS" emblem centered inside fan (zero collision with ticks or corner LED) ----
        const float emblemTopLimit = (rSub > capR + 12.0f) ? (rSub - (R >= 74.0f ? 19.0f : 7.0f)) : (rRail - 6.0f);
        const float emblemSpan     = emblemTopLimit - (capR + 4.0f);
        if (emblemSpan >= 16.0f)
        {
            const float midR   = capR + 4.0f + emblemSpan * 0.50f;
            const float vuSize = juce::jlimit (10.0f, 16.0f, emblemSpan * 0.48f);
            const float my     = cy - midR;
            const bool  showUnit = emblemSpan >= 28.0f;

            PinkXP::drawCachedText (g, "VU",
                                    juce::Rectangle<float> (cx - 24.0f,
                                                            my - (showUnit ? vuSize * 0.78f : vuSize * 0.55f),
                                                            48.0f, vuSize + 3.0f),
                                    juce::Justification::centred, vuSize, juce::Font::bold,
                                    inkCol.withAlpha (darkCanvas ? 0.72f : 0.78f), true);
            if (showUnit)
            {
                PinkXP::drawCachedText (g, "dBFS",
                                        juce::Rectangle<float> (cx - 22.0f, my + vuSize * 0.28f, 44.0f, 10.0f),
                                        juce::Justification::centred, 7.5f, juce::Font::bold,
                                        inkCol.withAlpha (0.45f), true);
            }
        }

        if (channelTag != nullptr)
            VuNeedleDetail::drawChannelBadge (g, pane, channelTag,
                                              vuPaletteColour (0.68f, -6.0f, darkCanvas), darkCanvas);

        outPivot     = pivot;
        outR         = R;
        outNeedleLen = juce::jmax (12.0f, rTick0 + tickMaj * 0.65f);
        outCapR      = capR;
        return true;
    };

    {
        juce::Point<float> piv;
        float R = 0.0f, nLen = 0.0f, capR = 0.0f;
        const float liveDb = activeSingleDbfs();
        if (drawOneVintageFace (inner.toFloat(), liveDb, activeChannelTag(), piv, R, nLen, capR))
        {
            dialGeom.valid     = true;
            dialGeom.dualSide  = false;
            dialGeom.pivot     = piv;
            dialGeom.radius    = R;
            dialGeom.needleLen = nLen;
            dialGeom.tailLen   = 0.0f;
            dialGeom.capR      = capR;
        }
    }
}

// ============================================================================
// 3. Dual Arc Dial (+/-120 deg Modern Y2K / Hi-Fi 240 deg Illuminated Gauge)
// ============================================================================
void VuMeterModule::drawNeedleDualArcDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    auto inner = dialArea.reduced (5);
    if (inner.getWidth() < 40 || inner.getHeight() < 34)
        return;

    const bool darkCanvas = VuNeedleDetail::isDarkCanvas();
    constexpr float aStart = -2.0943951f; // -120 deg
    constexpr float aEnd   = +2.0943951f; // +120 deg
    dialGeom.angleStart = aStart;
    dialGeom.angleEnd   = aEnd;

    const float readoutReserve = (showReadout_ && dialArea.getHeight() >= 76) ? 18.0f : 4.0f;
    const float minDb  = minDisplayDb();
    const float maxDb  = maxDisplayDb();
    const float aWarn  = needleAngleFor (warnDbfs);
    const auto  alertC = vuAlertColour();

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    auto drawOneDualArcFace = [&] (juce::Rectangle<float> pane,
                                   float primaryDb,
                                   float secondaryDb,
                                   bool coaxialStereo,
                                   float pkPrimary,
                                   float pkSecondary,
                                   const char* channelTag,
                                   juce::Point<float>& outPivot,
                                   float& outR,
                                   float& outNeedleLen) -> bool
    {
        const float availW = pane.getWidth() - 8.0f;
        const float availH = pane.getHeight() - readoutReserve - 6.0f;
        // Flat-bottomed circular pod spans 1.0*R above pivot and 0.62*R below pivot -> height = 1.62*R
        const float R = juce::jmin (availW * 0.5f - 4.0f, (availH - 4.0f) / 1.62f);
        if (R < 20.0f)
            return false;

        const float cx = pane.getCentreX();
        const float podH = R * 1.62f;
        const float cy = pane.getY() + 3.0f + (availH - podH) * 0.5f + R;
        const juce::Point<float> pivot { cx, cy };

        const float arcW = juce::jlimit (4.0f, 8.2f, R * 0.105f);
        const float rArc = R - juce::jlimit (4.2f, 7.2f, R * 0.080f) - arcW * 0.5f;
        const bool  chromaKey = VuNeedleDetail::isChromaKey();

        if (VuNeedleDetail::isPixelHardware())
        {
            // ---- Pixel XP Dual Arc: Win95 sunken gauge pod + crisp smooth segmented LED ring blocks! ----
            const int icx = juce::roundToInt (cx);
            const int icy = juce::roundToInt (cy);
            const int iR  = juce::roundToInt (R);
            const float fcx = (float) icx;
            const float fcy = (float) icy;

            const int podHalfW = juce::jmin (juce::roundToInt (pane.getWidth() * 0.5f) - 2, iR + 4);
            const int podTop   = juce::jmax (juce::roundToInt (pane.getY()) + 1, icy - iR - 4);
            const int podBot   = juce::jmin (juce::roundToInt (pane.getBottom() - readoutReserve) + 1,
                                             icy + juce::roundToInt (R * 0.62f) + 3);
            const juce::Rectangle<int> podRect (icx - podHalfW, podTop,
                                                juce::jmax (16, podHalfW * 2),
                                                juce::jmax (16, podBot - podTop));

            const juce::Colour wellBg = darkCanvas
                                            ? PinkXP::content.darker (0.38f)
                                            : PinkXP::content.interpolatedWith (PinkXP::pink100, 0.18f);
            const juce::Colour inkCol    = PinkXP::readableInkOn (wellBg);
            const juce::Colour hotNumCol = darkCanvas ? alertC : alertC.darker (0.20f);

            if (! chromaKey)
            {
                PinkXP::drawSunken (g, podRect, wellBg);

                const auto podInner = podRect.reduced (2);
                if (backlight_ && ! podInner.isEmpty())
                {
                    const juce::Colour auraCol = vuPaletteColour (0.62f, -8.0f, darkCanvas);
                    const juce::Colour hiBand  = wellBg.interpolatedWith (auraCol, darkCanvas ? 0.18f : 0.14f);
                    PinkXP::Craft::drawPixelDitherBand (g, podInner, hiBand, wellBg, 3);
                }

                // Clean anti-aliased outer dial ring + 4 corner 2x2 hardware pixel pins
                g.setColour (inkCol.withAlpha (darkCanvas ? 0.26f : 0.32f));
                g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, (float) (iR + 1), -2.26f, +2.26f),
                              juce::PathStrokeType (1.0f));
                if (podInner.getWidth() >= 30 && podInner.getHeight() >= 30)
                {
                    const auto pinC = darkCanvas ? PinkXP::hl.withAlpha (0.28f) : PinkXP::shdw.withAlpha (0.55f);
                    g.setColour (pinC);
                    g.fillRect (podInner.getX() + 2,     podInner.getY() + 2,     2, 2);
                    g.fillRect (podInner.getRight() - 4, podInner.getY() + 2,     2, 2);
                    g.fillRect (podInner.getX() + 2,     podInner.getBottom() - 4, 2, 2);
                    g.fillRect (podInner.getRight() - 4, podInner.getBottom() - 4, 2, 2);
                }
            }

            // Discrete segmented LED arc blocks (clean anti-aliased butt arcs with crisp 1.5-2px dark gaps)
            const float segW   = juce::jlimit (4.0f, 7.2f, arcW);
            const int numSegs  = (iR >= 68) ? 24 : ((iR >= 44) ? 20 : 16);
            const float totalSpan = aEnd - aStart;
            const float segPitch  = totalSpan / (float) numSegs;
            const float gapAng    = juce::jmin (segPitch * 0.34f,
                                                juce::jmax (0.026f, 2.2f / juce::jmax (16.0f, rArc)));

            auto drawSegmentedLedRing = [&] (float rTrack, float wTrack, float dbVal, bool altHue)
            {
                const float liveAng = needleAngleFor (dbVal);
                const bool  hasLive = std::isfinite (dbVal) && dbVal > minDb + 0.05f;
                const juce::Colour unlitSocket = darkCanvas ? juce::Colour (0xff14161e)
                                                            : wellBg.darker (0.14f);

                int topLitIdx = -1;
                if (hasLive)
                {
                    for (int i = 0; i < numSegs; ++i)
                    {
                        const float segMid = aStart + ((float) i + 0.5f) * segPitch;
                        if (segMid <= liveAng + 0.002f)
                            topLitIdx = i;
                    }
                }

                for (int i = 0; i < numSegs; ++i)
                {
                    const float sa = aStart + (float) i * segPitch + gapAng * 0.5f;
                    const float sb = aStart + (float) (i + 1) * segPitch - gapAng * 0.5f;
                    if (sb <= sa + 0.004f)
                        continue;

                    const float tMid  = ((float) i + 0.5f) / (float) numSegs;
                    const float segDb = juce::jmap (tMid, minDb, maxDb);
                    const bool  isRed = segDb >= warnDbfs;
                    const bool  isLit = (i <= topLitIdx);

                    juce::Colour segCol = vuPaletteColour (tMid, segDb, darkCanvas);
                    if (altHue && ! isRed)
                    {
                        const juce::Colour shift = VuNeedleDetail::resolveRightCoaxialAccent (colorMap_, darkCanvas, segCol);
                        segCol = segCol.interpolatedWith (shift, 0.62f);
                    }

                    auto segPath = VuNeedleDetail::makeArcPath (fcx, fcy, rTrack, sa, sb);
                    if (isLit)
                    {
                        const bool leading = (i == topLitIdx);
                        const auto col = leading ? segCol.interpolatedWith (juce::Colours::white, 0.32f) : segCol;
                        g.setColour (col);
                        g.strokePath (segPath, juce::PathStrokeType (wTrack, juce::PathStrokeType::curved,
                                                                     juce::PathStrokeType::butt));
                    }
                    else if (! chromaKey)
                    {
                        const auto dimCol = isRed
                                                ? unlitSocket.interpolatedWith (alertC, darkCanvas ? 0.26f : 0.24f)
                                                : unlitSocket.interpolatedWith (segCol, darkCanvas ? 0.14f : 0.12f);
                        g.setColour (dimCol);
                        g.strokePath (segPath, juce::PathStrokeType (wTrack, juce::PathStrokeType::curved,
                                                                     juce::PathStrokeType::butt));
                    }
                }
            };

            const float rInnerCompanion = rArc - segW * 0.5f - 3.0f;

            if (coaxialStereo)
            {
                const float splitW = juce::jmax (2.0f, segW * 0.46f);
                drawSegmentedLedRing (rArc + segW * 0.26f, splitW, primaryDb, false);
                drawSegmentedLedRing (rArc - segW * 0.28f, splitW, secondaryDb, true);
            }
            else
            {
                drawSegmentedLedRing (rArc, segW, primaryDb, false);

                // Clean anti-aliased inner companion ring
                if (! chromaKey)
                {
                    g.setColour (inkCol.withAlpha (darkCanvas ? 0.18f : 0.20f));
                    g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rInnerCompanion, aStart, aEnd),
                                  juce::PathStrokeType (1.15f));
                }
                const float liveAng = needleAngleFor (primaryDb);
                if (liveAng > aStart + 0.008f)
                {
                    VuNeedleDetail::strokeGraduatedArc (
                        g, fcx, fcy, rInnerCompanion, aStart, aEnd, aStart, liveAng, 1.4f, 32,
                        [&] (float t)
                        {
                            const float db = juce::jmap (t, minDb, maxDb);
                            return vuPaletteColour (t, db, darkCanvas).withAlpha (VuNeedleDetail::isGreenScreen() ? 1.0f : 0.88f);
                        });
                }
            }

            // Crisp peak-hold notch strictly gated by peakHold!
            if (peakHold)
            {
                auto drawPeakNotchClean = [&] (float pkDb, float rPos, float halfW, bool altHue)
                {
                    if (! std::isfinite (pkDb) || pkDb <= minDb)
                        return;
                    const float pkAng = needleAngleFor (pkDb);
                    const bool  hot   = pkDb >= warnDbfs;
                    juce::Colour pkCol = hot ? alertC
                                             : vuPaletteColour (dialTFor (pkDb), pkDb, darkCanvas);
                    if (altHue && ! hot)
                    {
                        const juce::Colour shift = VuNeedleDetail::resolveRightCoaxialAccent (colorMap_, darkCanvas, pkCol);
                        pkCol = pkCol.interpolatedWith (shift, 0.65f);
                    }

                    const auto pIn  = VuNeedleDetail::arcPoint ({ fcx, fcy }, rPos - halfW - 1.5f, pkAng);
                    const auto pOut = VuNeedleDetail::arcPoint ({ fcx, fcy }, rPos + halfW + 1.5f, pkAng);
                    const auto hiCol = pkCol.interpolatedWith (juce::Colours::white, darkCanvas ? 0.45f : 0.15f);
                    g.setColour (hiCol);
                    g.drawLine (pIn.x, pIn.y, pOut.x, pOut.y, 1.8f);

                    const auto sqPt = VuNeedleDetail::arcPoint ({ fcx, fcy }, rPos + halfW + 3.0f, pkAng);
                    g.fillRect (juce::roundToInt (sqPt.x) - 1, juce::roundToInt (sqPt.y) - 1, 3, 3);
                };

                if (coaxialStereo)
                {
                    drawPeakNotchClean (pkPrimary,   rArc + segW * 0.26f, segW * 0.26f, false);
                    drawPeakNotchClean (pkSecondary, rArc - segW * 0.28f, segW * 0.26f, true);
                }
                else
                {
                    drawPeakNotchClean (pkPrimary, rArc, segW * 0.50f, false);
                }
            }

            if (chromaKey)
            {
                outPivot     = { fcx, fcy };
                outR         = (float) iR;
                outNeedleLen = juce::jmax (12.0f, rArc - segW * 0.08f);
                return true;
            }

            // Smooth perimeter radial ticks & crisp pixel anti-collision dB labels
            const float tickTop  = (coaxialStereo ? (rArc - segW * 0.5f) : rInnerCompanion) - 2.0f;
            const float tickMaj  = juce::jlimit (3.5f, 6.5f, R * 0.072f);
            const float tickMin  = tickMaj * 0.52f;
            const float fontPx   = juce::jlimit (6.8f, 8.8f, R * 0.098f);
            const float rLabel   = tickTop - tickMaj - juce::jlimit (6.5f, 10.5f, R * 0.135f);

            std::vector<VuNeedleDetail::TickLabelCandidate> labels;
            labels.reserve (ticks.size());

            for (const auto& t : ticks)
            {
                if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                    continue;

                const float ang = needleAngleFor (t.db);
                const bool  hot = t.db >= warnDbfs - 0.01f;
                const auto  p1  = VuNeedleDetail::arcPoint ({ fcx, fcy }, tickTop, ang);
                const auto  p2  = VuNeedleDetail::arcPoint ({ fcx, fcy }, tickTop - (t.major ? tickMaj : tickMin), ang);

                g.setColour (hot ? hotNumCol : inkCol.withAlpha (t.major ? 0.90f : 0.48f));
                g.drawLine (p1.x, p1.y, p2.x, p2.y, t.major ? 1.4f : 0.9f);

                if (t.label != nullptr && rLabel > R * 0.28f)
                {
                    auto lp = VuNeedleDetail::arcPoint ({ fcx, fcy }, rLabel, ang);
                    lp.x = std::round (lp.x);
                    lp.y = std::round (lp.y);
                    labels.push_back ({
                        juce::String (t.label),
                        lp,
                        hot ? hotNumCol : inkCol.withAlpha (darkCanvas ? 0.88f : 0.95f),
                        VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                    });
                }
            }

            const juce::Rectangle<float> labelSafe (fcx - (float) iR + 3.0f, fcy - (float) iR + 3.0f,
                                                    ((float) iR - 3.0f) * 2.0f, (float) iR * 1.56f);
            VuNeedleDetail::drawNonOverlappingLabels (g, labels, labelSafe, fontPx);

            // Smooth central hub ring + lower bay "VU" / "dBFS"
            const float ringR = juce::jlimit (7.0f, 15.0f, R * 0.19f);
            g.setColour (vuPaletteColour (0.65f, -8.0f, darkCanvas).withAlpha (darkCanvas ? 0.36f : 0.32f));
            g.drawEllipse (fcx - ringR, fcy - ringR, ringR * 2.0f, ringR * 2.0f, 1.15f);

            if (R >= 54.0f)
            {
                const int   bayY   = juce::roundToInt (fcy + (float) iR * 0.36f);
                const float vuSize = juce::jlimit (9.0f, 13.0f, R * 0.15f);
                PinkXP::drawCachedText (g, "VU",
                                        juce::Rectangle<int> (icx - 22, bayY - juce::roundToInt (vuSize * 0.55f),
                                                              44, juce::roundToInt (vuSize) + 2),
                                        juce::Justification::centred, vuSize, juce::Font::bold,
                                        inkCol.withAlpha (0.76f), true);
                if (R >= 64.0f)
                {
                    PinkXP::drawCachedText (g, "dBFS",
                                            juce::Rectangle<int> (icx - 22, bayY + juce::roundToInt (vuSize * 0.42f), 44, 10),
                                            juce::Justification::centred, 7.2f, juce::Font::bold,
                                            inkCol.withAlpha (0.48f), true);
                }
            }

            if (channelTag != nullptr)
                VuNeedleDetail::drawChannelBadge (g, pane, channelTag,
                                                  vuPaletteColour (0.72f, -6.0f, darkCanvas), darkCanvas);

            outPivot     = { fcx, fcy };
            outR         = (float) iR;
            outNeedleLen = juce::jmax (12.0f, rArc - segW * 0.08f);
            return true;
        }

        // ---- Smooth, Chrome, Glass, Neon & CRT skins (& ChromaKey live trail only) ----
        const bool  isChrome = VuNeedleDetail::isBrushedMetal();
        const bool  isGlass  = VuNeedleDetail::isGlassDome();
        const bool  isNeon   = VuNeedleDetail::isNeonRings();
        const bool  isCrt    = VuNeedleDetail::isCrtVector();
        const bool  faceDark = darkCanvas;
        const float bezelR   = R + (isChrome ? 4.2f : (isGlass ? 3.8f : 3.0f));
        const float chordA   = 2.26f; // ~129.5 deg -> sleek flat-bottomed hi-fi pod

        juce::Colour inkCol = PinkXP::ink;

        if (! chromaKey)
        {
            juce::Path pod;
            pod.addCentredArc (cx, cy, bezelR, bezelR, 0.0f, -chordA, +chordA, true);
            pod.closeSubPath();

            if (isCrt)
            {
                // 0px square Tektronix vector gauge frame + subtle crosshair reticle
                const int podHalfW = juce::jmin (juce::roundToInt (pane.getWidth() * 0.5f) - 2, juce::roundToInt (R) + 4);
                const int podTop   = juce::jmax (juce::roundToInt (pane.getY()) + 1, juce::roundToInt (cy - R) - 4);
                const int podBot   = juce::jmin (juce::roundToInt (pane.getBottom() - readoutReserve) + 1,
                                                 juce::roundToInt (cy + R * 0.62f) + 3);
                const juce::Rectangle<int> podRect (juce::roundToInt (cx) - podHalfW, podTop,
                                                    juce::jmax (16, podHalfW * 2),
                                                    juce::jmax (16, podBot - podTop));
                const juce::Colour tubeBg = darkCanvas ? PinkXP::content.darker (0.28f)
                                                       : PinkXP::content.interpolatedWith (PinkXP::sel, 0.05f);
                const juce::Colour phosC  = darkCanvas ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.20f)
                                                       : PinkXP::sel.darker (0.22f);
                VuNeedleDetail::drawCrtTektronixBezel (g, podRect, phosC, tubeBg, true);
                inkCol = PinkXP::readableInkOn (tubeBg);

                g.setColour (phosC.withAlpha (0.55f));
                g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, R + 0.5f, -chordA, +chordA),
                              juce::PathStrokeType (1.0f));
            }
            else
            {
                if (isChrome)
                {
                    // CNC brushed circular aluminum instrument pod bezel
                    const juce::Colour bHi  = darkCanvas ? juce::Colour (0xff9aa4b4) : juce::Colour (0xffffffff);
                    const juce::Colour bMid = darkCanvas ? juce::Colour (0xff4a505e) : juce::Colour (0xffc0c8d6);
                    const juce::Colour bLo  = darkCanvas ? juce::Colour (0xff161820) : juce::Colour (0xff5c6472);
                    juce::ColourGradient bezelGrad (bHi, cx - bezelR * 0.45f, cy - bezelR,
                                                    bLo, cx + bezelR * 0.45f, cy + bezelR * 0.65f, false);
                    bezelGrad.addColour (0.44, bMid.interpolatedWith (juce::Colours::white, 0.18f));
                    bezelGrad.addColour (0.52, bMid.darker (0.12f));
                    g.setGradientFill (bezelGrad);
                    g.fillPath (pod);

                    g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.85f : 0.65f));
                    g.strokePath (pod, juce::PathStrokeType (1.1f));
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.46f : 0.78f));
                    g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, bezelR - 0.8f, -2.05f, -0.25f),
                                  juce::PathStrokeType (1.2f));
                }
                else if (isGlass)
                {
                    // Frosted crystal circular acrylic bezel ring
                    const juce::Colour bHi = darkCanvas ? juce::Colour (0xff6e7c96).withAlpha (0.85f)
                                                        : juce::Colours::white.withAlpha (0.94f);
                    const juce::Colour bLo = darkCanvas ? juce::Colour (0xff161c2c).withAlpha (0.92f)
                                                        : PinkXP::pink200.interpolatedWith (juce::Colour (0xff9ab0ca), 0.45f);
                    juce::ColourGradient bezelGrad (bHi, cx, cy - bezelR,
                                                    bLo, cx, cy + bezelR * 0.64f, false);
                    g.setGradientFill (bezelGrad);
                    g.fillPath (pod);
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.48f : 0.86f));
                    g.strokePath (pod, juce::PathStrokeType (1.1f));
                }
                else if (isNeon)
                {
                    // Synthwave turbine pod with double-pass glowing neon rim (respects dark & light themes!)
                    g.setColour (VuNeedleDetail::neonSurfaceBg (darkCanvas));
                    g.fillPath (pod);
                    if (! VuNeedleDetail::isGreenScreen())
                    {
                        g.setColour (PinkXP::sel.withAlpha (darkCanvas ? 0.34f : 0.28f));
                        g.strokePath (pod, juce::PathStrokeType (3.6f));
                    }
                    g.setColour (darkCanvas ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.36f)
                                            : PinkXP::sel.darker (0.10f));
                    g.strokePath (pod, juce::PathStrokeType (1.25f));
                }
                else
                {
                    const juce::Colour bezelHi = darkCanvas ? juce::Colour (0xff484c5a)
                                                            : PinkXP::hl.interpolatedWith (juce::Colour (0xffd8dce6), 0.55f);
                    const juce::Colour bezelLo = darkCanvas ? juce::Colour (0xff12141a)
                                                            : PinkXP::shdw.interpolatedWith (juce::Colour (0xff8a8e9c), 0.55f);
                    juce::ColourGradient bezelGrad (bezelHi, cx, cy - bezelR,
                                                    bezelLo, cx, cy + bezelR * 0.64f, false);
                    g.setGradientFill (bezelGrad);
                    g.fillPath (pod);
                }

                // Recessed inner gauge well
                juce::Path well;
                well.addCentredArc (cx, cy, R + 0.5f, R + 0.5f, 0.0f, -chordA, +chordA, true);
                well.closeSubPath();

                {
                    const juce::Colour wellCtr = (isNeon && darkCanvas)
                                                     ? juce::Colour (0xff0e1222)
                                                     : (darkCanvas
                                                            ? PinkXP::content.interpolatedWith (juce::Colour (0xff1b1f2a), 0.45f)
                                                            : PinkXP::content.brighter (0.12f));
                    const juce::Colour wellRim = (isNeon && darkCanvas)
                                                     ? juce::Colour (0xff05060c)
                                                     : (darkCanvas
                                                            ? juce::Colour (0xff07080c)
                                                            : PinkXP::content.interpolatedWith (PinkXP::pink200, 0.25f));
                    juce::ColourGradient wellGrad (wellCtr, cx, cy,
                                                   wellRim, cx, cy - R, true);
                    g.setGradientFill (wellGrad);
                    g.fillPath (well);
                    inkCol = PinkXP::readableInkOn (wellCtr);
                }

                if (isChrome)
                {
                    juce::Graphics::ScopedSaveState ss (g);
                    g.reduceClipRegion (well);
                    VuNeedleDetail::drawBrushedSheenOverlay (g, well.getBounds(), darkCanvas);
                    g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.55f : 0.22f));
                    g.strokePath (well, juce::PathStrokeType (3.0f), juce::AffineTransform::translation (0.0f, 1.3f));
                }
                else if (isGlass)
                {
                    juce::Graphics::ScopedSaveState ss (g);
                    g.reduceClipRegion (well);
                    VuNeedleDetail::drawGlassDomeReflection (g, well.getBounds(), darkCanvas);
                }

                // ---- Radial backlight aura + curved glass sheen ----
                if (backlight_)
                {
                    juce::Graphics::ScopedSaveState ss (g);
                    g.reduceClipRegion (well);

                    const juce::Colour auraCol = vuPaletteColour (0.62f, -8.0f, faceDark);
                    juce::ColourGradient auraGrad (auraCol.withAlpha (faceDark ? (isNeon ? 0.28f : 0.22f) : 0.14f),
                                                   cx, cy,
                                                   auraCol.withAlpha (0.0f),
                                                   cx, cy - R * 0.98f, true);
                    auraGrad.addColour (0.50, auraCol.withAlpha (faceDark ? 0.10f : 0.05f));
                    g.setGradientFill (auraGrad);
                    g.fillPath (well);

                    // Sleek curved top glass specular sheen
                    auto sheen = VuNeedleDetail::makeArcPath (cx, cy, R * 0.94f, -1.55f, 0.55f);
                    g.setColour (juce::Colours::white.withAlpha (faceDark ? 0.08f : 0.30f));
                    g.strokePath (sheen, juce::PathStrokeType (juce::jlimit (1.4f, 2.8f, R * 0.028f),
                                                               juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
                }

                // Crisp bezel inner outline
                g.setColour (isNeon
                                 ? PinkXP::sel.withAlpha (0.45f)
                                 : (isGlass
                                        ? juce::Colours::white.withAlpha (darkCanvas ? 0.36f : 0.78f)
                                        : (isChrome
                                               ? (darkCanvas ? juce::Colours::white.withAlpha (0.30f) : PinkXP::dark.withAlpha (0.48f))
                                               : (darkCanvas ? juce::Colours::white.withAlpha (0.16f) : PinkXP::ink.withAlpha (0.28f)))));
                g.strokePath (well, juce::PathStrokeType (1.0f));
            }
        }

        // ---- Track groove + LIVE illuminated Dual-Arc active trail ----

        if (! chromaKey)
        {
            // Unlit background channel groove
            g.setColour (faceDark ? juce::Colours::white.withAlpha (0.09f)
                                  : inkCol.withAlpha (0.11f));
            g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, rArc, aStart, aEnd),
                          juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

            // Faint standby danger zone marker past warnDbfs so threshold is always visible
            if (aEnd > aWarn + 0.005f)
            {
                g.setColour (alertC.withAlpha (faceDark ? 0.24f : 0.20f));
                g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, rArc, aWarn, aEnd),
                              juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            }
        }

        // Active glowing trail(s)
        auto drawGlowingTrail = [&] (float rTrack, float wTrack, float dbVal, bool altHue)
        {
            const float liveAng = needleAngleFor (dbVal);
            if (liveAng <= aStart + 0.006f)
                return;

            auto colFn = [&] (float t, float alphaMult) -> juce::Colour
            {
                const float db = juce::jmap (t, minDb, maxDb);
                juce::Colour c = vuPaletteColour (t, db, faceDark);
                if (altHue && db < warnDbfs)
                {
                    const juce::Colour shift = VuNeedleDetail::resolveRightCoaxialAccent (colorMap_, faceDark, c);
                    c = c.interpolatedWith (shift, 0.62f);
                }
                return c.withMultipliedAlpha (alphaMult);
            };

            // Outer soft neon halo pass (skip translucent bloom ONLY on green screen to avoid green fringing)
            if (! VuNeedleDetail::isGreenScreen())
            {
                const float bloomExtra = isNeon ? 6.8f : (isCrt ? 3.8f : 4.8f);
                const float bloomAlpha = isNeon ? 0.34f : (faceDark ? 0.24f : 0.13f);
                VuNeedleDetail::strokeGraduatedArc (
                    g, cx, cy, rTrack, aStart, aEnd, aStart, liveAng, wTrack + bloomExtra, 42,
                    [&] (float t) { return colFn (t, bloomAlpha); });
            }

            // Crisp luminous core trail
            VuNeedleDetail::strokeGraduatedArc (
                g, cx, cy, rTrack, aStart, aEnd, aStart, liveAng, wTrack, 48,
                [&] (float t)
                {
                    auto c = colFn (t, 1.0f);
                    return isNeon ? c.interpolatedWith (juce::Colours::white, 0.22f) : c;
                });

            // Razor-hot center neon laser filament inside active ring on Neon Cyber
            if (isNeon)
            {
                VuNeedleDetail::strokeGraduatedArc (
                    g, cx, cy, rTrack, aStart, aEnd, aStart, liveAng, juce::jmax (1.2f, wTrack * 0.30f), 42,
                    [&] (float t) { return colFn (t, 1.0f).interpolatedWith (juce::Colours::white, 0.65f); });
            }

            // Hot bright leading edge bead right at liveAng
            const auto tipPt = VuNeedleDetail::arcPoint (pivot, rTrack, liveAng);
            const auto tipC  = colFn (dialTFor (dbVal), 1.0f).interpolatedWith (juce::Colours::white, 0.48f);
            const float br   = juce::jmax (1.5f, wTrack * 0.42f);
            g.setColour (tipC);
            g.fillEllipse (tipPt.x - br, tipPt.y - br, br * 2.0f, br * 2.0f);
        };

        const float rInnerCompanion = rArc - arcW * 0.5f - 2.6f;

        if (coaxialStereo)
        {
            // Dual coaxial stereo trails: outer ring = L, inner ring = R
            const float splitW = juce::jmax (2.0f, arcW * 0.46f);
            drawGlowingTrail (rArc + arcW * 0.25f, splitW, primaryDb, false);
            drawGlowingTrail (rArc - arcW * 0.26f, splitW, secondaryDb, true);
        }
        else
        {
            // Signature Dual-Arc look: main glowing outer arc + delicate synchronized inner companion arc
            drawGlowingTrail (rArc, arcW, primaryDb, false);

            if (! chromaKey)
            {
                g.setColour (inkCol.withAlpha (faceDark ? 0.14f : 0.12f));
                g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, rInnerCompanion, aStart, aEnd),
                              juce::PathStrokeType (1.2f));
            }
            const float liveAng = needleAngleFor (primaryDb);
            if (liveAng > aStart + 0.006f)
            {
                VuNeedleDetail::strokeGraduatedArc (
                    g, cx, cy, rInnerCompanion, aStart, aEnd, aStart, liveAng, 1.5f, 36,
                    [&] (float t)
                    {
                        const float db = juce::jmap (t, minDb, maxDb);
                        return vuPaletteColour (t, db, faceDark).withAlpha (VuNeedleDetail::isGreenScreen() ? 1.0f : 0.85f);
                    });
            }
        }

        // Subtle dark segment micro-notches across the arc track for hi-fi precision feel
        if (! chromaKey)
        {
            const int notches = 24;
            const juce::Colour cutCol = faceDark ? juce::Colour (0x6606070a)
                                                 : PinkXP::content.withAlpha (0.45f);
            g.setColour (cutCol);
            for (int i = 1; i < notches; ++i)
            {
                const float ang = juce::jmap ((float) i, 0.0f, (float) notches, aStart, aEnd);
                const auto p1 = VuNeedleDetail::arcPoint (pivot, rArc - arcW * 0.55f, ang);
                const auto p2 = VuNeedleDetail::arcPoint (pivot, rArc + arcW * 0.55f, ang);
                g.drawLine (p1.x, p1.y, p2.x, p2.y, 0.9f);
            }
        }

        // ---- Peak-hold glowing tick notch on outer ring (ONLY when peakHold is ON!) ----
        if (peakHold)
        {
            auto drawPeakNotch = [&] (float pkDb, float rPos, float notchHalfW, bool altHue)
            {
                if (! std::isfinite (pkDb) || pkDb <= minDb)
                    return;
                const float pkAng = needleAngleFor (pkDb);
                const bool  hot   = pkDb >= warnDbfs;
                juce::Colour pkCol = hot ? alertC
                                         : vuPaletteColour (dialTFor (pkDb), pkDb, faceDark);
                if (altHue && ! hot)
                {
                    const juce::Colour shift = VuNeedleDetail::resolveRightCoaxialAccent (colorMap_, faceDark, pkCol);
                    pkCol = pkCol.interpolatedWith (shift, 0.65f);
                }

                // Glowing halo notch (suppress ONLY on green screen to prevent green fringe)
                const auto pIn  = VuNeedleDetail::arcPoint (pivot, rPos - notchHalfW - 1.5f, pkAng);
                const auto pOut = VuNeedleDetail::arcPoint (pivot, rPos + notchHalfW + 1.5f, pkAng);
                if (! VuNeedleDetail::isGreenScreen())
                {
                    g.setColour (pkCol.withAlpha (faceDark ? 0.45f : 0.28f));
                    g.drawLine (pIn.x, pIn.y, pOut.x, pOut.y, 4.2f);
                }

                // Crisp bright core notch + outer glowing dot
                g.setColour (pkCol.interpolatedWith (juce::Colours::white, faceDark ? 0.55f : 0.15f));
                g.drawLine (pIn.x, pIn.y, pOut.x, pOut.y, 1.8f);

                const auto dotP = VuNeedleDetail::arcPoint (pivot, rPos + notchHalfW + 2.2f, pkAng);
                g.fillEllipse (dotP.x - 1.6f, dotP.y - 1.6f, 3.2f, 3.2f);
            };

            if (coaxialStereo)
            {
                drawPeakNotch (pkPrimary,   rArc + arcW * 0.25f, arcW * 0.28f, false);
                drawPeakNotch (pkSecondary, rArc - arcW * 0.26f, arcW * 0.28f, true);
            }
            else
            {
                drawPeakNotch (pkPrimary, rArc, arcW * 0.52f, false);
            }
        }

        if (chromaKey)
        {
            outPivot     = pivot;
            outR         = R;
            outNeedleLen = juce::jmax (12.0f, rArc - arcW * 0.08f);
            return true;
        }

        // ---- Perimeter ticks & anti-collision dB labels ----
        const float tickTop  = (coaxialStereo ? (rArc - arcW * 0.5f) : rInnerCompanion) - 2.0f;
        const float tickMaj  = juce::jlimit (3.5f, 6.5f, R * 0.072f);
        const float tickMin  = tickMaj * 0.52f;
        const float fontPx   = juce::jlimit (6.8f, 8.8f, R * 0.098f);
        const float rLabel   = tickTop - tickMaj - juce::jlimit (6.5f, 10.5f, R * 0.135f);
        const juce::Colour hotNumCol = faceDark ? alertC : alertC.darker (0.20f);

        std::vector<VuNeedleDetail::TickLabelCandidate> labels;
        labels.reserve (ticks.size());

        for (const auto& t : ticks)
        {
            if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                continue;

            const float ang = needleAngleFor (t.db);
            const bool  hot = t.db >= warnDbfs - 0.01f;
            const auto  p1  = VuNeedleDetail::arcPoint (pivot, tickTop, ang);
            const auto  p2  = VuNeedleDetail::arcPoint (pivot, tickTop - (t.major ? tickMaj : tickMin), ang);

            g.setColour (hot ? hotNumCol : inkCol.withAlpha (t.major ? 0.86f : 0.44f));
            g.drawLine (p1.x, p1.y, p2.x, p2.y, t.major ? 1.35f : 0.85f);

            if (t.label != nullptr && rLabel > R * 0.28f)
            {
                labels.push_back ({
                    juce::String (t.label),
                    VuNeedleDetail::arcPoint (pivot, rLabel, ang),
                    hot ? hotNumCol : inkCol.withAlpha (faceDark ? 0.88f : 0.94f),
                    VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                });
            }
        }

        const juce::Rectangle<float> labelSafe (cx - R + 3.0f, cy - R + 3.0f,
                                                (R - 3.0f) * 2.0f, R * 1.56f);
        VuNeedleDetail::drawNonOverlappingLabels (g, labels, labelSafe, fontPx);

        // ---- Sleek central hub ring + lower bay "VU" / "dBFS" badge ----
        const float ringR = juce::jlimit (6.5f, 15.0f, R * 0.19f);
        const auto hubRingCol = vuPaletteColour (0.65f, -8.0f, faceDark);
        if (isNeon && ! VuNeedleDetail::isGreenScreen())
        {
            g.setColour (hubRingCol.withAlpha (0.26f));
            g.drawEllipse (cx - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f, 3.2f);
        }
        g.setColour (hubRingCol.withAlpha (isNeon ? 0.72f : (faceDark ? 0.34f : 0.28f)));
        g.drawEllipse (cx - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f, 1.15f);

        if (R >= 54.0f)
        {
            const float bayY   = cy + R * 0.36f;
            const float vuSize = juce::jlimit (9.0f, 13.5f, R * 0.155f);
            PinkXP::drawCachedText (g, "VU",
                                    juce::Rectangle<float> (cx - 22.0f, bayY - vuSize * 0.55f, 44.0f, vuSize + 2.0f),
                                    juce::Justification::centred, vuSize, juce::Font::bold,
                                    inkCol.withAlpha (faceDark ? 0.72f : 0.72f), true);
            if (R >= 64.0f)
            {
                PinkXP::drawCachedText (g, "dBFS",
                                        juce::Rectangle<float> (cx - 22.0f, bayY + vuSize * 0.42f, 44.0f, 9.0f),
                                        juce::Justification::centred, 7.2f, juce::Font::bold,
                                        inkCol.withAlpha (0.42f), true);
            }
        }

        if (channelTag != nullptr)
            VuNeedleDetail::drawChannelBadge (g, pane, channelTag,
                                              vuPaletteColour (0.72f, -6.0f, darkCanvas), darkCanvas);

        outPivot     = pivot;
        outR         = R;
        outNeedleLen = juce::jmax (12.0f, rArc - arcW * 0.08f);
        return true;
    };

    {
        juce::Point<float> piv;
        float R = 0.0f, nLen = 0.0f;
        const bool coaxial = isStereoMode();
        const float primDb = coaxial ? displayedL : activeSingleDbfs();
        const float secDb  = coaxial ? displayedR : primDb;
        const float pkP    = coaxial ? peakHoldL  : activeSinglePeakDbfs();
        const float pkS    = coaxial ? peakHoldR  : pkP;

        if (drawOneDualArcFace (inner.toFloat(), primDb, secDb, coaxial, pkP, pkS, activeChannelTag(), piv, R, nLen))
        {
            dialGeom.valid     = true;
            dialGeom.dualSide  = false;
            dialGeom.pivot     = piv;
            dialGeom.radius    = R;
            dialGeom.needleLen = nLen;
            dialGeom.tailLen   = juce::jlimit (6.0f, 12.5f, R * 0.145f);
            dialGeom.capR      = 0.0f;
        }
    }
}

// ============================================================================
// 4. Retro Warm Dial (+/-53 deg Hardware Broadcast Illuminated Rectangular VU)
// ============================================================================
void VuMeterModule::drawNeedleRetroWarmDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    if (dialArea.getWidth() < 48 || dialArea.getHeight() < 38)
        return;

    const bool darkCanvas = VuNeedleDetail::isDarkCanvas();
    constexpr float halfA = 0.92f; // ~52.7 deg
    dialGeom.angleStart = -halfA;
    dialGeom.angleEnd   = +halfA;

    // Reserve minimal clean upper faceplate space when Signal LED is enabled so
    // the top-right LED sits neatly on the brushed rack housing above the glass window.
    const auto ledRect = ledBounds (dialArea);
    const float topReserve = showLed
                                 ? juce::jmax (10.0f, (float) (ledRect.getBottom() - dialArea.getY()) + 3.0f)
                                 : 6.0f;
    const float botReserve = (showReadout_ && dialArea.getHeight() >= 76) ? 19.0f : 6.0f;
    const float sidePad    = 6.0f;

    const juce::Rectangle<float> housingArea ((float) dialArea.getX() + sidePad,
                                              (float) dialArea.getY() + topReserve,
                                              (float) dialArea.getWidth() - sidePad * 2.0f,
                                              (float) dialArea.getHeight() - topReserve - botReserve);
    if (housingArea.getWidth() < 36.0f || housingArea.getHeight() < 26.0f)
        return;

    const float minDb = minDisplayDb();
    const float maxDb = maxDisplayDb();
    const float aWarn = needleAngleFor (warnDbfs);
    const auto  face  = VuNeedleDetail::resolveRetroWarmFace (colorMap_, darkCanvas, backlight_, vuAlertColour());

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    auto drawOneRetroWarmFace = [&] (juce::Rectangle<float> bezelRect,
                                     float liveDb,
                                     const char* channelTag,
                                     juce::Point<float>& outPivot,
                                     float& outR,
                                     float& outNeedleLen,
                                     float& outCapR) -> bool
    {
        if (bezelRect.getWidth() < 36.0f || bezelRect.getHeight() < 26.0f)
            return false;

        const float bezelW = juce::jlimit (3.5f, 5.5f, juce::jmin (bezelRect.getWidth(), bezelRect.getHeight()) * 0.055f);
        const juce::Rectangle<float> winRect = bezelRect.reduced (bezelW);
        if (winRect.getWidth() < 26.0f || winRect.getHeight() < 20.0f)
            return false;

        // Compute internal arc geometry cleanly inside winRect
        const float sinH      = std::sin (halfA); // 0.7956
        const float marginX   = juce::jlimit (7.0f, 18.0f, winRect.getWidth() * 0.085f);
        const float marginTop = juce::jlimit (7.0f, 16.0f, winRect.getHeight() * 0.14f);
        const float R         = juce::jmin ((winRect.getWidth() * 0.5f - marginX) / sinH,
                                            winRect.getHeight() - marginTop - 3.5f);
        if (R < 20.0f)
            return false;

        const float cx   = winRect.getCentreX();
        const float cy   = winRect.getBottom() - 1.5f;
        const float capR = juce::jlimit (6.0f, 14.0f, R * 0.135f);
        const juce::Point<float> pivot { cx, cy };

        const float fontPx   = juce::jlimit (7.0f, 9.2f, R * 0.095f);
        const float rLabel   = R - 0.5f;
        const float tickMaj  = juce::jlimit (4.2f, 7.2f, R * 0.072f);
        const float tickMin  = tickMaj * 0.52f;
        const float rScale   = rLabel - fontPx * 0.65f - tickMaj - 2.0f;
        const float blockW   = juce::jlimit (3.6f, 7.2f, R * 0.078f);
        float calibratedNeedleLen = juce::jmax (12.0f, rScale + tickMaj * 0.72f);

        if (VuNeedleDetail::isChromaKey())
        {
            outPivot     = pivot;
            outR         = R;
            outNeedleLen = calibratedNeedleLen;
            outCapR      = capR;
            return true;
        }

        if (VuNeedleDetail::isPixelHardware())
        {
            // ---- Pixel XP Retro Warm: Iconic 1999 Winamp / rack-mount hardware VU ----
            // Beveled dark pixel faceplate + 4 corner 3x3 pixel slotted screw heads +
            // recessed flat warm-amber LCD well with 2px scanlines & 2x2 Bayer top tungsten lamp dither!
            const auto bezelI = bezelRect.toNearestInt();
            const int  bezelPx = juce::jlimit (4, 6, juce::roundToInt (bezelW));
            const juce::Colour plateFill = face.bezelTop.interpolatedWith (face.bezelBot, 0.45f);
            const bool darkBezel = plateFill.getPerceivedBrightness() < 0.48f;

            PinkXP::drawRaised (g, bezelI, plateFill);
            g.setColour (PinkXP::dark);
            g.drawRect (bezelI, 1);

            // 4 corner 3x3 pixel slotted screw heads
            if (bezelI.getWidth() >= 48 && bezelI.getHeight() >= 34)
            {
                const int scOff = juce::jmax (3, bezelPx / 2 + 1);
                VuNeedleDetail::drawPixelScrew (g, bezelI.getX() + scOff,     bezelI.getY() + scOff,     darkBezel);
                VuNeedleDetail::drawPixelScrew (g, bezelI.getRight() - scOff - 1, bezelI.getY() + scOff,     darkBezel);
                VuNeedleDetail::drawPixelScrew (g, bezelI.getX() + scOff,     bezelI.getBottom() - scOff - 1, darkBezel);
                VuNeedleDetail::drawPixelScrew (g, bezelI.getRight() - scOff - 1, bezelI.getBottom() - scOff - 1, darkBezel);
            }

            const auto winI = bezelI.reduced (bezelPx);
            if (winI.getWidth() < 20 || winI.getHeight() < 16)
                return false;

            PinkXP::drawSunken (g, winI, face.faceCenter);
            const auto winInner = winI.reduced (2);

            const int icx = juce::roundToInt (cx);
            const int icy = winInner.getBottom();
            const float fcx = (float) icx;
            const float fcy = (float) icy;

            if (! winInner.isEmpty())
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (winInner);

                if (backlight_)
                {
                    // Quantized 2x2 Bayer top tungsten lamp dither (NO smooth radial blur!)
                    const juce::Colour topLamp = face.faceCenter.interpolatedWith (face.lampCol, face.faceIsDark ? 0.36f : 0.44f);
                    PinkXP::Craft::drawPixelDitherBand (g, winInner, topLamp, face.faceEdge, 4);

                    if (liveDb >= warnDbfs && halfA > aWarn + 0.01f)
                    {
                        // Subtle overload highlight strictly inside the inner LCD frame behind the red block (zero top bezel bleed)
                        juce::Graphics::ScopedSaveState sInner (g);
                        g.reduceClipRegion (winInner.reduced (2));
                        const float rBlock = rScale + blockW * 0.30f;
                        g.setColour (face.dangerCol.withAlpha (face.faceIsDark ? 0.28f : 0.18f));
                        g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rBlock, aWarn, halfA),
                                      juce::PathStrokeType (blockW + 5.0f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::butt));
                    }
                }
                else
                {
                    PinkXP::Craft::drawPixelDitherBand (g, winInner, face.faceEdge, face.faceCenter, 2);
                }

                // Subtle 2px horizontal LCD scanlines across warm-amber window
                g.setColour (juce::Colours::black.withAlpha (face.faceIsDark ? 0.14f : 0.055f));
                for (int sy = winInner.getY() + 1; sy < winInner.getBottom(); sy += 2)
                    g.fillRect (winInner.getX(), sy, winInner.getWidth(), 1);

                const juce::Colour scaleInk = face.scaleInk;

                // Clean anti-aliased base scale arc + bold solid red overload band
                const float safeEnd = juce::jmin (halfA, aWarn);
                g.setColour (scaleInk.withAlpha (0.88f));
                g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rScale, -halfA, safeEnd),
                              juce::PathStrokeType (juce::jlimit (1.6f, 2.5f, R * 0.024f),
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::butt));

                if (halfA > aWarn + 0.005f)
                {
                    const float rBlock = rScale + blockW * 0.30f;
                    g.setColour (face.dangerCol);
                    g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rBlock, aWarn, halfA),
                                  juce::PathStrokeType (blockW, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::butt));
                }

                if (rScale - 6.5f > capR + 14.0f)
                {
                    g.setColour (scaleInk.withAlpha (0.34f));
                    g.strokePath (VuNeedleDetail::makeArcPath (fcx, fcy, rScale - 5.5f, -halfA, safeEnd),
                                  juce::PathStrokeType (0.9f));
                }

                // Clean anti-aliased radial ticks & crisp pixel vintage numerals
                std::vector<VuNeedleDetail::TickLabelCandidate> labels;
                labels.reserve (ticks.size());

                for (const auto& t : ticks)
                {
                    if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                        continue;

                    const float ang = needleAngleFor (t.db);
                    const bool  hot = t.db >= warnDbfs - 0.01f;
                    const float r0  = rScale - 0.5f;
                    const float r1  = rScale + (t.major ? tickMaj : tickMin) + (hot ? blockW * 0.25f : 0.0f);

                    const auto p0 = VuNeedleDetail::arcPoint ({ fcx, fcy }, r0, ang);
                    const auto p1 = VuNeedleDetail::arcPoint ({ fcx, fcy }, r1, ang);

                    g.setColour (hot ? face.dangerCol : scaleInk.withAlpha (t.major ? 0.90f : 0.52f));
                    g.drawLine (p0.x, p0.y, p1.x, p1.y, t.major ? 1.5f : 0.95f);

                    if (t.label != nullptr)
                    {
                        auto lp = VuNeedleDetail::arcPoint ({ fcx, fcy }, rLabel, ang);
                        lp.x = std::round (lp.x);
                        lp.y = std::round (lp.y);
                        labels.push_back ({
                            juce::String (t.label),
                            lp,
                            hot ? face.dangerCol : scaleInk.withAlpha (0.92f),
                            VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                        });
                    }
                }

                VuNeedleDetail::drawNonOverlappingLabels (g, labels, winInner.toFloat().reduced (2.0f, 1.5f), fontPx);

                // Bold pixel "VU" watermark centered below scale
                const float freeSpan = (rScale - 6.0f) - (capR + 4.0f);
                if (freeSpan >= 14.0f)
                {
                    const float midR   = capR + 4.0f + freeSpan * 0.52f;
                    const float vuSize = juce::jlimit (11.0f, 19.0f, freeSpan * 0.52f);
                    const int   vuY    = juce::roundToInt (fcy - midR);
                    const bool  withSub = freeSpan >= 28.0f;

                    PinkXP::drawCachedText (g, "VU",
                                            juce::Rectangle<int> (icx - 28,
                                                                  vuY - juce::roundToInt (withSub ? vuSize * 0.72f : vuSize * 0.54f),
                                                                  56, juce::roundToInt (vuSize) + 3),
                                            juce::Justification::centred, vuSize, juce::Font::bold,
                                            scaleInk.withAlpha (0.82f), true);

                    if (withSub)
                    {
                        PinkXP::drawCachedText (g, "dBFS",
                                                juce::Rectangle<int> (icx - 24, vuY + juce::roundToInt (vuSize * 0.30f), 48, 10),
                                                juce::Justification::centred, 7.4f, juce::Font::bold,
                                                scaleInk.withAlpha (0.54f), true);
                    }
                }

                // Crisp 0px pixel channel stamp ("L" / "R") inside upper-left corner
                if (channelTag != nullptr)
                {
                    const juce::Rectangle<int> tagBox (winInner.getX() + 3, winInner.getY() + 3, 15, 13);
                    PinkXP::Craft::drawBadgePill (g, tagBox, face.faceCenter.darker (0.10f), scaleInk.withAlpha (0.65f));
                    PinkXP::drawCachedText (g, channelTag, tagBox, juce::Justification::centred,
                                            8.5f, juce::Font::bold, scaleInk, true);
                }
            }

            outPivot     = { fcx, fcy };
            outR         = R;
            outNeedleLen = calibratedNeedleLen;
            outCapR      = capR;
            return true;
        }

        // ---- Smooth, Chrome, Glass, Neon & CRT skins: Recessed metal/crystal/neon/scope bezel frame ----
        const bool  isChrome    = VuNeedleDetail::isBrushedMetal();
        const bool  isGlass     = VuNeedleDetail::isGlassDome();
        const bool  isNeon      = VuNeedleDetail::isNeonRings();
        const bool  isCrt       = VuNeedleDetail::isCrtVector();
        const float outerCorner = (isChrome || isCrt) ? 0.0f : (isNeon ? 2.0f : (isGlass ? 6.0f : 5.0f));
        const float winCorner   = (isChrome || isCrt) ? 0.0f : (isNeon ? 1.5f : (isGlass ? 4.0f : 3.0f));

        if (isChrome)
        {
            // Anodized brushed-steel CNC rectilinear rack faceplate (0px sharp machined edge)
            const auto bezelI = bezelRect.toNearestInt();
            const juce::Colour bTop = face.bezelTop.interpolatedWith (juce::Colours::white, darkCanvas ? 0.18f : 0.26f);
            const juce::Colour bMid = face.bezelTop.interpolatedWith (face.bezelBot, 0.48f);
            const juce::Colour bBot = face.bezelBot.darker (0.12f);
            juce::ColourGradient bezelGrad (bTop, (float) bezelI.getX(), (float) bezelI.getY(),
                                            bBot, (float) bezelI.getX(), (float) bezelI.getBottom(), false);
            bezelGrad.addColour (0.45, bMid.interpolatedWith (juce::Colours::white, 0.14f));
            bezelGrad.addColour (0.53, bMid.darker (0.10f));
            g.setGradientFill (bezelGrad);
            g.fillRect (bezelI);

            VuNeedleDetail::drawBrushedSheenOverlay (g, bezelI.toFloat(), bMid.getPerceivedBrightness() < 0.45f);

            // 1px dark outer cut + 1px bright top/left chrome specular bevel + bottom/right shadow
            g.setColour (PinkXP::dark);
            g.drawRect (bezelI, 1);
            const auto inB = bezelI.reduced (1);
            if (! inB.isEmpty())
            {
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.34f : 0.72f));
                g.fillRect (inB.getX(), inB.getY(), inB.getWidth(), 1);
                g.fillRect (inB.getX(), inB.getY(), 1, inB.getHeight());
                g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.52f : 0.32f));
                g.fillRect (inB.getX(), inB.getBottom() - 1, inB.getWidth(), 1);
                g.fillRect (inB.getRight() - 1, inB.getY(), 1, inB.getHeight());
            }
        }
        else if (isGlass)
        {
            // Frosted Glass: rounded 6px glossy crystal bezel frame with top specular dome rim
            const juce::Colour gTop = face.bezelTop.interpolatedWith (juce::Colours::white, darkCanvas ? 0.22f : 0.52f);
            const juce::Colour gBot = face.bezelBot.interpolatedWith (PinkXP::sel, 0.08f);
            juce::ColourGradient bezelGrad (gTop, bezelRect.getX(), bezelRect.getY(),
                                            gBot, bezelRect.getX(), bezelRect.getBottom(), false);
            g.setGradientFill (bezelGrad);
            g.fillRoundedRectangle (bezelRect, outerCorner);

            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.44f : 0.86f));
            g.drawRoundedRectangle (bezelRect.reduced (0.6f), outerCorner, 1.15f);
            g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.50f : 0.28f));
            g.drawRoundedRectangle (bezelRect, outerCorner, 1.0f);
        }
        else if (isNeon)
        {
            // Neon Cyber: cyber rack unit with glowing double-pass bezel & corner L-brackets (respects dark & light themes!)
            const juce::Colour bzBg = darkCanvas ? juce::Colour (0xff0a0c16)
                                                 : face.bezelTop.interpolatedWith (face.bezelBot, 0.45f);
            VuNeedleDetail::drawNeonDoubleBezel (g, bezelRect, PinkXP::sel, bzBg);
        }
        else if (isCrt)
        {
            // CRT Terminal: 0px square Tektronix rack scope bezel
            const juce::Colour phosC = darkCanvas ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.20f)
                                                  : PinkXP::sel.darker (0.22f);
            const juce::Colour bezelBg = darkCanvas ? juce::Colour (0xff0b1210) : PinkXP::face.darker (0.08f);
            VuNeedleDetail::drawCrtTektronixBezel (g, bezelRect.toNearestInt(), phosC, bezelBg, false);
        }
        else
        {
            juce::ColourGradient bezelGrad (face.bezelTop, bezelRect.getX(), bezelRect.getY(),
                                            face.bezelBot, bezelRect.getX(), bezelRect.getBottom(), false);
            g.setGradientFill (bezelGrad);
            g.fillRoundedRectangle (bezelRect, outerCorner);

            // Subtle outer chamfer bevel highlight (top/left) & deep outer shadow border
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.14f : 0.42f));
            g.drawRoundedRectangle (bezelRect.reduced (0.5f), outerCorner, 1.0f);
            g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.65f : 0.35f));
            g.drawRoundedRectangle (bezelRect, outerCorner, 1.1f);
        }

        juce::Path winClip;
        if (winCorner > 0.5f)
            winClip.addRoundedRectangle (winRect, winCorner);
        else
            winClip.addRectangle (winRect);

        // ---- Illuminated warm backlit glass window ----
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (winClip);

            // Base warm radial parchment/amber/McIntosh-blue dial card gradient
            juce::ColourGradient cardGrad (face.faceCenter, cx, winRect.getY() + winRect.getHeight() * 0.38f,
                                           face.faceEdge,   winRect.getX(), winRect.getBottom(), true);
            g.setGradientFill (cardGrad);
            g.fillRect (winRect);

            if (isGlass)
                VuNeedleDetail::drawGlassDomeReflection (g, winRect, face.faceIsDark);
            else if (isCrt)
            {
                const juce::Colour retC = face.scaleInk.withAlpha (0.14f);
                const int wCx = juce::roundToInt (winRect.getCentreX());
                const int wCy = juce::roundToInt (winRect.getCentreY());
                g.setColour (retC);
                g.fillRect (juce::roundToInt (winRect.getX()), wCy, juce::roundToInt (winRect.getWidth()), 1);
                g.fillRect (wCx, juce::roundToInt (winRect.getY()), 1, juce::roundToInt (winRect.getHeight()));
            }

            if (backlight_)
            {
                // Twin upper warm tungsten incandescent bulb pools (authentic analog VU illumination!)
                const float lampY = winRect.getY() + winRect.getHeight() * 0.06f;
                const float lampR = juce::jmax (winRect.getWidth() * 0.44f, winRect.getHeight() * 0.75f);
                const float lx1   = winRect.getX() + winRect.getWidth() * 0.25f;
                const float lx2   = winRect.getX() + winRect.getWidth() * 0.75f;
                const float lampA = face.faceIsDark ? 0.24f : 0.34f;

                for (float lx : { lx1, lx2 })
                {
                    juce::ColourGradient bulb (face.lampCol.withAlpha (lampA), lx, lampY,
                                               face.lampCol.withAlpha (0.0f),  lx, lampY + lampR, true);
                    bulb.addColour (0.45, face.lampCol.withAlpha (lampA * 0.42f));
                    g.setGradientFill (bulb);
                    g.fillRect (winRect);
                }

                // Subtle red overload lamp bloom behind the danger block when signal >= warnDbfs
                if (liveDb >= warnDbfs && halfA > aWarn + 0.01f)
                {
                    const auto hotPt = VuNeedleDetail::arcPoint (pivot, R * 0.84f, 0.5f * (aWarn + halfA));
                    juce::ColourGradient redLamp (face.dangerCol.withAlpha (face.faceIsDark ? 0.34f : 0.24f),
                                                  hotPt.x, hotPt.y,
                                                  face.dangerCol.withAlpha (0.0f),
                                                  hotPt.x + R * 0.50f, hotPt.y, true);
                    g.setGradientFill (redLamp);
                    g.fillRect (winRect);
                }
            }

            // ---- Classic bold curved scale bar + thick solid red danger block ----

            // Base scale arc (below warnDbfs)
            const float safeEnd = juce::jmin (halfA, aWarn);
            g.setColour (face.scaleInk.withAlpha (0.86f));
            g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, rScale, -halfA, safeEnd),
                          juce::PathStrokeType (juce::jlimit (1.5f, 2.5f, R * 0.024f),
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::butt));

            // Thick solid red danger block from warnDbfs to maxDisplayDb()
            if (halfA > aWarn + 0.005f)
            {
                const float rBlock = rScale + blockW * 0.32f;
                auto dangerArc = VuNeedleDetail::makeArcPath (cx, cy, rBlock, aWarn, halfA);
                if (backlight_)
                {
                    g.setColour (face.dangerCol.withAlpha (face.faceIsDark ? 0.30f : 0.16f));
                    g.strokePath (dangerArc, juce::PathStrokeType (blockW + 4.0f,
                                                                   juce::PathStrokeType::curved,
                                                                   juce::PathStrokeType::butt));
                }
                g.setColour (face.dangerCol);
                g.strokePath (dangerArc, juce::PathStrokeType (blockW,
                                                               juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::butt));
            }

            // Subtle vintage inner secondary hairline arc
            if (rScale - 6.5f > capR + 14.0f)
            {
                g.setColour (face.scaleInk.withAlpha (0.30f));
                g.strokePath (VuNeedleDetail::makeArcPath (cx, cy, rScale - 5.5f, -halfA, safeEnd),
                              juce::PathStrokeType (0.85f));
            }

            // Stepped major/minor ticks + crisp vintage numerals
            std::vector<VuNeedleDetail::TickLabelCandidate> labels;
            labels.reserve (ticks.size());

            for (const auto& t : ticks)
            {
                if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                    continue;

                const float ang = needleAngleFor (t.db);
                const bool  hot = t.db >= warnDbfs - 0.01f;
                const float r0  = rScale - 0.5f;
                const float r1  = rScale + (t.major ? tickMaj : tickMin) + (hot ? blockW * 0.25f : 0.0f);

                const auto p0 = VuNeedleDetail::arcPoint (pivot, r0, ang);
                const auto p1 = VuNeedleDetail::arcPoint (pivot, r1, ang);

                g.setColour (hot ? face.dangerCol : face.scaleInk.withAlpha (t.major ? 0.90f : 0.50f));
                g.drawLine (p0.x, p0.y, p1.x, p1.y, t.major ? 1.5f : 0.95f);

                if (t.label != nullptr)
                {
                    labels.push_back ({
                        juce::String (t.label),
                        VuNeedleDetail::arcPoint (pivot, rLabel, ang),
                        hot ? face.dangerCol : face.scaleInk.withAlpha (0.90f),
                        VuNeedleDetail::tickLabelPriority (t.db, minDb, maxDb, warnDbfs)
                    });
                }
            }

            VuNeedleDetail::drawNonOverlappingLabels (g, labels, winRect.reduced (2.0f, 1.5f), fontPx);

            // ---- Bold vintage "VU" watermark centered below scale ----
            const float freeSpan = (rScale - 6.0f) - (capR + 4.0f);
            if (freeSpan >= 14.0f)
            {
                const float midR   = capR + 4.0f + freeSpan * 0.52f;
                const float vuSize = juce::jlimit (11.0f, 20.0f, freeSpan * 0.54f);
                const float vuY    = cy - midR;
                const bool  withSub = freeSpan >= 28.0f;

                PinkXP::drawCachedText (g, "VU",
                                        juce::Rectangle<float> (cx - 28.0f,
                                                                vuY - (withSub ? vuSize * 0.72f : vuSize * 0.54f),
                                                                56.0f, vuSize + 3.0f),
                                        juce::Justification::centred, vuSize, juce::Font::bold,
                                        face.scaleInk.withAlpha (0.76f), true);

                if (withSub)
                {
                    PinkXP::drawCachedText (g, "dBFS",
                                            juce::Rectangle<float> (cx - 24.0f, vuY + vuSize * 0.30f, 48.0f, 10.0f),
                                            juce::Justification::centred, 7.4f, juce::Font::bold,
                                            face.scaleInk.withAlpha (0.48f), true);
                }
            }

            // Vintage channel stamp ("L" / "R") inside upper-left glass corner
            if (channelTag != nullptr)
            {
                const juce::Rectangle<float> tagBox (winRect.getX() + 4.5f, winRect.getY() + 3.5f, 15.0f, 13.0f);
                if (isChrome || isGlass || isNeon || isCrt)
                {
                    const auto tagI = tagBox.toNearestInt();
                    const auto tagBg = (isNeon && face.faceIsDark) ? juce::Colour (0xff0a0d16) : face.faceCenter.darker (0.10f);
                    const auto tagBd = isNeon ? PinkXP::sel.withAlpha (0.85f) : face.scaleInk.withAlpha (0.65f);
                    PinkXP::Craft::drawBadgePill (g, tagI, tagBg, tagBd);
                    PinkXP::drawCachedText (g, channelTag, tagI, juce::Justification::centred,
                                            isCrt ? 7.8f : 8.5f, juce::Font::bold, PinkXP::readableInkOn (tagBg), true);
                }
                else
                {
                    g.setColour (face.scaleInk.withAlpha (0.14f));
                    g.fillRoundedRectangle (tagBox, 2.5f);
                    g.setColour (face.scaleInk.withAlpha (0.45f));
                    g.drawRoundedRectangle (tagBox, 2.5f, 0.85f);
                    PinkXP::drawCachedText (g, channelTag, tagBox, juce::Justification::centred,
                                            8.5f, juce::Font::bold, face.scaleInk.withAlpha (0.86f), true);
                }
            }

            // ---- Curved top-left glass reflection glare strip ----
            {
                juce::Path glare;
                const float gx0 = winRect.getX() - 4.0f;
                const float gy0 = winRect.getY() + winRect.getHeight() * 0.36f;
                const float gx1 = winRect.getX() + winRect.getWidth() * 0.52f;
                const float gy1 = winRect.getY() - 2.0f;
                glare.startNewSubPath (gx0, gy0);
                glare.quadraticTo (winRect.getX() + winRect.getWidth() * 0.16f,
                                   winRect.getY() + winRect.getHeight() * 0.10f,
                                   gx1, gy1);
                glare.lineTo (gx1 + winRect.getWidth() * 0.14f, gy1);
                glare.quadraticTo (winRect.getX() + winRect.getWidth() * 0.24f,
                                   winRect.getY() + winRect.getHeight() * 0.18f,
                                   gx0, gy0 + winRect.getHeight() * 0.14f);
                glare.closeSubPath();

                g.setColour (juce::Colours::white.withAlpha (face.faceIsDark ? 0.075f : 0.24f));
                g.fillPath (glare);
            }

            // Inner recessed window shadow along top & side bezel edges
            juce::ColourGradient topShdw (juce::Colours::black.withAlpha (face.faceIsDark ? 0.52f : 0.22f),
                                          winRect.getX(), winRect.getY(),
                                          juce::Colours::transparentBlack,
                                          winRect.getX(), winRect.getY() + juce::jmin (10.0f, winRect.getHeight() * 0.16f),
                                          false);
            g.setGradientFill (topShdw);
            g.fillRect (winRect);

            if (isChrome)
            {
                g.setColour (juce::Colours::black.withAlpha (0.72f));
                g.drawRect (winRect.toNearestInt(), 1);
            }
            else if (isNeon)
            {
                g.setColour (PinkXP::sel.withAlpha (0.72f));
                g.drawRoundedRectangle (winRect, winCorner, 1.2f);
            }
            else if (isCrt)
            {
                g.setColour (face.scaleInk.withAlpha (0.58f));
                g.drawRect (winRect.toNearestInt(), 1);
            }
            else
            {
                g.setColour (juce::Colours::black.withAlpha (face.faceIsDark ? 0.55f : 0.30f));
                g.drawRoundedRectangle (winRect, winCorner, 1.4f);
            }
        }

        if (isChrome)
        {
            // Crisp recessed inner chrome bevel lip around glass aperture (outside winClip!)
            const auto lip = winRect.toNearestInt().expanded (1);
            g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.68f : 0.44f));
            g.fillRect (lip.getX(), lip.getY(), lip.getWidth(), 1);
            g.fillRect (lip.getX(), lip.getY(), 1, lip.getHeight());
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.38f : 0.76f));
            g.fillRect (lip.getX(), lip.getBottom() - 1, lip.getWidth(), 1);
            g.fillRect (lip.getRight() - 1, lip.getY(), 1, lip.getHeight());
        }

        if (isChrome && bezelRect.getWidth() >= 64.0f && bezelRect.getHeight() >= 44.0f)
        {
            // 4 CNC recessed slotted corner bolts on anodized brushed-steel rack faceplate
            const float boltR  = juce::jlimit (1.7f, 2.7f, bezelW * 0.42f);
            const float pinOff = juce::jmax (boltR + 1.5f, bezelW * 0.55f);
            const bool  darkBz = face.bezelTop.interpolatedWith (face.bezelBot, 0.5f).getPerceivedBrightness() < 0.45f;
            const juce::Point<float> corners[4] = {
                { bezelRect.getX() + pinOff,     bezelRect.getY() + pinOff },
                { bezelRect.getRight() - pinOff, bezelRect.getY() + pinOff },
                { bezelRect.getX() + pinOff,     bezelRect.getBottom() - pinOff },
                { bezelRect.getRight() - pinOff, bezelRect.getBottom() - pinOff }
            };
            for (const auto& cp : corners)
                VuNeedleDetail::drawCncBolt (g, cp.x, cp.y, boltR, darkBz);
        }
        else if (! isChrome && bezelRect.getWidth() >= 150.0f && bezelRect.getHeight() >= 86.0f)
        {
            // Subtle metallic corner bezel pin dimples on larger hardware frames
            const float pinOff = bezelW * 0.50f;
            const float pr     = 1.15f;
            const juce::Point<float> corners[4] = {
                { bezelRect.getX() + pinOff,     bezelRect.getY() + pinOff },
                { bezelRect.getRight() - pinOff, bezelRect.getY() + pinOff },
                { bezelRect.getX() + pinOff,     bezelRect.getBottom() - pinOff },
                { bezelRect.getRight() - pinOff, bezelRect.getBottom() - pinOff }
            };
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.20f : 0.45f));
            for (const auto& cp : corners)
                g.fillEllipse (cp.x - pr, cp.y - pr, pr * 2.0f, pr * 2.0f);
        }

        outPivot     = pivot;
        outR         = R;
        outNeedleLen = calibratedNeedleLen;
        outCapR      = capR;
        return true;
    };

    {
        juce::Point<float> piv;
        float R = 0.0f, nLen = 0.0f, capR = 0.0f;
        const float liveDb = activeSingleDbfs();
        if (drawOneRetroWarmFace (housingArea, liveDb, activeChannelTag(), piv, R, nLen, capR))
        {
            dialGeom.valid     = true;
            dialGeom.dualSide  = false;
            dialGeom.pivot     = piv;
            dialGeom.radius    = R;
            dialGeom.needleLen = nLen;
            dialGeom.tailLen   = 0.0f;
            dialGeom.capR      = capR;
        }
    }
}

// ============================================================================
// 5. Precision Needle Pointer (Thin / Tapered / Glow + Realistic Cast Shadow)
// ============================================================================
void VuMeterModule::drawNeedlePointer (juce::Graphics& g,
                                       juce::Point<float> pivot,
                                       float length,
                                       float tailLen,
                                       float angleRad,
                                       juce::Colour colour,
                                       float thickness,
                                       NeedleShape shape,
                                       float alphaScale) const
{
    if (length <= 2.0f || alphaScale <= 0.01f || ! std::isfinite (angleRad) || ! std::isfinite (length))
        return;

    const auto& tokens      = PinkXP::getSkinTokens();
    const bool  greenScreen = PinkXP::isGreenScreenBackdrop();
    const bool  drawShadow  = tokens.needleShadow && ! tokens.isChromaKey;

    const float sinA = std::sin (angleRad);
    const float cosA = std::cos (angleRad);
    const float nx   = cosA;
    const float ny   = sinA;

    const juce::Point<float> tip  { pivot.x + sinA * length,  pivot.y - cosA * length  };
    const juce::Point<float> tail { pivot.x - sinA * tailLen, pivot.y + cosA * tailLen };

    const juce::Colour col = colour.withMultipliedAlpha (alphaScale);

    const float shDx = tokens.needleHardPixelShadow ? 1.0f : 1.2f;
    const float shDy = tokens.needleHardPixelShadow ? 1.5f : 1.8f;

    const juce::Colour shadowCol = juce::Colours::black.withAlpha ((tokens.needleHardPixelShadow ? 0.42f : 0.34f) * alphaScale);

    switch (shape)
    {
        case NeedleShape::thin:
        {
            const float shaftW = juce::jmax (1.0f, thickness * 0.72f);

            // Soft cast drop shadow
            if (drawShadow)
            {
                g.setColour (shadowCol);
                g.drawLine (tail.x + shDx, tail.y + shDy, tip.x + shDx, tip.y + shDy, shaftW + 0.35f);
            }

            // Counterweight tail (slightly thicker balanced rear bar + end bead)
            if (tailLen > 1.5f)
            {
                const float tailW = juce::jmax (shaftW * 1.6f, thickness * 1.25f);
                g.setColour (col.darker (0.15f));
                g.drawLine (pivot.x, pivot.y, tail.x, tail.y, tailW);
                const float beadR = juce::jlimit (1.3f, 2.5f, tailW * 0.62f);
                g.fillEllipse (tail.x - beadR, tail.y - beadR, beadR * 2.0f, beadR * 2.0f);
            }

            // Crisp precision hairline shaft
            juce::Path shaft;
            shaft.startNewSubPath (pivot);
            shaft.lineTo (tip);
            const bool isNeonNeedle = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
            const bool isCrtNeedle  = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
            if (tokens.needleGlow && ! greenScreen)
            {
                if (isNeonNeedle)
                {
                    g.setColour (col.withMultipliedAlpha (0.20f));
                    g.strokePath (shaft, juce::PathStrokeType (shaftW * 4.2f, juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
                    g.setColour (col.withMultipliedAlpha (0.40f));
                    g.strokePath (shaft, juce::PathStrokeType (shaftW * 2.2f, juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
                }
                else
                {
                    g.setColour (col.withMultipliedAlpha (0.26f));
                    g.strokePath (shaft, juce::PathStrokeType (shaftW * 2.8f, juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
                }
            }
            g.setColour ((isNeonNeedle || isCrtNeedle)
                             ? col.interpolatedWith (juce::Colours::white, isNeonNeedle ? 0.46f : 0.30f)
                             : col);
            g.strokePath (shaft, juce::PathStrokeType (shaftW, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));

            // Tiny bright jewel tip
            const float tipR = juce::jlimit (1.15f, 1.95f, shaftW * 0.90f);
            g.setColour (col.interpolatedWith (juce::Colours::white, 0.38f));
            g.fillEllipse (tip.x - tipR, tip.y - tipR, tipR * 2.0f, tipR * 2.0f);
            break;
        }

        case NeedleShape::glow:
        {
            const float coreW = juce::jmax (1.15f, thickness * 0.82f);

            juce::Path fullPath;
            fullPath.startNewSubPath (tail);
            fullPath.lineTo (tip);

            // Subtle drop shadow underneath
            if (drawShadow)
            {
                g.setColour (juce::Colours::black.withAlpha (0.24f * alphaScale));
                g.strokePath (fullPath, juce::PathStrokeType (coreW + 0.8f, juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded),
                              juce::AffineTransform::translation (shDx, shDy));
            }

            if (! greenScreen)
            {
                // Layer 1: wide soft outer neon halo
                g.setColour (col.withMultipliedAlpha (0.18f));
                g.strokePath (fullPath, juce::PathStrokeType (coreW * 4.2f, juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));

                // Layer 2: mid luminous glow sheath
                g.setColour (col.withMultipliedAlpha (0.40f));
                g.strokePath (fullPath, juce::PathStrokeType (coreW * 2.2f, juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
            }

            // Inner white-tinted luminous blade core
            const juce::Colour hotCore = col.interpolatedWith (juce::Colours::white, 0.56f);
            g.setColour (hotCore);
            g.strokePath (fullPath, juce::PathStrokeType (coreW, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));

            // Glowing jewel orb at tip
            const float haloR = juce::jlimit (2.6f, 4.4f, coreW * 2.2f);
            const float orbR  = juce::jlimit (1.2f, 2.0f, coreW * 0.95f);
            if (! greenScreen)
            {
                g.setColour (col.withMultipliedAlpha (0.34f));
                g.fillEllipse (tip.x - haloR, tip.y - haloR, haloR * 2.0f, haloR * 2.0f);
            }
            g.setColour (hotCore);
            g.fillEllipse (tip.x - orbR, tip.y - orbR, orbR * 2.0f, orbR * 2.0f);
            break;
        }

        case NeedleShape::tapered:
        default:
        {
            const float baseHalfW = juce::jmax (1.45f, thickness * 1.15f);
            const float tipHalfW  = 0.35f;
            const float tailHalfW = baseHalfW * 0.62f;

            // Sculpted spear/lancet body polygon
            juce::Path spear;
            if (tailLen > 1.0f)
            {
                spear.startNewSubPath (tail.x + nx * tailHalfW, tail.y + ny * tailHalfW);
                spear.lineTo (pivot.x + nx * baseHalfW, pivot.y + ny * baseHalfW);
                spear.lineTo (tip.x + nx * tipHalfW,    tip.y + ny * tipHalfW);
                spear.lineTo (tip.x - nx * tipHalfW,    tip.y - ny * tipHalfW);
                spear.lineTo (pivot.x - nx * baseHalfW, pivot.y - ny * baseHalfW);
                spear.lineTo (tail.x - nx * tailHalfW,  tail.y - ny * tailHalfW);
                spear.closeSubPath();
            }
            else
            {
                spear.startNewSubPath (pivot.x + nx * baseHalfW, pivot.y + ny * baseHalfW);
                spear.lineTo (tip.x + nx * tipHalfW,             tip.y + ny * tipHalfW);
                spear.lineTo (tip.x - nx * tipHalfW,             tip.y - ny * tipHalfW);
                spear.lineTo (pivot.x - nx * baseHalfW,          pivot.y - ny * baseHalfW);
                spear.closeSubPath();
            }

            // Realistic cast drop shadow
            if (drawShadow)
            {
                g.setColour (shadowCol);
                g.fillPath (spear, juce::AffineTransform::translation (shDx, shDy));
            }

            const bool isNeonNeedle = (tokens.dialFinish == PinkXP::DialFinish::neonRings);
            const bool isCrtNeedle  = (tokens.dialFinish == PinkXP::DialFinish::crtVector);
            if (tokens.needleGlow && ! greenScreen)
            {
                if (isNeonNeedle)
                {
                    g.setColour (col.withMultipliedAlpha (0.20f));
                    g.strokePath (spear, juce::PathStrokeType (4.2f, juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
                    g.setColour (col.withMultipliedAlpha (0.38f));
                    g.strokePath (spear, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
                }
                else
                {
                    g.setColour (col.withMultipliedAlpha (0.26f));
                    g.strokePath (spear, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
                }
            }

            // Sculpted lancet body (luminous hot core on Neon/CRT)
            g.setColour ((isNeonNeedle || isCrtNeedle)
                             ? col.interpolatedWith (juce::Colours::white, isNeonNeedle ? 0.36f : 0.22f)
                             : col);
            g.fillPath (spear);

            // Subtle machined highlight ridge along one facet of the blade
            const juce::Point<float> ridgeEnd {
                pivot.x + sinA * (length * 0.90f) - nx * (baseHalfW * 0.22f),
                pivot.y - cosA * (length * 0.90f) - ny * (baseHalfW * 0.22f)
            };
            const juce::Point<float> ridgeStart {
                pivot.x - nx * (baseHalfW * 0.36f),
                pivot.y - ny * (baseHalfW * 0.36f)
            };
            g.setColour (col.interpolatedWith (juce::Colours::white, 0.42f).withMultipliedAlpha (0.62f));
            g.drawLine (ridgeStart.x, ridgeStart.y, ridgeEnd.x, ridgeEnd.y, 0.85f);
            break;
        }
    }
}

// ============================================================================
// 6. Needle Overlay (Live Needles + Peak Ghost + Machined Hub + dB Readout Pill)
// ============================================================================
void VuMeterModule::drawNeedleOverlay (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    if (! dialGeom.valid || dialGeom.radius <= 8.0f)
        return;

    const bool darkCanvas  = VuNeedleDetail::isDarkCanvas();
    const bool isRetroWarm = (needleDialStyle_ == NeedleDialStyle::retroWarm);
    const auto alertC      = vuAlertColour();
    const bool faceIsDark  = VuNeedleDetail::isNeedleFaceDark (needleDialStyle_, colorMap_,
                                                               darkCanvas, backlight_, alertC);
    const float minDb      = minDisplayDb();
    const float baseTh     = juce::jlimit (1.45f, 2.55f, dialGeom.radius * 0.023f);

    auto colourFor = [&] (float db, bool isRightCoaxial) -> juce::Colour
    {
        return VuNeedleDetail::resolveNeedleColour (
            needleDialStyle_, colorMap_, needleShape_,
            db, warnDbfs, isRightCoaxial, faceIsDark,
            vuPaletteColour (dialTFor (db), db, faceIsDark), alertC);
    };

    // ---- 1. Ghost peak-hold needle(s) (STRICTLY when peakHold == true!) ----
    const auto& tokens     = PinkXP::getSkinTokens();
    const bool isPixel     = (tokens.dialFinish == PinkXP::DialFinish::pixelHardware);
    const bool chromaKey   = tokens.isChromaKey;
    const bool greenScreen = PinkXP::isGreenScreenBackdrop();

    if (peakHold)
    {
        const float ghostTh = juce::jmax (1.0f, baseTh * 0.65f);
        auto drawPeakGhost = [&] (juce::Point<float> piv, float len, float pkDb, bool isRightCoax, float alpha)
        {
            if (! std::isfinite (pkDb) || pkDb <= minDb)
                return;
            if (isPixel)
            {
                VuNeedleDetail::drawCrispDashedNeedle (g, piv, len, needleAngleFor (pkDb),
                                                       colourFor (pkDb, isRightCoax).withMultipliedAlpha (0.72f),
                                                       ghostTh);
            }
            else
            {
                drawNeedlePointer (g, piv, len, dialGeom.tailLen * 0.6f,
                                   needleAngleFor (pkDb), colourFor (pkDb, isRightCoax),
                                   ghostTh, NeedleShape::thin, greenScreen ? 0.78f : alpha);
            }
        };

        if (isStereoMode())
        {
            drawPeakGhost (dialGeom.pivot, dialGeom.needleLen, peakHoldR, true,  0.42f);
            drawPeakGhost (dialGeom.pivot, dialGeom.needleLen, peakHoldL, false, 0.45f);
        }
        else
        {
            drawPeakGhost (dialGeom.pivot, dialGeom.needleLen, activeSinglePeakDbfs(), false, 0.46f);
        }
    }

    // ---- 2. Live sweeping needle(s) ----
    if (isStereoMode())
    {
        // Coaxial Stereo L/R needles: ALWAYS one unified dial — draw R in distinct contrasting color, then L on top
        const auto colR = colourFor (displayedR, true);
        const auto colL = colourFor (displayedL, false);
        drawNeedlePointer (g, dialGeom.pivot, dialGeom.needleLen, dialGeom.tailLen,
                           needleAngleFor (displayedR), colR,
                           baseTh * 0.88f, needleShape_, 0.96f);
        drawNeedlePointer (g, dialGeom.pivot, dialGeom.needleLen, dialGeom.tailLen,
                           needleAngleFor (displayedL), colL,
                           baseTh * 1.00f, needleShape_, 1.0f);

        // Clean color-coded "L / R" legend pill tucked cleanly at top-left (away from top-right Signal LED)
        if (! chromaKey && dialArea.getWidth() >= 96 && dialArea.getHeight() >= 60)
        {
            const bool insideRetroGlass = isRetroWarm && ! showLed;
            const bool isGlassLeg = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
            const bool isNeonLeg  = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
            const bool isCrtLeg   = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
            const float lx = (float) dialArea.getX() + (insideRetroGlass ? 13.0f : 8.0f);
            const float ly = (float) dialArea.getY() + (insideRetroGlass ? 11.0f : (isRetroWarm ? 4.0f : 6.0f));
            const float pillW = (isNeonLeg || isCrtLeg) ? 50.0f : 46.0f;
            const float pillH = 14.0f;
            const juce::Rectangle<float> legBox (lx, ly, pillW, pillH);

            const bool legDark = insideRetroGlass ? faceIsDark : darkCanvas;
            const juce::Colour legBg = (isNeonLeg && legDark)
                                           ? juce::Colour (0xff0a0d16).withAlpha (0.94f)
                                           : (legDark
                                                  ? juce::Colour (0xff0c0e14).withAlpha (0.86f)
                                                  : PinkXP::content.interpolatedWith (PinkXP::ink, 0.05f).withAlpha (0.94f));
            const juce::Colour legBorder = isNeonLeg
                                               ? PinkXP::sel.withAlpha (0.85f)
                                               : (isCrtLeg
                                                      ? PinkXP::sel.withAlpha (0.80f)
                                                      : (legDark
                                                             ? juce::Colours::white.withAlpha (0.18f)
                                                             : PinkXP::ink.withAlpha (0.24f)));

            if (isPixel || isGlassLeg || isNeonLeg || isCrtLeg || tokens.badgeRadius < 0.5f)
            {
                const auto legI = legBox.toNearestInt();
                PinkXP::Craft::drawBadgePill (g, legI, legBg, legBorder);
                if (tokens.dialFinish == PinkXP::DialFinish::brushedMetal && legI.getWidth() >= 6 && legI.getHeight() >= 6)
                {
                    g.setColour (juce::Colours::white.withAlpha (legDark ? 0.24f : 0.62f));
                    g.fillRect (legI.getX() + 1, legI.getY() + 1, legI.getWidth() - 2, 1);
                }
            }
            else
            {
                g.setColour (legBg);
                g.fillRoundedRectangle (legBox, 3.5f);
                g.setColour (legBorder);
                g.drawRoundedRectangle (legBox.reduced (0.5f), 3.5f, 0.9f);
            }

            const juce::Colour legInk = PinkXP::readableInkOn (legBg);
            auto drawLegendItem = [&] (float x, const char* tag, juce::Colour swatch)
            {
                const float cy = legBox.getCentreY();
                g.setColour (swatch);
                if (isPixel || ! tokens.statusDotCircular)
                    g.fillRect (juce::roundToInt (x), juce::roundToInt (cy) - 2, 4, 4);
                else
                    g.fillEllipse (x, cy - 2.3f, 4.6f, 4.6f);
                PinkXP::drawCachedText (g, tag, juce::Rectangle<float> (x + 6.0f, ly, 12.0f, pillH),
                                        juce::Justification::centredLeft, isCrtLeg ? 7.5f : 8.0f, juce::Font::bold,
                                        legInk, true);
            };
            const float stepX = (isNeonLeg || isCrtLeg) ? 21.5f : 20.0f;
            drawLegendItem (lx + 4.5f,         "L", colL);
            drawLegendItem (lx + 4.5f + stepX, "R", colR);
        }
    }
    else
    {
        const float liveDb = activeSingleDbfs();
        drawNeedlePointer (g, dialGeom.pivot, dialGeom.needleLen, dialGeom.tailLen,
                           needleAngleFor (liveDb), colourFor (liveDb, false),
                           baseTh, needleShape_, 1.0f);
    }

    // ---- 3. Machined pivot hub cap / Win95 pivot base block cleanly over needle roots ----
    VuNeedleDetail::drawMachinedPivotCap (g, dialGeom.pivot, dialGeom.radius, dialGeom.capR,
                                          darkCanvas, isRetroWarm);

    // ---- 4. Digital dB mini-readout pill(s) (ONLY when showReadout_ is ON) ----
    if (showReadout_ && dialArea.getHeight() >= 76)
    {
        const float pillH = juce::jlimit (12.0f, 15.0f, (float) dialArea.getHeight() * 0.095f);
        const float pillY = (float) dialArea.getBottom() - pillH - 3.0f;

        if (isStereoMode())
        {
            const auto colL = colourFor (displayedL, false);
            const auto colR = colourFor (displayedR, true);
            const float cx  = (float) dialArea.getCentreX();

            if (dialArea.getWidth() >= 142)
            {
                const float halfAvail = ((float) dialArea.getWidth() - 18.0f) * 0.5f;
                const float gap       = 5.0f;
                const float eachMax   = juce::jmin (84.0f, halfAvail - gap * 0.5f);

                VuNeedleDetail::drawDbReadoutPill (g, cx - eachMax * 0.5f - gap * 0.5f, pillY, eachMax, pillH,
                                                   "L " + formatDbReadout (displayedL),
                                                   displayedL >= warnDbfs, darkCanvas, alertC, colL);
                VuNeedleDetail::drawDbReadoutPill (g, cx + eachMax * 0.5f + gap * 0.5f, pillY, eachMax, pillH,
                                                   "R " + formatDbReadout (displayedR),
                                                   displayedR >= warnDbfs, darkCanvas, alertC, colR);
            }
            else
            {
                const juce::String dualTxt = "L " + formatDbReadout (displayedL)
                                           + "  R " + formatDbReadout (displayedR);
                const bool anyHot = (displayedL >= warnDbfs || displayedR >= warnDbfs);
                VuNeedleDetail::drawDbReadoutPill (g, cx, pillY,
                                                   (float) dialArea.getWidth() - 12.0f, pillH,
                                                   dualTxt, anyHot, darkCanvas, alertC);
            }
        }
        else
        {
            const float liveDb = activeSingleDbfs();
            const char* chTag  = activeChannelTag();
            const juce::String readoutTxt = (chTag != nullptr)
                                                ? (juce::String (chTag) + " " + formatDbReadout (liveDb))
                                                : formatDbReadout (liveDb);
            VuNeedleDetail::drawDbReadoutPill (g, dialGeom.pivot.x, pillY,
                                               (float) dialArea.getWidth() - 16.0f, pillH,
                                               readoutTxt,
                                               liveDb >= warnDbfs, darkCanvas, alertC);
        }
    }
}
