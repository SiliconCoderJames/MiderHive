<#
.SYNOPSIS
  Deploy the current build as a runnable, debuggable workbench in one fixed folder: dist\miderhive.exe

.DESCRIPTION
  Refreshed every time work lands, so there is always one up-to-date executable
  to run and to debug. Uses the everyday dev build tree (no clean rebuild):
    1. build the four shipped targets (skippable with -SkipBuild)
    2. refuse if a program is still running from the output folder
    3. cmake --install into dist.new (windeployqt puts the Qt runtime next to the exe)
    4. assert the folder is runnable; copy PDBs when the configuration produced any
    5. write BUILD_INFO.txt (version, commit, uncommitted-changes flag, deploy time)
    6. swap dist.new -> dist
    7. smoke test: start the deployed exe offscreen on a scratch data directory and
       a free port, read the version back from /api/health, stop it
  Releases still go through scripts\package.ps1 (clean build, MSI, ZIP, checksums).

  The default configuration is RelWithDebInfo: the same optimisations as Release
  (/O2) plus debug symbols, so breakpoints in this code base work in a debugger
  while the program runs at release speed. Release builds carry no PDBs and cannot
  be debugged at source level. CI and the official packages stay on Release; pass
  -Configuration Release to deploy that instead.

  How to run and debug the result is written into dist\BUILD_INFO.txt and, in
  more detail, in docs/dev.md.

  The smoke test never touches your real settings: it runs on a scratch data
  directory, draws no window, and is stopped well before the 5-second mark where
  the startup paths that write user settings begin (update check, welcome dialog).

  NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads UTF-8-without-BOM as ANSI,
  which corrupts non-ASCII comments and can break parsing.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\deploy_dev.ps1

.EXAMPLE
  # the tree is already built and tested: only redeploy it
  powershell -ExecutionPolicy Bypass -File scripts\deploy_dev.ps1 -SkipBuild
#>
param(
    [string]$BuildDir = "build",
    # RelWithDebInfo = release speed + PDBs, so the deployed exe can be debugged
    # at source level. Release carries no symbols; CI and the official packages
    # stay on Release. A Debug configuration cannot be used with the prebuilt Qt
    # (it would link the debug C runtime against Qt's release DLLs).
    [string]$Configuration = "RelWithDebInfo",
    [string]$OutDir = "dist",
    [string]$QtPrefix = "C:/Qt/6.8.3/msvc2022_64",
    [switch]$SkipBuild,
    [switch]$NoSmokeTest
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

$outAbs = [System.IO.Path]::GetFullPath((Join-Path $root $OutDir))
$newAbs = $outAbs + ".new"
$dirSep = [System.IO.Path]::DirectorySeparatorChar

# name -> relative output path (without the configuration segment)
$programs = [ordered]@{
    "miderhive.exe"      = "src\gui"
    "agent-cli.exe"      = "src\cli"
    "platformd.exe"      = "src\cli"
    "miderhive-mcp.exe"  = "src\cli"
}

if (-not $SkipBuild) {
    Step "build ($Configuration) from $BuildDir"
    cmake --build $BuildDir --config $Configuration --target miderhive agent-cli platformd miderhive-mcp | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

foreach ($name in $programs.Keys) {
    $built = Join-Path $BuildDir (Join-Path $programs[$name] (Join-Path $Configuration $name))
    if (-not (Test-Path $built)) {
        throw "build output missing: $built (built with -DBUILD_GUI=OFF, or wrong -Configuration?)"
    }
}

# The swap below deletes the old folder. That cannot work while an exe or DLL in it
# is loaded, and silently leaving a half-old build behind would be worse than stopping.
$running = @(Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -and $programs.Contains($_.Name) -and
    $_.ExecutablePath.StartsWith($outAbs + $dirSep, [System.StringComparison]::OrdinalIgnoreCase)
})
if ($running.Count -gt 0) {
    $list = ($running | ForEach-Object { "$($_.Name) (PID $($_.ProcessId))" }) -join ", "
    throw "$list is still running from $OutDir. Close it first (tray icon -> quit), then run this script again. Nothing was changed."
}

Step "install into $OutDir.new"
if (Test-Path $newAbs) { Remove-Item -Recurse -Force $newAbs }
cmake --install $BuildDir --config $Configuration --prefix $newAbs --component Runtime | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed (nothing in $OutDir was changed)" }

Step "assert the build is runnable"
# Same list as scripts\package.ps1: each of these has been missing from a package that
# then failed to start, so their absence must stop the deploy instead of shipping it.
$required = @(
    "miderhive.exe", "agent-cli.exe", "platformd.exe", "miderhive-mcp.exe",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6Svg.dll",
    "platforms/qwindows.dll", "vcruntime140.dll",
    "licenses/THIRD-PARTY-NOTICES.md", "licenses/LICENSE"
)
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $newAbs $_)) })
if ($missing.Count -gt 0) { throw "deploy is incomplete, missing: $($missing -join ', ')" }

