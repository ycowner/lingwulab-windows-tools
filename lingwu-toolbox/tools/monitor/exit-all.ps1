# exit-all.ps1 —— 退出序列回归：所有窗口都关 → 进程真正消失
#
# 判据（plan.md §5.2 + 风险登记第 1 条）：
#   「多顶层窗口导致进程退不干净」是本方案最大的风险 ——
#   WinUI 3 的 Window 只要还有一个活着，XAML 框架的消息循环就不会结束，
#   进程会变成「窗口全没了但进程还在」的僵留状态（托盘图标也没了、
#   任务管理器里还在）。本脚本专门盯这一条。
#
# 【为什么要先打开托盘面板再退出】
# 只测「主窗口 → 退出」测不到「多一个顶层窗口」这个场景。
# 托盘指标面板是本轮新增的顶层窗口，它必须被连带关闭。
#
# 【为什么用 WM_CLOSE 而不是点 X】
# 两者走同一条 AppWindow.Closing 路径（AGENTS.md 踩坑 7 已证实
# 「SetWindowSubclass 拦 WM_CLOSE 拦不住」，反过来说明 WM_CLOSE 进了 XAML 层）。
# 而且点 X 需要合成鼠标输入 —— 本机实测合成鼠标点不动系统控件。
#
# 【为什么要改 CloseAction】
# 只有 CloseAction=0（每次询问）才会弹确认框，那是**走完整个退出路径**的
# 情形。脚本在启动前把它强制成 0，结束时**还原**成原值 ——
# 验收脚本不许把用户设置留在测试值上（apply.ps1 踩过这个坑，
# 还原失败把用户的 ini 留在了测试值上）。

param(
    [string]$Exe = '',
    [switch]$NoLaunch,
    [int]$TimeoutSec = 40
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
. (Join-Path $PSScriptRoot '..\common\tray.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
'@

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$WM_CLOSE = 0x0010

$script:Pass = 0
$script:Fail = 0
function Assert-True([string]$What, [bool]$Cond, [string]$Extra = '') {
    if ($Cond) {
        $script:Pass++
        Write-Output ("  [PASS] {0} {1}" -f $What, $Extra)
    } else {
        $script:Fail++
        Write-Output ("  [FAIL] {0} {1}" -f $What, $Extra)
    }
}

# 进程下的顶层窗口（含不可见的）。**按 ProcessId 过滤** ——
# 桌面上可能有别的进程同名窗口，只按类名找会认错。
function Get-AppWindows([int]$ProcId) {
    $pc = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $ProcId)
    $wins = $AE::RootElement.FindAll($TS::Children, $pc)
    $res = New-Object System.Collections.Generic.List[object]
    foreach ($w in $wins) {
        $res.Add(@{
            Hwnd = [int]$w.Current.NativeWindowHandle
            Name = $w.Current.Name
            El   = $w
        })
    }
    return ,$res
}

function Get-MainWindow($Wins) {
    foreach ($w in $Wins) {
        if ($w.Name -and $w.Name -like '*零物工具箱*') { return $w }
    }
    return $null
}

# 指标面板：进程下**没有标题**的那个额外顶层窗口。
# （托盘面板是 WS_EX_TOOLWINDOW 的工具窗，没有设标题。）
function Get-TrayPanelWindow($Wins, $Main) {
    foreach ($w in $Wins) {
        if ($Main -and $w.Hwnd -eq $Main.Hwnd) { continue }
        if ($w.Name -and $w.Name -like '*零物工具箱*') { continue }
        if ($w.Hwnd -eq 0) { continue }
        return $w
    }
    return $null
}

# 在进程下按名字找一个元素（按钮、复选框……）。有界轮询。
function Find-ElByName([int]$ProcId, [string]$Name, [int]$WaitSec = 20) {
    $deadline = (Get-Date).AddSeconds($WaitSec)
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $Name)
    while ((Get-Date) -lt $deadline) {
        $pc = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $ProcId)
        $wins = $AE::RootElement.FindAll($TS::Children, $pc)
        foreach ($w in $wins) {
            $found = $w.FindFirst($TS::Descendants, $cond)
            if ($found) { return $found }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# ---- ini：只碰 CloseAction 一个键 ------------------------------------------------
$IniPath = Join-Path $env:APPDATA 'LingwuToolbox\settings.ini'

function Get-CloseAction {
    if (-not (Test-Path $IniPath)) { return 0 }
    $m = Select-String -Path $IniPath -Pattern '^\s*CloseAction\s*=\s*(\d+)' -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return 0
}

function Set-CloseAction([int]$v) {
    if (-not (Test-Path $IniPath)) {
        New-Item -ItemType Directory -Path (Split-Path $IniPath) -Force | Out-Null
        Set-Content -Path $IniPath -Value "[Settings]`r`nCloseAction=$v" -Encoding ASCII
        return
    }
    $text = Get-Content $IniPath -Raw
    if ($text -match '(?m)^\s*CloseAction\s*=') {
        $text = $text -replace '(?m)^\s*CloseAction\s*=\s*\d+', "CloseAction=$v"
        Set-Content -Path $IniPath -Value $text -Encoding UTF8 -NoNewline
    } else {
        Add-Content -Path $IniPath -Value "CloseAction=$v" -Encoding UTF8
    }
}

$origCloseAction = Get-CloseAction
Write-Output "ini=$IniPath  CloseAction(before)=$origCloseAction"

# ---- 启动 --------------------------------------------------------------------------
$own = $false
if (-not $NoLaunch) {
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    # 强制走「每次询问」路径 —— 那是走完整个退出序列的情形。
    Set-CloseAction 0
    Write-Output "ini CloseAction(forced)=$(Get-CloseAction)"
    $p = Start-Process $Exe -PassThru
    $own = $true
    Write-Output "launched pid=$($p.Id)"
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) { Write-Output "FAIL: 应用启动即退出 code=$($p.ExitCode)"; exit 1 }
        if ($p.MainWindowHandle -ne 0) { break }
    }
    Start-Sleep -Seconds 4   # 等托盘图标注册完
}

