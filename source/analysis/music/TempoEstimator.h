#pragma once

#include <vector>

#include "source/analysis/music/RealFft.h"

// ==========================================================
// TempoEstimator —— 逐块输入的节拍（BPM）估计
//
// 处理链：
//   单声道 PCM（任意块长）
//     → 内部按固定 hop 分帧（46ms 窗 / 25% hop，约 86 帧/秒 @44.1k）
//     → Hann 窗 + FFT → 24 个对数频带能量
//     → 频带对数差分取正半波 = 谱通量（onset envelope）
//     → 8 秒包络窗口上做归一化自相关 + 谐波梳状求和 + 对数正态速度先验
//     → 抛物线插值细化周期 → 候选序列的中值/离散度稳定性判定
//     → 八度迟滞（避免 70/140 之间来回跳）
//
// 关键约定：
//   · 这里的 BPM 搜索范围 45–220 只用于"在哪个区间找周期峰值"，不是
//     "把结果硬修正到某个范围"；超出该区间的音乐通常会被报告为半速/倍速，
//     这属于节拍跟踪的固有八度歧义，已在文档中说明。
//   · 静音 / 低电平帧不产生 onset，包络自然归零；连续静音由上层引擎
//     调用 resetAccumulation() 清窗口，避免旧节拍残留。
//   · 只有在最近 3 次估计互相接近（±4%）且连续两轮成立时才发布 BPM，
//     启动阶段一律返回 false（上层显示"分析中"）。
//
// 线程模型：全部方法只在分析工作线程（或测试主线程）调用，内部无锁。
// ==========================================================

namespace y2k::music
{

class TempoEstimator
{
public:
    // 采样率变化时必须重新 prepare（内部重建 FFT / 频带映射 / 包络容量）。
    void prepare (double newSampleRate);

    // 清空"输入 / 包络 / 候选"，但保留已经发布的 BPM（静音时使用）。
    void resetAccumulation();

    // 彻底重置：清空所有状态，published 变为 false。
    void reset();

    // 任意块长、任意调用节奏（音频线程弹出的块、测试里的合成块）。
    void pushAudio (const float* mono, int numSamples);

    // 输入不连续（环形缓冲溢出丢样）时调用：丢弃未凑满 hop 的残余样本，
    // 避免把拼接后的波形喂给 FFT 造成假 onset。
    void dropPendingFrame() noexcept;

    bool   hasTempo() const noexcept           { return published; }
    float  bpm() const noexcept                { return publishedBpm; }
    double lastChangeSeconds() const noexcept  { return changeAtSec; }
    double analysedSeconds() const noexcept    { return totalSec; }

    // 置信度 [0,1]：启发式稳定性指标（不是概率）。
    //   = (0.35·周期性 + 0.40·候选稳定度 + 0.25·最佳/次佳间隔)
    //     × (0.20 + 0.80·数据充分度)
    //   数据充分度按"已分析秒数 / 20s"计，因此刚开始听时天然偏低。
    float confidence() const noexcept { return confidenceValue; }

    // 诊断用：最近一次估计的自相关得分 [0,1]，不是概率。
    float lastSalience() const noexcept { return salience; }
    float lastPeakMargin() const noexcept { return peakMargin; }
    float currentLevelDb() const noexcept { return levelDb; }
    bool  isFrameLoud() const noexcept { return lastFrameWasLoud; }

private:
    void processFrame();
    void pushEnvelope (float value);
    void estimateTempo();
    void updateConfidence();

    double sampleRate = 44100.0;
    double frameRate  = 86.13;
    int    windowSize = 2048;
    int    hopSize    = 512;

    RealFft fft;
    std::vector<float> window;
    std::vector<float> frameBuf;
    std::vector<float> mag;

    int bandCount = 24;
    std::vector<int>   binBand;
    std::vector<float> bandNow;
    std::vector<float> bandPrev;

    // 输入环形历史（windowSize）
    std::vector<float> hist;
    int histWrite = 0;
    int hopFill   = 0;

    // onset 包络环形缓冲
    std::vector<float> onsetEnv;
    std::vector<float> envLinear;
    std::vector<float> acf;
    std::vector<float> comb;
    int       envCapacity  = 0;
    int       envWrite     = 0;
    long long envTotal     = 0;

    int framesSinceEstimate = 0;
    int estimateEveryFrames = 43;
    int minEstimateFrames   = 344;

    std::vector<float> recentBpm;
    int stableRounds  = 0;
    int unstableCount = 0;

    bool          published    = false;
    float         publishedBpm = 0.0f;
    double        changeAtSec  = 0.0;
    float         salience     = 0.0f;

    // ---- 置信度相关（每次估计刷新）----
    float peakMargin       = 0.0f;  // (最佳分 - 次佳不同 BPM 峰) / 最佳分
    float candidateSpread  = 1.0f;  // 最近 3 个候选相对中值的最大偏差
    float confidenceValue  = 0.0f;  // [0,1]

    double totalSamples      = 0.0;
    double totalSec          = 0.0;
    float  levelDb           = -100.0f;
    bool   lastFrameWasLoud  = false;
};

} // namespace y2k::music
