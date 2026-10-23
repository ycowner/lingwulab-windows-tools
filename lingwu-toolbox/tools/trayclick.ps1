# 真实点击托盘图标，验证右键菜单 / 左键唤主窗 / X=最小化 / 真退出 / 单实例。
# 用 UIA 定位 SystemTray.NormalButton，用 SendInput 真实点击（UIA Invoke 不支持右键）。
param(
    [ValidateSet('rightmenu','leftclick','xminimize','exit','second','settings','aggregate')]
    [string]$Stage = 'rightmenu'
)

$ErrorActionPreference = 'Stop'
Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;

public class In {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint n, INPUT[] i, int cb);

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public HARDWAREINPUT hi;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL, wParamH; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public int type; public INPUTUNION u; }

    public const uint MOUSEEVENTF_MOVE       = 0x0001;
    public const uint MOUSEEVENTF_ABSOLUTE   = 0x8000;
    public const uint MOUSEEVENTF_LEFTDOWN  = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP    = 0x0004;
    public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    public const uint MOUSEEVENTF_RIGHTUP   = 0x0010;
    public const uint KEYEVENTF_KEYUP       = 0x0002;
    public const ushort VK_ESCAPE           = 0x1B;

    public static void ClickAt(int x, int y, bool right) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(80);
        uint dn = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        uint up = right ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;
        INPUT[] a = new INPUT[2];
        a[0].type = 0; a[0].u.mi.dx = 0; a[0].u.mi.dy = 0; a[0].u.mi.dwFlags = dn; a[0].u.mi.dwExtraInfo = IntPtr.Zero;
        a[1].type = 0; a[1].u.mi.dx = 0; a[1].u.mi.dy = 0; a[1].u.mi.dwFlags = up; a[1].u.mi.dwExtraInfo = IntPtr.Zero;
        if (SendInput(2, a, Marshal.SizeOf(typeof(INPUT))) != 2) throw new Exception("SendInput mouse failed");
        System.Threading.Thread.Sleep(80);
    }

    public static void Key(ushort vk) {
        INPUT[] a = new INPUT[2];
        a[0].type = 1; a[0].u.ki.wVk = vk; a[0].u.ki.wScan = 0; a[0].u.ki.dwFlags = 0;
        a[1].type = 1; a[1].u.ki.wVk = vk; a[1].u.ki.wScan = 0; a[1].u.ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(2, a, Marshal.SizeOf(typeof(INPUT))) != 2) throw new Exception("SendInput key failed");
        System.Threading.Thread.Sleep(120);
    }

    public static int[] Cursor() {
        POINT p; GetCursorPos(out p); return new int[] { p.X, p.Y };
    }
}
'@
Add-Type -AssemblyName UIAutomationClient

# ShowWindow / IsIconic 单独提出来：各个 stage 都要用，且在 Get-MainWindowRect
# 返回 null 的分支里也得能调到（曾经内联在 if 块里，走了另一条分支就未定义）。
Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
}
'@

# Alt+F4：点标题栏 X 的键盘等价物，走同一条 AppWindow.Closing 路径。
Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class W2 {
    [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] i, int cb);
    [StructLayout(LayoutKind.Explicit)] public struct IU { [FieldOffset(0)] public MI mi; [FieldOffset(0)] public KI ki; }
    [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx,dy; public uint md,df,t; public IntPtr e; }
    [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk,sc; public uint df,t; public IntPtr e; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public int type; public IU u; }
    public static void Send(ushort vk, uint flags) {
        INPUT[] a = new INPUT[1]; a[0].type = 1; a[0].u.ki.vk = vk; a[0].u.ki.sc = 0; a[0].u.ki.df = flags; a[0].u.ki.e = IntPtr.Zero;
        SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

# 当前被测进程 pid。由各 stage 自己决定怎么取（默认排除已知的僵尸残留）。
$script:AppPid = 0

$script:AppExe = Join-Path $PSScriptRoot '..\dist\Release\LingwuSysmon.exe'
$script:TracePath = Join-Path $env:LOCALAPPDATA 'LingwuSysMon\trace.log'

function Get-Trace {
    if (Test-Path $script:TracePath) {
        [System.IO.File]::ReadAllText($script:TracePath, [System.Text.Encoding]::UTF8)
    } else { '' }
}
function Trace-Len { (Get-Trace).Length }

function Find-TrayButton {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Shell_TrayWnd')
    $tray = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
    if (-not $tray) { return $null }
    $btnCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    foreach ($b in $tray.FindAll([System.Windows.Automation.TreeScope]::Descendants, $btnCond)) {
        if ($b.Current.ClassName -eq 'SystemTray.NormalButton' -and $b.Current.Name -like 'Lingwu SysMon*') {
            $r = $b.GetCurrentPropertyValue([System.Windows.Automation.AutomationElement]::BoundingRectangleProperty)
            return @{ X = [int]$r.X; Y = [int]$r.Y; W = [int]$r.Width; H = [int]$r.Height }
        }
    }
    return $null
}

# 带重试的版本。Shell_NotifyIcon 注册到托盘有延迟（explorer 侧异步刷新），
# 刚启动就去枚举会「找不到按钮」而误判成应用没托盘图标 —— 实测踩过：
# 启动后立即枚举为空，等 12 秒后同一个进程的按钮就在 (2291,1704)。
function Wait-TrayButton([int]$TimeoutSec = 25) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $t = Wait-TrayButton
        if ($t) { return $t }
        Start-Sleep -Milliseconds 700
    }
    return $null
}

