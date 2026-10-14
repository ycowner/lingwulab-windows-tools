@echo off
setlocal

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if "%VSPATH%"=="" set "VSPATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"

if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERROR: vcvars64.bat not found under "%VSPATH%"
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

"%VSPATH%\MSBuild\Current\Bin\amd64\MSBuild.exe" LingwuSysmon.vcxproj -restore -p:Configuration=Release -p:Platform=x64 -m -v:minimal -nologo %*

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo BUILD OK
endlocal