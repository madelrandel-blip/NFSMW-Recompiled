// nfsmw - mouse (and later touch) in the game's menus, through FEng's own mouse support
//
// FEng, EA's front-end engine, has mouse support in every build, the Xbox 360 one included; it is just never turned on
// there. In FEngine::Update (sub_822FD0D0, this in r31):
//     lbz r7,1(r31)          bMouseActive, byte +1 of the engine
//     beq ...                off: the mouse is skipped
//     lwz r3,264(r31)        pInterface
//     lwz r5,68(vtable)      GetMouseInfo, method 17 of the interface
//     bl  sub_822FB270       FEMouse::Update(&engine->Mouse (+8), Info, tDelta)
// sub_822FB270 matches FEMouse::Update in the decompilation (dbalatoni13/nfsmw, FEng/FEMouse.cpp) instruction by
// instruction: X and Y as int16 at +0 and +2 of the info, the button mask at +6, three held counters.
// With the flag on, the engine hit-tests the package objects flagged FF_MouseObject (FEngine::UpdateMouseState) and
// sends their mouse-over and click messages. Whether the console's menu packages flag their buttons is what this
// test finds out.
//
// Test switch: nfsmw_menu_raton. PC only for now: the Windows cursor over the game window, mapped to FEng's 640x480.

#include <array>
#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <cstring>
#include <memory>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

#include "nfsmw_fe_raton.h"
#include "lanzador/sistema.h"
#include "nfsmw_ajustes_graficos.h"

#if REX_PLATFORM_ANDROID
#include <SDL3/SDL.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// On by default on Android: touches drive the menus as the mouse does on PC.
REXCVAR_DEFINE_BOOL(nfsmw_menu_raton, REX_PLATFORM_ANDROID != 0, "NFSMW",
                    "Pruebas: activa el raton del motor de menus del juego (FEng) y le da la posicion del cursor de "
                    "Windows. Para saber si los menus de la consola responden a raton y, despues, a toques");

namespace {

std::atomic<uint32_t> g_actualizaciones{0};
std::atomic<uint16_t> g_botones{0};    // current button mask given to FEng
std::atomic<uint32_t> g_clic_numero{0};  // increases on every press

namespace in = rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
using Reloj = std::chrono::steady_clock;

// Until when the virtual pad holds A: a click released over the menu's current button accepts it, as A does. The
// console's menus react to the mouse messages of other objects (arrows, some options) but accept only with A.
std::atomic<int64_t> g_a_hasta_ms{0};
constexpr int64_t kPulsacionMs = 120;
// Other buttons of the virtual pad (the D-pad that walks a grid to the clicked cell) and until when.
std::atomic<uint16_t> g_otros_botones{0};
std::atomic<int64_t> g_otros_hasta_ms{0};
// Held while the mouse button stays down over a zone that stands for an analog control: 1 LT, 2 RT, 3 right stick to
// the right (the showcase's zoom and rotation).
std::atomic<int> g_mantenido{0};
// Dragging over the car in the showcase: the right stick follows the mouse's movement.
std::atomic<int16_t> g_stick_x{0}, g_stick_y{0};
constexpr uint16_t kArriba = 0x0001, kAbajo = 0x0002, kIzquierda = 0x0004, kDerecha = 0x0008, kBotonA = 0x1000, kBotonB = 0x2000, kBotonX = 0x4000, kBotonY = 0x8000;

// The menu's current button: FEPackage::pCurrentButton is at +0x8C in this build (+0x98 in the decompiled
// prototype), measured on the challenge grid: it followed every move of the selection.
constexpr uint32_t kBotonActual = 0x8C;

// Centre of every mouse object's rectangle, as the hit test last computed it (sub_82281C20), on the FEng thread.
std::unordered_map<uint32_t, std::pair<float, float>> g_centros;
// Rectangles of the challenge grid's up/down arrows: nothing under them takes the mouse.
struct Flecha {
  std::array<float, 4> r;
  int64_t visto_ms;  // the menu computed this rectangle lately: the arrow is on screen
};
std::unordered_map<uint32_t, Flecha> g_flechas;
// The button bar's slots (flags ...9) and their rectangles.
std::unordered_map<uint32_t, Flecha> g_barra;
// The arrow's drawing goes past its rectangle, and the neighbouring cell's hit area reaches into that gap.
constexpr float kMargenFlecha = 14.0f;
bool EsFlecha(uint32_t hash) { return hash == 0x444969FDu || hash == 0x444969FEu; }
// When the quick race mode carousel's arrows were last drawn: its chosen mode and its button bar have no mouse zones.
std::atomic<int64_t> g_carrusel_ms{0};
// The carousel's arrows (left 3DF39A82, right FCD5B255), as last drawn.
Flecha g_flechas_carrusel[2] = {};
// The menu screen on top (its package name), from FEngine::Update.
std::string g_pantalla;
// In the garage: from its main screen (CustomizeMain) until a screen that is not part of it (the main menu, a race,
// a loading screen). Every piece has its own screen name (Spoilers, ...), so the garage is followed, not listed.
bool g_en_taller = false;
// The package whose mouse objects FEng tested last (the menu on screen).
std::atomic<uint32_t> g_paquete_raton{0};
// A click on the paint grid waiting for the next FEng frame: the swatch nearest to it gets chosen.
std::atomic<bool> g_pintura_pendiente{false};
std::atomic<int32_t> g_pintura_x{0}, g_pintura_y{0};
// When FEng last hit-tested a mouse object: a menu is on screen (the race HUD has none).
std::atomic<int64_t> g_menu_ms{0};
// The options list (graphics, audio...): the object whose sub_822DECA8(this, option) selects a row, the last time
// its slider arrows were drawn, and a clicked row's y waiting for the start of the next FEng frame.
uint32_t g_opciones = 0;  // set when any list is known (the candidates are checked on use)
// Options lists seen lately, newest first; the one on screen is the one whose current row is the menu's button.
std::deque<uint32_t> g_listas;
uint32_t g_boton_hash = 0;
// The other kind of options list (graphics): rows linked at +44 (enabled byte at row +40), current row at +52 and
// its 1-based index at +260. Its vtable as raw guest bytes, to tell a live list from freed memory.
uint32_t g_lista_filas = 0, g_lista_filas_vtable = 0;  // the hash the menu's current button was last set to
void RecordarLista(uint32_t lista) {
  for (auto it = g_listas.begin(); it != g_listas.end(); ++it) {
    if (*it == lista) {
      g_listas.erase(it);
      break;
    }
  }
  g_listas.push_front(lista);
  if (g_listas.size() > 24) g_listas.pop_back();
  g_opciones = lista;
}
std::atomic<int64_t> g_opciones_ms{0};
std::atomic<int32_t> g_fila_pendiente{INT32_MIN};
// Global y of the list's first row (measured in the graphics list); every option keeps its offset at +20.
constexpr float kPrimeraFila = -77.0f, kAltoFila = 22.0f;


// A walk of the D-pad towards a clicked grid cell: one step at a time, reading the selection after each one.
struct Camino {
  uint32_t paquete = 0, destino = 0;
  int pasos = 0;
  int64_t siguiente_ms = 0;
  uint32_t hash_antes = 0;  // the selection's hash before the last vertical step, to learn the row width
  uint16_t ultimo_paso = 0;
};
Camino g_camino;
// D-pad steps to the clicked cell, one every kPasoMs (held for half of it).
std::deque<uint16_t> g_pasos;
int64_t g_paso_ms = 0;
constexpr int64_t kPasoMs = 80;
// The cell's place in the challenge grid, in reading order: rows 1-2 are 09C9A0F9.., row 3 is 42FDC049...
int IndiceCasilla(uint32_t h) {
  if (h - 0x09C9A0F9u < 8u) return int(h - 0x09C9A0F9u);
  if (h - 0x42FDC049u < 4u) return 8 + int(h - 0x42FDC049u);
  return -1;
}
// Cells per row of the grid being walked: learned from the first vertical step (4 in the challenge series).
int g_columnas = 4;

// A click waiting for the game to process it: arrows move the selection a frame or two after the click, so whether
// the click landed on the chosen option is decided later, in FEngine::Update.
std::atomic<uint32_t> g_pendiente_paquete{0}, g_pendiente_seleccion{0};
std::atomic<int64_t> g_pendiente_ms{0};
constexpr int64_t kEsperaMs = 150;

int64_t AhoraMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Reloj::now().time_since_epoch()).count();
}

constexpr in::DeviceId kMandoRaton = in::DeviceId(0x4E46534D57524154ull);  // "NFSMWRAT"

// A synthetic pad: the SDK routes synthetic devices to guest user 0 next to the real pad or the keyboard, so its A
// counts as the player's.
class MandoRaton final : public in::InputDriver {
 public:
  MandoRaton() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kMandoRaton;
    info.name = "Raton de los menus";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kMandoRaton) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_state) {
      const int64_t ahora = AhoraMs();
      uint16_t botones = ahora < g_a_hasta_ms.load(std::memory_order_relaxed) ? kBotonA : 0;
      if (ahora < g_otros_hasta_ms.load(std::memory_order_relaxed)) {
        botones |= g_otros_botones.load(std::memory_order_relaxed);
      }
      std::memset(out_state, 0, sizeof(*out_state));
      const int mantenido = g_mantenido.load(std::memory_order_relaxed);
      out_state->packet_number = botones | (uint32_t(mantenido) << 16);
      out_state->gamepad.buttons = botones;
      if (mantenido == 1) out_state->gamepad.left_trigger = 255;
      if (mantenido == 2) out_state->gamepad.right_trigger = 255;
      if (mantenido == 3) out_state->gamepad.thumb_rx = int16_t(32000);
      if (mantenido == 4) {
        out_state->gamepad.thumb_rx = g_stick_x.load(std::memory_order_relaxed);
        out_state->gamepad.thumb_ry = g_stick_y.load(std::memory_order_relaxed);
        out_state->packet_number = uint32_t(AhoraMs());  // a new state every read while dragging
      }
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t, in::X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kMandoRaton) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION*) override {
    return id == kMandoRaton ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t, in::X_INPUT_KEYSTROKE*) override {
    return id == kMandoRaton ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
};

// Test aid: a capture of the window on every left click, with a red cross where the cursor was, saved as
// logs/clics/clic_<n>_<x>_<y>.bmp (x, y in FEng's space), so the clicks can be checked against what the menu showed.
void CapturarClic(uint32_t numero, int16_t fx, int16_t fy) {
#ifdef _WIN32
  HWND ventana = GetForegroundWindow();
  POINT p;
  RECT r;
  if (!ventana || !GetCursorPos(&p) || !ScreenToClient(ventana, &p) || !GetClientRect(ventana, &r)) return;
  std::thread([ventana, p, r, numero, fx, fy] {
    const int w = r.right, h = r.bottom;
    if (w <= 0 || h <= 0) return;
    HDC pantalla = GetDC(ventana);
    HDC mem = CreateCompatibleDC(pantalla);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ viejo = SelectObject(mem, bmp);
    if (!PrintWindow(ventana, mem, 0x3 /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */)) {
      BitBlt(mem, 0, 0, w, h, pantalla, 0, 0, SRCCOPY);
    }
    auto* px = static_cast<uint32_t*>(bits);
    for (int d = -12; d <= 12; ++d) {
      for (int g = -1; g <= 1; ++g) {
        const int ax = p.x + d, ay = p.y + g, bx = p.x + g, by = p.y + d;
        if (ax >= 0 && ax < w && ay >= 0 && ay < h) px[ay * w + ax] = 0x00FF0000;
        if (bx >= 0 && bx < w && by >= 0 && by < h) px[by * w + bx] = 0x00FF0000;
      }
    }
    CreateDirectoryA("logs", nullptr);
    CreateDirectoryA("logs/clics", nullptr);
    char nombre[128];
    std::snprintf(nombre, sizeof(nombre), "logs/clics/clic_%03u_%d_%d.bmp", numero, int(fx), int(fy));
    if (FILE* f = std::fopen(nombre, "wb")) {
      BITMAPFILEHEADER fh = {};
      const DWORD datos = DWORD(w) * DWORD(h) * 4;
      fh.bfType = 0x4D42;
      fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
      fh.bfSize = fh.bfOffBits + datos;
      std::fwrite(&fh, sizeof(fh), 1, f);
      std::fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
      std::fwrite(bits, datos, 1, f);
      std::fclose(f);
    }
    SelectObject(mem, viejo);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(ventana, pantalla);
  }).detach();
#else
  (void)numero, (void)fx, (void)fy;
#endif
}

uint16_t Swap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }

