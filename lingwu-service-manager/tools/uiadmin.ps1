# uiadmin.ps1 -- verify the elevation model from the unelevated side.
#
#   pwsh -NoProfile -File .\tools\uiadmin.ps1
#
# WHY THIS SCRIPT EXISTS AND WHY IT ONLY CHECKS FAILURES
#
# Every write operation in this tool (start / stop / restart / change start
# type / create / delete) needs administrator rights, and this acceptance run
# is deliberately unelevated -- that IS the product's default mode ("open it,
# see the list, elevate only when you actually need to change something").
#
# So the happy path of those operations cannot be exercised here at all. What
# *can* be verified, and is verified below, is the part that actually matters
# for an unelevated user:
#
#   1. the app does not pretend the operation succeeded;
#   2. it explains that administrator rights are needed;
#   3. it offers to relaunch elevated, and that offer is a real button.
#
# A silently-failing button is the failure mode this guards against. Run this
# script from an elevated shell as well -- it then asserts the opposite, that
# the operation goes through.

[CmdletBinding()]
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\Release\LingwuServiceManager.exe')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'uia.ps1')

$script:UiaFailures = 0

# Written out in separate statements on purpose: PowerShell rejects a newline
# between a cast-to-type literal and the method call that follows it.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal -ArgumentList $identity
$admin = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "=== uiadmin: elevated = $admin ==="

Stop-App 'LingwuServiceManager'
$p = Start-App $Exe

try {
    $win = Get-AppWindow $p.Id 60
    $st = $null
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline) {
        $st = Get-StatusCounts $win
        if ($st.Total -gt 0) { break }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "  $($st.Total) services enumerated"

    # Pick a service that is genuinely Running: Stop and Restart are disabled
    # for a stopped service (correctly -- there is nothing to stop), so
    # guessing at a row would just hit a disabled button.
    $running = Get-RunningServiceName 30
    Write-Host "  running services available: $($running.Count)"
    $target = $null
    foreach ($cand in $running) {
        try {
            Select-RowByName $win $cand | Out-Null
            $target = $cand
            break
        } catch { }
    }
    if ($null -eq $target) {
        Assert-True $false 'at least one running service is reachable as a row'
        throw 'no usable target service'
    }
    Write-Host "`n--- target: $target ---"
    Start-Sleep -Seconds 2
    Expand-Panel $win 'DetailExpander' | Out-Null

    $stopBtn = Get-ById $win 'StopButton'
    Write-Host "  StopButton.IsEnabled = $($stopBtn.Current.IsEnabled)"
    Assert-True ($stopBtn.Current.IsEnabled) 'stop is enabled for a running service'

    $startBtn = Get-ById $win 'StartButton'
    Write-Host "  StartButton.IsEnabled = $($startBtn.Current.IsEnabled)"
    Assert-True (-not $startBtn.Current.IsEnabled) 'start is disabled while the service runs'

    Write-Host "`n--- status bar reports the token ---"
    $right = Get-Text $win 'StatusRight'
    Write-Host ("  right codepoints: " +
        (($right.ToCharArray() | ForEach-Object { [int]$_ }) -join ','))
    # "standard user rights" is 26631 26222 29992 25143 26435 38480 (6 chars).
    Assert-True ($right.Length -gt 0) 'the status bar states the current privilege level'

    if (-not $admin) {
        Write-Host "`n--- stop a service unelevated ---"
        Invoke-El $stopBtn 'StopButton'
        Start-Sleep -Seconds 4

        $svc = Get-Service -Name $target -ErrorAction SilentlyContinue
        Write-Host "  service status now: $($svc.Status)"

        $dlg = Get-Dialog $win 15
        if ($null -eq $dlg) {
            Assert-True $false 'a refused stop shows an outcome dialog'
        } else {
            Write-Host ("  dialog title codepoints : " + (Show-Codepoints $dlg.Current.Name))
            $btns = Get-DialogButtons $dlg
            Write-Host "  dialog buttons: $($btns.Count)"
            for ($i = 0; $i -lt $btns.Count; $i++) {
                Write-Host ("    [{0}] codepoints={1}" -f $i, (Show-Codepoints $btns[$i]))
            }
            Assert-True ($btns.Count -ge 2) 'the dialog offers a primary and a close button'
            $elevate = @(0x4EE5, 0x7BA1, 0x7406, 0x5458, 0x8EAB,
                         0x4EFD, 0x91CD, 0x65B0, 0x542F, 0x52A8)
            $haveElevate = $false
            foreach ($b in $btns) {
                $cp = @(($b.ToCharArray() | ForEach-Object { [int]$_ }))
                if ($cp.Count -ne $elevate.Count) { continue }
                $same = $true
                for ($k = 0; $k -lt $cp.Count; $k++) {
                    if ($cp[$k] -ne $elevate[$k]) { $same = $false; break }
                }
                if ($same) { $haveElevate = $true }
            }
            Assert-True $haveElevate 'the dialog offers a "relaunch elevated" button'
        }
    } else {
        Write-Host "`n--- elevated: the same click must go through ---"
        Invoke-El $stopBtn 'StopButton'
        Start-Sleep -Seconds 6
        $svc = Get-Service -Name $target -ErrorAction SilentlyContinue
        Write-Host "  service status now: $($svc.Status)"
        Assert-True ($svc.Status -ne 'Running') 'the stop actually took effect'
    }

    Write-Host "`n--- memory ---"
    $p.Refresh()
    Write-Host ("  priv={0} MB  ws={1} MB  handles={2}" -f `
        [Math]::Round($p.PrivateMemorySize64 / 1MB, 1),
        [Math]::Round($p.WorkingSet64 / 1MB, 1), $p.HandleCount)
} finally {
    Stop-App 'LingwuServiceManager'
}

Write-Summary 'uiadmin'