# 找 #32768 弹出菜单窗口并读出所有菜单项文本
function Get-PopupMenuItems {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, '#32768')
    $wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
    $items = New-Object System.Collections.Generic.List[string]
    foreach ($w in $wins) {
        $miCond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::MenuItem)
        foreach ($m in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)) {
            $items.Add($m.Current.Name)
        }
    }
    return $items
}

function Get-MainWindowRect {
    # 按 ProcessId 定位，不按类名/窗口名。
    # WinUI 3 的类名全系统统一，机器上每个 WinUI 应用都叫同一个名字，
    # 按类名找会命中别的应用。本脚本的 $script:AppPid 由调用方给出。
    if (-not $script:AppPid) { return $null }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $script:AppPid)
    $w = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
        [System.Windows.Automation.TreeScope]::Children, $cond)
    if (-not $w) { return $null }
    $r = $w.GetCurrentPropertyValue([System.Windows.Automation.AutomationElement]::BoundingRectangleProperty)
    return @{ X=[int]$r.X; Y=[int]$r.Y; W=[int]$r.Width; H=[int]$r.H;
              Hwnd=[int]$w.Current.NativeWindowHandle; Name=$w.Current.Name }
}

$saved = [In]::Cursor()
"保存光标: $($saved -join ',')"

switch ($Stage) {
    'rightmenu' {
        $t = Wait-TrayButton
        if (-not $t) { "FAIL: 未找到托盘按钮"; exit 1 }
        "托盘按钮: $($t.X),$($t.Y) $($t.W)x$($t.H)"
        $cx = $t.X + [int]($t.W/2); $cy = $t.Y + [int]($t.H/2)
        $len = Trace-Len
        [In]::ClickAt($cx, $cy, $true)
        Start-Sleep -Milliseconds 700
        $items = Get-PopupMenuItems
        "---- 托盘菜单项 ($($items.Count)) ----"
        $items | ForEach-Object { "  [$_]" }
        "---- 新增 trace ----"
        (Get-Trace).Substring($len)
        [In]::Key([uint16]0x1B)
        Start-Sleep -Milliseconds 500
        $after = Get-PopupMenuItems
        "按 Esc 后菜单项数: $($after.Count)  (期望 0)"
    }
    'leftclick' {
        $proc = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        if (-not $proc) { "FAIL: 应用未在运行"; exit 1 }
        $script:AppPid = $proc.Id
        $m = Get-MainWindowRect
        "点击前主窗口: $(if ($m) {"$($m.X),$($m.Y) $($m.W)x$($m.H) hwnd=$($m.Hwnd)"} else {'未找到'})"
        # 先把主窗口最小化，模拟「只进托盘」状态
        if ($m) {
            [void][W]::ShowWindow([IntPtr]$m.Hwnd, 6)   # SW_MINIMIZE
            Start-Sleep -Milliseconds 600
            "最小化后 IsIconic=$([W]::IsIconic([IntPtr]$m.Hwnd))"
        }
        $t = Wait-TrayButton
        if (-not $t) { "FAIL: 未找到托盘按钮"; exit 1 }
        $cx = $t.X + [int]($t.W/2); $cy = $t.Y + [int]($t.H/2)
        $len = Trace-Len
        [In]::ClickAt($cx, $cy, $false)
        Start-Sleep -Milliseconds 900
        "---- 新增 trace ----"
        (Get-Trace).Substring($len)
        $m2 = Get-MainWindowRect
        "点击后主窗口: $(if ($m2) {"$($m2.X),$($m2.Y) $($m2.W)x$($m2.H)"} else {'未找到'})"
        if ($m2) { "点击后 IsIconic=$([W]::IsIconic([IntPtr]$m2.Hwnd))  (期望 False)" }
    }
    'xminimize' {
        $proc = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        if (-not $proc) { "FAIL: 应用未在运行"; exit 1 }
        $script:AppPid = $proc.Id
        $m = Get-MainWindowRect
        if (-not $m) { "FAIL: 未找到主窗口"; exit 1 }
        "主窗口: $($m.X),$($m.Y) $($m.W)x$($m.H) hwnd=$($m.Hwnd)"
        # Alt+F4 是「点标题栏 X」的键盘等价物，走同一条 AppWindow.Closing 路径
        [W2]::Send(0x12, 0); Start-Sleep -Milliseconds 60
        [W2]::Send(0x73, 0); Start-Sleep -Milliseconds 60
        [W2]::Send(0x73, 2); Start-Sleep -Milliseconds 60
        [W2]::Send(0x12, 2); Start-Sleep -Milliseconds 800
        $alive = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        "Alt+F4 后进程: $(if ($alive) {($alive.Id -join ',')} else {'已退出'})  (期望仍存活)"
        "Alt+F4 后 IsIconic=$([W]::IsIconic([IntPtr]$m.Hwnd))  (期望 True)"
        $len = Trace-Len
        "---- 新增 trace ----"
        (Get-Trace).Substring($len)
    }
    'exit' {
        $t = Wait-TrayButton
        if (-not $t) { "FAIL: 未找到托盘按钮"; exit 1 }
        $cx = $t.X + [int]($t.W/2); $cy = $t.Y + [int]($t.H/2)
        $len = Trace-Len
        [In]::ClickAt($cx, $cy, $true)
        Start-Sleep -Milliseconds 700
        $items = Get-PopupMenuItems
        "---- 托盘菜单项 ----"; $items | ForEach-Object { "  [$_]" }
        $exitItem = ($items | Where-Object { $_ -eq '退出' })
        if (-not $exitItem) { "FAIL: 菜单里没有『退出』"; [In]::Key([uint16]0x1B); exit 1 }
        # 用 UIA 点菜单项
        $root = [System.Windows.Automation.AutomationElement]::RootElement
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, '#32768')
        $clicked = $false
        foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
            $miCond = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::MenuItem)
            foreach ($m in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)) {
                if ($m.Current.Name -eq '退出') {
                    $ip = $m.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern) -as [System.Windows.Automation.InvokePattern]
                    if ($ip) { $ip.Invoke(); $clicked = $true; "已 Invoke『退出』" }
                }
            }
        }
        if (-not $clicked) { "FAIL: 无法 Invoke『退出』"; exit 1 }
        Start-Sleep -Seconds 4
        $alive = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        "点击『退出』后进程: $(if ($alive) {($alive.Id -join ',')} else {'已退出'})  (期望 已退出)"
        "托盘按钮仍在? $(if (Find-TrayButton) {'在'} else {'不在'})  (期望 不在)"
        "---- 新增 trace ----"
        (Get-Trace).Substring($len)
    }
    'second' {
        # 启动第二份，验证它把第一份叫出来然后自己退出
        $proc = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        if (-not $proc) { "FAIL: 第一份未在运行"; exit 1 }
        $script:AppPid = $proc.Id
        $m1 = Get-MainWindowRect
        "第一份主窗口: $(if ($m1) {'在'} else {'未找到'})"
        if ($m1) {
            [void][W]::ShowWindow([IntPtr]$m1.Hwnd, 6); Start-Sleep -Milliseconds 500
            "第一份已最小化 IsIconic=$([W]::IsIconic([IntPtr]$m1.Hwnd))"
        }
        $len = Trace-Len
        $p2 = Start-Process -FilePath $script:AppExe -PassThru
        Start-Sleep -Seconds 6
        "第二份 pid=$($p2.Id) 已退出? $($p2.HasExited)  退出码=$($p2.ExitCode)"
        $all = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        "当前 LingwuSysmon 进程: $($all.Id -join ',')  (期望只剩第一份 $($proc.Id))"
        if ($m1) {
            "第一份 IsIconic=$([W]::IsIconic([IntPtr]$m1.Hwnd))  (期望 False，被第二份唤起)"
        }
        "---- 新增 trace ----"
        (Get-Trace).Substring($len)
    }
    'settings' {
        # 托盘菜单「设置…」：点开 → 应弹出设置窗口（复用 Phase 3 的 SettingsWindow）。
        $proc = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        if (-not $proc) { "FAIL: 应用未在运行"; exit 1 }
        $script:AppPid = $proc.Id
        $t = Wait-TrayButton
        if (-not $t) { "FAIL: 未找到托盘按钮"; exit 1 }
        $len = Trace-Len
        [In]::ClickAt($t.X + [int]($t.W/2), $t.Y + [int]($t.H/2), $true)
        Start-Sleep -Milliseconds 700
        $root = [System.Windows.Automation.AutomationElement]::RootElement
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, '#32768')
        $clicked = $false
        foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
            $miCond = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::MenuItem)
            foreach ($m in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)) {
                if ($m.Current.Name -eq '设置…') {
                    $ip = $m.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern) -as [System.Windows.Automation.InvokePattern]
                    if ($ip) { $ip.Invoke(); $clicked = $true }
                }
            }
        }
        "点『设置…』: $clicked"
        Start-Sleep -Seconds 3
        # 设置窗口是本进程的第二个顶层窗口。按 pid + 名字找。
        $pidCond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $script:AppPid)
        $wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $pidCond)
        "进程顶层窗口数: $($wins.Count)  (期望 2：主窗口 + 设置窗口)"
        foreach ($w in $wins) { "  name='$($w.Current.Name)'" }
        # 关掉设置窗口（Alt+F4），不影响主窗口
        [W2]::Send(0x73, 0); Start-Sleep -Milliseconds 60
        [W2]::Send(0x73, 2); Start-Sleep -Milliseconds 800
        "---- 新增 trace ----"
        (Get-Trace).Substring($len)
    }

    'aggregate' {
        # 托盘菜单「按应用聚合」：切换勾选态，列表应重新分组。
        $proc = Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne 37480 }
        if (-not $proc) { "FAIL: 应用未在运行"; exit 1 }
        $script:AppPid = $proc.Id
        $t = Wait-TrayButton
        if (-not $t) { "FAIL: 未找到托盘按钮"; exit 1 }
        $len = Trace-Len
        [In]::ClickAt($t.X + [int]($t.W/2), $t.Y + [int]($t.H/2), $true)
        Start-Sleep -Milliseconds 700
        $root = [System.Windows.Automation.AutomationElement]::RootElement
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, '#32768')
        # 先读一遍勾选态
        $beforeChecked = $null
        foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
            $miCond = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::MenuItem)
            foreach ($m in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)) {
                if ($m.Current.Name -eq '按应用聚合') {
                    $beforeChecked = $m.GetCurrentPropertyValue(
                        [System.Windows.Automation.SelectionItemPattern]::IsSelectedProperty)
                }
            }
        }
        "切换前『按应用聚合』勾选: $beforeChecked"
        $clicked = $false
        foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
            $miCond = New-object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::MenuItem)
            foreach ($m in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)) {
                if ($m.Current.Name -eq '按应用聚合') {
                    $ip = $m.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern) -as [System.Windows.Automation.InvokePattern]
                    if ($ip) { $ip.Invoke(); $clicked = $true }
                }
            }
        }
        "点『按应用聚合』: $clicked"
        Start-Sleep -Seconds 3
        "---- 新增 trace ----"
        (Get-Trace).Substring($len)
        # 再点一次，恢复原状
        [In]::ClickAt($t.X + [int]($t.W/2), $t.Y + [int]($t.H/2), $true)
        Start-Sleep -Milliseconds 700
        foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
            $miCond = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::MenuItem)
            foreach ($m in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)) {
                if ($m.Current.Name -eq '按应用聚合') {
                    $ip = $m.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern) -as [System.Windows.Automation.InvokePattern]
                    if ($ip) { $ip.Invoke() }
                }
            }
        }
        Start-Sleep -Seconds 3
        "---- 复原后 trace 新增 ----"
        (Get-Trace).Substring($len)
    }
}

[In]::SetCursorPos($saved[0], $saved[1]) | Out-Null
"光标已还原: $([In]::Cursor() -join ',')"
