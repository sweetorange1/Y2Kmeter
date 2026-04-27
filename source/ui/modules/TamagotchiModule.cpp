#include "source/ui/modules/TamagotchiModule.h"
#include "source/ui/PinkXPStyle.h"

#include <cmath>
#include <random>

namespace
{
juce::File findTamagotchiAssetsRoot()

{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto roleCut = base.getChildFile ("assets")
                               .getChildFile ("Tamagotchi")
                               .getChildFile ("role_cut_by_xlsx_40x40");
            if (roleCut.isDirectory())
                return roleCut;

            base = base.getParentDirectory();
        }
        return {};
    };

    if (auto f = tryFromBase (juce::File::getCurrentWorkingDirectory()); f.isDirectory())
        return f;

    if (auto f = tryFromBase (juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                                  .getParentDirectory()); f.isDirectory())
        return f;

    return {};
}

juce::File findTamagotchiRolePngDir()
{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto roleDir = base.getChildFile ("assets")
                               .getChildFile ("Tamagotchi")
                               .getChildFile ("role");
            if (roleDir.isDirectory())
                return roleDir;

            base = base.getParentDirectory();
        }
        return {};
    };

    if (auto f = tryFromBase (juce::File::getCurrentWorkingDirectory()); f.isDirectory())
        return f;

    if (auto f = tryFromBase (juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                                  .getParentDirectory()); f.isDirectory())
        return f;

    return {};
}
}
TamagotchiModule::TamagotchiModule()
    : ModulePanel (ModuleType::tamagotchi)
{
    setDefaultSize (80, 80);
    setMinSize (minW, minH);

    loadRandomRoleAnimations();
    startTimerHz (20); // 闲逛更新 20Hz；动画每 20 tick（约 1 秒）切帧
}

TamagotchiModule::~TamagotchiModule()
{
    stopTimer();
}

void TamagotchiModule::setFocusVisual (bool shouldFocus)
{
    if (focused == shouldFocus)
        return;

    focused = shouldFocus;
    if (! focused)
    {
        deleteBtnHovered = false;
        deleteBtnPressed = false;
        hoveredTestButton = -1;
        pressedTestButton = -1;
        if (dragMode == DragMode::none)
            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    repaint();
}

void TamagotchiModule::paint (juce::Graphics& g)

{
    auto bounds = getLocalBounds();

    auto hud = getHudBounds();
    auto barRow = hud.removeFromTop (juce::jmin (16, hud.getHeight()));

    const int halfHudW = barRow.getWidth() / 2;
    auto hungerBounds = barRow.withWidth (halfHudW).reduced (2, 2);
    auto healthBounds = barRow.withTrimmedLeft (halfHudW).reduced (2, 2);

    drawPixelBar (g, hungerBounds, hunger / 100.0f, juce::Colour (0xfff39c3d), "HNG");
    drawPixelBar (g, healthBounds, health / 100.0f, juce::Colour (0xff5bd48a), "HP");

    for (int i = 0; i < testButtonCount; ++i)
    {
        auto b = getTestButtonBounds (i);
        const bool pressed = (i == pressedTestButton);
        const bool hovered = (i == hoveredTestButton);

        if (pressed)
            PinkXP::drawPressed (g, b, PinkXP::pink100);
        else
            PinkXP::drawRaised (g, b, hovered ? PinkXP::pink200 : PinkXP::btnFace);

        static constexpr const char* labels[testButtonCount] = { "+H", "-H", "+HP", "-HP" };
        auto textBounds = b;
        if (pressed)
            textBounds.translate (1, 1);

        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (8.5f, juce::Font::bold));
        g.drawText (labels[i], textBounds, juce::Justification::centred, false);
    }

    bounds.removeFromTop (juce::jmin (hudHeight, bounds.getHeight()));

    const auto frame = getCurrentFrame();
    if (frame.isNull())
    {
        g.setColour (PinkXP::ink.withAlpha (0.7f));
        g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
        g.drawText ("Tamagotchi", bounds.removeFromTop (18), juce::Justification::centred, false);

        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.setFont (PinkXP::getFont (9.0f));
        g.drawFittedText ("assets/Tamagotchi not found", bounds.reduced (4), juce::Justification::centred, 2);
    }
    else
    {
        const float x = std::round (petPos.x);
        const float y = std::round (petPos.y);

        if (motionMode == MotionMode::patrol && ! patrolMoveLeft)
        {
            const auto t = juce::AffineTransform::scale (-1.0f, 1.0f)
                               .translated (x + (float) frame.getWidth(), y);
            g.drawImageTransformed (frame, t, false);
        }
        else
        {
            g.drawImageAt (frame, (int) x, (int) y);
        }
    }

    if (focused)
    {
        auto focusBounds = getFocusBounds();
        g.setColour (PinkXP::pink300.withAlpha (0.95f));
        g.drawRect (focusBounds, 1);

        auto del = getDeleteButtonBounds();
        if (deleteBtnPressed)
            PinkXP::drawPressed (g, del, PinkXP::pink100);
        else
            PinkXP::drawRaised (g, del, deleteBtnHovered ? PinkXP::pink200 : PinkXP::btnFace);

        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
        auto txt = del;
        txt.translate (-1, -1);
        if (deleteBtnPressed) txt.translate (1, 1);
        g.drawText ("x", txt, juce::Justification::centred, false);
    }
}

