#include "source/ui/GlDirectModule.h"
#include "source/diag/Y2KDiagLog.h"

#include <cmath>
#include <cstring>
#include <mutex>

// ==========================================================
// 诊断状态注册表（v3.0.x）
//
//   卡死报告需要"所有 GL 直渲染模块当前处于什么状态"，而看门狗线程
//   绝对不能去碰组件树（isVisible()/getBounds() 都在 UI/GL 线程里被改）。
//   因此每个模块在构造时领一个全局槽位，把自己关心的状态以原子量写进槽位；
//   看门狗只读槽位。槽位本身是静态数组，永不释放，不会出现悬垂指针。
// ==========================================================
namespace
{
    constexpr int kMaxDiagModules = 48;

    struct ModuleDiagSlot
    {
        std::atomic<bool>     used      { false };
        std::atomic<int>      type      { 0 };
        std::atomic<int>      visible   { 0 };
        std::atomic<int>      floating  { 0 };
        std::atomic<int>      glActive  { 0 };
        std::atomic<unsigned> version   { 0 };
        std::atomic<int>      verts     { 0 };
        std::atomic<int>      segs      { 0 };
        std::atomic<int>      texW      { 0 };
        std::atomic<int>      texH      { 0 };
        std::atomic<int>      fboW      { 0 };
        std::atomic<int>      fboH      { 0 };
        std::atomic<int>      buildUs   { -1 };
        std::atomic<int>      renderUs  { -1 };
        std::atomic<int>      renderMaxUs { 0 };
        std::atomic<unsigned> renders   { 0 };
        std::atomic<int>      bx { 0 }, by { 0 }, bw { 0 }, bh { 0 };
    };

    ModuleDiagSlot gDiagSlots[kMaxDiagModules];

    // 只在 UI 线程的构造/析构里用
    std::mutex& slotMutex()
    {
        static std::mutex m;
        return m;
    }

    int claimDiagSlot() noexcept
    {
        const std::lock_guard<std::mutex> lock (slotMutex());
        for (int i = 0; i < kMaxDiagModules; ++i)
        {
            bool expected = false;
            if (gDiagSlots[i].used.compare_exchange_strong (expected, true))
                return i;
        }
        return -1;
    }

    void releaseDiagSlot (int slot) noexcept
    {
        if (slot < 0 || slot >= kMaxDiagModules)
            return;
        const std::lock_guard<std::mutex> lock (slotMutex());
        gDiagSlots[slot].used.store (false);
    }
}

// ==========================================================
// 构造 / 析构
// ==========================================================
namespace
{
    std::atomic<juce::uint64> gNextGlResourceKey { 1 };
}

GlDirectModuleBase::GlDirectModuleBase (ModuleType type)
    : ModulePanel (type),
      glResourceKey (gNextGlResourceKey.fetch_add (1, std::memory_order_relaxed))
{
    // 预生成阶段标记：热路径只填一个常驻指针，不做任何分配
    try
    {
        const auto typeName = moduleTypeToString (type).toStdString();
        diagWaitPhase   = "GlDirect.waitSnapshotLock(" + typeName + ")";
        diagRenderPhase = "GlDirect.render(" + typeName + ")";
    }
    catch (...)
    {
        diagWaitPhase   = "GlDirect.waitSnapshotLock(?)";
        diagRenderPhase = "GlDirect.render(?)";
    }

    diagSlot = claimDiagSlot();
    if (diagSlot >= 0)
        gDiagSlots[diagSlot].type.store ((int) type);

    lastGlAppearanceGen_ = PinkXP::getAppearanceGeneration();
    lastGlFontGen_       = PinkXP::getFontGeneration();
    glThemeSubToken_ = PinkXP::subscribeThemeChanged ([this]()
    {
        glForceTrailClear.store (true, std::memory_order_release);
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            glRefreshAsync_.triggerAsyncUpdate();
        repaint();
    });

    y2k::diag::setModuleStateProvider (&GlDirectModuleBase::dumpDiagStates);

    Y2K_DIAG ("MODULE", "GL-direct module created: " + moduleTypeToString (type)
                             + " slot=" + juce::String (diagSlot));
}

GlDirectModuleBase::~GlDirectModuleBase()
{
    glRefreshAsync_.cancelPendingUpdate();
    if (glThemeSubToken_ >= 0)
    {
        PinkXP::unsubscribeThemeChanged (glThemeSubToken_);
        glThemeSubToken_ = -1;
    }

    Y2K_DIAG ("MODULE", "GL-direct module destroyed: slot=" + juce::String (diagSlot));
    releaseDiagSlot (diagSlot);
    diagSlot = -1;
}

void GlDirectModuleBase::syncGlAppearanceIfStale (bool requestRepaint)
{
    const auto appGen  = PinkXP::getAppearanceGeneration();
    const auto fontGen = PinkXP::getFontGeneration();
    if (appGen == lastGlAppearanceGen_ && fontGen == lastGlFontGen_)
        return;

    lastGlAppearanceGen_ = appGen;
    lastGlFontGen_       = fontGen;
    glForceTrailClear.store (true, std::memory_order_release);

    if (wantsGlSnapshot())
        buildGlSnapshot();

    if (requestRepaint)
        repaint();
}

// ==========================================================
// 诊断状态
// ==========================================================
void GlDirectModuleBase::diagSetVisible (bool visible) noexcept
{
    if (diagSlot < 0 || ! y2k::diag::isEnabled())
        return;

    auto& s = gDiagSlots[diagSlot];
    s.visible.store (visible ? 1 : 0);
    s.glActive.store (glDirectActive.load() ? 1 : 0);
    s.floating.store (isFloating() ? 1 : 0);

    const auto b = getBounds();
    s.bx.store (b.getX());
    s.by.store (b.getY());
    s.bw.store (b.getWidth());
    s.bh.store (b.getHeight());
}

void GlDirectModuleBase::diagOnRendered (double renderMs) noexcept
{
    if (! y2k::diag::isEnabled())
        return;

    const int us = (int) (juce::jmax (0.0, renderMs) * 1000.0 + 0.5);
    diagRenderUs.store (us);
    diagRenderCount.fetch_add (1, std::memory_order_relaxed);

    auto prevMax = diagRenderMaxUs.load (std::memory_order_relaxed);
    while (us > prevMax
           && ! diagRenderMaxUs.compare_exchange_weak (prevMax, us, std::memory_order_relaxed))
    {
    }

    if (diagSlot >= 0)
    {
        auto& s = gDiagSlots[diagSlot];
        s.renderUs.store (us);
        s.renders.fetch_add (1, std::memory_order_relaxed);
        prevMax = s.renderMaxUs.load (std::memory_order_relaxed);
        while (us > prevMax
               && ! s.renderMaxUs.compare_exchange_weak (prevMax, us, std::memory_order_relaxed))
        {
        }
    }
}

