#include "source/ui/ModulePanel.h"
#include "source/ui/ModuleControlDrawer.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/perf/PerformanceCounterSystem.h"
#include "source/perf/FramePerfLog.h"
#include "source/diag/Y2KDiagLog.h"
#include <juce_opengl/juce_opengl.h>
#include <unordered_map>

// ============================================================
// GlSafeComponentImageCache —— y2k::ui::setGlSafeImageCache 的实现
//   （设计说明见 ModuleWorkspace.h 的声明处）
//   与 JUCE 自带 setBufferedToImage（StandardCachedComponentImage）的区别：
//     · 只在绘制目标是 OpenGL 合成时启用，其它目标直接 paintEntireComponent，
//       行为与未缓存完全一致；
//     · 缓存固定 SoftwareImageType：JUCE 的 GL 渲染器与软件渲染器共用同一套
//       EdgeTable 光栅化，画面一致；且不持有 GL 帧缓冲 —— 自带实现在 GL 下会用
//       OpenGLImageType，Editor 的 GL 上下文重建（换 peer 等）后析构会在新上下文
//       里误删同名对象；
//     · 设备像素缩放 / 尺寸变化时自动重建。
// ============================================================
namespace
{
    class GlSafeComponentImageCache final : public juce::CachedComponentImage
    {
    public:
        explicit GlSafeComponentImageCache (juce::Component& c) noexcept : owner (c) {}

        void paint (juce::Graphics& g) override
        {
            auto& ctx = g.getInternalContext();

            // 判断绘制目标本身是否 GL 合成（不能用"当前线程有无 GL 上下文"：
            //   GL 线程上的离屏软件绘制同样有 current context，见 PROJECT_OVERVIEW §6.5）
            const auto preferredType = ctx.getPreferredImageTypeForTemporaryImages();
            if (dynamic_cast<const juce::OpenGLImageType*> (preferredType.get()) == nullptr)
            {
                owner.paintEntireComponent (g, false);
                return;
            }

            const float scale = ctx.getPhysicalPixelScaleFactor();
            const auto compBounds = owner.getLocalBounds();

            if (compBounds.isEmpty() || ! (scale > 0.0f))
                return;

            // 位图按"设备像素"取整（向上）：图像像素与屏幕像素 1:1。绘制时用
            //   scale(1/scale) + 低质量采样 —— JUCE renderImage 判定为纯平移后四舍五入
            //   到整像素直接贴图（见 RenderingHelpers::renderImage）。Windows 125% /
            //   150% 等小数缩放下也不会因 w/iw 的取整误差退化成插值采样而发虚；
            //   多出的不足 1px 由父组件按子组件边界裁掉。
            const juce::Rectangle<int> imageBounds (0, 0,
                                                    (int) std::ceil ((float) compBounds.getWidth()  * scale - 0.001f),
                                                    (int) std::ceil ((float) compBounds.getHeight() * scale - 0.001f));

            if (imageBounds.isEmpty())
                return;

            // 外观代次（主题 / 皮肤 / 配色）变化 → 整张缓存作废：
            //   缓存只按"自己失效的区域"重画，主题变化时只有被 repaint 波及的区域会更新，
            //   典型症状是模块抬头停在旧主题色（内容区因为逐帧刷新反而是新的）。
            const auto appearance = PinkXP::getAppearanceGeneration();
            if (appearance != cachedAppearance)
            {
                cachedAppearance = appearance;
                validArea.clear();
            }

            if (image.isNull() || image.getBounds() != imageBounds || cachedScale != scale)
            {
                // 恒用清空的 ARGB：小数缩放下右 / 下边不足 1px 的余量保持透明，
                //   不会露出未初始化像素（RGB 位图不清空）
                image = juce::Image (juce::Image::ARGB,
                                     imageBounds.getWidth(), imageBounds.getHeight(),
                                     true, juce::SoftwareImageType());
                cachedScale = scale;
                validArea.clear();
            }

            if (! validArea.containsRectangle (compBounds))
            {
                juce::Graphics imG (image);
                auto& lg = imG.getInternalContext();
                lg.addTransform (juce::AffineTransform::scale (scale));

                for (auto& r : validArea)
                    lg.excludeClipRectangle (r);

                if (! owner.isOpaque())
                {
                    lg.setFill (juce::Colours::transparentBlack);
                    lg.fillRect (compBounds, true);
                    lg.setFill (juce::Colours::black);
                }

                owner.paintEntireComponent (imG, true);
                validArea = compBounds;
            }

            g.setColour (juce::Colours::black.withAlpha (owner.getAlpha()));
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);   // 父组件已 ScopedSaveState
            g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale), false);
        }

        bool invalidateAll() override                                 { validArea.clear(); return true; }
        bool invalidate (const juce::Rectangle<int>& area) override   { validArea.subtract (area); return true; }
        void releaseResources() override                              { image = {}; validArea.clear(); }

    private:
        juce::Component& owner;
        juce::Image image;
        juce::RectangleList<int> validArea;
        float cachedScale = 0.0f;
        juce::uint32 cachedAppearance = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlSafeComponentImageCache)
    };
}

void y2k::ui::setGlSafeImageCache (juce::Component& c, bool enabled)
{
    const bool has = dynamic_cast<GlSafeComponentImageCache*> (c.getCachedComponentImage()) != nullptr;

    if (enabled && ! has)
        c.setCachedComponentImage (new GlSafeComponentImageCache (c));
    else if (! enabled && has)
        c.setCachedComponentImage (nullptr);
}

namespace
{
    // 按 ModulePanel 实例管理背景模式（Normal / Green Screen / Transparent）与悬停显露订阅：
    //   · 独立存放于翻译单元内，不修改 ModuleWorkspace.h 类布局。
    //   · 透明模式未悬停时切换 setOpaque(false) 并跳过外壳/标题栏/按钮绘制，保持纯 alpha=0。
    struct PanelChromaState
    {
        int  themeSubToken         = -1;
        int  revealSubToken        = -1;
        bool forcedNonOpaque       = false;
        bool lastTransparentHidden = false;
    };

    std::unordered_map<const ModulePanel*, PanelChromaState>& panelChromaStates()
    {
        static std::unordered_map<const ModulePanel*, PanelChromaState> states;
        return states;
    }

    void repaintPanelAndParentIfNonOpaque (ModulePanel& panel)
    {
        if (auto* cache = panel.getCachedComponentImage())
            cache->invalidateAll();
        panel.repaint();
        if (! panel.isOpaque())
            if (auto* parent = panel.getParentComponent())
                parent->repaint (panel.getBoundsInParent());
    }

    void syncPanelBackdropState (ModulePanel& panel, bool forceRepaint)
    {
        auto& states = panelChromaStates();
        auto it = states.find (&panel);
        if (it == states.end())
            return;

        auto& st = it->second;
        const bool wantTransparent = PinkXP::isTransparentBackdrop() && ! panel.isBareMode();
        bool changed = false;

        if (wantTransparent)
        {
            if (panel.isOpaque())
            {
                st.forcedNonOpaque = true;
                panel.setOpaque (false);
                changed = true;
            }
        }
        else if (st.forcedNonOpaque)
        {
            st.forcedNonOpaque = false;
            if (panel.getModuleType() != ModuleType::virtuPet)
            {
                panel.setOpaque (true);
                changed = true;
            }
        }

        const bool hidden = wantTransparent && ! PinkXP::isChromaRevealed (panel);
        if (hidden != st.lastTransparentHidden)
        {
            st.lastTransparentHidden = hidden;
            changed = true;
        }

        if (changed || forceRepaint)
            repaintPanelAndParentIfNonOpaque (panel);
    }

    // 从任一 ModulePanel 查取父 workspace 的布局锁定态。
    //   · 未挂到 workspace 上时默认"未锁定"，不会误拦临时孤立面板。
    //   · dynamic_cast 开销可忽略（仅在 mouseMove/Down 上下文发生）。
    bool isPanelLayoutLocked (const juce::Component& panel) noexcept
    {
        if (auto* module = dynamic_cast<const ModulePanel*> (&panel))
        {
            if (module->isFloatingLayoutLocked())
                return true;
        }
        if (auto* ws = dynamic_cast<const ModuleWorkspace*> (panel.getParentComponent()))
            return ws->isLayoutLocked();
        return false;
    }

    // 像素风小齿轮：4 根交叉齿条 + 圆盘 + 中心孔，按按钮尺寸自适应
    void drawGearIcon (juce::Graphics& g, juce::Rectangle<float> r,
                       juce::Colour ink, juce::Colour hole)
    {
        const float cx = r.getCentreX(), cy = r.getCentreY();
        const float R  = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
        const float toothW = juce::jmax (1.6f, R * 0.42f);

        g.setColour (ink);
        for (int i = 0; i < 4; ++i)
        {
            juce::Path tooth;
            tooth.addRectangle (-toothW * 0.5f, -R, toothW, R * 2.0f);
            g.fillPath (tooth, juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f * (float) i)
                                   .translated (cx, cy));
        }
        g.fillEllipse (cx - R * 0.74f, cy - R * 0.74f, R * 1.48f, R * 1.48f);

        g.setColour (hole);
        const float h = R * 0.36f;
        g.fillEllipse (cx - h, cy - h, h * 2.0f, h * 2.0f);
    }
}

// ==========================================================
// 设置面板的"点空白处自动收起"（支持窗口内弹出与独立桌面悬浮两种宿主）
//   · openDrawerPanels：当前打开着设置面板的模块（策略：同时只允许一个）
//   · 窗口内弹出 → 监听挂在 topLevel 上；独立桌面窗口 → 挂 Desktop 全局监听；
//   · 抽屉打开时才挂、全关时摘掉，平时零开销。
// ==========================================================
namespace
{
    juce::Array<ModulePanel*>& openDrawerPanels()
    {
        static juce::Array<ModulePanel*> list;
        return list;
    }

    juce::Component::SafePointer<juce::Component>& drawerListenerHost()
    {
        static juce::Component::SafePointer<juce::Component> host;
        return host;
    }

    bool& drawerListenerOnDesktop()
    {
        static bool onDesktop = false;
        return onDesktop;
    }

    struct DrawerDismissListener : public juce::MouseListener
    {
        void mouseDown (const juce::MouseEvent& e) override
        {
            auto* src = (e.originalComponent != nullptr) ? e.originalComponent : e.eventComponent;
            const auto pos = (src != nullptr) ? e.getEventRelativeTo (src).getPosition() : e.getPosition();
            ModulePanel::closeOpenControlDrawers (src, pos);
        }
    };

    DrawerDismissListener& drawerDismissListener()
    {
        static DrawerDismissListener listener;
        return listener;
    }

