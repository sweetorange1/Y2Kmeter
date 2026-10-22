// ==========================================================
// LinuxMonitorCapture.h
//   Linux 下采集"系统正在播放的声音"（默认输出设备的 monitor 源）。
//
//   为什么用外部进程：
//     · Windows 用 WASAPI loopback（见 WasapiLoopbackCapture），macOS 用
//       ScreenCaptureKit；Linux 这边没有跨桌面环境通用的 loopback API。
//     · PipeWire（及其 pulse 兼容层）已经暴露了 <sink>.monitor 源，
//       最省事、最稳的方式是 fork/exec pw-record 或 parec，从它们的
//       stdout 管道里读原始 f32 PCM。
//
//   用法：
//       LinuxMonitorCapture cap;
//       cap.onAudio = [](const float* L, const float* R, int n, double sr){ ... };
//       if (cap.start()) { ... cap.stop(); }
//
//   注意：
//     · 输出统一为 48000 Hz / 2ch / float32 平面数据（由子进程重采样）
//     · onAudio 在采集线程里调用，回调里不要再调 start()/stop()
//     · 不依赖 JUCE，仅 C++17 标准库 + POSIX
// ==========================================================

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace y2k
{

class LinuxMonitorCapture
{
public:
    LinuxMonitorCapture();
    ~LinuxMonitorCapture();

    // 采集线程里回调；left/right 为平面 float32，numSamples 为帧数
    using AudioCallback = std::function<void (const float* left, const float* right,
                                              int numSamples, double sampleRate)>;
    AudioCallback onAudio;

    bool start();          // 失败返回 false，错误写 lastError
    void stop();           // 幂等，同步 join
    bool isRunning() const noexcept;
    const std::string& getLastError() const noexcept;

    static bool isSupported();   // 系统上是否存在 pw-record 或 parec

private:
    void runCaptureThread();

    std::atomic<bool> running { false };
    std::atomic<bool> shouldStop { false };
    std::atomic<bool> threadExited { true };

    // 子进程 pid 由采集线程写入、stop() 读取（用于 SIGTERM）。
    // 管道 fd 只由采集线程持有和关闭，避免跨线程 close() 的 fd 复用竞态。
    std::atomic<int> childPid { -1 };

    std::unique_ptr<std::thread> captureThread;

    // 只在 start()/stop() 的同步点或采集线程退出前写入；
    // 生命周期约定：start() 返回后 / stop() 之后读取是安全的。
    std::string lastError;

    LinuxMonitorCapture (const LinuxMonitorCapture&) = delete;
    LinuxMonitorCapture& operator= (const LinuxMonitorCapture&) = delete;
};

} // namespace y2k
