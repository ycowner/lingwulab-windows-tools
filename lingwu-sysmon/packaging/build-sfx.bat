@echo off
rem ============================================================
rem  Build the single-file self-extracting launcher.
rem  Driven by make-sfx.ps1, which writes sfx.rc / payload.bin /
rem  app.ico into ..\build\pack. Sources stay here.
rem  Kept pure ASCII on purpose.
rem ============================================================
setlocal

set "HERE=%~dp0"
set "STAGE=%HERE%..\build\pack"
set "ROOT=%HERE%.."
set "OUT=%ROOT%\dist\package"

rem Version comes from make-sfx.ps1 as %1 -- it is the single source of truth
rem (see the "version: single source of truth" block there). Do NOT hardcode a
rem version here: it would silently drift from the VERSIONINFO in sfx.rc.
if "%~1"=="" (
    echo ERROR: usage: build-sfx.bat ^<version^>   e.g. build-sfx.bat 0.0.1
    exit /b 1
)
set "VER=%~1"
set "EXE=%OUT%\LingwuSysmon-%VER%-win64.exe"

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" set "VSPATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"

if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: vcvars64.bat not found under "%VSPATH%".
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: failed to initialize MSVC environment.
    exit /b 1
)

if not exist "%STAGE%\sfx.rc" (
    echo ERROR: "%STAGE%\sfx.rc" not found. Run make-sfx.ps1 first.
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

cd /d "%STAGE%"

echo [1/3] Compiling resource (embeds the ~60 MB payload)...
rc /nologo /fo sfx.res /i "%STAGE%" sfx.rc
if errorlevel 1 (
    echo ERROR: resource compile failed.
    exit /b 1
)

echo [2/3] Compiling launcher...
cl /nologo /c /W3 /WX /EHsc /O2 /MT /GS /utf-8 /std:c++17 /Fo:sfx_main.obj "%HERE%sfx_main.cpp"
if errorlevel 1 (
    echo ERROR: compile failed.
    exit /b 1
)

echo [3/3] Linking...
link /nologo /MACHINE:X64 /LTCG /OPT:REF /OPT:ICF ^
     /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup ^
     /OUT:"%EXE%" sfx_main.obj sfx.res user32.lib shell32.lib
if errorlevel 1 (
    echo ERROR: link failed.
    exit /b 1
)

for %%F in ("%EXE%") do echo.
echo [OK] version %VER% - %%~nxF = %%~zF bytes
exit /b 0
