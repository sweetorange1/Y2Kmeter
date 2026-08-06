@echo off
setlocal EnableDelayedExpansion

set "LOG=build\skill-verify\_build_log.txt"
set "BUILD_DIR=build\skill-verify"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"

echo [%DATE% %TIME%] === Incremental Build Start === >> "%LOG%"

echo [SKILL-BUILD] [1/2] Initializing VS toolchain...
echo [SKILL-BUILD] [1/2] Initializing VS toolchain... >> "%LOG%"

if not exist "%VCVARS%" (
  echo [SKILL-BUILD] [FAIL] vcvars not found
  echo [SKILL-BUILD] [FAIL] vcvars not found >> "%LOG%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo [SKILL-BUILD] [FAIL] VS init failed
  echo [SKILL-BUILD] [FAIL] VS init failed >> "%LOG%"
  exit /b 1
)

echo [SKILL-BUILD] VS ready.
echo [SKILL-BUILD] VS ready. >> "%LOG%"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo [SKILL-BUILD] [FAIL] CMake cache missing — run build_skill_verify.bat first
  echo [SKILL-BUILD] [FAIL] CMake cache missing >> "%LOG%"
  exit /b 1
)

echo [SKILL-BUILD] [2/2] Incremental build (NMake, changed files only)...
echo [SKILL-BUILD] [2/2] Incremental build (NMake, changed files only)... >> "%LOG%"

cmake --build "%BUILD_DIR%" -j >> "%LOG%" 2>&1
set BUILD_EXIT=!ERRORLEVEL!

if !BUILD_EXIT! equ 0 (
  echo [SKILL-BUILD] [PASS] Incremental build succeeded.
  echo [SKILL-BUILD] [PASS] Incremental build succeeded. >> "%LOG%"
) else (
  echo [SKILL-BUILD] [FAIL] Incremental build FAILED ^(exit !BUILD_EXIT!^).
  echo [SKILL-BUILD] [FAIL] Incremental build FAILED ^(exit !BUILD_EXIT!^). >> "%LOG%"
)

echo [%DATE% %TIME%] === Incremental Build End === >> "%LOG%"
exit /b !BUILD_EXIT!