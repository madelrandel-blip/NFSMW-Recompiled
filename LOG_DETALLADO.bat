@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ===========================================================================
rem  Launches the game with maximum-detail logging, to hunt the hang.
rem
rem  WHY IT IS NEEDED
rem  With the normal log, the session that hung left NOTHING: zero errors, and
rem  total silence from the moment it got stuck. That rules out an exception or
rem  an unregistered function -those show up-. What remains is that the game
rem  code stayed waiting for something that never arrives, or spinning.
rem
rem  At debug level the kernel calls are also logged: waits on events and
rem  semaphores, file reads, thread creation and exit. If the game is blocked
rem  on a wait, the LAST LINES before the silence say what it is waiting on.
rem
rem  WHY IT COULD NOT BE DONE BEFORE
rem  Because the "Too few processor cores" warning came out a thousand times
rem  per second and ate the whole log: 105 MB in a quarter of an hour, and the
rem  interesting part rotated out of the file before one had time to read it.
rem  It is fixed now, so the detail fits.
rem
rem  WHAT log_noisy IS, AND WHY IT WORKS NOW
rem  Many internal SDK messages -among them the WHOLE XMA audio decoder
rem  lifecycle- are behind REXLOG_NOISY_DEBUG, which is not compiled out: it is
rem  turned on with the log_noisy cvar. And the XMA is precisely where the game
rem  hung, spinning between XMAGetOutputBufferWriteOffset and
rem  XMAGetOutputBufferReadOffset waiting for data that never arrives. Without
rem  this you cannot see a single line of what that decoder does.
rem
rem  THE LOG IS GOING TO BE VERY BIG. That is normal. It rotates by itself, and
rem  the most recent part -which is what matters- always stays in
rem  logs\detallado.log.
rem
rem  HOW TO USE IT
rem    1. Double-click.
rem    2. Reproduce the failure: finish the prologue, leave the garage, wait
rem       for the audio to die, and try to return to the menu.
rem    3. When it hangs, WAIT A FEW SECONDS before closing. If there is
rem       anything logged with delay, give it time.
rem    4. Close it and report. The file is logs\detallado.log
rem ===========================================================================

set "EXE="
rem nfsmw.exe FIRST: since the launcher takes the name NFS_Most_Wanted.exe,
rem the game in build\ is called that. The old name is still looked at behind
rem it, for folders assembled before the change.
if exist "%~dp0build\nfsmw.exe" set "EXE=%~dp0build\nfsmw.exe"
if not defined EXE if exist "%~dp0build\NFS_Most_Wanted.exe" set "EXE=%~dp0build\NFS_Most_Wanted.exe"
if not defined EXE if exist "%~dp0app\out\build\win-amd64-release\nfsmw.exe" set "EXE=%~dp0app\out\build\win-amd64-release\nfsmw.exe"

if not defined EXE (
    echo [ERROR] Cannot find the executable.
    echo.
    pause
    exit /b 1
)

set "ISO="
for %%d in ("%EXE%") do set "DIREXE=%%~dpd"
for %%f in ("!DIREXE!*.iso") do if not defined ISO set "ISO=%%~ff"
if not defined ISO for %%f in ("%~dp0build\*.iso") do if not defined ISO set "ISO=%%~ff"
if not defined ISO for %%f in ("%~dp0*.iso") do if not defined ISO set "ISO=%%~ff"
if not defined ISO for %%f in ("%~dp0assets\*.iso") do if not defined ISO set "ISO=%%~ff"

if not defined ISO (
    echo [ERROR] Cannot find any .iso.
    echo.
    pause
    exit /b 1
)

if not exist "logs" mkdir "logs"

echo ============================================
echo   Detailed log - hunt the hang
echo ============================================
echo.
echo   Executable: %EXE%
echo   ISO       : %ISO%
echo   Log       : %~dp0logs\detallado.log
echo.
echo WHAT TO DO
echo   1. Finish the prologue and leave the garage with the car.
echo   2. Wait for the audio to die.
echo   3. Try to return to the menu.
echo   4. When it hangs, WAIT A FEW SECONDS before closing.
echo.
echo The game is going to run MUCH SLOWER: everything is logged, including
echo the internal detail of the audio decoder, which is where it hangs.
echo That does not matter for what we are after, we only need to reach the
echo failure.
echo.
echo And when it hangs, HOLD ON 30 SECONDS before closing: two or three
echo watchdog snapshots inside the hang are needed.
echo.
pause
echo.
echo Launching...
echo.

"%EXE%" --game_data_root="%ISO%" --log_level=debug --log_noisy=true --log_file="%~dp0logs\detallado.log" --fullscreen=false --vsync=false

echo.
echo ============================================
echo   Finished
echo ============================================
echo.
echo The log is at:
echo   %~dp0logs\detallado.log
echo.
echo If there are files detallado.1.log, detallado.2.log and so on, they are
echo the previous chunks. The one that matters is plain detallado.log: it
echo always has the most recent part, which is exactly the moment of the
echo hang.
echo.
pause
