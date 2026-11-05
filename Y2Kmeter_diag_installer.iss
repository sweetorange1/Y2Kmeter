; =============================================================================
;  Y2Kmeter_diag_installer.iss —— 诊断专用安装包（发给用户复现问题用）
;
;  与正式安装包（Y2Kmeter_installer.iss）的唯一区别：
;    · 产物取自 RelWithDebInfo 构建目录：性能与 Release 基本一致（/O2 + NDEBUG），
;      但保留 PDB —— 现场卡死时生成的 Minidump 才能解析出函数名与行号；
;    · 输出文件名与"卸载列表 / 安装向导"里显示的名称带 Diag 后缀，
;      避免和正式包混淆；
;    · AppId 与正式包保持一致 → 诊断包会原地覆盖正式安装，装完不用手动清理。
;
;  其余所有安装逻辑（组件页、VST3 目录页、预设/纹理解压、升级清理）全部复用
;  Y2Kmeter_installer.iss，不在这里复制粘贴。
;
;  用法：双击 build_installer_diag.bat，或直接
;        "ISCC.exe" Y2Kmeter_diag_installer.iss
; =============================================================================

; 构建配置名 → artefacts 下的配置子目录
#define MyConfig        "RelWithDebInfo"

; 诊断构建目录（与正式包的 cmake-build-release-visual-studio 区分开）
#define MyBuildDir      "cmake-build-relwithdebinfo-visual-studio-ninja"

; 输出文件后缀 / 安装向导与卸载列表里显示的版本后缀
#define MyOutputSuffix  "_Diag"
#define MyVersionSuffix " (Diag)"

#include "Y2Kmeter_installer.iss"
