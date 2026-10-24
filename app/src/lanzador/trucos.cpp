// nfsmw - gestor de trucos (ver trucos.h).

#include "trucos.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

REXCVAR_DEFINE_STRING(nfsmw_trucos_activos, "", "Interfaz",
                      "Trucos activados en el lanzador, separados por ';' (nombres de nfsmw_trucos.txt)");

namespace nfsmw::trucos {
namespace {

constexpr const char* kPlantilla =
    "# Trucos de NFSMW Recompiled. Se activan desde el lanzador (pagina Trucos).\n"
    "#\n"
    "# Un bloque por truco:\n"
    "#\n"
    "#   [Nombre del truco]\n"
    "#   nota = Lo que hace\n"
    "#   u32 0x82A01234 1000000\n"
    "#   u32 *0x82A05678+0x1C 5      (puntero: lee la direccion y suma el desplazamiento)\n"
    "#\n"
    "# Tipos: u8, u16, u32, f32. Las direcciones son de la memoria de la Xbox 360\n"
    "# (las que muestran los buscadores de memoria de Xenia). Mientras el juego\n"
    "# corre, los trucos activos se vuelven a escribir cada 100 ms.\n";

std::string Recortar(std::string s) {
  const auto a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return {};
  const auto b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

bool Numero(const std::string& s, uint32_t* out) {
  if (s.empty()) return false;
  char* fin = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &fin, 0);
  if (*fin != '\0' || v > 0xFFFFFFFFull) return false;
  *out = uint32_t(v);
  return true;
}

bool LeerEscritura(const std::string& linea, Escritura* e) {
  std::istringstream in(linea);
  std::string tipo, dir, valor;
  if (!(in >> tipo >> dir >> valor)) return false;
  if (tipo == "u8") e->tipo = Escritura::Tipo::kU8;
  else if (tipo == "u16") e->tipo = Escritura::Tipo::kU16;
  else if (tipo == "u32") e->tipo = Escritura::Tipo::kU32;
  else if (tipo == "f32") e->tipo = Escritura::Tipo::kF32;
  else return false;
  if (dir[0] == '*') {
    e->puntero = true;
    dir = dir.substr(1);
    const auto mas = dir.find('+');
    if (mas != std::string::npos) {
      if (!Numero(dir.substr(mas + 1), &e->desplazamiento)) return false;
      dir = dir.substr(0, mas);
    }
  }
  if (!Numero(dir, &e->direccion)) return false;
  if (e->tipo == Escritura::Tipo::kF32) {
    char* fin = nullptr;
    const float f = std::strtof(valor.c_str(), &fin);
    if (*fin != '\0') return false;
    std::memcpy(&e->valor, &f, 4);
    return true;
  }
  return Numero(valor, &e->valor);
}

std::vector<std::string> Activos() {
  std::vector<std::string> r;
  std::istringstream in(REXCVAR_GET(nfsmw_trucos_activos));
  std::string n;
  while (std::getline(in, n, ';'))
    if (!n.empty()) r.push_back(n);
  return r;
}

// La direccion es escribible ahora mismo (el juego puede no haberla reservado aun).
uint8_t* Destino(rex::memory::Memory* m, uint32_t direccion, uint32_t tam) {
  auto* heap = m->LookupHeap(direccion);
  uint32_t proteccion = 0;
  if (!heap || !heap->QueryProtect(direccion, &proteccion) ||
      !(proteccion & rex::memory::kMemoryProtectWrite))
    return nullptr;
  if (m->LookupHeap(direccion + tam - 1) != heap) return nullptr;
  return m->virtual_membase() + direccion;
}

void Aplicar(rex::memory::Memory* m, const Escritura& e) {
  uint32_t direccion = e.direccion;
  if (e.puntero) {
    uint8_t* p = Destino(m, direccion, 4);
    if (!p) return;
    uint32_t v;
    std::memcpy(&v, p, 4);
    direccion = __builtin_bswap32(v) + e.desplazamiento;
    if (direccion == e.desplazamiento) return;  // puntero nulo
  }
  switch (e.tipo) {
    case Escritura::Tipo::kU8:
      if (uint8_t* p = Destino(m, direccion, 1)) *p = uint8_t(e.valor);
      break;
    case Escritura::Tipo::kU16:
      if (uint8_t* p = Destino(m, direccion, 2)) {
        const uint16_t v = __builtin_bswap16(uint16_t(e.valor));
        std::memcpy(p, &v, 2);
      }
      break;
    default:
      if (uint8_t* p = Destino(m, direccion, 4)) {
        const uint32_t v = __builtin_bswap32(e.valor);
        std::memcpy(p, &v, 4);
      }
      break;
  }
}

std::atomic<bool> g_activo{false};
std::thread g_hilo;

}  // namespace

std::filesystem::path RutaFichero() { return rex::filesystem::GetExecutableFolder() / "nfsmw_trucos.txt"; }

std::vector<Truco> Cargar(int* errores) {
  std::vector<Truco> trucos;
  int malas = 0;
  std::ifstream in(RutaFichero());
  if (!in) {
    std::ofstream(RutaFichero()) << kPlantilla;
  } else {
    std::string linea;
    while (std::getline(in, linea)) {
      const auto almohadilla = linea.find('#');
      if (almohadilla != std::string::npos) linea.resize(almohadilla);
      linea = Recortar(linea);
      if (linea.empty()) continue;
      if (linea.front() == '[' && linea.back() == ']') {
        trucos.push_back({Recortar(linea.substr(1, linea.size() - 2)), {}, {}});
        continue;
      }
      if (trucos.empty()) {
        ++malas;
        continue;
      }
      if (linea.rfind("nota", 0) == 0 && linea.find('=') != std::string::npos) {
        trucos.back().nota = Recortar(linea.substr(linea.find('=') + 1));
        continue;
      }
      Escritura e;
      if (LeerEscritura(linea, &e)) trucos.back().escrituras.push_back(e);
      else ++malas;
    }
  }
  if (errores) *errores = malas;
  return trucos;
}

bool Activo(const std::string& nombre) {
  for (const auto& n : Activos())
    if (n == nombre) return true;
  return false;
}

void Activar(const std::string& nombre, bool activo) {
  std::string lista;
  for (const auto& n : Activos())
    if (n != nombre) lista += (lista.empty() ? "" : ";") + n;
  if (activo) lista += (lista.empty() ? "" : ";") + nombre;
  rex::cvar::SetFlagByName("nfsmw_trucos_activos", lista);
}

void Arrancar(rex::memory::Memory* memoria) {
  if (!memoria || g_activo.exchange(true)) return;
  std::vector<Escritura> escrituras;
  for (const auto& t : Cargar())
    if (Activo(t.nombre)) {
      escrituras.insert(escrituras.end(), t.escrituras.begin(), t.escrituras.end());
      REXLOG_INFO("[trucos] activo: {} ({} escritura(s))", t.nombre, t.escrituras.size());
    }
  if (escrituras.empty()) {
    g_activo = false;
    return;
  }
  g_hilo = std::thread([memoria, escrituras = std::move(escrituras)] {
    while (g_activo.load()) {
      for (const auto& e : escrituras) Aplicar(memoria, e);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
}

void Parar() {
  g_activo = false;
  if (g_hilo.joinable()) g_hilo.join();
}

}  // namespace nfsmw::trucos
