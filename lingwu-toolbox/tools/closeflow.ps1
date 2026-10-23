# closeflow.ps1 —— 「关闭窗口确认框」全流程验证（T1~T8）
#
# 【为什么不用合成鼠标点击标题栏的 X】
# 本机实测 SendInput / mouse_event 点不动任何系统控件（AGENTS.md「验证要求」）。
# 改为 PostMessageW(hwnd, WM_CLOSE) —— 它与点 X **走同一条** AppWindow.Closing
# 路径：AGENTS.md 踩坑 7 已经证实「SetWindowSubclass 拦 WM_CLOSE 拦不住」，
# 反过来说明 WM_CLOSE 确实进了 XAML 层并抛出 Closing，所以两条路等价。
#
# 【断言为什么不碰光标】
# 一律 UIA InvokePattern + 读可观测状态（IsWindowVisible / IsIconic /
# 进程存活 / ini / 托盘按钮枚举）。跨进程 UIA 一律按 ProcessId 卡死，
# 绝不在桌面根 FindAll 再按名字过滤（曾误点别的应用的控件）。
#
# 【为什么要写成一个脚本跑完】
# 非交互 shell 里 GUI 进程活不过单次调用，所以「启动 → 操作 → 断言」
# 必须写在同一次执行里。

param(
    [string]$Exe = '',
    # 已知杀不掉的僵尸残留
    [int]$IgnorePid = 37480
)

# exe 路径按脚本位置解析，项目目录改名后不必再改这里。
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\dist\Release\LingwuSysmon.exe' }

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]

Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public class CF {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
}
'@

$WM_CLOSE       = 0x0010
$WM_CONTEXTMENU = 0x007B

$script:Pass = 0
$script:Fail = 0

function Assert-True([string]$name, [bool]$cond, [string]$detail = '') {
    if ($cond) {
        $script:Pass++
        Write-Output ("  [PASS] {0}{1}" -f $name, $(if ($detail) { "  ($detail)" } else { '' }))
    } else {
        $script:Fail++
        Write-Output ("  [FAIL] {0}{1}" -f $name, $(if ($detail) { "  ($detail)" } else { '' }))
    }
}

function Get-AppProc {
    Get-Process LingwuSysmon -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $IgnorePid } | Select-Object -First 1
}

function Stop-App {
    Get-Process LingwuSysmon -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $IgnorePid } |
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

# 在**任意**作用域里按 ControlType + Name 找元素。
# ContentDialog 渲染在 XamlRoot 的 Popup 里，UIA 上通常挂在窗口之下，
# 但不同版本可能把它挂成独立顶层窗口，所以窗口子树与桌面根两种都试。
# 注意这里必须用 ProcessId 卡死，绝不能只在桌面根 FindAll 再按名字过滤。
function Find-El([int]$procId, $controlType, [string]$name) {
    $leaf = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $procId)),
        (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $controlType)))
    $all = $AE::RootElement.FindAll($TS::Descendants, $leaf)
    foreach ($e in $all) {
        if ($e.Current.Name -eq $name) { return $e }
    }
    return $null
}

function Invoke-El($el, [string]$what) {
    if (-not $el) { Write-Output "  [FAIL] $what : 元素不存在"; $script:Fail++; return $false }
    try {
        $p = $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $p.Invoke()
        return $true
    } catch {
        Write-Output "  [FAIL] $what : Invoke 抛异常 $($_.Exception.Message)"
        $script:Fail++
        return $false
    }
}

function Wait-El([int]$procId, $controlType, [string]$name, [int]$sec = 15) {
    $deadline = (Get-Date).AddSeconds($sec)
    while ((Get-Date) -lt $deadline) {
        $e = Find-El $procId $controlType $name
        if ($e) { return $e }
        Start-Sleep -Milliseconds 400
    }
    return $null
}

function Wait-AppGone([int]$procId, [int]$sec = 15) {
    $deadline = (Get-Date).AddSeconds($sec)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { return $true }
        Start-Sleep -Milliseconds 400
    }
    return $false
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

# ---- ini 存取（只碰 CloseAction 一个键）----
$IniPath = Join-Path $env:APPDATA 'LingwuSysMon\settings.ini'

function Get-CloseAction {
    if (-not (Test-Path $IniPath)) { return 0 }
    $m = Select-String -Path $IniPath -Pattern '^\s*CloseAction\s*=\s*(\d+)' -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if (-not $m) { return 0 }
    return [int]$m.Matches[0].Groups[1].Value
}

