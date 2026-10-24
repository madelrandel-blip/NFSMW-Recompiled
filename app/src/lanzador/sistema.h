// nfsmw - lo que el lanzador le pide al sistema operativo: elegir un fichero o
// una carpeta con el dialogo nativo, abrir enlaces y carpetas, y reiniciar.

#pragma once

#include <filesystem>
#include <string>

namespace nfsmw::sistema {

// Dialogos nativos. Devuelven vacio si se cancela o si el sistema no tiene
// dialogo (en Linux se usa zenity si esta instalado). Bloquean hasta cerrarse.
std::filesystem::path ElegirIso(const std::filesystem::path& inicial);
std::filesystem::path ElegirCarpeta(const std::filesystem::path& inicial);

// Abre una URL en el navegador o una carpeta en el explorador.
void Abrir(const std::string& destino);

// Lanza otra vez el ejecutable con `argumentos` (separados por espacios).
// Devuelve false si no se pudo; el llamador sigue con el proceso actual.
bool Relanzar(const std::string& argumentos);

// Lanza otro ejecutable (el de otra edicion del juego) con `argumentos`.
bool Lanzar(const std::filesystem::path& exe, const std::string& argumentos);

// Resolucion actual del monitor principal, en pixeles reales. {0, 0} si no se sabe.
struct Resolucion {
  int ancho = 0, alto = 0;
};
Resolucion ResolucionPantalla();

}  // namespace nfsmw::sistema
