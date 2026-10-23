# common.ps1 -- shared bootstrap for the acceptance scripts. Dot-source it.
#
# It carries the three things every script needs and none of the things
# only some of them need:
#
#   1. WHERE THE APP IS      -- Resolve-ToolboxExe
#   2. WHETHER WE MAY RUN IT -- Test-Elevated / Require-Elevated
#   3. NOTHING ELSE.
#
# UI Automation lives in uia.ps1, which dot-sources this file. Scripts that
# do not need UI Automation (pure process sampling, screenshots) dot-source
# this file directly and stay clear of the UIAutomation assemblies.
#
# WHY THE ELEVATION PART IS NOT OPTIONAL
#
#   The toolbox declares requireAdministrator, so the app ALWAYS runs
#   elevated. UI Automation cannot see across an integrity-level boundary:
#   measured on this machine, the identical query against the identical
#   window returned 451 descendants from an elevated client and 4
#   descendants from a non-elevated one. Those 4 are the window frame -- no
#   AutomationId resolves, no pattern can be invoked, and every script would
#   "pass" by finding nothing at all.
#
#   So an acceptance script that drives the UI has to run at the same
#   integrity level as the app, i.e. elevated. Instead of making every
#   caller remember that, Require-Elevated re-launches the script elevated
#   and relays the child's output and exit code back to the parent.
#
# HOW THE OUTPUT RELAY WORKS
#
#   Start-Process -Verb RunAs cannot redirect stdout (ShellExecute has no
#   such facility -- PowerShell throws on the combination), and the env
#   block does not survive the ShellExecute hop either (measured: the child
#   sees an empty value for a variable the parent had just set).
#
#   So the parent writes a small wrapper script next to a JSON file holding
#   the child's arguments, and elevates THAT. The wrapper derives the log
#   path from its own file name, captures the child's output with the *>
#   redirection, and exits with the child's exit code. The parent prints the
#   captured output once the child has exited.
#
# ARGUMENT REPLAY
#
#   The parent replays the child's arguments from $PSBoundParameters, not
#   from $args: $args only holds the arguments that did NOT bind to a
#   declared parameter, so a script invoked as `check.ps1 -Seconds 120`
#   would silently lose -Seconds across the re-launch.
#
# Pure ASCII on purpose (repo rule for .ps1).

Set-StrictMode -Version Latest

# Name of the environment variable that carries the transcript path from the
# parent shell to the elevated child.
$script:ElevationLogVar = 'LINGWU_TOOLS_ELEV_LOG'

# Project root, captured HERE rather than looked up later.
#
# $PSScriptRoot is a script-scoped automatic variable, and dot-sourcing a
# file can rebind it in the caller's scope -- so a helper that resolves
# paths from $PSScriptRoot at call time can end up looking in the wrong
# directory depending on who dot-sourced whom. Capturing the value once, in
# the file that owns it, removes that whole class of surprise.
#
# tools/common/common.ps1 sits exactly two levels below the project root,
# the same as tools/monitor/*.ps1 and tools/service/*.ps1.
$script:ToolboxRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# The toolbox process name, in one place. Both modules live in it now.
$script:ToolboxProcess = 'LingwuToolbox'

