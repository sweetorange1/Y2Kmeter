#include "source/analysis/LoudnessMeter.h"
#include <cmath>
#include <numeric>

// ==========================================================
// True Peak 多相插值核（4× 过采样，窗函数 sinc，12 抽头/相位）
//
//   v3.0.1 重写：旧表是手写的，phase2（半样本相位）系数和只有 0.3806
//   （DC 增益 -8.4 dB），采样点之间的峰值被严重低估 → True Peak 偏小。
//   现改为运行时按公式生成，并**每相单独归一化到 DC 增益 = 1**：
//     h[i] = sinc(i - (5 + p/4)) × Hann(i)
//   相位 0（整数延迟）退化为单抽头直通，等价于把原始采样也计入峰值（BS.1770 要求
//   TP 不小于 Sample Peak）。
// ==========================================================
void LoudnessMeter::initTruePeakCoeffs (float (&out)[tpOversample][tpFirLen]) noexcept
{
    constexpr int    kCenter = 5;   // 整数延迟抽头（history[5] = 5 个样本前）
    constexpr double kPi     = juce::MathConstants<double>::pi;

    for (int p = 0; p < tpOversample; ++p)
    {
        const double d = (double) kCenter + (double) p / (double) tpOversample;

        double h[tpFirLen] {};
        double sum = 0.0;

        for (int i = 0; i < tpFirLen; ++i)
        {
            const double t    = (double) i - d;
            const double sinc = (std::abs (t) < 1.0e-9) ? 1.0
                                                        : std::sin (kPi * t) / (kPi * t);
            // Hann 窗（以 12 抽头窗口中心为对称轴）
            const double w    = 0.5 - 0.5 * std::cos (2.0 * kPi * ((double) i + 0.5)
                                                        / (double) tpFirLen);
            h[i] = sinc * w;
            sum += h[i];
        }

        const double norm = (std::abs (sum) > 1.0e-12) ? (1.0 / sum) : 0.0;
        for (int i = 0; i < tpFirLen; ++i)
            out[p][i] = (float) (h[i] * norm);
    }
}

// ==========================================================
// TruePeakState::interpolate
// ==========================================================
float LoudnessMeter::TruePeakState::interpolate (int phase,
                                                 const float (&coeffs)[tpOversample][tpFirLen]) const noexcept
{
    float sum = 0.0f;
    for (int i = 0; i < tpFirLen; ++i)
        sum += coeffs[phase][i] * history[(size_t) i];
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

    // True Peak 多相插值核（与采样率无关，构造时生成一次）
    initTruePeakCoeffs (tpFirCoeffs);
}

