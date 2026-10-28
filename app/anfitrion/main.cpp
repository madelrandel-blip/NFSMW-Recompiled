// nfsmw.exe: the host. It picks the game module of the edition to play, nfsmw_<edition>.dll next to it, loads it
// and runs it (NfsmwEjecutar, src/nfsmw_modulo_win.cpp). One executable for every edition: the Spanish module
// also runs the French, German and Italian ones (src/edicion_familias.h), and the USA and European PAL editions,
// which are other builds of the game, have their own module.
//
// The module, in this order: --nfsmw_modulo=<edition> (the launcher relaunches with it when the chosen copy needs
// another module); the edition of the last copy played (nfsmw_edicion_copia in nfsmw.toml); the Spanish one; any
// module there is.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <string>

#include "../src/edicion_familias.h"

namespace {

std::wstring Carpeta() {
  std::wstring ruta(MAX_PATH, L'\0');
  DWORD n = 0;
  while ((n = GetModuleFileNameW(nullptr, ruta.data(), DWORD(ruta.size()))) == ruta.size()) ruta.resize(ruta.size() * 2);
  ruta.resize(n);
  return ruta.substr(0, ruta.find_last_of(L"\\/"));
}

// The value of --nfsmw_modulo=X (or --nfsmw_modulo X), or empty.
std::string ModuloPedido() {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::string valor;
  const std::wstring clave = L"--nfsmw_modulo";
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring a = argv[i];
    std::wstring v;
    if (a.rfind(clave + L"=", 0) == 0) v = a.substr(clave.size() + 1);
    else if (a == clave && i + 1 < argc) v = argv[i + 1];
    if (!v.empty()) valor.assign(v.begin(), v.end());  // edition names are ASCII
  }
  LocalFree(argv);
  return valor;
}

// nfsmw_edicion_copia from nfsmw.toml, or empty.
std::string UltimaCopia(const std::wstring& carpeta) {
  std::ifstream toml(carpeta + L"\\nfsmw.toml");
  std::string linea;
  while (std::getline(toml, linea)) {
    const size_t i = linea.find("nfsmw_edicion_copia");
    if (i == std::string::npos || linea.find_first_not_of(" \t") != i) continue;
    const size_t a = linea.find('"'), b = linea.rfind('"');
    if (a != std::string::npos && b > a) return linea.substr(a + 1, b - a - 1);
  }
  return {};
}

HMODULE Cargar(const std::wstring& carpeta, std::string_view edicion) {
  if (edicion.empty()) return nullptr;
  const std::string familia(nfsmw::edicion::FamiliaDe(edicion));
  const std::wstring dll = carpeta + L"\\nfsmw_" + std::wstring(familia.begin(), familia.end()) + L".dll";
  return LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const std::wstring carpeta = Carpeta();
  HMODULE modulo = Cargar(carpeta, ModuloPedido());
  if (!modulo) modulo = Cargar(carpeta, UltimaCopia(carpeta));
  if (!modulo) modulo = Cargar(carpeta, "es");
  if (!modulo) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((carpeta + L"\\nfsmw_*.dll").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
      modulo = LoadLibraryExW((carpeta + L"\\" + f.cFileName).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      FindClose(h);
    }
  }
  const auto ejecutar = modulo ? reinterpret_cast<int (*)()>(GetProcAddress(modulo, "NfsmwEjecutar")) : nullptr;
  if (!ejecutar) {
    MessageBoxW(nullptr, L"No game module (nfsmw_<edition>.dll) was found next to nfsmw.exe. Extract the whole "
                         L"release folder and try again.",
                L"NFSMW Recomp", MB_OK | MB_ICONERROR);
    return 1;
  }
  return ejecutar();
}
