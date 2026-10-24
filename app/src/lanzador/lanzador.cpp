// nfsmw - lanzador dentro del juego (ver lanzador.h).

#include "lanzador.h"

#include "actualizacion.h"
#include "idioma.h"
#include "iso.h"
#include "perfiles.h"
#include "shaders.h"
#include "sistema.h"
#include "trucos.h"

#include "../edicion.h"

#include "../nfsmw_ajustes_graficos.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/image_decode.h>

#include <imgui.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(nfsmw_lanzador, true, "Interfaz",
                    "Mostrar el lanzador al abrir el juego (eleccion del juego y ajustes). false = arrancar directo");
REXCVAR_DEFINE_BOOL(nfsmw_lanzador_omitir, false, "Interfaz",
                    "Interno: el lanzador reinicia el juego con esto para no volver a mostrarse");
REXCVAR_DEFINE_STRING(nfsmw_juego, "", "Interfaz",
                      "Copia del juego elegida en el lanzador: una .iso o una carpeta con default.xex");

namespace nfsmw::lanzador {

using idioma::T;

namespace {

// ---- Tema: negro puro, blanco y grises, como la interfaz del juego ------------
constexpr ImU32 kFondo = IM_COL32(0, 0, 0, 255);
constexpr ImU32 kBarra = IM_COL32(8, 8, 8, 255);
constexpr ImU32 kTarjeta = IM_COL32(17, 17, 17, 255);
constexpr ImU32 kBorde = IM_COL32(38, 38, 38, 255);
constexpr ImU32 kCampo = IM_COL32(10, 10, 10, 255);
constexpr ImU32 kBordeCampo = IM_COL32(58, 58, 58, 255);
constexpr ImU32 kHover = IM_COL32(32, 32, 32, 255);
constexpr ImU32 kTexto = IM_COL32(245, 245, 245, 255);
constexpr ImU32 kSuave = IM_COL32(176, 176, 176, 255);
constexpr ImU32 kNota = IM_COL32(118, 118, 118, 255);
constexpr ImU32 kResalte = IM_COL32(238, 238, 238, 255);  // lo elegido: blanco
constexpr ImU32 kResalteHover = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kSobreResalte = IM_COL32(0, 0, 0, 255);  // texto encima de lo elegido
constexpr ImU32 kAviso = IM_COL32(215, 215, 215, 255);

ImFont* g_normal = nullptr;
ImFont* g_negrita = nullptr;
float g_escala = 1.0f;
float g_interior = 400.0f;  // ancho util dentro de la tarjeta actual

float S(float v) { return v * g_escala; }
ImVec4 C(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void Fuente(bool negrita, float px) { ImGui::PushFont(negrita && g_negrita ? g_negrita : g_normal, S(px)); }

// Etiqueta: "@clave" se traduce; cualquier otra cosa se muestra tal cual.
const char* Et(const char* s) { return s[0] == '@' ? T(s + 1) : s; }

std::string Cv(const char* nombre) { return rex::cvar::GetFlagByName(nombre); }
void PonerCv(const char* nombre, const std::string& valor) {
  if (rex::cvar::GetFlagInfo(nombre) == nullptr) return;
  // El lanzador corre antes de construir el runtime, pero despues de que el
  // SDK cierre la inicializacion: sin esto los cvars kInitOnly (el
  // renderizador) rechazarian el cambio en silencio. Se guardan en el toml y
  // valen al reiniciar, que es justo lo que hace el lanzador con ellos.
  rex::cvar::testing::ScopedLifecycleOverride permitir;
  rex::cvar::SetFlagByName(nombre, valor);
}

// Los ajustes del lanzador que solo valen al reiniciar el juego: el
// renderizador se crea antes del lanzador (kInitOnly), el limite de fps es
// kRequiresRestart y la resolucion interna va al modo de video del arranque.
constexpr const char* kAjustesConReinicio[] = {"nfsmw_renderizador", "nfsmw_limite_fps",
                                               "nfsmw_resolucion_interna"};

std::filesystem::path DesdeUtf8(const std::string& s) { return std::u8string(s.begin(), s.end()); }

std::filesystem::path CarpetaExtraccion() { return rex::filesystem::GetExecutableFolder() / "game_root"; }

bool EsIso(const std::filesystem::path& p) {
  auto ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext == ".iso";
}

// La biblioteca de shaders de una edicion; vacia = la de este ejecutable.
std::filesystem::path BibliotecaDe(const std::string& ed) {
  return edicion::Biblioteca(ed.empty() ? std::string(edicion::Propia()) : ed);
}

bool HayBiblioteca(const std::string& ed) {
  std::error_code ec;
  return std::filesystem::is_regular_file(BibliotecaDe(ed), ec);
}

// ---- Iconos ---------------------------------------------------------------------
//
// Dibujados a trazo, en blanco, como los del menu de opciones del juego
// (engranajes, altavoz, monitor...). No se usa ninguna imagen del juego.

enum class Icono { kInicio, kGraficos, kControles, kAudio, kPartidas, kTrucos, kConfig, kAcerca };

void Engranaje(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float grosor) {
  constexpr int kDientes = 8;
  ImVec2 puntos[kDientes * 4];
  for (int i = 0; i < kDientes; ++i) {
    const float a = float(i) * 6.2831853f / kDientes;
    const float d = 6.2831853f / kDientes;
    const float radios[4] = {r * 0.72f, r, r, r * 0.72f};
    const float angulos[4] = {a - d * 0.32f, a - d * 0.18f, a + d * 0.18f, a + d * 0.32f};
    for (int j = 0; j < 4; ++j)
      puntos[i * 4 + j] = ImVec2(c.x + std::cos(angulos[j]) * radios[j], c.y + std::sin(angulos[j]) * radios[j]);
  }
  dl->AddPolyline(puntos, kDientes * 4, col, ImDrawFlags_Closed, grosor);
  dl->AddCircle(c, r * 0.3f, col, 0, grosor);
}

void DibujarIcono(ImDrawList* dl, Icono icono, ImVec2 c, float t, ImU32 col) {
  const float g = std::max(1.5f, t * 0.09f);  // grosor de trazo
  const float h = t * 0.5f;
  switch (icono) {
    case Icono::kInicio: {
      const ImVec2 tejado[3] = {{c.x - h, c.y - h * 0.05f}, {c.x, c.y - h * 0.95f}, {c.x + h, c.y - h * 0.05f}};
      dl->AddPolyline(tejado, 3, col, 0, g);
      dl->AddRect(ImVec2(c.x - h * 0.72f, c.y - h * 0.15f), ImVec2(c.x + h * 0.72f, c.y + h * 0.85f), col, 0, 0, g);
      dl->AddRectFilled(ImVec2(c.x - h * 0.2f, c.y + h * 0.3f), ImVec2(c.x + h * 0.2f, c.y + h * 0.85f), col);
      break;
    }
    case Icono::kGraficos: {
      dl->AddRect(ImVec2(c.x - h, c.y - h * 0.8f), ImVec2(c.x + h * 0.6f, c.y + h * 0.35f), col, t * 0.06f, 0, g);
      dl->AddLine(ImVec2(c.x - h * 0.2f, c.y + h * 0.35f), ImVec2(c.x - h * 0.2f, c.y + h * 0.7f), col, g);
      dl->AddLine(ImVec2(c.x - h * 0.55f, c.y + h * 0.7f), ImVec2(c.x + h * 0.15f, c.y + h * 0.7f), col, g);
      dl->AddCircleFilled(ImVec2(c.x + h * 0.62f, c.y + h * 0.42f), h * 0.42f, kBarra);
      Engranaje(dl, ImVec2(c.x + h * 0.62f, c.y + h * 0.42f), h * 0.4f, col, g * 0.85f);
      break;
    }
    case Icono::kControles: {
      dl->AddRect(ImVec2(c.x - h, c.y - h * 0.5f), ImVec2(c.x + h, c.y + h * 0.45f), col, h * 0.45f, 0, g);
      const float cx = c.x - h * 0.45f;
      dl->AddLine(ImVec2(cx - h * 0.22f, c.y), ImVec2(cx + h * 0.22f, c.y), col, g);
      dl->AddLine(ImVec2(cx, c.y - h * 0.22f), ImVec2(cx, c.y + h * 0.22f), col, g);
      dl->AddCircleFilled(ImVec2(c.x + h * 0.38f, c.y - h * 0.1f), g * 0.85f, col);
      dl->AddCircleFilled(ImVec2(c.x + h * 0.6f, c.y + h * 0.12f), g * 0.85f, col);
      break;
    }
    case Icono::kAudio: {
      const ImVec2 altavoz[6] = {{c.x - h, c.y - h * 0.3f}, {c.x - h * 0.55f, c.y - h * 0.3f},
                                 {c.x - h * 0.05f, c.y - h * 0.75f}, {c.x - h * 0.05f, c.y + h * 0.75f},
                                 {c.x - h * 0.55f, c.y + h * 0.3f}, {c.x - h, c.y + h * 0.3f}};
      dl->AddPolyline(altavoz, 6, col, ImDrawFlags_Closed, g);
      dl->PathArcTo(ImVec2(c.x - h * 0.05f, c.y), h * 0.45f, -0.9f, 0.9f);
      dl->PathStroke(col, 0, g);
      dl->PathArcTo(ImVec2(c.x - h * 0.05f, c.y), h * 0.85f, -0.9f, 0.9f);
      dl->PathStroke(col, 0, g);
      break;
    }
    case Icono::kPartidas: {  // disquete, como el icono de guardar
      const ImVec2 cuerpo[5] = {{c.x - h * 0.8f, c.y - h * 0.8f}, {c.x + h * 0.5f, c.y - h * 0.8f},
                                {c.x + h * 0.8f, c.y - h * 0.5f}, {c.x + h * 0.8f, c.y + h * 0.8f},
                                {c.x - h * 0.8f, c.y + h * 0.8f}};
      dl->AddPolyline(cuerpo, 5, col, ImDrawFlags_Closed, g);
      dl->AddRect(ImVec2(c.x - h * 0.42f, c.y - h * 0.8f), ImVec2(c.x + h * 0.3f, c.y - h * 0.3f), col, 0, 0, g);
      dl->AddRect(ImVec2(c.x - h * 0.5f, c.y + h * 0.1f), ImVec2(c.x + h * 0.5f, c.y + h * 0.8f), col, 0, 0, g);
      break;
    }
    case Icono::kTrucos: {  // rayo, como el nitro
      const ImVec2 rayo[6] = {{c.x + h * 0.15f, c.y - h * 0.95f}, {c.x - h * 0.55f, c.y + h * 0.12f},
                              {c.x - h * 0.02f, c.y + h * 0.12f}, {c.x - h * 0.15f, c.y + h * 0.95f},
                              {c.x + h * 0.55f, c.y - h * 0.12f}, {c.x + h * 0.02f, c.y - h * 0.12f}};
      dl->AddPolyline(rayo, 6, col, ImDrawFlags_Closed, g);
      break;
    }
    case Icono::kConfig: {
      Engranaje(dl, ImVec2(c.x - h * 0.22f, c.y + h * 0.12f), h * 0.68f, col, g);
      Engranaje(dl, ImVec2(c.x + h * 0.55f, c.y - h * 0.52f), h * 0.4f, col, g * 0.85f);
      break;
    }
    case Icono::kAcerca: {
      dl->AddCircle(c, h * 0.9f, col, 0, g);
      dl->AddCircleFilled(ImVec2(c.x, c.y - h * 0.42f), g * 0.9f, col);
      dl->AddLine(ImVec2(c.x, c.y - h * 0.12f), ImVec2(c.x, c.y + h * 0.5f), col, g * 1.1f);
      break;
    }
  }
}

// ---- Piezas de la interfaz ---------------------------------------------------

void TextoColor(ImU32 color, const char* texto) {
  ImGui::PushStyleColor(ImGuiCol_Text, C(color));
  ImGui::TextUnformatted(texto);
  ImGui::PopStyleColor();
}

void Nota(const char* texto) {
  Fuente(false, 14.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, C(kNota));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + g_interior);
  ImGui::TextWrapped("%s", texto);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

// Tarjeta: seccion con fondo y titulo. El fondo se pinta al cerrarla, cuando
// ya se sabe su alto, en un canal de debajo.
class Tarjeta {
 public:
  explicit Tarjeta(const char* titulo) {
    dl_ = ImGui::GetWindowDrawList();
    dl_->ChannelsSplit(2);
    dl_->ChannelsSetCurrent(1);
    inicio_ = ImGui::GetCursorScreenPos();
    ancho_ = ImGui::GetContentRegionAvail().x;
    g_interior = ancho_ - 2 * S(22);
    ImGui::SetCursorScreenPos(ImVec2(inicio_.x + S(22), inicio_.y + S(18)));
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl_->AddRectFilled(ImVec2(p.x, p.y + S(3)), ImVec2(p.x + S(3), p.y + S(18)), kResalte);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(11), p.y));
    Fuente(true, 14.5f);
    TextoColor(kTexto, titulo);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(6)));
  }
  ~Tarjeta() {
    ImGui::Dummy(ImVec2(g_interior, 0));
    ImGui::EndGroup();
    const float alto = ImGui::GetItemRectMax().y - inicio_.y + S(18);
    dl_->ChannelsSetCurrent(0);
    const ImVec2 fin(inicio_.x + ancho_, inicio_.y + alto);
    dl_->AddRectFilled(inicio_, fin, kTarjeta, S(6));
    dl_->AddRect(inicio_, fin, kBorde, S(6));
    dl_->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(inicio_.x, fin.y + S(16)));
    ImGui::Dummy(ImVec2(0, 0));
  }

 private:
  ImDrawList* dl_;
  ImVec2 inicio_;
  float ancho_;
};