    void detachDrawerDismissListener()
    {
        if (drawerListenerOnDesktop())
        {
            juce::Desktop::getInstance().removeGlobalMouseListener (&drawerDismissListener());
            drawerListenerOnDesktop() = false;
        }
        if (drawerListenerHost().getComponent() != nullptr)
        {
            drawerListenerHost()->removeMouseListener (&drawerDismissListener());
            drawerListenerHost() = nullptr;
        }
    }

    void attachDrawerDismissListener (juce::Component* owner, bool desktopFloating)
    {
        if (desktopFloating)
        {
            if (drawerListenerOnDesktop())
                return;
            detachDrawerDismissListener();
            juce::Desktop::getInstance().addGlobalMouseListener (&drawerDismissListener());
            drawerListenerOnDesktop() = true;
            return;
        }

        auto* top = (owner != nullptr) ? owner->getTopLevelComponent() : nullptr;
        if (top == nullptr)
            return;
        if (! drawerListenerOnDesktop() && drawerListenerHost().getComponent() == top)
            return;

        detachDrawerDismissListener();
        top->addMouseListener (&drawerDismissListener(), true);
        drawerListenerHost() = top;
    }
}

// ==========================================================
// getModuleDisplayName —— 模块类型 -> 默认标题
// ==========================================================
juce::String getModuleDisplayName(ModuleType t)
{
    switch (t)
    {
        case ModuleType::loudness:          return "Loudness";
        case ModuleType::oscilloscope:      return "Oscilloscope";
        case ModuleType::spectrum:          return "Spectrum";
        case ModuleType::phase:             return "Phase";
        case ModuleType::dynamics:          return "Dynamics";

        case ModuleType::lufsRealtime:      return "LUFS Real-time";
        case ModuleType::truePeak:          return "True Peak";
        case ModuleType::oscilloscopeWave:  return "Oscilloscope Wave";
        case ModuleType::phaseCorrelation:  return "Phase Correlation";
        case ModuleType::phaseBalance:      return "Phase Balance";
        case ModuleType::dynamicsMeters:    return "Dynamics Meters";
        case ModuleType::dynamicsDr:        return "Dynamics DR";
        case ModuleType::dynamicsCrest:     return "Dynamics Crest";
        case ModuleType::waveform:          return "Waveform History";
        case ModuleType::vuMeter:           return "VU Meter";
        case ModuleType::spectrogram:       return "Spectrogram";
        case ModuleType::spectrogram3d:    return "Spectrogram 3D";
        case ModuleType::stereoField:       return "Stereo Field";
        case ModuleType::stereoMap:         return "Stereo Map";
        case ModuleType::virtuPet:        return "VirtuPet";
        case ModuleType::milkdrop:         return "Milkdrop";
        case ModuleType::tempoKey:         return "Tempo / Key";
        case ModuleType::recorder:          return "Recorder";
        case ModuleType::systemMonitor:     return "System Monitor";
        case ModuleType::utility:           return "Tools";

        default:                            return "Module";

    }
}

// ==========================================================
// ModulePanel
// ==========================================================
ModulePanel::ModulePanel(ModuleType type)
    : moduleType(type),
      titleText(getModuleDisplayName(type))
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
    setWantsKeyboardFocus(false);
    // 让整个面板接收鼠标事件（包括内容区边缘），子组件也能正常接收
    setInterceptsMouseClicks(true, true);

    // 性能优化（阶段1）：
    //   面板的 paint() 在第一行就用 PinkXP::drawRaised 填满了整块 face 底色，
    //   即面板对外是完全不透明的。告知 JUCE setOpaque(true)，可以省掉父容器
    //   为我们这块区域再刷一层背景，减少一次全区 fillRect 开销。
    //   子组件（各模块内容）内部该怎么重绘不受影响。
    //
    //   【例外】某些模块（如 VirtuPetModule）在构造时会显式 setOpaque(false)
    //   覆盖这里的默认值，以获得"透背景叠加"的效果；那类模块必须用自己的
    //   repaintSelfAndParent 机制来避免拖影，属于 per-module 决策。
    setOpaque (true);

    // 绿幕 / 透明背景模式：每个模块是独立的悬停显示区域（见 PinkXP::isChromaRevealed）
    PinkXP::markChromaRegion (*this);

    {
        auto& st = panelChromaStates()[this];
        juce::Component::SafePointer<ModulePanel> safe (this);
        st.themeSubToken = PinkXP::subscribeThemeChanged ([safe]()
        {
            if (auto* self = safe.getComponent())
                syncPanelBackdropState (*self, true);
        });
        st.revealSubToken = PinkXP::subscribeChromaRevealChanged ([safe] (juce::Component* prev, juce::Component* next)
        {
            if (auto* self = safe.getComponent())
            {
                if (prev == self || next == self || (prev == nullptr && next == nullptr))
                    syncPanelBackdropState (*self, true);
            }
        });
        syncPanelBackdropState (*this, false);
    }

    // bug1：点击内部子组件也要置顶（见头文件 ChildClickToFront 说明）
    addMouseListener (&childClickToFront, /*wantsEventsForAllNestedChildComponents=*/ true);
}

ModulePanel::~ModulePanel()
{
    {
        auto& states = panelChromaStates();
        if (auto it = states.find (this); it != states.end())
        {
            if (it->second.themeSubToken >= 0)
                PinkXP::unsubscribeThemeChanged (it->second.themeSubToken);
            if (it->second.revealSubToken > 0)
                PinkXP::unsubscribeChromaRevealChanged (it->second.revealSubToken);
            states.erase (it);
        }
    }

    // 监听器是本对象的成员，先摘掉再析构，避免监听列表持有悬空指针
    removeMouseListener (&childClickToFront);

    // 析构前先关闭并把外部设置面板从工作区/桌面摘除，防止悬空引用
    setControlPanelOpen (false);
    if (controlDrawer_ != nullptr)
    {
        controlDrawer_->setDesktopFloating (false, nullptr);
        if (auto* p = controlDrawer_->getParentComponent())
            p->removeChildComponent (controlDrawer_.get());
    }

    openDrawerPanels().removeAllInstancesOf (this);
    if (openDrawerPanels().isEmpty())
        detachDrawerDismissListener();
}

void ModulePanel::ChildClickToFront::mouseDown (const juce::MouseEvent& e)
{
    auto* origin = e.originalComponent;

    // 落在面板自己身上的按下由 ModulePanel::mouseDown 处理，这里不重复执行
    if (origin == nullptr || origin == &panel || ! panel.isParentOf (origin))
        return;

    panel.bringToFrontFromChildClick();
}

void ModulePanel::bringToFrontFromChildClick()
{
    if (! isVisible())
        return;

    if (! isFloating_)
    {
        if (auto* ws = dynamic_cast<ModuleWorkspace*> (getParentComponent()))
        {
            if (ws->isModuleTopmostAmongModules (*this))
            {
                ws->clearOtherFocusVisuals (*this);
                return;
            }
        }
    }

    // 不抢键盘焦点：内部控件（便签 TextEditor / 计算器按钮）需要保留自己的焦点
    toFront (false);

    if (onBroughtToFront)
        onBroughtToFront (*this);
}

bool ModulePanel::isVisuallyActiveInWorkspace() const noexcept
{
    if (! isShowing() || getWidth() <= 0 || getHeight() <= 0)
        return false;

    if (const auto* p = getParentComponent())
    {
        const auto parentArea = p->getLocalBounds();
        const auto myAreaInParent = getBounds();
        if (myAreaInParent.getIntersection(parentArea).isEmpty())
            return false;
    }

    return true;
}

void ModulePanel::setTitleText(const juce::String& s)
{
    titleText = s;
    repaint(getTitleBarBounds());
}

juce::Rectangle<int> ModulePanel::getTitleBarBounds() const
{
    // 标题栏在外框 2px 内（高度随 UI 密度缩放，低分辨率下更紧凑）
    return juce::Rectangle<int>(2, 2, getWidth() - 4, PinkXP::ui(titleBarHeight));
}

juce::Rectangle<int> ModulePanel::getCloseButtonBounds() const
{
    auto tb = getTitleBarBounds();
    const int s = PinkXP::ui(closeButtonSize);
    const int y = tb.getY() + (tb.getHeight() - s) / 2;
    return juce::Rectangle<int>(tb.getRight() - 4 - s, y, s, s);
}

juce::Rectangle<int> ModulePanel::getPopOutButtonBounds() const
{
    auto cb = getCloseButtonBounds();
    constexpr int gap = 2;
    const int s = PinkXP::ui(popOutButtonSize);
    return juce::Rectangle<int>(cb.getX() - gap - s,
                                 cb.getY(), s, s);
}

// 控制面板（齿轮）按钮：紧挨在弹出按钮左侧；弹出按钮不显示时紧挨关闭按钮
juce::Rectangle<int> ModulePanel::getControlButtonBounds() const
{
    constexpr int gap = 2;
    const int s = PinkXP::ui(popOutButtonSize);
    const auto anchor = (popOutEnabled_ || isFloating_) ? getPopOutButtonBounds()
                                                        : getCloseButtonBounds();
    return juce::Rectangle<int>(anchor.getX() - gap - s, anchor.getY(), s, s);
}

// ----------------------------------------------------------
// 控制面板（v3.5）：外部智能弹出式设置面板（工作区外部 → 桌面悬浮 → 全屏角落兜底）
// ----------------------------------------------------------
void ModulePanel::setControlPanelEnabled (bool enabled) noexcept
{
    controlPanelEnabled_ = enabled;
}

bool ModulePanel::isComponentInControlDrawer (const juce::Component* c) const noexcept
{
    return controlDrawer_ != nullptr && c != nullptr
        && (c == controlDrawer_.get() || controlDrawer_->isParentOf (c));
}

juce::Rectangle<int> ModulePanel::getControlDrawerBounds() const
{
    if (controlDrawer_ == nullptr || ! controlPanelOpen_ || controlDrawer_->isDesktopFloating())
        return {};

    auto* p = controlDrawer_->getParentComponent();
    if (p == nullptr)
        return {};
    if (p == this)
        return controlDrawer_->getBounds().getIntersection (getLocalBounds());

    // 若宿主在工作区（位于模块外部），转回本模块本地坐标后取交集（正常外部展开时交集为空）
    const auto tl = getLocalPoint (p, controlDrawer_->getPosition());
    const juce::Rectangle<int> inLocal (tl.x, tl.y, controlDrawer_->getWidth(), controlDrawer_->getHeight());
    return inLocal.getIntersection (getLocalBounds());
}

