#pragma once

#include <JuceHeader.h>

#include "source/analysis/AnalyserHub.h"
#include "source/ui/ModuleWorkspace.h"

// ==========================================================
// RecorderModule —— 滚动录音机模块（Pink XP 像素风）
//
//   ┌─────────────────────────────────┐
//   │ RECORDER                  ● REC │   ← 顶部状态行：录制中亮红闪烁，空闲暗灰
//   │                                 │
//   │            ▪▪▪▪▪                │   ← 纯像素红点（无底座，随模块缩放）
//   │          ▪▪     ▪▪              │      未录制 = 发光；按下 = 变暗下沉一格；
//   │            ▪▪▪▪▪                │      录制中 = 轻微发光 + 轻微变暗 + 下沉半格
//   │                                 │      按下即切换：开始录制 / 结束并保存
//   │           00:00:12              │   ← 本次录制时长（HH:MM:SS，空闲为 00:00:00）
//   │                                 │
//   │  [=====|=======]       10 s     │   ← 拖动条 1–60 s
//   │  ┌───────────────────────────┐  │
//   │  │      SAVE LAST 10 s       │  │   ← 导出按下按钮前 N 秒
//   │  └───────────────────────────┘  │
//   │  SAVED: Y2Kmeter_clip_10_...wav │
//   └─────────────────────────────────┘
//
//   两条互相独立的保存路径：
//     1) 红点手动录制：点一下开始（亮红 + 计时），再点一下停止并保存为
//        Y2Kmeter_recording_<yyyy-MM-dd_HH-mm-ss>.wav（采集线程流式写盘；
//        单段最长 1 小时，到点自动停止写入并保存，状态灯显示 SAVED 1H）；
//     2) SAVE LAST N：从常驻的 60 s 滚动缓冲里抓最近 N 秒，保存为
//        Y2Kmeter_clip_<N秒>_<yyyy-MM-dd_HH-mm-ss>.wav。
//
//   共同点：都取 pre-gain 原始输入（忽略用户 gain）、32-bit float WAV、保存后自动
//   打开保存目录（已打开则不重复开窗）。
//
//   滚动采集与模块无关：软件一打开（插件实例一创建）后端就开始滚动存储声音，
//   模块只负责展示状态与触发保存；模块被移除 / 界面关闭都不影响采集。
// ==========================================================
class RecorderModule : public ModulePanel,
                       private juce::Timer
{
public:
    explicit RecorderModule (AnalyserHub& hub);
    ~RecorderModule() override;

    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

protected:
    void layoutContent (juce::Rectangle<int> contentBounds) override;

private:
    // 所有行切片的唯一来源（paint / layout / 命中测试共用，避免位置漂移）
    struct Rects
    {
        juce::Rectangle<int> header, footer, button, slider, dot, time, value;
    };

    Rects computeRects (juce::Rectangle<int> contentBounds) const;
    int  computeDotCell (juce::Rectangle<int> area) const;
    juce::Rectangle<int> computeDotSquare (juce::Rectangle<int> area) const;
    juce::Rectangle<int> getDotHitBounds() const;

    void timerCallback() override;

    void saveNow();
    void toggleTake();
    void updateButtonText();

    // 纯像素圆点（没有底座），三种状态全靠圆点自身表达：
    //   · 未录制     ：发光（像素阶梯光晕）、正常亮度、不下沉
    //   · 按钮按下   ：不发光、变暗、下沉一格（无论按下是开始还是结束录制）
    //   · 录制中     ：轻微发光、轻微变暗、下沉半格（松手后的保持态）
    void paintPixelDot (juce::Graphics& g, juce::Rectangle<int> area,
                        bool pressed, bool taking) const;

    AudioRecorder& recorder;

    juce::Slider     durationSlider;
    juce::TextButton saveButton;

    double       selectedSeconds = 10.0;
    double       lastTakeSeconds = -1.0;
    juce::String lastStatus;
    bool         lastTaking      = false;

    bool dotPressed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecorderModule)
};
