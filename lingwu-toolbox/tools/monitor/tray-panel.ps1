# tray-panel.ps1 —— 托盘指标面板验收
#
# 判据（对应 plan.md §5.5 的第 4 条）：
#   ① 托盘**左键** → 指标面板出现（不是主窗口！左键语义本轮已改）
#   ② 再点一次托盘 → 面板收起（切换语义，不是「每次都弹」）
#   ③ 面板内「打开工具箱」→ 主窗口出现
#   ④ 面板出现时**不抢焦点**（弹出的是 AppWindow::Show(false)）
#
# 【为什么用 UIA Invoke 而不是 SendInput 合成鼠标点击】
# 本机实测 SendInput / mouse_event 点不动任何系统控件（AGENTS.md
# 「验证要求」一节记录了同一结论）。托盘按钮实测支持 InvokePattern，
# 且不依赖光标位置 —— 见 tools/common/tray.ps1 的说明。
#
# 【跨进程 UIA 一律按 ProcessId 过滤】
# 桌面上可能有别的进程同名窗口，只按类名找会认错。

param(
    [string]$Exe = '',
    [switch]$NoLaunch,
    [switch]$SkipShot,
    [string]$ShotPrefix = 'traypanel'
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
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
'@

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
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

function Get-AppProc {
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne 37480 }   # 已知杀不掉的僵尸残留
}

# 进程下的顶层窗口（含不可见的）。按 ProcessId 过滤。
function Get-AppWindows([int]$ProcId) {
    $pc = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $ProcId)
    $wins = $AE::RootElement.FindAll($TS::Children, $pc)
    $res = New-Object System.Collections.Generic.List[object]
    foreach ($w in $wins) {
        $res.Add(@{
            Hwnd = [int]$w.Current.NativeWindowHandle
            Name = $w.Current.Name
            El   = $w
        })
    }
    return ,$res
}

# 找出主窗口：有名字且是「零物工具箱」的那个。
function Get-MainWindow($Wins) {
    foreach ($w in $Wins) {
        if ($w.Name -and $w.Name -like '*零物工具箱*') { return $w }
    }
    return $null
}

# 找出指标面板：进程下**没有标题**的那个额外顶层窗口。
#
# 【为什么靠「没有标题」区分】
# 面板是 WS_EX_TOOLWINDOW 的工具窗，没有设标题；主窗口的标题是
# 「零物工具箱 · 状态」这类带文字的。用「进程下多了一个窗口」判定更稳，
# 但要把主窗口排除掉，所以两边都查一遍。
function Get-TrayPanelWindow($Wins, $Main) {
    foreach ($w in $Wins) {
        if ($Main -and $w.Hwnd -eq $Main.Hwnd) { continue }
        if ($w.Name -and $w.Name -like '*零物工具箱*') { continue }
        if ($w.Hwnd -eq 0) { continue }
        return $w
    }
    return $null
}

# 抓窗口位图（返回 Bitmap 对象）。
#
# 【为什么要单独抽一个函数】
# 给人看的 PNG 和给脚本看的像素判据必须用**同一张图** ——
# 抓两次会拿到两帧不同的内容，判据就可能落在另一帧上。
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

# 在面板里按 AutomationId 找元素。
function Find-ByAutomationId($Root, [string]$Id) {
    $all = $Root.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) { if ($e.Current.AutomationId -eq $Id) { return $e } }
    return $null
}

# 量一条折线区里的「高饱和度像素」。
#
# 【为什么用饱和度，而不是「与背景色的距离」】
# 折线的描边是**彩色**的（MetricCpu / MetricNet），而磁贴里的文字与底色
# 都是灰色的。用饱和度（max-min）就能把两者分开 ——
# 用「与背景色的距离」的话，上一行副标题的字形会被算进来，
# 判据就成了「有没有文字」而不是「有没有折线」。
#
# 【折线区的位置怎么推出来】
# 锚点是磁贴的副标题（TileCpuSub），折线宿主在它下面一行：
#   Margin 0,3,0,0 + Height 14（DIP）→ 物理像素 = 3×2 起、14×2 高
# （与 src/ui/TrayPanel.xaml 的 TileCpuSparkHost 一致，改那边要改这里。）
# 用 UIA 给的锚点矩形推算，而不是写死坐标 —— 换缩放比例也不会错。
function Measure-Sparkline($Bmp, $WindowRect, $AnchorRect) {
    $scale = 2   # 本机 200% 缩放；下面的判定对 1x / 1.5x 同样成立
    $top = [int]($AnchorRect.Bottom - $WindowRect.Top) + [int](3 * $scale)
    $bot = $top + [int](14 * $scale)
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

# 在面板里按名字找按钮。
function Find-DescendantByName($Root, [string]$Name) {
    $all = $Root.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) {
        if ($e.Current.Name -eq $Name) { return $e }
    }
    return $null
}

