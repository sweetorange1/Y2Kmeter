#ifndef Y2K_AUDIO_RECORDER_H_INCLUDED
#define Y2K_AUDIO_RECORDER_H_INCLUDED

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

// ==========================================================
// AudioRecorder —— 录音机后端（RecorderModule 的音频/磁盘部分）
//
// 工作方式：**常驻滚动采集 + 按需导出最近 N 秒**
//   · 采集从 AudioRecorder 构造时就开始（打开软件 / 加载插件即运行），与是否加
//     Recorder 模块无关；模块被移除也不停采集；
//   · 采集内容是 **pre-gain 原始输入**（PluginProcessor::processBlock / Standalone
//     Loopback 采集线程在应用输入增益之前调用 pushRaw），因此用户调的 gain
//     不会进入导出文件，导出内容与送进软件的信号完全一致；
//   · 内存里保留最近 60 s（maxHistorySeconds）的滚动历史，导出时取最近 N 秒；
//   · 另有一条独立的手动录制通道（startTake / stopTake，红点按钮）：采集线程把
//     数据**同时**流式写进一个 WAV 文件，时长不受 60 s 限制；
//   · 文件格式 32-bit float WAV：AudioFormatWriter 在浮点模式下按位直写，
//     与输入浮点样本 bit-exact；
//   · 保存目录 <用户音乐目录>/Y2Kmeter Recordings，
//     滚动导出 = Y2Kmeter_clip_<N秒>_<yyyy-MM-dd_HH-mm-ss>.wav（N = 导出选择的秒数），
//     手动录制 = Y2Kmeter_recording_<yyyy-MM-dd_HH-mm-ss>.wav；
//   · 导出完成后自动打开目录；Windows 上若资源管理器已停在该目录则不重复开窗。
//
// 线程模型（三层，音频线程永不阻塞）：
//   音频线程   pushRaw()            → 无锁 SPSC 环形缓冲（AbstractFifo）
//   采集线程   run()                → 排空 FIFO，写入 60 s 滚动历史（historyLock）
//   消息线程   exportLastSeconds()  → 加锁快照最近 N 秒（几 ms～几十 ms），再落盘
// ==========================================================
class AudioRecorder : private juce::Thread
{
public:
    AudioRecorder();
    ~AudioRecorder() override;

    // ---- 常量 ----
    static constexpr double maxHistorySeconds = 60.0;
    static constexpr const char* folderName = "Y2Kmeter Recordings";
    // 导出文件名：Y2Kmeter_clip_<N秒>_<yyyy-MM-dd_HH-mm-ss>.wav
    static constexpr const char* clipPrefix = "Y2Kmeter_clip_";
    // 手动录制文件名：Y2Kmeter_recording_<yyyy-MM-dd_HH-mm-ss>.wav
    static constexpr const char* takePrefix = "Y2Kmeter_recording_";

    // <用户音乐目录>/Y2Kmeter Recordings（不保证已存在）
    static juce::File getRecordingsDirectory();

    // 打开目录。Windows：若资源管理器已经打开同一目录，只前置已有窗口；
    // 其它平台：本次会话内同一目录只主动打开一次。
    static void openDirectory (const juce::File& folder);

    // ---- 采集状态 ----
    //   采集在 AudioRecorder 构造时启动（软件一打开 / 插件实例一创建就开始滚动），
    //   析构时才停止；Recorder 模块只负责展示与导出，不再控制采集开关。
    bool isCapturing() const noexcept { return capturing.load (std::memory_order_acquire); }

    // ---- 查询（消息线程）----
    // 当前滚动缓冲里可导出的时长（0 .. maxHistorySeconds）
    double getBufferedSeconds() const noexcept;
    juce::File getLastSavedFile() const;
    juce::String getStatusText() const;

    // ---- 导出（消息线程，同步落盘）----
    // 把最近 seconds 秒写成 WAV 并打开保存目录；缓冲为空 / 写失败返回 false。
    bool exportLastSeconds (double seconds);

    // ---- 手动录制（红点按钮）：一段独立录音，直接流式写盘 ----
    //   startTake：立即开始，文件名 Y2Kmeter_recording_<yyyy-MM-dd_HH-mm-ss>.wav
    //   stopTake ：停止，采集线程把 FIFO 残余写完并关闭文件、打开目录
    bool   startTake();
    void   stopTake();
    bool   isTaking() const noexcept { return takeActive.load (std::memory_order_acquire); }
    double getTakeSeconds() const noexcept;
    juce::File getTakeFile() const;

    // ---- 音频线程 ----
    // 推送 pre-gain 原始立体声样本 + 当前采样率；未在采集时立即返回（一次原子读）。
    void pushRaw (const float* left, const float* right, int numSamples, double sampleRate) noexcept;

private:
    // 音频线程 → 采集线程 的搬运缓冲（每声道样本数）：1<<16 @48kHz ≈ 1.36 s
    static constexpr int fifoCapacityPerChannel = 1 << 16;
    // 采集线程单次搬运的最大样本数
    static constexpr int moveChunkSize = 4096;

    void run() override;

    void startCapture();
    void stopCapture();

    void ensureHistory (double sampleRate);
    void appendFromFifo();
    void appendHistory (const float* left, const float* right, int numSamples);

    // 手动录制（全部在采集线程执行，UI 线程只发请求 / 读状态）
    void openTakeWriter (double sampleRate);
    void closeTakeWriter();
    void writeTake (const float* left, const float* right, int numSamples);

    void setStatus (const juce::String& text);

    // 无锁环形缓冲：先 L 后 R（fifoData[0 .. N) = L，[N .. 2N) = R）
    juce::AbstractFifo fifo { fifoCapacityPerChannel };
    std::vector<float> fifoData;

    // 音频线程 → 采集/UI 线程
    std::atomic<bool>   captureEnabled      { false };
    std::atomic<bool>   capturing           { false };
    std::atomic<double> inputSampleRate     { 44100.0 };
    std::atomic<juce::int64> historyReady   { 0 };   // 有效样本数（供 UI 读）
    std::atomic<double> historyRateSnapshot { 44100.0 };

    // 滚动历史（采集线程独占写；导出时 UI 线程加锁快照）
    juce::CriticalSection historyLock;
    std::vector<float> historyL, historyR;
    int historyCapacity = 0;
    int historyWritePos = 0;
    double historySampleRate = 44100.0;
    juce::int64 historyValid = 0;

    // 手动录制（红点）
    std::atomic<bool> takeRequested { false };
    std::atomic<bool> takeActive    { false };
    std::atomic<juce::int64> takeSamples { 0 };
    std::atomic<double> takeSampleRate   { 44100.0 };
    std::atomic<bool> shuttingDown { false };
    juce::CriticalSection takeLock;
    juce::File pendingTakeFile;                        // UI 写、采集线程读
    std::unique_ptr<juce::AudioFormatWriter> takeWriter; // 采集线程独占

    // 展示信息
    mutable juce::SpinLock metaLock;
    juce::File   lastSavedFile;
    juce::String statusText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRecorder)
};

#endif // Y2K_AUDIO_RECORDER_H_INCLUDED
