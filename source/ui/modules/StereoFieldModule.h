#pragma once

#include <JuceHeader.h>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// StereoFieldModule —— Pink XP 像素风"声相指示"（半圆雷达）
//
// 功能概述：
//   * 订阅 AnalyserHub::Kind::Oscilloscope（复用 Oscilloscope 同一路
//     立体声 2048 样本）—— 后端音频线程零新增计算。
//   * 视觉：半圆形雷达（圆心在底部，圆弧朝上）。将立体声样本映射到
//     半圆内的"声像位置"：
//        电平 peak = max(|L|, |R|)            → 决定径向距离（越响越靠外）
//        平衡 balance = (|R| - |L|) / (|L|+|R|) → 决定方向（偏左/偏右）
//     因此：
//        纯左声道 → 恒在左水平线（直径线左端，9 点方向）
//        纯右声道 → 恒在右水平线（直径线右端，3 点方向）
//        居中单声道 → 恒在顶部垂线上
//     采用幅度（极性无关）而非瞬时样本，保证"纯左/纯右"不随信号正负
//     半周左右摆动，符合声像指示（stereo position）的语义。
//   * 渐隐残影：新样本点累积到离屏 trail 图像，每帧按固定系数衰减 alpha，
//     旧点逐渐变淡直至趋向模块底色（而非瞬时消失）。
//   * 固定比例尺：样本能量 [0,1] 直接线性映射到半圆半径 R，
//     不做随信号强度的实时自动缩放，整个图表比例尺恒定。
//
// 数据源：AnalyserHub::getLatestFrame() → frame.oscL / frame.oscR
// ==========================================================

class StereoFieldModule : public ModulePanel,
                          public AnalyserHub::FrameListener
{
public:
    explicit StereoFieldModule (AnalyserHub& hub);
    ~StereoFieldModule() override;

    // AnalyserHub::FrameListener
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // 雷达几何参数（圆心X / 底线Y / 半径 / 侧边留白 / 动态字号）
    struct RadarGeometry
    {
        int   cx            = 0;
        int   baseY         = 0;
        int   radius        = 0;
        int   sideMargin    = 0;
        float labelFontSize = 9.0f;
    };

    // 计算雷达几何：左右各固定预留 10px 侧边留白（让直径端点贴近仪表区边界），
    // 并依据模块尺寸推导标签字号（小模块字号小、大模块字号大）。
    static RadarGeometry computeRadarGeometry (const juce::Rectangle<int>& plot);

    // 绘制半圆底衬（圆弧、直径线、45° 参考线、同心标尺环、
    // 能量 dB 刻度、L/C/R 声像方位刻度）
    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;

    // 更新残影：1) 衰减旧点 alpha  2) 绘制当前快照的新点
    void updateTrail (juce::Rectangle<int> canvas);
    void invalidateTrail();

    AnalyserHub& hub;

    // 快照（UI 线程独占使用）
    juce::Array<float> snapshotL;
    juce::Array<float> snapshotR;

    // 渐隐残影离屏图像（ARGB 预乘；每帧 multiplyAlpha 衰减）
    juce::Image trailImage;
    bool  trailNeedsUpdate = true;

    // UI 侧 repaint 节流
    double lastRepaintMs = 0.0;
    int    themeSubToken = -1;

    // 每帧 alpha 衰减系数（越大尾巴越长；1.0 = 永不消失）
    static constexpr float kDecayPerFrame = 0.92f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoFieldModule)
};
