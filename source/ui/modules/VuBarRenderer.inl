// ============================================================================
// VuBarRenderer.inl
// 5 studio-grade Horizontal Bar VU Meter renderers included at the bottom of
// FineSplitModules.cpp:
//   1. drawBarStudioDial    (Studio bar)
//   2. drawBarLedBlocksDial (LED blocks)
//   3. drawBarLedLadderDial (LED ladder)
//   4. drawBarMirrorDial    (Mirrored)
//   5. drawBarMinimalDial   (Minimal)
// ============================================================================

namespace vuBarDetail
{
    inline bool isDarkCanvas() noexcept
    {
        return PinkXP::content.getPerceivedBrightness() < 0.5f;
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

    inline bool shouldDrawPeak (bool peakHoldEnabled, float peakDb, float minDb) noexcept
    {
        return peakHoldEnabled && std::isfinite (peakDb) && peakDb > minDb + 0.05f;
    }

    inline bool isSignalActive (float db, float minDb) noexcept
    {
        return std::isfinite (db) && db > minDb + 0.05f;
    }

    // Strip any trailing " dBFS" / " dB" so Minimal focal numerals and compact
    // header pills never duplicate units regardless of formatDbReadout format.
    inline juce::String numericReadoutOnly (const juce::String& formatted)
    {
        auto s = formatted.trim();
        if (s.endsWithIgnoreCase (" dBFS"))
            s = s.dropLastCharacters (5).trimEnd();
        else if (s.endsWithIgnoreCase (" dB"))
            s = s.dropLastCharacters (3).trimEnd();
        return s;
    }

    inline juce::Colour troughBackground (bool darkCanvas) noexcept
    {
        return darkCanvas
             ? PinkXP::content.interpolatedWith (juce::Colour (0xff040508), 0.68f)
             : PinkXP::content.interpolatedWith (PinkXP::ink, 0.085f);
    }

    inline juce::Colour pillBackground (bool darkCanvas) noexcept
    {
        return darkCanvas
             ? PinkXP::content.interpolatedWith (juce::Colour (0xff05060a), 0.55f)
             : PinkXP::content.interpolatedWith (PinkXP::ink, 0.055f);
    }

    inline void drawChannelTag (juce::Graphics& g, const char* tag,
                                float x, float centreY, juce::Colour accentCol,
                                bool active, bool darkCanvas)
    {
        if (isChromaKey())
            return;

        const auto col = active
                       ? (darkCanvas ? accentCol.interpolatedWith (PinkXP::ink, 0.45f)
                                     : PinkXP::ink.withAlpha (0.86f))
                       : PinkXP::ink.withAlpha (0.55f);
        PinkXP::drawCachedText (g, tag,
                                juce::Rectangle<float> (x, centreY - 6.0f, 12.0f, 12.0f),
                                juce::Justification::centredLeft, 9.0f, juce::Font::bold,
                                col, true);
    }

    // Recessed studio hardware meter trough with subtle 1px bevel frame (or Win95 sunken well in Pixel XP).
    inline void drawRecessedTrough (juce::Graphics& g,
                                    juce::Rectangle<float> r,
                                    float cornerRadius,
                                    bool darkCanvas)
    {
        const auto& tokens = PinkXP::getSkinTokens();
        if (tokens.isChromaKey)
            return;

        const auto bg = troughBackground (darkCanvas);
        if (tokens.dialFinish == PinkXP::DialFinish::pixelHardware)
        {
            PinkXP::drawSunken (g, r.toNearestInt(), bg);
            return;
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
         || tokens.frameStyle == PinkXP::FrameStyle::neonTube
         || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            const auto wellBg = (tokens.frameStyle == PinkXP::FrameStyle::neonTube && darkCanvas)
                                    ? juce::Colour (0xff0a0d16)
                                    : bg;
            PinkXP::Craft::drawPanelWell (g, r.toNearestInt(), wellBg, PinkXP::WellKind::trough);
            return;
        }

        if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
        {
            // Rounded 4.5px frosted acrylic trough with crisp top-inner white refraction catchlight
            const float rad = juce::jmin (4.5f, juce::jmin (r.getWidth(), r.getHeight()) * 0.42f);
            const auto glassBg = darkCanvas
                                     ? bg.interpolatedWith (juce::Colour (0xff121828), 0.35f)
                                     : bg.interpolatedWith (juce::Colours::white, 0.38f);
            g.setColour (glassBg);
            g.fillRoundedRectangle (r, rad);
            g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.48f : 0.30f));
            g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);

