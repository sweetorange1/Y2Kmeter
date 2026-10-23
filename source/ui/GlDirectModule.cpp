#include "source/ui/GlDirectModule.h"
#include <cmath>

// ==========================================================
// GlDirectModuleBase 实现
// ==========================================================

std::shared_ptr<GlDirectModuleBase::VertexStream> GlDirectModuleBase::beginGlSnapshot()
{
    auto snap = std::make_shared<VertexStream>();
    snap->version = ++glSnapshotVersion;
    return snap;
}

void GlDirectModuleBase::publishGlSnapshot (std::shared_ptr<VertexStream> snap)
{
    const juce::SpinLock::ScopedLockType sl (glSnapshotLock);
    glSnapshot = std::move (snap);
}

std::shared_ptr<GlDirectModuleBase::VertexStream> GlDirectModuleBase::getGlSnapshot() const
{
    const juce::SpinLock::ScopedLockType sl (glSnapshotLock);
    return glSnapshot;
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
