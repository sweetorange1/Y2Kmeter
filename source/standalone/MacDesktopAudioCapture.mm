#include "MacDesktopAudioCapture.h"

#if JUCE_MAC

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>                       // NSWorkspace 休眠/唤醒通知
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <AVFoundation/AVFoundation.h>

#include <CoreMedia/CoreMedia.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>                     // 单调时钟（无数据兜底自愈）
#include <vector>
#include <cstring>
#include <cmath>
#include <cctype>
#include <atomic>

@interface Y2KSCStreamAudioOutput : NSObject <SCStreamOutput>
@property (nonatomic, assign) void* impl;
@end

// NSWorkspace 休眠/唤醒通知桥接（外网 #1）：
//   以 NSObject 作为 selector 观察者（dealloc 时通知中心自动反注册），
//   回调只做原子标志置位，真正的重建由 watchdog 在后台串行队列执行。
@interface Y2KDesktopCaptureLifeProxy : NSObject
@property (nonatomic, assign) void* impl;
@end

namespace y2k
{

struct MacDesktopAudioCapture::Impl
{
    MacDesktopAudioCapture* owner = nullptr;

    SCStream* stream = nil;
    Y2KSCStreamAudioOutput* output = nil;
    dispatch_queue_t queue = nullptr;

    // ======================================================
    // 休眠/唤醒自愈（外网 #1）
    //   · NSWorkspace 通知 → 主线程回调设置原子标志
    //   · watchdog dispatch timer（1s 间隔）检查「唤醒标志」与
    //     「持续无数据 > 5s」，触发 stopCapture()+startCapture() 重建 SCStream
    //   · 所有 stop/start 操作串行化到 lifecycleQueue，避免与 UI 线程竞争
    // ======================================================
    std::atomic<bool>     streamWanted { false };      // 是否希望采集（串行队列内维护）
    std::atomic<bool>     wakeRequested { false };     // DidWake 置位，watchdog 消费
    std::atomic<uint64_t> lastSampleMachTicks { 0 };   // 最近一次音频缓冲到达时间
    dispatch_queue_t      lifecycleQueue = nullptr;    // stop/start 串行队列
    dispatch_queue_t      watchdogQueue  = nullptr;    // watchdog 定时器队列
    dispatch_source_t     watchdogTimer  = nullptr;
    double                machTimebaseNsPerTick = 1.0;
    id                    lifeProxy = nil;             // NSWorkspace 通知桥接对象（ARC）
    bool                  observersRegistered = false;

    static constexpr double kNoDataTimeoutSec = 5.0;   // 无任何 buffer 超时阈值

    Impl()
    {
        lifecycleQueue = dispatch_queue_create ("cn.iisaacbeats.y2kmeter.desktopaudio.lifecycle",
                                                DISPATCH_QUEUE_SERIAL);
        watchdogQueue  = dispatch_queue_create ("cn.iisaacbeats.y2kmeter.desktopaudio.watchdog",
                                                DISPATCH_QUEUE_SERIAL);
        mach_timebase_info_data_t tb {};
        mach_timebase_info (&tb);
        machTimebaseNsPerTick = static_cast<double> (tb.numer) / static_cast<double> (tb.denom);
    }

    ~Impl()
    {
        // 正常流程 stop() 已取消 watchdog；此处兜底防泄漏
        if (watchdogTimer != nullptr)
        {
            dispatch_source_cancel (watchdogTimer);
            dispatch_release (watchdogTimer);
            watchdogTimer = nullptr;
        }
        if (watchdogQueue != nullptr)
        {
            dispatch_release (watchdogQueue);
            watchdogQueue = nullptr;
        }
        if (lifecycleQueue != nullptr)
        {
            dispatch_release (lifecycleQueue);
            lifecycleQueue = nullptr;
        }
        lifeProxy = nil;
    }

    void markSleeping() noexcept
    {
        // 进入休眠：冻结"无数据超时"计时（避免休眠期间误判），唤醒后由 DidWake 主动重建
        lastSampleMachTicks.store (0, std::memory_order_relaxed);
    }

    void markWoken() noexcept
    {
        wakeRequested.store (true, std::memory_order_relaxed);
    }

    void registerSleepWakeObservers()
    {
        if (observersRegistered)
            return;

        lifeProxy = [Y2KDesktopCaptureLifeProxy new];
        [lifeProxy setImpl:this];

        NSNotificationCenter* nc = [[NSWorkspace sharedWorkspace] notificationCenter];
        [nc addObserver:lifeProxy
               selector:@selector (onWorkspaceWillSleep:)
                   name:NSWorkspaceWillSleepNotification
                 object:nil];
        [nc addObserver:lifeProxy
               selector:@selector (onWorkspaceDidWake:)
                   name:NSWorkspaceDidWakeNotification
                 object:nil];
        observersRegistered = true;
    }

    void unregisterSleepWakeObservers()
    {
        if (! observersRegistered)
            return;

        NSNotificationCenter* nc = [[NSWorkspace sharedWorkspace] notificationCenter];
        if (lifeProxy != nil)
        {
            [nc removeObserver:lifeProxy];
            [lifeProxy setImpl:nullptr];
            lifeProxy = nil;
        }
        observersRegistered = false;
    }

    void armWatchdog()
    {
        wakeRequested.store (false, std::memory_order_relaxed);
        lastSampleMachTicks.store (0, std::memory_order_relaxed);

        if (watchdogTimer != nullptr)
            return;

        watchdogTimer = dispatch_source_create (DISPATCH_SOURCE_TYPE_TIMER, 0, 0, watchdogQueue);
        dispatch_source_set_timer (watchdogTimer,
                                   dispatch_time (DISPATCH_TIME_NOW, (int64_t) NSEC_PER_SEC),
                                   (uint64_t) NSEC_PER_SEC,
                                   (uint64_t) (200 * NSEC_PER_MSEC));
        Impl* self = this;
        dispatch_source_set_event_handler (watchdogTimer, ^{
            self->watchdogTick();
        });
        dispatch_resume (watchdogTimer);
    }

    void disarmWatchdog()
    {
        if (watchdogTimer != nullptr)
        {
            dispatch_source_cancel (watchdogTimer);
            dispatch_release (watchdogTimer);
            watchdogTimer = nullptr;
        }
    }

    // 运行在 watchdogQueue：检测唤醒标志 / 无数据超时，派发重建到生命周期队列
    void watchdogTick()
    {
        if (owner == nullptr || ! streamWanted.load (std::memory_order_relaxed))
            return;

        const bool wake = wakeRequested.exchange (false, std::memory_order_relaxed);

        bool timedOut = false;
        const uint64_t last = lastSampleMachTicks.load (std::memory_order_relaxed);
        if (last != 0)
        {
            const uint64_t now = mach_absolute_time();
            const double elapsed = static_cast<double> (now - last) * machTimebaseNsPerTick * 1.0e-9;
            timedOut = elapsed > kNoDataTimeoutSec;
        }

        if (! wake && ! timedOut)
            return;

        // 串行化重建：与 UI 线程的 start/stop 互斥，且不会嵌套重入
        dispatch_async (lifecycleQueue, ^{
            if (owner == nullptr || ! streamWanted.load (std::memory_order_relaxed))
                return;

            juce::String err;
            stopCapture();               // 释放睡眠中失效的旧 SCStream
            if (startCapture (err))      // 重建新流（权限仍有效时静默成功）
            {
                // 新流尚未产出数据：清空计时，等首包到达后再开始超时监控
                lastSampleMachTicks.store (0, std::memory_order_relaxed);
            }
            else
            {
                // 重建失败（如权限被撤销）：把错误留给 UI 查询
                owner->lastError = err;
            }
        });
    }

