// 系统头必须在 JuceHeader.h 之前引入：CoreFoundation → MacTypes.h 会声明全局
//   Point，若此时 JuceHeader 的 `using namespace juce;` 已生效，MacTypes.h 内部
//   对 Point 的引用会与 juce::Point 二义（见 source/i18n/I18nTranslate.h 说明）。
#if defined (__APPLE__)
 #include <mach/mach.h>
 #include <mach/mach_host.h>
 #include <sys/sysctl.h>
 #include <sys/socket.h>
 #include <sys/time.h>
 #include <net/if.h>
 #include <net/route.h>
 #include <IOKit/IOKitLib.h>
 #include <IOKit/ps/IOPowerSources.h>
 #include <IOKit/ps/IOPSKeys.h>
 #include <CoreFoundation/CoreFoundation.h>
 #include <cstring>
#endif

#include "source/system/SystemStats.h"

#if JUCE_MAC

// ==========================================================
// SystemStatsMac.cpp —— macOS 后端（mach / IOKit / sysctl，全部公开 C API）
//
//   · CPU：host_processor_info 每核 tick 差分
//   · 内存：host_statistics64（App + 联动 + 压缩，接近"活动监视器 → 已用内存"）
//   · GPU：IOAccelerator 的 PerformanceStatistics（Device Utilization % / 显存占用）
//   · 磁盘：IOBlockStorageDriver Statistics（读写字节 + 读写耗时 → 活动百分比）
//   · 网络：sysctl NET_RT_IFLIST2（64 位计数器，跳过回环）
//   · 电池：IOPowerSources
//   · 温度：macOS 没有公开接口（SMC / IOHID 传感器属于私有 API），保持 N/A
// ==========================================================

#include <vector>

namespace y2k::sys
{
namespace
{
    double cfNumberToDouble (CFTypeRef ref, double fallback = -1.0)
    {
        if (ref == nullptr || CFGetTypeID (ref) != CFNumberGetTypeID())
            return fallback;

        double v = fallback;
        CFNumberGetValue ((CFNumberRef) ref, kCFNumberDoubleType, &v);
        return v;
    }

    double dictNumber (CFDictionaryRef dict, const char* key, double fallback = -1.0)
    {
        if (dict == nullptr)
            return fallback;

        CFStringRef k = CFStringCreateWithCString (kCFAllocatorDefault, key, kCFStringEncodingUTF8);
        const double v = cfNumberToDouble (CFDictionaryGetValue (dict, k), fallback);
        CFRelease (k);
        return v;
    }

    juce::String cfToString (CFTypeRef ref)
    {
        if (ref == nullptr)
            return {};

        if (CFGetTypeID (ref) == CFStringGetTypeID())
        {
            char buf[256] {};
            if (CFStringGetCString ((CFStringRef) ref, buf, sizeof (buf), kCFStringEncodingUTF8))
                return juce::String (juce::CharPointer_UTF8 (buf));
        }
        else if (CFGetTypeID (ref) == CFDataGetTypeID())
        {
            const auto len = (int) CFDataGetLength ((CFDataRef) ref);
            const auto* bytes = (const char*) CFDataGetBytePtr ((CFDataRef) ref);
            return juce::String::fromUTF8 (bytes, (int) strnlen (bytes, (size_t) len));
        }

        return {};
    }

    class MacBackend : public PlatformBackend
    {
    public:
        MacBackend()
        {
            gpuName = readGpuName();
        }

        void sample (Snapshot& s) override
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            const double dt  = lastSampleMs > 0.0 ? (now - lastSampleMs) / 1000.0 : 0.0;
            lastSampleMs = now;

            sampleCpu (s);
            sampleMemory (s);
            sampleGpu (s);
            sampleDisk (s, dt);
            sampleNetwork (s, dt);
            samplePower (s);
            sampleUptime (s);

            s.gpuName = gpuName;
        }

