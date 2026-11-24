<#
.SYNOPSIS
  Run the UI/UX harness (offscreen) in one command and point at its report.

.DESCRIPTION
  The same invocation as the CI step: offscreen platform, Qt's bin on PATH, a
  fresh scratch data directory. The harness compares 47 scenes with the
  committed baselines (tests/baseline/), checks the invariant rules and probes
  that user state survives a refresh. Its exit code is the result:
    0  identical to the baselines, no violations
    1  could not run (missing exe, no fonts, a filter that matches no scene, ...)
    2  structural differences or rule violations

  Output goes to build\ui_harness\: one JSON snapshot and one PNG per scene,
  the report ui_report.html (every screenshot, all rule findings), console.log.
  A structural difference prints where it differs (JSON path, both values).

  After an intentional UI change: run with -UpdateBaseline on a clean tree,
  review `git diff tests/baseline/` scene by scene, commit it with the code.

  NOTE: keep this file ASCII-only (see package.ps1).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\ui_harness.ps1

.EXAMPLE
  # only scenes whose id contains "logs", then open the report in the browser
  powershell -ExecutionPolicy Bypass -File scripts\ui_harness.ps1 -Filter logs -OpenReport
#>
param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$Filter = "",
    [switch]$UpdateBaseline,
    [switch]$OpenReport,
    [string]$QtPrefix = "C:/Qt/6.8.3/msvc2022_64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$exe = Join-Path $BuildDir "src\gui\$Configuration\ui_harness.exe"
if (-not (Test-Path $exe)) {
    throw "ui_harness.exe not found: $exe. Build it first: cmake --build $BuildDir --config $Configuration"
}
$qtBin = (Join-Path $QtPrefix "bin") -replace "/", "\"
if (-not (Test-Path (Join-Path $qtBin "Qt6Core.dll"))) {
    throw "Qt6Core.dll not found in $qtBin (use -QtPrefix)"
}

$outDir = Join-Path $BuildDir "ui_harness"
# A brand-new data directory every run: the harness seeds it and refuses to run
# on a directory that already holds a database.
$scratch = Join-Path $env:TEMP ("miderhive-uiharness-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $outDir, $scratch | Out-Null

$env:QT_QPA_PLATFORM = "offscreen"
$env:MIDERHIVE_HOME = $scratch
$env:PATH = "$qtBin;" + $env:PATH

$harnessArgs = @()
if ($Filter) { $harnessArgs += @("--filter", $Filter) }
if ($UpdateBaseline) { $harnessArgs += "--update-baseline" }
$argLine = ($harnessArgs | ForEach-Object { '"' + $_ + '"' }) -join " "

$consoleLog = Join-Path $outDir "console.log"
$stderrLog = Join-Path $outDir "console.stderr.log"
# The harness prints UTF-8. The console encoding decides how PowerShell decodes
# native output, so it has to be UTF-8 before the harness runs, not just before
# the lines are shown.
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
# stdout is captured and echoed below; Qt warnings on stderr go to a file.
# (Windows PowerShell 5.1 turns redirected native stderr into error records.)
$lines = cmd /c "`"$exe`" $argLine 2> `"$stderrLog`""
$code = $LASTEXITCODE

$lines | Set-Content -Path $consoleLog -Encoding UTF8 -ErrorAction SilentlyContinue
foreach ($line in $lines) { Write-Host $line }

Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue

$report = Join-Path $outDir "ui_report.html"
Write-Host ""
Write-Host ("report : {0}" -f $report)
if (Test-Path $stderrLog) {
    $warn = @(Get-Content $stderrLog | Where-Object { $_.Trim() })
    if ($warn.Count -gt 0) { Write-Host ("stderr : {0} line(s) of Qt/diagnostics in {1}" -f $warn.Count, $stderrLog) }
}
if ($UpdateBaseline) {
    Write-Host "baselines re-recorded: review 'git diff tests/baseline/' scene by scene before committing"
}
if ($OpenReport -and (Test-Path $report)) { Start-Process $report }
exit $code
