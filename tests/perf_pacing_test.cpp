// ==========================================================
// perf_pacing_test —— Pure C++17 unit tests (zero JUCE deps)
//
// Covers:
//   1. y2k::pacing::VBlankPacer (60Hz/59.95Hz/32Hz/144Hz/240Hz,
//      outlier-resistant monitor Hz detection, effective rate cap,
//      no negative credit debt, dedup-before-decrement, 1.35x fallback
//      grace including userCap==0, strict monotonic timestamps).
//   2. y2ktoy::DegradationController (60Hz/59.95Hz/1-miss-VBlank/
//      periodic jitter/30fps-cap/32Hz-RDP/120Hz@60fps/idle-safety/
//      sustained drop dwell >=1.5s/hysteresis deadband/reversible recovery).
//   3. y2ktoy::EmojiAtlasLayout + packToySpritesToQuads + isPointExcluded
//      (98x3=294 slots inside 1024x1024 with 1px gutters, zero overlap,
//      quad packing rotation/squash/halo/dot/cap, exclusion rects).
//   4. y2k::ui::regularModuleNeedsRaise (no-op Z-order guard when
//      overlays sit above regular ModulePanels).
//
// Exit 0 + "ALL_OK" on pass; exit 1 on failure.
// ==========================================================

#include "source/ui/VBlankPacer.h"
#include "source/ui/ModuleZOrder.h"
#include "source/ui/MilkdropPresetIndex.h"
#include "source/ui/toys/ToyGlTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int gFailures = 0;

