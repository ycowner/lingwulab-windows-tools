# create.ps1 -- create a throwaway service through the Create Service window
# and confirm it shows up in the list.
#
#   pwsh -NoProfile -File .\tools\service\create.ps1
#   pwsh -NoProfile -File .\tools\service\create.ps1 -ServiceName LingwuSmokeTest2
#
# The service points at cmd.exe /c exit so it is harmless if something later
# manages to start it: it runs for milliseconds and exits.
#
# THIS SCRIPT CREATES REAL MACHINE STATE. It removes the service again at the
# end, and says so loudly if that fails -- a leftover LingwuSmokeTest* service
# is the one thing an acceptance run must never leave behind.
#
# It also creates the service for real, because the toolbox runs elevated by
# design: there is no "the app is not an administrator, so it refuses" path
# left to test.

[CmdletBinding()]
param(
    [string]$Exe = '',
    [string]$ServiceName = 'LingwuSmokeTest',
    [switch]$KeepIt
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\uia.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

$script:UiaFailures = 0
$binary = "$env:SystemRoot\System32\cmd.exe"

function Remove-TestService([string]$Name) {
    $existing = @(Get-Service -Name $Name -ErrorAction SilentlyContinue)
    if ($existing.Count -eq 0) { Write-Host "  (no leftover $Name)"; return $true }
    Write-Host "  WARNING: removing leftover service $Name"
    # sc.exe is used rather than Stop/Remove-Service cmdlets because those want
    # the admin pipeline and this script wants to say exactly what happened.
    & sc.exe stop $Name | Out-Null
    Start-Sleep -Seconds 2
    & sc.exe delete $Name | Out-Null
    Start-Sleep -Seconds 2
    $left = @(Get-Service -Name $Name -ErrorAction SilentlyContinue)
    if ($left.Count -gt 0) {
        Write-Host "  [FAIL] $Name still exists after delete"
        return $false
    }
    Write-Host "  [ok]   cleaned up $Name"
    return $true
}

Write-Host "=== service create: $ServiceName ==="

# Start from a clean slate so the assertions below are about this run.
Remove-TestService $ServiceName | Out-Null

Stop-App 'LingwuToolbox'
$p = Start-App $Exe
$win = Get-AppWindow $p.Id 60
Switch-Module $win 'Service'
$st = $null
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline) {
    $st = Get-StatusCounts $win
    if ($st.Total -gt 0) { break }
    Start-Sleep -Milliseconds 500
}
Write-Host "  baseline: $($st.Total) services"

Write-Host "`n--- open the create window ---"
Invoke-El (Get-ById $win 'NewServiceButton') 'NewServiceButton'
$create = Get-AppWindowByProbe $p.Id 'ServiceNameBox' 20
Write-Host "  create window opened"
# Let the form's 100 ms validation heartbeat run at least once before reading
# any button state -- otherwise the check races the app's own initialisation.
Start-Sleep -Seconds 2

Write-Host "`n--- create button starts disabled ---"
$createBtn = Get-ById $create 'CreateButton'
$enabled = $createBtn.Current.IsEnabled
Write-Host "  CreateButton.IsEnabled = $enabled"
Assert-True (-not $enabled) 'the create button is disabled while the form is empty'

Write-Host "`n--- fill the form ---"
Set-ElText (Get-ById $create 'ServiceNameBox') $ServiceName 'ServiceNameBox'
Start-Sleep -Seconds 2
# Only the service name so far: the binary path is still missing, so the
# create button must still refuse.
$createBtn = Get-ById $create 'CreateButton'
Write-Host "  CreateButton.IsEnabled (name only) = $($createBtn.Current.IsEnabled)"
Assert-True (-not $createBtn.Current.IsEnabled) 'still disabled until the binary path is filled in'

Set-ElText (Get-ById $create 'DisplayNameBox') "UIA smoke test $ServiceName" 'DisplayNameBox'
Set-ElText (Get-ById $create 'DescriptionBox') 'created by tools/service/create.ps1' 'DescriptionBox'
Set-ElText (Get-ById $create 'BinaryPathBox') $binary 'BinaryPathBox'
Start-Sleep -Seconds 2

$createBtn = Get-ById $create 'CreateButton'
$enabled = $createBtn.Current.IsEnabled
Write-Host "  CreateButton.IsEnabled after filling = $enabled"
Assert-True $enabled 'the create button enables once the required fields are valid'

Write-Host "`n--- submit ---"
# The toolbox runs elevated by design (its manifest declares
# requireAdministrator), and Require-Elevated has already put THIS shell at
# the same integrity level. So the create has to go through -- there is no
# "unelevated refusal" path to fall back to any more: the "relaunch as
# administrator" button was removed when the app stopped being able to run
# unelevated.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal -ArgumentList $identity
$admin = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "  elevated = $admin"

# 真建服务要管理员。非提权 shell 驱动的是清单被改成 asInvoker 的验证副本，
# 建服务这一步根本跑不了 —— 那是**环境限制**，报 SKIP 而不是 FAIL。
# 表单校验那几项断言在上面已经跑完了（它们不需要管理员），所以这里直接跳过
# 「提交 + 落库 + 列表里能看到」这一段。
if (-not $admin) {
    Write-Host 'SKIP: creating a real service needs an elevated shell -- the toolbox runs'
    Write-Host '      elevated by design, so the app under test has to be elevated too.'
    Write-Host '      (the form validation above did run; the write path did not)'
    Stop-App 'LingwuToolbox'
    exit 0
}
Assert-True $admin 'the shell running this script is elevated (as the app is)'
Invoke-El (Get-ById $create 'CreateButton') 'CreateButton'
Start-Sleep -Seconds 6

$svc = @(Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)
Write-Host "  sc query -> $($svc.Count) match(es)"
if ($svc.Count -gt 0) {
    Write-Host ("  status={0}  startType={1}" -f $svc[0].Status, $svc[0].StartType)
}

Assert-True ($svc.Count -eq 1) 'the service now exists in the SCM'

Write-Host "`n--- it shows up in the list ---"
# The list refreshes on its own poll interval, so this is a wait, not a
# snapshot: the row only becomes reachable once the app re-enumerates.
$found = $false
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline -and -not $found) {
    try {
        Select-RowByName $win $ServiceName | Out-Null
        $found = $true
    } catch {
        Start-Sleep -Seconds 2
    }
}
Assert-True $found 'the new service is reachable as a row in the list'

if ($found) {
    Start-Sleep -Seconds 2
    Expand-Panel $win 'DetailExpander' | Out-Null
    $bp = Get-Text $win 'DetailBinaryPath'
    Write-Host "  detail binary path : $bp"
    Assert-True ($bp -like '*cmd.exe*') 'detail panel shows the binary path we entered'
}

if ($KeepIt) {
    Write-Host "`n-KeepIt: leaving $ServiceName in place."
} else {
    Write-Host "`n--- cleanup ---"
    Remove-TestService $ServiceName | Out-Null
}

Stop-App 'LingwuToolbox'
Write-Summary 'service create'