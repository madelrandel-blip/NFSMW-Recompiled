// nfsmw - genera la biblioteca de shaders desde el lanzador (ver shaders.h).

#include "shaders.h"

#include "../nfsmw_shader_library.h"
#include "nfsmw_contenedor.h"
#include "shader_recompiler.h"
#include "shader_common_texto.h"  // generado por CMake: kShaderCommon con shaders/shader_common.h

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
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

#if defined(_WIN32) && defined(NFSMW_CON_DXC)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>  // IUnknown e IStream: WIN32_LEAN_AND_MEAN los deja fuera
#include <objidl.h>
#include <dxcapi.h>
#endif

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

// Ficheros del disco con shaders: NFS/*.BIN (los datos, no los videos), en el
// orden del instalador: ruta en minusculas, de la A a la Z.
std::vector<fs::path> FicherosConShaders(const fs::path& juego) {
  std::vector<std::pair<std::string, fs::path>> lista;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(juego, ec)) {
    if (!e.is_regular_file(ec)) continue;
    std::string rel = fs::relative(e.path(), juego, ec).generic_string();
    std::string min = rel;
    std::transform(min.begin(), min.end(), min.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto tam = e.file_size(ec);
    if (min.rfind("nfs/", 0) == 0 && min.find('/', 4) == std::string::npos && min.size() > 8 &&
        min.compare(min.size() - 4, 4, ".bin") == 0 && tam >= 24 && tam <= (1ull << 30))
      lista.push_back({min, e.path()});
  }
  std::sort(lista.begin(), lista.end());
  std::vector<fs::path> rutas;
  for (auto& [_, ruta] : lista) rutas.push_back(ruta);
  return rutas;
}

// ---- 4. Reescrituras de fuente (lib/shaders.js) ---------------------------------

size_t Contar(const std::string& texto, const std::string& aguja) {
  size_t n = 0;
  for (size_t i = texto.find(aguja); i != std::string::npos; i = texto.find(aguja, i + aguja.size())) ++n;
  return n;
}
void Reemplazar(std::string& texto, const std::string& viejo, const std::string& nuevo) {
  for (size_t i = texto.find(viejo); i != std::string::npos; i = texto.find(viejo, i + nuevo.size()))
    texto.replace(i, viejo.size(), nuevo);
}

const std::string kSombraViejo = "tfetch2D(SHADOWMAP_SAMPLER_";
const std::string kSombraNuevo = "tfetch2DSombra(SHADOWMAP_SAMPLER_";
const std::string kMinViejo =
    "tfetch2DSombra(SHADOWMAP_SAMPLER_Texture2DDescriptorIndex, SHADOWMAP_SAMPLER_SamplerDescriptorIndex,";
const std::string kMinNuevo =
    "tfetch2DSombraMin(SHADOWMAP_SAMPLER_Texture2DDescriptorIndex, SHADOWMAP_SAMPLER_Texture3DDescriptorIndex, "
    "SHADOWMAP_SAMPLER_SamplerDescriptorIndex,";
const std::string kDefine3d = "#define SHADOWMAP_SAMPLER_Texture3DDescriptorIndex";
const std::string kDesenfoqueViejo = "r5.xyz = r5.xyz * r0.xxx + r3.xyz;";
const std::string kDesenfoqueNuevo =
    "r5.xyz = (g_SpecConstants() & SPEC_CONSTANT_SIN_DESENFOQUE) ? r3.xyz : (r5.xyz * r0.xxx + r3.xyz);";
// El contenedor del VisualTreatment de carrera: cambia de nombre entre ediciones, no de contenido.
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

// ---- 5. DXC ------------------------------------------------------------------

