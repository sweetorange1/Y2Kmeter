#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>

#include "source/ui/ModuleWorkspace.h"

// ==========================================================
// ModuleControlDrawer —— 模块控制面板（v3.6 分类标签页 + 卡片化重构）
//
//   · 由 ModulePanel 在首次点开齿轮按钮时懒创建；
//   · 默认在模块**外部**最合理的空余区域展开（工作区内侧边/上下，空间不足或
//     模块本身处于浮动态时提升为独立临时桌面窗口），完全不遮挡模块本身的实时显示；
//   · 顶部带精致的 PinkXP 迷你标题条（显示所属模块名 + 关闭 × 按钮），支持鼠标拖拽
//     自由微调位置，按 Esc 或点击外部空白处自动收起；
//   · 参数分组 >= 2 且可见参数 > 6 时自动启用 PinkXP 分类标签条（跨标签宽度恒定，
//     切换标签零横向抖动、高度平滑贴合当前标签内容）；简单模块则单页直接展示；
//   · 每组控件收进带细描边与自适应底色的卡片中：
//       choice  → >=4 项或换行时采用标签置顶 + 全宽均衡按钮网格；2~3 项单行齐平
//       toggle  → 同组相邻两个开关在宽度充足且零截断时自动双列并排，否则回落单列
//       slider  → 对齐标签列 + 宽裕轨道 + 实测防截断数值框
//   · 撤回 / 重做定位（revealParam）：自动切到对应标签、滚入视口并触发 ~900ms 柔光脉冲；
//   · 参数过多时内部用 Viewport 竖向滚动；
//   · 整个抽屉及其子控件标记 toy.optOut，避免点击控件触发 Toybox 彩蛋。
// ==========================================================
class ModuleControlDrawer : public juce::Component,
                            private juce::Timer
{
public:
    explicit ModuleControlDrawer (ModulePanel& ownerPanel);
    ~ModuleControlDrawer() override;

    // 重新读取参数表并重建全部控件（参数表本身变化时调用；通常只需构造后调一次）
    void rebuild();

    // 把模块当前参数值同步到控件，并按 visibleIf 重新排布
    void syncFromModule();

    // 在给定视口宽度下排布后的内容区总高度（含内边距，不含顶部标题栏、标签条与外框；纯计算不改子控件）
    int getContentHeight (int width);

    // 在给定外框宽度下，当前激活页完整面板的理想总高度（含外框 + 迷你标题条 + 标签条 + 参数区；纯计算不改子控件）
    int getPreferredTotalHeight (int outerWidth);

    // 在给定外框宽度下，所有标签页中最高一页的理想总高度（供宿主决策恒定落位侧与宽度，杜绝切标签横向跳动）
    int getTallestTabTotalHeight (int outerWidth);

    // 按当前语言 / 字体 / UI 缩放 / 全部可见标签页实测出的外框宽度区间（跨标签恒定，纯计算不改子控件）
    int getMinimumOuterWidth();
    int getPreferredOuterWidth();
    int getMaximumOuterWidth();

    // 便捷别名（与外部布局协议及测试口径一致）
    int getMinWidth()                      { return getMinimumOuterWidth(); }
    int getPreferredWidth()                { return getPreferredOuterWidth(); }
    int getMaxWidth()                      { return getMaximumOuterWidth(); }
    int getPreferredHeightForWidth (int w) { return getPreferredTotalHeight (w); }

    // 撤回 / 重做自动定位到指定参数：切换所属标签页 + 滚入视口 + 柔和脉冲高亮
    void revealParam (const juce::String& paramId);

    // 轻量只读 / 交互与测试查询接口
    bool hasTabs() const noexcept;
    int getTabCount() const noexcept;
    int getActiveTabIndex() const noexcept;
    juce::String getTabName (int index) const;
    void selectTab (int index);
    juce::String getHighlightedParamId() const noexcept;
    int getVisibleRowCount() const noexcept;

    // 确保当前内部控件布局与当前尺寸 / 字体 / 缩放 / 可见性 / 当前标签页完全一致
    // （当 setBounds 因外框尺寸未变而未触发 resized() 时由宿主调用）
    void ensureLayoutCurrent();

    // 迷你标题条高度（随 UI 密度缩放）
    static int headerBarHeight() noexcept;

    // ---- 独立临时桌面窗口悬浮（供浮动模块 / 最大化模块 / 小窗口外部展开使用）----
    void setDesktopFloating (bool shouldFloat, juce::Component* mainWindow = nullptr);
    bool isDesktopFloating() const noexcept { return desktopFloating_; }

    // 拖拽活动范围（工作区本地坐标或屏幕坐标）
    void setDragClampBounds (juce::Rectangle<int> bounds) noexcept { dragClampBounds_ = bounds; }
    void resetUserMoved() noexcept { userMoved_ = false; }
    bool hasUserMoved() const noexcept { return userMoved_; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    struct Row
    {
        ParamDef def;
        bool visible = true;                                    // visibleIf 结果
        bool shownOnActivePage = false;                         // 当前激活标签页是否展示此行
        juce::Rectangle<int> labelRect;                         // inner 坐标
        juce::Rectangle<int> highlightRect;                     // inner 坐标（脉冲高亮与滚入视口区域）
        std::vector<std::unique_ptr<juce::TextButton>> btns;    // choice：每个选项一个；toggle：1 个
        std::unique_ptr<juce::Slider> slider;
    };

    struct GroupBucket
    {
        juce::String name;
        std::vector<size_t> rowIndices;                         // 仅收录当前 visible==true 的行
    };

    struct GroupHeader
    {
        juce::String text;
        juce::Rectangle<int> rect;                              // inner 坐标（非标签页模式下的分组抬头）
    };

    struct SectionCard
    {
        juce::Rectangle<int> rect;                              // inner 坐标
    };

    struct TabButtonLayout
    {
        int groupIndex = 0;
        int rowIndex   = 0;
        juce::String name;
        juce::Rectangle<int> bounds;                            // ModuleControlDrawer 本地坐标
    };

    struct TabStripLayout
    {
        juce::Rectangle<int> stripBounds;                       // ModuleControlDrawer 本地坐标
        int height  = 0;
        int numRows = 0;
        std::vector<TabButtonLayout> tabs;
    };

    class Inner : public juce::Component
    {
    public:
        explicit Inner (ModuleControlDrawer& d) : drawer (d) {}
        void paint (juce::Graphics& g) override;
        void paintOverChildren (juce::Graphics& g) override;
    private:
        ModuleControlDrawer& drawer;
    };

    struct DrawerMetrics
    {
        int rowH = 22;
        int choiceLabelH = 18;
        int gap = 4;
        int stackedGap = 2;
        int pad = 6;
        int cardPad = 6;
        int cardGap = 6;
        int headH = 16;
        int headerToCardGap = 3;
        int labelGap = 8;
        int pairColGap = 10;
        int pairLblGap = 6;
        int scrollW = 10;

        int tabH = 22;
        int tabGap = 2;
        int tabPadX = 5;
        int tabPadTop = 4;
        int tabPadBot = 1;

        int toggleW = 48;
        int sliderMinTrack = 76;
        int sliderIdealTrack = 112;

        int minOuterW = 236;
        int prefOuterW = 320;
        int maxOuterW = 460;
    };

    struct ComputedRowLayout
    {
        size_t rowIndex = 0;
        bool visible = false;
        juce::Rectangle<int> labelRect;
        juce::Rectangle<int> highlightRect;
        juce::Rectangle<int> sliderRect;
        int sliderBoxW = 0;
        std::vector<juce::Rectangle<int>> btnRects;
    };

    struct ComputedLayout
    {
        int contentHeight = 0;
        std::vector<GroupHeader> headers;
        std::vector<SectionCard> cards;
        std::vector<juce::Rectangle<int>> pairDividers;
        std::vector<ComputedRowLayout> rows;
    };

    juce::Rectangle<int> getHeaderBarBounds() const;
    juce::Rectangle<int> getCloseButtonBounds() const;
    int hitTestTabIndex (juce::Point<int> pos) const noexcept;
    void paintTabStrip (juce::Graphics& g) const;

    bool rebuildGroups();
    DrawerMetrics computeMetrics();
    TabStripLayout computeTabStripLayout (int outerWidth, const DrawerMetrics& m) const;
    ComputedLayout computeLayoutForTab (int innerWidth, const DrawerMetrics& m, int tabIndex) const;
    void applyLayout();
    bool syncFromModuleInternal();
    void onParamChanged (size_t rowIndex, double value);
    void finishActiveSliderDrag (bool commitIfChanged);
    void detachSliderCallbacks();
    void refreshThemeColours();
    void updateTimerState();
    void timerCallback() override;

    ModulePanel& owner;
    juce::Viewport viewport;
    Inner inner { *this };
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<GroupBucket> groups_;
    std::vector<GroupHeader> headers;
    std::vector<SectionCard> cards_;
    std::vector<juce::Rectangle<int>> pairDividers_;
    TabStripLayout tabStripLayout_;

    bool useTabs_ = false;
    int  activeTabIndex_ = 0;
    int  hoveredTabIndex_ = -1;
    int  totalVisibleRows_ = 0;
    juce::String activeGroupName_;

    // 撤回 / 重做定位脉冲高亮状态（非活跃时零定时器开销）
    juce::String highlightedParamId_;
    juce::uint32 highlightStartMs_ = 0;
    float        highlightAlpha_   = 0.0f;

    bool syncing = false;
    int  activeDragRow_ = -1;
    bool dragHadValueChange_ = false;
    bool layoutDirty_ = true;
    bool refreshingLayout_ = false;
    int  lastInnerWidth = -1;
    int  lastInnerHeight = 0;
    int  lastOuterW_ = -1;
    int  lastOuterH_ = -1;
    int  lastBodyW_ = -1;
    int  lastBodyH_ = -1;
    int  lastAppliedTabIndex_ = -1;
    bool lastAppliedUseTabs_ = false;
    float lastUiScale_ = 0.0f;
    juce::uint32 lastFontGen_ = 0;

    // 迷你标题栏交互（关闭按钮 + 拖拽重定位）
    bool closeHovered_ = false;
    bool closePressed_ = false;
    bool dragging_     = false;
    bool userMoved_    = false;
    juce::Point<int>     dragStartScreen_;
    juce::Point<int>     dragStartTopLeft_;
    juce::Rectangle<int> dragClampBounds_;

    // 独立临时桌面窗口状态
    bool desktopFloating_   = false;
    bool lookAndFeelPinned_ = false;
    int  themeSubToken_     = -1;
    juce::Component::SafePointer<juce::Component> floatingMainWindow_;
    juce::Rectangle<int>                          floatingMainWindowBounds_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleControlDrawer)
};