void GlDirectModuleBase::diagSetFloating (bool floating) noexcept
{
    if (diagSlot >= 0)
        gDiagSlots[diagSlot].floating.store (floating ? 1 : 0);
}

juce::String GlDirectModuleBase::dumpDiagStates()
{
    juce::String s;
    int count = 0;

    for (int i = 0; i < kMaxDiagModules; ++i)
    {
        auto& slot = gDiagSlots[i];
        if (! slot.used.load())
            continue;

        ++count;
        const auto name = moduleTypeToString ((ModuleType) slot.type.load());

        s << "    " << name.paddedRight (' ', 16)
          << (slot.visible.load() ? "vis" : "hid")
          << (slot.floating.load() ? "/float" : "")
          << (slot.glActive.load() ? "/GL" : "/sw")
          << " v=" << (int) slot.version.load()
          << " verts=" << slot.verts.load()
          << " segs=" << slot.segs.load()
          << " tex=" << slot.texW.load() << "x" << slot.texH.load()
          << " fbo=" << slot.fboW.load() << "x" << slot.fboH.load()
          << " build=" << juce::String (slot.buildUs.load() / 1000.0, 2) << "ms"
          << " render=" << juce::String (slot.renderUs.load() / 1000.0, 2) << "ms"
          << " max=" << juce::String (slot.renderMaxUs.load() / 1000.0, 2) << "ms"
          << " frames=" << (int) slot.renders.load()
          << " bounds=" << slot.bx.load() << "," << slot.by.load()
          << " " << slot.bw.load() << "x" << slot.bh.load()
          << "\n";
    }

    if (count == 0)
        s << "    (no GL-direct modules registered)\n";

    return s;
}

// ==========================================================
// 快照发布
// ==========================================================
std::shared_ptr<GlDirectModuleBase::VertexStream> GlDirectModuleBase::beginGlSnapshot()
{
    const bool diagOn = y2k::diag::isEnabled();
    if (diagOn)
        diagBuildStartMs = juce::Time::getMillisecondCounterHiRes();

    auto snap = std::make_shared<VertexStream>();
    snap->version = ++glSnapshotVersion;
    return snap;
}

void GlDirectModuleBase::publishGlSnapshot (std::shared_ptr<VertexStream> snap)
{
    if (y2k::diag::isEnabled() && snap != nullptr)
    {
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - diagBuildStartMs;
        const int us = (int) (juce::jmax (0.0, elapsedMs) * 1000.0 + 0.5);
        diagBuildUs.store (us);

        if (diagSlot >= 0)
        {
            auto& s = gDiagSlots[diagSlot];
            s.buildUs.store (us);
            s.version.store ((unsigned) snap->version);
            s.verts.store ((int) (snap->vertices.size() / 6));
            s.segs.store ((int) snap->segments.size());
            s.texW.store (snap->texW);
            s.texH.store (snap->texH);
        }

        // 单帧构建超过 100ms 已经足够让消息线程卡出可感知停顿
        if (elapsedMs > 100.0)
            Y2K_DIAG ("SLOW", "buildGlSnapshot " + moduleTypeToString (getModuleType())
                                  + " " + juce::String (elapsedMs, 1) + "ms verts="
                                  + juce::String ((int) (snap->vertices.size() / 6)));
    }

    const juce::SpinLock::ScopedLockType sl (glSnapshotLock);
    glSnapshot = std::move (snap);
}

std::shared_ptr<GlDirectModuleBase::VertexStream> GlDirectModuleBase::getGlSnapshot() const
{
    const bool diagOn = y2k::diag::isEnabled();
    const double t0 = diagOn ? juce::Time::getMillisecondCounterHiRes() : 0.0;

    if (diagOn)
        y2k::diag::setPhase (diagWaitPhase.c_str());

    std::shared_ptr<VertexStream> snap;
    {
        const juce::SpinLock::ScopedLockType sl (glSnapshotLock);
        snap = glSnapshot;
    }

    if (diagOn)
    {
        const double waitedMs = juce::Time::getMillisecondCounterHiRes() - t0;
        // UI 线程构建快照时持有此锁；GL 线程自旋等待超过 8ms 说明两条线程
        // 已经在互相拖累（继续恶化就是"画面卡死但UI还活着"的典型现场）。
        if (waitedMs > 8.0)
            Y2K_DIAG ("LOCK", "getGlSnapshot spin-wait " + juce::String (waitedMs, 1) + "ms ("
                                  + juce::String (diagWaitPhase) + ")");
    }

    return snap;
}

void GlDirectModuleBase::appendQuad (std::vector<float>& v, juce::Rectangle<float> r, juce::Colour c)
{
    const float x0 = r.getX(), y0 = r.getY(), x1 = r.getRight(), y1 = r.getBottom();
    const float rr = c.getFloatRed(), gg = c.getFloatGreen(),
                bb = c.getFloatBlue(), aa = c.getFloatAlpha();
    const float quad[36] = {
        x0, y0, rr, gg, bb, aa,  x1, y0, rr, gg, bb, aa,  x0, y1, rr, gg, bb, aa,
        x1, y0, rr, gg, bb, aa,  x1, y1, rr, gg, bb, aa,  x0, y1, rr, gg, bb, aa,
    };
    v.insert (v.end(), quad, quad + 36);
}

void GlDirectModuleBase::appendLineSegment (std::vector<float>& v,
                                            float x0, float y0, float x1, float y1,
                                            float halfWidth, juce::Colour c)
{
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt (dx * dx + dy * dy);
    if (len < 1.0e-6f)
        return;

    const float nx = -dy / len * halfWidth;
    const float ny =  dx / len * halfWidth;
    const float r  = c.getFloatRed(), g = c.getFloatGreen(),
                b  = c.getFloatBlue(), a = c.getFloatAlpha();
    const float quad[36] = {
        x0 + nx, y0 + ny, r, g, b, a,   x1 + nx, y1 + ny, r, g, b, a,   x0 - nx, y0 - ny, r, g, b, a,
        x1 + nx, y1 + ny, r, g, b, a,   x1 - nx, y1 - ny, r, g, b, a,   x0 - nx, y0 - ny, r, g, b, a,
    };
    v.insert (v.end(), quad, quad + 36);
}

void GlDirectModuleBase::appendDisc (std::vector<float>& v,
                                     float cx, float cy, float radius,
                                     juce::Colour c, int segments)
{
    const float r  = c.getFloatRed(), g = c.getFloatGreen(),
                b  = c.getFloatBlue(), a = c.getFloatAlpha();
    for (int s = 0; s < segments; ++s)
    {
        const float a0 = juce::MathConstants<float>::twoPi * (float) s       / (float) segments;
        const float a1 = juce::MathConstants<float>::twoPi * (float) (s + 1) / (float) segments;
        const float x1 = cx + radius * std::cos (a0), y1 = cy + radius * std::sin (a0);
        const float x2 = cx + radius * std::cos (a1), y2 = cy + radius * std::sin (a1);
        const float q[18] = {
            cx, cy, r, g, b, a,
            x1, y1, r, g, b, a,
            x2, y2, r, g, b, a,
        };
        v.insert (v.end(), q, q + 18);
    }
}

