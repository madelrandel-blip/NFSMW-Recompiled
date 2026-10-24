// nfsmw - gestor de partidas guardadas.
//
// Las partidas viven donde las pone el runtime:
//   <datos>/<xuid>/454107D9/00000001/<perfil>/          los ficheros del guardado
//   <datos>/<xuid>/454107D9/Headers/00000001/<perfil>.header
// con <datos> = user_data_root o, por defecto, Documentos/nfsmw. Ademas, el
// runtime deja una copia legible en saves/<perfil>/{actual,anterior} junto al
// ejecutable (content_backup_root, ver nfsmw_app.h).
//
// Una partida exportada es una carpeta con <perfil>/ y <perfil>.header, la
// misma forma que dentro del runtime, para poder llevarla a otro PC.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nfsmw::partidas {

struct Partida {
  std::string perfil;
  std::filesystem::path carpeta;  // ficheros del guardado
  std::filesystem::path cabecera; // .header (puede faltar)
  uint64_t bytes = 0;
  std::string fecha;              // ultima modificacion, "AAAA-MM-DD HH:MM"
  bool hay_anterior = false;      // existe saves/<perfil>/anterior para restaurar
};

std::vector<Partida> Listar();

// Todas devuelven el error, o vacio si fue bien.
std::string Exportar(const Partida& p, const std::filesystem::path& destino);
std::string Importar(const std::filesystem::path& origen);  // la carpeta <perfil>/ exportada
std::string RestaurarAnterior(const Partida& p);
std::string Borrar(const Partida& p);

}  // namespace nfsmw::partidas
