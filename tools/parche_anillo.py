#!/usr/bin/env python3
"""
Listens to the conversation between the game and the XMA, on the kernel side.  (v3)

    python tools/parche_anillo.py            apply
    python tools/parche_anillo.py --estado
    python tools/parche_anillo.py --revertir

It touches one SDK file:  src/kernel/xboxkrnl/xboxkrnl_audio_xma.cpp


WHERE WE STAND
==============

The hang is cornered down to the millisecond. This is voice 19 dying, exactly
as it came out in the log:

    57.907  Work escritura=8  lectura=12  hueco=4     <- ultimo pase que produjo
    57.924  Work escritura=12 lectura=16  hueco=4     NO PRODUJO NADA ent0=0 ent1=0
    57.935  Work escritura=12 lectura=20  hueco=8     NO PRODUJO NADA ent0=0 ent1=0
    57.945  Work escritura=12 lectura=0   hueco=12    NO PRODUJO NADA ent0=0 ent1=0
    57.964  Work escritura=12 lectura=4   hueco=16    NO PRODUJO NADA ent0=0 ent1=0
    57.974  Work escritura=12 lectura=8   hueco=20    NO PRODUJO NADA ent0=0 ent1=0
    58.333  [guest] pide escritura: escritura=12 lectura=8 valida=1 ent0=0 ent1=0
            ... y esa misma linea 90 segundos seguidos.

In other words: at 57.907 the decoder uses up the last input it had. The two
input buffers end up at zero. The game keeps kicking five more times and keeps
consuming what was left -the read advances 16, 20, 0, 4, 8-, and on reaching 8
it stops dead. The write has been frozen at 12 since the start of that batch,
because there is nothing to decode.

The read stops ONE STEP short of reaching the write. And that matters, because
the game's loop only asks whether the output buffer is still valid when read
and write match. Being one block short, it never gets to ask, and it sits
waiting for audio that cannot arrive.


WHAT IS STILL MISSING, AND WHY IT WAS NOT KNOWN BEFORE
======================================================

Only one question is left: AFTER 57.907, does the game feed input to that
voice again?

  - If it does NOT, the fault is in the game: it got into the loop before
    refilling, and we must look at why it ran out of data.
  - If it DOES and the decoder still says ent0=0 ent1=0, then we are losing it
    when receiving it, and the fault is in the SDK.

v2 could not answer it because of a mistake of mine: I put ONE limit of one
line per second on all functions alike. It makes sense for the two the game
polls thousands of times per second in the loop, but not for the ones that
WRITE, which are called at a normal pace. With that limit, only one input
delivery per second was visible, and moreover whichever one came from any
context, not the one that matters.

So now:

  - the query ones -asking for offsets, checking validity- are still limited
  - the WRITING ones go unlimited: feeding input, delivering the buffer with
    its packet count, moving the read, revalidating the output, powering off

The kicks already come out whole through the other patch, so they are not
needed here.

With this, the stretch between 57.907 and the hang is recorded in full and the
question answers itself.
"""

import argparse
import pathlib
import shutil
import sys

MARCA = "PARCHE LOCAL - escucha de la conversacion XMA v3"
MARCAS_VIEJAS = [
    "PARCHE LOCAL - escucha de la conversacion XMA",
    "PARCHE LOCAL - el anillo de salida no se llena del todo",
]

CAB_ANCLA = """#include <cstring>
"""

CAB_NUEVO = """#include <atomic>   // PARCHE LOCAL - escucha de la conversacion XMA v3
#include <chrono>   // PARCHE LOCAL - escucha de la conversacion XMA v3
#include <cstring>
"""

# The helper and the first instrumented function go together, to avoid relying
# on one more anchor in the namespace header.
# The helper must be declared BEFORE its first use, and the first function that
# uses it in this file is XMAIsInputBuffer0Valid, which appears well before the
# output ones. That is why the block hangs off that one.
AYUDA_ANCLA = """u32 XMAIsInputBuffer0Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.input_buffer_0_valid;
}
"""

