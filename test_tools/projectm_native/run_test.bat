@echo off
REM ===================================================
REM  Y2Kmeter - projectM Native Test Tool Launcher
REM
REM  启动原生 projectM SDL 可视化工具，
REM  用于对比测试 Y2Kmeter 内置 Milkdrop 引擎的渲染效果。
REM
REM  首次使用请先运行: download_setup.ps1
REM ===================================================

setlocal

set "SCRIPT_DIR=%~dp0"

REM 自动查找 projectMSDL 目录（适配解压后的任意子目录名）
set "EXE_PATH="
for /d %%d in ("%SCRIPT_DIR%projectMSDL\*") do (
    for /r "%%d" %%f in (*.exe) do (
        set "EXE_PATH=%%f"
        goto :found
    )
)
:found

if "%EXE_PATH%"=="" (
    echo [ERROR] projectMSDL executable not found.
    echo.
    echo Please run download_setup.ps1 first to download the tool.
    echo Or manually download from:
    echo   https://github.com/projectM-visualizer/frontend-sdl-cpp/releases/tag/2.0.0-pre1
    echo   https://codav.itch.io/projectm
    echo.
    pause
    exit /b 1
)

echo ========================================
echo  Y2Kmeter - projectM Native Test Tool
echo ========================================
echo.
echo App:    %EXE_PATH%
echo.
echo Keyboard Shortcuts:
echo   ESC         Toggle settings UI
echo   N / P       Next / Previous preset
echo   R           Random preset
echo   SPACE       Lock current preset
echo   Y           Toggle shuffle mode
echo   Ctrl+F      Toggle fullscreen
echo   Ctrl+Q      Quit
echo   F1          Help / all shortcuts
echo   MouseWheel  Next / Previous preset
echo.
echo Audio: The tool captures system audio via loopback.
echo         Play any audio on your PC to see visualizations.
echo.
echo First-time setup:
echo   1. Press ESC to open the settings UI
echo   2. In Preset Paths, add your Y2Kmeter presets:
echo      %SCRIPT_DIR%..\..\assets\milkdrop_presets
echo.

REM 从 exe 所在目录启动，确保 projectM 能找到相对路径的 DLL
for %%F in ("%EXE_PATH%") do cd /d "%%~dpF"
start "" "%EXE_PATH%"

endlocal
