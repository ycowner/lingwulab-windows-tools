param(
    [string]$Exe,
    [int]$Seconds = 150,
    [int]$Every = 10
)

# 目的：把「私有内存曲线」与「真实行数」对齐，用来判断内存阶跃
# 到底是「行数变多」还是「框架预热 / 排序churn」。
#
# 行数不用 UIA 数 ListItem（虚拟化，只能数到可见的那些），
# 直接读状态栏文本 —— 聚合模式下「N 进程 · M 应用」的 M 就是行数。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
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

function Find-ById($root, [string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    return $root.FindFirst($TS::Descendants, $c)
}

function Get-StatusText($win) {
    $l = Find-ById $win 'StatusLeft'
    if ($l) { return $l.Current.Name } else { return '(none)' }
}

Write-Output ("{0,-8} {1,-9} {2,-9} {3,-6} {4}" -f 't', 'privMB', 'wsMB', 'h', 'status')
$prev = $null
for ($t = 1; $t -le $Seconds; $t++) {
    Start-Sleep -Seconds 1
    $live = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $live) { Write-Output "t=${t}s PROCESS GONE"; break }
    if ($t % $Every -ne 0 -and $t -ne 1) { continue }
    $live.Refresh()
    $priv = [math]::Round($live.PrivateMemorySize64 / 1MB, 1)
    $ws   = [math]::Round($live.WorkingSet64 / 1MB, 1)
    $h    = $live.HandleCount
    $st   = Get-StatusText $win
    $d    = if ($null -ne $prev) { [math]::Round($priv - $prev, 1) } else { 0.0 }
    Write-Output ("{0,-8} {1,-9} {2,-9} {3,-6} {4}   (d={5:+0.0;-0.0})" -f "${t}s", $priv, $ws, $h, $st, $d)
    $prev = $priv
}

$p.Refresh()
Write-Output ("FINAL priv={0}MB ws={1}MB handles={2}" -f `
    [math]::Round($p.PrivateMemorySize64/1MB,1), [math]::Round($p.WorkingSet64/1MB,1), $p.HandleCount)
