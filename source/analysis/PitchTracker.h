#pragma once

#include <array>
#include <cstdint>

// ==========================================================
// PitchTracker —— 音级（chroma）跟踪，供 Waveform 的 PITCH 染色使用
//
// 输入：AnalyserHub 主路 FFT 的线性幅度谱（magData，满幅正弦 ≈ 1.0）
// 输出：Info { hue01, noteNumber, hz, confidence, valid }
//
// 处理链：
//   1) 频带裁剪 55 Hz – 5 kHz；幅度平方 + 轻微高频倾斜（指数 0.35）作权重
//      —— 倾斜保证中高频的旋律不被低频贝斯完全压住；
//   2) 按 MIDI 音高折叠到 12 个音级桶（**线性插值**跨桶，半音边界不跳变）；
//   3) 12 桶归一化后一阶低通平滑（τ ≈ 100 ms）—— 换音时色相滑过去而不是硬切；
//   4) 取最强音级，在其 ±1 桶内做圆周加权重心 → 连续色相（0 = C）；
//   5) confidence = 最强桶占比 × 电平门（静音 → 0）
//
// 为什么不用过零检测：
//   过零只能在单音上稳定，鼓组 / 和弦 / 混音会乱跳；chroma 折叠对和声天然
//   鲁棒，且完全复用已有的 FFT，不新增任何音频线程开销。
//
// 性能：每个 bin 的频率 / 音级 / 倾斜只与 (binHz, numBins) 有关，
//   首次调用时预计算成 LUT，之后每帧只做乘加（1024 bin ≈ 数微秒）。
//
// 线程模型：只在 UI 线程（FrameDispatcher）调用，内部无锁、无分配。
// 纯 C++（不依赖 JUCE），可被 tests/ 直接单测。
// ==========================================================

namespace y2k::pitch
{

struct Info
{
    bool  valid      = false;   // 是否有可信音高（false 时 UI 应退回中性色）
    float hue01      = 0.0f;    // 音级色相 [0,1)：0 = C，1/12 = C#，…
    float noteNumber = 0.0f;    // 最强者谱峰的连续 MIDI 音高（60 = C4），仅作读数
    float hz         = 0.0f;    // 上述谱峰的频率（Hz）
    float confidence = 0.0f;    // 0..1
};

class Tracker
{
public:
    void reset() noexcept;

    // mag     : 线性幅度谱（满幅正弦 ≈ 1.0）
    // numBins : bin 个数（= fftSize / 2，上限 kMaxBins）
    // binHz   : 每个 bin 的频率间隔（= sampleRate / fftSize）
    // dtSec   : 距上次调用的时间（平滑用；<=0 或过大时按 1/60 处理）
    Info pushSpectrum (const float* mag, int numBins, double binHz, float dtSec) noexcept;

    Info get() const noexcept { return published; }

private:
    static constexpr int kPitchClasses = 12;
    static constexpr int kMaxBins      = 4096;

    // 每 bin 的预计算量
    struct BinInfo
    {
        float tilt = 0.0f;   // 高频倾斜权重
        float frac = 0.0f;   // 音级小数部分（跨桶线性插值用）
        int   pc   = 0;      // 音级 0..11
    };

    void rebuildBinLut (int numBins, double binHz) noexcept;

    std::array<float, (size_t) kPitchClasses> chroma {};   // 平滑后的音级分布
    Info  published {};
    float smoothedNote = 60.0f;   // 平滑后的 MIDI 音高（音高染色按音域取色，抖动会闪）
    bool  noteInit     = false;

    std::array<BinInfo, (size_t) kMaxBins> binLut {};
    int    lutBins      = -1;
    double lutBinHz     = -1.0;
    int    firstBin     = 0;      // 频带下界对应的 bin
    int    lastBin      = 0;      // 频带上界对应的 bin（不含）
};

} // namespace y2k::pitch
