// nfsmw - system monitor for performance reports on phones (Android, also Linux).
//
// Every nfsmw_monitor_sistema_s seconds one log line with what the game's own measurements cannot see: how much CPU
// the process used and which threads used it (with their names), the clock of every core, the hottest thermal zones
// and the memory. With it a stutter can be told apart: the game threads at their limit, the cores clocked down by
// heat, or the system short of memory. Everything comes from /proc and /sys; whatever a phone does not let an app
// read is left out of the line.

#include "nfsmw_monitor_sistema.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef NFSMW_DIAGNOSTICO
#define NFSMW_DIAGNOSTICO 0
#endif

REXCVAR_DEFINE_INT32(nfsmw_monitor_sistema_s, NFSMW_DIAGNOSTICO ? 10 : 0, "NFSMW",
                     "Diagnostico (Android): cada N segundos anota en el log la CPU del proceso y de sus hilos, la "
                     "frecuencia de cada nucleo, las temperaturas y la memoria; 0 = nada")
    .range(0, 600);

#if defined(__linux__) || defined(__ANDROID__)

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <rex/kernel/xboxkrnl/threading.h>
#include <unordered_map>
#include <vector>

namespace nfsmw::monitor {
namespace {

std::string LeerLinea(const std::string& ruta) {
  std::ifstream f(ruta);
  std::string linea;
  std::getline(f, linea);
  return linea;
}

long long LeerNumero(const std::string& ruta) {
  const std::string linea = LeerLinea(ruta);
  return linea.empty() ? -1 : std::atoll(linea.c_str());
}

// utime + stime (in clock ticks) and the name of a /proc/.../stat line. The name goes in parentheses and may have
// spaces, so the fields are counted from the last ')'.
bool LeerStat(const std::string& ruta, long long& ticks, std::string& nombre) {
  const std::string linea = LeerLinea(ruta);
  const size_t a = linea.find('('), b = linea.rfind(')');
  if (a == std::string::npos || b == std::string::npos || b < a) return false;
  nombre = linea.substr(a + 1, b - a - 1);
  // After ')': state (field 3) ... utime is field 14 and stime field 15, i.e. the 12th and 13th after ')'.
  const char* p = linea.c_str() + b + 1;
  long long campos[13] = {};
  for (int i = 0; i < 13; ++i) {
    while (*p == ' ') ++p;
    if (!*p) return false;
    campos[i] = std::atoll(p);
    while (*p && *p != ' ') ++p;
  }
  ticks = campos[11] + campos[12];
  return true;
}

// The value of a "Key:   1234 kB" line of /proc/self/status or /proc/meminfo, in MB.
long long LeerKb(const char* ruta, const char* clave) {
  std::ifstream f(ruta);
  std::string linea;
  const size_t n = std::strlen(clave);
  while (std::getline(f, linea))
    if (linea.compare(0, n, clave) == 0) return std::atoll(linea.c_str() + n + 1) / 1024;
  return -1;
}

void Bucle(int intervalo_s) {
  const long tick = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
  const int nucleos = int(std::max(1L, sysconf(_SC_NPROCESSORS_CONF)));
  std::unordered_map<int, long long> hilos_antes;
  long long proceso_antes = -1;
  auto antes = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(intervalo_s));
    const auto ahora = std::chrono::steady_clock::now();
    const double segundos = std::chrono::duration<double>(ahora - antes).count();
    antes = ahora;

