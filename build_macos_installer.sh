#!/usr/bin/env bash
# ==============================================================================
# Y2Kmeter · macOS 打包脚本（pkg 安装包 + DMG 分发镜像）
#
# 安装形态参考 SpectrumTag（pkgbuild + productbuild）：
#   · 组件一：Y2Kmeter Standalone 应用 —— 必装（对应 Inno Setup 的 Flags: fixed）
#   · 组件二：Y2Kmeter 插件（VST3 + AU）—— 可选，默认勾选
#   · 组件三：Y2Kmeter_milkdrop 插件（VST3 + AU）—— 可选，默认勾选
#   用户在 Installer 的"安装类型"步骤可点"自定义"取消勾选插件组件。
#
# 产物：
#   dist/Y2Kmeter-<version>-macOS.pkg   （Installer 安装包，含组件选择页）
#   dist/Y2Kmeter-<version>-macOS.dmg   （分发用磁盘镜像，内含上面的 pkg）
#
# 前置条件：
#   在 IDE（CLion 等）里已经用 Release 配置把下面 5 个 target 构建出来：
#     cmake-build-release/Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter.app
#     cmake-build-release/Y2Kmeter_artefacts/Release/VST3/Y2Kmeter.vst3
#     cmake-build-release/Y2Kmeter_artefacts/Release/AU/Y2Kmeter.component
#     cmake-build-release/Y2Kmeter_milkdrop_artefacts/Release/VST3/Y2Kmeter_milkdrop.vst3
#     cmake-build-release/Y2Kmeter_milkdrop_artefacts/Release/AU/Y2Kmeter_milkdrop.component
#   本脚本不负责触发 cmake 构建，只基于现有产物做签名 + 打包。
#
# 依赖：command line tools 自带的 ditto / pkgbuild / productbuild / hdiutil / codesign
#
# 使用：
#   chmod +x build_macos_installer.sh
#   ./build_macos_installer.sh                        # 完整打包（ad-hoc 签名）
#   ./build_macos_installer.sh --no-sign              # 跳过签名
#   ./build_macos_installer.sh --version 2.7.6        # 覆盖版本号
#   ./build_macos_installer.sh --skip-plugins         # 只打 Standalone + milkdrop
#   ./build_macos_installer.sh --skip-milkdrop        # 只打 Standalone + 完整版插件
#   ./build_macos_installer.sh --identity "Developer ID Application: ... (TEAMID)"
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"
BUILD_DIR="${PROJECT_ROOT}/cmake-build-release"
DIST_DIR="${PROJECT_ROOT}/dist"
WORK_DIR="${DIST_DIR}/.mac_installer_work"
STAGE_DIR="${WORK_DIR}/stage"
COMPONENTS_DIR="${WORK_DIR}/components"
SCRIPTS_DIR="${WORK_DIR}/scripts"
DMG_STAGE_DIR="${WORK_DIR}/dmg_stage"

PRODUCT_NAME="Y2Kmeter"
MILKDROP_NAME="Y2Kmeter_milkdrop"
PKG_ID_BASE="cn.iisaacbeats.y2kmeter"
STANDALONE_PKG_ID="${PKG_ID_BASE}.standalone"
PLUGINS_PKG_ID="${PKG_ID_BASE}.plugins"
MILKDROP_PKG_ID="${PKG_ID_BASE}.milkdrop"

# ---- 构建产物路径 -------------------------------------------------------------
STANDALONE_APP="${BUILD_DIR}/Y2Kmeter_artefacts/Release/Standalone/${PRODUCT_NAME}.app"
VST3_BUNDLE="${BUILD_DIR}/Y2Kmeter_artefacts/Release/VST3/${PRODUCT_NAME}.vst3"
AU_BUNDLE="${BUILD_DIR}/Y2Kmeter_artefacts/Release/AU/${PRODUCT_NAME}.component"
MILKDROP_VST3_BUNDLE="${BUILD_DIR}/Y2Kmeter_milkdrop_artefacts/Release/VST3/${MILKDROP_NAME}.vst3"
MILKDROP_AU_BUNDLE="${BUILD_DIR}/Y2Kmeter_milkdrop_artefacts/Release/AU/${MILKDROP_NAME}.component"