// Columna de la izquierda con la etiqueta; devuelve el ancho para el control.
float Fila(const char* etiqueta) {
  const float columna = std::min(S(240), g_interior * 0.42f);
  const float x0 = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  TextoColor(kSuave, etiqueta);
  ImGui::SameLine(x0 + columna);
  return g_interior - columna;
}

// Selector segmentado: todas las opciones a la vista, la elegida en blanco.
bool Segmentado(const char* id, const std::vector<const char*>& opciones, int* sel, float ancho) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float alto = ImGui::GetFrameHeight() + S(6);
  const int n = int(opciones.size());
  dl->AddRectFilled(p, ImVec2(p.x + ancho, p.y + alto), kCampo, S(4));
  dl->AddRect(p, ImVec2(p.x + ancho, p.y + alto), kBordeCampo, S(4));
  bool cambio = false;
  ImGui::PushID(id);
  for (int i = 0; i < n; ++i) {
    const float x0 = p.x + ancho * i / n, x1 = p.x + ancho * (i + 1) / n;
    ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##seg", ImVec2(x1 - x0, alto)) && *sel != i) {
      *sel = i;
      cambio = true;
    }
    const bool encima = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 a(x0 + S(3), p.y + S(3)), b(x1 - S(3), p.y + alto - S(3));
    ImU32 texto = kSuave;
    if (i == *sel) {
      dl->AddRectFilled(a, b, kResalte, S(3));
      texto = kSobreResalte;
    } else if (encima) {
      dl->AddRectFilled(a, b, kHover, S(3));
      texto = kTexto;
    }
    const char* et = opciones[i];
    const ImVec2 t = ImGui::CalcTextSize(et);
    dl->PushClipRect(a, b, true);
    dl->AddText(ImVec2((x0 + x1 - t.x) * 0.5f, p.y + (alto - t.y) * 0.5f), texto, et);
    dl->PopClipRect();
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + alto));
  ImGui::Dummy(ImVec2(ancho, S(4)));
  return cambio;
}

// Interruptor.
bool Interruptor(const char* id, const char* etiqueta, bool* valor) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float alto = ImGui::GetFrameHeight();
  const float pista_h = S(22), pista_w = S(40);
  ImGui::PushID(id);
  const bool pulsado = ImGui::InvisibleButton("##int", ImVec2(g_interior, alto));
  const bool encima = ImGui::IsItemHovered();
  ImGui::PopID();
  if (pulsado) *valor = !*valor;
  const float y = p.y + (alto - pista_h) * 0.5f;
  const ImU32 fondo = *valor ? (encima ? kResalteHover : kResalte) : (encima ? kHover : kCampo);
  dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + pista_w, y + pista_h), fondo, pista_h * 0.5f);
  if (!*valor) dl->AddRect(ImVec2(p.x, y), ImVec2(p.x + pista_w, y + pista_h), kBordeCampo, pista_h * 0.5f);
  const float r = pista_h * 0.5f - S(3);
  const float cx = *valor ? p.x + pista_w - S(3) - r : p.x + S(3) + r;
  dl->AddCircleFilled(ImVec2(cx, y + pista_h * 0.5f), r, *valor ? kSobreResalte : kSuave);
  dl->AddText(ImVec2(p.x + pista_w + S(12), p.y + (alto - ImGui::GetFontSize()) * 0.5f), kTexto, etiqueta);
  return pulsado;
}

bool Boton(const char* etiqueta, bool primario, ImVec2 tam, bool activo = true) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pulsado = ImGui::InvisibleButton(etiqueta, tam) && activo;
  const bool encima = ImGui::IsItemHovered() && activo;
  const bool abajo = ImGui::IsItemActive() && activo;
  ImU32 fondo, borde, texto;
  if (!activo) {
    fondo = kTarjeta, borde = kBorde, texto = kNota;
  } else if (primario) {
    fondo = abajo ? kSuave : encima ? kResalteHover : kResalte;
    borde = fondo, texto = kSobreResalte;
  } else {
    fondo = abajo ? kCampo : encima ? kHover : kTarjeta;
    borde = encima ? kResalte : kBordeCampo, texto = kTexto;
  }
  const ImVec2 q(p.x + tam.x, p.y + tam.y);
  dl->AddRectFilled(p, q, fondo, S(4));
  dl->AddRect(p, q, borde, S(4));
  // Lo que va tras "##" es el identificador, no se pinta.
  const char* fin = std::strstr(etiqueta, "##");
  if (!fin) fin = etiqueta + std::strlen(etiqueta);
  const ImVec2 t = ImGui::CalcTextSize(etiqueta, fin);
  dl->AddText(ImVec2(p.x + (tam.x - t.x) * 0.5f, p.y + (tam.y - t.y) * 0.5f), texto, etiqueta, fin);
  return pulsado;
}

std::string ConId(const char* texto, const char* id) { return std::string(texto) + "##" + id; }

// ---- Controles atados a un cvar ------------------------------------------------

struct Opcion {
  const char* etiqueta;  // "@clave" para traducir
  const char* valor;
};

int IndiceDe(const char* cvar, const std::vector<Opcion>& ops) {
  const std::string v = Cv(cvar);
  for (size_t i = 0; i < ops.size(); ++i)
    if (v == ops[i].valor) return int(i);
  return -1;
}

void SegCvar(const char* etiqueta, const char* cvar, const std::vector<Opcion>& ops) {
  const float ancho = Fila(etiqueta);
  std::vector<const char*> et;
  for (const auto& o : ops) et.push_back(Et(o.etiqueta));
  int sel = IndiceDe(cvar, ops);
  if (Segmentado(cvar, et, &sel, ancho)) PonerCv(cvar, ops[size_t(sel)].valor);
}