    enum class DecodePath
    {
        planarFloat32,
        planarInt16,
        interleavedFloat32,
        interleavedInt16
    };

    enum class DecodeMode
    {
        autoSelect,
        forceFloat,
        forceInteger
    };

    struct SignalStats
    {
        double mean = 0.0;
        double variance = 0.0;
        double rms = 0.0;
        double rmsDb = -160.0;
        double peakAbs = 0.0;
        double preClampPeakAbs = 0.0;
        double clipRatio = 0.0;
        double zeroCrossingRate = 0.0;
        bool lowLevel = false;
        bool pathologicalDc = false;
    };

    struct DecodeAttemptInfo
    {
        uint32_t numBuffers = 0;
        uint32_t buffer0Bytes = 0;
        uint32_t buffer1Bytes = 0;
        uint32_t bytesPerSampleUsed = 0;
        uint32_t bytesPerFrameUsed = 0;
        int effectiveFrames = 0;
    };

    struct CandidateResult
    {
        DecodePath path = DecodePath::interleavedFloat32;
        bool ok = false;
        std::vector<float> L;
        std::vector<float> R;
        SignalStats stats;
        DecodeAttemptInfo info;
    };

    double asbdLogElapsedSec = 0.0;
    double monitorLogElapsedSec = 0.0;
    double unhealthyDurationSec = 0.0;
    double forcedModeRemainSec = 0.0;
    double fullScanRemainSec = 0.8;
    double fullScanCooldownSec = 0.0;
    double fullScanProbeElapsedSec = 0.0;
    double coldStartRemainSec = 2.0;
    double dispatchElapsedSec = 0.0;
    double fallbackRetryCooldownSec = 0.0;
    double noPayloadElapsedSec = 0.0;
    std::vector<float> pendingDispatchL;
    std::vector<float> pendingDispatchR;
    double pendingDispatchSampleRate = 48000.0;

    DecodeMode decodeMode = DecodeMode::autoSelect;
    DecodePath stableSelectedPath = DecodePath::interleavedFloat32;
    DecodePath pendingSelectedPath = DecodePath::interleavedFloat32;
    int pendingSelectedWindows = 0;
    DecodePath lastSelectedPath = DecodePath::interleavedFloat32;
    bool stablePathReady = false;