            const auto inner = r.reduced (1.5f);
            if (inner.getWidth() > 6.0f && inner.getHeight() > 3.0f)
            {
                const float inset = juce::jmin (rad * 0.65f, inner.getWidth() * 0.25f);
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.28f : 0.78f));
                g.fillRect (inner.getX() + inset, inner.getY(),
                            juce::jmax (1.0f, inner.getWidth() - inset * 2.0f), 1.0f);
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.14f : 0.45f));
                g.fillRect (inner.getX() + inset, inner.getBottom() - 1.0f,
                            juce::jmax (1.0f, inner.getWidth() - inset * 2.0f), 1.0f);
            }
            return;
        }

        if (cornerRadius > 0.5f)
        {
            g.setColour (bg);
            g.fillRoundedRectangle (r, cornerRadius);

            // Outer refined hairline contour
            const auto outline = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
                                     ? PinkXP::skinHairlineOn (bg, darkCanvas ? 0.22f : 0.22f)
                                     : PinkXP::ink.withAlpha (darkCanvas ? 0.22f : 0.24f);
            g.setColour (outline);
            g.drawRoundedRectangle (r.reduced (0.5f), cornerRadius, 1.0f);

            // Subtle recessed top inner shadow & bottom lip highlight
            const auto inner = r.reduced (1.5f);
            if (inner.getWidth() > 4.0f && inner.getHeight() > 3.0f)
            {
                g.setColour (darkCanvas ? juce::Colours::black.withAlpha (0.48f)
                                        : PinkXP::ink.withAlpha (0.14f));
                g.fillRect (inner.getX() + cornerRadius * 0.5f, inner.getY(),
                            juce::jmax (1.0f, inner.getWidth() - cornerRadius), 1.0f);

                g.setColour (darkCanvas ? PinkXP::ink.withAlpha (0.10f)
                                        : juce::Colours::white.withAlpha (0.65f));
                g.fillRect (inner.getX() + cornerRadius * 0.5f, inner.getBottom() - 1.0f,
                            juce::jmax (1.0f, inner.getWidth() - cornerRadius), 1.0f);
            }
        }
        else
        {
            g.setColour (bg);
            g.fillRect (r);
            g.setColour (PinkXP::ink.withAlpha (darkCanvas ? 0.22f : 0.24f));
            g.drawRect (r, 1.0f);
        }
    }

    // Build a silky multi-stop horizontal ColourGradient across [leftX, rightX]
    // aligned to exact dB positions (including sharp accuracy at caution/warn).
    template <typename PaletteFn>
    inline juce::ColourGradient makePaletteGradient (float leftX, float rightX,
                                                     float minDb, float maxDb,
                                                     float cautionDb, float warnDb,
                                                     bool darkCanvas,
                                                     float alphaScale,
                                                     PaletteFn&& paletteAt)
    {
        const float span = juce::jmax (1.0e-4f, maxDb - minDb);
        auto colAtT = [&] (float t) -> juce::Colour
        {
            t = juce::jlimit (0.0f, 1.0f, t);
            const float db = minDb + t * span;
            return paletteAt (t, db, darkCanvas).withMultipliedAlpha (alphaScale);
        };

        juce::ColourGradient grad (colAtT (0.0f), leftX, 0.0f,
                                   colAtT (1.0f), rightX, 0.0f, false);

        constexpr int kSteps = 22;
        for (int i = 1; i < kSteps; ++i)
        {
            const float t = (float) i / (float) kSteps;
            grad.addColour ((double) t, colAtT (t));
        }

        auto addPin = [&] (float dbPin)
        {
            const float t = (dbPin - minDb) / span;
            if (t > 0.004f && t < 0.996f)
                grad.addColour ((double) t, colAtT (t));
        };

        // Pin exact transitions right around cautionDbfs and warnDbfs so the red
        // overload edge stays razor-accurate with the warnDbfs hairline.
        addPin (cautionDb - 0.25f);
        addPin (cautionDb + 0.25f);
        addPin (warnDb - 0.12f);
        addPin (warnDb + 0.06f);
        return grad;
    }

    // Subtle full-scale reference tint inside an unlit trough so the meter scale
    // reads immediately even during silence.
    template <typename PaletteFn>
    inline void drawReferenceZoneTint (juce::Graphics& g,
                                       juce::Rectangle<float> fillArea,
                                       float leftX, float rightX,
                                       float minDb, float maxDb,
                                       float cautionDb, float warnDb,
                                       float warnX,
                                       bool darkCanvas,
                                       PaletteFn&& paletteAt)
    {
        if (isChromaKey() || fillArea.getWidth() < 4.0f || fillArea.getHeight() < 2.0f)
            return;

        if (isPixelHardware())
        {
            const auto areaI = fillArea.toNearestInt();
            if (areaI.isEmpty())
                return;

            const int iWarnX = juce::jlimit (areaI.getX(), areaI.getRight(), juce::roundToInt (warnX));
            const auto safeCol = paletteAt (0.35f, minDb + (maxDb - minDb) * 0.35f, darkCanvas);
            g.setColour (safeCol.withAlpha (darkCanvas ? 0.08f : 0.07f));
            g.fillRect (areaI.getX(), areaI.getY(), juce::jmax (0, iWarnX - areaI.getX()), areaI.getHeight());

            if (iWarnX < areaI.getRight())
            {
                const auto alertCol = paletteAt (1.0f, maxDb, darkCanvas);
                g.setColour (alertCol.withAlpha (darkCanvas ? 0.14f : 0.12f));
                g.fillRect (iWarnX, areaI.getY(), areaI.getRight() - iWarnX, areaI.getHeight());
            }
            return;
        }

        auto refGrad = makePaletteGradient (leftX, rightX, minDb, maxDb,
                                            cautionDb, warnDb, darkCanvas,
                                            darkCanvas ? 0.09f : 0.085f,
                                            paletteAt);
        g.setGradientFill (refGrad);
        g.fillRect (fillArea);

        // Slightly more legible warm red-zone tint past warnDbfs
        if (warnX < fillArea.getRight() - 1.0f)
        {
            const float rx = juce::jmax (fillArea.getX(), warnX);
            const auto alertCol = paletteAt (1.0f, maxDb, darkCanvas);
            g.setColour (alertCol.withAlpha (darkCanvas ? 0.09f : 0.08f));
            g.fillRect (juce::Rectangle<float> (rx, fillArea.getY(),
                                                fillArea.getRight() - rx,
                                                fillArea.getHeight()));
        }
    }

    // Draw horizontal bar fill respecting BarFinish (smooth / notched / glow).
    template <typename PaletteFn, typename LinearXFn>
    inline void drawStyledBarFill (juce::Graphics& g,
                                   juce::Rectangle<float> innerRect,
                                   float leftX, float rightX,
                                   float levelDb,
                                   float minDb, float maxDb,
                                   float cautionDb, float warnDb,
                                   VuMeterModule::BarFinish finish,
                                   bool darkCanvas,
                                   bool sheenFromBottom,
                                   float maxHaloPx,
                                   PaletteFn&& paletteAt,
                                   LinearXFn&& xForDb)
    {
        const float scaleW = rightX - leftX;
        if (scaleW < 4.0f || innerRect.getHeight() < 2.0f)
            return;

        const bool chromaKey   = isChromaKey();
        const bool greenScreen = isGreenScreen();
        const bool active = isSignalActive (levelDb, minDb);
        const float rawLitX = active ? xForDb (levelDb) : leftX;
        const float xLit = juce::jlimit (innerRect.getX(), innerRect.getRight(), rawLitX);
        const float litW = xLit - innerRect.getX();

        if (isPixelHardware())
        {
            // ---- Pixel XP Studio/Mirrored Bar: Quantized integer pixel columns + 1px bevel highlight ----
            const auto innerI  = innerRect.toNearestInt();
            const int  litEndX = juce::jlimit (innerI.getX(), innerI.getRight(), juce::roundToInt (rawLitX));
            const int  iLitW   = litEndX - innerI.getX();

            if (iLitW > 0 && innerI.getHeight() > 0)
            {
                const int stepPx = (scaleW >= 120.0f) ? 3 : 2;
                const float spanDb = juce::jmax (1.0e-4f, maxDb - minDb);

                // BarFinish::glow -> crisp 1px outer pixel halo top & bottom (suppressed ONLY in greenScreen)
                if (finish == VuMeterModule::BarFinish::glow && ! greenScreen && maxHaloPx >= 1.0f)
                {
                    for (int sx = innerI.getX(); sx < litEndX; sx += stepPx)
                    {
                        const int sw = juce::jmin (stepPx, litEndX - sx);
                        const float t = juce::jlimit (0.0f, 1.0f, ((float) sx + (float) sw * 0.5f - leftX) / scaleW);
                        const auto c = paletteAt (t, minDb + t * spanDb, darkCanvas);
                        g.setColour (c.withAlpha (darkCanvas ? 0.36f : 0.24f));
                        g.fillRect (sx, innerI.getY() - 1, sw, 1);
                        g.fillRect (sx, innerI.getBottom(), sw, 1);
                    }
                }

                // Quantized stepped pixel color blocks across active span
                for (int sx = innerI.getX(); sx < litEndX; sx += stepPx)
                {
                    const int sw = juce::jmin (stepPx, litEndX - sx);
                    const float t = juce::jlimit (0.0f, 1.0f, ((float) sx + (float) sw * 0.5f - leftX) / scaleW);
                    const auto c = paletteAt (t, minDb + t * spanDb, darkCanvas);
                    g.setColour (c);
                    g.fillRect (sx, innerI.getY(), sw, innerI.getHeight());
                }

                if (finish == VuMeterModule::BarFinish::glow)
                {
                    // Crisp 1px (or 2px) bright inner pixel filament core
                    const int coreH = (innerI.getHeight() >= 8) ? 2 : 1;
                    const int coreY = innerI.getCentreY() - coreH / 2;
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.62f : 0.52f));
                    g.fillRect (innerI.getX(), coreY, iLitW, coreH);

                    if (innerI.getHeight() >= 5)
                    {
                        g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.22f : 0.28f));
                        g.fillRect (innerI.getX(), innerI.getY(), iLitW, 1);
                    }
                }
                else
                {
                    // Crisp 1px top pixel highlight & 1px bottom dark pixel shade
                    const int hiY   = sheenFromBottom ? (innerI.getBottom() - 1) : innerI.getY();
                    const int shdwY = sheenFromBottom ? innerI.getY() : (innerI.getBottom() - 1);
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.30f : 0.38f));
                    g.fillRect (innerI.getX(), hiY, iLitW, 1);
                    if (innerI.getHeight() >= 4)
                    {
                        g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.32f : 0.20f));
                        g.fillRect (innerI.getX(), shdwY, iLitW, 1);
                    }
                }

                // Crisp 1px bright leading front pixel edge
                if (iLitW >= 3)
                {
                    const float tEdge = juce::jlimit (0.0f, 1.0f, ((float) litEndX - leftX) / scaleW);
                    const auto edgeBase = paletteAt (tEdge, levelDb, darkCanvas);
                    g.setColour (edgeBase.interpolatedWith (juce::Colours::white, 0.58f));
                    g.fillRect (litEndX - 1, innerI.getY(), 1, innerI.getHeight());
                }
            }

            // Crisp 2px integer-snapped notch dividers for BarFinish::notched
            if (finish == VuMeterModule::BarFinish::notched)
            {
                const float spanDb  = juce::jmax (1.0f, maxDb - minDb);
                const float pxPerDb = scaleW / spanDb;
                const float stepDb  = (pxPerDb >= 6.5f) ? 1.0f
                                    : (pxPerDb >= 3.2f) ? 2.0f
                                    : (pxPerDb >= 1.9f) ? 3.0f
                                                        : 6.0f;
                const int notchW = (pxPerDb * stepDb >= 8.0f) ? 2 : 1;
                const auto litNotchCol = greenScreen
                                           ? PinkXP::chromaKeyGreen()
                                           : troughBackground (darkCanvas).withAlpha (chromaKey ? 0.78f : 0.94f);
                const auto dimNotchCol = PinkXP::ink.withAlpha (0.18f);

                const float firstDb = std::ceil (minDb / stepDb) * stepDb;
                for (float db = firstDb; db < maxDb - 0.1f; db += stepDb)
                {
                    if (db <= minDb + 0.1f)
                        continue;
                    const int nx = juce::roundToInt (xForDb (db));
                    if (nx <= innerI.getX() + 1 || nx >= innerI.getRight() - 1)
                        continue;
                    const bool insideLit = active && nx <= litEndX;
                    if (! insideLit && chromaKey)
                        continue;
                    g.setColour (insideLit ? litNotchCol : dimNotchCol);
                    g.fillRect (nx - notchW / 2, innerI.getY(), notchW, innerI.getHeight());
                }
            }
            return;
        }

        if (isBrushedMetal())
        {
            // ---- Millennium Chrome Studio/Mirrored Bar: 3-stop CNC metallic sheen + crisp milled edges ----
            if (finish == VuMeterModule::BarFinish::glow && ! greenScreen && litW > 1.0f && maxHaloPx >= 1.2f)
            {
                const float haloY = juce::jlimit (1.2f, 3.2f, maxHaloPx * 0.78f);
                const auto glowGrad = makePaletteGradient (leftX, rightX, minDb, maxDb,
                                                           cautionDb, warnDb, darkCanvas,
                                                           darkCanvas ? 0.26f : 0.16f,
                                                           paletteAt);
                g.setGradientFill (glowGrad);
                g.fillRect (juce::Rectangle<float> (innerRect.getX(),
                                                    innerRect.getY() - haloY,
                                                    juce::jmin (innerRect.getWidth() + 1.5f, litW + 1.5f),
                                                    innerRect.getHeight() + haloY * 2.0f));
            }

            if (litW > 0.5f)
            {
                const juce::Rectangle<float> litRect (innerRect.getX(), innerRect.getY(),
                                                      litW, innerRect.getHeight());
                auto mainGrad = makePaletteGradient (leftX, rightX, minDb, maxDb,
                                                     cautionDb, warnDb, darkCanvas, 1.0f,
                                                     paletteAt);
                g.setGradientFill (mainGrad);
                g.fillRect (litRect);

                // Signature 3-stop metallic crease highlight & grounded lower bevel
                const float yTop = litRect.getY();
                const float yBot = litRect.getBottom();
                const float y0   = sheenFromBottom ? yBot : yTop;
                const float y1   = sheenFromBottom ? yTop : yBot;
                juce::ColourGradient metalSheen (juce::Colours::white.withAlpha (darkCanvas ? 0.36f : 0.44f),
                                                 0.0f, y0,
                                                 juce::Colours::black.withAlpha (darkCanvas ? 0.34f : 0.22f),
                                                 0.0f, y1, false);
                metalSheen.addColour (0.42, juce::Colours::white.withAlpha (darkCanvas ? 0.12f : 0.16f));
                metalSheen.addColour (0.50, juce::Colours::black.withAlpha (darkCanvas ? 0.10f : 0.06f));
                g.setGradientFill (metalSheen);
                g.fillRect (litRect);

                // Crisp 1px upper chrome catchlight + 1px dark milled lower bevel
                const float hiY  = sheenFromBottom ? (yBot - 1.0f) : yTop;
                const float loY  = sheenFromBottom ? yTop : (yBot - 1.0f);
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.46f : 0.56f));
                g.fillRect (juce::Rectangle<float> (litRect.getX(), hiY, litW, 1.0f));
                if (litRect.getHeight() >= 4.0f)
                {
                    g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.38f : 0.24f));
                    g.fillRect (juce::Rectangle<float> (litRect.getX(), loY, litW, 1.0f));
                }

                if (finish == VuMeterModule::BarFinish::glow)
                {
                    const float coreH = juce::jmax (1.5f, litRect.getHeight() * 0.28f);
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.54f : 0.46f));
                    g.fillRect (juce::Rectangle<float> (litRect.getX(), litRect.getCentreY() - coreH * 0.5f,
                                                        litW, coreH));
                }

                // Razor-bright machined chrome leading edge
                if (litW >= 2.5f)
                {
                    const float tEdge = juce::jlimit (0.0f, 1.0f, (xLit - leftX) / scaleW);
                    const auto edgeCol = paletteAt (tEdge, levelDb, darkCanvas)
                                             .interpolatedWith (juce::Colours::white, 0.65f);
                    g.setColour (edgeCol);
                    g.fillRect (juce::Rectangle<float> (xLit - 1.5f, yTop, 1.5f, litRect.getHeight()));
                }
            }

            if (finish == VuMeterModule::BarFinish::notched)
            {
                const float spanDb  = juce::jmax (1.0f, maxDb - minDb);
                const float pxPerDb = scaleW / spanDb;
                const float stepDb  = (pxPerDb >= 6.5f) ? 1.0f
                                    : (pxPerDb >= 3.2f) ? 2.0f
                                    : (pxPerDb >= 1.9f) ? 3.0f
                                                        : 6.0f;
                const float notchW = (pxPerDb * stepDb >= 8.0f) ? 2.0f : 1.25f;
                const auto litNotchCol = greenScreen
                                           ? PinkXP::chromaKeyGreen()
                                           : troughBackground (darkCanvas).withAlpha (chromaKey ? 0.80f : 0.90f);
                const auto dimNotchCol = PinkXP::ink.withAlpha (0.16f);

                const float firstDb = std::ceil (minDb / stepDb) * stepDb;
                for (float db = firstDb; db < maxDb - 0.1f; db += stepDb)
                {
                    if (db <= minDb + 0.1f)
                        continue;
                    const float nx = xForDb (db);
                    if (nx <= innerRect.getX() + 1.5f || nx >= innerRect.getRight() - 1.5f)
                        continue;
                    const bool insideLit = active && nx <= xLit + 0.5f;
                    if (! insideLit && chromaKey)
                        continue;
                    g.setColour (insideLit ? litNotchCol : dimNotchCol);
                    g.fillRect (juce::Rectangle<float> (nx - notchW * 0.5f, innerRect.getY(),
                                                        notchW, innerRect.getHeight()));
                    if (insideLit && ! greenScreen && notchW >= 1.5f)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.24f));
                        g.fillRect (juce::Rectangle<float> (nx + notchW * 0.5f, innerRect.getY(),
                                                            0.65f, innerRect.getHeight()));
                    }
                }
            }
            return;
        }

        const bool isGlassBar = isGlassDome();
        const bool isNeonBar  = isNeonRings();
        const bool isCrtBar   = isCrtVector();
        const bool barDark    = darkCanvas;

        // 1) Optional outer neon/phosphor bloom halo (skip ONLY on greenScreen to avoid green fringing)
        if ((finish == VuMeterModule::BarFinish::glow || isNeonBar || isCrtBar)
            && ! greenScreen && litW > 1.0f && maxHaloPx >= 1.2f)
        {
            const float haloY = juce::jlimit (1.2f, isNeonBar ? 4.8f : 4.5f,
                                              isCrtBar ? (maxHaloPx * 0.60f) : maxHaloPx);
            const auto glowGradOuter = makePaletteGradient (leftX, rightX, minDb, maxDb,
                                                            cautionDb, warnDb, barDark,
                                                            isNeonBar ? 0.30f : (barDark ? 0.24f : 0.16f),
                                                            paletteAt);
            const auto glowGradInner = makePaletteGradient (leftX, rightX, minDb, maxDb,
                                                            cautionDb, warnDb, barDark,
                                                            isNeonBar ? 0.44f : (barDark ? 0.36f : 0.22f),
                                                            paletteAt);
            const juce::Rectangle<float> outerBloom (innerRect.getX(),
                                                     innerRect.getY() - haloY,
                                                     juce::jmin (innerRect.getWidth() + 2.0f, litW + 2.5f),
                                                     innerRect.getHeight() + haloY * 2.0f);
            g.setGradientFill (glowGradOuter);
            if (isCrtBar) g.fillRect (outerBloom);
            else          g.fillRoundedRectangle (outerBloom, isNeonBar ? 2.0f : 3.0f);

            const float midHalo = haloY * 0.5f;
            const juce::Rectangle<float> innerBloom (innerRect.getX(),
                                                     innerRect.getY() - midHalo,
                                                     juce::jmin (innerRect.getWidth() + 1.0f, litW + 1.2f),
                                                     innerRect.getHeight() + midHalo * 2.0f);
            g.setGradientFill (glowGradInner);
            if (isCrtBar) g.fillRect (innerBloom);
            else          g.fillRoundedRectangle (innerBloom, isNeonBar ? 1.0f : 2.0f);
        }

        // 2) Main horizontal multi-stop palette fill
        if (litW > 0.5f)
        {
            const juce::Rectangle<float> litRect (innerRect.getX(), innerRect.getY(),
                                                  litW, innerRect.getHeight());
            auto mainGrad = makePaletteGradient (leftX, rightX, minDb, maxDb,
                                                 cautionDb, warnDb, barDark, 1.0f,
                                                 paletteAt);
            g.setGradientFill (mainGrad);
            if (isGlassBar)
                g.fillRoundedRectangle (litRect, juce::jmin (3.0f, litRect.getHeight() * 0.45f));
            else
                g.fillRect (litRect);

            if (isGlassBar)
            {
                // Luminous Frutiger Aero glass-dome sheen: bright upper 44% dome + soft luminous belly bounce
                const float domeH = juce::jmax (1.2f, innerRect.getHeight() * 0.44f);
                const float domeY = sheenFromBottom ? (innerRect.getBottom() - domeH) : innerRect.getY();
                juce::ColourGradient domeG (
                    juce::Colours::white.withAlpha (sheenFromBottom ? 0.10f : (darkCanvas ? 0.42f : 0.52f)),
                    0.0f, domeY,
                    juce::Colours::white.withAlpha (sheenFromBottom ? (darkCanvas ? 0.42f : 0.52f) : 0.10f),
                    0.0f, domeY + domeH, false);
                g.setGradientFill (domeG);
                g.fillRect (juce::Rectangle<float> (litRect.getX(), domeY, litW, domeH));

                // Crisp 1px bright specular crystal crest + faint bottom refraction bounce
                const float topRimY = sheenFromBottom ? (innerRect.getBottom() - 1.0f) : innerRect.getY();
                const float botRimY = sheenFromBottom ? innerRect.getY() : (innerRect.getBottom() - 1.0f);
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.52f : 0.72f));
                g.fillRect (juce::Rectangle<float> (litRect.getX(), topRimY, litW, 1.0f));
                if (innerRect.getHeight() >= 5.0f)
                {
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.16f : 0.24f));
                    g.fillRect (juce::Rectangle<float> (litRect.getX(), botRimY, litW, 1.0f));
                }
            }
            else if (isNeonBar || finish == VuMeterModule::BarFinish::glow)
            {
                // Blazing hot white-tinted center neon laser filament
                const float coreH = juce::jmax (1.5f, innerRect.getHeight() * (isNeonBar ? 0.40f : 0.34f));
                const float coreY = innerRect.getCentreY() - coreH * 0.5f;
                g.setColour (juce::Colours::white.withAlpha (isNeonBar ? 0.62f : (barDark ? 0.42f : 0.36f)));
                g.fillRect (juce::Rectangle<float> (litRect.getX(), coreY, litW, coreH));

                if (innerRect.getHeight() >= 6.0f)
                {
                    g.setColour (juce::Colours::white.withAlpha (isNeonBar ? 0.28f : (barDark ? 0.16f : 0.22f)));
                    g.fillRect (juce::Rectangle<float> (litRect.getX(), litRect.getY(), litW, 1.0f));
                }
            }
            else if (isCrtBar)
            {
                // Crisp CRT phosphor bar: subtle center phosphor core + 2px scanlines
                const float coreH = juce::jmax (1.0f, innerRect.getHeight() * 0.26f);
                g.setColour (juce::Colours::white.withAlpha (0.34f));
                g.fillRect (juce::Rectangle<float> (litRect.getX(), innerRect.getCentreY() - coreH * 0.5f, litW, coreH));
                const auto li = litRect.toNearestInt();
                if (li.getHeight() >= 4)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.16f));
                    for (int py = li.getY() + 1; py < li.getBottom(); py += 2)
                        g.fillRect (li.getX(), py, li.getWidth(), 1);
                }
            }
            else
            {
                // Smooth & Notched: crisp studio glass specular sheen + subtle depth edge
                const float sheenH = juce::jmax (1.0f, innerRect.getHeight() * 0.38f);
                const float sheenY = sheenFromBottom ? (innerRect.getBottom() - sheenH)
                                                     : innerRect.getY();
                juce::ColourGradient sheen (
                    juce::Colours::white.withAlpha (sheenFromBottom ? 0.0f : (darkCanvas ? 0.24f : 0.30f)),
                    0.0f, sheenY,
                    juce::Colours::white.withAlpha (sheenFromBottom ? (darkCanvas ? 0.24f : 0.30f) : 0.0f),
                    0.0f, sheenY + sheenH,
                    false);
                g.setGradientFill (sheen);
                g.fillRect (juce::Rectangle<float> (litRect.getX(), sheenY, litW, sheenH));

                if (innerRect.getHeight() >= 5.0f)
                {
                    const float shadeY = sheenFromBottom ? innerRect.getY()
                                                         : (innerRect.getBottom() - 1.0f);
                    g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.28f : 0.14f));
                    g.fillRect (juce::Rectangle<float> (litRect.getX(), shadeY, litW, 1.0f));
                }
            }

            // Crisp bright leading edge right at live level front
            if (litW >= 2.5f)
            {
                const float tEdge = juce::jlimit (0.0f, 1.0f, (xLit - leftX) / scaleW);
                const auto edgeBase = paletteAt (tEdge, levelDb, barDark);
                const auto edgeCol = (barDark || isNeonBar)
                                   ? edgeBase.interpolatedWith (juce::Colours::white, isNeonBar ? 0.72f : 0.58f)
                                   : edgeBase.interpolatedWith (juce::Colours::white, 0.42f);
                g.setColour (edgeCol);
                g.fillRect (juce::Rectangle<float> (xLit - 1.5f, innerRect.getY(),
                                                    1.5f, innerRect.getHeight()));
            }
        }

        // 3) Broadcast console notch dividers for BarFinish::notched
        if (finish == VuMeterModule::BarFinish::notched)
        {
            const float spanDb = juce::jmax (1.0f, maxDb - minDb);
            const float pxPerDb = scaleW / spanDb;
            const float stepDb = (pxPerDb >= 6.5f) ? 1.0f
                               : (pxPerDb >= 3.2f) ? 2.0f
                               : (pxPerDb >= 1.9f) ? 3.0f
                                                   : 6.0f;
            const float notchW = (pxPerDb * stepDb >= 8.0f) ? 2.0f : 1.25f;
            const auto litNotchCol = greenScreen
                                       ? PinkXP::chromaKeyGreen()
                                       : troughBackground (darkCanvas).withAlpha (chromaKey ? 0.78f : (darkCanvas ? 0.88f : 0.78f));
            const auto dimNotchCol = PinkXP::ink.withAlpha (darkCanvas ? 0.14f : 0.14f);

            const float firstDb = std::ceil (minDb / stepDb) * stepDb;
            for (float db = firstDb; db < maxDb - 0.1f; db += stepDb)
            {
                if (db <= minDb + 0.1f)
                    continue;
                const float nx = xForDb (db);
                if (nx <= innerRect.getX() + 1.5f || nx >= innerRect.getRight() - 1.5f)
                    continue;

                const bool insideLit = active && nx <= xLit + 0.5f;
                if (! insideLit && chromaKey)
                    continue;
                g.setColour (insideLit ? litNotchCol : dimNotchCol);
                g.fillRect (juce::Rectangle<float> (nx - notchW * 0.5f, innerRect.getY(),
                                                    notchW, innerRect.getHeight()));
            }
        }
    }

    // Crisp glowing peak-hold vertical cap (ONLY called when shouldDrawPeak is true!).
    template <typename PaletteFn>
    inline void drawPeakHoldCap (juce::Graphics& g,
                                 float peakX,
                                 float yTop,
                                 float yBot,
                                 float peakT,
                                 float peakDb,
                                 float warnDb,
                                 juce::Colour alertCol,
                                 bool darkCanvas,
                                 PaletteFn&& paletteAt)
    {
        const float h = yBot - yTop;
        if (h < 2.0f)
            return;

        const bool inAlert = peakDb >= warnDb;
        const auto baseCol = inAlert ? alertCol : paletteAt (peakT, peakDb, darkCanvas);
        const auto coreCol = darkCanvas
                           ? baseCol.interpolatedWith (juce::Colours::white, inAlert ? 0.35f : 0.52f)
                           : baseCol.darker (0.12f);

        if (isPixelHardware())
        {
            // Hard 2px integer pixel peak marker (zero rounded halo blur!)
            const int ipx   = juce::roundToInt (peakX);
            const int iyTop = juce::roundToInt (yTop);
            const int ih    = juce::jmax (2, juce::roundToInt (h));
            g.setColour (coreCol);
            g.fillRect (ipx - 1, iyTop, 2, ih);
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.78f : 0.55f));
            g.fillRect (ipx - 1, iyTop, 1, ih);
            return;
        }

        if (isBrushedMetal())
        {
            // Sharp 0px CNC machined metallic peak marker
            if (! isGreenScreen())
            {
                g.setColour (baseCol.withAlpha (darkCanvas ? 0.34f : 0.22f));
                g.fillRect (juce::Rectangle<float> (peakX - 2.5f, yTop - 0.5f, 5.0f, h + 1.0f));
            }
            g.setColour (coreCol);
            g.fillRect (juce::Rectangle<float> (peakX - 1.25f, yTop, 2.5f, h));
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.86f : 0.72f));
            g.fillRect (juce::Rectangle<float> (peakX - 1.25f, yTop, 0.95f, h));
            g.setColour (PinkXP::dark.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (peakX + 0.65f, yTop, 0.75f, h));
            return;
        }

        if (isGlassDome())
        {
            if (! isGreenScreen())
            {
                g.setColour (baseCol.withAlpha (darkCanvas ? 0.36f : 0.24f));
                g.fillRoundedRectangle (juce::Rectangle<float> (peakX - 3.0f, yTop - 0.5f, 6.0f, h + 1.0f), 2.5f);
            }
            const juce::Rectangle<float> cap (peakX - 1.5f, yTop, 3.0f, h);
            g.setColour (coreCol);
            g.fillRoundedRectangle (cap, 1.4f);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.fillRoundedRectangle (cap.withHeight (juce::jmax (1.5f, h * 0.42f)), 1.0f);
            return;
        }

        if (isNeonRings())
        {
            if (! isGreenScreen())
            {
                g.setColour (baseCol.withAlpha (0.34f));
                g.fillRect (juce::Rectangle<float> (peakX - 3.4f, yTop - 1.0f, 6.8f, h + 2.0f));
                g.setColour (baseCol.withAlpha (0.58f));
                g.fillRect (juce::Rectangle<float> (peakX - 2.0f, yTop - 0.5f, 4.0f, h + 1.0f));
            }
            g.setColour (baseCol.interpolatedWith (juce::Colours::white, 0.68f));
            g.fillRect (juce::Rectangle<float> (peakX - 1.1f, yTop, 2.2f, h));
            return;
        }

        if (isCrtVector())
        {
            const int ipx   = juce::roundToInt (peakX);
            const int iyTop = juce::roundToInt (yTop);
            const int ih    = juce::jmax (2, juce::roundToInt (h));
            if (! isGreenScreen())
            {
                g.setColour (baseCol.withAlpha (0.32f));
                g.fillRect (ipx - 2, iyTop, 4, ih);
            }
            g.setColour (baseCol.interpolatedWith (juce::Colours::white, 0.55f));
            g.fillRect (ipx - 1, iyTop, 2, ih);
            return;
        }

        // Soft outer glow halo (skipped ONLY on greenScreen to prevent green-screen alpha fringing)
        if (! isGreenScreen())
        {
            const auto haloCol = baseCol.withAlpha (darkCanvas ? 0.38f : 0.24f);
            g.setColour (haloCol);
            g.fillRoundedRectangle (juce::Rectangle<float> (peakX - 2.75f, yTop - 0.5f, 5.5f, h + 1.0f), 1.5f);
        }

        // Crisp 2.5px vertical bar cap
        g.setColour (coreCol);
        g.fillRect (juce::Rectangle<float> (peakX - 1.25f, yTop, 2.5f, h));

        // Razor-thin bright inner glint
        g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.72f : 0.55f));
        g.fillRect (juce::Rectangle<float> (peakX - 0.45f, yTop + 0.5f, 0.9f, juce::jmax (1.0f, h - 1.0f)));
    }

    // Anti-collision horizontal scale ruler supporting all 4 scale ranges and
    // arbitrarily small module widths without overlapping labels or clipping edges.
    enum class TickDirection { down, up, both };

    template <typename TickType, typename LinearXFn>
    inline void drawScaleRuler (juce::Graphics& g,
                                const std::vector<TickType>& ticks,
                                float leftX, float rightX,
                                float clipLeft, float clipRight,
                                float tickAnchorTop, float tickAnchorBot,
                                float labelY, float labelH,
                                TickDirection dir,
                                float tickLenMajor, float tickLenMinor,
                                bool drawLabels,
                                float minLabelGap,
                                bool sparsePreference,
                                float minDb, float maxDb, float warnDb,
                                juce::Colour alertCol,
                                LinearXFn&& xForDb)
    {
        if (isChromaKey() || ticks.empty() || rightX <= leftX + 8.0f)
            return;

        const juce::Colour inkCol = PinkXP::ink;
        const bool isPixel = isPixelHardware();

        // 1) Draw all major & minor tick marks
        for (const auto& t : ticks)
        {
            if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                continue;

            const float x = xForDb (t.db);
            const bool red = t.db >= warnDb - 0.01f;
            const auto col = red ? alertCol : inkCol;
            const float len = t.major ? tickLenMajor : tickLenMinor;
            g.setColour (col.withAlpha (t.major ? 0.88f : 0.46f));

            if (isPixel)
            {
                const int ix = juce::roundToInt (x);
                const int tw = t.major ? 2 : 1;
                const int iLen = juce::jmax (1, juce::roundToInt (len));
                if (dir == TickDirection::down || dir == TickDirection::both)
                    g.fillRect (ix - tw / 2, juce::roundToInt (tickAnchorBot), tw, iLen);
                if (dir == TickDirection::up || dir == TickDirection::both)
                    g.fillRect (ix - tw / 2, juce::roundToInt (tickAnchorTop) - iLen, tw, iLen);
            }
            else
            {
                const float strokeW = t.major ? 1.25f : 0.9f;
                if (dir == TickDirection::down || dir == TickDirection::both)
                    g.drawLine (x, tickAnchorBot, x, tickAnchorBot + len, strokeW);
                if (dir == TickDirection::up || dir == TickDirection::both)
                    g.drawLine (x, tickAnchorTop, x, tickAnchorTop - len, strokeW);
            }
        }

        if (! drawLabels || labelH < 8.0f)
            return;

        // 2) Priority-ordered anti-collision label placement
        struct Candidate
        {
            float x0 = 0.0f;
            float x1 = 0.0f;
            const char* text = nullptr;
            bool red = false;
            int priority = 2; // 0 = must-show, 1 = round 10s/12s, 2 = secondary
        };
        struct PlacedSpan { float x0 = 0.0f; float x1 = 0.0f; };

        std::vector<Candidate> cands;
        cands.reserve (ticks.size());

        for (const auto& t : ticks)
        {
            if (t.label == nullptr || t.label[0] == '\0')
                continue;
            if (t.db < minDb - 0.05f || t.db > maxDb + 0.05f)
                continue;

            int lenChars = 0;
            while (t.label[lenChars] != '\0')
                ++lenChars;
            const float boxW = juce::jmax (14.0f, (float) lenChars * 6.2f + 4.0f);
            const float cx = xForDb (t.db);
            float lx = cx - boxW * 0.5f;
            lx = juce::jlimit (clipLeft, juce::jmax (clipLeft, clipRight - boxW), lx);

            const bool isEdge = (std::abs (t.db - minDb) < 0.2f || std::abs (t.db - maxDb) < 0.2f);
            const bool isZeroOrWarn = (std::abs (t.db) < 0.2f || std::abs (t.db - warnDb) < 0.2f);
            const int dbInt = juce::roundToInt (t.db);
            const bool isRoundStep = (dbInt % 10 == 0 || dbInt % 12 == 0);

            int prio = 2;
            if (isZeroOrWarn || isEdge)
                prio = 0;
            else if (isRoundStep)
                prio = 1;

            if (sparsePreference && prio == 2)
                continue;

            cands.push_back ({ lx, lx + boxW, t.label, t.db >= warnDb - 0.01f, prio });
        }

        std::vector<PlacedSpan> placed;
        placed.reserve (cands.size());

        for (int pass = 0; pass <= 2; ++pass)
        {
            for (const auto& c : cands)
            {
                if (c.priority != pass)
                    continue;

                bool collides = false;
                for (const auto& p : placed)
                {
                    if (c.x0 < p.x1 + minLabelGap && c.x1 > p.x0 - minLabelGap)
                    {
                        collides = true;
                        break;
                    }
                }
                if (collides)
                    continue;

                placed.push_back ({ c.x0, c.x1 });
                const auto col = c.red ? alertCol : inkCol.withAlpha (0.82f);
                PinkXP::drawCachedText (g, c.text,
                                        juce::Rectangle<float> (c.x0, labelY, c.x1 - c.x0, labelH),
                                        juce::Justification::centred, 8.5f, juce::Font::bold,
                                        col, true);
            }
        }
    }

    // Header strip shared by Studio bar, LED blocks, LED ladder, and Mirrored:
    // Left: subtle "dBFS" / "VU dBFS" caption.
    // Right (strictly left of Signal LED): sleek digital dB readout pill when showReadout_ is ON.
    inline void drawBarHeader (juce::Graphics& g,
                               juce::Rectangle<int> dialArea,
                               juce::Rectangle<int> ledRect,
                               bool showLed,
                               bool showReadout,
                               bool stereo,
                               const char* singleTag,
                               float monoDb,
                               float dbL,
                               float dbR,
                               float minDb,
                               float warnDb,
                               juce::Colour alertCol,
                               bool darkCanvas,
                               const juce::String& formattedMono,
                               const juce::String& formattedL,
                               const juce::String& formattedR)
    {
        const auto& tokens    = PinkXP::getSkinTokens();
        const bool  chromaKey = tokens.isChromaKey;
        const bool  isGlass   = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const bool  isNeon    = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
        const bool  isCrt     = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
        const bool  crispBox  = (tokens.dialFinish == PinkXP::DialFinish::pixelHardware || isGlass || isNeon || isCrt || tokens.badgeRadius < 0.5f);

        const float leftEdge = (float) dialArea.getX() + 8.0f;
        const float rightEdge = showLed ? ((float) ledRect.getX() - 6.0f)
                                        : ((float) dialArea.getRight() - 8.0f);
        const float availW = rightEdge - leftEdge;
        if (availW < 24.0f)
            return;

        const float pillH = juce::jlimit (12.0f, 16.0f, (float) ledRect.getHeight() + 2.0f);
        const float centreY = (float) ledRect.getCentreY();
        const float pillY = juce::jmax ((float) dialArea.getY() + 3.0f, centreY - pillH * 0.5f);

        float captionMaxRight = rightEdge;

        if (showReadout)
        {
            const bool tight = availW < 155.0f;
            const float fontH = isCrt ? (tight ? 7.6f : 8.2f) : (tight ? 8.2f : 9.0f);
            const auto headerFont = PinkXP::getAxisFont (fontH, juce::Font::bold);
            g.setFont (headerFont);

            const auto bgCol = (isNeon && darkCanvas) ? juce::Colour (0xff0a0d16).withAlpha (0.94f) : pillBackground (darkCanvas);
            const auto readInk = chromaKey ? PinkXP::ink : PinkXP::readableInkOn (bgCol);
            const auto subtleBorder = isNeon
                                          ? PinkXP::sel.withAlpha (0.82f)
                                          : (isCrt
                                                 ? PinkXP::sel.withAlpha (0.78f)
                                                 : ((tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
                                                        ? PinkXP::skinHairlineOn (bgCol, darkCanvas ? 0.22f : 0.24f)
                                                        : PinkXP::ink.withAlpha (darkCanvas ? 0.24f : 0.28f)));

            if (stereo)
            {
                const auto sL = numericReadoutOnly (formattedL);
                const auto sR = numericReadoutOnly (formattedR);
                const float slotW = tight ? 44.0f : 52.0f;
                const float gapW = 5.0f;
                const float padX = 5.0f;
                const float pillW = juce::jmin (availW, padX * 2.0f + slotW * 2.0f + gapW);
                const float pillX = rightEdge - pillW;
                const juce::Rectangle<float> pillRect (pillX, pillY, pillW, pillH);

                const bool anyAlert = (dbL >= warnDb || dbR >= warnDb);
                if (! chromaKey)
                {
                    const auto bCol = anyAlert ? alertCol.withAlpha (0.78f) : subtleBorder;
                    if (crispBox)
                    {
                        const auto pillI = pillRect.toNearestInt();
                        PinkXP::Craft::drawBadgePill (g, pillI, bgCol, bCol);
                        if (isBrushedMetal() && pillI.getWidth() >= 6 && pillI.getHeight() >= 6)
                        {
                            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.22f : 0.58f));
                            g.fillRect (pillI.getX() + 1, pillI.getY() + 1, pillI.getWidth() - 2, 1);
                        }
                    }
                    else
                    {
                        g.setColour (bgCol);
                        g.fillRoundedRectangle (pillRect, 3.5f);
                        g.setColour (bCol);
                        g.drawRoundedRectangle (pillRect.reduced (0.5f), 3.5f, 1.0f);
                    }
                }

                const float halfW = (pillW - padX * 2.0f - gapW) * 0.5f;
                auto drawSlot = [&] (const char* ch, const juce::String& val, float dbVal, float sx)
                {
                    const bool hot = dbVal >= warnDb;
                    const bool act = isSignalActive (dbVal, minDb);
                    const auto tagCol = hot ? alertCol : readInk.withAlpha (0.60f);
                    const auto valCol = hot ? alertCol
                                      : (act ? readInk : readInk.withAlpha (0.50f));
                    g.setColour (tagCol);
                    g.drawText (ch, juce::Rectangle<float> (sx, pillY, 9.0f, pillH),
                                juce::Justification::centredLeft, false);
                    g.setColour (valCol);
                    g.drawText (val, juce::Rectangle<float> (sx + 9.0f, pillY,
                                                             juce::jmax (12.0f, halfW - 9.0f), pillH),
                                juce::Justification::centredRight, false);
                };

                drawSlot ("L", sL, dbL, pillX + padX);

                // Subtle center divider inside stereo pill
                if (! chromaKey)
                {
                    const float divX = pillX + padX + halfW + gapW * 0.5f;
                    g.setColour (readInk.withAlpha (0.24f));
                    if (crispBox)
                        g.fillRect (juce::roundToInt (divX), juce::roundToInt (pillY) + 3, 1, juce::jmax (2, juce::roundToInt (pillH) - 6));
                    else
                        g.fillRect (juce::Rectangle<float> (divX - 0.5f, pillY + 3.0f, 1.0f, pillH - 6.0f));
                }

                drawSlot ("R", sR, dbR, pillX + padX + halfW + gapW);

                captionMaxRight = pillX - 6.0f;
            }
            else
            {
                const auto num = numericReadoutOnly (formattedMono);
                const juce::String prefix = (singleTag != nullptr) ? (juce::String (singleTag) + " ") : juce::String{};
                const juce::String label = tight ? (prefix + num) : (prefix + num + " dB");
                const float prefW = juce::jmax (tight ? 44.0f : 62.0f,
                                                std::ceil (PinkXP::SymbolGlyphs::getTextWidth (label, headerFont)) + 12.0f);
                const float pillW = juce::jmin (availW, prefW);
                const float pillX = rightEdge - pillW;
                const juce::Rectangle<float> pillRect (pillX, pillY, pillW, pillH);

                const bool hot = monoDb >= warnDb;
                const bool act = isSignalActive (monoDb, minDb);

                if (! chromaKey)
                {
                    const auto bCol = hot ? alertCol.withAlpha (0.80f) : subtleBorder;
                    if (crispBox)
                    {
                        const auto pillI = pillRect.toNearestInt();
                        PinkXP::Craft::drawBadgePill (g, pillI, bgCol, bCol);
                        if (isBrushedMetal() && pillI.getWidth() >= 6 && pillI.getHeight() >= 6)
                        {
                            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.22f : 0.58f));
                            g.fillRect (pillI.getX() + 1, pillI.getY() + 1, pillI.getWidth() - 2, 1);
                        }
                    }
                    else
                    {
                        g.setColour (bgCol);
                        g.fillRoundedRectangle (pillRect, 3.5f);
                        g.setColour (bCol);
                        g.drawRoundedRectangle (pillRect.reduced (0.5f), 3.5f, 1.0f);
                    }
                }

                g.setColour (hot ? alertCol
                           : (act ? readInk : readInk.withAlpha (0.50f)));
                g.drawText (label, pillRect.reduced (4.0f, 0.0f),
                            juce::Justification::centred, false);

                captionMaxRight = pillX - 6.0f;
            }
        }

        // Left header caption (skipped on ChromaKey; only drawn if ample clearance from readout pill & LED)
        const float capW = captionMaxRight - leftEdge;
        if (! chromaKey && capW >= 28.0f)
        {
            juce::String capText;
            if (singleTag != nullptr)
                capText = (capW >= 62.0f) ? (juce::String ("VU ") + singleTag + " dBFS")
                                          : (juce::String (singleTag) + " dBFS");
            else
                capText = (capW >= 62.0f) ? "VU dBFS" : "dBFS";
            PinkXP::drawCachedText (g, capText,
                                    juce::Rectangle<float> (leftEdge, pillY, capW, pillH),
                                    juce::Justification::centredLeft, 8.5f, juce::Font::bold,
                                    PinkXP::ink.withAlpha (0.62f), true);
        }
    }
} // namespace vuBarDetail


