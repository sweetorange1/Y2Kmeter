#include "AudioDumpRecorder.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr const char* kEnableEnv = "Y2K_AUDIO_DUMP";
constexpr const char* kDirEnv = "Y2K_AUDIO_DUMP_DIR";

juce::String routeFileName (AudioDumpRecorder::Route route)
{
    return route == AudioDumpRecorder::Route::microphone ? "mic_f32le.raw" : "output_f32le.raw";
}

juce::String routeLabel (AudioDumpRecorder::Route route)
{
    return route == AudioDumpRecorder::Route::microphone ? "microphone" : "output";
}

static double safeRmsDbfs (double sumSq, uint64_t n)
{
    if (n == 0) return -160.0;
    const double rms = std::sqrt (sumSq / (double) n);
    if (rms <= 1.0e-12) return -160.0;
    return 20.0 * std::log10 (rms);
}
}

AudioDumpRecorder& AudioDumpRecorder::instance()
{
    static AudioDumpRecorder g;
    return g;
}

void AudioDumpRecorder::configureFromEnvironment()
{
    const juce::SpinLock::ScopedLockType g (lock);
    if (configured)
        return;

    configured = true;

    const juce::String flag = juce::SystemStats::getEnvironmentVariable (kEnableEnv, {}).trim();
    const bool on = flag == "1" || flag.equalsIgnoreCase ("true") || flag.equalsIgnoreCase ("yes");
    enabled.store (on, std::memory_order_release);
    if (! on)
        return;

    const juce::String customDir = juce::SystemStats::getEnvironmentVariable (kDirEnv, {}).trim();
    if (customDir.isNotEmpty())
        sessionDir = juce::File (customDir);
    else
        sessionDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile ("Y2Kmeter_AudioDump");

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    sessionDir = sessionDir.getChildFile ("session_" + stamp);
    sessionDir.createDirectory();
}

void AudioDumpRecorder::ensureSessionPrepared()
{
    if (! configured)
        configureFromEnvironment();

    if (! enabled.load (std::memory_order_acquire))
        return;

    if (! sessionDir.isDirectory())
        sessionDir.createDirectory();

    if (micState.stream == nullptr)
    {
        micState.fileName = routeFileName (Route::microphone);
        const auto file = sessionDir.getChildFile (micState.fileName);
        micState.stream = file.createOutputStream();
    }

    if (outState.stream == nullptr)
    {
        outState.fileName = routeFileName (Route::output);
        const auto file = sessionDir.getChildFile (outState.fileName);
        outState.stream = file.createOutputStream();
    }

}

void AudioDumpRecorder::writeStream (StreamState& state,
                                     const float* left,
                                     const float* right,
                                     int numSamples,
                                     double sampleRate)
{
    if (state.stream == nullptr || left == nullptr || right == nullptr || numSamples <= 0)
        return;

    juce::HeapBlock<float> interleaved ((size_t) numSamples * 2u, true);
    auto* dst = interleaved.getData();

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right[i];
        dst[(size_t) i * 2u]     = l;
        dst[(size_t) i * 2u + 1] = r;

        const double al = std::abs ((double) l);
        const double ar = std::abs ((double) r);
        state.stats.peakAbs = std::max (state.stats.peakAbs, std::max (al, ar));
        state.stats.sumSqL += (double) l * (double) l;
        state.stats.sumSqR += (double) r * (double) r;

        if (al >= 0.999f) ++state.stats.clippedSamples;
        if (ar >= 0.999f) ++state.stats.clippedSamples;
    }

    state.stats.frames += (uint64_t) numSamples;
    if (sampleRate > 0.0)
        state.stats.sampleRate = sampleRate;

    state.stream->write (dst, (size_t) numSamples * 2u * sizeof (float));
}

void AudioDumpRecorder::push (Route route,
                              const float* left,
                              const float* right,
                              int numSamples,
                              double sampleRate)
{
    if (! enabled.load (std::memory_order_acquire))
        return;

    const juce::SpinLock::ScopedLockType g (lock);
    ensureSessionPrepared();
    if (! enabled.load (std::memory_order_acquire))
        return;

    if (route == Route::microphone)
        writeStream (micState, left, right, numSamples, sampleRate);
    else
        writeStream (outState, left, right, numSamples, sampleRate);
}

juce::File AudioDumpRecorder::getSessionDirectory() const
{
    const juce::SpinLock::ScopedLockType g (lock);
    return sessionDir;
}

void AudioDumpRecorder::writeSummaryToDisk() const
{
    if (! sessionDir.isDirectory())
        return;

    juce::String s;
    s << "audio_dump_version=1\n";

    auto append = [&s] (const StreamState& st, const char* key)
    {
        const double dbL = safeRmsDbfs (st.stats.sumSqL, st.stats.frames);
        const double dbR = safeRmsDbfs (st.stats.sumSqR, st.stats.frames);
        s << key << ".file=" << st.fileName << "\n";
        s << key << ".frames=" << (juce::int64) st.stats.frames << "\n";
        s << key << ".samplerate=" << st.stats.sampleRate << "\n";
        s << key << ".peak_abs=" << st.stats.peakAbs << "\n";
        s << key << ".clipped_samples=" << (juce::int64) st.stats.clippedSamples << "\n";
        s << key << ".rms_dbfs_L=" << dbL << "\n";
        s << key << ".rms_dbfs_R=" << dbR << "\n";
    };

    append (micState, "microphone");
    append (outState, "output");

    const auto summary = sessionDir.getChildFile ("summary.txt");
    summary.replaceWithText (s, false, false, "\n");
}

void AudioDumpRecorder::flushSummary()
{
    const juce::SpinLock::ScopedLockType g (lock);
    if (! enabled.load (std::memory_order_acquire))
        return;

    if (micState.stream != nullptr) micState.stream->flush();
    if (outState.stream != nullptr) outState.stream->flush();
    writeSummaryToDisk();
}
