@echo off

REM ============================================================
REM  Y2Kmeter milkdrop3-dev-guard Skill: BACKGROUND full build launcher
REM ------------------------------------------------------------
REM  Launches build_skill_verify.bat in a separate minimized
REM  cmd window and returns IMMEDIATELY. The AI's terminal tool
REM  won't block — AI can then poll build\skill-verify\_build_log.txt
REM  for the [PASS]/[FAIL] result.
REM
REM  Usage (from I:/Y2KMeter/ Git Bash):
REM    cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_bg_build.bat"
REM  Usage (from cmd.exe / PowerShell):
REM    cmd /c docs\skills\milkdrop3-dev-guard\scripts\_bg_build.bat
REM
REM  The actual build runs in a minimized window that auto-closes
REM  on completion. Result is written to build\skill-verify\_build_log.txt.
REM ============================================================

set "PROJECT_ROOT=I:\Y2KMeter"
cd /d "%PROJECT_ROOT%"

REM Clear previous log so polling doesn't see stale results
if exist "build\skill-verify\_build_log.txt" del "build\skill-verify\_build_log.txt"

REM Launch the real build script in a new minimized cmd window.
REM Using absolute paths avoids %~dp0 / %CD% issues when called from Git Bash.
start "Y2K-Skill-Build" /MIN cmd /c "cd /d I:\Y2KMeter && I:\Y2KMeter\docs\skills\milkdrop3-dev-guard\scripts\build_skill_verify.bat"

echo [SKILL-BUILD] Background build launched. Poll build\skill-verify\_build_log.txt for result.
