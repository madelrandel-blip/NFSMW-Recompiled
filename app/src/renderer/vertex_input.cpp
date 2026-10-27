#include "vertex_input.h"

#include "../nfsmw_nativo_shaders.h"

#include <rex/logging.h>

#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nfsmw::renderer {

namespace {

constexpr uint32_t kIdentityRemap = 0xFFF;

// USAGE_LOCATIONS of the translator (DeclUsage, usage index) -> location.
int32_t LocationOf(uint8_t usage, uint8_t usage_index) {
  struct Entry {
    uint8_t usage, index;
    int32_t location;
  };
  static constexpr Entry kLocations[] = {
      {0, 0, 0},  {3, 0, 1},  {6, 0, 2},  {7, 0, 3},  {5, 0, 4},   {5, 1, 5},  {5, 2, 6},  {5, 3, 7},  {10, 0, 8},
      {2, 0, 9},  {1, 0, 10}, {10, 1, 11}, {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15}, {0, 1, 15},
  };
  for (const Entry& e : kLocations) {
    if (e.usage == usage && e.index == usage_index) {
      return e.location;
    }
  }
  return -1;
}

// Components written by a fetch destination swizzle (3 bits each, 7 = not written).
uint32_t WrittenMask(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mask |= 1u << i;
    }
  }
  return mask;
}

