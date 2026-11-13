@echo off
setlocal EnableDelayedExpansion

rem 从本脚本位置反推项目根目录：<root>\docs\skills\project-maintenance-guard\scripts\
for %%i in ("%~dp0..\..\..\..") do set "PROJECT_ROOT=%%~fi"
set "SCRIPT_DIR=%~dp0"

cd /d "%PROJECT_ROOT%"
if not exist "build\skill-verify" mkdir "build\skill-verify"
if exist "build\skill-verify\_build_log.txt" del "build\skill-verify\_build_log.txt"

start "Y2K-Skill-IncBuild" /MIN cmd /c "cd /d ""%PROJECT_ROOT%"" && ""%SCRIPT_DIR%_incremental_build.bat"""

echo [SKILL-BUILD] Background incremental build launched at %PROJECT_ROOT%
echo [SKILL-BUILD] Poll build\skill-verify\_build_log.txt for result.
endlocal
