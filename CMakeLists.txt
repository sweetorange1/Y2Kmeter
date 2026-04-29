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
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

project(Y2Kmeter VERSION 1.5.0 LANGUAGES C CXX)

if (APPLE)
  enable_language(OBJCXX)
endif()

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

include(FetchContent)
FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.12
)
FetchContent_MakeAvailable(juce)

# add_subdirectory(grid_plugin)

# 图标资源：统一以 assets/icon.ico 为源。
#   · Windows Standalone 仍使用 app_icon.rc 直接嵌入 .ico
#   · macOS 通过 JUCE 的 ICON_BIG/ICON_SMALL（png）配置 app bundle 图标
set(Y2KM_RES_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources")
file(MAKE_DIRECTORY "${Y2KM_RES_DIR}")

set(Y2KM_ICON_ICO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
set(Y2KM_ICON_PNG_DST "${Y2KM_RES_DIR}/icon.png")
if (NOT EXISTS "${Y2KM_ICON_ICO_SRC}")
  message(FATAL_ERROR "缺少图标资源：${Y2KM_ICON_ICO_SRC}")
endif()

set(Y2KM_ICON_PLUGIN_ARGS)
if (APPLE)
  execute_process(
    COMMAND /usr/bin/sips -s format png "${Y2KM_ICON_ICO_SRC}" --out "${Y2KM_ICON_PNG_DST}"
    RESULT_VARIABLE Y2KM_ICON_CONVERT_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if (NOT Y2KM_ICON_CONVERT_RESULT EQUAL 0)
    message(FATAL_ERROR "无法将 ${Y2KM_ICON_ICO_SRC} 转换为 PNG（sips 失败）")
  endif()

  # 额外生成标准 macOS 多分辨率 Icon.icns，避免仅依赖 JUCE 的图标管线导致 Dock 图标缺失。
  set(Y2KM_ICONSET_DIR "${Y2KM_RES_DIR}/Icon.iconset")
  set(Y2KM_ICON_ICNS_DST "${Y2KM_RES_DIR}/Icon.icns")
  file(REMOVE_RECURSE "${Y2KM_ICONSET_DIR}")
  file(MAKE_DIRECTORY "${Y2KM_ICONSET_DIR}")

  foreach(_sz 16 32 128 256 512)
    math(EXPR _sz2 "${_sz} * 2")

    execute_process(
      COMMAND /usr/bin/sips -z ${_sz} ${_sz} "${Y2KM_ICON_PNG_DST}" --out "${Y2KM_ICONSET_DIR}/icon_${_sz}x${_sz}.png"
      RESULT_VARIABLE Y2KM_ICONSET_RESIZE_RESULT
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if (NOT Y2KM_ICONSET_RESIZE_RESULT EQUAL 0)
      message(FATAL_ERROR "生成 icon_${_sz}x${_sz}.png 失败")
    endif()

    execute_process(
      COMMAND /usr/bin/sips -z ${_sz2} ${_sz2} "${Y2KM_ICON_PNG_DST}" --out "${Y2KM_ICONSET_DIR}/icon_${_sz}x${_sz}@2x.png"
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
        VERSION 1.6.0
        COMPANY_NAME "iisaacbeats.cn"
        PLUGIN_MANUFACTURER_CODE Y2KM
        PLUGIN_CODE Y2Km
        FORMATS VST3 Standalone
        ${Y2KM_ICON_PLUGIN_ARGS}
        # 本插件是纯分析表（EQ 分析仪 / LUFS / 示波器 / 频谱 / 相位 / 动态等），
        # 对应 VST3 SDK 的 Analyzer 类别，让 DAW 的 FX 分类浏览器正确归类。
        VST3_CATEGORIES "Analyzer" "Fx"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
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

target_compile_definitions(Y2Kmeter
  PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_ARA=0
    JUCE_PLUGINHOST_LV2=0
    # 关闭 JUCE 默认的 StandaloneFilterApp / StandaloneFilterWindow，
    # 转而使用我们自定义的 source/standalone/Y2KStandaloneApp.cpp（无边框 Y2K 外壳）
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
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

  # === 分析模块 ===
  source/ui/modules/EqModule.h
  source/ui/modules/EqModule.cpp
  source/ui/modules/LoudnessModule.h
  source/ui/modules/LoudnessModule.cpp
  source/ui/modules/OscilloscopeModule.h
  source/ui/modules/OscilloscopeModule.cpp
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
  source/ui/modules/TamagotchiModule.h
  source/ui/modules/TamagotchiModule.cpp

  # === Standalone 自定义入口（仅 Standalone 构建时启用，内部已用宏保护）===

  source/standalone/Y2KStandaloneApp.cpp
  source/standalone/WasapiLoopbackCapture.h
  source/standalone/WasapiLoopbackCapture.cpp
  source/standalone/AudioDumpRecorder.h
  source/standalone/AudioDumpRecorder.cpp
)

if (APPLE)
  target_sources(Y2Kmeter PRIVATE
    source/standalone/MacDesktopAudioCapture.h
    source/standalone/MacDesktopAudioCapture.mm
  )
endif()

target_include_directories(Y2Kmeter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Y2Kmeter PRIVATE Y2Kmeter_BinaryData juce::juce_audio_utils juce::juce_dsp)

# Windows 下 WASAPI Loopback 采集（source/standalone/WasapiLoopbackCapture.cpp）所需系统库：
#   · ole32 / uuid —— CoCreateInstance / IID_IMMDeviceEnumerator 等 COM 符号
#   · avrt         —— AvSetMmThreadCharacteristics（把采集线程提升为 Pro Audio 优先级，
#                      减少 loopback 丢帧；未启用 loopback 时不会调用，纯开销 0）
if (WIN32)
  target_link_libraries(Y2Kmeter PRIVATE ole32 uuid avrt)
endif()

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
  target_compile_options(Y2Kmeter PRIVATE
    $<$<CONFIG:Debug>:/Z7>
    $<$<CONFIG:RelWithDebInfo>:/Z7>
  )
endif()

# ==========================================================
# Standalone EXE 图标（Windows）
#   · 通过一个独立的 .rc 把 assets/icon.ico 作为 EXE 的主图标嵌入。
#   · juce_add_plugin 的 ICON_BIG/ICON_SMALL 需要 png，这里我们已有 .ico，
#     所以绕过 JUCE，直接给 Standalone target 追加一个 RC 资源文件。
#   · 不影响 VST3 / AAX 等其它格式的构建；非 Windows 平台上此段静默跳过。
#   · JUCE 默认生成的 Y2Kmeter_resources.rc（VERSION_INFO）与本 RC 并存，
#     互相不干扰（ID 不冲突）。
# ==========================================================
if (WIN32 AND TARGET Y2Kmeter_Standalone)
  target_sources(Y2Kmeter_Standalone PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.rc"
  )
endif()

if (APPLE AND TARGET Y2Kmeter_Standalone)
  # 显式绑定 Dock 图标，避免仅靠生成流程隐式注入导致部分构建产物图标缺失。
  set_target_properties(Y2Kmeter_Standalone PROPERTIES
    MACOSX_BUNDLE_ICON_FILE "Icon.icns"
  )
endif()
