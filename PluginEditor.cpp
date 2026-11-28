#include "PluginEditor.h"
#include <JuceHeader.h>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <unordered_map>
#include "BinaryData.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/HoverHelp.h"
#include "source/i18n/Localisation.h"
#include "source/ui/palette/ThemeLibrary.h"
#include "source/ui/modules/LoudnessModule.h"
#include "source/ui/modules/OscilloscopeModule.h"
#include "source/ui/modules/SpectrumModule.h"
#include "source/ui/modules/PhaseModule.h"
#include "source/ui/modules/DynamicsModule.h"
#include "source/ui/modules/FineSplitModules.h"
#include "source/ui/modules/WaveformModule.h"
#include "source/ui/modules/SpectrogramModule.h"
#include "source/ui/modules/Spectrogram3DModule.h"
#include "source/ui/modules/StereoFieldModule.h"
#include "source/ui/modules/StereoMapModule.h"
#include "source/ui/modules/TempoKeyModule.h"
#include "source/ui/modules/VirtuPetModule.h"
#include "source/ui/modules/RecorderModule.h"
#include "source/ui/modules/SystemMonitorModule.h"
#include "source/ui/modules/UtilityModule.h"
#include "source/ui/modules/MilkdropModule.h"
#include "source/ui/modules/ProjectMApi.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/MilkdropParamIds.h"
#include "source/ui/MilkdropPresetIndex.h"
#include "source/ui/GlDirectModule.h"
#include "source/ui/GlDirectRenderer.h"
#include "source/ui/toys/ToyKit.h"
#include "source/ui/toys/GlToyRenderer.h"
#include "source/perf/FramePerfLog.h"
#include "source/diag/Y2KDiagLog.h"
#include "source/steam/Y2KAchievements.h"
#include "source/Y2KLogging.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX   // 阻止 windows.h 定义 min/max 宏（污染 std::min/std::max）
#endif
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>

namespace {
    // Windows AppBar（SHAppBarMessage）封装：把顶层窗口注册为 AppBar，
    // 停靠到屏幕顶/底边并让系统缩小工作区（其他窗口自动让开这条边）。
    // 任务栏式驻留（stick）就是靠这套机制实现的。
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

    // Windows DWM 硬件透明覆层开关（供独立 GL 浮窗同步真透明模式，与 Y2KMainWindow 完全一致）：
    //   配合 WGL PFD_SUPPORT_COMPOSITION + 8bit alpha FBO 0，60-120 FPS 真透明零卡顿。
    void y2kApplyWindowsDwmTransparency (HWND topHwnd, bool enable) noexcept
    {
        if (topHwnd == nullptr)
            return;

        using FnBlurBehind = HRESULT (WINAPI*) (HWND, const DWM_BLURBEHIND*);
        using FnSetAttr    = HRESULT (WINAPI*) (HWND, DWORD, LPCVOID, DWORD);

        struct DwmFns
        {
            FnBlurBehind blurBehind = nullptr;
            FnSetAttr    setAttr    = nullptr;
        };

        static const DwmFns fns = []() -> DwmFns
        {
            DwmFns out;
            HMODULE mod = GetModuleHandleW (L"dwmapi.dll");
            if (mod == nullptr)
                mod = LoadLibraryW (L"dwmapi.dll");
            if (mod != nullptr)
            {
                out.blurBehind = reinterpret_cast<FnBlurBehind> (GetProcAddress (mod, "DwmEnableBlurBehindWindow"));
                out.setAttr    = reinterpret_cast<FnSetAttr> (GetProcAddress (mod, "DwmSetWindowAttribute"));
            }
            return out;
        }();

        if (fns.blurBehind != nullptr)
        {
            HRGN emptyRgn = enable ? CreateRectRgn (0, 0, -1, -1) : nullptr;
            DWM_BLURBEHIND bb {};
            bb.dwFlags  = DWM_BB_ENABLE | DWM_BB_BLURREGION;
            bb.fEnable  = enable ? TRUE : FALSE;
            bb.hRgnBlur = emptyRgn;
            fns.blurBehind (topHwnd, &bb);
            if (emptyRgn != nullptr)
                DeleteObject (emptyRgn);
        }

        if (fns.setAttr != nullptr)
        {
            constexpr DWORD kDwmwaBorderColor  = 34;          // DWMWA_BORDER_COLOR (Win11+)
            constexpr DWORD kDwmwaColorNone    = 0xFFFFFFFEu; // DWMWA_COLOR_NONE
            constexpr DWORD kDwmwaColorDefault = 0xFFFFFFFFu; // DWMWA_COLOR_DEFAULT
            const DWORD borderCol = enable ? kDwmwaColorNone : kDwmwaColorDefault;
            fns.setAttr (topHwnd, kDwmwaBorderColor, &borderCol, sizeof (borderCol));
        }
    }

    // Windows DWMWA_CLOAK：让窗口在逻辑/消息/GL 层面均已可见，但在首帧 OpenGL SwapBuffers
    //   且嵌入式子 GL HWND 显示前由 DWM 隐藏合成，根治浮窗创建首帧闪黑 / 闪底色 / 闪旧软件帧。
    bool y2kSetWindowsDwmCloak (HWND topHwnd, bool cloak) noexcept
    {
        if (topHwnd == nullptr)
            return false;

        using FnSetAttr = HRESULT (WINAPI*) (HWND, DWORD, LPCVOID, DWORD);
        static const FnSetAttr setAttr = []() -> FnSetAttr
        {
            HMODULE mod = GetModuleHandleW (L"dwmapi.dll");
            if (mod == nullptr)
                mod = LoadLibraryW (L"dwmapi.dll");
            return mod != nullptr
                       ? reinterpret_cast<FnSetAttr> (GetProcAddress (mod, "DwmSetWindowAttribute"))
                       : nullptr;
        }();

        if (setAttr == nullptr)
            return false;

        constexpr DWORD kDwmwaCloak = 13; // DWMWA_CLOAK (Win8+)
        const BOOL val = cloak ? TRUE : FALSE;
        return SUCCEEDED (setAttr (topHwnd, kDwmwaCloak, &val, sizeof (val)));
    }

