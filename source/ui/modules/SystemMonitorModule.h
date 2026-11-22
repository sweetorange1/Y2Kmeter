#pragma once

#include <JuceHeader.h>

#include "source/ui/ModuleWorkspace.h"
#include "source/system/SystemStats.h"

// ==========================================================
// SystemMonitorModule —— 电脑状态模块（CPU / GPU / 温度 / 内存 / 显存 / 磁盘 / 网络 / 电池）
//
//   · 数据来自 y2k::sys::Sampler（后台线程采集，进程内共享；模块只拷快照）。
//     模块不可见（最小化 / 不在屏幕上）时自动退订，采集线程随之空闲。
//   · 四种显示样式（控制面板「样式」）：
//       Cards   —— 分段环形表 + 数值 + 迷你历史曲线（默认）
//       Bars    —— 任务管理器式横条列表（LED 分段条），可展开每核占用
//       Graphs  —— 每项一块历史曲线图（时间跨度可调）
//       Compact —— 一行 / 多行紧凑读数，适合横条布局
//   · 每一项都可独立开关；温度单位、刷新间隔、按负载变色、信息条可调。
//   · 不依赖音频分析（不订阅 AnalyserHub），对音频线程零影响。
// ==========================================================
class SystemMonitorModule : public ModulePanel,
                            private juce::Timer
{
public:
    SystemMonitorModule();
    ~SystemMonitorModule() override;

    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;

    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    HoverHelp getHoverHelp (juce::Point<int> localPos) const override;

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    enum Item
    {
        itemCpu = 0, itemCpuTemp, itemGpu, itemGpuTemp, itemRam,
        itemVram, itemDisk, itemNet, itemBattery, numItems
    };

    enum class Style { cards = 0, bars, graphs, compact };

    struct View
    {
        Item         item = itemCpu;
        juce::String label;       // 翻译键
        juce::String value;       // 主读数（数字，不翻译）
        juce::String sub;         // 副读数（可能含翻译键，绘制前已翻译）
        float        frac = -1.0f;   // 0..1，<0 = 不可用
        bool         available = false;
        juce::Colour colour;
        y2k::sys::Metric metric  = y2k::sys::Metric::cpu;
        bool         dualSeries = false;   // 网络：下行 + 上行两条曲线
    };

    void timerCallback() override;
    void updateSubscription();

    std::vector<View> buildViews() const;
    juce::Colour levelColour (Item item, float frac, float rawValue) const;
    juce::String formatTemp (float celsius) const;
    float        historyLo (const View& v) const;
    float        historyHi (const View& v) const;
    int          historySamples() const;

    void paintCards   (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views);
    void paintBars    (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views);
    void paintGraphs  (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views);
    void paintCompact (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views);
    void paintInfoBar (juce::Graphics& g, juce::Rectangle<int> area);
    void paintCoreStrip (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour colour);
    void paintHistory (juce::Graphics& g, juce::Rectangle<int> area, const View& v, bool withGrid);

    static std::vector<juce::Rectangle<int>> gridTiles (juce::Rectangle<int> area, int count, int gap);
    static juce::String shortHardwareName (const juce::String& name);

    bool itemShown (Item i) const noexcept;

    juce::SharedResourcePointer<y2k::sys::Sampler> sampler;
    bool subscribed = false;

    y2k::sys::Snapshot snap;
    juce::uint32       lastSerial = 0;

    std::array<float, numItems> shownFrac {};   // 动画中的显示值（0..1）
    std::array<float, numItems> targetFrac {};  // 最新采样对应的目标值（<0 = 不可用）
    std::array<bool,  numItems> enabled {};

    Style  style        = Style::cards;
    bool   perCore      = true;
    bool   infoBar      = true;
    bool   colourByLoad = true;
    bool   fahrenheit   = false;
    double intervalSec  = 1.0;
    double historySec   = 60.0;

    // 网络曲线的自动量程（bytes/s，按最近历史峰值平滑，避免跳变）
    float netScale = 256.0f * 1024.0f;

    // 悬停命中：最近一次绘制的 [项目 → 区域]
    std::vector<std::pair<Item, juce::Rectangle<int>>> hitTiles;

    mutable std::vector<float> histA, histB;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SystemMonitorModule)
};
