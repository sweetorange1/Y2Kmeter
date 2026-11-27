// ==========================================================
// WasapiLoopbackCapture.h
//   直接使用 Windows Core Audio（WASAPI）做回环采集，绕开 JUCE 的 AudioDeviceManager。
//   两种采集目标：
//
//   1) 整机输出（默认）：采集"默认播放端点"的系统混音输出
//      （即电脑当前外放到扬声器/耳机的那股声音）。
//
//   2) 定向程序（setTargetProcess）：只采集某一个程序（连同它的子进程）发出的声音，
//      其他程序的声音不进来 —— 与 OBS 的 "Application Audio Capture" 同一套系统能力
//      （进程级回环，Windows 10 2004+）。
//        · 目标程序没在运行时不算失败：采集器进入"等待"状态并持续输出静音，
//          程序启动后自动接上；程序退出后回到等待，重新启动又会自动接上。
//
//   为什么要自己写：
//     JUCE 8 内建 WASAPI 只有 shared / exclusive / sharedLowLatency 三种模式，
//     都是面向"从输入端点采集"或"向渲染端点播放"的常规流，
//     没有提供 AUDCLNT_STREAMFLAGS_LOOPBACK 的 render-endpoint-loopback 捕获模式，
//     更没有进程级回环。所以我们用裸 WASAPI 实现一个极简的 loopback 采集器。
//
//   用法：
//       WasapiLoopbackCapture cap;
//       cap.onAudio = [](const float* L, const float* R, int n, double sr){ ... };
//       cap.setTargetProcess ("chrome.exe");   // 可选；不调用 = 整机输出
//       if (cap.start()) { ... cap.stop(); }
//
//   注意：
//     · 输出统一为立体声 float32（若系统端点是 mono 则 L=R；若多声道则只取前两个）
//     · sampleRate 以系统混音器的格式为准（常见 44100/48000）；
//       定向程序模式下同样取默认播放端点的采样率，两种模式来回切不需要重新 prepare
//     · 该类仅在 Windows 上可用；其他平台构造即失败 start() 返回 false
// ==========================================================

#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <atomic>
#include <memory>
#include <thread>

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

    // 设定采集目标程序（exe 文件名，如 "chrome.exe"；空串 = 整机输出）。
    //   只能在未运行时调用（先 stop() 再改目标再 start()）；运行中调用会被忽略。
    void setTargetProcess (const juce::String& exeName);
    const juce::String& getTargetProcess() const noexcept { return targetExeName; }

    // 开启采集。失败返回 false（平台不支持、找不到默认端点、格式不支持等均返回 false）。
    //   定向程序模式：目标程序没在运行**不算失败**（返回 true，进入等待）；
    //   只有系统不支持进程级回环、或已找到程序却建不起采集流时才返回 false。
    bool start();

    // 停止采集并回收资源。多次调用是安全的。
    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    // 最近一次 start() 失败时的可读错误描述（供上层展示）
    const juce::String& getLastError() const noexcept { return lastError; }

private:
    // 采集线程实体（按平台选择实现）
    void runCaptureThread();

   #if JUCE_WINDOWS
    // 采集线程的两种目标
    void runEndpointCapture();
    void runProcessCapture();

    // 采集线程在退出前调用：记录错误并置 failed（start() 靠 failed 判定，
    //   避免主线程在采集线程写 lastError 的同时去读它）
    void fail (const juce::String& message);
   #endif

    std::atomic<bool>           running { false };
    std::atomic<bool>           shouldStop { false };
    std::atomic<bool>           failed { false };
    std::unique_ptr<std::thread> captureThread;

    juce::String                targetExeName;   // 空 = 整机输出
    juce::String                lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiLoopbackCapture)
};

} // namespace y2k
