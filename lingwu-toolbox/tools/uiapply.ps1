param(
    [string]$Exe,
    [string]$Prefix = 'apply'
)

# 验证「设置窗口里改的值真的作用到了运行时」。
#
# 为什么另写一个脚本而不是在 uisettings.ps1 里加：本环境**读不了图片**
# （read 工具对 png 返回 model does not support images），所以一切取证必须走
# 数值/文本断言。这里读列表行的 Name 列，直接判断是不是按名称排好了。
#
# 用例：设置里选「名称」+「2 秒」→ 确定 → 主窗口列表应变为名称升序，
#       且 ini 落 SortMode=4 / IntervalMs=2000。

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

$Ini = Join-Path $env:APPDATA 'LingwuSysMon\settings.ini'
$IniBackup = "$Ini.applybak"
$hadIni = Test-Path $Ini
if ($hadIni) { Copy-Item $Ini $IniBackup -Force }

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

$win = Get-Win $p.Id
if (-not $win) { Write-Host 'MAIN WINDOW NOT FOUND'; exit 1 }
$mainHwnd = $win.Current.NativeWindowHandle
Start-Sleep -Seconds 14

# 读当前列表前 12 行的名称
function Get-RowNames($root) {
    $list = Find-Id $root 'ProcessList'
    if (-not $list) { return @() }
    $items = $list.FindAll($TS::Children,
        (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)))
    $names = @()
    $n = 0
    foreach ($it in $items) {
        if ($n -ge 12) { break }
        $names += $it.Current.Name
        $n++
    }
    return ,$names
}

$before = Get-RowNames $win
Write-Host "before count=$($before.Count)"
$i = 0
foreach ($n in $before) { Write-Host ("  before[$i] = $n"); $i++ }

# ---- 打开设置，选「名称」+「2 秒」，确定 -----------------------------------
$settingsBtn = $null
foreach ($b in (Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::Button))) {
    if ($b.Current.Name -eq '设置') { $settingsBtn = $b; break }
}
if (-not $settingsBtn) { Write-Host 'SETTINGS BUTTON NOT FOUND'; exit 1 }
$settingsBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
Start-Sleep -Seconds 3

$sw = Get-SettingsWin $p.Id $mainHwnd
if (-not $sw) { Write-Host 'SETTINGS WINDOW NOT FOUND'; exit 1 }

$sort = Find-Id $sw 'SortCombo'
$intv = Find-Id $sw 'IntervalCombo'
function Set-Combo($combo, [int]$index, [string]$label) {
    $ec = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    if ($ec.Current.ExpandCollapseState -ne 'Expanded') { $ec.Expand() }
    Start-Sleep -Milliseconds 500
    $items = $combo.FindAll($TS::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)))
    if ($items.Count -le $index) { Write-Host "$label out of range"; $ec.Collapse(); return }
    $items[$index].GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 400
    $ec.Collapse()
}
Set-Combo $sort 2 'SortCombo'   # 第 3 项 = 名称
Set-Combo $intv 2 'IntervalCombo'  # 第 3 项 = 2 秒

$okBtn = Find-Id $sw 'OkButton'
if (-not $okBtn) { Write-Host 'OkButton NOT FOUND'; exit 1 }
$okBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
Write-Host 'clicked OK'
Start-Sleep -Seconds 6

# ---- 断言 1：列表按名称升序 ------------------------------------------------
$after = Get-RowNames $win
Write-Host "after count=$($after.Count)"
$i = 0
foreach ($n in $after) { Write-Host ("  after[$i] = $n"); $i++ }

# 名称列可能带进程数后缀，取第一段（制表符前的可见名）做比较。
function Name-Key([string]$s) {
    if ($null -eq $s) { return '' }
    $t = $s.Trim()
    $tab = $t.IndexOf("`t")
    if ($tab -ge 0) { $t = $t.Substring(0, $tab) }
    return $t
}
$keys = @()
foreach ($n in $after) { $keys += (Name-Key $n) }
$sorted = $keys | Sort-Object
$isSorted = $true
for ($i = 0; $i -lt $keys.Count; $i++) {
    if ($keys[$i] -ne $sorted[$i]) { $isSorted = $false; Write-Host "  MISMATCH at $i : '$($keys[$i])' vs '$($sorted[$i])'" }
}
if ($isSorted -and $keys.Count -gt 3) { Write-Host 'PASS: list is in name-ascending order' }
else { Write-Host "FAIL: list not name-sorted (count=$($keys.Count))" }

# ---- 断言 2：ini 落盘 ------------------------------------------------------
# 注意：变量名绝不能用 $Ini —— PowerShell 变量名**大小写不敏感**，
# 上面 $Ini 存的是路径，这里一旦写 $ini = Get-Content 就会把路径覆盖成文件内容，
# 末尾的还原 Copy-Item 于是把内容当成目标路径去解析，报一堆看不懂的语法错误。
# （这个坑本轮真的踩了，还原失败，用户的 ini 被留在测试值上。）
$IniText = Get-Content $Ini -Raw -Encoding Default
$sm = [regex]::Match($IniText, '(?m)^SortMode=(\d+)')
$iv = [regex]::Match($IniText, '(?m)^IntervalMs=(\d+)')
Write-Host ("ini SortMode    = " + $(if ($sm.Success) { $sm.Groups[1].Value } else { 'MISSING' }))
Write-Host ("ini IntervalMs  = " + $(if ($iv.Success) { $iv.Groups[1].Value } else { 'MISSING' }))
if ($sm.Success -and $sm.Groups[1].Value -eq '4') { Write-Host 'PASS: SortMode=4 (name asc)' } else { Write-Host 'FAIL: SortMode not 4' }
if ($iv.Success -and $iv.Groups[1].Value -eq '2000') { Write-Host 'PASS: IntervalMs=2000' } else { Write-Host 'FAIL: IntervalMs not 2000' }

# ---- 断言 3：进程还活着 ----------------------------------------------------
Start-Sleep -Seconds 2
$p.Refresh()
Write-Host ("alive=$(-not $p.HasExited) priv=$([math]::Round($p.PrivateMemorySize64/1MB,1))MB")

# ---- 还原 ini ------------------------------------------------------------
if ($hadIni) { Copy-Item $IniBackup $Ini -Force; Write-Host 'ini restored' }
elseif (Test-Path $Ini) { Remove-Item $Ini -Force; Write-Host 'ini removed' }
