#include "source/ui/ModulePanel.h"
#include "source/ui/PinkXPStyle.h"
#include "source/perf/PerformanceCounterSystem.h"

// ==========================================================
// getModuleDisplayName —— 模块类型 -> 默认标题
// ==========================================================
juce::String getModuleDisplayName(ModuleType t)
{
    switch (t)
    {
        case ModuleType::eq:                return "EQ";
        case ModuleType::loudness:          return "Loudness";
        case ModuleType::oscilloscope:      return "Oscilloscope";
        case ModuleType::spectrum:          return "Spectrum";
        case ModuleType::phase:             return "Phase";
        case ModuleType::dynamics:          return "Dynamics";

        case ModuleType::lufsRealtime:      return "LUFS Real-time";
        case ModuleType::truePeak:          return "True Peak";
        case ModuleType::oscilloscopeLeft:  return "Oscilloscope L";
        case ModuleType::oscilloscopeRight: return "Oscilloscope R";
        case ModuleType::phaseCorrelation:  return "Phase Correlation";
        case ModuleType::phaseBalance:      return "Phase Balance";
        case ModuleType::dynamicsMeters:    return "Dynamics Meters";
        case ModuleType::dynamicsDr:        return "Dynamics DR";
        case ModuleType::dynamicsCrest:     return "Dynamics Crest";
        case ModuleType::waveform:          return "Waveform";
        case ModuleType::vuMeter:           return "VU Meter";
        case ModuleType::spectrogram:       return "Spectrogram";
        case ModuleType::tamagotchi:        return "Tamagotchi";

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
}

ModulePanel::~ModulePanel() = default;

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
    // 标题栏在外框 2px 内
    return juce::Rectangle<int>(2, 2, getWidth() - 4, titleBarHeight);
}

juce::Rectangle<int> ModulePanel::getCloseButtonBounds() const
{
    auto tb = getTitleBarBounds();
    const int y = tb.getY() + (tb.getHeight() - closeButtonSize) / 2;
    return juce::Rectangle<int>(tb.getRight() - 4 - closeButtonSize, y, closeButtonSize, closeButtonSize);
}

juce::Rectangle<int> ModulePanel::getContentBounds() const
{
    // 去掉外边框 2px + 标题栏高度 + 1px 分割线
    return juce::Rectangle<int>(
        2,
        2 + titleBarHeight + 1,
        getWidth() - 4,
        getHeight() - 4 - titleBarHeight - 1);
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
void ModulePanel::paint(juce::Graphics& g)
{
    y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::ui, "UI-ModulePaint");
    const auto paintStartNs = y2k::perf::PerformanceCounterSystem::nowNs();

    const auto bounds = getLocalBounds();

    // 1. 像素凸起窗口外壳
    PinkXP::drawRaised(g, bounds, PinkXP::face);

    // 2. 玫瑰粉标题栏
    auto tb = getTitleBarBounds();
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
    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    auto cbText = cb;
    cbText.translate(-1, -1);
    if (closeButtonPressed) cbText.translate(1, 1);
    g.drawText("x", cbText, juce::Justification::centred, false);

    // 4. 内容区（默认白底凹陷，子类可覆盖 paintContent 重绘）
    auto content = getContentBounds();
    if (content.getWidth() > 0 && content.getHeight() > 0)
    {
        paintContent(g, content);
    }

    // 注意：CPU 小字放到 paintOverChildren() 里绘制，确保处在所有子组件之上
    // （例如 EQ 模块把 PixelEqGraph 覆盖了整个内容区，paint() 绘制的文字会被遮挡）

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

    pendingFullRepaint = false;
    pendingAreaRepaint = false;
    pendingRepaintArea = {};
}

void ModulePanel::paintOverChildren(juce::Graphics& g)
{
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
    g.drawText (txt, cpuBox.translated (1, 1), juce::Justification::centredRight, false);

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
    g.drawText (txt, cpuBox, juce::Justification::centredRight, false);
}

void ModulePanel::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    PinkXP::drawSunken(g, contentBounds, PinkXP::content);
}

void ModulePanel::repaint()
{
    if (! isVisuallyActiveInWorkspace())
    {
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintSkippedInvisible((int) moduleType);
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintDroppedOffscreen((int) moduleType);
        return;
    }

    if (pendingFullRepaint)
    {
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintCoalesced((int) moduleType);
        return;
    }

    y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintRequest((int) moduleType);
    pendingFullRepaint = true;
    pendingAreaRepaint = false;
    pendingRepaintArea = getLocalBounds();
    juce::Component::repaint();
}