#define CHECK(cond, msg)                                                 \
    do {                                                                 \
        if (! (cond)) {                                                  \
            std::printf ("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++gFailures;                                                 \
        }                                                                \
    } while (0)

void checkNear (double got, double want, double tol, const char* msg)
{
    if (std::abs (got - want) > tol)
    {
        std::printf ("  FAIL: %s (got %.4f, want %.4f, tol %.4f)\n", msg, got, want, tol);
        ++gFailures;
    }
}

// ----------------------------------------------------------
// 1. VBlankPacer tests
// ----------------------------------------------------------
void testVBlankPacerMonitorDetectionAndOutliers()
{
    std::printf ("[VBlankPacer: monitor detection & effective target]\n");
    y2k::pacing::VBlankPacer p;

    // 60.00 Hz with a single 33.33ms dropped VBlank outlier -> median stays 60.0 Hz
    double t = 1000.0;
    for (int i = 0; i < 12; ++i)
    {
        const double dt = (i == 6) ? 33.333 : 16.6667;
        t += dt;
        p.recordVBlankIntervalMs (dt);
    }
    checkNear (p.detectedMonitorHz(), 60.0, 0.1, "60Hz median ignores 33.33ms outlier");
    checkNear (p.effectiveTargetHz (0),   60.0, 0.1, "unlimited on 60Hz -> 60Hz");
    checkNear (p.effectiveTargetHz (60),  60.0, 0.1, "60 cap on 60Hz -> 60Hz");
    checkNear (p.effectiveTargetHz (120), 60.0, 0.1, "120 cap on 60Hz -> clamped to 60Hz (no double-dispatch)");
    checkNear (p.effectiveTargetHz (30),  30.0, 0.1, "30 cap on 60Hz -> 30Hz");

    // 59.95 Hz physical monitor (HP E273q: 1000/59.9506 = 16.6804ms)
    p.reset();
    for (int i = 0; i < 9; ++i)
        p.recordVBlankIntervalMs (16.6804);
    checkNear (p.detectedMonitorHz(), 59.95, 0.05, "59.95Hz monitor detected accurately");
    checkNear (p.effectiveTargetHz (60), 59.95, 0.05, "60 cap on 59.95Hz -> 59.95Hz");

    // 32.00 Hz RDP virtual monitor
    p.reset();
    for (int i = 0; i < 9; ++i)
        p.recordVBlankIntervalMs (31.25);
    checkNear (p.detectedMonitorHz(), 32.0, 0.05, "32Hz RDP detected accurately");
    checkNear (p.effectiveTargetHz (0), 32.0, 0.05, "unlimited on 32Hz RDP -> 32Hz");
    checkNear (p.effectiveTargetHz (60), 32.0, 0.05, "60 cap on 32Hz RDP -> 32Hz");

    // 144 Hz monitor
    p.reset();
    for (int i = 0; i < 9; ++i)
        p.recordVBlankIntervalMs (1000.0 / 144.0);
    checkNear (p.detectedMonitorHz(), 144.0, 0.2, "144Hz monitor detected");
    checkNear (p.effectiveTargetHz (60), 60.0, 0.1, "60 cap on 144Hz -> 60Hz");
    checkNear (p.effectiveTargetHz (0), 144.0, 0.2, "unlimited on 144Hz -> 144Hz");
}

void testVBlankPacerNativeFastPathAndCreditClamp()
{
    std::printf ("[VBlankPacer: native fast-path, credit clamp, dedup]\n");

    // 59.95 Hz monitor with 60fps cap and +/-0.5ms VBlank jitter:
    // MUST dispatch 100% of VBlanks (zero skipped frames from negative credit debt!)
    {
        y2k::pacing::VBlankPacer p;
        double now = 1000.0;
        int dispatched = 0;
        const int total = 300;
        for (int i = 0; i < total; ++i)
        {
            const double jitter = ((i % 3) - 1) * 0.45; // -0.45, 0, +0.45 ms
            const double dt = (i == 0) ? 0.0 : (16.6804 + jitter);
            now += dt;
            if (p.shouldDispatchOnVBlank (60, dt, now))
                ++dispatched;
            CHECK (p.creditMs() >= 0.0, "creditMs never goes negative on 59.95Hz");
        }
        CHECK (dispatched == total, "59.95Hz @ 60fps cap dispatches 100% of VBlanks without jitter skip");
    }

    // Fractional cap: 60fps on 144Hz & 240Hz -> exact ~60 dispatches/sec, credit >= 0 always
    for (double hz : { 120.0, 144.0, 240.0 })
    {
        y2k::pacing::VBlankPacer p;
        const double stepMs = 1000.0 / hz;
        double now = 1000.0;
        // Prime monitor Hz estimate
        for (int i = 0; i < 9; ++i) p.recordVBlankIntervalMs (stepMs);

        int dispatched = 0;
        const int vblanks = (int) std::lround (hz * 5.0); // 5 seconds
        for (int i = 0; i < vblanks; ++i)
        {
            const double jitter = ((i % 5) - 2) * 0.15;
            const double dt = stepMs + jitter;
            now += dt;
            if (p.shouldDispatchOnVBlank (60, dt, now))
                ++dispatched;
            CHECK (p.creditMs() >= 0.0, "fractional cap creditMs never goes negative");
        }
        const double actualHz = (double) dispatched / 5.0;
        checkNear (actualHz, 60.0, 1.0, "high-Hz @ 60fps cap achieves ~60 dispatches/sec");
    }

    // Dedup BEFORE credit decrement: near-simultaneous duplicate call (<1ms) must not burn credit
    {
        y2k::pacing::VBlankPacer p;
        for (int i = 0; i < 9; ++i) p.recordVBlankIntervalMs (16.6667);
        double now = 1000.0;
        // Dispatch frame 0 at 30fps cap (interval 33.33ms)
        CHECK (p.shouldDispatchOnVBlank (30, 0.0, now), "first dispatch succeeds");
        // 16.67ms later -> accumulates 16.67ms credit, does not dispatch yet
        now += 16.6667;
        CHECK (! p.shouldDispatchOnVBlank (30, 16.6667, now), "half interval accumulates credit");
        const double creditBefore = p.creditMs();
        checkNear (creditBefore, 16.6667, 0.05, "credit accumulated ~16.67ms");

        // Now dispatch at 33.33ms
        now += 16.6667;
        CHECK (p.shouldDispatchOnVBlank (30, 16.6667, now), "full 33.33ms dispatches");
        const double creditAfterDispatch = p.creditMs();
        // Spurious duplicate 0.4ms later must be deduped without touching credit
        CHECK (! p.shouldDispatchOnVBlank (30, 0.4, now + 0.4), "0.4ms duplicate deduped");
        checkNear (p.creditMs(), creditAfterDispatch, 1e-6, "dedup did not alter credit");
    }
}

void testVBlankPacerFallbackAndMonotonicTime()
{
    std::printf ("[VBlankPacer: 1.35x fallback grace & monotonic timestamps]\n");
    y2k::pacing::VBlankPacer p;
    for (int i = 0; i < 9; ++i) p.recordVBlankIntervalMs (16.6667);

    // Fallback timer interval stays bounded in [8, 16]
    CHECK (p.recommendedFallbackTimerMs (60) >= 8 && p.recommendedFallbackTimerMs (60) <= 16, "60fps fallback timer in [8,16]ms");
    CHECK (p.recommendedFallbackTimerMs (0)  >= 8 && p.recommendedFallbackTimerMs (0)  <= 16, "unlimited fallback timer in [8,16]ms");
    CHECK (p.recommendedFallbackTimerMs (120) == p.recommendedFallbackTimerMs (60), "120 cap on 60Hz uses same fallback timer as 60 cap");

    // 1.35x grace on 60Hz (22.5ms): normal 16.67..21.5ms must NOT fire fallback (even when userCap == 0!)
    double now = 5000.0;
    CHECK (p.shouldDispatchOnVBlank (0, 0.0, now), "initial unlimited VBlank dispatch");
    CHECK (! p.shouldDispatchOnFallback (0, now + 16.67), "16.67ms does not trigger fallback (cap=0)");
    CHECK (! p.shouldDispatchOnFallback (0, now + 21.50), "21.50ms (<1.35x) does not trigger fallback (cap=0)");
    CHECK (p.shouldDispatchOnFallback (0, now + 23.00), "23.00ms (>=1.35x) triggers fallback (cap=0)");
    checkNear (p.creditMs(), 0.0, 1e-9, "fallback clears credit");

    // Strict monotonic timestamps across VBlank <-> fallback epoch differences
    p.reset();
    double t1 = p.nextMonotonicTimeSec (true,  100.000, 5000.0);
    double t2 = p.nextMonotonicTimeSec (true,  100.016, 5016.0);
    double t3 = p.nextMonotonicTimeSec (false, 0.0,     5040.0); // fallback advances in VBlank epoch!
    double t4 = p.nextMonotonicTimeSec (true,  100.030, 5048.0); // older VBlank clamped strictly > t3
    CHECK (t2 > t1, "t2 > t1");
    CHECK (t3 > t2 && (t3 - t2) < 0.05, "fallback stays in VBlank epoch without huge jump");
    CHECK (t4 > t3, "non-monotonic VBlank clamped strictly increasing");
}

// ----------------------------------------------------------
// 2. DegradationController tests
// ----------------------------------------------------------
void testDegradationController()
{
    std::printf ("[DegradationController: 60Hz/59.95Hz/1-miss/30fps/32Hz/dwell/hysteresis/recovery]\n");
    using DC = y2ktoy::DegradationController;

    // A. 60.00Hz steady -> tier 0, target 400
    {
        DC dc; dc.reset (60.0);
        for (int i = 0; i < 600; ++i) dc.update (1.0 / 60.0, 200);
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "60.00Hz steady stays normal (400)");
    }

    // B. 59.95Hz steady (HP E273q) -> tier 0, target 400
    {
        DC dc; dc.reset (60.0);
        for (int i = 0; i < 600; ++i) dc.update (1.0 / 59.9506, 200);
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "59.95Hz steady stays normal (400)");
    }

    // C. Single missed 60Hz VBlank (33.33ms = 30fps) -> NEVER degrades (kills old 58fps cliff!)
    {
        DC dc; dc.reset (60.0);
        for (int i = 0; i < 60; ++i) dc.update (1.0 / 60.0, 200);
        dc.update (1.0 / 30.0, 200);
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "single 60Hz missed VBlank does not degrade");
        for (int i = 0; i < 60; ++i) dc.update (1.0 / 60.0, 200);
        CHECK (dc.tier() == 0 && dc.lowDwellSec() == 0.0, "recovers cleanly after single miss");
    }

    // D. Periodic 60Hz jitter (1 miss every 2.0s for 20s) -> NEVER degrades
    {
        DC dc; dc.reset (60.0);
        for (int sec = 0; sec < 10; ++sec)
        {
            for (int f = 0; f < 119; ++f) dc.update (1.0 / 60.0, 250);
            dc.update (1.0 / 30.0, 250);
        }
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "periodic single misses every 2s never degrade");
    }

    // E. 30fps cap steady -> NEVER degrades (fixes old 30fps always-dot bug!)
    {
        DC dc; dc.reset (60.0);
        dc.setExpectedFps (30.0);
        for (int i = 0; i < 300; ++i) dc.update (1.0 / 30.0, 200);
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "30fps cap steady stays normal (400)");
    }

    // F. 32Hz RDP steady -> NEVER degrades
    {
        DC dc; dc.reset (32.0);
        for (int i = 0; i < 300; ++i) dc.update (1.0 / 32.0, 200);
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "32Hz RDP steady stays normal (400)");
    }

    // G. 120Hz @ 60fps cap with occasional 25ms slip + 8.33ms catch-up -> stays normal
    {
        DC dc; dc.reset (60.0);
        for (int i = 0; i < 300; ++i)
        {
            if (i % 30 == 15)      dc.update (0.025, 200);
            else if (i % 30 == 16) dc.update (1.0 / 120.0, 200);
            else                   dc.update (1.0 / 60.0, 200);
        }
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "120Hz@60fps slip+catchup stays normal");
    }

    // H. Idle safety: 10fps stall while activeFreeCount==0 must not pre-poison degradation
    {
        DC dc; dc.reset (60.0);
        for (int i = 0; i < 50; ++i) dc.update (0.100, 0);
        CHECK (dc.tier() == 0 && dc.lowDwellSec() == 0.0, "idle stalls do not accumulate dwell or degrade");
    }

    // I. Sustained genuine slowdown (40fps = 66.7% of 60Hz):
    //    No trigger before 1.5s dwell; smooth step to T1 (>=1.5s) and T2 (>=2.5s);
    //    deadband hysteresis at 54fps (90%) holds tier without flapping;
    //    recovery at 60fps (>=95% for >=1.0s) steps back to T0 (400).
    {
        DC dc; dc.reset (60.0);
        dc.setGlBatched (true);

        // First 1.2s at 40fps (48 frames) -> must still be tier 0
        for (int i = 0; i < 48; ++i) dc.update (1.0 / 40.0, 200);
        CHECK (dc.tier() == 0, "does not degrade before 1.5s sustained dwell");

        // Run another 0.8s (total 2.0s at 40fps) -> EMA < 51 (<0.85*60) & dwell >= 1.5s -> tier 1 (120 GL)
        for (int i = 0; i < 32; ++i) dc.update (1.0 / 40.0, 200);
        CHECK (dc.tier() == 1 && dc.targetFullEmojiCount() == 120, "sustained 40fps enters Tier 1 (target 120 GL)");

        // Verify CPU fallback target is lower than GL batched
        dc.setGlBatched (false);
        CHECK (dc.targetFullEmojiCount() == 60, "CPU fallback Tier 1 target is 60");
        dc.setGlBatched (true);

        // Hold in hysteresis deadband (~54fps = 90%, between 85% enter & 95% recover) for 4s
        for (int i = 0; i < 216; ++i) dc.update (1.0 / 54.0, 200);
        CHECK (dc.tier() == 1, "hysteresis deadband [85%,95%) holds Tier 1 without flapping");

        // Return to 60fps (>=95%) for 2.5s -> EMA climbs >= 57.0 then holds >=1.0s -> recovers to Tier 0 (400)
        for (int i = 0; i < 150; ++i) dc.update (1.0 / 60.0, 200);
        CHECK (dc.tier() == 0 && dc.targetFullEmojiCount() == DC::kNormalTarget, "sustained 60fps restores Tier 0 (400)");
        CHECK (dc.restoreBudgetPerFrame() > 0, "restoreBudgetPerFrame > 0 for reversible particle recovery");

        // Deep slowdown (25fps = 41.7% < 58%) for 5s -> reaches Tier 3 (16 GL)
        for (int i = 0; i < 125; ++i) dc.update (1.0 / 25.0, 200);
        CHECK (dc.tier() == 3 && dc.targetFullEmojiCount() == 16, "sustained 25fps reaches Tier 3 (16 GL)");
    }
}

