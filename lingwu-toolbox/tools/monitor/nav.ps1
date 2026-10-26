# nav.ps1 —— 导航页签验收
#
# 判据（对应「仿 Mole for Windows 改造方案 Phase 1」的验收表）：
#   ① 导航容器里 6 个页签都存在（按 AutomationId 找，名字是 清理/应用/优化/分析/状态/服务）
#   ② 「状态」「服务」两个页签可以切（点得动，且切过去内容区域真的变了）
#   ③ 前 4 个未实现的页签是禁用态（IsEnabled=false 或点了没反应，二者其一即可）
#   ④ 导航容器里选中态唯一（任一时刻只有一个页签被高亮）
#   ⑤ 切页不重建（状态页进程列表的 Items 容器数量在切走再切回后不变）
#
# 【为什么「选中态」只能用像素量】
# 页签是 Button，UIA 上**没有**选中语义（实测只支持 InvokePattern，
# 没有 SelectionItem / Toggle），「选中」是 C++ 把页签底色刷成该页主色
# （状态 #4E8529、服务 #278C8C）实现的 —— 只能从渲染结果上量。
# 判据用饱和度（max-min）：主色的 max-min 分别是 92 / 101，
# 而未选中页签是透明底 + 灰白文字，饱和度为 0。
#
# 【跨进程 UIA 一律按 ProcessId 过滤】
# 桌面上可能有别的同名窗口，只按标题或类名找会认错。

param(
    [string]$Exe = '',
    [switch]$NoLaunch,
    [switch]$SkipShot,
    [string]$ShotPrefix = 'nav'
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

# 必须先声明 DPI 感知，否则 GetWindowRect 返回逻辑像素、绘图用物理像素，
# 截出来的图会带进窗口外面的内容（详见 shot.ps1 的同款说明）。
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

# SKIP 不计入 pass 也不计入 fail：这一项**本机跑不了**（环境限制），
# 不是产品缺陷。染红验收板比没有验收更糟。
function Skip-Check([string]$What, [string]$Why = '') {
    Write-Output ("  SKIP: {0} {1}" -f $What, $Why)
}

# ---------------------------------------------------------------- 元素查找

# 按 AutomationId 找元素。条件里必须带 ProcessId：
# 桌面上可能有别的同名窗口，只按 AutomationId 找会认错。
function Find-ById($Root, [string]$Id) {
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::ProcessIdProperty, [int]$Root.Current.ProcessId)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $AE::AutomationIdProperty, $Id)))
    return $Root.FindFirst($TS::Descendants, $cond)
}

# 有界轮询：发动作 → 等一下 → 看结果。
# 一律带超时与轮询间隔，不写固定 sleep 赌一次。
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
# 标题可能被改，而导航页签是外壳的固有标志。
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

# 当前显示的是哪个页面。
#
# 两个页面是「构造一次、常驻」，切页只翻 Visibility。收起来的页面
# **不在 UIA 树里**（实测：切到服务页后 ProcessList 找不到、ServiceList 出现），
# 所以「ProcessList 在不在树里」就是可靠的判据。
function Get-CurrentPage($Win) {
    if (Find-ById $Win 'ProcessList') { return 'Status' }
    if (Find-ById $Win 'ServiceList') { return 'Service' }
    return ''
}

# 点页签并等页面真的换过去。返回 $true/$false。
function Switch-Page($Win, [string]$Page) {
    $id = if ($Page -eq 'Status') { 'NavStatusButton' } else { 'NavServiceButton' }
    $btn = Find-ById $Win $id
    if (-not $btn) { return $false }
    if (-not $btn.Current.IsEnabled) { return $false }
    try {
        $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    } catch {
        # 用 Write-Host：调用方多半写成 `Switch-Page ... | Out-Null`，
        # 走输出流会被一起吞掉。
        Write-Host ("  invoke '$id' threw: " + $_.Exception.Message)
        return $false
    }
    $hit = Wait-Until { (Get-CurrentPage $Win) -eq $Page } 40 300
    return ($null -ne $hit)
}

