// nfsmw - extractor de imagenes .iso de Xbox 360 (ver iso.h).
//
// Formato XDVDFS ("MICROSOFT*XBOX*MEDIA"):
//   - El descriptor de volumen esta 32 sectores despues de la base de la
//     particion. La base cambia segun el tipo de disco (XGD1/2/3 o imagen ya
//     recortada): se prueban las conocidas y, si ninguna cuadra, se busca el
//     magic en el primer GB.
//   - Cada directorio es un arbol binario plano: cada entrada lleva hijo
//     izquierdo, hijo derecho, sector, tamano, atributos y nombre. Los hijos
//     van en unidades de 4 bytes.

#include "iso.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <stack>
#include <vector>

namespace nfsmw::iso {
namespace {

constexpr int64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicLen = sizeof(kMagic) - 1;
// Imagen ya recortada, XGD2 (casi todos los de 360), XGD3 y XGD1 (Xbox original).
constexpr std::array<int64_t, 4> kBasesConocidas = {0x00000000, 0x0FD90000, 0x02080000, 0x18300000};
constexpr const char* kMarcador = ".origen_iso.txt";

struct Entrada {
  std::string ruta;
  uint32_t sector = 0;
  uint32_t tam = 0;
  bool dir = false;
};

uint16_t Leer16(const std::vector<uint8_t>& b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t Leer32(const std::vector<uint8_t>& b, size_t o) {
  return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}

bool MagicEn(std::ifstream& f, int64_t base) {
  char buf[kMagicLen];
  f.clear();
  f.seekg(base + 32 * kSector);
  return f.read(buf, kMagicLen) && std::memcmp(buf, kMagic, kMagicLen) == 0;
}

int64_t DetectarBase(std::ifstream& f) {
  for (int64_t base : kBasesConocidas)
    if (MagicEn(f, base)) return base;

  // Barrido por trozos de 16 MB, con solape para no partir el magic.
  constexpr int64_t kTrozo = 16 << 20;
  std::vector<char> buf(kTrozo + kMagicLen);
  for (int64_t pos = 0; pos < (int64_t(1) << 30); pos += kTrozo) {
    f.clear();
    f.seekg(pos);
    f.read(buf.data(), std::streamsize(buf.size()));
    const auto leido = f.gcount();
    if (leido <= 0) break;
    for (std::streamsize i = 0; i + std::streamsize(kMagicLen) <= leido; ++i) {
      if (std::memcmp(buf.data() + i, kMagic, kMagicLen) != 0) continue;
      const int64_t abs = pos + i;
      if (abs % kSector == 0 && abs >= 32 * kSector && MagicEn(f, abs - 32 * kSector))
        return abs - 32 * kSector;
    }
  }
  return -1;
}

std::vector<uint8_t> LeerBloque(std::ifstream& f, int64_t offset, uint32_t tam) {
  std::vector<uint8_t> b(tam);
  f.clear();
  f.seekg(offset);
  f.read(reinterpret_cast<char*>(b.data()), tam);
  b.resize(size_t(std::max<std::streamsize>(0, f.gcount())));  // si viene corto, se sigue con lo leido
  return b;
}

void Recorrer(std::ifstream& f, int64_t base, uint32_t sector, uint32_t tam, const std::string& prefijo,
              std::vector<Entrada>& salida) {
  if (tam == 0 || tam > (256u << 20)) return;
  const auto tabla = LeerBloque(f, base + int64_t(sector) * kSector, tam);

  std::vector<Entrada> hijos;
  std::stack<size_t> pila;
  std::set<size_t> vistos;
  pila.push(0);
  while (!pila.empty()) {
    const size_t off = pila.top();
    pila.pop();
    if (!vistos.insert(off).second || off + 14 > tabla.size()) continue;

    const uint16_t izq = Leer16(tabla, off);
    const uint16_t der = Leer16(tabla, off + 2);
    if (izq != 0 && izq != 0xFFFF) pila.push(size_t(izq) * 4);
    if (der != 0 && der != 0xFFFF) pila.push(size_t(der) * 4);

    const uint8_t largo = tabla[off + 13];
    if (largo == 0 || off + 14 + largo > tabla.size()) continue;
    Entrada e;
    e.ruta.assign(reinterpret_cast<const char*>(tabla.data() + off + 14), largo);
    e.sector = Leer32(tabla, off + 4);
    e.tam = Leer32(tabla, off + 8);
    e.dir = (tabla[off + 12] & 0x10) != 0;
    hijos.push_back(std::move(e));
  }

  // Orden alfabetico sin distinguir mayusculas, como lo extrae el lanzador antiguo.
  std::sort(hijos.begin(), hijos.end(), [](const Entrada& a, const Entrada& b) {
    return std::lexicographical_compare(a.ruta.begin(), a.ruta.end(), b.ruta.begin(), b.ruta.end(),
                                        [](char x, char y) { return std::tolower(x) < std::tolower(y); });
  });
  for (auto& e : hijos) {
    e.ruta = prefijo.empty() ? e.ruta : prefijo + "/" + e.ruta;
    salida.push_back(e);
    if (e.dir) Recorrer(f, base, e.sector, e.tam, e.ruta, salida);
  }
}

}  // namespace

std::string Extraer(const std::filesystem::path& imagen, const std::filesystem::path& destino,
                    const Progreso& progreso, const std::atomic<bool>& cancelar) {
  std::ifstream f(imagen, std::ios::binary);
  if (!f) return "No se puede abrir " + imagen.string();

  const int64_t base = DetectarBase(f);
  if (base < 0) return "No es una imagen de Xbox 360 (no hay sistema de archivos XDVDFS).";

  const auto descriptor = LeerBloque(f, base + 32 * kSector, uint32_t(kSector));
  if (descriptor.size() < size_t(kSector)) return "Descriptor de volumen incompleto.";

  std::vector<Entrada> entradas;
  Recorrer(f, base, Leer32(descriptor, 0x14), Leer32(descriptor, 0x18), "", entradas);
  if (entradas.empty()) return "El sistema de archivos de la imagen esta vacio.";

  const int total = int(std::count_if(entradas.begin(), entradas.end(), [](const Entrada& e) { return !e.dir; }));
  std::error_code ec;
  std::filesystem::remove_all(destino, ec);  // restos de un intento anterior a medias
  std::filesystem::create_directories(destino, ec);

  std::vector<char> buf(1 << 20);
  int hechos = 0;
  for (const auto& e : entradas) {
    if (cancelar.load()) return "Cancelado.";
    if (e.dir) continue;

    const auto ruta = destino / std::filesystem::path(e.ruta).make_preferred();
    std::filesystem::create_directories(ruta.parent_path(), ec);
    std::ofstream salida(ruta, std::ios::binary | std::ios::trunc);
    if (!salida) return "No se puede escribir " + ruta.string();

    f.clear();
    f.seekg(base + int64_t(e.sector) * kSector);
    uint64_t restante = e.tam;
    while (restante > 0) {
      if (cancelar.load()) return "Cancelado.";
      const auto pedir = std::streamsize(std::min<uint64_t>(buf.size(), restante));
      f.read(buf.data(), pedir);
      const auto leido = f.gcount();
      if (leido <= 0) return "Fin de archivo inesperado leyendo " + e.ruta + ". Imagen incompleta?";
      salida.write(buf.data(), leido);
      restante -= uint64_t(leido);
    }
    ++hechos;
    if (progreso && (hechos % 5 == 0 || hechos == total)) progreso(e.ruta, hechos, total);
  }

  if (!EsCarpetaDelJuego(destino))
    return "La imagen no tiene default.xex en la raiz: no parece Need for Speed: Most Wanted de Xbox 360.";
  EscribirMarcador(destino, imagen);
  return {};
}

std::vector<Fichero> Listar(const std::filesystem::path& imagen) {
  std::ifstream f(imagen, std::ios::binary);
  if (!f) return {};
  const int64_t base = DetectarBase(f);
  if (base < 0) return {};
  const auto descriptor = LeerBloque(f, base + 32 * kSector, uint32_t(kSector));
  if (descriptor.size() < size_t(kSector)) return {};
  std::vector<Entrada> entradas;
  Recorrer(f, base, Leer32(descriptor, 0x14), Leer32(descriptor, 0x18), "", entradas);
  std::vector<Fichero> ficheros;
  for (const auto& e : entradas)
    if (!e.dir) ficheros.push_back({e.ruta, base + int64_t(e.sector) * kSector, e.tam});
  return ficheros;
}

std::vector<uint8_t> LeerFichero(const std::filesystem::path& imagen, std::string_view ruta) {
  for (const auto& e : Listar(imagen)) {
    if (e.ruta.size() != ruta.size() ||
        !std::equal(e.ruta.begin(), e.ruta.end(), ruta.begin(),
                    [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
      continue;
    std::ifstream f(imagen, std::ios::binary);
    return LeerBloque(f, e.offset, uint32_t(e.tam));
  }
  return {};
}

bool EsImagenDelJuego(const std::filesystem::path& imagen) {
  for (const auto& e : Listar(imagen))
    if (e.ruta.size() == 11 && std::equal(e.ruta.begin(), e.ruta.end(), "default.xex",
                                          [](char a, char b) { return std::tolower(a) == b; }))
      return true;
  return false;
}

bool EsCarpetaDelJuego(const std::filesystem::path& carpeta) {
  std::error_code ec;
  return std::filesystem::is_regular_file(carpeta / "default.xex", ec);
}

bool CopiaValida(const std::filesystem::path& destino, const std::filesystem::path& imagen) {
  std::error_code ec;
  if (!EsCarpetaDelJuego(destino)) return false;
  std::ifstream m(destino / kMarcador);
  std::string linea;
  if (!m || !std::getline(m, linea)) return false;
  const auto barra = linea.rfind('|');
  if (barra == std::string::npos) return false;
  const auto tam = std::filesystem::file_size(imagen, ec);
  if (ec) return false;
  const auto ruta = std::filesystem::absolute(imagen, ec).u8string();
  return linea.substr(0, barra) == std::string(ruta.begin(), ruta.end()) &&
         linea.substr(barra + 1) == std::to_string(tam);
}

void EscribirMarcador(const std::filesystem::path& destino, const std::filesystem::path& imagen) {
  std::error_code ec;
  const auto ruta = std::filesystem::absolute(imagen, ec).u8string();
  std::ofstream m(destino / kMarcador, std::ios::trunc);
  m << std::string(ruta.begin(), ruta.end()) << '|' << std::filesystem::file_size(imagen, ec) << '\n';
}

}  // namespace nfsmw::iso