void ComboCvar(const char* etiqueta, const char* cvar, const std::vector<Opcion>& ops) {
  const float ancho = Fila(etiqueta);
  const int sel = IndiceDe(cvar, ops);
  const std::string actual = sel >= 0 ? Et(ops[size_t(sel)].etiqueta) : Cv(cvar);
  ImGui::SetNextItemWidth(ancho);
  ImGui::PushID(cvar);
  if (ImGui::BeginCombo("##combo", actual.c_str())) {
    for (size_t i = 0; i < ops.size(); ++i) {
      if (ImGui::Selectable(Et(ops[i].etiqueta), int(i) == sel)) PonerCv(cvar, ops[i].valor);
      if (int(i) == sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  ImGui::Dummy(ImVec2(0, S(2)));
}

void BoolCvar(const char* etiqueta, const char* cvar, bool invertir = false) {
  bool v = (Cv(cvar) == "true") != invertir;
  if (Interruptor(cvar, etiqueta, &v)) PonerCv(cvar, (v != invertir) ? "true" : "false");
}

void Separador() { ImGui::Dummy(ImVec2(0, S(6))); }

constexpr Icono kIconos[] = {Icono::kInicio,   Icono::kGraficos, Icono::kControles, Icono::kAudio,
                             Icono::kPartidas, Icono::kTrucos,   Icono::kConfig,    Icono::kAcerca};
constexpr const char* kTitulos[] = {"nav.inicio",   "nav.graficos", "nav.controles", "nav.audio",
                                    "nav.partidas", "nav.trucos",   "nav.config",    "nav.acerca"};

std::string Tamano(uint64_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
  else std::snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
  return buf;
}

float AnchoLateral(float ancho) { return std::clamp(ancho * 0.22f, S(220), S(300)); }

}  // namespace

// ============================================================================
//  Funciones publicas
// ============================================================================

void CargarFuentes(ImFontAtlas* atlas) {
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> candidatas;
#if defined(_WIN32)
  const char* windir = std::getenv("WINDIR");
  const std::filesystem::path fuentes = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts";
  candidatas.push_back({fuentes / "segoeui.ttf", fuentes / "segoeuib.ttf"});
#else
  candidatas.push_back({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"});
  candidatas.push_back({"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"});
#endif
  std::error_code ec;
  for (const auto& [normal, negrita] : candidatas) {
    if (!std::filesystem::is_regular_file(normal, ec)) continue;
    g_normal = atlas->AddFontFromFileTTF(rex::path_to_utf8(normal).c_str(), 18.0f);
    if (std::filesystem::is_regular_file(negrita, ec))
      g_negrita = atlas->AddFontFromFileTTF(rex::path_to_utf8(negrita).c_str(), 18.0f);
    break;
  }
}

bool DebeMostrarse() {
  if (REXCVAR_GET(nfsmw_lanzador_omitir)) {
    // Se vuelve a su valor por defecto para que ningun guardado posterior lo
    // deje escrito en nfsmw.toml y el lanzador desaparezca para siempre.
    rex::cvar::SetFlagByName("nfsmw_lanzador_omitir", "false");
    return false;
  }
  return REXCVAR_GET(nfsmw_lanzador);
}

std::filesystem::path CarpetaGuardada(const std::filesystem::path& por_defecto) {
  const std::string guardado = REXCVAR_GET(nfsmw_juego);
  const auto juego = guardado.empty() ? por_defecto : DesdeUtf8(guardado);
  if (juego.empty()) return {};
  if (EsIso(juego)) return iso::CopiaValida(CarpetaExtraccion(), juego) ? CarpetaExtraccion() : juego;
  return juego;
}

// ============================================================================
//  Lanzador
// ============================================================================

Lanzador::Lanzador(rex::ui::ImGuiDrawer* drawer, std::filesystem::path juego_por_defecto, Callbacks callbacks)
    : rex::ui::ImGuiDialog(drawer),
      callbacks_(std::move(callbacks)),
      juego_por_defecto_(std::move(juego_por_defecto)),
      pantalla_completa_(Cv("fullscreen") == "true") {
  for (const char* nombre : kAjustesConReinicio) valores_iniciales_.push_back(Cv(nombre));
  if (callbacks_.pantalla_completa) callbacks_.pantalla_completa(false);
  // La consulta de version va aparte: sin red tarda lo que tarde el tiempo de espera.
  hilo_version_ = std::thread([this] {
    const auto r = actualizacion::Comprobar();
    if (r.hay_nueva) {
      std::lock_guard<std::mutex> l(mutex_);
      version_nueva_ = r.version;
      url_version_ = r.url;
    }
    version_lista_ = true;
  });
}

Lanzador::~Lanzador() {
  cancelar_ = true;
  if (hilo_.joinable()) hilo_.join();
  if (hilo_version_.joinable()) hilo_version_.join();
}

Lanzador::Estado Lanzador::EstadoJuego() const {
  const std::string guardado = REXCVAR_GET(nfsmw_juego);
  const auto juego = guardado.empty() ? juego_por_defecto_ : DesdeUtf8(guardado);
  if (juego.empty()) return Estado::kNinguno;
  std::error_code ec;
  if (!std::filesystem::exists(juego, ec)) return Estado::kFalta;
  if (EsIso(juego)) return iso::CopiaValida(CarpetaExtraccion(), juego) ? Estado::kIsoLista : Estado::kIsoNueva;
  return iso::EsCarpetaDelJuego(juego) ? Estado::kCarpeta : Estado::kInvalida;
}

std::filesystem::path Lanzador::CarpetaJuego() const {
  switch (EstadoJuego()) {
    case Estado::kIsoLista:
      return CarpetaExtraccion();
    case Estado::kCarpeta: {
      const std::string guardado = REXCVAR_GET(nfsmw_juego);
      return guardado.empty() ? juego_por_defecto_ : DesdeUtf8(guardado);
    }
    default:
      return {};
  }
}

// No se mira la lista de reinicios pendientes del SDK: al cargar nfsmw.toml
// cada cvar kRequiresRestart que viene del fichero se apunta como pendiente, y
// el lanzador pediria reiniciar sin que se haya tocado nada.
bool Lanzador::NecesitaReinicio() const {
  for (size_t i = 0; i < std::size(kAjustesConReinicio); ++i)
    if (Cv(kAjustesConReinicio[i]) != valores_iniciales_[i]) return true;
  return false;
}

void Lanzador::ArrancarTarea(Tarea tarea) {
  if (hilo_.joinable()) hilo_.join();
  {
    std::lock_guard<std::mutex> l(mutex_);
    error_.clear();
    detalle_.clear();
  }
  fase_ = 0;
  hechos_ = 0;
  total_ = 0;
  cancelar_ = false;
  tarea_terminada_ = false;
  tarea_ = tarea;

  if (tarea == Tarea::kExtraer) {
    const std::string guardado = REXCVAR_GET(nfsmw_juego);
    const auto imagen = guardado.empty() ? juego_por_defecto_ : DesdeUtf8(guardado);
    hilo_ = std::thread([this, imagen] {
      const std::string error = iso::Extraer(imagen, CarpetaExtraccion(),
                                             [this](const std::string& fichero, int hechos, int total) {
                                               std::lock_guard<std::mutex> l(mutex_);
                                               detalle_ = fichero;
                                               hechos_ = hechos;
                                               total_ = total;
                                             },
                                             cancelar_);
      std::lock_guard<std::mutex> l(mutex_);
      error_ = cancelar_ ? std::string() : error;
      tarea_terminada_ = true;
    });
  } else {
    const auto carpeta = CarpetaJuego();
    const auto salida = BibliotecaDe(EdicionJuego());
    hilo_ = std::thread([this, carpeta, salida] {
      const std::string error = shaders::Generar(carpeta, salida,
                                                 [this](shaders::Fase fase, int hechos, int total) {
                                                   fase_ = int(fase);
                                                   hechos_ = hechos;
                                                   total_ = total;
                                                 },
                                                 cancelar_);
      std::lock_guard<std::mutex> l(mutex_);
      error_ = cancelar_ ? std::string() : error;
      tarea_terminada_ = true;
    });
  }
}

std::string Lanzador::EdicionJuego() const {
  const auto carpeta = CarpetaJuego();
  if (carpeta.empty()) return {};
  // La clave incluye la fecha del default.xex: una ISO nueva extraida en la misma carpeta se vuelve a mirar.
  std::error_code ec;
  const auto xex = carpeta / "default.xex";
  const std::string clave = rex::path_to_utf8(carpeta) + "|" +
                            std::to_string(std::filesystem::last_write_time(xex, ec).time_since_epoch().count());
  if (clave != edicion_clave_) {
    edicion_clave_ = clave;
    edicion_juego_ = edicion::Detectar(carpeta);
  }
  return edicion_juego_;
}

void Lanzador::Jugar() {
  const Estado estado = EstadoJuego();
  // Antes de nada, lo que hace falta para arrancar: la copia extraida y la
  // biblioteca de shaders. Si luego hay que reiniciar, el proceso nuevo
  // arranca directo y ya lo necesita todo hecho.
  if (estado == Estado::kIsoNueva) {
    jugar_al_terminar_ = true;
    ArrancarTarea(Tarea::kExtraer);
    return;
  }
  if (estado != Estado::kIsoLista && estado != Estado::kCarpeta) return;
  const std::string ed = EdicionJuego();
  if (!HayBiblioteca(ed) && shaders::PuedeGenerar()) {
    jugar_al_terminar_ = true;
    ArrancarTarea(Tarea::kShaders);
    return;
  }

  PonerCv("fullscreen", pantalla_completa_ ? "true" : "false");
  callbacks_.guardar();
  // Copia de otra edicion: la juega su ejecutable, que esta al lado de este.
  if (!ed.empty() && ed != edicion::Propia()) {
    const auto exe = edicion::Ejecutable(ed);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exe, ec)) {
      std::lock_guard<std::mutex> l(mutex_);
      error_ = std::string(T("ed.falta_exe")) + " " + rex::path_to_utf8(exe.filename());
      return;
    }
    if (sistema::Lanzar(exe, "--nfsmw_lanzador_omitir=true")) callbacks_.salir();
    return;
  }
  if (NecesitaReinicio()) {
    if (sistema::Relanzar("--nfsmw_lanzador_omitir=true")) {
      callbacks_.salir();
      return;
    }
    // Sin reinicio posible se arranca con lo que hay; lo pendiente valdra la proxima vez.
  }

  const auto carpeta = CarpetaJuego();
  // Resolucion interna y limite de fps al modo de video, ahora que estan decididos.
  nfsmw::ajustes::AplicarAjustesGraficos();
  if (callbacks_.pantalla_completa) callbacks_.pantalla_completa(pantalla_completa_);
  auto jugar = callbacks_.jugar;
  Close();
  // Arrancar el juego en mitad del fotograma de ImGui no es seguro: despues.
  callbacks_.diferir([jugar, carpeta] { jugar(carpeta); });
}

// ---- Dibujo -----------------------------------------------------------------

void Lanzador::OnDraw(ImGuiIO& io) {
  if (tarea_terminada_.exchange(false)) {
    if (hilo_.joinable()) hilo_.join();
    tarea_ = Tarea::kNinguna;
    bool ok;
    {
      std::lock_guard<std::mutex> l(mutex_);
      ok = error_.empty() && !cancelar_;
    }
    const bool seguir = jugar_al_terminar_ && ok;
    jugar_al_terminar_ = false;
    if (seguir) {
      Jugar();
      if (tarea_ == Tarea::kNinguna) return;  // el lanzador ya se cerro
    }
  }

  escala_ = std::clamp(io.DisplaySize.y / 900.0f, 0.75f, 2.5f);
  g_escala = escala_;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(4));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(7)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(10)));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(4));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(10));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, C(kFondo));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, C(IM_COL32(0, 0, 0, 0)));
  ImGui::PushStyleColor(ImGuiCol_Text, C(kTexto));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, C(kCampo));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, C(kHover));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, C(kHover));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, C(kTarjeta));
  ImGui::PushStyleColor(ImGuiCol_Border, C(kBordeCampo));
  ImGui::PushStyleColor(ImGuiCol_Header, C(kHover));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, C(kBorde));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, C(kBorde));
  ImGui::PushStyleColor(ImGuiCol_Button, C(kHover));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C(kBorde));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, C(kResalte));
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, C(kResalteHover));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, C(IM_COL32(0, 0, 0, 0)));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, C(kBordeCampo));
  constexpr int kColores = 17;

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##nfsmw_lanzador", nullptr, flags)) {
    Fuente(false, 16.5f);
    const float ancho = io.DisplaySize.x, alto = io.DisplaySize.y;
    if (tarea_ != Tarea::kNinguna) {
      DibujarTarea();
    } else {
      const float lateral = AnchoLateral(ancho);
      const float pie = S(84);
      DibujarLateral(alto);

      ImGui::SetCursorScreenPos(ImVec2(lateral + S(36), S(30)));
      ImGui::BeginChild("##pagina", ImVec2(ancho - lateral - S(72), alto - pie - S(46)), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoBackground);
      DibujarAviso(ancho - lateral - S(72));
      Fuente(true, 30);
      ImGui::TextUnformatted(T(kTitulos[pagina_]));
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(0, S(6)));
      switch (pagina_) {
        case kInicio: DibujarInicio(); break;
        case kGraficos: DibujarGraficos(); break;
        case kControles: DibujarControles(); break;
        case kAudio: DibujarAudio(); break;
        case kPartidas: DibujarPartidas(); break;
        case kTrucos: DibujarTrucos(); break;
        case kConfig: DibujarConfig(); break;
        default: DibujarAcerca(); break;
      }
      ImGui::EndChild();
      DibujarPie(ancho, alto);
    }
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleColor(kColores);
  ImGui::PopStyleVar(7);
}

