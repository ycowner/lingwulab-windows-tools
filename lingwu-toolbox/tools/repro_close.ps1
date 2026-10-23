# repro_close.ps1 —— 最小复现：T1/T2 通过后，第三次 WM_CLOSE 为什么把进程弄没了
#
# 用法：pwsh -NoProfile -File .\tools\repro_close.ps1
# 需要 %LOCALAPPDATA%\LingwuSysMon\trace.on 存在（脚本自己会建）。

param(
    [string]$Exe = ''
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
public class RC {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@

$mdir = Join-Path $env:LOCALAPPDATA 'LingwuSysMon'
New-Item -ItemType Directory -Path $mdir -Force | Out-Null
Set-Content -Path (Join-Path $mdir 'trace.on') -Value '1' -Encoding ASCII
$log = Join-Path $mdir 'trace.log'
if (Test-Path $log) { Move-Item $log "$log.prev" -Force }

function Get-AppProc { Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 } | Select-Object -First 1 }
function Stop-App {
    Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
function Find-MainWindow([int]$procId) {
    $c = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $procId)),
        (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Window)))
    return $AE::RootElement.FindFirst($TS::Children, $c)
}
function Get-Hwnd([int]$procId) {
    $w = Find-MainWindow $procId
    if (-not $w) { return [IntPtr]::Zero }
    return [IntPtr]$w.Current.NativeWindowHandle
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
function Alive([int]$procId) { $null -ne (Get-Process -Id $procId -ErrorAction SilentlyContinue) }
function Mark([string]$s) { Write-Output ">> $s" }

$IniPath = Join-Path $env:APPDATA 'LingwuSysMon\settings.ini'
Stop-App
if (Test-Path $IniPath) {
    (Get-Content $IniPath) -replace '^\s*CloseAction\s*=\s*\d+', 'CloseAction=0' | Set-Content $IniPath -Encoding UTF8
}

$p = Start-Process $Exe -PassThru
$procId = $p.Id
$hwnd = [IntPtr]::Zero
$dl = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $dl -and $hwnd -eq [IntPtr]::Zero) { $hwnd = Get-Hwnd $procId; if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 } }
Write-Output "pid=$procId hwnd=$hwnd"
Start-Sleep -Seconds 8
Mark 'ROUND 1: post WM_CLOSE'
[RC]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$b = Wait-El $procId $CT::Button '最小化到托盘' 15
Write-Output "  dialog visible = $($null -ne $b) ; alive = $(Alive $procId)"
Mark 'ROUND 1: invoke cancel'
$can = Find-El $procId $CT::Button '取消'
if ($can) { $can.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
Start-Sleep -Seconds 4
Write-Output "  after cancel: alive=$(Alive $procId) visible=$([bool]([RC]::IsWindowVisible($hwnd))) dialogGone=$($null -eq (Find-El $procId $CT::Button '退出程序'))"

Mark 'ROUND 2: post WM_CLOSE (this is the one that used to kill the process)'
[RC]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 5
Write-Output "  after close#2: alive=$(Alive $procId) visible=$([bool]([RC]::IsWindowVisible($hwnd))) dialogShown=$($null -ne (Find-El $procId $CT::Button '退出程序'))"

Mark 'ROUND 3: post WM_CLOSE'
[RC]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 5
Write-Output "  after close#3: alive=$(Alive $procId) visible=$([bool]([RC]::IsWindowVisible($hwnd))) dialogShown=$($null -ne (Find-El $procId $CT::Button '退出程序'))"

Mark '---- trace.log (close-related lines) ----'
if (Test-Path $log) {
    $enc = [System.Text.Encoding]::UTF8
    $all = [System.IO.File]::ReadAllLines($log, $enc)
    $all | Where-Object { $_ -match 'close|Close|prompt|dialog|Hide|Request|ignored|remembered|Trace' } |
        Select-Object -Last 60 | ForEach-Object { Write-Output "   $_" }
    Write-Output "---- total lines: $($all.Count) ----"
} else { Write-Output '   (no trace.log)' }

if (Alive $procId) { Stop-App }
Write-Output 'done'
