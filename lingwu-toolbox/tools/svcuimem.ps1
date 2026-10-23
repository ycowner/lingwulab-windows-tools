# uimem.ps1 -- memory footprint measurement.
#
#   pwsh -NoProfile -File .\tools\uimem.ps1
#   pwsh -NoProfile -File .\tools\uimem.ps1 -Seconds 120
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

[CmdletBinding()]
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\Release\LingwuServiceManager.exe'),
    [int]$Seconds = 60,
    [int]$IntervalSec = 5,
    # Private-bytes budget in MB. Exceeding it fails the script.
    #
    # 160 MB, set by the project owner against a measured clean-instance
    # steady state of 106.4 MB private / 161.3 MB working set on the reference
    # machine (345 services, zero drift over a 60 s window). The headroom is
    # deliberate: the interactive peak reaches ~149 MB private after filtering,
    # selecting rows and expanding the detail panel, and different machines
    # differ. The gate is on PRIVATE bytes only -- see the note above about why
    # working set is recorded but never enforced.
    [double]$PrivateBudgetMb = 160
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'uia.ps1')

Write-Host "=== uimem: clean instance, ${Seconds}s window ==="
Stop-App 'LingwuServiceManager'
$p = Start-App $Exe
$win = Get-AppWindow $p.Id 60

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
    Stop-App 'LingwuServiceManager'
    exit 1
}
if ($PrivateBudgetMb -gt 0) {
    Write-Host ("`nOK: private peaked at {0} MB, within the {1} MB budget." -f $peak, $PrivateBudgetMb)
}

Stop-App 'LingwuServiceManager'
exit 0
