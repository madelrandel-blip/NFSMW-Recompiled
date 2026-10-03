#!/usr/bin/env python3
"""
Unsticks the XMA voice when the game gets stuck spinning on it.

    python tools/parche_desatasco.py            apply
    python tools/parche_desatasco.py --estado
    python tools/parche_desatasco.py --revertir

It touches one SDK file:  src/kernel/xboxkrnl/xboxkrnl_audio_xma.cpp
It goes AFTER tools/parche_anillo.py, on that same file.

WARNING UP FRONT: this is a PATCH-UP, not the cure. It breaks the jam from the
outside instead of preventing it from happening. I say it here so it is written
down.


WHAT IS ALREADY MEASURED, WITH NO GAPS
======================================

All the game's audio is carried by ONE SINGLE thread, 0xD. The same one feeds
the decoder, consumes what is decoded and mixes. This is the end, with
millisecond detail:

  01.528  the game feeds input to voice 19
  01.679  it enters the mixing loop (sub_825E1CD0) for that voice
  01.679  Work produces, write 0 -> 4
  01.700  Work produces, write 4 -> 8
  01.709  Work produces, write 8 -> 12
  01.728  Work PRODUCES NOTHING.  ent0=0 ent1=0.  The input is gone.
  01.739  ...
  01.782  ...  and meanwhile the game moves its read 16, 20, 0, 4, 8: a full
               lap around the ring consuming what nobody refills anymore.
               On returning to 8 it stops.
  02.119  from here on, 90 seconds reading the two offsets and nothing else.

And in that same stretch the game DOES feed input to the neighboring voices
-6500 and 6540- at 01.549, 01.608, 01.658, 01.698, 01.759 and 01.779. To voice
19 it gives none. It is not that it forgets: to get to feed it, it would have
to leave the mixing loop, and it never leaves.


WHY IT DOES NOT LEAVE
=====================

The loop, read instruction by instruction:

  - if a voice is badly served, it sets a flag and does NOT move to the next:
    it repeats that same voice endlessly
  - to know how much audio there is, it subtracts:  write*256 - its cursor
  - if that subtraction gives ZERO, and only then, it asks whether the output
    buffer is still valid. If told no, it takes it as "buffer complete" and
    takes the 6144 bytes at once. That is its emergency exit.

In the jam the subtraction gives about 1000, not zero: the game's read stops
ONE BLOCK short of reaching the write. So it never gets to ask, and its
emergency exit does not trigger. It waits for audio that only a decoder with
nothing to work on could produce, fed by the same thread that waits.


WHAT THIS PATCH DOES
====================

It watches that exact situation, in the very function the game polls in a loop.
When for more than 250 ms ALL of this holds at the same time:

  - the game asks for the write offset of the same context over and over
  - that context has its output marked as valid
  - both of its input buffers are empty, i.e. the decoder has absolutely
    nothing to produce
  - and write and read do not match, which is what keeps the game from ever
    getting to its question

then it gives it the signal its own code knows how to interpret: it makes the
write equal the read and turns output_buffer_valid off. That is, "this buffer
is finished". The game does its subtraction, gets zero or negative, asks, finds
out, takes what is left and moves on.

The 250 ms are plenty: in normal operation those queries resolve in
microseconds. The condition does not hold while playing properly.

The price is a hiccup in that voice's audio, because part of what it takes is
old material from the ring. In exchange for not hanging.


WHY IT IS A PATCH-UP AND NOT THE CURE
=====================================

The cure would be for the decoder to never run dry mid-mix, and that means
understanding why the game gets so short on input. I suspect the pacing: on
this machine, at 18 fps and with logging maxed out, the audio thread arrives
late to refill. But suspecting is not knowing, and I am not going to sell it as
if I knew.

What can be said is that it attacks a situation IMPOSSIBLE to reach while
playing properly -a thread spinning a quarter of a second on a voice with no
input- and that if it triggers it leaves a warning in the log. If it shows up
often, the pacing problem is serious and must be chased. If it never shows up
and the game stops hanging, this was it.
"""

import argparse
import pathlib
import sys

MARCA = "PARCHE LOCAL - desatasco de la voz XMA"

ANCLA = """u32 XMAGetOutputBufferWriteOffset_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
"""

