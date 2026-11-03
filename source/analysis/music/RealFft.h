#pragma once

#include <vector>

// ==========================================================
// RealFft —— 极简 radix-2 就地复 FFT（实数输入 → 幅度谱）
//
// 为什么不用 juce::dsp::FFT：
//   · 本模块的 DSP 核心要能被 tests/music_analysis_test.cpp 直接链接做
//     单元测试，测试目标不希望引入 JUCE 的全部依赖；
//   · 这里只做定长、单线程、后台线程内的 FFT，不需要 JUCE 的 SIMD 优化。
//
// 线程模型：
//   仅在分析工作线程中使用（prepare() 由工作线程在采样率变化时调用）。
// ==========================================================

namespace y2k::music
{

inline int nextPowerOfTwo (int v) noexcept
{
    int s = 1;
    while (s < v) s <<= 1;
    return s;
}

// 取最接近的 2 的幂（而不是一向上取整）—— 采样率较高时避免把
// "46ms 分析窗" 变成 85ms，导致 onset 时间分辨率被无谓地拉低。
inline int nearestPowerOfTwo (int v) noexcept
{
    if (v <= 1)
        return 1;

    const int lo = nextPowerOfTwo (v) >> 1;
    const int hi = nextPowerOfTwo (v);

    return (v - lo <= hi - v) ? lo : hi;
}

class RealFft
{
public:
    // fftSize 会被向上取整到 2 的幂；多次调用会复用已分配的缓冲。
    void prepare (int fftSize);

    int  size() const noexcept       { return n; }
    int  numBins() const noexcept    { return n / 2 + 1; }
    bool isPrepared() const noexcept { return n >= 8; }

    // input 长度必须是 size()，outMagnitude 长度必须是 numBins()。
    // 幅度按 2/N 归一化：满幅正弦在其所属 bin 上读数为 ≈ 1.0。
    void magnitude (const float* input, float* outMagnitude);

private:
    void transform();

    int n     = 0;
    int order = 0;

    std::vector<int>   bitReverse;
    std::vector<float> cosTable;
    std::vector<float> sinTable;
    std::vector<float> re;
    std::vector<float> im;
};

} // namespace y2k::music
