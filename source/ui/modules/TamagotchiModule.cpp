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

juce::File findTamagotchiMirrorAssetsRoot()

{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto roleCut = base.getChildFile ("assets")
                               .getChildFile ("Tamagotchi")
                               .getChildFile ("role_cut_by_xlsx_40x40_fan");
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

juce::File findTamagotchiEggAssetsDir()
{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto eggDir = base.getChildFile ("assets")
                              .getChildFile ("Tamagotchi")
                              .getChildFile ("egg_38x38");
            if (eggDir.isDirectory())
                return eggDir;

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

bool loadRandomEggFrames (juce::Array<juce::Image>& outEggFrames, int& outStyleId)
{
    outEggFrames.clearQuick();
    outStyleId = 1;

    const auto eggDir = findTamagotchiEggAssetsDir();
    if (! eggDir.isDirectory())
        return false;

    constexpr int styleCount = 8;
    constexpr int frameCount = 4;
    juce::Array<int> styleOrder;
    for (int i = 1; i <= styleCount; ++i)
        styleOrder.add (i);

    for (int i = styleOrder.size() - 1; i > 0; --i)
    {
        const int j = juce::Random::getSystemRandom().nextInt (i + 1);
        styleOrder.swap (i, j);
    }

    for (const int styleId : styleOrder)
    {
        juce::Array<juce::Image> candidateFrames;
        bool validStyle = true;

        for (int frame = 1; frame <= frameCount; ++frame)
        {
            const int seq = (styleId - 1) * frameCount + (frame - 1);
            const auto fileName = juce::String::formatted ("egg_%03d_%d_%d.png", seq, styleId, frame);
            const auto image = juce::ImageFileFormat::loadFrom (eggDir.getChildFile (fileName));
            if (image.isNull())
            {
                validStyle = false;
                break;
            }

            candidateFrames.add (image);
        }

        if (validStyle && candidateFrames.size() >= frameCount)
        {
            outEggFrames = candidateFrames;
            outStyleId = styleId;
            return true;
        }
    }

    return false;
}

bool loadRoleFramesFromDirectory (const juce::File& roleDir,
                                  const juce::File& mirrorRootDir,
                                  std::array<juce::Array<juce::Image>, 33>& animFrames,
                                  std::array<juce::Array<juce::Image>, 33>& animFramesRight,
                                  juce::Array<int>& availableAnimIds)
{
    if (! roleDir.isDirectory())
        return false;

    for (auto& arr : animFrames)
        arr.clearQuick();
    for (auto& arr : animFramesRight)
        arr.clearQuick();
    availableAnimIds.clearQuick();

    auto appendFramesFromDir = [] (const juce::File& dir,
                                   bool forceRight,
                                   std::array<juce::Array<juce::Image>, 33>& outLeft,
                                   std::array<juce::Array<juce::Image>, 33>& outRight)
    {
        if (! dir.isDirectory())
            return;

        juce::Array<juce::File> pngFiles;
        dir.findChildFiles (pngFiles, juce::File::findFiles, false, "*.png");

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

            bool isRightVariant = forceRight;
            if (! forceRight)
            {
                const juce::String lowerStem = stem.toLowerCase();
                isRightVariant = lowerStem.contains ("_right")
                              || lowerStem.contains ("_r")
                              || lowerStem.contains ("_mirror")
                              || lowerStem.contains ("_mirrored")
                              || lowerStem.contains ("_flip")
                              || lowerStem.contains ("_flipped");
            }

            if (isRightVariant)
                outRight[(size_t) (animId - 1)].add (src);
            else
                outLeft[(size_t) (animId - 1)].add (src);
        }
    };

    appendFramesFromDir (roleDir, false, animFrames, animFramesRight);

    if (mirrorRootDir.isDirectory())
    {
        const auto mirrorRoleDir = mirrorRootDir.getChildFile (roleDir.getFileName());
        appendFramesFromDir (mirrorRoleDir, true, animFrames, animFramesRight);
    }

    for (int i = 0; i < 33; ++i)
        if (animFrames[(size_t) i].size() > 0 || animFramesRight[(size_t) i].size() > 0)
            availableAnimIds.addIfNotAlreadyThere (i + 1);

    return ! availableAnimIds.isEmpty();
}

juce::String randomIdleSpeech()
{
    static const juce::StringArray lines {
        "Hmm... I should keep practicing my idle face.",
        "I wonder what's for dinner today.",
        "Nice breeze. Let me zone out for a bit.",
        "Please wait, I'm thinking about life.",
        "Should I go find a snack now?"
    };

    if (lines.isEmpty())
        return {};

    return lines[juce::Random::getSystemRandom().nextInt (lines.size())];
}

constexpr int dbgPatrolLookLeftSlow  = 101;
constexpr int dbgPatrolLookRightSlow = 102;
constexpr int dbgPatrolMoveLeftFast  = 103;
constexpr int dbgPatrolMoveRightFast = 104;
constexpr int dbgPatrolTalk          = 105;
constexpr int dbgPatrolJumpFight     = 106;
constexpr int dbgEggIdle             = 1001;
constexpr int dbgEggHatching         = 1002;
}

