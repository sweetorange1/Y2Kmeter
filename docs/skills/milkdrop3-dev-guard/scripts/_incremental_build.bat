@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  Y2Kmeter milkdrop3-dev-guard Skill: INCREMENTAL build
REM ------------------------------------------------------------
REM  Use when CMake configure was already done and only .cpp
REM  files changed. Much faster than a full cmake --build round.
REM
REM  Prerequisite: build\skill-verify\CMakeCache.txt must exist
REM  (i.e. build_skill_verify.bat was run at least once before).
REM
REM  Usage (from I:/Y2KMeter/ Git Bash):
REM    cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_incremental_build.bat"
REM  Usage (from cmd.exe / PowerShell):
REM    cmd /c docs\skills\milkdrop3-dev-guard\scripts\_incremental_build.bat
REM
REM  Log: same file as build_skill_verify.bat → build\skill-verify\_build_log.txt
REM    (appends to existing log so you have full history)
REM ============================================================

set "LOG=build\skill-verify\_build_log.txt"
set "BUILD_DIR=build\skill-verify"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"

echo [%DATE% %TIME%] === Incremental Build Start === >> "%LOG%"

REM --- VS environment ---
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

REM --- check cache ---
if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo [SKILL-BUILD] [FAIL] CMake cache missing — run build_skill_verify.bat first
  echo [SKILL-BUILD] [FAIL] CMake cache missing >> "%LOG%"
  exit /b 1
)

REM --- build (skip configure) ---
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