// Cursor over the foreground window, in FEng's centred screen space, and the button mask (1 left, 2 right).
bool LeerRaton(int16_t& x, int16_t& y, uint16_t& botones) {
#ifdef _WIN32
  HWND ventana = GetForegroundWindow();
  if (!ventana) return false;
  POINT p;
  if (!GetCursorPos(&p) || !ScreenToClient(ventana, &p)) return false;
  RECT r;
  if (!GetClientRect(ventana, &r) || r.right <= 0 || r.bottom <= 0) return false;
  // FEng's screen space is centred: Y from -240 to 240 and X with the same scale (-450 to 450 for the widest
  // backgrounds), as the object rectangles of the menus show.
  y = int16_t((p.y * 480 / r.bottom) - 240);
  x = int16_t((p.x - r.right / 2) * 480 / r.bottom);
  botones = uint16_t(((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0) |
                     ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2 : 0));
  return true;
#elif REX_PLATFORM_ANDROID
  // SDL turns a touch into the mouse (SDL_HINT_TOUCH_MOUSE_EVENTS, on by default): a finger down is the left
  // button at that point.
  SDL_Window* ventana = SDL_GetMouseFocus();
  if (!ventana) ventana = SDL_GetKeyboardFocus();
  if (!ventana) return false;
  float mx = 0.0f, my = 0.0f;
  const SDL_MouseButtonFlags estado = SDL_GetMouseState(&mx, &my);
  int ancho = 0, alto = 0;
  if (!SDL_GetWindowSize(ventana, &ancho, &alto) || ancho <= 0 || alto <= 0) return false;
  y = int16_t((my * 480.0f / float(alto)) - 240.0f);
  x = int16_t((mx - float(ancho) / 2.0f) * 480.0f / float(alto));
  botones = uint16_t(((estado & SDL_BUTTON_LMASK) ? 1 : 0) | ((estado & SDL_BUTTON_RMASK) ? 2 : 0));
  return true;
#else
  (void)x;
  (void)y;
  (void)botones;
  return false;
#endif
}

}  // namespace

// FEngine::Update: the mouse flag on before the original reads it.
REX_EXTERN(__imp__sub_822FD0D0);
// A clicked grid cell waiting for the next menu frame, chosen there outside the engine's walk over its objects.
std::atomic<uint32_t> g_casilla_pendiente{0};  // the clicked button's hash
// The scroller the last grid selection went through (sub_822E0740's this): the challenge grid, the paint swatches.
uint32_t g_scroller = 0;
bool SeleccionarEnScroller(PPCContext& ctx, uint8_t* base, uint32_t hash);
REX_EXTERN(__imp__sub_822E0ED8);
// The in-game graphics options' restart notice (defined with its rows, below): when it is due and the screen is up.
std::atomic<int64_t> g_aviso_reinicio_ms{0};
void MostrarAvisoReinicio(PPCContext& ctx, uint8_t* base);
REX_EXTERN(__imp__sub_822DECA8);
REX_EXTERN(__imp__sub_822E31F8);

// Selects the options list's row at global y directly: the list's own setter (sub_822DECA8) unhighlights the old row,
// highlights the new one and sets the menu's current button, as the D-pad does.
void SeleccionarOpcion(PPCContext& ctx, uint8_t* base, float y) {
  auto leer = [&](uint32_t d) {
    uint32_t v;
    std::memcpy(&v, base + d, 4);
    return __builtin_bswap32(v);
  };
  auto valida = [](uint32_t d) { return d >= 0x70000000u && d < 0xD0000000u && !(d & 3); };
  if (valida(g_lista_filas)) {
    uint32_t vt;
    std::memcpy(&vt, base + g_lista_filas, 4);
    if (vt == g_lista_filas_vtable && vt) {
      const uint32_t l = g_lista_filas, centinela = l + 44;
      const int indice = int(std::lround((y - kPrimeraFila) / kAltoFila));  // 0-based, top row of the screen
      int i = 0;
      uint32_t fila = 0;
      for (uint32_t n = leer(centinela); valida(n) && n != centinela && i < 64; n = leer(n), ++i) {
        if (i == indice) {
          fila = n - 4;
          break;
        }
      }
      const uint32_t actual = leer(l + 52);
      REXLOG_INFO("[menu raton] opciones: lista de filas {:08X}, clic en fila {} ({:08X}, actual {:08X})", l, indice + 1,
                  fila, actual);
      if (indice >= 0 && fila && fila != actual && base[fila + 40]) {
        const PPCContext guardado = ctx;
        ctx.r3.u64 = l;
        ctx.r4.u64 = fila;
        __imp__sub_822E31F8(ctx, base);
        ctx = guardado;
        uint32_t uno = __builtin_bswap32(uint32_t(indice + 1));
        std::memcpy(base + l + 260, &uno, 4);
      }
      return;
    }
  }
  uint32_t lista = 0;
  for (const uint32_t l : g_listas) {
    if (!valida(l) || !leer(l)) continue;
    const uint32_t o = leer(l + 12);
    if (!valida(o) || !valida(leer(o + 16))) continue;
    if (leer(leer(o + 16) + 16) == g_boton_hash) {
      lista = l;
      break;
    }
  }
  if (!lista) {
    REXLOG_INFO("[menu raton] opciones: ninguna lista con la fila {:08X} ({} vistas)", g_boton_hash, g_listas.size());
    return;
  }
  uint32_t mejor = 0, actual = leer(lista + 12);
  REXLOG_INFO("[menu raton] opciones: lista {:08X} vtable {:08X} actual {:08X} cabeza {:08X} cuenta {}", lista,
              leer(lista), actual, leer(lista + 4), leer(lista + 36));
  float mejor_d = kAltoFila * 0.5f, primera = 0.0f;
  bool hay_primera = false;
  int vueltas = 0;
  for (uint32_t n = leer(lista + 4); valida(n) && n != lista + 4 && vueltas < 64; n = leer(n), ++vueltas) {
    const uint32_t opcion = n - 4;
    uint32_t v = leer(opcion + 20);
    float dy;
    std::memcpy(&dy, &v, 4);
    if (!hay_primera) primera = dy, hay_primera = true;
    REXLOG_INFO("[menu raton] opciones: fila {} {:08X} y {:.1f}", vueltas, opcion, dy);
    const float centro = kPrimeraFila + (dy - primera);
    if (std::abs(y - centro) < mejor_d) mejor_d = std::abs(y - centro), mejor = opcion;
  }
  if (!mejor || mejor == actual) {
    REXLOG_INFO("[menu raton] opciones: fila en y {:.0f} {}", y, mejor ? "ya elegida" : "no encontrada");
    return;
  }
  REXLOG_INFO("[menu raton] opciones: eligiendo {:08X}", mejor);
  const PPCContext guardado = ctx;
  ctx.r3.u64 = lista;
  ctx.r4.u64 = mejor;
  __imp__sub_822DECA8(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = guardado;
  REXLOG_INFO("[menu raton] opciones: fila en y {:.0f} elegida ({})", y, r);
}

REX_HOOK_RAW(sub_822FD0D0) {
  static std::atomic<uint32_t> llamadas{0};
  const uint32_t n = llamadas.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n == 600) {
    REXLOG_INFO("[menu raton] FEngine::Update llamada {} veces; motor en {:08X}, bytes +0 {} +1 {}; opcion {}", n,
                ctx.r3.u32, ctx.r3.u32 ? base[ctx.r3.u32] : 0, ctx.r3.u32 ? base[ctx.r3.u32 + 1] : 0,
                REXCVAR_GET(nfsmw_menu_raton));
  }
  if (REXCVAR_GET(nfsmw_menu_raton) && ctx.r3.u32) {
    base[ctx.r3.u32 + 1] = 1;
    // The menu screen on top: the engine's package at +0xE0, its name at +0xC (e.g. "CustomizeParts.fng").
    {
      uint32_t p, c;
      std::memcpy(&p, base + ctx.r3.u32 + 0xE0, 4);
      p = __builtin_bswap32(p);
      std::string nombre;
      if (p >= 0x80000000u && p < 0xC0000000u) {
        std::memcpy(&c, base + p + 0xC, 4);
        c = __builtin_bswap32(c);
        if (c >= 0x80000000u && c < 0xC0000000u) {
          const char* t = reinterpret_cast<const char*>(base + c);
          size_t n = 0;
          while (n < 48 && t[n] >= 0x20 && t[n] < 0x7F) ++n;
          nombre.assign(t, n);
        }
      }
      // While driving the menus do not read the mouse, so this is checked here, every FEng frame: a press on the
      // event prompt's round icon (top left, measured on the player's clicks at -327..-298, -185..-166) enters the
      // event, as D-pad up does.
      if (nombre.rfind("HUD_", 0) == 0) {
        int16_t mx = 0, my = 0;
        uint16_t mb = 0;
        static uint16_t antes = 0;
        if (LeerRaton(mx, my, mb)) {
          if ((mb & 1) && !(antes & 1)) {
            REXLOG_INFO("[menu raton] carrera: clic en {},{}", mx, my);
            CapturarClic(g_clic_numero.fetch_add(1, std::memory_order_relaxed) + 1, mx, my);
          }
          if ((mb & 1) && !(antes & 1) && mx >= -355 && mx <= -282 && my >= -212 && my <= -140) {
            g_otros_botones.store(kArriba, std::memory_order_relaxed);
            g_otros_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
            REXLOG_INFO("[menu raton] carrera: entrar al evento (arriba)");
          }
          antes = mb;
        }
      }
      if (nombre != g_pantalla) {
        g_pantalla = nombre;
        if (nombre == "CustomizeMain.fng") g_en_taller = true;
        if (nombre.rfind("MainMenu", 0) == 0 || nombre.rfind("HUD", 0) == 0 || nombre.rfind("Loading", 0) == 0 ||
            nombre.rfind("WS_", 0) == 0 || nombre.rfind("Pause", 0) == 0 || nombre.empty())
          g_en_taller = false;
        REXLOG_INFO("[menu raton] pantalla: {}", nombre);
      }
    }
    if (g_pintura_pendiente.exchange(false, std::memory_order_relaxed) && g_scroller) {
      // The swatches' centres are in the grid's own space (20 x 4, x -140..148 and y -14..40, 15.2 and 18 apart):
      // measured on a 1600x900 capture (columns 27.4 px apart from x 322, rows 32.7 px from y 198), it sits at -119.9,-120.8 at 0.9645 x 0.968 scale. The click is taken there.
      const float px = (float(g_pintura_x.load(std::memory_order_relaxed)) + 119.9f) / 0.9645f;
      const float py = (float(g_pintura_y.load(std::memory_order_relaxed)) + 120.8f) / 0.968f;
      uint32_t mejor = 0;
      int mejor_k = -1;
      uint32_t hashes[128] = {};
      float mejor_d = 12.0f * 12.0f;
      const auto guardado = ctx;
      for (int k = 0; k < 128; ++k) {
        ctx.r3.u64 = g_scroller;
        ctx.r4.u64 = uint32_t(k);
        __imp__sub_822E0ED8(ctx, base);
        const uint32_t ranura = ctx.r3.u32;
        if (!ranura) break;
        uint32_t obj;
        std::memcpy(&obj, base + ranura + 12, 4);
        obj = __builtin_bswap32(obj);
        const auto c = g_centros.find(obj);
        if (obj) {
          uint32_t hh;
          std::memcpy(&hh, base + obj + 16, 4);
          hashes[k] = __builtin_bswap32(hh);
        }
        if (!obj || c == g_centros.end()) continue;
        const float dx = c->second.first - px, dy = c->second.second - py;
        if (dx * dx + dy * dy < mejor_d) {
          mejor_d = dx * dx + dy * dy;
          uint32_t h;
          std::memcpy(&h, base + obj + 16, 4);
          mejor = __builtin_bswap32(h);
          mejor_k = k;
        }
      }
      ctx = guardado;
      if (mejor) {
        // The paint screen repaints the car only on a D-pad press: the swatch next to the clicked one is chosen
        // directly and one step brings the selection onto it (20 per row).
        const int col = mejor_k % 20;
        const int vecino = col > 0 ? mejor_k - 1 : mejor_k + 1;
        if (vecino >= 0 && vecino < 128 && hashes[vecino] && g_pasos.empty()) {
          g_casilla_pendiente.store(hashes[vecino], std::memory_order_relaxed);
          g_pasos.push_back(col > 0 ? kDerecha : kIzquierda);
          g_paso_ms = AhoraMs() + kPasoMs;  // the step after the selection has been made
        } else {
          g_casilla_pendiente.store(mejor, std::memory_order_relaxed);
        }
      } else {
        REXLOG_INFO("[menu raton] pintura: ningun color cerca de {:.0f},{:.0f}", px, py);
      }
    }
    const uint32_t pendiente = g_casilla_pendiente.exchange(0, std::memory_order_relaxed);
    if (pendiente && !SeleccionarEnScroller(ctx, base, pendiente)) {
      REXLOG_INFO("[menu raton] cuadricula: boton {:08X} no se pudo elegir", pendiente);
    }
    const int64_t aviso = g_aviso_reinicio_ms.load(std::memory_order_relaxed);
    if (aviso && AhoraMs() >= aviso) {
      g_aviso_reinicio_ms.store(0, std::memory_order_relaxed);
      MostrarAvisoReinicio(ctx, base);
    }
    const int32_t fila_y = g_fila_pendiente.exchange(INT32_MIN, std::memory_order_relaxed);
    if (fila_y != INT32_MIN && (g_opciones || g_lista_filas)) SeleccionarOpcion(ctx, base, float(fila_y));
    if (!g_pasos.empty() && AhoraMs() >= g_paso_ms) {
      g_otros_botones.store(g_pasos.front(), std::memory_order_relaxed);
      // Held for 100 ms and 180 apart: at 30 FPS or less (phones) a shorter press can fall between two of the
      // game's input reads and be lost.
      g_otros_hasta_ms.store(AhoraMs() + 100, std::memory_order_relaxed);
      g_paso_ms = AhoraMs() + 180;
      g_pasos.pop_front();
    }
    // The D-pad walk to a clicked cell: a step every 180 ms until the selection is the cell.
    if (g_camino.destino && AhoraMs() >= g_camino.siguiente_ms) {
      uint32_t actual = 0;
      std::memcpy(&actual, base + g_camino.paquete + kBotonActual, 4);
      actual = __builtin_bswap32(actual);
      auto hash_de = [&](uint32_t o) {
        uint32_t v = 0;
        if (o) std::memcpy(&v, base + o + 0x10, 4);
        return __builtin_bswap32(v);
      };
      // The click may land on an object drawn over a cell (a frame with its own hash family) that the selection
      // never reaches: walk to the cell of the selection's family whose centre is nearest the clicked one.
      if (actual && g_camino.pasos == 0 &&
          std::abs(int32_t(hash_de(g_camino.destino) - hash_de(actual))) > 64) {
        const auto c = g_centros.find(g_camino.destino);
        uint32_t mejor = 0;
        float mejor_d = 80.0f * 80.0f;
        if (c != g_centros.end()) {
          const uint32_t h = hash_de(actual);
          for (const auto& [o, centro] : g_centros) {
            if (std::abs(int32_t(hash_de(o) - h)) > 64) continue;
            const float dx = centro.first - c->second.first, dy = centro.second - c->second.second;
            if (dx * dx + dy * dy < mejor_d) mejor_d = dx * dx + dy * dy, mejor = o;
          }
        }
        REXLOG_INFO("[menu raton] cuadricula: objeto {:08X} -> casilla {:08X}", g_camino.destino, mejor);
        g_camino.destino = mejor;
      }
      if (!g_camino.destino) g_camino.pasos = 24;  // no cell there: give up below
      const uint32_t hash_actual = hash_de(actual), hash_destino = hash_de(g_camino.destino);
      // A vertical step moves the selection by one row: the hash difference is the row width.
      if ((g_camino.ultimo_paso == kAbajo || g_camino.ultimo_paso == kArriba) && g_camino.hash_antes) {
        const int salto = std::abs(int32_t(hash_actual - g_camino.hash_antes));
        if (salto >= 2 && salto <= 8 && salto != g_columnas) {
          g_columnas = salto;
          REXLOG_INFO("[menu raton] cuadricula: {} columnas", g_columnas);
        }
      }
      const int32_t diferencia = int32_t(hash_destino - hash_actual);
      const bool por_hash = actual && std::abs(diferencia) <= 64;
      const auto a = g_centros.find(actual), d = g_centros.find(g_camino.destino);
      if (actual == g_camino.destino || (!por_hash && (a == g_centros.end() || d == g_centros.end())) ||
          g_camino.pasos >= 24) {
        REXLOG_INFO("[menu raton] cuadricula: {} en {} pasos", actual == g_camino.destino ? "llegado" : "abandonado",
                    g_camino.pasos);
        g_camino = Camino{};
      } else {
        uint16_t paso;
        if (por_hash) {
          // The cells' hashes are consecutive in reading order: whole rows first, then the rest of the way.
          if (diferencia >= g_columnas) paso = kAbajo;
          else if (diferencia <= -g_columnas) paso = kArriba;
          else paso = diferencia > 0 ? kDerecha : kIzquierda;
        } else {
          const float dx = d->second.first - a->second.first, dy = d->second.second - a->second.second;
          paso = std::abs(dx) > std::abs(dy) ? (dx > 0 ? kDerecha : kIzquierda) : (dy > 0 ? kAbajo : kArriba);
        }
        g_camino.hash_antes = hash_actual;
        g_camino.ultimo_paso = paso;
        g_otros_botones.store(paso, std::memory_order_relaxed);
        g_otros_hasta_ms.store(AhoraMs() + 80, std::memory_order_relaxed);
        g_camino.siguiente_ms = AhoraMs() + 180;
        ++g_camino.pasos;
      }
    }
    // A pending click, once the game has had time to process it: A only if the selection did not move.
    const int64_t vence = g_pendiente_ms.load(std::memory_order_acquire);
    if (vence && AhoraMs() >= vence) {
      g_pendiente_ms.store(0, std::memory_order_relaxed);
      const uint32_t paquete = g_pendiente_paquete.load(std::memory_order_relaxed);
      uint32_t ahora = 0;
      std::memcpy(&ahora, base + paquete + 0x98, 4);
      ahora = __builtin_bswap32(ahora);
      const bool igual = ahora == g_pendiente_seleccion.load(std::memory_order_relaxed);
      if (igual) g_a_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
      REXLOG_INFO("[menu raton] clic resuelto: la seleccion {} -> {}", igual ? "no cambio" : "cambio",
                  igual ? "se pulsa A" : "solo se mueve");
    }
    static std::atomic<bool> dicho{false};
    auto leer = [&](uint32_t d) {
      uint32_t v;
      std::memcpy(&v, base + d, 4);
      return __builtin_bswap32(v);
    };
    const uint32_t iface = leer(ctx.r3.u32 + 264);
    if (iface && !dicho.exchange(true)) {
      const uint32_t vtable = leer(iface);
      REXLOG_INFO("[menu raton] interfaz del motor en {:08X}, vtable {:08X}: GetMouseInfo (17) = {:08X}, "
                  "DoesPointTouchObject (18) = {:08X}",
                  iface, vtable, leer(vtable + 68), leer(vtable + 72));
    }
  }
  __imp__sub_822FD0D0(ctx, base);
}