TamagotchiModule::TamagotchiModule()
    : ModulePanel (ModuleType::tamagotchi)
{
    setDefaultSize (128, 128);
    setMinSize (minW, minH);

    // ---- 拖影 / focus 残留 修复（最终版）----
    //
    // 本模块 paint() 有大量非完整覆盖的区域（playArea 空白、focus rect 外的
    // 非聚焦帧、精灵透明通道）—— 必须声明为 non-opaque，让 JUCE 在每次
    // dirty repaint 时安排父组件（ModuleWorkspace）先重绘相同区域的底色（
    // content.withAlpha(0.70f) 透出桌面），再把本组件 paint 叠上来。
    //
    // 但 JUCE 的 repaint(localRect) 对 non-opaque 子组件并不会自动追质到根，
    // 当父组件也 non-opaque（ModuleWorkspace 本身就是）时，旧像素仍会留在
    // 缓冲中。解决方式：所有从本类发起的重绘均通过 repaintSelfAndParent()
    // 统一送出，其内部同时 getParentComponent()->repaint(parentRect) + repaint(rect)，
    // 来确保底色真的被重新绘制。
    //
    // 注：这会覆盖 ModulePanel 构造器默认的 setOpaque(true)。
    setOpaque (false);

    currentPatrolAction = PatrolAction::lookLeftSlow;
    patrolActionTicksRemaining = 0;
    patrolCooldownTicksRemaining = patrolCycleTicks;
    patrolLockedAnimId = 0;
    patrolFaceLeft = true;
    patrolActionMoveDir = 0.0f;
    patrolActionSpeedPxPerTick = 0.0f;

    showSpeechBubble = false;
    jumpFightActive = false;

    stateModeCombo.addItem ("Auto", 1);
    stateModeCombo.addItem ("Egg", 2);
    stateModeCombo.addItem ("Hatching", 3);
    stateModeCombo.addItem ("Toilet", 4);
    stateModeCombo.addItem ("Patrol", 5);
    stateModeCombo.addItem ("StartledIntro", 6);
    stateModeCombo.addItem ("Falling", 7);
    stateModeCombo.addItem ("LandingFall", 8);
    stateModeCombo.addItem ("Drowsy", 9);
    stateModeCombo.addItem ("Sleeping", 10);
    stateModeCombo.addItem ("Eating", 11);
    stateModeCombo.addItem ("Hungry", 12);
    stateModeCombo.addItem ("Starving", 13);
    stateModeCombo.addItem ("Sick", 14);
    stateModeCombo.addItem ("CriticalSick", 15);
    stateModeCombo.addItem ("Dead", 16);
    stateModeCombo.setSelectedId (1, juce::dontSendNotification);
    stateModeCombo.onChange = [this]
    {
        const int sel = stateModeCombo.getSelectedId();
        forceMotionModeEnabled = (sel != 1);

        switch (sel)
        {
            case 2: forcedMotionMode = MotionMode::egg; break;
            case 3: forcedMotionMode = MotionMode::hatching; break;
            case 4: forcedMotionMode = MotionMode::toilet; break;
            case 5: forcedMotionMode = MotionMode::patrol; break;
            case 6: forcedMotionMode = MotionMode::startledIntro; break;
            case 7: forcedMotionMode = MotionMode::falling; break;
            case 8: forcedMotionMode = MotionMode::landingFall; break;
            case 9: forcedMotionMode = MotionMode::drowsy; break;
            case 10: forcedMotionMode = MotionMode::sleeping; break;
            case 11: forcedMotionMode = MotionMode::eating; break;
            case 12: forcedMotionMode = MotionMode::hungry; break;
            case 13: forcedMotionMode = MotionMode::starving; break;
            case 14: forcedMotionMode = MotionMode::sick; break;
            case 15: forcedMotionMode = MotionMode::criticalSick; break;
            case 16: forcedMotionMode = MotionMode::dead; break;
            default: break;
        }

        applyForcedMotionMode();
        refreshDebugAnimTriggerItems();
    };
    addAndMakeVisible (stateModeCombo);
    stateModeCombo.setVisible (false);
    stateModeCombo.setEnabled (false);

    animTriggerCombo.setTextWhenNothingSelected ("Trigger animation...");
    animTriggerCombo.onChange = [this]
    {
        const int sel = animTriggerCombo.getSelectedId();
        if (sel > 0)
            triggerDebugAnimationById (sel);

        animTriggerCombo.setSelectedId (0, juce::dontSendNotification);
    };
    addAndMakeVisible (animTriggerCombo);
    animTriggerCombo.setVisible (false);
    animTriggerCombo.setEnabled (false);

    loadRandomRoleAnimations();
    refreshDebugAnimTriggerItems();

    currentVisualHz = getTargetVisualHzForMode (motionMode);
    currentTickDtSec = 1.0f / (float) juce::jmax (1, currentVisualHz);
    startTimerHz (currentVisualHz);
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

    stateModeCombo.setVisible (false);
    animTriggerCombo.setVisible (false);
    repaintSelfAndParent();
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

        const bool shouldMirrorPatrol = (motionMode == MotionMode::patrol)
                                     && patrolMirrorX
                                     && ! hasRightVariantFrames (currentAnimId);
        if (shouldMirrorPatrol)

        {
            const auto t = juce::AffineTransform::scale (-1.0f, 1.0f)
                               .translated (x + (float) frame.getWidth(), y);
            g.drawImageTransformed (frame, t, false);
        }

        else
        {
            g.drawImageAt (frame, (int) x, (int) y);
        }

        if (motionMode == MotionMode::patrol && showSpeechBubble && currentSpeechText.isNotEmpty())
        {

            const auto bubbleFont = PinkXP::getFont (8.5f, juce::Font::plain);
            const int bubblePaddingX = 6;
            const int bubblePaddingY = 3;
            const int bubbleMinW = 36;
            const int bubbleMaxW = juce::jmax (bubbleMinW, getWidth() - 6);
            const int maxTextW = juce::jmax (16, bubbleMaxW - bubblePaddingX * 2);
            const int singleLineW = juce::jmax (16, bubbleFont.getStringWidth (currentSpeechText));
            const int textW = juce::jlimit (16, maxTextW, singleLineW);

            juce::AttributedString bubbleText;
            bubbleText.setJustification (juce::Justification::centred);
            bubbleText.setWordWrap (juce::AttributedString::WordWrap::byWord);
            bubbleText.append (currentSpeechText, bubbleFont, PinkXP::ink);

            juce::TextLayout textLayout;
            textLayout.createLayout (bubbleText, (float) textW);

            const int textH = juce::jmax ((int) std::ceil (bubbleFont.getHeight()), (int) std::ceil (textLayout.getHeight()));
            const int bubbleW = juce::jlimit (bubbleMinW, bubbleMaxW, textW + bubblePaddingX * 2);
            const int bubbleH = juce::jmax (14, textH + bubblePaddingY * 2);

            auto bubble = juce::Rectangle<int> ((int) x + frame.getWidth() / 2 - bubbleW / 2,
                                                (int) y - bubbleH - 4,
                                                bubbleW,
                                                bubbleH);

            bubble = bubble.withX (juce::jlimit (0, juce::jmax (0, getWidth() - bubble.getWidth()), bubble.getX()));
            bubble = bubble.withY (juce::jlimit (0, juce::jmax (0, getHeight() - bubble.getHeight()), bubble.getY()));

            g.setColour (juce::Colours::white.withAlpha (0.92f));
            g.fillRoundedRectangle (bubble.toFloat(), 3.0f);
            g.setColour (PinkXP::ink.withAlpha (0.85f));
            g.drawRoundedRectangle (bubble.toFloat(), 3.0f, 1.0f);

            textLayout.draw (g, bubble.reduced (bubblePaddingX, bubblePaddingY).toFloat());
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

    stateModeCombo.setBounds (getStateModeComboBounds());
    animTriggerCombo.setBounds (getAnimTriggerComboBounds());
    stateModeCombo.setVisible (false);
    animTriggerCombo.setVisible (false);

    const auto now = getLocalBounds();

    const bool grewByHeightOnly = (! lastLocalBounds.isEmpty())
                               && (now.getHeight() > lastLocalBounds.getHeight());

    const bool isVerticalResizeDragging = (dragMode == DragMode::resize)
                                       && (resizeEdge == Edge::bottom || resizeEdge == Edge::bottomRight);

    const auto frame = getCurrentFrame();
    if (frame.isNull())
    {
        lastLocalBounds = now;
        return;
    }

    const auto playArea = now.withTrimmedTop (juce::jmin (hudHeight, now.getHeight()));
    const float minX = 0.0f;
    const float maxX = (float) juce::jmax (0, playArea.getWidth() - frame.getWidth());
    const float floorY = (float) juce::jmax (0, now.getHeight() - frame.getHeight());
    const float halfW = (float) frame.getWidth() * 0.5f;
    const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
    const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);

    if (! hasPetPosition)
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, (anchorMin + anchorMax) * 0.5f);
        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = floorY;
        hasPetPosition = true;
    }

    else
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = juce::jlimit (0.0f, floorY, petPos.y);

        if (motionMode == MotionMode::patrol)
            petPos.y = floorY;

    }

    if (motionMode == MotionMode::egg || motionMode == MotionMode::hatching)
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, (anchorMin + anchorMax) * 0.5f);
        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = floorY;
    }

    if (isVerticalResizeDragging
        && grewByHeightOnly
        && motionMode != MotionMode::egg
        && motionMode != MotionMode::hatching
        // ---- 修复"拖动高度时动画卡住" ----
        //
        // 原问题：用户持续向下拖动 resize 时，每一次 mouseDrag 都会 setBounds →
        //        resized 被调用。若此处无下面三项保护，则每个 drag tick 都会
        //        switchMotionMode(startledIntro)。
        //        · 第一次拖动时 motionMode 是 patrol → 切到 startledIntro，合理。
        //        · startledIntro 播完会自动过渡到 falling；但若此时用户仍在拖动、
        //          高度又一次增大，resized 再次触发，旧逻辑没有排除 falling，
        //          于是 switchMotionMode 从 falling 切回 startledIntro →
        //          forceAnimation(startled, restartFrame=true) → currentFrameIdx=0。
        //        · 同理，刚切到 landingFall 又会被打回 startledIntro。
        //        · 动画帧节律是 1 秒 1 帧（animFrameIntervalSec=1.0f），若每次 drag
        //          tick（最密集 60Hz）都重置 currentFrameIdx，则角色**永远卡在
        //          startledIntro 的第 0 帧**无法前进 → 视觉上"卡住"。
        //
        // 修复：仅当 motionMode **不在**瞬时状态链（startledIntro/falling/landingFall）
        //       时才触发一次 startledIntro。这样：
        //         · 初次拖动：patrol → startledIntro（正常触发）
        //         · 后续持续拖动：已在 startledIntro/falling/landingFall → 不再打断
        //         · 播完 landingFall 后若仍在拖动且高度又变大 → patrol 结束后再次
        //           触发 startledIntro，形成"每一轮摔倒结束才再摔一次"的合理节奏
        && motionMode != MotionMode::startledIntro
        && motionMode != MotionMode::falling
        && motionMode != MotionMode::landingFall
        && hasAnimation ((int) PetAnim::startled))
    {
        idleTicksRemaining = 0;
        switchMotionMode (MotionMode::startledIntro);
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
        repaintSelfAndParent (getDeleteButtonBounds());
    }

    if (! hoveredDel)
        updateCursorFor (detectEdge (e.getPosition()));
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);

}