$proc = Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $proc) { Write-Output 'FAIL: 应用未在运行'; exit 1 }
$pid_ = $proc.Id
Write-Output "app pid=$pid_"

try {
    # ---- ① 主窗口存在 -------------------------------------------------------------
    $wins = Get-AppWindows $pid_
    $main = Get-MainWindow $wins
    Assert-True '① 主窗口存在' ($null -ne $main) ("count=" + $wins.Count)

    # ---- ② 打开托盘指标面板（多出来的那个顶层窗口）--------------------------------
    $invoked = Invoke-ToolboxTrayButton
    Assert-True '托盘按钮可 Invoke（左键）' ($null -ne $invoked)
    $panel = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 400
        $wins = Get-AppWindows $pid_
        $panel = Get-TrayPanelWindow $wins (Get-MainWindow $wins)
        if ($panel) { break }
    }
    Assert-True '② 托盘指标面板已打开（多一个顶层窗口）' ($null -ne $panel) `
        $(if ($panel) { "hwnd=$($panel.Hwnd)" } else { "" })

    # ---- ③ 走退出路径 -------------------------------------------------------------
    # 关闭主窗口 → 确认框 → 「退出程序」。
    if ($main) {
        [void][Native.Win]::PostMessage([IntPtr]$main.Hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    $exitBtn = Find-ElByName $pid_ '退出程序' 20
    if ($exitBtn) {
        Assert-True '③ 关闭确认框出现且有「退出程序」' $true
        $ok = Invoke-UiaInvoke $exitBtn
        Assert-True '③ 「退出程序」可 Invoke' $ok
    } else {
        # CloseAction 不是「每次询问」时没有确认框，直接退也算合法路径。
        Write-Output 'SKIP: 没有出现退出确认框（CloseAction 不是每次询问），直接等退出'
    }

    # ---- ④ 所有顶层窗口都消失 -----------------------------------------------------
    # 这是本脚本的核心判据：**窗口归零但进程还在**就是僵尸态。
    $windowsGone = $false
    $lastCount = -1
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $left = Get-AppWindows $pid_
        $lastCount = $left.Count
        if ($lastCount -eq 0) { $windowsGone = $true; break }
        Start-Sleep -Milliseconds 400
    }
    Assert-True '④ 退出后进程下**没有任何顶层窗口**' $windowsGone ("剩余窗口数=" + $lastCount)

    # ---- ⑤ 进程真正消失（不是僵尸）------------------------------------------------
    $gone = $false
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Id $pid_ -ErrorAction SilentlyContinue)) { $gone = $true; break }
        Start-Sleep -Milliseconds 400
    }
    $still = Get-Process -Id $pid_ -ErrorAction SilentlyContinue
    Assert-True '⑤ 进程真正退出（没有留下僵尸进程）' $gone `
        $(if ($still) { "pid=$pid_ 仍在，WS=$([int]$still.WorkingSet64) 私有=$([int]$still.PrivateMemorySize64)" } else { "" })

    # ---- ⑥ 托盘图标也消失了 -------------------------------------------------------
    $iconGone = -not (Test-ToolboxTrayIconPresent)
    Assert-True '⑥ 托盘图标已消失' $iconGone
}
finally {
    if ($own) {
        Get-Process -Id $pid_ -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    # 还原 CloseAction —— 验收脚本不许把用户设置留在测试值上。
    if (-not $NoLaunch) {
        Set-CloseAction $origCloseAction
        Write-Output "ini CloseAction(restored)=$(Get-CloseAction)"
    }
}

Write-Output ""
Write-Output "RESULT: pass=$script:Pass fail=$script:Fail"
if ($script:Fail -gt 0) { exit 1 }
exit 0
