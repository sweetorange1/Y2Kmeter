// ==========================================================
// source/network/TelemetryClient.h
//   匿名遥测客户端——启动心跳 + 系统信息采集。
//
//   功能：
//     1. 首次运行自动生成匿名 UUID（存到 PropertiesFile），
//        后续启动复用同一 ID，不包含任何个人身份信息。
//     2. 采集系统元数据（OS 版本、CPU 架构/核心数、宿主名称、
//        软件版本、构建类型等），组装为 JSON。
//     3. 每次启动时异步 POST 到 iisaacbeats.cn/api/telemetry/ping，
//        在后台线程执行，绝不阻塞 UI 或音频线程。
//     4. 授权状态由安装程序写入注册表（HKCU\Software\iisaacbeats\
//        Y2Kmeter\TelemetryEnabled），软件内不提供开关。
//        默认未授权 = 不发送任何网络请求。
//
//   线程安全：
//     · 采集与 JSON 组装在调用线程（UI 线程）完成；
//     · HTTP 发送走 std::thread 后台线程；
//     · enabled_ 标志为 std::atomic，多线程安全读写。
// ==========================================================

#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace y2k {
namespace network {

class TelemetryClient {
public:
  // 全局单例
  static TelemetryClient& GetInstance();

  // 禁止拷贝与移动（单例）
  TelemetryClient(const TelemetryClient&) = delete;
  TelemetryClient& operator=(const TelemetryClient&) = delete;

  // 从 Windows 注册表读取授权状态。
  //   · 读取 HKCU\Software\iisaacbeats\Y2Kmeter\TelemetryEnabled (REG_DWORD)
  //   · 值为 1 → 启用遥测；值为 0 或键不存在 → 禁用（默认未授权）
  //   · 非 Windows 平台直接设为禁用
  //   · 应在启动流程早期调用，替代旧的 PropertiesFile 读取逻辑
  void LoadFromRegistry();

  // 查询是否已启用（线程安全）
  bool IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
  }

  // 发送启动心跳（异步，调用后立即返回）。
  //   · 内部采集系统信息 + 组装 JSON → 投递到后台线程 POST。
  //   · 若 IsEnabled()==false，立即返回。
  //   · 网络错误静默忽略，不弹任何对话框。
  //
  // 参数：
  //   settings  用于持久化/读取匿名 client_id 的 PropertiesFile。
  //             若为 nullptr，则用临时内存 ID（不持久化，仅当次有效）。
  //   isPlugin  区分 Standalone 与 VST3 宿主模式，用于遥测上报。
  void SendStartupPing(juce::PropertiesFile* settings, bool isPlugin);

private:
  TelemetryClient() = default;
  ~TelemetryClient() = default;

  // 内部设置开关（仅供 LoadFromRegistry 等初始化逻辑调用）
  void SetEnabled(bool enabled) noexcept;

  // 读取或首次生成匿名 client_id（存到 settings 的 "telemetry.clientId" key）
  static juce::String GetOrCreateClientId(juce::PropertiesFile* settings);

  // 收集当前环境的系统信息，组装为 juce::var (DynamicObject)
  static juce::var CollectSystemInfo(juce::PropertiesFile* settings,
                                     bool isPlugin);

  // 异步执行 HTTP POST，失败静默
  static void PostJsonAsync(const juce::URL& url, const juce::var& json);

  // 隐私授权标志。默认 false（未授权），仅当 LoadFromRegistry()
  // 读到注册表值为 1 时才设为 true。
  std::atomic<bool> enabled_{false};
};

}  // namespace network
}  // namespace y2k
