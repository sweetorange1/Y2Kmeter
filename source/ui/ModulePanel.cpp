#include "source/ui/ModulePanel.h"
#include "source/ui/ModuleControlDrawer.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/perf/PerformanceCounterSystem.h"
#include "source/perf/FramePerfLog.h"
#include "source/diag/Y2KDiagLog.h"

namespace
{
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
// 设置抽屉的"点空白处自动收起"
//   · openDrawerPanels：当前打开着抽屉的模块（策略：同时只允许一个）
//   · 监听挂在 topLevel 上（wantsEventsForAllNestedChildComponents = true）：
//     窗口内任何组件的鼠标按下都会通知我们，浮动窗口各自挂自己的顶层。
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

    struct DrawerDismissListener : public juce::MouseListener
    {
        void mouseDown (const juce::MouseEvent& e) override
        {
            ModulePanel::closeOpenControlDrawers (e.eventComponent, e.getPosition());
        }
    };

    DrawerDismissListener& drawerDismissListener()
    {
        static DrawerDismissListener listener;
        return listener;
    }

    void attachDrawerDismissListener (juce::Component* owner)
    {
        auto* top = (owner != nullptr) ? owner->getTopLevelComponent() : nullptr;
        if (top == nullptr || drawerListenerHost().getComponent() == top)
            return;

        if (drawerListenerHost().getComponent() != nullptr)
            drawerListenerHost()->removeMouseListener (&drawerDismissListener());

        top->addMouseListener (&drawerDismissListener(), true);
        drawerListenerHost() = top;
    }

    void detachDrawerDismissListener()
    {
        if (drawerListenerHost().getComponent() != nullptr)
            drawerListenerHost()->removeMouseListener (&drawerDismissListener());
        drawerListenerHost() = nullptr;
    }
}

