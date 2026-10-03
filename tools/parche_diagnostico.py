#!/usr/bin/env python3
"""
Two SDK patches about threads and memory.  (version 3)

    python tools/parche_diagnostico.py            apply
    python tools/parche_diagnostico.py --estado
    python tools/parche_diagnostico.py --revertir

It touches two SDK files:
    src/system/xmemory.cpp    the access violation message
    src/system/xthread.cpp    the startup of each guest thread

It keeps a .original of each on the first run and is idempotent. If it detects
an OLDER version of this same patch, it reverts it before applying the new
one: that way it can be reapplied on top without piling up layers.


WHAT IS ALREADY KNOWN, AND WHY VERSION 2 IS NEEDED
==================================================

Version 1 answered the first question. The crash log went from this:

    Unhandled guest access violation: read of guest 0x00000000
      on thread 0xF8000028

to this:

    [hilo guest] arrancando: entrada=0x8262E768 start_address=0x8262E768
                 contexto=0x00000000 trampolin_xapi=0x00000000 pila=262144
    [hilo guest] id=0x6 entrada=0x8262E768 principal=true
                 creado_por_el_juego=true
    [contexto ppc] lr=0x00000000 ultimo_salto_indirecto=0x00000000
                   r1=0x70190000 r3=0 r4=0 r5=0 ...

And that, in 20 runs out of 20, always the same. What it says:

  - 0x8262E768 is NOT a secondary thread: it is the GAME'S ENTRY POINT.
    principal=true, and start_address comes from the XEX header, not from any
    heuristic. A second thread never gets to start.
  - lr=0 and ultimo_salto_indirecto=0: it dies in the first instructions,
    before calling anything.
  - r1 is a valid stack (0x70190000, inside 0x70000000-0x7F000000).

And what points to the cause: the TWO addresses that fail are 0x00000000 and
0x00000100. They are not arbitrary numbers. In the SDK's own xthread.cpp,
right above where the PCR is reserved, is the map:

    // 0x000: pointer to tls data
    // 0x100: pointer to TEB(?)

So the two reads that blow up are 0(r13) and 0x100(r13) with r13 equal to
ZERO. r13 is the pointer to the PCR, and ThreadState sets it on construction:

    context_->r13.u64 = pcr_address;   // src/system/thread_state.cpp

The hypothesis, then, is that r13 reaches zero in the recompiled code. What is
NOT known is whether it reaches zero because pcr_address_ is zero -in which
case the fault is in the guest memory reservation- or because the context gets
lost between thread creation and the call -in which case the fault is
somewhere else-.

A single register separates the two stories. That is why this is version 2.


WHAT VERSION 2 ADDS
===================

1. r13 in the register dump, along with r2, r12 and r30. And also pcr_ptr()
   and tls_ptr() read from XThread itself, which is the value the SDK
   BELIEVES it has set. If pcr_ptr() brings a value and r13 is zero, the
   context is getting lost along the way. If both are zero, the PCR
   reservation failed silently.

2. The "arrancando" line now also carries r13, the PCR and the TLS. This is
   the important part: that line ALWAYS comes out, whether startup goes well
   or not. That way a good startup can be compared directly with a bad one
   -same line, two machines- instead of looking only at the failing one.

3. If the PCR is not zero, the two guest words at PCR+0x00 and PCR+0x100 are
   dumped, which are exactly the ones the game tries to read.


WHAT VERSION 3 ADDS, AND IT IS NOT DIAGNOSTICS BUT A FIX
========================================================

Reading the log of a real play session this appeared:

    44.803 lineas en 44 segundos, TODAS la misma:
        [warning] [sys] Too few processor cores - scheduling will be wonky

100% of the log. A thousand warnings per second, 105 MB in a quarter of an
hour.

It comes from XThread::SetActiveCpu, which the game calls every time it creates
a thread or changes its CPU, and the SDK wrote it ONCE PER CALL.

And it is not just annoying noise. Each line is a format, a lock and a write to
disk, done from a game thread. On a two-core machine -which is exactly the only
case in which that branch runs- that steals the CPU from everything else. The
symptom that uncovered it: audio cut out seconds after leaving the garage and
the game turned into a mud pit.

So the warning that said "scheduling is going to go badly here" was, itself, an
important part of why it went badly.

Now it comes out once per run. The original text is preserved whole in case
something searches for it.
"""