# ---- 暂存区副本（签名只作用于这些副本，构建目录里的原始产物保持不动）--------
STAGED_APP="${STAGE_DIR}/standalone/${PRODUCT_NAME}.app"
STAGED_VST3="${STAGE_DIR}/plugins/Library/Audio/Plug-Ins/VST3/${PRODUCT_NAME}.vst3"
STAGED_AU="${STAGE_DIR}/plugins/Library/Audio/Plug-Ins/Components/${PRODUCT_NAME}.component"
STAGED_MILKDROP_VST3="${STAGE_DIR}/milkdrop/Library/Audio/Plug-Ins/VST3/${MILKDROP_NAME}.vst3"
STAGED_MILKDROP_AU="${STAGE_DIR}/milkdrop/Library/Audio/Plug-Ins/Components/${MILKDROP_NAME}.component"

# 签名用的 entitlements：给 ad-hoc 签名补一个可读的 entitlements blob，
# 消除启动时 Security/CoreAudio 反复打印的 SecTaskLoadEntitlements 报错。
ENTITLEMENTS_FILE="${PROJECT_ROOT}/scripts/macos_entitlements.plist"

HAVE_VST3=0
HAVE_AU=0
HAVE_MILKDROP_VST3=0
HAVE_MILKDROP_AU=0

DO_SIGN=1
DO_PLUGINS=1
DO_MILKDROP=1
KEEP_WORK=0
SIGN_IDENTITY="-"     # 默认 ad-hoc 签名；用 --identity 指定 Developer ID
VERSION=""

log() { echo "[build_macos_installer] $*"; }

# ---- 架构探测：确认产物是 Intel / Apple Silicon 双架构 universal ------------
bundle_binary() {
  local bundle="$1"
  case "${bundle}" in
    *.app)       printf '%s' "${bundle}/Contents/MacOS/$(basename "${bundle}" .app)" ;;
    *.vst3)      printf '%s' "${bundle}/Contents/MacOS/$(basename "${bundle}" .vst3)" ;;
    *.component) printf '%s' "${bundle}/Contents/MacOS/$(basename "${bundle}" .component)" ;;
    *)           printf '%s' "" ;;
  esac
}

bundle_archs() {
  local bin
  bin="$(bundle_binary "$1")"
  if [[ -z "${bin}" || ! -f "${bin}" ]]; then
    printf '%s' "unknown"
    return 0
  fi
  /usr/bin/lipo -archs "${bin}" 2>/dev/null | tr '\n' ' ' | sed 's/[[:space:]]*$//'
}

usage() {
  cat <<'EOF'
Usage:
  ./build_macos_installer.sh [options]

Options:
  --version VER     Override version (default: parsed from CMakeLists.txt).
  --no-sign         Skip codesign before packaging.
  --identity NAME   Codesign identity (default: "-" ad-hoc).
                    e.g. --identity "Developer ID Application: Your Name (TEAMID)"
  --skip-plugins    Package the standalone app only (no Y2Kmeter VST3 / AU).
  --skip-milkdrop   Skip the Y2Kmeter_milkdrop plug-in component.
  --keep-work       Keep dist/.mac_installer_work for troubleshooting.
  -h, --help        Show this help.

Examples:
  # 本地自测：ad-hoc 签名，打完整包（Standalone + 完整版插件 + milkdrop 插件）
  ./build_macos_installer.sh

  # 正式发布：Developer ID 签名（之后仍需 notarize）
  ./build_macos_installer.sh --identity "Developer ID Application: iisaacbeats (XXXXXXXXXX)"
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-sign)       DO_SIGN=0; shift ;;
    --skip-plugins)  DO_PLUGINS=0; shift ;;
    --skip-milkdrop) DO_MILKDROP=0; shift ;;
    --keep-work)     KEEP_WORK=1; shift ;;
    --identity)
      SIGN_IDENTITY="${2:-}"
      if [[ -z "${SIGN_IDENTITY}" ]]; then
        echo "[ERROR] --identity requires a value" >&2
        exit 1
      fi
      shift 2
      ;;
    --version)
      VERSION="${2:-}"
      if [[ -z "${VERSION}" ]]; then
        echo "[ERROR] --version requires a value" >&2
        exit 1
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[ERROR] Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[ERROR] This script must run on macOS." >&2
  exit 1
