#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <vector>

// ==========================================================
// SystemStats —— 电脑状态采集层（供 SystemMonitorModule 使用）
//
//   设计约束（与项目"只读 → 计算 → 绘图"性质一致）：
//     · 只使用操作系统公开 API，不含驱动 / 内核组件 / 常驻服务；
//     · 采集在独立后台线程完成（PDH / WMI / IOKit 调用都可能阻塞几十 ms），
//       UI 线程只拷贝最新快照，绝不在消息线程上做任何系统查询；
//     · 无订阅者时线程阻塞等待，不轮询；最后一个模块移除后整个采集器销毁
//       （SharedResourcePointer 引用计数），不留任何后台活动。
//
//   数据源（Windows）：
//     · CPU / 每核 / 频率 / 磁盘 / 网络 / GPU 占用 / 显存：PDH 性能计数器
//       （PdhAddEnglishCounter，非英文系统同样可用）
//     · GPU 名称 / 显存总量 / LUID：DXGI
//     · GPU 温度：D3DKMT 适配器性能数据（与任务管理器同源，WDDM 2.4+）
//                 → NVML（NVIDIA 驱动自带）→ HWiNFO / LibreHardwareMonitor
//     · CPU 温度：HWiNFO 共享内存 → LibreHardwareMonitor / OpenHardwareMonitor
//                 WMI → ACPI 热区（PDH Thermal Zone，精度有限）→ 不可用
//       （Windows 没有无驱动的 CPU 温度公开接口，这里只"桥接"用户自己
//        运行的硬件监控工具，本程序不安装、不加载任何驱动）
//   数据源（macOS）：mach host 统计 / IOKit（IOAccelerator、IOBlockStorageDriver、
//     IOPowerSources）/ sysctl；温度没有公开 API，显示 N/A。
// ==========================================================

namespace y2k::sys
{
    // 历史曲线的指标（顺序只追加不重排：模块存档不依赖它，但便于调试对齐）
    enum class Metric
    {
        cpu = 0,     // %
        cpuTemp,     // °C
        gpu,         // %
        gpuTemp,     // °C
        ram,         // %
        vram,        // %
        disk,        // % 活动时间
        netDown,     // bytes/s
        netUp,       // bytes/s
        battery,     // %
        numMetrics
    };

    constexpr int kNumMetrics = (int) Metric::numMetrics;
    constexpr int kMaxCores   = 128;
    constexpr int kHistoryLen = 1200;   // 0.5 s 间隔下 10 分钟

    struct Snapshot
    {
        juce::uint32 serial = 0;        // 每完成一次采样 +1（0 = 尚未采样）
        double       intervalSec = 1.0;

        // ---- CPU ----
        float cpuLoad  = -1.0f;         // 0..100，<0 = 不可用
        int   numCores = 0;
        std::array<float, kMaxCores> coreLoad {};
        float cpuMHz   = -1.0f;
        float cpuTempC = std::numeric_limits<float>::quiet_NaN();
        juce::String cpuTempSource;     // "HWiNFO" / "LibreHardwareMonitor" / "ACPI" ...

        // ---- GPU ----
        float  gpuLoad  = -1.0f;
        float  gpuTempC = std::numeric_limits<float>::quiet_NaN();
        juce::String gpuTempSource;
        double vramUsed  = -1.0;        // bytes
        double vramTotal = -1.0;        // bytes（统一内存的 Apple Silicon 为 -1）

        // ---- 内存 ----
        double ramUsed  = -1.0;
        double ramTotal = -1.0;

        // ---- 磁盘 ----
        float  diskActive    = -1.0f;   // 0..100
        double diskReadBps   = -1.0;
        double diskWriteBps  = -1.0;
        double sysDriveFree  = -1.0;
        double sysDriveTotal = -1.0;

        // ---- 网络 ----
        double netDownBps = -1.0;
        double netUpBps   = -1.0;

        // ---- 电池 / 运行时间 ----
        bool  hasBattery  = false;
        float batteryPct  = -1.0f;
        bool  charging    = false;
        bool  onAcPower   = false;
        double uptimeSec  = -1.0;

        // ---- 静态信息 ----
        juce::String cpuName, gpuName, osName;

        static bool isValid (float v) noexcept  { return v >= 0.0f && std::isfinite (v); }
        static bool isValid (double v) noexcept { return v >= 0.0 && std::isfinite (v); }
        static bool hasTemp (float c) noexcept  { return std::isfinite (c) && c > -50.0f && c < 200.0f; }

        // 指标的"原始值"（百分比 / 摄氏度 / bytes/s）；不可用返回 NaN
        float metricValue (Metric m) const noexcept;
    };

    // ------------------------------------------------------
    // 平台后端接口（实现见 SystemStatsWin.cpp / SystemStatsMac.cpp）
    //   sample() 只在采集线程上调用；第一次调用前在同一线程完成初始化，
    //   以便 COM / PDH 等线程相关资源都绑定在采集线程。
    // ------------------------------------------------------
    struct PlatformBackend
    {
        virtual ~PlatformBackend() = default;
        virtual void sample (Snapshot& s) = 0;
    };

    std::unique_ptr<PlatformBackend> createPlatformBackend();

    // ------------------------------------------------------
    // Sampler —— 进程内共享的采集器
    //   用法：juce::SharedResourcePointer<y2k::sys::Sampler> sampler;
    //         sampler->subscribe (this, 1.0);  ...  sampler->unsubscribe (this);
    //   采样间隔 = 所有订阅者请求的最小值（0.5 – 10 s）。
    // ------------------------------------------------------
    class Sampler : private juce::Thread
    {
    public:
        Sampler();
        ~Sampler() override;

        void subscribe   (const void* who, double intervalSec);
        void unsubscribe (const void* who);

        Snapshot     getLatest() const;
        juce::uint32 getSerial() const noexcept { return serial.load (std::memory_order_acquire); }
        double       getIntervalSec() const noexcept { return currentInterval.load (std::memory_order_relaxed); }

        // 拷出某指标最近 maxCount 个样本（旧 → 新）；不可用样本为 NaN
        void copyHistory (Metric m, std::vector<float>& out, int maxCount) const;

    private:
        void run() override;
        void recomputeInterval();

        mutable juce::CriticalSection lock;
        Snapshot latest;

        std::array<std::array<float, kHistoryLen>, kNumMetrics> history {};
        int historyWrite = 0;
        int historyCount = 0;

        std::map<const void*, double> subscribers;     // 仅 UI 线程 + lock
        std::atomic<double>       currentInterval { 1.0 };
        std::atomic<int>          numSubscribers  { 0 };
        std::atomic<juce::uint32> serial          { 0 };

        juce::WaitableEvent wakeEvent;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sampler)
    };

    // ---- 格式化辅助（UI 共用）----
    juce::String formatBytes      (double bytes, int decimals = 1);   // "8.1 GB"
    juce::String formatBytesShort (double bytes);                     // "8.1G"
    juce::String formatRate       (double bytesPerSec);               // "1.2 MB/s"
    juce::String formatUptime     (double seconds);                   // "3d 04:12"
}
