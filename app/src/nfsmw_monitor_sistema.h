// nfsmw - system monitor for performance reports on phones (see nfsmw_monitor_sistema.cpp).

#pragma once

namespace nfsmw::monitor {

// Starts the monitor thread if nfsmw_monitor_sistema_s > 0 (Android and Linux; nothing elsewhere).
void Arrancar();

}  // namespace nfsmw::monitor
