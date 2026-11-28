#include "source/ui/toys/ToyKit.h"
#include <array>
#include <cmath>

// ==========================================================
// ToyFx_Oscilloscope —— 示波器专属双态荧光电子玩具（纯 L1 叠加，不碰 GL）
//
//   设计目标：取代原先遮挡画面的拍立得打印纸，做一套真正贴合示波器
//   CRT 电子束气质、同时 **绝不干扰用户按住慢放观察** 的轻量物理玩具：
//
//   1) Waveform 模式 —— 电子冲浪珠（OscWaveSurferActor）
//      · 点击瞬间在落点激起极细 1px 双环电磁脉冲（零白闪、零遮挡块）；
//      · 同步放出左 / 右两颗荧光光珠，沿着当前真实波形轮廓 y(x) 向画布两侧疾驰冲浪；
//      · 经过陡峭波峰时顺势腾空起跳划出轻盈抛物线，再回落贴波继续冲；
//      · 撞到画布左右边沿时反弹并溅出微小 2px 磷光火花。
//
//   2) X-Y / Lissajous 模式 —— 轨道荧光电子（OscOrbitElectronActor）
//      · 点击瞬间释放一颗带电荧光电子，切入画布中心引力势阱，绕散点中心
//        划出优雅的椭圆 / 玫瑰线轨道与 22 节渐变余辉细带；
//      · 轨道亮度与轻度离心扰动实时呼应音频能量（dir.audioEnergy()）；
//      · 触碰画布边界弹性折射并溅出微小磷光火花。
//
//   3) 按住慢放零干扰联动（isSlowMo 回调）：
//      · 用户按住鼠标观察 50x 慢放期间，玩具在 ~120ms 内平滑切入 0.10x
//        子弹时间，同时透明度柔化至 0.22x，不抢视线、不盖波形；
//      · 轻点或松手瞬间恢复 1.0x 全速与全亮，把余下的轨道/冲浪优雅飞完。
// ==========================================================

namespace
{
    struct MiniSpark
    {
        float x = 0.0f, y = 0.0f;
        float vx = 0.0f, vy = 0.0f;
        float life = 0.0f;
        float maxLife = 0.35f;
        bool  useAccent2 = false;
    };

    struct OscBoundsAccum
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

    // ------------------------------------------------------
    // 1) Waveform 模式：电子冲浪珠
    // ------------------------------------------------------
    struct OscWaveSurferActor : public ToyActor
    {
        static constexpr int kProfileBins = 128;
        static constexpr int kTrailLen    = 14;
        static constexpr int kMaxSparks   = 12;

        struct Rider
        {
            float x = 0.0f, y = 0.0f;
            float vx = 0.0f, vy = 0.0f;
            float dir = 1.0f;            // -1 向左 / +1 向右
            bool  airborne = false;
            bool  finished = false;
            bool  useAccent2 = false;
            std::array<juce::Point<float>, kTrailLen> trail {};
            int   trailCount = 0;
        };

        juce::Point<float>    origin;
        juce::Rectangle<int>  canvas;
        juce::Rectangle<float> inner;
        std::array<float, kProfileBins> waveY {};   // 预先采样好的波形屏幕 Y（画布局部坐标）
        std::array<Rider, 2>  riders {};
        std::array<MiniSpark, kMaxSparks> sparks {};
        int                   sparkCount = 0;

        std::function<bool()> isSlowMo;
        float                 slowFade = 1.0f;      // 1.0=全亮，按住慢放时平滑退让至 0.22

