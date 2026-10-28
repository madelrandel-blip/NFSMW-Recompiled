// Typed access to the guest GPU registers, as mirrored by the native graphics system.

#pragma once

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include <cstdint>
#include <cstring>

namespace nfsmw::renderer {

namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;

// Register indices of the shader constant and fetch constant files.
constexpr uint32_t kVertexConstantsRegister = 0x4000;  // c0-c255 of the vertex shader, 4 words each
constexpr uint32_t kPixelConstantsRegister = 0x4400;   // c0-c255 of the pixel shader
constexpr uint32_t kFetchConstantsRegister = 0x4800;   // 32 texture fetch constants of 6 words (or 96 vertex ones of 2)
constexpr uint32_t kBooleanConstantsRegister = 0x4900;
constexpr uint32_t kLoopConstantsRegister = 0x4908;

class GuestRegisters {
 public:
  explicit GuestRegisters(const uint32_t* values) : values_(values) {}

  uint32_t operator[](uint32_t index) const { return values_[index]; }

  float Float(uint32_t index) const {
    float value;
    std::memcpy(&value, &values_[index], sizeof(value));
    return value;
  }

  template <typename T>
  T Get() const {
    return Get<T>(uint32_t(T::register_index));
  }

  template <typename T>
  T Get(uint32_t index) const {
    T value;
    std::memcpy(&value, &values_[index], sizeof(value));
    return value;
  }

  xenos::xe_gpu_texture_fetch_t TextureFetch(uint32_t index) const {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, &values_[kFetchConstantsRegister + index * 6], sizeof(fetch));
    return fetch;
  }

  xenos::xe_gpu_vertex_fetch_t VertexFetch(uint32_t index) const {
    xenos::xe_gpu_vertex_fetch_t fetch;
    std::memcpy(&fetch, &values_[kFetchConstantsRegister + index * 2], sizeof(fetch));
    return fetch;
  }

  const uint32_t* data() const { return values_; }

 private:
  const uint32_t* values_;
};

// The four color render target registers, in order.
constexpr uint32_t kColorInfoRegisters[4] = {
    rex::graphics::XE_GPU_REG_RB_COLOR_INFO, rex::graphics::XE_GPU_REG_RB_COLOR1_INFO,
    rex::graphics::XE_GPU_REG_RB_COLOR2_INFO, rex::graphics::XE_GPU_REG_RB_COLOR3_INFO};
constexpr uint32_t kBlendControlRegisters[4] = {
    rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL1,
    rex::graphics::XE_GPU_REG_RB_BLENDCONTROL2, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL3};

}  // namespace nfsmw::renderer
