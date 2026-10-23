# mem-baseline.ps1 -- Phase 6b: the long-running memory baseline for the
# toolbox's red line.
#
# WHAT THIS IS FOR
#
#   The two tools were merged into one process, so the old red lines (145 MB
#   and 160 MB) cannot be carried over: the baseline after the merge is
#   necessarily different. The red line number is the user's to set, and it
#   has to be set against measured data from THIS binary -- so this script
#   produces that data.
#
#   The user's measurement contract (decided 2026-10-13):
#     * report BOTH the working set and the private memory,
#     * the red line gates on the PRIVATE MEMORY only,
#     * the verdict goes in the private-memory column,
#     * one sentence under the table explains where the difference comes from
#       (the framework DLL pages are shared across processes and are not
#       "used" by this one).
#
# WHY THE SCENARIOS ARE THESE
#
#   Five scenarios, chosen because each one exercises a different thing that
#   could allocate:
#
#     idle     -- the process just sits there. Baseline.
#     switch   -- the two pages are built once and kept resident; switching
#                 only flips Visibility. The first visit to each page creates
#                 hundreds of rows (347 services / ~25 realised process rows),
#                 so this is where a one-off warm-up step shows up.
#     hideshow -- the window is hidden and shown again. The app does this with
#                 AppWindow.Hide() / AppWindow.Show(), which operate on the
#                 top-level HWND and do NOT touch the XAML visual tree (see
#                 MainWindow.xaml.cpp, the comment on HideSelf), so this
#                 measures the cost of detaching and re-attaching the window.
#     settings -- the settings window is opened and closed 30 times. It is
#                 constructed ONCE and only hidden on close, precisely
#                 because destroying it would leak native resources on every
#                 round (SettingsWindow::HideSelf). This scenario is the
#                 regression test for that decision.
#     tray     -- the window is hidden and the process stays in the tray for
#                 three minutes. A background timer still ticks, so this is
#                 where a slow leak would show up.
#
# HOW LEAK AND WARM-UP ARE TOLD APART
#
#   The plan's criterion, kept verbatim:
#     "a wandering step moment + constant amplitude + permanently flat
#      afterwards + unchanged handle count => framework warm-up, not a leak;
#      a real leak keeps rising and usually comes with growing handles"
#
#   So each scenario reports:
#     slope     -- least-squares MB/min over the scenario
#     tail slope-- the same, but over the LAST THIRD only. A warm-up step
#                  makes the overall slope look bad while the tail is flat;
#                  a real leak has a rising tail too. The tail is what
#                  decides.
#     steps     -- how many samples moved by >= 2 MB
#     handles   -- the delta over the scenario
#
#   Verdict: a rising tail (or a growing handle count) => leak. A flat tail
#   with unchanged handles => warm-up / no leak. Steps alone are not a leak.
#
# Pure ASCII on purpose (repo rule for .ps1), and it must be run with pwsh
# (Windows PowerShell 5.1 mis-binds the UIAutomationClient interop).

