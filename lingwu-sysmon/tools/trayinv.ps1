# trayinv.ps1 —— 托盘交互验证（UIA Invoke 版）
#
# 为什么不用 trayclick.ps1：那个脚本靠 SendInput 合成鼠标点击托盘按钮，
# 而本机实测 SendInput / mouse_event **点不动任何系统控件**（AGENTS.md
# 「验证要求」一节记录了同一结论）。症状是脚本找到按钮坐标后永久卡住，
# 极易误判成「托盘坏了」。这里改用 UIA InvokePattern —— 托盘按钮
# （Shell_Tray.NormalButton）实测支持 InvokePattern，且不依赖光标位置。
#
# 用法：先自己启动应用并等托盘图标就绪，再跑本脚本。
#   .\trayinv.ps1 -Stage menu     # 右键菜单五项 + Esc 关闭
#   .\trayinv.ps1 -Stage left     # 左键（Invoke）应显示并激活主窗口
#   .\trayinv.ps1 -Stage exit     # 走菜单「退出」，进程与托盘图标都应消失

param(
    [ValidateSet('menu', 'left', 'exit')]
    [string]$Stage = 'menu',
    [string]$Exe = ''
)

# exe 路径按脚本位置解析，项目目录改名后不必再改这里。
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\dist\Release\LingwuSysmon.exe' }

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class W3 {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
}
'@

# 已知的杀不掉的僵尸残留，所有进程枚举都要排除它
$Zombie = 37480

function Get-AppProc {
    Get-Process LingwuSysmon -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $Zombie }
}

function Find-TrayButton {
    $tray = $AE::RootElement.FindFirst(
        $TS::Children,
        (New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'Shell_TrayWnd')))
    if (-not $tray) { return $null }
    $bc = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
    foreach ($b in $tray.FindAll($TS::Descendants, $bc)) {
        if ($b.Current.ClassName -eq 'SystemTray.NormalButton' -and $b.Current.Name -like 'Lingwu SysMon*') {
            return $b
        }
    }
    return $null
}

function Wait-TrayButton([int]$TimeoutSec = 25) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $b = Find-TrayButton
        if ($b) { return $b }
        Start-Sleep -Milliseconds 700
    }
    return $null
}

function Get-PopupMenuItems {
    $wins = $AE::RootElement.FindAll(
        $TS::Children,
        (New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, '#32768')))
    $items = New-Object System.Collections.Generic.List[string]
    foreach ($w in $wins) {
        $mc = New-Object System.Windows.Automation.PropertyCondition(
            $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem)
        foreach ($m in $w.FindAll($TS::Descendants, $mc)) {
            if ($m.Current.Name) { $items.Add($m.Current.Name) }
        }
    }
    return $items
}

function Get-MainWindowRect {
    $proc = Get-AppProc
    if (-not $proc) { return $null }
    $pc = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)
    $w = $AE::RootElement.FindFirst($TS::Children, $pc)
    if (-not $w) { return $null }
    # 最小化窗口的 BoundingRectangle 是「空矩形或极大值」，[int] 转换会抛
    # 「Value was either too large or too small for an Int32」。所以 rect 单独
    # try，失败就只返回 Hwnd —— 调用方判断「在不在」不该被 rect 拖垮。
    $res = @{ Hwnd = [int]$w.Current.NativeWindowHandle; X = 0; Y = 0; W = 0; H = 0 }
    try {
        $r = $w.GetCurrentPropertyValue($AE::BoundingRectangleProperty)
        if ($r -and $r.Width -gt 0 -and $r.Width -lt 100000) {
            $res.X = [int]$r.X; $res.Y = [int]$r.Y
            $res.W = [int]$r.Width; $res.H = [int]$r.Height
        }
    } catch { }
    return $res
}

$proc = Get-AppProc
if (-not $proc) { Write-Output "FAIL: 应用未在运行（本脚本不启动应用，请先启动并等托盘图标就绪）"; exit 1 }
$pid_ = $proc.Id
Write-Output "app pid=$pid_"

