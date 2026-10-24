// nfsmw - perfiles de calidad del lanzador: Bajo, Medio, Alto y Ultra.
//
// Un perfil es un conjunto fijo de valores de los cvars del renderizador
// nativo. No se guarda en ningun sitio cual esta elegido: se deduce de los
// valores actuales, asi que tocar una opcion a mano lo convierte en
// "Personalizado" sin mas logica.

#pragma once

namespace nfsmw::perfiles {

constexpr int kNumPerfiles = 4;
constexpr int kPersonalizado = -1;

// Clave de texto del perfil (perfil.bajo, perfil.medio...).
const char* Clave(int perfil);

// Pone todos los valores del perfil.
void Aplicar(int perfil);

// El perfil cuyos valores coinciden todos con los actuales, o kPersonalizado.
int Detectar();

// Deja el juego como en la Xbox 360: 1280x720 a 30 fps y sin ninguno de los
// recortes ni mejoras del port (sombras, reflejos, filtros y posproceso como el
// original). El renderizador nativo se mantiene: no cambia lo que se ve.
void AplicarXbox360();

}  // namespace nfsmw::perfiles