import argparse
import pathlib
import shutil
import sys

MARCA = "PARCHE LOCAL - diagnostico y ruido v3"

# Marks of previous versions. If any appears, it is reverted before applying
# the new one: reapplying on top would not find the anchors -they are already
# rewritten- and the script would stop saying the SDK has changed.
MARCAS_VIEJAS = [
    "PARCHE LOCAL - diagnostico del hilo que revienta v2",
    "PARCHE LOCAL - diagnostico del hilo que revienta",
]

# ---------------------------------------------------------------------------
#  1. src/system/xmemory.cpp
# ---------------------------------------------------------------------------

MEM_ANCLA_INC = """#include <rex/system/xmemory.h>
#include <rex/thread.h>
"""

MEM_NUEVO_INC = """#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>  // PARCHE LOCAL - diagnostico y ruido v3
#include <rex/thread.h>
"""

MEM_ANCLA = """    REXSYS_ERROR(
        "Unhandled guest access violation: {} of guest 0x{:08X} (host 0x{:016X}) on thread 0x{:X}",
        is_write ? "write" : "read", virtual_address, reinterpret_cast<uintptr_t>(host_address),
        rex::thread::current_thread_id());
    return false;
"""

MEM_NUEVO = """    REXSYS_ERROR(
        "Unhandled guest access violation: {} of guest 0x{:08X} (host 0x{:016X}) on thread 0x{:X}",
        is_write ? "write" : "read", virtual_address, reinterpret_cast<uintptr_t>(host_address),
        rex::thread::current_thread_id());

    // ============ PARCHE LOCAL - diagnostico y ruido v3 ======
    //
    // El mensaje de arriba se deja TAL CUAL: tools\\\\matriz.ps1 lo busca por
    // texto para clasificar los intentos. Aqui solo se anade debajo quien ha
    // muerto y con que valores.
    //
    // Se llama a REXSYS_ERROR igual que arriba, asi que no se introduce
    // ninguna forma nueva de bloquearse: este camino ya estaba logueando.
    {
      // IsInThread() antes de GetCurrentThread(): el segundo dispara un
      // assert_always si no hay hilo del kernel enlazado, y aqui es un caso
      // perfectamente posible -y ademas informativo-.
      auto* hilo =
          rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread() : nullptr;
      if (!hilo) {
        REXSYS_ERROR(
            "  [hilo guest] no hay XThread en este hilo: el fallo NO viene de "
            "codigo del juego, sino del propio runtime.");
      } else {
        const auto* cp = hilo->creation_params();
        REXSYS_ERROR(
            "  [hilo guest] id=0x{:X} entrada=0x{:08X} contexto=0x{:08X} "
            "trampolin_xapi=0x{:08X} principal={} creado_por_el_juego={}",
            hilo->thread_id(), cp->start_address, cp->start_context,
            cp->xapi_thread_startup, hilo->main_thread(), hilo->is_guest_thread());

        // Lo que el SDK CREE haber reservado para este hilo. Comparado con
        // r13 mas abajo, esto separa "la reserva fallo" de "el contexto se
        // perdio por el camino".
        REXSYS_ERROR("  [bloques del hilo] pcr=0x{:08X} tls=0x{:08X}", hilo->pcr_ptr(),
                     hilo->tls_ptr());

        auto* estado = hilo->thread_state();
        if (estado && estado->context()) {
          const auto& c = *estado->context();
          // r13 ES EL REGISTRO CLAVE: apunta al PCR, y ThreadState lo pone al
          // construirse. Las dos direcciones que revientan -0x00000000 y
          // 0x00000100- son exactamente los desplazamientos que el propio
          // xthread.cpp documenta dentro del PCR: el puntero al TLS y el
          // puntero al TEB. O sea que un r13 a cero explica el crash entero.
          //
          // lr = a donde volveria la funcion actual, o sea QUIEN llamo.
          // last_indirect_target lo mantiene el SDK aunque ctr se haya
          // optimizado a variable local (REX_CONFIG_CTR_AS_LOCAL).
          REXSYS_ERROR(
              "  [contexto ppc] r13=0x{:08X} r2=0x{:08X} lr=0x{:08X} "
              "ultimo_salto_indirecto=0x{:08X} r1=0x{:08X}",
              c.r13.u32, c.r2.u32, static_cast<uint32_t>(c.lr), c.last_indirect_target, c.r1.u32);
          REXSYS_ERROR(
              "  [registros] r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r6=0x{:08X} "
              "r7=0x{:08X} r11=0x{:08X} r12=0x{:08X} r30=0x{:08X} r31=0x{:08X}",
              c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r11.u32, c.r12.u32, c.r30.u32,
              c.r31.u32);
        }

        // Y si el PCR SI existe, que dicen las dos palabras que el juego
        // intenta leer. Si el PCR es valido y estan a cero, el problema es su
        // contenido; si r13 es cero, ni siquiera se llega a mirarlas.
        const uint32_t pcr = hilo->pcr_ptr();
        if (pcr) {
          auto* p0 = TranslateVirtual<const uint8_t*>(pcr);
          auto* p100 = TranslateVirtual<const uint8_t*>(pcr + 0x100);
          if (p0 && p100) {
            // Cualificado del todo: este fichero ya vive dentro de
            // namespace rex::memory, asi que sin cualificar tambien valdria,
            // pero escrito entero es el mismo nombre que usa xthread.cpp y no
            // depende de donde acabe cayendo el bloque si el SDK se reordena.
            REXSYS_ERROR("  [contenido del pcr] +0x000={:08X} +0x100={:08X}",
                         rex::memory::load_and_swap<uint32_t>(p0),
                         rex::memory::load_and_swap<uint32_t>(p100));
          }
        }
      }
    }
    // ======================= fin del parche local ==========================

    return false;
"""