// Aviso de version nueva, arriba de cualquier pagina hasta que se descarte.
void Lanzador::DibujarAviso(float ancho) {
  if (!version_lista_ || aviso_descartado_) return;
  std::string version, url;
  {
    std::lock_guard<std::mutex> l(mutex_);
    version = version_nueva_;
    url = url_version_;
  }
  if (version.empty()) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float alto = S(58);
  dl->AddRectFilled(p, ImVec2(p.x + ancho, p.y + alto), kTarjeta, S(6));
  dl->AddRect(p, ImVec2(p.x + ancho, p.y + alto), kResalte, S(6));
  const std::string texto = std::string(T("act.nueva")) + ": " + version + "   (" + T("act.tienes") + " " +
                            actualizacion::VersionActual() + ")";
  Fuente(true, 16);
  dl->AddText(ImVec2(p.x + S(18), p.y + (alto - ImGui::GetFontSize()) * 0.5f), kTexto, texto.c_str());
  ImGui::PopFont();
  const ImVec2 tam(S(150), S(38));
  ImGui::SetCursorScreenPos(ImVec2(p.x + ancho - S(10) - tam.x, p.y + (alto - tam.y) * 0.5f));
  if (Boton(ConId(T("btn.seguir"), "seguir").c_str(), false, tam)) aviso_descartado_ = true;
  ImGui::SetCursorScreenPos(ImVec2(p.x + ancho - S(20) - 2 * tam.x, p.y + (alto - tam.y) * 0.5f));
  if (Boton(ConId(T("btn.descargar"), "descargar").c_str(), true, tam)) sistema::Abrir(url);
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + alto + S(16)));
  ImGui::Dummy(ImVec2(0, 0));
}

void Lanzador::DibujarLateral(float alto) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float ancho = AnchoLateral(ImGui::GetIO().DisplaySize.x);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(ancho, alto), kBarra);
  dl->AddLine(ImVec2(ancho, 0), ImVec2(ancho, alto), kBorde);

  // Titulo.
  const float x = S(28);
  Fuente(true, 14);
  dl->AddText(ImVec2(x + S(1), S(34)), kSuave, "NEED FOR SPEED");
  ImGui::PopFont();
  Fuente(true, 30);
  dl->AddText(ImVec2(x, S(52)), kTexto, "MOST WANTED");
  ImGui::PopFont();
  Fuente(false, 14);
  dl->AddText(ImVec2(x, S(92)), kNota, T("app.subtitulo"));
  ImGui::PopFont();
  dl->AddRectFilled(ImVec2(x, S(122)), ImVec2(x + S(36), S(124)), kResalte);

  // Menu.
  float y = S(156);
  for (int i = 0; i < kNumPaginas; ++i) {
    const ImVec2 a(S(14), y), b(ancho - S(14), y + S(50));
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##nav", ImVec2(b.x - a.x, b.y - a.y))) pagina_ = Pagina(i);
    const bool encima = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool activa = pagina_ == i;
    if (activa || encima) dl->AddRectFilled(a, b, activa ? kTarjeta : kHover, S(4));
    if (activa) dl->AddRectFilled(ImVec2(a.x, a.y + S(12)), ImVec2(a.x + S(3), b.y - S(12)), kResalte);
    const ImU32 color = activa || encima ? kTexto : kSuave;
    DibujarIcono(dl, kIconos[i], ImVec2(a.x + S(34), a.y + S(25)), S(24), color);
    Fuente(activa, 17);
    dl->AddText(ImVec2(a.x + S(62), a.y + (S(50) - ImGui::GetFontSize()) * 0.5f), color, T(kTitulos[i]));
    ImGui::PopFont();
    y += S(54);
  }
}

void Lanzador::DibujarPie(float ancho, float alto) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lateral = AnchoLateral(ancho);
  const float pie = S(84);
  const float y0 = alto - pie;
  dl->AddRectFilled(ImVec2(lateral + 1, y0), ImVec2(ancho, alto), kBarra);
  dl->AddLine(ImVec2(lateral + 1, y0), ImVec2(ancho, y0), kBorde);

  const Estado estado = EstadoJuego();
  const bool puede = estado == Estado::kIsoNueva || estado == Estado::kIsoLista || estado == Estado::kCarpeta;
  const bool reinicio = NecesitaReinicio();

  const char* aviso = !puede ? T("aviso.sin_juego") : reinicio ? T("aviso.reinicio") : "";
  if (*aviso) {
    Fuente(false, 15);
    dl->AddText(ImVec2(lateral + S(36), y0 + (pie - ImGui::GetFontSize()) * 0.5f), kAviso, aviso);
    ImGui::PopFont();
  }

  const ImVec2 tam_jugar(S(230), S(52)), tam_salir(S(120), S(52));
  const float yb = y0 + (pie - tam_jugar.y) * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(ancho - S(36) - tam_jugar.x, yb));
  Fuente(true, reinicio ? 16 : 21);
  if (Boton(ConId(reinicio ? T("btn.reiniciar") : T("btn.jugar"), "jugar").c_str(), true, tam_jugar, puede) ||
      (puede && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
    Jugar();
  }
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(ancho - S(36) - tam_jugar.x - S(12) - tam_salir.x, yb));
  if (Boton(ConId(T("btn.salir"), "salir").c_str(), false, tam_salir)) callbacks_.salir();
}

