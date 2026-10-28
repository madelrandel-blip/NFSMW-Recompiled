// nfsmw - on-screen touch controls (see nfsmw_tactil.h).

#include "nfsmw_tactil.h"

#include "renderer/bc_decode.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#if defined(__ANDROID__)
#define NFSMW_TACTIL_POR_DEFECTO true
#else
#define NFSMW_TACTIL_POR_DEFECTO false
#endif
REXCVAR_DEFINE_BOOL(nfsmw_tactil, NFSMW_TACTIL_POR_DEFECTO, "NFSMW",
                    "On-screen touch controls over the game (hidden while a real pad is connected)");
REXCVAR_DEFINE_DOUBLE(nfsmw_tactil_opacidad, 0.45, "NFSMW", "Opacity of the touch controls, 0.1 to 1");
REXCVAR_DEFINE_DOUBLE(nfsmw_tactil_escala, 1.0, "NFSMW", "Size of the touch controls, 0.6 to 1.6 (the nut button changes it)");
REXCVAR_DEFINE_STRING(nfsmw_tactil_disposicion, "", "NFSMW",
                      "Touch controls moved or resized with the nut: NAME=x,y,size;... (x, y: center as a fraction of "
                      "the screen, -1 = default place; size: times the overall size)");

namespace nfsmw::tactil {
namespace {

namespace in = rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

constexpr in::DeviceId kMandoTactil = in::DeviceId(0x4E46534D57544354ull);  // "NFSMWTCT"

// The controls. Each one is anchored to a corner or edge of the screen and placed at a distance from it in screen
// heights (times the size setting), so the layout keeps its shape on any aspect ratio and grows away from the
// edges. The icons are the game's own DDS images (botones/<name>.dds, see CargarIconos); without them the label
// is drawn instead.
enum class Lado { kIzquierda, kDerecha, kCentro };
enum class Borde { kArriba, kAbajo };

enum class Accion { kBotones, kEje, kAjustes };

struct Control {
  const char* etiqueta;
  const char* icono;  // file name without extension, or nullptr
  Accion accion;
  uint32_t bits;      // X_INPUT_GAMEPAD bits; the triggers in bits 16 (LT) and 17 (RT)
  int giro;           // kEje: -1 or +1, the whole way along that axis (left/down or right/up)
  bool espejo;        // the icon flipped horizontally
  Lado lado;
  Borde borde;
  float x, y;         // center: distance from that side (kCentro: offset from the middle) and that edge
  float radio;
  int eje = 0;        // kEje: 0 left stick X, 1 left stick Y, 2 right stick X, 3 right stick Y
};

constexpr uint32_t kLT = 1u << 16;
constexpr uint32_t kRT = 1u << 17;

constexpr Control kControles[] = {
    // Steering: left and right only (the stick's horizontal axis).
    {"<", "XB360MW_LANALOG_LEFT", Accion::kEje, 0, -1, false, Lado::kIzquierda, Borde::kAbajo, 0.17f, 0.24f, 0.115f},
    {">", "XB360MW_LANALOG_RIGHT", Accion::kEje, 0, +1, false, Lado::kIzquierda, Borde::kAbajo, 0.45f, 0.24f, 0.115f},
    // The left stick's up and down (menus, the map), around the steering arrows.
    {"L^", "XB360MW_LANALOG_UP", Accion::kEje, 0, +1, false, Lado::kIzquierda, Borde::kAbajo, 0.31f, 0.43f, 0.06f, 1},
    {"Lv", "XB360MW_LANALOG_DOWN", Accion::kEje, 0, -1, false, Lado::kIzquierda, Borde::kAbajo, 0.31f, 0.07f, 0.06f, 1},
    // D-pad, on the left half way up.
    {"D^", "XB360MW_DPAD_UP", Accion::kBotones, 0x0001, 0, false, Lado::kIzquierda, Borde::kArriba, 0.20f, 0.31f, 0.045f},
    {"Dv", "XB360MW_DPAD_DOWN", Accion::kBotones, 0x0002, 0, false, Lado::kIzquierda, Borde::kArriba, 0.20f, 0.49f, 0.045f},
    {"D<", "XB360MW_DPAD_LEFT", Accion::kBotones, 0x0004, 0, false, Lado::kIzquierda, Borde::kArriba, 0.11f, 0.40f, 0.045f},
    {"D>", "XB360MW_DPAD_RIGHT", Accion::kBotones, 0x0008, 0, false, Lado::kIzquierda, Borde::kArriba, 0.29f, 0.40f, 0.045f},
    // Right stick (camera), to the left of the face buttons.
    {"R^", "XB360MW_RANALOG_UP", Accion::kEje, 0, +1, false, Lado::kDerecha, Borde::kAbajo, 0.52f, 0.59f, 0.045f, 3},
    {"Rv", "XB360MW_RANALOG_DOWN", Accion::kEje, 0, -1, false, Lado::kDerecha, Borde::kAbajo, 0.52f, 0.41f, 0.045f, 3},
    {"R<", "XB360MW_RANALOG_LEFT", Accion::kEje, 0, -1, false, Lado::kDerecha, Borde::kAbajo, 0.61f, 0.50f, 0.045f, 2},
    {"R>", "XB360MW_RANALOG_RIGHT", Accion::kEje, 0, +1, false, Lado::kDerecha, Borde::kAbajo, 0.43f, 0.50f, 0.045f, 2},
    // Gas, brake and the face buttons.
    {"RT", "XB360MW_RT", Accion::kBotones, kRT, 0, false, Lado::kDerecha, Borde::kAbajo, 0.16f, 0.22f, 0.125f},
    {"LT", "XB360MW_LT", Accion::kBotones, kLT, 0, false, Lado::kDerecha, Borde::kAbajo, 0.44f, 0.15f, 0.095f},
    {"A", "XB360MW_A", Accion::kBotones, 0x1000, 0, false, Lado::kDerecha, Borde::kAbajo, 0.21f, 0.50f, 0.068f},
    {"B", "XB360MW_B", Accion::kBotones, 0x2000, 0, false, Lado::kDerecha, Borde::kAbajo, 0.08f, 0.62f, 0.068f},
    {"X", "XB360MW_X", Accion::kBotones, 0x4000, 0, false, Lado::kDerecha, Borde::kAbajo, 0.34f, 0.62f, 0.068f},
    {"Y", "XB360MW_Y", Accion::kBotones, 0x8000, 0, false, Lado::kDerecha, Borde::kAbajo, 0.21f, 0.74f, 0.068f},
    // Shoulders in the top corners.
    {"LB", "XB360MW_LB", Accion::kBotones, 0x0100, 0, false, Lado::kIzquierda, Borde::kArriba, 0.13f, 0.14f, 0.075f},
    {"RB", "XB360MW_RB", Accion::kBotones, 0x0200, 0, false, Lado::kDerecha, Borde::kArriba, 0.13f, 0.14f, 0.075f},
    // Back (resets the car), Start and the settings nut, top center.
    {"BACK", "XB360MW_BACK", Accion::kBotones, 0x0020, 0, false, Lado::kCentro, Borde::kArriba, -0.15f, 0.09f, 0.058f},
    {"START", "XB360MW_START", Accion::kBotones, 0x0010, 0, false, Lado::kCentro, Borde::kArriba, 0.15f, 0.09f, 0.058f},
    {"AJUSTES", nullptr, Accion::kAjustes, 0, 0, false, Lado::kCentro, Borde::kArriba, 0.0f, 0.09f, 0.045f},
};
constexpr size_t kNumControles = sizeof(kControles) / sizeof(kControles[0]);

// Where the player put a control with the nut's panel, and its own size. One never moved keeps its anchor.
struct Ajuste {
  bool movido = false;
  float fx = 0.0f, fy = 0.0f;  // center as a fraction of the screen
  float tam = 1.0f;            // times the overall size
  bool oculto = false;         // hidden from the nut's panel: not drawn and takes no touches
};
using Disposicion = std::array<Ajuste, kNumControles>;

// nfsmw_tactil_disposicion: NAME=x,y,size[,hidden] entries (by label) separated by ';'.
Disposicion LeerDisposicion(const std::string& texto) {
  Disposicion d{};
  size_t inicio = 0;
  while (inicio < texto.size()) {
    size_t fin = texto.find(';', inicio);
    if (fin == std::string::npos) {
      fin = texto.size();
    }
    const std::string entrada = texto.substr(inicio, fin - inicio);
    inicio = fin + 1;
    const size_t igual = entrada.find('=');
    float x = 0.0f, y = 0.0f, tam = 1.0f;
    int oculto = 0;
    if (igual == std::string::npos ||
        std::sscanf(entrada.c_str() + igual + 1, "%f,%f,%f,%d", &x, &y, &tam, &oculto) < 3) {
      continue;
    }
    for (size_t i = 0; i < kNumControles; ++i) {
      if (entrada.compare(0, igual, kControles[i].etiqueta) == 0) {
        d[i].movido = x >= 0.0f && y >= 0.0f;
        d[i].fx = std::clamp(x, 0.0f, 1.0f);
        d[i].fy = std::clamp(y, 0.0f, 1.0f);
        d[i].tam = std::clamp(tam, 0.5f, 2.0f);
        d[i].oculto = oculto != 0 && kControles[i].accion != Accion::kAjustes;
      }
    }
  }
  return d;
}

std::string EscribirDisposicion(const Disposicion& d) {
  std::string texto;
  for (size_t i = 0; i < kNumControles; ++i) {
    if (!d[i].movido && d[i].tam == 1.0f && !d[i].oculto) {
      continue;
    }
    texto += fmt::format("{}{}={:.4f},{:.4f},{:.2f}{}", texto.empty() ? "" : ";", kControles[i].etiqueta,
                         d[i].movido ? d[i].fx : -1.0f, d[i].movido ? d[i].fy : -1.0f, d[i].tam,
                         d[i].oculto ? ",1" : "");
  }
  return texto;
}

// VK_PAD codes of XInputGetKeystroke for each button bit, for the menus that read keystrokes.
struct Tecla {
  uint32_t bit;
  uint16_t codigo;
};
constexpr Tecla kTeclas[] = {
    {0x1000, 0x5800}, {0x2000, 0x5801}, {0x4000, 0x5802}, {0x8000, 0x5803}, {0x0200, 0x5804}, {0x0100, 0x5805},
    {kLT, 0x5806},    {kRT, 0x5807},    {0x0001, 0x5810}, {0x0002, 0x5811}, {0x0004, 0x5812}, {0x0008, 0x5813},
    {0x0010, 0x5814}, {0x0020, 0x5815},
};

float Escala() { return std::clamp(float(REXCVAR_GET(nfsmw_tactil_escala)), 0.6f, 1.6f); }

ImVec2 Centro(size_t i, const Disposicion& d, float ancho, float alto, float escala) {
  if (d[i].movido) {
    return ImVec2(d[i].fx * ancho, d[i].fy * alto);
  }
  const Control& c = kControles[i];
  float x;
  switch (c.lado) {
    case Lado::kIzquierda: x = c.x * alto * escala; break;
    case Lado::kDerecha: x = ancho - c.x * alto * escala; break;
    default: x = ancho * 0.5f + c.x * alto * escala; break;
  }
  const float y = c.borde == Borde::kArriba ? c.y * alto * escala : alto - c.y * alto * escala;
  return ImVec2(x, y);
}

float Radio(size_t i, const Disposicion& d, float alto, float escala) {
  return kControles[i].radio * alto * escala * d[i].tam;
}

// What every finger is doing. Written from SDL's event watch (the Java UI thread on Android), read by the input
// system (the game's threads) and the overlay (the UI thread).
struct Estado {
  std::mutex mutex;
  float ancho = 0.0f, alto = 0.0f;  // screen in pixels, from the overlay
  float escala = 1.0f;              // size setting, from the overlay
  Disposicion disposicion{};        // moved and resized controls, from the overlay
  std::unordered_map<SDL_FingerID, int> dedos;  // finger -> index in kControles
  bool mando_real = false;  // a real pad is connected: everything off
  bool ajustes = false;     // the settings panel is open: the controls take no touches
  bool abrir_ajustes = false;  // the nut was touched (handled by the overlay)
};
Estado g_estado;

int ControlEn(float x, float y, float ancho, float alto, float escala, const Disposicion& disposicion) {
  int mejor = -1;
  float mejor_distancia = 0.0f;
  for (size_t i = 0; i < kNumControles; ++i) {
    if (disposicion[i].oculto) {
      continue;
    }
    const ImVec2 c = Centro(i, disposicion, ancho, alto, escala);
    const float d = std::hypot(x - c.x, y - c.y);
    // A little slack around each button for a thumb.
    if (d <= Radio(i, disposicion, alto, escala) * 1.25f && (mejor < 0 || d < mejor_distancia)) {
      mejor = int(i);
      mejor_distancia = d;
    }
  }
  return mejor;
}

bool SDLCALL VigilarDedos(void*, SDL_Event* evento) {
  if (evento->type != SDL_EVENT_FINGER_DOWN && evento->type != SDL_EVENT_FINGER_MOTION &&
      evento->type != SDL_EVENT_FINGER_UP && evento->type != SDL_EVENT_FINGER_CANCELED) {
    return true;
  }
  std::lock_guard<std::mutex> cerrojo(g_estado.mutex);
  const float ancho = g_estado.ancho, alto = g_estado.alto;
  if (ancho <= 0.0f || alto <= 0.0f || g_estado.mando_real) {
    return true;
  }
  const SDL_FingerID id = evento->tfinger.fingerID;
  const float x = evento->tfinger.x * ancho, y = evento->tfinger.y * alto;
  switch (evento->type) {
    case SDL_EVENT_FINGER_DOWN: {
      if (g_estado.ajustes) {
        break;  // the panel (ImGui) has the touches
      }
      const int control = ControlEn(x, y, ancho, alto, g_estado.escala, g_estado.disposicion);
      if (control < 0) {
        break;
      }
      if (kControles[control].accion == Accion::kAjustes) {
        g_estado.abrir_ajustes = true;
      } else {
        g_estado.dedos[id] = control;
      }
      break;
    }
    case SDL_EVENT_FINGER_MOTION: {
      // A finger that slides to the other arrow or to another button takes it over (steering by rocking the
      // thumb, sliding from brake to gas); one that slides off everything keeps what it had.
      auto it = g_estado.dedos.find(id);
      if (it != g_estado.dedos.end()) {
        const int control = ControlEn(x, y, ancho, alto, g_estado.escala, g_estado.disposicion);
        if (control >= 0 && kControles[control].accion != Accion::kAjustes) {
          it->second = control;
        }
      }
      break;
    }
    default:
      g_estado.dedos.erase(id);
      break;
  }
  return true;
}

// The pad as the fingers hold it now.
void Leer(uint32_t& bits, std::array<int16_t, 4>& ejes) {
  bits = 0;
  ejes = {};
  std::lock_guard<std::mutex> cerrojo(g_estado.mutex);
  if (g_estado.mando_real || g_estado.ajustes) {
    return;
  }
  std::array<int, 4> suma{};
  for (const auto& [id, control] : g_estado.dedos) {
    (void)id;
    const Control& c = kControles[control];
    if (c.accion == Accion::kEje) {
      suma[size_t(c.eje)] += c.giro;
    } else {
      bits |= c.bits;
    }
  }
  for (size_t e = 0; e < 4; ++e) {
    ejes[e] = int16_t(std::clamp(suma[e], -1, 1) * 32767);
  }
}

class MandoTactil final : public in::InputDriver {
 public:
  MandoTactil() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kMandoTactil;
    info.name = "Touch controls";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kMandoTactil) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    uint32_t bits;
    std::array<int16_t, 4> ejes;
    uint32_t paquete = Actualizar(bits, ejes);
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = paquete;
      out_state->gamepad.buttons = uint16_t(bits & 0xFFFF);
      out_state->gamepad.left_trigger = (bits & kLT) ? 0xFF : 0;
      out_state->gamepad.right_trigger = (bits & kRT) ? 0xFF : 0;
      out_state->gamepad.thumb_lx = ejes[0];
      out_state->gamepad.thumb_ly = ejes[1];
      out_state->gamepad.thumb_rx = ejes[2];
      out_state->gamepad.thumb_ry = ejes[3];
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags, in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kMandoTactil) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      // Like the SDK's empty pad (nop_input_driver.cpp) and the test script's pad.
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kMandoTactil ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags, in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (id != kMandoTactil) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    uint32_t bits;
    std::array<int16_t, 4> ejes;
    Actualizar(bits, ejes);
    std::lock_guard<std::mutex> cerrojo(mutex_);
    if (teclas_.empty()) {
      return X_ERROR_EMPTY;
    }
    if (out_keystroke) {
      *out_keystroke = teclas_.front();
    }
    teclas_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  // Reads the fingers and queues a keystroke for every button that changed (the menus that read keystrokes
  // instead of the state). Returns the packet number, which changes with the state.
  uint32_t Actualizar(uint32_t& bits, std::array<int16_t, 4>& ejes) {
    Leer(bits, ejes);
    const int16_t lx = ejes[0], ly = ejes[1], rx = ejes[2], ry = ejes[3];
    // The sticks as four keys each too, past half way: some dialogs only respond to them.
    uint32_t teclas = bits;
    if (ly > 16000) teclas |= 1u << 18;
    if (ly < -16000) teclas |= 1u << 19;
    if (lx > 16000) teclas |= 1u << 20;
    if (lx < -16000) teclas |= 1u << 21;
    if (ry > 16000) teclas |= 1u << 22;
    if (ry < -16000) teclas |= 1u << 23;
    if (rx > 16000) teclas |= 1u << 24;
    if (rx < -16000) teclas |= 1u << 25;
    std::lock_guard<std::mutex> cerrojo(mutex_);
    const uint32_t cambios = teclas ^ anteriores_;
    if (cambios) {
      for (const Tecla& t : kTeclas) {
        Encolar(cambios, teclas, t.bit, t.codigo);
      }
      Encolar(cambios, teclas, 1u << 18, 0x5820);  // VK_PAD_LTHUMB_UP
      Encolar(cambios, teclas, 1u << 19, 0x5821);  // VK_PAD_LTHUMB_DOWN
      Encolar(cambios, teclas, 1u << 20, 0x5822);  // VK_PAD_LTHUMB_RIGHT
      Encolar(cambios, teclas, 1u << 21, 0x5823);  // VK_PAD_LTHUMB_LEFT
      Encolar(cambios, teclas, 1u << 22, 0x5830);  // VK_PAD_RTHUMB_UP
      Encolar(cambios, teclas, 1u << 23, 0x5831);  // VK_PAD_RTHUMB_DOWN
      Encolar(cambios, teclas, 1u << 24, 0x5832);  // VK_PAD_RTHUMB_RIGHT
      Encolar(cambios, teclas, 1u << 25, 0x5833);  // VK_PAD_RTHUMB_LEFT
      anteriores_ = teclas;
    }
    if (bits != bits_ || ejes != ejes_) {
      bits_ = bits;
      ejes_ = ejes;
      ++paquete_;
    }
    return paquete_;
  }

