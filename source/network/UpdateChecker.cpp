// ==========================================================
// source/network/UpdateChecker.cpp
//   软件更新检查实现。
//
//   工作流程：
//     1. 构造 GET URL → 投递到后台线程执行 HTTP 请求
//     2. 解析服务端 JSON 响应 → 提取 UpdateInfo
//     3. 通过 MessageManager::callAsync 切回主线程执行回调
//     4. 回调中若 has_update==true，弹 UpdateDialog 提示用户
//
//   后台线程使用 std::thread + detach（JUCE 8 无默认全局线程池）。
// ==========================================================

#include "UpdateChecker.h"
#include "Version.h"
#include "TelemetryClient.h"
#include "source/ui/UpdateDialog.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <thread>

namespace y2k {
namespace network {

namespace {

// 从服务端 JSON 解析 UpdateInfo
UpdateInfo ParseUpdateResponse(const juce::var& json) {
  UpdateInfo info;
  if (auto* obj = json.getDynamicObject()) {
    info.has_update     = obj->getProperty("has_update");
    info.latest_version = obj->getProperty("latest_version").toString();
    info.download_url   = obj->getProperty("download_url").toString();
    info.changelog      = obj->getProperty("changelog").toString();
    info.force_update   = obj->getProperty("force_update");
  }
  return info;
}

}  // namespace

// ================================================================
// 公开函数
// ================================================================

// --------------------------------------------------
// ShowUpdateDialog（公开，供 Standalone/VST3 入口调用）
//
//   使用自定义 PinkXP 风格 UpdateDialog 替代系统原生对话框，
//   保持与软件整体视觉风格一致。
//   · addToDesktop 创建独立原生小窗口（480×340）+ setAlwaysOnTop 确保置顶
//   · 两个操作按钮：Download / Remind Me Later
//   · 弹窗退出后自删除（removeFromDesktop + MessageManager::callAsync）
// --------------------------------------------------
void ShowUpdateDialog(const UpdateInfo& info,
                      juce::PropertiesFile* settings) {
  // 确保在主线程
  jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

  // 优先使用自定义 PinkXP 风格弹窗。
  // VST3 插件模式下 Editor 可能尚未打开，若找不到父组件则回退到系统原生对话框。
  if (y2k::ui::UpdateDialog::ShowInComponent(
          /*parentComponent=*/nullptr, info, settings) != nullptr) {
    return;
  }

  // 回退路径：无可用 UI 组件时使用 NativeMessageBox
  juce::String message;
  message << "A new version of Y2Kmeter is available!\n\n"
          << "Current: " << JucePlugin_VersionString << "\n"
          << "Latest:  " << info.latest_version << "\n\n";

  if (info.changelog.isNotEmpty()) {
    message << "What's new:\n" << info.changelog << "\n\n";
  }

  message << "Would you like to download it?";

  juce::NativeMessageBox::showAsync(
    juce::MessageBoxOptions()
      .withIconType(info.force_update
                      ? juce::MessageBoxIconType::WarningIcon
                      : juce::MessageBoxIconType::QuestionIcon)
      .withTitle("Update Available")
      .withMessage(message)
      .withButton("Download")
      .withButton("Remind Me Later"),
    [info](int result) {
      if (result == 1) {
        if (info.download_url.isNotEmpty()) {
          juce::URL(info.download_url).launchInDefaultBrowser();
        } else {
          juce::URL("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
      }
    });
}

// --------------------------------------------------
// CheckForUpdatesAsync
//
//   进程级去重：static atomic 确保同一进程内仅执行一次更新检查，
//   避免 Standalone 模式下 PluginProcessor.ctor 与 StandaloneApp::initialise()
//   各自独立触发导致重复 HTTP 请求和重复弹窗。
// --------------------------------------------------
void CheckForUpdatesAsync(
    const juce::String& current_version,
    const juce::String& platform,
    juce::PropertiesFile* settings,
    std::function<void(const UpdateInfo&)> callback) {
  static std::atomic<bool> s_checked_this_session{false};
  if (s_checked_this_session.exchange(true, std::memory_order_acquire)) {
    // 已经由其他入口触发过更新检查，直接返回空结果
    if (callback) {
      juce::MessageManager::callAsync([callback]() {
        callback(UpdateInfo{});
      });
    }
    return;
  }

  if (!TelemetryClient::GetInstance().IsEnabled()) {
    if (callback) {
      juce::MessageManager::callAsync([callback]() {
        callback(UpdateInfo{});
      });
    }
    return;
  }

  juce::String urlStr =
    "https://iisaacbeats.cn/api/update/check";
  urlStr << "?version=" << juce::URL::addEscapeChars(current_version, false)
         << "&platform=" << juce::URL::addEscapeChars(platform, false);
  const auto url = juce::URL(urlStr);

  // 后台线程执行 HTTP GET
  std::thread([url, callback]() {
    UpdateInfo info;

    auto opts = juce::URL::InputStreamOptions(
                  juce::URL::ParameterHandling::inAddress)
                  .withConnectionTimeoutMs(5000)
                  .withExtraHeaders("Accept: application/json\r\n")
                  .withHttpRequestCmd("GET");

    if (auto stream = url.createInputStream(opts)) {
      const auto body = stream->readEntireStreamAsString();

      if (body.isNotEmpty()) {
        const auto json = juce::JSON::parse(body);
        info = ParseUpdateResponse(json);
      }
    }

    if (info.has_update && info.latest_version.isNotEmpty()) {
      const int cmp = CompareVersionStrings(
          juce::String(JucePlugin_VersionString),
          info.latest_version);
      if (cmp >= 0) {
        info.has_update = false;
      }
    }

    if (callback) {
      juce::MessageManager::callAsync(
        [callback, info]() { callback(info); });
    }
  }).detach();
}

}  // namespace network
}  // namespace y2k