#if defined(_WIN32) && defined(NFSMW_CON_DXC)
class Dxc {
 public:
  Dxc() {
    modulo_ = LoadLibraryW((rex::filesystem::GetExecutableFolder() / "dxcompiler.dll").wstring().c_str());
    if (!modulo_) return;
    auto crear = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(modulo_, "DxcCreateInstance"));
    if (!crear || FAILED(crear(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&compilador_))))
      compilador_ = nullptr;
  }
  ~Dxc() {
    if (compilador_) compilador_->Release();
    // dxcompiler.dll no se descarga: DXC no lo soporta bien en caliente.
  }
  bool Valido() const { return compilador_ != nullptr; }

  // Mismos argumentos que dxc_web.cpp del instalador.
  std::vector<uint32_t> Compilar(const std::string& nombre, const std::string& hlsl, bool vertice, std::string& error) {
    const std::wstring wnombre(nombre.begin(), nombre.end());
    std::vector<const wchar_t*> args = {wnombre.c_str(), L"-spirv", L"-T", vertice ? L"vs_6_6" : L"ps_6_6",
                                        L"-E", L"main", L"-HV", L"2021", L"-fspv-target-env=vulkan1.2",
                                        L"-fvk-use-dx-layout", L"-Werror=parameter-usage"};
    if (vertice) args.push_back(L"-fvk-invert-y");
    DxcBuffer fuente{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    IDxcResult* resultado = nullptr;
    std::vector<uint32_t> spirv;
    if (FAILED(compilador_->Compile(&fuente, args.data(), UINT32(args.size()), nullptr, __uuidof(IDxcResult),
                                    reinterpret_cast<void**>(&resultado))) ||
        !resultado) {
      error = "DXC no pudo compilar " + nombre;
      return spirv;
    }
    HRESULT estado = S_OK;
    resultado->GetStatus(&estado);
    if (FAILED(estado)) {
      IDxcBlobUtf8* errores = nullptr;
      if (SUCCEEDED(resultado->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&errores),
                                         nullptr)) &&
          errores) {
        error = nombre + ": " + std::string(errores->GetStringPointer(), errores->GetStringLength());
        errores->Release();
      } else {
        error = "DXC rechazo " + nombre;
      }
    } else {
      IDxcBlob* blob = nullptr;
      if (SUCCEEDED(resultado->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&blob), nullptr)) &&
          blob) {
        const auto* p = static_cast<const uint32_t*>(blob->GetBufferPointer());
        spirv.assign(p, p + blob->GetBufferSize() / 4);
        blob->Release();
      }
    }
    resultado->Release();
    return spirv;
  }

 private:
  HMODULE modulo_ = nullptr;
  IDxcCompiler3* compilador_ = nullptr;
};
#endif

}  // namespace

fs::path RutaBiblioteca() { return rex::filesystem::GetExecutableFolder() / "nfsmw_shaders.nfsp"; }

bool PuedeGenerar() {
#if defined(_WIN32) && defined(NFSMW_CON_DXC)
  std::error_code ec;
  return fs::is_regular_file(rex::filesystem::GetExecutableFolder() / "dxcompiler.dll", ec);
#else
  return false;
#endif
}

