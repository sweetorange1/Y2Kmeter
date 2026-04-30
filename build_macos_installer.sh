#!/usr/bin/env bash
# ==============================================================================
# Y2Kmeter · macOS 打包脚本（DMG 安装镜像）
#
# 前置条件：
#   在 IDE（CLion 等）里已经用 Release 配置把 Y2Kmeter_Standalone 与 Y2Kmeter_VST3
#   两个 target 构建出来，产物应位于：
#     cmake-build-release/Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter.app
#     cmake-build-release/Y2Kmeter_artefacts/Release/VST3/Y2Kmeter.vst3
#   本脚本不再负责触发 cmake 构建，只基于现有产物做签名 + 打包。
#
# 功能：
#   1. 校验 Standalone (.app) 与 VST3 (.vst3) 产物是否已经存在
#   2. 对两个 bundle 做 ad-hoc 代码签名（codesign --sign -），确保 macOS 不会因未签名
#      直接 Gatekeeper 拦截，也让 LaunchServices 正确读取 Icon
#   3. 强制刷新 LaunchServices / Dock 图标缓存，避免本地测试时继续显示"田字格"
#      默认图标（从 .app/Contents/MacOS/二进制 直接启动会绕过 LS 注册 → Dock 退回占位图标）
#   4. 组装一个 DMG 镜像，内含：
#        • Y2Kmeter.app                （Standalone 可执行，拖到 /Applications）
#        • Y2Kmeter.vst3               （VST3 插件，拖到 /Library/Audio/Plug-Ins/VST3）
#        • Applications 符号链接       （方便用户拖拽安装 Standalone）
#        • VST3 符号链接               （方便用户拖拽安装 VST3）
#        • README.txt                  （安装说明）
#
# 使用：
#   chmod +x build_macos_installer.sh
#   ./build_macos_installer.sh                     # 完整打包流程
#   ./build_macos_installer.sh --no-sign           # 跳过 ad-hoc 签名
#   ./build_macos_installer.sh --version 1.6.0     # 覆盖版本号（默认读 CMakeLists）
#
# 产物：
#   dist/Y2Kmeter-<version>-macOS.dmg
# ==============================================================================
set -euo pipefail

# ---- 路径常量 -----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"
BUILD_DIR="${PROJECT_ROOT}/cmake-build-release"
ARTEFACTS_DIR="${BUILD_DIR}/Y2Kmeter_artefacts/Release"
DIST_DIR="${PROJECT_ROOT}/dist"
STAGE_DIR="${DIST_DIR}/.dmg_stage"

PRODUCT_NAME="Y2Kmeter"
APP_BUNDLE_NAME="${PRODUCT_NAME}.app"
VST3_BUNDLE_NAME="${PRODUCT_NAME}.vst3"

# ---- 参数解析 -----------------------------------------------------------------
DO_SIGN=1
VERSION=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-sign)    DO_SIGN=0; shift ;;
    --version)    VERSION="${2:-}"; shift 2 ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "[build_macos_installer] 未知参数：$1" >&2
      exit 1
      ;;
  esac
done

# ---- 版本号：若未显式传入，从 CMakeLists 的 juce_add_plugin(... VERSION x.y.z) 抓取
if [[ -z "${VERSION}" ]]; then
  VERSION="$(awk '
    /juce_add_plugin/,/\)/ {
      if ($1 == "VERSION") { print $2; exit }
    }
  ' "${PROJECT_ROOT}/CMakeLists.txt" | tr -d '"' )"
fi
if [[ -z "${VERSION}" ]]; then
  echo "[build_macos_installer] 无法识别 VERSION，请 --version 1.x.y 显式指定" >&2
  exit 1
fi

DMG_NAME="${PRODUCT_NAME}-${VERSION}-macOS.dmg"
DMG_PATH="${DIST_DIR}/${DMG_NAME}"
DMG_VOLNAME="${PRODUCT_NAME} ${VERSION}"

log() { echo "[build_macos_installer] $*"; }

# ---- 环境检查 -----------------------------------------------------------------
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "此脚本仅支持在 macOS 上执行" >&2
  exit 1
fi
command -v hdiutil  >/dev/null || { echo "缺少 hdiutil";  exit 1; }
command -v codesign >/dev/null || { echo "缺少 codesign"; exit 1; }

# ---- Step 1: 校验 IDE 已构建出的产物 -----------------------------------------
#   本脚本不再触发 cmake 构建，用户需在 IDE 里以 Release 配置先把
#   Y2Kmeter_Standalone 与 Y2Kmeter_VST3 两个 target 构建出来。
log "Step 1/4 校验 Release 产物"

APP_BUNDLE="${ARTEFACTS_DIR}/Standalone/${APP_BUNDLE_NAME}"
VST3_BUNDLE="${ARTEFACTS_DIR}/VST3/${VST3_BUNDLE_NAME}"

if [[ ! -d "${APP_BUNDLE}" ]]; then
  cat >&2 <<EOF
找不到 Standalone 产物：${APP_BUNDLE}
请先在 IDE（CLion 等）里以 Release 配置构建 target: Y2Kmeter_Standalone
EOF
  exit 1
fi
if [[ ! -d "${VST3_BUNDLE}" ]]; then
  cat >&2 <<EOF
找不到 VST3 产物：${VST3_BUNDLE}
请先在 IDE（CLion 等）里以 Release 配置构建 target: Y2Kmeter_VST3
EOF
  exit 1
fi

