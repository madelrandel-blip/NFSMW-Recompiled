// Entry point of the Vulkan 1.1 renderer: the native system's DestinosNativos backed by the modules of this folder.

#pragma once

#include "../nfsmw_nativo_destinos.h"

#include <memory>

namespace nfsmw::renderer {

// nullptr if the device lacks something the renderer needs (the reason is logged).
std::unique_ptr<nativo::DestinosNativos> CreateVulkanRenderer(const rex::ui::vulkan::VulkanDevice* device,
                                                              rex::memory::Memory* memory);

}  // namespace nfsmw::renderer
