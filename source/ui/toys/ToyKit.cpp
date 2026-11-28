#include "source/ui/toys/ToyKit.h"
#include "source/i18n/Localisation.h"
#include "source/ui/modules/VirtuPetModule.h"
#include "source/Y2KLogging.h"

// ==========================================================
// ToyKit —— 监听器、路由、时钟、叠加层、连击、彩蛋（§7 / §8.4）
// ==========================================================

namespace
{
    // 连续互动窗口与升级步长：间隔 < 3 s 视为连续，每满 5 次升一档；
    // 3 s 无互动（timerCallback）连击归零并降回 Chill。
    constexpr double kInteractionWindowSec = 3.0;
    constexpr int    kUpgradeEveryClicks   = 5;
}

// ==========================================================
// 档位转换
// ==========================================================
juce::String toyLevelToString (ToyLevel l)
{
    switch (l)
    {
        case ToyLevel::Off:    return "Off";
        case ToyLevel::Chill:  return "Chill";
        case ToyLevel::Normal: return "Normal";
        case ToyLevel::Party:  return "Party";
    }
    return "Normal";
}

ToyLevel toyLevelFromInt (int v, bool* ok)
{
    if (ok) *ok = true;
    switch (v)
    {
        case 0: return ToyLevel::Off;
        case 1: return ToyLevel::Chill;
        case 2: return ToyLevel::Normal;
        case 3: return ToyLevel::Party;
    }
    if (ok) *ok = false;
    return ToyLevel::Normal;
}

ToyLevel toyLevelFromString (const juce::String& s, bool* ok)
{
    const auto ls = s.toLowerCase().trim();
    if (ls == "off")    { if (ok) *ok = true; return ToyLevel::Off; }
    if (ls == "chill")  { if (ok) *ok = true; return ToyLevel::Chill; }
    if (ls == "normal") { if (ok) *ok = true; return ToyLevel::Normal; }
    if (ls == "party")  { if (ok) *ok = true; return ToyLevel::Party; }
    if (ok) *ok = false;
    return ToyLevel::Normal;
}

// ==========================================================
// ModuleToyState
// ==========================================================
ModuleToyState::ModuleToyState (ModulePanel& o, ToyDirector& d)
    : owner (o), director (&d) {}

void ModuleToyState::clear()
{
    actors.clear();
}

juce::Rectangle<int> ModuleToyState::step (float dt)
{
    juce::Rectangle<int> dirty;
    for (auto it = actors.begin(); it != actors.end();)
    {
        auto& a = *it;
        a->prevBounds = a->getBounds();
        a->hasPrevBounds = true;
        a->step (dt, *director);

        const auto cur = a->getBounds().getSmallestIntegerContainer();
        if (! a->alive)
        {
            dirty = dirty.getUnion (cur);
            it = actors.erase (it);
            continue;
        }
        dirty = dirty.getUnion (cur);
        if (a->hasPrevBounds)
            dirty = dirty.getUnion (a->prevBounds.getSmallestIntegerContainer());
        ++it;
    }
    return dirty;
}

void ModuleToyState::paint (juce::Graphics& g)
{
    const auto content = owner.getContentBounds();
    if (content.getWidth() <= 0 || content.getHeight() <= 0)
        return;

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (content);
    const auto clip = g.getClipBounds();
    for (auto& a : actors)
        if (a->getBounds().getSmallestIntegerContainer().intersects (clip))
            a->paint (g, *director);
}

// ==========================================================
// ToyContext
// ==========================================================
ToyLevel ToyContext::level() const { return director ? director->getLevel() : ToyLevel::Normal; }
float    ToyContext::audioEnergy() const { return director ? director->audioEnergy() : 0.5f; }
float    ToyContext::uiScale() const noexcept { return director ? director->uiScale() : 1.0f; }

juce::Rectangle<int> ToyContext::contentBounds() const
{
    return panel ? panel->getContentBounds() : juce::Rectangle<int>();
}

void ToyContext::addModuleActor (std::unique_ptr<ToyActor> a)
{
    if (state && a != nullptr) state->actors.push_back (std::move (a));
}

void ToyContext::addModuleActorCapped (std::unique_ptr<ToyActor> a, size_t maxActors)
{
    if (! state || a == nullptr) return;
    const size_t cap = (maxActors < 1) ? 1 : maxActors;
    while (state->actors.size() >= cap)
        state->actors.erase (state->actors.begin());
    state->actors.push_back (std::move (a));
}

// 坐标偏移包装（R11）：把面板局部坐标的 actor 转成 Editor 坐标，
// 供 promoteToFree 使用；paint 时用 Graphics 变换平移，inner 无需改动。
namespace
{
    struct OffsetActor : public ToyActor
    {
        std::unique_ptr<ToyActor> inner;
        juce::Point<float> offset;

        OffsetActor (std::unique_ptr<ToyActor> a, juce::Point<float> off)
            : inner (std::move (a)), offset (off) {}

        void step (float dt, ToyDirector& dir) override
        {
            inner->step (dt, dir);
            age = inner->age;
            ttl = inner->ttl;
            alive = inner->alive;
        }

        void paint (juce::Graphics& g, ToyDirector& dir) override
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.addTransform (juce::AffineTransform::translation (offset.x, offset.y));
            inner->paint (g, dir);
        }

        juce::Rectangle<float> getBounds() const override
        {
            return inner->getBounds().translated (offset.x, offset.y);
        }

        bool onClick (juce::Point<float> p) override
        {
            return inner->onClick (p.translated (-offset.x, -offset.y));
        }

        bool isPoppable() const override { return inner->isPoppable(); }

        void impulse (juce::Point<float> from, float strength) override
        {
            inner->impulse (from.translated (-offset.x, -offset.y), strength);
        }
    };
}

void ToyContext::promoteToFree (std::unique_ptr<ToyActor> a)
{
    if (! director) return;
    if (panel && ! panel->isFloating())
    {
        // 面板局部坐标 → Editor 坐标（R11）
        const auto offset = director->editor.getLocalPoint (panel, juce::Point<int> (0, 0)).toFloat();
        director->clock->freeActors.push_back (std::make_unique<OffsetActor> (std::move (a), offset));
        director->ensureOverlay();
    }
    else
    {
        addModuleActor (std::move (a));
    }
}