// Cabecera del inicio. La imagen no va en el repositorio (es arte del juego): se lee de portada.png o
// portada.jpg junto al ejecutable, y si no esta no se dibuja nada.
void Lanzador::DibujarPortada() {
  if (!portada_leida_) {
    portada_leida_ = true;
    auto* inmediato = imgui_drawer() ? imgui_drawer()->immediate_drawer() : nullptr;
    for (const char* nombre : {"portada.png", "portada.jpg"}) {
      std::ifstream in(rex::filesystem::GetExecutableFolder() / nombre, std::ios::binary);
      if (!in || !inmediato) continue;
      const std::vector<uint8_t> datos((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      int w = 0, h = 0;
      const auto rgba = rex::ui::DecodeImageRGBA(datos.data(), datos.size(), w, h);
      if (rgba.empty()) continue;
      portada_ = inmediato->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                          rgba.data());
      portada_ancho_ = w;
      portada_alto_ = h;
      break;
    }
  }
  if (!portada_) return;
  // Todo el ancho de la pagina; la imagen se recorta por arriba y por abajo para llenarlo sin deformarse.
  const float ancho = ImGui::GetContentRegionAvail().x;
  const float alto = std::min(ancho * 0.32f, S(300));
  const float aspecto_img = float(portada_ancho_) / float(portada_alto_), aspecto = ancho / alto;
  ImVec2 uv0(0, 0), uv1(1, 1);
  if (aspecto_img > aspecto) {
    const float sobra = (1.0f - aspecto / aspecto_img) * 0.5f;
    uv0.x = sobra, uv1.x = 1.0f - sobra;
  } else {
    const float sobra = (1.0f - aspecto_img / aspecto) * 0.5f;
    uv0.y = sobra, uv1.y = 1.0f - sobra;
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddImageRounded(reinterpret_cast<ImTextureID>(portada_.get()), p,
                                              ImVec2(p.x + ancho, p.y + alto), uv0, uv1, IM_COL32_WHITE, S(6));
  ImGui::Dummy(ImVec2(ancho, alto + S(10)));
}

void Lanzador::DibujarInicio() {
  DibujarPortada();
  {
    Tarjeta t(T("juego.titulo"));
    const std::string guardado = REXCVAR_GET(nfsmw_juego);
    const auto juego = guardado.empty() ? juego_por_defecto_ : DesdeUtf8(guardado);
    const std::string ruta = juego.empty() ? T("juego.ninguno") : rex::path_to_utf8(juego);

    // La ruta en un campo, y los dos botones a su derecha.
    const float botones = S(170) * 2 + S(10);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float campo = std::max(S(120), g_interior - botones - S(10));
    const float alto = S(42);
    dl->AddRectFilled(p, ImVec2(p.x + campo, p.y + alto), kCampo, S(4));
    dl->AddRect(p, ImVec2(p.x + campo, p.y + alto), kBordeCampo, S(4));
    dl->PushClipRect(p, ImVec2(p.x + campo - S(10), p.y + alto), true);
    dl->AddText(ImVec2(p.x + S(12), p.y + (alto - ImGui::GetFontSize()) * 0.5f), juego.empty() ? kNota : kTexto,
                ruta.c_str());
    dl->PopClipRect();

    const auto inicial = juego.empty() ? rex::filesystem::GetExecutableFolder() : juego.parent_path();
    ImGui::SetCursorScreenPos(ImVec2(p.x + campo + S(10), p.y));
    if (Boton(ConId(T("btn.iso"), "iso").c_str(), false, ImVec2(S(170), alto))) {
      const auto elegida = sistema::ElegirIso(inicial);
      if (!elegida.empty()) PonerCv("nfsmw_juego", rex::path_to_utf8(elegida));
    }
    ImGui::SameLine(0, S(10));
    if (Boton(ConId(T("btn.carpeta"), "carpeta").c_str(), false, ImVec2(S(170), alto))) {
      const auto elegida = sistema::ElegirCarpeta(inicial);
      if (!elegida.empty()) PonerCv("nfsmw_juego", rex::path_to_utf8(elegida));
    }
    Separador();
    Nota(T("juego.nota"));
  }
  {
    Tarjeta t(T("estado.titulo"));
    const char* texto = "";
    ImU32 color = kTexto;
    switch (EstadoJuego()) {
      case Estado::kNinguno: texto = T("juego.ninguno"), color = kAviso; break;
      case Estado::kFalta: texto = T("estado.falta"), color = kAviso; break;
      case Estado::kInvalida: texto = T("estado.invalida"), color = kAviso; break;
      case Estado::kIsoNueva: texto = T("estado.iso_nueva"), color = kSuave; break;
      case Estado::kIsoLista: texto = T("estado.iso_lista"); break;
      case Estado::kCarpeta: texto = T("estado.carpeta"); break;
    }
    Fila(T("estado.juego"));
    TextoColor(color, texto);

    // Edicion de la copia: la detecta el SHA-256 de su default.xex.
    if (!CarpetaJuego().empty()) {
      const std::string ed = EdicionJuego();
      Fila(T("estado.edicion"));
      if (ed.empty()) TextoColor(kAviso, T("ed.desconocida"));
      else TextoColor(kTexto, edicion::Nombre(ed));
    }

    Fila(T("estado.shaders"));
    if (HayBiblioteca(EdicionJuego())) {
      TextoColor(kTexto, T("shaders.listos"));
    } else {
      TextoColor(shaders::PuedeGenerar() ? kSuave : kAviso,
                 shaders::PuedeGenerar() ? T("shaders.faltan") : T("shaders.sin_dxc"));
    }

    const int perfil = perfiles::Detectar();
    const std::string config = std::string(Cv("nfsmw_renderizador") == "nativo" ? T("graf.nativo") : T("graf.emulado")) +
                               "  ·  " + Cv("nfsmw_resolucion_interna") + "  ·  " + Cv("nfsmw_limite_fps") +
                               " fps  ·  " + T(perfiles::Clave(perfil));
    Fila(T("estado.config"));
    TextoColor(kTexto, config.c_str());
    {
      std::lock_guard<std::mutex> l(mutex_);
      if (!error_.empty()) {
        Fila(T("shaders.error"));
        TextoColor(kAviso, error_.c_str());
      }
    }
    Separador();
    Nota(T("inicio.atajos"));
  }
}

void Lanzador::DibujarGraficos() {
  {
    Tarjeta t(T("perfil.titulo"));
    std::vector<const char*> nombres;
    for (int i = 0; i < perfiles::kNumPerfiles; ++i) nombres.push_back(T(perfiles::Clave(i)));
    int sel = perfiles::Detectar();
    const float ancho = Fila(sel == perfiles::kPersonalizado ? T("perfil.personalizado") : T(perfiles::Clave(sel)));
    if (Segmentado("perfil", nombres, &sel, ancho)) perfiles::Aplicar(sel);
    Nota(T("perfil.nota"));
    Separador();
    if (Boton(ConId(T("btn.xbox360"), "xbox360").c_str(), false, ImVec2(S(300), S(42)))) perfiles::AplicarXbox360();
    Nota(T("graf.xbox360_nota"));
  }
  {
    Tarjeta t(T("graf.motor"));
    SegCvar(T("graf.renderizador"), "nfsmw_renderizador", {{"@graf.nativo", "nativo"}, {"@graf.emulado", "xenos"}});
    Nota(T("graf.renderizador_nota"));
  }
  {
    Tarjeta t(T("graf.pantalla"));
    Interruptor("pantalla_completa", T("graf.completa"), &pantalla_completa_);
    {
      // Hasta la de la pantalla: por encima solo se gasta GPU en pixeles que luego se encogen.
      static const sistema::Resolucion pantalla = sistema::ResolucionPantalla();
      std::vector<Opcion> resoluciones = {{"1024 × 576", "1024x576"}, {"1280 × 720", "1280x720"},
                                          {"1920 × 1080", "1920x1080"}};
      if (pantalla.alto >= 1440) resoluciones.push_back({"2560 × 1440 (exp.)", "2560x1440"});
      if (pantalla.alto >= 2160) resoluciones.push_back({"3840 × 2160 (exp.)", "3840x2160"});
      ComboCvar(T("graf.resolucion"), "nfsmw_resolucion_interna", resoluciones);
      if (pantalla.alto >= 1440) Nota(T("graf.resolucion_nota"));
    }
    SegCvar(T("graf.limite"), "nfsmw_limite_fps",
            {{"@graf.limite_30", "30"}, {"@graf.limite_60", "60"}, {"@graf.limite_120", "120"},
             {"@graf.limite_ilimitado", "ilimitado"}});
    Nota(T("graf.limite_nota"));
  }
  {
    Tarjeta t(T("graf.imagen"));
    SegCvar(T("graf.aa"), "nfsmw_antialiasing", {{"@val.apagado", "apagado"}, {"FXAA", "fxaa"}});
    ComboCvar(T("graf.aniso"), "nfsmw_nativo_anisotropico",
              {{"@val.apagado", "0"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}, {"16x", "16"}});
    SegCvar(T("graf.cielo"), "nfsmw_resplandor_cielo",
            {{"@val.original", "original"}, {"@val.natural", "natural"}, {"@val.suave", "suave"}});
    SegCvar(T("graf.filtro"), "nfsmw_tratamiento_visual",
            {{"@val.original", "original"}, {"@val.suave", "suave"}, {"@val.apagado", "apagado"}});
    ComboCvar(T("graf.posproceso"), "nfsmw_posproceso",
              {{"@val.apagado", "apagado"}, {"@pp.cine", "cine"}, {"@pp.sepia", "sepia"}, {"@pp.noir", "noir"},
               {"@pp.frio", "frio"}, {"@pp.calido", "calido"}, {"@pp.vivo", "vivo"}, {"@pp.matrix", "matrix"},
               {"@pp.crt", "crt"}});
  }
  {
    Tarjeta t(T("graf.detalle"));
    // Sombras proyectadas: nfsmw_nativo_omitir_sombras quita el pase entero, siempre, sin mirar el rendimiento.
    BoolCvar(T("graf.sombras"), "nfsmw_nativo_omitir_sombras", true);
    ComboCvar(T("graf.sombras_dist"), "nfsmw_sombras_corte",
              {{"@val.corta", "250"}, {"@val.media", "150"}, {"@val.normal", "100"}});
    SegCvar(T("graf.sombras_res"), "nfsmw_nativo_sombras_escala", {{"@val.media", "64"}, {"@val.alta", "100"}});
    BoolCvar(T("graf.sombras_veg"), "nfsmw_sombras_sin_vegetacion", true);
    ComboCvar(T("graf.reflejos"), "nfsmw_reflejo_refresco",
              {{"@val.cada8", "8"}, {"@val.cada4", "4"}, {"@val.cada2", "2"}, {"@val.cada1", "1"}});
    SegCvar(T("graf.coche"), "nfsmw_cubemap_caras_max",
            {{"@perfil.bajo", "1"}, {"@perfil.medio", "2"}, {"@val.completo", "-1"}});
    SegCvar(T("graf.distancia"), "nfsmw_escena_detalle_minimo", {{"@val.reducida", "6"}, {"@val.normal", "0"}});
  }
}

// ---- Mapeo de controles ------------------------------------------------------
namespace {

// Las acciones del mando de Xbox 360, con el cvar de teclado (mnk_input_driver.cpp) y su valor de serie.
struct Accion {
  const char* etiqueta;  // "@clave" o texto
  const char* cvar;
  const char* defecto;
};
constexpr Accion kAccionesTeclado[] = {
    {"@tk.rt", "keybind_right_trigger", "E,O"},
    {"@tk.lt", "keybind_left_trigger", "Q,I"},
    {"@tk.izq", "keybind_lstick_left", "A"},
    {"@tk.der", "keybind_lstick_right", "D"},
    {"@tk.arriba", "keybind_lstick_up", "W"},
    {"@tk.abajo", "keybind_lstick_down", "S"},
    {"A", "keybind_a", "Semicolon,Space"},
    {"B", "keybind_b", "Quote,Backspace"},
    {"X", "keybind_x", "L"},
    {"Y", "keybind_y", "P"},
    {"@tk.lb", "keybind_left_shoulder", "1"},
    {"@tk.rb", "keybind_right_shoulder", "3"},
    {"Start", "keybind_start", "X,Return"},
    {"Back", "keybind_back", "Z,Tab"},
    {"@tk.pulsarizq", "keybind_lstick_press", "F"},
    {"@tk.pulsarder", "keybind_rstick_press", "K"},
    {"@tk.camara_arriba", "keybind_rstick_up", "Up"},
    {"@tk.camara_abajo", "keybind_rstick_down", "Down"},
    {"@tk.camara_izq", "keybind_rstick_left", "Left"},
    {"@tk.camara_der", "keybind_rstick_right", "Right"},
    {"@tk.cruceta_arriba", "keybind_dpad_up", "Shift+Up"},
    {"@tk.cruceta_abajo", "keybind_dpad_down", "Shift+Down"},
    {"@tk.cruceta_izq", "keybind_dpad_left", "Shift+Left"},
    {"@tk.cruceta_der", "keybind_dpad_right", "Shift+Right"},
};

// Tecla de ImGui -> nombre que entiende el SDK (rex/ui/keybinds.cpp).
const char* NombreTecla(ImGuiKey k) {
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z) {
    static const char* kLetras[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    return kLetras[k - ImGuiKey_A];
  }
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9) {
    static const char* kDigitos[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    return kDigitos[k - ImGuiKey_0];
  }
  if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) {
    static const char* kNum[] = {"Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4",
                                 "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9"};
    return kNum[k - ImGuiKey_Keypad0];
  }
  if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) {
    static const char* kF[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    return kF[k - ImGuiKey_F1];
  }
  switch (k) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter: return "Return";
    case ImGuiKey_KeypadEnter: return "NumpadEnter";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_Insert: return "Insert";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp";
    case ImGuiKey_PageDown: return "PageDown";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: return "Shift";
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: return "Control";
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: return "Alt";
    case ImGuiKey_Apostrophe: return "Quote";
    case ImGuiKey_Comma: return "Comma";
    case ImGuiKey_Minus: return "Minus";
    case ImGuiKey_Period: return "Period";
    case ImGuiKey_Slash: return "Slash";
    case ImGuiKey_Semicolon: return "Semicolon";
    case ImGuiKey_Equal: return "Plus";
    case ImGuiKey_LeftBracket: return "LBracket";
    case ImGuiKey_Backslash: return "Backslash";
    case ImGuiKey_RightBracket: return "RBracket";
    case ImGuiKey_GraveAccent: return "Backtick";
    default: return nullptr;
  }
}

// "Semicolon,Space" -> "; / Espacio": lo que se ve en el boton.
std::string Legible(const std::string& v) {
  if (v.empty()) return T("k.sinasignar");
  std::string r;
  size_t i = 0;
  while (true) {
    const size_t coma = v.find(',', i);
    const std::string t = v.substr(i, coma == std::string::npos ? std::string::npos : coma - i);
    if (!r.empty()) r += "  /  ";
    if (t == "Space") r += T("k.espacio");
    else if (t == "Backspace") r += T("k.retroceso");
    else if (t == "Semicolon") r += ";";
    else if (t == "Quote") r += "'";
    else if (t == "Return") r += "Enter";
    else r += t;
    if (coma == std::string::npos) break;
    i = coma + 1;
  }
  return r;
}

// Fila con la etiqueta a la izquierda y un boton que, al pulsarlo, espera la nueva asignacion.
bool FilaAsignable(const char* etiqueta, const std::string& valor, const char* id, bool esperando) {
  const float ancho = Fila(etiqueta);
  const std::string texto = esperando ? std::string(T("ctl.pulsa")) : valor;
  return Boton(ConId(texto.c_str(), id).c_str(), esperando, ImVec2(std::min(ancho, S(320)), S(34)));
}

const char* g_tecla_esperada = nullptr;  // cvar de la accion que espera una tecla

void DibujarMapaTeclado() {
  Tarjeta t(T("ctl.mapa"));
  if (g_tecla_esperada) {
    // Escape cancela; Supr deja la accion sin tecla.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      g_tecla_esperada = nullptr;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
      PonerCv(g_tecla_esperada, "");
      g_tecla_esperada = nullptr;
    } else {
      for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (!ImGui::IsKeyPressed(ImGuiKey(k), false)) continue;
        const char* nombre = NombreTecla(ImGuiKey(k));
        if (!nombre) continue;
        // Shift + tecla, como la cruceta de serie.
        const bool con_mayus = std::strcmp(nombre, "Shift") != 0 && ImGui::GetIO().KeyShift;
        PonerCv(g_tecla_esperada, con_mayus ? std::string("Shift+") + nombre : std::string(nombre));
        g_tecla_esperada = nullptr;
        break;
      }
    }
  }
  for (const auto& a : kAccionesTeclado) {
    if (FilaAsignable(Et(a.etiqueta), Legible(Cv(a.cvar)), a.cvar, g_tecla_esperada == a.cvar))
      g_tecla_esperada = g_tecla_esperada == a.cvar ? nullptr : a.cvar;
  }
  Separador();
  if (Boton(ConId(T("btn.restaurar_teclas"), "rest_teclas").c_str(), false, ImVec2(S(260), S(40)))) {
    for (const auto& a : kAccionesTeclado) PonerCv(a.cvar, a.defecto);
    g_tecla_esperada = nullptr;
  }
  Nota(T("ctl.mapa_nota"));
}

