// nfsmw - mouse (and later touch) in the game's menus, through FEng's own mouse support (nfsmw_fe_raton.cpp).

#pragma once

#include <rex/runtime.h>

namespace nfsmw::fe_raton {

// Wraps config.input_factory to add the virtual pad that presses A when a click lands on the menu's current option.
// Call it in OnPreSetup. Does nothing if nfsmw_menu_raton is false.
void EnvolverEntrada(rex::RuntimeConfig& config);

}  // namespace nfsmw::fe_raton
