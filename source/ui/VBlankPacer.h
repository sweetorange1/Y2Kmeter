#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// ==========================================================
// VBlankPacer —— Pure C++17 VBlank credit clock & fallback pacer
//
// Zero JUCE dependencies so it can be directly unit-tested by
// Y2Kmeter_perf_pacing_test and shared with PluginEditor.
//
// Fixes five 60Hz/mixed-refresh pacing issues:
//   1. Detects actual monitor refresh rate from median VBlank intervals
//      (immune to single 33.33ms dropped-VBlank outliers).
//   2. Caps effective target rate to min(userCap, detectedMonitorHz) so
//      e.g. default 120FPS cap on a 60Hz display never double-dispatches.
//   3. Uses a 1:1 native VBlank fast-path when userCap <= 0 or
//      userCap >= detectedMonitorHz - 1.5 (no fractional credit jitter skips).
//   4. Checks <1ms dedup BEFORE modifying credit and clamps credit >= 0.0
//      (eliminates negative credit debt & dedup credit burn).
//   5. Uses a 1.35x grace window on fallback timer (active even for
//      userCap == 0 unlimited mode) and enforces strictly monotonic timestamps.
// ==========================================================
namespace y2k::pacing
{

class VBlankPacer
{
public:
    static constexpr std::size_t kMonitorWindowSize       = 9;
    static constexpr double      kMinValidVBlankMs        = 2.0;    // 500 Hz upper bound
    static constexpr double      kMaxValidVBlankMs        = 100.0;  // 10 Hz lower bound
    static constexpr double      kDedupMinMs              = 1.0;
    static constexpr double      kCreditToleranceMs       = 0.5;
    static constexpr double      kFallbackGraceMultiplier = 1.35;
    static constexpr double      kNativeSnapToleranceHz   = 1.5;

    void reset (bool keepMonitorEstimate = false) noexcept
    {
        vblankCreditMs_     = 0.0;
        lastDispatchWallMs_ = 0.0;

        if (! keepMonitorEstimate)
        {
            vblankDtWindow_.fill (0.0);
            vblankDtCount_          = 0;
            vblankDtCursor_         = 0;
            detectedMonitorHz_      = 0.0;
            lastDispatchedSec_      = 0.0;
            lastDispatchTimeWallMs_ = 0.0;
        }
    }

    void recordVBlankIntervalMs (double dtMs) noexcept
    {
        if (! std::isfinite (dtMs) || dtMs < kMinValidVBlankMs || dtMs > kMaxValidVBlankMs)
            return;

        vblankDtWindow_[vblankDtCursor_] = dtMs;
        vblankDtCursor_ = (vblankDtCursor_ + 1) % kMonitorWindowSize;
        if (vblankDtCount_ < kMonitorWindowSize)
            ++vblankDtCount_;

        if (vblankDtCount_ < 3)
            return;

        std::array<double, kMonitorWindowSize> sorted {};
        for (std::size_t i = 0; i < vblankDtCount_; ++i)
            sorted[i] = vblankDtWindow_[i];

        std::sort (sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t> (vblankDtCount_));
        const double medianMs = sorted[vblankDtCount_ / 2];
        if (medianMs > 0.0)
            detectedMonitorHz_ = std::clamp (1000.0 / medianMs, 20.0, 480.0);
    }

    double detectedMonitorHz() const noexcept { return detectedMonitorHz_; }

    double effectiveTargetHz (int userCap) const noexcept
    {
        if (detectedMonitorHz_ > 1.0)
        {
            if (userCap <= 0)
                return detectedMonitorHz_;
            return std::min (static_cast<double> (userCap), detectedMonitorHz_);
        }
        return (userCap > 0) ? static_cast<double> (userCap) : 60.0;
    }