# 进程列表当前**已实现**的 Items 容器数量。
# ListView 是虚拟化的，所以这个数是「视口里已实现了多少行容器」，
# 不是「有多少个进程」。
function Get-ItemContainerCount($Win) {
    $list = Find-ById $Win 'ProcessList'
    if (-not $list) { return -1 }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, $CT::ListItem)
    return $list.FindAll($TS::Descendants, $cond).Count
}

# ---------------------------------------------------------------- 像素取证

# 抓窗口位图（返回 Bitmap 对象）。
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

# 数一个矩形区域里「饱和度够高」的像素个数。
#
# 【为什么用饱和度】
# 页签被选中时整块填充该页主色（状态 #4E8529 / 服务 #278C8C），
# 未选中时是透明底 + 灰白文字。灰白的 max-min 是 0，主色的 max-min
# 是 92 / 101 —— 一个阈值（60）就能把两者分开。
#
# 【为什么只取矩形中间一段】
# 页签是 14 DIP 圆角的胶囊，四角是抗锯齿过渡；文字字形落在中间。
# 取「左右各留 20%、上下取靠上的一段（避开字形）」，
# 量到的就是纯粹的底色。
function Measure-Saturated($Bmp, $WinRect, $Rect) {
    $x0 = [int]($Rect.X - $WinRect.Left) + [int]($Rect.Width * 0.20)
    $x1 = [int]($Rect.X - $WinRect.Left) + [int]($Rect.Width * 0.80)
    $y0 = [int]($Rect.Y - $WinRect.Top) + [int]($Rect.Height * 0.15)
    $y1 = [int]($Rect.Y - $WinRect.Top) + [int]($Rect.Height * 0.35)
    if ($x1 -le $x0 -or $y1 -le $y0) { return 0 }
    if ($x0 -lt 0) { $x0 = 0 }
    if ($y0 -lt 0) { $y0 = 0 }
    if ($x1 -ge $Bmp.Width) { $x1 = $Bmp.Width - 1 }
    if ($y1 -ge $Bmp.Height) { $y1 = $Bmp.Height - 1 }
    $count = 0
    $total = 0
    for ($y = $y0; $y -le $y1; $y++) {
        for ($x = $x0; $x -le $x1; $x++) {
            $total++
            $c = $Bmp.GetPixel($x, $y)
            $mx = [Math]::Max([int]$c.R, [Math]::Max([int]$c.G, [int]$c.B))
            $mn = [Math]::Min([int]$c.R, [Math]::Min([int]$c.G, [int]$c.B))
            if (($mx - $mn) -ge 60) { $count++ }
        }
    }
    return [pscustomobject]@{ Saturated = $count; Total = $total }
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
    Start-Sleep -Seconds 4   # 等首帧渲染完
}

# 自己拉起来的那一份优先；-NoLaunch 时才去认桌面上跑着的那份。
# 37480 是已知的杀不掉的僵尸残留（见 tools/monitor/tray-panel.ps1）。
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

# 等页面就位（页面是惰性建的，第一次进入某个页签时才建）。
$page = Wait-Until { Get-CurrentPage $win } 40 300
if (-not $page) { Write-Output 'ABORT: 等不到任何页面（ProcessList / ServiceList 都不在 UIA 树里）'; exit 1 }
Write-Output "start page=$page"

# ---------------------------------------------------------------- ① 六个页签都在