        OscWaveSurferActor (juce::Point<float> clickPos,
                            juce::Rectangle<int> canvasBounds,
                            const juce::Array<float>& samples,
                            std::function<bool()> slowMoQuery)
            : origin (clickPos),
              canvas (canvasBounds),
              inner (canvasBounds.reduced (4).toFloat()),
              isSlowMo (std::move (slowMoQuery))
        {
            ttl = 1.65f;
            if (inner.getWidth() < 8.0f || inner.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }

            const float cy    = inner.getCentreY();
            const float halfH = inner.getHeight() * 0.45f;
            const int   N     = samples.size();

            // 把波形映射到 kProfileBins 个屏幕 Y 点（与 OscilloscopeModule::buildGlSnapshot 完全同口径）
            for (int i = 0; i < kProfileBins; ++i)
            {
                float s = 0.0f;
                if (N > 1)
                {
                    const float fIdx = (float) i / (float) (kProfileBins - 1) * (float) (N - 1);
                    const int   idx0 = juce::jlimit (0, N - 1, (int) fIdx);
                    const int   idx1 = juce::jmin (N - 1, idx0 + 1);
                    const float frac = fIdx - (float) idx0;
                    const float a = std::isfinite (samples.getUnchecked (idx0))
                                        ? juce::jlimit (-1.0f, 1.0f, samples.getUnchecked (idx0)) : 0.0f;
                    const float b = std::isfinite (samples.getUnchecked (idx1))
                                        ? juce::jlimit (-1.0f, 1.0f, samples.getUnchecked (idx1)) : 0.0f;
                    s = a + (b - a) * frac;
                }
                waveY[(size_t) i] = cy - s * halfH;
            }

            const float startX = juce::jlimit (inner.getX() + 2.0f, inner.getRight() - 2.0f, origin.x);
            const float startY = sampleWaveY (startX);
            origin = { startX, startY };

            const float baseSpeed = juce::jlimit (150.0f, 420.0f, inner.getWidth() * 0.78f);

            for (int r = 0; r < 2; ++r)
            {
                auto& rd = riders[(size_t) r];
                rd.dir        = (r == 0) ? -1.0f : 1.0f;
                rd.x          = startX;
                rd.y          = startY;
                rd.vx         = rd.dir * baseSpeed;
                rd.vy         = -28.0f;   // 起步微微一弹
                rd.useAccent2 = (r == 1);
                rd.trail.fill ({ startX, startY });
                rd.trailCount = 1;
            }
        }

        float sampleWaveY (float px) const noexcept
        {
            if (inner.getWidth() <= 1.0f)
                return inner.getCentreY();
            const float u = juce::jlimit (0.0f, 1.0f, (px - inner.getX()) / inner.getWidth());
            const float f = u * (float) (kProfileBins - 1);
            const int   i0 = juce::jlimit (0, kProfileBins - 1, (int) f);
            const int   i1 = juce::jmin (kProfileBins - 1, i0 + 1);
            return waveY[(size_t) i0] + (waveY[(size_t) i1] - waveY[(size_t) i0]) * (f - (float) i0);
        }

