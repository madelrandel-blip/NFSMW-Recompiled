@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ===========================================================================
rem  Assembles the portable folder  build\  and checks that it is self-contained.
rem
rem  NOW IT STARTS WITH THE PATCHES, AND THAT IS NOT DECORATION
rem
rem  The game patches do not live in the app code: they live in the SDK,
rem  applied by the tools\ scripts over its sources. That means they end up
rem  inside rexruntime.dll, not the .exe.
rem
rem  This used to build only the app and copy whatever was there. If the SDK
rem  was unpatched -freshly cloned, manually reverted, or from another branch-,
rem  build\ came out with a rexruntime.dll WITHOUT the patches and looking
rem  just like a good one. That failure is not visible until the game hangs at
rem  someone else's place, which is the worst place to find out.
rem
rem  So now the patches are applied -they are idempotent: if they are already
rem  there, they say so and touch nothing- and the SDK is rebuilt before
rem  assembling the folder. If everything was up to date, that phase takes
rem  seconds.
rem
rem  WHICH PATCHES GO IN
rem    tools\parche_desatasco.py    THE AUDIO / HANG ONE. When the game's
rem                                 audio thread has been spinning for more
rem                                 than a quarter of a second on a voice
rem                                 that ran out of data, it is given the
rem                                 "buffer finished" signal that its own code
rem                                 knows how to read, and it moves on.
rem    tools\parche_diagnostico.py  the "Too few processor cores" warning came
rem                                 out a thousand times per second and drowned
rem                                 the CPU on machines with few cores. Now it
rem                                 comes out once. Also, if something blows up,
rem                                 the log says on which game thread and with
rem                                 which registers.
rem    tools\parche_gpu_fallback.py if there is no graphics card with Direct3D 12
rem                                 feature level 11_0, it tries WARP before
rem                                 giving up; and if that fails too, a dialog box
rem                                 appears instead of doing nothing on open.
rem    tools\parche_restaurar.py    settings menu (F4) improvements: a
rem                                 "Restore defaults" button and sliders for
rem                                 decimal settings with limits. They fix
rem                                 nothing, but while testing performance cvars
rem                                 you touch six or seven and then there is no
rem                                 way back to the starting point without
rem                                 restarting.
rem    tools\parche_velocidad.py    a game_speed setting that multiplies the
rem                                 speed at which time passes inside the game,
rem                                 movable on the fly from F4. It is not an fps
rem                                 cap: fps is how many times it is drawn, this
rem                                 is how fast the game advances.
rem    tools\parche_backend.py      a gpu_backend setting to choose the graphics
rem                                 API: d3d12 or vulkan. The plugin already knew
rem                                 how to choose; what was missing was someone
rem                                 telling it. It asks for a restart to take
rem                                 effect. DX11 is not on the list because this
rem                                 SDK has no DX11 backend, nor ever did: the
rem                                 Xenos emulation uses things from the DX12
rem                                 generation -ROV, unbounded descriptors, typed
rem                                 writes from shaders-.
rem    tools\parche_anillo.py       XMA instrumentation. It does not change the
rem                                 behavior and with the normal log it prints
rem                                 nothing, but it is what gave the hang a name
rem                                 and a time, and what will be needed if it
rem                                 comes back. Also the unstick relies on its
rem                                 headers, so it goes first.
rem    tools\parche_fpu.py          keeps the host FP exceptions masked. Without
rem                                 it the USA build dies with a hardware FP
rem                                 exception (0xC000008F) inside generated code
rem                                 and no log line.
rem
rem  WHAT GOES INTO build\
rem    NFS_Most_Wanted.exe        THE LAUNCHER, with the game icon. It is what
rem                               you must open: it brings up the options window
rem                               and from there you play.
rem    nfsmw.exe                  the actual game. It was called
rem                               NFS_Most_Wanted.exe until the launcher took
rem                               the name. Opening it directly works the same
rem                               as always: the ISO is looked for next to it.
rem    rexruntime.dll             SDK runtime: the patches live here
rem    rexgpu-xenos.dll           GPU emulation. It is loaded with LoadLibrary
rem                               according to the gpu_plugin cvar, so it does
rem                               NOT appear in the linker dependencies: you
rem                               have to copy it by hand or the screen comes
rem                               out black.
rem    MSVCP140.dll               \  Visual C++ runtime. The ones the .exe and
rem    MSVCP140_ATOMIC_WAIT.dll    | rexruntime import and that do not come
rem    VCRUNTIME140.dll            | with Windows. They are resolved from the
rem    VCRUNTIME140_1.dll         /  toolchain itself, without fixed paths.
rem    LANZADOR.bat / lanzador.ps1
rem    nfsmw.toml  COMPARAR_VIDEO.bat  PROBAR.bat  matriz.ps1  LEEME.txt
rem
rem  THE ISO IS NOT COPIED. It weighs several GB, it is yours, and the game
rem  reads it on the fly. Put it in build\ yourself when you want to use the
rem  folder.
rem
rem  SALIDA is used for return codes, NEVER "RC": that variable is set by
rem  vcvars64 to the resource compiler path and CMake reads it when it
rem  detects the toolchain.
rem ===========================================================================

