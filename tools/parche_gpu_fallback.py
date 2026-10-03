#!/usr/bin/env python3
"""
Keeps the game from closing silently when the GPU is not good enough.

    python tools/parche_gpu_fallback.py            apply
    python tools/parche_gpu_fallback.py --estado
    python tools/parche_gpu_fallback.py --revertir

It touches a single SDK file:  src/ui/d3d12/d3d12_provider.cpp
It keeps a .original on the first run and is idempotent.


WHAT WAS HAPPENING
==================

The adapter choice was already by capabilities, not by model list: it walks
the adapters with EnumAdapters1 and takes the first one that can create a D3D12
device at feature level 11_0. That part is fine and is not touched.

What was wrong were the two emergency exits:

1. THE FALLBACK THAT ALREADY EXISTED WAS NOT USED.
   The d3d12_adapter cvar accepts -2, which means "use WARP" -Microsoft's
   software rasterizer-. But with the default value (-1) the loop explicitly
   discards adapters marked as software:

       if (!(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;

   So if there was no valid physical GPU, WARP was not tried: it failed
   directly. The fallback was there, but you had to know it existed and write
   it by hand on the command line.

2. NOBODY SAW THE ERROR.
   A REXLOG_ERROR and return false. The window never opens, so from the
   outside the game "does nothing": double click and not even a flicker. For
   whoever receives the folder and does not know there is a log, that is
   indistinguishable from a broken executable.


WHAT THE PATCH DOES
===================

  - Turns the loop into a function that is told whether it accepts software
    adapters. Same logic, same order, same criteria.

  - First pass: physical GPU only, exactly as before.

  - If there is none AND the user did not ask for a specific adapter, SECOND
    pass accepting software. If WARP is available, the game starts. It will be
    extremely slow -it is a CPU rasterizer-, and it warns about it in the log,
    but it starts and is visible, which is infinitely better than closing.

  - If WARP is not there either, a Windows dialog box is shown explaining what
    is needed and what to do. The log still has the same message as always, so
    as not to break anything that reads it.

The dialog box is called via LoadLibrary/GetProcAddress instead of linking
user32.lib. It is one more line of code and in exchange the patch does not
touch the SDK's linking configuration, which is exactly the kind of change
that later breaks someone else's build.

It does NOT change the selection criteria when there IS a GPU. A machine that
works today behaves exactly the same: the second pass only runs if the first
one ran out of candidates.
"""

import argparse
import pathlib
import shutil
import sys

MARCA = "PARCHE LOCAL - fallback de GPU y error visible"

ANCLA = """  // Choose the adapter.
  uint32_t adapter_index = 0;
  IDXGIAdapter1* adapter = nullptr;
  while (dxgi_factory->EnumAdapters1(adapter_index, &adapter) == S_OK) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
      if (SUCCEEDED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device),
                                             nullptr))) {
        if (REXCVAR_GET(d3d12_adapter) >= 0) {
          if (adapter_index == REXCVAR_GET(d3d12_adapter)) {
            break;
          }
        } else if (REXCVAR_GET(d3d12_adapter) == -2) {
          if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            break;
          }
        } else {
          if (!(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            break;
          }
        }
      }
    }
    adapter->Release();
    adapter = nullptr;
    ++adapter_index;
  }
  if (adapter == nullptr) {
    REXLOG_ERROR(
        "Failed to get an adapter supporting Direct3D 12 with the feature "
        "level of at least 11_0");
    dxgi_factory->Release();
    return false;
  }
"""