log "  · Standalone: ${APP_BUNDLE}"
log "  · VST3     : ${VST3_BUNDLE}"

# ---- Step 2: ad-hoc 代码签名 --------------------------------------------------
#   · 未签名的 app 在 macOS 上首次打开会被 Gatekeeper 拦截，且有时 Dock 图标也会
#     回退到"田字格"占位图；ad-hoc 签名（identity="-"）不提供分发信任，但足以
#     让本机 LaunchServices 将其识别为"合法 bundle"并正确读取 Icon/Info.plist
#   · 带 hardened runtime 以便后续如果要走公证也省一步
#   · Y2Kmeter.vst3 里嵌套了 _CodeSignature 的旧签名也会被 --force 覆盖
if [[ "${DO_SIGN}" -eq 1 ]]; then
  log "Step 2/4 ad-hoc 代码签名 (codesign --sign -)"
  sign_bundle() {
    local bundle="$1"
    codesign --force --deep --sign - \
      --options runtime \
      --timestamp=none \
      "${bundle}"
    codesign --verify --deep --strict --verbose=2 "${bundle}" || true
  }
  sign_bundle "${APP_BUNDLE}"
  sign_bundle "${VST3_BUNDLE}"
else
  log "Step 2/4 跳过签名（--no-sign）"
fi

# ---- Step 3: 刷新 LaunchServices 图标缓存 -------------------------------------
#   只对本机测试有意义；安装到 /Applications 后 Finder/Dock 自然会重新注册
log "Step 3/4 刷新 LaunchServices 注册（仅本机生效）"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
if [[ -x "${LSREGISTER}" ]]; then
  "${LSREGISTER}" -f "${APP_BUNDLE}" >/dev/null 2>&1 || true
fi

# ---- Step 4: 组装 DMG stage 目录并生成 DMG ------------------------------------
log "Step 4/4 组装 DMG 暂存目录"
mkdir -p "${DIST_DIR}"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"

# 拷贝 bundle（ditto 保留扩展属性、符号链接、硬链接、权限位）
ditto "${APP_BUNDLE}"  "${STAGE_DIR}/${APP_BUNDLE_NAME}"
ditto "${VST3_BUNDLE}" "${STAGE_DIR}/${VST3_BUNDLE_NAME}"

# 安装目标的符号链接 —— 让用户在 DMG 里直接拖拽即可安装
ln -s /Applications                       "${STAGE_DIR}/Applications"
ln -s /Library/Audio/Plug-Ins/VST3        "${STAGE_DIR}/VST3 (Library Audio Plug-Ins)"

# README 说明
cat > "${STAGE_DIR}/README.txt" <<README
Y2Kmeter ${VERSION} · macOS 安装说明
==========================================================

本镜像包含两种形态：

  1) ${APP_BUNDLE_NAME}        独立桌面应用（Standalone）
     —— 拖到右侧的 "Applications" 里即可完成安装
        （或直接拖到 /Applications）

  2) ${VST3_BUNDLE_NAME}       VST3 音频插件
     —— 拖到 "VST3 (Library Audio Plug-Ins)" 文件夹
        对应系统路径为：
            /Library/Audio/Plug-Ins/VST3/
        拖进去后重启 DAW（Logic / Ableton / Reaper / Cubase 等）
        并在 DAW 中触发一次插件扫描即可。
        该系统路径需要管理员权限，如被拒绝，也可放到用户目录：
            ~/Library/Audio/Plug-Ins/VST3/

首次打开注意：
  本包采用本地 ad-hoc 签名，未经过 Apple 公证。若打开 ${APP_BUNDLE_NAME}
  时系统提示"无法验证开发者"，请在 Finder 中右键点击 → 选择"打开"，
  然后在弹窗中再次确认"打开"，系统会记住该选择，后续可以直接双击。

卸载：
  直接从 /Applications 删除 ${APP_BUNDLE_NAME}
  直接从 /Library/Audio/Plug-Ins/VST3 删除 ${VST3_BUNDLE_NAME}

README

log "生成 DMG：${DMG_PATH}"
rm -f "${DMG_PATH}"

# 使用 UDZO（压缩）+ HFS+ 文件系统，体积小且兼容所有 macOS 版本
hdiutil create \
  -volname "${DMG_VOLNAME}" \
  -srcfolder "${STAGE_DIR}" \
  -ov \
  -format UDZO \
  -imagekey zlib-level=9 \
  "${DMG_PATH}"

# 如果启用了签名，顺手给 DMG 也加个 ad-hoc 签名
if [[ "${DO_SIGN}" -eq 1 ]]; then
  codesign --force --sign - "${DMG_PATH}" || true
fi

# 清理 stage
rm -rf "${STAGE_DIR}"

log "完成：${DMG_PATH}"
log "DMG 大小：$(du -h "${DMG_PATH}" | awk '{print $1}')"

# ---- 附：本地快速验证提示 ----------------------------------------------------
cat <<POSTINFO

下一步你可以：
  1) 挂载 DMG 做一次冒烟测试：
       open "${DMG_PATH}"

  2) 直接从 Finder 打开 .app（会走正常的 LaunchServices 注册，图标才会正确）：
       open "${APP_BUNDLE}"

  3) 如果你之前从命令行直接跑过 .app/Contents/MacOS/Y2Kmeter 导致 Dock 图标
     被缓存成"田字格"，可以用以下命令清掉缓存并重建：
       killall Dock
       killall Finder
POSTINFO
