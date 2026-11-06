// nfsmw - graphics settings in the menu (F4) that work with the native renderer.
//
// The FPS and resolution settings that do nothing are removed from the menu and the ones that work are kept.
// Measured on PC:
//   - NFSMW draws at the size of the video mode that VdQueryVideoMode returns, but it only has two:
//     1280x720 and, below that, 1024x576 (tried 854x480, 960x540, 1024x576 and 1152x648). Above
//     1280x720 it stays at 1280x720.
//   - Its pace is set by the vblank, which follows video_mode_refresh_rate: at 30 Hz it gives exactly
//     30 FPS and the game runs at normal speed (the race timer advances in real time).
//   - resolution_scale, draw_resolution_scale_x/y, vsync, anisotropic_override and swap_post_effect are
//     only read by the emulated path: with the native renderer they do nothing.
// With the gamepad, the menu makes it awkward to change free text (resolution), an integer one step at a
// time (video_mode_width/height) or a decimal (video_mode_refresh_rate). That is why the ones that work
// are offered as lists (nfsmw_resolucion_interna and nfsmw_limite_fps) and passed to the video mode at
// startup.

#pragma once

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>

#include <rex/platform.h>
#include <string>

namespace nfsmw::ajustes {

// Optional post-processing in the Graficos/Posproceso category, like the one in GoldenEye-Recomp (which only
// exists in D3D12): color grading (temperature, brightness, contrast, tint, saturation, vibrance and gamma),
// vignette and scanlines. Off by default: the output stays bit-identical. Applied by the output pass of the
// native renderer.
struct Posproceso {
  bool activo = false;
  float brillo = 0.0f;
  float contraste = 1.0f;
  float saturacion = 1.0f;
  float vibracion = 0.0f;
  float temperatura = 0.0f;
  float gamma = 1.0f;
  float tinte_r = 1.0f;
  float tinte_g = 1.0f;
  float tinte_b = 1.0f;
  float tinte = 0.0f;
  float vineta = 0.0f;
  float lineas = 0.0f;
};

// Values in effect (the chosen preset or the custom ones). Reads the cvars with the registry lock: call
// only when VersionPosproceso changes.
Posproceso LeerPosproceso();

// Increments every time the menu changes a post-processing cvar (lock-free: it can be checked every frame).
uint64_t VersionPosproceso();

// nfsmw_antialiasing = fxaa (lock-free: it can be checked every frame).
bool AntialiasingFxaa();

// nfsmw_resplandor_cielo: 0 original, 1 natural, 2 suave (lock-free: it can be checked on every draw).
int ResplandorCielo();

// nfsmw_tratamiento_visual: 0 original, 1 suave, 2 apagado (lock-free: it can be checked on every draw).
int TratamientoVisual();

// Registers the change notifications for the live Graphics settings (post-processing, antialiasing, sky glow
// and the color filter), once at startup.
void VigilarAjustesEnVivo();

// Passes nfsmw_resolucion_interna and nfsmw_limite_fps to the video mode (video_mode_width/height,
// resolution and video_mode_refresh_rate), except the ones that come from the command line. Must run
// before the game requests the video mode.
void AplicarAjustesGraficos();

// With the native renderer, removes from the menu the FPS, resolution and graphics settings that do
// nothing, and the video mode settings that the ones above replace.
void OcultarAjustesSinEfecto();

// Width over height the picture should have: the screen's (the primary monitor on PC, the display on Android) with
// nfsmw_relacion_aspecto = automatico, or the one chosen there; 0 if unknown.
float AspectoPantalla();

// nfsmw_resolucion_interna as the game started: the render targets are sized then, so everything that follows the
// picture's size (the wide camera, the interface's 16:9 middle, the presented shape) must keep using it until a
// restart, even after the setting is changed in the menus.
const std::string& ResolucionEnUso();

// An nfsmw_resolucion_interna other than 720p ("WxH", 16:9 or wider, width a multiple of 32): drawn through the game's high
// mode (nfsmw_render_targets.cpp) with that size, scene, HUD and front buffer alike, while the video mode stays
// 1280x720. That works below 720p too (360p to 576p): changing the video mode instead never made the scene smaller
// and left the start of the story mode without a city. False for 720p, for "automatico" and for the 4:3 640x480.
// On a screen wider than 16:9 (not on the Switch) every 16:9 choice keeps its height and takes the screen's shape:
// 1080p on a 21:9 monitor draws 2528x1080, 720p on a 19.5:9 phone 1568x720 (widths a multiple of 32). The camera
// opens to the sides (nfsmw_panoramico.cpp) and the interface stays 16:9 in the middle. "automatico" is 1080 high on
// PC and 720 on Android there; elsewhere it keeps its own meaning.
inline bool AnchoDePantalla(unsigned alto, unsigned& ancho) {
#if !REX_PLATFORM_SWITCH
  const float aspecto = AspectoPantalla();
  if (aspecto > 0.0f && std::abs(aspecto / (16.0f / 9.0f) - 1.0f) > 0.02f) {
    ancho = unsigned(float(alto) * aspecto / 32.0f + 0.5f) * 32u;
    return true;
  }
#endif
  (void)alto;
  (void)ancho;
  return false;
}
inline bool ResolucionAlta(const std::string& valor, uint32_t& ancho, uint32_t& alto) {
  unsigned w = 0, h = 0;
  if (valor == "automatico") {
    h = REX_PLATFORM_ANDROID ? 720u : 1080u;
    if (!AnchoDePantalla(h, w)) return false;
    ancho = w;
    alto = h;
    return true;
  }
  if (std::sscanf(valor.c_str(), "%ux%u", &w, &h) != 2) return false;
  // A 16:9 choice on a wider screen: same height, the screen's width (1280x720 included, through the high mode).
  if (w * 9 == h * 16 || (w == 832 && h == 468)) {
    unsigned ancha = 0;
    if (h >= 360 && AnchoDePantalla(h, ancha)) {
      ancho = ancha;
      alto = h;
      return true;
    }
  }
  // 1280x720 is the game's own mode (2); a wider 720 (phones) goes through the high one.
  if ((h == 720 && w <= 1280) || h < 360 || w % 32 != 0 || w * 9 < h * 15) return false;
  ancho = w;
  alto = h;
  return true;
}

}  // namespace nfsmw::ajustes
