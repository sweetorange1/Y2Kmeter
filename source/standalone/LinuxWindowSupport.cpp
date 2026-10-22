// ==========================================================
// LinuxWindowSupport.cpp
//   通过 hyprctl IPC 控制本进程窗口的浮动 / 置顶 / 层级。
//
//   实现要点：
//     · `hyprctl -j clients` 里按 pid 定位自己的窗口 address，
//       避免"改到别的窗口"或依赖焦点。
//     · `hyprctl version -j` 的 tag（如 v0.56.2）决定 dispatch 语法：
//         >= 0.55 → Lua:  hyprctl dispatch 'hl.dsp.window.float{...}'
//         <  0.55 → 旧语法: hyprctl dispatch setfloating /
//                            hyprctl dispatch focuswindow address:0x...
//     · hyprctl 输出通过 juce::ChildProcess 读取，超时/失败一律返回 false，
//       调用方按"没有桌面集成"降级处理（窗口仍可正常使用，只是可能被平铺）。
// ==========================================================

#include <JuceHeader.h>

#include "source/standalone/LinuxWindowSupport.h"

#if JUCE_LINUX

#include <cstdlib>
#include <unistd.h>
#include <cmath>

namespace y2k
{
namespace linuxwm
{

namespace
{
    // 跑一次 hyprctl，返回 stdout（失败返回空串）。
    //   不用 juce::ChildProcess：它的 readProcessOutput 走 fdopen/fread，
    //   在本机实测会拿不完整输出（clients JSON 只剩 1~2 个对象，导致按 pid
    //   找不到窗口）。这里直接用 popen + 循环 fread，简单可靠。
    juce::String runHyprctl (const juce::StringArray& args, int /*timeoutMs*/ = 1500)
    {
        // 参数全部是内部常量 / pid / 0x 地址，无用户输入，单引号包裹即可
        juce::String cmd = "hyprctl";
        for (const auto& a : args)
            cmd << " '" << a.replace ("'", "'\\''") << "'";

        auto* pipe = ::popen (cmd.toRawUTF8(), "r");
        if (pipe == nullptr)
            return {};

        juce::String out;
        char buffer[4096];
        size_t n = 0;
        while ((n = ::fread (buffer, 1, sizeof (buffer), pipe)) > 0)
            out += juce::String::fromUTF8 (buffer, (int) n);

        const int status = ::pclose (pipe);
        juce::ignoreUnused (status);
        return out;
    }

    // 解析 `hyprctl version -j` 的 tag，返回 (major, minor)。解析失败返回 (-1, -1)。
    juce::Point<int> queryHyprlandVersion()
    {
        const auto out = runHyprctl ({ "-j", "version" });
        if (out.isEmpty())
            return { -1, -1 };

        const auto json = juce::JSON::parse (out);
        if (auto* obj = json.getDynamicObject())
        {
            auto tag = obj->getProperty ("tag").toString().trim();
            if (tag.startsWithChar ('v') || tag.startsWithChar ('V'))
                tag = tag.substring (1);

            const auto parts = juce::StringArray::fromTokens (tag, ".", "");
            if (parts.size() >= 2)
                return { parts[0].getIntValue(), parts[1].getIntValue() };
        }

        return { -1, -1 };
    }

    bool isLuaDispatchAvailable()
    {
        const auto v = queryHyprlandVersion();
        if (v.x < 0)
            return true; // 版本查询失败时按新语法尝试（0.55+ 是当前主线）

        return v.x > 0 || (v.x == 0 && v.y >= 55);
    }

    // 在 hyprctl clients 里按 pid 找窗口 address（形如 "0x563f..."）。
    juce::String findWindowAddressByPid (int pid)
    {
        const auto out = runHyprctl ({ "-j", "clients" }, 2500);
        if (out.isEmpty())
            return {};

        const auto json = juce::JSON::parse (out);
        auto* arr = json.getArray();
        if (arr == nullptr)
            return {};

        for (const auto& item : *arr)
        {
            auto* obj = item.getDynamicObject();
            if (obj == nullptr)
                continue;

            if (obj->getProperty ("pid").toString().getLargeIntValue() != (juce::int64) pid)
                continue;

            const auto addr = obj->getProperty ("address").toString().trim();
            if (addr.isNotEmpty())
                return addr;
        }

        return {};
    }

    bool isPinned (int pid)
    {
        const auto out = runHyprctl ({ "-j", "clients" }, 2500);
        if (out.isEmpty())
            return false;

        const auto json = juce::JSON::parse (out);
        auto* arr = json.getArray();
        if (arr == nullptr)
            return false;

        for (const auto& item : *arr)
        {
            auto* obj = item.getDynamicObject();
            if (obj != nullptr && obj->getProperty ("pid").toString().getLargeIntValue() == (juce::int64) pid)
                return (bool) obj->getProperty ("pinned");
        }

        return false;
    }

    bool dispatch (const juce::String& expr)
    {
        // hyprctl dispatch 会把参数包成 hl.dispatch(<expr>)
        const auto out = runHyprctl ({ "dispatch", expr }, 2500);
        return ! out.containsIgnoreCase ("error");
    }

