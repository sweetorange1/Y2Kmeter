// ==========================================================
// source/network/TelemetryClient.cpp
//   匿名遥测客户端实现。
//
//   关键实现细节：
//     · client_id 在首次运行时由 juce::Uuid() 生成，存储到
//       PropertiesFile 的 "telemetry.clientId" key。
//       若 settings 为 nullptr（临时构建），则使用内存 UUID，
//       仅本次进程生命周期有效。
//     · 系统信息采集严格限定为无个人身份信息的元数据：
//       操作系统名/版本、CPU 型号/核心数、宿主名称、版本号、
//       构建类型、屏幕数量、安装来源标记等。
//     · 绝不收集：用户名、主机名、IP、MAC、硬盘序列号、
//       音频文件路径或内容。
//     · HTTP 请求超时 5 秒，失败静默丢弃，不阻塞任何流程。
//     · 后台线程使用 std::thread，避免依赖 JUCE ThreadPool
//       （JUCE 8 无默认全局线程池）。
//     · 授权状态由安装程序写入注册表，LoadFromRegistry() 读取；
//       默认未授权（enabled_=false），不发送任何数据。
// ==========================================================

#include "TelemetryClient.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

#include <thread>

namespace y2k {
namespace network {

// --------------------------------------------------
// 单例
// --------------------------------------------------
TelemetryClient& TelemetryClient::GetInstance() {
  static TelemetryClient instance;
  return instance;
}

// --------------------------------------------------
// 构造 / 析构
//
// 构造时启动一个常驻 worker 线程；所有遥测 POST 都经任务队列交给它串行
// 执行，取代原先每次请求 detach 一个线程的做法。析构时置 stopWorker_ 并
// join，保证进程退出时后台线程不会游离、被 OS 强制终止。
// --------------------------------------------------
TelemetryClient::TelemetryClient() {
  workerThread_ = std::thread([this]() { WorkerLoop(); });
}

TelemetryClient::~TelemetryClient() {
  stopWorker_.store(true, std::memory_order_release);
  queueCv_.notify_all();
  if (workerThread_.joinable())
    workerThread_.join();
}

// --------------------------------------------------
// 从注册表读取安装时授予的遥测授权状态
//
// 注册表路径（仅 Windows）：
//   HKCU\Software\iisaacbeats\Y2Kmeter
//   "TelemetryEnabled" = REG_DWORD: 1（已授权）/ 0 或不存在（未授权）
//
// VST3 无独立安装程序，直接复用同一注册表键值。
// 若用户仅安装 VST3 而未运行过 Standalone 安装包，
// 则注册表无此键 → enabled_ 保持默认 false → 不发送遥测。
// --------------------------------------------------
void TelemetryClient::LoadFromRegistry() {
#if JUCE_WINDOWS
  HKEY hKey = nullptr;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\iisaacbeats\\Y2Kmeter", 0, KEY_READ, &hKey)
      == ERROR_SUCCESS) {
    DWORD value = 0;
    DWORD size  = sizeof(value);
    if (RegQueryValueExA(hKey, "TelemetryEnabled", nullptr, nullptr,
          reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
      SetEnabled(value != 0);
    } else {
      SetEnabled(false);
    }
    RegCloseKey(hKey);
  } else {
    SetEnabled(false);
  }
#else
  // 非 Windows 平台暂不支持注册表授权，默认禁用
  SetEnabled(false);
#endif
}

// --------------------------------------------------
// 内部隐私开关设置方法
// --------------------------------------------------
void TelemetryClient::SetEnabled(bool enabled) noexcept {
  enabled_.store(enabled, std::memory_order_release);
}

// --------------------------------------------------
// id 持久化 key
// --------------------------------------------------
static constexpr const char* kClientIdKey = "telemetry.clientId";

// --------------------------------------------------
// 生成或读取匿名 client_id
// --------------------------------------------------
juce::String TelemetryClient::GetOrCreateClientId(
    juce::PropertiesFile* settings) {
  if (settings != nullptr) {
    if (settings->containsKey(kClientIdKey)) {
      return settings->getValue(kClientIdKey);
    }
    const auto id = juce::Uuid().toString();
    settings->setValue(kClientIdKey, id);
    settings->saveIfNeeded();
    return id;
  }
  // 无持久化存储时使用临时 UUID（仅本次运行有效）
  return juce::Uuid().toString();
}

// --------------------------------------------------
// 收集系统信息
// --------------------------------------------------
juce::var TelemetryClient::CollectSystemInfo(
    juce::PropertiesFile* settings,
    bool isPlugin) {
  using juce::SystemStats;

  juce::DynamicObject::Ptr obj = new juce::DynamicObject();

  // 匿名 client_id
  obj->setProperty("client_id", GetOrCreateClientId(settings));

  // 软件版本
  obj->setProperty("version", juce::String(JucePlugin_VersionString));

  // 构建类型（从 CMAKE_BUILD_TYPE 宏推导；若未定义则按 NDEBUG 判断）
#if defined(CMAKE_BUILD_TYPE)
  obj->setProperty("build_type", juce::String(CMAKE_BUILD_TYPE).toLowerCase());
#else
 #ifdef NDEBUG
  obj->setProperty("build_type", "release");
 #else
  obj->setProperty("build_type", "debug");
 #endif
#endif

  // 平台
#if JUCE_WINDOWS
  obj->setProperty("platform", "win-x64");
#elif JUCE_MAC
  obj->setProperty("platform", "macos");
#elif JUCE_LINUX
  obj->setProperty("platform", "linux");
#else
  obj->setProperty("platform", "unknown");
#endif

  // 操作系统
  obj->setProperty("os", SystemStats::getOperatingSystemName());

  // CPU 信息（JUCE 8 不提供 getCpuArchitecture，用 getCpuVendor + getCpuModel 组合）
  {
    const auto vendor = SystemStats::getCpuVendor();
    const auto model  = SystemStats::getCpuModel();
    juce::String cpuDesc;
    if (vendor.isNotEmpty() && model.isNotEmpty())
      cpuDesc = vendor + " " + model;
    else if (vendor.isNotEmpty())
      cpuDesc = vendor;
    else
      cpuDesc = model;
    obj->setProperty("cpu_desc", cpuDesc);
  }
  obj->setProperty("cpu_cores", SystemStats::getNumCpus());

  // 物理内存 (MB)，仅用于性能分档，不涉及任何身份
  obj->setProperty("ram_mb",
    static_cast<int>(SystemStats::getMemorySizeInMegabytes()));

  // 运行模式
  obj->setProperty("plugin_mode", isPlugin ? "plugin" : "standalone");

  // 宿主名称（仅插件模式下有意义；Standalone 下为空）
  if (isPlugin) {
    // PluginHostType::getHostDescription() 是非静态方法，需要实例
    obj->setProperty("host_name",
      juce::PluginHostType{}.getHostDescription());
  } else {
    obj->setProperty("host_name", "Standalone");
  }

  // 显示器数量与主屏分辨率
  {
    const auto& displays =
        juce::Desktop::getInstance().getDisplays();
    obj->setProperty("display_count", displays.displays.size());
    if (displays.getPrimaryDisplay() != nullptr) {
      const auto area = displays.getPrimaryDisplay()->userArea;
      obj->setProperty("primary_display_w", area.getWidth());
      obj->setProperty("primary_display_h", area.getHeight());
    }
  }

  // 安装来源标记
  obj->setProperty("source_tag",
    juce::String(JucePlugin_Manufacturer));

  // 语言/地区（仅系统级别的 locale 名，不含任何个人定制）
  obj->setProperty("system_locale",
    SystemStats::getUserLanguage() + "_" + SystemStats::getUserRegion());

  // 时区偏移（分钟），仅用于评估用户地理分布，精确到时区级别
  obj->setProperty("timezone_offset_min",
    juce::Time::getCurrentTime().getUTCOffsetSeconds() / 60);

  return juce::var(obj.get());
}

// --------------------------------------------------
// 异步 POST JSON
//
// 只在主调线程做 JSON 序列化并投递任务到队列；真正的网络 I/O 由常驻
// worker 线程串行执行。POST 数据通过 URL::withPOSTData() 挂载，配合
// InputStreamOptions(ParameterHandling::inPostData) 发出。
// --------------------------------------------------
void TelemetryClient::PostJsonAsync(const juce::URL& url,
                                     const juce::var& json) {
  // 在主调线程先把 JSON 序列化为字符串（避免跨线程引用临时对象）
  const auto body = juce::JSON::toString(json, false);

  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    tasks_.emplace_back([url, body]() {
      // 将 POST 数据挂到 URL 上
      const auto postUrl = url.withPOSTData(body);

      // 创建带 POST 数据的 stream options
      auto opts = juce::URL::InputStreamOptions(
                    juce::URL::ParameterHandling::inPostData)
                    .withConnectionTimeoutMs(5000)
                    .withExtraHeaders(
                        "Content-Type: application/json\r\n"
                        "Accept: application/json\r\n")
                    .withHttpRequestCmd("POST");

      // 尝试发送，任何错误直接吞掉
      if (auto stream = postUrl.createInputStream(opts)) {
        // 读取响应体以触发完整 HTTP 事务（即使我们不需要响应内容）
        stream->readEntireStreamAsString();
      }
      // 静默忽略所有错误（网络不通、DNS 失败、超时等）
    });
  }
  queueCv_.notify_one();
}

// --------------------------------------------------
// 后台 worker 主循环
// --------------------------------------------------
void TelemetryClient::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait(lock, [this]() {
        return stopWorker_.load(std::memory_order_acquire) || !tasks_.empty();
      });
      if (stopWorker_.load(std::memory_order_acquire) && tasks_.empty())
        return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

// --------------------------------------------------
// 发送启动心跳
// --------------------------------------------------
void TelemetryClient::SendStartupPing(juce::PropertiesFile* settings,
                                       bool isPlugin) {
  if (!IsEnabled()) return;

  // 采集阶段在主调线程完成，避免在后台线程触碰 juce API
  const auto payload = CollectSystemInfo(settings, isPlugin);

  const auto url = juce::URL(
    "https://iisaacbeats.cn/api/telemetry/ping");
  PostJsonAsync(url, payload);
}

}  // namespace network
}  // namespace y2k