void TamagotchiModule::resized()
{
    hoveredTestButton = -1;
    pressedTestButton = -1;

    const auto now = getLocalBounds();

    const bool grew = (! lastLocalBounds.isEmpty())
                   && (now.getWidth() > lastLocalBounds.getWidth()
                       || now.getHeight() > lastLocalBounds.getHeight());

    const auto frame = getCurrentFrame();
    if (frame.isNull())
    {
        lastLocalBounds = now;
        return;
    }

    const auto playArea = now.withTrimmedTop (juce::jmin (hudHeight, now.getHeight()));
    const float minX = 0.0f;
    const float maxX = (float) juce::jmax (0, playArea.getWidth() - frame.getWidth());
    const float floorY = (float) juce::jmax (playArea.getY(), playArea.getBottom() - frame.getHeight());
    const float halfW = (float) frame.getWidth() * 0.5f;
    const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
    const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);

    if (! hasPetPosition)
    {
        petGroundAnchorX = halfW;
        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = floorY;
        hasPetPosition = true;
    }
    else
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = juce::jlimit ((float) playArea.getY(), floorY, petPos.y);

        if (motionMode == MotionMode::patrol)
            petPos.y = floorY;

    }

    if (grew && hasAnimation ((int) PetAnim::startled))
    {
        motionMode = MotionMode::startledIntro;
        idleTicksRemaining = 0;
        fallVelocityPxPerTick = 0.0f;
        forceAnimation ((int) PetAnim::startled);
    }

    lastLocalBounds = now;
}

void TamagotchiModule::mouseMove (const juce::MouseEvent& e)
{
    if (! focused)
    {
        updateCursorFor (Edge::none);
        return;
    }

    const bool hoveredDel = getDeleteButtonBounds().contains (e.getPosition());
    if (hoveredDel != deleteBtnHovered)
    {
        deleteBtnHovered = hoveredDel;
        repaint (getDeleteButtonBounds());
    }

    const int testHit = hitTestButton (e.getPosition());
    if (testHit != hoveredTestButton)
    {
        const int old = hoveredTestButton;
        hoveredTestButton = testHit;
        if (old >= 0) repaint (getTestButtonBounds (old));
        if (hoveredTestButton >= 0) repaint (getTestButtonBounds (hoveredTestButton));
    }

    if (! hoveredDel && hoveredTestButton < 0)
        updateCursorFor (detectEdge (e.getPosition()));
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TamagotchiModule::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    deleteBtnHovered = false;
    const int oldHover = hoveredTestButton;
    hoveredTestButton = -1;
    if (dragMode == DragMode::none)
        setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint (getDeleteButtonBounds());
    if (oldHover >= 0)
        repaint (getTestButtonBounds (oldHover));
}

