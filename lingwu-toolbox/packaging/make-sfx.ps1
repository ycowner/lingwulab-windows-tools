# make-sfx.ps1 -- package dist\Release into ONE self-extracting .exe
#
# Run with pwsh (not Windows PowerShell): this file is UTF-8 without BOM and
# contains no non-ASCII characters at all, so either host would work -- but
# pwsh is what the rest of this repo's tooling uses.
#
#   pwsh -NoProfile -File .\packaging\make-sfx.ps1
#   pwsh -NoProfile -File .\packaging\make-sfx.ps1 -Version 0.1.0
#
# Steps:
#   1. collect dist\Release into build\pack\payload.bin (custom LSFX v1 format,
#      uncompressed -- the files are already-compressed DLLs, so deflating them
#      buys ~0 and costs a dependency on a decompressor inside the launcher);
#   2. emit build\pack\sfx.rc (icon + version info + RCDATA payload);
#   3. hand off to build-sfx.bat for rc/cl/link.
#
# The build id embedded in the payload becomes the extraction folder name, so a
# rebuilt launcher never reuses the previous release's extracted files.

param(
    # Three-part release version (e.g. 0.0.1).
    #
    # The release CI passes this in, derived from the git tag
    # (lingwu-toolbox-vX.Y.Z), so a published artifact can never disagree with the
    # tag it was published under -- the tag is authoritative for releases, this
    # literal is authoritative for local builds. When the two diverge, the
    # release workflow prints a notice so the stale literal gets updated; it
    # deliberately does not fail the build, because a bookkeeping mismatch in
    # this file must never block a correctly-tagged release.
    [string]$Version = '0.0.4'
)

$ErrorActionPreference = 'Stop'

# Reject anything that would produce a broken file name / VERSIONINFO block.
# An unvalidated tag-derived string is the realistic way this goes wrong:
# a typo'd or annotated tag ("lingwu-toolbox-v1.0", "v1.0.0-rc1") would
# otherwise flow straight into the launcher name and rc.exe FILEVERSION.
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version '$Version' is not a three-part version (expected e.g. 0.0.1)."
}

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $here
$dist  = Join-Path $root 'dist\Release'
$stage = Join-Path $root 'build\pack'
$icon  = Join-Path $root 'resources\app.ico'

if (-not (Test-Path $dist)) { throw "dist\Release not found. Run build.bat first." }
if (-not (Test-Path $icon)) { throw "resources\app.ico not found." }

New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $icon (Join-Path $stage 'app.ico') -Force

$exe = Join-Path $dist 'LingwuToolbox.exe'
if (-not (Test-Path $exe)) { throw "dist\Release\LingwuToolbox.exe not found." }

# ---- version: single source of truth ------------------------------------------
# Every version surface (output file name, payload build id, the launcher's
# VERSIONINFO block) is derived from this one number, and build-sfx.bat is
# handed the payload build id instead of keeping its own copy. Keep it that way:
# a second copy of the version string is exactly the kind of thing that silently
# drifts -- the launcher would then ship a version different from its own name.
#
# The only in-repo literal lives in the $Version parameter default above. Do not
# reintroduce one here.
#
# Four-part form goes into VERSIONINFO (rc.exe wants a.b.c.d);
# three-part form goes into the file name and the extraction folder name.
$ver       = $Version
$verQuad   = $ver + '.0'
$verComma  = (($verQuad -split '\.') -join ',')

# ---- build id: first 8 hex of the app exe hash -------------------------------
$sha = [System.Security.Cryptography.SHA256]::Create()
$fs  = [System.IO.File]::OpenRead($exe)
$hash = ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }) -join ''
$fs.Dispose(); $sha.Dispose()
$buildId = $ver + '-' + $hash.Substring(0, 8)

# ---- collect files ------------------------------------------------------------
$files = @(Get-ChildItem $dist -Recurse -File | Sort-Object FullName)
Write-Host ("[1/3] Packing {0} files from dist\Release" -f $files.Count)

$payload = Join-Path $stage 'payload.bin'
# UTF-16LE without BOM. Encoding::Unicode is a plain static property, so it needs
# no constructor call -- [Type]::new() is not available on every host this repo
# runs on, and the difference between Encoding.Unicode and Encoding.Unicode(0,1)
# matters here: the 0-throwOnInvalid flag only changes encoder behaviour.
$enc = [System.Text.Encoding]::Unicode
$out = [System.IO.File]::Create($payload)
$w = [System.IO.BinaryWriter]::new($out, $enc)

try {
    $w.Write([byte[]]@(0x4C, 0x53, 0x46, 0x58))    # 'LSFX'
    $w.Write([uint32]1)                            # version

    $b = $enc.GetBytes($buildId)
    $w.Write([uint32]$b.Length); $w.Write($b)

    $w.Write([uint32]$files.Count)

    foreach ($f in $files) {
        $rel = $f.FullName.Substring($dist.Length + 1)
        $rb  = $enc.GetBytes($rel)
        $w.Write([uint32]$rb.Length); $w.Write($rb)
        $w.Write([uint64]$f.Length)

        $in = [System.IO.File]::OpenRead($f.FullName)
        try {
            $buf = New-Object byte[] (1MB)
            while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                $w.Write($buf, 0, $n)
            }
        } finally { $in.Dispose() }
    }
} finally {
    $w.Dispose(); $out.Dispose()
}

$payloadMB = [math]::Round((Get-Item $payload).Length / 1MB, 1)
Write-Host ("      payload.bin = {0} MB  (build id {1})" -f $payloadMB, $buildId)

# ---- emit sfx.rc --------------------------------------------------------------
$rc = @"
#include <windows.h>
#include <winver.h>

1 ICON    "app.ico"
1 RCDATA  "payload.bin"

VS_VERSION_INFO VERSIONINFO
FILEVERSION     $verComma
PRODUCTVERSION  $verComma
FILEFLAGSMASK   VS_FFI_FILEFLAGSMASK
FILEFLAGS       0x0L
FILEOS          VOS_NT_WINDOWS32
FILETYPE        VFT_APP
FILESUBTYPE     VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "080404b0"
        BEGIN
            VALUE "CompanyName",      "LingwuLab"
            VALUE "FileDescription",  "Lingwu Toolbox - portable launcher"
            VALUE "FileVersion",      "$verQuad"
            VALUE "InternalName",     "LingwuToolboxPortable"
            VALUE "OriginalFilename", "LingwuToolbox-$ver-win64.exe"
            VALUE "ProductName",      "Lingwu Toolbox"
            VALUE "ProductVersion",   "$verQuad"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0804, 1200
    END
END
"@
Set-Content -Path (Join-Path $stage 'sfx.rc') -Value $rc -Encoding ASCII
Write-Host "[2/3] Wrote sfx.rc"

# ---- compile / link -----------------------------------------------------------
# The payload build id doubles as the launcher's numeric version, so the file name
# comes from build-sfx.bat as a plain number -- never from a second literal there.
Write-Host "[3/3] Building launcher (version $ver)..."
cmd.exe /c "`"$here\build-sfx.bat`" `"$ver`""
if ($LASTEXITCODE -ne 0) { throw "build-sfx.bat failed (exit $LASTEXITCODE)" }

$outExe = Join-Path $root ("dist\package\LingwuToolbox-{0}-win64.exe" -f $ver)
Write-Host ("`n[OK] {0} = {1} MB" -f (Split-Path $outExe -Leaf), [math]::Round((Get-Item $outExe).Length / 1MB, 1))
