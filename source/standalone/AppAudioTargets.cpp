// ==========================================================
// AppAudioTargets.cpp
//   定向程序音频采集的目标枚举 / 解析（说明见头文件）。
//
//   数据来源：
//     · 进程表      —— CreateToolhelp32Snapshot（pid / 父 pid / exe 名）
//     · 音频会话    —— IAudioSessionManager2::GetSessionEnumerator（所有活动渲染端点）
//     · 顶层窗口    —— EnumWindows（可见、有标题、非工具窗、无 owner）
//     · 显示名      —— exe 版本资源里的 FileDescription
// ==========================================================

#include "AppAudioTargets.h"

namespace y2k::appaudio
{
    juce::String fallbackDisplayName (const juce::String& exeName)
    {
        return exeName.endsWithIgnoreCase (".exe") ? exeName.dropLastCharacters (4) : exeName;
    }
}

#if JUCE_WINDOWS

#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "version.lib")

namespace y2k::appaudio
{

namespace
{
    using Microsoft::WRL::ComPtr;

    struct ProcEntry
    {
        DWORD        parentPid = 0;
        juce::String exeName;
    };

    using ProcTable = std::map<DWORD, ProcEntry>;

    ProcTable snapshotProcesses()
    {
        ProcTable table;

        HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return table;

        PROCESSENTRY32W pe {};
        pe.dwSize = sizeof (pe);
        if (Process32FirstW (snap, &pe))
        {
            do
            {
                ProcEntry e;
                e.parentPid = pe.th32ParentProcessID;
                e.exeName   = juce::String (pe.szExeFile);
                table[pe.th32ProcessID] = std::move (e);
            }
            while (Process32NextW (snap, &pe));
        }

        CloseHandle (snap);
        return table;
    }

    // 沿父进程链向上走到"同名进程树"的根：
    //   chrome.exe(子) → chrome.exe(子) → chrome.exe(浏览器主进程) → explorer.exe
    //                                     ^ 返回这个
    //   父进程已退出（查不到）或换了 exe 名就停。步数封顶防环（PID 复用的极端情况）。
    DWORD rootOfSameExe (const ProcTable& table, DWORD pid)
    {
        auto it = table.find (pid);
        if (it == table.end())
            return pid;

        const juce::String exe = it->second.exeName;

        for (int guard = 0; guard < 64; ++guard)
        {
            const DWORD parent = it->second.parentPid;
            if (parent == 0 || parent == pid)
                break;

            auto pit = table.find (parent);
            if (pit == table.end() || ! pit->second.exeName.equalsIgnoreCase (exe))
                break;

            pid = parent;
            it  = pit;
        }

        return pid;
    }

    struct SessionInfo
    {
        DWORD pid    = 0;
        bool  active = false;   // AudioSessionStateActive：此刻正在出声
    };