if /i "%~1"=="__run" goto :run

if not exist "logs" mkdir "logs"

echo ============================================
echo   Portable build
echo ============================================
echo.
echo Checks the patches, rebuilds whatever is needed and assembles the
echo portable folder  build\
echo.
echo   1. SDK patches       audio, vsync, GPU, F4 menu, graphics API
echo   2. Rebuild the SDK   that is where those patches live
echo   3. Build the app
echo   4. Assemble build\
echo   5. Check that the folder is self-contained
echo.
echo If everything was already up to date, it takes seconds.
echo If relinking is needed, the final step is almost 50 MB and prints nothing
echo for several minutes. DO NOT CLOSE THE WINDOW.
echo.
echo Log at logs\construir.log
echo.
pause

echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { & $env:ComSpec /c 'CONSTRUIR.bat __run 2>&1' | Tee-Object -FilePath 'logs\construir.log' }"

echo.
echo ============================================
pause
exit /b

rem ===========================================================================
:run
rem ===========================================================================

call "%~dp0tools\_entorno_vs.bat"
if not defined ENTORNO_OK goto fin

rem Without Python the patches cannot be applied, and without patches the
rem folder would come out without the fixes. Better to stop here than to
rem assemble a mute build.
if not defined PY (
    echo [ERROR] Cannot find Python. It is needed to apply the SDK
    echo         patches, which is where the audio one lives.
    echo         Install Python 3 and try again.
    goto fin
)

echo ############################################
echo # 1/5  SDK PATCHES
echo ############################################
rem Idempotent: if they are already applied they say so and touch nothing.
rem
rem ORDER MATTERS. parche_anillo runs before parche_desatasco because it is
rem the one that puts <atomic> and <chrono> into the kernel file, and the
rem unstick uses them. The unstick checks this and refuses to apply if they
rem are missing, so at most this stops here with a clear message, not in the
rem middle of the build.
%PY% "%~dp0tools\parche_diagnostico.py"
if errorlevel 1 (
    echo [ERROR] Could not apply the diagnostics patch. Stopping.
    goto fin
)
%PY% "%~dp0tools\parche_anillo.py"
if errorlevel 1 (
    echo [ERROR] Could not instrument the XMA kernel. Stopping.
    goto fin
)
%PY% "%~dp0tools\parche_desatasco.py"
if errorlevel 1 (
    echo [ERROR] Could not apply the audio unstick. Stopping.
    echo.
    echo         This is THE hang fix. Without it, the folder is not good
    echo         for distribution, so I will not continue and I will not touch build\
    goto fin
)
rem Vsync and fps limiter. Out of the box NEITHER of the two works: "vsync"
rem exists as a cvar but the presenter's Present had SyncInterval hardcoded
rem to 0, and there was no limiter at all. Without this, those two launcher
rem settings do nothing and the launcher itself warns about it in red.
%PY% "%~dp0tools\parche_presentador.py"
if errorlevel 1 (
    echo [ERROR] Could not apply vsync and the fps limit. Stopping.
    goto fin
)
%PY% "%~dp0tools\parche_gpu_fallback.py"
if errorlevel 1 (
    echo [ERROR] Could not apply the GPU patch. Stopping.
    goto fin
)
rem This one is for convenience, not correctness: the F4 menu restore button.
%PY% "%~dp0tools\parche_restaurar.py"
if errorlevel 1 (
    echo [ERROR] Could not improve the settings menu. Stopping.
    goto fin
)
%PY% "%~dp0tools\parche_velocidad.py"
if errorlevel 1 (
    echo [ERROR] Could not add the speed setting. Stopping.
    goto fin
)
%PY% "%~dp0tools\parche_backend.py"
if errorlevel 1 (
    echo [ERROR] Could not add the graphics API selector. Stopping.
    goto fin
)
rem The multiplayer gate. Adds the grant_user_privileges setting, OFF by
rem default, so putting it here changes nobody's behavior: it only makes the
rem switch available in F4.
%PY% "%~dp0tools\parche_privilegios.py"
if errorlevel 1 (
    echo [ERROR] Could not add the privileges setting. Stopping.
    goto fin
)
rem FP exception masks. Without this the game can die with a hardware FP
rem exception (0xC000008F) inside generated code, with no log line. It is what
rem killed the first USA builds.
%PY% "%~dp0tools\parche_fpu.py"
if errorlevel 1 (
    echo [ERROR] Could not apply the FP exception fix. Stopping.
    goto fin
)
echo.

