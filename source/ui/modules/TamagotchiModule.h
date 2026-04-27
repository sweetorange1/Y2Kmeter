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
        // 你给的描述里 29 有多朝向子态；当前资源编号只到 33，先以 30..33 对齐已有编号
        moveLeft = 30,
        yell = 31,
        runAwayLeft = 32,
        startled = 33
    };

    enum class MotionMode
    {
        patrol,
        startledIntro,
        falling,
        landingFall
    };

    void timerCallback() override;

    bool loadRandomRoleAnimations();
    void chooseNextAnimation();
    int chooseNextAnimByState() const;
    void stepOneFrame();
    void stepWander();
    void updateNeeds();
    void onAnimationFinished();
    int randomAnimFrom (std::initializer_list<int> ids) const;
    void drawPixelBar (juce::Graphics& g,
                       juce::Rectangle<int> area,
                       float value01,
                       juce::Colour fill,
                       juce::StringRef label) const;
    juce::Rectangle<int> getHudBounds() const;
    juce::Rectangle<int> getTestButtonBounds (int idx) const;
    int hitTestButton (juce::Point<int> pos) const;
    void applyTestButton (int idx);

    bool hasAnimation (int animId) const;

    void forceAnimation (int animId, bool restartFrame = true);

    juce::Image getCurrentFrame() const;

    juce::Rectangle<int> getDeleteButtonBounds() const;
    juce::Rectangle<int> getFocusBounds() const;
    Edge detectEdge (juce::Point<int> pos) const;
    void updateCursorFor (Edge e);

    juce::String roleName;
    std::array<juce::Array<juce::Image>, 33> animFrames;
    juce::Array<int> availableAnimIds;
    int currentAnimId = 1;
    int currentFrameIdx = 0;

    // 动画节拍：20 tick ≈ 1 秒切 1 帧
    int frameTickCounter = 0;

    // 动作/移动状态机
    MotionMode motionMode = MotionMode::patrol;
    bool patrolMoveLeft = true;
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

    // Tamagotchi 需求数值（0..100）
    float signalLevel01 = 0.0f;
    float hunger = 25.0f;
    float health = 90.0f;

    // 顶部像素风 HUD 区域高度（条和文字绘制区域）
    static constexpr int hudHeight = 34;

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