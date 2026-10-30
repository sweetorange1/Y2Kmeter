#include "source/ui/toys/ToyKit.h"

// ==========================================================
// ToyCanvasFx —— 画布"表情喷泉"（§8.1）
//   主题池选择 + 喷发参数 + 桌面纹理"人格"参数表。
//   全部通过 ToyDirector 挂到 Editor 叠加层（自由 actor）。
// ==========================================================

namespace
{
    // 桌面纹理人格参数（§8.1）——同一引擎，不同参数表。
    struct Personality
    {
        float gravity  = 1100.0f;
        float damping  = 0.15f;
        float vyBias   = 0.0f;   // 额外上抛（Float/Love）
        float swayX    = 0.0f;   // 横向摇摆强度
        float swayFreq = 0.0f;   // 摇摆频率
        float quantize = 0.0f;   // 位置量化步长（Glitch）
        float alphaFlicker = 0.0f; // 透明度闪烁强度（Twinkle）
        bool  fall      = true;  // 下落 or 上浮
    };

    Personality personalityFor (PinkXP::DesktopPattern pat)
    {
        using DP = PinkXP::DesktopPattern;
        Personality p;
        switch (pat)
        {
            case DP::pixelStars:  p.gravity = 500.0f;  p.alphaFlicker = 0.5f; break;
            case DP::scanGrid:    p.quantize = 2.0f; break;
            case DP::bubbles:     p.fall = false; p.vyBias = -180.0f; p.swayX = 60.0f; p.swayFreq = 1.8f; break;
            case DP::hearts:      p.fall = false; p.vyBias = -140.0f; break;
            case DP::diagStripes: p.gravity = 900.0f; p.damping = 0.6f; break;
            case DP::zigzag:      p.swayX = 40.0f; p.swayFreq = 9.0f; break;
            case DP::crossHatch:  p.gravity = 1500.0f; break;
            default: break;
        }
        return p;
    }

    // 主题 → 画布喷泉池（附录 A.2）
    juce::Array<EmojiId> poolFor (PinkXP::ThemeId id)
    {
        using T = PinkXP::ThemeId;
        juce::Array<EmojiId> p;
        switch (id)
        {
            case T::blackPink:
                return { EmojiId::Heart, EmojiId::SparklingHeart, EmojiId::Star,
                         EmojiId::Butterfly, EmojiId::Bolt, EmojiId::Cherry,
                         EmojiId::Vhs, EmojiId::Sparkles, EmojiId::Cd, EmojiId::BlackHeart };
            case T::bubblegum:
                return { EmojiId::SparklingHeart, EmojiId::Strawberry, EmojiId::Ribbon,
                         EmojiId::Lollipop, EmojiId::Candy, EmojiId::Blossom, EmojiId::Star };
            case T::starlight:
                return { EmojiId::Star, EmojiId::Moon, EmojiId::Earth, EmojiId::Sparkles,
                         EmojiId::Dizzy, EmojiId::Comet, EmojiId::ShootingStar };
            case T::cyberLilac:
                return { EmojiId::Alien, EmojiId::Bolt, EmojiId::Cd, EmojiId::CrystalBall,
                         EmojiId::Headphone, EmojiId::Joystick, EmojiId::SpaceInvader };
            case T::tangerinePop:
                return { EmojiId::Orange, EmojiId::Sun, EmojiId::IceCream,
                         EmojiId::Sunglasses, EmojiId::Cocktail, EmojiId::Umbrella };
            case T::aquaPearl:
                return { EmojiId::Shell, EmojiId::Drop, EmojiId::Fish, EmojiId::OceanWave,
                         EmojiId::Dolphin, EmojiId::Crab };
            case T::matchaSoda:
                return { EmojiId::CupWithStraw, EmojiId::Leaf, EmojiId::GreenApple,
                         EmojiId::Tea, EmojiId::Herb, EmojiId::Clover };
            case T::jungle:
                return { EmojiId::Leaf, EmojiId::Frog, EmojiId::Butterfly,
                         EmojiId::Mushroom, EmojiId::PalmTree, EmojiId::Lizard };
            case T::winXP:
                return { EmojiId::Floppy, EmojiId::Cd, EmojiId::Mouse, EmojiId::Hourglass,
                         EmojiId::Folder, EmojiId::Desktop, EmojiId::Laptop };
            case T::crimsonNoir:
                return { EmojiId::Skull, EmojiId::Bat, EmojiId::Rose,
                         EmojiId::WiltedRose, EmojiId::Spider, EmojiId::BlackHeart };
            case T::voidGrey:
            case T::paperGrey:
                return { EmojiId::BlackSquare, EmojiId::WhiteSquare, EmojiId::ButtonSquare,
                         EmojiId::FrameSquare, EmojiId::Plus, EmojiId::Check };
            default:
                break;
        }
        // custom / 未覆盖 → 通用池
        return { EmojiId::Heart, EmojiId::SparklingHeart, EmojiId::Star, EmojiId::GlowStar,
                 EmojiId::Sparkles, EmojiId::Note, EmojiId::Notes, EmojiId::Sunglasses,
                 EmojiId::HeartEyes, EmojiId::Dizzy, EmojiId::Fire, EmojiId::PartyPopper, EmojiId::Gem };
    }
}

