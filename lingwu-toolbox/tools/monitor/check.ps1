param(
    [string]$Exe,
    [int]$RowSample = 6
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$p = Start-Process $Exe -PassThru
Write-Output "launched pid=$($p.Id)"

function Get-Window($pid_) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    for ($i = 0; $i -lt 60; $i++) {
        $w = $AE::RootElement.FindFirst($TS::Children, $cond)
        if ($w) { return $w }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

$win = Get-Window $p.Id
if (-not $win) { Write-Output 'WINDOW NOT FOUND'; exit 1 }
Start-Sleep -Seconds 14   # 等刷新循环到稳态

function Find-ById($root, [string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    return $root.FindFirst($TS::Descendants, $c)
}

# 列表行：取 ProcessList 下所有 ListItem，读它们的 Name。
# 行模板没有 x:Name，所以只能靠 ListItem 暴露的合成名字（它会把 4 个 TextBlock
# 的文本拼起来），这也正好能验证「值确实渲染出来了」。
function Get-Rows($win, [int]$n) {
    $list = Find-ById $win 'ProcessList'
    if (-not $list) { return @() }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
    $items = $list.FindAll($TS::Descendants, $cond)
    $out = @()
    for ($i = 0; $i -lt [Math]::Min($n, $items.Count); $i++) {
        $out += $items[$i].Current.Name
    }
    return $out
}

function Get-Status($win) {
    $l = Find-ById $win 'StatusLeft'
    $r = Find-ById $win 'StatusRight'
    return @{ left = $(if ($l) { $l.Current.Name } else { '(none)' });
              right = $(if ($r) { $r.Current.Name } else { '(none)' }) }
}

function Invoke-El($win, [string]$id) {
    $el = Find-ById $win $id
    if (-not $el) { return "NOT FOUND: $id" }
    $pat = $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pat.Invoke()
    return 'ok'
}

Write-Output ''
Write-Output '=== BASELINE (default: sort by memory desc) ==='
$rows0 = Get-Rows $win $RowSample
$rows0 | ForEach-Object { Write-Output "  $_" }

Write-Output ''
Write-Output '=== TEST 1: click CPU header -> expect reorder ==='
Write-Output ("  invoke HeaderCpu: " + (Invoke-El $win 'HeaderCpu'))
Start-Sleep -Seconds 3
$rows1 = Get-Rows $win $RowSample
$rows1 | ForEach-Object { Write-Output "  $_" }
Write-Output ("  changed: " + (($rows0 -join '|') -ne ($rows1 -join '|')))

Write-Output ''
Write-Output '=== TEST 2: click Memory header -> expect back to memory desc ==='
Write-Output ("  invoke HeaderMem: " + (Invoke-El $win 'HeaderMem'))
Start-Sleep -Seconds 3
$rows2 = Get-Rows $win $RowSample
$rows2 | ForEach-Object { Write-Output "  $_" }

Write-Output ''
Write-Output '=== TEST 3: search box -> type "edge" ==='
$box = Find-ById $win 'SearchBox'
$edit = $null
if ($box) {
    $ec = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
    $edit = $box.FindFirst($TS::Descendants, $ec)
}
if (-not $edit) {
    Write-Output '  EDIT NOT FOUND'
} else {
    $vp = $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $vp.SetValue('edge')
    Start-Sleep -Seconds 3
    Write-Output '  rows after filter:'
    Get-Rows $win $RowSample | ForEach-Object { Write-Output "    $_" }
    $st = Get-Status $win
    Write-Output ("  StatusLeft: " + $st.left)
}

Write-Output ''
Write-Output '=== FINAL STATUS BAR ==='
$st = Get-Status $win
Write-Output ("  left : " + $st.left)
Write-Output ("  right: " + $st.right)
$p.Refresh()
Write-Output ("  priv=$([math]::Round($p.PrivateMemorySize64/1MB,1))MB ws=$([math]::Round($p.WorkingSet64/1MB,1))MB handles=$($p.HandleCount)")