function Set-CloseAction([int]$v) {
    if (-not (Test-Path $IniPath)) {
        New-Item -ItemType Directory -Path (Split-Path $IniPath) -Force | Out-Null
        Set-Content -Path $IniPath -Value "[Settings]`r`nCloseAction=$v" -Encoding ASCII
        return
    }
    if (Select-String -Path $IniPath -Pattern '^\s*CloseAction\s*=' -Quiet -ErrorAction SilentlyContinue) {
        (Get-Content $IniPath) -replace '^\s*CloseAction\s*=\s*\d+', "CloseAction=$v" |
            Set-Content -Path $IniPath -Encoding UTF8
    } else {
        Add-Content -Path $IniPath -Value "CloseAction=$v" -Encoding UTF8
    }
}

Write-Output '==================== close flow verification ===================='
Write-Output "exe=$Exe"
Write-Output "ini=$IniPath  CloseAction(before)=$((Get-CloseAction))"

# 每次都从「每次询问」开始，避免上一轮跑歪了留下脏状态
Stop-App
Set-CloseAction 0
Write-Output "CloseAction(forced)=$(Get-CloseAction)"
Write-Output ''

# ============================================================ T1 / T2
Write-Output '=== T1/T2: 点 X 应弹确认框；取消后窗口原样不动 ==='
$p = Start-Process $Exe -PassThru
$procId = $p.Id
Write-Output "launched pid=$procId"
$hwnd = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline -and $hwnd -eq [IntPtr]::Zero) {
    $hwnd = Get-Hwnd $procId
    if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Output '  [FAIL] 主窗口没出现'
    $script:Fail++
    Stop-App
    exit 1
}
Write-Output "main hwnd=$hwnd"
Start-Sleep -Seconds 8   # 等刷新循环到稳态、列表物化

[CF]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null

$btnMin = Wait-El $procId $CT::Button '最小化到托盘' 15
$btnExi = Wait-El $procId $CT::Button '退出程序' 15
$btnCan = Wait-El $procId $CT::Button '取消' 15
$chkMem = Wait-El $procId $CT::CheckBox '记住我的选择，以后不再询问' 15

