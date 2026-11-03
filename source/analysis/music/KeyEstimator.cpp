#include "source/analysis/music/KeyEstimator.h"

#include <algorithm>
#include <cmath>

namespace y2k::music
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // Krumhansl-Kessler 调性轮廓（音级 1..12 相对于主音）
    constexpr float kMajorProfile[12] =
    {
        6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
        2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
    };

    // 自然小调（对应 Krumhansl-Kessler minor）
    constexpr float kMinorProfile[12] =
    {
        6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
        2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
    };

    // ---- 和声小调 / 旋律小调 ----
    //   说明：KK 只给出大调与自然小调两组实测轮廓，没有和声/旋律小调版本。
    //   这里以 KK 自然小调为基底，把"升高音级"的权重从导音/下中音转移过来，
    //   属于**可解释的构造**而不是实测数据：
    //     · 和声小调：7 级升高（A 小调的 G#）→ 抬高 index 11，压低 index 10；
    //     · 旋律小调：6、7 级升高（F#、G#）→ 抬高 index 9/11，
    //                 6 级（b6）保留一部分权重（下行回到自然小调的用法）。
    //   因此"和声/旋律小调"的判定会比大/小调更保守（见 kModeSwitchMargin）。
    constexpr float kHarmonicMinorProfile[12] =
    {
        6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
        2.54f, 4.75f, 3.98f, 2.60f, 2.10f, 5.10f
    };

    constexpr float kMelodicMinorProfile[12] =
    {
        6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
        2.54f, 4.75f, 3.10f, 4.20f, 2.10f, 4.60f
    };

    constexpr float kLowLevelDb = -55.0f;

    // 判定"当前窗口是否支持一个明确的调性"的相关性下限。
    // 这个数字只用于内部判定，不代表准确率。
    constexpr float kMinKeyCorrelation = 0.30f;

    // 关系大小调之间需要更大的分数差距才允许切换（C major ↔ A minor）。
    constexpr float kRelativeSwitchMargin = 0.12f;
    constexpr float kOtherSwitchMargin    = 0.05f;

    // 和声/旋律小调必须先明显胜过同主音的自然小调，才允许改标签。
    //   （三种小调的轮廓只差两三个音级，不加这道门限会来回跳。）
    constexpr float kModeSwitchMargin = 0.03f;

    constexpr float kChromaFMinHz = 55.0f;    // A1
    constexpr float kChromaFMaxHz = 5000.0f;

    // 关系大小调判定（三种小调都算"小调"一侧）
    bool isRelativePair (int rootA, Mode modeA, int rootB, Mode modeB) noexcept
    {
        if (rootA < 0 || rootB < 0)
            return false;

        const bool minorA = isMinorMode (modeA);
        const bool minorB = isMinorMode (modeB);

        if (minorA == minorB)
            return false;

        // 大调的相对小调 = 主音 + 9 个半音
        const int majorRoot = minorA ? rootB : rootA;
        const int minorRoot = minorA ? rootA : rootB;

        return ((majorRoot + 9) % 12) == minorRoot;
    }
}

void KeyEstimator::prepare (double newSampleRate)
{
    sampleRate = std::clamp (newSampleRate, 8000.0, 384000.0);

    // 约 186 ms 分析窗（44.1k → 8192），hop = 1/4 窗（约 46 ms）
    windowSize = nearestPowerOfTwo ((int) std::lround (sampleRate * 0.186));
    windowSize = std::clamp (windowSize, 2048, 32768);
    hopSize    = std::max (256, windowSize / 4);
    frameHopSec = (double) hopSize / sampleRate;

    fft.prepare (windowSize);

    window.resize ((size_t) windowSize);

    for (int i = 0; i < windowSize; ++i)
    {
        const double t = (windowSize > 1) ? (double) i / (double) (windowSize - 1) : 0.0;
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * t));
    }

    frameBuf.assign ((size_t) windowSize, 0.0f);
    mag.assign ((size_t) fft.numBins(), 0.0f);
    hist.assign ((size_t) windowSize, 0.0f);

    // 上限固定按最大窗口（30s）分配，运行期按 windowSec 淘汰 → 内存有界。
    constexpr double kMaxWindowSec = 30.0;
    frameCapacity = std::max (8, (int) std::ceil (kMaxWindowSec / std::max (1.0e-4, frameHopSec)) + 2);
    frames.assign ((size_t) frameCapacity, ChromaFrame {});

    reset();
}

