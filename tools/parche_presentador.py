#!/usr/bin/env python3
"""
Makes VSYNC and the FPS LIMIT actually exist.

    python tools/parche_presentador.py            apply
    python tools/parche_presentador.py --estado
    python tools/parche_presentador.py --revertir

It touches a single SDK file:  src/ui/d3d12/d3d12_presenter.cpp
It keeps a .original on the first run and is idempotent.


WHY THIS WAS NEEDED
===================

VSYNC
-----
The "vsync" cvar exists, but it is NOT vsync. It is read in a single place in
the whole SDK, in command_processor.cpp, inside ExecutePacketType3_WAIT_REG_MEM:

    if (!REXCVAR_GET(vsync)) {
      // User wants it fast and dangerous.
      rex::thread::MaybeYield();
    } else {
      rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
    }

That is: it decides whether the command processor SLEEPS when the game's
command stream asks to wait, or keeps spinning. It is a "run wild", not a
synchronization with the screen.

The real synchronization is in the D3D12 presenter, and it was nailed down:

    swap_chain->Present(0, DXGI_PRESENT_RESTART | ...);

That first 0 is the SyncInterval. With 0 it always presents as soon as it can,
no matter what the cvar says. The SDK comment explains why it was chosen that
way -the monitor may run at 144 Hz, which is not a multiple of the guest's 30
or 60-, but the effect is that the vsync checkbox did nothing visible.

The patch passes SyncInterval 1 when vsync is enabled.

  A DETAIL THAT MATTERS: with SyncInterval not equal to 0, DXGI REJECTS the
  ALLOW_TEARING flag and returns DXGI_ERROR_INVALID_CALL. They are mutually
  exclusive. And DXGI_PRESENT_RESTART discards queued frames, which is just the
  opposite of what is wanted with vsync. That is why with vsync enabled neither
  is passed, and without vsync everything is left exactly as it was.

  The cvar is read BY NAME, with rex::cvar::Query<bool>("vsync"), not with
  REXCVAR_GET. That is on purpose: "vsync" is defined in the GPU plugin
  (rexgpu-xenos.dll) and the presenter lives in rexruntime.dll. Linking against
  a symbol from the plugin would not work; the cvar registry, on the other
  hand, is common and the lookup by name goes through it without a problem. It
  is checked first with GetFlagInfo in case the plugin were not loaded.

FPS LIMIT
---------
There was none. It was searched for in all the headers and in the symbols of
the compiled DLLs: only "vsync" is there. So a new cvar is added, max_fps,
defined right here in the presenter.

  0 = no limit (the usual behavior).

It sleeps until the next frame is due. It does not sleep all the way: it
leaves the last stretch spinning, because Sleep on Windows has a granularity of
between 1 and 15 ms and without that finish the limit falls short and stutters.
"""

import argparse
import pathlib
import shutil
import sys

MARCA = "PARCHE LOCAL - vsync real y limitador de fps"

# ---------------------------------------------------------------------------
#  El sitio exacto, copiado tal cual del fuente del SDK.
# ---------------------------------------------------------------------------
ANCLA = """  HRESULT present_result = paint_context_.swap_chain->Present(
      0, DXGI_PRESENT_RESTART |
             (paint_context_.swap_chain_allows_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0));
"""