void ToyContext::bubble (juce::Point<float> at, const juce::String& text, ToyRole r)
{
    if (! state)
        return;

    // 单面板最多保留 2 个并发读数气泡，防止连点时气泡叠加放大面板重绘区域
    int bubbleCount = 0;
    for (const auto& a : state->actors)
        if (a != nullptr && a->isReadoutBubble())
            ++bubbleCount;
    while (bubbleCount >= 2)
    {
        auto it = std::find_if (state->actors.begin(), state->actors.end(),
                                [] (const auto& a) { return a != nullptr && a->isReadoutBubble(); });
        if (it == state->actors.end())
            break;
        state->actors.erase (it);
        --bubbleCount;
    }

    // clamp 到模块内容区（反馈：气泡超出模块边界的部分不显示）
    const auto clamp = panel ? panel->getContentBounds() : juce::Rectangle<int>();
    addModuleActor (makeReadoutBubble (at, text, toyRoleEmoji (r), ToyPalette::accent(), clamp));
}

void ToyContext::spawnEmoji (EmojiId id, juce::Point<float> at,
                             juce::Point<float> v, int sizeTier)
{
    if (! director) return;
    if (panel && ! panel->isFloating())
    {
        const auto editorPos = director->editor.getLocalPoint (panel, at.toInt()).toFloat();
        director->clock->freeActors.push_back (
            makeEmojiParticle (id, editorPos, v, sizeTier, *director));
        if (director->isGlToyActive())
            director->requestGlToyFrame();
        else
            director->ensureOverlay();
    }
    else
    {
        addModuleActor (std::make_unique<SparkBurstActor> (at, 3, 0.6f));
    }
}

// ==========================================================
// ToyClock
// ==========================================================
ToyClock::ToyClock (ToyDirector& dir) : director (dir) {}

ToyClock::DiagSnapshot ToyClock::takeDiagSnapshot() noexcept
{
    DiagSnapshot s;
    s.freeActors           = (int) freeActors.size();
    s.fullEmoji            = highDetailFreeActorCount_;
    s.degradedFreeActors   = degradedFreeActorCount_;
    s.emojiTarget          = degradation_.targetFullEmojiCount();
    s.smoothedFps          = degradation_.smoothedFps();
    s.overlayPaintMs       = overlayPaintTotalMs_;
    s.overlayPaints        = overlayPaintCalls_;
    s.overlayActorsPainted = overlayActorsPainted_;
    s.glToyActive          = director.isGlToyActive();
    overlayPaintTotalMs_   = 0.0;
    overlayPaintCalls_     = 0;
    overlayActorsPainted_  = 0;
    return s;
}

void ToyClock::trackPanel (ModulePanel& p)
{
    for (auto& sp : panels)
        if (sp == &p) return;
    panels.push_back (juce::Component::SafePointer<ModulePanel> (&p));
}

void ToyClock::cancelPanel (ModulePanel& p)
{
    if (p.toyState)
        p.toyState->clear();
    for (auto it = panels.begin(); it != panels.end();)
    {
        if (*it == &p) it = panels.erase (it);
        else           ++it;
    }
}

