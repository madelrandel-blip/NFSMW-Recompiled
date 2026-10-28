// nfsmw - stack dump of every thread for hangs, Android version of VolcarPilas (nfsmw_perfil_pc.h).
//
// The watchdog in nfsmw_app.h calls it when the game stalls. Each thread of the process (/proc/self/task) gets
// a signal, one at a time; its handler walks its own stack with _Unwind_Backtrace (the code is built with
// -funwind-tables) and the caller writes it to the log as library + offset, which llvm-symbolizer turns into
// source lines with the unstripped libraries of the same build:
//
//   llvm-symbolizer --obj=libmain.so 0x<offset>

#include "nfsmw_perfil_pc.h"

#include <rex/logging.h>

#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace nfsmw::perfil_pc {
namespace {

// Not one of the SDK's thread signals (SIGRTMIN + n, threading_posix.cpp) nor ART's.
const int kSenal = SIGRTMAX - 2;
constexpr int kMarcos = 48;

// One request at a time: the thread the signal is for, and where its handler leaves the stack.
std::atomic<pid_t> g_objetivo{0};
std::atomic<bool> g_hecho{false};
uintptr_t g_marcos[kMarcos];
int g_num_marcos = 0;

struct Recorrido {
  uintptr_t* marcos;
  int n;
};

_Unwind_Reason_Code Paso(_Unwind_Context* contexto, void* datos) {
  auto* r = static_cast<Recorrido*>(datos);
  if (r->n >= kMarcos) {
    return _URC_END_OF_STACK;
  }
  const uintptr_t pc = _Unwind_GetIP(contexto);
  if (pc) {
    r->marcos[r->n++] = pc;
  }
  return _URC_NO_REASON;
}

void Manejador(int, siginfo_t*, void*) {
  if (pid_t(syscall(SYS_gettid)) != g_objetivo.load(std::memory_order_acquire)) {
    return;
  }
  Recorrido r{g_marcos, 0};
  _Unwind_Backtrace(Paso, &r);
  g_num_marcos = r.n;
  g_hecho.store(true, std::memory_order_release);
}

std::string NombreHilo(pid_t tid) {
  char ruta[64];
  std::snprintf(ruta, sizeof(ruta), "/proc/self/task/%d/comm", tid);
  std::string nombre;
  if (FILE* f = std::fopen(ruta, "r")) {
    char buf[64] = {};
    if (std::fgets(buf, sizeof(buf), f)) {
      nombre = buf;
      while (!nombre.empty() && (nombre.back() == '\n' || nombre.back() == '\r')) {
        nombre.pop_back();
      }
    }
    std::fclose(f);
  }
  return nombre;
}

std::string Marco(uintptr_t pc) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(pc), &info) || !info.dli_fname) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%zx", size_t(pc));
    return buf;
  }
  const char* barra = std::strrchr(info.dli_fname, '/');
  const char* lib = barra ? barra + 1 : info.dli_fname;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%s+0x%zx", lib, size_t(pc - uintptr_t(info.dli_fbase)));
  std::string texto = buf;
  if (info.dli_sname) {
    texto += " (";
    texto += info.dli_sname;
    texto += ")";
  }
  return texto;
}

}  // namespace

void VolcarPilas(const char* motivo) {
  static std::atomic<bool> instalado{false};
  if (!instalado.exchange(true)) {
    struct sigaction sa {};
    sa.sa_sigaction = Manejador;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(kSenal, &sa, nullptr);
  }
  const pid_t yo = pid_t(syscall(SYS_gettid));
  const pid_t proceso = getpid();
  REXLOG_ERROR("[pilas] {}: pilas de todos los hilos (llvm-symbolizer --obj=<biblioteca> <desplazamiento>)", motivo);
  DIR* dir = opendir("/proc/self/task");
  if (!dir) {
    return;
  }
  while (dirent* e = readdir(dir)) {
    const pid_t tid = pid_t(std::atoi(e->d_name));
    if (tid <= 0 || tid == yo) {
      continue;
    }
    g_hecho.store(false, std::memory_order_relaxed);
    g_objetivo.store(tid, std::memory_order_release);
    if (syscall(SYS_tgkill, proceso, tid, kSenal) != 0) {
      continue;
    }
    // A thread with the signal blocked never answers: give up on it after a moment.
    const auto limite = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!g_hecho.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < limite) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool respondio = g_hecho.load(std::memory_order_acquire);
    g_objetivo.store(0, std::memory_order_release);
    if (!respondio) {
      REXLOG_ERROR("[pilas] hilo {} '{}': no respondio", tid, NombreHilo(tid));
      continue;
    }
    std::string pila;
    // The first frames are the handler and the signal trampoline.
    for (int i = 0; i < g_num_marcos; ++i) {
      pila += "\n    #" + std::to_string(i) + " " + Marco(g_marcos[i]);
    }
    REXLOG_ERROR("[pilas] hilo {} '{}':{}", tid, NombreHilo(tid), pila);
  }
  closedir(dir);
}

}  // namespace nfsmw::perfil_pc