void TamagotchiModule::mouseDown (const juce::MouseEvent& e)
{
    toFront (true);
    if (onBroughtToFront)
        onBroughtToFront (*this);

    setFocusVisual (true);

    const auto pos = e.getPosition();
    if (getDeleteButtonBounds().contains (pos))
    {
        deleteBtnPressed = true;
        repaint (getDeleteButtonBounds());
        return;
    }

    const int testHit = hitTestButton (pos);
    if (testHit >= 0)
    {
        pressedTestButton = testHit;
        repaint (getTestButtonBounds (pressedTestButton));
        return;
    }

    const auto edge = detectEdge (pos);

    if (edge != Edge::none)
    {
        dragMode = DragMode::resize;
        resizeEdge = edge;
        dragStartMouse = e.getEventRelativeTo (getParentComponent()).getPosition();
        dragStartBounds = getBounds();
        return;
    }

    dragMode = DragMode::move;
    dragStartMouse = e.getEventRelativeTo (getParentComponent()).getPosition();
    dragStartBounds = getBounds();
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    repaint();
}

void TamagotchiModule::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::none)
        return;

    const auto mouseInParent = e.getEventRelativeTo (getParentComponent()).getPosition();
    const auto delta = mouseInParent - dragStartMouse;

    if (dragMode == DragMode::move)
    {
        auto newTopLeft = dragStartBounds.getTopLeft() + delta;

        if (auto* parent = getParentComponent())
        {
            const int maxX = juce::jmax (0, parent->getWidth()  - getWidth());
            const int maxY = juce::jmax (0, parent->getHeight() - getHeight());
            newTopLeft.x = juce::jlimit (0, maxX, newTopLeft.x);
            newTopLeft.y = juce::jlimit (0, maxY, newTopLeft.y);
        }

        setTopLeftPosition (newTopLeft);
    }
    else
    {
        auto newBounds = dragStartBounds;

        if (resizeEdge == Edge::right || resizeEdge == Edge::bottomRight)
            newBounds.setWidth (juce::jmax (minW, dragStartBounds.getWidth() + delta.x));
        if (resizeEdge == Edge::bottom || resizeEdge == Edge::bottomRight)
            newBounds.setHeight (juce::jmax (minH, dragStartBounds.getHeight() + delta.y));

        if (auto* parent = getParentComponent())
        {
            newBounds.setWidth  (juce::jmin (newBounds.getWidth(),  parent->getWidth()  - newBounds.getX()));
            newBounds.setHeight (juce::jmin (newBounds.getHeight(), parent->getHeight() - newBounds.getY()));
        }

        setBounds (newBounds);
    }

    if (onBoundsDragging)
        onBoundsDragging (*this);
}

void TamagotchiModule::mouseUp (const juce::MouseEvent& e)
{
    if (deleteBtnPressed)
    {
        deleteBtnPressed = false;
        const bool stillOnDel = getDeleteButtonBounds().contains (e.getPosition());
        repaint (getDeleteButtonBounds());

        if (stillOnDel && onCloseClicked)
            onCloseClicked (*this);
        return;
    }

    if (pressedTestButton >= 0)
    {
        const int releasedOn = hitTestButton (e.getPosition());
        const int applied = (releasedOn == pressedTestButton) ? pressedTestButton : -1;
        const int oldPressed = pressedTestButton;
        pressedTestButton = -1;
        repaint (getTestButtonBounds (oldPressed));
        if (applied >= 0)
            applyTestButton (applied);
        return;
    }

    if (dragMode != DragMode::none)
    {
        dragMode = DragMode::none;
        resizeEdge = Edge::none;
        setMouseCursor (juce::MouseCursor::NormalCursor);

        if (onBoundsChangedByUser)
            onBoundsChangedByUser (*this);
    }
}