void GlDirectModuleBase::appendWellFrame (std::vector<float>& v,
                                          juce::Rectangle<int> canvas,
                                          juce::Colour fill)
{
    if (canvas.getWidth() <= 0 || canvas.getHeight() <= 0)
        return;

    const auto cr = canvas.toFloat();
    const auto& tokens = PinkXP::getSkinTokens();

    // 绿幕 / 透明背景模式：继承当前视觉风格仪表工艺，但画布本身不画凹槽边框
    //   · Green Screen：铺纯绿 0xFF00FF00 供抠像
    //   · Transparent ：不铺不透明底色，保持纯 alpha=0
    if (tokens.isChromaKey)
    {
        if (PinkXP::isGreenScreenBackdrop())
            appendQuad (v, cr, PinkXP::chromaKeyBackdropColour());
        return;
    }

    // 非重叠 1px 矩形描边（避免半透明边在四角重复混合）
    const auto appendBorder1px = [&] (juce::Rectangle<float> r, juce::Colour c, float inset)
    {
        const auto o = r.reduced (inset);
        if (o.getWidth() <= 0.0f || o.getHeight() <= 0.0f)
            return;
        appendQuad (v, { o.getX(), o.getY(), o.getWidth(), 1.0f }, c);
        if (o.getHeight() > 1.0f)
            appendQuad (v, { o.getX(), o.getBottom() - 1.0f, o.getWidth(), 1.0f }, c);
        if (o.getHeight() > 2.0f)
        {
            appendQuad (v, { o.getX(), o.getY() + 1.0f, 1.0f, o.getHeight() - 2.0f }, c);
            if (o.getWidth() > 1.0f)
                appendQuad (v, { o.getRight() - 1.0f, o.getY() + 1.0f, 1.0f, o.getHeight() - 2.0f }, c);
        }
    };

    switch (tokens.frameStyle)
    {
        case PinkXP::FrameStyle::chromaNone:
        {
            if (PinkXP::isGreenScreenBackdrop())
                appendQuad (v, cr, PinkXP::chromaKeyBackdropColour());
            return;
        }

        case PinkXP::FrameStyle::frostedGlass:
        {
            // Frosted Glass（与 CPU Craft::drawPanelWell 1:1 对齐）：
            //   半透明深色外框 + 1px 顶/左晶亮白边 + 柔和顶部 32% 玻璃穹顶反光带 + 底部微弱折射高光
            const bool  darkFill = fill.getPerceivedBrightness() < 0.5f;
            const auto  outerRim = fill.interpolatedWith (darkFill ? juce::Colours::black : PinkXP::dark,
                                                          darkFill ? 0.38f : 0.34f);
            const auto  topRim   = fill.interpolatedWith (juce::Colours::white, darkFill ? 0.28f : 0.62f);
            const auto  leftRim  = fill.interpolatedWith (juce::Colours::white, darkFill ? 0.16f : 0.40f);
            const auto  botRefl  = fill.interpolatedWith (juce::Colours::white, darkFill ? 0.14f : 0.34f);

            appendQuad (v, cr, fill);

            // 柔和顶部穹顶反光渐隐带（避免冲淡深色图表，又保持水晶质感）
            if (cr.getWidth() > 4.0f && cr.getHeight() > 10.0f)
            {
                const float domeH = juce::jmin (cr.getHeight() * 0.32f, 44.0f);
                const float bandH = domeH / 3.0f;
                const float alphas[3] = { darkFill ? 0.055f : 0.14f,
                                          darkFill ? 0.032f : 0.08f,
                                          darkFill ? 0.014f : 0.035f };
                for (int bi = 0; bi < 3; ++bi)
                {
                    appendQuad (v,
                                { cr.getX() + 1.0f, cr.getY() + 1.0f + bandH * (float) bi,
                                  cr.getWidth() - 2.0f, bandH },
                                juce::Colours::white.withAlpha (alphas[bi]));
                }
            }

            appendBorder1px (cr, outerRim, 0.0f);
            if (cr.getWidth() > 2.0f && cr.getHeight() > 3.0f)
            {
                appendQuad (v, { cr.getX() + 1.0f, cr.getY() + 1.0f, cr.getWidth() - 2.0f, 1.0f }, topRim);
                if (cr.getHeight() > 4.0f)
                    appendQuad (v, { cr.getX() + 1.0f, cr.getY() + 2.0f, 1.0f, cr.getHeight() - 4.0f }, leftRim);
                appendQuad (v, { cr.getX() + 1.0f, cr.getBottom() - 2.0f, cr.getWidth() - 2.0f, 1.0f }, botRefl);
            }
            return;
        }

        case PinkXP::FrameStyle::neonTube:
        {
            // Neon Cyber（双层霓虹灯管边框 + 4px 赛博角标，深/浅主题均锐利通透）：
            //   外沿色晕 + 1px 炙热白芯霓虹管 + 内侧微光溢出 + 四角 4px L 型霓虹刻度
            const auto  hotCore   = PinkXP::sel.interpolatedWith (juce::Colours::white, 0.42f);
            const auto  outerAura = fill.interpolatedWith (PinkXP::sel, 0.34f);
            const auto  tubeCore  = fill.interpolatedWith (hotCore, juce::jlimit (0.58f, 0.88f, tokens.borderStrength * 0.88f));

            appendQuad (v, cr, fill);
            appendBorder1px (cr, outerAura, 0.0f);
            if (cr.getWidth() > 2.0f && cr.getHeight() > 2.0f)
                appendBorder1px (cr, tubeCore, 1.0f);
            if (tokens.glowIntensity > 0.0f && cr.getWidth() > 6.0f && cr.getHeight() > 6.0f)
            {
                const float spillAmt = juce::jlimit (0.10f, 0.24f, tokens.glowIntensity * 0.22f);
                appendBorder1px (cr, fill.interpolatedWith (PinkXP::sel, spillAmt), 2.0f);
            }
            if (cr.getWidth() >= 14.0f && cr.getHeight() >= 14.0f)
            {
                constexpr float tick = 4.0f;
                const auto cornerHi = hotCore.withAlpha (0.90f);
                // 左上
                appendQuad (v, { cr.getX() + 1.0f, cr.getY() + 1.0f, tick, 1.0f }, cornerHi);
                appendQuad (v, { cr.getX() + 1.0f, cr.getY() + 2.0f, 1.0f, tick - 1.0f }, cornerHi);
                // 右上
                appendQuad (v, { cr.getRight() - 1.0f - tick, cr.getY() + 1.0f, tick, 1.0f }, cornerHi);
                appendQuad (v, { cr.getRight() - 2.0f, cr.getY() + 2.0f, 1.0f, tick - 1.0f }, cornerHi);
                // 左下
                appendQuad (v, { cr.getX() + 1.0f, cr.getBottom() - 2.0f, tick, 1.0f }, cornerHi);
                appendQuad (v, { cr.getX() + 1.0f, cr.getBottom() - 1.0f - tick, 1.0f, tick - 1.0f }, cornerHi);
                // 右下
                appendQuad (v, { cr.getRight() - 1.0f - tick, cr.getBottom() - 2.0f, tick, 1.0f }, cornerHi);
                appendQuad (v, { cr.getRight() - 2.0f, cr.getBottom() - 1.0f - tick, 1.0f, tick - 1.0f }, cornerHi);
            }
            return;
        }

        case PinkXP::FrameStyle::crtPhosphor:
        {
            // CRT Terminal：锐利 0px 方角荧光外框 + 微弱 1px 内层荧光细线
            const float outerAmt = juce::jlimit (0.55f, 0.88f, tokens.borderStrength * 0.95f);
            appendQuad (v, cr, fill);
            appendBorder1px (cr, fill.interpolatedWith (PinkXP::sel, outerAmt), 0.0f);
            if (cr.getWidth() > 4.0f && cr.getHeight() > 4.0f)
            {
                const float innerAmt = juce::jlimit (0.14f, 0.28f, tokens.borderStrength * 0.28f);
                appendBorder1px (cr, fill.interpolatedWith (PinkXP::sel, innerAmt), 1.0f);
            }
            return;
        }

        case PinkXP::FrameStyle::modernStudio:
        {
            // Modern Minimal：精致 1px 演播室细描边 + 顶部 1px 微内凹层次 + 底部微抬升边，
            //   与 CPU 侧 SkinCraft::drawModernSurface 在深/浅主题下严格同阶。
            const bool  darkFill     = fill.getPerceivedBrightness() < 0.5f;
            const float baseStrength = juce::jmax (0.14f, tokens.borderStrength);
            const float lineAmt      = juce::jlimit (0.12f, 0.24f, baseStrength * (darkFill ? 0.90f : 0.78f));
            const float lipAmt       = juce::jlimit (0.20f, 0.36f, baseStrength * (darkFill ? 1.45f : 1.35f));
            appendQuad (v, cr, fill);
            appendBorder1px (cr, PinkXP::skinHairlineOn (fill, lineAmt), 0.0f);
            if (cr.getWidth() > 2.0f && cr.getHeight() > 2.0f)
            {
                const auto topRecess = fill.interpolatedWith (darkFill ? juce::Colours::black : PinkXP::dark,
                                                              darkFill ? 0.14f : 0.045f);
                appendQuad (v, { cr.getX() + 1.0f, cr.getY() + 1.0f, cr.getWidth() - 2.0f, 1.0f }, topRecess);
                appendQuad (v, { cr.getX() + 1.0f, cr.getBottom() - 1.0f, cr.getWidth() - 2.0f, 1.0f },
                            PinkXP::skinHairlineOn (fill, lipAmt));
            }
            return;
        }

        case PinkXP::FrameStyle::machinedChrome:
        {
            // Millennium Chrome（与 CPU Craft::drawPanelWell 1:1 对齐）：
            //   1px 深色机加工外框 + 1px 顶/左亮铬倒角高光沿 + 2–3px 顶部内凹投影 + 1px 底部微弱铬反光边
            const bool  darkFill  = fill.getPerceivedBrightness() < 0.5f;
            const auto  chromeHi  = PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f);
            const auto  outerEdge = darkFill ? PinkXP::dark.interpolatedWith (juce::Colours::black, 0.35f)
                                             : PinkXP::dark;
            const auto  topRim    = fill.interpolatedWith (chromeHi, darkFill ? 0.24f : 0.44f);
            const auto  botRefl   = fill.interpolatedWith (chromeHi, darkFill ? 0.16f : 0.38f);
            const auto  shdwSink  = darkFill ? juce::Colours::black : PinkXP::dark;
            const float baseShdw  = (tokens.innerShadowAlpha > 0.0f) ? tokens.innerShadowAlpha : 0.22f;
            const float rowScales[3] = { 1.0f, 0.55f, 0.24f };

            appendQuad (v, cr, fill);

            // 2–3px 顶部内凹投影（由机加工铝框唇边投射入画布内部）
            for (int i = 0; i < 3 && (float) i < cr.getHeight() - 5.0f && cr.getWidth() > 4.0f; ++i)
            {
                const float shdwAmt = juce::jlimit (0.0f, darkFill ? 0.44f : 0.32f, baseShdw * rowScales[i]);
                appendQuad (v, { cr.getX() + 2.0f, cr.getY() + 2.0f + (float) i, cr.getWidth() - 4.0f, 1.0f },
                            fill.interpolatedWith (shdwSink, shdwAmt));
            }

            // 1px 顶/左亮铬高光倒角沿 + 1px 底部/右侧微弱反光沿
            if (cr.getWidth() > 2.0f && cr.getHeight() > 2.0f)
            {
                appendQuad (v, { cr.getX() + 1.0f, cr.getY() + 1.0f, cr.getWidth() - 2.0f, 1.0f }, topRim);
                if (cr.getHeight() > 3.0f)
                    appendQuad (v, { cr.getX() + 1.0f, cr.getY() + 2.0f, 1.0f, cr.getHeight() - 3.0f }, topRim);
                appendQuad (v, { cr.getX() + 1.0f, cr.getBottom() - 2.0f, cr.getWidth() - 2.0f, 1.0f }, botRefl);
                if (cr.getHeight() > 4.0f)
                    appendQuad (v, { cr.getRight() - 2.0f, cr.getY() + 2.0f, 1.0f, cr.getHeight() - 4.0f },
                                botRefl.withMultipliedAlpha (0.75f));
            }

            // 1px 锐利深色机加工外边
            appendBorder1px (cr, outerEdge, 0.0f);
            return;
        }

        case PinkXP::FrameStyle::pixelBevel:
        default:
        {
            appendQuad (v, cr, fill);
            appendQuad (v, { cr.getX(), cr.getY(), cr.getWidth(), 2.0f }, PinkXP::dark);
            appendQuad (v, { cr.getX(), cr.getY(), 2.0f, cr.getHeight() }, PinkXP::dark);
            appendQuad (v, { cr.getX(), cr.getBottom() - 2.0f, cr.getWidth(), 2.0f }, PinkXP::hl);
            appendQuad (v, { cr.getRight() - 2.0f, cr.getY(), 2.0f, cr.getHeight() }, PinkXP::hl);
            if (cr.getWidth() > 4.0f && cr.getHeight() > 4.0f)
            {
                appendQuad (v, { cr.getX() + 2.0f, cr.getY() + 2.0f, cr.getWidth() - 4.0f, 1.0f }, PinkXP::shdw);
                appendQuad (v, { cr.getX() + 2.0f, cr.getY() + 2.0f, 1.0f, cr.getHeight() - 4.0f }, PinkXP::shdw);
                appendQuad (v, { cr.getX() + 2.0f, cr.getBottom() - 3.0f, cr.getWidth() - 4.0f, 1.0f }, juce::Colours::white);
                appendQuad (v, { cr.getRight() - 3.0f, cr.getY() + 2.0f, 1.0f, cr.getHeight() - 4.0f }, juce::Colours::white);
            }
            return;
        }
    }
}

