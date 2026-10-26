# run-all.ps1 -- drive the whole acceptance suite in one go.
#
# WHY THIS FILE EXISTS
#
#   The individual acceptance scripts live under tools\monitor, tools\service
#   and tools\common. Running them one by one is slow and easy to get wrong:
#   every script has its own timeout, every script has to be killed-and-
#   restarted cleanly, and the results have to be counted consistently.
#   This file does all three, so one command gives one answer.
#
#   It also encodes the OUTPUT CONTRACT the scripts follow (see AGENTS.md
#   "The output contract of the acceptance scripts"):
#       [PASS] / [FAIL]  -- assertion results
#       SKIP:            -- this check cannot run on this machine
#                           (environment limitation, NOT a product defect)
#       RESULT: pass=N fail=M
#   SKIP is counted in its own column: neither a pass nor a failure.
#
# USAGE
#
#   pwsh -NoProfile -File .\tools\build\run-all.ps1
#       Runs the whole suite ELEVATED. UI Automation cannot see across an
#       integrity-level boundary, so the suite has to run as an administrator
#       to be able to drive the app at all. If this shell is not elevated the
#       script re-launches itself elevated ONCE for the entire round -- so
#       there is exactly one UAC prompt per round, not one per script.
#
#   pwsh -NoProfile -File .\tools\build\run-all.ps1 -Unelevated
#       Runs the whole suite against the asInvoker verification copy
#       (build\unelevated\LingwuToolbox.exe). No UAC prompt, but every write
#       operation is refused by the OS, so the scripts that need to write
#       (service/create, service/admin) report SKIP with the reason.
#
#   pwsh -NoProfile -File .\tools\build\run-all.ps1 -Only 'monitor/close-flow,service/check'
#       Runs a subset. NOTE: -Only takes a COMMA SEPARATED string, because
#       `pwsh -File script.ps1 -Only 'a','b'` hands the whole thing over as
#       ONE string ("a,b") -- -File parses the tail as plain script
#       arguments and never builds an array. The script splits it apart
#       again so the filter still matches.
#
#   -OutDir <path>   Where the per-script logs and the summary go.
#                    Default: %TEMP%\lingwu_toolbox_runs
#
# EXIT CODE
#   0 when no script FAILED. A SKIP does not fail the round -- it is
#   reported, not hidden.

[CmdletBinding()]
param(
    [string[]]$Only = @(),
    [switch]$Unelevated,
    [string]$OutDir = (Join-Path $env:TEMP 'lingwu_toolbox_runs')
)

$ErrorActionPreference = 'Continue'

# Resolve the repository root from the script location, so the file works
# from any working directory and on any machine.
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'tools\common\common.ps1')

# One UAC prompt for the whole round. Require-Elevated re-launches THIS
# script elevated and relays the child's output and exit code back.
if (-not $Unelevated) {
    Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
}

