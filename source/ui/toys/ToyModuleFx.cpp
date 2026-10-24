#include "source/ui/toys/ToyKit.h"
#include <array>

// ==========================================================
// ToyModuleFx —— 各模块专属玩法 actor（§8.2 / §8.3 的 L1 效果）
//   全部是"半透明叠加 / 独立元素"（R8），纯 Graphics 绘制，
//   不读回 GL、不改模块 paint。坐标统一为面板局部坐标系。
// ==========================================================

namespace
{
    // 频率 → 音名（A4=440Hz），纯函数换算（§8.3 Spectrum）
    juce::String freqToNoteName (float freq)
    {
        if (freq <= 0.0f || freq > 20000.0f)
            return {};
        const float midi = 69.0f + 12.0f * std::log2 (freq / 440.0f);
        const int n = juce::roundToInt (midi);
        static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        const int idx = n % 12; if (idx < 0) return {};
        const int oct = n / 12 - 1;
        const int cents = juce::roundToInt ((midi - (float) n) * 100.0f);
        juce::String s;
        s << names[idx] << oct;
        if (cents != 0) s << (cents > 0 ? " +" : " ") << cents << "c";
        s << " " << juce::roundToInt (freq) << "Hz";
        return s;
    }

    // ---- Spectrum：拨弦涟漪 + 竖向余振线 ----
    struct SonarRingActor : public ToyActor
    {
        juce::Point<float> at;
        explicit SonarRingActor (juce::Point<float> p) : at (p) { ttl = 0.8f; }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float t = age / ttl;
            const float r = 6.0f + t * 46.0f;
            g.setColour (PinkXP::sel.withAlpha (0.55f * (1.0f - t)));
            g.drawEllipse (at.x - r, at.y - r, r * 2.0f, r * 2.0f, 2.0f);
            g.setColour (PinkXP::sel.withAlpha (0.28f * (1.0f - t)));
            g.drawEllipse (at.x - r * 0.6f, at.y - r * 0.6f, r * 1.2f, r * 1.2f, 1.0f);
            g.setColour (PinkXP::ink.withAlpha (0.35f * (1.0f - t)));
            for (int i = 0; i < 3; ++i)
            {
                const float xx = at.x - r * 0.5f + i * r * 0.5f;
                const float yy = at.y + std::sin (age * 30.0f + i * 2.1f) * (1.0f - t) * 14.0f;
                g.drawLine (xx, yy - 6.0f, xx, yy + 6.0f, 1.0f);
            }
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - 60, at.y - 60, 120, 120); }
    };

    // ---- Spectrum：频率/音名气泡 ----
    struct NoteBubbleActor : public ToyActor
    {
        juce::Point<float> at;
        juce::String       note;
        NoteBubbleActor (juce::Point<float> p, const juce::String& n) : at (p), note (n) { ttl = 1.6f; }
        void step (float dt, ToyDirector&) override { age += dt; at.y -= 14.0f * dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float a = age < ttl * 0.8f ? 1.0f : 1.0f - (age - ttl * 0.8f) / (ttl * 0.2f);
            const int w = 112;
            auto b = juce::Rectangle<int> ((int) at.x - w / 2, (int) at.y - 24, w, 20);
            g.setOpacity (a);
            PinkXP::drawRaised (g, b, PinkXP::face);
            g.setColour (PinkXP::ink);
            g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
            g.drawText (note, b, juce::Justification::centred, false);
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - 60, at.y - 26, 120, 30); }
    };

    // ---- Waveform：人浪 ----
    struct WaveBandActor : public ToyActor
    {
        juce::Point<float>     at;
        juce::Rectangle<float> canvas;
        float spread = 0.0f;
        WaveBandActor (juce::Point<float> p, juce::Rectangle<float> c) : at (p), canvas (c) { ttl = 1.4f; }
        void step (float dt, ToyDirector&) override { age += dt; spread += dt * 180.0f; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float t = age / ttl;
            const float halfW = juce::jmin (spread, (float) canvas.getWidth() * 0.5f);
            g.setColour (PinkXP::sel.withAlpha (0.16f * (1.0f - t)));
            g.fillRect (at.x - halfW, canvas.getY(), halfW * 2.0f, (float) canvas.getHeight());

            const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Cheer), EmojiAtlas::Size::M);
            if (img.isValid())
            {
                for (int i = -2; i <= 2; ++i)
                {
                    const float xx = at.x + i * 34.0f;
                    if (xx < canvas.getX() - 20.0f || xx > canvas.getRight() + 20.0f) continue;
                    const float wave = std::sin (age * 9.0f - std::abs ((float) i) * 0.8f) * 10.0f;
                    const float yy = at.y + wave;
                    g.setOpacity (0.85f * (1.0f - t));
                    g.drawImage (img, (int) xx - 12, (int) yy - 12, 24, 24, 0, 0, img.getWidth(), img.getHeight());
                    g.setOpacity (1.0f);
                }
            }
        }
        juce::Rectangle<float> getBounds() const override { return canvas; }
    };

    // ---- StereoField：入侵者（Foe 角色，可点击击落，边界自适应）----
    struct InvaderActor : public ToyActor
    {
        juce::Point<float> pos;
        juce::Rectangle<float> area;
        float dirX = 1.0f;
        float stepT = 0.0f;
        bool  landed = false;
        InvaderActor (juce::Point<float> p, juce::Rectangle<float> a) : pos (p), area (a)
        {
            ttl = 12.0f;
            // 反馈 C4：初始移动方向随机（不再永远向右）
            dirX = (juce::Random::getSystemRandom().nextBool()) ? 1.0f : -1.0f;
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt; stepT += dt;
            if (landed)
            {
                if (age >= ttl) alive = false;
                return;
            }
            if (stepT > 0.5f)
            {
                stepT = 0.0f;
                pos.x += dirX * 22.0f;
                if (pos.x > area.getRight() - 10.0f)  { dirX = -1.0f; pos.y += 22.0f; }
                else if (pos.x < area.getX() + 10.0f) { dirX = 1.0f;  pos.y += 22.0f; }
            }
            if (pos.y > area.getBottom()) { landed = true; ttl = age + 1.5f; }
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Foe), EmojiAtlas::Size::L);
            if (! img.isValid()) return;
            const float a = landed ? juce::jmax (0.0f, 1.0f - (age - (ttl - 1.5f)) / 1.5f) : 1.0f;
            g.setOpacity (a);
            g.drawImage (img, (int) pos.x - 17, (int) pos.y - 17, 34, 34, 0, 0, img.getWidth(), img.getHeight());
            g.setOpacity (1.0f);
            if (landed)
            {
                g.setColour (ToyPalette::ink().withAlpha (a));
                g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
                g.drawText ("LANDED", juce::Rectangle<int> ((int) pos.x - 24, (int) pos.y + 16, 48, 12),
                            juce::Justification::centred, false);
            }
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (pos.x - 20, pos.y - 20, 40, 40); }

        // 点击击落（B-12 / E.4.3）
        bool onClick (juce::Point<float> p) override
        {
            if (landed || p.getDistanceFrom (pos) > 22.0f)
                return false;
            alive = false;
            return true;
        }
    };

    // ---- OscilloscopeWave：拍立得（性能优化 P2：快照构造时一次性烘焙到离屏图）----
    struct PolaroidActor : public ToyActor
    {
        static constexpr int W = 112, H = 112;
        juce::Point<float> pos;
        juce::Image snapshot;   // 一次性烘焙的相纸（白底+边框+波形/散点）
        float vy = 0.0f;
        float vx = 0.0f;

        PolaroidActor (juce::Point<float> p, const juce::Array<float>& l, const juce::Array<float>& r,
                       const juce::String&, bool sc)
            : pos (p)
        {
            ttl = 6.0f;
            vx = juce::Random::getSystemRandom().nextFloat() * 30.0f - 15.0f;   // 慢速横向飘

            // 性能优化 P2：原实现 paint 每帧重建 2048 点 Path（波形）或
            //   1024 次 fillRect+setColour（散点），是 X-Y 模式卡顿的直接原因。
            //   现在只在构造时烘焙一次，paint 每帧仅 1 次整数 blit。
            snapshot = juce::Image (juce::Image::ARGB, W, H, true);
            juce::Graphics sg (snapshot);

            sg.setColour (ToyPalette::glow());
            sg.fillRect (0, 0, W, H);
            sg.setColour (ToyPalette::shadow());
            sg.drawRect (0, 0, W, H, 1);

            const int iw = W - 12, ih = H - 12;
            const int ix = (W - iw) / 2, iy = (H - ih) / 2;

            if (sc)
            {
                // 李萨如散点（L→x，R→y），一次性画入
                sg.setColour (ToyPalette::accent().withAlpha (0.85f));
                const int n = juce::jmin (l.size(), r.size());
                for (int i = 0; i < n; i += 2)
                {
                    const float px = (float) ix + (l[(size_t) i] * 0.5f + 0.5f) * (float) iw;
                    const float py = (float) iy + (r[(size_t) i] * 0.5f + 0.5f) * (float) ih;
                    sg.fillRect (px - 1.0f, py - 1.0f, 2.0f, 2.0f);
                }
            }
            else
            {
                // 波形放大占满相纸（L 在下半、R 在上半）
                auto drawWave = [&](const juce::Array<float>& w, juce::Colour c, float yCenter, float amp)
                {
                    if (w.size() < 2) return;
                    juce::Path path;
                    const int n = w.size();
                    for (int i = 0; i < n; ++i)
                    {
                        const float fx = (float) ix + (float) i / (float) (n - 1) * (float) iw;
                        const float fy = yCenter + w[(size_t) i] * amp * (float) ih;
                        if (i == 0) path.startNewSubPath (fx, fy); else path.lineTo (fx, fy);
                    }
                    sg.setColour (c);
                    sg.strokePath (path, juce::PathStrokeType (1.2f));
                };
                drawWave (r, ToyPalette::accent2(), (float) iy + (float) ih * 0.30f, 0.28f);
                drawWave (l, ToyPalette::accent(),  (float) iy + (float) ih * 0.78f, 0.28f);
            }
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            vy += 220.0f * dt;          // 重力减小：慢速飘落
            pos.y += vy * dt;
            pos.x += vx * dt + std::sin (age * 1.4f) * 6.0f * dt;
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 4.5f) / 1.5f);

            if (age < 0.12f)
            {
                g.setColour (ToyPalette::glow().withAlpha (0.8f));
                g.fillRect ((int) pos.x - W / 2, (int) pos.y - H / 2, W, H);
            }

            // 每帧只画一次快照图
            g.setOpacity (a);
            g.drawImage (snapshot, (int) pos.x - W / 2, (int) pos.y - H / 2, W, H,
                         0, 0, W, H, false);
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (pos.x - 80, pos.y - 80, 160, 160); }
    };

    // ---- Spectrogram：贴纸（反馈 A4：滚动速度平滑，消除抖动）----
    struct StickerActor : public ToyActor
    {
        EmojiId id;
        juce::Point<float> pos;
        float scroll;
        float smoothScroll = 0.0f;
        StickerActor (EmojiId i, juce::Point<float> p, float s) : id (i), pos (p), scroll (s), smoothScroll (s)
        { ttl = 12.0f; }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            // 一阶低通平滑目标速度（吸收 dt 抖动导致的"一抖一抖"）
            smoothScroll = y2ktoy::expSmooth (smoothScroll, scroll, dt, 8.0f);
            pos.x -= smoothScroll * dt;
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const auto& img = dir.atlas->themedImage (id, EmojiAtlas::Size::M);
            if (! img.isValid()) return;
            const float a = age > ttl - 1.0f ? (ttl - age) : 1.0f;
            // 用浮点中心绘制（drawEmoji），避免整数取整的抖动
            drawEmoji (g, img, pos, 24.0f, 0.0f, a);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (pos.x - 14, pos.y - 14, 28, 28); }
    };

    // ---- Spectrogram3D：火山 ----
    struct VolcanoActor : public ToyActor
    {
        struct Ember { juce::Point<float> p, v; float life; };
        juce::Point<float> at;
        juce::Array<Ember> embers;
        VolcanoActor (juce::Point<float> p, juce::Point<float>) : at (p)
        {
            ttl = 3.0f;
            for (int i = 0; i < 18; ++i)
            {
                const float ang = juce::Random::getSystemRandom().nextFloat() * 3.14159f;
                const float spd = juce::Random::getSystemRandom().nextFloat() * 160.0f + 60.0f;
                embers.add ({ at, { std::cos (ang) * spd * 0.5f, -std::sin (ang) * spd },
                              0.9f + juce::Random::getSystemRandom().nextFloat() * 0.6f });
            }
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            for (auto& e : embers) { e.p += e.v * dt; e.v.y += 500.0f * dt; e.life -= dt; }
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 2.0f) / 1.0f);
            const auto& vol = dir.atlas->themedImage (EmojiId::Volcano, EmojiAtlas::Size::L);
            if (vol.isValid())
            {
                g.setOpacity (a);
                g.drawImage (vol, (int) at.x - 17, (int) at.y - 34, 34, 34, 0, 0, vol.getWidth(), vol.getHeight());
            }
            const auto& fire = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Flame), EmojiAtlas::Size::S);
            for (auto& e : embers)
            {
                if (e.life <= 0.0f) continue;
                const float ea = a * juce::jlimit (0.0f, 1.0f, e.life);
                if (fire.isValid())
                {
                    g.setOpacity (ea);
                    g.drawImage (fire, (int) e.p.x - 9, (int) e.p.y - 9, 18, 18, 0, 0, fire.getWidth(), fire.getHeight());
                }
                else
                {
                    g.setColour (ToyPalette::warn().withAlpha (ea));
                    g.fillEllipse (e.p.x - 3.0f, e.p.y - 3.0f, 6.0f, 6.0f);
                }
            }
            const auto& flag = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Pin), EmojiAtlas::Size::M);
            if (flag.isValid())
            {
                g.setOpacity (a);
                g.drawImage (flag, (int) at.x + 20, (int) at.y - 10, 24, 24, 0, 0, flag.getWidth(), flag.getHeight());
            }
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - 80, at.y - 80, 160, 160); }
    };

    // ---- 指针虚影（反馈 A3：像"眼花"一样，围绕真实指针生成扩散残影序列）----
    //   theta 使用与 Phase/VU 真实指针一致的数学角度约定：
    //   0 = 最左，π/2 = 正上，π = 最右（上半圆表盘）。
    struct GhostNeedleActor : public ToyActor
    {
        juce::Point<float> pivot;
        float radius;
        float theta;
        GhostNeedleActor (juce::Point<float> p, float r, float th) : pivot (p), radius (r), theta (th)
        {
            ttl = 1.6f;
        }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float t = age / ttl;
            const float a = 1.0f - t;
            const float R = juce::jmax (4.0f, radius);

            auto tip = [&](float th, float r)
            {
                return juce::Point<float> (pivot.x - std::cos (th) * r,
                                           pivot.y - std::sin (th) * r);
            };

            // 主指针
            g.setColour (PinkXP::sel.withAlpha (0.9f * a));
            g.drawLine (pivot.x, pivot.y, tip (theta, R).x, tip (theta, R).y, 2.0f);

            // 4 组残影：围绕真实指针角度 ± 偏移，随年龄向外扩散（眼花散开）
            for (int i = 1; i <= 4; ++i)
            {
                const float off = (float) i * 0.10f * t + 0.02f;
                const float ghostA = a * (0.45f - 0.08f * (float) i);
                g.setColour (PinkXP::sel.withAlpha (juce::jmax (0.0f, ghostA)));
                for (int side = -1; side <= 1; side += 2)
                {
                    const float th = theta + off * (float) side;
                    const auto tp = tip (th, R);
                    g.drawLine (pivot.x, pivot.y, tp.x, tp.y, 1.5f);
                }
            }
        }
        juce::Rectangle<float> getBounds() const override
        {
            return juce::Rectangle<float> (pivot.x - radius, pivot.y - radius, radius * 2.0f, radius * 2.0f);
        }
    };

    // ---- Loudness：打砖块碎片 ----
    struct BrickShardsActor : public ToyActor
    {
        struct Shard { juce::Point<float> p, v; float rot, rotV; };
        juce::Point<float> origin;
        juce::Colour colour;
        juce::Array<Shard> shards;
        BrickShardsActor (juce::Point<float> at, juce::Colour c, int count) : origin (at), colour (c)
        {
            ttl = 1.4f;
            for (int i = 0; i < count; ++i)
            {
                const float ang = juce::Random::getSystemRandom().nextFloat() * 6.28318f;
                const float spd = juce::Random::getSystemRandom().nextFloat() * 140.0f + 40.0f;
                shards.add ({ at,
                              { std::cos (ang) * spd, -std::abs (std::sin (ang)) * spd - 60.0f },
                              juce::Random::getSystemRandom().nextFloat() * 3.0f,
                              juce::Random::getSystemRandom().nextFloat() * 8.0f - 4.0f });
            }
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            for (auto& s : shards) { s.v.y += 900.0f * dt; s.p += s.v * dt; s.rot += s.rotV * dt; }
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 0.8f) / 0.6f);
            g.setColour (colour.withAlpha (a));
            for (auto& s : shards)
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.addTransform (juce::AffineTransform::rotation (s.rot, s.p.x, s.p.y));
                g.fillRect (s.p.x - 4.0f, s.p.y - 4.0f, 8.0f, 8.0f);
            }
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (origin.x - 60, origin.y - 60, 120, 240); }
    };

    // ---- VU：玻璃裂 ----
    struct CrackActor : public ToyActor
    {
        juce::Point<float> at;
        float radius;
        juce::Array<juce::Point<float>> ends;
        CrackActor (juce::Point<float> p, float r) : at (p), radius (r)
        {
            ttl = 4.0f;
            const int n = 5 + juce::Random::getSystemRandom().nextInt (3);   // 5..7
            for (int i = 0; i < n; ++i)
            {
                const float ang = (float) i / (float) n * 6.28318f
                                + juce::Random::getSystemRandom().nextFloat() * 0.5f;
                const float len = radius * (0.5f + juce::Random::getSystemRandom().nextFloat() * 0.5f);
                ends.add ({ at.x + std::cos (ang) * len, at.y + std::sin (ang) * len });
            }
        }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 2.5f) / 1.5f);
            g.setColour (ToyPalette::glow().withAlpha (0.7f * a));
            for (auto& e : ends)
                g.drawLine (at.x, at.y, e.x, e.y, 1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - radius, at.y - radius, radius * 2.0f, radius * 2.0f); }
    };

    // ---- VU：迪斯科灯（反馈修订：LED 本体变色 + 固定原点扩散光晕环，无 emoji/细线）----
    struct DiscoLedActor : public ToyActor
    {
        juce::Rectangle<float> led;
        explicit DiscoLedActor (juce::Rectangle<float> l) : led (l) { ttl = 3.0f; }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            // E.4.2：颜色序列固定 ok → warn → danger → accent（主题化），换色 ≤3 次/秒
            const juce::Colour kColours[] = {
                ToyPalette::ok(), ToyPalette::warn(), ToyPalette::danger(), ToyPalette::accent()
            };
            const int ci = ((int) (age * 3.0f)) % 4;
            const juce::Colour c = kColours[ci];
            const float fade = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 2.2f) / 0.8f);

            // LED 本体变色（含外圈描边，更像"灯"而不是方块）
            g.setColour (c.withAlpha (fade));
            g.fillEllipse (led);
            g.setColour (c.brighter (0.3f).withAlpha (fade));
            g.drawEllipse (led, 2.0f);

            // 从 LED 中心（原点不动）扩散的多圈光晕
            const float cx = led.getCentreX(), cy = led.getCentreY();
            for (int i = 0; i < 3; ++i)
            {
                const float phase = std::fmod (age * 1.2f + (float) i / 3.0f, 1.0f);
                const float r = 6.0f + phase * 22.0f;
                g.setColour (c.withAlpha (fade * (1.0f - phase) * 0.5f));
                g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.5f);
            }
        }
        juce::Rectangle<float> getBounds() const override { return led.expanded (30.0f); }
    };

    // ---- LUFS：老虎机滚轮（滚到真实值）----
    struct SlotRollerActor : public ToyActor
    {
        juce::Point<float> at;
        juce::String       digits;
        float roll = 0.0f;
        SlotRollerActor (juce::Point<float> p, const juce::String& d) : at (p), digits (d) { ttl = 2.0f; }
        void step (float dt, ToyDirector&) override { age += dt; roll += dt * 6.0f; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.4f) / 0.6f);
            g.setOpacity (a);
            const auto& img = dir.atlas->themedImage (EmojiId::SlotMachine, EmojiAtlas::Size::L);
            if (img.isValid())
                g.drawImage (img, (int) at.x - 20, (int) at.y - 20, 40, 40, 0, 0, img.getWidth(), img.getHeight());

            // 前 0.6 秒随机滚动，随后停在真实读数
            const juce::String shown = (age < 0.6f) ? juce::String (((int) (roll * 10.0f)) % 10) : digits;
            g.setColour (PinkXP::ink.withAlpha (a));
            g.setFont (PinkXP::getFont (12.0f, juce::Font::bold));
            g.drawText (shown, juce::Rectangle<int> ((int) at.x - 24, (int) at.y + 22, 48, 14),
                        juce::Justification::centred, false);
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - 24, at.y - 24, 48, 60); }
    };

    // ---- LUFS：萤火虫 ----
    struct FireflyActor : public ToyActor
    {
        juce::Point<float> pos;
        explicit FireflyActor (juce::Point<float> p) : pos (p) { ttl = 2.5f; }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            pos.x += std::sin (age * 6.0f) * 40.0f * dt;
            pos.y -= 60.0f * dt;
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.8f) / 0.7f);
            g.setColour (ToyPalette::warn().withAlpha (a * 0.3f));
            g.fillEllipse (pos.x - 8.0f, pos.y - 8.0f, 16.0f, 16.0f);
            g.setColour (ToyPalette::warn().withAlpha (a));
            g.fillEllipse (pos.x - 3.0f, pos.y - 3.0f, 6.0f, 6.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (pos.x - 12, pos.y - 12, 24, 24); }
    };

    // ---- TruePeak：Win95 弹窗 ----
    struct PopUpWindowActor : public ToyActor
    {
        juce::Point<float> at;
        juce::String text;
        EmojiId icon;
        PopUpWindowActor (juce::Point<float> p, const juce::String& t, EmojiId ic)
            : at (p), text (t), icon (ic) { ttl = 2.2f; }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = age < 0.2f ? juce::jmin (1.0f, age / 0.2f) : 1.0f;
            const float a2 = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.8f) / 0.4f);
            const int w = 170, h = 64;
            const int x = (int) at.x - w / 2, y = (int) at.y - h / 2;
            g.setOpacity (a * a2);
            PinkXP::drawRaised (g, juce::Rectangle<int> (x, y, w, h), PinkXP::face);
            g.setColour (ToyPalette::accent());
            g.fillRect (x, y, w, 14);
            g.setColour (PinkXP::ink);
            g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
            g.drawText ("Y2K TRUE PEAK", juce::Rectangle<int> (x + 2, y, w, 14), juce::Justification::centredLeft, false);

            const auto& img = dir.atlas->themedImage (icon, EmojiAtlas::Size::S);
            if (img.isValid())
                g.drawImage (img, x + 6, y + 20, 18, 18, 0, 0, img.getWidth(), img.getHeight());
            g.setFont (PinkXP::getFont (9.0f, juce::Font::plain));
            g.drawText (text, juce::Rectangle<int> (x + 28, y + 18, w - 32, 18),
                        juce::Justification::centredLeft, false);

            PinkXP::drawRaised (g, juce::Rectangle<int> (x + w / 2 - 20, y + h - 22, 40, 16), PinkXP::face);
            g.drawText ("OK", juce::Rectangle<int> (x + w / 2 - 20, y + h - 22, 40, 16), juce::Justification::centred, false);
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - 90, at.y - 36, 180, 72); }

        // OK 按钮真实可点（E.4.2）
        bool onClick (juce::Point<float> p) override
        {
            const int w = 170, h = 64;
            const juce::Rectangle<float> ok ((float) ((int) at.x - w / 2 + w / 2 - 20),
                                             (float) ((int) at.y - h / 2 + h - 22), 40.0f, 16.0f);
            if (ok.contains (p))
            {
                alive = false;
                return true;
            }
            return false;
        }
    };

    // ---- DR：读数框弹出（反馈修订：压扁动画改为简单 emoji 弹出）----
    struct SquashActor : public ToyActor
    {
        juce::Point<float> pos;
        float vy = 0.0f;
        float spin = 0.0f;
        explicit SquashActor (juce::Point<float> p) : pos (p)
        {
            ttl = 1.6f;
            vy = -220.0f;
            spin = juce::Random::getSystemRandom().nextFloat() * 4.0f - 2.0f;
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            vy += 900.0f * dt;
            pos.y += vy * dt;
            spin += 3.0f * dt;
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.2f) / 0.4f);
            const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Burst), EmojiAtlas::Size::M);
            if (img.isValid())
                drawEmoji (g, img, pos, 26.0f, spin, a);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (pos.x - 24, pos.y - 40, 48, 70); }
    };

    // ---- PhaseBalance：弹珠 ----
    struct BallActor : public ToyActor
    {
        juce::Rectangle<float> track;
        juce::Point<float> pos;
        float vx;
        int bounces = 0;
        BallActor (juce::Rectangle<float> t, juce::Point<float> c) : track (t), pos (c) { ttl = 3.0f; vx = 260.0f; }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            pos.x += vx * dt;
            if (pos.x < track.getX() + 8.0f)      { pos.x = track.getX() + 8.0f;      vx = -vx; ++bounces; }
            if (pos.x > track.getRight() - 8.0f)  { pos.x = track.getRight() - 8.0f;  vx = -vx; ++bounces; }
            vx *= 0.995f;
            if (age >= ttl || bounces >= 3) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 2.2f) / 0.8f);
            const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Orb), EmojiAtlas::Size::S);
            g.setOpacity (a);
            if (img.isValid())
                g.drawImage (img, (int) pos.x - 9, (int) (track.getCentreY() - 9), 18, 18, 0, 0, img.getWidth(), img.getHeight());
            else { g.setColour (ToyPalette::danger().withAlpha (a)); g.fillEllipse (pos.x - 6.0f, track.getCentreY() - 6.0f, 12.0f, 12.0f); }
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return track; }
    };

    // ---- PhaseBalance：橡皮筋 ----
    struct RubberBandActor : public ToyActor
    {
        juce::Rectangle<float> track;
        float springX = 0.0f;
        float vel = 0.0f;
        RubberBandActor (juce::Rectangle<float> t, juce::Point<float> c) : track (t)
        {
            ttl = 1.5f;
            springX = juce::jlimit (track.getX() + 4.0f, track.getRight() - 4.0f, c.x);
        }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            const float k = 80.0f, d = 8.0f;
            vel += (-k * (springX - track.getX()) - d * vel) * dt;
            springX += vel * dt;
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.0f) / 0.5f);
            g.setColour (PinkXP::sel.withAlpha (0.7f * a));
            const float cy = track.getCentreY();
            g.fillRect (track.getX(), cy - 3.0f, springX - track.getX(), 6.0f);
        }
        juce::Rectangle<float> getBounds() const override { return track; }
    };

    // ---- DynamicsMeters：幸运方块 ----
    struct CoinPopActor : public ToyActor
    {
        juce::Point<float> at;
        float vy = 0.0f;
        explicit CoinPopActor (juce::Point<float> p) : at (p) { ttl = 1.8f; vy = -260.0f; }
        void step (float dt, ToyDirector&) override { age += dt; vy += 900.0f * dt; at.y += vy * dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 1.3f) / 0.5f);
            const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Reward), EmojiAtlas::Size::M);
            if (img.isValid())
                drawEmoji (g, img, at, 26.0f, 0.0f, a);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - 20, at.y - 24, 40, 50); }
    };

    // ---- Crest：Line Rider ----
    struct CrestBallActor : public ToyActor
    {
        juce::Point<float> pos;
        float vy = 0.0f;
        explicit CrestBallActor (juce::Point<float> p) : pos (p) { ttl = 3.5f; }
        void step (float dt, ToyDirector&) override
        {
            age += dt;
            vy += 700.0f * dt;
            pos.y += vy * dt;
            pos.x -= 40.0f * dt;
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float a = 1.0f - juce::jlimit (0.0f, 1.0f, (age - 2.8f) / 0.7f);
            const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Orb), EmojiAtlas::Size::S);
            g.setOpacity (a);
            if (img.isValid())
                g.drawImage (img, (int) pos.x - 9, (int) pos.y - 9, 18, 18, 0, 0, img.getWidth(), img.getHeight());
            else { g.setColour (ToyPalette::glow().withAlpha (a)); g.fillEllipse (pos.x - 6.0f, pos.y - 6.0f, 12.0f, 12.0f); }
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (pos.x - 20, pos.y - 20, 40, 40); }
    };

    // ---- EQ：拼豆爆米花 ----
    struct BeadPopActor : public ToyActor
    {
        juce::Point<float> at;
        std::array<juce::Point<float>, 4> neighbours;
        BeadPopActor (juce::Point<float> p) : at (p)
        {
            ttl = 1.2f;
            neighbours = {{ { p.x - 7, p.y }, { p.x + 7, p.y }, { p.x, p.y - 7 }, { p.x, p.y + 7 } }};
        }
        void step (float dt, ToyDirector&) override { age += dt; if (age >= ttl) alive = false; }
        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            const float t = age / ttl;
            const float a = 1.0f - t;
            const float bounce = std::sin (t * 3.14159f) * 16.0f;
            const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Burst), EmojiAtlas::Size::M);
            g.setOpacity (a);
            if (img.isValid())
                g.drawImage (img, (int) at.x - 12, (int) (at.y - bounce) - 12, 24, 24, 0, 0, img.getWidth(), img.getHeight());
            else { g.setColour (PinkXP::pink400.withAlpha (a)); g.fillRect (at.x - 5.0f, at.y - bounce - 5.0f, 10.0f, 10.0f); }

            g.setColour (PinkXP::pink400.withAlpha (a * 0.7f));
            for (int i = 0; i < 4; ++i)
            {
                const float wob = std::sin (t * 3.14159f * 3.0f + (float) i) * 2.5f;
                g.fillRect (neighbours[(size_t) i].x - 4.0f, neighbours[(size_t) i].y - 4.0f + wob, 8.0f, 8.0f);
            }
            g.setOpacity (1.0f);
        }
        juce::Rectangle<float> getBounds() const override { return juce::Rectangle<float> (at.x - 20, at.y - 30, 40, 50); }
    };

    // ---- 读数气泡（E.5，所有模块共用）：Win95 凸起框，上浮淡出 ----
    //   反馈修订：无尾巴线；emoji 在左、文字在右（互不遮挡）；
    //   边框加高；整体钳制在模块内容区内不越界。
    struct ReadoutBubbleActor : public ToyActor
    {
        juce::Point<float>   at;
        juce::String         text;
        EmojiId              icon;
        juce::Colour         accentColour;
        juce::Rectangle<int> clampRect;

        ReadoutBubbleActor (juce::Point<float> p, const juce::String& t, EmojiId ic, juce::Colour c,
                            juce::Rectangle<int> clamp)
            : at (p), text (t), icon (ic), accentColour (c), clampRect (clamp) { ttl = 1.6f; }

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

            // 反馈 A1：宽度按文字真实像素宽度计算（不再按字符数估算，避免截断）
            const juce::Font f = PinkXP::getFont (11.0f, juce::Font::bold);
            g.setFont (f);
            const int textW = juce::roundToInt (f.getStringWidthFloat (text)) + 8;
            const int w = iconW + textW;
            auto b = juce::Rectangle<int> ((int) at.x - w / 2, (int) at.y - h - 8, w, h);

            // 钳制在模块内容区内（反馈：超出模块边界的部分不显示）
            if (! clampRect.isEmpty())
                b = b.constrainedWithin (clampRect);

            g.setOpacity (a);
            PinkXP::drawRaised (g, b, PinkXP::face);
            if (icon != EmojiId::Count)
            {
                const auto& img = dir.atlas->themedImage (icon, EmojiAtlas::Size::S);
                if (img.isValid())
                    g.drawImage (img, b.getX() + 3, b.getY() + (h - 16) / 2, 16, 16, 0, 0,
                                 img.getWidth(), img.getHeight());
            }
            g.setColour (PinkXP::ink);
            // 文字区域在 emoji 右侧，不重叠
            g.drawText (text, b.withTrimmedLeft (iconW).reduced (2, 0),
                        juce::Justification::centredLeft, false);
            g.setOpacity (1.0f);
        }

        juce::Rectangle<float> getBounds() const override
        {
            return juce::Rectangle<float> ((float) clampRect.getX(), (float) clampRect.getY(),
                                           (float) clampRect.getWidth(), (float) clampRect.getHeight());
        }
    };
}

