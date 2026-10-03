#!/usr/bin/env python3
"""
Adds a graphics API selector: D3D12 or Vulkan, selectable from F4.

    python tools/parche_backend.py            apply
    python tools/parche_backend.py --estado
    python tools/parche_backend.py --revertir

It touches one SDK file:  src/ui/rex_app.cpp

It does not keep a .original: it applies and undoes by exact text replacement,
block by block, like the other patches in this project.

NOTE: this only adds the SETTING. For the "vulkan" option to do anything, the
SDK must be compiled with the Vulkan backend inside:

    cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON

DIST.bat and PARCHE_ARRANQUE.bat already do that. And if it is not compiled,
picking vulkan does not leave the game unable to start: it falls back to the
other API, says so in the log and goes on.


DIRECTX 11 IS NOT THERE, AND IT IS NOT AN OVERSIGHT
===================================================

The question this came from was whether DX11 could be used instead of DX12 to
gain performance. No: this SDK only has two backends, and its CMake says so.

    option(REXGLUE_USE_D3D12  "Enable D3D12 graphics backend" ON)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" OFF)

And it is no accident. Xenos emulation leans on things from the DX12
generation: the rasterizer ordered views of the ROV path, unbounded
descriptors, typed writes from shaders for memexport. A DX11 backend is not a
setting, it is redoing the GPU plugin.

Besides, it would not have fixed anything: the bottleneck is the GPU -100%
usage, with the CPU far from saturated-, and the API does not change how many
pixels must be shaded. Where DX11 sometimes wins is when the bottleneck is
sending draw orders from the CPU, which is not the case.


WHAT IS POSSIBLE, AND IT IS THIS
================================

The plugin already knows how to pick a backend by name. It is in
src/graphics/plugin_main.cpp:

    std::string_view backend = info->backend ? info->backend : "any";
    if (backend == "any" || backend == "d3d12")  return new D3D12GraphicsSystem();
    if (backend == "any" || backend == "vulkan") return new VulkanGraphicsSystem();

And LoadGpuPlugin accepts that name as a second parameter. The only thing
missing was someone passing it: rex_app.cpp called it with a single argument,
so it always came out "any", which in practice is D3D12 for being the first.

The Vulkan backend is all there in the code -around a megabyte of sources in
src/graphics/vulkan- and everything it needs already comes with the SDK:
vulkan-headers, vulkan-loader, vulkan-memory-allocator, glslang and spirv-tools.
Nothing extra needs to be installed.


WHAT TO EXPECT
==============

No idea, and I am not going to sugarcoat it. On an Intel of this generation the
Vulkan driver is a completely different path from D3D12's, and it may go better
or worse. What is certain is that this SDK's Vulkan backend is less proven than
the D3D12 one: if it comes out with graphical glitches or does not start, you
go back to d3d12 and nothing is lost but the time spent compiling.

A FAILURE OF THE FIRST VERSION, FOR THE RECORD
==============================================

v1 marked the setting as kInitOnly, copying what gpu_plugin did. In the menu it
showed up in red and disabled: visible, and it could not be touched. Bad.

kInitOnly means "you cannot even save the new value", and the SDK itself
refuses to write it once started. But here the value CAN be saved; what cannot
be done is applying it with the game open. That is kRequiresRestart, which also
makes the SDK note it in a list of pending changes that already existed
-GetPendingRestartFlags- and that nobody showed. The menu patch shows it now,
with a button to restart.

And while at it, "any" was removed from the options: with any there was no way
to know which one was really set, which was exactly what needed to be seen.
"""

import argparse
import pathlib
import sys

# ---------------------------------------------------------------------------
#  1) The cvar, next to the gpu_plugin one
# ---------------------------------------------------------------------------

CVAR_ANCLA = """REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",
                      "GPU emulation plugin to load at startup (e.g. 'xenos'); empty disables "
                      "GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
"""