// FEMouse::Update(this, FEMouseInfo* r4, tDelta r5): the info the game's interface returned is replaced by the cursor.
REX_EXTERN(__imp__sub_822FB270);
REX_HOOK_RAW(sub_822FB270) {
  if (REXCVAR_GET(nfsmw_menu_raton) && ctx.r4.u32) {
    int16_t x = 0, y = 0;
    uint16_t botones = 0;
    if (LeerRaton(x, y, botones)) {
      if (!(botones & 1)) g_mantenido.store(0, std::memory_order_relaxed);
      // Dragging the car: each FEng frame, the movement since the last one becomes the right stick.
      {
        static int16_t px = 0, py = 0;
        if (g_mantenido.load(std::memory_order_relaxed) == 4) {
          const int dx = x - px, dy = y - py;
          g_stick_x.store(int16_t(std::clamp(dx * 2600, -32767, 32767)), std::memory_order_relaxed);
          g_stick_y.store(int16_t(std::clamp(-dy * 2600, -32767, 32767)), std::memory_order_relaxed);
        }
        px = x;
        py = y;
      }
      // A click inside a grid arrow never reaches the menu (the cell under the arrow would take it and accept): the
      // arrow's step goes to the virtual pad instead.
      static bool en_flecha = false;
      static uint16_t previos = 0;
      // Where the last menu click landed, and until when the cursor is shown there: the grid accepts A only while
      // the cursor is over its selection, so the bar's A puts it back on the last clicked cell.
      static int16_t ultimo_x = 0, ultimo_y = 0;
      static int64_t volver_hasta_ms = 0;
      // The same two arrow pieces serve the challenge grid (one above the other: up and down) and the pause menu's
      // carousel (side by side: left and right).
      const Flecha* vivas[2] = {nullptr, nullptr};
      for (const auto& [o, f] : g_flechas) {
        if (AhoraMs() - f.visto_ms >= 500) continue;
        uint32_t h;
        std::memcpy(&h, base + o + 0x10, 4);
        vivas[__builtin_bswap32(h) == 0x444969FDu ? 0 : 1] = &f;
      }
      const bool lado = vivas[0] && vivas[1] && std::abs(vivas[0]->r[1] - vivas[1]->r[1]) < 10;
      const bool serie = (vivas[0] || vivas[1]) && !lado;
      // Only in a menu (FEng hit-tested some mouse object just now). While driving there is none, and the bottom of
      // the screen, where the bar zones are, is where the touch pedals are: no pad press may come from there.
      if (AhoraMs() - g_menu_ms.load(std::memory_order_relaxed) > 500) {
        previos = botones;
        botones = 0;
      }
      if (botones && !previos) {
        en_flecha = false;
        // Every press, also the ones the zones below turn into pad buttons.
        if (botones & 1) CapturarClic(g_clic_numero.load(std::memory_order_relaxed) + 1, x, y);
        for (const auto& [o, f] : g_flechas) {
          if (AhoraMs() - f.visto_ms < 500 && x >= f.r[0] - kMargenFlecha && x <= f.r[2] + kMargenFlecha &&
              y >= f.r[1] - kMargenFlecha && y <= f.r[3] + kMargenFlecha) {
            uint32_t h;
            std::memcpy(&h, base + o + 0x10, 4);
            const bool arriba = __builtin_bswap32(h) == 0x444969FDu;
            const bool izquierda = lado && f.r[0] < (arriba ? vivas[1] : vivas[0])->r[0];
            g_otros_botones.store(lado ? (izquierda ? kIzquierda : kDerecha) : (arriba ? kArriba : kAbajo),
                                  std::memory_order_relaxed);
            g_otros_hasta_ms.store(AhoraMs() + 80, std::memory_order_relaxed);
            REXLOG_INFO("[menu raton] flecha: {}", lado ? (izquierda ? "izquierda" : "derecha") : (arriba ? "arriba" : "abajo"));
            en_flecha = true;
            break;
          }
        }
      }
      // A click on an options list's row (not on its slider arrows, which are mouse objects) selects it.
      if ((botones & 1) && !(previos & 1) && !en_flecha && (g_opciones || g_lista_filas) &&
          AhoraMs() - g_opciones_ms.load(std::memory_order_relaxed) < 500 && y > kPrimeraFila - 14 && y < 150 &&
          x > -280 && x < 290) {
        g_fila_pendiente.store(y, std::memory_order_relaxed);
      }
      // Right click is B (back), as on PC games; the menu does not see it as a mouse button.
      if ((botones & 2) && !(previos & 2)) {
        g_otros_botones.store(kBotonB, std::memory_order_relaxed);
        g_otros_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
        REXLOG_INFO("[menu raton] clic derecho: B");
      }
      if ((botones & 1) && !(previos & 1) && y > 170) {
        auto leer = [&](uint32_t d) {
          uint32_t v;
          std::memcpy(&v, base + d, 4);
          return __builtin_bswap32(v);
        };
        for (const auto& [o, f] : g_barra) {
          if (AhoraMs() - f.visto_ms > 500) continue;
          std::string w;
          char t[16];
          for (uint32_t k = 0x20; k < 0x60; k += 4) std::snprintf(t, sizeof(t), " %08X", leer(o + k)), w += t;
          const uint32_t datos = leer(o + 0x2C);
          if (datos && (datos >> 28) == (o >> 28)) {
            w += " | datos";
            for (uint32_t k = 0; k < 0x40; k += 4) std::snprintf(t, sizeof(t), " %08X", leer(datos + k)), w += t;
          }
          REXLOG_INFO("[menu raton] barra {:08X} hash {:08X} rect {:.0f},{:.0f}-{:.0f},{:.0f}:{}", o, leer(o + 0x10),
                      f.r[0], f.r[1], f.r[2], f.r[3], w);
        }
      }
      // The button bar of the challenge series (its own mouse zones are misplaced and one runs another button's
      // action): zones measured on the drawing, each one pressing its pad button. The grid arrows mark the screen.
      if ((botones & 1) && !(previos & 1) && !en_flecha) {
        // The pause menu: its chosen item (between the side arrows) or the item's name bar enters, and its own
        // button bar (A then B) sits just below.
        if (lado) {
          const float medio = (vivas[0]->r[2] + vivas[1]->r[0]) * 0.5f, alto = vivas[0]->r[1];
          uint16_t boton = 0;
          const char* que = "";
          if (std::abs(x - (medio - 6)) <= 36 && y >= alto - 14 && y <= alto + 46) {
            boton = kBotonA, que = "elegido";
          } else if (y >= alto + 38 && y <= alto + 82) {
            if (x >= -250 && x <= -150) boton = kBotonA, que = "A";
            else if (x >= -140 && x <= -60) boton = kBotonB, que = "B";
            else if (x >= vivas[0]->r[0] - 10 && x <= vivas[1]->r[2] + 10) boton = kBotonA, que = "nombre";
          }
          if (boton) {
            en_flecha = true;
            g_otros_botones.store(boton, std::memory_order_relaxed);
            g_otros_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
            REXLOG_INFO("[menu raton] menu de pausa: {}", que);
          }
        }
        // The garage (and the career screens built like it) has a fourth bar slot, 18A4384F ("RB Visualizar"), and
        // its slots are where they are drawn: there the game answers every click itself and none of the fixed zones
        // below (measured on the challenge series, quick race, pause and options) may fire.
        // Screens with their own measured zones (from their screenshots), by name.
        {
          struct ZonaPantalla {
            const char* pantalla;
            float x0, x1, y0, y1;
            uint16_t boton;
            const char* nombre;
          };
          static const ZonaPantalla kZonasPantalla[] = {
              // Buying a car: B Salir, A Comprar coche, RB Visualizar, and the arrows that change the car.
              {"Car_Select.fng", -272, -198, 180, 206, kBotonB, "B"},
              {"Car_Select.fng", -170, -40, 180, 206, kBotonA, "A"},
              {"Car_Select.fng", -36, 72, 180, 206, 0x0200, "RB (visualizar)"},
              {"Car_Select.fng", -272, -236, -175, -145, kIzquierda, "coche anterior"},
              {"Car_Select.fng", 60, 100, -175, -145, kDerecha, "coche siguiente"},
          };
          if (!en_flecha) {
            for (const ZonaPantalla& z : kZonasPantalla) {
              if (g_pantalla == z.pantalla && x >= z.x0 && x <= z.x1 && y >= z.y0 && y <= z.y1) {
                en_flecha = true;
                g_otros_botones.store(z.boton, std::memory_order_relaxed);
                g_otros_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
                REXLOG_INFO("[menu raton] {}: {}", g_pantalla, z.nombre);
                break;
              }
            }
          }
        }
        const bool con_zonas_propias = g_pantalla == "Car_Select.fng";
        // The showcase (Visualizar) is reached from the garage and from buying a car alike.
        const bool taller = g_en_taller || con_zonas_propias || g_pantalla == "Showcase.fng";
        const bool carrusel = !taller && AhoraMs() - g_carrusel_ms.load(std::memory_order_relaxed) < 500;
        // The carousel's chosen mode (the framed icon between the arrows) enters, as A.
        if (carrusel && !serie && x >= 80 && x <= 160 && y >= 105 && y <= 180) {
          en_flecha = true;
          g_a_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
          REXLOG_INFO("[menu raton] carrusel: modo elegido, A");
        }
        // Any screen with a button bar (its slots, flags 9, drawn just now): the same layout everywhere, B then A on
        // the left, and the third button on the right (Y, "al azar") except in the challenge series (X, tutorial).
        bool barra = serie || carrusel;
        for (const auto& [o, f] : g_barra) barra |= AhoraMs() - f.visto_ms < 500;
        // The options screens (graphics, audio...) draw their bar a little higher and tighter: B, A, Y.
        if (taller) {
          // The garage: its mouse objects only get the mouse message. Its bar slots are where they are drawn and
          // each stands for one pad button (B, A, Start for the cart, RB to look at the car); its carousels' arrows
          // are left and right and the icon between them is A.
          struct Hueco {
            uint32_t hash;
            uint16_t boton;
            const char* nombre;
          };
          static const Hueco kHuecos[] = {{0xC0A32823u, kBotonB, "B"},
                                          {0x73DCB662u, kBotonA, "A"},
                                          {0xD6463100u, 0x0010, "Start"},
                                          {0x18A4384Fu, 0x0200, "RB"}};
          uint16_t boton = 0;
          const char* que = "";
          // Each garage screen's buttons where they are drawn (measured on its screenshots).
          struct Zona {
            float x0, x1, y0, y1;
            uint16_t boton;
            const char* nombre;
          };
          static const Zona kPiezas[] = {{-270, -192, 180, 206, kBotonB, "B"},
                                         {-185, -100, 180, 206, kBotonA, "A"},
                                         {-5, 114, 180, 206, 0x0010, "Start (carrito)"},
                                         {115, 250, 180, 206, 0x0200, "RB (visualizar)"},
                                         };
          static const Zona kCategorias[] = {{-270, -192, 178, 204, kBotonB, "B"},
                                             {-175, -90, 178, 204, kBotonA, "A"},
                                             {-270, -145, 148, 174, 0x0200, "RB (visualizar)"}};
          auto buscar = [&](const Zona* zonas, size_t n) {
            for (size_t i = 0; i < n; ++i)
              if (x >= zonas[i].x0 && x <= zonas[i].x1 && y >= zonas[i].y0 && y <= zonas[i].y1)
                boton = zonas[i].boton, que = zonas[i].nombre;
          };
          // Every piece screen (Spoilers, ...) is laid out as the parts one.
          const bool pieza = g_en_taller && !con_zonas_propias && g_pantalla != "CustomizeMain.fng" &&
                             g_pantalla != "CustomizeCategory.fng" && g_pantalla != "Showcase.fng";
          if (pieza) buscar(kPiezas, std::size(kPiezas));
          // The paint screen's finish selector (LT and RT icons over the grid, measured at 1600x900: x 340 and 827,
          // y 168), held while the button is down.
          if (g_pantalla == "Paint.fng" && !boton && y >= -166 && y <= -138) {
            int gatillo = 0;
            if (x >= -264 && x <= -226) gatillo = 1, que = "LT";
            else if (x >= -6 && x <= 34) gatillo = 2, que = "RT";
            if (gatillo) {
              g_mantenido.store(gatillo, std::memory_order_relaxed);
              en_flecha = true;
              REXLOG_INFO("[menu raton] pintura: {}", que);
            }
          }
          // The paint grid: the nearest swatch to the click (their hit areas are smaller than the squares drawn).
          if (g_pantalla == "Paint.fng" && !boton && !en_flecha && x >= -268 && x <= 34 && y >= -145 && y <= -70) {
            g_pintura_x.store(x, std::memory_order_relaxed);
            g_pintura_y.store(y, std::memory_order_relaxed);
            g_pintura_pendiente.store(true, std::memory_order_relaxed);
            en_flecha = true;
          }

          // Its brand selector ("Deportivos") is changed with the triggers, held while the button is down.
          if (pieza && !boton && y >= 150 && y <= 176) {
            int gatillo = 0;
            if (x >= 40 && x <= 80) gatillo = 1, que = "LT";
            else if (x >= 235 && x <= 275) gatillo = 2, que = "RT";
            if (gatillo) {
              g_mantenido.store(gatillo, std::memory_order_relaxed);
              en_flecha = true;
              REXLOG_INFO("[menu raton] taller: {}", que);
            }
          }
          // The showcase (Visualizar): B, and the analog ones held while the button is down.
          if (g_pantalla == "Showcase.fng" && y < 170) {
            g_mantenido.store(4, std::memory_order_relaxed);  // drag over the car to turn it
            en_flecha = true;
            que = "arrastrar el coche";
          }
          if (g_pantalla == "Showcase.fng" && y >= 178 && y <= 206) {
            int analogico = 0;
            if (x >= -270 && x <= -192) boton = kBotonB, que = "B";
            else if (x >= -185 && x <= -100) analogico = 3, que = "rotar";
            else if (x >= -68 && x <= 56) analogico = 1, que = "LT (alejar zoom)";
            else if (x >= 75 && x <= 225) analogico = 2, que = "RT (acercar zoom)";
            if (analogico) {
              g_mantenido.store(analogico, std::memory_order_relaxed);
              en_flecha = true;
              REXLOG_INFO("[menu raton] taller: {}", que);
            }
          }
          // The garage's main screen draws the same bar as the categories one (B Hecho, A Elegir, RB Visualizar).
          if (g_pantalla == "CustomizeCategory.fng" || g_pantalla == "CustomizeMain.fng")
            buscar(kCategorias, std::size(kCategorias));
          for (const auto& [o, f] : g_barra) {
            if (boton) break;
            if (AhoraMs() - f.visto_ms >= 4000 || x < f.r[0] || x > f.r[2] || y < f.r[1] || y > f.r[3]) continue;
            uint32_t h, visible;
            std::memcpy(&h, base + o + 0x10, 4);
            std::memcpy(&visible, base + o + 0x20, 4);
            if (!visible) continue;
            for (const Hueco& k : kHuecos)
              if (__builtin_bswap32(h) == k.hash) boton = k.boton, que = k.nombre;
          }
          const Flecha &izq = g_flechas_carrusel[0], &der = g_flechas_carrusel[1];
          if (!boton && AhoraMs() - izq.visto_ms < 500 && AhoraMs() - der.visto_ms < 500) {
            const float cy = (izq.r[1] + izq.r[3]) * 0.5f;
            if (std::abs(y - cy) < 26.0f) {
              const float medio = (izq.r[2] + der.r[0]) * 0.5f;
              if (x >= izq.r[0] - 6 && x <= izq.r[2] + 6) boton = kIzquierda, que = "carrusel izquierda";
              else if (x >= der.r[0] - 6 && x <= der.r[2] + 6) boton = kDerecha, que = "carrusel derecha";
              else if (std::abs(x - medio) < 34.0f) boton = kBotonA, que = "carrusel elegido";
            }
          }
          // The carousel's chosen item where the quick race one has it (the garage's piece carousels share it).
          if (!boton && AhoraMs() - g_carrusel_ms.load(std::memory_order_relaxed) < 500 && x >= 80 && x <= 160 &&
              y >= 105 && y <= 180) {
            boton = kBotonA, que = "carrusel elegido";
          }
          if (boton) {
            en_flecha = true;
            g_otros_botones.store(boton, std::memory_order_relaxed);
            g_otros_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
            REXLOG_INFO("[menu raton] taller: {}", que);
          }
        } else if (!lado && !serie && y >= 164 && y < 182) {
          struct Zona {
            int x0, x1;
            uint16_t boton;
            const char* nombre;
          };
          static const Zona kOpciones[] = {{-238, -164, kBotonB, "B"}, {-152, -56, kBotonA, "A"}, {-42, 82, kBotonY, "Y"}};
          for (const Zona& z : kOpciones) {
            if (x >= z.x0 && x <= z.x1) {
              en_flecha = true;
              g_otros_botones.store(z.boton, std::memory_order_relaxed);
              g_otros_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
              REXLOG_INFO("[menu raton] barra de opciones: {}", z.nombre);
            }
          }
        }
        // A bar whose own slots are where they are drawn (the garage, the career screens) answers the click by
        // itself: the game sends each slot's message. Only the challenge series has its slots misplaced.
        bool slot_propio = false;
        for (const auto& [o, f] : g_barra) {
          if (AhoraMs() - f.visto_ms < 500 && x >= f.r[0] && x <= f.r[2] && y >= f.r[1] && y <= f.r[3]) {
            uint32_t visible;
            std::memcpy(&visible, base + o + 0x20, 4);
            slot_propio |= visible != 0;
          }
        }
        if (taller) {
          // the garage zones above already answered
        } else if (slot_propio && !serie) {
          REXLOG_INFO("[menu raton] barra: el clic va al hueco del propio juego");
        } else if (barra && !lado && !taller && y >= 182 && y <= 212) {
          struct Zona {
            int x0, x1;
            uint16_t boton;
            const char* nombre;
          };
          static const Zona kZonas[] = {{-265, -186, kBotonB, "B"}, {-170, -80, kBotonA, "A"}, {-62, 54, kBotonX, "X"}, {140, 240, kBotonY, "Y"}};
          en_flecha = true;  // the menu does not see this click
          for (const Zona& z : kZonas) {
            if (serie ? z.boton == kBotonY : z.boton == kBotonX) continue;
            if (x >= z.x0 && x <= z.x1) {
              g_otros_botones.store(z.boton, std::memory_order_relaxed);
              g_otros_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
              if (serie && z.boton == kBotonA) volver_hasta_ms = AhoraMs() + kPulsacionMs + 100;
              REXLOG_INFO("[menu raton] barra de botones: {}", z.nombre);
            }
          }
        }
      }
      if ((botones & 1) && !(previos & 1) && !en_flecha) {
        ultimo_x = x;
        ultimo_y = y;
      }
      previos = botones;
      botones &= ~uint16_t(2);
      if (AhoraMs() < volver_hasta_ms) {
        x = ultimo_x;
        y = ultimo_y;
      }
      if (en_flecha) {
        if (!botones) en_flecha = false;
        botones = 0;
      }
      uint8_t* info = base + ctx.r4.u32;
      const uint16_t bx = Swap16(uint16_t(x)), by = Swap16(uint16_t(y)), bb = Swap16(botones), bw = 0;
      std::memcpy(info + 0, &bx, 2);
      std::memcpy(info + 2, &by, 2);
      std::memcpy(info + 4, &bw, 2);
      std::memcpy(info + 6, &bb, 2);
      static uint16_t anteriores = 0;
      g_botones.store(botones, std::memory_order_relaxed);
      if (botones && !anteriores) g_clic_numero.fetch_add(1, std::memory_order_relaxed);
      if (botones != anteriores) {
        REXLOG_INFO("[menu raton] {} en {},{} (espacio de FEng, centrado)", botones ? "clic" : "soltado", x, y);
        anteriores = botones;
      }
      const uint32_t n = g_actualizaciones.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n == 1 || n % 300 == 0) {
        REXLOG_INFO("[menu raton] raton del motor de menus activo: {} actualizaciones, cursor en {},{} (espacio de FEng, centrado), "
                    "botones {}",
                    n, x, y, botones);
      }
    }
  }
  __imp__sub_822FB270(ctx, base);
}