fi

for cmd in ditto pkgbuild productbuild hdiutil codesign; do
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "[ERROR] Missing required command: ${cmd}" >&2
    exit 1
  fi
done

# ----------------------------------------------------------------------------
# Step 1/7  解析版本号
# ----------------------------------------------------------------------------
log "Step 1/7 Resolve version"

parse_version() {
  local file="${PROJECT_ROOT}/CMakeLists.txt"
  local v=""

  # 优先：project(Y2Kmeter VERSION x.y.z ...)
  v="$(awk '/^[[:space:]]*project[[:space:]]*\(/ {
              for (i = 1; i <= NF; i++)
                if ($i == "VERSION") { print $(i + 1); exit }
            }' "${file}" | tr -d '"')"

  # 兜底：juce_add_plugin( ... VERSION x.y.z ... )
  if [[ -z "${v}" ]]; then
    v="$(awk '/juce_add_plugin/,/^[[:space:]]*\)/ {
                for (i = 1; i <= NF; i++)
                  if ($i == "VERSION") { print $(i + 1); exit }
              }' "${file}" | tr -d '"')"
  fi

  printf '%s' "${v}"
}

if [[ -z "${VERSION}" ]]; then
  VERSION="$(parse_version)"
fi

if [[ -z "${VERSION}" ]]; then
  echo "[ERROR] Could not resolve VERSION from CMakeLists.txt. Use --version x.y.z" >&2
  exit 1
fi

PKG_NAME="${PRODUCT_NAME}-${VERSION}-macOS.pkg"
PKG_PATH="${DIST_DIR}/${PKG_NAME}"
DMG_NAME="${PRODUCT_NAME}-${VERSION}-macOS.dmg"
DMG_PATH="${DIST_DIR}/${DMG_NAME}"

log "  - Version: ${VERSION}"

# ----------------------------------------------------------------------------
# Step 2/7  校验构建产物
#   Standalone 是主组件，缺失直接失败；插件是可选组件，缺失只告警
# ----------------------------------------------------------------------------
log "Step 2/7 Validate build artifacts"

if [[ ! -d "${STANDALONE_APP}" ]]; then
  echo "[ERROR] Missing standalone app: ${STANDALONE_APP}" >&2
  echo "[HINT] Build first:" >&2
  echo "       cmake --build cmake-build-release --target Y2Kmeter_Standalone" >&2
  exit 1
fi
log "  - App : ${STANDALONE_APP} [$(bundle_archs "${STANDALONE_APP}")]"

if [[ "${DO_PLUGINS}" -eq 1 ]]; then
  if [[ -d "${VST3_BUNDLE}" ]]; then HAVE_VST3=1; log "  - VST3: ${VST3_BUNDLE} [$(bundle_archs "${VST3_BUNDLE}")]";
  else log "  - VST3: NOT FOUND (skipped)"; fi
  if [[ -d "${AU_BUNDLE}" ]]; then HAVE_AU=1; log "  - AU  : ${AU_BUNDLE} [$(bundle_archs "${AU_BUNDLE}")]";
  else log "  - AU  : NOT FOUND (skipped)"; fi
  if [[ "${HAVE_VST3}" -eq 0 && "${HAVE_AU}" -eq 0 ]]; then
    log "[WARN] No Y2Kmeter plug-in bundles found, packaging standalone only."
    log "[HINT] Build Release target Y2Kmeter_VST3 / Y2Kmeter_AU to include them."
    DO_PLUGINS=0
  fi
else
  log "  - Plug-ins: skipped (--skip-plugins)"
fi

