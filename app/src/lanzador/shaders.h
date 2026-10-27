// nfsmw - generates the shader library (nfsmw_shaders.nfsp) from the game, in the launcher.
//
//   1. the PE image of default.xex (AES decryption and LZX decompression);
//   2. the shader containers in NFS/*.BIN and in that image, in the same order as the nfsmw-nx web installer;
//   3. each container translated straight from Xenos microcode to SPIR-V (shaders/XenosRecomp/spirv_recompiler.h),
//      with the shadow map and composite blur changes built in. No HLSL and no DXC;
//   4. packed with the same code the renderer loads it with.

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

// The library can be generated (always, now that the translator is built in; kept for the callers).
bool PuedeGenerar();

// The library exists and was made by the current generator. One made by an older generator (the HLSL + DXC path,
// without the Xenos multiply rule) counts as missing, so it is generated again.
bool BibliotecaActual(const std::filesystem::path& biblioteca);

// SHA-256 of a whole file, in lowercase hex. Empty if it cannot be read.
std::string Sha256Fichero(const std::filesystem::path& ruta);

// Genera la biblioteca de la carpeta del juego `juego` en `salida`. Devuelve
// vacio si todo fue bien o el mensaje de error.
std::string Generar(const std::filesystem::path& juego, const std::filesystem::path& salida,
                    const Progreso& progreso, const std::atomic<bool>& cancelar);

}  // namespace nfsmw::shaders