// Botones del mando y del volante: nombre en mando_mapa / volante_botones y lo que se ve.
struct BotonMando {
  const char* nombre;
  const char* etiqueta;
};
constexpr BotonMando kBotonesMando[] = {
    {"a", "A"},
    {"b", "B"},
    {"x", "X"},
    {"y", "Y"},
    {"lb", "@tk.lb"},
    {"rb", "@tk.rb"},
    {"start", "Start"},
    {"back", "Back"},
    {"ls", "@tk.pulsarizq"},
    {"rs", "@tk.pulsarder"},
    {"up", "@tk.cruceta_arriba"},
    {"down", "@tk.cruceta_abajo"},
    {"left", "@tk.cruceta_izq"},
    {"right", "@tk.cruceta_der"},
};

// "a=b,x=y" <-> lista de pares.
using Pares = std::vector<std::pair<std::string, std::string>>;
Pares LeerPares(const std::string& v) {
  Pares r;
  size_t i = 0;
  while (i < v.size()) {
    size_t coma = v.find(',', i);
    if (coma == std::string::npos) coma = v.size();
    const std::string par = v.substr(i, coma - i);
    const size_t igual = par.find('=');
    if (igual != std::string::npos) r.push_back({par.substr(0, igual), par.substr(igual + 1)});
    i = coma + 1;
  }
  return r;
}
std::string UnirPares(const Pares& pares) {
  std::string r;
  for (const auto& [a, b] : pares) r += (r.empty() ? "" : ",") + a + "=" + b;
  return r;
}

