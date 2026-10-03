@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1

rem =============================================================================
rem  Builds Lanzador.exe
rem
rem  Compiles tools\lanzador\Lanzador.cs with the C# compiler that ALREADY COMES
rem  with Windows. There is no need to install Visual Studio, nor the .NET SDK,
rem  nor anything: csc.exe is inside C:\Windows\Microsoft.NET\ since Windows 8.
rem
rem  The icon and the cover art stay INSIDE the exe. Once built, Lanzador.exe
rem  carries itself anywhere; it does not need the images next to it.
rem
rem  IN build\ THE LAUNCHER IS CALLED NFS_Most_Wanted.exe
rem  ==================================================
rem  And the game is renamed to nfsmw.exe, which is what it is called in the
rem  project tree. The only reason is that double-clicking the game icon brings
rem  up the options window, as in any game with a launcher.
rem
rem  The game STILL knows how to start by itself: nfsmw.exe on its own works
rem  the same as before -the ISO is looked for next to it and it sets
rem  gpu_plugin and mnk_mode by itself-, so that remains as a way out in case
rem  the launcher gives trouble.
rem
rem  It also does not change where the game stores its things: that folder comes
rem  from GetName() in the code, not from the file name.
rem
rem  The rename is done down below and is idempotent: if build\nfsmw.exe already
rem  exists, it was already done and only the launcher is refreshed.
rem
rem  It can be called from another .bat with  /silencioso  so that it does not
rem  pause.
rem
rem  WATCH OUT FOR "RC": vcvars64 uses it for the resource compiler, so in this
rem  project return codes always go in SALIDA.
rem =============================================================================

set "RAIZ=%~dp0"
if "%RAIZ:~-1%"=="\" set "RAIZ=%RAIZ:~0,-1%"
set "FUENTE=%RAIZ%\tools\lanzador"
set "SALIDA_EXE=%RAIZ%\Lanzador.exe"

set "SILENCIOSO="
if /i "%~1"=="/silencioso" set "SILENCIOSO=1"

echo.
echo  ======================================================================
echo   NFS Most Wanted Launcher - Recompilation
echo  ======================================================================
echo.

rem ---- What the ingredients are ----------------------------------------------
rem
rem  Only the code is required. The cover art and the icon are the game's
rem  artwork, Electronic Arts art, and that is why they are NOT in the
rem  repository.
rem
rem  The launcher starts perfectly fine without them: CargarRecurso returns null
rem  if the resource is missing and the side panel is drawn in black with the
rem  title. So here it warns and continues, instead of refusing to compile.
rem
rem  If you want to use your own, see docs\lanzador.md.
if not exist "%FUENTE%\Lanzador.cs" (
    echo  [ERROR] Missing %FUENTE%\Lanzador.cs
    echo.
    echo  It is the launcher code and without it there is nothing to compile.
    goto :fin_mal
)

set "ARG_ICONO="
set "ARG_PORTADA="
if exist "%FUENTE%\icono.ico" (
    set "ARG_ICONO=/win32icon:"%FUENTE%\icono.ico""
) else (
    echo  [warning] No icono.ico. The exe will come out with the generic icon.
)
if exist "%FUENTE%\portada.jpg" (
    set "ARG_PORTADA=/resource:"%FUENTE%\portada.jpg",portada.jpg"
) else (
    echo  [warning] No portada.jpg. The side panel will come out black.
)

rem ---- Find csc.exe ----------------------------------------------------------
rem
rem  Tried from newest to oldest. v4.0.30319 is on every modern Windows; v3.5
rem  and v2.0 are from Windows 7 and so old that they are not even tried,
rem  because WinForms from that era lacks things used here.
set "CSC="
for %%D in (Framework64 Framework) do (
    if not defined CSC (
        if exist "%WINDIR%\Microsoft.NET\%%D\v4.0.30319\csc.exe" (
            set "CSC=%WINDIR%\Microsoft.NET\%%D\v4.0.30319\csc.exe"
        )
    )
)

if not defined CSC (
    echo  [ERROR] Cannot find the Windows C# compiler.
    echo.
    echo  Looked in:
    echo     %WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
    echo     %WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
    echo.
    echo  That comes with .NET Framework 4, which Windows ships by default. If
    echo  it is not there, it is enabled in:
    echo     Control Panel ^> Programs ^> Turn Windows features on or off
    echo     ^> .NET Framework 4.x
    echo.
    echo  In the meantime you still have LANZADOR.bat, which does the same.
    goto :fin_mal
)