// ==========================================================
// EmojiParticle —— 自由 emoji 粒子（§7.2 自由 actor）
// ==========================================================
struct EmojiParticle : public ToyActor
{
    juce::Point<float>    pos;
    juce::Point<float>    vel;
    float spin     = 0.0f;
    float spinVel  = 0.0f;
    float alpha    = 1.0f;
    float hue      = 0.0f;
    float gravity  = 1100.0f;
    float damping  = 0.15f;
    float swayX    = 0.0f;
    float swayFreq = 0.0f;
    float swayPhase = 0.0f;
    float quantize = 0.0f;
    float alphaFlicker = 0.0f;
    juce::Rectangle<int> rainClipBounds;
    bool  fall     = true;
    bool  platformEnabled = true;
    int   maxBounces = 2;
    int   bounces = 0;
    bool  rainbow = false;
    bool  performanceSimplified = false;

    EmojiId id = EmojiId::Sparkles;
    int     sizeTier = 1;
    int     drawSize = 24;
    int     wallBounces = 0;   // C-5：左右墙各反弹一次
    float   squash = 0.0f;     // C-7：落地挤压（1→0 回弹）
    int     lastRotBucket_ = -1;  // P14：旋转分桶缓存（bucket 不变则零调用）

    explicit EmojiParticle (EmojiId i) : id (i) { ttl = 8.0f; }

    bool isPoppable() const override { return true; }
    bool isHighDetailForPerformance() const override { return ! performanceSimplified; }
    bool simplifyForPerformance() override
    {
        if (performanceSimplified)
            return false;
        performanceSimplified = true;
        return true;
    }
    void impulse (juce::Point<float> from, float strength) override
    {
        auto d = pos - from;
        const float dist = d.getDistanceFromOrigin();
        if (dist > 0.001f)
            vel += d / dist * strength;
    }

    void step (float dt, ToyDirector& dir) override
    {
        age += dt;
        if (age >= ttl) { alive = false; return; }

        const float prevY = pos.y;

        const float g = fall ? gravity : -gravity;
        float vx = vel.x, vy = vel.y;
        y2ktoy::integrate (pos.x, pos.y, vx, vy, dt, g, damping, 0.05f);

        // 横向摇摆（Bubble Float 等）
        if (swayFreq > 0.0f)
        {
            swayPhase += swayFreq * dt;
            pos.x += swayX * std::sin (swayPhase) * dt;
        }
        vel.x = vx;
        vel.y = vy;

        // 位置量化（Glitch）
        if (quantize > 0.0f)
        {
            pos.x = std::round (pos.x / quantize) * quantize;
            pos.y = std::round (pos.y / quantize) * quantize;
        }

        spin += spinVel * dt;
        squash = juce::jmax (0.0f, squash - 5.0f * dt);   // C-7：约 0.2 秒回弹

        // 性能优化 P1 + P14：step 阶段预热旋转分桶（烘焙不进 paint 回调）；
        //   bucket 未变化时零调用，省掉每帧每粒子的锁 + hash 查找。
        if (std::abs (spin) > 0.02f)
        {
            const int b = EmojiAtlas::bucketOf (spin);
            if (b != lastRotBucket_)
            {
                lastRotBucket_ = b;
                dir.atlas->ensureRotated (id, (EmojiAtlas::Size) sizeTier, spin);
            }
        }

        // 平台碰撞（模块标题栏上沿，仅下落时，带 x 范围）
        if (platformEnabled && fall && bounces < maxBounces && prevY < pos.y)
        {
            for (const auto& pf : dir.platformTops)
            {
                if (y2ktoy::platformHitX (pos.x, pf.x0, pf.x1, prevY, pos.y, pf.y, vel.y))
                {
                    pos.y = pf.y;
                    vel.y = -vel.y * 0.45f;   // 弹性 0.45
                    squash = 1.0f;            // C-7：落地挤压回弹
                    ++bounces;
                    break;
                }
            }
        }

        // 出窗口判定（C-5）：左右墙各反弹一次（贴墙即反弹，不留越界余量），底部销毁，顶部不限制
        const int w = dir.editor.getWidth();
        const int h = dir.editor.getHeight();
        const float margin = (float) drawSize;
        if (pos.x < 0.0f || pos.x > (float) w)
        {
            if (wallBounces < 1)
            {
                ++wallBounces;
                pos.x = juce::jlimit (0.0f, (float) w, pos.x);
                vel.x = -vel.x * 0.6f;   // 弹性 0.6
            }
            else
            {
                alive = false;
            }
        }
        if (pos.y > h + margin)
            alive = false;
        if (! rainClipBounds.isEmpty() && pos.y - margin >= (float) rainClipBounds.getBottom())
            alive = false;

        // 透明闪烁（Twinkle）
        if (alphaFlicker > 0.0f)
            alpha = 0.55f + 0.45f * std::sin (age * 6.0f);
    }

