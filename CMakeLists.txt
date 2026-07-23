cmake_minimum_required(VERSION 3.22)

# ==========================================================
# MSVC 静态 CRT 策略（必须在 project() 之前启用 CMP0091）
#   · 目的：让最终 exe 不再依赖 VCRUNTIME140.dll / MSVCP140.dll / UCRT 等
#     "Visual C++ 可再发行包"（VC_redist.x64.exe）里的系统 DLL。
#   · 效果：把 exe 发给任何一台干净的 Win10/Win11 x64 电脑都能直接双击运行，
#     不会再出现"启动画面转一下就消失"（缺少 VCRUNTIME*.dll 的静默失败）。
#   · 代价：exe 体积大约 +2~3 MB；CRT 不再随 Windows Update 打补丁，
#     对于这种桌面工具类应用完全可以接受。
#   · 作用域：必须放在任何 project() / FetchContent / add_subdirectory 之前，
#     这样 JUCE / grid_plugin / BinaryData 等所有子 target 都会被统一策略链接。
# ==========================================================
if (POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
# Release / RelWithDebInfo / MinSizeRel → 静态链接 CRT（/MT），
# 便于把最终 exe 发给任何一台干净的 Win10/Win11 x64 电脑而不依赖 VCRUNTIME*.dll。
# Debug → 动态链接 CRT（/MDd），本地调试专用，不需要静态发布；
#   踩坑记录一：VS 2026 Preview（MSVC 14.51.x）下 libcpmtd.lib（Debug 静态 C++ 标准库）
#     会引用 debug UCRT 的 _free_dbg / _malloc_dbg / _CrtDbgReport / _CrtSetDbgFlag /
#     _CrtDumpMemoryLeaks 等符号，但这些符号所在的 libucrtd.lib / libvcruntimed.lib
#     在部分 SDK 组合下并未安装或版本对不齐，触发 LNK2001/LNK2019 大批链接失败。
#     动态链接（ucrtbased.dll + vcruntime140d.dll + msvcp140d.dll）后本地机器的
#     Windows SDK 会一并提供这些 DLL，Debug 构建就不再报错。
#   踩坑记录二：不能使用 generator expression（例如 "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>"），
#     因为 JUCE 的 juceaide 是通过 execute_process(cmake -B ...) 单独启动的子 configure，
#     它不会去展开顶层项目的 $<CONFIG:...> 生成器表达式，结果 juceaide 依然按默认走 /MTd
#     依旧触发 libcpmtd.lib 的链接错误。这里必须根据 CMAKE_BUILD_TYPE 直接给出裸字符串。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL" CACHE STRING "" FORCE)
else()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
endif()

project(Y2Kmeter VERSION 2.2.3 LANGUAGES C CXX)

# ==========================================================
# macOS 平台：启用 Objective-C++ 语言
#   · MacDesktopAudioCapture.mm 走 ScreenCaptureKit 做桌面音频采集，
#     必须经由 Objective-C++ 编译才能解析 @interface / NSObject 等语法。
#   · 仅在 APPLE 分支启用，不影响 Windows 与 Linux 构建。
# ==========================================================
if (APPLE)
  enable_language(OBJCXX)
endif()

# 让 Ninja/Makefile 生成 compile_commands.json，供 clangd 精确读取
# include path、宏定义和 JUCE 生成头路径。Visual Studio 生成器会忽略该文件生成，
# 所以仓库里额外提供了 clangd-msvc preset 使用 Ninja。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 当外部环境设置了 CMAKE_GENERATOR_PLATFORM 但未设置 CMAKE_GENERATOR 时，
# CMake 在 VS CustomBuild 阶段会反复打印警告。
if (DEFINED ENV{CMAKE_GENERATOR_PLATFORM} AND NOT DEFINED ENV{CMAKE_GENERATOR})
  message(WARNING
    "检测到环境变量 CMAKE_GENERATOR_PLATFORM=$ENV{CMAKE_GENERATOR_PLATFORM}，"
    "但未设置 CMAKE_GENERATOR。"
    "建议移除该环境变量，或在 configure 阶段显式指定 -G 与 -A，"
    "以避免构建阶段重复警告。"
  )
endif()

set(CMAKE_CXX_STANDARD 17)  
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==========================================================
# Release 性能优化开关
#   · LTO/IPO：默认启用，只影响 Release / RelWithDebInfo / MinSizeRel。
#   · PGO：两阶段流程，默认关闭；通过 preset 切换 GENERATE / USE。
#     1) Y2K_PGO_MODE=GENERATE 构建并运行典型场景，生成 profile 数据。
#     2) Y2K_PGO_MODE=USE      使用同一份 profile 数据重新构建发布版。
# ==========================================================
option(Y2K_ENABLE_LTO "Enable link-time optimization for release builds" ON)

set(Y2K_PGO_MODE "OFF" CACHE STRING "PGO mode: OFF, GENERATE, USE")
set_property(CACHE Y2K_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
string(TOUPPER "${Y2K_PGO_MODE}" Y2K_PGO_MODE)
set(Y2K_PGO_MODE "${Y2K_PGO_MODE}" CACHE STRING "PGO mode: OFF, GENERATE, USE" FORCE)

set(Y2K_PGO_DATA_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH
    "Directory used to write/read profile-guided optimization data")

set(Y2K_VALID_PGO_MODES OFF GENERATE USE)
if (NOT Y2K_PGO_MODE IN_LIST Y2K_VALID_PGO_MODES)
  message(FATAL_ERROR "Y2K_PGO_MODE must be OFF, GENERATE, or USE; got '${Y2K_PGO_MODE}'")
endif()

if (DEFINED FETCHCONTENT_SOURCE_DIR_JUCE
    AND NOT FETCHCONTENT_SOURCE_DIR_JUCE STREQUAL ""
    AND NOT EXISTS "${FETCHCONTENT_SOURCE_DIR_JUCE}/CMakeLists.txt")
  message(WARNING
    "FETCHCONTENT_SOURCE_DIR_JUCE='${FETCHCONTENT_SOURCE_DIR_JUCE}' does not look like a JUCE checkout "
    "(missing CMakeLists.txt). Ignoring this override so FetchContent can populate JUCE.")
  unset(FETCHCONTENT_SOURCE_DIR_JUCE CACHE)
  unset(FETCHCONTENT_SOURCE_DIR_JUCE)
endif()

include(FetchContent)

# clangd 的专用 Ninja 配置需要 compile_commands.json，但 JUCE 默认会在配置期
# 递归再开一个子 CMake/Ninja 去单独构建 juceaide。Windows 上如果 PATH 里同时有
# MinGW，这个子配置很容易重新选回 gcc/g++，导致 JUCE 直接以 "MinGW is not
# supported" 失败。这里改为在当前工具链里直接构建 helper tool，避免递归子配置
# 的编译器漂移。
#
# 注意：此 workaround 只适用于 Windows。
#   · 默认分支（OFF）：JUCE 在 *配置阶段* 通过 execute_process 递归子 CMake 把
#     juceaide 立即编出来并以 IMPORTED target 形式暴露，juce_add_plugin 在配置
#     阶段调用 juceaide 生成图标 / plist 时文件已存在。
#   · 强开 ON：juceaide 变成主构建图里的普通 target，只在 build 阶段才会被编译；
#     但 juce_add_plugin 仍然在 *配置阶段* 就去执行 juceaide，于是会抛出
#     "juceaide was imported, but it doesn't exist!"。
# 因此在 macOS / Linux 上必须保持默认 OFF，只有 Windows 需要这个强制覆盖。
if (WIN32)
  set(JUCE_BUILD_HELPER_TOOLS ON CACHE BOOL
      "Build JUCE helper tools in the current configure pass" FORCE)
endif()

FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.12
)
FetchContent_MakeAvailable(juce)

if (TARGET juceaide AND NOT TARGET juce::juceaide)
  add_executable(juce::juceaide ALIAS juceaide)
endif()

# ==========================================================
# 覆盖 juceaide 的 MSVC_RUNTIME_LIBRARY
#   · JUCE 官方在 extras/Build/juceaide/CMakeLists.txt 里对 juceaide
#     硬编码：set_target_properties(juceaide PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
#     即"Release 静态 CRT (/MT)"。
#   · 而 CMake 在 Debug 配置下会自动给编译加 -D_DEBUG，STL 头因此选择 debug 版本，
#     用到 debug UCRT 符号（_free_dbg / _malloc_dbg / _CrtDbgReport /
#     _CrtDumpMemoryLeaks / _CrtSetDbgFlag）；但 /MT 链的是 release 静态 UCRT
#     libucrt.lib，符号缺失 → 大批 LNK2001/LNK2019。
#   · 修复：把 juceaide 的属性显式覆盖为与本项目一致的运行库
#       Debug   → MultiThreadedDebugDLL (/MDd)
#       非 Debug → MultiThreaded (/MT)
#     必须用 target property（优先级 > CMAKE_MSVC_RUNTIME_LIBRARY 全局默认）。
if (WIN32 AND TARGET juceaide)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(juceaide PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
  else()
    set_target_properties(juceaide PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endif()
endif()

# add_subdirectory(grid_plugin)

# ==========================================================
# macOS 图标流水线（仅 APPLE 分支执行，Windows 无影响）
#   · Windows Standalone 使用 assets/icon.ico（app_icon.rc 嵌入 EXE）
#   · macOS 同样以 assets/icon.ico 为源：
#       1) 先用 sips 把 .ico 解码成中间 PNG
#       2) 交给 scripts/macos_iconize.m 渲染成 1024×1024 圆角 squircle 模板
#       3) sips 缩多尺寸 → iconutil 打包成 Icon.icns
#   · 产物仅供 JUCE ICON_BIG/ICON_SMALL 与 Dock/Finder 图标使用。
# ==========================================================
set(Y2KM_ICON_PLUGIN_ARGS)
set(Y2KM_PLUGIN_FORMATS VST3 Standalone)
if (APPLE)
  # 仅 macOS 构建 AU 插件；Windows/Linux 维持原有格式不变
  list(APPEND Y2KM_PLUGIN_FORMATS AU)

  set(Y2KM_MACOS_RES_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources")
  file(MAKE_DIRECTORY "${Y2KM_MACOS_RES_DIR}")

  set(Y2KM_ICON_ICO_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
  set(Y2KM_ICON_PNG_DST  "${Y2KM_MACOS_RES_DIR}/icon.png")
  if (NOT EXISTS "${Y2KM_ICON_ICO_SRC}")
    message(FATAL_ERROR "缺少图标资源：${Y2KM_ICON_ICO_SRC}")
  endif()

  # ----------------------------------------------------------------
  # Step 0: 把 icon.ico 解码成 PNG（中间产物，给后续 iconize 用）
  # ----------------------------------------------------------------
  set(Y2KM_ICON_DECODED_PNG "${Y2KM_MACOS_RES_DIR}/icon_decoded.png")
  execute_process(
    COMMAND /usr/bin/sips -s format png "${Y2KM_ICON_ICO_SRC}" --out "${Y2KM_ICON_DECODED_PNG}"
    RESULT_VARIABLE Y2KM_ICON_DECODE_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if (NOT Y2KM_ICON_DECODE_RESULT EQUAL 0)
    message(FATAL_ERROR "sips 无法把 ${Y2KM_ICON_ICO_SRC} 解码成 PNG")
  endif()

  # ----------------------------------------------------------------
  # Step 1: 编译 scripts/macos_iconize.m（Objective-C + CoreGraphics）
  # ----------------------------------------------------------------
  set(Y2KM_ICONIZE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/scripts/macos_iconize.m")
  set(Y2KM_ICONIZE_BIN "${Y2KM_MACOS_RES_DIR}/macos_iconize")

  if (NOT EXISTS "${Y2KM_ICONIZE_SRC}")
    message(FATAL_ERROR "缺少工具源码：${Y2KM_ICONIZE_SRC}")
  endif()

  execute_process(
    COMMAND /usr/bin/clang
            -framework AppKit
            -framework CoreGraphics
            -framework ImageIO
            -framework CoreFoundation
            -framework Foundation
            -fobjc-arc
            -O2
            "${Y2KM_ICONIZE_SRC}"
            -o "${Y2KM_ICONIZE_BIN}"
    RESULT_VARIABLE Y2KM_ICONIZE_BUILD_RESULT
    OUTPUT_VARIABLE Y2KM_ICONIZE_BUILD_STDOUT
    ERROR_VARIABLE  Y2KM_ICONIZE_BUILD_STDERR
  )
  if (NOT Y2KM_ICONIZE_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
      "编译 scripts/macos_iconize.m 失败：\n${Y2KM_ICONIZE_BUILD_STDERR}")
  endif()

  # ----------------------------------------------------------------
  # Step 2: 把解码后的 PNG 渲染成 1024×1024 圆角 squircle 模板
  # ----------------------------------------------------------------
  set(Y2KM_ICON_TEMPLATE_PNG "${Y2KM_MACOS_RES_DIR}/icon_macos_template.png")
  execute_process(
    COMMAND "${Y2KM_ICONIZE_BIN}"
            "${Y2KM_ICON_DECODED_PNG}"
            "${Y2KM_ICON_TEMPLATE_PNG}"
    RESULT_VARIABLE Y2KM_ICONIZE_RUN_RESULT
    OUTPUT_VARIABLE Y2KM_ICONIZE_RUN_STDOUT
    ERROR_VARIABLE  Y2KM_ICONIZE_RUN_STDERR
  )
  if (NOT Y2KM_ICONIZE_RUN_RESULT EQUAL 0)
    message(FATAL_ERROR
      "运行 macos_iconize 失败：\n${Y2KM_ICONIZE_RUN_STDERR}")
  endif()

  execute_process(
    COMMAND /usr/bin/sips -s format png "${Y2KM_ICON_TEMPLATE_PNG}" --out "${Y2KM_ICON_PNG_DST}"
    RESULT_VARIABLE Y2KM_ICON_CONVERT_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if (NOT Y2KM_ICON_CONVERT_RESULT EQUAL 0)
    message(FATAL_ERROR "sips 无法从模板图生成 icon.png")
  endif()

  # ----------------------------------------------------------------
  # Step 3: 从圆角模板图生成标准 macOS 多分辨率 Icon.icns
  # ----------------------------------------------------------------
  set(Y2KM_ICONSET_DIR "${Y2KM_MACOS_RES_DIR}/Icon.iconset")
  set(Y2KM_ICON_ICNS_DST "${Y2KM_MACOS_RES_DIR}/Icon.icns")
  file(REMOVE_RECURSE "${Y2KM_ICONSET_DIR}")
  file(MAKE_DIRECTORY "${Y2KM_ICONSET_DIR}")

  foreach(_sz 16 32 128 256 512)
    math(EXPR _sz2 "${_sz} * 2")

    execute_process(
      COMMAND /usr/bin/sips -z ${_sz} ${_sz} "${Y2KM_ICON_TEMPLATE_PNG}" --out "${Y2KM_ICONSET_DIR}/icon_${_sz}x${_sz}.png"
      RESULT_VARIABLE Y2KM_ICONSET_RESIZE_RESULT
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if (NOT Y2KM_ICONSET_RESIZE_RESULT EQUAL 0)
      message(FATAL_ERROR "生成 icon_${_sz}x${_sz}.png 失败")
    endif()

    execute_process(
      COMMAND /usr/bin/sips -z ${_sz2} ${_sz2} "${Y2KM_ICON_TEMPLATE_PNG}" --out "${Y2KM_ICONSET_DIR}/icon_${_sz}x${_sz}@2x.png"
      RESULT_VARIABLE Y2KM_ICONSET_RESIZE2_RESULT
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if (NOT Y2KM_ICONSET_RESIZE2_RESULT EQUAL 0)
      message(FATAL_ERROR "生成 icon_${_sz}x${_sz}@2x.png 失败")
    endif()
  endforeach()

  execute_process(
    COMMAND /usr/bin/iconutil -c icns "${Y2KM_ICONSET_DIR}" -o "${Y2KM_ICON_ICNS_DST}"
    RESULT_VARIABLE Y2KM_ICONUTIL_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if (NOT Y2KM_ICONUTIL_RESULT EQUAL 0)
    message(FATAL_ERROR "生成 ${Y2KM_ICON_ICNS_DST} 失败（iconutil）")
  endif()

  list(APPEND Y2KM_ICON_PLUGIN_ARGS
    ICON_BIG "${Y2KM_ICON_PNG_DST}"
    ICON_SMALL "${Y2KM_ICON_PNG_DST}"
  )
endif()

juce_add_plugin(
Y2Kmeter
        PRODUCT_NAME "Y2Kmeter"
        # 显式声明插件显示名，避免部分 DAW 使用 target name 作为显示名导致不一致
        PLUGIN_NAME "Y2Kmeter"
        # 反写域名作为 bundle ID / VST3 Plug Identifier，保证在 DAW 里稳定识别
        # （升级版本或改 COMPANY_NAME 都不会换 ID，用户工程里的插件实例不会丢）
        BUNDLE_ID "cn.iisaacbeats.Y2Kmeter"
VERSION 2.2.3
        COMPANY_NAME "iisaacbeats.cn"
        PLUGIN_MANUFACTURER_CODE Y2KM
        PLUGIN_CODE Y2Km
        FORMATS ${Y2KM_PLUGIN_FORMATS}
        # macOS 下 Y2KM_ICON_PLUGIN_ARGS 展开为 ICON_BIG/ICON_SMALL，其它平台为空，不影响 Windows。
        ${Y2KM_ICON_PLUGIN_ARGS}
        # 本插件是纯分析表（EQ 分析仪 / LUFS / 示波器 / 频谱 / 相位 / 动态等），
        # 对应 VST3 SDK 的 Analyzer 类别，让 DAW 的 FX 分类浏览器正确归类。
        VST3_CATEGORIES "Analyzer" "Fx"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        # 注：Milkdrop 模块自 2.0.4 起改用 libprojectM（native OpenGL），
        # 因此不再声明 NEEDS_WEBVIEW2；相关的 juce_gui_extra + WebBrowser 依赖
        # 已经从下方 target_compile_definitions 中移除。projectM DLL 通过
        # LoadLibrary 动态加载，见 source/ui/modules/ProjectMApi.cpp。
        VST3_AUTO_MANIFEST FALSE
        COPY_PLUGIN_AFTER_BUILD TRUE
)

juce_generate_juce_header(Y2Kmeter)

# 字体资源打包：
#   - SilkscreenRegular.ttf ← Silkscreen 像素英文字体
set(Y2KM_RES_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources")
file(MAKE_DIRECTORY "${Y2KM_RES_DIR}")

set(Y2KM_FONT_EN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/ttf/Silkscreen-Regular.ttf")
set(Y2KM_LOGO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png")

if (NOT EXISTS "${Y2KM_FONT_EN_SRC}")
  message(FATAL_ERROR "缺少字体资源：${Y2KM_FONT_EN_SRC}")
endif()
if (NOT EXISTS "${Y2KM_LOGO_SRC}")
  message(FATAL_ERROR "缺少图片资源：${Y2KM_LOGO_SRC}")
endif()

configure_file("${Y2KM_FONT_EN_SRC}" "${Y2KM_RES_DIR}/SilkscreenRegular.ttf" COPYONLY)

# Logo 图片
configure_file("${Y2KM_LOGO_SRC}" "${Y2KM_RES_DIR}/logo.png" COPYONLY)

juce_add_binary_data(Y2Kmeter_BinaryData
  SOURCES
    "${CMAKE_CURRENT_BINARY_DIR}/resources/SilkscreenRegular.ttf"
    "${CMAKE_CURRENT_BINARY_DIR}/resources/logo.png"
)

# ==========================================================
# libprojectM (Milkdrop 模块运行时依赖，LGPL-2.1-or-later)
#   · third_party/projectm/bin/*.dll —— 运行时通过 LoadLibraryW 显式加载
#   · third_party/projectm/include   —— C API 头文件，仅用于类型定义 /
#     函数指针 typedef；不会在链接期依赖任何 .lib
#   · 见 source/ui/modules/ProjectMApi.{h,cpp}
# ==========================================================
set(Y2KM_PROJECTM_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/third_party/projectm")
set(Y2KM_PROJECTM_INCLUDE "${Y2KM_PROJECTM_DIR}/include")
set(Y2KM_PROJECTM_BIN     "${Y2KM_PROJECTM_DIR}/bin")
set(Y2KM_MILKDROP_PRESETS "${CMAKE_CURRENT_SOURCE_DIR}/assets/milkdrop_presets")
set(Y2KM_MILKDROP_TEXTURES "${CMAKE_CURRENT_SOURCE_DIR}/assets/milkdrop_textures")
if (NOT EXISTS "${Y2KM_PROJECTM_INCLUDE}/projectM-4/projectM.h")
  message(FATAL_ERROR "缺少 libprojectM 头文件：${Y2KM_PROJECTM_INCLUDE}/projectM-4/projectM.h\n"
                      "请参考 third_party/projectm/README.md 重新拉取。")
endif()
if (WIN32 AND NOT EXISTS "${Y2KM_PROJECTM_BIN}/projectM-4.dll")
  message(FATAL_ERROR "缺少 libprojectM 运行时：${Y2KM_PROJECTM_BIN}/projectM-4.dll\n"
                      "请参考 third_party/projectm/README.md 重新拉取。")
endif()

target_compile_definitions(Y2Kmeter
  PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0
    # 关闭 JUCE 内置的 ARA / LV2 宿主集成。
    #   · 本项目既不作为 ARA/LV2 宿主、也不作为 ARA 插件运行，
    #     保留这些默认开关只会让 juce_audio_processors 多编一批
    #     我们完全用不到的符号（尤其 ARA 会拖入 ARA SDK 搜索路径）。
    #   · 跨平台（Win/macOS）都生效：把编译期体积和启动时
    #     "是否存在这些 runtime 挂载点" 的分支收敛掉。
    JUCE_PLUGINHOST_ARA=0
    JUCE_PLUGINHOST_LV2=0
    # 外网发布版默认关闭性能计数系统（导出 + 运行时统计计算）
    # 目的是尽可能减少冗余统计开销，把算力留给真实音频/绘制路径。
    Y2K_ENABLE_PERF_COUNTERS=0
    # 关闭 JUCE 默认的 StandaloneFilterApp / StandaloneFilterWindow，
    # 转而使用我们自定义的 source/standalone/Y2KStandaloneApp.cpp（无边框 Y2K 外壳）
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    # Milkdrop 模块现在使用 libprojectM（native OpenGL）而不是 WebView2；
    # 因此不再启用 JUCE_WEB_BROWSER。如果未来其他模块需要，可以按需重新打开。
)

# MSVC: 强制按 UTF-8 解析源码，支持中文注释与字符串
if (MSVC)
  target_compile_options(Y2Kmeter PRIVATE /utf-8)
endif()

target_sources(Y2Kmeter PRIVATE
  PluginProcessor.cpp
  PluginProcessor.h
  PluginEditor.cpp
  PluginEditor.h

  # === 音频分析层（Phase B / D）===
  source/analysis/LoudnessMeter.h
  source/analysis/LoudnessMeter.cpp
  source/analysis/AnalyserHub.h
  source/analysis/AnalyserHub.cpp
  source/analysis/PhaseCorrelator.cpp
  source/analysis/DynamicRangeMeter.cpp

  # === 性能计数系统 ===
  source/perf/PerformanceCounterSystem.h
  source/perf/PerformanceCounterSystem.cpp

  # === 模块化 UI 框架 ===

  source/ui/PinkXPStyle.h
  source/ui/PinkXPStyle.cpp
  source/ui/ModulePanel.h
  source/ui/ModulePanel.cpp
  source/ui/ModuleWorkspace.h
  source/ui/ModuleWorkspace.cpp
  # 阶段1 性能改造：统一 UI 帧时钟节拍器（支持自适应降频）
  #   · 当前不强制接线，各模块仍旧在 AnalyserHub::startFrameDispatcher 的
  #     驱动下 onFrame 重绘；但源文件先入库，便于后续分阶段迁移。
  source/ui/UiFrameClock.h
  source/ui/UiFrameClock.cpp

  # === 分析模块 ===
  source/ui/modules/EqModule.h
  source/ui/modules/EqModule.cpp
  source/ui/modules/LoudnessModule.h
  source/ui/modules/LoudnessModule.cpp
source/ui/modules/OscilloscopeModule.h
source/ui/modules/OscilloscopeModule.cpp
source/ui/modules/OscilloscopeWaveModule.h
source/ui/modules/OscilloscopeWaveModule.cpp
  source/ui/modules/SpectrumModule.h
  source/ui/modules/SpectrumModule.cpp
  source/ui/modules/PhaseModule.h
  source/ui/modules/PhaseModule.cpp
  source/ui/modules/DynamicsModule.h
  source/ui/modules/DynamicsModule.cpp
  source/ui/modules/FineSplitModules.h
  source/ui/modules/FineSplitModules.cpp
  source/ui/modules/WaveformModule.h
  source/ui/modules/WaveformModule.cpp
  source/ui/modules/SpectrogramModule.h
  source/ui/modules/SpectrogramModule.cpp
  source/ui/modules/Spectrogram3DModule.h
  source/ui/modules/Spectrogram3DModule.cpp
  source/ui/modules/TamagotchiModule.h
  source/ui/modules/TamagotchiModule.cpp
  source/ui/modules/MilkdropModule.h
  source/ui/modules/MilkdropModule.cpp
  source/ui/modules/ProjectMApi.h
  source/ui/modules/ProjectMApi.cpp

  # === Standalone 自定义入口（仅 Standalone 构建时启用，内部已用宏保护）===

  source/standalone/Y2KStandaloneApp.cpp
  source/standalone/WasapiLoopbackCapture.h
  source/standalone/WasapiLoopbackCapture.cpp
)

# ==========================================================
# macOS 专属源文件：桌面音频采集 + 音频转储调试
#   · MacDesktopAudioCapture.h/.mm —— ScreenCaptureKit 桌面音频采集
#   · AudioDumpRecorder.h/.cpp     —— macOS Standalone 调试用音频转储
#   · Windows 构建中完全不编译这些文件，保持主干零影响。
# ==========================================================
if (APPLE)
  target_sources(Y2Kmeter PRIVATE
    source/standalone/MacDesktopAudioCapture.h
    source/standalone/MacDesktopAudioCapture.mm
    source/standalone/AudioDumpRecorder.h
    source/standalone/AudioDumpRecorder.cpp
  )
endif()

target_include_directories(Y2Kmeter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Y2KM_PROJECTM_INCLUDE}
)

target_link_libraries(Y2Kmeter PRIVATE
  Y2Kmeter_BinaryData
  juce::juce_audio_utils
  juce::juce_dsp
  # GPU 合成层：Editor 里挂 juce::OpenGLContext 后，整个绘制管线会被翻译
  # 成 GL 批次在 GPU 上执行。
  #   · Windows：JUCE 为 Editor 的 HWND 创建一个 GL 子窗口，底层走系统 OpenGL
  #     / 硬件驱动（或 ANGLE 回退到 D3D）。
  #   · macOS：JUCE 走 NSOpenGLContext，Apple Silicon 下底层走 Metal。
  #   软光栅路径下的 fillCGRect / drawGlyphs / fillPath / strokePath 等 CPU
  #   热点消失，主线程只剩 draw 批次组装与提交。Standalone 与 VST3 共用同
  #   一个 Editor，两种形态都受益。
  juce::juce_opengl
)

# Windows 下 WASAPI Loopback 采集（source/standalone/WasapiLoopbackCapture.cpp）所需系统库：
#   · ole32 / uuid —— CoCreateInstance / IID_IMMDeviceEnumerator 等 COM 符号
#   · avrt         —— AvSetMmThreadCharacteristics（把采集线程提升为 Pro Audio 优先级，
#                      减少 loopback 丢帧；未启用 loopback 时不会调用，纯开销 0）
if (WIN32)
  target_link_libraries(Y2Kmeter PRIVATE ole32 uuid avrt)
endif()

# ==========================================================
# macOS 下 MacDesktopAudioCapture.mm 所需系统 framework
#   · ScreenCaptureKit —— 桌面 / 系统音频采集入口
#   · AVFoundation     —— 音频格式描述、设备管理
#   · CoreMedia        —— CMSampleBuffer / CMFormatDescription
#   · Foundation       —— NSObject / NSArray / Dispatch 等基础运行时
#   · 仅 APPLE 分支链接，Windows 不涉及任何变更。
# ==========================================================
if (APPLE)
  target_link_libraries(Y2Kmeter PRIVATE
    "-framework ScreenCaptureKit"
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework Foundation"
  )
endif()

# ==========================================================
# MSVC 调试信息策略：/Z7（把调试符号打进 .obj，不生成共享 PDB）
#   · 原因：在 CLion + CMake + 多线程 /MP 编译下，
#     多个 cl.exe 同时写同一个 Y2Kmeter_SharedCode.pdb 极易触发
#     C1090 "PDB API 调用失败，错误代码 3"（mspdbsrv.exe 竞争 / 文件占用）
#   · /Z7 等价于 /Zi 的调试体验，区别只是把 DWARF-like 调试段嵌入每个 .obj，
#     Linker 汇总时再生成可调试的 .exe / .pdb，不需要编译期共享 PDB
#   · 仅在 MSVC 下生效；其它工具链完全不受影响
# ==========================================================
if (MSVC)
  target_compile_options(Y2Kmeter PRIVATE /MP)

  target_compile_options(Y2Kmeter PRIVATE
    $<$<CONFIG:Debug>:/Z7>
    $<$<CONFIG:RelWithDebInfo>:/Z7>
  )

  target_compile_options(Y2Kmeter PRIVATE
    $<$<CONFIG:Release>:/O2 /Ob3 /Oi /Ot /Gy /Gw>
  )

  target_link_options(Y2Kmeter PRIVATE
    $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
  )

endif()

# ==========================================================
# LTO / PGO 应用到插件本体与 JUCE 生成的格式 target
# ==========================================================
set(Y2K_IPO_SUPPORTED FALSE)
if (Y2K_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT Y2K_IPO_SUPPORTED OUTPUT Y2K_IPO_ERROR LANGUAGES CXX)
  if (Y2K_IPO_SUPPORTED)
    message(STATUS "Y2Kmeter: LTO/IPO enabled for Release, RelWithDebInfo, and MinSizeRel")
  else()
    message(WARNING "Y2Kmeter: LTO/IPO requested but not supported by this toolchain: ${Y2K_IPO_ERROR}")
  endif()
endif()

function(y2k_apply_release_lto target_name)
  if (NOT TARGET "${target_name}")
    return()
  endif()

  if (Y2K_ENABLE_LTO AND Y2K_IPO_SUPPORTED)
    set_property(TARGET "${target_name}" PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET "${target_name}" PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set_property(TARGET "${target_name}" PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
  endif()
endfunction()

function(y2k_apply_release_pgo target_name)
  if (NOT TARGET "${target_name}" OR Y2K_PGO_MODE STREQUAL "OFF")
    return()
  endif()

  file(MAKE_DIRECTORY "${Y2K_PGO_DATA_DIR}")

  if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_pgd_file "${Y2K_PGO_DATA_DIR}/${target_name}.pgd")

    # MSVC PGO is link-time PGO, so force /GL + /LTCG on the trained Release target.
    target_compile_options("${target_name}" PRIVATE
      $<$<CONFIG:Release>:/GL>
    )
    target_link_options("${target_name}" PRIVATE
      $<$<CONFIG:Release>:/LTCG>
    )

    if (Y2K_PGO_MODE STREQUAL "GENERATE")
      target_link_options("${target_name}" PRIVATE
        $<$<CONFIG:Release>:/GENPROFILE:PGD=${_pgd_file}>
      )
    elseif (Y2K_PGO_MODE STREQUAL "USE")
      target_link_options("${target_name}" PRIVATE
        $<$<CONFIG:Release>:/USEPROFILE:PGD=${_pgd_file}>
      )
    endif()
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if (Y2K_PGO_MODE STREQUAL "GENERATE")
      target_compile_options("${target_name}" PRIVATE
        $<$<CONFIG:Release>:-fprofile-generate=${Y2K_PGO_DATA_DIR}>
      )
      target_link_options("${target_name}" PRIVATE
        $<$<CONFIG:Release>:-fprofile-generate=${Y2K_PGO_DATA_DIR}>
      )
    elseif (Y2K_PGO_MODE STREQUAL "USE")
      target_compile_options("${target_name}" PRIVATE
        $<$<CONFIG:Release>:-fprofile-use=${Y2K_PGO_DATA_DIR}>
        $<$<CONFIG:Release>:-fprofile-correction>
      )
      target_link_options("${target_name}" PRIVATE
        $<$<CONFIG:Release>:-fprofile-use=${Y2K_PGO_DATA_DIR}>
      )
    endif()
  else()
    message(WARNING
      "Y2Kmeter: Y2K_PGO_MODE=${Y2K_PGO_MODE} was requested, "
      "but automatic PGO flags are only configured for MSVC and GNU compilers.")
  endif()
endfunction()

foreach(_y2k_target IN ITEMS
    Y2Kmeter
    Y2Kmeter_SharedCode
    Y2Kmeter_VST3
    Y2Kmeter_Standalone)
  y2k_apply_release_lto("${_y2k_target}")
  y2k_apply_release_pgo("${_y2k_target}")
endforeach()

# ==========================================================
# Standalone EXE 图标（Windows）
#   · 通过一个独立的 .rc 把 assets/icon.ico 作为 EXE 的主图标嵌入。
#   · juce_add_plugin 的 ICON_BIG/ICON_SMALL 需要 png，这里我们已有 .ico，
#     所以绕过 JUCE，直接把 .rc 编译成 .res 再链接。
#   · 关键：不使用 target_sources 直接添加 .rc 文件，因为 CMake 会把它
#     当作 RC 源文件并把 target 的所有 INCLUDE_DIRECTORIES 作为 /I 参数
#     传给 rc.exe。JUCE 模块 + VST3 SDK 路径极多，命令行轻松超过 32767
#     字符上限，触发 NMAKE U1095（rc.exe 命令过长）。
#   · 改为用自定义命令以最小参数（仅 /fo 输出路径）调用 rc.exe，.res
#     结果直接作为链接输入，完全绕过命令行长度的限制。
#   · 不影响 VST3 / AAX 等其它格式的构建；非 Windows 平台上此段静默跳过。
#   · JUCE 默认生成的 Y2Kmeter_resources.rc（VERSION_INFO）与本 RC 并存，
#     互相不干扰（ID 不冲突）。
# ==========================================================
if (WIN32 AND TARGET Y2Kmeter_Standalone)
  set(Y2KM_APP_ICON_RC  "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.rc")
  set(Y2KM_APP_ICON_RES "${CMAKE_CURRENT_BINARY_DIR}/app_icon.res")
  set(Y2KM_APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")

  add_custom_command(
    OUTPUT "${Y2KM_APP_ICON_RES}"
    COMMAND "${CMAKE_RC_COMPILER}" /nologo /fo "${Y2KM_APP_ICON_RES}" "${Y2KM_APP_ICON_RC}"
    DEPENDS "${Y2KM_APP_ICON_RC}" "${Y2KM_APP_ICON_ICO}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    COMMENT "Compiling Windows resource (app icon)"
    VERBATIM
  )

  # EXTERNAL_OBJECT 告诉 CMake 这个 .res 已经是编译好的对象文件，
  # 应直接作为链接器输入（而非再次编译），确保图标正确嵌入 EXE。
  set_source_files_properties("${Y2KM_APP_ICON_RES}" PROPERTIES
    EXTERNAL_OBJECT TRUE
    GENERATED TRUE
  )
  target_sources(Y2Kmeter_Standalone PRIVATE "${Y2KM_APP_ICON_RES}")
endif()

# ==========================================================
# Standalone Dock 图标（macOS）
#   · 显式绑定 Icon.icns，避免仅靠生成流程隐式注入导致部分构建产物图标缺失。
#   · 仅 APPLE 分支执行，Windows 构建无感知。
# ==========================================================
if (APPLE AND TARGET Y2Kmeter_Standalone)
  set_target_properties(Y2Kmeter_Standalone PROPERTIES
    MACOSX_BUNDLE_ICON_FILE "Icon.icns"
  )
endif()

# ==========================================================
# Tamagotchi 模块的动画 / 蛋资源打包（macOS）
#   · 运行时查找逻辑（TamagotchiModule.cpp 中的 findTamagotchiSubDir）会优先从
#     本 bundle 的 Contents/Resources/assets/Tamagotchi 读取，因此构建数 bundle
#     后需要把源仓库 assets/Tamagotchi 同步进去。
#   · Windows / Linux：暂按源仓库 / 运行目录兜底（Y2Kmeter_installer.iss 另行处理），
#     这里的 bundle 路径模式只适用于 macOS .app / .vst3。
# ==========================================================
if (APPLE)
  set(Y2KM_TAMAGOTCHI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/Tamagotchi")

  function(y2km_copy_tamagotchi_into_bundle tgt bundle_subpath)
    if (NOT TARGET ${tgt})
      return()
    endif()

    # JUCE 会把产物放在 $<TARGET_BUNDLE_DIR:${tgt}>，指向 bundle 根（.vst3 / .app）
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf
              "$<TARGET_BUNDLE_DIR:${tgt}>/Contents/Resources/assets/Tamagotchi"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_BUNDLE_DIR:${tgt}>/Contents/Resources/assets"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${Y2KM_TAMAGOTCHI_SRC}"
              "$<TARGET_BUNDLE_DIR:${tgt}>/Contents/Resources/assets/Tamagotchi"
      COMMENT "Y2Kmeter: sync Tamagotchi assets into ${bundle_subpath}/Contents/Resources/assets/Tamagotchi"
      VERBATIM
    )
  endfunction()

  if (TARGET Y2Kmeter_Standalone)
    y2km_copy_tamagotchi_into_bundle(Y2Kmeter_Standalone "Y2Kmeter.app")
  endif()
endif()

# ==========================================================
# libprojectM DLL + Milkdrop 预设/纹理 —— Post-build 部署
#   · Standalone (Windows)：把 projectM-4.dll / glew32.dll 拷贝到
#     Y2Kmeter.exe 同目录；MilkdropModule::ProjectMApi 用 LoadLibraryW
#     从 exe 所在目录（默认 DLL 搜索路径）加载。
#   · VST3   (Windows)：拷贝到 Y2Kmeter.vst3\Contents\x86_64-win\ 旁边
#     （bundle DLL 同级目录）。VST3 宿主加载插件时该目录会被加入 DLL
#     搜索路径。
#   · macOS：暂未打包（frontend-sdl-cpp 未提供 macOS libprojectM DLL；
#     后续可从 Homebrew 或源码构建补齐，本改动只保证 Windows 场景完整）。
#   · 预设 / 纹理放在可执行文件同级 `milkdrop_presets` / `milkdrop_textures`
#     目录，路径解析由 MilkdropModule 完成（详见 ProjectMApi.cpp）。
# ==========================================================
if (WIN32)
  set(Y2KM_PROJECTM_DLLS
      "${Y2KM_PROJECTM_BIN}/projectM-4.dll"
      "${Y2KM_PROJECTM_BIN}/glew32.dll"
  )

  function(y2km_copy_projectm_runtime tgt dest_expr comment_tag)
    if (NOT TARGET ${tgt})
      return()
    endif()

    # 1) DLLs
    foreach(_dll IN LISTS Y2KM_PROJECTM_DLLS)
      add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "${dest_expr}/"
        COMMENT "Y2Kmeter[${comment_tag}]: copy $<PATH:GET_FILENAME,${_dll}> next to target"
        VERBATIM
      )
    endforeach()

    # 2) 预设目录
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${Y2KM_MILKDROP_PRESETS}"
              "${dest_expr}/milkdrop_presets"
      COMMENT "Y2Kmeter[${comment_tag}]: sync milkdrop_presets/"
      VERBATIM
    )

    # 3) 纹理目录
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${Y2KM_MILKDROP_TEXTURES}"
              "${dest_expr}/milkdrop_textures"
      COMMENT "Y2Kmeter[${comment_tag}]: sync milkdrop_textures/"
      VERBATIM
    )
  endfunction()

  # Standalone EXE 旁边（$<TARGET_FILE_DIR:Y2Kmeter_Standalone>）
  if (TARGET Y2Kmeter_Standalone)
    y2km_copy_projectm_runtime(Y2Kmeter_Standalone
      "$<TARGET_FILE_DIR:Y2Kmeter_Standalone>"
      "Standalone")
  endif()

  # VST3 bundle 内 x86_64-win 目录（$<TARGET_FILE_DIR:Y2Kmeter_VST3>）
  if (TARGET Y2Kmeter_VST3)
    y2km_copy_projectm_runtime(Y2Kmeter_VST3
      "$<TARGET_FILE_DIR:Y2Kmeter_VST3>"
      "VST3")
  endif()
endif()

# ==========================================================
# VST3 系统部署：将 projectM 运行时从构建产物目录拷贝到系统 VST3 目录
#   · COPY_PLUGIN_AFTER_BUILD 通过 cmake --install 只拷贝 VST3
#     二进制本身，不包含 post-build 中额外拷贝的 DLL/预设/纹理。
#     这里的 post-build 命令在每次编译后直接把文件同步到
#     C:\Program Files\Common Files\VST3\Y2Kmeter.vst3\Contents\x86_64-win\，
#     无需额外 cmake --install 步骤即可让 DAW 加载到完整插件。
# ==========================================================
if (WIN32 AND TARGET Y2Kmeter_VST3)
  set(Y2KM_VST3_INSTALL_DEST
      "$ENV{CommonProgramFiles}/VST3/Y2Kmeter.vst3/Contents/x86_64-win")

  # DLL
  foreach(_dll IN LISTS Y2KM_PROJECTM_DLLS)
    add_custom_command(TARGET Y2Kmeter_VST3 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_dll}" "${Y2KM_VST3_INSTALL_DEST}/"
      COMMENT "Y2Kmeter[VST3 deploy]: copy $<PATH:GET_FILENAME,${_dll}> to system VST3 dir"
      VERBATIM
    )
  endforeach()

  # 预设
  add_custom_command(TARGET Y2Kmeter_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${Y2KM_MILKDROP_PRESETS}"
            "${Y2KM_VST3_INSTALL_DEST}/milkdrop_presets"
    COMMENT "Y2Kmeter[VST3 deploy]: sync milkdrop_presets/"
    VERBATIM
  )

  # 纹理
  add_custom_command(TARGET Y2Kmeter_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${Y2KM_MILKDROP_TEXTURES}"
            "${Y2KM_VST3_INSTALL_DEST}/milkdrop_textures"
    COMMENT "Y2Kmeter[VST3 deploy]: sync milkdrop_textures/"
    VERBATIM
  )
endif()