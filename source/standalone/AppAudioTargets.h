// ==========================================================
// AppAudioTargets.h
//   "定向程序音频采集"（Application Audio Capture）的目标枚举 / 解析。
//
//   与 OBS 的 Application Audio Capture 同思路：
//     · 采集走 WASAPI 进程级回环（见 WasapiLoopbackCapture 的 setTargetProcess）；
//     · 这里只负责回答两个问题：
//         1) 现在有哪些程序可以选？   → listCapturableApps()
//            （全量列表，每项带 isActive：此刻正在出声的进主列表，其余进 "All Apps"）
//         2) 某个 exe 现在该采哪个 PID？→ resolveTargetProcessId()
//
//   目标用 **exe 文件名**（如 "chrome.exe"）标识而不是 PID：
//     · PID 每次启动都变，exe 名可以跨重启持久化（settings 里存 "app:chrome.exe"）；
//     · 多进程程序（Chrome / Electron：声音由子进程发出）统一解析到**进程树根**，
//       配合 PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE 把整棵树的声音收全。
//
//   仅 Windows 有实现；其他平台 isProcessLoopbackSupported() 恒为 false、列表恒为空。
// ==========================================================

#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>

namespace y2k::appaudio
{

struct AppAudioTarget
{
    juce::String exeName;                 // 稳定键："chrome.exe"（比较时不区分大小写）
    juce::String displayName;             // 给用户看的名字："Google Chrome"（取不到则为 exe 去扩展名）
    bool         isActive = false;        // 此刻正在出声（有 Active 状态的音频会话）
};

// 系统是否支持进程级回环采集（Windows 10 2004 / build 19041 起）。
bool isProcessLoopbackSupported();

// 列出当前可选的程序（全量），每项用 isActive 标出"此刻是否正在出声"：
//   · isActive = 在任一活动播放设备上有 **Active** 状态的音频会话
//     （AudioSessionStateActive：至少有一条流正在运行）。上层把这些放进主列表。
//   · 其余 = 有会话但没在出声的程序 + 只有可见顶层窗口、还没碰过音频的程序。
//     上层把它们收进 "All Apps" 子菜单（暂停中的播放器、只发提示音的程序在这里选）。
//   · 为什么主列表不用"有未过期的会话"：会话一旦建立，只要进程还活着就一直是
//     Inactive 而不会过期 —— 实测把流全部释放后，直到进程退出才收到 Expired。
//     所以 QQ / Discord / Steam 这类开过一次音频、此后一直没声音的程序会永远挂在
//     "有会话"的名单里，那不叫有音频活动。
//   · 按名字排序；不含本进程与"系统声音"。
// 需要在已初始化 COM 的线程调用（JUCE 消息线程满足）。
juce::Array<AppAudioTarget> listCapturableApps();

// 把 exe 文件名解析成当前应当采集的 PID（进程树根）；程序没在运行则返回 0。
//   同名多实例时优先选"自己或子进程正在发声"的那一个。
// 需要在已初始化 COM 的线程调用（采集线程满足）。
std::uint32_t resolveTargetProcessId (const juce::String& exeName);

// 指定 PID 是否仍是那个 exe 的存活进程（同时比对 exe 名，防 PID 被别的程序复用）。
//   采集线程在拿不到进程句柄（受保护进程）时用它低频轮询目标是否退出。
bool isProcessRunning (std::uint32_t pid, const juce::String& exeName);

// 程序没在运行、拿不到版本信息时的显示名回退："chrome.exe" → "chrome"
juce::String fallbackDisplayName (const juce::String& exeName);

} // namespace y2k::appaudio