    static bool isFloatFormat (const AudioStreamBasicDescription& asbd)
    {
        return (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    }

    static bool isNonInterleavedFormat (const AudioStreamBasicDescription& asbd)
    {
        return (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    }

    static juce::String fourCCToString (uint32_t v)
    {
        juce::String out;
        for (int i = 3; i >= 0; --i)
        {
            const char c = (char) ((v >> (uint32_t) (i * 8)) & 0xFFu);
            if (std::isprint ((unsigned char) c)) out << juce::String::charToString (juce::juce_wchar (c));
            else out << "?";
        }
        return out;
    }

    static bool isSuspiciousFormat (const AudioStreamBasicDescription& asbd)
    {
        const bool flagsMissing = asbd.mFormatFlags == 0;

        bool bitsInconsistent = false;
        if (asbd.mChannelsPerFrame > 0 && asbd.mBytesPerFrame > 0 && asbd.mBitsPerChannel > 0)
        {
            const bool nonInterleaved = isNonInterleavedFormat (asbd);
            const uint32_t expectedBytesPerFrame = nonInterleaved
                ? ((asbd.mBitsPerChannel + 7u) / 8u)
                : (((asbd.mBitsPerChannel + 7u) / 8u) * asbd.mChannelsPerFrame);

            if (expectedBytesPerFrame > 0)
                bitsInconsistent = asbd.mBytesPerFrame != expectedBytesPerFrame;
        }

        const bool floatBitsOdd = isFloatFormat (asbd)
                               && (asbd.mBitsPerChannel != 0)
                               && (asbd.mBitsPerChannel != 32)
                               && (asbd.mBitsPerChannel != 64);

        const bool integerSignedFlagMissing = (! isFloatFormat (asbd))
                                           && asbd.mBitsPerChannel > 8
                                           && (asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) == 0;

        return flagsMissing || bitsInconsistent || floatBitsOdd || integerSignedFlagMissing;
    }

    void logAsbdIfNeeded (const AudioStreamBasicDescription& asbd, double durationSec)
    {
        const bool suspicious = isSuspiciousFormat (asbd);
        const double periodSec = coldStartRemainSec > 0.0 ? 1.0 : 5.0;

        asbdLogElapsedSec += durationSec;
        if (! suspicious && asbdLogElapsedSec < periodSec)
            return;

        asbdLogElapsedSec = 0.0f; // reset only, no logging
    }

    static float sanitizeDecodedSample (float v)
    {
        if (! std::isfinite (v))
            return 0.0f;

        return v;
    }

    static float protectOutputSample (float v)
    {
        if (! std::isfinite (v))
            return 0.0f;

        // 轻微软限幅，避免错误解码导致的硬削顶方波
        return std::tanh (v);
    }

    static uint32_t byteSwap32 (uint32_t v)
    {
        return ((v & 0x000000FFu) << 24)
             | ((v & 0x0000FF00u) << 8)
             | ((v & 0x00FF0000u) >> 8)
             | ((v & 0xFF000000u) >> 24);
    }

    static uint64_t byteSwap64 (uint64_t v)
    {
        return ((v & 0x00000000000000FFull) << 56)
             | ((v & 0x000000000000FF00ull) << 40)
             | ((v & 0x0000000000FF0000ull) << 24)
             | ((v & 0x00000000FF000000ull) << 8)
             | ((v & 0x000000FF00000000ull) >> 8)
             | ((v & 0x0000FF0000000000ull) >> 24)
             | ((v & 0x00FF000000000000ull) >> 40)
             | ((v & 0xFF00000000000000ull) >> 56);
    }

    static int32_t signExtendToI32 (uint32_t value, uint32_t bits)
    {
        bits = juce::jlimit ((uint32_t) 1u, (uint32_t) 32u, bits);
        if (bits == 32)
            return (int32_t) value;

        const uint32_t signBit = 1u << (bits - 1u);
        const uint32_t mask = (1u << bits) - 1u;
        value &= mask;

        if ((value & signBit) != 0u)
            value |= ~mask;

        return (int32_t) value;
    }

    static float decodeIntegerSample (const uint8_t* p, uint32_t bitsPerChannel, uint32_t bytesPerSample, AudioFormatFlags flags)
    {
        if (p == nullptr)
            return 0.0f;

        bytesPerSample = juce::jlimit ((uint32_t) 1u, (uint32_t) 4u, bytesPerSample);

        uint32_t bits = bitsPerChannel;
        if (bits == 0)
            bits = bytesPerSample * 8u;
        bits = juce::jlimit ((uint32_t) 1u, (uint32_t) 32u, bits);

        const bool isBigEndian = (flags & kAudioFormatFlagIsBigEndian) != 0;
        const bool isSignedFlag = (flags & kAudioFormatFlagIsSignedInteger) != 0;
        const bool isAlignedHigh = (flags & kAudioFormatFlagIsAlignedHigh) != 0;

        // 某些系统输出流会给出整数PCM，但 format flags 没有正确携带 signed 标记。
        // 对于 >8bit PCM，按音频行业约定优先当作 signed，避免被误解码成接近 -1 的直流。
        const bool isSigned = isSignedFlag || bits > 8u;

        uint32_t raw = 0;
        if (isBigEndian)
        {
            for (uint32_t i = 0; i < bytesPerSample; ++i)
                raw = (raw << 8u) | (uint32_t) p[i];
        }
        else
        {
            for (uint32_t i = 0; i < bytesPerSample; ++i)
                raw |= ((uint32_t) p[i]) << (8u * i);
        }

        const uint32_t totalBits = bytesPerSample * 8u;
        if (bits < totalBits)
        {
            if (isAlignedHigh)
                raw >>= (totalBits - bits);

            if (bits < 32)
                raw &= (1u << bits) - 1u;
        }

        int32_t signedValue = 0;
        if (isSigned)
        {
            signedValue = signExtendToI32 (raw, bits);
        }
        else
        {
            const int32_t mid = (bits == 32) ? (int32_t) 0x80000000u : (int32_t) (1u << (bits - 1u));
            signedValue = (int32_t) raw - mid;
        }

        const double denom = (bits == 32) ? 2147483648.0 : (double) (1u << (bits - 1u));
        return sanitizeDecodedSample ((float) ((double) signedValue / denom));
    }

    static float decodeSample (const uint8_t* p, bool isFloat, uint32_t bitsPerChannel, uint32_t bytesPerSample, AudioFormatFlags flags)
    {
        if (p == nullptr)
            return 0.0f;

        uint32_t bits = bitsPerChannel;
        if (bits == 0)
            bits = bytesPerSample * 8u;

        if (isFloat)
        {
            const bool isBigEndian = (flags & kAudioFormatFlagIsBigEndian) != 0;

            const bool preferF64 = (bits >= 64) || (bits == 0 && bytesPerSample == 8);
            if (preferF64 && bytesPerSample >= 8)
            {
                uint64_t raw = 0;
                std::memcpy (&raw, p, sizeof (uint64_t));
                if (isBigEndian)
                    raw = byteSwap64 (raw);

                double v = 0.0;
                std::memcpy (&v, &raw, sizeof (double));
                return sanitizeDecodedSample ((float) v);
            }

            const bool preferF32 = (bits >= 32) || (bits == 0);
            if (preferF32 && bytesPerSample >= 4)
            {
                uint32_t raw = 0;
                std::memcpy (&raw, p, sizeof (uint32_t));
                if (isBigEndian)
                    raw = byteSwap32 (raw);

                float v = 0.0f;
                std::memcpy (&v, &raw, sizeof (float));
                return sanitizeDecodedSample (v);
            }
        }

        return decodeIntegerSample (p, bits, bytesPerSample, flags);
    }

    static SignalStats evaluateSignal (const std::vector<float>& L, const std::vector<float>& R)
    {
        SignalStats st;

        const int n = juce::jmin ((int) L.size(), (int) R.size());
        if (n <= 0)
            return st;

        double sum = 0.0;
        double sumSq = 0.0;
        double peak = 0.0;
        double preClampPeak = 0.0;
        int clipped = 0;
        int zeroCross = 0;
        float prev = 0.0f;
        bool hasPrev = false;

        for (int i = 0; i < n; ++i)
        {
            const float mRaw = 0.5f * (L[(size_t) i] + R[(size_t) i]);
            const float m = protectOutputSample (mRaw);
            const double md = (double) m;
            const double absRaw = std::abs ((double) mRaw);

            sum += md;
            sumSq += md * md;
            peak = juce::jmax (peak, std::abs (md));
            preClampPeak = juce::jmax (preClampPeak, absRaw);
            if (absRaw > 1.0)
                ++clipped;

            if (hasPrev && ((prev < 0.0f && m > 0.0f) || (prev > 0.0f && m < 0.0f)))
                ++zeroCross;
            prev = m;
            hasPrev = true;
        }

        const double invN = 1.0 / (double) n;
        st.mean = sum * invN;
        st.rms = std::sqrt (sumSq * invN);
        st.rmsDb = st.rms <= 1.0e-12 ? -160.0 : 20.0 * std::log10 (st.rms);
        st.variance = juce::jmax (0.0, sumSq * invN - st.mean * st.mean);
        st.peakAbs = peak;
        st.preClampPeakAbs = preClampPeak;
        st.clipRatio = (double) clipped * invN;
        st.zeroCrossingRate = (n > 1) ? ((double) zeroCross / (double) (n - 1)) : 0.0;

        st.lowLevel = st.peakAbs <= (2.0 / 32768.0) && st.rmsDb < -85.0;
        st.pathologicalDc = st.peakAbs >= 0.98 && st.variance < 1.0e-8 && st.zeroCrossingRate < 0.001;

        return st;
    }

    static double scoreSignal (const SignalStats& st)
    {
        double score = st.rmsDb;

        if (st.pathologicalDc) score -= 120.0;
        if (st.lowLevel) score -= 25.0;
        if (st.clipRatio > 0.01) score -= 40.0;
        if (st.variance < 1.0e-8 && st.rmsDb > -20.0) score -= 30.0;
        if (st.zeroCrossingRate > 0.48 && st.rmsDb > -30.0) score -= 20.0;

        if (st.variance > 1.0e-7) score += 4.0;
        if (st.zeroCrossingRate > 0.001 && st.zeroCrossingRate < 0.45) score += 2.0;
        if (st.peakAbs > 0.0005 && st.peakAbs < 0.999) score += 3.0;
        if (st.clipRatio < 0.001) score += 5.0;

        return score;
    }

    static bool isFloatPath (DecodePath p)
    {
        return p == DecodePath::planarFloat32 || p == DecodePath::interleavedFloat32;
    }

    static bool isPlanarPath (DecodePath p)
    {
        return p == DecodePath::planarFloat32 || p == DecodePath::planarInt16;
    }

    static bool isPathCompatibleWithAsbd (DecodePath p, const AudioStreamBasicDescription& asbd)
    {
        const bool asbdFloat = isFloatFormat (asbd);
        const bool asbdPlanar = isNonInterleavedFormat (asbd);
        return (isFloatPath (p) == asbdFloat) && (isPlanarPath (p) == asbdPlanar);
    }

    static juce::String pathToString (DecodePath p)
    {
        switch (p)
        {
            case DecodePath::planarFloat32:      return "planar-f32";
            case DecodePath::planarInt16:        return "planar-s16";
            case DecodePath::interleavedFloat32: return "interleaved-f32";
            case DecodePath::interleavedInt16:   return "interleaved-s16";
        }

        return "unknown";
    }

    static DecodePath fallbackBackupPath (DecodePath p)
    {
        switch (p)
        {
            case DecodePath::planarFloat32:      return DecodePath::planarInt16;
            case DecodePath::planarInt16:        return DecodePath::planarFloat32;
            case DecodePath::interleavedFloat32: return DecodePath::interleavedInt16;
            case DecodePath::interleavedInt16:   return DecodePath::interleavedFloat32;
        }

        return DecodePath::interleavedInt16;
    }

    bool isPathAllowedByMode (DecodePath p) const
    {
        if (decodeMode == DecodeMode::autoSelect)
            return true;

        if (decodeMode == DecodeMode::forceFloat)
            return isFloatPath (p);

        return ! isFloatPath (p);
    }

    DecodePath decodePathFromMode (const AudioStreamBasicDescription& asbd) const
    {
        const bool nonInterleaved = isNonInterleavedFormat (asbd);

        if (decodeMode == DecodeMode::forceFloat)
            return nonInterleaved ? DecodePath::planarFloat32 : DecodePath::interleavedFloat32;

        if (decodeMode == DecodeMode::forceInteger)
            return nonInterleaved ? DecodePath::planarInt16 : DecodePath::interleavedInt16;

        if (isFloatFormat (asbd))
            return nonInterleaved ? DecodePath::planarFloat32 : DecodePath::interleavedFloat32;

        return nonInterleaved ? DecodePath::planarInt16 : DecodePath::interleavedInt16;
    }

    bool decodeSampleBufferWithPath (CMSampleBufferRef sampleBuffer,
                                     const AudioStreamBasicDescription& asbd,
                                     int numSamples,
                                     DecodePath path,
                                     std::vector<float>& L,
                                     std::vector<float>& R,
                                     DecodeAttemptInfo* outInfo)
    {
        const bool decodeAsFloat = isFloatPath (path);
        const bool forcePlanar = isPlanarPath (path);
        const uint32_t forcedBytesPerSample = decodeAsFloat ? 4u : 2u;
        const uint32_t forcedBitsPerSample = decodeAsFloat ? 32u : 16u;

        if (outInfo != nullptr)
            *outInfo = {};

        bool decoded = false;

        struct
        {
            AudioBufferList list;
            AudioBuffer buffers[7];
        } audioBufferList;

        size_t audioBufferListSizeNeeded = 0;
        CMBlockBufferRef retainedBlockBuffer = nullptr;

        const auto status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
            sampleBuffer,
            &audioBufferListSizeNeeded,
            &audioBufferList.list,
            sizeof (audioBufferList),
            kCFAllocatorSystemDefault,
            kCFAllocatorSystemDefault,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &retainedBlockBuffer);

        if (status == noErr)
        {
            const auto* abl = &audioBufferList.list;
            const bool nonInterleaved = isNonInterleavedFormat (asbd);
            const uint32_t channels = juce::jmax ((uint32_t) 1u, asbd.mChannelsPerFrame);

            if (outInfo != nullptr)
            {
                outInfo->numBuffers = abl->mNumberBuffers;
                outInfo->buffer0Bytes = (abl->mNumberBuffers > 0) ? abl->mBuffers[0].mDataByteSize : 0;
                outInfo->buffer1Bytes = (abl->mNumberBuffers > 1) ? abl->mBuffers[1].mDataByteSize : 0;
            }

            if (forcePlanar)
            {
                if (abl->mNumberBuffers >= 2)
                {
                    const auto& b0 = abl->mBuffers[0];
                    const auto& b1 = abl->mBuffers[1];
                    const auto* pL = static_cast<const uint8_t*> (b0.mData);
                    const auto* pR = static_cast<const uint8_t*> (b1.mData);

                    uint32_t bytesPerSample = (nonInterleaved && asbd.mBytesPerFrame > 0)
                        ? asbd.mBytesPerFrame
                        : forcedBytesPerSample;
                    bytesPerSample = juce::jmax ((uint32_t) 1u, bytesPerSample);

                    const int framesL = (pL != nullptr) ? (int) (b0.mDataByteSize / bytesPerSample) : 0;
                    const int framesR = (pR != nullptr) ? (int) (b1.mDataByteSize / bytesPerSample) : 0;
                    const int effectiveFrames = juce::jmin (numSamples, juce::jmin (framesL, framesR));

                    if (effectiveFrames > 0)
                    {
                        L.assign ((size_t) effectiveFrames, 0.0f);
                        R.assign ((size_t) effectiveFrames, 0.0f);

                        const uint32_t bitsPerSample = juce::jmax (forcedBitsPerSample,
                                                                   asbd.mBitsPerChannel > 0 ? asbd.mBitsPerChannel : forcedBitsPerSample);

                        for (int i = 0; i < effectiveFrames; ++i)
                        {
                            L[(size_t) i] = decodeSample (pL + (size_t) i * bytesPerSample,
                                                          decodeAsFloat,
                                                          bitsPerSample,
                                                          bytesPerSample,
                                                          asbd.mFormatFlags);
                            R[(size_t) i] = decodeSample (pR + (size_t) i * bytesPerSample,
                                                          decodeAsFloat,
                                                          bitsPerSample,
                                                          bytesPerSample,
                                                          asbd.mFormatFlags);
                        }

                        if (outInfo != nullptr)
                        {
                            outInfo->bytesPerSampleUsed = bytesPerSample;
                            outInfo->bytesPerFrameUsed = bytesPerSample;
                            outInfo->effectiveFrames = effectiveFrames;
                        }

                        decoded = true;
                    }
                }
                else if (abl->mNumberBuffers == 1 && nonInterleaved)
                {
                    // 某些驱动会把non-interleaved写成单buffer，尝试按双plane拼接
                    const auto& b = abl->mBuffers[0];
                    const auto* p = static_cast<const uint8_t*> (b.mData);

                    uint32_t bytesPerSample = (asbd.mBytesPerFrame > 0) ? asbd.mBytesPerFrame : forcedBytesPerSample;
                    bytesPerSample = juce::jmax ((uint32_t) 1u, bytesPerSample);

                    const size_t planeBytes = (size_t) b.mDataByteSize / 2u;
                    const int frames = (int) (planeBytes / bytesPerSample);
                    const int effectiveFrames = juce::jmin (numSamples, frames);

                    if (p != nullptr && effectiveFrames > 0)
                    {
                        const auto* pL = p;
                        const auto* pR = p + planeBytes;

                        L.assign ((size_t) effectiveFrames, 0.0f);
                        R.assign ((size_t) effectiveFrames, 0.0f);

                        const uint32_t bitsPerSample = juce::jmax (forcedBitsPerSample,
                                                                   asbd.mBitsPerChannel > 0 ? asbd.mBitsPerChannel : forcedBitsPerSample);

                        for (int i = 0; i < effectiveFrames; ++i)
                        {
                            L[(size_t) i] = decodeSample (pL + (size_t) i * bytesPerSample,
                                                          decodeAsFloat,
                                                          bitsPerSample,
                                                          bytesPerSample,
                                                          asbd.mFormatFlags);
                            R[(size_t) i] = decodeSample (pR + (size_t) i * bytesPerSample,
                                                          decodeAsFloat,
                                                          bitsPerSample,
                                                          bytesPerSample,
                                                          asbd.mFormatFlags);
                        }

                        if (outInfo != nullptr)
                        {
                            outInfo->bytesPerSampleUsed = bytesPerSample;
                            outInfo->bytesPerFrameUsed = bytesPerSample;
                            outInfo->effectiveFrames = effectiveFrames;
                        }

                        decoded = true;
                    }
                }
            }
            else
            {
                if (abl->mNumberBuffers == 1)
                {
                    const auto& b = abl->mBuffers[0];
                    const auto* p = static_cast<const uint8_t*> (b.mData);
                    if (p != nullptr && b.mDataByteSize > 0)
                    {
                        uint32_t bytesPerFrame = asbd.mBytesPerFrame;
                        if (bytesPerFrame == 0 || nonInterleaved)
                            bytesPerFrame = forcedBytesPerSample * channels;
                        bytesPerFrame = juce::jmax ((uint32_t) 1u, bytesPerFrame);

                        const uint32_t bytesPerSample = juce::jmax ((uint32_t) 1u, bytesPerFrame / channels);
                        const uint32_t bitsPerSample = juce::jmax (forcedBitsPerSample,
                                                                   asbd.mBitsPerChannel > 0 ? asbd.mBitsPerChannel : forcedBitsPerSample);

                        const int framesByBytes = (int) (b.mDataByteSize / bytesPerFrame);
                        const int effectiveFrames = juce::jmin (numSamples, framesByBytes);

                        if (effectiveFrames > 0)
                        {
                            L.assign ((size_t) effectiveFrames, 0.0f);
                            R.assign ((size_t) effectiveFrames, 0.0f);

                            for (int i = 0; i < effectiveFrames; ++i)
                            {
                                const auto* frame = p + (size_t) i * bytesPerFrame;
                                L[(size_t) i] = decodeSample (frame,
                                                              decodeAsFloat,
                                                              bitsPerSample,
                                                              bytesPerSample,
                                                              asbd.mFormatFlags);
                                if (channels >= 2)
                                    R[(size_t) i] = decodeSample (frame + bytesPerSample,
                                                                  decodeAsFloat,
                                                                  bitsPerSample,
                                                                  bytesPerSample,
                                                                  asbd.mFormatFlags);
                                else
                                    R[(size_t) i] = L[(size_t) i];
                            }

                            if (outInfo != nullptr)
                            {
                                outInfo->bytesPerSampleUsed = bytesPerSample;
                                outInfo->bytesPerFrameUsed = bytesPerFrame;
                                outInfo->effectiveFrames = effectiveFrames;
                            }

                            decoded = true;
                        }
                    }
                }
            }
        }

        if (retainedBlockBuffer != nullptr)
            CFRelease (retainedBlockBuffer);

        if (decoded)
            return true;

        // fallback: 从CMBlockBuffer读取（支持non-interleaved/planar）
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer (sampleBuffer);
        if (block == nullptr)
            return false;

        size_t totalLength = 0;
        char* dataPointer = nullptr;
        const OSStatus blockStatus = CMBlockBufferGetDataPointer (block, 0, nullptr, &totalLength, &dataPointer);
        if (blockStatus != noErr || dataPointer == nullptr)
            return false;

        const auto* p = reinterpret_cast<const uint8_t*> (dataPointer);
        const bool nonInterleaved = isNonInterleavedFormat (asbd);
        const uint32_t channels = juce::jmax ((uint32_t) 1u, asbd.mChannelsPerFrame);

        if (nonInterleaved)
        {
            // non-interleaved场景下，不允许interleaved候选误读plane布局
            if (! forcePlanar)
                return false;

            uint32_t bytesPerSample = asbd.mBytesPerFrame;
            if (bytesPerSample == 0)
                bytesPerSample = forcedBytesPerSample;
            bytesPerSample = juce::jmax ((uint32_t) 1u, bytesPerSample);

            const int framesByBytes = (int) (totalLength / ((size_t) bytesPerSample * (size_t) channels));
            const int effectiveFrames = juce::jmin (numSamples, framesByBytes);
            if (effectiveFrames <= 0)
                return false;

            const size_t planeBytes = (size_t) effectiveFrames * (size_t) bytesPerSample;
            if (totalLength < planeBytes * (size_t) channels)
                return false;

            const uint32_t bitsPerSample = juce::jmax (forcedBitsPerSample,
                                                       asbd.mBitsPerChannel > 0 ? asbd.mBitsPerChannel : forcedBitsPerSample);

            L.assign ((size_t) effectiveFrames, 0.0f);
            R.assign ((size_t) effectiveFrames, 0.0f);

            const auto* pL = p;
            const auto* pR = (channels >= 2) ? (p + planeBytes) : p;

            for (int i = 0; i < effectiveFrames; ++i)
            {
                L[(size_t) i] = decodeSample (pL + (size_t) i * bytesPerSample,
                                              decodeAsFloat,
                                              bitsPerSample,
                                              bytesPerSample,
                                              asbd.mFormatFlags);
                R[(size_t) i] = decodeSample (pR + (size_t) i * bytesPerSample,
                                              decodeAsFloat,
                                              bitsPerSample,
                                              bytesPerSample,
                                              asbd.mFormatFlags);
            }

            if (outInfo != nullptr)
            {
                outInfo->bytesPerSampleUsed = bytesPerSample;
                outInfo->bytesPerFrameUsed = bytesPerSample;
                outInfo->effectiveFrames = effectiveFrames;
            }

            return true;
        }

        if (forcePlanar)
            return false;

        uint32_t bytesPerFrame = asbd.mBytesPerFrame;
        if (bytesPerFrame == 0)
            bytesPerFrame = forcedBytesPerSample * channels;
        bytesPerFrame = juce::jmax ((uint32_t) 1u, bytesPerFrame);

        const uint32_t bytesPerSample = juce::jmax ((uint32_t) 1u, bytesPerFrame / channels);
        const uint32_t bitsPerSample = juce::jmax (forcedBitsPerSample,
                                                   asbd.mBitsPerChannel > 0 ? asbd.mBitsPerChannel : forcedBitsPerSample);

        const int framesByBytes = (int) (totalLength / (size_t) bytesPerFrame);
        const int effectiveFrames = juce::jmin (numSamples, framesByBytes);
        if (effectiveFrames <= 0)
            return false;

        L.assign ((size_t) effectiveFrames, 0.0f);
        R.assign ((size_t) effectiveFrames, 0.0f);

        for (int i = 0; i < effectiveFrames; ++i)
        {
            const auto* frame = p + (size_t) i * bytesPerFrame;
            L[(size_t) i] = decodeSample (frame, decodeAsFloat, bitsPerSample, bytesPerSample, asbd.mFormatFlags);
            if (channels >= 2)
                R[(size_t) i] = decodeSample (frame + bytesPerSample, decodeAsFloat, bitsPerSample, bytesPerSample, asbd.mFormatFlags);
            else
                R[(size_t) i] = L[(size_t) i];
        }

        if (outInfo != nullptr)
        {
            outInfo->bytesPerSampleUsed = bytesPerSample;
            outInfo->bytesPerFrameUsed = bytesPerFrame;
            outInfo->effectiveFrames = effectiveFrames;
        }

        return true;
    }

    void logMonitorIfNeeded (double durationSec,
                             DecodePath selected,
                             const SignalStats& selectedStats,
                             const DecodeAttemptInfo& selectedInfo,
                             const juce::Array<CandidateResult>& candidates,
                             bool suspicious)
    {
        // logging removed
    }

    void handleAudioSampleBuffer (CMSampleBufferRef sampleBuffer)
    {
        // 外网 #1：记录最近一次音频缓冲到达时间（watchdog 无数据兜底用）
        lastSampleMachTicks.store (mach_absolute_time(), std::memory_order_relaxed);

        if (owner == nullptr || sampleBuffer == nullptr)
            return;

        if (! CMSampleBufferIsValid (sampleBuffer) || ! CMSampleBufferDataIsReady (sampleBuffer))
            return;

        const int numSamples = (int) CMSampleBufferGetNumSamples (sampleBuffer);
        if (numSamples <= 0)
            return;

        const auto fmt = CMSampleBufferGetFormatDescription (sampleBuffer);
        const auto asbdPtr = fmt != nullptr ? CMAudioFormatDescriptionGetStreamBasicDescription (fmt) : nullptr;
        if (asbdPtr == nullptr)
            return;

        const auto& asbd = *asbdPtr;
        const double sampleRate = asbd.mSampleRate > 1.0 ? asbd.mSampleRate : 48000.0;
        const double durationSec = (sampleRate > 1.0) ? ((double) numSamples / sampleRate) : 0.0;

        if (coldStartRemainSec > 0.0)
            coldStartRemainSec = juce::jmax (0.0, coldStartRemainSec - durationSec);

        if (fallbackRetryCooldownSec > 0.0)
            fallbackRetryCooldownSec = juce::jmax (0.0, fallbackRetryCooldownSec - durationSec);

        logAsbdIfNeeded (asbd, durationSec);

        const bool suspicious = isSuspiciousFormat (asbd);

        if (owner->onAudio == nullptr)
        {
            noPayloadElapsedSec += durationSec;

            if (decodeMode != DecodeMode::autoSelect)
            {
                forcedModeRemainSec = juce::jmax (0.0, forcedModeRemainSec - durationSec);
                if (forcedModeRemainSec <= 0.0)
                {
                    decodeMode = DecodeMode::autoSelect;
                    fullScanRemainSec = 0.0;
                    fullScanCooldownSec = juce::jmax (fullScanCooldownSec, 1.0);
                    pendingSelectedWindows = 0;
                }
            }

            return;
        }

        noPayloadElapsedSec = 0.0;

        if (decodeMode != DecodeMode::autoSelect && forcedModeRemainSec > 0.0)
        {
            forcedModeRemainSec -= durationSec;
            if (forcedModeRemainSec <= 0.0)
            {
                decodeMode = DecodeMode::autoSelect;
                forcedModeRemainSec = 0.0;
                fullScanRemainSec = 0.8;
                fullScanCooldownSec = 0.0;
                pendingSelectedWindows = 0;
            }
        }

        juce::Array<CandidateResult> candidates;
        auto addCandidate = [&] (DecodePath p)
        {
            CandidateResult c;
            c.path = p;
            c.ok = decodeSampleBufferWithPath (sampleBuffer, asbd, numSamples, p, c.L, c.R, &c.info);
            if (c.ok)
                c.stats = evaluateSignal (c.L, c.R);
            candidates.add (std::move (c));
        };

        // 稳态快路径下，先轻量探测一次，如果连首选路径都拿不到有效 frame，
        // 就直接跳过本轮（不再触发全量评估），等系统给出真正有内容的窗口。
        // 这是"不该算就不算"的最小保护：持续静音/空缓冲不会被放大成 CPU 尖峰。
        if (decodeMode == DecodeMode::autoSelect
            && stablePathReady
            && fullScanRemainSec <= 0.0
            && fallbackRetryCooldownSec > 0.0)
        {
            CandidateResult probe;
            probe.path = stableSelectedPath;
            probe.ok = decodeSampleBufferWithPath (sampleBuffer, asbd, numSamples,
                                                   probe.path, probe.L, probe.R, &probe.info);

            const bool probeEmpty = (! probe.ok)
                                  || probe.info.effectiveFrames <= 0
                                  || probe.info.numBuffers == 0
                                  || (probe.info.buffer0Bytes == 0 && probe.info.buffer1Bytes == 0);

            if (probeEmpty)
                return;
        }

        bool runFullEvaluation = false;
        if (decodeMode == DecodeMode::autoSelect)
        {
            if (! stablePathReady)
                runFullEvaluation = true;

            if (fullScanCooldownSec > 0.0)
                fullScanCooldownSec = juce::jmax (0.0, fullScanCooldownSec - durationSec);

            if (fullScanRemainSec > 0.0)
            {
                runFullEvaluation = true;
                fullScanRemainSec = juce::jmax (0.0, fullScanRemainSec - durationSec);

                if (fullScanRemainSec <= 0.0)
                    fullScanCooldownSec = juce::jmax (fullScanCooldownSec, 4.0);
            }
            else
            {
                fullScanProbeElapsedSec += durationSec;

                // 冷启动阶段优先全量评估，尽快收敛到稳定解码路径。
                if (coldStartRemainSec > 0.0)
                {
                    fullScanRemainSec = 0.3;
                    runFullEvaluation = true;
                }
                // 稳态阶段改为低频探测：每8秒给一次短窗口，且要求不在cooldown。
                else if (fullScanCooldownSec <= 0.0 && fullScanProbeElapsedSec >= 8.0 && ! suspicious)
                {
                    fullScanProbeElapsedSec = 0.0;
                    fullScanRemainSec = 0.12;
                    runFullEvaluation = true;
                }
            }
        }

        DecodePath preferredPath = decodePathFromMode (asbd);

        if (decodeMode == DecodeMode::autoSelect && ! runFullEvaluation)
        {
            preferredPath = stableSelectedPath;
            addCandidate (preferredPath);

            if (candidates.isEmpty() || ! candidates.getReference(0).ok)
            {
                candidates.clear();
                runFullEvaluation = true;
                fullScanRemainSec = 0.25;
            }
        }

        if (decodeMode != DecodeMode::autoSelect)
            addCandidate (preferredPath);

        if (runFullEvaluation)
        {
            const DecodePath allPaths[] =
            {
                DecodePath::planarFloat32,
                DecodePath::planarInt16,
                DecodePath::interleavedFloat32,
                DecodePath::interleavedInt16
            };

            for (auto p : allPaths)
                addCandidate (p);
        }

        if (candidates.isEmpty())
            return;

        int bestIdx = -1;
        double bestScore = -1.0e9;
        for (int i = 0; i < candidates.size(); ++i)
        {
            const auto& c = candidates.getReference(i);
            if (! c.ok)
                continue;
            if (! isPathAllowedByMode (c.path))
                continue;

            const bool compatible = isPathCompatibleWithAsbd (c.path, asbd);
            if (! suspicious && ! compatible)
                continue;

            double score = scoreSignal (c.stats);
            if (! compatible)
                score -= 30.0;

            if (score > bestScore)
            {
                bestScore = score;
                bestIdx = i;
            }
        }

        if (bestIdx < 0)
            return;

        DecodePath selectedPath = candidates.getReference(bestIdx).path;

        if (decodeMode == DecodeMode::autoSelect)
        {
            if (runFullEvaluation)
            {
                if (! stablePathReady)
                {
                    stableSelectedPath = selectedPath;
                    stablePathReady = true;
                    pendingSelectedWindows = 0;
                }
                else if (selectedPath != stableSelectedPath)
                {
                    if (selectedPath == pendingSelectedPath)
                        ++pendingSelectedWindows;
                    else
                    {
                        pendingSelectedPath = selectedPath;
                        pendingSelectedWindows = 1;
                    }

                    if (pendingSelectedWindows >= 3)
                    {
                        stableSelectedPath = selectedPath;
                        pendingSelectedWindows = 0;
                    }
                }
                else
                {
                    pendingSelectedWindows = 0;
                }

                // 全量评估时仍优先使用稳态路径，避免窗口间抖动。
                for (int i = 0; i < candidates.size(); ++i)
                {
                    const auto& c = candidates.getReference(i);
                    if (c.ok && c.path == stableSelectedPath)
                    {
                        selectedPath = stableSelectedPath;
                        bestIdx = i;
                        break;
                    }
                }
            }
            else
            {
                selectedPath = stableSelectedPath;
                bestIdx = 0;
            }
        }

        const auto& selected = candidates.getReference(bestIdx);
        if (! selected.ok)
            return;

        lastSelectedPath = selectedPath;

        if (decodeMode == DecodeMode::autoSelect && ! runFullEvaluation)
        {
            if (selected.stats.pathologicalDc || selected.stats.clipRatio > 0.08)
            {
                fullScanRemainSec = 0.25;
                fullScanCooldownSec = 0.0;
            }
        }

        const bool hasPayload = selected.info.effectiveFrames > 0
                             && selected.info.numBuffers > 0
                             && (selected.info.buffer0Bytes > 0 || selected.info.buffer1Bytes > 0);

        // 空缓冲或 numBuffers=0 的窗口不参与健康检查：这类窗口本身就没有真实音频内容，
        // 如果累计进 unhealthyDurationSec 会把"静音/空闲"误判成"解码坏了"，导致反复 fallback。
        if (selected.stats.lowLevel && hasPayload)
            unhealthyDurationSec += durationSec;
        else
            unhealthyDurationSec = juce::jmax (0.0, unhealthyDurationSec - durationSec * 0.5);

        // fallback 防抖：冷却期内不再触发，避免 autoSelect <-> force* 抖动。冷却时间从 2.5s 提到 4s。
        if (unhealthyDurationSec >= 1.0 && fallbackRetryCooldownSec <= 0.0 && hasPayload)
        {
            const DecodePath backup = fallbackBackupPath (lastSelectedPath);

            decodeMode = isFloatPath (backup) ? DecodeMode::forceFloat : DecodeMode::forceInteger;
            forcedModeRemainSec = 2.0;
            fallbackRetryCooldownSec = 4.0;
            unhealthyDurationSec = 0.0;
        }

        logMonitorIfNeeded (durationSec, selectedPath, selected.stats, selected.info, candidates, suspicious);

        if (owner->onAudio)
        {
            const int n = juce::jmin ((int) selected.L.size(), (int) selected.R.size());
            if (n > 0)
            {
                pendingDispatchL.reserve (pendingDispatchL.size() + (size_t) n);
                pendingDispatchR.reserve (pendingDispatchR.size() + (size_t) n);

                for (int i = 0; i < n; ++i)
                {
                    pendingDispatchL.push_back (protectOutputSample (selected.L[(size_t) i]));
                    pendingDispatchR.push_back (protectOutputSample (selected.R[(size_t) i]));
                }

                pendingDispatchSampleRate = sampleRate;
                dispatchElapsedSec += durationSec;

                const bool urgentDispatch = selected.stats.pathologicalDc || selected.stats.clipRatio > 0.08;
                const bool timeReady = dispatchElapsedSec >= 0.010;
                const bool blockReady = pendingDispatchL.size() >= (size_t) 2048;

                if (urgentDispatch || timeReady || blockReady)
                {
                    const int dispatchN = juce::jmin ((int) pendingDispatchL.size(), (int) pendingDispatchR.size());
                    if (dispatchN > 0)
                        owner->onAudio (pendingDispatchL.data(), pendingDispatchR.data(), dispatchN, pendingDispatchSampleRate);

                    pendingDispatchL.clear();
                    pendingDispatchR.clear();
                    dispatchElapsedSec = 0.0;
                }
            }
        }
    }

    static juce::String buildPermissionHelpText()
    {
        juce::String appName = "This app";
        juce::String bundleId = "(unknown bundle id)";

        if (NSBundle* bundle = [NSBundle mainBundle])
        {
            id name = [bundle objectForInfoDictionaryKey:@"CFBundleName"];
            if (name != nil)
                appName = juce::String::fromUTF8 ([[name description] UTF8String]);

            NSString* bid = [bundle bundleIdentifier];
            if (bid != nil)
                bundleId = juce::String::fromUTF8 (bid.UTF8String);
        }

        // 说明：新装/升级后的 ad-hoc 签名 app 常出现 "TCC 记录存在但 csreq
        //   与新签名不匹配"的半残状态。此时用户仅切换"允许"开关是无效的，
        //   必须先把 app 从"屏幕与系统音频录制"列表移除（-号），退出 app，
        //   再重新打开 app 让它触发一次授权请求。以下文本把这一步骤明确
        //   写清楚，避免用户误以为只需切开关。
        return juce::String::fromUTF8 (u8"⚠️ 无法采集系统输出音频：\n")
             + juce::String::fromUTF8 (u8"macOS 拒绝了本 App 的\"屏幕与系统音频录制\"权限 (TCC)。\n\n")
             + juce::String::fromUTF8 (u8"请按以下步骤操作（这是 macOS 对 ad-hoc 签名 App 的限制）：\n")
             + juce::String::fromUTF8 (u8"  1) 打开 系统设置 → 隐私与安全性 → 屏幕与系统音频录制\n")
             + juce::String::fromUTF8 (u8"  2) 在列表里找到 ") + appName + juce::String::fromUTF8 (u8"，点右下角的 \"-\" 把它移除\n")
             + juce::String::fromUTF8 (u8"  3) 完全退出 ") + appName + juce::String::fromUTF8 (u8"（Cmd+Q）\n")
             + juce::String::fromUTF8 (u8"  4) 重新打开 ") + appName + juce::String::fromUTF8 (u8"，再次点击\"抓取系统输出\"，弹出授权时点\"允许\"\n\n")
             + juce::String::fromUTF8 (u8"⚠️ 请勿只切换开关——升级后旧的授权记录已失效，\n")
             + juce::String::fromUTF8 (u8"    必须先移除、再重新添加，权限才会真正生效。\n\n")
             + "-------- English --------\n"
             + "macOS denied Screen Recording / System Audio capture permission (TCC).\n\n"
             + "Steps to fix (required for ad-hoc signed apps):\n"
             + "  1) Open System Settings -> Privacy & Security -> Screen & System Audio Recording\n"
             + "  2) Select \"" + appName + "\" in the list, click the \"-\" button to REMOVE it\n"
             + "  3) Fully quit \"" + appName + "\" (Cmd+Q)\n"
             + "  4) Relaunch \"" + appName + "\", trigger System Output again, and click Allow\n\n"
             + "Do NOT just toggle the switch: the stale TCC record must be deleted first.\n\n"
             + "App: " + appName + "\n"
             + "Bundle ID: " + bundleId;
    }

    static bool hasScreenCapturePermission()
    {
        return CGPreflightScreenCaptureAccess();
    }

    static bool requestScreenCapturePermission()
    {
        return CGRequestScreenCaptureAccess();
    }

    bool startCapture (juce::String& err)
    {
        if (@available(macOS 13.0, *))
        {
            // 已在采集（例如刚由 watchdog 自愈重建成功、UI 又排队一次 start）→ 直接成功
            if (streamWanted.load (std::memory_order_relaxed))
                return true;

            registerSleepWakeObservers();

            if (! hasScreenCapturePermission())
            {
                (void) requestScreenCapturePermission();

                if (! hasScreenCapturePermission())
                {
                    err = buildPermissionHelpText();
                    return false;
                }
            }

            __block bool ok = false;
            __block juce::String localError;

            const dispatch_semaphore_t sem = dispatch_semaphore_create (0);

            [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                                      onScreenWindowsOnly:NO
                                                         completionHandler:^ (SCShareableContent* content, NSError* error)

            {
                if (error != nil)
                {
                    juce::String detail = juce::String::fromUTF8 (error.localizedDescription.UTF8String);
                    const juce::String detailLower = detail.toLowerCase();
                    if (detailLower.contains ("tcc") || detailLower.contains ("denied") || detailLower.contains ("拒绝"))
                        localError = buildPermissionHelpText();
                    else
                        localError = juce::String ("SCShareableContent failed: ") + detail;

                    dispatch_semaphore_signal (sem);
                    return;
                }

                if (content == nil || content.displays.count == 0)
                {
                    localError = "No shareable display found for ScreenCaptureKit.";
                    dispatch_semaphore_signal (sem);
                    return;
                }

                SCDisplay* display = content.displays.firstObject;
                SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                              excludingApplications:@[]
                                                                 exceptingWindows:@[]];

                SCStreamConfiguration* config = [SCStreamConfiguration new];
                // 仅采集系统音频：优先显式关闭视频流，降低系统消息量与无效调度。
                config.capturesAudio = YES;
                config.excludesCurrentProcessAudio = NO;
                config.sampleRate = 48000;
                config.channelCount = 2;

                bool needScreenOutputCompatibility = true;
                if ([config respondsToSelector:@selector(setCapturesVideo:)])
                {
                    [config setValue:@(NO) forKey:@"capturesVideo"];
                    needScreenOutputCompatibility = false;
                }

                stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:nil];
                output = [Y2KSCStreamAudioOutput new];
                output.impl = this;

                queue = dispatch_queue_create ("cn.iisaacbeats.y2kmeter.desktopaudio", DISPATCH_QUEUE_SERIAL);

                NSError* addOutputError = nil;
                const bool added = [stream addStreamOutput:output
                                                      type:SCStreamOutputTypeAudio
                                        sampleHandlerQueue:queue
                                                     error:&addOutputError];
                if (! added)
                {
                    localError = juce::String ("Failed to add SCStream audio output: ")
                               + juce::String::fromUTF8 (addOutputError.localizedDescription.UTF8String);
                    stream = nil;
                    output = nil;
                    queue = nullptr;
                    dispatch_semaphore_signal (sem);
                    return;
                }

                if (needScreenOutputCompatibility)
                {
                    // 旧系统/旧SDK缺少capturesVideo时，保留兼容分支，避免反复"stream output NOT found"日志。
                    NSError* addScreenOutputError = nil;
                    [stream addStreamOutput:output
                                       type:SCStreamOutputTypeScreen
                         sampleHandlerQueue:queue
                                      error:&addScreenOutputError];
                }

                [stream startCaptureWithCompletionHandler:^ (NSError* startError)
                {
                    if (startError != nil)
                    {
                        localError = juce::String ("Failed to start ScreenCaptureKit stream: ")
                                   + juce::String::fromUTF8 (startError.localizedDescription.UTF8String);
                        ok = false;
                    }
                    else
                    {
                        ok = true;
                    }
                    dispatch_semaphore_signal (sem);
                }];
            }];

            const auto timeoutResult = dispatch_semaphore_wait (sem, dispatch_time (DISPATCH_TIME_NOW, (int64_t) (5.0 * NSEC_PER_SEC)));
            if (timeoutResult != 0)
            {
                err = "Timed out while starting ScreenCaptureKit desktop audio capture.";
                stopCapture();
                return false;
            }

            if (! ok)
            {
                err = localError.isNotEmpty() ? localError : juce::String ("Unknown error while starting desktop audio capture.");
                stopCapture();
                return false;
            }

            coldStartRemainSec = 2.0;
            asbdLogElapsedSec = 0.0;
            monitorLogElapsedSec = 0.0;
            fullScanProbeElapsedSec = 0.0;
            fallbackRetryCooldownSec = 0.0;
            noPayloadElapsedSec = 0.0;
            dispatchElapsedSec = 0.0;
            pendingDispatchL.clear();
            pendingDispatchR.clear();

            // 外网 #1：启动无数据兜底 watchdog（唤醒重建 / 静默失效自愈）
            armWatchdog();
            streamWanted.store (true, std::memory_order_relaxed);

            return true;
        }

