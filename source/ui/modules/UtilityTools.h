#pragma once

#include <JuceHeader.h>

#include "source/ui/ModuleWorkspace.h"   // ParamDef

// ==========================================================
// UtilityTools —— 小工具模块里的各个工具视图
//
//   每个工具是一个独立的 Component（ToolView），由 UtilityModule 按当前
//   选择创建 / 销毁。工具自己负责：
//     · 绘制与交互（全部跟随 PinkXP 主题色，文字经 y2k::tr 翻译）；
//     · 控制面板里属于自己的参数（buildParams / getParam / setParam）；
//     · 状态存取（save / restore，写进 UtilityModule 存档里以工具名命名的子节点，
//       切换工具后再切回来，之前的计算 / 颜色 / 便签内容都还在）。
//   状态变化后调用 notifyChanged() → 模块触发布局存档（工作区自带防抖）。
// ==========================================================

namespace y2k::util
{
    enum class ToolKind
    {
        calculator = 0,
        colourPicker,
        stopwatch,
        timer,
        converter,
        clock,
        notes,
        numTools
    };

    constexpr int kNumTools = (int) ToolKind::numTools;

    // 存档子节点名（只追加，不改名）
    const char* toolStateName (ToolKind k) noexcept;

    // 工具显示名（翻译键）
    const char* toolDisplayName (ToolKind k) noexcept;

    // 某个工具在控制面板里的参数（与当前选中的工具无关，供模块一次性列出全部参数，
    //   再用 ParamDef::when 只显示当前工具的那几项 —— 抽屉的参数表因此保持稳定）
    void buildToolParams (ToolKind k, std::vector<ParamDef>& out);

    class ToolView : public juce::Component
    {
    public:
        ~ToolView() override = default;

        virtual bool   getParam (const juce::String& id, double& out) const { juce::ignoreUnused (id, out); return false; }
        virtual bool   setParam (const juce::String& id, double v)          { juce::ignoreUnused (id, v);   return false; }

        virtual void   save (juce::ValueTree& state) const          { juce::ignoreUnused (state); }
        virtual void   restore (const juce::ValueTree& state)       { juce::ignoreUnused (state); }

        // 主题 / 语言切换后调用（TextEditor 等缓存了颜色 / 字体的子控件需要刷新）
        virtual void   themeChanged() { repaint(); }

        std::function<void()> onStateChanged;

    protected:
        void notifyChanged() { if (onStateChanged) onStateChanged(); }
    };

    std::unique_ptr<ToolView> createToolView (ToolKind kind);
}