  void Encolar(uint32_t cambios, uint32_t teclas, uint32_t bit, uint16_t codigo) {
    if (!(cambios & bit) || teclas_.size() >= 64) {
      return;
    }
    in::X_INPUT_KEYSTROKE tecla{};
    tecla.virtual_key = codigo;
    tecla.flags = uint16_t((teclas & bit) ? in::X_INPUT_KEYSTROKE_KEYDOWN : in::X_INPUT_KEYSTROKE_KEYUP);
    teclas_.push_back(tecla);
  }

  std::mutex mutex_;
  std::deque<in::X_INPUT_KEYSTROKE> teclas_;
  uint32_t anteriores_ = 0;
  uint32_t bits_ = 0;
  std::array<int16_t, 4> ejes_{};
  uint32_t paquete_ = 0;
};

// A DDS image of the game (DXT1/3/5 or uncompressed 32-bit BGRA, the formats of its UI textures) as RGBA8, or empty.
std::vector<uint8_t> LeerDds(const std::vector<uint8_t>& dds, int& ancho, int& alto) {
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0) {
    return {};
  }
  auto u32 = [&](size_t o) { return uint32_t(dds[o]) | uint32_t(dds[o + 1]) << 8 | uint32_t(dds[o + 2]) << 16 |
                                    uint32_t(dds[o + 3]) << 24; };
  alto = int(u32(12));
  ancho = int(u32(16));
  const uint32_t fourcc = u32(84);
  if ((u32(80) & 0x40) && u32(88) == 32 && ancho > 0 && alto > 0 && ancho <= 4096 && alto <= 4096 &&
      dds.size() >= 128 + size_t(ancho) * alto * 4) {
    std::vector<uint8_t> rgba(dds.begin() + 128, dds.begin() + 128 + size_t(ancho) * alto * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
      std::swap(rgba[i], rgba[i + 2]);
    }
    return rgba;
  }
  const bool dxt1 = fourcc == 0x31545844, dxt3 = fourcc == 0x33545844, dxt5 = fourcc == 0x35545844;
  if ((!dxt1 && !dxt3 && !dxt5) || ancho <= 0 || alto <= 0 || ancho > 4096 || alto > 4096) {
    return {};
  }
  const uint32_t bx = uint32_t(ancho + 3) / 4, by = uint32_t(alto + 3) / 4;
  const size_t bytes_bloque = dxt1 ? 8 : 16;
  if (dds.size() < 128 + size_t(bx) * by * bytes_bloque) {
    return {};
  }
  // Decoded on whole blocks, then cropped to the image.
  const size_t paso = size_t(bx) * 16;
  std::vector<uint8_t> bloques(paso * by * 4);
  const uint8_t* b = dds.data() + 128;
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x, b += bytes_bloque) {
      uint8_t* t = bloques.data() + size_t(y) * 4 * paso + size_t(x) * 16;
      if (dxt1) {
        renderer::bc::DecodeColorBlock(b, true, t, paso);
      } else {
        renderer::bc::DecodeColorBlock(b + 8, false, t, paso);
        if (dxt3) {
          renderer::bc::DecodeExplicitAlpha(b, t, paso);
        } else {
          renderer::bc::DecodeChannelBlock(b, t + 3, paso, 4);
        }
      }
    }
  }
  std::vector<uint8_t> rgba(size_t(ancho) * alto * 4);
  for (int y = 0; y < alto; ++y) {
    std::memcpy(rgba.data() + size_t(y) * ancho * 4, bloques.data() + size_t(y) * paso, size_t(ancho) * 4);
  }
  return rgba;
}

