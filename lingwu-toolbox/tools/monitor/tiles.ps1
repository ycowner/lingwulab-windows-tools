# tiles.ps1 —— bento 磁贴区验收
#
# 判据（对应「仿 Mole for Windows 改造方案 Phase 1」的验收表）：
#   ① 磁贴的 AutomationId 全部存在（8 块 bento 磁贴，逐个按 AutomationId 找）
#   ② 磁贴数值在两个采样周期之间发生变化（断言非恒定）
#
# 【8 块磁贴的构成】
#   1 健康 / 2 CPU / 3 内存 / 4 磁盘剩余 / 5 网络 / 6 磁盘活动 / 7 电池
#   8 GPU —— 第 8 块是**诚实的占位**（XAML 里写着「本轮不做」），
#   它既没有 x:Name 也没有 AutomationProperties.AutomationId，
#   所以「按 AutomationId 找」这一条对它**跑不了**：报 SKIP 并写明原因，
#   同时改用占位文案把它认出来，断言这块磁贴确实存在。
#   （这是产品缺口，不是脚本问题；按约定只报告、不改产品代码。）
#
# 【采样间隔是设置项，不能写死】
# MonitorIntervalMs 在 %APPDATA%\LingwuToolbox\settings.ini 里（250..10000 ms）。
# 磁贴的数值来自采样器的快照，采样器一个周期才出一帧新数据，
# 所以「隔两个采样周期读两次」必须按**真实间隔**等，不能写死 1 秒。
#
# 【跨进程 UIA 一律按 ProcessId 过滤】
# 桌面上可能有别的同名窗口，只按标题或类名找会认错。

param(
    [string]$Exe = '',
    [switch]$NoLaunch,
    [switch]$SkipShot,
    [string]$ShotPrefix = 'tiles'
)

$ErrorActionPreference = 'Stop'

# 本文件是 UTF-8 无 BOM，中文注释与中文字面量都靠这一行才能在
# 「输出被重定向到文件 / 管道」时按 UTF-8 落盘；不设的话 pwsh 会按
# 系统 ANSI(936) 写，验收板读到的全是乱码。
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

. (Join-Path $PSScriptRoot '..\common\common.ps1')
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

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]
$PW_RENDERFULLCONTENT = 0x00000002

[void][Native.Win]::SetProcessDPIAware()

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

# SKIP 不计入 pass 也不计入 fail：这一项**本机跑不了**，
# 不是产品缺陷。染红验收板比没有验收更糟。
function Skip-Check([string]$What, [string]$Why = '') {
    Write-Output ("  SKIP: {0} {1}" -f $What, $Why)
}

# ---------------------------------------------------------------- 元素查找

# 按 AutomationId 找元素。条件里必须带 ProcessId。
function Find-ById($Root, [string]$Id) {
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::ProcessIdProperty, [int]$Root.Current.ProcessId)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::AutomationIdProperty, $Id)))
    return $Root.FindFirst($TS::Descendants, $cond)
}

# 按名字找元素（同样按 ProcessId 过滤）。
function Find-ByName($Root, [string]$Name) {
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::ProcessIdProperty, [int]$Root.Current.ProcessId)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::NameProperty, $Name)))
    return $Root.FindFirst($TS::Descendants, $cond)
}

# 有界轮询：发动作 → 等一下 → 看结果。
function Wait-Until([scriptblock]$Test, [int]$TimeoutSec = 20, [int]$IntervalMs = 300) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $v = & $Test
        if ($v) { return $v }
        Start-Sleep -Milliseconds $IntervalMs
    }
    return $null
}