// FEngine::UpdateMouseState(pkg, state, mx, my) (sub_822FE1F8, 0x35C bytes as in the prototype): called once per
// mouse object of the package. Counted, to know whether the console's menus flag any object for the mouse.
// The challenge grid's scroller (the object of sub_822E0740, its "set selected item"), seen on its last call.
REX_EXTERN(__imp__sub_822E0740);
REX_EXTERN(__imp__sub_822E0800);
REX_EXTERN(__imp__sub_822E0ED8);

// A recompiled function by guest address (the scroller's virtual refresh), cached.
PPCFunc* FuncionGuest(uint32_t direccion) {
  static std::unordered_map<uint32_t, PPCFunc*> cache;
  const auto it = cache.find(direccion);
  if (it != cache.end()) return it->second;
  PPCFunc* f = nullptr;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    if (m->guest == direccion) {
      f = m->host;
      break;
    }
  }
  cache[direccion] = f;
  return f;
}

// Chooses the grid's cell `casilla` (0-based on screen) the way the screen does: items are a list (node at item+4,
// next at +0, head at scroller+16) and +28 is the first item on screen.
bool SeleccionarEnScroller(PPCContext& ctx, uint8_t* base, uint32_t hash) {
  if (!g_scroller) return false;
  auto leer = [&](uint32_t d) {
    uint32_t v;
    std::memcpy(&v, base + d, 4);
    return __builtin_bswap32(v);
  };
  const uint32_t cabeza = g_scroller + 16, arriba = leer(g_scroller + 28);
  // The visible slot whose button is the clicked one: sub_822E0ED8(scroller, slot) gives the slot, its +12 the object.
  int casilla = -1;
  {
    const auto guardado = ctx;
    // Up to 128 slots: the paint grid shows 64 (16 x 4).
    std::string ranuras;
    for (int k = 0; k < 128 && casilla < 0; ++k) {
      ctx.r3.u64 = g_scroller;
      ctx.r4.u64 = uint32_t(k);
      __imp__sub_822E0ED8(ctx, base);
      const uint32_t ranura = ctx.r3.u32;
      if (!ranura) break;
      const uint32_t obj = leer(ranura + 12);
      if (obj && leer(obj + 16) == hash) casilla = k;
      if (k < 80) ranuras += fmt::format(" {}:{:08X}", k, obj ? leer(obj + 16) : 0);
    }
    ctx = guardado;
    if (casilla < 0) {
      static int avisos = 0;
      if (avisos++ < 3) REXLOG_INFO("[menu raton] ranuras del scroller {:08X}:{}", g_scroller, ranuras);
    }
  }
  if (casilla < 0) return false;
  uint32_t nodo = leer(cabeza);
  for (uint32_t i = 0; i < arriba + uint32_t(casilla); ++i) {
    if (nodo == cabeza || !nodo) return false;
    nodo = leer(nodo);
  }
  if (nodo == cabeza || !nodo) return false;
  const uint32_t dato = nodo - 4;
  if (!base[dato + 20]) return false;  // the setter ignores items without this flag
  // As the scroller's own move (sub_822E0870): the new first row, the setter, the "changed" flag and its virtual
  // refresh (vtable +8), which lights the icon.
  const auto guardado = ctx;
  ctx.r3.u64 = g_scroller;
  ctx.r4.u64 = arriba + uint32_t(casilla);  // the item's number in the list, not its address
  ctx.r5.u64 = arriba;
  __imp__sub_822E0800(ctx, base);
  const uint32_t nueva_arriba = ctx.r3.u32;
  ctx.r3.u64 = g_scroller;
  ctx.r4.u64 = dato;
  ctx.r5.u64 = nueva_arriba;
  __imp__sub_822E0740(ctx, base);
  base[g_scroller + 4] = 1;
  const uint32_t refresco = leer(leer(g_scroller) + 8);
  if (PPCFunc* f = FuncionGuest(refresco)) {
    ctx.r3.u64 = g_scroller;
    f(ctx, base);
  } else {
    REXLOG_WARN("[menu raton] refresco del scroller {:08X} no encontrado", refresco);
  }
  ctx = guardado;
  REXLOG_INFO("[menu raton] cuadricula: casilla {} elegida directamente (dato {:08X}, arriba {} -> {})", casilla, dato,
              arriba, nueva_arriba);
  return true;
}

