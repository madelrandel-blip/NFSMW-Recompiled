// nfsmw - guards for null pointers the original code dereferences.
//
// On the Xbox 360 a read near address 0 returns garbage and the game carries on; here it faults. Each guard skips
// one function when the object it works on is missing, as if it had nothing to do.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

}  // namespace

// Walks the entries of a sorted float table (object +12) between two keys (f1, f2), on the audio server thread.
// Seen on the AYN Thor with the table still null: the lookup inside (sub_8237FE58) read address 4 and the thread
// faulted in a loop.
REX_EXTERN(__imp__sub_823A13F0);
REX_HOOK_RAW(sub_823A13F0) {
  if (ctx.r3.u32 == 0 || Leer32(base, ctx.r3.u32 + 12) == 0) {
    static bool avisado = false;
    if (!avisado) {
      avisado = true;
      REXLOG_WARN("[guardas] sub_823A13F0 without its table (object 0x{:08X}): skipped", ctx.r3.u32);
    }
    return;
  }
  __imp__sub_823A13F0(ctx, base);
}

// Per-frame update of the same object (called through its vtable): it advances its time and, after the walk above,
// reads the table again (object +12, then +12 of it). Without the table there is nothing to play: return 0, its own
// "nothing done" exit.
REX_EXTERN(__imp__sub_823A0FA0);
REX_HOOK_RAW(sub_823A0FA0) {
  if (ctx.r3.u32 == 0 || Leer32(base, ctx.r3.u32 + 12) == 0) {
    static bool avisado = false;
    if (!avisado) {
      avisado = true;
      REXLOG_WARN("[guardas] sub_823A0FA0 without its table (object 0x{:08X}): skipped", ctx.r3.u32);
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823A0FA0(ctx, base);
}

// Evaluates the same object's tracks (r4) into r5. With r4's track list (+4) null the original already returns r5
// untouched. On the AYN Thor it read address 4: r4 null, or a list that is no real address (below the first 64 KB,
// where nothing of the game lives); both get the same exit. It also copies one word per track into an array on its
// stack frame (r1+80 of 416 bytes, room for 80): a garbage track count ran past the thread's stack, so a count
// beyond that is taken as no tracks too.
REX_EXTERN(__imp__sub_823A1678);
REX_HOOK_RAW(sub_823A1678) {
  const uint32_t lista = ctx.r4.u32 ? Leer32(base, ctx.r4.u32 + 4) : 0;
  const uint32_t pistas = lista >= 0x10000 ? Leer32(base, lista) : 0;
  if (ctx.r4.u32 == 0 || (lista != 0 && lista < 0x10000) || pistas > 80) {
    static bool avisado = false;
    if (!avisado) {
      avisado = true;
      REXLOG_WARN("[guardas] sub_823A1678 without valid tracks (object 0x{:08X}, tracks 0x{:08X}, list 0x{:08X}, "
                  "count {}): skipped", ctx.r3.u32, ctx.r4.u32, lista, pistas);
    }
    ctx.r3.u64 = ctx.r5.u32;
    return;
  }
  __imp__sub_823A1678(ctx, base);
}
