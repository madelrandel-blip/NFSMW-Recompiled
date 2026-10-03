#!/usr/bin/env python3
"""
Improves the settings menu (F4): restore button, and sliders.

    python tools/parche_restaurar.py            apply
    python tools/parche_restaurar.py --estado
    python tools/parche_restaurar.py --revertir

It touches one SDK file:  src/ui/overlay/settings_overlay.cpp

It does not keep a .original and does not need one: it applies and undoes by
exact text replacement, block by block. That is on purpose. In this SDK there
are files that already carry another patch on top, and keeping a ".original"
there at this point would save the ALREADY patched file as if it were the clean
one; incidentally, a --revertir would wipe out the other patch. This way each
patch removes its own part and only its own.

And it goes block by block, not with a global mark, BECAUSE OF A BUG THAT
ALREADY HAPPENED. The first version only brought the button. When I added the
sliders, the script checked whether its mark was present, found it -from the
button- and said "already applied" without applying anything new. Result:
button yes, slider no, and without a single error message. Now each block is
checked separately, so adding one more in the future applies it without having
to undo the previous one.


THE RESTORE BUTTON
==================

The F4 menu already brings a "Reset" per setting, but only in the keybinds, and
you have to go one by one. While testing performance cvars you touch six or
seven in a while and then there is no way to know which ones were left moved:
the following numbers no longer compare against anything.

It goes back to THE STARTUP CONFIGURATION, not the SDK's factory values. That
is: what the game had set when it opened, with nfsmw.toml already applied. It
is what is expected from a "reset" here: if the toml leaves the video engine in
rtv and anisotropic filtering off, restoring must leave it that way, not take
you back to SDK values you have never used and that work worse.

It is done with a snapshot of all settings taken the first time this window is
opened. In practice that snapshot IS startup, because nothing here changes on
its own; if you had touched something through the console before opening F4,
that would be the snapshot. It is said in the warning so it does not surprise.

Three caveats, and all three matter:

  - It does NOT touch the read-only ones. They are fixed at startup and the
    interface already paints them disabled; trying would be lying.

  - It does NOT write nfsmw.toml. It only changes the live values. If you want
    the change to survive a restart, you have to press "Save to config"
    afterwards. That way an accidental click does not wipe out the
    configuration on disk.

  - The ones that ask for a restart are changed anyway, but are not noticed
    until the next time you open the game.

Before doing anything it asks, because it is destructive and it is glued to the
save button.


THE SLIDERS
===========

Decimal settings were edited with a text box: type the number and Enter. For
one that is tuned by trial and error -game_speed, for example- that is
uncomfortable, because you cannot drag and see the effect on the fly.

When the setting declares a minimum and maximum, a slider now appears. When it
does not declare them, the usual box remains, because without limits there is
nowhere to slide. Ctrl+click on the slider still lets you type the exact value.

The decimals it shows come from the range itself: if it goes from 0 to 200 -a
percentage- it shows whole, and if it goes from 0 to 1 it shows three decimals.
With a fixed format, either the percentages came out as "100.00" or the short
bars looked broken.


WHY THE TEXTS ARE IN ENGLISH
============================

The buttons next to it are "Save to config", "Rebind" and "Reset". That window
belongs to the SDK and is entirely in English; putting Spanish text in the
middle looks like a bug, not like a translation. The code comments are in
Spanish, as in the rest of this project's patches.
"""

import argparse
import pathlib
import sys

# ---------------------------------------------------------------------------
#  1) Headers
# ---------------------------------------------------------------------------

CAB_ANCLA = """#include <rex/ui/keybinds.h>
#include <imgui.h>
"""