    // 枚举所有活动渲染端点上的音频会话（不含"系统声音"与已过期会话）。
    std::vector<SessionInfo> enumerateAudioSessions()
    {
        std::vector<SessionInfo> out;

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> devices;
        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS (enumerator.GetAddressOf())))
            || FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices)))
            return out;

        UINT deviceCount = 0;
        devices->GetCount (&deviceCount);

        for (UINT d = 0; d < deviceCount; ++d)
        {
            ComPtr<IMMDevice>               device;
            ComPtr<IAudioSessionManager2>   manager;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED (devices->Item (d, &device))
                || FAILED (device->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                             (void**) manager.GetAddressOf()))
                || FAILED (manager->GetSessionEnumerator (&sessions)))
                continue;

            int sessionCount = 0;
            sessions->GetCount (&sessionCount);

            for (int s = 0; s < sessionCount; ++s)
            {
                ComPtr<IAudioSessionControl>  control;
                ComPtr<IAudioSessionControl2> control2;
                if (FAILED (sessions->GetSession (s, &control)) || FAILED (control.As (&control2)))
                    continue;

                AudioSessionState state = AudioSessionStateExpired;
                control2->GetState (&state);

                DWORD pid = 0;
                // GetProcessId 对跨进程会话返回 AUDCLNT_S_NO_SINGLE_PROCESS（成功码），pid 仍有效
                const HRESULT hrPid = control2->GetProcessId (&pid);

                if (SUCCEEDED (hrPid)
                    && pid != 0
                    && state != AudioSessionStateExpired
                    && control2->IsSystemSoundsSession() != S_OK)
                {
                    out.push_back ({ pid, state == AudioSessionStateActive });
                }
            }
        }

        return out;
    }

    BOOL CALLBACK collectWindowPids (HWND hwnd, LPARAM lParam)
    {
        auto& pids = *reinterpret_cast<std::vector<DWORD>*> (lParam);

        if (! IsWindowVisible (hwnd))                         return TRUE;
        if (GetWindow (hwnd, GW_OWNER) != nullptr)            return TRUE;   // 对话框 / 弹出层
        if ((GetWindowLongPtrW (hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;

        // 跨进程 GetWindowText 读的是内核里缓存的标题，不会给对方发消息（对方卡死也不挂）
        wchar_t title[4] {};
        if (GetWindowTextW (hwnd, title, 4) <= 0)             return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId (hwnd, &pid);
        if (pid != 0)
            pids.push_back (pid);

        return TRUE;
    }

    // 只是"有窗口"而不可能是用户想采集的系统外壳进程
    bool isShellProcess (const juce::String& exeName)
    {
        static const juce::StringArray names
        {
            "explorer.exe",              // 桌面 / 任务栏 / 资源管理器
            "ApplicationFrameHost.exe",  // UWP 窗口外框（声音来自真正的应用进程，会话枚举能拿到）
            "TextInputHost.exe",
            "SystemSettings.exe",
            "ShellExperienceHost.exe",
            "SearchHost.exe",
            "StartMenuExperienceHost.exe",
        };

        return names.contains (exeName, true);
    }

    // 进程的完整 exe 路径；读不到返回空串
    juce::String queryImagePath (HANDLE process)
    {
        wchar_t path[1024] {};
        DWORD   pathLen = (DWORD) juce::numElementsInArray (path);
        return QueryFullProcessImageNameW (process, 0, path, &pathLen) ? juce::String (path) : juce::String();
    }

    // exe 版本资源里的 FileDescription（"Google Chrome" / "Spotify" ...）；取不到返回空串。
    //   · 提权 / 受保护进程 OpenProcess 会失败 → 由调用方回退到 exe 名。
    //   · 按完整路径缓存（含空结果）：下拉弹出时在 UI 主线程枚举数十个进程，
    //     避免每次打开都对每个 exe 重复走磁盘 GetFileVersionInfoW I/O。
    juce::String queryFileDescription (DWORD pid)
    {
        HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr)
            return {};

        const juce::String path = queryImagePath (process);
        CloseHandle (process);
        if (path.isEmpty())
            return {};

        const std::string cacheKey = path.toLowerCase().toStdString();
        static std::mutex cacheMutex;
        static std::unordered_map<std::string, juce::String> descCache;
        {
            std::lock_guard<std::mutex> guard (cacheMutex);
            if (auto it = descCache.find (cacheKey); it != descCache.end())
                return it->second;
        }

        auto storeCache = [&] (const juce::String& result) -> juce::String
        {
            std::lock_guard<std::mutex> guard (cacheMutex);
            if (descCache.size() >= 512)
                descCache.clear();
            descCache[cacheKey] = result;
            return result;
        };

        DWORD ignored = 0;
        const DWORD infoSize = GetFileVersionInfoSizeW (path.toWideCharPointer(), &ignored);
        if (infoSize == 0)
            return storeCache ({});

        std::vector<BYTE> info (infoSize);
        if (! GetFileVersionInfoW (path.toWideCharPointer(), 0, infoSize, info.data()))
            return storeCache ({});

        struct LangCodePage { WORD language; WORD codePage; };
        LangCodePage* translations = nullptr;
        UINT          translationBytes = 0;

        auto readDescription = [&info] (WORD language, WORD codePage) -> juce::String
        {
            wchar_t key[64] {};
            swprintf_s (key, L"\\StringFileInfo\\%04x%04x\\FileDescription", language, codePage);

            wchar_t* value = nullptr;
            UINT     valueLen = 0;
            if (VerQueryValueW (info.data(), key, (LPVOID*) &value, &valueLen)
                && value != nullptr && valueLen > 0)
                return juce::String (value).trim();
            return {};
        };

        if (VerQueryValueW (info.data(), L"\\VarFileInfo\\Translation",
                            (LPVOID*) &translations, &translationBytes)
            && translations != nullptr)
        {
            const UINT count = translationBytes / (UINT) sizeof (LangCodePage);
            for (UINT i = 0; i < count; ++i)
            {
                const auto text = readDescription (translations[i].language, translations[i].codePage);
                if (text.isNotEmpty())
                    return storeCache (text);
            }
        }

        // 没有 Translation 表的老程序：试一下最常见的 en-US / Unicode
        return storeCache (readDescription (0x0409, 0x04B0));
    }
} // namespace

bool isProcessLoopbackSupported()
{
    // 微软文档写的是 build 20348，但 OBS 实测 Windows 10 2004（19041）起就能用，这里跟随 OBS。
    //   GetVersionEx 会被 manifest 兼容层"骗"成旧版本，所以直接问 ntdll。
    static const bool supported = []
    {
        using RtlGetVersionFn = LONG (WINAPI*) (OSVERSIONINFOEXW*);

        HMODULE ntdll = GetModuleHandleW (L"ntdll.dll");
        if (ntdll == nullptr)
            return false;

        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn> (
            reinterpret_cast<void*> (GetProcAddress (ntdll, "RtlGetVersion")));
        if (rtlGetVersion == nullptr)
            return false;

        OSVERSIONINFOEXW info {};
        info.dwOSVersionInfoSize = sizeof (info);
        if (rtlGetVersion (&info) != 0)
            return false;

        return info.dwMajorVersion > 10
            || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 19041);
    }();

    return supported;
}

