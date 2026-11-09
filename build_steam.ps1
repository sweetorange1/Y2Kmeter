# ==========================================================
#  build_steam.ps1  -  Y2Kmeter Steam build & packaging
#
#  Produces:  dist-steam\content\  (the only folder uploaded to the depot)
#             dist-steam\manifest.json
#
#  This script NEVER touches:
#    - cmake-build-release-visual-studio\   (CLion / installer build tree)
#    - dist\                                (Inno Setup installer output)
#    - Y2Kmeter_installer.iss
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File .\build_steam.ps1
#    powershell ... -File .\build_steam.ps1 -SkipBuild             # repack only
#    powershell ... -File .\build_steam.ps1 -Preset steam-ninja    # Ninja tree
#    powershell ... -File .\build_steam.ps1 -SteamSdk "I:\Steam相关\SteamSdk"
#
#  Presets:
#    steam        ->  Visual Studio generator,  build-steam\        (default)
#    steam-ninja  ->  Ninja + MSVC (cl),        build-steam-ninja\  (CLion 常用)
#
#  Exit code 0 = success, 1 = failure (see the [FAIL] line for the reason).
# ==========================================================

[CmdletBinding()]
param(
    [string] $Config   = 'Release',
    [string] $Preset   = 'steam',
    [string] $BuildDir = '',
    [string] $SteamSdk = $env:Y2KM_STEAM_SDK,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $script:RepoRoot

# 构建目录：不显式指定时按 preset 推导（steam -> build-steam，steam-ninja -> build-steam-ninja）
if (-not $BuildDir)
{
    $BuildDir = if ($Preset -eq 'steam') { 'build-steam' } else { 'build-' + $Preset }
}
if ([System.IO.Path]::IsPathRooted($BuildDir))
{
    $script:BuildDir = $BuildDir
}
else
{
    $script:BuildDir = Join-Path $script:RepoRoot $BuildDir
}

$script:BuildPreset = $Preset + '-release'
$script:DistDir  = Join-Path $script:RepoRoot 'dist-steam'
$script:Content  = Join-Path $script:DistDir  'content'
$script:ExeDir   = Join-Path $script:BuildDir ('Y2Kmeter_artefacts\{0}\Standalone' -f $Config)
$script:ExePath  = Join-Path $script:ExeDir  'Y2Kmeter.exe'
$script:TempRoot = Join-Path $env:TEMP ('y2km_steam_' + [guid]::NewGuid().ToString('N'))

function Write-Step([string] $Message) { Write-Host "[steam] $Message" -ForegroundColor Cyan }

function Fail([string] $Message)
{
    Write-Host "[FAIL] $Message" -ForegroundColor Red
    throw $Message
}

function Get-Sha256([string] $Path)
{
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-ProductVersion
{
    $cmake = Join-Path $script:RepoRoot 'CMakeLists.txt'
    $text  = Get-Content -LiteralPath $cmake -Raw -Encoding UTF8
    $m = [regex]::Match($text, 'project\s*\(\s*Y2Kmeter\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $m.Success) { Fail 'Cannot parse project(Y2Kmeter VERSION x.y.z) from CMakeLists.txt' }
    return $m.Groups[1].Value
}

# Copies assets\<InnerName> (or the matching zip) into content\assets\<InnerName>.
function Install-AssetTree
{
    param(
        [string] $SourceDir,
        [string] $ZipPath,
        [string] $InnerName,
        [string] $DestRoot
    )

    $dest = Join-Path $DestRoot $InnerName
    New-Item -ItemType Directory -Force -Path $dest | Out-Null

    if ((Test-Path -LiteralPath $SourceDir) -and
        ((Get-ChildItem -LiteralPath $SourceDir -Force | Select-Object -First 1) -ne $null))
    {
        Write-Step ("copy assets\{0} (from working tree)" -f $InnerName)
        Copy-Item -Path (Join-Path $SourceDir '*') -Destination $dest -Recurse -Force
        return
    }

    if (-not (Test-Path -LiteralPath $ZipPath))
    {
        Fail ("Assets missing: neither '{0}' nor '{1}' exists" -f $SourceDir, $ZipPath)
    }

    Write-Step ("extract {0}" -f (Split-Path -Leaf $ZipPath))
    $stage = Join-Path $script:TempRoot ([guid]::NewGuid().ToString('N'))
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $stage -Force

    # Zips may keep a top-level "<InnerName>\" wrapper or ship loose files.
    $src = $stage
    foreach ($candidate in @((Join-Path $stage $InnerName), (Join-Path $stage ('assets\' + $InnerName))))
    {
        if (Test-Path -LiteralPath $candidate) { $src = $candidate; break }
    }

    Copy-Item -Path (Join-Path $src '*') -Destination $dest -Recurse -Force
    Remove-Item -LiteralPath $stage -Recurse -Force
}

try
{
    # ------------------------------------------------------
    # 0. Prerequisites
    # ------------------------------------------------------
    $appIdFile = Join-Path $script:RepoRoot 'cmake\steam_appid.txt'
    if (-not (Test-Path -LiteralPath $appIdFile)) { Fail 'cmake\steam_appid.txt is missing' }

    if (-not $SkipBuild)
    {
        # --------------------------------------------------
        # 1. Configure (isolated tree, Y2K_STEAM_BUILD=ON)
        # --------------------------------------------------
        $configureArgs = @('--preset', $Preset)
        if ($SteamSdk) { $configureArgs += ('-DY2KM_STEAM_SDK=' + $SteamSdk) }

        Write-Step ('cmake --preset ' + $Preset)
        & cmake @configureArgs
        if ($LASTEXITCODE -ne 0) { Fail 'cmake configure failed' }

        # --------------------------------------------------
        # 2. Build (Standalone only - VST3 is intentionally excluded)
        # --------------------------------------------------
        Write-Step ('cmake --build --preset ' + $script:BuildPreset)
        & cmake --build --preset $script:BuildPreset
        if ($LASTEXITCODE -ne 0) { Fail 'cmake build failed' }
    }

    # 若指定的构建目录里没有 exe（例如在 CLion 里用另一条 preset 构建过），
    #   退而查找另一个独立的 Steam 构建树，避免让用户手工指定路径。
    if (-not (Test-Path -LiteralPath $script:ExePath))
    {
        foreach ($alt in @('build-steam', 'build-steam-ninja'))
        {
            $altDir = Join-Path $script:RepoRoot ('{0}\Y2Kmeter_artefacts\{1}\Standalone' -f $alt, $Config)
            $altExe = Join-Path $altDir 'Y2Kmeter.exe'
            if (Test-Path -LiteralPath $altExe)
            {
                Write-Step ('exe not found in {0}, using {1} instead' -f $script:BuildDir, $alt)
                $script:BuildDir = Join-Path $script:RepoRoot $alt
                $script:ExeDir   = $altDir
                $script:ExePath  = $altExe
                break
            }
        }
    }

    if (-not (Test-Path -LiteralPath $script:ExePath))
    {
        Fail ("Steam exe not found: {0}`nRun a steam preset build first (CLion: profile with -DY2K_STEAM_BUILD=ON)." -f $script:ExePath)
    }

    $builtDll = Join-Path $script:ExeDir 'steam_api64.dll'
    if (-not (Test-Path -LiteralPath $builtDll))
    {
        Fail 'steam_api64.dll was not copied next to the exe (check the Steam POST_BUILD step)'
    }

    # --------------------------------------------------
    # 3. Rebuild dist-steam\content
    # --------------------------------------------------
    if (Test-Path -LiteralPath $script:Content) { Remove-Item -LiteralPath $script:Content -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $script:Content | Out-Null

    Write-Step 'copy runtime files'
    $runtimeFiles = @('Y2Kmeter.exe', 'steam_api64.dll', 'glew32.dll', 'projectM-4.dll')
    foreach ($name in $runtimeFiles)
    {
        $src = Join-Path $script:ExeDir $name
        if (-not (Test-Path -LiteralPath $src))
        {
            Fail ("Runtime file missing in build output: {0}" -f $src)
        }
        Copy-Item -LiteralPath $src -Destination (Join-Path $script:Content $name) -Force
    }

    # --------------------------------------------------
    # 4. Assets (same layout as the existing depot)
    # --------------------------------------------------
    $assetsRoot = Join-Path $script:Content 'assets'
    New-Item -ItemType Directory -Force -Path $assetsRoot | Out-Null

    $assetSrc = Join-Path $script:RepoRoot 'assets'
    Install-AssetTree -SourceDir (Join-Path $assetSrc 'VirtuPet') `
                      -ZipPath  (Join-Path $assetSrc 'virtupet_assets.zip') `
                      -InnerName 'VirtuPet' `
                      -DestRoot $assetsRoot

    Install-AssetTree -SourceDir (Join-Path $assetSrc 'milkdrop_presets') `
                      -ZipPath  (Join-Path $assetSrc 'milkdrop_presets.zip') `
                      -InnerName 'milkdrop_presets' `
                      -DestRoot $assetsRoot

    Install-AssetTree -SourceDir (Join-Path $assetSrc 'milkdrop_textures') `
                      -ZipPath  (Join-Path $assetSrc 'milkdrop_textures.zip') `
                      -InnerName 'milkdrop_textures' `
                      -DestRoot $assetsRoot

    # --------------------------------------------------
    # 5. Self-checks (fail the build, never ship a broken depot)
    # --------------------------------------------------
    Write-Step 'self-check: forbidden files'
    $forbiddenExt = @('.pdb', '.ilk', '.exp', '.lib', '.log', '.tmp')
    $forbidden = Get-ChildItem -LiteralPath $script:Content -Recurse -File | Where-Object {
        ($_.Name -ieq 'steam_appid.txt') -or ($forbiddenExt -contains $_.Extension.ToLowerInvariant())
    }
    if ($forbidden)
    {
        $list = ($forbidden | Select-Object -First 10 | ForEach-Object { $_.FullName }) -join "`n  "
        Fail ("Forbidden files under content:\n  {0}" -f $list)
    }

    Write-Step 'self-check: exe really links Steamworks'
    $bytes = [System.IO.File]::ReadAllBytes($script:ExePath)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    if ($ascii.IndexOf('steam_api64.dll', [System.StringComparison]::Ordinal) -lt 0)
    {
        Fail 'Y2Kmeter.exe does not reference steam_api64.dll - Y2K_STEAM was probably not enabled'
    }
    $bytes = $null
    $ascii = $null

    # --------------------------------------------------
    # 6. manifest.json
    # --------------------------------------------------
    Write-Step 'write manifest.json'

    $files = Get-ChildItem -LiteralPath $script:Content -Recurse -File
    $totalBytes = 0
    foreach ($f in $files) { $totalBytes += $f.Length }

    $exeItem = Get-Item -LiteralPath $script:ExePath
    $dllItem = Get-Item -LiteralPath $builtDll

    $now = Get-Date
    $builtAt = $now.ToString('yyyy-MM-dd HH:mm:ss') + ' ' + $now.ToString('zzz').Replace(':', '')

    $manifest = [ordered]@{
        product_version = Get-ProductVersion
        steam_enabled   = $true
        built_at        = $builtAt
        config          = $Config
        cmake_preset    = $Preset
        exe             = [ordered]@{
            name   = 'Y2Kmeter.exe'
            bytes  = $exeItem.Length
            sha256 = Get-Sha256 $script:ExePath
        }
        steam_api_dll   = [ordered]@{
            name   = 'steam_api64.dll'
            bytes  = $dllItem.Length
            sha256 = Get-Sha256 $builtDll
        }
        file_count      = $files.Count
        total_bytes     = $totalBytes
        notes           = 'Steam build: achievements + Auto-Cloud. steam_appid.txt excluded.'
    }

    $manifestPath = Join-Path $script:DistDir 'manifest.json'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    # --------------------------------------------------
    # 7. Summary
    # --------------------------------------------------
    Write-Host ''
    Write-Host '[steam] DONE' -ForegroundColor Green
    Write-Host ("  exe        : {0} ({1:N0} bytes)" -f $manifest.exe.name, $manifest.exe.bytes)
    Write-Host ("  exe sha256 : {0}" -f $manifest.exe.sha256)
    Write-Host ("  content    : {0} files / {1:N0} bytes" -f $manifest.file_count, $manifest.total_bytes)
    Write-Host ("  upload root: {0}" -f $script:Content)
    exit 0
}
catch
{
    Write-Host ("[FAIL] " + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally
{
    if (Test-Path -LiteralPath $script:TempRoot)
    {
        Remove-Item -LiteralPath $script:TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
