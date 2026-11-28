#include "source/ui/toys/ToyKit.h"
#include "source/i18n/Localisation.h"
#include <array>
#include <cmath>

// ==========================================================
// ToyModuleFx —— 全模块统一荧光矢量物理玩法（v3.5，纯 L1 叠加，不碰 GL）
//
//   设计语言（以示波器双态荧光电子为标杆）：
//     · 极细 1~2px 透明荧光矢量线 + 渐变余辉尾迹 + 微小 2px 磷光火花 + 亮核（PinkXP::hl）；
//     · 全部色彩跟随当前 PinkXP 主题（PinkXP::sel / accent2 / hl / 模块真实量值色）；
//     · 零不透明遮挡块、零冗余弹窗、零破坏读数可读性的大尺寸贴纸；
//     · 支持按住慢放 / 冻结的 5 个模块（Oscilloscope / Spectrum / Waveform /
//       Spectrogram / StereoMap）在按住期间自动切入子弹时间 / 静止并柔淡退让，
//       松手瞬间恢复全速全亮，绝不干扰用户观察。
// ==========================================================

namespace
{
    constexpr float kTwoPi = juce::MathConstants<float>::twoPi;
    constexpr float kPi    = juce::MathConstants<float>::pi;

    struct MicroSpark
    {
        float x = 0.0f, y = 0.0f;
        float vx = 0.0f, vy = 0.0f;
        float life = 0.0f;
        float maxLife = 0.30f;
        bool  useAccent2 = false;
    };

    inline float clampFinite (float v, float lo, float hi, float fallback) noexcept
    {
        if (! std::isfinite (v))
            return fallback;
        if (hi < lo)
            return lo;
        return juce::jlimit (lo, hi, v);
    }

    inline void drawGlowDot (juce::Graphics& g, float x, float y,
                             juce::Colour auraCol, float alpha, float haloR = 3.8f, float coreR = 1.5f)
    {
        if (alpha <= 0.01f)
            return;
        g.setColour (auraCol.withAlpha (0.36f * alpha));
        g.fillEllipse (x - haloR, y - haloR, haloR * 2.0f, haloR * 2.0f);
        g.setColour (PinkXP::hl.withAlpha (0.95f * alpha));
        g.fillEllipse (x - coreR, y - coreR, coreR * 2.0f, coreR * 2.0f);
    }