    bool focusOwnWindow (const juce::String& address)
    {
        if (isLuaDispatchAvailable())
            return dispatch ("hl.dsp.focus{window=\"address:" + address + "\"}");

        return dispatch ("focuswindow address:" + address);
    }
} // namespace

bool isHyprland()
{
    return juce::SystemStats::getEnvironmentVariable ("HYPRLAND_INSTANCE_SIGNATURE", {}).isNotEmpty();
}

bool ensureFloating (int pid, bool floating)
{
    if (! isHyprland())
        return false;

    if (pid <= 0)
        pid = (int) ::getpid();

    const auto address = findWindowAddressByPid (pid);
    if (address.isEmpty())
        return false;

    if (isLuaDispatchAvailable())
    {
        return dispatch (juce::String ("hl.dsp.window.float{action=\"")
                         + (floating ? "enable" : "disable")
                         + "\", window=\"address:" + address + "\"}");
    }

    if (! focusOwnWindow (address))
        return false;

    return dispatch (floating ? "setfloating" : "settiled");
}

bool setPinned (int pid, bool pinned)
{
    if (! isHyprland())
        return false;

    if (pid <= 0)
        pid = (int) ::getpid();

    const auto address = findWindowAddressByPid (pid);
    if (address.isEmpty())
        return false;

    // 已是目标状态就不重复 toggle（pin 在 Hyprland 是 toggle 语义）
    if (isPinned (pid) == pinned)
        return true;

    if (isLuaDispatchAvailable())
        return dispatch ("hl.dsp.window.pin{window=\"address:" + address + "\"}");

    if (! focusOwnWindow (address))
        return false;

    return dispatch ("pin");
}

bool raiseToTop (int pid)
{
    if (! isHyprland())
        return false;

    if (pid <= 0)
        pid = (int) ::getpid();

    const auto address = findWindowAddressByPid (pid);
    if (address.isEmpty())
        return false;

    if (isLuaDispatchAvailable())
        return dispatch ("hl.dsp.window.alter_zorder{mode=\"top\", window=\"address:" + address + "\"}");

    if (! focusOwnWindow (address))
        return false;

    return dispatch ("bringactivetotop");
}

bool moveWindowToX11Pixels (int pid, int xPixels, int yPixels, X11DisplayArea display)
{
    if (! isHyprland())
        return false;

    if (pid <= 0)
        pid = (int) ::getpid();

    // 找窗口 address 与所在 monitor id
    const auto clientsOut = runHyprctl ({ "-j", "clients" }, 2500);
    if (clientsOut.isEmpty())
        return false;

    juce::String address;
    int monitorId = -1;
    {
        const auto json = juce::JSON::parse (clientsOut);
        if (auto* arr = json.getArray())
        {
            for (const auto& item : *arr)
            {
                auto* obj = item.getDynamicObject();
                if (obj == nullptr || obj->getProperty ("pid").toString().getLargeIntValue() != (juce::int64) pid)
                    continue;
                address = obj->getProperty ("address").toString().trim();
                monitorId = (int) obj->getProperty ("monitor").toString().getIntValue();
                break;
            }
        }
    }

    if (address.isEmpty())
        return false;

    // 找该 monitor 的逻辑几何
    int monLogicalX = 0, monLogicalY = 0, monLogicalW = display.w;
    {
        const auto monOut = runHyprctl ({ "-j", "monitors" }, 2500);
        const auto json = juce::JSON::parse (monOut);
        if (auto* arr = json.getArray())
        {
            for (const auto& item : *arr)
            {
                auto* obj = item.getDynamicObject();
                if (obj == nullptr)
                    continue;
                if (monitorId >= 0 && (int) obj->getProperty ("id").toString().getIntValue() != monitorId)
                    continue;
                monLogicalX = (int) obj->getProperty ("x").toString().getIntValue();
                monLogicalY = (int) obj->getProperty ("y").toString().getIntValue();
                monLogicalW = (int) obj->getProperty ("width").toString().getIntValue();
                break;
            }
        }
    }

    // X11 像素 → Hyprland 逻辑坐标
    const double scale = (display.w > 0 && monLogicalW > 0)
                             ? (double) monLogicalW / (double) display.w
                             : 1.0;
    const int logicalX = (int) std::lround ((double) monLogicalX + ((double) (xPixels - display.x)) * scale);
    const int logicalY = (int) std::lround ((double) monLogicalY + ((double) (yPixels - display.y)) * scale);

    if (isLuaDispatchAvailable())
    {
        return dispatch (juce::String ("hl.dsp.window.move{x=") + juce::String (logicalX)
                         + ", y=" + juce::String (logicalY)
                         + ", window=\"address:" + address + "\"}");
    }

    // 旧语法：movewindowpixel exact X Y,address:0x...
    if (! focusOwnWindow (address))
        return false;

    return dispatch ("movewindowpixel exact " + juce::String (logicalX) + " "
                     + juce::String (logicalY) + ",address:" + address);
}

bool adoptMainWindow()
{
    if (! isHyprland())
        return false;

    bool any = ensureFloating (0, true);
    any = raiseToTop (0) || any;
    return any;
}

} // namespace linuxwm
} // namespace y2k

#else // ! JUCE_LINUX

namespace y2k
{
namespace linuxwm
{
bool isHyprland()                                  { return false; }
bool ensureFloating (int, bool)                    { return false; }
bool setPinned (int, bool)                         { return false; }
bool raiseToTop (int)                              { return false; }
bool adoptMainWindow()                             { return false; }
bool moveWindowToX11Pixels (int, int, int, X11DisplayArea) { return false; }
} // namespace linuxwm
} // namespace y2k

#endif // JUCE_LINUX
