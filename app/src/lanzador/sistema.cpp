// nfsmw - servicios del sistema operativo para el lanzador (ver sistema.h).

#include "sistema.h"

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <cctype>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#elif defined(__ANDROID__)
// Android also defines __linux__: it has to go first, because zenity, xdg-open, xrandr and fork() do not apply.
#include <SDL3/SDL.h>
#include <jni.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace nfsmw::sistema {
namespace {

// Las rutas viajan en UTF-8 por los cvars; en Windows hacen falta en UTF-16.
std::filesystem::path DesdeUtf8(const std::string& s) { return std::u8string(s.begin(), s.end()); }

#if defined(_WIN32)
// IFileOpenDialog: el dialogo moderno de Windows, para ficheros y carpetas.
std::filesystem::path Dialogo(const std::filesystem::path& inicial, bool carpeta) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::filesystem::path elegido;
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dlg)))) {
    DWORD opciones = 0;
    dlg->GetOptions(&opciones);
    dlg->SetOptions(opciones | FOS_FORCEFILESYSTEM | (carpeta ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!carpeta) {
      const COMDLG_FILTERSPEC filtros[] = {{L"Xbox 360 ISO (*.iso)", L"*.iso"}, {L"*.*", L"*.*"}};
      dlg->SetFileTypes(2, filtros);
    }
    std::error_code ec;
    if (!inicial.empty() && std::filesystem::is_directory(inicial, ec)) {
      IShellItem* carpeta_inicial = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(inicial.wstring().c_str(), nullptr,
                                                IID_PPV_ARGS(&carpeta_inicial)))) {
        dlg->SetFolder(carpeta_inicial);
        carpeta_inicial->Release();
      }
    }
    if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR ruta = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &ruta))) {
          elegido = ruta;
          CoTaskMemFree(ruta);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return elegido;
}
#elif defined(__ANDROID__)
// Android's file picker, through NfsmwActivity.pickGamePath (app/android): it asks for storage access if it is
// missing and turns the chosen document into a filesystem path. Blocks this thread until the user is done.
std::filesystem::path Dialogo(const std::filesystem::path&, bool carpeta) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return {};
  std::filesystem::path elegido;
  jclass clase = env->GetObjectClass(activity);
  jmethodID metodo = env->GetMethodID(clase, "pickGamePath", "(Z)Ljava/lang/String;");
  if (metodo) {
    auto ruta = static_cast<jstring>(env->CallObjectMethod(activity, metodo, jboolean(carpeta)));
    if (ruta) {
      if (const char* utf8 = env->GetStringUTFChars(ruta, nullptr)) {
        elegido = DesdeUtf8(utf8);
        env->ReleaseStringUTFChars(ruta, utf8);
      }
      env->DeleteLocalRef(ruta);
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  env->DeleteLocalRef(clase);
  env->DeleteLocalRef(activity);
  return elegido;
}
#elif defined(__linux__)
// zenity viene con casi todos los escritorios GTK. Sin el, no hay dialogo y
// el lanzador pide escribir la ruta.
std::filesystem::path Dialogo(const std::filesystem::path& inicial, bool carpeta) {
  std::string orden = "zenity --file-selection";
  orden += carpeta ? " --directory" : " --file-filter='Xbox 360 ISO | *.iso *.ISO'";
  if (!inicial.empty()) orden += " --filename='" + inicial.string() + "/'";
  orden += " 2>/dev/null";
  FILE* p = popen(orden.c_str(), "r");
  if (!p) return {};
  std::string salida;
  char buf[512];
  while (fgets(buf, sizeof(buf), p)) salida += buf;
  pclose(p);
  while (!salida.empty() && (salida.back() == '\n' || salida.back() == '\r')) salida.pop_back();
  return salida;
}
#else
std::filesystem::path Dialogo(const std::filesystem::path&, bool) { return {}; }
#endif

}  // namespace

std::filesystem::path ElegirIso(const std::filesystem::path& inicial) { return Dialogo(inicial, false); }
std::filesystem::path ElegirCarpeta(const std::filesystem::path& inicial) { return Dialogo(inicial, true); }

void Abrir(const std::string& destino) {
#if defined(_WIN32)
  const std::wstring w = DesdeUtf8(destino).wstring();
  ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__ANDROID__)
  SDL_OpenURL(destino.c_str());
#elif defined(__linux__)
  if (fork() == 0) {
    execlp("xdg-open", "xdg-open", destino.c_str(), static_cast<char*>(nullptr));
    _exit(1);
  }
#else
  (void)destino;
#endif
}

void ExportarLog() {
#if defined(__ANDROID__)
  // NfsmwActivity.exportLogs (app/android) writes the report and opens the share sheet; it returns at once.
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return;
  jclass clase = env->GetObjectClass(activity);
  if (jmethodID metodo = env->GetMethodID(clase, "exportLogs", "()V")) {
    env->CallVoidMethod(activity, metodo);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  env->DeleteLocalRef(clase);
  env->DeleteLocalRef(activity);
#else
  Abrir(rex::path_to_utf8(rex::filesystem::GetExecutableFolder() / "logs"));
#endif
}

bool Lanzar(const std::filesystem::path& exe, const std::string& argumentos) {
  if (exe.empty()) return false;
#if defined(_WIN32)
  const std::wstring args = DesdeUtf8(argumentos).wstring();
  const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), args.c_str(),
                                                         exe.parent_path().wstring().c_str(), SW_SHOWNORMAL));
  if (r <= 32) REXLOG_ERROR("[lanzador] no se pudo abrir {} (ShellExecuteW = {})", rex::path_to_utf8(exe), int(r));
  return r > 32;
#elif defined(__ANDROID__)
  // One edition per APK, and an app cannot start a second copy of itself: the user reopens it.
  (void)argumentos;
  return false;
#elif defined(__linux__)
  const pid_t pid = fork();
  if (pid == 0) {
    std::vector<std::string> partes{exe.string()};
    size_t i = 0;
    while (i < argumentos.size()) {
      const size_t j = argumentos.find(' ', i);
      partes.push_back(argumentos.substr(i, j == std::string::npos ? std::string::npos : j - i));
      if (j == std::string::npos) break;
      i = j + 1;
    }
    std::vector<char*> argv;
    for (auto& p : partes) argv.push_back(p.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(1);
  }
  return pid > 0;
#else
  (void)argumentos;
  return false;
#endif
}


Resolucion ResolucionPantalla() {
#if defined(_WIN32)
  // EnumDisplaySettings da el modo real, sin el escalado de DPI que aplica GetSystemMetrics.
  DEVMODEW modo{};
  modo.dmSize = sizeof(modo);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &modo)) return {int(modo.dmPelsWidth), int(modo.dmPelsHeight)};
#elif defined(__ANDROID__)
  if (const SDL_DisplayMode* modo = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay())) {
    return {int(modo->w * modo->pixel_density), int(modo->h * modo->pixel_density)};
  }
#elif defined(__linux__)
  if (FILE* p = popen("xrandr --current 2>/dev/null | grep -o 'current [0-9]* x [0-9]*'", "r")) {
    Resolucion r;
    std::fscanf(p, "current %d x %d", &r.ancho, &r.alto);
    pclose(p);
    return r;
  }
#endif
  return {};
}

bool Relanzar(const std::string& argumentos) { return Lanzar(rex::filesystem::GetExecutablePath(), argumentos); }
}  // namespace nfsmw::sistema
