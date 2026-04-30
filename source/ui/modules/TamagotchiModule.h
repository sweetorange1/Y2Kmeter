#pragma once

#include <JuceHeader.h>
#include <array>
#include <initializer_list>
#include "source/ui/ModuleWorkspace.h"

// ==========================================================
// TamagotchiModule —— 独立小宠物模块（非拖图）
//   · 模块名固定：Tamagotchi
//   · 通过右键/双击 ModuleWorkspace 空白区添加
//   · 默认尺寸 80x80
//   · 当前阶段：随机播放角色动画（20 角色 / 33 动画），1 秒 1 帧
// ==========================================================
class TamagotchiModule : public ModulePanel,
                         private juce::Timer
{
public:
    TamagotchiModule();
    ~TamagotchiModule() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

    void setFocusVisual (bool shouldFocus);
    void setSignalLevel01 (float level01) noexcept;

    juce::String getRoleName() const noexcept;
    float getHunger() const noexcept;
    float getHealth() const noexcept;
    void restorePersistentState (const juce::String& savedRoleName,
                                 float savedHunger,
                                 float savedHealth);

private:

    enum class Edge { none, right, bottom, bottomRight };
    enum class DragMode { none, move, resize };

    // 动画行为枚举（按资源编号 1..33 映射）
    enum class PetAnim : int
    {
        wantToToilet = 1,
        toiletSquat = 2,
        wantToEat = 3,
        sleep = 4,
        almostSick = 5,
        sick = 6,
        nearDeath = 7,
        angry = 8,
        death = 9,
        runLeftToFood = 10,
        runLeftToHate = 11,
        runLeftToLike = 12,
        eatLeft = 13,
        full = 14,
        depressed = 15,
        lookAround = 16,
        talkNegative = 17,
        talkNormal = 18,
        talkHappy = 19,
        cry = 20,
        talkVeryNegative = 21,
        talkHappier = 22,
        talkHappiest = 23,
        talkShy = 24,
        shocked = 25,
        fight = 26,
        fall = 27,
        dazeThenWalkLeft = 28,
        lookLeft = 29,
        moveLeft = 30,
        yell = 31,
        runAwayLeft = 32,
        startled = 33
    };

    enum class MotionMode
    {
        egg,
        hatching,
        toilet,
        patrol,
        startledIntro,
        falling,
        landingFall,
        drowsy,
        sleeping,
        eating,
        hungry,
        starving,
        sick,
        criticalSick,
        dead
    };

    enum class PatrolAction
    {
        lookLeftSlow,
        lookRightSlow,
        moveLeftFast,
        moveRightFast,
        talk,
        jumpFight
    };

    void timerCallback() override;

    bool loadRandomRoleAnimations();

    void chooseNextAnimation();
    int chooseNextAnimByState() const;
    void stepOneFrame();
    void stepWander();
    bool shouldUseRightVariantForAnim (int animId) const;
    const juce::Array<juce::Image>& getFramesForAnim (int animId) const;
    const juce::Array<juce::Image>& getRightFramesForAnim (int animId) const;
    bool hasRightVariantFrames (int animId) const;

    void updateNeeds();
    MotionMode evaluateAutoMotionMode() const;
    void switchMotionMode (MotionMode newMode);
    void onAnimationFinished();
    int randomAnimFrom (std::initializer_list<int> ids) const;
    void beginPatrolCycle();
    void beginPatrolAction (PatrolAction action);
    void stepPatrolAction();
    void stepJumpFight();
    void drawPixelBar (juce::Graphics& g,
                       juce::Rectangle<int> area,
                       float value01,
                       juce::Colour fill,
                       juce::StringRef label) const;

    juce::Rectangle<int> getHudBounds() const;
    juce::Rectangle<int> getTestButtonBounds (int idx) const;
    juce::Rectangle<int> getStateModeComboBounds() const;
    juce::Rectangle<int> getAnimTriggerComboBounds() const;
    int hitTestButton (juce::Point<int> pos) const;
    void applyTestButton (int idx);

    void refreshDebugAnimTriggerItems();
    void applyForcedMotionMode();
    void triggerDebugAnimationById (int triggerId);

    bool hasAnimation (int animId) const;

    void forceAnimation (int animId, bool restartFrame = true);

    juce::Image getCurrentFrame() const;
    juce::Rectangle<int> getPetVisualBoundsFor (const juce::Image& frame,
                                                juce::Point<float> pos,
                                                bool bubbleVisible,
                                                const juce::String& bubbleText,
                                                MotionMode mode) const;
    juce::Rectangle<int> getCurrentPetVisualBounds() const;
    int getTargetVisualHzForMode (MotionMode mode) const noexcept;
    void enqueuePetDirtyRepaint (juce::Rectangle<int> area);
    void flushVisualRepaintQueue (bool forceNow);

    juce::Rectangle<int> getDeleteButtonBounds() const;
    juce::Rectangle<int> getFocusBounds() const;
    Edge detectEdge (juce::Point<int> pos) const;
    void updateCursorFor (Edge e);

    // ---- non-opaque 正确重绘辅助 ----
    //
    // 本模块 setOpaque(false)（background 需要透出 ModuleWorkspace 的半透明
    // content + 桌面），因此 repaint(dirty) 必须先让父组件把底色画一遍，否则
    // 新帧叠在旧帧上形成拖影，或 focus/delete 旧像素残留。
    // 下面两个函数同时通知父组件和本组件重绘同一块区域。
    void repaintSelfAndParent (juce::Rectangle<int> localRect);
    void repaintSelfAndParent();

    juce::String roleName;
    std::array<juce::Array<juce::Image>, 33> animFrames;
    std::array<juce::Array<juce::Image>, 33> animFramesRight;
    juce::Array<int> availableAnimIds;

    int currentAnimId = 1;
    int currentFrameIdx = 0;

    // 动画节拍：20Hz 下约每 1 秒切 1 帧；动态刷新率下使用时间累积
    float frameAccumSec = 0.0f;

    int currentVisualHz = 20;
    float currentTickDtSec = 1.0f / 20.0f;

    bool hasQueuedPetDirty = false;
    juce::Rectangle<int> queuedPetDirtyBounds;

    // 动作/移动状态机
    MotionMode motionMode = MotionMode::patrol;
    bool hasPetPosition = false;

    // 宠物位置（以当前帧左上角为基准）
    juce::Point<float> petPos { 0.0f, 0.0f };
    // 世界坐标锚点：代表角色“脚底中心”的 X（不随帧宽变化）
    float petGroundAnchorX = 0.0f;

    // 时间节拍与物理参数
    int idleTicksRemaining = 0;
    float patrolSpeedPxPerTick = 0.95f;
    float fallVelocityPxPerTick = 0.0f;
    float gravityPxPerTick2 = 0.28f;
    juce::Rectangle<int> lastLocalBounds;

    // eating 状态（饥饿增长时）：每 4 秒轮播 10/11/12/13，并左右随机小幅走动
    static constexpr int eatingSwitchTicks = 80;
    int eatingAnimTicksRemaining = 0;
    int eatingCurrentAnimId = (int) PetAnim::runLeftToFood;
    float eatingMoveDir = 1.0f; // -1 left, +1 right
    float eatingMoveSpeedPxPerTick = 0.9f;
    int eatingMoveRetargetTicksRemaining = 0;
    bool eatingMirrorX = false;
    static constexpr int eatingLowSignalHoldTicks = 60;
    int eatingLowSignalTicksRemaining = 0;

    // 静音计时（20Hz）
    int silentTicks = 0;
    static constexpr int hungerDecaySilentDelayTicks = 1200;
    static constexpr int drowsySilentTicks = 400;
    static constexpr int sleepSilentTicks = 800;

    // patrol 行为节拍（20Hz）
    static constexpr int ticksPerSecond = 20;
    static constexpr int patrolCycleTicks = 6 * ticksPerSecond;
    static constexpr int patrolFastMoveTicks = 4 * ticksPerSecond;

    PatrolAction currentPatrolAction = PatrolAction::lookLeftSlow;
    int patrolActionTicksRemaining = 0;
    int patrolCooldownTicksRemaining = patrolCycleTicks;
    int patrolLockedAnimId = 0;
    bool patrolFaceLeft = true;
    bool patrolMirrorX = false;
    float patrolActionMoveDir = 0.0f; // -1 left, +1 right
    float patrolActionSpeedPxPerTick = 0.0f;

    // 说话动作（头顶单行文本）
    bool showSpeechBubble = false;
    juce::String currentSpeechText;

    // 蛋状态资源：新建模块随机蛋样式；孵化时播放前4帧
    juce::Array<juce::Image> eggFrames;
    int eggStyleId = 1;

    // 测试下拉框：状态机强制 + 动画触发
    juce::ComboBox stateModeCombo;
    juce::ComboBox animTriggerCombo;
    bool forceMotionModeEnabled = false;
    MotionMode forcedMotionMode = MotionMode::patrol;

    // fight 跳跃动作（2 秒内快速上下两次）
    bool jumpFightActive = false;
    int jumpFightTick = 0;
    float jumpFightBaseY = 0.0f;
    static constexpr int jumpFightTotalTicks = 2 * ticksPerSecond;
    static constexpr int jumpFightCount = 2;
    static constexpr float jumpFightAmplitudePx = 6.0f;

    // Tamagotchi 需求数值（0..100）

    float signalLevel01 = 0.0f;
    float hunger = 75.0f;
    float health = 75.0f;

    float hungerDeltaPerSecond = 0.0f;

    // toilet 状态：当饥饿值从 50 连续增长到 100 时触发，持续 6 秒
    bool hungerRiseFrom50Tracking = false;
    bool toiletTriggerPending = false;
    static constexpr int toiletDurationTicks = 120;
    int toiletTicksRemaining = 0;

    // 顶部像素风 HUD 区域高度（条、测试按钮、调试下拉框）
    static constexpr int hudHeight = 64;

    // 测试按钮（+H/-H/+HP/-HP）
    static constexpr int testButtonCount = 4;
    int hoveredTestButton = -1;
    int pressedTestButton = -1;

    // 聚焦/删除按钮
    bool focused = false;

    bool deleteBtnHovered = false;
    bool deleteBtnPressed = false;

    // 拖拽/缩放
    DragMode dragMode = DragMode::none;
    Edge resizeEdge = Edge::none;
    juce::Point<int> dragStartMouse;
    juce::Rectangle<int> dragStartBounds;

    static constexpr int edgeHotSize = 8;
    static constexpr int deleteButtonSize = 18;
    static constexpr int minW = 64;
    static constexpr int minH = 64;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TamagotchiModule)
};