void TamagotchiModule::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    deleteBtnHovered = false;
    hoveredTestButton = -1;
    if (dragMode == DragMode::none)
        setMouseCursor (juce::MouseCursor::NormalCursor);
    repaintSelfAndParent (getDeleteButtonBounds());
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
        repaintSelfAndParent (getDeleteButtonBounds());
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
    repaintSelfAndParent();
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
        repaintSelfAndParent (getDeleteButtonBounds());

        if (stillOnDel && onCloseClicked)
            onCloseClicked (*this);
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
    const int targetHz = getTargetVisualHzForMode (forceMotionModeEnabled ? forcedMotionMode
                                                                            : evaluateAutoMotionMode());

    if (targetHz != currentVisualHz)
    {
        currentVisualHz = targetHz;
        currentTickDtSec = 1.0f / (float) juce::jmax (1, currentVisualHz);
        startTimerHz (currentVisualHz);

        if (currentVisualHz >= ticksPerSecond)
            flushVisualRepaintQueue (true);
    }

    updateNeeds();

    if (forceMotionModeEnabled)
        switchMotionMode (forcedMotionMode);
    else
        switchMotionMode (evaluateAutoMotionMode());

    stepWander();

    frameAccumSec += currentTickDtSec;
    constexpr float animFrameIntervalSec = 1.0f;
    if (frameAccumSec >= animFrameIntervalSec)
    {
        frameAccumSec = std::fmod (frameAccumSec, animFrameIntervalSec);
        stepOneFrame();
    }

    flushVisualRepaintQueue (false);
}