    bool y2kHasVisibleChildHwnd (HWND topHwnd) noexcept
    {
        if (topHwnd == nullptr)
            return false;

        BOOL found = FALSE;
        EnumChildWindows (topHwnd, [] (HWND child, LPARAM lp) -> BOOL
        {
            if (IsWindowVisible (child) != FALSE)
            {
                *reinterpret_cast<BOOL*> (lp) = TRUE;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM> (&found));
        return found != FALSE;
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

    // ==========================================================
    // 多屏几何 —— 一律用「物理像素」的地面真值（Win32）
    //
    // 为什么不能用 JUCE 的 Displays 逻辑表：
    //   混合 DPI 桌面（上屏 150% / 下屏 100% 之类）里，物理矩形之间是有"空洞"的：
    //   DIP 布局相邻的两块屏，物理坐标可能差出上千像素（详见桌面显示设置）。
    //   juce_Displays.cpp 的 updateToLogical() 是靠「物理矩形是否相接」连图的，
    //   且 on-bottom 分支用 `physicalX / 父屏 scale` 求子屏逻辑 x —— 于是
    //   "上下排布 + 缩放不同"时下屏的逻辑矩形会被横向算偏（甚至与主屏重叠），
    //   同一物理点在中缝两侧换算出的逻辑坐标不连续。
    //   后果：
    //     · ComponentDragger 用逻辑鼠标坐标算目标点 → 越过中缝窗口整体瞬移；
    //     · getDisplayForRect 用逻辑交集面积判屏 → 吸附基准线在两屏间翻转。
    //   MonitorFromPoint / GetMonitorInfo 的 rcMonitor / rcWork 是物理像素，
    //   与 DPI 无关，是本场景下唯一稳定的基准。
    // ==========================================================

    struct Y2KMonitorPx
    {
        juce::Rectangle<int> total;   // rcMonitor（物理 px，含任务栏区域）
        juce::Rectangle<int> work;    // rcWork（物理 px，扣任务栏 / AppBar）
        double scale = 1.0;           // 该屏的 DPI 缩放（1.0 / 1.5 ...）
    };

    bool y2kGetCursorPosPx (juce::Point<int>& out) noexcept
    {
        POINT p = {};
        if (GetCursorPos (&p) == FALSE)
            return false;

        out = { (int) p.x, (int) p.y };
        return true;
    }

    // 逻辑表里 topLeftPhysical 始终是正确的物理原点（updateToLogical 覆写逻辑
    // 矩形之前先记录了它），即使逻辑区被算错也能用它反查该屏的 scale。
    const juce::Displays::Display* y2kFindDisplayByPhysicalTopLeft (juce::Point<int> topLeft) noexcept
    {
        const auto& all = juce::Desktop::getInstance().getDisplays().displays;

        for (const auto& d : all)
            if (d.topLeftPhysical == topLeft)
                return &d;

        return nullptr;
    }

    bool y2kFillMonitorPx (HMONITOR mon, Y2KMonitorPx& out) noexcept
    {
        if (mon == nullptr)
            return false;

        MONITORINFO mi = {};
        mi.cbSize = sizeof (mi);
        if (GetMonitorInfo (mon, &mi) == FALSE)
            return false;

        out.total = { (int) mi.rcMonitor.left, (int) mi.rcMonitor.top,
                      (int) (mi.rcMonitor.right  - mi.rcMonitor.left),
                      (int) (mi.rcMonitor.bottom - mi.rcMonitor.top) };
        out.work  = { (int) mi.rcWork.left, (int) mi.rcWork.top,
                      (int) (mi.rcWork.right  - mi.rcWork.left),
                      (int) (mi.rcWork.bottom - mi.rcWork.top) };

        if (const auto* d = y2kFindDisplayByPhysicalTopLeft (out.total.getTopLeft()))
            if (d->scale > 0.1)
                out.scale = d->scale;

        return true;
    }

    bool y2kGetMonitorPxForPoint (juce::Point<int> pt, Y2KMonitorPx& out) noexcept
    {
        POINT p { pt.x, pt.y };
        return y2kFillMonitorPx (MonitorFromPoint (p, MONITOR_DEFAULTTONEAREST), out);
    }

    // 窗口当前所在显示器（MONITOR_DEFAULTTONEAREST：窗口跨屏时取交集更大的那块）。
    //   "把窗口摆到某块屏的整块区域"一律以它为准 —— 与 Windows 自身对顶层窗口的
    //   显示器归属判定一致，也和 AppBar（SHAppBarMessage）用的是同一块屏。
    bool y2kGetMonitorPxForWindow (juce::Component* window, Y2KMonitorPx& out) noexcept
    {
        const HWND hwnd = y2kGetWindowHwnd (window);
        if (hwnd == nullptr)
            return false;

        return y2kFillMonitorPx (MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST), out);
    }

    // 调试用屏幕尺寸模拟（Y2KM_SIMULATE_SCREEN）开着时，跳过所有物理像素定位，
    //   保证该调试路径的既有行为不变。
    bool y2kScreenSimulationActive() noexcept
    {
        static const bool on = juce::SystemStats::getEnvironmentVariable ("Y2KM_SIMULATE_SCREEN", {}).isNotEmpty();
        return on;
    }

    // 物理 px → 逻辑 px（该屏 scale）。JUCE 的窗口边界换算就是"逻辑 × 屏 scale"，
    //   所以反向换算不需要（也不能）经过已算错的 Displays 逻辑表。
    inline int y2kPxToLogical (int px, double scale) noexcept
    {
        return juce::roundToInt ((double) px / (scale > 0.1 ? scale : 1.0));
    }

    inline int y2kLogicalToPx (int logical, double scale) noexcept
    {
        return juce::roundToInt ((double) logical * (scale > 0.1 ? scale : 1.0));
    }

    // 只搬位置、不动尺寸：跨 DPI 时 Windows 会按目标屏重新给尺寸
    // （WM_DPICHANGED 的建议矩形），这里若强行写死尺寸会与之互相打架。
    bool y2kMoveWindowToPx (juce::Component* window, juce::Point<int> topLeft) noexcept
    {
        const HWND hwnd = y2kGetWindowHwnd (window);
        if (hwnd == nullptr)
            return false;

        SetWindowPos (hwnd, nullptr, topLeft.x, topLeft.y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return true;
    }

    bool y2kIsRectOnAnyMonitorPx (juce::Rectangle<int> r) noexcept
    {
        RECT rc = { r.getX(), r.getY(), r.getRight(), r.getBottom() };
        return MonitorFromRect (&rc, MONITOR_DEFAULTTONULL) != nullptr;
    }

    // ---- 诊断（默认零开销）：Y2KM_SNAP_DEBUG=1 → %TEMP%\Y2Kmeter_snap_debug.txt ----
    bool y2kSnapDebugEnabled() noexcept
    {
        static const bool on = juce::SystemStats::getEnvironmentVariable ("Y2KM_SNAP_DEBUG", {}) == "1";
        return on;
    }

    void y2kSnapDebug (const juce::String& line) noexcept
    {
        if (! y2kSnapDebugEnabled())
            return;

        static const juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                        .getChildFile ("Y2Kmeter_snap_debug.txt");
        f.appendText (line + "\r\n");
    }

    BOOL CALLBACK y2kEnumMonitorProc (HMONITOR hMon, HDC, LPRECT, LPARAM lp)
    {
        auto* out = reinterpret_cast<juce::String*> (lp);
        MONITORINFO mi = {};
        mi.cbSize = sizeof (mi);

        if (GetMonitorInfo (hMon, &mi) != FALSE)
            *out << "  mon total=" << (int) mi.rcMonitor.left << "," << (int) mi.rcMonitor.top
                 << " " << (int) (mi.rcMonitor.right - mi.rcMonitor.left)
                 << "x" << (int) (mi.rcMonitor.bottom - mi.rcMonitor.top)
                 << "  work=" << (int) mi.rcWork.left << "," << (int) mi.rcWork.top
                 << " " << (int) (mi.rcWork.right - mi.rcWork.left)
                 << "x" << (int) (mi.rcWork.bottom - mi.rcWork.top)
                 << (mi.dwFlags & 1 ? "  PRIMARY" : "") << "\n";

        return TRUE;
    }

    void y2kSnapDebugDumpDisplays (const char* reason) noexcept
    {
        if (! y2kSnapDebugEnabled())
            return;

        static bool dumped = false;
        if (dumped)
            return;

        dumped = true;

        juce::String s;
        s << "\r\n=== " << reason << " ===\r\n[JUCE Displays]\r\n";

        for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
            s << "  scale=" << d.scale
              << " dpi="   << d.dpi
              << " total=" << d.totalArea.toString()
              << " user="  << d.userArea.toString()
              << " physTL=" << d.topLeftPhysical.toString()
              << (d.isMain ? "  MAIN" : "") << "\n";

        s << "[Win32 Monitors]\r\n";
        ::EnumDisplayMonitors (nullptr, nullptr, &y2kEnumMonitorProc, (LPARAM) &s);

        y2kSnapDebug (s);
    }

    // ==========================================================
    // 松手吸附（四边软吸附 + 防止窗口丢失），全程物理像素
    //
    //   基准屏 = 光标所在显示器（Windows 自身的贴边行为也是这个规则）：
    //     · 光标是一个点，永远只属于一块屏，不存在"交集最大"的翻转；
    //     · 窗口跨中缝时，吸附目标线仍然取自用户眼睛所在的那块屏。
    //   顶部回弹只在「标题栏在所有显示器上都不可见」时触发（真正防丢窗口），
    //   不再因为窗口越过中缝就把整块窗口搬下来。
    // ==========================================================
    void y2kSnapWindowToMonitorEdgesPx (juce::Component* window, int titleBarHeightDips) noexcept
    {
        const HWND hwnd = y2kGetWindowHwnd (window);
        if (hwnd == nullptr)
            return;

        RECT wr = {};
        if (GetWindowRect (hwnd, &wr) == FALSE)
            return;

        const juce::Rectangle<int> winRect { (int) wr.left, (int) wr.top,
                                             (int) (wr.right - wr.left),
                                             (int) (wr.bottom - wr.top) };

        juce::Point<int> cursorPx;
        if (! y2kGetCursorPosPx (cursorPx))
            cursorPx = winRect.getCentre();

        Y2KMonitorPx mon;
        if (! y2kGetMonitorPxForPoint (cursorPx, mon))
            return;

        // 8 DIP → 该屏物理像素
        const int thresholdPx = juce::jmax (4, juce::roundToInt (8.0 * mon.scale));

        const int stripH = juce::jmax (1, juce::roundToInt ((double) titleBarHeightDips * mon.scale));
        const auto titleStrip = juce::Rectangle<int> (winRect.getX(), winRect.getY(),
                                                      winRect.getWidth(), stripH);

        juce::String dbg;
        if (y2kSnapDebugEnabled())
            dbg << "snap: win=" << winRect.toString()
                << " cursor=" << cursorPx.toString()
                << " work=" << mon.work.toString()
                << " scale=" << mon.scale;

        // 1) 防丢窗口：标题栏在所有显示器上都不存在时，搬回光标所在屏工作区顶部
        if (! y2kIsRectOnAnyMonitorPx (titleStrip))
        {
            y2kMoveWindowToPx (window, { winRect.getX(), mon.work.getY() });

            if (y2kSnapDebugEnabled())
                y2kSnapDebug (dbg + " -> rebound y=" + juce::String (mon.work.getY()));

            return;
        }

        int x = winRect.getX();
        int y = winRect.getY();

        // 左右：目标屏放得下才吸附（窗口比屏还宽时不把窗口推到屏外）
        if (std::abs (winRect.getX() - mon.work.getX()) <= thresholdPx)
            x = mon.work.getX();
        else if (mon.work.getWidth() >= winRect.getWidth()
                 && std::abs (winRect.getRight() - mon.work.getRight()) <= thresholdPx)
            x = mon.work.getRight() - winRect.getWidth();

        if (std::abs (winRect.getY() - mon.work.getY()) <= thresholdPx)
            y = mon.work.getY();
        else if (mon.work.getHeight() >= winRect.getHeight()
                 && std::abs (winRect.getBottom() - mon.work.getBottom()) <= thresholdPx)
            y = mon.work.getBottom() - winRect.getHeight();

        if (x != winRect.getX() || y != winRect.getY())
        {
            y2kMoveWindowToPx (window, { x, y });

            if (y2kSnapDebugEnabled())
                y2kSnapDebug (dbg + " -> snap " + juce::String (x) + "," + juce::String (y));
        }
        else if (y2kSnapDebugEnabled())
        {
            y2kSnapDebug (dbg + " -> no snap");
        }
    }
} // namespace
#endif

namespace {
    // ---- 诊断构建标识（v3.0.1）----
    //   纯编译期判断、无任何平台 API 依赖，因此必须放在上面的
    //   #if JUCE_WINDOWS 块之外：drawTitleBar / ChromeHiddenOverlay
    //   在 macOS 上同样会调用它。
    //   "能出日志"的构建 = RelWithDebInfo（Y2K_ENABLE_LOGGING）
    //   或显式打开诊断日志的构建（Y2K_DIAG_LOG_DEFAULT_ON=ON）。
    //   这类构建在顶部抬头（drawTitleBar / ChromeHiddenOverlay）的版本号右侧
    //   显示 " debug"，让用户和客服一眼确认装的是诊断包而不是正式包。
    bool y2kIsDiagnosticBuild() noexcept
    {
       #if defined (Y2K_ENABLE_LOGGING) || (defined (Y2K_DIAG_LOG_DEFAULT) && Y2K_DIAG_LOG_DEFAULT)
        return true;
       #else
        return false;
       #endif
    }
} // namespace

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

// ----------------------------------------------------------
// 是否为"横向条"预设（顶部/底部任务条 + 3 套 stick 变体）
//   · 共用同一套流程：窗口切成「屏幕宽 × 条高（250）」→ 贴顶/贴底 → 8px 网格对齐平铺。
// ----------------------------------------------------------
bool isHorizontalStripPreset (int presetId)
{
    return presetId == (int) ModuleWorkspace::LayoutPreset::horizontalFull
        || presetId == (int) ModuleWorkspace::LayoutPreset::horizontalBottom
        || presetId == (int) ModuleWorkspace::LayoutPreset::stickBarMixing
        || presetId == (int) ModuleWorkspace::LayoutPreset::stickBarVisuals
        || presetId == (int) ModuleWorkspace::LayoutPreset::stickBarDesk;
}

// 条状预设在 stick 下拉 placeholder 里显示的名字（与 ModuleWorkspace 条目文案一致）
juce::String stripPresetLabel (int presetId)
{
    if (presetId == (int) ModuleWorkspace::LayoutPreset::stickBarMixing)  return "Bar: Mixing";
    if (presetId == (int) ModuleWorkspace::LayoutPreset::stickBarVisuals) return "Bar: Visuals";
    if (presetId == (int) ModuleWorkspace::LayoutPreset::stickBarDesk)    return "Bar: Desk Companion";
    return "Bar: Classic";
}

// ----------------------------------------------------------
// v3.5 全新多场景布局预设表（全部基于现行 17 类存活模块，零退役模块）
//   · 使用归一化坐标 [0..1] 描述模块矩形，应用时通过统一 8px 网格映射函数换算：
//     相邻模块共享同一浮点边界 → 任意分辨率（1080p / 1440p / 4K / 高分屏）均无缝密排、零重叠；
//   · 可选携带最多 3 组现行模块参数种子（如 VU 双针/条模式、波形齿音/双声道、小工具时钟/便签等），
//     每个场景开箱即达最佳观感。
// ----------------------------------------------------------
struct PresetTile
{
    ModuleType  type;
    float       x0, y0, x1, y1;
    const char* p1 = nullptr; double v1 = 0.0;
    const char* p2 = nullptr; double v2 = 0.0;
    const char* p3 = nullptr; double v3 = 0.0;
};

struct PresetLayoutSpec
{
    const PresetTile* tiles   = nullptr;
    int               count   = 0;
    int               targetW = 0;   // >0 表示窗口化目标宽；0 表示全屏铺满
    int               targetH = 0;
};

// 1) Default Workspace（1120×680）：清晰双行平铺，零重叠，独立版 & DAW 通用
static const PresetTile kPresetDefaultWorkspace[] =
{
    { ModuleType::spectrum,     0.00f, 0.00f, 0.46f, 0.52f },
    { ModuleType::waveform,     0.46f, 0.00f, 0.78f, 0.52f },
    { ModuleType::vuMeter,      0.78f, 0.00f, 1.00f, 0.52f, "channels", 1.0 },
    { ModuleType::loudness,     0.00f, 0.52f, 0.25f, 1.00f },
    { ModuleType::stereoField,  0.25f, 0.52f, 0.48f, 1.00f },
    { ModuleType::oscilloscope, 0.48f, 0.52f, 0.73f, 1.00f, "displayMode", 0.0 },
    { ModuleType::spectrogram,  0.73f, 0.52f, 1.00f, 1.00f, "displayMode", 1.0 }
};

// 2) Compact Mini Bar（960×248）：精简悬浮条，不占满屏宽
static const PresetTile kPresetCompactMiniBar[] =
{
    { ModuleType::vuMeter,  0.00f, 0.00f, 0.22f, 1.00f, "channels", 1.0 },
    { ModuleType::spectrum, 0.22f, 0.00f, 0.56f, 1.00f },
    { ModuleType::loudness, 0.56f, 0.00f, 0.74f, 1.00f, "style", 1.0 },
    { ModuleType::waveform, 0.74f, 0.00f, 1.00f, 1.00f }
};

// 3) Desktop Companion（1360×720）：桌面日常伴侣（电脑状态 + 时钟 + 便签 + 音频 + MilkDrop）
static const PresetTile kPresetDesktopCompanion[] =
{
    { ModuleType::systemMonitor, 0.00f, 0.00f, 0.30f, 0.65f },
    { ModuleType::utility,       0.00f, 0.65f, 0.30f, 1.00f, "tool", 5.0 },  // Clock
    { ModuleType::spectrum,      0.30f, 0.00f, 0.74f, 0.46f },
    { ModuleType::milkdrop,      0.30f, 0.46f, 0.74f, 1.00f },
    { ModuleType::vuMeter,       0.74f, 0.00f, 1.00f, 0.34f, "channels", 1.0 },
    { ModuleType::waveform,      0.74f, 0.34f, 1.00f, 0.66f },
    { ModuleType::utility,       0.74f, 0.66f, 1.00f, 1.00f, "tool", 6.0 }   // Notes
};

// 4) Mixing Console（1440×800）：专业双行混音工作台
static const PresetTile kPresetMixingConsole[] =
{
    { ModuleType::spectrum,    0.00f, 0.00f, 0.44f, 0.52f },
    { ModuleType::spectrogram, 0.44f, 0.00f, 0.74f, 0.52f, "displayMode", 1.0 },
    { ModuleType::waveform,    0.74f, 0.00f, 1.00f, 0.52f },
    { ModuleType::loudness,    0.00f, 0.52f, 0.21f, 1.00f },
    { ModuleType::vuMeter,     0.21f, 0.52f, 0.40f, 1.00f, "channels", 1.0 },
    { ModuleType::phase,       0.40f, 0.52f, 0.60f, 1.00f },
    { ModuleType::stereoMap,   0.60f, 0.52f, 0.80f, 1.00f, "mode", 0.0 },
    { ModuleType::dynamics,    0.80f, 0.52f, 1.00f, 1.00f }
};

// 5) Mastering & Broadcast（1440×780）：母带与广播响度合规
static const PresetTile kPresetMasteringBroadcast[] =
{
    { ModuleType::loudness,  0.00f, 0.00f, 0.34f, 0.48f, "style", 1.0 },
    { ModuleType::dynamics,  0.34f, 0.00f, 0.68f, 0.48f },
    { ModuleType::vuMeter,   0.68f, 0.00f, 1.00f, 0.48f, "meterType", 1.0, "channels", 1.0 },
    { ModuleType::spectrum,  0.00f, 0.48f, 0.38f, 1.00f },
    { ModuleType::phase,     0.38f, 0.48f, 0.60f, 1.00f },
    { ModuleType::stereoMap, 0.60f, 0.48f, 0.82f, 1.00f, "mode", 1.0 },  // Mono check
    { ModuleType::waveform,  0.82f, 0.48f, 1.00f, 1.00f, "colorSource", 1.0 }
};

// 6) Vocal & Podcast（1280×680）：人声齿音检测 + 32-bit 录音 + 响度/复古表 + 秒表
static const PresetTile kPresetVocalPodcast[] =
{
    { ModuleType::waveform, 0.00f, 0.00f, 0.48f, 0.52f, "colorSource", 2.0 }, // Sibilance
    { ModuleType::spectrum, 0.48f, 0.00f, 0.82f, 0.52f },
    { ModuleType::recorder, 0.82f, 0.00f, 1.00f, 0.52f },
    { ModuleType::loudness, 0.00f, 0.52f, 0.30f, 1.00f },
    { ModuleType::vuMeter,  0.30f, 0.52f, 0.56f, 1.00f, "meterType", 0.0, "needleDialStyle", 3.0 },
    { ModuleType::tempoKey, 0.56f, 0.52f, 0.80f, 1.00f },
    { ModuleType::utility,  0.80f, 0.52f, 1.00f, 1.00f, "tool", 2.0 }         // Stopwatch
};

// 7) DJ & Beat Analysis（1280×720）：BPM/调性 + 双声道波形 + 双针 VU + 柱状频谱 + 雷达 + 3D 语谱
static const PresetTile kPresetDjBeatLive[] =
{
    { ModuleType::tempoKey,      0.00f, 0.00f, 0.28f, 0.48f },
    { ModuleType::waveform,      0.28f, 0.00f, 0.74f, 0.48f, "dual", 1.0, "colorSource", 0.0 },
    { ModuleType::vuMeter,       0.74f, 0.00f, 1.00f, 0.48f, "meterType", 0.0, "needleDialStyle", 2.0, "channels", 1.0 },
    { ModuleType::spectrum,      0.00f, 0.48f, 0.42f, 1.00f, "fillStyle", 4.0 },
    { ModuleType::stereoField,   0.42f, 0.48f, 0.70f, 1.00f },
    { ModuleType::spectrogram3d, 0.70f, 0.48f, 1.00f, 1.00f }
};

// 8) Stream / OBS Overlay（1200×280）：极简直播挂件条（适配绿幕 / 透明覆层）
static const PresetTile kPresetStreamOverlay[] =
{
    { ModuleType::vuMeter,  0.00f, 0.00f, 0.22f, 1.00f, "channels", 1.0 },
    { ModuleType::spectrum, 0.22f, 0.00f, 0.56f, 1.00f },
    { ModuleType::waveform, 0.56f, 0.00f, 0.80f, 1.00f },
    { ModuleType::tempoKey, 0.80f, 0.00f, 1.00f, 1.00f }
};

// 9) Music Visualizer (MV)（全屏）：顶部 6 模块视觉监测条 + 下方超大 MilkDrop 舞台
static const PresetTile kPresetMusicVisualizerMv[] =
{
    { ModuleType::spectrogram3d, 0.00f, 0.00f, 0.15f, 0.18f },
    { ModuleType::vuMeter,       0.15f, 0.00f, 0.28f, 0.18f, "channels", 1.0 },
    { ModuleType::spectrum,      0.28f, 0.00f, 0.52f, 0.18f },
    { ModuleType::stereoField,   0.52f, 0.00f, 0.66f, 0.18f },
    { ModuleType::waveform,      0.66f, 0.00f, 0.86f, 0.18f },
    { ModuleType::tempoKey,      0.86f, 0.00f, 1.00f, 0.18f },
    { ModuleType::milkdrop,      0.00f, 0.18f, 1.00f, 1.00f }
};

// 10) Pure MilkDrop（全屏）：单模块沉浸舞台
static const PresetTile kPresetOnlyMilkdrop[] =
{
    { ModuleType::milkdrop, 0.00f, 0.00f, 1.00f, 1.00f }
};

// 11) Full: Deep Analysis（全屏）：12 块工程级频谱/语谱/声像/相位/动态全景分析墙
static const PresetTile kPresetFullDeepAnalysis[] =
{
    // 左侧 64%：频谱 + 2D/3D 语谱 + 声像/单声道兼容双视图
    { ModuleType::spectrum,      0.00f, 0.00f, 0.64f, 0.34f },
    { ModuleType::spectrogram,   0.00f, 0.34f, 0.36f, 0.68f, "displayMode", 1.0 },
    { ModuleType::spectrogram3d, 0.36f, 0.34f, 0.64f, 0.68f },
    { ModuleType::stereoMap,     0.00f, 0.68f, 0.32f, 1.00f, "mode", 0.0 },
    { ModuleType::stereoMap,     0.32f, 0.68f, 0.64f, 1.00f, "mode", 1.0 },
    // 右侧 36%：双声道波形 + 响度/动态 + 相位/示波 X-Y + VU/调性
    { ModuleType::waveform,      0.64f, 0.00f, 1.00f, 0.26f, "dual", 1.0 },
    { ModuleType::loudness,      0.64f, 0.26f, 0.82f, 0.56f },
    { ModuleType::dynamics,      0.82f, 0.26f, 1.00f, 0.56f },
    { ModuleType::phase,         0.64f, 0.56f, 0.82f, 0.78f },
    { ModuleType::oscilloscope,  0.82f, 0.56f, 1.00f, 0.78f, "displayMode", 1.0 },
    { ModuleType::vuMeter,       0.64f, 0.78f, 0.82f, 1.00f, "channels", 1.0 },
    { ModuleType::tempoKey,      0.82f, 0.78f, 1.00f, 1.00f }
};

// 12) Full: All-in-One（全屏）：无缝密排覆盖全部 16 类功能模块（零退役、零重叠、1080p+ 友好）
static const PresetTile kPresetFullAllInOne[] =
{
    // 第 1 行：电脑状态 + 响度 + VU + 动态 + 调性 + 录音机
    { ModuleType::systemMonitor, 0.00f, 0.00f, 0.22f, 0.24f },
    { ModuleType::loudness,      0.22f, 0.00f, 0.39f, 0.24f },
    { ModuleType::vuMeter,       0.39f, 0.00f, 0.54f, 0.24f, "channels", 1.0 },
    { ModuleType::dynamics,      0.54f, 0.00f, 0.72f, 0.24f },
    { ModuleType::tempoKey,      0.72f, 0.00f, 0.87f, 0.24f },
    { ModuleType::recorder,      0.87f, 0.00f, 1.00f, 0.24f },
    // 第 2 行：宽频谱 + 波形历史 + 时钟 + 便签
    { ModuleType::spectrum,      0.00f, 0.24f, 0.42f, 0.50f },
    { ModuleType::waveform,      0.42f, 0.24f, 0.72f, 0.50f },
    { ModuleType::utility,       0.72f, 0.24f, 0.86f, 0.50f, "tool", 5.0 },
    { ModuleType::utility,       0.86f, 0.24f, 1.00f, 0.50f, "tool", 6.0 },
    // 第 3 行：2D 语谱 + 3D 语谱 + 相位 + 李萨如示波
    { ModuleType::spectrogram,   0.00f, 0.50f, 0.28f, 0.76f, "displayMode", 1.0 },
    { ModuleType::spectrogram3d, 0.28f, 0.50f, 0.54f, 0.76f },
    { ModuleType::phase,         0.54f, 0.50f, 0.74f, 0.76f },
    { ModuleType::oscilloscope,  0.74f, 0.50f, 1.00f, 0.76f, "displayMode", 2.0 },
    // 第 4 行：声场雷达 + 频段声像 + 宽幅 MilkDrop
    { ModuleType::stereoField,   0.00f, 0.76f, 0.24f, 1.00f },
    { ModuleType::stereoMap,     0.24f, 0.76f, 0.50f, 1.00f, "mode", 0.0 },
    { ModuleType::milkdrop,      0.50f, 0.76f, 1.00f, 1.00f }
};

// 横向任务条 / Stick 吸附条（4 套，全部基于现行存活模块）
static const PresetTile kStripClassic[] =
{
    { ModuleType::spectrogram3d, 0.00f, 0.00f, 0.13f, 1.00f },
    { ModuleType::loudness,      0.13f, 0.00f, 0.23f, 1.00f, "style", 1.0 },
    { ModuleType::vuMeter,       0.23f, 0.00f, 0.35f, 1.00f, "channels", 1.0 },
    { ModuleType::stereoField,   0.35f, 0.00f, 0.47f, 1.00f },
    { ModuleType::spectrum,      0.47f, 0.00f, 0.68f, 1.00f },
    { ModuleType::oscilloscope,  0.68f, 0.00f, 0.80f, 1.00f, "displayMode", 0.0 },
    { ModuleType::waveform,      0.80f, 0.00f, 1.00f, 1.00f }
};

static const PresetTile kStripMixing[] =
{
    { ModuleType::spectrogram, 0.00f, 0.00f, 0.14f, 1.00f, "displayMode", 1.0 },
    { ModuleType::loudness,    0.14f, 0.00f, 0.24f, 1.00f },
    { ModuleType::vuMeter,     0.24f, 0.00f, 0.36f, 1.00f, "channels", 1.0 },
    { ModuleType::phase,       0.36f, 0.00f, 0.48f, 1.00f },
    { ModuleType::spectrum,    0.48f, 0.00f, 0.68f, 1.00f },
    { ModuleType::stereoMap,   0.68f, 0.00f, 0.82f, 1.00f, "mode", 0.0 },
    { ModuleType::waveform,    0.82f, 0.00f, 1.00f, 1.00f }
};

static const PresetTile kStripVisuals[] =
{
    { ModuleType::spectrogram, 0.00f, 0.00f, 0.13f, 1.00f, "displayMode", 1.0 },
    { ModuleType::vuMeter,     0.13f, 0.00f, 0.24f, 1.00f, "channels", 1.0 },
    { ModuleType::milkdrop,    0.24f, 0.00f, 0.54f, 1.00f },
    { ModuleType::spectrum,    0.54f, 0.00f, 0.72f, 1.00f },
    { ModuleType::tempoKey,    0.72f, 0.00f, 0.84f, 1.00f },
    { ModuleType::waveform,    0.84f, 0.00f, 1.00f, 1.00f }
};

static const PresetTile kStripDeskCompanion[] =
{
    { ModuleType::systemMonitor, 0.00f, 0.00f, 0.20f, 1.00f },
    { ModuleType::utility,       0.20f, 0.00f, 0.32f, 1.00f, "tool", 5.0 },
    { ModuleType::vuMeter,       0.32f, 0.00f, 0.44f, 1.00f, "channels", 1.0 },
    { ModuleType::spectrum,      0.44f, 0.00f, 0.68f, 1.00f },
    { ModuleType::tempoKey,      0.68f, 0.00f, 0.81f, 1.00f },
    { ModuleType::waveform,      0.81f, 0.00f, 1.00f, 1.00f }
};

bool isFullScreenTilePreset (int presetId)
{
    return presetId == (int) ModuleWorkspace::LayoutPreset::musicVisualizerMv
        || presetId == (int) ModuleWorkspace::LayoutPreset::onlyMilkdrop
        || presetId == (int) ModuleWorkspace::LayoutPreset::fullDeepAnalysis
        || presetId == (int) ModuleWorkspace::LayoutPreset::fullAllInOne;
}

PresetLayoutSpec resolvePresetSpec (int presetId)
{
    using LP = ModuleWorkspace::LayoutPreset;
    switch ((LP) presetId)
    {
        case LP::defaultGrid:
            return { kPresetDefaultWorkspace, (int) std::size (kPresetDefaultWorkspace), 1120, 680 };
        case LP::compactMiniBar:
            return { kPresetCompactMiniBar, (int) std::size (kPresetCompactMiniBar), 960, 248 };
        case LP::desktopCompanion:
            return { kPresetDesktopCompanion, (int) std::size (kPresetDesktopCompanion), 1360, 720 };
        case LP::mixingConsole:
            return { kPresetMixingConsole, (int) std::size (kPresetMixingConsole), 1440, 800 };
        case LP::masteringBroadcast:
            return { kPresetMasteringBroadcast, (int) std::size (kPresetMasteringBroadcast), 1440, 780 };
        case LP::vocalPodcast:
            return { kPresetVocalPodcast, (int) std::size (kPresetVocalPodcast), 1280, 680 };
        case LP::djBeatLive:
            return { kPresetDjBeatLive, (int) std::size (kPresetDjBeatLive), 1280, 720 };
        case LP::streamOverlay:
            return { kPresetStreamOverlay, (int) std::size (kPresetStreamOverlay), 1200, 280 };
        case LP::musicVisualizerMv:
            return { kPresetMusicVisualizerMv, (int) std::size (kPresetMusicVisualizerMv), 0, 0 };
        case LP::onlyMilkdrop:
            return { kPresetOnlyMilkdrop, (int) std::size (kPresetOnlyMilkdrop), 0, 0 };
        case LP::fullDeepAnalysis:
            return { kPresetFullDeepAnalysis, (int) std::size (kPresetFullDeepAnalysis), 0, 0 };
        case LP::fullAllInOne:
            return { kPresetFullAllInOne, (int) std::size (kPresetFullAllInOne), 0, 0 };
        case LP::stickBarMixing:
            return { kStripMixing, (int) std::size (kStripMixing), 0, 250 };
        case LP::stickBarVisuals:
            return { kStripVisuals, (int) std::size (kStripVisuals), 0, 250 };
        case LP::stickBarDesk:
            return { kStripDeskCompanion, (int) std::size (kStripDeskCompanion), 0, 250 };
        case LP::horizontalFull:
        case LP::horizontalBottom:
        default:
            return { kStripClassic, (int) std::size (kStripClassic), 0, 250 };
    }
}

// UI 密度缩放的判据区域（跨平台差异）：
//   · Windows：必须用 totalArea —— AppBar/stick 驻留会真实压缩 userArea，
//     条高拉大时若用 userArea 会把"屏幕变矮了"的假象喂给分级表；
//   · macOS：userArea（菜单栏 / Dock 是真实可用空间，且没有 stick/AppBar），
//     但必须换算回【物理像素】—— JUCE 的 userArea 是 Retina 缩放后的逻辑
//     points，200% Retina 下逻辑高度只有同尺寸 Windows 屏（150%）的一半，
//     会被 uiScaleForDisplay 误判为低分辨率屏（UI 整体缩到 0.75，工具栏里的
//     色票/预览块随之变小）。换成物理尺寸后，同一块物理屏两端拿到同一档位。
juce::Rectangle<int> resolveUiScaleArea (const juce::Displays::Display& display) noexcept
{
   #if JUCE_WINDOWS
    return display.totalArea;
   #else
    const double s = display.scale > 0.1 ? display.scale : 1.0;
    return { 0, 0,
             juce::roundToInt ((double) display.userArea.getWidth()  * s),
             juce::roundToInt ((double) display.userArea.getHeight() * s) };
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
    // 子级 GL 宿主容器作为 DocumentWindow 内容，镜像 Y2KMainWindow → Editor 结构，
    //   避免 OpenGLContext 挂到顶层 peer 自身导致 Windows WM_PAINT 绕过 CachedImage 走软件绘制。
    setContentNonOwned (&glHost_, false);

    // Toybox：浮动态模块脱离 Editor 组件树，Editor 级全局监听器收不到它的
    //   鼠标事件，需把 director 作为 MouseListener 直接挂到面板上（§11.2）。
    if (editor_ != nullptr && editor_->toyDirector != nullptr)
        module_.addMouseListener (editor_->toyDirector.get(), true);

    // 悬停提示：挂到 glHost_ 内，与模块共享浮窗 OpenGL 合成层，不会被子 GL HWND 遮挡
    tooltip_ = std::make_unique<Y2KTooltipWindow> (&glHost_);
}

Y2KmeterAudioProcessorEditor::FloatingModuleWindow::~FloatingModuleWindow()
{
    // 最先解绑 GL 上下文：保证 GL 线程彻底退出并释放本上下文 VBO/FBO/纹理，
    //   再销毁子组件/监听器，杜绝悬垂引用或跨上下文资源泄漏。
    detachGlContext (false, true);
    cancelCloakReveal (false);

    tooltip_.reset();
    detachHostedModule();
    clearContentComponent();

    if (editor_ != nullptr && editor_->toyDirector != nullptr)
        module_.removeMouseListener (editor_->toyDirector.get());
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::GlHostComponent::resized()
{
    if (owner_.moduleAttached_)
        owner_.module_.setBounds (getLocalBounds());
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::attachHostedModule()
{
    if (moduleAttached_)
        return;

    glHost_.addAndMakeVisible (module_);
    moduleAttached_ = true;
    module_.setBounds (glHost_.getLocalBounds());
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::detachHostedModule()
{
    if (! moduleAttached_)
        return;

    moduleAttached_ = false;
    glHost_.removeChildComponent (&module_);
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::beginCloakUntilFirstGlFrame()
{
   #if JUCE_WINDOWS
    auto* hwnd = reinterpret_cast<HWND> (getWindowHandle());
    if (hwnd == nullptr)
        return;

    firstGlFrameRendered_.store (false, std::memory_order_release);
    cloakRetryCount_ = 0;
    cloakTimerTicks_ = 0;
    if (y2kSetWindowsDwmCloak (hwnd, true))
    {
        cloakedUntilFirstGl_ = true;
        cloakStartMs_ = juce::Time::getMillisecondCounter();
        cloakRevealTimer_.startTimer (8);
    }
   #endif
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::cancelCloakReveal (bool uncloakNow)
{
    cloakRevealTimer_.stopTimer();
    if (! cloakedUntilFirstGl_)
        return;

    cloakedUntilFirstGl_ = false;
    cloakRetryCount_ = 0;
    cloakTimerTicks_ = 0;
   #if JUCE_WINDOWS
    if (uncloakNow)
        if (auto* hwnd = reinterpret_cast<HWND> (getWindowHandle()))
            y2kSetWindowsDwmCloak (hwnd, false);
   #else
    juce::ignoreUnused (uncloakNow);
   #endif
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::tryRevealAfterFirstGlFrame (bool fromTimer)
{
    if (! cloakedUntilFirstGl_)
        return;

   #if JUCE_WINDOWS
    auto* hwnd = reinterpret_cast<HWND> (getWindowHandle());
    const auto nowMs = juce::Time::getMillisecondCounter();
    const bool firstGlDone = firstGlFrameRendered_.load (std::memory_order_acquire);

    // 冷启动恢复浮窗时，Editor::ctor 与 WASAPI 初始化可能先阻塞消息循环数百毫秒；
    //   首个真正进入消息循环的定时器滴答才开始计 250ms 超时预算，防止尚未跑首帧就被误超时揭幕。
    if (fromTimer)
    {
        if (cloakTimerTicks_ == 0 && ! firstGlDone)
            cloakStartMs_ = nowMs;
        ++cloakTimerTicks_;
    }

    const bool timedOut = (cloakTimerTicks_ >= 4) && (nowMs - cloakStartMs_ >= 250);
    const bool ready = firstGlDone && y2kHasVisibleChildHwnd (hwnd);
    if (ready || timedOut || hwnd == nullptr)
    {
        if (Y2K_DIAG_ENABLED())
        {
            const auto elapsed = nowMs - cloakStartMs_;
            Y2K_DIAG ("GL", "floating uncloak: " + moduleTypeToString (module_.getModuleType())
                                  + " elapsed=" + juce::String ((int) elapsed) + "ms"
                                  + " ready=" + juce::String (ready ? 1 : 0)
                                  + " timedOut=" + juce::String (timedOut ? 1 : 0)
                                  + " retries=" + juce::String (cloakRetryCount_));
        }
        cancelCloakReveal (true);
        return;
    }

    // 首帧 GL 已完成但 NativeContext::handleAsyncUpdate 仍在消息队列后方排队时，
    //   紧跟下一轮消息循环立即复检，做到子 GL HWND 显露后亚毫秒级揭幕。
    if (firstGlDone && cloakRetryCount_ < 6)
    {
        ++cloakRetryCount_;
        juce::Component::SafePointer<FloatingModuleWindow> safeWin (this);
        juce::MessageManager::callAsync ([safeWin]()
        {
            if (auto* w = safeWin.getComponent())
                w->tryRevealAfterFirstGlFrame (false);
        });
    }
   #else
    juce::ignoreUnused (fromTimer);
    cancelCloakReveal (true);
   #endif
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::paint (juce::Graphics& g)
{
    // 透明覆层 / 桌宠模式下跳过 DocumentWindow 的不透明底色填充，保持逐像素透明；
    //   其余模式下（因 OpenGLContext 挂在子组件 glHost_ 上，WM_PAINT 不会进入 module_）
    //   用卡片底色或绿幕色兜底，避免未覆盖区域透出未初始化黑底。
    if (PinkXP::isTransparentBackdrop()
        || module_.getModuleType() == ModuleType::virtuPet)
        return;

    if (glEnabledForModule_)
    {
        g.fillAll (PinkXP::isGreenScreenBackdrop() ? PinkXP::chromaKeyGreen() : PinkXP::face);
        return;
    }

    juce::DocumentWindow::paint (g);
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::newOpenGLContextCreated()
{
    glContext_.setImageCacheSize (16 * 1024 * 1024);
    if (glDirectRenderer_ == nullptr)
        glDirectRenderer_ = std::make_unique<GlDirectRenderer>();
    glDirectRenderer_->ensureInitialised (glContext_);
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::renderOpenGL()
{
    using namespace juce::gl;

    if (glClosing_.load (std::memory_order_acquire))
        return;

    glFrameCounter_.fetch_add (1, std::memory_order_relaxed);

    GLint origViewport[4];
    glGetIntegerv (GL_VIEWPORT, origViewport);

    const float dpiScale = static_cast<float> (glContext_.getRenderingScale());
    const int   logicalH = glHost_.getHeight();
    const int   vw       = juce::jmax (1, juce::roundToInt ((float) glHost_.getWidth() * dpiScale));
    const int   vh       = juce::jmax (1, juce::roundToInt ((float) logicalH * dpiScale));

    // 清空 FBO 0 后缓冲（透明覆层清为纯 alpha=0；绿幕清为纯绿；正常态清为卡片底色）
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, vw, vh);
    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (PinkXP::isTransparentBackdrop())
    {
        glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    }
    else
    {
        const auto c = PinkXP::isGreenScreenBackdrop() ? PinkXP::chromaKeyGreen() : PinkXP::face;
        glClearColor (c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), 1.0f);
    }
    glClear (GL_COLOR_BUFFER_BIT);

    if (moduleAttached_)
    {
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (&module_))
        {
            if (gmod->isVisible() && gmod->isShowing()
                && glDirectRenderer_ != nullptr
                && glDirectRenderer_->ensureInitialised (glContext_))
            {
                auto guard = glDirectRenderer_->beginFrame (vw, vh);
                glDirectRenderer_->renderModule (glHost_, *gmod, dpiScale, logicalH, vh, vw, vh);
                glDirectRenderer_->endFrame (guard);
            }
            else
            {
                if (glDirectRenderer_ != nullptr && ! glDirectRenderer_->isReady())
                {
                    gmod->setGlHostAvailable (false);
                    if (gmod->glDirectActive.exchange (false, std::memory_order_acq_rel))
                        gmod->forceRepaintImmediate();
                }
                gmod->diagSetVisible (false);
            }
        }
    }

    glViewport (origViewport[0], origViewport[1], origViewport[2], origViewport[3]);

    if (! firstGlFrameRendered_.exchange (true, std::memory_order_acq_rel))
    {
        juce::Component::SafePointer<FloatingModuleWindow> safeWin (this);
        juce::MessageManager::callAsync ([safeWin]()
        {
            if (auto* w = safeWin.getComponent())
                w->tryRevealAfterFirstGlFrame();
        });
    }
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::openGLContextClosing()
{
    if (glDirectRenderer_ != nullptr)
        glDirectRenderer_->releaseAll();

    if (moduleAttached_)
    {
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (&module_))
        {
            if (! glPreserveActiveOnClose_.load (std::memory_order_acquire))
                gmod->glDirectActive.store (false, std::memory_order_release);
            gmod->glTexUploadedGen.store (0, std::memory_order_release);
        }
    }
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::syncBackdropTransparency()
{
    const bool wantTransparent = PinkXP::isTransparentBackdrop();
   #if JUCE_WINDOWS
    setBackgroundColour (wantTransparent ? juce::Colours::transparentBlack : PinkXP::face);
    setOpaque (true); // 严禁触发 WS_EX_LAYERED；由 DWM 硬件 Alpha 合成 OpenGL FBO 0
    if (auto* hwnd = reinterpret_cast<HWND> (getWindowHandle()))
    {
        y2kApplyWindowsDwmTransparency (hwnd, wantTransparent);
        transparentDwmApplied_ = wantTransparent;
    }
   #else
    setBackgroundColour (wantTransparent ? juce::Colours::transparentBlack : PinkXP::face);
    setOpaque (! wantTransparent);
    if (glEnabledForModule_)
        setOpenGlEnabledForBackdrop (! wantTransparent);
   #endif
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::attachGlContextIfEligible()
{
    // Milkdrop 浮动态已自带独立原生 GLView + OpenGLContext，严禁重复挂载嵌套 GL 上下文
    if (module_.getModuleType() == ModuleType::milkdrop)
    {
        glEnabledForModule_ = false;
        cancelCloakReveal (true);
        return;
    }

    glEnabledForModule_ = true;
    syncBackdropTransparency();

   #if JUCE_WINDOWS
    const bool attachNow = ! GlDirectRenderer::isGloballyDisabled();
   #else
    const bool attachNow = ! PinkXP::isTransparentBackdrop() && ! GlDirectRenderer::isGloballyDisabled();
   #endif

    if (! attachNow)
    {
        cancelCloakReveal (true);
        module_.setFloatingGlHosted (false);
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (&module_))
        {
            gmod->setGlHostAvailable (false);
            if (gmod->glDirectActive.exchange (false, std::memory_order_acq_rel))
                gmod->forceRepaintImmediate();
        }
        return;
    }

    glClosing_.store (false, std::memory_order_release);
    glPreserveActiveOnClose_.store (false, std::memory_order_release);
    firstGlFrameRendered_.store (false, std::memory_order_release);

    // 先把模块切换到浮窗 GL 宿主并同步重建一帧全量快照，再 attach OpenGLContext，
    //   确保独立 GL 线程首帧一定拿到当前浮窗尺寸与完整纹理数据（零首帧空白 / 错尺）。
    if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (&module_))
    {
        gmod->setGlHostAvailable (true);
        juce::Component::SafePointer<FloatingModuleWindow> safeWin (this);
        gmod->setGlFrameRequester ([safeWin]()
        {
            if (auto* w = safeWin.getComponent())
                w->requestGlFrame();
        });
        gmod->forceRebuildGlSnapshot();
    }

    if (! glContext_.isAttached())
    {
       #if JUCE_MAC
        glContext_.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
       #endif
        glContext_.setRenderer (this);
        glContext_.setContinuousRepainting (false);
        glContext_.setComponentPaintingEnabled (true);
        // 独立 RenderThread（JUCE 补丁 0010）：避免多浮窗与主窗口在同一进程级
        //   RenderThread 上串行等待 SwapBuffers VSync，保持全窗口 VSync 且各自满帧。
        glContext_.setUseDedicatedRenderThread (true);
       #if JUCE_MAC
        glContext_.setComponentRepaintsPacedToDisplay (true);
       #else
        glContext_.setExternalFramePacing (
            editor_ != nullptr && editor_->processor.getAnalyserHub().isUsingExternalFrameClock());
       #endif
        // 挂载到子容器 glHost_（而非顶层 FloatingModuleWindow），保证 WM_PAINT
        //   经 glHost_::paintWithinParentContext 命中 CachedImage，绝不退回软件 GDI 绘模块！
        glContext_.attachTo (glHost_);
    }

    const bool attached = glContext_.isAttached();
    module_.setFloatingGlHosted (attached);

    if (! attached)
    {
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (&module_))
        {
            gmod->setGlFrameRequester (nullptr);
            gmod->setGlHostAvailable (false);
            if (gmod->glDirectActive.exchange (false, std::memory_order_acq_rel))
                gmod->forceRepaintImmediate();
        }
        cancelCloakReveal (true);
        return;
    }

    requestGlFrame();
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::detachGlContext (bool targetHostHasGl, bool hideNativeWindow)
{
    glClosing_.store (true, std::memory_order_release);
    glPreserveActiveOnClose_.store (targetHostHasGl, std::memory_order_release);
    cancelAsyncUpdates();
    cancelCloakReveal (false);

   #if JUCE_WINDOWS
    // 先在 Win32 层即时隐藏顶层窗口，避免 glContext_.detach() 销毁子 GL HWND 时
    //   把底层父 HWND 暴露闪一帧；不走 JUCE setVisible(false) 以避免提前重入 releaseResources。
    if (hideNativeWindow)
        if (auto* hwnd = reinterpret_cast<HWND> (getWindowHandle()))
            ShowWindow (hwnd, SW_HIDE);
   #else
    juce::ignoreUnused (hideNativeWindow);
   #endif

    auto* gmod = moduleAttached_ ? dynamic_cast<GlDirectModuleBase*> (&module_) : nullptr;
    if (gmod != nullptr)
        gmod->setGlFrameRequester (nullptr);

    // 必须无条件 detach：即使 nativeContext 暂时为空（isAttached()==false），attachment 可能仍已注册，
    //   跳过 detach 会导致析构期 ComponentMovementWatcher 重入 attach/detach。
    glContext_.detach();
    glContext_.setRenderer (nullptr);
    glDirectRenderer_.reset();

    if (! moduleAttached_)
        return;

    module_.setFloatingGlHosted (false);

    if (gmod != nullptr)
    {
        if (! targetHostHasGl)
            gmod->glDirectActive.store (false, std::memory_order_release);
        gmod->glTexUploadedGen.store (0, std::memory_order_release);
        gmod->glForceTrailClear.store (true, std::memory_order_release);

        if (editor_ != nullptr && ! editor_->editorBeingDestructed)
        {
            juce::Component::SafePointer<Y2KmeterAudioProcessorEditor> safeEd (editor_);
            gmod->setGlFrameRequester ([safeEd]()
            {
                if (auto* ed = safeEd.getComponent())
                    ed->RequestGlFrame();
            });
            gmod->setGlHostAvailable (editor_->openGLContext.isAttached()
                                      && ! GlDirectRenderer::isGloballyDisabled());
        }
        else
        {
            gmod->setGlFrameRequester (nullptr);
            gmod->setGlHostAvailable (false);
        }
    }
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::requestGlFrame()
{
    if (! glClosing_.load (std::memory_order_acquire) && glContext_.isAttached())
    {
        // 首帧尚未落屏（Windows DWMWA_CLOAK 遮罩等待首帧揭幕）时直接唤醒渲染，避免延迟揭幕
        if (! firstGlFrameRendered_.load (std::memory_order_acquire))
            glContext_.triggerRepaint();
        else
            glContext_.triggerRepaintPacedToDisplay();
    }
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::setExternalFramePacing (bool shouldPace)
{
   #if ! JUCE_MAC
    glContext_.setExternalFramePacing (shouldPace);
   #else
    juce::ignoreUnused (shouldPace);
   #endif
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::flushPacedRepaint()
{
    if (! glClosing_.load (std::memory_order_acquire) && glContext_.isAttached())
        glContext_.flushPacedRepaint();
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::setOpenGlEnabledForBackdrop (bool enabled)
{
    if (! glEnabledForModule_)
        return;

    if (! enabled)
    {
        glClosing_.store (true, std::memory_order_release);
        glPreserveActiveOnClose_.store (false, std::memory_order_release);
        cancelCloakReveal (true);
        glContext_.detach();
        glContext_.setRenderer (nullptr);
        glDirectRenderer_.reset();
        module_.setFloatingGlHosted (false);
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (&module_))
        {
            gmod->setGlHostAvailable (false);
            if (gmod->glDirectActive.exchange (false, std::memory_order_acq_rel))
                gmod->forceRepaintImmediate();
            gmod->glTexUploadedGen.store (0, std::memory_order_release);
        }
        repaint();
        return;
    }

    if (! PinkXP::isTransparentBackdrop() && ! glContext_.isAttached())
        attachGlContextIfEligible();
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::closeButtonPressed()
{
    cancelAsyncUpdates();  // 取消 resize 触发的 pending async update，防止引用已删除窗口
    if (onDock_)
    {
        // 先拷到本地栈再调用，防止回调释放 FloatingModuleWindow 导致 onDock_ 闭包自毁 UAF
        auto cb = onDock_;
        cb (module_, savedBounds_);
    }
}

bool Y2KmeterAudioProcessorEditor::FloatingModuleWindow::keyPressed(
    const juce::KeyPress& key) {
  // Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y 撤回与重做：转发给主工作区（TextEditor 聚焦时自动让行）
  if (editor_ != nullptr && editor_->getWorkspace() != nullptr
      && editor_->getWorkspace()->handleUndoRedoKeyPress (key))
    return true;

  // 浮动 Milkdrop 有独立的 GLView 本地渲染器（Editor renderer 已被挂起），
  // 键盘事件必须直接路由到本窗口的模块，调用其公开 API 以操作本地渲染器。
  // 不能转发给 Editor::keyPressed，否则会误操作 workspace 中其它嵌入态 Milkdrop。
  if (auto* milk = dynamic_cast<MilkdropModule*>(&module_)) {
    if (! milk->isKeyboardControlEnabled())
      return false;
    if (key == juce::KeyPress::leftKey)  { milk->prevPreset();   return true; }
    if (key == juce::KeyPress::rightKey) { milk->nextPreset();   return true; }
    if (key == juce::KeyPress::spaceKey) { milk->randomPreset(); return true; }
  }
  return false;
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::resized()
{
    DocumentWindow::resized();
    glHost_.setBounds (getLocalBounds());
    if (moduleAttached_)
    {
        module_.setBounds (glHost_.getLocalBounds());
        requestGlFrame();
        // resize → 更新持久化 screenBounds；Milkdrop 还会借此触发 GL 重绘更新共享帧。
        if (editor_ != nullptr)
            triggerAsyncUpdate();
    }
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::handleAsyncUpdate()
{
    if (editor_ == nullptr) return;

    // 仅 Milkdrop 浮窗仍依赖 Editor 共享帧尺寸探测；其余模块已有独立 GL 上下文，
    //   无需拖动浮窗时多余触发主窗口全屏重绘。
    if (module_.getModuleType() == ModuleType::milkdrop)
        editor_->repaint();
    else
        requestGlFrame();

    // v3.0.1：伪全屏只是临时视图，不写回 workspace 的"浮动窗口尺寸"，
    //   否则用户从全屏直接停靠回工作区时，模块会变成一整屏大小。
    if (pseudoFullScreen_)
        return;

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

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::applyModuleResizeLimits()
{
    // 与嵌入态完全一致：不能比模块自身的最小尺寸更小
    setResizeLimits (juce::jmax (1, module_.getMinWidth()),
                     juce::jmax (1, module_.getMinHeight()),
                     0x3fffffff, 0x3fffffff);
}

// ----------------------------------------------------------
// v3.0.1：浮动模块窗口抬头双击 → 伪全屏 / 恢复
//   · Windows：铺满当前显示器的 userArea（已扣除任务栏，**不覆盖任务栏**）；
//     走物理像素 MoveWindow —— 与主窗口 toggleFakeFullScreen 同一套口径，
//     避免混合 DPI 下 setBounds(userArea) 把窗口甩到另一块屏；
//   · macOS：走原生 setFullScreen（独立 Space，覆盖菜单栏/Dock）；
//   · 尺寸约束本来就是"模块最小尺寸 ~ 0x3fffffff"，全屏尺寸不会撞到上限，
//     因此这里不需要像主窗口那样接管 constrainer。
//   · 全屏期间不把窗口几何回写 workspace（见 handleAsyncUpdate），
//     否则停靠回工作区时会变成一整屏大小。
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::togglePseudoFullScreen()
{
   #if JUCE_MAC
    setFullScreen (! isFullScreen());
    return;
   #endif

    if (! pseudoFullScreen_)
    {
        preFullScreenBounds_ = getBounds();

       #if JUCE_WINDOWS
        Y2KMonitorPx monPx;
        if (! y2kScreenSimulationActive()
             && y2kGetMonitorPxForWindow (this, monPx)
             && ! monPx.work.isEmpty())
        {
            y2kMoveWindowToRectPx (this, monPx.work);
            pseudoFullScreen_ = true;
            triggerAsyncUpdate();
            return;
        }
       #endif

        if (auto* display = juce::Desktop::getInstance().getDisplays()
                                .getDisplayForPoint (getScreenBounds().getCentre()))
            setBounds (display->userArea);

        pseudoFullScreen_ = true;
    }
    else
    {
        pseudoFullScreen_ = false;
        setBounds (preFullScreenBounds_);
    }

    triggerAsyncUpdate();
}

void Y2KmeterAudioProcessorEditor::toggleFloatingModuleFullScreen (ModulePanel& panel)
{
    for (auto& w : floatingWindows_)
        if (w != nullptr && &w->getModulePanel() == &panel)
        {
            w->togglePseudoFullScreen();
            return;
        }
}

void Y2KmeterAudioProcessorEditor::dockFloatingModule (ModulePanel* panel,
                                                       juce::Rectangle<int> savedBounds)
{
    if (editorBeingDestructed || panel == nullptr)
        return;

    const bool editorHasGl = openGLContext.isAttached()
                             && ! GlDirectRenderer::isGloballyDisabled();

    for (auto it = floatingWindows_.begin(); it != floatingWindows_.end(); ++it)
    {
        if ((*it) != nullptr && (*it)->getHostedModule() == panel)
        {
            auto wnd = std::move (*it);
            floatingWindows_.erase (it);

            // 仅 Milkdrop 需要从浮动态本地 GLView 把当前预设写回模块状态；
            // 其余模块（含 Spectrogram3D）状态本就在 panel 内，避免在即将销毁的浮窗上下文里触发多余快照重建与重绘。
            if (panel->getModuleType() == ModuleType::milkdrop)
            {
                if (auto state = panel->saveModuleSpecificState(); state.isValid())
                    panel->restoreModuleSpecificState (state);
                --floating_milkdrop_count_;
            }

            // 目标宿主（Editor）同样有活跃 GL 时，在隐藏并释放浮窗 GL 的过程中保持 glDirectActive，
            //   避免停靠瞬间回退到 paintLegacyContent() 闪出旧软件帧。
            wnd->detachGlContext (editorHasGl, true);
            wnd->detachHostedModule();

            if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (panel))
                gmod->prepareForHostTransfer (false, editorHasGl);
            else
                panel->setFloating (false);

            wnd.reset();

            if (workspace != nullptr)
                workspace->dockModule (*panel, savedBounds);

            if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (panel))
            {
                gmod->forceRebuildGlSnapshot();
                if (Y2K_DIAG_ENABLED())
                {
                    const auto edBounds = getLocalArea (workspace.get(), panel->getBounds());
                    Y2K_DIAG ("GL", "dockFloatingModule: " + moduleTypeToString (panel->getModuleType())
                                          + " editorBounds=" + edBounds.toString()
                                          + " glActive=" + juce::String (gmod->glDirectActive.load (std::memory_order_acquire) ? 1 : 0));
                }
            }
            RequestGlFrame();
            return;
        }
    }
}

void Y2KmeterAudioProcessorEditor::closeFloatingModule (ModulePanel* panel)
{
    if (editorBeingDestructed || panel == nullptr)
        return;

    for (auto it = floatingWindows_.begin(); it != floatingWindows_.end(); ++it)
    {
        if ((*it) != nullptr && (*it)->getHostedModule() == panel)
        {
            auto wnd = std::move (*it);
            floatingWindows_.erase (it);

            wnd->detachGlContext (false, true);
            // ★ 先标记回嵌入态，再拆浮动窗口内容：保证 Milkdrop DetachOpenGL 的 should_resume 为真，
            //   Editor renderer 能正确恢复。
            panel->setFloating (false);
            if (workspace != nullptr)
                workspace->removeFloatingState (panel->getModuleId());
            if (panel->getModuleType() == ModuleType::milkdrop)
                --floating_milkdrop_count_;

            wnd->detachHostedModule();
            wnd.reset();
            delete panel;
            return;
        }
    }
}

void Y2KmeterAudioProcessorEditor::FloatingModuleWindow::refreshThemeColours()
{
    syncBackdropTransparency();

    // 让子控件重新读取主题色（ComboBox / Label / TextButton 等），再整体重绘
    sendLookAndFeelChange();
    if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (&module_))
        gmod->forceRebuildGlSnapshot();
    repaint();
    requestGlFrame();
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
    else
    {
        // 解锁后统一回到「模块自身最小尺寸」，而不是恢复可能为 0 的默认限制
        applyModuleResizeLimits();
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
        auto* panel = wnd->getHostedModule();
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
    if (workspace == nullptr)
        return;

    if (! isPluginHost)
    {
        // 先补登记"存活但不在记录里"的浮动窗口（stick 期间新脱离的模块不在备份里），
        // 否则运行期的 dock 回位信息会缺失。
        for (auto& wnd : floatingWindows_)
        {
            if (wnd == nullptr) continue;
            auto* c = wnd->getHostedModule();
            if (c == nullptr || c->getModuleId().isEmpty()) continue;

            const auto& statesNow = workspace->getFloatingModuleStatesRaw();
            if (statesNow.find (c->getModuleId()) == statesNow.end())
                workspace->addFloatingState (c->getModuleId(), c->getModuleType(),
                                             c->getBounds(), wnd->getScreenBounds(),
                                             c->saveModuleSpecificState());
        }

        const auto floatingStates = workspace->getFloatingModuleStatesRaw();
        if (! floatingStates.empty())
        {
            // popOutModule 会修改 workspace 内部 map，不能边遍历引用边修改 → 使用上面的副本
            for (const auto& [id, state] : floatingStates)
            {
                // 已经有同 id 的浮动窗口（例如 stick 期间一直保持浮动的模块）→ 跳过，
                // 否则会重复实例化出第二个窗口。
                bool alreadyOpen = false;
                for (auto& wnd : floatingWindows_)
                {
                    if (wnd == nullptr) continue;
                    if (auto* c = wnd->getHostedModule())
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
                    if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (wnd->getHostedModule()))
                        gmod->forceRebuildGlSnapshot();
                    wnd->requestGlFrame();
                }

                if (! floatingWindows_.empty())
                    workspace->updateFloatingStateScreenBounds (id, floatingWindows_.back()->getScreenBounds());
            }
        }
    }

    // 同步工作区中是否存在嵌入态 Milkdrop：
    //   · 存在时立即启动后台线程预扫 9,921 个预设，与窗口创建 / WGL 初始化并行完成；
    //   · 不存在（或全部在浮动态）时延后 projectM 初始化至真正需要时。
    const bool hasMilkdropNow = hasEmbeddedMilkdropModule();
    milkdrop_has_module_in_editor_.store (hasMilkdropNow, std::memory_order_release);
    if (hasMilkdropNow && ! milkdrop_presets_ready_.load (std::memory_order_acquire))
        startMilkdropPresetScanAsync (milkdrop_use_like_library_.load (std::memory_order_acquire));
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
        const int nameW    = nameFont.getStringWidth (y2k::tr ("Y2Kmeter"));
        // 与 paint() 保持一致：诊断构建的版本号后面还有 " debug"（否则命中矩形偏窄）
        const int versionW = versionFont.getStringWidth (y2k::tr (y2kIsDiagnosticBuild() ? "v3.5.0 debug"
                                                                               : "v3.5.0"));
        const int urlW     = urlFont.getStringWidth (y2k::tr ("iisaacbeats.cn"));
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
        // 绿幕 / 透明背景模式下未悬停显露 Editor chrome 时完全隐藏浮动抬头文字与关闭按钮，
        //   保证空画布纯 alpha=0（透明模式）或纯绿（绿幕模式），无半透明残影。
        if (PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (owner))
            return;

        // ------- 1) 顶部抬头文字：软件名 + 版本号 + 官网（低对比度，贴在底图上）-------
        //   诊断构建额外追加 " debug"（与标题栏 drawTitleBar 保持一致）。
        const juce::String nameText    = "Y2Kmeter";
        const juce::String versionBase = "v3.5.0";
        const juce::String versionTag  = y2kIsDiagnosticBuild() ? " debug" : juce::String();
        const juce::String versionText = versionBase + versionTag;
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont(12.0f, juce::Font::plain);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW        = nameFont.getStringWidth (y2k::tr ("Y2Kmeter"));
        const int versionBaseW = versionFont.getStringWidth (y2k::tr (versionBase));
        const int versionW     = versionFont.getStringWidth (y2k::tr (versionText));   // 含 " debug"
        const int versionTagW  = versionW - versionBaseW;
        const int urlW         = urlFont.getStringWidth (y2k::tr (urlText));

        constexpr int gap1 = 6;
        constexpr int gap2 = 10;

        const int x0 = 28;
        const int y  = 0;
        const int h  = getHeight();

        // chrome 隐藏态：文字淡淡贴在底图上，hover 时略亮并加下划线
        const float textAlpha = titleTextHovered ? 0.95f : 0.55f;

        // 跨字体光学居中（与 drawTitleBar 同一个口径）
        const int nameDy    = juce::roundToInt (PinkXP::opticalNudge (nameFont));
        const int versionDy = juce::roundToInt (PinkXP::opticalNudge (versionFont));
        const int urlDy     = juce::roundToInt (PinkXP::opticalNudge (urlFont));

        // 文字描边（1px 阴影），帮助文字在任何底图上都可读
        g.setColour (juce::Colours::white.withAlpha (textAlpha * 0.6f));
        g.setFont (nameFont);
        g.drawText (y2k::tr (nameText), x0 + 1, y + nameDy + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (y2k::tr (versionText), x0 + nameW + gap1 + 1, y + versionDy + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (y2k::tr (urlText), x0 + nameW + gap1 + versionW + gap2 + 1, y + urlDy + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（墨色）
        g.setColour (PinkXP::ink.withAlpha (textAlpha));
        g.setFont (nameFont);
        g.drawText (y2k::tr (nameText), x0, y + nameDy, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (y2k::tr (versionBase), x0 + nameW + gap1, y + versionDy, versionBaseW, h, juce::Justification::centredLeft, false);
        if (versionTagW > 0)
        {
            // " debug" 用强调色（hover 时更亮），一眼能认出诊断包
            g.setColour (PinkXP::warn.withAlpha (juce::jmax (0.75f, textAlpha)));
            g.drawText (y2k::tr (versionTag), x0 + nameW + gap1 + versionBaseW, y + versionDy, versionTagW, h,
                        juce::Justification::centredLeft, false);
            g.setColour (PinkXP::ink.withAlpha (textAlpha));   // 恢复 url 文字颜色
        }
        g.setFont (urlFont);
        g.drawText (y2k::tr (urlText), x0 + nameW + gap1 + versionW + gap2, y + urlDy, urlW, h, juce::Justification::centredLeft, false);

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

        auto cbText = cb;
        if (closeButtonPressed) cbText.translate (1, 1);
        PinkXP::SymbolGlyphs::drawIconGlyph (g, "x", cbText, 12.0f, PinkXP::ink, -1, -1);
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
        // Toybox：新手引导期间整层静默（§6.1）
        getProperties().set ("toy.optOut", true);
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
            // 语言切换按钮：新手引导期间也允许切换界面语言（点击循环切换）
            const auto langBtn = getLangButtonBounds();
            if (langBtn.contains (pos))
            {
                y2k::i18n::cycleToNextLanguage();
                repaint();
                return;
            }

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

    // 气泡标题行右侧、× 左边的语言切换按钮（新手引导期间允许切换界面语言）
    juce::Rectangle<int> getLangButtonBounds() const
    {
        const auto cb = getCloseButtonBounds();
        constexpr int btnW = 42;
        return { cb.getX() - 4 - btnW, cb.getY(), btnW, 18 };
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
        g.drawText (y2k::tr ("SKIP TUTORIAL?"), titleRow.reduced (3, 1), juce::Justification::centredLeft, false);

        // 正文
        auto body = inner.reduced (2, 2);
        auto textArea = body.removeFromTop (body.getHeight() - 24);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f));
        g.drawFittedText (y2k::tr ("U will miss the fun :(\nthis can't be undone~"),
                          textArea, juce::Justification::centred, 2);

        // Cancel 按钮（始终平面，不单独高亮）
        auto cancelBounds = getConfirmBtnBounds (0);
        PinkXP::drawRaised (g, cancelBounds, PinkXP::btnFace);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText (y2k::tr ("Cancel"), cancelBounds, juce::Justification::centred, false);

        // Skip 按钮（粉红突出，始终醒目）
        auto skipBounds = getConfirmBtnBounds (1);
        PinkXP::drawRaised (g, skipBounds, PinkXP::pink200);
        g.setColour (PinkXP::pink700);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText (y2k::tr ("Skip"), skipBounds, juce::Justification::centred, false);
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
        g.drawText (y2k::tr (title), titleRow.reduced (3, 1), juce::Justification::centredLeft, false);

        // 正文
        auto body = inner.reduced (2, 2);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f));
        const juce::String text = (currentStep == Step::step1)
            ? (menuIsOpen
                ? "Click 'VirtuPet' in\nthe menu to add ur pet! <3"
                : "Right-click the canvas to\nadd ur VirtuPet pet! <3")
            : "Play some audio to\nhatch the egg! :3";
        g.drawFittedText (y2k::tr (text), body, juce::Justification::centred, 2);

        // 语言切换按钮（× 左边）：显示"目标语言"标签，点击循环切换。
        //   新手引导期间工具栏被遮罩挡住，这里是唯一的语言切换入口。
        {
            auto lb = getLangButtonBounds();
            PinkXP::drawRaised (g, lb, PinkXP::btnFace);

            const auto currentCode = y2k::i18n::getCurrentLanguageCode();
            juce::String targetLabel;
            for (int i = 0; i < y2k::i18n::getNumLanguages(); ++i)
                if (y2k::i18n::getLanguage (i).code != currentCode)
                {
                    targetLabel = y2k::i18n::getLanguage (i).buttonLabel;
                    break;
                }

            if (targetLabel.isNotEmpty())
            {
                g.setColour (PinkXP::ink);
                // 目标语言标签可能是 CJK（"中"）：必须用当前语言字体，
                //   不能走"纯符号 → 英文字体"分支（Silkscreen 无 CJK 字形）。
                PinkXP::SymbolGlyphs::drawText (g, targetLabel, lb,
                                                juce::Justification::centred,
                                                PinkXP::getFont (8.0f, juce::Font::bold));
            }
        }

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
            g.setFont   (PinkXP::SymbolGlyphs::iconFont (11.0f, juce::Font::bold));
            auto cbText = cb;
            cbText.translate (-1, -1);
            if (closeBtnPressed) cbText.translate (1, 1);
            g.drawText (y2k::tr ("x"), cbText, juce::Justification::centred, false);
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

    // 0) 运行时诊断日志（现场排查用）：插件形态下也必须尽早初始化，
    //    否则"打开界面就卡死"的场景连日志文件都来不及创建。
    y2k::diag::ensureInitialised();
    y2k::diag::setThreadTag ("UI");
    y2k::diag::setPhase ("Editor::ctor");
    y2k::diag::heartbeat (y2k::diag::Channel::UiMessage);
    Y2K_DIAGC ("BOOT", juce::String ("PluginEditor ctor begin | pluginHost=")
                           + (isPluginHost ? "yes" : "no")
                           + " log=" + y2k::diag::getLogFile().getFullPathName());

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

    // Logo 进程级静态软件位图缓存（SoftwareImageType，预缩至最大水印边长 1280px）：
    //   · 原始 logo.png 为 3000×3000 RGBA（解码需 ~36MB），而 PinkXP::drawLogo()
    //     按工作区短边 80% 绘制（4K@150% DPI 最大约 1093px）；预缩至 1280px
    //     画质 100% 等价且峰值内存降 5.8×，多开/重开编辑器零重复 PNG 解码；
    //   · 显式使用 SoftwareImageType 避免静态对象持有任何 Direct2D/COM 资源
    //     （VST3 DLL 卸载安全）。
    {
      static const juce::Image sCachedLogo = []() -> juce::Image {
        try {
          juce::Image decoded = juce::ImageFileFormat::loadFrom (
              BinaryData::logo_png, (size_t) BinaryData::logo_pngSize);
          if (decoded.isNull())
            return {};

          constexpr int kMaxLogoSide = 1280;
          const int srcW = decoded.getWidth();
          const int srcH = decoded.getHeight();
          int dstW = srcW;
          int dstH = srcH;
          if (srcW > kMaxLogoSide || srcH > kMaxLogoSide)
          {
            const float scale = (float) kMaxLogoSide / (float) juce::jmax (srcW, srcH);
            dstW = juce::jmax (1, juce::roundToInt ((float) srcW * scale));
            dstH = juce::jmax (1, juce::roundToInt ((float) srcH * scale));
          }

          juce::Image soft (juce::Image::ARGB, dstW, dstH, true, juce::SoftwareImageType{});
          juce::Graphics gg (soft);
          gg.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
          gg.drawImage (decoded, 0, 0, dstW, dstH, 0, 0, srcW, srcH);
          return soft;
        } catch (const std::exception& e) {
          DBG ("!! Logo load failed (exception): " << e.what());
          return {};
        } catch (...) {
          DBG ("!! Logo load failed (unknown exception)");
          return {};
        }
      }();
      logoImage = sCachedLogo;
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
    // ⚠️ 这份列表才是"添加模块"菜单的**唯一事实来源**（会覆盖
    //   ModuleWorkspace::availableTypes 的默认值）。
    workspace->setAvailableModuleTypes({
        // v3.1：响度 / 示波器 / 相位 / 动态四类已合并，旧的细分类型不再出现在菜单里
        //   （旧存档里的细分类型字符串仍能加载，见 resolveRetiredModuleType）。
        ModuleType::loudness,

        // 模拟 VU 指针表（复用 Loudness 路的 RMS L/R，后端零新增计算）
        ModuleType::vuMeter,

        ModuleType::oscilloscope,

        ModuleType::spectrum,

        ModuleType::phase,

        ModuleType::dynamics,

        // 持续滚动瀑布波形（复用 Oscilloscope 原始样本，后端零新增计算）
        ModuleType::waveform,

        // 实时频谱瀑布图（复用 Spectrum 路的 FFT 幅度，后端零新增计算）
        ModuleType::spectrogram,

        // 3D 频谱瀑布图（45° 俯视三维曲面效果）
        ModuleType::spectrogram3d,

        // 声相指示（半圆雷达，复用 Oscilloscope 立体声样本）
        ModuleType::stereoField,

        // 按频率显示左右声道声像偏向（上 L / 下 R）
        ModuleType::stereoMap,

        // 独立小宠物模块（右键/双击空白区添加）
        ModuleType::virtuPet,

        // Milkdrop WebGL 可视化模块（WebView 嵌入 Butterchurn 引擎）
        ModuleType::milkdrop,

        // 实时 BPM + 音乐调性（后台线程分析同一路 PCM）
        ModuleType::tempoKey,

        // 录音机（红点录制 pre-gain 原始输入，保存 32-bit float WAV）
        ModuleType::recorder,

        // v3.3：电脑状态（CPU / GPU / 温度 / 内存 / 网络…，后台线程采集系统公开计数器）
        ModuleType::systemMonitor,

        // v3.3：小工具（计算器 / 取色器 / 秒表 / 倒计时 / 音频换算 / 时钟 / 便签）
        ModuleType::utility
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
    //
    // v3.0.1 修复：插件宿主模式**没有** L 按钮（paint / hit-test 都跳过它），
    //   一旦从存档（shared settings 或工程状态）里恢复了 layoutLocked=true，
    //   用户看到的就是"所有模块拖不动 / 关不掉 / 右键弹不出添加菜单"，
    //   而且没有任何按钮能解锁 —— 即外网反馈的"VST 模式偶发锁死"。
    //   这里强制清零并回写一次，让该工程自愈（下次打开不再锁定）。
    if (isPluginHost && processor.getLayoutLocked())
    {
        processor.setLayoutLocked (false);
        Y2K_LOG ("[Layout] plugin host: cleared stale layoutLocked state");
    }

    applyLayoutLocked (isPluginHost ? false : processor.getLayoutLocked(), /*initial*/ true);
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
    if (workspace != nullptr)
        workspace->resetUndoHistory();

    // 4) 用户布局变更 → 立即写回 Processor state 的"当前活动槽"
    //    （v2.8.5：stick 期间写 stick 槽，绝不污染非 stick 布局）
    workspace->onLayoutChanged = [this]()
    {
        persistCurrentLayoutToProcessor();
    };
    workspace->onCaptureHostWindowState = [this]()
    {
        return captureHostWindowState();
    };
    workspace->onRestoreHostWindowState = [this](const juce::ValueTree& state)
    {
        restoreHostWindowState (state);
    };

    // 4.0) v3.0.1：浮动态模块抬头双击 → 该浮动窗口伪全屏 / 恢复
    workspace->onFloatingTitleDoubleClicked = [this](ModulePanel& panel)
    {
        toggleFloatingModuleFullScreen (panel);
    };

    // 4.0) 模块弹出请求 → 创建浮动窗口（仅 Standalone 模式）
    workspace->onModulePopOutRequested = [this](ModulePanel* panel,
                                                 juce::Rectangle<int> savedBounds)
    {
        if (panel == nullptr) return;

        // Milkdrop 嵌入态的当前预设在 Editor renderer 中，浮动态会新建本地 renderer；
        // 若不先写入 restored state，新 renderer 会从预设 0 启动。
        if (panel->getModuleType() == ModuleType::milkdrop)
        {
            auto currentModuleState = panel->saveModuleSpecificState();
            if (currentModuleState.isValid())
                panel->restoreModuleSpecificState (currentModuleState);
        }

        // 模块离开 Editor 上下文前，把 Editor 侧为其创建的 GPU 资源排入延迟回收队列
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (panel))
            if (glDirectRenderer_ != nullptr)
                glDirectRenderer_->scheduleReleaseModule (gmod->glResourceKey);

        const bool willUseFloatingGl = (panel->getModuleType() != ModuleType::milkdrop)
                                       && ! GlDirectRenderer::isGloballyDisabled()
                                      #if ! JUCE_WINDOWS
                                       && ! PinkXP::isTransparentBackdrop()
                                      #endif
                                       ;

        // ★ 重新绑定 onPopOutClicked：浮动态下点击停靠按钮 → 异步投递至消息队列执行 dockFloatingModule。
        //   必须异步执行：① 避免在 FloatingModuleWindow 自身 WM_LBUTTONUP / ModulePanel::mouseUp 栈帧内
        //   同步销毁 HWND peer 与 OpenGLContext；② 避免 workspace->dockModule() → hookPanel() 原地覆写
        //   panel->onPopOutClicked 导致正在执行的闭包 capture（this/panel）被破坏产生 UAF/类型混淆崩溃。
        juce::Component::SafePointer<Y2KmeterAudioProcessorEditor> safeEditor (this);
        juce::Component::SafePointer<ModulePanel> safePanel (panel);
        panel->onPopOutClicked = [safeEditor, safePanel, savedBounds] (ModulePanel&)
        {
            juce::MessageManager::callAsync ([safeEditor, safePanel, savedBounds]()
            {
                if (auto* ed = safeEditor.getComponent())
                    if (auto* p = safePanel.getComponent())
                        ed->dockFloatingModule (p, savedBounds);
            });
        };

        // ★ 重新绑定 onCloseClicked：浮动态下关闭模块 → 异步销毁浮动窗口并删除模块，
        //   避免在 ModulePanel::mouseUp / 浮窗 WM_LBUTTONUP 回调栈内直接 delete panel 与销毁 HWND。
        panel->onCloseClicked = [safeEditor, safePanel] (ModulePanel&)
        {
            juce::MessageManager::callAsync ([safeEditor, safePanel]()
            {
                if (auto* ed = safeEditor.getComponent())
                    if (auto* p = safePanel.getComponent())
                        ed->closeFloatingModule (p);
            });
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

        auto name = y2k::tr (panel->getTitleText()) + " - Y2Kmeter";
        auto* window = new FloatingModuleWindow(
            name, *panel, savedBounds,
            [safeEditor] (ModulePanel& p, juce::Rectangle<int> bounds)
            {
                juce::Component::SafePointer<ModulePanel> safeP (&p);
                juce::MessageManager::callAsync ([safeEditor, safeP, bounds]()
                {
                    if (auto* ed = safeEditor.getComponent())
                        if (auto* mp = safeP.getComponent())
                            ed->dockFloatingModule (mp, bounds);
                });
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

        // 先把 panel 从 workspace 移入浮窗子级 glHost_，再切换浮动态并保留 GL 接管态：
        //   ① 避免在旧父容器 workspace 中误置 pendingFullRepaint 导致后续 120ms 重绘被吞；
        //   ② 目标浮窗具备独立 GL 时不复位 glDirectActive，杜绝弹出瞬间回退旧软件路径闪错帧。
        window->attachHostedModule();
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (panel))
            gmod->prepareForHostTransfer (true, willUseFloatingGl);
        else
            panel->setFloating (true);

        // 模块脱离 tree 后 bounds 可能被清零，显式恢复到脱离前尺寸
        panel->setBounds(savedBounds.withPosition(0, 0));
        window->setBounds(screenBounds);
        // 缩放下限统一成模块自身的最小尺寸（必须在 setBounds 之后、锁定之前）
        window->applyModuleResizeLimits();
        window->setLayoutLocked(layoutLocked);

        window->addToDesktop();               // 创建 native peer → 触发 resized() → 模块填满窗口

       #if JUCE_WINDOWS
        // 浮窗同步切回 Software 2D 后端（与主窗口一致，规避 AMD D2D 卸载死锁），
        //   随后由下方 attachGlContextIfEligible() 挂载独立 OpenGLContext 承担全部 GPU 渲染！
        if (auto* floatPeer = window->getPeer())
            if (floatPeer->getAvailableRenderingEngines().size() > 1)
                floatPeer->setCurrentRenderingEngine (0);
       #endif
        window->syncBackdropTransparency();

       #if JUCE_WINDOWS
        // 多屏（混合 DPI）落位修正（v2.9.9 经验：跨屏定位必须物理像素——JUCE 的
        //   setBounds 会按"窗口当前所在显示器"换算，目标屏不是当前屏时位置整体用错
        //   原点。模块靠近上方低分辨率屏时，peer 创建的初始落点会整块跳偏）。
        //   修正：模块相对 Editor 的逻辑偏移 × Editor 所在屏 scale = 物理偏移，
        //   以 Editor 物理左上角为基准 SetWindowPos（SWP_NOSIZE，尺寸交给系统按
        //   目标屏处理），再夹到落点所在显示器的物理边界内。窗口尚未 setVisible，
        //   全程不可见，无闪烁。
        if (! y2kScreenSimulationActive())
        {
            if (auto* top = getTopLevelComponent())
            {
                Y2KMonitorPx mon;
                if (y2kGetMonitorPxForWindow (top, mon) && ! mon.total.isEmpty())
                {
                    const auto topLogical = top->getScreenBounds();
                    const auto topPx      = y2kGetWindowRectPx (top);
                    if (! topPx.isEmpty())
                    {
                        const juce::Point<int> px {
                            topPx.getX() + y2kLogicalToPx (screenBounds.getX() - topLogical.getX(), mon.scale),
                            topPx.getY() + y2kLogicalToPx (screenBounds.getY() - topLogical.getY(), mon.scale)
                        };

                        Y2KMonitorPx target;
                        if (y2kGetMonitorPxForPoint (px, target) && ! target.total.isEmpty())
                        {
                            const auto winRect = y2kGetWindowRectPx (window);
                            y2kMoveWindowToPx (window, {
                                juce::jlimit (target.total.getX(),
                                              juce::jmax (target.total.getX(),
                                                          target.total.getRight()  - winRect.getWidth()),  px.x),
                                juce::jlimit (target.total.getY(),
                                              juce::jmax (target.total.getY(),
                                                          target.total.getBottom() - winRect.getHeight()), px.y) });
                        }
                        else
                        {
                            y2kMoveWindowToPx (window, px);
                        }
                    }
                }
            }
        }
       #endif

        // Windows 独立 GL 浮窗：
        //   ① setVisible(true) 前先开启 DWMWA_CLOAK 并暂隐子容器 glHost_，防止 Windows 在
        //      ShowWindow/SetWindowPos 同步发送 WM_PAINT 时（此时 glHost_ 尚未挂 CachedImage）
        //      直接以软件 GDI 绘出 module_::paintLegacyContent()；
        //   ② setAlwaysOnTop + setVisible 完成后立即恢复 glHost_ 可见并同步 attachGlContextIfEligible()，
        //      确保任何后续 WM_PAINT 均命中 glHost_::cachedImage，且等首帧 GL SwapBuffers +
        //      子 GL HWND 显示后再瞬间揭幕。
        if (willUseFloatingGl)
        {
            window->beginCloakUntilFirstGlFrame();
            window->setGlHostVisible (false);
        }

        window->setAlwaysOnTop(true);         // native peer 已存在，置顶生效
        window->setVisible(true);             // ★ DocumentWindow ctor addToDesktop=false 时 isVisible=false，
                                              //    addToDesktop() 仅创建 peer 不保证 setVisible(true)
        if (willUseFloatingGl)
            window->setGlHostVisible (true);

        // 独立浮窗挂载专属 OpenGLContext + GlDirectRenderer（与主窗口完全一致的 GPU 渲染管线）
        window->attachGlContextIfEligible();
        if (Y2K_DIAG_ENABLED())
        {
            const auto* gmod = dynamic_cast<const GlDirectModuleBase*> (panel);
            Y2K_DIAG ("GL", "popOutModule: " + moduleTypeToString (panel->getModuleType())
                                  + " screenBounds=" + window->getScreenBounds().toString()
                                  + " glAttached=" + juce::String (window->isGlAttached() ? 1 : 0)
                                  + " glActive=" + juce::String (gmod != nullptr && gmod->glDirectActive.load (std::memory_order_acquire) ? 1 : 0));
        }

        // 主窗口立刻刷清已脱离模块的旧占位区域
        if (workspace != nullptr)
            workspace->repaint();
        RequestGlFrame();

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

            const bool editorHasGl = openGLContext.isAttached()
                                     && ! GlDirectRenderer::isGloballyDisabled();
            for (auto& wnd : floatingWindows_)
            {
                if (wnd != nullptr)
                {
                    wnd->detachGlContext (editorHasGl, true);
                    if (auto* content = wnd->getHostedModule())
                    {
                        const auto savedBounds = wnd->getSavedBounds();
                        wnd->detachHostedModule();
                        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (content))
                            gmod->prepareForHostTransfer (false, editorHasGl);
                        else
                            content->setFloating (false);
                        workspace->dockModule(*content, savedBounds);
                        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (content))
                            gmod->forceRebuildGlSnapshot();
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

        if (workspace != nullptr)
        {
            ModuleWorkspace::ScopedUndoTransaction undoTx (
                *workspace, y2k::tr ("Switch layout preset"), /*structural=*/true, /*captureWindow=*/true);
            applyLayoutPreset ((int) preset);
        }
        else
        {
            applyLayoutPreset ((int) preset);
        }
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
    workspace->onAudioSourceListRequested = [this]()
    {
        if (onAudioSourceListRequested) onAudioSourceListRequested();
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
        vblankPacer_.reset (true);   // 保留已测得的显示器刷新率估计
        syncToyExpectedFps();
        updateFramePacingFallbackTimer();
        lastFpsTimeMs = juce::Time::getMillisecondCounterHiRes();
    };

    // 4.4) gain 控制条变化 → 下发到分析输入增益（仅影响分析链，不影响透传输出）
    workspace->onInputGainChanged = [this](float db)
    {
        processor.setAnalysisInputGainDb (db);
    };
    workspace->setInputGainDb (processor.getAnalysisInputGainDb());

    // 4.5) FFT size 按钮切换（反馈 #7 第二轮）→ 修改 AnalyserHub 主路 FFT 点数
    workspace->onFftSizeChanged = [this](int size)
    {
        processor.getAnalyserHub().setMainFftSize (size);
    };
    // 初始同步：把控制台档位应用到后端（workspace 默认 2048 与 Hub 默认一致）
    processor.getAnalyserHub().setMainFftSize (workspace->getFftSize());

    // 颜色存档独立：把用户级的独立主题库文件（Y2Kmeter.themeLibrary.xml）读入内存。
    //   · Standalone 在 initialise 1.15 已经读过（本函数幂等，直接返回）；
    //   · VST3/AU 没有那个启动阶段，靠这里保证"我的主题"在编辑器首次绘制前就位，
    //     且它是用户级的、不会随宿主工程切换而丢失。
    PinkXP::ThemeLibrary::instance().ensureLoadedFromLibraryFile();

    // 订阅主题变更 → 切主题后刷新全局重绘
    // 绿幕 / 透明背景模式：Editor 自身（标题栏 / 工具栏）是一个悬停显露区域；
    //   透明模式下通过 markChromaChromeOnlyArea 仅在标题栏 + 工具栏区域显露，
    //   模块之间的空白画布缝隙保持 100% 透明与点击穿透。
    if (PinkXP::isTransparentBackdrop())
        setOpaque (false);

    PinkXP::markChromaRegion (*this);
    PinkXP::markChromaChromeOnlyArea (*this, [this] (juce::Point<int> p) -> bool
    {
        if (! PinkXP::isTransparentBackdrop())
            return true;

        // 1) 顶部标题栏（Hide 态下取顶部 18px 热区）
        const int topBandH = chromeDim ? PinkXP::ui (18)
                                       : (getTitleBarBounds().getBottom() + 1);
        if (p.y >= 0 && p.y < topBandH)
            return true;

        // 2) 底部工具栏 + 展开中的非模块工作区浮层（More 溢出条 / Gain 滑条 / 内嵌调色工作室）
        if (workspace != nullptr)
        {
            const auto wsLocal = p - workspace->getPosition();
            const auto tb = workspace->getToolbarArea();
            if (! tb.isEmpty() && tb.contains (wsLocal))
                return true;

            for (int i = workspace->getNumChildComponents(); --i >= 0;)
            {
                auto* ch = workspace->getChildComponent (i);
                if (ch == nullptr || ! ch->isVisible())
                    continue;
                if (dynamic_cast<ModulePanel*> (ch) != nullptr)
                    continue;
                if (ch->getBounds().contains (wsLocal)
                    && (ch->getWidth() < workspace->getWidth() || ch->getHeight() < workspace->getHeight()))
                    return true;
            }
        }

        return false;
    });

    chromaRevealSubToken = PinkXP::subscribeChromaRevealChanged ([this] (juce::Component* prev, juce::Component* next)
    {
        syncEditorChromeAlpha();
        if (prev == this || next == this || PinkXP::isTransparentBackdrop())
            repaint (getTitleBarBounds().withTrimmedBottom (-1));
    });

    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        Y2K_LOG ("[palette] editor theme callback: invalidate desktop cache");
        if (auto* lnf = dynamic_cast<juce::LookAndFeel_V4*>(&getLookAndFeel()))
            juce::ignoreUnused(lnf);
        if (PinkXP::isTransparentBackdrop())
        {
            setOpaque (false);
           #if ! JUCE_WINDOWS
            // Windows 通过 DWM 硬件 Alpha 保持 OpenGLContext 持续挂载（60-120 FPS）；
            //   非 Windows（macOS）走 CoreAnimation 透明层，摘除 GL 由 CPU 路径绘制。
            setOpenGlEnabledForBackdrop (false);
           #endif
        }
        syncEditorChromeAlpha();
        invalidateDesktopCache();
        repaint();

        // 浮动模块是独立的 top-level 窗口，不在 Editor 的组件子树里，
        //   上面的 repaint() 覆盖不到：这里逐个刷新窗口背景色、通知子控件重读
        //   主题色并重绘，否则脱离态下只有自己会重绘的控件（如按钮）跟着变。
        for (auto& wnd : floatingWindows_)
            if (wnd != nullptr)
                wnd->refreshThemeColours();

        // 视觉风格切换后字体也要换（像素 / 矢量 / 等宽终端字体）：广播期间不能
        //   再嵌套广播，异步走与切语言相同的"换字体 + 刷新布局"流程。
        if (PinkXP::activeTypefaceStale())
        {
            juce::Component::SafePointer<juce::Component> safe (this);
            juce::MessageManager::callAsync ([safe]
            {
                if (auto* self = dynamic_cast<Y2KmeterAudioProcessorEditor*> (safe.getComponent()))
                    if (PinkXP::activeTypefaceStale())
                        self->applyLanguageChange();
            });
        }
    });

    // 应用默认主题（会把全局配色重写一次，确保状态一致）
    PinkXP::applyTheme(PinkXP::getCurrentThemeId());

    // 订阅界面语言切换（工具栏语言按钮 → y2k::i18n::setLanguage → 这里统一刷新）
    languageSubToken = y2k::i18n::subscribe ([safe = juce::Component::SafePointer<juce::Component> (this)]()
    {
        if (auto* self = dynamic_cast<Y2KmeterAudioProcessorEditor*> (safe.getComponent()))
            self->applyLanguageChange();
    });

    // 构造阶段 isShowing() 可能仍为 false，先确保分析链启动；
    // 后续由 visibilityChanged() 继续做可见性联动。
    processor.setAnalysisActive(true);

    // Editor 自身 10Hz 轮询：拉 Processor 的 CPU 占比并广播到所有模块的
    //   右下角标签，同时计算实际 FPS 下发给 workspace 的 FPS 标签
    startTimerHz (10);

    // 诊断：到这里为止构造已完成、Timer 已启动，UI 通道才开始参与
    //   "卡死"判定（否则构造本身耗时较长会被误报）。
    y2k::diag::setChannelActive (y2k::diag::Channel::UiMessage, true);
    y2k::diag::heartbeat (y2k::diag::Channel::UiMessage);

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

    // ------------------------------------------------------
    // Toybox：点击解压互动主管（挂在 Editor 全局鼠标监听 + FrameListener）
    //   · 启动时恒为 Chill；档位由连续互动（间隔 < 3 s）每 5 次自动升一档，
    //     3 s 无互动自动降回 Chill；不再有工具栏切换按钮 / OFF 选项。
    // ------------------------------------------------------
    Y2K_LOG ("[boot] ToyDirector create begin");
    toyDirector = std::make_unique<ToyDirector> (*this, *workspace, processor.getAnalyserHub());
    glToyRenderer_ = std::make_unique<y2ktoy::GlToyRenderer>();
    Y2K_LOG ("[boot] ToyDirector created");

    // Toybox 档位改为「连续互动自动升级」：启动时恒为 Chill，
    //   不再从工程状态 / ui.toyLevel 恢复（旧存档里的 Off=0 也不再可达）。
    const int initialToy = (int) ToyLevel::Chill;
    workspace->setToyDirector (toyDirector.get());
    workspace->setToyLevel (initialToy);
    toyDirector->setLevel ((ToyLevel) initialToy);
    toyDirector->setGlFrameRequester ([this]()
    {
        RequestGlFrame();
    });
    toyDirector->setExternalExclusionRectsProvider ([this] (std::vector<juce::Rectangle<int>>& out)
    {
        // workspace->collectToyExclusionRects(*this, out) 已由 ToyDirector::collectExclusionRects() 直接调用，
        //   这里只追加 Editor 顶层自持的可见浮层矩形。
        if (tooltipWindow != nullptr && tooltipWindow->isVisible() && ! tooltipWindow->getBounds().isEmpty())
            out.push_back (tooltipWindow->getBounds());

        if (tutorialOverlay != nullptr && tutorialOverlay->isVisible() && ! tutorialOverlay->getBounds().isEmpty())
            out.push_back (tutorialOverlay->getBounds());

        if (chromeDim && chromeRevealed
            && ! (PinkXP::isTransparentBackdrop() && ! PinkXP::isChromaRevealed (*this)))
        {
            const auto tb = getTitleBarBounds();
            if (! tb.isEmpty())
                out.push_back (tb);
        }
    });
    syncToyExpectedFps();
    toyDirector->onLevelChanged = [this] (int level)
    {
        if (workspace) workspace->setToyLevel (level);
        processor.setSavedToyLevel (level);
    };

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
    //   macOS（v2.9.7 GPU 化）：重新启用 Editor GL。
    //     历史背景：2026-05 曾因"各模块每帧修改 juce::Image → NSOpenGLContext
    //     整图纹理重传"的瓶颈主动关闭 mac Editor GL（见 git 历史）。但 v2.9.4
    //     的 GlDirectModule 直渲染框架已不再走 Image 上传（直接 VBO 直画），
    //     该瓶颈不复存在，因此恢复启用，让 mac 也享受：
    //       ① GlDirectModuleBase 模块（Spectrum/Oscilloscope/Spectrogram/
    //          Waveform/Phase/S3D 等）的 GPU 直渲染；
    //       ② JUCE 组件树的 CPE 纹理合成（替代 CoreGraphics 软光栅）。
    //     注意：macOS 上 Milkdrop 嵌入态仍由 GLView 自己的 OpenGL 上下文渲染
    //     （见 MilkdropModule::UpdateOpenGLAttachment 的 JUCE_MAC 分支），
    //     Editor 侧不创建第二个 projectM 实例（newOpenGLContextCreated 的
    //     JUCE_MAC 分支提前返回），两者通过嵌套 NSView 共存。
    // ==================================================================
#if JUCE_MAC
    // projectM 4 的 GLSL shader 需要 OpenGL Core Profile 3.2+。
    // 不设置时 macOS 默认给 Legacy Profile（NSOpenGLProfileVersionLegacy），
    // projectM 在 Editor 上下文中 shader 编译失败、画面全黑（与 v2.5.3
    // Milkdrop GLView 的根因四相同）。仅 macOS 强制：Windows 强制 Core
    // Profile 会导致部分预设 shader 编译失败（历史实测），保持默认。
    openGLContext.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
#endif
    openGLContext.setRenderer(this);
    openGLContext.setContinuousRepainting(false);
    openGLContext.setComponentPaintingEnabled(true);
#if JUCE_MAC
    // mac 性能：repaint() 不再立即唤醒 GL 线程，而是合并到下一个显示器刷新
    //   （CVDisplayLink）再出帧 —— 各模块 / 计时器相位不同的 repaint 在同一刷新
    //   周期内只画一帧，GL 帧率与屏幕刷新率对齐（此前约 90~100 帧/秒 @60Hz）。
    //   JUCE 补丁，见 OpenGLContext::setComponentRepaintsPacedToDisplay。
    openGLContext.setComponentRepaintsPacedToDisplay(true);
#else
    // Win/非 macOS 性能（JUCE 补丁 0010）：仅在外部 VBlank 帧时钟接管期间合并组件 repaint
    //   与 RequestGlFrame（并在 performFrameDispatch 末尾统一 flushPacedRepaint）；
    //   启动尚未进入外部时钟或看门狗回退内部 Timer 时保持关闭，避免任何帧被延迟或丢失。
    openGLContext.setExternalFramePacing (processor.getAnalyserHub().isUsingExternalFrameClock());
#endif

    // 诊断：attachTo 是"整个窗口进入 GL 合成"的开关，某些驱动在这一步就会
    //   卡住或抛异常。前后各留一行，卡在这里时日志能立刻看出来。
    //   · Windows：即使在 BackdropMode::transparent 下也始终挂载 OpenGLContext，
    //     借助 DwmEnableBlurBehindWindow + FBO 0 Premultiplied Alpha 实现 60-120 FPS 真透明；
    //   · 非 Windows（macOS）：透明模式跳过 GL 挂载，走 CoreAnimation 逐像素透明。
   #if JUCE_WINDOWS
    const bool attachGlNow = true;
   #else
    const bool attachGlNow = ! PinkXP::isTransparentBackdrop();
   #endif
    if (attachGlNow)
    {
        Y2K_DIAGC ("GL", juce::String ("openGLContext.attachTo begin | bounds=")
                              + juce::String (getWidth()) + "x"
                              + juce::String (getHeight()) + " displayScale="
                              + juce::String ((double) openGLContext.getRenderingScale(), 2));
        {
            Y2K_DIAG_SLOW ("GL", "openGLContext.attachTo", 500.0);
            openGLContext.attachTo(*this);
        }
        Y2K_DIAGC ("GL", juce::String ("openGLContext.attachTo done | isAttached=")
                              + (openGLContext.isAttached() ? "1" : "0"));
        y2k::diag::setGlAttached (openGLContext.isAttached());
        if (toyDirector != nullptr)
            toyDirector->setGlToyHostActive (openGLContext.isAttached());
    }
    else
    {
        Y2K_DIAGC ("GL", "openGLContext.attachTo skipped (BackdropMode::transparent)");
        y2k::diag::setGlAttached (false);
        if (toyDirector != nullptr)
            toyDirector->setGlToyHostActive (false);
    }

    syncEditorChromeAlpha();

    // 悬停提示窗口：以 Editor 为宿主（随 GL 合成绘制；插件宿主内不开原生窗口）
    tooltipWindow = std::make_unique<Y2KTooltipWindow> (this);

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
    editorBeingDestructed.store (true, std::memory_order_release);

    // 停掉并回收 Milkdrop 后台预设扫描线程（先置 shutdown 再获锁 join，确保之后任何线程都无法重启）
    milkdrop_scan_shutdown_.store (true, std::memory_order_release);
    stopMilkdropPresetScanThread();

    // 悬停提示窗口最先拆：停掉它的轮询 Timer / 全局鼠标监听，
    //   避免后续 dock / workspace 析构过程中再去查询正在销毁的模块。
    tooltipWindow.reset();

    y2k::diag::setThreadTag ("UI");
    y2k::diag::setPhase ("Editor::~ctor");
    Y2K_DIAGC ("GL", "Editor destructor begin");

    // 析构期间关闭心跳判定：编辑器销毁/窗口关闭本身就要停 Timer、拆 GL，
    //   不关掉会被看门狗误判成"卡死"。析构若真的挂住，日志最后一行
    //   （phase=Editor::~ctor）仍然在，足够定位。
    y2k::diag::setChannelActive (y2k::diag::Channel::GlFrame, false);
    y2k::diag::setChannelActive (y2k::diag::Channel::UiMessage, false);

    // 解绑顶层窗口级 Ctrl+Z 快捷键监听，避免析构后悬垂回调
    if (auto* host = topLevelUndoKeyHost_.getComponent())
        host->removeKeyListener (&topLevelUndoKeyListener_);
    topLevelUndoKeyHost_ = nullptr;

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
            wnd->detachGlContext (false, true);
            if (auto* content = wnd->getHostedModule())
            {
                const auto savedBounds = wnd->getSavedBounds();
                content->setFloating (false);
                wnd->detachHostedModule();
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
    Y2K_DIAGC ("GL", "openGLContext.detach begin");
    {
        Y2K_DIAG_SLOW ("GL", "openGLContext.detach", 500.0);
        openGLContext.detach();
    }
    y2k::diag::setGlAttached (false);
    Y2K_DIAGC ("GL", "openGLContext.detach done");

    // GL 线程已完全退出：再次回收扫描线程，消除析构竞态窗口
    stopMilkdropPresetScanThread();

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

    PinkXP::markChromaChromeOnlyArea (*this, {});
    if (chromaRevealSubToken != 0)
    {
        PinkXP::unsubscribeChromaRevealChanged (chromaRevealSubToken);
        chromaRevealSubToken = 0;
    }

    if (themeSubToken != 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = 0;
    }

    if (languageSubToken != 0)
    {
        y2k::i18n::unsubscribe (languageSubToken);
        languageSubToken = 0;
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
    customTypeface = nullptr;
    // 同理释放按界面语言加载的字体缓存（Silkscreen / 中文像素字体等）
    PinkXP::releaseTypefaceCache();
    // 绿幕悬停轮询定时器（进程级静态对象）：不能拖到 DLL 卸载期再析构
    PinkXP::releaseChromaHoverTracker();

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
// 默认工作区布局（v3.5）：干净双行平铺，零重叠，全部使用现行存活模块
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::seedDefaultModules()
{
    if (workspace == nullptr) return;
    auto canvas = workspace->getCanvasArea();
    if (canvas.getWidth() <= 16 || canvas.getHeight() <= 16)
        canvas = workspace->getLocalBounds();

    const auto spec = resolvePresetSpec ((int) ModuleWorkspace::LayoutPreset::defaultGrid);
    constexpr int kGrid = 8;
    const int xL = ((canvas.getX() + kGrid - 1) / kGrid) * kGrid;
    const int xR = (canvas.getRight()  / kGrid) * kGrid;
    const int yT = ((canvas.getY() + kGrid - 1) / kGrid) * kGrid;
    const int yB = (canvas.getBottom() / kGrid) * kGrid;
    const int usableW = juce::jmax (kGrid, xR - xL);
    const int usableH = juce::jmax (kGrid, yB - yT);

    auto mapX = [&] (float u)
    {
        const int cells = juce::roundToInt ((double) u * (double) usableW / (double) kGrid);
        return juce::jlimit (xL, xR, xL + cells * kGrid);
    };
    auto mapY = [&] (float v)
    {
        const int cells = juce::roundToInt ((double) v * (double) usableH / (double) kGrid);
        return juce::jlimit (yT, yB, yT + cells * kGrid);
    };

    for (int i = 0; i < spec.count; ++i)
    {
        const auto& t = spec.tiles[i];
        auto panel = createModule (t.type);
        if (panel == nullptr) continue;

        if (t.p1 != nullptr) panel->setParamValue (t.p1, t.v1);
        if (t.p2 != nullptr) panel->setParamValue (t.p2, t.v2);
        if (t.p3 != nullptr) panel->setParamValue (t.p3, t.v3);
        if (t.p1 != nullptr || t.p2 != nullptr || t.p3 != nullptr)
            panel->refreshControlPanel();

        const int x0 = mapX (t.x0);
        const int y0 = mapY (t.y0);
        const int w  = juce::jmax (panel->getMinWidth(),  mapX (t.x1) - x0);
        const int h  = juce::jmax (panel->getMinHeight(), mapY (t.y1) - y0);
        panel->setBounds (x0, y0, w, h);
        workspace->addModule (std::move (panel), /*autoPosition*/ false);
    }
}

// ----------------------------------------------------------
// 应用布局预设（由 ModuleWorkspace 的布局下拉框触发）
//   · presetId = 1 (defaultGrid)    → 恢复默认布局 + 默认窗口大小 960×640
//   · presetId = 2 (horizontalFull) → 拉伸顶层窗口到当前屏幕宽，默认模块横向
//                                      等分 canvas，高度撑满 canvas
//   · presetId = 14/15/16           → 全屏磁贴预设（存档布局 + 模块默认设置），
//                                      详见 applyFullScreenTilePreset()
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::applyLayoutPreset (int presetId)
{
    if (workspace == nullptr) return;

    // ★ v2.9.9 补充：伪全屏中切布局预设 → 先退出全屏。
    //   否则下面预设会改写顶层窗口几何，但 pseudoFullScreen / preFullScreenBounds
    //   仍是旧值，带来三个可见问题：
    //     1) 退出程序时 saveMainWindowBounds 落盘的是"上次进全屏前"的尺寸，
    //        用户最后看到的窗口几何被丢弃（重开尺寸不对）；
    //     2) Hide 按钮在 pseudoFullScreen 下不收缩窗口（canResizeWindow=false），
    //        表现为"点了 Hide 没反应"；
    //     3) 双击抬头会跳回很早以前的 pre 尺寸。
    //   与 enterStickMode 的处理保持一致（那里也是先退全屏再备份几何）。
    if (pseudoFullScreen)
        toggleFakeFullScreen();

    // bug4：macOS 原生全屏 → 切到"非全屏磁贴预设"时必须先退出全屏。
    //   否则全屏磁贴预设锁死的 resizeLimits（min=max=整屏尺寸）会把后面
    //   setSize(960×640) / 横条高度全部夹回去，窗口保持全屏、只有模块被换掉。
    //   退出动画是异步的：轮询到真正离开全屏后再重新进入本函数。
   #if JUCE_MAC
    if (! isFullScreenTilePreset (presetId)
        && juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* rw = dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent()))
        {
            if (rw->isFullScreen())
            {
                rw->setFullScreen (false);

                juce::Component::SafePointer<Y2KmeterAudioProcessorEditor> self (this);
                juce::Timer::callAfterDelay (80, [self, presetId] { continueLayoutPresetAfterFullScreenExit (self, presetId, 40); });
                return;
            }
        }
    }
   #endif

    // 切换预设前统一放开尺寸限制：全屏磁贴预设会把 Editor / 顶层窗口的 limits
    //   锁死到整屏尺寸，不解开会把非全屏预设的 setSize 夹回去（bug4 的另一半）。
    //   各预设分支随后会按自己的目标尺寸重新设置 limits。
    {
        if (auto* ec = getConstrainer())
            setResizeLimits (juce::jmax (1, ec->getMinimumWidth()),
                             juce::jmax (1, ec->getMinimumHeight()),
                             juce::jmax (ec->getMaximumWidth(),  8000),
                             juce::jmax (ec->getMaximumHeight(), 8000));

        if (auto* top = getTopLevelComponent())
            if (auto* rw = dynamic_cast<juce::ResizableWindow*> (top))
                if (auto* wc = rw->getConstrainer())
                    rw->setResizeLimits (juce::jmax (1, wc->getMinimumWidth()),
                                         juce::jmax (1, wc->getMinimumHeight()),
                                         32768, 32768);
    }

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

    // 统一归一化 8px 网格磁贴铺排器（相邻模块共享同一网格边，零缝隙、零重叠）
    auto placeNormalizedTiles = [this] (juce::Rectangle<int> canvas, const PresetTile* tiles, int tileCount)
    {
        if (workspace == nullptr || tiles == nullptr || tileCount <= 0)
            return;

        constexpr int kGrid = 8;
        const int xL = ((canvas.getX() + kGrid - 1) / kGrid) * kGrid;
        const int xR = (canvas.getRight()  / kGrid) * kGrid;
        const int yT = ((canvas.getY() + kGrid - 1) / kGrid) * kGrid;
        const int yB = (canvas.getBottom() / kGrid) * kGrid;
        const int usableW = juce::jmax (kGrid, xR - xL);
        const int usableH = juce::jmax (kGrid, yB - yT);

        auto mapX = [&] (float u)
        {
            const int cells = juce::roundToInt ((double) u * (double) usableW / (double) kGrid);
            return juce::jlimit (xL, xR, xL + cells * kGrid);
        };
        auto mapY = [&] (float v)
        {
            const int cells = juce::roundToInt ((double) v * (double) usableH / (double) kGrid);
            return juce::jlimit (yT, yB, yT + cells * kGrid);
        };

        for (int i = 0; i < tileCount; ++i)
        {
            const auto& t = tiles[i];
            auto panel = createModule (t.type);
            if (panel == nullptr) continue;

            if (t.p1 != nullptr) panel->setParamValue (t.p1, t.v1);
            if (t.p2 != nullptr) panel->setParamValue (t.p2, t.v2);
            if (t.p3 != nullptr) panel->setParamValue (t.p3, t.v3);
            if (t.p1 != nullptr || t.p2 != nullptr || t.p3 != nullptr)
                panel->refreshControlPanel();

            const int x0 = mapX (t.x0);
            const int y0 = mapY (t.y0);
            const int x1 = mapX (t.x1);
            const int y1 = mapY (t.y1);
            const int maxW = juce::jmax (kGrid, canvas.getRight()  - x0);
            const int maxH = juce::jmax (kGrid, canvas.getBottom() - y0);
            const int w  = juce::jmin (maxW, juce::jmax (panel->getMinWidth(),  x1 - x0));
            const int h  = juce::jmin (maxH, juce::jmax (panel->getMinHeight(), y1 - y0));
            panel->setBounds (x0, y0, w, h);
            workspace->addModule (std::move (panel), /*autoPosition*/ false);
        }
    };

    if (isHorizontalStripPreset (presetId))
    {
        // 横向任务条 / Stick 吸附条预设：铺满屏幕宽度、固定目标高 250px（对齐 8px 网格）
        bool bottomAligned = (presetId == (int) ModuleWorkspace::LayoutPreset::horizontalBottom);
       #if JUCE_WINDOWS
        if (stickActive)
            bottomAligned = stickBottomAligned;
       #endif

        auto* top = getTopLevelComponent();
        if (top == nullptr) top = this;

       #if JUCE_WINDOWS
        Y2KMonitorPx monPx;
        const bool usePxTarget = (top != this)
                                 && ! y2kScreenSimulationActive()
                                 && y2kGetMonitorPxForWindow (top, monPx)
                                 && ! monPx.work.isEmpty();
       #endif

        const auto display = juce::Desktop::getInstance()
                                 .getDisplays()
                                 .getDisplayForPoint (top->getScreenBounds().getCentre());
        const auto userArea = resolveDisplayArea ((display != nullptr) ? display->userArea
                                                                       : juce::Rectangle<int> (1280, 720));
       #if JUCE_WINDOWS
        const int screenW = usePxTarget ? y2kPxToLogical (monPx.work.getWidth(), monPx.scale)
                                        : userArea.getWidth();
       #else
        const int screenW = userArea.getWidth();
       #endif

        constexpr int kHorizontalStripHeight = 250;
        int targetH = kHorizontalStripHeight;

        if (auto* cbc0 = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc0->getMinimumWidth(),  screenW),
                             juce::jmin (cbc0->getMinimumHeight(), kHorizontalStripHeight),
                             juce::jmax (cbc0->getMaximumWidth(),  screenW),
                             juce::jmax (cbc0->getMaximumHeight(), 1200));
        }

        if (top == this)
        {
            setSize (screenW, targetH);
        }
        else
        {
           #if JUCE_WINDOWS
            if (usePxTarget)
                y2kMoveWindowToRectPx (top, { monPx.work.getX(), monPx.work.getY(),
                                              monPx.work.getWidth(),
                                              y2kLogicalToPx (targetH, monPx.scale) });
            else
           #endif
            top->setBounds (userArea.getX(), userArea.getY(), screenW, targetH);
        }

        const int probeContainerH = (top == this) ? getHeight() : top->getHeight();
        const int probeCanvasH    = workspace->getCanvasArea().getHeight();
        const int overheadH       = probeContainerH - probeCanvasH;

        constexpr int kGridForH = 8;
        const int desiredCanvasRaw = kHorizontalStripHeight - overheadH;
        const int desiredCanvasH   = juce::jmax (kGridForH,
                                                 ((desiredCanvasRaw + kGridForH / 2) / kGridForH) * kGridForH);
        targetH = desiredCanvasH + overheadH;

        if (auto* cbc = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  screenW),
                             juce::jmin (cbc->getMinimumHeight(), targetH),
                             juce::jmax (cbc->getMaximumWidth(),  screenW),
                             juce::jmax (cbc->getMaximumHeight(), targetH));
        }

        if (top == this)
        {
            setSize (screenW, targetH);
        }
        else
        {
           #if JUCE_WINDOWS
            if (usePxTarget)
            {
                const int hPx = y2kLogicalToPx (targetH, monPx.scale);
                const int yPx = bottomAligned ? (monPx.work.getBottom() - hPx)
                                              :  monPx.work.getY();
                y2kMoveWindowToRectPx (top, { monPx.work.getX(), yPx, monPx.work.getWidth(), hPx });
            }
            else
           #endif
            {
                const int targetY = bottomAligned ? (userArea.getBottom() - targetH)
                                                  :  userArea.getY();
                top->setBounds (userArea.getX(), targetY, screenW, targetH);
            }
        }

        const auto spec = resolvePresetSpec (presetId);
        placeNormalizedTiles (workspace->getCanvasArea(), spec.tiles, spec.count);

        if (stickActive)
            workspace->setStickPresetLabel (stripPresetLabel (presetId));
    }
    else if (isFullScreenTilePreset (presetId))
    {
        applyFullScreenTilePreset (presetId);
    }
    else
    {
        // 所有窗口化预设（Default Workspace / Compact Mini Bar / Desktop Companion /
        //   Mixing Console / Mastering & Broadcast / Vocal & Podcast / DJ & Beat / Stream Overlay）
        const auto spec = resolvePresetSpec (presetId);
        const int tgtW = (spec.targetW > 0) ? spec.targetW : 1120;
        const int tgtH = (spec.targetH > 0) ? spec.targetH : 680;
        const auto canvas = resizeToTargetCanvas (tgtW, tgtH);
        placeNormalizedTiles (canvas, spec.tiles, spec.count);
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
// 全屏磁贴预设（14/15/16）
//   · 窗口几何与 Preset 10 (Only Milkdrop) 一致：Windows 铺满 userArea
//     （物理口径 rcWork，兼容混合 DPI），macOS 铺满 totalArea 后进入原生
//     全屏；resizeLimits 锁死到全屏尺寸。
//   · 模块按静态表的参考画布（2560×1328）归一化映射到实际 canvas，边界
//     统一吸附到 8px 网格 → 相邻模块共享同一条边界，密排无缝隙。
//   · 只调用 createModule()：每个模块保持自己独立的默认设置，不套用存档
//     里的颜色 / 缩放 / Milkdrop 预设等任何内部状态。
//     唯一例外：静态表里 tile.mode >= 0 的磁贴会显式套用显示模式
//     （波形 / X-Y / 李萨如、左 / 右 / 双声道、PAN / MONO、Classic / Sharp），
//     因为同一预设里同类模块原本就用了不同模式，丢掉就会几块画面重复。
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::applyFullScreenTilePreset (int presetId)
{
    if (workspace == nullptr) return;

    const auto spec = resolvePresetSpec (presetId);
    if (spec.tiles == nullptr || spec.count <= 0) return;

    auto* top = getTopLevelComponent();
    if (top == nullptr) top = this;

   #if JUCE_WINDOWS
    Y2KMonitorPx monPx;
    const bool usePxTarget = (top != this)
                             && ! y2kScreenSimulationActive()
                             && y2kGetMonitorPxForWindow (top, monPx)
                             && ! monPx.work.isEmpty();
   #endif

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
   #if JUCE_WINDOWS
    const int screenW = usePxTarget ? y2kPxToLogical (monPx.work.getWidth(),  monPx.scale)  : area.getWidth();
    const int screenH = usePxTarget ? y2kPxToLogical (monPx.work.getHeight(), monPx.scale) : area.getHeight();
   #else
    const int screenW = area.getWidth();
    const int screenH = area.getHeight();
   #endif

    if (auto* cbc0 = getConstrainer())
        setResizeLimits (640, 420,
                         juce::jmax (cbc0->getMaximumWidth(),  screenW),
                         juce::jmax (cbc0->getMaximumHeight(), screenH));

    if (top == this)
        setSize (screenW, screenH);
    else
    {
       #if JUCE_WINDOWS
        if (usePxTarget)
            y2kMoveWindowToRectPx (top, monPx.work);
        else
       #endif
        top->setBounds (area.getX(), area.getY(), screenW, screenH);
    }

    if (auto* cbc = getConstrainer())
    {
        const int lockW = (top == this) ? screenW : juce::jmax (640, getWidth());
        const int lockH = (top == this) ? screenH : juce::jmax (420, getHeight());
        setResizeLimits (lockW, lockH, lockW, lockH);
    }

    const auto canvas = workspace->getCanvasArea();
    constexpr int kGrid = 8;

    const int xL = ((canvas.getX() + kGrid - 1) / kGrid) * kGrid;
    const int xR = (canvas.getRight()  / kGrid) * kGrid;
    const int yT = ((canvas.getY() + kGrid - 1) / kGrid) * kGrid;
    const int yB = (canvas.getBottom() / kGrid) * kGrid;
    const int usableW = juce::jmax (kGrid, xR - xL);
    const int usableH = juce::jmax (kGrid, yB - yT);

    auto mapX = [&] (float u)
    {
        const int cells = juce::roundToInt ((double) u * (double) usableW / (double) kGrid);
        return juce::jlimit (xL, xR, xL + cells * kGrid);
    };
    auto mapY = [&] (float v)
    {
        const int cells = juce::roundToInt ((double) v * (double) usableH / (double) kGrid);
        return juce::jlimit (yT, yB, yT + cells * kGrid);
    };

    for (int i = 0; i < spec.count; ++i)
    {
        const auto& t = spec.tiles[i];
        auto panel = createModule (t.type);
        if (panel == nullptr) continue;

        if (t.p1 != nullptr) panel->setParamValue (t.p1, t.v1);
        if (t.p2 != nullptr) panel->setParamValue (t.p2, t.v2);
        if (t.p3 != nullptr) panel->setParamValue (t.p3, t.v3);
        if (t.p1 != nullptr || t.p2 != nullptr || t.p3 != nullptr)
            panel->refreshControlPanel();

        const int x0 = mapX (t.x0);
        const int y0 = mapY (t.y0);
        const int x1 = mapX (t.x1);
        const int y1 = mapY (t.y1);
        const int maxW = juce::jmax (kGrid, canvas.getRight()  - x0);
        const int maxH = juce::jmax (kGrid, canvas.getBottom() - y0);
        const int w  = juce::jmin (maxW, juce::jmax (panel->getMinWidth(),  x1 - x0));
        const int h  = juce::jmin (maxH, juce::jmax (panel->getMinHeight(), y1 - y0));
        panel->setBounds (x0, y0, w, h);
        workspace->addModule (std::move (panel), false);
    }

   #if JUCE_MAC
    if (juce::JUCEApplicationBase::isStandaloneApp())
        if (auto* rw = dynamic_cast<juce::ResizableWindow*> (top))
            rw->setFullScreen (true);
   #endif
}

// bug4：等待 macOS 退出原生全屏（异步动画）后重新应用预设。
//   triesLeft × 80ms ≈ 最长 3.2s 的等待，覆盖系统动画 / 慢机器。
void Y2KmeterAudioProcessorEditor::continueLayoutPresetAfterFullScreenExit (
    juce::Component::SafePointer<Y2KmeterAudioProcessorEditor> self,
    int presetId, int triesLeft)
{
    if (self == nullptr)
        return;

    if (auto* rw = dynamic_cast<juce::ResizableWindow*> (self->getTopLevelComponent()))
        if (rw->isFullScreen() && triesLeft > 0)
        {
            juce::Timer::callAfterDelay (80, [self, presetId, triesLeft]
            {
                continueLayoutPresetAfterFullScreenExit (self, presetId, triesLeft - 1);
            });
            return;
        }

    self->applyLayoutPreset (presetId);
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
    // v3.1：退役类型（lufs_realtime / true_peak / oscilloscope_wave / phase_correlation /
    //   phase_balance / dynamics_* ）统一在这里换成存活类型，并把"旧外观种子"喂给新模块。
    //   旧存档、旧布局预设、全屏 tile 表都经过这个唯一漏斗，所以无需逐个改写。
    if (isRetiredModuleType (type))
    {
        juce::ValueTree seed;
        const auto survivor = resolveRetiredModuleType (type, seed);
        auto panel = createModule (survivor);
        if (panel != nullptr && seed.isValid())
            panel->restoreModuleSpecificState (seed);
        return panel;
    }

    auto wireGlRequester = [this] (std::unique_ptr<ModulePanel> panel) -> std::unique_ptr<ModulePanel>
    {
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (panel.get()))
        {
            juce::Component::SafePointer<Y2KmeterAudioProcessorEditor> safeEd (this);
            gmod->setGlFrameRequester ([safeEd]()
            {
                if (auto* ed = safeEd.getComponent())
                    ed->RequestGlFrame();
            });
           #if JUCE_WINDOWS
            gmod->setGlHostAvailable (! GlDirectRenderer::isGloballyDisabled());
           #else
            gmod->setGlHostAvailable (! PinkXP::isTransparentBackdrop()
                                      && ! GlDirectRenderer::isGloballyDisabled());
           #endif
        }
        return panel;
    };

    switch (type)
    {
        case ModuleType::loudness:
            return std::make_unique<LoudnessModule>(processor.getAnalyserHub());
        case ModuleType::oscilloscope:
            return wireGlRequester (std::make_unique<OscilloscopeModule>(processor.getAnalyserHub()));
        case ModuleType::spectrum:
            return wireGlRequester (std::make_unique<SpectrumModule>(processor.getAnalyserHub()));
        case ModuleType::phase:
            return wireGlRequester (std::make_unique<PhaseModule>(processor.getAnalyserHub()));
        case ModuleType::dynamics:
            return std::make_unique<DynamicsModule>(processor.getAnalyserHub());

        case ModuleType::waveform:
            return wireGlRequester (std::make_unique<WaveformModule>(processor.getAnalyserHub()));

        case ModuleType::vuMeter:
            return std::make_unique<VuMeterModule>(processor.getAnalyserHub());

        case ModuleType::spectrogram:
            return wireGlRequester (std::make_unique<SpectrogramModule>(processor.getAnalyserHub()));

        case ModuleType::spectrogram3d:
            return wireGlRequester (std::make_unique<Spectrogram3DModule>(processor.getAnalyserHub()));

        case ModuleType::stereoField:
            return wireGlRequester (std::make_unique<StereoFieldModule>(processor.getAnalyserHub()));

        case ModuleType::stereoMap:
            return wireGlRequester (std::make_unique<StereoMapModule>(processor.getAnalyserHub()));

        case ModuleType::virtuPet:
            return std::make_unique<VirtuPetModule>();

        case ModuleType::milkdrop:
            milkdrop_has_module_in_editor_.store (true, std::memory_order_release);
            if (! milkdrop_presets_ready_.load (std::memory_order_acquire))
                startMilkdropPresetScanAsync (milkdrop_use_like_library_.load (std::memory_order_acquire));
            RequestGlFrame();
            return std::make_unique<MilkdropModule>(&processor.getAnalyserHub(), this);

        case ModuleType::tempoKey:
            return std::make_unique<TempoKeyModule>(processor.getAnalyserHub());

        case ModuleType::recorder:
            return std::make_unique<RecorderModule>(processor.getAnalyserHub());

        case ModuleType::systemMonitor:
            return std::make_unique<SystemMonitorModule>();

        case ModuleType::utility:
            return std::make_unique<UtilityModule>();

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
    if (bounds.isEmpty() || PinkXP::isTransparentBackdrop())
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
                                     false,
                                     juce::SoftwareImageType{});
    desktopCacheBounds = bounds;
    desktopCacheDirty = false;

    juce::Graphics cacheGraphics (desktopCacheImage);
    cacheGraphics.drawImageAt (sharedBase, 0, 0);
    // drawLogo 期望 area 是"目标绘制矩形"，而 cache Image 以 (0,0) 起点，
    // 因此传递一个左上角为 (0,0) 的等尺寸矩形。
    //   绿幕风格：桌面只留纯绿，不叠 logo
    if (! PinkXP::isChromaKeySkin())
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
    //    透明覆层模式（BackdropMode::transparent）跳过一切不透明背景，保持纯 alpha=0
    if (workspace != nullptr && ! PinkXP::isTransparentBackdrop())
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
                if (! PinkXP::isChromaKeySkin())
                    PinkXP::drawLogo (g, wsBounds, logoImage);
            }
        }
    }

    // 2) 顶部 TitleBar + 右上角按钮组
    //    · 透明模式：未悬停显露 Editor chrome 时完全跳过（零不透明像素、零绿幕填色）；
    //      悬停显露时按当前视觉风格（SkinId）完整绘制标题栏。
    //    · 绿幕模式：未悬停显露 Editor chrome 时整条标题栏填纯绿 #00FF00；
    //      悬停显露时按当前视觉风格（SkinId）完整绘制标题栏。
    if (! chromeDim)
    {
        if (PinkXP::isTransparentBackdrop())
        {
            if (PinkXP::isChromaRevealed (*this))
                drawTitleBar (g);
        }
        else if (PinkXP::isGreenScreenBackdrop() && ! PinkXP::isChromaRevealed (*this))
        {
            const auto tb = getTitleBarBounds();
            g.setColour (PinkXP::chromaKeyGreen());
            g.fillRect (tb.withHeight (tb.getHeight() + 1));
        }
        else
        {
            drawTitleBar (g);
        }
    }
    // chrome 隐藏态（未悬停）的浮动关闭按钮 + 抬头文字由 ChromeHiddenOverlay 处理
}

void Y2KmeterAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    // v2.8.2：Hide 悬停浮层 —— 标题栏盖在所有子组件（模块）之上，窗口几何不变
    if (chromeDim && chromeRevealed
        && ! (PinkXP::isTransparentBackdrop() && ! PinkXP::isChromaRevealed (*this)))
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
        //   诊断构建（RelWithDebInfo / 显式开启诊断日志）在版本号右侧追加 " debug"，
        //   让用户和客服一眼确认装的是"能出日志"的诊断包。
        const juce::String nameText    = "Y2Kmeter";
        const juce::String versionBase = "v3.5.0";
        const juce::String versionTag  = y2kIsDiagnosticBuild() ? " debug" : juce::String();
        const juce::String versionText = versionBase + versionTag;
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW        = nameFont.getStringWidth (y2k::tr (nameText));
        const int versionBaseW = versionFont.getStringWidth (y2k::tr (versionBase));
        const int versionW     = versionFont.getStringWidth (y2k::tr (versionText));   // 含 " debug"
        const int versionTagW  = versionW - versionBaseW;
        const int urlW         = urlFont.getStringWidth (y2k::tr (urlText));

        constexpr int gap1 = 6;   // name ↔ version 之间
        constexpr int gap2 = 10;  // version ↔ url 之间

        const int y  = textArea.getY();
        const int h  = textArea.getHeight();
        const int x0 = textArea.getX();

        // 跨字体光学居中（见 PinkXP::FontOptics）：三种字号各自按字形带修正；
        //   像素复古的修正量 < 0.5px，四舍五入后为 0 → 旧外观逐像素不变。
        const int nameDy    = juce::roundToInt (PinkXP::opticalNudge (nameFont));
        const int versionDy = juce::roundToInt (PinkXP::opticalNudge (versionFont));
        const int urlDy     = juce::roundToInt (PinkXP::opticalNudge (urlFont));

        // 文字阴影（1,1）
        g.setColour (PinkXP::sel.darker(0.55f));
        g.setFont (nameFont);
        g.drawText (y2k::tr (nameText), x0 + 1, y + nameDy + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (y2k::tr (versionText), x0 + nameW + gap1 + 1, y + versionDy + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (y2k::tr (urlText), x0 + nameW + gap1 + versionW + gap2 + 1, y + urlDy + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（白色）
        g.setColour (PinkXP::selInk);
        g.setFont (nameFont);
        g.drawText (y2k::tr (nameText), x0, y + nameDy, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (y2k::tr (versionBase), x0 + nameW + gap1, y + versionDy, versionBaseW, h, juce::Justification::centredLeft, false);
        if (versionTagW > 0)
        {
            // " debug" 用强调色，方便一眼认出诊断包
            g.setColour (PinkXP::warn);
            g.drawText (y2k::tr (versionTag), x0 + nameW + gap1 + versionBaseW, y + versionDy, versionTagW, h,
                        juce::Justification::centredLeft, false);
            g.setColour (PinkXP::selInk);   // 恢复后续 url 文字颜色
        }
        g.setFont (urlFont);
        g.drawText (y2k::tr (urlText), x0 + nameW + gap1 + versionW + gap2, y + urlDy, urlW, h, juce::Justification::centredLeft, false);

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

        // 标题栏下沿 1px 分割线（与 ModulePanel 视觉一致；霓虹风格下换成灯管亮色）
        g.setColour (PinkXP::titleBarDividerColour());
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

                // 图标字符（L / _ / * / x）：恒用英文 UI 字体，不随语言切换变样。
                //   bug2：drawText 的 centred 是按字体行框居中的，切风格换字体后
                //   （Orbitron / Exo 2 / VT323 / Open Sans ... 的 x-height / descent
                //   比例差别很大）字符会整体上下跑位。改由 drawIconGlyph 按字形
                //   墨迹包围盒居中；像素复古风格内部仍走旧路径 + 旧偏移，外观不变。
                auto txt = rc;
                if (pressed || activeLatched) txt.translate (1, 1);
                PinkXP::SymbolGlyphs::drawIconGlyph (g, glyph, txt, fontHeight, PinkXP::ink,
                                                     glyphDxWhenIdle, glyphDyWhenIdle);
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
            //   高度可调，置顶/最小化语义不再适用。
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

    // Toybox 叠加层：缩放窗口后同步 bounds，否则新增区域内 emoji 不显示（FIX-4 / B-04）
    if (toyDirector != nullptr && toyDirector->overlay != nullptr &&
        toyDirector->overlay->getParentComponent() != nullptr)
        toyDirector->overlay->setBounds (getLocalBounds());

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
    syncTopLevelUndoKeyListener();
}

bool Y2KmeterAudioProcessorEditor::TopLevelUndoKeyListener::keyPressed (
    const juce::KeyPress& key, juce::Component* originatingComponent)
{
    if (dynamic_cast<juce::TextEditor*> (originatingComponent) != nullptr)
        return false;
    if (editor_.workspace != nullptr && editor_.workspace->handleUndoRedoKeyPress (key))
        return true;
    return false;
}

void Y2KmeterAudioProcessorEditor::syncTopLevelUndoKeyListener()
{
    auto* newHost = (! isPluginHost) ? getTopLevelComponent() : nullptr;
    if (newHost == this)
        newHost = nullptr;

    auto* oldHost = topLevelUndoKeyHost_.getComponent();
    if (oldHost == newHost)
        return;

    if (oldHost != nullptr)
        oldHost->removeKeyListener (&topLevelUndoKeyListener_);
    topLevelUndoKeyHost_ = newHost;
    if (newHost != nullptr)
        newHost->addKeyListener (&topLevelUndoKeyListener_);
}

juce::ValueTree Y2KmeterAudioProcessorEditor::captureHostWindowState()
{
    juce::ValueTree s ("HostWindowState");
    s.setProperty ("editorW", getWidth(),  nullptr);
    s.setProperty ("editorH", getHeight(), nullptr);
    s.setProperty ("pseudoFullScreen", pseudoFullScreen, nullptr);
    if (pseudoFullScreen)
    {
        s.setProperty ("preFsX", preFullScreenBounds.getX(), nullptr);
        s.setProperty ("preFsY", preFullScreenBounds.getY(), nullptr);
        s.setProperty ("preFsW", preFullScreenBounds.getWidth(), nullptr);
        s.setProperty ("preFsH", preFullScreenBounds.getHeight(), nullptr);
        s.setProperty ("preFsPxX", preFullScreenRectPx.getX(), nullptr);
        s.setProperty ("preFsPxY", preFullScreenRectPx.getY(), nullptr);
        s.setProperty ("preFsPxW", preFullScreenRectPx.getWidth(), nullptr);
        s.setProperty ("preFsPxH", preFullScreenRectPx.getHeight(), nullptr);
        s.setProperty ("preFsLimitsValid", preFullScreenLimitsValid, nullptr);
        s.setProperty ("preFsMinW", preFullScreenMinW, nullptr);
        s.setProperty ("preFsMinH", preFullScreenMinH, nullptr);
        s.setProperty ("preFsMaxW", preFullScreenMaxW, nullptr);
        s.setProperty ("preFsMaxH", preFullScreenMaxH, nullptr);
        s.setProperty ("preFsEdLimitsValid", preFullScreenEditorLimitsValid, nullptr);
        s.setProperty ("preFsEdMinW", preFullScreenEditorMinW, nullptr);
        s.setProperty ("preFsEdMinH", preFullScreenEditorMinH, nullptr);
        s.setProperty ("preFsEdMaxW", preFullScreenEditorMaxW, nullptr);
        s.setProperty ("preFsEdMaxH", preFullScreenEditorMaxH, nullptr);
       #if JUCE_WINDOWS
        if (! pseudoFullScreenRectPx.isEmpty())
        {
            s.setProperty ("pseudoFsPxX", pseudoFullScreenRectPx.getX(),      nullptr);
            s.setProperty ("pseudoFsPxY", pseudoFullScreenRectPx.getY(),      nullptr);
            s.setProperty ("pseudoFsPxW", pseudoFullScreenRectPx.getWidth(),  nullptr);
            s.setProperty ("pseudoFsPxH", pseudoFullScreenRectPx.getHeight(), nullptr);
        }
       #endif
    }

    if (auto* ec = getConstrainer())
    {
        s.setProperty ("eMinW", ec->getMinimumWidth(),  nullptr);
        s.setProperty ("eMinH", ec->getMinimumHeight(), nullptr);
        s.setProperty ("eMaxW", ec->getMaximumWidth(),  nullptr);
        s.setProperty ("eMaxH", ec->getMaximumHeight(), nullptr);
    }

    auto* top = getTopLevelComponent();
    if (top != nullptr && top != this)
    {
        const auto tb = top->getBounds();
        s.setProperty ("hasTop", true, nullptr);
        s.setProperty ("winX", tb.getX(),      nullptr);
        s.setProperty ("winY", tb.getY(),      nullptr);
        s.setProperty ("winW", tb.getWidth(),  nullptr);
        s.setProperty ("winH", tb.getHeight(), nullptr);

       #if JUCE_WINDOWS
        if (! y2kScreenSimulationActive())
        {
            const auto px = y2kGetWindowRectPx (top);
            if (! px.isEmpty())
            {
                s.setProperty ("winPxX", px.getX(),      nullptr);
                s.setProperty ("winPxY", px.getY(),      nullptr);
                s.setProperty ("winPxW", px.getWidth(),  nullptr);
                s.setProperty ("winPxH", px.getHeight(), nullptr);
            }
        }
       #endif

        if (auto* rw = dynamic_cast<juce::ResizableWindow*> (top))
        {
            if (auto* wc = rw->getConstrainer())
            {
                s.setProperty ("wMinW", wc->getMinimumWidth(),  nullptr);
                s.setProperty ("wMinH", wc->getMinimumHeight(), nullptr);
                s.setProperty ("wMaxW", wc->getMaximumWidth(),  nullptr);
                s.setProperty ("wMaxH", wc->getMaximumHeight(), nullptr);
            }
           #if JUCE_MAC
            s.setProperty ("macFullScreen", rw->isFullScreen(), nullptr);
           #endif
        }
    }

    return s;
}

void Y2KmeterAudioProcessorEditor::restoreHostWindowState (const juce::ValueTree& state)
{
    if (! state.hasType ("HostWindowState"))
        return;

    auto* top = getTopLevelComponent();
    auto* rw  = (top != nullptr && top != this)
                    ? dynamic_cast<juce::ResizableWindow*> (top) : nullptr;

    // 1) 先临时放宽 Editor 与顶层窗口的尺寸限制（例如从锁死全屏的磁贴预设撤回回普通窗口时，
    //    若不先解开 constrainer，setBounds / setSize 会被夹回全屏尺寸）。
    if (getConstrainer() != nullptr)
        setResizeLimits (1, 1, 32768, 32768);
    if (rw != nullptr && rw->getConstrainer() != nullptr)
        rw->setResizeLimits (1, 1, 32768, 32768);

    // 2) 还原伪全屏与 macOS 原生全屏状态
    pseudoFullScreen = (bool) state.getProperty ("pseudoFullScreen", false);
    if (pseudoFullScreen)
    {
        preFullScreenBounds = { (int) state.getProperty ("preFsX", 0),
                                (int) state.getProperty ("preFsY", 0),
                                (int) state.getProperty ("preFsW", 960),
                                (int) state.getProperty ("preFsH", 640) };
        preFullScreenRectPx = { (int) state.getProperty ("preFsPxX", 0),
                                (int) state.getProperty ("preFsPxY", 0),
                                (int) state.getProperty ("preFsPxW", 0),
                                (int) state.getProperty ("preFsPxH", 0) };
        preFullScreenLimitsValid       = (bool) state.getProperty ("preFsLimitsValid", false);
        preFullScreenMinW              = (int) state.getProperty ("preFsMinW", 640);
        preFullScreenMinH              = (int) state.getProperty ("preFsMinH", 420);
        preFullScreenMaxW              = (int) state.getProperty ("preFsMaxW", 1600);
        preFullScreenMaxH              = (int) state.getProperty ("preFsMaxH", 1100);
        preFullScreenEditorLimitsValid = (bool) state.getProperty ("preFsEdLimitsValid", false);
        preFullScreenEditorMinW        = (int) state.getProperty ("preFsEdMinW", 640);
        preFullScreenEditorMinH        = (int) state.getProperty ("preFsEdMinH", 420);
        preFullScreenEditorMaxW        = (int) state.getProperty ("preFsEdMaxW", 1600);
        preFullScreenEditorMaxH        = (int) state.getProperty ("preFsEdMaxH", 1100);
       #if JUCE_WINDOWS
        pseudoFullScreenRectPx = { (int) state.getProperty ("pseudoFsPxX", 0),
                                   (int) state.getProperty ("pseudoFsPxY", 0),
                                   (int) state.getProperty ("pseudoFsPxW", 0),
                                   (int) state.getProperty ("pseudoFsPxH", 0) };
       #endif
    }
    else
    {
        preFullScreenBounds = {};
        preFullScreenRectPx = {};
        pseudoFullScreenRectPx = {};
        preFullScreenLimitsValid = false;
        preFullScreenEditorLimitsValid = false;
    }

   #if JUCE_MAC
    if (rw != nullptr && state.hasProperty ("macFullScreen"))
    {
        const bool wantFs = (bool) state.getProperty ("macFullScreen", false);
        if (rw->isFullScreen() != wantFs)
            rw->setFullScreen (wantFs);
    }
   #endif

    // 3) 恢复窗口几何（Standalone 恢复顶层窗口位置与尺寸；插件宿主/无外框恢复 Editor 尺寸）
    const int targetEdW = juce::jmax (120, (int) state.getProperty ("editorW", getWidth()));
    const int targetEdH = juce::jmax (80,  (int) state.getProperty ("editorH", getHeight()));

    if (top != nullptr && top != this && (bool) state.getProperty ("hasTop", false))
    {
        const juce::Rectangle<int> winRect {
            (int) state.getProperty ("winX", top->getX()),
            (int) state.getProperty ("winY", top->getY()),
            juce::jmax (120, (int) state.getProperty ("winW", top->getWidth())),
            juce::jmax (80,  (int) state.getProperty ("winH", top->getHeight()))
        };

       #if JUCE_WINDOWS
        bool appliedPx = false;
        if (! y2kScreenSimulationActive() && state.hasProperty ("winPxW") && state.hasProperty ("winPxH"))
        {
            const juce::Rectangle<int> pxRect {
                (int) state.getProperty ("winPxX", 0),
                (int) state.getProperty ("winPxY", 0),
                (int) state.getProperty ("winPxW", 0),
                (int) state.getProperty ("winPxH", 0)
            };
            if (pxRect.getWidth() > 0 && pxRect.getHeight() > 0 && y2kIsRectOnAnyMonitorPx (pxRect))
                appliedPx = y2kMoveWindowToRectPx (top, pxRect);
        }
        if (! appliedPx)
        {
            top->setBounds (winRect);
            if (getWidth() != targetEdW || getHeight() != targetEdH)
                setSize (targetEdW, targetEdH);
        }
       #else
        top->setBounds (winRect);
        if (getWidth() != targetEdW || getHeight() != targetEdH)
            setSize (targetEdW, targetEdH);
       #endif
    }
    else
    {
        setSize (targetEdW, targetEdH);
    }

    // 4) 恢复保存的 constrainer 尺寸约束（确保不会把刚刚恢复好的当前尺寸夹掉）
    if (state.hasProperty ("eMinW") && state.hasProperty ("eMaxW"))
    {
        const int minW = juce::jmin (getWidth(),  juce::jmax (1, (int) state.getProperty ("eMinW", 640)));
        const int minH = juce::jmin (getHeight(), juce::jmax (1, (int) state.getProperty ("eMinH", 420)));
        const int maxW = juce::jmax (getWidth(),  juce::jmax (minW, (int) state.getProperty ("eMaxW", 1600)));
        const int maxH = juce::jmax (getHeight(), juce::jmax (minH, (int) state.getProperty ("eMaxH", 1100)));
        setResizeLimits (minW, minH, maxW, maxH);
    }
    if (rw != nullptr && state.hasProperty ("wMinW") && state.hasProperty ("wMaxW"))
    {
        const int minW = juce::jmin (rw->getWidth(),  juce::jmax (1, (int) state.getProperty ("wMinW", 640)));
        const int minH = juce::jmin (rw->getHeight(), juce::jmax (1, (int) state.getProperty ("wMinH", 420)));
        const int maxW = juce::jmax (rw->getWidth(),  juce::jmax (minW, (int) state.getProperty ("wMaxW", 32768)));
        const int maxH = juce::jmax (rw->getHeight(), juce::jmax (minH, (int) state.getProperty ("wMaxH", 32768)));
        rw->setResizeLimits (minW, minH, maxW, maxH);
    }

    // 5) 立即同步布局 workspace canvas，确保随后的 restoreSnapshotInPlace 拿到准确画布尺寸
    resized();
    if (workspace != nullptr)
        workspace->resized();

   #if JUCE_WINDOWS
    if (stickActive)
        redockStickAfterGeometryChange();
   #endif
    if (alwaysOnTopActive)
        setAlwaysOnTopActive (true);
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

   #if JUCE_WINDOWS
    // 混合 DPI 多屏：用 Win32 的 rcMonitor 物理尺寸 ÷ 该屏 scale 得到"逻辑分辨率"，
    //   作为档位判据。不用 Displays::getDisplayForRect：
    //     · 该函数是"交集面积最大"，窗口中心越过中缝时判定会翻转；
    //     · 上下排布 + 缩放不同时 JUCE 的逻辑矩形本身就被 updateToLogical()
    //       重排过（可能互相重叠），面积判据在这里没有意义。
    //   改为"窗口中心点所在显示器"，点判定是确定的，不会在两块屏之间来回跳。
    if (auto* top = getTopLevelComponent())
    {
        if (const HWND hwnd = y2kGetWindowHwnd (top))
        {
            RECT wr = {};
            if (GetWindowRect (hwnd, &wr) != FALSE)
            {
                const juce::Point<int> centre { (int) (wr.left + wr.right) / 2,
                                                (int) (wr.top + wr.bottom) / 2 };

                Y2KMonitorPx mon;
                if (y2kGetMonitorPxForPoint (centre, mon) && ! mon.total.isEmpty())
                {
                    const double s = mon.scale > 0.1 ? mon.scale : 1.0;
                    displayArea = { 0, 0,
                                    juce::roundToInt ((double) mon.total.getWidth()  / s),
                                    juce::roundToInt ((double) mon.total.getHeight() / s) };
                }
            }
        }
    }
   #endif

    if (displayArea.isEmpty())
    {
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

// ==========================================================
// 显示器拓扑兜底（Windows）
//   现场演出 / 副屏全屏场景常见：投影仪热拔、分辨率变化、任务栏自动隐藏、
//   主副屏互换。JUCE 没有订阅 Displays::Listener，这里用 10Hz 轮询兜底：
//     · 窗口已完全不在任何显示器上 → 拉回最近的显示器工作区（防丢窗）；
//     · 伪全屏的目标工作区变了（rcWork 变化 / 换了显示器）→ 重新贴合。
//   全部物理像素比较，不依赖 JUCE 的逻辑显示器表。
// ==========================================================
void Y2KmeterAudioProcessorEditor::ensureWindowOnScreen()
{
   #if JUCE_WINDOWS
    if (isPluginHost || stickActive || ! isShowing())
        return;

    auto* top = getTopLevelComponent();
    if (top == nullptr || top == this)
        return;

    // 最小化时 GetWindowRect 返回 -32000 之类的哨兵值，绝不能在此时搬窗口
    if (auto* rw = dynamic_cast<juce::ResizableWindow*> (top))
        if (rw->isMinimised())
            return;

    const auto winPx = y2kGetWindowRectPx (top);
    if (winPx.isEmpty())
        return;

    // 1) 完全离开所有显示器（拔屏 / 改分辨率 / 最小化后恢复异常）
    if (! y2kIsRectOnAnyMonitorPx (winPx))
    {
        // 注意：变量名不要用 near / far —— MSVC 保留这两个 16 位时代的关键字，
        //   用作标识符会直接报 C2059 语法错误。
        Y2KMonitorPx targetMon;
        const juce::Point<int> probe = winPx.getCentre();

        if (! y2kGetMonitorPxForPoint (probe, targetMon) || targetMon.work.isEmpty())
            return;

        y2kMoveWindowToRectPx (top, { targetMon.work.getX(), targetMon.work.getY(),
                                      juce::jmin (winPx.getWidth(),  targetMon.work.getWidth()),
                                      juce::jmin (winPx.getHeight(), targetMon.work.getHeight()) });
        return;
    }

    // 2) 伪全屏：只在"目标屏工作区与自己提交的矩形不一致"时重新贴合，
    //    避免与 WM_DPICHANGED 或用户手动调整互相拉扯。
    if (pseudoFullScreen && ! pseudoFullScreenRectPx.isEmpty())
    {
        Y2KMonitorPx mon;
        if (y2kGetMonitorPxForWindow (top, mon) && ! mon.work.isEmpty()
            && mon.work != pseudoFullScreenRectPx)
        {
            pseudoFullScreenRectPx = mon.work;
            y2kMoveWindowToRectPx (top, mon.work);
        }
    }
   #endif
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

void Y2KmeterAudioProcessorEditor::syncEditorChromeAlpha()
{
    const bool hideChrome = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);
    if (chromeHiddenOverlay != nullptr)
        chromeHiddenOverlay->setAlpha (hideChrome ? 0.0f : 1.0f);
}

void Y2KmeterAudioProcessorEditor::setOpenGlEnabledForBackdrop (bool enabled)
{
    if (editorBeingDestructed)
        return;

    auto syncGlHostFlags = [enabled] (auto& self, juce::Component* root) -> void
    {
        if (root == nullptr)
            return;
        for (int i = 0; i < root->getNumChildComponents(); ++i)
        {
            auto* ch = root->getChildComponent (i);
            if (ch == nullptr)
                continue;
            if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (ch))
            {
                gmod->setGlHostAvailable (enabled);
                if (! enabled)
                {
                    if (gmod->glDirectActive.exchange (false, std::memory_order_acq_rel))
                        gmod->forceRepaintImmediate();
                    gmod->glTexUploadedGen.store (0, std::memory_order_release);
                }
                else
                {
                    gmod->forceRebuildGlSnapshot();
                }
            }
            else
            {
                self (self, ch);
            }
        }
    };

    for (auto& wnd : floatingWindows_)
        if (wnd != nullptr)
            wnd->setOpenGlEnabledForBackdrop (enabled);

    if (! enabled)
    {
        if (openGLContext.isAttached())
        {
            Y2K_DIAGC ("GL", "openGLContext.detach (BackdropMode::transparent)");
            openGLContext.detach();
        }
        y2k::diag::setGlAttached (false);
        if (toyDirector != nullptr)
            toyDirector->setGlToyHostActive (false);

        syncGlHostFlags (syncGlHostFlags, this);
        invalidateDesktopCache();
        repaint();
        return;
    }

    if (PinkXP::isTransparentBackdrop() || openGLContext.isAttached())
        return;

    Y2K_DIAGC ("GL", "openGLContext.attachTo (leaving BackdropMode::transparent)");
    openGLContext.attachTo (*this);
    y2k::diag::setGlAttached (openGLContext.isAttached());
    if (toyDirector != nullptr)
        toyDirector->setGlToyHostActive (openGLContext.isAttached());
    syncGlHostFlags (syncGlHostFlags, this);
    invalidateDesktopCache();
    repaint();
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

   #if JUCE_WINDOWS
    // Windows：条宽 / 条位一律按物理工作区算（AppBar 占位本来就是物理 px），
    //   避免混合 DPI 下逻辑 userArea 原点错位把条摆到另一块屏上。
    if (top != this && ! y2kScreenSimulationActive())
    {
        Y2KMonitorPx monPx;
        if (y2kGetMonitorPxForWindow (top, monPx) && ! monPx.work.isEmpty())
        {
            const double s = monPx.scale;
            const int wPx  = monPx.work.getWidth();
            const int wLog = y2kPxToLogical (wPx, s);

            const int windowHLogical = juce::jmax (120, targetEditorHeight + frameH);
            const int hPx            = y2kLogicalToPx (windowHLogical, s);

            if (auto* cbc = getConstrainer())
            {
                setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  wLog), 120,
                                 juce::jmax (cbc->getMaximumWidth(),  wLog),
                                 juce::jmax (cbc->getMaximumHeight(), windowHLogical));
            }

            const int yPx = bottom ? (monPx.work.getBottom() - hPx) : monPx.work.getY();
            y2kMoveWindowToRectPx (top, { monPx.work.getX(), yPx, wPx, hPx });
            return;
        }
    }
   #endif

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

    // 伪全屏中进入 stick：先退出伪全屏，还原成普通窗口再备份。
    //   否则备份的窗口 bounds 是"全屏矩形"，退出 stick 后会得到一个铺满整块屏、
    //   却不处于全屏态的窗口（VJ 现场切条状模式后经常遇到）。
    if (pseudoFullScreen)
        toggleFakeFullScreen();

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
    workspace->resetUndoHistory();
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
    {
        workspace->setStickActive (false);
        workspace->resetUndoHistory();
    }
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
    {
        workspace->setStickActive (true, stickBottomAligned);
        workspace->resetUndoHistory();
    }

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
    // v3.0.1：插件宿主模式一律视为"未锁定"。
    //   插件模式不绘制也不命中 L 按钮（见 getLockButtonBounds / drawTitleBar），
    //   任何来源的锁定（历史 settings、工程状态、布局预设里的 layoutLocked）
    //   都会变成无法解锁的死锁，因此在这里做最后一道闸门。
    if (isPluginHost)
        locked = false;

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
           #if JUCE_WINDOWS
            // Windows：用物理矩形与物理工作区比较。getDisplayForRect 在混合 DPI
            //   的上下双屏上会选错屏（逻辑矩形被算偏），Hide 时会往错的方向收。
            bool decided = false;
            if (! y2kScreenSimulationActive())
            {
                const auto winPx = y2kGetWindowRectPx (top);
                Y2KMonitorPx monPx;
                if (! winPx.isEmpty()
                    && y2kGetMonitorPxForPoint (winPx.getCentre(), monPx)
                    && ! monPx.work.isEmpty())
                {
                    anchorBottom = winPx.getCentre().y > monPx.work.getCentre().y;
                    decided = true;
                }
            }

            if (! decided)
           #endif
            {
                const auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (b);
                const auto userArea = (d != nullptr) ? d->userArea : b;
                anchorBottom = b.getCentreY() > userArea.getCentreY();
            }
        }

        // 记录进入 Hide 时的锚点，Show 时优先复用，避免中心点跨中线导致几何漂移。
        if (shouldHide)
        {
            hideAnchorBottom = anchorBottom;
            hideAnchorKnown = true;
        }
        else if (hideAnchorKnown)
        {
            anchorBottom = hideAnchorBottom;
        }

        const int newH = shouldHide ? juce::jmax (1, b.getHeight() - chromeH)
                                    : b.getHeight() + chromeH;
        const juce::Rectangle<int> nb (b.getX(),
                                       anchorBottom ? b.getBottom() - newH : b.getY(),
                                       b.getWidth(), newH);

        // 应用几何：Windows 下按物理像素落地（宽度取窗口当前物理宽，高度按 scale 换算），
        //   保证"逻辑高度 × scale"与窗口位置同源 —— 否则混合 DPI 下每次 Hide/Show
        //   都会因为换算原点不同而漂移。
        auto applyWindowRect = [&] (const juce::Rectangle<int>& logicalRect)
        {
           #if JUCE_WINDOWS
            if (top != this && ! y2kScreenSimulationActive())
            {
                const auto winPx = y2kGetWindowRectPx (top);
                Y2KMonitorPx monPx;
                if (! winPx.isEmpty() && y2kGetMonitorPxForPoint (winPx.getCentre(), monPx))
                {
                    const int hPx = juce::jmax (1, y2kLogicalToPx (logicalRect.getHeight(), monPx.scale));
                    const int yPx = anchorBottom ? (winPx.getBottom() - hPx) : winPx.getY();
                    y2kMoveWindowToRectPx (top, { winPx.getX(), yPx, winPx.getWidth(), hPx });
                    return;
                }
            }
           #endif
            top->setBounds (logicalRect);
        };

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
            applyWindowRect (nb);
        }
        else
        {
            chromeDim = false;
            applyWindowRect (nb);
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

void Y2KmeterAudioProcessorEditor::RestoreFftSize (int size)
{
    if (workspace == nullptr) return;

    // 先同步控制台按钮文案
    workspace->setFftSize (size);

    // 再真正作用到后端 FFT（等同用户手动点击按钮）
    if (workspace->onFftSizeChanged)
        workspace->onFftSizeChanged (workspace->getFftSize());
}

int Y2KmeterAudioProcessorEditor::GetUserRequestedFftSize() const noexcept
{
    return (workspace != nullptr) ? workspace->getFftSize() : 2048;
}

// ======================================================
// Toybox 档位（Off 已下线，钳制到 1..3）
// ======================================================
void Y2KmeterAudioProcessorEditor::setToyLevel (int level)
{
    const int l = juce::jlimit ((int) ToyLevel::Chill, (int) ToyLevel::Party, level);
    if (toyDirector) toyDirector->setLevel ((ToyLevel) l);
    if (workspace)   workspace->setToyLevel (l);
    processor.setSavedToyLevel (l);
}

int Y2KmeterAudioProcessorEditor::getToyLevel() const noexcept
{
    return toyDirector ? (int) toyDirector->getLevel() : (int) ToyLevel::Chill;
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
// ==========================================================
// 界面语言切换（y2k::i18n 广播回调，消息线程）
//   控件内部保存的是英文源文案，显示层在绘制时翻译 —— 所以这里不需要逐个
//   setText，只需把"会缓存字体 / 文字位图"的地方全部刷新一遍：
//     1) 换成新语言的字体（Typeface 进程内缓存，切回时零成本）；
//     2) 广播主题刷新：各模块订阅回调本就会失效静态文字层 / GL 快照 / 位图缓存；
//        Editor 自己的回调会失效桌面缓存并刷新所有浮动窗口；
//     3) sendLookAndFeelChange：ComboBox / Slider 数值框等重建内部 Label 并重取字体；
//     4) 工具栏语言按钮文案 + 重排；整窗重绘。
// ==========================================================
void Y2KmeterAudioProcessorEditor::applyLanguageChange()
{
    Y2K_LOG ("[i18n] language -> " + y2k::i18n::getCurrentLanguageCode());

    customTypeface = PinkXP::loadActiveTypeface();
    PinkXP::initCustomTypeface (customTypeface);

    PinkXP::notifyThemeSubscribers();

    sendLookAndFeelChange();
    for (auto& wnd : floatingWindows_)
    {
        if (wnd == nullptr)
            continue;
        // 浮动窗口标题（任务栏 / 窗口标题）在创建时按当时语言生成，这里同步刷新
        wnd->setName (y2k::tr (wnd->getModulePanel().getTitleText()) + " - Y2Kmeter");
        wnd->sendLookAndFeelChange();
        wnd->repaint();
    }

    if (workspace != nullptr)
        workspace->refreshLanguageUi();

    resized();
    repaint();
}

// ==========================================================
// 悬停提示（HoverHelp）：标题栏自绘按钮
//   直接复用 mouseMove 维护的 hover 状态，不重复做几何命中；
//   Y2KTooltipWindow 只在 Editor 本身位于鼠标下（标题栏 / 隐藏态浮层挖洞区）时
//   才会问到这里，其余区域的鼠标都被 workspace / 模块截走。
// ==========================================================
juce::String Y2KmeterAudioProcessorEditor::getTooltip()
{
    using y2k::help::make;

    if (titleTextHovered)
        return make ("Y2Kmeter website",
                     "Click to open the official website for updates, presets and news.");

    if (isPluginHost)
        return {};

    if (closeButtonHovered)
        return make ("Quit", "Close Y2Kmeter. Layout, theme and window position are saved "
                             "automatically and restored next time.");

    if (pinButtonHovered)
        return make ("Always on top",
                     alwaysOnTopActive ? juce::String ("ON - the window floats above other apps. Click to release it.")
                                       : juce::String ("OFF - click to keep the window above other apps."));

    if (minButtonHovered)
        return make ("Minimise", "Send the window to the taskbar / Dock. Audio analysis keeps running.");

    if (lockButtonHovered)
        return make ("Layout lock (L)",
                     layoutLocked
                         ? juce::String ("LOCKED - modules and the window cannot be moved, resized or "
                                         "closed. Buttons inside modules still work. Click to unlock.")
                         : juce::String ("Click to lock the layout so nothing moves by accident while "
                                         "you work. Buttons inside modules keep working."));

    // 标题栏空白区：拖窗 / 双击伪全屏
    if (isTitleBarInteractive() && getTitleBarBounds().contains (getMouseXYRelative()))
        return make ("Title bar",
                     stickActive
                         ? juce::String ("Stick mode: the bar is docked to the screen edge. "
                                         "Use X / T/B in the toolbar to leave or flip it.")
                         : juce::String ("Drag to move the window. Double-click to fill the screen, "
                                         "double-click again to restore."));

    return {};
}

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

// ==========================================================
// 标题文字点击 → 打开官网 + 成就 #7（ACH_ISAAC_DEPT_STORE）
//   三处调用点（插件宿主 / chrome 隐藏态 / 常规态）行为完全一致：
//   先起浏览器、再解锁。即使用户的系统没默认浏览器（launchInDefaultBrowser
//   返回 false），用户意图已经发生，成就照样给。
// ==========================================================
void Y2KmeterAudioProcessorEditor::openWebsiteFromTitleClick()
{
    juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
    y2k::ach::reportWebsiteVisited();
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
            openWebsiteFromTitleClick();
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
            openWebsiteFromTitleClick();
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
            openWebsiteFromTitleClick();
            return;
        }
    }

    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        // 锁定态 / stick 驻留态 / 伪全屏态：不启动顶层窗口拖拽。
        //   stick 驻留态的几何由 AppBar 管理，拖动会与占位脱节；双击全屏也会
        //   破坏贴边矩形，因此 mouseDoubleClick 里同样对 stickActive 早退。
        //   伪全屏态拖动会让"进入前的 bounds"与实际窗口脱节（退出还原时跳回去），
        //   演出场景里也容易误拖把全屏窗口拖走，因此直接禁止；双击仍可退出全屏。
        if (! layoutLocked && ! stickActive && ! pseudoFullScreen)
        {
            if (auto* top = getTopLevelComponent())
            {
               #if JUCE_WINDOWS
                // 混合 DPI 多屏：窗口拖动必须在物理像素上做（见 y2kSnapWindowToMonitorEdgesPx
                // 上方的说明）。ComponentDragger 的目标点 = 逻辑光标 - 按下偏移，而
                // "上下排布 + 缩放不同" 的桌面上逻辑光标坐标在中缝两侧不连续
                // （juce_Displays.cpp updateToLogical 的 on-bottom 分支用
                // physicalX / 父屏 scale 求逻辑 x），光标一过中缝窗口就整体瞬移。
                winDragStartRectPx = y2kGetWindowRectPx (top);
                winDragPxActive    = ! winDragStartRectPx.isEmpty()
                                     && y2kGetCursorPosPx (winDragStartCursorPx);
               #endif

                if (! winDragPxActive)
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
        {
           #if JUCE_WINDOWS
            // 绝对式物理拖动：目标 = 按下时窗口物理位置 + 光标物理位移。
            //   绝对式本身自带纠错 —— 跨 DPI 时 Windows 给的建议矩形（尺寸变化）
            //   不会累积漂移；只写位置不写尺寸，避免与 WM_DPICHANGED 互相覆盖。
            if (winDragPxActive)
            {
                juce::Point<int> cursorPx;
                if (y2kGetCursorPosPx (cursorPx))
                    y2kMoveWindowToPx (top, winDragStartRectPx.getTopLeft()
                                              + (cursorPx - winDragStartCursorPx));

                return;   // 物理路径失败也不回退：windowDragger 本次未启动
            }
           #endif

            windowDragger.dragComponent(top, e.getEventRelativeTo(top), nullptr);
        }
    }
}

void Y2KmeterAudioProcessorEditor::mouseUp(const juce::MouseEvent& e)
{
    // 插件宿主模式：无 chrome 按钮点击处理
    if (isPluginHost) return;

    // 拖窗松手时做四边软吸附 + 防丢窗口：
    //   · Windows：全程物理像素，基准屏 = 光标所在显示器（详见
    //     y2kSnapWindowToMonitorEdgesPx 上方说明）。不再用
    //     getDisplayForRect（交集面积最大）判屏 —— 混合 DPI 的上下双屏上该判据
    //     会在窗口中心越过中缝时整体翻转，吸附基准线跳到另一块屏。
    //   · 其它平台：沿用 JUCE 逻辑坐标路径。
    if (draggingWindow)
    {
        if (auto* top = getTopLevelComponent())
        {
           #if JUCE_WINDOWS
            if (winDragPxActive)
            {
                winDragPxActive = false;

                if (! stickActive && top != static_cast<juce::Component*> (this))
                {
                    y2kSnapDebugDumpDisplays ("snap (first window drag release)");
                    y2kSnapWindowToMonitorEdgesPx (top, PinkXP::ui (titleBarHeight));
                }
            }
            else
           #endif
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

                    // 顶部拖出回弹：标题栏拖出工作区顶部 → 强制弹回，
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
    }

    winDragPxActive = false;
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
  // ---- 全局撤回 / 重做：Ctrl(⌘)+Z / Ctrl(⌘)+Shift+Z / Ctrl+Y ----
  if (workspace != nullptr && workspace->handleUndoRedoKeyPress (key))
    return true;

  // ---- 诊断快捷键：Ctrl(⌘)+Shift+D ----
  //   导出"当前状态快照"并打开日志目录。用于用户反馈"偶尔卡一下但没卡死"
  //   的场景：让他在卡顿刚发生时按一下，日志里就留下当时的完整状态。
  if (y2k::diag::isEnabled())
  {
      const auto mods = key.getModifiers();
      if ((mods.isCtrlDown() || mods.isCommandDown()) && mods.isShiftDown()
          && key.getKeyCode() == 'D')
      {
          // 顺手把当前 z 序打进日志：图片被 GL 盖住时，这一行能直接说明
          //   "图片在模块下面"还是"合成没跑"。
          if (workspace != nullptr)
              Y2K_DIAG ("PAINT", juce::String ("[manual] z=") + workspace->diagDescribeZOrder());

          const auto snapshotPath = y2k::diag::exportSnapshot();
          y2k::diag::reportNow ("user pressed Ctrl+Shift+D");

          const auto dir = y2k::diag::getLogDirectory();
          dir.revealToUser();

          // 界面语言：标题 / 字段名走 y2k::tr（中文译文见 i18n/zh-CN.lang）
          juce::NativeMessageBox::showMessageBoxAsync (
              juce::MessageBoxIconType::InfoIcon,
              y2k::tr ("Diagnostics exported"),
              y2k::tr ("Log folder:") + "\n" + dir.getFullPathName()
                  + (snapshotPath.isNotEmpty()
                         ? "\n\n" + y2k::tr ("Snapshot:") + "\n" + snapshotPath
                         : juce::String()));
          return true;
      }
  }

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

  if (milk == nullptr || ! milk->isKeyboardControlEnabled())
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
        // 1) 保存当前 bounds（逻辑；Windows 另存物理矩形，退出时优先用物理矩形还原）
        preFullScreenBounds = top->getBounds();
       #if JUCE_WINDOWS
        preFullScreenRectPx = y2kGetWindowRectPx (top);
       #endif

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

        // 2.5) 同步放宽编辑器自身 constrainer（关键修复）：AudioProcessorEditor 在
        //   构造期设过 setResizeLimits(640,420,1600,1100)，伪全屏窗口宽度常 >1600。
        //   若只放宽顶层窗口 constrainer、漏掉编辑器自己的，后续任何编辑器级
        //   setResizeLimits（如 Hide/Show 切 chrome）都会经 setBoundsConstrained 把
        //   编辑器夹回 1600 宽，再经 ResizableWindow 的 resizeToFitContent 反噬把
        //   窗口一并压小。这里保存编辑器 limits 并一并放宽，退出时还原。
        if (auto* ec = getConstrainer())
        {
            preFullScreenEditorMinW = ec->getMinimumWidth();
            preFullScreenEditorMinH = ec->getMinimumHeight();
            preFullScreenEditorMaxW = ec->getMaximumWidth();
            preFullScreenEditorMaxH = ec->getMaximumHeight();
            preFullScreenEditorLimitsValid = true;
            setResizeLimits (1, 1, 32768, 32768);
        }

        // 3) 计算目标 bounds：当前显示器的 userArea（已扣除任务栏）
       #if JUCE_WINDOWS
        // Windows：全程物理像素（MonitorFromWindow + rcWork + MoveWindow）。
        //   不能用 setBounds(userArea)：混合 DPI 桌面上 JUCE 的
        //   逻辑→物理 换算会用错显示器原点，窗口会"大小对了但位置窜到另一块屏"。
        if (! y2kScreenSimulationActive())
        {
            Y2KMonitorPx mon;
            if (y2kGetMonitorPxForWindow (top, mon) && ! mon.work.isEmpty())
            {
                auto targetRect = mon.work;
                // Windows 11 + NVIDIA 驱动：自动隐藏任务栏导致 rcWork == rcMonitor 时，
                //   与物理屏严格等大的无边框窗口会被 FSO 接管而关闭 DWM 透明合成变黑，
                //   透明模式下底边让出 1 物理像素即可彻底规避。
                if (PinkXP::isTransparentBackdrop() && targetRect == mon.total && targetRect.getHeight() > 2)
                    targetRect = targetRect.withTrimmedBottom (1);
                y2kMoveWindowToRectPx (top, targetRect);
                pseudoFullScreenRectPx = targetRect;
                pseudoFullScreen = true;
                return;
            }
        }
       #endif

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

       #if JUCE_WINDOWS
        pseudoFullScreenRectPx = {};
        // 只恢复到"仍存在的显示器上"的原位置：全屏期间拔掉的那块屏不能再作为落点
        //   （否则窗口会跑到不存在的区域，虽然 ensureWindowOnScreen 会兜底，
        //     但那一帧是可见的跳动）。
        if (! preFullScreenRectPx.isEmpty() && y2kIsRectOnAnyMonitorPx (preFullScreenRectPx))
        {
            y2kMoveWindowToRectPx (top, preFullScreenRectPx);
            preFullScreenRectPx = {};
        }
        else
       #endif
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

        // 2.5) 还原编辑器自身 constrainer（与进入分支的放宽对应）
        if (preFullScreenEditorLimitsValid)
        {
            const int eminW = juce::jmax (1, preFullScreenEditorMinW);
            const int eminH = juce::jmax (1, preFullScreenEditorMinH);
            const int emaxW = juce::jmax (eminW, preFullScreenEditorMaxW);
            const int emaxH = juce::jmax (eminH, preFullScreenEditorMaxH);
            setResizeLimits (eminW, eminH, emaxW, emaxH);
            preFullScreenEditorLimitsValid = false;
        }
    }
}

// ==========================================================
// 启动恢复伪全屏（v2.9.9 补充）
//   Standalone 读到 window.fullscreen=1 后，在窗口上屏后的 callAsync 里调用。
//   必须晚于 restoreMainWindowBounds()（普通几何先就位）与 peer 创建：
//   Windows 伪全屏按物理像素贴合当前显示器工作区（MonitorFromWindow）。
//
//   直接复用 toggleFakeFullScreen() 的"进入"分支，因此：
//     · preFullScreenBounds 记录的是刚恢复出来的普通窗口尺寸，落盘严格幂等；
//     · resize limits / 编辑器 constrainer 的放宽与双击进入完全同源；
//     · 若进入失败（拿不到显示器信息），pseudoFullScreen 保持 false，
//       下次退出写回 window.fullscreen=0，不会反复尝试。
// ==========================================================
void Y2KmeterAudioProcessorEditor::restorePseudoFullScreen()
{
    if (isPluginHost) return;
    if (pseudoFullScreen) return;
    if (! isShowing()) return;

    // stick 驻留态与全屏互斥：启动时若有待恢复的 stick 会话，
    //   restoreSavedStickSession() 会接管窗口几何，这里不能抢先搬窗口。
    //   pendingStickRestore 是 Windows 专属成员（见 PluginEditor.h 的
    //   #if JUCE_WINDOWS 块），非 Windows 上 stick 恒不激活，无需判断。
   #if JUCE_WINDOWS
    if (stickActive || pendingStickRestore) return;
   #endif

    auto* top = getTopLevelComponent();
    if (top == nullptr || top == this) return;

   #if JUCE_MAC
    // macOS 走系统原生全屏（不是本函数的伪全屏），恢复动作由
    //   Y2KStandaloneApp::initialise() 在窗口上屏后按 window.nativeFullScreen
    //   延迟调用 setFullScreen(true) —— 这里直接返回。
    return;
   #else
    toggleFakeFullScreen();
   #endif
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
        ws.add ({ it.displayName, it.sourceId, it.isLoopback, it.submenuPath, it.separatorBefore });

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

void Y2KmeterAudioProcessorEditor::relaxConstrainerForWindowSize (int w, int h)
{
    if (isPluginHost)
        return;  // 插件宿主 constrainer 已是 8192，无需处理

    auto* c = getConstrainer();
    if (c == nullptr)
        return;

    const int minW  = c->getMinimumWidth();
    const int minH  = c->getMinimumHeight();
    const int needW = juce::jmax (w, minW);
    const int needH = juce::jmax (h, minH);

    if (c->getMaximumWidth() >= needW && c->getMaximumHeight() >= needH)
        return;  // 已能容纳，无需放宽

    // 只放宽 max（min 保持不变），使 constrainer 能容纳当前窗口尺寸，
    // 避免后续 setResizeLimits 夹回导致窗口缩水。
    setResizeLimits (minW, minH,
                     juce::jmax (c->getMaximumWidth(),  needW),
                     juce::jmax (c->getMaximumHeight(), needH));
}

void Y2KmeterAudioProcessorEditor::prepareConstrainerForWindowRestore (int w, int h)
{
    if (isPluginHost)
        return;

    auto* c = getConstrainer();
    if (c == nullptr)
        return;

    const int wantW = juce::jmax (1, w);
    const int wantH = juce::jmax (1, h);

    const int minW = juce::jmin (c->getMinimumWidth(),  wantW);
    const int minH = juce::jmin (c->getMinimumHeight(), wantH);
    const int maxW = juce::jmax (c->getMaximumWidth(),  wantW);
    const int maxH = juce::jmax (c->getMaximumHeight(), wantH);

    setResizeLimits (minW, minH, maxW, maxH);
}

// ==========================================================
// timerCallback —— 10Hz 拉 Processor 的 CPU 占用，广播给所有模块；
//                  同时按 1s 窗口换算实际 FPS，下发给 workspace 的 FPS 标签
// ==========================================================
void Y2KmeterAudioProcessorEditor::timerCallback()
{
    // v2.9.3：帧耗时统计周期性落盘（诊断构建；release 构建为空操作）
   #if Y2K_ENABLE_FRAME_PERF_LOG
    y2k::perflog::dumpIfDue();
   #endif

    // 成就 #3（深潜）：每 ≈300ms 采样一次当前 Milkdrop 预设。
    //   · 采样而不是挂钩 GL 线程的切换点 —— 预设切换发生在 GL 线程，
    //     那里不能碰 Steam；而"当前预设路径"这个 getter 本身是线程安全的。
    //   · 未 attach（插件宿主 / 非 Steam 版）时 isActive() 为 false，直接跳过。
    if (y2k::ach::isActive() && ++achPresetPollTicks >= 3)
    {
        achPresetPollTicks = 0;
        y2k::ach::reportMilkdropPreset (GetMilkdropCurrentPresetFilePath());
    }

    // ---- 诊断：消息线程心跳 ----
    //   这个 Timer 是 10Hz 消息线程存活的最可靠标志。它停了 = 消息线程
    //   被卡住（无论是死在 GL 锁、音频锁还是绘制里），看门狗据此报警。
    if (y2k::diag::isEnabled())
    {
        y2k::diag::setThreadTag ("UI");
        y2k::diag::heartbeat (y2k::diag::Channel::UiMessage);
        y2k::diag::setChannelActive (y2k::diag::Channel::UiMessage, true);
        y2k::diag::addUiTick();

        // GL 通道：仅当窗口真的在屏幕上、且最近确实在出帧时才要求出帧。
        //   · 最小化 / 隐藏 / 上下文未 attach：JUCE 本来就不出帧，不能算卡死；
        //   · 窗口静止（空工作区、无任何重绘）：30s 内出帧计数不变 → 不再监视，
        //     否则"界面静止"会被误报成"卡死"。
        const juce::int64 glNow = glFrameCounter.load (std::memory_order_relaxed);
        const double nowWallMs  = juce::Time::getMillisecondCounterHiRes();
        if (glNow != diagLastGlCounter)
        {
            diagLastGlCounter         = glNow;
            diagGlCounterChangeWallMs = nowWallMs;
        }

        // 至少真正出过一帧才开始监视（从未出帧的窗口不算"卡死"，
        //   那种情况由看门狗的"attach 后 10s 无 GL 上下文"告警覆盖）。
        const bool glFlowing = glNow > 0
                                 && (nowWallMs - diagGlCounterChangeWallMs) < 30000.0;

        bool glActive = openGLContext.isAttached() && isShowing() && isVisible() && glFlowing;
        if (auto* peer = getPeer())
            if (peer->isMinimised())
                glActive = false;
        y2k::diag::setChannelActive (y2k::diag::Channel::GlFrame, glActive);

        // ---- v3.0.1：组件层合成健康度（每 5s 一行）----
        //   GL 每帧都会写后缓冲，但 JUCE 只在"存在组件脏区"时才重新合成组件层
        //   （juce_OpenGLContext.cpp: renderOpenGL → 仅 isUpdating 时 paintComponent）。
        //   所以一旦组件重绘链断了，GL 内容就会永久压住图片/其它模块。
        //   这里同时记录：GL 出帧数、组件合成数、各模块的重绘/合并/跳过计数、
        //   以及工作区 z 序（判断图片到底在模块上面还是下面）。
        {
            static juce::uint32 lastGlTicks   = 0;
            static juce::uint32 lastCompTicks = 0;
            static double       lastReportMs  = 0.0;

            const juce::uint32 glT   = (juce::uint32) glNow;
            // 用工作区的 paint 次数当"组件层合成次数"：
            //   Editor 自身的 paint 会被不透明子组件（模块）遮挡裁剪掉，读数偏小，
            //   工作区是模块的父级，几乎每次合成都会跑到。
            const juce::uint32 compT = (workspace != nullptr) ? workspace->diagGetPaintTicks() : 0;

            if (lastReportMs <= 0.0)
            {
                lastReportMs  = nowWallMs;
                lastGlTicks   = glT;
                lastCompTicks = compT;
            }
            else if (nowWallMs - lastReportMs >= 5000.0)
            {
                const double dt = juce::jmax (0.001, (nowWallMs - lastReportMs) / 1000.0);
                const juce::uint32 dGl   = glT   - lastGlTicks;
                const juce::uint32 dComp = compT - lastCompTicks;

                juce::String line;
                line << "gl=" << juce::String ((int) std::lround ((double) dGl / dt)) << "/s"
                     << " comp=" << juce::String ((double) dComp / dt, 1) << "/s";

                if (workspace != nullptr)
                    line << " | z=" << workspace->diagDescribeZOrder()
                         << " | "  << workspace->diagTakePaintStats();

                Y2K_DIAG ("PAINT", line);

                // 渲染线程分段耗时（JUCE 补丁 0006）：定位 GL 帧时间花在哪。
                //   每项为"每帧平均 ms"；paint 项按"实际重绘组件层的帧"平均。
                {
                    const auto rs = openGLContext.takeRenderStats();
                    if (rs.frames > 0)
                    {
                        const double f  = (double) rs.frames;
                        const double pf = (double) juce::jmax (1, rs.paintedFrames);
                        juce::String ph;
                        ph << "frames=" << rs.frames << " paint=" << rs.paintedFrames
                           << " | per-frame ms: preLock " << juce::String (rs.preLockMs / f, 2)
                           << " sleep " << juce::String (rs.sleepMs / f, 2)
                           << " lockWait " << juce::String (rs.lockWaitMs / f, 2)
                           << " renderOpenGL " << juce::String (rs.rendererMs / f, 2)
                           << " paintComp " << juce::String (rs.paintMs / f, 2)
                           << " draw " << juce::String (rs.drawMs / f, 2)
                           << " swap " << juce::String (rs.swapMs / f, 2)
                           << " | paintComp/painted-frame " << juce::String (rs.paintMs / pf, 2)
                           << "ms, " << juce::String (rs.paintedMPix / pf, 3) << "Mpx";
                        Y2K_DIAG ("GLPHASE", ph);
                        if (toyDirector != nullptr && toyDirector->clock != nullptr)
                        {
                            const auto ts = toyDirector->clock->takeDiagSnapshot();
                            juce::String tl;
                            tl << "free=" << ts.freeActors << " fullEmoji=" << ts.fullEmoji
                               << " target=" << ts.emojiTarget << " smoothedFps="
                               << juce::String (ts.smoothedFps, 1)
                               << " | overlay paints=" << ts.overlayPaints
                               << " actors=" << ts.overlayActorsPainted
                               << " ms=" << juce::String (ts.overlayPaintMs, 1)
                               << " glToy=" << (toyDirector->isGlToyActive() ? 1 : 0)
                               << " degraded=" << ts.degradedFreeActors;
                            Y2K_DIAG ("TOY", tl);
                        }
                    }
                }
                if (workspace != nullptr)
                {
                    for (int i = 0; i < workspace->getNumModules(); ++i)
                        if (auto* md = dynamic_cast<MilkdropModule*> (workspace->getModule (i)))
                        {
                            Y2K_DIAG ("PAINT", juce::String ("milkdrop ") + md->diagDescribeGlView());
                            break;
                        }
                }

                if (dGl > 0 && dComp == 0)
                    Y2K_DIAG ("PAINT", y2k::diag::u8 ("!! 5s 内 GL 出了 ")
                                        + juce::String ((int) dGl)
                                        + y2k::diag::u8 (" 帧但组件层零合成 —— GL 内容会永久压住所有组件"));

                lastReportMs  = nowWallMs;
                lastGlTicks   = glT;
                lastCompTicks = compT;
            }
        }

        y2k::diag::setEditorState (getWidth(), getHeight(),
                                   (float) openGLContext.getRenderingScale(),
                                   openGLContext.isAttached(),
                                   true,
                                   milkdrop_render_ready_.load(),
                                   (int) getNumChildComponents(),
                                   glFrameCounter.load (std::memory_order_relaxed));
    }

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

    // 显示器拓扑兜底：窗口跑出所有显示器 / 伪全屏目标工作区变了（拔投影仪等）。
    ensureWindowOnScreen();
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
        //   v2.9.7：去掉 min()。mac 上 Editor GL 启用后，GL 线程持锁会吞掉消息
        //   线程的 vblank 回调（分发掉帧），但 GL 侧仍持续真实出帧；此时
        //   min(dispatch, gl) 会取到被低估的分发数，导致"显示帧率 < 实际帧率"。
        //   每次 renderOpenGL = 一次真实屏幕出帧提交，因此 GL attached 时直接以
        //   glFps 为准（GL 跟不上时 glFps 本身就是真实低值，无需再与分发数取小）。
        const juce::int64 glCur  = glFrameCounter.load (std::memory_order_relaxed);
        const juce::int64 glDiff = glCur - lastGlFrameCounterSample;
        lastGlFrameCounterSample = glCur;

        float fps = dispatchFps;
        if (glDiff > 0 && openGLContext.isAttached())
            fps = (float) (glDiff * 1000.0 / deltaMs);

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

        // 成就心跳：#2 长跑选手（累计运行 10h）。
        //   · 用真实 dt 而不是固定 1.0，掉帧 / 系统睡眠时不会虚增时长；
        //   · 非 Steam 版 / 插件宿主内 ach 未 attach → 立即返回，零开销。
        y2k::ach::sessionTick (deltaMs * 0.001);

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
void Y2KmeterAudioProcessorEditor::syncExternalFramePacing (bool active)
{
#if ! JUCE_MAC
    openGLContext.setExternalFramePacing (active);
    for (auto& wnd : floatingWindows_)
        if (wnd != nullptr)
            wnd->setExternalFramePacing (active);
#else
    juce::ignoreUnused (active);
#endif
}

void Y2KmeterAudioProcessorEditor::syncToyExpectedFps()
{
    if (toyDirector == nullptr)
        return;

    const double target = vblankPacer_.effectiveTargetHz (userRequestedFpsLimit);
    if (std::abs (target - lastSyncedExpectedFps_) > 0.25)
    {
        lastSyncedExpectedFps_ = target;
        toyDirector->setExpectedFps (target);
    }
}

void Y2KmeterAudioProcessorEditor::onDisplayVBlank (double timestampSec)
{
    const double nowWallMs = juce::Time::getMillisecondCounterHiRes();
    vblankLastWallMs = nowWallMs;

    // 首个 vblank（或看门狗回退后 vblank 恢复）：接管帧时钟并立即出一帧
    if (! processor.getAnalyserHub().isUsingExternalFrameClock())
    {
        processor.getAnalyserHub().setExternalFrameClock (true);
        syncExternalFramePacing (true);
        vblankPacer_.reset (false);
        vblankLastSec = timestampSec;
        syncToyExpectedFps();
        updateFramePacingFallbackTimer();
        if (vblankPacer_.shouldDispatchOnVBlank (userRequestedFpsLimit, 0.0, nowWallMs))
            performFrameDispatch (true, timestampSec, nowWallMs);
        return;
    }

    tryDispatchFromVBlank (nowWallMs, timestampSec);
}

void Y2KmeterAudioProcessorEditor::performFrameDispatch (bool fromVBlank, double vblankSec, double nowWallMs)
{
    const double dispatchSec = vblankPacer_.nextMonotonicTimeSec (fromVBlank, vblankSec, nowWallMs);
    processor.getAnalyserHub().dispatchFrameNow (dispatchSec);

    // Win/非 macOS（JUCE 补丁 0010）：将本帧分发期间各模块与 ToyBox 产生的 repaint /
    //   RequestGlFrame 合并为一次统一唤醒，保持 1:1 VBlank 节奏并消除中途争锁。
    if (openGLContext.isAttached())
        openGLContext.flushPacedRepaint();
    for (auto& wnd : floatingWindows_)
        if (wnd != nullptr)
            wnd->flushPacedRepaint();
}

void Y2KmeterAudioProcessorEditor::tryDispatchFromVBlank (double nowWallMs, double timestampSec)
{
    double dtMs = 0.0;
    if (vblankLastSec > 0.0)
        dtMs = (timestampSec - vblankLastSec) * 1000.0;
    vblankLastSec = timestampSec;

    const bool shouldDispatch =
        vblankPacer_.shouldDispatchOnVBlank (userRequestedFpsLimit, dtMs, nowWallMs);

    // 显示器刷新率首次锁定或跨屏切换后，同步期望帧率与兜底 Timer 周期
    syncToyExpectedFps();
    updateFramePacingFallbackTimer();

    if (shouldDispatch)
        performFrameDispatch (true, timestampSec, nowWallMs);
}

void Y2KmeterAudioProcessorEditor::tryDispatchFromFallbackTimer (double nowWallMs)
{
    if (vblankPacer_.shouldDispatchOnFallback (userRequestedFpsLimit, nowWallMs))
        performFrameDispatch (false, 0.0, nowWallMs);
}

void Y2KmeterAudioProcessorEditor::updateFramePacingFallbackTimer()
{
    if (framePacingFallback == nullptr)
        return;

    if (processor.getAnalyserHub().isUsingExternalFrameClock())
    {
        // 所有模式（含无上限 userRequestedFpsLimit==0）均挂兜底 Timer；
        //   仅当推荐周期改变时才重启定时器，避免每次 VBlank 重置倒计时。
        const int intervalMs = vblankPacer_.recommendedFallbackTimerMs (userRequestedFpsLimit);
        if (intervalMs != activeFallbackTimerMs_ || ! framePacingFallback->isTimerRunning())
        {
            activeFallbackTimerMs_ = intervalMs;
            framePacingFallback->startTimer (intervalMs);
        }
    }
    else
    {
        if (framePacingFallback->isTimerRunning())
            framePacingFallback->stopTimer();
        activeFallbackTimerMs_ = 0;
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
        // 立即关闭外部合并并 flush 任意挂起的重绘，避免 fallback timer 刚刚刷新过
        //   lastExternalPaceMs 导致切回内部 Timer 后 ~100ms 内的重绘被暂存丢失。
        syncExternalFramePacing (false);
        vblankPacer_.reset (false);
        vblankLastSec = 0.0;
        syncToyExpectedFps();
        updateFramePacingFallbackTimer();
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

  // ----------------------------------------------------------------
  // Milkdrop 现场诊断（v3.0.5 诊断包）
  //   目的：一份用户日志就能判定"纯黑"卡在哪一环 ——
  //     ① projectM 手里没有有效预设（LoadMilkdropPresetInternal 早退）
  //     ② 渲染被 60fps 节流/预渲染复用跳过，离屏 FBO 里一直是分配时的空内容
  //     ③ 渲染了但 FBO→FBO 的 glBlitFramebuffer 被驱动丢弃（0x502）
  //     ④ 内容与 FBO 都正常，则问题在"搬到屏幕"那一步（矩形/GL 状态/组件层覆盖）
  //   约束：只在 GL 线程访问（匿名命名空间、无锁）；只在诊断开启时写入，
  //         关闭时每个埋点只剩一次 relaxed 原子读（Y2K_DIAG 宏内部）。
  //         glReadPixels 这类会同步等 GPU 的采样额外用 Y2K_ENABLE_LOGGING 做
  //         编译期隔离 —— 正式 Release 包里连代码都不存在。
  // ----------------------------------------------------------------
  struct MilkdropDiagState
  {
      double lastReportMs = 0.0;
      double lastProbeMs  = 0.0;
      int    rendered = 0, skipped = 0, blitErr = 0;        // 离屏渲染阶段
      int    screenBlit = 0, screenBlitErr = 0;             // blit 到后缓冲阶段
      int    allocCount = 0;                                // FBO 分配/重分配次数
      int    reportRendered = 0, reportSkipped = 0;         // 上次统计窗口的基线
      int    reportBlitErr = 0, reportScreenBlit = 0, reportScreenBlitErr = 0;
      juce::Rectangle<int> lastScreenRect;
      unsigned int lastScreenSrcFbo = 0xFFFFFFFFu;
      juce::String lastProbe = "(none)";
      int    lastFboW = 0, lastFboH = 0;
      bool   lastTintActive = false;
      bool   lastUseFboApi  = false;
  };
  MilkdropDiagState gMdDiag;

  // 亮度采样：从 FBO 采 3x3 个 4x4 小块（共 144 像素）。
  //   · 诊断用：把结果拼成字符串进日志；
  //   · 自愈用：maxLuma == 0 即"整块全黑"，用来判定"是不是该自救了"。
  //   glReadPixels 会同步等 GPU，调用方必须限流（渲染路径里 1Hz、诊断 2s）。
  //   这个函数**不**做诊断开关隔离 —— 黑帧自愈在正式包里也要靠它。
  struct MilkdropLumaSample
  {
      int maxLuma  = -1;
      int nonBlack = 0;
      int total    = 0;
      bool valid   = false;
  };

  MilkdropLumaSample MilkdropSampleLuma (unsigned int fbo, int w, int h)
  {
      using namespace juce::gl;

      MilkdropLumaSample out;
      if (fbo == 0 || w <= 0 || h <= 0)
          return out;

      constexpr int kPatch = 4;
      static constexpr double kPos[] = { 0.5, 0.2, 0.8 };

      unsigned char buf[kPatch * kPatch * 4];

      GLint prevRead = 0;
      glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &prevRead);
      glBindFramebuffer (GL_READ_FRAMEBUFFER, fbo);

      out.maxLuma = 0;
      for (double py : kPos)
      {
          for (double px : kPos)
          {
              const int x = juce::jlimit (0, juce::jmax (0, w - kPatch), (int) (px * (double) w));
              const int y = juce::jlimit (0, juce::jmax (0, h - kPatch), (int) (py * (double) h));
              glReadPixels (x, y, kPatch, kPatch, GL_RGBA, GL_UNSIGNED_BYTE, buf);

              for (int i = 0; i < kPatch * kPatch; ++i)
              {
                  const int luma = (buf[i * 4 + 0] * 299
                                    + buf[i * 4 + 1] * 587
                                    + buf[i * 4 + 2] * 114) / 1000;
                  out.maxLuma = juce::jmax (out.maxLuma, luma);
                  if (luma > 2)
                      ++out.nonBlack;
                  ++out.total;
              }
          }
      }

      glBindFramebuffer (GL_READ_FRAMEBUFFER,
                         (unsigned int) juce::jmax (0, (int) prevRead));
      while (glGetError() != GL_NO_ERROR) {}   // 采样自身的错误不要污染帧级检查

      out.valid = true;
      return out;
  }

  // 诊断字符串（Release + Y2K_DIAG_LOG_DEFAULT 的 DiagRel 包也要有 —— 上一版只按
  //   Y2K_ENABLE_LOGGING 隔离，导致那个包里 lumaMax 一直是 "(none)"，白丢一轮证据）。
#if defined (Y2K_ENABLE_LOGGING) || (defined (Y2K_DIAG_LOG_DEFAULT) && Y2K_DIAG_LOG_DEFAULT)
  juce::String MilkdropProbeFbo (unsigned int fbo, int w, int h)
  {
      const auto s = MilkdropSampleLuma (fbo, w, h);
      if (! s.valid)
          return "probe=skip(no fbo)";

      return "lumaMax=" + juce::String (s.maxLuma)
           + " nonBlack=" + juce::String (s.nonBlack) + "/" + juce::String (s.total);
  }
#endif
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

  // v2.9.7：安装包将 milkdrop_presets 压缩为单文件 zip，bundle 内只有 zip 时
  //   首次查找即解压 seed 到 AppData（zip 顶层直接是 .milk 文件）。
  auto seedZipToAppDataIfNeeded = [&](const juce::File& zipFile) -> juce::File {
    auto parent = appDataDir.getParentDirectory();
    if (!parent.exists())
      parent.createDirectory();
    if (!appDataDir.exists())
      appDataDir.createDirectory();
    juce::ZipFile zf (zipFile);
    if (zf.uncompressTo (appDataDir, true).wasOk() && isValidAssetsDir (appDataDir))
      return appDataDir;
    return appDataDir;
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
    if (subdir == "milkdrop_presets")
    {
      auto zipCandidate = contentsDir
          .getChildFile("Resources")
          .getChildFile("assets")
          .getChildFile("milkdrop_presets.zip");
      if (zipCandidate.existsAsFile())
        return seedZipToAppDataIfNeeded(zipCandidate);
    }
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
      if (subdir == "milkdrop_presets")
      {
        auto pluginZip = pluginContents
            .getChildFile("Resources")
            .getChildFile("assets")
            .getChildFile("milkdrop_presets.zip");
        if (pluginZip.existsAsFile())
          return seedZipToAppDataIfNeeded(pluginZip);
      }
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

bool Y2KmeterAudioProcessorEditor::hasEmbeddedMilkdropModule() const
{
  if (workspace == nullptr)
    return false;
  for (int i = 0; i < workspace->getNumModules(); ++i)
    if (auto* m = workspace->getModule (i))
      if (m->getModuleType() == ModuleType::milkdrop)
        return true;
  return false;
}

void Y2KmeterAudioProcessorEditor::stopMilkdropPresetScanThread()
{
  std::lock_guard<std::mutex> lock (milkdrop_scan_lifecycle_mutex_);
  milkdrop_scan_cancel_.store (true, std::memory_order_release);
  milkdrop_scan_generation_.fetch_add (1, std::memory_order_acq_rel);

  if (milkdrop_scan_thread_.joinable())
    milkdrop_scan_thread_.join();
  milkdrop_scan_in_flight_.store (false, std::memory_order_release);
}

void Y2KmeterAudioProcessorEditor::startMilkdropPresetScanAsync (bool useLike, bool forceRestart)
{
  std::lock_guard<std::mutex> lock (milkdrop_scan_lifecycle_mutex_);
  if (milkdrop_scan_shutdown_.load (std::memory_order_acquire)
      || editorBeingDestructed.load (std::memory_order_acquire))
    return;

  if (! forceRestart)
  {
    // 若用户刚刚切换了目标库，丢弃持有旧 useLike 的非强制请求，避免覆盖新库状态
    if (milkdrop_use_like_library_.load (std::memory_order_acquire) != useLike)
      return;

    // 若同库列表已经扫描完成，直接早退（防止 Message/GL 并发调用把 ready 误清为 false）
    if (milkdrop_presets_ready_.load (std::memory_order_acquire))
    {
      const std::lock_guard<std::mutex> stateLock (milkdrop_state_mutex_);
      if (milkdrop_scanned_use_like_ == useLike)
        return;
    }

    if (milkdrop_scan_in_flight_.load (std::memory_order_acquire)
        && milkdrop_scan_target_like_ == useLike)
      return;
  }

  // 严格持 milkdrop_scan_lifecycle_mutex_ 回收旧线程（worker 绝不碰该锁，不会死锁），
  // 杜绝对 joinable std::thread 做并发 move-assign 触发 std::terminate()。
  milkdrop_scan_cancel_.store (true, std::memory_order_release);
  milkdrop_scan_generation_.fetch_add (1, std::memory_order_acq_rel);
  if (milkdrop_scan_thread_.joinable())
    milkdrop_scan_thread_.join();
  milkdrop_scan_in_flight_.store (false, std::memory_order_release);

  const auto presetsDir = useLike
                              ? FindMilkdropLikeDir()
                              : FindMilkdropAssetsDir ("milkdrop_presets");

  milkdrop_scan_cancel_.store (false, std::memory_order_release);
  milkdrop_presets_ready_.store (false, std::memory_order_release);
  const uint64_t gen = milkdrop_scan_generation_.fetch_add (1, std::memory_order_acq_rel) + 1;

  milkdrop_scan_target_like_ = useLike;
  milkdrop_scan_in_flight_.store (true, std::memory_order_release);
  milkdrop_scan_thread_ = std::thread ([this, presetsDir, useLike, gen]()
  {
    const double t0 = y2k::diag::isEnabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    juce::StringArray newPaths;
    if (presetsDir.isDirectory())
    {
      newPaths.ensureStorageAllocated (useLike ? 128 : 10240);
      for (const auto& entry : juce::RangedDirectoryIterator (
               presetsDir, false, "*.milk", juce::File::findFiles))
      {
        if (milkdrop_scan_cancel_.load (std::memory_order_acquire)
            || milkdrop_scan_generation_.load (std::memory_order_acquire) != gen)
          break;
        newPaths.add (entry.getFile().getFullPathName());
      }
      if (! milkdrop_scan_cancel_.load (std::memory_order_acquire)
          && milkdrop_scan_generation_.load (std::memory_order_acquire) == gen)
        newPaths.sort (false);
    }

    const bool cancelled = milkdrop_scan_cancel_.load (std::memory_order_acquire)
                           || milkdrop_scan_generation_.load (std::memory_order_acquire) != gen;
    if (! cancelled)
    {
      int count = 0;
      {
        const std::lock_guard<std::mutex> stateLock (milkdrop_state_mutex_);
        milkdrop_preset_paths_.swapWith (newPaths);
        milkdrop_scanned_use_like_ = useLike;
        count = milkdrop_preset_paths_.size();
      }
      milkdrop_presets_ready_.store (true, std::memory_order_release);
      milkdrop_needs_reconcile_.store (true, std::memory_order_release);

      if (t0 > 0.0)
      {
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        Y2K_DIAG ("MD", juce::String ("async rescan done useLike=")
                              + (useLike ? "1" : "0")
                              + " count=" + juce::String (count)
                              + " ms=" + juce::String (ms, 1));
      }
    }

    if (milkdrop_scan_generation_.load (std::memory_order_acquire) == gen)
      milkdrop_scan_in_flight_.store (false, std::memory_order_release);
  });
}

juce::String Y2KmeterAudioProcessorEditor::findFastInitialMilkdropPresetPath (
    bool useLike, int preferredIndex) const
{
  const auto presetsDir = useLike
                              ? FindMilkdropLikeDir()
                              : FindMilkdropAssetsDir ("milkdrop_presets");
  if (! presetsDir.isDirectory())
    return {};

  // 1) 若存在已保存的预设序号（内置库按 4 位数字前缀 0001_... 命名），单步通配符命中 (<0.5ms)
  if (! useLike && preferredIndex >= 0)
  {
    const auto pattern = juce::String::formatted ("%04d_*.milk", preferredIndex + 1);
    for (const auto& entry : juce::RangedDirectoryIterator (
             presetsDir, false, pattern, juce::File::findFiles))
      return entry.getFile().getFullPathName();
  }

  // 2) 兜底：单步目录迭代取首个 .milk 文件 (<0.5ms)，不扫描剩余 9,920 个文件
  for (const auto& entry : juce::RangedDirectoryIterator (
           presetsDir, false, "*.milk", juce::File::findFiles))
    return entry.getFile().getFullPathName();

  return {};
}

void Y2KmeterAudioProcessorEditor::initMilkdropEditorResources() {
  if (milkdrop_renderer_suspended_.load (std::memory_order_acquire)
      || milkdrop_pm_handle_ != nullptr)
    return;

  Y2K_DIAG_SLOW ("GL", "initMilkdropEditorResources (projectM init total)", 300.0);

  auto& api = projectm_api::Api::instance();
  if (!api.isAvailable()) {
    SetMilkdropError (api.loadError());
    return;
  }

  // 单实例防护
  int expected = 0;
  if (!gEditorProjectMInstances.compare_exchange_strong(expected, 1)) {
    SetMilkdropError ("Only 1 Milkdrop instance allowed (libprojectM limit).");
    return;
  }

  // GLEW init（Editor GL 上下文中，一次性）
  if (!api.initGlew()) {
    SetMilkdropError (api.loadError());
    gEditorProjectMInstances.store(0);
    return;
  }

  milkdrop_pm_handle_ = api.create();
  Y2K_LOG(juce::String("[EditorGL] projectm_create -> ")
          + juce::String(milkdrop_pm_handle_ != nullptr ? "OK" : "NULL")
          + " hasFboAPI=" + juce::String(api.hasOpenglRenderFrameFbo() ? 1 : 0));
  if (milkdrop_pm_handle_ == nullptr) {
    SetMilkdropError ("projectm_create() returned NULL.");
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

  // 快速首帧预设加载（禁止在 GL 线程首帧同步遍历/排序 9,921 个 .milk 文件）：
  //   · 若后台线程已完成同库的完整扫描 → 直接按索引加载（0ms 扫描开销）；
  //   · 否则单步 <0.5ms 命中首个/目标预设立刻渲染首帧，后台扫描完成后在
  //     renderOpenGLPreLock() 中无锁对账真实索引并消费在途切换请求。
  const bool useLike = milkdrop_use_like_library_.load (std::memory_order_acquire);
  bool fullListReadyForLib = false;
  int totalCount = 0;
  if (milkdrop_presets_ready_.load (std::memory_order_acquire))
  {
    const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
    fullListReadyForLib = (milkdrop_scanned_use_like_ == useLike);
    totalCount = milkdrop_preset_paths_.size();
  }

  if (fullListReadyForLib)
  {
    milkdrop_startup_jump_fallback_ = -1;
    const int pending = milkdrop_requested_preset_jump_.exchange (-1);
    if (pending >= 0 && pending < totalCount)
      milkdrop_current_preset_ = pending;
    else
      milkdrop_current_preset_ = (totalCount > 0) ? 0 : -1;

    if (milkdrop_current_preset_ >= 0)
      LoadMilkdropPresetInternal();
    milkdrop_current_preset_ui_.store (milkdrop_current_preset_);
    milkdrop_needs_reconcile_.store (false, std::memory_order_release);
  }
  else
  {
    startMilkdropPresetScanAsync (useLike);
    // 立即消费启动阶段队列中的保存预设序号（避免后续对账时用旧 startup jump 吞掉用户期间的 Next/Prev/Random），
    // 仅在扫描期间无用户切换操作时用 milkdrop_startup_jump_fallback_ 兜底恢复精确索引。
    const int preferred = milkdrop_requested_preset_jump_.exchange (-1);
    milkdrop_startup_jump_fallback_ = preferred;
    const juce::String fastPath = findFastInitialMilkdropPresetPath (useLike, preferred);
    if (fastPath.isNotEmpty())
    {
      loadMilkdropPresetPathInternal (fastPath, preferred >= 0 ? preferred : 0, 1);
      milkdrop_current_preset_ = (preferred >= 0) ? preferred : 0;
      milkdrop_current_preset_ui_.store (milkdrop_current_preset_);
    }
    else
    {
      milkdrop_current_preset_ = -1;
      milkdrop_current_preset_ui_.store (-1);
    }
    milkdrop_needs_reconcile_.store (true, std::memory_order_release);
  }

  // 创建 projectM 独立 offscreen FBO + 纹理附件。
  juce::gl::glGenFramebuffers(1, &milkdrop_render_fbo_);
  juce::gl::glGenTextures(1, &milkdrop_render_tex_);

  // 创建整体染色后处理着色器（master output colors）。
  milkdrop_tint_pass_.reset(new MilkdropTintPass(openGLContext));
  if (!milkdrop_tint_pass_->init())
  {
    Y2K_LOG("[EditorGL] Milkdrop tint pass init FAILED: "
            + milkdrop_tint_pass_->getLastError());
    milkdrop_tint_pass_.reset();
  }
  else
  {
    Y2K_LOG("[EditorGL] Milkdrop tint pass init OK");
  }

  milkdrop_render_ready_.store (true, std::memory_order_release);

  Y2K_DIAGC ("MD", juce::String ("editor projectM init | fboAPI=")
                        + (api.hasOpenglRenderFrameFbo() ? "1" : "0")
                        + " loadPresetData=" + (api.hasLoadPresetData() ? "1" : "0")
                        + " likeLib=" + (useLike ? "1" : "0")
                        + " presets=" + juce::String (MilkdropPresetCount())
                        + " presetsReady=" + (fullListReadyForLib ? "1" : "0")
                        + " idx=" + juce::String (milkdrop_current_preset_)
                        + " tintPass=" + (milkdrop_tint_pass_ != nullptr ? "1" : "0")
                        + " mesh=" + juce::String (kMilkdropDefaultMeshW) + "x"
                                   + juce::String (kMilkdropDefaultMeshH)
                        + " fps=" + juce::String (kMilkdropTargetFps));
}

void Y2KmeterAudioProcessorEditor::newOpenGLContextCreated() {
  Y2K_LOG("[EditorGL] newOpenGLContextCreated fired");

  // ---- 诊断：GL 线程身份 + 驱动能力 ----
  y2k::diag::setThreadTag ("GL");
  y2k::diag::setPhase ("Editor::newOpenGLContextCreated");
  y2k::diag::markGlContextCreated();
  milkdrop_gl_context_ready_.store (true, std::memory_order_release);
  // GL 上下文异步创建完成（例如 Standalone 窗口首次上屏）后，激活 ToyBox GL 批渲染宿主
  if (toyDirector != nullptr && glToyRenderer_ != nullptr)
    toyDirector->setGlToyHostActive (true);
  {
    using namespace juce::gl;
    const auto glString = [] (GLenum e) -> juce::String
    {
      const auto* s = reinterpret_cast<const char*> (glGetString (e));
      return s != nullptr ? juce::String (s) : juce::String ("(null)");
    };

    GLint maxTex = 0, maxViewport[2] = { 0, 0 };
    juce::gl::glGetIntegerv (GL_MAX_TEXTURE_SIZE, &maxTex);
    juce::gl::glGetIntegerv (GL_MAX_VIEWPORT_DIMS, maxViewport);

    const juce::String info =
        "vendor=" + glString (GL_VENDOR)
      + " | renderer=" + glString (GL_RENDERER)
      + " | version=" + glString (GL_VERSION)
      + " | glsl=" + glString (GL_SHADING_LANGUAGE_VERSION)
      + " | maxTex=" + juce::String (maxTex)
      + " | maxViewport=" + juce::String ((int) maxViewport[0]) + "x" + juce::String ((int) maxViewport[1])
      + " | renderScale=" + juce::String ((double) openGLContext.getRenderingScale(), 2)
      + " | editor=" + juce::String (getWidth()) + "x" + juce::String (getHeight())
      + " | imageCacheMB=96";

    y2k::diag::setGlInfo (info);

    const juce::String rendererLower = glString (GL_RENDERER).toLowerCase();
    if (rendererLower.contains ("gdi generic")
        || rendererLower.contains ("basic render")
        || rendererLower.contains ("llvmpipe")
        || rendererLower.contains ("swiftshader")
        || rendererLower.contains ("software"))
    {
      Y2K_DIAGC ("GL", "!! SOFTWARE RENDERER DETECTED -> GL 直渲染可能极慢或卡死");
    }
  }

  Y2K_DIAGC ("GL", "newOpenGLContextCreated enter");
  // v2.9.3：调大 GL 图片纹理缓存（JUCE 默认仅 8MB → 96MB，避免多模块 LRU 逐出）
  openGLContext.setImageCacheSize (96 * 1024 * 1024);

  // 仅当工作区实际存在嵌入态 Milkdrop 时才在首帧初始化 projectM；
  // 无 Milkdrop 模块时零 DLL / 零扫描 / 零着色器开销，后续用户添加时由 renderOpenGLPreLock 锁外懒创建。
  if (! milkdrop_renderer_suspended_.load (std::memory_order_acquire)
      && milkdrop_has_module_in_editor_.load (std::memory_order_acquire))
  {
    initMilkdropEditorResources();
  }

  Y2K_DIAGC ("GL", juce::String ("newOpenGLContextCreated done | milkdropRenderReady=")
                        + (milkdrop_render_ready_.load() ? "1" : "0")
                        + " projectM=" + (milkdrop_pm_handle_ != nullptr ? "ok" : "deferred")
                        + " presets=" + juce::String (MilkdropPresetCount()));
}

// ================================================================
// projectM 渲染一帧 + 整体染色后处理（GL 线程）
//   Win 性能优化：从 renderOpenGL() 抽出，既可在 JUCE 补丁的 renderOpenGLPreLock()
//   （未持消息锁、消息线程并行运行）里调用，也可在 renderOpenGL() 里同步调用。
//   只触碰 GL 线程私有状态与 PCM 互斥量，禁止访问组件树。
//   返回值：供各模块 blit 的源 FBO（染色后为 post FBO，否则为 render FBO）。
//   forceRender：忽略 60fps 节流强制渲染（FBO 尺寸变化后需要立刻有有效内容）。
// ================================================================
unsigned int Y2KmeterAudioProcessorEditor::renderMilkdropFrameCore (int fbo_w, int fbo_h, bool forceRender)
{
  using namespace juce::gl;
  auto& api = projectm_api::Api::instance();

  // 消费 PCM
  {
    // 诊断：这把锁由 UI 线程（PushMilkdropPcm）与 GL 线程共同持有。
    //   若 GL 线程卡在 "wait milkdrop_pcm_mutex_"，说明 UI 线程持锁未放。
    Y2K_DIAG_PHASE ("gl:wait milkdrop_pcm_mutex_");
    std::lock_guard<std::mutex> lock(milkdrop_pcm_mutex_);
    Y2K_DIAG_PHASE ("gl:milkdrop pcm feed");
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

  // 分配/重分配 offscreen FBO 纹理
  bool need_realloc = (milkdrop_render_fbo_ != 0 && milkdrop_last_fbo_w_ == 0);
  bool need_resize  = (milkdrop_render_fbo_ != 0
                       && (fbo_w != milkdrop_last_fbo_w_
                           || fbo_h != milkdrop_last_fbo_h_));
  if (need_realloc || need_resize) {
    // 诊断：projectM 离屏 FBO 的尺寸随最上层 Milkdrop 模块尺寸 × DPI / renderScale
    //   变化而重分配。4K + 150% DPI 时这里可能是 5000×3000 级别，弱显卡上
    //   glTexImage2D 会明显卡顿甚至失败。
    Y2K_DIAG ("GL", "milkdrop FBO resize -> " + juce::String (fbo_w) + "x" + juce::String (fbo_h)
                          + " (dpi=" + juce::String (openGLContext.getRenderingScale(), 2)
                          + " renderScale=1/" + juce::String (milkdrop_render_scale_) + ")");
    Y2K_DIAG_PHASE ("gl:TexImage2D (milkdrop render FBO)");
  }
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

  // 诊断：FBO 分配/重分配 —— 记录尺寸、对象 ID 与"帧缓冲是否完整"。
  //   status 不是 0x8CD5(GL_FRAMEBUFFER_COMPLETE) 时，后面所有 blit 都会返回
  //   GL_INVALID_FRAMEBUFFER_OPERATION，画面必然全黑，但旧日志一个字都看不到。
  if ((need_realloc || need_resize) && Y2K_DIAG_ENABLED())
  {
    ++gMdDiag.allocCount;
    juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, milkdrop_render_fbo_);
    const auto st = juce::gl::glCheckFramebufferStatus (juce::gl::GL_FRAMEBUFFER);
    juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);
    while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}

    Y2K_DIAG ("MD", juce::String (need_realloc ? "fbo alloc " : "fbo resize ")
                          + juce::String (fbo_w) + "x" + juce::String (fbo_h)
                          + " fbo=" + juce::String ((int) milkdrop_render_fbo_)
                          + " tex=" + juce::String ((int) milkdrop_render_tex_)
                          + " status=0x" + juce::String::toHexString ((int) st)
                          + (st == juce::gl::GL_FRAMEBUFFER_COMPLETE ? " (complete)"
                                                                    : " (!! INCOMPLETE)")
                          + " scale=1/" + juce::String (milkdrop_render_scale_)
                          + " alloc#=" + juce::String (gMdDiag.allocCount));
  }

  bool use_fbo_api = api.hasOpenglRenderFrameFbo();

  // ================================================================
  // 渲染 projectM
  // v2.9.7：projectM 渲染按目标帧率（60fps）节流。Editor GL 单线程承担
  //   Milkdrop 渲染 + 直渲染模块 + CPE 组件合成，120Hz 下每帧重复渲染
  //   projectM 是纯浪费（projectM 内部动画按 setFps(60) 计时），尤其 mac
  //   上 NSOpenGL 驱动 draw call 成本高。节流后 blit 仍每帧执行，画面位置
  //   实时跟随模块拖动；FBO 保留上一帧内容，视觉无跳变。
  const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
  // Win 性能优化：节流改为"相位累加"。旧写法（距上次渲染 < 1/60s 就跳过）在帧周期不能整除
  //   16.67ms 时会系统性偏低（例如 6.5ms 一帧 → 每 3 帧渲染一次 = 51fps）；
  //   按下一次到期时刻累加，长期平均严格等于 kMilkdropTargetFps。
  const double projectMIntervalSec = 1.0 / (double) kMilkdropTargetFps;
  const bool skipProjectMRender =
      ! forceRender && (nowSec + 0.0005 < milkdrop_next_render_sec_);
  if (! skipProjectMRender) {
    milkdrop_next_render_sec_ += projectMIntervalSec;
    if (milkdrop_next_render_sec_ < nowSec)
      milkdrop_next_render_sec_ = nowSec + projectMIntervalSec;   // 落后太多：重置相位
  }

  // ================================================================
  if (use_fbo_api) {
    // 主路径：projectM → offscreen FBO（openglRenderFrameFbo 接受 FBO ID）
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, milkdrop_render_fbo_);
    juce::gl::glViewport(0, 0, fbo_w, fbo_h);
    juce::gl::glScissor(0, 0, fbo_w, fbo_h);
    juce::gl::glEnable(juce::gl::GL_SCISSOR_TEST);
    if (! skipProjectMRender) {
      juce::gl::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      juce::gl::glClear(juce::gl::GL_COLOR_BUFFER_BIT);
      api.setWindowSize(milkdrop_pm_handle_,
                        static_cast<std::size_t>(fbo_w),
                        static_cast<std::size_t>(fbo_h));
      Y2K_DIAG_PHASE ("gl:projectM renderFrameFbo");
      api.openglRenderFrameFbo(milkdrop_pm_handle_, milkdrop_render_fbo_);
      milkdrop_last_render_sec_ = nowSec;
    }
    juce::gl::glDisable(juce::gl::GL_SCISSOR_TEST);
  } else if (! skipProjectMRender) {
    // 降级路径：projectM → FBO 0 (0,0)（openglRenderFrame 内部绑定 FBO 0）
    //   Win 性能优化：Windows 随包的 projectM-4.dll 没有 projectm_opengl_render_frame_fbo，
    //   一直走本路径。此前降级路径"每个 GL 帧都真渲染"（88fps 下渲染 88 次/秒、每次 ~3ms），
    //   现与主路径一样按 kMilkdropTargetFps 节流：跳过的帧 render FBO 保留上次内容
    //   （blit 目标是离屏 FBO，不受 FBO 0 被 JUCE 覆盖影响），视觉无变化。
    milkdrop_last_render_sec_ = nowSec;
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

    // FBO 0 (0,0) → offscreen FBO。
    // ⚠ 不用 glBlitFramebuffer：外网 RTX 3070 / NVIDIA 595.79 现场实测，
    //   "默认后缓冲 → 纹理 FBO" 的 blit 会被驱动拒绝（GL_INVALID_OPERATION 0x502），
    //   离屏 FBO 于是一直是分配时的空内容 → 模块纯黑，而 ready/timer/md 耗时全正常。
    //   glCopyTexSubImage2D 是另一条拷贝路径（GL 1.1 起就有），同为 1:1 拷屏，且
    //   对默认帧缓冲的多重采样会自动 resolve，不会碰 blit 的那些限制。
    juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, 0);
    juce::gl::glBindFramebuffer(juce::gl::GL_DRAW_FRAMEBUFFER,
                                 milkdrop_render_fbo_);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, milkdrop_render_tex_);
    juce::gl::glCopyTexSubImage2D(juce::gl::GL_TEXTURE_2D, 0,
                                  0, 0,          // 目标偏移（纹理内）
                                  0, 0,          // 源左下角（后缓冲内）
                                  fbo_w, fbo_h);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
  }

  // 诊断：离屏渲染这一步（projectM 渲染 + 降级路径的 FBO 0 → 离屏 FBO 复制）。
  //   降级路径的 blit 若被驱动拒绝（0x502），离屏 FBO 永远停在"分配时的空内容"，
  //   而画面所有健康指标（ready / timer / md 耗时 / gl fps）都是正常的 —— 这正是
  //   "纯黑但看不出异常"的典型现场。这里把它显式记下来。
  if (Y2K_DIAG_ENABLED())
  {
    if (skipProjectMRender)
      ++gMdDiag.skipped;
    else
      ++gMdDiag.rendered;

    const GLenum e = juce::gl::glGetError();
    if (e != juce::gl::GL_NO_ERROR)
    {
      ++gMdDiag.blitErr;
      // 限流：前 10 次全记，之后每 300 次记一次，避免刷爆日志
      if (gMdDiag.blitErr <= 10 || (gMdDiag.blitErr % 300) == 0)
      {
        Y2K_DIAG ("MD", juce::String ("!! offscreen stage GL error 0x")
                              + juce::String::toHexString ((int) e)
                              + " path=" + (use_fbo_api ? "renderFrameFbo" : "renderToFBO0+blit")
                              + " fbo=" + juce::String (fbo_w) + "x" + juce::String (fbo_h)
                              + " err#=" + juce::String (gMdDiag.blitErr));
      }
    }
  }

  // ================================================================
  // 整体染色后处理（master output colors）：projectM → render FBO → post FBO → 各模块
  // ================================================================
  const auto visual_state = GetMilkdropVisualState();
  const bool tint_active = !visual_state.isNeutral();

  // v2.9.9：后处理与 projectM 渲染同节流。
  //   · projectM 本帧被 60fps 节流跳过（skipProjectMRender）时，render FBO 内容
  //     与上一帧逐像素相同，后处理结果也相同 → 直接复用上次的 post FBO，
  //     省掉一次全屏采样 pass（120/144Hz 屏上相当于减半）。
  //   · 视觉状态变化（拖滑块 / 切效果，版本号自增）必须立即重算，保证预览跟手。
  //   · 降级路径（无 openglRenderFrameFbo）Win 性能优化 起同样节流。
  const bool projectMRenderedThisFrame = ! skipProjectMRender;
  const uint32_t visualVersion = milkdrop_visual_version_.load (std::memory_order_relaxed);

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
      milkdrop_post_valid_ = false;   // 新分配的 FBO 内容无效
    }

    // 后处理：采样 render FBO 纹理 → post FBO，统一 pass 应用完整视觉状态。
    const bool needTintPass =
        milkdrop_tint_pass_ != nullptr && milkdrop_tint_pass_->isReady()
        && (! milkdrop_post_valid_
            || projectMRenderedThisFrame
            || visualVersion != milkdrop_tint_applied_version_);

    if (needTintPass) {
      Y2K_DIAG_PHASE ("gl:milkdrop tint pass");
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, milkdrop_post_fbo_);
      juce::gl::glViewport(0, 0, fbo_w, fbo_h);

      // 先清空错误队列：apply() 内部靠"画完查错"判断这一帧有没有真的落笔，
      //   不能让 projectM / 前面步骤残留的错误污染判定。
      while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}

      const bool tintOk = milkdrop_tint_pass_->apply(milkdrop_render_tex_, visual_state);
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);

      // 只有真的画成功才把 post FBO 当"有效画面"。
      //   旧实现无条件置 true：一旦 tint pass 没落笔，post FBO 里就是分配时的空内容，
      //   而 blit 会把它当成当前帧 → 整块纯黑，且没有任何环节会自愈。
      milkdrop_post_valid_ = tintOk;
      if (tintOk) {
        milkdrop_tint_applied_version_ = visualVersion;
      } else if (Y2K_DIAG_ENABLED()) {
        static int sTintFailLogs = 0;
        if (++sTintFailLogs <= 5 || (sTintFailLogs % 300) == 0)
          Y2K_DIAG ("MD", juce::String ("!! tint pass 未画成 → 回退到 projectM 原始输出 fail#=")
                                + juce::String (sTintFailLogs));
      }
    }

    if (milkdrop_post_valid_)
      blit_source_fbo = milkdrop_post_fbo_;
  }
  else
  {
    // 回到中性：post FBO 内容不再代表当前画面，下次启用时必须重算。
    milkdrop_post_valid_ = false;
  }

  // ================================================================
  // 诊断：内容自检 + 5s 汇总
  //   ① 采样 blit 源 FBO 的亮度 —— 直接回答"离屏 FBO 里到底是不是黑的"；
  //   ② 5s 一行汇总渲染/跳过/错误计数，和 [PAINT]/[STATS] 对齐看时序。
  //   采样会同步等 GPU，因此只在诊断构建（Y2K_ENABLE_LOGGING）里编译。
  // ================================================================
  if (Y2K_DIAG_ENABLED())
  {
    gMdDiag.lastFboW = fbo_w;
    gMdDiag.lastFboH = fbo_h;
    gMdDiag.lastTintActive = tint_active;
    gMdDiag.lastUseFboApi  = use_fbo_api;

   #if defined (Y2K_ENABLE_LOGGING) || (defined (Y2K_DIAG_LOG_DEFAULT) && Y2K_DIAG_LOG_DEFAULT)
    {
      const double nowMs = juce::Time::getMillisecondCounterHiRes();
      if (nowMs - gMdDiag.lastProbeMs >= 2000.0)
      {
        gMdDiag.lastProbeMs = nowMs;
        gMdDiag.lastProbe   = MilkdropProbeFbo (blit_source_fbo, fbo_w, fbo_h);
      }
    }
   #endif

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (gMdDiag.lastReportMs <= 0.0)
      gMdDiag.lastReportMs = nowMs;

    if (nowMs - gMdDiag.lastReportMs >= 5000.0)
    {
      const double dt = juce::jmax (0.001, (nowMs - gMdDiag.lastReportMs) / 1000.0);
      const int dRender   = gMdDiag.rendered         - gMdDiag.reportRendered;
      const int dSkip     = gMdDiag.skipped          - gMdDiag.reportSkipped;
      const int dBlitErr  = gMdDiag.blitErr          - gMdDiag.reportBlitErr;
      const int dScreen   = gMdDiag.screenBlit       - gMdDiag.reportScreenBlit;
      const int dScreenErr= gMdDiag.screenBlitErr    - gMdDiag.reportScreenBlitErr;

      Y2K_DIAG ("MD", juce::String ("5s | path=")
                            + (use_fbo_api ? "fboAPI" : "fallback(fbo0+blit)")
                            + " tint=" + (tint_active ? "1" : "0")
                            + " fbo=" + juce::String (fbo_w) + "x" + juce::String (fbo_h)
                            + " src=" + juce::String ((int) blit_source_fbo)
                            + " | render " + juce::String ((int) std::lround (dRender / dt)) + "/s"
                            + " skip " + juce::String ((int) std::lround (dSkip / dt)) + "/s"
                            + " | offErr " + juce::String (dBlitErr)
                            + " | screenBlit " + juce::String ((int) std::lround (dScreen / dt)) + "/s"
                            + " screenErr " + juce::String (dScreenErr)
                            + " | " + gMdDiag.lastProbe
                            + " | alloc=" + juce::String (gMdDiag.allocCount));

      // 关键判定：屏幕 blit 在跑、但源 FBO 采样几乎全黑 → 内容侧（①②③）的问题；
      //   采样有内容、屏幕仍黑 → 合成侧（④：矩形/GL 状态/组件层覆盖）的问题。
      if (gMdDiag.lastProbe.contains ("lumaMax=0")
          || gMdDiag.lastProbe.contains ("lumaMax=1 ")
          || gMdDiag.lastProbe.contains ("lumaMax=2 "))
      {
        Y2K_DIAG ("MD", y2k::diag::u8 ("!! 源 FBO 采样接近全黑（")
                              + gMdDiag.lastProbe
                              + y2k::diag::u8 ("）→ 内容侧问题：无预设 / 渲染被跳过 / blit 被丢弃"));
      }

      gMdDiag.reportRendered         = gMdDiag.rendered;
      gMdDiag.reportSkipped          = gMdDiag.skipped;
      gMdDiag.reportBlitErr          = gMdDiag.blitErr;
      gMdDiag.reportScreenBlit       = gMdDiag.screenBlit;
      gMdDiag.reportScreenBlitErr    = gMdDiag.screenBlitErr;
      gMdDiag.lastReportMs           = nowMs;
    }
  }

  // ---- 黑帧自愈：判定需要时，本帧立刻改回 projectM 原始输出 ----
  if (milkdropTickBlackFrameSelfHeal (blit_source_fbo, fbo_w, fbo_h)
      && blit_source_fbo != milkdrop_render_fbo_)
  {
    blit_source_fbo = milkdrop_render_fbo_;
  }

  return blit_source_fbo;
}

// ================================================================
// 黑帧自愈
//   现场症状："启动后 Milkdrop 纯黑，删掉模块重加 / 切一次布局就恢复"。
//   这说明黑不是"每帧都在算错"，而是某个状态被固化了（离屏 FBO 没拿到数据、
//   染色 post FBO 是空内容、预设根本没加载成功……）。与其让用户手动重建模块，
//   不如按阶梯自动重建一次。
//   安全阀（避免误伤"本来就很暗"的预设）：
//     · 只在 projectM 就绪 + 有有效预设 + 收到过真实 PCM 时动作（排除静音黑屏）；
//     · 判定用 maxLuma == 0（144 个采样点全黑），不是"很暗"；
//     · 连续 3 次采样（≈3s）才动手，且最多 6s 一次；
//     · 画面一旦有内容立刻清零，不会反复折腾。
//   恢复阶梯：① 作废染色 post FBO → ② 重载当前预设 → ③ 重建离屏 FBO（循环）。
// ================================================================
bool Y2KmeterAudioProcessorEditor::milkdropTickBlackFrameSelfHeal (unsigned int blitSourceFbo,
                                                                  int fboW, int fboH)
{
  const double nowMs = juce::Time::getMillisecondCounterHiRes();

  // 采样限流 1Hz：glReadPixels 会同步等 GPU，不能每帧做。
  if (nowMs - milkdrop_lastLumaProbeMs_ < 1000.0)
    return false;
  milkdrop_lastLumaProbeMs_ = nowMs;

  const bool havePreset = milkdrop_current_preset_ >= 0
                          && milkdrop_current_preset_ < MilkdropPresetCount();

  if (! havePreset || ! milkdrop_has_ever_received_pcm_
      || blitSourceFbo == 0 || fboW <= 0 || fboH <= 0)
  {
    milkdrop_blackStreak_ = 0;
    return false;
  }

  const auto sample = MilkdropSampleLuma (blitSourceFbo, fboW, fboH);
  if (! sample.valid)
    return false;

  if (sample.maxLuma > 0)
  {
    // 有内容 → 一切正常，复位阶梯
    milkdrop_blackStreak_   = 0;
    milkdrop_selfHealStage_ = 0;
    return false;
  }

  ++milkdrop_blackStreak_;
  if (milkdrop_blackStreak_ < 3)          // ≈3s 连续全黑才动手
    return false;

  if (nowMs - milkdrop_lastHealMs_ < 6000.0)   // 限流：最多 6s 一次
    return false;

  milkdrop_lastHealMs_  = nowMs;
  milkdrop_blackStreak_ = 0;
  ++milkdrop_selfHealCount_;

  const juce::String tail = " idx=" + juce::String (milkdrop_current_preset_)
                          + " fbo=" + juce::String ((int) blitSourceFbo)
                          + " " + juce::String (fboW) + "x" + juce::String (fboH)
                          + " heal#=" + juce::String (milkdrop_selfHealCount_);

  switch (milkdrop_selfHealStage_)
  {
    case 0:
      // ① 染色 pass 没落笔是"整块黑"的常见来源：作废 post FBO，立即改回原始输出
      milkdrop_post_valid_ = false;
      milkdrop_tint_applied_version_ = 0xFFFFFFFFu;
      milkdrop_selfHealStage_ = 1;
      Y2K_DIAGC ("MD", y2k::diag::u8 ("!! 源 FBO 全黑 → 自愈① 作废染色后处理") + tail);
      return true;

    case 1:
      // ② 重载当前预设（等价于用户手动切一次预设）
      LoadMilkdropPresetInternal();
      milkdrop_post_valid_ = false;
      milkdrop_last_preset_switch_ms_.store (
          static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count()));
      milkdrop_selfHealStage_ = 2;
      Y2K_DIAGC ("MD", y2k::diag::u8 ("!! 源 FBO 仍全黑 → 自愈② 重载当前预设") + tail);
      return true;

    default:
      // ③ 重建离屏 FBO：把尺寸标记清零，下一帧 need_realloc 会重新分配
      //    （等价于"删掉模块重新加"，但不动模块/布局）
      milkdrop_post_valid_ = false;
      milkdrop_last_fbo_w_ = 0;
      milkdrop_last_fbo_h_ = 0;
      milkdrop_selfHealStage_ = 0;
      Y2K_DIAGC ("MD", y2k::diag::u8 ("!! 源 FBO 依旧全黑 → 自愈③ 重建离屏 FBO") + tail);
      return true;
  }
}

// ================================================================
// 消费 Milkdrop 预设/库切换请求（GL 线程专用，无 MessageManager::Lock 依赖）
//   · Windows 上由 renderOpenGLPreLock() 在锁外优先消费，避免 50~150ms 的
//     磁盘读与 shader 编译阻塞消息线程；
//   · macOS 或恰好在 preLock 之后到达的请求由 renderOpenGL() 兜底消费。
// ================================================================
bool Y2KmeterAudioProcessorEditor::processPendingMilkdropPresetRequests()
{
  bool anySwitched = false;

  // 消费收藏库切换请求：在扫描生命周期锁内停掉在途异步扫描并完成同步重扫，
  // 消除 Message 线程并发 startMilkdropPresetScanAsync 与 GL 线程同步重扫之间的窗口竞态。
  if (milkdrop_requested_library_toggle_.exchange(false)) {
    const bool now_like = milkdrop_use_like_library_.load();
    if (now_like)
      milkdrop_builtin_preset_index_ = milkdrop_current_preset_;
    else
      milkdrop_like_preset_index_ = milkdrop_current_preset_;

    {
      std::lock_guard<std::mutex> lock (milkdrop_scan_lifecycle_mutex_);
      milkdrop_scan_cancel_.store (true, std::memory_order_release);
      milkdrop_scan_generation_.fetch_add (1, std::memory_order_acq_rel);
      if (milkdrop_scan_thread_.joinable())
        milkdrop_scan_thread_.join();
      milkdrop_scan_in_flight_.store (false, std::memory_order_release);
      milkdrop_startup_jump_fallback_ = -1;

      RescanMilkdropPresetPaths();
      milkdrop_presets_ready_.store (true, std::memory_order_release);
      milkdrop_needs_reconcile_.store (false, std::memory_order_release);
    }

    int target = now_like ? milkdrop_like_preset_index_
                          : milkdrop_builtin_preset_index_;
    if (target < 0 || target >= MilkdropPresetCount())
      target = 0;
    milkdrop_current_preset_ = (MilkdropPresetCount() == 0) ? -1 : target;

    if (milkdrop_current_preset_ >= 0) {
      LoadMilkdropPresetInternal();
      milkdrop_current_preset_ui_.store(milkdrop_current_preset_);
      milkdrop_last_preset_switch_ms_.store(
          static_cast<int64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count()));
      anySwitched = true;
    }
  }

  // 消费取消收藏触发的重扫请求：同上，在扫描生命周期锁内停掉异步扫描并完成同步重扫（此时位于 preLock 锁外）。
  if (milkdrop_requested_rescan_.exchange(false)) {
    {
      std::lock_guard<std::mutex> lock (milkdrop_scan_lifecycle_mutex_);
      milkdrop_scan_cancel_.store (true, std::memory_order_release);
      milkdrop_scan_generation_.fetch_add (1, std::memory_order_acq_rel);
      if (milkdrop_scan_thread_.joinable())
        milkdrop_scan_thread_.join();
      milkdrop_scan_in_flight_.store (false, std::memory_order_release);
      milkdrop_startup_jump_fallback_ = -1;

      RescanMilkdropPresetPaths();
      milkdrop_presets_ready_.store (true, std::memory_order_release);
      milkdrop_needs_reconcile_.store (false, std::memory_order_release);
    }

    if (MilkdropPresetCount() == 0) {
      SetMilkdropUseLikeLibrary(false);
      {
        std::lock_guard<std::mutex> lock (milkdrop_scan_lifecycle_mutex_);
        milkdrop_scan_cancel_.store (true, std::memory_order_release);
        milkdrop_scan_generation_.fetch_add (1, std::memory_order_acq_rel);
        if (milkdrop_scan_thread_.joinable())
          milkdrop_scan_thread_.join();
        milkdrop_scan_in_flight_.store (false, std::memory_order_release);

        RescanMilkdropPresetPaths();
        milkdrop_presets_ready_.store (true, std::memory_order_release);
        milkdrop_needs_reconcile_.store (false, std::memory_order_release);
      }
      int target = milkdrop_builtin_preset_index_;
      if (target < 0 || target >= MilkdropPresetCount())
        target = 0;
      milkdrop_current_preset_ = (MilkdropPresetCount() == 0) ? -1 : target;
    } else if (milkdrop_current_preset_ >= MilkdropPresetCount()) {
      milkdrop_current_preset_ = 0;
    }

    if (milkdrop_current_preset_ >= 0) {
      LoadMilkdropPresetInternal();
      milkdrop_current_preset_ui_.store(milkdrop_current_preset_);
      milkdrop_last_preset_switch_ms_.store(
          static_cast<int64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count()));
      anySwitched = true;
    }
  }

  // 后台 9,921 预设扫描仍在途中（<80ms）：保留 jump/delta/random 原子不消费，等扫描就绪后一并精确对账
  if (! milkdrop_presets_ready_.load (std::memory_order_acquire))
    return anySwitched;

  // 后台扫描刚刚完成：将首帧快速加载的预设路径对账到完整有序表中的真实索引，
  // 并一并消费在途期间积累的 jump/delta/random 请求。
  if (milkdrop_needs_reconcile_.exchange (false, std::memory_order_acq_rel)) {
    const int  userJump     = milkdrop_requested_preset_jump_.exchange (-1);
    const int  delta        = milkdrop_requested_preset_delta_.exchange (0);
    const bool random       = milkdrop_requested_preset_random_.exchange (false);
    const int  fallbackJump = milkdrop_startup_jump_fallback_;
    milkdrop_startup_jump_fallback_ = -1;

    // 仅当用户在扫描期间没有发过任何 jump/delta/random 时，才用启动保存索引兜底，
    // 防止旧 startup jump 吞掉用户的 Next/Prev/Random 点击。
    const int effectiveJump = (userJump >= 0)
                                  ? userJump
                                  : ((! random && delta == 0) ? fallbackJump : -1);

    std::vector<std::string> sortedUtf8;
    std::string loadedUtf8;
    {
      const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
      sortedUtf8.reserve ((size_t) milkdrop_preset_paths_.size());
      for (const auto& s : milkdrop_preset_paths_)
        sortedUtf8.push_back (s.toStdString());
      loadedUtf8 = milkdrop_loaded_preset_path_.toStdString();
    }

    const std::size_t seed = sortedUtf8.empty()
                                 ? 0u
                                 : (std::size_t) juce::Random::getSystemRandom().nextInt (
                                       juce::jmax (1, (int) sortedUtf8.size()));
    const auto rec = y2k::milkdrop::reconcileLoadedPresetIndex (
        sortedUtf8, loadedUtf8, effectiveJump, delta, random, seed);

    milkdrop_current_preset_ = rec.index;
    milkdrop_current_preset_ui_.store (rec.index);

    if (rec.shouldReload && rec.index >= 0) {
      LoadMilkdropPresetInternal();
      milkdrop_last_preset_switch_ms_.store (
          static_cast<int64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count()));
      anySwitched = true;
    }
    return anySwitched;
  }

  // 稳态消费预设切换请求
  const int  jump_idx = milkdrop_requested_preset_jump_.exchange(-1);
  const int  delta    = milkdrop_requested_preset_delta_.exchange(0);
  const bool random   = milkdrop_requested_preset_random_.exchange(false);
  bool switched = false;

  if (jump_idx >= 0 && jump_idx < MilkdropPresetCount()) {
    milkdrop_current_preset_ = jump_idx;
    switched = true;
  } else if (random) {
    const int n = MilkdropPresetCount();
    if (n <= 1) {
      if (milkdrop_current_preset_ < 0 || milkdrop_current_preset_ >= n)
        milkdrop_current_preset_ = 0;
      switched = true;
    } else {
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
  } else if (delta != 0 && MilkdropPresetCount() > 0) {
    milkdrop_current_preset_ = y2k::milkdrop::wrapPresetIndex (
        milkdrop_current_preset_, delta, MilkdropPresetCount());
    switched = true;
  }

  if (switched) {
    LoadMilkdropPresetInternal();
    milkdrop_current_preset_ui_.store(milkdrop_current_preset_);
    milkdrop_last_preset_switch_ms_.store(
        static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
    anySwitched = true;
  }

  return anySwitched;
}

// ================================================================
// JUCE 补丁 renderOpenGLPreLock()（仅 Windows 调用）：未持消息锁时提前完成
//   projectM 按需懒初始化 + 预设切换 + 渲染 + 染色后处理。原先这 ~3ms（切预设时可达 128ms）
//   在持锁的 renderOpenGL() 里执行，整段时间消息线程被迫停转；挪到锁外后
//   两者并行，帧周期不再是"消息线程耗时 + GL 持锁耗时"的串行叠加。
//   FBO 尺寸取自上一帧持锁阶段的测量结果（milkdrop_pre_w_/h_）；
//   首帧或尺寸变化时由 renderOpenGL() 持锁阶段补渲染。
// ================================================================
void Y2KmeterAudioProcessorEditor::renderOpenGLPreLock()
{
  // 工作区运行时首次添加 Milkdrop 模块时，在锁外完成 projectM 懒初始化（零消息线程阻塞）
  if (! milkdrop_render_ready_.load (std::memory_order_acquire)
      && milkdrop_pm_handle_ == nullptr
      && ! milkdrop_renderer_suspended_.load (std::memory_order_acquire)
      && milkdrop_has_module_in_editor_.load (std::memory_order_acquire)
      && milkdrop_gl_context_ready_.load (std::memory_order_acquire))
  {
    initMilkdropEditorResources();
  }

  if (! milkdrop_render_ready_.load() || milkdrop_pm_handle_ == nullptr)
    return;

  y2k::diag::setThreadTag ("GL");
  y2k::diag::setPhase ("Editor::renderOpenGLPreLock");

  const double t0 = y2k::diag::isEnabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;

  const bool presetSwitched = processPendingMilkdropPresetRequests();

  if (milkdrop_pre_w_ > 0 && milkdrop_pre_h_ > 0)
  {
    milkdrop_pre_blit_fbo_ = renderMilkdropFrameCore (milkdrop_pre_w_, milkdrop_pre_h_, presetSwitched);
    milkdrop_pre_done_ = true;
    juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);
  }

  // 诊断：必须在返回前查一次 GL 错误。
  //   JUCE 补丁 0006 在本函数返回后立刻 clearGLError()（避免旧错误拖到下一帧），
  //   prelock 阶段产生的 0x502 会被它整段抹掉 —— 这正是"日志里一个 GL 错误都
  //   没有、画面却全黑"的原因。这里先自己看一眼再放行。
  if (Y2K_DIAG_ENABLED())
  {
    const GLenum e = juce::gl::glGetError();
    if (e != juce::gl::GL_NO_ERROR)
    {
      ++gMdDiag.blitErr;
      if (gMdDiag.blitErr <= 10 || (gMdDiag.blitErr % 300) == 0)
        Y2K_DIAG ("MD", juce::String ("!! prelock GL error 0x")
                              + juce::String::toHexString ((int) e)
                              + " fbo=" + juce::String (milkdrop_pre_w_) + "x"
                              + juce::String (milkdrop_pre_h_)
                              + " src=" + juce::String ((int) milkdrop_pre_blit_fbo_)
                              + " err#=" + juce::String (gMdDiag.blitErr));
    }
  }

  if (t0 > 0.0)
    milkdrop_pre_ms_ += juce::Time::getMillisecondCounterHiRes() - t0;
}

// ================================================================
// JUCE 补丁 0010 renderOpenGLPostComponentBuffer()：
//   在 drawComponentBuffer() 合成完所有 JUCE 组件层之后、已释放 MessageManager::Lock、
//   紧贴 SwapBuffers() 之前执行。用于单次 draw call 批渲染全部 ToyBox 粒子与光晕，
//   零消息线程持锁阻塞、零 ToyOverlay 组件重绘放大。
// ================================================================
void Y2KmeterAudioProcessorEditor::renderOpenGLPostComponentBuffer()
{
  if (toyDirector == nullptr || glToyRenderer_ == nullptr)
    return;

  if (! toyDirector->isGlToyActive() || ! toyDirector->hasActiveGlToyFrame())
    return;

  if (! glToyRenderer_->render (*toyDirector, openGLContext))
    toyDirector->setGlToyHostActive (false);
}

void Y2KmeterAudioProcessorEditor::renderOpenGL() {
  using namespace juce::gl;

  // ---- 诊断：GL 帧入口 ----
  //   · heartbeat 让看门狗知道"GL 线程还在出帧"；
  //   · 后续每个大步骤都会 setPhase，卡死时日志最后一行就是卡住的位置。
  y2k::diag::setThreadTag ("GL");
  y2k::diag::setPhase ("Editor::renderOpenGL");
  y2k::diag::heartbeat (y2k::diag::Channel::GlFrame);

  const bool diagOn = y2k::diag::isEnabled();
  const double diagFrameStartMs = diagOn ? juce::Time::getMillisecondCounterHiRes() : 0.0;
  double diagMilkdropMs = 0.0, diagDirectMs = 0.0;

  // 诊断：帧间隔异常（500ms 以上但还没到看门狗 5s 阈值）也要留痕 ——
  //   现场常见的不是"彻底死住"，而是"卡 1~3 秒后恢复"，这种最容易被漏掉。
  if (diagOn)
  {
      static double s_lastGlFrameMs = 0.0;
      if (s_lastGlFrameMs > 0.0)
      {
          const double gapMs = diagFrameStartMs - s_lastGlFrameMs;
          if (gapMs > 500.0)
              Y2K_DIAG ("GL", "frame gap " + juce::String (gapMs, 0) + "ms");
      }
      s_lastGlFrameMs = diagFrameStartMs;
  }

  // v2.9：真实出帧计数（GL 线程每出一帧调用一次 renderOpenGL）
  glFrameCounter.fetch_add(1, std::memory_order_relaxed);

  // v2.9.3：GL 帧耗时统计（诊断构建）
  Y2K_PERF_FRAME_SCOPE("glFrame");

  // 保存原始 viewport 和 DPI 缩放（Milkdrop 和 Spectrogram3D 共用）
  GLint orig_viewport[4];
  juce::gl::glGetIntegerv(juce::gl::GL_VIEWPORT, orig_viewport);

  float dpi_scale = static_cast<float>(openGLContext.getRenderingScale());
  int   editor_h  = getHeight();

  // macOS 兜底（不走 renderOpenGLPreLock）：有嵌入态 Milkdrop 时按需懒初始化 projectM
  if (! milkdrop_render_ready_.load (std::memory_order_acquire)
      && milkdrop_pm_handle_ == nullptr
      && ! milkdrop_renderer_suspended_.load (std::memory_order_acquire)
      && milkdrop_has_module_in_editor_.load (std::memory_order_acquire)
      && milkdrop_gl_context_ready_.load (std::memory_order_acquire))
  {
    initMilkdropEditorResources();
  }

  if (milkdrop_render_ready_.load() && milkdrop_pm_handle_ != nullptr) {

  static bool editorMilkdropRenderLogged = false;
  if (! editorMilkdropRenderLogged)
  {
      editorMilkdropRenderLogged = true;
      Y2K_LOG("[EditorGL] Milkdrop render path active (FBO blit)");
  }

  auto& api = projectm_api::Api::instance();

  // 兜底消费预设切换请求（Windows 上已在 renderOpenGLPreLock 锁外消费，此处为快路径空操作；
  //   macOS 或恰好在 preLock 之后到达的请求在此即时生效）
  const bool latePresetSwitch = processPendingMilkdropPresetRequests();
  if (latePresetSwitch)
    milkdrop_pre_done_ = false;

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
            if (auto* content = dynamic_cast<MilkdropModule*>(wnd->getHostedModule()))
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


  // Win 性能优化：projectM 渲染 + 染色后处理优先复用 renderOpenGLPreLock() 在锁外完成的结果；
  //   首帧 / FBO 尺寸变化 / 非 Windows 平台则在这里同步渲染。
  //   预渲染过但尺寸变了时必须强制渲染（否则 60fps 节流会让新 FBO 空一帧）。
  GLuint blit_source_fbo = 0;
  {
    const bool reusePre = milkdrop_pre_done_
                          && fbo_w == milkdrop_pre_w_ && fbo_h == milkdrop_pre_h_;
    if (reusePre)
      blit_source_fbo = milkdrop_pre_blit_fbo_;
    else
      blit_source_fbo = renderMilkdropFrameCore (fbo_w, fbo_h, milkdrop_pre_done_ || latePresetSwitch);
    milkdrop_pre_done_ = false;
    milkdrop_pre_w_ = fbo_w;
    milkdrop_pre_h_ = fbo_h;
  }


  // ================================================================
  // v2.9.3：GL 直渲染 z 序统一遍历（Milkdrop blit + S3D 条带按组件树
  //   顺序绘制，重叠区域由 z 序决定遮挡 —— 聚焦模块覆盖另一个）
  // ================================================================
  if (diagOn)
    diagMilkdropMs = juce::Time::getMillisecondCounterHiRes() - diagFrameStartMs;

  // 透明覆层模式：在 projectM 离屏渲染（含 FBO 0 降级路径）完成后、直渲染模块与
  //   JUCE drawComponentBuffer 合成前，把 FBO 0 整幅清成 (0,0,0,0)，确保空背景真透明。
  if (PinkXP::isTransparentBackdrop())
  {
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (orig_viewport[0], orig_viewport[1], orig_viewport[2], orig_viewport[3]);
    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);
  }

  y2k::diag::setPhase ("Editor::renderOpenGL -> zOrder walk");
  renderGlDirectModulesZOrdered(dpi_scale, editor_h,
                               (int)(editor_h * dpi_scale),
                               true, blit_source_fbo);

  if (diagOn)
    diagDirectMs = juce::Time::getMillisecondCounterHiRes() - diagFrameStartMs - diagMilkdropMs;

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
      // 诊断：浮动窗口共享帧锁（UI 线程读帧、GL 线程写帧）
      Y2K_DIAG_PHASE ("gl:wait milkdrop_shared_frame_mutex_");
      std::lock_guard<std::mutex> lock(milkdrop_shared_frame_mutex_);
      Y2K_DIAG_PHASE ("gl:floating milkdrop capture");

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
      //   注意：glReadPixels 会强制等待 GPU 完成，是整条 GL 链路上最容易
      //   "看起来卡死"的调用（驱动差 / 独显切换 / 软件渲染时尤其明显）。
      Y2K_DIAG_PHASE ("gl:ReadPixels (floating milkdrop capture)");
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

    juce::Component::SafePointer<Y2KmeterAudioProcessorEditor> safeEd (this);
    juce::MessageManager::callAsync([safeEd]() {
        auto* ed = safeEd.getComponent();
        if (ed == nullptr || ed->editorBeingDestructed)
            return;
        for (auto& wnd : ed->floatingWindows_) {
            if (wnd != nullptr) {
                if (auto* milk = dynamic_cast<MilkdropModule*>(wnd->getHostedModule())) {
                    milk->repaint();
                }
            }
        }
    });
  }

  juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, 0);

  } // if Milkdrop ready
  else
  {
    // Milkdrop 未就绪/挂起：仍要画 S3D（glDirectActive 已接管的模块不能变透明）
    if (diagOn)
      diagMilkdropMs = juce::Time::getMillisecondCounterHiRes() - diagFrameStartMs;

    // 透明覆层模式：整幅清空 FBO 0 为 (0,0,0,0)，保证直渲染模块与组件层之下纯透明
    if (PinkXP::isTransparentBackdrop())
    {
      glBindFramebuffer (GL_FRAMEBUFFER, 0);
      glViewport (orig_viewport[0], orig_viewport[1], orig_viewport[2], orig_viewport[3]);
      glDisable (GL_SCISSOR_TEST);
      glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
      glClear (GL_COLOR_BUFFER_BIT);
    }

    y2k::diag::setPhase ("Editor::renderOpenGL -> zOrder walk (no milkdrop)");
    renderGlDirectModulesZOrdered(dpi_scale, editor_h,
                                  (int)(editor_h * dpi_scale),
                                  false, 0);

    if (diagOn)
      diagDirectMs = juce::Time::getMillisecondCounterHiRes() - diagFrameStartMs - diagMilkdropMs;
  }

  // 恢复原始 viewport，供 JUCE CPE 渲染子组件 CachedImage
  juce::gl::glViewport(orig_viewport[0], orig_viewport[1],
                        orig_viewport[2], orig_viewport[3]);

  // ---- 诊断：帧统计 + 状态镜像 + GL 错误码 ----
  //   注意：glGetError 每帧只查一次，避免逐模块查询带来的驱动往返开销。
  if (diagOn)
  {
    const double diagPreMs = milkdrop_pre_ms_;   // 锁外预渲染耗时也计入本帧统计
    milkdrop_pre_ms_ = 0.0;
    const double totalMs = juce::Time::getMillisecondCounterHiRes() - diagFrameStartMs + diagPreMs;
    y2k::diag::addFrameSample (totalMs, diagMilkdropMs + diagPreMs, diagDirectMs);

    // 诊断埋点（GL）：单帧耗时超过阈值时即时落一行（带毫秒时间戳）。与 UIPOPUP
    //   的"点击 → 弹菜单"时间戳对齐后，可判断点击后的卡顿是否夹在一条慢 GL 帧
    //   之间（即 GL 线程持消息锁渲染整帧、把 callAsync 的弹窗排队到帧后）。
    if (totalMs > 25.0)
        Y2K_DIAG ("GL", "slow frame total=" + juce::String (totalMs, 1)
                        + "ms milkdrop=" + juce::String (diagMilkdropMs, 1)
                        + "ms direct=" + juce::String (diagDirectMs, 1));

    y2k::diag::setEditorState (getWidth(), getHeight(),
                               (float) openGLContext.getRenderingScale(),
                               openGLContext.isAttached(),
                               true,
                               milkdrop_render_ready_.load(),
                               (int) getNumChildComponents(),
                               glFrameCounter.load (std::memory_order_relaxed));

    static int  s_glErrorCount = 0;      // 本会话累计（限流用）
    static bool s_glErrorActive = false; // 上一帧是否处于报错状态

    const GLenum glErr = juce::gl::glGetError();
    if (glErr != juce::gl::GL_NO_ERROR)
    {
      s_glErrorActive = true;
      if (s_glErrorCount < 40)
      {
        ++s_glErrorCount;
        Y2K_DIAG ("GL", "glGetError=0x" + juce::String::toHexString ((int) glErr)
                              + " (occurrence " + juce::String (s_glErrorCount) + ")");
      }
    }
    else if (s_glErrorActive)
    {
      s_glErrorActive = false;
      Y2K_DIAG ("GL", "glGetError clean again");
    }

    y2k::diag::setPhase ("Editor::renderOpenGL done");
  }
}

// ================================================================
// v2.9.3 / v3.5 GL 直渲染（z 序统一遍历）
//   复用共享 GlDirectRenderer（与 FloatingModuleWindow 100% 同源管线）：
//     · Milkdrop 模块 → projectM 帧 blit；
//     · GlDirectModuleBase 模块 → 统一通过 glDirectRenderer_->renderModule(...) 绘制。
// ================================================================
void Y2KmeterAudioProcessorEditor::renderGlDirectModulesZOrdered (float dpiScale, int editorLogicalH,
                                                                  int editorPhysH,
                                                                  bool milkdropReady,
                                                                  unsigned int milkdropBlitSourceFbo)
{
  using namespace juce::gl;

  if (glDirectRenderer_ == nullptr)
    glDirectRenderer_ = std::make_unique<GlDirectRenderer>();

  const bool s3dOk  = glDirectRenderer_->ensureInitialised (openGLContext);
  const bool milkOk = milkdropReady;

  if (! s3dOk && ! milkOk)
    return;

  const int vw = juce::jmax (1, juce::roundToInt ((float) getWidth() * dpiScale));
  const int vh = juce::jmax (1, editorPhysH);
  const bool isTransparentBackdrop = PinkXP::isTransparentBackdrop();

  const auto frameGuard = glDirectRenderer_->beginFrame (vw, vh);

  // 统一 z 序遍历：组件树先序（兄弟按索引序）即 z 从低到高，
  //   重叠区域后画的覆盖先画的 —— 聚焦（toFront）模块自然在上层。
  //   Milkdrop（FBO 渲染 + blit）与 GlDirectModuleBase（共享 GlDirectRenderer）在此同权参与。
  std::vector<juce::uint64> liveGlKeys;
  liveGlKeys.reserve (24);

  auto walk = [&] (auto& self, juce::Component* root) -> void
  {
    for (int i = 0; i < root->getNumChildComponents(); ++i)
    {
      auto* child = root->getChildComponent (i);
      if (child == nullptr)
        continue;

      if (auto* milk = dynamic_cast<MilkdropModule*> (child))
      {
        if (! milkOk && ! milkdrop_renderer_suspended_.load (std::memory_order_acquire))
          milkdrop_has_module_in_editor_.store (true, std::memory_order_release);
        if (milkOk && milk->isVisible() && milk->isShowing())
        {
          auto cl = milk->GetContentLocalBounds();
          auto tl = getLocalPoint (milk, cl.getPosition());
          auto br = getLocalPoint (milk, cl.getBottomRight());
          int lw = br.x - tl.x, lh = br.y - tl.y;
          if (lw > 0 && lh > 0)
          {
            const double fx0 = (double) tl.x * dpiScale;
            const double fx1 = (double) br.x * dpiScale;
            const double fy0 = (double) (editorLogicalH - br.y) * dpiScale;
            const double fy1 = (double) (editorLogicalH - tl.y) * dpiScale;

            int gx = (int) std::floor (fx0);
            int gy = (int) std::floor (fy0);
            int gw = juce::jmax (1, (int) std::ceil (fx1) - gx);
            int gh = juce::jmax (1, (int) std::ceil (fy1) - gy);

            int sw = std::max (gw / milkdrop_render_scale_, 1);
            int sh = std::max (gh / milkdrop_render_scale_, 1);

            Y2K_DIAG_PHASE ("gl:BlitFramebuffer (milkdrop -> screen)");
            glBindFramebuffer (GL_READ_FRAMEBUFFER, milkdropBlitSourceFbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer (0, 0, sw, sh,
                               gx, gy, gx + gw, gy + gh,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
            if (isTransparentBackdrop)
            {
              glBindFramebuffer (GL_FRAMEBUFFER, 0);
              glViewport (0, 0, vw, vh);
              glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
              glEnable (GL_SCISSOR_TEST);
              glScissor (gx, gy, gw, gh);
              glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
              glClear (GL_COLOR_BUFFER_BIT);
              glDisable (GL_SCISSOR_TEST);
              glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }

            if (Y2K_DIAG_ENABLED())
            {
              ++gMdDiag.screenBlit;

              const juce::Rectangle<int> dstRect (gx, gy, gw, gh);
              if (dstRect != gMdDiag.lastScreenRect
                  || milkdropBlitSourceFbo != gMdDiag.lastScreenSrcFbo)
              {
                gMdDiag.lastScreenRect   = dstRect;
                gMdDiag.lastScreenSrcFbo = milkdropBlitSourceFbo;
                Y2K_DIAG ("MD", juce::String ("screen blit src=")
                                      + juce::String ((int) milkdropBlitSourceFbo)
                                      + " srcRect=0,0," + juce::String (sw) + "x" + juce::String (sh)
                                      + " dst=" + juce::String (gx) + "," + juce::String (gy)
                                      + " " + juce::String (gw) + "x" + juce::String (gh)
                                      + " | content=" + juce::String (lw) + "x" + juce::String (lh)
                                      + " viewport=" + juce::String (vw) + "x" + juce::String (vh)
                                      + " dpi=" + juce::String (dpiScale, 2));
              }

              const GLenum blitErr = glGetError();
              if (blitErr != GL_NO_ERROR)
              {
                ++gMdDiag.screenBlitErr;
                if (gMdDiag.screenBlitErr <= 10 || (gMdDiag.screenBlitErr % 300) == 0)
                  Y2K_DIAG ("MD", juce::String ("!! screen blit GL error 0x")
                                        + juce::String::toHexString ((int) blitErr)
                                        + " src=" + juce::String ((int) milkdropBlitSourceFbo)
                                        + " srcRect=" + juce::String (sw) + "x" + juce::String (sh)
                                        + " dst=" + juce::String (gx) + "," + juce::String (gy)
                                        + " " + juce::String (gw) + "x" + juce::String (gh)
                                        + " err#=" + juce::String (gMdDiag.screenBlitErr));
              }
            }
          }
        }
      }
      else if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (child))
      {
        liveGlKeys.push_back (gmod->glResourceKey);
        if (s3dOk && gmod->isVisible() && gmod->isShowing())
          glDirectRenderer_->renderModule (*this, *gmod, dpiScale, editorLogicalH, editorPhysH, vw, vh);
        else
          gmod->diagSetVisible (false);
      }
      else
      {
        self (self, child);
      }
    }
  };
  walk (walk, this);

  glDirectRenderer_->retainOnlyKeys (liveGlKeys.data(), liveGlKeys.size());
  glDirectRenderer_->endFrame (frameGuard);
}

void Y2KmeterAudioProcessorEditor::destroyMilkdropEditorResources() {
  // 只销毁 Editor 侧 Milkdrop 渲染资源。**不要**在这里动 GlDirectModule 直渲染
  // 模块的 VBO/FBO/shader——Milkdrop 脱离时 Editor GL 上下文仍然 attach、直渲染
  // 模块仍在渲染，误删会导致它们 glDirectActive 外壳透明但 GL 内容空白（残影）。
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
  {
    const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
    milkdrop_loaded_preset_path_.clear();
  }
  milkdrop_current_preset_ = 0;
  milkdrop_current_preset_ui_.store(-1);
  milkdrop_startup_jump_fallback_ = -1;
  milkdrop_last_fbo_w_ = 0;
  milkdrop_last_fbo_h_ = 0;
  milkdrop_render_ready_.store (false, std::memory_order_release);
}

void Y2KmeterAudioProcessorEditor::openGLContextClosing() {
  // GL 上下文真正关闭（Editor 析构/重建）时才走这里：清理直渲染资源 + milkdrop。
  y2k::diag::setThreadTag ("GL");
  y2k::diag::setPhase ("Editor::openGLContextClosing");
  Y2K_DIAGC ("GL", "openGLContextClosing enter (释放 GL 资源)");
  milkdrop_gl_context_ready_.store (false, std::memory_order_release);

  if (glToyRenderer_ != nullptr)
    glToyRenderer_->releaseResources();

  if (toyDirector != nullptr)
    toyDirector->setGlToyHostActive (false);

  if (glDirectRenderer_ != nullptr)
    glDirectRenderer_->releaseAll();

  {
    auto resetUploadedGen = [&] (auto& self, juce::Component* root) -> void
    {
      for (int i = 0; i < root->getNumChildComponents(); ++i)
      {
        auto* child = root->getChildComponent (i);
        if (child == nullptr) continue;
        if (auto* gmod = dynamic_cast<GlDirectModuleBase*> (child))
        {
          gmod->glTexUploadedGen.store (0, std::memory_order_release);
        }
        else
        {
          self (self, child);
        }
      }
    };
    resetUploadedGen (resetUploadedGen, this);
  }

  destroyMilkdropEditorResources();
}

void Y2KmeterAudioProcessorEditor::SuspendMilkdropEditorRendererForFloating() {
  // 先置挂起标志：即便 Editor GL 上下文尚未 attach、Editor handle 尚未创建，
  // 也要阻止后续异步触发的 newOpenGLContextCreated 再创建 Editor handle，
  // 避免与 GLView 的本地 projectM handle 共存导致 GLEW 全局状态互相干扰。
  milkdrop_renderer_suspended_ = true;

  if (openGLContext.isAttached()) {
    openGLContext.executeOnGLThread([this](juce::OpenGLContext&) {
      // 只销毁 Editor 侧 Milkdrop 资源；直渲染模块的 VBO/FBO/shader 必须保留
      // （它们仍在 Editor GL 上下文里渲染，误删会导致残影/卡死）。
      destroyMilkdropEditorResources();
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
  milkdrop_has_module_in_editor_.store (true, std::memory_order_release);

  if (editorBeingDestructed || !openGLContext.isAttached()) return;

  openGLContext.executeOnGLThread([this](juce::OpenGLContext&) {
    if (!milkdrop_render_ready_.load() && milkdrop_pm_handle_ == nullptr)
      initMilkdropEditorResources();
  }, false);
}

// ----------------------------------------------------------
// RequestGlFrame —— 只驱动 GL 出帧，不标脏组件层
//   · 嵌入态 Milkdrop 的内容由 Editor GL 渲染，而 Editor 的 OpenGLContext
//     是 setContinuousRepainting(false)：没有组件脏区就不出帧。
//     整个界面只剩 Milkdrop 一块时，组件层唯一的重绘来源是 GLView 的
//     30Hz 兜底 Timer → FPS 标签被钉在 30（外网反馈）。
//   · triggerRepaint() 会置 pendingRender，GL 线程照常跑 renderOpenGL；
//     组件层没有脏区时 paintComponent 的 invalid 为空，不会重新光栅化，
//     所以这里比 repaint()（整块内容区重绘）便宜得多。
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::RequestGlFrame()
{
  if (editorBeingDestructed || !openGLContext.isAttached())
    return;

  // 全平台（macOS 补丁 0005/0008 + 非 macOS 补丁 0010）：与组件重绘合并到下一次
  //   显示器刷新（macOS CVDisplayLink / Win performFrameDispatch flush）统一出帧。
  openGLContext.triggerRepaintPacedToDisplay();
}

void Y2KmeterAudioProcessorEditor::RescanMilkdropPresetPaths() {
  const bool useLike = milkdrop_use_like_library_.load();
  auto presetsDir = useLike
                        ? FindMilkdropLikeDir()
                        : FindMilkdropAssetsDir("milkdrop_presets");
  Y2K_LOG("[Rescan] useLike=" + juce::String(useLike ? 1 : 0)
          + " dir=" + presetsDir.getFullPathName()
          + " exists=" + juce::String(presetsDir.exists() ? 1 : 0));

  // 磁盘 IO 全部在锁外完成；最后一次性 swap 进成员表。
  //   旧写法是"先 clear 再逐个 add"，而消息线程此刻可能正在读这张表
  //   （预设名 / 总数 / 收藏按钮）→ 读到"清空到一半"的 StringArray。
  juce::StringArray newPaths;
  if (presetsDir.isDirectory()) {
    newPaths.ensureStorageAllocated (useLike ? 128 : 10240);
    for (const auto& entry : juce::RangedDirectoryIterator (
             presetsDir, false, "*.milk", juce::File::findFiles))
      newPaths.add (entry.getFile().getFullPathName());
    newPaths.sort(false);
  }

  int count = 0;
  {
    const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
    milkdrop_preset_paths_.swapWith (newPaths);
    milkdrop_scanned_use_like_ = useLike;
    count = milkdrop_preset_paths_.size();
  }

  // 诊断：扫的是哪个目录、扫到几个预设。
  //   presets=0 时后续 LoadMilkdropPresetInternal 会早退（projectM 无预设 → 全黑），
  //   这是"启动就纯黑、且只有重建模块/重启才会变"的第一嫌疑人。
  Y2K_DIAG ("MD", juce::String ("rescan useLike=")
                        + (useLike ? "1" : "0")
                        + " dir=" + presetsDir.getFullPathName()
                        + " exists=" + (presetsDir.exists() ? "1" : "0")
                        + " count=" + juce::String (count)
                        + (count == 0
                               ? y2k::diag::u8 ("  << 空库！后续会渲染纯黑")
                               : juce::String()));
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
  // UI 线程：翻转收藏库状态，然后入队异步切换请求，由 GL 线程消费
  // （重扫 + 恢复到切换后库之前记录的预设）。
  ToggleMilkdropLibraryState();
  milkdrop_requested_library_toggle_.store(true);
  RequestGlFrame();
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropUnlinkReload() {
  // UI 线程：取消收藏后入队重扫请求，由 GL 线程消费。
  milkdrop_requested_rescan_.store(true);
  RequestGlFrame();
}

bool Y2KmeterAudioProcessorEditor::HasMilkdropLikedPresets() const {
  juce::File likeDir = FindMilkdropLikeDir();
  if (!likeDir.isDirectory())
    return false;
  return likeDir.findChildFiles(juce::File::findFiles, false, "*.milk").size() > 0;
}

juce::String Y2KmeterAudioProcessorEditor::GetMilkdropCurrentPresetFilePath() const {
  // UI 线程：索引取原子镜像，路径走加锁访问器（GL 线程可能正在重扫）。
  // 若异步全量扫描尚未落盘，兜底返回首帧快速加载的预设路径。
  const juce::String p = MilkdropPresetPathAt (milkdrop_current_preset_ui_.load());
  if (p.isNotEmpty())
    return p;
  const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
  return milkdrop_loaded_preset_path_;
}

bool Y2KmeterAudioProcessorEditor::loadMilkdropPresetPathInternal (
    const juce::String& path, int displayIdx, int displayTotal) {
  const bool dOn = Y2K_DIAG_ENABLED();

  if (milkdrop_pm_handle_ == nullptr) {
    if (dOn)
      Y2K_DIAG ("MD", "loadPreset SKIP: handle=null（projectM 未创建）");
    return false;
  }
  if (path.isEmpty()) {
    if (dOn)
      Y2K_DIAG ("MD", juce::String ("loadPreset SKIP: path empty idx=")
                            + juce::String (displayIdx));
    return false;
  }

  auto& api = projectm_api::Api::instance();
  if (api.hasLoadPresetData()) {
    juce::File f(path);
    if (f.existsAsFile()) {
      auto data = f.loadFileAsString().toStdString();
      const auto rawBytes = data.size();
      // 注入 wave 样式覆盖（wave_mode/x/y/r/g/b/a/...），启用时才生效。
      data = ApplyWaveParamsToPresetText(data, GetMilkdropWaveState());
      if (dOn)
        Y2K_DIAG ("MD", juce::String ("loadPreset data idx=")
                              + juce::String (displayIdx) + "/"
                              + juce::String (displayTotal)
                              + " bytes=" + juce::String ((int) rawBytes)
                              + " injected=" + juce::String ((int) data.size() - (int) rawBytes)
                              + " file=" + f.getFileName());
      api.loadPresetData(milkdrop_pm_handle_, data, true);
      {
        const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
        milkdrop_loaded_preset_path_ = path;
      }
      return true;
    } else if (dOn) {
      // 文件不见了：projectM 保持旧预设（或黑），但界面上仍然显示这个名字
      Y2K_DIAG ("MD", juce::String ("loadPreset SKIP: file missing → ") + path);
    }
    return false;
  }

  if (dOn)
    Y2K_DIAG ("MD", juce::String ("loadPreset file idx=")
                          + juce::String (displayIdx) + "/"
                          + juce::String (displayTotal)
                          + " file=" + path.fromLastOccurrenceOf("/", false, false));
  api.loadPresetFile(milkdrop_pm_handle_, path.toRawUTF8(), true);
  {
    const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
    milkdrop_loaded_preset_path_ = path;
  }
  return true;
}

void Y2KmeterAudioProcessorEditor::LoadMilkdropPresetInternal() {
  const bool dOn = Y2K_DIAG_ENABLED();

  if (milkdrop_pm_handle_ == nullptr) {
    if (dOn)
      Y2K_DIAG ("MD", "loadPreset SKIP: handle=null（projectM 未创建）");
    return;
  }
  const int total = MilkdropPresetCount();
  if (milkdrop_current_preset_ < 0 || milkdrop_current_preset_ >= total) {
    if (dOn)
      Y2K_DIAG ("MD", juce::String ("loadPreset SKIP: idx=")
                            + juce::String (milkdrop_current_preset_)
                            + " total=" + juce::String (total)
                            + " likeLib=" + (milkdrop_use_like_library_.load() ? "1" : "0")
                            + y2k::diag::u8 (" → 无有效预设（projectM 会渲染纯黑且不会自愈）"));
    return;
  }

  const juce::String path = MilkdropPresetPathAt (milkdrop_current_preset_);
  loadMilkdropPresetPathInternal (path, milkdrop_current_preset_, total);
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
  RequestGlFrame();
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropPresetJump(int index) {
  milkdrop_requested_preset_jump_.store(index);
  RequestGlFrame();
}

void Y2KmeterAudioProcessorEditor::RequestMilkdropPresetRandom() {
  milkdrop_requested_preset_random_.store(true);
  RequestGlFrame();
}

juce::String Y2KmeterAudioProcessorEditor::GetMilkdropError() const {
  // UI 线程读、GL 线程写 → 加锁取一份拷贝
  const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
  return milkdrop_error_;
}

// ---- 预设表的加锁访问器（UI 线程与 GL 线程都可调用）----

int Y2KmeterAudioProcessorEditor::MilkdropPresetCount() const {
  const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
  return milkdrop_preset_paths_.size();
}

juce::String Y2KmeterAudioProcessorEditor::MilkdropPresetPathAt (int index) const {
  const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
  if (index < 0 || index >= milkdrop_preset_paths_.size())
    return {};
  return milkdrop_preset_paths_[index];
}

void Y2KmeterAudioProcessorEditor::SetMilkdropError (const std::string& e) {
  const std::lock_guard<std::mutex> lock (milkdrop_state_mutex_);
  milkdrop_error_ = e;
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
  return MilkdropPresetCount();
}

juce::String Y2KmeterAudioProcessorEditor::GetMilkdropCurrentPresetName()
    const {
  const juce::String path = GetMilkdropCurrentPresetFilePath();
  if (path.isEmpty())
    return {};
  return path.fromLastOccurrenceOf("/", false, false)
             .fromLastOccurrenceOf("\\", false, false)
             .upToLastOccurrenceOf(".milk", false, false);
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
  // 通知 GL 线程：状态变了，下一帧必须重算后处理（否则会被 60fps 节流跳过）。
  milkdrop_visual_version_.fetch_add(1, std::memory_order_relaxed);
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