    void paint (juce::Graphics& g, ToyDirector& dir) override
    {
        g.setOpacity (1.0f);
        if (rainClipBounds.isEmpty())
        {
            paintContent (g, dir);
            return;
        }

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (rainClipBounds);
        paintContent (g, dir);
    }

    void paintContent (juce::Graphics& g, ToyDirector& dir)
    {
        if (performanceSimplified)
        {
            const float dotSize = 4.0f + (float) juce::jmin (sizeTier, 2);
            g.setColour ((rainbow ? juce::Colour::fromHSV (hue, 0.7f, 1.0f, 1.0f)
                                  : (sizeTier == 0 ? ToyPalette::accent() : ToyPalette::accent2()))
                         .withAlpha (alpha * 0.85f));
            const int pixelSize = juce::roundToInt (dotSize);
            g.fillRect (juce::roundToInt (pos.x - dotSize * 0.5f),
                        juce::roundToInt (pos.y - dotSize * 0.5f),
                        pixelSize, pixelSize);
            g.setOpacity (1.0f);
            return;
        }

        const float s = (float) drawSize;

        // 彩虹着色（Y2K MODE）：叠加一层随时间旋转的色相光晕
        if (rainbow)
        {
            hue = std::fmod (hue + 0.02f, 1.0f);
            g.setColour (juce::Colour::fromHSV (hue, 0.7f, 1.0f, 0.25f));
            g.fillEllipse (pos.x - s * 0.6f, pos.y - s * 0.6f, s * 1.2f, s * 1.2f);
        }

        const float scaleY = 1.0f - squash * 0.35f;

        // 性能优化 P1：旋转且无挤压 → 查预烘焙角度分桶图（paint 内零烘焙、零风险），
        //   命中则整数坐标 blit；未命中走慢路径（step 阶段已预热，最多首帧 miss 一次）
        if (std::abs (spin) > 0.02f && scaleY > 0.99f)
        {
            const juce::Image rimg = dir.atlas->rotatedCached (id, (EmojiAtlas::Size) sizeTier, spin);
            if (rimg.isValid())
            {
                g.setOpacity (alpha);
                const int px = juce::roundToInt (pos.x - s * 0.5f);
                const int py = juce::roundToInt (pos.y - s * 0.5f);
                // P15：目标尺寸 == 源图尺寸时走 drawImageAt（1:1 无缩放快路径）
                if (juce::roundToInt (s) == rimg.getWidth())
                    g.drawImageAt (rimg, px, py, false);
                else
                    g.drawImage (rimg, px, py, (int) s, (int) s,
                                 0, 0, rimg.getWidth(), rimg.getHeight(), false);
                g.setOpacity (1.0f);
                return;
            }
        }

        // 慢路径：落地挤压 / 无旋转 → 统一绘制入口（FIX-1）
        //   未烘焙完成时跳过本帧（绝不触发 bakeOne 的 D2D 渲染）
        if (dir.atlas->isBaked (id, (EmojiAtlas::Size) sizeTier))
        {
            const auto& image = dir.atlas->themedImage (id, (EmojiAtlas::Size) sizeTier);
            if (std::abs (spin) <= 0.02f && scaleY > 0.99f
                && juce::roundToInt (s) == image.getWidth()
                && image.getWidth() == image.getHeight())
            {
                g.setOpacity (alpha);
                g.drawImageAt (image, juce::roundToInt (pos.x - s * 0.5f),
                               juce::roundToInt (pos.y - s * 0.5f), false);
                g.setOpacity (1.0f);
                return;
            }

            drawEmoji (g, image, pos, s, spin, alpha, scaleY);
        }
    }

