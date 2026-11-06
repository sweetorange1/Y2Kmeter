# ==========================================================
# 把 third_party/juce_patches/*.patch 按文件名顺序打到当前目录（JUCE 源码根）。
#   用法（脚本模式）：
#     cd <JUCE 源码根>
#     cmake -DPATCH_DIR=<.../third_party/juce_patches> -P ApplyJucePatches.cmake
#   已打过的补丁（可反向应用）自动跳过，重复执行安全。
#   CMakeLists.txt 在 FetchContent 拉取原版 JUCE 时作为 PATCH_COMMAND 调用。
# ==========================================================
cmake_minimum_required(VERSION 3.22)

if (NOT PATCH_DIR)
  message(FATAL_ERROR "ApplyJucePatches.cmake: PATCH_DIR 未设置")
endif()

find_package(Git REQUIRED)

file(GLOB _patches "${PATCH_DIR}/*.patch")
list(SORT _patches)

foreach(_p IN LISTS _patches)
  get_filename_component(_name "${_p}" NAME)

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${_p}"
    RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)

  if (_already EQUAL 0)
    message(STATUS "Y2K JUCE patch: ${_name} 已存在，跳过")
    continue()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${_p}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)

  if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "Y2K JUCE patch: ${_name} 打补丁失败（JUCE 版本不匹配？）\n${_err}")
  endif()

  message(STATUS "Y2K JUCE patch: ${_name} 已应用")
endforeach()
