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
    # Phase A of the native renderer: wraps the game's Direct3D to confirm that each
    # address is the function we think it is. It does not change drawing; it only writes rex_d3d.log.
    src/nfsmw_d3d_trace.cpp
    # The scene is drawn a single time: without the Xbox 360's 3-strip tiling or MSAA
    # (cvar nfsmw_render_sin_mosaico, on by default on Switch).
    src/nfsmw_render_targets.cpp
    # Race-time reductions: road reflection and faces of the car's cubemap
    # (cvars nfsmw_reflejo_carretera and nfsmw_cubemap_caras_max).
    src/nfsmw_recortes_carrera.cpp
    # Rescues the game's audio server thread if the XAudio voice stops
    # returning end-of-packet notifications (cvar nfsmw_audio_rescate).
    src/nfsmw_audio_servidor.cpp
    # Priority of the current thread on the Switch (nfsmw_audio_servidor_prioridad).
    src/nfsmw_hilos_switch.cpp
    # Diagnostic: timing of the sound engine functions on the audio server thread
    # (cvar nfsmw_audio_diag_funciones).
    src/nfsmw_audio_perfil_funciones.cpp
    # Measurement probe for the car engine sound (Ginsu): buffers, volumes and voices; it only
    # measures (cvar nfsmw_audio_sonda_motor).
    src/nfsmw_audio_motor_sonda.cpp
    # Guards for null pointers the original code dereferences.
    src/nfsmw_guardas.cpp
    # The language number of the PAL editions that share the Spanish build (French, German, Italian).
    src/nfsmw_idioma_edicion.cpp
    # Linear resamplers of the sound engine in native code, bit-identical
    # (cvar nfsmw_audio_remuestreo_nativo).
    src/nfsmw_audio_remuestreo.cpp
    # Recursive filter of the sound engine (sub_825CD088) in native code, bit-identical
    # (cvar nfsmw_audio_filtro_nativo).
    src/nfsmw_audio_filtro.cpp
    # Gain-scaled sum of the sound engine (sub_825FDFB0) in native code, bit-identical
    # (cvar nfsmw_audio_suma_nativa).
    src/nfsmw_audio_suma.cpp
    # Environment variables for Mesa/NVK before creating Vulkan (cvar nfsmw_mesa_entorno) and, on the
    # Switch, the size of Mesa's on-disk shader cache.
    src/nfsmw_entorno_mesa.cpp
    # Native memset of the game's CRT (sub_826BE610), like the memcpy of [rexcrt] (cvar nfsmw_crt_memset_nativo):
    # src/nfsmw_crt_nativo.cpp is left out of the build. The recompiler's [rexcrt] (overrides.toml) now does that
    # memset: it emits rexcrt_memset at all 1,109 call sites, so the original function no longer exists and the
    # hook has nothing to hook (on PC it broke linking). Both did the same: std::memset with the same address
    # translation.
    # Cutscene diagnostic: frame presentation (cvar nfsmw_video_diag).
    src/nfsmw_video_diag.cpp
    # WMV3 cutscenes decoded with the SDK's FFmpeg instead of the recompiled
    # XDK decoder (cvar nfsmw_video_wmv3_nativo).
    src/nfsmw_video_wmv3.cpp
    src/nfsmw_video_nativo.cpp
    # Shadows updated once every N frames (cvar nfsmw_sombras_cada,
    # 1 = as the game does; it can be changed at run time from the settings menu).
    src/nfsmw_recorte_sombras.cpp
    src/nfsmw_sombras_lod.cpp
    # The native graphics system: runs the game's PM4 ring and hands draws, copies and swaps to the
    # renderer (src/renderer).
    src/nfsmw_nativo_sistema.cpp
    # The game's D3D waits without spinning: sleeps until the PM4 ring thread advances
    # (cvar nfsmw_espera_anillo_bloqueante).
    src/nfsmw_espera_anillo.cpp
    # Frame handoff between the game's two threads without a Sleep(0) loop
    # (cvar nfsmw_espera_fotograma_bloqueante).
    src/nfsmw_espera_fotograma.cpp
    # Dump of the changed registers of the game's D3D (sub_825A2AA0) in native code: one copy instead of
    # ~15-20 instructions per word, bit-identical (cvar nfsmw_d3d_registros_nativo).
    src/nfsmw_d3d_registros_nativo.cpp
    # Per-draw material parameters (sub_824511E8 and its writers 8244F2E0, 82449360, 82449618,
    # 82449988 and 82449C00) in native code, bit-identical and checked at run time against the original
    # (cvar nfsmw_material_nativo).
    src/nfsmw_material_nativo.cpp
    # eViewPlatInterface::Render (sub_8243E358, the submesh loop of each model) in native code, bit-identical
    # and checked at run time against the original (cvar nfsmw_eview_nativo).
    src/nfsmw_eview_nativo.cpp
    # Per-draw matrices (sub_824538D0: A x P, A x Q, A x C and the rigid inverse of A) in native code,
    # bit-identical and checked at run time against the original (cvar nfsmw_matrices_nativo).
    src/nfsmw_matrices_nativo.cpp
    # ScenerySectionHeader::DrawAScenery (sub_824C2850: culling, size in pixels, LOD and SceneryDrawInfo of
    # each scenery object) in native code, bit-identical and checked at run time against the original
    # (cvar nfsmw_escenario_nativo).
    src/nfsmw_escenario_nativo.cpp
    # Start of an effect pass (sub_82448E80: dirty masks, shaders, render states and sampler states) in
    # native code, identical and checked at run time against a literal copy of the original
    # (cvar nfsmw_efecto_pasada_nativo).
    src/nfsmw_efecto_pasada_nativo.cpp
    # The draw glue (sub_82452730 with the streams, the indices and the draw of each object) and the loop
    # over the list that calls it (sub_82454B50) in native code, identical, keeping the game's cache hints
    # (dcbt), and checked at run time against a literal copy of the original (cvar nfsmw_pegamento_nativo).
    src/nfsmw_pegamento_nativo.cpp
    # Identifies the shaders that reach the PM4 ring
    # in the NFSSPV library (nfsmw_shaders.nfsp next to the executable).
    src/nfsmw_nativo_shaders.cpp
    src/nfsmw_shader_library.cpp
    # VS and PS of each draw from the game thread (shader constructors and the D3D Draw* functions).
    src/nfsmw_nativo_ganchos.cpp
    src/nfsmw_escenario_lod.cpp
    # The streamer requests the zone too late: facade popping and a hitch when entering a new
    # zone (hook on TrackStreamer::GetPredictedZone).
    src/nfsmw_streaming_anticipacion.cpp
    # Automated tests on PC: virtual gamepad driven by a script (nfsmw_prueba_botones).
    src/nfsmw_prueba_entrada.cpp
    src/nfsmw_tactil.cpp
    # Internal resolution and FPS limit in the menu (F4) that work with the native renderer, with the
    # settings that have no effect taken out of the menu.
    src/nfsmw_ajustes_graficos.cpp
    # Profile dump of the instrumented build for PGO (NFSMW_PGO=generar; empty otherwise).
    src/nfsmw_pgo.cpp
    # Lanzador dentro del juego: eleccion de la copia del juego, ajustes e idioma.
    src/edicion.cpp
    src/lanzador/actualizacion.cpp
    src/lanzador/idioma.cpp
    src/lanzador/iso.cpp
    src/lanzador/lanzador.cpp
    src/lanzador/partidas.cpp
    src/lanzador/perfiles.cpp
    src/lanzador/sistema.cpp
    src/lanzador/trucos.cpp
)

