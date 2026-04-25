#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// PhaseCorrelator —— 立体声相位相关 & 宽度
//
// 使用 EMA（exponential moving average）估计一阶矩，避免
// 维护大量历史样本。时间常数 ~50ms，与人耳感知一致。
// ==========================================================

void PhaseCorrelator::prepare(double sampleRate)
{
    // EMA 一阶低通：tau = 50ms
    const double tau = 0.050;
    alpha = 1.0 - std::exp(-1.0 / (tau * juce::jmax(1.0, sampleRate)));
    reset();
}

void PhaseCorrelator::reset()
{
    sLL = sRR = sLR = sMM = sSS = 0.0;

    const juce::SpinLock::ScopedLockType sl(snapLock);
    snap = Snapshot{};
}

void PhaseCorrelator::pushStereo(const float* left, const float* right, int numSamples)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    const double a = alpha;
    const double oneMinusA = 1.0 - a;

    double lLocal = sLL, rLocal = sRR, lrLocal = sLR;
    double mmLocal = sMM, ssLocal = sSS;

    for (int i = 0; i < numSamples; ++i)
    {
        const double L = left[i];
        const double R = right[i];
        const double M = (L + R) * 0.5;
        const double S = (L - R) * 0.5;

        lLocal  = oneMinusA * lLocal  + a * L * L;
        rLocal  = oneMinusA * rLocal  + a * R * R;
        lrLocal = oneMinusA * lrLocal + a * L * R;
        mmLocal = oneMinusA * mmLocal + a * M * M;
        ssLocal = oneMinusA * ssLocal + a * S * S;
    }

    sLL = lLocal; sRR = rLocal; sLR = lrLocal;
    sMM = mmLocal; sSS = ssLocal;

    // 计算快照
    Snapshot ns;
    const double denom = std::sqrt(juce::jmax(1.0e-20, sLL * sRR));
    ns.correlation = (float) juce::jlimit(-1.0, 1.0, sLR / denom);

    const double totalMS = sMM + sSS;
    ns.width = (totalMS > 1.0e-20)
                 ? (float) juce::jlimit(0.0, 1.0, sSS / totalMS)
                 : 0.0f;

    const double totalLR = sLL + sRR;
    ns.balance = (totalLR > 1.0e-20)
                   ? (float) juce::jlimit(-1.0, 1.0, (sRR - sLL) / totalLR)
                   : 0.0f;

    const juce::SpinLock::ScopedLockType sl(snapLock);
    snap = ns;
}

PhaseCorrelator::Snapshot PhaseCorrelator::getSnapshot() const
{
    const juce::SpinLock::ScopedLockType sl(snapLock);
    return snap;
}
