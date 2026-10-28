// nfsmw - entry point of the game module on Windows.
//
// The game (its recompiled code, the renderer, the launcher...) is a DLL per edition, nfsmw_<edition>.dll, which
// nfsmw.exe loads (anfitrion/main.cpp). The SDK's wWinMain (windowed_app_main_sdl.cpp) is still the entry: it reads
// the command line itself, and every path the game uses hangs from the executable's folder, the host's.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line, int show_cmd);

extern "C" __declspec(dllexport) int NfsmwEjecutar() {
  return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWNORMAL);
}
