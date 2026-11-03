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

    // ---- 候选直方图（发布用）----
    //   逐帧 argmax 在"两个拍级只差几个百分点"时必然乱跳（实测 trap 的 92/138
    //   在 0.24 / 0.14 之间来回翻），所以发布改成"跨时间投票"。
    constexpr int    kMaxVotes        = 40;    // 约 20 秒（0.5s/次）
    constexpr int    kMinVotes        = 8;     // 至少要 4 秒的估计才敢发布
    constexpr double kVoteTolerance   = 0.02;  // 2% 内视为同一个假设
    constexpr double kHemiolaVoteRatio = 0.55; // 3:2 更快伙伴的票数门槛

    // ---- 第二路 onset（音级分布变化率）----
    //   权重把"一次和声变化"折算到和"一次中等力度的鼓击"同一量级。
    constexpr float kChromaOnsetWeight = 0.30f;

    // 起音强度的慢速均值（去基线）。时间常数约 3 秒（约 260 帧 @86fps）：
    //   比一拍长得多，所以不会把拍点本身减掉；只吃掉"恒定起伏"这个直流。
    constexpr float kOnsetBaselineAlpha = 0.004f;

    // 谱通量的慢速均值（用于判断"本帧是不是一次能量突增"）
    constexpr float kFluxMeanAlpha = 0.010f;

    // chroma 变化的门限：低于它视为频谱抖动，不算和声变化
    constexpr float kChromaOnsetFloor = 0.06f;

    // 纯谱通量包络的自相关峰值达到这个值 → 认为"打击乐型素材"，
    //   只用谱通量，不让 chroma 路参与（实测点击轨 0.63、织体 0.003）
    constexpr double kFluxPeriodicityGate = 0.10;

    // 八度逃生口：连续多少轮估计都坚持 2× / 0.5× 才接受（0.5s/轮 → 约 5 秒）
    constexpr int   kOctaveEscapeRounds = 10;

    // 半速修正：半周期得分达到当前周期的这个比例时，认为真正的一拍是更快的那层。
    //   0.80 的依据：候选里"2× 周期"经常因为整数对齐而虚高，而速度先验
    //   （120 BPM 附近）已经偏向更快的一侧；两者叠加后 0.8 是保守的可分点。
    constexpr double kHalfTempoPreference = 0.80;

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

    // bin → 音级映射（55 Hz–5 kHz）：第二路 onset 用
    binPc.assign ((size_t) fft.numBins(), -1);

    {
        const double pcFMin = 55.0;
        const double pcFMax = std::min (5000.0, sampleRate * 0.45);

        for (int b = 1; b < fft.numBins(); ++b)
        {
            const double f = (double) b * sampleRate / (double) windowSize;

            if (f < pcFMin || f > pcFMax)
                continue;

            const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
            binPc[(size_t) b] = ((int) std::lround (midi) % 12 + 12) % 12;
        }
    }

    chromaNow.fill (0.0f);
    chromaPrev.fill (0.0f);
    chromaPrevValid = false;
    onsetBaseline   = 0.0f;

    envCapacity = std::max (32, (int) std::ceil (frameRate * kEnvelopeSeconds));
    onsetEnv.assign ((size_t) envCapacity, 0.0f);
    envFlux.assign ((size_t) envCapacity, 0.0f);
    envLinear.assign ((size_t) envCapacity, 0.0f);
    envFluxLinear.assign ((size_t) envCapacity, 0.0f);
    envFluxZero.assign ((size_t) envCapacity, 0.0f);
    envZeroMean.assign ((size_t) envCapacity, 0.0f);
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
    voteWeight.clear();
    stableRounds     = 0;
    unstableCount    = 0;
    octaveHoldRounds = 0;

    totalSamples     = 0.0;
    totalSec         = 0.0;
    levelDb          = -100.0f;
    lastFrameWasLoud = false;
    salience         = 0.0f;

    peakMargin      = 0.0f;
    candidateSpread = 1.0f;
    confidenceValue = 0.0f;

    chromaNow.fill (0.0f);
    chromaPrev.fill (0.0f);
    chromaPrevValid = false;
    onsetBaseline   = 0.0f;
    fluxMean        = 0.0f;
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
        chromaPrevValid = false;          // 恢复播放后不要拿静音前的 chroma 做差
        onsetBaseline  *= 0.995f;
        fluxMean       *= 0.995f;
        pendingFlux     = 0.0f;
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

    // ---- 第二路 onset：音级分布变化率 ----
    //   谱通量只认"能量上升"。对没有鼓的织体（pad / 弦乐 / 长音铺底），
    //   每个频带的能量此起彼伏：任何一帧都有一半频带在升、一半在降，
    //   正增量之和几乎是常数 → 包络是一条直线 → 自相关为 0，永远出不了 BPM。
    //   但这类素材的音高/和弦仍在变化，所以再算一路 "chroma 向量的 L1 距离"，
    //   它不需要能量突增，只需要"音级分布变了"。
    chromaNow.fill (0.0f);

    for (int b = 0; b < fft.numBins(); ++b)
    {
        const int pc = binPc[(size_t) b];

        if (pc >= 0)
            chromaNow[(size_t) pc] += mag[(size_t) b];
    }

    float chromaSum = 0.0f;

    for (int i = 0; i < 12; ++i)
        chromaSum += chromaNow[(size_t) i];

    float chromaDist = 0.0f;

    if (chromaSum > 1.0e-9f)
    {
        float acc = 0.0f;

        for (int i = 0; i < 12; ++i)
        {
            chromaNow[(size_t) i] /= chromaSum;

            if (chromaPrevValid)
                acc += std::abs (chromaNow[(size_t) i] - chromaPrev[(size_t) i]);
        }

        chromaDist      = 0.5f * acc;          // 0..1
        chromaPrev      = chromaNow;
        chromaPrevValid = true;
    }
    else
    {
        chromaPrevValid = false;
    }

    // 两路合成后减掉慢速基线：只保留"相对最近 3 秒明显更突出"的变化。
    // 两路的取舍用"本帧是否是一次能量突增"来决定：
    //   · 有突增（鼓击）→ 只用谱通量。加了 chroma 反而有害：一次 click 的
    //     谐波衰减会让 chroma 在 burst 内部抖动，等于给干净的点击轨加噪，
    //     实测会把 90 BPM 判成"出不了结果"。
    //   · 没有突增（织体 / pad）→ 用 max(谱通量, chroma 变化)。
    const float fluxNorm = (float) (flux / (double) bandCount);

    fluxMean += (fluxNorm - fluxMean) * kFluxMeanAlpha;

    const bool percussive = fluxNorm > fluxMean * 2.0f + 1.0e-4f;

    // 不应期：一次鼓击之后约 3 帧内不使用 chroma 路。
    //   click 的谐波在衰减过程中会让 chroma 在"击后一帧"再抖一次，
    //   等于给干净的点击轨加了一个延迟 1 帧的假脉冲 —— 正是它把
    //   90 BPM 的点击轨判成了"出不了结果"。
    if (percussive)
        chromaRefractory = 3;

    const bool useChroma = (chromaRefractory <= 0);

    if (! useChroma)
        --chromaRefractory;

    // chroma 变化低于门限时视为"没有真正的和声变化"（只是频谱抖动），
    //   这样鼓类素材里 click 衰减造成的 1–3% 抖动不会被当成事件。
    const float chromaOnset = (chromaDist > kChromaOnsetFloor)
                                  ? kChromaOnsetWeight * chromaDist
                                  : 0.0f;

    const float onset = useChroma
                            ? std::max (fluxNorm, chromaOnset)
                            : fluxNorm;

    // 基线只跟随谱通量（稳态参考），不跟随 chroma 尖峰：
    //   否则鼓类素材的基线会被 chroma 抖动抬高，反过来削弱真实鼓点。
    onsetBaseline += (fluxNorm - onsetBaseline) * kOnsetBaselineAlpha;

    pendingFlux = fluxNorm;   // 供 estimateTempo 判断"该用哪一路包络"
    pushEnvelope (std::max (0.0f, onset - onsetBaseline));
}