    inline void drawPulseRing (juce::Graphics& g, float cx, float cy, float r,
                               juce::Colour col, float alpha, float stroke = 1.1f)
    {
        if (alpha <= 0.01f || r <= 0.5f)
            return;
        g.setColour (col.withAlpha (alpha));
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, stroke);
    }

    struct BoundsAccum
    {
        float minX = 1.0e9f, minY = 1.0e9f;
        float maxX = -1.0e9f, maxY = -1.0e9f;
        bool  hasAny = false;

        void includePoint (float x, float y, float pad = 0.0f) noexcept
        {
            if (! std::isfinite (x) || ! std::isfinite (y))
                return;
            minX = juce::jmin (minX, x - pad);
            minY = juce::jmin (minY, y - pad);
            maxX = juce::jmax (maxX, x + pad);
            maxY = juce::jmax (maxY, y + pad);
            hasAny = true;
        }

        void includeRect (float x0, float y0, float x1, float y1, float pad = 0.0f) noexcept
        {
            includePoint (x0, y0, pad);
            includePoint (x1, y1, pad);
        }

        juce::Rectangle<float> finish (const juce::Rectangle<int>& clipRect, float extraPad = 8.0f) const noexcept
        {
            if (! hasAny)
                return {};
            const juce::Rectangle<float> raw (minX - extraPad, minY - extraPad,
                                              (maxX - minX) + extraPad * 2.0f,
                                              (maxY - minY) + extraPad * 2.0f);
            return raw.getIntersection (clipRect.toFloat());
        }
    };

    // 频率 → 音名（A4=440Hz，纯函数换算）
    juce::String freqToNoteName (float freq)
    {
        if (freq <= 0.0f || freq > 20000.0f)
            return {};
        const float midi = 69.0f + 12.0f * std::log2 (freq / 440.0f);
        const int n = juce::roundToInt (midi);
        static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        const int idx = n % 12;
        if (idx < 0) return {};
        const int oct = n / 12 - 1;
        const int cents = juce::roundToInt ((midi - (float) n) * 100.0f);
        juce::String s;
        s << names[idx] << oct;
        if (cents != 0) s << (cents > 0 ? " +" : " ") << cents << "c";
        s << " " << juce::roundToInt (freq) << "Hz";
        return s;
    }

    // ------------------------------------------------------
    // 1) Spectrum：拨弦驻波 + 真实泛音节点（2f/3f/4f）+ 沿真实频谱轮廓冲浪的双微火花
    // ------------------------------------------------------
    struct SpectrumPluckActor : public ToyActor
    {
        static constexpr int kProfileBins  = 96;
        static constexpr int kTrailLen     = 12;
        static constexpr int kMaxHarmonics = 3;
        static constexpr int kMaxSparks    = 10;

        struct Surfer
        {
            float x = 0.0f, y = 0.0f, vx = 0.0f;
            bool  finished = false;
            bool  useAccent2 = false;
            std::array<juce::Point<float>, kTrailLen> trail {};
            int   trailCount = 0;
        };

        juce::Rectangle<int>   canvas;
        juce::Rectangle<float> inner;
        float                  pluckX = 0.0f;
        float                  pluckCurveY = 0.0f;
        std::array<float, kProfileBins> curveY {};
        std::array<juce::Point<float>, kMaxHarmonics> harmonics {};
        int                    harmonicCount = 0;
        std::array<Surfer, 2>  surfers {};
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        std::function<bool()>  isSlowMo;
        float                  slowFade = 1.0f;

        SpectrumPluckActor (juce::Point<float> clickPos,
                            juce::Rectangle<int> canvasBounds,
                            const std::vector<float>& profileY,
                            const std::vector<juce::Point<float>>& harmPts,
                            std::function<bool()> slowMoQuery)
            : canvas (canvasBounds),
              inner (canvasBounds.reduced (3).toFloat()),
              isSlowMo (std::move (slowMoQuery))
        {
            ttl = 1.55f;
            if (inner.getWidth() < 8.0f || inner.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }

            const float fallbackY = inner.getCentreY();
            const int srcN = (int) profileY.size();
            for (int i = 0; i < kProfileBins; ++i)
            {
                float y = fallbackY;
                if (srcN > 1)
                {
                    const float f = (float) i / (float) (kProfileBins - 1) * (float) (srcN - 1);
                    const int i0 = juce::jlimit (0, srcN - 1, (int) f);
                    const int i1 = juce::jmin (srcN - 1, i0 + 1);
                    const float a = clampFinite (profileY[(size_t) i0], inner.getY(), inner.getBottom(), fallbackY);
                    const float b = clampFinite (profileY[(size_t) i1], inner.getY(), inner.getBottom(), fallbackY);
                    y = a + (b - a) * (f - (float) i0);
                }
                else if (srcN == 1)
                {
                    y = clampFinite (profileY[0], inner.getY(), inner.getBottom(), fallbackY);
                }
                curveY[(size_t) i] = y;
            }

            pluckX      = clampFinite (clickPos.x, inner.getX() + 2.0f, inner.getRight() - 2.0f, inner.getCentreX());
            pluckCurveY = sampleCurveY (pluckX);

            harmonicCount = 0;
            for (const auto& hp : harmPts)
            {
                if (harmonicCount >= kMaxHarmonics) break;
                if (hp.x > inner.getX() + 2.0f && hp.x < inner.getRight() - 2.0f)
                {
                    const float hy = clampFinite (hp.y, inner.getY() + 2.0f, inner.getBottom() - 2.0f, sampleCurveY (hp.x));
                    harmonics[(size_t) harmonicCount++] = { hp.x, hy };
                }
            }

            const float spd = juce::jlimit (150.0f, 430.0f, inner.getWidth() * 0.85f);
            for (int i = 0; i < 2; ++i)
            {
                auto& s = surfers[(size_t) i];
                s.x = pluckX;
                s.y = pluckCurveY;
                s.vx = (i == 0 ? -1.0f : 1.0f) * spd;
                s.useAccent2 = (i == 1);
                s.trail.fill ({ s.x, s.y });
                s.trailCount = 1;
            }
        }

        float sampleCurveY (float x) const noexcept
        {
            if (inner.getWidth() <= 1.0f) return inner.getCentreY();
            const float u = juce::jlimit (0.0f, 1.0f, (x - inner.getX()) / inner.getWidth());
            const float f = u * (float) (kProfileBins - 1);
            const int i0 = juce::jlimit (0, kProfileBins - 1, (int) f);
            const int i1 = juce::jmin (kProfileBins - 1, i0 + 1);
            return curveY[(size_t) i0] + (curveY[(size_t) i1] - curveY[(size_t) i0]) * (f - (float) i0);
        }

        void spawnEdgeSparks (float ex, float ey, float nx, bool accent2)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 4 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = ex;
                sp.y = ey;
                const float spd = 40.0f + rng.nextFloat() * 75.0f;
                const float ang = (rng.nextFloat() - 0.5f) * 1.9f;
                sp.vx = nx * (0.45f + 0.55f * std::abs (std::cos (ang))) * spd;
                sp.vy = std::sin (ang) * spd - 30.0f;
                sp.life = 0.0f;
                sp.maxLife = 0.25f + rng.nextFloat() * 0.18f;
                sp.useAccent2 = accent2;
            }
        }

        void step (float dt, ToyDirector& dir) override
        {
            const bool holding = (isSlowMo != nullptr) && isSlowMo();
            const float targetFade = holding ? 0.22f : 1.0f;
            slowFade += (targetFade - slowFade) * juce::jlimit (0.0f, 1.0f, dt * 14.0f);

            const float effDt = dt * (holding ? 0.10f : 1.0f);
            age += effDt;

            const float energyBoost = 1.0f + 0.30f * juce::jlimit (0.0f, 1.0f, dir.audioEnergy());
            for (auto& s : surfers)
            {
                if (! s.finished)
                {
                    s.x += s.vx * energyBoost * effDt;
                    if (s.x <= inner.getX() + 1.0f)
                    {
                        s.x = inner.getX() + 1.0f;
                        s.y = sampleCurveY (s.x);
                        s.finished = true;
                        spawnEdgeSparks (s.x, s.y, +1.0f, s.useAccent2);
                    }
                    else if (s.x >= inner.getRight() - 1.0f)
                    {
                        s.x = inner.getRight() - 1.0f;
                        s.y = sampleCurveY (s.x);
                        s.finished = true;
                        spawnEdgeSparks (s.x, s.y, -1.0f, s.useAccent2);
                    }
                    else
                    {
                        s.y = sampleCurveY (s.x);
                    }
                    for (int i = kTrailLen - 1; i > 0; --i)
                        s.trail[(size_t) i] = s.trail[(size_t) i - 1];
                    s.trail[0] = { s.x, s.y };
                    s.trailCount = juce::jmin (kTrailLen, s.trailCount + 1);
                }
                else if (s.trailCount > 1)
                {
                    --s.trailCount;
                }
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += effDt;
                sp.vy += 380.0f * effDt;
                sp.x += sp.vx * effDt;
                sp.y += sp.vy * effDt;
            }

            if (age >= ttl)
                alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            const float masterA = endFade * slowFade;
            if (masterA <= 0.01f)
                return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (canvas);

            // 1) 拨弦垂直驻波丝线（从频谱曲线到底边振动 + 顶段微弱泛音丝）
            const float strDecay = std::exp (-4.2f * age);
            if (strDecay > 0.02f)
            {
                const float amp = juce::jlimit (3.5f, 14.0f, inner.getWidth() * 0.028f)
                                * strDecay * std::sin (age * 38.0f);
                const float y0 = pluckCurveY;
                const float y1 = inner.getBottom();
                if (y1 - y0 > 4.0f)
                {
                    juce::Path strPath;
                    constexpr int kSegs = 14;
                    for (int i = 0; i <= kSegs; ++i)
                    {
                        const float u = (float) i / (float) kSegs;
                        const float px = pluckX + amp * std::sin (kPi * u);
                        const float py = y0 + (y1 - y0) * u;
                        if (i == 0) strPath.startNewSubPath (px, py);
                        else        strPath.lineTo (px, py);
                    }
                    g.setColour (PinkXP::sel.withAlpha (0.78f * strDecay * masterA));
                    g.strokePath (strPath, juce::PathStrokeType (1.25f));
                }

                // 曲线上方的微弱驻波延展丝
                if (pluckCurveY - inner.getY() > 6.0f)
                {
                    g.setColour (PinkXP::accent2.withAlpha (0.28f * strDecay * masterA));
                    g.drawLine (pluckX, inner.getY(), pluckX, pluckCurveY, 0.9f);
                }
            }

            // 2) 基频落点双环涟漪
            if (age < 0.42f)
            {
                const float t = age / 0.42f;
                drawPulseRing (g, pluckX, pluckCurveY, 3.0f + t * 22.0f, PinkXP::sel,     (1.0f - t) * 0.65f * masterA, 1.15f);
                drawPulseRing (g, pluckX, pluckCurveY, 2.0f + t * 13.0f, PinkXP::accent2, (1.0f - t) * 0.50f * masterA, 1.0f);
            }
            if (strDecay > 0.05f)
                drawGlowDot (g, pluckX, pluckCurveY, PinkXP::sel, strDecay * masterA, 3.6f, 1.4f);

            // 3) 真实泛音节点（2f / 3f / 4f）：错峰点亮脉冲环 + 细微连底刻度
            for (int h = 0; h < harmonicCount; ++h)
            {
                const float delay = 0.06f * (float) (h + 1);
                const float ha = age - delay;
                if (ha <= 0.0f) continue;
                const float ht = juce::jlimit (0.0f, 1.0f, ha / 0.85f);
                if (ht >= 1.0f) continue;
                const float fade = (1.0f - ht);
                const auto& pt = harmonics[(size_t) h];
                const juce::Colour hc = (h % 2 == 0) ? PinkXP::accent2 : PinkXP::sel;

                g.setColour (hc.withAlpha (0.25f * fade * masterA));
                g.drawLine (pt.x, pt.y, pt.x, inner.getBottom(), 0.8f);
                drawPulseRing (g, pt.x, pt.y, 2.5f + ht * (15.0f - 2.5f * (float) h), hc, 0.62f * fade * masterA, 1.0f);
                drawGlowDot (g, pt.x, pt.y, hc, fade * masterA, 3.0f, 1.25f);
            }

            // 4) 沿频谱曲线疾驰的双冲浪光珠
            for (const auto& s : surfers)
            {
                const juce::Colour col = s.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                for (int i = 1; i < s.trailCount; ++i)
                {
                    const float frac = 1.0f - (float) i / (float) kTrailLen;
                    const auto& p0 = s.trail[(size_t) (i - 1)];
                    const auto& p1 = s.trail[(size_t) i];
                    g.setColour (col.withAlpha (0.70f * frac * frac * masterA));
                    g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.7f + 1.2f * frac);
                }
                if (! s.finished)
                    drawGlowDot (g, s.x, s.y, col, masterA, 3.6f, 1.45f);
            }

            // 5) 撞边微火花
            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour sc = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (sc.withAlpha (0.82f * f * masterA));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            if (std::exp (-4.2f * age) > 0.02f)
                b.includeRect (pluckX - 15.0f, inner.getY(), pluckX + 15.0f, inner.getBottom());
            if (age < 0.42f)
                b.includePoint (pluckX, pluckCurveY, 26.0f);
            for (int h = 0; h < harmonicCount; ++h)
            {
                const float ha = age - 0.06f * (float) (h + 1);
                if (ha > 0.0f && ha < 0.85f)
                {
                    const auto& pt = harmonics[(size_t) h];
                    b.includeRect (pt.x - 18.0f, pt.y - 18.0f, pt.x + 18.0f, inner.getBottom());
                }
            }
            for (const auto& s : surfers)
            {
                for (int i = 0; i < s.trailCount; ++i)
                    b.includePoint (s.trail[(size_t) i].x, s.trail[(size_t) i].y, 4.0f);
                if (! s.finished)
                    b.includePoint (s.x, s.y, 4.5f);
            }
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (canvas, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 2) Waveform：贴合真实波形上下包络的双磷光扫描波前
    // ------------------------------------------------------
    struct WaveformShockActor : public ToyActor
    {
        static constexpr int kProfileBins = 96;
        static constexpr int kTrailLen    = 12;
        static constexpr int kMaxSparks   = 10;

        struct Front
        {
            float x = 0.0f, vx = 0.0f;
            float yTop = 0.0f, yBot = 0.0f;
            bool  finished = false;
            bool  useAccent2 = false;
            std::array<juce::Point<float>, kTrailLen> topTrail {};
            std::array<juce::Point<float>, kTrailLen> botTrail {};
            int   trailCount = 0;
        };

        juce::Rectangle<int>   lane;
        juce::Rectangle<float> inner;
        float                  originX = 0.0f;
        float                  originY = 0.0f;
        std::array<float, kProfileBins> topY {};
        std::array<float, kProfileBins> botY {};
        std::array<Front, 2>   fronts {};
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        std::function<bool()>  isSlowMo;
        float                  slowFade = 1.0f;

        WaveformShockActor (juce::Point<float> clickPos,
                            juce::Rectangle<int> laneRect,
                            const std::vector<float>& envTop,
                            const std::vector<float>& envBot,
                            std::function<bool()> slowMoQuery)
            : lane (laneRect),
              inner (laneRect.reduced (2).toFloat()),
              isSlowMo (std::move (slowMoQuery))
        {
            ttl = 1.45f;
            if (inner.getWidth() < 8.0f || inner.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }

            const float cy = inner.getCentreY();
            const int srcN = (int) juce::jmin (envTop.size(), envBot.size());
            for (int i = 0; i < kProfileBins; ++i)
            {
                float tY = cy - 2.0f;
                float bY = cy + 2.0f;
                if (srcN > 1)
                {
                    const float f = (float) i / (float) (kProfileBins - 1) * (float) (srcN - 1);
                    const int i0 = juce::jlimit (0, srcN - 1, (int) f);
                    const int i1 = juce::jmin (srcN - 1, i0 + 1);
                    const float frac = f - (float) i0;
                    const float ta = clampFinite (envTop[(size_t) i0], inner.getY(), inner.getBottom(), cy);
                    const float tb = clampFinite (envTop[(size_t) i1], inner.getY(), inner.getBottom(), cy);
                    const float ba = clampFinite (envBot[(size_t) i0], inner.getY(), inner.getBottom(), cy);
                    const float bb = clampFinite (envBot[(size_t) i1], inner.getY(), inner.getBottom(), cy);
                    tY = ta + (tb - ta) * frac;
                    bY = ba + (bb - ba) * frac;
                }
                if (bY < tY) std::swap (tY, bY);
                topY[(size_t) i] = tY;
                botY[(size_t) i] = bY;
            }

            originX = clampFinite (clickPos.x, inner.getX() + 2.0f, inner.getRight() - 2.0f, inner.getCentreX());
            originY = clampFinite (clickPos.y, inner.getY() + 2.0f, inner.getBottom() - 2.0f, cy);

            const float spd = juce::jlimit (170.0f, 480.0f, inner.getWidth() * 0.92f);
            float t0 = cy, b0 = cy;
            sampleEnv (originX, t0, b0);

            for (int i = 0; i < 2; ++i)
            {
                auto& fr = fronts[(size_t) i];
                fr.x = originX;
                fr.vx = (i == 0 ? -1.0f : 1.0f) * spd;
                fr.yTop = t0;
                fr.yBot = b0;
                fr.useAccent2 = (i == 1);
                fr.topTrail.fill ({ fr.x, fr.yTop });
                fr.botTrail.fill ({ fr.x, fr.yBot });
                fr.trailCount = 1;
            }
        }

        void sampleEnv (float x, float& outTop, float& outBot) const noexcept
        {
            if (inner.getWidth() <= 1.0f)
            {
                outTop = outBot = inner.getCentreY();
                return;
            }
            const float u = juce::jlimit (0.0f, 1.0f, (x - inner.getX()) / inner.getWidth());
            const float f = u * (float) (kProfileBins - 1);
            const int i0 = juce::jlimit (0, kProfileBins - 1, (int) f);
            const int i1 = juce::jmin (kProfileBins - 1, i0 + 1);
            const float frac = f - (float) i0;
            outTop = topY[(size_t) i0] + (topY[(size_t) i1] - topY[(size_t) i0]) * frac;
            outBot = botY[(size_t) i0] + (botY[(size_t) i1] - botY[(size_t) i0]) * frac;
        }

        void spawnEdgeSparks (float ex, float ey, float nx, bool accent2)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 4 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = ex;
                sp.y = ey;
                const float spd = 45.0f + rng.nextFloat() * 75.0f;
                const float ang = (rng.nextFloat() - 0.5f) * 2.0f;
                sp.vx = nx * (0.4f + 0.6f * std::abs (std::cos (ang))) * spd;
                sp.vy = std::sin (ang) * spd;
                sp.life = 0.0f;
                sp.maxLife = 0.24f + rng.nextFloat() * 0.16f;
                sp.useAccent2 = accent2;
            }
        }

        void step (float dt, ToyDirector& dir) override
        {
            const bool holding = (isSlowMo != nullptr) && isSlowMo();
            const float targetFade = holding ? 0.22f : 1.0f;
            slowFade += (targetFade - slowFade) * juce::jlimit (0.0f, 1.0f, dt * 14.0f);

            const float effDt = dt * (holding ? 0.10f : 1.0f);
            age += effDt;

            const float boost = 1.0f + 0.25f * juce::jlimit (0.0f, 1.0f, dir.audioEnergy());
            bool anyActive = (age < 0.35f);

            for (auto& fr : fronts)
            {
                if (! fr.finished)
                {
                    anyActive = true;
                    fr.x += fr.vx * boost * effDt;
                    if (fr.x <= inner.getX() + 1.0f)
                    {
                        fr.x = inner.getX() + 1.0f;
                        sampleEnv (fr.x, fr.yTop, fr.yBot);
                        fr.finished = true;
                        spawnEdgeSparks (fr.x, (fr.yTop + fr.yBot) * 0.5f, +1.0f, fr.useAccent2);
                    }
                    else if (fr.x >= inner.getRight() - 1.0f)
                    {
                        fr.x = inner.getRight() - 1.0f;
                        sampleEnv (fr.x, fr.yTop, fr.yBot);
                        fr.finished = true;
                        spawnEdgeSparks (fr.x, (fr.yTop + fr.yBot) * 0.5f, -1.0f, fr.useAccent2);
                    }
                    else
                    {
                        sampleEnv (fr.x, fr.yTop, fr.yBot);
                    }

                    for (int i = kTrailLen - 1; i > 0; --i)
                    {
                        fr.topTrail[(size_t) i] = fr.topTrail[(size_t) i - 1];
                        fr.botTrail[(size_t) i] = fr.botTrail[(size_t) i - 1];
                    }
                    fr.topTrail[0] = { fr.x, fr.yTop };
                    fr.botTrail[0] = { fr.x, fr.yBot };
                    fr.trailCount = juce::jmin (kTrailLen, fr.trailCount + 1);
                }
                else if (fr.trailCount > 1)
                {
                    anyActive = true;
                    --fr.trailCount;
                }
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                anyActive = true;
                sp.life += effDt;
                sp.x += sp.vx * effDt;
                sp.y += sp.vy * effDt;
            }

            if (age >= ttl || ! anyActive)
                alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.30f)) / 0.30f);
            const float masterA = endFade * slowFade;
            if (masterA <= 0.01f)
                return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (lane);

            // 1) 触发点极细竖向电火花丝 + 双环脉冲
            if (age < 0.35f)
            {
                const float t = age / 0.35f;
                const float a = (1.0f - t) * masterA;
                g.setColour (PinkXP::hl.withAlpha (0.55f * a));
                g.drawLine (originX, inner.getY(), originX, inner.getBottom(), 1.0f);
                drawPulseRing (g, originX, inner.getCentreY(), 3.0f + t * 20.0f, PinkXP::sel,     0.62f * a, 1.1f);
                drawPulseRing (g, originX, inner.getCentreY(), 2.0f + t * 11.0f, PinkXP::accent2, 0.48f * a, 0.9f);
            }

            // 2) 左右双包络扫描波前
            for (const auto& fr : fronts)
            {
                const juce::Colour col = fr.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                for (int i = 1; i < fr.trailCount; ++i)
                {
                    const float frac = 1.0f - (float) i / (float) kTrailLen;
                    const float la = 0.72f * frac * frac * masterA;
                    const float lw = 0.7f + 1.25f * frac;
                    g.setColour (col.withAlpha (la));
                    g.drawLine (fr.topTrail[(size_t) (i - 1)].x, fr.topTrail[(size_t) (i - 1)].y,
                                fr.topTrail[(size_t) i].x,       fr.topTrail[(size_t) i].y, lw);
                    g.drawLine (fr.botTrail[(size_t) (i - 1)].x, fr.botTrail[(size_t) (i - 1)].y,
                                fr.botTrail[(size_t) i].x,       fr.botTrail[(size_t) i].y, lw);
                }

                if (! fr.finished)
                {
                    // 极细竖向扫描桥接丝（高透明不盖波形）
                    if (fr.yBot - fr.yTop > 3.0f)
                    {
                        g.setColour (col.withAlpha (0.30f * masterA));
                        g.drawLine (fr.x, fr.yTop, fr.x, fr.yBot, 1.0f);
                    }
                    drawGlowDot (g, fr.x, fr.yTop, col, masterA, 3.4f, 1.35f);
                    if (fr.yBot - fr.yTop > 4.0f)
                        drawGlowDot (g, fr.x, fr.yBot, col, masterA, 3.4f, 1.35f);
                }
            }

            // 3) 边缘微火花
            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour sc = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (sc.withAlpha (0.82f * f * masterA));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            if (age < 0.35f)
                b.includeRect (originX - 24.0f, inner.getY(), originX + 24.0f, inner.getBottom());
            for (const auto& fr : fronts)
            {
                for (int i = 0; i < fr.trailCount; ++i)
                {
                    b.includePoint (fr.topTrail[(size_t) i].x, fr.topTrail[(size_t) i].y, 4.0f);
                    b.includePoint (fr.botTrail[(size_t) i].x, fr.botTrail[(size_t) i].y, 4.0f);
                }
                if (! fr.finished)
                {
                    b.includePoint (fr.x, fr.yTop, 4.5f);
                    b.includePoint (fr.x, fr.yBot, 4.5f);
                }
            }
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (lane, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 3) Spectrogram：四方向真实跟随瀑布流的十字声呐浮标
    // ------------------------------------------------------
    struct SpectrogramBuoyActor : public ToyActor
    {
        static constexpr int kWakeLen = 18;

        juce::Rectangle<int>   canvas;
        juce::Rectangle<float> inner;
        juce::Point<float>     pos;
        juce::Point<float>     vel;
        juce::Point<float>     smoothVel;
        std::array<juce::Point<float>, kWakeLen> wake {};
        int                    wakeCount = 0;

        std::function<bool()>  isFrozen;
        float                  slowFade = 1.0f;
        bool                   useAccent2 = false;

        SpectrogramBuoyActor (juce::Point<float> clickPos,
                              juce::Rectangle<int> canvasBounds,
                              juce::Point<float> driftVel,
                              std::function<bool()> frozenQuery)
            : canvas (canvasBounds),
              inner (canvasBounds.reduced (2).toFloat()),
              vel (driftVel),
              smoothVel (driftVel),
              isFrozen (std::move (frozenQuery))
        {
            ttl = 7.5f;
            if (inner.getWidth() < 8.0f || inner.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }
            pos.x = clampFinite (clickPos.x, inner.getX() + 1.0f, inner.getRight() - 1.0f, inner.getCentreX());
            pos.y = clampFinite (clickPos.y, inner.getY() + 1.0f, inner.getBottom() - 1.0f, inner.getCentreY());
            useAccent2 = juce::Random::getSystemRandom().nextBool();
            wake.fill (pos);
            wakeCount = 1;
        }

        void step (float dt, ToyDirector&) override
        {
            const bool frozen = (isFrozen != nullptr) && isFrozen();
            const float targetFade = frozen ? 0.28f : 1.0f;
            slowFade += (targetFade - slowFade) * juce::jlimit (0.0f, 1.0f, dt * 14.0f);

            // 按住冻结时浮标完全静止在原地（0x 漂移），松手恢复随瀑布流动
            if (frozen)
                return;

            age += dt;
            smoothVel.x = y2ktoy::expSmooth (smoothVel.x, vel.x, dt, 10.0f);
            smoothVel.y = y2ktoy::expSmooth (smoothVel.y, vel.y, dt, 10.0f);
            pos.x += smoothVel.x * dt;
            pos.y += smoothVel.y * dt;

            for (int i = kWakeLen - 1; i > 0; --i)
                wake[(size_t) i] = wake[(size_t) i - 1];
            wake[0] = pos;
            wakeCount = juce::jmin (kWakeLen, wakeCount + 1);

            const auto exitBounds = inner.expanded (24.0f);
            if (age >= ttl || ! exitBounds.contains (pos))
                alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.8f)) / 0.8f);
            const float masterA = endFade * slowFade;
            if (masterA <= 0.01f)
                return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (canvas);

            const juce::Colour col = useAccent2 ? PinkXP::accent2 : PinkXP::sel;

            // 1) 漂移尾迹细线
            for (int i = 1; i < wakeCount; ++i)
            {
                const float frac = 1.0f - (float) i / (float) kWakeLen;
                const auto& p0 = wake[(size_t) (i - 1)];
                const auto& p1 = wake[(size_t) i];
                g.setColour (col.withAlpha (0.52f * frac * frac * masterA));
                g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.7f + 0.8f * frac);
            }

            // 2) 初始双环声呐定位脉冲（0.45s）
            if (age < 0.45f)
            {
                const float t = age / 0.45f;
                drawPulseRing (g, pos.x, pos.y, 4.0f + t * 24.0f, col,       (1.0f - t) * 0.65f * masterA, 1.1f);
                drawPulseRing (g, pos.x, pos.y, 2.0f + t * 13.0f, PinkXP::hl, (1.0f - t) * 0.50f * masterA, 0.9f);
            }

            // 3) 精致 1px 十字准星浮标 + 微弱呼吸环 + 亮核（极低遮挡）
            const float pulse = 0.5f + 0.5f * std::sin (age * 6.5f);
            const float armIn  = 3.0f;
            const float armOut = 7.5f;
            g.setColour (col.withAlpha ((0.65f + 0.25f * pulse) * masterA));
            g.drawLine (pos.x - armOut, pos.y, pos.x - armIn,  pos.y, 1.0f);
            g.drawLine (pos.x + armIn,  pos.y, pos.x + armOut, pos.y, 1.0f);
            g.drawLine (pos.x, pos.y - armOut, pos.x, pos.y - armIn,  1.0f);
            g.drawLine (pos.x, pos.y + armIn,  pos.x, pos.y + armOut, 1.0f);

            const float ringR = 5.5f + 1.5f * pulse;
            drawPulseRing (g, pos.x, pos.y, ringR, col, 0.35f * masterA, 0.9f);
            drawGlowDot (g, pos.x, pos.y, col, masterA, 3.0f, 1.3f);
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            for (int i = 0; i < wakeCount; ++i)
                b.includePoint (wake[(size_t) i].x, wake[(size_t) i].y, 2.0f);
            b.includePoint (pos.x, pos.y, age < 0.45f ? 29.0f : 10.0f);
            return b.finish (canvas, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 4) Spectrogram3D：透视倾斜激光雷达等高脉冲 + 沿深度轴漂移磷光微粒
    // ------------------------------------------------------
    struct Spec3DPulseActor : public ToyActor
    {
        static constexpr int kNumEmbers = 10;

        struct Ember
        {
            float x = 0.0f, y = 0.0f;
            float vx = 0.0f, vy = 0.0f;
            float phase = 0.0f;
            float life = 0.0f, maxLife = 1.0f;
            bool  useAccent2 = false;
        };

        juce::Rectangle<int>   canvas;
        juce::Point<float>     origin;
        juce::Point<float>     depthDir;   // 单位向量（沿当前 Back-right / Back-left 退远方向）
        std::array<Ember, kNumEmbers> embers {};

        Spec3DPulseActor (juce::Point<float> clickPos,
                          juce::Rectangle<int> canvasBounds,
                          juce::Point<float> depthDirUnit)
            : canvas (canvasBounds),
              origin (clickPos)
        {
            ttl = 1.65f;
            if (canvas.getWidth() < 8 || canvas.getHeight() < 8)
            {
                alive = false;
                return;
            }
            const float len = std::hypot (depthDirUnit.x, depthDirUnit.y);
            depthDir = (len > 1.0e-4f) ? juce::Point<float> (depthDirUnit.x / len, depthDirUnit.y / len)
                                       : juce::Point<float> (0.45f, -0.89f);

            origin.x = clampFinite (origin.x, (float) canvas.getX() + 2.0f, (float) canvas.getRight() - 2.0f, (float) canvas.getCentreX());
            origin.y = clampFinite (origin.y, (float) canvas.getY() + 2.0f, (float) canvas.getBottom() - 2.0f, (float) canvas.getCentreY());

            auto& rng = juce::Random::getSystemRandom();
            for (int i = 0; i < kNumEmbers; ++i)
            {
                auto& e = embers[(size_t) i];
                const float spreadX = (rng.nextFloat() - 0.5f) * 32.0f;
                const float spreadY = (rng.nextFloat() - 0.5f) * 14.0f;
                e.x = origin.x + spreadX * 0.45f;
                e.y = origin.y + spreadY * 0.45f;
                const float depthSpd = 38.0f + rng.nextFloat() * 58.0f;
                e.vx = depthDir.x * depthSpd + spreadX * 1.1f;
                e.vy = depthDir.y * depthSpd - (14.0f + rng.nextFloat() * 24.0f);
                e.phase = rng.nextFloat() * kTwoPi;
                e.life = 0.0f;
                e.maxLife = 0.75f + rng.nextFloat() * 0.75f;
                e.useAccent2 = (i & 1) != 0;
            }
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            for (auto& e : embers)
            {
                if (e.life >= e.maxLife) continue;
                e.life += dt;
                const float sway = std::sin (age * 7.5f + e.phase) * 16.0f;
                e.x += (e.vx + sway * 0.35f) * dt;
                e.y += e.vy * dt;
            }
            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            if (endFade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (canvas);

            // 1) 两条透视斜切平行四边形激光雷达波（严格对齐频率水平轴 + depthDir 纵深斜轴）
            const float ux = 1.0f, uy = 0.0f;
            const float vx = depthDir.x * 0.65f, vy = depthDir.y * 0.65f;
            for (int r = 0; r < 2; ++r)
            {
                const float t = (age - (float) r * 0.14f) / 0.78f;
                if (t <= 0.0f || t >= 1.0f) continue;
                const float ease = 1.0f - (1.0f - t) * (1.0f - t);
                const float a = (1.0f - t) * (r == 0 ? 0.75f : 0.55f) * endFade;
                const float radX = 6.0f + ease * 42.0f;
                const float radD = 4.0f + ease * 26.0f;

                // 波心随时间轻微沿深度轴漂移，融入 3D 地形滚动感
                const float cx = origin.x + depthDir.x * ease * 14.0f;
                const float cy = origin.y + depthDir.y * ease * 14.0f;

                juce::Path quad;
                quad.startNewSubPath (cx - ux * radX - vx * radD, cy - uy * radX - vy * radD);
                quad.lineTo          (cx + ux * radX - vx * radD, cy + uy * radX - vy * radD);
                quad.lineTo          (cx + ux * radX + vx * radD, cy + uy * radX + vy * radD);
                quad.lineTo          (cx - ux * radX + vx * radD, cy - uy * radX + vy * radD);
                quad.closeSubPath();

                g.setColour ((r == 0 ? PinkXP::sel : PinkXP::accent2).withAlpha (a));
                g.strokePath (quad, juce::PathStrokeType (r == 0 ? 1.25f : 1.0f));
            }

            // 2) 落点瞬间垂直峰顶激光标针（0.28s）
            if (age < 0.28f)
            {
                const float t = age / 0.28f;
                const float h = 22.0f * (1.0f - t * 0.5f);
                g.setColour (PinkXP::hl.withAlpha ((1.0f - t) * 0.85f * endFade));
                g.drawLine (origin.x, origin.y, origin.x, origin.y - h, 1.2f);
                drawGlowDot (g, origin.x, origin.y, PinkXP::sel, (1.0f - t) * endFade, 3.5f, 1.4f);
            }

            // 3) 沿 3D 深度方向飘逸的磷光微粒
            for (const auto& e : embers)
            {
                if (e.life >= e.maxLife) continue;
                const float f = 1.0f - e.life / e.maxLife;
                const juce::Colour col = e.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (col.withAlpha (0.80f * f * endFade));
                g.fillRect (e.x - 1.0f, e.y - 1.0f, 2.0f, 2.0f);
                if (f > 0.45f)
                {
                    g.setColour (PinkXP::hl.withAlpha (0.65f * f * endFade));
                    g.fillRect (e.x - 0.5f, e.y - 0.5f, 1.0f, 1.0f);
                }
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            const float vx = depthDir.x * 0.65f, vy = depthDir.y * 0.65f;
            for (int r = 0; r < 2; ++r)
            {
                const float t = (age - (float) r * 0.14f) / 0.78f;
                if (t <= 0.0f || t >= 1.0f) continue;
                const float ease = 1.0f - (1.0f - t) * (1.0f - t);
                const float radX = 6.0f + ease * 42.0f;
                const float radD = 4.0f + ease * 26.0f;
                const float cx = origin.x + depthDir.x * ease * 14.0f;
                const float cy = origin.y + depthDir.y * ease * 14.0f;
                b.includePoint (cx - radX - vx * radD, cy - vy * radD, 2.0f);
                b.includePoint (cx + radX - vx * radD, cy - vy * radD, 2.0f);
                b.includePoint (cx + radX + vx * radD, cy + vy * radD, 2.0f);
                b.includePoint (cx - radX + vx * radD, cy + vy * radD, 2.0f);
            }
            if (age < 0.28f)
                b.includeRect (origin.x - 4.0f, origin.y - 24.0f, origin.x + 4.0f, origin.y + 4.0f);
            for (const auto& e : embers)
                if (e.life < e.maxLife)
                    b.includePoint (e.x, e.y, 2.0f);
            return b.finish (canvas, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 5) StereoField：半圆雷达扫描光束 + 极坐标弧形涟漪 + 沿等电平弧摆动的阻尼光珠
    // ------------------------------------------------------
    struct StereoRadarSweepActor : public ToyActor
    {
        static constexpr int kTrailLen  = 16;
        static constexpr int kMaxSparks = 8;

        juce::Rectangle<int> canvas;
        juce::Point<float>   origin;       // 半圆圆心 (cx, baseY)
        float                radarR = 40.0f;
        float                orbR   = 20.0f;
        float                startPhi = 0.0f;   // 0 = 正上，-pi/2 = 左，+pi/2 = 右
        float                phi      = 0.0f;
        float                omega    = 0.0f;
        bool                 useAccent2 = false;

        std::array<juce::Point<float>, kTrailLen> trail {};
        int                  trailCount = 0;
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                  sparkCount = 0;

        StereoRadarSweepActor (juce::Point<float> clickPos,
                               juce::Rectangle<int> canvasBounds,
                               juce::Point<float> radarOrigin,
                               float radius)
            : canvas (canvasBounds),
              origin (radarOrigin),
              radarR (juce::jmax (12.0f, radius))
        {
            ttl = 1.85f;
            if (canvas.getWidth() < 8 || canvas.getHeight() < 8)
            {
                alive = false;
                return;
            }

            const float dx = clickPos.x - origin.x;
            const float dy = juce::jmax (1.0f, origin.y - clickPos.y);
            orbR = juce::jlimit (radarR * 0.18f, radarR * 0.96f, std::hypot (dx, dy));
            startPhi = juce::jlimit (-kPi * 0.47f, kPi * 0.47f, std::atan2 (dx, dy));
            phi = startPhi;

            // 沿弧线向反侧切入摆动（点偏左往右荡，点居中随机方向）
            auto& rng = juce::Random::getSystemRandom();
            const float dirSign = (std::abs (startPhi) > 0.12f) ? (startPhi > 0.0f ? -1.0f : 1.0f)
                                                                : (rng.nextBool() ? 1.0f : -1.0f);
            omega = dirSign * (3.8f + rng.nextFloat() * 1.4f);
            useAccent2 = rng.nextBool();

            const auto p0 = polarToScreen (orbR, phi);
            trail.fill (p0);
            trailCount = 1;
        }

        juce::Point<float> polarToScreen (float r, float a) const noexcept
        {
            return { origin.x + std::sin (a) * r, origin.y - std::cos (a) * r };
        }

        void spawnArcSparks (juce::Point<float> at, bool accent2)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 3 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = at.x;
                sp.y = at.y;
                const float ang = rng.nextFloat() * kTwoPi;
                const float spd = 35.0f + rng.nextFloat() * 55.0f;
                sp.vx = std::cos (ang) * spd;
                sp.vy = std::sin (ang) * spd - 20.0f;
                sp.life = 0.0f;
                sp.maxLife = 0.25f + rng.nextFloat() * 0.15f;
                sp.useAccent2 = accent2;
            }
        }

        void step (float dt, ToyDirector& dir) override
        {
            age += dt;
            // 谐振摆引力回到中线 + 轻微阻尼，形成优雅的极坐标钟摆弧线
            const float kSpring = 18.0f + 6.0f * juce::jlimit (0.0f, 1.0f, dir.audioEnergy());
            omega += (-kSpring * phi - 1.55f * omega) * dt;
            phi   += omega * dt;

            constexpr float kLimit = kPi * 0.485f;
            if (phi < -kLimit)
            {
                phi = -kLimit;
                omega = std::abs (omega) * 0.82f;
                spawnArcSparks (polarToScreen (orbR, phi), ! useAccent2);
            }
            else if (phi > kLimit)
            {
                phi = kLimit;
                omega = -std::abs (omega) * 0.82f;
                spawnArcSparks (polarToScreen (orbR, phi), ! useAccent2);
            }

            const auto pt = polarToScreen (orbR, phi);
            for (int i = kTrailLen - 1; i > 0; --i)
                trail[(size_t) i] = trail[(size_t) i - 1];
            trail[0] = pt;
            trailCount = juce::jmin (kTrailLen, trailCount + 1);

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += dt;
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
            }

            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.40f)) / 0.40f);
            if (endFade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (canvas);

            const juce::Colour col = useAccent2 ? PinkXP::accent2 : PinkXP::sel;

            // 1) 雷达径向扫描光束（0.55s，极细丝线 + 淡扇形余辉）
            if (age < 0.55f)
            {
                const float t = age / 0.55f;
                const float a = (1.0f - t) * endFade;
                const auto tip = polarToScreen (radarR, startPhi);
                g.setColour (col.withAlpha (0.60f * a));
                g.drawLine (origin.x, origin.y, tip.x, tip.y, 1.2f);

                // 半圆内沿点击半径扩散的极坐标弧线涟漪
                const float rExp = juce::jmin (radarR, orbR + t * 28.0f);
                const float halfSpan = juce::jlimit (0.15f, kPi * 0.48f, 0.22f + t * 0.65f);
                const float a0 = juce::jmax (-kPi * 0.5f, startPhi - halfSpan);
                const float a1 = juce::jmin ( kPi * 0.5f, startPhi + halfSpan);
                if (a1 > a0 + 0.02f)
                {
                    juce::Path arc;
                    arc.addCentredArc (origin.x, origin.y, rExp, rExp, 0.0f, a0, a1, true);
                    g.setColour (PinkXP::accent2.withAlpha (0.55f * a));
                    g.strokePath (arc, juce::PathStrokeType (1.1f));
                }
            }

            // 2) 等电平弧上的阻尼钟摆尾迹 + 光珠
            for (int i = 1; i < trailCount; ++i)
            {
                const float frac = 1.0f - (float) i / (float) kTrailLen;
                const auto& p0 = trail[(size_t) (i - 1)];
                const auto& p1 = trail[(size_t) i];
                g.setColour (col.withAlpha (0.74f * frac * frac * endFade));
                g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.7f + 1.4f * frac);
            }
            const auto head = trail[0];
            drawGlowDot (g, head.x, head.y, col, endFade, 3.8f, 1.5f);

            // 3) 边界微火花
            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour sc = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (sc.withAlpha (0.82f * f * endFade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            if (age < 0.55f)
            {
                const float t = age / 0.55f;
                b.includePoint (origin.x, origin.y, 2.0f);
                const auto tip = polarToScreen (radarR, startPhi);
                b.includePoint (tip.x, tip.y, 2.0f);
                const float rExp = juce::jmin (radarR, orbR + t * 28.0f);
                const float halfSpan = juce::jlimit (0.15f, kPi * 0.48f, 0.22f + t * 0.65f);
                const float a0 = juce::jmax (-kPi * 0.5f, startPhi - halfSpan);
                const float a1 = juce::jmin ( kPi * 0.5f, startPhi + halfSpan);
                constexpr int kArcSamples = 12;
                for (int s = 0; s <= kArcSamples; ++s)
                {
                    const float a = a0 + (a1 - a0) * ((float) s / (float) kArcSamples);
                    const auto pt = polarToScreen (rExp, a);
                    b.includePoint (pt.x, pt.y, 2.0f);
                }
                if (a0 < 0.0f && a1 > 0.0f)
                {
                    const auto apex = polarToScreen (rExp, 0.0f);
                    b.includePoint (apex.x, apex.y, 2.0f);
                }
            }
            for (int i = 0; i < trailCount; ++i)
                b.includePoint (trail[(size_t) i].x, trail[(size_t) i].y, 4.5f);
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (canvas, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 6) StereoMap：PAN 模式弹性中线拨弦 + 对称双光珠 / MONO 模式相干干涉波纹
    // ------------------------------------------------------
    struct StereoMapBeamActor : public ToyActor
    {
        static constexpr int kTrailLen  = 12;
        static constexpr int kMaxSparks = 8;

        struct MirrorBead
        {
            float x = 0.0f, y = 0.0f, vy = 0.0f;
            bool  finished = false;
            bool  useAccent2 = false;
            std::array<juce::Point<float>, kTrailLen> trail {};
            int   trailCount = 0;
        };

        juce::Rectangle<int>   plot;
        juce::Rectangle<float> inner;
        juce::Point<float>     origin;
        bool                   monoMode = false;
        std::array<MirrorBead, 2> beads {};
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        std::function<bool()>  isSlowMo;
        float                  slowFade = 1.0f;

        StereoMapBeamActor (juce::Point<float> clickPos,
                            juce::Rectangle<int> plotRect,
                            bool isMono,
                            std::function<bool()> slowMoQuery)
            : plot (plotRect),
              inner (plotRect.reduced (2).toFloat()),
              monoMode (isMono),
              isSlowMo (std::move (slowMoQuery))
        {
            ttl = 1.45f;
            if (inner.getWidth() < 8.0f || inner.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }
            origin.x = clampFinite (clickPos.x, inner.getX() + 2.0f, inner.getRight() - 2.0f, inner.getCentreX());
            origin.y = clampFinite (clickPos.y, inner.getY() + 2.0f, inner.getBottom() - 2.0f, inner.getCentreY());

            const float midY = inner.getCentreY();
            const float spd = juce::jlimit (120.0f, 320.0f, inner.getHeight() * 1.45f);
            for (int i = 0; i < 2; ++i)
            {
                auto& b = beads[(size_t) i];
                b.x = origin.x;
                b.y = midY;
                b.vy = (i == 0 ? -1.0f : 1.0f) * spd;
                b.useAccent2 = (i == 1);
                b.trail.fill ({ b.x, b.y });
                b.trailCount = 1;
            }
        }

        void spawnEdgeSparks (float ex, float ey, float ny, bool accent2)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 4 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = ex;
                sp.y = ey;
                const float spd = 35.0f + rng.nextFloat() * 65.0f;
                const float ang = (rng.nextFloat() - 0.5f) * 2.0f;
                sp.vx = std::sin (ang) * spd;
                sp.vy = ny * (0.45f + 0.55f * std::abs (std::cos (ang))) * spd;
                sp.life = 0.0f;
                sp.maxLife = 0.24f + rng.nextFloat() * 0.16f;
                sp.useAccent2 = accent2;
            }
        }

        void step (float dt, ToyDirector&) override
        {
            const bool holding = (isSlowMo != nullptr) && isSlowMo();
            const float targetFade = holding ? 0.22f : 1.0f;
            slowFade += (targetFade - slowFade) * juce::jlimit (0.0f, 1.0f, dt * 14.0f);

            const float effDt = dt * (holding ? 0.10f : 1.0f);
            age += effDt;

            if (! monoMode)
            {
                for (auto& b : beads)
                {
                    if (! b.finished)
                    {
                        b.y += b.vy * effDt;
                        if (b.y <= inner.getY() + 1.0f)
                        {
                            b.y = inner.getY() + 1.0f;
                            b.finished = true;
                            spawnEdgeSparks (b.x, b.y, +1.0f, b.useAccent2);
                        }
                        else if (b.y >= inner.getBottom() - 1.0f)
                        {
                            b.y = inner.getBottom() - 1.0f;
                            b.finished = true;
                            spawnEdgeSparks (b.x, b.y, -1.0f, b.useAccent2);
                        }
                        for (int i = kTrailLen - 1; i > 0; --i)
                            b.trail[(size_t) i] = b.trail[(size_t) i - 1];
                        b.trail[0] = { b.x, b.y };
                        b.trailCount = juce::jmin (kTrailLen, b.trailCount + 1);
                    }
                    else if (b.trailCount > 1)
                    {
                        --b.trailCount;
                    }
                }
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += effDt;
                sp.x += sp.vx * effDt;
                sp.y += sp.vy * effDt;
            }

            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            const float masterA = endFade * slowFade;
            if (masterA <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (plot);

            if (! monoMode)
            {
                // PAN 模式：弹性中线弦在点击处拉起再阻尼回弹 + 对称上下（L / R）光珠
                const float midY = inner.getCentreY();
                float pull = origin.y - midY;
                if (std::abs (pull) < 10.0f)
                    pull = (pull >= 0.0f ? 1.0f : -1.0f) * 14.0f;
                const float decay = std::exp (-4.2f * age);
                if (decay > 0.02f)
                {
                    const float disp = pull * decay * std::cos (24.0f * age);
                    const float halfSpan = juce::jmin (inner.getWidth() * 0.42f, 95.0f);
                    const float x0 = juce::jmax (inner.getX(), origin.x - halfSpan);
                    const float x1 = juce::jmin (inner.getRight(), origin.x + halfSpan);
                    if (x1 - x0 > 6.0f)
                    {
                        juce::Path str;
                        constexpr int kSegs = 20;
                        for (int i = 0; i <= kSegs; ++i)
                        {
                            const float u = (float) i / (float) kSegs;
                            const float px = x0 + (x1 - x0) * u;
                            const float py = midY + disp * std::sin (kPi * u);
                            if (i == 0) str.startNewSubPath (px, py);
                            else        str.lineTo (px, py);
                        }
                        g.setColour (PinkXP::sel.withAlpha (0.75f * decay * masterA));
                        g.strokePath (str, juce::PathStrokeType (1.25f));
                    }
                }

                for (const auto& b : beads)
                {
                    const juce::Colour col = b.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                    for (int i = 1; i < b.trailCount; ++i)
                    {
                        const float frac = 1.0f - (float) i / (float) kTrailLen;
                        const auto& p0 = b.trail[(size_t) (i - 1)];
                        const auto& p1 = b.trail[(size_t) i];
                        g.setColour (col.withAlpha (0.72f * frac * frac * masterA));
                        g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.7f + 1.3f * frac);
                    }
                    if (! b.finished)
                        drawGlowDot (g, b.x, b.y, col, masterA, 3.6f, 1.45f);
                }
            }
            else
            {
                // MONO 模式：两条反向传播的 1px 相干干涉正弦波 + 中心干涉节点
                const float t = age / ttl;
                const float waveFade = (1.0f - t) * masterA;
                const float spread = 16.0f + age * inner.getWidth() * 0.72f;
                const float x0 = juce::jmax (inner.getX(), origin.x - spread);
                const float x1 = juce::jmin (inner.getRight(), origin.x + spread);
                if (x1 - x0 > 6.0f)
                {
                    const float maxAmp = juce::jlimit (5.0f, 16.0f, inner.getHeight() * 0.16f) * (1.0f - t * 0.7f);
                    for (int w = 0; w < 2; ++w)
                    {
                        juce::Path p;
                        constexpr int kSteps = 32;
                        const float phaseSign = (w == 0 ? 1.0f : -1.0f);
                        for (int i = 0; i <= kSteps; ++i)
                        {
                            const float u = (float) i / (float) kSteps;
                            const float px = x0 + (x1 - x0) * u;
                            const float env = std::sin (kPi * u);
                            const float py = origin.y + maxAmp * env * env
                                           * std::sin ((px - origin.x) * 0.14f - phaseSign * age * 18.0f);
                            if (i == 0) p.startNewSubPath (px, py);
                            else        p.lineTo (px, py);
                        }
                        g.setColour ((w == 0 ? PinkXP::sel : PinkXP::accent2).withAlpha (0.70f * waveFade));
                        g.strokePath (p, juce::PathStrokeType (1.1f));
                    }
                }

                // 波前两个亮点
                if (origin.x - spread >= inner.getX())
                    drawGlowDot (g, origin.x - spread, origin.y, PinkXP::sel, waveFade, 3.2f, 1.3f);
                if (origin.x + spread <= inner.getRight())
                    drawGlowDot (g, origin.x + spread, origin.y, PinkXP::accent2, waveFade, 3.2f, 1.3f);
            }

            if (age < 0.36f)
            {
                const float t = age / 0.36f;
                drawPulseRing (g, origin.x, origin.y, 3.0f + t * 18.0f, PinkXP::sel, (1.0f - t) * 0.62f * masterA, 1.05f);
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour sc = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (sc.withAlpha (0.82f * f * masterA));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            if (! monoMode)
            {
                const float midY = inner.getCentreY();
                float pull = origin.y - midY;
                if (std::abs (pull) < 10.0f)
                    pull = (pull >= 0.0f ? 1.0f : -1.0f) * 14.0f;
                const float decay = std::exp (-4.2f * age);
                if (decay > 0.02f)
                {
                    const float halfSpan = juce::jmin (inner.getWidth() * 0.42f, 95.0f);
                    const float x0 = juce::jmax (inner.getX(), origin.x - halfSpan);
                    const float x1 = juce::jmin (inner.getRight(), origin.x + halfSpan);
                    const float maxDisp = std::abs (pull) * decay + 2.0f;
                    b.includeRect (x0, midY - maxDisp, x1, midY + maxDisp);
                }
                for (const auto& bd : beads)
                {
                    for (int i = 0; i < bd.trailCount; ++i)
                        b.includePoint (bd.trail[(size_t) i].x, bd.trail[(size_t) i].y, 4.0f);
                    if (! bd.finished)
                        b.includePoint (bd.x, bd.y, 4.5f);
                }
            }
            else
            {
                const float spread = 16.0f + age * inner.getWidth() * 0.72f;
                const float x0 = juce::jmax (inner.getX(), origin.x - spread);
                const float x1 = juce::jmin (inner.getRight(), origin.x + spread);
                const float maxAmp = juce::jlimit (5.0f, 16.0f, inner.getHeight() * 0.16f) + 4.0f;
                b.includeRect (x0 - 4.0f, origin.y - maxAmp, x1 + 4.0f, origin.y + maxAmp);
            }
            if (age < 0.36f)
                b.includePoint (origin.x, origin.y, 22.0f);
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (plot, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 7) Phase Gonio：限制在 M/S 菱形内的荧光引力电子（四壁菱形反射）
    // ------------------------------------------------------
    struct PhaseGonioOrbiterActor : public ToyActor
    {
        static constexpr int kTrailLen  = 20;
        static constexpr int kMaxSparks = 10;

        juce::Rectangle<int> area;
        juce::Point<float>   center;
        float                diamondR = 24.0f;
        juce::Point<float>   origin;
        float                x = 0.0f, y = 0.0f;
        float                vx = 0.0f, vy = 0.0f;
        bool                 useAccent2 = false;

        std::array<juce::Point<float>, kTrailLen> trail {};
        int                  trailCount = 0;
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                  sparkCount = 0;

        PhaseGonioOrbiterActor (juce::Point<float> clickPos, juce::Rectangle<int> gonioArea)
            : area (gonioArea)
        {
            ttl = 1.95f;
            auto inner = gonioArea.reduced (6);
            if (inner.getWidth() < 12 || inner.getHeight() < 12)
            {
                alive = false;
                return;
            }
            center   = inner.getCentre().toFloat();
            diamondR = (float) juce::jmin (inner.getWidth(), inner.getHeight()) * 0.48f;

            // 将落点投射进菱形 |dx| + |dy| <= R - 3 内
            float dx = clickPos.x - center.x;
            float dy = clickPos.y - center.y;
            const float manh = std::abs (dx) + std::abs (dy);
            const float lim  = juce::jmax (4.0f, diamondR - 3.0f);
            if (manh > lim && manh > 1.0e-4f)
            {
                const float s = lim / manh;
                dx *= s;
                dy *= s;
            }
            x = center.x + dx;
            y = center.y + dy;
            origin = { x, y };

            auto& rng = juce::Random::getSystemRandom();
            useAccent2 = rng.nextBool();
            float dist = std::hypot (dx, dy);
            if (dist < 4.0f)
            {
                const float a = rng.nextFloat() * kTwoPi;
                dx = std::cos (a) * 10.0f;
                dy = std::sin (a) * 10.0f;
                dist = 10.0f;
            }
            const float spin = rng.nextBool() ? 1.0f : -1.0f;
            const float spd  = juce::jlimit (100.0f, 280.0f, diamondR * (2.6f + rng.nextFloat() * 0.9f));
            vx = (-dy / dist) * spin * spd + (dx / dist) * (rng.nextFloat() - 0.5f) * spd * 0.4f;
            vy = ( dx / dist) * spin * spd + (dy / dist) * (rng.nextFloat() - 0.5f) * spd * 0.4f;

            trail.fill ({ x, y });
            trailCount = 1;
        }

        void spawnWallSparks (float bx, float by, float nx, float ny)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 3 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = bx;
                sp.y = by;
                const float spd = 30.0f + rng.nextFloat() * 55.0f;
                const float j = (rng.nextFloat() - 0.5f) * 1.4f;
                const float cs = std::cos (j), sn = std::sin (j);
                sp.vx = (nx * cs - ny * sn) * spd;
                sp.vy = (nx * sn + ny * cs) * spd;
                sp.life = 0.0f;
                sp.maxLife = 0.24f + rng.nextFloat() * 0.16f;
                sp.useAccent2 = ! useAccent2;
            }
        }

        void step (float dt, ToyDirector& dir) override
        {
            age += dt;
            const float dx = center.x - x;
            const float dy = center.y - y;
            const float kSpring = 11.0f + 5.0f * juce::jlimit (0.0f, 1.0f, dir.audioEnergy());
            vx += dx * kSpring * dt;
            vy += dy * kSpring * dt;

            const float damp = std::exp (-0.30f * dt);
            vx *= damp;
            vy *= damp;
            x += vx * dt;
            y += vy * dt;

            // M/S 菱形四壁弹性反射（|rx| + |ry| <= lim）
            float rx = x - center.x;
            float ry = y - center.y;
            const float lim = juce::jmax (4.0f, diamondR - 2.5f);
            const float manh = std::abs (rx) + std::abs (ry);
            if (manh > lim && manh > 1.0e-4f)
            {
                const float sx = (rx >= 0.0f) ? 1.0f : -1.0f;
                const float sy = (ry >= 0.0f) ? 1.0f : -1.0f;
                // 朝向菱形内部的单位法线
                const float nx = -sx * 0.70710678f;
                const float ny = -sy * 0.70710678f;
                const float scale = lim / manh;
                x = center.x + rx * scale;
                y = center.y + ry * scale;
                const float vn = vx * nx + vy * ny;
                if (vn < 0.0f)
                {
                    vx -= 1.88f * vn * nx;
                    vy -= 1.88f * vn * ny;
                    spawnWallSparks (x, y, nx, ny);
                }
            }

            for (int i = kTrailLen - 1; i > 0; --i)
                trail[(size_t) i] = trail[(size_t) i - 1];
            trail[0] = { x, y };
            trailCount = juce::jmin (kTrailLen, trailCount + 1);

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += dt;
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
            }

            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.40f)) / 0.40f);
            if (endFade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (area);

            const juce::Colour col = useAccent2 ? PinkXP::accent2 : PinkXP::sel;

            // 落点极细菱形微脉冲（0.34s）
            if (age < 0.34f)
            {
                const float t = age / 0.34f;
                const float r = 3.0f + t * 18.0f;
                juce::Path d;
                d.startNewSubPath (origin.x, origin.y - r);
                d.lineTo (origin.x + r, origin.y);
                d.lineTo (origin.x, origin.y + r);
                d.lineTo (origin.x - r, origin.y);
                d.closeSubPath();
                g.setColour (col.withAlpha ((1.0f - t) * 0.65f * endFade));
                g.strokePath (d, juce::PathStrokeType (1.1f));
            }

            for (int i = 1; i < trailCount; ++i)
            {
                const float frac = 1.0f - (float) i / (float) kTrailLen;
                const auto& p0 = trail[(size_t) (i - 1)];
                const auto& p1 = trail[(size_t) i];
                g.setColour (col.withAlpha (0.72f * frac * frac * endFade));
                g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.65f + 1.35f * frac);
            }
            drawGlowDot (g, x, y, col, endFade, 3.7f, 1.5f);

            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour sc = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (sc.withAlpha (0.82f * f * endFade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            if (age < 0.34f)
                b.includePoint (origin.x, origin.y, 22.0f);
            for (int i = 0; i < trailCount; ++i)
                b.includePoint (trail[(size_t) i].x, trail[(size_t) i].y, 4.0f);
            b.includePoint (x, y, 4.5f);
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (area, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 8) Phase WIDTH / BAL 条形槽：纯矢量荧光弹珠（零 emoji）
    // ------------------------------------------------------
    struct TrackOrbActor : public ToyActor
    {
        static constexpr int kTrailLen  = 12;
        static constexpr int kMaxSparks = 8;

        juce::Rectangle<float> track;
        float                  x = 0.0f, y = 0.0f, vx = 0.0f;
        int                    bounces = 0;
        bool                   useAccent2 = false;
        std::array<float, kTrailLen> trailX {};
        int                    trailCount = 0;
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        TrackOrbActor (juce::Rectangle<float> t, juce::Point<float> clickPos, bool accent2)
            : track (t), useAccent2 (accent2)
        {
            ttl = 1.65f;
            if (track.getWidth() < 10.0f || track.getHeight() < 4.0f)
            {
                alive = false;
                return;
            }
            x = clampFinite (clickPos.x, track.getX() + 3.0f, track.getRight() - 3.0f, track.getCentreX());
            y = track.getCentreY();
            // 朝离得更远的一端发射，手感最舒展
            const float dir = (x - track.getX() < track.getRight() - x) ? 1.0f : -1.0f;
            vx = dir * juce::jlimit (190.0f, 420.0f, track.getWidth() * 1.45f);
            trailX.fill (x);
            trailCount = 1;
        }

        void spawnWallSparks (float ex, float nx)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 3 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = ex;
                sp.y = y;
                const float spd = 35.0f + rng.nextFloat() * 55.0f;
                sp.vx = nx * (0.5f + rng.nextFloat() * 0.5f) * spd;
                sp.vy = (rng.nextFloat() - 0.5f) * 55.0f;
                sp.life = 0.0f;
                sp.maxLife = 0.22f + rng.nextFloat() * 0.14f;
                sp.useAccent2 = ! useAccent2;
            }
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            x += vx * dt;
            const float left  = track.getX() + 3.0f;
            const float right = track.getRight() - 3.0f;
            if (x < left)
            {
                x = left;
                vx = std::abs (vx) * 0.84f;
                ++bounces;
                spawnWallSparks (x, +1.0f);
            }
            else if (x > right)
            {
                x = right;
                vx = -std::abs (vx) * 0.84f;
                ++bounces;
                spawnWallSparks (x, -1.0f);
            }
            vx *= std::exp (-0.55f * dt);

            for (int i = kTrailLen - 1; i > 0; --i)
                trailX[(size_t) i] = trailX[(size_t) i - 1];
            trailX[0] = x;
            trailCount = juce::jmin (kTrailLen, trailCount + 1);

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += dt;
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
            }

            if (age >= ttl || bounces >= 4)
                alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            if (fade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (track.expanded (2.0f, 4.0f).getSmallestIntegerContainer());

            const juce::Colour col = useAccent2 ? PinkXP::accent2 : PinkXP::sel;
            for (int i = 1; i < trailCount; ++i)
            {
                const float frac = 1.0f - (float) i / (float) kTrailLen;
                g.setColour (col.withAlpha (0.74f * frac * frac * fade));
                g.drawLine (trailX[(size_t) (i - 1)], y, trailX[(size_t) i], y, 0.8f + 1.6f * frac);
            }
            drawGlowDot (g, x, y, col, fade, 3.6f, 1.45f);

            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour sc = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (sc.withAlpha (0.85f * f * fade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override { return track.expanded (4.0f, 6.0f); }
    };

    // ------------------------------------------------------
    // 9) 指针弹拨虚影（Phase Corr Dial + VU 指针式共用）
    //    从点击角度（startTheta）以欠阻尼弹簧弹回真实指针角（restTheta），留 8 层荧光扇形残影
    // ------------------------------------------------------
    struct GhostNeedleActor : public ToyActor
    {
        static constexpr int kTrailLen = 8;

        juce::Point<float> pivot;
        float              radius = 20.0f;
        float              restTheta = kPi * 0.5f;
        float              startTheta = kPi * 0.5f;
        float              theta = kPi * 0.5f;
        std::array<float, kTrailLen> trail {};
        int                trailCount = 0;

        GhostNeedleActor (juce::Point<float> p, float r, float restTh, float startTh)
            : pivot (p), radius (juce::jmax (6.0f, r)),
              restTheta (clampFinite (restTh, 0.04f, kPi - 0.04f, kPi * 0.5f))
        {
            ttl = 1.35f;
            if (startTh >= 0.0f && std::isfinite (startTh))
                startTheta = juce::jlimit (0.04f, kPi - 0.04f, startTh);
            else
                startTheta = juce::jlimit (0.04f, kPi - 0.04f, restTheta + (restTheta > kPi * 0.5f ? -0.35f : 0.35f));

            // 若点得离当前真实指针非常近，补一个干脆的拨动初角
            if (std::abs (startTheta - restTheta) < 0.16f)
                startTheta = juce::jlimit (0.04f, kPi - 0.04f, restTheta + (restTheta > kPi * 0.5f ? -0.28f : 0.28f));

            theta = startTheta;
            trail.fill (theta);
            trailCount = 1;
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            theta = restTheta + (startTheta - restTheta) * std::exp (-4.6f * age) * std::cos (16.0f * age);
            theta = juce::jlimit (0.02f, kPi - 0.02f, theta);

            for (int i = kTrailLen - 1; i > 0; --i)
                trail[(size_t) i] = trail[(size_t) i - 1];
            trail[0] = theta;
            trailCount = juce::jmin (kTrailLen, trailCount + 1);

            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            if (fade <= 0.01f) return;

            auto tipAt = [&](float th, float r)
            {
                return juce::Point<float> (pivot.x - std::cos (th) * r,
                                           pivot.y - std::sin (th) * r);
            };

            // 8 层渐变荧光扇形残影
            for (int i = trailCount - 1; i >= 1; --i)
            {
                const float frac = 1.0f - (float) i / (float) kTrailLen;
                const auto tp = tipAt (trail[(size_t) i], radius);
                const juce::Colour c = (i % 2 == 0) ? PinkXP::sel : PinkXP::accent2;
                g.setColour (c.withAlpha (0.42f * frac * frac * fade));
                g.drawLine (pivot.x, pivot.y, tp.x, tp.y, 0.8f + 0.9f * frac);
            }

            // 指针主体 + 针尖亮核 + 针尖小弧迹
            const auto mainTip = tipAt (theta, radius);
            g.setColour (PinkXP::sel.withAlpha (0.88f * fade));
            g.drawLine (pivot.x, pivot.y, mainTip.x, mainTip.y, 1.6f);
            drawGlowDot (g, mainTip.x, mainTip.y, PinkXP::sel, fade, 3.6f, 1.45f);

            if (age < 0.30f)
            {
                const float t = age / 0.30f;
                const auto clickTip = tipAt (startTheta, radius);
                drawPulseRing (g, clickTip.x, clickTip.y, 2.5f + t * 12.0f, PinkXP::accent2, (1.0f - t) * 0.62f * fade, 1.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            const float pad = radius + 16.0f;
            return { pivot.x - pad, pivot.y - pad, pad * 2.0f, pad * 2.0f };
        }
    };

    // ------------------------------------------------------
    // 10) 纵向 LED 柱体（Loudness LED / Dynamics Peak-RMS）：荧光扫光柱 + 弹跳发光顶帽 + 微火花
    // ------------------------------------------------------
    struct MeterBarLaunchActor : public ToyActor
    {
        static constexpr int kMaxSparks = 6;
        static constexpr float kSweepSec = 0.18f;

        juce::Rectangle<float> bar;
        float                  targetY = 0.0f;
        juce::Colour           accent;
        float                  capY = 0.0f, capVy = 0.0f;
        bool                   launched = false;
        int                    bounces = 0;
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        MeterBarLaunchActor (juce::Rectangle<float> barRect, float clickY, juce::Colour c)
            : bar (barRect), accent (c)
        {
            ttl = 1.45f;
            if (bar.getWidth() < 4.0f || bar.getHeight() < 10.0f)
            {
                alive = false;
                return;
            }
            targetY = clampFinite (clickY, bar.getY() + 5.0f, bar.getBottom() - 4.0f, bar.getCentreY());
            capY = targetY;
        }

        void spawnCapSparks()
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < kMaxSparks && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = bar.getX() + rng.nextFloat() * bar.getWidth();
                sp.y = targetY;
                sp.vx = (rng.nextFloat() - 0.5f) * 90.0f;
                sp.vy = -(55.0f + rng.nextFloat() * 95.0f);
                sp.life = 0.0f;
                sp.maxLife = 0.28f + rng.nextFloat() * 0.20f;
                sp.useAccent2 = (k & 1) != 0;
            }
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            if (! launched && age >= kSweepSec)
            {
                launched = true;
                capY  = targetY;
                capVy = -juce::jlimit (120.0f, 240.0f, bar.getHeight() * 1.1f);
                spawnCapSparks();
            }
            if (launched && bounces < 3)
            {
                capVy += 780.0f * dt;
                capY  += capVy * dt;
                if (capY < bar.getY() + 2.0f)
                {
                    capY = bar.getY() + 2.0f;
                    capVy = std::abs (capVy) * 0.45f;
                }
                if (capY >= targetY)
                {
                    capY = targetY;
                    capVy = -capVy * 0.38f;
                    if (++bounces >= 3) capVy = 0.0f;
                }
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += dt;
                sp.vy += 420.0f * dt;
                sp.x  += sp.vx * dt;
                sp.y  += sp.vy * dt;
            }

            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            if (fade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (bar.expanded (12.0f, 12.0f).getSmallestIntegerContainer());

            const float sweepT = juce::jlimit (0.0f, 1.0f, age / kSweepSec);
            const float beamTop = bar.getBottom() - (bar.getBottom() - targetY) * (1.0f - (1.0f - sweepT) * (1.0f - sweepT));
            const float beamFade = (age < kSweepSec) ? 1.0f : std::exp (-4.5f * (age - kSweepSec));

            if (beamFade > 0.02f)
            {
                // 半透明自底向上荧光扫光柱（不挡底层 LED 格子）
                const juce::Rectangle<float> beam (bar.getX() + 1.0f, beamTop,
                                                   juce::jmax (1.0f, bar.getWidth() - 2.0f),
                                                   juce::jmax (1.0f, bar.getBottom() - beamTop));
                g.setColour (accent.withAlpha (0.24f * beamFade * fade));
                g.fillRect (beam);
                g.setColour (accent.withAlpha (0.72f * beamFade * fade));
                g.drawLine (bar.getX() + 1.0f, beamTop, bar.getRight() - 1.0f, beamTop, 1.4f);
            }

            // 发光弹跳顶帽（3px 亮边 + 中心亮丝）
            if (launched)
            {
                const juce::Rectangle<float> cap (bar.getX() + 1.0f, capY - 1.5f,
                                                  juce::jmax (2.0f, bar.getWidth() - 2.0f), 3.0f);
                g.setColour (accent.withAlpha (0.55f * fade));
                g.fillRect (cap.expanded (1.0f, 1.0f));
                g.setColour (PinkXP::hl.withAlpha (0.95f * fade));
                g.fillRect (cap.getX(), capY - 0.6f, cap.getWidth(), 1.2f);
            }

            // 柱色微火花
            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                g.setColour ((sp.useAccent2 ? PinkXP::hl : accent).withAlpha (0.85f * f * fade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override { return bar.expanded (16.0f, 16.0f); }
    };

    // ------------------------------------------------------
    // 11) 数字读数框（Loudness Readouts）：双向绕框霓虹光束（零遮挡中央数字）
    // ------------------------------------------------------
    struct PerimeterSweepActor : public ToyActor
    {
        static constexpr int kTrailLen  = 14;
        static constexpr int kMaxSparks = 6;

        juce::Rectangle<float> box;
        juce::Colour           accent;
        float                  perim = 100.0f;
        float                  s0 = 0.0f;
        float                  distTraveled = 0.0f;
        bool                   collided = false;
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        PerimeterSweepActor (juce::Rectangle<float> boxRect, juce::Point<float> clickPos, juce::Colour c)
            : box (boxRect.reduced (1.5f)), accent (c)
        {
            ttl = 1.05f;
            if (box.getWidth() < 10.0f || box.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }
            perim = 2.0f * (box.getWidth() + box.getHeight());

            // 把点击位置投影到框体最近的边上作为双光束起点 s0
            const float dTop = std::abs (clickPos.y - box.getY());
            const float dBot = std::abs (clickPos.y - box.getBottom());
            const float dLft = std::abs (clickPos.x - box.getX());
            const float dRgt = std::abs (clickPos.x - box.getRight());
            const float minD = juce::jmin (juce::jmin (dTop, dBot), juce::jmin (dLft, dRgt));
            const float cx = clampFinite (clickPos.x, box.getX(), box.getRight(), box.getCentreX());
            const float cy = clampFinite (clickPos.y, box.getY(), box.getBottom(), box.getCentreY());

            if (minD == dTop)      s0 = cx - box.getX();
            else if (minD == dRgt) s0 = box.getWidth() + (cy - box.getY());
            else if (minD == dBot) s0 = box.getWidth() + box.getHeight() + (box.getRight() - cx);
            else                   s0 = 2.0f * box.getWidth() + box.getHeight() + (box.getBottom() - cy);
        }

        juce::Point<float> pointAtS (float s) const noexcept
        {
            float m = std::fmod (s, perim);
            if (m < 0.0f) m += perim;
            const float w = box.getWidth(), h = box.getHeight();
            if (m <= w)             return { box.getX() + m, box.getY() };
            if (m <= w + h)         return { box.getRight(), box.getY() + (m - w) };
            if (m <= 2.0f * w + h)  return { box.getRight() - (m - w - h), box.getBottom() };
            return { box.getX(), box.getBottom() - (m - 2.0f * w - h) };
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            const float halfPerim = perim * 0.5f;
            const float speed = halfPerim / 0.52f;
            distTraveled = juce::jmin (halfPerim, distTraveled + speed * dt);

            if (! collided && distTraveled >= halfPerim - 0.5f)
            {
                collided = true;
                const auto meet = pointAtS (s0 + halfPerim);
                auto& rng = juce::Random::getSystemRandom();
                for (int k = 0; k < 5 && sparkCount < kMaxSparks; ++k)
                {
                    auto& sp = sparks[(size_t) sparkCount++];
                    sp.x = meet.x;
                    sp.y = meet.y;
                    const float ang = rng.nextFloat() * kTwoPi;
                    const float spd = 30.0f + rng.nextFloat() * 55.0f;
                    sp.vx = std::cos (ang) * spd;
                    sp.vy = std::sin (ang) * spd;
                    sp.life = 0.0f;
                    sp.maxLife = 0.25f + rng.nextFloat() * 0.15f;
                    sp.useAccent2 = (k & 1) != 0;
                }
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += dt;
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
            }

            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            if (fade <= 0.01f) return;

            const float trailArc = juce::jmin (perim * 0.24f, 68.0f);
            for (int dirSign = -1; dirSign <= 1; dirSign += 2)
            {
                const juce::Colour col = (dirSign > 0) ? accent : PinkXP::accent2;
                const float headS = s0 + (float) dirSign * distTraveled;
                for (int i = 0; i < kTrailLen; ++i)
                {
                    const float f0 = (float) i / (float) kTrailLen;
                    const float f1 = (float) (i + 1) / (float) kTrailLen;
                    const float d0 = distTraveled - f0 * trailArc;
                    const float d1 = distTraveled - f1 * trailArc;
                    if (d0 <= 0.0f) break;
                    const auto p0 = pointAtS (s0 + (float) dirSign * juce::jmax (0.0f, d0));
                    const auto p1 = pointAtS (s0 + (float) dirSign * juce::jmax (0.0f, d1));
                    const float frac = 1.0f - f0;
                    g.setColour (col.withAlpha (0.78f * frac * frac * fade));
                    g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.8f + 1.3f * frac);
                }
                if (! collided)
                {
                    const auto hp = pointAtS (headS);
                    drawGlowDot (g, hp.x, hp.y, col, fade, 3.3f, 1.35f);
                }
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                g.setColour ((sp.useAccent2 ? PinkXP::hl : accent).withAlpha (0.85f * f * fade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override { return box.expanded (10.0f); }
    };

    // ------------------------------------------------------
    // 12) Dynamics DR 框：左右荧光压缩括号 [ ] 弹性钳位（零遮挡中央数字）
    // ------------------------------------------------------
    struct CompressorClampActor : public ToyActor
    {
        static constexpr int kMaxSparks = 8;

        juce::Rectangle<float> cell;
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        CompressorClampActor (juce::Rectangle<float> cellRect, juce::Point<float>)
            : cell (cellRect.reduced (2.0f))
        {
            ttl = 1.05f;
            if (cell.getWidth() < 16.0f || cell.getHeight() < 12.0f)
            {
                alive = false;
                return;
            }
            auto& rng = juce::Random::getSystemRandom();
            const float xs[2] = { cell.getX() + 4.0f, cell.getRight() - 4.0f };
            const float ys[2] = { cell.getY() + 4.0f, cell.getBottom() - 4.0f };
            for (int sx = 0; sx < 2; ++sx)
            {
                for (int sy = 0; sy < 2; ++sy)
                {
                    auto& sp = sparks[(size_t) sparkCount++];
                    sp.x = xs[sx];
                    sp.y = ys[sy];
                    sp.vx = (sx == 0 ? 1.0f : -1.0f) * (25.0f + rng.nextFloat() * 35.0f);
                    sp.vy = (sy == 0 ? 1.0f : -1.0f) * (15.0f + rng.nextFloat() * 25.0f);
                    sp.life = 0.0f;
                    sp.maxLife = 0.28f + rng.nextFloat() * 0.16f;
                    sp.useAccent2 = (sx != sy);
                }
            }
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += dt;
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
            }
            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            if (fade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (cell.expanded (4.0f).getSmallestIntegerContainer());

            // 模拟压缩器 Gain Reduction 钳位：快速向内夹紧 16% 再阻尼回弹
            const float clampEnv = std::exp (-4.6f * age) * std::sin (18.5f * age);
            const float maxInset = juce::jmin (16.0f, cell.getWidth() * 0.17f);
            const float inset = juce::jmax (0.0f, maxInset * (0.35f * std::exp (-3.0f * age) + 0.65f * clampEnv));

            const float y0 = cell.getY() + 3.0f;
            const float y1 = cell.getBottom() - 3.0f;
            const float lx = cell.getX() + 2.0f + inset;
            const float rx = cell.getRight() - 2.0f - inset;
            const float hook = juce::jmin (8.0f, cell.getWidth() * 0.09f);

            // 左括号 [
            g.setColour (PinkXP::sel.withAlpha (0.85f * fade));
            g.drawLine (lx + hook, y0, lx, y0, 1.5f);
            g.drawLine (lx, y0, lx, y1, 1.5f);
            g.drawLine (lx, y1, lx + hook, y1, 1.5f);

            // 右括号 ]
            g.setColour (PinkXP::accent2.withAlpha (0.85f * fade));
            g.drawLine (rx - hook, y0, rx, y0, 1.5f);
            g.drawLine (rx, y0, rx, y1, 1.5f);
            g.drawLine (rx, y1, rx - hook, y1, 1.5f);

            // 中央外侧极细压限指示点
            drawGlowDot (g, lx, (y0 + y1) * 0.5f, PinkXP::sel,     fade, 3.0f, 1.25f);
            drawGlowDot (g, rx, (y0 + y1) * 0.5f, PinkXP::accent2, fade, 3.0f, 1.25f);

            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                g.setColour ((sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel).withAlpha (0.80f * f * fade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override { return cell.expanded (6.0f); }
    };

    // ------------------------------------------------------
    // 13) Dynamics Crest 历史带：沿真实 Crest 折线向两侧冲浪的双荧光光珠
    // ------------------------------------------------------
    struct CrestSurferActor : public ToyActor
    {
        static constexpr int kProfileBins = 96;
        static constexpr int kTrailLen    = 12;
        static constexpr int kMaxSparks   = 8;

        struct Rider
        {
            float x = 0.0f, y = 0.0f, vx = 0.0f;
            bool  finished = false;
            bool  useAccent2 = false;
            std::array<juce::Point<float>, kTrailLen> trail {};
            int   trailCount = 0;
        };

        juce::Rectangle<int>   plot;
        juce::Rectangle<float> inner;
        juce::Point<float>     origin;
        std::array<float, kProfileBins> curveY {};
        std::array<Rider, 2>   riders {};
        std::array<MicroSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        CrestSurferActor (juce::Point<float> clickPos,
                          juce::Rectangle<int> plotRect,
                          const std::vector<float>& profileY)
            : plot (plotRect),
              inner (plotRect.reduced (2).toFloat())
        {
            ttl = 1.50f;
            if (inner.getWidth() < 8.0f || inner.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }
            const float cy = inner.getCentreY();
            const int srcN = (int) profileY.size();
            for (int i = 0; i < kProfileBins; ++i)
            {
                float y = cy;
                if (srcN > 1)
                {
                    const float f = (float) i / (float) (kProfileBins - 1) * (float) (srcN - 1);
                    const int i0 = juce::jlimit (0, srcN - 1, (int) f);
                    const int i1 = juce::jmin (srcN - 1, i0 + 1);
                    const float a = clampFinite (profileY[(size_t) i0], inner.getY(), inner.getBottom(), cy);
                    const float b = clampFinite (profileY[(size_t) i1], inner.getY(), inner.getBottom(), cy);
                    y = a + (b - a) * (f - (float) i0);
                }
                curveY[(size_t) i] = y;
            }

            const float sx = clampFinite (clickPos.x, inner.getX() + 2.0f, inner.getRight() - 2.0f, inner.getCentreX());
            const float sy = sampleY (sx);
            origin = { sx, sy };

            const float spd = juce::jlimit (140.0f, 360.0f, inner.getWidth() * 0.85f);
            for (int i = 0; i < 2; ++i)
            {
                auto& rd = riders[(size_t) i];
                rd.x = sx;
                rd.y = sy;
                rd.vx = (i == 0 ? -1.0f : 1.0f) * spd;
                rd.useAccent2 = (i == 1);
                rd.trail.fill ({ sx, sy });
                rd.trailCount = 1;
            }
        }

        float sampleY (float x) const noexcept
        {
            if (inner.getWidth() <= 1.0f) return inner.getCentreY();
            const float u = juce::jlimit (0.0f, 1.0f, (x - inner.getX()) / inner.getWidth());
            const float f = u * (float) (kProfileBins - 1);
            const int i0 = juce::jlimit (0, kProfileBins - 1, (int) f);
            const int i1 = juce::jmin (kProfileBins - 1, i0 + 1);
            return curveY[(size_t) i0] + (curveY[(size_t) i1] - curveY[(size_t) i0]) * (f - (float) i0);
        }

        void spawnEdgeSparks (float ex, float ey, float nx, bool accent2)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 4 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = ex;
                sp.y = ey;
                const float spd = 35.0f + rng.nextFloat() * 65.0f;
                const float ang = (rng.nextFloat() - 0.5f) * 1.8f;
                sp.vx = nx * (0.45f + 0.55f * std::abs (std::cos (ang))) * spd;
                sp.vy = std::sin (ang) * spd - 20.0f;
                sp.life = 0.0f;
                sp.maxLife = 0.24f + rng.nextFloat() * 0.16f;
                sp.useAccent2 = accent2;
            }
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            bool anyActive = (age < 0.35f);
            for (auto& rd : riders)
            {
                if (! rd.finished)
                {
                    anyActive = true;
                    rd.x += rd.vx * dt;
                    if (rd.x <= inner.getX() + 1.0f)
                    {
                        rd.x = inner.getX() + 1.0f;
                        rd.y = sampleY (rd.x);
                        rd.finished = true;
                        spawnEdgeSparks (rd.x, rd.y, +1.0f, rd.useAccent2);
                    }
                    else if (rd.x >= inner.getRight() - 1.0f)
                    {
                        rd.x = inner.getRight() - 1.0f;
                        rd.y = sampleY (rd.x);
                        rd.finished = true;
                        spawnEdgeSparks (rd.x, rd.y, -1.0f, rd.useAccent2);
                    }
                    else
                    {
                        rd.y = sampleY (rd.x);
                    }
                    for (int i = kTrailLen - 1; i > 0; --i)
                        rd.trail[(size_t) i] = rd.trail[(size_t) i - 1];
                    rd.trail[0] = { rd.x, rd.y };
                    rd.trailCount = juce::jmin (kTrailLen, rd.trailCount + 1);
                }
                else if (rd.trailCount > 1)
                {
                    anyActive = true;
                    --rd.trailCount;
                }
            }
            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                anyActive = true;
                sp.life += dt;
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
            }
            if (age >= ttl || ! anyActive) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.30f)) / 0.30f);
            if (fade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (plot);

            if (age < 0.34f)
            {
                const float t = age / 0.34f;
                drawPulseRing (g, origin.x, origin.y, 2.5f + t * 16.0f, PinkXP::sel, (1.0f - t) * 0.65f * fade, 1.05f);
            }

            for (const auto& rd : riders)
            {
                const juce::Colour col = rd.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                for (int i = 1; i < rd.trailCount; ++i)
                {
                    const float frac = 1.0f - (float) i / (float) kTrailLen;
                    g.setColour (col.withAlpha (0.72f * frac * frac * fade));
                    g.drawLine (rd.trail[(size_t) (i - 1)].x, rd.trail[(size_t) (i - 1)].y,
                                rd.trail[(size_t) i].x,       rd.trail[(size_t) i].y, 0.7f + 1.25f * frac);
                }
                if (! rd.finished)
                    drawGlowDot (g, rd.x, rd.y, col, fade, 3.4f, 1.35f);
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                g.setColour ((sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel).withAlpha (0.82f * f * fade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            if (age < 0.34f)
                b.includePoint (origin.x, origin.y, 20.0f);
            for (const auto& rd : riders)
            {
                for (int i = 0; i < rd.trailCount; ++i)
                    b.includePoint (rd.trail[(size_t) i].x, rd.trail[(size_t) i].y, 4.0f);
                if (! rd.finished)
                    b.includePoint (rd.x, rd.y, 4.5f);
            }
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (plot, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 15) TempoKey BPM 列：真实 BPM 周期节拍脉冲环 + 底边摆动节拍光点
    // ------------------------------------------------------
    struct BpmMetronomePulseActor : public ToyActor
    {
        juce::Rectangle<int>   col;
        juce::Rectangle<float> inner;
        juce::Point<float>     center;
        float                  beatSec = 0.5f;

        BpmMetronomePulseActor (juce::Rectangle<int> colRect, juce::Point<float>, float bpm)
            : col (colRect), inner (colRect.reduced (4).toFloat()), center (inner.getCentre())
        {
            if (inner.getWidth() < 12.0f || inner.getHeight() < 12.0f)
            {
                alive = false;
                return;
            }
            const float validBpm = (std::isfinite (bpm) && bpm >= 30.0f && bpm <= 300.0f) ? bpm : 120.0f;
            beatSec = 60.0f / validBpm;
            // 播满 3 个真实节拍周期（最长钳制到 2.8s）
            ttl = juce::jlimit (1.1f, 2.8f, beatSec * 3.0f);
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.35f)) / 0.35f);
            if (endFade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (col);

            const float maxR = juce::jmin (inner.getWidth(), inner.getHeight()) * 0.46f;
            // 3 拍节奏脉冲环（第 1 拍重拍主色，第 2/3 拍副色）
            for (int b = 0; b < 3; ++b)
            {
                const float bt = (age - (float) b * beatSec) / juce::jmax (0.28f, beatSec * 0.92f);
                if (bt < 0.0f || bt >= 1.0f) continue;
                const float r = 6.0f + bt * maxR;
                const float a = (1.0f - bt) * (b == 0 ? 0.62f : 0.44f) * endFade;
                drawPulseRing (g, center.x, center.y, r, b == 0 ? PinkXP::sel : PinkXP::accent2, a, b == 0 ? 1.3f : 1.0f);
            }

            // 底边水平节拍器光轨 + 同步摆动亮珠（完全不遮挡中间 BPM 大字）
            const float barY = inner.getBottom() - 2.0f;
            const float x0 = inner.getX() + 6.0f;
            const float x1 = inner.getRight() - 6.0f;
            if (x1 - x0 > 12.0f)
            {
                g.setColour (PinkXP::sel.withAlpha (0.28f * endFade));
                g.drawLine (x0, barY, x1, barY, 1.0f);
                const float u = 0.5f - 0.5f * std::cos (kPi * age / beatSec);
                const float px = x0 + (x1 - x0) * u;
                drawGlowDot (g, px, barY, PinkXP::sel, endFade, 3.6f, 1.5f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            const float maxR = juce::jmin (inner.getWidth(), inner.getHeight()) * 0.46f;
            for (int i = 0; i < 3; ++i)
            {
                const float bt = (age - (float) i * beatSec) / juce::jmax (0.28f, beatSec * 0.92f);
                if (bt >= 0.0f && bt < 1.0f)
                    b.includePoint (center.x, center.y, 6.0f + bt * maxR + 2.0f);
            }
            const float barY = inner.getBottom() - 2.0f;
            b.includeRect (inner.getX() + 4.0f, barY - 6.0f, inner.getRight() - 4.0f, barY + 6.0f);
            return b.finish (col, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 16) TempoKey KEY 列：12 音高星座环（点亮当前调式真实 7 个音级 + 细星轨连线）
    // ------------------------------------------------------
    struct KeyConstellationActor : public ToyActor
    {
        juce::Rectangle<int>   col;
        juce::Rectangle<float> inner;
        juce::Point<float>     center;
        float                  ringR = 24.0f;
        int                    rootPc = -1;
        std::array<int, 7>     scalePcs {};
        int                    scaleCount = 0;

        KeyConstellationActor (juce::Rectangle<int> colRect, juce::Point<float>, int keyRoot, int keyMode)
            : col (colRect), inner (colRect.reduced (4).toFloat()), center (inner.getCentre())
        {
            ttl = 1.90f;
            if (inner.getWidth() < 16.0f || inner.getHeight() < 16.0f)
            {
                alive = false;
                return;
            }
            ringR = juce::jmin (inner.getWidth(), inner.getHeight()) * 0.39f;

            if (keyRoot >= 0 && keyRoot < 12)
            {
                rootPc = keyRoot;
                static constexpr int kMajor[7]    = { 0, 2, 4, 5, 7, 9, 11 };
                static constexpr int kNatMin[7]   = { 0, 2, 3, 5, 7, 8, 10 };
                static constexpr int kHarmMin[7]  = { 0, 2, 3, 5, 7, 8, 11 };
                static constexpr int kMelMin[7]   = { 0, 2, 3, 5, 7, 9, 11 };
                static constexpr int kMixo[7]     = { 0, 2, 4, 5, 7, 9, 10 };
                const int* steps = (keyMode == 1) ? kNatMin
                                 : (keyMode == 2) ? kHarmMin
                                 : (keyMode == 3) ? kMelMin
                                 : (keyMode == 4) ? kMixo
                                                  : kMajor;
                scaleCount = 7;
                for (int i = 0; i < 7; ++i)
                    scalePcs[(size_t) i] = (keyRoot + steps[i]) % 12;
            }
        }

        juce::Point<float> pcPoint (int pc) const noexcept
        {
            const float ang = -kPi * 0.5f + (float) pc * (kTwoPi / 12.0f);
            return { center.x + std::cos (ang) * ringR, center.y + std::sin (ang) * ringR };
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.45f)) / 0.45f);
            if (fade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (col);

            // 12 音高细环底轨（极低透明度，不遮挡中央调名文字）
            drawPulseRing (g, center.x, center.y, ringR, PinkXP::sel, 0.22f * fade, 0.9f);
            for (int pc = 0; pc < 12; ++pc)
            {
                const auto pt = pcPoint (pc);
                g.setColour (PinkXP::sel.withAlpha (0.24f * fade));
                g.fillEllipse (pt.x - 1.0f, pt.y - 1.0f, 2.0f, 2.0f);
            }

            if (scaleCount == 7)
            {
                // 逐级点亮 7 个音级星点并用极细几何星轨连接
                const float reveal = juce::jlimit (0.0f, 7.0f, age / 0.065f);
                const int litN = juce::jmin (7, (int) std::floor (reveal) + 1);

                for (int i = 0; i < litN - 1; ++i)
                {
                    const auto p0 = pcPoint (scalePcs[(size_t) i]);
                    const auto p1 = pcPoint (scalePcs[(size_t) (i + 1)]);
                    g.setColour (PinkXP::accent2.withAlpha (0.45f * fade));
                    g.drawLine (p0.x, p0.y, p1.x, p1.y, 1.0f);
                }
                if (litN == 7)
                {
                    const auto pLast  = pcPoint (scalePcs[6]);
                    const auto pFirst = pcPoint (scalePcs[0]);
                    g.setColour (PinkXP::accent2.withAlpha (0.40f * fade));
                    g.drawLine (pLast.x, pLast.y, pFirst.x, pFirst.y, 1.0f);
                }

                for (int i = 0; i < litN; ++i)
                {
                    const auto pt = pcPoint (scalePcs[(size_t) i]);
                    const bool isTonic = (i == 0);
                    drawGlowDot (g, pt.x, pt.y, isTonic ? PinkXP::sel : PinkXP::accent2, fade,
                                 isTonic ? 4.2f : 3.0f, isTonic ? 1.6f : 1.2f);
                    if (isTonic && age < 0.50f)
                    {
                        const float rt = age / 0.50f;
                        drawPulseRing (g, pt.x, pt.y, 3.0f + rt * 11.0f, PinkXP::sel, (1.0f - rt) * 0.65f * fade, 1.0f);
                    }
                }
            }
            else
            {
                // 尚未识别调性时：12 半音顺序扫描光点
                const int activePc = ((int) (age * 14.0f)) % 12;
                const auto pt = pcPoint (activePc);
                drawGlowDot (g, pt.x, pt.y, PinkXP::accent2, fade, 3.6f, 1.4f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            b.includePoint (center.x, center.y, ringR + 15.0f);
            return b.finish (col, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 17) SystemMonitor：PCB 电路板 90°/45° 信号走线脉冲 + 发光焊盘端点
    // ------------------------------------------------------
    struct SysCircuitPulseActor : public ToyActor
    {
        static constexpr int kNumTraces = 4;

        struct Trace
        {
            std::array<juce::Point<float>, 3> pts {};   // 起点 → 45°/90° 拐点 → 焊盘终点
            float totalLen = 1.0f;
            float seg1Len  = 0.5f;
            bool  useAccent2 = false;
        };

        juce::Rectangle<int>   tile;
        juce::Rectangle<float> inner;
        juce::Point<float>     origin;
        juce::Colour           accent;
        std::array<Trace, kNumTraces> traces {};

        SysCircuitPulseActor (juce::Point<float> clickPos,
                              juce::Rectangle<int> tileRect,
                              juce::Colour accentColour)
            : tile (tileRect),
              inner (tileRect.reduced (4).toFloat()),
              accent (accentColour)
        {
            ttl = 1.30f;
            if (inner.getWidth() < 12.0f || inner.getHeight() < 12.0f)
            {
                alive = false;
                return;
            }
            origin.x = clampFinite (clickPos.x, inner.getX() + 2.0f, inner.getRight() - 2.0f, inner.getCentreX());
            origin.y = clampFinite (clickPos.y, inner.getY() + 2.0f, inner.getBottom() - 2.0f, inner.getCentreY());

            auto& rng = juce::Random::getSystemRandom();
            const float baseSpan = juce::jlimit (18.0f, 64.0f, juce::jmin (inner.getWidth(), inner.getHeight()) * 0.45f);
            const float qx[4] = {  1.0f, -1.0f, -1.0f,  1.0f };
            const float qy[4] = { -1.0f, -1.0f,  1.0f,  1.0f };

            for (int i = 0; i < kNumTraces; ++i)
            {
                auto& tr = traces[(size_t) i];
                const float len1 = baseSpan * (0.38f + rng.nextFloat() * 0.35f);
                const float len2 = baseSpan * (0.35f + rng.nextFloat() * 0.45f);
                const bool diagFirst = ((i & 1) == 0);

                juce::Point<float> p1 = origin;
                juce::Point<float> p2 = origin;
                if (diagFirst)
                {
                    p1 = { origin.x + qx[i] * len1 * 0.707f, origin.y + qy[i] * len1 * 0.707f };
                    p2 = { p1.x + qx[i] * len2, p1.y };
                }
                else
                {
                    p1 = { origin.x, origin.y + qy[i] * len1 };
                    p2 = { p1.x + qx[i] * len2 * 0.707f, p1.y + qy[i] * len2 * 0.707f };
                }

                p1.x = juce::jlimit (inner.getX() + 2.0f, inner.getRight() - 2.0f, p1.x);
                p1.y = juce::jlimit (inner.getY() + 2.0f, inner.getBottom() - 2.0f, p1.y);
                p2.x = juce::jlimit (inner.getX() + 2.0f, inner.getRight() - 2.0f, p2.x);
                p2.y = juce::jlimit (inner.getY() + 2.0f, inner.getBottom() - 2.0f, p2.y);

                tr.pts[0] = origin;
                tr.pts[1] = p1;
                tr.pts[2] = p2;
                tr.seg1Len = origin.getDistanceFrom (p1);
                tr.totalLen = juce::jmax (1.0f, tr.seg1Len + p1.getDistanceFrom (p2));
                tr.useAccent2 = (i & 1) != 0;
            }
        }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.40f)) / 0.40f);
            if (fade <= 0.01f) return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (tile);

            constexpr float kTravelSec = 0.36f;
            const float prog = juce::jlimit (0.0f, 1.0f, age / kTravelSec);
            const float ease = 1.0f - (1.0f - prog) * (1.0f - prog);

            // 起点过孔小环
            drawPulseRing (g, origin.x, origin.y, 2.5f + juce::jmin (1.0f, age / 0.30f) * 8.0f,
                           accent, (1.0f - juce::jmin (1.0f, age / 0.30f)) * 0.55f * fade, 1.0f);
            g.setColour (accent.withAlpha (0.65f * fade));
            g.drawEllipse (origin.x - 2.2f, origin.y - 2.2f, 4.4f, 4.4f, 1.0f);

            for (const auto& tr : traces)
            {
                const juce::Colour col = tr.useAccent2 ? PinkXP::accent2 : accent;
                const float d = tr.totalLen * ease;

                juce::Point<float> head;
                g.setColour (col.withAlpha (0.68f * fade));
                if (d <= tr.seg1Len && tr.seg1Len > 0.5f)
                {
                    const float u = d / tr.seg1Len;
                    head = tr.pts[0] + (tr.pts[1] - tr.pts[0]) * u;
                    g.drawLine (tr.pts[0].x, tr.pts[0].y, head.x, head.y, 1.15f);
                }
                else
                {
                    g.drawLine (tr.pts[0].x, tr.pts[0].y, tr.pts[1].x, tr.pts[1].y, 1.15f);
                    const float seg2 = juce::jmax (0.5f, tr.totalLen - tr.seg1Len);
                    const float u = juce::jlimit (0.0f, 1.0f, (d - tr.seg1Len) / seg2);
                    head = tr.pts[1] + (tr.pts[2] - tr.pts[1]) * u;
                    g.drawLine (tr.pts[1].x, tr.pts[1].y, head.x, head.y, 1.15f);
                }

                if (prog < 1.0f)
                {
                    drawGlowDot (g, head.x, head.y, col, fade, 3.2f, 1.3f);
                }
                else
                {
                    // 抵达焊盘终点：点亮空心焊盘圆 + 微扩散环
                    const float padT = juce::jlimit (0.0f, 1.0f, (age - kTravelSec) / 0.40f);
                    const auto& endPt = tr.pts[2];
                    drawPulseRing (g, endPt.x, endPt.y, 2.5f + padT * 7.0f, col, (1.0f - padT) * 0.65f * fade, 1.0f);
                    g.setColour (col.withAlpha (0.85f * fade));
                    g.drawEllipse (endPt.x - 2.3f, endPt.y - 2.3f, 4.6f, 4.6f, 1.1f);
                    g.setColour (PinkXP::hl.withAlpha (0.90f * fade));
                    g.fillEllipse (endPt.x - 1.0f, endPt.y - 1.0f, 2.0f, 2.0f);
                }
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            BoundsAccum b;
            b.includePoint (origin.x, origin.y, 12.0f);
            for (const auto& tr : traces)
            {
                b.includePoint (tr.pts[0].x, tr.pts[0].y, 4.0f);
                b.includePoint (tr.pts[1].x, tr.pts[1].y, 4.0f);
                b.includePoint (tr.pts[2].x, tr.pts[2].y, 11.0f);
            }
            return b.finish (tile, 8.0f);
        }
    };

    // ======================================================
    // VU 专属玩法（保留 4 套备受好评的横向表盘物理 + 迪斯科灯，统一荧光质感）
    // ======================================================

    // ---- VU 削波灯：霓虹光晕脉冲环 ----
    struct DiscoLedActor : public ToyActor
    {
        juce::Rectangle<float> led;
        explicit DiscoLedActor (juce::Rectangle<float> l) : led (l) { ttl = 2.4f; }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const juce::Colour kColours[] = {
                ToyPalette::ok(), ToyPalette::warn(), ToyPalette::danger(), ToyPalette::accent()
            };
            const int ci = ((int) (age * 3.0f)) % 4;
            const juce::Colour c = kColours[ci];
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.7f) / 0.7f);

            const float cx = led.getCentreX(), cy = led.getCentreY();
            // 柔和外晕 + LED 本体变色 + 中心高光点
            g.setColour (c.withAlpha (0.28f * fade));
            g.fillEllipse (led.expanded (3.0f));
            g.setColour (c.withAlpha (0.88f * fade));
            g.fillEllipse (led);
            g.setColour (PinkXP::hl.withAlpha (0.90f * fade));
            g.fillEllipse (cx - 1.4f, cy - 1.4f, 2.8f, 2.8f);

            for (int i = 0; i < 3; ++i)
            {
                const float phase = std::fmod (age * 1.35f + (float) i / 3.0f, 1.0f);
                const float r = juce::jmax (4.0f, led.getWidth() * 0.5f) + phase * 22.0f;
                drawPulseRing (g, cx, cy, r, c, fade * (1.0f - phase) * 0.58f, 1.2f);
            }
        }
        juce::Rectangle<float> getBounds() const override { return led.expanded (28.0f); }
    };

    // ---- VU 横向刻度条：荧光弹簧游标 ----
    struct SliderFlickActor : public ToyActor
    {
        float y0, y1, fromX, restX;
        std::array<float, 10> trail {};
        int   trailCount = 0;
        float x = 0.0f;

        SliderFlickActor (float top, float bottom, float clickX, float rest)
            : y0 (top), y1 (bottom), fromX (clickX), restX (rest), x (clickX)
        {
            ttl = 1.45f;
            trail.fill (clickX);
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            x = restX + (fromX - restX) * std::exp (-4.2f * age) * std::cos (13.0f * age);
            for (int i = (int) trail.size() - 1; i > 0; --i) trail[(size_t) i] = trail[(size_t) i - 1];
            trail[0] = x;
            trailCount = juce::jmin ((int) trail.size(), trailCount + 1);
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.05f) / 0.40f);
            const float mid  = (y0 + y1) * 0.5f;

            // 荧光弹簧丝
            const float span = x - restX;
            if (std::abs (span) > 3.0f)
            {
                const int   n   = juce::jlimit (4, 24, (int) (std::abs (span) / 6.0f));
                const float amp = (y1 - y0) * 0.20f;
                juce::Path spring;
                spring.startNewSubPath (restX, mid);
                for (int i = 1; i < n; ++i)
                    spring.lineTo (restX + span * (float) i / (float) n, mid + ((i & 1) ? -amp : amp));
                spring.lineTo (x, mid);
                g.setColour (PinkXP::accent2.withAlpha (0.62f * fade));
                g.strokePath (spring, juce::PathStrokeType (1.15f));
            }

            // 渐隐荧光残影
            for (int i = trailCount - 1; i >= 1; --i)
            {
                const float frac = 1.0f - (float) i / (float) trail.size();
                g.setColour (PinkXP::sel.withAlpha (0.38f * frac * frac * fade));
                g.fillRect (juce::Rectangle<float> (trail[(size_t) i] - 1.5f, y0, 3.0f, y1 - y0));
            }

            // 荧光游标本体（柔晕 + 亮核丝）
            g.setColour (PinkXP::sel.withAlpha (0.38f * fade));
            g.fillRect (juce::Rectangle<float> (x - 4.0f, y0 - 2.0f, 8.0f, y1 - y0 + 4.0f));
            g.setColour (PinkXP::sel.withAlpha (0.90f * fade));
            g.fillRect (juce::Rectangle<float> (x - 1.5f, y0 - 2.0f, 3.0f, y1 - y0 + 4.0f));
            g.setColour (PinkXP::hl.withAlpha (0.95f * fade));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, y0 - 1.0f, 1.0f, y1 - y0 + 2.0f));
        }
        juce::Rectangle<float> getBounds() const override
        {
            const float l = juce::jmin (fromX, restX) - std::abs (fromX - restX) - 10.0f;
            const float w = std::abs (fromX - restX) * 3.0f + 20.0f;
            return { l, y0 - 8.0f, w, y1 - y0 + 16.0f };
        }
    };

    // ---- VU LED 梯柱：跑马冲顶 + 顶帽弹飞溅微火花 ----
    struct LedChaseActor : public ToyActor
    {
        static constexpr int kMaxSparks = 6;
        static constexpr float sweepSec = 0.28f;

        std::vector<juce::Rectangle<float>> cols;
        std::vector<juce::Colour>           colours;
        float dir;
        float capY = 0.0f, capV = 0.0f, capRest = 0.0f;
        bool  launched = false;
        int   bounces  = 0;
        std::array<MicroSpark, kMaxSparks> sparks {};
        int   sparkCount = 0;

        LedChaseActor (std::vector<juce::Rectangle<float>> c, std::vector<juce::Colour> col, float d)
            : cols (std::move (c)), colours (std::move (col)), dir (d)
        {
            ttl = 1.80f;
            if (! cols.empty())
            {
                const auto& top = cols.back();
                capRest = (dir < 0.0f) ? top.getY() - 5.0f : top.getBottom() + 1.0f;
                capY    = capRest;
            }
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            if (! launched && age >= sweepSec)
            {
                launched = true;
                capV = dir * 240.0f;
                if (! cols.empty())
                {
                    const auto& top = cols.back();
                    auto& rng = juce::Random::getSystemRandom();
                    for (int k = 0; k < 4 && sparkCount < kMaxSparks; ++k)
                    {
                        auto& sp = sparks[(size_t) sparkCount++];
                        sp.x = top.getCentreX();
                        sp.y = capRest;
                        sp.vx = (rng.nextFloat() - 0.5f) * 90.0f;
                        sp.vy = dir * (60.0f + rng.nextFloat() * 80.0f);
                        sp.life = 0.0f;
                        sp.maxLife = 0.26f + rng.nextFloat() * 0.16f;
                        sp.useAccent2 = (k & 1) != 0;
                    }
                }
            }
            if (launched && bounces < 3)
            {
                capV -= dir * 880.0f * dt;
                capY += capV * dt;
                const bool back = (dir < 0.0f) ? capY >= capRest : capY <= capRest;
                if (back)
                {
                    capY = capRest;
                    capV = -capV * 0.35f;
                    if (++bounces >= 3) capV = 0.0f;
                }
            }
            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += dt;
                sp.vy -= dir * 360.0f * dt;
                sp.x  += sp.vx * dt;
                sp.y  += sp.vy * dt;
            }
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            if (cols.empty()) return;
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.25f) / 0.55f);
            const int   n    = (int) cols.size();
            const int   lit  = juce::jmin (n, (int) std::ceil ((age / sweepSec) * (float) n));

            for (int i = 0; i < lit; ++i)
            {
                const float head = juce::jlimit (0.0f, 1.0f, 1.0f - (float) (lit - 1 - i) / 6.0f);
                g.setColour (colours[(size_t) i].brighter (0.25f + 0.40f * head).withAlpha ((0.50f + 0.42f * head) * fade));
                g.fillRect (cols[(size_t) i]);
            }

            if (launched)
            {
                const auto& top = cols.back();
                const juce::Rectangle<float> cap (top.getX(), capY, top.getWidth(), 3.5f);
                g.setColour (colours.back().withAlpha (0.45f * fade));
                g.fillRect (cap.expanded (1.0f, 1.0f));
                g.setColour (colours.back().brighter (0.45f).withAlpha (fade));
                g.fillRect (cap);
                g.setColour (PinkXP::hl.withAlpha (0.95f * fade));
                g.fillRect (cap.getX(), cap.getCentreY() - 0.5f, cap.getWidth(), 1.0f);
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                g.setColour ((sp.useAccent2 ? PinkXP::hl : colours.back()).withAlpha (0.85f * f * fade));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }
        juce::Rectangle<float> getBounds() const override
        {
            if (cols.empty()) return {};
            auto b = cols.front();
            for (const auto& c : cols) b = b.getUnion (c);
            return b.expanded (10.0f, 80.0f);
        }
    };

    // ---- VU 极简游标：卷尺（含 ΔdB 读数）----
    struct MeasureTapeActor : public ToyActor
    {
        float y, fromX, toX;
        juce::String label;
        MeasureTapeActor (float lineY, float from, float to, const juce::String& text)
            : y (lineY), fromX (from), toX (to), label (text) { ttl = 2.2f; }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float outT  = juce::jlimit (0.0f, 1.0f, age / 0.25f);
            const float backT = juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.3f)) / 0.3f);
            const float ease  = 1.0f - std::pow (1.0f - outT, 3.0f);
            const float len   = (toX - fromX) * ease * (1.0f - backT * backT);
            const float x1    = fromX + len;
            const float l = juce::jmin (fromX, x1), r = juce::jmax (fromX, x1);
            const float h = 6.0f;
            const juce::Rectangle<float> tape (l, y - h * 0.5f, r - l, h);

            g.setColour (PinkXP::warn.withAlpha (0.9f));
            g.fillRect (tape);
            g.setColour (PinkXP::ink.withAlpha (0.8f));
            for (float xx = fromX; (toX >= fromX) ? xx <= x1 : xx >= x1; xx += (toX >= fromX ? 4.0f : -4.0f))
                g.fillRect (juce::Rectangle<float> (xx - 0.5f, y - h * 0.5f, 1.0f, (std::fmod (std::abs (xx - fromX), 20.0f) < 2.0f) ? h : h * 0.5f));
            g.drawRect (tape, 1.0f);

            g.setColour (PinkXP::ink);
            g.fillRect (juce::Rectangle<float> (x1 - 1.5f, y - h, 3.0f, h * 2.0f));
            const juce::Rectangle<float> box (fromX - 6.0f, y - 7.0f, 12.0f, 14.0f);
            g.setColour (PinkXP::sel);
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (PinkXP::ink);
            g.drawRoundedRectangle (box, 3.0f, 1.0f);

            if (outT >= 1.0f && backT <= 0.0f)
            {
                g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
                const float w = 70.0f;
                const juce::Rectangle<int> b ((int) ((fromX + toX) * 0.5f - w * 0.5f), (int) (y - h - 18.0f), (int) w, 16);
                PinkXP::drawRaised (g, b, PinkXP::face);
                g.setColour (PinkXP::readableInkOn (PinkXP::face));
                g.drawText (label, b, juce::Justification::centred, false);
            }
        }
        juce::Rectangle<float> getBounds() const override
        {
            const float l = juce::jmin (fromX, toX) - 40.0f;
            return { l, y - 34.0f, std::abs (toX - fromX) + 80.0f, 48.0f };
        }
    };

    // ---- VU 镜像刻度：水滴涟漪（双色荧光谐波）----
    struct MirrorRippleActor : public ToyActor
    {
        float x, spineY, top, bottom, xMin, xMax;
        MirrorRippleActor (float atX, float spine, float t, float b, float l, float r)
            : x (atX), spineY (spine), top (t), bottom (b), xMin (l), xMax (r) { ttl = 1.6f; }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            constexpr float dropSec = 0.16f;
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (juce::Rectangle<float> (xMin, top - 4.0f, xMax - xMin, bottom - top + 8.0f)
                                    .getSmallestIntegerContainer());

            if (age < dropSec)
            {
                const float t  = age / dropSec;
                const float dy = (spineY - top) * (1.0f - t * t);
                drawGlowDot (g, x, spineY - dy, PinkXP::sel, 0.95f, 3.6f, 1.4f);
                drawGlowDot (g, x, spineY + dy, PinkXP::accent2, 0.45f, 3.0f, 1.1f);
                return;
            }

            const float halfH = juce::jmax (4.0f, juce::jmax (spineY - top, bottom - spineY));
            for (int i = 0; i < 3; ++i)
            {
                const float t = (age - dropSec - (float) i * 0.14f) / (ttl - dropSec - 0.3f);
                if (t <= 0.0f || t >= 1.0f) continue;
                const float rx = 6.0f + t * 92.0f;
                const float ry = 3.0f + t * halfH;
                const juce::Colour col = (i == 1) ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (col.withAlpha (0.72f * (1.0f - t)));
                g.drawEllipse (x - rx, spineY - ry, rx * 2.0f, ry * 2.0f, 1.8f - 0.8f * t);
            }

            const float flash = juce::jlimit (0.0f, 1.0f, 1.0f - (age - dropSec) / 0.25f);
            if (flash > 0.0f)
            {
                g.setColour (PinkXP::hl.withAlpha (0.85f * flash));
                g.fillRect (juce::Rectangle<float> (x - 12.0f * flash - 1.0f, spineY - 0.8f, 24.0f * flash + 2.0f, 1.6f));
            }
        }
        juce::Rectangle<float> getBounds() const override
        {
            return { x - 100.0f, top - 6.0f, 200.0f, bottom - top + 12.0f };
        }
    };

    // ---- 读数气泡（保留供 TempoKey 实用倍速/音阶读数使用）----
    struct ReadoutBubbleActor : public ToyActor
    {
        juce::Point<float>   at;
        juce::String         text;
        EmojiId              icon;
        juce::Colour         accentColour;
        juce::Rectangle<int> clampRect;
        int                  cachedWidth = 120;

        ReadoutBubbleActor (juce::Point<float> p, const juce::String& t, EmojiId ic, juce::Colour c,
                            juce::Rectangle<int> clamp)
            : at (p), text (t), icon (ic), accentColour (c), clampRect (clamp)
        {
            ttl = 1.6f;
            const int iconW = (icon != EmojiId::Count) ? 22 : 0;
            const juce::Font f = PinkXP::getFont (11.0f, juce::Font::bold);
            cachedWidth = juce::jmax (40, iconW + juce::roundToInt (f.getStringWidthFloat (y2k::tr (text))) + 12);
        }

        bool isReadoutBubble() const noexcept override { return true; }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            at.y -= 14.0f * dt;
            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = age < ttl * 0.8f ? 1.0f : 1.0f - (age - ttl * 0.8f) / (ttl * 0.2f);
            const int iconW = (icon != EmojiId::Count) ? 22 : 0;
            const int h = 22;

            const juce::Font f = PinkXP::getFont (11.0f, juce::Font::bold);
            g.setFont (f);
            const int textW = juce::roundToInt (f.getStringWidthFloat (y2k::tr (text))) + 8;
            const int w = iconW + textW;
            auto b = juce::Rectangle<int> ((int) at.x - w / 2, (int) at.y - h - 8, w, h);

            if (! clampRect.isEmpty())
                b = b.constrainedWithin (clampRect);

            g.setOpacity (a);
            PinkXP::drawRaised (g, b, PinkXP::face);
            if (icon != EmojiId::Count && dir.atlas != nullptr)
            {
                const auto& img = dir.atlas->themedImage (icon, EmojiAtlas::Size::S);
                if (img.isValid())
                    g.drawImage (img, b.getX() + 3, b.getY() + (h - 16) / 2, 16, 16, 0, 0,
                                 img.getWidth(), img.getHeight());
            }
            g.setColour (PinkXP::readableInkOn (PinkXP::face));
            g.drawText (y2k::tr (text), b.withTrimmedLeft (iconW).reduced (2, 0),
                        juce::Justification::centredLeft, false);
            g.setOpacity (1.0f);
        }

        juce::Rectangle<float> getBounds() const override
        {
            constexpr int h = 22;
            auto b = juce::Rectangle<int> ((int) at.x - cachedWidth / 2, (int) at.y - h - 8, cachedWidth, h);
            if (! clampRect.isEmpty())
                b = b.constrainedWithin (clampRect);
            auto expanded = b.toFloat().expanded (12.0f, 8.0f);
            if (! clampRect.isEmpty())
                expanded = expanded.getIntersection (clampRect.toFloat());
            return expanded;
        }
    };
}