juce::Rectangle<int> ModulePanel::getOpenInWindowDrawerBoundsIn (const juce::Component& target)
{
    for (auto* panel : openDrawerPanels())
    {
        if (panel == nullptr || ! panel->isControlPanelOpen() || panel->controlDrawer_ == nullptr)
            continue;
        auto* drawer = panel->controlDrawer_.get();
        if (drawer->isDesktopFloating() || ! drawer->isVisible())
            continue;
        if (drawer->getTopLevelComponent() != target.getTopLevelComponent())
            continue;
        const auto tl = target.getLocalPoint (drawer, juce::Point<int> (0, 0));
        return { tl.x, tl.y, drawer->getWidth(), drawer->getHeight() };
    }
    return {};
}

void ModulePanel::relayoutOpenControlDrawers()
{
    const juce::Array<ModulePanel*> snapshot (openDrawerPanels());
    for (auto* p : snapshot)
    {
        if (p != nullptr && p->isControlPanelOpen())
            p->layoutControlDrawer();
    }
}

ModulePanel::ControlDrawerPlacement ModulePanel::computeControlDrawerPlacement (
    bool isFloating,
    juce::Rectangle<int> ownerInWorkspace,
    juce::Rectangle<int> workspaceCanvas,
    const std::vector<juce::Rectangle<int>>& otherModulesInWorkspace,
    juce::Rectangle<int> ownerOnScreen,
    juce::Rectangle<int> screenWorkArea,
    juce::Rectangle<int> ownerContentLocal,
    int prefW, int minW, int maxW, int gap, int minH,
    const std::function<int(int)>& heightForWidth)
{
    minW  = juce::jmax (80, minW);
    prefW = juce::jmax (minW, prefW);
    maxW  = juce::jmax (prefW, maxW);
    minH  = juce::jmax (40, minH);

    struct Candidate
    {
        bool                 valid = false;
        juce::Rectangle<int> rect;
        float                fitRatio = 0.0f;
        float                score    = -1.0e9f;
    };

    auto evalExternal = [&] (juce::Rectangle<int> target,
                             juce::Rectangle<int> container,
                             const std::vector<juce::Rectangle<int>>& obstacles) -> Candidate
    {
        Candidate best;
        if (container.isEmpty() || target.isEmpty())
            return best;

        auto pickSideSize = [&] (int availW, int availH) -> std::pair<int, int>
        {
            int w = juce::jmin (prefW, availW);
            int wantH = juce::jmax (minH, heightForWidth (w));

            // 高度不足且侧边仍有水平余量时，适度拉宽到 maxW 以减少多选项换行次数
            if (wantH > availH && availW > w && maxW > w)
            {
                const int wideW = juce::jmin (maxW, availW);
                const int wideH = juce::jmax (minH, heightForWidth (wideW));
                if (wideH < wantH)
                {
                    const int midW = w + (wideW - w) / 2;
                    const int midH = juce::jmax (minH, heightForWidth (midW));
                    if (midH <= wideH || midH <= availH)
                    {
                        w = midW;
                        wantH = midH;
                    }
                    else
                    {
                        w = wideW;
                        wantH = wideH;
                    }
                }
            }
            return { w, wantH };
        };

        auto scoreCand = [&] (juce::Rectangle<int> r, int wantH, float sideBonus)
        {
            if (r.getWidth() < minW || r.getHeight() < minH)
                return;
            // 硬约束：绝不越出容器边界，且绝不与所属模块自身重叠
            if (! container.contains (r) || r.intersects (target))
                return;

            const float fit = juce::jlimit (0.0f, 1.0f, (float) r.getHeight() / (float) juce::jmax (1, wantH));
            const float wBonus = (r.getWidth() >= prefW)
                                     ? 0.50f
                                     : 0.50f * (float) r.getWidth() / (float) juce::jmax (1, prefW);

            const float area = (float) juce::jmax (1, r.getWidth() * r.getHeight());
            float overlapArea = 0.0f;
            for (const auto& ob : obstacles)
            {
                const auto inter = r.getIntersection (ob);
                if (! inter.isEmpty())
                    overlapArea += (float) (inter.getWidth() * inter.getHeight());
            }
            const float overlapRatio = juce::jlimit (0.0f, 1.0f, overlapArea / area);

            const float sc = fit * 2.2f + (1.0f - overlapRatio) * 1.4f + wBonus + sideBonus;
            if (! best.valid || sc > best.score)
            {
                best.valid    = true;
                best.rect     = r;
                best.fitRatio = fit;
                best.score    = sc;
            }
        };

        // 1) 右侧（紧挨齿轮按钮，首选）
        {
            const int x = target.getRight() + gap;
            const int availW = container.getRight() - x;
            const int availH = container.getHeight();
            if (x >= container.getX() && availW >= minW && availH >= minH)
            {
                const auto [w, wantH] = pickSideSize (availW, availH);
                const int h = juce::jmin (wantH, availH);
                const int y = juce::jlimit (container.getY(),
                                            juce::jmax (container.getY(), container.getBottom() - h),
                                            target.getY());
                scoreCand ({ x, y, w, h }, wantH, 0.10f);
            }
        }

        // 2) 左侧
        {
            const int availW = target.getX() - gap - container.getX();
            const int availH = container.getHeight();
            if (availW >= minW && availH >= minH)
            {
                const auto [w, wantH] = pickSideSize (availW, availH);
                const int h = juce::jmin (wantH, availH);
                const int x = target.getX() - gap - w;
                const int y = juce::jlimit (container.getY(),
                                            juce::jmax (container.getY(), container.getBottom() - h),
                                            target.getY());
                scoreCand ({ x, y, w, h }, wantH, 0.07f);
            }
        }

        // 3) 下方（右缘对齐齿轮按钮所在侧）
        {
            const int y = target.getBottom() + gap;
            const int availH = container.getBottom() - y;
            const int availW = container.getWidth();
            if (y >= container.getY() && availH >= minH && availW >= minW)
            {
                const int w = juce::jlimit (minW, juce::jmin (maxW, availW), juce::jmax (prefW, target.getWidth()));
                const int wantH = juce::jmax (minH, heightForWidth (w));
                const int h = juce::jmin (wantH, availH);
                const int x = juce::jlimit (container.getX(),
                                            juce::jmax (container.getX(), container.getRight() - w),
                                            target.getRight() - w);
                scoreCand ({ x, y, w, h }, wantH, 0.04f);
            }
        }

        // 4) 上方
        {
            const int availH = target.getY() - gap - container.getY();
            const int availW = container.getWidth();
            if (availH >= minH && availW >= minW)
            {
                const int w = juce::jlimit (minW, juce::jmin (maxW, availW), juce::jmax (prefW, target.getWidth()));
                const int wantH = juce::jmax (minH, heightForWidth (w));
                const int h = juce::jmin (wantH, availH);
                const int y = target.getY() - gap - h;
                const int x = juce::jlimit (container.getX(),
                                            juce::jmax (container.getX(), container.getRight() - w),
                                            target.getRight() - w);
                scoreCand ({ x, y, w, h }, wantH, 0.00f);
            }
        }

        return best;
    };

    const std::vector<juce::Rectangle<int>> noObstacles;

    // Tier 1：嵌入态先在工作区外部空余区域择优
    if (! isFloating && ! workspaceCanvas.isEmpty() && ! ownerInWorkspace.isEmpty())
    {
        const auto wsCand = evalExternal (ownerInWorkspace, workspaceCanvas, otherModulesInWorkspace);

        // 若工作区里被压扁（例如超扁条状窗口导致可见高度 < 65%），检查桌面外部是否有显著更舒展的空间
        if (wsCand.valid)
        {
            if (wsCand.fitRatio < 0.65f && ! screenWorkArea.isEmpty() && ! ownerOnScreen.isEmpty())
            {
                const auto deskCand = evalExternal (ownerOnScreen, screenWorkArea, noObstacles);
                if (deskCand.valid && deskCand.fitRatio >= wsCand.fitRatio + 0.25f)
                    return { ControlDrawerPlacement::Host::desktop, deskCand.rect };
            }
            return { ControlDrawerPlacement::Host::workspace, wsCand.rect };
        }
    }

    // Tier 2：浮动模块 / 最大化模块 / 工作区放不下 → 提升到桌面工作区外侧
    if (! screenWorkArea.isEmpty() && ! ownerOnScreen.isEmpty())
    {
        const auto deskCand = evalExternal (ownerOnScreen, screenWorkArea, noObstacles);
        if (deskCand.valid)
            return { ControlDrawerPlacement::Host::desktop, deskCand.rect };
    }

    // Tier 3：极端全屏铺满显示器（四周物理上无任何外部空间）→ 紧凑右上角卡片，尽量保留左侧画面
    if (ownerContentLocal.getWidth() < 24 || ownerContentLocal.getHeight() < 24)
        return { ControlDrawerPlacement::Host::insideOwner, {} };

    const int margin = juce::jmin (4, juce::jmin (ownerContentLocal.getWidth(), ownerContentLocal.getHeight()) / 8);
    const int availW = juce::jmax (1, ownerContentLocal.getWidth()  - margin * 2);
    const int availH = juce::jmax (1, ownerContentLocal.getHeight() - margin * 2);

    int targetW = prefW;
    if (availW < prefW * 2)
    {
        const int compactW = juce::jmax (juce::jmin (minW, availW),
                                         (int) ((float) ownerContentLocal.getWidth() * 0.62f));
        targetW = juce::jmin (prefW, compactW);
    }
    const int w = juce::jlimit (juce::jmin (availW, 80), availW, targetW);
    const int wantH = heightForWidth (w);
    const int maxH  = juce::jmin (availH, juce::jmax (juce::jmin (availH, minH),
                                                      (int) ((float) availH * 0.88f)));
    const int h     = juce::jmin (wantH, maxH);
    const int x     = juce::jlimit (ownerContentLocal.getX(),
                                    juce::jmax (ownerContentLocal.getX(), ownerContentLocal.getRight() - w),
                                    ownerContentLocal.getRight() - margin - w);
    const int y     = juce::jlimit (ownerContentLocal.getY(),
                                    juce::jmax (ownerContentLocal.getY(), ownerContentLocal.getBottom() - h),
                                    ownerContentLocal.getY() + margin);
    return { ControlDrawerPlacement::Host::insideOwner,
             juce::Rectangle<int> (x, y, w, h).getIntersection (ownerContentLocal) };
}