$exe = Join-Path $root 'dist\Release\LingwuToolbox.exe'
if ($Unelevated) {
    # The verification copy: same binary with the manifest patched to
    # asInvoker, so it starts without a UAC prompt and can be driven from
    # this shell. UI Automation cannot see across the integrity boundary.
    $exe = Join-Path $root 'build\unelevated\LingwuToolbox.exe'
    $env:LINGWU_TOOLBOX_UNELEVATED = '1'
    $env:LINGWU_TOOLBOX_EXE = $exe
    if (-not (Test-Path $exe)) {
        Write-Host ("MISSING: the unelevated verification copy is not there." -f $exe)
        Write-Host 'Build it once: copy dist\Release to build\unelevated, then patch the'
        Write-Host 'embedded manifest from requireAdministrator to asInvoker.'
        exit 2
    }
}
if (-not (Test-Path $exe)) {
    Write-Host ("MISSING: {0} -- build it first (build.bat)." -f $exe)
    exit 2
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$summaryFile = Join-Path $OutDir 'summary.txt'
Remove-Item -LiteralPath $summaryFile -Force -ErrorAction SilentlyContinue

function Note([string]$s) {
    Write-Host $s
    Add-Content -LiteralPath $summaryFile -Value $s -Encoding UTF8
}

# name, script path relative to $root, timeout in seconds
#
# The timeouts are generous on purpose: the memory scripts legitimately
# take minutes (close-mem-ab runs three groups of 120 s each), and a
# timeout that fires on a slow machine produces a false "TIMEOUT".
$scripts = @(
    @{ n = 'monitor/check';           p = 'tools\monitor\check.ps1';           t = 240 }
    @{ n = 'monitor/sort';            p = 'tools\monitor\sort.ps1';            t = 240 }
    @{ n = 'monitor/align';           p = 'tools\monitor\align.ps1';           t = 300 }
    @{ n = 'monitor/menu';            p = 'tools\monitor\menu.ps1';            t = 240 }
    @{ n = 'monitor/apply';           p = 'tools\monitor\apply.ps1';           t = 300 }
    @{ n = 'monitor/dump';            p = 'tools\monitor\dump.ps1';            t = 240 }
    @{ n = 'monitor/settings';        p = 'tools\monitor\settings.ps1';        t = 300 }
    @{ n = 'monitor/settings-reopen'; p = 'tools\monitor\settings-reopen.ps1'; t = 700 }
    @{ n = 'monitor/mem-rows';        p = 'tools\monitor\mem-rows.ps1';        t = 400 }
    @{ n = 'monitor/mem-scroll';      p = 'tools\monitor\mem-scroll.ps1';      t = 400 }
    @{ n = 'monitor/virtualize';      p = 'tools\monitor\virtualize.ps1';      t = 400 }
    @{ n = 'monitor/diag';            p = 'tools\monitor\diag.ps1';            t = 240 }
    @{ n = 'monitor/shot';            p = 'tools\monitor\shot.ps1';            t = 240 }
    @{ n = 'monitor/repro-close';     p = 'tools\monitor\repro-close.ps1';     t = 300 }
    @{ n = 'monitor/close-flow';      p = 'tools\monitor\close-flow.ps1';      t = 700 }
    @{ n = 'monitor/close-ab';        p = 'tools\monitor\close-ab.ps1';        t = 900 }
    @{ n = 'monitor/close-hide-dlg';  p = 'tools\monitor\close-hide-dlg.ps1';  t = 700 }
    @{ n = 'monitor/close-mem-ab';    p = 'tools\monitor\close-mem-ab.ps1';    t = 1100 }
    @{ n = 'monitor/tray-invoke';     p = 'tools\monitor\tray-invoke.ps1';     t = 700; prepare = $true }
    @{ n = 'monitor/tray-click';      p = 'tools\monitor\tray-click.ps1';      t = 700; prepare = $true }
    # Phase 1 (Mole-style shell + status page + tray panel).
    @{ n = 'monitor/nav';             p = 'tools\monitor\nav.ps1';             t = 400 }
    @{ n = 'monitor/tiles';           p = 'tools\monitor\tiles.ps1';           t = 400 }
    @{ n = 'monitor/proc-icon';       p = 'tools\monitor\proc-icon.ps1';       t = 400 }
    @{ n = 'monitor/tray-panel';      p = 'tools\monitor\tray-panel.ps1';      t = 400; prepare = $true }
    @{ n = 'monitor/keep-awake';      p = 'tools\monitor\keep-awake.ps1';      t = 400; prepare = $true }
    @{ n = 'monitor/clean-screen';    p = 'tools\monitor\clean-screen.ps1';    t = 400; prepare = $true }
    @{ n = 'monitor/sparkline';       p = 'tools\monitor\sparkline.ps1';       t = 700; prepare = $true }
    @{ n = 'monitor/exit-all';        p = 'tools\monitor\exit-all.ps1';        t = 400; prepare = $true }
    @{ n = 'service/check';           p = 'tools\service\check.ps1';           t = 300 }
    @{ n = 'service/create';          p = 'tools\service\create.ps1';          t = 700 }
    @{ n = 'service/mem';             p = 'tools\service\mem.ps1';             t = 300 }
    @{ n = 'service/admin';           p = 'tools\service\admin.ps1';           t = 900 }
    @{ n = 'common/mem-baseline';     p = 'tools\common\mem-baseline.ps1';     t = 1500 }
)

if ($Only.Count -eq 1 -and $Only[0] -like '*,*') {
    $Only = $Only[0] -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
}
if ($Only.Count -gt 0) {
    $scripts = $scripts | Where-Object { $Only -contains $_.n }
    if ($scripts.Count -eq 0) {
        Write-Host ("No script matches -Only '{0}'." -f ($Only -join ','))
        Write-Host ('Known names: ' + (($scripts | ForEach-Object { $_.n }) -join ', '))
        exit 2
    }
}

Note ('run-all: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Note ('  exe      = ' + $exe)
Note ('  elevated = ' + (Test-Elevated))
Note ('  mode     = ' + $(if ($Unelevated) { 'UNELEVATED (verification copy)' } else { 'ELEVATED' }))
Note ('  outdir   = ' + $OutDir)
Note ''

$results = @()
foreach ($s in $scripts) {
    Write-Host ''
    Write-Host ('==================== ' + $s.n + ' ====================')

    # Every script starts from a clean slate: a leftover toolbox holds the
    # exe open and the NEXT build fails with LNK1104.
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    # NOT $s.prepare directly: common.ps1 turns on Set-StrictMode -Version
    # Latest, and under StrictMode reading a key that a hashtable does not
    # have raises PropertyNotFoundException -- so every script WITHOUT a
    # 'prepare' key used to print a red error block before it even started.
    # ContainsKey is the check that does not throw.
    if ($s.ContainsKey('prepare') -and $s['prepare']) {
        Write-Host ('  preparing: starting the app for ' + $s.n)
        $null = Start-Process $exe -PassThru
        Start-Sleep -Seconds 14
    }

    $log = Join-Path $OutDir (($s.n -replace '[\\/]', '_') + '.txt')
    $errLog = $log + '.err'
    $child = Start-Process -FilePath 'pwsh.exe' `
        -ArgumentList @('-NoProfile', '-File', (Join-Path $root $s.p)) `
        -RedirectStandardOutput $log -RedirectStandardError $errLog `
        -PassThru -NoNewWindow
    try {
        Wait-Process -Id $child.Id -Timeout $s.t -ErrorAction Stop
        $child.Refresh()
        $code = $child.ExitCode
    } catch {
        $child | Stop-Process -Force -ErrorAction SilentlyContinue
        $code = 'TIMEOUT'
    }

    $out = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    $err = if (Test-Path $errLog) { Get-Content $errLog -Raw } else { '' }
    $text = "$out`n$err"

    $verdict = if ($code -eq 'TIMEOUT') { 'TIMEOUT' }
               elseif ($code -eq 0) { 'EXIT0' }
               else { "EXIT$code" }

    # The scripts report in three shapes. Counting only one of them is how
    # a failing script ends up looking green, so count all of them:
    #   [ok] / [PASS] / "PASS:"   -> a pass
    #   [FAIL] / "FAIL:"           -> a failure
    #   "SKIP:"                    -> could not run here (environment)
    $okCount = ([regex]::Matches($text, '(?m)\[ok\]|\[PASS\]|^\s*PASS\b')).Count
    $failCount = ([regex]::Matches($text, '(?m)\[FAIL\]|^\s*FAIL\b')).Count
    $skipCount = ([regex]::Matches($text, '(?m)^\s*SKIP\b')).Count
    $passLine = ($text -split "`n" |
        Where-Object { $_ -match '^(PASS|FAIL|RESULT|SKIP)' } |
        Select-Object -Last 1)

    Write-Host ("  verdict={0}  ok={1}  fail={2}  skip={3}  {4}" -f `
        $verdict, $okCount, $failCount, $skipCount, $passLine)
    Note ("{0,-26} {1,-8} ok={2,-4} fail={3,-4} skip={4,-4} {5}" -f `
        $s.n, $verdict, $okCount, $failCount, $skipCount, $passLine)
    $results += [pscustomobject]@{
        Name = $s.n; Verdict = $verdict; Ok = $okCount
        Fail = $failCount; Skip = $skipCount; Line = $passLine
    }

    if ($s.ContainsKey('prepare') -and $s['prepare']) {
        Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host '==================== SUMMARY ===================='
Note '==================== SUMMARY ===================='
($results | Format-Table -AutoSize | Out-String) -split "`n" |
    ForEach-Object { Note $_.TrimEnd() }

$failed = @($results | Where-Object { $_.Verdict -ne 'EXIT0' -or $_.Fail -gt 0 })
$skipped = @($results | Where-Object { $_.Skip -gt 0 })
Note ''
Note ("scripts: {0}   failed: {1}   with SKIPs: {2}" -f `
    $results.Count, $failed.Count, $skipped.Count)
foreach ($f in $failed) { Note ("  FAILED: {0} ({1}) {2}" -f $f.Name, $f.Verdict, $f.Line) }
foreach ($s in $skipped) { Note ("  SKIP  : {0} {1}" -f $s.Name, $s.Line) }
Note ("summary file: {0}" -f $summaryFile)

if ($failed.Count -gt 0) { exit 1 } else { exit 0 }