CAB_NUEVO = """#include <rex/logging.h>  // PARCHE LOCAL - para dejar constancia del restaurado
#include <rex/ui/keybinds.h>
#include <imgui.h>

#include <cstdlib>  // PARCHE LOCAL - std::_Exit, para el boton de reiniciar
#include <utility>  // PARCHE LOCAL - std::pair, que llegaba de rebote por <map>

// PARCHE LOCAL - para relanzar el juego desde el boton de reiniciar.
//
// Las dos guardas van como en src/ui/windowed_app_main_sdl.cpp, que ya incluye
// windows.h de esta manera. NOMINMAX importa aqui de verdad: sin el, windows.h
// define min y max como macros y este fichero usa std::min y std::max.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
"""

# ---------------------------------------------------------------------------
#  2) The startup configuration snapshot
# ---------------------------------------------------------------------------

FOTO_ANCLA = """void SettingsDialog::OnDraw(ImGuiIO& /*io*/) {
  auto& registry = rex::cvar::GetRegistry();
"""

FOTO_NUEVO = """void SettingsDialog::OnDraw(ImGuiIO& /*io*/) {
  auto& registry = rex::cvar::GetRegistry();

  // PARCHE LOCAL - foto de la configuracion de partida
  //
  // Es a lo que vuelve el boton "Restore defaults", y no a los valores de
  // fabrica del SDK. La diferencia importa: el nfsmw.toml deja puestas cosas
  // -el motor de video, el filtrado- que son la configuracion buena de este
  // juego. Restaurar a los valores del SDK dejaria una que nunca se ha usado,
  // y encima peor.
  //
  // Se toma la primera vez que se dibuja esta ventana. En la practica eso es
  // el arranque, porque nada de aqui cambia solo. Si alguien hubiera tocado
  // algo por la consola antes de abrir F4, esa seria la foto; el aviso del
  // boton lo dice para que no sorprenda.
  //
  // Los cvars del complemento de GPU se registran al cargar su DLL, que es
  // mucho antes de que nadie pulse F4, asi que entran en la foto igual.
  static std::vector<std::pair<std::string, std::string>> configuracion_de_partida;
  static bool foto_tomada = false;
  if (!foto_tomada) {
    foto_tomada = true;
    configuracion_de_partida.reserve(registry.size());
    for (auto& e : registry) {
      configuracion_de_partida.emplace_back(e.name, e.getter());
    }
    REXLOG_DEBUG("[ajustes] foto de partida: {} valores", configuracion_de_partida.size());
  }
"""

# ---------------------------------------------------------------------------
#  3) Slider for decimals with limits
# ---------------------------------------------------------------------------

DOBLE_ANCLA = """      } else if (entry.type == rex::cvar::FlagType::Double) {
        double v = std::atof(current_val.c_str());
        if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
          if (entry.constraints.min)
            v = std::max(v, *entry.constraints.min);
          if (entry.constraints.max)
            v = std::min(v, *entry.constraints.max);
          rex::cvar::SetFlagByName(entry.name, std::to_string(v));
        }
"""

DOBLE_NUEVO = """      } else if (entry.type == rex::cvar::FlagType::Double) {
        // PARCHE LOCAL - deslizador cuando el ajuste trae minimo y maximo.
        //
        // Con la caja de texto hay que escribir el numero y darle a Enter. Para
        // un ajuste que se busca a tanteo -la velocidad del juego, sin ir mas
        // lejos- eso es incomodo: no puedes arrastrar y ver el efecto. Con
        // limites conocidos, un deslizador es lo suyo; sin ellos no hay por
        // donde deslizar, asi que se queda la caja de siempre.
        //
        // Ctrl+clic sobre el deslizador sigue dejando escribir el valor exacto.
        if (entry.constraints.min.has_value() && entry.constraints.max.has_value()) {
          float v = static_cast<float>(std::atof(current_val.c_str()));
          const float vmin = static_cast<float>(*entry.constraints.min);
          const float vmax = static_cast<float>(*entry.constraints.max);
          // Cuantos decimales enseniar. Un recorrido largo -0 a 200, que es un
          // porcentaje- se lee mejor entero; uno corto -0 a 1- necesita
          // decimales o la barra parece que no hace nada.
          const char* formato = (vmax - vmin >= 10.0f) ? "%.0f" : "%.3f";
          if (ImGui::SliderFloat("##v", &v, vmin, vmax, formato)) {
            v = std::clamp(v, vmin, vmax);
            rex::cvar::SetFlagByName(entry.name, std::to_string(v));
          }
        } else {
          double v = std::atof(current_val.c_str());
          if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
            if (entry.constraints.min)
              v = std::max(v, *entry.constraints.min);
            if (entry.constraints.max)
              v = std::min(v, *entry.constraints.max);
            rex::cvar::SetFlagByName(entry.name, std::to_string(v));
          }
        }
"""