NUEVO = """  // ------------------------------------------------------------------
  //  PARCHE LOCAL - vsync real y limitador de fps
  //
  //  Aqui antes habia un Present(0, ...) con el SyncInterval clavado a 0,
  //  asi que la sincronizacion con la pantalla no ocurria nunca por mucho
  //  que se activara el cvar "vsync" -que en realidad solo decide si el
  //  procesador de comandos duerme en las esperas del guest-.
  // ------------------------------------------------------------------

  // Limitador. max_fps = 0 deja el comportamiento original.
  {
    const int32_t tope = REXCVAR_GET(max_fps);
    if (tope > 0) {
      using Reloj = std::chrono::steady_clock;
      // Estatica de funcion: PaintAndPresentImpl corre siempre en el hilo de
      // pintado, asi que no hace falta sincronizar nada.
      static Reloj::time_point siguiente{};
      const auto periodo = std::chrono::duration_cast<Reloj::duration>(
          std::chrono::duration<double>(1.0 / double(tope)));
      const auto ahora = Reloj::now();
      if (siguiente > ahora) {
        // Dormir casi todo y rematar girando: Sleep tiene una granularidad
        // de 1 a 15 ms y sin el remate el limite se queda corto.
        const auto margen = std::chrono::milliseconds(2);
        if (siguiente - ahora > margen) {
          std::this_thread::sleep_for((siguiente - ahora) - margen);
        }
        while (Reloj::now() < siguiente) {
          std::this_thread::yield();
        }
      }
      siguiente = std::max(Reloj::now(), siguiente) + periodo;
    }
  }

  // Vsync. Se busca por nombre porque el cvar lo define el plugin de GPU, que
  // es otro DLL: enlazar contra su simbolo no funcionaria, pero el registro de
  // cvars es comun.
  bool con_vsync = false;
  if (rex::cvar::GetFlagInfo("vsync") != nullptr) {
    con_vsync = rex::cvar::Query<bool>("vsync");
  }

  UINT sync_interval = 0;
  UINT present_flags = 0;
  if (con_vsync) {
    sync_interval = 1;
    // Ni ALLOW_TEARING ni RESTART: la primera es incompatible con
    // SyncInterval != 0 (DXGI devuelve DXGI_ERROR_INVALID_CALL) y la segunda
    // descarta los fotogramas encolados, que es lo contrario de lo que se
    // busca al sincronizar.
  } else {
    present_flags = DXGI_PRESENT_RESTART |
                    (paint_context_.swap_chain_allows_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
  }

  HRESULT present_result = paint_context_.swap_chain->Present(sync_interval, present_flags);
"""

# El cvar nuevo y las cabeceras que necesita el codigo de arriba.
ANCLA_CVAR = """REXCVAR_DEFINE_BOOL(d3d12_allow_variable_refresh_rate_and_tearing, true, "UI/D3D12",
                    "Allow variable refresh rate and tearing");
"""

NUEVO_CVAR = """REXCVAR_DEFINE_BOOL(d3d12_allow_variable_refresh_rate_and_tearing, true, "UI/D3D12",
                    "Allow variable refresh rate and tearing");

// PARCHE LOCAL - vsync real y limitador de fps
// El SDK no traia ningun limitador: solo estaba "vsync", y ese ni siquiera
// tocaba el SyncInterval del Present. Este es nuevo.
REXCVAR_DEFINE_INT32(max_fps, 0, "UI/Present",
                     "Limite de fotogramas por segundo (0 = sin limite)")
    .range(0, 1000);
"""

ANCLA_INC = """#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <utility>
"""

NUEVO_INC = """#include <algorithm>
#include <chrono>   // PARCHE LOCAL - limitador de fps
#include <climits>
#include <cmath>
#include <memory>
#include <thread>   // PARCHE LOCAL - limitador de fps
#include <utility>
"""


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        f = cand / "src" / "ui" / "d3d12" / "d3d12_presenter.cpp"
        if f.exists():
            return f
    sys.exit("[ERROR] Cannot find the SDK's src/ui/d3d12/d3d12_presenter.cpp.\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    f = localizar_sdk()
    original = f.with_suffix(".cpp.original")
    txt = f.read_text(encoding="utf-8")
    puesto = MARCA in txt

    if args.estado:
        print(f"  {f}")
        print("  Patch:", "APPLIED" if puesto else "not applied")
        return 0

    if args.revertir:
        if original.exists():
            shutil.copy2(original, f)
            print("[ok] Restored from .original")
        else:
            print("[aviso] There is no .original to restore.")
        return 0

    if puesto:
        print("[ok] It was already applied. I am touching nothing.")
        return 0

    # Check the three anchors BEFORE writing anything. If the SDK changes
    # version and one does not fit, better not leave the file half done.
    for nombre, ancla in [("includes", ANCLA_INC),
                          ("cvar definitions", ANCLA_CVAR),
                          ("Present call", ANCLA)]:
        n = txt.count(ancla)
        if n != 1:
            sys.exit(f"[ERROR] The anchor '{nombre}' appears {n} times, expected 1.\n"
                     f"        The SDK must have changed. I have not touched anything.")

    if not original.exists():
        shutil.copy2(f, original)
        print(f"[ok] Backup: {original.name}")

    txt = txt.replace(ANCLA_INC, NUEVO_INC)
    txt = txt.replace(ANCLA_CVAR, NUEVO_CVAR)
    txt = txt.replace(ANCLA, NUEVO)
    f.write_text(txt, encoding="utf-8")

    print("[ok] Patch applied.")
    print()
    print("  vsync    now passes SyncInterval 1 to Present")
    print("  max_fps  new cvar, 0 = no limit")
    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