param(
    [string]$Exe,
    [int]$IdleSeconds = 60,
    [int]$SwitchRounds = 20,
    [int]$HideShowRounds = 20,
    [int]$SettingsRounds = 30,
    [int]$TraySeconds = 180,
    [string]$OutCsv = "$env:TEMP\lingwu_toolbox_mem_baseline.csv"
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'uia.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

# ShowWindow / IsWindowVisible, for the hide-show scenario.
# Guarded: re-running the script inside one PowerShell session would otherwise
# fail with "the type name already exists".
if (-not ('MemBaseWin' -as [type])) {
    Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class MemBaseWin {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
}

$script:ProcId = 0
$script:MainHwnd = 0
$script:T0 = Get-Date
$samples = New-Object System.Collections.Generic.List[object]
$results = New-Object System.Collections.Generic.List[object]

# ---------------------------------------------------------------- sampling --

function Add-Sample([string]$Scenario, [string]$Tag = '') {
    $live = Get-Process -Id $script:ProcId -ErrorAction SilentlyContinue
    if (-not $live) { return $null }
    $live.Refresh()
    $row = [pscustomobject]@{
        scenario = $Scenario
        tag      = $Tag
        t        = [math]::Round(((Get-Date) - $script:T0).TotalSeconds, 1)
        priv     = [math]::Round($live.PrivateMemorySize64 / 1MB, 1)
        ws       = [math]::Round($live.WorkingSet64 / 1MB, 1)
        h        = [live].HandleCount
    }
    $samples.Add($row)
    return $row
}

# Least-squares slope in MB per minute over the given rows.
function Get-Slope($rows, [string]$Metric = 'priv') {
    if ($rows.Count -lt 3) { return 0.0 }
    $n = [double]$rows.Count
    $sumT = 0.0; $sumX = 0.0; $sumTT = 0.0; $sumTX = 0.0
    foreach ($r in $rows) {
        $t = [double]$r.t
        $x = [double]$r.$Metric
        $sumT += $t; $sumX += $x; $sumTT += $t * $t; $sumTX += $t * $x
    }
    $den = $n * $sumTT - $sumT * $sumT
    if ([math]::Abs($den) -lt 1e-9) { return 0.0 }
    $perSecond = ($n * $sumTX - $sumT * $sumX) / $den
    return [math]::Round($perSecond * 60.0, 2)
}

function Add-Result([string]$Name, $rows) {
    if (-not $rows -or $rows.Count -eq 0) { return }
    $privStart = [double]$rows[0].priv
    $privEnd   = [double]$rows[$rows.Count - 1].priv
    $wsStart   = [double]$rows[0].ws
    $wsEnd     = [double]$rows[$rows.Count - 1].ws
    $privPeak  = [double](($rows | Measure-Object priv -Maximum).Maximum)
    $wsPeak    = [double](($rows | Measure-Object ws -Maximum).Maximum)
    $hStart    = [int]$rows[0].h
    $hEnd      = [int]$rows[$rows.Count - 1].h

    # The tail is the last third of the samples: it is what separates a
    # one-off warm-up step from a leak that keeps climbing.
    $tailStart = [int][math]::Floor($rows.Count / 3) * 2
    if ($tailStart -ge $rows.Count) { $tailStart = [math]::Max(0, $rows.Count - 3) }
    $tail = @($rows[$tailStart..($rows.Count - 1)])

    $steps = 0
    for ($i = 1; $i -lt $rows.Count; $i++) {
        if ([math]::Abs([double]$rows[$i].priv - [double]$rows[$i - 1].priv) -ge 2.0) { $steps++ }
    }

    $slope     = Get-Slope $rows 'priv'
    $tailSlope = Get-Slope $tail 'priv'
    $hDelta    = $hEnd - $hStart

    $verdict = if ($tailSlope -gt 0.5) { 'RISING (possible leak)' }
               elseif ($hDelta -gt 8) { 'RISING (handle count grew)' }
               elseif ($steps -gt 0) { 'warm-up step(s), then flat' }
               else { 'flat' }

    $results.Add([pscustomobject]@{
        Scenario   = $Name
        Samples    = $rows.Count
        WsStart    = [math]::Round($wsStart, 1)
        WsEnd      = [math]::Round($wsEnd, 1)
        WsPeak     = [math]::Round($wsPeak, 1)
        PrivStart  = [math]::Round($privStart, 1)
        PrivEnd    = [math]::Round($privEnd, 1)
        PrivPeak   = [math]::Round($privPeak, 1)
        HandleFrom = $hStart
        HandleTo   = $hEnd
        HandleDelta= $hDelta
        Slope      = $slope
        TailSlope  = $tailSlope
        Steps      = $steps
        Verdict    = $verdict
    })
}

# ------------------------------------------------------------------- setup --

Write-Host "exe = $Exe"
Stop-App
Start-Sleep -Seconds 2

$proc = Start-App $Exe
$script:ProcId = $proc.Id
Write-Host "pid = $($proc.Id)"

$win = Get-AppWindow $proc.Id 60
$script:MainHwnd = [int]$win.Current.NativeWindowHandle
Write-Host "main window hwnd = $script:MainHwnd"
Start-Sleep -Seconds 12          # let the first frame settle

try {
    # ------------------------------------------------------------- idle ----
    Write-Host ''
    Write-Host '--- scenario: idle ---'
    $rows = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $IdleSeconds; $i += 2) {
        $r = Add-Sample 'idle' ("t+{0}s" -f $i)
        if ($r) { $rows.Add($r) }
        Start-Sleep -Seconds 2
    }
    Add-Result 'idle' $rows

    # ----------------------------------------------------------- switch ----
    Write-Host ''
    Write-Host ("--- scenario: switch x{0} ---" -f $SwitchRounds)
    $rows = New-Object System.Collections.Generic.List[object]
    for ($i = 1; $i -le $SwitchRounds; $i++) {
        Switch-Module $win 'Service'
        $r = Add-Sample 'switch' ("svc#{0}" -f $i)
        if ($r) { $rows.Add($r) }
        Switch-Module $win 'Monitor'
        $r = Add-Sample 'switch' ("mon#{0}" -f $i)
        if ($r) { $rows.Add($r) }
    }
    Add-Result 'switch' $rows

    # ---------------------------------------------------------- hideshow ----
    Write-Host ''
    Write-Host ("--- scenario: hide/show x{0} ---" -f $HideShowRounds)
    $hwnd = [IntPtr]$script:MainHwnd
    $rows = New-Object System.Collections.Generic.List[object]
    for ($i = 1; $i -le $HideShowRounds; $i++) {
        [void][MemBaseWin]::ShowWindow($hwnd, 0)          # SW_HIDE
        Start-Sleep -Milliseconds 400
        $r = Add-Sample 'hideshow' ("hidden#{0}" -f $i)
        if ($r) { $rows.Add($r) }
        [void][MemBaseWin]::ShowWindow($hwnd, 5)          # SW_SHOW
        [void][MemBaseWin]::SetForegroundWindow($hwnd)
        Start-Sleep -Milliseconds 400
        $r = Add-Sample 'hideshow' ("shown#{0}" -f $i)
        if ($r) { $rows.Add($r) }
    }
    Add-Result 'hideshow' $rows

    # Re-acquire the window: after 40 hide/show cycles the cached
    # AutomationElement can be stale, and the settings scenario needs a live
    # one to look the SettingsButton up in.
    $win = Get-AppWindow $proc.Id 30
    $script:MainHwnd = [int]$win.Current.NativeWindowHandle
    Write-Host "main window re-acquired, hwnd = $script:MainHwnd"

    # ---------------------------------------------------------- settings ----
    Write-Host ''
    Write-Host ("--- scenario: settings open/close x{0} ---" -f $SettingsRounds)
    Switch-Module $win 'Monitor'
    $rows = New-Object System.Collections.Generic.List[object]
    for ($i = 1; $i -le $SettingsRounds; $i++) {
        $btn = Get-ById $win 'SettingsButton'
        Invoke-El $btn 'SettingsButton'
        Start-Sleep -Milliseconds 700
        $r = Add-Sample 'settings' ("open#{0}" -f $i)
        if ($r) { $rows.Add($r) }
        $sw = Find-VisibleSettingsWindow $script:ProcId $script:MainHwnd
        if ($sw) {
            $cancel = Get-ById $sw 'CancelButton'
            Invoke-El $cancel 'CancelButton'
            Start-Sleep -Milliseconds 700
        } else {
            Write-Host "  round ${i}: settings window not visible -- skipped the close"
        }
        $r = Add-Sample 'settings' ("closed#{0}" -f $i)
        if ($r) { $rows.Add($r) }
    }
    Add-Result 'settings' $rows

    # -------------------------------------------------------------- tray ----
    Write-Host ''
    Write-Host ("--- scenario: tray idle {0}s ---" -f $TraySeconds)
    $hwnd = [IntPtr]$script:MainHwnd
    [void][MemBaseWin]::ShowWindow($hwnd, 0)              # SW_HIDE
    $rows = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $TraySeconds; $i += 5) {
        $r = Add-Sample 'tray' ("t+{0}s" -f $i)
        if ($r) { $rows.Add($r) }
        Start-Sleep -Seconds 5
    }
    [void][MemBaseWin]::ShowWindow($hwnd, 5)              # SW_SHOW
    Add-Result 'tray' $rows
}
finally {
    Stop-App
}