// remapInput code: the shader computes r[i] = input[original[i]]; the patched fetch writes r[i] = data[patched[i]],
// so the host input at original[i] must come from data component patched[i].
uint32_t RemapCode(uint32_t original, uint32_t patched) {
  uint32_t code = kIdentityRemap;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

// Vertex fetch format -> VkFormat. `integer_input`: the shader declares the input as uint4 (BLENDINDICES).
VkFormat AttributeFormat(uint32_t format, bool integer_input, bool is_signed, bool integer) {
  auto pick = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled, VkFormat uint_, VkFormat sint) {
    if (integer_input) return is_signed ? sint : uint_;
    if (integer) return is_signed ? sscaled : uscaled;
    return is_signed ? snorm : unorm;
  };
  switch (format) {
    case 6:  // k_8_8_8_8
      return pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED,
                  VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      return pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                  VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                  VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 25:  // k_16_16
      return pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED,
                  VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      return pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_USCALED,
                  VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return integer_input ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return integer_input ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return integer_input ? (is_signed ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT) : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return integer_input ? (is_signed ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT) : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return integer_input ? (is_signed ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                           : VK_FORMAT_UNDEFINED;
    case 36:  // k_32_FLOAT
      return integer_input ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT;
    case 37:  // k_32_32_FLOAT
      return integer_input ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT;
    case 57:  // k_32_32_32_FLOAT
      return integer_input ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT;
    case 38:  // k_32_32_32_32_FLOAT
      return integer_input ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

}  // namespace

const ShaderVertexInput* VertexInputs::Get(const nativo::EntradaShader& vs, std::span<const uint32_t> patched) {
  // Everything Build reads: the shader and, for every element, its original and patched fetch words.
  std::vector<uint32_t> words;
  words.reserve(vs.elementos.size() * 4);
  for (const nativo::ElementoVertice& element : vs.elementos) {
    const size_t i = size_t(element.instruccion) * 3;
    if (i + 2 >= patched.size()) {
      return nullptr;
    }
    words.insert(words.end(), {patched[i], patched[i + 1], patched[i + 2], vs.microcodigo[i + 1]});
  }
  const uint64_t key = XXH3_64bits_withSeed(words.data(), words.size() * sizeof(uint32_t), uint64_t(uintptr_t(&vs)));
  if (auto it = cache_.find(key); it != cache_.end()) {
    return &it->second;
  }
  if (failed_.count(key)) {
    return nullptr;
  }
  ShaderVertexInput input;
  if (!Build(vs, patched, input)) {
    failed_.emplace(key, true);
    return nullptr;
  }
  return &cache_.emplace(key, std::move(input)).first->second;
}

bool VertexInputs::Build(const nativo::EntradaShader& vs, std::span<const uint32_t> patched, ShaderVertexInput& out) {
  out.remap_codes.fill(kIdentityRemap);
  if (patched.size() != vs.microcodigo.size()) {
    REXLOG_WARN("[renderer] VS n{}: the patched microcode does not have the container's length", vs.numero);
    return false;
  }
  uint32_t used_locations = 0;
  for (const nativo::ElementoVertice& element : vs.elementos) {
    const size_t element_word = size_t(element.instruccion) * 3;
    const uint32_t dest_register = (vs.microcodigo[element_word] >> 12) & 0x3F;
    const uint32_t original_swizzle = vs.microcodigo[element_word + 1] & 0xFFF;
    // D3D reorders the fetches but keeps the registers they write: the patched fetch of this element is the one
    // writing the same register (and, among several, the one writing the same components).
    size_t match = SIZE_MAX;
    for (const nativo::ElementoVertice& other : vs.elementos) {
      const size_t i = size_t(other.instruccion) * 3;
      if (((patched[i] >> 12) & 0x3F) != dest_register || (patched[i] & 0x1F) != 0) {
        continue;
      }
      if (match == SIZE_MAX) {
        match = i;
      }
      if (WrittenMask(patched[i + 1] & 0xFFF) == WrittenMask(original_swizzle)) {
        match = i;
        break;
      }
    }
    if (match == SIZE_MAX) {
      REXLOG_WARN("[renderer] VS n{}: no patched fetch for an element writing r{}", vs.numero, dest_register);
      return false;
    }
    const uint32_t d0 = patched[match], d1 = patched[match + 1], d2 = patched[match + 2];
    const uint32_t fetch_slot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
    const uint32_t format = (d1 >> 16) & 0x3F;
    const uint32_t stride = (d2 & 0xFF) * 4;
    const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
    const int32_t location = LocationOf(element.uso, element.indice_uso);
    if (location < 0 || ((used_locations >> location) & 0x1)) {
      continue;  // no free location in the translated shader: it cannot read this element anyway
    }
    const bool integer_input = element.uso == 2;  // BLENDINDICES is uint4 in the translated shader
    const VkFormat vk_format = AttributeFormat(format, integer_input, (d1 >> 12) & 0x1, (d1 >> 13) & 0x1);
    if (vk_format == VK_FORMAT_UNDEFINED) {
      REXLOG_WARN("[renderer] VS n{}: vertex format {} is not supported", vs.numero, format);
      return false;
    }
    if (offset < 0) {
      REXLOG_WARN("[renderer] VS n{}: negative vertex offset", vs.numero);
      return false;
    }
    uint32_t binding = 0;
    while (binding < out.input.bindings.size() && out.input.bindings[binding].fetch_slot != fetch_slot) {
      ++binding;
    }
    if (binding == out.input.bindings.size()) {
      out.input.bindings.push_back({fetch_slot, stride});
    } else if (!out.input.bindings[binding].stride) {
      out.input.bindings[binding].stride = stride;
    }
    out.input.attributes.push_back({uint32_t(location), binding, vk_format, uint32_t(offset)});
    out.remap_codes[location] = RemapCode(original_swizzle, d1 & 0xFFF);
    used_locations |= 1u << location;
  }
  for (const VertexBinding& binding : out.input.bindings) {
    if (!binding.stride) {
      REXLOG_WARN("[renderer] VS n{}: vertex stream without a stride", vs.numero);
      return false;
    }
  }
  uint64_t hash = XXH3_64bits(out.input.attributes.data(), out.input.attributes.size() * sizeof(VertexAttribute));
  hash = XXH3_64bits_withSeed(out.input.bindings.data(), out.input.bindings.size() * sizeof(VertexBinding), hash);
  out.input.hash = hash ? hash : 1;
  return true;
}

}  // namespace nfsmw::renderer
