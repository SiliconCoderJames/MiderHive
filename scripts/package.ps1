<#
.SYNOPSIS
  Build the MiderHive Windows release artifacts: portable ZIP + MSI + SHA256SUMS.

.DESCRIPTION
  Single entry point for local packaging and CI. Steps:
    1. configure + build (skippable with -SkipBuild)
    2. cmake --install to a clean stage dir (this is where windeployqt puts the Qt runtime)
    3. assert the stage really is runnable (exe, Qt DLLs, platform plugin, MSVC runtime)
    4. generate the WiX file manifest from the stage dir
    5. build the MSI with WiX (pinned version, see -WixVersion) + ICE validation
    6. zip the stage dir as the portable build
    7. write SHA256SUMS.txt
    8. write latest.json (the in-app updater's manifest)

  Everything lands in release/ (overridable with -OutDir).

  NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads UTF-8-without-BOM as ANSI,
  which corrupts non-ASCII comments and can break parsing.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.0.0

.EXAMPLE
  # CI: version comes from the git tag
  powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version $env:GITHUB_REF_NAME
#>
param(
    [string]$Version = "1.0.0",
    [string]$BuildDir = "build/full",
    [string]$QtPrefix = "C:/Qt/6.8.3/msvc2022_64",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$Configuration = "Release",
    [string]$OutDir = "release",
    [string]$StageDir = "_stage",
    [string]$WixVersion = "6.0.2",
    # owner/repo used to build the download URLs inside latest.json (the in-app updater
    # consumes them); CI passes ${{ github.repository }} so forks stay correct.
    [string]$Repo = "SiliconCoderJames/MiderHive",
    # Tag name written into latest.json (tag field + download/notes URLs). Empty (default)
    # auto-follows the tag that actually exists in the repo for this version: GitHub release
    # download URLs are CASE-SENSITIVE for the tag part (v1.0.4 404s when the tag is V1.0.4),
    # so the manifest must reference the real spelling. Falls back to v<version>.
    [string]$TagName = "",
    [switch]$SkipBuild,
    [switch]$PerMachine,
    # Per-version release folder: derive OutDir/BuildDir/StageDir from the version so each
    # release lives in one self-contained folder (release/V<version>/ holds the build tree,
    # the stage dir, the generated WiX manifest, the MSI/ZIP artifacts and the release notes);
    # the release/ root then only contains one folder per version plus the process README.
    [switch]$VersionedDir
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# ---- 版本号：允许 v1.2.3 / 1.2.3-rc1；MSI 需要纯 x.y.z ----
$verRaw = $Version -replace '^v', ''
if ($verRaw -notmatch '^([0-9]+)\.([0-9]+)\.([0-9]+)') {
    throw "version must look like 1.2.3 (got '$Version')"
}
$verNumeric = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
# ---- latest.json 的 tag 名：显式参数 > 仓库里已存在的同名 tag > v<版本> ----
$tagName = "v$verNumeric"
if ($TagName) {
    $tagName = $TagName
} else {
    $existing = @(git tag --list "*$verNumeric" 2>$null) |
        Where-Object { $_ -match "^[vV]?[0-9]+\.[0-9]+\.[0-9]+$" } |
        Select-Object -First 1
    if ($existing) { $tagName = $existing }
}
Write-Host "Release version: $Version (MSI: $verNumeric, tag: $tagName)"

if ($VersionedDir) {
    # Folder name mirrors the historical snapshots: release-V1.0.1 -> release-V1.0.2 ...
    $OutDir   = Join-Path "release" ("release-V" + $verNumeric)
    $BuildDir = Join-Path $OutDir "build"
    $StageDir = Join-Path $OutDir "stage"
    Write-Host "per-version release folder: $OutDir (build: $BuildDir, stage: $StageDir)"
}

Step "1/7 configure + build"
if (-not $SkipBuild) {
    # Pass the version as ONE quoted argument: PowerShell 5.1 truncates an unquoted
    # -DMIDERHIVE_VERSION=1.0.0 down to "...=1", which would silently stamp the wrong
    # version into the UI, /api/health and the installer.
    cmake -S . -B $BuildDir -G $Generator -A x64 "-DCMAKE_PREFIX_PATH=$QtPrefix" `
        "-DMIDERHIVE_VERSION=$Version" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build $BuildDir --config $Configuration | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} else {
    Write-Host "skipped (-SkipBuild)"
}

Step "2/7 install to stage dir ($StageDir)"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
cmake --install $BuildDir --config $Configuration --prefix "$root/$StageDir" --component Runtime | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }

Step "3/7 assert the staged build is runnable"
# These are exactly the files whose absence used to make a hand-assembled package fail to
# start on a clean machine. Fail loudly instead of publishing a broken artifact.
$required = @(
    "miderhive.exe",
    "agent-cli.exe",
    "platformd.exe",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6Svg.dll",
    "platforms/qwindows.dll",
    "licenses/THIRD-PARTY-NOTICES.md", "licenses/LICENSE"
)
$missing = @()
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $StageDir $f))) { $missing += $f }
}
if ($missing.Count -gt 0) { throw "stage dir is incomplete, missing: $($missing -join ', ')" }
foreach ($f in $required) { Write-Host ("  OK  {0}" -f $f) }

# MSVC runtime: windeployqt --compiler-runtime only copies it when VCINSTALLDIR/VCToolsRedistDir
# is visible. Without it the app cannot start on a machine that lacks the VC++ Redistributable.
if (-not (Test-Path (Join-Path $StageDir "vcruntime140.dll"))) {
    throw @"
vcruntime140.dll is missing from the stage dir, so this package would NOT start on a clean
Windows install. Fix by running the build from a Visual Studio developer environment
(VCToolsRedistDir set) or by installing the VC++ Redistributable as a prerequisite.
Do not publish this artifact as-is.
"@
}
Write-Host "  OK  vcruntime140.dll"

Step "4/7 generate WiX file manifest"
# release/ is fully untracked now (published via GitHub Releases), so the output dir may not
# exist on a fresh clone; gen-wix-files.ps1 writes with [IO.File]::WriteAllText, which does
# not create directories.
New-Item -ItemType Directory -Force -Path (Join-Path $root "$OutDir/wix") | Out-Null
if ($VersionedDir) {
    # Snapshot the installer definition next to the artifacts, mirroring the historical
    # release-V1.0.x folders that carried wix/<Brand>.wxs alongside files.generated.wxs.
    Copy-Item (Join-Path $root "installer/MiderHive.wxs") (Join-Path $root "$OutDir/wix/MiderHive.wxs")
}
if ($PerMachine) {
    & "$PSScriptRoot/gen-wix-files.ps1" -StageDir (Join-Path $root $StageDir) `
        -OutFile (Join-Path $root "$OutDir/wix/files.generated.wxs") -PerMachine
} else {
    & "$PSScriptRoot/gen-wix-files.ps1" -StageDir (Join-Path $root $StageDir) `
        -OutFile (Join-Path $root "$OutDir/wix/files.generated.wxs")
}

Step "5/7 build MSI (WiX $WixVersion)"
# WiX v7 requires accepting the Open Source Maintenance Fee EULA; v6 is the last release
# without it, which is why the version is pinned here.
$env:PATH = "$env:PATH;$env:USERPROFILE\.dotnet\tools"
if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    Write-Host "installing WiX $WixVersion as a dotnet global tool..."
    dotnet tool install --global wix --version $WixVersion | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "failed to install WiX $WixVersion" }
}
if ($PerMachine) { $scope = "perMachine"; $pm = 1 } else { $scope = "perUser"; $pm = 0 }
$msi = Join-Path $OutDir "MiderHive-$verNumeric-x64.msi"
# Adding an already-present extension reports a non-zero exit code; that is not a failure.
wix extension add -g "WixToolset.Util.wixext/$WixVersion" 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Host "(extension already installed, continuing)" }
wix build (Join-Path $root "installer/MiderHive.wxs") (Join-Path $OutDir "wix/files.generated.wxs") `
    -arch x64 -ext WixToolset.Util.wixext `
    -d Version="$verNumeric" -d Scope="$scope" -d PerMachine=$pm `
    -d IconFile="$root/src/gui/icon.ico" `
    -o "$root/$msi" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "wix build failed" }

# ICE validation gate: catches real installer defects before they ship. Suppressed ICEs:
#   ICE91 - "per-user directory does not vary with ALLUSERS": expected for a per-user
#           package and harmless (we never use ALLUSERS=1).
Write-Host "validating MSI (ICE)..."
wix msi validate "$root/$msi" -ext WixToolset.Util.wixext -sice ICE91 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "MSI failed ICE validation - see the errors above" }
Write-Host "  ICE validation passed"

Step "6/7 build portable ZIP"
$zipName = "MiderHive-$verNumeric-win64-portable.zip"
$zipPath = Join-Path $OutDir $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
# Top-level folder inside the zip so extracting does not scatter files.
$staging = Join-Path $env:TEMP ("miderhive-zip-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$inner = Join-Path $staging "MiderHive-$verNumeric"
New-Item -ItemType Directory -Force -Path $inner | Out-Null
Copy-Item -Path (Join-Path $StageDir "*") -Destination $inner -Recurse -Force
Compress-Archive -Path $inner -DestinationPath $zipPath -CompressionLevel Optimal
Remove-Item -Recurse -Force $staging

Step "7/8 checksums"
$sums = Join-Path $OutDir "SHA256SUMS.txt"
$lines = @()
$hashes = @{}
foreach ($f in @($msi, $zipPath)) {
    $h = (Get-FileHash -Algorithm SHA256 (Join-Path $root $f)).Hash.ToLower()
    $hashes[(Split-Path -Leaf $f)] = $h
    $lines += "$h  $(Split-Path -Leaf $f)"
}
[System.IO.File]::WriteAllLines((Join-Path $root $sums), $lines, (New-Object System.Text.UTF8Encoding($false)))
$lines | ForEach-Object { Write-Host "  $_" }

Step "8/8 update manifest (latest.json)"
# The in-app updater reads this instead of the GitHub API: it is fetched from the stable
# URL https://github.com/<repo>/releases/latest/download/latest.json, so it needs no token
# and is not subject to the unauthenticated API rate limit. It carries the SHA256 of every
# artifact, which is what the updater verifies before installing.
$msiName = Split-Path -Leaf $msi
$zipNameOnly = Split-Path -Leaf $zipPath
$base = "https://github.com/$Repo/releases/download/$tagName"
$manifest = [ordered]@{
    version      = $verNumeric
    tag          = $tagName
    published_at = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    repo         = $Repo
    notes_url    = "https://github.com/$Repo/releases/tag/$tagName"
    msi          = [ordered]@{
        name   = $msiName
        url    = "$base/$msiName"
        sha256 = $hashes[$msiName]
        size   = (Get-Item (Join-Path $root $msi)).Length
    }
    portable     = [ordered]@{
        name   = $zipNameOnly
        url    = "$base/$zipNameOnly"
        sha256 = $hashes[$zipNameOnly]
        size   = (Get-Item (Join-Path $root $zipPath)).Length
    }
}
$manifestPath = Join-Path $OutDir "latest.json"
$json = ($manifest | ConvertTo-Json -Depth 4)
[System.IO.File]::WriteAllText((Join-Path $root $manifestPath), $json + "`n",
                              (New-Object System.Text.UTF8Encoding($false)))
Write-Host "  $manifestPath"
Write-Host $json

if ($VersionedDir) {
    # Final contents must mirror the release-V1.0.x snapshot layout: deliverables, wix/ and
    # docs only. Drop the reproducible build/stage intermediates and snapshot the process
    # README so the folder is fully self-contained.
    Copy-Item (Join-Path $root "release/README.md") (Join-Path $root "$OutDir/README.md") -Force
    Remove-Item (Join-Path $root $BuildDir) -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $root $StageDir) -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "per-version folder finalized: $OutDir (README.md snapshotted, build/ stage/ removed)"
}

Write-Host "`n=== artifacts ===" -ForegroundColor Green
Get-ChildItem $OutDir -File | Where-Object { $_.Extension -in @(".msi", ".zip", ".txt", ".json") } |
    Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } } | Format-Table -AutoSize | Out-Host

# Native commands above may leave a non-zero $LASTEXITCODE even on success
# (e.g. "wix extension add" for an already-installed extension); reaching here means
# every checked step passed, so report success explicitly.
exit 0
