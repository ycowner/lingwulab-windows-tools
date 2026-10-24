@echo off
setlocal

rem Toolchain location. Resolution order:
rem   1. LINGWU_VSPATH env var -- CI sets this explicitly. The vswhere probe below
rem      picks the highest installed VS, and GitHub's runner image ships VS 2022
rem      alongside VS 2026 for compatibility, so auto-detection can land on the
rem      wrong one; this project needs v144 (VCProjectVersion 18.0).
rem   2. vswhere -latest.
rem   3. Hardcoded local path. Local convenience only -- this does not exist on
rem      a CI runner, which is why (1) exists.
set "VSPATH=%LINGWU_VSPATH%"
if "%VSPATH%"=="" set "VSPATH="

if "%VSPATH%"=="" (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

if "%VSPATH%"=="" set "VSPATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"

if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERROR: vcvars64.bat not found under "%VSPATH%"
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

rem Clean the output directory before building.
rem
rem MSBuild's incremental build never removes the output of a source file that
rem no longer exists, so a renamed or removed XAML page leaves its compiled .xbf
rem behind in dist\Release. packaging\make-sfx.ps1 copies that whole tree into
rem the single-file package, so the shipped file would carry a dead resource.
rem
rem It also made a local package differ from the published one: CI always
rem builds from a fresh checkout, so its payload holds only current files,
rem while a local build kept the leftovers. Measured: the local tree carried
rem src\ui\ServiceMainWindow.xbf, a page deleted during the merge, and the file
rem lists differed by exactly that one entry.
rem
rem Only dist\Release is removed -- dist\package holds the packaged output and
rem must survive. The path is anchored to this script's own directory (%~dp0)
rem so it can never delete a dist\Release belonging to some other project.
rem Set LINGWU_NO_CLEAN=1 to keep the tree for quick incremental iteration.
if not "%LINGWU_NO_CLEAN%"=="1" if exist "%~dp0dist\Release" rmdir /s /q "%~dp0dist\Release"

"%VSPATH%\MSBuild\Current\Bin\amd64\MSBuild.exe" LingwuToolbox.vcxproj -restore -p:Configuration=Release -p:Platform=x64 -m -v:minimal -nologo %*

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo BUILD OK
endlocal