# ---------------------------------------------------------------------------
#  4) The bottom bar: the button and its confirmation
# ---------------------------------------------------------------------------

BARRA_ANCLA = """  // Bottom bar: Save button.
  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    rex::cvar::SaveConfig(config_path_);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());
"""

BARRA_NUEVO = """  // Bottom bar: Save button.
  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    rex::cvar::SaveConfig(config_path_);
  }
  ImGui::SameLine();

  // PARCHE LOCAL - boton de restaurar la configuracion de partida
  //
  // Va detras de una confirmacion a proposito: esta pegado al de guardar y es
  // destructivo. Un clic de mas no puede costar media tarde de ajustes.
  if (ImGui::Button("Restore defaults")) {
    ImGui::OpenPopup("Restore defaults?##rex_restore");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());

  if (ImGui::BeginPopupModal("Restore defaults?##rex_restore", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Every setting goes back to how it was when the game started,");
    ImGui::TextUnformatted("with your config file already applied.");
    ImGui::Spacing();
    ImGui::BulletText("Read-only settings are left alone: they are fixed when the\\n"
                      "game starts, so changing them now would do nothing.");
    ImGui::BulletText("Settings that need a restart do change, but only take\\n"
                      "effect the next time you start the game.");
    ImGui::BulletText("%s is NOT written. Press \\"Save to config\\"\\n"
                      "afterwards if you want this to survive a restart.",
                      config_path_.filename().string().c_str());
    ImGui::Separator();

    if (ImGui::Button("Restore", ImVec2(120.0f, 0))) {
      // Se recoge la lista ANTES de tocar nada. SetFlagByName escribe en la
      // entrada del registro -al menos su origen-, y no me apetece estar
      // recorriendo el contenedor mientras se modifica.
      std::vector<std::pair<std::string, std::string>> pendientes;
      for (auto& e : registry) {
        // Solo lectura: fijados al arrancar. Entre ellos esta el motor de
        // video, que es justo el que no queremos perder.
        if (e.lifecycle == rex::cvar::Lifecycle::kInitOnly) {
          continue;
        }
        // Los comandos son botones, no tienen valor que restaurar.
        if (e.type == rex::cvar::FlagType::Command) {
          continue;
        }

        // El valor de la foto de partida. Si el ajuste no estaba -se registro
        // despues de abrir esta ventana la primera vez, que es raro pero
        // posible- se cae al valor de fabrica, que es lo unico que se sabe.
        const std::string* objetivo = nullptr;
        for (auto& [nombre, valor] : configuracion_de_partida) {
          if (nombre == e.name) {
            objetivo = &valor;
            break;
          }
        }
        const std::string& valor_bueno = objetivo ? *objetivo : e.default_value;

        if (e.getter() == valor_bueno) {
          continue;
        }
        pendientes.emplace_back(e.name, valor_bueno);
      }

      int restaurados = 0;
      for (auto& [nombre, valor] : pendientes) {
        if (rex::cvar::SetFlagByName(nombre, valor)) {
          ++restaurados;
        }
      }
      REXLOG_INFO("[ajustes] {} de {} valores devueltos a la configuracion de partida",
                  restaurados, pendientes.size());
      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
"""

