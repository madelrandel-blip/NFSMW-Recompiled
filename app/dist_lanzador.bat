@echo off
setlocal
cd /d "%~dp0"

rem ===========================================================================
rem  LAUNCHER - the normal way to play.
rem
rem  Opens a window to choose resolution, fullscreen or windowed,
rem  the ISO, vsync and fps limit. It remembers what you choose for next time.
rem
rem  If you prefer to just play, you can also open nfsmw.exe
rem  directly: it will take the ISO it finds in this folder and the settings
rem  from nfsmw.toml.
rem ===========================================================================

if not exist "%~dp0lanzador.ps1" (
    echo [ERROR] lanzador.ps1 is missing from this folder.
    echo         It has to be next to the game
    echo.
    pause
    exit /b 1
)

rem  The game is nfsmw.exe: in build\ the name NFS_Most_Wanted.exe belongs to
rem  THE LAUNCHER, so that the game icon opens the options window. The old
rem  name is accepted as a fallback, for folders from before the change.
set "JUEGO=%~dp0nfsmw.exe"
if not exist "%JUEGO%" set "JUEGO=%~dp0NFS_Most_Wanted.exe"
if not exist "%JUEGO%" (
    echo [ERROR] I cannot find nfsmw.exe in this folder.
    echo.
    pause
    exit /b 1
)

where powershell >nul 2>&1
if errorlevel 1 (
    echo [ERROR] I cannot find Windows PowerShell.
    echo         Open nfsmw.exe directly.
    echo.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0lanzador.ps1"
