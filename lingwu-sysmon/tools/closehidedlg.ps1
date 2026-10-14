# closehidedlg.ps1 —— 第四组：CloseAction=1 的「隐藏」完全不经过 ContentDialog
#
# 【回答的问题】
# 若 closememab.ps1 显示「最小化-还原」几乎不涨内存、而「真隐藏-显示」涨 ~12 MB，
# 那就要区分这 12 MB 到底是
#   ① AppWindow.Hide() 本身（XamlIsland / 行容器被回收，重建要重新物化）
# 还是
#   ② 反复弹关闭确认框（每次 ShowAsync 都要建 Popup / XamlRoot 内容呈现）
#
# 本组把对话框摘掉：CloseAction=1 时点 X **直接隐藏、不弹框**。
# 与 closememab.ps1 的 hideshow 组逐项对照，差值就是对话框那一项的代价。
#
# 用法：pwsh -NoProfile -File .\tools\closehidedlg.ps1

param(
    [string]$Exe = '',
    [int]$Settle = 120,
    [int]$Tail = 90,
    [int]$Cycles = 5,
    [string]$OutCsv = "$env:TEMP\lingsysmon_closehidedlg.csv"
)

# exe 路径按脚本位置解析，项目目录改名后不必再改这里。
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\dist\Release\LingwuSysmon.exe' }

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]
$WM_CLOSE = 0x0010

Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class HD {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@

function Stop-App {
    Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 } |
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
function Find-TrayButton {
    $tray = $AE::RootElement.FindFirst($TS::Children,
        (New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'Shell_TrayWnd')))
    if (-not $tray) { return $null }
    $bc = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Button)
    foreach ($b in $tray.FindAll($TS::Descendants, $bc)) {
        if ($b.Current.ClassName -eq 'SystemTray.NormalButton' -and $b.Current.Name -like 'Lingwu SysMon*') { return $b }
    }
    return $null
}

# 等托盘按钮并 Invoke，带重试。
#
# 【为什么要重试、为什么要区分「没找到」与「Invoke 了但没反应」】
# 首版写成 `if ($tray) { Invoke }`，找不到就静默跳过，于是「窗口没回来」
# 和「脚本根本没去点托盘」两种完全不同的情况在输出里长得一模一样 ——
# 实测第 3 轮就出现了这种歧义，根本没法判断是应用缺陷还是脚本自己没点。
function Restore-ViaTray([int]$procId, [IntPtr]$hwnd, [int]$sec = 25) {
    $dl = (Get-Date).AddSeconds($sec)
    $attempts = 0
    while ((Get-Date) -lt $dl) {
        if ([bool]([HD]::IsWindowVisible($hwnd))) { return @{ ok = $true; why = "already visible (attempts=$attempts)" } }
        $b = Find-TrayButton
        if ($b) {
            $attempts++
            try {
                $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            } catch {
                Start-Sleep -Milliseconds 600
                continue
            }
            Start-Sleep -Seconds 2
            if ([bool]([HD]::IsWindowVisible($hwnd))) { return @{ ok = $true; why = "restored via tray, invoke#$attempts" } }
        }
        Start-Sleep -Milliseconds 800
    }
    return @{ ok = $false; why = "NOT RESTORED: trayFound=$([bool](Find-TrayButton)) invokeAttempts=$attempts" }
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

$mdir = Join-Path $env:LOCALAPPDATA 'LingwuSysMon'
if (Test-Path (Join-Path $mdir 'trace.on')) {
    Move-Item (Join-Path $mdir 'trace.on') (Join-Path $mdir 'trace.on.disabled') -Force
}

# CloseAction = 1：点 X 直接隐藏，不弹确认框
$IniPath = Join-Path $env:APPDATA 'LingwuSysMon\settings.ini'
Stop-App
if (Test-Path $IniPath) {
    (Get-Content $IniPath) -replace '^\s*CloseAction\s*=\s*\d+', 'CloseAction=1' | Set-Content $IniPath -Encoding UTF8
} else {
    New-Item -ItemType Directory -Path (Split-Path $IniPath) -Force | Out-Null
    Set-Content -Path $IniPath -Value "[Settings]`r`nCloseAction=1" -Encoding ASCII
}

Write-Output "=== CloseAction=1 隐藏/显示 x$Cycles（全程不弹对话框）==="
$p = Start-Process $Exe -PassThru
$procId = $p.Id
$hwnd = [IntPtr]::Zero
$dl = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $dl -and $hwnd -eq [IntPtr]::Zero) {
    $w = Get-MainWindow $procId
    if ($w) { $hwnd = [IntPtr]$w.Current.NativeWindowHandle }
    if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'FAIL: 窗口没出现'; exit 1 }
