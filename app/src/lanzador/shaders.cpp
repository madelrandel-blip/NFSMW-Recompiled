// nfsmw - genera la biblioteca de shaders desde el lanzador (ver shaders.h).

#include "shaders.h"

#include "../edicion.h"
#include "iso.h"

#include "../nfsmw_shader_library.h"
#include "nfsmw_contenedor.h"
#include "spirv_recompiler.h"

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <lzx.h>
#include <mspack.h>
}

// AES de la clave del XEX. Se incluye el .c como hace el propio SDK
// (sdk/src/system/xex_module.cpp): la app solo compila C++.
#include "crypto/rijndael-alg-fst.c"

namespace nfsmw::shaders {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Be32(const Bytes& b, size_t o) {
  if (o + 4 > b.size()) throw std::runtime_error("cabecera del XEX truncada");
  return Be32(b.data() + o);
}

// ---- SHA-256 (para reconocer el shader de composicion por su contenido) -----

std::string Sha256(const uint8_t* datos, size_t n) {
  static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rot = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
  Bytes m(datos, datos + n);
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back(uint8_t(uint64_t(n) * 8 >> (i * 8)));
  for (size_t b = 0; b < m.size(); b += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = Be32(&m[b + i * 4]);
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rot(e, 6) ^ rot(e, 11) ^ rot(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rot(a, 2) ^ rot(a, 13) ^ rot(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
      hh = g, g = f, f = e, e = d + t1, d = c, c = bb, bb = a, a = t1 + t2;
    }
    h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (uint32_t v : h)
    for (int i = 28; i >= 0; i -= 4) s += kHex[(v >> i) & 0xF];
  return s;
}

// ---- 1. Imagen PE del default.xex ----------------------------------------------
//
// Los discos de tienda van cifrados con AES-128 y comprimidos con LZX "normal".
// Las traducciones de fans suelen ir sin cifrar y sin comprimir o con
// compresion "basica"; se aceptan las tres, como el instalador (lib/xex.js).

constexpr uint8_t kClaveTienda[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                      0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

// AES-128-CBC con IV a cero y sin relleno.
Bytes Descifrar(const uint8_t* datos, size_t n, const uint8_t clave[16]) {
  uint32_t rk[4 * (14 + 1)];
  const int rondas = rijndaelKeySetupDec(rk, clave, 128);
  Bytes salida(n - n % 16);
  uint8_t previo[16] = {};
  for (size_t i = 0; i < salida.size(); i += 16) {
    uint8_t bloque[16];
    rijndaelDecrypt(rk, rondas, datos + i, bloque);
    for (int j = 0; j < 16; ++j) salida[i + j] = bloque[j] ^ previo[j];
    std::memcpy(previo, datos + i, 16);
  }
  return salida;
}

// mspack lee y escribe en memoria, como hace el runtime (sdk/src/system/lzx.cpp).
struct FicheroMemoria {
  mspack_system sys;
  uint8_t* datos;
  size_t tam;
  size_t pos;
};

int LeerMemoria(mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<FicheroMemoria*>(f);
  const size_t k = std::min<size_t>(size_t(n), m->tam - m->pos);
  std::memcpy(buf, m->datos + m->pos, k);
  m->pos += k;
  return int(k);
}
int EscribirMemoria(mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<FicheroMemoria*>(f);
  const size_t k = std::min<size_t>(size_t(n), m->tam - m->pos);
  std::memcpy(m->datos + m->pos, buf, k);
  m->pos += k;
  return int(k);
}
void* Reservar(mspack_system*, size_t n) { return std::calloc(n, 1); }
void Liberar(void* p) { std::free(p); }
void Copiar(void* src, void* dst, size_t n) { std::memcpy(dst, src, n); }

Bytes DescomprimirLzx(Bytes comprimido, uint32_t bits, size_t tam) {
  mspack_system sys{};
  sys.read = LeerMemoria;
  sys.write = EscribirMemoria;
  sys.alloc = Reservar;
  sys.free = Liberar;
  sys.copy = Copiar;
  Bytes imagen(tam);
  FicheroMemoria entrada{sys, comprimido.data(), comprimido.size(), 0};
  FicheroMemoria salida{sys, imagen.data(), imagen.size(), 0};
  lzxd_stream* lzx = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&entrada), reinterpret_cast<mspack_file*>(&salida),
                               int(bits), 0, 0x8000, off_t(tam), 0);
  if (!lzx) throw std::runtime_error("no se pudo iniciar LZX");
  const int r = lzxd_decompress(lzx, off_t(tam));
  lzxd_free(lzx);
  if (r != 0) throw std::runtime_error("fallo LZX (" + std::to_string(r) + ")");
  return imagen;
}

Bytes ImagenXex(const Bytes& xex) {
  if (Be32(xex, 0) != 0x58455832) throw std::runtime_error("default.xex no es un XEX2");
  const uint32_t cabecera = Be32(xex, 8), seguridad = Be32(xex, 16), opcionales = Be32(xex, 20);
  if (cabecera > xex.size() || seguridad + 0x160 > cabecera || 24 + 8ull * opcionales > cabecera)
    throw std::runtime_error("cabecera del XEX fuera de limites");
  uint32_t formato = 0;
  for (uint32_t i = 0; i < opcionales; ++i)
    if (Be32(xex, 24 + 8 * i) == 0x000003FF) formato = Be32(xex, 28 + 8 * i);
  if (!formato || formato + 8 > cabecera) throw std::runtime_error("el XEX no describe su formato");
  const uint32_t info = Be32(xex, formato), tipos = Be32(xex, formato + 4);
  const uint32_t compresion = tipos & 0xFFFF, cifrado = tipos >> 16;
  const uint32_t tam_imagen = Be32(xex, seguridad + 4);
  if (info < 8 || formato + info > cabecera || tam_imagen > 512u * 1024 * 1024)
    throw std::runtime_error("formato del XEX invalido");

  Bytes cuerpo(xex.begin() + cabecera, xex.end());
  if (cifrado == 1) {
    const Bytes sesion = Descifrar(xex.data() + seguridad + 0x150, 16, kClaveTienda);
    cuerpo = Descifrar(cuerpo.data(), cuerpo.size(), sesion.data());
  } else if (cifrado != 0) {
    throw std::runtime_error("cifrado del XEX no soportado");
  }

  Bytes imagen;
  if (compresion == 0) {
    imagen.assign(cuerpo.begin(), cuerpo.begin() + std::min<size_t>(cuerpo.size(), tam_imagen));
    imagen.resize(tam_imagen);
  } else if (compresion == 1) {
    imagen.assign(tam_imagen, 0);
    size_t origen = 0, destino = 0;
    for (uint32_t at = formato + 8; at + 8 <= formato + info; at += 8) {
      const uint32_t datos = Be32(xex, at), ceros = Be32(xex, at + 4);
      if (origen + datos > cuerpo.size() || destino + datos + ceros > tam_imagen)
        throw std::runtime_error("bloque fuera del XEX");
      std::memcpy(imagen.data() + destino, cuerpo.data() + origen, datos);
      origen += datos;
      destino += datos + ceros;
    }
  } else if (compresion == 2) {
    if (info < 36) throw std::runtime_error("falta la informacion LZX del XEX");
    const uint32_t ventana = Be32(xex, formato + 8);
    if (!ventana || (ventana & (ventana - 1))) throw std::runtime_error("ventana LZX invalida");
    uint32_t bits = 0;
    while ((1u << bits) < ventana) ++bits;
    // Cadena de bloques: cada uno empieza con el tamano del siguiente y su
    // SHA-1 (20 bytes), y luego trozos de [tamano de 2 bytes][datos] hasta un 0.
    uint32_t tam_bloque = Be32(xex, formato + 12);
    size_t pos = 0;
    Bytes comprimido;
    while (tam_bloque) {
      const size_t fin = pos + tam_bloque;
      if (tam_bloque < 26 || fin > cuerpo.size()) throw std::runtime_error("bloque fuera del XEX");
      const uint32_t siguiente = Be32(cuerpo, pos);
      size_t p = pos + 24;
      for (;;) {
        if (p + 2 > fin) throw std::runtime_error("falta el final de un bloque del XEX");
        const size_t n = size_t(cuerpo[p]) << 8 | cuerpo[p + 1];
        p += 2;
        if (!n) break;
        if (p + n > fin) throw std::runtime_error("trozo fuera del bloque del XEX");
        comprimido.insert(comprimido.end(), cuerpo.begin() + p, cuerpo.begin() + p + n);
        p += n;
      }
      pos = fin;
      tam_bloque = siguiente;
    }
    imagen = DescomprimirLzx(std::move(comprimido), bits, tam_imagen);
  } else {
    throw std::runtime_error("compresion del XEX no soportada");
  }
  if (imagen.size() < 2 || imagen[0] != 'M' || imagen[1] != 'Z')
    throw std::runtime_error("la imagen del XEX no tiene la firma MZ");
  return imagen;
}

// ---- 2. Contenedores de shaders ------------------------------------------------
//
// Firma 10 2A 0E 00 (pixel) o 10 2A 0E 01 (vertex), probada en cada byte, y se
// acepta si los tamanos de la cabecera cuadran (lib/containers.js). Nombres
// "<p|v>_<prefijo><contador>" con un contador por buscador.

struct Contenedor {
  std::string nombre;  // sin .bin
  Bytes datos;
};

class Buscador {
 public:
  explicit Buscador(std::string prefijo) : prefijo_(std::move(prefijo)) {}

  void EmpezarFichero(uint64_t tam) {
    tam_fichero_ = tam;
    arrastre_.clear();
    desplazamiento_ = 0;
  }

  void Anadir(const uint8_t* trozo, size_t n, bool final, std::vector<Contenedor>& salida) {
    constexpr size_t kMaximo = 65536, kAdelanto = kMaximo + 24;
    Bytes datos;
    datos.reserve(arrastre_.size() + n);
    datos.insert(datos.end(), arrastre_.begin(), arrastre_.end());
    datos.insert(datos.end(), trozo, trozo + n);
    const uint64_t base = desplazamiento_;
    const size_t limite = final ? datos.size() : (datos.size() > kAdelanto ? datos.size() - kAdelanto : 0);
    for (size_t i = 0; i < limite; ++i) {
      const uint8_t* p = datos.data() + i;
      if (p[0] != 0x10 || i + 4 > datos.size() || p[1] != 0x2A || p[2] != 0x0E || p[3] > 1) continue;
      if (i + 24 > datos.size()) continue;
      const uint64_t v = Be32(p + 4), f = Be32(p + 8), c = Be32(p + 16), s = Be32(p + 20);
      const uint64_t restante = tam_fichero_ - (base + i);
      if (v < 24 || v + f > kMaximo || v + f > restante || c < 24 || c >= v || s < 24 || s + 24 > v || !f) continue;
      if (i + v + f > datos.size()) continue;
      char nombre[32];
      std::snprintf(nombre, sizeof(nombre), "%c_%s%06u", p[3] ? 'v' : 'p', prefijo_.c_str(), contador_++);
      salida.push_back({nombre, Bytes(p, p + v + f)});
    }
    arrastre_.assign(datos.begin() + std::ptrdiff_t(limite), datos.end());
    desplazamiento_ = base + limite;
    if (final) arrastre_.clear();
  }

 private:
  std::string prefijo_;
  unsigned contador_ = 0;
  uint64_t tam_fichero_ = 0;
  uint64_t desplazamiento_ = 0;
  Bytes arrastre_;
};

// A game file to scan: a whole file of an extracted copy, or its span inside the .iso.
struct Origen {
  std::string ruta;  // relative, lowercase, with '/'
  fs::path fichero;  // the host file to read
  int64_t offset = 0;
  uint64_t tam = 0;
};

// Ficheros del disco con shaders: NFS/*.BIN (los datos, no los videos), en el
// orden del instalador: ruta en minusculas, de la A a la Z. Igual de una carpeta que de una ISO, para que los
// contenedores salgan con los mismos nombres.
std::vector<Origen> FicherosConShaders(const fs::path& juego) {
  std::vector<Origen> lista;
  auto anadir = [&](std::string rel, fs::path fichero, int64_t offset, uint64_t tam) {
    std::transform(rel.begin(), rel.end(), rel.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (rel.rfind("nfs/", 0) == 0 && rel.find('/', 4) == std::string::npos && rel.size() > 8 &&
        rel.compare(rel.size() - 4, 4, ".bin") == 0 && tam >= 24 && tam <= (1ull << 30))
      lista.push_back({std::move(rel), std::move(fichero), offset, tam});
  };
  std::error_code ec;
  if (fs::is_regular_file(juego, ec)) {
    for (const auto& f : iso::Listar(juego)) anadir(f.ruta, juego, f.offset, f.tam);
  } else {
    for (const auto& e : fs::recursive_directory_iterator(juego, ec)) {
      if (!e.is_regular_file(ec)) continue;
      anadir(fs::relative(e.path(), juego, ec).generic_string(), e.path(), 0, e.file_size(ec));
    }
  }
  std::sort(lista.begin(), lista.end(), [](const Origen& a, const Origen& b) { return a.ruta < b.ruta; });
  return lista;
}

// ---- 3. Translation ------------------------------------------------------------

// The race VisualTreatment container: its name changes between editions, its content does not. Its radial blur
// blend gets the SPEC_CONSTANT_SIN_DESENFOQUE switch (SpirvRecompilerOptions::compositionBlurSwitch).
constexpr const char* kComposicionSha256 = "e80cca037bd4becd8a192daa4583e3a3a9281e06436015746583f30701a296f9";
constexpr uint32_t kMarcaSombra = 0x5E3B1A84;

bool TieneMarcaSombra(const std::vector<uint32_t>& w) {
  if (w.size() < 5 || w[0] != 0x07230203) return false;
  for (size_t i = 5; i < w.size();) {
    const uint32_t n = w[i] >> 16;
    if (!n || i + n > w.size()) return false;
    if ((w[i] & 0xFFFF) == 43 && n == 4 && w[i + 3] == kMarcaSombra) return true;
    i += n;
  }
  return false;
}

// Written next to the library once it is complete. A library without it, or with another value, was made by an
// older generator (the HLSL + DXC one, before the Xenos multiply rule) and is generated again.
constexpr const char* kGenerador = "nfsmw-spirv-direct 1";

fs::path RutaSello(const fs::path& biblioteca) { return fs::path(biblioteca).concat(".generator"); }

}  // namespace

fs::path RutaBiblioteca() { return edicion::BibliotecaPropia(); }

std::string Sha256Datos(const uint8_t* datos, size_t n) { return Sha256(datos, n); }

std::string Sha256Fichero(const fs::path& ruta) {
  std::ifstream in(ruta, std::ios::binary);
  if (!in) return {};
  const Bytes datos((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return Sha256(datos.data(), datos.size());
}

bool PuedeGenerar() {
  // The translator is built in: nothing has to be next to the executable.
  return true;
}

bool BibliotecaActual(const fs::path& biblioteca) {
  std::error_code ec;
  if (!fs::is_regular_file(biblioteca, ec)) return false;
  std::ifstream f(RutaSello(biblioteca));
  std::string sello;
  std::getline(f, sello);
  return sello == kGenerador;
}

std::string Generar(const fs::path& juego, const fs::path& salida, const Progreso& progreso,
                    const std::atomic<bool>& cancelar) try {
  auto avisar = [&](Fase f, int h, int t) {
    if (progreso) progreso(f, h, t);
  };

  // 1. Imagen del ejecutable.
  avisar(Fase::kImagen, 0, 1);
  Bytes xex;
  std::error_code ec_juego;
  if (fs::is_regular_file(juego, ec_juego)) {
    xex = iso::LeerFichero(juego, "default.xex");
    if (xex.empty()) return "No se encuentra default.xex en la imagen del juego.";
  } else {
    std::ifstream f(juego / "default.xex", std::ios::binary);
    if (!f) return "No se encuentra default.xex en la carpeta del juego.";
    xex.assign(std::istreambuf_iterator<char>(f), {});
  }
  const Bytes imagen = ImagenXex(xex);

  // 2. Contenedores: primero el disco, en orden, y despues el ejecutable.
  std::vector<Contenedor> disco, ejecutable;
  const auto ficheros = FicherosConShaders(juego);
  if (ficheros.empty()) return "No se encuentran los datos del juego (NFS/*.BIN).";
  Buscador buscador_disco("");
  std::vector<char> buf(64 << 20);
  for (size_t i = 0; i < ficheros.size(); ++i) {
    avisar(Fase::kBuscar, int(i), int(ficheros.size()));
    std::ifstream f(ficheros[i].fichero, std::ios::binary);
    f.seekg(ficheros[i].offset);
    const uint64_t tam = ficheros[i].tam;
    buscador_disco.EmpezarFichero(tam);
    uint64_t leido = 0;
    while (leido < tam) {
      if (cancelar) return "Cancelado.";
      f.read(buf.data(), std::streamsize(std::min<uint64_t>(buf.size(), tam - leido)));
      const auto n = size_t(f.gcount());
      if (!n) return "No se pudo leer " + ficheros[i].ruta;
      leido += n;
      buscador_disco.Anadir(reinterpret_cast<const uint8_t*>(buf.data()), n, leido >= tam, disco);
    }
  }
  Buscador buscador_xex("xex_");
  buscador_xex.EmpezarFichero(imagen.size());
  buscador_xex.Anadir(imagen.data(), imagen.size(), true, ejecutable);
  std::vector<Contenedor> contenedores = std::move(disco);
  contenedores.insert(contenedores.end(), std::make_move_iterator(ejecutable.begin()),
                      std::make_move_iterator(ejecutable.end()));
  REXLOG_INFO("[shaders] {} contenedores encontrados", contenedores.size());

  std::string composicion;
  for (const auto& c : contenedores)
    if (Sha256(c.datos.data(), c.datos.size()) == kComposicionSha256) composicion = c.nombre;
  if (composicion.empty()) return "No se encuentra el shader de composicion: el juego no parece compatible.";

  // 3. Microcode -> SPIR-V, directly (spirv_recompiler.h): no HLSL and no DXC. The shadow map samples become
  // tfetch2DSombraMin and the composite gets its blur switch inside the translator.
  SpirvRecompilerOptions opciones;
  opciones.legacyMultiply = true;
  opciones.sanitizeColorOutputs = true;
  std::vector<nfsmw::native::Shader> paquete;
  uint32_t sombras = 0, notas = 0;
  for (size_t i = 0; i < contenedores.size(); ++i) {
    if (cancelar) return "Cancelled.";
    avisar(Fase::kTraducir, int(i), int(contenedores.size()));
    const auto& datos = contenedores[i].datos;
    const uint32_t firma = Be32(datos.data()) & 0xFFFFFF00u;
    if (firma != 0x102A0E00u && firma != 0x102A1100u) return contenedores[i].nombre + ": unknown container signature";
    nfsmw::Flujo flujo;
    const Bytes conv = firma == 0x102A0E00u ? nfsmw::Convertir2005(datos, flujo) : datos;
    opciones.compositionBlurSwitch = contenedores[i].nombre == composicion;
    SpirvRecompileResult traducido = RecompileToSpirv(conv.data(), opciones);
    if (traducido.spirv.empty()) return contenedores[i].nombre + ": " + traducido.error;
    if (traducido.shadowFetches && !TieneMarcaSombra(traducido.spirv))
      return contenedores[i].nombre + ": the SPIR-V lacks the shadow minimum marker";
    sombras += traducido.shadowFetches;
    notas += uint32_t(traducido.notes.size());
    nfsmw::native::Shader s;
    s.original = datos;
    s.spirv = std::move(traducido.spirv);
    paquete.push_back(std::move(s));
  }
  if (!sombras) return "No shadow map sample was found to rewrite.";
  REXLOG_INFO("[shaders] {} shaders translated to SPIR-V ({} shadow map samples; {} texture LOD or call instructions "
              "skipped, as the HLSL path did)",
              paquete.size(), sombras, notas);

  // 6. Empaquetado, con el mismo codigo que la carga el renderizador.
  avisar(Fase::kEmpaquetar, 0, 1);
  const Bytes biblioteca = nfsmw::native::EmpaquetarShaders(std::move(paquete));
  const fs::path temporal = fs::path(salida).concat(".tmp");
  {
    std::ofstream f(temporal, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(biblioteca.data()), std::streamsize(biblioteca.size())) || !f.flush())
      return "No se pudo escribir " + salida.string();
  }
  std::error_code ec;
  fs::rename(temporal, salida, ec);
  if (ec) return "No se pudo guardar " + salida.string() + ": " + ec.message();
  REXLOG_INFO("[shaders] biblioteca de {} bytes, SHA-256 {}", biblioteca.size(),
              Sha256(biblioteca.data(), biblioteca.size()));
  {
    // Last, so an interrupted generation leaves no stamp and is redone.
    std::ofstream sello(RutaSello(salida), std::ios::trunc);
    sello << kGenerador << '\n';
  }
  avisar(Fase::kEmpaquetar, 1, 1);
  return {};
} catch (const std::exception& e) {
  return e.what();
}

}  // namespace nfsmw::shaders