void GlDirectModuleBase::appendSunkenFrame (std::vector<float>& v, juce::Rectangle<int> canvas)
{
    appendWellFrame (v, canvas, PinkXP::content);
}

void GlDirectModuleBase::appendRoundedQuad (std::vector<float>& v,
                                            juce::Rectangle<float> rect,
                                            juce::Colour col,
                                            float radius,
                                            int cornerSegs)
{
    if (rect.getWidth() <= 0.0f || rect.getHeight() <= 0.0f || col.getFloatAlpha() <= 0.001f)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const float r = juce::jmin (radius, rect.getWidth() * 0.5f, rect.getHeight() * 0.5f);
    if (r <= 0.5f || tokens.pixelSnapped)
    {
        appendQuad (v, rect, col);
        return;
    }

    const int segs = juce::jlimit (2, 16, cornerSegs);
    const int ringCount = 4 * (segs + 1);
    float ringX[4 * 17];
    float ringY[4 * 17];

    const float x0 = rect.getX(), y0 = rect.getY();
    const float x1 = rect.getRight(), y1 = rect.getBottom();
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    const float halfPi = juce::MathConstants<float>::halfPi;

    const float cornerCx[4] = { x1 - r, x1 - r, x0 + r, x0 + r };
    const float cornerCy[4] = { y0 + r, y1 - r, y1 - r, y0 + r };
    const float startAng[4] = { -halfPi, 0.0f, halfPi, juce::MathConstants<float>::pi };

    int idx = 0;
    for (int corner = 0; corner < 4; ++corner)
    {
        for (int s = 0; s <= segs; ++s)
        {
            const float ang = startAng[corner] + halfPi * ((float) s / (float) segs);
            ringX[idx] = cornerCx[corner] + r * std::cos (ang);
            ringY[idx] = cornerCy[corner] + r * std::sin (ang);
            ++idx;
        }
    }

    const float rr = col.getFloatRed(), gg = col.getFloatGreen(),
                bb = col.getFloatBlue(), aa = col.getFloatAlpha();
    v.reserve (v.size() + (size_t) ringCount * 18);
    for (int i = 0; i < ringCount; ++i)
    {
        const int next = (i + 1) % ringCount;
        const float tri[18] = {
            cx, cy, rr, gg, bb, aa,
            ringX[i], ringY[i], rr, gg, bb, aa,
            ringX[next], ringY[next], rr, gg, bb, aa,
        };
        v.insert (v.end(), tri, tri + 18);
    }
}