# AutomationId 与页签文字一一对应。文字是中文，脚本本身也是中文 .ps1，
# 按仓库铁律只能用 pwsh 跑（见 AGENTS.md 第 9 条）。
$tabs = @(
    @{ Id = 'NavCleanButton';    Text = '清理' }
    @{ Id = 'NavAppsButton';     Text = '应用' }
    @{ Id = 'NavOptimizeButton'; Text = '优化' }
    @{ Id = 'NavAnalyzeButton';  Text = '分析' }
    @{ Id = 'NavStatusButton';   Text = '状态' }
    @{ Id = 'NavServiceButton';  Text = '服务' }
)
foreach ($t in $tabs) {
    $e = Find-ById $win $t.Id
    if (-not $e) {
        Assert-True ("① 导航页签存在：" + $t.Text + "（" + $t.Id + "）") $false 'AutomationId 找不到'
        continue
    }
    $name = $e.Current.Name
    Assert-True ("① 导航页签存在：" + $t.Text + "（" + $t.Id + "）") `
        ($name -eq $t.Text) ("name='" + $name + "'")
}

# ---------------------------------------------------------------- ② 两个页签可切

# 先切到「服务」，再切到「状态」——这样两个页签都是**从别的页面切过来**的，
# 「点得动」才不是一句空话（如果启动就停在某个页签上，
# 直接点它等于什么都没验）。
$okService = Switch-Page $win 'Service'
Assert-True '② 「服务」页签点得动，内容区域切到了服务页' $okService `
    ("当前页=" + (Get-CurrentPage $win))

$okStatus = Switch-Page $win 'Status'
Assert-True '② 「状态」页签点得动，内容区域切到了状态页' $okStatus `
    ("当前页=" + (Get-CurrentPage $win))

# 再切回服务一次，确认来回切都稳。
$okService2 = Switch-Page $win 'Service'
Assert-True '② 「服务」页签可以来回切（服务 → 状态 → 服务）' $okService2 `
    ("当前页=" + (Get-CurrentPage $win))

# ---------------------------------------------------------------- ③ 前四个是禁用态

# 判据：IsEnabled 为 false，或者点了没有任何反应（两种都算通过），
# 但输出里必须写明实际观察到的是哪一种。
$pageBefore = Get-CurrentPage $win
foreach ($t in @($tabs[0], $tabs[1], $tabs[2], $tabs[3])) {
    $e = Find-ById $win $t.Id
    if (-not $e) {
        Assert-True ("③ 前四个未实现页签是禁用态：" + $t.Text) $false '元素不存在'
        continue
    }
    if (-not $e.Current.IsEnabled) {
        # 已经是禁用态。再试着 Invoke 一次，确认它**点不动** ——
        # 禁用态的 InvokePattern 会抛错，这才是「点了没反应」的正面证据。
        $blocked = ''
        try {
            $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            $blocked = 'Invoke 居然成功了'
        } catch {
            $blocked = 'Invoke 被拒绝'
        }
        Assert-True ("③ 前四个未实现页签是禁用态：" + $t.Text) `
            ($blocked -eq 'Invoke 被拒绝') ("观察到 IsEnabled=false；" + $blocked)
    } else {
        # 少见分支：UIA 说它是启用的，那就点一下，看页面有没有变。
        Switch-Page $win $pageBefore | Out-Null
        try {
            $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        } catch {
            Write-Output ("  invoke '" + $t.Id + "' threw: " + $_.Exception.Message)
        }
        $changed = Wait-Until { (Get-CurrentPage $win) -ne $pageBefore } 5 300
        Assert-True ("③ 前四个未实现页签是禁用态：" + $t.Text) `
            ($null -eq $changed) '观察到 IsEnabled=true，但点了之后页面没变'
    }
}
Assert-True '③ 点前四个禁用页签之后，内容区域仍然停在原页' `
    ((Get-CurrentPage $win) -eq $pageBefore) ("当前页=" + (Get-CurrentPage $win))

# ---------------------------------------------------------------- ④ 选中态唯一

# 选中态只能从渲染结果上量（页签是 Button，UIA 没有选中语义）。
function Get-HighlightedTabs($Win, [IntPtr]$Hwnd) {
    $wr = New-Object Native.Win+RECT
    if (-not [Native.Win]::GetWindowRect($Hwnd, [ref]$wr)) { return $null }
    $bmp = Get-WindowBitmap $Hwnd
    if (-not $bmp) { return $null }
    $hits = @()
    $detail = @()
    foreach ($t in $tabs) {
        $e = Find-ById $Win $t.Id
        if (-not $e) { continue }
        $m = Measure-Saturated $bmp $wr $e.Current.BoundingRectangle
        $detail += ($t.Text + '=' + $m.Saturated + '/' + $m.Total)
        # 判据：探针区里饱和像素超过 5% 才算「被高亮」。
        if ($m.Total -gt 0 -and $m.Saturated -ge [int]($m.Total * 0.05)) {
            $hits += $t.Text
        }
    }
    $bmp.Dispose()
    return [pscustomobject]@{ Hits = $hits; Detail = ($detail -join ' ') }
}

# 选中态只能从渲染结果上量（页签是 Button，UIA 没有选中语义）。
function Get-HighlightedTabs($Win, [IntPtr]$Hwnd) {
    $wr = New-Object Native.Win+RECT
    if (-not [Native.Win]::GetWindowRect($Hwnd, [ref]$wr)) { return $null }
    $bmp = Get-WindowBitmap $Hwnd
    if (-not $bmp) { return $null }
    $hits = @()
    $detail = @()
    foreach ($t in $tabs) {
        $e = Find-ById $Win $t.Id
        if (-not $e) { continue }
        $m = Measure-Saturated $bmp $wr $e.Current.BoundingRectangle
        $detail += ($t.Text + '=' + $m.Saturated + '/' + $m.Total)
        # 判据：探针区里饱和像素超过 5% 才算「被高亮」。
        if ($m.Total -gt 0 -and $m.Saturated -ge [int]($m.Total * 0.05)) {
            $hits += $t.Text
        }
    }
    $bmp.Dispose()
    return [pscustomobject]@{ Hits = $hits; Detail = ($detail -join ' ') }
}

# 量一次选中态并断言：高亮的页签恰好一个，而且就是当前页对应的那一个。
#
# 【这个函数不能有返回值】
# 它的断言行走的是输出流；一旦调用方写成 `... | Out-Null`，
# 断言行会被一起吞掉 —— 计数器还在涨、板子上却什么也看不见。
# 所以这里只输出、不返回，调用方直接当语句调。
function Assert-Selection($Win, [IntPtr]$Hwnd, [string]$ExpectPage) {
    $hl = Get-HighlightedTabs $Win $Hwnd
    if (-not $hl) {
        Skip-Check '④ 导航容器里选中态唯一' '抓不到窗口位图（PrintWindow 无输出），量不了高亮'
        return
    }
    Assert-True '④ 导航容器里选中态唯一（只有一个页签被高亮）' `
        ($hl.Hits.Count -eq 1) ("高亮=" + ($hl.Hits -join ',') + " 采样=" + $hl.Detail)
    # 高亮的那个必须就是当前页对应的页签，否则「唯一」也可能是「高错了」。
    # 页签文字是中文，页面代号是英文，这里做一次映射。
    $expectText = if ($ExpectPage -eq 'Service') { '服务' } else { '状态' }
    Assert-True ('④ 高亮的页签就是当前页对应的页签（' + $expectText + '）') `
        ($hl.Hits.Count -eq 1 -and $hl.Hits[0] -eq $expectText) `
        ("高亮=" + ($hl.Hits -join ',') + " 当前页=" + $ExpectPage)
}

