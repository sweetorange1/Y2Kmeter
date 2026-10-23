#include "PluginEditor.h"
#include <JuceHeader.h>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include "BinaryData.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/modules/EqModule.h"
#include "source/ui/modules/LoudnessModule.h"
#include "source/ui/modules/OscilloscopeModule.h"
#include "source/ui/modules/OscilloscopeWaveModule.h"
#include "source/ui/modules/SpectrumModule.h"
#include "source/ui/modules/PhaseModule.h"
#include "source/ui/modules/DynamicsModule.h"
#include "source/ui/modules/FineSplitModules.h"
#include "source/ui/modules/WaveformModule.h"
#include "source/ui/modules/SpectrogramModule.h"
#include "source/ui/modules/Spectrogram3DModule.h"
#include "source/ui/modules/StereoFieldModule.h"
#include "source/ui/modules/VirtuPetModule.h"
#include "source/ui/modules/MilkdropModule.h"
#include "source/ui/modules/ProjectMApi.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/MilkdropParamIds.h"
#include "source/ui/GlDirectModule.h"
#include "source/perf/FramePerfLog.h"
#include "source/Y2KLogging.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX   // 阻止 windows.h 定义 min/max 宏（污染 std::min/std::max）
#endif
#include <windows.h>
#include <shellapi.h>

namespace {
    // Windows AppBar（SHAppBarMessage）封装：把顶层窗口注册为 AppBar，
    // 停靠到屏幕顶/底边并让系统缩小工作区（其他窗口自动让开这条边）。
    // 这是 MiniMeters "stick" 的同一套机制（任务栏式驻留）。
    HWND y2kGetWindowHwnd (juce::Component* c) noexcept
    {
        if (c == nullptr) return nullptr;
        auto* peer = c->getPeer();
        return peer != nullptr ? reinterpret_cast<HWND> (peer->getNativeHandle()) : nullptr;
    }

    // 窗口所在显示器的物理工作区（px）。不含本窗口的 AppBar 占位 —— 必须在
    // ABM_REMOVE 之后调用，否则读到的仍是自己被压小后的工作区。
    bool y2kGetWindowWorkArea (HWND hwnd, RECT& out) noexcept
    {
        if (const HMONITOR mon = MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST))
        {
            MONITORINFO mi = {};
            mi.cbSize = sizeof (mi);
            if (GetMonitorInfo (mon, &mi) != FALSE)
            {
                out = mi.rcWork;
                return true;
            }
        }

        return SystemParametersInfo (SPI_GETWORKAREA, 0, &out, 0) != FALSE;
    }

    // 窗口当前物理矩形（px）。失败返回空矩形。
    juce::Rectangle<int> y2kGetWindowRectPx (juce::Component* window) noexcept
    {
        RECT r = {};
        const HWND hwnd = y2kGetWindowHwnd (window);
        if (hwnd == nullptr || ! GetWindowRect (hwnd, &r))
            return {};

        return { (int) r.left, (int) r.top, (int) (r.right - r.left), (int) (r.bottom - r.top) };
    }

    // 窗口当前物理高度（px），失败返回 0。
    int y2kGetWindowHeightPx (juce::Component* window) noexcept
    {
        return y2kGetWindowRectPx (window).getHeight();
    }

    // 把窗口搬到物理矩形（px）。
    bool y2kMoveWindowToRectPx (juce::Component* window, juce::Rectangle<int> r) noexcept
    {
        const HWND hwnd = y2kGetWindowHwnd (window);
        if (hwnd == nullptr || r.isEmpty())
            return false;

        return MoveWindow (hwnd, r.getX(), r.getY(), r.getWidth(), r.getHeight(), TRUE) != FALSE;
    }

    // 把窗口注册成 AppBar 并驻留到工作区顶/底边。
    // 关键点：占位矩形必须由「显示器工作区 + 窗口高度」显式构造，不能用窗口当前位置
    // 直接丢给 ABM_QUERYPOS —— QUERYPOS 只做贴边/避让修正，不会把矩形搬到该边：
    //   窗口若位于 y=250、高 254，SETPOS 会把 [0..504] 整段算成占位，
    //   于是屏幕顶部空出一条 ≈ 2 倍窗口高的区域。
    // outDockedRectPx 非空时写回最终提交的物理矩形（供 timer 做几何自检）。
    bool y2kAppBarDock (juce::Component* window, bool bottom,
                        juce::Rectangle<int>* outDockedRectPx = nullptr) noexcept
    {
        const HWND hwnd = y2kGetWindowHwnd (window);
        if (hwnd == nullptr) return false;

        // 1) 先撤销旧注册：让工作区立刻恢复成"不含本窗口"的完整区域，
        //    这样重新 dock（顶/底切换、高度变化）不会把上一次的占位累加进来。
        APPBARDATA abd = {};
        abd.cbSize = sizeof (abd);
        abd.hWnd   = hwnd;
        SHAppBarMessage (ABM_REMOVE, &abd);

        // 2) 占位厚度 = 窗口当前物理高度（stick 模式只锁宽度，高度随窗口走）
        const auto winRect = y2kGetWindowRectPx (window);
        const int thickness = winRect.getHeight();
        if (thickness <= 0)
            return false;

        RECT work = {};
        if (! y2kGetWindowWorkArea (hwnd, work))
            return false;

        if (SHAppBarMessage (ABM_NEW, &abd) == FALSE)
            return false;   // 注册失败（如 hwnd 无效），放弃

        // 3) 由工作区边 + 厚度构造目标矩形（宽 = 工作区宽，避开任务栏/其他 AppBar）
        abd.uEdge    = bottom ? ABE_BOTTOM : ABE_TOP;
        abd.rc.left  = work.left;
        abd.rc.right = work.right;

        if (bottom)
        {
            abd.rc.bottom = work.bottom;
            abd.rc.top    = abd.rc.bottom - thickness;
        }
        else
        {
            abd.rc.top    = work.top;
            abd.rc.bottom = abd.rc.top + thickness;
        }

        // 4) 让系统做避让修正（任务栏自动隐藏、同边已有其他 AppBar 等）
        SHAppBarMessage (ABM_QUERYPOS, &abd);

        // 5) ★ 按目标边重新写一次厚度：QUERYPOS 只保证贴边，不保证尺寸
        if (bottom)
            abd.rc.top = abd.rc.bottom - thickness;
        else
            abd.rc.bottom = abd.rc.top + thickness;

        // 6) 提交：系统据此缩小工作区（其他窗口让开这条边）
        SHAppBarMessage (ABM_SETPOS, &abd);

        // 7) 把窗口搬到最终矩形（宽度铺满工作区，高度 = 厚度）
        const juce::Rectangle<int> docked { (int) abd.rc.left, (int) abd.rc.top,
                                            (int) (abd.rc.right - abd.rc.left),
                                            (int) (abd.rc.bottom - abd.rc.top) };
        if (! y2kMoveWindowToRectPx (window, docked))
            return false;

        if (outDockedRectPx != nullptr)
            *outDockedRectPx = docked;

        return true;
    }

    void y2kAppBarUndock (juce::Component* window) noexcept
    {
        const HWND hwnd = y2kGetWindowHwnd (window);
        if (hwnd == nullptr) return;

        APPBARDATA abd = {};
        abd.cbSize = sizeof (abd);
        abd.hWnd   = hwnd;
        SHAppBarMessage (ABM_REMOVE, &abd);
    }
} // namespace
#endif

// ==========================================================
// 屏幕尺寸模拟（仅开发调试用）
//   环境变量 Y2KM_SIMULATE_SCREEN 格式 "宽x高"（如 "2560x1080" 模拟 21:9、
//   "1280x1024" 模拟 5:4）。设置且解析成功时，所有基于 userArea/totalArea
//   的布局计算改用该虚拟尺寸（原点固定 0,0）；未设置或非法时回退真实屏幕。
//   用于在本地模拟不同分辨率/比例的屏幕，验证布局预设的显示效果。
// ==========================================================
namespace
{
juce::Rectangle<int> resolveDisplayArea (const juce::Rectangle<int>& realArea)
{
    const auto raw = juce::SystemStats::getEnvironmentVariable ("Y2KM_SIMULATE_SCREEN", {});
    if (raw.isEmpty())
        return realArea;

    const int sep = raw.indexOfChar ('x');
    if (sep <= 0)
        return realArea;

    const int w = raw.substring (0, sep).trim().getIntValue();
    const int h = raw.substring (sep + 1).trim().getIntValue();
    if (w < 320 || h < 200)
        return realArea;

    return juce::Rectangle<int> (0, 0, w, h);
}

// UI 密度缩放的判据区域（跨平台差异）：
//   · Windows：必须用 totalArea —— AppBar/stick 驻留会真实压缩 userArea，
//     条高拉大时若用 userArea 会把"屏幕变矮了"的假象喂给分级表；
//   · macOS：保持 userArea —— 菜单栏 / Dock 减少的是真实可用空间，
//     且 mac 没有 stick/AppBar，用 totalArea 会改变部分机型的缩放档位。
juce::Rectangle<int> resolveUiScaleArea (const juce::Displays::Display& display) noexcept
{
   #if JUCE_WINDOWS
    return display.totalArea;
   #else
    return display.userArea;
   #endif
}
}  // namespace

// ==========================================================
// FloatingModuleWindow —— 模块弹出独立窗口实现
//   无系统标题栏，ModulePanel 自身的 PinkXP 标题栏作为窗口 chrome。
//   关闭按钮触发 dock 回 workspace。
// ==========================================================
Y2KmeterAudioProcessorEditor::FloatingModuleWindow::FloatingModuleWindow(
    const juce::String& name,
    ModulePanel& module,
    juce::Rectangle<int> savedBounds,
    std::function<void(ModulePanel&, juce::Rectangle<int>)> onDock,
    Y2KmeterAudioProcessorEditor* editor)
    : juce::DocumentWindow(name,
                            PinkXP::face,  // 背景色匹配模块底色
                            juce::DocumentWindow::minimiseButton,
                            false)
    , module_(module)
    , savedBounds_(savedBounds)
    , onDock_(std::move(onDock))
    , editor_(editor)
{
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::closeButtonPressed()
{
    cancelAsyncUpdates();  // 取消 resize 触发的 pending async update，防止引用已删除窗口
    if (onDock_)
        onDock_(module_, savedBounds_);
}

bool Y2KmeterAudioProcessorEditor::FloatingModuleWindow::keyPressed(
    const juce::KeyPress& key) {
  // 浮动 Milkdrop 有独立的 GLView 本地渲染器（Editor renderer 已被挂起），
  // 键盘事件必须直接路由到本窗口的模块，调用其公开 API 以操作本地渲染器。
  // 不能转发给 Editor::keyPressed，否则会误操作 workspace 中其它嵌入态 Milkdrop。
  if (auto* milk = dynamic_cast<MilkdropModule*>(&module_)) {
    if (key == juce::KeyPress::leftKey)  { milk->prevPreset();   return true; }
    if (key == juce::KeyPress::rightKey) { milk->nextPreset();   return true; }
    if (key == juce::KeyPress::spaceKey) { milk->randomPreset(); return true; }
  }
  return false;
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::resized()
{
    DocumentWindow::resized();
    if (auto* content = getContentComponent())
    {
        content->setBounds(getLocalBounds());
        // resize → 更新持久化 screenBounds；Milkdrop 还会借此触发 GL 重绘更新共享帧。
        if (editor_ != nullptr)
            triggerAsyncUpdate();
    }
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::handleAsyncUpdate()
{
    if (editor_ == nullptr) return;

    // 触发 GL 线程重绘（renderOpenGL → measureMax 重新扫描浮动窗口尺寸 →
    //   更新 FBO → 捕获新尺寸共享帧 → callAsync 通知浮动窗口 repaint）
    editor_->repaint();

    // 更新 workspace 浮动状态屏幕坐标（持久化用）
    if (editor_->workspace)
        editor_->workspace->updateFloatingStateScreenBounds(
            module_.getModuleId(), getScreenBounds());
}

juce::BorderSize<int> Y2KmeterAudioProcessorEditor::FloatingModuleWindow::getBorderThickness() const
{
    // 无边框设计：窗口内容（ModulePanel）紧贴窗口边缘
    return {};
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::setLayoutLocked(bool locked)
{
    module_.setFloatingLayoutLocked(locked);

    auto* cbc = getConstrainer();
    if (cbc == nullptr)
        return;

    if (locked)
    {
        if (!floatingLockLimitsValid_)
        {
            floatingLockMinW_ = cbc->getMinimumWidth();
            floatingLockMinH_ = cbc->getMinimumHeight();
            floatingLockMaxW_ = cbc->getMaximumWidth();
            floatingLockMaxH_ = cbc->getMaximumHeight();
            floatingLockLimitsValid_ = true;
        }

        const int w = juce::jmax(1, getWidth());
        const int h = juce::jmax(1, getHeight());
        setResizeLimits(w, h, w, h);
    }
    else if (floatingLockLimitsValid_)
    {
        setResizeLimits(floatingLockMinW_, floatingLockMinH_, floatingLockMaxW_, floatingLockMaxH_);
        floatingLockLimitsValid_ = false;
    }
}

juce::ValueTree Y2KmeterAudioProcessorEditor::FloatingModuleWindow::saveModuleSpecificState() const
{
    return module_.saveModuleSpecificState();
}

void Y2KmeterAudioProcessorEditor::syncFloatingWindowStatesToWorkspace()
{
    if (workspace == nullptr) return;

    for (auto& wnd : floatingWindows_)
    {
        if (wnd == nullptr) continue;
        auto* panel = dynamic_cast<ModulePanel*>(wnd->getContentComponent());
        if (panel == nullptr) continue;
        workspace->updateFloatingStateScreenBounds(panel->getModuleId(), wnd->getScreenBounds());
        workspace->updateFloatingStateModuleState(panel->getModuleId(), wnd->saveModuleSpecificState());
    }
}

void Y2KmeterAudioProcessorEditor::reloadFloatingModuleRecordsFromProcessor()
{
    if (workspace == nullptr) return;
    workspace->loadFloatingModulesFromXml (processor.getSavedFloatingModulesXml());
}

// 由 workspace 的脱离态记录重建浮动窗口。
//   · 用于启动恢复、退出 stick、导入 settings 后补齐窗口；
//   · 已存在同 moduleId 的窗口会跳过（不会重复实例化）；
//   · 先把"存活但不在记录里"的窗口补登记回去，保证记录与真实窗口一致。
void Y2KmeterAudioProcessorEditor::restoreFloatingWindowsFromWorkspace()
{
    if (isPluginHost || workspace == nullptr)
        return;

    // 先补登记"存活但不在记录里"的浮动窗口（stick 期间新脱离的模块不在备份里），
    // 否则运行期的 dock 回位信息会缺失。
    for (auto& wnd : floatingWindows_)
    {
        if (wnd == nullptr) continue;
        auto* c = dynamic_cast<ModulePanel*> (wnd->getContentComponent());
        if (c == nullptr || c->getModuleId().isEmpty()) continue;

        const auto& statesNow = workspace->getFloatingModuleStatesRaw();
        if (statesNow.find (c->getModuleId()) == statesNow.end())
            workspace->addFloatingState (c->getModuleId(), c->getModuleType(),
                                         c->getBounds(), wnd->getScreenBounds(),
                                         c->saveModuleSpecificState());
    }

    const auto floatingStates = workspace->getFloatingModuleStatesRaw();
    if (floatingStates.empty())
        return;

    // popOutModule 会修改 workspace 内部 map，不能边遍历引用边修改 → 使用上面的副本
    for (const auto& [id, state] : floatingStates)
    {
        // 已经有同 id 的浮动窗口（例如 stick 期间一直保持浮动的模块）→ 跳过，
        // 否则会重复实例化出第二个窗口。
        bool alreadyOpen = false;
        for (auto& wnd : floatingWindows_)
        {
            if (wnd == nullptr) continue;
            if (auto* c = dynamic_cast<ModulePanel*> (wnd->getContentComponent()))
                if (c->getModuleId() == id) { alreadyOpen = true; break; }
        }
        if (alreadyOpen)
            continue;

        auto panel = createModule (state.type);
        if (panel == nullptr)
            continue;

        panel->setModuleId (id);
        if (state.moduleState.isValid())
            panel->restoreModuleSpecificState (state.moduleState);
        panel->setBounds (state.bounds);
        auto* raw = panel.get();

        // addModule 接管所有权；popOutModule 再把它交给 Editor 创建浮动窗口
        workspace->addModule (std::move (panel), false);
        workspace->popOutModule (*raw);

        // popOutModule 会用当前状态重写脱离记录，这里写回备份里的业务状态
        if (state.moduleState.isValid())
            workspace->updateFloatingStateModuleState (id, state.moduleState);

        if (! state.screenBounds.isEmpty() && ! floatingWindows_.empty())
        {
            auto* wnd = floatingWindows_.back().get();
            wnd->setLayoutLocked (false);
            wnd->setBounds (state.screenBounds);
            wnd->setLayoutLocked (layoutLocked);
        }

        if (! floatingWindows_.empty())
            workspace->updateFloatingStateScreenBounds (id, floatingWindows_.back()->getScreenBounds());
    }
}



// ==========================================================
// FpsFrameListener——嵌套实现（封装在 .cpp 以绕开 AnalyserHub 完整头
//   在 PluginEditor.h 里暴露时导致的 MSVC include guard 串扰问题）
// ==========================================================
class Y2KmeterAudioProcessorEditor::FpsFrameListener : public AnalyserHub::FrameListener
{
public:
    explicit FpsFrameListener (Y2KmeterAudioProcessorEditor& ownerRef) : owner (ownerRef) {}

    void onFrame (const AnalyserHub::FrameSnapshot& /*frame*/) override
    {
        // UI 线程回调：仅原子计数器 +1（轻量、不做其它处理）
        owner.frameCounter.fetch_add (1, std::memory_order_relaxed);
    }

private:
    Y2KmeterAudioProcessorEditor& owner;
};

// ==========================================================
// FramePacingFallbackTimer —— vblank 被吞时的分发兜底（v2.9.1）
//   JUCE Windows 的 VBlankThread 用 AsyncUpdater 把 vblank 事件派发到消息线程，
//   消息线程没处理完上一个回调时新事件会被直接丢弃（flagPaintPending 合并语义）；
//   GL 渲染线程持锁执行 renderOpenGL + swapBuffers（等 vsync）又会周期性阻塞
//   消息线程，形成负反馈：分发越慢 → vblank 事件丢得越多 → 帧率锁死。
//   兜底 Timer 按"距上次分发 ≥ 1 个目标间隔"补帧，与 vblank 入口共用
//   lastDispatchWallMs 去重：vblank 正常时它永远不触发，只在 vblank 失效时接管。
// ==========================================================
class Y2KmeterAudioProcessorEditor::FramePacingFallbackTimer : public juce::Timer
{
public:
    explicit FramePacingFallbackTimer (Y2KmeterAudioProcessorEditor& ownerRef) : owner (ownerRef) {}

    void timerCallback() override
    {
        owner.tryDispatchFromFallbackTimer (juce::Time::getMillisecondCounterHiRes());
    }

private:
    Y2KmeterAudioProcessorEditor& owner;
};

// ==========================================================
// ChromeHiddenOverlay —— chrome 隐藏态的"纯视觉"浮层
//
// 职责：
//   · 底图上打印软件名 / 版本号 / 官网链接（低对比度）
//   · 右上角半透明关闭按钮（视觉）
//
// 关键设计：
//   · 整体宽度与 Editor 同宽、高度 = titleBarHeight。
//   · z-order 最底层（在 workspace 之下）→ 模块可以自然遮挡 overlay 的文字和按钮。
//   · 本组件不处理任何鼠标事件：setInterceptsMouseClicks(false, false)。
//     顶部按钮的点击由 Editor 在 mouseMove/Down/Up 中统一处理（workspace 在对应
//     矩形区域做 hit-test 挖洞，让事件冒泡到 Editor）。
//   · 视觉状态（closeButtonHovered / titleTextHovered 等）由 Editor 在鼠标事件中
//     通过 setCloseButtonHovered / setTitleTextHovered 下发。
// ==========================================================
class Y2KmeterAudioProcessorEditor::ChromeHiddenOverlay : public juce::Component
{
public:
    explicit ChromeHiddenOverlay (Y2KmeterAudioProcessorEditor& ownerRef)
        : owner (ownerRef)
    {
        // 纯视觉层：完全不拦截鼠标事件（事件全部透传到下方的 workspace / Editor）
        setInterceptsMouseClicks (false, false);

        // 预先计算标题文字像素宽度，这样 Editor 首次调用 updateWorkspaceHitTestHoles
        //   就能拿到精确的文字矩形，而不用等第一次 paint 写入。
        //   （与 paint() 里完全相同的字体/文本/间距计算）
        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);
        const int nameW    = nameFont.getStringWidth ("Y2Kmeter");
        const int versionW = versionFont.getStringWidth ("v2.9.1");
        const int urlW     = urlFont.getStringWidth ("iisaacbeats.cn");
        constexpr int gap1 = 6;
        constexpr int gap2 = 10;
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;
    }

    // 由 Editor 更新视觉状态（hover/pressed）
    void setCloseButtonHovered (bool h)
    {
        if (h != closeButtonHovered) { closeButtonHovered = h; repaint (getFloatingCloseButtonRect()); }
    }
    void setCloseButtonPressed (bool p)
    {
        if (p != closeButtonPressed) { closeButtonPressed = p; repaint (getFloatingCloseButtonRect()); }
    }
    void setTitleTextHovered (bool h)
    {
        if (h != titleTextHovered) { titleTextHovered = h; repaint(); }
    }

    // 几何（Editor 需要用来做 hit-test 与冒泡处理）
    juce::Rectangle<int> getFloatingCloseButtonRect() const
    {
        constexpr int margin = 4;
        const int size = PinkXP::ui(18); // 与 Editor::closeButtonSize 保持一致
        return { getWidth() - margin - size, margin, size, size };
    }

    // 标题文字整体命中矩形（宽度取实际像素宽度，由 paint 写入）
    juce::Rectangle<int> getTitleTextHitRect() const
    {
        const int x = 28;
        const int w = juce::jmax (0, cachedTitleTextW);
        return { x, 0, w, getHeight() };
    }

    void paint (juce::Graphics& g) override
    {
        // ------- 1) 顶部抬头文字：软件名 + 版本号 + 官网（低对比度，贴在底图上）-------
        const juce::String nameText    = "Y2Kmeter";
        const juce::String versionText = "v2.9.1";
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont(12.0f, juce::Font::plain);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW    = nameFont.getStringWidth ("Y2Kmeter");
        const int versionW = versionFont.getStringWidth (versionText);
        const int urlW     = urlFont.getStringWidth (urlText);

        constexpr int gap1 = 6;
        constexpr int gap2 = 10;

        const int x0 = 28;
        const int y  = 0;
        const int h  = getHeight();

        // chrome 隐藏态：文字淡淡贴在底图上，hover 时略亮并加下划线
        const float textAlpha = titleTextHovered ? 0.95f : 0.55f;

        // 文字描边（1px 阴影），帮助文字在任何底图上都可读
        g.setColour (juce::Colours::white.withAlpha (textAlpha * 0.6f));
        g.setFont (nameFont);
        g.drawText (nameText, x0 + 1, y + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1 + 1, y + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2 + 1, y + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（墨色）
        g.setColour (PinkXP::ink.withAlpha (textAlpha));
        g.setFont (nameFont);
        g.drawText (nameText, x0, y, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1, y, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2, y, urlW, h, juce::Justification::centredLeft, false);

        if (titleTextHovered)
        {
            const int lineY = y + h - 3;
            const int totalW = nameW + gap1 + versionW + gap2 + urlW;
            g.setColour (PinkXP::ink.withAlpha (0.95f));
            g.fillRect (x0, lineY, totalW, 1);
        }

        // 缓存实际绘制宽度，供 Editor 做精确命中测试
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;

        // ------- 2) 右上角浮动关闭按钮（15% / 100% 两档透明度）-------
        juce::Graphics::ScopedSaveState save (g);
        g.setOpacity (closeButtonHovered ? 1.0f : 0.15f);

        auto cb = getFloatingCloseButtonRect();
        if (closeButtonPressed)
            PinkXP::drawPressed (g, cb, PinkXP::pink100);
        else
            PinkXP::drawRaised  (g, cb, closeButtonHovered ? PinkXP::pink200 : PinkXP::btnFace);

        g.setColour (PinkXP::ink);
        g.setFont   (PinkXP::getFont (12.0f, juce::Font::bold));
        auto cbText = cb;
        cbText.translate (-1, -1);
        if (closeButtonPressed) cbText.translate (1, 1);
        g.drawText ("x", cbText, juce::Justification::centred, false);
    }

    int getCachedTitleTextWidth() const noexcept { return cachedTitleTextW; }

private:
    Y2KmeterAudioProcessorEditor& owner;
    bool closeButtonHovered = false;
    bool closeButtonPressed = false;
    bool titleTextHovered   = false;
    mutable int cachedTitleTextW = 0;
};

// ==========================================================
// TutorialOverlay —— 新手引导全屏覆盖层
//
// 职责：
//   · 半透明遮罩 + "聚光灯"镂空区域（高亮引导目标）
//   · Y2K 风格气泡对话框提示（右上角带 × 关闭按钮）
//   · STEP 1：右键画布添加 VirtuPet
//   · STEP 2：播放音频孵化宠物蛋
//   · 点击 × 按钮 → 弹出二次确认对话框（"跳过引导不可撤销"）
//
// 关键设计：
//   · 覆盖整个 Editor，z-order 最高（在 workspace 之上）
//   · setInterceptsMouseClicks (true, true)：拦截点击
//   · 仅右键点击聚光灯区域触发回调；左键/其他区域忽略
//   · 视觉风格参考 VirtuPetConfirmOverlay（PinkXP 凸起边框对话框）
// ==========================================================
class Y2KmeterAudioProcessorEditor::TutorialOverlay : public juce::Component
{
public:
    TutorialOverlay()
    {
        setOpaque (false);
    }

    void showStep1 (const juce::Rectangle<int>& canvasAreaInEditor)
    {
        currentStep = Step::step1;
        highlightArea = canvasAreaInEditor;
        confirmSkipVisible = false;
        menuIsOpen = false;
        setAlwaysOnTop (true);
        setInterceptsMouseClicks (true, true);
        setVisible (true);
        repaint();
    }

    // 右键菜单已弹出：保持遮罩和聚光灯不变，只更换气泡引导文案
    void showStep1MenuOpened()
    {
        jassert (currentStep == Step::step1); // 仅 STEP 1 有效
        menuIsOpen = true;
        repaint();
    }

    void showStep2 (const juce::Rectangle<int>& petAreaInEditor)
    {
        currentStep = Step::step2;
        highlightArea = petAreaInEditor.expanded (12);
        confirmSkipVisible = false;
        menuIsOpen = false;
        setAlwaysOnTop (true);
        setInterceptsMouseClicks (true, true);
        setVisible (true);
        repaint();
    }

    void hide()
    {
        currentStep = Step::hidden;
        highlightArea = {};
        bubbleHovered = false;
        closeBtnHovered = false;
        closeBtnPressed = false;
        confirmSkipVisible = false;
        confirmBtnHovered = false;
        menuIsOpen = false;
        setVisible (false);
        setAlwaysOnTop (false);
        setInterceptsMouseClicks (false, false);
    }

    void paint (juce::Graphics& g) override
    {
        if (currentStep == Step::hidden) return;

        drawSpotlight (g);
        drawSpeechBubble (g);

        // 二次确认弹窗（在气泡之上）
        if (confirmSkipVisible)
            drawConfirmSkipDialog (g);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (currentStep == Step::hidden) return;
        const auto pos = e.getPosition();

        // 二次确认弹窗可见 → 只处理确认/取消
        if (confirmSkipVisible)
        {
            if (e.mods.isLeftButtonDown())
            {
                const auto dlg = getConfirmDialogBounds();
                if (! dlg.contains (pos)) { confirmSkipVisible = false; repaint(); return; }
                if (getConfirmBtnBounds (1).contains (pos)) // Skip 按钮
                {
                    confirmSkipVisible = false;
                    hide();
                    if (onSkipRequested) onSkipRequested();
                    return;
                }
                // Cancel 按钮或 dialog 内非按钮区 → 关闭弹窗
                confirmSkipVisible = false;
                repaint();
                return;
            }
        }

        // 左键点击气泡 × 关闭按钮
        if (e.mods.isLeftButtonDown())
        {
            const auto closeBtn = getCloseButtonBounds();
            if (closeBtn.contains (pos))
            {
                confirmSkipVisible = true;
                confirmBtnHovered = false;
                repaint();
                return;
            }
        }

        // STEP 1：右键聚光灯区域 → 触发添加
        if (currentStep == Step::step1 && e.mods.isRightButtonDown())
        {
            if (highlightArea.contains (pos) && onRightClickHighlight)
                onRightClickHighlight (pos);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (currentStep == Step::hidden) return;
        const auto pos = e.getPosition();

        if (confirmSkipVisible)
        {
            const auto dlg = getConfirmDialogBounds();
            const bool btnHover = getConfirmBtnBounds (1).contains (pos)
                               || getConfirmBtnBounds (0).contains (pos);
            const bool dlgHover = dlg.contains (pos);
            if (btnHover != confirmBtnHovered || dlgHover != bubbleHovered)
            {
                confirmBtnHovered = btnHover;
                bubbleHovered = dlgHover;
                repaint (dlg);
            }
            return;
        }

        // 关闭按钮 hover
        const auto closeBtn = getCloseButtonBounds();
        const bool cbHover = closeBtn.contains (pos);
        if (cbHover != closeBtnHovered)
        {
            closeBtnHovered = cbHover;
            repaint (closeBtn.expanded (2));
        }

        // 气泡整体 hover
        const auto bubble = getBubbleBounds();
        const bool hover = bubble.contains (pos);
        if (hover != bubbleHovered)
        {
            bubbleHovered = hover;
            repaint (bubble);
        }
    }

    // 用户右键点击了高亮区域 → 回调（参数：相对本组件的点击位置）
    std::function<void(juce::Point<int> clickPos)> onRightClickHighlight;
    // 用户确认跳过新手引导 → 回调
    std::function<void()> onSkipRequested;

    // 返回当前聚光灯区域（Editor 坐标系），供外部恢复 STEP1 时复用
    juce::Rectangle<int> getHighlightArea() const { return highlightArea; }

private:
    enum class Step { hidden, step1, step2 };
    Step currentStep = Step::hidden;
    juce::Rectangle<int> highlightArea;
    bool bubbleHovered = false;
    bool closeBtnHovered = false;
    bool closeBtnPressed = false;
    bool confirmSkipVisible = false;
    bool confirmBtnHovered = false;
    bool menuIsOpen = false;     // STEP 1 右键菜单已弹出，此时切换气泡文案

    // 气泡右上角 × 按钮（相对本组件坐标，基于 getBubbleBounds 偏移）
    juce::Rectangle<int> getCloseButtonBounds() const
    {
        const auto bubble = getBubbleBounds();
        constexpr int btnSize = 18;
        constexpr int margin  = 4;
        return { bubble.getRight() - margin - btnSize,
                 bubble.getY() + margin,
                 btnSize, btnSize };
    }

    // 二次确认弹窗区域（居中于 Editor）
    juce::Rectangle<int> getConfirmDialogBounds() const
    {
        constexpr int dlgW = 260;
        constexpr int dlgH = 100;
        const int cx = getWidth()  / 2;
        const int cy = getHeight() / 2;
        return { cx - dlgW / 2, cy - dlgH / 2, dlgW, dlgH };
    }

    // 二次确认弹窗按钮（idx: 0=Cancel, 1=Skip）
    juce::Rectangle<int> getConfirmBtnBounds (int idx) const
    {
        const auto dlg = getConfirmDialogBounds();
        auto inner = dlg.reduced (6);
        // 跳过标题行 18 + 分隔线(1) 后的正文区
        inner.removeFromTop (juce::jmin (18, inner.getHeight() - 30));

        const int btnH = 20;
        auto btnRow = juce::Rectangle<int> (inner.getX(), inner.getBottom() - btnH,
                                             inner.getWidth(), btnH);
        const int btnW = juce::jmin (70, (btnRow.getWidth() - 12) / 2);
        const int gap = 8;

        if (idx == 0) // Cancel（左）
            return { btnRow.getCentreX() - btnW - gap / 2, btnRow.getY(), btnW, btnH };
        else          // Skip（右）
            return { btnRow.getCentreX() + gap / 2, btnRow.getY(), btnW, btnH };
    }

    void drawConfirmSkipDialog (juce::Graphics& g)
    {
        const auto dlg = getConfirmDialogBounds();

        // 半透明底衬
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (getLocalBounds());

        // 对话框外壳
        PinkXP::drawRaised (g, dlg, PinkXP::btnFace);

        auto inner = dlg.reduced (6);
        if (inner.getHeight() < 50) return;

        // 标题行
        auto titleRow = inner.removeFromTop (juce::jmin (18, inner.getHeight() - 30));
        g.setColour (PinkXP::sel);
        g.fillRect (titleRow);
        g.setColour (PinkXP::dark);
        g.fillRect (titleRow.getX(), titleRow.getBottom(), titleRow.getWidth(), 1);
        g.setColour (PinkXP::selInk);
        g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
        g.drawText ("SKIP TUTORIAL?", titleRow.reduced (3, 1), juce::Justification::centredLeft, false);

        // 正文
        auto body = inner.reduced (2, 2);
        auto textArea = body.removeFromTop (body.getHeight() - 24);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f));
        g.drawFittedText ("U will miss the fun :(\nthis can't be undone~",
                          textArea, juce::Justification::centred, 2);

        // Cancel 按钮（始终平面，不单独高亮）
        auto cancelBounds = getConfirmBtnBounds (0);
        PinkXP::drawRaised (g, cancelBounds, PinkXP::btnFace);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText ("Cancel", cancelBounds, juce::Justification::centred, false);

        // Skip 按钮（粉红突出，始终醒目）
        auto skipBounds = getConfirmBtnBounds (1);
        PinkXP::drawRaised (g, skipBounds, PinkXP::pink200);
        g.setColour (PinkXP::pink700);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText ("Skip", skipBounds, juce::Justification::centred, false);
    }

    void drawSpotlight (juce::Graphics& g)
    {
        const auto fullArea = getLocalBounds();

        // 半透明黑色遮罩 —— 镂空聚光灯区域（VirtuPet 模块保持可见）
        {
            juce::Graphics::ScopedSaveState save (g);
            g.excludeClipRegion (highlightArea);
            g.setColour (juce::Colours::black.withAlpha (0.65f));
            g.fillRect (fullArea);
        }

        // 聚光灯边框：虚线闪烁效果（粉色，暗示交互区域）
        {
            g.setColour (PinkXP::pink600.withAlpha (0.8f));
            const float dash[] = { 4.0f, 4.0f };
            g.drawDashedLine (juce::Line<float> (highlightArea.getTopLeft().toFloat(),
                              highlightArea.getTopRight().toFloat()), dash, 2);
            g.drawDashedLine (juce::Line<float> (highlightArea.getTopRight().toFloat(),
                              highlightArea.getBottomRight().toFloat()), dash, 2);
            g.drawDashedLine (juce::Line<float> (highlightArea.getBottomRight().toFloat(),
                              highlightArea.getBottomLeft().toFloat()), dash, 2);
            g.drawDashedLine (juce::Line<float> (highlightArea.getBottomLeft().toFloat(),
                              highlightArea.getTopLeft().toFloat()), dash, 2);
        }
    }

    // 气泡位置：
    //   STEP 1：优先聚光灯上方 → 下方 → 居中（canvas 占满整个区域）
    //   STEP 2：放在 VirtuPet 模块的左侧或右侧（取决于模块在 Editor 中的位置），
    //           绝不覆盖模块本身
    juce::Rectangle<int> getBubbleBounds() const
    {
        constexpr int bubbleW = 280;
        constexpr int bubbleH = 76;

        if (currentStep == Step::step2)
        {
            const int editorW = getWidth();

            // 模块中心 X > Editor 中心 → 模块偏右 → 气泡放在模块左侧
            // 模块中心 X < Editor 中心 → 模块偏左 → 气泡放在模块右侧
            const bool petOnRight = highlightArea.getCentreX() > editorW / 2;

            int bubbleX;
            if (petOnRight)
                bubbleX = highlightArea.getX() - bubbleW - 12;  // 气泡右缘贴模块左缘
            else
                bubbleX = highlightArea.getRight() + 12;         // 气泡左缘贴模块右缘

            // 垂直居中于模块
            int bubbleY = highlightArea.getCentreY() - bubbleH / 2;

            // 夹紧到 Editor 可见范围
            bubbleX = juce::jlimit (8, juce::jmax (8, editorW - bubbleW - 8), bubbleX);
            bubbleY = juce::jlimit (4, juce::jmax (4, getHeight() - bubbleH - 4), bubbleY);

            return { bubbleX, bubbleY, bubbleW, bubbleH };
        }

        // --- STEP 1: 原有上下避让逻辑 ---
        const int cx = highlightArea.getCentreX();
        int bubbleX = cx - bubbleW / 2;

        // 尝试上方
        int bubbleY = highlightArea.getY() - bubbleH - 14;

        // 上方不够 → 尝试下方
        if (bubbleY < 10)
            bubbleY = highlightArea.getBottom() + 14;

        // 下方也超出 Editor → 直接居中放
        if (bubbleY + bubbleH > getHeight() - 8)
            bubbleY = (getHeight() - bubbleH) / 2;

        // 保底不超出顶部
        bubbleY = juce::jmax (4, bubbleY);

        // 不超出左右边界
        const int rightLimit = getWidth() - 16;
        bubbleX = juce::jlimit (8, juce::jmax (8, rightLimit - bubbleW), bubbleX);

        return { bubbleX, bubbleY, bubbleW, bubbleH };
    }

    void drawSpeechBubble (juce::Graphics& g)
    {
        if (currentStep == Step::hidden) return;

        auto bubble = getBubbleBounds();

        // 外壳
        PinkXP::drawRaised (g, bubble, bubbleHovered ? PinkXP::pink200 : PinkXP::btnFace);

        auto inner = bubble.reduced (6);
        if (inner.getHeight() < 40) return;

        // 标题行
        auto titleRow = inner.removeFromTop (juce::jmin (18, inner.getHeight() - 24));
        g.setColour (PinkXP::sel);
        g.fillRect (titleRow);
        g.setColour (PinkXP::dark);
        g.fillRect (titleRow.getX(), titleRow.getBottom(), titleRow.getWidth(), 1);

        // 标题
        g.setColour (PinkXP::selInk);
        g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
        const juce::String title = (currentStep == Step::step1)
            ? (menuIsOpen ? "NOW CHOOSE IT!" : "WELCOME 2 Y2KMETER!")
            : "ALMOST THERE!";
        g.drawText (title, titleRow.reduced (3, 1), juce::Justification::centredLeft, false);

        // 正文
        auto body = inner.reduced (2, 2);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f));
        const juce::String text = (currentStep == Step::step1)
            ? (menuIsOpen
                ? "Click 'VirtuPet' in\nthe menu to add ur pet! <3"
                : "Right-click the canvas to\nadd ur VirtuPet pet! <3")
            : "Play some audio to\nhatch the egg! :3";
        g.drawFittedText (text, body, juce::Justification::centred, 2);

        // 右上角 × 关闭按钮（参考 ChromeHiddenOverlay 的浮动关闭按钮样式）
        {
            auto cb = getCloseButtonBounds();
            juce::Graphics::ScopedSaveState save (g);
            g.setOpacity (closeBtnHovered ? 1.0f : 0.55f);

            if (closeBtnPressed)
                PinkXP::drawPressed (g, cb, PinkXP::pink100);
            else
                PinkXP::drawRaised  (g, cb, closeBtnHovered ? PinkXP::pink200 : PinkXP::btnFace);

            g.setColour (PinkXP::ink);
            g.setFont   (PinkXP::getFont (11.0f, juce::Font::bold));
            auto cbText = cb;
            cbText.translate (-1, -1);
            if (closeBtnPressed) cbText.translate (1, 1);
            g.drawText ("x", cbText, juce::Justification::centred, false);
        }
    }
};

// ==========================================================
// Y2KmeterAudioProcessorEditor —— 多模块框架外壳
// 窗口：960 × 640（Pink XP 桌面 + 凸起主窗口 + 粉色标题栏）
// 中部放置 ModuleWorkspace，所有分析模块都作为子模块存在
// ==========================================================

juce::Font Y2KmeterAudioProcessorEditor::getCustomFont(float height, int styleFlags)
{
    return PinkXP::getFont(height, styleFlags);
}

Y2KmeterAudioProcessorEditor::Y2KmeterAudioProcessorEditor(Y2KmeterAudioProcessor& p)
    : juce::AudioProcessorEditor(&p),
      processor(p),
      // 根据 AudioProcessor::wrapperType 一次性判定是否插件宿主模式。
      // JUCE 在 AudioProcessor 构造时会把本次加载的 wrapper 类型填好
      // （VST3 / AU / AAX / LV2 / Standalone / Undefined 等），对于
      // Standalone 以外的一切情况我们都视为"插件宿主模式"。
      isPluginHost (p.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
{
    Y2K_LOG ("[boot] PluginEditor ctor begin");

    // 0) 异步加载自定义字体。
    //   VST3/AU 宿主在创建插件 Editor 时，Direct2D/DirectWrite 基础
    //   设施可能尚未初始化。同步调用 createSystemTypefaceFor() 会触发
    //   SharedResourcePointer<Direct2DFactories> 构造，若 DWrite 工厂
    //   未就绪则访问违例崩溃。
    //   改为 callAsync 延迟加载：消息循环就绪后 DWrite 已可用。
    //   首帧使用系统字体渲染，异步回调完成后再切换为 Silkscreen。
    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<juce::Component>(this)] {
        if (safeThis == nullptr) return;
        auto* self = dynamic_cast<Y2KmeterAudioProcessorEditor*>(safeThis.getComponent());
        if (self == nullptr) return;
        self->customTypeface = PinkXP::loadActiveTypeface();
        PinkXP::initCustomTypeface(self->customTypeface);
        self->repaint();
    });

    // Logo 加载不受 DWrite 影响，保持同步加载。
    // 防御性 try-catch：3000×3000 RGBA 解码需 ~36MB 连续内存，
    // 在内存碎片化或堆损坏场景可能失败，此时降级为空 Image 并记日志。
    try {
      logoImage = juce::ImageFileFormat::loadFrom(BinaryData::logo_png,
                                                   (size_t)BinaryData::logo_pngSize);
    } catch (const std::exception& e) {
      DBG("!! Logo load failed (exception): " << e.what());
      logoImage = juce::Image();
    } catch (...) {
      DBG("!! Logo load failed (unknown exception)");
      logoImage = juce::Image();
    }
    if (logoImage.isNull()) {
      DBG("!! Logo image is null after load: size="
          << BinaryData::logo_pngSize << " bytes");
    }

    // 从 Processor 读回上次保存的 Milkdrop 整体视觉状态（染色 + 效果），
    // 实现关闭→重开复原。
    milkdrop_visual_state_ = processor.getSavedMilkdropVisualState();
    // 简单波形样式覆盖（wave_mode/x/y/r/g/b/a/...）
    milkdrop_wave_state_ = processor.getSavedMilkdropWaveState();
    // 收藏库切换状态（浏览内置库还是收藏库）
    milkdrop_use_like_library_.store(processor.getSavedMilkdropUseLikeLibrary());

    initLookAndFeel();

#if JUCE_WINDOWS
    // v2.8.5：读取上次退出时的 stick 会话（仅 Standalone）。
    //   此处只记录标志；真正 AppBar 驻留要等窗口上屏有 HWND 之后
    //   （timerCallback → restoreSavedStickSession）。
    pendingStickRestore = ! isPluginHost && processor.isSavedStickActive();
    pendingStickBottom  = pendingStickRestore && processor.isSavedStickBottom();
#endif

    // 0.5) 低分辨率适配：先按主显示器预估 UI 密度缩放（Windows 用 totalArea /
    //   mac 用 userArea，见 resolveUiScaleArea），让紧接着的
    //   setSize()/loadInitialModules() 就用上正确的缩放 chrome 布局。
    //   （此时 Editor 尚未挂到顶层窗口，无法确定最终所在显示器，故用主显示器
    //    先给一个合理初值；visibilityChanged 会按实际窗口所在显示器再校准一次。）
    if (auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        PinkXP::setUiScale (PinkXP::uiScaleForDisplay (resolveUiScaleArea (*primary)));

    // 1) 先创建 ChromeHiddenOverlay 并以 invisible 状态加入 —— 让它成为"最底层"子组件
    //    （后续 workspace / 其它组件 addAndMakeVisible 时都会排到它之上）。
    //    这样 chrome 隐藏态下，overlay 的抬头文字和浮动关闭按钮会被模块自然遮挡。
    chromeHiddenOverlay = std::make_unique<ChromeHiddenOverlay> (*this);
    addChildComponent (*chromeHiddenOverlay);

    // 2) 创建 workspace 并挂到编辑器上（z-order 高于 overlay）
    workspace = std::make_unique<ModuleWorkspace>();
    workspace->setModuleFactory([this](ModuleType t) -> std::unique_ptr<ModulePanel>
    {
        return createModule(t);
    });
    workspace->setAvailableModuleTypes({
        ModuleType::loudness,
        ModuleType::lufsRealtime,
        ModuleType::truePeak,

        // 模拟 VU 指针表（复用 Loudness 路的 RMS L/R，后端零新增计算）
        ModuleType::vuMeter,

        ModuleType::oscilloscope,
        ModuleType::oscilloscopeWave,

        ModuleType::spectrum,
        ModuleType::eq,

        ModuleType::phase,
        ModuleType::phaseCorrelation,
        ModuleType::phaseBalance,

        ModuleType::dynamics,
        ModuleType::dynamicsMeters,
        ModuleType::dynamicsDr,
        ModuleType::dynamicsCrest,

        // 持续滚动瀑布波形（复用 Oscilloscope 原始样本，后端零新增计算）
        ModuleType::waveform,

        // 实时频谱瀑布图（复用 Spectrum 路的 FFT 幅度，后端零新增计算）
        ModuleType::spectrogram,

        // 3D 频谱瀑布图（45° 俯视三维曲面效果）
        ModuleType::spectrogram3d,

        // 声相指示（半圆雷达，复用 Oscilloscope 立体声样本）
        ModuleType::stereoField,

        // 独立小宠物模块（右键/双击空白区添加）
        ModuleType::virtuPet,

        // Milkdrop WebGL 可视化模块（WebView 嵌入 Butterchurn 引擎）
        ModuleType::milkdrop
    });

#if JUCE_WINDOWS
    // 插件宿主模式屏蔽 stick 按钮：SHAppBarMessage 会把「宿主顶层窗口」注册成
    //   AppBar 并压缩桌面工作区（等于劫持宿主），且窗口几何完全归宿主管理，
    //   无法保证条状驻留。该功能只在 Standalone 暴露。
    workspace->setStickUiVisible (! isPluginHost);
#endif

    addAndMakeVisible(*workspace);

    // 2) 先给编辑器设尺寸 —— 触发 resized() 让 workspace 拿到实际 bounds
    //    这样后续 addModuleByType 走 findNextSlot 时 canvas 不再是 0x0
    //
    //   尺寸上限分模式：
    //     · Standalone：1600×1100 —— 默认预设下的合理上限；切换到"横向铺满"
    //       类预设时 applyLayoutPreset 会临时把上限抬到屏幕宽度，预设回 1 时
    //       会再夹回 1600×1100，避免用户手动拖得过大错位。
    //     · VST3 / AU 插件宿主：8192×8192 —— 远大于常规显示器分辨率。插件模式
    //       下布局预设下拉被隐藏（不会再有地方动态抬高上限），如果仍用 1600×1100
    //       会被 JUCE 的 ComponentBoundsConstrainer 死死夹住；用户在 DAW 里
    //       拉宽窗口到 1600 就拉不动了。给一个足够大的上限让宿主自由 resize。
    //
    //   尺寸下限同样分模式：
    //     · Standalone：640×420 —— 保证伪标题栏 + 底部 toolbar + 至少一行模块
    //       能完整显示，避免用户误缩到按钮挤压错位。
    //     · VST3 / AU 插件宿主：320×240 —— 插件模式下不画伪标题栏（chrome 由宿主
    //       提供），底部 toolbar 里大量 UI 也被隐藏（Source 下拉、布局预设下拉等），
    //       实际需要的最小可用宽度远小于 Standalone；若仍沿用 640×420，用户在 DAW
    //       里就会"缩到一半缩不动了"。给到 320×240 让宿主充分自由。
    setResizable(true, true);
    if (isPluginHost)
        setResizeLimits(50, 50, 8192, 8192);
    else
        setResizeLimits(640, 420, 1600, 1100);

    // ------------------------------------------------------------------
    // v1.8.3：从 Processor state 恢复"布局锁定态"。
    //   · 初始化时已确保 workspace / 顶层窗口已就位（workspace 上面刚 make_unique
    //     + addAndMakeVisible；顶层窗口则交给 setResizable 自己取）。
    //   · initial=true 避免回写 processor（保持与已保存一致即可）；
    //   · 若 processor 未保存过（缺省 false），则仅刷新一下按钮视觉即 no-op，
    //     applyLayoutLocked 内部会将 setResizable(true,true) 重推一次——与上面刚刚手动
    //     setResizable(true,true) 同步，安全。
    applyLayoutLocked (processor.getLayoutLocked(), /*initial*/ true);
    // ------------------------------------------------------------------

    // 插件模式（VST3/AU）：若 host state 里保存过上次 Editor 关闭时的窗口尺寸，
    //   优先用它恢复，实现"每次重开窗口保持上次大小"（尤其是 FL Studio Windows 版
    //   本身不会记住 VST3 窗口尺寸）。
    //   Standalone 模式由 PropertiesFile 体系记忆位置+尺寸，不走这里。
    //
    //   下限校验与 resized() 的回写下限 (400×300) 保持一致：若历史 state 里
    //   残留了异常小尺寸（例如早期版本曾回存过 FL Studio 关闭子窗口时的
    //   ~175×85 中间态），直接视为无效并回退到默认 960×640，不要盲目 setSize
    //   让用户看到一个几乎看不见的小窗口。
    constexpr int kMinRestoreW = 400;
    constexpr int kMinRestoreH = 300;
    const int savedW = processor.getSavedEditorWidth();
    const int savedH = processor.getSavedEditorHeight();

    if (isPluginHost
        && savedW >= kMinRestoreW
        && savedH >= kMinRestoreH)
    {
        const int maxW = 8192, maxH = 8192;
        const int w = juce::jlimit (kMinRestoreW, maxW, savedW);
        const int h = juce::jlimit (kMinRestoreH, maxH, savedH);
        setSize (w, h);
    }
    else
    {
        setSize(960, 640);
    }

    // 3) 再装载默认/已保存模块
#ifndef Y2KMETER_MILKDROP_ONLY
    loadInitialModules();
#endif

    // 4) 用户布局变更 → 立即写回 Processor state 的"当前活动槽"
    //    （v2.8.5：stick 期间写 stick 槽，绝不污染非 stick 布局）
    workspace->onLayoutChanged = [this]()
    {
        persistCurrentLayoutToProcessor();
    };

    // 4.0) 模块弹出请求 → 创建浮动窗口（仅 Standalone 模式）
    workspace->onModulePopOutRequested = [this](ModulePanel* panel,
                                                 juce::Rectangle<int> savedBounds)
    {
        if (panel == nullptr) return;

        // 设置模块为浮动态前先把当前业务状态回灌到模块自身。
        // Milkdrop 嵌入态的当前预设在 Editor renderer 中，浮动态会新建本地 renderer；
        // 若不先写入 restored state，新 renderer 会从预设 0 启动。
        auto currentModuleState = panel->saveModuleSpecificState();
        if (currentModuleState.isValid())
            panel->restoreModuleSpecificState(currentModuleState);

        // 设置模块为浮动态（按钮图标从 □→←）
        panel->setFloating(true);

        // ★ 重新绑定 onPopOutClicked：浮动态下点击按钮 → 触发停靠回 workspace
        panel->onPopOutClicked = [this, panel, savedBounds](ModulePanel&)
        {
            // 查找包含此模块的浮动窗口并关闭
            for (auto it = floatingWindows_.begin();
                 it != floatingWindows_.end(); ++it)
            {
                if ((*it) != nullptr && (*it)->getContentComponent() == panel)
                {
                    auto wnd = std::move(*it);
                    floatingWindows_.erase(it);
                    if (auto state = panel->saveModuleSpecificState(); state.isValid())
                        panel->restoreModuleSpecificState(state);
                    panel->setFloating(false);
                    if (panel->getModuleType() == ModuleType::milkdrop)
                        --floating_milkdrop_count_;
                    wnd->setContentNonOwned(panel, false);
                    wnd.reset();  // 销毁浮动窗口

                    if (workspace != nullptr)
                        workspace->dockModule(*panel, savedBounds);

                    return;
                }
            }
        };

        // ★ 重新绑定 onCloseClicked：浮动态下关闭模块 → 销毁浮动窗口 + 删除模块
        panel->onCloseClicked = [this, panel](ModulePanel&)
        {
            for (auto it = floatingWindows_.begin();
                 it != floatingWindows_.end(); ++it)
            {
                if ((*it) != nullptr && (*it)->getContentComponent() == panel)
                {
                    // 取消 resize 触发的 pending async update，
                    // 防止 handleAsyncUpdate 访问即将被删除的窗口
                    (*it)->cancelAsyncUpdates();
                    auto wnd = std::move(*it);
                    floatingWindows_.erase(it);
                    // ★ 先标记回嵌入态，再拆浮动窗口内容。
                    // 浮动态创建时 SuspendMilkdropEditorRendererForFloating() 已挂起
                    // Editor projectM renderer；若此处不先 setFloating(false)，则
                    // setContentNonOwned → DetachOpenGL 的 should_resume 条件为假，
                    // Editor renderer 永不恢复 → 后续重新添加 Milkdrop 会黑屏。
                    panel->setFloating(false);
                    if (workspace != nullptr)
                        workspace->removeFloatingState(panel->getModuleId());
                    if (panel->getModuleType() == ModuleType::milkdrop)
                        --floating_milkdrop_count_;
                    wnd->setContentNonOwned(panel, false);
                    wnd.reset();
                    // 浮动模块直接 delete（生命周期由调用方管理）
                    delete panel;
                    return;
                }
            }
            // 后备：不在浮动列表中（异常情况），直接删除模块
            delete panel;
        };

        // 获取模块脱离前在屏幕上的真实位置和大小（savedBounds 是 workspace 相对坐标，
        // 需用 localAreaToGlobal 转换为屏幕坐标；此时模块已脱离 tree，必须用 workspace 转换）
        auto screenBounds = workspace->localAreaToGlobal(savedBounds);

        // Clamp screen bounds 到可见屏幕区域，防止窗口被创建在屏幕外侧
        {
          auto& desktop = juce::Desktop::getInstance();
          auto totalArea = desktop.getDisplays().getTotalBounds(true);  // true=包含所有显示器
          if (screenBounds.getX() < totalArea.getX())
            screenBounds.setX(totalArea.getX());
          if (screenBounds.getY() < totalArea.getY())
            screenBounds.setY(totalArea.getY());
          if (screenBounds.getRight() > totalArea.getRight())
            screenBounds.setX(juce::jmax(totalArea.getX(),
                                          totalArea.getRight() - screenBounds.getWidth()));
          if (screenBounds.getBottom() > totalArea.getBottom())
            screenBounds.setY(juce::jmax(totalArea.getY(),
                                          totalArea.getBottom() - screenBounds.getHeight()));
        }

        auto name = panel->getTitleText() + " - Y2Kmeter";
        auto* window = new FloatingModuleWindow(
            name, *panel, savedBounds,
            [this](ModulePanel& p, juce::Rectangle<int> bounds)
            {
                // 停靠回调：从浮动窗口列表中移除，dock 回 workspace
                for (auto it = floatingWindows_.begin();
                     it != floatingWindows_.end(); ++it)
                {
                    if ((*it) != nullptr && (*it)->getContentComponent() == &p)
                    {
                        auto wnd = std::move(*it);
                        floatingWindows_.erase(it);
                        if (auto state = p.saveModuleSpecificState(); state.isValid())
                            p.restoreModuleSpecificState(state);
                        p.setFloating(false);
                        wnd->setContentNonOwned(&p, false);
                        wnd.reset();

                        if (workspace != nullptr)
                            workspace->dockModule(p, bounds);

                        return;
                    }
                }
            },
            this);

        floatingWindows_.push_back(std::unique_ptr<FloatingModuleWindow>(window));

        // 浮动 Milkdrop 窗口计数：renderOpenGL 仅在有浮动 Milkdrop 时才做帧捕获
        if (panel->getModuleType() == ModuleType::milkdrop)
            ++floating_milkdrop_count_;

        window->setUsingNativeTitleBar(false);
        window->setTitleBarHeight(0);
        window->setResizable(true, false);
        window->setDropShadowEnabled(false);
        window->setLookAndFeel(&getPinkXPLookAndFeel()); // Bug 修复：防止脱离后 LookAndFeel 链断裂导致子控件回退为系统默认样式
        window->setContentNonOwned(panel, false);
        // 模块脱离 tree 后 bounds 可能被清零，显式恢复到脱离前尺寸
        panel->setBounds(savedBounds.withPosition(0, 0));
        window->setBounds(screenBounds);
        window->setLayoutLocked(layoutLocked);

        window->addToDesktop();               // 创建 native peer → 触发 resized() → 模块填满窗口
        window->setVisible(true);             // ★ DocumentWindow ctor addToDesktop=false 时 isVisible=false，
                                              //    addToDesktop() 仅创建 peer 不保证 setVisible(true)

        window->setAlwaysOnTop(true);         // native peer 已存在，置顶生效

        // screenBounds 不在此处记录：popOutModule 创建的窗口位置尚未被 restore 逻辑
        // 修正（restore 时会在 setBounds 后立即更新），且首个 handleAsyncUpdate 会覆盖。

    };

    // ★ Bug2修复：恢复上一次会话中处于脱离态的模块（仅在 Standalone 模式）
    //   必须在 onModulePopOutRequested 设置完毕和 onLayoutPresetChanged 设置之前执行，
    //   因为需要 onModulePopOutRequested 回调来创建 FloatingModuleWindow。
    //   具体重建逻辑见 restoreFloatingWindowsFromWorkspace()（退出 stick 后也复用它）。
    restoreFloatingWindowsFromWorkspace();

    // 4.b) P4：host 调用 getStateInformation 前，Processor 会触发此钩子，
    //      把 ModuleWorkspace 里 debounce 合并中的变更立即 flush，并
    //      同步回写最新 XML 到 savedLayoutXml，避免保存到工程的布局
    //      落后 16ms 的"上一版"。
    processor.flushPendingUiStateBeforeSave = [this]()
    {
        if (workspace != nullptr)
        {
            syncFloatingWindowStatesToWorkspace();
            workspace->flushPendingLayoutChange();
            // flushPendingLayoutChange 内部若有待决通知会同步回调
            // onLayoutChanged → 写回当前槽；若无待决，这里兜底
            // 再写一次也廉价（仅字符串赋值），保证一致性。
            persistCurrentLayoutToProcessor();
        }
    };

    // 4.05) 布局预设切换：Preset 1 = 默认布局 + 默认窗口大小；
    //       Preset 2 = 把顶层窗口宽度拉到当前屏幕宽，并让默认模块横向等分 canvas。
    //       实现细节：
    //         · 先清空 workspace 的所有现有模块 / 拼豆贴画；
    //         · Preset 1：恢复 Editor 尺寸到 960×640，然后调 loadInitialModules()
    //           的"默认分支"（强制传空 XML 以绕过恢复路径）；
    //         · Preset 2：取当前所在屏幕 userArea 的宽度，调整顶层窗口宽度到该值，
    //           顶层窗口 x 移到屏幕左边；workspace 的 canvas 拿到后按横向等分
    //           7 个默认模块。
    //         · 切换后手动 notifyLayoutChanged 以把新布局回写 Processor。
    workspace->onLayoutPresetChanged = [this](ModuleWorkspace::LayoutPreset preset)
    {
        // ★ Bug4修复：切换布局预设前，强制归位所有脱离态模块
        //   否则新旧布局混杂，切换后的模块与残留浮动窗口并存。
        //   v2.8.5：stick 驻留态例外 —— 重置的是"条状槽布局"，用户脱离出去的
        //   模块应当继续以浮动窗口存在，不能被 dock 回来再被 clearAllModules 删掉。
        if (workspace != nullptr && !floatingWindows_.empty() && ! stickActive)
        {
            // 先保存当前布局（包含浮动模块的干净布局，写入当前活动槽）
            persistCurrentLayoutToProcessor();

            for (auto& wnd : floatingWindows_)
            {
                if (wnd != nullptr)
                {
                    if (auto* content = dynamic_cast<ModulePanel*>(wnd->getContentComponent()))
                    {
                        const auto savedBounds = wnd->getSavedBounds();
                        wnd->setContentNonOwned(content, false);
                        workspace->dockModule(*content, savedBounds);
                    }
                    wnd.reset();
                }
            }
            floatingWindows_.clear();
        }

        // ★ 预设切换前：先退出 Hide（窗口展开回常驻态）。
        //   applyLayoutPreset 通过 getCanvasArea() 反推 overheadH 时依赖 chromeVisible；
        //   Hide 态 canvas 满铺会让 overheadH 少算工具栏高度。
        //   v2.8.2：L 只是交互闸门、不再改 resizeLimits，因此预设切换无需解锁 L。
        if (workspace != nullptr && ! workspace->isChromeVisible())
            workspace->setChromeVisible (true);

        // v1.9.x：新手引导期间的预设切换处理
        //   · 切换到非 default 预设 → 跳过引导
        //   · 切换回 default 时若之前被跳过 → 重新触发
        if (tutorialStep != TutorialStep::hidden && preset != ModuleWorkspace::LayoutPreset::defaultGrid)
        {
            skipTutorial();
        }
        else if (tutorialStep == TutorialStep::hidden && tutorialWasSkipped
                 && preset == ModuleWorkspace::LayoutPreset::defaultGrid)
        {
            startTutorial();
        }

        applyLayoutPreset ((int) preset);
    };

    // 4.05b) 预设 Save/Load —— 仅做透传：workspace 已经弹完 FileChooser，
    //        Editor 没有 settings 文件的写入能力（PropertiesFile 归 Standalone App
    //        持有），所以把 File 原样转发给外层订阅者（Y2KStandaloneApp）。
    //        外层未订阅时 onSave/LoadSettingsRequested 为空回调 → 点击按钮无效
    //        但不崩溃（VST3/AU 插件模式下即使按钮被误显也安全）。
    workspace->onSavePresetRequested = [this](juce::File dest)
    {
        if (onSaveSettingsRequested) onSaveSettingsRequested (dest);
    };
    workspace->onLoadPresetRequested = [this](juce::File src)
    {
        if (onLoadSettingsRequested) onLoadSettingsRequested (src);
    };

#if JUCE_WINDOWS
    // 4.05c) stick 模式按钮（Windows 专属）：STICK 进入 / X 退出 / T-B 顶底切换
    workspace->onStickClicked       = [this]() { enterStickMode(); };
    workspace->onStickExitClicked   = [this]() { exitStickMode(); };
    workspace->onStickToggleClicked = [this]() { toggleStickTopBottom(); };
#endif

    // 4.1) chrome 可见性变化（Hide/Show 按钮 / 启动恢复）→ 进入/退出 Hide 模式
    //     v2.8.2：workspace 的 chromeVisible 只在"用户点 Hide/Show"或"启动恢复"时翻转；
    //     悬停浮层不经过此回调（浮层不改窗口几何），这里只做一次性的收缩/展开。
    //     详细状态机见 setAutoHideMode / setChromeRevealed / updateChromeReveal。
    workspace->onChromeVisibleChanged = [this](bool visible)
    {
        setAutoHideMode (! visible);
    };

    // v2.8.2：旧版 workspace 悬停回调（onMouseEntered/Moved/Exited）不再使用 ——
    //   Hide 悬停改为 30Hz 轮询鼠标是否位于顶层窗口内（updateChromeReveal），
    //   不依赖任何 enter/exit 事件，也就不会因窗口 resize 产生的虚假事件而抖动。
    workspace->onMouseEntered = nullptr;
    workspace->onMouseMoved   = nullptr;
    workspace->onMouseExited  = nullptr;

    // 4.2) 音频源下拉变化 → 透传给外部（Standalone App 订阅后会真正切换音频设备）
    workspace->onAudioSourceChanged = [this](const juce::String& sourceId, bool isLoopback)
    {
        if (onAudioSourceChanged) onAudioSourceChanged (sourceId, isLoopback);
    };

    // 4.3) FPS 限制按钮切换 → 修改 AnalyserHub 的 FrameDispatcher 频率
    workspace->onFpsLimitChanged = [this](int hz)
    {
        // FPS 限制采用"封顶"语义：FrameDispatcher 固定以用户选择的上限频率调度。
        // hz=0 表示无上限：下发 120Hz（analyser 内部允许的最大值）。
        if (hz == 0)
        {
            userRequestedFpsLimit = 0;
            activeDispatchHz      = 120;
        }
        else
        {
            userRequestedFpsLimit = juce::jlimit (15, 120, hz);
            activeDispatchHz      = userRequestedFpsLimit;
        }

        processor.getAnalyserHub().startFrameDispatcher (activeDispatchHz);
        // 立即重置统计起点，避免切换后显示跨区间的均值
        frameCounter.store (0, std::memory_order_relaxed);
        lastFrameCounterSample = 0;
        lastGlFrameCounterSample = glFrameCounter.load (std::memory_order_relaxed);
        lastDispatchWallMs = 0.0;
        vblankCreditMs     = 0.0;
        updateFramePacingFallbackTimer();
        lastFpsTimeMs = juce::Time::getMillisecondCounterHiRes();
    };

    // 4.4) gain 控制条变化 → 下发到分析输入增益（仅影响分析链，不影响透传输出）
    workspace->onInputGainChanged = [this](float db)
    {
        processor.setAnalysisInputGainDb (db);
    };
    workspace->setInputGainDb (processor.getAnalysisInputGainDb());

    // 订阅主题变更 → 切主题后刷新全局重绘
    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        Y2K_LOG ("[palette] editor theme callback: invalidate desktop cache");
        if (auto* lnf = dynamic_cast<juce::LookAndFeel_V4*>(&getLookAndFeel()))
            juce::ignoreUnused(lnf);
        invalidateDesktopCache();
        repaint();
    });

    // 应用默认主题（会把全局配色重写一次，确保状态一致）
    PinkXP::applyTheme(PinkXP::getCurrentThemeId());

    // 构造阶段 isShowing() 可能仍为 false，先确保分析链启动；
    // 后续由 visibilityChanged() 继续做可见性联动。
    processor.setAnalysisActive(true);

    // Editor 自身 10Hz 轮询：拉 Processor 的 CPU 占比并广播到所有模块的
    //   右下角标签，同时计算实际 FPS 下发给 workspace 的 FPS 标签
    startTimerHz (10);

    // Phase F：启动全局 FrameDispatcher（模块构造中的 retain() 已让 refCounts 就绪）
    userRequestedFpsLimit = workspace->getFpsLimit();
    if (userRequestedFpsLimit == 0)
    {
        activeDispatchHz = 120;
    }
    else
    {
        userRequestedFpsLimit = juce::jlimit (15, 120, userRequestedFpsLimit);
        activeDispatchHz      = userRequestedFpsLimit;
    }
    processor.getAnalyserHub().startFrameDispatcher (activeDispatchHz);

    // 订阅帧分发，以计算实际 FPS（内部辅助类，避免 header 对 AnalyserHub 的硬依赖）
    fpsListener = std::make_unique<FpsFrameListener> (*this);
    processor.getAnalyserHub().addFrameListener (fpsListener.get());
    lastFpsTimeMs = juce::Time::getMillisecondCounterHiRes();

    // v2.9：挂 vblank 帧时钟。首个 vblank 到来时才切换到外部时钟，
    //   在此之前（peer 尚未创建）仍由 Hub 内部 Timer 驱动，不会出现空窗。
    // v2.9.1：兜底 Timer 在外部时钟接管后启用（vblank 被 GL 持锁吞掉时补帧）。
    framePacingFallback = std::make_unique<FramePacingFallbackTimer> (*this);
    vblankAttachment = std::make_unique<juce::VBlankAttachment> (this, [this] (double ts)
    {
        onDisplayVBlank (ts);
    });

    // ------------------------------------------------------
    // 插件宿主（VST3 / AU / AAX / LV2）模式下的一次性差异化配置
    //   · 隐藏底部 Toolbar 中的"Source"下拉（宿主直接提供音频）
    //   · 不绘制自画伪标题栏 / 右上三按钮 / 窗口拖拽（由宿主窗口边框负责）
    //   · 不启用"默认置顶"行为（对宿主无意义，且多数宿主会忽略该调用）
    //   · 关闭 chromeHiddenOverlay（插件下永远不会进入 Hide 态的浮层）
    // 注：paint/resized/mouseXxx 中会再次基于 isPluginHost 做绕行，
    //     此处只是一次性的初始状态设置。
    // ------------------------------------------------------
    if (isPluginHost)
    {
        if (workspace != nullptr)
        {
            workspace->setAudioSourceUiVisible (false);
            // 布局预设下拉在插件模式下同样隐藏：
            //   切换预设会改顶层窗口尺寸 / 位置（横向铺满屏幕等），和宿主窗口会打架；
            //   插件下始终使用默认预设即可，无需用户选择。
            workspace->setLayoutPresetUiVisible (false);

            // 但 Save/Load 两个按钮仍然保留 —— 用户依然需要在 DAW 里导入/导出
            //   预设文件（与 Standalone Save 出的 .settings 互通）。按钮独立贴在
            //   Grid 左侧显示。
            workspace->setSaveLoadUiVisible (true);

            // 插件模式下不提供模块弹出功能（无法创建独立顶层窗口）
            workspace->setPopOutEnabled (false);
        }

        // 插件模式下 Save/Load 不经过 Standalone App，直接由 Editor 就地处理：
        //   · Save：把 Processor::getStateInformation 的数据包装成与 Standalone
        //     完全兼容的 .settings 文件（<PROPERTIES> 外壳 + base64 编码的
        //     filterState 条目），Standalone 下一次打开该文件也能正常恢复。
        //   · Load：容错读取三种格式
        //       a) Standalone .settings —— 解析 <PROPERTIES> 里 filterState 条目，
        //          base64 解码 + copyXmlToBinary 反序列化为 MemoryBlock，再喂给
        //          Processor::setStateInformation；
        //       b) 裸 PBEQ_State XML（Processor state 原生 XML 形态）；
        //       c) 裸 PBEQ_Layout XML（仅布局，不含其它）—— 直接灌给 workspace。
        //     成功后调用 workspace->loadLayoutFromXml 热重载，无需重启宿主。
        //
        //   注：插件模式下"主题/窗口尺寸/音频源"等 runtime 状态由 DAW 或 Editor 自身
        //   管理，Load 过来的 .settings 里的那些 key 在这里被有意忽略；我们只抽取
        //   和 Processor state 相关的部分 —— 这与"预设 = 模块布局"的用户心智一致。
        onSaveSettingsRequested = [this](juce::File dest)
        {
            if (dest == juce::File{}) return;
            saveStateAsSettingsFile (dest);
        };
        onLoadSettingsRequested = [this](juce::File src)
        {
            if (src == juce::File{} || ! src.existsAsFile()) return;
            loadStateFromSettingsFile (src);
        };

        // 插件模式下不画伪标题栏 / chrome 浮层
        chromeHiddenOverlay.reset();

        // 插件下不主动置顶（让宿主完全掌控窗口行为）
        alwaysOnTopActive             = false;
        initialAlwaysOnTopApplied     = true; // 抑制 visibilityChanged 里的首次推送

        // 触发一次重排：workspace 在插件模式下从 y=0 起铺满整个 Editor
        resized();
    }

#ifdef Y2KMETER_MILKDROP_ONLY
    // ==================================================================
    // Milkdrop-only 插件变体：整个窗口 = 单个满屏 Milkdrop 模块。
    //   · 隐藏底部 toolbar / 标题栏 / 主题 / 布局预设 / Save-Load / Source。
    //   · 锁定布局（禁拖拽/缩放/右键添加菜单）。
    //   · 添加一个裸模式 Milkdrop 模块铺满 canvas（无卡片外壳）。
    // ==================================================================
    if (workspace != nullptr)
    {
        workspace->setMinimalMode (true);
        workspace->setAvailableModuleTypes ({ ModuleType::milkdrop });
        workspace->setLayoutLocked (true);
        workspace->setAudioSourceUiVisible (false);
        workspace->setLayoutPresetUiVisible (false);
        workspace->setSaveLoadUiVisible (false);
        workspace->setPopOutEnabled (false);

        const auto canvas = workspace->getCanvasArea();
        auto milk = createModule (ModuleType::milkdrop);
        if (milk != nullptr)
        {
            if (auto* md = dynamic_cast<MilkdropModule*> (milk.get()))
                md->setBareMode (true);
            milk->setBounds (canvas.getX(), canvas.getY(),
                             canvas.getWidth(), canvas.getHeight());
            workspace->addModule (std::move (milk), false);
        }
    }

    // Milkdrop-only：无标题栏，workspace 铺满整个 Editor
    resized();
#endif

    // ==================================================================
    // 宿主自动化参数恢复 + 双向同步（milkdrop-only 与完整版插件共用）。
    //   仅插件宿主形态（VST3/AU）执行：Standalone 无宿主参数，auto 状态由
    //   布局 XML 恢复、visual/wave 由 host state 顶层属性恢复，无需走 APVTS。
    // ==================================================================
    if (isPluginHost)
    {
        // 1) 先从宿主恢复的参数应用到模块：auto 状态是模块级状态，仅通过 APVTS
        //    参数（md_auto_*）持久化，必须在此先行恢复；否则下方 push 会用模块
        //    默认状态覆盖宿主恢复值，导致"关闭界面重开后 auto 回归默认"。
        {
            MilkdropModule* md = nullptr;
            if (workspace != nullptr)
                for (int i = 0; i < workspace->getNumModules(); ++i)
                    if (auto* m = dynamic_cast<MilkdropModule*> (workspace->getModule (i)))
                    { md = m; break; }

            if (md != nullptr)
            {
                auto& apvts = processor.getApvts();
                md->setAutoModeEnabled (apvts.getRawParameterValue (MilkdropParams::kAutoMode)->load() >= 0.5f);
                md->setAutoBeatMode (apvts.getRawParameterValue (MilkdropParams::kAutoBeatMode)->load() >= 0.5f);
                md->setAutoBeats (juce::roundToInt (apvts.getRawParameterValue (MilkdropParams::kAutoBeats)->load()));
                // applyAutoInterval 内部会触发一次 push 回写参数，放最后确保此时
                // autoMode/autoBeatMode/autoBeats 均已恢复，push 回写的是完整状态。
                md->applyAutoInterval (apvts.getRawParameterValue (MilkdropParams::kAutoInterval)->load());
            }

            // 预设索引恢复：直接入队跳转，Editor GL 上下文就绪后消费。
            const int presetIndex = juce::roundToInt (processor.getApvts()
                .getRawParameterValue (MilkdropParams::kPresetIndex)->load());
            if (presetIndex >= 0)
                RequestMilkdropPresetJump (presetIndex);
        }

        // 2) 再把当前状态同步到宿主参数（visual/wave 从 Processor 恢复，auto 已从
        //    参数恢复），避免 timer 首次 pull 用默认参数覆盖已恢复状态。
        pushMilkdropAutomationParams();

        // 3) 注册宿主参数监听器：宿主 automation / MIDI CC 改参数时，APVTS 可能
        //    在音频线程回调 parameterChanged；HostParamListener 内部用 AsyncUpdater
        //    转到消息线程高频应用视觉/wave 状态，消除 10Hz pull 采样造成的"阶梯式"
        //    变化。auto/preset 仍由 10Hz pull 兜底（离散操作，无需高频）。
        hostParamListener_ = std::make_unique<HostParamListener> (*this);
        {
            auto& apvts = processor.getApvts();
            for (auto* p : processor.getParameters())
                if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                    apvts.addParameterListener (withId->getParameterID(), hostParamListener_.get());
        }
    }

    // ==================================================================
    // GPU 合成层挂载（Standalone + VST3 共用此入口）
    //
    //   · attachTo(*this) 之后，本 Editor 及其所有子组件（workspace、各
    //     ModulePanel、chromeHiddenOverlay）的 paint() 命令都会被 JUCE 翻译
    //     成 OpenGL 批次在 GPU 上执行。软光栅路径（CoreGraphicsContext::
    //     fillCGRect / drawGlyphs / fillPath / strokePath）彻底绕开，主线程
    //     只剩 draw 命令组装与提交，CPU 负载大幅下降。
    //
    //   · setContinuousRepainting(false)：**关键**。默认为 true 时，GL 上下文
    //     会按屏幕刷新率不停重绘，等于强制 60~120Hz，不受我们 FrameDispatcher
    //     的 adaptiveDispatchHz 控制。设为 false 后只在子组件调用 repaint()
    //     时才触发一次 GL 重绘，和现有的节流/脏区策略完美配合。
    //
    //   · setComponentPaintingEnabled(true) 是默认值，显式写出来只是强调
    //     "让 GL 上下文负责普通 JUCE 组件的 paint"，不是手动 glDraw。
    //
    //   · 插件宿主（VST3/AU）场景：
    //       - macOS：JUCE 为 Editor 顶层 NSView 创建一个 NSOpenGLContext 子层，
    //         宿主窗口其余部分不受影响。ProTools/Logic/Cubase/Live 等主流 DAW 均支持。
    //       - Windows：JUCE 为 HWND 创建一个子 GL 子窗口，同理。
    //     极少数老版 ProTools/AU-Hosts 对子 GL 上下文兼容性差；若将来发现某宿主
    //     花屏/黑屏，可在此处加 isPluginHost 保护把插件模式下回退到软光栅。
    //     当前按"两种模式都走 GPU"的需求开启。
    //
    //   · 析构顺序：openGLContext 声明在 Editor 类末尾 → 成员反向析构会最先析构
    //     GL 上下文（JUCE 会自动 detach），但为了防止 GL 资源释放时 child
    //     components 已被部分销毁的 UB，~Editor 起始处仍显式 detach() 一次兜底。
    //
    //   · **macOS 专项关闭**（2026-05 性能修复）：
    //       Apple 从 10.14 起把 OpenGL 标记为 Deprecated，legacy NSOpenGLContext 在
    //       Apple Silicon / 新版 Intel GPU 上走的是 Metal 兼容转译层；每次 CPU 端
    //       修改 juce::Image（Spectrogram 的 imageBuf、Oscilloscope 的 staticLayer/
    //       dynamicLayer、Editor 的 desktopCacheImage 等）都会触发"纹理失效 →
    //       整张重新 upload"的隐式同步，多个 SpectrogramModule / SpectrumModule /
    //       WaveformModule / OscilloscopeModule 叠加时会线性放大到几十 MB/帧的
    //       带宽，导致明显卡顿。macOS 下 CoreGraphics 本身已有 Metal 后端加速，
    //       直接走软光栅路径反而更快。
    //     Windows 侧 WGL + 硬件驱动对小批次 draw / 纹理部分更新优化成熟，保持开启。
    // ==================================================================
    //
    // ==================================================================
// ✅ v2.2.1 恢复 Editor 级 OpenGL 上下文 —— 核心目的：解决 Z-order 遮盖
    //
    //   Editor 级 GL (setComponentPaintingEnabled=true, attachTo(*this))
    //   使主窗口进入 GPU 合成管线，所有子组件（包括 Milkdrop GLView）的 paint
    //   输出通过 Editor 的 CachedImage FBO 在 GPU 侧统一合成 → 单次 SwapBuffers
    //   输出到屏幕。不存在"GL 原生 HWND 覆盖 GDI 内容"的 Z-order 冲突。
    //
    //   嵌套 GL 上下文说明：
    //     Editor GL + Milkdrop GLView GL (componentPaintingEnabled=false)
    //     两个 GL context 在同一窗口中共存。JUCE 官方不建议嵌套，但在
    //     Release 模式下 (NDEBUG) jassertfalse 编译为 no-op。实测 NVIDIA 驱动
    //     (nvoglv64.dll) 下嵌套 GL 工作正常—v2.1.9 (d39397e) 已验证 30fps 无遮盖。
    //     AMD (atidxx64.dll) 可能有兼容问题，但当前用户为 NVIDIA 环境。
    //
    //   macOS：CoreGraphics 已有 Metal 后端，Editor GL 反而降低性能，跳过 attach。
    // ==================================================================
#if ! JUCE_MAC
    openGLContext.setRenderer(this);
    openGLContext.setContinuousRepainting(false);
    openGLContext.setComponentPaintingEnabled(true);
    openGLContext.attachTo(*this);
#endif

    // ==================================================================
    // v1.9.x：新手引导覆盖层 —— 按需创建/销毁，不提前驻留在 child list 中
    //   · startTutorial() 时创建 + addChildComponent
    //   · dismissTutorialOverlay() 时 removeChildComponent + reset
    //   · 关键：引导不活动时 Editor 子组件列表中完全不存在此组件，
    //     从根本上杜绝 JUCE 子组件遍历 / OpenGL 渲染合成层的任何干扰
    // ==================================================================

    // 首次启动检测：非插件模式且 processor 未记录完成 → 启动引导
    if (! isPluginHost && ! processor.isTutorialCompleted())
        startTutorial();
}

Y2KmeterAudioProcessorEditor::~Y2KmeterAudioProcessorEditor()
{
    // 进入析构：后续宿主/JUCE 对 Editor 的任何 resize（包括 FL Studio 关闭
    //   嵌入子窗口时塞给我们的 ~175×85 极小中间态）都不应被 resized() 回写到
    //   Processor.savedEditorSize，否则下次打开窗口会恢复成异常小尺寸。
    editorBeingDestructed = true;

    // -3) 先把所有浮动窗口里的模块 dock 回 workspace，并在停靠前后各存一次布局。
    //   关键：dock 前先清零 onLayoutChanged，防止 dockModule → notifyLayoutChanged
    //   把 pending 标志留在 workspace 析构函数里调用 onLayoutChanged → saveLayoutTree
    //   时因为 children 状态不一致而崩溃。
    if (workspace != nullptr)
    {
        // 先同步所有浮动窗口真实屏幕 bounds，再保存脱离态布局。
        //   v2.8.5：按当前活动槽写回 —— 若退出时仍处于 stick 态，这里写的是
        //   stick 槽，非 stick 布局保持原样（下次启动恢复 stick 后点 X 能回去）。
        syncFloatingWindowStatesToWorkspace();
        workspace->flushPendingLayoutChange();
        persistCurrentLayoutToProcessor();
        // 清零回调，后续 dock 操作不再触发保存
        workspace->onLayoutChanged = nullptr;
    }

    for (auto& wnd : floatingWindows_)
    {
        if (wnd != nullptr)
        {
            if (auto* content = dynamic_cast<ModulePanel*>(wnd->getContentComponent()))
            {
                const auto savedBounds = wnd->getSavedBounds();
                wnd->setContentNonOwned(content, false);
                if (workspace != nullptr)
                    workspace->dockModule(*content, savedBounds);
            }
            wnd.reset();
        }
    }
    floatingWindows_.clear();

    // ★ 析构安全：dock 完成后重置 Milkdrop 浮动计数器（防止 renderOpenGL 在析构中途访问已释放的 FBO）
    floating_milkdrop_count_ = 0;

    // 这里不能再保存 dock 后的临时布局：关闭前已经保存了包含 floating=true 的真实布局，
    // 析构阶段 dock 只是为了安全释放窗口资源，若再次写回会把脱离态存档覆盖成嵌入态。

    // P4：先解绑 Processor → Editor 的 flush 钩子，避免析构阶段里
    //   宿主线程调 getStateInformation 触发回调到已部分销毁的 this。
    processor.flushPendingUiStateBeforeSave = nullptr;

    // 注销宿主参数监听器，避免 Editor 析构后 APVTS 再回调到悬垂的 this。
    if (hostParamListener_ != nullptr)
    {
        auto& apvts = processor.getApvts();
        for (auto* p : processor.getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                apvts.removeParameterListener (withId->getParameterID(), hostParamListener_.get());
        hostParamListener_.reset();
    }

    // -1) Editor 级 GL 上下文 detach：最先解除 GPU 绑定，确保 workspace 等子组件
    //      析构时 GL 资源（Texture/VBO/FBO）已经在上下文解除后安全释放。
    //      macOS 下构造时未 attach，这里同样跳过。
#if ! JUCE_MAC
    openGLContext.detach();
#endif

    // 0) 先停掉 Editor 自身的 timer，避免 workspace.reset() 中途被调
    stopTimer();

    // v2.9：先拆掉 vblank 帧时钟与兜底 Timer，避免之后的析构过程中仍有分发
    vblankAttachment.reset();
    framePacingFallback.reset();

    // 兜底：若曾为 VirtuPet 临时保活 Loudness，析构前配平 release。
    if (virtuPetSignalRetained)
    {
        processor.getAnalyserHub().release (AnalyserHub::Kind::Loudness);
        virtuPetSignalRetained = false;
    }

    // 取消订阅帧分发（在 stopFrameDispatcher 之前取消，更严谨）
    if (fpsListener != nullptr)
    {
        processor.getAnalyserHub().removeFrameListener (fpsListener.get());
    }

    // Phase F：停掉 FrameDispatcher，避免 workspace/模块离场后
    //   还有 UI Timer 回调到已析构的 FrameListener。
    processor.getAnalyserHub().stopFrameDispatcher();

    if (themeSubToken != 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = 0;
    }

    // 1) 释放 workspace（内部会停止所有 Timer、清空子组件）
    //    必须在清理 LookAndFeel / Typeface 之前完成
    workspace.reset();

    // Editor 销毁后关闭分析，避免后台继续进行无意义计算
    processor.setAnalysisActive(false);

    // 2) 解绑 LookAndFeel（本组件 + 自身）
    setLookAndFeel(nullptr);

    // 3) 关键：清空全局 Typeface 缓存，防止插件 DLL 卸载时
    //    BinaryData 内存已被释放而全局 gTypeface 仍持有悬垂引用导致宿主卡死
    PinkXP::initCustomTypeface(nullptr);

    // 3.1) 关键：清空 PinkXPStyle 的桌面纹理缓存（getSharedDesktopTexture）。
    //
    //   【此行是 2026-05 Windows VST3 FL Studio 删除插件卡死修复的核心】
    //
    //   根因：该缓存以 TU 内部 static vector 持有若干 juce::Image，Windows 上
    //   实际类型是 Direct2DPixelData，内部通过 SharedResourcePointer<DirectX>
    //   持有 ID3D11Device / DxgiAdapters 全局单例。若不在这里主动清理：
    //     · DLL 卸载时 CRT 走 execute_onexit_table 析构这些 Image；
    //     · Direct2DPixelData 析构 → DirectX 引用计数归零 → ID3D11Device 释放；
    //     · AMD 驱动 atidxx64.dll::CDevice::DestroyDriverInstance 内部需要
    //       等待 GPU worker thread 退出（GetExitCodeThread / SleepEx）；
    //     · 但此时 CRT 持有 atexit lock，loader lock 也在 FreeLibrary 链路里
    //       被持有，驱动 worker 线程无法完成 DLL 引用链处理 → 主线程永久等待。
    //   修复：在 Editor 析构时（此时 loader lock 未持有、audio 回调已停），
    //   主动把这些 Image 释放干净，让 DirectX 单例在此刻就自然析构，DLL 卸载
    //   时 atexit 链里就没有"等 GPU 驱动 worker 线程"这种重活。
    PinkXP::invalidateDesktopTextureCache();

    // 3.2) 关键：解除 LookAndFeel 对 Typeface 的引用。
    //   PinkXPLookAndFeel 是 TU 级 static 实例（生命周期与 DLL 相同），通过
    //   setDefaultSansSerifTypeface 持有 Typeface::Ptr。Windows 上
    //   DirectWriteTypeface 内部持有 SharedResourcePointer<Direct2DFactories>，
    //   不提前解引用的话，同样会在 atexit 阶段触发 DirectWrite / Direct2D
    //   全局资源的 DLL 卸载链析构。
    getPinkXPLookAndFeel().setDefaultSansSerifTypeface (nullptr);
    juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface (nullptr);

    // 4) 清空 ImageCache（保险；我们自己不用它，但有些 JUCE 默认路径可能写入）
    juce::ImageCache::releaseUnusedImages();

    // 4.1) 清空 JUCE 内置 Typeface 缓存（juce::Font 查字型时内部 LRU；
    //   也可能持有 DirectWriteTypeface 实例 → 同样的 atexit 风险）。
    juce::Typeface::clearTypefaceCache();

    // 5) 实例成员 customTypeface / logoImage 会在随后自动析构，
    //    此时已没有任何 LookAndFeel / Font / Image 引用 BinaryData，安全
}

// ----------------------------------------------------------
// 初始化
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::initLookAndFeel()
{
    // 把字体注入全局 PinkXP，供所有模块使用
    PinkXP::initCustomTypeface(customTypeface);
    setLookAndFeel(&getPinkXPLookAndFeel());
}

void Y2KmeterAudioProcessorEditor::initWorkspace()
{
    // 已合并到构造器 + loadInitialModules()。保留空实现以兼容旧声明（若 header 仍声明）
    loadInitialModules();
}

void Y2KmeterAudioProcessorEditor::loadInitialModules()
{
    // 1) 优先恢复已保存布局
    //    v2.8.5：若上次退出时处于 stick 驻留态，装载对应的一侧槽（T/B），
    //    让窗口上屏时显示的就是用户上次看到的条状布局；AppBar 驻留本身
    //    要等 HWND 就绪，由 timerCallback → restoreSavedStickSession() 补做。
    juce::String savedXml;
#if JUCE_WINDOWS
    if (pendingStickRestore)
    {
        savedXml = processor.getSavedLayoutXml (
            pendingStickBottom ? Y2KmeterAudioProcessor::LayoutSlot::stickBottom
                               : Y2KmeterAudioProcessor::LayoutSlot::stickTop);

        if (savedXml.isEmpty())
        {
            // stick 槽缺失（存档不完整）：放弃本次自动驻留，退回普通布局。
            //   否则布局槽映射会把普通布局写进 stick 槽，还会 dock 一个不匹配
            //   的条状窗口。
            pendingStickRestore = false;
            processor.setSavedStickSession (false, pendingStickBottom);
            savedXml = processor.getSavedLayoutXml (Y2KmeterAudioProcessor::LayoutSlot::normal);
        }
    }
    else
#endif
    {
        savedXml = processor.getSavedLayoutXml();
    }

    // 1.5) 先恢复脱离态模块记录（独立存档，不隶属于任何布局槽）。
    //   必须在 applyLayoutPreset / 任何 persist 之前装载：否则启动阶段写盘会把
    //   Processor 里已有的浮动记录覆盖成空。
    //   这里只填充 workspace 的记录，不创建窗口 —— 创建窗口依赖
    //   onModulePopOutRequested，此时 callback 还没接上。
    workspace->loadFloatingModulesFromXml (processor.getSavedFloatingModulesXml());

    bool layoutLoaded = false;
    if (savedXml.isNotEmpty())
        layoutLoaded = workspace->loadLayoutFromXml(savedXml);

    if (! layoutLoaded)
    {
        // 2) 首次打开（无已保存布局）的默认预设：
        //    · Standalone 模式：Horizontal Bar(T)——保留老用户习惯（横向铺满屏幕顶部）；
        //    · 插件模式（VST3 / AU 等）：Default 预设——宿主 DAW 的子窗口不适合让
        //      Y2Kmeter 主动去改顶层窗口尺寸/位置（horizontalFull 会尝试把窗口拉到
        //      屏幕宽度并移到屏幕左上，和宿主嵌入布局冲突），因此首次使用"默认瀑布
        //      布局 + 默认 960×640 窗口"更安全、体验更一致。
        //    注意：仅变更首次默认入口，不影响预设列表里的 Default / Horizontal Bar 选项。
        const auto firstRunPreset = isPluginHost
                                        ? ModuleWorkspace::LayoutPreset::defaultGrid
                                        : ModuleWorkspace::LayoutPreset::horizontalFull;
        applyLayoutPreset ((int) firstRunPreset);
    }
    // 浮动窗口由构造流程后半段的 restoreFloatingWindowsFromWorkspace() 统一创建
    //   （那时 onModulePopOutRequested 已就绪）。
}

// ----------------------------------------------------------
// v1.9.x：新手引导流程控制（仅 Standalone 模式）
//   · startTutorial()       —— 启动 STEP 1：右键添加 VirtuPet
//   · advanceTutorialStep2()—— 用户添加了 VirtuPet → 推进到 STEP 2
//   · completeTutorial()    —— 宠物孵化完成 → 标记完成并持久化
//   · skipTutorial()        —— 用户切换预设时跳过引导
//   · dismissTutorialOverlay—— 隐藏覆盖层，清空 step 状态
//   · checkTutorialStep2Condition —— timer 中轮询：蛋是否孵化
// ----------------------------------------------------------

void Y2KmeterAudioProcessorEditor::startTutorial()
{
    // 仅 Standalone 生效；插件模式与已完成状态直接跳过
    if (isPluginHost || processor.isTutorialCompleted()) return;
    if (workspace == nullptr) return;

    // 按需创建覆盖层（不提前创建，避免干扰子组件事件路由）
    if (tutorialOverlay == nullptr)
    {
        tutorialOverlay = std::make_unique<TutorialOverlay>();
        addChildComponent (*tutorialOverlay);

        // 订阅 "右键点击聚光灯区域" → 打开模块选择菜单
        tutorialOverlay->onRightClickHighlight = [this](juce::Point<int> clickPos)
        {
            if (tutorialStep == TutorialStep::step1_rightClick && workspace != nullptr)
            {
                const auto screenPos = tutorialOverlay->localPointToGlobal (clickPos);
                const auto wsPos = workspace->getPosition();
                const auto canvasPos = clickPos - wsPos;

                // 不销毁 overlay！切换文案引导用户点击菜单中的 VirtuPet
                tutorialOverlay->showStep1MenuOpened();
                tutorialStep = TutorialStep::step1_menuOpened;

                // 弹出受限菜单：仅 VirtuPet 可选，关闭时恢复 STEP1 文案
                workspace->showAddMenu (screenPos, canvasPos,
                    { ModuleType::virtuPet },
                    [this]()
                    {
                        // 用户关闭了菜单但没有选择 → 恢复文案，回到 STEP1
                        if (tutorialOverlay != nullptr && tutorialStep == TutorialStep::step1_menuOpened)
                        {
                            tutorialOverlay->showStep1 (
                                tutorialOverlay->getHighlightArea());
                            tutorialStep = TutorialStep::step1_rightClick;
                        }
                    });
            }
        };

        // 订阅 "点击气泡 × 按钮 → 确认跳过" → 跳过新手引导
        tutorialOverlay->onSkipRequested = [this]()
        {
            skipTutorial();
        };
    }

    // 获取 canvas 区域（相对 Editor 坐标 = workspace 坐标 + workspace 的 Y 偏移）
    const auto canvas = workspace->getCanvasArea();
    const auto wsPos  = workspace->getPosition();
    const auto canvasInEditor = canvas.translated (wsPos.x, wsPos.y);

    tutorialStep = TutorialStep::step1_rightClick;
    tutorialWasSkipped = false;

    tutorialOverlay->setBounds (getLocalBounds());
    tutorialOverlay->showStep1 (canvasInEditor);
    tutorialOverlay->toFront (false);
}

void Y2KmeterAudioProcessorEditor::advanceTutorialStep2()
{
    if (tutorialStep != TutorialStep::step1_rightClick
        && tutorialStep != TutorialStep::step1_menuOpened) return;

    // overlay 一直存活（STEP1 右键后未销毁），直接切换到 STEP2
    jassert (tutorialOverlay != nullptr);

    // 查找刚添加的 VirtuPet 模块所在位置
    juce::Rectangle<int> petAreaInEditor;
    if (workspace != nullptr)
    {
        const auto wsPos = workspace->getPosition();
        for (int i = 0; i < workspace->getNumModules(); ++i)
        {
            if (auto* m = workspace->getModule (i))
            {
                if (m->getModuleType() == ModuleType::virtuPet)
                {
                    const auto modBounds = m->getBounds();
                    petAreaInEditor = modBounds.translated (wsPos.x, wsPos.y);
                    break;
                }
            }
        }
    }

    tutorialStep = TutorialStep::step2_playAudio;

    tutorialOverlay->setBounds (getLocalBounds());
    tutorialOverlay->showStep2 (petAreaInEditor);
    tutorialOverlay->toFront (false);
}

void Y2KmeterAudioProcessorEditor::completeTutorial()
{
    tutorialStep = TutorialStep::hidden;
    tutorialWasSkipped = false;
    dismissTutorialOverlay();

    // 持久化完成状态
    processor.setTutorialCompleted (true);

    // 清理 VirtuPet 信号保留（由正常的 tick 逻辑接管）
}

void Y2KmeterAudioProcessorEditor::skipTutorial()
{
    tutorialStep = TutorialStep::hidden;
    tutorialWasSkipped = true;
    dismissTutorialOverlay();

    // 持久化跳过状态：用户主动跳过引导后，下次启动不再触发。
    // （v2.1.5 之前不标记 tutorialCompleted，导致每次重启都重新出现）
    processor.setTutorialCompleted (true);
}

void Y2KmeterAudioProcessorEditor::dismissTutorialOverlay()
{
    if (tutorialOverlay != nullptr)
    {
        // 先隐藏（内部清理状态），再移出 child list，最后销毁
        tutorialOverlay->hide();
        removeChildComponent (tutorialOverlay.get());
        tutorialOverlay.reset();
    }
}

void Y2KmeterAudioProcessorEditor::checkTutorialStep2Condition()
{
    if (workspace == nullptr || isPluginHost) return;

    // ------ STEP 1: 等待用户从右键菜单中选中 VirtuPet -------
    if (tutorialStep == TutorialStep::step1_menuOpened)
    {
        for (int i = 0; i < workspace->getNumModules(); ++i)
        {
            if (auto* m = workspace->getModule (i))
            {
                if (m->getModuleType() == ModuleType::virtuPet)
                {
                    // 用户已添加 VirtuPet → 推进到 STEP 2
                    advanceTutorialStep2();
                    return;
                }
            }
        }
        return;
    }

    // ------ STEP 2: 等待蛋孵化 -------
    if (tutorialStep != TutorialStep::step2_playAudio) return;

    // 遍历寻找 VirtuPet 模块：检查是否已从蛋阶段孵化
    for (int i = 0; i < workspace->getNumModules(); ++i)
    {
        if (auto* m = workspace->getModule (i))
        {
            if (m->getModuleType() == ModuleType::virtuPet)
            {
                if (auto* virtuPet = dynamic_cast<VirtuPetModule*> (m))
                {
                    // 宠物已不再处于蛋/孵化阶段 → 孵化完成
                    if (! virtuPet->isInEggPhase())
                    {
                        completeTutorial();
                        return;
                    }
                }
            }
        }
    }

    // 如果没有找到 VirtuPet（被用户删除了），则重试 STEP 1
    bool hasVirtuPet = false;
    for (int i = 0; i < workspace->getNumModules(); ++i)
    {
        if (auto* m = workspace->getModule (i))
        {
            if (m->getModuleType() == ModuleType::virtuPet)
            {
                hasVirtuPet = true;
                break;
            }
        }
    }

    if (! hasVirtuPet)
    {
        // 用户删除了 VirtuPet，重新回到 STEP 1
        dismissTutorialOverlay();
        startTutorial();
    }
}

// ----------------------------------------------------------
// 按默认层叠瀑布布局加载七个默认模块（eq / loudness / oscilloscope /
// spectrum / phase / dynamics / waveform）。
//   · 调用方需先清空 workspace（首次启动由构造器保证）
//   · 每个模块相对前一个偏移 (stepX, stepY)，形成老式"弹出大量窗口"的层叠感
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::seedDefaultModules()
{
    static const ModuleType defaultOrder[] = {
        ModuleType::eq,
        ModuleType::loudness,
        ModuleType::vuMeter,
        ModuleType::oscilloscope,
        ModuleType::spectrum,
        ModuleType::phase,
        ModuleType::dynamics,
        ModuleType::waveform,
        ModuleType::spectrogram,
        ModuleType::spectrogram3d
    };

    // workspace 的 canvas 原点（此时 setSize 已触发 resized，canvas 有效）
    const auto canvas = workspace->getLocalBounds();

    // 瀑布布局的行列偏移 / 起点（与模块默认大小无关，保持固定）
    constexpr int stepX          = 28;
    constexpr int stepY          = 28;
    constexpr int startX         = 16;
    constexpr int startY         = 16;

    // 当瀑布到达右/下边缘时，换一列重新从顶部开始（像 XP 那样）
    int x = startX;
    int y = startY;
    int column = 0;

    for (auto type : defaultOrder)
    {
        auto panel = createModule(type);
        if (panel == nullptr) continue;

        // 尺寸：优先使用每个模块自己声明的"默认大小"（setDefaultSize），
        //   再用 minW/minH 做下限保护；这样 EQ / Loudness / Spectrum 等
        //   各自拿到 384×256 / 320×256 / 384×256 …而不是一刀切 340×240。
        const int w = juce::jmax(panel->getMinWidth(),  panel->getDefaultWidth());
        const int h = juce::jmax(panel->getMinHeight(), panel->getDefaultHeight());

        // 如果越界，换到下一列的顶部
        if (x + w > canvas.getRight() - 8 || y + h > canvas.getBottom() - 8)
        {
            ++column;
            x = startX + column * (stepX * 4);
            y = startY;
            // 还是越界就截取到边缘以内
            if (x + w > canvas.getRight() - 8)
                x = juce::jmax(0, canvas.getRight()  - w - 8);
            if (y + h > canvas.getBottom() - 8)
                y = juce::jmax(0, canvas.getBottom() - h - 8);
        }

        panel->setBounds(x, y, w, h);

        // autoPosition=false 让 workspace 保留我们设定的 bounds
        workspace->addModule(std::move(panel), /*autoPosition*/ false);

        x += stepX;
        y += stepY;
    }
}

// ----------------------------------------------------------
// 应用布局预设（由 ModuleWorkspace 的布局下拉框触发）
//   · presetId = 1 (defaultGrid)    → 恢复默认布局 + 默认窗口大小 960×640
//   · presetId = 2 (horizontalFull) → 拉伸顶层窗口到当前屏幕宽，默认模块横向
//                                      等分 canvas，高度撑满 canvas
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::applyLayoutPreset (int presetId)
{
    if (workspace == nullptr) return;

    // 保存所有 VirtuPet 模块的状态（切换预设时保留，不被清除）
    struct VirtuPetState { juce::String roleName; float hunger; float health; };
    juce::Array<VirtuPetState> virtuPetStates;
    for (int i = 0; i < workspace->getNumModules(); ++i)
    {
        auto* m = workspace->getModule (i);
        if (m->getModuleType() == ModuleType::virtuPet)
            if (auto* t = dynamic_cast<VirtuPetModule*> (m))
                virtuPetStates.add ({ t->getRoleName(), t->getHunger(), t->getHealth() });
    }

    // 说明：浮动模块不参与本次重建，也不会被 clearAllModules 清掉 ——
    //   脱离态记录与布局槽完全解耦（见 ModuleWorkspace::saveFloatingModulesTree）。
    //   非 stick 预设切换时，Editor 会先把浮动窗口 dock 回 canvas（既有行为）。

    // 先清空现有模块 / 拼豆贴画（clearAllModules 不触发 onLayoutChanged）
    workspace->clearAllModules();

    if (presetId == 1)
    {
        // Preset 1: 默认布局 + 默认窗口 960×640
        //   · 通过 setSize 触发 resized，workspace 会拿到对应的 canvas 尺寸后
        //     seedDefaultModules 才能正确计算默认瀑布位置。
        //   · 如果当前已经是 960×640（用户刚刚打开），setSize 不会有副作用。
        setSize (960, 640);
        seedDefaultModules();
    }
    else if (presetId == 2 || presetId == 3)
    {
        // Preset 2 / 3: 横向铺满屏幕宽度（高 250px）
        //   · Preset 2 = 贴屏幕顶部 (Y = userArea.getY())
        //   · Preset 3 = 贴屏幕底部 (Y = userArea.getBottom() - targetH)
        //
        //   其余逻辑（宽度取屏幕 userArea 宽、固定高 250、横向等分 7 个默认模块、
        //   放开 resizeLimits）完全相同。之前 Preset 2 累积 *2/3 导致高度不断缩水
        //   的问题已在上一轮改成常量修复。
        //
        //   Y 必须基于 userArea 而不是 top->getY()：Editor 被用户拖到副屏后，
        //   top->getY() 是跨屏绝对坐标（可能是 1440 或负数），而要的是"当前屏的
        //   顶/底边在屏幕坐标中的 Y"——这正是 userArea.getY() / getBottom()。
        const bool bottomAligned = (presetId == 3);

        auto* top = getTopLevelComponent();
        if (top == nullptr) top = this;

        const auto display = juce::Desktop::getInstance()
                                 .getDisplays()
                                 .getDisplayForPoint(top->getScreenBounds().getCentre());
        const auto userArea = resolveDisplayArea ((display != nullptr) ? display->userArea
                                                                       : juce::Rectangle<int> (1280, 720));
        const int screenW = userArea.getWidth();

        // ------------------------------------------------------------
        // 目标高度自动对齐到 8px 网格整数倍 canvas：
        //   期望高度 kHorizontalStripHeight = 250，但 Editor 内 = Y2K 标题栏(26) +
        //   workspace；workspace 内 = canvas + 底部 toolbar(36)。因此：
        //       canvas.height = targetH - 26 - 36 = targetH - 62
        //   250 - 62 = 188，188 % 8 = 4 → canvas 不是整格，7 个等分模块
        //   floor 到 8 倍数后底部会留 4px 空白。
        //
        //   这里不想硬编码"26+36"这类依赖 ModuleWorkspace 内部常量的数值，
        //   改为"先试探一次布局，读取实际 canvas 高度，反推 overheadH"——
        //   这样无论未来 chrome/toolbar 高度怎么变都能自洽。
        //     1) 先按期望高度 setSize，触发 resized() → workspace 拿到 canvas
        //     2) overheadH = targetH - canvas.getHeight()（Editor 里非 canvas 的部分）
        //     3) 调整 targetH 使 (targetH - overheadH) 是 8 的倍数，并尽量靠近 250
        // ------------------------------------------------------------
        // 重置预设 = 布局与条高一起回到默认 250（stick 态下用户点下拉的语义）。
        constexpr int kHorizontalStripHeight = 250;
        int targetH = kHorizontalStripHeight;

        // 关键前置：先把 resizeLimits 放到一个足够宽松的区间，确保后面的
        //   试探 setSize(screenW, 250) 不会被夹回（Editor 默认
        //   setResizeLimits(640, 420, 1600, 1100)：高度下限 420、宽度上限 1600）。
        //   如果试探被夹，probeCanvasH 会变成夹后 Editor 高度对应的 canvas，
        //   反推出来的 overheadH 是错的 → 最终 canvas 高度不是 8 的倍数 →
        //   模块 floor 后底部留 4~6px 空白。
        //   · 下限给一个极小值（例如 kHorizontalStripHeight 本身 250），
        //     上限至少覆盖 screenW × 1200（单屏宽 × 超出极限的高度）。
        if (auto* cbc0 = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc0->getMinimumWidth(),  screenW),
                             juce::jmin (cbc0->getMinimumHeight(), kHorizontalStripHeight),
                             juce::jmax (cbc0->getMaximumWidth(),  screenW),
                             juce::jmax (cbc0->getMaximumHeight(), 1200));
        }

        // 第 1 步：试探布局，让 workspace 计算出当前 chrome 状态下的 canvas 高。
        //   · Standalone 模式（top != this）：直接对顶层窗口 setBounds，
        //     这样 probe 反推的 overhead 天然包含"ResizableBorder 4px 上下边框"
        //     （setResizable(true, false) 下 DocumentWindow 会给 content 挖上下各 4px，
        //     setBoundsInset 压缩 Editor 高度 8px）。若 probe 只对 Editor setSize，
        //     反推的 overheadH 少算这 8px，最终 top->setBounds 走的才是"扣边框"路径，
        //     真实 canvas 比预期少 8px → 模块底部出现 8px 空白。
        //   · 插件模式（top == this）：无法搬动顶层窗口，直接 setSize。
        if (top == this)
        {
            setSize (screenW, targetH);
        }
        else
        {
            top->setBounds (userArea.getX(), userArea.getY(), screenW, targetH);
        }

        // 第 2 步：根据试探结果反推"顶层窗口高度里不属于 canvas 的那部分"。
        //   probeContainerH 是 probe 时真正被我们"占用"的高度：
        //     · standalone：top 的 height（=250，不被 border 吃掉，因为 border 从
        //       top 高度里内扣分给 Editor）
        //     · 插件：Editor 自身 height（=250）
        const int probeContainerH = (top == this) ? getHeight() : top->getHeight();
        const int probeCanvasH    = workspace->getCanvasArea().getHeight();
        const int overheadH       = probeContainerH - probeCanvasH;

        // 第 3 步：取最接近 250-overheadH 的"8 的倍数"作为期望 canvas 高，
        //          再加回 overheadH 得到对齐后的 targetH
        constexpr int kGridForH = 8;
        const int desiredCanvasRaw = kHorizontalStripHeight - overheadH;
        const int desiredCanvasH   = juce::jmax (kGridForH,
                                                 ((desiredCanvasRaw + kGridForH / 2) / kGridForH) * kGridForH);
        targetH = desiredCanvasH + overheadH;

        // 第 4 步：按最终 targetH 锁定 resizeLimits（上下限收紧到 screenW × targetH）
        if (auto* cbc = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  screenW),
                             juce::jmin (cbc->getMinimumHeight(), targetH),
                             juce::jmax (cbc->getMaximumWidth(),  screenW),
                             juce::jmax (cbc->getMaximumHeight(), targetH));
        }

        // 目标 Y：按贴顶/贴底切换
        const int targetY = bottomAligned ? (userArea.getBottom() - targetH)
                                          :  userArea.getY();

        if (top == this)
        {
            // 插件模式（嵌在宿主里）：无法移动顶层窗口，只调整尺寸
            setSize (screenW, targetH);
        }
        else
        {
            // Standalone 模式：连同外层窗口一起搬到目标位置
            top->setBounds (userArea.getX(), targetY, screenW, targetH);
        }

        // setSize / setBounds 会触发 resized()，workspace 随之拿到新 canvas。
        //   此时按横向等分的方式铺默认 7 个模块。
        static const ModuleType horizOrder[] = {
            ModuleType::spectrogram3d,
            ModuleType::lufsRealtime,
            ModuleType::vuMeter,
            ModuleType::stereoField,
            ModuleType::spectrum,
            ModuleType::oscilloscopeWave,
            ModuleType::waveform
        };

        // 使用 workspace->getCanvasArea() 而不是 getLocalBounds()，
        //   后者包含底部 toolbarHeight（36px）与 chrome 控件区。
        const auto canvas = workspace->getCanvasArea();
        const int count    = (int) (sizeof (horizOrder) / sizeof (horizOrder[0]));

        // ============================================================
        // 网格对齐：必须与 ModuleWorkspace::gridSize 保持一致（8 像素）。
        //   原实现直接用 canvas.getWidth()/count 作为 slotW，不是 8 的倍数，
        //   canvas 原点也未必对齐 8；模块落点和大小都偏离网格。
        //   → 一旦用户之后拖动/缩放任意模块，snapToGrid 会把它吸附到最近
        //     的 8 像素位，立刻与相邻模块拉开空隙或错位，"密排列"被破坏。
        //
        //   修复思路：
        //     1) 起点 x0/y0 按 gridSize 向上取整，保证第一个模块左上角在网格上。
        //     2) 可用宽度 usableW 从 canvas.getRight() 向下取整到 gridSize
        //        的倍数后，减去 x0；高度 slotH 同理。
        //     3) 把 usableW 切成 count 份，每份都是 gridSize 的整倍数：
        //        · baseCells = usableW / gridSize / count 个小格
        //        · 剩余 leftoverCells 分摊到前若干个模块各 +1 小格。
        //        这样 7 个模块宽度之和 == usableW，全部在网格上且密排无缝。
        // ============================================================
        constexpr int kGrid = 8;
        auto ceilToGrid  = [kGrid] (int v) { return ((v + kGrid - 1) / kGrid) * kGrid; };
        auto floorToGrid = [kGrid] (int v) { return (v / kGrid) * kGrid; };

        const int x0 = ceilToGrid  (canvas.getX());
        const int y0 = ceilToGrid  (canvas.getY());
        const int xR = floorToGrid (canvas.getRight());
        const int yB = floorToGrid (canvas.getBottom());

        const int usableW = juce::jmax (kGrid * count, xR - x0);
        const int usableH = juce::jmax (kGrid,         yB - y0);

        const int totalCells    = usableW / kGrid;              // 小格数量（8px/格）

        // 加权宽度分配（非均分），按以下比例从左到右：
        //   SPECTROGRAM3D:1.0 | LUFS RT:0.7 | VU:1.0 | STEREO FIELD:1.0
        //   SPECTRUM:1.5 | OSC WAVE:1.0 | WAVEFORM:1.5
        static const float kWidthRatios[] = {
            1.0f, 0.7f, 1.0f, 1.0f, 1.5f, 1.0f, 1.5f
        };
        static constexpr float kTotalRatio = 7.7f;

        int cellsForModule[7] = {};
        int cellsAllocated = 0;
        for (int i = 0; i < count; ++i)
        {
            int cells = (int) std::round ((float) totalCells * kWidthRatios[i] / kTotalRatio);
            cells = juce::jmax (1, cells);
            cellsForModule[i] = cells;
            cellsAllocated += cells;
        }
        // 修正最后一个模块（WAVEFORM）消纳舍入误差
        cellsForModule[count - 1] += totalCells - cellsAllocated;

        const int slotH = juce::jmax (80, usableH);             // 高度占满 canvas 的整网格

        int curX = x0;
        for (int i = 0; i < count; ++i)
        {
            auto panel = createModule (horizOrder[i]);
            if (panel == nullptr) continue;

            const int cellsForThis = cellsForModule[i];
            const int slotW        = cellsForThis * kGrid;

            const int w = juce::jmax (panel->getMinWidth(),  slotW);
            const int h = juce::jmax (panel->getMinHeight(), slotH);
            panel->setBounds (curX, y0, w, h);
            workspace->addModule (std::move (panel), /*autoPosition*/ false);

            curX += slotW;
        }
    }
    else if (presetId == 5)
    {
        // Preset 5 (MV): 全屏 + 上方横向模块条（同 Preset 2） + 下方 Milkdrop 占满
        //
        //   布局：
        //     ┌────────────────────────────────────────────┐
        //     │  SPECT3D | DYN | VU | OSC(L) | SPEC | ...  │  ← 横向模块条 (180px)
        //     ├────────────────────────────────────────────┤
        //     │                                            │
        //     │              Milkdrop 模块                  │  ← 占满剩余 canvas
        //     │                                            │
        //     └────────────────────────────────────────────┘
        //
        //   注意：Windows 下顶层窗口不应走 setFullScreen（会创建独立的全屏 HWND
        //   并改变消息循环，与我们的 auto-hide 边界行为冲突），而是铺满 userArea
        //   （任务栏之外的可用区），视觉等同全屏但技术上仍是普通顶层窗口，
        //   避免 PopupMenu 被 Z 序遮挡。
        //   macOS 下则调用 rw->setFullScreen(true) 进入系统原生全屏（独立 Space、
        //   自动隐藏菜单栏与 Dock），与双击标题栏行为一致；模块布局仍按
        //   totalArea（完整显示器尺寸）计算。

        auto* top = getTopLevelComponent();
        if (top == nullptr) top = this;

        const auto display = juce::Desktop::getInstance()
                                 .getDisplays()
                                 .getDisplayForPoint(top->getScreenBounds().getCentre());
        // 全屏预设的窗口区域按平台区分：
        //   · Windows：使用 userArea（任务栏之外的可用区），而非 totalArea。
        //     totalArea 包含任务栏区域，窗口覆盖整个显示器后 Windows 的
        //     PopupMenu（添加模块/布局预设/音频源下拉）将不可见——因为
        //     PopupMenu 窗口是普通 z-order 窗口，被铺满全屏的主窗口遮挡。
        //     userArea 为任务栏留出空间，确保 PopupMenu 有可见区域。
        //   · macOS：使用 totalArea（完整显示器尺寸）计算模块布局，并在末尾
        //     调用 rw->setFullScreen(true) 进入系统原生全屏（覆盖菜单栏/Dock），
        //     因此这里的 totalArea 仅用于布局，真正隐藏菜单栏/Dock 由原生全屏完成。
#if JUCE_MAC
        const auto realArea = (display != nullptr) ? display->totalArea
                                                  : juce::Rectangle<int>(1280, 720);
#else
        const auto realArea = (display != nullptr) ? display->userArea
                                                  : juce::Rectangle<int>(1280, 720);
#endif
        const auto area = resolveDisplayArea (realArea);
        const int screenW = area.getWidth();
        const int screenH = area.getHeight();

        // -------- 第 1 步：全屏尺寸 ----------
        // 先放开 resizeLimits 到足够宽松，确保 setBounds 不会被 constriner 夹回
        if (auto* cbc0 = getConstrainer())
        {
            setResizeLimits(640, 420,
                            juce::jmax(cbc0->getMaximumWidth(),  screenW),
                            juce::jmax(cbc0->getMaximumHeight(), screenH));
        }

        if (top == this)
        {
            setSize(screenW, screenH);
        }
        else
        {
            top->setBounds(area.getX(), area.getY(), screenW, screenH);
        }

        // -------- 第 2 步：反推 overhead + 分配 canvas ----------
        const int probeContainerH = (top == this) ? getHeight() : top->getHeight();

        // 试探布局让 workspace 初始化 canvas
        const int probeCanvasH = workspace->getCanvasArea().getHeight();
        const int overheadH    = probeContainerH - probeCanvasH;

        // 上方模块条目标高度（与 Horizontal Bar(T) 一致的 250px）
        // 剩下部分全给 Milkdrop
        constexpr int kTopStripHeight = 250;
        const int topStripCanvasH = juce::jmax(80, kTopStripHeight - overheadH);

        // 锁死 resizeLimits 到全屏尺寸（MV 模式下不同尺寸没意义）
        if (auto* cbc = getConstrainer())
        {
            setResizeLimits(screenW, screenH, screenW, screenH);
        }

        // -------- 第 3 步：上方横向模块条（复用 preset 2 的布局逻辑）---------
        const auto canvas = workspace->getCanvasArea();
        constexpr int kGrid = 8;

        static const ModuleType horizOrder[] = {
            ModuleType::spectrogram3d,
            ModuleType::lufsRealtime,
            ModuleType::vuMeter,
            ModuleType::oscilloscope,
            ModuleType::spectrum,
            ModuleType::oscilloscopeWave,
            ModuleType::waveform
        };
        constexpr int kHorizCount = 7;

        static const float kWidthRatios[] = {
            1.0f, 0.7f, 1.0f, 0.7f, 1.5f, 1.0f, 1.5f
        };
        static constexpr float kTotalRatio = 7.4f;

        const int topY   = canvas.getY();
        const int topH   = (topStripCanvasH / kGrid) * kGrid;  // 向下取整到 8px 网格
        const int usableW = (canvas.getWidth()  / kGrid) * kGrid;
        const int x0      = canvas.getX() + (canvas.getWidth() - usableW) / 2;

        const int totalCells = usableW / kGrid;
        int cellsForModule[kHorizCount] = {};
        int cellsAllocated = 0;
        for (int i = 0; i < kHorizCount; ++i)
        {
            int cells = (int)std::round((float)totalCells * kWidthRatios[i] / kTotalRatio);
            cells = juce::jmax(1, cells);
            cellsForModule[i] = cells;
            cellsAllocated += cells;
        }
        cellsForModule[kHorizCount - 1] += totalCells - cellsAllocated;

        int curX = x0;
        for (int i = 0; i < kHorizCount; ++i)
        {
            auto panel = createModule(horizOrder[i]);
            if (panel == nullptr) continue;

            if (horizOrder[i] == ModuleType::oscilloscope)
                if (auto* osc = dynamic_cast<OscilloscopeModule*>(panel.get()))
                    osc->setDisplayMode(OscilloscopeModule::DisplayMode::lissajous);

            const int cellsForThis = cellsForModule[i];
            const int slotW = cellsForThis * kGrid;

            const int w = juce::jmax(panel->getMinWidth(),  slotW);
            const int h = juce::jmax(panel->getMinHeight(), topH);
            panel->setBounds(curX, topY, w, h);
            workspace->addModule(std::move(panel), /*autoPosition*/ false);

            curX += slotW;
        }

        // -------- 第 4 步：下方 Milkdrop 占满剩余空间 ----------
        const int milkY = topY + topH;
        const int milkH = juce::jmax(80, (canvas.getBottom() / kGrid) * kGrid - milkY);

        auto milkdrop = createModule(ModuleType::milkdrop);
        if (milkdrop != nullptr)
        {
            milkdrop->setBounds(x0, milkY, usableW, milkH);
            workspace->addModule(std::move(milkdrop), /*autoPosition*/ false);
        }

        // -------- 第 5 步（macOS）：进入系统原生全屏 ----------
        // 与双击标题栏行为一致：调用 setFullScreen(true) 进入独立 Space，
        // 自动隐藏顶部菜单栏与 Dock。窗口尺寸已在上方按 totalArea 铺满、
        // 模块布局已就绪；此处的原生全屏切换由系统接管，不影响已完成布局。
        // 仅在 Standalone 下触发（插件模式窗口由宿主管理，不接管全屏）。
        // Windows 不进入此分支，保持 userArea 伪最大化。
#if JUCE_MAC
        if (juce::JUCEApplicationBase::isStandaloneApp())
            if (auto* rw = dynamic_cast<juce::ResizableWindow*>(top))
                rw->setFullScreen(true);
#endif
    }
    else if (presetId == 6)
    {
        // Preset 6 (Studio Monitor)：双行监听
        //   上排（频谱行，宽而扁 ≈1.5:1）：Spectrum | Spectrogram | Waveform
        //     · 横向频谱/瀑布拉宽，保证各频率信号强度可读
        //   下排（仪表行 ≈2:1）：VU | StereoField | LUFS RT | TruePeak
        //     · 半圆仪表保持 2:1 避免区域空白
        const auto canvas = resizeToTargetCanvas (1440, 720);

        static const ModuleType topOrder[] = {
            ModuleType::spectrum, ModuleType::spectrogram, ModuleType::waveform
        };
        static const float topRatios[] = { 1.5f, 1.5f, 1.0f };
        constexpr int kTopH = 360;
        const int rowBottom = placeModuleRow (canvas, canvas.getY(), kTopH,
                                              topOrder, 3, topRatios);

        static const ModuleType bottomOrder[] = {
            ModuleType::vuMeter, ModuleType::stereoField,
            ModuleType::lufsRealtime, ModuleType::truePeak
        };
        static const float bottomRatios[] = { 1.0f, 1.0f, 0.8f, 0.8f };
        placeModuleRow (canvas, rowBottom, canvas.getBottom() - rowBottom,
                        bottomOrder, 4, bottomRatios);
    }
    else if (presetId == 7)
    {
        // Preset 7 (Focus + Sidecar)：主视觉 + 右侧仪表列
        //   左侧 65%：Spectrogram（普通瀑布，帧率友好，避免 3D 拉大掉帧）
        //   右侧 35%：VU | StereoField | LUFS RT 竖排（每格 ≈2:1 半圆比例）
        const auto canvas = resizeToTargetCanvas (1280, 720);

        constexpr int kGrid = 8;
        const int x0 = canvas.getX();
        const int y0 = canvas.getY();
        const int xR = (canvas.getRight() / kGrid) * kGrid;
        const int yB = (canvas.getBottom() / kGrid) * kGrid;

        const int mainW = ((xR - x0) * 65) / 100;
        const int sideW = (xR - x0) - mainW;

        auto main = createModule (ModuleType::spectrogram);
        if (main != nullptr)
        {
            main->setBounds (x0, y0, mainW, yB - y0);
            workspace->addModule (std::move (main), false);
        }

        static const ModuleType sideOrder[] = {
            ModuleType::vuMeter, ModuleType::stereoField, ModuleType::lufsRealtime
        };
        const int sideX = x0 + mainW;
        const int sideSlotH = (yB - y0) / 3;
        int curY = y0;
        for (int i = 0; i < 3; ++i)
        {
            auto panel = createModule (sideOrder[i]);
            if (panel != nullptr)
            {
                const int slotH = (i == 2) ? (yB - curY) : sideSlotH;
                panel->setBounds (sideX, curY, sideW, slotH);
                workspace->addModule (std::move (panel), false);
            }
            curY += sideSlotH;
        }
    }
    else if (presetId == 9)
    {
        // Preset 9 (Broadcast Loudness)：广播响度合规
        //   上排（响度读数主视觉）：LUFS RT | TruePeak | Dynamics DR | Dynamics Crest
        //   下排（仪表辅助）：VU | StereoField | Dynamics Meters（半圆仪表 2:1）
        const auto canvas = resizeToTargetCanvas (1280, 620);

        static const ModuleType topOrder[] = {
            ModuleType::lufsRealtime, ModuleType::truePeak,
            ModuleType::dynamicsDr, ModuleType::dynamicsCrest
        };
        static const float topRatios[] = { 1.5f, 1.0f, 1.0f, 1.0f };
        constexpr int kTopH = 300;
        const int rowBottom = placeModuleRow (canvas, canvas.getY(), kTopH,
                                              topOrder, 4, topRatios);

        static const ModuleType bottomOrder[] = {
            ModuleType::vuMeter, ModuleType::stereoField, ModuleType::dynamicsMeters
        };
        static const float bottomRatios[] = { 1.0f, 1.0f, 0.9f };
        placeModuleRow (canvas, rowBottom, canvas.getBottom() - rowBottom,
                        bottomOrder, 3, bottomRatios);
    }
    else if (presetId == 10)
    {
        // Preset 10 (Only Milkdrop)：全屏 + 单个最大化 Milkdrop
        //   窗口铺满 userArea（macOS 原生全屏），canvas 内只放一个
        //   占满整个 canvas 的 Milkdrop 模块。
        auto* top = getTopLevelComponent();
        if (top == nullptr) top = this;

        const auto display = juce::Desktop::getInstance()
                                 .getDisplays()
                                 .getDisplayForPoint (top->getScreenBounds().getCentre());
#if JUCE_MAC
        const auto realArea = (display != nullptr) ? display->totalArea
                                                  : juce::Rectangle<int> (1280, 720);
#else
        const auto realArea = (display != nullptr) ? display->userArea
                                                  : juce::Rectangle<int> (1280, 720);
#endif
        const auto area = resolveDisplayArea (realArea);
        const int screenW = area.getWidth();
        const int screenH = area.getHeight();

        if (auto* cbc0 = getConstrainer())
        {
            setResizeLimits (640, 420,
                             juce::jmax (cbc0->getMaximumWidth(),  screenW),
                             juce::jmax (cbc0->getMaximumHeight(), screenH));
        }

        if (top == this)
            setSize (screenW, screenH);
        else
            top->setBounds (area.getX(), area.getY(), screenW, screenH);

        if (auto* cbc = getConstrainer())
            setResizeLimits (screenW, screenH, screenW, screenH);

        const auto canvas = workspace->getCanvasArea();

        auto milk = createModule (ModuleType::milkdrop);
        if (milk != nullptr)
        {
            milk->setBounds (canvas.getX(), canvas.getY(),
                             canvas.getWidth(), canvas.getHeight());
            workspace->addModule (std::move (milk), false);
        }

#if JUCE_MAC
        if (juce::JUCEApplicationBase::isStandaloneApp())
            if (auto* rw = dynamic_cast<juce::ResizableWindow*>(top))
                rw->setFullScreen(true);
#endif
    }

    // 重新添加 VirtuPet 模块到 canvas 右下角（切换预设时保留宠物不被清除）
    for (const auto& state : virtuPetStates)
    {
        auto virtuPet = std::make_unique<VirtuPetModule>();
        if (state.roleName.isNotEmpty())
            virtuPet->restorePersistentState (state.roleName, state.hunger, state.health);

        const auto canvas = workspace->getCanvasArea();
        constexpr int padding = 8;
        const int petW = virtuPet->getDefaultWidth();
        const int petH = virtuPet->getDefaultHeight();
        virtuPet->setBounds (canvas.getRight()  - petW - padding,
                               canvas.getBottom() - petH - padding,
                               petW, petH);
        workspace->addModule (std::move (virtuPet), false);
    }

    // 手动回写布局到 Processor（clearAllModules + 批量 addModule 会触发多次
    // onLayoutChanged，我们在这里统一再写一次确保最终态被持久化）
    //   · v2.8.5：写入当前活动槽（stick 期间 = 对应的 T/B 槽）
    persistCurrentLayoutToProcessor();

#if JUCE_WINDOWS
    // stick 态重置布局后：条高/窗口位置可能被预设流程调整过，立即把 AppBar
    //   占位与窗口重新对齐，避免工作区出现空档。
    if (stickActive)
        redockStickAfterGeometryChange();
#endif

    // ------------------------------------------------------------------
    // 修复：切换预设（特别是预设 2）后顶层窗口的 alwaysOnTop 会"显示按下但
    // 实际不置顶"的 bug。根因：
    //   · 预设 2 会通过 top->setBounds(...) 大幅改变顶层窗口，Windows 在某些
    //     情况下会把 HWND_TOPMOST 丢掉；
    //   · 但 JUCE Component::setAlwaysOnTop 有早退优化：
    //       if (shouldStayOnTop != flags.alwaysOnTopFlag) { ... }
    //     当我们再次调用 setAlwaysOnTop(true) 时，内部 flag 仍是 true，
    //     方法会直接 return，OS 层的 TopMost 位就不会被重新打上。
    //   · 用户"按两下 pin 按钮"能恢复，就是先 false 再 true 绕开了早退。
    // 解决：这里在布局变更后显式做一次 false→true（仅在当前是置顶态时），
    //      强制让 peer 重新应用 HWND_TOPMOST。
    // ------------------------------------------------------------------
    if (alwaysOnTopActive)
        setAlwaysOnTopActive (true);
}

// ----------------------------------------------------------
// 布局预设辅助：把顶层窗口调整到目标尺寸并返回对齐后的 canvas
//   · 居中于当前屏幕 userArea（standalone）或仅调整 Editor 尺寸（插件）
//   · 放开 resizeLimits 后先试探 setSize/setBounds，反推 overheadH，
//     再把目标高度对齐到 8px 网格并锁定 resizeLimits
// ----------------------------------------------------------
juce::Rectangle<int> Y2KmeterAudioProcessorEditor::resizeToTargetCanvas (int targetW, int targetH)
{
    auto* top = getTopLevelComponent();
    if (top == nullptr) top = this;

    const auto display = juce::Desktop::getInstance()
                             .getDisplays()
                             .getDisplayForPoint (top->getScreenBounds().getCentre());
    const auto userArea = resolveDisplayArea ((display != nullptr) ? display->userArea
                                                                   : juce::Rectangle<int> (1280, 720));

    // 目标尺寸不超过屏幕可用区，并居中放置
    const int w = juce::jmin (targetW, userArea.getWidth());
    const int h = juce::jmin (targetH, userArea.getHeight());
    const int targetX = userArea.getX() + (userArea.getWidth()  - w) / 2;
    const int targetY = userArea.getY() + (userArea.getHeight() - h) / 2;

    // 放开 resizeLimits，确保试探 setBounds 不会被 constrainer 夹回
    if (auto* cbc0 = getConstrainer())
    {
        setResizeLimits (juce::jmin (cbc0->getMinimumWidth(),  w),
                         juce::jmin (cbc0->getMinimumHeight(), h),
                         juce::jmax (cbc0->getMaximumWidth(),  w),
                         juce::jmax (cbc0->getMaximumHeight(), h));
    }

    // 第 1 步：试探布局（standalone 对顶层窗口 setBounds，含 4px 边框；
    //          插件模式无法搬动顶层窗口，直接 setSize）
    if (top == this)
        setSize (w, h);
    else
        top->setBounds (targetX, targetY, w, h);

    // 第 2 步：反推"顶层窗口高度里不属于 canvas 的部分"
    const int probeContainerH = (top == this) ? getHeight() : top->getHeight();
    const int probeCanvasH    = workspace->getCanvasArea().getHeight();
    const int overheadH       = probeContainerH - probeCanvasH;

    // 第 3 步：把目标高度对齐到 8px 网格
    constexpr int kGrid = 8;
    const int desiredCanvasH = juce::jmax (kGrid,
                                           ((h - overheadH + kGrid / 2) / kGrid) * kGrid);
    const int alignedH = desiredCanvasH + overheadH;

    // 第 4 步：锁定 resizeLimits 到最终尺寸
    if (auto* cbc = getConstrainer())
    {
        setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  w),
                         juce::jmin (cbc->getMinimumHeight(), alignedH),
                         juce::jmax (cbc->getMaximumWidth(),  w),
                         juce::jmax (cbc->getMaximumHeight(), alignedH));
    }

    // 第 5 步：应用最终尺寸
    if (top == this)
        setSize (w, alignedH);
    else
        top->setBounds (targetX, targetY, w, alignedH);

    return workspace->getCanvasArea();
}

// ----------------------------------------------------------
// 布局预设辅助：在 canvas 内横向摆放一行模块（加权宽度 + 网格对齐）
//   · ratios 为 nullptr 时均分；否则按 ratios[i] / sum(ratios) 分配宽度
//   · 所有模块左边界从 canvas 网格对齐点开始，密排占满整行
//   · 返回该行的实际底部 Y
// ----------------------------------------------------------
int Y2KmeterAudioProcessorEditor::placeModuleRow (juce::Rectangle<int> canvas,
                                                  int rowY, int rowH,
                                                  const ModuleType* order, int count,
                                                  const float* ratios)
{
    constexpr int kGrid = 8;
    auto ceilToGrid  = [kGrid] (int v) { return ((v + kGrid - 1) / kGrid) * kGrid; };
    auto floorToGrid = [kGrid] (int v) { return (v / kGrid) * kGrid; };

    const int x0 = ceilToGrid  (canvas.getX());
    const int y0 = ceilToGrid  (rowY);
    const int xR = floorToGrid (canvas.getRight());
    const int yB = floorToGrid (canvas.getBottom());

    const int usableW = juce::jmax (kGrid * count, xR - x0);
    const int rowHGrid = juce::jmax (kGrid, (rowH / kGrid) * kGrid);
    const int usableH = juce::jmax (kGrid, juce::jmin (rowHGrid, yB - y0));

    const int totalCells = usableW / kGrid;

    // 计算每个模块分配的网格小格数（按加权比例）
    float totalRatio = 0.0f;
    for (int i = 0; i < count; ++i)
        totalRatio += (ratios != nullptr) ? ratios[i] : 1.0f;

    juce::Array<int> cellsForModule;
    cellsForModule.resize (count);
    int cellsAllocated = 0;
    for (int i = 0; i < count; ++i)
    {
        const float ratio = (ratios != nullptr) ? ratios[i] : 1.0f;
        int cells = (int) std::round ((float) totalCells * ratio / totalRatio);
        cells = juce::jmax (1, cells);
        cellsForModule.set (i, cells);
        cellsAllocated += cells;
    }
    // 最后一个模块消纳舍入误差
    cellsForModule.set (count - 1,
                        cellsForModule[count - 1] + (totalCells - cellsAllocated));

    const int slotH = juce::jmax (80, usableH);

    int curX = x0;
    for (int i = 0; i < count; ++i)
    {
        auto panel = createModule (order[i]);
        if (panel == nullptr)
        {
            curX += cellsForModule[i] * kGrid;
            continue;
        }

        const int slotW = cellsForModule[i] * kGrid;
        const int w = juce::jmax (panel->getMinWidth(),  slotW);
        const int h = juce::jmax (panel->getMinHeight(), slotH);
        panel->setBounds (curX, y0, w, h);
        workspace->addModule (std::move (panel), /*autoPosition*/ false);

        curX += slotW;
    }

    return y0 + slotH;
}

// ----------------------------------------------------------
// 工厂方法
// ----------------------------------------------------------
std::unique_ptr<ModulePanel> Y2KmeterAudioProcessorEditor::createModule(ModuleType type)
{
    switch (type)
    {
        case ModuleType::eq:
            return std::make_unique<EqModule>(processor);
        case ModuleType::loudness:
            return std::make_unique<LoudnessModule>(processor.getAnalyserHub());
        case ModuleType::oscilloscope:
            return std::make_unique<OscilloscopeModule>(processor.getAnalyserHub());
        case ModuleType::spectrum:
            return std::make_unique<SpectrumModule>(processor.getAnalyserHub());
        case ModuleType::phase:
            return std::make_unique<PhaseModule>(processor.getAnalyserHub());
        case ModuleType::dynamics:
            return std::make_unique<DynamicsModule>(processor.getAnalyserHub());

        case ModuleType::lufsRealtime:
            return std::make_unique<LufsRealtimeModule>(processor.getAnalyserHub());
        case ModuleType::truePeak:
            return std::make_unique<TruePeakModule>(processor.getAnalyserHub());
        case ModuleType::oscilloscopeWave:
            return std::make_unique<OscilloscopeWaveModule>(processor.getAnalyserHub());
        case ModuleType::phaseCorrelation:
            return std::make_unique<PhaseCorrelationModule>(processor.getAnalyserHub());
        case ModuleType::phaseBalance:
            return std::make_unique<PhaseBalanceModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsMeters:
            return std::make_unique<DynamicsMetersModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsDr:
            return std::make_unique<DynamicsDrModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsCrest:
            return std::make_unique<DynamicsCrestModule>(processor.getAnalyserHub());

        case ModuleType::waveform:
            return std::make_unique<WaveformModule>(processor.getAnalyserHub());

        case ModuleType::vuMeter:
            return std::make_unique<VuMeterModule>(processor.getAnalyserHub());

        case ModuleType::spectrogram:
            return std::make_unique<SpectrogramModule>(processor.getAnalyserHub());

        case ModuleType::spectrogram3d:
            return std::make_unique<Spectrogram3DModule>(processor.getAnalyserHub());

        case ModuleType::stereoField:
            return std::make_unique<StereoFieldModule>(processor.getAnalyserHub());

        case ModuleType::virtuPet:
            return std::make_unique<VirtuPetModule>();

        case ModuleType::milkdrop:
            return std::make_unique<MilkdropModule>(&processor.getAnalyserHub(), this);

        default:
            jassertfalse; // 暂未实现
            return nullptr;

    }
}

// ============================================================
// 插件模式（VST3 / AU / AAX）下的预设 Save —— 把当前 Processor state
// 打包成一个与 Standalone 完全兼容的 <PROPERTIES> XML 文件
//
// 文件结构（与 JUCE PropertiesFile 在磁盘上的 XML 格式对齐）：
//   <?xml version="1.0" ...?>
//   <PROPERTIES>
//     <VALUE name="filterState" val="BASE64(processor state binary)"/>
//   </PROPERTIES>
//
// 这样：
//   · 用户在 VST3 里 Save 出来的 .settings，Standalone 下次启动时 Load
//     能原样恢复（Standalone 的 reloadPluginState 会读 filterState）；
//   · Standalone 里 Save 出来的 .settings，VST3 这边 Load 也能正确解析
//     （loadStateFromSettingsFile 会识别同样的 <PROPERTIES>/filterState 结构）。
//
// 注：插件模式下我们**有意不**写入主题 / 窗口 bounds / 音频源 等 Standalone
// 专属字段 —— 这些在 VST3 场景下的语义与 Standalone 完全不同（窗口由宿主管、
// 主题由插件内独立持久化）。如果用户用 VST3 存的文件里缺这些键，Standalone
// 首次 Load 时会按其默认值兜底，不会报错。
// ============================================================
void Y2KmeterAudioProcessorEditor::saveStateAsSettingsFile (const juce::File& dest)
{
    // 1) 先把"当前 workspace 的实时布局"回写到 Processor，保证 getStateInformation
    //    拿到的是用户眼睛看到的最新状态（onLayoutChanged 是异步节流的，不能指望
    //    点击 Save 的那一瞬间刚好已经被刷到 Processor）
    if (workspace != nullptr)
        persistCurrentLayoutToProcessor();

    // 2) 导出 Processor state（内部是 XML 的二进制化形式）
    juce::MemoryBlock stateBlock;
    processor.getStateInformation (stateBlock);

    // 3) 构造 <PROPERTIES> 根节点 + 一个 filterState VALUE 子节点（base64）
    juce::XmlElement props ("PROPERTIES");
    auto* v = props.createNewChildElement ("VALUE");
    v->setAttribute ("name", "filterState");
    v->setAttribute ("val",  stateBlock.toBase64Encoding());

    // 4) 原子化写入目标文件（先写 TempFile，成功后 rename；避免半截文件）
    dest.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (dest);
    if (! props.writeTo (tmp.getFile()))
    {
        DBG ("saveStateAsSettingsFile: write temp failed");
        return;
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        DBG ("saveStateAsSettingsFile: replace failed → " + dest.getFullPathName());
    }
}

// ============================================================
// 插件模式下的预设 Load —— 把用户选择的 .settings 文件解析出 Processor state
// 并就地热重载（不重启宿主）。容错三种来源：
//   a) Standalone / 本 Save 写出的 <PROPERTIES> + filterState (base64) 结构；
//   b) 裸 <PBEQ_State ...> XML（即 Processor::getStateInformation 的原生 XML 形态）；
//   c) 裸 <PBEQ_Layout ...> XML（仅包含布局，无其他元信息）。
// 优先级 a > b > c。任一成功即可。
// ============================================================
void Y2KmeterAudioProcessorEditor::loadStateFromSettingsFile (const juce::File& src)
{
    if (! src.existsAsFile()) return;

    auto xml = juce::parseXML (src);
    if (xml == nullptr)
    {
        DBG ("loadStateFromSettingsFile: not a valid XML → " + src.getFullPathName());
        return;
    }

    // ---- 分支 a：<PROPERTIES> 外壳，含 filterState base64 -------
    if (xml->hasTagName ("PROPERTIES"))
    {
        // 在 VALUE 子节点里找 name="filterState" 的那一个
        const juce::XmlElement* filterVal = nullptr;
        for (auto* child : xml->getChildWithTagNameIterator ("VALUE"))
        {
            if (child != nullptr && child->getStringAttribute ("name") == "filterState")
            {
                filterVal = child;
                break;
            }
        }
        if (filterVal != nullptr)
        {
            const auto b64 = filterVal->getStringAttribute ("val");
            juce::MemoryBlock block;
            if (block.fromBase64Encoding (b64) && block.getSize() > 0)
            {
                // 反序列化回 Processor —— 内部会更新三槽布局
                processor.setStateInformation (block.getData(), (int) block.getSize());
                // 立刻把新布局灌到 workspace（不用等宿主再开关一次 Editor）
                //   v2.8.5：装载"当前活动槽"（stick 态装载对应一侧，否则非 stick 槽）
                if (workspace != nullptr)
                {
                    auto layoutXml = processor.getSavedLayoutXml (
                        static_cast<Y2KmeterAudioProcessor::LayoutSlot> (activeLayoutSlot()));

                    if (layoutXml.isEmpty())
                        layoutXml = processor.getSavedLayoutXml (Y2KmeterAudioProcessor::LayoutSlot::normal);

                    if (layoutXml.isNotEmpty())
                        workspace->loadLayoutFromXml (layoutXml);
                }

                // 脱离态模块是独立存档：导入后重新装载记录并补齐浮动窗口
                reloadFloatingModuleRecordsFromProcessor();
                restoreFloatingWindowsFromWorkspace();

                // v2.8.5：存档里的 stickActive 描述的是导出方当时的状态，
                //   按本机会话的实际情况校正，避免下次启动莫名进入 stick。
                syncStickSessionFlagToProcessor();
                return;
            }
        }
        DBG ("loadStateFromSettingsFile: <PROPERTIES> without valid filterState");
        // 不 return，允许继续尝试把整文件当作其它格式（极少见）
    }

    // ---- 分支 b：裸 PBEQ_State XML -------
    if (xml->hasTagName ("PBEQ_State"))
    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*xml, block);
        if (block.getSize() > 0)
        {
            processor.setStateInformation (block.getData(), (int) block.getSize());
            if (workspace != nullptr)
            {
                auto layoutXml = processor.getSavedLayoutXml (
                    static_cast<Y2KmeterAudioProcessor::LayoutSlot> (activeLayoutSlot()));

                if (layoutXml.isEmpty())
                    layoutXml = processor.getSavedLayoutXml (Y2KmeterAudioProcessor::LayoutSlot::normal);

                if (layoutXml.isNotEmpty())
                    workspace->loadLayoutFromXml (layoutXml);
            }

            reloadFloatingModuleRecordsFromProcessor();
            restoreFloatingWindowsFromWorkspace();

            // v2.8.5：同分支 a，按本机会话实际情况校正 stick 标记
            syncStickSessionFlagToProcessor();
            return;
        }
    }

    // ---- 分支 c：裸 PBEQ_Layout XML -------
    if (xml->hasTagName ("PBEQ_Layout"))
    {
        const auto layoutXml = xml->toString (juce::XmlElement::TextFormat{}.singleLine());
        // 同步写回 Processor 的当前活动槽，以便下次 getStateInformation 能拿到
        processor.setSavedLayoutXml (
            static_cast<Y2KmeterAudioProcessor::LayoutSlot> (activeLayoutSlot()), layoutXml);
        if (workspace != nullptr)
        {
            // 旧版布局文件里可能仍带 floating 节点 → 迁移成独立浮动记录
            workspace->loadLayoutFromXml (layoutXml, true);
            restoreFloatingWindowsFromWorkspace();
        }
        return;
    }

    DBG ("loadStateFromSettingsFile: unknown root tag = " + xml->getTagName());
}

void Y2KmeterAudioProcessorEditor::invalidateDesktopCache() noexcept
{
    // 除置 dirty 外，直接把缓存图与 bounds 清空：
    //   paint 里的判定（isValid && bounds 匹配）会失败 → 走 drawDesktop 直接
    //   绘制分支（实时读全局色）→ 下一帧 rebuildDesktopCacheIfNeeded 重建缓存。
    //   这样即使 dirty 标志因任何原因丢失，桌面也不会停留在旧主题的图。
    desktopCacheDirty = true;
    desktopCacheImage = {};
    desktopCacheBounds = {};
}

void Y2KmeterAudioProcessorEditor::rebuildDesktopCacheIfNeeded()
{
    // P6 · 跨实例共享桌面纹理：
    //   之前按"每个 Editor 实例"各自维护一张 desktopCacheImage，
    //   并且 macOS 分支因顾虑 legacy NSOpenGLContext 的纹理 upload 开销
    //   选择**每帧重绘** drawDesktop / drawLogo 的循环图样——多实例场景
    //   （DAW 同时加载 N 份）下这段循环被放大 N 倍，是 UI 线程热点。
    //
    //   P0 已不再 attach OpenGLContext 到 mac，软光栅 drawImageAt 走的是
    //   CoreGraphics blit，比每帧重新画循环图样便宜一个数量级；因此 mac
    //   分支也启用共享 cache，并把底图 Image 抽到 PinkXP 进程级 weak-ref
    //   共享池（getSharedDesktopTexture），多实例 + 同尺寸时零重复绘制。
    //   Logo 仍由各实例按自己的 logoImage 独立叠加（中心区单次 drawImage，
    //   成本可忽略）。
    const auto bounds = workspace != nullptr ? workspace->getBounds() : getLocalBounds();
    if (bounds.isEmpty())
    {
        desktopCacheImage = {};
        desktopCacheBounds = {};
        desktopCacheDirty = false;
        return;
    }

    if (! desktopCacheDirty
        && desktopCacheImage.isValid()
        && desktopCacheBounds == bounds)
        return;

    // 1) 从进程级共享池取（或新建）底图 —— 多个 Editor 同尺寸时共用像素数据
    juce::Image sharedBase = PinkXP::getSharedDesktopTexture (bounds.getWidth(),
                                                              bounds.getHeight());

    Y2K_LOG ("[palette] rebuild desktop cache: dirty="
             + juce::String (desktopCacheDirty ? "yes" : "no")
             + " sharedValid=" + juce::String (sharedBase.isValid() ? "yes" : "no")
             + " pixel00=" + (sharedBase.isValid()
                                  ? sharedBase.getPixelAt (0, 0).toString()
                                  : juce::String ("(invalid)"))
             + " bounds=" + juce::String (bounds.getWidth()) + "x"
             + juce::String (bounds.getHeight()));

    if (! sharedBase.isValid())
    {
        // 兜底：共享池失败时退回原有路径
        desktopCacheImage = {};
        desktopCacheBounds = {};
        desktopCacheDirty = false;
        return;
    }

    // 2) 本实例的 cache Image = 共享底图 + 居中 logo 叠加
    //    由于 logo 位置/尺寸依赖 bounds，无法跨实例共用最终图；
    //    但共享部分已经覆盖了 drawDesktop 所有重复循环（最大头）。
    desktopCacheImage = juce::Image (juce::Image::RGB,
                                     bounds.getWidth(),
                                     bounds.getHeight(),
                                     false);
    desktopCacheBounds = bounds;
    desktopCacheDirty = false;

    juce::Graphics cacheGraphics (desktopCacheImage);
    cacheGraphics.drawImageAt (sharedBase, 0, 0);
    // drawLogo 期望 area 是"目标绘制矩形"，而 cache Image 以 (0,0) 起点，
    // 因此传递一个左上角为 (0,0) 的等尺寸矩形。
    PinkXP::drawLogo (cacheGraphics,
                      juce::Rectangle<int>(0, 0, bounds.getWidth(), bounds.getHeight()),
                      logoImage);
}

// ----------------------------------------------------------
// 绘制：方案乙（完全铺满）
//   · 顶部 TitleBar（26px，Pink XP 风格抬头；含 Logo + 标题文字 + 右侧 × 按钮）
//   · 中部 Workspace（半透明），下面铺桌面棋盘纹理 + 中央 logo，作为"透过来的纹理背景"
//   · 底部 Toolbar（由 ModuleWorkspace 自己画）
//   关闭按钮嵌入 TitleBar 最右侧；chrome 隐藏时 TitleBar + × 均按 chromeAlpha 半透明
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::paint(juce::Graphics& g)
{
#ifdef Y2KMETER_MILKDROP_ONLY
    // Milkdrop-only：整个窗口由 projectM 帧（Editor GL 渲染）填充，
    // 不画桌面纹理 / 标题栏。
    juce::ignoreUnused (g);
    return;
#endif
    // 1) 桌面纹理底图：只在 workspace 矩形范围内绘制
    //    workspace 是半透明的，纹理会作为模块背后的视觉肌理透出来
    if (workspace != nullptr)
    {
        const auto wsBounds = workspace->getBounds();
        if (! wsBounds.isEmpty())
        {
            rebuildDesktopCacheIfNeeded();

            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (wsBounds);

            if (desktopCacheImage.isValid() && desktopCacheBounds == wsBounds)
                g.drawImageAt (desktopCacheImage, wsBounds.getX(), wsBounds.getY());
            else
            {
                PinkXP::drawDesktop (g, getLocalBounds());
                PinkXP::drawLogo    (g, wsBounds, logoImage);
            }
        }
    }

    // 2) 顶部 TitleBar + 右上角按钮组
    //    · chrome 可见：正常绘制完整标题栏 + 三个按钮
    //    · chrome 隐藏：整个标题栏消失，只在右上角绘制一个半透明小关闭浮标
    //      （样式参照底部 Hide 按钮：未悬停时 15%、悬停时 100%）
    //    · 插件宿主模式（VST3 等）：也画"精简抬头"——只绘制背景 + Logo +
    //      "Y2Kmeter v1.1 iisaacbeats.cn"文字；不画右侧最小化/固定/关闭按钮
    //      （宿主窗口已经提供系统级边框/关闭按钮，伪按钮会产生两套 UI 打架）。
    //    · Hide 态：标题栏不占位；悬停浮层时由 paintOverChildren 画在模块之上。
    if (! chromeDim)
        drawTitleBar (g);
    // chrome 隐藏态（未悬停）的浮动关闭按钮 + 抬头文字由 ChromeHiddenOverlay 处理
}

void Y2KmeterAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    // v2.8.2：Hide 悬停浮层 —— 标题栏盖在所有子组件（模块）之上，窗口几何不变
    if (chromeDim && chromeRevealed)
        drawTitleBar (g);
}

void Y2KmeterAudioProcessorEditor::drawTitleBar (juce::Graphics& g)
{
    {
        auto tb = getTitleBarBounds();

        // 标题栏背景（Pink XP 风格 —— 粉色渐变 + 像素高光/阴影）
        //   文字留给我们自己画，这样"软件名 + 版本号 + 官网"可以拥有不同字号 / 下划线
        PinkXP::drawPinkTitleBar (g, tb, juce::String(), 12.0f);

        // 左侧 Logo 图标：drawPinkTitleBar 已保留最左侧 24px 给 icon 区域，
        //   我们继续在 icon 右侧顺延绘制标题文字，所以 textArea 的 x 从 28 开始。
        //   标题布局：[Y2Kmeter] [空格] [v1.1 小字] [空格-空格] [iisaacbeats.cn 小字]
        auto textArea = getTitleTextBounds();

        // 主标题 "Y2Kmeter"
        const juce::String nameText    = "Y2Kmeter";
        const juce::String versionText = "v2.9.1";
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW    = nameFont.getStringWidth (nameText);
                const int versionW = versionFont.getStringWidth ("v2.9.1");
        const int urlW     = urlFont.getStringWidth (urlText);

        constexpr int gap1 = 6;   // name ↔ version 之间
        constexpr int gap2 = 10;  // version ↔ url 之间

        const int y  = textArea.getY();
        const int h  = textArea.getHeight();
        const int x0 = textArea.getX();

        // 文字阴影（1,1）
        g.setColour (PinkXP::sel.darker(0.55f));
        g.setFont (nameFont);
        g.drawText (nameText, x0 + 1, y + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1 + 1, y + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2 + 1, y + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（白色）
        g.setColour (PinkXP::selInk);
        g.setFont (nameFont);
        g.drawText (nameText, x0, y, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1, y, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2, y, urlW, h, juce::Justification::centredLeft, false);

        // hover 时在可点击区域（name + version + url 整段）画下划线
        if (titleTextHovered)
        {
            const int lineY = y + h - 3;
            const int totalW = nameW + gap1 + versionW + gap2 + urlW;
            g.setColour (PinkXP::selInk);
            g.fillRect (x0, lineY, totalW, 1);
        }

        // 缓存实际绘制宽度，供 mouseMove / mouseDown 精确命中（参考 titleTextHovered）
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;

        // 标题栏下沿 1px 深色分割线（与 ModulePanel 视觉一致）
        g.setColour (PinkXP::dark);
        g.fillRect (tb.getX(), tb.getBottom(), tb.getWidth(), 1);

        // 3) 右上角三个按钮（从右到左：关闭 × / 固定 ★ / 最小化 _ ）
        //    · 插件宿主模式（VST3 等）：完全不画这三个按钮 —— 宿主自带系统级
        //      最小化/关闭按钮，我们只保留"软件名 + 版本号 + 官网"文字抬头。
        //    样式完全参考 XP：drawRaised + hover 粉色 + pressed 凹陷
        if (! isPluginHost)
        {
            auto drawTitleBtn = [&] (juce::Rectangle<int> rc,
                                     bool hovered, bool pressed, bool activeLatched,
                                     const juce::String& glyph,
                                     float fontHeight,
                                     int glyphDxWhenIdle,
                                     int glyphDyWhenIdle)
            {
                // Pin 按钮激活（alwaysOnTop=true）时呈现"凹陷锁定"视觉
                if (pressed || activeLatched)
                    PinkXP::drawPressed (g, rc, activeLatched ? PinkXP::pink300 : PinkXP::pink100);
                else
                    PinkXP::drawRaised  (g, rc, hovered ? PinkXP::pink200 : PinkXP::btnFace);

                g.setColour (PinkXP::ink);
                g.setFont   (PinkXP::getFont (fontHeight, juce::Font::bold));
                auto txt = rc;
                txt.translate (glyphDxWhenIdle, glyphDyWhenIdle);
                if (pressed || activeLatched) txt.translate (1, 1);
                g.drawText (glyph, txt, juce::Justification::centred, false);
            };

            // 3a-lock) 布局锁定按钮（v1.8.3 新增）："L" 作为 Lock 的助记符号
            //   · 之所以选 "L" 而非 Unicode 锁头符 🔒 是因为项目内自制像素字体 PinkXP
            //     未包含 Emoji 字形，直接绘会回退到系统字体，与周围三个按钮不归一。
            //   · pressed（点击瞬间凹陷）与 activeLatched=layoutLocked（长按锁定态凹陷）
            //     两条视觉路径复用同一 pressedLook，切换手感与 Pin 按钮一致。
            drawTitleBtn (getLockButtonBounds(),
                          lockButtonHovered, lockButtonPressed, layoutLocked,
                          "L", 12.0f, -1, -2);

            // 3a) 最小化按钮 + 3b) 固定（置顶）按钮
            //   stick 模式（AppBar 驻留）下隐藏：驻留态宽度被系统锁死、
            //   高度可调，置顶/最小化语义不再适用（对齐 MiniMeters 的 stick）。
            if (! stickActive)
            {
                drawTitleBtn (getMinimiseButtonBounds(),
                              minButtonHovered, minButtonPressed, false,
                              "_", 12.0f, -1, -3);

                drawTitleBtn (getPinButtonBounds(),
                              pinButtonHovered, pinButtonPressed, alwaysOnTopActive,
                              "*", 14.0f, -1, -2);
            }

            // 3c) 关闭按钮："x"（字体里 'x' 位置正常，仅 y=-1 做微调）
            drawTitleBtn (getCloseButtonBounds(),
                          closeButtonHovered, closeButtonPressed, false,
                          "x", 12.0f, -1, -1);
        }
    }
}

// ----------------------------------------------------------
// 布局：方案乙（完全铺满）
//   titleBar (26px) | workspace（自带底部 toolbar 36px） —— 三段占满整个 Editor
//   workspace 内部的 toolbar 会自动占据最底部 36px（由 ModuleWorkspace 处理）
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::resized()
{
    // 注意：这里**不再**无条件 invalidateDesktopCache()。
    //   · rebuildDesktopCacheIfNeeded() 内部已经做了 bounds 对比，workspace 尺寸
    //     真正变化时会自己走重建分支；
    //   · 模块冒前 / 拖动 / hide-chrome 过渡等场景会触发 Editor::resized()，但
    //     workspace.getBounds() 通常不变，没必要把整张背景纹理扔掉重传 GPU。
    //   · 主题切换仍会在 themeSubscribe 回调里显式调用 invalidateDesktopCache()。
    //   · 这一改动对 Windows 路径无影响（行为等价），对 macOS 能避免每次 resize
    //     触发整张 workspace 尺寸 RGB Image 重建 + legacy NSOpenGLContext 的同步
    //     纹理 upload，是 macOS 多模块卡顿的关键修复之一。

    auto r = getLocalBounds();
    // chrome 可见时顶部让给 TitleBar；chrome 隐藏时让 workspace 占满整个窗口，
    //   浮层 overlay 作为"最底层 child"铺在顶部 titleBarHeight 区域，会被模块自然遮挡。
    // 插件宿主模式（VST3 等）：也为标题栏预留 titleBarHeight —— 我们会画一个
    //   "精简抬头"（只有软件名 + 版本号 + 官网文字，无右侧最小化/固定/关闭按钮），
    //   宿主窗口已提供自己的系统标题栏和边框，不会与此抬头冲突。
#ifndef Y2KMETER_MILKDROP_ONLY
    if (! chromeDim)
        r.removeFromTop (PinkXP::ui(titleBarHeight));
#endif
    workspace->setBounds (r);

    // 浮层固定在顶部，与 TitleBar 同尺寸（Editor 同宽 × titleBarHeight）。
    if (chromeHiddenOverlay != nullptr)
        chromeHiddenOverlay->setBounds (0, 0, getWidth(), PinkXP::ui(titleBarHeight));

    // 教程覆盖层始终铺满整个 Editor
    if (tutorialOverlay != nullptr && tutorialOverlay->isVisible())
        tutorialOverlay->setBounds (getLocalBounds());

    // 同步 workspace 的 hit-test 挖洞（按钮位置依赖 getWidth()，resize 后必须重新计算）
    updateWorkspaceHitTestHoles();

    // Hide 悬停浮层：标题栏条带随窗口宽度变化，穿透矩形同步刷新
    if (chromeRevealed && workspace != nullptr)
        workspace->setOverlayPassThroughRect (getTitleBarBounds().withTrimmedBottom (-1)
                                              - workspace->getPosition());

    // 插件模式下实时把 Editor 尺寸写回 Processor：host 调用 getStateInformation
    //   时会把最新尺寸一起序列化，下一次重新打开编辑器窗口时即可恢复。
    //   · Standalone 不走此路径（PropertiesFile 体系自行处理位置+尺寸）；
    //   · 析构期间 (editorBeingDestructed) 不再写回：FL Studio 等宿主在关闭
    //     嵌入子窗口的瞬间会先把插件视图 resize 到 ~175×85 这种极小中间态
    //     （目的是让窗口先从屏幕上隐形），若此时回写到 savedEditorSize，下次
    //     重新打开插件窗口就会恢复成这个异常小尺寸；
    //   · 写入下限提到 400×300：该阈值大于任何正常可用的最小 Editor 尺寸
    //     （标题栏 26 + toolbar 36 + 至少一行模块 ≈ 需要 300+ 高度），但又
    //     远小于构造默认值 960×640，起到"防止宿主偷偷塞小尺寸"的护栏作用，
    //     同时不会阻碍用户主动把窗口拉到合理的较小尺寸。
    if (isPluginHost && ! editorBeingDestructed)
    {
        constexpr int kMinPersistW = 400;
        constexpr int kMinPersistH = 300;

        const int w = getWidth();
        const int h = getHeight();
        if (w >= kMinPersistW && h >= kMinPersistH)
            processor.setSavedEditorSize (w, h);
    }
}

void Y2KmeterAudioProcessorEditor::parentHierarchyChanged()
{
    // --------------------------------------------------------------------
    // Windows VST3 卡死修复 —— 强制使用软件渲染引擎
    //
    // 背景（基于 dump 栈实证）：
    //   JUCE 8 在 Windows 默认启用 Direct2D 渲染器；Direct2D 用
    //   SharedResourcePointer<DxgiAdapters>（DLL 级 static shared_ptr）缓存
    //   D3D11/DXGI 设备。当 FL Studio / 某些 DAW 刷新/删除 VST3 时会调用
    //   FreeLibrary，CRT 在 DLL_PROCESS_DETACH 路径（持有 loader lock 的上下文）
    //   里执行 atexit 析构链，触发 DxgiAdapters 析构 → ID3D11Device 释放 →
    //   AMD 驱动 atidxx64.dll::CDevice::DestroyDriverInstance 内部 SleepEx
    //   等待 GPU fence / worker thread，而 loader lock 又阻塞了那些线程的推进，
    //   主线程从此无限 Sleep（= 进程未响应 / 宿主卡死）。
    //
    //   解决方案：在 peer 刚拿到时把渲染引擎切到 Software（index 0），
    //   让 Direct2D 全局资源根本不被构造，DLL 卸载链路上就没有 AMD 驱动销毁
    //   环节，彻底避开 loader lock 死等。
    //
    // 限定范围：
    //   · 仅 Windows 生效（mac/Linux 没有该问题）。
    //   · peer 可用且有 Direct2D 可选项时才切换（少数旧系统仅 Software 一档，
    //     此时 setCurrentRenderingEngine 是 no-op）。
    //   · 只做一次：首次切换成功后 renderingEngineConfigured 置 true，后续
    //     parentHierarchyChanged 不再重复（避免用户手动切回 Direct2D 后被覆盖）。
    //
    // 代价：Windows 下 UI 由 GDI/软件光栅绘制，性能略降。对当前插件（UI 主要为
    //   低频仪表 + 频谱小图，且我们另外开启了 OpenGLContext 兜底）几乎无感知。
    // --------------------------------------------------------------------
   #if JUCE_WINDOWS
    if (! renderingEngineConfigured)
    {
        if (auto* peer = getPeer())
        {
            const auto engines = peer->getAvailableRenderingEngines();
            if (engines.size() > 1)
            {
                peer->setCurrentRenderingEngine (0); // 0 = Software Renderer
            }
            renderingEngineConfigured = true;
        }
    }
   #endif

    juce::AudioProcessorEditor::parentHierarchyChanged();
}

void Y2KmeterAudioProcessorEditor::visibilityChanged()
{
    processor.setAnalysisActive(true);

    // 低分辨率适配：首次上屏时按所在显示器分辨率计算 UI 密度缩放，
    // 让标题栏 / 工具栏 / 模块 chrome 在低分辨率下更紧凑。
    updateUiScaleForCurrentDisplay();

    // 默认启用"固定窗口置顶"（alwaysOnTopActive 初始 true）：
    //   · 仅在 Standalone 下应用（插件模式下顶层是宿主窗口，setAlwaysOnTop
    //     会影响宿主行为，且多数 DAW 会忽略该调用，但为避免副作用，限定
    //     Standalone 场景）。
    //   · visibilityChanged 是 getTopLevelComponent() 已指向实际顶层 Y2KMainWindow
    //     的最早时机；构造器里调 getTopLevelComponent() 可能还是 Editor 自己。
    //   · 只在首次 visibilityChanged 做一次，避免每次隐藏/显示时反复强推。
    //   · 通过 setAlwaysOnTopActive() 强推（内部做 false→true 绕开 JUCE 早退），
    //     防止"按钮显按下但实际未置顶"的 bug。
    if (! initialAlwaysOnTopApplied
        && juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* top = getTopLevelComponent())
        {
            if (top != this)
            {
                setAlwaysOnTopActive (alwaysOnTopActive);
            }
        }
    }

    // v1.8.3 → v2.8.2：L 闸门不再依赖顶层窗口 constrainer，因此不需要"上屏后补做"，
    //   只需在顶层窗口就绪后同步一次缩放手柄的可交互状态即可（幂等）。
    applyUserResizeGate();
}

// ==========================================================
// 标题栏 / 关闭按钮几何
// ==========================================================
juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getTitleBarBounds() const
{
    return { 0, 0, getWidth(), PinkXP::ui(titleBarHeight) };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getTitleTextBounds() const
{
    // 标题栏左侧可点击文字区域：从 x=28 开始（drawPinkTitleBar 已为左侧 icon 保留 6..25 的 19px）
    //   · Standalone：右边界到最左按钮（lock）左侧留 8px 间隔，避免误点到按钮
    //   · 插件宿主模式：没有右侧按钮，右边界直接贴到 Editor 右缘，留 8px 边距
    auto tb = getTitleBarBounds();
    const int x = tb.getX() + 28;
    const int right = isPluginHost ? (tb.getRight() - 8)
                                    : (getLockButtonBounds().getX() - 8);
    return { x, tb.getY(), juce::jmax (0, right - x), tb.getHeight() };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getCloseButtonBounds() const
{
    auto tb = getTitleBarBounds();
    const int s = PinkXP::ui(closeButtonSize);
    const int y = tb.getY() + (tb.getHeight() - s) / 2;
    return { tb.getRight() - closeButtonMargin - s, y, s, s };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getPinButtonBounds() const
{
    // stick 驻留态：置顶按钮隐藏，不占位（否则会与 L/× 之间留出一段空隙）
    if (stickActive) return {};

    // 关闭按钮左侧（中间那一个）：固定（置顶）按钮
    auto cb = getCloseButtonBounds();
    const int s = PinkXP::ui(closeButtonSize);
    const int gap = PinkXP::ui(titleButtonGap);
    return { cb.getX() - gap - s, cb.getY(), s, s };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getMinimiseButtonBounds() const
{
    // stick 驻留态：最小化按钮隐藏，不占位
    if (stickActive) return {};

    // Pin 按钮左侧（中间那一个）：最小化按钮
    auto pb = getPinButtonBounds();
    const int s = PinkXP::ui(closeButtonSize);
    const int gap = PinkXP::ui(titleButtonGap);
    return { pb.getX() - gap - s, pb.getY(), s, s };
}

// 最小化按钮左侧（最左那一个）：布局锁定按钮（v1.8.3 新增）
juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getLockButtonBounds() const
{
    // 依次向左排：× ← 固定 ← 最小化 ← L
    //   stick 模式下固定/最小化不占位，L 直接紧邻 ×（排版随隐藏自动收拢）。
    const auto prev = stickActive ? getCloseButtonBounds() : getMinimiseButtonBounds();
    const int s = PinkXP::ui(closeButtonSize);
    const int gap = PinkXP::ui(titleButtonGap);
    return { prev.getX() - gap - s, prev.getY(), s, s };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getFloatingCloseButtonBounds() const
{
    // chrome 隐藏态下的悬浮关闭按钮：右上角，距边距 4px
    constexpr int margin = 4;
    const int s = PinkXP::ui(closeButtonSize);
    return { getWidth() - margin - s, margin, s, s };
}

// ==========================================================
// UI 密度缩放（低分辨率适配）
//   根据顶层窗口所在显示器的逻辑分辨率计算缩放因子并写入 PinkXP 全局。
//   只在缩放因子真正变化时触发一次 resized() + repaint()，避免频繁重排。
//
//   判据区域由 resolveUiScaleArea() 决定：
//     · Windows 用 totalArea —— userArea 会被任务栏和**我们自己的 AppBar 驻留**
//       压缩，stick 条拉高时会误判为矮屏；
//     · macOS 用 userArea —— 菜单栏 / Dock 是真实可用空间，且没有 stick/AppBar。
// ==========================================================
void Y2KmeterAudioProcessorEditor::updateUiScaleForCurrentDisplay()
{
    juce::Rectangle<int> displayArea;

    if (auto* top = getTopLevelComponent())
    {
        const auto sb = (top == this) ? getScreenBounds() : top->getScreenBounds();
        if (auto* display = juce::Desktop::getInstance()
                                 .getDisplays()
                                 .getDisplayForRect (sb))
        {
            displayArea = resolveUiScaleArea (*display);
        }
    }

    if (displayArea.isEmpty())
    {
        if (auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            displayArea = resolveUiScaleArea (*primary);
    }

    const float target = PinkXP::uiScaleForDisplay (displayArea);
    if (std::abs (target - PinkXP::uiScale()) < 0.001f)
        return;

    PinkXP::setUiScale (target);
    resized();
    repaint();
}

void Y2KmeterAudioProcessorEditor::moved()
{
    juce::AudioProcessorEditor::moved();
    updateUiScaleForCurrentDisplay();
}

// chrome 可见时恒为 1.0；chrome 隐藏时本函数已废弃（整个标题栏直接不绘制，
// 只留悬浮关闭按钮按自己的 hover 态切换透明度）。保留仅为兼容旧调用点。
float Y2KmeterAudioProcessorEditor::getChromeAlpha() const
{
    return 1.0f;
}

void Y2KmeterAudioProcessorEditor::handleCloseClicked()
{
    // Standalone 模式：请求应用退出（JUCE 会调用 StandaloneApp::systemRequestedQuit）
    // VST3 模式：顶层是 Editor 本身，调用 quit 无效；改为发 WM_CLOSE 到宿主无意义，
    //           直接忽略即可（关闭按钮在插件模式下仅视觉存在，宿主自带窗口 × 会关闭）。
    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
    }
}

// 固定（置顶）：切换顶层窗口的 alwaysOnTop 属性
//   · Standalone：生效于自定义的 Y2KMainWindow（继承 DocumentWindow）
//   · 插件模式：顶层通常是宿主窗口，setAlwaysOnTop 会被 JUCE 向上调用；
//     宿主有时会忽略，但不会有负作用。
void Y2KmeterAudioProcessorEditor::handlePinClicked()
{
    setAlwaysOnTopActive (! alwaysOnTopActive);
}

// 直接设置"固定置顶"状态（供 handlePinClicked 与 StandaloneApp 恢复使用）
//   为什么要显式 false→true：JUCE Component::setAlwaysOnTop 有早退优化
//   （flag 与当前相同则 no-op）。Editor 首次 visibilityChanged 可能在
//   top==this 的瞬间被调用过一次（flag 已置 true 但实际未落到真正的顶层窗口），
//   之后再推 setAlwaysOnTop(true) 就被早退吃掉，出现"按钮按下但实际未置顶"的
//   bug。这里总是先 false 再 true 强推一次，彻底避开早退。
void Y2KmeterAudioProcessorEditor::setAlwaysOnTopActive (bool shouldBeOnTop)
{
    alwaysOnTopActive = shouldBeOnTop;

    if (auto* top = getTopLevelComponent())
    {
        if (top != this)
        {
            // 先清一次再置目标值，绕开 flag 相同时的 no-op 早退
            top->setAlwaysOnTop (false);
            top->setAlwaysOnTop (shouldBeOnTop);
        }
        else
        {
            top->setAlwaysOnTop (shouldBeOnTop);
        }
    }

    initialAlwaysOnTopApplied = true;
    repaint (getTitleBarBounds());
}

// 最小化：仅在 Standalone 下有意义；调用顶层窗口的 peer 将其最小化
void Y2KmeterAudioProcessorEditor::handleMinimiseClicked()
{
    if (auto* top = getTopLevelComponent())
    {
        if (auto* peer = top->getPeer())
            peer->setMinimised (true);
    }
}

// ==========================================================
// 布局三槽（v2.8.5）
// ----------------------------------------------------------
//   stick 模式下窗口形状完全不同（屏幕宽 × 条高），模块布局与常驻态不兼容，
//   因此按"当前形态"分槽保存：
//       0 = 非 stick（常驻窗口）
//       1 = stick 顶(T)
//       2 = stick 底(B)
//   保存时只写当前槽：切 T/B 只换槽，另一侧调好的布局不会被覆盖。
// ==========================================================
int Y2KmeterAudioProcessorEditor::activeLayoutSlot() const noexcept
{
#if JUCE_WINDOWS
    if (stickActive)
        return stickBottomAligned ? 2 : 1;

    // 启动恢复窗口期：loadInitialModules 已按 stick 槽装载布局，但 AppBar 驻留
    //   要等窗口上屏（timerCallback → restoreSavedStickSession）。这段时间里
    //   任何 onLayoutChanged / persist 仍必须写 stick 槽 —— 否则会把条状布局
    //   覆盖进非 stick 槽，导致「退出 stick 后无法还原原布局」。
    if (pendingStickRestore)
        return pendingStickBottom ? 2 : 1;
#endif
    return 0;
}

void Y2KmeterAudioProcessorEditor::persistCurrentLayoutToProcessor()
{
    if (workspace == nullptr) return;

#if JUCE_WINDOWS
    // 启动恢复窗口期：窗口还是普通窗口尺寸，模块可能已被 resized() 夹紧，
    //   此时布局只是过渡态 —— 一律不写回任何槽（否则会把过渡态固化：
    //   旧 bug 正是把条状布局写进非 stick 槽，或把夹紧后的条状布局写回 stick 槽）。
    if (pendingStickRestore)
        return;
#endif

    // 保存前同步浮动窗口的真实屏幕位置 / 模块状态
    syncFloatingWindowStatesToWorkspace();

    // 1) 当前布局槽：只存 canvas 内模块（不含浮动模块）
    processor.setSavedLayoutXml (
        static_cast<Y2KmeterAudioProcessor::LayoutSlot> (activeLayoutSlot()),
        workspace->saveLayoutAsXml());

    // 2) 脱离态模块独立存档：与布局槽解耦，切换 stick / T-B 不会改动它。
    processor.setSavedFloatingModulesXml (workspace->saveFloatingModulesAsXml());

#if JUCE_WINDOWS
    // stick 驻留条高度随布局一起存档（按当前侧）：退出 stick / 重启后按该侧高度恢复。
    //   仅当窗口确实完成过驻留（stickDockedHeightPx>0）时才记录：启动恢复期间
    //   窗口还处在普通/条状过渡尺寸，此时记录会覆盖存档里的真实条高。
    if (stickActive && stickDockedHeightPx > 0)
        processor.setSavedStickEditorHeight (stickBottomAligned, stickEditorHeightForSave());
#endif
}

void Y2KmeterAudioProcessorEditor::syncStickSessionFlagToProcessor()
{
#if JUCE_WINDOWS
    processor.setSavedStickSession (stickActive, stickBottomAligned);
#else
    processor.setSavedStickSession (false, false);
#endif
}

#if JUCE_WINDOWS
// ==========================================================
// stick 模式（Windows 专属：AppBar 硬 stick 驻留）
//   进入：备份当前布局 + 窗口 bounds → 切换 Horizontal Bar(T) → AppBar 驻留
//         （缩小工作区，其他窗口被挤走）→ 隐藏最小化/置顶按钮
//   退出：取消 AppBar → 恢复备份布局 + 窗口 bounds → 恢复按钮
//   T/B：顶端（horizontalFull）/ 底端（horizontalBottom）切换
// ==========================================================
// stick 期间把顶层窗口 constrainer 的"最小可见量"清零。
//   ResizableWindow 默认 setMinimumOnscreenAmounts(0x10000, 16, 24, 16)：顶边必须完整落在
//   显示器 userArea 内。stick 后 userArea 已被自己的占位压缩，JUCE 在 WM_WINDOWPOSCHANGING /
//   setBoundsConstrained 里会把窗口推到 y=占位厚度（= 旧版"向下闪一下"的根因）。
void Y2KmeterAudioProcessorEditor::setStickOnscreenRelaxed (bool relaxed)
{
    auto* rw = dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent());
    if (rw == nullptr || rw == static_cast<juce::Component*> (this)) return;

    auto* c = rw->getConstrainer();
    if (c == nullptr) return;

    if (relaxed)
    {
        if (! stickOnscreenSaved)
        {
            stickSavedOnscreenTop    = c->getMinimumWhenOffTheTop();
            stickSavedOnscreenLeft   = c->getMinimumWhenOffTheLeft();
            stickSavedOnscreenBottom = c->getMinimumWhenOffTheBottom();
            stickSavedOnscreenRight  = c->getMinimumWhenOffTheRight();
            stickOnscreenSaved = true;
        }
        c->setMinimumOnscreenAmounts (0, 0, 0, 0);
    }
    else if (stickOnscreenSaved)
    {
        c->setMinimumOnscreenAmounts (stickSavedOnscreenTop, stickSavedOnscreenLeft,
                                      stickSavedOnscreenBottom, stickSavedOnscreenRight);
        stickOnscreenSaved = false;
    }
}

int Y2KmeterAudioProcessorEditor::stickEditorHeightForSave() const noexcept
{
    int h = getHeight();

    // hide 态下 chrome 收起，Editor 高度比 show 态少 62px（titleBar+toolbar）；
    //   存档统一按 show 态等效高度，恢复时（先展开再设高度）才不会逐次缩水。
    if (workspace != nullptr && ! workspace->isChromeVisible())
        h += getChromeHeightPx();

    return juce::jmax (1, h);
}

void Y2KmeterAudioProcessorEditor::applyStickBarGeometry (bool bottom, int targetEditorHeight)
{
    auto* top = getTopLevelComponent();
    if (top == nullptr) top = this;

    // Standalone 下顶层窗口比 Editor 多出上下边框（各 4px）；按"当前窗口 - 当前
    //   Editor"实测差值补偿，避免直接用 Editor 高度设窗口导致每次恢复缩水 8px。
    int frameH = 0;
    if (top != this)
        frameH = juce::jmax (0, top->getHeight() - getHeight());

    const auto display = juce::Desktop::getInstance().getDisplays()
                             .getDisplayForPoint (top->getScreenBounds().getCentre());
    const auto userArea = resolveDisplayArea ((display != nullptr) ? display->userArea
                                                                  : juce::Rectangle<int> (1280, 720));

    const int w       = userArea.getWidth();
    const int windowH = juce::jmax (120, targetEditorHeight + frameH);
    const int y       = bottom ? (userArea.getBottom() - windowH) : userArea.getY();

    // 先放宽 resizeLimits：水平条宽度 = 屏幕宽，高度可能小于默认下限（420）
    if (auto* cbc = getConstrainer())
    {
        setResizeLimits (juce::jmin (cbc->getMinimumWidth(), w), 120,
                         juce::jmax (cbc->getMaximumWidth(), w),
                         juce::jmax (cbc->getMaximumHeight(), windowH));
    }

    if (top == this)
        setSize (w, juce::jmax (120, targetEditorHeight));
    else
        top->setBounds (userArea.getX(), y, w, windowH);
}

void Y2KmeterAudioProcessorEditor::redockStickAfterGeometryChange()
{
    if (! stickActive) return;

    auto* top = getTopLevelComponent();
    if (y2kAppBarDock (top, stickBottomAligned, &stickDockRectPx))
    {
        stickDockedHeightPx  = y2kGetWindowHeightPx (top);
        stickPendingHeightPx = stickDockedHeightPx;
        stickStableTicks     = 0;
    }
}

void Y2KmeterAudioProcessorEditor::enterStickMode()
{
    if (stickActive || workspace == nullptr) return;

    // Hide 态下进入：先展开 → 进入 stick → 再收起。
    //   保证备份的窗口 bounds / resizeLimits 都是 Show 态的值，Hide 的 limits 快照
    //   始终嵌套在 stick 之内，退出时不会互相污染。
    if (! workspace->isChromeVisible())
    {
        workspace->setChromeVisible (true);
        enterStickMode();
        if (stickActive)
        {
            workspace->setChromeVisible (false);
            revealArmed = true;   // 用户正在浮层上操作：保持浮现

            // 收起后条高变化：立刻按新高度重新提交占位
            redockStickAfterGeometryChange();
        }
        return;
    }

    auto* top = getTopLevelComponent();
    setStickOnscreenRelaxed (true);

    // 1) 备份非 stick 布局（写 0 槽）+ 窗口 bounds + resizeLimits
    //    先把浮动窗口的最新屏幕位置/业务状态同步进 workspace，备份才是最新态。
    syncFloatingWindowStatesToWorkspace();
    persistCurrentLayoutToProcessor();                 // stickActive 仍 false → 0 槽
    stickBackupLayout = workspace->saveLayoutTree();   // 内存兜底（0 槽读取失败时用）
    if (top != nullptr)
        stickBackupWindowBounds = top->getScreenBounds();
    processor.setSavedStickRestoreBounds (stickBackupWindowBounds);
    if (auto* cbc = getConstrainer())
    {
        stickSavedMinW = cbc->getMinimumWidth();
        stickSavedMinH = cbc->getMinimumHeight();
        stickSavedMaxW = cbc->getMaximumWidth();
        stickSavedMaxH = cbc->getMaximumHeight();
    }

    // 1.5) 浮动模块保持原样（不 dock 回来）：
    //      stick 只是临时条状布局，用户脱离出去的模块应当继续以浮动窗口存在。
    //      applyLayoutPreset 会在重建前后快照/写回脱离态记录，因此它们不会
    //      被 clearAllModules 从布局树里抹掉；退出 stick 时再按备份布局恢复。

    // 2) 标记激活（沿用上次驻留的那一侧）+ 持久化会话，供下次启动自动恢复
    stickActive        = true;
    stickBottomAligned = processor.isSavedStickBottom();
    processor.setSavedStickSession (true, stickBottomAligned);

    // 3) 先把窗口切成条（高度优先用该侧上次调好的值），再装载布局：
    //    槽内模块坐标按"条状 canvas"记录，先切窗口可避免加载后被 resized() 夹紧。
    const int savedH = processor.getSavedStickEditorHeight (stickBottomAligned);
    if (savedH > 0)
        applyStickBarGeometry (stickBottomAligned, savedH);

    // 恢复该侧存档布局；首次进入才用默认横向条
    //   （applyLayoutPreset 末尾会把默认布局写入当前活动槽 = 对应 T/B 槽）。
    const int enterSlot = stickBottomAligned ? 2 : 1;

    if (! loadSavedStickSlotLayout (enterSlot))
    {
        applyLayoutPreset ((int) (stickBottomAligned
            ? ModuleWorkspace::LayoutPreset::horizontalBottom
            : ModuleWorkspace::LayoutPreset::horizontalFull));
    }
    else if (savedH <= 0)
    {
        // 有布局但没记高度（旧数据）：按默认水平条高度 250 切成条
        applyStickBarGeometry (stickBottomAligned, 250);
    }

    // 3.5) AppBar 驻留（缩小工作区，其他窗口让开这条边）。
    //      先 dock 再锁宽度：dock 会按「工作区宽 × 窗口高」把窗口搬到贴边矩形，
    //      用搬完后的实际宽度做宽度锁定才准确。
    if (! y2kAppBarDock (getTopLevelComponent(), stickBottomAligned, &stickDockRectPx))
    {
        // 注册失败（极端环境）——回退到普通窗口，不进入驻留态，
        // 避免工具栏显示 stick 按钮"按下了"但实际没有占位。
        const auto floatingStatesForRollback = workspace->getFloatingModuleStatesRaw();
        stickActive = false;
        processor.setSavedStickSession (false, stickBottomAligned);
        setStickOnscreenRelaxed (false);

        if (auto* cbc = getConstrainer())
            setResizeLimits (stickSavedMinW, stickSavedMinH, stickSavedMaxW, stickSavedMaxH);

        // 先还原窗口尺寸，再装载布局（避免模块坐标被条状 canvas 夹紧）
        if (auto* t = getTopLevelComponent())
            if (! stickBackupWindowBounds.isEmpty())
                t->setBounds (stickBackupWindowBounds);

        // 回到非 stick 布局（0 槽优先，内存备份兜底）
        bool layoutRestored = false;
        const auto normalXml = processor.getSavedLayoutXml (Y2KmeterAudioProcessor::LayoutSlot::normal);
        if (normalXml.isNotEmpty())
            layoutRestored = workspace->loadLayoutFromXml (normalXml);
        if (! layoutRestored && stickBackupLayout.isValid())
            workspace->loadLayoutFromTree (stickBackupLayout);

        // 回退后 stickActive 已为 false，浮动状态需要手动写回
        workspace->restoreFloatingModuleStates (floatingStatesForRollback);

        processor.setSavedStickRestoreBounds ({});
        repaint();
        return;
    }

    stickDockedHeightPx  = y2kGetWindowHeightPx (getTopLevelComponent());
    stickPendingHeightPx = stickDockedHeightPx;
    stickStableTicks     = 0;

    // 3.6) 锁宽度：stick 模式只允许调高度，宽度锁死到当前（屏幕工作区）宽。
    if (auto* cbc = getConstrainer())
    {
        const int w = (top != nullptr) ? top->getWidth() : cbc->getMaximumWidth();
        setResizeLimits (w, cbc->getMinimumHeight(), w, cbc->getMaximumHeight());
    }

    // 记录驻留条高度（按当前侧持久化）
    processor.setSavedStickEditorHeight (stickBottomAligned, stickEditorHeightForSave());

    // 4) 更新 UI（隐藏最小化/置顶按钮；toolbar 切换到 X + T/B）
    repaint();
    workspace->setStickActive (true, stickBottomAligned);
}

void Y2KmeterAudioProcessorEditor::exitStickMode()
{
    if (! stickActive) return;

    // Hide 态下退出：先展开 → 退出 stick（恢复 Show 态备份）→ 再收起
    if (workspace != nullptr && ! workspace->isChromeVisible())
    {
        workspace->setChromeVisible (true);
        exitStickMode();
        workspace->setChromeVisible (false);
        revealArmed = true;
        return;
    }

    // 1) 先把当前 stick 槽布局存盘（用户可能在驻留态里调过模块）
    if (workspace != nullptr)
    {
        syncFloatingWindowStatesToWorkspace();
        persistCurrentLayoutToProcessor();   // stickActive 仍 true → 当前 T/B 槽
    }

    // 2) 取消 AppBar（恢复工作区）
    y2kAppBarUndock (getTopLevelComponent());

    stickActive = false;
    stickDockedHeightPx  = 0;
    stickPendingHeightPx = 0;
    stickStableTicks     = 0;
    stickDockRectPx      = {};

    // 3) 恢复 resizeLimits（解除宽度锁定）
    setResizeLimits (stickSavedMinW, stickSavedMinH, stickSavedMaxW, stickSavedMaxH);

    // 4) 先恢复窗口 bounds：优先存档（进入 stick 前的值，跨重启可用），
    //    内存备份兜底。必须在装载布局之前 —— 模块坐标按 normal 画布记录，
    //    窗口还是条状时装载会被 resized() 当成越界而夹紧。
    auto restoreBounds = processor.getSavedStickRestoreBounds();
    if (restoreBounds.isEmpty())
        restoreBounds = stickBackupWindowBounds;

    if (! restoreBounds.isEmpty())
        if (auto* top = getTopLevelComponent())
            top->setBounds (restoreBounds);

    // 5) 恢复非 stick 布局：优先存档 0 槽（跨重启有效），内存备份兜底
    bool layoutRestored = false;
    if (workspace != nullptr)
    {
        const auto normalXml = processor.getSavedLayoutXml (Y2KmeterAudioProcessor::LayoutSlot::normal);
        if (normalXml.isNotEmpty())
            layoutRestored = workspace->loadLayoutFromXml (normalXml);

        if (! layoutRestored && stickBackupLayout.isValid())
            workspace->loadLayoutFromTree (stickBackupLayout);
    }

    // 6) 清除 stick 会话存档（方向保留：下次进入仍从上次那一侧开始）
    processor.setSavedStickSession (false, stickBottomAligned);
    processor.setSavedStickRestoreBounds ({});

    // 7) 布局里 floating=true 的模块恢复成浮动窗口。
    //      stick 期间一直悬浮的模块（同 moduleId）会被跳过，不会重复创建。
    restoreFloatingWindowsFromWorkspace();

    stickBackupLayout       = juce::ValueTree();
    stickBackupWindowBounds = {};

    // 窗口已搬回原位后再恢复"最小可见量"（此时 JUCE 缓存的 userArea 可能仍是
    //   被占位压缩过的旧值，过早恢复会把刚还原的窗口夹下去）
    setStickOnscreenRelaxed (false);

    // 3) 更新 UI
    repaint();
    if (workspace != nullptr)
        workspace->setStickActive (false);
}

void Y2KmeterAudioProcessorEditor::toggleStickTopBottom()
{
    if (! stickActive) return;

    // Hide 态下切边：先展开（预设按 Show 态计算条高）→ 切边 → 再收起
    if (workspace != nullptr && ! workspace->isChromeVisible())
    {
        workspace->setChromeVisible (true);
        toggleStickTopBottom();
        workspace->setChromeVisible (false);
        revealArmed = true;

        // 收起后条高变化：立刻按新高度重新提交占位
        redockStickAfterGeometryChange();
        return;
    }

    // 先撤销上一次驻留：让工作区恢复，避免 applyLayoutPreset 基于"被自己压小"
    // 的 userArea 计算贴顶/贴底位置（否则窗口会落在 y=250 这类偏移位置，
    // 再 dock 时系统把 [屏幕顶..窗口底] 整段算成占位 → 顶部空出 2 倍高度）。
    y2kAppBarUndock (getTopLevelComponent());

    // 1) 保存当前这一侧的布局到它自己的槽（此时 stickActive 仍指向旧侧）
    if (workspace != nullptr)
    {
        syncFloatingWindowStatesToWorkspace();
        persistCurrentLayoutToProcessor();
    }

    // 2) 切到另一侧：该侧有存档就直接装载（不重建模块），首次进入才用默认横条
    stickBottomAligned = ! stickBottomAligned;
    processor.setSavedStickSession (true, stickBottomAligned);

    const int targetSlot = stickBottomAligned ? 2 : 1;

    // 先把窗口切成另一侧的条（用该侧自己上次调好的高度；首次切过去则沿用
    //   当前高度），再装载该侧布局，避免模块坐标在旧侧 canvas 上被夹紧。
    const int targetH = processor.getSavedStickEditorHeight (stickBottomAligned);
    applyStickBarGeometry (stickBottomAligned,
                           targetH > 0 ? targetH : stickEditorHeightForSave());

    if (! loadSavedStickSlotLayout (targetSlot))
    {
        // 该侧首次进入：用默认横向条（内部会重设窗口几何）
        applyLayoutPreset ((int) (stickBottomAligned
            ? ModuleWorkspace::LayoutPreset::horizontalBottom
            : ModuleWorkspace::LayoutPreset::horizontalFull));
    }

    if (y2kAppBarDock (getTopLevelComponent(), stickBottomAligned, &stickDockRectPx))
    {
        stickDockedHeightPx  = y2kGetWindowHeightPx (getTopLevelComponent());
        stickPendingHeightPx = stickDockedHeightPx;
        stickStableTicks     = 0;
    }

    // 3) 锁定宽度（applyStickBarGeometry 放宽了 limits；预设路径由预设自行处理）
    if (auto* cbc = getConstrainer())
    {
        const int w = (getTopLevelComponent() != nullptr) ? getTopLevelComponent()->getWidth()
                                                         : cbc->getMaximumWidth();
        setResizeLimits (w, cbc->getMinimumHeight(), w, cbc->getMaximumHeight());
    }

    // 预设下拉切到新一侧的形态指示（HORIZONTAL BAR(T) / (B)）
    if (workspace != nullptr)
        workspace->setStickActive (true, stickBottomAligned);

    processor.setSavedStickEditorHeight (stickBottomAligned, stickEditorHeightForSave());
}

bool Y2KmeterAudioProcessorEditor::loadSavedStickSlotLayout (int slot)
{
    if (workspace == nullptr) return false;

    const auto xml = processor.getSavedLayoutXml (
        static_cast<Y2KmeterAudioProcessor::LayoutSlot> (slot));

    if (xml.isEmpty()) return false;
    if (! workspace->loadLayoutFromXml (xml)) return false;

    // 槽里不会有 floating 节点（存档时已排除），这里只需把当前仍存活的
    //   浮动窗口重新登记进 workspace，保证它们不被后续保存"遗忘"。
    restoreFloatingWindowsFromWorkspace();
    return true;
}

void Y2KmeterAudioProcessorEditor::restoreSavedStickSession()
{
    const bool restoreBottom = pendingStickBottom;

    // pendingStickRestore 要等整个恢复流程结束才清除：
    //   restoreSavedStickSession 期间 persistCurrentLayoutToProcessor 依靠它跳过
    //   写盘，避免把"窗口尺寸尚未正确"时的过渡态布局固化到某个槽。
    struct PendingReset
    {
        bool& flag;
        ~PendingReset() { flag = false; }
    } pendingReset { pendingStickRestore };

    if (stickActive || workspace == nullptr || isPluginHost) return;

    auto* top = getTopLevelComponent();
    if (top == nullptr || getPeer() == nullptr) return;

    // 0) 先切到"逻辑 stick 态"：从这一刻起布局槽映射指向 stick 槽。
    //    否则下面 setChromeVisible / 装载布局触发的 onLayoutChanged 会把条状
    //    布局写进非 stick 槽（历史 bug：退出 stick 后原布局丢失）。
    stickActive        = true;
    stickBottomAligned = restoreBottom;

    // 1) 先展开 chrome：存档高度是 show 态等效高度，条几何按 show 态铺
    const bool wasHidden = ! workspace->isChromeVisible();
    if (wasHidden)
        workspace->setChromeVisible (true);

    // 2) 备份进入 stick 前的窗口 bounds / resizeLimits（退出时还原）
    stickBackupWindowBounds = processor.getSavedStickRestoreBounds();
    if (stickBackupWindowBounds.isEmpty())
        stickBackupWindowBounds = top->getScreenBounds();

    // 退出 stick 时要恢复的是"普通窗口可调整范围"，而不是构造时的默认 limits：
    //   启动恢复路径里这里拿到的还是 Editor 默认 limits（例如 max 1600×1100），
    //   若用户的普通窗口比它大，退出时 setBounds 会被夹回，造成"还原后窗口变小"。
    if (auto* cbc = getConstrainer())
    {
        stickSavedMinW = cbc->getMinimumWidth();
        stickSavedMinH = cbc->getMinimumHeight();
        stickSavedMaxW = juce::jmax (cbc->getMaximumWidth(),  stickBackupWindowBounds.getWidth());
        stickSavedMaxH = juce::jmax (cbc->getMaximumHeight(), stickBackupWindowBounds.getHeight());
    }

    // 3) 按存档方向/高度铺条（stickActive / 方向已在步骤 0 设置）
    setStickOnscreenRelaxed (true);

    const int savedH = processor.getSavedStickEditorHeight (stickBottomAligned);
    applyStickBarGeometry (stickBottomAligned, savedH > 0 ? savedH : 250);

    // 3.5) 启动阶段窗口经历了 normal → 条状 →（可能的）hide 过渡，resized() 的
    //      clamp 可能把模块高度缩到过渡期的 canvas 尺寸；此时窗口几何已最终就位，
    //      重载一次存档布局，保证与用户上次调好的逐像素一致。
    loadSavedStickSlotLayout (stickBottomAligned ? 2 : 1);

    // 4) AppBar 驻留
    if (! y2kAppBarDock (top, stickBottomAligned, &stickDockRectPx))
    {
        // 注册失败：回退到普通窗口 + 非 stick 布局（保持状态机与存档一致）
        stickActive = false;
        setStickOnscreenRelaxed (false);
        setResizeLimits (stickSavedMinW, stickSavedMinH, stickSavedMaxW, stickSavedMaxH);
        processor.setSavedStickSession (false, stickBottomAligned);

        // 先还原窗口尺寸，再装载布局（避免模块坐标被条状 canvas 夹紧）
        if (! stickBackupWindowBounds.isEmpty())
            top->setBounds (stickBackupWindowBounds);

        const auto normalXml = processor.getSavedLayoutXml (Y2KmeterAudioProcessor::LayoutSlot::normal);
        if (normalXml.isNotEmpty())
            workspace->loadLayoutFromXml (normalXml);
        restoreFloatingWindowsFromWorkspace();

        if (wasHidden)
        {
            workspace->setChromeVisible (false);
            revealArmed = true;
        }
        return;
    }

    stickDockedHeightPx  = y2kGetWindowHeightPx (top);
    stickPendingHeightPx = stickDockedHeightPx;
    stickStableTicks     = 0;

    // 5) 锁宽度（与 enterStickMode 一致：只允许调高度）
    if (auto* cbc = getConstrainer())
    {
        const int w = top->getWidth();
        setResizeLimits (w, cbc->getMinimumHeight(), w, cbc->getMaximumHeight());
    }

    // 6) UI 切到 X + T/B
    repaint();
    workspace->setStickActive (true, stickBottomAligned);

    // 7) 存档是 hide 态：最后再收起（与运行时进入 stick 的顺序一致）
    if (wasHidden)
    {
        workspace->setChromeVisible (false);
        revealArmed = true;

        // 收起后窗口变矮：立刻按新高度重新提交占位，避免工作区残留空档
        redockStickAfterGeometryChange();
    }
}

void Y2KmeterAudioProcessorEditor::updateStickDockGeometry()
{
    if (! stickActive)
        return;

    auto* top = getTopLevelComponent();
    const auto winRect = y2kGetWindowRectPx (top);
    const int h = winRect.getHeight();
    if (h <= 0)
        return;

    // 1) 高度变化（用户拖拽调高/调低）：等稳定 2 个 tick（10Hz → ~200ms）后重新 SETPOS，
    //    让系统工作区占位跟着窗口高度走。
    if (h != stickDockedHeightPx)
    {
        if (h != stickPendingHeightPx)
        {
            stickPendingHeightPx = h;
            stickStableTicks     = 0;
            return;
        }

        if (++stickStableTicks < 2)
            return;

        stickStableTicks = 0;

        if (y2kAppBarDock (top, stickBottomAligned, &stickDockRectPx))
        {
            stickDockedHeightPx  = y2kGetWindowHeightPx (top);
            stickPendingHeightPx = stickDockedHeightPx;
        }

        return;
    }

    stickPendingHeightPx = h;
    stickStableTicks     = 0;

    // 2) 高度没变但窗口被挪走了（JUCE 的 constrainer 在窗口尺寸变化后会用
    //    display->userArea 重新夹一次顶层窗口位置，把窗口推到工作区顶边 = 占位厚度）：
    //    直接搬回上次提交的驻留矩形，保证窗口与占位始终重合。
    if (! stickDockRectPx.isEmpty() && winRect != stickDockRectPx)
        y2kMoveWindowToRectPx (top, stickDockRectPx);
}
#endif

// ==========================================================
// 窗口行为模型（v2.8.2 重构）
// ----------------------------------------------------------
//   ① L（布局锁定）—— 纯"交互闸门"：
//        · 禁止拖动窗口（mouseDown 不启动 windowDragger）
//        · 禁止用户缩放窗口（禁用 Editor / 顶层窗口上的缩放手柄）
//        · 禁止编辑模块 / 贴画（workspace->setLayoutLocked）
//      **不再**修改任何 resizeLimits、**不再**移动/缩放窗口。
//      旧实现靠 setResizeLimits(w,h,w,h) 锁尺寸，副作用是 setBoundsConstrained 会
//      把窗口夹进工作区（stick 下被推下去）、Hide 期间的尺寸快照与锁冲突（解锁后
//      拖不动 / 拖动后被拉回原位）。闸门模式从根上消除这些耦合。
//
//   ② Hide —— 标题栏 + 工具栏收起：
//        · 进入/退出：基于"当前"窗口一次性收缩/展开 chrome 高度（不再使用旧快照）。
//          锚点：stick 时固定驻留边；否则上半屏固定顶边、下半屏固定底边。
//        · 悬停：鼠标位于窗口内 → 标题栏/工具栏以浮层形式盖在内容之上；
//          离开 → 浮层收起。整个过程窗口几何**完全不变**。
//        · 进入 Hide 自动置顶（沿用旧行为）；不再强制 L。
//
//   ③ Stick —— AppBar 驻留：只有几何真正变化（进入/退出/切边/Hide 切换/
//      用户拖高度）才重新提交占位；悬停浮层不会触发任何占位变化。
//
//   存档兼容：ui.chromeVisible / layoutLocked / window.{x,y,w,h} 语义不变
//   （window bounds 仍按"Show 态等效尺寸"保存，见 Y2KStandaloneApp::saveMainWindowBounds）。
// ==========================================================
void Y2KmeterAudioProcessorEditor::handleLockClicked()
{
    applyLayoutLocked (! layoutLocked, /*initial*/ false);
}

void Y2KmeterAudioProcessorEditor::applyLayoutLocked (bool locked, bool initial)
{
    layoutLocked = locked;

    // 复位任何进行中的窗口拖拽（避免用户按住鼠标切锁的边缘态）
    draggingWindow = false;

    // 同步 workspace（模块 tile / 贴画所在层）和所有浮动窗口
    if (workspace != nullptr)
        workspace->setLayoutLocked (locked);
    for (auto& wnd : floatingWindows_)
        if (wnd != nullptr)
            wnd->setLayoutLocked (locked);

    applyUserResizeGate();

    // 持久化：初次由 Processor state 驱动时不写回
    if (! initial)
        processor.setLayoutLocked (locked);

    repaint (getTitleBarBounds());
}

void Y2KmeterAudioProcessorEditor::applyUserResizeGate()
{
    // 插件宿主：窗口尺寸归宿主管理，保持旧行为（L 只锁模块编辑）
    if (isPluginHost)
        return;

    const bool allow = ! layoutLocked;

    auto gate = [allow] (juce::Component& parent)
    {
        for (auto* c : parent.getChildren())
        {
            if (dynamic_cast<juce::ResizableCornerComponent*> (c) != nullptr
                || dynamic_cast<juce::ResizableBorderComponent*> (c) != nullptr)
            {
                c->setInterceptsMouseClicks (allow, false);
                c->setAlpha (allow ? 1.0f : 0.0f);
            }
        }
    };

    gate (*this);
    if (auto* top = getTopLevelComponent())
        if (top != this)
            gate (*top);
}

// AudioProcessorEditor::setResizeLimits 每次都会重建右下角缩放手柄 →
//   在子组件变化时重新套一次闸门，保证 L 状态始终生效。
void Y2KmeterAudioProcessorEditor::childrenChanged()
{
    juce::AudioProcessorEditor::childrenChanged();
    applyUserResizeGate();
}

int Y2KmeterAudioProcessorEditor::getChromeHeightPx() const noexcept
{
    // 必须与 Y2KStandaloneApp::saveMainWindowBounds 的 kChromeShrink 保持一致
    constexpr int kToolbarHeight = 36;   // == ModuleWorkspace::toolbarHeight
    return PinkXP::ui (titleBarHeight) + PinkXP::ui (kToolbarHeight);
}

void Y2KmeterAudioProcessorEditor::setAutoHideMode (bool shouldHide)
{
    if (autoHideMode == shouldHide && chromeDim == shouldHide)
    {
        if (workspace != nullptr)
            workspace->setAutoHideActive (shouldHide);
        return;
    }

    // 切换态时清空按钮 hover/pressed 残留
    closeButtonHovered = closeButtonPressed = false;
    pinButtonHovered   = pinButtonPressed   = false;
    minButtonHovered   = minButtonPressed   = false;
    lockButtonHovered  = lockButtonPressed  = false;
    titleTextHovered   = false;
    draggingWindow     = false;

    // 浮层只属于 Hide 态：任何切换都先收起
    setChromeRevealed (false);

    autoHideMode = shouldHide;
    if (workspace != nullptr)
        workspace->setAutoHideActive (shouldHide);

    auto* top = getTopLevelComponent();
    const bool canResizeWindow = ! isPluginHost
                                 && juce::JUCEApplicationBase::isStandaloneApp()
                                 && top != nullptr && top != this
                                 && isShowing()
                                 && ! pseudoFullScreen;

    const int chromeH = getChromeHeightPx();

    auto restoreEditorLimits = [this]
    {
        if (! hideLimitsSaved)
            return;
        hideLimitsSaved = false;
        setResizeLimits (hideSavedMinW, hideSavedMinH, hideSavedMaxW, hideSavedMaxH);
    };

    if (canResizeWindow)
    {
        const auto b = top->getBounds();

        bool anchorBottom = false;
       #if JUCE_WINDOWS
        if (stickActive)
            anchorBottom = stickBottomAligned;
        else
       #endif
        {
            const auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (b);
            const auto userArea = (d != nullptr) ? d->userArea : b;
            anchorBottom = b.getCentreY() > userArea.getCentreY();
        }

        const int newH = shouldHide ? juce::jmax (1, b.getHeight() - chromeH)
                                    : b.getHeight() + chromeH;
        const juce::Rectangle<int> nb (b.getX(),
                                       anchorBottom ? b.getBottom() - newH : b.getY(),
                                       b.getWidth(), newH);

        if (shouldHide)
        {
            // 只放宽 Editor 最小高度（宽度/最大值保持，stick 的宽度锁也因此保留），
            //   否则用户在 Hide 下拖缩放手柄时会被旧的 minH 一把拉回。
            if (! hideLimitsSaved)
            {
                if (auto* c = getConstrainer())
                {
                    hideSavedMinW = c->getMinimumWidth();
                    hideSavedMinH = c->getMinimumHeight();
                    hideSavedMaxW = c->getMaximumWidth();
                    hideSavedMaxH = c->getMaximumHeight();
                    hideLimitsSaved = true;
                    setResizeLimits (hideSavedMinW, juce::jmax (1, hideSavedMinH - chromeH),
                                     hideSavedMaxW, hideSavedMaxH);
                }
            }

            chromeDim = true;
            top->setBounds (nb);
        }
        else
        {
            chromeDim = false;
            top->setBounds (nb);
            restoreEditorLimits();
        }

       #if JUCE_WINDOWS
        if (stickActive)
            redockStickAfterGeometryChange();
       #endif
    }
    else
    {
        chromeDim = shouldHide;
        if (! shouldHide)
            restoreEditorLimits();
    }

    if (chromeHiddenOverlay != nullptr)
        chromeHiddenOverlay->setVisible (chromeDim);

    resized();
    updateWorkspaceHitTestHoles();
    repaint();

    if (shouldHide)
    {
        // 沿用旧行为：进入 Hide 自动置顶
        setAlwaysOnTopActive (true);

        // 刚点完 Hide 鼠标还在窗口内：需先离开一次再进入才浮现，避免"点了没反应"的错觉
        revealArmed        = false;
        revealLastInsideMs = 0;
        chromeRevealPoller.callback = [this] { updateChromeReveal(); };
        chromeRevealPoller.startTimerHz (30);
    }
    else
    {
        chromeRevealPoller.stopTimer();
    }
}

void Y2KmeterAudioProcessorEditor::setChromeRevealed (bool shouldReveal)
{
    if (! autoHideMode || ! chromeDim)
        shouldReveal = false;

    if (chromeRevealed == shouldReveal)
        return;

    chromeRevealed = shouldReveal;

    closeButtonHovered = closeButtonPressed = false;
    pinButtonHovered   = pinButtonPressed   = false;
    minButtonHovered   = minButtonPressed   = false;
    lockButtonHovered  = lockButtonPressed  = false;
    titleTextHovered   = false;

    if (workspace != nullptr)
    {
        workspace->setToolbarOverlayRevealed (shouldReveal);
        workspace->setOverlayPassThroughRect (shouldReveal
            ? getTitleBarBounds().withTrimmedBottom (-1) - workspace->getPosition()
            : juce::Rectangle<int>());
    }

    if (chromeHiddenOverlay != nullptr)
        chromeHiddenOverlay->setVisible (chromeDim && ! shouldReveal);

    updateWorkspaceHitTestHoles();
    repaint();
}

void Y2KmeterAudioProcessorEditor::updateChromeReveal()
{
    if (! autoHideMode)
    {
        chromeRevealPoller.stopTimer();
        return;
    }

    auto* top = getTopLevelComponent();
    if (top == nullptr)
        return;

    const auto area  = top->getScreenBounds();
    const auto mouse = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
    const bool inside = top->isShowing() && area.contains (mouse);
    const auto now = juce::Time::getMillisecondCounter();

    if (inside)
        revealLastInsideMs = now;
    else
        revealArmed = true;

    // 正在交互（按住鼠标拖拽 / 下拉菜单、对话框打开 / 正在拖 gain 滑块）时维持当前状态，
    //   避免拖窗口或选菜单项的过程中浮层突然消失。
    //   注：ModifierKeys::getCurrentModifiersRealtime() 在 macOS 上由定时器读取时
    //   并不总能反映"按钮仍按住"，因此额外用 workspace 自己的拖动状态兜底 ——
    //   否则收起浮层会在拖拽中途隐藏 gain 弹窗，导致界面失去鼠标响应。
    const bool busy = juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown()
                      || juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0
                      || (workspace != nullptr && workspace->isGainSliderDragging());
    if (busy)
        return;

    if (inside && revealArmed)
        setChromeRevealed (true);
    else if (! inside && chromeRevealed && now - revealLastInsideMs > 250)
        setChromeRevealed (false);   // 250ms 宽限：鼠标擦过窗口边缘不闪烁
}
void Y2KmeterAudioProcessorEditor::RestoreFpsLimit (int hz)
{
    if (workspace == nullptr) return;

    // 先同步 workspace 内部的 fpsLimit 和按钮文案
    workspace->setFpsLimit (hz);

    // 再触发完整的 FPS 调度链路（等同用户手动点击按钮）
    if (workspace->onFpsLimitChanged)
        workspace->onFpsLimitChanged (workspace->getFpsLimit());
}


// 同步 workspace 的 hit-test 挖洞。
//   · chrome 可见时：无挖洞（workspace 占据 y=titleBarHeight 以下区域，顶部 26px 是
//     Editor 的标题栏范围，workspace 根本不覆盖那里，自然不需要挖洞）。
//   · chrome 隐藏时：workspace 扩到全窗口，顶部 26px 中存在"浮动关闭按钮矩形 +
//     标题文字矩形"两块区域需要冒泡给 Editor 处理；挖洞坐标已转换成 workspace
//     坐标系（workspace 此时 y=0，与 Editor 坐标系一致）。
void Y2KmeterAudioProcessorEditor::updateWorkspaceHitTestHoles()
{
    if (workspace == nullptr) return;

    // 插件宿主模式：没有自画浮动关闭按钮 / 标题文字，强制清空挖洞列表即可
    if (isPluginHost)
    {
        workspace->setHitTestHoles ({});
        return;
    }

    // 常驻态 / Hide 悬停浮层态：标题栏由 Editor 直接处理（浮层态靠 passThroughRect），无需挖洞
    if (! chromeDim || chromeRevealed)
    {
        workspace->setHitTestHoles ({});
        return;
    }

    juce::Array<juce::Rectangle<int>> holes;

    // 1) 浮动关闭按钮矩形（固定位置，与 ChromeHiddenOverlay::getFloatingCloseButtonRect 保持一致）
    constexpr int floatMargin = 4;
    juce::Rectangle<int> closeRect (getWidth() - floatMargin - closeButtonSize,
                                     floatMargin,
                                     closeButtonSize, closeButtonSize);
    holes.add (closeRect);

    // 2) 标题文字矩形（实际像素宽度由 overlay paint 后回写；首次未 paint 时宽度为 0 不添加）
    if (chromeHiddenOverlay != nullptr)
    {
        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        if (textW > 0)
        {
            juce::Rectangle<int> textRect (28, 0, textW, titleBarHeight);
            holes.add (textRect);
        }
    }

    workspace->setHitTestHoles (holes);
}

// ==========================================================
// 鼠标事件：关闭按钮 hover/press/click + TitleBar 区拖动顶层窗口
//   · chromeDim 模式下，mouseEnter/Exit 控制 TitleBar 恢复/淡化
// ==========================================================
void Y2KmeterAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    // 插件宿主模式：不处理按钮 hover 与 chromeDim 分支；但仍要处理"标题文字 hover"，
    //   让鼠标悬停到 "Y2Kmeter v1.1 iisaacbeats.cn" 上时出现手型光标和下划线，
    //   以便点击打开官网。
    if (isPluginHost)
    {
        mouseInsideEditor = true;

        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        const bool hovered = textW > 0 && hotspot.contains (e.getPosition());
        if (hovered != titleTextHovered)
        {
            titleTextHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
        return;
    }

    mouseInsideEditor = true;

    auto updateHover = [&] (bool& state, juce::Rectangle<int> rc)
    {
        const bool h = rc.contains (e.getPosition());
        if (h != state) { state = h; repaint (rc); }
    };

    if (chromeDim && ! chromeRevealed)
    {
        // chrome 隐藏态（未悬停）：Editor 负责处理"浮动关闭按钮 / 标题文字"的 hover，
        //   workspace 已通过 hit-test 挖洞让这些事件冒泡过来；视觉状态由 overlay 负责绘制。
        if (chromeHiddenOverlay == nullptr)
            return;

        const auto p = e.getPosition();
        const auto closeRc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                                .translated (chromeHiddenOverlay->getX(),
                                             chromeHiddenOverlay->getY());
        const bool overClose = closeRc.contains (p);
        chromeHiddenOverlay->setCloseButtonHovered (overClose);
        closeButtonHovered = overClose; // 复用 Editor 的状态跟踪 press/click 流转

        // 标题文字 hover（宽度按 overlay 缓存的实际像素宽度计算，避免整条横条命中）
        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        juce::Rectangle<int> textRc (28 + chromeHiddenOverlay->getX(),
                                      chromeHiddenOverlay->getY(),
                                      juce::jmax (0, textW),
                                      titleBarHeight);
        const bool overText = textW > 0 && textRc.contains (p);
        chromeHiddenOverlay->setTitleTextHovered (overText);
        if (overText != titleTextHovered)
        {
            titleTextHovered = overText;
            setMouseCursor (overText ? juce::MouseCursor::PointingHandCursor
                                     : juce::MouseCursor::NormalCursor);
        }

        // 隐藏态下不再有 pin/minimise 按钮；强制清零以免切换回显示态时有残留 hover 视觉
        if (pinButtonHovered) pinButtonHovered = false;
        if (minButtonHovered) minButtonHovered = false;
    }
    else
    {
        updateHover (closeButtonHovered, getCloseButtonBounds());
        // stick 模式：pin/minimise 已隐藏，跳过 hover 检测（避免 cursor 变化）
        if (! stickActive)
        {
            updateHover (pinButtonHovered,   getPinButtonBounds());
            updateHover (minButtonHovered,   getMinimiseButtonBounds());
        }
        updateHover (lockButtonHovered,  getLockButtonBounds());

        // 标题文字 hover 检测（实际文字像素矩形，宽度由 paint 回写）
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        const bool hovered = textW > 0 && hotspot.contains (e.getPosition());
        if (hovered != titleTextHovered)
        {
            titleTextHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
    }
}

// v2.8.2：Hide 悬停改由 updateChromeReveal 轮询，mouseEnter 只记录状态
void Y2KmeterAudioProcessorEditor::mouseEnter(const juce::MouseEvent&)
{
    mouseInsideEditor = true;
}

void Y2KmeterAudioProcessorEditor::mouseExit(const juce::MouseEvent& e)
{
    // 插件宿主模式：没有按钮 hover 要清理，但标题文字 hover 状态仍要复位
    //   （否则鼠标移出顶部抬头后，下划线和手型光标不会消失）。
    if (isPluginHost)
    {
        if (titleTextHovered)
        {
            titleTextHovered = false;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
        return;
    }

    auto clearHover = [&] (bool& state, juce::Rectangle<int> rc)
    {
        if (state) { state = false; repaint (rc); }
    };
    // 统一清理（两套 rect 都 repaint 一下开销可忽略）
    clearHover (closeButtonHovered, getCloseButtonBounds());
    clearHover (pinButtonHovered,   getPinButtonBounds());
    clearHover (minButtonHovered,   getMinimiseButtonBounds());
    clearHover (lockButtonHovered,  getLockButtonBounds());

    if (titleTextHovered)
    {
        titleTextHovered = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (getTitleBarBounds());
    }

    // 隐藏态：同步清空 overlay 的 hover 视觉
    if (chromeHiddenOverlay != nullptr)
    {
        chromeHiddenOverlay->setCloseButtonHovered (false);
        chromeHiddenOverlay->setTitleTextHovered (false);
    }

    juce::ignoreUnused (e);
    mouseInsideEditor = false;
}

void Y2KmeterAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    // 插件宿主模式：不处理右侧按钮（因为根本没画）和窗口拖拽（宿主负责），
    //   但允许"标题文字"点击打开官网。其他区域点击继续冒泡给子组件。
    if (isPluginHost)
    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (e.getPosition()))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
        return;
    }

    // chrome 隐藏态（未悬停）：仅处理浮动关闭按钮 + 标题文字点击；
    //   悬停浮层显示时标题栏完整可用，走下方常规路径（可拖窗、可点 L/置顶/最小化/关闭）。
    if (chromeDim && ! chromeRevealed)
    {
        if (chromeHiddenOverlay == nullptr) return;

        const auto p = e.getPosition();
        const auto closeRc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                                .translated (chromeHiddenOverlay->getX(),
                                             chromeHiddenOverlay->getY());
        if (closeRc.contains (p))
        {
            closeButtonPressed = true;
            chromeHiddenOverlay->setCloseButtonPressed (true);
            return;
        }

        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        juce::Rectangle<int> textRc (28 + chromeHiddenOverlay->getX(),
                                      chromeHiddenOverlay->getY(),
                                      juce::jmax (0, textW),
                                      titleBarHeight);
        if (textW > 0 && textRc.contains (p))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
        return;
    }

    // 1) 命中标题栏右侧三个按钮之一
    if (getCloseButtonBounds().contains (e.getPosition()))
    {
        closeButtonPressed = true;
        repaint (getCloseButtonBounds());
        return;
    }
    // stick 模式：最小化/置顶按钮已隐藏（paint 不画），这里也不再响应点击
    if (! stickActive && getPinButtonBounds().contains (e.getPosition()))
    {
        pinButtonPressed = true;
        repaint (getPinButtonBounds());
        return;
    }
    if (! stickActive && getMinimiseButtonBounds().contains (e.getPosition()))
    {
        minButtonPressed = true;
        repaint (getMinimiseButtonBounds());
        return;
    }
    if (getLockButtonBounds().contains (e.getPosition()))
    {
        lockButtonPressed = true;
        repaint (getLockButtonBounds());
        return;
    }

    // 2) 只有 TitleBar 区域才允许拖拽顶层窗口（模块 / toolbar 区不参与窗口拖拽）
    if (! getTitleBarBounds().contains(e.getPosition()))
        return;

    // 2.1) 命中标题文字热区 → 打开官网（iisaacbeats.cn），不启动窗口拖拽
    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (e.getPosition()))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
            return;
        }
    }

    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        // 锁定态 / stick 驻留态：不启动顶层窗口拖拽。
        //   stick 驻留态的几何由 AppBar 管理，拖动会与占位脱节；双击全屏也会
        //   破坏贴边矩形，因此 mouseDoubleClick 里同样对 stickActive 早退。
        if (! layoutLocked && ! stickActive)
        {
            if (auto* top = getTopLevelComponent())
            {
                windowDragger.startDraggingComponent(top, e.getEventRelativeTo(top));
                draggingWindow = true;
            }
        }
    }
}

void Y2KmeterAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e)
{
    // 插件宿主模式：无 chrome 拖窗需求
    if (isPluginHost) return;

    if (draggingWindow)
    {
        if (auto* top = getTopLevelComponent())
            windowDragger.dragComponent(top, e.getEventRelativeTo(top), nullptr);
    }
}

void Y2KmeterAudioProcessorEditor::mouseUp(const juce::MouseEvent& e)
{
    // 插件宿主模式：无 chrome 按钮点击处理
    if (isPluginHost) return;

    // 拖窗松手时再做顶部边界判断：若抬头（标题栏）已被拖出当前显示器
    // 可用区顶部，则把窗口整体弹回可用区顶部。这样拖动过程中不会因实时
    // 钳制而闪现，多屏场景下用户也能把窗口拖到上方屏幕（松手时窗口已在
    // 上方屏幕，display 判定切换到上方屏幕，自然不会弹回）。
    if (draggingWindow)
    {
        if (auto* top = getTopLevelComponent())
        {
            auto& displays = juce::Desktop::getInstance().getDisplays();
            auto  disp     = displays.getDisplayForRect (top->getScreenBounds());
            juce::Rectangle<int> area;
            if (disp != nullptr && ! disp->userArea.isEmpty())
                area = disp->userArea;
            else if (auto primary = displays.getPrimaryDisplay())
                area = primary->userArea;

            if (! area.isEmpty())
            {
                const auto b = top->getScreenBounds();

                // 顶部拖出回弹（原有）：标题栏拖出工作区顶部 → 强制弹回，
                //   防止窗口"丢失"在屏幕上方（找不到标题栏拖回）。
                if (b.getY() < area.getY())
                {
                    top->setTopLeftPosition (b.getX(), area.getY());
                }
                else
                {
                    // 四边边缘吸附（软 stick）：松手时任一边距工作区边缘
                    //   ≤ 阈值即贴到该边，方便把窗口精确调到屏幕边缘；
                    //   再次拖动窗口会自动脱离吸附（无"锁定"语义）。
                    constexpr int kStickThresholdPx = 8;
                    int x = b.getX();
                    int y = b.getY();

                    if (std::abs (b.getX() - area.getX()) <= kStickThresholdPx)
                        x = area.getX();
                    else if (std::abs (b.getRight() - area.getRight()) <= kStickThresholdPx)
                        x = area.getRight() - b.getWidth();

                    if (std::abs (b.getY() - area.getY()) <= kStickThresholdPx)
                        y = area.getY();
                    else if (std::abs (b.getBottom() - area.getBottom()) <= kStickThresholdPx)
                        y = area.getBottom() - b.getHeight();

                    if (x != b.getX() || y != b.getY())
                        top->setTopLeftPosition (x, y);
                }
            }
        }
    }

    draggingWindow = false;

    // 按下与抬起都落在同一按钮内才触发 click —— XP 风按钮的标准行为
    if (closeButtonPressed)
    {
        closeButtonPressed = false;

        // 根据当前 chrome 状态决定使用"浮动关闭按钮"还是"标题栏内的关闭按钮"的几何
        juce::Rectangle<int> rc;
        if (chromeDim && ! chromeRevealed && chromeHiddenOverlay != nullptr)
        {
            rc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                    .translated (chromeHiddenOverlay->getX(),
                                 chromeHiddenOverlay->getY());
            chromeHiddenOverlay->setCloseButtonPressed (false);
        }
        else
        {
            rc = getCloseButtonBounds();
        }

        const bool stillOn = rc.contains (e.getPosition());
        repaint (rc);
        if (stillOn) handleCloseClicked();
        return;
    }
    if (pinButtonPressed)
    {
        pinButtonPressed = false;
        const bool stillOn = getPinButtonBounds().contains (e.getPosition());
        repaint (getPinButtonBounds());
        if (stillOn) handlePinClicked();
        return;
    }
    if (minButtonPressed)
    {
        minButtonPressed = false;
        const bool stillOn = getMinimiseButtonBounds().contains (e.getPosition());
        repaint (getMinimiseButtonBounds());
        if (stillOn) handleMinimiseClicked();
        return;
    }
    if (lockButtonPressed)
    {
        lockButtonPressed = false;
        const bool stillOn = getLockButtonBounds().contains (e.getPosition());
        repaint (getLockButtonBounds());
        if (stillOn) handleLockClicked();
        return;
    }
}

// ==========================================================
// 双击标题栏空白区 → 切换顶层窗口"伪最大化" / 还原（v2.5.8 修复）
//   · 参考 Windows 系统标题栏双击最大化的用户习惯；
//   · 命中矩形：抬头 titleBarHeight 高度内、避开右侧四个按钮和左侧标题文字热区；
//   · 仅 Standalone 有效，插件宿主模式下窗口由 DAW 管理，直接忽略；
//   · chrome 隐藏态下不响应（隐藏时"标题栏"实际不存在，浮层区域也不合适当"抬头"）；
//   · 【重要】不再使用 juce::ResizableWindow::setFullScreen —— JUCE 在 Windows 上
//     的实现是 ShowWindow(SW_SHOWMAXIMIZED)；对无原生标题栏窗口（Y2KMainWindow
//     使用 setUsingNativeTitleBar(false)）该 API 会把窗口撑到 monitor 物理边界
//     （覆盖任务栏），并且此时主窗口是 alwaysOnTop=true 与 PopupMenu / ComboBox
//     弹窗平级 topmost，主窗口铺满屏后 Popup 被 Z 序压住 → "右键添加模块"菜单、
//     底部 Toolbar 里 FPS/Source 下拉都看不见。
//   · 新方案：改走 toggleFakeFullScreen() —— Windows 手动 setBounds 到当前显示器的
//     userArea（已扣除任务栏），Popup 依然可以在主窗口内正常弹出且可视；
//     macOS 走原生 setFullScreen（覆盖菜单栏/Dock，完全全屏）。
//
// 踩坑记录：
//   · JUCE 双击流程是 mouseDown → mouseUp → mouseDoubleClick，中间的 mouseDown
//     会启动 windowDragger.startDraggingComponent()。这里进入全屏前要主动把
//     draggingWindow 复位，否则接下来任何 mouseDrag 都会尝试拖动窗口。
// ==========================================================
void Y2KmeterAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isPluginHost) return;
    // Hide 态：浮现的标题栏只是浮层，全屏会与 Hide 的几何收缩互相干扰，不响应
    if (chromeDim)    return;
    // L 锁定：禁止改变窗口尺寸（但允许从伪全屏还原）
    if (layoutLocked && ! pseudoFullScreen) return;
    // stick 驻留态：全屏会破坏 AppBar 占位（窗口脱离贴边），禁止双击全屏
    if (stickActive)  return;

    // 命中区域：仅标题栏矩形，且不落在三个按钮 + 标题文字热区
    const auto pos = e.getPosition();
    if (! getTitleBarBounds().contains (pos)) return;

    if (getCloseButtonBounds()   .contains (pos)) return;
    if (getPinButtonBounds()     .contains (pos)) return;
    if (getMinimiseButtonBounds().contains (pos)) return;
    if (getLockButtonBounds()    .contains (pos)) return;

    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (pos)) return;
    }

    if (! juce::JUCEApplicationBase::isStandaloneApp()) return;

    // 前置的 mouseDown 已启动了 windowDragger —— 复位，避免全屏后残留拖动状态
    draggingWindow = false;

    toggleFakeFullScreen();
}

bool Y2KmeterAudioProcessorEditor::keyPressed(const juce::KeyPress& key) {
  // Milkdrop 键盘快捷键（v2.5.7）：← → 切预设，空格随机预设。
  // 只处理嵌入态 Milkdrop（脱离态由 FloatingModuleWindow::keyPressed 自行路由）。
  // 必须调用 MilkdropModule 的公开 API（prevPreset/nextPreset/randomPreset），
  // 它们内部会根据 isFloating() 正确路由到本地 GLView 或 Editor renderer；
  // 直接调 Editor 的 RequestMilkdropPreset* 在脱离态下会被挂起的 renderer 忽略。

#ifdef Y2KMETER_MILKDROP_ONLY
  // Milkdrop-only 插件变体：屏蔽 ← / → / 空格 键盘预设切换。
  // 该变体把预设切换暴露为宿主自动化参数（MD CC / automation）驱动，
  // 键盘控制会与宿主自动化产生冲突；完整版 Y2Kmeter（含 Standalone）仍保留。
  juce::ignoreUnused (key);
  return false;
#else

  MilkdropModule* milk = nullptr;
  if (workspace != nullptr) {
    for (int i = 0; i < workspace->getNumModules(); ++i) {
      if (auto* m = workspace->getModule(i)) {
        if (m->getModuleType() == ModuleType::milkdrop) {
          milk = dynamic_cast<MilkdropModule*>(m);
          break;
        }
      }
    }
  }

  if (milk == nullptr)
    return false;

  if (key == juce::KeyPress::leftKey)  { milk->prevPreset();   return true; }
  if (key == juce::KeyPress::rightKey) { milk->nextPreset();   return true; }
  if (key == juce::KeyPress::spaceKey) { milk->randomPreset(); return true; }

  return false;
#endif
}

// ==========================================================
// 伪最大化 / 还原（v2.5.8）
//   替代 juce::ResizableWindow::setFullScreen，规避以下 Windows 下的两个副作用：
//     A) 无原生标题栏窗口（Y2KMainWindow 使用 setUsingNativeTitleBar(false)）
//        的 SW_SHOWMAXIMIZED 会撑到 monitor 物理边界（覆盖任务栏）；
//     B) 全屏后主窗口 alwaysOnTop=true 铺满屏，与 PopupMenu / ComboBox 弹窗平级
//        topmost，弹窗 Z 序被压住不可见。
//
// 方案（Windows）：手动 setBounds 到当前显示器 userArea（Windows 已扣除任务栏），
// 并暂时放宽 constrainer 的 resize limits；退出时把 bounds 与 limits 都还原。
// macOS：走原生 setFullScreen（toggleFullScreen → 独立 Space 覆盖菜单栏/Dock），
// 不受 Windows 伪最大化副作用影响，也不应使用 userArea 伪最大化。
//
// 与 layoutLocked 兼容：
//   · 若在锁定态进入，applyLayoutLocked 之前已把 constrainer min=max=当前尺寸
//     锁死，会阻止 setBounds 到 userArea。这里保存进入前的 limits，覆盖为
//     (0, 0, MAX, MAX)，setBounds 完再返回。退出时优先按锁定态重新 apply。
// ==========================================================
void Y2KmeterAudioProcessorEditor::toggleFakeFullScreen()
{
    auto* top = getTopLevelComponent();
    if (top == nullptr || top == this) return;

    auto* rw = dynamic_cast<juce::ResizableWindow*> (top);
    if (rw == nullptr) return;

#if JUCE_MAC
    // macOS 单独适配：恢复原生全屏（NSWindow toggleFullScreen → 进入独立 Space，
    // 覆盖菜单栏与 Dock），避免 v2.5.8 的伪最大化（setBounds 到 userArea）在
    // macOS 上留下系统菜单栏/Dock 空间。Windows 端仍走下方伪最大化路径。
    rw->setFullScreen (! rw->isFullScreen());
    return;
#endif

    if (! pseudoFullScreen)
    {
        // -------- 进入伪全屏 --------
        // 1) 保存当前 bounds
        preFullScreenBounds = top->getBounds();

        // 2) 保存并放宽 constrainer 的 resize limits（避免被夹回）
        if (auto* cbc = rw->getConstrainer())
        {
            preFullScreenMinW = cbc->getMinimumWidth();
            preFullScreenMinH = cbc->getMinimumHeight();
            preFullScreenMaxW = cbc->getMaximumWidth();
            preFullScreenMaxH = cbc->getMaximumHeight();
            preFullScreenLimitsValid = true;
        }
        // 临时把 limits 放到 1..very-large，保证 setBounds 到 userArea 能生效。
        // 用 1（而不是 0）避免命中 ComponentBoundsConstrainer 内部
        // jassert(minW>0 && minH>0)。
        rw->setResizeLimits (1, 1, 32768, 32768);

        // 3) 计算目标 bounds：当前显示器的 userArea（已扣除任务栏）
        auto& displays = juce::Desktop::getInstance().getDisplays();
        auto  disp     = displays.getDisplayForRect (top->getScreenBounds());
        juce::Rectangle<int> target;
        if (disp != nullptr && ! disp->userArea.isEmpty())
            target = disp->userArea;
        else if (auto primary = displays.getPrimaryDisplay())
            target = primary->userArea;
        else
            target = top->getScreenBounds(); // 兜底，理论上到不了

        // 4) 应用
        top->setBounds (target);

        pseudoFullScreen = true;
    }
    else
    {
        // -------- 退出伪全屏，恢复原 bounds 与 limits --------
        // 1) 先把 limits 放宽，防止 setBounds 被夹住
        rw->setResizeLimits (1, 1, 32768, 32768);

        if (! preFullScreenBounds.isEmpty())
            top->setBounds (preFullScreenBounds);

        pseudoFullScreen = false;

        // 2) 还原进入前的 resize limits（v2.8.2：L 不再改写 limits，无需特殊处理）
        if (preFullScreenLimitsValid)
        {
            const int minW = juce::jmax (1, preFullScreenMinW);
            const int minH = juce::jmax (1, preFullScreenMinH);
            const int maxW = juce::jmax (minW, preFullScreenMaxW);
            const int maxH = juce::jmax (minH, preFullScreenMaxH);
            rw->setResizeLimits (minW, minH, maxW, maxH);
        }

        preFullScreenLimitsValid = false;
    }
}

// ==========================================================
// 音频源下拉：Editor 对外 API → 转发到 Workspace
// ==========================================================
void Y2KmeterAudioProcessorEditor::setAudioSourceItems (
    const juce::Array<AudioSourceEntry>& items,
    const juce::String& selectedSourceId)
{
    if (workspace == nullptr) return;

    juce::Array<ModuleWorkspace::AudioSourceItem> ws;
    ws.ensureStorageAllocated (items.size());
    for (const auto& it : items)
        ws.add ({ it.displayName, it.sourceId, it.isLoopback });

    workspace->setAudioSourceItems (ws, selectedSourceId);
}

// ==========================================================
// 持久化辅助：chrome 可见性（Standalone 在 save/restore 时使用）
// ==========================================================
bool Y2KmeterAudioProcessorEditor::isChromeVisible() const
{
    return workspace != nullptr ? workspace->isChromeVisible() : true;
}

void Y2KmeterAudioProcessorEditor::setChromeVisible (bool shouldBeVisible)
{
    if (workspace != nullptr)
        workspace->setChromeVisible (shouldBeVisible);
}

// ==========================================================
// timerCallback —— 10Hz 拉 Processor 的 CPU 占用，广播给所有模块；
//                  同时按 1s 窗口换算实际 FPS，下发给 workspace 的 FPS 标签
// ==========================================================
void Y2KmeterAudioProcessorEditor::timerCallback()
{
    // v2.9.2：帧耗时统计周期性落盘（诊断构建；release 构建为空操作）
   #if Y2K_ENABLE_FRAME_PERF_LOG
    y2k::perflog::dumpIfDue();
   #endif

    if (workspace == nullptr) return;

    // 低分辨率适配（多屏）：10Hz 轮询当前窗口所在显示器，跨屏移动或显示器
    //   分辨率变化时自动校准 UI 密度缩放。Editor 是顶层窗口的子组件，窗口被
    //   拖到另一台显示器时 Editor::moved() 不会触发，因此必须用轮询兜底。
    updateUiScaleForCurrentDisplay();

#if JUCE_WINDOWS
    // v2.8.5：启动恢复 stick 会话（等窗口上屏、peer 就绪后再注册 AppBar）
    if (pendingStickRestore && isShowing() && getPeer() != nullptr)
        restoreSavedStickSession();

    // stick 驻留态：窗口高度被改动 / 窗口被挪走时，重新对齐 AppBar 占位。
    if (stickActive)
        updateStickDockGeometry();
#endif

    // 10Hz 拉取宿主自动化参数 → 应用到 Milkdrop 渲染（仅插件宿主形态）。
    if (isPluginHost)
    {
        pullMilkdropAutomationParams();

        // 预设索引持久化：渲染索引变化时回写 md_preset_index。
        //   · 预设切换由 GL 线程异步消费并更新渲染索引，UI 线程无法在点击时立即
        //     拿到新索引，故在此轮询检测变化后 push；
        //   · 不在 pull 里做反向跳转，避免与"UI 切换后回写参数"产生双向竞争。
        {
            const int currentIdx = GetMilkdropCurrentPresetIndex();
            if (currentIdx >= 0 && currentIdx != lastSyncedPresetIndex_)
            {
                lastSyncedPresetIndex_ = currentIdx;
                pushMilkdropAutomationParams();
            }
        }
    }

#ifdef Y2KMETER_MILKDROP_ONLY
    // 同步右下角 resize 斜线（ResizableCornerComponent）的可见性，使其跟随
    // Milkdrop 顶部 overlay 控制栏的显隐：控制栏自动隐藏时斜线一起隐藏，
    // hover 重新显示时斜线恢复。（用轮询兜底，覆盖 editorResized 对可见性的重置）
    if (resizableCorner != nullptr)
    {
        bool milkdropFocused = false;
        for (int i = 0; i < workspace->getNumModules(); ++i)
            if (auto* md = dynamic_cast<MilkdropModule*> (workspace->getModule (i)))
            { milkdropFocused = md->isFocused(); break; }
        resizableCorner->setVisible (milkdropFocused);
    }
#endif

    // v1.9.x：新手引导 STEP 2 条件检查（孵化完成检测）
    if (! isPluginHost)
        checkTutorialStep2Condition();

    // 仅当存在 VirtuPet 模块时，才保活/计算对应的音频信号。
    const int n = workspace->getNumModules();
    juce::Array<VirtuPetModule*> virtuPetModules;
    virtuPetModules.ensureStorageAllocated (n);

    for (int i = 0; i < n; ++i)
        if (auto* virtuPet = dynamic_cast<VirtuPetModule*> (workspace->getModule (i)))
            virtuPetModules.add (virtuPet);

    const bool hasVirtuPet = ! virtuPetModules.isEmpty();
    if (hasVirtuPet != virtuPetSignalRetained)
    {
        if (hasVirtuPet)
            processor.getAnalyserHub().retain (AnalyserHub::Kind::Loudness);
        else
            processor.getAnalyserHub().release (AnalyserHub::Kind::Loudness);

        virtuPetSignalRetained = hasVirtuPet;
    }

    float signal01 = 0.0f;
    if (hasVirtuPet)
    {
        if (auto frame = processor.getAnalyserHub().getLatestFrame())
        {
            if (frame->has (AnalyserHub::Kind::Loudness))
            {
                const float rmsDb = juce::jmax (frame->loudness.rmsL, frame->loudness.rmsR);
                signal01 = juce::jlimit (0.0f, 1.0f, juce::Decibels::decibelsToGain (rmsDb, -144.0f));
            }
            else if (frame->has (AnalyserHub::Kind::Dynamics))
            {
                const float rmsDb = juce::jmax (frame->dynamics.rmsL, frame->dynamics.rmsR);
                signal01 = juce::jlimit (0.0f, 1.0f, juce::Decibels::decibelsToGain (rmsDb, -144.0f));
            }
            else if (frame->has (AnalyserHub::Kind::Oscilloscope))
            {
                double sumSqL = 0.0;
                double sumSqR = 0.0;
                for (int i = 0; i < AnalyserHub::oscilloscopeBufferSize; ++i)
                {
                    const double l = (double) frame->oscL[(size_t) i];
                    const double r = (double) frame->oscR[(size_t) i];
                    sumSqL += l * l;
                    sumSqR += r * r;
                }

                const double invN = 1.0 / (double) AnalyserHub::oscilloscopeBufferSize;
                const float rmsLinear = (float) std::sqrt (juce::jmax (0.0, juce::jmax (sumSqL * invN, sumSqR * invN)));
                signal01 = juce::jlimit (0.0f, 1.0f, rmsLinear);
            }
        }
    }

    const float cpu01 = (float) processor.getCpuLoad();
    for (int i = 0; i < n; ++i)
        if (auto* m = workspace->getModule (i))
            m->setCpuLoad (cpu01);

    for (auto* virtuPet : virtuPetModules)
        virtuPet->setSignalLevel01 (signal01);

    // FPS 统计：每秒更新一次显示（使用高精度时戳防止 wall-clock 跨日问题）
    updateFrameClockWatchdog();

    const double nowMs   = juce::Time::getMillisecondCounterHiRes();
    const double deltaMs = nowMs - lastFpsTimeMs;
    if (deltaMs >= 1000.0)
    {
        const juce::int64 cur = frameCounter.load (std::memory_order_relaxed);
        const juce::int64 diff = cur - lastFrameCounterSample;
        const float dispatchFps = (float) (diff * 1000.0 / deltaMs);

        // v2.9：以 GL 线程真实出帧数为准。GL 跟不上时多次 repaint 会被合并，
        //   分发数仍是 120 但屏幕实际帧更少 —— 旧逻辑只看分发数，会把掉帧"藏"起来。
        //   取 min 是因为鼠标 hover 等非分发 repaint 也会额外出帧。
        //   macOS（不挂 Editor GL）或 GL 尚未出过帧时回退到分发数。
        const juce::int64 glCur  = glFrameCounter.load (std::memory_order_relaxed);
        const juce::int64 glDiff = glCur - lastGlFrameCounterSample;
        lastGlFrameCounterSample = glCur;

        float fps = dispatchFps;
       #if ! JUCE_MAC
        if (glDiff > 0 && openGLContext.isAttached())
            fps = juce::jmin (dispatchFps, (float) (glDiff * 1000.0 / deltaMs));
       #endif

        float displayedFps;
        if (userRequestedFpsLimit == 0)
            displayedFps = fps;  // 无限模式直接显示实际帧率
        else if (userRequestedFpsLimit == 30 && fps >= 28.0f)
            displayedFps = 30.0f;
        else if (userRequestedFpsLimit == 60 && fps >= 58.0f)
            displayedFps = 60.0f;
        else if (userRequestedFpsLimit == 120 && fps >= 116.0f)
            displayedFps = 120.0f;
        else
            displayedFps = juce::jmin ((float) userRequestedFpsLimit, fps);

        workspace->setMeasuredFps (displayedFps);
        applyAdaptiveFrameRate (fps);

        lastFrameCounterSample = cur;
        lastFpsTimeMs          = nowMs;
    }
}

// ==========================================================
// applyAdaptiveFrameRate —— 保持 FrameDispatcher 与用户设定上限一致
//   · 采用"封顶"语义：未触及上限前不做干预，触及上限时 FrameDispatcher 即起限制作用。
//   · 插件与 Standalone 统一逻辑，不再区分宿主模式。
// ==========================================================
void Y2KmeterAudioProcessorEditor::applyAdaptiveFrameRate (float /*measuredFps*/)
{
    // FPS 限制采用"封顶"语义：FrameDispatcher 固定以用户选择的上限频率调度，
    // 不再根据实测帧率动态降档。若系统渲染能力不足以达到上限，多余的调度
    // 仅为空转（开销极小），不会造成功能性问题。
    auto& hub = processor.getAnalyserHub();

    const int targetHz = (userRequestedFpsLimit == 0) ? 120 : userRequestedFpsLimit;

    if (activeDispatchHz != targetHz || hub.getFrameDispatcherHz() != targetHz)
    {
        activeDispatchHz = targetHz;
        hub.startFrameDispatcher (activeDispatchHz);
    }

    // v2.9.1：FPS 上限变化后同步兜底 Timer 的周期
    updateFramePacingFallbackTimer();
}

// ==========================================================
// v2.9.1 vblank 对齐帧时钟（credit 累加节流 + 兜底 Timer）
//
// 设计原则（v2.9.1 修正 v2.9 的两处问题）：
//   · v2.9 把分发完全押在 vblank 回调上。但 JUCE Windows VBlankThread 通过
//     AsyncUpdater 派发事件，消息线程没处理完上一个回调时新事件被直接丢弃
//     （flagPaintPending 合并）；GL 渲染线程又持锁执行 renderOpenGL +
//     swapBuffers（等 vsync）周期性阻塞消息线程 → 负反馈循环，分发频率
//     断崖式掉到吞吐平衡点（实测 ~30fps）。
//   · v2.9.1 改为"vblank 时间信用"模型：
//       每个 vblank 事件把"距上一个事件的真实时间"累加进 credit；
//       credit ≥ 目标间隔时消费一个间隔并分发。分数倍率（如 144Hz 显示器
//       限 120fps = 6/5 分频）由 credit 余量自动实现，且错过若干 vblank 后
//       下一次回调一定追帧（credit 已超额）。
//       同时启动兜底 Timer：vblank 被吞期间按墙钟维持分发频率 ≥ 上限，
//       两个入口共用 lastDispatchWallMs 去重（vblank 正常时兜底不触发）。
//   · 无上限模式（userRequestedFpsLimit==0）每个 vblank 都分发 = 跟随刷新率。
// ==========================================================
void Y2KmeterAudioProcessorEditor::onDisplayVBlank (double timestampSec)
{
    const double nowWallMs = juce::Time::getMillisecondCounterHiRes();
    vblankLastWallMs = nowWallMs;

    // 首个 vblank（或看门狗回退后 vblank 恢复）：接管帧时钟并立即出一帧
    if (! processor.getAnalyserHub().isUsingExternalFrameClock())
    {
        processor.getAnalyserHub().setExternalFrameClock (true);
        updateFramePacingFallbackTimer();
        vblankLastSec  = timestampSec;
        vblankCreditMs = 0.0;
        performFrameDispatch (timestampSec);
        return;
    }

    tryDispatchFromVBlank (nowWallMs, timestampSec);
}

void Y2KmeterAudioProcessorEditor::performFrameDispatch (double timestampSec)
{
    // 双入口去重：vblank 与兜底 Timer 在 ~1ms 内竞争时只分发一次
    const double nowWallMs = juce::Time::getMillisecondCounterHiRes();
    if (lastDispatchWallMs > 0.0 && nowWallMs - lastDispatchWallMs < 1.0)
        return;

    lastDispatchWallMs = nowWallMs;
    processor.getAnalyserHub().dispatchFrameNow (timestampSec);
}

void Y2KmeterAudioProcessorEditor::tryDispatchFromVBlank (double nowWallMs, double timestampSec)
{
    juce::ignoreUnused (nowWallMs);

    // 无上限：每个 vblank 分发一次（跟随显示器刷新率）
    if (userRequestedFpsLimit <= 0)
    {
        performFrameDispatch (timestampSec);
        return;
    }

    // credit 累加：两次回调之间的真实 vblank 时间（回调被吞时一次补齐多个周期）
    if (vblankLastSec > 0.0)
    {
        const double dtSec = timestampSec - vblankLastSec;
        if (dtSec > 0.0 && dtSec < 0.25)
            vblankCreditMs += dtSec * 1000.0;
    }
    vblankLastSec = timestampSec;

    const double intervalMs = 1000.0 / (double) userRequestedFpsLimit;

    // 追赶保护：credit 超额过多（长停顿）时清空，避免连续补帧风暴
    if (vblankCreditMs > intervalMs * 2.0)
        vblankCreditMs = intervalMs * 2.0;

    if (vblankCreditMs >= intervalMs - 0.5)   // 0.5ms 容差吸收浮点/舍入抖动
    {
        vblankCreditMs = juce::jmin (vblankCreditMs - intervalMs, intervalMs); // 保留余量供 6/5 分频
        performFrameDispatch (timestampSec);
    }
}

void Y2KmeterAudioProcessorEditor::tryDispatchFromFallbackTimer (double nowWallMs)
{
    if (userRequestedFpsLimit <= 0)
        return; // 无上限：由 vblank 独占驱动，兜底不介入

    const double intervalMs = 1000.0 / (double) userRequestedFpsLimit;
    if (lastDispatchWallMs <= 0.0 || nowWallMs - lastDispatchWallMs >= intervalMs)
        performFrameDispatch (juce::Time::getMillisecondCounterHiRes() * 0.001);
}

void Y2KmeterAudioProcessorEditor::updateFramePacingFallbackTimer()
{
    if (framePacingFallback == nullptr)
        return;

    if (userRequestedFpsLimit > 0
        && processor.getAnalyserHub().isUsingExternalFrameClock())
    {
        // 兜底周期取目标间隔（1000/hz 整数除法 = 8ms@120Hz），实际出帧由
        // 判据（≥ intervalMs）限制在 120Hz；仅在 vblank 被吞时承担分发。
        framePacingFallback->startTimer (1000 / userRequestedFpsLimit);
    }
    else
    {
        framePacingFallback->stopTimer();
    }
}

void Y2KmeterAudioProcessorEditor::updateFrameClockWatchdog()
{
    auto& hub = processor.getAnalyserHub();
    if (! hub.isUsingExternalFrameClock())
        return;

    // 250ms 内没有 vblank：显示器休眠 / 远程桌面 / 窗口被最小化到无输出 —— 交回内部 Timer
    if (juce::Time::getMillisecondCounterHiRes() - vblankLastWallMs > 250.0)
    {
        hub.setExternalFrameClock (false);
        updateFramePacingFallbackTimer();
        lastDispatchWallMs = 0.0;
        vblankLastSec      = 0.0;
        vblankCreditMs     = 0.0;
    }
}

// ================================================================
// OpenGLRenderer — projectM 在 Editor GL 上下文中渲染
// ================================================================

namespace {
  constexpr int kMilkdropTargetFps        = 60;
  constexpr int kMilkdropDefaultMeshW     = 128;
  constexpr int kMilkdropDefaultMeshH     = 80;
  constexpr double kMilkdropPresetDuration = 20.0;
  constexpr double kMilkdropSoftCut        = 1.0;
  // 进程内 projectM handle 计数（libprojectM Windows/GLEW 全局指针表限制）
  std::atomic<int> gEditorProjectMInstances{0};
}  // namespace

juce::File Y2KmeterAudioProcessorEditor::FindMilkdropAssetsDir(
    const juce::String& subdir) {
  // 判空规则（与 MilkdropModule.cpp 内的 isValidAssetsDir 保持一致）：
  //   · milkdrop_presets: 至少存在 1 个 .milk 文件
  //   · milkdrop_textures: 至少存在 1 个子文件
  // 保证旧版本残留的空 AppData 目录不会屏蔽 bundle 内合法资源。
  auto isValidAssetsDir = [&](const juce::File& d) -> bool {
    if (!d.exists() || !d.isDirectory()) return false;
    if (subdir == "milkdrop_presets") {
      return d.findChildFiles(juce::File::findFiles, false, "*.milk").size() > 0;
    }
    return d.getNumberOfChildFiles(juce::File::findFiles) > 0;
  };

  // v2.3.1: 预设/纹理集中存放在 %APPDATA%\Y2Kmeter\ 下，
  //   Standalone 和 VST3 共享同一份，不再各自携带独立副本。
  //   AppData 路径在所有搜索策略中拥有最高优先级。
  //   v2.4+: AppData 必须"有效"（有 .milk / 子文件）才被采纳，
  //   否则继续 fallback 到 bundle，避免旧版本残留空目录屏蔽资源。
  juce::File appDataDir = juce::File::getSpecialLocation(
      juce::File::userApplicationDataDirectory)
      .getChildFile("Y2Kmeter")
      .getChildFile(subdir);
  if (isValidAssetsDir(appDataDir))
    return appDataDir;

 #if defined (__APPLE__)
  // macOS Seed 机制（v2.4+）：Standalone bundle 内的 milkdrop_presets
  // 首次被查找命中时同步复制到 AppData，让 VST3/AU 后续可以从共享 AppData
  // 目录读取（VST3/AU bundle 已不再内置 presets）。
  auto seedToAppDataIfNeeded = [&](const juce::File& srcDir) -> juce::File {
    if (!isValidAssetsDir(srcDir))
      return srcDir;
    if (subdir != "milkdrop_presets")
      return srcDir;
    auto parent = appDataDir.getParentDirectory();
    if (!parent.exists())
      parent.createDirectory();
    if (srcDir.copyDirectoryTo(appDataDir) && isValidAssetsDir(appDataDir))
      return appDataDir;
    return srcDir;
  };

  // macOS bundle 内置路径（优先级高于向上遍历）：
  //   可执行文件位于 Y2Kmeter.app/Contents/MacOS/Y2Kmeter，
  //   资源打包在    Y2Kmeter.app/Contents/Resources/assets/<subdir>
  //   currentExecutableFile → .../Contents/MacOS/Y2Kmeter
  //   getParentDirectory()  → .../Contents/MacOS/
  //   getParentDirectory()  → .../Contents/
  {
    auto contentsDir = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile)
        .getParentDirectory()   // MacOS/
        .getParentDirectory();  // Contents/
    auto bundleCandidate = contentsDir
        .getChildFile("Resources")
        .getChildFile("assets")
        .getChildFile(subdir);
    if (isValidAssetsDir(bundleCandidate))
      return seedToAppDataIfNeeded(bundleCandidate);
  }
  // VST3 / AU bundle 内置路径：
  //   currentApplicationFile 在插件场景下指向 .vst3/.component bundle 根，
  //   资源打包在 <bundle>/Contents/Resources/assets/<subdir>
  //   注意：v2.4+ VST3/AU bundle 已剥离 milkdrop_presets（走共享 AppData），
  //   只保留 milkdrop_textures 副本。
  {
    auto appFile = juce::File::getSpecialLocation(
        juce::File::currentApplicationFile);
    auto pluginContents = appFile.getChildFile("Contents");
    if (pluginContents.isDirectory())
    {
      auto pluginCandidate = pluginContents
          .getChildFile("Resources")
          .getChildFile("assets")
          .getChildFile(subdir);
      if (isValidAssetsDir(pluginCandidate))
        return seedToAppDataIfNeeded(pluginCandidate);
    }
  }
 #endif

  juce::File exeDir = juce::File::getSpecialLocation(
      juce::File::currentExecutableFile).getParentDirectory();
  juce::Array<juce::File> candidates;
  // 从 exe 所在目录向上逐层搜索 assets/<subdir>，最多 8 层，
  // 覆盖 CMake build tree、IDE 输出目录、安装目录等各种布局
  juce::File cur = exeDir;
  for (int i = 0; i < 8; ++i) {
    candidates.add(cur.getChildFile("assets").getChildFile(subdir));
    cur = cur.getParentDirectory();
  }
  for (auto& f : candidates) {
    if (isValidAssetsDir(f)) return f;
  }
  return {};
}

// 收藏库（like library）目录：固定位于用户数据目录 Y2Kmeter/milkdrop_presets_like，
// 与内置 milkdrop_presets 同级。该目录只由"收藏"动作创建/写入，不参与 bundle
// seed，也不会因版本更新覆盖内置预设而被清空。
static juce::File FindMilkdropLikeDir()
{
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Y2Kmeter")
      .getChildFile("milkdrop_presets_like");
}

void Y2KmeterAudioProcessorEditor::newOpenGLContextCreated() {
  // v2.9.2：调大 GL 图片纹理缓存（JUCE 默认仅 8MB）。
  //   workspace 画布背景（~5.8MB）+ 二十个模块的静态/动态图层纹理总量远超 8MB，
  //   默认缓存下每帧互相挤占 LRU —— workspace 背景纹理被逐出后整图重传
  //   （实测 10.9ms/帧，占全场景帧预算的三分之一以上）。96MB 足以容纳全部
  //   常驻图层，且只在实际用到时分配。
  openGLContext.setImageCacheSize (96 * 1024 * 1024);

  // 脱离态期间 Editor renderer 被挂起，跳过创建，避免与 GLView 的本地
  // projectM handle 共存（Windows libprojectM/GLEW 全局指针表冲突）。
  if (milkdrop_renderer_suspended_)
    return;

  auto& api = projectm_api::Api::instance();
  if (!api.isAvailable()) {
    milkdrop_error_ = api.loadError();
    return;
  }

  // 单实例防护
  int expected = 0;
  if (!gEditorProjectMInstances.compare_exchange_strong(expected, 1)) {
    milkdrop_error_ = "Only 1 Milkdrop instance allowed (libprojectM limit).";
    return;
  }

  // GLEW init（Editor GL 上下文中，一次性）
  if (!api.initGlew()) {
    milkdrop_error_ = api.loadError();
    gEditorProjectMInstances.store(0);
    return;
  }

  milkdrop_pm_handle_ = api.create();
  if (milkdrop_pm_handle_ == nullptr) {
    milkdrop_error_ = "projectm_create() returned NULL.";
    gEditorProjectMInstances.store(0);
    return;
  }

  api.setMeshSize(milkdrop_pm_handle_,
                  kMilkdropDefaultMeshW, kMilkdropDefaultMeshH);
  api.setFps(milkdrop_pm_handle_, kMilkdropTargetFps);
  api.setPresetDuration(milkdrop_pm_handle_, kMilkdropPresetDuration);
  api.setSoftCutDuration(milkdrop_pm_handle_, kMilkdropSoftCut);
  api.setHardCutEnabled(milkdrop_pm_handle_, false);

  // 纹理搜索路径
  auto texDir = FindMilkdropAssetsDir("milkdrop_textures");
  if (texDir.exists()) {
    std::vector<std::string> paths{texDir.getFullPathName().toStdString()};
    api.setTextureSearchPaths(milkdrop_pm_handle_, paths);
  }

  // 扫描预设。newOpenGLContextCreated() 在脱离态 dock 回嵌入态时会再次执行，
  // 因此每次扫描前必须清空旧列表，避免总预设数被重复追加成 2N、3N。
  RescanMilkdropPresetPaths();

  // 检查是否有待处理的预设跳转请求（Standalone 启动恢复等）。
  //   时序说明：GL 上下文可能异步创建，此时 4.5 恢复代码可能
  //   已通过 RequestMilkdropPresetJump() 入队了跳转索引。
  //   这里 atomically 取出并清零，避免后续 renderOpenGL() 重复处理。
  int pending = milkdrop_requested_preset_jump_.exchange(-1);
  if (pending >= 0 && pending < milkdrop_preset_paths_.size()) {
    milkdrop_current_preset_ = pending;
  } else {
    milkdrop_current_preset_ = 0;
  }
  LoadMilkdropPresetInternal();
  milkdrop_current_preset_ui_.store(milkdrop_current_preset_);

  // 创建 projectM 独立 offscreen FBO + 纹理附件。
  // 配合 openglRenderFrameFbo(fbo_id) 使用，projectM 渲染到此 FBO
  // 而非 FBO 0，随后 glBlitFramebuffer 跨 FBO 搬运到各模块位置，
  // 彻底消除同 FBO 上 blit 源/目重叠的未定义行为。
  juce::gl::glGenFramebuffers(1, &milkdrop_render_fbo_);
  juce::gl::glGenTextures(1, &milkdrop_render_tex_);

  // 创建整体染色后处理着色器（master output colors）。
  // 编译失败不致命，仅关闭染色能力；post FBO 纹理在 renderOpenGL 里按需分配。
  milkdrop_tint_pass_.reset(new MilkdropTintPass(openGLContext));
  if (!milkdrop_tint_pass_->init())
    milkdrop_tint_pass_.reset();

  milkdrop_render_ready_ = true;

}

void Y2KmeterAudioProcessorEditor::renderOpenGL() {
  using namespace juce::gl;

  // v2.9：真实出帧计数（GL 线程每出一帧调用一次 renderOpenGL）
  glFrameCounter.fetch_add(1, std::memory_order_relaxed);

  // v2.9.2：GL 帧耗时统计（诊断构建）
  Y2K_PERF_FRAME_SCOPE("glFrame");

  // 保存原始 viewport 和 DPI 缩放（Milkdrop 和 Spectrogram3D 共用）
  GLint orig_viewport[4];
  juce::gl::glGetIntegerv(juce::gl::GL_VIEWPORT, orig_viewport);

  float dpi_scale = static_cast<float>(openGLContext.getRenderingScale());
  int   editor_h  = getHeight();

  if (milkdrop_render_ready_ && milkdrop_pm_handle_ != nullptr) {

  auto& api = projectm_api::Api::instance();

  // 消费收藏库切换请求：重扫预设列表并恢复到切换后库之前记录的预设。
  if (milkdrop_requested_library_toggle_.exchange(false)) {
    // 切换前 milkdrop_use_like_library_ 已被 RequestMilkdropToggleLibrary 翻转成新值，
    // 因此按新值反推"旧库"，把当前索引（仍指向旧库）存入旧库的记忆。
    const bool now_like = milkdrop_use_like_library_.load();
    if (now_like)
      milkdrop_builtin_preset_index_ = milkdrop_current_preset_;
    else
      milkdrop_like_preset_index_ = milkdrop_current_preset_;

    RescanMilkdropPresetPaths();

    // 恢复到目标库（当前库）之前记录的索引，未记录或越界则用 0。
    int target = now_like ? milkdrop_like_preset_index_
                          : milkdrop_builtin_preset_index_;
    if (target < 0 || target >= milkdrop_preset_paths_.size())
      target = 0;
    milkdrop_current_preset_ = milkdrop_preset_paths_.isEmpty() ? -1 : target;

    if (milkdrop_current_preset_ >= 0) {
      LoadMilkdropPresetInternal();
      milkdrop_current_preset_ui_.store(milkdrop_current_preset_);
      milkdrop_last_preset_switch_ms_.store(
          static_cast<int64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count()));
    }
  }

  // 消费取消收藏触发的重扫请求：预设列表少一项，保持当前索引（后续预设顶上来），
  // 越界则回绕到 0；若收藏库因此被删空，则回退到内置库。
  if (milkdrop_requested_rescan_.exchange(false)) {
    RescanMilkdropPresetPaths();
    if (milkdrop_preset_paths_.isEmpty()) {
      SetMilkdropUseLikeLibrary(false);
      RescanMilkdropPresetPaths();
      int target = milkdrop_builtin_preset_index_;
      if (target < 0 || target >= milkdrop_preset_paths_.size())
        target = 0;
      milkdrop_current_preset_ = milkdrop_preset_paths_.isEmpty() ? -1 : target;
    } else if (milkdrop_current_preset_ >= milkdrop_preset_paths_.size()) {
      milkdrop_current_preset_ = 0;
    }

    if (milkdrop_current_preset_ >= 0) {
      LoadMilkdropPresetInternal();
      milkdrop_current_preset_ui_.store(milkdrop_current_preset_);
      milkdrop_last_preset_switch_ms_.store(
          static_cast<int64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count()));
    }
  }

  // 消费预设切换请求
  int jump_idx = milkdrop_requested_preset_jump_.exchange(-1);
  int delta    = milkdrop_requested_preset_delta_.exchange(0);
  bool random  = milkdrop_requested_preset_random_.exchange(false);
  bool switched = false;

  if (jump_idx >= 0 && jump_idx < milkdrop_preset_paths_.size()) {
    milkdrop_current_preset_ = jump_idx;
    switched = true;
  } else if (random) {
    const int n = milkdrop_preset_paths_.size();
    if (n <= 1) {
      // 特殊处理：目录里只有一个预设，无法排除当前，重新加载当前预设
      // （软切换刷新），给用户一次"有反馈"的随机点击。
      if (milkdrop_current_preset_ < 0 || milkdrop_current_preset_ >= n)
        milkdrop_current_preset_ = 0;
      switched = true;
    } else {
      // 排除当前预设：从 n-1 个候选中均匀随机，选中 >= 当前索引则 +1 跳过。
      const int cur = (milkdrop_current_preset_ >= 0 && milkdrop_current_preset_ < n)
                          ? milkdrop_current_preset_ : -1;
      if (cur < 0) {
        milkdrop_current_preset_ = juce::Random::getSystemRandom().nextInt(n);
      } else {
        int next = juce::Random::getSystemRandom().nextInt(n - 1);
        if (next >= cur)
          ++next;
        milkdrop_current_preset_ = next;
      }
      switched = true;
    }
  } else if (delta != 0 && !milkdrop_preset_paths_.isEmpty()) {
    milkdrop_current_preset_ = (milkdrop_current_preset_ + delta)
        % milkdrop_preset_paths_.size();
    if (milkdrop_current_preset_ < 0)
      milkdrop_current_preset_ += milkdrop_preset_paths_.size();
    switched = true;
  }

  if (switched) {
    LoadMilkdropPresetInternal();
    milkdrop_current_preset_ui_.store(milkdrop_current_preset_);
    milkdrop_last_preset_switch_ms_.store(
        static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
  }

  // 消费 PCM
  {
    std::lock_guard<std::mutex> lock(milkdrop_pcm_mutex_);
    if (milkdrop_pending_frames_ > 0) {
      api.addPcmFloat(milkdrop_pm_handle_,
                      milkdrop_pending_pcm_.data(),
                      milkdrop_pending_frames_, true);
      milkdrop_has_ever_received_pcm_ = true;
      milkdrop_last_real_pcm_ = milkdrop_pending_pcm_;
      milkdrop_last_real_frames_ = milkdrop_pending_frames_;
      milkdrop_pending_frames_ = 0;
    } else {
      // 无新音频 / 冷启动：送入静音，projectM 内部音频缓冲自然衰减，
      // bass/mid/treb → 0，驱动动画逐渐趋近静止（或渲染 idle 预设）。
      // v2.9：静音缓冲改为静态只读数组，避免 GL 线程每帧 16KB 堆分配 + 清零。
      constexpr unsigned int kSilenceFrames = 2048;
      static const std::array<float, kSilenceFrames * 2> kSilence {};
      api.addPcmFloat(milkdrop_pm_handle_, kSilence.data(),
                      kSilenceFrames, true);
    }
  }

  // projectM 渲染: openglRenderFrameFbo → offscreen FBO → 跨FBO blit 到各模块
  //   关键 API：openglRenderFrameFbo(fbo_id)——projectM 渲染到指定 FBO。
  //   openglRenderFrame() 内部 glBindFramebuffer(0)，外部 FBO 被绕过。
  //   使用 openglRenderFrameFbo 直接将 projectM 渲染到 offscreen FBO，
  //   再跨 FBO blit 到各模块在 FBO 0 (CachedImage) 上的正确位置。

  // 收集最大模块尺寸 → projectM 窗口和 offscreen FBO 大小
  int max_w = 256, max_h = 256;
  int floating_cw = 0, floating_ch = 0;  // 浮动 Milkdrop 模块 content 尺寸（独立追踪）
  {
    // v2.9：泛型递归 lambda 替代 std::function（GL 线程每帧调用，避免类型擦除 + 可能的堆分配）
    auto measureMax = [&](auto& self, juce::Component* root) -> void {
      for (int i = 0; i < root->getNumChildComponents(); ++i) {
        auto* child = root->getChildComponent(i);
        if (child == nullptr) continue;
        auto* milk = dynamic_cast<MilkdropModule*>(child);
        if (milk != nullptr && milk->isVisible() && milk->isShowing()) {
          auto cl = milk->GetContentLocalBounds();
          if (cl.getWidth() > 0 && cl.getHeight() > 0) {
            max_w = std::max(max_w, cl.getWidth());
            max_h = std::max(max_h, cl.getHeight());
          }
        } else {
          self(self, child);
        }
      }
    };
    measureMax(measureMax, this);

    // 同时扫描浮动窗口，记录其 content 尺寸供共享帧捕获
    for (auto& wnd : floatingWindows_)
    {
        if (wnd != nullptr)
        {
            if (auto* content = dynamic_cast<MilkdropModule*>(wnd->getContentComponent()))
            {
                auto cl = content->GetContentLocalBounds();
                if (cl.getWidth() > 0 && cl.getHeight() > 0)
                {
                    floating_cw = std::max(floating_cw, cl.getWidth());
                    floating_ch = std::max(floating_ch, cl.getHeight());
                    max_w = std::max(max_w, cl.getWidth());
                    max_h = std::max(max_h, cl.getHeight());
                }
            }
        }
    }
  }

  int fbo_w = static_cast<int>(max_w * dpi_scale) / milkdrop_render_scale_;
  int fbo_h = static_cast<int>(max_h * dpi_scale) / milkdrop_render_scale_;
  fbo_w = std::max(fbo_w, 1);
  fbo_h = std::max(fbo_h, 1);

  // 分配/重分配 offscreen FBO 纹理
  bool need_realloc = (milkdrop_render_fbo_ != 0 && milkdrop_last_fbo_w_ == 0);
  bool need_resize  = (milkdrop_render_fbo_ != 0
                       && (fbo_w != milkdrop_last_fbo_w_
                           || fbo_h != milkdrop_last_fbo_h_));
  if (need_realloc) {
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, milkdrop_render_fbo_);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, milkdrop_render_tex_);
    juce::gl::glTexImage2D(juce::gl::GL_TEXTURE_2D, 0, juce::gl::GL_RGBA8,
                           fbo_w, fbo_h, 0,
                           juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE,
                           nullptr);
    juce::gl::glTexParameteri(juce::gl::GL_TEXTURE_2D,
                              juce::gl::GL_TEXTURE_MIN_FILTER,
                              juce::gl::GL_LINEAR);
    juce::gl::glTexParameteri(juce::gl::GL_TEXTURE_2D,
                              juce::gl::GL_TEXTURE_MAG_FILTER,
                              juce::gl::GL_LINEAR);
    juce::gl::glFramebufferTexture2D(juce::gl::GL_FRAMEBUFFER,
                                     juce::gl::GL_COLOR_ATTACHMENT0,
                                     juce::gl::GL_TEXTURE_2D,
                                     milkdrop_render_tex_, 0);
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
  } else if (need_resize) {
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, milkdrop_render_tex_);
    juce::gl::glTexImage2D(juce::gl::GL_TEXTURE_2D, 0, juce::gl::GL_RGBA8,
                           fbo_w, fbo_h, 0,
                           juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE,
                           nullptr);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);
  }
  milkdrop_last_fbo_w_ = fbo_w;
  milkdrop_last_fbo_h_ = fbo_h;

  bool use_fbo_api = api.hasOpenglRenderFrameFbo();

  // ================================================================
  // 渲染 projectM
  // ================================================================
  if (use_fbo_api) {
    // 主路径：projectM → offscreen FBO（openglRenderFrameFbo 接受 FBO ID）
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, milkdrop_render_fbo_);
    juce::gl::glViewport(0, 0, fbo_w, fbo_h);
    juce::gl::glScissor(0, 0, fbo_w, fbo_h);
    juce::gl::glEnable(juce::gl::GL_SCISSOR_TEST);
    juce::gl::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    juce::gl::glClear(juce::gl::GL_COLOR_BUFFER_BIT);
    api.setWindowSize(milkdrop_pm_handle_,
                      static_cast<std::size_t>(fbo_w),
                      static_cast<std::size_t>(fbo_h));
    api.openglRenderFrameFbo(milkdrop_pm_handle_, milkdrop_render_fbo_);
    juce::gl::glDisable(juce::gl::GL_SCISSOR_TEST);
  } else {
    // 降级路径：projectM → FBO 0 (0,0)（openglRenderFrame 内部绑定 FBO 0）
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
    juce::gl::glViewport(0, 0, fbo_w, fbo_h);
    juce::gl::glScissor(0, 0, fbo_w, fbo_h);
    juce::gl::glEnable(juce::gl::GL_SCISSOR_TEST);
    juce::gl::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    juce::gl::glClear(juce::gl::GL_COLOR_BUFFER_BIT);
    api.setWindowSize(milkdrop_pm_handle_,
                      static_cast<std::size_t>(fbo_w),
                      static_cast<std::size_t>(fbo_h));
    api.openglRenderFrame(milkdrop_pm_handle_);
    juce::gl::glDisable(juce::gl::GL_SCISSOR_TEST);

    // FBO 0 (0,0) → offscreen FBO（跨 FBO，安全）
    juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, 0);
    juce::gl::glBindFramebuffer(juce::gl::GL_DRAW_FRAMEBUFFER,
                                 milkdrop_render_fbo_);
    juce::gl::glBlitFramebuffer(0, 0, fbo_w, fbo_h, 0, 0, fbo_w, fbo_h,
                                 juce::gl::GL_COLOR_BUFFER_BIT,
                                 juce::gl::GL_LINEAR);
  }

  // ================================================================
  // 整体染色后处理（master output colors）：projectM → render FBO → post FBO → 各模块
  // ================================================================
  const auto visual_state = GetMilkdropVisualState();
  const bool tint_active = !visual_state.isNeutral();

  GLuint blit_source_fbo = milkdrop_render_fbo_;
  if (tint_active) {
    // 分配 / 重分配 post FBO（与 render FBO 同尺寸）
    if (milkdrop_post_fbo_ == 0) {
      juce::gl::glGenFramebuffers(1, &milkdrop_post_fbo_);
      juce::gl::glGenTextures(1, &milkdrop_post_tex_);
    }
    if (milkdrop_post_w_ != fbo_w || milkdrop_post_h_ != fbo_h) {
      juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, milkdrop_post_tex_);
      juce::gl::glTexImage2D(juce::gl::GL_TEXTURE_2D, 0, juce::gl::GL_RGBA8,
                             fbo_w, fbo_h, 0,
                             juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE, nullptr);
      juce::gl::glTexParameteri(juce::gl::GL_TEXTURE_2D,
                                juce::gl::GL_TEXTURE_MIN_FILTER,
                                juce::gl::GL_LINEAR);
      juce::gl::glTexParameteri(juce::gl::GL_TEXTURE_2D,
                                juce::gl::GL_TEXTURE_MAG_FILTER,
                                juce::gl::GL_LINEAR);
      juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, milkdrop_post_fbo_);
      juce::gl::glFramebufferTexture2D(juce::gl::GL_FRAMEBUFFER,
                                       juce::gl::GL_COLOR_ATTACHMENT0,
                                       juce::gl::GL_TEXTURE_2D,
                                       milkdrop_post_tex_, 0);
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
      milkdrop_post_w_ = fbo_w;
      milkdrop_post_h_ = fbo_h;
    }

    // 后处理：采样 render FBO 纹理 → post FBO，统一 pass 应用完整视觉状态。
    if (milkdrop_tint_pass_ != nullptr && milkdrop_tint_pass_->isReady()) {
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, milkdrop_post_fbo_);
      juce::gl::glViewport(0, 0, fbo_w, fbo_h);
      milkdrop_tint_pass_->apply(milkdrop_render_tex_, visual_state);
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
      blit_source_fbo = milkdrop_post_fbo_;
    }
  }

  // ================================================================
  // v2.9.3：GL 直渲染 z 序统一遍历（Milkdrop blit + S3D 条带按组件树
  //   顺序绘制，重叠区域由 z 序决定遮挡 —— 聚焦模块覆盖另一个）
  // ================================================================
  renderGlDirectModulesZOrdered(dpi_scale, editor_h,
                                (int)(editor_h * dpi_scale),
                                true, blit_source_fbo);

  // ================================================================
  // 浮动 Milkdrop 窗口共享帧捕获
  //   离线 FBO → glReadPixels → juce::Image（线程安全写入）
  //   浮动窗口的 MilkdropModule::paintContent() 读取此帧并绘制
  // ================================================================
  if (floating_milkdrop_count_ > 0)
  {
    juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, milkdrop_render_fbo_);
    // ★ 共享帧捕获使用浮动模块自身 content 尺寸（与 measureMax 中追踪的
    //   floating_cw/floating_ch 一致），而非整个 FBO 尺寸。因 FBO 基于所有
    //   模块的 max_w/max_h，不同模块长宽比可能不同，直接捕整个 FBO 会导致
    //   paintContent 中 stretchToFit 非等比拉伸 → STALE_FRAME → 黑屏。
    const int capture_w = juce::jmax(
        static_cast<int>(floating_cw * dpi_scale) / milkdrop_render_scale_, 1);
    const int capture_h = juce::jmax(
        static_cast<int>(floating_ch * dpi_scale) / milkdrop_render_scale_, 1);
    {
      std::lock_guard<std::mutex> lock(milkdrop_shared_frame_mutex_);

      // 确保共享帧 Image 尺寸匹配
      const bool frameNeedsResize = (milkdrop_shared_frame_.getWidth() != capture_w
          || milkdrop_shared_frame_.getHeight() != capture_h
          || milkdrop_shared_frame_.isNull());
      if (frameNeedsResize)
      {
        milkdrop_shared_frame_ = juce::Image(juce::Image::ARGB, capture_w, capture_h, true);
        milkdrop_capture_buf_size_ = 0;  // 强制重新分配
      }

      // 预分配原始 RGBA 缓冲区（只分配一次，尺寸不变时复用）
      const int bufSize = capture_w * capture_h * 4;
      if (milkdrop_capture_buf_size_ < bufSize)
      {
        milkdrop_capture_buf_.reset(new uint8_t[bufSize]);
        milkdrop_capture_buf_size_ = bufSize;
      }

      // 同步回读 FBO → CPU 缓冲区（仅在浮动 Milkdrop 存在时执行）
      juce::gl::glReadPixels(0, 0, capture_w, capture_h,
                              juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE,
                              milkdrop_capture_buf_.get());

      // RGBA → ARGB 像素交换并写入 juce::Image。
      // GL 帧的 alpha 在不同显卡/驱动下可能为 0 或未定义；浮动窗口使用 JUCE
      // 2D 合成绘制时必须强制为不透明，否则放大后的新增区域会表现为透明或纯色块。
      {
        juce::Image::BitmapData bd(milkdrop_shared_frame_,
                                    juce::Image::BitmapData::writeOnly);
        auto* src = milkdrop_capture_buf_.get();
        for (int y = 0; y < capture_h; ++y)
        {
          auto* dst = bd.data + y * bd.lineStride;
          for (int x = 0; x < capture_w; ++x)
          {
            dst[0] = src[2];  // R
            dst[1] = src[1];  // G
            dst[2] = src[0];  // B
            dst[3] = 0xff;    // A：强制不透明
            src += 4;
            dst += 4;
          }
        }
      }
    }
    juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, 0);

    juce::MessageManager::callAsync([this]() {
        for (auto& wnd : floatingWindows_) {
            if (wnd != nullptr
                && dynamic_cast<MilkdropModule*>(wnd->getContentComponent()) != nullptr) {
                wnd->getContentComponent()->repaint();
            }
        }
    });
  }

  juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, 0);

  } // if Milkdrop ready
  else
  {
    // Milkdrop 未就绪/挂起：仍要画 S3D（glDirectActive 已接管的模块不能变透明）
    renderGlDirectModulesZOrdered(dpi_scale, editor_h,
                                  (int)(editor_h * dpi_scale),
                                  false, 0);
  }

  // 恢复原始 viewport，供 JUCE CPE 渲染子组件 CachedImage
  juce::gl::glViewport(orig_viewport[0], orig_viewport[1],
                        orig_viewport[2], orig_viewport[3]);
}

// ================================================================
// v2.9.3 GL 直渲染（z 序统一遍历）
//   在 renderOpenGL（GL 线程、FBO 0 后缓冲）里按组件树顺序统一绘制：
//     · Milkdrop 模块 → projectM 帧 blit；
//     · Spectrogram3D 模块 → VBO 直画凹陷背景 + 条带 + 顶部轮廓线。
//   一次遍历保证重叠区域由组件 z 序决定遮挡（聚焦模块覆盖另一个），
//   而不是固定"Milkdrop 先画、S3D 后画"。
//   shader 编译失败 → s3dGlReady=false → S3D 模块保持旧软件渲染路径。
// ================================================================
void Y2KmeterAudioProcessorEditor::renderGlDirectModulesZOrdered (float dpiScale, int editorLogicalH,
                                                                  int editorPhysH,
                                                                  bool milkdropReady,
                                                                  unsigned int milkdropBlitSourceFbo)
{
  using namespace juce::gl;

  // v2.9.3 诊断开关：Y2K_NO_S3D_GL 非空时禁用 S3D GL 直渲染（模块保持旧路径）
  static const bool glDisabled = juce::SystemStats::getEnvironmentVariable ("Y2K_NO_S3D_GL", "").isNotEmpty();
  const bool s3dEnabled = ! glDisabled;

  if (s3dEnabled && ! s3dShaderUseChecked)
  {
    s3dShaderUseChecked = true;

    const char* vs =
      "attribute vec2 position;\n"
      "attribute vec4 colour;\n"
      "uniform vec4 transform;\n"   // xy = scale, zw = offset（物理像素）
      "uniform vec4 ndcScale;\n"    // xy = 2/w, 2/h；zw = -1, -1
      "uniform float snapMode;\n"   // >0.5：顶点对齐物理像素中心（像素风细线防混合闪动）
      "varying vec4 vColour;\n"
      "void main()\n"
      "{\n"
      "  vec2 px = position * transform.xy + transform.zw;\n"
      "  if (snapMode > 0.5) px = floor (px) + vec2 (0.5);\n"
      "  gl_Position = vec4 (px * ndcScale.xy + ndcScale.zw, 0.0, 1.0);\n"
      "  vColour = colour;\n"
      "}\n";
    const char* fs =
      "varying vec4 vColour;\n"
      "void main() { gl_FragColor = vColour; }\n";

    s3dShader = std::make_unique<juce::OpenGLShaderProgram> (openGLContext);
    if (s3dShader->addVertexShader (vs)
        && s3dShader->addFragmentShader (fs)
        && s3dShader->link())
    {
      s3dShader->use();
      s3dTransformUniform.reset (new juce::OpenGLShaderProgram::Uniform (*s3dShader, "transform"));
      s3dNdcUniform.reset (new juce::OpenGLShaderProgram::Uniform (*s3dShader, "ndcScale"));
      s3dSnapUniform.reset (new juce::OpenGLShaderProgram::Uniform (*s3dShader, "snapMode"));
      glUseProgram (s3dShader->getProgramID());
      s3dPosAttrib = (int) glGetAttribLocation (s3dShader->getProgramID(), "position");
      s3dColAttrib = (int) glGetAttribLocation (s3dShader->getProgramID(), "colour");

      // VAO（attrib 布局容器；VBO 为 per-module 懒创建，避免多模块共享单
      //   VBO 时"版本化跳过上传"导致画了别的模块的数据）
      glGenVertexArrays (1, &s3dVao);
      glBindVertexArray (s3dVao);
      glBindVertexArray (0);
      glBindBuffer (GL_ARRAY_BUFFER, 0);
      // v2.9.5：临时 quad VBO（纹理 quad / trail 衰减 quad 专用，每次上传）
      glGenBuffers (1, &s3dQuadVbo);

      s3dGlReady = true;
    }

    // ---- v2.9.5：纹理 shader（纹理通道 + trail FBO 合成）----
    const char* tvs =
      "attribute vec2 position;\n"
      "attribute vec2 texCoord;\n"
      "uniform vec4 transform;\n"
      "uniform vec4 ndcScale;\n"
      "varying vec2 vUv;\n"
      "void main()\n"
      "{\n"
      "  vec2 px = position * transform.xy + transform.zw;\n"
      "  gl_Position = vec4 (px * ndcScale.xy + ndcScale.zw, 0.0, 1.0);\n"
      "  vUv = texCoord;\n"
      "}\n";
    const char* tfs =
      "varying vec2 vUv;\n"
      "uniform sampler2D tex;\n"
      "uniform vec2 texWindow;\n"   // x = uv 窗口起点，y = uv 窗口宽度
      "void main()\n"
      "{\n"
      "  vec2 uv = vec2 (vUv.x * texWindow.y + texWindow.x, vUv.y);\n"
      "  gl_FragColor = texture2D (tex, uv);\n"
      "}\n";

    s3dTexShader = std::make_unique<juce::OpenGLShaderProgram> (openGLContext);
    if (s3dTexShader->addVertexShader (tvs)
        && s3dTexShader->addFragmentShader (tfs)
        && s3dTexShader->link())
    {
      s3dTexShader->use();
      s3dTexTransformUniform.reset (new juce::OpenGLShaderProgram::Uniform (*s3dTexShader, "transform"));
      s3dTexNdcUniform.reset (new juce::OpenGLShaderProgram::Uniform (*s3dTexShader, "ndcScale"));
      s3dTexWindowUniform.reset (new juce::OpenGLShaderProgram::Uniform (*s3dTexShader, "texWindow"));
      glUseProgram (s3dTexShader->getProgramID());
      s3dTexPosAttrib = (int) glGetAttribLocation (s3dTexShader->getProgramID(), "position");
      s3dTexUvAttrib = (int) glGetAttribLocation (s3dTexShader->getProgramID(), "texCoord");
      if (s3dTexPosAttrib < 0 || s3dTexUvAttrib < 0)
        s3dTexShader.reset();   // 纹理 shader 不可用 → 纹理通道退化（模块走旧路径）
    }
    else
    {
      s3dTexShader.reset();
    }
  }

  const bool s3dOk = s3dEnabled && s3dGlReady;
  const bool milkOk = milkdropReady;

  if (! s3dOk && ! milkOk)
    return;

  const int vw = juce::jmax (1, juce::roundToInt ((float) getWidth() * dpiScale));
  const int vh = juce::jmax (1, editorPhysH);

  // 固定画到后缓冲 FBO 0（blit 的 READ 绑定由调用方设置）
  glBindFramebuffer (GL_FRAMEBUFFER, 0);
  glViewport (0, 0, vw, vh);

  // v2.9.3：projectM 是 3D 渲染，运行后会残留 GL_CULL_FACE /
  //   GL_SCISSOR_TEST 等状态。Y 翻转后条带三角形绕序变成"背面"，若 cull 未关
  //   会被全部剔除；scissor 残留则会把绘制裁剪掉。2D 直渲染不需要这些状态。
  glDisable (GL_CULL_FACE);
  glDisable (GL_SCISSOR_TEST);
  glDisable (GL_STENCIL_TEST);
  glDisable (GL_DEPTH_TEST);
  glEnable (GL_BLEND);
  glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (s3dOk)
  {
    s3dShader->use();
    s3dNdcUniform->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
    glBindVertexArray (s3dVao);
  }

  // v2.9.4：统一 GL 直渲染入口 —— 所有 GlDirectModuleBase 模块共用同一条
  //   "transform + 版本化上传 + 按 segments 逐段 draw"路径（此前三个模块
  //   各复制一份 lambda）。
  //   renderToLogical 模式：先渲染到模块专属的逻辑分辨率 FBO，再线性放大
  //   blit 到后缓冲 —— 与旧版"CPU 光栅化到逻辑纹理 + 纹理放大"逐像素等效，
  //   解决 150% DPI 下 1px 柱在物理网格上的交替混合闪动。
  auto renderOneGlModule = [&] (GlDirectModuleBase& mod)
  {
    auto snap = mod.getGlSnapshot();
    if (snap == nullptr || snap->segments.empty())
      return;
    if (snap->vertices.empty() && snap->texW <= 0 && ! snap->trailFbo)
      return;

    // v2.9.5：每个模块开始前恢复默认渲染目标与视口 —— renderToLogical 分支
    //   结束时 GL_FRAMEBUFFER 仍绑定模块 FBO（blit 只解绑了 READ/DRAW 别名），
    //   下一个走直接路径的模块若不恢复，会把内容画进上一个模块的 FBO、
    //   且视口仍是 FBO 尺寸 → "底图画大、盖住所有模块"的根因。
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, vw, vh);

    // VBO 版本化上传 —— 快照未变时跳过，只复用旧 VBO 内容重画。
    //   每个模块独立 VBO（懒创建）：共享单 VBO 时，跳过上传的模块会画出
    //   "别的模块刚上传的数据"。
    const bool needUpload = snap->version != mod.lastUploadedVersion;
    if (mod.glVbo == 0)
      glGenBuffers (1, &mod.glVbo);

    auto setVertexAttribs = [&] ()
    {
      glBindBuffer (GL_ARRAY_BUFFER, mod.glVbo);
      glVertexAttribPointer ((GLuint) s3dPosAttrib, 2, GL_FLOAT, GL_FALSE, 24, nullptr);
      glEnableVertexAttribArray ((GLuint) s3dPosAttrib);
      glVertexAttribPointer ((GLuint) s3dColAttrib, 4, GL_FLOAT, GL_FALSE, 24, (void*) 8);
      glEnableVertexAttribArray ((GLuint) s3dColAttrib);
    };

    // 纹理 quad 绘制（纹理通道显示 / trail FBO 合成共用）
    auto drawTexQuad = [&] (GLuint tex, juce::Rectangle<int> target,
                            float scrollU, float winW)
    {
      const auto tl = getLocalPoint (&mod, juce::Point<int>());
      glActiveTexture (GL_TEXTURE0);
      glBindTexture (GL_TEXTURE_2D, tex);
      s3dTexShader->use();
      s3dTexNdcUniform->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
      s3dTexTransformUniform->set (dpiScale, -dpiScale,
                                   (float) tl.x * dpiScale,
                                   (float) editorPhysH - (float) tl.y * dpiScale);
      s3dTexWindowUniform->set (scrollU, winW);
      const float x0 = (float) target.getX(), y0 = (float) target.getY();
      const float x1 = (float) target.getRight(), y1 = (float) target.getBottom();
      // v2.9.5 修复：GL 纹理 v=0 是内存第一行（FBO 渲染时为底部）——
      //   屏幕顶(y0)必须配 uv(0,1)（纹理顶），否则内容上下颠倒
      //   （StereoField trail 合成实测点云翻转）。
      const float quad[24] = {
        x0, y0, 0.0f, 1.0f,  x1, y0, 1.0f, 1.0f,  x0, y1, 0.0f, 0.0f,
        x1, y0, 1.0f, 1.0f,  x1, y1, 1.0f, 0.0f,  x0, y1, 0.0f, 0.0f,
      };
      // v2.9.5：用编辑器级 s3dQuadVbo（不能借模块 glVbo —— 模块 VBO 的
      //   版本化复用会被 quad 数据覆盖，needUpload=false 帧画错）
      glBindBuffer (GL_ARRAY_BUFFER, s3dQuadVbo);
      glVertexAttribPointer ((GLuint) s3dTexPosAttrib, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
      glEnableVertexAttribArray ((GLuint) s3dTexPosAttrib);
      glVertexAttribPointer ((GLuint) s3dTexUvAttrib, 2, GL_FLOAT, GL_FALSE, 16, (void*) 8);
      glEnableVertexAttribArray ((GLuint) s3dTexUvAttrib);
      glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_DYNAMIC_DRAW);
      glDrawArrays (GL_TRIANGLES, 0, 6);
    };

    if (snap->renderToLogical && snap->logicalW > 0 && snap->logicalH > 0)
    {
      // 恢复主 shader（上一模块的纹理 quad 可能切到了纹理 shader）
      s3dShader->use();

      // 2x 超采样（SSAA）：FBO 渲染 2 倍逻辑分辨率，blit 时线性缩小 ——
      //   缩小滤波天然提供抗锯齿（旧版 JUCE path 渲染带 AA；顶点流是硬边
      //   三角形，直接渲染会呈现 1px 台阶锯齿）。纯 2D 机制，无 MSAA 的
      //   驱动兼容性风险（上一版 GL_TEXTURE_2D_MULTISAMPLE 在部分驱动上
      //   创建失败导致 FBO 不完整 → 全部模块透明）。
      constexpr int kSsaa = 2;
      const int fboW = snap->logicalW * kSsaa;
      const int fboH = snap->logicalH * kSsaa;

      // 懒创建/重建逻辑分辨率 FBO
      if (mod.glFbo == 0 || mod.glFboW != fboW || mod.glFboH != fboH)
      {
        if (mod.glFbo != 0)
        {
          glDeleteFramebuffers (1, &mod.glFbo);
          glDeleteTextures (1, &mod.glFboTex);
          mod.glFbo = 0;
          mod.glFboTex = 0;
        }
        glGenFramebuffers (1, &mod.glFbo);
        glGenTextures (1, &mod.glFboTex);
        glBindTexture (GL_TEXTURE_2D, mod.glFboTex);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
                      fboW, fboH, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture (GL_TEXTURE_2D, 0);
        glBindFramebuffer (GL_FRAMEBUFFER, mod.glFbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, mod.glFboTex, 0);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        mod.glFboW = fboW;
        mod.glFboH = fboH;
      }

      // 渲染到逻辑 FBO（模块本地坐标 → FBO：2x 缩放、x 减 canvas 偏移、
      //   Y 翻转；背景 quad 每帧全量覆盖，无需 clear）
      glBindFramebuffer (GL_FRAMEBUFFER, mod.glFbo);
      glViewport (0, 0, fboW, fboH);
      s3dNdcUniform->set (2.0f / (float) fboW, 2.0f / (float) fboH, -1.0f, -1.0f);
      s3dTransformUniform->set ((float) kSsaa, (float) -kSsaa,
                                -(float) snap->canvasLocal.getX() * (float) kSsaa,
                                (float) (fboH + snap->canvasLocal.getY() * kSsaa));
      s3dSnapUniform->set (0.0f);

      // v2.9.5：显式设置 attrib（上一模块若是纹理/trail 路径，drawTexQuad
      //   会把 attrib 切成"纹理布局（stride 16B、指向 s3dQuadVbo）"，z 序
      //   变化后本分支若不重设，会用错误布局读 VBO → 顶点/颜色全错（黄斑））
      setVertexAttribs();
      if (needUpload)
        glBufferData (GL_ARRAY_BUFFER,
                      (GLsizeiptr) (snap->vertices.size() * sizeof (float)),
                      snap->vertices.data(), GL_DYNAMIC_DRAW);
      for (const auto& seg : snap->segments)
        glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                      seg.offset, seg.count);

      // 线性缩小 blit 到后缓冲的模块 canvas 物理区域（2x 缩小 = 4 采样 box AA）
      const auto cPos = getLocalPoint (&mod, snap->canvasLocal.getPosition());
      const auto cBR  = getLocalPoint (&mod, snap->canvasLocal.getBottomRight());
      glBindFramebuffer (GL_READ_FRAMEBUFFER, mod.glFbo);
      glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
      glViewport (0, 0, vw, vh);
      glBlitFramebuffer (0, 0, fboW, fboH,
                         (int) (cPos.x * dpiScale),
                         (int) (editorPhysH - cBR.y * dpiScale),
                         (int) (cBR.x * dpiScale),
                         (int) (editorPhysH - cPos.y * dpiScale),
                         GL_COLOR_BUFFER_BIT, GL_LINEAR);
      glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);
    }
    else if (snap->trailFbo && snap->texW > 0 && snap->texH > 0 && s3dTexShader != nullptr)
    {
      // ============ trail 累积通道（v2.9.5，StereoField 残影）============
      //   1) 持久 trail FBO（1x 逻辑尺寸，不清 —— 衰减 + 累积）
      if (mod.glFbo == 0 || mod.glFboW != snap->texW || mod.glFboH != snap->texH)
      {
        if (mod.glFbo != 0)
        {
          glDeleteFramebuffers (1, &mod.glFbo);
          glDeleteTextures (1, &mod.glFboTex);
          mod.glFbo = 0;
          mod.glFboTex = 0;
        }
        glGenFramebuffers (1, &mod.glFbo);
        glGenTextures (1, &mod.glFboTex);
        glBindTexture (GL_TEXTURE_2D, mod.glFboTex);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
                      snap->texW, snap->texH, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture (GL_TEXTURE_2D, 0);
        glBindFramebuffer (GL_FRAMEBUFFER, mod.glFbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, mod.glFboTex, 0);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        mod.glFboW = snap->texW;
        mod.glFboH = snap->texH;
        mod.glTrailCleared = false;
      }

      glBindFramebuffer (GL_FRAMEBUFFER, mod.glFbo);
      glViewport (0, 0, snap->texW, snap->texH);
      if (! mod.glTrailCleared)
      {
        glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        mod.glTrailCleared = true;
      }

      //   2) 衰减 pass：覆盖 quad，blend (ZERO, ONE_MINUS_SRC_ALPHA) → dst×decay
      s3dShader->use();
      s3dNdcUniform->set (2.0f / (float) snap->texW, 2.0f / (float) snap->texH, -1.0f, -1.0f);
      s3dTransformUniform->set (1.0f, -1.0f, 0.0f, (float) snap->texH);
      s3dSnapUniform->set (0.0f);
      glBlendFunc (GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
      {
        const float w = (float) snap->texW, h = (float) snap->texH;
        const float a = 1.0f - snap->trailDecay;
        const float quad[36] = {
          0.0f, 0.0f, 1, 1, 1, a,   w, 0.0f, 1, 1, 1, a,   0.0f, h, 1, 1, 1, a,
          w, 0.0f, 1, 1, 1, a,      w, h, 1, 1, 1, a,      0.0f, h, 1, 1, 1, a,
        };
        // v2.9.5：衰减 quad 用 s3dQuadVbo（勿污染模块 glVbo）
        glBindBuffer (GL_ARRAY_BUFFER, s3dQuadVbo);
        glVertexAttribPointer ((GLuint) s3dPosAttrib, 2, GL_FLOAT, GL_FALSE, 24, nullptr);
        glEnableVertexAttribArray ((GLuint) s3dPosAttrib);
        glVertexAttribPointer ((GLuint) s3dColAttrib, 4, GL_FLOAT, GL_FALSE, 24, (void*) 8);
        glEnableVertexAttribArray ((GLuint) s3dColAttrib);
        glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_DYNAMIC_DRAW);
        glDrawArrays (GL_TRIANGLES, 0, 6);
      }

      //   3) 点 pass：trail 段（叠加累积；点顶点为模块本地坐标，
      //      经 canvasLocal 偏移换算到 trail FBO 坐标）
      glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      s3dTransformUniform->set (1.0f, -1.0f,
                                -(float) snap->canvasLocal.getX(),
                                (float) (snap->texH + snap->canvasLocal.getY()));
      setVertexAttribs();
      if (needUpload)
        glBufferData (GL_ARRAY_BUFFER,
                      (GLsizeiptr) (snap->vertices.size() * sizeof (float)),
                      snap->vertices.data(), GL_DYNAMIC_DRAW);
      for (size_t si = (size_t) snap->trailSegStart; si < snap->segments.size(); ++si)
      {
        const auto& seg = snap->segments[si];
        glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                      seg.offset, seg.count);
      }

      //   4) 背景段直接画后缓冲
      glBindFramebuffer (GL_FRAMEBUFFER, 0);
      glViewport (0, 0, vw, vh);
      {
        const auto tl = getLocalPoint (&mod, juce::Point<int>());
        s3dNdcUniform->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
        s3dTransformUniform->set (dpiScale, -dpiScale,
                                  (float) tl.x * dpiScale,
                                  (float) editorPhysH - (float) tl.y * dpiScale);
        s3dSnapUniform->set (snap->snapToPixels ? 1.0f : 0.0f);
        setVertexAttribs();
        for (size_t si = 0; si < (size_t) snap->trailSegStart && si < snap->segments.size(); ++si)
        {
          const auto& seg = snap->segments[si];
          glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                        seg.offset, seg.count);
        }
      }

      //   5) 合成：trail 纹理 alpha 混合到 texTargetRect
      glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      drawTexQuad (mod.glFboTex, snap->texTargetRect, 0.0f, 1.0f);
    }
    else
    {
      // 直接渲染到后缓冲：快照顶点坐标是"模块本地"坐标（含 canvas 偏移），
      //   transform 原点取模块左上角 (0,0)。
      // v2.9.5 修复（两处状态残留，都必须在每个分支显式设置）：
      //   1) use 顺序：Uniform::set 写入"当前绑定的 program"，上一模块（trail/
      //      纹理路径）结束时当前 program 是纹理 shader —— 先 set 会写到错误
      //      program，主 shader 的 transform 残留上一模块的 FBO 局部变换；
      //   2) NDC：上一模块（renderToLogical）留下的是 FBO 尺寸 NDC —— 不重设
      //      会让背景顶点被错误缩放画飞（四周透底 + 巨大错位底图）。
      const auto tl = getLocalPoint (&mod, juce::Point<int>());
      s3dShader->use();
      s3dNdcUniform->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
      s3dTransformUniform->set (dpiScale, -dpiScale,
                                (float) tl.x * dpiScale,
                                (float) editorPhysH - (float) tl.y * dpiScale);
      s3dSnapUniform->set (snap->snapToPixels ? 1.0f : 0.0f);

      // 顶点段（背景/边框）先画 —— 背景 9 quad 含全 canvas 底色，若画在纹理
      //   之后会把瀑布图整个盖住
      setVertexAttribs();
      if (needUpload)
        glBufferData (GL_ARRAY_BUFFER,
                      (GLsizeiptr) (snap->vertices.size() * sizeof (float)),
                      snap->vertices.data(), GL_DYNAMIC_DRAW);
      for (const auto& seg : snap->segments)
        glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                      seg.offset, seg.count);

      // 纹理通道：上传 + quad（画在背景之后 —— 瀑布图覆盖 content 底色）
      if (snap->texW > 0 && snap->texH > 0 && s3dTexShader != nullptr
          && ! snap->texPixels.empty())
      {
        if (mod.glTex == 0)
          glGenTextures (1, &mod.glTex);
        glBindTexture (GL_TEXTURE_2D, mod.glTex);
        if (snap->texDirty)
        {
          // 像素为 0xAARRGGBB（juce::Colour::getPixelARGB），小端内存字节序
          //   即 B,G,R,A → GL_BGRA 上传避免字节重排。
          // 最近邻过滤（与旧版 lowResamplingQuality 一致）：像素风瀑布图
          //   线性采样会在双拷贝环形窗口边缘混入镜像列 → 边缘 1px"拖影"。
          glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
                        snap->texW, snap->texH, 0,
                        GL_BGRA, GL_UNSIGNED_BYTE, snap->texPixels.data());
          glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
          glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
          glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture (GL_TEXTURE_2D, 0);
        drawTexQuad (mod.glTex, snap->texTargetRect, snap->texScrollU, snap->texWinW);
        s3dShader->use();   // 恢复主 shader（下一模块开头也会再设）
      }
    }

    if (needUpload)
      mod.lastUploadedVersion = snap->version;

    // 首次成功绘制后接管组件绘制路径，并强制组件缓存重绘 —— 接管标志
    //   变化后组件缓存的"软件路径不透明内容"若不同步失效，会盖住 GL 内容
    //   （dock 回来图像丢失/拖影的根源；repaint 线程安全，GL 线程可调）。
    if (! mod.glDirectActive.load())
    {
      mod.glDirectActive.store (true);
      mod.repaint();
    }
  };

  // 统一 z 序遍历：组件树先序（兄弟按索引序）即 z 从低到高，
  //   重叠区域后画的覆盖先画的 —— 聚焦（toFront）模块自然在上层。
  //   Milkdrop（FBO 渲染 + blit）与 GlDirectModuleBase（顶点流）在此同权参与。
  auto walk = [&] (auto& self, juce::Component* root) -> void
  {
    for (int i = 0; i < root->getNumChildComponents(); ++i)
    {
      auto* child = root->getChildComponent (i);
      if (child == nullptr)
        continue;

      if (auto* milk = dynamic_cast<MilkdropModule*> (child))
      {
        if (milkOk && milk->isVisible() && milk->isShowing())
        {
          auto cl = milk->GetContentLocalBounds();
          auto tl = getLocalPoint (milk, cl.getPosition());
          auto br = getLocalPoint (milk, cl.getBottomRight());
          int lw = br.x - tl.x, lh = br.y - tl.y;
          if (lw > 0 && lh > 0)
          {
            int gx = static_cast<int> (tl.x * dpiScale);
            int gy = static_cast<int> ((editorLogicalH - (tl.y + lh)) * dpiScale);
            int gw = static_cast<int> (lw * dpiScale);
            int gh = static_cast<int> (lh * dpiScale);

            int sw = gw / milkdrop_render_scale_;
            int sh = gh / milkdrop_render_scale_;
            sw = std::max (sw, 1);
            sh = std::max (sh, 1);

            glBindFramebuffer (GL_READ_FRAMEBUFFER, milkdropBlitSourceFbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer (0, 0, sw, sh,
                               gx, gy, gx + gw, gy + gh,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
          }
        }
      }
      else if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (child))
      {
        if (s3dOk && gmod->isVisible() && gmod->isShowing())
          renderOneGlModule (*gmod);
      }
      else
      {
        self (self, child);
      }
    }
  };
  walk (walk, this);

  // v2.9.3：恢复 GL 状态。JUCE 组件绘制（paintComponent）在本函数之后执行，
  //   其状态机假设"干净"的初始状态 —— 残留 program / blend / VAO 会让组件
  //   绘制命令走错 shader 路径（示波器 paint 实测因此慢 3 倍）。
  if (s3dOk)
  {
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
    glUseProgram (0);
    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // JUCE GL 渲染器的预乘约定
  }
}

void Y2KmeterAudioProcessorEditor::openGLContextClosing() {
  // v2.9.4：清理 GL 直渲染资源（VAO + 各 GlDirectModuleBase 模块专属 VBO）
  if (s3dVao != 0) { juce::gl::glDeleteVertexArrays (1, &s3dVao); s3dVao = 0; }
  if (s3dQuadVbo != 0) { juce::gl::glDeleteBuffers (1, &s3dQuadVbo); s3dQuadVbo = 0; }
  {
    auto releaseVbos = [&] (auto& self, juce::Component* root) -> void
    {
      for (int i = 0; i < root->getNumChildComponents(); ++i)
      {
        auto* child = root->getChildComponent (i);
        if (child == nullptr) continue;
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (child))
        {
          if (gmod->glVbo != 0) { juce::gl::glDeleteBuffers (1, &gmod->glVbo); gmod->glVbo = 0; }
          if (gmod->glFbo != 0)
          {
            juce::gl::glDeleteFramebuffers (1, &gmod->glFbo);
            juce::gl::glDeleteTextures (1, &gmod->glFboTex);
            gmod->glFbo = 0;
            gmod->glFboTex = 0;
          }
          if (gmod->glTex != 0) { juce::gl::glDeleteTextures (1, &gmod->glTex); gmod->glTex = 0; }
        }
        else
        {
          self (self, child);
        }
      }
    };
    releaseVbos (releaseVbos, this);
  }
  s3dShader.reset();
  s3dTransformUniform.reset();
  s3dNdcUniform.reset();
  s3dSnapUniform.reset();
  s3dTexShader.reset();
  s3dTexTransformUniform.reset();
  s3dTexNdcUniform.reset();
  s3dTexWindowUniform.reset();
  s3dGlReady = false;

  if (milkdrop_pm_handle_ != nullptr) {
    auto& api = projectm_api::Api::instance();
    api.destroy(milkdrop_pm_handle_);
    milkdrop_pm_handle_ = nullptr;
    gEditorProjectMInstances.store(0);
    api.resetGlewInitialization();
  }
  // 销毁整体染色后处理着色器与 post FBO
  if (milkdrop_tint_pass_ != nullptr) {
    milkdrop_tint_pass_->shutdown();
    milkdrop_tint_pass_.reset();
  }
  if (milkdrop_post_fbo_ != 0) {
    juce::gl::glDeleteFramebuffers(1, &milkdrop_post_fbo_);
    milkdrop_post_fbo_ = 0;
  }
  if (milkdrop_post_tex_ != 0) {
    juce::gl::glDeleteTextures(1, &milkdrop_post_tex_);
    milkdrop_post_tex_ = 0;
  }
  milkdrop_post_w_ = 0;
  milkdrop_post_h_ = 0;
  if (milkdrop_render_fbo_ != 0) {
    juce::gl::glDeleteFramebuffers(1, &milkdrop_render_fbo_);
    milkdrop_render_fbo_ = 0;
  }
  if (milkdrop_render_tex_ != 0) {
    juce::gl::glDeleteTextures(1, &milkdrop_render_tex_);
    milkdrop_render_tex_ = 0;
  }
  milkdrop_preset_paths_.clear();
  milkdrop_current_preset_ = 0;
  milkdrop_current_preset_ui_.store(-1);
  milkdrop_last_fbo_w_ = 0;
  milkdrop_last_fbo_h_ = 0;
  milkdrop_render_ready_ = false;
}

void Y2KmeterAudioProcessorEditor::SuspendMilkdropEditorRendererForFloating() {
  // 先置挂起标志：即便 Editor GL 上下文尚未 attach、Editor handle 尚未创建，
  // 也要阻止后续异步触发的 newOpenGLContextCreated 再创建 Editor handle，
  // 避免与 GLView 的本地 projectM handle 共存导致 GLEW 全局状态互相干扰。
  milkdrop_renderer_suspended_ = true;

  if (openGLContext.isAttached()) {
    openGLContext.executeOnGLThread([this](juce::OpenGLContext&) {
      openGLContextClosing();
    }, true);
  }

  // Windows/GLEW：Editor handle 已销毁（若存在）。无论 Editor GL 是否 attached
  // 都必须彻底重载 projectM/GLEW DLL，让进程全局的 GL 函数指针表归零。否则
  // GLView 本地上下文再 create 时，projectM 内部仍残留 Editor HGLRC 的旧指针：
  //   · framebuffer 0 直接渲染碰巧兼容，所以"默认参数"正常；
  //   · 渲染到自定义 FBO（scale_fbo_）时 glBindFramebuffer 指向旧上下文，
  //     静默失败 → scale_fbo_ 全黑 → 后处理对黑纹理做偏移/反相，表现为纯色/纯黑/纯白。
  // 这正是"脱离模式下修改任意参数后画面异常"的根因。
#if defined(_WIN32)
  projectm_api::Api::instance().reload();
#endif
}

void Y2KmeterAudioProcessorEditor::ResumeMilkdropEditorRendererAfterFloating() {
  // 解除挂起：dock 回嵌入态后允许恢复 Editor renderer（可能重新创建 handle）。
  milkdrop_renderer_suspended_ = false;

  if (editorBeingDestructed || !openGLContext.isAttached()) return;

  openGLContext.executeOnGLThread([this](juce::OpenGLContext&) {
    if (!milkdrop_render_ready_ && milkdrop_pm_handle_ == nullptr)
      newOpenGLContextCreated();
  }, false);
}

void Y2KmeterAudioProcessorEditor::RescanMilkdropPresetPaths() {
  milkdrop_preset_paths_.clear();
  auto presetsDir = milkdrop_use_like_library_.load()
                        ? FindMilkdropLikeDir()
                        : FindMilkdropAssetsDir("milkdrop_presets");
  if (presetsDir.exists()) {
    auto files = presetsDir.findChildFiles(juce::File::findFiles,
                                           false, "*.milk");
    for (auto& f : files)
      milkdrop_preset_paths_.add(f.getFullPathName());
    milkdrop_preset_paths_.sort(false);
  }
}

void Y2KmeterAudioProcessorEditor::SetMilkdropUseLikeLibrary(bool use_like) {
  milkdrop_use_like_library_.store(use_like);
  processor.setSavedMilkdropUseLikeLibrary(use_like);
  // 注：use_like_library 不再是宿主自动化参数，无需 push。
}

void Y2KmeterAudioProcessorEditor::ToggleMilkdropLibraryState() {
  // UI 线程：翻转收藏库标志并持久化。不触发 Editor GL 线程消费。
  SetMilkdropUseLikeLibrary(!milkdrop_use_like_library_.load());
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropToggleLibrary() {
  // UI 线程：翻转收藏库状态，然后入队异步切换请求，由 renderOpenGL 在 GL 线程
  // 消费（重扫 + 恢复到切换后库之前记录的预设）。
  ToggleMilkdropLibraryState();
  milkdrop_requested_library_toggle_.store(true);
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropUnlinkReload() {
  // UI 线程：取消收藏后入队重扫请求，由 renderOpenGL 在 GL 线程消费。
  milkdrop_requested_rescan_.store(true);
}

bool Y2KmeterAudioProcessorEditor::HasMilkdropLikedPresets() const {
  juce::File likeDir = FindMilkdropLikeDir();
  if (!likeDir.isDirectory())
    return false;
  return likeDir.findChildFiles(juce::File::findFiles, false, "*.milk").size() > 0;
}

juce::String Y2KmeterAudioProcessorEditor::GetMilkdropCurrentPresetFilePath() const {
  const int idx = milkdrop_current_preset_ui_.load();
  if (idx >= 0 && idx < milkdrop_preset_paths_.size())
    return milkdrop_preset_paths_[idx];
  return {};
}

void Y2KmeterAudioProcessorEditor::LoadMilkdropPresetInternal() {
  if (milkdrop_pm_handle_ == nullptr) return;
  if (milkdrop_current_preset_ < 0
      || milkdrop_current_preset_ >= milkdrop_preset_paths_.size()) return;

  auto& api = projectm_api::Api::instance();
  auto path = milkdrop_preset_paths_[milkdrop_current_preset_];
  if (api.hasLoadPresetData()) {
    juce::File f(path);
    if (f.existsAsFile()) {
      auto data = f.loadFileAsString().toStdString();
      // 注入 wave 样式覆盖（wave_mode/x/y/r/g/b/a/...），启用时才生效。
      data = ApplyWaveParamsToPresetText(data, GetMilkdropWaveState());
      api.loadPresetData(milkdrop_pm_handle_, data, true);
    }
  } else {
    api.loadPresetFile(milkdrop_pm_handle_, path.toRawUTF8(), true);
  }
}

// ---- Bridge APIs（供 MilkdropModule 调用）----

void Y2KmeterAudioProcessorEditor::PushMilkdropPcm(
    const float* interleaved_lr, unsigned int frame_count) {
  std::lock_guard<std::mutex> lock(milkdrop_pcm_mutex_);
  milkdrop_pending_pcm_.assign(interleaved_lr,
                               interleaved_lr + frame_count * 2);
  milkdrop_pending_frames_ = frame_count;
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropPresetDelta(int delta) {
  milkdrop_requested_preset_delta_.fetch_add(delta);
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropPresetJump(int index) {
  milkdrop_requested_preset_jump_.store(index);
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropPresetRandom() {
  milkdrop_requested_preset_random_.store(true);
}

juce::String Y2KmeterAudioProcessorEditor::GetMilkdropError() const {
  return milkdrop_error_;
}

HostPlayheadSnapshot Y2KmeterAudioProcessorEditor::getHostPlayheadSnapshot()
    const noexcept {
  return processor.getHostPlayheadSnapshot();
}

int Y2KmeterAudioProcessorEditor::GetMilkdropCurrentPresetIndex()
    const noexcept {
  return milkdrop_current_preset_ui_.load();
}

int Y2KmeterAudioProcessorEditor::GetMilkdropTotalPresets() const noexcept {
  return milkdrop_preset_paths_.size();
}

juce::String Y2KmeterAudioProcessorEditor::GetMilkdropCurrentPresetName()
    const {
  int idx = milkdrop_current_preset_ui_.load();
  if (idx >= 0 && idx < milkdrop_preset_paths_.size()) {
    return milkdrop_preset_paths_[idx]
        .fromLastOccurrenceOf("/", false, false)
        .fromLastOccurrenceOf("\\", false, false)
        .upToLastOccurrenceOf(".milk", false, false);
  }
  return {};
}

int64_t Y2KmeterAudioProcessorEditor::GetMilkdropLastPresetSwitchTimeMs()
    const noexcept {
  return milkdrop_last_preset_switch_ms_.load();
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropRenderScale() {
  // 循环 1 → 2 → 4 → 1
  int s = milkdrop_render_scale_;
  s = (s == 1) ? 2 : (s == 2) ? 4 : 1;
  milkdrop_render_scale_ = s;
}

void Y2KmeterAudioProcessorEditor::SetMilkdropVisualState(const MilkdropVisualState& state) {
  {
    std::lock_guard<std::mutex> lock(milkdrop_visual_mutex_);
    milkdrop_visual_state_ = state;
  }
  // 同步写回 Processor，作为 host state 顶层属性持久化（关闭→重开复原）。
  processor.setSavedMilkdropVisualState(state);
  // UI 改了视觉状态 → 回写宿主参数，保持参数与渲染一致（Standalone 下空操作）。
  pushMilkdropAutomationParams();
}

MilkdropVisualState Y2KmeterAudioProcessorEditor::GetMilkdropVisualState() const {
  std::lock_guard<std::mutex> lock(milkdrop_visual_mutex_);
  return milkdrop_visual_state_;
}

void Y2KmeterAudioProcessorEditor::SetMilkdropWaveState(const MilkdropWaveState& state) {
  {
    std::lock_guard<std::mutex> lock(milkdrop_wave_mutex_);
    milkdrop_wave_state_ = state;
  }
  processor.setSavedMilkdropWaveState(state);
  // UI 改了波形状态 → 回写宿主参数（Standalone 下空操作）。
  pushMilkdropAutomationParams();
}

MilkdropWaveState Y2KmeterAudioProcessorEditor::GetMilkdropWaveState() const {
  std::lock_guard<std::mutex> lock(milkdrop_wave_mutex_);
  return milkdrop_wave_state_;
}

// ==========================================================
// 宿主自动化参数 ↔ Milkdrop 渲染状态 双向同步（插件宿主形态）
//
//   · pullMilkdropAutomationParams：宿主（CC/automation）→ 渲染。
//     在 timerCallback 里 10Hz 轮询；参数值 ≠ 渲染状态 → 应用参数。
//   · pushMilkdropAutomationParams：渲染 → 宿主。
//     在 SetMilkdropVisualState / SetMilkdropWaveState /
//     SetMilkdropUseLikeLibrary 里调用；渲染状态 ≠ 参数 → 回写参数。
//
//   方向判断：pull 只在"参数 != 状态"时把参数写进状态；push 只在
//   "状态 != 参数"时把状态写进参数。二者互为镜像且幂等，不会形成循环：
//   · UI 改状态 → push 让参数=状态 → 下次 pull 无差异；
//   · 宿主改参数 → pull 让状态=参数 → 途中触发的 push 无差异。
//
//   · auto_mode / auto_interval：双向（宿主↔UI）。UI 修改后 MilkdropModule
//     会调 push 回写宿主参数，pull 只在参数 ≠ 状态时把参数写进模块状态。
//   · preset_next/prev/random：开关型，只做 off→on 上升沿触发一次动作。
//
//   仅插件宿主形态（isPluginHost）生效：Standalone 无宿主参数，各函数在
//   开头直接返回，避免无意义的参数树读写。
// ==========================================================

bool Y2KmeterAudioProcessorEditor::applyVisualAndWaveFromHostParams()
{
    if (! isPluginHost)
        return false;   // Standalone 无宿主参数，无需同步。

    // 只把宿主的 color / effects / tweak / wave 参数应用到渲染状态。
    //   · 不刷新 UI 控件（refreshStateFromEditor 由 10Hz pull 兜底）；
    //   · 不处理 auto / preset（离散操作，10Hz pull 足够）；
    //   · 调用方须已置 pullingParams_ = true（listener 回调 / pull 内部都会设置），
    //     以抑制 SetMilkdrop*State 内部的 push 反向覆盖宿主参数。
    auto& apvts = processor.getApvts();
    bool anyChanged = false;

    // ---- 1. Visual state（color + effects + tweak offset）----
    {
        auto vs = GetMilkdropVisualState();
        bool changed = false;

        auto f = [&](float& field, float v) {
            if (std::fabs (field - v) > 1e-6f) { field = v; changed = true; }
        };

        f (vs.tint_r,     apvts.getRawParameterValue (MilkdropParams::kTintR)->load());
        f (vs.tint_g,     apvts.getRawParameterValue (MilkdropParams::kTintG)->load());
        f (vs.tint_b,     apvts.getRawParameterValue (MilkdropParams::kTintB)->load());
        f (vs.brightness, apvts.getRawParameterValue (MilkdropParams::kBrightness)->load());

        for (const auto& def : GetMilkdropEffectDefs())
        {
            if (! def.implemented) continue;
            auto* p = apvts.getRawParameterValue (MilkdropParams::fxId (def));
            if (p == nullptr) continue;
            const bool v = p->load() >= 0.5f;
            if (def.get (vs) != v) { def.set (vs, v); changed = true; }
        }

        static const char* kOffIds[MilkdropVisualOffsetState::kParamCount] = {
            MilkdropParams::kTweakZoom, MilkdropParams::kTweakRot,  MilkdropParams::kTweakWarp,
            MilkdropParams::kTweakDx,   MilkdropParams::kTweakDy,   MilkdropParams::kTweakSx,
            MilkdropParams::kTweakSy,
        };
        for (int i = 0; i < MilkdropVisualOffsetState::kParamCount; ++i)
            f (vs.offset.value[i], apvts.getRawParameterValue (kOffIds[i])->load());

        static const char* kIntIds[MilkdropVisualOffsetState::kIntParamCount] = {
            MilkdropParams::kTweakKaleido, MilkdropParams::kTweakFoldX, MilkdropParams::kTweakFoldY,
        };
        for (int i = 0; i < MilkdropVisualOffsetState::kIntParamCount; ++i)
        {
            const int v = juce::roundToInt (apvts.getRawParameterValue (kIntIds[i])->load());
            if (v != vs.offset.ivalue[i]) { vs.offset.ivalue[i] = v; changed = true; }
        }

        if (changed)
        {
            SetMilkdropVisualState (vs);
            anyChanged = true;
        }
    }

    // ---- 2. Wave state ----
    {
        auto ws = GetMilkdropWaveState();
        bool changed = false;

        auto f = [&](float& field, float v) {
            if (std::fabs (field - v) > 1e-6f) { field = v; changed = true; }
        };
        auto b = [&](bool& field, float v) {
            const bool nv = v >= 0.5f; if (field != nv) { field = nv; changed = true; }
        };
        auto i = [&](int& field, float v) {
            const int nv = juce::roundToInt (v); if (field != nv) { field = nv; changed = true; }
        };

        b (ws.enabled,  apvts.getRawParameterValue (MilkdropParams::kWaveEnabled)->load());
        i (ws.mode,     apvts.getRawParameterValue (MilkdropParams::kWaveMode)->load());
        f (ws.x,        apvts.getRawParameterValue (MilkdropParams::kWaveX)->load());
        f (ws.y,        apvts.getRawParameterValue (MilkdropParams::kWaveY)->load());
        f (ws.r,        apvts.getRawParameterValue (MilkdropParams::kWaveR)->load());
        f (ws.g,        apvts.getRawParameterValue (MilkdropParams::kWaveG)->load());
        f (ws.b,        apvts.getRawParameterValue (MilkdropParams::kWaveB)->load());
        f (ws.a,        apvts.getRawParameterValue (MilkdropParams::kWaveA)->load());
        f (ws.mystery,  apvts.getRawParameterValue (MilkdropParams::kWaveMystery)->load());
        b (ws.usedots,  apvts.getRawParameterValue (MilkdropParams::kWaveDots)->load());
        b (ws.thick,    apvts.getRawParameterValue (MilkdropParams::kWaveThick)->load());
        b (ws.additive, apvts.getRawParameterValue (MilkdropParams::kWaveAdditive)->load());
        b (ws.brighten, apvts.getRawParameterValue (MilkdropParams::kWaveBrighten)->load());

        if (changed)
        {
            SetMilkdropWaveState (ws);
            anyChanged = true;
        }
    }

    return anyChanged;
}

void Y2KmeterAudioProcessorEditor::pullMilkdropAutomationParams()
{
    if (! isPluginHost)
        return;   // Standalone 无宿主参数，无需同步。

    pullingParams_ = true;   // 抑制 pull 过程中 SetMilkdrop*State 内部触发的 push
    auto& apvts = processor.getApvts();

    // 从 workspace 中找到唯一的 MilkdropModule。
    MilkdropModule* md = nullptr;
    if (workspace != nullptr)
        for (int i = 0; i < workspace->getNumModules(); ++i)
            if (auto* m = dynamic_cast<MilkdropModule*> (workspace->getModule (i)))
            { md = m; break; }

    // ---- 1+2. Visual + Wave 状态（color/effects/tweak/wave）----
    //   复用 applyVisualAndWaveFromHostParams（listener 高频也用它），
    //   pull 此处仅作为 10Hz 兜底 + 借机刷新二级面板控件。
    if (applyVisualAndWaveFromHostParams() && md != nullptr)
        md->refreshStateFromEditor();   // 刷新二级面板控件

    // ---- 3. Auto（模块级，双向：UI 改后 push 回写参数）----
    if (md != nullptr)
    {
        const bool autoParam = apvts.getRawParameterValue (MilkdropParams::kAutoMode)->load() >= 0.5f;
        if (autoParam != md->isAutoModeActive())
            md->setAutoModeEnabled (autoParam);

        const float interval = apvts.getRawParameterValue (MilkdropParams::kAutoInterval)->load();
        if (std::fabs (interval - md->getAutoIntervalSeconds()) > 1e-5f)
            md->applyAutoInterval (interval);

        const bool beatMode = apvts.getRawParameterValue (MilkdropParams::kAutoBeatMode)->load() >= 0.5f;
        if (beatMode != md->isAutoBeatMode())
            md->setAutoBeatMode (beatMode);

        const int beats = juce::roundToInt (apvts.getRawParameterValue (MilkdropParams::kAutoBeats)->load());
        if (beats != md->getAutoBeats())
            md->setAutoBeats (beats);
    }

    // ---- 4. 预设切换开关（宿主 off→on 触发一次，on→off 不触发）----
    {
        const bool next = apvts.getRawParameterValue (MilkdropParams::kPresetNext)->load() >= 0.5f;
        if (next && ! lastPresetNext_)  RequestMilkdropPresetDelta (+1);
        lastPresetNext_ = next;

        const bool prev = apvts.getRawParameterValue (MilkdropParams::kPresetPrev)->load() >= 0.5f;
        if (prev && ! lastPresetPrev_)  RequestMilkdropPresetDelta (-1);
        lastPresetPrev_ = prev;

        const bool rnd = apvts.getRawParameterValue (MilkdropParams::kPresetRandom)->load() >= 0.5f;
        if (rnd && ! lastPresetRandom_) RequestMilkdropPresetRandom();
        lastPresetRandom_ = rnd;
    }

    // 注：md_preset_index 不在此 pull（不做宿主 automation 绝对跳转），
    //   仅用于持久化：渲染索引变化时由 timerCallback 检测并 push 回写，
    //   初始化恢复由 Editor 构造末尾直接 RequestMilkdropPresetJump 完成。
    //   若在此 pull 会与"UI 切换预设后 push 回写"产生双向竞争，导致
    //   预设被跳回旧索引。

    pullingParams_ = false;
}

void Y2KmeterAudioProcessorEditor::pushMilkdropAutomationParams()
{
    if (! isPluginHost)
        return;   // Standalone 无宿主参数，无需同步。
    if (pullingParams_) return;   // pull 期间跳过，避免用未更新状态回滚宿主参数
    auto& apvts = processor.getApvts();

    auto setF = [&](juce::StringRef id, float value) {
        if (auto* p = apvts.getParameter (id))
        {
            const float normalized = p->convertTo0to1 (value);
            if (std::fabs (p->getValue() - normalized) > 1e-6f)
                p->setValueNotifyingHost (normalized);
        }
    };
    auto setB = [&](juce::StringRef id, bool v) {
        if (auto* p = apvts.getParameter (id))
        {
            const float target = v ? 1.0f : 0.0f;
            if (std::fabs (p->getValue() - target) > 1e-6f)
                p->setValueNotifyingHost (target);
        }
    };
    auto setI = [&](juce::StringRef id, int v) {
        if (auto* p = apvts.getParameter (id))
        {
            const float normalized = p->convertTo0to1 ((float) v);
            if (std::fabs (p->getValue() - normalized) > 1e-6f)
                p->setValueNotifyingHost (normalized);
        }
    };

    // ---- Visual state ----
    {
        auto vs = GetMilkdropVisualState();
        setF (MilkdropParams::kTintR,      vs.tint_r);
        setF (MilkdropParams::kTintG,      vs.tint_g);
        setF (MilkdropParams::kTintB,      vs.tint_b);
        setF (MilkdropParams::kBrightness, vs.brightness);

        for (const auto& def : GetMilkdropEffectDefs())
            if (def.implemented)
                setB (MilkdropParams::fxId (def), def.get (vs));

        static const char* kOffIds[MilkdropVisualOffsetState::kParamCount] = {
            MilkdropParams::kTweakZoom, MilkdropParams::kTweakRot,  MilkdropParams::kTweakWarp,
            MilkdropParams::kTweakDx,   MilkdropParams::kTweakDy,   MilkdropParams::kTweakSx,
            MilkdropParams::kTweakSy,
        };
        for (int i = 0; i < MilkdropVisualOffsetState::kParamCount; ++i)
            setF (kOffIds[i], vs.offset.value[i]);

        static const char* kIntIds[MilkdropVisualOffsetState::kIntParamCount] = {
            MilkdropParams::kTweakKaleido, MilkdropParams::kTweakFoldX, MilkdropParams::kTweakFoldY,
        };
        for (int i = 0; i < MilkdropVisualOffsetState::kIntParamCount; ++i)
            setI (kIntIds[i], vs.offset.ivalue[i]);
    }

    // ---- Wave state ----
    {
        auto ws = GetMilkdropWaveState();
        setB (MilkdropParams::kWaveEnabled,  ws.enabled);
        setI (MilkdropParams::kWaveMode,     ws.mode);
        setF (MilkdropParams::kWaveX,        ws.x);
        setF (MilkdropParams::kWaveY,        ws.y);
        setF (MilkdropParams::kWaveR,        ws.r);
        setF (MilkdropParams::kWaveG,        ws.g);
        setF (MilkdropParams::kWaveB,        ws.b);
        setF (MilkdropParams::kWaveA,        ws.a);
        setF (MilkdropParams::kWaveMystery,  ws.mystery);
        setB (MilkdropParams::kWaveDots,     ws.usedots);
        setB (MilkdropParams::kWaveThick,    ws.thick);
        setB (MilkdropParams::kWaveAdditive, ws.additive);
        setB (MilkdropParams::kWaveBrighten, ws.brighten);
    }

    // ---- Auto（模块级）----
    {
        MilkdropModule* md = nullptr;
        if (workspace != nullptr)
            for (int i = 0; i < workspace->getNumModules(); ++i)
                if (auto* m = dynamic_cast<MilkdropModule*> (workspace->getModule (i)))
                { md = m; break; }
        if (md != nullptr)
        {
            setB (MilkdropParams::kAutoMode,     md->isAutoModeActive());
            setF (MilkdropParams::kAutoInterval, md->getAutoIntervalSeconds());
            setB (MilkdropParams::kAutoBeatMode, md->isAutoBeatMode());
            setI (MilkdropParams::kAutoBeats,    md->getAutoBeats());
        }
    }

    // ---- 预设索引（持久化；-1 = 尚未加载，跳过以免覆盖宿主恢复值）----
    {
        const int idx = GetMilkdropCurrentPresetIndex();
        if (idx >= 0)
            setI (MilkdropParams::kPresetIndex, idx);
    }
}

