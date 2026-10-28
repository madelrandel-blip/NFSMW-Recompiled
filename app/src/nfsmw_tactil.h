// nfsmw - on-screen touch controls (Android).
//
// A virtual pad drawn over the game with the game's own icons: left and right arrows for steering, the triggers
// (gas and brake), A/B/X/Y, the shoulder buttons, Back (resets the car), Start and a nut that opens a panel for the
// size and opacity of the controls. Fingers are read from SDL's touch events, several at once, and
// the result is a synthetic device of the SDK input system, merged into user 0 like the keyboard emulation
// (SlotAssignment), so it coexists with a real pad. While a real pad is connected the controls are hidden and
// report nothing.

#pragma once

#include <rex/runtime.h>

namespace rex::ui {
class ImGuiDrawer;
}

namespace nfsmw::tactil {

// Wraps config.input_factory to add the touch pad. Call it in OnPreSetup, once ReXApp has installed the default
// factory. Does nothing if nfsmw_tactil is false.
void EnvolverEntrada(rex::RuntimeConfig& config);

// Shows the controls over the game (the dialog deletes itself with the drawer). Call it on the UI thread once the
// game starts.
void MostrarControles(rex::ui::ImGuiDrawer* drawer);

}  // namespace nfsmw::tactil