# ---------------------------------------------------------------------------
#  2. src/system/xthread.cpp
# ---------------------------------------------------------------------------

RUIDO_ANCLA = """  } else {
    REXSYS_WARN("Too few processor cores - scheduling will be wonky");
  }
"""

RUIDO_NUEVO = """  } else {
    // PARCHE LOCAL - diagnostico y ruido v3
    //
    // Este aviso salia UNA VEZ POR LLAMADA, y SetActiveCpu se llama cada vez
    // que el juego crea un hilo o le cambia la CPU. Medido en una partida
    // real: 44.803 avisos en 44 segundos -unos mil por segundo-, y 105 MB de
    // log en un cuarto de hora. El 100% de las lineas del log eran esta.
    //
    // No es solo ruido. Cada linea es un formateo, un cerrojo y una escritura
    // a disco, hecha DESDE UN HILO DEL JUEGO. En un equipo de dos nucleos
    // -que es justo el caso en el que esta rama se ejecuta- eso le roba la
    // CPU al resto: el audio se corta y el juego se vuelve un barrizal.
    //
    // O sea que el aviso que dice "aqui va a ir mal" era, el mismo, una parte
    // importante de por que iba mal.
    //
    // Se deja una sola vez por proceso. El texto original se conserva entero
    // para no romper nada que lo busque.
    static std::atomic_flag avisado = ATOMIC_FLAG_INIT;
    if (!avisado.test_and_set(std::memory_order_relaxed)) {
      REXSYS_WARN("Too few processor cores - scheduling will be wonky"
                  " (este aviso solo se muestra una vez por ejecucion)");
    }
  }
"""

HILO_ANCLA = """  auto* dispatcher = runtime->function_dispatcher();
  auto* memory = runtime->memory();
  PPCFunc* func = dispatcher->GetFunction(address);
"""