// The bytes of botones/<name>.dds: from the data folder if the player put them there, else from the APK's assets
// (SDL reads relative paths from them on Android; tools/android_build.sh packs them from outside the repository,
// since they are the game's art).
std::vector<uint8_t> LeerArchivoIcono(const char* nombre) {
  const std::string relativa = std::string("botones/") + nombre + ".dds";
  std::ifstream in(rex::filesystem::GetExecutableFolder() / relativa, std::ios::binary);
  if (in) {
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  size_t tam = 0;
  void* datos = SDL_LoadFile(relativa.c_str(), &tam);
  if (!datos) {
    return {};
  }
  std::vector<uint8_t> bytes(static_cast<uint8_t*>(datos), static_cast<uint8_t*>(datos) + tam);
  SDL_free(datos);
  return bytes;
}

// Draws the controls every frame on ImGui's foreground layer, with no window of its own, so it never takes the
// touches from the game. The nut opens a small panel to change their size and opacity.
class Controles final : public rex::ui::ImGuiDialog {
 public:
  explicit Controles(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  // Drawn with the game's frames: the presses show up at its frame rate. Only the panel needs the UI's own rate
  // (dragging and sliders).
  bool NeedsContinuousRepaint() const override { return panel_abierto_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float ancho = io.DisplaySize.x, alto = io.DisplaySize.y;
    if (!iconos_cargados_) {
      iconos_cargados_ = true;
      CargarIconos();
    }
    bool mando_real, abrir;
    std::array<bool, kNumControles> pulsados{};
    const float escala = Escala();
    Disposicion disposicion;
    {
      std::lock_guard<std::mutex> cerrojo(g_estado.mutex);
      if (!disposicion_leida_) {
        // Before the first width is published: until then the fingers are ignored.
        disposicion_leida_ = true;
        g_estado.disposicion = LeerDisposicion(REXCVAR_GET(nfsmw_tactil_disposicion));
      }
      disposicion = g_estado.disposicion;
      g_estado.ancho = ancho;
      g_estado.alto = alto;
      g_estado.escala = escala;
      g_estado.mando_real = SDL_HasGamepad();
      mando_real = g_estado.mando_real;
      abrir = std::exchange(g_estado.abrir_ajustes, false);
      if (abrir) {
        g_estado.ajustes = true;
        g_estado.dedos.clear();
      }
      for (const auto& [id, control] : g_estado.dedos) {
        (void)id;
        pulsados[control] = true;
      }
    }
    if (mando_real || alto <= 0.0f) {
      return;
    }
    if (abrir) {
      escala_panel_ = escala;
      opacidad_panel_ = float(REXCVAR_GET(nfsmw_tactil_opacidad));
      disposicion_panel_ = disposicion;
      seleccionado_ = -1;
      arrastrado_ = -1;
      panel_abierto_ = true;
    }
    const float opacidad = std::clamp(panel_abierto_ ? opacidad_panel_ : float(REXCVAR_GET(nfsmw_tactil_opacidad)),
                                      0.1f, 1.0f);
    const float escala_dibujo = panel_abierto_ ? escala_panel_ : escala;
    if (panel_abierto_) {
      Editar(io, ancho, alto, escala_dibujo);
    }
    const Disposicion& d = panel_abierto_ ? disposicion_panel_ : disposicion;
    const auto color = [opacidad](int gris, float alfa) {
      return IM_COL32(gris, gris, gris, int(255.0f * alfa * opacidad));
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* fuente = ImGui::GetFont();

    for (size_t i = 0; i < kNumControles; ++i) {
      const Control& c = kControles[i];
      if (d[i].oculto) {
        continue;
      }
      const ImVec2 centro = Centro(i, d, ancho, alto, escala_dibujo);
      const bool pulsado = pulsados[i];
      // No backdrop: the icon is the button. Held, it lights up and grows a little.
      const float r = Radio(i, d, alto, escala_dibujo) * (pulsado ? 1.1f : 1.0f);
      if (panel_abierto_ && int(i) == seleccionado_) {
        // Only while editing: which control the size slider changes.
        dl->AddCircle(centro, r * 1.1f, IM_COL32(255, 200, 40, 230), 48, std::max(2.0f, alto * 0.004f));
      }
      if (c.accion == Accion::kAjustes) {
        DibujarTuerca(dl, centro, r * 0.8f, color(255, 0.95f), color(10, 0.9f));
        continue;
      }
      const ImU32 tinte = color(255, pulsado ? 1.0f : 0.85f);
      if (c.icono) {
        if (auto it = iconos_.find(c.icono); it != iconos_.end() && it->second.textura) {
          // Fitted into the button's square, keeping its proportions.
          const float lado = r * 2.0f;
          const float aspecto = float(it->second.ancho) / float(it->second.alto);
          const float w = aspecto >= 1.0f ? lado : lado * aspecto, h = aspecto >= 1.0f ? lado / aspecto : lado;
          const ImVec2 a(centro.x - w * 0.5f, centro.y - h * 0.5f), b(centro.x + w * 0.5f, centro.y + h * 0.5f);
          dl->AddImage(reinterpret_cast<ImTextureID>(it->second.textura.get()), a, b,
                       ImVec2(c.espejo ? 1.0f : 0.0f, 0.0f), ImVec2(c.espejo ? 0.0f : 1.0f, 1.0f), tinte);
          continue;
        }
      }
      const float tam = r * (std::strlen(c.etiqueta) > 2 ? 0.7f : 1.1f);
      const ImVec2 medida = fuente->CalcTextSizeA(tam, FLT_MAX, 0.0f, c.etiqueta);
      dl->AddText(fuente, tam, ImVec2(centro.x - medida.x * 0.5f, centro.y - medida.y * 0.5f), tinte, c.etiqueta);
    }

    if (panel_abierto_) {
      DibujarPanel(io);
    }
  }

 private:
  struct Icono {
    std::unique_ptr<rex::ui::ImmediateTexture> textura;
    int ancho = 1, alto = 1;
  };

  void CargarIconos() {
    auto* inmediato = imgui_drawer() ? imgui_drawer()->immediate_drawer() : nullptr;
    if (!inmediato) {
      return;
    }
    int cargados = 0;
    for (const Control& c : kControles) {
      if (!c.icono || iconos_.count(c.icono)) {
        continue;
      }
      int w = 0, h = 0;
      const std::vector<uint8_t> rgba = LeerDds(LeerArchivoIcono(c.icono), w, h);
      Icono& icono = iconos_[c.icono];
      if (rgba.empty()) {
        continue;
      }
      icono.textura = inmediato->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear,
                                               false, rgba.data());
      icono.ancho = w;
      icono.alto = h;
      ++cargados;
    }
    REXLOG_INFO("[tactil] {} of {} button icons loaded (botones/*.dds)", cargados, iconos_.size());
  }

  // A hex nut: the settings button (there is no icon for it among the game's).
  static void DibujarTuerca(ImDrawList* dl, ImVec2 centro, float r, ImU32 color, ImU32 agujero) {
    constexpr float kPi = 3.14159265f;  // M_PI is not standard (MSVC's headers lack it)
    ImVec2 puntos[6];
    for (int i = 0; i < 6; ++i) {
      const float a = kPi / 6.0f + float(i) * kPi / 3.0f;
      puntos[i] = ImVec2(centro.x + std::cos(a) * r, centro.y + std::sin(a) * r);
    }
    dl->AddConvexPolyFilled(puntos, 6, color);
    dl->AddCircleFilled(centro, r * 0.45f, agujero, 24);
  }

  // While the panel is open (the controls take no touches then, ImGui's pointer is the finger): touching a control
  // selects it for the size slider, dragging it moves it.
  void Editar(ImGuiIO& io, float ancho, float alto, float escala) {
    if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
        !ImGui::IsAnyItemActive()) {
      arrastrado_ = ControlEn(io.MousePos.x, io.MousePos.y, ancho, alto, escala, disposicion_panel_);
      if (arrastrado_ >= 0) {
        seleccionado_ = arrastrado_;
        const ImVec2 centro = Centro(size_t(arrastrado_), disposicion_panel_, ancho, alto, escala);
        agarre_ = ImVec2(centro.x - io.MousePos.x, centro.y - io.MousePos.y);
        inicio_ = io.MousePos;
        moviendo_ = false;
      }
    }
    if (arrastrado_ < 0) {
      return;
    }
    if (!ImGui::IsMouseDown(0)) {
      arrastrado_ = -1;
      return;
    }
    // A touch that barely moves only selects.
    moviendo_ |= std::hypot(io.MousePos.x - inicio_.x, io.MousePos.y - inicio_.y) > alto * 0.015f;
    if (moviendo_) {
      Ajuste& a = disposicion_panel_[size_t(arrastrado_)];
      a.movido = true;
      a.fx = std::clamp((io.MousePos.x + agarre_.x) / ancho, 0.0f, 1.0f);
      a.fy = std::clamp((io.MousePos.y + agarre_.y) / alto, 0.0f, 1.0f);
    }
  }

  void DibujarPanel(ImGuiIO& io) {
    const float alto = io.DisplaySize.y;
    // It can be dragged away from the controls behind it by its title bar.
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, alto * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(alto * 0.9f, 0.0f), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Controles tactiles / Touch controls", nullptr, flags)) {
      ImGui::SetWindowFontScale(std::max(1.0f, alto / 450.0f));
      ImGui::TextWrapped("Arrastra un boton para moverlo; tocalo para cambiar su tamano. / Drag a button to move it; "
                         "tap it to resize it.");
      ImGui::Separator();
      ImGui::TextUnformatted("Tamano general / Overall size");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::SliderFloat("##escala", &escala_panel_, 0.6f, 1.6f, "%.2f");
      ImGui::TextUnformatted("Opacidad / Opacity");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::SliderFloat("##opacidad", &opacidad_panel_, 0.1f, 1.0f, "%.2f");
      if (seleccionado_ >= 0) {
        ImGui::Text("Tamano de %s / %s size", kControles[seleccionado_].etiqueta, kControles[seleccionado_].etiqueta);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##tam_boton", &disposicion_panel_[size_t(seleccionado_)].tam, 0.5f, 2.0f, "%.2f");
      } else {
        ImGui::TextDisabled("Toca un boton para su tamano / Tap a button for its size");
      }
      ImGui::Separator();
      ImGui::TextUnformatted("Botones visibles / Visible buttons");
      if (ImGui::BeginTable("##visibles", 4)) {
        for (size_t i = 0; i < kNumControles; ++i) {
          if (kControles[i].accion == Accion::kAjustes) {
            continue;
          }
          ImGui::TableNextColumn();
          bool visible = !disposicion_panel_[i].oculto;
          if (ImGui::Checkbox(kControles[i].etiqueta, &visible)) {
            disposicion_panel_[i].oculto = !visible;
            if (!visible && seleccionado_ == int(i)) {
              seleccionado_ = -1;
            }
          }
        }
        ImGui::EndTable();
      }
      ImGui::Spacing();
      const ImVec2 boton(-1.0f, ImGui::GetFrameHeight() * 1.6f);
      if (ImGui::Button("Restablecer / Reset", boton)) {
        disposicion_panel_ = Disposicion{};
        seleccionado_ = -1;
      }
      if (ImGui::Button("OK", boton)) {
        Guardar();
      }
    }
    ImGui::End();
  }

  // Applies and keeps the panel's values in nfsmw.toml, and gives the touches back to the controls.
  void Guardar() {
    rex::cvar::SetFlagByName("nfsmw_tactil_escala", fmt::format("{:.2f}", escala_panel_));
    rex::cvar::SetFlagByName("nfsmw_tactil_opacidad", fmt::format("{:.2f}", opacidad_panel_));
    rex::cvar::SetFlagByName("nfsmw_tactil_disposicion", EscribirDisposicion(disposicion_panel_));
    rex::cvar::SaveConfig(rex::filesystem::GetExecutableFolder() / "nfsmw.toml");
    panel_abierto_ = false;
    arrastrado_ = -1;
    std::lock_guard<std::mutex> cerrojo(g_estado.mutex);
    g_estado.disposicion = disposicion_panel_;
    g_estado.ajustes = false;
  }

  bool iconos_cargados_ = false;
  std::unordered_map<std::string, Icono> iconos_;
  bool disposicion_leida_ = false;
  bool panel_abierto_ = false;
  float escala_panel_ = 1.0f;
  float opacidad_panel_ = 0.45f;
  Disposicion disposicion_panel_{};
  int seleccionado_ = -1;  // control the size slider changes
  int arrastrado_ = -1;    // control under the finger while it is down
  bool moviendo_ = false;  // that finger went past the tap slack
  ImVec2 agarre_{};        // from the finger to the control's center
  ImVec2 inicio_{};        // where the finger went down
};

}  // namespace

void EnvolverEntrada(rex::RuntimeConfig& config) {
  if (!REXCVAR_GET(nfsmw_tactil) || !config.input_factory) {
    return;
  }
  auto base = config.input_factory;
  config.input_factory = [base](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto sistema = base(tool_mode);
    if (sistema && !tool_mode) {
      // The default factory is CreateDefaultInputSystem, which returns a rex::input::InputSystem.
      static_cast<in::InputSystem*>(sistema.get())->AddDriver(std::make_unique<MandoTactil>());
      SDL_AddEventWatch(VigilarDedos, nullptr);
      REXLOG_INFO("[tactil] touch controls on");
    }
    return sistema;
  };
}

void MostrarControles(rex::ui::ImGuiDrawer* drawer) {
  if (!REXCVAR_GET(nfsmw_tactil) || !drawer) {
    return;
  }
  // Registers itself in the drawer, which owns it from here on.
  new Controles(drawer);
}

}  // namespace nfsmw::tactil