    juce::Rectangle<float> getBounds() const override
    {
        return juce::Rectangle<float> (pos.x - drawSize, pos.y - drawSize,
                                       drawSize * 2.0f, drawSize * 2.0f);
    }
};

// ==========================================================
// 喷发实现（挂到 ToyDirector）
// ==========================================================
static void spawnFountain (ToyDirector& dir, juce::Point<float> at, float charge, bool rainbow)
{
    using DP = PinkXP::DesktopPattern;
    const PinkXP::Theme& theme = PinkXP::getCurrentTheme();
    const Personality pers = personalityFor (theme.desktopPattern);
    const juce::Array<EmojiId> pool = poolFor (theme.id);

    if (pool.isEmpty())
        return;

    // 数量按档位 + 音频能量 + 蓄力缩放（§8.1）
    int minN = 7, maxN = 14;
    switch (dir.getLevel())
    {
        case ToyLevel::Chill:  minN = 5;  maxN = 8;  break;
        case ToyLevel::Normal: minN = 7;  maxN = 14; break;
        case ToyLevel::Party:  minN = 10; maxN = 20; break;
        case ToyLevel::Off:    return;
    }

    const float energy = dir.audioEnergy();
    const float chargeBoost = 1.0f + charge;   // 0..1 → 1..2
    int count = juce::roundToInt ((minN + (maxN - minN) * energy) * chargeBoost);
    count = juce::jlimit (minN, maxN, count);
    // 反馈修订：删除 P4 软上限——每次点击喷发量恒定（不再"越点越少"），
    //   堆积控制统一由 onFrame 的 FIFO 硬上限淘汰最老粒子完成。

    // 初速按窗口高度换算（反馈 A5：降低初速与重力，峰值高度 12%..24%、重力 ×0.75）
    const float h = (float) juce::jmax (1, dir.editor.getHeight());
    const float peakFrac = 0.12f + 0.12f * energy;
    const float g = pers.gravity * dir.uiScale() * 0.75f;
    const float v0 = std::sqrt (2.0f * g * h * peakFrac);

    // C-6：连击喷口形态（5+ 宽扇形 / 10+ 双向对喷 / 20+ 360° 环形）
    const int combo = dir.comboCountForFx();

    for (int i = 0; i < count; ++i)
    {
        const EmojiId id = pool[juce::Random::getSystemRandom().nextInt (pool.size())];
        auto p = std::make_unique<EmojiParticle> (id);

        float ang;
        if (combo >= 20)          ang = juce::Random::getSystemRandom().nextFloat() * 6.28318f;
        else if (combo >= 10)     ang = (i % 2 == 0 ? 0.0f : 3.14159f) + juce::Random::getSystemRandom().nextFloat() * 1.22f - 0.61f;
        else if (combo >= 5)      ang = juce::Random::getSystemRandom().nextFloat() * 2.44f - 1.22f;   // ±70°
        else                      ang = juce::Random::getSystemRandom().nextFloat() * 1.22f - 0.61f;   // ±35°

        const float speed = v0 * (0.75f + 0.5f * juce::Random::getSystemRandom().nextFloat());
        p->vel = { std::sin (ang) * speed, -std::cos (ang) * speed + pers.vyBias * dir.uiScale() };

        p->pos = at;
        p->gravity  = pers.gravity * dir.uiScale() * 0.75f;   // 反馈 A5：重力降低
        p->damping  = pers.damping;
        p->swayX    = pers.swayX * dir.uiScale();
        p->swayFreq = pers.swayFreq;
        p->quantize = pers.quantize;
        // Normal/Party 保持常亮，关闭呼吸式透明闪烁。
        const auto level = dir.getLevel();
        p->alphaFlicker = (level == ToyLevel::Normal || level == ToyLevel::Party) ? 0.0f : pers.alphaFlicker;
        // 喷泉统一向下落（桌面纹理"上浮"人格仅用于桌面，不用于喷泉）
        p->fall     = true;
        p->rainbow  = rainbow;
        p->spinVel  = 0.0f;
        p->sizeTier = juce::Random::getSystemRandom().nextInt (3);
        p->drawSize = dir.atlas->drawSize ((EmojiAtlas::Size) p->sizeTier);

        dir.clock->freeActors.push_back (std::move (p));
    }
}