echo ############################################
echo # 2/5  REBUILD THE SDK
echo ############################################
echo The patches live in rexruntime.dll, not in the .exe. If the SDK was
echo already built and nothing changed, this takes seconds.
echo.
rem ---------------------------------------------------------------------------
rem  CONFIGURE WITH VULKAN ENABLED
rem
rem  On Windows the SDK ships REXGLUE_USE_VULKAN as OFF, so the Vulkan
rem  backend -which is entirely in src/graphics/vulkan- is not built and the
rem  gpu_backend=vulkan setting would have nothing to load.
rem
rem  This turns it on in the CMake cache. It is idempotent: if it was already
rem  on, the configuration changes nothing and takes seconds. THE FIRST TIME
rem  IT IS NOT: changing an option forces half the SDK to rebuild, and on top
rem  of that glslang and spirv-tools come in. That time it takes a good while.
rem
rem  There is no need to install the Vulkan SDK: the headers, the loader, the
rem  memory allocator and glslang already come in thirdparty\
rem ---------------------------------------------------------------------------
pushd "%SDK%"
cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON
set "SALIDA=!errorlevel!"
if not "!SALIDA!"=="0" (
    popd
    echo [ERROR] SDK configuration failed with code !SALIDA!
    echo         If it complains about Vulkan, you can continue without it:
    echo             cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=OFF
    echo         The rest of the patches do not need it.
    goto fin
)
cmake --build out/build/win-amd64 --config Release --target install
set "SALIDA=!errorlevel!"
popd
if not "!SALIDA!"=="0" (
    echo [ERROR] SDK build failed with code !SALIDA!
    echo.
    echo To leave the SDK as it was:
    echo     %PY% tools\parche_anillo.py --revertir
    echo     %PY% tools\parche_diagnostico.py --revertir
    echo     %PY% tools\parche_gpu_fallback.py --revertir
    goto fin
)
echo.

echo ############################################
echo # 3/5  BUILD THE APP IN RELEASE
echo ############################################
set "DIRREL=%~dp0app\out\build\win-amd64-release"
if exist "%DIRREL%\.ninja_lock" del /q "%DIRREL%\.ninja_lock" >nul 2>&1

pushd "app"
cmake --preset win-amd64-release

rem ---------------------------------------------------------------------------
rem  TWO PASSES, AND IT IS NOT A WHIM
rem
rem  The codegen rewrites generated\default\nfsmw_pch.h, and from that header
rem  comes the precompiled header (cmake_pch.hxx.pch) used by the 131 generated
rem  files.
rem
rem  In ONE single pass, ninja decides at startup which files are dirty. At
rem  that moment nfsmw_pch.h has not changed yet, so it takes the PCH as good.
rem  Then, already inside the same pass, the codegen changes it. When it is
rem  the .cpp files' turn, clang compares and aborts:
rem
rem      fatal error: file 'nfsmw_pch.h' has been modified since the
rem      precompiled header was built: size changed (was 18553, now 18522)
rem
rem  By launching the codegen first and separately, the second pass starts
rem  with the final headers and correctly recalculates what must be redone.
rem ---------------------------------------------------------------------------
echo -- Pass 1: codegen --
cmake --build --preset win-amd64-release --target nfsmw_codegen
set "SALIDA=!errorlevel!"
if not "!SALIDA!"=="0" (
    popd
    echo [ERROR] The codegen failed with code !SALIDA!
    goto fin
)

