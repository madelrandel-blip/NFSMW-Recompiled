#!/usr/bin/env python3
"""
Adds an in-game speed setting, as a percentage, movable from F4.

    python tools/parche_velocidad.py            apply
    python tools/parche_velocidad.py --estado
    python tools/parche_velocidad.py --revertir

It touches one SDK file:  src/system/runtime.cpp

It does not keep a .original and does not need one: it applies and undoes by
exact text replacement, block by block. That is on purpose: runtime.cpp ALREADY
carries another patch on top, and keeping a ".original" there at this point
would save the already patched file as if it were the clean one; incidentally,
a --revertir would wipe out the other patch.

And it goes block by block instead of with a global mark BECAUSE OF A BUG THAT
ALREADY HAPPENED in the sibling patch: with a single mark, changing the patch
content was useless -it found the mark from the previous version, said "already
applied" and touched nothing-. Here there is also migration: if it detects the
old version of the patch itself, it removes it before putting in the new one.


FIRST, THE QUESTION: DO ANIMATIONS FOLLOW THE FPS?
==================================================

No. And it can be checked by reading the SDK, in src/graphics/graphics_system.cpp:

    // Guest vblank timer based on the configured guest video mode.
    ...
    double refresh_rate_hz = video_mode.refresh_rate;         // 60 por defecto
    uint64_t vsync_interval_ticks = guest_tick_frequency / refresh_rate_hz;
    while (vsync_worker_running_) {
      uint64_t current_time = Clock::QueryGuestTickCount();
      while (current_time - last_frame_time >= interval_ticks) {
        MarkVblank();
        last_frame_time += interval_ticks;
      }
      Sleep(1ms);
    }

The vertical blank -the heartbeat by which the game measures time- is generated
by a SEPARATE THREAD with a wall clock, not by the drawing loop. At 18 fps the
game still receives its 60 notifications per second; all that happens is that
fewer frames are drawn. Limiting to 30 fps does NOT slow the game down.

A TRAP THAT DOES MATTER, AND IT IS IN THE SAME FUNCTION:

    uint64_t no_vsync_interval_ticks = guest_tick_frequency / 1000;
    interval_ticks = vsync ? vsync_interval_ticks : no_vsync_interval_ticks;

With vsync OFF the notification goes to 1000 per second instead of 60. That is
on purpose -so the game does not sit waiting for the blank-, but if the game
counted time by those notifications instead of by the clock, with vsync off it
would run wild. It is worth looking at, because we are doing the performance
tests exactly that way.


WHAT THIS PATCH ADDS
====================

A cvar  game_speed  AS A PERCENTAGE: 100 is normal, 50 half, 200 double.

As a percentage and not a multiplier because in the F4 window a bare number
shows up and "1.0" does not say what it is; typing "100" there was natural, and
with the multiplier range -0.05 to 4.0- that got clipped to the maximum and the
setting stayed nailed at 4. Also 0..200 is a comfortable range for a bar.

It does not invent it: the SDK already brings Clock::set_guest_time_scalar(),
which scales the whole guest clock -the tick counter, system time, timers and
waits-. It was fixed at 1.0 with no way to touch it. Since the blank thread
compares guest ticks, that one also adjusts itself: there is nothing that can
get out of sync.

And it can be moved on the fly. The SDK has RegisterChangeCallback for that -it
already uses it for fullscreen mode-, so as soon as you move the bar in F4 it
is applied, without restarting.

WATCH OUT FOR WHAT IT IS AND WHAT IT IS NOT:

  - limiting the fps  = how many times it DRAWS per second
  - game_speed        = how fast TIME PASSES inside the game

They are different things. This does not give performance: lowering it makes
the game go in slow motion, not smoother.
"""

import argparse
import pathlib
import sys

# ---------------------------------------------------------------------------
#  1) Headers
# ---------------------------------------------------------------------------

CAB_ANCLA = """#include <rex/chrono/clock.h>
#include <rex/cvar.h>
"""

CAB_NUEVO = """#include <algorithm>  // PARCHE LOCAL - std::max, para el suelo de la velocidad

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
"""

