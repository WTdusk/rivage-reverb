@echo off
rem AddressSanitizer build: same optimisation level as Release (/O2) but with
rem every memory access instrumented, so an out-of-bounds write is reported at
rem the instruction that performs it instead of surfacing later as heap
rem corruption during teardown.
rem
rem   build-asan.cmd [target] [build-dir]
rem
rem Defaults: target=all, build-dir=build-asan

setlocal enabledelayedexpansion

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

set "BUILD_DIR=%~2"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build-asan"

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

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)

pushd "%~dp0.."

if not exist "%BUILD_DIR%\build.ninja" (
    echo == configuring asan build ==
    "%CMAKE%" -S . -B "%BUILD_DIR%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /fsanitize=address /Zi /wd4996" ^
        "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /fsanitize=address /Zi" ^
        "-DCMAKE_EXE_LINKER_FLAGS=/fsanitize=address" ^
        "-DCMAKE_SHARED_LINKER_FLAGS=/fsanitize=address" ^
        "-DCMAKE_MODULE_LINKER_FLAGS=/fsanitize=address"
    if errorlevel 1 (
        popd
        exit /b 1
    )
)

echo == building asan %TARGET% ==
"%CMAKE%" --build "%BUILD_DIR%" --target %TARGET%
set "RESULT=%ERRORLEVEL%"

popd
exit /b %RESULT%
