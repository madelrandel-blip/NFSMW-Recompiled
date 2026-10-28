// nfsmw - which edition of the game this executable was built for, and which one a game copy is.
//
// Every different default.xex (PAL Spanish, USA, European PAL) is a different program and needs its own game
// module; the French, German and Italian PAL editions run on the Spanish one (edicion_familias.h). On Windows the
// modules are nfsmw_<edition>.dll, loaded by the one nfsmw.exe (anfitrion/main.cpp); elsewhere, executables
// nfsmw_<edition> next to each other. The launcher recognizes the chosen copy by the SHA-256 of its default.xex
// and, if another module runs it, relaunches with that one. Each edition also has its own shader library.

#pragma once

#include <filesystem>
#include <string>

namespace nfsmw::edicion {

// The edition of this executable ("es", "usa"...), set by CMake (NFSMW_EDICION).
const char* Propia();

// The edition of a game folder (with default.xex) or .iso image, or empty if the executable is not a known one.
std::string Detectar(const std::filesystem::path& carpeta_juego);

// The edition whose executable runs this one (the French PAL runs on the Spanish one); itself if unknown.
std::string Familia(const std::string& edicion);

// Display name, e.g. "PAL (Español)" or "USA".
const char* Nombre(const std::string& edicion);

// The game module of an edition family, next to the executable: nfsmw_<edition>.dll on Windows, an executable
// elsewhere.
std::filesystem::path Modulo(const std::string& edicion);

// Shader library of an edition, next to the executable. Each edition has its own, also those that share an
// executable: the French PAL runs on the Spanish one but has some different shaders.
std::filesystem::path Biblioteca(const std::string& edicion);
// The library of the copy being played (nfsmw_edicion_copia, set by the launcher) if this executable runs it, else
// this executable's.
std::filesystem::path BibliotecaPropia();

}  // namespace nfsmw::edicion
