#include "source/analysis/music/TempoEstimator.h"

#include <algorithm>
#include <cmath>

namespace y2k::music
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // 周期搜索区间（不是结果修正区间）
    constexpr double kMinBpm = 45.0;
    constexpr double kMaxBpm = 220.0;

    constexpr double kEnvelopeSeconds    = 8.0;
    constexpr double kMinEnvelopeSeconds = 4.0;
    constexpr double kEstimateInterval   = 0.5;

    // 电平门限：低于此值的帧不产生 onset（-55 dBFS）
    constexpr float kLowLevelDb = -55.0f;

    // 归一化自相关得分下限（低于此值认为"没有可解释的周期性"）
    constexpr float kMinSalience = 0.15f;

    // 稳定性判定：最近 3 次估计的最大相对离散度
    constexpr float kStableSpread = 0.04f;
    constexpr int   kMaxRecent    = 5;
    constexpr int   kBandCount    = 24;

    double median3 (double a, double b, double c) noexcept
    {
        if (a > b) std::swap (a, b);
        if (b > c) std::swap (b, c);
        if (a > b) std::swap (a, b);
        return b;
    }
}

void TempoEstimator::prepare (double newSampleRate)
{
    sampleRate = std::clamp (newSampleRate, 8000.0, 384000.0);

    // 46 ms 分析窗（44.1k → 2048），hop = 1/4 窗（约 11.6 ms）
    windowSize = nearestPowerOfTwo ((int) std::lround (sampleRate * 0.046));
    windowSize = std::clamp (windowSize, 256, 16384);
    hopSize    = std::max (64, windowSize / 4);
    frameRate  = sampleRate / (double) hopSize;

    fft.prepare (windowSize);

    window.resize ((size_t) windowSize);

    for (int i = 0; i < windowSize; ++i)
    {
        const double t = (windowSize > 1) ? (double) i / (double) (windowSize - 1) : 0.0;
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * t));
    }

    frameBuf.assign ((size_t) windowSize, 0.0f);
    mag.assign ((size_t) fft.numBins(), 0.0f);

    // 对数频带映射：40 Hz – 16 kHz 均分 24 个频带
    bandCount = kBandCount;
    bandNow.assign ((size_t) bandCount, 0.0f);
    bandPrev.assign ((size_t) bandCount, 0.0f);
    binBand.assign ((size_t) fft.numBins(), -1);

    const double fMin = 40.0;
    const double fMax = std::min (16000.0, sampleRate * 0.5);

    if (fMax > fMin * 2.0)
    {
        const double logSpan = std::log (fMax / fMin);

        for (int b = 0; b < fft.numBins(); ++b)
        {
            const double f = (double) b * sampleRate / (double) windowSize;

            if (f < fMin || f > fMax)
                continue;

            const double t = std::log (f / fMin) / logSpan;
            binBand[(size_t) b] = std::clamp ((int) (t * (double) bandCount),
                                              0, bandCount - 1);
        }
    }

    envCapacity = std::max (32, (int) std::ceil (frameRate * kEnvelopeSeconds));
    onsetEnv.assign ((size_t) envCapacity, 0.0f);
    envLinear.assign ((size_t) envCapacity, 0.0f);
    acf.assign ((size_t) envCapacity + 2, 0.0f);
    comb.assign ((size_t) envCapacity + 2, 0.0f);

    estimateEveryFrames = std::max (1, (int) std::lround (frameRate * kEstimateInterval));
    minEstimateFrames   = std::max (8, (int) std::ceil (frameRate * kMinEnvelopeSeconds));

    hist.assign ((size_t) windowSize, 0.0f);

    resetAccumulation();

    published    = false;
    publishedBpm = 0.0f;
    changeAtSec  = 0.0;
}