// ==========================================================
// 工厂
// ==========================================================
juce::String toyFreqToNoteName (float freq) { return freqToNoteName (freq); }

std::unique_ptr<ToyActor> makeSonarRingActor (juce::Point<float> at)    { return std::make_unique<SonarRingActor> (at); }
std::unique_ptr<ToyActor> makeNoteBubbleActor (juce::Point<float> at, const juce::String& note)
                                                                        { return std::make_unique<NoteBubbleActor> (at, note); }
std::unique_ptr<ToyActor> makeWaveBandActor (juce::Point<float> at, juce::Rectangle<float> canvas)
                                                                        { return std::make_unique<WaveBandActor> (at, canvas); }
std::unique_ptr<ToyActor> makeInvaderActor (juce::Point<float> at, juce::Rectangle<float> area)
                                                                        { return std::make_unique<InvaderActor> (at, area); }
std::unique_ptr<ToyActor> makePolaroidActor (juce::Point<float> at, const juce::Array<float>& l, const juce::Array<float>& r,
                                             const juce::String& caption, bool scatter)
                                                                        { return std::make_unique<PolaroidActor> (at, l, r, caption, scatter); }
std::unique_ptr<ToyActor> makeStickerActor (EmojiId id, juce::Point<float> at, float scroll)
                                                                        { return std::make_unique<StickerActor> (id, at, scroll); }
