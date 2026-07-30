<#
.SYNOPSIS
  下载并设置原生 projectM SDL 可视化测试工具

.DESCRIPTION
  从 GitHub Releases 下载最新的 projectMSDL 预编译包，
  解压到当前目录，并将 Y2Kmeter 的预设软链接到工具中。
  用于对比测试 Y2Kmeter 内置的 Milkdrop 渲染与原生 projectM 引擎的差异。

.NOTES
  版本: 1.0
  来源: https://github.com/projectM-visualizer/frontend-sdl-cpp
#>

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Y2KMeterRoot = Resolve-Path "$ScriptDir\..\.."

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Y2Kmeter - projectM Native Test Tool" -ForegroundColor Cyan
Write-Host " Setup Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# --- 下载源配置 ---
# 从官方 frontend-sdl-cpp 仓库的 GitHub Release 下载
# 这是 Y2Kmeter third_party/projectm 中 DLL 的同一来源
#   Tag: 2.0.0-pre1
#   URL: https://github.com/projectM-visualizer/frontend-sdl-cpp/releases/tag/2.0.0-pre1

$DownloadUrl = "https://github.com/projectM-visualizer/frontend-sdl-cpp/releases/download/2.0.0-pre1/projectMSDL-2.0.0-win64.zip"
$ZipFile = Join-Path $ScriptDir "projectMSDL.zip"
$ExtractDir = Join-Path $ScriptDir "projectMSDL"

# --- 步骤 1: 下载 ---
Write-Host "[1/4] Downloading projectMSDL..." -ForegroundColor Yellow
Write-Host "  URL: $DownloadUrl" -ForegroundColor Gray

if (Test-Path $ZipFile) {
    Write-Host "  ZIP already exists, skipping download." -ForegroundColor Gray
} else {
    try {
        # 如果系统使用 TLS 1.2
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipFile -UseBasicParsing
        Write-Host "  Download complete: $ZipFile" -ForegroundColor Green
    } catch {
        Write-Host "  ERROR: Download failed!" -ForegroundColor Red
        Write-Host "  $_" -ForegroundColor Red
        Write-Host ""
        Write-Host "  Manual download options:" -ForegroundColor Yellow
        Write-Host "  1. GitHub: https://github.com/kblaschke/frontend-sdl2/releases/tag/2.0-windows-pre3" -ForegroundColor White
        Write-Host "  2. itch.io (newer): https://codav.itch.io/projectm" -ForegroundColor White
        Write-Host ""
        Write-Host "  After downloading, place the ZIP as: $ZipFile" -ForegroundColor White
        Write-Host "  Then re-run this script." -ForegroundColor White
        exit 1
    }
}

# --- 步骤 2: 解压（逐文件提取，跳过预设目录以避免 Windows 长路径限制） ---
Write-Host ""
Write-Host "[2/4] Extracting core files (skipping bundled presets)..." -ForegroundColor Yellow

if (Test-Path $ExtractDir) {
    Write-Host "  Removing old extraction..." -ForegroundColor Gray
    Remove-Item -Recurse -Force $ExtractDir -ErrorAction SilentlyContinue
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ZipFile)
$extractedCount = 0
$skippedCount = 0

foreach ($entry in $zip.Entries) {
    $entryPath = $entry.FullName

    # 跳过预设文件 —— 路径过长无法解压，我们用 Y2Kmeter 预设替代
    if ($entryPath -like "*presets-cream-of-the-crop*") {
        $skippedCount++
        continue
    }

    $targetFile = Join-Path $ExtractDir $entryPath
    $targetDir = Split-Path $targetFile -Parent

    try {
        if (-not (Test-Path $targetDir)) {
            New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
        }
        if ($entryPath.EndsWith('/') -or $entryPath.EndsWith('\')) {
            continue
        }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $targetFile, $true)
        $extractedCount++
    } catch {
        Write-Host "  Skipped (path too long): $entryPath" -ForegroundColor DarkGray
        $skippedCount++
    }
}

$zip.Dispose()
Write-Host "  Extracted: $extractedCount core files, Skipped: $skippedCount presets" -ForegroundColor Green

# 找到实际的子目录（ZIP 里通常有一层包装目录）
$SubDir = Get-ChildItem -Directory -Path $ExtractDir | Select-Object -First 1
if ($SubDir) {
    $AppDir = $SubDir.FullName
    Write-Host "  Application directory: $AppDir" -ForegroundColor Gray
} else {
    $AppDir = $ExtractDir
}

# --- 步骤 3: 链接预设 ---
Write-Host ""
Write-Host "[3/4] Linking Y2Kmeter presets..." -ForegroundColor Yellow

$PresetSource = Join-Path $Y2KMeterRoot "assets\milkdrop_presets"
$PresetDest = Join-Path $AppDir "presets\y2kmeter_presets"

