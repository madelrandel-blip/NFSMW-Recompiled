// Wide screens (21:9, 32:9, phones at 20:9): the 3D camera opens its horizontal field of view instead of stretching
// the 16:9 picture ("Hor+"). The interface stays 16:9 in the middle (nfsmw_fe_raton/renderer side).
//
// sub_8243EC28(matrices r3, view r4) builds each view's projection every frame. The field of view is the camera's
// (view +64) u16 at +196, in binary angle units (65536 = 360 degrees), and is horizontal: the aspect comes from a
// fixed 640x480 table times a constant (0x820B0620 or 0x820B0628, depending on the flag byte 0x828F4A8F), so the
// picture is always framed as 16:9 whatever the render target size. For a wider picture, the field of view is
// widened for the call (tan(new / 2) = tan(old / 2) * aspect / (16 / 9)) and the aspect constant shrunk by the same
// factor, so the vertical field of view stays the same; both are put back afterwards.

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_ajustes_graficos.h"

REXCVAR_DECLARE(std::string, nfsmw_resolucion_interna);

namespace {
uint32_t Leer32(const uint8_t* base, uint32_t d) {
  uint32_t v;
  std::memcpy(&v, base + d, 4);
  return __builtin_bswap32(v);
}
void Escribir32(uint8_t* base, uint32_t d, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + d, &v, 4);
}
float LeerF(const uint8_t* base, uint32_t d) {
  const uint32_t v = Leer32(base, d);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void EscribirF(uint8_t* base, uint32_t d, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Escribir32(base, d, v);
}

// The picture's width over its height divided by 16:9 (1 = no change).
float Factor() {
  uint32_t ancho = 0, alto = 0;
  if (!nfsmw::ajustes::ResolucionAlta(nfsmw::ajustes::ResolucionEnUso(), ancho, alto) || !alto) return 1.0f;
  const float f = (float(ancho) / float(alto)) / (16.0f / 9.0f);
  return std::abs(f - 1.0f) > 0.01f ? f : 1.0f;
}

}  // namespace

REX_EXTERN(__imp__sub_8243EC28);

namespace nfsmw::panoramico {
// Runs the original projection builder with the wide field of view; called from its hook (nfsmw_recorte_sombras).
void ConstruirProyeccion(PPCContext& ctx, uint8_t* base) {
  const float factor = Factor();
  const uint32_t camara = ctx.r4.u32 ? Leer32(base, ctx.r4.u32 + 64) : 0;
  if (factor == 1.0f || !camara) {
    __imp__sub_8243EC28(ctx, base);
    return;
  }
  uint16_t fov_be;
  std::memcpy(&fov_be, base + camara + 196, 2);
  const uint16_t fov = __builtin_bswap16(fov_be);
  const double medio = double(fov) * 3.14159265358979 / 65536.0;  // half the angle, in radians
  const double nuevo = 2.0 * std::atan(std::tan(medio) * factor) * 65536.0 / (2.0 * 3.14159265358979);
  const uint16_t fov_ancho = __builtin_bswap16(uint16_t(std::min(nuevo, 32000.0)));
  // The aspect is height / width of the view's entry in the table at 0x82C3F590 (384 bytes per view index, object
  // at +368 with the width at +16 and the height at +20); the constants it is multiplied by are read-only, so the
  // width grows by the factor instead.
  const uint32_t indice = ctx.r4.u32 ? Leer32(base, ctx.r4.u32 + 4) : 0;
  const uint32_t tabla = Leer32(base, 0x82C3F590u + indice * 384u + 368u);
  const uint32_t ancho = tabla ? Leer32(base, tabla + 16) : 0;
  static int avisos = 0;
  if (avisos++ < 3) {
    REXLOG_INFO("[panoramico] factor {:.3f}: campo de vision {} -> {} (de 65536), vista {} con tabla {:08X} de "
                "{}x{}", factor, fov, uint32_t(nuevo), indice, tabla, ancho, tabla ? Leer32(base, tabla + 20) : 0);
  }
  // Only views framed as a screen (the 640x480 entry); the shadow maps' square ones keep their projection.
  if (!ancho || ancho == Leer32(base, tabla + 20)) {
    __imp__sub_8243EC28(ctx, base);
    return;
  }
  std::memcpy(base + camara + 196, &fov_ancho, 2);
  // Only the field of view: changing the aspect as well (the table's width) closed the vertical view by the same
  // factor, measured on a 19.5:9 phone (the car 22 % taller than at 16:9).
  __imp__sub_8243EC28(ctx, base);
  std::memcpy(base + camara + 196, &fov_be, 2);
}
}  // namespace nfsmw::panoramico
