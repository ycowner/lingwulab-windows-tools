param(
    [string]$Exe,
    [string]$OutFile = "$env:TEMP\lingsysmon_uia.txt",
    [int]$MaxDepth = 4
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$p = Start-Process $Exe -PassThru
Write-Host "launched pid=$($p.Id)"

# 等 UIA 能看到窗口（XAML 的可访问性树要等可视化树建好才出来）
$win = $null
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
    $win = $AE::RootElement.FindFirst($TS::Children, $cond)
    if ($win) { break }
}
if (-not $win) { Write-Host 'WINDOW NOT FOUND VIA UIA'; exit 1 }

# 给刷新循环留时间跑到稳态
Start-Sleep -Seconds 12

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("WINDOW name='$($win.Current.Name)' class='$($win.Current.ClassName)'")
$lines.Add("rect=$($win.Current.BoundingRectangle)")

# 矩形要防 ±∞：列表里被虚拟化（已回收/未实现）的元素 BoundingRectangle 会是
# double.NegativeInfinity，直接 [int] 会抛
#   「无法将值 "-∞" 转换为类型 System.Int32」
# 整个 dump 就断在那里。夹到 int 范围内即可，反正这些元素本来就没位置。
function Clamp-Int([double]$v) {
    if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { return -1 }
    if ($v -lt [int]::MinValue) { return [int]::MinValue }
    if ($v -gt [int]::MaxValue) { return [int]::MaxValue }
    return [int]$v
}

function Dump-Node {
    param($el, [int]$depth)
    if ($depth -gt $MaxDepth) { return }
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $child = $walker.GetFirstChild($el)
    while ($child) {
        $c = $child.Current
        $r = $c.BoundingRectangle
        $script:lines.Add(('  ' * $depth) +
            "$($c.ControlType.ProgrammaticName) name='$($c.Name)' " +
            "aid='$($c.AutomationId)' rect=$(Clamp-Int $r.X),$(Clamp-Int $r.Y)," +
            "$(Clamp-Int $r.Width)x$(Clamp-Int $r.Height)")
        Dump-Node -el $child -depth ($depth + 1)
        $child = $walker.GetNextSibling($child)
    }
}

Dump-Node -el $win -depth 1
$lines | Set-Content -Path $OutFile -Encoding UTF8
Write-Host "dumped $($lines.Count) nodes to $OutFile"
Write-Host "OUT=$OutFile"