        err = "Desktop audio capture requires macOS 13.0 or newer.";
        return false;
    }

    void stopCapture()
    {
        // 外网 #1：先摘掉 watchdog 与休眠通知，再释放 SCStream
        streamWanted.store (false, std::memory_order_relaxed);
        disarmWatchdog();
        unregisterSleepWakeObservers();

        if (stream == nil)
        {
            output = nil;
            queue = nullptr;
            return;
        }

        const dispatch_semaphore_t sem = dispatch_semaphore_create (0);
        [stream stopCaptureWithCompletionHandler:^ (NSError*)
        {
            dispatch_semaphore_signal (sem);
        }];
        dispatch_semaphore_wait (sem, dispatch_time (DISPATCH_TIME_NOW, (int64_t) (2.0 * NSEC_PER_SEC)));

        if (output != nil)
        {
            NSError* removeErr = nil;
            [stream removeStreamOutput:output type:SCStreamOutputTypeScreen error:&removeErr];
            [stream removeStreamOutput:output type:SCStreamOutputTypeAudio error:&removeErr];
            (void) removeErr;
        }

        stream = nil;
        output = nil;
        queue = nullptr;
    }
};

} // namespace y2k

// ==========================================================
// 休眠/唤醒通知桥接实现（外网 #1）
// ==========================================================
@implementation Y2KDesktopCaptureLifeProxy