Write-Output "  pid=$procId"

Start-Sleep -Seconds 8
for ($i = 1; $i -le $Settle; $i++) {
    if (-not (Sample $procId 'hidenodlg' $i)) { break }
    Start-Sleep -Seconds 1
}
$t = $Settle; $ok = 0; $failRestore = 0
for ($c = 1; $c -le $Cycles; $c++) {
    [HD]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 3; $t++
    if (-not (Sample $procId 'hidenodlg' $t "hide$c")) { break }
    $hid = -not [bool]([HD]::IsWindowVisible($hwnd))
    # 关键自检：这一轮绝不能出现对话框
    $dlg = Find-El $procId $CT::Button '最小化到托盘'
    Start-Sleep -Seconds 7; $t++
    if (-not (Sample $procId 'hidenodlg' $t "hidden$c")) { break }
    $r = Restore-ViaTray $procId $hwnd 25
    $t++
    if (-not (Sample $procId 'hidenodlg' $t "show$c")) { break }
    Write-Output ("  cycle {0}: hidden={1} restoreOk={2} noDialog={3}  [{4}]" -f $c, $hid, $r.ok, ($null -eq $dlg), $r.why)
    if (-not $hid) { Write-Output '  隐藏失败，中止'; break }
    if (-not $r.ok) { $failRestore++; Write-Output "  !! 第 $c 轮唤回失败：$($r.why)"; continue }
    $ok++
    Start-Sleep -Seconds 7; $t++
    if (-not (Sample $procId 'hidenodlg' $t "rest$c")) { break }
}
for ($i = 1; $i -le $Tail; $i++) {
    if (-not (Sample $procId 'hidenodlg' ($t + $i))) { break }
    Start-Sleep -Seconds 1
}
Stop-App
$samples | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8

# 注意：不能写 `@($samples)` —— 对 List[object] 会抛
# 「Argument types do not match」（首版就这么炸了，数据全丢）。
# 用管道让 PowerShell 自行枚举即可。
$g = @($samples | ForEach-Object { $_ })
Write-Output ''
Write-Output '============ RESULT (hidenodlg) ============'
Write-Output ("cycles ok={0}/{1}  restoreFailures={2}" -f $ok, $Cycles, $failRestore)
Write-Output ("first={0}  last={1}  last-ws={2}  last-h={3}  samples={4}" -f `
    $g[0].priv, $g[-1].priv, $g[-1].ws, $g[-1].h, $g.Count)
if ($g.Count -ge 90) {
    $l = @($g | Select-Object -Last 60)
    $lo = ($l | Measure-Object priv -Minimum).Minimum
    $hi = ($l | Measure-Object priv -Maximum).Maximum
    Write-Output ("tail-60s spread={0} MB ({1}..{2})" -f [math]::Round($hi - $lo, 1), $lo, $hi)
}
Write-Output ''
Write-Output '同一次构建的四组对照：'
Write-Output '  control      130.6 MB  静止 120s'
Write-Output '  minrestore   135.8 MB  最小化/还原 5 轮（无对话框）'
Write-Output '  hidenodlg    <=本组<=  隐藏/显示（无对话框，CloseAction=1）'
Write-Output '  hideshow     144.0 MB  隐藏/显示 5 轮（每轮弹一次确认框）'
Write-Output "CSV=$OutCsv"

# 还原 ini 与 trace
if (Test-Path $IniPath) {
    (Get-Content $IniPath) -replace '^\s*CloseAction\s*=\s*\d+', 'CloseAction=0' | Set-Content $IniPath -Encoding UTF8
}
if (Test-Path (Join-Path $mdir 'trace.on.disabled')) {
    Move-Item (Join-Path $mdir 'trace.on.disabled') (Join-Path $mdir 'trace.on') -Force
    Write-Output '[cleanup] ini CloseAction=0、trace.on 已还原'
}
