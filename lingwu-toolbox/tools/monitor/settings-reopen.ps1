param(
    [string]$Exe,
    [string]$Prefix = 'set2',
    [int]$Rounds = 10,          # 「开→关」循环多少轮（顺带量内存阶跃）
    [double]$GrowthLimitMB = 25 # 私有内存允许的增长上限（MB）
)

# 设置窗口「二次打开」回归。
#
# 【为什么必须有这个脚本】
# 2026-10-20 的崩溃：托盘右键菜单点「设置…」→ 进程**必现**退出。
# 根因是 WinUI 3 的 Window 关闭后不可复用 —— 原代码用 if (!settingsWindow_)
# 判断「要不要新建」，而窗口关闭后该成员仍然非空，于是第二次打开时对着一
# 个已失效的窗口调 Activate()，抛出的 hresult_error 一路穿过托盘的
# LRESULT CALLBACK 窗口过程 → std::terminate → abort() → 0xC0000409。
#
# 既有 uisettings.ps1 **只开一次**设置窗口就关闭、从不重开，所以这条
# （也是唯一能触发崩溃的）路径从来没有被任何测试覆盖过。
#
# 【等价路径论证】
# 托盘菜单「设置…」与主窗口的「设置」按钮调用的是**同一个**
# MainWindow::OnOpenSettings()：前者经 g_cb.onSettings，后者经
# SettingsButton().Click。所以用主窗口按钮做自动回归，等价覆盖托盘路径
# 的根因。托盘右键菜单本身的 live 验证仍是已知缺口（本机 SendInput 点不动
# 系统控件），需要人工确认，见 AGENTS.md「验证要求」。
#
# 【设计约束（照 uisettings.ps1 踩过的坑）】
#   * 跨进程找元素一律 AndCondition(ControlType, ProcessId)，绝不只用
#     ControlType 在桌面根 FindAll 再按名字 -like 过滤 —— 那会点到别的应用
#     的控件，产生真实副作用。
#   * 只点「取消」：不碰自启动开关（它写 HKCU，是跨重启的持久副作用），
#     也不点「确定」（那会改 ini）。
#   * 断言全部走 UIA 读回的状态 + 进程存活 + 私有内存，不靠肉眼。
#
# 用法：pwsh -NoProfile -File .\tools\monitor\settings-reopen.ps1
# （本文件含中文，必须用 pwsh；Windows PowerShell 5.1 按 ANSI 936 读会碎掉。）

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe
Write-Host "exe = $Exe"

# UIA 程序集必须显式加载 —— Windows PowerShell / PowerShell 7 都不会自动
# 带上 UIAutomationClient，漏了会报「找不到类型 [AutomationElement]」。
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$BT = [System.Windows.Automation.ControlType]::Button

$Ini = Join-Path $env:APPDATA 'LingwuToolbox\settings.ini'
$IniBackup = "$Ini.set2bak"

# 保留用户原始 ini，测完还原。绝不在用户机器上留下我们编的值。
$hadIni = Test-Path $Ini
if ($hadIni) { Copy-Item $Ini $IniBackup -Force }
Write-Host ("ini exists before test = $hadIni")

Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$p = Start-Process $Exe -PassThru
Write-Host "launched pid=$($p.Id)"

function Get-TopWins($pid_) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    # The comma stops PowerShell from unrolling the collection: with exactly
    # one window in the tree, an unrolled return hands back the element
    # itself, and $w.Count then fails with "property Count not found".
    return ,$AE::RootElement.FindAll($TS::Children, $cond)
}