// 小爆发（C-2 持续喷，1~2 个），向上小角度
void ToyDirector::spawnSmallBurst (juce::Point<float> at, float speedBoost)
{
    const PinkXP::Theme& theme = PinkXP::getCurrentTheme();
    const juce::Array<EmojiId> pool = poolFor (theme.id);
    if (pool.isEmpty()) return;

    const Personality pers = personalityFor (theme.desktopPattern);
    const float g = pers.gravity * uiScale() * 0.75f;   // 反馈 A5：重力降低
    const float h = (float) juce::jmax (1, editor.getHeight());
    const float v0 = std::sqrt (2.0f * g * h * 0.14f) * speedBoost;

    const int n = 1 + juce::Random::getSystemRandom().nextInt (2);
    for (int i = 0; i < n; ++i)
    {
        auto p = std::make_unique<EmojiParticle> (pool[juce::Random::getSystemRandom().nextInt (pool.size())]);
        const float ang = juce::Random::getSystemRandom().nextFloat() * 0.7f - 0.35f;
        const float speed = v0 * (0.8f + 0.4f * juce::Random::getSystemRandom().nextFloat());
        p->vel = { std::sin (ang) * speed, -std::cos (ang) * speed };
        p->pos = at;
        p->gravity  = pers.gravity * uiScale() * 0.75f;   // 反馈 A5
        p->damping  = pers.damping;
        p->fall     = true;   // 喷泉统一向下
        p->sizeTier = 0;
        p->drawSize = atlas->drawSize (EmojiAtlas::Size::S);
        clock->freeActors.push_back (std::move (p));
    }
    ensureOverlay();
}

void ToyDirector::spawnCanvasFountain (juce::Point<float> editorPos, float charge)
{
    const bool rainbow = (level_ == ToyLevel::Party);
    spawnFountain (*this, editorPos, charge, rainbow);
    ++totalPokes_;

    // 性能日志（节流：粒子堆积时每秒最多一条，供持续点击场景比对）
    if ((int) clock->freeActors.size() > 80)
    {
        static double lastLog = 0.0;
        const double nowT = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (nowT - lastLog > 1.0)
        {
            lastLog = nowT;
            toyperf::log ("fountain active: freeActors=" + juce::String ((int) clock->freeActors.size()));
        }
    }

    // 画布一次喷发 ≥ 8 精灵 → 宠物 startled（冷却 8 秒，§8.4）
    if (level_ == ToyLevel::Normal || level_ == ToyLevel::Party)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (now - lastStartledTime_ >= 8.0)
        {
            lastStartledTime_ = now;
            notifyPetReaction (33);   // startled
        }
    }
}

// ==========================================================
// 通用兜底（§8.5）
// ==========================================================
void ToyDirector::spawnGenericFallback (ModulePanel& panel, juce::Point<float> panelPos)
{
    ToyContext ctx;
    ctx.director = this;
    ctx.panel    = &panel;
    ctx.state    = panel.toyState.get();
    ctx.hit.pos  = panelPos;
    ctx.addModuleActor (std::make_unique<SparkBurstActor> (panelPos, 5, 1.0f));
}

// ==========================================================
// 工厂：自由 emoji 粒子
// ==========================================================
std::unique_ptr<ToyActor> makeEmojiParticle (EmojiId id, juce::Point<float> at,
                                             juce::Point<float> v, int sizeTier,
                                             ToyDirector& dir, float ttl, float gravity)
{
    auto p = std::make_unique<EmojiParticle> (id);
    p->pos = at;
    p->vel = v;
    p->ttl = ttl;
    p->gravity = gravity;
    p->sizeTier = juce::jlimit (0, 2, sizeTier);
    p->drawSize = dir.atlas->drawSize ((EmojiAtlas::Size) p->sizeTier);
    return p;
}

std::unique_ptr<ToyActor> makeConfettiRainParticle (juce::Point<float> at,
                                                    juce::Point<float> velocity,
                                                    ToyDirector& dir, bool rainbow,
                                                    juce::Rectangle<int> clipBounds)
{
    auto p = std::make_unique<EmojiParticle> (EmojiId::PartyPopper);
    p->pos = at;
    p->vel = velocity;
    p->ttl = 4.0f;
    p->gravity = 1500.0f * dir.uiScale();
    p->damping = 0.05f;
    p->platformEnabled = false;
    p->rainbow = rainbow;
    p->rainClipBounds = clipBounds;
    p->sizeTier = juce::Random::getSystemRandom().nextInt (2);
    p->drawSize = dir.atlas->drawSize ((EmojiAtlas::Size) p->sizeTier);
    return p;
}
