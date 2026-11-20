#include "source/system/SystemStats.h"

#if JUCE_WINDOWS

// ==========================================================
// SystemStatsWin.cpp —— Windows 后端（只用公开 API，无驱动）
//
//   · PDH（PdhAddEnglishCounterW）：CPU 总占用 / 每核 / 频率、磁盘活动与吞吐、
//     网络吞吐、GPU 引擎占用、GPU 显存占用、ACPI 热区温度
//   · DXGI：选出主 GPU（独显优先 = 专用显存最大）的名称 / LUID / 显存总量
//   · D3DKMT（gdi32 导出）：GPU 温度（与任务管理器"GPU 温度"同源，WDDM 2.4+）
//   · NVML（NVIDIA 驱动自带 nvml.dll，动态加载）：GPU 温度兜底
//   · HWiNFO 共享内存 / LibreHardwareMonitor & OpenHardwareMonitor WMI：
//     读取用户自己运行的监控工具已经采集好的温度（CPU 温度的主要来源）
//   · ACPI 热区（PDH Thermal Zone Information）：最后的 CPU 温度近似
//
//   所有调用都发生在 Sampler 的采集线程上，构造 / 析构也在同一线程。
// ==========================================================

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <wbemidl.h>
#include <oleauto.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")

namespace y2k::sys
{
namespace
{
    std::wstring toLower (std::wstring s)
    {
        std::transform (s.begin(), s.end(), s.begin(),
                        [] (wchar_t c) { return (wchar_t) std::towlower (c); });
        return s;
    }

    bool containsI (const std::wstring& hay, const wchar_t* needle)
    {
        return toLower (hay).find (toLower (needle)) != std::wstring::npos;
    }

    // HWiNFO 的字符串字段是定长 char 数组（ANSI 或 UTF-8，不保证以 0 结尾）：
    //   逐字节做 ASCII 大小写无关匹配，不经过 juce::String（避免非法 UTF-8 断言）
    bool containsAsciiI (const char* hay, size_t maxLen, const char* needle)
    {
        const size_t n = strnlen (hay, maxLen);
        const size_t m = strlen (needle);

        if (m == 0)
            return true;

        for (size_t i = 0; i + m <= n; ++i)
        {
            size_t j = 0;
            while (j < m && std::tolower ((unsigned char) hay[i + j]) == std::tolower ((unsigned char) needle[j]))
                ++j;
            if (j == m)
                return true;
        }
        return false;
    }

    template <size_t N>
    bool containsAsciiI (const char (&hay)[N], const char* needle)
    {
        return containsAsciiI (hay, N, needle);
    }

    float plausibleTemp (double c) noexcept
    {
        return (std::isfinite (c) && c > 1.0 && c < 150.0) ? (float) c
                                                           : std::numeric_limits<float>::quiet_NaN();
    }

    // ======================================================
    // PDH
    // ======================================================
    struct PdhItem
    {
        std::wstring name;
        double value = 0.0;
    };

    class PdhQuery
    {
    public:
        PdhQuery()
        {
            if (PdhOpenQueryW (nullptr, 0, &query) != ERROR_SUCCESS)
                query = nullptr;
        }

        ~PdhQuery()
        {
            if (query != nullptr)
                PdhCloseQuery (query);
        }

        PDH_HCOUNTER add (const wchar_t* path)
        {
            if (query == nullptr)
                return nullptr;

            PDH_HCOUNTER c = nullptr;
            if (PdhAddEnglishCounterW (query, path, 0, &c) != ERROR_SUCCESS)
                return nullptr;
            return c;
        }

        bool collect()
        {
            return query != nullptr && PdhCollectQueryData (query) == ERROR_SUCCESS;
        }

        static bool readDouble (PDH_HCOUNTER c, double& out)
        {
            if (c == nullptr)
                return false;

            PDH_FMT_COUNTERVALUE v {};
            if (PdhGetFormattedCounterValue (c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS)
                return false;
            if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA)
                return false;

            out = v.doubleValue;
            return std::isfinite (out);
        }

