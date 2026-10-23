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

# 断言计数器。
#
# 【为什么这个脚本原来只在「打印所见」】
# 它原本把每一步的结果直接打印出来，从不断言，也从不返回非零 ——
# 于是「一切正常」和「脚本跑完但什么都没验成」在验收板上长得一模一样
# （都是 ok=0 fail=0 EXIT0）。现在每一步都有断言，板上才有数。
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
Assert-True 'BASELINE 列表有已实现的行' ($rows0.Count -gt 0) "rows=$($rows0.Count)"

Write-Output ''
Write-Output '=== TEST 1: click CPU header -> expect reorder ==='
$inv1 = Invoke-El $win 'HeaderCpu'
Write-Output ("  invoke HeaderCpu: " + $inv1)
Assert-True 'TEST 1 点得动 CPU 表头' ($inv1 -eq 'ok') $inv1
Start-Sleep -Seconds 3
$rows1 = Get-Rows $win $RowSample
$rows1 | ForEach-Object { Write-Output "  $_" }
$changed1 = ($rows0 -join '|') -ne ($rows1 -join '|')
Write-Output ("  changed: " + $changed1)
Assert-True 'TEST 1 列表按 CPU 重新排序' $changed1

Write-Output ''
Write-Output '=== TEST 2: click Memory header -> expect back to memory desc ==='
$inv2 = Invoke-El $win 'HeaderMem'
Write-Output ("  invoke HeaderMem: " + $inv2)
Assert-True 'TEST 2 点得动内存表头' ($inv2 -eq 'ok') $inv2
Start-Sleep -Seconds 3
$rows2 = Get-Rows $win $RowSample
$rows2 | ForEach-Object { Write-Output "  $_" }
$changed2 = ($rows1 -join '|') -ne ($rows2 -join '|')
Write-Output ("  changed: " + $changed2)
Assert-True 'TEST 2 列表按内存重新排序' $changed2

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
    Assert-True 'TEST 3 找得到搜索框' $false
} else {
    Assert-True 'TEST 3 找得到搜索框' $true
    $vp = $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $vp.SetValue('edge')
    Start-Sleep -Seconds 3
    Write-Output '  rows after filter:'
    $rows3 = Get-Rows $win $RowSample
    $rows3 | ForEach-Object { Write-Output "    $_" }
    $st = Get-Status $win
    Write-Output ("  StatusLeft: " + $st.left)
    # 过滤后可见的行都应该是命中 'edge' 的那些（行的 Name 是四列拼起来的
    # 合成名，所以整串里出现 edge 就算命中）。
    $off = @($rows3 | Where-Object { $_ -notmatch 'edge' })
    Assert-True 'TEST 3 过滤后剩下的行全都命中 edge' ($off.Count -eq 0) "off=$($off.Count)"
    # 过滤了多少条要读状态栏 —— 列表是虚拟化的，UIA 只能看到已实现的那几行。
    Assert-True 'TEST 3 状态栏报告了筛选后的计数' ($st.left -match '\d') "left=$($st.left)"
}

Write-Output ''
Write-Output '=== FINAL STATUS BAR ==='
$st = Get-Status $win
Write-Output ("  left : " + $st.left)
Write-Output ("  right: " + $st.right)
$p.Refresh()
Write-Output ("  priv=$([math]::Round($p.PrivateMemorySize64/1MB,1))MB ws=$([math]::Round($p.WorkingSet64/1MB,1))MB handles=$($p.HandleCount)")
Assert-True '状态栏两段都有内容' (($st.left -ne '(none)') -and ($st.right -ne '(none)'))

Write-Output ''
Write-Output '=========================================================='
Write-Output ("RESULT: pass={0}  fail={1}" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 } else { exit 0 }