# ---------------------------------------------------------------------------
#  2) The cvar and the function that applies it
# ---------------------------------------------------------------------------

CVAR_ANCLA = """REXCVAR_DEFINE_STRING(metadata_root, "", "Runtime", "Override metadata path");
"""

CVAR_NUEVO = """REXCVAR_DEFINE_STRING(metadata_root, "", "Runtime", "Override metadata path");

// PARCHE LOCAL - velocidad del juego ajustable
//
// Velocidad a la que pasa el tiempo DENTRO del juego, EN PORCENTAJE: 100 es
// normal, 50 la mitad, 200 el doble. No tiene nada que ver con el limite de
// fps: los fps son cuantas veces se dibuja, esto es a que ritmo avanza.
//
// En porcentaje y no en multiplicador porque en la ventana de F4 sale un
// numero pelado, y "1.0" no dice de que. Ademas 0..200 es un recorrido comodo
// para una barra; 0.05..4.0 se apelotonaba todo a la izquierda.
//
// El texto va en ingles porque es lo que sale en esa ventana, que es del SDK
// y esta entera en ingles.
REXCVAR_DEFINE_DOUBLE(game_speed, 100.0, "Runtime",
                      "Speed of in-game time, as a percentage. 100 = normal, 50 = half, "
                      "200 = double. This is not an fps limit: it changes how fast time "
                      "passes in the game, not how often it is drawn.")
    .range(0.0, 200.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// PARCHE LOCAL - velocidad del juego ajustable
//
// Pasa el porcentaje a la escala que espera el reloj, con un SUELO. El suelo
// no es capricho: en src/core/clock.cpp, RecomputeGuestTickScalar hace esto
// cuando la escala es <= 1.0
//
//     frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
//
// Con la escala a cero eso es 10.0/0.0 = infinito, y convertir infinito a
// uint64_t es comportamiento indefinido; en la practica sale 0 o un numero
// enorme. Si sale 0, la division de UpdateGuestClock revienta. O sea que un
// 0% literal no cuelga el juego: lo tumba.
//
// Asi que el 0% de la barra se queda en una milesima de la velocidad normal.
// A ese ritmo el juego esta parado a todos los efectos -un segundo suyo son
// mas de dieciseis minutos- pero el reloj sigue siendo un numero valido.
void aplicar_velocidad_del_juego() {
  const double por_ciento = REXCVAR_GET(game_speed);
  const double escala = std::max(por_ciento, 0.1) / 100.0;
  rex::chrono::Clock::set_guest_time_scalar(escala);
  REXSYS_INFO("[velocidad] el tiempo del juego pasa al {:.0f}% (escala x{:.3f})", por_ciento,
              escala);
}

}  // namespace
"""

# ---------------------------------------------------------------------------
#  3) Apply it at startup, and hook it to hot changes
# ---------------------------------------------------------------------------

RELOJ_ANCLA = """  chrono::Clock::set_guest_tick_frequency(50000000);
  chrono::Clock::set_guest_system_time_base(chrono::Clock::QueryHostSystemTime());
  chrono::Clock::set_guest_time_scalar(1.0);
"""

RELOJ_NUEVO = """  chrono::Clock::set_guest_tick_frequency(50000000);
  chrono::Clock::set_guest_system_time_base(chrono::Clock::QueryHostSystemTime());

  // PARCHE LOCAL - velocidad del juego ajustable
  //
  // set_guest_time_scalar escala el reloj del guest ENTERO: el contador de
  // ticks, la hora del sistema, los temporizadores y las esperas. Por eso vale
  // como mando de velocidad y no hace falta tocar nada mas: el hilo que genera
  // el parpadeo vertical compara ticks del guest, asi que se ajusta solo y no
  // queda nada desincronizado.
  //
  // Antes esto era un 1.0 fijo.
  aplicar_velocidad_del_juego();

  // Y para poder moverlo desde F4 sin reiniciar. El aviso llega DESPUES de que
  // el valor nuevo este puesto -asi lo hace SetFlagFromSource-, o sea que
  // REXCVAR_GET dentro ya devuelve el nuevo y no el anterior.
  rex::cvar::RegisterChangeCallback(
      "game_speed", [](std::string_view, std::string_view) { aplicar_velocidad_del_juego(); });
"""

