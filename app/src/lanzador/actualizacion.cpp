// nfsmw - aviso de version nueva (ver actualizacion.h).

#include "actualizacion.h"

#include "sistema.h"

#include <rex/logging.h>

#include <array>
#include <cstdio>
#include <regex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

#ifndef NFSMW_VERSION
#define NFSMW_VERSION "0.0.0"
#endif

namespace nfsmw::actualizacion {
namespace {

constexpr const char* kRepositorio = "madelrandel-blip/NFSMW-Recompiled";

std::string Descargar(const std::string& ruta) {
  std::string cuerpo;
#if defined(_WIN32)
  HINTERNET sesion = WinHttpOpen(L"nfsmw-lanzador", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
  if (!sesion) return cuerpo;
  WinHttpSetTimeouts(sesion, 4000, 4000, 4000, 4000);
  HINTERNET conexion = WinHttpConnect(sesion, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  const std::wstring wruta(ruta.begin(), ruta.end());
  HINTERNET peticion = conexion ? WinHttpOpenRequest(conexion, L"GET", wruta.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
  if (peticion &&
      WinHttpSendRequest(peticion, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0,
                         0, 0) &&
      WinHttpReceiveResponse(peticion, nullptr)) {
    DWORD disponible = 0;
    while (WinHttpQueryDataAvailable(peticion, &disponible) && disponible) {
      std::string trozo(disponible, '\0');
      DWORD leido = 0;
      if (!WinHttpReadData(peticion, trozo.data(), disponible, &leido)) break;
      cuerpo.append(trozo.data(), leido);
      if (cuerpo.size() > (1 << 20)) break;  // una release no pesa tanto
    }
  }
  if (peticion) WinHttpCloseHandle(peticion);
  if (conexion) WinHttpCloseHandle(conexion);
  WinHttpCloseHandle(sesion);
#elif defined(__ANDROID__)
  // There is no curl for an app: Java's HttpURLConnection through the activity.
  cuerpo = sistema::DescargarTexto("https://api.github.com" + ruta);
#else
  const std::string orden = "curl -fsSL --max-time 4 -H 'Accept: application/vnd.github+json' "
                            "'https://api.github.com" + ruta + "' 2>/dev/null";
  if (FILE* p = popen(orden.c_str(), "r")) {
    std::array<char, 4096> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) cuerpo.append(buf.data(), n);
    pclose(p);
  }
#endif
  return cuerpo;
}

// "v0.2.1", "NFSMW-Recompiled-0.2.1"... -> {0, 2, 1}. Sin numeros, {}.
std::array<int, 3> Numeros(const std::string& texto) {
  std::smatch m;
  static const std::regex patron(R"((\d+)\.(\d+)(?:\.(\d+))?)");
  if (!std::regex_search(texto, m, patron)) return {-1, -1, -1};
  return {std::stoi(m[1]), std::stoi(m[2]), m[3].matched ? std::stoi(m[3]) : 0};
}

std::string Campo(const std::string& json, const std::string& clave) {
  std::smatch m;
  const std::regex patron("\"" + clave + "\"\\s*:\\s*\"([^\"]*)\"");
  return std::regex_search(json, m, patron) ? m[1].str() : std::string();
}

}  // namespace

const char* VersionActual() { return NFSMW_VERSION; }

Resultado Comprobar() {
  Resultado r;
  const std::string json = Descargar(std::string("/repos/") + kRepositorio + "/releases/latest");
  const std::string etiqueta = Campo(json, "tag_name");
  const auto remota = Numeros(etiqueta), local = Numeros(VersionActual());
  if (remota[0] < 0) return r;
  if (remota > local) {
    r.hay_nueva = true;
    r.version = std::to_string(remota[0]) + "." + std::to_string(remota[1]) + "." + std::to_string(remota[2]);
    r.url = Campo(json, "html_url");
    if (r.url.empty()) r.url = std::string("https://github.com/") + kRepositorio + "/releases/latest";
    REXLOG_INFO("[lanzador] version nueva disponible: {} (esta es {})", r.version, VersionActual());
  }
  return r;
}

}  // namespace nfsmw::actualizacion
