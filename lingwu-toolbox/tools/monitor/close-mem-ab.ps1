# closememab.ps1 —— 私有内存三组对照：静止 / 最小化-还原 / 真隐藏-显示
#
# 【要回答的唯一问题】
# 真隐藏（AppWindow.Hide）实测比静止态多约 12 MB。那这份代价是
#   ①「真隐藏」特有的，还是
#   ② 任何「窗口离开前台 → 再回来」都要付的？
# 决定性对照就是第 ② 组：**最小化-还原**。
#   · 若 ②≈① → Hide 并不比最小化更贵，回退成 SW_MINIMIZE 省不下内存，
#     该讨论的是「要不要接受 143 MB」，而不是怎么优化。
#   · 若 ②≈0 → 代价确实由 Hide 引起，值得进一步查（行容器回收 / 重新物化）。
#
# 最小化-还原这一组完全从外部用 ShowWindow 驱动，不经过应用代码，
# 所以它测的是「窗口不可见 → 可见」这件事本身的开销。
#
# 用法：pwsh -NoProfile -File .\tools\monitor\close-mem-ab.ps1

param(
    [string]$Exe = '',
    [int]$Control = 120,
    [int]$Settle = 120,
    [int]$Tail = 90,
    [int]$Cycles = 5,
    [string]$OutCsv = "$env:TEMP\lingwu_toolbox_closememab.csv"
)


$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
. (Join-Path $PSScriptRoot '..\common\tray.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]
$WM_CLOSE = 0x0010
$SW_MINIMIZE = 6
$SW_RESTORE  = 9

Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class MA {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
}
'@

function Stop-App {
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
function Get-MainWindow([int]$procId) {
    $c = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $procId)),
        (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Window)))
    return $AE::RootElement.FindFirst($TS::Children, $c)
}
function Find-El([int]$procId, $controlType, [string]$name) {
    $leaf = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $procId)),
        (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $controlType)))
    foreach ($e in $AE::RootElement.FindAll($TS::Descendants, $leaf)) {
        if ($e.Current.Name -eq $name) { return $e }
    }
    return $null
}
function Wait-El([int]$procId, $controlType, [string]$name, [int]$sec = 15) {
    $dl = (Get-Date).AddSeconds($sec)
    while ((Get-Date) -lt $dl) {
        $e = Find-El $procId $controlType $name
        if ($e) { return $e }
        Start-Sleep -Milliseconds 400
    }
    return $null
}
function Find-TrayButton {
    return (Test-ToolboxTrayIconPresent)
}