function Get-MainWin($pid_) {
    for ($i = 0; $i -lt 60; $i++) {
        $w = Get-TopWins $pid_
        if ($w.Count -gt 0) { return $w[0] }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# 设置窗口是第二个顶层窗口，所以按「非主窗口」挑。
function Get-SettingsWin($pid_, $mainHwnd, [int]$timeoutSec = 20) {
    for ($i = 0; $i -lt ($timeoutSec * 5); $i++) {
        $all = Get-TopWins $pid_
        foreach ($w in $all) {
            if ($w.Current.NativeWindowHandle -ne $mainHwnd) { return $w }
        }
        Start-Sleep -Milliseconds 200
    }
    return $null
}

# 关闭之后要确认它**真的**不见了（不是还在那儿半死不活）。
function Wait-SettingsGone($pid_, $mainHwnd, [int]$timeoutSec = 20) {
    for ($i = 0; $i -lt ($timeoutSec * 5); $i++) {
        $all = Get-TopWins $pid_
        $extra = 0
        foreach ($w in $all) {
            if ($w.Current.NativeWindowHandle -ne $mainHwnd) { $extra++ }
        }
        if ($extra -eq 0) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Count-TopWins($pid_, $mainHwnd) {
    $n = 0
    foreach ($w in Get-TopWins $pid_) {
        if ($w.Current.NativeWindowHandle -ne $mainHwnd) { $n++ }
    }
    return $n
}

function Find-Id($root, [string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    return $root.FindFirst($TS::Descendants, $c)
}

# 主窗口的「设置」按钮。先在主窗口子树里找，找不到再全进程找
# （设置窗口里的按钮叫「确定」「取消」，不会撞名）。
function Find-SettingsButton($win, $pid_) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $BT)
    foreach ($b in $win.FindAll($TS::Descendants, $c)) {
        if ($b.Current.Name -eq '设置') { return $b }
    }
    $c2 = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    foreach ($b in $AE::RootElement.FindAll($TS::Descendants, $c)) {
        if ($b.Current.Name -eq '设置') { return $b }
    }
    return $null
}

function Get-PrivateMB($proc) {
    $proc.Refresh()
    return [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)
}

$fail = 0
function Fail([string]$msg) {
    Write-Host "FAIL: $msg"
    $script:fail++
}
function Pass([string]$msg) {
    Write-Host "PASS: $msg"
}

$win = Get-MainWin $p.Id
if (-not $win) { Write-Host 'MAIN WINDOW NOT FOUND'; exit 1 }
$mainHwnd = $win.Current.NativeWindowHandle
Write-Host "main window hwnd=$mainHwnd"
Start-Sleep -Seconds 12   # 等列表刷出第一帧，和 uisettings.ps1 一致

$settingsBtn = Find-SettingsButton $win $p.Id
if (-not $settingsBtn) { Write-Host 'SETTINGS BUTTON NOT FOUND'; exit 1 }
Write-Host ("settings button found: '" + $settingsBtn.Current.Name + "'")

function Open-Settings {
    $settingsBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

# ---- 1) 第一次打开：基线 ---------------------------------------------------
Open-Settings
$sw = Get-SettingsWin $p.Id $mainHwnd
if (-not $sw) {
    Fail 'first open: settings window not found'
} else {
    Pass "first open: settings window appeared (hwnd=$($sw.Current.NativeWindowHandle))"
}

$p.Refresh()
if ($p.HasExited) { Fail 'process died after first open'; exit 1 }

# ---- 2) 已开着时再点一次：只激活，不叠窗口 ---------------------------------
$before = Count-TopWins $p.Id $mainHwnd
Open-Settings
Start-Sleep -Seconds 2
$after = Count-TopWins $p.Id $mainHwnd
if ($after -eq $before) { Pass "reopen while open: no extra window (count=$after)" }
else { Fail "reopen while open: window count $before -> $after (should not stack)" }

$p.Refresh()
if ($p.HasExited) { Fail 'process died after reopen-while-open'; exit 1 }

# ---- 3) 关闭 ----------------------------------------------------------------
$sw = Get-SettingsWin $p.Id $mainHwnd
if (-not $sw) {
    Fail 'settings window missing before close'
} else {
    $cancelBtn = Find-Id $sw 'CancelButton'
    if (-not $cancelBtn) {
        Fail 'CancelButton not found'
    } else {
        $cancelBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        Start-Sleep -Seconds 2
    }
}
if (Wait-SettingsGone $p.Id $mainHwnd) { Pass 'settings window closed' }
else { Fail 'settings window still present after close' }

$p.Refresh()
if ($p.HasExited) { Fail 'process died after close'; exit 1 }

# ---- 4) 第二次打开 —— 这就是 2026-10-20 那个必现崩溃的触发点 ---------------
Write-Host '--- SECOND OPEN (the crash trigger) ---'
Open-Settings
Start-Sleep -Seconds 3

$p.Refresh()
if ($p.HasExited) {
    Fail 'CRASH REPRODUCED: process exited on second open'
    exit 1
}
Pass 'process alive after second open'

$sw2 = Get-SettingsWin $p.Id $mainHwnd
if (-not $sw2) {
    Fail 'second open: settings window not found'
    exit 1
}
Pass "second open: settings window appeared again (hwnd=$($sw2.Current.NativeWindowHandle))"

# 关掉它，继续后面的循环
$cancelBtn = Find-Id $sw2 'CancelButton'
if ($cancelBtn) {
    $cancelBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 2
}
[void](Wait-SettingsGone $p.Id $mainHwnd)

# ---- 5) 反复开关：顺带量私有内存有没有阶跃 ---------------------------------
$memStart = Get-PrivateMB $p
Write-Host "private memory before loop = $memStart MB"

for ($i = 1; $i -le $Rounds; $i++) {
    Open-Settings
    Start-Sleep -Milliseconds 1200
    $p.Refresh()
    if ($p.HasExited) { Fail "CRASH REPRODUCED at round $i"; exit 1 }
    $s = Get-SettingsWin $p.Id $mainHwnd 10
    # 注意 ${i}: 的写法 —— "$i:" 会被 PowerShell 当成驱动器限定变量
    # （$i: 表示「i 盘上的变量」），直接写会报「没有有效的文本终止符」。
    if (-not $s) { Fail "round ${i}: settings window not found"; exit 1 }
    $c = Find-Id $s 'CancelButton'
    if ($c) {
        $c.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        Start-Sleep -Milliseconds 800
    }
    [void](Wait-SettingsGone $p.Id $mainHwnd 10)
    Write-Host ("  round $i ok, private = " + (Get-PrivateMB $p) + " MB")
}

$memEnd = Get-PrivateMB $p
$growth = [math]::Round($memEnd - $memStart, 1)
Write-Host "private memory after loop = $memEnd MB (growth = $growth MB over $Rounds rounds)"
if ($growth -le $GrowthLimitMB) {
    Pass "private memory growth $growth MB <= limit $GrowthLimitMB MB"
} else {
    Fail "private memory growth $growth MB > limit $GrowthLimitMB MB"
}

# ---- 6) 收尾：还原 ini、关掉进程 -------------------------------------------
if ($hadIni) {
    Copy-Item $IniBackup $Ini -Force
    Write-Host 'ini restored from backup'
} elseif (Test-Path $Ini) {
    Remove-Item $Ini -Force
    Write-Host 'ini removed (did not exist before test)'
}
# 备份文件本身也要清掉，不能留在用户的 AppData 里。
if (Test-Path $IniBackup) {
    Remove-Item $IniBackup -Force
    Write-Host 'ini backup removed'
}

Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($fail -gt 0) {
    Write-Host "RESULT: FAIL ($fail problem(s))"
    exit 1
}
Write-Host 'RESULT: PASS'
exit 0
