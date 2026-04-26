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

// ==========================================================
// VuMeterModule —— Pink XP 像素风"模拟 VU 表"指针仪表
//
//   · 与其它矩形柱状/数字模块视觉差异化：采用**半圆扇形表盘 + 指针**
//   · 单指针（总电平：L/R 功率合成），复用 AnalyserHub 已计算的 RMS L/R
//     → 后端零新增计算
//   · 右上角圆形 LED 信号灯：
//        无信号 (合成 dBFS < -60) → 暗灭
//        正常有信号               → 绿色 (慢速呼吸)
//        危险  (合成 dBFS >= warnDbfs) → 红色 (快速脉动)
//   · 指针 300ms 弹道（模拟机械迟滞），通过 smoothed 平滑实现
//   · 刻度采用直接的 **dBFS**（-60 .. 0），与其他 Meter 统一；
//     靠近 0 dBFS 的段红色警戒。
//   · LED 判定阈值 = 刻度红色段起点 → 指针到红段 = LED 变红，完全同步。
// ==========================================================
class VuMeterModule : public ModulePanel,
                     public AnalyserHub::FrameListener,
                     private juce::Timer
{
public:
    explicit VuMeterModule(AnalyserHub& hub);
    ~VuMeterModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // 60Hz 动画 Tick：按真实 dt 推进 displayedL/R 朝目标 targetL/R 插值，
    //   同时推进 LED 跟随值与脉动相位，并 repaint()。
    //   这样即便后端 Loudness 快照仍按 400ms 粗颗粒更新，前端每帧都在
    //   做子步插值，指针看起来是连续扫过的。
    void timerCallback() override;

    // 画一条指针（从圆心向刻度位置延伸）
    void drawNeedle (juce::Graphics& g,
                     juce::Point<float> pivot,
                     float              radius,
                     float              valueDb,       // 当前 dBFS 值
                     juce::Colour       colour,
                     float              thickness) const;

    // 画表盘底色 + 刻度 + 数字
    void drawDial   (juce::Graphics& g,
                     juce::Rectangle<int> dialArea,
                     juce::Point<float>&  outPivot,
                     float&               outRadius) const;

    // 画右上角 LED 信号灯
    void drawLed    (juce::Graphics& g,
                     juce::Rectangle<int> ledArea) const;

    // dBFS 值 → 指针角度（弧度）
    float vuToAngle(float valueDb) const noexcept;

    // 计算当前帧的"单声道总电平"（dBFS） —— 基于已平滑的 displayedL/R
    float currentMonoDbfs() const noexcept;

    // ---- 成员 ----
    AnalyserHub& hub;

    // -------- 解决"指针跟不上动态"的数据源方案 --------
    //
    //   问题背景：
    //     后端 LoudnessMeter.updateSnapshot() 只在每个 400ms K-weighting
    //     块边界刷新 rmsL/R；且 rmsL/R 本身是 100ms 滑窗 RMS，再叠上前端
    //     的 τ=180ms 平滑后，指针相对 Waveform 会明显"慢一拍"，连续瞬态
    //     期间指针持续悬在高位，回不来。
    //
    //   新方案（仍然零新增后端计算）：
    //     · 订阅 AnalyserHub::Kind::Oscilloscope —— 它是 2048 样本的滚动
    //       原始波形（Waveform 模块在用的同一数据源）。
    //     · onFrame 里从 oscL/R 尾部取 ~20ms 样本自己算瞬时 RMS，作为
    //       targetL/R。这比 Loudness 路的 100ms 滑窗/400ms 发布快一个
    //       数量级，能贴合 Waveform 看到的动态。
    //     · 指针用"非对称弹道"：上升 τ≈80ms（追瞬态）、下降 τ≈350ms
    //       （模拟 VU 机械回落感；同时保证连续瞬态时能逐步"踩上"新的高
    //       点而不是一直粘顶）。
    //
    // ----------------------------------------------------

    // 目标值（来自 onFrame，按原始样本逐帧刷新），单位 dBFS
    float targetL    = -144.0f;
    float targetR    = -144.0f;

    // 显示值（由 60Hz Timer 按 dt 连续插值），单位 dBFS
    float displayedL = -144.0f;
    float displayedR = -144.0f;

    // LED 快速跟随电平（dBFS），基于 displayed 合成 + 更短的衰减
    float ledLevelDb = -144.0f;

    // Timer tick 之间的真实时间（毫秒），用 HiRes counter 计算
    double lastTickMs = 0.0;

    // 脉动相位（用于 LED 呼吸/脉动，也由 Timer 推进）
    float pulsePhase = 0.0f;

    // 瞬时 RMS 的样本窗口长度（毫秒）。20ms 在典型 44.1/48 kHz 下约
    //   880/960 样本，远小于 oscilloscopeBufferSize=2048，够用。
    static constexpr float instantRmsWindowMs = 20.0f;

    // 非对称弹道的时间常数（毫秒）
    //   · 上升慢一点避免单样本尖峰把指针推到极值；取 80ms
    //   · 下降保留经典 VU 机械回落感，取 350ms —— 连续瞬态时由上升主导，
    //     下降不会锁死指针；静音后也能缓慢落回
    static constexpr float tauRiseMs = 80.0f;
    static constexpr float tauFallMs = 350.0f;

    // 刻度显示上下界（dBFS） —— 均匀等距从 -25 到 +3
    static constexpr float minDisplayDb = -25.0f;
    static constexpr float maxDisplayDb =  +3.0f;

    // 红色警戒段的起点（dBFS） —— 也是 LED 变红的同一个阈值
    //   0 dBFS = 数字满刻度，到达即开始削波风险
    static constexpr float warnDbfs     =   0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VuMeterModule)
};
