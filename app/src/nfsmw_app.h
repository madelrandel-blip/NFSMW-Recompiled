// nfsmw - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

// Windows first, deliberately lean: rex headers do not expect windows.h to
// have left macros lying around.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "nfsmw_menu.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/keybinds.h>  // RegisterBind/UnregisterBind (menu ESC key)
#include <rex/system/kernel_state.h>  // HANG WATCHDOG
#include <rex/system/xmemory.h>       // TranslateVirtual (Black Edition patch)
#include <rex/system/xthread.h>       // HANG WATCHDOG

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>  // HANG WATCHDOG - the signature is sorted by thread id
#include <string>
#include <thread>
#include <vector>

// Cvar "Content > black_edition", defined in nfsmw_menu.cpp.
REXCVAR_DECLARE(bool, black_edition);

class NfsmwApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NfsmwApp>(new NfsmwApp(ctx, "nfsmw",
        PPCImageConfig));
  }

  // Available, unused hooks:
  //   void OnPreSetup(rex::RuntimeConfig& config) override {}
  //   void OnLoadXexImage(std::string& xex_image) override {}
  //
  // Hooks used: portable paths, mandatory settings, fps counter,
  // the Black Edition patch (OnPostLoadXexImage) and the settings menu via ESC
  // (OnCreateDialogs).

 protected:
  // ==========================================================================
  //  1. PORTABLE PATHS: find the ISO next to the .exe
  //
  //  Without this, starting without --game_data_root dies with
  //      "--game_data_root was not provided."
  //  because SetupEnvironment only looks at the cvar and, if it is empty,
  //  ConstructRuntime aborts.
  //
  //  OnConfigurePaths is called right after the PathConfig is built and
  //  before anyone uses it, so it is the place to fill the gap.
  //
  //  ORDER, WHICH MATTERS: this runs BEFORE nfsmw.toml is loaded -the SDK
  //  reads it a few lines further down, in SetupEnvironment-. So the real
  //  priority is: --game_data_root from the command line, and if not, what is
  //  found right here. Putting game_data_root in the toml does NOT work, and
  //  that is not our doing: it is how the SDK is ordered.
  //
  //  It searches, in this order:
  //    1. an .iso whose name matches the executable's
  //    2. any other .iso in the folder, in alphabetical order
  //    3. a game_root\ folder, in case someone prefers to extract it
  //
  //  (1) exists so that a folder with NFS_Most_Wanted.exe and
  //  NFS_Most_Wanted.iso works without ambiguity even if there are more
  //  images.
  // ==========================================================================
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;  // the user specified it on the command line; it wins.
    }

    std::error_code ec;
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty() || !std::filesystem::is_directory(carpeta, ec)) {
      return;
    }

    // The executable name, for the preferred case.
    std::filesystem::path preferida;
    std::vector<std::filesystem::path> otras;

    std::string yo;
    {
      const auto exe = rex::filesystem::GetExecutablePath();
      if (!exe.empty()) {
        yo = exe.stem().string();
        std::transform(yo.begin(), yo.end(), yo.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
      }
    }

    for (const auto& e : std::filesystem::directory_iterator(carpeta, ec)) {
      if (ec) break;
      if (!e.is_regular_file(ec)) continue;

      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (ext != ".iso") continue;

      std::string base = e.path().stem().string();
      std::transform(base.begin(), base.end(), base.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });

      if (!yo.empty() && base == yo) {
        preferida = e.path();
      } else {
        otras.push_back(e.path());
      }
    }

    if (!preferida.empty()) {
      paths.game_data_root = preferida;
    } else if (!otras.empty()) {
      std::sort(otras.begin(), otras.end());
      paths.game_data_root = otras.front();
    } else {
      // No ISO: an extracted folder next to it also works. The ISO patch
      // left --game_data_root accepting both.
      const auto extraida = carpeta / "game_root";
      if (std::filesystem::is_directory(extraida, ec)) {
        paths.game_data_root = extraida;
      }
    }
    // If nothing is found, it is left empty on purpose: the SDK will give its
    // own message, which is clearer than any we could put here.
  }

  // ==========================================================================
  //  2. MANDATORY SETTINGS
  //
  //  So that "NFS_Most_Wanted.exe" on its own, without a single argument,
  //  starts just as well as with the usual long command line.
  //
  //  Only settings the user has NOT set are touched: HasNonDefaultValue
  //  distinguishes "this comes from the factory" from "someone asked for
  //  this". That way the command line and nfsmw.toml still take precedence.
  //
  //  WHY IN TWO DIFFERENT PLACES
  //  The readback_resolve cvar does not exist yet when logging starts: it is
  //  registered by the GPU plugin (rexgpu-xenos.dll), which loads later, in
  //  SetupPresentation. Setting it earlier would write to a flag that does not
  //  exist yet. Hence:
  //
  //    OnPostInitLogging  -> gpu_plugin and mnk_mode, which belong to the
  //                          runtime and are already registered. And it has to
  //                          be HERE, because SetupPresentation reads
  //                          gpu_plugin right after.
  //    OnPostSetup        -> readback_resolve, once the plugin has loaded and
  //                          not a single frame has been drawn yet.
  // ==========================================================================
  void OnPostInitLogging() override {
    // Without a GPU plugin the screen stays black: the game runs, but the
    // runtime drops its graphics calls with "no GPU emulation loaded".
    PonerSiNadieLoPidio("gpu_plugin", "xenos");
    // Keyboard and mouse in addition to the gamepad.
    PonerSiNadieLoPidio("mnk_mode", "true");
  }

  void OnPostSetup() override {
    // THIS IS NOT A PREFERENCE, IT IS A FIX. The game computes its exposure
    // by measuring the average brightness of the scene and reading that value
    // back on the CPU. That readback is disabled by default ("none"), so the
    // game receives garbage, concludes the scene is pitch black and raises
    // exposure to the max: washed-out image and blown-out sun.
    PonerSiNadieLoPidio("readback_resolve", "fast");

    // FPS counter for the F3 overlay, see below.
    SetGuestFrameStats([this] { return MuestreaFotograma(); });

    // Hang watchdog, see below.
    ArrancarVigilante();
  }

  // ==========================================================================
  //  2b. BLACK EDITION PATCH (NATIVE)
  //
  //  In the PAL edition (454107D9) there is a flag at 0x82A2CE04 that decides
  //  whether to sell the paid cars (Black edition) as downloadable or not.
  //  Xenia enabled it with  data_write(be32, 0x82a2ce04, 0x00000100); here the
  //  guest memory is overwritten directly.
  //
  //  Memory managed by memoria::Memory is exposed in big-endian: offset 0 is
  //  the most significant byte. That is why writing the value as-is
  //  (0x00000100) is enough, with no endian swap: it is the same thing the
  //  .toml patch did with the patched XEX file.
  //
  //  It can be turned off from the menu (Content > Black Edition), but it only
  //  applies on the next load: this function runs every time the XEX is
  //  loaded, be it at startup or when the image is re-read.
  // ==========================================================================
  void OnPostLoadXexImage() override { AplicarParcheBlackEdition(); }

  // ==========================================================================
  //  2c. SETTINGS MENU VIA ESC
  //
  //  Just like F3 (debug) and F4 (technical settings), a key is registered and
  //  the dialog is created and destroyed with it (the SDK's ImGui dialogs
  //  register themselves in the drawer when constructed and delete themselves
  //  when closed; all that is needed is to keep the pointer and clear it with
  //  on_closed).
  //
  //  The key was assigned to Escape. It can be rebound from F4
  //  (the "Keybinds" section).
  // ==========================================================================
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::ui::RegisterBind("bind_nfsmw_menu", "Escape",
                          "Open/close the in-game settings menu",
                          [this] { AlternarMenu(); });
  }

  void OnShutdown() override {
    PararVigilante();
    rex::ui::UnregisterBind("bind_nfsmw_menu");
    if (menu_ != nullptr) {
      menu_->RequestClose();
      menu_ = nullptr;
    }
  }

 private:
  static void PonerSiNadieLoPidio(const char* nombre, const char* valor) {
    if (rex::cvar::GetFlagInfo(nombre) == nullptr) {
      REXLOG_DEBUG("Setting '{}' is not registered yet; leaving it alone.", nombre);
      return;
    }
    if (rex::cvar::HasNonDefaultValue(nombre)) {
      return;  // the user set it: do not override it.
    }
    if (rex::cvar::SetFlagByName(nombre, valor)) {
      REXLOG_DEBUG("Portable build default setting: {} = {}", nombre, valor);
    }
  }

  // ==========================================================================
  //  3. FPS COUNTER FOR THE F3 OVERLAY
  //
  //  In a RELEASE build, F3 opens an empty box that only says "Debug". These
  //  are two different things and both were shut:
  //
  //    1. Almost the whole panel lives inside #ifdef REXGLUE_ENABLE_PERF_COUNTERS,
  //       and the SDK's CMakeLists says
  //         add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
  //       meaning the define is not applied in Release. That is deliberate:
  //       "compiled out in Release", its comment says.
  //
  //    2. The "Guest: X FPS" line is NOT inside that #ifdef. It only asks that
  //       someone register a provider with SetGuestFrameStats, and nobody in
  //       the SDK calls it: it is an API the app has to use.
  //
  //  (2) is the door that can be opened without touching the SDK.
  //
  //  The provider is called by the overlay's OnDraw once per frame while it is
  //  open, so no frame hook is needed: looking at the clock each time they ask
  //  is enough. Exponential moving average, because the instantaneous value
  //  jumps so much it can neither be read nor compared.
  //
  //  It measures the frames the window presents. It only counts while the
  //  overlay is open: when it closes the calls stop and the average freezes.
  // ==========================================================================
  rex::ui::FrameStats MuestreaFotograma() {
    using Reloj = std::chrono::steady_clock;
    const auto ahora = Reloj::now();

    const double dt_ms =
        std::chrono::duration<double, std::milli>(ahora - ultimo_).count();
    // The first interval and any absurd one are discarded: when the overlay
    // reopens, the "previous" one spans all the time it was closed.
    const bool valido = tiene_anterior_ && dt_ms > 0.0 && dt_ms < 1000.0;

    ultimo_ = ahora;
    tiene_anterior_ = true;
    if (!valido) {
      return stats_;
    }

    suave_ms_ = (suave_ms_ <= 0.0) ? dt_ms : (suave_ms_ * 0.9 + dt_ms * 0.1);
    stats_.frame_time_ms = suave_ms_;
    stats_.fps = (suave_ms_ > 0.0) ? (1000.0 / suave_ms_) : 0.0;
    stats_.frame_count = ++fotogramas_;  // the overlay does not draw if this is 0
    return stats_;
  }

  // ==========================================================================
  //  4. HANG WATCHDOG
  //
  //  THE PROBLEM IT SOLVES
  //  When returning to the menu the game freezes, and the log shows absolutely
  //  nothing: not an error, not a kernel call, not a graphics command. Total
  //  silence until one closes the window. That rules out an exception or an
  //  unregistered function -those show up- and leaves a single explanation:
  //  ALL the game threads are stopped at once, waiting for something that
  //  never arrives.
  //
  //  And you cannot get out of a deadlock by looking at the log, because what
  //  defines it is precisely that nothing is written anymore. You have to go
  //  and ask the threads.
  //
  //  HOW IT WORKS, AND WHY IT DOES NOT NEED ANYONE TO WARN IT
  //  A separate thread looks once per second at ALL the guest threads and
  //  records two registers from each one:
  //
  //    lr  where the function it is in would return to. It changes constantly
  //        in code that advances.
  //    r1  the stack pointer. Same.
  //
  //  If for several seconds in a row NO thread has moved either of the two,
  //  the game is not slow: it is stopped. Then the table is dumped.
  //
  //  The good thing about measuring it this way is that it depends on nothing:
  //  not on the frame counter -which only runs with the overlay open-, not on
  //  the game calling the kernel, not on the graphics thread staying alive. If
  //  everything stops, it is noticed precisely because everything stops.
  //
  //  WHAT THE DUMP TELLS YOU
  //  For each thread: its entry address -which says WHICH thread it is-, lr,
  //  r1 and r13. That distinguishes the one waiting -lr stuck in a kernel wait
  //  function- from the one spinning -lr jumping between two or three
  //  addresses-. And since it dumps every 15 seconds while it lasts, you can
  //  see whether something moves very slowly or does not move at all.
  //
  //  COST WHEN NOTHING IS HAPPENING
  //  One pass per second reading two integers per thread. Nothing.
  //
  //  It lives in the app and not in the SDK on purpose: that way it can be
  //  changed without recompiling the whole SDK, and it does not impose a
  //  watchdog thread on anyone else.
  // ==========================================================================

  void ArrancarVigilante() {
    vigilante_activo_ = true;
    vigilante_ = std::thread([this] { VigilanteMain(); });
  }

  void PararVigilante() {
    vigilante_activo_ = false;
    if (vigilante_.joinable()) {
      vigilante_.join();
    }
  }

  // Thread table dump. 'grave' decides whether it comes out as an error -when
  // it is a real alarm- or as debug -the routine snapshots-.
  template <typename Lista>
  static void VolcarHilos(const Lista& hilos, bool grave) {
    for (auto& h : hilos) {
      const auto* cp = h->creation_params();
      auto* estado = h->thread_state();
      if (estado && estado->context()) {
        const auto& c = *estado->context();
        if (grave) {
          REXLOG_ERROR("[watchdog]   thread id=0x{:X} entry=0x{:08X} main={} running={} | "
                       "lr=0x{:08X} r1=0x{:08X} r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X} "
                       "last_indirect=0x{:08X}",
                       h->thread_id(), cp->start_address, h->main_thread(), h->is_running(),
                       static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32,
                       c.last_indirect_target);
        } else {
          REXLOG_DEBUG("[watchdog]   thread id=0x{:X} entry=0x{:08X} main={} running={} | "
                       "lr=0x{:08X} r1=0x{:08X} r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X} "
                       "last_indirect=0x{:08X}",
                       h->thread_id(), cp->start_address, h->main_thread(), h->is_running(),
                       static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32,
                       c.last_indirect_target);
        }
      } else {
        REXLOG_DEBUG("[watchdog]   thread id=0x{:X} entry=0x{:08X} no context", h->thread_id(),
                     cp->start_address);
      }
    }
  }

  void VigilanteMain() {
    using Reloj = std::chrono::steady_clock;

    // How many seconds in a row without ANYTHING moving before raising the
    // alarm. Five is lenient: this game at 10 fps still moves registers a
    // hundred times per second, so five seconds still is not slowness.
    constexpr int kSegundosParaSospechar = 5;
    constexpr int kSegundosEntreVolcados = 15;

    uint64_t firma_anterior = 0;
    int quietos = 0;
    int desde_ultimo_volcado = 0;
    int desde_instantanea = 0;
    bool avisado = false;

    while (vigilante_activo_) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!vigilante_activo_) break;

      auto* kernel = rex::system::kernel_state();
      if (!kernel) continue;

      auto hilos = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
      if (hilos.empty()) continue;

      // A signature of "where everyone is". It does not need to be a good
      // hash: it only has to change if any register changes.
      //
      // MIND THE ORDER. The first version of this multiplied and mixed on the
      // fly, walking the list as it came. And GetObjectsByType does NOT
      // guarantee order: in real dumps the threads came out shuffled from one
      // pass to the next, and even repeated -0x6 showed up twice-. So the
      // signature changed on its own even when nothing moved, and the alarm
      // NEVER fired on the real hang. The only thing that was of any use were
      // the periodic snapshots further below.
      //
      // It is fixed by putting each thread into a map keyed by its id: the map
      // sorts itself, so shuffling stops mattering, and a repeated id gets
      // overwritten instead of counted twice. Only then is it mixed.
      std::map<uint32_t, uint64_t> por_hilo;
      for (auto& h : hilos) {
        auto* estado = h->thread_state();
        if (!estado || !estado->context()) continue;
        const auto& c = *estado->context();
        por_hilo[h->thread_id()] =
            static_cast<uint64_t>(c.lr) ^ (static_cast<uint64_t>(c.r1.u32) << 20) ^
            (static_cast<uint64_t>(c.r3.u32) << 40);
      }

      uint64_t firma = 1469598103934665603ull;
      for (const auto& [id_hilo, huella] : por_hilo) {
        firma = (firma ^ id_hilo) * 1099511628211ull;
        firma = (firma ^ huella) * 1099511628211ull;
      }

      // PERIODIC SNAPSHOT, NO MATTER WHAT.
      //
      // The alarm above only fires if NOTHING moves, and it turned out the
      // hang we were chasing is not of that kind: the registers kept changing,
      // meaning the game executes code but does not advance. A tight loop
      // waiting for something that never comes looks just as stopped from the
      // outside and yet the alarm does not catch it.
      //
      // That is what this is for: every ten seconds it records where each
      // thread is, whether there is a problem or not. When the game freezes,
      // two or three snapshots of the bad stretch remain, and if lr spins
      // between the same two or three addresses, there is the loop.
      //
      // It goes at debug level -it does not get in the way in normal use- and
      // it is a few lines every ten seconds.
      if (++desde_instantanea >= 10) {
        desde_instantanea = 0;
        REXLOG_DEBUG("[watchdog] snapshot: {} game threads", hilos.size());
        VolcarHilos(hilos, false);
      }

      if (firma != firma_anterior) {
        if (avisado) {
          REXLOG_WARN("[watchdog] the game is moving again after {} s stopped.", quietos);
          avisado = false;
        }
        firma_anterior = firma;
        quietos = 0;
        desde_ultimo_volcado = 0;
        continue;
      }

      ++quietos;
      ++desde_ultimo_volcado;
      if (quietos < kSegundosParaSospechar) continue;
      if (avisado && desde_ultimo_volcado < kSegundosEntreVolcados) continue;
      desde_ultimo_volcado = 0;

      REXLOG_ERROR("[watchdog] {} s without a single register moving in any of the {} game "
                   "threads. This is not slowness: it is stopped.",
                   quietos, hilos.size());
      VolcarHilos(hilos, true);
      avisado = true;
    }
  }

  // ==========================================================================
  //  5. BLACK EDITION PATCH + THE SETTINGS MENU (ESC)
  // ==========================================================================

  void AplicarParcheBlackEdition() {
    // The Black Edition flag is a piece of DATA in the XEX, and it is at the
    // SAME address in every known version: Xenia's patch DB
    // (454107D9 - Need for Speed Most Wanted (2005).patch.toml) applies
    // data_write(be32, 0x82A2CE04, 0x00000100) to all six hashes, NTSC-U
    // included (PAL-ENG/GER/ITA/SPA, NTSC-J, NTSC-U).
    //
    // WARNING: the address is right, but unlocking the content on a regular
    // USA disc makes the game read Black Edition car data that is not on the
    // disc (guest access violation reading guest 0x8). The cvar defaults to
    // false for that reason; enable it only with Black Edition data.
    constexpr uint32_t kBlackEditionAddr = 0x82A2CE04u;
    auto* kernel = rex::system::kernel_state();
    if (kernel == nullptr || kernel->memory() == nullptr) {
      REXLOG_WARN("[black-edition] no memory kernel; cannot patch.");
      return;
    }
    if (!REXCVAR_GET(black_edition)) {
      REXLOG_INFO("[black-edition] disabled (black_edition=false).");
      return;
    }
    auto* bandera = kernel->memory()->TranslateVirtual<uint32_t*>(kBlackEditionAddr);
    if (bandera == nullptr) {
      REXLOG_WARN("[black-edition] could not translate 0x{:08X}; Black Edition "
                  "content will stay hidden.", kBlackEditionAddr);
      return;
    }
    // Guest memory is exposed in big-endian: the value is written as-is.
    *bandera = 0x00000100u;
    REXLOG_INFO("[black-edition] flag 0x{:08X} = 0x{:08X} (content unlocked).",
                kBlackEditionAddr, *bandera);
  }

  void AlternarMenu() {
    if (menu_ == nullptr) {
      auto* drawer = imgui_drawer();
      if (drawer == nullptr) {
        return;  // premature keypress: there is no UI yet
      }
      menu_ = new NfsmwMenuDialog(drawer, NfsmwMenuDialog::Callbacks{
          [this] { GuardarConfigDetras(); },
          [this] { RelanzarJuego(); },
          [this] {
            if (window() != nullptr) {
              window()->RequestClose();
            }
          },
          [this] { menu_ = nullptr; },
          [this] { return MuestreaFotograma(); },
      });
    } else {
      menu_->RequestClose();  // the dialog closes and deletes itself
    }
  }

  void GuardarConfigDetras() {
    auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty()) {
      carpeta = std::filesystem::current_path();
    }
    rex::cvar::SaveConfig(carpeta / "nfsmw.toml");
  }

  void RelanzarJuego() {
    GuardarConfigDetras();
#if defined(_WIN32)
    const auto exe = rex::filesystem::GetExecutablePath();
    if (!exe.empty()) {
      const std::wstring ruta = exe.wstring();
      const std::wstring carpeta = exe.parent_path().wstring();
      const INT_PTR resultado = reinterpret_cast<INT_PTR>(ShellExecuteW(
          nullptr, L"open", ruta.c_str(), nullptr, carpeta.c_str(), SW_SHOWNORMAL));
      if (resultado > 32) {
        // The new process starts with the freshly saved toml; this one closes.
        if (window() != nullptr) {
          window()->RequestClose();
        }
        return;
      }
      REXLOG_ERROR("[menu] could not relaunch the game (ShellExecuteW = {}); keep what "
                   "was applied and restart manually.", int32_t(resultado));
    } else {
      REXLOG_ERROR("[menu] no executable path; restart the game manually.");
    }
#else
    REXLOG_WARN("[menu] restart the game manually to apply the changes.");
#endif
  }

  rex::ui::FrameStats stats_{};
  std::chrono::steady_clock::time_point ultimo_{};
  double suave_ms_ = 0.0;
  uint64_t fotogramas_ = 0;
  bool tiene_anterior_ = false;

  std::thread vigilante_;
  std::atomic<bool> vigilante_activo_{false};

  NfsmwMenuDialog* menu_ = nullptr;  // ImGui dialogs delete themselves when closed
};