void TempoEstimator::resetAccumulation()
{
    std::fill (hist.begin(), hist.end(), 0.0f);
    std::fill (bandNow.begin(), bandNow.end(), 0.0f);
    std::fill (bandPrev.begin(), bandPrev.end(), 0.0f);
    std::fill (onsetEnv.begin(), onsetEnv.end(), 0.0f);

    histWrite = 0;
    hopFill   = 0;
    envWrite  = 0;
    envTotal  = 0;

    framesSinceEstimate = 0;
    recentBpm.clear();
    stableRounds  = 0;
    unstableCount = 0;

    totalSamples     = 0.0;
    totalSec         = 0.0;
    levelDb          = -100.0f;
    lastFrameWasLoud = false;
    salience         = 0.0f;

    peakMargin      = 0.0f;
    candidateSpread = 1.0f;
    confidenceValue = 0.0f;
}

void TempoEstimator::reset()
{
    resetAccumulation();

    published    = false;
    publishedBpm = 0.0f;
    changeAtSec  = 0.0;
}

void TempoEstimator::dropPendingFrame() noexcept
{
    hopFill = 0;
    std::fill (hist.begin(), hist.end(), 0.0f);
}

void TempoEstimator::pushAudio (const float* mono, int numSamples)
{
    if (mono == nullptr || numSamples <= 0 || windowSize <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        hist[(size_t) histWrite] = mono[i];

        if (++histWrite >= windowSize)
            histWrite = 0;

        if (++hopFill >= hopSize)
        {
            hopFill = 0;
            processFrame();
        }
    }
}

void TempoEstimator::processFrame()
{
    // histWrite 指向最旧样本 → 从它开始读就是"从旧到新"
    for (int i = 0; i < windowSize; ++i)
        frameBuf[(size_t) i] = hist[(size_t) ((histWrite + i) % windowSize)];

    double sumSq = 0.0;

    for (int i = 0; i < windowSize; ++i)
        sumSq += (double) frameBuf[(size_t) i] * (double) frameBuf[(size_t) i];

    levelDb = (float) (10.0 * std::log10 (sumSq / (double) windowSize + 1.0e-12));
    lastFrameWasLoud = levelDb > kLowLevelDb;

    totalSamples += (double) hopSize;
    totalSec = totalSamples / sampleRate;

    if (! lastFrameWasLoud)
    {
        // 静音帧推 0，让包络自然衰减；不做 FFT，也不产生 onset。
        std::fill (bandNow.begin(), bandNow.end(), 0.0f);
        std::fill (bandPrev.begin(), bandPrev.end(), 0.0f);
        pushEnvelope (0.0f);
        return;
    }

    for (int i = 0; i < windowSize; ++i)
        frameBuf[(size_t) i] *= window[(size_t) i];

    fft.magnitude (frameBuf.data(), mag.data());

    std::fill (bandNow.begin(), bandNow.end(), 0.0f);

    for (int b = 0; b < fft.numBins(); ++b)
    {
        const int band = binBand[(size_t) b];

        if (band >= 0)
            bandNow[(size_t) band] += mag[(size_t) b];
    }

    // 频带能量的对数差分，取正半波（spectral flux）
    double flux = 0.0;

    for (int band = 0; band < bandCount; ++band)
    {
        const float now  = std::log1p (60.0f * bandNow[(size_t) band]);
        const float prev = std::log1p (60.0f * bandPrev[(size_t) band]);

        if (now > prev)
            flux += (double) (now - prev);

        bandPrev[(size_t) band] = now;
    }

    pushEnvelope ((float) (flux / (double) bandCount));
}

void TempoEstimator::pushEnvelope (float value)
{
    if (envCapacity <= 0)
        return;

    onsetEnv[(size_t) envWrite] = value;

    if (++envWrite >= envCapacity)
        envWrite = 0;

    ++envTotal;

    if (++framesSinceEstimate >= estimateEveryFrames)
    {
        framesSinceEstimate = 0;

        if (envTotal >= minEstimateFrames)
            estimateTempo();
    }
}

