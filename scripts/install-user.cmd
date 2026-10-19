@echo off
rem Installs Rivage Reverb into the per-user VST3 folder, which needs no
rem administrator rights and cannot touch any other plug-in installation.
rem
rem   %LOCALAPPDATA%\Programs\Common\VST3\WTdusk\Rivage Reverb.vst3
rem
rem Undo: delete the "WTdusk" folder named below.

setlocal enableextensions

set "SRC=%~dp0..\build\Source\RivageReverb_artefacts\Release\VST3\Rivage Reverb.vst3"
set "DEST=%LOCALAPPDATA%\Programs\Common\VST3\WTdusk"
set "BUNDLE=%DEST%\Rivage Reverb.vst3"

if not exist "%SRC%\Contents\x86_64-win\Rivage Reverb.vst3" (
    echo ERROR: build the plugin first:
    echo        scripts\build.cmd RivageReverb_VST3 build Release
    exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%" 2>nul

if exist "%BUNDLE%" rmdir /s /q "%BUNDLE%"

xcopy "%SRC%" "%BUNDLE%\" /e /i /y /q >nul
if errorlevel 1 (
    echo ERROR: copy failed
    exit /b 1
)

echo Installed: "%BUNDLE%"
exit /b 0
