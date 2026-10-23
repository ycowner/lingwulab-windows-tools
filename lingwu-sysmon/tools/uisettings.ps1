param(
    [string]$Exe,
    [string]$Prefix = 'set',
    [switch]$CancelMode,      # 只验证「取消不改动」
    [switch]$NoAutoStart,     # 不动注册表（默认就走这条，避免副作用）
    [int]$SortIndex = 2,      # 选排序下拉的第几项（0..4）。默认 2 = CPU 占用 ↓
    [int]$IntervalIndex = 2   # 选间隔下拉的第几项（0..3）。默认 2 = 2 秒
)

# 设置窗口实测。设计约束（照 uimenu.ps1 踩过的坑）：
#   * 跨进程找元素一律 AndCondition(ControlType, ProcessId)，绝不只用 ControlType
#     在桌面根 FindAll 再按名字 -like 过滤 —— 那会点到别的应用的控件，产生真实副作用。
#   * 自启动开关**默认不碰**：它写 HKCU 注册表，是跨重启的持久副作用。
#     只有在显式传 -AutoStart 时才测，且测完立刻还原。
#   * 截图只是给人看的，断言全部走 UIA 读回的值 + ini 文件内容。

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
[void][Native.Win]::SetProcessDPIAware()

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$PW = 0x00000002

$Ini = Join-Path $env:APPDATA 'LingwuSysMon\settings.ini'
$IniBackup = "$Ini.setbak"

function Read-Ini {
    if (-not (Test-Path $Ini)) { return $null }
    return Get-Content $Ini -Raw -Encoding Default
}

# 保留用户原始 ini，测完还原。绝不在用户机器上留下我们编的值。
$hadIni = Test-Path $Ini
if ($hadIni) { Copy-Item $Ini $IniBackup -Force }
Write-Host ("ini exists before test = $hadIni")

Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$p = Start-Process $Exe -PassThru
Write-Host "launched pid=$($p.Id)"

function Get-Win($pid_) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    for ($i = 0; $i -lt 60; $i++) {
        $w = $AE::RootElement.FindFirst($TS::Children, $cond)
        if ($w) { return $w }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# 设置窗口是第二个顶层窗口，所以等它出现要按「非主窗口」挑。
function Get-SettingsWin($pid_, $mainHwnd) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    for ($i = 0; $i -lt 60; $i++) {
        $all = $AE::RootElement.FindAll($TS::Children, $cond)
        foreach ($w in $all) {
            if ($w.Current.NativeWindowHandle -ne $mainHwnd) { return $w }
        }
        Start-Sleep -Milliseconds 400
    }
    return $null
}

function Find-Id($root, [string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    return $root.FindFirst($TS::Descendants, $c)
}

function Find-ByTypeInProc($pid_, $controlType) {
    $c1 = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $controlType)
    $c2 = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    $and = New-Object System.Windows.Automation.AndCondition($c1, $c2)
    return $AE::RootElement.FindAll($TS::Descendants, $and)
}

function Shot([string]$path) {
    $r = New-Object Native.Win+RECT
    if (-not [Native.Win]::GetWindowRect($p.MainWindowHandle, [ref]$r)) { return 'rect failed' }
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [Native.Win]::PrintWindow($p.MainWindowHandle, $hdc, $PW)
    $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $(if ($ok) { "$w x $h" } else { 'PrintWindow failed' })
}

$win = Get-Win $p.Id
if (-not $win) { Write-Host 'MAIN WINDOW NOT FOUND'; exit 1 }
$mainHwnd = $win.Current.NativeWindowHandle
Start-Sleep -Seconds 14   # 等列表刷出第一帧，和 uimenu.ps1 一致

# ---- 1) 打开设置窗口 ------------------------------------------------------
# 命令栏上的「设置」按钮。先按 AutomationId 找，找不到再按名字（按钮没有显式 id）。
$settingsBtn = $null
$btns = Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::Button)
foreach ($b in $btns) {
    $n = $b.Current.Name
    if ($n) { Write-Host "  mainwin button: '$n'" }
    if (-not $settingsBtn -and $n -eq '设置') { $settingsBtn = $b }
}
if (-not $settingsBtn) { Write-Host 'SETTINGS BUTTON NOT FOUND'; exit 1 }
Write-Host ("clicking settings: '" + $settingsBtn.Current.Name + "'")
$settingsBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
Start-Sleep -Seconds 3

$sw = Get-SettingsWin $p.Id $mainHwnd
if (-not $sw) { Write-Host 'SETTINGS WINDOW NOT FOUND'; exit 1 }
Write-Host "settings window found, hwnd=$($sw.Current.NativeWindowHandle)"

$shot1 = Join-Path $env:TEMP "$Prefix`_1_open.png"
Write-Host ("settings shot: " + (Shot $shot1))

# ---- 2) 四个控件都在，且读回当前生效值 --------------------------------------
$agg   = Find-Id $sw 'AggregateSwitch'
$sort  = Find-Id $sw 'SortCombo'
$intv  = Find-Id $sw 'IntervalCombo'
$auto  = Find-Id $sw 'AutoStartSwitch'
$err   = Find-Id $sw 'AutoStartError'

foreach ($pair in @(@('AggregateSwitch',$agg), @('SortCombo',$sort), @('IntervalCombo',$intv), @('AutoStartSwitch',$auto))) {
    if (-not $pair[1]) { Write-Host "MISSING CONTROL: $($pair[0])"; exit 1 }
}
Write-Host 'all four controls present'

