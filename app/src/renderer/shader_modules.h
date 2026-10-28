// Vulkan shader modules for the identified game shaders.
//
// The shader library holds every game shader's original container; this renderer translates each one with the
// Vulkan 1.1 resource model the first time it is needed (a few milliseconds) and keeps the module. The ring thread
// and the maintenance thread (which translates ahead of time) both call Get.

#pragma once

#include "vulkan_context.h"

#include <mutex>
#include <unordered_map>

namespace nfsmw::nativo {
struct EntradaShader;
}

namespace nfsmw::renderer {

class ShaderModules {
 public:
  explicit ShaderModules(const VulkanContext& context) : context_(context) {}
  ~ShaderModules();

  // VK_NULL_HANDLE if the shader cannot be translated (the reason is logged once). Thread-safe.
  VkShaderModule Get(const nativo::EntradaShader& shader);

  // Bytes of float constants the translated shader may read (SpirvRecompileResult::floatConstantRegisters * 16);
  // 0 if it is not translated (yet, or it failed). Thread-safe.
  uint32_t ConstantBytes(const nativo::EntradaShader& shader);

  // Shaders translated since the last call, and the time spent.
  struct Stats {
    uint64_t translated = 0;
    uint64_t translate_ns = 0;
  };
  Stats TakeStats();

 private:
  struct Module {
    VkShaderModule module = VK_NULL_HANDLE;
    uint32_t constant_bytes = 0;
  };

  Module Translate(const nativo::EntradaShader& shader);

  const VulkanContext& context_;
  std::mutex mutex_;
  std::unordered_map<const nativo::EntradaShader*, Module> modules_;
  Stats stats_;
};

}  // namespace nfsmw::renderer
