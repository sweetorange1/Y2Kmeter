#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <atomic>
#include <memory>

namespace y2k
{

class MacDesktopAudioCapture
{
public:
    struct Impl;

    MacDesktopAudioCapture();
    ~MacDesktopAudioCapture();

    using AudioCallback = std::function<void (const float* left,
                                              const float* right,
                                              int         numSamples,
                                              double      sampleRate)>;
    AudioCallback onAudio;

    bool start();
    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }
    const juce::String& getLastError() const noexcept { return lastError; }

private:
    std::unique_ptr<Impl> impl;

    std::atomic<bool> running { false };
    juce::String      lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MacDesktopAudioCapture)
};

} // namespace y2k
