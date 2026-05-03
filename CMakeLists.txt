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

project(Y2Kmeter VERSION 1.6.0 LANGUAGES C CXX)

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
set(JUCE_BUILD_HELPER_TOOLS ON CACHE BOOL
    "Build JUCE helper tools in the current configure pass" FORCE)

FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.12
)
FetchContent_MakeAvailable(juce)

if (TARGET juceaide AND NOT TARGET juce::juceaide)
  add_executable(juce::juceaide ALIAS juceaide)
endif()

# add_subdirectory(grid_plugin)

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
)

target_include_directories(Y2Kmeter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
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