void TamagotchiModule::timerCallback()
{
    updateNeeds();
    stepWander();

    if (idleTicksRemaining > 0)
    {
        --idleTicksRemaining;
        return;
    }

    ++frameTickCounter;
    if (frameTickCounter >= 20)
    {
        frameTickCounter = 0;
        stepOneFrame();
    }
}

bool TamagotchiModule::loadRandomRoleAnimations()
{
    for (auto& arr : animFrames)
        arr.clearQuick();
    availableAnimIds.clearQuick();
    currentAnimId = 1;
    currentFrameIdx = 0;
    hasPetPosition = false;

    const auto root = findTamagotchiAssetsRoot();

    if (root.isDirectory())
    {
        juce::Array<juce::File> roleDirs;
        root.findChildFiles (roleDirs, juce::File::findDirectories, false);
        if (! roleDirs.isEmpty())
        {
            auto chosenRoleDir = roleDirs.getReference (
                juce::Random::getSystemRandom().nextInt (roleDirs.size()));
            roleName = chosenRoleDir.getFileName();

            juce::Array<juce::File> pngFiles;
            chosenRoleDir.findChildFiles (pngFiles, juce::File::findFiles, false, "*.png");

            for (const auto& pf : pngFiles)
            {
                auto src = juce::ImageFileFormat::loadFrom (pf);
                if (src.isNull())
                    continue;

                const auto stem = pf.getFileNameWithoutExtension();
                juce::StringArray tokens;
                tokens.addTokens (stem, "_", "");
                if (tokens.size() < 4)
                    continue;

                const int animId = tokens[tokens.size() - 2].getIntValue();
                if (animId < 1 || animId > 33)
                    continue;

                animFrames[(size_t) (animId - 1)].add (src);
            }

            for (int i = 0; i < 33; ++i)
                if (animFrames[(size_t) i].size() > 0)
                    availableAnimIds.addIfNotAlreadyThere (i + 1);

            if (! availableAnimIds.isEmpty())
            {
                motionMode = MotionMode::patrol;
                patrolMoveLeft = true;
                idleTicksRemaining = 0;
                chooseNextAnimation();
                hasPetPosition = false;
                repaint();
                return true;
            }

        }
    }

    // 兜底：如果切帧目录不可用，退化为 role 目录随机单图（至少有可见内容）
    const auto rolePngDir = findTamagotchiRolePngDir();
    if (! rolePngDir.isDirectory())
        return false;

    juce::Array<juce::File> rolePngs;
    rolePngDir.findChildFiles (rolePngs, juce::File::findFiles, false, "*.png");
    if (rolePngs.isEmpty())
        return false;

    auto selected = rolePngs.getReference (juce::Random::getSystemRandom().nextInt (rolePngs.size()));
    auto single = juce::ImageFileFormat::loadFrom (selected);
    if (single.isNull())
        return false;

    roleName = selected.getFileNameWithoutExtension();
    currentAnimId = 1;
    currentFrameIdx = 0;
    animFrames[0].clearQuick();
    animFrames[0].add (single);
    availableAnimIds.clearQuick();
    availableAnimIds.add (1);
    motionMode = MotionMode::patrol;
    patrolMoveLeft = true;
    idleTicksRemaining = 0;
    hasPetPosition = false;

    repaint();
    return true;

}

void TamagotchiModule::chooseNextAnimation()
{
    if (availableAnimIds.isEmpty())
        return;

    const auto oldFrame = getCurrentFrame();
    if (! oldFrame.isNull())
        petGroundAnchorX = petPos.x + (float) oldFrame.getWidth() * 0.5f;

    const int nextAnim = chooseNextAnimByState();
    currentAnimId = juce::jlimit (1, 33, nextAnim);
    currentFrameIdx = 0;

    const auto newFrame = getCurrentFrame();
    if (! newFrame.isNull())
        petPos.x = petGroundAnchorX - (float) newFrame.getWidth() * 0.5f;
}