void ModulePanel::repaint(juce::Rectangle<int> area)
{
    if (! isVisuallyActiveInWorkspace())
    {
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintSkippedInvisible((int) moduleType);
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintDroppedOffscreen((int) moduleType);
        return;
    }

    if (area.isEmpty())
        return;

    area = area.getIntersection(getLocalBounds());
    if (area.isEmpty())
        return;

    if (pendingFullRepaint)
    {
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintCoalesced((int) moduleType);
        return;
    }

    if (pendingAreaRepaint)
    {
        pendingRepaintArea = pendingRepaintArea.getUnion(area);
        y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintCoalesced((int) moduleType);
        juce::Component::repaint(area);
        return;
    }

    y2k::perf::PerformanceCounterSystem::instance().recordUiRepaintRequest((int) moduleType);
    pendingAreaRepaint = true;
    pendingRepaintArea = area;
    juce::Component::repaint(area);
}

void ModulePanel::resized()
{
    layoutContent(getContentBounds());
}

// ----------------------------------------------------------
// 鼠标事件：拖拽移动 / 边缘缩放 / 关闭按钮
// ----------------------------------------------------------
ModulePanel::Edge ModulePanel::detectEdge(juce::Point<int> pos) const
{
    const bool nearRight  = pos.x >= getWidth()  - edgeHotSize;
    const bool nearBottom = pos.y >= getHeight() - edgeHotSize;

    if (nearRight && nearBottom) return Edge::bottomRight;
    if (nearRight)                return Edge::right;
    if (nearBottom)               return Edge::bottom;
    return Edge::none;
}

void ModulePanel::updateCursorFor(Edge e)
{
    switch (e)
    {
        case Edge::right:        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); break;
        case Edge::bottom:       setMouseCursor(juce::MouseCursor::UpDownResizeCursor);    break;
        case Edge::bottomRight:  setMouseCursor(juce::MouseCursor::BottomRightCornerResizeCursor); break;
        default:                 setMouseCursor(juce::MouseCursor::NormalCursor);          break;
    }
}

void ModulePanel::mouseEnter(const juce::MouseEvent& e)
{
    mouseMove(e);
}

void ModulePanel::mouseExit(const juce::MouseEvent&)
{
    closeButtonHovered = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint(getCloseButtonBounds());
}

void ModulePanel::mouseMove(const juce::MouseEvent& e)
{
    // 关闭按钮悬浮
    const bool hovered = getCloseButtonBounds().contains(e.getPosition());
    if (hovered != closeButtonHovered)
    {
        closeButtonHovered = hovered;
        repaint(getCloseButtonBounds());
    }

    // 边缘缩放 cursor
    if (!hovered)
        updateCursorFor(detectEdge(e.getPosition()));
    else
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void ModulePanel::mouseDown(const juce::MouseEvent& e)
{
    // 抬到顶层
    toFront(true);
    if (onBroughtToFront)
        onBroughtToFront(*this);

    const auto pos = e.getPosition();

    // 1) 点击关闭按钮
    if (getCloseButtonBounds().contains(pos))
    {
        closeButtonPressed = true;
        repaint(getCloseButtonBounds());
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
        auto newBounds = dragStartBounds;
        if (resizeEdge == Edge::right || resizeEdge == Edge::bottomRight)
            newBounds.setWidth(juce::jmax(minW, dragStartBounds.getWidth()  + delta.x));
        if (resizeEdge == Edge::bottom || resizeEdge == Edge::bottomRight)
            newBounds.setHeight(juce::jmax(minH, dragStartBounds.getHeight() + delta.y));

        // 不出父容器
        if (auto* parent = getParentComponent())
        {
            newBounds.setWidth (juce::jmin(newBounds.getWidth(),  parent->getWidth()  - newBounds.getX()));
            newBounds.setHeight(juce::jmin(newBounds.getHeight(), parent->getHeight() - newBounds.getY()));
        }

        setBounds(newBounds);
    }

    // 拖拽过程中持续回调（workspace 用它更新吸附预览）
    if (onBoundsDragging)
        onBoundsDragging(*this);
}

void ModulePanel::mouseUp(const juce::MouseEvent& e)
{
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

    if (dragMode != DragMode::none)
    {
        dragMode = DragMode::none;
        resizeEdge = Edge::none;
        setMouseCursor(juce::MouseCursor::NormalCursor);

        if (onBoundsChangedByUser)
            onBoundsChangedByUser(*this);
    }
}