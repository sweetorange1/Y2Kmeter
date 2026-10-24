#include "source/ui/toys/ToyKit.h"
#include "source/ui/modules/VirtuPetModule.h"
#include "source/Y2KLogging.h"

// ==========================================================
// ToyKit —— 监听器、路由、时钟、叠加层、连击、彩蛋（§7 / §8.4）
// ==========================================================

namespace
{
    // ---- 连击字幕 ----
    struct ComboTextActor : public ToyActor
    {
        juce::String text;
        juce::Point<float> pos;

        ComboTextActor (const juce::String& t, juce::Point<float> p)
            : text (t), pos (p) { ttl = 1.2f; }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            pos.y -= 40.0f * dt;
            if (age >= ttl) alive = false;
        }
        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float fade = 1.0f - age / ttl;
            g.setFont (PinkXP::getFont (15.0f, juce::Font::bold));
            g.setColour (PinkXP::ink.withAlpha (fade));
            g.drawText (text, juce::Rectangle<int> ((int) pos.x - 60, (int) pos.y - 14, 120, 28),
                        juce::Justification::centred, false);
        }
        juce::Rectangle<float> getBounds() const override
        {
            return juce::Rectangle<float> (pos.x - 60, pos.y - 16, 120, 32);
        }
    };

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
    for (auto& a : actors)
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
    if (state) state->actors.push_back (std::move (a));
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
    static bool perfSessionBegun = false;
    if (! perfSessionBegun)
    {
        perfSessionBegun = true;
        toyperf::beginSession();
    }

    if (director.getLevel() == ToyLevel::Off)
        return;

    const float dt = frame.dtSec;
    bool any = false;
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    // C-2：按住持续喷（花洒）
    director.sprayTick (frame.timeSec);

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
    //   用 nth_element（O(n)）按 age 升序分区：前 limit 个是最年轻的，删除其余。
    //   注意：上一版 sort 降序 + 删尾部实际是"删最新留最老"，与预期相反，已修正。
    constexpr int kFreeActorLimit = 400;
    if ((int) freeActors.size() > kFreeActorLimit)
    {
        std::nth_element (freeActors.begin(), freeActors.begin() + kFreeActorLimit, freeActors.end(),
                          [](const auto& a, const auto& b) { return a->age < b->age; });
        freeActors.erase (freeActors.begin() + kFreeActorLimit, freeActors.end());
    }

    // 自由 actor
    if (! freeActors.empty())
    {
        // 性能优化 P9v3：粒子数 > 40 时动画降频到一半帧率（每 2 帧 step 一次）。
        //   模块多的布局下，粒子 repaint 会触发底下模块重绘（放大效应），
        //   阈值前移让重绘频率尽早减半（日志实测 41 粒子 126→18fps 的元凶之一）。
        const bool heavy = freeActors.size() > 40;
        if (heavy)
        {
            skipCounter_ ^= 1;
            if (skipCounter_ == 1)
                goto perfLogOnly;   // 跳帧：本轮不更新粒子（保留日志打点）
        }

        // 性能优化 P10：tile 分块 repaint（128px 块，只重绘粒子实际覆盖的块）。
        //   union 单次大 dirty 在粒子散布全屏时每帧重绘大半窗口的底层组件
        //   （日志实测 60Hz 段 48~150 粒子仅 22~30fps），tile 化后重绘面积
        //   只与粒子总覆盖成正比。
        juce::RectangleList<int> rl;
        for (auto it = freeActors.begin(); it != freeActors.end();)
        {
            auto& a = *it;
            a->prevBounds = a->getBounds();
            a->hasPrevBounds = true;
            a->step (dt, director);

            const auto cur = a->getBounds().getSmallestIntegerContainer();
            if (! a->alive)
            {
                rl.add (cur);
                it = freeActors.erase (it);
                continue;
            }
            rl.add (cur);
            if (a->hasPrevBounds)
                rl.add (a->prevBounds.getSmallestIntegerContainer());
            ++it;
        }
        if (! freeActors.empty())
            any = true;

        if (director.overlay && director.overlay->isVisible() && ! rl.isEmpty())
        {
            constexpr int kTile = 128;
            const auto bounds = rl.getBounds();
            const int cols = (bounds.getWidth()  + kTile - 1) / kTile;
            const int rows = (bounds.getHeight() + kTile - 1) / kTile;

            // 性能优化 P13：粒子遍布全屏时 tile 数爆炸（2560×1366 → 220 块，
            //   数百次 repaint 又重蹈 P5 覆辙）→ 回退 union 单次 repaint
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
                        if (rl.intersectsRectangle (tileRect))
                            director.overlay->repaint (tileRect);
                    }
                }
            }
        }
    }