// ----------------------------------------------------------
// 3. EmojiAtlasLayout, Quad Packing & Exclusion Rect tests
// ----------------------------------------------------------
void testAtlasPackingAndQuads()
{
    std::printf ("[EmojiAtlasLayout & packToySpritesToQuads & ExclusionRect]\n");
    using Layout = y2ktoy::EmojiAtlasLayout;

    struct Box { int x0, y0, x1, y1; };
    std::vector<Box> allCells;
    allCells.reserve ((size_t) (Layout::kMaxEmojiSlots * Layout::kNumSizeTiers));

    for (int s = 0; s < Layout::kNumSizeTiers; ++s)
    {
        const int expectSz = Layout::imageSizeForTier (s);
        for (int e = 0; e < Layout::kMaxEmojiSlots; ++e)
        {
            const auto slot = Layout::slotFor (e, s);
            CHECK (slot.valid, "all 98x3 slots are valid");
            CHECK (slot.w == expectSz && slot.h == expectSz, "slot matches tier image size");
            CHECK (slot.x >= Layout::kGutterPx && slot.y >= Layout::kGutterPx, "slot preserves leading gutter");
            CHECK (slot.x + slot.w + Layout::kGutterPx <= Layout::kAtlasWidth, "slot+gutter fits within 1024 width");
            CHECK (slot.y + slot.h + Layout::kGutterPx <= Layout::kAtlasHeight, "slot+gutter fits within 1024 height");
            CHECK (slot.u0 > 0.0f && slot.u1 > slot.u0 && slot.u1 < 1.0f, "normalized U valid");
            CHECK (slot.v0 > 0.0f && slot.v1 > slot.v0 && slot.v1 < 1.0f, "normalized V valid");

            allCells.push_back ({ slot.x - Layout::kGutterPx, slot.y - Layout::kGutterPx,
                                  slot.x + slot.w + Layout::kGutterPx, slot.y + slot.h + Layout::kGutterPx });
        }
    }

    // Pairwise zero-overlap check across all 294 padded cells
    bool anyOverlap = false;
    for (size_t i = 0; i < allCells.size() && ! anyOverlap; ++i)
    {
        for (size_t j = i + 1; j < allCells.size(); ++j)
        {
            const auto& a = allCells[i];
            const auto& b = allCells[j];
            if (a.x0 < b.x1 && a.x1 > b.x0 && a.y0 < b.y1 && a.y1 > b.y0)
            {
                anyOverlap = true;
                break;
            }
        }
    }
    CHECK (! anyOverlap, "zero overlapping padded cells across all 294 atlas slots");
    CHECK (! Layout::slotFor (-1, 0).valid, "negative emoji index invalid");
    CHECK (! Layout::slotFor (Layout::kMaxEmojiSlots, 0).valid, "out-of-range emoji index invalid");
    CHECK (! Layout::slotFor (0, 3).valid, "out-of-range tier invalid");

    // Quad packing: Party emoji (halo + rotated squashed quad) + dot quad + 400 cap
    std::vector<y2ktoy::GlToyVertex> verts;
    std::vector<uint16_t> indices;

    y2ktoy::GlToySprite s0 {};
    const auto sl = Layout::slotFor (1, 2);
    s0.cx = 320.0f; s0.cy = 240.0f; s0.drawSize = 34.0f;
    s0.angleRad = 0.75f; s0.scaleY = 0.72f; s0.alpha = 0.9f;
    s0.u0 = sl.u0; s0.v0 = sl.v0; s0.u1 = sl.u1; s0.v1 = sl.v1;
    s0.haloR = 1.0f; s0.haloG = 0.2f; s0.haloB = 0.8f; s0.haloA = 0.25f;

    y2ktoy::GlToySprite s1 {};
    s1.cx = 100.0f; s1.cy = 120.0f; s1.dotSize = 6.0f;
    s1.alpha = 1.0f; s1.dotR = 1.0f; s1.dotG = 0.5f; s1.dotB = 0.0f; s1.isDot = 1;

    y2ktoy::GlToySprite pair[2] = { s0, s1 };
    y2ktoy::packToySpritesToQuads (pair, 2, verts, indices);
    CHECK (verts.size() == 12 && indices.size() == 18, "Party emoji (halo+glyph) + dot emits 3 quads (12v/18i)");
    CHECK (verts[0].mode == 1.0f && verts[4].mode == 0.0f && verts[8].mode == 2.0f, "quad modes: halo(1), emoji(0), dot(2)");

    // Check rotated+squashed emoji quad centroid == (cx, cy)
    const float midX = 0.25f * (verts[4].x + verts[5].x + verts[6].x + verts[7].x);
    const float midY = 0.25f * (verts[4].y + verts[5].y + verts[6].y + verts[7].y);
    checkNear (midX, s0.cx, 1e-3, "rotated+squashed quad centroid X == cx");
    checkNear (midY, s0.cy, 1e-3, "rotated+squashed quad centroid Y == cy");
    checkNear (verts[0].r, s0.haloR * s0.haloA, 1e-4, "halo vertex color premultiplied");
    checkNear (verts[8].a, 0.85f, 1e-4, "dot alpha scaled by 0.85");

    // Hard cap clamp at kMaxToySprites (400)
    std::vector<y2ktoy::GlToySprite> burst (500, s0);
    y2ktoy::packToySpritesToQuads (burst.data(), burst.size(), verts, indices);
    CHECK (verts.size() == (size_t) y2ktoy::kMaxToySprites * 8, "packToySpritesToQuads clamps to kMaxToySprites (400)");

    // Exclusion rects
    y2ktoy::ExclusionRect ex[2] = { { 100.0f, 50.0f, 400.0f, 300.0f }, { 800.0f, 0.0f, 1000.0f, 40.0f } };
    CHECK (y2ktoy::isPointExcluded (250.0f, 150.0f, ex, 2), "point inside drawer exclusion rect excluded");
    CHECK (y2ktoy::isPointExcluded (900.0f, 20.0f, ex, 2), "point inside toolbar exclusion rect excluded");
    CHECK (! y2ktoy::isPointExcluded (500.0f, 500.0f, ex, 2), "point outside exclusions allowed");
    CHECK (! y2ktoy::isPointExcluded (250.0f, 150.0f, ex, 0), "count==0 never excludes");
}