REX_EXTERN(__imp__sub_822FE1F8);
REX_EXTERN(__imp__sub_82303220);
// The press that landed on a grid arrow: the cell under the arrow does not take it too.
std::atomic<uint32_t> g_clic_flecha{0};
bool EsFilaFpsActual(uint8_t* base);

REX_HOOK_RAW(sub_822FE1F8) {
  auto leer = [&](uint32_t d) {
    uint32_t v;
    std::memcpy(&v, base + d, 4);
    return __builtin_bswap32(v);
  };
  const uint32_t paquete = ctx.r4.u32, estado = ctx.r5.u32;
  if (paquete) g_paquete_raton.store(paquete, std::memory_order_relaxed);
  const uint32_t antes = estado ? leer(estado + 12) : 0;
  const uint32_t objeto = estado ? leer(estado) : 0;
  static std::atomic<uint32_t> llamadas{0};
  const uint32_t n = llamadas.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n % 500 == 0) {
    REXLOG_INFO("[menu raton] UpdateMouseState: {} llamadas (objetos de raton en los menus); paquete {:08X}, "
                "cursor {:.0f},{:.0f}",
                n, ctx.r4.u32, ctx.f1.f64, ctx.f2.f64);
  }
  __imp__sub_822FE1F8(ctx, base);
  // The package's fields from +0x80 to +0xC8, every 1.5 s while it holds challenge grid cells (hash 09C9A0xx), with
  // the cells' addresses: the field that follows the selection when it moves is the current button.
  if (paquete && objeto) {
    const uint32_t hash = leer(objeto + 0x10);
    static std::string celdas;
    static int64_t ultimo_ms = 0;
    if ((hash & 0xFFFFFE00u) == 0x09C9A000u) {
      char t[24];
      std::snprintf(t, sizeof(t), "%02X=%08X ", hash & 0xFF, objeto);
      if (celdas.find(t) == std::string::npos && celdas.size() < 400) celdas += t;
      const int64_t ahora = AhoraMs();
      if (ahora - ultimo_ms > 1500) {
        ultimo_ms = ahora;
        std::string campos;
        for (uint32_t o = 0x80; o <= 0xC8; o += 4) {
          std::snprintf(t, sizeof(t), "%02X:%08X ", o, leer(paquete + o));
          campos += t;
        }
        REXLOG_INFO("[menu raton] paquete {:08X} campos {}| casillas {}", paquete, campos, celdas);
      }
    }
  }
  // State bits: 1 over, 2 left pressed. A left click released over the package's current button
  // (FEPackage::pCurrentButton, +0x98) accepts it.
  if (REXCVAR_GET(nfsmw_menu_raton) && estado && paquete) {
    const uint32_t despues = leer(estado + 12);
    // The menu's selection (FEPackage::pCurrentButton, +0x98) when the press started on this object.
    static uint32_t seleccion_al_pulsar = 0, estado_pulsado = 0;
    if (!(antes & 2) && (despues & 2) && (despues & 1)) {
      seleccion_al_pulsar = leer(paquete + 0x98);
      estado_pulsado = estado;
    }
    if ((antes & 2) && !(despues & 2) && (despues & 1)) {
      const uint32_t actual = leer(paquete + 0x98);
      // A click that did not move the selection (not an arrow, not another option) lands on the chosen one: it
      // enters it, as A does. One click chooses, a click on what is chosen enters.
      // The package's current button does not follow the carousel (it stays the same object while the arrows turn
      // it), so the selection says nothing. What tells them apart is what each object answers: the arrows act on
      // the press (F459B307) and do not answer the release; the chosen option answers the release (7EABCA56) but
      // the console's menus only use it for an animation, and accept with A.
      bool atiende_soltado = false, atiende_raton = false;
      if (objeto) {
        const uint32_t lista = objeto + 0x34;
        for (int forma = 0; forma < 2 && !atiende_soltado; ++forma) {
          const uint32_t cuenta = leer(lista + (forma ? 4 : 0)), cabeza = leer(lista + (forma ? 8 : 4));
          if (cuenta == 0 || cuenta > 64 || (cabeza >> 28) != (objeto >> 28)) continue;
          uint32_t nodo = cabeza;
          for (uint32_t i = 0; i < cuenta && nodo && (nodo >> 28) == (objeto >> 28); ++i) {
            const uint32_t m = leer(nodo + 0xC);
            atiende_soltado |= m == 0x7EABCA56u;
            atiende_raton |= m == 0x7EABCA56u || m == 0xF459B307u || m == 0x13F4BD45u || m == 0xB30D0683u ||
                             m == 0x1E646B2Eu;
            nodo = leer(nodo + 4);
          }
        }
      }
      (void)seleccion_al_pulsar;
      // The menu's current button: a click on it enters. A click on an object with no mouse answer at all (the
      // challenge grid's cells) walks the D-pad to it. An object that answers the release but is not the current
      // button (the main menu carousel's chosen slot) enters too; the arrows answer only the press and are left alone.
      const uint32_t boton = leer(paquete + kBotonActual);
      const bool mismo = objeto && estado == estado_pulsado &&
                         g_clic_flecha.load(std::memory_order_relaxed) != g_clic_numero.load(std::memory_order_relaxed);
      // Images that answer the release (the challenge grid's up/down arrows, type 5) act on their own message.
      const bool imagen = objeto && leer(objeto + 0x18) == 5;
      const bool acepta = mismo && (objeto == boton || (atiende_soltado && !imagen));
      // The options lists' slider arrows (< and > on the selected row) only get the press message: their click
      // changes the value as the D-pad does.
      if (mismo) {
        const uint32_t h = leer(objeto + 0x10);
        // The FPS row gets the arrow press from the game itself; a pad press on top would step twice.
        if ((h == 0xD1CDA594u || h == 0x9345E4A7u) && !EsFilaFpsActual(base)) {
          g_otros_botones.store(h == 0xD1CDA594u ? kIzquierda : kDerecha, std::memory_order_relaxed);
          g_otros_hasta_ms.store(AhoraMs() + 80, std::memory_order_relaxed);
          REXLOG_INFO("[menu raton] opciones: valor {}", h == 0xD1CDA594u ? "menos" : "mas");
        }
      }
      if (mismo && !acepta && !atiende_raton && boton && boton != objeto) {
        // FEPackage::SetCurrentButton (sub_82303220): unhighlights the old button and highlights the clicked one, as
        // the D-pad would, in a single jump.
        // The challenges screen keeps its own selection and lights the icon only on the D-pad's move: the walk is
        // made of real D-pad steps, all queued at once.
        const int desde = IndiceCasilla(leer(boton + 0x10)), hasta = IndiceCasilla(leer(objeto + 0x10));
        if (g_scroller) {
          g_casilla_pendiente.store(leer(objeto + 0x10), std::memory_order_relaxed);  // chosen on the next menu frame
        } else if (desde >= 0 && hasta >= 0 && g_pasos.empty()) {
          const int filas = hasta / g_columnas - desde / g_columnas, cols = hasta % g_columnas - desde % g_columnas;
          for (int k = 0; k < std::abs(filas); ++k) g_pasos.push_back(filas > 0 ? kAbajo : kArriba);
          for (int k = 0; k < std::abs(cols); ++k) g_pasos.push_back(cols > 0 ? kDerecha : kIzquierda);
          g_paso_ms = AhoraMs();
        }
        REXLOG_INFO("[menu raton] cuadricula: casilla {} a {} ({} pasos)", desde, hasta, g_pasos.size());
      }
      estado_pulsado = 0;
      if (acepta) g_a_hasta_ms.store(AhoraMs() + kPulsacionMs, std::memory_order_relaxed);
      auto posicion = [&](uint32_t o, float& x, float& y) {
        x = y = 0.0f;
        if (!o) return;
        const uint32_t datos = leer(o + 0x2C);
        // Only data in the same 256 MB as the object (the front-end heap): anything else is not a pointer to read.
        if (!datos || (datos >> 28) != (o >> 28)) return;
        uint32_t vx = leer(datos + 0x1C), vy = leer(datos + 0x20);
        std::memcpy(&x, &vx, 4);
        std::memcpy(&y, &vy, 4);
      };
      // The messages the clicked object answers (FEObject::Responses at +0x34, nodes with next at +4 and MsgID at
      // +0xC). Both list layouts are tried: {count, head, tail} and {vtable, count, head, tail}.
      std::string respuestas;
      if (objeto) {
        const uint32_t lista = objeto + 0x34;
        for (int forma = 0; forma < 2 && respuestas.empty(); ++forma) {
          const uint32_t cuenta = leer(lista + (forma ? 4 : 0)), cabeza = leer(lista + (forma ? 8 : 4));
          if (cuenta == 0 || cuenta > 64 || (cabeza >> 28) != (objeto >> 28)) continue;
          uint32_t nodo = cabeza;
          for (uint32_t i = 0; i < cuenta && nodo && (nodo >> 28) == (objeto >> 28); ++i) {
            char t[16];
            std::snprintf(t, sizeof(t), "%08X ", leer(nodo + 0xC));
            respuestas += t;
            nodo = leer(nodo + 4);
          }
          if (!respuestas.empty()) respuestas = (forma ? "[v] " : "[c] ") + respuestas;
        }
      }
      REXLOG_INFO("[menu raton] respuestas del objeto {:08X}: {}", objeto, respuestas.empty() ? "ninguna" : respuestas);
      float ox, oy, ax, ay;
      posicion(objeto, ox, oy);
      posicion(actual, ax, ay);
      REXLOG_INFO("[menu raton] clic soltado sobre {:08X} (hash {:08X}, pos {:.1f},{:.1f}); boton actual del menu "
                  "{:08X} (hash {:08X}, pos {:.1f},{:.1f}): {}",
                  objeto, objeto ? leer(objeto + 0x10) : 0, ox, oy, actual, actual ? leer(actual + 0x10) : 0, ax, ay,
                  acepta ? "atiende el clic soltado: se pulsa A" : "solo el mensaje de raton");
    }
  }
}