    private:
        // ---------------- CPU ----------------
        void sampleCpu (Snapshot& s)
        {
            natural_t cpuCount = 0;
            processor_info_array_t info = nullptr;
            mach_msg_type_number_t infoCount = 0;

            if (host_processor_info (mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                     &cpuCount, &info, &infoCount) != KERN_SUCCESS)
                return;

            const int n = juce::jmin ((int) cpuCount, kMaxCores);
            std::vector<juce::uint64> busy ((size_t) n), total ((size_t) n);

            for (int i = 0; i < n; ++i)
            {
                const auto* t = info + (CPU_STATE_MAX * i);
                const juce::uint64 user = (juce::uint64) (juce::uint32) t[CPU_STATE_USER];
                const juce::uint64 sysT = (juce::uint64) (juce::uint32) t[CPU_STATE_SYSTEM];
                const juce::uint64 nice = (juce::uint64) (juce::uint32) t[CPU_STATE_NICE];
                const juce::uint64 idle = (juce::uint64) (juce::uint32) t[CPU_STATE_IDLE];
                busy[(size_t) i]  = user + sysT + nice;
                total[(size_t) i] = user + sysT + nice + idle;
            }

            vm_deallocate (mach_task_self(), (vm_address_t) info, (vm_size_t) (infoCount * sizeof (integer_t)));

            if ((int) prevBusy.size() == n)
            {
                juce::uint64 sumBusy = 0, sumTotal = 0;

                for (int i = 0; i < n; ++i)
                {
                    const auto db = busy[(size_t) i]  - prevBusy[(size_t) i];
                    const auto dt = total[(size_t) i] - prevTotal[(size_t) i];
                    sumBusy += db;
                    sumTotal += dt;
                    s.coreLoad[(size_t) i] = dt > 0 ? (float) (100.0 * (double) db / (double) dt) : 0.0f;
                }

                s.numCores = n;
                if (sumTotal > 0)
                    s.cpuLoad = (float) juce::jlimit (0.0, 100.0, 100.0 * (double) sumBusy / (double) sumTotal);
            }

            prevBusy  = std::move (busy);
            prevTotal = std::move (total);

            // Intel Mac 才有 hw.cpufrequency；Apple Silicon 不公开，保持 N/A
            juce::uint64 hz = 0;
            size_t len = sizeof (hz);
            if (sysctlbyname ("hw.cpufrequency", &hz, &len, nullptr, 0) == 0 && hz > 0)
                s.cpuMHz = (float) ((double) hz / 1.0e6);
        }

        // ---------------- 内存 ----------------
        static void sampleMemory (Snapshot& s)
        {
            juce::uint64 memSize = 0;
            size_t len = sizeof (memSize);
            if (sysctlbyname ("hw.memsize", &memSize, &len, nullptr, 0) == 0)
                s.ramTotal = (double) memSize;

            vm_statistics64_data_t vm {};
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
            if (host_statistics64 (mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vm, &count) != KERN_SUCCESS)
                return;

            vm_size_t pageSize = 0;
            host_page_size (mach_host_self(), &pageSize);

            const double app        = (double) vm.internal_page_count - (double) vm.purgeable_count;
            const double wired      = (double) vm.wire_count;
            const double compressed = (double) vm.compressor_page_count;

            s.ramUsed = juce::jmax (0.0, (app + wired + compressed) * (double) pageSize);
        }

        // ---------------- GPU ----------------
        static juce::String readGpuName()
        {
            io_iterator_t it = 0;
            if (IOServiceGetMatchingServices (0, IOServiceMatching ("IOAccelerator"), &it) != KERN_SUCCESS)
                return {};

            juce::String name;
            if (io_object_t entry = IOIteratorNext (it))
            {
                CFTypeRef model = IORegistryEntrySearchCFProperty (entry, kIOServicePlane, CFSTR ("model"),
                                                                   kCFAllocatorDefault,
                                                                   kIORegistryIterateRecursively | kIORegistryIterateParents);
                if (model != nullptr)
                {
                    name = cfToString (model).trim();
                    CFRelease (model);
                }
                IOObjectRelease (entry);
            }
            IOObjectRelease (it);
            return name;
        }