void TamagotchiModule::stepOneFrame()
{
    if (availableAnimIds.isEmpty())
        return;

    int animIdx = juce::jlimit (1, 33, currentAnimId) - 1;
    auto* frames = &animFrames[(size_t) animIdx];

    if (frames->isEmpty())
    {
        chooseNextAnimation();
        animIdx = juce::jlimit (1, 33, currentAnimId) - 1;
        frames = &animFrames[(size_t) animIdx];
        if (frames->isEmpty())
            return;
    }

    ++currentFrameIdx;
    if (currentFrameIdx >= frames->size())
        onAnimationFinished();

    repaint();
}

int TamagotchiModule::randomAnimFrom (std::initializer_list<int> ids) const
{
    juce::Array<int> candidates;
    for (int id : ids)
    {
        const int safeId = juce::jlimit (1, 33, id);
        if (availableAnimIds.contains (safeId))
            candidates.addIfNotAlreadyThere (safeId);
    }

    if (candidates.isEmpty())
        return availableAnimIds.getReference (juce::Random::getSystemRandom().nextInt (availableAnimIds.size()));

    return candidates.getReference (juce::Random::getSystemRandom().nextInt (candidates.size()));
}

bool TamagotchiModule::hasAnimation (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);
    return availableAnimIds.contains (safeId) && ! animFrames[(size_t) (safeId - 1)].isEmpty();
}

void TamagotchiModule::forceAnimation (int animId, bool restartFrame)
{
    if (! hasAnimation (animId))
        return;

    const auto oldFrame = getCurrentFrame();
    if (! oldFrame.isNull())
        petGroundAnchorX = petPos.x + (float) oldFrame.getWidth() * 0.5f;

    currentAnimId = juce::jlimit (1, 33, animId);
    if (restartFrame)
        currentFrameIdx = 0;

    const auto newFrame = getCurrentFrame();
    if (! newFrame.isNull())
        petPos.x = petGroundAnchorX - (float) newFrame.getWidth() * 0.5f;
}

int TamagotchiModule::chooseNextAnimByState() const
{
    switch (motionMode)
    {
        case MotionMode::patrol:
            return randomAnimFrom ({ (int) PetAnim::moveLeft,
                                     (int) PetAnim::dazeThenWalkLeft,
                                     (int) PetAnim::lookLeft,
                                     (int) PetAnim::lookAround });

        case MotionMode::startledIntro:
            return hasAnimation ((int) PetAnim::startled)
                ? (int) PetAnim::startled
                : randomAnimFrom ({ (int) PetAnim::shocked, (int) PetAnim::fall });

        case MotionMode::falling:
            return hasAnimation ((int) PetAnim::startled)
                ? (int) PetAnim::startled
                : randomAnimFrom ({ (int) PetAnim::shocked });

        case MotionMode::landingFall:
            return hasAnimation ((int) PetAnim::fall)
                ? (int) PetAnim::fall
                : randomAnimFrom ({ (int) PetAnim::shocked, (int) PetAnim::lookAround });
    }

    return randomAnimFrom ({ (int) PetAnim::lookAround });
}

void TamagotchiModule::onAnimationFinished()
{
    currentFrameIdx = 0;

    switch (motionMode)
    {
        case MotionMode::patrol:
            idleTicksRemaining = juce::Random::getSystemRandom().nextInt ({ 0, 5 });
            chooseNextAnimation();
            break;

        case MotionMode::startledIntro:
            motionMode = MotionMode::falling;
            fallVelocityPxPerTick = 0.0f;
            break;

        case MotionMode::falling:
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::landingFall:
            motionMode = MotionMode::patrol;
            idleTicksRemaining = juce::Random::getSystemRandom().nextInt ({ 2, 8 });
            chooseNextAnimation();
            break;
    }
}