// ----------------------------------------------------------
// 4. ModuleZOrder no-op toFront guard tests
// ----------------------------------------------------------
void testModuleZOrderGuard()
{
    std::printf ("[ModuleZOrder: regularModuleNeedsRaise]\n");
    using R = y2k::ui::WorkspaceChildRole;

    // [Mod0, Mod1, Mod2, Perler, VirtuPet, HideBtn]
    const R roles[] = {
        R::regularModule, R::regularModule, R::regularModule,
        R::overlayOrTopmost, R::overlayOrTopmost, R::overlayOrTopmost
    };
    CHECK (y2k::ui::regularModuleNeedsRaise (roles, 6, 0),  "bottom module needs raise");
    CHECK (y2k::ui::regularModuleNeedsRaise (roles, 6, 1),  "middle module needs raise");
    CHECK (! y2k::ui::regularModuleNeedsRaise (roles, 6, 2), "topmost regular module below overlays does NOT need raise");
    CHECK (! y2k::ui::regularModuleNeedsRaise (roles, 6, 3), "overlay role returns false");
    CHECK (! y2k::ui::regularModuleNeedsRaise (roles, 6, 9), "out-of-bounds returns false");
    CHECK (! y2k::ui::regularModuleNeedsRaise (nullptr, 6, 0), "nullptr returns false");

    // Interleaved other child between modules
    const R interleaved[] = { R::regularModule, R::other, R::regularModule, R::overlayOrTopmost };
    CHECK (y2k::ui::regularModuleNeedsRaise (interleaved, 4, 0),   "module below other+module needs raise");
    CHECK (! y2k::ui::regularModuleNeedsRaise (interleaved, 4, 2), "topmost module after other does NOT need raise");
}

