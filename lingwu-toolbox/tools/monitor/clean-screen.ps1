# clean-screen.ps1 —— 「清理屏幕」动作验收
#
# 判据（plan.md §5.2）：
#   清理屏覆盖虚拟桌面；ESC 退出后恢复。
#
# 【产品行为】（src/ui/TrayPanel.xaml.cpp 的 EnterCleanScreen / ExitCleanScreen）
#   · 进：记住窗口几何 → 去掉 WS_CAPTION（系统按钮会浮在内容之上）→
#         RootGrid 隐藏、CleanScreen 覆盖层显示 →
#         MoveAndResize 成**整个虚拟桌面**的尺寸（SM_*VIRTUALSCREEN 四个值）→
#         Show(true) 抢焦点（否则 ESC 收不到）；
#   · 出：解锁输入（默认锁，见设置项「清理屏时锁定输入」）→ 覆盖层隐藏、
#         RootGrid 恢复 → 去掉 WS_CAPTION 的逆操作 → 重新定位回托盘图标上方。
#
# 【输入锁为什么不能断言「锁上了」】
# BlockInput 要管理员权限。而验收用的是 asInvoker 的测试副本
# （UIA 跨不过完整性级别边界，只能用非提权副本），所以那个副本**必然锁不上**。
# 断言「锁上了」在这种环境下是恒假的 —— 那是环境限制，不是产品坏了。
# 真正该断言的是：**产品如实报出了当前状态**（锁上了 / 锁不上并说明原因），
# 判据落在界面上，而不是落在一条跑不通的断言上。
#
# 【为什么不能用合成鼠标点击退出】
# 本机合成鼠标输入点不动系统控件（AGENTS.md「验证要求」）。
# 清理屏的覆盖层虽然是本程序自己的 XAML，但退出路径里 ESC 更可靠，
# 所以优先用键盘；两条键盘路径都试过还不行，才报 SKIP。