HILO_NUEVO = """  auto* dispatcher = runtime->function_dispatcher();
  auto* memory = runtime->memory();

  // PARCHE LOCAL - diagnostico y ruido v3
  //
  // Una linea por hilo del guest que arranca. Los puntos de entrada de los
  // hilos son INVISIBLES para el analisis estatico -la direccion se pasa como
  // parametro a ExCreateThread, no aparece como destino de ningun salto-, asi
  // que sin esto no hay forma de saber cuantos hilos crea el juego, en que
  // orden ni cual es el que no vuelve.
  //
  // Y lleva r13, el PCR y el TLS a proposito: esta linea sale SIEMPRE, tanto
  // si el arranque va bien como si no. Es la unica forma de comparar un
  // arranque bueno con uno malo mirando exactamente lo mismo en las dos
  // maquinas, en vez de mirar solo la que falla.
  //
  // Nivel info y una sola linea por hilo: no cambia el timing de forma
  // apreciable, que es justo lo que hay que cuidar cuando se persigue algo
  // que solo pasa en maquinas rapidas.
  {
    auto* ctx_log = thread_state_ ? thread_state_->context() : nullptr;
    REXSYS_INFO(
        "[hilo guest] arrancando: entrada=0x{:08X} start_address=0x{:08X} "
        "contexto=0x{:08X} trampolin_xapi=0x{:08X} pila={} bytes | "
        "pcr=0x{:08X} tls=0x{:08X} r13=0x{:08X} r1=0x{:08X}",
        address, creation_params_.start_address, creation_params_.start_context,
        creation_params_.xapi_thread_startup, creation_params_.stack_size, pcr_address_,
        tls_static_address_, ctx_log ? ctx_log->r13.u32 : 0u, ctx_log ? ctx_log->r1.u32 : 0u);
  }

  PPCFunc* func = dispatcher->GetFunction(address);
"""


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "src" / "system" / "xmemory.cpp").exists():
            return cand
    sys.exit("[ERROR] Cannot find the SDK (src/system/xmemory.cpp).\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    sdk = localizar_sdk()
    f_mem = sdk / "src" / "system" / "xmemory.cpp"
    f_hilo = sdk / "src" / "system" / "xthread.cpp"

    trabajos = [
        (f_mem, [("includes", MEM_ANCLA_INC, MEM_NUEVO_INC),
                 ("access violation message", MEM_ANCLA, MEM_NUEVO)]),
        (f_hilo, [("guest thread startup", HILO_ANCLA, HILO_NUEVO),
                  ("too few cores warning", RUIDO_ANCLA, RUIDO_NUEVO)]),
    ]

    if args.estado:
        for f, _ in trabajos:
            t = f.read_text(encoding="utf-8")
            if MARCA in t:
                estado = "v2 APPLIED"
            elif any(m in t for m in MARCAS_VIEJAS):
                estado = "v1 applied (reapply needed to move to v2)"
            else:
                estado = "not applied"
            print(f"  {f.name:16s}  {estado}")
        return 0

    if args.revertir:
        for f, _ in trabajos:
            original = f.with_suffix(".cpp.original")
            if original.exists():
                shutil.copy2(original, f)
                print(f"[ok] Restored {f.name} from .original")
            else:
                print(f"[aviso] There is no .original for {f.name}.")
        return 0

    # If an older version is installed, it is removed first. Reapplying on top
    # would never find the anchors -they are already rewritten- and the script
    # would stop saying the SDK has changed, which is a misleading message.
    for f, _ in trabajos:
        t = f.read_text(encoding="utf-8")
        if MARCA not in t and any(m in t for m in MARCAS_VIEJAS):
            original = f.with_suffix(".cpp.original")
            if not original.exists():
                sys.exit(f"[ERROR] {f.name} has a previous version of the patch but there\n"
                         f"        is no .original to undo it. I am stopping: restore that\n"
                         f"        file from the SDK repository and run me again.")
            shutil.copy2(original, f)
            print(f"[ok] Removed the previous version of {f.name}")

    # Check ALL anchors of ALL files before writing anything. If the SDK
    # changes version and one does not fit, it is much worse to leave one file
    # patched and another not than to patch none.
    planes = []
    for f, anclas in trabajos:
        txt = f.read_text(encoding="utf-8")
        if MARCA in txt:
            print(f"[ok] {f.name} was already on v2.")
            planes.append((f, txt, None))
            continue
        for nombre, ancla, _ in anclas:
            n = txt.count(ancla)
            if n != 1:
                sys.exit(f"[ERROR] In {f.name}, the anchor '{nombre}' appears {n} times,\n"
                         f"        expected 1. The SDK must have changed. I have not touched anything.")
        planes.append((f, txt, anclas))

    if all(a is None for _, _, a in planes):
        print("[ok] Everything was already applied. I am touching nothing.")
        return 0

    for f, txt, anclas in planes:
        if anclas is None:
            continue
        original = f.with_suffix(".cpp.original")
        if not original.exists():
            shutil.copy2(f, original)
            print(f"[ok] Backup: {original.name}")
        for _, ancla, nuevo in anclas:
            txt = txt.replace(ancla, nuevo)
        f.write_text(txt, encoding="utf-8")
        print(f"[ok] Patched {f.name}")

    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