void ModulePanel::layoutControlDrawer()
{
    if (controlDrawer_ == nullptr || ! controlPanelOpen_)
        return;

    auto* ws = dynamic_cast<ModuleWorkspace*> (getParentComponent());
    auto* top = getTopLevelComponent();
    const bool hasRealPeer = (top != nullptr && top->getPeer() != nullptr);
    const auto ownerScreen = hasRealPeer ? getScreenBounds() : juce::Rectangle<int>();
    juce::Rectangle<int> screenWorkArea;
    if (hasRealPeer && ! ownerScreen.isEmpty())
    {
        if (auto* disp = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (ownerScreen.getCentre()))
            screenWorkArea = disp->userArea.reduced (6);
    }

    const int minW  = controlDrawer_->getMinimumOuterWidth();
    const int prefW = controlDrawer_->getPreferredOuterWidth();
    const int maxW  = controlDrawer_->getMaximumOuterWidth();
    const int gap   = PinkXP::ui (6);
    const int tallestPrefH = controlDrawer_->getTallestTabTotalHeight (prefW);
    const int minH  = juce::jmin (PinkXP::ui (120), juce::jmax (PinkXP::ui (72), tallestPrefH));

    // 用户已经手动拖动过设置面板位置：保留当前位置与宽度（仅在过窄/越界时修正），
    //   随可见参数变化调整高度并钳制在活动范围内
    if (controlDrawer_->hasUserMoved())
    {
        juce::Rectangle<int> clampRect;
        if (controlDrawer_->isDesktopFloating())
            clampRect = screenWorkArea;
        else if (controlDrawer_->getParentComponent() == ws && ws != nullptr)
            clampRect = ws->getCanvasArea().reduced (2);
        else
            clampRect = getContentBounds();

        if (! clampRect.isEmpty())
        {
            const int curW = juce::jlimit (juce::jmin (minW, clampRect.getWidth()),
                                           clampRect.getWidth(),
                                           juce::jmax (minW, controlDrawer_->getWidth()));
            const int wantH = controlDrawer_->getPreferredTotalHeight (curW);
            const int h = juce::jmin (wantH, clampRect.getHeight());
            const int x = juce::jlimit (clampRect.getX(), juce::jmax (clampRect.getX(), clampRect.getRight() - curW),
                                        controlDrawer_->getX());
            const int y = juce::jlimit (clampRect.getY(), juce::jmax (clampRect.getY(), clampRect.getBottom() - h),
                                        controlDrawer_->getY());
            controlDrawer_->setDragClampBounds (clampRect);
            controlDrawer_->setBounds (x, y, curW, h);
            controlDrawer_->ensureLayoutCurrent();
        }
        controlDrawer_->toFront (false);
        return;
    }

    std::vector<juce::Rectangle<int>> otherModules;
    juce::Rectangle<int> wsCanvas;
    if (! isFloating_ && ws != nullptr)
    {
        wsCanvas = ws->getCanvasArea().reduced (4);
        otherModules.reserve ((size_t) ws->getNumModules());
        for (int i = 0; i < ws->getNumModules(); ++i)
        {
            if (auto* m = ws->getModule (i))
                if (m != this && m->isVisible() && ! m->isFloating())
                    otherModules.push_back (m->getBounds());
        }
    }

    auto placement = computeControlDrawerPlacement (
        isFloating_,
        getBounds(),
        wsCanvas,
        otherModules,
        ownerScreen,
        screenWorkArea,
        getContentBounds(),
        prefW, minW, maxW, gap, minH,
        [this] (int w) { return controlDrawer_->getTallestTabTotalHeight (w); });

    // 多标签面板：用全标签最高高度锁定宿主、落位侧与宽度（切换标签 0 横向抖动），
    //   再将实际高度平滑贴合当前激活标签页的内容高度（避免出现多余空白或无谓滚动）。
    if (controlDrawer_->hasTabs() && ! placement.bounds.isEmpty())
    {
        const int w = placement.bounds.getWidth();
        const int activeWantH = controlDrawer_->getPreferredTotalHeight (w);

        juce::Rectangle<int> container;
        juce::Rectangle<int> ownerRef;
        if (placement.host == ControlDrawerPlacement::Host::desktop)
        {
            container = screenWorkArea;
            ownerRef  = ownerScreen;
        }
        else if (placement.host == ControlDrawerPlacement::Host::workspace && ws != nullptr)
        {
            container = wsCanvas;
            ownerRef  = getBounds();
        }
        else
        {
            container = getContentBounds();
        }

        const int maxAllowedH = placement.bounds.getHeight();
        const int finalH = juce::jlimit (1, juce::jmax (1, maxAllowedH), activeWantH);

        int finalY = placement.bounds.getY();
        if (! ownerRef.isEmpty() && placement.bounds.getBottom() <= ownerRef.getY())
        {
            // 位于模块上方：底边紧贴模块顶部
            finalY = placement.bounds.getBottom() - finalH;
        }
        else if (! ownerRef.isEmpty() && placement.bounds.getY() >= ownerRef.getBottom())
        {
            // 位于模块下方：顶边紧贴模块底部
            finalY = placement.bounds.getY();
        }
        else if (! ownerRef.isEmpty() && ! container.isEmpty())
        {
            // 位于模块左右两侧：尽量与模块顶缘齐平并钳制在容器内
            finalY = juce::jlimit (container.getY(),
                                   juce::jmax (container.getY(), container.getBottom() - finalH),
                                   ownerRef.getY());
        }

        if (! container.isEmpty())
            finalY = juce::jlimit (container.getY(),
                                   juce::jmax (container.getY(), container.getBottom() - finalH),
                                   finalY);

        placement.bounds.setY (finalY);
        placement.bounds.setHeight (finalH);
    }

    auto* oldParent = controlDrawer_->getParentComponent();
    const auto oldBounds = controlDrawer_->getBounds();

    if (placement.host == ControlDrawerPlacement::Host::desktop)
    {
        controlDrawer_->setDesktopFloating (true, top);
        controlDrawer_->setDragClampBounds (screenWorkArea);
        controlDrawer_->setBounds (placement.bounds);
        controlDrawer_->ensureLayoutCurrent();
        controlDrawer_->setVisible (true);
        controlDrawer_->toFront (false);
    }
    else
    {
        controlDrawer_->setDesktopFloating (false, nullptr);
        juce::Component* targetParent = (placement.host == ControlDrawerPlacement::Host::workspace && ws != nullptr)
                                            ? static_cast<juce::Component*> (ws)
                                            : static_cast<juce::Component*> (this);
        if (controlDrawer_->getParentComponent() != targetParent)
        {
            if (auto* curP = controlDrawer_->getParentComponent())
                curP->removeChildComponent (controlDrawer_.get());
            targetParent->addChildComponent (*controlDrawer_);
        }
        const auto clampRect = (targetParent == ws && ws != nullptr)
                                   ? ws->getCanvasArea().reduced (2)
                                   : getContentBounds();
        controlDrawer_->setDragClampBounds (clampRect);
        controlDrawer_->setBounds (placement.bounds);
        controlDrawer_->ensureLayoutCurrent();
        controlDrawer_->setVisible (! placement.bounds.isEmpty());
        controlDrawer_->toFront (false);
    }

    if (oldParent != nullptr && oldParent != controlDrawer_->getParentComponent() && ! oldBounds.isEmpty())
        oldParent->repaint (oldBounds);

    attachDrawerDismissListener (this, controlDrawer_->isDesktopFloating());
}

void ModulePanel::setControlPanelOpen (bool open)
{
    if (! hasControlPanel())
        open = false;
    if (controlPanelOpen_ == open)
        return;

    // 打开前先收起别人的：同时只允许一个设置面板，点别的齿轮时旧的自动让位
    if (open)
        closeOpenControlDrawers (nullptr, {});

    controlPanelOpen_ = open;

    if (open)
    {
        if (controlDrawer_ == nullptr)
            controlDrawer_ = std::make_unique<ModuleControlDrawer> (*this);
        else
            controlDrawer_->syncFromModule();

        controlDrawer_->resetUserMoved();
        openDrawerPanels().addIfNotAlreadyThere (this);
        layoutControlDrawer();
    }
    else
    {
        if (controlDrawer_ != nullptr)
        {
            auto* oldParent = controlDrawer_->getParentComponent();
            const auto oldBounds = controlDrawer_->getBounds();
            const bool wasDesktop = controlDrawer_->isDesktopFloating();

            controlDrawer_->setVisible (false);
            if (wasDesktop)
                controlDrawer_->setDesktopFloating (false, nullptr);
            if (oldParent != nullptr)
            {
                oldParent->removeChildComponent (controlDrawer_.get());
                if (! oldBounds.isEmpty())
                    oldParent->repaint (oldBounds);
            }
        }

        openDrawerPanels().removeAllInstancesOf (this);
        if (openDrawerPanels().isEmpty())
            detachDrawerDismissListener();
    }

    repaint (getTitleBarBounds());
}

// 点空白处自动收起：由顶层 / 桌面全局 MouseListener 调用
void ModulePanel::closeOpenControlDrawers (const juce::Component* source,
                                           juce::Point<int>       posInSource)
{
    if (openDrawerPanels().isEmpty())
        return;

    // 点在某个模块的齿轮按钮上 → 由该模块自己切换开合，这里不插手
    if (source != nullptr)
    {
        const ModulePanel* panel = dynamic_cast<const ModulePanel*> (source);
        if (panel == nullptr)
            panel = source->findParentComponentOfClass<ModulePanel>();

        if (panel != nullptr && panel->hasControlPanel()
            && panel->getControlButtonBounds()
                     .contains (panel->getLocalPoint (source, posInSource)))
            return;
    }

    const juce::Array<ModulePanel*> snapshot (openDrawerPanels());
    for (auto* p : snapshot)
    {
        if (p == nullptr || ! p->isControlPanelOpen())
            continue;

        // 点在这个设置面板内部（含其滑块 / 按钮 / 标题条）→ 保留
        if (source != nullptr && p->isComponentInControlDrawer (source))
            continue;

        if (source != nullptr && p->controlDrawer_ != nullptr && p->controlDrawer_->isDesktopFloating())
        {
            const auto screenPt = source->localPointToGlobal (posInSource);
            if (p->controlDrawer_->getScreenBounds().contains (screenPt))
                continue;
        }

        p->setControlPanelOpen (false);
    }
}

void ModulePanel::refreshControlPanel()
{
    if (controlDrawer_ != nullptr && controlPanelOpen_)
    {
        controlDrawer_->syncFromModule();
        layoutControlDrawer();
    }
}

void ModulePanel::revealUndoTarget (const juce::String& focusHint)
{
    // 内嵌视图自带控件（如小工具模块内直接显示的便签/节拍器/换算器），无需弹出齿轮抽屉
    if (focusHint == "inline")
        return;

    if (! hasControlPanel())
        return;

    if (! controlPanelOpen_)
        setControlPanelOpen (true);
    else
        refreshControlPanel();

    if (controlDrawer_ != nullptr && focusHint.startsWith ("drawer:"))
    {
        const auto paramId = focusHint.fromFirstOccurrenceOf ("drawer:", false, false);
        if (paramId.isNotEmpty())
            controlDrawer_->revealParam (paramId);
    }
}

