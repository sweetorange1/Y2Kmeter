#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "source/analysis/music/KeyEstimator.h"
#include "source/analysis/music/MusicAnalysisTypes.h"
#include "source/analysis/music/TempoEstimator.h"

// ==========================================================
// MusicAnalysisEngine —— 音频线程 → 有界环形缓冲 → 后台分析线程 → 结果发布
//
// 线程模型：
//   · pushStereo()   音频线程调用。仅做"立体声降为单声道 + 写入无锁 SPSC
//                    环形缓冲"，无锁、无分配、无 I/O、无日志。
//   · run()          后台 juce::Thread。从环形缓冲取块，喂给 TempoEstimator /
//                    KeyEstimator（FFT、自相关、chroma 全部在这里跑）。
//   · getResult()    任意线程。SpinLock 保护的一份 Result 拷贝。
//   · prepare()      采样率变化。只写原子变量 + 置位请求，由工作线程完成
//                    重建 —— 这样从 Loopback 采集线程调用也不会阻塞音频路径。
//
// 生命周期（"非持续激活"）：
//   · 构造时不建线程、不分配缓冲；
//   · AnalyserHub::retain(Kind::MusicAnalysis) 的 0→1 触发 setActive(true)
//     → 分配环形缓冲 + 启动工作线程；
//   · release 到 0（模块被移除）→ setActive(false) → 线程退出、析构时释放缓冲；
//   · 用户按 STOP 也会停掉线程（不是空转轮询），START 再拉起。
//
// 缓冲与过载策略：
//   · 环形缓冲容量固定 131072 采样（48kHz 约 2.7 秒，512 KB），永不增长。
//   · 空闲不足时丢弃"新来的溢出部分"，并递增 gapEpoch；工作线程发现 epoch
//     变化会丢弃未凑满 hop 的残余样本，避免把拼接波形送进 FFT。
//   · 丢样只会让某一帧的 onset/chroma 略有偏差，不会累积、不会阻塞。
// ==========================================================

namespace y2k::music
{

class MusicAnalysisEngine : private juce::Thread
{
public:
    MusicAnalysisEngine();
    ~MusicAnalysisEngine() override;

    // ---- 订阅开关（由 AnalyserHub 的 Kind 引用计数驱动，UI 线程）----
    //   0 个订阅者（没有 Tempo/Key 模块）→ 线程不启动、缓冲不分配、
    //   音频线程也不会往环形缓冲写一个采样。
    void setActive (bool shouldBeActive) noexcept;
    bool isActive() const noexcept { return active.load (std::memory_order_acquire); }

    // ---- 控制（UI 线程）----
    //   用户级 START/STOP：START 会临时启动工作线程，STOP 会把它停掉
    //   （而不是空转轮询）。
    void setEnabled (bool shouldRun) noexcept;
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

    // 清空全部分析状态并重新开始计时（"重置分析"按钮）。
    void reset() noexcept;

    // ---- 采样率 / 块长变化（可能在采集线程调用）----
    void prepare (double newSampleRate, int maxBlockSamples) noexcept;

    // ---- 音频线程 ----
    void pushStereo (const float* left, const float* right, int numSamples) noexcept;

    // ---- 结果读取（任意线程）----
    Result getResult() const noexcept;

    struct Diagnostics
    {
        double   sampleRate     = 0.0;
        uint64_t droppedSamples = 0;
        uint64_t queuedSamples  = 0;
        uint32_t gapEpoch       = 0;
        bool     running        = false;
    };

    Diagnostics getDiagnostics() const noexcept;

private:
    void run() override;
    void applyPendingFormat();
    void clearAccumulation();
    void publishResult (bool force);
    void discardQueuedAudio() noexcept;

    // 首次订阅时才分配缓冲 / 创建线程（未加载模块时零资源占用）
    bool ensureBuffers() noexcept;
    void startWorker() noexcept;
    void stopWorker() noexcept;

    static constexpr int      ringCapacity = 1 << 17;   // 131072 采样（48k ≈ 2.7s）
    static constexpr uint64_t ringMask     = (uint64_t) (ringCapacity - 1);

    std::vector<float> ring;
    std::atomic<uint64_t> writeIndex { 0 };
    std::atomic<uint64_t> readIndex  { 0 };

    std::atomic<uint64_t> droppedSamples { 0 };
    std::atomic<uint32_t> gapEpoch       { 0 };
    uint32_t consumedGapEpoch = 0;

    std::atomic<double> requestedSampleRate { 44100.0 };
    std::atomic<bool>   resetRequested { false };

    // active：是否有模块订阅（线程是否被允许运行）
    // enabled：用户是否按下了 START
    // 真正跑分析线程的条件 = active && enabled
    std::atomic<bool>   active  { false };
    std::atomic<bool>   enabled { true };

    double appliedSampleRate = 0.0;

    TempoEstimator tempo;
    KeyEstimator   key;

    std::vector<float> scratch;

    double fedSamples    = 0.0;
    double silentSamples = 0.0;
    bool   silenceCleared = false;
    int    emptyPolls    = 0;

    mutable juce::SpinLock resultLock;
    Result published;
    Result lastPublished;
    double lastPublishMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MusicAnalysisEngine)
};

} // namespace y2k::music