    bool shouldDispatchOnVBlank (int userCap, double dtMs, double nowWallMs) noexcept
    {
        if (dtMs > 0.0)
            recordVBlankIntervalMs (dtMs);

        // Dedup BEFORE touching credit so duplicate/near-simultaneous events
        // never silently burn accumulated VBlank credit.
        if (lastDispatchWallMs_ > 0.0 && (nowWallMs - lastDispatchWallMs_) < kDedupMinMs)
            return false;

        // First VBlank with no elapsed dt yet: dispatch immediately to establish baseline.
        if (lastDispatchWallMs_ <= 0.0 && dtMs <= 0.0)
        {
            vblankCreditMs_     = 0.0;
            lastDispatchWallMs_ = nowWallMs;
            return true;
        }

        // Native 1:1 VBlank fast-path: unlimited or cap >= monitor refresh rate.
        const bool nativeRate = (userCap <= 0)
                             || (detectedMonitorHz_ > 1.0
                                 && static_cast<double> (userCap) >= detectedMonitorHz_ - kNativeSnapToleranceHz);
        if (nativeRate)
        {
            vblankCreditMs_     = 0.0;
            lastDispatchWallMs_ = nowWallMs;
            return true;
        }

        // Fractional down-cap path (e.g. 60fps on 144Hz or 30fps on 60Hz).
        const double intervalMs = 1000.0 / static_cast<double> (userCap);
        if (dtMs > 0.0 && dtMs < 250.0)
            vblankCreditMs_ += dtMs;

        if (vblankCreditMs_ > intervalMs * 2.0)
            vblankCreditMs_ = intervalMs * 2.0;

        if (vblankCreditMs_ >= intervalMs - kCreditToleranceMs)
        {
            // Never carry negative debt across jittery VBlanks.
            vblankCreditMs_     = std::clamp (vblankCreditMs_ - intervalMs, 0.0, intervalMs);
            lastDispatchWallMs_ = nowWallMs;
            return true;
        }

        return false;
    }

    bool shouldDispatchOnFallback (int userCap, double nowWallMs) noexcept
    {
        const double targetHz            = effectiveTargetHz (userCap);
        const double effectiveIntervalMs = 1000.0 / std::max (1.0, targetHz);

        if (lastDispatchWallMs_ <= 0.0)
        {
            lastDispatchWallMs_ = nowWallMs;
            vblankCreditMs_     = 0.0;
            return true;
        }

        const double elapsedMs = nowWallMs - lastDispatchWallMs_;
        if (elapsedMs >= effectiveIntervalMs * kFallbackGraceMultiplier)
        {
            lastDispatchWallMs_ = nowWallMs;
            vblankCreditMs_     = 0.0;
            return true;
        }

        return false;
    }

    double nextMonotonicTimeSec (bool fromVBlank, double vblankSec, double nowWallMs) noexcept
    {
        double candidate = 0.0;
        if (fromVBlank && std::isfinite (vblankSec) && vblankSec > 0.0)
        {
            candidate = vblankSec;
        }
        else if (lastDispatchedSec_ > 0.0 && lastDispatchTimeWallMs_ > 0.0)
        {
            // Advance fallback timestamps by elapsed wall-clock delta in the same
            // clock epoch as lastDispatchedSec_ (immune to VBlank vs wall epoch offsets).
            const double dtWallSec = std::clamp ((nowWallMs - lastDispatchTimeWallMs_) * 0.001,
                                                 0.0005, 0.25);
            candidate = lastDispatchedSec_ + dtWallSec;
        }
        else
        {
            candidate = nowWallMs * 0.001;
        }

        if (lastDispatchedSec_ > 0.0 && candidate <= lastDispatchedSec_)
            candidate = lastDispatchedSec_ + 0.0005;

        lastDispatchedSec_      = candidate;
        lastDispatchTimeWallMs_ = nowWallMs;
        return candidate;
    }

    int recommendedFallbackTimerMs (int userCap) const noexcept
    {
        const double targetHz            = effectiveTargetHz (userCap);
        const double effectiveIntervalMs = 1000.0 / std::max (1.0, targetHz);
        return std::clamp (static_cast<int> (std::lround (effectiveIntervalMs * 0.5)), 8, 16);
    }

    double creditMs() const noexcept           { return vblankCreditMs_; }
    double lastDispatchWallMs() const noexcept { return lastDispatchWallMs_; }
    double lastDispatchedTimeSec() const noexcept { return lastDispatchedSec_; }

private:
    std::array<double, kMonitorWindowSize> vblankDtWindow_ {};
    std::size_t vblankDtCount_      = 0;
    std::size_t vblankDtCursor_     = 0;
    double      detectedMonitorHz_  = 0.0;

    double      vblankCreditMs_         = 0.0;
    double      lastDispatchWallMs_     = 0.0;
    double      lastDispatchedSec_      = 0.0;
    double      lastDispatchTimeWallMs_ = 0.0;
};

} // namespace y2k::pacing
