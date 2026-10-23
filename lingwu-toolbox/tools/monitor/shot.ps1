param(
    [string]$Exe,
    [string]$Prefix,
    [int]$WarmupSec = 8,
    [int]$GapSec = 10
)
. (Join-Path $PSScriptRoot '..\common\common.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@

# 必须先声明 DPI 感知。
#
# 不声明时 PowerShell 线程处于 DPI 虚拟化状态：GetWindowRect 返回的是**逻辑**
# 像素，而绘图用的是**物理**像素。在 150% 缩放下两者差 1.5 倍，截出来的图
# 右侧和底部会带进窗口外面几十像素的内容（表现为窗口右边缘黏着别的窗口的
# 文字碎片），看起来像应用渲染错位，其实只是坐标系不一致。
[void][Native.Win]::SetProcessDPIAware()

# PW_RENDERFULLCONTENT：让 PrintWindow 走 DWM 的合成表面。
# 不加这一位时，WinUI 3 这类 DirectComposition 窗口会抓到一片空白。
$PW_RENDERFULLCONTENT = 0x00000002

function Save-WindowShot {
    param([IntPtr]$Handle, [string]$Path)
    $r = New-Object Native.Win+RECT
    if (-not [Native.Win]::GetWindowRect($Handle, [ref]$r)) { return "GetWindowRect failed" }
    $w = $r.Right - $r.Left
    $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { return "bad rect $w x $h" }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)

    # 首选 PrintWindow：抓的是窗口自己的内容，**与遮挡无关**。
    # 曾经用 CopyFromScreen，12 秒后的第二张截图抓到了盖在上面的浏览器 ——
    # 因为 CopyFromScreen 取的是屏幕该矩形的像素，不是这个窗口的。
    # 那个假象一度让我误判成「应用崩了」，其实进程一直活着。
    $hdc = $g.GetHdc()
    $ok = [Native.Win]::PrintWindow($Handle, $hdc, $PW_RENDERFULLCONTENT)
    $g.ReleaseHdc($hdc)

    if (-not $ok) {
        # 兜底：PrintWindow 失败时退回屏幕截图（先把窗口提到前台）。
        [void][Native.Win]::SetForegroundWindow($Handle)
        Start-Sleep -Milliseconds 400
        $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    }

    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()
    return "$w x $h$(if(-not $ok){' (via screen fallback)'})"
}

Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$trace = Join-Path $env:LOCALAPPDATA 'LingwuToolbox\trace.log'
if (Test-Path $trace) { Remove-Item $trace -Force }

$p = Start-Process $Exe -PassThru
Write-Output "launched pid=$($p.Id)"

# 等主窗口出现（最多 30 秒）
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    $p.Refresh()
    if ($p.HasExited) { Write-Output "EXITED early code=$($p.ExitCode)"; exit 1 }
    if ($p.MainWindowHandle -ne 0) { $hwnd = $p.MainWindowHandle; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO MAIN WINDOW'; exit 1 }
Write-Output "hwnd=$hwnd after warmup wait"

Start-Sleep -Seconds $WarmupSec
[void][Native.Win]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 600

$shotA = Join-Path $env:TEMP "$Prefix`_a.png"
Write-Output ("A " + (Save-WindowShot -Handle $hwnd -Path $shotA))

Start-Sleep -Seconds $GapSec
$shotB = Join-Path $env:TEMP "$Prefix`_b.png"
Write-Output ("B " + (Save-WindowShot -Handle $hwnd -Path $shotB))

$p.Refresh()
Write-Output ("privMB=" + [math]::Round($p.PrivateMemorySize64 / 1MB, 1) +
              " wsMB=" + [math]::Round($p.WorkingSet64 / 1MB, 1) +
              " handles=" + $p.HandleCount)
Write-Output "A_PATH=$shotA"
Write-Output "B_PATH=$shotB"
