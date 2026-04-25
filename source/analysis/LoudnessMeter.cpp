#include "source/analysis/LoudnessMeter.h"
#include <cmath>
#include <numeric>

// ==========================================================
// True Peak FIR 系数（4× 过采样，Sinc + Hann 窗，12 抽头/相位）
// 相位 0 = 原始采样（直通），相位 1/2/3 = 插值相位
// ==========================================================
const float LoudnessMeter::tpFirCoeffs[tpOversample][tpFirLen] =
{
    // Phase 0（直通，中心抽头 = 1）
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    // Phase 1（0.25 偏移 Sinc+Hann）
    { -0.00065f,  0.00424f, -0.01678f,  0.05928f, -0.17659f,  0.61710f,
       0.61710f, -0.17659f,  0.05928f, -0.01678f,  0.00424f, -0.00065f },
    // Phase 2（0.5 偏移 Sinc+Hann）
    { -0.00092f,  0.00601f, -0.02381f,  0.08404f, -0.25000f,  0.75000f,
      -0.25000f,  0.08404f, -0.02381f,  0.00601f, -0.00092f,  0.00000f },
    // Phase 3（0.75 偏移 Sinc+Hann）
    { -0.00065f,  0.00424f, -0.01678f,  0.05928f, -0.17659f,  0.61710f,
       0.61710f, -0.17659f,  0.05928f, -0.01678f,  0.00424f, -0.00065f },
};

// ==========================================================
// TruePeakState::interpolate
// ==========================================================
float LoudnessMeter::TruePeakState::interpolate(int phase) const noexcept
{
    float sum = 0.0f;
    for (int i = 0; i < tpFirLen; ++i)
        sum += tpFirCoeffs[phase][i] * history[(size_t)i];
    return sum;
}

// ==========================================================
// K-weighting 系数计算（ITU-R BS.1770-4 附录 1）
// 两级 biquad：
//   Stage 1：高频搁架（pre-filter）
//   Stage 2：高通（RLB-weighting）
// ==========================================================
void LoudnessMeter::calcKWeightingCoeffs(double sr,
                                          BiquadCoeffs& pre,
                                          BiquadCoeffs& rlb)
{
    // ---- Stage 1: High-shelf pre-filter ----
    // 参考：ITU-R BS.1770-4 Table 1
    const double db  =  3.999843853973347;
    const double f0  = 1681.974450955533;
    const double Q   =  0.7071752369554196;
    const double K   = std::tan(juce::MathConstants<double>::pi * f0 / sr);
    const double Vh  = std::pow(10.0, db / 20.0);
    const double Vb  = std::pow(Vh, 0.4996667741545416);
    const double a0  = 1.0 + K / Q + K * K;
    pre.b0 = (Vh + Vb * K / Q + K * K) / a0;
    pre.b1 = 2.0 * (K * K - Vh) / a0;
    pre.b2 = (Vh - Vb * K / Q + K * K) / a0;
    pre.a1 = 2.0 * (K * K - 1.0) / a0;
    pre.a2 = (1.0 - K / Q + K * K) / a0;

    // ---- Stage 2: High-pass RLB filter ----
    const double f1 = 38.13547087602444;
    const double Q1 =  0.5003270373238773;
    const double K1 = std::tan(juce::MathConstants<double>::pi * f1 / sr);
    const double a0b = 1.0 + K1 / Q1 + K1 * K1;
    rlb.b0 =  1.0;
    rlb.b1 = -2.0;
    rlb.b2 =  1.0;
    rlb.a1 = 2.0 * (K1 * K1 - 1.0) / a0b;
    rlb.a2 = (1.0 - K1 / Q1 + K1 * K1) / a0b;
}

// ==========================================================
// applyKWeighting
// ==========================================================
double LoudnessMeter::applyKWeighting(double sample, ChannelFilter& ch) const noexcept
{
    double s1 = ch.stage1.process(sample, preFilterCoeffs);
    double s2 = ch.stage2.process(s1,     rlbFilterCoeffs);
    return s2;
}

// ==========================================================
// LoudnessMeter
// ==========================================================
LoudnessMeter::LoudnessMeter()
{
    // 默认系数（44100 Hz）
    calcKWeightingCoeffs(44100.0, preFilterCoeffs, rlbFilterCoeffs);
}

void LoudnessMeter::prepare(double sr, int /*samplesPerBlock*/)
{
    sampleRate = sr;
    calcKWeightingCoeffs(sr, preFilterCoeffs, rlbFilterCoeffs);

    // 400ms 块大小
    momentaryBlockSize  = juce::roundToInt(sr * 0.4);
    momentaryWriteCount = 0;
    momentarySumL = momentarySumR = 0.0;

    // RMS 100ms 窗口
    rmsWindowSize = juce::roundToInt(sr * 0.1);

    reset();
}

void LoudnessMeter::reset()
{
    filterL.reset();
    filterR.reset();
    tpL.reset();
    tpR.reset();

    momentaryWriteCount = 0;
    momentarySumL = momentarySumR = 0.0;

    shortTermBlocksL.clear();
    shortTermBlocksR.clear();

    integratedSumL = integratedSumR = 0.0;
    integratedCount = 0;

    rmsHistL.clear();
    rmsHistR.clear();
    rmsSumL = rmsSumR = 0.0;

    {
        const juce::SpinLock::ScopedLockType sl(snapshotLock);
        snapshot = Snapshot{};
    }
}