// ============================================================================
// 1. Studio bar —— Sleek broadcast / mastering horizontal level meter
//    Mono: bold single recessed track + bottom dB ruler.
//    Stereo: twin stacked L/R precision tracks flanking a shared central ruler
//            (or compact bottom ruler at tiny heights).
//    Respects barFinish_ (smooth / notched / glow), colorMap_, showReadout_,
//    and strictly obeys peakHold.
// ============================================================================
void VuMeterModule::drawBarStudioDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody (dialArea);
    if (body.getWidth() < 52.0f || body.getHeight() < 16.0f)
        return;

    const bool darkCanvas  = vuBarDetail::isDarkCanvas();
    const bool stereo      = isStereoMode();
    const char* singleTag  = activeChannelTag();
    const float minDb      = minDisplayDb();
    const float maxDb      = maxDisplayDb();
    const float cautDb     = cautionDbfs();
    const float singleDb   = activeSingleDbfs();
    const float singlePk   = activeSinglePeakDbfs();
    const auto  alertCol   = vuAlertColour();

    vuBarDetail::drawBarHeader (g, dialArea, ledBounds (dialArea), showLed, showReadout_,
                                stereo, singleTag, singleDb, displayedL, displayedR,
                                minDb, warnDbfs, alertCol, darkCanvas,
                                formatDbReadout (singleDb),
                                formatDbReadout (displayedL),
                                formatDbReadout (displayedR));

    const float padL = (stereo || singleTag != nullptr) ? 16.0f : 6.0f;
    const float padR = 6.0f;
    dialGeom.leftX  = body.getX() + padL;
    dialGeom.rightX = body.getRight() - padR;
    const float scaleW = dialGeom.rightX - dialGeom.leftX;
    if (scaleW < 24.0f)
        return;

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    auto paletteAt = [this] (float t, float db, bool dark) noexcept
    {
        return vuPaletteColour (t, db, dark);
    };
    auto xForDb = [this] (float db) noexcept
    {
        return linearXFor (db);
    };

    const float warnX    = linearXFor (warnDbfs);
    const float H        = body.getHeight();
    const bool  isPixel  = vuBarDetail::isPixelHardware();
    const bool  chromaKey = vuBarDetail::isChromaKey();

    auto drawWarnRule = [&] (float y0, float y1)
    {
        if (chromaKey)
            return;
        if (isPixel)
        {
            g.setColour (alertCol.withAlpha (0.82f));
            g.fillRect (juce::roundToInt (warnX), juce::roundToInt (y0), 1, juce::jmax (1, juce::roundToInt (y1 - y0)));
        }
        else
        {
            g.setColour (alertCol.withAlpha (0.74f));
            g.drawLine (warnX, y0, warnX, y1, 1.2f);
        }
    };

    auto renderTrack = [&] (juce::Rectangle<float> trackRect, float levelDb,
                            float peakDb, float maxHaloPx)
    {
        vuBarDetail::drawRecessedTrough (g, trackRect, 2.5f, darkCanvas);
        const auto inner = trackRect.reduced (isPixel ? 2.0f : 1.5f);
        if (inner.getWidth() < 4.0f || inner.getHeight() < 2.0f)
            return;

        vuBarDetail::drawReferenceZoneTint (g, inner, dialGeom.leftX, dialGeom.rightX,
                                            minDb, maxDb, cautDb, warnDbfs, warnX,
                                            darkCanvas, paletteAt);

        // Subtle vertical reference hairlines at major dB ticks inside trough
        if (! chromaKey)
        {
            if (isPixel)
            {
                for (const auto& t : ticks)
                {
                    if (! t.major) continue;
                    const float tx = linearXFor (t.db);
                    if (tx > inner.getX() + 2.0f && tx < inner.getRight() - 2.0f)
                        PinkXP::Craft::drawGridLine (g, tx, inner.getY(), tx, inner.getBottom() - 1.0f,
                                                     false, PinkXP::ink.withAlpha (0.85f));
                }
            }
            else
            {
                g.setColour (PinkXP::ink.withAlpha (darkCanvas ? 0.09f : 0.11f));
                for (const auto& t : ticks)
                {
                    if (! t.major) continue;
                    const float tx = linearXFor (t.db);
                    if (tx > inner.getX() + 2.0f && tx < inner.getRight() - 2.0f)
                        g.fillRect (juce::Rectangle<float> (tx - 0.5f, inner.getY(), 1.0f, inner.getHeight()));
                }
            }
        }

        // Live horizontal fill respecting barFinish_
        vuBarDetail::drawStyledBarFill (g, inner, dialGeom.leftX, dialGeom.rightX,
                                        levelDb, minDb, maxDb, cautDb, warnDbfs,
                                        barFinish_, darkCanvas, false, maxHaloPx,
                                        paletteAt, xForDb);

        // Peak-hold marker ONLY when peakHold == true and peak > minDisplayDb()
        if (vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb))
        {
            const float px = juce::jlimit (inner.getX() + 1.0f, inner.getRight() - 1.0f, linearXFor (peakDb));
            vuBarDetail::drawPeakHoldCap (g, px, inner.getY(), inner.getBottom(),
                                          dialTFor (peakDb), peakDb, warnDbfs, alertCol,
                                          darkCanvas, paletteAt);
        }
    };

    float topY = body.getY();
    float botY = body.getBottom();

    if (stereo)
    {
        const bool sharedCentreRuler = H >= 56.0f;
        const bool bottomRuler       = ! sharedCentreRuler && H >= 38.0f;

        if (sharedCentreRuler)
        {
            const float tickLen = juce::jlimit (3.0f, 5.0f, H * 0.055f);
            const float labelH  = 11.0f;
            const float rulerH  = labelH + tickLen * 2.0f + 4.0f;
            const float trackH  = juce::jlimit (10.0f, 28.0f, (H - rulerH - 6.0f) * 0.44f);
            const float totalH  = trackH * 2.0f + rulerH;
            const float startY  = body.getY() + juce::jmax (0.0f, (H - totalH) * 0.5f);

            const juce::Rectangle<float> rectL (dialGeom.leftX, startY, scaleW, trackH);
            const float rulerTop = rectL.getBottom() + 2.0f;
            const float labelY   = rulerTop + tickLen;
            const float rulerBot = labelY + labelH + tickLen;
            const juce::Rectangle<float> rectR (dialGeom.leftX, rulerBot + 2.0f, scaleW, trackH);

            renderTrack (rectL, displayedL, peakHoldL, 3.5f);
            renderTrack (rectR, displayedR, peakHoldR, 3.5f);

            // Shared central dB ruler with symmetrical ticks pointing to L & R tracks
            vuBarDetail::drawScaleRuler (g, ticks, dialGeom.leftX, dialGeom.rightX,
                                         body.getX(), body.getRight(),
                                         labelY, labelY + labelH,
                                         labelY, labelH,
                                         vuBarDetail::TickDirection::both,
                                         tickLen, tickLen * 0.55f,
                                         true, 4.0f, false,
                                         minDb, maxDb, warnDbfs, alertCol, xForDb);

            vuBarDetail::drawChannelTag (g, "L", body.getX(), rectL.getCentreY(),
                                         vuPaletteColour (dialTFor (displayedL), displayedL, darkCanvas),
                                         vuBarDetail::isSignalActive (displayedL, minDb), darkCanvas);
            vuBarDetail::drawChannelTag (g, "R", body.getX(), rectR.getCentreY(),
                                         vuPaletteColour (dialTFor (displayedR), displayedR, darkCanvas),
                                         vuBarDetail::isSignalActive (displayedR, minDb), darkCanvas);

            // Crisp warnDbfs threshold hairline spanning both tracks
            drawWarnRule (rectL.getY() - 2.0f, rectR.getBottom() + 2.0f);

            topY = rectL.getY();
            botY = rectR.getBottom();
        }
        else
        {
            const float rulerH = bottomRuler ? 16.0f : 0.0f;
            const float gap    = 3.0f;
            const float trackH = juce::jlimit (6.0f, 18.0f, (H - rulerH - gap - 2.0f) * 0.5f);
            const float totalH = trackH * 2.0f + gap + rulerH;
            const float startY = body.getY() + juce::jmax (0.0f, (H - totalH) * 0.5f);

            const juce::Rectangle<float> rectL (dialGeom.leftX, startY, scaleW, trackH);
            const juce::Rectangle<float> rectR (dialGeom.leftX, rectL.getBottom() + gap, scaleW, trackH);

            renderTrack (rectL, displayedL, peakHoldL, 2.0f);
            renderTrack (rectR, displayedR, peakHoldR, 2.0f);

            vuBarDetail::drawChannelTag (g, "L", body.getX(), rectL.getCentreY(),
                                         vuPaletteColour (dialTFor (displayedL), displayedL, darkCanvas),
                                         vuBarDetail::isSignalActive (displayedL, minDb), darkCanvas);
            vuBarDetail::drawChannelTag (g, "R", body.getX(), rectR.getCentreY(),
                                         vuPaletteColour (dialTFor (displayedR), displayedR, darkCanvas),
                                         vuBarDetail::isSignalActive (displayedR, minDb), darkCanvas);

            if (bottomRuler)
            {
                vuBarDetail::drawScaleRuler (g, ticks, dialGeom.leftX, dialGeom.rightX,
                                             body.getX(), body.getRight(),
                                             rectR.getBottom() + 1.0f, rectR.getBottom() + 1.0f,
                                             rectR.getBottom() + 5.0f, 11.0f,
                                             vuBarDetail::TickDirection::down,
                                             3.5f, 2.0f, true, 4.0f, false,
                                             minDb, maxDb, warnDbfs, alertCol, xForDb);
            }

            drawWarnRule (rectL.getY() - 1.5f, rectR.getBottom() + 1.5f);

            topY = rectL.getY();
            botY = rectR.getBottom();
        }
    }
    else
    {
        const bool withRuler = H >= 34.0f;
        const float tickLen  = withRuler ? 4.5f : 0.0f;
        const float rulerH   = withRuler ? (tickLen + 13.0f) : 0.0f;
        const float trackH   = juce::jlimit (10.0f, 36.0f, (H - rulerH - 6.0f) * 0.62f);
        const float totalH   = trackH + rulerH;
        const float startY   = body.getY() + juce::jmax (0.0f, (H - totalH) * 0.5f);

        const juce::Rectangle<float> trackRect (dialGeom.leftX, startY, scaleW, trackH);
        renderTrack (trackRect, singleDb, singlePk, 4.0f);

        if (singleTag != nullptr)
        {
            vuBarDetail::drawChannelTag (g, singleTag, body.getX(), trackRect.getCentreY(),
                                         vuPaletteColour (dialTFor (singleDb), singleDb, darkCanvas),
                                         vuBarDetail::isSignalActive (singleDb, minDb), darkCanvas);
        }

        if (withRuler)
        {
            vuBarDetail::drawScaleRuler (g, ticks, dialGeom.leftX, dialGeom.rightX,
                                         body.getX(), body.getRight(),
                                         trackRect.getBottom() + 2.0f, trackRect.getBottom() + 2.0f,
                                         trackRect.getBottom() + 2.0f + tickLen + 1.0f, 11.0f,
                                         vuBarDetail::TickDirection::down,
                                         tickLen, tickLen * 0.55f,
                                         true, 4.0f, false,
                                         minDb, maxDb, warnDbfs, alertCol, xForDb);
        }

        // Crisp vertical warnDbfs threshold hairline
        drawWarnRule (trackRect.getY() - 2.0f,
                      trackRect.getBottom() + (withRuler ? (tickLen + 1.5f) : 2.0f));

        topY = trackRect.getY();
        botY = trackRect.getBottom();
    }

    dialGeom.trackTop    = topY;
    dialGeom.trackBottom = botY;
    dialGeom.bandTop     = topY;
    dialGeom.bandBottom  = botY;
    dialGeom.radius      = juce::jmax (4.0f, botY - topY);
    dialGeom.valid       = true;
}