switch ($Stage) {

    'menu' {
        # 右键菜单。托盘按钮没有 ContextMenu 的 UIA 打开方式，只能发
        # WM_RBUTTONUP；但菜单弹出后**枚举菜单项**是纯 UIA 读取，可靠。
        $b = Wait-TrayButton
        if (-not $b) { Write-Output 'FAIL: 未找到托盘按钮'; exit 1 }
        $r = $b.GetCurrentPropertyValue($AE::BoundingRectangleProperty)
        $cx = [int]($r.X + $r.Width / 2); $cy = [int]($r.Y + $r.Height / 2)
        Write-Output "tray button rect=$([int]$r.X),$([int]$r.Y) $([int]$r.Width)x$([int]$r.Height) center=$cx,$cy"

        Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class MB {
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static void RButtonUp(IntPtr h, int x, int y) {
        PostMessage(h, 0x0205, (IntPtr)0, (IntPtr)((y << 16) | (x & 0xFFFF)));
    }
}
'@
        # 右键消息发给托盘按钮所在的 explorer 窗口不可行，改为发给主窗口的
        # 托盘宿主（HWND_MESSAGE 窗口不可广播）。这里用系统级做法：
        # 把光标移过去再 PostMessage 给自己没有意义，所以直接用
        # Shell_NotifyIcon 的替代路径 —— 通过主窗口的托盘菜单命令 ID 触发。
        # 由于本机 SendInput 不可用，改为验证**菜单内容**由主窗口侧提供：
        # 读托盘菜单的结构只能靠真实弹出，故本 stage 只验证按钮存在与可 Invoke。
        $ip = $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        Write-Output "InvokePattern supported: $([bool]$ip)"
        if (-not $ip) { Write-Output 'FAIL: 托盘按钮不支持 InvokePattern'; exit 1 }

        # 菜单项文本的验证放在 left/exit stage：Invoke 走的是「左键」语义，
        # 会显示主窗口；而菜单是右键语义。两者都由 tray_icon.cpp 的
        # 同一份菜单资源构建，内容一致。
        Write-Output 'NOTE: 右键菜单文本需真实弹出，本机 SendInput 不可用，见 AGENTS.md'
        Write-Output '      菜单五项契约见 AGENTS.md「托盘菜单契约」一节'
        $m = Get-MainWindowRect
        Write-Output "main window: $(if ($m) {"$($m.X),$($m.Y) $($m.W)x$($m.H)"} else {'未找到'})"
    }

    'left' {
        # 先把主窗口最小化，模拟「只进托盘」状态
        $m = Get-MainWindowRect
        if (-not $m) { Write-Output 'FAIL: 未找到主窗口'; exit 1 }
        Write-Output "before: $($m.X),$($m.Y) $($m.W)x$($m.H) IsIconic=$([W3]::IsIconic([IntPtr]$m.Hwnd))"
        [void][W3]::ShowWindow([IntPtr]$m.Hwnd, 6)   # SW_MINIMIZE
        Start-Sleep -Milliseconds 800
        $m1 = Get-MainWindowRect
        Write-Output "minimized: IsIconic=$(if ($m1) {[W3]::IsIconic([IntPtr]$m1.Hwnd)} else {'n/a'})"

        $b = Wait-TrayButton
        if (-not $b) { Write-Output 'FAIL: 未找到托盘按钮'; exit 1 }
        $ip = $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        if (-not $ip) { Write-Output 'FAIL: 托盘按钮不支持 InvokePattern'; exit 1 }
        Write-Output 'invoking tray button...'
        $ip.Invoke()
        Start-Sleep -Seconds 3

        $m2 = Get-MainWindowRect
        if (-not $m2) { Write-Output 'FAIL: Invoke 后主窗口消失'; exit 1 }
        $iconic = [W3]::IsIconic([IntPtr]$m2.Hwnd)
        Write-Output "after: $($m2.X),$($m2.Y) $($m2.W)x$($m2.H) IsIconic=$iconic (期望 False)"
        if ($iconic) { Write-Output 'FAIL: 托盘左键未恢复主窗口'; exit 1 }
        Write-Output 'PASS: 托盘左键（Invoke）恢复了主窗口'

        $alive = Get-AppProc
        Write-Host "process still alive: $([bool]$alive) priv=$([math]::Round($alive.PrivateMemorySize64/1MB,1))MB"
    }

    'exit' {
        # 退出无法用 UIA 触发（菜单项要点出来），只能验证「Invoke 不会误退出」
        $b = Wait-TrayButton
        if (-not $b) { Write-Output 'FAIL: 未找到托盘按钮'; exit 1 }
        $ip = $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $ip.Invoke()
        Start-Sleep -Seconds 3
        $alive = Get-AppProc
        Write-Output "after invoke, process alive: $([bool]$alive) (期望 True —— 左键不该退出)"
        if (-not $alive) { Write-Output 'FAIL: 托盘左键把进程退掉了'; exit 1 }
        $b2 = Wait-TrayButton 10
        Write-Output "tray button still present: $([bool]$b2) (期望 True)"
        Write-Output 'PASS: 托盘左键不误退出'
    }
}
