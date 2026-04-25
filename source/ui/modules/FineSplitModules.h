#pragma once

#include <JuceHeader.h>
#include <array>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

class LufsRealtimeModule : public ModulePanel,
                           public AnalyserHub::FrameListener
{
public:
    explicit LufsRealtimeModule(AnalyserHub& hub);
    ~LufsRealtimeModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    AnalyserHub& hub;
    float lufsM = -144.0f;
    float lufsS = -144.0f;
    float lufsI = -144.0f;

    // 动画："活性心跳"脂点相位（每帧 +0.18）+ 上一帧值用于"值变化闪烁"
    float pulsePhase = 0.0f;
    float prevM = -144.0f, prevS = -144.0f, prevI = -144.0f;
    float flashM = 0.0f,   flashS = 0.0f,   flashI = 0.0f; // 闪烁衰减计时 [0,1]
};

class TruePeakModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit TruePeakModule(AnalyserHub& hub);
    ~TruePeakModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    AnalyserHub& hub;
    float tpL = -144.0f;
    float tpR = -144.0f;
    float rmsL = -144.0f;
    float rmsR = -144.0f;

    float pulsePhase = 0.0f;
    float prevTp = -144.0f, prevRms = -144.0f;
    float flashTp = 0.0f,   flashRms = 0.0f;
};

class OscilloscopeChannelModule : public ModulePanel,
                                  public AnalyserHub::FrameListener
{
public:
    OscilloscopeChannelModule(AnalyserHub& hub, bool leftChannel);
    ~OscilloscopeChannelModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    AnalyserHub& hub;
    bool useLeft = true;
    juce::Array<float> oscL;
    juce::Array<float> oscR;
};

class SpectrumOverviewModule_REMOVED {}; // 已废弃（原 Spectrum Overview） – 保留空壳防编译器获得路径时报错，无任何功能

class PhaseCorrelationModule : public ModulePanel,
                               public AnalyserHub::FrameListener
{
public:
    explicit PhaseCorrelationModule(AnalyserHub& hub);
    ~PhaseCorrelationModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    AnalyserHub& hub;
    float corr = 0.0f;
};

class PhaseBalanceModule : public ModulePanel,
                           public AnalyserHub::FrameListener
{
public:
    explicit PhaseBalanceModule(AnalyserHub& hub);
    ~PhaseBalanceModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    AnalyserHub& hub;
    float width = 0.0f;
    float balance = 0.0f;
};

class DynamicsMetersModule : public ModulePanel,
                             public AnalyserHub::FrameListener
{
public:
    explicit DynamicsMetersModule(AnalyserHub& hub);
    ~DynamicsMetersModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    AnalyserHub& hub;
    std::array<float, 4> meterDb {{ -144.0f, -144.0f, -144.0f, -144.0f }};
};

class DynamicsDrModule : public ModulePanel,
                         public AnalyserHub::FrameListener
{
public:
    explicit DynamicsDrModule(AnalyserHub& hub);
    ~DynamicsDrModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    AnalyserHub& hub;
    float shortDR = 0.0f;
    float integDR = 0.0f;
    float crest = 0.0f;

    float pulsePhase = 0.0f;
    float prevShort = 0.0f, prevInteg = 0.0f, prevCrest = 0.0f;
    float flashShort = 0.0f, flashInteg = 0.0f, flashCrest = 0.0f;
};

class DynamicsCrestModule : public ModulePanel,
                            public AnalyserHub::FrameListener
{
public:
    explicit DynamicsCrestModule(AnalyserHub& hub);
    ~DynamicsCrestModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    void layoutContent(juce::Rectangle<int> contentBounds) override;

private:
    // 额外的帺幻条样式满足用户的格子风 UI。
    AnalyserHub& hub;

    // 运行时可变的环形历史缓冲 + 写指针
    std::vector<float> crestHist;
    int writePos = 0;

    // 颗粒度控制：缓冲长度固定为 3600 点（预留空间，当前最大只需 35s×30=1050）
    //   · 每一帧 onFrame 都写入一个样本（framesPerSample 恒为 1）
    //   · 显示窗口按 spanSeconds 取"最近 spanSeconds × 30"个点绘制
    //   · 这样无论 span 大小，图像每帧都推进 1 像素（不再"调大 span 就卡"）
    //   滑条维度："时间跨度"（秒） – 滑条越高→显示窗口越长→看长期趋势
    //   → 直观上对用户就是“颗粒度”
    int   spanSeconds      = 20;   // 默认 20 秒，范围 [1, 35]
    int   framesPerSample  = 1;    // 保留为 1（每帧写入），此字段保留兼容但不再参与颗粒度计算
    int   frameCounter     = 0;    // 保留为 0

    static constexpr int histMaxLen = 3600; // 120s × 30fps 的最大容量

    // 右侧的额外滑条 UI（与 EqModule 的 SIZE 滑条同样风格）
    juce::Slider spanSlider;
    juce::Label  spanLabel;

    // 主题订阅 token：切换主题时重新下发 spanLabel 的 textColourId，
    //   避免 Label 缓存的旧 ink 颜色在主题切换后与标题栏不一致。
    int themeSubToken = -1;
};