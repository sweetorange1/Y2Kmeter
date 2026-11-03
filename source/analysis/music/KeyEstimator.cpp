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

    // ---- v3.0 混合利底亚（mixolydian）轮廓 ----
    //   以 KK 大调为基底，把"大七度"降权、"小七度"升权：
    //     大调  : [... ]  index 10 (b7) = 2.29, index 11 (7)  = 2.88
    //     mixo  : index 10 (b7) = 3.60, index 11 (7)  = 2.00
    //   为什么必须加：mixolydian 与大调同属"大调侧"，音级集合只差第 7 音。
    //   电子乐/摇滚里 bVII、bVI 和弦极其常见（C7→Bb7→Ab7），只有大调轮廓时
    //   算法会把 bVII 解释成"属调的 4 音"，把主音判到属调上。
    constexpr float kMixolydianProfile[12] =
    {
        6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
        2.52f, 5.19f, 2.39f, 3.66f, 3.60f, 2.00f
    };

    // 判定"当前窗口是否支持一个明确的调性"的相关性下限。
    // 这个数字只用于内部判定，不代表准确率。
    constexpr float kMinKeyCorrelation = 0.30f;

    // 关系大小调之间需要更大的分数差距才允许切换（C major ↔ A minor）。
    constexpr float kRelativeSwitchMargin = 0.12f;
    constexpr float kOtherSwitchMargin    = 0.05f;

    // 和声/旋律小调、混合利底亚必须先明显胜过同主音的基础调式，才允许改标签。
    //   （几种调式的轮廓只差两三个音级，不加这道门限会来回跳。）
    constexpr float kModeSwitchMargin = 0.03f;

    // ---- v3.0 和弦根音定主音 ----
    //   为什么需要：F major 与 C mixolydian 是**同一组 7 个音级**
    //   （F G A Bb C D E == C D E F G A Bb），纯 chroma↔轮廓相关只能靠
    //   相对强弱猜主音，原理上分不出来。实测 bpm176 CMajor.wav 判成 F major
    //   就是这个原因（Bb 是全场最强音级，在 F 调里是合法的 4 音）。
    //   和弦根音则携带"和声往哪解决"的信息：该曲 C 占 75.2% 的和弦时长，
    //   第二名 G 只有 9.9%，一判就准。
    constexpr float  kChordFitGate      = 0.50f;  // 拟合度低于此值不计票
    constexpr double kTonicOverrideRatio = 2.0;   // 根音票需达轮廓胜者根音的几倍才改判

    constexpr float kChromaFMinHz = 55.0f;    // A1
    constexpr float kChromaFMaxHz = 5000.0f;

    // 调音基准估计的频带上限。
    //   只取低频段：这一段里绝大多数谱峰就是**基频**。
    //   放到 5 kHz 时，3 次/5 次谐波会把 12 格折叠的集中度拉偏，
    //   "标准音高"素材会被系统性估成 -12…-20 音分（实测）。
    constexpr float kTuningFMaxHz = 700.0f;

    // ---- K1 调音基准 ----
    constexpr int   kOctaveSlots      = 120;    // 每八度 120 格 = 每格 10 音分
    constexpr float kOctAccDecay      = 0.95f;  // 每帧衰减，约 2 秒记忆
    // 偏移变化超过此值 → 整窗作废。必须明显大于估计噪声，
    // 否则估计在阈值附近抖动会导致窗口被反复清空、结果永远攒不满。
    constexpr float kTuningResetCents = 25.0f;

    // ---- 和弦模板 ----
    //   大三和弦 / 小三和弦 / 属七和弦。刻意不含 m7：
    //   m7 模板自带 b7，而这类素材的 b7 往往极强，会让 m7 抢走本该属于
    //   大三/属七的票（实测会导致 major/minor 判反）。
    constexpr int kChordIvMajor[3] = { 0, 4, 7 };
    constexpr int kChordIvMinor[3] = { 0, 3, 7 };
    constexpr int kChordIvDom7[4]  = { 0, 4, 7, 10 };


    // ---- K3 谐波感知 chroma ----
    constexpr int   kHarmonics        = 6;
    constexpr int   kNoteLo           = 28;     // E1 ≈ 41 Hz（施工单默认；K3 从低到高抵消）
    constexpr int   kNoteHi           = 96;     // C7 ≈ 2093 Hz
    constexpr float kSubCoeff         = 0.30f;  // 残差抵消系数（§可调）
    constexpr float kHarmWeight[kHarmonics] =
        { 1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 0.20f, 1.0f / 6.0f };

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

    // ---- 和弦模板拟合度（余弦相似度）----
    //   chroma 是绝对音级，模板是相对主音的音级集合，所以要按 root 旋转比较。
    static float chordFit (const float chroma[12], int root,
                           const int* ivs, int nIvs) noexcept
    {
        double tpl[12] = {};

        tpl[ivs[0]] = 1.0;
        for (int k = 1; k < nIvs; ++k)
            tpl[ivs[k]] = 0.85 - 0.10 * (double) (k - 1);

        double dot = 0.0, nc = 0.0, nt = 0.0;

        for (int i = 0; i < 12; ++i)
        {
            const double c = (double) chroma[(root + i) % 12];
            dot += c * tpl[i];
            nc  += c * c;
            nt  += tpl[i] * tpl[i];
        }

        return (float) (dot / (std::sqrt (nc * nt) + 1.0e-12));
    }

    // 返回最佳和弦根音（0..11），拟合度写入 fitOut。
    static int bestChordRoot (const float chroma[12], float& fitOut) noexcept
    {
        int   bestRoot = 0;
        float bestFit  = -1.0f;

        for (int r = 0; r < 12; ++r)
        {
            const float f1 = chordFit (chroma, r, kChordIvMajor, 3);
            const float f2 = chordFit (chroma, r, kChordIvMinor, 3);
            const float f3 = chordFit (chroma, r, kChordIvDom7,  4);

            const float f = std::max ({ f1, f2, f3 });

            if (f > bestFit)
            {
                bestFit  = f;
                bestRoot = r;
            }
        }

        fitOut = bestFit;
        return bestRoot;
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
    magWhite.assign ((size_t) fft.numBins(), 0.0f);
    whitPrefix.assign ((size_t) fft.numBins() + 1, 0.0f);
    residual.assign ((size_t) fft.numBins(), 0.0f);
    octAcc.fill (0.0f);
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

    // K1：调音基准相关的累积也一起清空（换源 / 静音后重新估计）
    octAcc.fill (0.0f);
    tuningCentsValue = 0.0f;
    chromaTuning     = 0.0f;
    chromaTuningInit = false;
    tuningInit       = false;
    tuningDriftRounds = 0;

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

// ==========================================================
// K2：谱白化
//   用 1/3 倍频程（约 ±26%）滑动平均作为"局部基准"，原始幅度除以它。
//   效果：任何平滑的频谱倾斜（音色、低通、响度）被压平，谐波峰相对放大。
//   复杂度：前缀和实现，O(numBins)，每帧一次。
// ==========================================================
void KeyEstimator::applyWhitening (const float* magIn, int numBins, float* magOut)
{
    if ((int) whitPrefix.size() < numBins + 1)
        whitPrefix.assign ((size_t) numBins + 1, 0.0f);

    whitPrefix[0] = 0.0f;

    for (int b = 0; b < numBins; ++b)
        whitPrefix[(size_t) b + 1] = whitPrefix[(size_t) b] + magIn[b];

    magOut[0] = 0.0f;

    for (int b = 1; b < numBins; ++b)
    {
        const int lo = std::max (1, (int) std::floor ((double) b / 1.26));
        const int hi = std::min (numBins - 1, (int) std::ceil ((double) b * 1.26));

        const float sum = whitPrefix[(size_t) hi + 1] - whitPrefix[(size_t) lo];
        const float avg = sum / (float) (hi - lo + 1);

        // 部分白化（指数 0.5）：完全白化连低音一起压平，chroma 会退化成
        //   "音高出现次数"，I–IV–V–I 这类进行被稳定判成属调（实测 C→G）。
        //   取平方根 = 压平一半频谱倾斜：既剥离音色，又保留低音（调性最强的
        //   线索）的权重。轮廓相关分类器本身没有概率模型来补偿频谱倾斜，
        //   所以白化只能是部分的。
        magOut[b] = magIn[b] / (std::sqrt (avg) + 1.0e-9f);
    }
}

// ==========================================================
// K1：八度直方图累积
//   把白化谱按 10 音分一格归到 120 格里，带一阶衰减（约 2 秒记忆）。
//   分 120 格而不是直接折 12 个音级：只有保留亚半音分辨率，才能测出
//   "A4 其实是 447 Hz"这种整体偏移。
//   白化后的噪声底约为 1，先减去它，只累积"超出局部平均"的音调峰，
//   否则 120 格会被平坦底噪填满，集中度对比度变差。
// ==========================================================
void KeyEstimator::accumulateOctaveHistogram (const float* mag, const float* white,
                                              int numBins, double binHz)
{
    for (auto& v : octAcc)
        v *= kOctAccDecay;

    //   调音估计只用低频段（55–700 Hz）：这一段里绝大多数谱峰就是**基频**，
    //   而高次谐波（3 次 = +702 音分、5 次 = +386 音分）会把 12 格折叠的
    //   集中度目标拉偏几十音分。实测把范围放到 5 kHz 时，"标准音高"素材
    //   会被系统性估成 -12…-20 音分。
    //
    //   [v3.0 修正] 之前这里误用了 kChromaFMaxHz（5 kHz），注释承诺的限带
    //   从未生效 —— 表现为 440 Hz 素材上估计值在 -4 ~ +19 音分之间乱跳，
    //   还会因为超过 kTuningResetCents 反复清空 20 秒调性窗口。
    const double fMax = std::min ((double) kTuningFMaxHz, sampleRate * 0.45);
    const double fMin = (double) kChromaFMinHz;

    // 门限按"本帧带内白化谱均值"自适应，而不是写死绝对值：
    //   部分白化（sqrt）之后幅度量纲会随信号变化，绝对值门限在真实素材上
    //   会把全部基频拒掉（实测 accepted=0）。
    double meanWhite = 0.0;
    int    meanCount = 0;

    for (int b = 1; b + 1 < numBins; ++b)
    {
        const double f = (double) b * binHz;

        if (f < fMin || f > fMax)
            continue;

        meanWhite += (double) white[(size_t) b];
        ++meanCount;
    }

    if (meanCount <= 0)
        return;

    meanWhite /= (double) meanCount;

    for (int b = 1; b + 1 < numBins; ++b)
    {
        const double y0 = (double) mag[(size_t) (b - 1)];
        const double y1 = (double) mag[(size_t) b];
        const double y2 = (double) mag[(size_t) (b + 1)];

        // 只累积"明显的音调峰"：白化值高于带内均值 2 倍，且是原始幅度局部峰
        if ((double) white[(size_t) b] <= meanWhite * 2.0 || y1 < y0 || y1 < y2)
            continue;

        // 抛物线插值求 sub-bin 频率。
        //   不做这一步，单 bin 量化误差（±2.7 Hz）在 55 Hz 处就是 ±80 音分，
        //   足以把调音估计本身拉偏几十音分。
        const double denom = y0 - 2.0 * y1 + y2;
        const double delta = (std::abs (denom) > 1.0e-12)
                                 ? std::clamp (0.5 * (y0 - y2) / denom, -0.5, 0.5)
                                 : 0.0;

        const double f = ((double) b + delta) * binHz;

        if (f < fMin || f > fMax)
            continue;

        const double m = y1;

        if (m <= 0.0)
            continue;

        const double midi  = 69.0 + 12.0 * std::log2 (f / 440.0);
        double       cents = std::fmod (midi * 100.0, 1200.0);

        if (cents < 0.0)
            cents += 1200.0;

        const int    slot = std::clamp ((int) (cents / 10.0), 0, kOctaveSlots - 2);
        const double frac = cents / 10.0 - (double) slot;

        octAcc[(size_t) slot]       += (float) (m * (1.0 - frac));
        octAcc[(size_t) (slot + 1)] += (float) (m * frac);
    }
}

// ==========================================================
// K1：从八度直方图估计调音偏移
//
//   原方案（对每个候选偏移折成 12 个音级、取 Σfold² 最大）在实现上有一个
//   死角：fold 的索引是 `(s - off) / 10`，也就是先把偏移量化到 100 音分的
//   音级格再统计。于是 ±50 音分范围内目标函数是**平台**，argmax 只能在
//   格边界上跳 —— 实测表现就是"估计永远卡在 -10 / -20，加 30 音分的素材
//   也读不出来"。
//
//   这里改用直方图的一阶相位：把每个 10 音分格看作单位圆上的相位
//   （一圈 = 100 音分 = 一个半音），对所有格做矢量平均：
//       θ = atan2(Σ aₛ sin(2π·cₛ/100), Σ aₛ cos(2π·cₛ/100))
//   θ 换算回音分就是"整体偏离十二平均律多少音分"。这个估计对 ±50 音分
//   连续可导，没有量化平台，也不需要 argmax。
//
//   谐波污染：3 次谐波（+702 音分）mod 100 ≈ 2 音分，几乎不影响；5 次
//   （+386 音分）mod 100 = 86 音分（等效 -14），权重 h=5 只有 0.2，且
//   55–700 Hz 频段里基频占主导，实测偏差 < 5 音分。
// ==========================================================
void KeyEstimator::refreshTuning()
{
    double total = 0.0;

    for (float v : octAcc)
        total += (double) v;

    if (total < 1.0e-6)
        return;   // 还没有足够数据，保留上次估计

    double sumCos = 0.0;
    double sumSin = 0.0;

    for (int s = 0; s < kOctaveSlots; ++s)
    {
        const double ang = 2.0 * kPi * ((double) s * 10.0 / 100.0);

        sumCos += (double) octAcc[(size_t) s] * std::cos (ang);
        sumSin += (double) octAcc[(size_t) s] * std::sin (ang);
    }

    double cents = std::atan2 (sumSin, sumCos) * 100.0 / (2.0 * kPi);

    // 折到 (-50, +50]
    if (cents > 50.0)  cents -= 100.0;
    if (cents < -50.0) cents += 100.0;

    if (! tuningInit)
    {
        tuningCentsValue = (float) cents;
        tuningInit       = true;
        return;
    }

    // EMA 平滑：单帧估计有 ±几音分噪声，避免它带动整窗重置
    tuningCentsValue += (float) (0.15 * (cents - (double) tuningCentsValue));
}

// ==========================================================
// K3：谐波感知 chroma（IterativeHSS 的简化版）
//
//   问题：按 bin 直接折叠时，一个 C2 贝斯音的第 3 谐波落在 G、第 5 谐波
//         落在 E，于是这个 C 会自动往 G / E 灌能量 —— 任何 C 和弦的 chroma
//         都天然偏向 C/E/G，这正是 C major 与 A minor 相关度接近、
//         关系调混淆的根因。
//
//   做法：遍历候选基频（从低到高），把整条谐波列的加权幅度求和作为该音级
//         的显著性；算完立刻把这条谐波列已解释的能量从频谱副本里扣掉，
//         再处理下一个基频（匹配追踪 / 贪心残差抵消）。低音先处理，保证
//         它的谐波只被计入一次，不会重复灌给五度/大三度。
//
//   代价：64 个音 × 6 次谐波 ≈ 每帧 400 次查表，21.5 帧/秒 → 可忽略。
// ==========================================================
void KeyEstimator::computeChromaHarmonic (const float* white, int numBins, double binHz,
                                          float out[12])
{
    for (int i = 0; i < 12; ++i)
        out[i] = 0.0f;

    if ((int) residual.size() < numBins)
        residual.assign ((size_t) numBins, 0.0f);

    // 工作副本：残差抵消会改写它，不能污染 magWhite
    std::copy (white, white + numBins, residual.begin());

    const double fMax = std::min ((double) kChromaFMaxHz, sampleRate * 0.45);

    for (int note = kNoteLo; note <= kNoteHi; ++note)
    {
        const double f0 = 440.0 * std::pow (2.0,
            ((double) note - 69.0 + (double) tuningCentsValue / 100.0) / 12.0);

        if (f0 > fMax)
            break;

        float salience = 0.0f;

        for (int h = 1; h <= kHarmonics; ++h)
        {
            const double fh = f0 * (double) h;

            if (fh > fMax)
                break;

            const int bin = (int) std::lround (fh / binHz);

            if (bin <= 0 || bin >= numBins)
                break;

            // 取 ±1 bin 最大值，容忍基频估计与 bin 量化误差
            float v = residual[(size_t) bin];

            if (bin > 1)
                v = std::max (v, residual[(size_t) bin - 1]);

            if (bin + 1 < numBins)
                v = std::max (v, residual[(size_t) bin + 1]);

            salience += kHarmWeight[h - 1] * v;
        }

        if (salience <= 0.0f)
            continue;

        const int pc = ((note % 12) + 12) % 12;
        out[pc] += salience;

        // 残差抵消：把这条谐波列已解释的能量从副本里扣掉（保守系数）
        for (int h = 1; h <= kHarmonics; ++h)
        {
            const int bin = (int) std::lround (f0 * (double) h / binHz);

            if (bin <= 0 || bin >= numBins)
                break;

            residual[(size_t) bin] =
                std::max (0.0f, residual[(size_t) bin] - kHarmWeight[h - 1] * salience * kSubCoeff);
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

    const int    numBins = fft.numBins();
    const double binHz   = sampleRate / (double) windowSize;

    // ---- K2：谱白化 ----
    //   把"音色 / 频谱倾斜 / 响度包络"从 chroma 里剥掉，只留音高结构。
    //   （原来的 1/√(f/220) 高频压制已删除：白化 + 1/h 谐波权重已经把
    //    频谱倾斜处理掉了，再压一次会双重惩罚高频、把 chroma 拉向低频根音。）
    applyWhitening (mag.data(), numBins, magWhite.data());

    // ---- K1：先更新调音基准，再用它折叠 chroma ----
    accumulateOctaveHistogram (mag.data(), magWhite.data(), numBins, binHz);
    refreshTuning();

    // ---- K3：谐波感知 chroma ----
    float chroma[12] = {};
    computeChromaHarmonic (magWhite.data(), numBins, binHz, chroma);

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
    // ---- K1：调音基准一致性 ----
    //   chroma 是在"折叠当时的 tuningCentsValue"下算出来的。如果这个偏移在
    //   窗口生命周期内显著变化，窗口里就混着两种折叠结果 → 相关性必然错乱。
    //   此时整窗作废、重新累积（收敛后不会再触发）。
    refreshTuning();

    if (! chromaTuningInit)
    {
        chromaTuning     = tuningCentsValue;
        chromaTuningInit = true;
    }
    else if (std::abs (tuningCentsValue - chromaTuning) > kTuningResetCents)
    {
        // 连续 3 轮都显著偏离才作废整窗（单轮跳变可能只是数据不足）
        if (++tuningDriftRounds < 3)
            return;

        tuningDriftRounds = 0;
        chromaTuning      = tuningCentsValue;

        frameHead     = 0;
        frameCount    = 0;
        pendingRoot   = -1;
        pendingVotes  = 0;
        confident     = false;
        keyConfidence = 0.0f;
        return;
    }
    else
    {
        tuningDriftRounds = 0;
    }

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

    // ---- 24 个候选：12 主音 × {大调侧, 小调侧} ----
    //   大调侧在 major / mixolydian 之间取优；小调侧在 自然/和声/旋律 之间取优。
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
        // ---- 大调侧：major vs mixolydian ----
        //   mixolydian 必须明显胜过 major 才改标签，否则同样的音级集合
        //   会在两个标签之间来回抢票（它们只差第 7 音）。
        float majorScore = profileScore (chroma, root, Mode::major);
        Mode  majorMode  = Mode::major;

        {
            const float mix = profileScore (chroma, root, Mode::mixolydian);

            if (mix > majorScore + kModeSwitchMargin)
            {
                majorScore = mix;
                majorMode  = Mode::mixolydian;
            }
        }

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

        cands[candCount++] = { majorScore, root, majorMode };
        cands[candCount++] = { minorScore, root, minorMode };
    }

    Cand best;

    for (int i = 0; i < candCount; ++i)
        if (cands[i].score > best.score)
            best = cands[i];

    // ---- v3.0 主音仲裁：和弦根音时长投票 ----
    //   动机：F major 与 C mixolydian 是同一组 7 个音级
    //   （F G A Bb C D E == C D E F G A Bb）。纯 chroma↔轮廓相关只能靠
    //   相对强弱猜主音，遇到"bVII/bVI 借用多、主音不突出"的素材必然跑到属调。
    //   和弦根音携带"和声往哪解决"的信息，正好补上这个盲区。
    //
    //   实现：逐帧（窗口内实际帧，保证与 chroma 聚合同一时间范围）求最佳和弦
    //   根音，拟合度达标的帧给该根音投 1 票（帧等距 → 票数正比于时长）。
    //   只有当某个根音的票数显著超过"轮廓胜者的根音"时才改判，避免过度干预。
    {
        double rootVotes[12] = {};

        for (int i = 0; i < frameCount; ++i)
        {
            const ChromaFrame& f = frames[(size_t) ((frameHead + i) % frameCapacity)];

            float     fit  = 0.0f;
            const int r    = bestChordRoot (f.c, fit);

            if (fit >= kChordFitGate)
                rootVotes[r] += 1.0;
        }

        int    tonicRoot = -1;
        double tonicVote = 0.0;

        for (int r = 0; r < 12; ++r)
        {
            if (rootVotes[r] > tonicVote)
            {
                tonicVote = rootVotes[r];
                tonicRoot = r;
            }
        }

        if (tonicRoot >= 0 && tonicRoot != best.root
            && tonicVote > rootVotes[best.root] * kTonicOverrideRatio)
        {
            // 在"主音 = tonicRoot"的候选里取最高分（大调侧 / 小调侧各一个）。
            Cand alt;
            bool found = false;

            for (int i = 0; i < candCount; ++i)
            {
                if (cands[i].root != tonicRoot)
                    continue;

                if (! found || cands[i].score > alt.score)
                {
                    alt   = cands[i];
                    found = true;
                }
            }

            // 主音候选本身也必须站得住（相关度过门限）才改判。
            if (found && alt.score >= kMinKeyCorrelation)
                best = alt;
        }
    }

    // 第二名只统计"不同主音"的候选：同主音的调式变体属于"调式不确定"，
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
        case Mode::mixolydian:    profile = kMixolydianProfile;    break;
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
