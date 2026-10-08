#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {

struct PrebakedShaderEntry {
  uint64_t ucode_hash = 0;
  xenos::ShaderType shader_type = xenos::ShaderType::kVertex;
  std::vector<SpirvShader::TextureBinding> texture_bindings;
  std::vector<SpirvShader::SamplerBinding> sampler_bindings;
  std::vector<uint8_t> spirv_binary;
};

class PrebakedShaderCache {
 public:
  static constexpr uint32_t kMagic = 0x53584552;  // "REXS"
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kDefaultTitleId = 0x454107D9;

  static PrebakedShaderCache& Get();

  PrebakedShaderCache() = default;
  ~PrebakedShaderCache() = default;

  void Clear();

  void AddShader(PrebakedShaderEntry entry);

  const PrebakedShaderEntry* FindShader(uint64_t ucode_hash) const;

  bool HasShader(uint64_t ucode_hash) const;

  size_t Count() const { return entries_.size(); }

  bool LoadFromFile(const std::string& file_path);

  bool LoadFromMemory(const void* data, size_t size);

  bool SaveToFile(const std::string& file_path) const;

 private:
  std::unordered_map<uint64_t, PrebakedShaderEntry> entries_;
};

}  // namespace rex::graphics