void TamagotchiModule::setSignalLevel01 (float level01) noexcept
{
    signalLevel01 = juce::jlimit (0.0f, 1.0f, level01);
}

void TamagotchiModule::updateNeeds()
{
    // 20Hz tick；按“每秒速率 / 20”累计
    constexpr float dt = 1.0f / 20.0f;

    const float loudThreshold = 0.45f;
    const float loudBoostK = 6.0f;
    const float baseHungerRate = 0.8f;

    const float loudBoost = signalLevel01 > loudThreshold
        ? loudBoostK * (signalLevel01 - loudThreshold)
        : 0.0f;

    hunger += (baseHungerRate + loudBoost) * dt;

    float healthDelta = 0.0f;
    if (hunger < 40.0f)
        healthDelta += 0.35f * dt;
    if (hunger > 70.0f)
        healthDelta -= 0.5f * dt;
    if (hunger > 90.0f)
        healthDelta -= 1.2f * dt;
    if (signalLevel01 > 0.75f)
        healthDelta -= 0.6f * dt;

    health += healthDelta;

    hunger = juce::jlimit (0.0f, 100.0f, hunger);
    health = juce::jlimit (0.0f, 100.0f, health);
}

void TamagotchiModule::drawPixelBar (juce::Graphics& g,
                                     juce::Rectangle<int> area,
                                     float value01,
                                     juce::Colour fill,
                                     juce::StringRef label) const
{
    if (area.getWidth() < 8 || area.getHeight() < 6)
        return;

    value01 = juce::jlimit (0.0f, 1.0f, value01);

    g.setColour (PinkXP::ink.withAlpha (0.8f));
    g.fillRect (area);

    auto inner = area.reduced (1);
    g.setColour (juce::Colour (0xff1a1a1a));
    g.fillRect (inner);

    const int segCount = juce::jmax (1, inner.getWidth() / 4);
    const int lit = juce::roundToInt ((float) segCount * value01);

    for (int i = 0; i < segCount; ++i)
    {
        juce::Rectangle<int> seg (inner.getX() + i * 4, inner.getY() + 1, 3, inner.getHeight() - 2);

        if (i < lit)
            g.setColour (fill);
        else
            g.setColour (juce::Colour (0xff2a2a2a));

        g.fillRect (seg);
    }

    g.setColour (PinkXP::ink.withAlpha (0.92f));
    g.setFont (PinkXP::getFont (8.0f, juce::Font::bold));
    g.drawText (label, area.removeFromLeft (22), juce::Justification::centredLeft, false);
}

juce::Rectangle<int> TamagotchiModule::getHudBounds() const
{
    auto b = getLocalBounds();
    return b.removeFromTop (juce::jmin (hudHeight, b.getHeight()));
}

juce::Rectangle<int> TamagotchiModule::getTestButtonBounds (int idx) const
{
    const int safeIdx = juce::jlimit (0, testButtonCount - 1, idx);
    auto hud = getHudBounds();
    auto btnRow = hud.withTrimmedTop (16).reduced (2, 1);

    const int gap = 2;
    const int totalGap = gap * (testButtonCount - 1);
    const int cellW = juce::jmax (8, (btnRow.getWidth() - totalGap) / testButtonCount);
    const int x = btnRow.getX() + safeIdx * (cellW + gap);
    return { x, btnRow.getY(), cellW, juce::jmax (10, btnRow.getHeight()) };
}

int TamagotchiModule::hitTestButton (juce::Point<int> pos) const
{
    for (int i = 0; i < testButtonCount; ++i)
        if (getTestButtonBounds (i).contains (pos))
            return i;

    return -1;
}