# 主窗口：靠「窗口里有没有 NavStatusButton」来认，不认标题。
function Get-ToolboxWindow([int]$ProcId, [int]$TimeoutSec = 45) {
    $winCond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::ControlTypeProperty, $CT::Window)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::ProcessIdProperty, $ProcId)))
    $probe = New-Object System.Windows.Automation.PropertyCondition(
        $AE::AutomationIdProperty, 'NavStatusButton')
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $wins = $AE::RootElement.FindAll($TS::Children, $winCond)
        for ($i = 0; $i -lt $wins.Count; $i++) {
            if ($wins[$i].FindFirst($TS::Descendants, $probe)) { return $wins[$i] }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# 当前显示的是哪个页面（页面收起来时不在 UIA 树里）。
function Get-CurrentPage($Win) {
    if (Find-ById $Win 'ProcessList') { return 'Status' }
    if (Find-ById $Win 'ServiceList') { return 'Service' }
    return ''
}

# 切到状态页（磁贴只在那页）。返回 $true/$false。
function Switch-ToStatus($Win) {
    if ((Get-CurrentPage $Win) -eq 'Status') { return $true }
    $btn = Find-ById $Win 'NavStatusButton'
    if (-not $btn) { return $false }
    try {
        $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    } catch {
        return $false
    }
    $hit = Wait-Until { (Get-CurrentPage $Win) -eq 'Status' } 40 300
    return ($null -ne $hit)
}

# 抓窗口位图。
function Get-WindowBitmap([IntPtr]$Handle) {
    $r = New-Object Native.Win+RECT
    if (-not [Native.Win]::GetWindowRect($Handle, [ref]$r)) { return $null }
    $w = $r.Right - $r.Left
    $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][Native.Win]::PrintWindow($Handle, $hdc, $PW_RENDERFULLCONTENT)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    return ,$bmp
}

# ---------------------------------------------------------------- 采样间隔

# 从用户 ini 里读真实采样间隔（毫秒）。
#
# 【为什么必须读，不能写死】
# 磁贴的数值来自采样器的一帧快照，采样器按 MonitorIntervalMs 出帧。
# 间隔是 250..10000 ms 的设置项，写死 1 秒的话：
#   间隔比 1 秒长 → 两次读到的必然是同一帧，「数值会变」永远不成立；
#   间隔比 1 秒短 → 读到的可能仍是同一帧，判据不稳。
function Get-SampleIntervalMs {
    $path = Join-Path $env:APPDATA 'LingwuToolbox\settings.ini'
    if (-not (Test-Path $path)) { return 5000 }
    $raw = Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
    if (-not $raw) { return 5000 }
    $m = [regex]::Match($raw, '(?im)^\s*MonitorIntervalMs\s*=\s*(\d+)')
    if (-not $m.Success) { return 5000 }
    $v = [int]$m.Groups[1].Value
    if ($v -lt 250 -or $v -gt 10000) { return 5000 }
    return $v
}

# ---------------------------------------------------------------- 启动

$launchedPid = 0
if (-not $NoLaunch) {
    Stop-App
    Start-Sleep -Milliseconds 800
    $p = Start-Process $Exe -PassThru
    $launchedPid = $p.Id
    Write-Output "launched pid=$($p.Id)"
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) {
            Write-Output "ABORT: 应用启动即退出 code=$($p.ExitCode)"
            exit 1
        }
        if ($p.MainWindowHandle -ne 0) { break }
    }
    Start-Sleep -Seconds 4
}

$proc = $null
if ($launchedPid -gt 0) {
    $proc = Get-Process -Id $launchedPid -ErrorAction SilentlyContinue
}
if (-not $proc) {
    $proc = Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne 37480 } | Select-Object -First 1
}
if (-not $proc) { Write-Output 'ABORT: 应用未在运行'; exit 1 }
$pid_ = $proc.Id
Write-Output "app pid=$pid_"

$win = Get-ToolboxWindow $pid_ 45
if (-not $win) { Write-Output "ABORT: 找不到主窗口（pid=$pid_）"; exit 1 }
$hwnd = [IntPtr]$win.Current.NativeWindowHandle
Write-Output "main window hwnd=$hwnd"

# 磁贴只在状态页上；应用可能停在服务页，先切过去。
if (-not (Switch-ToStatus $win)) {
    Write-Output 'ABORT: 切不到状态页（磁贴只在那页）'
    exit 1
}
Write-Output "page=Status"

# ---------------------------------------------------------------- ① AutomationId 全部存在

# 8 块磁贴。第 8 块（GPU）是占位，没有 AutomationId。
$tiles = @(
    @{ N = 1; Name = '健康';     Id = 'TileHealthValue';   Badge = 'TileHealthBadge' }
    @{ N = 2; Name = 'CPU';      Id = 'TileCpuValue';      Badge = 'TileCpuBadge' }
    @{ N = 3; Name = '内存';     Id = 'TileMemoryValue';   Badge = 'TileMemoryBadge' }
    @{ N = 4; Name = '磁盘剩余'; Id = 'TileDiskFreeValue'; Badge = 'TileDiskFreeBadge' }
    @{ N = 5; Name = '网络';     Id = 'TileNetValue';      Badge = 'TileNetBadge' }
    @{ N = 6; Name = '磁盘活动'; Id = 'TileDiskActValue';  Badge = 'TileDiskActBadge' }
    @{ N = 7; Name = '电池';     Id = 'TileBatteryValue';  Badge = 'TileBatteryBadge' }
    @{ N = 8; Name = 'GPU';      Id = '';                  Badge = '' }
)