# ---------------------------------------------------------------------------
#  Previous version of THIS patch, so it can be migrated
#
#  v1 only brought the button, and restored to the SDK's FACTORY values instead
#  of the startup configuration. If it is still installed it must be removed
#  first, or the button anchor does not fit: its place is taken.
#
#  This text is taken as-is from the already patched file, not written by hand,
#  so the replacement is exact.
# ---------------------------------------------------------------------------

VIEJA_BARRA = '  // Bottom bar: Save button.\n  ImGui::Separator();\n  if (ImGui::Button("Save to config")) {\n    rex::cvar::SaveConfig(config_path_);\n  }\n  ImGui::SameLine();\n\n  // PARCHE LOCAL - boton de restaurar valores por defecto\n  //\n  // Va detras de una confirmacion a proposito: esta pegado al de guardar y es\n  // destructivo. Un clic de mas no puede costar media tarde de ajustes.\n  if (ImGui::Button("Restore defaults")) {\n    ImGui::OpenPopup("Restore defaults?##rex_restore");\n  }\n  ImGui::SameLine();\n  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());\n\n  if (ImGui::BeginPopupModal("Restore defaults?##rex_restore", nullptr,\n                             ImGuiWindowFlags_AlwaysAutoResize)) {\n    ImGui::TextUnformatted("Every setting goes back to its built-in default.");\n    ImGui::Spacing();\n    ImGui::BulletText("Read-only settings are left alone: they are fixed when the\\n"\n                      "game starts, so changing them now would do nothing.");\n    ImGui::BulletText("Settings that need a restart do change, but only take\\n"\n                      "effect the next time you start the game.");\n    ImGui::BulletText("%s is NOT written. Press \\"Save to config\\"\\n"\n                      "afterwards if you want this to survive a restart.",\n                      config_path_.filename().string().c_str());\n    ImGui::Separator();\n\n    if (ImGui::Button("Restore", ImVec2(120.0f, 0))) {\n      // Los nombres se recogen ANTES de tocar nada. SetFlagByName escribe en\n      // la entrada del registro -al menos su origen- y no me apetece estar\n      // recorriendo el contenedor mientras se modifica.\n      std::vector<std::pair<std::string, std::string>> pendientes;\n      for (auto& e : registry) {\n        // Solo lectura: fijados al arrancar. Entre ellos esta el motor de\n        // video, que es justo el que no queremos perder.\n        if (e.lifecycle == rex::cvar::Lifecycle::kInitOnly) {\n          continue;\n        }\n        // Los comandos son botones, no tienen valor que restaurar.\n        if (e.type == rex::cvar::FlagType::Command) {\n          continue;\n        }\n        if (e.getter() == e.default_value) {\n          continue;\n        }\n        pendientes.emplace_back(e.name, e.default_value);\n      }\n\n      int restaurados = 0;\n      for (auto& [nombre, porDefecto] : pendientes) {\n        if (rex::cvar::SetFlagByName(nombre, porDefecto)) {\n          ++restaurados;\n        }\n      }\n      REXLOG_INFO("[ajustes] {} de {} valores devueltos a su valor por defecto", restaurados,\n                  pendientes.size());\n      ImGui::CloseCurrentPopup();\n    }\n\n    ImGui::SameLine();\n    if (ImGui::Button("Cancel", ImVec2(120.0f, 0))) {\n      ImGui::CloseCurrentPopup();\n    }\n    ImGui::EndPopup();\n  }\n'

VIEJAS_CABECERAS = '#include <rex/logging.h>  // PARCHE LOCAL - para dejar constancia del restaurado\n#include <rex/ui/keybinds.h>\n#include <imgui.h>\n\n#include <utility>  // PARCHE LOCAL - std::pair, que llegaba de rebote por <map>\n'

