@echo off

set "PROJECT_ROOT=I:\Y2KMeter"
cd /d "%PROJECT_ROOT%"

if exist "build\skill-verify\_build_log.txt" del "build\skill-verify\_build_log.txt"

start "Y2K-Skill-Build" /MIN cmd /c "cd /d I:\Y2KMeter && I:\Y2KMeter\docs\skills\project-maintenance-guard\scripts\build_skill_verify.bat"

echo [SKILL-BUILD] Background build launched. Poll build\skill-verify\_build_log.txt for result.