echo.
echo -- Pass 2: build --
cmake --build --preset win-amd64-release
set "SALIDA=!errorlevel!"
popd
if not "!SALIDA!"=="0" (
    echo [ERROR] The build failed with code !SALIDA!
    goto fin
)
echo.

echo ############################################
echo # 4/5  ASSEMBLE build\
echo ############################################
rem Besides copying, it deletes leftovers from previous runs -logs\,
rem matriz\, shaders\, cache\-, which belong to THIS machine and must not
rem travel.
pushd "app"
cmake --build --preset win-amd64-release --target dist
set "SALIDA=!errorlevel!"
popd
if not "!SALIDA!"=="0" (
    echo [ERROR] Could not assemble build\ with code !SALIDA!
    goto fin
)
echo.

rem ---------------------------------------------------------------------------
rem  The launcher, and the rename that puts it in front
rem
rem  The dist target leaves the game as build\NFS_Most_Wanted.exe. This renames
rem  it to nfsmw.exe and puts the launcher in its place, so that double-
rem  clicking the game icon opens the options window. Opening nfsmw.exe
rem  directly still works as before.
rem
rem  It goes BEFORE the self-containment check on purpose: that way what is
rem  checked is the folder as it will end up, launcher included.
rem
rem  If it fails, the folder is still usable: the game will be as nfsmw.exe or
rem  as NFS_Most_Wanted.exe and LANZADOR.bat works the same. That is why it
rem  does not abort.
echo -- Launcher --
call "%~dp0CONSTRUIR_LANZADOR.bat" /silencioso
set "SALIDA=!errorlevel!"
if not "!SALIDA!"=="0" (
    echo [warning] Could not build the launcher ^(code !SALIDA!^).
    echo         The folder is still usable: play with LANZADOR.bat.
)
echo.

echo ############################################
echo # 5/5  CHECK THAT IT IS SELF-CONTAINED
echo ############################################
rem Reads the PE import table of every binary in build\ and follows the
rem dependencies in a chain. It does not use dumpbin on purpose: dumpbin comes
rem with Visual Studio, and the point is to check this without assuming tools.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\comprobar_dist.ps1"

echo.

rem ---------------------------------------------------------------------------
rem  And while at it, the distribution folders
rem
rem  This used to be a manual step: "compress build\ without the ISO". It is
rem  done here because this is exactly the moment when build\ is freshly made
rem  and clean, and because the manual step had a trap: the ISO is several GB
rem  and it is easy to send it by mistake.
rem
rem  It leaves two folders next to the project, in "build release":
rem
rem    NFSMW Windows x64\              ready to play and to send to someone
rem                                    who has THEIR OWN copy of the game
rem    NFSMW Windows x64 - Portable\   everything except the game; this one is
rem                                    what gets published
rem
rem  If it does not find the script, nothing happens: build\ is already made
rem  and can be compressed by hand as always.
set "RELEASE=%~dp0..\build release\PREPARAR_RELEASE.bat"
if exist "%RELEASE%" (
    echo ############################################
    echo # EXTRA  DISTRIBUTION FOLDERS
    echo ############################################
    call "%RELEASE%" /silencioso
    if errorlevel 1 (
        echo [warning] Could not assemble the distribution folders.
        echo         build\ is fine; compress it by hand if needed.
    )
    echo.
)

echo ============================================
echo   DONE
echo ============================================
echo.
echo build\ is remade, with the patches inside and no trace of
echo previous runs.
echo.
echo TO PLAY YOURSELF
echo   Copy your ISO into build\ and double-click NFS_Most_Wanted.exe
echo.
echo   That is the LAUNCHER, with the game icon: it opens the options
echo   window and from there you play. The actual game is nfsmw.exe, and
echo   normally there is no need to touch it. LANZADOR.bat still does the
echo   same.
echo.
echo TO SEND IT TO SOMEONE
echo   It is already made, in  ..\build release\NFSMW Windows x64\
echo   Without the ISO inside. Compress it and send it.
echo.
echo   They have to put THEIR OWN ISO, and from the SAME default.xex. This
echo   is not an emulator: the .exe carries inside the code from that specific
echo   ISO, translated and compiled. With a ROM from another region it will
echo   not start, and it already cost us a day to find out the first time.
echo.

:fin
exit /b
