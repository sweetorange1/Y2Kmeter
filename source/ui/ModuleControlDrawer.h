#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>

#include "source/ui/ModuleWorkspace.h"

// ==========================================================
// ModuleControlDrawer —— 模块控制面板（v3.1 参数系统）
//
//   · 由 ModulePanel 在首次点开齿轮按钮时懒创建，作为模块的子组件覆盖在
//     内容区顶部（覆盖式抽屉：不改变画布尺寸，不触发任何模块的历史清空）；
//   · 控件完全由 ModulePanel::buildParamDefs() 的参数表生成：
//       choice  → 一排互斥按钮（放不下自动换行）
//       toggle  → ON / OFF 按钮
//       slider  → 横向滑条（数值框在右侧，可带后缀）
//   · 数据读写只经 ModulePanel::getParamValue / setParamValue，抽屉自己不存状态；
//   · 参数过多时内部用 Viewport 竖向滚动，抽屉高度上限 = 内容区的 80%；
//   · 整个抽屉及其子控件标记 toy.optOut，避免点击控件触发 Toybox 彩蛋。
// ==========================================================
class ModuleControlDrawer : public juce::Component
{
public:
    explicit ModuleControlDrawer (ModulePanel& ownerPanel);
    ~ModuleControlDrawer() override;

    // 重新读取参数表并重建全部控件（参数表本身变化时调用；通常只需构造后调一次）
    void rebuild();

    // 把模块当前参数值同步到控件，并按 visibleIf 重新排布
    void syncFromModule();

    // 在给定宽度下排布后的内容总高度（含内边距）
    int getContentHeight (int width);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Row
    {
        ParamDef def;
        bool visible = true;
        juce::Rectangle<int> labelRect;                       // inner 坐标
        std::vector<std::unique_ptr<juce::TextButton>> btns;  // choice：每个选项一个；toggle：1 个
        std::unique_ptr<juce::Slider> slider;
    };

    struct GroupHeader
    {
        juce::String text;
        juce::Rectangle<int> rect;                            // inner 坐标
    };

    class Inner : public juce::Component
    {
    public:
        explicit Inner (ModuleControlDrawer& d) : drawer (d) {}
        void paint (juce::Graphics& g) override;
    private:
        ModuleControlDrawer& drawer;
    };

    int  layoutInner (int width);                 // 返回 inner 内容高度，并写入各控件 bounds
    void onParamChanged (size_t rowIndex, double value);

    ModulePanel& owner;
    juce::Viewport viewport;
    Inner inner { *this };
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<GroupHeader> headers;
    bool syncing = false;
    int  lastInnerWidth = -1;
    int  lastInnerHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleControlDrawer)
};