void KeyEstimator::resetAccumulation()
{
    std::fill (hist.begin(), hist.end(), 0.0f);

    histWrite = 0;
    hopFill   = 0;

    frameHead  = 0;
    frameCount = 0;

    secondsSinceUpdate = 0.0;

    totalSamples = 0.0;
    totalSec     = 0.0;
    levelDb      = -100.0f;

    pendingRoot  = -1;
    pendingMode  = Mode::major;
    pendingVotes = 0;

    keyConfidence = 0.0f;

    // 保留 publishedRoot / publishedMode（UI 可以继续显示"旧结果"并标注时效），
    // 但当前窗口不再支持它 → 不能算作有效结果。
    confident = false;
}

void KeyEstimator::reset()
{
    resetAccumulation();

    published      = false;
    publishedRoot  = -1;
    publishedMode  = Mode::major;
    confident      = false;
    changeAtSec    = 0.0;
    lastBestScore  = 0.0f;
    keyConfidence  = 0.0f;
}

void KeyEstimator::setWindowSeconds (double seconds) noexcept
{
    // 15–30 秒是文档给出的合理起点；这里允许 8–30 秒的可配置范围。
    windowSec = std::clamp (seconds, 8.0, 30.0);
    minWindowSec = std::min (8.0, windowSec);
}

void KeyEstimator::dropPendingFrame() noexcept
{
    hopFill = 0;
    std::fill (hist.begin(), hist.end(), 0.0f);
}

void KeyEstimator::pushAudio (const float* mono, int numSamples)
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

void KeyEstimator::processFrame()
{
    for (int i = 0; i < windowSize; ++i)
        frameBuf[(size_t) i] = hist[(size_t) ((histWrite + i) % windowSize)];

    double sumSq = 0.0;

    for (int i = 0; i < windowSize; ++i)
        sumSq += (double) frameBuf[(size_t) i] * (double) frameBuf[(size_t) i];

    levelDb = (float) (10.0 * std::log10 (sumSq / (double) windowSize + 1.0e-12));

    totalSamples += (double) hopSize;
    totalSec = totalSamples / sampleRate;

    if (levelDb <= kLowLevelDb)
    {
        secondsSinceUpdate += frameHopSec;

        if (secondsSinceUpdate >= updateIntervalSec)
        {
            secondsSinceUpdate = 0.0;
            updateKey();
        }

        return;
    }

    for (int i = 0; i < windowSize; ++i)
        frameBuf[(size_t) i] *= window[(size_t) i];

    fft.magnitude (frameBuf.data(), mag.data());

    float chroma[12] = {};

    const double binHz = sampleRate / (double) windowSize;
    const double fMax  = std::min ((double) kChromaFMaxHz, sampleRate * 0.45);
    const int    binLo = std::max (1, (int) std::ceil ((double) kChromaFMinHz / binHz));
    const int    binHi = std::min (fft.numBins() - 1, (int) std::floor (fMax / binHz));

    if (binHi > binLo)
    {
        for (int b = binLo; b <= binHi; ++b)
        {
            const float m = mag[(size_t) b];

            if (m <= 0.0f)
                continue;

            const double f = (double) b * binHz;

            // MIDI 音高 → 音级；在相邻两个音级之间线性分配，减小 bin 量化误差。
            const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
            const double pcF  = midi - 12.0 * std::floor (midi / 12.0);
            const int    p0   = ((int) std::floor (pcF)) % 12;
            const float  frac = (float) (pcF - std::floor (pcF));

            // 轻微压低高频，避免高次谐波（五度/大三度）主导 chroma。
            const float weight = (float) (1.0 / std::sqrt (std::max (0.5, f / 220.0)));

            chroma[p0]             += m * (1.0f - frac) * weight;
            chroma[(p0 + 1) % 12]  += m * frac * weight;
        }
    }

    double chromaSum = 0.0;

    for (int i = 0; i < 12; ++i)
        chromaSum += (double) chroma[i];

    if (chromaSum > 1.0e-9)
    {
        ChromaFrame f;
        f.t = totalSec;

        // 每帧按电平加权：安静段落的 chroma 不应该和主歌一样重。
        f.weight = std::clamp ((levelDb + 50.0f) / 30.0f, 0.0f, 1.0f);

        for (int i = 0; i < 12; ++i)
            f.c[i] = (float) ((double) chroma[i] / chromaSum);

        if (frameCount >= frameCapacity)
        {
            frameHead = (frameHead + 1) % frameCapacity;
            --frameCount;
        }

        const int writeIndex = (frameHead + frameCount) % frameCapacity;
        frames[(size_t) writeIndex] = f;
        ++frameCount;
    }

    secondsSinceUpdate += frameHopSec;

    if (secondsSinceUpdate >= updateIntervalSec)
    {
        secondsSinceUpdate = 0.0;
        updateKey();
    }
}