        bool readArray (PDH_HCOUNTER c, std::vector<PdhItem>& out)
        {
            out.clear();
            if (c == nullptr)
                return false;

            // 通配符计数器的实例数会变化（进程启停）：PDH_MORE_DATA 时重试
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                DWORD size = 0, count = 0;
                PDH_STATUS st = PdhGetFormattedCounterArrayW (c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                                              &size, &count, nullptr);
                if (st != (PDH_STATUS) PDH_MORE_DATA || size == 0)
                    return false;

                scratch.resize ((size_t) size + 256);
                size = (DWORD) scratch.size();
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*> (scratch.data());

                st = PdhGetFormattedCounterArrayW (c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                                   &size, &count, items);
                if (st == (PDH_STATUS) PDH_MORE_DATA)
                    continue;
                if (st != ERROR_SUCCESS)
                    return false;

                for (DWORD i = 0; i < count; ++i)
                {
                    const auto& fv = items[i].FmtValue;
                    if (fv.CStatus != PDH_CSTATUS_VALID_DATA && fv.CStatus != PDH_CSTATUS_NEW_DATA)
                        continue;
                    if (! std::isfinite (fv.doubleValue))
                        continue;
                    out.push_back ({ items[i].szName != nullptr ? items[i].szName : L"", fv.doubleValue });
                }
                return true;
            }
            return false;
        }

    private:
        PDH_HQUERY query = nullptr;
        std::vector<juce::uint8> scratch;
    };

    // ======================================================
    // DXGI：主 GPU
    // ======================================================
    struct GpuAdapter
    {
        bool         valid = false;
        LUID         luid {};
        UINT         vendorId = 0;
        juce::String name;
        double       dedicated = 0.0;
        double       shared    = 0.0;
        std::wstring luidTag;      // "luid_0x00000000_0x0000f1a4"（小写，用于匹配 PDH 实例名）
    };

    GpuAdapter pickPrimaryGpu()
    {
        GpuAdapter best;

        IDXGIFactory1* factory = nullptr;
        if (FAILED (CreateDXGIFactory1 (__uuidof (IDXGIFactory1), (void**) &factory)) || factory == nullptr)
            return best;

        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1 (i, &adapter) == DXGI_ERROR_NOT_FOUND || adapter == nullptr)
                break;

            DXGI_ADAPTER_DESC1 d {};
            if (SUCCEEDED (adapter->GetDesc1 (&d))
                && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0
                && d.VendorId != 0x1414)   // Microsoft Basic Render Driver
            {
                const double ded = (double) d.DedicatedVideoMemory;
                if (! best.valid || ded > best.dedicated)
                {
                    best.valid     = true;
                    best.luid      = d.AdapterLuid;
                    best.vendorId  = d.VendorId;
                    best.name      = juce::String (d.Description).trim();
                    best.dedicated = ded;
                    best.shared    = (double) d.SharedSystemMemory;

                    wchar_t buf[64] {};
                    swprintf_s (buf, L"luid_0x%08x_0x%08x",
                                (unsigned) d.AdapterLuid.HighPart, (unsigned) d.AdapterLuid.LowPart);
                    best.luidTag = buf;
                }
            }