        void spawnEdgeSparks (float ex, float ey, float normalX, bool accent2)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 4 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = ex;
                sp.y = ey;
                const float spd = 45.0f + rng.nextFloat() * 85.0f;
                const float ang = (rng.nextFloat() - 0.5f) * 2.0f;
                sp.vx = normalX * (0.45f + 0.55f * std::abs (std::cos (ang))) * spd;
                sp.vy = std::sin (ang) * spd - 35.0f;
                sp.life = 0.0f;
                sp.maxLife = 0.28f + rng.nextFloat() * 0.20f;
                sp.useAccent2 = accent2;
            }
        }

        void step (float dt, ToyDirector& dir) override
        {
            const bool holding = (isSlowMo != nullptr) && isSlowMo();
            const float targetFade = holding ? 0.22f : 1.0f;
            slowFade += (targetFade - slowFade) * juce::jlimit (0.0f, 1.0f, dt * 14.0f);

            // 按住慢放时玩具同步进入 0.10x 子弹时间，不乱窜分心；松手立即恢复 1.0x
            const float effDt = dt * (holding ? 0.10f : 1.0f);
            age += effDt;

            const float energyBoost = 1.0f + 0.35f * juce::jlimit (0.0f, 1.0f, dir.audioEnergy());
            const float gravity     = 760.0f;
            bool anyActive = (age < 0.35f);

            for (auto& rd : riders)
            {
                if (! rd.finished)
                {
                    anyActive = true;
                    const float prevY = rd.y;
                    rd.x += rd.vx * energyBoost * effDt;

                    if (rd.x <= inner.getX() + 1.0f)
                    {
                        rd.x = inner.getX() + 1.0f;
                        rd.finished = true;
                        spawnEdgeSparks (rd.x, rd.y, +1.0f, rd.useAccent2);
                    }
                    else if (rd.x >= inner.getRight() - 1.0f)
                    {
                        rd.x = inner.getRight() - 1.0f;
                        rd.finished = true;
                        spawnEdgeSparks (rd.x, rd.y, -1.0f, rd.useAccent2);
                    }
                    else
                    {
                        const float surfY = sampleWaveY (rd.x);
                        if (rd.airborne)
                        {
                            rd.vy += gravity * effDt;
                            rd.y  += rd.vy * effDt;
                            if (rd.y >= surfY)
                            {
                                rd.y = surfY;
                                rd.airborne = false;
                                rd.vy = 0.0f;
                            }
                        }
                        else
                        {
                            const float slopeVy = (effDt > 1.0e-5f) ? (surfY - prevY) / effDt : 0.0f;
                            // 冲过陡峭向上波峰后若前方曲线骤降，顺势腾空起跳
                            if (slopeVy > 190.0f && rd.vy < -80.0f)
                            {
                                rd.airborne = true;
                                rd.y = prevY + rd.vy * effDt;
                            }
                            else
                            {
                                rd.y = surfY;
                                rd.vy = slopeVy;
                            }
                        }
                        rd.y = juce::jlimit (inner.getY() + 2.0f, inner.getBottom() - 2.0f, rd.y);
                    }

                    for (int i = kTrailLen - 1; i > 0; --i)
                        rd.trail[(size_t) i] = rd.trail[(size_t) i - 1];
                    rd.trail[0] = { rd.x, rd.y };
                    rd.trailCount = juce::jmin (kTrailLen, rd.trailCount + 1);
                }
                else if (rd.trailCount > 1)
                {
                    // 到边后尾迹自然收短消失
                    anyActive = true;
                    --rd.trailCount;
                }
            }

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                anyActive = true;
                sp.life += effDt;
                sp.vy += 420.0f * effDt;
                sp.x  += sp.vx * effDt;
                sp.y  += sp.vy * effDt;
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
            g.reduceClipRegion (canvas);

            // 1) 落点极细双环电磁涟漪（0.35s，1px 细线，无填充）
            if (age < 0.36f)
            {
                const float t = age / 0.36f;
                const float r1 = 3.0f + t * 22.0f;
                const float a1 = (1.0f - t) * 0.65f * masterA;
                g.setColour (PinkXP::sel.withAlpha (a1));
                g.drawEllipse (origin.x - r1, origin.y - r1, r1 * 2.0f, r1 * 2.0f, 1.1f);

                const float r2 = 2.0f + t * 13.0f;
                g.setColour (PinkXP::accent2.withAlpha (a1 * 0.7f));
                g.drawEllipse (origin.x - r2, origin.y - r2, r2 * 2.0f, r2 * 2.0f, 1.0f);
            }

            // 2) 左右两颗波形冲浪光珠 + 渐变荧光尾迹
            for (const auto& rd : riders)
            {
                const juce::Colour col = rd.useAccent2 ? PinkXP::accent2 : PinkXP::sel;

                for (int i = 1; i < rd.trailCount; ++i)
                {
                    const float frac = 1.0f - (float) i / (float) kTrailLen;
                    const auto& p0 = rd.trail[(size_t) (i - 1)];
                    const auto& p1 = rd.trail[(size_t) i];
                    g.setColour (col.withAlpha (0.72f * frac * frac * masterA));
                    g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.7f + 1.3f * frac);
                }

                if (! rd.finished)
                {
                    // 极细外光环 + 3px 亮核
                    g.setColour (col.withAlpha (0.38f * masterA));
                    g.fillEllipse (rd.x - 4.0f, rd.y - 4.0f, 8.0f, 8.0f);
                    g.setColour (PinkXP::hl.withAlpha (0.95f * masterA));
                    g.fillEllipse (rd.x - 1.6f, rd.y - 1.6f, 3.2f, 3.2f);
                }
            }

            // 3) 撞边溅出的微小 2px 磷光火花
            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour col = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (col.withAlpha (0.85f * f * masterA));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            OscBoundsAccum b;
            if (age < 0.36f)
                b.includePoint (origin.x, origin.y, 26.0f);
            for (const auto& rd : riders)
            {
                for (int i = 0; i < rd.trailCount; ++i)
                    b.includePoint (rd.trail[(size_t) i].x, rd.trail[(size_t) i].y, 4.5f);
                if (! rd.finished)
                    b.includePoint (rd.x, rd.y, 4.5f);
            }
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (canvas, 8.0f);
        }
    };

    // ------------------------------------------------------
    // 2) X-Y / Lissajous 模式：轨道荧光电子
    // ------------------------------------------------------
    struct OscOrbitElectronActor : public ToyActor
    {
        static constexpr int kTrailLen  = 22;
        static constexpr int kMaxSparks = 8;

        juce::Point<float>     origin;
        juce::Rectangle<int>   canvas;
        juce::Rectangle<float> inner;
        juce::Point<float>     center;
        float                  x = 0.0f, y = 0.0f;
        float                  vx = 0.0f, vy = 0.0f;
        float                  baseR = 60.0f;
        bool                   useAccent2 = false;

        std::array<juce::Point<float>, kTrailLen> trail {};
        int                    trailCount = 0;
        std::array<MiniSpark, kMaxSparks> sparks {};
        int                    sparkCount = 0;

        std::function<bool()>  isSlowMo;
        float                  slowFade = 1.0f;
        float                  pulseGlow = 0.0f;

        OscOrbitElectronActor (juce::Point<float> clickPos,
                               juce::Rectangle<int> canvasBounds,
                               std::function<bool()> slowMoQuery)
            : origin (clickPos),
              canvas (canvasBounds),
              inner (canvasBounds.reduced (6).toFloat()),
              center (inner.getCentre()),
              isSlowMo (std::move (slowMoQuery))
        {
            ttl = 2.0f;
            if (inner.getWidth() < 8.0f || inner.getHeight() < 8.0f)
            {
                alive = false;
                return;
            }
            baseR = juce::jmax (18.0f, juce::jmin (inner.getWidth(), inner.getHeight()) * 0.44f);

            x = juce::jlimit (inner.getX() + 3.0f, inner.getRight() - 3.0f, origin.x);
            y = juce::jlimit (inner.getY() + 3.0f, inner.getBottom() - 3.0f, origin.y);
            origin = { x, y };

            auto& rng = juce::Random::getSystemRandom();
            useAccent2 = rng.nextBool();

            // 切线方向初速度（顺/逆时针随机），形成优雅的中心势阱玫瑰/椭圆轨道
            float dx = x - center.x;
            float dy = y - center.y;
            float dist = std::sqrt (dx * dx + dy * dy);
            if (dist < 6.0f)
            {
                const float a = rng.nextFloat() * 6.283185f;
                dx = std::cos (a) * 14.0f;
                dy = std::sin (a) * 14.0f;
                dist = 14.0f;
            }
            const float spin = rng.nextBool() ? 1.0f : -1.0f;
            const float tx = -dy / dist * spin;
            const float ty =  dx / dist * spin;
            const float orbitalSpeed = juce::jlimit (120.0f, 340.0f, baseR * (2.2f + rng.nextFloat() * 0.8f));

            // 叠一点径向分量，让轨道呈现椭圆/玫瑰进动而不是呆板正圆
            const float rx = dx / dist;
            const float ry = dy / dist;
            const float radialKick = (rng.nextFloat() - 0.5f) * orbitalSpeed * 0.35f;
            vx = tx * orbitalSpeed + rx * radialKick;
            vy = ty * orbitalSpeed + ry * radialKick;

            trail.fill ({ x, y });
            trailCount = 1;
        }

        void spawnBounceSparks (float bx, float by, float nx, float ny)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int k = 0; k < 3 && sparkCount < kMaxSparks; ++k)
            {
                auto& sp = sparks[(size_t) sparkCount++];
                sp.x = bx;
                sp.y = by;
                const float spd = 35.0f + rng.nextFloat() * 65.0f;
                const float jitter = (rng.nextFloat() - 0.5f) * 1.4f;
                const float cs = std::cos (jitter), sn = std::sin (jitter);
                sp.vx = (nx * cs - ny * sn) * spd;
                sp.vy = (nx * sn + ny * cs) * spd;
                sp.life = 0.0f;
                sp.maxLife = 0.25f + rng.nextFloat() * 0.18f;
                sp.useAccent2 = ! useAccent2;
            }
        }

        void step (float dt, ToyDirector& dir) override
        {
            const bool holding = (isSlowMo != nullptr) && isSlowMo();
            const float targetFade = holding ? 0.22f : 1.0f;
            slowFade += (targetFade - slowFade) * juce::jlimit (0.0f, 1.0f, dt * 14.0f);

            const float effDt = dt * (holding ? 0.10f : 1.0f);
            age += effDt;

            const float energy = juce::jlimit (0.0f, 1.0f, dir.audioEnergy());
            pulseGlow += (energy - pulseGlow) * juce::jlimit (0.0f, 1.0f, dt * 12.0f);

            // 柔化弹簧引力（略带非线性进动，轨道像电子云/李萨如玫瑰线）
            const float dx = center.x - x;
            const float dy = center.y - y;
            const float distSq = dx * dx + dy * dy + 160.0f;
            const float dist   = std::sqrt (distSq);
            const float kSpring = 14.5f;
            const float kCore   = baseR * baseR * 8.0f;
            const float accel   = kSpring * dist + kCore / distSq;

            // 音频能量大时给一点极轻柔的向外呼吸推力
            const float breath = energy * 160.0f;
            vx += ((dx / dist) * (accel - breath)) * effDt;
            vy += ((dy / dist) * (accel - breath)) * effDt;

            // 轻微阻尼防发散；生命末段 0.35s 略微松开引力优雅向外旋出
            const float damp = std::exp (-0.28f * effDt);
            vx *= damp;
            vy *= damp;

            x += vx * effDt;
            y += vy * effDt;

            // 边界弹性折射
            if (x < inner.getX() + 2.0f)
            {
                x = inner.getX() + 2.0f;
                vx = std::abs (vx) * 0.85f;
                spawnBounceSparks (x, y, +1.0f, 0.0f);
            }
            else if (x > inner.getRight() - 2.0f)
            {
                x = inner.getRight() - 2.0f;
                vx = -std::abs (vx) * 0.85f;
                spawnBounceSparks (x, y, -1.0f, 0.0f);
            }
            if (y < inner.getY() + 2.0f)
            {
                y = inner.getY() + 2.0f;
                vy = std::abs (vy) * 0.85f;
                spawnBounceSparks (x, y, 0.0f, +1.0f);
            }
            else if (y > inner.getBottom() - 2.0f)
            {
                y = inner.getBottom() - 2.0f;
                vy = -std::abs (vy) * 0.85f;
                spawnBounceSparks (x, y, 0.0f, -1.0f);
            }

            for (int i = kTrailLen - 1; i > 0; --i)
                trail[(size_t) i] = trail[(size_t) i - 1];
            trail[0] = { x, y };
            trailCount = juce::jmin (kTrailLen, trailCount + 1);

            for (int i = 0; i < sparkCount; ++i)
            {
                auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                sp.life += effDt;
                sp.x += sp.vx * effDt;
                sp.y += sp.vy * effDt;
            }

            if (age >= ttl)
                alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float endFade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - (ttl - 0.45f)) / 0.45f);
            const float masterA = endFade * slowFade;
            if (masterA <= 0.01f)
                return;

            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (canvas);

            const juce::Colour col = useAccent2 ? PinkXP::accent2 : PinkXP::sel;

            // 1) 落点极细单环激发涟漪（0.32s）
            if (age < 0.32f)
            {
                const float t = age / 0.32f;
                const float r = 3.0f + t * 18.0f;
                g.setColour (col.withAlpha ((1.0f - t) * 0.60f * masterA));
                g.drawEllipse (origin.x - r, origin.y - r, r * 2.0f, r * 2.0f, 1.1f);
            }

            // 2) 22 节渐变余辉轨迹
            for (int i = 1; i < trailCount; ++i)
            {
                const float frac = 1.0f - (float) i / (float) kTrailLen;
                const auto& p0 = trail[(size_t) (i - 1)];
                const auto& p1 = trail[(size_t) i];
                g.setColour (col.withAlpha (0.72f * frac * frac * masterA));
                g.drawLine (p0.x, p0.y, p1.x, p1.y, 0.6f + 1.4f * frac);
            }

            // 3) 电子主体（外晕随音频能量微弱呼吸 + 亮核）
            const float haloR = 3.6f + 2.2f * pulseGlow;
            g.setColour (col.withAlpha ((0.32f + 0.25f * pulseGlow) * masterA));
            g.fillEllipse (x - haloR, y - haloR, haloR * 2.0f, haloR * 2.0f);
            g.setColour (PinkXP::hl.withAlpha (0.95f * masterA));
            g.fillEllipse (x - 1.6f, y - 1.6f, 3.2f, 3.2f);

            // 4) 撞边微小火花
            for (int i = 0; i < sparkCount; ++i)
            {
                const auto& sp = sparks[(size_t) i];
                if (sp.life >= sp.maxLife) continue;
                const float f = 1.0f - sp.life / sp.maxLife;
                const juce::Colour sc = sp.useAccent2 ? PinkXP::accent2 : PinkXP::sel;
                g.setColour (sc.withAlpha (0.80f * f * masterA));
                g.fillRect (sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f);
            }
        }

        juce::Rectangle<float> getBounds() const override
        {
            OscBoundsAccum b;
            if (age < 0.32f)
                b.includePoint (origin.x, origin.y, 22.0f);
            for (int i = 0; i < trailCount; ++i)
                b.includePoint (trail[(size_t) i].x, trail[(size_t) i].y, 4.0f);
            b.includePoint (x, y, 6.5f);
            for (int i = 0; i < sparkCount; ++i)
                if (sparks[(size_t) i].life < sparks[(size_t) i].maxLife)
                    b.includePoint (sparks[(size_t) i].x, sparks[(size_t) i].y, 2.0f);
            return b.finish (canvas, 8.0f);
        }
    };
}

std::unique_ptr<ToyActor> makeOscWaveSurferActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> canvas,
                                                  const juce::Array<float>& waveSamples,
                                                  std::function<bool()> isSlowMo)
{
    return std::make_unique<OscWaveSurferActor> (clickPos, canvas, waveSamples, std::move (isSlowMo));
}

std::unique_ptr<ToyActor> makeOscOrbitElectronActor (juce::Point<float> clickPos,
                                                     juce::Rectangle<int> canvas,
                                                     std::function<bool()> isSlowMo)
{
    return std::make_unique<OscOrbitElectronActor> (clickPos, canvas, std::move (isSlowMo));
}