void KeyEstimator::updateKey()
{
    if (frameCount <= 0)
        return;

    // 按时间淘汰窗口外的旧帧 —— 转调后旧调性会被逐步挤出窗口。
    while (frameCount > 0)
    {
        const ChromaFrame& oldest = frames[(size_t) frameHead];

        if (totalSec - oldest.t <= windowSec)
            break;

        frameHead = (frameHead + 1) % frameCapacity;
        --frameCount;
    }

    if (frameCount < 4)
        return;

    const double covered = totalSec - frames[(size_t) frameHead].t;

    if (covered < minWindowSec)
        return;

    float chroma[12] = {};
    double weightSum = 0.0;

    for (int i = 0; i < frameCount; ++i)
    {
        const ChromaFrame& f = frames[(size_t) ((frameHead + i) % frameCapacity)];

        for (int pc = 0; pc < 12; ++pc)
            chroma[pc] += f.c[pc] * f.weight;

        weightSum += (double) f.weight;
    }

    if (weightSum <= 1.0e-6)
        return;

    double chromaSum = 0.0;

    for (int pc = 0; pc < 12; ++pc)
        chromaSum += (double) chroma[pc];

    if (chromaSum <= 1.0e-9)
        return;

    for (int pc = 0; pc < 12; ++pc)
        chroma[pc] = (float) ((double) chroma[pc] / chromaSum);

    // ---- 24 个候选：12 主音 × {大调, 自然小调, 和声小调, 旋律小调} ----
    struct Cand
    {
        float score = -2.0f;
        int   root  = 0;
        Mode  mode  = Mode::major;
    };

    Cand cands[24];
    int  candCount = 0;

    for (int root = 0; root < 12; ++root)
    {
        const float maj  = profileScore (chroma, root, Mode::major);
        const float nat  = profileScore (chroma, root, Mode::naturalMinor);
        const float harm = profileScore (chroma, root, Mode::harmonicMinor);
        const float mel  = profileScore (chroma, root, Mode::melodicMinor);

        // 小调一侧默认自然小调；和声/旋律小调必须明显更好才改标签，
        // 否则三种小调会互相抢票（它们的轮廓只差两三个音级）。
        float minorScore = nat;
        Mode  minorMode  = Mode::naturalMinor;

        if (harm > minorScore + kModeSwitchMargin)
        {
            minorScore = harm;
            minorMode  = Mode::harmonicMinor;
        }

        if (mel > minorScore + kModeSwitchMargin)
        {
            minorScore = mel;
            minorMode  = Mode::melodicMinor;
        }

        cands[candCount++] = { maj,        root, Mode::major };
        cands[candCount++] = { minorScore, root, minorMode };
    }

    Cand best;

    for (int i = 0; i < candCount; ++i)
        if (cands[i].score > best.score)
            best = cands[i];

    // 第二名只统计"不同主音"的候选：同主音的小调变体属于"调式不确定"，
    //   已经由 kModeSwitchMargin 处理，不应该再压低调性置信度。
    float secondDistinct = -2.0f;

    for (int i = 0; i < candCount; ++i)
        if (cands[i].root != best.root && cands[i].score > secondDistinct)
            secondDistinct = cands[i].score;

    const int   candRoot  = best.root;
    const Mode  candMode  = best.mode;
    const float candScore = best.score;

    lastBestScore = candScore;

    // ---- 置信度（启发式，不是概率）----
    //   base = 0.60·轮廓相关度 + 0.40·与"不同主音"次佳的差距
    //   conf = base × (0.20 + 0.80·窗口覆盖度)
    //   窗口覆盖度 = 已覆盖秒数 / 20s —— 刚开始听时天然偏低。
    auto computeConfidence = [&]()
    {
        const double dataFactor = std::clamp (covered / 20.0, 0.0, 1.0);
        const double modeFactor = std::clamp (((double) candScore - 0.30) / 0.40, 0.0, 1.0);
        const double marginF    = std::clamp (((double) candScore - (double) secondDistinct) / 0.15,
                                              0.0, 1.0);

        double conf = (0.60 * modeFactor + 0.40 * marginF) * (0.20 + 0.80 * dataFactor);

        if (published && (candRoot != publishedRoot || candMode != publishedMode))
            conf *= 0.5;

        keyConfidence = (float) std::clamp (conf, 0.0, 1.0);
    };

    if (candScore < kMinKeyCorrelation)
    {
        // 当前窗口没有可解释的调性（噪声 / 无调性素材）：不发布新结果。
        confident     = false;
        keyConfidence = 0.0f;
        return;
    }

    if (! published)
    {
        if (candRoot == pendingRoot && candMode == pendingMode)
        {
            if (++pendingVotes >= 2)
            {
                published     = true;
                publishedRoot = candRoot;
                publishedMode = candMode;
                confident     = true;
                changeAtSec   = totalSec;
                pendingVotes  = 0;
                computeConfidence();
            }
        }
        else
        {
            pendingRoot  = candRoot;
            pendingMode  = candMode;
            pendingVotes = 1;
        }

        return;
    }

    const float currentScore = profileScore (chroma, publishedRoot, publishedMode);

    if (candRoot == publishedRoot && candMode == publishedMode)
    {
        confident    = true;
        pendingVotes = 0;
    }
    else
    {
        const float margin = isRelativePair (publishedRoot, publishedMode, candRoot, candMode)
                                 ? kRelativeSwitchMargin
                                 : kOtherSwitchMargin;

        if (candScore <= currentScore + margin)
        {
            // 候选没有明显胜过当前调性（典型情况：关系大小调摇摆）→ 保持当前结果。
            confident    = true;
            pendingVotes = 0;
        }
        else if (candRoot == pendingRoot && candMode == pendingMode)
        {
            if (++pendingVotes >= 2)
            {
                publishedRoot = candRoot;
                publishedMode = candMode;
                confident     = true;
                changeAtSec   = totalSec;
                pendingVotes  = 0;
            }
            else
            {
                confident = false;   // 正在确认新调性：当前显示的是旧结果
            }
        }
        else
        {
            pendingRoot  = candRoot;
            pendingMode  = candMode;
            pendingVotes = 1;
            confident    = false;
        }
    }

    if (confident)
        computeConfidence();
    else
        keyConfidence = 0.0f;
}

