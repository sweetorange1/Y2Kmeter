#pragma once

#include <JuceHeader.h>
#include <atomic>

class AudioDumpRecorder
{
public:
    enum class Route
    {
        microphone,
        output
    };

    static AudioDumpRecorder& instance();

    void configureFromEnvironment();
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_acquire); }

    void push (Route route,
               const float* left,
               const float* right,
               int numSamples,
               double sampleRate);

    void flushSummary();
    juce::File getSessionDirectory() const;

private:
    AudioDumpRecorder() = default;

    struct StreamStats
    {
        uint64_t frames = 0;
        uint64_t clippedSamples = 0;
        double peakAbs = 0.0;
        double sumSqL = 0.0;
        double sumSqR = 0.0;
        double sampleRate = 0.0;
    };

    struct StreamState
    {
        std::unique_ptr<juce::FileOutputStream> stream;
        StreamStats stats;
        juce::String fileName;
    };

    void ensureSessionPrepared();
    void writeStream (StreamState& state,
                      const float* left,
                      const float* right,
                      int numSamples,
                      double sampleRate);
    void writeSummaryToDisk() const;

    mutable juce::SpinLock lock;
    std::atomic<bool> enabled { false };
    bool configured = false;

    juce::File sessionDir;
    StreamState micState;
    StreamState outState;
};