# Resolve the exe the acceptance scripts drive.
#
# Order:
#   1. an explicit path (the caller passed -Exe),
#   2. $env:LINGWU_TOOLBOX_EXE (CI or one-off runs),
#   3. <project root>\dist\Release\LingwuToolbox.exe.
function Resolve-ToolboxExe([string]$Exe = '') {
    $candidate = $Exe
    if (-not $candidate) { $candidate = $env:LINGWU_TOOLBOX_EXE }
    if (-not $candidate) {
        $candidate = Join-Path $script:ToolboxRoot 'dist\Release\LingwuToolbox.exe'
    }
    if (-not (Test-Path $candidate)) {
        throw ("LingwuToolbox.exe not found at '{0}'. Build it first " +
               "(build.bat) or pass -Exe explicitly.") -f $candidate
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Test-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal -ArgumentList $identity
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Kill any running copy of the toolbox and wait for it to go away.
#
# This is not optional housekeeping: a leftover process holds the exe open,
# and the next build fails with LNK1104 "cannot open file" -- an error that
# points at the linker while the real cause is the previous test run.
function Stop-App([string]$ProcessName = '') {
    if (-not $ProcessName) { $ProcessName = $script:ToolboxProcess }
    Get-Process $ProcessName -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
}

function Start-App([string]$Exe) {
    if (-not (Test-Path $Exe)) { throw "Exe not found: $Exe" }
    $p = Start-Process $Exe -PassThru
    Write-Host "launched pid=$($p.Id)"
    return $p
}

# The wrapper the parent elevates. It is written to disk rather than passed
# as a command line, because the paths it has to carry (its own log, its own
# argument file) are only known at run time -- and it derives both from its
# own file name, so it needs no path baked into it at all.
#
# The child's output is captured with the *> redirection rather than with
# Start-Transcript: the transcript adds a header and a footer full of
# locale-specific noise to every single run, and it has to be started and
# stopped by the child, which means the child needs to know the path.
# With *> the wrapper owns the path and the child stays unaware of it.
$script:ElevationWrapper = @'
$__dir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$__base = [IO.Path]::GetFileNameWithoutExtension($MyInvocation.MyCommand.Path)
$__log  = Join-Path $__dir ($__base + '.log')
$__code = Join-Path $__dir ($__base + '.code')
$__j = Get-Content -Raw (Join-Path $__dir ($__base + '.json')) | ConvertFrom-Json
$__script = $__j.__script
$__p = @{}
foreach ($n in $__j.PSObject.Properties.Name) {
    if ($n -eq '__script') { continue }
    $__p[$n] = $__j.$n
}
$__rc = 0
try {
    & $__script @__p *> $__log
    $__rc = $global:LASTEXITCODE
} catch {
    ($_ | Out-String) | Add-Content -LiteralPath $__log
    $__rc = 1
}
if ($null -eq $__rc) { $__rc = 0 }
Set-Content -LiteralPath $__code -Value $__rc -Encoding UTF8
# Tidy up after the script, whether it succeeded or not: an acceptance run
# must not leave the toolbox running, and the next script expects to start
# from a clean slate.
Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
exit $__rc
'@

function Require-Elevated {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,
        [hashtable]$ScriptArgs = @{},
        [switch]$NoElevate,
        [int]$TimeoutSec = 3600
    )

    if (Test-Elevated) { return }

    if ($NoElevate) {
        throw ("This script must run elevated: -NoElevate was given but the " +
               "current shell is not an administrator.")
    }

    # Escape hatch for environments where elevation is not available at all
    # (a machine whose UAC prompt nobody can answer, a CI runner without an
    # interactive desktop). It is opt-in and loud, because the UI tests then
    # run against a build that does NOT have administrator rights -- which is
    # exactly the configuration the toolbox is not supposed to ship in.
    if ($env:LINGWU_TOOLBOX_UNELEVATED -eq '1') {
        Write-Warning ("LINGWU_TOOLBOX_UNELEVATED=1: running UNELEVATED. The " +
                       "UI can be inspected, but every write operation will " +
                       "be refused by the OS.")
        return
    }

    $dir = Join-Path ([IO.Path]::GetTempPath()) 'lingwu-tools'
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $base = [guid]::NewGuid().ToString('N')
    $wrapper = Join-Path $dir ($base + '.ps1')
    $json = Join-Path $dir ($base + '.json')
    $log = Join-Path $dir ($base + '.log')
    $codeFile = Join-Path $dir ($base + '.code')

    $payload = @{}
    if ($null -ne $ScriptArgs) {
        foreach ($k in $ScriptArgs.Keys) {
            $v = $ScriptArgs[$k]
            # A SwitchParameter is a struct, and ConvertTo-Json serialises it
            # as an OBJECT ({"IsPresent":true}), which then fails to bind back
            # to a [switch] parameter. Flatten it to a plain boolean first.
            if ($v -is [System.Management.Automation.SwitchParameter]) {
                $v = $v.IsPresent
            }
            $payload[$k] = $v
        }
    }
    $payload['__script'] = $ScriptPath
    ($payload | ConvertTo-Json -Depth 5 -Compress) |
        Set-Content -LiteralPath $json -Encoding UTF8
    Set-Content -LiteralPath $wrapper -Value $script:ElevationWrapper -Encoding UTF8

    $self = $null
    try { $self = (Get-Process -Id $PID).Path } catch { $self = $null }
    if (-not $self) { $self = 'pwsh.exe' }

    Write-Host ("Not elevated - re-launching elevated: {0}" -f $ScriptPath)
    # NOTE: deliberately no -Wait here. See the comment above the wrapper:
    # -Wait waits for the whole descendant tree, and an acceptance script
    # that leaves the toolbox running would keep the parent waiting for
    # ever. The exit code arrives through a file instead.
    $child = $null
    try {
        $child = Start-Process -Verb RunAs -FilePath $self `
            -ArgumentList @('-NoProfile', '-File', ('"' + $wrapper + '"')) `
            -PassThru
    } catch {
        Remove-Item -LiteralPath $wrapper, $json -Force -ErrorAction SilentlyContinue
        throw ("Could not launch an elevated copy of this script: {0}" -f
               $_.Exception.Message)
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $code = $null
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $codeFile) {
            try { $code = (Get-Content -LiteralPath $codeFile -Raw).Trim() } catch { $code = $null }
            if ($code) { break }
        }
        Start-Sleep -Milliseconds 300
    }

    if ($null -eq $code) {
        Write-Warning ("The elevated child did not report an exit code within " +
                       "{0}s; treating the run as failed." -f $TimeoutSec)
        if ($child) { $child | Stop-Process -Force -ErrorAction SilentlyContinue }
        $code = '1'
    }

    if (Test-Path $log) {
        Get-Content -LiteralPath $log | ForEach-Object { Write-Host $_ }
    } else {
        Write-Warning ("The elevated child produced no output at '{0}'." -f $log)
    }
    Remove-Item -LiteralPath $wrapper, $json, $log, $codeFile `
        -Force -ErrorAction SilentlyContinue

    $rc = 0
    if ($code -match '^\d+$') { $rc = [int]$code }
    exit $rc
}