AYUDA_NUEVO = """// PARCHE LOCAL - escucha de la conversacion XMA v3
//
// El bucle del juego llama a estas funciones miles de veces por segundo, asi
// que van limitadas a una linea por segundo CADA UNA. Cada una lleva su propio
// reloj -la estatica dentro de la macro-, para que la mas ruidosa no tape a
// las demas.
namespace {
bool XmaDiagToca(std::atomic<int64_t>& ultimo) {
  const int64_t ahora = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  int64_t anterior = ultimo.load(std::memory_order_relaxed);
  return ahora - anterior >= 1000 && ultimo.compare_exchange_strong(anterior, ahora);
}
}  // namespace

#define REX_DIAG_XMA(...)                          \\
  do {                                             \\
    static std::atomic<int64_t> _ultimo{0};        \\
    if (XmaDiagToca(_ultimo)) {                    \\
      REXAPU_DEBUG(__VA_ARGS__);                   \\
    }                                              \\
  } while (0)

u32 XMAIsInputBuffer0Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REX_DIAG_XMA("[guest] pregunta entrada 0: ctx={:08X} -> {}", context_ptr.guest_address(),
               uint32_t(context.input_buffer_0_valid));
  return context.input_buffer_0_valid;
}
"""

PARES = [
("""u32 XMAIsOutputBufferValid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.output_buffer_valid;
}
""",
 """u32 XMAIsOutputBufferValid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REX_DIAG_XMA("[guest] pregunta si la salida vale: ctx={:08X} -> {}", context_ptr.guest_address(),
               uint32_t(context.output_buffer_valid));
  return context.output_buffer_valid;
}
"""),

("""u32 XMASetOutputBufferValid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.output_buffer_valid = 1;
""",
 """u32 XMASetOutputBufferValid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REXAPU_DEBUG("[guest] revalida la salida: ctx={:08X}", context_ptr.guest_address());
  context.output_buffer_valid = 1;
"""),

("""u32 XMAGetOutputBufferReadOffset_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.output_buffer_read_offset;
}
""",
 """u32 XMAGetOutputBufferReadOffset_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REX_DIAG_XMA("[guest] pide lectura: ctx={:08X} lectura={} escritura={} valida={} ent0={} ent1={}",
               context_ptr.guest_address(), uint32_t(context.output_buffer_read_offset),
               uint32_t(context.output_buffer_write_offset),
               uint32_t(context.output_buffer_valid), uint32_t(context.input_buffer_0_valid),
               uint32_t(context.input_buffer_1_valid));
  return context.output_buffer_read_offset;
}
"""),

("""u32 XMASetOutputBufferReadOffset_entry(mapped_void context_ptr, u32 value) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.output_buffer_read_offset = value;
""",
 """u32 XMASetOutputBufferReadOffset_entry(mapped_void context_ptr, u32 value) {
  XMA_CONTEXT_DATA context(context_ptr);
  REXAPU_DEBUG("[guest] mueve la lectura: ctx={:08X} {} -> {}", context_ptr.guest_address(),
               uint32_t(context.output_buffer_read_offset), value);
  context.output_buffer_read_offset = value;
"""),

("""u32 XMAGetOutputBufferWriteOffset_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.output_buffer_write_offset;
}
""",
 """u32 XMAGetOutputBufferWriteOffset_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REX_DIAG_XMA("[guest] pide escritura: ctx={:08X} escritura={} lectura={} valida={} ent0={} ent1={}",
               context_ptr.guest_address(), uint32_t(context.output_buffer_write_offset),
               uint32_t(context.output_buffer_read_offset),
               uint32_t(context.output_buffer_valid), uint32_t(context.input_buffer_0_valid),
               uint32_t(context.input_buffer_1_valid));
  return context.output_buffer_write_offset;
}
"""),

# --- the input, which is the new part and what matters ---
("""u32 XMASetInputBuffer0Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.input_buffer_0_valid = 1;
""",
 """u32 XMASetInputBuffer0Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REXAPU_DEBUG("[guest] DA ENTRADA 0: ctx={:08X}", context_ptr.guest_address());
  context.input_buffer_0_valid = 1;
"""),

("""u32 XMAIsInputBuffer1Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.input_buffer_1_valid;
}
""",
 """u32 XMAIsInputBuffer1Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REX_DIAG_XMA("[guest] pregunta entrada 1: ctx={:08X} -> {}", context_ptr.guest_address(),
               uint32_t(context.input_buffer_1_valid));
  return context.input_buffer_1_valid;
}
"""),

("""u32 XMASetInputBuffer1Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.input_buffer_1_valid = 1;
""",
 """u32 XMASetInputBuffer1Valid_entry(mapped_void context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  REXAPU_DEBUG("[guest] DA ENTRADA 1: ctx={:08X}", context_ptr.guest_address());
  context.input_buffer_1_valid = 1;
"""),

("""u32 XMAEnableContext_entry(mapped_void context_ptr) {
  StoreXmaContextIndexedRegister(REX_KERNEL_STATE(), 0x1940, context_ptr.guest_address());
""",
 """u32 XMAEnableContext_entry(mapped_void context_ptr) {
  REX_DIAG_XMA("[guest] enciende el contexto: ctx={:08X}", context_ptr.guest_address());
  StoreXmaContextIndexedRegister(REX_KERNEL_STATE(), 0x1940, context_ptr.guest_address());
"""),

("""u32 XMADisableContext_entry(mapped_void context_ptr, u32 wait) {
  X_HRESULT result = X_E_SUCCESS;
""",
 """u32 XMADisableContext_entry(mapped_void context_ptr, u32 wait) {
  REXAPU_DEBUG("[guest] apaga el contexto: ctx={:08X} esperar={}", context_ptr.guest_address(),
               wait);
  X_HRESULT result = X_E_SUCCESS;
"""),
("""u32 XMASetInputBuffer0_entry(mapped_void context_ptr, mapped_void buffer, u32 packet_count) {
""",
 """u32 XMASetInputBuffer0_entry(mapped_void context_ptr, mapped_void buffer, u32 packet_count) {
  REXAPU_DEBUG("[guest] ENTREGA BUFFER 0: ctx={:08X} datos={:08X} paquetes={}",
               context_ptr.guest_address(), buffer.guest_address(), packet_count);
"""),

("""u32 XMASetInputBuffer1_entry(mapped_void context_ptr, mapped_void buffer, u32 packet_count) {
""",
 """u32 XMASetInputBuffer1_entry(mapped_void context_ptr, mapped_void buffer, u32 packet_count) {
  REXAPU_DEBUG("[guest] ENTREGA BUFFER 1: ctx={:08X} datos={:08X} paquetes={}",
               context_ptr.guest_address(), buffer.guest_address(), packet_count);
"""),
]