void ToyClock::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    // 性能日志 session 行：延迟到首帧（消息循环已稳定）才写，构造期零文件 IO
    //   Release 下 TOYPERF_BEGIN_SESSION() 展开为空（ToyPerf.h 编译期门控）。
    static bool perfSessionBegun = false;
    if (! perfSessionBegun)
    {
        perfSessionBegun = true;
        TOYPERF_BEGIN_SESSION();
    }

    const double elapsed = lastFrameTime_ > 0.0
                         ? juce::jmax (0.0, frame.timeSec - lastFrameTime_)
                         : (double) frame.dtSec;
    lastFrameTime_ = frame.timeSec;

    const float dt = frame.dtSec;
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    // C-2：按住持续喷（花洒）
    if (director.getLevel() != ToyLevel::Off)
    {
        director.sprayTick (frame.timeSec);
        director.tickConfettiRain (frame.timeSec);
    }

    const bool glActive = director.isGlToyActive();
    degradation_.setGlBatched (glActive);
    degradation_.update (elapsed, (int) freeActors.size());

    // 刷新平台上沿（Editor 坐标，带 x 范围）。
    // FIX-4 / B-13：仅当存在活动自由 actor 时才遍历模块，空闲零开销。
    // 性能优化 P3：平台几何缓存 0.5 秒（模块不常移动，无需每帧遍历）。
    if (! freeActors.empty() && frame.timeSec - platformCacheTime_ >= 0.5)
    {
        platformCacheTime_ = frame.timeSec;
        director.platformTops.clear();
        for (int i = 0; i < director.workspace.getNumModules(); ++i)
        {
            auto* m = director.workspace.getModule (i);
            if (m && ! m->isFloating())
            {
                const auto tl = director.editor.getLocalPoint (m, juce::Point<int> (0, 0));
                const auto br = director.editor.getLocalPoint (m, juce::Point<int> (m->getWidth(), m->getHeight()));
                director.platformTops.push_back ({ (float) tl.x, (float) br.x, (float) tl.y + 2.0f });
            }
        }
    }

    // 性能优化 P12v2：FIFO 硬上限（400）——超过时淘汰"最先创建"（age 最大）的粒子，
    //   保证每次点击喷发量恒定、屏上始终是最新的粒子。
    constexpr int kFreeActorLimit = y2ktoy::kMaxToySprites;
    if ((int) freeActors.size() > kFreeActorLimit)
    {
        std::nth_element (freeActors.begin(), freeActors.begin() + kFreeActorLimit, freeActors.end(),
                          [](const auto& a, const auto& b) { return a->age < b->age; });
        freeActors.erase (freeActors.begin() + kFreeActorLimit, freeActors.end());
    }

    const auto visibleBounds = director.editor.getLocalBounds().toFloat();
    bool anyCpuOverlay = false;

    // 1) 推进所有自由 actor（swap-and-pop 删除失效粒子，避免 O(N^2) vector 中间搬移）
    juce::RectangleList<int> cpuDirtyRl;
    if (! freeActors.empty())
    {
        const float actorDt = (float) elapsed;
        for (size_t i = 0; i < freeActors.size();)
        {
            auto& a = freeActors[i];
            if (! glActive)
            {
                a->prevBounds = a->getBounds();
                a->hasPrevBounds = true;
            }
            a->step (actorDt, director);

            if (! a->alive)
            {
                if (! glActive)
                {
                    cpuDirtyRl.add (a->getBounds().getSmallestIntegerContainer());
                    if (a->hasPrevBounds)
                        cpuDirtyRl.add (a->prevBounds.getSmallestIntegerContainer());
                }
                if (i + 1 < freeActors.size())
                    freeActors[i] = std::move (freeActors.back());
                freeActors.pop_back();
                continue;
            }
            if (! glActive)
            {
                cpuDirtyRl.add (a->getBounds().getSmallestIntegerContainer());
                if (a->hasPrevBounds)
                    cpuDirtyRl.add (a->prevBounds.getSmallestIntegerContainer());
            }
            ++i;
        }
    }

    // 2) 目标 FPS 相对迟滞降级与可逆恢复（先于 GL 快照提取，确保本帧即生效）
    visibleFreeActorCount_ = 0;
    highDetailFreeActorCount_ = 0;
    degradedFreeActorCount_ = 0;
    for (const auto& actor : freeActors)
    {
        if (actor != nullptr && actor->alive)
        {
            if (actor->isPoppable() && ! actor->isHighDetailForPerformance())
                ++degradedFreeActorCount_;
            if (actor->getBounds().intersects (visibleBounds))
            {
                ++visibleFreeActorCount_;
                if (actor->isHighDetailForPerformance())
                    ++highDetailFreeActorCount_;
            }
        }
    }

    const int targetFull = degradation_.targetFullEmojiCount();
    if (highDetailFreeActorCount_ > targetFull)
    {
        std::vector<ToyActor*> oldestFirst;
        oldestFirst.reserve ((size_t) highDetailFreeActorCount_);
        for (auto& actor : freeActors)
            if (actor != nullptr && actor->alive
                && actor->getBounds().intersects (visibleBounds)
                && actor->isHighDetailForPerformance())
                oldestFirst.push_back (actor.get());

        std::sort (oldestFirst.begin(), oldestFirst.end(), [] (const ToyActor* a, const ToyActor* b)
        {
            return a->age > b->age;
        });

        const int simplifyCount = juce::jmin (degradation_.degradeBudgetPerFrame(),
            highDetailFreeActorCount_ - targetFull);
        for (int i = 0; i < simplifyCount && i < (int) oldestFirst.size(); ++i)
            if (oldestFirst[(size_t) i]->simplifyForPerformance())
            {
                --highDetailFreeActorCount_;
                ++degradedFreeActorCount_;
            }
    }
    else if (highDetailFreeActorCount_ < targetFull
             && highDetailFreeActorCount_ < visibleFreeActorCount_)
    {
        std::vector<ToyActor*> youngestFirst;
        youngestFirst.reserve ((size_t) (visibleFreeActorCount_ - highDetailFreeActorCount_));
        for (auto& actor : freeActors)
            if (actor != nullptr && actor->alive
                && actor->getBounds().intersects (visibleBounds)
                && ! actor->isHighDetailForPerformance())
                youngestFirst.push_back (actor.get());

        std::sort (youngestFirst.begin(), youngestFirst.end(), [] (const ToyActor* a, const ToyActor* b)
        {
            return a->age < b->age;
        });

        const int restoreCount = juce::jmin (degradation_.restoreBudgetPerFrame(),
            targetFull - highDetailFreeActorCount_);
        for (int i = 0; i < restoreCount && i < (int) youngestFirst.size(); ++i)
            if (youngestFirst[(size_t) i]->restoreDetailForPerformance())
            {
                ++highDetailFreeActorCount_;
                if (degradedFreeActorCount_ > 0)
                    --degradedFreeActorCount_;
            }
    }

    // 3) 分流：GL 批渲染（零 ToyOverlay 重绘） vs CPU ToyOverlay 回退
    if (glActive)
    {
        bool hasNonGlFreeActor = false;
        std::vector<y2ktoy::GlToySprite> sprites;
        if (! freeActors.empty())
        {
            sprites.reserve (freeActors.size());
            y2ktoy::GlToySprite sp;
            for (const auto& actor : freeActors)
            {
                if (actor == nullptr || ! actor->alive)
                    continue;
                if (! actor->extractGlSprite (sp, director))
                {
                    hasNonGlFreeActor = true;
                    break;
                }
                if (sp.alpha > 0.002f)
                    sprites.push_back (sp);
            }
        }

        if (! hasNonGlFreeActor)
        {
            if (! sprites.empty())
            {
                auto snap = std::make_shared<y2ktoy::GlToySnapshot>();
                snap->version        = ++glSnapshotVersion_;
                snap->editorLogicalW = director.editor.getWidth();
                snap->editorLogicalH = director.editor.getHeight();
                snap->atlasGen       = director.atlas != nullptr ? director.atlas->glAtlasGeneration() : 0u;
                snap->sprites        = std::move (sprites);

                director.collectExclusionRects (exclusionScratch_);
                int exCount = 0;
                for (const auto& r : exclusionScratch_)
                {
                    if (r.isEmpty() || exCount >= y2ktoy::kMaxExclusionRects)
                        continue;
                    snap->exclusions[(size_t) exCount++] = {
                        (float) r.getX(), (float) r.getY(),
                        (float) r.getRight(), (float) r.getBottom()
                    };
                }
                snap->exclusionCount = exCount;

                director.publishGlToySnapshot (std::move (snap), true, false);
                director.requestGlToyFrame();
                hadGlSpritesLastFrame_ = true;
                lastActivityTime = frame.timeSec;
            }
            else if (hadGlSpritesLastFrame_)
            {
                director.publishGlToySnapshot (nullptr, false, true);
                director.requestGlToyFrame();
                hadGlSpritesLastFrame_ = false;
            }

            director.hideOverlayIfUnused();
        }
        else
        {
            // 含非 GL 自由 actor（如 promoteToFree）：本帧回退 CPU ToyOverlay，清空 GL 快照防重影
            if (hadGlSpritesLastFrame_)
            {
                director.publishGlToySnapshot (nullptr, false, true);
                director.requestGlToyFrame();
                hadGlSpritesLastFrame_ = false;
            }
            anyCpuOverlay = true;
            director.ensureOverlay();
            if (director.overlay && director.overlay->isVisible())
                director.overlay->repaint();
        }
    }
    else
    {
        if (hadGlSpritesLastFrame_)
        {
            director.publishGlToySnapshot (nullptr, false, false);
            hadGlSpritesLastFrame_ = false;
        }
        if (! freeActors.empty())
            anyCpuOverlay = true;

        if (director.overlay && director.overlay->isVisible() && ! cpuDirtyRl.isEmpty())
        {
            constexpr int kTile = 128;
            const auto bounds = cpuDirtyRl.getBounds();
            const int cols = (bounds.getWidth()  + kTile - 1) / kTile;
            const int rows = (bounds.getHeight() + kTile - 1) / kTile;

            if (cols * rows > 80)
            {
                director.overlay->repaint (bounds);
            }
            else
            {
                for (int ty = bounds.getY(); ty < bounds.getBottom(); ty += kTile)
                {
                    for (int tx = bounds.getX(); tx < bounds.getRight(); tx += kTile)
                    {
                        const juce::Rectangle<int> tileRect (tx, ty, kTile, kTile);
                        if (cpuDirtyRl.intersectsRectangle (tileRect))
                            director.overlay->repaint (tileRect);
                    }
                }
            }
        }
    }

    // 4) 模块内 actor（仅重绘该面板的紧凑脏区，绝不挂载全窗 ToyOverlay）
    for (auto it = panels.begin(); it != panels.end();)
    {
        auto& sp = *it;
        if (sp == nullptr) { it = panels.erase (it); continue; }

        ModulePanel* p = sp;
        if (p->toyState == nullptr || p->toyState->actors.empty())
        {
            if (p->toyState)
                p->toyState.reset();
            it = panels.erase (it);
            continue;
        }

        const auto dirty = p->toyState->step (dt);
        if (! dirty.isEmpty())
            p->repaint (dirty);
        ++it;
    }

    if (anyCpuOverlay)
    {
        lastActivityTime = frame.timeSec;
        director.ensureOverlay();
    }
    else if (! glActive && director.overlay && director.overlay->isVisible())
    {
        if (frame.timeSec - lastActivityTime > 3.0)
            director.hideOverlayIfUnused();
    }

    // ---- 性能日志：每秒一行（帧率 / actor 数 / 单帧处理耗时）----
    lastFrameMs_ = juce::Time::getMillisecondCounterHiRes() - t0;
    frameTotalMs_ += lastFrameMs_;
    frameMaxMs_ = juce::jmax (frameMaxMs_, lastFrameMs_);
    ++perfFrames_;
   #if defined (Y2K_ENABLE_LOGGING)
    // Release 下整段不编译：连每秒一次的统计与字符串拼接都不做
    if (frame.timeSec - perfLastLogTime_ >= 1.0)
    {
        const double span = frame.timeSec - perfLastLogTime_;
        if (span > 0.0)
        {
            int moduleActors = 0;
            for (auto& p : panels)
                if (p != nullptr && p->toyState)
                    moduleActors += (int) p->toyState->actors.size();
            TOYPERF_LOG (juce::String ("frame: fps=")
                          + juce::String ((double) perfFrames_ / span, 1)
                          + " freeActors=" + juce::String ((int) freeActors.size())
                          + " visibleFreeActors=" + juce::String (visibleFreeActorCount_)
                          + " fullEmoji=" + juce::String (highDetailFreeActorCount_)
                          + " degraded=" + juce::String (degradedFreeActorCount_)
                          + " emojiTarget=" + juce::String (degradation_.targetFullEmojiCount())
                          + " smoothedFps=" + juce::String (degradation_.smoothedFps(), 1)
                          + " glToy=" + juce::String (glActive ? 1 : 0)
                          + " moduleActors=" + juce::String (moduleActors)
                          + " frameMsAvg=" + juce::String (frameTotalMs_ / (double) perfFrames_, 2)
                          + " frameMsMax=" + juce::String (frameMaxMs_, 2)
                          + " overlayPaints=" + juce::String (overlayPaintCalls_)
                          + " overlayActors=" + juce::String (overlayActorsPainted_)
                          + " overlayPaintMs=" + juce::String (overlayPaintTotalMs_, 2));
        }
        perfFrames_ = 0;
        frameTotalMs_ = 0.0;
        frameMaxMs_ = 0.0;
        overlayPaintCalls_ = 0;
        overlayActorsPainted_ = 0;
        overlayPaintTotalMs_ = 0.0;
        perfLastLogTime_ = frame.timeSec;
    }
   #endif
}

