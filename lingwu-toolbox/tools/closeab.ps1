# closeab.ps1 —— 「真隐藏到托盘」的私有内存 A/B 实测
#
# 【为什么必须实测，不能靠推断】
# 旧代码注释里有一条判断：「用 XAML 的 Visibility=Collapsed 隐藏窗口会让列表的
# 已实现容器被回收，下次显示要重新物化全部行，撞上 +26 MB 阶跃」。
# 但现在的实现走的是 AppWindow.Hide() —— 它只隐藏顶层 HWND，**不动 XAML 树**，
# 与那条判断针对的不是同一件事。所以「隐藏到底要不要付 +26 MB」是未知的，
# 只能靠同构建、同机器、同样本的受控对照来定。
#
# 【两组设计】
#   对照：启动 → 静置 Control 秒 → 读末端私有内存
#   实验：启动 → 静置到走平 → 隐藏/显示 ×Cycles 次 → 再静置 Tail 秒 → 读末端
# 两组都独立进程，采集风格沿用 mem.ps1（全存数组、结束一次性输出）。
#
# 【隐藏/显示走真实用户路径】
# 不是直接调 API，而是：PostMessage(WM_CLOSE) → Invoke「最小化到托盘」→ 隐藏；
# 托盘 InvokePattern → 显示。这样测到的才��用户实际会经历的那条路。
#
# 用法：pwsh -NoProfile -File .\tools\closeab.ps1

param(
    [string]$Exe = '',
    [int]$Control = 180,
    [int]$Settle = 120,
    [int]$Tail = 120,
    [int]$Cycles = 5,
    [string]$OutCsv = "$env:TEMP\lingsysmon_closeab.csv"
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
public class AB {
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
    $tray = $AE::RootElement.FindFirst($TS::Children,
        (New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'Shell_TrayWnd')))
    if (-not $tray) { return $null }
    $bc = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Button)
    foreach ($b in $tray.FindAll($TS::Descendants, $bc)) {
        if ($b.Current.ClassName -eq 'SystemTray.NormalButton' -and $b.Current.Name -like 'Lingwu SysMon*') { return $b }
    }
    return $null
}

# 采样一条
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

# 启动并等到稳态
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

# ---- 关掉 trace：它每次 ListCommit 都写文件，会污染内存曲线 ----
$mdir = Join-Path $env:LOCALAPPDATA 'LingwuSysMon'
if (Test-Path (Join-Path $mdir 'trace.on')) {
    Move-Item (Join-Path $mdir 'trace.on') (Join-Path $mdir 'trace.on.disabled') -Force
    Write-Output '[setup] trace.on 已临时停用（避免文件 I/O 污染内存曲线）'
}

$IniPath = Join-Path $env:APPDATA 'LingwuSysMon\settings.ini'
Stop-App
if (Test-Path $IniPath) {
    (Get-Content $IniPath) -replace '^\s*CloseAction\s*=\s*\d+', 'CloseAction=0' | Set-Content $IniPath -Encoding UTF8
}