// ============================================================================
// 2. LED blocks —— Hardware segmented horizontal LED block bargraph
//    Density controlled by barDensity_ (chunky ~22, medium ~38, fine ~60).
//    Unlit cells render as recessed dimmed sockets; lit cells glow vividly
//    with inner acrylic highlight; peak-hold brick drawn ONLY when peakHold==true.
// ============================================================================
void VuMeterModule::drawBarLedBlocksDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody (dialArea);
    if (body.getWidth() < 52.0f || body.getHeight() < 16.0f)
        return;

    const bool darkCanvas = vuBarDetail::isDarkCanvas();
    const bool stereo     = isStereoMode();
    const char* singleTag = activeChannelTag();
    const float minDb     = minDisplayDb();
    const float maxDb     = maxDisplayDb();
    const float singleDb  = activeSingleDbfs();
    const float singlePk  = activeSinglePeakDbfs();
    const auto  alertCol  = vuAlertColour();

    vuBarDetail::drawBarHeader (g, dialArea, ledBounds (dialArea), showLed, showReadout_,
                                stereo, singleTag, singleDb, displayedL, displayedR,
                                minDb, warnDbfs, alertCol, darkCanvas,
                                formatDbReadout (singleDb),
                                formatDbReadout (displayedL),
                                formatDbReadout (displayedR));

    const float padL = (stereo || singleTag != nullptr) ? 16.0f : 6.0f;
    const float padR = 6.0f;
    dialGeom.leftX  = body.getX() + padL;
    dialGeom.rightX = body.getRight() - padR;
    const float scaleW = dialGeom.rightX - dialGeom.leftX;
    if (scaleW < 24.0f)
        return;

    // Cell count from barDensity_ with small-width legibility protection
    int targetCells = 38;
    switch (barDensity_)
    {
        case BarDensity::chunky: targetCells = 22; break;
        case BarDensity::fine:   targetCells = 58; break;
        case BarDensity::medium:
        default:                 targetCells = 38; break;
    }
    const int maxReadable = juce::jmax (12, (int) std::floor (scaleW / 2.2f));
    const int nCells      = juce::jmin (targetCells, maxReadable);
    const float pitch     = scaleW / (float) nCells;
    const float gap       = (pitch >= 5.0f) ? juce::jlimit (1.4f, 2.6f, pitch * 0.22f)
                          : (pitch >= 2.8f) ? juce::jlimit (0.8f, 1.4f, pitch * 0.24f)
                                            : 0.5f;
    const float cellW     = juce::jmax (1.2f, pitch - gap);
    const float cellRad   = (cellW >= 4.5f) ? 1.8f : (cellW >= 2.8f ? 1.0f : 0.0f);

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    const bool isPixel        = vuBarDetail::isPixelHardware();
    const bool isBrushedMetal = vuBarDetail::isBrushedMetal();
    const bool isGlass        = vuBarDetail::isGlassDome();
    const bool isNeon         = vuBarDetail::isNeonRings();
    const bool isCrt          = vuBarDetail::isCrtVector();
    const bool chromaKey      = vuBarDetail::isChromaKey();
    const bool greenScreen    = vuBarDetail::isGreenScreen();
    const float H = body.getHeight();
    const bool withRuler = H >= 36.0f;
    const float tickLen  = withRuler ? 4.0f : 0.0f;
    const float rulerH   = withRuler ? (tickLen + 13.0f) : 0.0f;
    const float availH   = juce::jmax (10.0f, H - rulerH - 4.0f);

    auto drawLedRow = [&] (juce::Rectangle<float> bayRect, float levelDb, float peakDb)
    {
        // Recessed hardware LED array socket bay
        vuBarDetail::drawRecessedTrough (g, bayRect, 2.5f, darkCanvas);
        const float cellTop = bayRect.getY() + (isPixel ? 3.0f : 2.5f);
        const float cellH   = juce::jmax (3.0f, bayRect.getHeight() - (isPixel ? 6.0f : 5.0f));

        const int topLit = vuBarDetail::isSignalActive (levelDb, minDb)
                         ? juce::jlimit (0, nCells - 1,
                                         (int) std::floor (dialTFor (levelDb) * (float) nCells - 1.0e-4f))
                         : -1;

        const int peakCell = vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb)
                           ? juce::jlimit (0, nCells - 1,
                                           (int) std::floor (dialTFor (peakDb) * (float) nCells - 1.0e-4f))
                           : -1;

        const juce::Colour socketBase = darkCanvas
                                      ? juce::Colour (0xff0a0b10)
                                      : PinkXP::content.interpolatedWith (PinkXP::ink, 0.075f);

        if (isPixel)
        {
            // Iconic Winamp / Win95 crisp rectangular pixel LED bricks (0px radius, 1px hard dark gap)
            const int iyTop = juce::roundToInt (cellTop);
            const int ih    = juce::jmax (3, juce::roundToInt (cellH));
            const int gapPx = (pitch >= 2.5f) ? 1 : 0;

            for (int i = 0; i < nCells; ++i)
            {
                const float t   = ((float) i + 0.5f) / (float) nCells;
                const float db  = minDb + t * (maxDb - minDb);
                const auto  col = vuPaletteColour (t, db, darkCanvas);
                const int   x0  = juce::roundToInt (dialGeom.leftX + (float) i * pitch);
                const int   x1  = juce::roundToInt (dialGeom.leftX + (float) (i + 1) * pitch) - gapPx;
                const int   cw  = juce::jmax (1, x1 - x0);
                const juce::Rectangle<int> crI (x0, iyTop, cw, ih);

                const bool isLit  = (i <= topLit);
                const bool isPeak = (peakCell >= 0 && i == peakCell);
                const bool isRed  = (db >= warnDbfs);

                if (isLit)
                {
                    PinkXP::Craft::drawMeterCell (g, crI, col, true, (i == topLit) ? 0.85f : 0.0f);
                    if (isPeak)
                    {
                        g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.85f)
                                                : col.darker (0.45f));
                        g.drawRect (crI, 1);
                    }
                }
                else if (isPeak)
                {
                    const auto pkFill = darkCanvas ? col.interpolatedWith (juce::Colours::white, 0.30f) : col;
                    PinkXP::Craft::drawMeterCell (g, crI, pkFill, true, 1.0f);
                    g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.85f)
                                            : col.darker (0.42f));
                    g.drawRect (crI, 1);
                }
                else if (! chromaKey)
                {
                    // Dark flat unlit pixel LED socket
                    g.setColour (socketBase.interpolatedWith (col, isRed ? 0.16f : 0.09f));
                    g.fillRect (crI);
                }
            }
            return;
        }

        if (isBrushedMetal)
        {
            // Millennium Chrome: CNC precision 0px beveled jewel-metallic LED cells (Craft::drawMeterCell metallicBevel)
            for (int i = 0; i < nCells; ++i)
            {
                const float t   = ((float) i + 0.5f) / (float) nCells;
                const float db  = minDb + t * (maxDb - minDb);
                const auto  col = vuPaletteColour (t, db, darkCanvas);
                const float cx  = dialGeom.leftX + (float) i * pitch + gap * 0.5f;
                const juce::Rectangle<float> cr (cx, cellTop, cellW, cellH);

                const bool isLit  = (i <= topLit);
                const bool isPeak = (peakCell >= 0 && i == peakCell);
                const bool isRed  = (db >= warnDbfs);

                if (isLit)
                {
                    const bool leading = (i == topLit);
                    PinkXP::Craft::drawMeterCell (g, cr, col, true, leading ? 0.85f : 0.0f);
                    if (cr.getHeight() >= 4.0f)
                    {
                        g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.42f : 0.52f));
                        g.fillRect (juce::Rectangle<float> (cr.getX(), cr.getY(), cr.getWidth(), 1.0f));
                    }
                    if (isPeak)
                    {
                        g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.86f)
                                                : PinkXP::dark.withAlpha (0.72f));
                        g.drawRect (cr, 1.0f);
                    }
                }
                else if (isPeak)
                {
                    if (! greenScreen)
                    {
                        g.setColour (col.withAlpha (darkCanvas ? 0.34f : 0.22f));
                        g.fillRect (cr.expanded (1.0f));
                    }
                    const auto pkFill = darkCanvas ? col.interpolatedWith (juce::Colours::white, 0.26f) : col;
                    PinkXP::Craft::drawMeterCell (g, cr, pkFill, true, 1.0f);
                    g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.86f)
                                            : PinkXP::dark.withAlpha (0.68f));
                    g.drawRect (cr, 1.0f);
                }
                else if (! chromaKey)
                {
                    // Recessed dark milled LED socket with faint etched chrome bottom reflection
                    g.setColour (socketBase.interpolatedWith (col, isRed ? 0.16f : 0.09f));
                    g.fillRect (cr);
                    if (cellW >= 2.2f)
                    {
                        g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.60f : 0.32f));
                        g.drawRect (cr, 0.75f);
                        g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.10f : 0.38f));
                        g.fillRect (juce::Rectangle<float> (cr.getX(), cr.getBottom() - 0.8f, cr.getWidth(), 0.8f));
                    }
                }
            }
            return;
        }

        if (isGlass || isNeon || isCrt)
        {
            // Frosted Glass (glossyCapsule), Neon Cyber (neonLit), CRT Terminal (phosphorBar) via Craft::drawMeterCell
            const bool cellDark = darkCanvas;
            const juce::Colour sockCol = (isNeon && darkCanvas) ? juce::Colour (0xff0c0f18) : socketBase;
            const float sockRad = isGlass ? juce::jmin (3.0f, juce::jmin (cellW, cellH) * 0.45f) : 0.0f;

            for (int i = 0; i < nCells; ++i)
            {
                const float t   = ((float) i + 0.5f) / (float) nCells;
                const float db  = minDb + t * (maxDb - minDb);
                const auto  col = vuPaletteColour (t, db, cellDark);
                const float cx  = dialGeom.leftX + (float) i * pitch + gap * 0.5f;
                const juce::Rectangle<float> cr (cx, cellTop, cellW, cellH);

                const bool isLit  = (i <= topLit);
                const bool isPeak = (peakCell >= 0 && i == peakCell);
                const bool isRed  = (db >= warnDbfs);

                if (isLit)
                {
                    const bool leading = (i == topLit);
                    PinkXP::Craft::drawMeterCell (g, cr, col, true, leading ? 0.85f : 0.0f);
                    if (isPeak)
                    {
                        g.setColour (cellDark ? juce::Colours::white.withAlpha (0.86f)
                                              : col.darker (0.38f));
                        if (sockRad > 0.5f) g.drawRoundedRectangle (cr.reduced (0.35f), sockRad, 1.0f);
                        else                g.drawRect (cr, 1.0f);
                    }
                }
                else if (isPeak)
                {
                    if (! greenScreen)
                    {
                        g.setColour (col.withAlpha (isNeon ? 0.42f : (cellDark ? 0.34f : 0.24f)));
                        if (sockRad > 0.5f) g.fillRoundedRectangle (cr.expanded (1.2f), sockRad + 1.0f);
                        else                g.fillRect (cr.expanded (1.0f));
                    }
                    const auto pkFill = cellDark ? col.interpolatedWith (juce::Colours::white, 0.28f) : col;
                    PinkXP::Craft::drawMeterCell (g, cr, pkFill, true, 1.0f);
                    g.setColour (cellDark ? juce::Colours::white.withAlpha (0.88f) : col.darker (0.35f));
                    if (sockRad > 0.5f) g.drawRoundedRectangle (cr.reduced (0.35f), sockRad, 1.0f);
                    else                g.drawRect (cr, 1.0f);
                }
                else if (! chromaKey)
                {
                    g.setColour (sockCol.interpolatedWith (col, isRed ? 0.16f : 0.10f));
                    if (sockRad > 0.5f) g.fillRoundedRectangle (cr, sockRad);
                    else                g.fillRect (cr);
                    if (cellW >= 2.2f)
                    {
                        const auto rimC = isGlass
                                              ? juce::Colours::white.withAlpha (darkCanvas ? 0.16f : 0.45f)
                                              : col.withAlpha (isRed ? 0.28f : (isNeon ? 0.22f : 0.18f));
                        g.setColour (rimC);
                        if (sockRad > 0.5f) g.drawRoundedRectangle (cr.reduced (0.35f), sockRad, 0.75f);
                        else                g.drawRect (cr, 0.75f);
                    }
                }
            }
            return;
        }

        for (int i = 0; i < nCells; ++i)
        {
            const float t   = ((float) i + 0.5f) / (float) nCells;
            const float db  = minDb + t * (maxDb - minDb);
            const auto  col = vuPaletteColour (t, db, darkCanvas);
            const float cx  = dialGeom.leftX + (float) i * pitch + gap * 0.5f;
            const juce::Rectangle<float> cr (cx, cellTop, cellW, cellH);

            const bool isLit  = (i <= topLit);
            const bool isPeak = (peakCell >= 0 && i == peakCell);
            const bool isRed  = (db >= warnDbfs);

            if (isLit)
            {
                // Soft chromatic glow behind lit LED on dark canvas (skipped ONLY on greenScreen)
                if (darkCanvas && ! greenScreen)
                {
                    g.setColour (col.withAlpha (0.22f));
                    g.fillRoundedRectangle (cr.expanded (0.9f, 1.0f), cellRad + 0.8f);
                }

                const bool leading = (i == topLit);
                const auto fillCol = leading
                                   ? col.interpolatedWith (juce::Colours::white, darkCanvas ? 0.22f : 0.14f)
                                   : col;
                g.setColour (fillCol);
                if (cellRad > 0.5f)
                    g.fillRoundedRectangle (cr, cellRad);
                else
                    g.fillRect (cr);

                // Inner acrylic glass specular highlight on upper third
                if (cellH >= 5.0f)
                {
                    const float hiH = juce::jmax (1.2f, cellH * 0.30f);
                    const float inset = (cellW >= 4.0f) ? 0.8f : 0.0f;
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.30f : 0.34f));
                    g.fillRect (juce::Rectangle<float> (cr.getX() + inset, cr.getY() + 0.6f,
                                                        juce::jmax (0.8f, cr.getWidth() - inset * 2.0f), hiH));
                }

                // Subtle bottom bevel shade
                if (cellH >= 6.0f)
                {
                    g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.24f : 0.14f));
                    g.fillRect (juce::Rectangle<float> (cr.getX(), cr.getBottom() - 1.0f, cr.getWidth(), 1.0f));
                }

                // Highlight rim if peak-hold rests on the leading lit brick
                if (isPeak)
                {
                    g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.72f)
                                            : col.darker (0.35f));
                    if (cellRad > 0.5f)
                        g.drawRoundedRectangle (cr.reduced (0.4f), cellRad, 1.0f);
                    else
                        g.drawRect (cr, 1.0f);
                }
            }
            else if (isPeak)
            {
                // Floating glowing peak-hold LED brick (ONLY when peakHold == true!)
                if (! greenScreen)
                {
                    g.setColour (col.withAlpha (darkCanvas ? 0.38f : 0.26f));
                    g.fillRoundedRectangle (cr.expanded (1.2f, 1.2f), cellRad + 1.0f);
                }

                const auto pkFill = darkCanvas
                                  ? col.interpolatedWith (juce::Colours::white, 0.28f)
                                  : col;
                g.setColour (pkFill);
                if (cellRad > 0.5f)
                    g.fillRoundedRectangle (cr, cellRad);
                else
                    g.fillRect (cr);

                g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.75f)
                                        : col.darker (0.32f));
                if (cellRad > 0.5f)
                    g.drawRoundedRectangle (cr.reduced (0.35f), cellRad, 1.0f);
                else
                    g.drawRect (cr, 1.0f);
            }
            else if (! chromaKey)
            {
                // Unlit recessed dimmed socket
                const float tintAmt = isRed ? (darkCanvas ? 0.16f : 0.15f)
                                            : (darkCanvas ? 0.11f : 0.11f);
                g.setColour (socketBase.interpolatedWith (col, tintAmt));
                if (cellRad > 0.5f)
                    g.fillRoundedRectangle (cr, cellRad);
                else
                    g.fillRect (cr);

                if (cellW >= 2.2f)
                {
                    g.setColour (col.withAlpha (isRed ? (darkCanvas ? 0.26f : 0.32f)
                                                      : (darkCanvas ? 0.16f : 0.22f)));
                    if (cellRad > 0.5f)
                        g.drawRoundedRectangle (cr.reduced (0.35f), cellRad, 0.75f);
                    else
                        g.drawRect (cr, 0.75f);
                }
            }
        }
    };

    float topY = body.getY();
    float botY = body.getBottom();

    if (stereo)
    {
        const float rowGap = juce::jlimit (3.0f, 7.0f, availH * 0.10f);
        const float rowH   = juce::jlimit (9.0f, 24.0f, (availH - rowGap) * 0.45f);
        const float totalH = rowH * 2.0f + rowGap + rulerH;
        const float startY = body.getY() + juce::jmax (0.0f, (H - totalH) * 0.5f);

        const juce::Rectangle<float> bayL (dialGeom.leftX - 2.5f, startY, scaleW + 5.0f, rowH);
        const juce::Rectangle<float> bayR (dialGeom.leftX - 2.5f, bayL.getBottom() + rowGap, scaleW + 5.0f, rowH);

        drawLedRow (bayL, displayedL, peakHoldL);
        drawLedRow (bayR, displayedR, peakHoldR);

        vuBarDetail::drawChannelTag (g, "L", body.getX(), bayL.getCentreY(),
                                     vuPaletteColour (dialTFor (displayedL), displayedL, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedL, minDb), darkCanvas);
        vuBarDetail::drawChannelTag (g, "R", body.getX(), bayR.getCentreY(),
                                     vuPaletteColour (dialTFor (displayedR), displayedR, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedR, minDb), darkCanvas);

        topY = bayL.getY();
        botY = bayR.getBottom();

        dialGeom.ladderRows = 2;
        dialGeom.rowTop[0]  = bayL.getY();     dialGeom.rowBot[0] = bayL.getBottom(); dialGeom.rowHang[0] = false;
        dialGeom.rowTop[1]  = bayR.getY();     dialGeom.rowBot[1] = bayR.getBottom(); dialGeom.rowHang[1] = false;
    }
    else
    {
        const float rowH   = juce::jlimit (12.0f, 34.0f, availH * 0.58f);
        const float totalH = rowH + rulerH;
        const float startY = body.getY() + juce::jmax (0.0f, (H - totalH) * 0.5f);

        const juce::Rectangle<float> bay (dialGeom.leftX - 2.5f, startY, scaleW + 5.0f, rowH);
        drawLedRow (bay, singleDb, singlePk);

        if (singleTag != nullptr)
        {
            vuBarDetail::drawChannelTag (g, singleTag, body.getX(), bay.getCentreY(),
                                         vuPaletteColour (dialTFor (singleDb), singleDb, darkCanvas),
                                         vuBarDetail::isSignalActive (singleDb, minDb), darkCanvas);
        }

        topY = bay.getY();
        botY = bay.getBottom();

        dialGeom.ladderRows = 1;
        dialGeom.rowTop[0]  = bay.getY();      dialGeom.rowBot[0] = bay.getBottom(); dialGeom.rowHang[0] = false;
    }

    // Subtle warnDbfs vertical reference tick across bay edge
    if (! chromaKey)
    {
        const float warnX = linearXFor (warnDbfs);
        if (isPixel)
        {
            g.setColour (alertCol.withAlpha (0.82f));
            g.fillRect (juce::roundToInt (warnX), juce::roundToInt (topY - 2.0f),
                        1, juce::jmax (1, juce::roundToInt (botY + (withRuler ? (tickLen + 1.5f) : 2.0f) - (topY - 2.0f))));
        }
        else
        {
            g.setColour (alertCol.withAlpha (0.72f));
            g.drawLine (warnX, topY - 2.0f, warnX, botY + (withRuler ? (tickLen + 1.5f) : 2.0f), 1.15f);
        }
    }

    if (withRuler)
    {
        auto xForDb = [this] (float db) noexcept { return linearXFor (db); };
        vuBarDetail::drawScaleRuler (g, ticks, dialGeom.leftX, dialGeom.rightX,
                                     body.getX(), body.getRight(),
                                     botY + 2.0f, botY + 2.0f,
                                     botY + 2.0f + tickLen + 1.0f, 11.0f,
                                     vuBarDetail::TickDirection::down,
                                     tickLen, tickLen * 0.55f,
                                     true, 4.0f, false,
                                     minDb, maxDb, warnDbfs, alertCol, xForDb);
    }

    dialGeom.trackTop    = topY;
    dialGeom.trackBottom = botY;
    dialGeom.bandTop     = topY;
    dialGeom.bandBottom  = botY;
    dialGeom.radius      = juce::jmax (4.0f, botY - topY);
    dialGeom.valid       = true;
}


