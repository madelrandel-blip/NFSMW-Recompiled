# nfsmw - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nfsmw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NFSMW_SOURCES
    src/main.cpp
    src/nfsmw_menu.cpp
)

if(WIN32)
    add_executable(nfsmw WIN32 ${NFSMW_SOURCES})
else()
    add_executable(nfsmw ${NFSMW_SOURCES})
endif()

# GPU_PLUGINS leaves the graphics backend DLL next to the .exe
# (rexgpu-xenosd.dll in Debug, rexgpu-xenos.dll in Release).
#
# BOTH THINGS are needed to see an image:
#   1. this line, which places the DLL where the loader looks for it
#   2. gpu_plugin being "xenos" at runtime
#
# The gpu_plugin CVar is empty by default, and empty means "no GPU emulation":
# the game initializes its ring buffer and its graphics callbacks, the runtime
# drops them with "no GPU emulation loaded", and the screen stays black even
# though the game is running perfectly.
#
# Since src/nfsmw_app.h defaults gpu_plugin=xenos, the --gpu_plugin argument
# is no longer needed; it can still be passed to change it.
rexglue_setup_target(nfsmw GPU_PLUGINS xenos)

# Native Qt launcher (Linux). Used by packaging/appimage/.
option(NFSMW_BUILD_LAUNCHER "Build the native Qt launcher" ON)
option(NFSMW_LAUNCHER_BUILD_TESTS "Build Qt launcher tests" OFF)
if(NFSMW_BUILD_LAUNCHER AND UNIX AND NOT APPLE)
    find_package(Qt6 6.4 COMPONENTS Core Gui Widgets QUIET)
    if(Qt6_FOUND)
        if(NFSMW_LAUNCHER_BUILD_TESTS)
            enable_testing()
        endif()
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../launcher-linux"
                         "${CMAKE_CURRENT_BINARY_DIR}/launcher-linux")
    else()
        message(WARNING
            "Qt 6.4+ was not found; nfsmw_launcher will not be built")
    endif()
endif()


# =============================================================================
#  TARGET "dist" - the portable folder, which is called  build\
#
#  Gathers into a single folder the executable, the project DLLs and the
#  Visual C++ runtime DLLs, so it can be taken to a machine without
#  development tools.
#
#      cmake --build --preset win-amd64-release --target dist
#
#  THE TARGET IS CALLED "dist" AND THE FOLDER "build". That is not an
#  oversight: build/ was already in .gitignore and dist/ was not, so leaving
#  the folder there -it contains the executable with the game code- keeps it
#  out of git by itself. To put it somewhere else:  -D NFSMW_DIST_DIR=...
#
#  DO NOT CONFUSE it with app/out/build/win-amd64-release, which is where
#  CMake compiles. This is the folder already assembled, ready to be taken
#  away.
#
#  WHAT IT DOES NOT DO, AND IT MATTERS:
#  it does not put the ISO inside anything. It remains a loose file that the
#  game reads on the fly, and src/nfsmw_app.h looks for it next to the .exe.
#  That way the folder works wherever it is, without any fixed path.
#
#  None of this changes how the project is built: it is one more step, on the
#  side.
# =============================================================================
if(WIN32)
    # Resolves the VC runtime DLLs from the toolchain being used.
    # With _SKIP set to TRUE it installs nothing: it only fills the list, which
    # is the only thing we want. Without this, a path like
    # "C:\Program Files\Microsoft Visual Studio\...\Microsoft.VC143.CRT" would
    # have to be written by hand, which is exactly what must not be here.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
    include(InstallRequiredSystemLibraries)

    # Deliberately NOT cached. If they were cache variables, changing the
    # default value would do nothing in an already configured tree: it would
    # keep using the old one without a peep, the target would say "done" and
    # the folder would appear where it did before. This way what is here always
    # wins, and a -D from the command line still overrides it.
    if(NOT DEFINED NFSMW_DIST_NOMBRE)
        set(NFSMW_DIST_NOMBRE "NFS_Most_Wanted")
    endif()
    if(NOT DEFINED NFSMW_DIST_DIR)
        set(NFSMW_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build")
    endif()

    add_custom_target(dist
        DEPENDS nfsmw
        COMMAND ${CMAKE_COMMAND}
                -D "D_EXE=$<TARGET_FILE:nfsmw>"
                -D "D_BUILD=$<TARGET_FILE_DIR:nfsmw>"
                -D "D_DIST=${NFSMW_DIST_DIR}"
                -D "D_NOMBRE=${NFSMW_DIST_NOMBRE}"
                -D "D_VCRUNTIME=${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}"
                -D "D_REDIST=$ENV{VCToolsRedistDir}"
                -D "D_PLANTILLA=${CMAKE_CURRENT_SOURCE_DIR}/dist_leeme.txt"
                -D "D_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/nfsmw.toml"
                -D "D_EDRAM=${CMAKE_CURRENT_SOURCE_DIR}/dist_comparar_edram.bat"
                -D "D_LANZADOR=${CMAKE_CURRENT_SOURCE_DIR}/../tools/lanzador/lanzador.ps1"
                -D "D_LANZADOR_BAT=${CMAKE_CURRENT_SOURCE_DIR}/dist_lanzador.bat"
                -D "D_MATRIZ=${CMAKE_CURRENT_SOURCE_DIR}/../tools/matriz.ps1"
                -D "D_PROBAR=${CMAKE_CURRENT_SOURCE_DIR}/dist_probar.bat"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hacer_dist.cmake"
        COMMENT "Assembling the portable folder in ${NFSMW_DIST_DIR}"
        VERBATIM
    )
endif()
