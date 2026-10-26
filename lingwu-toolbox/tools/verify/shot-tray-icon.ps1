# tools/verify/shot-tray-icon.ps1 -- capture the toolbox tray icon pixels (临时验收脚本)
#
# 用途：验收托盘图标是不是真的换成了小蜜蜂。
# 做法：用仓库自带的 tools\common\tray.ps1 找到托盘按钮的屏幕矩形，
#      再按该矩形抓一块屏幕像素存成 PNG。
param(
    [string]$Out = "$PSScriptRoot\..\..\build\tray-icon.png"
)
. (Join-Path $PSScriptRoot '..\..\tools\common\common.ps1')
. (Join-Path $PSScriptRoot '..\..\tools\common\tray.ps1')

Add-Type -AssemblyName System.Drawing

# KeepOpen is required: the overflow flyout closes when Get-ToolboxTrayState
# returns without it, and a closed flyout no longer paints the icon --
# the rect is then stale and the capture comes back empty.
$state = Get-ToolboxTrayState -KeepOpen
Write-Output ("present=" + $state.Present + " overflow=" + $state.Overflow)
if (-not $state.Rect) { Write-Output 'NO RECT'; exit 1 }
$r = $state.Rect
Write-Output ("rect=" + $r.X + "," + $r.Y + " " + $r.W + "x" + $r.H)

try {
    $pad = 6
    $x = [Math]::Max(0, $r.X - $pad)
    $y = [Math]::Max(0, $r.Y - $pad)
    $w = $r.W + 2 * $pad
    $h = $r.H + 2 * $pad
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $dir = Split-Path -Parent $Out
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "OUT=$Out"
} finally {
    Close-ToolboxTray $state.Ctx
}
