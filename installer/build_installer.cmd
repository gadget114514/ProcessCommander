@echo off
setlocal
rem Build the Inno Setup installer for ProcessCommander.
rem Requires Inno Setup 6 and an assembled payload in dist\ProcessCommander-v1.2.1-win64\.

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo [ERROR] ISCC.exe not found. Install Inno Setup 6 from https://jrsoftware.org/isdl.php
    exit /b 1
)

"%ISCC%" "%~dp0ProcessCommander.iss"
if errorlevel 1 (
    echo [ERROR] Inno Setup compilation failed.
    exit /b 1
)
echo [OK] Installer written to dist\ProcessCommander-v1.2.1-win64-setup.exe
exit /b 0