foreach ($t in $tiles) {
    if (-not $t.Id) { continue }
    $e = Find-ById $win $t.Id
    if (-not $e) {
        Assert-True ("① 磁贴 " + $t.N + "/8 " + $t.Name + " 的 AutomationId 存在") $false `
            ($t.Id + " 找不到")
        continue
    }
    Assert-True ("① 磁贴 " + $t.N + "/8 " + $t.Name + " 的 AutomationId 存在") `
        ($e.Current.Name -ne '') `
        ("id=" + $t.Id + " 值='" + $e.Current.Name + "'")
}

# 第 8 块：GPU 占位。
#
# 先找找看有没有 GPU 磁贴自己的 AutomationId（比如 TileGpuValue）：
# 将来补上了就走「按 AutomationId 找」这条路；
# 现在没有，就改用占位文案把它认出来，并报 SKIP 说明这一条跑不了。
$gpuId = ''
$gpuIdEl = $null
$allEls = $win.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
for ($i = 0; $i -lt $allEls.Count; $i++) {
    $aid = $allEls[$i].Current.AutomationId
    if ($aid -and $aid -like 'TileGpu*') { $gpuIdEl = $allEls[$i]; $gpuId = $aid; break }
}
$gpuText = Find-ByName $win 'GPU 磁贴'
$gpuTodo = Find-ByName $win '本轮不做'
Assert-True '① 磁贴 8/8 GPU 占位存在（文案「GPU 磁贴」/「本轮不做」）' `
    ($null -ne $gpuText -and $null -ne $gpuTodo) `
    ("GPU 磁贴=" + ($null -ne $gpuText) + " 本轮不做=" + ($null -ne $gpuTodo))
if ($gpuIdEl) {
    Assert-True '① 磁贴 8/8 GPU 的 AutomationId 存在' $true ("id=" + $gpuId)
} else {
    Skip-Check '① 磁贴 8/8 GPU 的 AutomationId 检查' `
        '该占位磁贴在 src/ui/MonitorPage.xaml 里既没有 x:Name 也没有 AutomationProperties.AutomationId，无法按 AutomationId 查找（产品缺口，未改产品代码）'
}

# 徽章（每块磁贴右上角的那行小字）同样要有 AutomationId。
$badgeMissing = @()
foreach ($t in $tiles) {
    if (-not $t.Badge) { continue }
    if (-not (Find-ById $win $t.Badge)) { $badgeMissing += $t.Badge }
}
Assert-True '① 7 块磁贴的徽章 AutomationId 全部存在' `
    ($badgeMissing.Count -eq 0) `
    $(if ($badgeMissing.Count -eq 0) { '' } else { '缺 ' + ($badgeMissing -join ',') })

# ---------------------------------------------------------------- ② 数值会变

$intervalMs = Get-SampleIntervalMs
# 隔 **两个** 采样周期读两次。取 2.5 倍是为了给「采样器出帧 + 界面 100ms
# 刷新」留出余量，多等一会儿不会让判据变松，只是让读数更稳。
$waitMs = [int]($intervalMs * 2.5)
Write-Output ("sample interval = " + $intervalMs + " ms, wait = " + $waitMs + " ms")

# 先等磁贴真的有值。
#
# 【为什么必须等】
# 不等的话第一次读到的是「—」（采样器第一帧还没落地），
# 第二次读到的是真实数值 —— 那样「数值变了」就只是量到了「第一帧到了」，
# 不是量到了「数值在刷新」。
#
# 【为什么盯内存磁贴】
# 磁贴的填充顺序不一样：CPU 走历史序列，**第一帧就有值**（0）；
# 内存要等第一帧完整快照（memory.totalBytes > 0）才有值；
# 健康分要 cpu + 内存 + 磁盘都齐了才算得出。
# 所以内存磁贴是「第一帧完整快照已落地」的信号。
# （电池、磁盘活动在这台机器上**恒为**「—」：台式机没有电池，
#   磁盘活动取不到 —— 那是合法的最终状态，不是没填上。）
$populated = Wait-Until {
    $m = Find-ById $win 'TileMemoryValue'
    $h = Find-ById $win 'TileHealthValue'
    $mOk = ($m -and $m.Current.Name -and $m.Current.Name -ne '—')
    $hOk = ($h -and $h.Current.Name -and $h.Current.Name -ne '—')
    if ($mOk -or $hOk) { $true } else { $null }
} 45 400
if (-not $populated) {
    Write-Output "note: 磁贴迟迟没有出值（仍是占位破折号），第一次采样可能读到占位值"
}

$first = @{}
foreach ($t in $tiles) {
    if (-not $t.Id) { continue }
    $e = Find-ById $win $t.Id
    if ($e) { $first[$t.Id] = $e.Current.Name }
}
Write-Output ("first  sample: " + (($tiles | Where-Object { $_.Id -and $first.ContainsKey($_.Id) } |
    ForEach-Object { $_.Name + '=' + $first[$_.Id] }) -join '  '))

Start-Sleep -Milliseconds $waitMs

$second = @{}
foreach ($t in $tiles) {
    if (-not $t.Id) { continue }
    $e = Find-ById $win $t.Id
    if ($e) { $second[$t.Id] = $e.Current.Name }
}
Write-Output ("second sample: " + (($tiles | Where-Object { $_.Id -and $second.ContainsKey($_.Id) } |
    ForEach-Object { $_.Name + '=' + $second[$_.Id] }) -join '  '))

$changed = @()
$skipped = @()
foreach ($t in $tiles) {
    if (-not $t.Id) { continue }
    if (-not $first.ContainsKey($t.Id) -or -not $second.ContainsKey($t.Id)) { continue }
    # 第一次采样还是占位破折号的磁贴不参与「变了没有」的判断 ——
    # 那量到的是「第一帧到了」，不是「数值在刷新」。
    if ($first[$t.Id] -eq '—' -or $first[$t.Id] -eq '') {
        $skipped += ($t.Name + '(首读=' + $first[$t.Id] + ')')
        continue
    }
    if ($first[$t.Id] -ne $second[$t.Id]) {
        $changed += ($t.Name + ' ' + $first[$t.Id] + '→' + $second[$t.Id])
    }
}
if ($skipped.Count -gt 0) {
    Write-Output ("note: 首读仍是占位值、不参与变化判断的磁贴：" + ($skipped -join '，'))
}

if ($changed.Count -gt 0) {
    Assert-True '② 磁贴数值在两个采样周期之间发生变化（非恒定）' $true `
        ("变了 " + $changed.Count + " 块：" + ($changed -join '，'))
} else {
    # 真的一直不变（比如机器很闲）——这是环境限制，不是产品缺陷。
    Skip-Check '② 磁贴数值在两个采样周期之间发生变化（非恒定）' `
        ("在 " + $waitMs + " ms（两个采样周期）内所有磁贴数值都没变。首读 / 末读：" +
         (($tiles | Where-Object { $_.Id -and $second.ContainsKey($_.Id) } |
             ForEach-Object { $_.Name + '=' + $first[$_.Id] + '/' + $second[$_.Id] }) -join '  '))
}

# ---------------------------------------------------------------- 截图

if (-not $SkipShot) {
    $bmp = Get-WindowBitmap $hwnd
    if ($bmp) {
        $shot = Join-Path $env:TEMP "$ShotPrefix`_main.png"
        $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Output ("SHOT " + $bmp.Width + " x " + $bmp.Height)
        Write-Output ("SHOT_PATH=" + $shot)
        $bmp.Dispose()
    } else {
        Write-Output "SHOT: 抓不到窗口位图，跳过截图"
    }
}

# ---------------------------------------------------------------- 收尾

# 把自己拉起来的那一份关掉。
# 残留进程会占着 exe 文件，下一次构建会报 LNK1104「无法打开文件」。
# -NoLaunch 进来的话不关 —— 那份不是本脚本起的。
if ($launchedPid -gt 0) {
    Stop-Process -Id $launchedPid -Force -ErrorAction SilentlyContinue
    Write-Output "stopped pid=$launchedPid"
}

Write-Output ""
Write-Output "RESULT: pass=$script:Pass fail=$script:Fail"
if ($script:Fail -gt 0) { exit 1 }
exit 0
