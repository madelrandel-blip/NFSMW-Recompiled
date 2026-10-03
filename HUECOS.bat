@echo off
setlocal
cd /d "%~dp0"

echo ============================================
echo   NFSMW Recomp - Code gap map
echo ============================================
echo.
echo It compiles nothing: it only reads the generated C++ and works out where
echo there is code with no function assigned. That is where the indirect
echo calls that blow up the boot live.
echo.

set "PY="
py -3 --version >nul 2>nul && set "PY=py -3"
if not defined PY (
    python --version >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [ERROR] Python was not found.
    pause
    exit /b 1
)

if not exist "logs" mkdir "logs"

%PY% tools\huecos.py --min 8 --comprobar 0x82869668 0x8220C090 0x8285CB90 0x8215FEA8 0x826BE258 0x824EDEB0 0x8285CE80

echo.
echo ============================================
pause