// ==========================================================
// ToyOverlay
// ==========================================================
ToyOverlay::ToyOverlay (ToyClock& c) : clock (c)
{
    setInterceptsMouseClicks (false, false);
    setWantsKeyboardFocus (false);
    setOpaque (false);
}

void ToyOverlay::paint (juce::Graphics& g)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const auto clip = g.getClipBounds();
    if (clock.director.atlas != nullptr)
        clock.director.atlas->setPhysicalScale (g.getInternalContext().getPhysicalPixelScaleFactor());

    // 设置抽屉的层级必须高于 emoji：叠加层画在所有模块之上（飞出模块的自由
    //   actor 走这里），抽屉却是模块子组件 —— 不排除就会被盖住。
    //   把当前打开的抽屉矩形从裁剪区里扣掉，emoji 经过时自动"钻到面板后面"。
    juce::RectangleList<int> drawerRects;
    const auto openDrawer = ModulePanel::getOpenInWindowDrawerBoundsIn (*this);
    if (! openDrawer.isEmpty() && openDrawer.intersects (clip))
        drawerRects.addWithoutMerging (openDrawer);

    for (auto& sp : clock.panels)
    {
        if (auto* p = sp.getComponent())
        {
            const auto drawer = p->getControlDrawerBounds();
            if (! drawer.isEmpty())
            {
                const auto tl = getLocalPoint (p, drawer.getTopLeft());
                const juce::Rectangle<int> inOverlay (tl.x, tl.y, drawer.getWidth(), drawer.getHeight());
                if (inOverlay.intersects (clip))
                    drawerRects.addWithoutMerging (inOverlay);
            }
        }
    }

    juce::Graphics::ScopedSaveState ss (g);
    // 注：本仓库的 JUCE 只有 excludeClipRegion(Rectangle<int>) 单矩形重载，
    //     没有 RectangleList 版本 —— 逐个扣除（调用是累积的）。
    for (int i = 0; i < drawerRects.getNumRectangles(); ++i)
        g.excludeClipRegion (drawerRects.getRectangle (i));

    for (auto& a : clock.freeActors)
        if (a->getBounds().getSmallestIntegerContainer().intersects (clip))
        {
            a->paint (g, clock.director);
            ++clock.overlayActorsPainted_;
        }
    ++clock.overlayPaintCalls_;
    clock.overlayPaintTotalMs_ += juce::Time::getMillisecondCounterHiRes() - t0;
}

