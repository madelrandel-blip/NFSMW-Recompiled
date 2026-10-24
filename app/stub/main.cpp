// "NFSMW Recomp.exe": the only file at the top of the release folder. It starts data\nfsmw.exe (the game
// with its launcher), which keeps every other file, the settings and the saves inside data\. Arguments are
// passed through unchanged.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::wstring self(MAX_PATH, L'\0');
  DWORD n = 0;
  while ((n = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()))) == self.size()) self.resize(self.size() * 2);
  self.resize(n);
  const std::wstring folder = self.substr(0, self.find_last_of(L"\\/"));
  const std::wstring data = folder + L"\\data";
  const std::wstring game = data + L"\\nfsmw.exe";

  // The command line minus our own program name.
  std::wstring args;
  if (const wchar_t* line = GetCommandLineW()) {
    const wchar_t* p = line;
    if (*p == L'"') {
      ++p;
      while (*p && *p != L'"') ++p;
      if (*p) ++p;
    } else {
      while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    args = p;
  }
  std::wstring command = L"\"" + game + L"\"" + args;

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, data.c_str(), &si, &pi)) {
    MessageBoxW(nullptr, L"data\\nfsmw.exe was not found. Extract the whole release folder and try again.",
                L"NFSMW Recomp", MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}