ANCLAS = ([("headers", CAB_ANCLA, CAB_NUEVO),
           ("helper and input 0 query", AYUDA_ANCLA, AYUDA_NUEVO)]
          + [(f"function {i + 1}", a, b) for i, (a, b) in enumerate(PARES)])


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
    orig = f.with_suffix(f.suffix + ".original")

    if args.estado:
        t = f.read_text(encoding="utf-8")
        if MARCA in t:
            print(f"  {f.name:30s} applied")
        elif any(m in t for m in MARCAS_VIEJAS):
            print(f"  {f.name:30s} OLDER version (will be changed on apply)")
        else:
            print(f"  {f.name:30s} not applied")
        return 0

    if args.revertir:
        if orig.exists():
            shutil.copy2(orig, f)
            print(f"[ok] Restored {f.name}")
        else:
            print(f"[aviso] There is no copy of {f.name}, there was nothing to undo")
        print()
        print("  THE SDK MUST BE RECOMPILED.")
        return 0

    txt = f.read_text(encoding="utf-8")
    if MARCA in txt:
        print(f"[ok] {f.name}: already up to date, leaving it alone")
        return 0

    # The previous version of this patch left its lines behind, and the
    # anchors below are written against the clean file.
    if any(m in txt for m in MARCAS_VIEJAS):
        if not orig.exists():
            sys.exit(f"[ERROR] {f.name} has the previous version but there is no\n"
                     f"        {orig.name} to undo it.")
        shutil.copy2(orig, f)
        txt = f.read_text(encoding="utf-8")
        print(f"[ok] {f.name}: previous version removed")

    for nombre, ancla, _ in ANCLAS:
        n = txt.count(ancla)
        if n != 1:
            sys.exit(f"[ERROR] The anchor '{nombre}' appears {n} times, expected 1.\n"
                     f"        The SDK must have changed. I have not touched anything.")

    if not orig.exists():
        shutil.copy2(f, orig)
        print(f"[ok] Backup: {orig.name}")

    for _, ancla, nuevo in ANCLAS:
        txt = txt.replace(ancla, nuevo)
    f.write_text(txt, encoding="utf-8")
    print(f"[ok] Patched {f.name}")
    print()
    print("  The whole conversation between the game and the XMA will be shown.")
    print("  Queries are limited to one line per second; the ones that")
    print("  WRITE -feed input, move the read, power off- go in full,")
    print("  as they were the ones needed and the limit was hiding them.")
    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
