#!/usr/bin/env bash
# ==============================================================================
# Y2Kmeter · macOS 打包脚本（DMG 安装镜像，带可视化拖拽引导）
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
#   2. 对两个 bundle 做 ad-hoc 代码签名（codesign --sign -），让 macOS 不会因未签名
#      直接 Gatekeeper 拦截，也让 LaunchServices 正确读取 Icon
#   3. 刷新本机 LaunchServices / Dock 图标缓存
#   4. 用 scripts/macos_dmg_background.m 生成一张带箭头的引导背景图
#   5. 创建 UDRW 可写 DMG → osascript 摆好图标/背景图/窗口尺寸 → 转 UDZO 只读
#      用户双击 DMG 时会看到：
#           Y2Kmeter.app  ──▶  Applications
#           Y2Kmeter.vst3 ──▶  VST3 Plug-Ins
#      两行清晰的拖拽引导。
#
# 使用：
#   chmod +x build_macos_installer.sh
#   ./build_macos_installer.sh                  # 完整打包流程
#   ./build_macos_installer.sh --no-sign        # 跳过 ad-hoc 签名
#   ./build_macos_installer.sh --version 1.6.0  # 覆盖版本号（默认读 CMakeLists）
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
TOOLS_DIR="${DIST_DIR}/.tools"

PRODUCT_NAME="Y2Kmeter"
APP_BUNDLE_NAME="${PRODUCT_NAME}.app"
VST3_BUNDLE_NAME="${PRODUCT_NAME}.vst3"

# DMG 视觉布局常量 —— **必须与 scripts/macos_dmg_background.m 里的坐标保持一致**。
# 图标坐标是 Finder 窗口坐标（原点在左上角、y 向下增长）。
WINDOW_W=720
WINDOW_H=460
ICON_SIZE=128
TEXT_SIZE=12
ROW1_Y=170    # 上行（Y2Kmeter.app → Applications）
ROW2_Y=290    # 下行（Y2Kmeter.vst3 → VST3 Plug-Ins）
LEFT_X=150    # 左侧源 bundle
RIGHT_X=570   # 右侧安装目标链接

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
DMG_RW_PATH="${DIST_DIR}/.${PRODUCT_NAME}-${VERSION}-rw.dmg"
DMG_VOLNAME="${PRODUCT_NAME} ${VERSION}"

log() { echo "[build_macos_installer] $*"; }

# ---- 环境检查 -----------------------------------------------------------------
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "此脚本仅支持在 macOS 上执行" >&2
  exit 1
fi
command -v hdiutil  >/dev/null || { echo "缺少 hdiutil";  exit 1; }
command -v codesign >/dev/null || { echo "缺少 codesign"; exit 1; }
command -v clang    >/dev/null || { echo "缺少 clang";    exit 1; }
command -v osascript>/dev/null || { echo "缺少 osascript";exit 1; }

# ---- Step 1: 校验 IDE 已构建出的产物 -----------------------------------------
log "Step 1/5 校验 Release 产物"

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
if [[ "${DO_SIGN}" -eq 1 ]]; then
  log "Step 2/5 ad-hoc 代码签名 (codesign --sign -)"
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
  log "Step 2/5 跳过签名（--no-sign）"
fi

# ---- Step 3: 刷新 LaunchServices 图标缓存 -------------------------------------
log "Step 3/5 刷新 LaunchServices 注册（仅本机生效）"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
if [[ -x "${LSREGISTER}" ]]; then
  "${LSREGISTER}" -f "${APP_BUNDLE}" >/dev/null 2>&1 || true
fi

# ---- Step 4: 生成 DMG 引导背景图 ----------------------------------------------
#   · scripts/macos_dmg_background.m 是一个一次性 Objective-C 工具，用 CoreGraphics
#     画出带箭头的安装引导图，输出 @1x / @2x 两张 PNG。
#   · 每次脚本都重新编译一次，免得本地工具二进制和源码脱节；编译产物放在
#     dist/.tools/，随 stage 清理一起消失。
log "Step 4/5 生成 DMG 引导背景图"
mkdir -p "${TOOLS_DIR}"
DMG_BG_TOOL_SRC="${PROJECT_ROOT}/scripts/macos_dmg_background.m"
DMG_BG_TOOL_BIN="${TOOLS_DIR}/macos_dmg_background"

if [[ ! -f "${DMG_BG_TOOL_SRC}" ]]; then
  echo "找不到背景图工具源码：${DMG_BG_TOOL_SRC}" >&2
  exit 1
fi