$samples | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8

# ----------------------------------------------------------------- report --

Write-Host ''
Write-Host '==================== MEMORY BASELINE ===================='
Write-Host ('{0,-10} {1,-26} {2,-30} {3,-8} {4,-10} {5}' -f `
    'scenario', 'working set MB (start>end/peak)',
    'private MB (start>end/peak)', 'handles', 'slope', 'verdict')
Write-Host ('-' * 120)
foreach ($r in $results) {
    $ws = ('{0} > {1} / {2}' -f $r.WsStart, $r.WsEnd, $r.WsPeak)
    $pv = ('{0} > {1} / {2}   {3}' -f $r.PrivStart, $r.PrivEnd, $r.PrivPeak, $r.Verdict)
    Write-Host ('{0,-10} {1,-26} {2,-30} {3,-8} {4,-10} {5}' -f `
        $r.Scenario, $ws, $pv, ("{0}>{1}" -f $r.HandleFrom, $r.HandleTo),
        ("{0}/min" -f $r.Slope), ("tail {0}/min steps {1}" -f $r.TailSlope, $r.Steps))
}
Write-Host ('-' * 120)

$privPeakAll = [double](($results | Measure-Object PrivPeak -Maximum).Maximum)
$wsPeakAll   = [double](($results | Measure-Object WsPeak -Maximum).Maximum)
$rising = @($results | Where-Object { $_.Verdict -like 'RISING*' })

Write-Host ("peak private memory across all scenarios : {0} MB" -f $privPeakAll)
Write-Host ("peak working set across all scenarios     : {0} MB" -f $wsPeakAll)
if ($rising.Count -eq 0) {
    Write-Host 'verdict: no scenario shows a rising tail -- no leak detected.'
} else {
    Write-Host ("verdict: {0} scenario(s) show a rising tail -- investigate before setting a red line." -f $rising.Count)
}
Write-Host ("CSV = {0}" -f $OutCsv)
Write-Host ('The red line gates on the PRIVATE MEMORY column. The working set is')
Write-Host ('reported for information only: the gap between the two is the shared')
Write-Host ('framework DLL pages (Microsoft.UI.Xaml.dll and friends), which are')
Write-Host ('mapped into every process that uses the framework and are not owned')
Write-Host ('by this one.')

exit 0