float KeyEstimator::profileScore (const float chroma[12], int root, Mode mode)
{
    if (root < 0)
        return -2.0f;

    const float* profile = nullptr;

    switch (mode)
    {
        case Mode::major:         profile = kMajorProfile;         break;
        case Mode::naturalMinor:  profile = kMinorProfile;         break;
        case Mode::harmonicMinor: profile = kHarmonicMinorProfile; break;
        case Mode::melodicMinor:  profile = kMelodicMinorProfile;  break;
    }

    if (profile == nullptr)
        return -2.0f;

    double meanC = 0.0;
    double meanP = 0.0;

    for (int i = 0; i < 12; ++i)
    {
        meanC += (double) chroma[i];
        meanP += (double) profile[i];
    }

    meanC /= 12.0;
    meanP /= 12.0;

    double cov = 0.0;
    double varC = 0.0;
    double varP = 0.0;

    for (int i = 0; i < 12; ++i)
    {
        const double c = (double) chroma[(root + i) % 12] - meanC;
        const double p = (double) profile[i] - meanP;

        cov  += c * p;
        varC += c * c;
        varP += p * p;
    }

    const double denom = std::sqrt (varC * varP);

    if (denom < 1.0e-12)
        return -2.0f;

    return (float) (cov / denom);
}

} // namespace y2k::music
