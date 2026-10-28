// The Vulkan vertex input of a vertex shader, from the vertex fetches the game's Direct3D patched into it.
//
// The translated shader reads each vertex element at a fixed location (USAGE_LOCATIONS in the translator). The
// microcode the game uploads has its fetch instructions patched by D3D with the vertex declaration of the draw:
// the fetch constant, format, offset, stride and component order. Each element is matched to its patched fetch by
// the register it writes, and the component order difference becomes the shader's input remap code.

#pragma once

#include "pipelines.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace nfsmw::nativo {
struct EntradaShader;
}

namespace nfsmw::renderer {

struct ShaderVertexInput {
  VertexInput input;
  std::array<uint32_t, 16> remap_codes{};  // per location: SharedBlockV11::kInputRemap
};

class VertexInputs {
 public:
  // nullptr if the patched microcode does not match the shader (the reason is logged once per shader).
  const ShaderVertexInput* Get(const nativo::EntradaShader& vertex_shader, std::span<const uint32_t> patched);

 private:
  bool Build(const nativo::EntradaShader& vertex_shader, std::span<const uint32_t> patched, ShaderVertexInput& out);

  std::unordered_map<uint64_t, ShaderVertexInput> cache_;
  std::unordered_map<uint64_t, bool> failed_;
};

}  // namespace nfsmw::renderer
