#pragma once

// ==========================================================
// ToyGlTypes —— Pure C++17 ToyBox GL Atlas Packing, Sprite
//               Snapshot PODs, Quad Packer, and Degradation
//               State Machine (ZERO JUCE / GL dependencies).
//
//   Shared directly between production code (ToyKit /
//   ToyEmoji / ToyCanvasFx / GlToyRenderer) and CTest unit
//   tests (Y2Kmeter_toy_test / Y2Kmeter_perf_pacing_test).
// ==========================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace y2ktoy
{
    // ======================================================
    // 1. Single-Texture 3-Tier Emoji Atlas Layout (1024x1024)
    //
    //    Packs up to 98 emojis x 3 sizes (L=70, M=50, S=38 px)
    //    with a 1px transparent gutter around every image so
    //    hardware GL_LINEAR filtering never bleeds across slots.
    //      Tier 2 (L): 70x70 -> 72x72 cell, 14 cols x 7 rows =  98 slots, y=[0..504)
    //      Tier 1 (M): 50x50 -> 52x52 cell, 19 cols x 6 rows = 114 slots, y=[504..816)
    //      Tier 0 (S): 38x38 -> 40x40 cell, 25 cols x 4 rows = 100 slots, y=[816..976)
    //    Total height = 976 <= 1024 -> fits in one 1024x1024 RGBA8 texture (4 MB VRAM).
    // ======================================================
    struct AtlasSlot
    {
        int   x = 0;
        int   y = 0;
        int   w = 0;
        int   h = 0;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        bool  valid = false;
    };

    struct EmojiAtlasLayout
    {
        static constexpr int kAtlasWidth    = 1024;
        static constexpr int kAtlasHeight   = 1024;
        static constexpr int kMaxEmojiSlots = 98;   // EmojiId::Count == 97
        static constexpr int kNumSizeTiers  = 3;    // 0=S, 1=M, 2=L
        static constexpr int kGutterPx      = 1;

        static constexpr int imageSizeForTier (int sizeIndex) noexcept
        {
            switch (sizeIndex)
            {
                case 0:  return 38;   // 18 * 2 + 2
                case 1:  return 50;   // 24 * 2 + 2
                case 2:  return 70;   // 34 * 2 + 2
                default: return 50;
            }
        }

        static constexpr int bandBaseY (int sizeIndex) noexcept
        {
            // L sits at top (0..504), M in middle (504..816), S at bottom (816..976)
            switch (sizeIndex)
            {
                case 2:  return 0;
                case 1:  return 504;  // 7 * 72
                case 0:  return 816;  // 504 + 6 * 52
                default: return 504;
            }
        }

        static constexpr int cellSizeForTier (int sizeIndex) noexcept
        {
            return imageSizeForTier (sizeIndex) + kGutterPx * 2;
        }

        static constexpr int colsForTier (int sizeIndex) noexcept
        {
            return kAtlasWidth / cellSizeForTier (sizeIndex);
        }

        static constexpr bool isValidSlot (int emojiIndex, int sizeIndex) noexcept
        {
            return emojiIndex >= 0 && emojiIndex < kMaxEmojiSlots
                && sizeIndex  >= 0 && sizeIndex  < kNumSizeTiers;
        }

        static constexpr AtlasSlot slotFor (int emojiIndex, int sizeIndex) noexcept
        {
            AtlasSlot s {};
            if (! isValidSlot (emojiIndex, sizeIndex))
                return s;

            const int imgSz  = imageSizeForTier (sizeIndex);
            const int cellSz = cellSizeForTier (sizeIndex);
            const int cols   = colsForTier (sizeIndex);
            const int col    = emojiIndex % cols;
            const int row    = emojiIndex / cols;
            const int px     = col * cellSz + kGutterPx;
            const int py     = bandBaseY (sizeIndex) + row * cellSz + kGutterPx;

            if (px < 0 || py < 0 || px + imgSz > kAtlasWidth || py + imgSz > kAtlasHeight)
                return s;

            s.x = px;
            s.y = py;
            s.w = imgSz;
            s.h = imgSz;
            constexpr float invW = 1.0f / (float) kAtlasWidth;
            constexpr float invH = 1.0f / (float) kAtlasHeight;
            // Half-texel inset stays strictly inside the emoji pixels during bilinear filtering
            s.u0 = ((float) px + 0.25f) * invW;
            s.v0 = ((float) py + 0.25f) * invH;
            s.u1 = ((float) (px + imgSz) - 0.25f) * invW;
            s.v1 = ((float) (py + imgSz) - 0.25f) * invH;
            s.valid = true;
            return s;
        }
    };

    // ======================================================
    // 2. Exclusion Rects & Sprite Snapshot (Message -> GL)
    // ======================================================
    static constexpr int kMaxExclusionRects = 16;
    static constexpr int kMaxToySprites     = 400;

    struct ExclusionRect
    {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;

        constexpr bool isEmpty() const noexcept
        {
            return x1 <= x0 || y1 <= y0;
        }

        constexpr bool contains (float x, float y) const noexcept
        {
            return ! isEmpty() && x >= x0 && x <= x1 && y >= y0 && y <= y1;
        }
    };

    inline bool isPointExcluded (float x, float y,
                                 const ExclusionRect* rects, int count) noexcept
    {
        if (rects == nullptr || count <= 0)
            return false;
        for (int i = 0; i < count; ++i)
            if (rects[i].contains (x, y))
                return true;
        return false;
    }

    struct GlToySprite
    {
        float    cx       = 0.0f;
        float    cy       = 0.0f;
        float    drawSize = 24.0f;
        float    angleRad = 0.0f;
        float    scaleY   = 1.0f;
        float    alpha    = 1.0f;
        float    u0       = 0.0f;
        float    v0       = 0.0f;
        float    u1       = 0.0f;
        float    v1       = 0.0f;
        float    clipX0   = 0.0f;
        float    clipY0   = 0.0f;
        float    clipX1   = 0.0f;   // clipX1 <= clipX0 => unclipped
        float    clipY1   = 0.0f;
        float    haloR    = 0.0f;
        float    haloG    = 0.0f;
        float    haloB    = 0.0f;
        float    haloA    = 0.0f;   // > 0 => Party rainbow radial glow quad behind emoji
        float    dotR     = 0.0f;
        float    dotG     = 0.0f;
        float    dotB     = 0.0f;
        float    dotSize  = 5.0f;
        uint8_t  isDot    = 0;
        uint8_t  hasClip  = 0;
    };

    struct GlToySnapshot
    {
        uint64_t version        = 0;
        int      editorLogicalW = 0;
        int      editorLogicalH = 0;
        uint32_t atlasGen       = 0;
        std::vector<GlToySprite> sprites;
        std::array<ExclusionRect, kMaxExclusionRects> exclusions {};
        int      exclusionCount = 0;
    };

    // ======================================================
    // 3. Interleaved Vertex Packing (shared with benchmark)
    //    Modes: 0.0 = premul emoji atlas, 1.0 = radial halo, 2.0 = solid dot
    // ======================================================
    struct GlToyVertex
    {
        float x, y;               // Editor logical coords
        float u, v;               // Atlas UV (mode 0) or local [-1..1] (mode 1)
        float r, g, b, a;         // Premultiplied colour (modes 1/2) or (1,1,1,alpha) (mode 0)
        float cx0, cy0, cx1, cy1; // Per-sprite clip rect (cx1 <= cx0 => disabled)
        float mode;               // 0.0 = emoji, 1.0 = halo, 2.0 = dot
    };

    inline void packToySpritesToQuads (const GlToySprite* sprites, size_t count,
                                       std::vector<GlToyVertex>& outVerts,
                                       std::vector<uint16_t>& outIndices)
    {
        outVerts.clear();
        outIndices.clear();
        if (sprites == nullptr || count == 0)
            return;

        const size_t clampedCount = std::min<size_t> (count, (size_t) kMaxToySprites);
        outVerts.reserve (clampedCount * 8);
        outIndices.reserve (clampedCount * 12);

        auto pushQuad = [&] (float x0, float y0, float x1, float y1,
                             float x2, float y2, float x3, float y3,
                             float u0, float v0, float u1, float v1,
                             float r, float g, float b, float a,
                             float cx0, float cy0, float cx1, float cy1,
                             float mode)
        {
            const uint16_t base = (uint16_t) outVerts.size();
            outVerts.push_back ({ x0, y0, u0, v0, r, g, b, a, cx0, cy0, cx1, cy1, mode });
            outVerts.push_back ({ x1, y1, u1, v0, r, g, b, a, cx0, cy0, cx1, cy1, mode });
            outVerts.push_back ({ x2, y2, u1, v1, r, g, b, a, cx0, cy0, cx1, cy1, mode });
            outVerts.push_back ({ x3, y3, u0, v1, r, g, b, a, cx0, cy0, cx1, cy1, mode });
            outIndices.push_back (base);
            outIndices.push_back ((uint16_t) (base + 1));
            outIndices.push_back ((uint16_t) (base + 2));
            outIndices.push_back (base);
            outIndices.push_back ((uint16_t) (base + 2));
            outIndices.push_back ((uint16_t) (base + 3));
        };

        for (size_t i = 0; i < clampedCount; ++i)
        {
            const auto& sp = sprites[i];
            if (sp.alpha <= 0.002f)
                continue;

            const float cx0 = sp.hasClip ? sp.clipX0 : 0.0f;
            const float cy0 = sp.hasClip ? sp.clipY0 : 0.0f;
            const float cx1 = sp.hasClip ? sp.clipX1 : -1.0f;
            const float cy1 = sp.hasClip ? sp.clipY1 : -1.0f;

            if (sp.isDot)
            {
                const float half = sp.dotSize * 0.5f;
                const float xL = std::round (sp.cx - half);
                const float yT = std::round (sp.cy - half);
                const float sz = std::round (sp.dotSize);
                const float a  = std::max (0.0f, std::min (1.0f, sp.alpha * 0.85f));
                pushQuad (xL, yT, xL + sz, yT, xL + sz, yT + sz, xL, yT + sz,
                          0.0f, 0.0f, 1.0f, 1.0f,
                          sp.dotR * a, sp.dotG * a, sp.dotB * a, a,
                          cx0, cy0, cx1, cy1, 2.0f);
                continue;
            }

            // 1) Party-mode rainbow radial glow behind the emoji
            if (sp.haloA > 0.002f && sp.drawSize > 0.0f)
            {
                const float rad = sp.drawSize * 0.6f;
                const float a   = std::max (0.0f, std::min (1.0f, sp.haloA));
                pushQuad (sp.cx - rad, sp.cy - rad,
                          sp.cx + rad, sp.cy - rad,
                          sp.cx + rad, sp.cy + rad,
                          sp.cx - rad, sp.cy + rad,
                          -1.0f, -1.0f, 1.0f, 1.0f,
                          sp.haloR * a, sp.haloG * a, sp.haloB * a, a,
                          cx0, cy0, cx1, cy1, 1.0f);
            }

            // 2) Emoji textured quad (rotated + vertical landing squash)
            if (sp.drawSize > 0.0f && sp.u1 > sp.u0 && sp.v1 > sp.v0)
            {
                const float hx = sp.drawSize * 0.5f;
                const float hy = sp.drawSize * 0.5f * sp.scaleY;
                const float cs = std::cos (sp.angleRad);
                const float sn = std::sin (sp.angleRad);

                const float rx = hx * cs;
                const float ry = hx * sn;
                const float ux = -hy * sn;
                const float uy =  hy * cs;

                const float x0 = sp.cx - rx - ux;
                const float y0 = sp.cy - ry - uy;
                const float x1 = sp.cx + rx - ux;
                const float y1 = sp.cy + ry - uy;
                const float x2 = sp.cx + rx + ux;
                const float y2 = sp.cy + ry + uy;
                const float x3 = sp.cx - rx + ux;
                const float y3 = sp.cy - ry + uy;

                const float a = std::max (0.0f, std::min (1.0f, sp.alpha));
                pushQuad (x0, y0, x1, y1, x2, y2, x3, y3,
                          sp.u0, sp.v0, sp.u1, sp.v1,
                          1.0f, 1.0f, 1.0f, a,
                          cx0, cy0, cx1, cy1, 0.0f);
            }
        }
    }

    // ======================================================
    // 4. Target-FPS-Relative Hysteretic Degradation Controller
    //
    //    Fixes:
    //      - 60Hz 1-missed-VBlank cliff (58fps threshold -> 24)
    //      - 59.95Hz / 30fps cap / 32Hz RDP always-dot bug
    //      - Idle MilkDrop/UI spikes pre-poisoning ToyBox
    //      - Permanent lifetime pixel-dot lock-in (now reversible)
    // ======================================================
    class DegradationController
    {
    public:
        static constexpr int    kNormalTarget       = kMaxToySprites; // 400: 0 degraded under normal load
        static constexpr double kMinTargetFps       = 15.0;
        static constexpr double kMaxTargetFps       = 240.0;
        static constexpr double kEmaAlpha           = 0.08;
        static constexpr double kEnterRatioT1       = 0.85;
        static constexpr double kEnterRatioT2       = 0.72;
        static constexpr double kEnterRatioT3       = 0.58;
        static constexpr double kRecoverRatio       = 0.95;           // >= 10% hysteresis above 0.85
        static constexpr double kResetLowDwellRatio = 0.90;
        static constexpr double kDwellTier1Sec      = 1.5;
        static constexpr double kDwellTier2Sec      = 2.5;
        static constexpr double kDwellTier3Sec      = 3.5;
        static constexpr double kRecoverDwellSec    = 1.0;

        DegradationController() = default;

        void reset (double expectedHz = 60.0) noexcept
        {
            expectedFps_ = clampExpected (expectedHz);
            smoothedFps_ = expectedFps_;
            tier_        = 0;
            lowDwellSec_ = 0.0;
            highDwellSec_ = 0.0;
        }

        void setExpectedFps (double expectedHz) noexcept
        {
            const double clamped = clampExpected (expectedHz);
            if (std::abs (clamped - expectedFps_) < 0.05)
                return;

            expectedFps_ = clamped;
            // Re-baseline EMA on explicit cap / monitor-rate change so
            // stepping down (e.g. 60->30fps) does not falsely accumulate low dwell.
            if (smoothedFps_ > expectedFps_ * 1.15 || smoothedFps_ < expectedFps_ * 0.5)
                smoothedFps_ = expectedFps_;
            lowDwellSec_  = 0.0;
            highDwellSec_ = 0.0;
        }

        void setGlBatched (bool glBatched) noexcept { glBatched_ = glBatched; }
        bool isGlBatched() const noexcept           { return glBatched_; }

        void update (double elapsedSec, int activeFreeCount) noexcept
        {
            if (elapsedSec <= 0.0001)
                return;

            const double clampedDt  = std::min (elapsedSec, 0.5);
            const double maxInstFps = expectedFps_ * 1.5;
            const double instFps    = std::max (1.0, std::min (maxInstFps, 1.0 / clampedDt));
            smoothedFps_ += (instFps - smoothedFps_) * kEmaAlpha;

            // Idle safety: unrelated spikes while no emojis are on screen must never
            // pre-poison degradation state or carry dwell into future clicks.
            if (activeFreeCount <= 0)
            {
                lowDwellSec_  = 0.0;
                highDwellSec_ = 0.0;
                tier_         = 0;
                return;
            }

            const double ratio = smoothedFps_ / expectedFps_;

            if (ratio < kEnterRatioT1)
            {
                lowDwellSec_  += clampedDt;
                highDwellSec_  = 0.0;

                int desiredTier = tier_;
                if (ratio < kEnterRatioT3 && lowDwellSec_ >= kDwellTier3Sec)
                    desiredTier = std::max (desiredTier, 3);
                else if (ratio < kEnterRatioT2 && lowDwellSec_ >= kDwellTier2Sec)
                    desiredTier = std::max (desiredTier, 2);
                else if (lowDwellSec_ >= kDwellTier1Sec)
                    desiredTier = std::max (desiredTier, 1);

                tier_ = desiredTier;
            }
            else if (ratio >= kRecoverRatio)
            {
                lowDwellSec_ = 0.0;
                if (tier_ > 0)
                {
                    highDwellSec_ += clampedDt;
                    if (highDwellSec_ >= kRecoverDwellSec)
                    {
                        --tier_;
                        highDwellSec_ = 0.0;
                    }
                }
                else
                {
                    highDwellSec_ = 0.0;
                }
            }
            else
            {
                // Hysteresis deadband [0.85, 0.95): hold current tier without flapping.
                highDwellSec_ = 0.0;
                if (tier_ == 0 && ratio >= kResetLowDwellRatio)
                    lowDwellSec_ = 0.0;
            }
        }

        int targetFullEmojiCount() const noexcept
        {
            switch (tier_)
            {
                case 0:  return kNormalTarget;
                case 1:  return glBatched_ ? 120 : 60;
                case 2:  return glBatched_ ? 48  : 24;
                default: return glBatched_ ? 16  : 8;
            }
        }

        int degradeBudgetPerFrame() const noexcept
        {
            switch (tier_)
            {
                case 0:  return 1;
                case 1:  return 2;
                case 2:  return 4;
                default: return 6;
            }
        }

        int restoreBudgetPerFrame() const noexcept
        {
            return 4;
        }

        int    tier()         const noexcept { return tier_; }
        double smoothedFps()  const noexcept { return smoothedFps_; }
        double expectedFps()  const noexcept { return expectedFps_; }
        double lowDwellSec()  const noexcept { return lowDwellSec_; }
        double highDwellSec() const noexcept { return highDwellSec_; }

    private:
        static double clampExpected (double hz) noexcept
        {
            if (! std::isfinite (hz) || hz <= 0.0)
                return 60.0;
            return std::max (kMinTargetFps, std::min (kMaxTargetFps, hz));
        }

        double expectedFps_  = 60.0;
        double smoothedFps_  = 60.0;
        int    tier_         = 0;
        double lowDwellSec_  = 0.0;
        double highDwellSec_ = 0.0;
        bool   glBatched_    = true;
    };
}
