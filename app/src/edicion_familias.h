// nfsmw - editions that run on the game module of another one.
//
// The French, German and Italian PAL editions are the Spanish build with another language number
// (nfsmw_idioma_edicion.cpp), so the Spanish module runs them. Every other edition is its own family. Shared by
// edicion.cpp and the host executable (anfitrion/main.cpp), which picks the module before any of the game runs.

#pragma once

#include <string_view>

namespace nfsmw::edicion {

struct FamiliaEdicion {
  const char* edicion;
  const char* familia;
};
inline constexpr FamiliaEdicion kFamilias[] = {{"fr", "es"}, {"de", "es"}, {"it", "es"}};

// The edition whose module runs this one: itself unless it is in kFamilias.
inline std::string_view FamiliaDe(std::string_view edicion) {
  for (const auto& f : kFamilias)
    if (edicion == f.edicion) return f.familia;
  return edicion;
}

}  // namespace nfsmw::edicion