// ==========================================================
// 悬停提示（HoverHelp）默认实现
// ==========================================================
ModulePanel::HoverHelp ModulePanel::getHoverHelp (juce::Point<int> pos) const
{
    HoverHelp h;
    const bool locked = isPanelLayoutLocked (*this);

    if (! bareMode_)
    {
        if (getCloseButtonBounds().contains (pos))
        {
            h.text = y2k::help::make ("Close module",
                locked ? juce::String ("Layout is locked (L in the title bar) - unlock it to remove modules.")
                       : juce::String ("Remove this module. Add it back any time with a right-click "
                                       "or double-click on the empty desktop."));
            return h;
        }

        if ((popOutEnabled_ || isFloating_) && getPopOutButtonBounds().contains (pos))
        {
            h.text = isFloating_
                ? y2k::help::make ("Dock", "Put this module back into the main window at its old place.")
                : y2k::help::make ("Pop out", "Detach this module into its own floating window "
                                              "(great for a second screen). Its settings travel with it.");
            return h;
        }

        if (hasControlPanel() && getControlButtonBounds().contains (pos))
        {
            h.text = y2k::help::make ("Controls",
                "Open the settings panel beside this module so you can tweak display modes, colours and speed while watching live. "
                "Drag its title bar to reposition, or click outside / Esc to close.");
            return h;
        }

        if (getTitleBarBounds().contains (pos))
        {
            h.text = y2k::help::moduleHelp (moduleType) + "\n\n"
                   + y2k::help::moduleTitleUsage (isFloating_, locked);
            return h;
        }
    }

    h.text = y2k::help::moduleHelp (moduleType);
    h.contentZone = true;
    return h;
}

void ModulePanel::setFloating(bool floating)
{
    if (isFloating_ == floating)
        return;
    if (controlPanelOpen_)
        setControlPanelOpen (false);
    isFloating_ = floating;
    if (!isFloating_)
        floatingLayoutLocked_ = false;
    resetRepaintCoalescing();
    repaint(getTitleBarBounds());
}

void ModulePanel::setFloatingLayoutLocked(bool locked) noexcept
{
    if (floatingLayoutLocked_ == locked)
        return;

    floatingLayoutLocked_ = locked;
    if (locked)
    {
        dragMode = DragMode::none;
        resizeEdge = Edge::none;
        closeButtonHovered = false;
        closeButtonPressed = false;
        popOutButtonHovered_ = false;
        popOutButtonPressed_ = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
    repaint();
}

void ModulePanel::setPopOutEnabled(bool enabled) noexcept
{
    if (popOutEnabled_ == enabled)
        return;
    popOutEnabled_ = enabled;
    repaint(getTitleBarBounds());
}

juce::Rectangle<int> ModulePanel::getContentBounds() const
{
    // 裸模式：内容区 = 整个面板（无标题栏/边框）
    if (bareMode_)
        return getLocalBounds();

    // 去掉外边框 2px + 标题栏高度 + 1px 分割线（标题栏高度随 UI 密度缩放）
    const int th = PinkXP::ui(titleBarHeight);
    return juce::Rectangle<int>(
        2,
        2 + th + 1,
        getWidth() - 4,
        getHeight() - 4 - th - 1);
}

// ----------------------------------------------------------
// Toybox（v3.0.0）默认实现（旧模块零改动可编译）
// ----------------------------------------------------------
bool ModulePanel::toyAccepts (juce::Point<int> panelPos) const
{
    // 仅内容区，且排除 8px 边缘缩放热区（§6.1）
    if (! getContentBounds().contains (panelPos))
        return false;
    const int hot = PinkXP::ui (edgeHotSize);
    if (panelPos.x < hot || panelPos.x >= getWidth()  - hot ||
        panelPos.y < hot || panelPos.y >= getHeight() - hot)
        return false;
    return true;
}

bool ModulePanel::toyHitTest (juce::Point<int>, ToyHit&) const
{
    return false;
}

void ModulePanel::toyPoke (const ToyHit&, ToyContext&)
{
}

// 右下角 CPU 小字区域：贴着内容区右下角，宽 74px × 高 12px（留内邊 3px）
juce::Rectangle<int> ModulePanel::getCpuLabelBounds() const
{
    auto c = getContentBounds();
    constexpr int w = 74;
    constexpr int h = 12;
    constexpr int pad = 3;
    return juce::Rectangle<int>(c.getRight()  - w - pad,
                                 c.getBottom() - h - pad,
                                 w, h);
}

void ModulePanel::setCpuLoad (float load01) noexcept
{
    const float pct = juce::jlimit (0.0f, 100.0f, load01 * 100.0f);
    // 大于 0.1% 的变化才 repaint，避免每帧脉冲引发微量重绘
    if (std::abs (pct - cpuPercent) < 0.1f)
        return;
    cpuPercent = pct;
    repaint (getCpuLabelBounds());
}

// ----------------------------------------------------------
// 绘制
// ----------------------------------------------------------
juce::Colour ModulePanel::getShellFillColour() const
{
    return PinkXP::face;
}

void ModulePanel::paint(juce::Graphics& g)
{
    // ---- 诊断：单模块绘制耗时 ----
    //   paint 跑在消息线程上，任一模块超过 40ms 都会造成肉眼可见的掉帧/卡顿；
    //   与 GL 直渲染卡死区分开（GL 卡死时日志会显示 GL 线程停摆而 paint 正常）。
    const bool diagPaint = y2k::diag::isEnabled();
    const double diagPaintStartMs = diagPaint ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    this->diagPaintStartMs = diagPaintStartMs;
    if (diagPaint)
        y2k::diag::setPhase ("UI:ModulePanel::paint");

#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::ui, "UI-ModulePaint");
    const auto paintStartNs = y2k::perf::PerformanceCounterSystem::nowNs();
#endif
#if Y2K_ENABLE_FRAME_PERF_LOG
    const juce::String y2kPerfCat = moduleTypeToString (moduleType);
    Y2K_PERF_FRAME_SCOPE (y2kPerfCat.toRawUTF8());
#endif

    const auto bounds = getLocalBounds();
    const bool transparentHidden = PinkXP::isTransparentBackdrop()
                                   && ! isBareMode()
                                   && ! PinkXP::isChromaRevealed (*this);

    if (! transparentHidden)
    {
        // 1. 像素凸起窗口外壳
        //   v2.9.3：填充色走 getShellFillColour() 钩子 —— GL 直渲染模块返回
        //   transparentBlack，内容区透明以透出 FBO 0 的 GL 内容（边框/标题栏不变）。
        //   v2.9.5：透明外壳改为"face 填充（挖掉 contentBounds）+ 仅边框"——
        //   透明填充会让标题栏/分割线与内容区之间的缝隙行透出 FBO 0（抬头下方
        //   1px 透底），face 兜底后 canvas 外所有像素都不透明；canvas 区域由
        //   paintContent 的挖洞分支保持透明。
        if (getShellFillColour().isTransparent())
        {
            g.setColour (PinkXP::face);
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.excludeClipRegion (getContentBounds());
                g.fillRect (bounds);
            }
            PinkXP::drawRaisedFrame (g, bounds);
        }
        else
        {
            PinkXP::drawRaised(g, bounds, getShellFillColour());
        }

        // 2~3.5：标题栏 / 分割线 / 关闭 & 弹出按钮 —— 均位于 tb（含下沿 1px 分割线）内。
        //   逐帧刷新只失效内容区（repaintFrameContent），此时裁剪区与标题栏不相交，
        //   整段跳过即可（裁剪区外的绘制本来就不会产生像素，输出完全一致），省掉
        //   标题图标/按钮文字逐帧塑形 + 逐 glyph 绘制的 CPU 开销。
        auto tb = getTitleBarBounds();
        if (g.clipRegionIntersects (tb.withHeight (tb.getHeight() + 1)))
        {
            PinkXP::drawPinkTitleBar(g, tb, titleText, 12.0f);

            // 标题栏下沿分割线（凸出边框外 1px；霓虹风格下是灯管亮色，让光漏进内容区）
            g.setColour(PinkXP::titleBarDividerColour());
            g.fillRect(tb.getX(), tb.getBottom(), tb.getWidth(), 1);

            const auto titleFrameStyle = PinkXP::getSkinTokens().frameStyle;
            const bool useCraftTitleBtns = (titleFrameStyle == PinkXP::FrameStyle::machinedChrome
                                            || titleFrameStyle == PinkXP::FrameStyle::frostedGlass
                                            || titleFrameStyle == PinkXP::FrameStyle::neonTube
                                            || titleFrameStyle == PinkXP::FrameStyle::crtPhosphor);
            auto resolveCraftTitleFill = [&] (bool pressed, bool active) -> juce::Colour
            {
                if (titleFrameStyle == PinkXP::FrameStyle::machinedChrome)
                    return (pressed || active) ? PinkXP::pink100 : PinkXP::btnFace;
                if (titleFrameStyle == PinkXP::FrameStyle::crtPhosphor)
                    return (pressed || active) ? PinkXP::sel : PinkXP::btnFace;
                if (titleFrameStyle == PinkXP::FrameStyle::neonTube)
                {
                    if (pressed || active) return PinkXP::sel;
                    return (PinkXP::face.getPerceivedBrightness() < 0.45f)
                               ? PinkXP::face.interpolatedWith (juce::Colours::black, 0.26f)
                               : PinkXP::btnFace;
                }
                // frostedGlass：半透明晶透胶囊按钮
                return pressed ? PinkXP::sel : PinkXP::btnFace;
            };

            // 3. 关闭按钮（×）
            auto cb = getCloseButtonBounds();
            juce::Colour cbInk = PinkXP::ink;
            if (useCraftTitleBtns)
            {
                const auto fill = resolveCraftTitleFill (closeButtonPressed, false);
                const auto st   = closeButtonPressed ? PinkXP::ControlState::pressed
                                : closeButtonHovered ? PinkXP::ControlState::hover
                                                     : PinkXP::ControlState::normal;
                PinkXP::Craft::drawControlChrome (g, cb, fill, st);
                cbInk = PinkXP::readableInkOn (fill);
            }
            else if (closeButtonPressed)
                PinkXP::drawPressed(g, cb, PinkXP::pink100);
            else
                PinkXP::drawRaised(g, cb, closeButtonHovered ? PinkXP::pink200 : PinkXP::btnFace);

            // 图标字符（× / - / =）：恒用英文 UI 字体，不随语言切换变样；
            //   非像素风格下按字形墨迹居中（切风格换字体后不再上下跑位）
            auto cbText = cb;
            if (closeButtonPressed) cbText.translate(1, 1);
            PinkXP::SymbolGlyphs::drawIconGlyph (g, "x", cbText, 11.0f, cbInk, 0, -1);

            // 3.5. 弹出/停靠按钮（仅在 Standalone 模式或已处于浮动态时绘制）
            if (popOutEnabled_ || isFloating_)
            {
                auto popBtn = getPopOutButtonBounds();
                juce::Colour popInk = PinkXP::ink;
                if (useCraftTitleBtns)
                {
                    const auto fill = resolveCraftTitleFill (popOutButtonPressed_, false);
                    const auto st   = popOutButtonPressed_ ? PinkXP::ControlState::pressed
                                    : popOutButtonHovered_ ? PinkXP::ControlState::hover
                                                           : PinkXP::ControlState::normal;
                    PinkXP::Craft::drawControlChrome (g, popBtn, fill, st);
                    popInk = PinkXP::readableInkOn (fill);
                }
                else if (popOutButtonPressed_)
                    PinkXP::drawPressed(g, popBtn, PinkXP::pink100);
                else
                    PinkXP::drawRaised(g, popBtn, popOutButtonHovered_ ? PinkXP::pink200 : PinkXP::btnFace);

                auto popBtnText = popBtn;
                if (popOutButtonPressed_) popBtnText.translate(1, 1);
                PinkXP::SymbolGlyphs::drawIconGlyph (g, isFloating_ ? "=" : "-", popBtnText, 11.0f,
                                                     popInk, 0, -1);
            }

            // 3.6. 控制面板（齿轮）按钮：仅声明了控制面板的模块显示
            if (hasControlPanel())
            {
                auto ctl = getControlButtonBounds();
                const bool down = controlButtonPressed_ || controlPanelOpen_;
                juce::Colour ctlFill = down ? PinkXP::pink100 : PinkXP::btnFace;
                if (useCraftTitleBtns)
                {
                    ctlFill = resolveCraftTitleFill (controlButtonPressed_, controlPanelOpen_);
                    const auto st = controlButtonPressed_ ? PinkXP::ControlState::pressed
                                  : controlPanelOpen_     ? PinkXP::ControlState::active
                                  : controlButtonHovered_ ? PinkXP::ControlState::hover
                                                          : PinkXP::ControlState::normal;
                    PinkXP::Craft::drawControlChrome (g, ctl, ctlFill, st);
                }
                else if (down)
                    PinkXP::drawPressed(g, ctl, PinkXP::pink100);
                else
                    PinkXP::drawRaised(g, ctl, controlButtonHovered_ ? PinkXP::pink200 : PinkXP::btnFace);

                auto icon = ctl.toFloat().reduced (3.0f);
                if (down) icon.translate (1.0f, 1.0f);
                drawGearIcon (g, icon,
                              useCraftTitleBtns ? PinkXP::readableInkOn (ctlFill) : PinkXP::ink,
                              ctlFill);
            }
        } // 标题栏区域
    } // ! transparentHidden

    // 4. 内容区（默认白底凹陷，子类可覆盖 paintContent 重绘）
    auto content = getContentBounds();
    if (content.getWidth() > 0 && content.getHeight() > 0
        && g.clipRegionIntersects (content))
    {
        paintContent(g, content);
    }

    // 注意：CPU 小字放到 paintOverChildren() 里绘制，确保处在所有子组件之上
    // （避免填满内容区的子组件把 paint() 绘制的文字遮挡）

#if Y2K_ENABLE_PERF_COUNTERS
    const auto paintEndNs = y2k::perf::PerformanceCounterSystem::nowNs();
    y2k::perf::PerformanceCounterSystem::instance().recordUiModulePaint((int) moduleType,
                                                                         paintEndNs - paintStartNs);

    const auto areaAll = juce::jmax(1, getWidth() * getHeight());
    double dirtyRatio = 1.0;
    if (pendingAreaRepaint && ! pendingRepaintArea.isEmpty())
    {
        const auto dirtyArea = juce::jmax(1, pendingRepaintArea.getWidth() * pendingRepaintArea.getHeight());
        dirtyRatio = juce::jlimit(0.0, 1.0, (double) dirtyArea / (double) areaAll);
    }
    y2k::perf::PerformanceCounterSystem::instance().recordUiDirtyAreaSample((int) moduleType, dirtyRatio);

    y2k::perf::PerformanceCounterSystem::instance().recordDuration(
        y2k::perf::FunctionId::uiModulePanelPaint,
        y2k::perf::Partition::uiRendering,
        y2k::perf::ThreadRole::ui,
        paintEndNs - paintStartNs,
        0);
#endif

    if (diagPaint)
    {
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - diagPaintStartMs;
        if (elapsedMs > 40.0)
            Y2K_DIAG ("SLOW", "ModulePanel::paint " + moduleTypeToString (moduleType)
                                  + " " + juce::String (elapsedMs, 1) + "ms");
    }
}