bool TamagotchiModule::loadRandomRoleAnimations()
{
    for (auto& arr : animFrames)
        arr.clearQuick();
    for (auto& arr : animFramesRight)
        arr.clearQuick();
    availableAnimIds.clearQuick();

    currentAnimId = 1;
    currentFrameIdx = 0;
    hasPetPosition = false;
    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;

    const bool hasEggFrames = loadRandomEggFrames (eggFrames, eggStyleId);

    const auto root = findTamagotchiAssetsRoot();
    const auto mirrorRoot = findTamagotchiMirrorAssetsRoot();

    if (root.isDirectory())

    {
        juce::Array<juce::File> roleDirs;
        root.findChildFiles (roleDirs, juce::File::findDirectories, false);
        if (! roleDirs.isEmpty())
        {
            auto chosenRoleDir = roleDirs.getReference (
                juce::Random::getSystemRandom().nextInt (roleDirs.size()));

            if (loadRoleFramesFromDirectory (chosenRoleDir, mirrorRoot, animFrames, animFramesRight, availableAnimIds))

            {
                roleName = chosenRoleDir.getFileName();
                motionMode = hasEggFrames ? MotionMode::egg : MotionMode::patrol;
                if (! hasEggFrames)
                    beginPatrolCycle();
                hasPetPosition = false;
                repaintSelfAndParent();
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
    for (auto& arr : animFramesRight)
        arr.clearQuick();

    availableAnimIds.clearQuick();
    availableAnimIds.add (1);
    motionMode = hasEggFrames ? MotionMode::egg : MotionMode::patrol;
    if (! hasEggFrames)
        beginPatrolCycle();
    hasPetPosition = false;

    repaintSelfAndParent();
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
    const auto oldMode = motionMode;
    const int oldAnimId = currentAnimId;
    const int oldFrameIdx = currentFrameIdx;
    const bool oldBubbleVisible = showSpeechBubble;
    const auto oldBubbleText = currentSpeechText;
    const auto oldFrame = getCurrentFrame();
    const auto oldPetBounds = getPetVisualBoundsFor (oldFrame, petPos, oldBubbleVisible, oldBubbleText, oldMode);

    if (motionMode == MotionMode::egg)
    {
        currentFrameIdx = 0;

        if (oldFrameIdx != currentFrameIdx)
            enqueuePetDirtyRepaint (oldPetBounds.getUnion (getCurrentPetVisualBounds()));
        return;
    }

    if (motionMode == MotionMode::hatching)
    {
        if (eggFrames.size() < 4)
        {
            onAnimationFinished();
        }
        else
        {
            ++currentFrameIdx;
            if (currentFrameIdx >= 4)
                onAnimationFinished();
        }

        const auto newPetBounds = getCurrentPetVisualBounds();
        const bool changed = oldMode != motionMode
                          || oldAnimId != currentAnimId
                          || oldFrameIdx != currentFrameIdx
                          || oldBubbleVisible != showSpeechBubble
                          || oldBubbleText != currentSpeechText
                          || oldPetBounds != newPetBounds;
        if (changed)
            enqueuePetDirtyRepaint (oldPetBounds.getUnion (newPetBounds));
        return;
    }

    if (availableAnimIds.isEmpty())
        return;

    const auto* frames = &getFramesForAnim (currentAnimId);
    const auto* rightFrames = &getRightFramesForAnim (currentAnimId);
    if (shouldUseRightVariantForAnim (currentAnimId) && ! rightFrames->isEmpty())
        frames = rightFrames;

    if (frames->isEmpty())
    {
        chooseNextAnimation();
        frames = &getFramesForAnim (currentAnimId);
        rightFrames = &getRightFramesForAnim (currentAnimId);
        if (shouldUseRightVariantForAnim (currentAnimId) && ! rightFrames->isEmpty())
            frames = rightFrames;
        if (frames->isEmpty())
            return;
    }

    ++currentFrameIdx;
    if (currentFrameIdx >= frames->size())
        onAnimationFinished();

    const auto newPetBounds = getCurrentPetVisualBounds();
    const bool changed = oldMode != motionMode
                      || oldAnimId != currentAnimId
                      || oldFrameIdx != currentFrameIdx
                      || oldBubbleVisible != showSpeechBubble
                      || oldBubbleText != currentSpeechText
                      || oldPetBounds != newPetBounds;
    if (changed)
        enqueuePetDirtyRepaint (oldPetBounds.getUnion (newPetBounds));
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

bool TamagotchiModule::shouldUseRightVariantForAnim (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);

    if (motionMode == MotionMode::patrol)
    {
        if (! patrolMirrorX)
            return false;

        return safeId == (int) PetAnim::lookLeft || safeId == (int) PetAnim::moveLeft;
    }

    if (motionMode == MotionMode::eating)
    {
        if (! eatingMirrorX)
            return false;

        return safeId == (int) PetAnim::runLeftToFood
            || safeId == (int) PetAnim::runLeftToHate
            || safeId == (int) PetAnim::runLeftToLike
            || safeId == (int) PetAnim::eatLeft;
    }

    return false;
}

const juce::Array<juce::Image>& TamagotchiModule::getFramesForAnim (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);
    return animFrames[(size_t) (safeId - 1)];
}

const juce::Array<juce::Image>& TamagotchiModule::getRightFramesForAnim (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);
    return animFramesRight[(size_t) (safeId - 1)];
}

bool TamagotchiModule::hasRightVariantFrames (int animId) const
{
    const auto& rightFrames = getRightFramesForAnim (animId);
    return ! rightFrames.isEmpty();
}

int TamagotchiModule::chooseNextAnimByState() const

{
    switch (motionMode)
    {
        case MotionMode::egg:
        case MotionMode::hatching:
            return 1;

        case MotionMode::toilet:
            return hasAnimation ((int) PetAnim::toiletSquat)
                ? (int) PetAnim::toiletSquat
                : randomAnimFrom ({ (int) PetAnim::wantToToilet, (int) PetAnim::depressed });

        case MotionMode::patrol:
        {
            if (patrolLockedAnimId > 0 && hasAnimation (patrolLockedAnimId))
                return patrolLockedAnimId;

            switch (currentPatrolAction)
            {
                case PatrolAction::lookLeftSlow:
                case PatrolAction::lookRightSlow:
                    return hasAnimation ((int) PetAnim::lookLeft)
                        ? (int) PetAnim::lookLeft
                        : randomAnimFrom ({ (int) PetAnim::lookAround });

                case PatrolAction::moveLeftFast:
                case PatrolAction::moveRightFast:
                    return hasAnimation ((int) PetAnim::moveLeft)
                        ? (int) PetAnim::moveLeft
                        : randomAnimFrom ({ (int) PetAnim::dazeThenWalkLeft });

                case PatrolAction::talk:
                    return randomAnimFrom ({ (int) PetAnim::talkHappier,
                                             (int) PetAnim::talkHappiest,
                                             (int) PetAnim::talkShy });

                case PatrolAction::jumpFight:
                    return hasAnimation ((int) PetAnim::fight)
                        ? (int) PetAnim::fight
                        : randomAnimFrom ({ (int) PetAnim::moveLeft });
            }
            break;
        }

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

        case MotionMode::drowsy:
            return hasAnimation ((int) PetAnim::wantToEat)
                ? (int) PetAnim::wantToEat
                : randomAnimFrom ({ (int) PetAnim::depressed, (int) PetAnim::lookAround });

        case MotionMode::sleeping:
            return hasAnimation ((int) PetAnim::sleep)
                ? (int) PetAnim::sleep
                : randomAnimFrom ({ (int) PetAnim::dazeThenWalkLeft, (int) PetAnim::lookAround });

        case MotionMode::eating:
            return randomAnimFrom ({ (int) PetAnim::runLeftToFood,
                                     (int) PetAnim::runLeftToHate,
                                     (int) PetAnim::runLeftToLike,
                                     (int) PetAnim::eatLeft });

        case MotionMode::hungry:
            return hasAnimation ((int) PetAnim::depressed)
                ? (int) PetAnim::depressed
                : randomAnimFrom ({ (int) PetAnim::wantToEat, (int) PetAnim::cry });

        case MotionMode::starving:
            return hasAnimation ((int) PetAnim::talkVeryNegative)
                ? (int) PetAnim::talkVeryNegative
                : randomAnimFrom ({ (int) PetAnim::depressed, (int) PetAnim::cry });

        case MotionMode::sick:
            return hasAnimation ((int) PetAnim::almostSick)
                ? (int) PetAnim::almostSick
                : randomAnimFrom ({ (int) PetAnim::sick, (int) PetAnim::nearDeath });

        case MotionMode::criticalSick:
            return hasAnimation ((int) PetAnim::sick)
                ? (int) PetAnim::sick
                : randomAnimFrom ({ (int) PetAnim::almostSick, (int) PetAnim::nearDeath });

        case MotionMode::dead:
            return hasAnimation ((int) PetAnim::death)
                ? (int) PetAnim::death
                : randomAnimFrom ({ (int) PetAnim::nearDeath, (int) PetAnim::sick });
    }

    return randomAnimFrom ({ (int) PetAnim::lookAround });
}

void TamagotchiModule::onAnimationFinished()
{
    currentFrameIdx = 0;

    switch (motionMode)
    {
        case MotionMode::egg:
            currentFrameIdx = 0;
            break;

        case MotionMode::hatching:
            if (! forceMotionModeEnabled)
                switchMotionMode (MotionMode::patrol);
            break;

        case MotionMode::toilet:
            forceAnimation ((int) PetAnim::toiletSquat, true);
            break;

        case MotionMode::patrol:
            chooseNextAnimation();
            break;

        case MotionMode::startledIntro:
            if (! forceMotionModeEnabled)
                switchMotionMode (MotionMode::falling);
            break;

        case MotionMode::falling:
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::landingFall:
            if (! forceMotionModeEnabled)
                switchMotionMode (MotionMode::patrol);
            break;

        case MotionMode::drowsy:
            forceAnimation ((int) PetAnim::wantToEat, true);
            break;

        case MotionMode::sleeping:
            forceAnimation ((int) PetAnim::sleep, true);
            break;

        case MotionMode::eating:
            forceAnimation (eatingCurrentAnimId, true);
            break;

        case MotionMode::hungry:
        case MotionMode::starving:
        case MotionMode::sick:
        case MotionMode::criticalSick:
        case MotionMode::dead:
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
    // 使用当前 tick 时长，保证动态刷新率下需求变化仍按真实时间推进
    const float dt = currentTickDtSec;

    const float growthBaseRate = 1.2f;
    const float growthGainK = 12.0f;
    const float quietDecayRate = 0.8f;
    const float prevHunger = hunger;
    const float prevHealth = health;

    const bool hasSignal = signalLevel01 > 0.02f;

    if (hasSignal)
        silentTicks = 0;
    else
        silentTicks = juce::jmin (sleepSilentTicks + ticksPerSecond * 60, silentTicks + 1);

    if (motionMode == MotionMode::eating)
    {
        if (hasSignal)
            eatingLowSignalTicksRemaining = eatingLowSignalHoldTicks;
        else
            eatingLowSignalTicksRemaining = juce::jmax (0, eatingLowSignalTicksRemaining - 1);
    }
    else
    {
        eatingLowSignalTicksRemaining = 0;
    }

    float hungerDelta = 0.0f;
    if (hasSignal)
    {
        hungerDelta = growthBaseRate + growthGainK * signalLevel01;
    }
    else if (silentTicks > hungerDecaySilentDelayTicks)
    {
        hungerDelta = -quietDecayRate;
    }

    hungerDeltaPerSecond = hungerDelta;
    hunger += hungerDelta * dt;

    float healthDelta = 0.0f;
    if (hunger < 40.0f)
        healthDelta -= 0.35f * dt;
    if (hunger > 70.0f)
        healthDelta += 0.5f * dt;
    if (hunger > 90.0f)
        healthDelta += 1.2f * dt;

    health += healthDelta;

    hunger = juce::jlimit (0.0f, 100.0f, hunger);
    health = juce::jlimit (0.0f, 100.0f, health);

    const bool hungerIncreasingThisTick = hunger > prevHunger + 0.0001f;

    if (! hungerRiseFrom50Tracking)
    {
        if (prevHunger <= 50.0f
            && hunger >= 50.0f
            && hunger < 100.0f
            && hungerIncreasingThisTick)
        {
            hungerRiseFrom50Tracking = true;
        }
    }
    else
    {
        if (! hungerIncreasingThisTick || hunger < 50.0f)
        {
            hungerRiseFrom50Tracking = false;
        }
        else if (hunger >= 100.0f)
        {
            hungerRiseFrom50Tracking = false;
            toiletTriggerPending = true;
        }
    }

    if (std::abs (hunger - prevHunger) >= 0.1f || std::abs (health - prevHealth) >= 0.1f)
        repaintSelfAndParent (getHudBounds());
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
    auto btnRow = hud.withTrimmedTop (16).removeFromTop (16).reduced (2, 1);

    const int gap = 2;
    const int totalGap = gap * (testButtonCount - 1);
    const int cellW = juce::jmax (8, (btnRow.getWidth() - totalGap) / testButtonCount);
    const int x = btnRow.getX() + safeIdx * (cellW + gap);
    return { x, btnRow.getY(), cellW, juce::jmax (10, btnRow.getHeight()) };
}

juce::Rectangle<int> TamagotchiModule::getStateModeComboBounds() const
{
    auto hud = getHudBounds();
    auto comboRow = hud.withTrimmedTop (32).removeFromTop (15).reduced (2, 0);
    const int gap = 2;
    const int leftW = juce::jmax (16, (comboRow.getWidth() - gap) / 2);
    return { comboRow.getX(), comboRow.getY(), leftW, comboRow.getHeight() };
}

juce::Rectangle<int> TamagotchiModule::getAnimTriggerComboBounds() const
{
    auto hud = getHudBounds();
    auto comboRow = hud.withTrimmedTop (32).removeFromTop (15).reduced (2, 0);
    const int gap = 2;
    const int leftW = juce::jmax (16, (comboRow.getWidth() - gap) / 2);
    return { comboRow.getX() + leftW + gap, comboRow.getY(), comboRow.getWidth() - leftW - gap, comboRow.getHeight() };
}

void TamagotchiModule::refreshDebugAnimTriggerItems()
{
    animTriggerCombo.clear (juce::dontSendNotification);

    if (forceMotionModeEnabled)
    {
        switch (forcedMotionMode)
        {
            case MotionMode::egg:
                animTriggerCombo.addItem ("egg idle frame 1", dbgEggIdle);
                break;

            case MotionMode::hatching:
                animTriggerCombo.addItem ("egg hatch 4 frames", dbgEggHatching);
                break;

            case MotionMode::toilet:
                animTriggerCombo.addItem ("toiletSquat (2)", (int) PetAnim::toiletSquat);
                break;

            case MotionMode::patrol:
                animTriggerCombo.addItem ("1) lookLeft 6s + slow left",  dbgPatrolLookLeftSlow);
                animTriggerCombo.addItem ("2) lookLeft mirrored 6s + slow right", dbgPatrolLookRightSlow);
                animTriggerCombo.addItem ("3) moveLeft 4s + fast left", dbgPatrolMoveLeftFast);
                animTriggerCombo.addItem ("4) moveLeft mirrored 4s + fast right", dbgPatrolMoveRightFast);
                animTriggerCombo.addItem ("5) talk(22/23/24) 6s + bubble", dbgPatrolTalk);
                animTriggerCombo.addItem ("6) fight 2s + two hops", dbgPatrolJumpFight);
                break;

            case MotionMode::startledIntro:
                animTriggerCombo.addItem ("startled (33)", (int) PetAnim::startled);
                animTriggerCombo.addItem ("shocked (25)", (int) PetAnim::shocked);
                break;
            case MotionMode::falling:
                animTriggerCombo.addItem ("startled (33)", (int) PetAnim::startled);
                animTriggerCombo.addItem ("shocked (25)", (int) PetAnim::shocked);
                animTriggerCombo.addItem ("fall (27)", (int) PetAnim::fall);
                break;
            case MotionMode::landingFall:
                animTriggerCombo.addItem ("fall (27)", (int) PetAnim::fall);
                animTriggerCombo.addItem ("shocked (25)", (int) PetAnim::shocked);
                break;

            case MotionMode::drowsy:
                animTriggerCombo.addItem ("wantToEat (3)", (int) PetAnim::wantToEat);
                animTriggerCombo.addItem ("depressed (15)", (int) PetAnim::depressed);
                break;

            case MotionMode::sleeping:
                animTriggerCombo.addItem ("sleep (4)", (int) PetAnim::sleep);
                animTriggerCombo.addItem ("dazeThenWalkLeft (28)", (int) PetAnim::dazeThenWalkLeft);
                break;

            case MotionMode::eating:
                animTriggerCombo.addItem ("runLeftToFood (10)", (int) PetAnim::runLeftToFood);
                animTriggerCombo.addItem ("runLeftToHate (11)", (int) PetAnim::runLeftToHate);
                animTriggerCombo.addItem ("runLeftToLike (12)", (int) PetAnim::runLeftToLike);
                animTriggerCombo.addItem ("eatLeft (13)", (int) PetAnim::eatLeft);
                break;

            case MotionMode::hungry:
                animTriggerCombo.addItem ("depressed (15)", (int) PetAnim::depressed);
                animTriggerCombo.addItem ("wantToEat (3)", (int) PetAnim::wantToEat);
                break;

            case MotionMode::starving:
                animTriggerCombo.addItem ("talkVeryNegative (21)", (int) PetAnim::talkVeryNegative);
                animTriggerCombo.addItem ("depressed (15)", (int) PetAnim::depressed);
                break;

            case MotionMode::sick:
                animTriggerCombo.addItem ("almostSick (5)", (int) PetAnim::almostSick);
                animTriggerCombo.addItem ("sick (6)", (int) PetAnim::sick);
                break;

            case MotionMode::criticalSick:
                animTriggerCombo.addItem ("sick (6)", (int) PetAnim::sick);
                animTriggerCombo.addItem ("nearDeath (7)", (int) PetAnim::nearDeath);
                break;

            case MotionMode::dead:
                animTriggerCombo.addItem ("death (9)", (int) PetAnim::death);
                animTriggerCombo.addItem ("nearDeath (7)", (int) PetAnim::nearDeath);
                break;
        }
    }
    else

    {
        animTriggerCombo.addItem ("lookAround (16)", (int) PetAnim::lookAround);
        animTriggerCombo.addItem ("startled (33)", (int) PetAnim::startled);
        animTriggerCombo.addItem ("fall (27)", (int) PetAnim::fall);
        animTriggerCombo.addItem ("moveLeft (30)", (int) PetAnim::moveLeft);
        animTriggerCombo.addItem ("talkHappy (19)", (int) PetAnim::talkHappy);
    }

    animTriggerCombo.setSelectedId (0, juce::dontSendNotification);
}

TamagotchiModule::MotionMode TamagotchiModule::evaluateAutoMotionMode() const
{
    // egg/hatching 启动流程：蛋阶段等待音频，孵化阶段必须完整播放
    if (motionMode == MotionMode::egg)
        return signalLevel01 > 0.02f ? MotionMode::hatching : MotionMode::egg;

    if (motionMode == MotionMode::hatching)
        return MotionMode::hatching;

    // 最高优先级：toilet（触发后持续 6 秒）
    if (motionMode == MotionMode::toilet && toiletTicksRemaining > 0)
        return MotionMode::toilet;

    if (toiletTriggerPending)
        return MotionMode::toilet;

    // 瞬时状态链优先：由边界拖拽触发后，必须完整经历 惊吓 -> 下落 -> 摔倒
    if (motionMode == MotionMode::startledIntro
        || motionMode == MotionMode::falling
        || motionMode == MotionMode::landingFall)
        return motionMode;

    // 需求状态优先级：死亡 > 吃饭(含低信号保持) > 重病 > 生病 > 睡觉 > 困倦 > 极度饥饿 > 饥饿 > 巡逻
    if (health <= 0.0f)
        return MotionMode::dead;

    if (motionMode == MotionMode::eating
        && signalLevel01 <= 0.02f
        && eatingLowSignalTicksRemaining > 0)
        return MotionMode::eating;

    if (hunger < 100.0f && hungerDeltaPerSecond > 0.0f)
        return MotionMode::eating;

    if (health < 25.0f)
        return MotionMode::criticalSick;

    if (health < 50.0f)
        return MotionMode::sick;

    if (silentTicks >= sleepSilentTicks)
        return MotionMode::sleeping;

    if (silentTicks >= drowsySilentTicks)
        return MotionMode::drowsy;

    if (hunger <= 0.0f)
        return MotionMode::starving;

    if (hunger < 40.0f)
        return MotionMode::hungry;

    return MotionMode::patrol;
}

void TamagotchiModule::switchMotionMode (MotionMode newMode)
{
    if (motionMode == newMode)
        return;

    const auto oldMode = motionMode;
    const bool oldBubbleVisible = showSpeechBubble;
    const auto oldBubbleText = currentSpeechText;
    const auto oldFrame = getCurrentFrame();
    const auto oldPetBounds = getPetVisualBoundsFor (oldFrame, petPos, oldBubbleVisible, oldBubbleText, oldMode);

    motionMode = newMode;

    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;
    patrolMirrorX = false;
    eatingMirrorX = false;
    eatingLowSignalTicksRemaining = 0;

    switch (motionMode)

    {
        case MotionMode::egg:
            currentFrameIdx = 0;
            break;

        case MotionMode::hatching:
            currentFrameIdx = 0;
            break;

        case MotionMode::toilet:
            toiletTriggerPending = false;
            hungerRiseFrom50Tracking = false;
            toiletTicksRemaining = toiletDurationTicks;
            forceAnimation ((int) PetAnim::toiletSquat);
            break;

        case MotionMode::patrol:
            beginPatrolCycle();
            break;

        case MotionMode::startledIntro:
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::falling:
            fallVelocityPxPerTick = 0.0f;
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::landingFall:
            forceAnimation ((int) PetAnim::fall);
            break;

        case MotionMode::drowsy:
            forceAnimation ((int) PetAnim::wantToEat);
            break;

        case MotionMode::sleeping:
            forceAnimation ((int) PetAnim::sleep);
            break;

        case MotionMode::eating:
        {
            eatingCurrentAnimId = randomAnimFrom ({ (int) PetAnim::runLeftToFood,
                                                    (int) PetAnim::runLeftToHate,
                                                    (int) PetAnim::runLeftToLike,
                                                    (int) PetAnim::eatLeft });
            eatingAnimTicksRemaining = eatingSwitchTicks;
            eatingMoveDir = juce::Random::getSystemRandom().nextBool() ? 1.0f : -1.0f;
            eatingMirrorX = (eatingMoveDir > 0.0f);
            eatingLowSignalTicksRemaining = eatingLowSignalHoldTicks;
            eatingMoveRetargetTicksRemaining = juce::Random::getSystemRandom().nextInt (ticksPerSecond * 3 / 2) + ticksPerSecond / 2;
            forceAnimation (eatingCurrentAnimId);
            break;
        }

        case MotionMode::hungry:
            forceAnimation ((int) PetAnim::depressed);
            break;

        case MotionMode::starving:
            forceAnimation ((int) PetAnim::talkVeryNegative);
            break;

        case MotionMode::sick:
            forceAnimation ((int) PetAnim::almostSick);
            break;

        case MotionMode::criticalSick:
            forceAnimation ((int) PetAnim::sick);
            break;

        case MotionMode::dead:
            forceAnimation ((int) PetAnim::death);
            break;
    }

    enqueuePetDirtyRepaint (oldPetBounds.getUnion (getCurrentPetVisualBounds()));
}

void TamagotchiModule::applyForcedMotionMode()
{
    if (! forceMotionModeEnabled)
        return;

    switchMotionMode (forcedMotionMode);
    flushVisualRepaintQueue (true);
}

void TamagotchiModule::triggerDebugAnimationById (int triggerId)
{
    if (forceMotionModeEnabled && forcedMotionMode == MotionMode::patrol)
    {
        switch (triggerId)
        {
            case dbgPatrolLookLeftSlow:  beginPatrolAction (PatrolAction::lookLeftSlow);  return;
            case dbgPatrolLookRightSlow: beginPatrolAction (PatrolAction::lookRightSlow); return;
            case dbgPatrolMoveLeftFast:  beginPatrolAction (PatrolAction::moveLeftFast);  return;
            case dbgPatrolMoveRightFast: beginPatrolAction (PatrolAction::moveRightFast); return;
            case dbgPatrolTalk:          beginPatrolAction (PatrolAction::talk);          return;
            case dbgPatrolJumpFight:     beginPatrolAction (PatrolAction::jumpFight);     return;
            default: break;
        }
    }

    if (forceMotionModeEnabled)
    {
        if (forcedMotionMode == MotionMode::egg && triggerId == dbgEggIdle)
        {
            currentFrameIdx = 0;
            enqueuePetDirtyRepaint (getCurrentPetVisualBounds());
            flushVisualRepaintQueue (true);
            return;
        }

        if (forcedMotionMode == MotionMode::hatching && triggerId == dbgEggHatching)
        {
            currentFrameIdx = 0;
            enqueuePetDirtyRepaint (getCurrentPetVisualBounds());
            flushVisualRepaintQueue (true);
            return;
        }
    }

    const int safeId = juce::jlimit (1, 33, triggerId);
    if (! hasAnimation (safeId))
        return;

    const auto oldPetBounds = getCurrentPetVisualBounds();

    if (safeId == (int) PetAnim::fight)

    {
        jumpFightActive = true;
        jumpFightTick = 0;
        jumpFightBaseY = petPos.y;
    }

    if (motionMode == MotionMode::patrol
        && (safeId == (int) PetAnim::talkHappier
            || safeId == (int) PetAnim::talkHappiest
            || safeId == (int) PetAnim::talkShy
            || safeId == (int) PetAnim::talkHappy
            || safeId == (int) PetAnim::talkNormal
            || safeId == (int) PetAnim::talkNegative
            || safeId == (int) PetAnim::talkVeryNegative))
    {
        showSpeechBubble = true;
        currentSpeechText = randomIdleSpeech();
    }
    else
    {
        showSpeechBubble = false;
        currentSpeechText.clear();
    }

    forceAnimation (safeId, true);
    enqueuePetDirtyRepaint (oldPetBounds.getUnion (getCurrentPetVisualBounds()));
    flushVisualRepaintQueue (true);
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
    repaintSelfAndParent (getHudBounds());
}

juce::Image TamagotchiModule::getCurrentFrame() const

{
    if (motionMode == MotionMode::egg || motionMode == MotionMode::hatching)
    {
        if (eggFrames.isEmpty())
            return {};

        const int maxIdx = juce::jmax (0, eggFrames.size() - 1);
        const int frameIdx = (motionMode == MotionMode::egg)
            ? 0
            : juce::jlimit (0, maxIdx, currentFrameIdx);
        return eggFrames.getReference (frameIdx);
    }

    const auto& baseFrames = getFramesForAnim (currentAnimId);
    const auto& rightFrames = getRightFramesForAnim (currentAnimId);
    const bool useRight = shouldUseRightVariantForAnim (currentAnimId) && ! rightFrames.isEmpty();
    const auto& frames = useRight ? rightFrames : baseFrames;

    if (frames.isEmpty())
        return {};

    const int frameIdx = juce::jlimit (0, frames.size() - 1, currentFrameIdx);
    return frames.getReference (frameIdx);
}

juce::Rectangle<int> TamagotchiModule::getPetVisualBoundsFor (const juce::Image& frame,
                                                              juce::Point<float> pos,
                                                              bool bubbleVisible,
                                                              const juce::String& bubbleText,
                                                              MotionMode mode) const
{
    if (frame.isNull())
        return {};

    auto bounds = juce::Rectangle<int> ((int) std::floor (pos.x),
                                        (int) std::floor (pos.y),
                                        frame.getWidth(),
                                        frame.getHeight());

    if (mode == MotionMode::patrol && bubbleVisible && bubbleText.isNotEmpty())
    {
        const auto bubbleFont = PinkXP::getFont (8.5f, juce::Font::plain);
        const int bubblePaddingX = 6;
        const int bubblePaddingY = 3;
        const int bubbleMinW = 36;
        const int bubbleMaxW = juce::jmax (bubbleMinW, getWidth() - 6);
        const int maxTextW = juce::jmax (16, bubbleMaxW - bubblePaddingX * 2);
        const int singleLineW = juce::jmax (16, bubbleFont.getStringWidth (bubbleText));
        const int textW = juce::jlimit (16, maxTextW, singleLineW);

        juce::AttributedString bubbleTextLayout;
        bubbleTextLayout.setJustification (juce::Justification::centred);
        bubbleTextLayout.setWordWrap (juce::AttributedString::WordWrap::byWord);
        bubbleTextLayout.append (bubbleText, bubbleFont, PinkXP::ink);

        juce::TextLayout textLayout;
        textLayout.createLayout (bubbleTextLayout, (float) textW);

        const int textH = juce::jmax ((int) std::ceil (bubbleFont.getHeight()), (int) std::ceil (textLayout.getHeight()));
        const int bubbleW = juce::jlimit (bubbleMinW, bubbleMaxW, textW + bubblePaddingX * 2);
        const int bubbleH = juce::jmax (14, textH + bubblePaddingY * 2);

        auto bubble = juce::Rectangle<int> ((int) std::round (pos.x) + frame.getWidth() / 2 - bubbleW / 2,
                                            (int) std::round (pos.y) - bubbleH - 4,
                                            bubbleW,
                                            bubbleH);
        bubble = bubble.withX (juce::jlimit (0, juce::jmax (0, getWidth() - bubble.getWidth()), bubble.getX()));
        bubble = bubble.withY (juce::jlimit (0, juce::jmax (0, getHeight() - bubble.getHeight()), bubble.getY()));

        bounds = bounds.getUnion (bubble);
    }

    return bounds.getIntersection (getLocalBounds());
}

juce::Rectangle<int> TamagotchiModule::getCurrentPetVisualBounds() const
{
    return getPetVisualBoundsFor (getCurrentFrame(), petPos, showSpeechBubble, currentSpeechText, motionMode);
}

int TamagotchiModule::getTargetVisualHzForMode (MotionMode mode) const noexcept
{
    switch (mode)
    {
        case MotionMode::patrol:
        case MotionMode::startledIntro:
        case MotionMode::falling:
        case MotionMode::landingFall:
        case MotionMode::hatching:
        case MotionMode::eating:
            return 20;

        case MotionMode::drowsy:
        case MotionMode::hungry:
        case MotionMode::starving:
        case MotionMode::sick:
        case MotionMode::criticalSick:
        case MotionMode::toilet:
            return 10;

        case MotionMode::egg:
        case MotionMode::sleeping:
        case MotionMode::dead:
            return 5;
    }

    return 10;
}

void TamagotchiModule::enqueuePetDirtyRepaint (juce::Rectangle<int> area)
{
    area = area.getIntersection (getLocalBounds());
    if (area.isEmpty())
        return;

    if (hasQueuedPetDirty)
        queuedPetDirtyBounds = queuedPetDirtyBounds.getUnion (area);
    else
        queuedPetDirtyBounds = area;

    hasQueuedPetDirty = true;
}

// ---- non-opaque 正确重绘：先让父组件 repaint 对应区域，再 repaint 本组件 ----
// 组件是 non-opaque（背景需要透出 workspace 半透明 content + 桌面），
// 必须让父组件先把底色画一遍再把本组件叠上去，否则旧像素残留形成拖影或
// focus/delete 的残留。JUCE 对嵌套 non-opaque 链路的 repaint 不会自动追到根，
// 所以在这里显式转换坐标并调父组件 repaint()。
void TamagotchiModule::repaintSelfAndParent (juce::Rectangle<int> localRect)
{
    localRect = localRect.getIntersection (getLocalBounds());
    if (localRect.isEmpty())
        return;

    if (auto* parent = getParentComponent())
    {
        const auto parentRect = localRect + getBounds().getTopLeft();
        parent->repaint (parentRect);
    }

    repaint (localRect);
}

void TamagotchiModule::repaintSelfAndParent()
{
    repaintSelfAndParent (getLocalBounds());
}

void TamagotchiModule::flushVisualRepaintQueue (bool forceNow)
{
    juce::ignoreUnused (forceNow);

    if (! hasQueuedPetDirty)
        return;

    const auto dirty = queuedPetDirtyBounds.getIntersection (getLocalBounds());
    hasQueuedPetDirty = false;
    queuedPetDirtyBounds = {};

    if (! dirty.isEmpty())
        repaintSelfAndParent (dirty);
}

void TamagotchiModule::beginPatrolCycle()

{
    currentPatrolAction = PatrolAction::lookLeftSlow;
    patrolActionTicksRemaining = 0;
    patrolCooldownTicksRemaining = patrolCycleTicks;
    patrolLockedAnimId = 0;
    patrolFaceLeft = true;
    patrolMirrorX = false;
    patrolActionMoveDir = 0.0f;
    patrolActionSpeedPxPerTick = 0.0f;
    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;

    forceAnimation (hasAnimation ((int) PetAnim::dazeThenWalkLeft)
                        ? (int) PetAnim::dazeThenWalkLeft
                        : randomAnimFrom ({ (int) PetAnim::lookLeft, (int) PetAnim::moveLeft }));
}

void TamagotchiModule::beginPatrolAction (PatrolAction action)
{
    currentPatrolAction = action;
    patrolCooldownTicksRemaining = 0;
    patrolLockedAnimId = 0;
    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;

    switch (currentPatrolAction)
    {
        case PatrolAction::lookLeftSlow:
            patrolActionTicksRemaining = patrolCycleTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = -1.0f;
            patrolActionSpeedPxPerTick = 0.45f;
            patrolLockedAnimId = (int) PetAnim::lookLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::lookRightSlow:
            patrolActionTicksRemaining = patrolCycleTicks;
            patrolFaceLeft = false;
            patrolMirrorX = true;
            patrolActionMoveDir = 1.0f;
            patrolActionSpeedPxPerTick = 0.45f;
            patrolLockedAnimId = (int) PetAnim::lookLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::moveLeftFast:
            patrolActionTicksRemaining = patrolFastMoveTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = -1.0f;
            patrolActionSpeedPxPerTick = 1.25f;
            patrolLockedAnimId = (int) PetAnim::moveLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::moveRightFast:
            patrolActionTicksRemaining = patrolFastMoveTicks;
            patrolFaceLeft = false;
            patrolMirrorX = true;
            patrolActionMoveDir = 1.0f;
            patrolActionSpeedPxPerTick = 1.25f;
            patrolLockedAnimId = (int) PetAnim::moveLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::talk:
            patrolActionTicksRemaining = patrolCycleTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = 0.0f;
            patrolActionSpeedPxPerTick = 0.0f;
            showSpeechBubble = true;
            currentSpeechText = randomIdleSpeech();
            patrolLockedAnimId = randomAnimFrom ({ (int) PetAnim::talkHappier,
                                                   (int) PetAnim::talkHappiest,
                                                   (int) PetAnim::talkShy });
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::jumpFight:
            patrolActionTicksRemaining = jumpFightTotalTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = 0.0f;
            patrolActionSpeedPxPerTick = 0.0f;
            jumpFightActive = true;
            jumpFightTick = 0;
            jumpFightBaseY = petPos.y;
            patrolLockedAnimId = (int) PetAnim::fight;
            forceAnimation (patrolLockedAnimId);
            break;
    }
}

void TamagotchiModule::stepJumpFight()
{
    if (! jumpFightActive)
        return;

    const float progress = (float) jumpFightTick / (float) juce::jmax (1, jumpFightTotalTicks - 1);
    const float phase = progress * juce::MathConstants<float>::twoPi * (float) jumpFightCount;
    const float hop = juce::jmax (0.0f, std::sin (phase));
    petPos.y = jumpFightBaseY - hop * jumpFightAmplitudePx;

    ++jumpFightTick;
    if (jumpFightTick >= jumpFightTotalTicks)
    {
        jumpFightActive = false;
        petPos.y = jumpFightBaseY;
    }
}

void TamagotchiModule::stepPatrolAction()
{
    if (motionMode != MotionMode::patrol)
        return;

    const bool allowMovement = (! focused) || forceMotionModeEnabled;

    auto finishCurrentAction = [this]()
    {
        patrolActionTicksRemaining = 0;
        patrolCooldownTicksRemaining = patrolCycleTicks;
        patrolMirrorX = false;
        patrolActionMoveDir = 0.0f;
        patrolActionSpeedPxPerTick = 0.0f;
        jumpFightActive = false;

        patrolLockedAnimId = hasAnimation ((int) PetAnim::dazeThenWalkLeft)
                                 ? (int) PetAnim::dazeThenWalkLeft
                                 : 0;
        if (showSpeechBubble)
        {
            showSpeechBubble = false;
            currentSpeechText.clear();
        }
        if (patrolLockedAnimId > 0)
            forceAnimation (patrolLockedAnimId);
    };

    if (patrolActionTicksRemaining > 0)
    {
        if (allowMovement)
            petGroundAnchorX += patrolActionMoveDir * patrolActionSpeedPxPerTick;

        if (jumpFightActive)
            stepJumpFight();

        const bool isEdgeStopAction = currentPatrolAction == PatrolAction::lookLeftSlow
                                   || currentPatrolAction == PatrolAction::lookRightSlow
                                   || currentPatrolAction == PatrolAction::moveLeftFast
                                   || currentPatrolAction == PatrolAction::moveRightFast;

        if (isEdgeStopAction)
        {
            const auto frame = getCurrentFrame();
            if (! frame.isNull())
            {
                const auto area = getLocalBounds();
                const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
                const float halfW = (float) frame.getWidth() * 0.5f;
                const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
                const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);

                petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

                const bool hitLeftEdge = petGroundAnchorX <= anchorMin + 0.01f;
                const bool hitRightEdge = petGroundAnchorX >= anchorMax - 0.01f;
                if (hitLeftEdge || hitRightEdge)
                {
                    finishCurrentAction();
                    return;
                }
            }
        }

        --patrolActionTicksRemaining;
        if (patrolActionTicksRemaining <= 0)
            finishCurrentAction();

        return;
    }

    if (patrolCooldownTicksRemaining > 0)
    {
        --patrolCooldownTicksRemaining;
        if (patrolCooldownTicksRemaining > 0)
            return;
    }

    juce::Array<PatrolAction> candidates;

    const auto area = getLocalBounds();
    const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
    const float playCenterX = (float) playArea.getWidth() * 0.5f;

    if (petGroundAnchorX <= playCenterX)
    {
        // 在左侧：不随机到行为1和3（向左）
        candidates.add (PatrolAction::lookRightSlow);
        candidates.add (PatrolAction::moveRightFast);
    }
    else
    {
        // 在右侧：不随机到行为2和4（向右）
        candidates.add (PatrolAction::lookLeftSlow);
        candidates.add (PatrolAction::moveLeftFast);
    }

    candidates.add (PatrolAction::talk);
    candidates.add (PatrolAction::jumpFight);

    beginPatrolAction (candidates.getReference (juce::Random::getSystemRandom().nextInt (candidates.size())));
}

juce::String TamagotchiModule::getRoleName() const noexcept
{
    return roleName;
}

float TamagotchiModule::getHunger() const noexcept
{
    return hunger;
}

float TamagotchiModule::getHealth() const noexcept
{
    return health;
}

void TamagotchiModule::restorePersistentState (const juce::String& savedRoleName,
                                               float savedHunger,
                                               float savedHealth)
{
    hunger = juce::jlimit (0.0f, 100.0f, savedHunger);
    health = juce::jlimit (0.0f, 100.0f, savedHealth);

    if (savedRoleName.isEmpty())
    {
        // 新建模块：保留构造阶段已初始化的随机角色与蛋状态，仅恢复数值
        repaintSelfAndParent();
        return;
    }

    const auto root = findTamagotchiAssetsRoot();
    const auto mirrorRoot = findTamagotchiMirrorAssetsRoot();
    if (root.isDirectory())
    {
        juce::Array<juce::File> roleDirs;
        root.findChildFiles (roleDirs, juce::File::findDirectories, false);

        for (const auto& dir : roleDirs)
        {
            if (dir.getFileName() == savedRoleName)
            {
                if (loadRoleFramesFromDirectory (dir, mirrorRoot, animFrames, animFramesRight, availableAnimIds))

                {

                    roleName = savedRoleName;
                    currentAnimId = 1;
                    currentFrameIdx = 0;
                    hasPetPosition = false;
                    motionMode = MotionMode::patrol;
                    beginPatrolCycle();
                    repaintSelfAndParent();
                    return;
                }
                break;
            }
        }
    }

    // 如果找不到历史角色，保留当前随机角色，仅恢复数值
    beginPatrolCycle();
    repaintSelfAndParent();
}

void TamagotchiModule::stepWander()

{
    const auto oldMode = motionMode;
    const int oldAnimId = currentAnimId;
    const int oldFrameIdx = currentFrameIdx;
    const auto oldPos = petPos;
    const bool oldBubbleVisible = showSpeechBubble;
    const auto oldBubbleText = currentSpeechText;
    const auto oldFrame = getCurrentFrame();
    const auto oldPetBounds = getPetVisualBoundsFor (oldFrame, oldPos, oldBubbleVisible, oldBubbleText, oldMode);

    const auto frame = getCurrentFrame();
    if (frame.isNull())
        return;

    const auto area = getLocalBounds();
    const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
    const float halfW = (float) frame.getWidth() * 0.5f;
    const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
    const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);
    const float floorY = (float) juce::jmax (0, area.getHeight() - frame.getHeight());

    if (! hasPetPosition)
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, (anchorMin + anchorMax) * 0.5f);
        petPos = { juce::jlimit (0.0f, (float) juce::jmax (0, playArea.getWidth() - frame.getWidth()), petGroundAnchorX - halfW), floorY };
        jumpFightBaseY = floorY;
        hasPetPosition = true;
    }

    switch (motionMode)
    {
        case MotionMode::egg:
        case MotionMode::hatching:
            petPos.y = floorY;
            break;

        case MotionMode::toilet:
            petPos.y = floorY;
            toiletTicksRemaining = juce::jmax (0, toiletTicksRemaining - 1);
            break;

        case MotionMode::patrol:
        {
            if (! jumpFightActive)
                petPos.y = floorY;

            stepPatrolAction();
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
                switchMotionMode (MotionMode::landingFall);
            }

            break;
        }

        case MotionMode::landingFall:
            petPos.y = floorY;
            break;

        case MotionMode::drowsy:
        case MotionMode::sleeping:
            petPos.y = floorY;
            break;

        case MotionMode::eating:
        {
            petPos.y = floorY;

            if (--eatingAnimTicksRemaining <= 0)
            {
                const int oldEatingAnimId = eatingCurrentAnimId;
                for (int attempt = 0; attempt < 4; ++attempt)
                {
                    eatingCurrentAnimId = randomAnimFrom ({ (int) PetAnim::runLeftToFood,
                                                            (int) PetAnim::runLeftToHate,
                                                            (int) PetAnim::runLeftToLike,
                                                            (int) PetAnim::eatLeft });
                    if (eatingCurrentAnimId != oldEatingAnimId)
                        break;
                }

                eatingAnimTicksRemaining = eatingSwitchTicks;
                forceAnimation (eatingCurrentAnimId, true);
            }

            if (--eatingMoveRetargetTicksRemaining <= 0)
            {
                eatingMoveDir = juce::Random::getSystemRandom().nextBool() ? 1.0f : -1.0f;
                eatingMoveRetargetTicksRemaining = juce::Random::getSystemRandom().nextInt (ticksPerSecond * 2) + ticksPerSecond / 2;
            }

            eatingMirrorX = (eatingMoveDir > 0.0f);
            const float eatingSpeedByHealth = juce::jmap (juce::jlimit (0.0f, 100.0f, health),
                                                          0.0f,
                                                          100.0f,
                                                          eatingMoveSpeedPxPerTick * 0.5f,
                                                          eatingMoveSpeedPxPerTick * 1.8f);
            petGroundAnchorX += eatingMoveDir * eatingSpeedByHealth;
            if (petGroundAnchorX <= anchorMin + 0.01f)
            {
                petGroundAnchorX = anchorMin;
                eatingMoveDir = 1.0f;
                eatingMirrorX = true;
            }
            else if (petGroundAnchorX >= anchorMax - 0.01f)
            {
                petGroundAnchorX = anchorMax;
                eatingMoveDir = -1.0f;
                eatingMirrorX = false;
            }
            break;
        }

        case MotionMode::hungry:
        case MotionMode::starving:
        case MotionMode::sick:
        case MotionMode::criticalSick:
        case MotionMode::dead:
            petPos.y = floorY;
            break;
    }

    petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

    const float minX = 0.0f;
    const float maxX = (float) juce::jmax (0, playArea.getWidth() - frame.getWidth());
    petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
    petPos.y = juce::jlimit ((float) playArea.getY(), floorY, petPos.y);

    const auto newPetBounds = getCurrentPetVisualBounds();
    const bool moved = std::abs (petPos.x - oldPos.x) > 0.01f || std::abs (petPos.y - oldPos.y) > 0.01f;
    const bool changed = moved
                      || oldMode != motionMode
                      || oldAnimId != currentAnimId
                      || oldFrameIdx != currentFrameIdx
                      || oldBubbleVisible != showSpeechBubble
                      || oldBubbleText != currentSpeechText
                      || oldPetBounds != newPetBounds;

    if (changed)
        enqueuePetDirtyRepaint (oldPetBounds.getUnion (newPetBounds));
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