// ==========================================================
// getModuleDisplayName —— 模块类型 -> 默认标题
// ==========================================================
juce::String getModuleDisplayName(ModuleType t)
{
    switch (t)
    {
        case ModuleType::eq:                return "Spectrum Perlerbeads";
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
}

ModulePanel::~ModulePanel()
{
    // 模块销毁时摘掉注册/监听，避免"点空白处收起"持有悬空指针
    openDrawerPanels().removeAllInstancesOf (this);
    if (openDrawerPanels().isEmpty())
        detachDrawerDismissListener();
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
// 控制面板（v3.1）：开关 / 抽屉布局
// ----------------------------------------------------------
void ModulePanel::setControlPanelEnabled (bool enabled) noexcept
{
    controlPanelEnabled_ = enabled;
}

juce::Rectangle<int> ModulePanel::getControlDrawerBounds() const
{
    return (controlDrawer_ != nullptr && controlPanelOpen_) ? controlDrawer_->getBounds()
                                                            : juce::Rectangle<int>();
}

void ModulePanel::layoutControlDrawer()
{
    if (controlDrawer_ == nullptr || ! controlPanelOpen_)
        return;

    const auto content = getContentBounds();
    if (content.getWidth() < 24 || content.getHeight() < 24)
    {
        controlDrawer_->setBounds ({});
        return;
    }

    const int wantH = controlDrawer_->getContentHeight (content.getWidth() - 4) + 4;
    const int maxH  = juce::jmax (40, (int) ((float) content.getHeight() * 0.8f));
    controlDrawer_->setBounds (content.getX(), content.getY(),
                               content.getWidth(), juce::jmin (wantH, maxH));
    controlDrawer_->toFront (false);
}

void ModulePanel::setControlPanelOpen (bool open)
{
    if (! hasControlPanel())
        open = false;
    if (controlPanelOpen_ == open)
        return;

    // 打开前先收起别人的：同时只允许一个抽屉，点别的齿轮时旧的自动让位
    if (open)
        closeOpenControlDrawers (nullptr, {});

    controlPanelOpen_ = open;

    if (open)
    {
        if (controlDrawer_ == nullptr)
        {
            controlDrawer_ = std::make_unique<ModuleControlDrawer> (*this);
            addChildComponent (*controlDrawer_);
        }
        else
        {
            controlDrawer_->syncFromModule();
        }
        controlDrawer_->setVisible (true);
        layoutControlDrawer();

        openDrawerPanels().addIfNotAlreadyThere (this);
        attachDrawerDismissListener (this);
    }
    else
    {
        if (controlDrawer_ != nullptr)
            controlDrawer_->setVisible (false);

        // 注册表 / 监听无论如何都要摘干净（防御：抽屉组件可能已被提前清掉）
        openDrawerPanels().removeAllInstancesOf (this);
        if (openDrawerPanels().isEmpty())
            detachDrawerDismissListener();
    }

    // 抽屉出现 / 消失会改变内容区的遮挡关系：整块重绘一次
    repaint();
}

// 点空白处自动收起：由顶层 MouseListener 调用（source = 实际被点中的组件）
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

    // 拷贝一份再关：关闭会改写注册表
    const juce::Array<ModulePanel*> snapshot (openDrawerPanels());
    for (auto* p : snapshot)
    {
        if (p == nullptr || ! p->isControlPanelOpen())
            continue;

        // 点在这个抽屉内部（含其子控件）→ 保留
        if (source != nullptr
            && p->getControlDrawerBounds().contains (p->getLocalPoint (source, posInSource)))
            continue;

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
                "Open the settings drawer for this module: display modes, items shown and speed. "
                "Click again to fold it away. Closed, the module looks exactly like before.");
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

void ModulePanel::setFloating(bool floating) noexcept
{
    if (isFloating_ == floating)
        return;
    isFloating_ = floating;
    if (!isFloating_)
        floatingLayoutLocked_ = false;
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

        // 标题栏下沿深色分割线（凸出边框外 1px）
        g.setColour(PinkXP::dark);
        g.fillRect(tb.getX(), tb.getBottom(), tb.getWidth(), 1);

        // 3. 关闭按钮（×）
        auto cb = getCloseButtonBounds();
        if (closeButtonPressed)
            PinkXP::drawPressed(g, cb, PinkXP::pink100);
        else
            PinkXP::drawRaised(g, cb, closeButtonHovered ? PinkXP::pink200 : PinkXP::btnFace);

        g.setColour(PinkXP::ink);
        // 图标字符（× / - / =）：恒用英文 UI 字体，不随语言切换变样
        g.setFont(PinkXP::SymbolGlyphs::iconFont(11.0f, juce::Font::bold));
        auto cbText = cb;
        cbText.translate(0, -1);
        if (closeButtonPressed) cbText.translate(1, 1);
        g.drawText(y2k::tr ("x"), cbText, juce::Justification::centred, false);

        // 3.5. 弹出/停靠按钮（仅在 Standalone 模式或已处于浮动态时绘制）
        if (popOutEnabled_ || isFloating_)
        {
            auto popBtn = getPopOutButtonBounds();
            if (popOutButtonPressed_)
                PinkXP::drawPressed(g, popBtn, PinkXP::pink100);
            else
                PinkXP::drawRaised(g, popBtn, popOutButtonHovered_ ? PinkXP::pink200 : PinkXP::btnFace);

            g.setColour(PinkXP::ink);
            g.setFont(PinkXP::SymbolGlyphs::iconFont(11.0f, juce::Font::bold));
            auto popBtnText = popBtn;
            popBtnText.translate(0, -1);
            if (popOutButtonPressed_) popBtnText.translate(1, 1);
            g.drawText(y2k::tr (isFloating_ ? "=" : "-"), popBtnText, juce::Justification::centred, false);
        }

        // 3.6. 控制面板（齿轮）按钮：仅声明了控制面板的模块显示
        if (hasControlPanel())
        {
            auto ctl = getControlButtonBounds();
            const bool down = controlButtonPressed_ || controlPanelOpen_;
            if (down)
                PinkXP::drawPressed(g, ctl, PinkXP::pink100);
            else
                PinkXP::drawRaised(g, ctl, controlButtonHovered_ ? PinkXP::pink200 : PinkXP::btnFace);

            auto icon = ctl.toFloat().reduced (3.0f);
            if (down) icon.translate (1.0f, 1.0f);
            drawGearIcon (g, icon, PinkXP::ink, down ? PinkXP::pink100 : PinkXP::btnFace);
        }
    } // 标题栏区域

    // 4. 内容区（默认白底凹陷，子类可覆盖 paintContent 重绘）
    auto content = getContentBounds();
    if (content.getWidth() > 0 && content.getHeight() > 0
        && g.clipRegionIntersects (content))
    {
        paintContent(g, content);
    }

    // 注意：CPU 小字放到 paintOverChildren() 里绘制，确保处在所有子组件之上
    // （例如 EQ 模块把 PixelEqGraph 覆盖了整个内容区，paint() 绘制的文字会被遮挡）

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
    const auto area = getFrameRepaintBounds();
    if (area.isEmpty())
        repaint();
    else
        repaint (area);
}

void ModulePanel::resized()
{
    layoutContent(getContentBounds());
    layoutControlDrawer();
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

    // 抹到顶层
    toFront(true);
    if (onBroughtToFront)
        onBroughtToFront(*this);

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
            onCloseClicked(*this);
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
            onPopOutClicked(*this);
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
