<#
.SYNOPSIS
  Run the same checks as .github/workflows/ci.yml against the local build, before pushing.

.DESCRIPTION
  v1.6.0 shipped while the CI workflow was red on master: its last gate (bool-return
  discipline) failed and nobody had looked, because the local routine only ever ran
  ctest and the GUI self-test. This script runs every post-build step of ci.yml, in
  the same order, against build\, and reports all results at once instead of stopping
  at the first failure.

  It does not build. Run `cmake --build build --config Release` first.
  Integration steps start platformd on ports 19090..19092 with scratch data
  directories under %TEMP%; nothing reads or writes the real data directory.

  Every step is required, as in CI. That includes the UI harness (a required check since
  2026-11-24; before that it was observation-only while CI showed 27-30 differences as
  success). After an intentional UI change, re-record the baselines on a fresh data
  directory (`build\src\gui\Release\ui_harness.exe --update-baseline`), review
  `git diff tests/baseline/` scene by scene, and commit them together with the code.

  NOTE: keep this file ASCII-only (see package.ps1).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\ci_local.ps1
#>
param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$QtPrefix = "C:/Qt/6.8.3/msvc2022_64"
)

# Native tools write progress and warnings to stderr; "Stop" would turn the first
# such line into a terminating error. Every step checks its own outcome instead.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
# The scripts print Chinese; under a cp1252 console Python raises UnicodeEncodeError
# in the middle of a report (it did on CI, hiding most of the violations).
$env:PYTHONIOENCODING = "utf-8"
# ...and PowerShell has to read that UTF-8 as UTF-8, not as the console's code page,
# or every Chinese line a script prints arrives garbled.
$consoleUtf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $consoleUtf8
$OutputEncoding = $consoleUtf8

$cliDir = Join-Path $BuildDir "src\cli\$Configuration"
$guiDir = Join-Path $BuildDir "src\gui\$Configuration"
$platformd = Join-Path $cliDir "platformd.exe"
$agentCli = Join-Path $cliDir "agent-cli.exe"
$mcpExe = Join-Path $cliDir "miderhive-mcp.exe"
$harness = Join-Path $guiDir "ui_harness.exe"

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

function Assert-Exit($what) {
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        throw "$what failed (exit code $LASTEXITCODE)"
    }
}

function Test-Health($port) {
    try {
        # Proxy = $null: a system proxy would otherwise be asked for 127.0.0.1 too.
        $request = [System.Net.HttpWebRequest]::Create("http://127.0.0.1:$port/api/health")
        $request.Proxy = $null
        $request.Timeout = 2000
        $response = $request.GetResponse()
        $response.Close()
        return $true
    } catch {
        return $false
    }
}

function Wait-Healthy($proc, $port) {
    foreach ($i in 1..30) {
        if ($proc.HasExited) { throw "platformd exited early on port $port (exit code $($proc.ExitCode))" }
        if (Test-Health $port) { return }
        Start-Sleep -Seconds 1
    }
    throw "platformd did not become healthy on port $port within 30 s"
}

# Starts platformd on $port with a fresh scratch data directory. The caller must stop
# the returned process with Stop-Platformd.
function Start-Platformd($port, $dataHome) {
    if (Test-Path $dataHome) { Remove-Item -Recurse -Force $dataHome }
    New-Item -ItemType Directory -Force -Path $dataHome | Out-Null
    $proc = Start-Process -FilePath $platformd -PassThru -NoNewWindow
    try {
        Wait-Healthy $proc $port
    } catch {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw
    }
    return $proc
}

function Stop-Platformd($proc) {
    if ($null -eq $proc) { return }
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    $null = $proc.WaitForExit(5000)
}

# CI gives every step a fresh shell, so a variable set for one step never reaches the
# next. -Vars does the same here: the variables are set for the step only, and
# whatever they held before is restored afterwards.
function Run-Step($name, [scriptblock]$Body, [hashtable]$Vars = @{}) {
    Step $name
    $saved = @{}
    foreach ($k in $Vars.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k, "Process")
        [Environment]::SetEnvironmentVariable($k, [string]$Vars[$k], "Process")
    }
    $ok = $true
    $note = ""
    try {
        $note = & $Body
    } catch {
        $ok = $false
        $note = $_.Exception.Message
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], "Process") }
    }
    $null = $results.Add([pscustomobject]@{ Name = $name; Ok = $ok })
    if (-not $ok) { Write-Host ("  -> " + $note) -ForegroundColor Red }
}

# Same as CI's runner, where no MIDERHIVE_*/MH_* variable exists: a variable set for
# the developer's real installation must not steer a check.
$inherited = @{}
foreach ($entry in @([Environment]::GetEnvironmentVariables("Process").GetEnumerator())) {
    if ($entry.Key -match "^(MIDERHIVE_|MH_)") {
        $inherited[$entry.Key] = $entry.Value
        [Environment]::SetEnvironmentVariable($entry.Key, $null, "Process")
    }
}

$results = New-Object System.Collections.ArrayList

Run-Step "Build outputs" -Body {
    foreach ($f in @($platformd, $agentCli, $mcpExe, (Join-Path $guiDir "miderhive.exe"))) {
        if (-not (Test-Path $f)) { throw "missing: $f (build first: cmake --build $BuildDir --config $Configuration)" }
    }
    "ok"
}

Run-Step "Unit tests" -Body {
    ctest --test-dir $BuildDir -C $Configuration --output-on-failure | Out-Host
    Assert-Exit "ctest"
    "ok"
}

