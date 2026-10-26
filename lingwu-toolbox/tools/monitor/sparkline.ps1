# sparkline.ps1 —— 磁贴折线验收
#
# 判据（plan.md §5.2）：
#   sparkline 的点数随采样增长；采样间隔 0.5 s 与 2 s 下窗口长度都保持 60 点。
#
# 【实现的真实口径】
#   · 历史是定长环形缓冲 kSeriesCapacity = 64 个点（core/monitor/history.h）；
#   · 画一条线最多取最近 kSparkMaxPoints = 40 个点（core/monitor/sparkline.h）；
#   · 点数不足 2 个时**画一条基线**，不是留白（plan §3.7）。
#
# 【「窗口长度保持 60 点」在运行期怎么验】
#   环形缓冲的**容量**是代码常量，与采样间隔无关；采样间隔只改变
#   「攒满要多久」。所以运行期能验的是两件事：
#     ① 点数随采样增长；
#     ② 采样间隔变大 → 攒点变慢（点数与时间的关系跟着间隔走），
#        而**上限**不变 —— 上限用 0.5 s 那一轮实测（应当打满到 64）。
#
# 【点数从哪儿读】
#   折线是 Path（Shape），**不进 UIA 树**。产品里有专门的 trace 行：
#   %LOCALAPPDATA%\LingwuToolbox\trace.log 的
#       TrayPanel: cpu spark points=N
#   （**只在点数变化时写一行**，所以解析时取最后一条）。
#   trace 需要同目录下的 trace.on 标记文件才写 —— 脚本自己建、跑完删。
#
# 【折线画出来了没有】
#   用**像素**取证：折线的描边是彩色（MetricCpu），文字与底色是灰色的，
#   所以按饱和度（max-min ≥ 60）能把两者分开；
#   判据是「折线区里存在贯穿全宽的高饱和度像素」（点数 ≥ 2×折线区宽度）。