void GlDirectModuleBase::appendStyledBarCell (std::vector<float>& v,
                                              juce::Rectangle<float> rect,
                                              juce::Colour col,
                                              bool lit,
                                              float pulse)
{
    if (rect.getWidth() <= 0.25f || rect.getHeight() <= 0.25f || col.getFloatAlpha() <= 0.001f)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const float p = juce::jlimit (0.0f, 1.0f, pulse);

    if (! lit)
    {
        if (tokens.isChromaKey)
            return;

        const auto dim = col.withMultipliedAlpha (tokens.unlitCellAlpha);
        if (dim.getFloatAlpha() <= 0.002f)
            return;

        if (tokens.cellFinish == PinkXP::CellFinish::metallicBevel)
        {
            // 千禧金属未亮槽位：精密直角内凹槽 + 微弱蚀刻底高光细线（兼顾深/浅色主题）
            const auto socket = dim.interpolatedWith (PinkXP::dark.withAlpha (dim.getFloatAlpha()), 0.22f);
            appendQuad (v, rect, socket);
            if (rect.getWidth() >= 2.0f && rect.getHeight() >= 3.0f)
            {
                if (rect.getHeight() >= 4.0f)
                    appendQuad (v, { rect.getX(), rect.getY(), rect.getWidth(), 1.0f },
                                PinkXP::dark.withAlpha (0.16f));
                appendQuad (v, { rect.getX(), rect.getBottom() - 1.0f, rect.getWidth(), 1.0f },
                            PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.12f));
            }
            return;
        }

        if (tokens.cellFinish == PinkXP::CellFinish::glossyCapsule)
        {
            const float capR = juce::jmin (juce::jmax (tokens.barCellRadius, 3.0f),
                                           juce::jmin (rect.getWidth(), rect.getHeight()) * 0.5f);
            appendRoundedQuad (v, rect, dim, capR, 4);
            if (rect.getWidth() >= 4.0f && rect.getHeight() >= 4.0f)
            {
                const float ins = juce::jmin (capR * 0.7f, rect.getWidth() * 0.25f);
                appendQuad (v, { rect.getX() + ins, rect.getY() + 0.5f, rect.getWidth() - ins * 2.0f, 1.0f },
                            juce::Colours::white.withAlpha (0.08f));
            }
            return;
        }

        if (tokens.cellFinish == PinkXP::CellFinish::phosphorBar)
        {
            appendQuad (v, rect, dim);
            return;
        }

        appendRoundedQuad (v, rect, dim, tokens.barCellRadius, 4);
        return;
    }

    const auto base = (p > 0.0f) ? col.interpolatedWith (juce::Colours::white, p * 0.30f) : col;
    // 仅绿幕模式抑制半透明外晕（防止绿幕抠像产生绿边）；
    //   透明模式与正常模式一样完整继承当前视觉风格的 CellFinish 工艺（含 Pixel XP 1px 高光与 Modern 圆角）。
    const bool suppressOuterHalo = PinkXP::isGreenScreenBackdrop();

    switch (tokens.cellFinish)
    {
        case PinkXP::CellFinish::flatPixel:
        {
            appendQuad (v, rect, base);
            if (tokens.cellHighlightAlpha > 0.0f && rect.getWidth() >= 2.5f && rect.getHeight() >= 2.0f)
            {
                const auto hi = base.interpolatedWith (juce::Colours::white,
                                                       juce::jlimit (0.0f, 0.85f, tokens.cellHighlightAlpha + p * 0.18f));
                appendQuad (v, { rect.getX(), rect.getY(), rect.getWidth(), 1.0f }, hi);
                if (rect.getHeight() >= 3.0f)
                    appendQuad (v, { rect.getX(), rect.getY() + 1.0f, 1.0f, rect.getHeight() - 1.0f }, hi);
            }
            if (rect.getWidth() >= 4.0f && rect.getHeight() >= 4.0f)
            {
                const auto lo = base.interpolatedWith (PinkXP::dark, 0.22f);
                appendQuad (v, { rect.getX() + 1.0f, rect.getBottom() - 1.0f, rect.getWidth() - 1.0f, 1.0f }, lo);
            }
            return;
        }

        case PinkXP::CellFinish::metallicBevel:
        {
            // 千禧金属点亮灯块：精密 0px 直角 + 三段金属宝石光泽
            //   （1px 顶面高光倒角 + 饱满核心/高柱体微光折痕 + 1px 底面压暗斜边）
            appendQuad (v, rect, base);
            if (rect.getHeight() >= 2.0f)
            {
                if (rect.getHeight() >= 5.0f)
                {
                    const float sheenH = juce::jmax (1.0f, rect.getHeight() * 0.24f);
                    const auto sheen = base.interpolatedWith (juce::Colours::white,
                                                              juce::jlimit (0.08f, 0.30f, 0.14f + p * 0.10f));
                    appendQuad (v, { rect.getX(), rect.getY() + 1.0f, rect.getWidth(), sheenH }, sheen);
                }

                const auto hi = base.interpolatedWith (juce::Colours::white,
                                                       juce::jlimit (0.28f, 0.72f, tokens.cellHighlightAlpha + p * 0.20f));
                appendQuad (v, { rect.getX(), rect.getY(), rect.getWidth(), 1.0f }, hi);
            }
            if (rect.getHeight() >= 3.0f)
            {
                const auto lo = base.interpolatedWith (PinkXP::dark, 0.30f);
                appendQuad (v, { rect.getX(), rect.getBottom() - 1.0f, rect.getWidth(), 1.0f }, lo);
            }
            return;
        }

        case PinkXP::CellFinish::cleanRounded:
        {
            // Modern Minimal：精致微圆角演播室纯色灯块 + 严格内缩于圆角之内的微弱顶部反光带
            appendRoundedQuad (v, rect, base, tokens.barCellRadius, 4);
            if (tokens.cellHighlightAlpha > 0.0f && rect.getWidth() >= 4.0f && rect.getHeight() >= 4.0f)
            {
                const float radInset = juce::jmin (rect.getWidth() * 0.32f,
                                                   juce::jmax (1.0f, tokens.barCellRadius));
                const float sheenW   = rect.getWidth() - radInset * 2.0f;
                if (sheenW >= 1.5f)
                {
                    const float sheenAmt = juce::jlimit (0.06f, 0.24f, tokens.cellHighlightAlpha * 0.85f + p * 0.14f);
                    const auto hi = base.interpolatedWith (juce::Colours::white, sheenAmt);
                    appendQuad (v, { rect.getX() + radInset, rect.getY() + 0.5f, sheenW, 1.0f }, hi);
                }
            }
            return;
        }

        case PinkXP::CellFinish::glossyCapsule:
        {
            // Frosted Glass 水晶胶囊灯块：3px 圆角 + 顶部晶体穹顶高光条 + 底部微弱通透折射反光
            const float rad = juce::jmin (juce::jmax (tokens.barCellRadius, 3.0f),
                                          juce::jmin (rect.getWidth(), rect.getHeight()) * 0.5f);
            appendRoundedQuad (v, rect, base, rad, 4);
            if (rect.getWidth() >= 3.0f && rect.getHeight() >= 3.0f)
            {
                const float inset  = juce::jmin (juce::jmax (1.0f, rad * 0.45f), rect.getWidth() * 0.25f);
                const float sheenH = juce::jmax (1.0f, rect.getHeight() * 0.38f);
                const float sheenA = juce::jlimit (0.20f, 0.52f, tokens.cellHighlightAlpha + p * 0.16f);
                const float innerW = rect.getWidth() - inset * 2.0f;
                if (innerW >= 1.5f)
                {
                    appendRoundedQuad (v,
                                       { rect.getX() + inset, rect.getY() + 0.5f, innerW, sheenH },
                                       juce::Colours::white.withAlpha (sheenA),
                                       juce::jmax (0.0f, rad - 1.0f), 3);
                    if (rect.getHeight() >= 4.0f)
                    {
                        const auto bounce = base.interpolatedWith (juce::Colours::white, 0.32f).withAlpha (0.30f);
                        appendQuad (v, { rect.getX() + inset, rect.getBottom() - 1.5f, innerW, 1.0f }, bounce);
                    }
                }
            }
            return;
        }

        case PinkXP::CellFinish::neonLit:
        {
            // Neon Cyber 霓虹灯块：1.5px 科技圆角 + 外围彩色辉光（绿幕模式严格关闭）+ 炙热中心灯丝
            const float rad = juce::jmin (1.5f, juce::jmin (rect.getWidth(), rect.getHeight()) * 0.40f);
            if (! suppressOuterHalo && tokens.glowIntensity > 0.0f)
            {
                const float pad   = 1.25f + p * 0.65f;
                const float haloA = juce::jlimit (0.12f, 0.34f, 0.22f * tokens.glowIntensity + p * 0.10f);
                appendRoundedQuad (v, rect.expanded (pad), base.withMultipliedAlpha (haloA),
                                   rad + pad * 0.5f, 3);
            }
            appendRoundedQuad (v, rect, base, rad, 3);
            if (rect.getWidth() >= 3.0f && rect.getHeight() >= 3.0f)
            {
                const auto hot = base.interpolatedWith (juce::Colours::white,
                                                        juce::jlimit (0.35f, 0.72f, tokens.cellHighlightAlpha * 0.85f + p * 0.22f));
                if (rect.getWidth() >= rect.getHeight())
                {
                    const float insX = juce::jmin (1.5f, rect.getWidth() * 0.18f);
                    const float filH = juce::jlimit (1.0f, 1.6f, rect.getHeight() * 0.28f);
                    appendQuad (v, { rect.getX() + insX, rect.getCentreY() - filH * 0.5f,
                                     rect.getWidth() - insX * 2.0f, filH }, hot);
                }
                else
                {
                    const float insY = juce::jmin (1.5f, rect.getHeight() * 0.18f);
                    const float filW = juce::jlimit (1.0f, 1.6f, rect.getWidth() * 0.28f);
                    appendQuad (v, { rect.getCentreX() - filW * 0.5f, rect.getY() + insY,
                                     filW, rect.getHeight() - insY * 2.0f }, hot);
                }
            }
            return;
        }

        case PinkXP::CellFinish::phosphorBar:
        {
            // CRT Terminal 荧光灯块：0px 锐利方角 + 轻柔荧光底晕（绿幕禁用）+ 明亮中心荧光灯丝
            if (! suppressOuterHalo && tokens.glowIntensity > 0.0f)
            {
                const float haloA = juce::jlimit (0.09f, 0.22f, 0.17f * tokens.glowIntensity + p * 0.07f);
                appendQuad (v, rect.expanded (0.85f, 0.45f), base.withMultipliedAlpha (haloA));
            }
            appendQuad (v, rect, base);
            if (rect.getWidth() >= 3.0f && rect.getHeight() >= 3.0f)
            {
                const auto core = base.interpolatedWith (juce::Colours::white,
                                                         juce::jlimit (0.24f, 0.56f, tokens.cellHighlightAlpha + p * 0.16f));
                if (rect.getWidth() >= rect.getHeight())
                    appendQuad (v, { rect.getX(), rect.getCentreY() - 0.5f, rect.getWidth(), 1.0f }, core);
                else
                    appendQuad (v, { rect.getCentreX() - 0.5f, rect.getY(), 1.0f, rect.getHeight() }, core);
            }
            return;
        }
    }
}

