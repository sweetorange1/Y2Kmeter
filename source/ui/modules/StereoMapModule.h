#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/GlDirectModule.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// StereoMapModule
//   Compare L/R spectrum and map per-frequency stereo position:
//   * top   = left channel stronger
//   * center= balanced
//   * bottom= right channel stronger
// ==========================================================
class StereoMapModule : public GlDirectModuleBase,
                        public AnalyserHub::FrameListener
{
public:
    explicit StereoMapModule (AnalyserHub& hub);
    ~StereoMapModule() override;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

protected:
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    void computeStereoMap (float dtSec);
    void drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawCurve (juce::Graphics& g, juce::Rectangle<int> plot) const;

    static float freqAt (float norm01) noexcept;
    static float normFromFreq (float freqHz) noexcept;
    static float xForNorm (float norm01, juce::Rectangle<int> plot) noexcept;

    juce::Rectangle<int> getPlotBounds (juce::Rectangle<int> canvas) const;

    AnalyserHub& hub;

    juce::Array<float> magsL;
    juce::Array<float> magsR;

    std::vector<float> panByBand;
    std::vector<float> strengthByBand;

    int cachedBandCount = 0;

    static constexpr float kMinFreq = 20.0f;
    static constexpr float kMaxFreq = 20000.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoMapModule)
};