            adapter->Release();
        }

        factory->Release();
        return best;
    }

    // ======================================================
    // D3DKMT：GPU 温度（结构体布局与 Windows SDK shared/d3dkmthk.h 一致，
    //   只声明用到的部分，避免依赖 SDK 版本 / NTDDI 宏）
    // ======================================================
    struct KmtOpenAdapterFromLuid { LUID AdapterLuid; UINT hAdapter; };
    struct KmtCloseAdapter        { UINT hAdapter; };
    struct KmtQueryAdapterInfo    { UINT hAdapter; UINT Type; void* pPrivateDriverData; UINT PrivateDriverDataSize; };

    struct KmtAdapterPerfData
    {
        UINT32    PhysicalAdapterIndex;
        ULONGLONG MemoryFrequency;
        ULONGLONG MaxMemoryFrequency;
        ULONGLONG MaxMemoryFrequencyOC;
        ULONGLONG MemoryBandwidth;
        ULONGLONG PCIEBandwidth;
        ULONG     FanRPM;
        ULONG     Power;          // 0.1 %
        ULONG     Temperature;    // 0.1 °C
        UCHAR     PowerStateOverride;
    };

    constexpr UINT kKmtQaiAdapterPerfData = 62;   // KMTQAITYPE_ADAPTERPERFDATA

    class KmtGpuTemp
    {
    public:
        explicit KmtGpuTemp (LUID luid)
        {
            gdi = LoadLibraryW (L"gdi32.dll");
            if (gdi == nullptr)
                return;

            openFn  = (OpenFn)  (void*) GetProcAddress (gdi, "D3DKMTOpenAdapterFromLuid");
            closeFn = (CloseFn) (void*) GetProcAddress (gdi, "D3DKMTCloseAdapter");
            queryFn = (QueryFn) (void*) GetProcAddress (gdi, "D3DKMTQueryAdapterInfo");

            if (openFn == nullptr || closeFn == nullptr || queryFn == nullptr)
                return;

            KmtOpenAdapterFromLuid o {};
            o.AdapterLuid = luid;
            if (openFn (&o) == 0)
            {
                handle = o.hAdapter;
                opened = true;
            }
        }

        ~KmtGpuTemp()
        {
            if (opened && closeFn != nullptr)
            {
                KmtCloseAdapter c { handle };
                closeFn (&c);
            }
            if (gdi != nullptr)
                FreeLibrary (gdi);
        }

        float read()
        {
            if (! opened || failures > 5)
                return std::numeric_limits<float>::quiet_NaN();

            KmtAdapterPerfData perf {};
            perf.PhysicalAdapterIndex = 0;

            KmtQueryAdapterInfo q {};
            q.hAdapter = handle;
            q.Type = kKmtQaiAdapterPerfData;
            q.pPrivateDriverData = &perf;
            q.PrivateDriverDataSize = (UINT) sizeof (perf);

            if (queryFn (&q) != 0 || perf.Temperature == 0)
            {
                ++failures;   // 老驱动 / 不支持：连续失败几次后不再尝试
                return std::numeric_limits<float>::quiet_NaN();
            }

            failures = 0;
            return plausibleTemp ((double) perf.Temperature / 10.0);
        }

    private:
        using OpenFn  = LONG (APIENTRY*) (KmtOpenAdapterFromLuid*);
        using CloseFn = LONG (APIENTRY*) (KmtCloseAdapter*);
        using QueryFn = LONG (APIENTRY*) (KmtQueryAdapterInfo*);

        HMODULE gdi = nullptr;
        OpenFn  openFn  = nullptr;
        CloseFn closeFn = nullptr;
        QueryFn queryFn = nullptr;
        UINT    handle  = 0;
        bool    opened  = false;
        int     failures = 0;
    };

    // ======================================================
    // NVML：NVIDIA GPU 温度兜底（驱动自带 nvml.dll）
    // ======================================================
    class NvmlGpuTemp
    {
    public:
        NvmlGpuTemp()
        {
            lib = LoadLibraryExW (L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

            if (lib == nullptr)
            {
                wchar_t pf[MAX_PATH] {};
                if (GetEnvironmentVariableW (L"ProgramW6432", pf, MAX_PATH) > 0)
                {
                    std::wstring p = std::wstring (pf) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
                    lib = LoadLibraryW (p.c_str());
                }
            }

            if (lib == nullptr)
                return;

            auto initFn  = (InitFn)   (void*) GetProcAddress (lib, "nvmlInit_v2");
            auto countFn = (CountFn)  (void*) GetProcAddress (lib, "nvmlDeviceGetCount_v2");
            auto handFn  = (HandleFn) (void*) GetProcAddress (lib, "nvmlDeviceGetHandleByIndex_v2");
            tempFn       = (TempFn)   (void*) GetProcAddress (lib, "nvmlDeviceGetTemperature");
            shutdownFn   = (ShutFn)   (void*) GetProcAddress (lib, "nvmlShutdown");

            if (initFn == nullptr || countFn == nullptr || handFn == nullptr || tempFn == nullptr)
                return;

            if (initFn() != 0)
                return;
            initialised = true;

            unsigned count = 0;
            if (countFn (&count) == 0 && count > 0 && handFn (0, &device) == 0)
                ready = true;
        }

        ~NvmlGpuTemp()
        {
            if (initialised && shutdownFn != nullptr)
                shutdownFn();
            if (lib != nullptr)
                FreeLibrary (lib);
        }

        float read()
        {
            if (! ready)
                return std::numeric_limits<float>::quiet_NaN();

            unsigned t = 0;
            if (tempFn (device, 0 /* NVML_TEMPERATURE_GPU */, &t) != 0)
                return std::numeric_limits<float>::quiet_NaN();

            return plausibleTemp ((double) t);
        }

    private:
        using InitFn   = int (*) ();
        using CountFn  = int (*) (unsigned*);
        using HandleFn = int (*) (unsigned, void**);
        using TempFn   = int (*) (void*, int, unsigned*);
        using ShutFn   = int (*) ();

        HMODULE lib = nullptr;
        TempFn  tempFn = nullptr;
        ShutFn  shutdownFn = nullptr;
        void*   device = nullptr;
        bool    initialised = false;
        bool    ready = false;
    };

    // ======================================================
    // HWiNFO 共享内存（用户在 HWiNFO 设置里勾选 "Shared Memory Support"）
    // ======================================================
   #pragma pack(push, 1)
    struct HwiHeader
    {
        DWORD   signature;          // 'HWiS'；HWiNFO 关闭共享内存后为 'DEAD'
        DWORD   version;
        DWORD   revision;
        __int64 pollTime;
        DWORD   offsetOfSensorSection;
        DWORD   sizeOfSensorElement;
        DWORD   numSensorElements;
        DWORD   offsetOfReadingSection;
        DWORD   sizeOfReadingElement;
        DWORD   numReadingElements;
    };

    struct HwiSensor
    {
        DWORD sensorId;
        DWORD sensorInst;
        char  nameOrig[128];
        char  nameUser[128];
    };

    struct HwiReading
    {
        DWORD  type;                 // 1 = 温度
        DWORD  sensorIndex;
        DWORD  readingId;
        char   labelOrig[128];
        char   labelUser[128];
        char   unit[16];
        double value;
        double valueMin;
        double valueMax;
        double valueAvg;
    };
   #pragma pack(pop)

    struct BridgeTemps
    {
        float cpu = std::numeric_limits<float>::quiet_NaN();
        float gpu = std::numeric_limits<float>::quiet_NaN();
    };

    int scoreCpuLabel (const char (&label)[128])
    {
        if (containsAsciiI (label, "CPU Package"))       return 100;
        if (containsAsciiI (label, "Tctl/Tdie"))         return 95;
        if (containsAsciiI (label, "CPU Die (average)")) return 90;
        if (containsAsciiI (label, "Core Max"))          return 80;
        if (containsAsciiI (label, "CPU (Tctl)"))        return 78;
        if (containsAsciiI (label, "Core Temperatures")) return 70;
        if (strnlen (label, 128) == 3 && containsAsciiI (label, "CPU")) return 60;
        return 0;
    }

    int scoreGpuLabel (const char (&label)[128])
    {
        if (containsAsciiI (label, "GPU Temperature"))   return 100;
        if (containsAsciiI (label, "GPU Core"))          return 90;
        if (containsAsciiI (label, "GPU Hot Spot"))      return 40;
        return 0;
    }

    class HwinfoReader
    {
    public:
        bool read (BridgeTemps& out)
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (now < retryAfterMs)
                return false;

            HANDLE map = OpenFileMappingW (FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
            if (map == nullptr)
                map = OpenFileMappingW (FILE_MAP_READ, FALSE, L"HWiNFO_SENS_SM2");

            if (map == nullptr)
            {
                retryAfterMs = now + 5000;
                return false;
            }

            bool ok = false;

            if (auto* base = (const juce::uint8*) MapViewOfFile (map, FILE_MAP_READ, 0, 0, 0))
            {
                MEMORY_BASIC_INFORMATION mbi {};
                const size_t mapSize = VirtualQuery (base, &mbi, sizeof (mbi)) != 0 ? (size_t) mbi.RegionSize : 0;

                ok = parse (base, mapSize, out);
                UnmapViewOfFile (base);
            }

            CloseHandle (map);

            if (! ok)
                retryAfterMs = now + 5000;
            return ok;
        }

    private:
        static bool parse (const juce::uint8* base, size_t size, BridgeTemps& out)
        {
            if (size < sizeof (HwiHeader))
                return false;

            const auto* h = reinterpret_cast<const HwiHeader*> (base);
            if (h->signature != 0x53695748u)   // 'HWiS'
                return false;

            const size_t sensStride = h->sizeOfSensorElement;
            const size_t readStride = h->sizeOfReadingElement;
            if (readStride < sizeof (HwiReading) || sensStride < sizeof (HwiSensor))
                return false;

            const size_t readEnd = (size_t) h->offsetOfReadingSection + readStride * h->numReadingElements;
            const size_t sensEnd = (size_t) h->offsetOfSensorSection  + sensStride * h->numSensorElements;
            if (readEnd > size || sensEnd > size)
                return false;

            int bestCpu = 0, bestGpu = 0;

            for (DWORD i = 0; i < h->numReadingElements; ++i)
            {
                const auto* r = reinterpret_cast<const HwiReading*> (base + h->offsetOfReadingSection + readStride * i);
                if (r->type != 1)
                    continue;

                const char* sensorName = "";
                if (r->sensorIndex < h->numSensorElements)
                    sensorName = reinterpret_cast<const HwiSensor*> (base + h->offsetOfSensorSection
                                                                     + sensStride * r->sensorIndex)->nameOrig;

                double c = r->value;
                if (containsAsciiI (r->unit, "F"))
                    c = (c - 32.0) * 5.0 / 9.0;

                const float t = plausibleTemp (c);
                if (! std::isfinite (t))
                    continue;

                const bool cpuSensor = containsAsciiI (sensorName, 128, "CPU");
                const bool gpuSensor = containsAsciiI (sensorName, 128, "GPU");

                if (cpuSensor)
                {
                    const int sc = scoreCpuLabel (r->labelOrig);
                    if (sc > bestCpu) { bestCpu = sc; out.cpu = t; }
                }

                if (gpuSensor)
                {
                    const int sc = scoreGpuLabel (r->labelOrig);
                    if (sc > bestGpu) { bestGpu = sc; out.gpu = t; }
                }
            }

            return bestCpu > 0 || bestGpu > 0;
        }

        juce::uint32 retryAfterMs = 0;
    };

    // ======================================================
    // LibreHardwareMonitor / OpenHardwareMonitor WMI
    //   两者运行时都会发布 ROOT\<Name>\Sensor 类（SensorType / Name / Value / Identifier）
    // ======================================================
    class WmiHardwareMonitor
    {
    public:
        ~WmiHardwareMonitor() { disconnect(); }

        bool read (BridgeTemps& out, juce::String& sourceName)
        {
            const auto now = juce::Time::getMillisecondCounter();

            if (services == nullptr)
            {
                if (now < retryAfterMs)
                    return false;

                if (! connect (L"ROOT\\LibreHardwareMonitor", "LibreHardwareMonitor")
                    && ! connect (L"ROOT\\OpenHardwareMonitor", "OpenHardwareMonitor"))
                {
                    retryAfterMs = now + 15000;
                    return false;
                }
            }

            if (! query (out))
            {
                disconnect();
                retryAfterMs = now + 15000;
                return false;
            }

            sourceName = connectedName;
            return std::isfinite (out.cpu) || std::isfinite (out.gpu);
        }

    private:
        bool connect (const wchar_t* ns, const char* name)
        {
            disconnect();

            if (FAILED (CoCreateInstance (CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_IWbemLocator, (void**) &locator)) || locator == nullptr)
                return false;

            BSTR nsStr = SysAllocString (ns);
            const HRESULT hr = locator->ConnectServer (nsStr, nullptr, nullptr, nullptr, 0,
                                                       nullptr, nullptr, &services);
            SysFreeString (nsStr);

            if (FAILED (hr) || services == nullptr)
            {
                disconnect();
                return false;
            }

            CoSetProxyBlanket (services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                               RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                               nullptr, EOAC_NONE);

            connectedName = name;
            return true;
        }

        void disconnect()
        {
            if (services != nullptr) { services->Release(); services = nullptr; }
            if (locator  != nullptr) { locator->Release();  locator  = nullptr; }
            connectedName.clear();
        }

        static int scoreCpuName (const juce::String& n)
        {
            if (n.equalsIgnoreCase ("CPU Package"))       return 100;
            if (n.containsIgnoreCase ("Tctl/Tdie"))       return 95;
            if (n.containsIgnoreCase ("Tctl"))            return 90;
            if (n.equalsIgnoreCase ("Core Max"))          return 80;
            if (n.equalsIgnoreCase ("Core Average"))      return 75;
            if (n.containsIgnoreCase ("CPU"))             return 50;
            return 10;
        }

        static int scoreGpuName (const juce::String& n)
        {
            if (n.equalsIgnoreCase ("GPU Core"))          return 100;
            if (n.containsIgnoreCase ("Hot Spot"))        return 40;
            return 10;
        }

        bool query (BridgeTemps& out)
        {
            BSTR lang = SysAllocString (L"WQL");
            BSTR q    = SysAllocString (L"SELECT Name, Value, Identifier FROM Sensor WHERE SensorType='Temperature'");

            IEnumWbemClassObject* en = nullptr;
            const HRESULT hr = services->ExecQuery (lang, q,
                                                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                                    nullptr, &en);
            SysFreeString (lang);
            SysFreeString (q);

            if (FAILED (hr) || en == nullptr)
                return false;

            int bestCpu = 0, bestGpu = 0;

            for (;;)
            {
                IWbemClassObject* obj = nullptr;
                ULONG ret = 0;
                if (en->Next (500, 1, &obj, &ret) != S_OK || ret == 0 || obj == nullptr)
                    break;

                juce::String name, ident;
                double value = std::numeric_limits<double>::quiet_NaN();

                VARIANT v;
                VariantInit (&v);
                if (SUCCEEDED (obj->Get (L"Name", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR)
                    name = juce::String (v.bstrVal);
                VariantClear (&v);

                if (SUCCEEDED (obj->Get (L"Identifier", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR)
                    ident = juce::String (v.bstrVal);
                VariantClear (&v);

                if (SUCCEEDED (obj->Get (L"Value", 0, &v, nullptr, nullptr)))
                {
                    if (v.vt == VT_R4)      value = (double) v.fltVal;
                    else if (v.vt == VT_R8) value = v.dblVal;
                }
                VariantClear (&v);
                obj->Release();

                const float t = plausibleTemp (value);
                if (! std::isfinite (t))
                    continue;

                const auto id = ident.toLowerCase();

                if (id.contains ("cpu/") || id.contains ("cpu\\"))
                {
                    const int sc = scoreCpuName (name);
                    if (sc > bestCpu) { bestCpu = sc; out.cpu = t; }
                }
                else if (id.contains ("gpu"))
                {
                    const int sc = scoreGpuName (name);
                    if (sc > bestGpu) { bestGpu = sc; out.gpu = t; }
                }
            }

            en->Release();
            return true;
        }

        IWbemLocator*  locator  = nullptr;
        IWbemServices* services = nullptr;
        juce::String   connectedName;
        juce::uint32   retryAfterMs = 0;
    };

    // ======================================================
    // 后端主体
    // ======================================================
    class WindowsBackend : public PlatformBackend
    {
    public:
        WindowsBackend()
        {
            const HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            comInitialised = SUCCEEDED (hr);

            gpu = pickPrimaryGpu();

            if (gpu.valid)
            {
                kmt = std::make_unique<KmtGpuTemp> (gpu.luid);
                if (gpu.vendorId == 0x10DE)
                    nvml = std::make_unique<NvmlGpuTemp>();
            }

            baseMHz = readBaseMHz();

            cCpuUtil = pdh.add (L"\\Processor Information(_Total)\\% Processor Utility");
            if (cCpuUtil == nullptr)
                cCpuUtil = pdh.add (L"\\Processor(_Total)\\% Processor Time");
            cCpuPerf   = pdh.add (L"\\Processor Information(_Total)\\% Processor Performance");
            cCores     = pdh.add (L"\\Processor(*)\\% Processor Time");
            cDiskIdle  = pdh.add (L"\\PhysicalDisk(_Total)\\% Idle Time");
            cDiskRead  = pdh.add (L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec");
            cDiskWrite = pdh.add (L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec");
            cNetRecv   = pdh.add (L"\\Network Interface(*)\\Bytes Received/sec");
            cNetSent   = pdh.add (L"\\Network Interface(*)\\Bytes Sent/sec");
            cGpuEngine = pdh.add (L"\\GPU Engine(*)\\Utilization Percentage");
            cGpuDedic  = pdh.add (L"\\GPU Adapter Memory(*)\\Dedicated Usage");
            cGpuShared = pdh.add (L"\\GPU Adapter Memory(*)\\Shared Usage");
            cThermal   = pdh.add (L"\\Thermal Zone Information(*)\\Temperature");

            pdh.collect();
            lastCollectMs = juce::Time::getMillisecondCounterHiRes();
        }

        ~WindowsBackend() override
        {
            wmi.reset();
            nvml.reset();
            kmt.reset();

            if (comInitialised)
                CoUninitialize();
        }

        void sample (Snapshot& s) override
        {
            // 速率类计数器需要两次采集之间有足够间隔，首帧补齐 300 ms
            const double sinceLast = juce::Time::getMillisecondCounterHiRes() - lastCollectMs;
            if (sinceLast < 300.0)
                Sleep ((DWORD) (300.0 - sinceLast));

            pdh.collect();
            lastCollectMs = juce::Time::getMillisecondCounterHiRes();

            sampleCpu (s);
            sampleMemory (s);
            sampleDisk (s);
            sampleNetwork (s);
            sampleGpu (s);
            sampleTemps (s);
            samplePower (s);

            s.uptimeSec = (double) GetTickCount64() / 1000.0;
        }

    private:
        static float readBaseMHz()
        {
            DWORD mhz = 0, size = sizeof (mhz);
            if (RegGetValueW (HKEY_LOCAL_MACHINE,
                              L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                              L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size) == ERROR_SUCCESS)
                return (float) mhz;
            return -1.0f;
        }

        void sampleCpu (Snapshot& s)
        {
            double v = 0.0;
            if (PdhQuery::readDouble (cCpuUtil, v))
                s.cpuLoad = (float) juce::jlimit (0.0, 100.0, v);

            if (baseMHz > 0.0f && PdhQuery::readDouble (cCpuPerf, v) && v > 0.0)
                s.cpuMHz = baseMHz * (float) (v / 100.0);

            if (pdh.readArray (cCores, items))
            {
                int maxIndex = -1;
                for (const auto& it : items)
                {
                    if (it.name.empty() || ! std::iswdigit (it.name[0]))
                        continue;   // "_Total"

                    const int idx = _wtoi (it.name.c_str());
                    if (idx >= 0 && idx < kMaxCores)
                    {
                        s.coreLoad[(size_t) idx] = (float) juce::jlimit (0.0, 100.0, it.value);
                        maxIndex = juce::jmax (maxIndex, idx);
                    }
                }

                if (maxIndex >= 0)
                    s.numCores = maxIndex + 1;
            }
        }

        static void sampleMemory (Snapshot& s)
        {
            MEMORYSTATUSEX ms {};
            ms.dwLength = sizeof (ms);
            if (GlobalMemoryStatusEx (&ms))
            {
                s.ramTotal = (double) ms.ullTotalPhys;
                s.ramUsed  = (double) (ms.ullTotalPhys - ms.ullAvailPhys);
            }
        }

        void sampleDisk (Snapshot& s)
        {
            double v = 0.0;
            if (PdhQuery::readDouble (cDiskIdle, v))
                s.diskActive = (float) juce::jlimit (0.0, 100.0, 100.0 - v);
            if (PdhQuery::readDouble (cDiskRead, v))
                s.diskReadBps = juce::jmax (0.0, v);
            if (PdhQuery::readDouble (cDiskWrite, v))
                s.diskWriteBps = juce::jmax (0.0, v);
        }

        static bool isVirtualNic (const std::wstring& name)
        {
            return containsI (name, L"loopback") || containsI (name, L"isatap")
                || containsI (name, L"teredo")   || containsI (name, L"6to4")
                || containsI (name, L"vethernet");
        }

        void sampleNetwork (Snapshot& s)
        {
            auto sum = [this] (PDH_HCOUNTER c, double& out)
            {
                if (! pdh.readArray (c, items))
                    return;

                double total = 0.0;
                for (const auto& it : items)
                    if (! isVirtualNic (it.name))
                        total += juce::jmax (0.0, it.value);
                out = total;
            };

            sum (cNetRecv, s.netDownBps);
            sum (cNetSent, s.netUpBps);
        }

        void sampleGpu (Snapshot& s)
        {
            if (gpu.valid)
                s.gpuName = gpu.name;

            // ---- GPU 占用：按"引擎"聚合所有进程，取主 GPU 上最忙的引擎（同任务管理器）----
            if (pdh.readArray (cGpuEngine, items))
            {
                engineSums.clear();

                for (const auto& it : items)
                {
                    auto key = toLower (it.name);
                    const auto pos = key.find (L"luid_");
                    if (pos != std::wstring::npos)
                        key = key.substr (pos);
                    engineSums[key] += it.value;
                }

                double bestMatch = -1.0, bestAny = -1.0;
                for (const auto& kv : engineSums)
                {
                    const double v = juce::jlimit (0.0, 100.0, kv.second);
                    bestAny = juce::jmax (bestAny, v);
                    if (! gpu.luidTag.empty() && kv.first.rfind (gpu.luidTag, 0) == 0)
                        bestMatch = juce::jmax (bestMatch, v);
                }

                const double load = bestMatch >= 0.0 ? bestMatch : bestAny;
                if (load >= 0.0)
                    s.gpuLoad = (float) load;
                else if (gpu.valid)
                    s.gpuLoad = 0.0f;   // 计数器存在但没有任何引擎实例 = 空闲
            }

            // ---- 显存：独显看专用显存；核显（专用 < 512 MB）看专用 + 共享 ----
            if (gpu.valid)
            {
                const bool integrated = gpu.dedicated < 512.0 * 1024.0 * 1024.0;

                auto sumFor = [this] (PDH_HCOUNTER c, double& out) -> bool
                {
                    if (! pdh.readArray (c, items))
                        return false;

                    double total = 0.0;
                    bool any = false;
                    for (const auto& it : items)
                        if (toLower (it.name).find (gpu.luidTag) != std::wstring::npos)
                        {
                            total += juce::jmax (0.0, it.value);
                            any = true;
                        }
                    if (any) out = total;
                    return any;
                };

                double ded = 0.0, sha = 0.0;
                const bool hasDed = sumFor (cGpuDedic, ded);

                if (integrated)
                {
                    const bool hasSha = sumFor (cGpuShared, sha);
                    if (hasDed || hasSha)
                    {
                        s.vramUsed  = ded + sha;
                        s.vramTotal = gpu.dedicated + gpu.shared;
                    }
                }
                else if (hasDed)
                {
                    s.vramUsed  = ded;
                    s.vramTotal = gpu.dedicated;
                }
            }
        }

        void sampleTemps (Snapshot& s)
        {
            // ---- GPU：D3DKMT → NVML ----
            if (kmt != nullptr)
            {
                const float t = kmt->read();
                if (std::isfinite (t)) { s.gpuTempC = t; s.gpuTempSource = "Windows"; }
            }

            if (! std::isfinite (s.gpuTempC) && nvml != nullptr)
            {
                const float t = nvml->read();
                if (std::isfinite (t)) { s.gpuTempC = t; s.gpuTempSource = "NVML"; }
            }

            // ---- 桥接：HWiNFO → LibreHardwareMonitor / OpenHardwareMonitor ----
            BridgeTemps bridge;
            juce::String bridgeName;

            if (hwinfo.read (bridge))
            {
                bridgeName = "HWiNFO";
            }
            else
            {
                if (wmi == nullptr && comInitialised)
                    wmi = std::make_unique<WmiHardwareMonitor>();
                if (wmi != nullptr)
                    wmi->read (bridge, bridgeName);
            }

            if (std::isfinite (bridge.cpu))
            {
                s.cpuTempC = bridge.cpu;
                s.cpuTempSource = bridgeName;
            }

            if (! std::isfinite (s.gpuTempC) && std::isfinite (bridge.gpu))
            {
                s.gpuTempC = bridge.gpu;
                s.gpuTempSource = bridgeName;
            }

            // ---- CPU 最后兜底：ACPI 热区（开尔文；很多主板只给一个固定值，仅作近似）----
            if (! std::isfinite (s.cpuTempC) && pdh.readArray (cThermal, items))
            {
                double best = -1.0;
                for (const auto& it : items)
                    best = juce::jmax (best, it.value);

                const float t = plausibleTemp (best - 273.15);
                if (std::isfinite (t) && t > 10.0f)
                {
                    s.cpuTempC = t;
                    s.cpuTempSource = "ACPI";
                }
            }
        }

        static void samplePower (Snapshot& s)
        {
            SYSTEM_POWER_STATUS ps {};
            if (! GetSystemPowerStatus (&ps))
                return;

            s.onAcPower  = ps.ACLineStatus == 1;
            s.hasBattery = (ps.BatteryFlag & 128) == 0 && ps.BatteryFlag != 255 && ps.BatteryLifePercent != 255;

            if (s.hasBattery)
            {
                s.batteryPct = (float) ps.BatteryLifePercent;
                s.charging   = (ps.BatteryFlag & 8) != 0;
            }
        }

        bool comInitialised = false;
        PdhQuery pdh;
        double lastCollectMs = 0.0;

        PDH_HCOUNTER cCpuUtil = nullptr, cCpuPerf = nullptr, cCores = nullptr;
        PDH_HCOUNTER cDiskIdle = nullptr, cDiskRead = nullptr, cDiskWrite = nullptr;
        PDH_HCOUNTER cNetRecv = nullptr, cNetSent = nullptr;
        PDH_HCOUNTER cGpuEngine = nullptr, cGpuDedic = nullptr, cGpuShared = nullptr;
        PDH_HCOUNTER cThermal = nullptr;

        float baseMHz = -1.0f;

        GpuAdapter gpu;
        std::unique_ptr<KmtGpuTemp>  kmt;
        std::unique_ptr<NvmlGpuTemp> nvml;
        HwinfoReader hwinfo;
        std::unique_ptr<WmiHardwareMonitor> wmi;

        std::vector<PdhItem> items;
        std::unordered_map<std::wstring, double> engineSums;
    };
}

std::unique_ptr<PlatformBackend> createPlatformBackend()
{
    return std::make_unique<WindowsBackend>();
}

} // namespace y2k::sys

#endif // JUCE_WINDOWS