$samples = New-Object System.Collections.Generic.List[object]
function Sample([int]$procId, [string]$group, [int]$t, [string]$note = '') {
    $live = Get-Process -Id $procId -ErrorAction SilentlyContinue
    if (-not $live) { return $false }
    $samples.Add([pscustomobject]@{
        group = $group; t = $t; note = $note
        priv  = [math]::Round($live.PrivateMemorySize64 / 1MB, 1)
        ws    = [math]::Round($live.WorkingSet64 / 1MB, 1)
        h     = $live.HandleCount
    })
    return $true
}
function Start-App([string]$exe) {
    $p = Start-Process $exe -PassThru
    $dl = (Get-Date).AddSeconds(40)
    $h = [IntPtr]::Zero
    while ((Get-Date) -lt $dl -and $h -eq [IntPtr]::Zero) {
        $w = Get-MainWindow $p.Id
        if ($w) { $h = [IntPtr]$w.Current.NativeWindowHandle }
        if ($h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
    }
    return @{ proc = $p; hwnd = $h }
}
function Settle([int]$procId, [string]$g, [int]$sec) {
    Start-Sleep -Seconds 8
    for ($i = 1; $i -le $sec; $i++) {
        if (-not (Sample $procId $g $i)) { return $false }
        Start-Sleep -Seconds 1
    }
    return $true
}
function TailObs([int]$procId, [string]$g, [int]$from, [int]$sec) {
    for ($i = 1; $i -le $sec; $i++) {
        if (-not (Sample $procId $g ($from + $i))) { return }
        Start-Sleep -Seconds 1
    }
}

# 关掉 trace（文件 I/O 会污染曲线）
$mdir = Join-Path $env:LOCALAPPDATA 'LingwuToolbox'
if (Test-Path (Join-Path $mdir 'trace.on')) {
    Move-Item (Join-Path $mdir 'trace.on') (Join-Path $mdir 'trace.on.disabled') -Force
}
$IniPath = Join-Path $env:APPDATA 'LingwuToolbox\settings.ini'
Stop-App
if (Test-Path $IniPath) {
    (Get-Content $IniPath) -replace '^\s*CloseAction\s*=\s*\d+', 'CloseAction=0' | Set-Content $IniPath -Encoding UTF8
}

# ---------------------------------------------------------- 组 1：静止
Write-Output "=== GROUP control: 静止 $Control 秒 ==="
$g1 = Start-App $Exe
Write-Output "  pid=$($g1.proc.Id)"
Settle $g1.proc.Id 'control' $Control | Out-Null
Stop-App

# ------------------------------------------- 组 2：最小化-还原（外部驱动）
Write-Output "=== GROUP minrestore: 最小化/还原 x$Cycles（外部 ShowWindow，不经应用代码）==="
$g2 = Start-App $Exe
Write-Output "  pid=$($g2.proc.Id)"
Settle $g2.proc.Id 'minrestore' $Settle | Out-Null
$t2 = $Settle; $ok2 = 0
for ($c = 1; $c -le $Cycles; $c++) {
    [MA]::ShowWindow($g2.hwnd, $SW_MINIMIZE) | Out-Null
    Start-Sleep -Seconds 3; $t2++
    if (-not (Sample $g2.proc.Id 'minrestore' $t2 "min$c")) { break }
    $ic = [bool]([MA]::IsIconic($g2.hwnd))
    Start-Sleep -Seconds 7; $t2++
    if (-not (Sample $g2.proc.Id 'minrestore' $t2 "minned$c")) { break }
    [MA]::ShowWindow($g2.hwnd, $SW_RESTORE) | Out-Null
    Start-Sleep -Seconds 3; $t2++
    if (-not (Sample $g2.proc.Id 'minrestore' $t2 "restore$c")) { break }
    $vis = [bool]([MA]::IsWindowVisible($g2.hwnd)) -and (-not [bool]([MA]::IsIconic($g2.hwnd)))
    Write-Output ("  cycle {0}: minimized={1} restored={2}" -f $c, $ic, $vis)
    if (-not ($ic -and $vis)) { Write-Output '  中止'; break }
    $ok2++
    Start-Sleep -Seconds 7; $t2++
    if (-not (Sample $g2.proc.Id 'minrestore' $t2 "rest$c")) { break }
}
TailObs $g2.proc.Id 'minrestore' $t2 $Tail
Stop-App

# ------------------------------------------------ 组 3：真隐藏-显示（真实用户路径）
Write-Output "=== GROUP hideshow: 隐藏/显示 x$Cycles（走关闭确认框 + 托盘）==="
$g3 = Start-App $Exe
Write-Output "  pid=$($g3.proc.Id)"
Settle $g3.proc.Id 'hideshow' $Settle | Out-Null
$t3 = $Settle; $ok3 = 0
for ($c = 1; $c -le $Cycles; $c++) {
    [MA]::PostMessage($g3.hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $btn = Wait-El $g3.proc.Id $CT::Button '最小化到托盘' 15
    if (-not $btn) { Write-Output "  cycle $c : 确认框没出现，跳过"; continue }
    $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 3; $t3++
    if (-not (Sample $g3.proc.Id 'hideshow' $t3 "hide$c")) { break }
    $hid = -not [bool]([MA]::IsWindowVisible($g3.hwnd))
    Start-Sleep -Seconds 7; $t3++
    if (-not (Sample $g3.proc.Id 'hideshow' $t3 "hidden$c")) { break }
    [void](Invoke-ToolboxTrayButton)
    Start-Sleep -Seconds 3; $t3++
    if (-not (Sample $g3.proc.Id 'hideshow' $t3 "show$c")) { break }
    $shw = [bool]([MA]::IsWindowVisible($g3.hwnd))
    Write-Output ("  cycle {0}: hidden={1} shown={2}" -f $c, $hid, $shw)
    if (-not ($hid -and $shw)) { Write-Output '  中止'; break }
    $ok3++
    Start-Sleep -Seconds 7; $t3++
    if (-not (Sample $g3.proc.Id 'hideshow' $t3 "rest$c")) { break }
}
TailObs $g3.proc.Id 'hideshow' $t3 $Tail
Stop-App

$samples | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8

$ctrl  = @($samples | Where-Object { $_.group -eq 'control' })
$minr  = @($samples | Where-Object { $_.group -eq 'minrestore' })
$hide  = @($samples | Where-Object { $_.group -eq 'hideshow' })

Write-Output ''
Write-Output '=================== 3-GROUP RESULT ==================='
Write-Output ("cycles: minrestore={0}/{1}  hideshow={2}/{1}" -f $ok2, $Cycles, $ok3)
Write-Output ''
Write-Output 'group       n    first-priv  last-priv   last-ws   last-h  tail-spread'
function Spread($a) {
    if ($a.Count -lt 30) { return 'n/a' }
    $l = @($a | Select-Object -Last 60)
    $lo = ($l | Measure-Object priv -Minimum).Minimum
    $hi = ($l | Measure-Object priv -Maximum).Maximum
    return [math]::Round($hi - $lo, 1)
}
Write-Output ("control     {0,-4} {1,-11} {2,-11} {3,-9} {4,-7} {5}" -f $ctrl.Count, $ctrl[0].priv, $ctrl[-1].priv, $ctrl[-1].ws, $ctrl[-1].h, (Spread $ctrl))
Write-Output ("minrestore  {0,-4} {1,-11} {2,-11} {3,-9} {4,-7} {5}" -f $minr.Count, $minr[0].priv, $minr[-1].priv, $minr[-1].ws, $minr[-1].h, (Spread $minr))
Write-Output ("hideshow    {0,-4} {1,-11} {2,-11} {3,-9} {4,-7} {5}" -f $hide.Count, $hide[0].priv, $hide[-1].priv, $hide[-1].ws, $hide[-1].h, (Spread $hide))

$dMin = [math]::Round($minr[-1].priv - $ctrl[-1].priv, 1)
$dHid = [math]::Round($hide[-1].priv - $ctrl[-1].priv, 1)
Write-Output ''
Write-Output ("DELTA minrestore vs control = {0} MB" -f $dMin)
Write-Output ("DELTA hideshow   vs control = {0} MB" -f $dHid)
Write-Output ("DELTA hideshow   vs minrestore = {0} MB" -f [math]::Round($dHid - $dMin, 1))
Write-Output ''
if ($dHid -ne $null -and $dMin -ne $null) {
    if ([math]::Abs($dHid - $dMin) -le 3) {
        Write-Output 'VERDICT: 两者代价基本相同 ⇒ 隐藏并不比最小化更贵。'
        Write-Output '         这份内存是「窗口不可见 → 可见」的固有成本，回退成 SW_MINIMIZE 省不下。'
    } elseif ($dMin -lt 3 -and $dHid -ge 8) {
        Write-Output 'VERDICT: 最小化几乎不花内存、真隐藏明显更贵 ⇒ 代价由 Hide 引起，值得查（行容器回收）。'
    } else {
        Write-Output 'VERDICT: 介于两者之间，需要进一步细分。'
    }
}
Write-Output ''
Write-Output "CSV=$OutCsv"

if (Test-Path (Join-Path $mdir 'trace.on.disabled')) {
    Move-Item (Join-Path $mdir 'trace.on.disabled') (Join-Path $mdir 'trace.on') -Force
    Write-Output '[cleanup] trace.on 已还原'
}
