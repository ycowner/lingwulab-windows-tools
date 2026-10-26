param(
    [string]$Exe,
    [string]$Prefix = 'menu'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern IntPtr PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
[void][Native.Win]::SetProcessDPIAware()

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$PW = 0x00000002
$WM_CONTEXTMENU = 0x007B

Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
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
$win = Get-Win $p.Id
if (-not $win) { Write-Host 'WINDOW NOT FOUND'; exit 1 }
Start-Sleep -Seconds 14

# 断言计数器。这个脚本原本只在「打印所见」，失败路径靠 exit 1 表达，
# 验收板上数不出断言条数 —— 补上标记，板上的数才有意义。
$script:Pass = 0
$script:Fail = 0
function Assert-True([string]$What, [bool]$Cond, [string]$Extra = '') {
    if ($Cond) {
        $script:Pass++
        Write-Host ("  [PASS] {0} {1}" -f $What, $Extra)
    } else {
        $script:Fail++
        Write-Host ("  [FAIL] {0} {1}" -f $What, $Extra)
    }
}

function Find-Id($root, [string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    return $root.FindFirst($TS::Descendants, $c)
}

# 按「控件类型 + **进程 id**」在桌面范围内找元素。
#
# 必须卡 pid。曾经只用 ControlType 在桌面根下 FindAll，再用 -like '*取消*' 过滤名字，
# 结果把**别的应用**的按钮也匹配进来了（MiniMax Code 任务列表里就有一条标题含
# 「取消」的条目）。循环里 $cancel 被后面的匹配覆盖，最后点中的是别的应用的控件 ——
# 测试脚本因此产生了副作用。判据：凡是跨进程枚举 UI 元素，必须用 AndCondition
# 把 ProcessIdProperty 一起卡住。
function Find-ByTypeInProc($pid_, $controlType) {
    $c1 = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $controlType)
    $c2 = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    $and = New-Object System.Windows.Automation.AndCondition($c1, $c2)
    # Comma prevents PowerShell from unrolling the collection (see the note
    # in Get-TopWins): an unrolled single-element result loses .Count.
    return ,$AE::RootElement.FindAll($TS::Descendants, $and)
}

function Shot([string]$path) {
    $r = New-Object Native.Win+RECT
    if (-not [Native.Win]::GetWindowRect($p.MainWindowHandle, [ref]$r)) { return 'rect failed' }
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [Native.Win]::PrintWindow($p.MainWindowHandle, $hdc, $PW)
    $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $(if ($ok) { "$w x $h" } else { 'PrintWindow failed' })
}

# 等某个条件成立（最多 $TimeoutSec 秒），成立就立刻返回。
#
# 【为什么不用固定 Start-Sleep】
# 这里原来写的是「睡 2 秒，然后看一眼在不在」。那个写法有两个问题：
#   ① 弹窗出现得慢的时候（应用当时忙），2 秒不够 → 明明是好的也判失败；
#   ② 弹窗**根本**没出现的时候（真的坏了），也是同一句「没找到」——
#      两种完全不同的结论在输出里长得一模一样。
# 改成有界轮询之后，超时仍然判失败，但结论是明确的
# 「12 秒内没出现」，而不再是一个含糊的「等了 2 秒没看到」。
#
# 【这条脚本为什么之前会偶发红】
# 真因是产品缺陷（不是时序）：右键命中后到点「结束任务」之间，
# 列表重排把选中项换掉 → 确认框压根没弹 → 2 秒后当然找不到「取消」。
# 那个缺陷已修（见 AGENTS.md 第 2 节第 24 条）。
# 本脚本的轮询化是为了让**下一次**同类问题能被准确指认，而不是被误判成时序抖动。
function Wait-Until([scriptblock]$Probe, [int]$TimeoutSec = 12) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $v = & $Probe
        if ($v) { return $v }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

# ---- 1) 右键菜单 ---------------------------------------------------------
$list = Find-Id $win 'ProcessList'
if (-not $list) { Write-Host 'ProcessList NOT FOUND'; exit 1 }
Assert-True '找得到进程列表' $true
$lr = $list.Current.BoundingRectangle
Write-Host ("list rect = $([int]$lr.X),$([int]$lr.Y),$([int]$lr.Width)x$([int]$lr.Height)")

# 从 UIA 反推缩放：表头按钮 Style 里 Height=28 是逻辑值，UIA 报的是物理值。
$hdr = Find-Id $win 'HeaderProc'
$scale = 1.0
if ($hdr) { $scale = [math]::Round($hdr.Current.BoundingRectangle.Height / 28.0) ; if ($scale -le 0) { $scale = 1.0 } }
Write-Host ("scale guess = $scale")

# WM_CONTEXTMENU 的 lParam 是**屏幕坐标**（物理像素）；窗口过程里自己 ScreenToClient，
# 再按 RasterizationScale 换算成逻辑坐标做命中测试。
# 第一次行**中部**：行高 32 逻辑 px，取 16 逻辑 px 处。
$x = [int]($lr.X + 60 * $scale)
$y = [int]($lr.Y + 16 * $scale)
$lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
Write-Host ("posting WM_CONTEXTMENU at screen $x,$y")
[void][Native.Win]::PostMessageW($p.MainWindowHandle, $WM_CONTEXTMENU, [IntPtr]::Zero, $lp)

# 等右键菜单真的弹出来（最多 12 秒）。原来这里是固定睡 2 秒 ——
# 见 Wait-Until 的注释：固定睡眠会把「应用当时忙」误判成「菜单坏了」。
$menuUp = Wait-Until {
    $probe = Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::MenuItem)
    return ($probe.Count -gt 0)
}

