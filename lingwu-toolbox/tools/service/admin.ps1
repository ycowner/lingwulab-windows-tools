# admin.ps1 -- verify the elevation model of the merged toolbox.
#
#   pwsh -NoProfile -File .\tools\service\admin.ps1
#
# WHY THIS SCRIPT LOOKS DIFFERENT FROM THE ONE IT REPLACES
#
# The old version of this script tested the unelevated app: it started the
# service manager as a normal user, asked it to stop a service, and asserted
# that the app refused, said why, and offered a "relaunch as administrator"
# button. That mode is gone. The toolbox now declares requireAdministrator,
# so the app is ALWAYS elevated, and the "relaunch elevated" entry point was
# deleted together with it (Phase 4 of the merge plan).
#
# What is left to verify is the opposite, and it is worth verifying because
# "the app is supposed to be elevated" is a claim about the manifest, not
# about the running process:
#
#   1. the app really is running elevated -- its own status bar says so;
#   2. a write operation (stop a service) goes through on the first click,
#      with no elevation prompt in between;
#   3. no "relaunch as administrator" dialog shows up any more;
#   4. the machine is left exactly as it was found -- the service that was
#      stopped for the test is started again before the script exits.
#
# Pure ASCII on purpose (repo rule for .ps1): the Chinese labels are matched
# by codepoint, never written literally.

[CmdletBinding()]
param(
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\uia.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

$script:UiaFailures = 0

# Written out in separate statements on purpose: PowerShell rejects a newline
# between a cast-to-type literal and the method call that follows it.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal -ArgumentList $identity
$admin = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "=== service admin: shell elevated = $admin ==="

# 这个脚本验的是「提权模型」—— 应用本身以管理员身份运行，写操作直接生效、
# 中间不弹提权框。要验这个，**驱动它的 shell 也必须是管理员**：
# 非提权 shell 驱动的是清单被改成 asInvoker 的验证副本，应用不是管理员，
# 那这条契约根本无从谈起。
#
# 所以非提权时是 SKIP（环境限制），不是 FAIL（产品缺陷）。
if (-not $admin) {
    Write-Host 'SKIP: this check needs an elevated shell -- the toolbox runs elevated by design,'
    Write-Host '      so the app under test has to be elevated too. Re-run from an elevated shell:'
    Write-Host '      pwsh -NoProfile -File .\tools\service\admin.ps1'
    # 注意：不能走 Write-Summary —— 它会打印 "PASS:"，那是在骗人。
    exit 0
}
Assert-True $admin 'this shell is elevated, so the app under test is too'

Stop-App 'LingwuToolbox'
$p = Start-App $Exe

# The service that was stopped for the test. Restored in the finally block.
$stopped = $null

try {
    $win = Get-AppWindow $p.Id 60
    Switch-Module $win 'Service'

    $st = $null
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline) {
        $st = Get-StatusCounts $win
        if ($st.Total -gt 0) { break }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "  $($st.Total) services enumerated"

    Write-Host "`n--- the app reports its own privilege level ---"
    $right = Get-Text $win 'StatusRight'
    Write-Host ("  right codepoints: " +
        (($right.ToCharArray() | ForEach-Object { [int]$_ }) -join ','))
    # "guan li yuan quan xian" = administrator rights =
    # U+7BA1 U+7406 U+5458 U+8EAB U+6743 U+9650.
    $wantAdmin = @(0x7BA1, 0x7406, 0x5458, 0x8EAB, 0x6743, 0x9650)
    $rightCp = @(($right.ToCharArray() | ForEach-Object { [int]$_ }))
    $hasAdmin = $false
    for ($i = 0; $i -le $rightCp.Count - $wantAdmin.Count; $i++) {
        $same = $true
        for ($k = 0; $k -lt $wantAdmin.Count; $k++) {
            if ($rightCp[$i + $k] -ne $wantAdmin[$k]) { $same = $false; break }
        }
        if ($same) { $hasAdmin = $true; break }
    }
    Assert-True $hasAdmin 'the status bar says the app holds administrator rights'

    Write-Host "`n--- pick a running service and stop it ---"
    $running = Get-RunningServiceName 40
    Write-Host "  running services available: $($running.Count)"
    if ($running.Count -eq 0) {
        Assert-True $false 'at least one running service is available as a target'
        throw 'no running service to test with'
    }

    # Not every running service can be stopped (it may have dependents, or the
    # SCM may refuse), so candidates are tried in turn until one actually
    # stops. Whatever is stopped is restarted in the finally block below.
    foreach ($cand in $running) {
        try {
            Select-RowByName $win $cand | Out-Null
        } catch {
            continue
        }
        Start-Sleep -Seconds 2
        Expand-Panel $win 'DetailExpander' | Out-Null

        $stopBtn = Get-ById $win 'StopButton'
        $startBtn = Get-ById $win 'StartButton'
        Write-Host ("  {0}: StopButton.IsEnabled = {1}, StartButton.IsEnabled = {2}" -f
            $cand, $stopBtn.Current.IsEnabled, $startBtn.Current.IsEnabled)
        if (-not $stopBtn.Current.IsEnabled) { continue }
        Assert-True (-not $startBtn.Current.IsEnabled) 'start is disabled while the service runs'

        Invoke-El $stopBtn 'StopButton'
        Start-Sleep -Seconds 8

        $svc = Get-Service -Name $cand -ErrorAction SilentlyContinue
        $status = if ($svc) { $svc.Status } else { 'Gone' }
        Write-Host "  status after stop: $status"
        if ($status -ne 'Running') {
            $stopped = $cand
            break
        }
        Write-Host "  (it did not stop -- trying the next candidate)"
    }

    Assert-True ($null -ne $stopped) 'a running service could be stopped through the UI'
    if ($null -eq $stopped) { throw 'no service could be stopped' }

    Write-Host "`n--- the write went through with no elevation prompt ---"
    $dlg = Get-Dialog $win 6
    if ($null -eq $dlg) {
        Assert-True $true 'no dialog appeared: the operation went straight through'
    } else {
        Write-Host ("  dialog title codepoints : " + (Show-Codepoints $dlg.Current.Name))
        $btns = Get-DialogButtons $dlg
        for ($i = 0; $i -lt $btns.Count; $i++) {
            Write-Host ("    [{0}] codepoints={1}" -f $i, (Show-Codepoints $btns[$i]))
        }
        Assert-True $false 'no elevation dialog should appear for a successful operation'
    }

    Write-Host "`n--- memory ---"
    $p.Refresh()
    Write-Host ("  priv={0} MB  ws={1} MB  handles={2}" -f `
        [Math]::Round($p.PrivateMemorySize64 / 1MB, 1),
        [Math]::Round($p.WorkingSet64 / 1MB, 1), $p.HandleCount)
} finally {
    if ($stopped) {
        Write-Host "`n--- restoring $stopped ---"
        & sc.exe start $stopped | Out-Null
        Start-Sleep -Seconds 4
        $svc = Get-Service -Name $stopped -ErrorAction SilentlyContinue
        $status = if ($svc) { $svc.Status } else { 'Gone' }
        Write-Host "  $stopped is now: $status"
        if ($status -ne 'Running') {
            Write-Warning ("Could not restart {0} (status {1}). It has to be" -f $stopped, $status)
            Write-Warning "started by hand -- the acceptance run changed machine state."
        }
    }
    Stop-App 'LingwuToolbox'
}

Write-Summary 'service admin'