// ============================================================================
// 3. LED ladder —— Ascending stepped wedge LED columns
//    Mono: single rising ladder from bottom baseline.
//    Stereo: upper-L rising UP + lower-R hanging DOWN from central horizon line.
//    Step count controlled by barDensity_ (chunky ~22, medium ~36, fine ~54).
//    Peak-hold column highlight drawn ONLY when peakHold == true.
// ============================================================================
void VuMeterModule::drawBarLedLadderDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody (dialArea);
    if (body.getWidth() < 52.0f || body.getHeight() < 18.0f)
        return;

    const bool darkCanvas = vuBarDetail::isDarkCanvas();
    const bool stereo     = isStereoMode();
    const char* singleTag = activeChannelTag();
    const float minDb     = minDisplayDb();
    const float maxDb     = maxDisplayDb();
    const float singleDb  = activeSingleDbfs();
    const float singlePk  = activeSinglePeakDbfs();
    const auto  alertCol  = vuAlertColour();

    vuBarDetail::drawBarHeader (g, dialArea, ledBounds (dialArea), showLed, showReadout_,
                                stereo, singleTag, singleDb, displayedL, displayedR,
                                minDb, warnDbfs, alertCol, darkCanvas,
                                formatDbReadout (singleDb),
                                formatDbReadout (displayedL),
                                formatDbReadout (displayedR));

    const float padL = (stereo || singleTag != nullptr) ? 16.0f : 6.0f;
    const float padR = 6.0f;
    dialGeom.leftX  = body.getX() + padL;
    dialGeom.rightX = body.getRight() - padR;
    const float scaleW = dialGeom.rightX - dialGeom.leftX;
    if (scaleW < 24.0f)
        return;

    int targetCols = 36;
    switch (barDensity_)
    {
        case BarDensity::chunky: targetCols = 22; break;
        case BarDensity::fine:   targetCols = 54; break;
        case BarDensity::medium:
        default:                 targetCols = 36; break;
    }
    const int maxReadable = juce::jmax (12, (int) std::floor (scaleW / 2.2f));
    const int nCols       = juce::jmin (targetCols, maxReadable);
    const float pitch     = scaleW / (float) nCols;
    const float gap       = (pitch >= 4.5f) ? juce::jlimit (1.2f, 2.2f, pitch * 0.24f)
                          : (pitch >= 2.6f) ? juce::jlimit (0.8f, 1.3f, pitch * 0.26f)
                                            : 0.5f;
    const float colW      = juce::jmax (1.2f, pitch - gap);
    const float colRad    = (colW >= 4.0f) ? 1.4f : 0.0f;

    const float H = body.getHeight();
    const bool withRuler = H >= 36.0f;
    const float tickLen  = withRuler ? 4.0f : 0.0f;
    const float rulerH   = withRuler ? (tickLen + 13.0f) : 0.0f;

    const float colTop = body.getY() + 2.0f;
    const float colBot = body.getBottom() - rulerH - 2.0f;
    if (colBot - colTop < 10.0f)
        return;

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    const bool isPixel        = vuBarDetail::isPixelHardware();
    const bool isBrushedMetal = vuBarDetail::isBrushedMetal();
    const bool isGlass        = vuBarDetail::isGlassDome();
    const bool isNeon         = vuBarDetail::isNeonRings();
    const bool isCrt          = vuBarDetail::isCrtVector();
    const bool chromaKey      = vuBarDetail::isChromaKey();
    const bool greenScreen    = vuBarDetail::isGreenScreen();

    // Subtle warnDbfs vertical marker behind the ladder columns
    if (! chromaKey)
    {
        const float warnX = linearXFor (warnDbfs);
        if (isPixel)
        {
            g.setColour (alertCol.withAlpha (0.70f));
            g.fillRect (juce::roundToInt (warnX), juce::roundToInt (colTop),
                        1, juce::jmax (1, juce::roundToInt (colBot + (withRuler ? (tickLen + 1.0f) : 0.0f) - colTop)));
        }
        else
        {
            g.setColour (alertCol.withAlpha (0.55f));
            g.drawLine (warnX, colTop, warnX, colBot + (withRuler ? (tickLen + 1.0f) : 0.0f), 1.1f);
        }
    }

    auto drawLadderRow = [&] (float rowTop, float rowBot, float levelDb, float peakDb, bool hang)
    {
        const float hMax = rowBot - rowTop;
        if (hMax < 4.0f)
            return;
        const float hMin = juce::jmax (3.0f, hMax * 0.28f);

        const int topLit = vuBarDetail::isSignalActive (levelDb, minDb)
                         ? juce::jlimit (0, nCols - 1,
                                         (int) std::floor (dialTFor (levelDb) * (float) nCols - 1.0e-4f))
                         : -1;

        const int peakCol = vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb)
                          ? juce::jlimit (0, nCols - 1,
                                          (int) std::floor (dialTFor (peakDb) * (float) nCols - 1.0e-4f))
                          : -1;

        const juce::Colour dimBg = vuBarDetail::troughBackground (darkCanvas);

        if (isPixel)
        {
            // Crisp 8-bit ascending pixel staircase bricks (0px radius, integer height steps, hard 1px gaps)
            const int iRowTop = juce::roundToInt (rowTop);
            const int iRowBot = juce::roundToInt (rowBot);
            const int gapPx   = (pitch >= 2.5f) ? 1 : 0;

            for (int i = 0; i < nCols; ++i)
            {
                const float frac = (nCols > 1) ? ((float) i / (float) (nCols - 1)) : 1.0f;
                const float t    = ((float) i + 0.5f) / (float) nCols;
                const float db   = minDb + t * (maxDb - minDb);
                const auto  col  = vuPaletteColour (t, db, darkCanvas);

                const int x0 = juce::roundToInt (dialGeom.leftX + (float) i * pitch);
                const int x1 = juce::roundToInt (dialGeom.leftX + (float) (i + 1) * pitch) - gapPx;
                const int cw = juce::jmax (1, x1 - x0);
                const int ih = juce::jmax (2, juce::roundToInt (hMin + (hMax - hMin) * frac));
                const int iy = hang ? iRowTop : (iRowBot - ih);
                const juce::Rectangle<int> rI (x0, iy, cw, ih);

                const bool isLit  = (i <= topLit);
                const bool isPeak = (peakCol >= 0 && i == peakCol);
                const bool isRed  = (db >= warnDbfs);

                if (isLit)
                {
                    PinkXP::Craft::drawMeterCell (g, rI, col, true, (i == topLit) ? 0.85f : 0.0f);
                    // Crisp 1px bright crown step edge
                    const int capY = hang ? (rI.getBottom() - 1) : rI.getY();
                    g.setColour (col.interpolatedWith (juce::Colours::white, (i == topLit) ? 0.60f : 0.36f));
                    g.fillRect (rI.getX(), capY, rI.getWidth(), 1);
                }
                else if (isPeak)
                {
                    const auto pkCol = darkCanvas ? col.interpolatedWith (juce::Colours::white, 0.34f) : col;
                    PinkXP::Craft::drawMeterCell (g, rI, pkCol, true, 1.0f);
                    const int capY = hang ? (rI.getBottom() - 2) : rI.getY();
                    g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.88f) : col.darker (0.35f));
                    g.fillRect (rI.getX(), capY, rI.getWidth(), juce::jmin (2, rI.getHeight()));
                }
                else if (! chromaKey)
                {
                    g.setColour (dimBg.interpolatedWith (col, isRed ? 0.15f : 0.08f));
                    g.fillRect (rI);
                    if (cw >= 3)
                    {
                        g.setColour (col.withAlpha (isRed ? 0.30f : 0.18f));
                        g.drawRect (rI, 1);
                    }
                }
            }
            return;
        }

        if (isBrushedMetal)
        {
            // Millennium Chrome: CNC beveled ascending metallic wedge columns (0px precision steps)
            for (int i = 0; i < nCols; ++i)
            {
                const float frac = (nCols > 1) ? ((float) i / (float) (nCols - 1)) : 1.0f;
                const float t    = ((float) i + 0.5f) / (float) nCols;
                const float db   = minDb + t * (maxDb - minDb);
                const auto  col  = vuPaletteColour (t, db, darkCanvas);
                const float h    = hMin + (hMax - hMin) * frac;
                const float cx   = dialGeom.leftX + (float) i * pitch + gap * 0.5f;
                const float cy   = hang ? rowTop : (rowBot - h);
                const juce::Rectangle<float> r (cx, cy, colW, h);

                const bool isLit  = (i <= topLit);
                const bool isPeak = (peakCol >= 0 && i == peakCol);
                const bool isRed  = (db >= warnDbfs);

                if (isLit)
                {
                    const bool leading = (i == topLit);
                    PinkXP::Craft::drawMeterCell (g, r, col, true, leading ? 0.85f : 0.0f);
                    // Crisp bright metallic crown bevel edge
                    const float capH = juce::jmin (2.0f, h * 0.25f);
                    const float capY = hang ? (r.getBottom() - capH) : r.getY();
                    g.setColour (col.interpolatedWith (juce::Colours::white, leading ? 0.66f : 0.44f));
                    g.fillRect (juce::Rectangle<float> (r.getX(), capY, r.getWidth(), capH));
                }
                else if (isPeak)
                {
                    if (! greenScreen)
                    {
                        g.setColour (col.withAlpha (darkCanvas ? 0.32f : 0.20f));
                        g.fillRect (r.expanded (1.0f));
                    }
                    const auto pkFill = darkCanvas ? col.interpolatedWith (juce::Colours::white, 0.28f) : col;
                    PinkXP::Craft::drawMeterCell (g, r, pkFill, true, 1.0f);
                    const float capH = juce::jmin (2.2f, h * 0.28f);
                    const float capY = hang ? (r.getBottom() - capH) : r.getY();
                    g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.88f) : PinkXP::dark.withAlpha (0.72f));
                    g.fillRect (juce::Rectangle<float> (r.getX(), capY, r.getWidth(), capH));
                }
                else if (! chromaKey)
                {
                    g.setColour (dimBg.interpolatedWith (col, isRed ? 0.15f : 0.08f));
                    g.fillRect (r);
                    if (colW >= 2.2f)
                    {
                        g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.52f : 0.28f));
                        g.drawRect (r, 0.75f);
                    }
                }
            }
            return;
        }

        if (isGlass || isNeon || isCrt)
        {
            // Frosted Glass (glossyCapsule), Neon Cyber (neonLit), CRT Terminal (phosphorBar) ascending ladder columns
            const bool cellDark = darkCanvas;
            const juce::Colour sockBg = (isNeon && darkCanvas) ? juce::Colour (0xff0c0f18) : dimBg;
            const float cRad = isGlass ? juce::jmin (2.6f, colW * 0.45f) : 0.0f;

            for (int i = 0; i < nCols; ++i)
            {
                const float frac = (nCols > 1) ? ((float) i / (float) (nCols - 1)) : 1.0f;
                const float t    = ((float) i + 0.5f) / (float) nCols;
                const float db   = minDb + t * (maxDb - minDb);
                const auto  col  = vuPaletteColour (t, db, cellDark);
                const float h    = hMin + (hMax - hMin) * frac;
                const float cx   = dialGeom.leftX + (float) i * pitch + gap * 0.5f;
                const float cy   = hang ? rowTop : (rowBot - h);
                const juce::Rectangle<float> r (cx, cy, colW, h);

                const bool isLit  = (i <= topLit);
                const bool isPeak = (peakCol >= 0 && i == peakCol);
                const bool isRed  = (db >= warnDbfs);

                if (isLit)
                {
                    const bool leading = (i == topLit);
                    PinkXP::Craft::drawMeterCell (g, r, col, true, leading ? 0.85f : 0.0f);
                    const float capH = juce::jmin (2.0f, h * 0.25f);
                    const float capY = hang ? (r.getBottom() - capH) : r.getY();
                    g.setColour (col.interpolatedWith (juce::Colours::white, leading ? 0.65f : 0.40f));
                    g.fillRect (juce::Rectangle<float> (r.getX(), capY, r.getWidth(), capH));
                }
                else if (isPeak)
                {
                    if (! greenScreen)
                    {
                        g.setColour (col.withAlpha (isNeon ? 0.40f : (cellDark ? 0.32f : 0.22f)));
                        if (cRad > 0.5f) g.fillRoundedRectangle (r.expanded (1.0f), cRad + 1.0f);
                        else             g.fillRect (r.expanded (1.0f));
                    }
                    const auto pkFill = cellDark ? col.interpolatedWith (juce::Colours::white, 0.30f) : col;
                    PinkXP::Craft::drawMeterCell (g, r, pkFill, true, 1.0f);
                    const float capH = juce::jmin (2.4f, h * 0.28f);
                    const float capY = hang ? (r.getBottom() - capH) : r.getY();
                    g.setColour (cellDark ? juce::Colours::white.withAlpha (0.88f) : col.darker (0.32f));
                    g.fillRect (juce::Rectangle<float> (r.getX(), capY, r.getWidth(), capH));
                }
                else if (! chromaKey)
                {
                    g.setColour (sockBg.interpolatedWith (col, isRed ? 0.15f : 0.09f));
                    if (cRad > 0.5f) g.fillRoundedRectangle (r, cRad);
                    else             g.fillRect (r);
                    if (colW >= 2.2f)
                    {
                        const auto rimC = isGlass
                                              ? juce::Colours::white.withAlpha (darkCanvas ? 0.14f : 0.42f)
                                              : col.withAlpha (isRed ? 0.28f : 0.20f);
                        g.setColour (rimC);
                        if (cRad > 0.5f) g.drawRoundedRectangle (r.reduced (0.35f), cRad, 0.75f);
                        else             g.drawRect (r, 0.75f);
                    }
                }
            }
            return;
        }

        for (int i = 0; i < nCols; ++i)
        {
            const float frac = (nCols > 1) ? ((float) i / (float) (nCols - 1)) : 1.0f;
            const float t    = ((float) i + 0.5f) / (float) nCols;
            const float db   = minDb + t * (maxDb - minDb);
            const auto  col  = vuPaletteColour (t, db, darkCanvas);
            const float h    = hMin + (hMax - hMin) * frac;
            const float cx   = dialGeom.leftX + (float) i * pitch + gap * 0.5f;
            const float cy   = hang ? rowTop : (rowBot - h);
            const juce::Rectangle<float> r (cx, cy, colW, h);

            const bool isLit  = (i <= topLit);
            const bool isPeak = (peakCol >= 0 && i == peakCol);
            const bool isRed  = (db >= warnDbfs);

            if (isLit)
            {
                // Soft top/crown bloom on dark canvas (skipped ONLY on greenScreen)
                if (darkCanvas && ! greenScreen)
                {
                    const float glowY = hang ? (r.getBottom() - 3.5f) : (r.getY() - 2.0f);
                    g.setColour (col.withAlpha (0.28f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (r.getX() - 0.8f, glowY,
                                                                    r.getWidth() + 1.6f, 5.5f), 2.0f);
                }

                // Vertical depth gradient: vivid luminous crown tapering to deeper base
                const auto crownCol = (i == topLit)
                                    ? col.interpolatedWith (juce::Colours::white, darkCanvas ? 0.26f : 0.16f)
                                    : col;
                const auto baseCol  = col.darker (darkCanvas ? 0.30f : 0.14f);
                juce::ColourGradient vg (hang ? baseCol : crownCol, 0.0f, r.getY(),
                                         hang ? crownCol : baseCol, 0.0f, r.getBottom(), false);
                g.setGradientFill (vg);
                if (colRad > 0.5f)
                    g.fillRoundedRectangle (r, colRad);
                else
                    g.fillRect (r);

                // Crisp bright crown tip cap
                const float capH = juce::jmin (2.0f, h * 0.25f);
                const float capY = hang ? (r.getBottom() - capH) : r.getY();
                g.setColour (col.interpolatedWith (juce::Colours::white,
                                                   (i == topLit) ? 0.55f : 0.32f));
                g.fillRect (juce::Rectangle<float> (r.getX(), capY, r.getWidth(), capH));
            }
            else if (isPeak)
            {
                // Floating glowing peak-hold column (ONLY when peakHold == true!)
                if (! greenScreen)
                {
                    g.setColour (col.withAlpha (darkCanvas ? 0.34f : 0.22f));
                    g.fillRoundedRectangle (r.expanded (1.0f, 1.0f), colRad + 1.0f);
                }

                const auto pkTop = darkCanvas
                                 ? col.interpolatedWith (juce::Colours::white, 0.34f)
                                 : col;
                const auto pkBase = col.withAlpha (greenScreen ? 1.0f : (darkCanvas ? 0.55f : 0.65f));
                juce::ColourGradient pg (hang ? pkBase : pkTop, 0.0f, r.getY(),
                                         hang ? pkTop : pkBase, 0.0f, r.getBottom(), false);
                g.setGradientFill (pg);
                if (colRad > 0.5f)
                    g.fillRoundedRectangle (r, colRad);
                else
                    g.fillRect (r);

                // Crisp bright crown cap on floating peak column
                const float capH = juce::jmin (2.5f, h * 0.28f);
                const float capY = hang ? (r.getBottom() - capH) : r.getY();
                g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.85f)
                                        : col.darker (0.28f));
                g.fillRect (juce::Rectangle<float> (r.getX(), capY, r.getWidth(), capH));
            }
            else if (! chromaKey)
            {
                // Subtle dimmed architectural unlit ladder column
                g.setColour (dimBg.interpolatedWith (col, isRed ? 0.14f : 0.09f));
                if (colRad > 0.5f)
                    g.fillRoundedRectangle (r, colRad);
                else
                    g.fillRect (r);

                if (colW >= 2.2f)
                {
                    g.setColour (col.withAlpha (isRed ? (darkCanvas ? 0.28f : 0.34f)
                                                      : (darkCanvas ? 0.18f : 0.24f)));
                    if (colRad > 0.5f)
                        g.drawRoundedRectangle (r.reduced (0.35f), colRad, 0.75f);
                    else
                        g.drawRect (r, 0.75f);
                }
            }
        }
    };

    if (stereo)
    {
        const float midY = (colTop + colBot) * 0.5f;
        const float halfGap = 2.0f;

        drawLadderRow (colTop, midY - halfGap, displayedL, peakHoldL, false);
        drawLadderRow (midY + halfGap, colBot, displayedR, peakHoldR, true);

        // Crisp central horizon baseline shared by mirrored L (up) & R (down) ladders
        if (! chromaKey)
        {
            g.setColour (PinkXP::ink.withAlpha (darkCanvas ? 0.42f : 0.48f));
            if (isPixel)
                g.fillRect (juce::roundToInt (dialGeom.leftX), juce::roundToInt (midY),
                            juce::roundToInt (scaleW), 1);
            else
                g.fillRect (juce::Rectangle<float> (dialGeom.leftX, midY - 0.5f, scaleW, 1.0f));
        }

        const float halfH = juce::jmax (4.0f, (midY - halfGap) - colTop);
        const float tagOffset = juce::jlimit (4.5f, 9.0f, halfH * 0.52f);
        vuBarDetail::drawChannelTag (g, "L", body.getX(), midY - tagOffset,
                                     vuPaletteColour (dialTFor (displayedL), displayedL, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedL, minDb), darkCanvas);
        vuBarDetail::drawChannelTag (g, "R", body.getX(), midY + tagOffset,
                                     vuPaletteColour (dialTFor (displayedR), displayedR, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedR, minDb), darkCanvas);

        dialGeom.ladderRows = 2;
        dialGeom.rowTop[0]  = colTop;          dialGeom.rowBot[0] = midY - halfGap; dialGeom.rowHang[0] = false;
        dialGeom.rowTop[1]  = midY + halfGap;  dialGeom.rowBot[1] = colBot;         dialGeom.rowHang[1] = true;
    }
    else
    {
        drawLadderRow (colTop, colBot, singleDb, singlePk, false);

        if (singleTag != nullptr)
        {
            vuBarDetail::drawChannelTag (g, singleTag, body.getX(), (colTop + colBot) * 0.5f,
                                         vuPaletteColour (dialTFor (singleDb), singleDb, darkCanvas),
                                         vuBarDetail::isSignalActive (singleDb, minDb), darkCanvas);
        }

        // Crisp subtle ground baseline under Mono rising ladder
        if (! chromaKey)
        {
            g.setColour (PinkXP::ink.withAlpha (darkCanvas ? 0.28f : 0.34f));
            if (isPixel)
                g.fillRect (juce::roundToInt (dialGeom.leftX), juce::roundToInt (colBot),
                            juce::roundToInt (scaleW), 1);
            else
                g.fillRect (juce::Rectangle<float> (dialGeom.leftX, colBot + 0.5f, scaleW, 1.0f));
        }

        dialGeom.ladderRows = 1;
        dialGeom.rowTop[0]  = colTop;          dialGeom.rowBot[0] = colBot;         dialGeom.rowHang[0] = false;
    }

    if (withRuler)
    {
        auto xForDb = [this] (float db) noexcept { return linearXFor (db); };
        vuBarDetail::drawScaleRuler (g, ticks, dialGeom.leftX, dialGeom.rightX,
                                     body.getX(), body.getRight(),
                                     colBot + 2.0f, colBot + 2.0f,
                                     colBot + 2.0f + tickLen + 1.0f, 11.0f,
                                     vuBarDetail::TickDirection::down,
                                     tickLen, tickLen * 0.55f,
                                     true, 4.0f, false,
                                     minDb, maxDb, warnDbfs, alertCol, xForDb);
    }

    dialGeom.trackTop    = colTop;
    dialGeom.trackBottom = colBot;
    dialGeom.bandTop     = colTop;
    dialGeom.bandBottom  = colBot;
    dialGeom.radius      = juce::jmax (4.0f, colBot - colTop);
    dialGeom.valid       = true;
}