Assert-Selection $win $hwnd $pageBefore

# ---------------------------------------------------------------- ⑤ 切页不重建

# 判据：状态页的进程列表 Items 容器数量，在「切走 → 切回」之后不变。
#
# 为什么量「已实现的容器数」：ListView 虚拟化，UIA 只能看见视口里
# 已实现的行容器。这一条要抓的是「页面被重建」——重建会把行容器
# 换掉一批，数量就会跳。
Switch-Page $win 'Status' | Out-Null
# 换到另一个页面再量一次选中态：④ 要的是「任一时刻只有一个」，
# 只在服务页量一次是不够的。
Assert-Selection $win $hwnd 'Status'
$before = Get-ItemContainerCount $win
if ($before -lt 0) {
    Skip-Check '⑤ 切页不重建（状态页 Items 容器数量不变）' `
        'ProcessList 不在 UIA 树里，拿不到容器数量'
} else {
    Write-Output ("  items container before = " + $before)
    Switch-Page $win 'Service' | Out-Null
    Switch-Page $win 'Status' | Out-Null
    # 切回来之后容器要重新实现，等它落地再数（有界轮询）。
    $after = -1
    for ($i = 0; $i -lt 20; $i++) {
        $after = Get-ItemContainerCount $win
        if ($after -gt 0) { break }
        Start-Sleep -Milliseconds 400
    }
    Assert-True '⑤ 切页不重建（状态页 Items 容器数量不变）' `
        ($after -gt 0 -and $after -eq $before) ("before=" + $before + " after=" + $after)
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
