<#
.SYNOPSIS
  从源码编译 projectM SDL 前端（可选方案）

.DESCRIPTION
  如果预编译的 projectMSDL 版本太旧，或需要特定版本的 libprojectM，
  可以使用此脚本从 GitHub 源码编译。

  前置条件：
    - Git
    - CMake >= 3.22
    - Visual Studio 2022 (含 C++ 桌面开发工作负载)
    - vcpkg (用于自动管理依赖: SDL2, POCO, Freetype)

  用法:
    .\build_from_source.ps1

  编译产物位置:
    build\Release\projectMSDL.exe

.NOTES
  依赖: vcpkg 会自动下载 SDL2, POCO, Freetype
  首次编译约需 15-30 分钟（主要耗时在 vcpkg 下载编译依赖）
#>

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Build projectM SDL from Source" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# --- 检查前置条件 ---
Write-Host "[0/5] Checking prerequisites..." -ForegroundColor Yellow

$gitOk = $null -ne (Get-Command git -ErrorAction SilentlyContinue)
$cmakeOk = $null -ne (Get-Command cmake -ErrorAction SilentlyContinue)
if (-not $gitOk)   { Write-Host "  ERROR: git not found" -ForegroundColor Red; exit 1 }
if (-not $cmakeOk) { Write-Host "  ERROR: cmake not found" -ForegroundColor Red; exit 1 }
Write-Host "  git:    $(git --version)" -ForegroundColor Gray
Write-Host "  cmake:  $(cmake --version | Select-Object -First 1)" -ForegroundColor Gray

# --- 克隆 libprojectM ---
$ProjectMDir = Join-Path $ScriptDir "src\projectm"
Write-Host ""
Write-Host "[1/5] Cloning libprojectM..." -ForegroundColor Yellow
if (-not (Test-Path $ProjectMDir)) {
    git clone --depth 1 --branch master https://github.com/projectM-visualizer/projectm.git $ProjectMDir
    Push-Location $ProjectMDir
    git submodule update --init --recursive
    Pop-Location
    Write-Host "  Cloned to: $ProjectMDir" -ForegroundColor Green
} else {
    Write-Host "  Already exists, skipping." -ForegroundColor Gray
    Push-Location $ProjectMDir
    git pull
    git submodule update --init --recursive
    Pop-Location
}

# --- 克隆 frontend-sdl-cpp ---
$SdlDir = Join-Path $ScriptDir "src\frontend-sdl-cpp"
Write-Host ""
Write-Host "[2/5] Cloning frontend-sdl-cpp..." -ForegroundColor Yellow
if (-not (Test-Path $SdlDir)) {
    git clone --depth 1 --branch master https://github.com/projectM-visualizer/frontend-sdl-cpp.git $SdlDir
    Push-Location $SdlDir
    git submodule update --init --recursive
    Pop-Location
    Write-Host "  Cloned to: $SdlDir" -ForegroundColor Green
} else {
    Write-Host "  Already exists, skipping." -ForegroundColor Gray
    Push-Location $SdlDir
    git pull
    git submodule update --init --recursive
    Pop-Location
}

# --- vcpkg 依赖安装 ---
Write-Host ""
Write-Host "[3/5] Setting up vcpkg dependencies..." -ForegroundColor Yellow

# 查找 vcpkg: 优先 VCPKG_ROOT 环境变量，否则找常见位置
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
    $candidates = @(
        "$env:USERPROFILE\vcpkg",
        "C:\vcpkg",
        "C:\dev\vcpkg"
    )
    foreach ($c in $candidates) {
        if (Test-Path "$c\vcpkg.exe") { $vcpkgRoot = $c; break }
    }
}

if (-not $vcpkgRoot) {
    Write-Host "  vcpkg not found. Cloning..." -ForegroundColor Yellow
    $vcpkgRoot = Join-Path $ScriptDir "vcpkg"
    if (-not (Test-Path $vcpkgRoot)) {
        git clone --depth 1 https://github.com/Microsoft/vcpkg.git $vcpkgRoot
    }
    Push-Location $vcpkgRoot
    .\bootstrap-vcpkg.bat
    Pop-Location
}
Write-Host "  vcpkg at: $vcpkgRoot" -ForegroundColor Gray

# 安装依赖（使用 vcpkg.json manifest 模式，projectM 和 frontend-sdl-cpp 都有自己的 vcpkg.json）
$env:VCPKG_ROOT = $vcpkgRoot

# --- 构建 libprojectM ---
Write-Host ""
Write-Host "[4/5] Building libprojectM..." -ForegroundColor Yellow
$PmBuildDir = Join-Path $ProjectMDir "cmake-build-release"
if (-not (Test-Path $PmBuildDir)) { New-Item -ItemType Directory -Path $PmBuildDir | Out-Null }

Push-Location $PmBuildDir
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$vcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    -DCMAKE_INSTALL_PREFIX="$ScriptDir\install"
cmake --build . --config Release --parallel
cmake --install . --config Release
Pop-Location
Write-Host "  libprojectM built and installed." -ForegroundColor Green

# --- 构建 frontend-sdl-cpp ---
Write-Host ""
Write-Host "[5/5] Building projectMSDL..." -ForegroundColor Yellow
$SdlBuildDir = Join-Path $SdlDir "cmake-build-release"
if (-not (Test-Path $SdlBuildDir)) { New-Item -ItemType Directory -Path $SdlBuildDir | Out-Null }

Push-Location $SdlBuildDir
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$vcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    -DCMAKE_PREFIX_PATH="$ScriptDir\install"
cmake --build . --config Release --parallel
Pop-Location

# 找到生成的 exe
$BuiltExe = Get-ChildItem -Path $SdlBuildDir -Recurse -Filter "projectMSDL.exe" | Select-Object -First 1
if ($BuiltExe) {
    Write-Host "  Build successful!" -ForegroundColor Green
    Write-Host "  Executable: $($BuiltExe.FullName)" -ForegroundColor Green

    # 复制 DLL 到 exe 目录
    $ExeDir = $BuiltExe.DirectoryName
    Copy-Item "$ScriptDir\install\bin\*.dll" $ExeDir -Force -ErrorAction SilentlyContinue
    Write-Host "  DLLs copied." -ForegroundColor Gray
} else {
    Write-Host "  WARNING: projectMSDL.exe not found after build." -ForegroundColor Yellow
    Write-Host "  Check build output in: $SdlBuildDir" -ForegroundColor Yellow
}