// ============================================================================
// 4. Mirrored —— Symmetrical dual horizontal bars flanking a central dB spine
//    Mono: upper & lower mirror of mono level.
//    Stereo: upper L & lower R with crisp L/R channel tags.
//    Respects barFinish_ (smooth / notched / glow), colorMap_, showReadout_,
//    and strictly obeys peakHold.
// ============================================================================
void VuMeterModule::drawBarMirrorDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody (dialArea);
    if (body.getWidth() < 52.0f || body.getHeight() < 22.0f)
        return;

    const bool darkCanvas  = vuBarDetail::isDarkCanvas();
    const bool stereo      = isStereoMode();
    const char* singleTag  = activeChannelTag();
    const float minDb      = minDisplayDb();
    const float maxDb      = maxDisplayDb();
    const float cautDb     = cautionDbfs();
    const float singleDb   = activeSingleDbfs();
    const float singlePk   = activeSinglePeakDbfs();
    const auto  alertCol   = vuAlertColour();

    vuBarDetail::drawBarHeader (g, dialArea, ledBounds (dialArea), showLed, showReadout_,
                                stereo, singleTag, singleDb, displayedL, displayedR,
                                minDb, warnDbfs, alertCol, darkCanvas,
                                formatDbReadout (singleDb),
                                formatDbReadout (displayedL),
                                formatDbReadout (displayedR));

    const float padL = (stereo || singleTag != nullptr) ? 16.0f : 6.0f;
    const float padR = 6.0f;
    dialGeom.leftX  = body.getX() + padL;
    dialGeom.rightX = body.getRight() - padR;
    const float scaleW = dialGeom.rightX - dialGeom.leftX;
    if (scaleW < 24.0f)
        return;

    const float H          = body.getHeight();
    const bool  withLabels = H >= 42.0f;
    const float spineH     = withLabels ? 12.0f : 3.0f;
    const float tickH      = juce::jlimit (2.5f, 6.0f, H * 0.075f);
    const float gap        = 2.5f;
    const float barH       = juce::jlimit (6.0f, 26.0f, (H - spineH - 2.0f * (tickH + gap)) * 0.45f);
    const float blockH     = spineH + 2.0f * (tickH + gap + barH);

    const float upTop    = body.getY() + juce::jmax (0.0f, (H - blockH) * 0.5f);
    const float upBot    = upTop + barH;
    const float spineTop = upBot + gap + tickH;
    const float spineBot = spineTop + spineH;
    const float loTop    = spineBot + tickH + gap;
    const float loBot    = loTop + barH;

    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    auto paletteAt = [this] (float t, float db, bool dark) noexcept
    {
        return vuPaletteColour (t, db, dark);
    };
    auto xForDb = [this] (float db) noexcept
    {
        return linearXFor (db);
    };

    const float warnX          = linearXFor (warnDbfs);
    const bool  isPixel        = vuBarDetail::isPixelHardware();
    const bool  isBrushedMetal = vuBarDetail::isBrushedMetal();
    const bool  chromaKey      = vuBarDetail::isChromaKey();

    auto renderMirrorBar = [&] (juce::Rectangle<float> barRect, float levelDb,
                                float peakDb, bool isLower)
    {
        vuBarDetail::drawRecessedTrough (g, barRect, 2.0f, darkCanvas);
        const auto inner = barRect.reduced (isPixel ? 2.0f : 1.5f);
        if (inner.getWidth() < 4.0f || inner.getHeight() < 2.0f)
            return;

        vuBarDetail::drawReferenceZoneTint (g, inner, dialGeom.leftX, dialGeom.rightX,
                                            minDb, maxDb, cautDb, warnDbfs, warnX,
                                            darkCanvas, paletteAt);

        vuBarDetail::drawStyledBarFill (g, inner, dialGeom.leftX, dialGeom.rightX,
                                        levelDb, minDb, maxDb, cautDb, warnDbfs,
                                        barFinish_, darkCanvas, isLower, 3.0f,
                                        paletteAt, xForDb);

        // Peak-hold marker ONLY when peakHold == true and peak > minDisplayDb()
        if (vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb))
        {
            const float px = juce::jlimit (inner.getX() + 1.0f, inner.getRight() - 1.0f, linearXFor (peakDb));
            vuBarDetail::drawPeakHoldCap (g, px, inner.getY(), inner.getBottom(),
                                          dialTFor (peakDb), peakDb, warnDbfs, alertCol,
                                          darkCanvas, paletteAt);
        }
    };

    const juce::Rectangle<float> upperRect (dialGeom.leftX, upTop, scaleW, barH);
    const juce::Rectangle<float> lowerRect (dialGeom.leftX, loTop, scaleW, barH);

    const float upLevel = stereo ? displayedL   : singleDb;
    const float loLevel = stereo ? displayedR   : singleDb;
    const float upPeak  = stereo ? peakHoldL    : singlePk;
    const float loPeak  = stereo ? peakHoldR    : singlePk;

    renderMirrorBar (upperRect, upLevel, upPeak, false);
    renderMirrorBar (lowerRect, loLevel, loPeak, true);

    // Central dB spine guide rails + symmetrical up/down ticks + anti-collision labels
    if (! chromaKey)
    {
        g.setColour (PinkXP::ink.withAlpha (darkCanvas ? 0.30f : 0.36f));
        if (isPixel)
        {
            g.fillRect (juce::roundToInt (dialGeom.leftX), juce::roundToInt (spineTop),
                        juce::roundToInt (scaleW), 1);
            g.fillRect (juce::roundToInt (dialGeom.leftX), juce::roundToInt (spineBot),
                        juce::roundToInt (scaleW), 1);
        }
        else if (isBrushedMetal)
        {
            // Laser-etched CNC metallic spine grooves (1px dark cut + 1px bright specular lip)
            const int sx = juce::roundToInt (dialGeom.leftX);
            const int sw = juce::roundToInt (scaleW);
            const int yA = juce::roundToInt (spineTop);
            const int yB = juce::roundToInt (spineBot);
            g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.65f : 0.45f));
            g.fillRect (sx, yA, sw, 1);
            g.fillRect (sx, yB, sw, 1);
            g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.18f : 0.58f));
            g.fillRect (sx, yA + 1, sw, 1);
            g.fillRect (sx, yB + 1, sw, 1);
        }
        else
        {
            g.fillRect (juce::Rectangle<float> (dialGeom.leftX, spineTop - 0.5f, scaleW, 1.0f));
            g.fillRect (juce::Rectangle<float> (dialGeom.leftX, spineBot - 0.5f, scaleW, 1.0f));
        }
    }

    vuBarDetail::drawScaleRuler (g, ticks, dialGeom.leftX, dialGeom.rightX,
                                 body.getX(), body.getRight(),
                                 spineTop - 0.5f, spineBot + 0.5f,
                                 spineTop, spineH,
                                 vuBarDetail::TickDirection::both,
                                 tickH, tickH * 0.52f,
                                 withLabels, 4.0f, false,
                                 minDb, maxDb, warnDbfs, alertCol, xForDb);

    if (stereo)
    {
        vuBarDetail::drawChannelTag (g, "L", body.getX(), upperRect.getCentreY(),
                                     vuPaletteColour (dialTFor (displayedL), displayedL, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedL, minDb), darkCanvas);
        vuBarDetail::drawChannelTag (g, "R", body.getX(), lowerRect.getCentreY(),
                                     vuPaletteColour (dialTFor (displayedR), displayedR, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedR, minDb), darkCanvas);
    }
    else if (singleTag != nullptr)
    {
        vuBarDetail::drawChannelTag (g, singleTag, body.getX(), (spineTop + spineBot) * 0.5f,
                                     vuPaletteColour (dialTFor (singleDb), singleDb, darkCanvas),
                                     vuBarDetail::isSignalActive (singleDb, minDb), darkCanvas);
    }

    // Crisp vertical warnDbfs accent line uniting upper bar, spine, and lower bar
    if (! chromaKey)
    {
        if (isPixel)
        {
            g.setColour (alertCol.withAlpha (0.80f));
            g.fillRect (juce::roundToInt (warnX), juce::roundToInt (upTop - 2.0f),
                        1, juce::jmax (1, juce::roundToInt ((loBot + 2.0f) - (upTop - 2.0f))));
        }
        else
        {
            g.setColour (alertCol.withAlpha (0.70f));
            g.drawLine (warnX, upTop - 2.0f, warnX, loBot + 2.0f, 1.2f);
        }
    }

    dialGeom.trackTop    = upTop;
    dialGeom.trackBottom = loBot;
    dialGeom.radius      = juce::jmax (4.0f, loBot - upTop);
    dialGeom.bandTop     = spineTop;
    dialGeom.bandBottom  = spineBot;
    dialGeom.rowTop[0]   = upTop;
    dialGeom.rowBot[0]   = loBot;
    dialGeom.valid       = true;
}


