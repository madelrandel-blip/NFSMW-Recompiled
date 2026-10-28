// nfsmw - editions of the game (see edicion.h).

#include "edicion.h"

#include "edicion_familias.h"

#include "lanzador/iso.h"
#include "lanzador/shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>

#ifndef NFSMW_EDICION
#define NFSMW_EDICION "es"
#endif

REXCVAR_DEFINE_STRING(nfsmw_edicion_copia, "", "NFSMW",
                      "Edicion de la copia del juego con la que se juega (la pone el lanzador): elige su biblioteca de "
                      "shaders, que puede ser otra que la del ejecutable (el PAL frances usa el ejecutable espanol, "
                      "con algunos shaders distintos)");
// Read by nfsmw.exe (anfitrion/main.cpp) before the module loads; declared here so the flag is a known one.
REXCVAR_DEFINE_STRING(nfsmw_modulo, "", "NFSMW",
                      "Modulo del juego que carga nfsmw.exe (nfsmw_<edicion>.dll); lo pone el lanzador al relanzar");

namespace nfsmw::edicion {
namespace {

struct Conocida {
  const char* edicion;
  const char* sha256;  // of default.xex
  const char* nombre;
};
constexpr Conocida kConocidas[] = {
    {"es", "aad15fc218d034de3131f746e0b763dd8049a40dd676efc87667fcaf4fc72694", "PAL (Español)"},
    {"fr", "229c5ed5dcc67871982000fb923acfd59494f21f30996b25f36b30a8d85c9b7b", "PAL (Français)"},
    {"de", "23ece29d18724ea28971d52f936964d127148bd2359ff24795da7442b9069bf4", "PAL (Deutsch)"},
    {"it", "acb09332bcafc15e8e21dc15062f1963804a3176fa4db48fea7f6c8afbdff066", "PAL (Italiano)"},
    {"usa", "aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c", "USA"},
    {"eu", "e2ea2ff11bf84cb30430d824d7bfe699a3acf3e7400d21b9a7c938e3d7a10474", "PAL (Europa)"},
};

}  // namespace

const char* Propia() { return NFSMW_EDICION; }

std::string Detectar(const std::filesystem::path& carpeta_juego) {
  std::string sha;
  std::error_code ec;
  if (std::filesystem::is_regular_file(carpeta_juego, ec)) {
    const auto xex = iso::LeerFichero(carpeta_juego, "default.xex");
    if (!xex.empty()) sha = shaders::Sha256Datos(xex.data(), xex.size());
  } else {
    sha = shaders::Sha256Fichero(carpeta_juego / "default.xex");
  }
  for (const auto& c : kConocidas)
    if (sha == c.sha256) return c.edicion;
  return {};
}

std::string Familia(const std::string& edicion) { return std::string(FamiliaDe(edicion)); }

const char* Nombre(const std::string& edicion) {
  for (const auto& c : kConocidas)
    if (edicion == c.edicion) return c.nombre;
  return "?";
}

std::filesystem::path Modulo(const std::string& edicion) {
  const auto carpeta = rex::filesystem::GetExecutableFolder();
#if defined(_WIN32)
  return carpeta / ("nfsmw_" + edicion + ".dll");
#else
  return carpeta / (edicion == "es" ? std::string("nfsmw") : "nfsmw_" + edicion);
#endif
}

std::filesystem::path BibliotecaPropia() {
  const std::string copia = REXCVAR_GET(nfsmw_edicion_copia);
  return Biblioteca(!copia.empty() && Familia(copia) == Propia() ? copia : std::string(Propia()));
}

std::filesystem::path Biblioteca(const std::string& edicion) {
  return rex::filesystem::GetExecutableFolder() /
         (edicion == "es" ? std::string("nfsmw_shaders.nfsp") : "nfsmw_shaders_" + edicion + ".nfsp");
}

}  // namespace nfsmw::edicion
