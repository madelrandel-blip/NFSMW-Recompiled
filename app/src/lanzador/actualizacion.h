// nfsmw - aviso de version nueva.
//
// Consulta la ultima release del repositorio publico y la compara con la
// version de este ejecutable. Solo avisa: no descarga nada. El ejecutable
// jugable lleva dentro el codigo del juego y no se distribuye; la release
// publica explica como conseguir la version nueva.

#pragma once

#include <string>

namespace nfsmw::actualizacion {

// Version de este ejecutable (NFSMW_VERSION, la pone CMake).
const char* VersionActual();

struct Resultado {
  bool hay_nueva = false;
  std::string version;  // de la release, p. ej. "0.2.0"
  std::string url;      // pagina de la release
};

// Bloquea mientras consulta (unos cientos de ms): llamarlo desde un hilo.
// Sin red, o si algo falla, devuelve hay_nueva = false.
Resultado Comprobar();

}  // namespace nfsmw::actualizacion
