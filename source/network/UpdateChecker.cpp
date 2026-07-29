// ==========================================================
// source/network/UpdateChecker.cpp
//   软件更新检查实现。
//
//   工作流程：
//     1. 构造 GET URL → 投递到后台线程执行 HTTP 请求
//     2. 解析服务端 JSON 响应 → 提取 UpdateInfo
//     3. 与本地"忽略版本"比较 → 若已被忽略则 has_update=false
//     4. 通过 MessageManager::callAsync 切回主线程执行回调
//     5. 回调中若 has_update==true，弹 AlertWindow 提示用户
//
//   后台线程使用 std::thread + detach（JUCE 8 无默认全局线程池）。
// ==========================================================

#include "UpdateChecker.h"
#include "Version.h"
#include "TelemetryClient.h"

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

// "update.ignoredVersion" 持久化 key
static constexpr const char* kIgnoredVersionKey = "update.ignoredVersion";

}  // namespace

// ================================================================
// 公开函数
// ================================================================

// --------------------------------------------------
// ShowUpdateDialog（公开，供 Standalone/VST3 入口调用）
// --------------------------------------------------
void ShowUpdateDialog(const UpdateInfo& info,
                      juce::PropertiesFile* settings) {
  // 确保在主线程
  jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

  juce::String message;
  message << "A new version of Y2Kmeter is available!\n\n"
          << "Current: " << JucePlugin_VersionString << "\n"
          << "Latest:  " << info.latest_version << "\n\n";

  if (info.changelog.isNotEmpty()) {
    message << "What's new:\n" << info.changelog << "\n\n";
  }

  message << "Would you like to download it?";

  // 使用 showAsync 替代 show：后者在 JUCE_MODAL_LOOPS_PERMITTED 未定义时不可用。
  // showAsync 为 3 按钮返回：button[0]=1, button[1]=2, button[2]=0,
  // 恰好对应 Download(1) / Ignore(2) / Remind(0)。
  juce::NativeMessageBox::showAsync(
    juce::MessageBoxOptions()
      .withIconType(info.force_update
                      ? juce::MessageBoxIconType::WarningIcon
                      : juce::MessageBoxIconType::QuestionIcon)
      .withTitle("Update Available")
      .withMessage(message)
      .withButton("Download")
      .withButton("Ignore This Version")
      .withButton("Remind Me Later"),
    [info, settings](int result) {
      if (result == 1) {
        if (info.download_url.isNotEmpty()) {
          juce::URL(info.download_url).launchInDefaultBrowser();
        } else {
          juce::URL("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
      } else if (result == 2) {
        if (settings != nullptr) {
          IgnoreVersion(settings, info.latest_version);
        }
      }
      // result == 0 → "Remind Me Later": 什么都不做
    });
}

// --------------------------------------------------
// CheckForUpdatesAsync
// --------------------------------------------------
void CheckForUpdatesAsync(
    const juce::String& current_version,
    const juce::String& platform,
    juce::PropertiesFile* settings,
    std::function<void(const UpdateInfo&)> callback) {
  if (!TelemetryClient::GetInstance().IsEnabled()) {
    if (callback) {
      juce::MessageManager::callAsync([callback]() {
        callback(UpdateInfo{});
      });
    }
    return;
  }

  // 在主调线程预读"已忽略版本号"——settings 可能指向局部
  // ApplicationProperties 对象（如 VST3 PluginProcessor 中），
  // 若在后台线程访问，该对象可能已被析构（UAF）。
  juce::String ignoredVersion;
  if (settings != nullptr) {
    ignoredVersion = settings->getValue(kIgnoredVersionKey);
  }

  juce::String urlStr =
    "https://iisaacbeats.cn/api/update/check";
  urlStr << "?version=" << juce::URL::addEscapeChars(current_version, false)
         << "&platform=" << juce::URL::addEscapeChars(platform, false);
  const auto url = juce::URL(urlStr);

  // 后台线程执行 HTTP GET（使用 std::thread 替代不存在的 ThreadPool::getDefaultJobPool）
  std::thread([url, callback, ignoredVersion]() {
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

      if (info.has_update && ignoredVersion.isNotEmpty()
          && ignoredVersion == info.latest_version) {
        info.has_update = false;
      }
    }

    if (callback) {
      juce::MessageManager::callAsync(
        [callback, info]() { callback(info); });
    }
  }).detach();
}

// --------------------------------------------------
// IgnoreVersion
// --------------------------------------------------
void IgnoreVersion(juce::PropertiesFile* settings,
                   const juce::String& version) {
  if (settings != nullptr && version.isNotEmpty()) {
    settings->setValue(kIgnoredVersionKey, version);
    settings->saveIfNeeded();
  }
}

}  // namespace network
}  // namespace y2k