void ModulePanel::paintOverChildren(juce::Graphics& g)
{
    // 诊断：paint() 起点 → 子组件绘制完毕（本函数入口）的耗时累计
    if (diagPaintStartMs > 0.0)
    {
        diagPaintMsAccum += juce::Time::getMillisecondCounterHiRes() - diagPaintStartMs;
        diagPaintStartMs = 0.0;
    }

    // 诊断：组件层真正被合成的次数（与 GL 出帧数对比，即可判断"合成饥饿"）
    ++diagPaints;

    // 重绘合并标记在这里统一清理：
    // 某些模块（如 VirtuPet）会重写 paint()，导致 ModulePanel::paint() 不一定执行；
    // 若仅在 paint() 里清理，pendingFullRepaint 可能长期为 true，后续 repaint 全被合并掉。
    pendingFullRepaint = false;
    pendingAreaRepaint = false;
    pendingRepaintArea = {};
    pendingRepaintStampMs = 0;

    // Toybox：模块内 actor 绘制（插在标志清零之后、早退 return 之前，§R5）
    //   设置抽屉是子组件，而 paintOverChildren 画在所有子组件之上 —— 不排除抽屉
    //   区域的话，点模块产生的 emoji / 粒子会盖住设置界面。抽屉打开时把这块裁掉，
    //   actor 进入抽屉范围就"钻到面板后面"。
    if (toyState != nullptr)
    {
        const auto drawer = getControlDrawerBounds();
        if (drawer.isEmpty())
        {
            toyState->paint (g);
        }
        else
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.excludeClipRegion (drawer);
            toyState->paint (g);
        }
    }

    // 绿幕：未悬停时把外壳（标题栏 / 边框 / 按钮 / 控件条 / 抽屉）整块盖成纯绿，
    //   只留 getChromaKeyCanvas() 返回的数据画布（不支持的模块返回空 = 整块纯绿）。
    //   透明模式下严禁刷绿（外壳已在 paint() 中跳过，保持 alpha=0）；
    //   VirtuPet 自身无外壳并在 VirtuPetModule::paint 内隐藏 HUD/聚焦框，不盖纯绿以免遮住宠物精灵。
    if (PinkXP::isGreenScreenBackdrop() && ! isBareMode()
        && moduleType != ModuleType::virtuPet && ! PinkXP::isChromaRevealed (*this))
    {
        const auto green = PinkXP::chromaKeyGreen();
        {
            juce::Graphics::ScopedSaveState ss (g);
            const auto keep = getChromaKeyCanvas();
            if (! keep.isEmpty())
                g.excludeClipRegion (keep);
            g.setColour (green);
            g.fillRect (getLocalBounds());
        }

        const auto drawer = getControlDrawerBounds();
        if (! drawer.isEmpty())
        {
            g.setColour (green);
            g.fillRect (drawer);
        }
    }

    // 右下角 CPU 占用提示文字：暂时屏蔽（不再做视觉展示，但保留
    //   setCpuLoad / getCpuLabelBounds 等 API，方便后续测试时重新开启）
    juce::ignoreUnused (g);
    return;

    // 右下角 CPU 小字（方便定位卡顿原因）—— 所有模块共享同一个 Processor
    // CPU 占比；显示 1 位小数；系统默认字体 + 亮红色。
    auto content = getContentBounds();
    if (content.getWidth() <= 0 || content.getHeight() <= 0) return;

    auto cpuBox = getCpuLabelBounds();
    if (! content.contains (cpuBox)) return;

    const auto txt = juce::String ("CPU: ")
                         + juce::String (cpuPercent, 1)
                         + "%";

    // 系统默认字体，10pt，bold（覆盖 LookAndFeel 的粉色 XP 字体）
    g.setFont (juce::Font (10.0f, juce::Font::bold));

    // 先画一层 65% 黑色阴影（文字下方 1px），保证在亮红不够醒目的背景上也能读
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.drawText (y2k::tr (txt), cpuBox.translated (1, 1), juce::Justification::centredRight, false);

    // CPU 颜色：0% 深暗红 → 9% 及以上 纯亮红，线性插值
    //   · 低负载时字色偏暗，避免满屏都是亮红看不出差异
    //   · 9% 作为阈值：常规空载本机在 2–5% 附近，9% 以上代表有显著负载
    const float t = juce::jlimit (0.0f, 1.0f, cpuPercent / 9.0f);

    // 起点（t=0）：深暗红 #6E0000（亮度低、但仍能看出是红系）
    // 终点（t=1）：纯亮红 juce::Colours::red (#FF0000)
    const juce::Colour coldRed { (juce::uint8) 0x6E, (juce::uint8) 0x00, (juce::uint8) 0x00 };
    const juce::Colour hotRed  = juce::Colours::red;
    const auto fg = coldRed.interpolatedWith (hotRed, t);

    g.setColour (fg);
    g.drawText (y2k::tr (txt), cpuBox, juce::Justification::centredRight, false);
}

void ModulePanel::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    if (PinkXP::isTransparentBackdrop() && ! PinkXP::isChromaRevealed (*this))
        return;
    PinkXP::drawSunken(g, contentBounds, PinkXP::content);
}

ModulePanel::DiagPaintCounters ModulePanel::diagTakePaintCounters() noexcept
{
    DiagPaintCounters c;
    c.paints    = diagPaints;
    c.issued    = diagIssued;
    c.coalesced = diagCoalesced;
    c.skipped   = diagSkipped;
    c.paintMs   = diagPaintMsAccum;

    diagPaints = diagIssued = diagCoalesced = diagSkipped = 0;
    diagPaintMsAccum = 0.0;
    return c;
}

void ModulePanel::resetRepaintCoalescing() noexcept
{
    pendingFullRepaint = false;
    pendingAreaRepaint = false;
    pendingRepaintArea = {};
    pendingRepaintStampMs = 0;
}

void ModulePanel::forceRepaintImmediate()
{
    resetRepaintCoalescing();
    juce::Component::repaint();
}