namespace nfsmw::fe_raton {
void EnvolverEntrada(rex::RuntimeConfig& config) {
  if (!REXCVAR_GET(nfsmw_menu_raton) || !config.input_factory) return;
  auto base = config.input_factory;
  config.input_factory = [base](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto sistema = base(tool_mode);
    if (sistema && !tool_mode) {
      static_cast<in::InputSystem*>(sistema.get())->AddDriver(std::make_unique<MandoRaton>());
      REXLOG_INFO("[menu raton] mando virtual del raton anadido (pulsa A al hacer clic en la opcion elegida)");
    }
    return sistema;
  };
}
}  // namespace nfsmw::fe_raton

// The interface's DoesPointTouchObject (method 18, sub_82283D58) jumps here with the point and the object: counted,
// with and without a hit, to see whether the cursor ever lands on a menu object.
REX_EXTERN(__imp__sub_82281AB8);
REX_HOOK_RAW(sub_82281AB8) {
  const double x = ctx.f1.f64, y = ctx.f2.f64;
  const uint32_t objeto = ctx.r5.u32;
  __imp__sub_82281AB8(ctx, base);
  // The challenge grid's up/down arrows (images 444969FD/444969FE): their release message accepts the cell on this
  // screen, so the game never sees them under the cursor; a click on one moves the selection with the D-pad instead.
  if (objeto && (ctx.r3.u32 & 0xFF) && REXCVAR_GET(nfsmw_menu_raton)) {
    uint32_t h;
    std::memcpy(&h, base + objeto + 0x10, 4);
    h = __builtin_bswap32(h);
    // The cursor point where an arrow was last under it: anything else at that point stays out of the mouse.
    static double flecha_x = 1e9, flecha_y = 1e9;
    static int64_t flecha_ms = 0;
    if (!EsFlecha(h)) {
      // The hit test gets coordinates of each object's own space, so the time is what tells: an arrow under the
      // cursor in the last frames.
      if (AhoraMs() - flecha_ms < 100) {
        ctx.r3.u64 = 0;
        return;
      }
    }
    if (EsFlecha(h)) {
      flecha_x = x, flecha_y = y, flecha_ms = AhoraMs();
      ctx.r3.u64 = 0;
      static uint32_t clic_hecho = 0;
      const uint32_t clic = g_clic_numero.load(std::memory_order_relaxed);
      if (g_botones.load(std::memory_order_relaxed) && clic != clic_hecho) {
        clic_hecho = clic;
        g_clic_flecha.store(clic, std::memory_order_relaxed);
        g_otros_botones.store(h == 0x444969FDu ? kArriba : kAbajo, std::memory_order_relaxed);
        g_otros_hasta_ms.store(AhoraMs() + 80, std::memory_order_relaxed);
        REXLOG_INFO("[menu raton] flecha de la cuadricula: {}", h == 0x444969FDu ? "arriba" : "abajo");
      }
      return;
    }
  }
  // Grid cells (hashes 09C9A0xx in the challenge series): their flags and colour, every so often, to find which data
  // follows the selected cell when the selection moves.
  if (objeto) {
    auto leer = [&](uint32_t d) {
      uint32_t v;
      std::memcpy(&v, base + d, 4);
      return __builtin_bswap32(v);
    };
    const uint32_t hash = leer(objeto + 0x10);
    if ((hash & 0xFFFFFF00u) == 0x09C9A000u || (hash & 0xFFFFFF00u) == 0x09C9A100u) {
      static std::string linea;
      static uint32_t primero = 0, vistos = 0;
      static int64_t ultimo_ms = 0;
      if (hash == primero || primero == 0) {
        const int64_t ahora = AhoraMs();
        if (!linea.empty() && ahora - ultimo_ms > 1500) {
          REXLOG_INFO("[menu raton] casillas: {}", linea);
          ultimo_ms = ahora;
        }
        linea.clear();
        primero = hash;
        vistos = 0;
      }
      if (vistos++ < 24) {
        const uint32_t datos = leer(objeto + 0x2C);
        uint32_t color = 0;
        if (datos && (datos >> 28) == (objeto >> 28)) color = leer(datos + 0xC);
        char t[48];
        std::snprintf(t, sizeof(t), "%02X:f%08X:c%08X ", hash & 0xFF, leer(objeto + 0x1C), color);
        linea += t;
      }
    }
  }
  static std::atomic<uint32_t> pruebas{0}, aciertos{0};
  const uint32_t n = pruebas.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool encima = (ctx.r3.u32 & 0xFF) != 0;
  const uint32_t a = encima ? aciertos.fetch_add(1, std::memory_order_relaxed) + 1 : aciertos.load();
  // On every press, the objects under the cursor by name: which ones the menus react to and which ones not.
  if (encima && objeto && g_botones.load(std::memory_order_relaxed)) {
    static uint32_t ultimo_clic = 0;
    static uint32_t vistos[16];
    static int nvistos = 0;
    const uint32_t clic = g_clic_numero.load(std::memory_order_relaxed);
    if (clic != ultimo_clic) {
      ultimo_clic = clic;
      nvistos = 0;
    }
    bool repetido = false;
    for (int i = 0; i < nvistos; ++i) repetido |= vistos[i] == objeto;
    if (!repetido && nvistos < 16) {
      vistos[nvistos++] = objeto;
      auto leer = [&](uint32_t d) {
        uint32_t v;
        std::memcpy(&v, base + d, 4);
        return __builtin_bswap32(v);
      };
      const uint32_t hash = leer(objeto + 0x10), nombre = leer(objeto + 0x14), tipo = leer(objeto + 0x18),
                     flags = leer(objeto + 0x1C);
      char texto[48] = {};
      if (nombre && nombre < 0xE0000000u) {
        for (int i = 0; i < 47 && base[nombre + i]; ++i) texto[i] = char(base[nombre + i]);
      }
      REXLOG_INFO("[menu raton] clic {} sobre objeto {:08X}: hash {:08X} nombre '{}' tipo {} flags {:08X}", clic, objeto,
                  hash, texto, tipo, flags);
      // The paint swatches belong to the same scroller as the challenge grid (sub_822E0740 moves its selection):
      // the clicked one is chosen directly at the start of the next FEng frame.
      if (g_pantalla == "Paint.fng" && g_scroller && tipo == 1) {
        g_casilla_pendiente.store(hash, std::memory_order_relaxed);
      }
      // Button bar slots (flags ...9): the object's words and its data's, to find which pad glyph it shows.
      if ((flags & 0xF) == 9) {
        std::string w = "obj";
        char t[16];
        for (uint32_t o = 0x20; o < 0x60; o += 4) std::snprintf(t, sizeof(t), " %08X", leer(objeto + o)), w += t;
        const uint32_t datos = leer(objeto + 0x2C);
        if (datos && (datos >> 28) == (objeto >> 28)) {
          w += " | datos";
          for (uint32_t o = 0; o < 0x40; o += 4) std::snprintf(t, sizeof(t), " %08X", leer(datos + o)), w += t;
        }
        REXLOG_INFO("[menu raton] barra {:08X}: {}", hash, w);
      }
    }
  }
  if ((encima && (a <= 10 || a % 200 == 0)) || n % 2000 == 0) {
    REXLOG_INFO("[menu raton] toca objeto {:08X} en {:.0f},{:.0f}: {} ({} aciertos de {} pruebas)", objeto, x, y,
                encima ? "SI" : "no", a, n);
  }
}