        static void sampleGpu (Snapshot& s)
        {
            io_iterator_t it = 0;
            if (IOServiceGetMatchingServices (0, IOServiceMatching ("IOAccelerator"), &it) != KERN_SUCCESS)
                return;

            double bestLoad = -1.0;

            while (io_object_t entry = IOIteratorNext (it))
            {
                CFMutableDictionaryRef props = nullptr;
                if (IORegistryEntryCreateCFProperties (entry, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props != nullptr)
                {
                    auto perf = (CFDictionaryRef) CFDictionaryGetValue (props, CFSTR ("PerformanceStatistics"));
                    if (perf != nullptr && CFGetTypeID (perf) == CFDictionaryGetTypeID())
                    {
                        double load = dictNumber (perf, "Device Utilization %");
                        if (load < 0.0)
                            load = dictNumber (perf, "GPU Activity(%)");

                        if (load > bestLoad)
                        {
                            bestLoad = load;

                            double used = dictNumber (perf, "In use system memory");
                            if (used < 0.0) used = dictNumber (perf, "vramUsedBytes");
                            if (used >= 0.0) s.vramUsed = used;

                            const double freeV = dictNumber (perf, "vramFreeBytes");
                            if (used >= 0.0 && freeV >= 0.0)
                                s.vramTotal = used + freeV;
                        }
                    }
                    CFRelease (props);
                }
                IOObjectRelease (entry);
            }
            IOObjectRelease (it);

            if (bestLoad >= 0.0)
                s.gpuLoad = (float) juce::jlimit (0.0, 100.0, bestLoad);
        }

        // ---------------- 磁盘 ----------------
        void sampleDisk (Snapshot& s, double dt)
        {
            io_iterator_t it = 0;
            if (IOServiceGetMatchingServices (0, IOServiceMatching ("IOBlockStorageDriver"), &it) != KERN_SUCCESS)
                return;

            double rd = 0.0, wr = 0.0, busyNs = 0.0;

            while (io_object_t entry = IOIteratorNext (it))
            {
                CFTypeRef statsRef = IORegistryEntryCreateCFProperty (entry, CFSTR ("Statistics"), kCFAllocatorDefault, 0);
                if (statsRef != nullptr)
                {
                    if (CFGetTypeID (statsRef) == CFDictionaryGetTypeID())
                    {
                        auto stats = (CFDictionaryRef) statsRef;
                        rd     += juce::jmax (0.0, dictNumber (stats, "Bytes (Read)", 0.0));
                        wr     += juce::jmax (0.0, dictNumber (stats, "Bytes (Write)", 0.0));
                        busyNs += juce::jmax (0.0, dictNumber (stats, "Total Time (Read)", 0.0))
                                + juce::jmax (0.0, dictNumber (stats, "Total Time (Write)", 0.0));
                    }
                    CFRelease (statsRef);
                }
                IOObjectRelease (entry);
            }
            IOObjectRelease (it);

            if (dt > 0.05 && prevDiskRead >= 0.0)
            {
                s.diskReadBps  = juce::jmax (0.0, (rd - prevDiskRead)  / dt);
                s.diskWriteBps = juce::jmax (0.0, (wr - prevDiskWrite) / dt);
                s.diskActive   = (float) juce::jlimit (0.0, 100.0, (busyNs - prevDiskBusyNs) / (dt * 1.0e9) * 100.0);
            }

            prevDiskRead = rd;
            prevDiskWrite = wr;
            prevDiskBusyNs = busyNs;
        }

        // ---------------- 网络 ----------------
        void sampleNetwork (Snapshot& s, double dt)
        {
            int mib[] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0 };
            size_t len = 0;
            if (sysctl (mib, 6, nullptr, &len, nullptr, 0) != 0 || len == 0)
                return;

            buffer.resize (len);
            if (sysctl (mib, 6, buffer.data(), &len, nullptr, 0) != 0)
                return;

            double rx = 0.0, tx = 0.0;
            const char* p   = buffer.data();
            const char* end = p + len;

            while (p + sizeof (if_msghdr) <= end)
            {
                const auto* ifm = reinterpret_cast<const if_msghdr*> (p);
                if (ifm->ifm_msglen == 0)
                    break;

                if (ifm->ifm_type == RTM_IFINFO2 && p + sizeof (if_msghdr2) <= end)
                {
                    const auto* ifm2 = reinterpret_cast<const if_msghdr2*> (p);
                    if ((ifm2->ifm_flags & IFF_LOOPBACK) == 0)
                    {
                        rx += (double) ifm2->ifm_data.ifi_ibytes;
                        tx += (double) ifm2->ifm_data.ifi_obytes;
                    }
                }

                p += ifm->ifm_msglen;
            }

            if (dt > 0.05 && prevRx >= 0.0)
            {
                s.netDownBps = juce::jmax (0.0, (rx - prevRx) / dt);
                s.netUpBps   = juce::jmax (0.0, (tx - prevTx) / dt);
            }

            prevRx = rx;
            prevTx = tx;
        }

