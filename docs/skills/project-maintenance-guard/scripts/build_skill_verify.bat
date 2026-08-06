@echo off
setlocal EnableDelayedExpansion

set "LOG=build\skill-verify\_build_log.txt"
set "BUILD_DIR=build\skill-verify"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
echo [%DATE% %TIME%] === Y2Kmeter Skill Build Start === > "%LOG%"

echo [SKILL-BUILD] [1/3] Initializing VS 2026 toolchain...
echo [SKILL-BUILD] [1/3] Initializing VS 2026 toolchain... >> "%LOG%"

if not exist "%VCVARS%" (
  echo [SKILL-BUILD] [FAIL] vcvars64.bat not found: %VCVARS%
  echo [SKILL-BUILD] [FAIL] vcvars64.bat not found: %VCVARS% >> "%LOG%"
  exit /b 1
)

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo [SKILL-BUILD] [FAIL] VS environment init failed ^(code !ERRORLEVEL!^)
  echo [SKILL-BUILD] [FAIL] VS environment init failed >> "%LOG%"
  exit /b 1
)

echo [SKILL-BUILD] VS toolchain ready.
echo [SKILL-BUILD] VS toolchain ready. >> "%LOG%"

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

echo [SKILL-BUILD] [3/3] Building with NMake (this may take several minutes)...
echo [SKILL-BUILD] Log: %CD%\%LOG%
echo [SKILL-BUILD] [3/3] Building with NMake (this may take several minutes)... >> "%LOG%"

cmake --build "%BUILD_DIR%" -j >> "%LOG%" 2>&1
set BUILD_EXIT=!ERRORLEVEL!

echo [SKILL-BUILD] Build phase finished, exit code: !BUILD_EXIT!
echo [SKILL-BUILD] Build phase finished, exit code: !BUILD_EXIT! >> "%LOG%"

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