void LoudnessMeter::prepare(double sr, int /*samplesPerBlock*/)
{
    sampleRate = sr;
    calcKWeightingCoeffs(sr, preFilterCoeffs, rlbFilterCoeffs);

    // 100ms 子块（门控块 400ms = 4 个子块，步进 100ms）
    momentaryBlockSize  = juce::roundToInt(sr * 0.1);
    momentaryWriteCount = 0;
    momentarySumL = momentarySumR = 0.0;

    // Momentary 实时滑动窗口（400ms）
    momentaryWindowSize = juce::roundToInt(sr * 0.4);

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

    momentaryHistL.clear();
    momentaryHistR.clear();
    momentarySlideSumL = momentarySlideSumR = 0.0;

    energy100L.clear();
    energy100R.clear();

    gatedBlocks.clear();

    silenceSamples = 0.0;

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
void LoudnessMeter::pushStereo(const float* left, const float* right, int numSamples, int numChannels)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    // 单声道输入的计权（v3.0.1 修复 +3.01 dB）：
    //   单声道时调用方把同一路信号同时传给 left/right，若按立体声各算一遍，
    //   能量 = p + p = 2p → LUFS 比参考表高 10*log10(2) = 3.01 dB。
    //   这里把每个通道的能量按 1/2 分摊：0.5p + 0.5p = p，等价于 BS.1770 里
    //   "1 个通道、G = 1.0" 的定义。
    const double chWeight = (numChannels <= 1) ? 0.5 : 1.0;
    const double w        = std::sqrt (chWeight);

    for (int i = 0; i < numSamples; ++i)
    {
        const double l = (double) left[i];
        const double r = (double) right[i];

        // ---- K-weighting ----
        const double kl = applyKWeighting(l, filterL) * w;
        const double kr = applyKWeighting(r, filterR) * w;

        // ---- 累积 400ms 块均方（供 Short-term / Integrated）----
        momentarySumL += kl * kl;
        momentarySumR += kr * kr;
        ++momentaryWriteCount;

        // ---- Momentary 实时滑动窗口（400ms，供 LUFS-M）----
        momentaryHistL.push_back(kl);
        momentaryHistR.push_back(kr);
        momentarySlideSumL += kl * kl;
        momentarySlideSumR += kr * kr;
        if ((int)momentaryHistL.size() > momentaryWindowSize)
        {
            const double old = momentaryHistL.front(); momentaryHistL.pop_front();
            momentarySlideSumL -= old * old;
        }
        if ((int)momentaryHistR.size() > momentaryWindowSize)
        {
            const double old = momentaryHistR.front(); momentaryHistR.pop_front();
            momentarySlideSumR -= old * old;
        }

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
                tpL.peak = juce::jmax (tpL.peak, std::abs (tpL.interpolate (ph, tpFirCoeffs)));
                tpR.peak = juce::jmax (tpR.peak, std::abs (tpR.interpolate (ph, tpFirCoeffs)));
            }
        }

        // ---- 100ms 子块结束 ----
        //   BS.1770-4 的门控块是 400ms 窗口、100ms 步进（相邻重叠 75%），
        //   旧实现用 400ms 非重叠块，统计样本数只有 1/4，I 会随素材偏差。
        if (momentaryWriteCount >= momentaryBlockSize)
        {
            const double e100L = momentarySumL / (double) momentaryBlockSize;
            const double e100R = momentarySumR / (double) momentaryBlockSize;
            // BS.1770-4：立体声两通道能量"相加"（G_L = G_R = 1.0），不是平均。

            // Short-term 环形队列：最近 30 个 100ms 子块 = 3.0 s（无门限）
            energy100L.push_back (e100L);
            energy100R.push_back (e100R);
            while ((int) energy100L.size() > shortTermSubBlocks)
            {
                energy100L.pop_front();
                energy100R.pop_front();
            }

            // Integrated 门控块：最近 4 个子块 = 400ms（与上一块重叠 300ms）
            if ((int) energy100L.size() >= gatingSubBlocks)
            {
                double sumL = 0.0, sumR = 0.0;
                const int n = (int) energy100L.size();
                for (int k = n - gatingSubBlocks; k < n; ++k)
                {
                    sumL += energy100L[(size_t) k];
                    sumR += energy100R[(size_t) k];
                }

                const double blockMean = sumL / (double) gatingSubBlocks
                                       + sumR / (double) gatingSubBlocks;

                // 绝对门限（-70 LUFS ≈ 1.1725e-7 线性均方）：
                //   低于门限的块不参与相对门限的计算。
                if (blockMean >= absGateThreshold)
                    gatedBlocks.push_back (blockMean);
            }

            momentarySumL = momentarySumR = 0.0;
            momentaryWriteCount = 0;

            updateSnapshot();

            // True Peak 改为按块统计：发布快照后清零，下一块重新累计。
            tpL.peak = 0.0f;
            tpR.peak = 0.0f;
        }
    }

    // ---- Integrated 静音自动重置 ----
    //   基于 100ms 滑动 RMS 的合成均方判断“无信号”：连续 silenceResetSeconds
    //   秒低于 -60 dBFS 门限时清空 integrated 积分，下次有信号重新累积。
    {
        const int rmsN = static_cast<int>(rmsHistL.size());
        if (rmsN > 0)
        {
            const double meanSq = (rmsSumL + rmsSumR) / (2.0 * static_cast<double>(rmsN));
            if (meanSq < silenceMeanSqThreshold)
            {
                silenceSamples += static_cast<double>(numSamples);
                if (silenceSamples >= sampleRate * silenceResetSeconds
                    && !gatedBlocks.empty())
                {
                    gatedBlocks.clear();
                    silenceSamples = 0.0;
                    updateSnapshot(); // 立即刷新快照，让 lufsI 归零
                }
            }
            else
            {
                silenceSamples = 0.0;
            }
        }
    }

    // 每次 pushStereo 都刷新 LUFS-M（实时滑动窗口；静音时 400ms 内归零）。
    //   这是修复"停止后 M 不归零"的关键：不再依赖 400ms 块凑满才更新。
    {
        const int n = juce::jmax(1, (int)momentaryHistL.size());
        const double msL = momentarySlideSumL / (double) n;
        const double msR = momentarySlideSumR / (double) n;
        const double ms  = msL + msR;
        const float lufsM = (ms >= absGateThreshold) ? linearToLUFS(ms) : -144.0f;
        const juce::SpinLock::ScopedLockType sl(snapshotLock);
        snapshot.lufsM = lufsM;
    }
}