void TempoEstimator::pushEnvelope (float value)
{
    if (envCapacity <= 0)
        return;

    onsetEnv[(size_t) envWrite] = value;
    envFlux[(size_t) envWrite]  = pendingFlux;

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
    {
        const int idx = (start + i) % envCapacity;

        envLinear[(size_t) i]     = onsetEnv[(size_t) idx];
        envFluxLinear[(size_t) i] = envFlux[(size_t) idx];
    }

    // ---- T1：自相关前去掉包络的直流分量（同时做 3 点平滑）----
    //   onset 包络恒为正，直流分量会让所有 lag 的相关系数一起被抬向 1.0，
    //   峰谷差被压扁，78 / 140 这类接近的候选就很难区分。
    //   平滑（[0.25 0.5 0.25]）解决的是另一件事：当拍周期不是整数帧
    //   （90 BPM @48k = 62.5 帧）时，相邻两拍的尖峰落在不同采样相位，
    //   整数 lag 的自相关会明显偏低，2× 周期反而满分。
    if ((int) envZeroMean.size() < n)
        envZeroMean.assign ((size_t) n, 0.0f);

    if ((int) envFluxZero.size() < n)
        envFluxZero.assign ((size_t) n, 0.0f);

    auto buildZeroMean = [&] (const std::vector<float>& src, std::vector<float>& dst)
    {
        double mean = 0.0;

        for (int i = 0; i < n; ++i)
            mean += (double) src[(size_t) i];

        mean /= (double) n;

        for (int i = 0; i < n; ++i)
        {
            const double prev = (double) src[(size_t) std::max (0, i - 1)] - mean;
            const double cur  = (double) src[(size_t) i] - mean;
            const double next = (double) src[(size_t) std::min (n - 1, i + 1)] - mean;

            dst[(size_t) i] = (float) (0.25 * prev + 0.5 * cur + 0.25 * next);
        }
    };

    buildZeroMean (envLinear, envZeroMean);
    buildZeroMean (envFluxLinear, envFluxZero);

    const int lagMin = std::max (2, (int) std::lround (frameRate * 60.0 / kMaxBpm));
    const int lagMax = std::min (n / 2, (int) std::lround (frameRate * 60.0 / kMinBpm));

    if (lagMax <= lagMin + 2)
        return;

    // ---- 选择用哪一路包络 ----
    //   打击乐：纯谱通量本身就有很强的周期性 → 只用它。
    //     把 chroma 路混进来会引入"击后一帧"的假脉冲，实测会让 90 BPM 的
    //     点击轨判不出结果。
    //   织体：谱通量的 ACF 峰值很低（没有瞬态）→ 改用含 chroma 的混合包络。
    {
        double peak = 0.0;

        for (int lag = lagMin; lag <= lagMax; ++lag)
        {
            double num = 0.0;
            double e0  = 0.0;
            double e1  = 0.0;

            for (int i = lag; i < n; ++i)
            {
                const double a = (double) envFluxZero[(size_t) i];
                const double b = (double) envFluxZero[(size_t) (i - lag)];

                num += a * b;
                e0  += a * a;
                e1  += b * b;
            }

            peak = std::max (peak, num / (std::sqrt (e0 * e1) + 1.0e-12));
        }

        if (peak >= kFluxPeriodicityGate)
            envZeroMean = envFluxZero;
    }

    double energy = 0.0;

    for (int i = 0; i < n; ++i)
        energy += (double) envZeroMean[(size_t) i] * (double) envZeroMean[(size_t) i];

    if (energy < 1.0e-12)
        return;

    const int acfMax = std::min (n - 2, lagMax * 2);

    for (int lag = 1; lag <= acfMax; ++lag)
    {
        double num = 0.0;
        double e0  = 0.0;
        double e1  = 0.0;

        for (int i = lag; i < n; ++i)
        {
            const double a = (double) envZeroMean[(size_t) i];
            const double b = (double) envZeroMean[(size_t) (i - lag)];

            num += a * b;
            e0  += a * a;
            e1  += b * b;
        }

        acf[(size_t) lag] = (float) (num / (std::sqrt (e0 * e1) + 1.0e-12));
    }

    const double weights[4] = { 1.0, 0.5, 0.33, 0.25 };

    // ---- 连续 lag 上的自相关（线性插值）----
    //   整数 lag 时与 acf[] 完全一致；引入它是因为真实周期经常落在两个整数
    //   lag 之间（90 BPM @48k = 62.5 帧）。只看整数 lag 时，2× 周期恰好是
    //   整数、自相关更完整，会把 90 BPM 判成 45 BPM。
    //   注意：这里必须是"直接按小数 lag 重算相关"，不能对整数 lag 的 ACF 做插值。
    //   90 BPM @48k 的周期正好是 62.5 帧，整数 62 / 63 各自都有半帧错位，
    //   acf[62] 只有 0.2 左右；对它们做线性插值仍然得到 0.2，看不出真实峰值。
    auto envAt = [&] (double x) -> double
    {
        if (x < 0.0 || x > (double) (n - 1))
            return 0.0;

        const int    i = (int) x;
        const double f = x - (double) i;

        const double a = (double) envZeroMean[(size_t) i];
        const double b = (i + 1 < n) ? (double) envZeroMean[(size_t) (i + 1)] : a;

        return a + (b - a) * f;
    };

    auto acfExact = [&] (double x) -> double
    {
        double num = 0.0;
        double e0  = 0.0;
        double e1  = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double a = (double) envZeroMean[(size_t) i];
            const double b = envAt ((double) i - x);

            num += a * b;
            e0  += a * a;
            e1  += b * b;
        }

        return num / (std::sqrt (e0 * e1) + 1.0e-12);
    };

    auto acfAt = [&] (double x) -> double
    {
        if (x < 1.0 || x > (double) acfMax)
            return 0.0;

        // 整数 lag 直接用已算好的表；小数 lag 才走 O(n) 重算
        const int nearest = (int) std::lround (x);

        if (std::abs (x - (double) nearest) < 1.0e-6)
            return (double) acf[(size_t) nearest];

        return acfExact (x);
    };

    auto combScoreAt = [&] (double lag) -> double
    {
        if (lag < 1.0)
            return 0.0;

        double sum  = 0.0;
        double wsum = 0.0;

        for (int m = 1; m <= 4; ++m)
        {
            const double l = lag * (double) m;

            if (l > (double) acfMax)
                break;

            sum  += weights[m - 1] * acfAt (l);
            wsum += weights[m - 1];
        }

        if (wsum <= 0.0)
            return 0.0;

        const double bpm = 60.0 * frameRate / lag;

        // 速度先验：**平台型**，不是以某点为中心的对数正态。
        //   95–170 BPM 是平台（斜率 = 0 → 对结果零偏置、零抖动）；
        //   区间外按对数正态衰减（σ = 0.30 八度），用于压掉"半速"。
        //
        //   为什么需要它：很多织体素材（和弦/铺底每 2 拍变化一次、1 拍位置
        //   没有事件）在 onset 包络里只有"半个周期"的强度，纯周期性判据
        //   必然给出半速，只有速度先验能救回来。
        //
        //   为什么不会把"真的慢"的曲子拉快：当慢速才是真实拍点时，快一倍的
        //   候选在自相关里几乎没有支撑（梳状得分差好几倍），先验盖不过去；
        //   只有"两拍 / 四拍都成立"的歧义素材才会被先验决定。
        //
        //   为什么必须是平台而不是尖峰：尖峰先验在平台区间外有明显斜率，
        //   会把 90–110 / 150–170 的结果持续往中心推 —— 实测表现为"偏 +5
        //   且数值来回跳"。
        double prior = 1.0;

        if (bpm < 95.0)
            prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 95.0) / 0.30, 2.0));
        else if (bpm > 170.0)
            prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 170.0) / 0.30, 2.0));

        return sum / wsum * (0.20 + 0.80 * prior);
    };

    for (int lag = lagMin; lag <= lagMax; ++lag)
        comb[(size_t) lag] = (float) combScoreAt ((double) lag);

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

    // ---- 整数 lag 的倍周期偏置修正 ----
    //   真实周期落在两个整数 lag 之间时（90 BPM @48k = 62.5 帧），2× 周期往往
    //   恰好是整数、自相关更完整；梳状平均又把 2×/3×/4× 的错位放大，于是
    //   90 BPM 会被判成 45 BPM。这里用连续打分比较"半周期 / 当前周期"：
    //   只有半周期得分确实更高（含速度先验）才降；真实 45 BPM 素材在半周期
    //   处没有 onset 能量，不会被误降。

    double lagF = (double) bestLag;

    while (lagF * 0.5 >= (double) lagMin)
    {
        const double half = lagF * 0.5;

        if (combScoreAt (half) < combScoreAt (lagF) * kHalfTempoPreference)
            break;

        lagF = half;
    }

    // 峰位细化：±1 帧内做加权质心，而不是 3 点抛物线。
    //   帧率只有 ~90Hz 时，150 BPM 的相邻两个整数 lag 只差 2.7%（≈4 BPM），
    //   峰顶几乎是平的 → 抛物线分母 ≈ 0、失去细化能力 → 结果在 148 / 152
    //   之间来回跳（实测正是用户反馈的"±5 抖动"）。
    //   质心在平顶处给出中点（≈150），把这种抖动消掉。
    double refinedLag = lagF;

    // 细化**只能用整数 lag 上的分数做抛物线**，不能用小数 lag 重算的自相关：
    //   小数 lag 是靠"包络线性插值"求相关，对 1–2 帧宽的尖峰会系统性衰减，
    //   于是 acf(37.5) 反而低于 acf(37)/acf(38)，峰变成双峰 →
    //   结果在 148 / 152 之间跳（150 BPM，lag=37.5）。
    //   整数三点抛物线对这种情况给出 37.5，正好是正确值。
    const int nearestLag = (int) std::lround (lagF);

    if (std::abs (lagF - (double) nearestLag) < 1.0e-6
        && nearestLag > lagMin && nearestLag < lagMax)
    {
        // 峰顶质心：只统计"接近峰顶（≥90%）"的整数 lag。
        //   150 BPM 的真实周期是 37.5 帧，comb[37] 与 comb[38] 几乎相等，
        //   三点抛物线的分母 ≈ 0 → 完全不细化 → 输出在 148/152 之间跳。
        //   取峰顶几个 Integer lag 的加权平均，平顶时正好给 37.5。
        const double peak = (double) comb[(size_t) nearestLag];
        const double threshold = peak * 0.90;

        double num = 0.0;
        double den = 0.0;

        for (int lag = nearestLag - 1; lag <= nearestLag + 1; ++lag)
        {
            if (lag < lagMin || lag > lagMax)
                continue;

            const double s = (double) comb[(size_t) lag] - threshold;

            if (s > 0.0)
            {
                num += (double) lag * s;
                den += s;
            }
        }

        if (den > 1.0e-9)
            refinedLag = num / den;
    }

    if (refinedLag <= 0.0)
        return;

    // ---- T2：拍格引导挑峰 + 中位间隔校正 ----
    //   抛物线只能细化到"连续打分"级；真正落在拍上的位置仍受包络帧量化影响。
    //   用拍格在包络上挑出真实拍峰、取相邻间隔中位数；只有与当前周期相差
    //   < 8% 时才采纳，避免把漏检 / 多检的峰当成新周期。
    double correctedLag = refinedLag;

    {
        const int P     = std::max (2, (int) std::lround (refinedLag));
        const int guard = std::max (1, P / 4);

        std::vector<double> phaseAcc ((size_t) P, 0.0);

        for (int i = 0; i < n; ++i)
            phaseAcc[(size_t) (i % P)] += (double) envLinear[(size_t) i];

        int phasePeak = 0;

        for (int i = 1; i < P; ++i)
            if (phaseAcc[(size_t) i] > phaseAcc[(size_t) phasePeak])
                phasePeak = i;

        std::vector<int> beats;
        beats.reserve ((size_t) (n / P + 2));

        for (int grid = phasePeak; grid < n; grid += P)
        {
            const int lo = std::max (0, grid - guard);
            const int hi = std::min (n - 1, grid + guard);

            int best = lo;

            for (int i = lo; i <= hi; ++i)
                if (envLinear[(size_t) i] > envLinear[(size_t) best])
                    best = i;

            if (beats.empty() || best > beats.back())
                beats.push_back (best);
        }

        if (beats.size() >= 5)
        {
            std::vector<double> intervals;
            intervals.reserve (beats.size());

            for (size_t k = 1; k < beats.size(); ++k)
            {
                const double d = (double) (beats[k] - beats[k - 1]);

                // 剔除漏检（约 2×）与多检（约 0.5×）
                if (d > refinedLag * 0.7 && d < refinedLag * 1.4)
                    intervals.push_back (d);
            }

            if (intervals.size() >= 4)
            {
                std::sort (intervals.begin(), intervals.end());

                const double med = intervals[intervals.size() / 2];

                if (std::abs (med - refinedLag) / refinedLag < 0.08)
                    correctedLag = med;
            }
        }
    }

    const double bpmRaw = 60.0 * frameRate / correctedLag;

    recentBpm.push_back ((float) bpmRaw);
    voteWeight.push_back (std::max (0.05f, bestScore));

    while ((int) recentBpm.size() > kMaxVotes)
    {
        recentBpm.erase (recentBpm.begin());
        voteWeight.erase (voteWeight.begin());
    }

    // 稳定性指标只用于置信度；发布不再依赖它
    if (recentBpm.size() >= 3)
    {
        const size_t cnt = recentBpm.size();
        const double a = recentBpm[cnt - 3];
        const double b = recentBpm[cnt - 2];
        const double c = recentBpm[cnt - 1];
        const double med = median3 (a, b, c);

        candidateSpread = (med > 0.0)
                              ? (float) (std::max ({ std::abs (a - med), std::abs (b - med),
                                                     std::abs (c - med) }) / med)
                              : 1.0f;
    }
    else
    {
        candidateSpread = 1.0f;
    }

    if ((int) recentBpm.size() < kMinVotes)
    {
        updateConfidence();
        return;
    }

    // ---- 候选直方图：2% 容差内累计票数（票重 = 该次估计的梳状得分）----
    auto votesNear = [&] (double bpm) -> double
    {
        double w = 0.0;

        for (size_t i = 0; i < recentBpm.size(); ++i)
            if (std::abs ((double) recentBpm[i] - bpm) <= bpm * kVoteTolerance)
                w += (double) voteWeight[i];

        return w;
    };

    double bestBpm    = 0.0;
    double bestWeight = 0.0;

    for (size_t i = 0; i < recentBpm.size(); ++i)
    {
        const double w = votesNear ((double) recentBpm[i]);

        if (w > bestWeight)
        {
            bestWeight = w;
            bestBpm    = (double) recentBpm[i];
        }
    }

    if (bestBpm <= 0.0)
    {
        updateConfidence();
        return;
    }

    // ---- 3:2 修正（在累计票数上做，而不是单帧）----
    //   trap / plugg 的 92 vs 138：单帧里 92 常赢，但 138 一直有票；
    //   若快 1.5 倍的那个假设累计到 55% 的票，就采用它。
    {
        const double bpmMin = 60.0 * frameRate / (double) lagMax;
        const double bpmMax = 60.0 * frameRate / (double) lagMin;

        const double faster = bestBpm * 1.5;
        const double slower = bestBpm / 1.5;

        if (faster <= bpmMax && votesNear (faster) >= bestWeight * kHemiolaVoteRatio)
            bestBpm = faster;
        else if (slower >= bpmMin && votesNear (slower) >= bestWeight * kHemiolaVoteRatio)
            bestBpm = slower;
    }

    double candidate = bestBpm;

    // 收敛平滑：相邻整数 lag 之间的往返（150 BPM 的 37/38 帧 ≈ 148/152）
    //   单帧层面无可分辨的差异，直接发布就会看到 ±4 BPM 的来回跳。
    //   差异在 5% 以内时用 EMA 收敛，跳变（真正换速）仍走原路径。
    if (published && publishedBpm > 0.0f
        && std::abs (candidate - (double) publishedBpm) <= (double) publishedBpm * 0.05)
    {
        candidate = 0.70 * (double) publishedBpm + 0.30 * candidate;
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
