// nfsmw - lanzador dentro del juego.
//
// Es la primera pantalla al abrir el ejecutable: elegir la copia del juego,
// ajustar graficos, controles, audio e idioma, y jugar. Se dibuja con ImGui
// en la propia ventana del juego, antes de arrancarlo: NfsmwApp lo abre desde
// ReXApp::OnFinalizePaths, que deja el arranque en espera hasta que el
// lanzador llama a `jugar`.
//
// Antes de arrancar se asegura de que esta todo: extrae la ISO si hace falta y
// genera la biblioteca de shaders si no existe. Lo que se crea antes de esta
// pantalla (el renderizador) no se puede cambiar en caliente: si se toca, el
// juego se reinicia solo con los ajustes ya guardados.

#pragma once

#include "partidas.h"

#include <rex/ui/imgui_dialog.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ImFont;
struct ImFontAtlas;

namespace nfsmw::lanzador {

// Fuentes del lanzador (Segoe UI en Windows). Se registran desde
// ReXApp::OnConfigureFonts; sin ellas se usa la fuente por defecto de ImGui.
void CargarFuentes(ImFontAtlas* atlas);

// El lanzador debe mostrarse: nfsmw_lanzador activado y no venimos de un
// reinicio pedido por el propio lanzador (--nfsmw_lanzador_omitir).
bool DebeMostrarse();

// La carpeta del juego con la que arrancar sin lanzador: la ISO o carpeta
// guardada en nfsmw_juego (ya extraida) o, si no hay, `por_defecto`.
std::filesystem::path CarpetaGuardada(const std::filesystem::path& por_defecto);

class Lanzador : public rex::ui::ImGuiDialog {
 public:
  struct Callbacks {
    std::function<void()> guardar;                             // nfsmw.toml
    std::function<void(std::filesystem::path carpeta)> jugar;  // arranca con esa carpeta
    std::function<void()> salir;                               // cierra la ventana
    std::function<void(std::function<void()>)> diferir;        // ejecutar tras el fotograma actual
    std::function<void(bool)> pantalla_completa;               // poner la ventana a pantalla completa o no
  };

  Lanzador(rex::ui::ImGuiDrawer* drawer, std::filesystem::path juego_por_defecto, Callbacks callbacks);
  ~Lanzador() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum Pagina { kInicio, kGraficos, kControles, kAudio, kPartidas, kTrucos, kConfig, kAcerca, kNumPaginas };
  enum class Estado { kNinguno, kFalta, kInvalida, kIsoNueva, kIsoLista, kCarpeta };
  // Trabajo en segundo plano antes de jugar.
  enum class Tarea { kNinguna, kExtraer, kShaders };

  void DibujarLateral(float alto);
  void DibujarPie(float ancho, float alto);
  void DibujarAviso(float ancho);
  void DibujarInicio();
  void DibujarGraficos();
  void DibujarControles();
  void DibujarAudio();
  void DibujarPartidas();
  void DibujarTrucos();
  void DibujarConfig();
  void DibujarAcerca();
  void DibujarTarea();

  Estado EstadoJuego() const;
  std::filesystem::path CarpetaJuego() const;  // vacia si aun no hay carpeta lista
  std::string EdicionJuego() const;             // edicion de esa copia ("es", "usa"...), vacia si no se conoce
  bool NecesitaReinicio() const;

  void Jugar();
  void ArrancarTarea(Tarea tarea);

  Callbacks callbacks_;
  std::filesystem::path juego_por_defecto_;
  Pagina pagina_ = kInicio;
  float escala_ = 1.0f;
  // Valor al abrir el lanzador de cada ajuste que exige reiniciar.
  std::vector<std::string> valores_iniciales_;
  // El lanzador siempre va en ventana. La eleccion de pantalla completa se
  // guarda aqui y solo pasa al cvar al jugar: el cvar cambia la ventana en el
  // acto (ReXApp lo vigila) y no se quiere que salte mientras se configura.
  bool pantalla_completa_ = true;

  // Tarea en segundo plano (extraer la ISO o generar los shaders).
  std::thread hilo_;
  std::atomic<Tarea> tarea_{Tarea::kNinguna};
  std::atomic<bool> cancelar_{false};
  std::atomic<bool> tarea_terminada_{false};
  std::atomic<int> fase_{0};
  std::atomic<int> hechos_{0};
  std::atomic<int> total_{0};
  std::mutex mutex_;
  std::string detalle_;  // fichero actual
  std::string error_;
  bool jugar_al_terminar_ = false;

  // Edicion detectada de la copia elegida: el SHA-256 de su default.xex, calculado una vez por copia.
  mutable std::string edicion_juego_, edicion_clave_;

  // Comprobacion de version nueva, en segundo plano al abrir.
  std::thread hilo_version_;
  std::atomic<bool> version_lista_{false};
  bool aviso_descartado_ = false;
  std::string version_nueva_, url_version_;

  // Imagen de cabecera del inicio (portada.png o .jpg junto al ejecutable; sin ella no se dibuja nada).
  void DibujarPortada();
  std::unique_ptr<rex::ui::ImmediateTexture> portada_;
  int portada_ancho_ = 0, portada_alto_ = 0;
  bool portada_leida_ = false;

  // Gestor de partidas: lo que se ve, el resultado de la ultima accion y la partida a punto de borrarse.
  bool partidas_leidas_ = false;
  std::vector<partidas::Partida> partidas_;
  std::string mensaje_partidas_;
  int borrar_ = -1;
};

}  // namespace nfsmw::lanzador
