// nfsmw - in-game settings menu (GoldenEye style, opened with ESC)
//
// It relies on the SDK's ImGui dialog: when constructed it registers itself
// (ImGuiDrawer::AddDialog) and when Close() is called it closes and deletes
// itself (ImGuiDialog::Draw). The app does not need to own it: it keeps a raw
// pointer that is cleared with on_closed.

#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/debug_overlay.h>

#include <functional>

class NfsmwMenuDialog : public rex::ui::ImGuiDialog {
 public:
  struct Callbacks {
    std::function<void()> persist_config;      // SaveConfig to nfsmw.toml
    std::function<void()> request_restart;     // save + relaunch the .exe
    std::function<void()> request_quit;        // close the window
    std::function<void()> on_closed;           // notify the app (clears its pointer)
    std::function<rex::ui::FrameStats()> sample_fps;  // F3 overlay meter
  };

  NfsmwMenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks);
  ~NfsmwMenuDialog() override;

  void RequestClose();

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void Persistir();
  static void MarcaVivo(const char* texto);
  static void MarcaReinicio(const char* texto = nullptr);
  static void MarcaReinicioConAviso(const char* texto);
  static bool BotonAplicar();

  Callbacks callbacks_;
  int selected_tab_ = 0;
  bool quit_requested_ = false;

  char gamertag_[16];  // 15 characters + null, like an Xbox Live gamertag
  bool gamertag_sync_ = false;
};