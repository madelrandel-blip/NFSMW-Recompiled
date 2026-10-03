@echo off
title NFSMW Recomp - Phase 1: extract the ISO
cd /d "%~dp0"

echo ============================================
echo   NFSMW Recomp - Phase 1
echo   Extract the ISO and read the XEX header
echo ============================================
echo.

rem --- locate Python --------------------------------------------------------
set "PY="
py -3 --version >nul 2>nul && set "PY=py -3"
if not defined PY (
    python --version >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [ERROR] Python was not found in the PATH.
    echo.
    echo Install it from https://www.python.org/downloads/
    echo IMPORTANT: check the "Add Python to PATH" box during installation.
    echo.
    pause
    exit /b 1
)

for /f "delims=" %%v in ('%PY% --version 2^>^&1') do set "PYVER=%%v"
echo Python found: %PYVER%
echo.

rem --- menu -----------------------------------------------------------------
echo What do you want to do?
echo.
echo   1. Only list the ISO contents          (quick, writes nothing)
echo   2. Extract EVERYTHING                  (~7 GB, several minutes)
echo   3. Extract only default.xex            (quick, a few MB)
echo   4. View the header of assets\default.xex
echo.
set /p OPCION="Choose 1-4 and press Enter: "
echo.

if "%OPCION%"=="1" goto listar
if "%OPCION%"=="2" goto extraer_todo
if "%OPCION%"=="3" goto extraer_xex
if "%OPCION%"=="4" goto info
echo Invalid option.
goto fin

:listar
%PY% tools\fase1_extraer.py --listar
goto fin

:extraer_todo
%PY% tools\fase1_extraer.py -o assets\game_root
if errorlevel 1 goto fin
call :copiar_xex
goto fin

:extraer_xex
%PY% tools\fase1_extraer.py -o assets\game_root --solo-xex
if errorlevel 1 goto fin
call :copiar_xex
goto fin

:copiar_xex
if exist "assets\game_root\default.xex" (
    copy /y "assets\game_root\default.xex" "assets\default.xex" >nul
    echo.
    echo default.xex copied to assets\default.xex
    if not exist "docs" mkdir "docs"
    %PY% tools\fase1_extraer.py "assets\default.xex" --info > "docs\xex_info.txt"
    echo Header saved to docs\xex_info.txt
    echo.
    type "docs\xex_info.txt"
) else (
    echo.
    echo [WARNING] No default.xex appeared at the root of the ISO.
    echo Run option 1 to see where the executable is.
)
exit /b 0

:info
if not exist "assets\default.xex" (
    echo assets\default.xex does not exist yet. Use option 2 or 3 first.
    goto fin
)
%PY% tools\fase1_extraer.py "assets\default.xex" --info
goto fin

:fin
echo.
echo ============================================
pause