NUEVO = """  // ============ PARCHE LOCAL - fallback de GPU y error visible ============
  //
  // La busqueda es la de siempre -recorrer adaptadores y quedarse con el
  // primero que cree un dispositivo D3D12 a feature level 11_0-, solo que
  // ahora esta en una funcion para poder repetirla.
  //
  // Se le pasa si acepta adaptadores marcados como software. Con el valor por
  // defecto del cvar (-1) la primera pasada los rechaza, igual que antes; si
  // esa pasada se queda sin candidatos se repite aceptandolos, y ahi es donde
  // aparece WARP. Antes ese segundo intento no existia: el fallback estaba
  // implementado pero solo se alcanzaba escribiendo d3d12_adapter=-2 a mano.
  auto buscar_adaptador = [&](bool aceptar_software) -> IDXGIAdapter1* {
    uint32_t indice = 0;
    IDXGIAdapter1* candidato = nullptr;
    while (dxgi_factory->EnumAdapters1(indice, &candidato) == S_OK) {
      DXGI_ADAPTER_DESC1 desc;
      if (SUCCEEDED(candidato->GetDesc1(&desc))) {
        if (SUCCEEDED(pfn_d3d12_create_device_(candidato, D3D_FEATURE_LEVEL_11_0,
                                               _uuidof(ID3D12Device), nullptr))) {
          const bool es_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
          if (REXCVAR_GET(d3d12_adapter) >= 0) {
            // El usuario pidio un indice concreto: se respeta tal cual.
            if (indice == uint32_t(REXCVAR_GET(d3d12_adapter))) {
              return candidato;
            }
          } else if (REXCVAR_GET(d3d12_adapter) == -2) {
            // El usuario pidio WARP explicitamente.
            if (es_software) {
              return candidato;
            }
          } else if (aceptar_software || !es_software) {
            return candidato;
          }
        }
      }
      candidato->Release();
      candidato = nullptr;
      ++indice;
    }
    return nullptr;
  };

  IDXGIAdapter1* adapter = buscar_adaptador(false);

  bool usando_software = false;
  if (adapter == nullptr && REXCVAR_GET(d3d12_adapter) == -1) {
    // Ninguna GPU fisica sirve. Antes se acababa aqui.
    REXLOG_WARN(
        "No hay ninguna GPU fisica con Direct3D 12 feature level 11_0. "
        "Probando el rasterizador por software (WARP).");
    adapter = buscar_adaptador(true);
    if (adapter != nullptr) {
      usando_software = true;
      REXLOG_WARN(
          "Usando WARP: el render lo hace la CPU. Va a ir MUY lento -unos "
          "pocos fotogramas por segundo- pero el juego arranca y se ve. "
          "Actualiza el driver de la GPU para volver a la aceleracion por "
          "hardware.");
    }
  }

  if (adapter == nullptr) {
    // Este mensaje se deja PALABRA POR PALABRA como estaba: hay scripts y
    // logs viejos que lo buscan por texto.
    REXLOG_ERROR(
        "Failed to get an adapter supporting Direct3D 12 with the feature "
        "level of at least 11_0");

    // Y ademas se dice en pantalla, porque a estas alturas la ventana todavia
    // no existe: sin esto el ejecutable simplemente no hace nada al abrirlo, y
    // quien lo recibe no tiene forma de distinguirlo de un archivo roto.
    //
    // Se carga user32 a mano en vez de enlazarla para no tocar la
    // configuracion de enlazado del SDK.
    if (HMODULE user32 = LoadLibraryA("user32.dll")) {
      using PFN_MessageBoxW = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
      auto message_box =
          reinterpret_cast<PFN_MessageBoxW>(GetProcAddress(user32, "MessageBoxW"));
      if (message_box) {
        message_box(nullptr,
                    L"No se ha encontrado ninguna tarjeta grafica compatible.\\n"
                    L"\\n"
                    L"Hace falta Direct3D 12 con feature level 11_0. Eso lo\\n"
                    L"cumple practicamente cualquier GPU de 2012 en adelante,\\n"
                    L"asi que lo mas probable es que el problema sea el driver.\\n"
                    L"\\n"
                    L"Que probar, por orden:\\n"
                    L"  1. Actualizar el driver de la tarjeta grafica.\\n"
                    L"  2. Comprobar que Windows esta al dia.\\n"
                    L"  3. Si es un portatil con dos graficas, forzar que el\\n"
                    L"     juego use la dedicada.\\n"
                    L"\\n"
                    L"Hay mas detalle en la carpeta logs, junto al ejecutable.",
                    L"Need for Speed: Most Wanted", 0x00000010 /* MB_ICONERROR */);
      }
      FreeLibrary(user32);
    }

    dxgi_factory->Release();
    return false;
  }

  if (usando_software) {
    REXGPU_INFO("Adaptador elegido: WARP (rasterizador por software)");
  }
  // ====================== fin del parche local ============================
"""


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        f = cand / "src" / "ui" / "d3d12" / "d3d12_provider.cpp"
        if f.exists():
            return f
    sys.exit("[ERROR] Cannot find the SDK's src/ui/d3d12/d3d12_provider.cpp.\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    f = localizar_sdk()
    original = f.with_suffix(".cpp.original")
    txt = f.read_text(encoding="utf-8")
    puesto = MARCA in txt

    if args.estado:
        print(f"  {f}")
        print("  Patch:", "APPLIED" if puesto else "not applied")
        return 0

    if args.revertir:
        if original.exists():
            shutil.copy2(original, f)
            print("[ok] Restored from .original")
        else:
            print("[aviso] There is no .original to restore.")
        return 0

    if puesto:
        print("[ok] It was already applied. I am touching nothing.")
        return 0

    n = txt.count(ANCLA)
    if n != 1:
        sys.exit(f"[ERROR] The anchor 'adapter choice' appears {n} times,\n"
                 f"        expected 1. The SDK must have changed. I have not touched anything.")

    # NOTE: does this file share the .original with parche_presentador.py? No.
    # That one touches d3d12_presenter.cpp, this one d3d12_provider.cpp. They
    # are different.
    if not original.exists():
        shutil.copy2(f, original)
        print(f"[ok] Backup: {original.name}")

    f.write_text(txt.replace(ANCLA, NUEVO), encoding="utf-8")
    print("[ok] Patch applied.")
    print()
    print("  No valid GPU -> WARP is tried before giving up")
    print("  No WARP      -> dialog box explaining what is happening")
    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
