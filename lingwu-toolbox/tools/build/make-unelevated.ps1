# make-unelevated.ps1 -- build the asInvoker verification copy of the toolbox.
#
# WHY THIS EXISTS
#   The toolbox declares requireAdministrator, so the shipped binary always
#   starts through a UAC prompt. That is correct for the product, but it makes
#   the acceptance suite unusable wherever nobody can answer the prompt --
#   CI, a locked-down desktop, an agent-driven shell.
#
#   UI Automation cannot see across the integrity-level boundary AT ALL: the
#   identical query against the identical window returned 451 descendants from
#   an elevated client and 4 from a non-elevated one. Those 4 are the window
#   frame -- no AutomationId resolves, no pattern can be invoked, and every
#   script would "pass" by finding nothing at all.
#
#   So the suite needs a binary that runs at the SAME integrity level as the
#   script driving it. This script produces that binary: the same exe, with
#   the embedded manifest patched from requireAdministrator to asInvoker.
#
#   The copy has to live in its OWN directory. The app is a self-contained
#   deployment: the Windows App SDK framework DLLs are loaded from beside the
#   exe, so a lone exe would not even start.
#
# USAGE
#   pwsh -NoProfile -File .\tools\build\make-unelevated.ps1
#   pwsh -NoProfile -File .\tools\build\make-unelevated.ps1 -Clean
#
# THEN
#   pwsh -NoProfile -File .\tools\build\run-all.ps1 -Unelevated
#
# WHAT IT DOES, STEP BY STEP
#   1. wipe build\unelevated (a stale copy is worse than none -- build.bat
#      clears dist\Release on every run, so the copy must be rebuilt too),
#   2. copy the whole dist\Release tree (exe + framework payload),
#   3. extract the embedded manifest with mt.exe,
#   4. rewrite requireAdministrator -> asInvoker,
#   5. put the manifest back,
#   6. read it back and assert the level really changed.
#
# WHY mt.exe AND NOT A TEXT EDIT
#   The manifest lives in the PE resource section, not in the file as text.
#   mt.exe is the tool that ships with the Windows SDK for exactly this.
#
# This file is pure ASCII (repo rule for .ps1).

[CmdletBinding()]
param(
    # Remove the copy and exit. Useful when the suite is going to run
    # elevated anyway and the verification copy would only be dead weight.
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$script:Pass = 0
$script:Fail = 0

function Pass([string]$What) {
    $script:Pass++
    Write-Host ("[PASS] " + $What)
}

function Fail([string]$What) {
    $script:Fail++
    Write-Host ("[FAIL] " + $What)
}

# This file lives in tools/build/, which is two levels below the project root
# -- the same depth as tools/monitor/ and tools/service/.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)

$src = Join-Path $root 'dist\Release'
$dst = Join-Path $root 'build\unelevated'
$exeName = 'LingwuToolbox.exe'

if ($Clean) {
    if (Test-Path $dst) {
        Remove-Item -LiteralPath $dst -Recurse -Force
        Write-Host ("removed " + $dst)
    }
    Write-Host 'RESULT: pass=0 fail=0'
    exit 0
}

if (-not (Test-Path (Join-Path $src $exeName))) {
    Fail ("source exe not found: " + (Join-Path $src $exeName) + " -- build it first (build.bat)")
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}

# ---- 1. locate mt.exe -------------------------------------------------------
#
# The SDK keeps one copy per version under bin\<version>\<arch>. Pick the
# highest version that has an x64 mt.exe -- that is the one whose resource
# format matches the toolchain that built the exe.
$kitBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$mt = $null
if (Test-Path $kitBin) {
    $mt = Get-ChildItem -Path $kitBin -Filter 'mt.exe' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.DirectoryName -like '*\x64' } |
        Sort-Object -Property @{ Expression = { [version]$_.Directory.Parent.Name } } -Descending |
        Select-Object -First 1
}
if (-not $mt) {
    Fail ('mt.exe not found under ' + $kitBin + ' -- install the Windows SDK (build tools)')
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}
Write-Host ('mt.exe = ' + $mt.FullName)

# ---- 2. wipe + copy ---------------------------------------------------------
#
# Always rebuild. A copy left over from an older build would be driven by the
# acceptance scripts and silently test the wrong binary.
#
# Stop anything still running FROM THE COPY first. A live process keeps the
# framework DLLs mapped, and Windows refuses to delete a mapped file -- which
# is exactly the state an interrupted acceptance round leaves behind. Only
# processes whose path is inside the destination are touched: a build helper
# must never kill the user's own installed instance.
$running = @()
Get-Process -Name 'LingwuToolbox' -ErrorAction SilentlyContinue | ForEach-Object {
    $p = $null
    try { $p = $_.Path } catch { $p = $null }
    if ($p -and $p.StartsWith($dst, [StringComparison]::OrdinalIgnoreCase)) {
        $running += $_
    }
}
foreach ($p in $running) {
    try {
        Stop-Process -Id $p.Id -Force -ErrorAction Stop
        Write-Host ('stopped leftover pid ' + $p.Id + ' from the verification copy')
    } catch {
        Write-Host ('could not stop pid ' + $p.Id + ': ' + $_.Exception.Message)
    }
}
if ($running.Count -gt 0) { Start-Sleep -Milliseconds 1200 }

