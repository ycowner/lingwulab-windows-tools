# mem.ps1 -- memory footprint measurement for the service module.
#
#   pwsh -NoProfile -File .\tools\service\mem.ps1
#   pwsh -NoProfile -File .\tools\service\mem.ps1 -Seconds 120
#
# Measures a CLEAN instance: the script launches its own process, waits for
# the poll loop to reach steady state, then samples. Re-running against an
# instance that has already been clicked through measures the wrong thing.
#
# Both numbers are reported, and the verdict is taken against PRIVATE bytes:
#
#   Working set includes the shared, copy-on-write text pages of the WinUI 3
#   framework DLLs (Microsoft.UI.Xaml.dll and friends) that the App SDK maps
#   in from the framework package. Those pages are shared across every process
#   using the runtime -- they grow with the *number of apps*, not with this
#   one. Gating on working set would be penalising the app for using the
#   framework, which is exactly the deployment model this project chose.
#
# Private bytes is what this process actually costs the system on its own,
# so that is the number the budget is set against.
#
# NOTE: the toolbox is ONE process with TWO resident pages now (the monitor
# page and the service page are both constructed once and kept alive; only
# Visibility flips). So this measures the whole application, not "the service
# half of it" -- the service module cannot be unloaded to save memory.

[CmdletBinding()]
param(
    [string]$Exe = '',
    [int]$Seconds = 60,
    [int]$IntervalSec = 5,
    # Private-bytes budget in MB. Exceeding it fails the script.
    #
    # 220 MB, set by the project owner on 2026-10-24 against the Phase 6b
    # five-scenario baseline of the MERGED toolbox: the steady state with both
    # pages realised is 201.3 MB private / 251.5 MB working set (347 service
    # rows plus ~200 process rows of ListView item containers).
    #
    # The previous 160 MB was set on 2026-10-23, when the merge was still
    # assumed to cost only the two background pipelines; the page
    # realisation cost had not been measured yet, and 160 cannot hold the
    # normal state.
    #
    # NOTE this script measures the SERVICE page only (a clean instance that
    # never visits the monitor page): that steady state is 113.8 MB private,
    # well under the budget. The 201 MB figure is the two-pages-realised one.
    #
    # The headroom is deliberate: different machines differ, and the gate is
    # on PRIVATE bytes only -- see the note above about why the working set
    # is recorded but never enforced.
    [double]$PrivateBudgetMb = 220
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\uia.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

Write-Host "=== service mem: clean instance, ${Seconds}s window ==="
Stop-App 'LingwuToolbox'
$p = Start-App $Exe
$win = Get-AppWindow $p.Id 60
Switch-Module $win 'Service'

# Let the first full enumeration land before sampling: the steady state is the
# thing worth measuring, not the transient spike while 345 services are being
# resolved for the first time.
$st = $null
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline) {
    $st = Get-StatusCounts $win
    if ($st.Total -gt 0) { break }
    Start-Sleep -Milliseconds 500
}
Write-Host "  enumeration settled: $($st.Total) services"
Start-Sleep -Seconds 5

$samples = @()
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $p.Refresh()
    $samples += [pscustomobject]@{
        T     = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        Priv  = [Math]::Round($p.PrivateMemorySize64 / 1MB, 1)
        Ws    = [Math]::Round($p.WorkingSet64 / 1MB, 1)
        Handles = $p.HandleCount
        Threads = $p.Threads.Count
    }
    Write-Host ("  t={0,6:N1}s  priv={1,7:N1} MB  ws={2,7:N1} MB  handles={3,5}  threads={4}" -f `
        $samples[-1].T, $samples[-1].Priv, $samples[-1].Ws, $samples[-1].Handles, $samples[-1].Threads)
    Start-Sleep -Seconds $IntervalSec
}

Write-Host "`n--- summary ---"
$last = $samples[-1]
$min = ($samples | Measure-Object Priv -Minimum).Minimum
$max = ($samples | Measure-Object Priv -Maximum).Maximum
$peak = ($samples | Measure-Object Priv -Maximum).Maximum
Write-Host ("  private   : start {0} MB  min {1} MB  end {2} MB  peak {3} MB  budget {4} MB" -f `
    $samples[0].Priv, $min, $last.Priv, $peak, $PrivateBudgetMb)
Write-Host ("  workingset: start {0} MB  end {1} MB  (recorded, never enforced)" -f `
    $samples[0].Ws, $last.Ws)
Write-Host ("  handles   : {0}   threads: {1}" -f $last.Handles, $last.Threads)
$delta = [Math]::Round($last.Priv - $samples[0].Priv, 1)
Write-Host ("  private drift over the window: {0} MB" -f $delta)
if ([Math]::Abs($delta) -gt 10) {
    Write-Host ("  [warn] private moved {0} MB during the window; the steady state" -f $delta)
    Write-Host ("         is supposed to be flat. A rising curve means something is" +
                " still accumulating per frame.")
}

if ($PrivateBudgetMb -gt 0 -and $peak -gt $PrivateBudgetMb) {
    Write-Host ("`nFAIL: private peaked at {0} MB, over the {1} MB budget." -f $peak, $PrivateBudgetMb)
    Stop-App 'LingwuToolbox'
    exit 1
}
if ($PrivateBudgetMb -gt 0) {
    Write-Host ("`nOK: private peaked at {0} MB, within the {1} MB budget." -f $peak, $PrivateBudgetMb)
}

Stop-App 'LingwuToolbox'
exit 0
