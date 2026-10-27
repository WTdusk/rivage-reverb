@echo off
rem ---------------------------------------------------------------- package.cmd
rem Builds the VST3 and wraps it in the Inno Setup installer.
rem
rem   scripts\package.cmd                 build (incremental) + compile installer
rem   scripts\package.cmd nobuild         reuse the existing build tree
rem   scripts\package.cmd reconfigure     delete the build tree and rebuild
rem
rem Output: dist\RivageReverb-<version>-win64.exe
rem ----------------------------------------------------------------
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
pushd "%ROOT%" || exit /b 1
set "ROOT=%CD%"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=incremental"

rem ------------------------------------------------------------------ version
rem Source/CMakeLists.txt is the single source of truth for the version.
for /f "delims=" %%v in ('python -c "import re,io;t=io.open(r'Source/CMakeLists.txt',encoding='utf-8').read();m=re.search(r'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)',t);print(m.group(1) if m else '')"') do set "APPVER=%%v"
if "%APPVER%"=="" (
    echo [package] could not read the version from Source\CMakeLists.txt
    goto :fail
)
echo [package] version %APPVER%

rem -------------------------------------------------------------------- build
if /I not "%MODE%"=="nobuild" (
    set "RCARG="
    if /I "%MODE%"=="reconfigure" set "RCARG=reconfigure"
    call "%ROOT%\scripts\build.cmd" RivageReverb_VST3 build Release !RCARG!
    if errorlevel 1 goto :fail
)

set "BUNDLE=%ROOT%\build\Source\RivageReverb_artefacts\Release\VST3\Rivage Reverb.vst3"
if not exist "%BUNDLE%\Contents\x86_64-win\Rivage Reverb.vst3" (
    echo [package] bundle not found: "%BUNDLE%"
    goto :fail
)

rem ------------------------------------------------------------------ compile
set "ISCC=%ROOT%\tools\inno\app\ISCC.exe"
if exist "%ROOT%\tools\inno\6.7.3\tools\ISCC.exe" set "ISCC=%ROOT%\tools\inno\6.7.3\tools\ISCC.exe"
if defined RIVAGE_ISCC set "ISCC=%RIVAGE_ISCC%"
if not exist "%ISCC%" (
    echo [package] Inno Setup compiler missing: "%ISCC%"
    echo [package] set RIVAGE_ISCC to ISCC.exe or see BUILDING.md
    goto :fail
)

if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"

echo [package] compiler: "%ISCC%"
echo [package] compiling installer...
"%ISCC%" /Q "/DAppVersion=%APPVER%" "/DBundleDir=%BUNDLE%" "%ROOT%\installer\RivageReverb.iss" > "%ROOT%\dist\iscc.log" 2>&1
if errorlevel 1 (
    echo [package] compiler output:
    type "%ROOT%\dist\iscc.log"
    goto :fail
)

set "OUT=%ROOT%\dist\RivageReverb-%APPVER%-win64.exe"
if not exist "%OUT%" (
    echo [package] expected output missing: "%OUT%"
    goto :fail
)

copy /y "%ROOT%\installer\Install-RivageReverb.cmd" "%ROOT%\dist\Install-RivageReverb.cmd" >nul
if errorlevel 1 goto :fail

echo.
echo [package] done: "%OUT%"
python -c "import hashlib,os,sys;p=sys.argv[1];print('[package] size:',os.path.getsize(p),'bytes');print('[package] sha256:',hashlib.sha256(open(p,'rb').read()).hexdigest())" "%OUT%"
popd
exit /b 0

:fail
echo [package] FAILED
popd
exit /b 1