void DibujarMapaMando() {
  Tarjeta t(T("ctl.mapa_mando"));
  auto pares = LeerPares(Cv("mando_mapa"));
  for (const auto& b : kBotonesMando) {
    // Lo que hace ahora el boton fisico b: lo mismo si no esta en la lista.
    std::string actual = b.nombre;
    for (const auto& [de, a] : pares)
      if (de == b.nombre) actual = a;
    const float ancho = Fila(Et(b.etiqueta));
    const char* mostrado = actual.c_str();
    for (const auto& o : kBotonesMando)
      if (actual == o.nombre) mostrado = Et(o.etiqueta);
    ImGui::SetNextItemWidth(std::min(ancho, S(320)));
    ImGui::PushID(b.nombre);
    if (ImGui::BeginCombo("##mando", mostrado)) {
      for (const auto& o : kBotonesMando) {
        if (ImGui::Selectable(Et(o.etiqueta), actual == o.nombre)) {
          std::erase_if(pares, [&](const auto& p) { return p.first == b.nombre; });
          if (std::strcmp(o.nombre, b.nombre) != 0) pares.push_back({b.nombre, o.nombre});
          PonerCv("mando_mapa", UnirPares(pares));
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
  Separador();
  if (Boton(ConId(T("btn.restaurar_mando"), "rest_mando").c_str(), false, ImVec2(S(260), S(40))))
    PonerCv("mando_mapa", "");
  Nota(T("ctl.mapa_mando_nota"));
}

// Lectura en vivo del volante, para ver que eje es cada cosa y asignar botones pulsandolos.
struct Volante {
  bool iniciado = false;
  SDL_Joystick* joy = nullptr;
  std::string nombre;
  std::vector<int16_t> reposo;      // ejes al empezar a detectar
  const char* esperando = nullptr;  // cvar de eje, o nombre de boton del juego
  bool esperando_eje = false;
};
Volante g_volante;

void AbrirVolante() {
  if (!g_volante.iniciado) {
    g_volante.iniciado = SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
    if (!g_volante.iniciado) return;
  }
  SDL_UpdateJoysticks();
  if (g_volante.joy && SDL_JoystickConnected(g_volante.joy)) return;
  g_volante.joy = nullptr;
  int n = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&n);
  for (int i = 0; ids && i < n; ++i) {
    if (SDL_IsGamepad(ids[i])) continue;  // los mandos van por el otro camino
    g_volante.joy = SDL_OpenJoystick(ids[i]);
    if (g_volante.joy) {
      const char* nombre = SDL_GetJoystickName(g_volante.joy);
      g_volante.nombre = nombre ? nombre : "";
      break;
    }
  }
  SDL_free(ids);
}

void DibujarVolante() {
  Tarjeta t(T("ctl.volante"));
  BoolCvar(T("ctl.volante_activo"), "volante_activo");
  AbrirVolante();
  SDL_Joystick* joy = g_volante.joy;
  Fila(T("ctl.volante_dispositivo"));
  TextoColor(joy ? kTexto : kNota, joy ? g_volante.nombre.c_str() : T("ctl.volante_ninguno"));
  if (!joy) {
    Nota(T("ctl.volante_nota"));
    return;
  }
  const int ejes = SDL_GetNumJoystickAxes(joy), botones = SDL_GetNumJoystickButtons(joy);

  // Asignacion pendiente: el eje que mas se aleja del reposo, o el primer boton que se pulse.
  if (g_volante.esperando && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    g_volante.esperando = nullptr;
  } else if (g_volante.esperando && g_volante.esperando_eje) {
    int mejor = -1, distancia = 12000;
    for (int e = 0; e < ejes && e < int(g_volante.reposo.size()); ++e) {
      const int d = std::abs(int(SDL_GetJoystickAxis(joy, e)) - g_volante.reposo[size_t(e)]);
      if (d > distancia) distancia = d, mejor = e;
    }
    if (mejor >= 0) {
      PonerCv(g_volante.esperando, std::to_string(mejor));
      g_volante.esperando = nullptr;
    }
  } else if (g_volante.esperando) {
    for (int b = 0; b < botones; ++b) {
      if (!SDL_GetJoystickButton(joy, b)) continue;
      const std::string numero = std::to_string(b);
      auto pares = LeerPares(Cv("volante_botones"));
      std::erase_if(pares, [&](const auto& p) { return p.first == g_volante.esperando || p.second == numero; });
      pares.push_back({g_volante.esperando, numero});
      PonerCv("volante_botones", UnirPares(pares));
      g_volante.esperando = nullptr;
      break;
    }
  }

  // Ejes: el elegido para cada funcion, con "Detectar" y su barra en vivo.
  struct Eje {
    const char* etiqueta;
    const char* cvar;
  };
  constexpr Eje kEjes[] = {{"ctl.volante_dir", "volante_eje_direccion"},
                           {"ctl.volante_acel", "volante_eje_acelerador"},
                           {"ctl.volante_freno", "volante_eje_freno"}};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (const auto& e : kEjes) {
    const int eje = std::atoi(Cv(e.cvar).c_str());
    const bool espera = g_volante.esperando == e.cvar;
    const std::string valor = espera ? std::string(T("ctl.mueve")) : std::string(T("ctl.eje")) + " " + std::to_string(eje);
    const float x0 = ImGui::GetCursorPosX();
    if (FilaAsignable(T(e.etiqueta), valor, e.cvar, espera)) {
      g_volante.esperando = espera ? nullptr : e.cvar;
      g_volante.esperando_eje = true;
      g_volante.reposo.clear();
      for (int i = 0; i < ejes; ++i) g_volante.reposo.push_back(SDL_GetJoystickAxis(joy, i));
    }
    // Barra con la posicion del eje, a la derecha del boton.
    ImGui::SameLine(0, S(12));
    const float v = eje < ejes ? (SDL_GetJoystickAxis(joy, eje) + 32768.0f) / 65535.0f : 0.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(S(40), x0 + g_interior - ImGui::GetCursorPosX());
    const float y = p.y + S(13), h = S(8);
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), kCampo, S(4));
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * v, y + h), kSuave, S(4));
    ImGui::Dummy(ImVec2(w, S(34)));
  }
  BoolCvar(T("ctl.volante_invertir"), "volante_pedales_invertidos");
  {
    const float ancho = Fila(T("ctl.volante_giro"));
    int giro = std::atoi(Cv("volante_giro").c_str());
    ImGui::SetNextItemWidth(ancho);
    if (ImGui::SliderInt("##giro", &giro, 10, 100, "%d %%")) PonerCv("volante_giro", std::to_string(giro));
  }
  {
    const float ancho = Fila(T("ctl.volante_muerta"));
    int muerta = std::atoi(Cv("volante_zona_muerta").c_str());
    ImGui::SetNextItemWidth(ancho);
    if (ImGui::SliderInt("##muerta", &muerta, 0, 30, "%d %%")) PonerCv("volante_zona_muerta", std::to_string(muerta));
  }

  // Botones: se pulsa el del volante para asignarlo.
  Separador();
  const auto pares = LeerPares(Cv("volante_botones"));
  for (const auto& b : kBotonesMando) {
    std::string actual = T("k.sinasignar");
    for (const auto& [boton, numero] : pares)
      if (boton == b.nombre) actual = std::string(T("ctl.boton_volante")) + " " + numero;
    const bool espera = g_volante.esperando == b.nombre && !g_volante.esperando_eje;
    if (FilaAsignable(Et(b.etiqueta), actual, b.nombre, espera)) {
      g_volante.esperando = espera ? nullptr : b.nombre;
      g_volante.esperando_eje = false;
    }
  }
  Separador();
  if (Boton(ConId(T("btn.restaurar_volante"), "rest_volante").c_str(), false, ImVec2(S(260), S(40)))) {
    for (const char* c : {"volante_eje_direccion", "volante_eje_acelerador", "volante_eje_freno",
                          "volante_pedales_invertidos", "volante_giro", "volante_zona_muerta", "volante_botones"}) {
      if (const auto* info = rex::cvar::GetFlagInfo(c)) PonerCv(c, info->default_value);
    }
    g_volante.esperando = nullptr;
  }
  Nota(T("ctl.volante_nota"));
}

}  // namespace

void Lanzador::DibujarControles() {
  {
    Tarjeta t(T("ctl.teclado"));
    BoolCvar(T("ctl.jugar_teclado"), "mnk_mode");
    BoolCvar(T("ctl.raton"), "mnk_mouse");
    const float ancho = Fila(T("ctl.sensibilidad"));
    float sens = float(std::atof(Cv("mnk_sensitivity").c_str()));
    ImGui::SetNextItemWidth(ancho);
    if (ImGui::SliderFloat("##sens", &sens, 0.1f, 5.0f, "%.2fx")) PonerCv("mnk_sensitivity", std::to_string(sens));
    Nota(T("ctl.teclado_nota"));
  }
  {
    Tarjeta t(T("ctl.mando"));
    SegCvar(T("ctl.entrada"), "input_backend", {{"@ctl.sdl", "sdl"}, {"XInput", "xinput"}});
    BoolCvar(T("ctl.guia"), "guide_button");
    Nota(T("ctl.mando_nota"));
  }
  DibujarMapaTeclado();
  DibujarMapaMando();
  DibujarVolante();
}

void Lanzador::DibujarAudio() {
  Tarjeta t(T("audio.titulo"));
  BoolCvar(T("audio.silenciar"), "audio_mute");
  Nota(T("audio.nota"));
}

