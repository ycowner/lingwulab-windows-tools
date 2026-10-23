# install-winui-workload.ps1
#
# Installs the Visual Studio component needed to build LingwuToolbox (C++/WinRT + WinUI 3).
#
# WHY THIS IS NEEDED
#   The native C++ WinUI 3 XAML compiler reads the MSBuild item @(WinMDReferenceToCompile).
#   That item is only ever READ by every Windows App SDK NuGet package - never written.
#   It is injected by the Visual Studio "C++ WinUI app development tools" component.
#   This machine has a bare BuildTools install without that component, so
#   MarkupCompilePass2 fails with:
#       WMC1007: Cannot resolve metadata for WinUI types
#
# HOW TO RUN
#   Open PowerShell as Administrator, then:
#       powershell -ExecutionPolicy Bypass -File .\tools\install-winui-workload.ps1
#
# ENCODING NOTE
#   This file is deliberately pure ASCII. Windows PowerShell 5.1 reads .ps1 files
#   WITHOUT a BOM using the system ANSI code page (GBK on zh-CN). UTF-8 Chinese
#   comments get decoded into mojibake that can break quote/brace parsing and
#   produce a cascade of bogus "missing terminator" errors. Same class of trap as
#   the project's existing rule that build.bat must stay ASCII + CRLF.

$ErrorActionPreference = 'Stop'

$vsInstallPath = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
$vsInstaller   = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'

# Official component id for "C++ WinUI app development tools" (cross-checked).
$componentId = 'Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Write-Host 'Administrator rights required - relaunching elevated...' -ForegroundColor Yellow
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath
    )
    exit
}

if (-not (Test-Path $vsInstaller)) {
    throw "VS Installer not found: $vsInstaller"
}
if (-not (Test-Path $vsInstallPath)) {
    throw "VS install path not found: $vsInstallPath (edit $PSCommandPath if it differs)"
}

Write-Host "Installing component: $componentId" -ForegroundColor Cyan
Write-Host "Instance:           $vsInstallPath" -ForegroundColor DarkGray
Write-Host 'This downloads several hundred MB. Do not close this window.' -ForegroundColor DarkGray

# TRAP 1: do NOT pass --wait. The "modify" subcommand does not define it and
#         rejects it with exit code 87 (ERROR_INVALID_PARAMETER).
#
# TRAP 2 (this one cost three failed runs): Start-Process -ArgumentList with an
#         ARRAY joins the elements with single spaces and adds NO quoting. The
#         install path contains spaces, so the installer silently parsed
#                     --installPath C:\Program
#         and died with exit code 1 plus a localized
#             "no installed product matches installPath: C:\Program".
#         The installer log makes it obvious: compare the "Raw Command line:"
#         line (correct) against the "Parsed command line options:" line (broken).
#         Fix: build ONE argument string and embed the quotes yourself.
$argLine = 'modify --installPath "{0}" --add {1} --quiet --norestart --nocache' -f $vsInstallPath, $componentId
Write-Host "Args: $argLine" -ForegroundColor DarkGray

$logDir  = $env:TEMP
$stamp   = Get-Date -Format 'yyyyMMddHHmmss'
$before  = @(Get-ChildItem $logDir -Filter 'dd_installer_*.log' -ErrorAction SilentlyContinue |
             ForEach-Object { $_.Name })

$proc = Start-Process -FilePath $vsInstaller -ArgumentList $argLine -PassThru -Wait

$exit = $proc.ExitCode
Write-Host ''
Write-Host "VS Installer exit code: $exit" -ForegroundColor $(if ($exit -eq 0) { 'Green' } else { 'Red' })

$new = @(Get-ChildItem $logDir -Filter 'dd_installer_*.log' -ErrorAction SilentlyContinue |
         Where-Object { $before -notcontains $_.Name } |
         Sort-Object LastWriteTime -Descending)
if ($new) {
    Write-Host "New installer log: $($new[0].FullName)" -ForegroundColor Yellow
    $raw    = Select-String -Path $new[0].FullName -Pattern 'Raw Command line:'       | Select-Object -Last 1
    $parsed = Select-String -Path $new[0].FullName -Pattern 'Parsed command line options:' | Select-Object -Last 1
    if ($raw)    { Write-Host "  $($raw.Line.Trim())"    -ForegroundColor DarkGray }
    if ($parsed) { Write-Host "  $($parsed.Line.Trim())" -ForegroundColor DarkGray }
}

if ($exit -ne 0) {
    $hint = 'See %TEMP%\dd_installer_*.log for the real reason.'
    if ($new) {
        $hint = "Installer log: $($new[0].FullName)"
        $notFound = Select-String -Path $new[0].FullName -Pattern 'installPath:' |
                    Select-Object -Last 1
        if ($notFound) { $hint += "`n  >> $($notFound.Line.Trim())" }
    }
    throw ("VS Installer exited with code {0}.`n{1}" -f $exit, $hint)
}

# Exit code 0 is necessary but not sufficient: prove the component actually
# landed in this instance's selection state before declaring success.
$stateFiles = Get-ChildItem 'C:\ProgramData\Microsoft\VisualStudio\Packages\_Instances' -Filter 'state.json' -Recurse -ErrorAction SilentlyContinue
$found = $false
foreach ($sf in $stateFiles) {
    if (Select-String -Path $sf.FullName -SimpleMatch $componentId -Quiet) { $found = $true; break }
}

if ($found) {
    Write-Host ''
    Write-Host 'OK: component is present in the VS instance selection state.' -ForegroundColor Green
    Write-Host 'Next: run build.bat and check whether MarkupCompilePass2 still reports WMC1007.' -ForegroundColor Green
} else {
    Write-Host ''
    Write-Host "WARNING: exit code was 0 but '$componentId' was not found in any instance state.json." -ForegroundColor Yellow
    Write-Host 'The XAML compiler workaround in LingwuToolbox.vcxproj must stay in place.' -ForegroundColor Yellow
    exit 2
}