NUEVO = """u32 XMAGetOutputBufferWriteOffset_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);

  // PARCHE LOCAL - desatasco de la voz XMA
  //
  // Aqui es donde el juego se queda girando cuando se cuelga: pide este
  // offset, pide el de lectura, y vuelta a empezar, para siempre.
  //
  // Se vigila un solo contexto a la vez, el ultimo que haya preguntado. No
  // hace falta mas: cuando se atasca pregunta por uno y solo por uno, asi que
  // dos enteros atomicos bastan y esto no cuesta nada en el camino normal,
  // que es lo que importa estando en un bucle tan caliente.
  {
    const uint32_t direccion = context_ptr.guest_address();
    static std::atomic<uint32_t> vigilado{0};
    static std::atomic<int64_t> desde{0};

    const int64_t ahora = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();

    if (vigilado.load(std::memory_order_relaxed) != direccion) {
      vigilado.store(direccion, std::memory_order_relaxed);
      desde.store(ahora, std::memory_order_relaxed);
    } else {
      const int64_t llevo = ahora - desde.load(std::memory_order_relaxed);

      // La foto exacta del atasco, y nada mas que esa:
      //   salida valida  +  las dos entradas vacias  +  offsets distintos.
      // Con las entradas vacias el descodificador no puede producir ni una
      // muestra por mucho que se le insista, asi que esperar no arregla nada.
      // Y con los offsets distintos el juego nunca llega a preguntar si el
      // buffer sigue valido, que es su unica salida.
      const bool atascado = llevo > 250 && context.output_buffer_valid &&
                            !context.input_buffer_0_valid && !context.input_buffer_1_valid &&
                            context.output_buffer_write_offset != context.output_buffer_read_offset;

      if (atascado) {
        REXAPU_WARN(
            "[desatasco] ctx={:08X} lleva {} ms girando sin entrada "
            "(escritura={} lectura={}). Le digo que el buffer esta terminado.",
            direccion, llevo, uint32_t(context.output_buffer_write_offset),
            uint32_t(context.output_buffer_read_offset));

        // Igualar los dos offsets hace que la resta del juego de cero o
        // negativo, que es lo que le empuja a preguntar; y apagar la validez
        // es la respuesta que su codigo entiende como "buffer completo".
        context.output_buffer_write_offset = context.output_buffer_read_offset;
        context.output_buffer_valid = 0;
        context.Store(context_ptr);

        // El reloj se reinicia para no repetirlo en la vuelta siguiente si el
        // juego tardara un poco en reaccionar.
        desde.store(ahora, std::memory_order_relaxed);
        return context.output_buffer_write_offset;
      }
    }
  }
"""


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "src" / "audio" / "xma_context.cpp").exists():
            return cand
    sys.exit("[ERROR] SDK not found. Looked in ..\\rexglue-sdk and .\\sdk")


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    f = localizar_sdk() / "src" / "kernel" / "xboxkrnl" / "xboxkrnl_audio_xma.cpp"
    if not f.exists():
        sys.exit(f"[ERROR] Cannot find {f}")

    if args.estado:
        puesto = MARCA in f.read_text(encoding="utf-8")
        print(f"  {f.name:30s} unstick {'applied' if puesto else 'not applied'}")
        return 0

    if args.revertir:
        # The backup of this file is made by parche_anillo.py, which is the one
        # that touches it first. Restoring it here would wipe out its
        # instrumentation, so this is redirected to the one in charge.
        print("  This patch goes on top of parche_anillo.py and shares its")
        print("  backup, so it is undone from there:")
        print(r"    py -3 tools\parche_anillo.py --revertir")
        print()
        print("  And if you want the instrumentation but without the unstick,")
        print(r"  run tools\parche_anillo.py again afterwards.")
        return 0

    txt = f.read_text(encoding="utf-8")
    if MARCA in txt:
        print(f"[ok] {f.name}: the unstick was already in place")
        return 0

    # This patch uses std::atomic and std::chrono, and the one that puts those
    # two headers in the file is parche_anillo.py. Without it, this would fail
    # to compile and the error would come out halfway through the SDK build,
    # which is the worst possible place to find out. Better to stop here.
    if "PARCHE LOCAL - escucha de la conversacion XMA" not in txt:
        sys.exit("[ERROR] parche_anillo.py is missing, and it is the one that puts\n"
                 "        in the headers this needs. Run it first:\n"
                 "            py -3 tools\\parche_anillo.py\n"
                 "        I have not touched anything.")

    n = txt.count(ANCLA)
    if n != 1:
        sys.exit(f"[ERROR] The anchor appears {n} times, expected 1.\n"
                 f"        Run tools\\parche_anillo.py first. I have not touched anything.")

    f.write_text(txt.replace(ANCLA, NUEVO), encoding="utf-8")
    print(f"[ok] Unstick installed in {f.name}")
    print()
    print("  If it triggers, it will leave a [desatasco] warning in the log.")
    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