void Lanzador::DibujarPartidas() {
  if (!partidas_leidas_) {
    partidas_ = partidas::Listar();
    partidas_leidas_ = true;
  }
  // Tras cada accion se vuelve a leer la lista, y el resultado queda escrito abajo.
  auto hecho = [this](const std::string& error) {
    mensaje_partidas_ = error.empty() ? T("part.ok") : error;
    partidas_leidas_ = false;
    borrar_ = -1;
  };
  {
    Tarjeta t(T("part.titulo"));
    if (partidas_.empty()) TextoColor(kNota, T("part.ninguna"));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tam(S(150), S(36));
    for (size_t i = 0; i < partidas_.size(); ++i) {
      const auto& p = partidas_[i];
      ImGui::PushID(int(i));
      const ImVec2 a = ImGui::GetCursorScreenPos();
      const float alto = S(96);
      dl->AddRectFilled(a, ImVec2(a.x + g_interior, a.y + alto), kCampo, S(4));
      dl->AddRect(a, ImVec2(a.x + g_interior, a.y + alto), kBorde, S(4));
      DibujarIcono(dl, Icono::kPartidas, ImVec2(a.x + S(30), a.y + S(30)), S(26), kTexto);
      Fuente(true, 18);
      dl->AddText(ImVec2(a.x + S(58), a.y + S(14)), kTexto, p.perfil.c_str());
      ImGui::PopFont();
      const std::string datos = std::string(T("part.fecha")) + ": " + p.fecha + "   ·   " + T("part.tam") + ": " +
                                Tamano(p.bytes);
      Fuente(false, 14);
      dl->AddText(ImVec2(a.x + S(58), a.y + S(40)), kSuave, datos.c_str());
      ImGui::PopFont();
      float x = a.x + S(58);
      const float y = a.y + alto - tam.y - S(10);
      auto boton = [&](const char* texto, const char* id, bool primario, bool activo = true) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        x += tam.x + S(8);
        return Boton(ConId(texto, id).c_str(), primario, tam, activo);
      };
      if (borrar_ == int(i)) {
        Fuente(false, 14);
        dl->AddText(ImVec2(x, y - S(20)), kAviso, T("part.confirmar"));
        ImGui::PopFont();
        if (boton(T("btn.confirmar"), "si", true)) hecho(partidas::Borrar(p));
        if (boton(T("btn.cancelar"), "no", false)) borrar_ = -1;
      } else {
        if (boton(T("btn.exportar"), "exp", false)) {
          const auto destino = sistema::ElegirCarpeta(rex::filesystem::GetExecutableFolder());
          if (!destino.empty()) hecho(partidas::Exportar(p, destino));
        }
        if (boton(T("btn.restaurar"), "rest", false, p.hay_anterior)) hecho(partidas::RestaurarAnterior(p));
        if (boton(T("btn.borrar"), "borrar", false)) borrar_ = int(i);
      }
      ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + alto + S(10)));
      ImGui::Dummy(ImVec2(0, 0));
      ImGui::PopID();
      if (!partidas_leidas_) break;  // la lista cambio: se redibuja el proximo fotograma
    }
    Separador();
    if (Boton(ConId(T("btn.importar"), "importar").c_str(), true, ImVec2(S(220), S(42)))) {
      const auto origen = sistema::ElegirCarpeta(rex::filesystem::GetExecutableFolder());
      if (!origen.empty()) hecho(partidas::Importar(origen));
    }
    ImGui::SameLine(0, S(10));
    if (Boton(ConId(T("btn.guardados"), "carpeta_saves").c_str(), false, ImVec2(S(220), S(42))))
      sistema::Abrir(rex::path_to_utf8(rex::filesystem::GetExecutableFolder() / "saves"));
    ImGui::SameLine(0, S(10));
    if (Boton(ConId(T("btn.recargar"), "recargar_p").c_str(), false, ImVec2(S(140), S(42)))) partidas_leidas_ = false;
    if (!mensaje_partidas_.empty()) TextoColor(kSuave, mensaje_partidas_.c_str());
    Separador();
    Nota(T("part.nota"));
  }
}

void Lanzador::DibujarTrucos() {
  int errores = 0;
  static std::vector<trucos::Truco> lista;
  static bool leidos = false;
  static int errores_leidos = 0;
  if (!leidos) {
    lista = trucos::Cargar(&errores_leidos);
    leidos = true;
  }
  errores = errores_leidos;
  Tarjeta t(T("tr.titulo"));
  if (lista.empty()) Nota(T("tr.ninguno"));
  for (const auto& truco : lista) {
    bool activo = trucos::Activo(truco.nombre);
    if (Interruptor(truco.nombre.c_str(), truco.nombre.c_str(), &activo)) trucos::Activar(truco.nombre, activo);
    if (!truco.nota.empty()) Nota(truco.nota.c_str());
  }
  if (errores > 0) {
    const std::string texto = std::string(T("tr.errores")) + ": " + std::to_string(errores);
    TextoColor(kAviso, texto.c_str());
  }
  Separador();
  if (Boton(ConId(T("btn.editar"), "editar").c_str(), false, ImVec2(S(260), S(42))))
    sistema::Abrir(rex::path_to_utf8(trucos::RutaFichero()));
  ImGui::SameLine(0, S(10));
  if (Boton(ConId(T("btn.recargar"), "recargar_t").c_str(), false, ImVec2(S(140), S(42)))) leidos = false;
  Separador();
  Nota(T("tr.nota"));
}

void Lanzador::DibujarConfig() {
  {
    Tarjeta t(T("cfg.idioma"));
    const float ancho = Fila(T("cfg.idioma"));
    const std::string actual = Cv("nfsmw_idioma");
    static constexpr const char* kCodigos[idioma::kNumIdiomas] = {"es", "en", "pt"};
    const char* mostrado = T("cfg.idioma_auto");
    for (int i = 0; i < idioma::kNumIdiomas; ++i)
      if (actual == kCodigos[i]) mostrado = idioma::Nombre(i);
    ImGui::SetNextItemWidth(ancho);
    if (ImGui::BeginCombo("##idioma", mostrado)) {
      if (ImGui::Selectable(T("cfg.idioma_auto"), actual.empty())) idioma::Elegir(-1);
      for (int i = 0; i < idioma::kNumIdiomas; ++i)
        if (ImGui::Selectable(idioma::Nombre(i), mostrado == idioma::Nombre(i))) idioma::Elegir(i);
      ImGui::EndCombo();
    }
  }
  {
    Tarjeta t(T("cfg.inicio"));
    BoolCvar(T("cfg.lanzador"), "nfsmw_lanzador");
    Nota(T("cfg.lanzador_nota"));
  }
  {
    Tarjeta t(T("cfg.shaders"));
    const bool puede = shaders::PuedeGenerar() && !CarpetaJuego().empty();
    if (Boton(ConId(T("btn.regenerar"), "regenerar").c_str(), false, ImVec2(S(260), S(42)), puede)) {
      jugar_al_terminar_ = false;
      ArrancarTarea(Tarea::kShaders);
    }
    Nota(T("cfg.shaders_nota"));
  }
  {
    Tarjeta t(T("cfg.carpetas"));
    const auto exe = rex::filesystem::GetExecutableFolder();
    if (Boton(ConId(T("btn.logs"), "logs").c_str(), false, ImVec2(S(230), S(42))))
      sistema::Abrir(rex::path_to_utf8(exe / "logs"));
    ImGui::SameLine(0, S(10));
    if (Boton(ConId(T("btn.guardados"), "saves").c_str(), false, ImVec2(S(230), S(42))))
      sistema::Abrir(rex::path_to_utf8(exe / "saves"));
  }
}

void Lanzador::DibujarAcerca() {
  Tarjeta t("Need for Speed: Most Wanted  ·  Recompiled");
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + g_interior);
  ImGui::PushStyleColor(ImGuiCol_Text, C(kSuave));
  ImGui::TextWrapped("%s", T("acerca.texto"));
  ImGui::Dummy(ImVec2(0, S(4)));
  ImGui::TextWrapped("%s", T("acerca.base"));
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  Separador();
  Fila(T("acerca.version"));
  TextoColor(kTexto, actualizacion::VersionActual());
  Separador();
  Nota(T("acerca.legal"));
  Separador();
  if (Boton(ConId(T("btn.repo"), "repo").c_str(), false, ImVec2(S(260), S(42))))
    sistema::Abrir("https://github.com/madelrandel-blip/NFSMW-Recompiled");
}

// Pantalla de progreso de la extraccion de la ISO o de la generacion de shaders.
void Lanzador::DibujarTarea() {
  const bool shaders = tarea_ == Tarea::kShaders;
  const ImVec2 pantalla = ImGui::GetIO().DisplaySize;
  const float ancho = std::min(S(640), pantalla.x - S(40)), alto = S(260);
  const ImVec2 p((pantalla.x - ancho) * 0.5f, (pantalla.y - alto) * 0.5f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + ancho, p.y + alto), kTarjeta, S(6));
  dl->AddRect(p, ImVec2(p.x + ancho, p.y + alto), kBorde, S(6));

  const float x = p.x + S(28);
  Fuente(true, 24);
  dl->AddText(ImVec2(x, p.y + S(26)), kTexto, shaders ? T("shaders.titulo") : T("extraer.titulo"));
  ImGui::PopFont();
  Fuente(false, 15);
  dl->PushClipRect(ImVec2(x, p.y), ImVec2(p.x + ancho - S(28), p.y + alto), true);
  dl->AddText(ImVec2(x, p.y + S(64)), kNota, shaders ? T("shaders.nota") : T("extraer.nota"));
  dl->PopClipRect();
  ImGui::PopFont();

  const int hechos = hechos_, total = total_;
  const float barra_w = ancho - S(56), y = p.y + S(112);
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + barra_w, y + S(8)), kCampo, S(4));
  if (total > 0)
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + std::max(S(8), barra_w * hechos / total), y + S(8)), kResalte, S(4));

  std::string detalle;
  if (shaders) {
    static constexpr const char* kFases[] = {"fase.imagen", "fase.buscar", "fase.traducir", "fase.compilar",
                                             "fase.empaquetar"};
    detalle = T(kFases[std::clamp(fase_.load(), 0, 4)]);
    if (total > 1) detalle += "   " + std::to_string(hechos) + " / " + std::to_string(total);
  } else {
    std::lock_guard<std::mutex> l(mutex_);
    detalle = total > 0 ? std::to_string(hechos) + " / " + std::to_string(total) + "   " + detalle_ : "…";
  }
  Fuente(false, 14);
  dl->PushClipRect(ImVec2(x, y), ImVec2(x + barra_w, y + S(60)), true);
  dl->AddText(ImVec2(x, y + S(22)), kSuave, detalle.c_str());
  dl->PopClipRect();
  ImGui::PopFont();

  ImGui::SetCursorScreenPos(ImVec2(p.x + ancho - S(28) - S(140), p.y + alto - S(26) - S(44)));
  if (Boton(ConId(T("btn.cancelar"), "cancelar").c_str(), false, ImVec2(S(140), S(44)))) cancelar_ = true;
}

}  // namespace nfsmw::lanzador
