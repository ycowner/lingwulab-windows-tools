# uicreate.ps1 -- create a throwaway service through the Create Service window
# and confirm it shows up in the list.
#
#   pwsh -NoProfile -File .\tools\uicreate.ps1
#   pwsh -NoProfile -File .\tools\uicreate.ps1 -ServiceName LingwuSmokeTest2
#
# The service points at cmd.exe /c exit so it is harmless if something later
# manages to start it: it runs for milliseconds and exits.
#
# THIS SCRIPT CREATES REAL MACHINE STATE. It removes the service again at the
# end, and says so loudly if that fails -- a leftover LingwuSmokeTest* service
# is the one thing an acceptance run must never leave behind.

[CmdletBinding()]
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\Release\LingwuServiceManager.exe'),
    [string]$ServiceName = 'LingwuSmokeTest',
    [switch]$KeepIt
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'uia.ps1')

$script:UiaFailures = 0
$binary = "$env:SystemRoot\System32\cmd.exe"

function Remove-TestService([string]$Name) {
    $existing = @(Get-Service -Name $Name -ErrorAction SilentlyContinue)
    if ($existing.Count -eq 0) { Write-Host "  (no leftover $Name)"; return $true }
    Write-Host "  WARNING: removing leftover service $Name"
    # sc.exe is used rather than Stop/Remove-Service cmdlets because those want
    # the admin pipeline and this script runs unelevated on purpose.
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

Write-Host "=== uicreate: $ServiceName ==="

# Start from a clean slate so the assertions below are about this run.
Remove-TestService $ServiceName | Out-Null

Stop-App 'LingwuServiceManager'
$p = Start-App $Exe
$win = Get-AppWindow $p.Id 60
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
Set-ElText (Get-ById $create 'DescriptionBox') 'created by tools/uicreate.ps1' 'DescriptionBox'
Set-ElText (Get-ById $create 'BinaryPathBox') $binary 'BinaryPathBox'
Start-Sleep -Seconds 2

$createBtn = Get-ById $create 'CreateButton'
$enabled = $createBtn.Current.IsEnabled
Write-Host "  CreateButton.IsEnabled after filling = $enabled"
Assert-True $enabled 'the create button enables once the required fields are valid'

Write-Host "`n--- submit ---"
# PowerShell will not let a newline sit between a cast-to-type literal and the
# method call that follows it, so the elevation check is written out in
# separate statements instead of one fluent expression.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal -ArgumentList $identity
$admin = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "  elevated = $admin"
Invoke-El (Get-ById $create 'CreateButton') 'CreateButton'
Start-Sleep -Seconds 6

$svc = @(Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)
Write-Host "  sc query -> $($svc.Count) match(es)"
if ($svc.Count -gt 0) {
    Write-Host ("  status={0}  startType={1}" -f $svc[0].Status, $svc[0].StartType)
}

if ($admin) {
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
} else {
    # Creating a service needs administrator rights, and this acceptance run is
    # deliberately unelevated. So what is under test here is the *refusal*
    # path: the app must say why it failed and offer to relaunch elevated,
    # rather than failing silently or pretending it worked.
    Write-Host "`n--- unelevated: the refusal path is what matters ---"
    Assert-True ($svc.Count -eq 0) 'no service was created without administrator rights'

    $dlg = Get-Dialog $win
    Write-Host "  dialog title codepoints : $(Show-Codepoints $dlg.Current.Name)"
    $btns = Get-DialogButtons $dlg
    Write-Host "  buttons: $($btns.Count)"
    for ($i = 0; $i -lt $btns.Count; $i++) {
        Write-Host ("    [{0}] codepoints={1}" -f $i, (Show-Codepoints $btns[$i]))
    }
    Assert-True ($btns.Count -ge 2) 'the refusal dialog offers a primary and a close button'
    # The primary button must be the elevation offer. Codepoints, not text:
    # this file is pure ASCII by repo rule, so the Chinese label cannot be
    # written literally here.
    #
    # The label is the six characters "yi guan li yuan shen fen chong xin qi
    # dong" = "relaunch as administrator", which is the linger of
    # U+4EE5 U+7BA1 U+7406 U+5458 U+8EAB U+4EFD U+91CD U+65B0 U+542F U+52A8.
    # That comment is deliberately romanised rather than written in Chinese:
    # a Chinese comment here would put non-ASCII bytes in a .ps1, which the
    # repo forbids because the console code page mangles them.
    $elevate = @(0x4EE5, 0x7BA1, 0x7406, 0x5458, 0x8EAB, 0x4EFD, 0x91CD, 0x65B0, 0x542F, 0x52A8)
    $haveElevate = $false
    foreach ($b in $btns) {
        $cp = @(( $b.ToCharArray() | ForEach-Object { [int]$_ }))
        if ($cp.Count -eq $elevate.Count) {
            $same = $true
            for ($k = 0; $k -lt $cp.Count; $k++) { if ($cp[$k] -ne $elevate[$k]) { $same = $false; break } }
            if ($same) { $haveElevate = $true }
        }
    }
    Assert-True $haveElevate 'the dialog offers a "relaunch elevated" button'
    Write-Host ("`n  NOTE: creation itself can only be verified from an elevated" +
                " run of this script.")
}

if ($KeepIt) {
    Write-Host "`n-KeepIt: leaving $ServiceName in place."
} else {
    Write-Host "`n--- cleanup ---"
    Remove-TestService $ServiceName | Out-Null
}

Stop-App 'LingwuServiceManager'
Write-Summary 'uicreate'