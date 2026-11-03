#pragma once

#include <vector>

#include "source/analysis/music/MusicAnalysisTypes.h"
#include "source/analysis/music/RealFft.h"

// ==========================================================
// KeyEstimator —— 滑动窗口的音乐调性（Key）估计
//
// 注意：这里检测的是"调性"（C major / A minor），不是单音 pitch，
//       也不是"当前音符"。
//
// 处理链：
//   单声道 PCM（任意块长）
//     → 内部按固定 hop 分帧（约 186ms 窗 / 25% hop，约 21 帧/秒 @44.1k）
//     → Hann 窗 + FFT → 55 Hz–5 kHz 频带按音级折叠成 12 维 chroma
//     → 每帧 chroma 归一化 + 电平加权
//     → 20 秒滑动窗口累加（按时间淘汰旧帧，转调后旧调性会被逐步挤出）
//     → 与 Krumhansl-Kessler 大/小调音级轮廓做 Pearson 相关（24 个旋转）
//     → 关系大小调（C major ↔ A minor）迟滞，避免来回摇摆
//
// 为什么是滑动窗口而不是把全部历史喂进一个分析器：
//   全局累积会让 10 分钟前的内容一直压住当前段落，转调后结果不跟随。
//
// 准确率说明：
//   没有任何"已验证准确率"的声明。KK 轮廓的相关性分数只作为内部判定阈值，
//   不对外显示为百分比。
//
// 线程模型：全部方法只在分析工作线程（或测试主线程）调用，内部无锁。
// ==========================================================

namespace y2k::music
{

class KeyEstimator
{
public:
    void prepare (double newSampleRate);

    // 清空滑动窗口（静音 / 输入不连续时使用），保留已经发布的调性。
    void resetAccumulation();

    // 彻底重置：清空所有状态，published/confident 变为 false。
    void reset();

    void pushAudio (const float* mono, int numSamples);

    void dropPendingFrame() noexcept;

    bool hasKey() const noexcept   { return published && confident; }
    int  root() const noexcept     { return publishedRoot; }
    Mode mode() const noexcept     { return publishedMode; }
    bool minor() const noexcept    { return isMinorMode (publishedMode); }

    // 置信度 [0,1]：启发式稳定性指标（不是概率）。
    //   = (0.60·轮廓相关度 + 0.40·与不同主音候选的差距)
    //     × (0.30 + 0.70·窗口覆盖度)
    float confidence() const noexcept { return keyConfidence; }

    // 内部相关性得分 [0,1]（Pearson r），不是概率。
    float lastScore() const noexcept { return lastBestScore; }

    double lastChangeSeconds() const noexcept { return changeAtSec; }
    double analysedSeconds() const noexcept   { return totalSec; }

    // 滑动窗口配置：15–30 秒是合理起点，可根据性能/曲目调整。
    void   setWindowSeconds (double seconds) noexcept;
    double getWindowSeconds() const noexcept { return windowSec; }

    // 测试 / 诊断：当前窗口内保存的 chroma 帧数（用于验证长时间运行内存有界）。
    int debugFrameCount() const noexcept { return frameCount; }
    int debugFrameCapacity() const noexcept { return frameCapacity; }

private:
    struct ChromaFrame
    {
        float  c[12] = {};
        double t     = 0.0;
        float  weight = 1.0f;
    };

    void processFrame();
    void updateKey();
    static float profileScore (const float chroma[12], int root, Mode mode);

    double sampleRate  = 44100.0;
    int    windowSize  = 8192;
    int    hopSize     = 2048;
    double frameHopSec = 0.0464;

    RealFft fft;
    std::vector<float> window;
    std::vector<float> frameBuf;
    std::vector<float> mag;

    std::vector<float> hist;
    int histWrite = 0;
    int hopFill   = 0;

    std::vector<ChromaFrame> frames;
    int frameCapacity = 0;
    int frameHead     = 0;   // 最旧帧下标
    int frameCount    = 0;

    double windowSec    = 20.0;
    double minWindowSec = 8.0;
    double updateIntervalSec = 1.5;
    double secondsSinceUpdate = 0.0;

    bool  published      = false;
    int   publishedRoot  = -1;
    Mode  publishedMode  = Mode::major;
    bool  confident      = false;

    int   pendingRoot  = -1;
    Mode  pendingMode  = Mode::major;
    int   pendingVotes = 0;

    double changeAtSec   = 0.0;
    float  lastBestScore = 0.0f;

    float  keyConfidence = 0.0f;

    double totalSamples = 0.0;
    double totalSec     = 0.0;
    float  levelDb      = -100.0f;
};

} // namespace y2k::music
