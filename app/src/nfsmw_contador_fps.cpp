// FPS counter of our own, drawn over the game in the top-left corner (where the SDK's debug overlay goes), in the
// orange of the game's menus with a dark outline, in a plain system font (Segoe UI on Windows, Roboto on
// Android). Turned on from the in-game graphics options ("Mostrar FPS", nfsmw_fe_raton.cpp) or the cvar.

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

REXCVAR_DEFINE_BOOL(nfsmw_mostrar_fps, false, "Graficos",
                    "Contador de FPS propio en la esquina superior izquierda, con un estilo parecido al del juego");

extern std::atomic<uint64_t> g_nfsmw_fotogramas_juego;

namespace nfsmw::contador_fps {
namespace {

class Contador final : public rex::ui::ImGuiDialog {
 public:
  explicit Contador(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!REXCVAR_GET(nfsmw_mostrar_fps)) {
      tiene_anterior_ = false;
      return;
    }
    // The game's frames (counted in its Swap), every half second.
    using Reloj = std::chrono::steady_clock;
    const auto ahora = Reloj::now();
    const uint64_t total = g_nfsmw_fotogramas_juego.load(std::memory_order_relaxed);
    if (!tiene_anterior_) {
      tiene_anterior_ = true;
      anterior_ = ahora;
      fotogramas_ = total;
    } else {
      const double s = std::chrono::duration<double>(ahora - anterior_).count();
      if (s >= 0.5) {
        fps_ = double(total - fotogramas_) / s;
        anterior_ = ahora;
        fotogramas_ = total;
      }
    }

    char texto[40];
    std::snprintf(texto, sizeof(texto), "%.1f FPS (%.2f ms)", fps_, fps_ > 0.0 ? 1000.0 / fps_ : 0.0);
    // A plain system font (loaded once; ImGui 1.92 builds its glyphs on demand), the ImGui one if there is none.
    if (!fuente_cargada_) {
      fuente_cargada_ = true;
      for (const char* ruta : {"C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf",
                               "/system/fonts/Roboto-Bold.ttf", "/system/fonts/Roboto-Regular.ttf"}) {
        if (FILE* f = std::fopen(ruta, "rb")) {
          std::fclose(f);
          fuente_ = io.Fonts->AddFontFromFileTTF(ruta, 32.0f);
          if (fuente_) break;
        }
      }
    }
    ImFont* fuente = fuente_ ? fuente_ : ImGui::GetFont();
    const float tam = std::max(16.0f, io.DisplaySize.y * 0.032f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 origen(14.0f, 10.0f);
    // A dark outline around the orange of the game's menu text.
    const ImU32 borde = IM_COL32(20, 12, 0, 230);
    for (int dx = -2; dx <= 2; ++dx)
      for (int dy = -2; dy <= 2; ++dy)
        if (dx || dy) dl->AddText(fuente, tam, ImVec2(origen.x + dx, origen.y + dy), borde, texto);
    dl->AddText(fuente, tam, origen, IM_COL32(255, 186, 40, 255), texto);
  }

 private:
  bool tiene_anterior_ = false;
  std::chrono::steady_clock::time_point anterior_;
  uint64_t fotogramas_ = 0;
  double fps_ = 0.0;
  bool fuente_cargada_ = false;
  ImFont* fuente_ = nullptr;
};

}  // namespace

void Mostrar(rex::ui::ImGuiDrawer* drawer) {
  if (drawer) new Contador(drawer);  // registers itself in the drawer, which owns it
}

}  // namespace nfsmw::contador_fps