clang -framework AppKit \
      -framework CoreGraphics \
      -framework ImageIO \
      -framework CoreFoundation \
      -framework Foundation \
      -fobjc-arc -O2 \
      "${DMG_BG_TOOL_SRC}" \
      -o "${DMG_BG_TOOL_BIN}"

DMG_BG_BASE="${TOOLS_DIR}/dmg_background"
"${DMG_BG_TOOL_BIN}" "${DMG_BG_BASE}" "${PRODUCT_NAME}" "${VERSION}" >/dev/null

DMG_BG_1X="${DMG_BG_BASE}.png"
DMG_BG_2X="${DMG_BG_BASE}@2x.png"

if [[ ! -f "${DMG_BG_1X}" || ! -f "${DMG_BG_2X}" ]]; then
  echo "背景图生成失败：缺少 ${DMG_BG_1X} 或 ${DMG_BG_2X}" >&2
  exit 1
fi

# ---- Step 5: 组装 DMG ---------------------------------------------------------
log "Step 5/5 组装 DMG（UDRW → 布局 → UDZO）"

mkdir -p "${DIST_DIR}"
rm -rf  "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"

# Step 5.1 准备 stage 目录 —— 与最终 DMG 内容完全一致
ditto "${APP_BUNDLE}"  "${STAGE_DIR}/${APP_BUNDLE_NAME}"
ditto "${VST3_BUNDLE}" "${STAGE_DIR}/${VST3_BUNDLE_NAME}"

# 安装目标的符号链接 —— 让用户在 DMG 里直接拖拽即可安装
ln -s /Applications                       "${STAGE_DIR}/Applications"
ln -s /Library/Audio/Plug-Ins/VST3        "${STAGE_DIR}/VST3 Plug-Ins"

# 背景图放到 .background/（Finder 习惯，并且 Finder 对隐藏目录的 resolve 最稳）
mkdir -p "${STAGE_DIR}/.background"
cp "${DMG_BG_1X}" "${STAGE_DIR}/.background/background.png"
cp "${DMG_BG_2X}" "${STAGE_DIR}/.background/background@2x.png"

# 简短 README（图示已经表达了拖拽语义，这里只留关键补充说明）
cat > "${STAGE_DIR}/README.txt" <<README
Y2Kmeter ${VERSION} · macOS 安装说明
==========================================================

安装方式（窗口里已有可视化引导）：

  • Standalone（桌面应用）：
      把 ${APP_BUNDLE_NAME} 拖到 "Applications"
      系统路径为 /Applications

  • VST3 插件：
      把 ${VST3_BUNDLE_NAME} 拖到 "VST3 Plug-Ins"
      系统路径为 /Library/Audio/Plug-Ins/VST3/
      若该路径因权限问题拒绝写入，可改放：
          ~/Library/Audio/Plug-Ins/VST3/

首次打开注意：
  本包采用本地 ad-hoc 签名，未经过 Apple 公证。若打开 ${APP_BUNDLE_NAME}
  时系统提示"无法验证开发者"，请在 Finder 中右键点击 → 选择"打开"，
  然后在弹窗中再次确认"打开"，系统会记住该选择。

卸载：
  从 /Applications 删除 ${APP_BUNDLE_NAME}
  从 /Library/Audio/Plug-Ins/VST3 删除 ${VST3_BUNDLE_NAME}
README

# 让 Finder 少一些无关文件可见
touch "${STAGE_DIR}/.background/.keep"

# Step 5.2 计算 stage 大小（预留 30% 余量，避免"格式化时磁盘不足"）
STAGE_KB=$(du -sk "${STAGE_DIR}" | awk '{print $1}')
DMG_KB=$(( STAGE_KB * 13 / 10 + 2048 ))
log "  · stage=$(du -sh "${STAGE_DIR}" | awk '{print $1}')  → rw dmg ~ ${DMG_KB} KB"

# Step 5.3 创建可写 UDRW DMG 并挂载
#   · 先检查是否有同名残留 Volume 没卸载（多次失败尝试会累积 /Volumes/XXX 1、XXX 2）
for stale in "/Volumes/${DMG_VOLNAME}" "/Volumes/${DMG_VOLNAME} 1" "/Volumes/${DMG_VOLNAME} 2"; do
  if [[ -d "${stale}" ]]; then
    log "  · 清理残留挂载点 ${stale}"
    hdiutil detach "${stale}" -force >/dev/null 2>&1 || true
  fi
done

rm -f "${DMG_RW_PATH}"
hdiutil create \
  -volname "${DMG_VOLNAME}" \
  -srcfolder "${STAGE_DIR}" \
  -ov \
  -format UDRW \
  -fs HFS+ \
  -size "${DMG_KB}k" \
  "${DMG_RW_PATH}" >/dev/null