# Removal can still lose a race with the dying process, so retry briefly
# rather than failing the whole build on a timing window.
$removed = $false
for ($i = 0; $i -lt 5 -and -not $removed; $i++) {
    if (-not (Test-Path $dst)) { $removed = $true; break }
    try {
        Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction Stop
        $removed = $true
    } catch {
        Start-Sleep -Milliseconds 800
    }
}
if (-not $removed) {
    Fail ('could not remove the old copy at ' + $dst +
          ' -- a process is still holding it open')
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}

New-Item -ItemType Directory -Path $dst -Force | Out-Null
Copy-Item -Path (Join-Path $src '*') -Destination $dst -Recurse -Force

$dstExe = Join-Path $dst $exeName
if (-not (Test-Path $dstExe)) {
    Fail ('copy failed: ' + $dstExe + ' is missing')
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}
Pass 'copied dist\Release -> build\unelevated'

# The self-contained deployment needs its framework payload beside the exe.
# A missing DLL shows up as a silent failure to launch, so assert the payload
# is actually there rather than trusting the copy to have been complete.
$frameworkDll = Join-Path $dst 'Microsoft.UI.Xaml.Controls.dll'
if (Test-Path $frameworkDll) {
    Pass 'framework payload copied beside the exe'
} else {
    Fail ('framework payload missing: ' + $frameworkDll +
          ' -- the copy cannot launch without it')
}

# ---- 3..5. patch the embedded manifest --------------------------------------
#
# mt.exe argument spelling, verified against this SDK:
#   extract : -nologo "-inputresource:<exe>;#1" "-out:<xml>"
#   embed   : -nologo -manifest "<xml>" "-outputresource:<exe>;#1"
# Both the input and the output forms take a COLON here. The resource id ";#1"
# has to stay inside one quoted argument -- an unquoted ";" would be read as a
# statement separator by the shell and truncate the command.
$manifestXml = Join-Path $dst 'app.manifest'
$resId = ';#1'

& $mt.FullName -nologo ("-inputresource:" + $dstExe + $resId) ("-out:" + $manifestXml) | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail ('mt.exe extract failed (exit ' + $LASTEXITCODE + ')')
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}
if (-not (Test-Path $manifestXml)) {
    Fail 'mt.exe extract produced no manifest file'
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}
Pass 'extracted the embedded manifest'

$xml = Get-Content -LiteralPath $manifestXml -Raw
if ($xml -notmatch 'requireAdministrator') {
    Fail 'the manifest does not contain requireAdministrator -- nothing to patch'
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}
$xml = $xml -replace 'level="requireAdministrator"', 'level="asInvoker"'
# UTF-8 without BOM: mt.exe reads it back, and a BOM would end up inside the
# XML prolog.
[IO.File]::WriteAllText($manifestXml, $xml, (New-Object Text.UTF8Encoding($false)))
Pass 'patched requireAdministrator -> asInvoker'

& $mt.FullName -nologo -manifest $manifestXml ("-outputresource:" + $dstExe + $resId) | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail ('mt.exe embed failed (exit ' + $LASTEXITCODE + ')')
    Write-Host 'RESULT: pass=0 fail=1'
    exit 1
}
Pass 'wrote the patched manifest back into the exe'

# ---- 6. read it back --------------------------------------------------------
#
# The patch has to be verified in the BINARY, not in the intermediate xml:
# the embed step is the one that can silently do nothing.
$verifyXml = Join-Path $dst 'app.verify.manifest'
& $mt.FullName -nologo ("-inputresource:" + $dstExe + $resId) ("-out:" + $verifyXml) | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $verifyXml)) {
    Fail 'could not read the manifest back out of the exe'
} else {
    $v = Get-Content -LiteralPath $verifyXml -Raw
    if ($v -match 'level="asInvoker"' -and $v -notmatch 'requireAdministrator') {
        Pass 'the exe now carries level="asInvoker"'
    } else {
        Fail 'the exe still asks for administrator rights'
    }
    Remove-Item -LiteralPath $verifyXml -Force -ErrorAction SilentlyContinue
}

# The intermediate manifest is a build artefact, not something to ship.
Remove-Item -LiteralPath $manifestXml -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host ('copy: ' + $dstExe)
Write-Host ('run:  pwsh -NoProfile -File .\tools\build\run-all.ps1 -Unelevated')
Write-Host ''
Write-Host ('RESULT: pass=' + $script:Pass + ' fail=' + $script:Fail)
if ($script:Fail -gt 0) { exit 1 }
exit 0