VIEJO_AVISO_V1 = '  // PARCHE LOCAL - aviso de reinicio pendiente, con boton para reiniciar\n  //\n  // El SDK ya llevaba la cuenta de los ajustes cambiados que piden reinicio\n  // -SetFlagFromSource llama a MarkPendingRestart, y GetPendingRestartFlags los\n  // devuelve-, pero no lo ensenaba en ninguna parte. Sin eso, cambiar la API\n  // grafica o cualquier otro de esos parecia no hacer nada: pones el valor,\n  // vuelves al juego, y todo sigue igual sin una sola pista de por que.\n  //\n  // Sirve para todos, no solo para el de la API.\n  {\n    // La API grafica que se acabo usando. La pone rex_app.cpp al arrancar, y no\n    // siempre coincide con el cvar gpu_backend: si la que pediste no estaba\n    // compilada en esta copia, se arranca con la otra. Por eso se ensena la\n    // real y no el ajuste.\n    //\n    // Un extern a secas, sin cabecera nueva: rex_app.cpp y este fichero se\n    // compilan en la misma biblioteca.\n    extern std::string g_gpu_backend_en_uso;\n    ImGui::Separator();\n    ImGui::Text("Graphics API in use: %s", g_gpu_backend_en_uso.c_str());\n\n    const auto pendientes_reinicio = rex::cvar::GetPendingRestartFlags();\n    if (!pendientes_reinicio.empty()) {\n      std::string lista;\n      for (const auto& n : pendientes_reinicio) {\n        if (!lista.empty()) {\n          lista += ", ";\n        }\n        lista += n;\n      }\n      ImGui::TextColored(imgui_drawer()->style().settings.warning,\n                         "Restart needed to apply: %s", lista.c_str());\n#if defined(_WIN32)\n      // Guarda ANTES de reiniciar, y no es un extra: estos ajustes solo viven\n      // en memoria hasta que se guardan. Reiniciar sin guardar volveria con el\n      // valor viejo y pareceria que el boton no hace nada.\n      if (ImGui::Button("Save and restart")) {\n        rex::cvar::SaveConfig(config_path_);\n\n        // Sin std::size a proposito: sale de <iterator>, que este fichero no\n        // incluye y que llega de rebote segun la implementacion. Una constante\n        // no depende de nada.\n        constexpr DWORD kMaxRuta = 1024;\n        wchar_t ruta[kMaxRuta];\n        const DWORD largo = GetModuleFileNameW(nullptr, ruta, kMaxRuta);\n        bool relanzado = false;\n        if (largo > 0 && largo < kMaxRuta) {\n          // La misma linea de comandos con la que se abrio, para no perder la\n          // ISO ni las opciones que le pasa el lanzador. CreateProcessW puede\n          // escribir en ese buffer, asi que se le da una copia propia.\n          std::wstring linea = GetCommandLineW();\n          std::vector<wchar_t> linea_editable(linea.begin(), linea.end());\n          linea_editable.push_back(L\'\\0\');\n\n          STARTUPINFOW si{};\n          si.cb = sizeof(si);\n          PROCESS_INFORMATION pi{};\n          if (CreateProcessW(ruta, linea_editable.data(), nullptr, nullptr, FALSE, 0, nullptr,\n                             nullptr, &si, &pi)) {\n            CloseHandle(pi.hProcess);\n            CloseHandle(pi.hThread);\n            relanzado = true;\n          }\n        }\n\n        if (relanzado) {\n          REXLOG_INFO("[ajustes] reiniciando para aplicar: {}", lista);\n          // Misma salida que usa la app al cerrar la ventana: vaciar el log y\n          // cortar en seco. El desmontaje ordenado puede quedarse colgado en un\n          // cerrojo que dejo algun hilo del juego, y por eso el SDK tampoco lo\n          // hace ahi.\n          rex::FlushLogging();\n          std::_Exit(0);\n        }\n        REXLOG_ERROR("[ajustes] no se pudo relanzar el juego (error {}). Cierralo y abrelo tu.",\n                     GetLastError());\n      }\n      ImGui::SameLine();\n      ImGui::TextDisabled("(saves first, then opens again with the same options)");\n#else\n      ImGui::TextDisabled("Close the game and open it again to apply.");\n#endif\n    }\n  }\n\n'