void TempoEstimator::estimateTempo()
{
    const int n = std::min ((int) envTotal, envCapacity);

    if (n < minEstimateFrames || envCapacity <= 0)
        return;

    const int start = ((envWrite - n) % envCapacity + envCapacity) % envCapacity;

    for (int i = 0; i < n; ++i)
        envLinear[(size_t) i] = onsetEnv[(size_t) ((start + i) % envCapacity)];

    double energy = 0.0;

    for (int i = 0; i < n; ++i)
        energy += (double) envLinear[(size_t) i] * (double) envLinear[(size_t) i];

    if (energy < 1.0e-9)
        return;

    const int lagMin = std::max (2, (int) std::lround (frameRate * 60.0 / kMaxBpm));
    const int lagMax = std::min (n / 2, (int) std::lround (frameRate * 60.0 / kMinBpm));

    if (lagMax <= lagMin + 2)
        return;

    const int acfMax = std::min (n - 2, lagMax * 2);

    for (int lag = 1; lag <= acfMax; ++lag)
    {
        double num = 0.0;
        double e0  = 0.0;
        double e1  = 0.0;

        for (int i = lag; i < n; ++i)
        {
            const double a = (double) envLinear[(size_t) i];
            const double b = (double) envLinear[(size_t) (i - lag)];

            num += a * b;
            e0  += a * a;
            e1  += b * b;
        }

        acf[(size_t) lag] = (float) (num / (std::sqrt (e0 * e1) + 1.0e-12));
    }

    const double weights[4] = { 1.0, 0.5, 0.33, 0.25 };

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        double sum = 0.0;
        double wsum = 0.0;

        for (int m = 1; m <= 4; ++m)
        {
            const int l = lag * m;

            if (l > acfMax)
                break;

            sum  += weights[m - 1] * (double) acf[(size_t) l];
            wsum += weights[m - 1];
        }

        if (wsum <= 0.0)
        {
            comb[(size_t) lag] = 0.0f;
            continue;
        }

        const double bpm   = 60.0 * frameRate / (double) lag;
        const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.85, 2.0));

        // 先验只作为轻微偏置（0.55–1.0），不强行把结果拉到 120。
        comb[(size_t) lag] = (float) (sum / wsum * (0.55 + 0.45 * prior));
    }

    float bestScore = -1.0f;
    int   bestLag   = lagMin;

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        const float s = comb[(size_t) lag];

        const bool localMax = (lag <= lagMin || s >= comb[(size_t) (lag - 1)])
                           && (lag >= lagMax || s > comb[(size_t) (lag + 1)]);

        if (localMax && s > bestScore)
        {
            bestScore = s;
            bestLag   = lag;
        }
    }

    salience = bestScore;

    // 第二峰：BPM 与最佳值相差 > 8% 的局部极大。
    //   两者越接近，说明"这个周期"越不独特（例如 120 与 118 同时成立），
    //   置信度应该越低。
    {
        const double bestBpmRef = 60.0 * frameRate / (double) std::max (1, bestLag);
        float secondScore = 0.0f;

        for (int lag = lagMin; lag <= lagMax; ++lag)
        {
            const float s = comb[(size_t) lag];

            const bool localMax = (lag <= lagMin || s >= comb[(size_t) (lag - 1)])
                               && (lag >= lagMax || s > comb[(size_t) (lag + 1)]);

            if (! localMax)
                continue;

            const double bpm = 60.0 * frameRate / (double) lag;

            if (std::abs (bpm - bestBpmRef) / bestBpmRef <= 0.08)
                continue;

            secondScore = std::max (secondScore, s);
        }

        peakMargin = (bestScore > 1.0e-6f)
                         ? std::clamp ((bestScore - secondScore) / bestScore, 0.0f, 1.0f)
                         : 0.0f;
    }

    if (bestScore < kMinSalience)
    {
        candidateSpread = 1.0f;   // 没有可信候选 → 稳定度计 0

        if (++unstableCount > 4)
        {
            recentBpm.clear();
            stableRounds = 0;
        }

        updateConfidence();
        return;
    }

    unstableCount = 0;

    // 抛物线插值：把周期细化到亚帧精度
    double refinedLag = (double) bestLag;

    if (bestLag > lagMin && bestLag < lagMax)
    {
        const double y0 = (double) comb[(size_t) (bestLag - 1)];
        const double y1 = (double) comb[(size_t) bestLag];
        const double y2 = (double) comb[(size_t) (bestLag + 1)];
        const double denom = y0 - 2.0 * y1 + y2;

        if (std::abs (denom) > 1.0e-9)
            refinedLag += std::clamp (0.5 * (y0 - y2) / denom, -0.5, 0.5);
    }

    if (refinedLag <= 0.0)
        return;

    const double bpmRaw = 60.0 * frameRate / refinedLag;

    recentBpm.push_back ((float) bpmRaw);

    while ((int) recentBpm.size() > kMaxRecent)
        recentBpm.erase (recentBpm.begin());

    if (recentBpm.size() < 3)
    {
        candidateSpread = 1.0f;
        updateConfidence();
        return;
    }

    const size_t cnt = recentBpm.size();
    const double a = recentBpm[cnt - 3];
    const double b = recentBpm[cnt - 2];
    const double c = recentBpm[cnt - 1];
    const double med = median3 (a, b, c);

    if (med <= 0.0)
    {
        candidateSpread = 1.0f;
        updateConfidence();
        return;
    }

    const double maxDev = std::max ({ std::abs (a - med), std::abs (b - med), std::abs (c - med) });

    candidateSpread = (float) (maxDev / med);

    if (candidateSpread > (float) kStableSpread)
    {
        stableRounds = 0;
        updateConfidence();
        return;
    }

    if (++stableRounds < 2)
    {
        updateConfidence();
        return;
    }

    double candidate = med;

    // 八度迟滞：候选恰好是已发布值的 2 倍 / 半速时不翻转，避免 70↔140 抖动。
    if (published && publishedBpm > 0.0f)
    {
        const double ratio = candidate / (double) publishedBpm;
        const double octaveEps = 0.06;

        if (std::abs (ratio - 2.0) < octaveEps || std::abs (ratio - 0.5) < octaveEps)
            candidate = (double) publishedBpm;
    }

    const bool changed = ! published
                      || std::abs (candidate - (double) publishedBpm) > 0.5;

    publishedBpm = (float) candidate;

    if (changed || ! published)
        changeAtSec = totalSec;

    published = true;

    updateConfidence();
}