void GlDirectModuleBase::appendStyledPolyline (std::vector<float>& v,
                                               const juce::Point<float>* pts,
                                               int count,
                                               juce::Colour col,
                                               float width)
{
    if (pts == nullptr || count < 2 || col.getFloatAlpha() <= 0.001f)
        return;

    const auto& tokens = PinkXP::getSkinTokens();
    const float effWidth = juce::jmax (0.5f, width * tokens.strokeWidthScale);

    auto emitRibbon = [&] (float halfW, juce::Colour c)
    {
        if (halfW <= 0.0f || c.getFloatAlpha() <= 0.001f)
            return;

        // 保持平滑浮点坐标（禁止像素阶梯吸附），确保斜线/波形/圆弧在所有风格下柔滑无锯齿
        auto p0 = pts[0];
        int i1 = 1;
        juce::Point<float> p1;
        float segLen = 0.0f;
        while (i1 < count)
        {
            p1 = pts[i1];
            const float dx = p1.x - p0.x, dy = p1.y - p0.y;
            segLen = std::sqrt (dx * dx + dy * dy);
            if (segLen >= 1.0e-4f)
                break;
            ++i1;
        }
        if (i1 >= count)
            return;

        const float rr = c.getFloatRed(), gg = c.getFloatGreen(),
                    bb = c.getFloatBlue(), aa = c.getFloatAlpha();
        v.reserve (v.size() + (size_t) (count - 1) * 36);

        float prevNx = -(p1.y - p0.y) / segLen;
        float prevNy =  (p1.x - p0.x) / segLen;
        float startOx = prevNx * halfW;
        float startOy = prevNy * halfW;

        for (;;)
        {
            int i2 = i1 + 1;
            juce::Point<float> p2;
            float nextLen = 0.0f;
            while (i2 < count)
            {
                p2 = pts[i2];
                const float dx = p2.x - p1.x, dy = p2.y - p1.y;
                nextLen = std::sqrt (dx * dx + dy * dy);
                if (nextLen >= 1.0e-4f)
                    break;
                ++i2;
            }

            float endOx = prevNx * halfW;
            float endOy = prevNy * halfW;
            float nextStartOx = endOx;
            float nextStartOy = endOy;
            float nextNx = prevNx, nextNy = prevNy;

            if (i2 < count)
            {
                nextNx = -(p2.y - p1.y) / nextLen;
                nextNy =  (p2.x - p1.x) / nextLen;
                const float mx = prevNx + nextNx;
                const float my = prevNy + nextNy;
                const float mlen = std::sqrt (mx * mx + my * my);
                if (mlen > 1.0e-4f)
                {
                    const float ux = mx / mlen;
                    const float uy = my / mlen;
                    const float dot = ux * prevNx + uy * prevNy;
                    const float miterScale = halfW / juce::jmax (0.45f, dot);
                    endOx = ux * miterScale;
                    endOy = uy * miterScale;
                    nextStartOx = endOx;
                    nextStartOy = endOy;
                }
                else
                {
                    nextStartOx = nextNx * halfW;
                    nextStartOy = nextNy * halfW;
                }
            }

            const float x0a = p0.x + startOx, y0a = p0.y + startOy;
            const float x0b = p0.x - startOx, y0b = p0.y - startOy;
            const float x1a = p1.x + endOx,   y1a = p1.y + endOy;
            const float x1b = p1.x - endOx,   y1b = p1.y - endOy;

            const float quad[36] = {
                x0a, y0a, rr, gg, bb, aa,  x1a, y1a, rr, gg, bb, aa,  x0b, y0b, rr, gg, bb, aa,
                x1a, y1a, rr, gg, bb, aa,  x1b, y1b, rr, gg, bb, aa,  x0b, y0b, rr, gg, bb, aa,
            };
            v.insert (v.end(), quad, quad + 36);

            if (i2 >= count)
                break;

            p0 = p1;
            i1 = i2;
            p1 = p2;
            prevNx = nextNx;
            prevNy = nextNy;
            startOx = nextStartOx;
            startOy = nextStartOy;
        }
    };

    if (tokens.strokeGlowPass && ! PinkXP::isGreenScreenBackdrop() && tokens.strokeGlowAlpha > 0.0f)
    {
        const float glowMul = juce::jmax (1.25f, tokens.strokeGlowWidthMul);
        emitRibbon (effWidth * 0.5f * glowMul, col.withMultipliedAlpha (tokens.strokeGlowAlpha));
    }

    emitRibbon (effWidth * 0.5f, col);

    // Neon / CRT 电子束核心微亮灯丝：保持柔滑曲线无锯齿，仅在主线够宽时叠加克制的中心高光
    if (tokens.strokeGlowPass && effWidth >= 1.4f)
        emitRibbon (effWidth * 0.22f, col.interpolatedWith (juce::Colours::white, 0.28f).withMultipliedAlpha (0.42f));
}

