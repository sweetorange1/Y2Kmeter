#pragma once

#include <JuceHeader.h>
#include <array>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/FramePacing.h"


// ==========================================================
// VuMeterModule —— Pink XP 像素风"模拟 VU 表"指针仪表
//
//   · 与其它矩形柱状/数字模块视觉差异化：指针式表盘，三种结构完全不同的样式
//       · 0 半圆（经典）—— 底边为直径的 180° 上半圆
//       · 1 弧形        —— 240° 圆表 + 粗弧轨
//       · 2 梯形        —— 老式扇形刻度：刻度线左长右短（轮廓成梯形），长指针横扫
//   · 单指针（总电平：L/R 功率合成），订阅 Oscilloscope 从原始波形算瞬时 RMS
//     → 后端零新增计算
//   · 右上角圆形 LED 信号灯：
//        无信号 (合成 dBFS <= ledSignalDbfs) → 暗灭
//        正常有信号 (ledSignalDbfs < 合成 dBFS < warnDbfs) → 绿色 (慢速呼吸)
//        危险  (合成 dBFS >= warnDbfs) → 红色 (快速脉动)
//   · 指针非对称弹道：上升 τ≈80ms（追瞬态）、下降 τ≈350ms（模拟机械回落）
//   · 刻度采用直接的 **dBFS**（minDisplayDb .. maxDisplayDb），与其他 Meter 统一；
//     靠近 warnDbfs 的段红色警戒。
//   · LED 变红阈值 = warnDbfs（表盘红色段起点）；LED 绿灯点亮阈值 = ledSignalDbfs，
//     与表底刻度（minDisplayDb）解耦。
// ==========================================================
class VuMeterModule : public ModulePanel,
                     public AnalyserHub::FrameListener,
                     private juce::Timer
{
public:
    explicit VuMeterModule(AnalyserHub& hub);
    ~VuMeterModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // ---- 持久化 + 控制面板（v3.1）----
    //   dialStyle:   0 半圆（旧外观）/ 1 弧形（240° 圆表，深色面）/ 2 梯形（顶边直线刻度）
    //   needleStyle: 0 Line（旧外观）/ 1 Wedge（锥形）/ 2 Stereo（L、R 双指针）
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState (const juce::ValueTree& state) override;
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

    // ---- Toybox（v3.0.0）：玻璃裂 + 迪斯科灯（§8.2）----
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

protected:
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // 60Hz 动画 Tick：按真实 dt 推进 displayedL/R 朝目标 targetL/R 插值，
    //   同时推进 LED 跟随值与脉动相位，并 repaint()。
    //   这样即便后端 Loudness 快照仍按 400ms 粗颗粒更新，前端每帧都在
    //   做子步插值，指针看起来是连续扫过的。
    void timerCallback() override;

    // 画一条指针（几何取自最近一次 drawDial 的结果，三种表盘自适应）
    void drawNeedle (juce::Graphics& g,
                     float              valueDb,       // 当前 dBFS 值
                     juce::Colour       colour,
                     float              thickness,
                     bool               wedge = false) const;

    // ---- 控制面板参数（构造默认 = 旧外观）----
    int  dialStyle   = 0;
    int  needleStyle = 0;
    bool showLed     = true;

    // 画表盘底色 + 刻度 + 数字（三种结构完全不同的表盘）
    void drawDial   (juce::Graphics& g,
                     juce::Rectangle<int> dialArea,
                     juce::Point<float>&  outPivot,
                     float&               outRadius) const;
    void drawArcDial        (juce::Graphics& g, juce::Rectangle<int> dialArea,
                             juce::Point<float>& outPivot, float& outRadius) const;
    void drawTrapezoidDial  (juce::Graphics& g, juce::Rectangle<int> dialArea,
                             juce::Point<float>& outPivot, float& outRadius) const;

    // dB → 表盘归一化位置 [0,1]（minDisplayDb … maxDisplayDb）
    float dialTFor (float db) const noexcept;
    // 半圆 / 弧形：指针角度（弧度，0 = 正上，顺时针为正）；梯形按尖端位置反算
    float needleAngleFor  (float db) const noexcept;
    // 当前样式下指针长度（像素）
    float needleLengthFor (float db) const noexcept;

    // 最近一次 drawDial 产出的几何（指针 / 玩具虚影共用）
    struct DialGeom
    {
        juce::Point<float> pivot { 0.0f, 0.0f };
        float radius  = 0.0f;    // 半圆 / 弧形：指针长度基准
        // 楔形刻度（样式 2）：刻度带的左右端、刻度线起点与最长刻度
        float leftX    = 0.0f;
        float rightX   = 0.0f;
        float tickTopY = 0.0f;
        float tickLenHi = 0.0f;
    };
    mutable DialGeom dialGeom;

    // 楔形刻度：dB → 横向位置 / 刻度线长度（左长右短，警戒段再抬高一档）
    float wedgeXFor         (float db) const noexcept;
    float wedgeTickLengthFor(float db) const noexcept;
    float wedgeBaselineY    (float db) const noexcept;

    // 画 LED 信号灯（位置见 ledBounds）
    void drawLed    (juce::Graphics& g,
                     juce::Rectangle<int> ledArea) const;
    // LED 位置：半圆 / 弧形放右上角（盘面顶部是空的）；楔形刻度占满顶部，
    //   灯改放右下角，避免压住刻度数字。绘制与玩具点击共用这一份。
    juce::Rectangle<int> ledBounds(juce::Rectangle<int> area) const;

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
    double lastOnFrameMs = 0.0;          // v2.9：最近一次 onFrame 时间（兜底 Timer 判定用）
    void advanceBallistics (float dtMs); // v2.9：弹道 / LED / 相位推进 + repaint

    // 脉动相位（用于 LED 呼吸/脉动，也由 Timer 推进）
    float pulsePhase = 0.0f;

    // 瞬时 RMS 的样本窗口长度（毫秒）。20ms 在典型 44.1/48 kHz 下约
    //   880/960 样本，远小于 oscilloscopeBufferSize=2048，够用。
    static constexpr float instantRmsWindowMs = 20.0f;

    // 非对称弹道的时间常数（毫秒）
    //   · 上升慢一点避免单样本尖峰把指针推到极值；取 80ms
    //   · 下降保留经典 VU 机械回落感，取 1200ms —— 连续瞬态时由上升主导，
    //     下降不会锁死指针；静音后也能缓慢落回
    //   · 反馈修复（外网 #6）：静音（target=-144）时用更慢的独立时间常数，
    //     避免"没信号的一瞬间表针打到底"的观感
    static constexpr float tauRiseMs     = 80.0f;
    static constexpr float tauFallMs     = 1200.0f;
    static constexpr float tauMuteFallMs = 1800.0f;

    // 刻度显示上下界（dBFS） —— 均匀等距从 -36 到 +3
    static constexpr float minDisplayDb = -36.0f;
    static constexpr float maxDisplayDb =  +3.0f;

    // LED 绿灯点亮阈值（dBFS） —— 与表底刻度 minDisplayDb 解耦。
    //   表底已扩展到 -36 以便捕捉更低瞬态，但 LED 仍在 -25 才点亮，
    //   避免 -25~-36 之间的微弱底噪导致绿灯常亮。
    static constexpr float ledSignalDbfs = -25.0f;

    // 红色警戒段的起点（dBFS） —— 也是 LED 变红的同一个阈值
    //   0 dBFS = 数字满刻度，到达即开始削波风险
    static constexpr float warnDbfs     =   0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VuMeterModule)
};
