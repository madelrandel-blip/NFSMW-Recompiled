@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ===========================================================================
rem  COMPARE THE TWO VIDEO ENGINES
rem
rem  Goes in the same folder as the game. Double-click and done.
rem
rem  WHAT IT IS FOR
rem  We are chasing a visual glitch and need to know whether everyone gets it
rem  or only a specific graphics card. This .bat launches the game in two
rem  different ways so they can be compared.
rem
rem  WHAT TO LOOK FOR
rem  A HORIZONTAL BAND crossing the screen. Below it the road and the ground
rem  look brighter, in yellow; above it, more muted. The edge is straight and
rem  always stays at the same height on screen, it does not move with the
rem  scenery.
rem
rem  It is best seen driving on an open road in daylight.
rem
rem  WHAT TO ANSWER
rem  Only two things per option:
rem     1. whether that band is visible or not
rem     2. the fps shown by F3
rem
rem  That is all. With those four data points we know whether the glitch is in
rem  the game or in a specific graphics card.
rem ===========================================================================

rem  The game is nfsmw.exe: in build\ the name NFS_Most_Wanted.exe belongs to
rem  THE LAUNCHER, so that the game icon opens the options window. The old
rem  name is accepted as a fallback, for folders from before the change.
set "JUEGO=%~dp0nfsmw.exe"
if not exist "%JUEGO%" set "JUEGO=%~dp0NFS_Most_Wanted.exe"
if not exist "%JUEGO%" (
    echo [ERROR] I cannot find nfsmw.exe in this folder.
    echo         This file has to be next to the game.
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

:menu
cls
echo ============================================
echo   Compare the two video engines
echo ============================================
echo.
echo   1  Fast      (rtv)
echo   2  Accurate  (rov)
echo   3  Exit
echo.
echo   WHAT TO LOOK FOR IN EACH ONE
echo.
echo     A HORIZONTAL BAND crossing the screen. Below it the ground
echo     looks brighter and yellowish, above it more muted. The edge is
echo     straight and does NOT move with the scenery: it stays fixed at the
echo     same height on screen even when you turn the car.
echo.
echo     It is more noticeable on an open road in daylight.
echo.
echo     Note two things per option:  is the band visible (yes/no)  and  the fps.
echo     F3 in-game shows the fps.
echo.
echo   Both start windowed and without vsync, so the fps are real.
echo   Test both IN THE SAME SPOT on the map.
echo.
set "OPCION="
set /p "OPCION=Choose: "

if "%OPCION%"=="1" set "CAMINO=rtv" & goto :lanzar
if "%OPCION%"=="2" set "CAMINO=rov" & goto :lanzar
if "%OPCION%"=="3" goto :fin
goto menu

:lanzar
echo.
echo Launching in %CAMINO% mode. Close the game window when you are done.
echo.
"%JUEGO%" --render_target_path_d3d12=%CAMINO% --fullscreen=false --vsync=false

echo.
echo ============================================
echo   %CAMINO% mode
echo ============================================
set "FRANJA="
set /p "FRANJA=Was the horizontal band visible? (yes/no): "
set "FPS="
set /p "FPS=How many fps did F3 show?: "

if defined FRANJA (
    echo %DATE% %TIME%  mode=%CAMINO%  band=%FRANJA%  fps=%FPS%>>"%~dp0resultado_video.txt"
    echo.
    echo Noted.
)
echo.
pause
goto menu

:fin
echo.
if exist "%~dp0resultado_video.txt" (
    echo ============================================
    echo   THIS IS WHAT YOU NEED TO SEND
    echo ============================================
    echo.
    type "%~dp0resultado_video.txt"
    echo.
    echo It is saved in:
    echo   %~dp0resultado_video.txt
) else (
    echo You have not noted any result yet.
)
echo.
pause
exit /b
