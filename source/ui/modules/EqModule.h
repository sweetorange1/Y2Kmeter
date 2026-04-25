#ifndef PBEQ_EQ_MODULE_H_INCLUDED
#define PBEQ_EQ_MODULE_H_INCLUDED

#include <JuceHeader.h>
#include "source/ui/ModulePanel.h"
#include "source/analysis/AnalyserHub.h"
#include "PluginProcessor.h"

// ==========================================================
// EqModule —— 原 Pink XP 像素 EQ 图形组件的模块化封装
// 该模块内部持有：
//   * PixelEqGraph：像素网格频谱绘制（Y2K 装饰）
//   * BandSelector：LOW/MID/HIGH 按钮组
//   * CellSizeSlider：格子大小纵向滑条
// ==========================================================

class EqModule : public ModulePanel
{
public:
    explicit EqModule(Y2KmeterAudioProcessor& processor);
    ~EqModule() override;

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

        enum class Band { low, mid, high };
        void setActiveBand(Band b);
        Band getActiveBand() const noexcept { return activeBand; }

        void setCellSize(int newSize);
        int  getCellSize() const noexcept { return cellSize; }

        void paint(juce::Graphics&) override;
        void visibilityChanged() override;

    private:
        Y2KmeterAudioProcessor& processor;
        juce::Array<float> spectrumSnapshot;
        juce::Array<float> smoothedSpectrum;
        juce::Array<float> peakSpectrum;
        Band activeBand = Band::mid;
        int cellSize = 10;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PixelEqGraph)
    };

    class BandSelector : public juce::Component
    {
    public:
        BandSelector();
        void setSelectedIndex(int newIndex, bool notify = true);
        int  getSelectedIndex() const noexcept { return selectedIndex; }

        std::function<void(int)> onSelectionChanged;

        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp  (const juce::MouseEvent&) override;

    private:
        int selectedIndex = 1;
        bool isDragging = false;
        juce::StringArray labels { "LOW", "MID", "HIGH" };
        int getIndexForX(float x) const;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BandSelector)
    };

    Y2KmeterAudioProcessor& processor;

    PixelEqGraph   eqGraph;
    BandSelector   bandSelector;

    juce::Slider cellSizeSlider;
    juce::Label  cellSizeLabel { {}, "SIZE" };

    // 主题订阅 token：切换主题时重新下发 cellSizeLabel 的 textColourId，
    //   避免 JUCE Label 在构造时缓存的 PinkXP::ink 颜色在切主题后失效导致
    //   "SIZE" 文字颜色与标题栏不一致。
    int themeSubToken = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqModule)
};

#endif // PBEQ_EQ_MODULE_H_INCLUDED