if(WIN32)
    # The game is a module per edition, nfsmw_<edition>.dll, that the one nfsmw.exe loads (nfsmw_anfitrion below;
    # see src/edicion.h). Its entry point is the SDK's wWinMain, exported by src/nfsmw_modulo_win.cpp.
    add_library(nfsmw SHARED ${NFSMW_SOURCES} src/nfsmw_modulo_win.cpp)
    set_target_properties(nfsmw PROPERTIES OUTPUT_NAME "nfsmw_${NFSMW_EDICION}")
    # PC build: masks again the floating-point exceptions that
    # rex::ppc::FPSCR can leave unmasked on x86 (see the file).
    target_sources(nfsmw PRIVATE src/nfsmw_pc_coma_flotante.cpp)
    # Sampling CPU profiler for the PC tests (cvar nfsmw_perfil_pc_desde_s, 0 = never).
    target_sources(nfsmw PRIVATE src/nfsmw_perfil_pc.cpp)
    # Dialogo nativo de ficheros del lanzador (IFileOpenDialog) y ShellExecute.
    target_link_libraries(nfsmw PRIVATE ole32 shell32 uuid winhttp)
    # Full symbols (CodeView) of the native renderer in the PDB: without them the profiler only sees the public
    # symbols, and all the code in anonymous namespaces shows up under the name of the preceding public symbol.
    # Debug info only; it does not change the generated code.
    set_source_files_properties(
        src/nfsmw_nativo_sistema.cpp
        src/nfsmw_nativo_shaders.cpp src/nfsmw_nativo_ganchos.cpp src/nfsmw_espera_anillo.cpp
        src/nfsmw_espera_fotograma.cpp
        PROPERTIES COMPILE_OPTIONS "-g;-gcodeview")
elseif(ANDROID)
    # SDLActivity (app/android) loads the game as libmain.so and calls its SDL_main.
    add_library(nfsmw SHARED ${NFSMW_SOURCES})
    set_target_properties(nfsmw PROPERTIES OUTPUT_NAME main)
    # Stack dump of every thread when the watchdog sees the game stalled.
    target_sources(nfsmw PRIVATE src/nfsmw_pilas_android.cpp)