// ==========================================================
// 工厂
// ==========================================================
juce::String toyFreqToNoteName (float freq) { return freqToNoteName (freq); }

std::unique_ptr<ToyActor> makeSpectrumPluckActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> canvas,
                                                  const std::vector<float>& curveY,
                                                  const std::vector<juce::Point<float>>& harmonics,
                                                  std::function<bool()> isSlowMo)
{
    return std::make_unique<SpectrumPluckActor> (clickPos, canvas, curveY, harmonics, std::move (isSlowMo));
}

std::unique_ptr<ToyActor> makeWaveformShockActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> laneRect,
                                                  const std::vector<float>& envTopY,
                                                  const std::vector<float>& envBotY,
                                                  std::function<bool()> isSlowMo)
{
    return std::make_unique<WaveformShockActor> (clickPos, laneRect, envTopY, envBotY, std::move (isSlowMo));
}

std::unique_ptr<ToyActor> makeSpectrogramBuoyActor (juce::Point<float> clickPos,
                                                    juce::Rectangle<int> canvas,
                                                    juce::Point<float> driftVelPxPerSec,
                                                    std::function<bool()> isFrozen)
{
    return std::make_unique<SpectrogramBuoyActor> (clickPos, canvas, driftVelPxPerSec, std::move (isFrozen));
}

