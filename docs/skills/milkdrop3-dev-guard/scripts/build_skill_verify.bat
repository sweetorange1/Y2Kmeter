@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  Y2Kmeter milkdrop3-dev-guard Skill: build verification
REM ------------------------------------------------------------
REM  Designed for AI autonomy:
REM    - [SKILL-BUILD] markers appear on console IMMEDIATELY at each phase,
REM      so AI terminal never sees "total silence" even if build takes minutes.
REM    - All detailed cmake/nmake output is written to the log file below.
REM    - AI reads the log after the command to determine PASS/FAIL.
REM    - Exit code is propagated for quick judgment in tooling.
REM
REM  Usage (from I:/Y2KMeter/ Git Bash):
REM    cmd //c "docs\skills\milkdrop3-dev-guard\scripts\build_skill_verify.bat"
REM  Usage (from cmd.exe / PowerShell):
REM    cmd /c docs\skills\milkdrop3-dev-guard\scripts\build_skill_verify.bat
REM
REM  Log file: build\skill-verify\_build_log.txt
REM    → grep "[PASS]" build\skill-verify\_build_log.txt   (success check)
REM    → grep -i "error " build\skill-verify\_build_log.txt (failure details)
REM ============================================================

set "LOG=build\skill-verify\_build_log.txt"
set "BUILD_DIR=build\skill-verify"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"

REM --- ensure build dir + start log ---
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
echo [%DATE% %TIME%] === Y2Kmeter Skill Build Start === > "%LOG%"

REM ========================================================================
REM  Phase 1/3: VS toolchain
REM ========================================================================
echo [SKILL-BUILD] [1/3] Initializing VS 2026 toolchain...
echo [SKILL-BUILD] [1/3] Initializing VS 2026 toolchain... >> "%LOG%"

if not exist "%VCVARS%" (
  echo [SKILL-BUILD] [FAIL] vcvars64.bat not found: %VCVARS%
  echo [SKILL-BUILD] [FAIL] vcvars64.bat not found: %VCVARS% >> "%LOG%"
  exit /b 1
)

REM  vcvars output is verbose (~300 lines) and spawns child processes.
REM  Redirecting to the main log file causes child processes to inherit
REM  the file handle, locking it and preventing subsequent writes.
REM  Instead, redirect vcvars to nul and only log success/failure.
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo [SKILL-BUILD] [FAIL] VS environment init failed ^(code !ERRORLEVEL!^)
  echo [SKILL-BUILD] [FAIL] VS environment init failed >> "%LOG%"
  exit /b 1
)
echo [SKILL-BUILD] VS toolchain ready.
echo [SKILL-BUILD] VS toolchain ready. >> "%LOG%"

REM ========================================================================
REM  Phase 2/3: CMake configure (only when cache missing)
REM ========================================================================
echo [SKILL-BUILD] [2/3] CMake configure...
echo [SKILL-BUILD] [2/3] CMake configure... >> "%LOG%"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
  cmake -S . -B "%BUILD_DIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo >> "%LOG%" 2>&1
  if errorlevel 1 (
    echo [SKILL-BUILD] [FAIL] CMake configure failed
    echo [SKILL-BUILD] [FAIL] CMake configure failed >> "%LOG%"
    exit /b 1
  )
  echo [SKILL-BUILD] CMake configure done.
  echo [SKILL-BUILD] CMake configure done. >> "%LOG%"
) else (
  echo [SKILL-BUILD] CMake cache exists, skip configure.
  echo [SKILL-BUILD] CMake cache exists, skip configure. >> "%LOG%"
)

REM ========================================================================
REM  Phase 3/3: Build (NMake, full project, the slow part)
REM ========================================================================
echo [SKILL-BUILD] [3/3] Building with NMake (this may take several minutes)...
echo [SKILL-BUILD] Log: %CD%\%LOG%
echo [SKILL-BUILD] [3/3] Building with NMake (this may take several minutes)... >> "%LOG%"

cmake --build "%BUILD_DIR%" -j >> "%LOG%" 2>&1
set BUILD_EXIT=!ERRORLEVEL!

echo [SKILL-BUILD] Build phase finished, exit code: !BUILD_EXIT!
echo [SKILL-BUILD] Build phase finished, exit code: !BUILD_EXIT! >> "%LOG%"

REM ========================================================================
REM  Verdict
REM ========================================================================
if !BUILD_EXIT! equ 0 (
  echo [SKILL-BUILD] [PASS] All targets built successfully.
  echo [SKILL-BUILD] [PASS] All targets built successfully. >> "%LOG%"
) else (
  echo [SKILL-BUILD] [FAIL] Build FAILED with exit code !BUILD_EXIT!.
  echo [SKILL-BUILD] [FAIL] Build FAILED with exit code !BUILD_EXIT!. >> "%LOG%"
  echo [SKILL-BUILD] To locate errors, run: grep -i "error " "%LOG%"
  echo [SKILL-BUILD] To locate errors, run: grep -i "error " "%LOG%" >> "%LOG%"
)

echo [%DATE% %TIME%] === Build End === >> "%LOG%"
exit /b !BUILD_EXIT!