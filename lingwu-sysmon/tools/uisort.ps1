param(
    [string]$Exe,
    [string]$Prefix = 'sort'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
[void][Native.Win]::SetProcessDPIAware()

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$PW = 0x00000002

Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
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

function Invoke-Id([string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    $el = $win.FindFirst($TS::Descendants, $c)
    if (-not $el) { return "NOT FOUND $id" }
    $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    return 'ok'
}

$a = Join-Path $env:TEMP "$Prefix`_1_default.png"
Write-Host ("default  : " + (Shot $a))

Write-Host ("click CPU: " + (Invoke-Id 'HeaderCpu'))
Start-Sleep -Seconds 3
$b = Join-Path $env:TEMP "$Prefix`_2_cpu.png"
Write-Host ("cpu      : " + (Shot $b))

Write-Host ("click Name: " + (Invoke-Id 'HeaderName'))
Start-Sleep -Seconds 3
$c = Join-Path $env:TEMP "$Prefix`_3_name.png"
Write-Host ("name     : " + (Shot $c))

Write-Host "A=$a"
Write-Host "B=$b"
Write-Host "C=$c"
