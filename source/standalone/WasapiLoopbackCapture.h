// ==========================================================
// WasapiLoopbackCapture.h
//   直接使用 Windows Core Audio（WASAPI）采集"默认播放端点"的系统混音输出
//   （即电脑当前外放到扬声器/耳机的那股声音），绕开 JUCE 的 AudioDeviceManager。
//
//   为什么要自己写：
//     JUCE 8 内建 WASAPI 只有 shared / exclusive / sharedLowLatency 三种模式，
//     都是面向"从输入端点采集"或"向渲染端点播放"的常规流，
//     没有提供 AUDCLNT_STREAMFLAGS_LOOPBACK 的 render-endpoint-loopback 捕获模式。
//     所以我们用裸 WASAPI 实现一个极简的 loopback 采集器。
//
//   用法：
//       WasapiLoopbackCapture cap;
//       cap.onAudio = [](const float* L, const float* R, int n, double sr){ ... };
//       if (cap.start()) { ... cap.stop(); }
//
//   注意：
//     · 输出统一为立体声 float32（若系统端点是 mono 则 L=R；若多声道则只取前两个）
//     · sampleRate 以系统混音器的格式为准（常见 44100/48000）
//     · 该类仅在 Windows 上可用；其他平台构造即失败 start() 返回 false
// ==========================================================

#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <atomic>
#include <memory>

namespace y2k
{

class WasapiLoopbackCapture
{
public:
    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture();

    // 音频数据回调：在采集线程里调用。
    //   left  / right : 两路平面 float32 PCM（长度均为 numSamples）
    //   numSamples    : 本次数据的采样数（帧数）
    //   sampleRate    : 当前 mix format 的采样率
    using AudioCallback = std::function<void (const float* left,
                                              const float* right,
                                              int         numSamples,
                                              double      sampleRate)>;
    AudioCallback onAudio;

    // 开启采集。失败返回 false（平台不支持、找不到默认端点、格式不支持等均返回 false）。
    bool start();

    // 停止采集并回收资源。多次调用是安全的。
    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    // 最近一次 start() 失败时的可读错误描述（供上层展示）
    const juce::String& getLastError() const noexcept { return lastError; }

private:
    // 采集线程实体（按平台选择实现）
    void runCaptureThread();

    std::atomic<bool>           running { false };
    std::atomic<bool>           shouldStop { false };
    std::unique_ptr<std::thread> captureThread;

    juce::String                lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiLoopbackCapture)
};

} // namespace y2k
