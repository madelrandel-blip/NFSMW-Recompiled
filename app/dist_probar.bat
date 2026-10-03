@echo off
setlocal
cd /d "%~dp0"

rem ===========================================================================
rem  STARTUP TEST - for anyone who sees the game fail to start.
rem
rem  Goes in the same folder as the game. Double-click and done.
rem
rem  WHAT IT DOES
rem  Launches the game 20 times: 5 for each of 4 different thread scheduling
rem  configurations. Each attempt starts with an EMPTY shader cache, which is
rem  what exposes the failure.
rem
rem  Windows will open and close on their own. That is normal. It takes about
rem  8 minutes.
rem
rem  At the end it prints a table. THAT TABLE IS WHAT YOU NEED TO SEND.
rem ===========================================================================

if not exist "%~dp0matriz.ps1" (
    echo [ERROR] matriz.ps1 is missing from this folder.
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

set "HAYISO="
for %%f in ("%~dp0*.iso") do set "HAYISO=1"
if not defined HAYISO (
    echo [ERROR] There is no .iso in this folder.
    echo         Copy your game ISO here before testing.
    echo.
    pause
    exit /b 1
)

echo ============================================
echo   Startup test
echo ============================================
echo.
echo I am going to launch the game 20 times in a row, with different options,
echo to find out which of them makes it start.
echo.
echo Windows will open and close on their own. That is normal, do not touch
echo anything. It takes about 8 minutes.
echo.
echo When it finishes a TABLE comes out. That table is what needs to be sent.
echo.
pause
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0matriz.ps1"

echo.
echo ============================================
echo   Send the table above
echo ============================================
echo.
echo If you also want to send the details, they are in the folder:
echo   %~dp0matriz
echo.
pause
