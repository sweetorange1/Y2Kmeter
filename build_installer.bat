@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "ISS_FILE=%SCRIPT_DIR%Y2Kmeter_installer.iss"

if not exist "%ISS_FILE%" (
  echo [ERROR] 未找到安装脚本: "%ISS_FILE%"
  echo.
  echo 按任意键关闭窗口...
  pause >nul
  exit /b 1
)

set "ISCC_PATH=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC_PATH%" set "ISCC_PATH=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC_PATH%" set "ISCC_PATH=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"

if not exist "%ISCC_PATH%" (
  echo [ERROR] 未找到 ISCC.exe，请先安装 Inno Setup 6。
  echo 你可以运行: winget install --id JRSoftware.InnoSetup -e
  echo.
  echo 按任意键关闭窗口...
  pause >nul
  exit /b 1
)

echo [INFO] 使用编译器: "%ISCC_PATH%"
echo [INFO] 开始打包: "%ISS_FILE%"
"%ISCC_PATH%" "%ISS_FILE%"

if errorlevel 1 (
  echo [ERROR] 打包失败，请查看上方日志。
  echo.
  echo 按任意键关闭窗口...
  pause >nul
  exit /b 1
)

echo [OK] 打包完成，输出目录通常为: "%SCRIPT_DIR%dist"
echo.
echo 按任意键关闭窗口...
pause >nul
endlocal
exit /b 0