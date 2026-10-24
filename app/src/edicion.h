// nfsmw - which edition of the game this executable was built for, and which one a game copy is.
//
// Every edition (PAL Spanish, USA...) has its own default.xex and needs its own executable. They ship side by
// side in one folder: nfsmw.exe for the PAL Spanish edition and nfsmw_<edition>.exe for the others. The
// launcher in any of them recognizes the chosen copy by the SHA-256 of its default.xex and, if it belongs to
// another edition, starts that executable instead. Each edition also has its own shader library.

#pragma once

#include <filesystem>
#include <string>

namespace nfsmw::edicion {

// The edition of this executable ("es", "usa"...), set by CMake (NFSMW_EDICION).
const char* Propia();

// The edition of a game folder (with default.xex), or empty if the executable is not a known one.
std::string Detectar(const std::filesystem::path& carpeta_juego);

// Display name, e.g. "PAL (Español)" or "USA".
const char* Nombre(const std::string& edicion);

// Executable of an edition, next to this one.
std::filesystem::path Ejecutable(const std::string& edicion);

// Shader library of an edition, next to the executable.
std::filesystem::path Biblioteca(const std::string& edicion);
inline std::filesystem::path BibliotecaPropia() { return Biblioteca(Propia()); }

}  // namespace nfsmw::edicion