void TamagotchiModule::applyTestButton (int idx)
{
    switch (idx)
    {
        case 0: hunger += 10.0f; break; // +H
        case 1: hunger -= 10.0f; break; // -H
        case 2: health += 10.0f; break; // +HP
        case 3: health -= 10.0f; break; // -HP
        default: break;
    }

    hunger = juce::jlimit (0.0f, 100.0f, hunger);
    health = juce::jlimit (0.0f, 100.0f, health);
    repaint (getHudBounds());
}

juce::Image TamagotchiModule::getCurrentFrame() const

{
    const int animIdx = juce::jlimit (1, 33, currentAnimId) - 1;
    const auto& frames = animFrames[(size_t) animIdx];
    if (frames.isEmpty())
        return {};

    const int frameIdx = juce::jlimit (0, frames.size() - 1, currentFrameIdx);
    return frames.getReference (frameIdx);
}

void TamagotchiModule::stepWander()
{
    const auto frame = getCurrentFrame();
    if (frame.isNull())
        return;

    const auto area = getLocalBounds();
    const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
    const float halfW = (float) frame.getWidth() * 0.5f;
    const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
    const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);
    const float floorY = (float) juce::jmax (playArea.getY(), playArea.getBottom() - frame.getHeight());

    if (! hasPetPosition)
    {
        petGroundAnchorX = anchorMin;
        petPos = { juce::jmax (0.0f, petGroundAnchorX - halfW), floorY };
        hasPetPosition = true;
    }

    switch (motionMode)
    {
        case MotionMode::patrol:
        {
            petPos.y = floorY;
            if (! focused)
            {
                const float dir = patrolMoveLeft ? -1.0f : 1.0f;
                petGroundAnchorX += dir * patrolSpeedPxPerTick;
            }

            if (petGroundAnchorX <= anchorMin)
            {
                petGroundAnchorX = anchorMin;
                patrolMoveLeft = false;
            }
            else if (petGroundAnchorX >= anchorMax)
            {
                petGroundAnchorX = anchorMax;
                patrolMoveLeft = true;
            }

            break;
        }

        case MotionMode::startledIntro:
            break;

        case MotionMode::falling:
        {
            fallVelocityPxPerTick += gravityPxPerTick2;
            petPos.y += fallVelocityPxPerTick;

            if (petPos.y >= floorY)
            {
                petPos.y = floorY;
                motionMode = MotionMode::landingFall;
                fallVelocityPxPerTick = 0.0f;
                forceAnimation ((int) PetAnim::fall);
            }
            break;
        }

        case MotionMode::landingFall:
            petPos.y = floorY;
            break;
    }

    petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

    const float minX = 0.0f;
    const float maxX = (float) juce::jmax (0, playArea.getWidth() - frame.getWidth());
    petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
    petPos.y = juce::jlimit ((float) playArea.getY(), floorY, petPos.y);

    repaint();
}

juce::Rectangle<int> TamagotchiModule::getFocusBounds() const
{
    return getLocalBounds().reduced (1);
}

juce::Rectangle<int> TamagotchiModule::getDeleteButtonBounds() const
{
    auto focus = getFocusBounds();
    return { focus.getRight() - deleteButtonSize - 2, focus.getY() + 2, deleteButtonSize, deleteButtonSize };
}

TamagotchiModule::Edge TamagotchiModule::detectEdge (juce::Point<int> pos) const
{
    const auto b = getLocalBounds();
    const bool nearRight  = pos.x >= b.getRight()  - edgeHotSize;
    const bool nearBottom = pos.y >= b.getBottom() - edgeHotSize;

    if (nearRight && nearBottom) return Edge::bottomRight;
    if (nearRight)               return Edge::right;
    if (nearBottom)              return Edge::bottom;
    return Edge::none;
}

void TamagotchiModule::updateCursorFor (Edge e)
{
    switch (e)
    {
        case Edge::right:        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Edge::bottom:       setMouseCursor (juce::MouseCursor::UpDownResizeCursor); break;
        case Edge::bottomRight:  setMouseCursor (juce::MouseCursor::BottomRightCornerResizeCursor); break;
        default:                 setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}
