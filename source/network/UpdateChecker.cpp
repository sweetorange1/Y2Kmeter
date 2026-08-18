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
#include "source/Y2KLogging.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <mutex>
#include <thread>
#include <vector>

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

// --------------------------------------------------
// 后台线程登记器
//
// 更新检查是命名空间级自由函数，没有对象可挂线程句柄。这里用一个函数内
// static 的登记器持有所有进行中的后台线程，进程退出（static 析构阶段）时
// 统一 join，取代原先 std::thread(...).detach() 导致线程游离、被 OS 强制
// 终止的问题。join 最多等待网络超时（5 秒）。
// --------------------------------------------------
struct BackgroundThreadRegistry {
  ~BackgroundThreadRegistry() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& t : threads)
      if (t.joinable())
        t.join();
  }

  std::mutex mutex;
  std::vector<std::thread> threads;
};

BackgroundThreadRegistry& GetBackgroundThreadRegistry() {
  static BackgroundThreadRegistry registry;
  return registry;
}

void LaunchBackgroundThread(std::function<void()> fn) {
  auto& registry = GetBackgroundThreadRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.threads.emplace_back(std::move(fn));
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
  auto* dlg = y2k::ui::UpdateDialog::ShowInComponent(
          /*parentComponent=*/nullptr, info, settings);
  Y2K_LOG("[UpdateCheck] ShowInComponent returned: " + juce::String(dlg ? "DIALOG" : "NULL"));
  if (dlg != nullptr) {
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
      Y2K_LOG("[UpdateCheck] NativeMessageBox result=" + juce::String(result));
      // JUCE NativeMessageBox::showAsync 使用 plainIndex 模式（0-based）：
      //   0 = 第一个按钮 (Download)，也可能表示关闭对话框
      //   1 = 第二个按钮 (Remind Me Later)
      if (result == 0) {
        Y2K_LOG("[UpdateCheck] Download button pressed (or dialog closed), opening URL");
        if (info.download_url.isNotEmpty()) {
          juce::URL(info.download_url).launchInDefaultBrowser();
        } else {
          juce::URL("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
      } else {
        Y2K_LOG("[UpdateCheck] Remind Me Later / dismissed, no action");
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

  juce::String urlStr =
    "https://iisaacbeats.cn/api/update/check";
  urlStr << "?version=" << juce::URL::addEscapeChars(current_version, false)
         << "&platform=" << juce::URL::addEscapeChars(platform, false);
  const auto url = juce::URL(urlStr);

  Y2K_LOG("[UpdateCheck] URL: " + urlStr);

  // 后台线程执行 HTTP GET（句柄由登记器持有，进程退出时统一 join）
  LaunchBackgroundThread([url, callback, current_version]() {
    UpdateInfo info;

    auto opts = juce::URL::InputStreamOptions(
                  juce::URL::ParameterHandling::inAddress)
                  .withConnectionTimeoutMs(5000)
                  .withExtraHeaders("Accept: application/json\r\n")
                  .withHttpRequestCmd("GET");

    if (auto stream = url.createInputStream(opts)) {
      const auto body = stream->readEntireStreamAsString();
      Y2K_LOG("[UpdateCheck] Response body: " + body);

      if (body.isNotEmpty()) {
        const auto json = juce::JSON::parse(body);
        info = ParseUpdateResponse(json);
        Y2K_LOG("[UpdateCheck] Parsed: has_update=" + juce::String(info.has_update ? "1" : "0")
            + " latest_version=" + info.latest_version);
      }
    } else {
      Y2K_LOG("[UpdateCheck] HTTP request FAILED (stream is null)");
    }

    if (info.has_update && info.latest_version.isNotEmpty()) {
      const int cmp = CompareVersionStrings(
          current_version,
          info.latest_version);
      Y2K_LOG("[UpdateCheck] Compare: current=" + current_version
          + " latest=" + info.latest_version + " cmp=" + juce::String(cmp));
      if (cmp >= 0) {
        info.has_update = false;
        Y2K_LOG("[UpdateCheck] has_update set to FALSE (current >= latest)");
      }
    }

    Y2K_LOG("[UpdateCheck] Final: has_update=" + juce::String(info.has_update ? "1" : "0")
        + " firing callback=" + juce::String(callback ? "YES" : "NO"));

    if (callback) {
      juce::MessageManager::callAsync(
        [callback, info]() { callback(info); });
    }
  });
}

}  // namespace network
}  // namespace y2k