void GlDirectModuleBase::appendGridLine (std::vector<float>& v,
                                         float x0, float y0,
                                         float x1, float y1,
                                         bool major,
                                         juce::Colour col)
{
    const auto& tokens = PinkXP::getSkinTokens();
    if (tokens.isChromaKey || tokens.gridStyle == PinkXP::GridStyle::none || col.getFloatAlpha() <= 0.001f)
        return;

    const bool isChrome = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const float alphaMul = isChrome ? (major ? 0.18f : 0.09f)
                                    : (major ? tokens.gridMajorAlpha : tokens.gridMinorAlpha);
    // 千禧金属刻度线：微冷激光蚀刻钢色调（0.18 主线 / 0.09 次线，1px 锐利细线，适配深/浅底色）
    const auto tintedCol = [&]() -> juce::Colour
    {
        if (! isChrome)
            return col;
        const auto steel = (col.getPerceivedBrightness() < 0.5f)
                               ? juce::Colour (0xFF3E4B5C)
                               : juce::Colour (0xFFC8D6E6);
        return col.withAlpha (1.0f).interpolatedWith (steel, 0.28f).withAlpha (col.getFloatAlpha());
    }();
    const auto c = tintedCol.withMultipliedAlpha (alphaMul);
    if (c.getFloatAlpha() <= 0.002f)
        return;

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt (dx * dx + dy * dy);
    if (len < 0.5f)
        return;

    const bool horiz = std::abs (dy) < 0.25f;
    const bool vert  = std::abs (dx) < 0.25f;

    auto emitDashed = [&] (float dashOn, float dashOff, bool pixelSnap)
    {
        const float step = dashOn + dashOff;
        if (horiz)
        {
            const float py = pixelSnap ? std::floor ((y0 + y1) * 0.5f) : ((y0 + y1) * 0.5f - 0.5f);
            const float sx = pixelSnap ? std::floor (juce::jmin (x0, x1)) : juce::jmin (x0, x1);
            const float ex = pixelSnap ? std::floor (juce::jmax (x0, x1)) : juce::jmax (x0, x1);
            for (float t = sx; t < ex; t += step)
            {
                const float w = juce::jmin (dashOn, ex - t);
                if (w > 0.1f)
                    appendQuad (v, { t, py, w, 1.0f }, c);
            }
            return;
        }
        if (vert)
        {
            const float px = pixelSnap ? std::floor ((x0 + x1) * 0.5f) : ((x0 + x1) * 0.5f - 0.5f);
            const float sy = pixelSnap ? std::floor (juce::jmin (y0, y1)) : juce::jmin (y0, y1);
            const float ey = pixelSnap ? std::floor (juce::jmax (y0, y1)) : juce::jmax (y0, y1);
            for (float t = sy; t < ey; t += step)
            {
                const float h = juce::jmin (dashOn, ey - t);
                if (h > 0.1f)
                    appendQuad (v, { px, t, 1.0f, h }, c);
            }
            return;
        }

        const float ux = dx / len;
        const float uy = dy / len;
        for (float t = 0.0f; t < len; t += step)
        {
            const float tEnd = juce::jmin (len, t + dashOn);
            if (pixelSnap && dashOn <= 1.25f)
            {
                const float mx = std::floor (x0 + ux * ((t + tEnd) * 0.5f));
                const float my = std::floor (y0 + uy * ((t + tEnd) * 0.5f));
                appendQuad (v, { mx, my, 1.0f, 1.0f }, c);
            }
            else
            {
                appendLineSegment (v, x0 + ux * t, y0 + uy * t, x0 + ux * tEnd, y0 + uy * tEnd, 0.5f, c);
            }
        }
    };

    switch (tokens.gridStyle)
    {
        case PinkXP::GridStyle::dottedPixel:
            emitDashed (major ? 2.0f : 1.0f, major ? 2.0f : 3.0f, true);
            return;

        case PinkXP::GridStyle::dashedTech:
            emitDashed (4.0f, 4.0f, false);
            return;

        case PinkXP::GridStyle::phosphorCrosshair:
            if (! major)
            {
                emitDashed (2.0f, 4.0f, true);
                return;
            }
            if (tokens.glowIntensity > 0.0f && ! PinkXP::isGreenScreenBackdrop())
                appendLineSegment (v, x0, y0, x1, y1, 1.1f, c.withMultipliedAlpha (0.20f));
            if (horiz)
            {
                const float py = std::floor ((y0 + y1) * 0.5f);
                const float sx = juce::jmin (x0, x1);
                const float ex = juce::jmax (x0, x1);
                appendQuad (v, { sx, py, ex - sx, 1.0f }, c);
                // 泰克示波器主轴微刻度齿（以中点对称分布，±2px 垂直短刻线）
                if (len >= 24.0f)
                {
                    const float step = juce::jlimit (10.0f, 18.0f, len / 18.0f);
                    const float mid  = (sx + ex) * 0.5f;
                    const auto  tickCol = c.withMultipliedAlpha (0.85f);
                    for (float d = step; mid - d > sx + 4.0f && mid + d < ex - 4.0f; d += step)
                    {
                        appendQuad (v, { std::floor (mid - d), py - 2.0f, 1.0f, 5.0f }, tickCol);
                        appendQuad (v, { std::floor (mid + d), py - 2.0f, 1.0f, 5.0f }, tickCol);
                    }
                }
            }
            else if (vert)
            {
                const float px = std::floor ((x0 + x1) * 0.5f);
                const float sy = juce::jmin (y0, y1);
                const float ey = juce::jmax (y0, y1);
                appendQuad (v, { px, sy, 1.0f, ey - sy }, c);
                if (len >= 24.0f)
                {
                    const float step = juce::jlimit (10.0f, 18.0f, len / 18.0f);
                    const float mid  = (sy + ey) * 0.5f;
                    const auto  tickCol = c.withMultipliedAlpha (0.85f);
                    for (float d = step; mid - d > sy + 4.0f && mid + d < ey - 4.0f; d += step)
                    {
                        appendQuad (v, { px - 2.0f, std::floor (mid - d), 5.0f, 1.0f }, tickCol);
                        appendQuad (v, { px - 2.0f, std::floor (mid + d), 5.0f, 1.0f }, tickCol);
                    }
                }
            }
            else
            {
                appendLineSegment (v, x0, y0, x1, y1, 0.5f, c);
            }
            return;

        case PinkXP::GridStyle::solidHairline:
            if (horiz)
                appendQuad (v, { juce::jmin (x0, x1), std::floor ((y0 + y1) * 0.5f), std::abs (dx), 1.0f }, c);
            else if (vert)
                appendQuad (v, { std::floor ((x0 + x1) * 0.5f), juce::jmin (y0, y1), 1.0f, std::abs (dy) }, c);
            else
                appendLineSegment (v, x0, y0, x1, y1, major ? 0.5f : 0.45f, c);
            return;

        case PinkXP::GridStyle::none:
        default:
            return;
    }
}