AVISO_ANCLA = """  ImGui::EndChild();

  // Bottom bar: Save button.
"""

AVISO_NUEVO = """  ImGui::EndChild();

  // PARCHE LOCAL - aviso de reinicio pendiente, con boton para reiniciar
  //
  // El SDK ya llevaba la cuenta de los ajustes cambiados que piden reinicio
  // -SetFlagFromSource llama a MarkPendingRestart, y GetPendingRestartFlags los
  // devuelve-, pero no lo ensenaba en ninguna parte. Sin eso, cambiar la API
  // grafica o cualquier otro de esos parecia no hacer nada: pones el valor,
  // vuelves al juego, y todo sigue igual sin una sola pista de por que.
  //
  // Sirve para todos, no solo para el de la API.
  {
    // La API grafica, leida del registro de cvars por nombre.
    //
    // La primera version de esto usaba una variable global compartida con
    // rex_app.cpp, y NO ENLAZABA: rex_app.cpp no se compila dentro del SDK,
    // se instala como fuente y lo compila cada aplicacion, asi que la
    // definicion acaba en el .exe y esta referencia en la DLL. El registro de
    // cvars, en cambio, esta hecho justo para hablar entre modulos.
    //
    // Y desde que se quito "any" de las opciones, el ajuste ES la API: el
    // unico caso en que no coincide es que la elegida no este compilada en
    // esta copia, y entonces el arranque lo deja escrito en el log.
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", rex::cvar::GetFlagByName("gpu_backend").c_str());

    const auto pendientes_reinicio = rex::cvar::GetPendingRestartFlags();
    if (!pendientes_reinicio.empty()) {
      std::string lista;
      for (const auto& n : pendientes_reinicio) {
        if (!lista.empty()) {
          lista += ", ";
        }
        lista += n;
      }
      ImGui::TextColored(imgui_drawer()->style().settings.warning,
                         "Restart needed to apply: %s", lista.c_str());
#if defined(_WIN32)
      // Guarda ANTES de reiniciar, y no es un extra: estos ajustes solo viven
      // en memoria hasta que se guardan. Reiniciar sin guardar volveria con el
      // valor viejo y pareceria que el boton no hace nada.
      if (ImGui::Button("Save and restart")) {
        rex::cvar::SaveConfig(config_path_);

        // Sin std::size a proposito: sale de <iterator>, que este fichero no
        // incluye y que llega de rebote segun la implementacion. Una constante
        // no depende de nada.
        constexpr DWORD kMaxRuta = 1024;
        wchar_t ruta[kMaxRuta];
        const DWORD largo = GetModuleFileNameW(nullptr, ruta, kMaxRuta);
        bool relanzado = false;
        if (largo > 0 && largo < kMaxRuta) {
          // La misma linea de comandos con la que se abrio, para no perder la
          // ISO ni las opciones que le pasa el lanzador. CreateProcessW puede
          // escribir en ese buffer, asi que se le da una copia propia.
          std::wstring linea = GetCommandLineW();
          std::vector<wchar_t> linea_editable(linea.begin(), linea.end());
          linea_editable.push_back(L'\\0');

          STARTUPINFOW si{};
          si.cb = sizeof(si);
          PROCESS_INFORMATION pi{};
          if (CreateProcessW(ruta, linea_editable.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            relanzado = true;
          }
        }

        if (relanzado) {
          REXLOG_INFO("[ajustes] reiniciando para aplicar: {}", lista);
          // Misma salida que usa la app al cerrar la ventana: vaciar el log y
          // cortar en seco. El desmontaje ordenado puede quedarse colgado en un
          // cerrojo que dejo algun hilo del juego, y por eso el SDK tampoco lo
          // hace ahi.
          rex::FlushLogging();
          std::_Exit(0);
        }
        REXLOG_ERROR("[ajustes] no se pudo relanzar el juego (error {}). Cierralo y abrelo tu.",
                     GetLastError());
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(saves first, then opens again with the same options)");
#else
      ImGui::TextDisabled("Close the game and open it again to apply.");
#endif
    }
  }

  // Bottom bar: Save button.
"""

