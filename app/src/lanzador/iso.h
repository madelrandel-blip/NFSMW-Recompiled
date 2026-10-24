// nfsmw - extractor de imagenes .iso de Xbox 360 (XDVDFS) a una carpeta.
//
// El SDK exige que --game_data_root sea una carpeta (ReXApp::ConstructRuntime)
// y no sabe montar imagenes, asi que el lanzador extrae la ISO una vez y
// despues arranca con esa copia.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace nfsmw::iso {

// Progreso: fichero actual, ficheros hechos y total. Se llama desde el hilo
// que extrae, no desde el de la interfaz.
using Progreso = std::function<void(const std::string& fichero, int hechos, int total)>;

// Extrae toda la imagen en `destino`. Devuelve vacio si todo fue bien o el
// mensaje de error. Se para en cuanto `cancelar` pase a true.
std::string Extraer(const std::filesystem::path& imagen, const std::filesystem::path& destino,
                    const Progreso& progreso, const std::atomic<bool>& cancelar);

// La copia de `destino` es de esta misma imagen (misma ruta y tamano), segun
// el marcador que deja EscribirMarcador.
bool CopiaValida(const std::filesystem::path& destino, const std::filesystem::path& imagen);
void EscribirMarcador(const std::filesystem::path& destino, const std::filesystem::path& imagen);

// Es una carpeta con el juego ya extraido (tiene default.xex).
bool EsCarpetaDelJuego(const std::filesystem::path& carpeta);

}  // namespace nfsmw::iso