BLOQUES = [
    ("cabeceras", CAB_ANCLA, CAB_NUEVO),
    ("el cvar y su funcion", CVAR_ANCLA, CVAR_NUEVO),
    ("arranque del reloj", RELOJ_ANCLA, RELOJ_NUEVO),
]

# ---------------------------------------------------------------------------
#  Previous version of THIS patch, so it can be migrated
#
#  v1 defined game_speed as a MULTIPLIER (1.0, range 0.05..4.0) and called the
#  clock directly, with no helper function. If it is still installed it must be
#  removed first, or the anchors above do not fit: its place is taken.
# ---------------------------------------------------------------------------

VIEJO_CVAR = """REXCVAR_DEFINE_STRING(metadata_root, "", "Runtime", "Override metadata path");

// PARCHE LOCAL - velocidad del juego ajustable
//
// Multiplica la velocidad a la que pasa el tiempo DENTRO del juego. No tiene
// nada que ver con el limite de fps: los fps son cuantas veces se dibuja, esto
// es a que ritmo avanza el juego.
//
// El texto va en ingles porque es lo que sale en la ventana de F4, que es del
// SDK y esta entera en ingles.
REXCVAR_DEFINE_DOUBLE(game_speed, 1.0, "Runtime",
                      "Speed of in-game time. 1.0 = normal, 0.5 = half, 2.0 = double. "
                      "This is not an fps limit: it changes how fast time passes in the "
                      "game, not how often it is drawn.")
    .range(0.05, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
"""

VIEJO_RELOJ = """  chrono::Clock::set_guest_tick_frequency(50000000);
  chrono::Clock::set_guest_system_time_base(chrono::Clock::QueryHostSystemTime());

  // PARCHE LOCAL - velocidad del juego ajustable
  //
  // set_guest_time_scalar escala el reloj del guest ENTERO: el contador de
  // ticks, la hora del sistema, los temporizadores y las esperas. Por eso vale
  // como mando de velocidad y no hace falta tocar nada mas: el hilo que genera
  // el parpadeo vertical compara ticks del guest, asi que se ajusta solo y no
  // queda nada desincronizado.
  //
  // Antes esto era un 1.0 fijo.
  chrono::Clock::set_guest_time_scalar(REXCVAR_GET(game_speed));

  // Y para poder moverlo desde F4 sin reiniciar. El aviso llega DESPUES de que
  // el valor nuevo este puesto -asi lo hace SetFlagFromSource-, o sea que
  // REXCVAR_GET aqui dentro ya devuelve el nuevo y no el anterior.
  rex::cvar::RegisterChangeCallback("game_speed", [](std::string_view, std::string_view) {
    const double escala = REXCVAR_GET(game_speed);
    chrono::Clock::set_guest_time_scalar(escala);
    REXSYS_INFO("[velocidad] el tiempo del juego pasa a x{:.2f}", escala);
  });
"""

VIEJOS = [
    # (name, fingerprint that ONLY appears in that version, whole block, anchor)
    ("v1 cvar (multiplier)",
     'REXCVAR_DEFINE_DOUBLE(game_speed, 1.0, "Runtime",', VIEJO_CVAR, CVAR_ANCLA),
    ("v1 clock (without helper function)",
     'chrono::Clock::set_guest_time_scalar(REXCVAR_GET(game_speed));', VIEJO_RELOJ, RELOJ_ANCLA),
]


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "src" / "system" / "runtime.cpp").exists():
            return cand
    sys.exit("[ERROR] Cannot find the SDK's src/system/runtime.cpp.\n"
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

    f = localizar_sdk() / "src" / "system" / "runtime.cpp"
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
                     f"        The SDK must have changed, or remains of a previous version\n"
                     f"        that I do not recognize are left. I have not touched anything.")

    for nombre, ancla, nuevo in faltan:
        txt = txt.replace(ancla, nuevo)
        print(f"[ok] Applied: {nombre}")
    f.write_text(txt, encoding="utf-8")
    print()
    print("  In F4, Runtime category, setting  game_speed  (0 to 200 %)")
    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