Write-Output ''
Write-Output "=== GROUP A (control): 启动后静置 $Control 秒，全程不隐藏 ==="
$a = Start-App $Exe
Write-Output "  pid=$($a.proc.Id)"
Start-Sleep -Seconds 10
for ($i = 1; $i -le $Control; $i++) {
    if (-not (Sample $a.proc.Id 'control' $i)) { Write-Output "  process gone at t=$i"; break }
    Start-Sleep -Seconds 1
}
$ctrl = @($samples | Where-Object { $_.group -eq 'control' })
Write-Output ("  control: first priv={0}  last priv={1}  last ws={2}  handles={3}" -f `
    $ctrl[0].priv, $ctrl[-1].priv, $ctrl[-1].ws, $ctrl[-1].h)
Stop-App

Write-Output ''
Write-Output "=== GROUP B (hide/show x$Cycles): 静置 $Settle 秒 → 隐藏/显示 $Cycles 轮 → 再静置 $Tail 秒 ==="
$b = Start-App $Exe
Write-Output "  pid=$($b.proc.Id)"
Start-Sleep -Seconds 10
for ($i = 1; $i -le $Settle; $i++) {
    if (-not (Sample $b.proc.Id 'exp' $i)) { Write-Output "  process gone at settle t=$i"; break }
    Start-Sleep -Seconds 1
}
$settleEnd = $Settle
$cycleOk = 0
for ($c = 1; $c -le $Cycles; $c++) {
    $t = $settleEnd
    # --- 隐藏 ---
    [AB]::PostMessage($b.hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $btn = Wait-El $b.proc.Id $CT::Button '最小化到托盘' 15
    if (-not $btn) { Write-Output "  cycle $c : 关闭确认框没出现，跳过本轮"; continue }
    $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 3
    $t++
    if (-not (Sample $b.proc.Id 'exp' $t "hide$c")) { break }
    $hidden = -not [bool]([AB]::IsWindowVisible($b.hwnd))
    Write-Output ("  cycle {0}: hidden={1}" -f $c, $hidden)
    if (-not $hidden) { Write-Output "  cycle $c 隐藏失败，中止"; break }
    Start-Sleep -Seconds 7
    $t++
    if (-not (Sample $b.proc.Id 'exp' $t "hidden$c")) { break }
    # --- 显示 ---
    $tray = Find-TrayButton
    if ($tray) { $tray.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
    Start-Sleep -Seconds 3
    $t++
    if (-not (Sample $b.proc.Id 'exp' $t "show$c")) { break }
    $shown = [bool]([AB]::IsWindowVisible($b.hwnd))
    Write-Output ("  cycle {0}: shown={1}" -f $c, $shown)
    if (-not $shown) { Write-Output "  cycle $c 显示失败，中止"; break }
    $cycleOk++
    Start-Sleep -Seconds 7
    $t++
    if (-not (Sample $b.proc.Id 'exp' $t "rest$c")) { break }
}
$settleEnd = $t
for ($i = 1; $i -le $Tail; $i++) {
    if (-not (Sample $b.proc.Id 'exp' ($settleEnd + $i))) { Write-Output "  process gone at tail t=$i"; break }
    Start-Sleep -Seconds 1
}
$exp = @($samples | Where-Object { $_.group -eq 'exp' })
Stop-App

$samples | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8

Write-Output ''
Write-Output '=================== A/B RESULT ==================='
Write-Output ("completed hide/show cycles: {0} / {1}" -f $cycleOk, $Cycles)
Write-Output ''
Write-Output 'group   n     first-priv  last-priv   last-ws    last-handles'
Write-Output ("control {0,-4} {1,-11} {2,-11} {3,-10} {4}" -f $ctrl.Count, $ctrl[0].priv, $ctrl[-1].priv, $ctrl[-1].ws, $ctrl[-1].h)
Write-Output ("exp     {0,-4} {1,-11} {2,-11} {3,-10} {4}" -f $exp.Count, $exp[0].priv, $exp[-1].priv, $exp[-1].ws, $exp[-1].h)

$delta = [math]::Round($exp[-1].priv - $ctrl[-1].priv, 1)
Write-Output ''
Write-Output ("DELTA (exp.last.priv - control.last.priv) = {0} MB" -f $delta)

# 末端走平判定：实验组最后 60 秒
$tail60 = @($exp | Select-Object -Last 60)
$flat = $false; $handleTrend = $null; $spread = $null
if ($tail60.Count -ge 30) {
    $lo = ($tail60 | Measure-Object priv -Minimum).Minimum
    $hi = ($tail60 | Measure-Object priv -Maximum).Maximum
    $hFirst = $tail60[0].h; $hLast = $tail60[-1].h
    $spread = [math]::Round($hi - $lo, 1)
    Write-Output ("tail-60s priv range = {0} .. {1} MB (spread {2})" -f $lo, $hi, $spread)
    Write-Output ("tail-60s handles  = {0} -> {1}" -f $hFirst, $hLast)
    $flat = ($spread -lt 3.0)
    $handleTrend = $hLast - $hFirst
} else {
    Write-Output 'tail sample too short'
}

# 绝对红线：AGENTS.md 的「常驻内存 ~160 MB 红线」，判定只卡私有内存。
# 2026-10-23 由 132 上调到 160：合并成工具箱后两个模块的数据管道同时常驻，
# 合并后实测私有内存 157 MB。口径不变 —— 两个指标都记，红线只卡私有内存。
$RedLine = 160.0

Write-Output ''
Write-Output '--- verdict (判据写死，避免事后解释) ---'
$reasons = New-Object System.Collections.Generic.List[string]
if ($cycleOk -lt $Cycles) {
    $reasons.Add("hide/show 轮次未跑完 ($cycleOk/$Cycles)")
}
if (-not $flat)        { $reasons.Add("末端未走平（60 秒跨度 $spread MB）") }
# 句柄：泄漏的signature是**持续增长**，不是「有任何变化」。
# 句柄数下降、或小幅上下抖动，都是正常噪声。首版这里写成了「必须完全相等」，
# 结果一次 892->888 的下降被判成 FAIL，方向正好与泄漏相反。
if ($null -ne $handleTrend -and $handleTrend -gt 20) {
    $reasons.Add("末端句柄净增 $handleTrend（泄漏特征）")
}
if ($exp[-1].priv -gt $RedLine) {
    $reasons.Add("末端私有内存 $($exp[-1].priv) MB 超过红线 $RedLine MB")
}
if ($delta -ge 20) { $reasons.Add("相对对照组涨幅 $delta MB >= 20 MB") }

if ($reasons.Count -eq 0) {
    Write-Output ("PASS  真隐藏可接受：相对涨幅 {0} MB < 20、末端走平、句柄无净增、且 {1} MB <= 红线 {2} MB" -f $delta, $exp[-1].priv, $RedLine)
} else {
    Write-Output 'FAIL  触发回退（HideToTray 改回 ::ShowWindow(SW_MINIMIZE)）：'
    foreach ($r in $reasons) { Write-Output "        · $r" }
}
Write-Output ''
Write-Output ("handle trend (tail-60s) = {0}  （>0 才是泄漏方向；下降或抖动属噪声）" -f $handleTrend)
Write-Output ''
Write-Output "工作集不可作为判据：它含 Windows App Runtime 框架 DLL 的跨进程共享页，"
Write-Output "同一应用状态下两次运行可差数倍（AGENTS.md 已记录）。"
Write-Output "CSV=$OutCsv"

# 还原 trace 标记
if (Test-Path (Join-Path $mdir 'trace.on.disabled')) {
    Move-Item (Join-Path $mdir 'trace.on.disabled') (Join-Path $mdir 'trace.on') -Force
    Write-Output '[cleanup] trace.on 已还原'
}
