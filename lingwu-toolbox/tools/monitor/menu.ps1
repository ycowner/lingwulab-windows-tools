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

# ---- 1) 右键菜单 ---------------------------------------------------------
$list = Find-Id $win 'ProcessList'
if (-not $list) { Write-Host 'ProcessList NOT FOUND'; exit 1 }
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
Start-Sleep -Seconds 2

$m = Join-Path $env:TEMP "$Prefix`_1_menu.png"
Write-Host ("menu shot: " + (Shot $m))

# 菜单项：MenuFlyout 是独立的弹出层，从桌面根往下找，但**只认本进程**的元素。
$menuItem = $null
$items = Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::MenuItem)
Write-Host ("menuitems in our process: " + $items.Count)
foreach ($it in $items) {
    $n = $it.Current.Name
    if ($n) { Write-Host "  item: '$n'" }
    if (-not $menuItem -and $n -like '*结束任务*') { $menuItem = $it }
}
if (-not $menuItem) { Write-Host 'MENU ITEM NOT FOUND'; Write-Host "M=$m"; exit 1 }
Write-Host ("found menu item: '" + $menuItem.Current.Name + "'")
$menuItem.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
Start-Sleep -Seconds 2

# ---- 2) 确认弹窗 ---------------------------------------------------------
$d = Join-Path $env:TEMP "$Prefix`_2_confirm.png"
Write-Host ("confirm shot: " + (Shot $d))

$btns = Find-ByTypeInProc $p.Id ([System.Windows.Automation.ControlType]::Button)
$cancel = $null
foreach ($b in $btns) {
    $n = $b.Current.Name
    if (-not $n) { continue }
    Write-Host "  button: '$n'"
    if (-not $cancel -and $n -like '*取消*') { $cancel = $b }
}

# ---- 3) 取消，不杀任何进程 ----------------------------------------------
if ($cancel) {
    Write-Host ("clicking cancel: '" + $cancel.Current.Name + "'")
    $cancel.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 2
} else {
    Write-Host 'CANCEL BUTTON NOT FOUND'
}
$e = Join-Path $env:TEMP "$Prefix`_3_cancelled.png"
Write-Host ("after cancel shot: " + (Shot $e))

Write-Host "M=$m"
Write-Host "D=$d"
Write-Host "E=$e"
$p.Refresh()
Write-Host ("alive=$(-not $p.HasExited) priv=$([math]::Round($p.PrivateMemorySize64/1MB,1))MB")
