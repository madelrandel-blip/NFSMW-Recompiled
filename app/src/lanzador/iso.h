// nfsmw - imagenes .iso de Xbox 360 (XDVDFS): leer sus ficheros y extraerlas a una carpeta.
//
// En PC y Android el juego arranca de la ISO tal cual: el runtime la monta (DiscImageDevice) y el lanzador lee de
// ella lo que necesita (el default.xex para saber la edicion, los datos para generar los shaders). En la Switch el
// SDK no puede mapear una imagen entera, y alli se extrae una vez a una carpeta.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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

// Un fichero de la imagen: ruta con '/' (p. ej. "NFS/ZZDATA0.BIN"), donde empieza en el .iso y su tamano.
struct Fichero {
  std::string ruta;
  int64_t offset = 0;
  uint64_t tam = 0;
};
// Los ficheros (no las carpetas) de la imagen, en el orden de la extraccion; vacio si no es una imagen.
std::vector<Fichero> Listar(const std::filesystem::path& imagen);
// Un fichero entero de la imagen, por su ruta sin distinguir mayusculas; vacio si no esta.
std::vector<uint8_t> LeerFichero(const std::filesystem::path& imagen, std::string_view ruta);
// Es una imagen con el juego (tiene default.xex en la raiz).
bool EsImagenDelJuego(const std::filesystem::path& imagen);

// Es una carpeta con el juego ya extraido (tiene default.xex).
bool EsCarpetaDelJuego(const std::filesystem::path& carpeta);

}  // namespace nfsmw::iso
