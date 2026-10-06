# Deploy the workbench to %LOCALAPPDATA%\MiderHive and create a desktop shortcut.
# Survives repo/build directory cleanup.
#
# NOTE: for releases prefer the MSI (release/MiderHive-<ver>-x64.msi), which installs to the
# same directory and also registers an uninstaller. This script remains for a quick
# local deployment straight from a build tree.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -BuildDir build/msvc-release
#
# Source priority: an installed stage dir (_stage, produced by scripts/package.ps1) if present,
# otherwise <BuildDir>\src\gui\Release. Previously this path was hardcoded to build\full, which
# the documented build command (cmake -B build) never produces.
param(
    [string]$BuildDir = "build/full",
    [switch]$FromStage
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$src = $null
if ($FromStage -or (Test-Path (Join-Path $root "_stage\miderhive.exe"))) {
    $src = Join-Path $root "_stage"
} else {
    $src = Join-Path $root (Join-Path $BuildDir "src\gui\Release")
}
if (-not (Test-Path (Join-Path $src "miderhive.exe"))) {
    Write-Error "miderhive.exe not found in $src`n  build first: cmake --build $BuildDir --config Release`n  or install the stage dir: cmake --install $BuildDir --config Release --prefix _stage --component Runtime"
    exit 1
}
# /MIR 会把目标里"源里没有"的东西整体删掉（等同 /E + /PURGE）。构建树的 GUI
# 输出目录只有 miderhive.exe 三个文件——没有 platformd/agent-cli/miderhive-mcp、
# 没有 licenses、连 Qt6*.dll 都不在（windeployqt 只写安装前缀）。拿它 /MIR 一个
# 正常工作的部署目录，会把守护进程、CLI、许可乃至全部 Qt 运行库删光，已装环境
# 从此起不来。所以部署源必须**完整**，缺件就拒绝并给下一步，绝不 /MIR。
$required = @("miderhive.exe", "agent-cli.exe", "platformd.exe", "miderhive-mcp.exe",
              "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6Svg.dll",
              "licenses\THIRD-PARTY-NOTICES.md", "licenses\LICENSE")
$missing = @()
foreach ($f in $required) { if (-not (Test-Path (Join-Path $src $f))) { $missing += $f } }
if ($missing.Count -gt 0) {
    Write-Error "source dir is incomplete (would /MIR-delete these from the target): $($missing -join ', ')`n  install a complete stage first: cmake --install $BuildDir --config Release --prefix _stage --component Runtime`n  then re-run with -FromStage (or let the script pick up _stage automatically)"
    exit 1
}
Write-Host "SRC=$src"

$dest = Join-Path $env:LOCALAPPDATA "MiderHive"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
# /MIR keeps the deployed tree exactly in sync (removes stale Qt DLLs)
robocopy $src $dest /MIR /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed: $LASTEXITCODE"; exit 1 }

# 桌面快捷方式：MiderHive 命名 + 应用图标；并清理历史品牌的旧快捷方式
$desktop = [Environment]::GetFolderPath("Desktop")
$lnkPath = Join-Path $desktop "MiderHive.lnk"
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut($lnkPath)
$lnk.TargetPath = Join-Path $dest "miderhive.exe"
$lnk.WorkingDirectory = $dest
$lnk.IconLocation = (Join-Path $dest "miderhive.exe") + ",0"
$lnk.Description = "MiderHive - local-first collaboration hub for AI agents"
$lnk.Save()
foreach ($old in @("ZCode Platform.lnk", "ZCode 协作平台.lnk", "AgentHive.lnk")) {
    $oldPath = Join-Path $desktop $old
    if (Test-Path $oldPath) { Remove-Item $oldPath -Force }
}

# 清理历史品牌部署目录（仅含程序副本，不含用户数据；被占用时跳过留待下次）。
# 注意：%LOCALAPPDATA%\AgentHive 可能仍被旧品牌 MSI 安装占用/登记，这里不主动删除，
# 由 MSI 的 MajorUpgrade（同一 UpgradeCode）在升级时整体卸载。
$legacy = Join-Path $env:LOCALAPPDATA "ZCodePlatform"
if (Test-Path $legacy) {
    try {
        Remove-Item $legacy -Recurse -Force -ErrorAction Stop
        Write-Host "LEGACY_DIR_REMOVED=$legacy"
    } catch {
        Write-Host "LEGACY_DIR_BUSY=$legacy"
    }
}

Write-Host "DEPLOY_DIR=$dest"
Write-Host "LNK=$lnkPath"
