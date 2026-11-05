@echo off
setlocal

:: ===========================================================================
::  打包"诊断版"安装程序（发给用户复现问题用）
::
::  与 build_installer.bat 的区别仅在于取产物的地方：
::    正式包 → cmake-build-release-visual-studio          \Y2Kmeter_artefacts\Release\...
::    诊断包 → cmake-build-relwithdebinfo-visual-studio-ninja\Y2Kmeter_artefacts\RelWithDebInfo\...
::  安装逻辑、组件、预设解压全部复用 Y2Kmeter_installer.iss（见 Y2Kmeter_diag_installer.iss）。
::
::  前置条件：该构建目录必须是带诊断日志开关配置过的 RelWithDebInfo 构建：
::    cmake -S . -B cmake-build-relwithdebinfo-visual-studio-ninja -DY2K_DIAG_LOG_DEFAULT_ON=ON
:: ===========================================================================

set "SCRIPT_DIR=%~dp0"
set "ISS_FILE=%SCRIPT_DIR%Y2Kmeter_diag_installer.iss"

set "BUILD_DIR=%SCRIPT_DIR%cmake-build-relwithdebinfo-visual-studio-ninja"
set "CONFIG=RelWithDebInfo"

set "EXE_PATH=%BUILD_DIR%\Y2Kmeter_artefacts\%CONFIG%\Standalone\Y2Kmeter.exe"
set "VST3_PATH=%BUILD_DIR%\Y2Kmeter_artefacts\%CONFIG%\VST3\Y2Kmeter.vst3"
set "VST3_MD_PATH=%BUILD_DIR%\Y2Kmeter_milkdrop_artefacts\%CONFIG%\VST3\Y2Kmeter_milkdrop.vst3"

set "ISCC_PATH=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC_PATH%" set "ISCC_PATH=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC_PATH%" set "ISCC_PATH=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"

echo ============================================================
echo  Y2Kmeter 诊断包打包
echo ============================================================
echo  脚本      : %ISS_FILE%
echo  构建目录  : %BUILD_DIR%
echo  配置      : %CONFIG%
echo.

if not exist "%ISS_FILE%" (
  echo [ERROR] 未找到安装脚本: "%ISS_FILE%"
  echo.
  echo 按任意键关闭窗口...
  pause >nul
  exit /b 1
)

if not exist "%ISCC_PATH%" (
  echo [ERROR] 未找到 ISCC.exe，请先安装 Inno Setup 6。
  echo 你可以运行: winget install --id JRSoftware.InnoSetup -e
  echo.
  echo 按任意键关闭窗口...
  pause >nul
  exit /b 1
)

:: ---------------------------------------------------------------------------
:: 1) 产物检查 —— 缺文件时直接说清楚缺哪个，不要让 ISCC 抛 "Source file not found"
:: ---------------------------------------------------------------------------
set "MISSING=0"

if not exist "%EXE_PATH%" (
  echo [ERROR] 缺少 Standalone 产物:
  echo         %EXE_PATH%
  set "MISSING=1"
)
if not exist "%VST3_PATH%" (
  echo [ERROR] 缺少 VST3 bundle:
  echo         %VST3_PATH%
  set "MISSING=1"
)
if not exist "%VST3_MD_PATH%" (
  echo [ERROR] 缺少 Milkdrop-only VST3 bundle:
  echo         %VST3_MD_PATH%
  set "MISSING=1"
)

if "%MISSING%"=="1" (
  echo.
  echo 请先在 Visual Studio / 命令行里编译 RelWithDebInfo 配置，再重新运行本脚本。
  echo.
  echo 按任意键关闭窗口...
  pause >nul
  exit /b 1
)

:: ---------------------------------------------------------------------------
:: 2) 诊断开关检查 —— 开关为 OFF 时打出来的包不会写日志，等于白跑一趟
:: ---------------------------------------------------------------------------
::   诊断日志在 RelWithDebInfo 下默认开启（Y2K_ENABLE_LOGGING），
::   因此这里直接查编译命令里有没有日志宏，比查缓存变量更准。
set "DIAG_LOG_ON=0"
findstr /C:"-DY2K_DIAG_LOG_DEFAULT=1" "%BUILD_DIR%\build.ninja" >nul 2>&1
if not errorlevel 1 set "DIAG_LOG_ON=1"
findstr /C:"-DY2K_ENABLE_LOGGING=1" "%BUILD_DIR%\build.ninja" >nul 2>&1
if not errorlevel 1 set "DIAG_LOG_ON=1"
findstr /C:"Y2K_DIAG_LOG_DEFAULT_ON:BOOL=ON" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
if not errorlevel 1 set "DIAG_LOG_ON=1"

if "%DIAG_LOG_ON%"=="0" (
  echo [WARN] 这个构建里没有任何日志宏，打出来的包不会写 Y2Kmeter_Diag 日志！
  echo        正常情况应有 Y2K_ENABLE_LOGGING=1（RelWithDebInfo 自动带）。
  echo.
  echo        处理办法：用 RelWithDebInfo 配置重新编译，或显式打开诊断日志：
  echo        cmake -S "%SCRIPT_DIR%." -B "%BUILD_DIR%" -DY2K_DIAG_LOG_DEFAULT_ON=ON
  echo.
  choice /C YN /N /M "仍要继续打包吗? [Y=继续 / N=退出] "
  if errorlevel 2 (
    echo 已取消。
    echo.
    echo 按任意键关闭窗口...
    pause >nul
    exit /b 1
  )
  echo.
)

:: ---------------------------------------------------------------------------
:: 3) 打印产物信息（确认打的是刚刚编译出来的那份，而不是几天前的旧文件）
:: ---------------------------------------------------------------------------
echo [INFO] 产物:
for %%F in ("%EXE_PATH%") do echo        exe    %%~tF   %%~zF bytes
for %%F in ("%VST3_PATH%\Contents\x86_64-win\Y2Kmeter.vst3") do echo        vst3   %%~tF   %%~zF bytes
for %%F in ("%VST3_MD_PATH%\Contents\x86_64-win\Y2Kmeter_milkdrop.vst3") do echo        vst3md %%~tF   %%~zF bytes
for %%F in ("%BUILD_DIR%\Y2Kmeter_artefacts\%CONFIG%\Standalone\Y2Kmeter.pdb") do echo        pdb    %%~tF   %%~zF bytes
echo.

:: ---------------------------------------------------------------------------
:: 4) 调用 ISCC
:: ---------------------------------------------------------------------------
echo [INFO] 使用编译器: "%ISCC_PATH%"
echo [INFO] 开始打包...
echo.

cd /d "%SCRIPT_DIR%"
"%ISCC_PATH%" "%ISS_FILE%"

if errorlevel 1 (
  echo.
  echo [ERROR] 打包失败，请查看上方 ISCC 日志。
  echo.
  echo 按任意键关闭窗口...
  pause >nul
  exit /b 1
)

echo.
echo [OK] 诊断包打包完成，输出目录: "%SCRIPT_DIR%dist"
echo.
echo 提醒:
echo   · 发出去之前先在自己机器上装一次，确认桌面出现 Y2Kmeter_Diag 文件夹并写入了日志；
echo   · 保留本次构建目录里的 Y2Kmeter.pdb，用户回传的 .dmp 要靠它解析调用栈；
echo   · 打包后不要再重新编译，否则 PDB 与安装包里的 exe 就对不上了。
echo.
echo 按任意键关闭窗口...
pause >nul
endlocal
exit /b 0