// ============================================================================
// 5. Minimal —— Oversized Digital Readout + Precision Slim Level Ribbon(s)
//    Mono: huge centered digital dB focal value + subtle "dBFS" caption over
//          one ultra-clean slim gradient ribbon + glowing cursor bead.
//    Stereo: clean side-by-side L & R large readouts with vertical color accent
//            bars over two slim L/R precision ribbons.
//    Peak-hold tick marker drawn on ribbon(s) ONLY when peakHold == true.
// ============================================================================
void VuMeterModule::drawBarMinimalDial (juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody (dialArea);
    if (body.getWidth() < 52.0f || body.getHeight() < 22.0f)
        return;

    const bool darkCanvas = vuBarDetail::isDarkCanvas();
    const bool stereo     = isStereoMode();
    const char* singleTag = activeChannelTag();
    const float minDb     = minDisplayDb();
    const float maxDb     = maxDisplayDb();
    const float cautDb    = cautionDbfs();
    const float singleDb  = activeSingleDbfs();
    const float singlePk  = activeSinglePeakDbfs();
    const auto  alertCol  = vuAlertColour();

    const bool isPixel        = vuBarDetail::isPixelHardware();
    const bool isBrushedMetal = vuBarDetail::isBrushedMetal();
    const bool isGlass        = vuBarDetail::isGlassDome();
    const bool isNeon         = vuBarDetail::isNeonRings();
    const bool isCrt          = vuBarDetail::isCrtVector();
    const bool chromaKey      = vuBarDetail::isChromaKey();
    const bool greenScreen    = vuBarDetail::isGreenScreen();

    // Subtle top-left "VU" / "VU L" / "VU R" marking aligned with LED row (never overlapping LED; skipped on ChromaKey)
    if (! chromaKey)
    {
        const auto led = ledBounds (dialArea);
        const float leftX = (float) dialArea.getX() + 8.0f;
        const float maxW  = (showLed ? ((float) led.getX() - 6.0f) : ((float) dialArea.getRight() - 8.0f)) - leftX;
        if (maxW >= 24.0f)
        {
            const juce::String vuMark = (singleTag != nullptr) ? (juce::String ("VU ") + singleTag) : juce::String ("VU");
            PinkXP::drawCachedText (g, vuMark,
                                    juce::Rectangle<float> (leftX, (float) led.getY(),
                                                            juce::jmin (48.0f, maxW), (float) led.getHeight()),
                                    juce::Justification::centredLeft, 8.5f, juce::Font::bold,
                                    PinkXP::ink.withAlpha (0.56f), true);
        }
    }

    const float padL = (stereo || singleTag != nullptr) ? 16.0f : 8.0f;
    const float padR = 8.0f;
    dialGeom.leftX  = body.getX() + padL;
    dialGeom.rightX = body.getRight() - padR;
    const float scaleW = dialGeom.rightX - dialGeom.leftX;
    if (scaleW < 24.0f)
        return;

    const float H           = body.getHeight();
    const bool  withLabels  = H >= 46.0f;
    const bool  compactH    = H < 54.0f;
    const float tickLen     = withLabels ? 3.5f : 2.0f;
    const float rulerH      = withLabels ? (tickLen + 11.5f) : 4.0f;
    const float ribbonH     = stereo ? (compactH ? 4.0f : 5.0f) : (compactH ? 5.0f : 6.0f);
    const float ribbonGap   = compactH ? 4.0f : 6.0f;
    const float ribbonsH    = stereo ? (ribbonH * 2.0f + ribbonGap) : ribbonH;
    const float ribbonBotY  = body.getBottom() - rulerH - (compactH ? 1.0f : 2.0f);
    const float ribbonTopY  = ribbonBotY - ribbonsH;
    const float readGap     = compactH ? 3.0f : 7.0f;

    // 1) Upper Focal Area: Oversized Crisp Bold Digital dB Readout (Digital LCD card in Pixel XP!)
    const juce::Rectangle<float> readRect (body.getX(), body.getY(),
                                           body.getWidth(),
                                           juce::jmax (0.0f, ribbonTopY - body.getY() - readGap));
    if (readRect.getHeight() >= 10.0f)
    {
        juce::Colour lcdInk = PinkXP::ink;
        juce::Rectangle<float> innerRead = readRect;

        if (isPixel && ! chromaKey)
        {
            const auto cardI = readRect.toNearestInt().reduced (2, 0);
            if (cardI.getWidth() >= 20 && cardI.getHeight() >= 10)
            {
                const juce::Colour lcdBg = darkCanvas
                                               ? PinkXP::content.darker (0.34f)
                                               : PinkXP::content.interpolatedWith (PinkXP::pink100, 0.24f);
                PinkXP::drawSunken (g, cardI, lcdBg);
                lcdInk = PinkXP::readableInkOn (lcdBg);

                // Subtle 2px horizontal LCD scanlines inside sunken readout card
                const auto lcdIn = cardI.reduced (2);
                g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.14f : 0.045f));
                for (int sy = lcdIn.getY() + 1; sy < lcdIn.getBottom(); sy += 2)
                    g.fillRect (lcdIn.getX(), sy, lcdIn.getWidth(), 1);

                innerRead = lcdIn.toFloat();
            }
        }
        else if (isBrushedMetal && ! chromaKey)
        {
            const auto cardI = readRect.toNearestInt().reduced (2, 0);
            if (cardI.getWidth() >= 20 && cardI.getHeight() >= 10)
            {
                const juce::Colour plateBg = darkCanvas
                                                 ? PinkXP::content.interpolatedWith (juce::Colour (0xff1a1e28), 0.45f)
                                                 : PinkXP::content.interpolatedWith (juce::Colour (0xffe6ecf4), 0.38f);
                PinkXP::Craft::drawReadoutCard (g, cardI, plateBg);
                lcdInk = PinkXP::readableInkOn (plateBg);

                // Subtle horizontal brushed sheen across aluminum digital faceplate
                const auto plateIn = cardI.reduced (2);
                if (! plateIn.isEmpty())
                {
                    juce::ColourGradient sheen (juce::Colours::white.withAlpha (darkCanvas ? 0.06f : 0.22f),
                                                (float) plateIn.getX(), (float) plateIn.getY(),
                                                juce::Colours::transparentWhite,
                                                (float) plateIn.getRight(), (float) plateIn.getBottom(), false);
                    g.setGradientFill (sheen);
                    g.fillRect (plateIn);
                }
                innerRead = plateIn.toFloat();
            }
        }
        else if (isGlass && ! chromaKey)
        {
            const auto cardI = readRect.toNearestInt().reduced (2, 0);
            if (cardI.getWidth() >= 20 && cardI.getHeight() >= 10)
            {
                const juce::Colour glassBg = darkCanvas
                                                 ? PinkXP::content.interpolatedWith (juce::Colour (0xff1a2234), 0.45f)
                                                 : PinkXP::content.interpolatedWith (juce::Colours::white, 0.42f);
                PinkXP::Craft::drawReadoutCard (g, cardI, glassBg);
                lcdInk = PinkXP::readableInkOn (glassBg);
                innerRead = cardI.reduced (3, 2).toFloat();
            }
        }
        else if (isNeon && ! chromaKey)
        {
            const auto cardI = readRect.toNearestInt().reduced (2, 0);
            if (cardI.getWidth() >= 20 && cardI.getHeight() >= 10)
            {
                const juce::Colour hudBg = darkCanvas
                                               ? juce::Colour (0xff0a0d16)
                                               : PinkXP::content.interpolatedWith (PinkXP::pink100, 0.16f);
                PinkXP::Craft::drawReadoutCard (g, cardI, hudBg);
                lcdInk = PinkXP::readableInkOn (hudBg);
                innerRead = cardI.reduced (3, 2).toFloat();
            }
        }
        else if (isCrt && ! chromaKey)
        {
            const auto cardI = readRect.toNearestInt().reduced (2, 0);
            if (cardI.getWidth() >= 20 && cardI.getHeight() >= 10)
            {
                const juce::Colour scopeBg = darkCanvas ? PinkXP::content.darker (0.28f)
                                                        : PinkXP::content.interpolatedWith (PinkXP::sel, 0.05f);
                PinkXP::Craft::drawReadoutCard (g, cardI, scopeBg);
                lcdInk = PinkXP::readableInkOn (scopeBg);

                const auto scIn = cardI.reduced (2);
                g.setColour (juce::Colours::black.withAlpha (darkCanvas ? 0.14f : 0.045f));
                for (int sy = scIn.getY() + 1; sy < scIn.getBottom(); sy += 2)
                    g.fillRect (scIn.getX(), sy, scIn.getWidth(), 1);
                innerRead = scIn.toFloat();
            }
        }

        const bool withCaption = innerRead.getHeight() >= 28.0f;
        const float capH       = withCaption ? 10.5f : 0.0f;
        const auto numArea     = innerRead.withTrimmedBottom (capH);

        if (stereo)
        {
            const float colW = innerRead.getWidth() * 0.5f;
            const float maxHScale = isCrt ? 0.76f : 0.84f;
            const float fh   = juce::jlimit (10.0f, isCrt ? 28.0f : 34.0f,
                                             juce::jmin (numArea.getHeight() * maxHScale,
                                                         juce::jmax (16.0f, colW - 14.0f) / 4.2f));
            g.setFont (PinkXP::getAxisFont (fh, juce::Font::bold));

            // Center vertical separator between L and R readouts (skipped on ChromaKey)
            if (! chromaKey)
            {
                g.setColour (lcdInk.withAlpha (darkCanvas ? 0.20f : 0.22f));
                if (isPixel)
                    g.fillRect (juce::roundToInt (innerRead.getCentreX()),
                                juce::roundToInt (innerRead.getY()) + 2,
                                1, juce::jmax (4, juce::roundToInt (innerRead.getHeight()) - 4));
                else
                    g.fillRect (juce::Rectangle<float> (innerRead.getCentreX() - 0.5f,
                                                        innerRead.getY() + 3.0f,
                                                        1.0f, juce::jmax (6.0f, innerRead.getHeight() - 6.0f)));
            }

            for (int c = 0; c < 2; ++c)
            {
                const float dbVal = (c == 0) ? displayedL : displayedR;
                const bool active = vuBarDetail::isSignalActive (dbVal, minDb);
                const bool hot    = dbVal >= warnDbfs;
                const auto chCol  = hot ? alertCol
                                        : vuPaletteColour (dialTFor (dbVal), dbVal, darkCanvas);
                const float cx    = innerRead.getX() + colW * (float) c;
                const juce::Rectangle<float> cell (cx + 4.0f, numArea.getY(),
                                                   juce::jmax (12.0f, colW - 8.0f), numArea.getHeight());

                // Vertical color accent bar alongside each channel's readout (sharp 0px block in Pixel XP!)
                const float barH = juce::jlimit (8.0f, numArea.getHeight() * 0.72f, fh * 0.85f);
                const juce::Rectangle<float> accentBar (cell.getX(),
                                                        cell.getCentreY() - barH * 0.5f,
                                                        3.0f, barH);
                if (active || ! chromaKey)
                {
                    if (isPixel)
                    {
                        g.setColour (active ? chCol : lcdInk.withAlpha (0.20f));
                        g.fillRect (accentBar.toNearestInt());
                    }
                    else if (isBrushedMetal || isGlass || isNeon || isCrt)
                    {
                        if (active)
                            PinkXP::Craft::drawMeterCell (g, accentBar, chCol, true, 0.0f);
                        else
                        {
                            g.setColour (lcdInk.withAlpha (0.18f));
                            if (isGlass) g.fillRoundedRectangle (accentBar, 1.5f);
                            else         g.fillRect (accentBar.toNearestInt());
                        }
                    }
                    else
                    {
                        g.setColour (active ? chCol : lcdInk.withAlpha (0.20f));
                        g.fillRoundedRectangle (accentBar, 1.5f);
                    }
                }

                const auto valStr = vuBarDetail::numericReadoutOnly (formatDbReadout (dbVal));
                const auto txtRect = cell.withTrimmedLeft (5.0f);

                if (darkCanvas && active && ! chromaKey)
                {
                    if (isPixel)
                    {
                        // Crisp 1px hard pixel drop shadow on dark LCD
                        g.setColour (juce::Colours::black.withAlpha (0.45f));
                        g.drawText (valStr, txtRect.translated (1.0f, 1.0f), juce::Justification::centred, false);
                    }
                    else
                    {
                        g.setColour (chCol.withAlpha (hot ? 0.22f : 0.12f));
                        g.drawText (valStr, txtRect.translated (0.0f, 1.0f), juce::Justification::centred, false);
                    }
                }

                g.setColour (hot ? alertCol : (active ? lcdInk : lcdInk.withAlpha (0.45f)));
                g.drawText (valStr, txtRect, juce::Justification::centred, false);

                if (withCaption && ! chromaKey)
                {
                    const char* cap = (c == 0) ? "L dBFS" : "R dBFS";
                    PinkXP::drawCachedText (g, cap,
                                            juce::Rectangle<float> (cx + 4.0f, numArea.getBottom(),
                                                                    colW - 8.0f, capH),
                                            juce::Justification::centred, 8.0f, juce::Font::bold,
                                            hot ? alertCol.withAlpha (0.88f) : lcdInk.withAlpha (darkCanvas ? 0.62f : 0.76f),
                                            true);
                }
            }
        }
        else
        {
            const bool active = vuBarDetail::isSignalActive (singleDb, minDb);
            const bool hot    = singleDb >= warnDbfs;
            const auto accent = hot ? alertCol
                                    : vuPaletteColour (dialTFor (singleDb), singleDb, darkCanvas);
            const float fh    = juce::jlimit (12.0f, isCrt ? 34.0f : 44.0f,
                                              juce::jmin (numArea.getHeight() * (isCrt ? 0.78f : 0.88f),
                                                          numArea.getWidth() / 4.2f));
            const auto valStr = vuBarDetail::numericReadoutOnly (formatDbReadout (singleDb));

            g.setFont (PinkXP::getAxisFont (fh, juce::Font::bold));
            if (darkCanvas && active && ! chromaKey)
            {
                if (isPixel)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.45f));
                    g.drawText (valStr, numArea.translated (1.0f, 1.0f), juce::Justification::centred, false);
                }
                else
                {
                    g.setColour (accent.withAlpha (hot ? 0.24f : 0.13f));
                    g.drawText (valStr, numArea.translated (0.0f, 1.2f), juce::Justification::centred, false);
                }
            }

            g.setColour (hot ? alertCol : (active ? lcdInk : lcdInk.withAlpha (0.45f)));
            g.drawText (valStr, numArea, juce::Justification::centred, false);

            if (withCaption && ! chromaKey)
            {
                const juce::String cap = (singleTag != nullptr) ? (juce::String (singleTag) + " dBFS") : juce::String ("dBFS");
                PinkXP::drawCachedText (g, cap,
                                        juce::Rectangle<float> (innerRead.getX(), numArea.getBottom(),
                                                                innerRead.getWidth(), capH),
                                        juce::Justification::centred, 8.2f, juce::Font::bold,
                                        hot ? alertCol.withAlpha (0.88f) : lcdInk.withAlpha (darkCanvas ? 0.62f : 0.76f),
                                        true);
            }
        }
    }

    // 2) Precision Slim Horizontal Level Ribbon(s) (Segmented pixel strip + 3x3 square cursor in Pixel XP!)
    auto paletteAt = [this] (float t, float db, bool dark) noexcept
    {
        return vuPaletteColour (t, db, dark);
    };
    auto xForDb = [this] (float db) noexcept
    {
        return linearXFor (db);
    };
    const float warnX = linearXFor (warnDbfs);

    auto drawRibbon = [&] (float yTop, float height, float levelDb, float peakDb)
    {
        const float rad = height * 0.5f;
        const juce::Rectangle<float> slot (dialGeom.leftX, yTop, scaleW, height);

        if (isPixel)
        {
            const auto slotI = slot.toNearestInt();
            const int  ix0   = slotI.getX();
            const int  iy0   = slotI.getY();
            const int  iw    = juce::jmax (4, slotI.getWidth());
            const int  ih    = juce::jmax (3, slotI.getHeight());

            if (! chromaKey)
            {
                const auto bg = vuBarDetail::troughBackground (darkCanvas);
                g.setColour (bg);
                g.fillRect (ix0, iy0, iw, ih);
                g.setColour (PinkXP::dark.withAlpha (darkCanvas ? 0.85f : 0.55f));
                g.drawRect (ix0 - 1, iy0 - 1, iw + 2, ih + 2, 1);
            }

            // Slim segmented pixel bar strip (3px bricks + 1px hard dark gaps)
            const bool active = vuBarDetail::isSignalActive (levelDb, minDb);
            const int  litX   = active ? juce::jlimit (ix0, ix0 + iw, juce::roundToInt (linearXFor (levelDb))) : ix0;
            const int  stepPx = 4; // 3px pixel cell + 1px gap
            const int  cellW  = 3;
            const float spanDb = juce::jmax (1.0e-4f, maxDb - minDb);

            for (int sx = ix0; sx < ix0 + iw; sx += stepPx)
            {
                const int sw = juce::jmin (cellW, ix0 + iw - sx);
                if (sw <= 0)
                    continue;
                const float t   = juce::jlimit (0.0f, 1.0f, ((float) sx + (float) sw * 0.5f - dialGeom.leftX) / scaleW);
                const float db  = minDb + t * spanDb;
                const auto  col = vuPaletteColour (t, db, darkCanvas);
                const bool  lit = active && (sx < litX);

                if (lit)
                {
                    g.setColour (col);
                    g.fillRect (sx, iy0, sw, ih);
                    if (ih >= 4)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.30f));
                        g.fillRect (sx, iy0, sw, 1);
                    }
                }
                else if (! chromaKey)
                {
                    const bool isRed = db >= warnDbfs;
                    g.setColour (col.withAlpha (isRed ? 0.22f : 0.11f));
                    g.fillRect (sx, iy0, sw, ih);
                }
            }

            // Sharp 3x3 square pixel cursor right at live level edge (ZERO ellipse!)
            if (active)
            {
                const int cy = iy0 + ih / 2;
                const auto tipCol = vuPaletteColour (dialTFor (levelDb), levelDb, darkCanvas);
                const auto curCol = darkCanvas ? tipCol.interpolatedWith (juce::Colours::white, 0.55f) : tipCol;
                g.setColour (PinkXP::dark);
                g.fillRect (litX - 2, cy - 2, 5, 5);
                g.setColour (curCol);
                g.fillRect (litX - 1, cy - 1, 3, 3);
            }

            // Crisp 1px warnDbfs notch tick across strip
            if (! chromaKey)
            {
                g.setColour (alertCol.withAlpha (0.85f));
                g.fillRect (juce::roundToInt (warnX), iy0 - 2, 1, ih + 4);
            }

            // Hard 2px pixel peak-hold tick (STRICTLY gated by peakHold)
            if (vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb))
            {
                const int  ipx    = juce::jlimit (ix0 + 1, ix0 + iw - 1, juce::roundToInt (linearXFor (peakDb)));
                const bool pkAlert = peakDb >= warnDbfs;
                const auto pkCol   = pkAlert ? alertCol : vuPaletteColour (dialTFor (peakDb), peakDb, darkCanvas);
                g.setColour (darkCanvas ? pkCol.interpolatedWith (juce::Colours::white, 0.45f)
                                        : pkCol.darker (0.18f));
                g.fillRect (ipx - 1, iy0 - 2, 2, ih + 4);
            }
            return;
        }

        if (isBrushedMetal)
        {
            // Millennium Chrome: milled 0px metallic ribbon channel + CNC rectangular chamfered fader slider bead
            const auto slotI = slot.toNearestInt();
            const int  ix0   = slotI.getX();
            const int  iy0   = slotI.getY();
            const int  iw    = juce::jmax (4, slotI.getWidth());
            const int  ih    = juce::jmax (3, slotI.getHeight());

            if (! chromaKey)
            {
                PinkXP::Craft::drawPanelWell (g, slotI.expanded (1), vuBarDetail::troughBackground (darkCanvas),
                                              PinkXP::WellKind::trough);
                if (warnX < (float) (ix0 + iw - 1))
                {
                    const int rx = juce::jmax (ix0, juce::roundToInt (warnX));
                    g.setColour (alertCol.withAlpha (darkCanvas ? 0.18f : 0.14f));
                    g.fillRect (rx, iy0, juce::jmax (0, ix0 + iw - rx), ih);
                }
            }

            if (vuBarDetail::isSignalActive (levelDb, minDb))
            {
                const float xLit = juce::jlimit ((float) ix0, (float) (ix0 + iw), linearXFor (levelDb));
                const float litW = xLit - (float) ix0;
                const auto  tipCol = vuPaletteColour (dialTFor (levelDb), levelDb, darkCanvas);

                if (litW > 0.5f)
                {
                    auto grad = vuBarDetail::makePaletteGradient (dialGeom.leftX, dialGeom.rightX,
                                                                  minDb, maxDb, cautDb, warnDbfs,
                                                                  darkCanvas, 1.0f, paletteAt);
                    g.setGradientFill (grad);
                    g.fillRect (juce::Rectangle<float> ((float) ix0, (float) iy0, litW, (float) ih));
                    // Crisp 1px top chrome specular crease + bottom metallic bevel
                    g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.45f : 0.52f));
                    g.fillRect (juce::Rectangle<float> ((float) ix0, (float) iy0, litW, 1.0f));
                    if (ih >= 4)
                    {
                        g.setColour (juce::Colours::black.withAlpha (0.28f));
                        g.fillRect (juce::Rectangle<float> ((float) ix0, (float) (iy0 + ih - 1), litW, 1.0f));
                    }
                }

                // CNC machined rectangular chamfered metallic slider cap at live edge
                const int capW = 6;
                const int capH = ih + 4;
                const juce::Rectangle<int> capR (juce::roundToInt (xLit) - capW / 2,
                                                 iy0 - 2, capW, capH);
                juce::ColourGradient capGrad (juce::Colour (0xfff4f7fc), (float) capR.getX(), (float) capR.getY(),
                                              juce::Colour (0xff565e6c), (float) capR.getX(), (float) capR.getBottom(), false);
                capGrad.addColour (0.48, juce::Colour (0xffaeb6c4));
                g.setGradientFill (capGrad);
                g.fillRect (capR);
                g.setColour (PinkXP::dark);
                g.drawRect (capR, 1);
                // Vivid center jewel indicator stripe inside CNC slider cap
                g.setColour (tipCol.interpolatedWith (juce::Colours::white, darkCanvas ? 0.35f : 0.12f));
                g.fillRect (capR.getCentreX() - 1, capR.getY() + 1, 2, juce::jmax (1, capR.getHeight() - 2));
            }

            if (! chromaKey)
            {
                g.setColour (alertCol.withAlpha (0.78f));
                g.fillRect (juce::roundToInt (warnX), iy0 - 2, 1, ih + 4);
            }

            if (vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb))
            {
                const int  ipx     = juce::jlimit (ix0 + 1, ix0 + iw - 1, juce::roundToInt (linearXFor (peakDb)));
                const bool pkAlert = peakDb >= warnDbfs;
                const auto pkCol   = pkAlert ? alertCol : vuPaletteColour (dialTFor (peakDb), peakDb, darkCanvas);
                if (! greenScreen)
                {
                    g.setColour (pkCol.withAlpha (darkCanvas ? 0.34f : 0.22f));
                    g.fillRect (ipx - 2, iy0 - 3, 4, ih + 6);
                }
                g.setColour (darkCanvas ? pkCol.interpolatedWith (juce::Colours::white, 0.48f)
                                        : pkCol.darker (0.16f));
                g.fillRect (ipx - 1, iy0 - 2, 2, ih + 4);
                g.setColour (juce::Colours::white.withAlpha (0.72f));
                g.fillRect (ipx - 1, iy0 - 2, 1, ih + 4);
            }
            return;
        }

        if (isGlass || isNeon || isCrt)
        {
            // Frosted Glass (crystal orb cursor), Neon Cyber (blazing diamond laser cursor), CRT Terminal (scope cursor bar)
            const bool ribDark = darkCanvas;
            const auto slotI   = slot.toNearestInt();
            const float gRad   = isGlass ? juce::jmin (4.5f, height * 0.5f) : 0.0f;

            if (! chromaKey)
            {
                if (isGlass)
                {
                    vuBarDetail::drawRecessedTrough (g, slot.expanded (0.5f), gRad, darkCanvas);
                }
                else
                {
                    const auto bg = (isNeon && darkCanvas) ? juce::Colour (0xff0a0d16) : vuBarDetail::troughBackground (darkCanvas);
                    PinkXP::Craft::drawPanelWell (g, slotI.expanded (1), bg, PinkXP::WellKind::trough);
                }

                if (warnX < slot.getRight() - 1.0f)
                {
                    const float rx = juce::jmax (slot.getX(), warnX);
                    g.setColour (alertCol.withAlpha (ribDark ? 0.18f : 0.14f));
                    g.fillRect (juce::Rectangle<float> (rx, slot.getY() + 0.5f,
                                                        slot.getRight() - rx - 0.5f,
                                                        juce::jmax (1.0f, height - 1.0f)));
                }
            }

            if (vuBarDetail::isSignalActive (levelDb, minDb))
            {
                const float xLit = juce::jlimit (slot.getX(), slot.getRight(), linearXFor (levelDb));
                const float litW = xLit - slot.getX();
                const auto  tipCol = vuPaletteColour (dialTFor (levelDb), levelDb, ribDark);

                if (litW > 0.5f)
                {
                    if ((isNeon || isCrt) && ! greenScreen)
                    {
                        g.setColour (tipCol.withAlpha (isNeon ? 0.30f : 0.22f));
                        g.fillRect (juce::Rectangle<float> (slot.getX(), slot.getY() - 1.5f, litW, height + 3.0f));
                    }

                    auto grad = vuBarDetail::makePaletteGradient (dialGeom.leftX, dialGeom.rightX,
                                                                  minDb, maxDb, cautDb, warnDbfs,
                                                                  ribDark, 1.0f, paletteAt);
                    g.setGradientFill (grad);
                    if (isGlass)
                        g.fillRoundedRectangle (juce::Rectangle<float> (slot.getX(), slot.getY(), litW, height), gRad);
                    else
                        g.fillRect (juce::Rectangle<float> (slot.getX(), slot.getY(), litW, height));

                    if (isGlass)
                    {
                        g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.45f : 0.65f));
                        g.fillRect (juce::Rectangle<float> (slot.getX(), slot.getY() + 0.4f, litW, juce::jmax (1.0f, height * 0.40f)));
                    }
                    else if (isNeon)
                    {
                        const float ch = juce::jmax (1.2f, height * 0.38f);
                        g.setColour (juce::Colours::white.withAlpha (0.64f));
                        g.fillRect (juce::Rectangle<float> (slot.getX(), slot.getCentreY() - ch * 0.5f, litW, ch));
                    }
                }

                const float cy = slot.getCentreY();
                if (isGlass)
                {
                    // Glossy crystal orb cursor
                    const float orbR = juce::jmax (3.0f, height * 0.66f);
                    if (! greenScreen)
                    {
                        g.setColour (tipCol.withAlpha (darkCanvas ? 0.36f : 0.24f));
                        g.fillEllipse (xLit - orbR * 1.45f, cy - orbR * 1.45f, orbR * 2.9f, orbR * 2.9f);
                    }
                    juce::ColourGradient orbG (juce::Colours::white.withAlpha (0.94f),
                                               xLit - orbR * 0.4f, cy - orbR * 0.45f,
                                               tipCol.darker (0.18f),
                                               xLit + orbR * 0.6f, cy + orbR * 0.65f, true);
                    orbG.addColour (0.50, tipCol.interpolatedWith (juce::Colours::white, 0.25f));
                    g.setGradientFill (orbG);
                    g.fillEllipse (xLit - orbR, cy - orbR, orbR * 2.0f, orbR * 2.0f);
                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.drawEllipse (xLit - orbR, cy - orbR, orbR * 2.0f, orbR * 2.0f, 0.85f);
                }
                else if (isNeon)
                {
                    // Glowing 45-deg neon diamond laser cursor
                    const float dr = juce::jmax (3.2f, height * 0.72f);
                    juce::Path dia;
                    dia.startNewSubPath (xLit,      cy - dr);
                    dia.lineTo          (xLit + dr, cy);
                    dia.lineTo          (xLit,      cy + dr);
                    dia.lineTo          (xLit - dr, cy);
                    dia.closeSubPath();
                    if (! greenScreen)
                    {
                        g.setColour (tipCol.withAlpha (0.42f));
                        g.strokePath (dia, juce::PathStrokeType (3.0f));
                    }
                    g.setColour (tipCol.interpolatedWith (juce::Colours::white, 0.58f));
                    g.fillPath (dia);
                }
                else
                {
                    // Crisp vertical CRT phosphor scope cursor bar
                    const int ix = juce::roundToInt (xLit);
                    const int iy = slotI.getY() - 3;
                    const int ch = slotI.getHeight() + 6;
                    if (! greenScreen)
                    {
                        g.setColour (tipCol.withAlpha (0.34f));
                        g.fillRect (ix - 2, iy, 5, ch);
                    }
                    g.setColour (tipCol.interpolatedWith (juce::Colours::white, 0.50f));
                    g.fillRect (ix - 1, iy, 3, ch);
                }
            }

            if (! chromaKey)
            {
                g.setColour (alertCol.withAlpha (0.76f));
                g.drawLine (warnX, yTop - 1.5f, warnX, yTop + height + 1.5f, 1.1f);
            }

            if (vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb))
            {
                const float px = juce::jlimit (slot.getX() + 1.0f, slot.getRight() - 1.0f, linearXFor (peakDb));
                vuBarDetail::drawPeakHoldCap (g, px, yTop - 2.5f, yTop + height + 2.5f,
                                              dialTFor (peakDb), peakDb, warnDbfs, alertCol,
                                              ribDark, paletteAt);
            }
            return;
        }

        // Smooth skins (& ChromaKey live ribbon only)
        if (! chromaKey)
        {
            // Sleek recessed slot background + subtle contour
            g.setColour (vuBarDetail::troughBackground (darkCanvas));
            g.fillRoundedRectangle (slot, rad);
            g.setColour (PinkXP::ink.withAlpha (darkCanvas ? 0.22f : 0.24f));
            g.drawRoundedRectangle (slot.reduced (0.35f), rad, 0.8f);

            // Subtle red-zone hint inside slot past warnDbfs
            if (warnX < slot.getRight() - 1.0f)
            {
                const float rx = juce::jmax (slot.getX(), warnX);
                g.setColour (alertCol.withAlpha (darkCanvas ? 0.16f : 0.14f));
                g.fillRect (juce::Rectangle<float> (rx, slot.getY() + 0.8f,
                                                    slot.getRight() - rx - 0.8f,
                                                    juce::jmax (1.0f, height - 1.6f)));
            }
        }

        // Live horizontal gradient ribbon fill + crisp glowing cursor dot
        if (vuBarDetail::isSignalActive (levelDb, minDb))
        {
            const float xLit = juce::jlimit (slot.getX(), slot.getRight(), linearXFor (levelDb));
            const float litW = xLit - slot.getX();
            const float tLit = dialTFor (levelDb);
            const auto  tipCol = vuPaletteColour (tLit, levelDb, darkCanvas);

            if (litW > 0.5f)
            {
                juce::Graphics::ScopedSaveState ss (g);
                juce::Path clipSlot;
                clipSlot.addRoundedRectangle (slot.reduced (0.5f), juce::jmax (0.5f, rad - 0.5f));
                g.reduceClipRegion (clipSlot);

                auto grad = vuBarDetail::makePaletteGradient (dialGeom.leftX, dialGeom.rightX,
                                                              minDb, maxDb, cautDb, warnDbfs,
                                                              darkCanvas, 1.0f, paletteAt);
                g.setGradientFill (grad);
                g.fillRect (juce::Rectangle<float> (slot.getX(), slot.getY(), litW, height));

                // Top specular micro-sheen
                g.setColour (juce::Colours::white.withAlpha (darkCanvas ? 0.26f : 0.30f));
                g.fillRect (juce::Rectangle<float> (slot.getX(), slot.getY() + 0.5f, litW, 1.0f));
            }

            // Crisp glowing cursor dot at live level edge
            const float cy   = slot.getCentreY();
            const float haloR = height * 0.92f;
            const float coreR = height * 0.50f;
            if (! greenScreen)
            {
                g.setColour (tipCol.withAlpha (darkCanvas ? 0.38f : 0.26f));
                g.fillEllipse (xLit - haloR, cy - haloR, haloR * 2.0f, haloR * 2.0f);
            }
            g.setColour (darkCanvas ? tipCol.interpolatedWith (juce::Colours::white, 0.52f)
                                    : tipCol);
            g.fillEllipse (xLit - coreR, cy - coreR, coreR * 2.0f, coreR * 2.0f);
            g.setColour (darkCanvas ? juce::Colours::white.withAlpha (0.85f)
                                    : PinkXP::ink.withAlpha (0.55f));
            g.drawEllipse (xLit - coreR, cy - coreR, coreR * 2.0f, coreR * 2.0f, 0.75f);
        }

        // Subtle warnDbfs notch tick across the ribbon
        if (! chromaKey)
        {
            g.setColour (alertCol.withAlpha (0.70f));
            g.drawLine (warnX, yTop - 1.5f, warnX, yTop + height + 1.5f, 1.1f);
        }

        // Peak-hold tick marker ONLY when peakHold == true and peak > minDisplayDb()
        if (vuBarDetail::shouldDrawPeak (peakHold, peakDb, minDb))
        {
            const float px = juce::jlimit (slot.getX() + 1.0f, slot.getRight() - 1.0f, linearXFor (peakDb));
            const float pt = dialTFor (peakDb);
            const bool  pkAlert = peakDb >= warnDbfs;
            const auto  pkCol   = pkAlert ? alertCol : vuPaletteColour (pt, peakDb, darkCanvas);
            const float tickTop = yTop - 2.5f;
            const float tickBot = yTop + height + 2.5f;

            if (! greenScreen)
            {
                g.setColour (pkCol.withAlpha (darkCanvas ? 0.36f : 0.24f));
                g.fillRoundedRectangle (juce::Rectangle<float> (px - 2.2f, tickTop, 4.4f, tickBot - tickTop), 1.5f);
            }
            g.setColour (darkCanvas ? pkCol.interpolatedWith (juce::Colours::white, 0.45f)
                                    : pkCol.darker (0.15f));
            g.fillRect (juce::Rectangle<float> (px - 1.0f, tickTop, 2.0f, tickBot - tickTop));
        }
    };

    if (stereo)
    {
        const float yL = ribbonTopY;
        const float yR = ribbonTopY + ribbonH + ribbonGap;
        drawRibbon (yL, ribbonH, displayedL, peakHoldL);
        drawRibbon (yR, ribbonH, displayedR, peakHoldR);

        vuBarDetail::drawChannelTag (g, "L", body.getX(), yL + ribbonH * 0.5f,
                                     vuPaletteColour (dialTFor (displayedL), displayedL, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedL, minDb), darkCanvas);
        vuBarDetail::drawChannelTag (g, "R", body.getX(), yR + ribbonH * 0.5f,
                                     vuPaletteColour (dialTFor (displayedR), displayedR, darkCanvas),
                                     vuBarDetail::isSignalActive (displayedR, minDb), darkCanvas);
    }
    else
    {
        drawRibbon (ribbonTopY, ribbonH, singleDb, singlePk);

        if (singleTag != nullptr)
        {
            vuBarDetail::drawChannelTag (g, singleTag, body.getX(), ribbonTopY + ribbonH * 0.5f,
                                         vuPaletteColour (dialTFor (singleDb), singleDb, darkCanvas),
                                         vuBarDetail::isSignalActive (singleDb, minDb), darkCanvas);
        }
    }

    // 3) Sparse Clean dB Ticks & Labels Below Ribbon(s)
    std::vector<VuScaleTick> ticks;
    getScaleTicks (ticks);

    vuBarDetail::drawScaleRuler (g, ticks, dialGeom.leftX, dialGeom.rightX,
                                 body.getX(), body.getRight(),
                                 ribbonBotY + 2.0f, ribbonBotY + 2.0f,
                                 ribbonBotY + 2.0f + tickLen + 1.0f, 10.5f,
                                 vuBarDetail::TickDirection::down,
                                 tickLen, tickLen * 0.55f,
                                 withLabels, 10.0f, true,
                                 minDb, maxDb, warnDbfs, alertCol, xForDb);

    dialGeom.trackTop    = body.getY();
    dialGeom.trackBottom = ribbonBotY;
    dialGeom.radius      = juce::jmax (4.0f, ribbonBotY - body.getY());
    dialGeom.bandTop     = ribbonTopY;
    dialGeom.bandBottom  = ribbonBotY;
    dialGeom.valid       = true;
}