if [[ "${DO_MILKDROP}" -eq 1 ]]; then
  if [[ -d "${MILKDROP_VST3_BUNDLE}" ]]; then HAVE_MILKDROP_VST3=1; log "  - Milkdrop VST3: ${MILKDROP_VST3_BUNDLE} [$(bundle_archs "${MILKDROP_VST3_BUNDLE}")]";
  else log "  - Milkdrop VST3: NOT FOUND (skipped)"; fi
  if [[ -d "${MILKDROP_AU_BUNDLE}" ]]; then HAVE_MILKDROP_AU=1; log "  - Milkdrop AU  : ${MILKDROP_AU_BUNDLE} [$(bundle_archs "${MILKDROP_AU_BUNDLE}")]";
  else log "  - Milkdrop AU  : NOT FOUND (skipped)"; fi
  if [[ "${HAVE_MILKDROP_VST3}" -eq 0 && "${HAVE_MILKDROP_AU}" -eq 0 ]]; then
    log "[WARN] No Y2Kmeter_milkdrop plug-in bundles found, packaging without milkdrop component."
    log "[HINT] Build Release target Y2Kmeter_milkdrop_VST3 / Y2Kmeter_milkdrop_AU to include them."
    DO_MILKDROP=0
  fi
else
  log "  - Milkdrop plug-in: skipped (--skip-milkdrop)"
fi

# ---- 双架构校验：任一产物缺少 arm64 或 x86_64 就告警 ------------------------
ARCH_TEXT=""
UNIVERSAL_OK=1
verify_universal() {
  local archs
  archs="$(bundle_archs "$1")"
  [[ -n "${ARCH_TEXT}" ]] || ARCH_TEXT="${archs}"
  if [[ "${archs}" != *arm64* || "${archs}" != *x86_64* ]]; then
    log "[WARN] Not a universal binary: $1 -> ${archs}"
    log "[HINT] Configure with -DCMAKE_OSX_ARCHITECTURES=\"x86_64;arm64\" and rebuild."
    UNIVERSAL_OK=0
  fi
}
verify_universal "${STANDALONE_APP}"
if [[ "${HAVE_VST3}" -eq 1 ]]; then verify_universal "${VST3_BUNDLE}"; fi
if [[ "${HAVE_AU}"   -eq 1 ]]; then verify_universal "${AU_BUNDLE}";   fi
if [[ "${HAVE_MILKDROP_VST3}" -eq 1 ]]; then verify_universal "${MILKDROP_VST3_BUNDLE}"; fi
if [[ "${HAVE_MILKDROP_AU}"   -eq 1 ]]; then verify_universal "${MILKDROP_AU_BUNDLE}";   fi
if [[ "${UNIVERSAL_OK}" -eq 1 ]]; then
  log "  - Universal binary OK (arm64 + x86_64)"
fi

# ----------------------------------------------------------------------------
# Step 3/7  复制产物到暂存区
#   签名一律只作用于这里的副本，绝不改动 cmake 构建目录里的原始产物。
# ----------------------------------------------------------------------------
log "Step 3/7 Stage bundles"

rm -rf "${WORK_DIR}"
mkdir -p "${DIST_DIR}" "${COMPONENTS_DIR}" "${SCRIPTS_DIR}"

mkdir -p "${STAGE_DIR}/standalone"
/usr/bin/ditto "${STANDALONE_APP}" "${STAGED_APP}"
log "  - ${PRODUCT_NAME}.app"

if [[ "${DO_PLUGINS}" -eq 1 ]]; then
  mkdir -p "${STAGE_DIR}/plugins/Library/Audio/Plug-Ins/VST3"
  mkdir -p "${STAGE_DIR}/plugins/Library/Audio/Plug-Ins/Components"
  if [[ "${HAVE_VST3}" -eq 1 ]]; then
    /usr/bin/ditto "${VST3_BUNDLE}" "${STAGED_VST3}"
    log "  - ${PRODUCT_NAME}.vst3"
  fi
  if [[ "${HAVE_AU}" -eq 1 ]]; then
    /usr/bin/ditto "${AU_BUNDLE}" "${STAGED_AU}"
    log "  - ${PRODUCT_NAME}.component"
  fi
fi