else()
    add_executable(nfsmw ${NFSMW_SOURCES})
endif()

# GPU_PLUGINS leaves the graphics backend DLL next to the .exe
# (rexgpu-xenosd.dll in Debug, rexgpu-xenos.dll in Release).
#
# Both things are needed to get an image:
#   1. this line, which puts the DLL where the loader looks for it
#   2. gpu_plugin set to "xenos" at run time
#
# The gpu_plugin CVar is empty by default, and empty means "no GPU
# emulation": the game initializes its ring buffer and its graphics callbacks, the
# runtime discards them with "no GPU emulation loaded", and the screen stays
# black even though the game is running perfectly.
#
# Since src/nfsmw_app.h sets gpu_plugin=xenos by default, the --gpu_plugin
# argument is no longer needed; it can still be passed to change it.
rexglue_setup_target(nfsmw GPU_PLUGINS xenos)

# The launcher's game art (src/lanzador, LeerArte), built into the module: the images of the website
# (site/assets/arte) that the launcher draws, as byte arrays in a generated file of the build folder. No art next to
# the executable, in the release or in the APK. The list has to follow kIconosArte and the other LeerArte calls.
set(NFSMW_ARTE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../site/assets/arte")
set(NFSMW_ARTE_FICHEROS grano.png logo-mw.png icono-career.png icono-customize-car.png icono-quick-race.png
    icono-profile-manager.png icono-bounty.png icono-options.png icono-rap-sheet.png)
set(_nfsmw_arte_arrays "")
set(_nfsmw_arte_tabla "")
set(_nfsmw_arte_n 0)
foreach(_fichero IN LISTS NFSMW_ARTE_FICHEROS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NFSMW_ARTE_DIR}/${_fichero}")
    file(READ "${NFSMW_ARTE_DIR}/${_fichero}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
    string(APPEND _nfsmw_arte_arrays "static const unsigned char kArte${_nfsmw_arte_n}[] = {${_hex}};\n")
    string(APPEND _nfsmw_arte_tabla "    {\"${_fichero}\", kArte${_nfsmw_arte_n}, sizeof(kArte${_nfsmw_arte_n})},\n")
    math(EXPR _nfsmw_arte_n "${_nfsmw_arte_n} + 1")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generado/arte_embebido.inc" CONTENT
     "// Generated by app/CMakeLists.txt from site/assets/arte: do not edit.\n${_nfsmw_arte_arrays}\nstruct ArteEmbebido {\n  const char* nombre;\n  const unsigned char* datos;\n  unsigned long long tam;\n};\nconstexpr ArteEmbebido kArteEmbebido[] = {\n${_nfsmw_arte_tabla}};\n"
     @ONLY)
target_include_directories(nfsmw PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generado")

# The SDK's FFmpeg (WMV3 decoder) for the cutscenes: nfsmw_video_wmv3.cpp.
target_link_libraries(nfsmw PRIVATE libavcodec)

# PC build with the SDK as a subdirectory (tools/pc/construir_pc.bat):
# rex_app.cpp includes imgui.h and rexruntime links imgui privately, so its
# include path does not reach the executable. On Switch rexglue_switch.cmake handles it, and with
# the SDK installed the headers are already in its include directory (the target does not exist there).
# The same goes for renderdoc_app.h, which rex/ui/vulkan/instance.h includes outside
# Switch (nfsmw_nativo_sistema.cpp uses the SDK's Vulkan device).
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(nfsmw PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(nfsmw PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# The SDK's xxHash: fingerprints for the shader library and the native renderer.
find_path(NFSMW_XXHASH_INCLUDE xxhash.h HINTS "${REXSDK_DIR}/thirdparty/xxHash" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_include_directories(nfsmw PRIVATE "${NFSMW_XXHASH_INCLUDE}")

# The launcher's shader library generator (src/lanzador/shaders.cpp): Xenos microcode straight to SPIR-V with
# shaders/XenosRecomp/spirv_recompiler.cpp, built on the SDK's glslang SPIR-V builder. Neither HLSL nor DXC is
# involved, so nothing has to be shipped next to the executable. PC and Android.
if(WIN32 OR ANDROID)
    set(NFSMW_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
    set(NFSMW_GENERADOR_FUENTES src/lanzador/shaders.cpp "${NFSMW_SHADERS_DIR}/XenosRecomp/spirv_recompiler.cpp")
    target_sources(nfsmw PRIVATE ${NFSMW_GENERADOR_FUENTES})
    # XenosRecomp's minimal header (big-endian fields, fmt) and this game's changes (NFSMW_RECOMP). NOMINMAX and
    # WIN32_LEAN_AND_MEAN because that header includes Windows.h.
    set_source_files_properties(${NFSMW_GENERADOR_FUENTES} PROPERTIES
        COMPILE_OPTIONS "-include;${NFSMW_SHADERS_DIR}/pch_min.h;-Wno-switch;-Wno-unused-variable;-Wno-null-arithmetic"
        COMPILE_DEFINITIONS "NFSMW_RECOMP;NOMINMAX;WIN32_LEAN_AND_MEAN;XXH_INLINE_ALL;_CRT_SECURE_NO_WARNINGS")
    target_include_directories(nfsmw PRIVATE
        "${NFSMW_SHADERS_DIR}" "${NFSMW_SHADERS_DIR}/XenosRecomp"
        "${REXSDK_DIR}/thirdparty" "${REXSDK_DIR}/thirdparty/glslang/SPIRV")
    target_link_libraries(nfsmw PRIVATE mspack fmt glslang::SPIRV)

    # The renderer (Vulkan 1.1, see src/renderer/README.md). It translates shaders at run time with the same
    # translator.
    target_sources(nfsmw PRIVATE
        src/renderer/depth_resolve.cpp
        src/renderer/draws.cpp
        src/renderer/frame_ring.cpp
        src/renderer/geometry_cache.cpp
        src/renderer/output.cpp
        src/renderer/pipelines.cpp
        src/renderer/render_targets.cpp
        src/renderer/shader_modules.cpp
        src/renderer/textures.cpp
        src/renderer/vertex_input.cpp
        src/renderer/vulkan_context.cpp
        src/renderer/vulkan_renderer.cpp)
endif()

# Experimental library: the synthetic tests do not validate the full game.
# Regression confirmed on Switch: the release build excludes this path.
option(NFSMW_NATIVE_SHADER_LIBRARY "Integrar la biblioteca experimental de shaders nativos" OFF)
if(NFSMW_NATIVE_SHADER_LIBRARY)
    target_sources(nfsmw PRIVATE
        src/nfsmw_shader_vulkan.cpp
        src/nfsmw_video_vulkan.cpp
        src/nfsmw_video_session.cpp
        src/nfsmw_video_bridge.cpp
        src/nfsmw_video_present.cpp
        src/nfsmw_shader_hooks.cpp)
    target_compile_definitions(nfsmw PRIVATE NFSMW_NATIVE_SHADER_LIBRARY=1)
endif()

# Version of this executable, for the launcher's new-version notice.
set(NFSMW_VERSION "1.0.2" CACHE STRING "NFSMW Recompiled version")
set_source_files_properties(src/lanzador/actualizacion.cpp PROPERTIES COMPILE_DEFINITIONS "NFSMW_VERSION=\"${NFSMW_VERSION}\"")

# Edition of the game this executable is built for (src/edicion.h): es, usa...
set(NFSMW_EDICION "es" CACHE STRING "Game edition (es, usa, eu)")
set_source_files_properties(src/edicion.cpp PROPERTIES COMPILE_DEFINITIONS "NFSMW_EDICION=\"${NFSMW_EDICION}\"")

# Windows: the icon and version in both executables, and the "NFSMW Recomp.exe" starter that sits at the top of the
# release folder and runs data\nfsmw.exe (stub/main.cpp).
if(WIN32)
    enable_language(RC)
    # The resource only takes numbers: a test build like 1.0.2-amd1 keeps 1.0.2.
    string(REGEX REPLACE "-.*$" "" NFSMW_VERSION_NUM "${NFSMW_VERSION}")
    string(REPLACE "." "," NFSMW_VERSION_COMAS "${NFSMW_VERSION_NUM}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generado/version_rc.h"
         "#define NFSMW_VERSION_RC ${NFSMW_VERSION_COMAS},0\n#define NFSMW_VERSION_STR \"${NFSMW_VERSION}\"\n")
    set(NFSMW_RC "${CMAKE_CURRENT_SOURCE_DIR}/resources/nfsmw.rc")
    set_source_files_properties(${NFSMW_RC} PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generado;${CMAKE_CURRENT_SOURCE_DIR}/resources")
    target_sources(nfsmw PRIVATE ${NFSMW_RC})
    # nfsmw.exe: picks the edition's module and runs it (anfitrion/main.cpp).
    add_executable(nfsmw_anfitrion WIN32 anfitrion/main.cpp ${NFSMW_RC})
    set_target_properties(nfsmw_anfitrion PROPERTIES OUTPUT_NAME nfsmw)
    target_link_libraries(nfsmw_anfitrion PRIVATE shell32)
    add_executable(nfsmw_inicio WIN32 stub/main.cpp ${NFSMW_RC})
    set_target_properties(nfsmw_inicio PROPERTIES OUTPUT_NAME "NFSMW Recomp")
endif()