void GlDirectModuleBase::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    syncGlAppearanceIfStale (false);

    const auto canvas = getGlCanvasBounds (contentBounds);

    // 接管分支：画布内容全部由 Editor::renderOpenGL 绘制，组件缓存的 canvas
    //   区域保持透明透出 GL 内容（挖洞填充），只保留 overlay 文字/标记。
    //   透明背景模式未悬停时跳过领口不透明底色（保持 alpha=0）；
    //   绿幕背景模式未悬停时领口填纯绿 #00FF00。
    if (glDirectActive.load() && canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        const bool unrevealed = ! PinkXP::isChromaRevealed (*this);
        if (! (PinkXP::isTransparentBackdrop() && unrevealed))
        {
            const auto& tokens = PinkXP::getSkinTokens();
            const juce::Colour collarCol = (PinkXP::isGreenScreenBackdrop() && unrevealed)
                                               ? PinkXP::chromaKeyGreen()
                                               : (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel
                                                      ? PinkXP::btnFace
                                                      : PinkXP::face);
            g.setColour (collarCol);
            juce::Graphics::ScopedSaveState ss (g);
            g.excludeClipRegion (canvas);
            g.fillRect (contentBounds);
        }
        paintGlOverlay (g, canvas);
        return;
    }

    // 未接管（GL 不可用 / 回退态）：完整旧路径
    paintLegacyContent (g, contentBounds);
}