CVAR_NUEVO = """REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",
                      "GPU emulation plugin to load at startup (e.g. 'xenos'); empty disables "
                      "GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// PARCHE LOCAL - selector de API grafica
//
// El plugin ya sabia elegir entre D3D12 y Vulkan por nombre; lo que faltaba
// era que alguien se lo dijera. Ver plugin_main.cpp:
//
//     if (backend == "any" || backend == "d3d12")  -> D3D12GraphicsSystem
//     if (backend == "any" || backend == "vulkan") -> VulkanGraphicsSystem
//
// "any" coge el primero que este compilado, que es D3D12. Con esto se puede
// forzar uno concreto y comparar.
//
// PIDE REINICIO, PERO NO ES DE SOLO LECTURA. La primera version lo puse como
// kInitOnly, igual que gpu_plugin, y el menu de F4 lo pintaba en rojo y
// deshabilitado: se veia pero no se podia tocar. Es que kInitOnly significa
// "no se puede ni guardar el valor nuevo", y aqui si se puede: lo que no se
// puede es aplicarlo sin reiniciar. Eso es kRequiresRestart, que ademas hace
// que el SDK lo apunte en su lista de cambios pendientes -y el menu la
// ensena, con un boton para reiniciar-.
//
// Y ya no hay "any". Con any no se sabia cual estaba puesta de verdad, que era
// justo lo que habia que ensenar. Con dos opciones y ambas explicitas, el
// ajuste ES la respuesta a "cual se esta usando".
//
// El texto va en ingles porque es lo que sale en esa ventana, que es del SDK
// y esta entera en ingles.
REXCVAR_DEFINE_STRING(gpu_backend, "d3d12", "GPU",
                      "Graphics API: d3d12 or vulkan. Takes effect on restart. If the one "
                      "you pick was not built into this copy, the game falls back to the "
                      "other one and says so in the log.")
    .allowed({"d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

"""

# ---------------------------------------------------------------------------
#  2) Handing it to the loader
# ---------------------------------------------------------------------------

CARGA_ANCLA = """  if (!config_.graphics && !config_.gpu_plugin.empty()) {
    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin);
"""

CARGA_NUEVO = """  if (!config_.graphics && !config_.gpu_plugin.empty()) {
    // PARCHE LOCAL - selector de API grafica
    //
    // Antes se llamaba con un solo argumento, asi que el backend quedaba en
    // "any" por defecto y siempre salia D3D12 por ser el primero del if. El
    // segundo parametro ya existia en LoadGpuPlugin; solo faltaba usarlo.
    const std::string backend_elegido = REXCVAR_GET(gpu_backend);
    REXLOG_INFO("API grafica pedida: {}", backend_elegido);
    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin, backend_elegido);

    // Si la que se ha pedido no esta compilada en esta copia, el plugin
    // devuelve nada. Antes de eso significaba pantalla de error y a editar el
    // toml a mano; ahora se cae a la otra y se dice bien claro. Como solo hay
    // dos, "any" es exactamente la otra.
    if (!config_.graphics) {
      REXLOG_WARN("La API grafica '{}' no esta compilada en esta copia. Se prueba con la otra.",
                  backend_elegido);
      config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin, "any");
      if (config_.graphics) {
        // Solo hay dos, asi que "any" es exactamente la otra.
        const char* la_otra = (backend_elegido == "vulkan") ? "d3d12" : "vulkan";
        REXLOG_WARN("Arrancando con '{}' en su lugar. Cambia gpu_backend para quitar el aviso.",
                    la_otra);
      }
    }

    // Y aqui se borra la lista de "pendiente de reiniciar", que a estas alturas
    // solo puede tener mentiras.
    //
    // SetFlagFromSource apunta en esa lista cualquier cvar kRequiresRestart que
    // se toque, SIN MIRAR de donde viene el valor. Con lo cual un
    // --gpu_backend=vulkan en la linea de comandos, que se aplica en el
    // arranque y ya esta puesto cuando se lee aqui arriba, entraba igualmente
    // en la lista, y el menu de F4 abria diciendo "Restart needed to apply:
    // gpu_backend" desde el primer segundo. Un aviso que no se puede quitar
    // deja de leerse, y entonces tampoco se lee cuando es de verdad.
    //
    // Todo lo que hay en la lista en este punto viene del toml o de la linea de
    // comandos, o sea que ya esta aplicado por definicion. Lo que se cambie
    // luego desde F4 se vuelve a apuntar y ese aviso si es real.
    rex::cvar::ClearPendingRestartFlags();
"""

