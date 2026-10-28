// nfsmw - the language of the PAL editions that share the Spanish build.
//
// The French, German and Italian PAL executables are the Spanish one with another language number: 1 French,
// 2 German, 3 Italian, 4 Spanish. It is an immediate in seven instructions; the rest that changes (.rdata) is
// read from the loaded default.xex anyway. The recompiled code has the Spanish number built in, so these hooks
// take it from the loaded image instead: the instruction at 0x8245FD70 ("li r3,N") of whichever default.xex is
// running. That way the Spanish executable runs the four editions (edicion.cpp, familia "es").

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

constexpr uint32_t kFuncionIdioma = 0x8245FD70;  // li r3,N; blr
constexpr uint32_t kIdiomaCompilado = 4;          // Spanish: what the recompiled code returns

// The number in the loaded image, or the built-in one if that instruction is not "li r3,N" (another build).
uint32_t Idioma(const uint8_t* base) {
  // 0 is a language too (English, the European PAL edition): read once, flagged apart.
  static uint32_t idioma = 0;
  static bool leido = false;
  if (!leido) {
    leido = true;
    uint32_t palabra = 0;
    std::memcpy(&palabra, base + kFuncionIdioma, sizeof(palabra));
    palabra = __builtin_bswap32(palabra);
    idioma = (palabra >> 16) == 0x3860 ? (palabra & 0xFFFF) : kIdiomaCompilado;
    REXLOG_INFO("[idioma] numero de idioma del default.xex: {}", idioma);
  }
  return idioma;
}

}  // namespace

// The language number.
REX_HOOK_RAW(sub_8245FD70) { ctx.r3.u64 = Idioma(base); }

// "Is the language ...?": one per edition, each returning 1 for its own.
REX_HOOK_RAW(sub_823AEF80) { ctx.r3.u64 = Idioma(base) == 1; }  // French
REX_HOOK_RAW(sub_823AEF88) { ctx.r3.u64 = Idioma(base) == 2; }  // German
REX_HOOK_RAW(sub_823AEF90) { ctx.r3.u64 = Idioma(base) == 4; }  // Spanish
REX_HOOK_RAW(sub_823AEF98) { ctx.r3.u64 = Idioma(base) == 3; }  // Italian

// Sets up the language (r3). sub_822BCD10 calls it with the number ("li r3,N; b"), and so does the call at
// 0x823B4A38 in the middle of another function (li r3,N; bl, return address 0x823B4A40); the third caller passes
// its own value, left as it is.
REX_EXTERN(__imp__sub_822BCBD0);
REX_HOOK_RAW(sub_822BCBD0) {
  if (uint32_t(ctx.lr) == 0x823B4A40) {
    ctx.r3.u64 = Idioma(base);
  }
  __imp__sub_822BCBD0(ctx, base);
}

REX_HOOK_RAW(sub_822BCD10) {
  ctx.r3.u64 = Idioma(base);
  sub_822BCBD0(ctx, base);
}
