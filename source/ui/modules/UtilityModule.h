#pragma once

#include <JuceHeader.h>

#include "source/ui/ModuleWorkspace.h"
#include "source/ui/modules/UtilityTools.h"

// ==========================================================
// UtilityModule —— 小工具模块
//
//   控制面板「工具」里选择：计算器 / 取色器 / 秒表 / 倒计时 / 音频换算 /
//   时钟 / 便签；每个工具的专属选项也在同一个控制面板里（只显示当前工具的）。
//
//   · 每个工具的状态各自保存在存档的子节点里，切走再切回来内容都还在
//     （计算结果、取色历史、秒表 / 倒计时进度、便签文本……）。
//   · 秒表 / 倒计时用绝对时间戳记录，关掉软件再打开仍按真实时间继续。
//   · 模块是交互型的：退出 Toybox 点击彩蛋（toy.optOut），按钮 / 输入框
//     不会被点击特效打扰。
//   · 不订阅 AnalyserHub，对音频分析零影响。
// ==========================================================
class UtilityModule : public ModulePanel
{
public:
    UtilityModule();
    ~UtilityModule() override;

    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;

    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

protected:
    void paintContent  (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    void layoutContent (juce::Rectangle<int> contentBounds) override;
    juce::Rectangle<int> getChromaKeyCanvas() override;

private:
    void setTool (y2k::util::ToolKind kind, bool force = false);
    void captureCurrentToolState() const;
    void updateTitle();
    void syncBackdropMode();

    y2k::util::ToolKind tool = y2k::util::ToolKind::calculator;
    std::unique_ptr<y2k::util::ToolView> view;

    // 所有工具的状态（子节点名 = toolStateName）；save() 是 const，故 mutable
    mutable juce::ValueTree toolStates { "tools" };

    int themeToken        = -1;
    int chromaRevealToken = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UtilityModule)
};
