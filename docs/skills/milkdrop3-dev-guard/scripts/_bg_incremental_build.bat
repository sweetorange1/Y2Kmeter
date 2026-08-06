@echo off

REM ============================================================
REM  Y2Kmeter milkdrop3-dev-guard Skill: BACKGROUND incremental build launcher
REM ------------------------------------------------------------
REM  Launches _incremental_build.bat in a separate minimized
REM  cmd window and returns IMMEDIATELY. AI polls the log file
REM  for the [PASS]/[FAIL] result.
REM
REM  Prerequisite: build\skill-verify\CMakeCache.txt must exist
REM  (i.e. build_skill_verify.bat was run at least once before).
REM
REM  Usage (from I:/Y2KMeter/ Git Bash):
REM    cmd //c "docs\skills\milkdrop3-dev-guard\scripts\_bg_incremental_build.bat"
REM  Usage (from cmd.exe / PowerShell):
REM    cmd /c docs\skills\milkdrop3-dev-guard\scripts\_bg_incremental_build.bat
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
start "Y2K-Skill-IncBuild" /MIN cmd /c "cd /d I:\Y2KMeter && I:\Y2KMeter\docs\skills\milkdrop3-dev-guard\scripts\_incremental_build.bat"

echo [SKILL-BUILD] Background incremental build launched. Poll build\skill-verify\_build_log.txt for result.