# ---- 启动 ---------------------------------------------------------------
$own = $false
if (-not $NoLaunch) {
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $p = Start-Process $Exe -PassThru
    $own = $true
    Write-Output "launched pid=$($p.Id)"
    # 等主窗口
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) { Write-Output "FAIL: 应用启动即退出 code=$($p.ExitCode)"; exit 1 }
        if ($p.MainWindowHandle -ne 0) { break }
    }
    Start-Sleep -Seconds 4   # 等托盘图标注册完
}

$proc = Get-AppProc
if (-not $proc) { Write-Output 'FAIL: 应用未在运行'; exit 1 }
$pid_ = $proc.Id
Write-Output "app pid=$pid_"

# ---- ① 托盘左键 → 面板出现 ----------------------------------------------
$before = Get-AppWindows $pid_
$mainBefore = Get-MainWindow $before
Assert-True '主窗口存在' ($null -ne $mainBefore) ("count=" + $before.Count)

$invoked = Invoke-ToolboxTrayButton
Assert-True '托盘按钮可 Invoke（左键）' ($null -ne $invoked)

# 面板出现是**异步**的（Shell 通知 → 回调 → Show），必须有界轮询。
$panel = $null
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 400
    $wins = Get-AppWindows $pid_
    $panel = Get-TrayPanelWindow $wins (Get-MainWindow $wins)
    if ($panel) { break }
}
Assert-True '① 托盘左键 → 指标面板出现' ($null -ne $panel) `
    $(if ($panel) { "hwnd=$($panel.Hwnd)" } else { "窗口数=" + (Get-AppWindows $pid_).Count })

if ($panel) {
    # 尺寸：360 × 530 DIP，本机 200% 缩放 → 720 × 1060 物理像素。
    #
    # 【为什么高度是 530 而不是 640】
    # 面板内容量出来是 505 客户区 DIP，外框至少 519。
    # 原值 640 多出来的 121 DIP 全是空白（中间那行 * 撑出来的）。
    $r = New-Object Native.Win+RECT
    [void][Native.Win]::GetWindowRect([IntPtr]$panel.Hwnd, [ref]$r)
    $pw = $r.Right - $r.Left
    $ph = $r.Bottom - $r.Top
    Write-Output "panel rect = $pw x $ph physical"
    Assert-True '面板尺寸符合 360x530 DIP（200% 缩放 → 720x1060 物理）' `
        ($pw -ge 700 -and $pw -le 740 -and $ph -ge 1020 -and $ph -le 1100) "$pw x $ph"

    # 面板内容：健康分 + 磁贴 + 动作行 + 入口
    $names = @()
    $all = $panel.El.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) { if ($e.Current.Name) { $names += $e.Current.Name } }
    $joined = ($names -join ' | ')
    Assert-True '面板含「打开工具箱」入口' (($names -contains '打开工具箱'))
    Assert-True '面板含「保持常亮」动作' (($names -contains '保持常亮'))
    Assert-True '面板含「清理屏幕」动作' (($names -contains '清理屏幕'))
    Write-Output "panel text sample: $($joined.Substring(0, [Math]::Min(300, $joined.Length)))"

    # ---- ⑤ 磁贴折线画出来了没有 ------------------------------------------
    # 【为什么必须用像素取证】
    # 折线是 Path（Shape），**不进 UIA 树**（实测面板里 UIA 只看得到
    # Text / Button / Pane），所以「折线上有没有像素」只能从渲染结果上量。
    #
    # 一张图两用：给人看的 PNG 与给脚本看的判据必须是**同一帧** ——
    # 抓两次会拿到两帧不同的内容，判据可能落在另一帧上。
    $bmp = Get-WindowBitmap ([IntPtr]$panel.Hwnd)
    Assert-True '面板位图抓取成功' ($null -ne $bmp)
    if ($bmp) {
        if (-not $SkipShot) {
            $shot = Join-Path $env:TEMP "$ShotPrefix`_panel.png"
            $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
            Write-Output "SHOT $($bmp.Width) x $($bmp.Height)"
            Write-Output "SHOT_PATH=$shot"
        }

        $anchor = Find-ByAutomationId $panel.El 'TileCpuSub'
        Assert-True 'CPU 磁贴有折线锚点（TileCpuSub）' ($null -ne $anchor)
        if ($anchor) {
            $wr = New-Object Native.Win+RECT
            [void][Native.Win]::GetWindowRect([IntPtr]$panel.Hwnd, [ref]$wr)
            $m = Measure-Sparkline $bmp $wr $anchor.Current.BoundingRectangle
            if ($m) {
                # 判定线宽：一条贯穿全宽的折线至少要有「两个满宽」的饱和像素。
                # 【为什么看总量而不是「最宽的一行」】
                # 线可能是斜的 —— 两点之间落差很大时，斜线每一行都不到满宽，
                # 拿「最宽行」去判会把正常的斜线误判成没画。
                Write-Output ("spark pixels = {0} (band width {1})" -f $m.Count, $m.BandWidth)
                Assert-True '⑤ CPU 磁贴的折线画出来了' `
                    ($m.Count -ge ($m.BandWidth * 2)) ("saturated=" + $m.Count)
            } else {
                Assert-True '⑤ CPU 磁贴的折线画出来了' $false '折线区算不出来'
            }
        }
        $bmp.Dispose()
    }

    # ---- ⑥ 进程表的三列：顺序、不重叠、没被裁掉 ---------------------------
    # 【为什么这三条要断言】
    # 旧实现把三列宽度写死成 228 + 62 + 44 = 334 DIP，
    # 而那个 334 是按**外框**宽度推算的 —— 客户区还要再减去不可见边框，
    # 实测少 13 DIP，于是最右边的 CPU 列整个被裁掉（数字看不见）。
    # 这类问题不报错、不崩，只是「有一列是空的」。
    $nameCell = Find-ByAutomationId $panel.El 'ProcName0'
    $memCell  = Find-ByAutomationId $panel.El 'ProcMem0'
    $cpuCell  = Find-ByAutomationId $panel.El 'ProcCpu0'
    $headCpu  = Find-ByAutomationId $panel.El 'ProcHeadCpu'
    Assert-True '进程表三列都在' `
        (($null -ne $nameCell) -and ($null -ne $memCell) -and ($null -ne $cpuCell))
    if ($nameCell -and $memCell -and $cpuCell) {
        $nr = $nameCell.Current.BoundingRectangle
        $mr = $memCell.Current.BoundingRectangle
        $cr = $cpuCell.Current.BoundingRectangle
        # 客户区右边界：取所有 Pane 里最靠右的那个（RootGrid）的右边缘。
        # 【为什么不能取「所有后代元素的最右边缘」】
        # 表格自己的格子也在里面 —— 格子被裁掉时它的矩形照样伸到客户区外，
        # 那样算出来的边界会被「被裁掉的格子」带着走，断言恒成立。
        $clientRight = 0
        $allDesc = $panel.El.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
        foreach ($e in $allDesc) {
            if ($e.Current.ControlType.ProgrammaticName -ne 'ControlType.Pane') { continue }
            $rr = $e.Current.BoundingRectangle
            if (($rr.X + $rr.Width) -gt $clientRight) { $clientRight = $rr.X + $rr.Width }
        }
        Write-Output ("proc row0: name={0}..{1} mem={2}..{3} cpu={4}..{5} clientRight={6}" -f `
            [int]$nr.X, [int]($nr.X + $nr.Width), [int]$mr.X, [int]($mr.X + $mr.Width),
            [int]$cr.X, [int]($cr.X + $cr.Width), [int]$clientRight)
        Assert-True '⑥ 进程表列序正确（名称 → 内存 → CPU）' `
            (($nr.X -lt $mr.X) -and ($mr.X -lt $cr.X)) `
            ("name={0} mem={1} cpu={2}" -f [int]$nr.X, [int]$mr.X, [int]$cr.X)
        Assert-True '⑥ 进程表三列不重叠' `
            (($nr.X + $nr.Width) -le ($mr.X + 1) -and ($mr.X + $mr.Width) -le ($cr.X + 1))
        Assert-True '⑥ 进程表 CPU 列没被裁掉（完全落在客户区内）' `
            ((($cr.X + $cr.Width) -le ($clientRight + 1)) -and ($cr.Width -gt 0)) `
            ("cpuRight=" + [int]($cr.X + $cr.Width) + " clientRight=" + [int]$clientRight)
        if ($headCpu) {
            $hr = $headCpu.Current.BoundingRectangle
            Assert-True '⑥ 表头与数据行的 CPU 列对齐' `
                ([Math]::Abs($hr.X - $cr.X) -le 2) `
                ("head=" + [int]$hr.X + " row0=" + [int]$cr.X)
        }
    }

    # ---- ⑥ 面板底部没被裁掉 ------------------------------------------------
    # 【为什么要有这条断言】
    # 面板高度是写死的常量（kPanelHeightLogical = 530 DIP）。
    # 内容比常量高的时候，最下面那行会被裁掉 —— 不报错、不崩，
    # 只是「打开工具箱」按钮看不见也点不到，面板成了一条死路。
    # 「内容有多高」在界面上看不出来，只能从
    # 「底部按钮的矩形 vs 客户区底边」上量。
    $crc = New-Object Native.Win+RECT
    [void][Native.Win]::GetClientRect([IntPtr]$panel.Hwnd, [ref]$crc)
    $cpt = New-Object Native.Win+POINT
    $cpt.X = 0
    $cpt.Y = $crc.Bottom
    [void][Native.Win]::ClientToScreen([IntPtr]$panel.Hwnd, [ref]$cpt)
    $clientBottom = $cpt.Y

    $openBtnRect = $null
    $openEl = Find-DescendantByName $panel.El '打开工具箱'
    if ($openEl) { $openBtnRect = $openEl.Current.BoundingRectangle }
    Assert-True '⑥ 底部「打开工具箱」按钮完整落在客户区里（没被裁掉）' `
        ($null -ne $openBtnRect -and $openBtnRect.Bottom -le ($clientBottom + 1)) `
        $(if ($openBtnRect) {
              ("btnBottom=" + [int]$openBtnRect.Bottom + " clientBottom=" + [int]$clientBottom)
          } else { '找不到「打开工具箱」按钮' })

    # ---- ④ 不抢焦点 ------------------------------------------------------
    # 面板用 AppWindow::Show(false) 弹出，**不应该**成为前台窗口。
    $fg = [Native.Win]::GetForegroundWindow()
    Assert-True '④ 面板弹出不抢焦点' ($fg -ne [IntPtr]$panel.Hwnd) `
        ("fg=$fg panel=$($panel.Hwnd)")

    # ---- ③ 面板内「打开工具箱」→ 主窗口出现 -------------------------------
    $openBtn = Find-DescendantByName $panel.El '打开工具箱'
    Assert-True '面板内可定位「打开工具箱」按钮' ($null -ne $openBtn)
    if ($openBtn) {
        $inv = $false
        try { Invoke-UiaInvoke $openBtn; $inv = $true } catch { $inv = $false }
        Assert-True '「打开工具箱」可 Invoke' $inv
        Start-Sleep -Seconds 2
        $wins = Get-AppWindows $pid_
        $mainAfter = Get-MainWindow $wins
        Assert-True '③「打开工具箱」→ 主窗口出现' ($null -ne $mainAfter)
        # 点完之后面板应该收起（它自己 Hide 掉了）
        $still = Get-TrayPanelWindow $wins (Get-MainWindow $wins)
        Assert-True '「打开工具箱」后面板收起' ($null -eq $still)
    }
}

# ---- ② 再点一次托盘 → 面板收起 ------------------------------------------
# 先把面板重新弹出来
Invoke-ToolboxTrayButton | Out-Null
$panel2 = $null
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 400
    $wins = Get-AppWindows $pid_
    $panel2 = Get-TrayPanelWindow $wins (Get-MainWindow $wins)
    if ($panel2) { break }
}
Assert-True '面板可再次弹出' ($null -ne $panel2)
if ($panel2) {
    Invoke-ToolboxTrayButton | Out-Null
    $gone = $false
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 400
        $wins = Get-AppWindows $pid_
        if ($null -eq (Get-TrayPanelWindow $wins (Get-MainWindow $wins))) { $gone = $true; break }
    }
    Assert-True '② 再点一次托盘 → 面板收起' $gone
}

Write-Output ""
Write-Output "RESULT: pass=$script:Pass fail=$script:Fail"
if ($script:Fail -gt 0) { exit 1 }
exit 0
