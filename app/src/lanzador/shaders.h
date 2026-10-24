// nfsmw - genera la biblioteca de shaders (nfsmw_shaders.nfsp) a partir del
// juego, desde el lanzador.
//
// Hace lo mismo y en el mismo orden que el instalador web de nfsmw-nx
// (lib/installer.js y lib/shaders.js), para que el resultado sea identico:
//   1. sacar la imagen PE del default.xex (descifrado AES y descompresion LZX);
//   2. buscar los contenedores de shaders en NFS/*.BIN y en esa imagen;
//   3. traducirlos a HLSL con XenosRecomp;
//   4. las tres reescrituras de fuente (sombras, sombra por minimo, desenfoque);
//   5. compilarlos a SPIR-V con DXC (dxcompiler.dll junto al ejecutable);
//   6. empaquetarlos con el mismo codigo que los carga el renderizador.

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace nfsmw::shaders {

enum class Fase { kImagen, kBuscar, kTraducir, kCompilar, kEmpaquetar };

// Se llama desde el hilo que genera, no desde el de la interfaz.
using Progreso = std::function<void(Fase fase, int hechos, int total)>;

// Nombre de la biblioteca, que el renderizador busca junto al ejecutable.
std::filesystem::path RutaBiblioteca();

// Hay DXC para compilar (dxcompiler.dll junto al ejecutable).
bool PuedeGenerar();

// SHA-256 of a whole file, in lowercase hex. Empty if it cannot be read.
std::string Sha256Fichero(const std::filesystem::path& ruta);

// Genera la biblioteca de la carpeta del juego `juego` en `salida`. Devuelve
// vacio si todo fue bien o el mensaje de error.
std::string Generar(const std::filesystem::path& juego, const std::filesystem::path& salida,
                    const Progreso& progreso, const std::atomic<bool>& cancelar);

}  // namespace nfsmw::shaders