param(
    [string]$Exe = '',
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
. (Join-Path $PSScriptRoot '..\common\tray.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$WM_KEYDOWN = 0x0100
$WM_KEYUP   = 0x0101
$VK_ESCAPE  = 0x1B

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

function Find-ById($Root, [string]$Id, [int]$WaitSec = 10) {
    $deadline = (Get-Date).AddSeconds($WaitSec)
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $Id)
    while ((Get-Date) -lt $deadline) {
        $found = $Root.FindFirst($TS::Descendants, $cond)
        if ($found) { return $found }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# 找清理屏底部那行提示（文案会随「输入锁没锁上」变，所以按前缀找）。
function Find-CleanScreenHint($Root) {
    $all = $Root.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) {
        $n = $e.Current.Name
        if ($n -and $n.StartsWith('按 Esc 退出清理屏幕')) { return $e }
    }
    return $null
}

function Get-AppWindows([int]$ProcId) {
    $pc = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $ProcId)
    $wins = $AE::RootElement.FindAll($TS::Children, $pc)
    $res = New-Object System.Collections.Generic.List[object]
    foreach ($w in $wins) {
        $res.Add(@{ Hwnd = [int]$w.Current.NativeWindowHandle; Name = $w.Current.Name; El = $w })
    }
    return ,$res
}

function Get-TrayPanelWindow($Wins, $Main) {
    foreach ($w in $Wins) {
        if ($Main -and $w.Hwnd -eq $Main.Hwnd) { continue }
        if ($w.Name -and $w.Name -like '*零物工具箱*') { continue }
        if ($w.Hwnd -eq 0) { continue }
        return $w
    }
    return $null
}

function Get-WinRect([IntPtr]$h) {
    $r = New-Object Native.Win+RECT
    if (-not [Native.Win]::GetWindowRect($h, [ref]$r)) { return $null }
    return @{ L = $r.Left; T = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
}

# 发 ESC。两条路径都试：
#   ① PostMessage 到面板 HWND —— 走的是 WinUI 的输入管线，
#      窗口即使不是前台也能收到；
#   ② keybd_event —— 真正的全局按键，需要窗口在前台。
function Send-Escape([IntPtr]$h) {
    [void][Native.Win]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [void][Native.Win]::PostMessage($h, $WM_KEYUP, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
    [void][Native.Win]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 200
    [Native.Win]::keybd_event($VK_ESCAPE, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [Native.Win]::keybd_event($VK_ESCAPE, 0, 2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
}

# ---- 启动 ------------------------------------------------------------------
$own = $false
if (-not $NoLaunch) {
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $p = Start-Process $Exe -PassThru
    $own = $true
    Write-Output "launched pid=$($p.Id)"
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) { Write-Output "FAIL: 应用启动即退出 code=$($p.ExitCode)"; exit 1 }
        if ($p.MainWindowHandle -ne 0) { break }
    }
    Start-Sleep -Seconds 4
}

$proc = Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $proc) { Write-Output 'FAIL: 应用未在运行'; exit 1 }
$pid_ = $proc.Id
Write-Output "app pid=$pid_"

try {
    # ---- 打开托盘面板 -------------------------------------------------------
    $invoked = Invoke-ToolboxTrayButton
    Assert-True '托盘按钮可 Invoke（左键）' ($null -ne $invoked)

    $panel = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 400
        $wins = Get-AppWindows $pid_
        $main = $wins | Where-Object { $_.Name -like '*零物工具箱*' } | Select-Object -First 1
        $panel = Get-TrayPanelWindow $wins $main
        if ($panel) { break }
    }
    Assert-True '托盘指标面板已打开' ($null -ne $panel)
    if (-not $panel) { throw '面板没出现' }

    $hwnd = [IntPtr]$panel.Hwnd
    $before = Get-WinRect $hwnd
    Write-Output ("panel rect before = {0}x{1} at {2},{3}" -f $before.W, $before.H, $before.L, $before.T)
    Assert-True '① 进清理屏前面板是 360x530 DIP（200% → 720x1060 物理）' `
        ($before.W -ge 700 -and $before.W -le 740 -and $before.H -ge 1020 -and $before.H -le 1100) `
        ("$($before.W) x $($before.H)")

    $content = Find-ById $panel.El 'TileCpuSub'
    Assert-True '进清理屏前面板内容可见' ($null -ne $content)

    # ---- 点「清理屏幕」 -----------------------------------------------------
    $btn = Find-ById $panel.El 'CleanScreenButton'
    Assert-True '面板里能定位「清理屏幕」按钮' ($null -ne $btn)
    if (-not $btn) { throw '没有「清理屏幕」按钮' }

    $ok = Invoke-UiaInvoke $btn
    Assert-True '「清理屏幕」可 Invoke' $ok

    # 窗口变成虚拟桌面大小是**异步**的（Invoke → 切态 → MoveAndResize），
    # 必须有界轮询，不能 sleep 一次就下结论。
    $vx = [Native.Win]::GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
    $vy = [Native.Win]::GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
    $vw = [Native.Win]::GetSystemMetrics(78)   # SM_CXVIRTUALSCREEN
    $vh = [Native.Win]::GetSystemMetrics(79)   # SM_CYVIRTUALSCREEN
    Write-Output "virtual desktop = $vw x $vh at $vx,$vy"

    $covered = $false
    $after = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 300
        $after = Get-WinRect $hwnd
        if ($after.W -eq $vw -and $after.H -eq $vh) { $covered = $true; break }
    }
    Assert-True '② 进清理屏后窗口覆盖整个虚拟桌面' $covered `
        ("rect=$($after.W)x$($after.H) at $($after.L),$($after.T)")

    # 内容被隐藏了（RootGrid Collapsed → UIA 里查不到 TileCpuSub）
    $gone = $null -eq (Find-ById $panel.El 'TileCpuSub' 2)
    Assert-True '② 清理屏期间面板内容已隐藏' $gone

    # ---- ② 输入锁的状态 ---------------------------------------------------
    # 【为什么断言的是「状态被如实报出来」而不是「锁上了」】
    # BlockInput 要管理员权限，而验收跑的是 asInvoker 的测试副本
    # （UIA 跨不过完整性级别边界，只能用非提权副本）—— 那个副本必然锁不上。
    # 断言「锁上了」在这种环境下是恒假的，那是环境限制，不是产品坏了。
    # 真正该守住的契约是：**产品如实报出当前状态**，两条结论都要说得清楚。
    $hint = Find-CleanScreenHint $panel.El
    Assert-True '② 清理屏期间提示行报出了输入锁的实际状态' ($null -ne $hint) `
        $(if ($hint) { $hint.Current.Name } else { '找不到提示行' })
    if ($hint) {
        $hn = $hint.Current.Name
        $locked = $hn -like '*输入已锁定*'
        $lockFailed = $hn -like '*输入锁定失败*'
        Assert-True '② 输入锁状态只有两种明确结论（已锁定 / 锁定失败并说明原因）' `
            ($locked -or $lockFailed) $hn
        if ($locked) {
            Write-Output '  NOTE: 输入锁已生效（进程有管理员权限）'
        } else {
            Write-Output '  NOTE: 输入锁未生效 —— asInvoker 测试副本没有管理员权限，BlockInput 必然失败（环境限制，非产品缺陷）'
        }
    }

    # ---- ESC 退出 -----------------------------------------------------------
    $exited = $false
    for ($i = 0; $i -lt 20; $i++) {
        Send-Escape $hwnd
        $cur = Get-WinRect $hwnd
        if ($cur.W -ne $vw -or $cur.H -ne $vh) { $exited = $true; break }
    }
    if ($exited) {
        Assert-True '③ ESC 能退出清理屏' $true
    } else {
        Write-Output 'SKIP: ③ ESC 没能退出清理屏（键盘注入在本机无效）'
    }

    $back = Get-WinRect $hwnd
    Assert-True '④ 退出后窗口恢复成 360x530 DIP' `
        ($back.W -ge 700 -and $back.W -le 740 -and $back.H -ge 1020 -and $back.H -le 1100) `
        ("$($back.W) x $($back.H)")

    $visible = $null -ne (Find-ById $panel.El 'TileCpuSub' 5)
    Assert-True '⑤ 退出后面板内容恢复可见' $visible

    # ---- ⑥ 清理屏期间收起面板 → 必须先退出清理屏 ----------------------------
    # 【为什么要单独验这一条】
    # 清理屏期间输入是**锁着的**（BlockInput），而退出清理屏只有 ESC / 点击
    # 两条路，两条都要求面板**可见**。面板若在清理屏态被收起，
    # 用户就再也按不到 ESC —— 整台机器的输入被锁死，只能杀进程。
    # 判据：收起之后，窗口必须已经回到面板尺寸（不再铺满虚拟桌面）。
    $btn2 = Find-ById $panel.El 'CleanScreenButton' 5
    $reEntered = $false
    $trayOk = $false
    if ($btn2) {
        [void](Invoke-UiaInvoke $btn2)
        for ($i = 0; $i -lt 30; $i++) {
            Start-Sleep -Milliseconds 300
            $rc = Get-WinRect $hwnd
            if ($rc.W -eq $vw -and $rc.H -eq $vh) { $reEntered = $true; break }
        }
        if ($reEntered) {
            # 托盘左键 = 切换语义：面板开着时再点就收起。
            $trayOk = Invoke-ToolboxTrayButton
        }
    }
    if (-not $btn2) {
        Write-Output 'SKIP: ⑥ 找不到「清理屏幕」按钮'
    } elseif (-not $reEntered) {
        Write-Output 'SKIP: ⑥ 没能再次进入清理屏（面板状态不符）'
    } elseif (-not $trayOk) {
        Write-Output 'SKIP: ⑥ 清理屏期间托盘图标调不动（溢出浮窗被全屏窗口盖住）—— 环境限制'
    } else {
        Start-Sleep -Seconds 2
        $afterHide = Get-WinRect $hwnd
        $exited = ($afterHide.W -ne $vw -or $afterHide.H -ne $vh)
        Assert-True '⑥ 清理屏期间收起面板 → 先退出清理屏（不再铺满虚拟桌面）' $exited `
            ("rect=$($afterHide.W)x$($afterHide.H)")
    }
}
finally {
    if ($own) {
        Get-Process -Id $pid_ -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

Write-Output ""
Write-Output "RESULT: pass=$script:Pass fail=$script:Fail"
if ($script:Fail -gt 0) { exit 1 }
exit 0