// ==========================================================
// ToyDirector
// ==========================================================
ToyDirector::ToyDirector (juce::Component& editorComponent,
                          ModuleWorkspace& ws, AnalyserHub& h)
    : editor (editorComponent), workspace (ws), hub (h)
{
    // 构造期不写任何文件（性能日志 session 行延迟到消息循环稳定后的首帧）
    Y2K_LOG ("[toy] director: EmojiAtlas begin");
    atlas   = std::make_unique<EmojiAtlas>();
    Y2K_LOG ("[toy] director: EmojiAtlas done");

    clock   = std::make_unique<ToyClock> (*this);
    overlay = std::make_unique<ToyOverlay> (*clock);
    Y2K_LOG ("[toy] director: clock+overlay done");

    editor.addMouseListener (this, true);
    Y2K_LOG ("[toy] director: mouse listener attached");

    hub.addFrameListener (clock.get());
    Y2K_LOG ("[toy] director: frame listener attached");

    // 主题切换：清空图集主题版本（duotone/描边），由预烘焙 Timer 重新生成（E.3.2）
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        if (atlas)
            atlas->invalidateThemed();
    });
    // 3 s 无互动降级检查走消息线程 Timer（与鼠标事件同线程，无需加锁）。
    startTimer (100);
    Y2K_LOG ("[toy] director: ctor done");
}

ToyDirector::~ToyDirector()
{
    stopTimer();

    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    editor.removeMouseListener (this);
    hub.removeFrameListener (clock.get());

    if (overlay && overlay->getParentComponent())
        overlay->getParentComponent()->removeChildComponent (overlay.get());
    clock->freeActors.clear();
    if (atlas)
        atlas->release();
}

void ToyDirector::setLevel (ToyLevel l)
{
    // Off 已下线：任何 Off / 越界入参一律钳制到 [Chill, Party]，
    // 保证用户永远无法把互动关掉（旧存档里的 0 也会被抬到 Chill）。
    const int li = juce::jlimit ((int) ToyLevel::Chill, (int) ToyLevel::Party, (int) l);
    const auto clamped = (ToyLevel) li;

    if (level_ == clamped)
        return;
    level_ = clamped;

    // 降回 Chill 时收掉 Party 级的持续效果，避免降级后仍在喷彩带。
    if (level_ == ToyLevel::Chill)
    {
        spraying_ = false;
        confettiRainRemaining_ = 0;
        nextConfettiRainTime_ = 0.0;
    }

    if (onLevelChanged)
        onLevelChanged (li);
}

void ToyDirector::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // v3.0.6：按住持续喷（C-2）期间不能走"3 s 无点击 → 收摊"逻辑。
    //   · 旧实现只看 lastComboTime_（只在 mouseDown 时刷新），按住不放超过 3 s
    //     就会被这里掐掉 spraying_ → "鼠标按住一小会之后就不喷了"（现场 bug）。
    //   · 先用实时按键状态兜底：在窗口外松开鼠标时 mouseUp 可能收不到，
    //     不能让 spraying_ 永久挂着（否则会一直喷、档位也永不降级）。
    if (spraying_ && ! juce::ModifierKeys::getCurrentModifiersRealtime().isLeftButtonDown())
    {
        spraying_ = false;
        hasLastMousePos_ = false;   // 力场起点作废，避免下次移动算出巨大速度
    }

    if (spraying_)
    {
        // 鼠标确实还按着 = 持续互动：续上组合窗口，且不降级。
        lastComboTime_ = now;
        return;
    }

    if (lastComboTime_ <= 0.0)
        return;

    if (now - lastComboTime_ < kInteractionWindowSec)
        return;

    // 3 s 没有互动：连击归零、停掉彩带雨，并降回 Chill。
    lastComboTime_ = 0.0;
    comboCount_ = 0;
    confettiRainRemaining_ = 0;
    setLevel (ToyLevel::Chill);
}

void ToyDirector::attachToFloating (ModulePanel& p)
{
    p.addMouseListener (this, true);
}

void ToyDirector::detachFromFloating (ModulePanel& p)
{
    p.removeMouseListener (this);
}

void ToyDirector::ensureOverlay()
{
    if (! overlay) return;
    if (overlay->getParentComponent() == nullptr)
    {
        overlay->setBounds (editor.getLocalBounds());
        editor.addChildComponent (overlay.get());
    }
    overlay->setVisible (true);
}

void ToyDirector::hideOverlayIfUnused()
{
    if (overlay && overlay->isVisible())
    {
        overlay->setVisible (false);
        if (auto* parent = overlay->getParentComponent())
            parent->removeChildComponent (overlay.get());
    }
}

void ToyDirector::setGlToyHostActive (bool active) noexcept
{
    const bool prev = glToyHostActive_.exchange (active, std::memory_order_acq_rel);
    if (prev && ! active)
    {
        glToyHasSprites_.store (false, std::memory_order_release);
        glToyClearPending_.store (false, std::memory_order_release);
        const juce::SpinLock::ScopedLockType sl (glSnapshotLock_);
        glSnapshot_.reset();
    }
}

void ToyDirector::setExpectedFps (double targetFps) noexcept
{
    if (clock)
        clock->setExpectedFps (targetFps);
}

bool ToyDirector::hasActiveGlToyFrame() const noexcept
{
    return glToyHostActive_.load (std::memory_order_acquire)
        && (glToyHasSprites_.load (std::memory_order_acquire)
            || glToyClearPending_.load (std::memory_order_acquire));
}

std::shared_ptr<const y2ktoy::GlToySnapshot> ToyDirector::getGlToySnapshot() const noexcept
{
    const juce::SpinLock::ScopedLockType sl (glSnapshotLock_);
    return glSnapshot_;
}