std::unique_ptr<ToyActor> makeVolcanoActor (juce::Point<float> at, juce::Point<float> slant)
                                                                        { return std::make_unique<VolcanoActor> (at, slant); }
std::unique_ptr<ToyActor> makeGhostNeedleActor (juce::Point<float> pivot, float radius, float thetaRad)
                                                                        { return std::make_unique<GhostNeedleActor> (pivot, radius, thetaRad); }
std::unique_ptr<ToyActor> makeBrickShardsActor (juce::Point<float> at, juce::Colour c, int count)
                                                                        { return std::make_unique<BrickShardsActor> (at, c, count); }
std::unique_ptr<ToyActor> makeCrackActor (juce::Point<float> at, float radius)
                                                                        { return std::make_unique<CrackActor> (at, radius); }
std::unique_ptr<ToyActor> makeDiscoLedActor (juce::Rectangle<float> led)
                                                                        { return std::make_unique<DiscoLedActor> (led); }
std::unique_ptr<ToyActor> makeSlotRollerActor (juce::Point<float> at, const juce::String& digits)
                                                                        { return std::make_unique<SlotRollerActor> (at, digits); }
std::unique_ptr<ToyActor> makeFireflyActor (juce::Point<float> at)      { return std::make_unique<FireflyActor> (at); }
std::unique_ptr<ToyActor> makePopUpWindowActor (juce::Point<float> at, const juce::String& text, EmojiId icon)
                                                                        { return std::make_unique<PopUpWindowActor> (at, text, icon); }
std::unique_ptr<ToyActor> makeSquashActor (juce::Point<float> at)       { return std::make_unique<SquashActor> (at); }
std::unique_ptr<ToyActor> makeBallActor (juce::Rectangle<float> track, juce::Point<float> click)
                                                                        { return std::make_unique<BallActor> (track, click); }
std::unique_ptr<ToyActor> makeRubberBandActor (juce::Rectangle<float> track, juce::Point<float> click)
                                                                        { return std::make_unique<RubberBandActor> (track, click); }
std::unique_ptr<ToyActor> makeCoinPopActor (juce::Point<float> at)      { return std::make_unique<CoinPopActor> (at); }
std::unique_ptr<ToyActor> makeCrestBallActor (juce::Point<float> at)    { return std::make_unique<CrestBallActor> (at); }
std::unique_ptr<ToyActor> makeBeadPopActor (juce::Point<float> at)      { return std::make_unique<BeadPopActor> (at); }
std::unique_ptr<ToyActor> makeReadoutBubble (juce::Point<float> at, const juce::String& text,
                                             EmojiId icon, juce::Colour accentColour,
                                             juce::Rectangle<int> clampRect)
                                                                        { return std::make_unique<ReadoutBubbleActor> (at, text, icon, accentColour, clampRect); }
