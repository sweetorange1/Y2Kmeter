#include "source/analysis/AnalyserHub.h"
#include <cmath>
#include <algorithm>
#include <vector>

// ==========================================================
// DynamicRangeMeter —— 实时动态范围检测
// ==========================================================

void DynamicRangeMeter::prepare(double sr)
{
    sampleRate = juce::jmax(1.0, sr);
    blockSize  = (int) std::round(sampleRate * 0.1); // 100ms
    reset();
}

void DynamicRangeMeter::reset()
{
    blockCounter = 0;
    blockPeakL = blockPeakR = 0.0f;
    blockSumSqL = blockSumSqR = 0.0;

    shortPeakDb.clear();
    shortRmsDb.clear();

    integratedSumSq   = 0.0;
    integratedSamples = 0;
    integratedPeakDb  = -144.0f;

    lastPeakL = lastPeakR = -144.0f;
    lastRmsL  = lastRmsR  = -144.0f;

    const juce::SpinLock::ScopedLockType sl(snapLock);
    snap = Snapshot{};
}

float DynamicRangeMeter::percentileTop20(const std::deque<float>& v)
{
    if (v.empty()) return -144.0f;
    std::vector<float> tmp(v.begin(), v.end());
    std::sort(tmp.begin(), tmp.end());  // 升序
    // top 20% 对应下标 [ceil(0.8*N) , N)，取其均值
    const int N = (int) tmp.size();
    const int start = juce::jlimit(0, N - 1, (int) std::ceil(0.8 * N) - 1);
    double sum = 0.0;
    int cnt = 0;
    for (int i = start; i < N; ++i) { sum += tmp[(size_t) i]; ++cnt; }
    return (cnt > 0) ? (float)(sum / cnt) : -144.0f;
}

void DynamicRangeMeter::finishBlock()
{
    if (blockCounter <= 0) return;

    const float peakL = blockPeakL;
    const float peakR = blockPeakR;
    const float rmsL  = (float) std::sqrt(blockSumSqL / (double) blockCounter);
    const float rmsR  = (float) std::sqrt(blockSumSqR / (double) blockCounter);

    // 转 dB
    auto toDb = [](float lin) -> float
    {
        return (lin < 1.0e-7f) ? -144.0f
                               : juce::Decibels::gainToDecibels(lin);
    };

    const float peakDb   = toDb(juce::jmax(peakL, peakR));
    const float rmsMixed = 0.5f * (rmsL + rmsR);
    const float rmsDb    = toDb(rmsMixed);

    // 写入短时队列
    shortPeakDb.push_back(peakDb);
    shortRmsDb .push_back(rmsDb);
    while ((int) shortPeakDb.size() > shortBlockCount) shortPeakDb.pop_front();
    while ((int) shortRmsDb .size() > shortBlockCount) shortRmsDb .pop_front();

    // 全程统计
    integratedPeakDb = juce::jmax(integratedPeakDb, peakDb);

    // 计算快照
    Snapshot ns;
    ns.peakL = toDb(peakL);
    ns.peakR = toDb(peakR);
    ns.rmsL  = toDb(rmsL);
    ns.rmsR  = toDb(rmsR);
    ns.crest = peakDb - rmsDb;
    ns.crest = juce::jlimit(0.0f, 60.0f, ns.crest);

    // shortDR
    const float sPeakTop = percentileTop20(shortPeakDb);
    const float sRmsTop  = percentileTop20(shortRmsDb);
    ns.shortDR = juce::jlimit(0.0f, 60.0f, sPeakTop - sRmsTop);

    // integratedDR：用全程 integrated peak 与 integrated rms 近似
    const double avgSumSq = (integratedSamples > 0)
                              ? integratedSumSq / (double) integratedSamples
                              : 0.0;
    const float integRms = toDb((float) std::sqrt(juce::jmax(0.0, avgSumSq)));
    ns.integratedDR = juce::jlimit(0.0f, 60.0f, integratedPeakDb - integRms);

    // 记录最后一次原始值
    lastPeakL = ns.peakL;
    lastPeakR = ns.peakR;
    lastRmsL  = ns.rmsL;
    lastRmsR  = ns.rmsR;

    {
        const juce::SpinLock::ScopedLockType sl(snapLock);
        snap = ns;
    }

    // 块状态清空
    blockCounter = 0;
    blockPeakL = blockPeakR = 0.0f;
    blockSumSqL = blockSumSqR = 0.0;
}

void DynamicRangeMeter::pushStereo(const float* left, const float* right, int numSamples)
{
    if (left == nullptr || right == nullptr || numSamples <= 0 || blockSize <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const float L = left[i];
        const float R = right[i];
        const float absL = std::abs(L);
        const float absR = std::abs(R);

        blockPeakL = juce::jmax(blockPeakL, absL);
        blockPeakR = juce::jmax(blockPeakR, absR);
        blockSumSqL += (double) L * (double) L;
        blockSumSqR += (double) R * (double) R;

        // 全程积分
        integratedSumSq += (double)(L * L + R * R) * 0.5;
        ++integratedSamples;

        ++blockCounter;
        if (blockCounter >= blockSize)
            finishBlock();
    }
}

DynamicRangeMeter::Snapshot DynamicRangeMeter::getSnapshot() const
{
    const juce::SpinLock::ScopedLockType sl(snapLock);
    return snap;
}
