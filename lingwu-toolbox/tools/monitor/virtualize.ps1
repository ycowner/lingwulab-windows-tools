param(
    [string]$Exe,
    [int]$MaxRealizedRows = 40,
    [int]$MaxViewSizePct = 25
)

# 验证列表的虚拟化「真的到底了」。
#
# 【为什么这个脚本是 2026-10-25 新加的】
# 为了压常驻内存，两个列表的 ItemsPanel 显式写成了
#     <ItemsStackPanel Orientation="Vertical" CacheLength="0" />
# 也就是「视口之外一行都不预先实现」。这个改动把监控页一次实现的行容器
# 从 86 个降到 18 个，25 轮互切峰值从 192.7 MB 降到 134.0 MB。
#
# 但这种改动的失败模式是**静默**的：面板如果把 extent 只按已实现的行算，
# 列表就会滚不到底，而且界面上完全看不出异常（照样有行、照样能滚几屏）。
# 所以这里用 UIA 的 ScrollPattern 断言两件事：
#
#   1) 滚到底后 VerticalScrollPercent ≈ 100 —— 证明内容总长是完整的；
#   2) VerticalViewSize 很小 —— 证明「视口/总长」很小，即总长确实是
#      全部行（192 行 ≈ 6144 DIP）的长度，而不是只有已实现那十几行的长度。
#      如果面板只按已实现行算 extent，这个比例会接近 90% 甚至 100%，
#      断言会立刻失败。
#
# 顺带断言已实现行数保持在上限以内 —— 这是内存红线的直接护栏：
# 哪天有人把 CacheLength 改回去，这里会先红。

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

function Find-ById($root, [string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    return $root.FindFirst($TS::Descendants, $c)
}

function Get-RealizedRows($root, [string]$listId) {
    $list = Find-ById $root $listId
    if (-not $list) { return @() }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
    $items = $list.FindAll($TS::Descendants, $cond)
    $out = @()
    for ($i = 0; $i -lt $items.Count; $i++) {
        $out += $items[$i].Current.Name
    }
    return ,$out
}

function Get-StatusText($root, [string]$id) {
    $el = Find-ById $root $id
    if ($el) { return $el.Current.Name }
    return '(none)'
}

function Invoke-El($root, [string]$id) {
    $el = Find-ById $root $id
    if (-not $el) { return "NOT FOUND: $id" }
    $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    return 'ok'
}

# 把某个列表滚到底。ScrollAmount 枚举里没有 Top/Bottom，
# 只有 LargeDecrement/SmallDecrement/NoAmount/LargeIncrement/SmallIncrement，
# 所以「滚到底」只能反复 LargeIncrement，直到滚动百分比不再变化。
function Scroll-To-Bottom($root, [string]$listId) {
    $list = Find-ById $root $listId
    if (-not $list) { return $null }
    $sp = $null
    try { $sp = $list.GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern) } catch {}
    if (-not $sp) { return $null }

    $last = -1.0
    for ($i = 0; $i -lt 60; $i++) {
        if (-not $sp.Current.VerticallyScrollable) { break }
        $before = $sp.Current.VerticalScrollPercent
        if ($before -ge 100.0) { break }
        $sp.ScrollVertical([System.Windows.Automation.ScrollAmount]::LargeIncrement)
        Start-Sleep -Milliseconds 250
        $after = $sp.Current.VerticalScrollPercent
        if ([math]::Abs($after - $before) -lt 0.01) {
            # 再也推不动了 —— 到底了。
            $last = $after
            break
        }
        $last = $after
    }
    Start-Sleep -Milliseconds 800
    return [pscustomobject]@{
        percent  = [math]::Round($sp.Current.VerticalScrollPercent, 1)
        viewSize = [math]::Round($sp.Current.VerticalViewSize, 1)
        last     = $last
    }
}

function Test-List([string]$label, [string]$listId) {
    Write-Output ''
    Write-Output ("=== {0}: {1} ===" -f $label, $listId)

    $rowsTop = Get-RealizedRows $win $listId
    Write-Output ("  realized at top   : {0}" -f $rowsTop.Count)
    Assert-True ("{0} 顶部有已实现的行" -f $label) ($rowsTop.Count -gt 0) "rows=$($rowsTop.Count)"
    Assert-True ("{0} 已实现行数在上限内" -f $label) ($rowsTop.Count -le $MaxRealizedRows) `
        ("rows={0} limit={1}" -f $rowsTop.Count, $MaxRealizedRows)

    $st = Get-StatusText $win 'StatusLeft'
    Write-Output ("  StatusLeft        : {0}" -f $st)

    $res = Scroll-To-Bottom $win $listId
    if (-not $res) {
        Assert-True ("{0} 列表支持 ScrollPattern" -f $label) $false
        return
    }
    Write-Output ("  scroll percent    : {0}" -f $res.percent)
    Write-Output ("  view size pct     : {0}" -f $res.viewSize)

    $rowsBottom = Get-RealizedRows $win $listId
    Write-Output ("  realized at bottom: {0}" -f $rowsBottom.Count)
    if ($rowsBottom.Count -gt 0) {
        Write-Output ("  last realized row : {0}" -f $rowsBottom[$rowsBottom.Count - 1])
    }

    Assert-True ("{0} 能滚到底" -f $label) ($res.percent -ge 99.0) "percent=$($res.percent)"
    # 视口占总长的比例必须很小 —— 这才是「总长是全部行、不是已实现那几行」的证据。
    Assert-True ("{0} 滚动总长覆盖全部行（视口占比小）" -f $label) ($res.viewSize -le $MaxViewSizePct) `
        ("viewSize={0}% limit={1}%" -f $res.viewSize, $MaxViewSizePct)
    Assert-True ("{0} 滚到底后仍有行渲染" -f $label) ($rowsBottom.Count -gt 0) "rows=$($rowsBottom.Count)"
    Assert-True ("{0} 滚到底后已实现行数仍在上限内" -f $label) ($rowsBottom.Count -le $MaxRealizedRows) `
        ("rows={0} limit={1}" -f $rowsBottom.Count, $MaxRealizedRows)
    # 滚到底后行集合应该和顶部不同 —— 证明滚动真的换了内容，
    # 而不是 ScrollPattern 报了个百分比但视图没动。
    $changed = (($rowsTop -join '|') -ne ($rowsBottom -join '|'))
    Assert-True ("{0} 滚动后内容确实换了" -f $label) $changed
}

$win = Get-Window $p.Id
if (-not $win) { Write-Output 'WINDOW NOT FOUND'; exit 1 }
Start-Sleep -Seconds 14   # 等刷新循环到稳态

Test-List '监控页' 'ProcessList'

Write-Output ''
Write-Output '=== switch to the service page ==='
$inv = Invoke-El $win 'NavServiceButton'
Write-Output ("  invoke NavServiceButton: " + $inv)
Assert-True '切得到服务页' ($inv -eq 'ok') $inv
Start-Sleep -Seconds 8

Test-List '服务页' 'ServiceList'

Write-Output ''
Write-Output '=== memory ==='
$p.Refresh()
$priv = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
$ws   = [math]::Round($p.WorkingSet64 / 1MB, 1)
Write-Output ("  priv={0}MB ws={1}MB handles={2}" -f $priv, $ws, $p.HandleCount)
# 红线只卡私有内存；工作集只记录（见 AGENTS.md 的「内存口径」）。
Assert-True '滚动到底后私有内存没有超过 220 MB 红线' ($priv -le 220.0) "priv=${priv}MB"

Write-Output ''
Write-Output '=========================================================='
Write-Output ("RESULT: pass={0}  fail={1}" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 } else { exit 0 }