if [[ "${DO_MILKDROP}" -eq 1 ]]; then
  mkdir -p "${STAGE_DIR}/milkdrop/Library/Audio/Plug-Ins/VST3"
  mkdir -p "${STAGE_DIR}/milkdrop/Library/Audio/Plug-Ins/Components"
  if [[ "${HAVE_MILKDROP_VST3}" -eq 1 ]]; then
    /usr/bin/ditto "${MILKDROP_VST3_BUNDLE}" "${STAGED_MILKDROP_VST3}"
    log "  - ${MILKDROP_NAME}.vst3"
  fi
  if [[ "${HAVE_MILKDROP_AU}" -eq 1 ]]; then
    /usr/bin/ditto "${MILKDROP_AU_BUNDLE}" "${STAGED_MILKDROP_AU}"
    log "  - ${MILKDROP_NAME}.component"
  fi
fi

# ----------------------------------------------------------------------------
# Step 4/7  签名（只签暂存区副本）
#
#   重要：**不要**加 `--options runtime`（hardened runtime）。
#   hardened runtime 会强制校验：主进程与它 dlopen 的所有 dylib 必须拥有一致的
#   Team ID。而 ad-hoc 签名的 libprojectM-4.dylib 会得到一个基于内容哈希的
#   "隐式 Team ID"，与主 binary 不同，dyld 会拒绝加载，导致 Milkdrop 模块
#   dlopen 失败、界面纯黑。只有走 Apple Developer ID + notarization 的正式
#   签名才应配合 `--options runtime` 使用。
# ----------------------------------------------------------------------------
if [[ "${DO_SIGN}" -eq 1 ]]; then
  log "Step 4/7 Codesign staged bundles (identity: ${SIGN_IDENTITY})"

  sign_bundle() {
    local bundle="$1"
    local ts_args=()
    if [[ "${SIGN_IDENTITY}" == "-" ]]; then
      ts_args=(--timestamp=none)
    else
      ts_args=(--timestamp)
    fi

    codesign --force --deep --sign "${SIGN_IDENTITY}" \
             "${ts_args[@]}" \
             --entitlements "${ENTITLEMENTS_FILE}" \
             "${bundle}"
    codesign --verify --deep --strict --verbose=2 "${bundle}" >/dev/null || true
  }

  sign_bundle "${STAGED_APP}"
  if [[ "${HAVE_VST3}" -eq 1 ]]; then sign_bundle "${STAGED_VST3}"; fi
  if [[ "${HAVE_AU}"   -eq 1 ]]; then sign_bundle "${STAGED_AU}";   fi
  if [[ "${HAVE_MILKDROP_VST3}" -eq 1 ]]; then sign_bundle "${STAGED_MILKDROP_VST3}"; fi
  if [[ "${HAVE_MILKDROP_AU}"   -eq 1 ]]; then sign_bundle "${STAGED_MILKDROP_AU}";   fi
else
  log "Step 4/7 Skip signing (--no-sign)"
fi

# ----------------------------------------------------------------------------
# Step 5/7  打 component 包
# ----------------------------------------------------------------------------
log "Step 5/7 Build component packages"

# ---- 5a. Standalone 组件的 preinstall / postinstall ----
mkdir -p "${SCRIPTS_DIR}/standalone"
cat > "${SCRIPTS_DIR}/standalone/preinstall" <<'EOF'
#!/bin/sh
# 安装前关闭正在运行的旧版本，避免旧文件被占用（对应 Windows 侧的 CloseApplications=force）
/usr/bin/osascript -e 'tell application "Y2Kmeter" to quit' >/dev/null 2>&1 || true
/usr/bin/pkill -x "Y2Kmeter" >/dev/null 2>&1 || true

