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

"%VSPATH%\MSBuild\Current\Bin\amd64\MSBuild.exe" LingwuToolbox.vcxproj -restore -p:Configuration=Release -p:Platform=x64 -m -v:minimal -nologo %*

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo BUILD OK
endlocal