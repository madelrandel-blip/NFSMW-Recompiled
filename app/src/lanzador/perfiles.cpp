// nfsmw - perfiles de calidad del lanzador (ver perfiles.h).
//
// Los valores salen de lo que se midio en el port de Switch para cada cvar
// (docs/performance-history.md y los comentarios de cada REXCVAR_DEFINE):
//   - nfsmw_sombras_corte: 100 = distancia del juego; 150 y 250 recortan.
//   - nfsmw_nativo_sombras_escala: 100 = 1600x1600 como la Xbox 360; 64 = como el PC original.
//   - nfsmw_sombras_sin_vegetacion: quita arboles y vallas del mapa de sombras (-1,2 ms GPU, -3,1 ms CPU).
//   - nfsmw_reflejo_refresco: cada cuantos fotogramas se renueva el reflejo de la carretera.
//   - nfsmw_cubemap_caras_max: caras del reflejo del coche por fotograma (-1 = las del juego).
//   - nfsmw_escena_detalle_minimo: tamano minimo en pixeles para dibujar un objeto (0 = el juego).

#include "perfiles.h"

#include <rex/cvar.h>

#include <string>
#include <utility>

namespace nfsmw::perfiles {
namespace {

struct Valor {
  const char* cvar;
  const char* valor[kNumPerfiles];  // bajo, medio, alto, ultra
};

// clang-format off
constexpr Valor kValores[] = {
  {"nfsmw_resolucion_interna",     {"1024x576", "1280x720", "1280x720", "1920x1080"}},
  {"nfsmw_antialiasing",           {"apagado",  "apagado",  "fxaa",     "fxaa"}},
  {"nfsmw_nativo_anisotropico",    {"0",        "0",        "4",        "16"}},
  {"nfsmw_sombras_corte",          {"250",      "150",      "100",      "100"}},
  {"nfsmw_nativo_sombras_escala",  {"64",       "100",      "100",      "100"}},
  {"nfsmw_sombras_sin_vegetacion", {"true",     "true",     "false",    "false"}},
  {"nfsmw_nativo_pcf_barato",      {"true",     "true",     "false",    "false"}},
  {"nfsmw_reflejo_refresco",       {"8",        "4",        "2",        "1"}},
  {"nfsmw_cubemap_caras_max",      {"1",        "2",        "-1",       "-1"}},
  {"nfsmw_escena_detalle_minimo",  {"6",        "0",        "0",        "0"}},
};
// clang-format on

constexpr const char* kClaves[kNumPerfiles] = {"perfil.bajo", "perfil.medio", "perfil.alto", "perfil.ultra"};

}  // namespace

const char* Clave(int perfil) {
  return perfil >= 0 && perfil < kNumPerfiles ? kClaves[perfil] : "perfil.personalizado";
}

void Aplicar(int perfil) {
  if (perfil < 0 || perfil >= kNumPerfiles) return;
  for (const auto& v : kValores)
    if (rex::cvar::GetFlagInfo(v.cvar) != nullptr) rex::cvar::SetFlagByName(v.cvar, v.valor[perfil]);
}

void AplicarXbox360() {
  // Cada valor es "como el juego" segun la descripcion de su cvar.
  static constexpr std::pair<const char*, const char*> kOriginal[] = {
      {"nfsmw_resolucion_interna", "1280x720"},  // la de la Xbox 360
      {"nfsmw_limite_fps", "30"},                // la consola va a 30
      {"nfsmw_antialiasing", "apagado"},
      {"nfsmw_nativo_omitir_sombras", "false"},  // con sombras
      {"nfsmw_guardia_30", "false"},
      {"nfsmw_nativo_anisotropico", "0"},        // trilineal, como la Xbox 360
      {"nfsmw_resplandor_cielo", "original"},
      {"nfsmw_tratamiento_visual", "original"},
      {"nfsmw_posproceso", "apagado"},
      {"nfsmw_sombras_corte", "100"},
      {"nfsmw_nativo_sombras_escala", "100"},    // 1600x1600, como la Xbox 360
      {"nfsmw_sombras_sin_vegetacion", "false"},
      {"nfsmw_nativo_pcf_barato", "false"},      // el 3x3 del juego
      {"nfsmw_sombras_mapas", "2"},
      {"nfsmw_sombras_cada", "1"},
      {"nfsmw_reflejo_carretera", "true"},
      {"nfsmw_reflejo_refresco", "1"},
      {"nfsmw_cubemap_caras_max", "-1"},         // las que decide el juego
      {"nfsmw_cubemap_detalle_minimo", "0"},
      {"nfsmw_escena_detalle_minimo", "0"},
      {"nfsmw_escenario_detalle", "0"},
      {"nfsmw_retrovisor_recorte", "false"},
      {"nfsmw_streaming_anticipacion", "100"},   // el streamer original
      {"nfsmw_nativo_sin_desenfoque", "false"},  // con el desenfoque de velocidad
  };
  for (const auto& [cvar, valor] : kOriginal)
    if (rex::cvar::GetFlagInfo(cvar) != nullptr) rex::cvar::SetFlagByName(cvar, valor);
}

int Detectar() {
  for (int p = 0; p < kNumPerfiles; ++p) {
    bool igual = true;
    for (const auto& v : kValores) {
      if (rex::cvar::GetFlagInfo(v.cvar) == nullptr) continue;
      if (rex::cvar::GetFlagByName(v.cvar) != v.valor[p]) {
        igual = false;
        break;
      }
    }
    if (igual) return p;
  }
  return kPersonalizado;
}

}  // namespace nfsmw::perfiles