# ToggleSwitch 用 TogglePattern 读状态；ComboBox 用 Value/Selection 读当前项。
if ($agg) {
    $t = $agg.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    Write-Host ("AggregateSwitch.ToggleState = " + $t.Current.ToggleState)
}
if ($auto) {
    $t = $auto.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    Write-Host ("AutoStartSwitch.ToggleState = " + $t.Current.ToggleState)
}
if ($sort) {
    $sel = $sort.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
    $cur = $sel.Current.GetSelection()
    $nm = if ($cur.Count -gt 0) { $cur[0].Current.Name } else { '<none>' }
    Write-Host ("SortCombo selection = '$nm'")
}
if ($intv) {
    $sel = $intv.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
    $cur = $sel.Current.GetSelection()
    $nm = if ($cur.Count -gt 0) { $cur[0].Current.Name } else { '<none>' }
    Write-Host ("IntervalCombo selection = '$nm'")
}
# 自启错误提示必须平时折叠
if ($err) {
    Write-Host ("AutoStartError IsOffscreen = " + $err.Current.IsOffscreen)
}

# ---- 3) 改值 --------------------------------------------------------------
# ComboBox 的项在**折叠的弹出层**里，不先展开就 FindAll 不到（实测 items = 0）。
# 用 ExpandCollapsePattern.Expand() 展开后再找 ListItem，用
# SelectionItemPattern.Select() 选中。全程走 UIA 标准 pattern，
# 不靠键盘和坐标注入。
function Set-Combo($combo, [int]$index, [string]$label) {
    if (-not $combo) { Write-Host "$label : combo not found"; return $false }
    $ec = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    if ($ec.Current.ExpandCollapseState -ne 'Expanded') { $ec.Expand() }
    Start-Sleep -Milliseconds 500
    $items = $combo.FindAll($TS::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)))
    Write-Host ("$label items = " + $items.Count)
    if ($items.Count -le $index) {
        Write-Host "$label : index $index out of range"
        $ec.Collapse()
        return $false
    }
    $items[$index].GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 500
    $ec.Collapse()
    $sel = $combo.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
    $cur = $sel.Current.GetSelection()
    $nm = if ($cur.Count -gt 0) { $cur[0].Current.Name } else { '<none>' }
    Write-Host ("$label now = '$nm'")
    return $true
}

# 第 3 项：按参数选排序档与间隔档。
# SortIndex 覆盖 0..4 全部 5 个排序档 —— 曾经下拉只有 3 项，
# MEM_ASC(1) 与 CPU_ASC(3) 两个档在设置里永远选不到，必须逐档验。
[void](Set-Combo $sort $SortIndex 'SortCombo')
[void](Set-Combo $intv $IntervalIndex 'IntervalCombo')
$shot2 = Join-Path $env:TEMP "$Prefix`_2_changed.png"
Write-Host ("after change shot: " + (Shot $shot2))

# ---- 4) 确定 / 取消 -------------------------------------------------------
$okBtn = Find-Id $sw 'OkButton'
$cancelBtn = Find-Id $sw 'CancelButton'
if (-not $okBtn) { Write-Host 'OkButton NOT FOUND'; exit 1 }
if (-not $cancelBtn) { Write-Host 'CancelButton NOT FOUND'; exit 1 }

$beforeIni = Read-Ini
if ($CancelMode) {
    Write-Host '--- CANCEL MODE: clicking 取消 ---'
    $cancelBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 2
    $afterIni = Read-Ini
    if ($beforeIni -eq $afterIni) { Write-Host 'PASS: ini unchanged after cancel' }
    else { Write-Host 'FAIL: ini changed after cancel' }
} else {
    Write-Host '--- OK MODE: clicking 确定 ---'
    $okBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 3
}

$shot3 = Join-Path $env:TEMP ($Prefix + '_3_after.png')
Write-Host ("after button shot: " + (Shot $shot3))

# ---- 5) ini 往返 ----------------------------------------------------------
$afterIni = Read-Ini
Write-Host '--- ini after ---'
if ($afterIni) { Write-Host $afterIni } else { Write-Host '<no ini file>' }

# ---- 6) 主窗口是否还活着（设置窗口关闭不该带走主窗口） ---------------------
Start-Sleep -Seconds 2
$p.Refresh()
Write-Host ("alive=$(-not $p.HasExited) priv=$([math]::Round($p.PrivateMemorySize64/1MB,1))MB")

# ---- 7) 还原 ini，绝不在用户机器上留我们编的值 ----------------------------
if ($hadIni) {
    Copy-Item $IniBackup $Ini -Force
    Write-Host 'ini restored from backup'
} elseif (Test-Path $Ini) {
    Remove-Item $Ini -Force
    Write-Host 'ini removed (did not exist before test)'
}
# 备份文件本身也要清掉：还原完 ini 之后它就是一份躺在用户 AppData 里的
# 配置副本，不该留下（2026-10-21 修：每次跑这个脚本都会留一个 .setbak）。
if (Test-Path $IniBackup) {
    Remove-Item $IniBackup -Force
    Write-Host 'ini backup removed'
}

Write-Host "SHOT1=$shot1"
Write-Host "SHOT2=$shot2"
Write-Host "SHOT3=$shot3"
