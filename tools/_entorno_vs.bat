@echo off
rem ===========================================================================
rem  Prepares the build environment. It is invoked with CALL from the other
rem  .bat files, so it does NOT use setlocal: the variables must survive.
rem
rem  It leaves ready:
rem    - the Visual Studio x64 environment (INCLUDE, LIB, PATH)
rem    - VS NATIVE cmake / ninja / clang ahead of any MSYS2
rem    - the installed ReXGlue SDK bin, so that "rexglue" responds
rem    - ENTORNO_OK=1 if everything went well
rem ===========================================================================

set "ENTORNO_OK="
set "VSPATH="
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"

set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%PF%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
)

if defined VSPATH (
    if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH="
)

if not defined VSPATH (
    for %%R in ("%PF%\Microsoft Visual Studio" "%PF86%\Microsoft Visual Studio") do (
        if exist "%%~R\" (
            for /d %%Y in ("%%~R\*") do (
                if exist "%%~Y\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%~Y"
                for /d %%E in ("%%~Y\*") do (
                    if exist "%%~E\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%~E"
                )
            )
        )
    )
)

if not defined VSPATH (
    echo [ERROR] Visual Studio with C++ tools was not found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed.
    exit /b 1
)

rem System32 FIRST, before the VS ones. With MSYS2 in the PATH, Windows
rem utilities such as find.exe, sort.exe or where.exe get covered by their
rem Unix namesakes, which accept other parameters and fail in strange ways
rem ("find: /c/$Recycle.Bin: Permission denied"). It is prepended here so
rem that the VS prepends that come after end up ahead of this one.
set "PATH=%SystemRoot%\System32;%PATH%"

set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VSNINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "VSLLVM=%VSPATH%\VC\Tools\Llvm\x64\bin"
if exist "%VSNINJA%\ninja.exe" set "PATH=%VSNINJA%;%PATH%"
if exist "%VSCMAKE%\cmake.exe" set "PATH=%VSCMAKE%;%PATH%"
if exist "%VSLLVM%\clang.exe"  set "PATH=%VSLLVM%;%PATH%"

rem The installed SDK
set "SDK=%~dp0..\..\rexglue-sdk"
set "SDKBIN=%SDK%\out\install\win-amd64\bin"
if exist "%SDKBIN%" set "PATH=%SDKBIN%;%PATH%"

set "PY="
py -3 --version >nul 2>nul && set "PY=py -3"
if not defined PY (
    python --version >nul 2>nul && set "PY=python"
)

set "ENTORNO_OK=1"
exit /b 0