// ==========================================================
// 置信度（启发式，不是概率）
//
//   base = 0.35·周期性 + 0.40·候选稳定度 + 0.25·最佳/次佳峰间隔
//   conf = base × (0.20 + 0.80·数据充分度)
//
//   · 周期性   ：归一化自相关得分，0.15（发布门限）→ 0，0.60 → 1
//   · 候选稳定度：最近 3 个候选相对中值的最大偏差，0 → 1，4%（门限）→ 0
//   · 峰间隔   ：最佳峰与"不同 BPM 的第二峰"的相对差，25% → 1
//   · 数据充分度：已分析时长 / 20s —— 这就是"刚开始读数不可靠"的直接体现
//
//   另外：最新候选已偏离已发布值 > 3% 时再乘 0.6（读数正在失效）。
// ==========================================================
void TempoEstimator::updateConfidence()
{
    if (! published || publishedBpm <= 0.0f)
    {
        confidenceValue = 0.0f;
        return;
    }

    const double dataFactor = std::clamp (totalSec / 20.0, 0.0, 1.0);
    const double periodic   = std::clamp (((double) salience - 0.15) / 0.45, 0.0, 1.0);
    const double stability  = std::clamp (1.0 - (double) candidateSpread / 0.04, 0.0, 1.0);
    const double marginF    = std::clamp ((double) peakMargin / 0.25, 0.0, 1.0);

    const double base = 0.35 * periodic + 0.40 * stability + 0.25 * marginF;

    double conf = base * (0.20 + 0.80 * dataFactor);

    if (! recentBpm.empty())
    {
        const double dev = std::abs ((double) recentBpm.back() - (double) publishedBpm)
                         / (double) publishedBpm;

        if (dev > 0.03)
            conf *= 0.6;
    }

    confidenceValue = (float) std::clamp (conf, 0.0, 1.0);
}

} // namespace y2k::music
