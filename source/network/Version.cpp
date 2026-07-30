// ==========================================================
// source/network/Version.cpp
//   语义化版本号解析与比较实现。
// ==========================================================

#include "Version.h"

#include <JuceHeader.h>

namespace y2k {
namespace network {

// 解析 "X.Y.Z" 或 "X.Y.Z-prerelease" 或 "X.Y.Z-prerelease+build"
// 解析失败时返回 SemVer{major=-1}
SemVer SemVer::Parse(const juce::String& raw) {
  SemVer v;
  if (raw.isEmpty()) {
    v.major = -1;
    return v;
  }

  auto str = raw.trimStart();

  // 剥离 build 元数据（不参与比较）
  const int plusPos = str.indexOfChar('+');
  if (plusPos >= 0) {
    v.build = str.substring(plusPos + 1);
    str = str.substring(0, plusPos);
  }

  // 剥离预发布标签
  const int dashPos = str.indexOfChar('-');
  if (dashPos >= 0) {
    v.prerelease = str.substring(dashPos + 1);
    str = str.substring(0, dashPos);
  }

  // 解析 major.minor.patch
  juce::StringArray parts;
  parts.addTokens(str, ".", "");
  if (parts.size() < 1 || parts.size() > 4) {
    v.major = -1;
    return v;
  }

  // 尝试将每个部分转为整数，任一失败即整体失败
  auto tryParse = [](const juce::String& s, int& out) -> bool {
    if (s.isEmpty()) return false;
    // 每位都必须是数字
    for (int i = 0; i < s.length(); ++i) {
      if (!juce::CharacterFunctions::isDigit(s[i])) return false;
    }
    out = s.getIntValue();
    return true;
  };

  if (!tryParse(parts[0], v.major)) { v.major = -1; return v; }

  // minor 未提供时为 0
  if (parts.size() >= 2) {
    if (!tryParse(parts[1], v.minor)) { v.major = -1; return v; }
  }

  // patch 未提供时为 0
  if (parts.size() >= 3) {
    if (!tryParse(parts[2], v.patch)) { v.major = -1; return v; }
  }

  return v;
}

juce::String SemVer::ToString() const {
  juce::String s;
  s << major << '.' << minor << '.' << patch;
  if (prerelease.isNotEmpty()) {
    s << '-' << prerelease;
  }
  if (build.isNotEmpty()) {
    s << '+' << build;
  }
  return s;
}

// 比较规则：
//   1. major → minor → patch 逐级比较
//   2. 正式版 > 预发布版
//   3. 两个预发布版之间按字典序比较
int CompareVersions(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return (a.major > b.major) ? 1 : -1;
  if (a.minor != b.minor) return (a.minor > b.minor) ? 1 : -1;
  if (a.patch != b.patch) return (a.patch > b.patch) ? 1 : -1;

  // 一个正式版 vs 一个预发布版 → 正式版更大
  const bool aPre = a.prerelease.isNotEmpty();
  const bool bPre = b.prerelease.isNotEmpty();
  if (!aPre && bPre) return 1;
  if (aPre && !bPre) return -1;

  // 同为预发布或同为正式版 → 比较 pre-release 字段
  if (aPre && bPre) {
    const int cmp = a.prerelease.compare(b.prerelease);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
  }

  return 0;
}

}  // namespace network
}  // namespace y2k