# 向下兼容：清理旧版 DMG「拖拽安装」可能残留在用户目录里的插件。
#   旧版 README 引导用户把插件拖到 /Library/Audio/Plug-Ins/，权限不足时可改放
#   ~/Library/Audio/Plug-Ins/。新版 pkg 固定装到 /Library（系统目录），若不清掉
#   用户目录里的旧版（同 bundle id），宿主会同时扫到两份并冲突（VST3 只加载一份、
#   AU 可能重复注册）。系统目录里的旧版由 pkg 安装时按 bundle 覆盖，无需在此删除。
#   注意：preinstall 以 root 运行，`~` 会展开成 root 的 home（/var/root），因此这里
#   显式遍历 /Users/* 来定位各用户的 home，清理用户级插件副本。
for user_home in /Users/*; do
  [ -d "${user_home}/Library/Audio/Plug-Ins" ] || continue
  /bin/rm -rf \
    "${user_home}/Library/Audio/Plug-Ins/VST3/Y2Kmeter.vst3" \
    "${user_home}/Library/Audio/Plug-Ins/VST3/Y2Kmeter_milkdrop.vst3" \
    "${user_home}/Library/Audio/Plug-Ins/Components/Y2Kmeter.component" \
    "${user_home}/Library/Audio/Plug-Ins/Components/Y2Kmeter_milkdrop.component" \
    2>/dev/null || true
done

exit 0
EOF
cat > "${SCRIPTS_DIR}/standalone/postinstall" <<'EOF'
#!/bin/sh
APP_PATH="/Applications/Y2Kmeter.app"
# 清掉隔离属性：ad-hoc 签名的 app 若带 com.apple.quarantine，首次启动会被 Gatekeeper 拦截
/usr/bin/xattr -dr com.apple.quarantine "$APP_PATH" >/dev/null 2>&1 || true
/bin/chmod -R a+rX "$APP_PATH" >/dev/null 2>&1 || true
exit 0
EOF
/bin/chmod +x "${SCRIPTS_DIR}/standalone/preinstall" "${SCRIPTS_DIR}/standalone/postinstall"

pkgbuild \
  --root "${STAGE_DIR}/standalone" \
  --identifier "${STANDALONE_PKG_ID}" \
  --version "${VERSION}" \
  --install-location "/Applications" \
  --scripts "${SCRIPTS_DIR}/standalone" \
  "${COMPONENTS_DIR}/standalone.pkg"
log "  - standalone.pkg -> /Applications/${PRODUCT_NAME}.app"

# ---- 5b. 完整版插件组件（可选） ----
if [[ "${DO_PLUGINS}" -eq 1 ]]; then
  mkdir -p "${SCRIPTS_DIR}/plugins"
  {
    echo '#!/bin/sh'
    echo '# 清掉插件 bundle 的隔离属性，避免 Logic / 宿主把插件判定为不可用'
    if [[ "${HAVE_VST3}" -eq 1 ]]; then
      echo '/usr/bin/xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Y2Kmeter.vst3" >/dev/null 2>&1 || true'
    fi
    if [[ "${HAVE_AU}" -eq 1 ]]; then
      echo '/usr/bin/xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/Components/Y2Kmeter.component" >/dev/null 2>&1 || true'
    fi
    echo 'exit 0'
  } > "${SCRIPTS_DIR}/plugins/postinstall"
  /bin/chmod +x "${SCRIPTS_DIR}/plugins/postinstall"

  pkgbuild \
    --root "${STAGE_DIR}/plugins" \
    --identifier "${PLUGINS_PKG_ID}" \
    --version "${VERSION}" \
    --install-location "/" \
    --scripts "${SCRIPTS_DIR}/plugins" \
    "${COMPONENTS_DIR}/plugins.pkg"
  log "  - plugins.pkg"
fi

# ---- 5c. milkdrop 插件组件（可选） ----
if [[ "${DO_MILKDROP}" -eq 1 ]]; then
  mkdir -p "${SCRIPTS_DIR}/milkdrop"
  {
    echo '#!/bin/sh'
    echo '# 清掉 milkdrop 插件 bundle 的隔离属性'
    if [[ "${HAVE_MILKDROP_VST3}" -eq 1 ]]; then
      echo '/usr/bin/xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Y2Kmeter_milkdrop.vst3" >/dev/null 2>&1 || true'
    fi
    if [[ "${HAVE_MILKDROP_AU}" -eq 1 ]]; then
      echo '/usr/bin/xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/Components/Y2Kmeter_milkdrop.component" >/dev/null 2>&1 || true'
    fi
    echo 'exit 0'
  } > "${SCRIPTS_DIR}/milkdrop/postinstall"
  /bin/chmod +x "${SCRIPTS_DIR}/milkdrop/postinstall"

  pkgbuild \
    --root "${STAGE_DIR}/milkdrop" \
    --identifier "${MILKDROP_PKG_ID}" \
    --version "${VERSION}" \
    --install-location "/" \
    --scripts "${SCRIPTS_DIR}/milkdrop" \
    "${COMPONENTS_DIR}/milkdrop.pkg"
  log "  - milkdrop.pkg"
fi

# ----------------------------------------------------------------------------
# Step 6/7  合成带组件选择页的 product 包
#   standalone : selected="true" enabled="false"  —— 必装，勾选框置灰
#   plugins    : selected="true" enabled="true"   —— 默认勾选，可取消
#   milkdrop   : selected="true" enabled="true"   —— 默认勾选，可取消
# ----------------------------------------------------------------------------
log "Step 6/7 Build product package"

# 有任意可选组件时显示"自定义安装"；只有 standalone 时隐藏
if [[ "${DO_PLUGINS}" -eq 1 || "${DO_MILKDROP}" -eq 1 ]]; then
  CUSTOMIZE_ATTR="always"
else
  CUSTOMIZE_ATTR="never"
fi

{
  echo '<?xml version="1.0" encoding="utf-8"?>'
  echo '<installer-gui-script minSpecVersion="1">'
  echo "    <title>${PRODUCT_NAME} ${VERSION}</title>"
  echo "    <options customize=\"${CUSTOMIZE_ATTR}\"/>"
  echo '    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>'
  echo '    <choices-outline>'
  echo "        <line choice=\"${STANDALONE_PKG_ID}\"/>"
  if [[ "${DO_PLUGINS}" -eq 1 ]]; then
    echo "        <line choice=\"${PLUGINS_PKG_ID}\"/>"
  fi
  if [[ "${DO_MILKDROP}" -eq 1 ]]; then
    echo "        <line choice=\"${MILKDROP_PKG_ID}\"/>"
  fi
  echo '    </choices-outline>'

  echo "    <choice id=\"${STANDALONE_PKG_ID}\""
  echo "            title=\"${PRODUCT_NAME} (Standalone Application)\""
  echo "            description=\"The standalone desktop application.\""
  echo '            enabled="false" selected="true">'
  echo "        <pkg-ref id=\"${STANDALONE_PKG_ID}\"/>"
  echo '    </choice>'

  if [[ "${DO_PLUGINS}" -eq 1 ]]; then
    echo "    <choice id=\"${PLUGINS_PKG_ID}\""
    echo "            title=\"${PRODUCT_NAME} Plug-ins (VST3 + AU)\""
    echo "            description=\"Audio plug-ins for your DAW.\""
    echo '            enabled="true" selected="true">'
    echo "        <pkg-ref id=\"${PLUGINS_PKG_ID}\"/>"
    echo '    </choice>'
  fi

  if [[ "${DO_MILKDROP}" -eq 1 ]]; then
    echo "    <choice id=\"${MILKDROP_PKG_ID}\""
    echo "            title=\"${PRODUCT_NAME} Milkdrop Plug-in (VST3 + AU)\""
    echo "            description=\"A Milkdrop-only visualizer plug-in — a single fullscreen Milkdrop module.\""
    echo '            enabled="true" selected="true">'
    echo "        <pkg-ref id=\"${MILKDROP_PKG_ID}\"/>"
    echo '    </choice>'
  fi

  echo "    <pkg-ref id=\"${STANDALONE_PKG_ID}\" version=\"${VERSION}\" onConclusion=\"none\">standalone.pkg</pkg-ref>"
  if [[ "${DO_PLUGINS}" -eq 1 ]]; then
    echo "    <pkg-ref id=\"${PLUGINS_PKG_ID}\" version=\"${VERSION}\" onConclusion=\"none\">plugins.pkg</pkg-ref>"
  fi
  if [[ "${DO_MILKDROP}" -eq 1 ]]; then
    echo "    <pkg-ref id=\"${MILKDROP_PKG_ID}\" version=\"${VERSION}\" onConclusion=\"none\">milkdrop.pkg</pkg-ref>"
  fi
  echo '</installer-gui-script>'
} > "${WORK_DIR}/Distribution.xml"

rm -f "${PKG_PATH}"
productbuild \
  --distribution "${WORK_DIR}/Distribution.xml" \
  --package-path "${COMPONENTS_DIR}" \
  "${PKG_PATH}"

# ----------------------------------------------------------------------------
# Step 7/7  打 dmg
# ----------------------------------------------------------------------------
log "Step 7/7 Build .dmg"

rm -rf "${DMG_STAGE_DIR}"
mkdir -p "${DMG_STAGE_DIR}"
/usr/bin/ditto "${PKG_PATH}" "${DMG_STAGE_DIR}/${PKG_NAME}"

{
  echo "${PRODUCT_NAME} ${VERSION} macOS installer"
  echo "========================================"
  echo
  echo "Architectures: ${ARCH_TEXT:-unknown}"
  echo
  echo "Open \"${PKG_NAME}\" to install."
  echo
  echo "Components:"
  echo
  echo "  1) ${PRODUCT_NAME} (Standalone Application)       [required]"
  echo "       -> /Applications/${PRODUCT_NAME}.app"
  if [[ "${DO_PLUGINS}" -eq 1 ]]; then
    echo
    echo "  2) ${PRODUCT_NAME} Plug-ins (VST3 + AU)           [optional]"
    if [[ "${HAVE_VST3}" -eq 1 ]]; then
      echo "       -> /Library/Audio/Plug-Ins/VST3/${PRODUCT_NAME}.vst3"
    fi
    if [[ "${HAVE_AU}" -eq 1 ]]; then
      echo "       -> /Library/Audio/Plug-Ins/Components/${PRODUCT_NAME}.component"
    fi
  fi
  if [[ "${DO_MILKDROP}" -eq 1 ]]; then
    echo
    echo "  3) ${PRODUCT_NAME} Milkdrop Plug-in (VST3 + AU)   [optional]"
    if [[ "${HAVE_MILKDROP_VST3}" -eq 1 ]]; then
      echo "       -> /Library/Audio/Plug-Ins/VST3/${MILKDROP_NAME}.vst3"
    fi
    if [[ "${HAVE_MILKDROP_AU}" -eq 1 ]]; then
      echo "       -> /Library/Audio/Plug-Ins/Components/${MILKDROP_NAME}.component"
    fi
  fi
  echo
  echo "Click \"Customize\" on the Installation Type step to select plug-ins."
  echo
  echo "After installation, reopen your DAW and rescan plug-ins if needed."
  echo
  echo "Note: this build uses an ad-hoc signature (not notarized)."
  echo "If Gatekeeper blocks the app on first launch, right-click it in Finder"
  echo "and choose Open."
} > "${DMG_STAGE_DIR}/README.txt"

rm -f "${DMG_PATH}"
hdiutil create \
  -volname "${PRODUCT_NAME} ${VERSION}" \
  -srcfolder "${DMG_STAGE_DIR}" \
  -ov \
  -format UDZO \
  -imagekey zlib-level=9 \
  "${DMG_PATH}" >/dev/null

# 如果启用了签名，顺手给 DMG 也加个 ad-hoc 签名（pkg 本身不单独签名，
# ad-hoc 分发下无需 productsign，保持与 SpectrumTag 一致）。
if [[ "${DO_SIGN}" -eq 1 ]]; then
  codesign --force --sign - "${DMG_PATH}" || true
fi

if [[ "${KEEP_WORK}" -eq 1 ]]; then
  log "Keep work dir: ${WORK_DIR}"
else
  log "Cleanup"
  rm -rf "${WORK_DIR}"
fi

log "Done"
log "  - PKG: ${PKG_PATH}"
log "  - DMG: ${DMG_PATH}"