echo  Compiler:    %CSC%
echo  Source:      %FUENTE%\Lanzador.cs
echo  Output:      %SALIDA_EXE%
echo.

rem ---- Compile ---------------------------------------------------------------
rem
rem  /target:winexe  and not /target:exe, so that a black console window does
rem                  not appear behind the launcher.
rem  /win32icon      the icon the file explorer shows.
rem  /resource       puts the cover art INSIDE the exe. The name after the
rem                  comma is the one the code looks for, so it has to be
rem                  exactly "portada.jpg".
echo  Compiling...
"%CSC%" /nologo /target:winexe /optimize+ /platform:anycpu ^
    /out:"%SALIDA_EXE%" ^
    %ARG_ICONO% ^
    %ARG_PORTADA% ^
    /reference:System.dll ^
    /reference:System.Drawing.dll ^
    /reference:System.Windows.Forms.dll ^
    "%FUENTE%\Lanzador.cs"
set SALIDA=%ERRORLEVEL%

if not "%SALIDA%"=="0" (
    echo.
    echo  [ERROR] The compilation failed ^(code %SALIDA%^).
    echo.
    echo  The errors above carry a line number from Lanzador.cs. If they
    echo  mention odd characters or missing ';', it is almost certainly because
    echo  this Windows ships an older csc than expected.
    goto :fin_mal
)

if not exist "%SALIDA_EXE%" (
    echo.
    echo  [ERROR] The compiler said yes, but there is no Lanzador.exe.
    goto :fin_mal
)

rem ---- Put it in the distributable folder -------------------------------------
rem
rem  This is where the launcher takes the game's name. Two cases, told apart
rem  by whether build\nfsmw.exe already exists:
rem
rem    not yet       build\NFS_Most_Wanted.exe is THE GAME. It is renamed to
rem                  nfsmw.exe and the launcher takes its place.
rem    already done  build\nfsmw.exe exists, so NFS_Most_Wanted.exe is already a
rem                  launcher from a previous run. It is only refreshed.
rem
rem  This way it can be run as many times as needed without breaking anything,
rem  which is exactly what happens when DIST.bat calls it on each rebuild.
set "DESTINO=%RAIZ%\build"
if not exist "%DESTINO%" goto sin_build

if not exist "%DESTINO%\nfsmw.exe" (
    if exist "%DESTINO%\NFS_Most_Wanted.exe" (
        echo  Renaming the game to nfsmw.exe to leave the name to the launcher...
        move /Y "%DESTINO%\NFS_Most_Wanted.exe" "%DESTINO%\nfsmw.exe" >nul
        if errorlevel 1 (
            echo  [ERROR] Could not rename it. Do you have the game open?
            goto :fin_mal
        )
    ) else (
        echo  [warning] There is neither nfsmw.exe nor NFS_Most_Wanted.exe in build\.
        echo          Run DIST.bat to assemble the portable folder.
        goto sin_build
    )
)

copy /Y "%SALIDA_EXE%" "%DESTINO%\NFS_Most_Wanted.exe" >nul
if errorlevel 1 (
    echo  [warning] Could not copy it to build\. Is it open?
) else (
    set "PUESTO=1"
)

:sin_build
echo.
echo  ======================================================================
echo   DONE
echo  ======================================================================
echo.
echo   Lanzador.exe               in the project root
if defined PUESTO (
    echo   build\NFS_Most_Wanted.exe  the launcher, with the game icon
    echo   build\nfsmw.exe            the actual game
    echo.
    echo   Double-clicking NFS_Most_Wanted.exe opens the options window, and
    echo   from there you play. nfsmw.exe on its own still works too.
)
echo.
echo   The icon and the cover art go inside the exe, so it can be moved
echo   on its own, without taking anything along.
echo.
echo   The settings are the same as always ^(lanzador.json^), so what you
echo   already had configured stays. LANZADOR.bat is still there if you
echo   prefer it.
echo.
if not defined SILENCIOSO pause
exit /b 0

:fin_mal
echo.
if not defined SILENCIOSO pause
exit /b 1