# Symbols for debugger sessions. Release does not produce any; RelWithDebInfo does.
$symbols = @()
foreach ($name in $programs.Keys) {
    $pdbName = $name -replace "\.exe$", ".pdb"
    $pdb = Join-Path $BuildDir (Join-Path $programs[$name] (Join-Path $Configuration $pdbName))
    if (Test-Path $pdb) {
        Copy-Item $pdb $newAbs -Force
        $symbols += $pdbName
    }
}

# "Debuggable" is checked, not assumed: a PDB that belongs to another build is
# rejected by debuggers just like a missing one. scripts\check_symbols.py asks
# DbgHelp whether the copied PDB matches this exe and resolves source lines.
# Done before the swap so a bad result is visible while the old dist is still there.
$symbolStatus = "no (built without debug info: breakpoints in a debugger will not bind)"
if ($symbols.Count -gt 0) {
    $symbolStatus = $symbols -join ", "
    $checker = Join-Path $root "scripts\check_symbols.py"
    if ((Get-Command python -ErrorAction SilentlyContinue) -and (Test-Path $checker)) {
        $savedPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"   # python on stderr must not abort the deploy
        try {
            $checkOutput = & python $checker (Join-Path $newAbs "miderhive.exe") 2>&1
            $checkCode = $LASTEXITCODE
        } catch {
            $checkCode = 2
            $checkOutput = $_.Exception.Message
        } finally {
            $ErrorActionPreference = $savedPreference
        }
        if ($checkCode -eq 0) {
            $symbolStatus += " - verified against miderhive.exe (DbgHelp: match, line numbers resolve)"
            Write-Host "  OK  miderhive.pdb matches miderhive.exe; breakpoints in src\ will bind"
        } else {
            $symbolStatus += " - NOT verified against miderhive.exe"
            Write-Host "warning: the copied symbols were not verified against miderhive.exe." -ForegroundColor Yellow
            Write-Host (($checkOutput | Select-Object -Last 2) -join [Environment]::NewLine)
        }
    } else {
        $symbolStatus += " - not verified (scripts\check_symbols.py or python not available)"
    }
}

$version = "unknown"
$versionHeader = Join-Path $BuildDir "src\core\generated\core\version.h"
if (Test-Path $versionHeader) {
    $m = Select-String -Path $versionHeader -Pattern '#define MIDERHIVE_VERSION\s+"([^"]+)"' |
        Select-Object -First 1
    if ($m) { $version = $m.Matches[0].Groups[1].Value }
}
$commit = "unknown"
$branch = "unknown"
$dirty = $false
try {
    $commit = (git rev-parse --short HEAD | Out-String).Trim()
    $branch = (git rev-parse --abbrev-ref HEAD | Out-String).Trim()
    $dirty = [bool]((git status --porcelain | Out-String).Trim())
} catch {
    Write-Host "warning: could not read git state ($($_.Exception.Message))"
}

Step "swap $OutDir.new -> $OutDir"
if (Test-Path $outAbs) { Remove-Item -Recurse -Force $outAbs }
Rename-Item -Path $newAbs -NewName (Split-Path -Leaf $outAbs)