perfLogOnly:
    // 跳帧路径（P9）：粒子仍活跃，保持 overlay 存活，跳过自由粒子的 step/repaint
    if (! freeActors.empty())
        any = true;

    // 模块 actor
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
        any = true;
        ++it;
    }

    if (any)
    {
        lastActivityTime = frame.timeSec;
        director.ensureOverlay();
    }
    else if (director.overlay && director.overlay->isVisible())
    {
        if (frame.timeSec - lastActivityTime > 3.0)
        {
            director.overlay->setVisible (false);
            if (auto* parent = director.overlay->getParentComponent())
                parent->removeChildComponent (director.overlay.get());
        }
    }

    // ---- 性能日志：每秒一行（帧率 / actor 数 / 单帧处理耗时）----
    lastFrameMs_ = juce::Time::getMillisecondCounterHiRes() - t0;
    ++perfFrames_;
    if (frame.timeSec - perfLastLogTime_ >= 1.0)
    {
        const double span = frame.timeSec - perfLastLogTime_;
        if (span > 0.0)
        {
            int moduleActors = 0;
            for (auto& p : panels)
                if (p != nullptr && p->toyState)
                    moduleActors += (int) p->toyState->actors.size();
            toyperf::log (juce::String ("frame: fps=")
                          + juce::String ((double) perfFrames_ / span, 1)
                          + " freeActors=" + juce::String ((int) freeActors.size())
                          + " moduleActors=" + juce::String (moduleActors)
                          + " frameMs=" + juce::String (lastFrameMs_, 2));
        }
        perfFrames_ = 0;
        perfLastLogTime_ = frame.timeSec;
    }
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
    for (auto& a : clock.freeActors)
        a->paint (g, clock.director);
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
    Y2K_LOG ("[toy] director: ctor done");
}

ToyDirector::~ToyDirector()
{
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
    if (level_ == l)
        return;
    level_ = l;

    if (level_ == ToyLevel::Off)
    {
        clock->freeActors.clear();
        for (int i = 0; i < workspace.getNumModules(); ++i)
            if (auto* m = workspace.getModule (i))
                if (m->toyState) m->toyState->clear();
        comboCount_ = 0;
    }
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
    if (level_ == ToyLevel::Chill || level_ == ToyLevel::Off)
        return;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (now - lastComboTime_ > 1.2)
        comboCount_ = 0;
    lastComboTime_ = now;
    ++comboCount_;

    if (comboCount_ == 5 || comboCount_ == 10 || comboCount_ == 20 || comboCount_ == 40)
        onComboMilestone (comboCount_, editorPos);
}

void ToyDirector::onComboMilestone (int count, juce::Point<float> pos)
{
    juce::String text;
    switch (count)
    {
        case 5:  text = "NICE!"; break;
        case 10: text = "COOL!"; break;
        case 20: text = "WOW!";  break;
        case 40: text = "Y2K MODE"; break;
        default: return;
    }

    clock->freeActors.push_back (makeComboTextActor (text, pos));

    if (count >= 20)
        notifyPetReaction (31);   // yell
    if (count >= 40)
    {
        notifyPetReaction (8);    // angry
        // 🎉 彩带雨（反馈：更夸张，PARTY 档加倍）+ 彩虹着色
        const int w = editor.getWidth();
        const int n = (level_ == ToyLevel::Party) ? 120 : 60;
        for (int i = 0; i < n; ++i)
        {
            const int tier = juce::Random::getSystemRandom().nextInt (3);
            auto p = makeEmojiParticle (EmojiId::PartyPopper,
                                        { (float) (juce::Random::getSystemRandom().nextInt (w)), -20.0f },
                                        { (float) (juce::Random::getSystemRandom().nextInt (200) - 100) * uiScale(),
                                          120.0f * uiScale() }, tier, *this);
            clock->freeActors.push_back (std::move (p));
        }
        // 后续喷发走彩虹着色（通过 spawnCanvasFountain 的 Party 判断实现）
    }
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

// ==========================================================
// 工厂
// ==========================================================
std::unique_ptr<ToyActor> makeComboTextActor (const juce::String& text, juce::Point<float> at)
{
    return std::make_unique<ComboTextActor> (text, at);
}
