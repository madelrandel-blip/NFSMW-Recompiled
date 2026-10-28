// nfsmw - gestor de partidas guardadas (ver partidas.h).

#include "partidas.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace nfsmw::partidas {
namespace {

namespace fs = std::filesystem;

constexpr const char* kTitulo = "454107D9";
constexpr const char* kTipo = "00000001";
constexpr const char* kXuidPorDefecto = "B13EBABEBABEBABE";

fs::path Datos() {
  const std::string cvar = rex::cvar::GetFlagByName("user_data_root");
  if (!cvar.empty()) return fs::path(std::u8string(cvar.begin(), cvar.end()));
  return rex::filesystem::GetUserFolder() / "nfsmw";
}

fs::path Copias() { return rex::filesystem::GetExecutableFolder() / "saves"; }

std::string Fecha(const fs::path& p) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  if (ec) return {};
#if defined(__ANDROID__)
  // The NDK's libc++ has no clock_cast; its file_clock converts by itself.
  const auto s = std::chrono::time_point_cast<std::chrono::system_clock::duration>(std::chrono::file_clock::to_sys(t));
#else
  const auto s = std::chrono::clock_cast<std::chrono::system_clock>(t);
#endif
  const std::time_t c = std::chrono::system_clock::to_time_t(s);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &c);
#else
  localtime_r(&c, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

// Copia una carpeta entera, reemplazando lo que haya.
std::string Copiar(const fs::path& origen, const fs::path& destino) {
  std::error_code ec;
  fs::create_directories(destino, ec);
  fs::copy(origen, destino, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  return ec ? ec.message() : std::string();
}

}  // namespace

std::vector<Partida> Listar() {
  std::vector<Partida> lista;
  std::error_code ec;
  for (const auto& usuario : fs::directory_iterator(Datos(), ec)) {
    const fs::path tipo = usuario.path() / kTitulo / kTipo;
    if (!fs::is_directory(tipo, ec)) continue;
    for (const auto& e : fs::directory_iterator(tipo, ec)) {
      if (!e.is_directory()) continue;
      Partida p;
      p.perfil = rex::path_to_utf8(e.path().filename());
      p.carpeta = e.path();
      p.cabecera = usuario.path() / kTitulo / "Headers" / kTipo / (e.path().filename().string() + ".header");
      for (const auto& f : fs::recursive_directory_iterator(e.path(), ec))
        if (f.is_regular_file()) p.bytes += f.file_size();
      p.fecha = Fecha(e.path());
      p.hay_anterior = fs::is_directory(Copias() / e.path().filename() / "anterior", ec);
      lista.push_back(std::move(p));
    }
  }
  std::sort(lista.begin(), lista.end(), [](const Partida& a, const Partida& b) { return a.fecha > b.fecha; });
  return lista;
}

std::string Exportar(const Partida& p, const fs::path& destino) {
  const fs::path nombre = p.carpeta.filename();
  if (auto error = Copiar(p.carpeta, destino / nombre); !error.empty()) return error;
  std::error_code ec;
  if (fs::is_regular_file(p.cabecera, ec))
    fs::copy_file(p.cabecera, destino / p.cabecera.filename(), fs::copy_options::overwrite_existing, ec);
  REXLOG_INFO("[partidas] exportada «{}» a {}", p.perfil, rex::path_to_utf8(destino));
  return ec ? ec.message() : std::string();
}

std::string Importar(const fs::path& origen) {
  std::error_code ec;
  const fs::path nombre = origen.filename();
  const fs::path cabecera = origen.parent_path() / (nombre.string() + ".header");
  if (!fs::is_directory(origen, ec) || nombre.empty()) return "carpeta no valida";
  if (!fs::is_regular_file(cabecera, ec)) return "falta " + rex::path_to_utf8(cabecera.filename());
  // Al usuario que ya tenga partidas; si no hay ninguno, al perfil por defecto del runtime.
  fs::path usuario = Datos() / kXuidPorDefecto;
  for (const auto& p : Listar()) {
    usuario = p.carpeta.parent_path().parent_path().parent_path();
    break;
  }
  const fs::path base = usuario / kTitulo;
  if (auto error = Copiar(origen, base / kTipo / nombre); !error.empty()) return error;
  fs::create_directories(base / "Headers" / kTipo, ec);
  fs::copy_file(cabecera, base / "Headers" / kTipo / cabecera.filename(), fs::copy_options::overwrite_existing, ec);
  REXLOG_INFO("[partidas] importada «{}» desde {}", rex::path_to_utf8(nombre), rex::path_to_utf8(origen));
  return ec ? ec.message() : std::string();
}

std::string RestaurarAnterior(const Partida& p) {
  const fs::path anterior = Copias() / p.carpeta.filename() / "anterior";
  std::error_code ec;
  if (!fs::is_directory(anterior, ec)) return "no hay copia anterior";
  // <perfil>/anterior tiene la misma forma que la carpeta del guardado (CopiaParaElUsuario).
  fs::remove_all(p.carpeta, ec);
  if (auto error = Copiar(anterior, p.carpeta); !error.empty()) return error;
  REXLOG_INFO("[partidas] restaurada la copia anterior de «{}»", p.perfil);
  return {};
}

std::string Borrar(const Partida& p) {
  std::error_code ec;
  fs::remove_all(p.carpeta, ec);
  if (ec) return ec.message();
  fs::remove(p.cabecera, ec);
  REXLOG_INFO("[partidas] borrada «{}»", p.perfil);
  return {};
}

}  // namespace nfsmw::partidas