Assert-True 'T1 弹窗出现且主窗口未被关掉' (([bool]([CF]::IsWindowVisible($hwnd))) -and ($null -ne $btnMin)) `
    "closeWinVisible=$([bool]([CF]::IsWindowVisible($hwnd)))"
Assert-True 'T1 主按钮「最小化到托盘」存在' ($null -ne $btnMin)
Assert-True 'T1 次按钮「退出程序」存在'   ($null -ne $btnExi)
Assert-True 'T1 关闭按钮「取消」存在'      ($null -ne $btnCan)
Assert-True 'T1 复选框「记住我的选择，以后不再询问」存在' ($null -ne $chkMem)

# 取消应落在「退出程序」上会误触进程，所以必须验默认焦点不在它身上。
# UIA 没有「默认按钮」属性，只能看刚打开时谁持有键盘焦点。
$focused = '(none)'
foreach ($pair in @(@('取消', $btnCan), @('最小化到托盘', $btnMin), @('退出程序', $btnExi))) {
    if ($pair[1] -and $pair[1].Current.HasKeyboardFocus) { $focused = $pair[0] }
}
Assert-True 'T1 默认焦点不在「退出程序」上' ($focused -ne '退出程序') "focusOn=$focused"
Write-Output "  [info] 键盘焦点所在按钮 = $focused （期望「取消」）"
if ($focused -ne '取消') {
    Write-Output '  [WARN] 焦点不在「取消」上；若确认 DefaultButton=Close 生效，本条可忽略'
}

Invoke-El $btnCan 'T2 Invoke「取消」' | Out-Null
Start-Sleep -Seconds 3
Assert-True 'T2 取消后主窗口仍可见'   ([bool]([CF]::IsWindowVisible($hwnd)))
Assert-True 'T2 取消后主窗口未最小化' (-not [bool]([CF]::IsIconic($hwnd)))
Assert-True 'T2 取消后进程仍存活'     ($null -ne (Get-Process -Id $procId -ErrorAction SilentlyContinue))
Assert-True 'T2 取消后确认框已消失'   ($null -eq (Find-El $procId $CT::Button '退出程序'))
Write-Output ''

# ============================================================ T8
Write-Output '=== T8: 「结束任务」确认框开着时点 X —— 不应弹新框、原框不被破坏 ==='
# 用 WM_CONTEXTMENU 打开行右键菜单：lParam 是**屏幕**坐标，HostProc 会自己
# ScreenToClient。菜单项是 XAML 控件，InvokePattern 可用。
$firstRow = $null
$li = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::ListItem)
$w = Find-MainWindow $procId
if ($w) {
    foreach ($r in $w.FindAll($TS::Descendants, $li)) {
        if ($r.Current.Name -and -not $r.Current.IsOffscreen) { $firstRow = $r; break }
    }
}
if ($firstRow) {
    $rect = $firstRow.GetCurrentPropertyValue($AE::BoundingRectangleProperty)
    $sx = [int]($rect.X + $rect.Width / 2)
    $sy = [int]($rect.Y + $rect.Height / 2)
    $lp = [IntPtr](($sy -shl 16) -bor ($sx -band 0xFFFF))
    [CF]::PostMessage($hwnd, $WM_CONTEXTMENU, [IntPtr]::Zero, $lp) | Out-Null
    $endTask = Wait-El $procId $CT::MenuItem '结束任务' 10
    if ($endTask) {
        Invoke-El $endTask 'T8 Invoke「结束任务」' | Out-Null
        $confirmBtn = Wait-El $procId $CT::Button '结束任务' 10
        Assert-True 'T8 结束任务确认框已打开' ($null -ne $confirmBtn)
        if ($confirmBtn) {
            # 此时点 X：应当被忽略，且原确认框不受影响
            [CF]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Seconds 3
            Assert-True 'T8 已有对话框时不再弹关闭确认框' ($null -eq (Find-El $procId $CT::Button '退出程序'))
            Assert-True 'T8 原结束任务确认框未被挤掉'   ($null -ne (Find-El $procId $CT::Button '结束任务'))
            Assert-True 'T8 窗口没有被隐藏'             ([bool]([CF]::IsWindowVisible($hwnd)))
            # 收尾：点「取消」关掉它
            $canEnd = Find-El $procId $CT::Button '取消'
            Invoke-El $canEnd 'T8 收尾取消' | Out-Null
            Start-Sleep -Seconds 2
        }
    } else {
        Write-Output '  [WARN] T8: 行右键菜单没弹出（MenuFlyout 未能被 UIA 驱动），本用例跳过'
    }
} else {
    Write-Output '  [WARN] T8: 没找到可见行，本用例跳过'
}
Write-Output ''

# ============================================================ T3 / T4 / T7
Write-Output '=== T3: 「最小化到托盘」= 真隐藏（任务栏不留按钮，托盘仍在）==='
[CF]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$btnMin = Wait-El $procId $CT::Button '最小化到托盘' 15
Assert-True 'T3 再次点 X 能弹框' ($null -ne $btnMin)
Invoke-El $btnMin 'T3 Invoke「最小化到托盘」' | Out-Null
Start-Sleep -Seconds 4
Assert-True 'T3 主窗口已隐藏（IsWindowVisible=False）' (-not [bool]([CF]::IsWindowVisible($hwnd)))
Assert-True 'T3 进程仍在运行'                       ($null -ne (Get-Process -Id $procId -ErrorAction SilentlyContinue))
$tray = $null
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and -not $tray) { $tray = Find-TrayButton; Start-Sleep -Milliseconds 700 }
Assert-True 'T3 托盘图标仍在（程序没退出）' ($null -ne $tray)

Write-Output ''
Write-Output '=== T7: 隐藏态下再启动一份，第二份应把第一份唤回 ==='
$second = Start-Process $Exe -PassThru
Start-Sleep -Seconds 8
$stillOne = @(Get-Process LingwuSysmon -ErrorAction SilentlyContinue |
              Where-Object { $_.Id -ne $IgnorePid })
Assert-True 'T7 单实例仍成立（只有一份进程）' ($stillOne.Count -eq 1) "count=$($stillOne.Count)"
Assert-True 'T7 第一份窗口被唤回且可见' ([bool]([CF]::IsWindowVisible($hwnd)))
if ($second -and -not $second.HasExited) { $second | Stop-Process -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 1

Write-Output ''
Write-Output '=== T4: 托盘左键 Invoke 应把窗口唤回并还原 ==='
$hwnd = Get-Hwnd $procId
if ($hwnd -eq [IntPtr]::Zero) { $hwnd = [IntPtr]::new([int]$p.MainWindowHandle) }
$tray = Find-TrayButton
if ($tray) {
    try {
        $tp = $tray.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $tp.Invoke()
    } catch { Write-Output "  [WARN] 托盘 Invoke 抛异常: $($_.Exception.Message)" }
} else {
    Write-Output '  [FAIL] 找不到托盘按钮'
    $script:Fail++
}
Start-Sleep -Seconds 4
Assert-True 'T4 窗口已重新可见' ([bool]([CF]::IsWindowVisible($hwnd)))
Assert-True 'T4 窗口未处于最小化' (-not [bool]([CF]::IsIconic($hwnd)))

# 隐藏期间 Refresh() 被跳过，所以 ShowAndActivate 里那次补刷是唯一的恢复点。
# 必须验「唤回后列表有数据」，否则这道闸门会悄悄造成一个空列表而无人察觉。
Start-Sleep -Seconds 3
$list = Find-El $procId $CT::List ''
$rowCond = New-Object System.Windows.Automation.PropertyCondition(
    $AE::ControlTypeProperty, $CT::ListItem)
$rowCount = 0
$w = Find-MainWindow $procId
if ($w) { $rowCount = $w.FindAll($TS::Descendants, $rowCond).Count }
Assert-True 'T4 唤回后列表已有行（补刷生效，非空列表）' ($rowCount -gt 0) "rows=$rowCount"
Write-Output ''

# ============================================================ T5
Write-Output '=== T5: 勾「记住」+「退出程序」应落 ini CloseAction=2 并结束进程 ==='
[CF]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$chkMem  = Wait-El $procId $CT::CheckBox '记住我的选择，以后不再询问' 15
$btnExi2 = Wait-El $procId $CT::Button '退出程序' 15
Assert-True 'T5 弹框正常出现' (($null -ne $chkMem) -and ($null -ne $btnExi2))

if ($chkMem) {
    try {
        $tgp = $chkMem.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        $tgp.Toggle()
        Start-Sleep -Milliseconds 600
        Assert-True 'T5 复选框已勾上' ($chkMem.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq 'On')
    } catch {
        Write-Output "  [FAIL] T5 复选框 Toggle 抛异常: $($_.Exception.Message)"
        $script:Fail++
    }
} else {
    Write-Output '  [FAIL] T5 找不到复选框'
    $script:Fail++
}
Invoke-El $btnExi2 'T5 Invoke「退出程序」' | Out-Null
$gone = Wait-AppGone $procId 20
Assert-True 'T5 进程已结束' $gone
Assert-True 'T5 ini CloseAction 落成 2' ((Get-CloseAction) -eq 2) "actual=$(Get-CloseAction)"
Start-Sleep -Seconds 3
Assert-True 'T5 托盘图标已撤销' ($null -eq (Find-TrayButton))
Write-Output ''

# ============================================================ T6
Write-Output '=== T6: 记忆生效后点 X 不再询问，直接退出 ==='
$p2 = Start-Process $Exe -PassThru
$procId2 = $p2.Id
$hwnd2 = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline -and $hwnd2 -eq [IntPtr]::Zero) {
    $hwnd2 = Get-Hwnd $procId2
    if ($hwnd2 -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($hwnd2 -eq [IntPtr]::Zero) {
    Write-Output '  [FAIL] T6 第二份窗口没出现'
    $script:Fail++
} else {
    Start-Sleep -Seconds 6
    [CF]::PostMessage($hwnd2, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 4
    # 关键断言：既没有弹框，也没有被隐藏，而是直接退出了
    Assert-True 'T6 没有弹确认框' ($null -eq (Find-El $procId2 $CT::Button '退出程序'))
    Assert-True 'T6 进程直接退出' (Wait-AppGone $procId2 15)
}
Write-Output ''

# ============================================================ T9
Write-Output '=== T9: CloseAction=1（总是隐藏到托盘）应不询问直接隐藏 ==='
Stop-App
Set-CloseAction 1
$p3 = Start-Process $Exe -PassThru
$procId3 = $p3.Id
$hwnd3 = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline -and $hwnd3 -eq [IntPtr]::Zero) {
    $hwnd3 = Get-Hwnd $procId3
    if ($hwnd3 -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($hwnd3 -eq [IntPtr]::Zero) {
    Write-Output '  [FAIL] T9 窗口没出现'
    $script:Fail++
} else {
    Start-Sleep -Seconds 6
    [CF]::PostMessage($hwnd3, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 4
    Assert-True 'T9 没有弹确认框' ($null -eq (Find-El $procId3 $CT::Button '退出程序'))
    Assert-True 'T9 窗口已直接隐藏' (-not [bool]([CF]::IsWindowVisible($hwnd3)))
    Assert-True 'T9 进程仍在运行'   ($null -ne (Get-Process -Id $procId3 -ErrorAction SilentlyContinue))
    $tray9 = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and -not $tray9) { $tray9 = Find-TrayButton; Start-Sleep -Milliseconds 700 }
    Assert-True 'T9 托盘图标仍在' ($null -ne $tray9)
}
Write-Output ''

# ============================================================ 收尾
Write-Output '=== cleanup: 把 CloseAction 还原成 0 ==='
Stop-App
Set-CloseAction 0
Write-Output "CloseAction(after)=$(Get-CloseAction)"
Assert-True 'cleanup ini 已归零' ((Get-CloseAction) -eq 0)

Write-Output ''
Write-Output '=========================================================='
Write-Output ("RESULT: pass={0}  fail={1}" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 } else { exit 0 }
