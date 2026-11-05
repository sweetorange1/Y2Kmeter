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
GlDirectModuleBase::GlDirectModuleBase (ModuleType type) : ModulePanel (type)
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

    y2k::diag::setModuleStateProvider (&GlDirectModuleBase::dumpDiagStates);

    Y2K_DIAG ("MODULE", "GL-direct module created: " + moduleTypeToString (type)
                             + " slot=" + juce::String (diagSlot));
}

GlDirectModuleBase::~GlDirectModuleBase()
{
    Y2K_DIAG ("MODULE", "GL-direct module destroyed: slot=" + juce::String (diagSlot));
    releaseDiagSlot (diagSlot);
    diagSlot = -1;
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

void GlDirectModuleBase::appendSunkenFrame (std::vector<float>& v, juce::Rectangle<int> canvas)
{
    auto cr = canvas.toFloat();
    appendQuad (v, cr, PinkXP::content);
    appendQuad (v, { cr.getX(), cr.getY(), (float) cr.getWidth(), 2.0f }, PinkXP::dark);
    appendQuad (v, { cr.getX(), cr.getY(), 2.0f, (float) cr.getHeight() }, PinkXP::dark);
    appendQuad (v, { cr.getX(), cr.getBottom() - 2.0f, (float) cr.getWidth(), 2.0f }, PinkXP::hl);
    appendQuad (v, { cr.getRight() - 2.0f, cr.getY(), 2.0f, (float) cr.getHeight() }, PinkXP::hl);
    appendQuad (v, { cr.getX() + 2.0f, cr.getY() + 2.0f, (float) cr.getWidth() - 4.0f, 1.0f }, PinkXP::shdw);
    appendQuad (v, { cr.getX() + 2.0f, cr.getY() + 2.0f, 1.0f, (float) cr.getHeight() - 4.0f }, PinkXP::shdw);
    appendQuad (v, { cr.getX() + 2.0f, cr.getBottom() - 3.0f, (float) cr.getWidth() - 4.0f, 1.0f }, juce::Colours::white);
    appendQuad (v, { cr.getRight() - 3.0f, cr.getY() + 2.0f, 1.0f, (float) cr.getHeight() - 4.0f }, juce::Colours::white);
}

void GlDirectModuleBase::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    const auto canvas = getGlCanvasBounds (contentBounds);

    // 接管分支：画布内容全部由 Editor::renderOpenGL 绘制，组件缓存的 canvas
    //   区域保持透明透出 GL 内容（挖洞填充），只保留 overlay 文字/标记。
    if (glDirectActive.load() && canvas.getWidth() > 8 && canvas.getHeight() > 8)
    {
        g.setColour (PinkXP::btnFace);
        {
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
