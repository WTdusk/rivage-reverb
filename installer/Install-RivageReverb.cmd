@echo off
setlocal
rem Use a dedicated temporary folder for systems where Inno Setup cannot
rem create its protected extraction directory in the default TEMP folder.
set "INSTALLER="
for /f "delims=" %%f in ('dir /b /a-d /o-d "%~dp0RivageReverb-*-win64.exe" 2^>nul') do if not defined INSTALLER set "INSTALLER=%~dp0%%f"
if not defined INSTALLER (
    echo Installer not found. Keep this file next to RivageReverb-*-win64.exe.
    pause
    exit /b 1
)
set "TEMP=%LOCALAPPDATA%\RivageReverb\InstallerTemp"
set "TMP=%TEMP%"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%TEMP%" (
    echo Cannot create temporary folder: "%TEMP%"
    pause
    exit /b 1
)
start "" /wait "%INSTALLER%" %*
exit /b %ERRORLEVEL%