BLOQUES = [
    ("el cvar gpu_backend", CVAR_ANCLA, CVAR_NUEVO),
    ("pasarselo al cargador", CARGA_ANCLA, CARGA_NUEVO),
]

# ---------------------------------------------------------------------------
#  Previous version of THIS patch, so it can be migrated
#
#  v1 left gpu_backend as kInitOnly -which is why it showed in red and could
#  not be touched in the menu-, with "any" among the options and no fallback if
#  the chosen API was not compiled. Texts taken as-is from the already patched
#  file, not written by hand, so the replacement is exact.
# ---------------------------------------------------------------------------

VIEJO_CVAR = 'REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",\n                      "GPU emulation plugin to load at startup (e.g. \'xenos\'); empty disables "\n                      "GPU emulation")\n    .lifecycle(rex::cvar::Lifecycle::kInitOnly);\n\n// PARCHE LOCAL - selector de API grafica\n//\n// El plugin ya sabia elegir entre D3D12 y Vulkan por nombre; lo que faltaba\n// era que alguien se lo dijera. Ver plugin_main.cpp:\n//\n//     if (backend == "any" || backend == "d3d12")  -> D3D12GraphicsSystem\n//     if (backend == "any" || backend == "vulkan") -> VulkanGraphicsSystem\n//\n// "any" coge el primero que este compilado, que es D3D12. Con esto se puede\n// forzar uno concreto y comparar.\n//\n// De solo lectura en marcha: el sistema grafico se crea una vez al arrancar y\n// no se puede cambiar con el juego abierto. En F4 se ve, se cambia, y hace\n// falta reiniciar; igual que gpu_plugin, que esta justo encima.\n//\n// El texto va en ingles porque es lo que sale en esa ventana, que es del SDK\n// y esta entera en ingles.\nREXCVAR_DEFINE_STRING(gpu_backend, "any", "GPU",\n                      "Graphics API to use: any (first one available), d3d12 or vulkan. "\n                      "Vulkan only works if the SDK was built with REXGLUE_USE_VULKAN=ON. "\n                      "Takes effect on restart.")\n    .allowed({"any", "d3d12", "vulkan"})\n    .lifecycle(rex::cvar::Lifecycle::kInitOnly);\n'

VIEJA_CARGA = '  if (!config_.graphics && !config_.gpu_plugin.empty()) {\n    // PARCHE LOCAL - selector de API grafica\n    //\n    // Antes se llamaba con un solo argumento, asi que el backend quedaba en\n    // "any" por defecto y siempre salia D3D12 por ser el primero del if. El\n    // segundo parametro ya existia en LoadGpuPlugin; solo faltaba usarlo.\n    const std::string backend_elegido = REXCVAR_GET(gpu_backend);\n    REXLOG_INFO("API grafica pedida: {}", backend_elegido);\n    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin, backend_elegido);\n'

