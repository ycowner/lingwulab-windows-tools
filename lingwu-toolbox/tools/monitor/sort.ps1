param(
    [string]$Exe,
    [string]$Prefix = 'sort'
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
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
[void][Native.Win]::SetProcessDPIAware()

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$PW = 0x00000002

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

# 断言计数器。
#
# 【为什么这个脚本原来只在「拍照片」】
# 它原本只把三种排序各截一张图留给人看，从不断言、也从不返回非零 ——
# 于是「排序真的变了」和「脚本跑完但什么都没验成」在验收板上长得一模一样。
# 现在除了截图，还读回每一档的第一行，断言三档的行序互不相同。
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

# 行模板没有 x:Name，只能靠 ListItem 暴露的合成名（4 个 TextBlock 拼起来的）。
function Get-FirstRow($win) {
    $c = New-Object System.Windows.Automation.PropertyCondition(
        $AE::AutomationIdProperty, 'ProcessList')
    $list = $win.FindFirst($TS::Descendants, $c)
    if (-not $list) { return '(no list)' }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
    $items = $list.FindAll($TS::Descendants, $cond)
    if ($items.Count -eq 0) { return '(no rows)' }
    return $items[0].Current.Name
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

function Invoke-Id([string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    $el = $win.FindFirst($TS::Descendants, $c)
    if (-not $el) { return "NOT FOUND $id" }
    $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    return 'ok'
}

$a = Join-Path $env:TEMP "$Prefix`_1_default.png"
Write-Host ("default  : " + (Shot $a))
$rowDefault = Get-FirstRow $win
Write-Host ("  first row: " + $rowDefault)
Assert-True '默认档列表有行' ($rowDefault -ne '(no list)' -and $rowDefault -ne '(no rows)') $rowDefault

$invCpu = Invoke-Id 'HeaderCpu'
Write-Host ("click CPU: " + $invCpu)
Assert-True '点得动 CPU 表头' ($invCpu -eq 'ok') $invCpu
Start-Sleep -Seconds 3
$b = Join-Path $env:TEMP "$Prefix`_2_cpu.png"
Write-Host ("cpu      : " + (Shot $b))
$rowCpu = Get-FirstRow $win
Write-Host ("  first row: " + $rowCpu)
Assert-True 'CPU 档的行序与默认档不同' ($rowCpu -ne $rowDefault)

$invName = Invoke-Id 'HeaderName'
Write-Host ("click Name: " + $invName)
Assert-True '点得动名称表头' ($invName -eq 'ok') $invName
Start-Sleep -Seconds 3
$c = Join-Path $env:TEMP "$Prefix`_3_name.png"
Write-Host ("name     : " + (Shot $c))
$rowName = Get-FirstRow $win
Write-Host ("  first row: " + $rowName)
Assert-True '名称档的行序与 CPU 档不同' ($rowName -ne $rowCpu)

Write-Host "A=$a"
Write-Host "B=$b"
Write-Host "C=$c"

Write-Host ''
Write-Host '=========================================================='
Write-Host ("RESULT: pass={0}  fail={1}" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 } else { exit 0 }
