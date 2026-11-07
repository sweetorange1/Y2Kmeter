#ifndef Y2K_HOVER_HELP_H_INCLUDED
#define Y2K_HOVER_HELP_H_INCLUDED

#include <JuceHeader.h>

// ==========================================================
// HoverHelp —— 悬停提示（按钮 / 仪表模块的详细功能说明）
//
//   组成：
//     1. y2k::help 命名空间：帮助文案库（模块详细说明）+ 全局开关（持久化）
//     2. Y2KTooltipWindow：继承 juce::TooltipWindow，统一解析"鼠标下是谁"：
//          · juce 控件（Button / Slider / ComboBox / Label ...）→ 自身 setTooltip 文案
//            （子组件没有文案时沿父链向上找，如 Slider 的数值框 → Slider）；
//          · 落在 ModulePanel（含其任意子组件）上 → ModulePanel::getHoverHelp(pos)，
//            由模块按鼠标位置返回标题栏 / 自绘按钮 / 仪表内容区的说明；
//          · 仪表内容区（contentZone）提示需鼠标静止一段时间才出现、移动即收起，
//            避免挡住频谱 / 波形等模块自带的悬停读数标尺。
//     3. 绘制由 PinkXPLookAndFeel::drawTooltip / getTooltipBounds 完成
//        （Pink XP 小窗口：粉色标题条 + 正文自动换行，跟随主题）。
//
//   文案格式约定：  "标题\n正文"（正文可再含 \n 换行）；不含 \n 时整段作为正文。
//
//   宿主方式：TooltipWindow 作为 Editor（以及每个浮动模块窗口）的子组件存在，
//     走 Editor 的 GL 合成层绘制 —— 插件宿主内不另开原生窗口，伪全屏时也不会
//     被主窗口表面压住（与 PinkXP::getInWindowMenuHost 同一思路）。
// ==========================================================

enum class ModuleType;

namespace y2k::help
{
    // 组装 "标题\n正文"
    juce::String make (const juce::String& title, const juce::String& body);

    // 全局开关（进程内共享；独立存档到用户目录 Y2Kmeter.uiPrefs.xml，
    //   Standalone 与插件共用，不随布局 / 预设切换）
    bool isEnabled();
    void setEnabled (bool shouldBeEnabled);

    // 模块详细说明（"模块名\n功能说明"）
    juce::String moduleHelp (ModuleType type);

    // 标题栏附加的通用操作提示（拖动 / 缩放 / 双击最大化等）
    juce::String moduleTitleUsage (bool isFloating, bool layoutLocked);
}

// ----------------------------------------------------------
// Y2KTooltipWindow —— 项目唯一的悬停提示窗口类型
//   parent 必须非空（Editor 或浮动模块窗口），同一个 parent 只能挂一个实例。
// ----------------------------------------------------------
class Y2KTooltipWindow : public juce::TooltipWindow
{
public:
    explicit Y2KTooltipWindow (juce::Component* parentComponent);
    ~Y2KTooltipWindow() override;

    juce::String getTipFor (juce::Component& c) override;

    // 鼠标在仪表内容区需静止多久才弹出说明（ms）
    static constexpr juce::uint32 kContentStillMs = 700;
    // 静止判定容差（屏幕逻辑像素）
    static constexpr int kStillTolerancePx = 4;
    // 普通控件（按钮 / 标题栏）弹出延迟
    static constexpr int kTipDelayMs = 650;

private:
    juce::Point<int> lastPollPos { -100000, -100000 };
    juce::uint32     stillSinceMs = 0;

    // 旧提示回收：鼠标移出本提示窗口所属 peer（浮动模块窗口、macOS 上
    //   Milkdrop 的顶层控制栏窗口等）后，JUCE 的 TooltipWindow 会因
    //   "鼠标下组件的 peer ≠ 提示窗口的 peer" 跳过整个更新分支 —— 旧提示
    //   既不会刷新也不会隐藏，看起来就像"悬停按钮却显示别的模块的说明"。
    //   这个低频守卫只负责把已经不属于本窗口的提示收起来。
    struct StaleTipGuard : public juce::Timer
    {
        explicit StaleTipGuard (Y2KTooltipWindow& o) : owner (o) {}
        void timerCallback() override;
        Y2KTooltipWindow& owner;
    };

    StaleTipGuard staleTipGuard { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Y2KTooltipWindow)
};

#endif // Y2K_HOVER_HELP_H_INCLUDED