std::unique_ptr<ToyActor> makeSpec3DPulseActor (juce::Point<float> clickPos,
                                                juce::Rectangle<int> canvas,
                                                juce::Point<float> depthDirUnit)
{
    return std::make_unique<Spec3DPulseActor> (clickPos, canvas, depthDirUnit);
}

std::unique_ptr<ToyActor> makeStereoRadarSweepActor (juce::Point<float> clickPos,
                                                     juce::Rectangle<int> canvas,
                                                     juce::Point<float> origin,
                                                     float radarRadius)
{
    return std::make_unique<StereoRadarSweepActor> (clickPos, canvas, origin, radarRadius);
}

std::unique_ptr<ToyActor> makeStereoMapBeamActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> plot,
                                                  bool isMonoMode,
                                                  std::function<bool()> isSlowMo)
{
    return std::make_unique<StereoMapBeamActor> (clickPos, plot, isMonoMode, std::move (isSlowMo));
}

std::unique_ptr<ToyActor> makePhaseGonioOrbiterActor (juce::Point<float> clickPos,
                                                      juce::Rectangle<int> gonioArea)
{
    return std::make_unique<PhaseGonioOrbiterActor> (clickPos, gonioArea);
}

std::unique_ptr<ToyActor> makeTrackOrbActor (juce::Rectangle<float> track,
                                             juce::Point<float> clickPos,
                                             bool useAccent2)
{
    return std::make_unique<TrackOrbActor> (track, clickPos, useAccent2);
}

