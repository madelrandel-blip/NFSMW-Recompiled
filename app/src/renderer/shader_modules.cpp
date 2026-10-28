#include "shader_modules.h"

#include "../nfsmw_nativo_shaders.h"
#include "../nfsmw_shader_library.h"
#include "nfsmw_contenedor.h"
#include "spirv_recompiler.h"

#include <rex/logging.h>

#include <chrono>
#include <utility>

namespace nfsmw::renderer {

namespace {

// XXH3 of the race VisualTreatment container (p_000139): its radial blur gets the SPEC_CONSTANT_SIN_DESENFOQUE
// switch. The same in every edition.
constexpr uint64_t kCompositeFingerprint = 0x19C0C358044A29BFull;

}  // namespace

ShaderModules::~ShaderModules() {
  for (auto& [shader, module] : modules_) {
    if (module.module) {
      context_.fn().vkDestroyShaderModule(context_.handle(), module.module, nullptr);
    }
  }
}

VkShaderModule ShaderModules::Get(const nativo::EntradaShader& shader) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = modules_.find(&shader); it != modules_.end()) {
      return it->second.module;
    }
  }
  // Translated outside the lock: the other thread may be translating another shader meanwhile.
  const auto start = std::chrono::steady_clock::now();
  const Module module = Translate(shader);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  std::lock_guard<std::mutex> lock(mutex_);
  auto [it, inserted] = modules_.emplace(&shader, module);
  if (!inserted && module.module) {
    // Both threads translated it: keep the first.
    context_.fn().vkDestroyShaderModule(context_.handle(), module.module, nullptr);
  }
  if (inserted) {
    ++stats_.translated;
    stats_.translate_ns += ns;
  }
  return it->second.module;
}

uint32_t ShaderModules::ConstantBytes(const nativo::EntradaShader& shader) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = modules_.find(&shader);
  return it != modules_.end() ? it->second.constant_bytes : 0;
}

ShaderModules::Stats ShaderModules::TakeStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(stats_, Stats{});
}

ShaderModules::Module ShaderModules::Translate(const nativo::EntradaShader& shader) {
  Module module;
  if (!shader.shader) {
    return module;
  }
  std::vector<uint8_t> container;
  try {
    nfsmw::Flujo flow;
    container = nfsmw::Convertir2005(shader.shader->original, flow);
  } catch (const std::exception& e) {
    REXLOG_ERROR("[renderer] shader n{}: container: {}", shader.numero, e.what());
    return module;
  }
  SpirvRecompilerOptions options;
  options.resources = SpirvResourceModel::Vulkan11;
  options.legacyMultiply = true;
  options.sanitizeColorOutputs = true;
  options.compositionBlurSwitch = shader.shader->huella == kCompositeFingerprint;
  options.spirvVersion = context_.spirv_version();
  const SpirvRecompileResult result = RecompileToSpirv(container.data(), options);
  if (result.spirv.empty()) {
    REXLOG_ERROR("[renderer] shader n{}: translation: {}", shader.numero, result.error);
    return module;
  }
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = result.spirv.size() * sizeof(uint32_t);
  info.pCode = result.spirv.data();
  if (context_.fn().vkCreateShaderModule(context_.handle(), &info, nullptr, &module.module) != VK_SUCCESS) {
    REXLOG_ERROR("[renderer] shader n{}: vkCreateShaderModule failed", shader.numero);
    module.module = VK_NULL_HANDLE;
  }
  module.constant_bytes = result.floatConstantRegisters * 16;
  return module;
}

}  // namespace nfsmw::renderer