// The object's screen rectangle for the hit test (sub_82281C20, r4 = {x0, y0, x1, y1} as floats): logged for a few
// objects to learn the coordinate space the menus use.
REX_EXTERN(__imp__sub_82281C20);
REX_HOOK_RAW(sub_82281C20) {
  const uint32_t objeto = ctx.r3.u32, rect = ctx.r4.u32;
  __imp__sub_82281C20(ctx, base);
  if (objeto && rect) {
    float r[4];
    for (int i = 0; i < 4; ++i) {
      uint32_t v;
      std::memcpy(&v, base + rect + i * 4, 4);
      v = __builtin_bswap32(v);
      std::memcpy(&r[i], &v, 4);
    }
    g_centros[objeto] = {(r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f};
    g_menu_ms.store(AhoraMs(), std::memory_order_relaxed);
    uint32_t h;
    std::memcpy(&h, base + objeto + 0x10, 4);
    h = __builtin_bswap32(h);
    uint32_t fl;
    std::memcpy(&fl, base + objeto + 0x1C, 4);
    if ((__builtin_bswap32(fl) & 0xF) == 9) g_barra[objeto] = {{r[0], r[1], r[2], r[3]}, AhoraMs()};
    if (h == 0xD1CDA594u || h == 0x9345E4A7u) g_opciones_ms.store(AhoraMs(), std::memory_order_relaxed);
    if (h == 0xFCD5B255u || h == 0x3DF39A82u) {
      g_flechas_carrusel[h == 0x3DF39A82u ? 0 : 1] = {
          {std::min(r[0], r[2]), std::min(r[1], r[3]), std::max(r[0], r[2]), std::max(r[1], r[3])}, AhoraMs()};
    }
    if (h == 0xFCD5B255u || h == 0x3DF39A82u) g_carrusel_ms.store(AhoraMs(), std::memory_order_relaxed);
    if (EsFlecha(h)) {
      if (!g_flechas.count(objeto)) {
        REXLOG_INFO("[menu raton] flecha {:08X}: {:.0f},{:.0f} - {:.0f},{:.0f}", h, r[0], r[1], r[2], r[3]);
      }
      g_flechas[objeto] = {{std::min(r[0], r[2]), std::min(r[1], r[3]), std::max(r[0], r[2]), std::max(r[1], r[3])},
                           AhoraMs()};
    }
  }
  static std::atomic<uint32_t> vistos{0};
  const uint32_t n = vistos.fetch_add(1, std::memory_order_relaxed) + 1;
  if (rect && (n <= 40 || n % 3000 == 0)) {
    float r[4];
    for (int i = 0; i < 4; ++i) {
      uint32_t v;
      std::memcpy(&v, base + rect + i * 4, 4);
      v = __builtin_bswap32(v);
      std::memcpy(&r[i], &v, 4);
    }
    REXLOG_INFO("[menu raton] rectangulo del objeto {:08X}: {:.1f},{:.1f} - {:.1f},{:.1f}", objeto, r[0], r[1], r[2],
                r[3]);
  }
}

// Game screen message handlers that compare the message with ABC08912 and 0C407210, the only two answers of the
// challenge grid's cells: logged with the object, to learn which one marks the selected cell.
namespace {
void NotarMensaje(const char* quien, uint8_t* base, uint32_t mensaje, uint32_t objeto) {
  if (mensaje != 0xABC08912u && mensaje != 0x0C407210u) return;
  uint32_t hash = 0;
  if (objeto && objeto < 0xE0000000u) {
    std::memcpy(&hash, base + objeto + 0x10, 4);
    hash = __builtin_bswap32(hash);
  }
  static std::atomic<uint32_t> n{0};
  if (n.fetch_add(1, std::memory_order_relaxed) < 400) {
    REXLOG_INFO("[menu raton] {}: mensaje {:08X} al objeto {:08X} (hash {:08X})", quien, mensaje, objeto, hash);
  }
}
}  // namespace

REX_EXTERN(__imp__sub_82586960);
REX_HOOK_RAW(sub_82586960) {
  NotarMensaje("pantalla 82586960", base, ctx.r4.u32, ctx.r5.u32);
  __imp__sub_82586960(ctx, base);
}

REX_EXTERN(__imp__sub_8225EB80);
REX_HOOK_RAW(sub_8225EB80) {
  NotarMensaje("pantalla 8225EB80", base, ctx.r4.u32, ctx.r5.u32);
  __imp__sub_8225EB80(ctx, base);
}

// FEngSetCurrentButton(package name, button hash) (sub_82282B28), what the game screens call to choose a button:
// logged with its caller, to find the challenge grid's own selection code.
REX_EXTERN(__imp__sub_82282B28);
REX_HOOK_RAW(sub_82282B28) {
  if (REXCVAR_GET(nfsmw_menu_raton)) {
    REXLOG_INFO("[menu raton] FEngSetCurrentButton hash {:08X} desde {:08X}", ctx.r4.u32, uint32_t(ctx.lr));
    g_boton_hash = ctx.r4.u32;
  }
  __imp__sub_82282B28(ctx, base);
}

// Adding an option (sub_822DE838(list, option)): a list that may be on screen.
REX_EXTERN(__imp__sub_822DE838);
REX_HOOK_RAW(sub_822DE838) {
  if (REXCVAR_GET(nfsmw_menu_raton)) RecordarLista(ctx.r3.u32);
  __imp__sub_822DE838(ctx, base);
}

// The graphics options' list sets its current row with sub_822E31F8(list, row): it unhighlights the old row,
// highlights the new one and refreshes the list.
REX_EXTERN(__imp__sub_822E31F8);
REX_HOOK_RAW(sub_822E31F8) {
  if (REXCVAR_GET(nfsmw_menu_raton) && ctx.r3.u32) {
    g_lista_filas = ctx.r3.u32;
    std::memcpy(&g_lista_filas_vtable, base + ctx.r3.u32, 4);
  }
  __imp__sub_822E31F8(ctx, base);
}

REX_HOOK_RAW(sub_822DECA8) {
  if (REXCVAR_GET(nfsmw_menu_raton)) RecordarLista(ctx.r3.u32);
  __imp__sub_822DECA8(ctx, base);
}

REX_HOOK_RAW(sub_822E0740) {
  if (REXCVAR_GET(nfsmw_menu_raton)) {
    g_scroller = ctx.r3.u32;
    REXLOG_INFO("[menu raton] scroller {:08X}: elegido {:08X} arriba {} desde {:08X}", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, uint32_t(ctx.lr));
  }
  __imp__sub_822E0740(ctx, base);
}

REXCVAR_DECLARE(std::string, nfsmw_limite_fps);
REXCVAR_DECLARE(std::string, nfsmw_tratamiento_visual);
REXCVAR_DECLARE(std::string, nfsmw_resolucion_interna);
REXCVAR_DECLARE(std::string, nfsmw_posproceso);
REXCVAR_DECLARE(std::string, nfsmw_msaa);
REXCVAR_DECLARE(std::string, nfsmw_antialiasing);
REXCVAR_DECLARE(std::string, nfsmw_rcas);

namespace nfsmw::nativo {
void FijarHzVblank(double hz);
}

// Experimental rows in the in-game graphics options. sub_8223D760 builds that screen with two slider rows (time of
// day and brightness); toggle rows (the class the other options screens use for on/off settings, vtable 82079D38:
// +4 change, +12 refresh; label text at +44, value text at +48) are added after them, and those two methods are
// redirected when called on one of these rows, so the setting that class belongs to is never touched. Each row is a
// short list of values stepped with left/right.
REX_EXTERN(__imp__sub_8223D760);
REX_EXTERN(sub_8215CB08);
REX_EXTERN(sub_822E46C0);
REX_EXTERN(sub_822E23D8);
REX_EXTERN(sub_822827E8);
REX_EXTERN(sub_822824E0);
REX_EXTERN(__imp__sub_8223A3B0);
REX_EXTERN(__imp__sub_8223A448);

namespace {
struct FilaExtra {
  const char* nombre;
  std::vector<const char*> valores;  // nullptr: the game's own "on"/"off" strings by hash
  std::vector<uint32_t> hashes;
  int indice;
  void (*aplicar)(int indice);
  uint32_t fila = 0, texto = 0;  // the row and a guest buffer for its strings
  // Rows that only set a cvar: its name, the cvar value of each row value, whether it applies on restart and the
  // value the game started with.
  const char* cvar = nullptr;
  std::vector<const char*> cvar_valores = {};
  bool reinicio = false;
  int arranque = -1;
};

constexpr double kHzFps[] = {30.0, 60.0, 120.0, 1000.0};

// Saves a setting as the launcher does (the SDK writes changed cvars to nfsmw.toml); the override lets the ones
// that only apply on restart take the new value too.
void Guardar(const char* nombre, const char* valor) {
  {
    rex::cvar::testing::ScopedLifecycleOverride permitir;
    rex::cvar::SetFlagByName(nombre, valor);
  }
  rex::cvar::SaveConfig(rex::filesystem::GetExecutableFolder() / "nfsmw.toml");
}

constexpr const char* kResoluciones[] = {"automatico", "640x360",   "640x480",   "832x468",   "960x540",  "1024x576",
                                         "1280x720",   "1600x900",  "1920x1080", "2560x1440", "3840x2160", "1600x720", "2400x1080", "2560x1080", "3456x1440"};
bool g_reinicio_pendiente = false;
// The extra row objects on screen (as many as slots were left) and which setting the first one shows.
std::vector<uint32_t> g_ranuras;
int g_desplazamiento = 0;
uint32_t g_pantalla_graficos = 0;  // the graphics options screen while its rows are ours
std::string g_resolucion_arranque;
constexpr const char* kMsaa[] = {"apagado", "2x", "4x", "8x"};
constexpr const char* kNitidez[] = {"apagado", "bajo", "medio", "alto"};
std::string g_msaa_arranque;
void ComprobarReinicio();
constexpr const char* kFiltros[] = {"apagado", "cine", "sepia", "noir", "frio", "calido", "vivo", "matrix", "crt"};  // the one the game started with: another one applies on restart
// Positions of the rows the code refers to by name.
enum : int {
  kFilaFps = 0,
  kFilaMostrarFps,
  kFilaAmarillo,
  kFilaResolucion,
  kFilaAspecto,
  kFilaFiltro,
  kFilaMsaa,
  kFilaFxaa,
  kFilaFsr,
};
FilaExtra g_filas_extra[] = {
    {"FPS", {"30", "60", "120", "Ilimitado"}, {}, 1,
     [](int i) {
       nfsmw::nativo::FijarHzVblank(kHzFps[i]);
       Guardar("nfsmw_limite_fps", std::array{"30", "60", "120", "ilimitado"}[i]);
     }},
    {.nombre = "Mostrar FPS", .valores = {"desactivado", "activado"}, .hashes = {0x70DFE5C2u, 0x417B2604u},
     .indice = 0, .aplicar = nullptr, .cvar = "nfsmw_mostrar_fps", .cvar_valores = {"false", "true"}},
    // The game's own off/on strings (the toggle class shows them the same way).
    {"Filtro amarillo", {"desactivado", "activado"}, {0x70DFE5C2u, 0x417B2604u}, 1,
     [](int i) { Guardar("nfsmw_tratamiento_visual", i ? "original" : "apagado"); }},
    {"Resoluci\xF3n",
     {"Autom\xE1tica","640x360", "640x480", "832x468", "960x540", "1024x576", "1280x720", "1600x900", "1920x1080",
      "2560x1440", "3840x2160", "1600x720", "2400x1080", "2560x1080", "3456x1440"},
     {}, 0,
     [](int i) {
       Guardar("nfsmw_resolucion_interna", kResoluciones[i]);
       // A restart is pending: the notice comes when the player accepts or leaves the screen.
       ComprobarReinicio();
     }},
    {.nombre = "Relaci\xF3n de aspecto", .valores = {"Autom\xE1tica", "16:9", "4:3", "19,5:9", "21:9"}, .indice = 0,
     .aplicar = nullptr, .cvar = "nfsmw_relacion_aspecto",
     .cvar_valores = {"automatico", "16:9", "4:3", "19.5:9", "21:9"}, .reinicio = true},
    // The post-processing presets (the launcher's "personalizado" values stay for the launcher).
    {"Filtro",
     {"Ninguno", "Cine", "Sepia", "Noir", "Fr\xEDo", "C\xE1lido", "Vivo", "Matrix", "CRT"},
     {}, 0, [](int i) { Guardar("nfsmw_posproceso", kFiltros[i]); }},
    // MSAA only applies on restart, like the resolution.
    {"MSAA", {"No", "2x", "4x", "8x"}, {}, 0,
     [](int i) {
       Guardar("nfsmw_msaa", kMsaa[i]);
       ComprobarReinicio();
     }},
    {"FXAA", {"desactivado", "activado"}, {0x70DFE5C2u, 0x417B2604u}, 0,
     [](int i) { Guardar("nfsmw_antialiasing", i ? "fxaa" : "apagado"); }},
    {"Nitidez FSR", {"No", "Baja", "Media", "Alta"}, {}, 0,
     [](int i) { Guardar("nfsmw_rcas", kNitidez[i]); }},
    // The launcher's detail settings.
    {.nombre = "Distancia de sombras", .valores = {"Corta", "Media", "Normal"}, .indice = 2, .aplicar = nullptr,
     .cvar = "nfsmw_sombras_corte", .cvar_valores = {"250", "150", "100"}},
    {.nombre = "Resoluci\xF3n de sombras", .valores = {"Baja", "Media", "Alta"}, .indice = 2, .aplicar = nullptr,
     .cvar = "nfsmw_nativo_sombras_escala", .cvar_valores = {"50", "64", "100"}, .reinicio = true},
    {.nombre = "Sombras de vegetaci\xF3n", .valores = {"desactivado", "activado"},
     .hashes = {0x70DFE5C2u, 0x417B2604u}, .indice = 1, .aplicar = nullptr,
     .cvar = "nfsmw_sombras_sin_vegetacion", .cvar_valores = {"true", "false"}},
    {.nombre = "Reflejos de la carretera", .valores = {"Cada 8", "Cada 4", "Cada 2", "Siempre"}, .indice = 1,
     .aplicar = nullptr, .cvar = "nfsmw_reflejo_refresco", .cvar_valores = {"8", "4", "2", "1"}, .reinicio = true},
    {.nombre = "Reflejos del coche", .valores = {"Bajo", "Medio", "Completo"}, .indice = 2, .aplicar = nullptr,
     .cvar = "nfsmw_cubemap_caras_max", .cvar_valores = {"1", "2", "-1"}, .reinicio = true},
    {.nombre = "Distancia de dibujado", .valores = {"Baja", "Reducida", "Normal"}, .indice = 2, .aplicar = nullptr,
     .cvar = "nfsmw_escena_detalle_minimo", .cvar_valores = {"8", "6", "0"}},
};

uint32_t LeerG(uint8_t* base, uint32_t d) {
  uint32_t v;
  std::memcpy(&v, base + d, 4);
  return __builtin_bswap32(v);
}

FilaExtra* FilaDe(uint32_t fila) {
  if (!fila) return nullptr;
  for (FilaExtra& f : g_filas_extra)
    if (f.fila == fila) return &f;
  return nullptr;
}

void Imprimir(PPCContext& ctx, uint8_t* base, uint32_t objeto, uint32_t buffer, const std::string& texto) {
  if (!objeto || !buffer) return;
  std::memcpy(base + buffer, texto.c_str(), texto.size() + 1);
  ctx.r3.u64 = objeto;
  ctx.r4.u64 = buffer;
  sub_822827E8(ctx, base);  // FEPrintf
}

// The row's name on its label and its value on its value text.
void Pintar(PPCContext& ctx, uint8_t* base, FilaExtra& f) {
  if (!f.fila || !f.texto) return;
  const PPCContext guardado = ctx;
  Imprimir(ctx, base, LeerG(base, f.fila + 44), f.texto, f.nombre);
  const uint32_t valor = LeerG(base, f.fila + 48);
  if (!f.hashes.empty()) {
    ctx.r3.u64 = valor;
    ctx.r4.u64 = f.hashes[f.indice];
    if (valor) sub_822824E0(ctx, base);  // the localized string by hash
  } else {
    std::string v = f.valores[f.indice];
    if ((&f == &g_filas_extra[kFilaResolucion] && kResoluciones[f.indice] != g_resolucion_arranque) ||
        (&f == &g_filas_extra[kFilaMsaa] && kMsaa[f.indice] != g_msaa_arranque) || (f.reinicio && f.indice != f.arranque))
      v += " (al reiniciar)";
    Imprimir(ctx, base, valor, f.texto + 64, v);
  }
  ctx = guardado;
}
}  // namespace

// A restart is pending when the resolution or the MSAA chosen differs from what the game started with.
namespace {
void ComprobarReinicio() {
  g_reinicio_pendiente = kResoluciones[g_filas_extra[kFilaResolucion].indice] != g_resolucion_arranque ||
                         kMsaa[g_filas_extra[kFilaMsaa].indice] != g_msaa_arranque;
  for (const FilaExtra& f : g_filas_extra) g_reinicio_pendiente |= f.reinicio && f.indice != f.arranque;
}
}  // namespace

// Whether the options list's current row is one of the extra rows (the game passes them the arrows' clicks).
bool EsFilaFpsActual(uint8_t* base) {
  return g_lista_filas && FilaDe(LeerG(base, g_lista_filas + 52)) != nullptr;
}

REX_HOOK_RAW(sub_8223D760) {
  const uint32_t pantalla = ctx.r3.u32;
  __imp__sub_8223D760(ctx, base);
  for (FilaExtra& f : g_filas_extra) f.fila = 0;
  g_pantalla_graficos = 0;
  g_ranuras.clear();
  g_desplazamiento = 0;
  if (!pantalla || !base[pantalla + 288]) return;  // the rows do not need the menu mouse
  static bool leidos = false;
  if (!leidos) {
    leidos = true;
    const std::string limite = REXCVAR_GET(nfsmw_limite_fps);
    for (size_t i = 0; i < g_filas_extra[kFilaFps].valores.size(); ++i)
      if (limite == g_filas_extra[kFilaFps].valores[i]) g_filas_extra[kFilaFps].indice = int(i);
    g_filas_extra[kFilaAmarillo].indice = std::string(REXCVAR_GET(nfsmw_tratamiento_visual)) == "apagado" ? 0 : 1;
    g_resolucion_arranque = nfsmw::ajustes::ResolucionEnUso();
    for (int i = 0; i < int(std::size(kResoluciones)); ++i)
      if (g_resolucion_arranque == kResoluciones[i]) g_filas_extra[kFilaResolucion].indice = i;
    g_msaa_arranque = REXCVAR_GET(nfsmw_msaa);
    for (int i = 0; i < 4; ++i) {
      if (g_msaa_arranque == kMsaa[i]) g_filas_extra[kFilaMsaa].indice = i;
      if (std::string(REXCVAR_GET(nfsmw_rcas)) == kNitidez[i]) g_filas_extra[kFilaFsr].indice = i;
    }
    g_filas_extra[kFilaFxaa].indice = std::string(REXCVAR_GET(nfsmw_antialiasing)) == "fxaa" ? 1 : 0;
    for (FilaExtra& f : g_filas_extra) {
      if (!f.cvar) continue;
      const std::string actual = rex::cvar::GetFlagByName(f.cvar);
      for (size_t i = 0; i < f.cvar_valores.size(); ++i)
        if (actual == f.cvar_valores[i]) f.indice = int(i);
      f.arranque = f.indice;
    }
    const std::string filtro = REXCVAR_GET(nfsmw_posproceso);
    for (int i = 0; i < int(std::size(kFiltros)); ++i)
      if (filtro == kFiltros[i]) g_filas_extra[kFilaFiltro].indice = i;
  }
  g_pantalla_graficos = pantalla;
  const PPCContext guardado = ctx;
  for (FilaExtra& f : g_filas_extra) {
    if (!f.texto) {
      ctx.r3.u64 = 128;
      ctx.r4.u64 = ctx.r5.u64 = ctx.r6.u64 = 0;
      sub_8215CB08(ctx, base);
      f.texto = ctx.r3.u32;
    }
    // The screen's package has 9 row slots (B2980BA8 to B2980BB0, the counter starts at 1): the settings that do
    // not fit are reached by scrolling the extra rows (g_desplazamiento).
    if (LeerG(base, pantalla + 256) >= 10) continue;
    ctx.r3.u64 = 92;
    ctx.r4.u64 = ctx.r5.u64 = ctx.r6.u64 = 0;
    sub_8215CB08(ctx, base);
    const uint32_t fila = ctx.r3.u32;
    if (!fila || !f.texto) continue;
    ctx.r3.u64 = fila;
    ctx.r4.u64 = 1;
    sub_822E46C0(ctx, base);
    const uint32_t vt = __builtin_bswap32(0x82079D38u);
    std::memcpy(base + fila, &vt, 4);
    f.fila = fila;  // before adding: the add calls its refresh
    g_ranuras.push_back(fila);
    ctx.r3.u64 = pantalla;
    ctx.r4.u64 = fila;
    ctx.r5.u64 = 1;
    sub_822E23D8(ctx, base);
    REXLOG_INFO("[ajustes juego] fila '{}' anadida: {}", f.nombre, f.valores[f.indice]);
  }
  ctx = guardado;
}

// Refresh (+12): our name and value instead of that setting's.
REX_HOOK_RAW(sub_8223A448) {
  if (FilaExtra* f = FilaDe(ctx.r3.u32)) {
    Pintar(ctx, base, *f);
    return;
  }
  __imp__sub_8223A448(ctx, base);
}

// Change (+4): r5 is the pad message, 9120409E left and B5971BF1 right; one step, applied at once.
REX_HOOK_RAW(sub_8223A3B0) {
  if (FilaExtra* f = FilaDe(ctx.r3.u32)) {
    const int paso = ctx.r5.u32 == 0x9120409Eu ? -1 : ctx.r5.u32 == 0xB5971BF1u ? 1 : 0;
    const int nuevo = std::clamp(f->indice + paso, 0, int(f->valores.size()) - 1);
    if (nuevo != f->indice) {
      f->indice = nuevo;
      if (f->aplicar) f->aplicar(nuevo);
      if (f->cvar) {
        Guardar(f->cvar, f->cvar_valores[nuevo]);
        ComprobarReinicio();
      }
      REXLOG_INFO("[ajustes juego] {}: {}", f->nombre, f->valores[nuevo]);
    }
    Pintar(ctx, base, *f);
    return;
  }
  __imp__sub_8223A3B0(ctx, base);
}

// The restart notice: the game's own two-button dialog, as the options screens show it for "defaults"
// (sub_822BFB08(package, dialog package, 1, yes text, no text, yes message, no message, no message; stack +84 = 1,
// +92 = the body text)), with our text. Its answer comes back to the options screen's message handler.
REX_EXTERN(sub_822BFB08);
constexpr uint32_t kMsgReiniciarSi = 0x4E46534Du, kMsgReiniciarNo = 0x4E46534Eu;
uint32_t g_texto_aviso = 0;

void MostrarAvisoReinicio(PPCContext& ctx, uint8_t* base) {
  const uint32_t pantalla = g_pantalla_graficos;
  if (!pantalla) return;
  const PPCContext guardado = ctx;
  if (!g_texto_aviso) {
    ctx.r3.u64 = 160;
    ctx.r4.u64 = ctx.r5.u64 = ctx.r6.u64 = 0;
    sub_8215CB08(ctx, base);
    g_texto_aviso = ctx.r3.u32;
    const char* texto = "Para aplicar estos cambios necesitas reiniciar el juego. \xBFReiniciar ahora?";
    if (g_texto_aviso) std::memcpy(base + g_texto_aviso, texto, std::strlen(texto) + 1);
  }
  if (g_texto_aviso) {
    auto escribir = [&](uint32_t d, uint32_t v) {
      v = __builtin_bswap32(v);
      std::memcpy(base + d, &v, 4);
    };
    const uint32_t pila = ctx.r1.u32 - 256;  // our own frame for the stack arguments
    escribir(pila, ctx.r1.u32);
    escribir(pila + 84, 1);
    escribir(pila + 92, g_texto_aviso);
    ctx.r1.u64 = pila;
    ctx.r3.u64 = LeerG(base, pantalla + 16);
    ctx.r4.u64 = base[pantalla + 288] ? 0x8206E51Cu : 0x820760D8u;
    ctx.r5.u64 = 1;
    ctx.r6.u64 = 0x70E01038u;
    ctx.r7.u64 = 0x417B25E4u;
    ctx.r8.u64 = kMsgReiniciarSi;
    ctx.r9.u64 = kMsgReiniciarNo;
    ctx.r10.u64 = kMsgReiniciarNo;
    sub_822BFB08(ctx, base);
    REXLOG_INFO("[ajustes juego] aviso de reinicio mostrado");
  }
  ctx = guardado;
}

#if defined(_WIN32)
#include <windows.h>
#endif

// Starts the game again with the same command line (the settings are already saved) and closes this one.
void ReiniciarJuego() {
  rex::cvar::SaveConfig(rex::filesystem::GetExecutableFolder() / "nfsmw.toml");
#if defined(_WIN32)
  wchar_t ruta[MAX_PATH];
  GetModuleFileNameW(nullptr, ruta, MAX_PATH);
  std::wstring linea = GetCommandLineW();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(ruta, linea.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    REXLOG_INFO("[ajustes juego] reiniciando el juego");
    TerminateProcess(GetCurrentProcess(), 0);
  }
  REXLOG_WARN("[ajustes juego] no se pudo reiniciar el juego");
#elif REX_PLATFORM_ANDROID
  // NfsmwActivity.restartApp opens the game again in a new process, straight into the game, and ends this one.
  REXLOG_INFO("[ajustes juego] reiniciando el juego (Android)");
  if (!nfsmw::sistema::Relanzar("--nfsmw_lanzador_omitir=true")) {
    REXLOG_WARN("[ajustes juego] no se pudo reiniciar: se cierra y se aplica al abrirlo");
    std::_Exit(0);
  }
#endif
}

// The options screen's message handler (sub_8223CD70(screen, message)). With a restart pending, accepting (406415E3)
// or leaving (911AB364) the graphics screen shows the notice first; its answer comes back here: yes restarts, no
// carries on with the accept or the exit that was held.
REX_EXTERN(__imp__sub_8223CD70);
uint32_t g_mensaje_retenido = 0;
REX_HOOK_RAW(sub_8223CD70) {
  const uint32_t m = ctx.r4.u32;
  if (m == kMsgReiniciarSi || m == kMsgReiniciarNo) {
    REXLOG_INFO("[ajustes juego] aviso de reinicio: {}", m == kMsgReiniciarSi ? "si" : "no");
  }
  if (m == kMsgReiniciarSi) {
    ReiniciarJuego();
    return;
  }
  if (m == kMsgReiniciarNo) {
    g_reinicio_pendiente = false;
    const uint32_t retenido = std::exchange(g_mensaje_retenido, 0u);
    if (retenido) {
      ctx.r4.u64 = retenido;
      __imp__sub_8223CD70(ctx, base);
    }
    return;
  }
  // Down (911C0A4B) on the last extra row or up (72619778) on the first one, with settings beyond: the extra rows
  // shift by one instead of the selection moving.
  if (g_pantalla_graficos && ctx.r3.u32 == g_pantalla_graficos && !g_ranuras.empty() && g_lista_filas &&
      (m == 0x911C0A4Bu || m == 0x72619778u)) {
    const uint32_t actual = LeerG(base, g_lista_filas + 52);
    const int total = int(std::size(g_filas_extra)), visibles = int(g_ranuras.size());
    int nuevo = g_desplazamiento;
    if (m == 0x911C0A4Bu && actual == g_ranuras.back() && g_desplazamiento + visibles < total) ++nuevo;
    if (m == 0x72619778u && actual == g_ranuras.front() && g_desplazamiento > 0) --nuevo;
    if (nuevo != g_desplazamiento) {
      g_desplazamiento = nuevo;
      for (FilaExtra& f : g_filas_extra) f.fila = 0;
      for (int k = 0; k < visibles; ++k) g_filas_extra[nuevo + k].fila = g_ranuras[k];
      const PPCContext guardado = ctx;
      for (int k = 0; k < visibles; ++k) Pintar(ctx, base, g_filas_extra[nuevo + k]);
      ctx = guardado;
      return;
    }
  }
  if (g_reinicio_pendiente && ctx.r3.u32 == g_pantalla_graficos && (m == 0xE1FDE1D1u || m == 0x406415E3u || m == 0x911AB364u)) {
    g_mensaje_retenido = m;
    MostrarAvisoReinicio(ctx, base);
    return;
  }
  if (g_pantalla_graficos && ctx.r3.u32 == g_pantalla_graficos && m != 0x9120409Eu && m != 0xB5971BF1u && m != 0xC98356BAu) {
    REXLOG_INFO("[ajustes juego] mensaje de la pantalla de graficos {:08X}", m);
  }
  __imp__sub_8223CD70(ctx, base);
}
