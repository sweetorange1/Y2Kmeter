# ==========================================================
# Y2Kmeter 本地全量清理脚本
#   删除：VST3 插件、Standalone 安装目录、AppData 共享目录、
#         遥测文件、快捷方式、注册表项、卸载注册表项。
#   需管理员权限（删除 C:\Program Files 下文件 + HKLM 注册表）。
# ==========================================================
$ErrorActionPreference = 'SilentlyContinue'

# --- 管理员权限检查 ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host '[WARN] 非管理员运行，系统目录/注册表可能无法删除。建议用管理员 PowerShell 重跑。' -ForegroundColor Yellow
}

$deleted = 0
function TryRemove-Path([string]$Path, [switch]$Recurse) {
    if (Test-Path -LiteralPath $Path) {
        if ($Recurse) { Remove-Item -LiteralPath $Path -Recurse -Force }
        else          { Remove-Item -LiteralPath $Path -Force }
        if (-not (Test-Path -LiteralPath $Path)) {
            Write-Host "[DEL] $Path" -ForegroundColor Green
            $script:deleted++
        } else {
            Write-Host "[FAIL] 无法删除（可能被占用）: $Path" -ForegroundColor Red
        }
    }
}

Write-Host '=== 1. VST3 插件（系统默认目录 + installer 目录）==='
foreach ($p in @(
    'C:\Program Files\Common Files\VST3\Y2Kmeter.vst3',
    'C:\Program Files\Common Files\VST3\Y2Kmeter_milkdrop.vst3',
    'C:\Program Files\Common Files\VST3\iisaacbeats.cn\Y2Kmeter.vst3',
    'C:\Program Files\Common Files\VST3\iisaacbeats.cn\Y2Kmeter_milkdrop.vst3',
    'C:\Program Files\Common Files\VST3\iisaacbeats.cn'
)) { TryRemove-Path $p -Recurse }

Write-Host '=== 2. Standalone 安装目录 ==='
TryRemove-Path 'C:\Program Files\iisaacbeats.cn\Y2Kmeter' -Recurse
# 若父目录已空，一并删除
$parent = 'C:\Program Files\iisaacbeats.cn'
if ((Test-Path -LiteralPath $parent) -and -not (Get-ChildItem -LiteralPath $parent -Force)) {
    Remove-Item -LiteralPath $parent -Force
    Write-Host "[DEL] $parent (空目录)" -ForegroundColor Green
}

Write-Host '=== 3. AppData 共享目录（预设/纹理）==='
TryRemove-Path "$env:APPDATA\Y2Kmeter" -Recurse

Write-Host '=== 4. 遥测 client_id 文件 ==='
TryRemove-Path "$env:APPDATA\Y2Kmeter.telemetry"

Write-Host '=== 5. 快捷方式 ==='
foreach ($p in @(
    "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Y2Kmeter.lnk",
    "$env:USERPROFILE\Desktop\Y2Kmeter.lnk"
)) { TryRemove-Path $p }

Write-Host '=== 6. 注册表：软件设置 ==='
foreach ($rk in @(
    'HKCU:\Software\iisaacbeats\Y2Kmeter',
    'HKLM:\Software\iisaacbeats\Y2Kmeter'
)) {
    if (Test-Path $rk) {
        Remove-Item -Path $rk -Recurse -Force
        if (-not (Test-Path $rk)) { Write-Host "[DEL] $rk" -ForegroundColor Green; $deleted++ }
        else { Write-Host "[FAIL] 无法删除注册表键: $rk" -ForegroundColor Red }
    }
}
# 空父键清理
foreach ($rp in @('HKCU:\Software\iisaacbeats', 'HKLM:\Software\iisaacbeats')) {
    if ((Test-Path $rp) -and -not (Get-ChildItem -Path $rp)) {
        Remove-Item -Path $rp -Force
        Write-Host "[DEL] $rp (空键)" -ForegroundColor Green
    }
}

Write-Host '=== 7. 卸载注册表项 ==='
$appId = 'C9B9FCA5-9D58-4F34-A856-214E5F972B25'
foreach ($unKey in @(
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{$appId}_is1",
    "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{$appId}_is1",
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{$appId}_is1"
)) {
    if (Test-Path $unKey) {
        Remove-Item -Path $unKey -Recurse -Force
        if (-not (Test-Path $unKey)) { Write-Host "[DEL] $unKey" -ForegroundColor Green; $deleted++ }
        else { Write-Host "[FAIL] 无法删除: $unKey" -ForegroundColor Red }
    }
}

Write-Host ''
Write-Host "=== 清理完成，共删除 $deleted 项 ==="
