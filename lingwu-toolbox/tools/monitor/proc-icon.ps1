# proc-icon.ps1 —— 进程表图标列验收
#
# 判据（对应「仿 Mole for Windows 改造方案 Phase 1」的验收表）：
#   ① 进程表有图标列（列头 / 行的结构里能看到图标元素或图标徽章）
#   ② 图标非空或已退化成色块（首字母徽章），不出现破图
#
# 【行模板的列序（src/ui/MonitorPage.xaml 的 ProcessRowTemplate）】
#   图标 36 / 名称 * / 内存 96 / CPU 76 / PID 60 / 进程数 64
# 图标列在最左边，里面现在放的是「应用名首字母徽章」：
#   一个 26×26 的圆角底 + 一行 TextBlock（绑定 IconText）。
# 徽章是 TextBlock，所以它在 UIA 树里是一个 **Text** 元素，
# 名字就是那一个字母；破图（Image 控件没加载上）则是**画不出东西**，
# 只能从像素上判断 —— UIA 不会告诉你「这张图没加载出来」。
#
# 【跨进程 UIA 一律按 ProcessId 过滤】
# 桌面上可能有别的同名窗口，只按标题或类名找会认错。

param(
    [string]$Exe = '',
    [switch]$NoLaunch,
    [switch]$SkipShot,
    [string]$ShotPrefix = 'procicon'
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
[DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
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

# 切到状态页（进程表只在那页）。
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

# ---------------------------------------------------------------- 行结构

# 一行里的所有 Text 元素，按屏幕 X 从左到右排。
# 行模板的列序是：图标 36 / 名称 * / 内存 96 / CPU 76 / PID 60 / 进程数 64。
function Get-RowTexts($Row) {
    $out = @()
    $all = $Row.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    for ($i = 0; $i -lt $all.Count; $i++) {
        if ($all[$i].Current.ControlType -eq $CT::Text) { $out += $all[$i] }
    }
    return ,($out | Sort-Object { $_.Current.BoundingRectangle.X })
}

# 一行里的 Image 元素（图标做成真实 exe 图标时才会有）。
function Get-RowImages($Row) {
    $out = @()
    $all = $Row.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    for ($i = 0; $i -lt $all.Count; $i++) {
        if ($all[$i].Current.ControlType -eq $CT::Image) { $out += $all[$i] }
    }
    return ,$out
}

# 进程表当前已实现的行容器。ListView 是虚拟化的，
# 所以这只是「视口里已实现了多少行」，不是「有多少个进程」。
function Get-Rows($Win) {
    $list = Find-ById $Win 'ProcessList'
    if (-not $list) { return @() }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, $CT::ListItem)
    $items = $list.FindAll($TS::Descendants, $cond)
    $out = @()
    for ($i = 0; $i -lt $items.Count; $i++) { $out += $items[$i] }
    return ,$out
}

# ---------------------------------------------------------------- 像素取证

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

# 量一块区域里「画了东西」的像素。
#
# 【为什么不能只看 UIA】
# UIA 看不出「一张图没加载出来」——破图在 UIA 上就是一个什么都没有的
# Image 元素，跟「图标是空的」长得一模一样。只能看渲染结果。
#
# 【怎么量】
# ① 众数颜色 = 这一块区域的底色（行背景）；
# ② 与众数差异 >= 8 的像素 = 画出来了的东西（徽章底色、字形、真实图标）；
# ③ 亮度 >= 120 的像素 = 字形（徽章里的那个字母是 75% 不透明的浅色，
#    实测 204；底色只有 54，底噪只有 32）。
#
# 【为什么 ② 与 ③ 分开判】
# 徽章底色（54）本来就比行背景（32）亮一点，② 过了只能说明「有东西」；
# 要证明「首字母徽章不是空壳」，还得看 ③ 里有没有字形像素。
function Measure-Cell($Bmp, $WinRect, $Cell) {
    $x0 = [int]($Cell.X - $WinRect.Left)
    $y0 = [int]($Cell.Y - $WinRect.Top)
    $x1 = [int]($Cell.X + $Cell.Width - $WinRect.Left)
    $y1 = [int]($Cell.Y + $Cell.Height - $WinRect.Top)
    if ($x0 -lt 0) { $x0 = 0 }
    if ($y0 -lt 0) { $y0 = 0 }
    if ($x1 -ge $Bmp.Width) { $x1 = $Bmp.Width - 1 }
    if ($y1 -ge $Bmp.Height) { $y1 = $Bmp.Height - 1 }
    if ($x1 -le $x0 -or $y1 -le $y0) { return $null }

    $hist = @{}
    $samples = @()
    for ($y = $y0; $y -le $y1; $y += 2) {
        for ($x = $x0; $x -le $x1; $x += 2) {
            $c = $Bmp.GetPixel($x, $y)
            $key = "$($c.R),$($c.G),$($c.B)"
            if ($hist.ContainsKey($key)) { $hist[$key]++ } else { $hist[$key] = 1 }
            $samples += ,@($c.R, $c.G, $c.B)
        }
    }
    $modal = ''
    $modalCount = -1
    foreach ($k in $hist.Keys) {
        if ($hist[$k] -gt $modalCount) { $modalCount = $hist[$k]; $modal = $k }
    }
    $m = $modal.Split(',')
    $mr = [int]$m[0]; $mg = [int]$m[1]; $mb = [int]$m[2]

    $diff = 0
    $bright = 0
    foreach ($s in $samples) {
        $d = [Math]::Max([Math]::Abs($s[0] - $mr), [Math]::Max([Math]::Abs($s[1] - $mg), [Math]::Abs($s[2] - $mb)))
        if ($d -ge 8) { $diff++ }
        $lum = [int](0.299 * $s[0] + 0.587 * $s[1] + 0.114 * $s[2])
        if ($lum -ge 120) { $bright++ }
    }
    return [pscustomobject]@{
        Modal   = $modal
        Diff    = $diff
        Bright  = $bright
        Samples = $samples.Count
    }
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

if (-not (Switch-ToStatus $win)) {
    Write-Output 'ABORT: 切不到状态页（进程表只在那页）'
    exit 1
}
Write-Output "page=Status"

# 等进程表出第一批行（有界轮询，不赌固定 sleep）。
$rows = @()
$got = Wait-Until {
    $r = Get-Rows $win
    if ($r.Count -gt 0) { $r } else { $null }
} 30 400
if ($got) { $rows = $got }
Write-Output ("rows realised = " + $rows.Count)

# ---------------------------------------------------------------- 收尾

# 收尾：把自己拉起来的那一份关掉，再给汇总行。
#
# 【为什么要在收尾里关进程】
# 残留进程会占着 exe 文件，下一次构建会报 LNK1104「无法打开文件」。
# -NoLaunch 进来的话不关 —— 那份不是本脚本起的。
function Finish {
    if ($script:LaunchedPid -gt 0) {
        Stop-Process -Id $script:LaunchedPid -Force -ErrorAction SilentlyContinue
        Write-Output "stopped pid=$script:LaunchedPid"
    }
    Write-Output ""
    Write-Output "RESULT: pass=$script:Pass fail=$script:Fail"
    if ($script:Fail -gt 0) { exit 1 }
    exit 0
}

if ($rows.Count -eq 0) {
    Skip-Check '① 进程表有图标列' 'ProcessList 里一行都没实现出来（UIA 树里没有 ListItem）'
    Skip-Check '② 图标非空或已退化成色块，不出现破图' '没有行可量'
    Finish
}

# ---------------------------------------------------------------- 抓一张位图

$wr = New-Object Native.Win+RECT
[void][Native.Win]::GetWindowRect($hwnd, [ref]$wr)
$bmp = Get-WindowBitmap $hwnd
if (-not $bmp) {
    Skip-Check '② 图标非空或已退化成色块，不出现破图' '抓不到窗口位图（PrintWindow 无输出），量不了像素'
    Finish
}

# ---------------------------------------------------------------- ① 图标列存在

# 表头那一侧也要有图标列：表头的列宽与行模板完全一致
# （图标 36 / 名称 * / 内存 96 / CPU 76 / PID 60 / 进程数 64），
# 所以名称列表头的左缘应当与行里名称文字的左缘对齐。
$headName = Find-ById $win 'HeaderName'
$sample = $rows[0]
$texts = Get-RowTexts $sample
if ($texts.Count -ge 2 -and $headName) {
    $hr = $headName.Current.BoundingRectangle
    $nr = $texts[1].Current.BoundingRectangle
    $gap = [int]($nr.X - $hr.X)
    Assert-True '① 表头的名称列与行里的名称列左缘对齐（表头也有那一格图标列）' `
        ([Math]::Abs($gap) -le 2) ("表头名称左缘=" + [int]$hr.X + " 行名称左缘=" + [int]$nr.X)

    # 图标列本身有多宽：行左缘到名称列左缘之间那一格。
    # 行模板写死的是 36 DIP，所以用窗口 DPI 换算回 DIP 核对 ——
    # 宽了窄了都说明列宽被改过。
    $scale = [double][Native.Win]::GetDpiForWindow($hwnd) / 96.0
    if ($scale -le 0) { $scale = 1.0 }
    $rowR = $sample.Current.BoundingRectangle
    $colDip = ($nr.X - $rowR.X) / $scale
    Assert-True '① 图标列宽度是 36 DIP（与行模板一致）' `
        ([Math]::Abs($colDip - 36.0) -le 1.0) `
        ("实测 " + [Math]::Round($colDip, 1) + " DIP（DPI=" + [Math]::Round($scale * 100) + "%）")
} elseif (-not $headName) {
    Skip-Check '① 表头的名称列与行里的名称列左缘对齐' 'HeaderName 不在 UIA 树里'
} else {
    Skip-Check '① 表头的名称列与行里的名称列左缘对齐' '这一行里的文本元素不足 2 个'
}

$checked = 0
$badgeForm = 0
$imageForm = 0
$maxRows = [Math]::Min(5, $rows.Count)
for ($i = 0; $i -lt $maxRows; $i++) {
    $row = $rows[$i]
    $rowR = $row.Current.BoundingRectangle
    $texts = Get-RowTexts $row
    $images = Get-RowImages $row

    if ($texts.Count -lt 2) {
        Assert-True ("① 第 " + ($i + 1) + " 行有图标列") $false `
            ("这一行只有 " + $texts.Count + " 个文本元素")
        continue
    }

    $icon = $texts[0]
    $name = $texts[1]
    $iconR = $icon.Current.BoundingRectangle
    $nameR = $name.Current.BoundingRectangle

    # 图标列 = 行左缘到名称文字左缘之间那一格。
    $cell = New-Object System.Windows.Rect(
        $rowR.X, $rowR.Y, ($nameR.X - $rowR.X), $rowR.Height)

    $hasImage = ($images.Count -gt 0)
    if ($hasImage) { $imageForm++ } else { $badgeForm++ }

    if ($hasImage) {
        Assert-True ("① 第 " + ($i + 1) + " 行有图标列（真实 exe 图标元素）") `
            ($iconR.X -ge $rowR.X -and $iconR.X -lt $nameR.X) `
            ("Image 元素 " + [int]$images[0].Current.BoundingRectangle.X)
    } else {
        # 徽章形态：图标列里是一个 **Text**，内容是应用名的首字母。
        $badge = $icon.Current.Name
        Assert-True ("① 第 " + ($i + 1) + " 行有图标列（首字母徽章='" + $badge + "'）") `
            ($badge.Length -eq 1 -and $iconR.X -lt $nameR.X) `
            ("徽章左缘=" + [int]$iconR.X + " 名称左缘=" + [int]$nameR.X)
    }
    $checked++

    # ---- ② 图标不是破图 / 空白 ----
    $m = Measure-Cell $bmp $wr $cell
    if (-not $m) {
        Skip-Check ("② 第 " + ($i + 1) + " 行的图标不是破图") '图标列区域算不出来'
        continue
    }
    Assert-True ("② 第 " + ($i + 1) + " 行的图标区域画出了内容（不是空白/破图）") `
        ($m.Diff -ge 20) `
        ("底色=" + $m.Modal + " 有内容像素=" + $m.Diff + "/" + $m.Samples)
    if (-not $hasImage) {
        # 徽章形态还要证明「字母真的画出来了」，不是个空壳圆角块。
        Assert-True ("② 第 " + ($i + 1) + " 行的首字母徽章画出了字形") `
            ($m.Bright -ge 1) ("亮像素=" + $m.Bright + "/" + $m.Samples)
    }
}

Assert-True '① 进程表有图标列（至少取到 3 行样本）' ($checked -ge 3) `
    ("取样行数=" + $checked + " 徽章形态=" + $badgeForm + " 真实图标形态=" + $imageForm)

# ---------------------------------------------------------------- 截图

if (-not $SkipShot) {
    $shot = Join-Path $env:TEMP "$ShotPrefix`_main.png"
    $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output ("SHOT " + $bmp.Width + " x " + $bmp.Height)
    Write-Output ("SHOT_PATH=" + $shot)
}
$bmp.Dispose()

Finish
