// nfsmw - EA's mixer evicts the engine's AEMS layers when it thinks it is running late (sub_82601980)
//
// The mixer times its own thread with the wall clock on every 256-sample block, averages the last three blocks,
// adds the cost of its voices and compares that with a CPU budget. When it goes over, sub_82601980 kills the
// lowest priority voices until the excess fits:
//   while (excess (f1) > 0) { voice = lowest_priority() /* sub_826018B0 */; if (!voice) break;
//                             excess -= cost(voice + 48) * factor; kill(voice, 0) /* sub_825D06B0 */; }
// On the 360 the mixer has a hardware thread of its own and this only happens under a real overload. In a port it
// is one more thread: whenever the system does not run it for a moment, the stopwatch counts that pause as mixing
// time and EA kills voices that cost almost nothing. The engine's AEMS layers (body, engine and exhaust samples)
// have a low priority, so they go first: the car suddenly sounds quieter and duller, usually until the next gear
// change restarts them, while the Ginsu synth layers (priority 101) keep playing. More often on slow CPUs: phones.
//
// The budget check (sub_82601720) only reads and keeps running; what is skipped is the eviction. Nothing uses its
// return value.
//
// Found by SpeedBreaker (ProjectUgarte, issue 35, runtime/apu/ea_cpu_budget.*); the same function in our edition
// was confirmed by its shape: it calls sub_826018B0 and sub_825D06B0, and its only caller
// (nfsmw_recomp.4.cpp) calls the hookable name, not __imp__.
//
// nfsmw_audio_ea_expulsa: false (default) = never evict; true = evict as the console does. A line every second at
// most says how many times EA would have evicted, which shows the fix is acting.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports

REXCVAR_DEFINE_BOOL(nfsmw_audio_ea_expulsa, false, "Audio",
                    "EA expulsa voces cuando su mezcla parece tardar demasiado, como en la consola. Apagado evita que "
                    "el motor del coche pierda cuerpo de golpe hasta el siguiente cambio de marcha");

REX_EXTERN(__imp__sub_82601980);

namespace {

std::atomic<uint64_t> g_veces{0};
std::atomic<int64_t> g_ultima_ms{0};

int64_t AhoraMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// r3 = mixer + 68, f1 = how far over the budget it is, in its own units.
REX_HOOK_RAW(sub_82601980) {
  const uint64_t n = g_veces.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool expulsa = REXCVAR_GET(nfsmw_audio_ea_expulsa);
  const int64_t ahora = AhoraMs();
  int64_t ultima = g_ultima_ms.load(std::memory_order_relaxed);
  if (ahora - ultima >= 1000 && g_ultima_ms.compare_exchange_strong(ultima, ahora, std::memory_order_relaxed)) {
    NFSMW_INFORME_DIFERIDO("[presupuesto] EA {} voces: {} veces hasta ahora, esta vez se pasa {:.0f}",
                           expulsa ? "expulsa" : "habria expulsado", n, ctx.f1.f64);
  }
  if (expulsa) {
    __imp__sub_82601980(ctx, base);
  }
}