log "  · attach rw DMG"
ATTACH_OUT="$(hdiutil attach -readwrite -noverify -noautoopen "${DMG_RW_PATH}")"
MOUNT_DEV=$(echo "${ATTACH_OUT}" | awk '/\/dev\// { print $1; exit }')
MOUNT_DIR=$(echo "${ATTACH_OUT}" | sed -n 's|.*\(/Volumes/.*\)$|\1|p' | head -n1)

if [[ -z "${MOUNT_DIR}" || ! -d "${MOUNT_DIR}" ]]; then
  echo "挂载 DMG 失败，无法定位挂载点" >&2
  hdiutil detach "${MOUNT_DEV}" 2>/dev/null || true
  exit 1
fi
log "  · mounted at ${MOUNT_DIR}"

# AppleScript 里用来寻址的 "disk" 名称 —— 必须是挂载点的 basename，
# 不一定等于 DMG_VOLNAME（macOS 遇到同名 volume 会自动加 " 1" 后缀）
MOUNT_VOLNAME="$(basename "${MOUNT_DIR}")"
BG_ABSPATH="${MOUNT_DIR}/.background/background.png"

# Step 5.4 osascript 设定 Finder 窗口视觉布局
#   · AppleScript 里 position 是"图标中心坐标"，origin 在窗口左上角、y 向下。
#   · 背景图用 "POSIX file <绝对路径>" 的写法最稳（新版 Finder 对冒号路径语法
#     经常报 -10006）。
log "  · 调整 Finder 窗口视觉布局（背景图 + 图标位置）"

osascript <<APPLESCRIPT
tell application "Finder"
    tell disk "${MOUNT_VOLNAME}"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set sidebar width of container window to 0
        set the bounds of container window to {200, 120, 200 + ${WINDOW_W}, 120 + ${WINDOW_H}}
        set theViewOptions to the icon view options of container window
        set arrangement of theViewOptions to not arranged
        set icon size of theViewOptions to ${ICON_SIZE}
        set text size of theViewOptions to ${TEXT_SIZE}
        set background picture of theViewOptions to POSIX file "${BG_ABSPATH}"
        set position of item "${APP_BUNDLE_NAME}" of container window to {${LEFT_X},  ${ROW1_Y}}
        set position of item "Applications"             of container window to {${RIGHT_X}, ${ROW1_Y}}
        set position of item "${VST3_BUNDLE_NAME}" of container window to {${LEFT_X},  ${ROW2_Y}}
        set position of item "VST3 Plug-Ins"            of container window to {${RIGHT_X}, ${ROW2_Y}}
        -- README 放到窗口下方，靠左，不抢主视觉
        try
            set position of item "README.txt" of container window to {${LEFT_X}, ${WINDOW_H} - 70}
        end try
        update without registering applications
        delay 1
        close
    end tell
end tell
APPLESCRIPT

# 等一下让 .DS_Store 落盘
sync
sleep 1

# Step 5.5 签名 .DS_Store 所在的 volume、卸载
log "  · detach rw DMG"
hdiutil detach "${MOUNT_DEV}" >/dev/null

# Step 5.6 转为只读压缩 UDZO
log "  · convert UDRW → UDZO"
rm -f "${DMG_PATH}"
hdiutil convert "${DMG_RW_PATH}" \
  -format UDZO \
  -imagekey zlib-level=9 \
  -o "${DMG_PATH}" >/dev/null

# 如果启用了签名，顺手给 DMG 也加个 ad-hoc 签名
if [[ "${DO_SIGN}" -eq 1 ]]; then
  codesign --force --sign - "${DMG_PATH}" || true
fi

# 清理中间件
rm -f  "${DMG_RW_PATH}"
rm -rf "${STAGE_DIR}"
rm -rf "${TOOLS_DIR}"

log "完成：${DMG_PATH}"
log "DMG 大小：$(du -h "${DMG_PATH}" | awk '{print $1}')"

cat <<POSTINFO

下一步你可以：
  1) 挂载 DMG 做一次冒烟测试：
       open "${DMG_PATH}"
     打开后应看到：
       · 粉色背景 + 两条箭头
       · 上行：Y2Kmeter.app     →  Applications
       · 下行：Y2Kmeter.vst3    →  VST3 Plug-Ins
     用户拖拽即可完成安装，不用再读文字说明。

  2) 直接从 Finder 打开 .app（走正常 LaunchServices 注册）：
       open "${APP_BUNDLE}"

  3) 如果之前从命令行直接跑过 .app/Contents/MacOS/Y2Kmeter 导致 Dock
     图标缓存成"田字格"，可用以下命令清掉缓存并重建：
       killall Dock
       killall Finder
POSTINFO
