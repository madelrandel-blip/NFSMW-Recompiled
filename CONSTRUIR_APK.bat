@echo off
:: ============================================================================
::  CONSTRUIR_APK.bat - Android launcher APK build (arm64-v8a)
::
::  Builds the standalone launcher APK: TitleActivity (ISO selection +
::  settings) -> GameActivity (NativeActivity) with the on-screen Xbox 360
::  gamepad, linking the recompiled USA game code in app\generated\default.
::
::  Requirements (already set up on this machine):
::    - Android SDK at D:\android-sdk  (local.properties points here)
::    - NDK 27.2.12479018, CMake 3.22.1, platforms;android-34
::    - JDK 17+ (system Java 21 works)
::
::  Output:
::    android\app\build\outputs\apk\release\app-release.apk
::
::  WARNING (Legal): the APK contains the game's own translated code
::  (libnfsmw.so). Never distribute it. Personal use only.
::
::  Usage:  CONSTRUIR_APK.bat [clean] [debug]
:: ============================================================================
setlocal
cd /d "%~dp0android"

set GRADLE_TASK=assembleRelease
:parse
if "%~1"=="" goto run
if /i "%~1"=="debug" set GRADLE_TASK=assembleDebug
if /i "%~1"=="clean" set DO_CLEAN=1
shift
goto parse

:run
if defined DO_CLEAN call gradlew.bat clean --no-daemon
call gradlew.bat %GRADLE_TASK% --no-daemon
if errorlevel 1 exit /b 1

echo.
echo APK: %cd%\app\build\outputs\apk\release\app-release.apk
if "%GRADLE_TASK%"=="assembleDebug" echo APK: %cd%\app\build\outputs\apk\debug\app-debug.apk
endlocal