$feasibilityHome = Join-Path $env:TEMP "miderhive-ci-local-feasibility"
Run-Step "Integration feasibility check" -Vars @{
    MIDERHIVE_MASTER_KEY = "ci-test-master-key-not-a-secret"
    MIDERHIVE_HOME       = $feasibilityHome
    MIDERHIVE_PORT       = "19090"
} -Body {
    $p = Start-Platformd 19090 $feasibilityHome
    try {
        # bootstrap writes the preset zcode manager key to config\agents.json in the
        # data directory.
        $agents = Get-Content (Join-Path $feasibilityHome "config\agents.json") | ConvertFrom-Json
        $env:MIDERHIVE_ZCODE_KEY = $agents.zcode
        python scripts/feasibility_check.py 19090 | Out-Host
        Assert-Exit "feasibility check"
        "ok"
    } finally {
        Stop-Platformd $p
    }
}

$mcpHome = Join-Path $env:TEMP "miderhive-ci-local-mcp"
Run-Step "MCP server check (stdio JSON-RPC)" -Vars @{
    MIDERHIVE_MASTER_KEY = "ci-test-master-key-not-a-secret"
    MIDERHIVE_HOME       = $mcpHome
    MIDERHIVE_PORT       = "19091"
} -Body {
    $p = Start-Platformd 19091 $mcpHome
    try {
        $key = (Get-Content (Join-Path $mcpHome "config\agents.json") | ConvertFrom-Json).zcode
        python scripts/mcp_check.py 19091 $mcpExe zcode $key | Out-Host
        Assert-Exit "MCP check"
        "ok"
    } finally {
        Stop-Platformd $p
    }
}

Run-Step "GUI end-to-end self-test (offscreen)" -Body {
    python scripts/verify_gui_selftest.py --build $BuildDir | Out-Host
    Assert-Exit "GUI self-test"
    "ok"
}

$harnessHome = Join-Path $env:TEMP "miderhive-ci-local-uiharness"
Run-Step "UI harness baseline check" -Vars @{
    MIDERHIVE_HOME   = $harnessHome
    QT_QPA_PLATFORM  = "offscreen"
} -Body {
    if (-not (Test-Path $harness)) { throw "ui_harness.exe not built (BUILD_TESTS=OFF?)" }
    if (Test-Path $harnessHome) { Remove-Item -Recurse -Force $harnessHome }
    New-Item -ItemType Directory -Force -Path $harnessHome | Out-Null
    # ui_harness needs the Qt DLLs next to nothing: they live in the Qt prefix.
    $qtBin = (Join-Path $QtPrefix "bin") -replace "/", "\"
    if (Test-Path $qtBin) { $env:PATH = "$qtBin;" + $env:PATH }
    $log = Join-Path $env:TEMP "miderhive-ci-local-uiharness.log"
    # Output goes to a file: Qt writes warnings to stderr, and the summary line is
    # what matters here.
    $p = Start-Process -FilePath $harness -PassThru -NoNewWindow -Wait `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $line = Select-String -Path $log -Pattern "ui_harness " | Select-Object -Last 1
    $text = if ($line) { $line.Line.Trim() } else { "no summary line in $log" }
    Write-Host "  $text"
    if ($p.ExitCode -ne 0) { throw "$text (exit code $($p.ExitCode); log: $log)" }
    $text
}

Run-Step "Onboarding and key-rotation self-test" -Vars @{ MH_PLATFORMD = $platformd; MH_PORT = "19092" } -Body {
    python scripts/test_onboarding_and_safety.py | Out-Host
    Assert-Exit "onboarding/key-rotation self-test"
    "ok"
}

Run-Step "Config merge fuzz test" -Body {
    python scripts/fuzz_config_merge.py --agent-cli $agentCli | Out-Host
    Assert-Exit "config merge fuzz test"
    "ok"
}

Run-Step "Per-client connect matrix" -Body {
    python scripts/verify_clients.py --agent-cli $agentCli | Out-Host
    Assert-Exit "client connect matrix"
    "ok"
}

Run-Step "External embedding endpoint round-trip" -Body {
    python scripts/embed_cli_check.py --agent-cli $agentCli --platformd $platformd | Out-Host
    Assert-Exit "external embedding round-trip"
    "ok"
}

Run-Step "Bool-return discipline" -Body {
    python scripts/check_bool_returns.py --fail | Out-Host
    Assert-Exit "bool-return check"
    "ok"
}

foreach ($k in $inherited.Keys) { [Environment]::SetEnvironmentVariable($k, $inherited[$k], "Process") }
Remove-Item -Recurse -Force $feasibilityHome, $mcpHome, $harnessHome -ErrorAction SilentlyContinue

Step "summary"
$width = ($results | ForEach-Object { $_.Name.Length } | Measure-Object -Maximum).Maximum
foreach ($r in $results) {
    $ok = $r.Ok
    $color = if ($ok) { "Gray" } else { "Red" }
    $mark = if ($ok) { "ok  " } else { "FAIL" }
    Write-Host ("  {0}  {1}" -f $mark, $r.Name.PadRight($width)) -ForegroundColor $color
}
$failed = @($results | Where-Object { -not $_.Ok })
if ($failed.Count -gt 0) {
    Write-Host ("`n{0} check(s) failed. Do not push until ci_local.ps1 is clean." -f $failed.Count) -ForegroundColor Red
    exit 1
}
Write-Host "`nAll required checks passed." -ForegroundColor Green
exit 0