- (void)onWorkspaceWillSleep:(NSNotification*)note
{
    (void) note;
    if (self.impl != nullptr)
        static_cast<y2k::MacDesktopAudioCapture::Impl*> (self.impl)->markSleeping();
}

- (void)onWorkspaceDidWake:(NSNotification*)note
{
    (void) note;
    if (self.impl != nullptr)
        static_cast<y2k::MacDesktopAudioCapture::Impl*> (self.impl)->markWoken();
}

@end

@implementation Y2KSCStreamAudioOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type API_AVAILABLE(macos(13.0))
{
    (void) stream;
    if (type != SCStreamOutputTypeAudio)
        return;

    if (self.impl != nullptr)
        static_cast<y2k::MacDesktopAudioCapture::Impl*> (self.impl)->handleAudioSampleBuffer (sampleBuffer);
}

@end

namespace y2k
{

MacDesktopAudioCapture::MacDesktopAudioCapture() : impl (std::make_unique<Impl>())
{
    impl->owner = this;
}

MacDesktopAudioCapture::~MacDesktopAudioCapture()
{
    stop();
}

bool MacDesktopAudioCapture::start()
{
    if (running.load (std::memory_order_acquire))
        return true;

    lastError = {};

    if (impl == nullptr)
        impl = std::make_unique<Impl>();

    impl->owner = this;

    // 外网 #1：start/stop 与 watchdog 自愈统一在 lifecycleQueue 上串行化，
    // 避免 UI 线程与自愈重建并发操作 SCStream。
    struct StartResult
    {
        bool         ok = false;
        juce::String err;
    } result;

    Impl* p = impl.get();
    StartResult* pr = &result;
    dispatch_sync (p->lifecycleQueue, ^{
        pr->ok = p->startCapture (pr->err);
    });

    if (! result.ok)
    {
        lastError = result.err;
        running.store (false, std::memory_order_release);
        return false;
    }

    running.store (true, std::memory_order_release);
    return true;
}

void MacDesktopAudioCapture::stop()
{
    if (impl != nullptr)
    {
        Impl* p = impl.get();

        // 1) 在生命周期串行队列上停止采集（排队中的自愈恢复会先完成，
        //    最终 streamWanted=false，恢复任务若排在后面会直接空操作）
        dispatch_sync (p->lifecycleQueue, ^{
            p->stopCapture();
        });

        // 2) 取消 watchdog 并排空其队列 —— 此后不再产生新的恢复任务
        p->disarmWatchdog();
        if (p->watchdogQueue != nullptr)
            dispatch_sync (p->watchdogQueue, ^{});

        // 3) 再次排空生命周期队列：步骤 1~2 之间 tick 已派发的恢复任务在此执行完
        //    （此时 streamWanted=false，恢复任务内部直接返回）
        dispatch_sync (p->lifecycleQueue, ^{});
    }

    running.store (false, std::memory_order_release);
}

} // namespace y2k

#else

namespace y2k
{
MacDesktopAudioCapture::MacDesktopAudioCapture() = default;
MacDesktopAudioCapture::~MacDesktopAudioCapture() = default;
bool MacDesktopAudioCapture::start()
{
    lastError = "Mac desktop audio capture is only supported on macOS.";
    return false;
}
void MacDesktopAudioCapture::stop() {}
}

#endif