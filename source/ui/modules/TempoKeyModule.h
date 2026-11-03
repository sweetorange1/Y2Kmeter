#pragma once

#include <JuceHeader.h>

#include "source/analysis/AnalyserHub.h"
#include "source/ui/ModuleWorkspace.h"

// ==========================================================
// TempoKeyModule —— 实时节拍（BPM）+ 调性（Key）显示
//
// 数据源：AnalyserHub::Kind::MusicAnalysis
//   · 音频线程 → 无锁环形缓冲 → 后台分析线程（TempoEstimator / KeyEstimator）
//   · Hub 的 FrameDispatcher 把 Result 拷进 FrameSnapshot，本模块只读快照，
//     绝不直接碰分析线程状态。
//
// UI 约定：
//   · 结果未就绪 → 显示 "--" 并标注 ANALYSING（不提前给出看似确定的数字）；
//   · 静音 → 状态 NO SIGNAL，保留上一次结果但明显变暗（旧结果 ≠ 当前结果）；
//   · 每列下方显示置信度（细条 + 百分比）。这个数字是**启发式稳定性指标**，
//     不是标定过的概率：公式集中在 TempoEstimator::updateConfidence() /
//     KeyEstimator::updateKey()，并且乘了"数据充分度"，所以刚开始听时天然偏低；
//     < 50% 时数值半暗并标注 (provisional)。
//   · 调性显示：主音+大小调（大字）+ 紧随其下的关系调（rel. X minor/major，
//     字号约为主数值的 0.6，而不是原来 8px 的小字）。
//     关系调 = 与当前调共享完全相同一组音、只是主音不同的调（C major ↔ A minor）；
//     自然小调是默认值不再标注，只有和声/旋律小调才写限定词（它们没有关系调）。
//     拼写按调式选常用形式（大调 Db 而不是 C#，小调 Eb 而不是 D#），
//     关系调的拼写跟随主调调号侧（Eb minor → Gb major，不是 F# major）。
//   · 所有字号都按"实际文字宽度"测量后收缩（fitTextHeight），
//     缩到最小尺寸也不会把 minor/major 的末尾字母裁掉。
//   · 点击 BPM 列 → 气泡显示 x2 / /2 的换算；点击 KEY 列 → 气泡显示该调式
//     的 7 个音名。两列都拦截了点击，因此不会再触发通用的 ✨ 兜底粒子
//     （以前这些小 emoji 会落到置信度条附近停住）。
// ==========================================================
class TempoKeyModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    explicit TempoKeyModule (AnalyserHub& hub);
    ~TempoKeyModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // ---- Toybox：点击 BPM 列 / KEY 列弹数据气泡（覆盖通用 ✨ 兜底）----
    //   BPM 列 → "x2 = 240.0  /2 = 60.0"
    //   KEY 列 → 当前调式的 7 个音名（按调号正确拼写）
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    void layoutContent (juce::Rectangle<int> contentBounds) override;

private:
    // 绘制与点击命中共用同一套几何，避免两边算出来的列位置不一致
    struct LayoutRects
    {
        juce::Rectangle<int> bpmCol;
        juce::Rectangle<int> keyCol;
        juce::Rectangle<int> statusRow;
    };

    LayoutRects computeLayout (juce::Rectangle<int> contentBounds) const;

    // 该调式的 7 个音名（含正确升降号），例如 Eb minor → "Eb F Gb Ab Bb Cb Db"
    static juce::String scaleNoteNames (int root, y2k::music::Mode mode);

    void refreshButtons();
    void onStartStopClicked();
    void onResetClicked();

    static juce::String formatBpm (const y2k::music::Result& r);
    static juce::String formatKey (const y2k::music::Result& r);
    static juce::String formatAge (double updatedMs);
    static juce::Colour statusColour (y2k::music::Status s);

    AnalyserHub& hub;

    y2k::music::Result latest;
    double lastRepaintMs = 0.0;

    juce::TextButton btnStartStop { "STOP" };
    juce::TextButton btnReset     { "RESET" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoKeyModule)
};