juce::Array<AppAudioTarget> listCapturableApps()
{
    juce::Array<AppAudioTarget> result;

    if (! isProcessLoopbackSupported())
        return result;

    const auto  table   = snapshotProcesses();
    const DWORD selfPid = GetCurrentProcessId();

    struct Found
    {
        AppAudioTarget target;
        DWORD          rootPid = 0;
    };
    std::vector<Found> found;

    // 音频会话 + 可见顶层窗口合并成一份全量列表，按 exe 名去重
    //   （一个程序可能在多个设备 / 多个子进程上各有会话，还可能同时有窗口）。
    auto addProcess = [&] (DWORD pid, bool active, bool fromWindow)
    {
        if (pid == 0 || pid == selfPid)
            return;

        const DWORD root = rootOfSameExe (table, pid);
        if (root == selfPid)
            return;

        const auto it = table.find (root);
        if (it == table.end() || it->second.exeName.isEmpty())
            return;   // 会话还在但进程已退出

        const auto& exe = it->second.exeName;
        if (fromWindow && isShellProcess (exe))
            return;

        for (auto& f : found)
        {
            if (f.target.exeName.equalsIgnoreCase (exe))
            {
                f.target.isActive = f.target.isActive || active;
                return;
            }
        }

        Found f;
        f.target.exeName  = exe;
        f.target.isActive = active;
        f.rootPid         = root;
        found.push_back (std::move (f));
    };

    for (const auto& session : enumerateAudioSessions())
        addProcess (session.pid, session.active, false);

    std::vector<DWORD> windowPids;
    EnumWindows (collectWindowPids, reinterpret_cast<LPARAM> (&windowPids));
    for (const DWORD pid : windowPids)
        addProcess (pid, false, true);

    for (auto& f : found)
    {
        f.target.displayName = queryFileDescription (f.rootPid);
        if (f.target.displayName.isEmpty())
            f.target.displayName = fallbackDisplayName (f.target.exeName);
    }

    // 同名消歧：不同 exe 共享同一 FileDescription 时（例如多个 Java / Electron 程序），
    //   附上括号里的 exe 名让用户能分辨；不撞名时保持清爽原名。
    std::unordered_map<std::string, int> displayNameCounts;
    for (const auto& f : found)
        ++displayNameCounts[f.target.displayName.toLowerCase().toStdString()];

    for (auto& f : found)
    {
        if (displayNameCounts[f.target.displayName.toLowerCase().toStdString()] > 1
            && ! f.target.displayName.containsIgnoreCase (f.target.exeName))
        {
            f.target.displayName << " (" << f.target.exeName << ")";
        }
    }

    std::sort (found.begin(), found.end(), [] (const Found& a, const Found& b)
    {
        return a.target.displayName.compareIgnoreCase (b.target.displayName) < 0;
    });

    for (auto& f : found)
        result.add (std::move (f.target));

    return result;
}

std::uint32_t resolveTargetProcessId (const juce::String& exeName)
{
    if (exeName.isEmpty())
        return 0;

    const auto table = snapshotProcesses();

    // 候选 = 该 exe 名下所有"进程树根"（一个程序开多份就有多个根）
    std::vector<DWORD> roots;
    for (const auto& [pid, entry] : table)
        if (entry.exeName.equalsIgnoreCase (exeName) && rootOfSameExe (table, pid) == pid)
            roots.push_back (pid);

    if (roots.empty())
        return 0;
    if (roots.size() == 1)
        return roots.front();

    // 多个实例：优先正在出声的，其次有会话的，最后退回第一个
    DWORD withSession = 0;
    for (const auto& session : enumerateAudioSessions())
    {
        const DWORD root = rootOfSameExe (table, session.pid);
        if (std::find (roots.begin(), roots.end(), root) == roots.end())
            continue;

        if (session.active)
            return root;
        if (withSession == 0)
            withSession = root;
    }

    return withSession != 0 ? withSession : roots.front();
}

bool isProcessRunning (std::uint32_t pid, const juce::String& exeName)
{
    HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (process == nullptr)
        // 拒绝访问 = 进程还在，只是不让查；其余错误（ERROR_INVALID_PARAMETER）= 这个 PID 已不存在
        return GetLastError() == ERROR_ACCESS_DENIED;

    DWORD exitCode = 0;
    bool running = GetExitCodeProcess (process, &exitCode) && exitCode == STILL_ACTIVE;

    if (running)
    {
        // 路径读得到就核对 exe 名（防 PID 被别的程序复用）；读不到则只认"进程还活着"
        const juce::String path = queryImagePath (process);
        running = path.isEmpty()
               || path.fromLastOccurrenceOf ("\\", false, false).equalsIgnoreCase (exeName);
    }

    CloseHandle (process);
    return running;
}

} // namespace y2k::appaudio

#else // !JUCE_WINDOWS

namespace y2k::appaudio
{
    bool isProcessLoopbackSupported()                              { return false; }
    juce::Array<AppAudioTarget> listCapturableApps()               { return {}; }
    std::uint32_t resolveTargetProcessId (const juce::String&)     { return 0; }
    bool isProcessRunning (std::uint32_t, const juce::String&)     { return false; }
}

#endif