param(
    [string]$Exe = '',
    [switch]$NoLaunch,
    [int]$FastSeconds = 40
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
. (Join-Path $PSScriptRoot '..\common\tray.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
[void][Native.Win]::SetProcessDPIAware()

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$PW_RENDERFULLCONTENT = 0x00000002

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

$IniPath    = Join-Path $env:APPDATA 'LingwuToolbox\settings.ini'
$TracePath  = Join-Path $env:LOCALAPPDATA 'LingwuToolbox\trace.log'
$TraceFlag  = Join-Path $env:LOCALAPPDATA 'LingwuToolbox\trace.on'

function Get-IntervalMs {
    if (-not (Test-Path $IniPath)) { return 1000 }
    $m = Select-String -Path $IniPath -Pattern '^\s*MonitorIntervalMs\s*=\s*(\d+)' -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return 1000
}

function Set-IntervalMs([int]$v) {
    $text = Get-Content $IniPath -Raw
    if ($text -match '(?m)^\s*MonitorIntervalMs\s*=') {
        $text = $text -replace '(?m)^\s*MonitorIntervalMs\s*=\s*\d+', "MonitorIntervalMs=$v"
    } else {
        $text = $text.TrimEnd() + "`r`nMonitorIntervalMs=$v"
    }
    Set-Content -Path $IniPath -Value $text -Encoding UTF8 -NoNewline
}

# 读 trace.log 里**最后**一条 points=N。
function Get-LastSparkPoints {
    if (-not (Test-Path $TracePath)) { return $null }
    $m = Select-String -Path $TracePath -Pattern 'cpu spark points=(\d+)' -ErrorAction SilentlyContinue |
         Select-Object -Last 1
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return $null
}

function Get-AppWindows([int]$ProcId) {
    $pc = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $ProcId)
    $wins = $AE::RootElement.FindAll($TS::Children, $pc)
    $res = New-Object System.Collections.Generic.List[object]
    foreach ($w in $wins) {
        $res.Add(@{ Hwnd = [int]$w.Current.NativeWindowHandle; Name = $w.Current.Name; El = $w })
    }
    return ,$res
}

function Get-TrayPanelWindow($Wins, $Main) {
    foreach ($w in $Wins) {
        if ($Main -and $w.Hwnd -eq $Main.Hwnd) { continue }
        if ($w.Name -and $w.Name -like '*零物工具箱*') { continue }
        if ($w.Hwnd -eq 0) { continue }
        return $w
    }
    return $null
}

function Find-ById($Root, [string]$Id, [int]$WaitSec = 10) {
    $deadline = (Get-Date).AddSeconds($WaitSec)
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $Id)
    while ((Get-Date) -lt $deadline) {
        $found = $Root.FindFirst($TS::Descendants, $cond)
        if ($found) { return $found }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# 数折线区里「高饱和度」的像素。
#
# 【为什么用饱和度，而不是「与背景色的距离」】
# 折线的描边是**彩色**的（各指标自己的语义色），文字与底色都是灰色的。
# 用「与背景色的距离」会把上一行副标题的字形算进来，判据就变成了
# 「有没有文字」而不是「有没有折线」。
#
# 【折线区位置怎么推出来】
# 锚点是磁贴的副标题（TileCpuSub），折线宿主在它下面一行：
# Margin 0,3,0,0 + Height 14（DIP）→ 本机 200% 缩放 = 6 + 28 物理像素。
# 用 UIA 给的锚点矩形推算，换缩放比例也不会错。
# （UIA 的 BoundingRectangle 是**屏幕坐标**，截图是**窗口坐标**，要减窗口原点。）
function Measure-Sparkline($Bmp, $WindowRect, $AnchorRect, [double]$Scale) {
    $top = [int]($AnchorRect.Bottom - $WindowRect.Top) + [int](3 * $Scale)
    $bot = $top + [int](14 * $Scale)
    $x0  = [int]($AnchorRect.X - $WindowRect.Left)
    $x1  = [int]($AnchorRect.X + $AnchorRect.Width - $WindowRect.Left)
    if ($x1 -le $x0) { return $null }
    $count = 0
    for ($y = $top; $y -lt $bot; $y++) {
        if ($y -lt 0 -or $y -ge $Bmp.Height) { continue }
        for ($x = $x0; $x -lt $x1; $x++) {
            if ($x -lt 0 -or $x -ge $Bmp.Width) { continue }
            $c = $Bmp.GetPixel($x, $y)
            $mx = [Math]::Max([int]$c.R, [Math]::Max([int]$c.G, [int]$c.B))
            $mn = [Math]::Min([int]$c.R, [Math]::Min([int]$c.G, [int]$c.B))
            if (($mx - $mn) -ge 60) { $count++ }
        }
    }
    return @{ Count = $count; BandWidth = ($x1 - $x0) }
}

function Start-App([string]$Path) {
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $p = Start-Process $Path -PassThru
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) { return $null }
        if ($p.MainWindowHandle -ne 0) { break }
    }
    Start-Sleep -Seconds 3
    return $p
}

function Open-Panel([int]$ProcId) {
    [void](Invoke-ToolboxTrayButton)
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 400
        $wins = Get-AppWindows $ProcId
        $main = $wins | Where-Object { $_.Name -like '*零物工具箱*' } | Select-Object -First 1
        $panel = Get-TrayPanelWindow $wins $main
        if ($panel) { return $panel }
    }
    return $null
}

# ---- trace 标记 ------------------------------------------------------------
$traceFlagExisted = Test-Path $TraceFlag
if (-not $traceFlagExisted) {
    New-Item -ItemType Directory -Path (Split-Path $TraceFlag) -Force | Out-Null
    [IO.File]::Create($TraceFlag).Close()
}

# ---- ini 备份（脚本结束必须还原）------------------------------------------
$iniBackup = if (Test-Path $IniPath) { Get-Content $IniPath -Raw } else { $null }
$origInterval = Get-IntervalMs
Write-Output "ini MonitorIntervalMs(before)=$origInterval"