# v2 added a global variable shared between rex_app.cpp and the settings menu.
# IT COULD NOT WORK, and the linker said it in so many words:
#
#   lld-link: error: undefined symbol: rex::ui::g_gpu_backend_en_uso
#   >>> referenced by settings_overlay.cpp.obj
#
# I took it for granted that both files went into the same library. Well, no:
# rex_app.cpp is NOT compiled inside the SDK, it is INSTALLED as source in
# share/rexglue/ and each application compiles it. So the definition ended up
# inside nfsmw.exe and the reference inside rexruntime.dll. It can be seen by
# looking at where the error itself leaves the .obj files.
VIEJO_CVAR_V2 = 'REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",\n                      "GPU emulation plugin to load at startup (e.g. \'xenos\'); empty disables "\n                      "GPU emulation")\n    .lifecycle(rex::cvar::Lifecycle::kInitOnly);\n\n// PARCHE LOCAL - selector de API grafica\n//\n// El plugin ya sabia elegir entre D3D12 y Vulkan por nombre; lo que faltaba\n// era que alguien se lo dijera. Ver plugin_main.cpp:\n//\n//     if (backend == "any" || backend == "d3d12")  -> D3D12GraphicsSystem\n//     if (backend == "any" || backend == "vulkan") -> VulkanGraphicsSystem\n//\n// "any" coge el primero que este compilado, que es D3D12. Con esto se puede\n// forzar uno concreto y comparar.\n//\n// PIDE REINICIO, PERO NO ES DE SOLO LECTURA. La primera version lo puse como\n// kInitOnly, igual que gpu_plugin, y el menu de F4 lo pintaba en rojo y\n// deshabilitado: se veia pero no se podia tocar. Es que kInitOnly significa\n// "no se puede ni guardar el valor nuevo", y aqui si se puede: lo que no se\n// puede es aplicarlo sin reiniciar. Eso es kRequiresRestart, que ademas hace\n// que el SDK lo apunte en su lista de cambios pendientes -y el menu la\n// ensena, con un boton para reiniciar-.\n//\n// Y ya no hay "any". Con any no se sabia cual estaba puesta de verdad, que era\n// justo lo que habia que ensenar. Con dos opciones y ambas explicitas, el\n// ajuste ES la respuesta a "cual se esta usando".\n//\n// El texto va en ingles porque es lo que sale en esa ventana, que es del SDK\n// y esta entera en ingles.\nREXCVAR_DEFINE_STRING(gpu_backend, "d3d12", "GPU",\n                      "Graphics API: d3d12 or vulkan. Takes effect on restart. If the one "\n                      "you pick was not built into this copy, the game falls back to the "\n                      "other one and says so in the log.")\n    .allowed({"d3d12", "vulkan"})\n    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);\n\n// PARCHE LOCAL - selector de API grafica\n//\n// La API que se acabo usando de verdad, para que el menu de F4 la pueda\n// ensenar. Casi siempre es la que dice el cvar, pero si esa no estaba\n// compilada se arranca con la otra, y entonces el cvar miente.\n//\n// Vive aqui y no en un sitio mas elegante porque rex_app.cpp y el menu de\n// ajustes se compilan en la misma biblioteca; una variable suelta y una\n// declaracion extern bastan, sin cabeceras nuevas ni ABI que mantener.\nnamespace rex::ui {\nstd::string g_gpu_backend_en_uso = "?";\n}  // namespace rex::ui\n'

VIEJA_CARGA_V2 = '  if (!config_.graphics && !config_.gpu_plugin.empty()) {\n    // PARCHE LOCAL - selector de API grafica\n    //\n    // Antes se llamaba con un solo argumento, asi que el backend quedaba en\n    // "any" por defecto y siempre salia D3D12 por ser el primero del if. El\n    // segundo parametro ya existia en LoadGpuPlugin; solo faltaba usarlo.\n    const std::string backend_elegido = REXCVAR_GET(gpu_backend);\n    REXLOG_INFO("API grafica pedida: {}", backend_elegido);\n    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin, backend_elegido);\n    rex::ui::g_gpu_backend_en_uso = backend_elegido;\n\n    // Si la que se ha pedido no esta compilada en esta copia, el plugin\n    // devuelve nada. Antes de eso significaba pantalla de error y a editar el\n    // toml a mano; ahora se cae a la otra y se dice bien claro. Como solo hay\n    // dos, "any" es exactamente la otra.\n    if (!config_.graphics) {\n      REXLOG_WARN("La API grafica \'{}\' no esta compilada en esta copia. Se prueba con la otra.",\n                  backend_elegido);\n      config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin, "any");\n      if (config_.graphics) {\n        rex::ui::g_gpu_backend_en_uso = (backend_elegido == "vulkan") ? "d3d12" : "vulkan";\n        REXLOG_WARN("Arrancando con \'{}\' en su lugar. Para quitar el aviso, cambia gpu_backend.",\n                    rex::ui::g_gpu_backend_en_uso);\n      }\n    }\n'

