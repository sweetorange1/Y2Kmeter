@echo off
REM ============================================================
REM  Y2Kmeter milkdrop3-dev-guard Skill: build verification
REM ------------------------------------------------------------
REM  Usage (from I:/Y2KMeter/):
REM    cmd /c docs\skills\milkdrop3-dev-guard\scripts\build_skill_verify.bat
REM
REM  Result:
REM    build/skill-verify/Y2Kmeter_artefacts/RelWithDebInfo/Standalone/Y2Kmeter.exe
REM    build/skill-verify/Y2Kmeter_artefacts/RelWithDebInfo/VST3/.../Y2Kmeter.vst3
REM ============================================================
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
  echo [ERR] vcvars64.bat not found at %VCVARS%
  echo       Edit this script to match your Visual Studio install path.
  exit /b 1
)
call %VCVARS% >nul
if errorlevel 1 (
  echo [ERR] vcvars64.bat failed
  exit /b 1
)
if not exist build\skill-verify (
  cmake -S . -B build\skill-verify -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
  if errorlevel 1 exit /b 1
)
cmake --build build\skill-verify -j
exit /b %errorlevel%