void ModulePanel::repaint()
{
    if (! isVisuallyActiveInWorkspace())
    {
        ++diagSkipped;
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintSkippedInvisible((int) moduleType);
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintDroppedOffscreen((int) moduleType);
#endif
        return;
    }

    // 合并守卫 + 自愈：
    //   正常情况下上一次 repaint 会在几十毫秒内被 paint()/paintOverChildren() 消费，
    //   标记随之清零。但如果那次 damage 被 JUCE 丢弃了（窗口最小化/恢复、被完全
    //   遮挡、DPI 切换、移动窗口跨屏……），标记会**永久**停在 true，之后这个模块
    //   所有 repaint() 都被合并掉 → 该矩形内再也不会重新合成组件层。
    //   由于 GL 内容（Milkdrop 视频 / GL 直渲染模块）每帧都会覆盖后缓冲，而组件层
    //   不再重合成，视觉上就变成"GL 永远压在所有组件之上、图片和其他模块盖不住"。
    //   → 超时 120ms 仍未消费就强制再发一次，避免永久卡死。
    const auto stampNow = juce::Time::getMillisecondCounter();
    if (pendingFullRepaint && stampNow - pendingRepaintStampMs < 120)
    {
        ++diagCoalesced;
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintCoalesced((int) moduleType);
#endif
        return;
    }

#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintRequest((int) moduleType);
#endif
    ++diagIssued;
    pendingFullRepaint = true;
    pendingAreaRepaint = false;
    pendingRepaintArea = getLocalBounds();
    pendingRepaintStampMs = stampNow;
    juce::Component::repaint();
}

void ModulePanel::repaint(juce::Rectangle<int> area)
{
    if (! isVisuallyActiveInWorkspace())
    {
        ++diagSkipped;
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintSkippedInvisible((int) moduleType);
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintDroppedOffscreen((int) moduleType);
#endif
        return;
    }

    if (area.isEmpty())
        return;

    area = area.getIntersection(getLocalBounds());
    if (area.isEmpty())
        return;

    const auto stampNow = juce::Time::getMillisecondCounter();

    if (pendingFullRepaint && stampNow - pendingRepaintStampMs < 120)
    {
        ++diagCoalesced;
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintCoalesced((int) moduleType);
#endif
        return;
    }

    if (pendingAreaRepaint)
    {
        pendingRepaintArea = pendingRepaintArea.getUnion(area);
        ++diagIssued;
#if Y2K_ENABLE_PERF_COUNTERS
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintCoalesced((int) moduleType);
#endif
        juce::Component::repaint(area);
        return;
    }

#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintRequest((int) moduleType);
#endif
    ++diagIssued;
    pendingAreaRepaint = true;
    pendingRepaintArea = area;
    pendingRepaintStampMs = stampNow;
    juce::Component::repaint(area);
}

juce::Rectangle<int> ModulePanel::getFrameRepaintBounds()
{
    return getContentBounds();
}

void ModulePanel::repaintFrameContent()
{
    // 浮动态：整帧内容都要在消息线程上软件重绘 → 与悬停重绘共享限流预算
    if (floatingRepaintThrottled())
        return;

    const auto area = getFrameRepaintBounds();
    if (area.isEmpty())
        repaint();
    else
        repaint (area);
}

void ModulePanel::repaintHover (juce::Rectangle<int> area)
{
    if (area.isEmpty())
        return;

    // 鼠标事件（高回报率鼠标悬停 / 拖拽）频率远高于显示器刷新率：
    //   软件浮动态按 floatingRepaintHz 限流，嵌入态 / GL 浮动态按 16ms (~60Hz) 限流并补齐尾帧，
    //   与 floatingRepaintThrottled() 完全解耦，不干扰逐帧内容刷新。
    if (! hoverRepaintThrottled())
    {
        repaint (area);
        return;
    }

    // 被限流掉的这一帧不能丢：累积区域，等节流窗口结束补一次 —— 否则鼠标
    //   快速拖拽后停住时，标尺会定格在上一帧的位置（最后一次事件被丢弃）。
    pendingHoverArea_ = pendingHoverArea_.getUnion (area);
    if (hoverCatchUpScheduled_)
        return;

    hoverCatchUpScheduled_ = true;
    juce::Component::SafePointer<ModulePanel> safeThis (this);
    const bool softFloating = isFloating_ && ! floatingGlHosted_;
    const int delayMs = softFloating
        ? juce::jmax (1, 1000 / juce::jmax (1, floatingRepaintHz))
        : 16;

    juce::Timer::callAfterDelay (delayMs, [safeThis]() mutable
    {
        if (auto* p = safeThis.getComponent())
        {
            p->hoverCatchUpScheduled_ = false;
            const auto pending = p->pendingHoverArea_;
            p->pendingHoverArea_ = {};
            if (! pending.isEmpty())
                p->repaint (pending);
        }
    });
}

bool ModulePanel::hoverRepaintThrottled() noexcept
{
    const bool softFloating = isFloating_ && ! floatingGlHosted_;
    const auto minGapMs = (juce::uint32) (softFloating
        ? juce::jmax (1, 1000 / juce::jmax (1, floatingRepaintHz))
        : 16);

    const auto now = juce::Time::getMillisecondCounter();
    if (lastHoverRepaintMs_ != 0 && now - lastHoverRepaintMs_ < minGapMs)
        return true;

    lastHoverRepaintMs_ = juce::jmax ((juce::uint32) 1, now);
    return false;
}

bool ModulePanel::floatingRepaintThrottled() noexcept
{
    if (! isFloating_ || floatingGlHosted_)
        return false;

    const auto now = juce::Time::getMillisecondCounter();
    const auto minGapMs = (juce::uint32) juce::jmax (1, 1000 / juce::jmax (1, floatingRepaintHz));

    if (lastFloatingRepaintMs != 0 && now - lastFloatingRepaintMs < minGapMs)
        return true;

    lastFloatingRepaintMs = juce::jmax ((juce::uint32) 1, now);
    return false;
}

void ModulePanel::resized()
{
    layoutContent(getContentBounds());
    layoutControlDrawer();
}

void ModulePanel::moved()
{
    if (controlPanelOpen_ && controlDrawer_ != nullptr && ! controlDrawer_->hasUserMoved())
        layoutControlDrawer();
}

void ModulePanel::parentHierarchyChanged()
{
    // 面板在 workspace ↔ FloatingModuleWindow 间重新挂载时，旧父容器上记录的
    //   pendingFullRepaint 永远不会被旧父容器的 paintOverChildren 消费；
    //   若不清零，接下来 120ms（~7 帧）内所有 repaint 都会被合并守卫静默吞掉。
    resetRepaintCoalescing();
}

// ----------------------------------------------------------
// 鼠标事件：拖拽移动 / 边缘缩放 / 关闭按钮
// ----------------------------------------------------------
ModulePanel::Edge ModulePanel::detectEdge(juce::Point<int> pos) const
{
    const int hot = PinkXP::ui(edgeHotSize);
    const bool nearLeft   = pos.x < hot;
    const bool nearRight  = pos.x >= getWidth()  - hot;
    const bool nearTop    = pos.y < hot;
    const bool nearBottom = pos.y >= getHeight() - hot;

    // 角优先于边（四角同时命中两条边）
    if (nearLeft  && nearTop)    return Edge::topLeft;
    if (nearRight && nearTop)    return Edge::topRight;
    if (nearLeft  && nearBottom) return Edge::bottomLeft;
    if (nearRight && nearBottom) return Edge::bottomRight;
    if (nearLeft)                return Edge::left;
    if (nearRight)               return Edge::right;
    if (nearTop)                 return Edge::top;
    if (nearBottom)              return Edge::bottom;
    return Edge::none;
}

void ModulePanel::updateCursorFor(Edge e)
{
    switch (e)
    {
        case Edge::left:
        case Edge::right:        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);         break;
        case Edge::top:
        case Edge::bottom:       setMouseCursor(juce::MouseCursor::UpDownResizeCursor);            break;
        case Edge::topLeft:      setMouseCursor(juce::MouseCursor::TopLeftCornerResizeCursor);     break;
        case Edge::bottomRight:  setMouseCursor(juce::MouseCursor::BottomRightCornerResizeCursor); break;
        case Edge::topRight:     setMouseCursor(juce::MouseCursor::TopRightCornerResizeCursor);    break;
        case Edge::bottomLeft:   setMouseCursor(juce::MouseCursor::BottomLeftCornerResizeCursor);  break;
        default:                 setMouseCursor(juce::MouseCursor::NormalCursor);                  break;
    }
}

void ModulePanel::mouseEnter(const juce::MouseEvent& e)
{
    mouseMove(e);
}

void ModulePanel::mouseExit(const juce::MouseEvent&)
{
    closeButtonHovered = false;
    popOutButtonHovered_ = false;
    controlButtonHovered_ = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint(getCloseButtonBounds());
    if (popOutEnabled_ || isFloating_)
        repaint(getPopOutButtonBounds());
    if (hasControlPanel())
        repaint(getControlButtonBounds());
}