// ==========================================================
// updateSnapshot —— 在音频线程每 100ms 子块结束时调用
// ==========================================================
void LoudnessMeter::updateSnapshot()
{
    Snapshot s;

    // Momentary（400ms 实时滑动窗口）
    {
        const int n = juce::jmax(1, (int)momentaryHistL.size());
        const double msL = momentarySlideSumL / (double) n;
        const double msR = momentarySlideSumR / (double) n;
        const double ms  = msL + msR;
        s.lufsM = (ms >= absGateThreshold) ? linearToLUFS(ms) : -144.0f;
    }

    // Short-term（最近 30 个 100ms 子块 = 3.0 s 的矩形滑动窗口，无门限）
    //   EBU Tech 3341：S 不应用 -70 LUFS 或相对 -10 LU 门限，窗口内容变小时
    //   读数应随之降低（旧实现把低能量块归零，相当于偷偷加了门限）。
    if (!energy100L.empty())
    {
        double sumL = 0.0, sumR = 0.0;
        for (auto v : energy100L) sumL += v;
        for (auto v : energy100R) sumR += v;
        const int n = (int) energy100L.size();
        const double meanS = (sumL + sumR) / (double) n;
        s.lufsS = (meanS >= absGateThreshold) ? linearToLUFS (meanS) : -144.0f;
    }

    // Integrated（BS.1770 两阶段门限）
    if (!gatedBlocks.empty())
    {
        // 相对门限：通过绝对门限的块的平均能量再 -10 LU（×0.1）
        double sum = 0.0;
        for (double e : gatedBlocks) sum += e;
        const double relativeGate = (sum / (double) gatedBlocks.size()) * 0.1;

        // 只对 blockMean >= 相对门限 的块求平均
        double gatedSum = 0.0;
        int    gatedCount = 0;
        for (double e : gatedBlocks)
        {
            if (e >= relativeGate) { gatedSum += e; ++gatedCount; }
        }
        if (gatedCount > 0)
        {
            s.lufsI = linearToLUFS(gatedSum / (double) gatedCount);
            s.integrated = true;
        }
    }

    // RMS（dBFS = 10·log10(均方)）。不能用 linearToLUFS —— 它带 -0.691 的
    //   LUFS 校准偏移，会让 RMS 读数偏低 0.691 dB。
    {
        const int n = juce::jmax(1, (int)rmsHistL.size());
        const double msL = rmsSumL / (double) n;
        const double msR = rmsSumR / (double) n;
        // RMS 是"电平条"用途，仍按 -45 dBFS 均方把本底噪声压成"无读数"；
        //   不能用 absGateThreshold（那是 -70 LUFS 的响度门限），否则
        //   loopback / 输入本底（约 -55 dBFS）会一直显示成有效 RMS。
        constexpr double rmsFloor = 3.162e-5;   // 10^(-45/10)
        s.rmsL = (msL >= rmsFloor) ? (float)(10.0 * std::log10(msL)) : -144.0f;
        s.rmsR = (msR >= rmsFloor) ? (float)(10.0 * std::log10(msR)) : -144.0f;
    }

    // True Peak：峰值比 RMS 高 crest factor（噪声约 10~15dB），用 -45dB 均方门限的
    //   开方（-45dB 幅度）会把本底峰值（约 -40dB）误判为有效值。改用 -35dB 幅度门限。
    const float tpGate = 1.78e-2f;  // 10^(-35/20)
    s.truePeakL = (tpL.peak >= tpGate)
        ? juce::Decibels::gainToDecibels(tpL.peak)
        : -144.0f;
    s.truePeakR = (tpR.peak >= tpGate)
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