    // The process and each of its threads: % of one core over the interval.
    long long proceso = 0;
    std::string nombre;
    LeerStat("/proc/self/stat", proceso, nombre);
    struct Hilo {
      double porcentaje;
      int tid;
      std::string nombre;
    };
    std::vector<Hilo> hilos;
    std::unordered_map<int, long long> hilos_ahora;
    if (DIR* dir = opendir("/proc/self/task")) {
      while (dirent* e = readdir(dir)) {
        const int tid = std::atoi(e->d_name);
        if (tid <= 0) continue;
        long long ticks = 0;
        if (!LeerStat(std::string("/proc/self/task/") + e->d_name + "/stat", ticks, nombre)) continue;
        hilos_ahora[tid] = ticks;
        const auto it = hilos_antes.find(tid);
        if (it != hilos_antes.end() && segundos > 0)
          hilos.push_back({100.0 * double(ticks - it->second) / double(tick) / segundos, tid, nombre});
      }
      closedir(dir);
    }
    hilos_antes.swap(hilos_ahora);
    const double proceso_pct =
        proceso_antes >= 0 && segundos > 0 ? 100.0 * double(proceso - proceso_antes) / double(tick) / segundos : 0.0;
    proceso_antes = proceso;
    std::sort(hilos.begin(), hilos.end(), [](const Hilo& a, const Hilo& b) { return a.porcentaje > b.porcentaje; });
    std::string texto_hilos;
    for (size_t i = 0; i < hilos.size() && i < 8; ++i) {
      if (hilos[i].porcentaje < 1.0) break;
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s%s %.0f%%", i ? ", " : "", hilos[i].nombre.c_str(), hilos[i].porcentaje);
      texto_hilos += buf;
    }

    // Clock of each core now, in MHz (0 = offline or not readable).
    std::string mhz;
    for (int c = 0; c < nucleos; ++c) {
      const long long khz = LeerNumero("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/scaling_cur_freq");
      mhz += (c ? " " : "") + std::to_string(khz > 0 ? khz / 1000 : 0);
    }

    // The three hottest thermal zones a normal app can read (values in m°C, a few report °C).
    struct Zona {
      double grados;
      std::string tipo;
    };
    std::vector<Zona> zonas;
    for (int z = 0; z < 96; ++z) {
      const std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(z);
      const long long t = LeerNumero(base + "/temp");
      if (t <= 0) continue;
      const double grados = t > 1000 ? double(t) / 1000.0 : double(t);
      if (grados > 5 && grados < 150) zonas.push_back({grados, LeerLinea(base + "/type")});
    }
    std::sort(zonas.begin(), zonas.end(), [](const Zona& a, const Zona& b) { return a.grados > b.grados; });
    std::string texto_zonas;
    for (size_t i = 0; i < zonas.size() && i < 3; ++i) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s%s %.0f C", i ? ", " : "", zonas[i].tipo.c_str(), zonas[i].grados);
      texto_zonas += buf;
    }

    REXLOG_INFO("[monitor] CPU del proceso {:.0f}% (de {}% con {} nucleos) | hilos: {} | MHz por nucleo: {} | "
                "temperaturas: {} | memoria: el juego {} MB, libre en el sistema {} MB",
                proceso_pct, nucleos * 100, nucleos, texto_hilos.empty() ? "-" : texto_hilos, mhz,
                texto_zonas.empty() ? "no legibles" : texto_zonas, LeerKb("/proc/self/status", "VmRSS:"),
                LeerKb("/proc/meminfo", "MemAvailable:"));
    // Guest threads stuck in a kernel wait for 2 s or more: what each waits on and who called it.
    const std::string esperas = rex::kernel::xboxkrnl::DescribeLongWaits(2000);
    if (!esperas.empty()) {
      REXLOG_INFO("[monitor] esperas largas: {}", esperas);
    }
  }
}

}  // namespace

void Arrancar() {
  const int intervalo = REXCVAR_GET(nfsmw_monitor_sistema_s);
  if (intervalo <= 0) return;
  rex::kernel::xboxkrnl::SetWaitRegistryEnabled(true);
  std::thread([intervalo] { Bucle(intervalo); }).detach();
  REXLOG_INFO("[monitor] cada {} s: CPU por hilo, frecuencias, temperaturas y memoria", intervalo);
}

}  // namespace nfsmw::monitor

#else

namespace nfsmw::monitor {
void Arrancar() {}
}  // namespace nfsmw::monitor

#endif