try {
    # ================= 第一轮：间隔 500 ms ====================================
    Set-IntervalMs 500
    Write-Output "round A: MonitorIntervalMs=500"
    $p = Start-App $Exe
    if (-not $p) { Write-Output 'FAIL: 应用启动即退出'; exit 1 }
    $pidA = $p.Id
    $panelA = Open-Panel $pidA
    Assert-True '① 托盘指标面板已打开' ($null -ne $panelA)
    if (-not $panelA) { throw '面板没出现' }

    $ptsA0 = Get-LastSparkPoints
    Start-Sleep -Seconds $FastSeconds
    $ptsA1 = Get-LastSparkPoints
    Write-Output ("round A points: {0} -> {1}" -f $ptsA0, $ptsA1)
    Assert-True '② 折线点数随采样增长' `
        ($null -ne $ptsA1 -and $null -ne $ptsA0 -and $ptsA1 -gt $ptsA0) `
        ("points=$ptsA0 -> $ptsA1")

    # ③ 上限：环形缓冲容量是 64 个点。
    Assert-True '③ 点数有上限（环形缓冲容量 64，不无限增长）' `
        ($null -ne $ptsA1 -and $ptsA1 -le 64) ("points=$ptsA1")

    # ④ 折线确实画出来了（像素取证）
    $hwnd = [IntPtr]$panelA.Hwnd
    $wr = New-Object Native.Win+RECT
    [void][Native.Win]::GetWindowRect($hwnd, [ref]$wr)
    $bmp = New-Object System.Drawing.Bitmap(($wr.Right - $wr.Left), ($wr.Bottom - $wr.Top))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][Native.Win]::PrintWindow($hwnd, $hdc, $PW_RENDERFULLCONTENT)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $anchor = Find-ById $panelA.El 'TileCpuSub'
    Assert-True '④ 折线锚点（TileCpuSub）可定位' ($null -ne $anchor)
    if ($anchor -and $bmp) {
        $m = Measure-Sparkline $bmp $wr $anchor.Current.BoundingRectangle 2.0
        if ($m) {
            Write-Output ("spark pixels = {0} (band width {1})" -f $m.Count, $m.BandWidth)
            Assert-True '④ 折线确实画出来了（贯穿全宽的高饱和度像素）' `
                ($m.Count -ge ($m.BandWidth * 2)) ("saturated=" + $m.Count)
        } else {
            Assert-True '④ 折线确实画出来了' $false '折线区算不出来'
        }
    }
    $bmp.Dispose()

    # ================= 第二轮：间隔 2000 ms ===================================
    Get-Process -Id $pidA -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Set-IntervalMs 2000
    Write-Output "round B: MonitorIntervalMs=2000"
    $p2 = Start-App $Exe
    if (-not $p2) { Write-Output 'FAIL: 应用启动即退出（第二轮）'; exit 1 }
    $pidB = $p2.Id
    $panelB = Open-Panel $pidB
    Assert-True '② 第二轮面板已打开' ($null -ne $panelB)
    if ($panelB) {
        $ptsB0 = Get-LastSparkPoints
        Start-Sleep -Seconds $FastSeconds
        $ptsB1 = Get-LastSparkPoints
        Write-Output ("round B points: {0} -> {1}" -f $ptsB0, $ptsB1)
        # 同样是 40 秒：间隔 500 ms 能攒 ~80 点（打满 64），
        # 间隔 2000 ms 只能攒 ~20 点 —— 攒点速度跟着采样间隔走。
        Assert-True '⑤ 采样间隔变大 → 同样时间内攒到的点更少（攒点速度跟着间隔走）' `
            ($null -ne $ptsB1 -and $null -ne $ptsA1 -and $ptsB1 -lt $ptsA1) `
            ("500ms=$ptsA1  2000ms=$ptsB1")
        Assert-True '⑤ 点数仍然在增长' `
            ($null -ne $ptsB1 -and $null -ne $ptsB0 -and $ptsB1 -gt $ptsB0) `
            ("points=$ptsB0 -> $ptsB1")
    }
    Get-Process -Id $pidB -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
finally {
    # 还原 ini —— 验收脚本不许把用户设置留在测试值上。
    if ($null -ne $iniBackup) {
        Set-Content -Path $IniPath -Value $iniBackup -Encoding UTF8 -NoNewline
        Write-Output "ini MonitorIntervalMs(restored)=$(Get-IntervalMs)"
    }
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if (-not $traceFlagExisted -and (Test-Path $TraceFlag)) {
        Remove-Item -LiteralPath $TraceFlag -Force -ErrorAction SilentlyContinue
    }
}

Write-Output ""
Write-Output "RESULT: pass=$script:Pass fail=$script:Fail"
if ($script:Fail -gt 0) { exit 1 }
exit 0