void ToyDirector::publishGlToySnapshot (std::shared_ptr<y2ktoy::GlToySnapshot> snap,
                                        bool hasSprites,
                                        bool needsClearFrame)
{
    {
        const juce::SpinLock::ScopedLockType sl (glSnapshotLock_);
        glSnapshot_ = std::move (snap);
    }
    glToyHasSprites_.store (hasSprites, std::memory_order_release);
    if (needsClearFrame)
        glToyClearPending_.store (true, std::memory_order_release);
    else if (! hasSprites)
        glToyClearPending_.store (false, std::memory_order_release);
}

void ToyDirector::consumeGlToyClearPending() noexcept
{
    glToyClearPending_.store (false, std::memory_order_release);
}

void ToyDirector::requestGlToyFrame()
{
    if (glFrameRequester_)
        glFrameRequester_();
}

void ToyDirector::collectExclusionRects (std::vector<juce::Rectangle<int>>& out) const
{
    out.clear();

    const auto openDrawer = ModulePanel::getOpenInWindowDrawerBoundsIn (editor);
    if (! openDrawer.isEmpty())
        out.push_back (openDrawer);

    if (clock)
    {
        for (const auto& sp : clock->panels)
        {
            if (auto* p = sp.getComponent())
            {
                const auto drawer = p->getControlDrawerBounds();
                if (! drawer.isEmpty())
                {
                    const auto tl = editor.getLocalPoint (p, drawer.getTopLeft());
                    out.emplace_back (tl.x, tl.y, drawer.getWidth(), drawer.getHeight());
                }
            }
        }
    }

    workspace.collectToyExclusionRects (editor, out);

    if (externalExclusionsProvider_)
        externalExclusionsProvider_ (out);

    // Deduplicate identical rects (e.g. if external provider also forwards workspace exclusions)
    size_t writeIdx = 0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i].isEmpty())
            continue;
        bool dup = false;
        for (size_t j = 0; j < writeIdx; ++j)
        {
            if (out[j] == out[i])
            {
                dup = true;
                break;
            }
        }
        if (! dup)
            out[writeIdx++] = out[i];
    }
    out.resize (writeIdx);
}

bool ToyDirector::gateAllows (const juce::MouseEvent& e) const
{
    if (level_ == ToyLevel::Off)
        return false;
    if (! e.mods.isLeftButtonDown())
        return false;
    if (juce::Component::getCurrentlyModalComponent() != nullptr)
        return false;
    if (auto* peer = editor.getPeer())
        if (! peer->isFocused())
            return false;
    return true;
}

bool ToyDirector::isOptOut (juce::Component* c) const
{
    for (; c != nullptr; c = c->getParentComponent())
    {
        if (c->getProperties().getWithDefault ("toy.optOut", false))
            return true;
        if (c == &editor)
            break;
    }
    return false;
}

void ToyDirector::mouseDown (const juce::MouseEvent& e)
{
    if (! gateAllows (e))
        return;

    auto* orig = e.originalComponent;
    if (isOptOut (orig))
        return;

    // 控件自己吃事件：Button/Slider/ComboBox/TextEditor/ScrollBar 一律排除（§6.1）
    for (juce::Component* c = orig; c != nullptr; c = c->getParentComponent())
    {
        if (dynamic_cast<juce::Button*> (c)      || dynamic_cast<juce::Slider*> (c) ||
            dynamic_cast<juce::ComboBox*> (c)    || dynamic_cast<juce::TextEditor*> (c) ||
            dynamic_cast<juce::ScrollBar*> (c))
            return;
        if (c == &editor) break;
    }

    // 找到命中面板
    ModulePanel* panel = nullptr;
    for (juce::Component* c = orig; c != nullptr; c = c->getParentComponent())
    {
        if (auto* p = dynamic_cast<ModulePanel*> (c)) { panel = p; break; }
        if (c == &editor) break;
    }

    const auto editorPos = e.getEventRelativeTo (&editor).getPosition().toFloat();

    // E.5 统一 onClick：可点击 actor 优先消费（Clippo / Invader / TruePeak OK 等）
    if (panel && panel->toyState)
    {
        const auto panelPos = e.getEventRelativeTo (panel).getPosition().toFloat();
        for (auto& a : panel->toyState->actors)
        {
            if (a->alive && a->onClick (panelPos))
            {
                const auto local = a->getBounds().getCentre();
                spawnBurst (editor.getLocalPoint (panel, local.toInt()).toFloat());
                return;
            }
        }
    }
    for (auto& a : clock->freeActors)
    {
        if (a->alive && a->onClick (editorPos))
            return;
    }

    if (panel)
        routeModule (*panel, e);
    else
        routeGlobal (orig, e);
}

// 爆裂火花（点爆 / 击落共用，Editor 坐标）
void ToyDirector::spawnBurst (juce::Point<float> at)
{
    const int n = 4 + juce::Random::getSystemRandom().nextInt (3);
    for (int i = 0; i < n; ++i)
    {
        const float ang = (float) i / (float) n * 6.28318f
                        + juce::Random::getSystemRandom().nextFloat() * 0.5f;
        const float spd = (140.0f + 80.0f * juce::Random::getSystemRandom().nextFloat()) * uiScale();
        clock->freeActors.push_back (
            makeEmojiParticle (toyRoleEmoji (ToyRole::Burst), at,
                               { std::cos (ang) * spd, std::sin (ang) * spd },
                               0, *this, 0.9f, 500.0f * uiScale()));
    }
    if (isGlToyActive())
        requestGlToyFrame();
    else
        ensureOverlay();
}

void ToyDirector::routeModule (ModulePanel& panel, const juce::MouseEvent& e)
{
    const auto p = e.getEventRelativeTo (&panel).getPosition();
    if (! panel.toyAccepts (p))
        return;

    if (panel.toyState == nullptr)
        panel.toyState = std::make_unique<ModuleToyState> (panel, *this);
    clock->trackPanel (panel);

    ToyContext ctx;
    ctx.director = this;
    ctx.panel    = &panel;
    ctx.state    = panel.toyState.get();
    ctx.hit.pos  = p.toFloat();

    ToyHit hit;
    if (panel.toyHitTest (p, hit))
    {
        ctx.hit = hit;
        panel.toyPoke (hit, ctx);
    }
    else
    {
        spawnGenericFallback (panel, p.toFloat());
    }

    ++totalPokes_;
    registerComboClick (e.getEventRelativeTo (&editor).getPosition().toFloat());
}

