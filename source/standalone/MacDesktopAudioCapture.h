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
    // shared_ptr：ScreenCaptureKit 的异步 completion 会持有 Impl 的引用，
    // 避免 stop()/析构后迟到的回调访问已销毁的 Impl（悬垂指针）。
    std::shared_ptr<Impl> impl;

    std::atomic<bool> running { false };
    juce::String      lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MacDesktopAudioCapture)
};

} // namespace y2k
