// ==========================================================
// source/network/UpdateChecker.h
//   软件更新检查模块。
//
//   启动时异步 GET iisaacbeats.cn/api/update/check?version=X&platform=Y，
//   服务端返回 JSON 告知是否有新版本。若有，在主线程弹窗提示用户；
//   用户可选择"立即下载"（打开浏览器）或"稍后提醒"。
//
//   线程安全：
//     · HTTP 请求在后台线程执行；
//     · 回调通过 MessageManager::callAsync 切回主线程；
//     · 回调中的 UI 操作在主线程安全。
// ==========================================================

#pragma once

#include <JuceHeader.h>
#include <functional>

namespace y2k {
namespace network {

// 服务端返回的更新信息
struct UpdateInfo {
  bool        has_update = false;     // 是否有新版本
  juce::String latest_version;        // 最新版本号，如 "2.4.0"
  juce::String download_url;          // 下载链接
  juce::String changelog;             // 更新日志（纯文本或简单 Markdown）
  bool        force_update = false;   // 强制更新（严重 bug 修复）
};

// 异步检查更新，完成后在主线程回调。
//
// 参数：
//   current_version  当前软件版本字符串，如 "2.3.2"
//   platform         平台标识，如 "win-x64" / "macos"
//   settings         用于弹窗的 PropertiesFile，传 nullptr 则不可用
//   callback         检查完成后的回调，在主线程执行。
//                    参数为 UpdateInfo 结构体。
//
// 行为：
//   · 若 PrivacyClient::IsEnabled()==false（用户关闭了隐私统计），
//     则跳过更新检查（callback 收到 has_update=false）。
//   · 网络超时 5 秒，失败或超时静默返回 has_update=false。
void CheckForUpdatesAsync(
    const juce::String& current_version,
    const juce::String& platform,
    juce::PropertiesFile* settings,
    std::function<void(const UpdateInfo&)> callback);

// 展示更新对话框（必须在主线程调用）。
//   · 包含"Download"、"Remind Me Later"两个按钮
//   · "Download" → 在默认浏览器打开 download_url
//   · "Remind" → 关闭对话框，下次启动仍会提醒
void ShowUpdateDialog(const UpdateInfo& info,
                      juce::PropertiesFile* settings);

}  // namespace network
}  // namespace y2k