void ToyDirector::routeGlobal (juce::Component*, const juce::MouseEvent& e)
{
    const auto editorPos = e.getEventRelativeTo (&editor).getPosition().toFloat();

    // C-4：先点爆判定（点中空中 emoji 则消费本次点击，不再喷发/路由）
    if (tryPopEmoji (editorPos))
    {
        registerComboClick (editorPos);
        return;
    }

    // 画布：按下立即喷 + 进入持续喷状态（FIX-2 / C-1 / C-2）
    if (workspace.isEmptyCanvasPoint (e.getEventRelativeTo (&workspace).getPosition()))
    {
        spawnCanvasFountain (editorPos, 0.0f);
        registerComboClick (editorPos);
        // totalPokes_ 已在 spawnCanvasFountain 内计数，不重复（FIX-4 / B-08）

        spraying_ = true;
        sprayPos_ = editorPos;
        sprayStartTime_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
        lastSprayTime_  = sprayStartTime_;
        hasLastMousePos_ = true;
        lastMousePos_ = editorPos;
    }
}

void ToyDirector::mouseDrag (const juce::MouseEvent& e)
{
    const auto editorPos = e.getEventRelativeTo (&editor).getPosition().toFloat();

    // C-2：喷口跟随鼠标（花洒）
    if (spraying_)
        sprayPos_ = editorPos;

    // C-3：鼠标力场（推开经过的 emoji）
    mouseForceField (editorPos, 0.016f);

    hasLastMousePos_ = true;
    lastMousePos_ = editorPos;
}

void ToyDirector::mouseUp (const juce::MouseEvent&)
{
    // C-2：松开即停
    spraying_ = false;
}

// C-2：按住持续喷（由 ToyClock::onFrame 每帧驱动）
void ToyDirector::sprayTick (double timeSec)
{
    if (! spraying_ || level_ == ToyLevel::Off)
        return;

    const double held = timeSec - sprayStartTime_;
    if (held < 0.35)   // 首次爆发已由 mouseDown 完成
        return;

    if (timeSec - lastSprayTime_ >= 0.07)
    {
        lastSprayTime_ = timeSec;
        // 按住越久初速越大（×1.0 → ×1.5）
        const float boost = juce::jmin (1.5f, 1.0f + (float) (held - 0.35f) * 0.25f);
        spawnSmallBurst (sprayPos_, boost);
    }
}

void ToyDirector::tickConfettiRain (double timeSec)
{
    if (confettiRainRemaining_ <= 0 || level_ == ToyLevel::Off)
        return;
    if (nextConfettiRainTime_ > 0.0 && timeSec < nextConfettiRainTime_)
        return;

    const auto canvas = workspace.getCanvasArea();
    if (canvas.isEmpty())
    {
        confettiRainRemaining_ = 0;
        return;
    }

    constexpr double spawnInterval = 0.075;
    const int x = canvas.getX() + juce::Random::getSystemRandom().nextInt (canvas.getWidth());
    const float top = (float) (workspace.getY() + canvas.getY());
    const auto rainBounds = canvas.translated (workspace.getX(), workspace.getY());
    const float scale = uiScale();
    const float vx = (float) juce::Random::getSystemRandom().nextInt (161) - 80.0f;
    const float vy = 35.0f + juce::Random::getSystemRandom().nextFloat() * 55.0f;
    clock->freeActors.push_back (makeConfettiRainParticle (
        { (float) (workspace.getX() + x), top - 24.0f },
        { vx * scale, vy * scale }, *this, level_ == ToyLevel::Party, rainBounds));

    --confettiRainRemaining_;
    nextConfettiRainTime_ = timeSec + spawnInterval;
    if (isGlToyActive())
        requestGlToyFrame();
    else
        ensureOverlay();
}

// C-3：鼠标力场 —— 只在存在自由 actor 时处理，冲量正比于鼠标速度
void ToyDirector::mouseForceField (juce::Point<float> editorPos, float dtSec)
{
    if (level_ == ToyLevel::Off || level_ == ToyLevel::Chill)
        return;
    if (clock->freeActors.empty() || ! hasLastMousePos_)
        return;

    const float radius = 48.0f * uiScale();
    const auto mouseVel = (editorPos - lastMousePos_) / juce::jmax (0.001f, dtSec);
    const float speed = juce::jmin (900.0f, mouseVel.getDistanceFromOrigin());

    for (auto& a : clock->freeActors)
    {
        if (! a->isPoppable() || ! a->alive)
            continue;
        const auto c = a->getBounds().getCentre();
        const float d = editorPos.getDistanceFrom (c);
        if (d <= radius)
        {
            const float strength = (1.0f - d / radius) * juce::jmin (260.0f, speed) * uiScale();
            a->impulse (editorPos, strength);
        }
    }
}

// C-4：点爆空中 emoji（命中半径取包围盒 0.3 倍），命中则消费本次点击
bool ToyDirector::tryPopEmoji (juce::Point<float> editorPos)
{
    if (level_ == ToyLevel::Off)
        return false;

    for (auto it = clock->freeActors.begin(); it != clock->freeActors.end(); ++it)
    {
        auto& a = *it;
        if (! a->isPoppable() || ! a->alive)
            continue;

        const auto b = a->getBounds();
        const float r = juce::jmax (10.0f, juce::jmin (b.getWidth(), b.getHeight()) * 0.3f);
        if (editorPos.getDistanceFrom (b.getCentre()) > r)
            continue;

        const auto centre = b.getCentre();

        // 邻近 emoji 冲量（先做：只读遍历 + impulse，不修改容器）
        for (auto& o : clock->freeActors)
            if (o.get() != a.get() && o->isPoppable())
                o->impulse (centre, 160.0f * uiScale());

        // 先删除命中粒子（此时迭代器 it 仍有效）
        clock->freeActors.erase (it);

        // 再生成爆裂火花。注意：必须在 erase 之后再 push_back ——
        //   若先 push 触发 vector 扩容，it 会悬空，erase 时崩溃
        //   （连续点击时点爆命中即触发，堆栈 std::exchange 即此 bug）。
        const int n = 4 + juce::Random::getSystemRandom().nextInt (3);
        for (int i = 0; i < n; ++i)
        {
            const float ang = (float) i / (float) n * 6.28318f
                            + juce::Random::getSystemRandom().nextFloat() * 0.5f;
            const float spd = (140.0f + 80.0f * juce::Random::getSystemRandom().nextFloat()) * uiScale();
            clock->freeActors.push_back (
                makeEmojiParticle (toyRoleEmoji (ToyRole::Burst), centre,
                                   { std::cos (ang) * spd, std::sin (ang) * spd },
                                   0, *this, 0.9f, 500.0f * uiScale()));
        }

        if (isGlToyActive())
            requestGlToyFrame();
        else
            ensureOverlay();
        return true;
    }
    return false;
}