if (Test-Path $PresetSource) {
    $presetCount = (Get-ChildItem -Path $PresetSource -Filter "*.milk" -Recurse).Count
    Write-Host "  Source presets: $PresetSource ($presetCount .milk files)" -ForegroundColor Gray

    # 使用目录联接 (Junction) —— 比符号链接更兼容，无需管理员权限
    if (Test-Path $PresetDest) {
        try {
            cmd /c "rmdir `"$PresetDest`" 2>nul"
        } catch { }
    }

    try {
        New-Item -ItemType Junction -Path $PresetDest -Target $PresetSource -Force | Out-Null
        Write-Host "  Junction created: $PresetDest -> $PresetSource" -ForegroundColor Green
    } catch {
        Write-Host "  Junction failed, falling back to copy..." -ForegroundColor Yellow
        Copy-Item -Path $PresetSource -Destination $PresetDest -Recurse -Force
        Write-Host "  Copied presets to: $PresetDest" -ForegroundColor Green
    }
} else {
    Write-Host "  WARNING: Preset source not found at $PresetSource" -ForegroundColor Yellow
    Write-Host "  You can manually copy .milk files to: $PresetDest" -ForegroundColor Yellow
}

# --- 步骤 4: 创建快速启动脚本 ---
Write-Host ""
Write-Host "[4/4] Creating launch scripts..." -ForegroundColor Yellow

# 找到 exe
$ExePath = Get-ChildItem -Path $AppDir -Filter "*.exe" -Recurse | Select-Object -First 1
if (-not $ExePath) {
    Write-Host "  ERROR: No .exe found in $AppDir" -ForegroundColor Red
    exit 1
}

# 批处理启动脚本
$BatContent = @"
@echo off
cd /d "%~dp0$($AppDir.Substring($ScriptDir.Length))"
echo ========================================
echo  Y2Kmeter - projectM Native Test Tool
echo ========================================
echo.
echo Keyboard shortcuts:
echo   ESC       - Toggle UI
echo   N / P     - Next / Previous preset
echo   R         - Random preset
echo   SPACE     - Lock current preset
echo   Y         - Toggle shuffle
echo   Ctrl+F    - Toggle fullscreen
echo   Ctrl+Q    - Quit
echo   F1        - Help
echo.
echo Your Y2Kmeter presets are in: presets\y2kmeter_presets\
echo Add this path in Settings ^> General ^> Preset Paths
echo.
start "" "$($ExePath.Name)"
"@

$BatPath = Join-Path $ScriptDir "run_test.bat"
$BatContent | Out-File -FilePath $BatPath -Encoding ASCII
Write-Host "  Created: run_test.bat" -ForegroundColor Green

# PowerShell 启动脚本
$PsContent = @"
# Y2Kmeter - projectM Native Test Tool Launcher
Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Y2Kmeter - projectM Native Test Tool" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Keyboard shortcuts:" -ForegroundColor Yellow
Write-Host "  ESC       - Toggle UI"
Write-Host "  N / P     - Next / Previous preset"
Write-Host "  R         - Random preset"
Write-Host "  SPACE     - Lock current preset"
Write-Host "  Y         - Toggle shuffle"
Write-Host "  Ctrl+F    - Toggle fullscreen"
Write-Host "  Ctrl+Q    - Quit"
Write-Host "  F1        - Help"
Write-Host ""
Write-Host "Your Y2Kmeter presets are in: presets\y2kmeter_presets\" -ForegroundColor Green
Write-Host "Add this path in Settings > General > Preset Paths" -ForegroundColor Green
Write-Host ""

Set-Location "$($AppDir -replace '\\','\\')"
Start-Process "$($ExePath.Name)"
"@

$PsPath = Join-Path $ScriptDir "run_test.ps1"
$PsContent | Out-File -FilePath $PsPath -Encoding UTF8
Write-Host "  Created: run_test.ps1" -ForegroundColor Green

# --- 完成 ---
Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host " Setup Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "To launch the test tool:" -ForegroundColor White
Write-Host "  双击:   run_test.bat" -ForegroundColor Cyan
Write-Host "  或运行: .\run_test.ps1" -ForegroundColor Cyan
Write-Host ""
Write-Host "First-time setup:" -ForegroundColor Yellow
Write-Host "  1. Launch projectMSDL" -ForegroundColor White
Write-Host "  2. Press ESC to open settings UI" -ForegroundColor White
Write-Host "  3. Go to Settings > General > Preset Paths" -ForegroundColor White
Write-Host "  4. Add: presets\y2kmeter_presets\" -ForegroundColor White
Write-Host "  5. The tool captures system audio automatically (loopback)" -ForegroundColor White
Write-Host "  6. Play any audio on your system to see the visualizations" -ForegroundColor White
Write-Host ""
