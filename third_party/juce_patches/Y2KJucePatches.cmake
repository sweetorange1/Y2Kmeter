# ==========================================================
# Y2Kmeter 对 JUCE 8.0.12 的本地补丁：配置期完整性校验
#   每个补丁对应若干"文件|标记字符串"。配置时逐一检查实际使用的 JUCE 源码
#   是否含有这些标记；缺失说明 JUCE 被替换成了未打补丁的版本（例如升级 /
#   重新 clone / FetchContent 拉了原版），直接报错，避免优化静默丢失。
#   确需用原版 JUCE 构建时传 -DY2K_ALLOW_UNPATCHED_JUCE=ON（仅警告）。
#
#   新增补丁时：①把 diff 导出到本目录（编号递增）②在下表补标记 ③更新 README.md
# ==========================================================

set(Y2K_JUCE_PATCH_MARKERS
  # 0001 PopupMenu::Options::withParentAreaOverride（v2.8.6）
  "modules/juce_gui_basics/menus/juce_PopupMenu.h|withParentAreaOverride"
  # 0002 混合 DPI 多屏显示器表 / 物理几何（v2.9.9）
  "modules/juce_gui_basics/desktop/juce_Displays.cpp|Y2K patch"
  "modules/juce_gui_basics/native/juce_Windowing_windows.cpp|Y2K patch"
  # 0003 Font 比较快速路径（v3.0.2 mac 性能）
  "modules/juce_graphics/fonts/juce_Font.cpp|[Y2Kmeter patch]"
  "modules/juce_graphics/fonts/juce_FontOptions.cpp|[Y2Kmeter patch]"
  # 0004 mac GL VBO orphaning（v3.0.2 mac 性能）
  "modules/juce_opengl/opengl/juce_OpenGLGraphicsContext.cpp|[Y2Kmeter patch] VBO orphaning"
  # 0005 mac GL 组件重绘按显示器刷新合并（v3.0.2 mac 性能）
  "modules/juce_opengl/opengl/juce_OpenGLContext.h|setComponentRepaintsPacedToDisplay"
  "modules/juce_opengl/opengl/juce_OpenGLContext.cpp|pacedRepaintPending"
)

function(y2k_verify_juce_patches juce_dir)
  set(_missing "")

  foreach(_entry IN LISTS Y2K_JUCE_PATCH_MARKERS)
    string(FIND "${_entry}" "|" _sep)
    string(SUBSTRING "${_entry}" 0 ${_sep} _file)
    math(EXPR _markerStart "${_sep} + 1")
    string(SUBSTRING "${_entry}" ${_markerStart} -1 _marker)

    set(_path "${juce_dir}/${_file}")
    if (NOT EXISTS "${_path}")
      list(APPEND _missing "${_file}（文件不存在）")
      continue()
    endif()

    file(READ "${_path}" _content)
    string(FIND "${_content}" "${_marker}" _pos)
    if (_pos EQUAL -1)
      list(APPEND _missing "${_file}（缺少标记 \"${_marker}\"）")
    endif()
  endforeach()

  if (_missing)
    list(JOIN _missing "\n  " _missingText)
    set(_msg "当前 JUCE（${juce_dir}）缺少 Y2Kmeter 本地补丁：\n  ${_missingText}\n"
             "补丁文件在 third_party/juce_patches/，用法见该目录 README.md。")
    if (Y2K_ALLOW_UNPATCHED_JUCE)
      message(WARNING "${_msg}")
    else()
      message(FATAL_ERROR "${_msg}\n（确需用原版 JUCE 构建：-DY2K_ALLOW_UNPATCHED_JUCE=ON）")
    endif()
  else()
    message(STATUS "Y2K: JUCE 本地补丁校验通过")
  endif()
endfunction()