// ==========================================================
// pushStereo —— 音频线程
// ==========================================================
void LoudnessMeter::pushStereo(const float* left, const float* right, int numSamples)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const double l = (double) left[i];
        const double r = (double) right[i];

        // ---- K-weighting ----
        const double kl = applyKWeighting(l, filterL);
        const double kr = applyKWeighting(r, filterR);

        // ---- 累积 400ms 块均方 ----
        momentarySumL += kl * kl;
        momentarySumR += kr * kr;
        ++momentaryWriteCount;

        // ---- RMS（100ms 滑动）----
        {
            const float fl = left[i];
            const float fr = right[i];
            rmsSumL += (double)(fl * fl);
            rmsSumR += (double)(fr * fr);
            rmsHistL.push_back(fl);
            rmsHistR.push_back(fr);
            if ((int)rmsHistL.size() > rmsWindowSize)
            {
                const float old = rmsHistL.front(); rmsHistL.pop_front();
                rmsSumL -= (double)(old * old);
            }
            if ((int)rmsHistR.size() > rmsWindowSize)
            {
                const float old = rmsHistR.front(); rmsHistR.pop_front();
                rmsSumR -= (double)(old * old);
            }
        }

        // ---- True Peak ----
        {
            tpL.push(left[i]);
            tpR.push(right[i]);
            for (int ph = 0; ph < tpOversample; ++ph)
            {
                tpL.peak = juce::jmax(tpL.peak, std::abs(tpL.interpolate(ph)));
                tpR.peak = juce::jmax(tpR.peak, std::abs(tpR.interpolate(ph)));
            }
        }

        // ---- 400ms 块结束 ----
        if (momentaryWriteCount >= momentaryBlockSize)
        {
            const double meanL = momentarySumL / (double) momentaryBlockSize;
            const double meanR = momentarySumR / (double) momentaryBlockSize;
            const double blockMean = (meanL + meanR) * 0.5;

            // 短期队列（保留最近 shortTermBlocks 个块）
            shortTermBlocksL.push_back(meanL);
            shortTermBlocksR.push_back(meanR);
            if ((int)shortTermBlocksL.size() > shortTermBlocks)
            {
                shortTermBlocksL.pop_front();
                shortTermBlocksR.pop_front();
            }

            // 积分（绝对门限 -70 LUFS）
            if (blockMean >= absGateThreshold)
            {
                integratedSumL += meanL;
                integratedSumR += meanR;
                ++integratedCount;
            }

            momentarySumL = momentarySumR = 0.0;
            momentaryWriteCount = 0;

            updateSnapshot();
        }
    }
}

// ==========================================================
// updateSnapshot —— 在音频线程每 400ms 块结束时调用
// ==========================================================
void LoudnessMeter::updateSnapshot()
{
    Snapshot s;

    // Momentary（最新一块）
    if (!shortTermBlocksL.empty())
    {
        const double mL = shortTermBlocksL.back();
        const double mR = shortTermBlocksR.back();
        s.lufsM = linearToLUFS((mL + mR) * 0.5);
    }

    // Short-term（最近 shortTermBlocks 块均值）
    if (!shortTermBlocksL.empty())
    {
        double sumL = 0.0, sumR = 0.0;
        for (auto v : shortTermBlocksL) sumL += v;
        for (auto v : shortTermBlocksR) sumR += v;
        const int n = (int)shortTermBlocksL.size();
        s.lufsS = linearToLUFS((sumL + sumR) * 0.5 / (double) n);
    }

    // Integrated
    if (integratedCount > 0)
    {
        const double iL = integratedSumL / (double) integratedCount;
        const double iR = integratedSumR / (double) integratedCount;
        s.lufsI = linearToLUFS((iL + iR) * 0.5);
        s.integrated = true;
    }

    // RMS
    {
        const int n = juce::jmax(1, (int)rmsHistL.size());
        s.rmsL = linearToLUFS(rmsSumL / (double) n);
        s.rmsR = linearToLUFS(rmsSumR / (double) n);
    }

    // True Peak
    s.truePeakL = (tpL.peak > 1.0e-7f)
        ? juce::Decibels::gainToDecibels(tpL.peak)
        : -144.0f;
    s.truePeakR = (tpR.peak > 1.0e-7f)
        ? juce::Decibels::gainToDecibels(tpR.peak)
        : -144.0f;

    {
        const juce::SpinLock::ScopedLockType sl(snapshotLock);
        snapshot = s;
    }
}

// ==========================================================
// getSnapshot —— UI 线程
// ==========================================================
LoudnessMeter::Snapshot LoudnessMeter::getSnapshot() const
{
    const juce::SpinLock::ScopedLockType sl(snapshotLock);
    return snapshot;
}

// ==========================================================
// linearToLUFS
// ==========================================================
float LoudnessMeter::linearToLUFS(double meanSquare) noexcept
{
    if (meanSquare <= 1.0e-10)
        return -144.0f;
    return (float)(-0.691 + 10.0 * std::log10(meanSquare));
}