# Each line is one complete string. Do not write "text" + (expr) inside the array
# literal: PowerShell binds the comma before the plus sign, so the rest of the
# array becomes the right operand and the lines come out split.
$stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
$dirtyNote = if ($dirty) { " + UNCOMMITTED CHANGES" } else { "" }
$info = @(
    "MiderHive dev build"
    "version : $version"
    "commit  : $commit ($branch)$dirtyNote"
    "config  : $Configuration, built from $BuildDir"
    "symbols : $symbolStatus"
    "deployed: $stamp"
    ""
    "Run"
    "  miderhive.exe  - double-click, or run it from a terminal. It uses the normal data"
    "                   directory (%USERPROFILE%\.miderhive) and port 8787 unless"
    "                   MIDERHIVE_HOME / MIDERHIVE_PORT say otherwise."
    "  agent-cli.exe, platformd.exe, miderhive-mcp.exe sit next to it."
    "  Not an installer: nothing is added to the Start menu and no file outside this"
    "  folder is written by the deploy."
    ""
    "Debug (source-level, needs the symbols above)"
    "  1. Start miderhive.exe yourself."
    "  2. Visual Studio: Debug > Attach to Process... > pick miderhive.exe."
    "  3. Open a file under src\ and set breakpoints. Variables may read as"
    "     'optimized away': this build runs at release speed."
    "  To keep a debug session away from your real data, start it on a scratch profile:"
    "    PowerShell:  `$env:MIDERHIVE_HOME=`"`$env:TEMP\mh-scratch`"; `$env:MIDERHIVE_PORT=`"8790`"; .\miderhive.exe"
    "  More (tests, UI harness, demo data, mock LLM): docs/dev.md in the repository."
)
Set-Content -Path (Join-Path $outAbs "BUILD_INFO.txt") -Value $info -Encoding ASCII

if (-not $NoSmokeTest) {
    Step "smoke test (offscreen, scratch data directory)"
    # A free port nobody else holds: bind port 0, read what the OS handed out, release it.
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    $scratch = Join-Path $env:TEMP ("miderhive-deploysmoke-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $outAbs "miderhive.exe"
    $start.WorkingDirectory = $outAbs
    $start.UseShellExecute = $false
    $start.EnvironmentVariables["MIDERHIVE_HOME"] = $scratch
    $start.EnvironmentVariables["MIDERHIVE_PORT"] = [string]$port
    $start.EnvironmentVariables["QT_QPA_PLATFORM"] = "offscreen"
    # The deploy ships only the Windows platform plugin. The offscreen plugin comes from
    # the same Qt installation the build used, so every other DLL is still the deployed one.
    $start.EnvironmentVariables["QT_PLUGIN_PATH"] = (Join-Path $QtPrefix "plugins") -replace "/", "\"

    $proc = [System.Diagnostics.Process]::Start($start)
    $seenVersion = $null
    try {
        foreach ($i in 1..12) {   # 12 x 250 ms: always stops before the 5 s mark
            Start-Sleep -Milliseconds 250
            if ($proc.HasExited) { break }
            try {
                $request = [System.Net.HttpWebRequest]::Create("http://127.0.0.1:$port/api/health")
                $request.Proxy = $null
                $request.Timeout = 1000
                $response = $request.GetResponse()
                $reader = New-Object System.IO.StreamReader($response.GetResponseStream())
                $health = $reader.ReadToEnd() | ConvertFrom-Json
                $reader.Close(); $response.Close()
                $seenVersion = $health.data.version
                if (-not $seenVersion) { $seenVersion = $health.version }
                if ($seenVersion) { break }
            } catch {
                # not listening yet
            }
        }
    } finally {
        if (-not $proc.HasExited) { $proc.Kill() }
        $proc.WaitForExit(5000) | Out-Null
        Start-Sleep -Milliseconds 300
        Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
    }
    if (-not $seenVersion) {
        throw "the deployed miderhive.exe did not answer /api/health (exit code $($proc.ExitCode)). The build is in $OutDir but has not been shown to start."
    }
    if ($version -ne "unknown" -and $seenVersion -ne $version) {
        throw "deployed exe reports version $seenVersion but the build tree says $version. $OutDir holds a stale build: rebuild before deploying."
    }
    Write-Host "  OK  miderhive.exe started and answered /api/health with version $seenVersion"
}

Step "done"
Write-Host ("  run:   {0}" -f (Join-Path $outAbs "miderhive.exe"))
Write-Host ("  build: {0}  commit {1}{2}" -f $version, $commit, $(if ($dirty) { " (+ uncommitted changes)" } else { "" }))