// ----------------------------------------------------------
// 5. MilkdropPresetIndex async scan reconciliation tests
// ----------------------------------------------------------
void testMilkdropPresetIndex()
{
    std::printf ("[MilkdropPresetIndex: async scan reconciliation]\n");
    using namespace y2k::milkdrop;

    // Case & slash normalization
    CHECK (asciiToLower ('A') == 'a' && asciiToLower ('Z') == 'z' && asciiToLower ('9') == '9',
           "asciiToLower handles ASCII A-Z and preserves non-alpha");
    CHECK (normalizePathChar ('\\') == '/' && normalizePathChar ('C') == 'c' && normalizePathChar ('/') == '/',
           "normalizePathChar maps backslash to slash and lowercases");
    CHECK (pathsEqualIgnoreCase ("C:\\Users\\Test\\0001_Royal.MILK", "c:/users/test/0001_royal.milk"),
           "pathsEqualIgnoreCase matches Windows vs POSIX slashes and mixed case");
    CHECK (! pathsEqualIgnoreCase ("C:/a/0001.milk", "C:/a/0002.milk"),
           "pathsEqualIgnoreCase rejects distinct paths");
    CHECK (! pathsEqualIgnoreCase ("C:/a/0001.milk", "C:/a/0001.milk2"),
           "pathsEqualIgnoreCase rejects different lengths");

    const std::vector<std::string> emptyList;
    const std::vector<std::string> sorted = {
        "C:\\Presets\\0001_Alpha.milk",
        "C:\\Presets\\0002_Beta.milk",
        "C:\\Presets\\0003_Gamma.milk",
        "C:\\Presets\\0004_Delta.milk"
    };

    // findPresetPathIndex
    CHECK (findPresetPathIndex (emptyList, "C:/Presets/0001_Alpha.milk") == -1, "empty list returns -1");
    CHECK (findPresetPathIndex (sorted, "") == -1, "empty target returns -1");
    CHECK (findPresetPathIndex (sorted, "c:/presets/0003_gamma.MILK") == 2, "finds normalized match at index 2");
    CHECK (findPresetPathIndex (sorted, "C:\\Presets\\9999_Missing.milk") == -1, "missing preset returns -1");

    // wrapPresetIndex
    CHECK (wrapPresetIndex (0, 1, 0) == -1, "wrapPresetIndex count<=0 returns -1");
    CHECK (wrapPresetIndex (3, 1, 4) == 0, "positive wrap from last -> 0");
    CHECK (wrapPresetIndex (0, -1, 4) == 3, "negative wrap from 0 -> last");
    CHECK (wrapPresetIndex (1, -10, 4) == 3, "large negative delta wraps cleanly");
    CHECK (wrapPresetIndex (2, 100, 4) == 2, "large positive multiple wraps cleanly");
    CHECK (wrapPresetIndex (-1, 1, 4) == 1, "invalid current defaults base to 0");

    // reconcileLoadedPresetIndex: empty list
    {
        const auto r = reconcileLoadedPresetIndex (emptyList, "C:/Presets/0001_Alpha.milk", 0, 1, true, 7);
        CHECK (r.index == -1 && ! r.shouldReload, "empty list yields {-1, false}");
    }

    // Saved / fast-path preset matched, no pending user requests -> zero redundant reload!
    {
        const auto r = reconcileLoadedPresetIndex (sorted, "c:/presets/0002_beta.milk", -1, 0, false, 0);
        CHECK (r.index == 1 && ! r.shouldReload, "matched loaded preset with no pending action does NOT reload");
    }

    // Loaded path missing from new library (e.g. switched to Like folder), no pending -> load index 0
    {
        const auto r = reconcileLoadedPresetIndex (sorted, "C:\\Other\\Deleted.milk", -1, 0, false, 0);
        CHECK (r.index == 0 && r.shouldReload, "unmatched loaded path falls back to 0 with reload");
    }

    // Pending jump index
    {
        const auto rDiff = reconcileLoadedPresetIndex (sorted, sorted[1], 3, 0, false, 0);
        CHECK (rDiff.index == 3 && rDiff.shouldReload, "pendingJumpIndex != current reloads target");
        const auto rSame = reconcileLoadedPresetIndex (sorted, sorted[2], 2, 0, false, 0);
        CHECK (rSame.index == 2 && ! rSame.shouldReload, "pendingJumpIndex == current skips redundant reload");
        const auto rOob = reconcileLoadedPresetIndex (sorted, sorted[1], 99, 1, false, 0);
        CHECK (rOob.index == 2 && rOob.shouldReload, "out-of-range pendingJumpIndex falls through to pendingDelta");
    }

    // Pending delta (Next/Prev clicks during <80ms scan)
    {
        const auto rNext = reconcileLoadedPresetIndex (sorted, sorted[3], -1, 1, false, 0);
        CHECK (rNext.index == 0 && rNext.shouldReload, "pendingDelta +1 wraps from last to 0");
        const auto rPrev = reconcileLoadedPresetIndex (sorted, sorted[0], -1, -1, false, 0);
        CHECK (rPrev.index == 3 && rPrev.shouldReload, "pendingDelta -1 wraps from 0 to last");
        const auto rFull = reconcileLoadedPresetIndex (sorted, sorted[1], -1, 4, false, 0);
        CHECK (rFull.index == 1 && ! rFull.shouldReload, "pendingDelta full cycle landing on current skips reload");
        const auto rMiss = reconcileLoadedPresetIndex (sorted, "missing.milk", -1, 2, false, 0);
        CHECK (rMiss.index == 2 && rMiss.shouldReload, "pendingDelta with unmatched loaded path forces reload");
    }

    // Pending random (single & multi-preset)
    {
        const std::vector<std::string> single = { "C:\\Presets\\Only.milk" };
        const auto rSingleSame = reconcileLoadedPresetIndex (single, single[0], -1, 0, true, 42);
        CHECK (rSingleSame.index == 0 && ! rSingleSame.shouldReload, "random on 1-preset already loaded skips reload");
        const auto rSingleDiff = reconcileLoadedPresetIndex (single, "other.milk", -1, 0, true, 42);
        CHECK (rSingleDiff.index == 0 && rSingleDiff.shouldReload, "random on 1-preset unmatched reloads 0");

        bool anyRepeat = false;
        for (std::size_t seed = 0; seed < 16; ++seed)
        {
            const auto r = reconcileLoadedPresetIndex (sorted, sorted[2], -1, 0, true, seed);
            if (r.index < 0 || r.index >= 4 || r.index == 2 || ! r.shouldReload)
            {
                anyRepeat = true;
                break;
            }
        }
        CHECK (! anyRepeat, "pendingRandom across all seeds never repeats currentIdx and sets shouldReload");
    }

    // Precedence: pendingJumpIndex > pendingRandom > pendingDelta
    {
        const auto rJumpWins = reconcileLoadedPresetIndex (sorted, sorted[0], 3, 1, true, 5);
        CHECK (rJumpWins.index == 3 && rJumpWins.shouldReload, "pendingJumpIndex takes priority over random/delta");
        const auto rRandWins = reconcileLoadedPresetIndex (sorted, sorted[0], -1, 1, true, 2);
        CHECK (rRandWins.index == 3 && rRandWins.shouldReload, "pendingRandom takes priority over pendingDelta");
    }
}

} // namespace

int main()
{
    testVBlankPacerMonitorDetectionAndOutliers();
    testVBlankPacerNativeFastPathAndCreditClamp();
    testVBlankPacerFallbackAndMonotonicTime();
    testDegradationController();
    testAtlasPackingAndQuads();
    testModuleZOrderGuard();
    testMilkdropPresetIndex();

    if (gFailures == 0)
    {
        std::printf ("ALL_OK\n");
        return 0;
    }
    std::printf ("%d FAILURES\n", gFailures);
    return 1;
}