BLOQUES = [
    ("cabeceras", CAB_ANCLA, CAB_NUEVO),
    ("aviso de reinicio pendiente", AVISO_ANCLA, AVISO_NUEVO),
    ("foto de la configuracion de partida", FOTO_ANCLA, FOTO_NUEVO),
    ("deslizador para decimales con limites", DOBLE_ANCLA, DOBLE_NUEVO),
    ("boton de restaurar", BARRA_ANCLA, BARRA_NUEVO),
]

# It goes AFTER the blocks on purpose: each entry needs its anchor, and the
# anchors are defined above. The first version of this list was before them
# and the warning entry ended up with an anchor of "", which worked by
# accident -replacing with an empty string erases the block and leaves the
# anchor untouched- until the migration started looking at which block belongs
# to each anchor and ran into a key that did not exist.
#
# FROM NEWEST TO OLDEST. See quitar_version_vieja for why it matters.
VIEJOS = [
    # (name, fingerprint to warn with, whole block in anchor form, anchor)
    ("v1 warning (shared variable)",
     'g_gpu_backend_en_uso',
     '  ImGui::EndChild();\n\n' + VIEJO_AVISO_V1 + '  // Bottom bar: Save button.\n',
     AVISO_ANCLA),
    ("v1 button (restored to factory values)",
     'ImGui::TextUnformatted("Every setting goes back to its built-in default.");',
     VIEJA_BARRA, BARRA_ANCLA),
    ("v1 headers (without windows.h)",
     '#include <rex/logging.h>  // PARCHE LOCAL - para dejar constancia del restaurado\n'
     '#include <rex/ui/keybinds.h>\n#include <imgui.h>\n\n#include <utility>',
     VIEJAS_CABECERAS, CAB_ANCLA),
]