void ModulePanel::mouseMove(const juce::MouseEvent& e)
{
    // 锁定态：不展示 resize 光标，也不展示关闭按钮 hover（关闭已被禁用）。
    const bool locked = isPanelLayoutLocked (*this);

    // 关闭按钮悬浮：锁定态下一律不高亮（确保与"不可点"视觉一致）
    const bool hovered = (! locked) && getCloseButtonBounds().contains(e.getPosition());
    if (hovered != closeButtonHovered)
    {
        closeButtonHovered = hovered;
        repaint(getCloseButtonBounds());
    }

    // 弹出按钮悬浮
    const bool popShow = (popOutEnabled_ || isFloating_);
    bool popHovered = false;
    if (popShow && ! locked)
    {
        popHovered = getPopOutButtonBounds().contains(e.getPosition());
        if (popHovered != popOutButtonHovered_)
        {
            popOutButtonHovered_ = popHovered;
            repaint(getPopOutButtonBounds());
        }
    }

    // 控制按钮悬浮（控制面板不是布局操作：锁定态下也可用）
    bool ctlHovered = false;
    if (hasControlPanel())
    {
        ctlHovered = getControlButtonBounds().contains(e.getPosition());
        if (ctlHovered != controlButtonHovered_)
        {
            controlButtonHovered_ = ctlHovered;
            repaint(getControlButtonBounds());
        }
    }

    // 边缘缩放 cursor（锁定态下不变光标；浮动态下也不启用模块自身 resize —— 由窗口边框处理）
    if (locked || isFloating_)
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    if (!hovered && !popHovered && !ctlHovered)
        updateCursorFor(detectEdge(e.getPosition()));
    else
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void ModulePanel::mouseDown(const juce::MouseEvent& e)
{
    // 右键 → 仅嵌入态模块允许冒泡给 workspace 弹出"添加模块"菜单。
    // 浮动窗口内没有添加模块语义，避免脱离后右键误触发主窗口菜单。
    if (e.mods.isPopupMenu())
    {
        if (!isFloating_ && onRightClick)
            onRightClick (*this, e.getPosition());
        return;
    }

    // 抹到顶层（若在工作区内已是最上层普通模块，则跳过无谓的 z-order 重排与整块重绘）
    bool alreadyTopmost = false;
    if (! isFloating_)
    {
        if (auto* ws = dynamic_cast<ModuleWorkspace*> (getParentComponent()))
        {
            if (ws->isModuleTopmostAmongModules (*this))
            {
                alreadyTopmost = true;
                ws->clearOtherFocusVisuals (*this);
                if (getWantsKeyboardFocus() && ! hasKeyboardFocus (true))
                    grabKeyboardFocus();
            }
        }
    }
    if (! alreadyTopmost)
    {
        toFront(true);
        if (onBroughtToFront)
            onBroughtToFront(*this);
    }

    const auto pos = e.getPosition();

    // 0) 控制按钮：不是布局操作，锁定态下也允许（放在锁定早退之前）
    if (hasControlPanel() && getControlButtonBounds().contains(pos))
    {
        controlButtonPressed_ = true;
        repaint(getControlButtonBounds());
        return;
    }

    // 布局锁定态：关闭 / resize / move 均不启动（锁定不允许删除子组件）。
    if (isPanelLayoutLocked (*this))
        return;

    // 1) 点击关闭按钮
    if (getCloseButtonBounds().contains(pos))
    {
        closeButtonPressed = true;
        repaint(getCloseButtonBounds());
        return;
    }

    // 1.5) 点击弹出/停靠按钮
    if ((popOutEnabled_ || isFloating_) && getPopOutButtonBounds().contains(pos))
    {
        popOutButtonPressed_ = true;
        repaint(getPopOutButtonBounds());
        return;
    }

    // 浮动态：标题栏区域（非按钮）用于拖拽整个浮动窗口
    if (isFloating_ && getTitleBarBounds().contains(pos))
    {
        if (auto* topLevel = getTopLevelComponent())
        {
            floatingWindowDragger_.startDraggingComponent(
                topLevel, e.getEventRelativeTo(topLevel));
            isDraggingFloatingWindow_ = true;
        }
        return;
    }

    // 2) 点击边缘 -> 开始缩放
    const auto edge = detectEdge(pos);
    if (edge != Edge::none)
    {
        dragMode = DragMode::resize;
        resizeEdge = edge;
        dragStartMouse = e.getEventRelativeTo(getParentComponent()).getPosition();
        dragStartBounds = getBounds();
        return;
    }

    // 3) 点击标题栏 -> 开始移动
    if (getTitleBarBounds().contains(pos))
    {
        dragMode = DragMode::move;
        dragStartMouse = e.getEventRelativeTo(getParentComponent()).getPosition();
        dragStartBounds = getBounds();
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }
}

void ModulePanel::mouseDrag(const juce::MouseEvent& e)
{
    if (isPanelLayoutLocked(*this))
        return;

    // 浮动态：标题栏拖拽 → 移动顶层窗口（仅当从标题栏 mouseDown 启动的拖拽）
    if (isFloating_)
    {
        if (isDraggingFloatingWindow_)
        {
            if (auto* topLevel = getTopLevelComponent())
                floatingWindowDragger_.dragComponent(topLevel, e.getEventRelativeTo(topLevel), nullptr);
        }
        return;
    }

    if (dragMode == DragMode::none)
        return;

    const auto mouseInParent = e.getEventRelativeTo(getParentComponent()).getPosition();
    const auto delta = mouseInParent - dragStartMouse;

    if (dragMode == DragMode::move)
    {
        auto newTopLeft = dragStartBounds.getTopLeft() + delta;
        // 不出父容器
        if (auto* parent = getParentComponent())
        {
            const int maxX = juce::jmax(0, parent->getWidth()  - getWidth());
            const int maxY = juce::jmax(0, parent->getHeight() - getHeight());
            newTopLeft.x = juce::jlimit(0, maxX, newTopLeft.x);
            newTopLeft.y = juce::jlimit(0, maxY, newTopLeft.y);
        }
        setTopLeftPosition(newTopLeft);
    }
    else if (dragMode == DragMode::resize)
    {
        // 四边 resize：锚点边固定，拖动边跟随鼠标（delta）。
        //   用 left/top/right/bottom 四条边统一表达，避免"只调宽高"无法处理
        //   左/上边（它们会移动 x/y，锚点在对面）。
        int left   = dragStartBounds.getX();
        int top    = dragStartBounds.getY();
        int right  = dragStartBounds.getRight();
        int bottom = dragStartBounds.getBottom();

        const bool touchLeft   = (resizeEdge == Edge::left   || resizeEdge == Edge::topLeft    || resizeEdge == Edge::bottomLeft);
        const bool touchRight  = (resizeEdge == Edge::right  || resizeEdge == Edge::topRight   || resizeEdge == Edge::bottomRight);
        const bool touchTop    = (resizeEdge == Edge::top    || resizeEdge == Edge::topLeft    || resizeEdge == Edge::topRight);
        const bool touchBottom = (resizeEdge == Edge::bottom || resizeEdge == Edge::bottomLeft || resizeEdge == Edge::bottomRight);

        if (touchLeft)   left   = dragStartBounds.getX()      + delta.x;
        if (touchRight)  right  = dragStartBounds.getRight()  + delta.x;
        if (touchTop)    top    = dragStartBounds.getY()      + delta.y;
        if (touchBottom) bottom = dragStartBounds.getBottom() + delta.y;

        // 最小尺寸约束
        if (touchLeft)   left   = juce::jmin (left,   right - minW);
        if (touchRight)  right  = juce::jmax (right,  left  + minW);
        if (touchTop)    top    = juce::jmin (top,    bottom - minH);
        if (touchBottom) bottom = juce::jmax (bottom, top   + minH);

        // 不出父容器
        if (auto* parent = getParentComponent())
        {
            left   = juce::jmax (0, left);
            top    = juce::jmax (0, top);
            right  = juce::jmin (parent->getWidth(),  right);
            bottom = juce::jmin (parent->getHeight(), bottom);
        }

        setBounds (juce::Rectangle<int> (left, top,
                                         juce::jmax (1, right - left),
                                         juce::jmax (1, bottom - top)));
    }

    // 拖拽过程中持续回调（workspace 用它更新吸附预览）
    if (onBoundsDragging)
        onBoundsDragging(*this);
}

void ModulePanel::mouseUp(const juce::MouseEvent& e)
{
    // 控制按钮点击判定（锁定态下同样有效，所以放在锁定早退之前）
    if (controlButtonPressed_)
    {
        controlButtonPressed_ = false;
        const bool stillOnBtn = hasControlPanel() && getControlButtonBounds().contains(e.getPosition());
        repaint(getTitleBarBounds());
        if (stillOnBtn)
            setControlPanelOpen (! controlPanelOpen_);
        return;
    }

    if (isPanelLayoutLocked(*this))
    {
        closeButtonPressed = false;
        popOutButtonPressed_ = false;
        dragMode = DragMode::none;
        resizeEdge = Edge::none;
        isDraggingFloatingWindow_ = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint(getTitleBarBounds());
        return;
    }

    // 关闭按钮点击判定：按下和抬起都在按钮上
    if (closeButtonPressed)
    {
        closeButtonPressed = false;
        const bool stillOnCloseBtn = getCloseButtonBounds().contains(e.getPosition());
        repaint(getCloseButtonBounds());

        if (stillOnCloseBtn && onCloseClicked)
        {
            // 先拷到本地栈再调用：防止回调内部重绑或销毁本面板导致正在执行的 std::function 自毁 UAF
            auto cb = onCloseClicked;
            cb(*this);
        }
        return;
    }

    // 弹出/停靠按钮点击判定
    if (popOutButtonPressed_)
    {
        popOutButtonPressed_ = false;
        const bool stillOnBtn = getPopOutButtonBounds().contains(e.getPosition());
        repaint(getPopOutButtonBounds());

        if (stillOnBtn && onPopOutClicked)
        {
            // 先拷到本地栈再调用：popOutModule / dockModule 会立刻覆写 panel.onPopOutClicked，
            //   直接调用成员会造成正在执行的闭包的 SBO 内存被原地覆写（UAF + 类型混淆）。
            auto cb = onPopOutClicked;
            cb(*this);
        }
        return;
    }

    if (dragMode != DragMode::none || isDraggingFloatingWindow_)
    {
        // v3.0.1：只有几何**真的变了**才回调。
        //   单纯点一下抬头时 dragMode 已经进入 move 态（但没移动），
        //   旧实现会照常触发 onBoundsChangedByUser → 工作区吸附 + 通知存档，
        //   顺带把"抬头双击最大化"的状态清掉，导致第二次双击无法恢复。
        const bool boundsChanged = (getBounds() != dragStartBounds);
        const bool floatingDrag  = isDraggingFloatingWindow_;

        dragMode = DragMode::none;
        resizeEdge = Edge::none;
        isDraggingFloatingWindow_ = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);

        if ((boundsChanged || floatingDrag) && onBoundsChangedByUser)
            onBoundsChangedByUser(*this);
    }
}

// ----------------------------------------------------------
// v3.0.1：抬头（标题栏空白区）双击
//   · 嵌入式：工作区内最大化 / 恢复（详见 ModuleWorkspace::toggleModuleMaximize）
//   · 浮动态：浮动窗口伪全屏 / 恢复（由 Editor 接管）
//   · 刻意排除关闭 / 弹出按钮：它们的点击已经有单击语义，双击不应触发最大化
// ----------------------------------------------------------
void ModulePanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    if (! getTitleBarBounds().contains (pos))
        return;
    if (getCloseButtonBounds().contains (pos))
        return;
    if ((popOutEnabled_ || isFloating_) && getPopOutButtonBounds().contains (pos))
        return;
    if (hasControlPanel() && getControlButtonBounds().contains (pos))
        return;

    // 双击前的 mouseDown 已经启动过拖拽（浮动态拖窗口 / 嵌入态移动），
    //   这里复位，避免双击之后残留拖动状态。
    dragMode = DragMode::none;
    resizeEdge = Edge::none;
    isDraggingFloatingWindow_ = false;
    setMouseCursor (juce::MouseCursor::NormalCursor);

    if (onTitleDoubleClicked)
        onTitleDoubleClicked (*this);
}