std::unique_ptr<ToyActor> makeGhostNeedleActor (juce::Point<float> pivot,
                                                float radius,
                                                float restThetaRad,
                                                float startThetaRad)
{
    return std::make_unique<GhostNeedleActor> (pivot, radius, restThetaRad, startThetaRad);
}

std::unique_ptr<ToyActor> makeMeterBarLaunchActor (juce::Rectangle<float> barRect,
                                                   float clickY,
                                                   juce::Colour accentColour)
{
    return std::make_unique<MeterBarLaunchActor> (barRect, clickY, accentColour);
}

std::unique_ptr<ToyActor> makePerimeterSweepActor (juce::Rectangle<float> boxRect,
                                                   juce::Point<float> clickPos,
                                                   juce::Colour accentColour)
{
    return std::make_unique<PerimeterSweepActor> (boxRect, clickPos, accentColour);
}

std::unique_ptr<ToyActor> makeCompressorClampActor (juce::Rectangle<float> cellRect,
                                                    juce::Point<float> clickPos)
{
    return std::make_unique<CompressorClampActor> (cellRect, clickPos);
}

std::unique_ptr<ToyActor> makeCrestSurferActor (juce::Point<float> clickPos,
                                                juce::Rectangle<int> plotRect,
                                                const std::vector<float>& curveY)
{
    return std::make_unique<CrestSurferActor> (clickPos, plotRect, curveY);
}

