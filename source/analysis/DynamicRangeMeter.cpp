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

    allPeakHist.fill(0);
    allRmsHist .fill(0);

    silenceSamples = 0.0;

    lastPeakL = lastPeakR = -144.0f;
    lastRmsL  = lastRmsR  = -144.0f;

    const juce::SpinLock::ScopedLockType sl(snapLock);
    snap = Snapshot{};
}

float DynamicRangeMeter::percentileTop20(const std::deque<float>& v)
{
    const int N = (int) v.size();
    if (N <= 0 || N > shortBlockCount) return -144.0f;

    // 栈上定长拷贝 + 排序：短时队列上限 30 个元素，
    //   避免音频线程每块都 new/delete 一个 std::vector。
    std::array<float, shortBlockCount> tmp {};
    std::copy (v.begin(), v.end(), tmp.begin());
    std::sort (tmp.begin(), tmp.begin() + N);  // 升序

    // top 20% 对应下标 [ceil(0.8*N) , N)，取其均值
    const int start = juce::jlimit(0, N - 1, (int) std::ceil(0.8 * N) - 1);
    double sum = 0.0;
    int cnt = 0;
    for (int i = start; i < N; ++i) { sum += tmp[(size_t) i]; ++cnt; }
    return (cnt > 0) ? (float)(sum / cnt) : -144.0f;
}

void DynamicRangeMeter::addToHist (std::array<int, drHistBins>& h, float db) noexcept
{
    if (! std::isfinite (db))
        db = drHistMinDb;

    const int idx = juce::jlimit (0, drHistBins - 1,
                                  (int) std::lround ((db - drHistMinDb) / drHistBinDb));
    ++h[(size_t) idx];
}

float DynamicRangeMeter::percentileTop20FromHist (const std::array<int, drHistBins>& h) noexcept
{
    long long total = 0;
    for (int c : h) total += c;
    if (total <= 0) return -144.0f;

    // 上尾 20% 的样本数（至少 1 个）
    float need = juce::jmax (1.0f, (float) total * 0.2f);

    double sum = 0.0;
    float taken = 0.0f;
    for (int i = drHistBins - 1; i >= 0 && need > 0.0f; --i)
    {
        const int c = h[(size_t) i];
        if (c <= 0) continue;

        const float binDb = drHistMinDb + (float) i * drHistBinDb;
        const float take  = juce::jmin ((float) c, need);   // 边界箱按比例计入
        sum  += (double) binDb * (double) take;
        taken += take;
        need  -= take;
    }

    return (taken > 0.0f) ? (float) (sum / (double) taken) : -144.0f;
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
    // 能量平均：sqrt((rmsL² + rmsR²)/2)。算术平均 0.5*(rmsL+rmsR) 会在左右
    //   声道不均衡时低估混合 RMS（单声道时最多差 3 dB）。
    const float rmsMixed = std::sqrt (0.5f * (rmsL * rmsL + rmsR * rmsR));
    const float rmsDb    = toDb(rmsMixed);

    // 写入短时队列
    shortPeakDb.push_back(peakDb);
    shortRmsDb .push_back(rmsDb);
    while ((int) shortPeakDb.size() > shortBlockCount) shortPeakDb.pop_front();
    while ((int) shortRmsDb .size() > shortBlockCount) shortRmsDb .pop_front();

    // 写入全程直方图（供 integratedDR 分位数计算，恒定内存、零分配）
    addToHist (allPeakHist, peakDb);
    addToHist (allRmsHist,  rmsDb);

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

    // integratedDR：与 shortDR 一致的分位数法，窗口为全程（直方图统计）
    const float iPeakTop = percentileTop20FromHist (allPeakHist);
    const float iRmsTop  = percentileTop20FromHist (allRmsHist);
    ns.integratedDR = juce::jlimit(0.0f, 60.0f, iPeakTop - iRmsTop);

    // 记录最后一次原始值
    lastPeakL = ns.peakL;
    lastPeakR = ns.peakR;
    lastRmsL  = ns.rmsL;
    lastRmsR  = ns.rmsR;

    {
        const juce::SpinLock::ScopedLockType sl(snapLock);
        snap = ns;
    }

    // ---- Integrated DR 静音自动重置 ----
    //   用本块（尚未清零）的合成 RMS 均方判断“无信号”：连续 silenceResetSeconds
    //   秒低于 -60 dBFS 门限时清空 integrated 累积，下次有信号重新累积。
    {
        const double blockMeanSq =
            (blockSumSqL + blockSumSqR) / (2.0 * static_cast<double>(blockCounter));
        if (blockMeanSq < silenceMeanSqThreshold)
        {
            silenceSamples += static_cast<double>(blockCounter);
            if (silenceSamples >= sampleRate * silenceResetSeconds)
            {
                allPeakHist.fill(0);
                allRmsHist .fill(0);
                silenceSamples = 0.0;
            }
        }
        else
        {
            silenceSamples = 0.0;
        }
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