std::string Generar(const fs::path& juego, const fs::path& salida, const Progreso& progreso,
                    const std::atomic<bool>& cancelar) try {
#if !(defined(_WIN32) && defined(NFSMW_CON_DXC))
  (void)juego, (void)salida, (void)progreso, (void)cancelar;
  return "Esta compilacion no trae DXC: no puede generar los shaders.";
#else
  auto avisar = [&](Fase f, int h, int t) {
    if (progreso) progreso(f, h, t);
  };

  // 1. Imagen del ejecutable.
  avisar(Fase::kImagen, 0, 1);
  Bytes xex;
  {
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
    std::ifstream f(ficheros[i], std::ios::binary);
    std::error_code ec;
    const uint64_t tam = fs::file_size(ficheros[i], ec);
    buscador_disco.EmpezarFichero(tam);
    uint64_t leido = 0;
    while (leido < tam) {
      if (cancelar) return "Cancelado.";
      f.read(buf.data(), std::streamsize(std::min<uint64_t>(buf.size(), tam - leido)));
      const auto n = size_t(f.gcount());
      if (!n) return "No se pudo leer " + ficheros[i].string();
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

  // 3. Traduccion a HLSL.
  const std::string_view comun(kShaderCommon);
  std::vector<std::string> hlsl(contenedores.size());
  for (size_t i = 0; i < contenedores.size(); ++i) {
    if (cancelar) return "Cancelado.";
    avisar(Fase::kTraducir, int(i), int(contenedores.size()));
    const auto& datos = contenedores[i].datos;
    const uint32_t firma = Be32(datos.data()) & 0xFFFFFF00u;
    if (firma != 0x102A0E00u && firma != 0x102A1100u)
      return contenedores[i].nombre + ": firma de contenedor desconocida";
    nfsmw::Flujo flujo;
    const Bytes conv = firma == 0x102A0E00u ? nfsmw::Convertir2005(datos, flujo) : datos;
    ShaderRecompiler recompilador;
    recompilador.recompile(conv.data(), comun);
    if (recompilador.out.empty()) return contenedores[i].nombre + ": la traduccion no produjo nada";
    hlsl[i] = std::move(recompilador.out);
  }

  // 4. Reescrituras.
  size_t sombras = 0;
  for (auto& t : hlsl) {
    const size_t n = Contar(t, kSombraViejo);
    if (n) sombras += n, Reemplazar(t, kSombraViejo, kSombraNuevo);
  }
  if (!sombras) return "No se reescribio ninguna lectura del mapa de sombras.";
  size_t minimos = 0;
  for (size_t i = 0; i < hlsl.size(); ++i) {
    const size_t n = Contar(hlsl[i], kSombraNuevo);
    if (!n) continue;
    if (Contar(hlsl[i], kMinViejo) != n || hlsl[i].find(kDefine3d) == std::string::npos)
      return contenedores[i].nombre + ": lectura de sombras con una forma inesperada";
    Reemplazar(hlsl[i], kMinViejo, kMinNuevo);
    minimos += n;
  }
  if (!minimos) return "No se reescribio ninguna lectura de sombra por minimo.";
  for (size_t i = 0; i < hlsl.size(); ++i) {
    if (contenedores[i].nombre != composicion) continue;
    if (Contar(hlsl[i], kDesenfoqueViejo) != 1) return "No se encuentra la mezcla del desenfoque de la composicion.";
    hlsl[i].replace(hlsl[i].find(kDesenfoqueViejo), kDesenfoqueViejo.size(), kDesenfoqueNuevo);
  }

  // 5. SPIR-V.
  Dxc dxc;
  if (!dxc.Valido()) return "No se pudo cargar dxcompiler.dll.";
  std::vector<nfsmw::native::Shader> paquete;
  for (size_t i = 0; i < contenedores.size(); ++i) {
    if (cancelar) return "Cancelado.";
    avisar(Fase::kCompilar, int(i), int(contenedores.size()));
    std::string error;
    const bool vertice = contenedores[i].nombre[0] == 'v';
    auto spirv = dxc.Compilar(contenedores[i].nombre, hlsl[i], vertice, error);
    if (spirv.empty()) return error.empty() ? contenedores[i].nombre + ": DXC no devolvio nada" : error;
    if (hlsl[i].find("tfetch2DSombraMin(SHADOWMAP_SAMPLER_") != std::string::npos && !TieneMarcaSombra(spirv))
      return contenedores[i].nombre + ": el SPIR-V no trae la marca de sombra por minimo";
    nfsmw::native::Shader s;
    s.original = contenedores[i].datos;
    s.spirv = std::move(spirv);
    paquete.push_back(std::move(s));
  }

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
  avisar(Fase::kEmpaquetar, 1, 1);
  return {};
#endif
} catch (const std::exception& e) {
  return e.what();
}

}  // namespace nfsmw::shaders