float ToyDirector::audioEnergy() const
{
    const auto frame = hub.getLatestFrame();
    if (frame == nullptr)
        return 0.5f;

    if (frame->has (AnalyserHub::Kind::Loudness))
    {
        const float rms = (frame->loudness.rmsL + frame->loudness.rmsR) * 0.5f; // dBFS -144..0
        return juce::jlimit (0.0f, 1.0f, (rms + 60.0f) / 60.0f);
    }
    if (frame->has (AnalyserHub::Kind::Oscilloscope))
    {
        double sum = 0.0;
        for (float s : frame->oscL) sum += (double) s * s;
        const float rms = (float) std::sqrt (sum / (double) frame->oscL.size());
        return juce::jlimit (0.0f, 1.0f, rms * 8.0f);
    }
    return 0.5f;
}

void ToyDirector::registerComboClick (juce::Point<float> editorPos)
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // 连续互动判定：与上一次互动间隔 >= 3 s 即断连，连击归零并降回 Chill。
    if (lastComboTime_ > 0.0 && now - lastComboTime_ >= kInteractionWindowSec)
    {
        comboCount_ = 0;
        setLevel (ToyLevel::Chill);
    }

    lastComboTime_ = now;
    ++comboCount_;

    // 每满 5 次连续互动升一档：Chill → Normal → Party（Party 封顶）。
    // 升级放在连击字幕之前，这样第 5/10 击的文字对应升级后的档位效果。
    if (comboCount_ % kUpgradeEveryClicks == 0)
    {
        const int cur  = juce::jlimit ((int) ToyLevel::Chill, (int) ToyLevel::Party, (int) level_);
        const int next = juce::jmin ((int) ToyLevel::Party, cur + 1);
        if (next != cur)
            setLevel ((ToyLevel) next);
    }

    // 连击里程碑（20 / 40 次）：只保留宠物反应与彩带雨彩蛋。
    //   连击字幕 NICE!/COOL!/WOW!/Y2K MODE 已按用户反馈移除 —— 连续点击时
    //   弹出的短语干扰操作、观感也不好。
    if (comboCount_ == 20 || comboCount_ == 40)
        onComboMilestone (comboCount_, editorPos);
}

void ToyDirector::onComboMilestone (int count, juce::Point<float> pos)
{
    juce::ignoreUnused (pos);

    if (count >= 20)
        notifyPetReaction (31);   // yell
    if (count >= 40)
    {
        notifyPetReaction (8);    // angry
        // 分批从可见画布顶部喷出，避免同帧创建大量粒子和被底部 toolbar 截断。
        confettiRainRemaining_ = (level_ == ToyLevel::Party) ? 48 : 36;
        nextConfettiRainTime_ = 0.0;
        // 后续喷发走彩虹着色（通过 spawnCanvasFountain 的 Party 判断实现）
    }
    if (isGlToyActive())
        requestGlToyFrame();
    else
        ensureOverlay();
}

void ToyDirector::notifyPetReaction (int animId)
{
    for (int i = 0; i < workspace.getNumModules(); ++i)
    {
        auto* m = workspace.getModule (i);
        if (m && m->getModuleType() == ModuleType::virtuPet)
        {
            if (auto* pet = dynamic_cast<VirtuPetModule*> (m))
                pet->playToyReaction (animId);
        }
    }
}

// ==========================================================
// SparkBurstActor —— 通用兜底（§8.5）
// ==========================================================
SparkBurstActor::SparkBurstActor (juce::Point<float> c, int count, float spread)
    : center (c)
{
    ttl = 0.9f;
    juce::Random rng;
    for (int i = 0; i < count; ++i)
    {
        const float ang = rng.nextFloat() * juce::MathConstants<float>::twoPi;
        const float spd = (60.0f + rng.nextFloat() * 120.0f) * spread;
        sparks.push_back ({ c, { std::cos (ang) * spd, -std::abs (std::sin (ang)) * spd * 0.9f },
                           0.0f, 0.5f + rng.nextFloat() * 0.4f });
    }
}

void SparkBurstActor::step (float dt, ToyDirector& dir)
{
    age += dt;
    if (age >= ttl) { alive = false; return; }
    for (auto& s : sparks)
    {
        s.life += dt;
        s.v.y += 600.0f * dir.uiScale() * dt;
        s.p += s.v * dt;
    }
}

void SparkBurstActor::paint (juce::Graphics& g, ToyDirector& dir)
{
    // 扩散环（主题色）
    const float t = age / ttl;
    g.setColour (ToyPalette::accent().withAlpha (0.5f * (1.0f - t)));
    g.drawEllipse (center.x - 30.0f * t, center.y - 30.0f * t, 60.0f * t, 60.0f * t, 1.5f);

    // 小号 Burst 角色 emoji（E.4.4）
    const auto& img = dir.atlas->themedImage (toyRoleEmoji (ToyRole::Burst), EmojiAtlas::Size::S);
    for (auto& s : sparks)
    {
        const float f = 1.0f - s.life / s.maxLife;
        if (img.isValid())
            drawEmoji (g, img, s.p, 14.0f * dir.uiScale(), 0.0f, f);
        else
        {
            g.setColour (ToyPalette::accent().withAlpha (f));
            const float r = 2.0f + 2.0f * f;
            g.fillEllipse (s.p.x - r, s.p.y - r, r * 2.0f, r * 2.0f);
        }
    }
}

juce::Rectangle<float> SparkBurstActor::getBounds() const
{
    return juce::Rectangle<float> (center.x - 60, center.y - 60, 120, 120);
}
