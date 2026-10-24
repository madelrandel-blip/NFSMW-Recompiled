// nfsmw - editions of the game (see edicion.h).

#include "edicion.h"

#include "lanzador/shaders.h"

#include <rex/filesystem.h>

#ifndef NFSMW_EDICION
#define NFSMW_EDICION "es"
#endif

namespace nfsmw::edicion {
namespace {

struct Conocida {
  const char* edicion;
  const char* sha256;  // of default.xex
  const char* nombre;
};
constexpr Conocida kConocidas[] = {
    {"es", "aad15fc218d034de3131f746e0b763dd8049a40dd676efc87667fcaf4fc72694", "PAL (Español)"},
    {"usa", "aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c", "USA"},
};

}  // namespace

const char* Propia() { return NFSMW_EDICION; }

std::string Detectar(const std::filesystem::path& carpeta_juego) {
  const std::string sha = shaders::Sha256Fichero(carpeta_juego / "default.xex");
  for (const auto& c : kConocidas)
    if (sha == c.sha256) return c.edicion;
  return {};
}

const char* Nombre(const std::string& edicion) {
  for (const auto& c : kConocidas)
    if (edicion == c.edicion) return c.nombre;
  return "?";
}

std::filesystem::path Ejecutable(const std::string& edicion) {
  const auto carpeta = rex::filesystem::GetExecutableFolder();
#if defined(_WIN32)
  return carpeta / (edicion == "es" ? std::string("nfsmw.exe") : "nfsmw_" + edicion + ".exe");
#else
  return carpeta / (edicion == "es" ? std::string("nfsmw") : "nfsmw_" + edicion);
#endif
}

std::filesystem::path Biblioteca(const std::string& edicion) {
  return rex::filesystem::GetExecutableFolder() /
         (edicion == "es" ? std::string("nfsmw_shaders.nfsp") : "nfsmw_shaders_" + edicion + ".nfsp");
}

}  // namespace nfsmw::edicion
