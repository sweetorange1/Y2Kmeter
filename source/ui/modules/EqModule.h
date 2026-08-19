#ifndef PBEQ_EQ_MODULE_H_INCLUDED
#define PBEQ_EQ_MODULE_H_INCLUDED

#include <JuceHeader.h>
#include "source/ui/ModulePanel.h"
#include "source/analysis/AnalyserHub.h"
#include "PluginProcessor.h"

// ==========================================================
// EqModule —— 像素拼豆频谱（Spectrum Perlerbeads）
// 该模块内部持有：
//   * PixelEqGraph：像素网格频谱绘制（Y2K 装饰）
//   * FreqRangeSlider：一个滑轨 + 两个滑块（TwoValueHorizontal），
//     分别控制显示频谱的最低频率 / 最高频率
//   * CellSizeSlider：格子大小纵向滑条
// ==========================================================

class EqModule : public ModulePanel
{
public:
    explicit EqModule(Y2KmeterAudioProcessor& processor);
    ~EqModule() override;

    // v1.8.4 持久化：保存/恢复 cellSize（格子大小滑条值）
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState(const juce::ValueTree& state) override;

    // 右键模块区域 → 弹出"添加模块"选择器（子组件 eqGraph 会拦截鼠标，
    //   需要在此显式处理右键，与 Milkdrop / Tamagotchi 的做法一致）
    void mouseDown(const juce::MouseEvent& e) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;
    void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // ---- 子组件 ----
    class PixelEqGraph : public juce::Component,
                         public AnalyserHub::FrameListener
    {
    public:
        explicit PixelEqGraph(Y2KmeterAudioProcessor& p);
        ~PixelEqGraph() override;

        // Phase F：Hub 分发器回调
        void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

        void setFreqRange(float minHz, float maxHz);
        float getMinFreqHz() const noexcept { return minFreqHz; }
        float getMaxFreqHz() const noexcept { return maxFreqHz; }

        void setCellSize(int newSize);
        int  getCellSize() const noexcept { return cellSize; }

        void paint(juce::Graphics&) override;
        void visibilityChanged() override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;

    private:
        Y2KmeterAudioProcessor& processor;
        juce::Array<float> spectrumSnapshot;
        juce::Array<float> smoothedSpectrum;
        juce::Array<float> peakSpectrum;
        float minFreqHz = 20.0f;
        float maxFreqHz = 20000.0f;
        int cellSize = 4;

        // 鼠标悬停标尺状态
        juce::Point<int> hoverPos;
        bool             hoverActive = false;

        // 性能优化（阶段1）：UI 侧 repaint 节流，避免 Hub 回调时每帧都整块重绘。
        double lastRepaintMs = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PixelEqGraph)
    };

    Y2KmeterAudioProcessor& processor;

    PixelEqGraph   eqGraph;

    juce::Slider freqRangeSlider;  // 双滑块：最低/最高显示频率
    juce::Label  freqRangeLabel;   // 常驻显示 Hz 数值

    juce::Slider cellSizeSlider;
    juce::Label  cellSizeLabel { {}, "SIZE" };

    void updateFreqLabel();

    // 主题订阅 token：切换主题时重新下发 cellSizeLabel 的 textColourId，
    //   避免 JUCE Label 在构造时缓存的 PinkXP::ink 颜色在切主题后失效导致
    //   "SIZE" 文字颜色与标题栏不一致。
    int themeSubToken = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqModule)
};

#endif // PBEQ_EQ_MODULE_H_INCLUDED