        // ---------------- 电池 ----------------
        static void samplePower (Snapshot& s)
        {
            CFTypeRef info = IOPSCopyPowerSourcesInfo();
            if (info == nullptr)
                return;

            CFArrayRef list = IOPSCopyPowerSourcesList (info);
            if (list != nullptr)
            {
                for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
                {
                    CFDictionaryRef d = IOPSGetPowerSourceDescription (info, CFArrayGetValueAtIndex (list, i));
                    if (d == nullptr)
                        continue;

                    auto type = (CFStringRef) CFDictionaryGetValue (d, CFSTR (kIOPSTypeKey));
                    if (type == nullptr || CFStringCompare (type, CFSTR (kIOPSInternalBatteryType), 0) != kCFCompareEqualTo)
                        continue;

                    const double cur = cfNumberToDouble (CFDictionaryGetValue (d, CFSTR (kIOPSCurrentCapacityKey)));
                    const double mx  = cfNumberToDouble (CFDictionaryGetValue (d, CFSTR (kIOPSMaxCapacityKey)));

                    if (cur >= 0.0 && mx > 0.0)
                    {
                        s.hasBattery = true;
                        s.batteryPct = (float) juce::jlimit (0.0, 100.0, cur / mx * 100.0);
                    }

                    auto charging = (CFBooleanRef) CFDictionaryGetValue (d, CFSTR (kIOPSIsChargingKey));
                    s.charging = charging != nullptr && CFBooleanGetValue (charging);

                    auto state = (CFStringRef) CFDictionaryGetValue (d, CFSTR (kIOPSPowerSourceStateKey));
                    s.onAcPower = state != nullptr
                               && CFStringCompare (state, CFSTR (kIOPSACPowerValue), 0) == kCFCompareEqualTo;
                    break;
                }
                CFRelease (list);
            }
            CFRelease (info);
        }

        static void sampleUptime (Snapshot& s)
        {
            struct timeval boot {};
            size_t len = sizeof (boot);
            int mib[] = { CTL_KERN, KERN_BOOTTIME };
            if (sysctl (mib, 2, &boot, &len, nullptr, 0) == 0 && boot.tv_sec > 0)
            {
                struct timeval now {};
                gettimeofday (&now, nullptr);
                s.uptimeSec = (double) (now.tv_sec - boot.tv_sec);
            }
        }

        juce::String gpuName;
        double lastSampleMs = 0.0;

        std::vector<juce::uint64> prevBusy, prevTotal;
        double prevDiskRead = -1.0, prevDiskWrite = -1.0, prevDiskBusyNs = 0.0;
        double prevRx = -1.0, prevTx = -1.0;
        std::vector<char> buffer;
    };
}

std::unique_ptr<PlatformBackend> createPlatformBackend()
{
    return std::make_unique<MacBackend>();
}

} // namespace y2k::sys

#endif // JUCE_MAC
