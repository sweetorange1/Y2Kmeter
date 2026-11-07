; =============================================================================
;  Y2Kmeter_diagrel_installer.iss —— "Release + 诊断日志默认开" 安装包
;
;  为什么需要这一份：
;    · 正式包（Y2Kmeter_installer.iss）是 Release，运行时诊断日志**默认关闭**；
;    · 诊断包（Y2Kmeter_diag_installer.iss）是 RelWithDebInfo（无 LTO），
;      实测在外网机器上**复现不了** Milkdrop 黑屏；
;    · 所以要用"和正式包完全一致的 Release 编译配置（/O2 + /GL + /LTCG）"，
;      但把运行时诊断日志**默认打开**（-DY2K_DIAG_LOG_DEFAULT_ON=ON），
;      这样才能在"会出问题的那个配置"里拿到 [MD] 日志。
;
;  构建目录：cmake-build-release-diag（独立目录，不影响正式产物）
;     cmake -S . -B cmake-build-release-diag -G Ninja -DCMAKE_BUILD_TYPE=Release \
;           -DFETCHCONTENT_SOURCE_DIR_JUCE=D:/Code/Y2KMeter/third_party/JUCE \
;           -DY2K_DIAG_LOG_DEFAULT_ON=ON
;
;  用法：
;     "ISCC.exe" Y2Kmeter_diagrel_installer.iss
;  产物：dist\Y2Kmeter_Setup_<版本>_DiagRel_x64.exe
; =============================================================================

; 构建配置名 → artefacts 下的配置子目录
#define MyConfig        "Release"

; 专用构建目录（与正式包、RelWithDebInfo 诊断包都区分开）
#define MyBuildDir      "cmake-build-release-diag"

; 输出文件名 / 安装向导与卸载列表里显示的版本后缀
#define MyOutputSuffix  "_DiagRel"
#define MyVersionSuffix " (Diag-Rel)"

#include "Y2Kmeter_installer.iss"