std::unique_ptr<ToyActor> makeBpmMetronomePulseActor (juce::Rectangle<int> colRect,
                                                      juce::Point<float> clickPos,
                                                      float bpm)
{
    return std::make_unique<BpmMetronomePulseActor> (colRect, clickPos, bpm);
}

std::unique_ptr<ToyActor> makeKeyConstellationActor (juce::Rectangle<int> colRect,
                                                     juce::Point<float> clickPos,
                                                     int keyRoot, int keyMode)
{
    return std::make_unique<KeyConstellationActor> (colRect, clickPos, keyRoot, keyMode);
}

std::unique_ptr<ToyActor> makeSysCircuitPulseActor (juce::Point<float> clickPos,
                                                    juce::Rectangle<int> tileRect,
                                                    juce::Colour accentColour)
{
    return std::make_unique<SysCircuitPulseActor> (clickPos, tileRect, accentColour);
}

std::unique_ptr<ToyActor> makeSliderFlickActor (float top, float bottom, float clickX, float restX)
{
    return std::make_unique<SliderFlickActor> (top, bottom, clickX, restX);
}

std::unique_ptr<ToyActor> makeLedChaseActor (std::vector<juce::Rectangle<float>> cols,
                                             std::vector<juce::Colour> colours, float capDir)
{
    return std::make_unique<LedChaseActor> (std::move (cols), std::move (colours), capDir);
}

std::unique_ptr<ToyActor> makeMeasureTapeActor (float lineY, float fromX, float toX, const juce::String& label)
{
    return std::make_unique<MeasureTapeActor> (lineY, fromX, toX, label);
}

std::unique_ptr<ToyActor> makeMirrorRippleActor (float x, float spineY, float top, float bottom, float xMin, float xMax)
{
    return std::make_unique<MirrorRippleActor> (x, spineY, top, bottom, xMin, xMax);
}

std::unique_ptr<ToyActor> makeDiscoLedActor (juce::Rectangle<float> led)
{
    return std::make_unique<DiscoLedActor> (led);
}

std::unique_ptr<ToyActor> makeReadoutBubble (juce::Point<float> at, const juce::String& text,
                                             EmojiId icon, juce::Colour accentColour,
                                             juce::Rectangle<int> clampRect)
{
    return std::make_unique<ReadoutBubbleActor> (at, text, icon, accentColour, clampRect);
}
