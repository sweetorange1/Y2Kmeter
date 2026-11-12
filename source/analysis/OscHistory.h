#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// OscHistory —— 示波器样本的长环形历史（v3.1，按住慢放用）
//
//   Hub 每帧只给最近 2048 个样本的窗口（AnalyserHub::FrameSnapshot::oscL/oscR），
//   并带一个单调递增的 oscTotalSamples。慢放需要"比窗口更长的过去"，这里在 UI 线程
//   用 oscTotalSamples 做差，把每帧新增的样本接到一个长环里。
//
//   只在需要时录制：begin() 之后才分配并开始接收；end() 之后停止（内存保留复用）。
//   慢放读指针只会落后实时、永远不会领先，所以只需要"按下之后"的样本——
//   不按住的时候零成本（不分配、不拷贝）。
//
//   容量 2^18 = 262144 样本：48 kHz ≈ 5.5 s，96 kHz ≈ 2.7 s；
//   在 1/50 慢放下，读指针落后速度为 49/50 倍实时，超容量时读指针被夹回最老可读位置。
// ==========================================================
class OscHistory
{
public:
    static constexpr int kCapacity = 1 << 18;

    bool isRecording() const noexcept { return recording; }

    // 开始录制：用当前帧的 2048 窗口作为起点（窗口末尾 = 当前实时位置）
    void begin (const AnalyserHub::FrameSnapshot& f)
    {
        if (L.empty()) { L.assign ((size_t) kCapacity, 0.0f); R.assign ((size_t) kCapacity, 0.0f); }
        written  = 0;
        recording = true;
        appendWindow (f, (int) f.oscL.size());
        lastTotal = f.oscTotalSamples;
    }

    void end() noexcept { recording = false; }

    // 录制中每帧调用：把 Hub 在两帧之间新增的样本接上来
    void feed (const AnalyserHub::FrameSnapshot& f)
    {
        if (! recording) return;

        const std::uint64_t total = f.oscTotalSamples;
        if (total <= lastTotal) return;

        const std::uint64_t fresh = total - lastTotal;
        lastTotal = total;
        const int win = (int) f.oscL.size();

        // 新增样本多于窗口 = 丢帧：只能接上整个窗口（中间缺的部分不可恢复）
        appendWindow (f, (int) std::min<std::uint64_t> (fresh, (std::uint64_t) win));
    }

    // 已写入的绝对样本数（读指针的上界）
    std::uint64_t totalWritten() const noexcept { return written; }

    // 最老仍可读的绝对位置
    std::uint64_t oldestReadable() const noexcept
    {
        return written > (std::uint64_t) kCapacity ? written - (std::uint64_t) kCapacity : 0;
    }

    // 读取以 endExclusive 结尾（不含）的 n 个样本，时间从旧到新。读不到的部分补 0。
    void read (std::uint64_t endExclusive, int n, float* outL, float* outR) const
    {
        const std::uint64_t oldest = oldestReadable();
        for (int i = 0; i < n; ++i)
        {
            const std::int64_t idx = (std::int64_t) endExclusive - (std::int64_t) n + i;
            if (idx < (std::int64_t) oldest || idx >= (std::int64_t) written)
            {
                outL[i] = 0.0f; outR[i] = 0.0f;
                continue;
            }
            const size_t pos = (size_t) ((std::uint64_t) idx & (std::uint64_t) (kCapacity - 1));
            outL[i] = L[pos];
            outR[i] = R[pos];
        }
    }

private:
    // 追加窗口末尾的 count 个样本
    void appendWindow (const AnalyserHub::FrameSnapshot& f, int count)
    {
        const int win = (int) f.oscL.size();
        count = std::min (count, win);
        for (int i = win - count; i < win; ++i)
        {
            const size_t pos = (size_t) (written & (std::uint64_t) (kCapacity - 1));
            L[pos] = f.oscL[(size_t) i];
            R[pos] = f.oscR[(size_t) i];
            ++written;
        }
    }

    std::vector<float> L, R;
    std::uint64_t written   = 0;
    std::uint64_t lastTotal = 0;
    bool recording = false;
};
