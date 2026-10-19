@echo off
rem Build driver: locates the Visual Studio toolchain, configures CMake+Ninja on
rem the first run, then builds the requested target.
rem
rem   build.cmd [target] [build-dir] [config] [reconfigure]
rem
rem Defaults: target=all, build-dir=build, config=Release

setlocal enabledelayedexpansion

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

set "BUILD_DIR=%~2"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build"

set "CONFIG=%~3"
if "%CONFIG%"=="" set "CONFIG=Release"

set "RECONFIGURE=%~4"
set "EXTRA=%RIVAGE_CMAKE_EXTRA%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found
    exit /b 1
)

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo ERROR: no Visual Studio C++ toolchain found
    exit /b 1
)

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%CMAKE%" (
    echo ERROR: cmake.exe not found under %VSPATH%
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)

pushd "%~dp0.."

if /i "%RECONFIGURE%"=="reconfigure" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

if not exist "%BUILD_DIR%\build.ninja" (
    echo == configuring %CONFIG% ==
    "%CMAKE%" -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_MAKE_PROGRAM="%NINJA%"
    if errorlevel 1 (
        popd
        exit /b 1
    )
)

if not "%EXTRA%"=="" (
    echo == reconfiguring with %EXTRA% ==
    "%CMAKE%" -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_MAKE_PROGRAM="%NINJA%" %EXTRA%
    if errorlevel 1 (
        popd
        exit /b 1
    )
)

echo == building %TARGET% ==
"%CMAKE%" --build "%BUILD_DIR%" --target %TARGET%
set "RESULT=%ERRORLEVEL%"

popd
exit /b %RESULT%