def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "src" / "ui" / "overlay" / "settings_overlay.cpp").exists():
            return cand
    sys.exit("[ERROR] Cannot find the SDK's src/ui/overlay/settings_overlay.cpp.\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def quitar_version_vieja(txt):
    """Removes the remains of a previous version of this same patch.

    THE PROBLEM, WHICH COST ME THREE ATTEMPTS
    -----------------------------------------
    An old block and the current one can overlap in two ways, and each one
    breaks the obvious solution to the other:

      * THE OLD ONE IS A PIECE OF THE CURRENT ONE (code was added to the
        block). Searching for the old one finds it INSIDE the good one, and
        replacing it with the anchor cuts the head off the freshly placed
        block. Then applying again leaves the tail DUPLICATED. The file grew
        on every pass.

      * THE CURRENT ONE IS A PIECE OF THE OLD ONE (code was removed from the
        block). Then "the good block is there" says yes even though what is
        present is still the whole old one, and the script considers itself
        applied while leaving dead code inside.

    I tried to solve it with a FINGERPRINT per version -a piece that only
    existed in that version-. It does not always exist: when the old one is an
    exact prefix of the new one, EVERYTHING in the old one is also in the new
    one.

    THE RULE THAT DOES WORK, AND NEEDS NO FINGERPRINTS
    --------------------------------------------------
    Finding the old block only counts if it CANNOT be the good one seen
    halfway:

        es_de_verdad_vieja = (viejo in txt) and
                             (viejo not in nuevo or nuevo not in txt)

    Both cases above come out right with that, and it is checked with the
    texts alone, without me having to guess any fingerprint by hand.

    VIEJOS still goes FROM NEWEST TO OLDEST, and as soon as one version fits
    an anchor the rest for that anchor are skipped: if v2 is v1 with things
    added, looking at v1 first would orphan v2's tail. That happened too.

    And this is tested by running the patch TWICE in a row on the real file
    and comparing. The duplication bug does not show on the first pass, which
    is the only one usually looked at.
    """
    ahora = {ancla: nuevo for _, ancla, nuevo in BLOQUES}
    quitados = 0
    anclajes_hechos = set()
    for nombre, huella, viejo, ancla in VIEJOS:
        if ancla in anclajes_hechos:
            continue
        nuevo = ahora[ancla]
        if viejo not in txt:
            # The fingerprint is only used to warn: if a piece of that version
            # shows up but the whole block does not fit, someone edited it by
            # hand and I prefer not to guess.
            if huella in txt and nuevo not in txt:
                print(f"[aviso] I see remains of '{nombre}' but not in the form I expected.")
                print(f"        Leaving it alone; look at it by hand if something seems off.")
            continue
        if viejo in nuevo and nuevo in txt:
            # It is not an old version: it is the current block, which
            # contains the old one inside. This anchor is already up to date.
            anclajes_hechos.add(ancla)
            continue
        txt = txt.replace(viejo, ancla)
        anclajes_hechos.add(ancla)
        print(f"[ok] Removed previous version: {nombre}")
        quitados += 1
    return txt, quitados


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    f = localizar_sdk() / "src" / "ui" / "overlay" / "settings_overlay.cpp"
    txt = f.read_text(encoding="utf-8")

    if args.estado:
        puestos = sum(1 for _, _, nuevo in BLOQUES if nuevo in txt)
        print(f"  {f.name:26s} {puestos} of {len(BLOQUES)} blocks applied")
        for nombre, _, nuevo in BLOQUES:
            print(f"      {'yes' if nuevo in txt else 'NO':>2}  {nombre}")
        # With the same rule the migration uses, so --estado does not warn
        # about remains that are actually pieces of the good block.
        ahora = {ancla: nuevo for _, ancla, nuevo in BLOQUES}
        viejos = sum(1 for _, _, viejo, ancla in VIEJOS
                     if viejo in txt
                     and (viejo not in ahora[ancla] or ahora[ancla] not in txt))
        if viejos:
            print(f"      -- {viejos} blocks of the previous version remain")
        return 0

    if args.revertir:
        quitados = 0
        for nombre, ancla, nuevo in BLOQUES:
            if nuevo not in txt:
                continue
            if txt.count(nuevo) != 1:
                sys.exit(f"[ERROR] The block '{nombre}' appears {txt.count(nuevo)} times.\n"
                         f"        I am not touching it, remove it yourself.")
            txt = txt.replace(nuevo, ancla)
            quitados += 1
        txt, viejos = quitar_version_vieja(txt)
        quitados += viejos
        if not quitados:
            print(f"[ok] {f.name}: there was nothing applied")
            return 0
        f.write_text(txt, encoding="utf-8")
        print(f"[ok] Removed {quitados} blocks from {f.name}")
        print()
        print("  THE SDK MUST BE RECOMPILED.")
        return 0

    txt, _ = quitar_version_vieja(txt)

    # Block by block: the ones already there are left alone, the missing ones
    # are applied. That way, adding a new block later does not force undoing
    # what was placed.
    faltan = [(n, a, v) for n, a, v in BLOQUES if v not in txt]
    if not faltan:
        print(f"[ok] {f.name}: all {len(BLOQUES)} blocks were already there")
        return 0

    for nombre, ancla, _ in faltan:
        n = txt.count(ancla)
        if n != 1:
            sys.exit(f"[ERROR] The anchor for '{nombre}' appears {n} times, expected 1.\n"
                     f"        The SDK must have changed. I have not touched anything.")

    for nombre, ancla, nuevo in faltan:
        txt = txt.replace(ancla, nuevo)
        print(f"[ok] Applied: {nombre}")
    f.write_text(txt, encoding="utf-8")
    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