$m = Join-Path $env:TEMP "$Prefix`_1_menu.png"
Write-Host ("menu shot: " + (Shot $m))

# 菜单项：MenuFlyout 是独立的弹出层，从桌面根往下找，但**只认本进程**的元素。
$menuItem = $null
$items = Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::MenuItem)
Write-Host ("menuitems in our process: " + $items.Count)
Assert-True '右键菜单真的弹出来了' ($menuUp -eq $true) "items=$($items.Count)"
foreach ($it in $items) {
    $n = $it.Current.Name
    if ($n) { Write-Host "  item: '$n'" }
    if (-not $menuItem -and $n -like '*结束任务*') { $menuItem = $it }
}
if (-not $menuItem) { Write-Host 'MENU ITEM NOT FOUND'; Write-Host "M=$m"; exit 1 }
Write-Host ("found menu item: '" + $menuItem.Current.Name + "'")
Assert-True '菜单里有「结束任务」' $true $menuItem.Current.Name
$menuItem.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()

# ---- 2) 确认弹窗 ---------------------------------------------------------
# 同样改成等它出来，而不是睡固定时间 —— 确认框是 ContentDialog，
# 要走 XAML 的内容呈现，比 MenuFlyout 慢。
$cancel = Wait-Until {
    $probe = Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::Button)
    foreach ($b in $probe) {
        $n = $b.Current.Name
        if ($n -and $n -like '*取消*') { return $b }
    }
    return $null
}

$d = Join-Path $env:TEMP "$Prefix`_2_confirm.png"
Write-Host ("confirm shot: " + (Shot $d))

$btns = Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::Button)
foreach ($b in $btns) {
    $n = $b.Current.Name
    if ($n) { Write-Host "  button: '$n'" }
}

# ---- 3) 取消，不杀任何进程 ----------------------------------------------
if ($cancel) {
    Write-Host ("clicking cancel: '" + $cancel.Current.Name + "'")
    $cancel.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 2
    Assert-True '确认框里有「取消」并且点得动' $true $cancel.Current.Name
} else {
    Write-Host 'CANCEL BUTTON NOT FOUND'
    Assert-True '确认框里有「取消」' $false
}
$e = Join-Path $env:TEMP "$Prefix`_3_cancelled.png"
Write-Host ("after cancel shot: " + (Shot $e))

Write-Host "M=$m"
Write-Host "D=$d"
Write-Host "E=$e"
$p.Refresh()
Write-Host ("alive=$(-not $p.HasExited) priv=$([math]::Round($p.PrivateMemorySize64/1MB,1))MB")
# 点「取消」之后应用必须还活着 —— 这一步要是杀错了进程（跨进程误匹配过），
# 应用会跟着没。
Assert-True '点「取消」之后应用还活着' (-not $p.HasExited)

Write-Host ''
Write-Host '=========================================================='
Write-Host ("RESULT: pass={0}  fail={1}" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 } else { exit 0 }