# v3 was already the good one -no shared variable, with fallback-, but it was
# missing clearing the pending-restart list, so the F4 menu opened with a false
# warning as soon as the launcher passed --gpu_backend. It is v4 without that
# last line, that is, an EXACT PREFIX of v4: the case that forced changing the
# rule in quitar_version_vieja.
#
# It is trimmed from the current block instead of copied by hand, which is
# where typos slip in. The cut goes JUST BEFORE the newline that opens the
# separating blank line, so what remains ends in "    }\n", which is exactly
# how v3 ended. One extra "\n" here and the block is never found, because in
# the file right after that brace comes "    if (!config_.graphics) {",
# with no blank line.
VIEJA_CARGA_V3 = CARGA_NUEVO[:CARGA_NUEVO.index(
    "\n\n    // Y aqui se borra la lista") + 1]

# FROM NEWEST TO OLDEST. v2 is v1 with things added, so looking at v1 first
# would take only its part and leave v2's tail loose.
VIEJOS = [
    # (name, fingerprint that ONLY appears in that version, whole block, anchor)
    ("v3 load block (without clearing pending restarts)",
     'const char* la_otra = (backend_elegido == "vulkan") ? "d3d12" : "vulkan";',
     VIEJA_CARGA_V3, CARGA_ANCLA),
    ("v2 cvar (shared variable)",
     'std::string g_gpu_backend_en_uso = "?";', VIEJO_CVAR_V2, CVAR_ANCLA),
    ("v2 load block (shared variable)",
     'rex::ui::g_gpu_backend_en_uso = backend_elegido;', VIEJA_CARGA_V2, CARGA_ANCLA),
    ("v1 cvar (any / read-only)",
     '.allowed({"any", "d3d12", "vulkan"})', VIEJO_CVAR, CVAR_ANCLA),
    ("v1 load block (no fallback)",
     'REXLOG_INFO("API grafica pedida: {}", backend_elegido);', VIEJA_CARGA, CARGA_ANCLA),
]


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "src" / "ui" / "rex_app.cpp").exists():
            return cand
    sys.exit("[ERROR] Cannot find the SDK's src/ui/rex_app.cpp.\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def quitar_version_vieja(txt):
    """Removes the remains of a previous version of this same patch.

    THE PROBLEM, WHICH COST ME THREE ATTEMPTS
    -----------------------------------------
    An old block and the current one can overlap in two ways, and each one
    breaks the obvious solution to the other:

      * THE OLD ONE IS A PIECE OF THE CURRENT ONE (code was added to the
        block). Searching for the old one finds it INSIDE the good one, and
        replacing it with the anchor cuts the head off the freshly placed
        block. Then applying again leaves the tail DUPLICATED. The file grew
        on every pass.

      * THE CURRENT ONE IS A PIECE OF THE OLD ONE (code was removed from the
        block). Then "the good block is there" says yes even though what is
        present is still the whole old one, and the script considers itself
        applied while leaving dead code inside.

    I tried to solve it with a FINGERPRINT per version -a piece that only
    existed in that version-. It does not always exist: when the old one is an
    exact prefix of the new one, EVERYTHING in the old one is also in the new
    one.

    THE RULE THAT DOES WORK, AND NEEDS NO FINGERPRINTS
    --------------------------------------------------
    Finding the old block only counts if it CANNOT be the good one seen
    halfway:

        es_de_verdad_vieja = (viejo in txt) and
                             (viejo not in nuevo or nuevo not in txt)

    Both cases above come out right with that, and it is checked with the
    texts alone, without me having to guess any fingerprint by hand.

    VIEJOS still goes FROM NEWEST TO OLDEST, and as soon as one version fits
    an anchor the rest for that anchor are skipped: if v2 is v1 with things
    added, looking at v1 first would orphan v2's tail. That happened too.

    And this is tested by running the patch TWICE in a row on the real file
    and comparing. The duplication bug does not show on the first pass, which
    is the only one usually looked at.
    """
    ahora = {ancla: nuevo for _, ancla, nuevo in BLOQUES}
    quitados = 0
    anclajes_hechos = set()
    for nombre, huella, viejo, ancla in VIEJOS:
        if ancla in anclajes_hechos:
            continue
        nuevo = ahora[ancla]
        if viejo not in txt:
            # The fingerprint is only used to warn: if a piece of that version
            # shows up but the whole block does not fit, someone edited it by
            # hand and I prefer not to guess.
            if huella in txt and nuevo not in txt:
                print(f"[aviso] I see remains of '{nombre}' but not in the form I expected.")
                print(f"        Leaving it alone; look at it by hand if something seems off.")
            continue
        if viejo in nuevo and nuevo in txt:
            # It is not an old version: it is the current block, which
            # contains the old one inside. This anchor is already up to date.
            anclajes_hechos.add(ancla)
            continue
        txt = txt.replace(viejo, ancla)
        anclajes_hechos.add(ancla)
        print(f"[ok] Removed previous version: {nombre}")
        quitados += 1
    return txt, quitados


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    f = localizar_sdk() / "src" / "ui" / "rex_app.cpp"
    txt = f.read_text(encoding="utf-8")

    if args.estado:
        puestos = sum(1 for _, _, nuevo in BLOQUES if nuevo in txt)
        print(f"  {f.name:26s} {puestos} of {len(BLOQUES)} blocks applied")
        for nombre, _, nuevo in BLOQUES:
            print(f"      {'yes' if nuevo in txt else 'NO':>2}  {nombre}")
        # With the same rule the migration uses, so --estado does not warn
        # about remains that are actually pieces of the good block.
        ahora = {ancla: nuevo for _, ancla, nuevo in BLOQUES}
        viejos = sum(1 for _, _, viejo, ancla in VIEJOS
                     if viejo in txt
                     and (viejo not in ahora[ancla] or ahora[ancla] not in txt))
        if viejos:
            print(f"      -- {viejos} blocks of the previous version remain")
        return 0

    if args.revertir:
        quitados = 0
        for nombre, ancla, nuevo in BLOQUES:
            if nuevo not in txt:
                continue
            if txt.count(nuevo) != 1:
                sys.exit(f"[ERROR] The block '{nombre}' appears {txt.count(nuevo)} times.\n"
                         f"        I am not touching it, remove it yourself.")
            txt = txt.replace(nuevo, ancla)
            quitados += 1
        txt, viejos = quitar_version_vieja(txt)
        quitados += viejos
        if not quitados:
            print(f"[ok] {f.name}: there was nothing applied")
            return 0
        f.write_text(txt, encoding="utf-8")
        print(f"[ok] Removed {quitados} blocks from {f.name}")
        print()
        print("  THE SDK MUST BE RECOMPILED.")
        return 0

    txt, _ = quitar_version_vieja(txt)

    faltan = [(n, a, v) for n, a, v in BLOQUES if v not in txt]
    if not faltan:
        print(f"[ok] {f.name}: all {len(BLOQUES)} blocks were already there")
        return 0

    for nombre, ancla, _ in faltan:
        n = txt.count(ancla)
        if n != 1:
            sys.exit(f"[ERROR] The anchor for '{nombre}' appears {n} times, expected 1.\n"
                     f"        The SDK must have changed. I have not touched anything.")

    for nombre, ancla, nuevo in faltan:
        txt = txt.replace(ancla, nuevo)
        print(f"[ok] Applied: {nombre}")
    f.write_text(txt, encoding="utf-8")
    print()
    print("  In F4, GPU category, setting  gpu_backend  (d3d12 / vulkan)")
    print("  It asks for a game restart to take effect.")
    print()
    print("  THE SDK MUST BE RECOMPILED, and with Vulkan enabled:")
    print("    cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
