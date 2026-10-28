// One guest draw: render targets, indices, vertices, constants, textures, pipeline and the draw command.
//
// Draws into the same render targets share one render pass; it is closed before anything that has to run outside a
// pass (copies, clears, the swap) through EndPass().

#pragma once

#include "frame_ring.h"
#include "geometry_cache.h"
#include "guest_state.h"
#include "pipelines.h"
#include "render_targets.h"
#include "textures.h"
#include "vertex_input.h"
#include "vulkan_context.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace nfsmw::nativo {
struct PeticionDibujo;
}

namespace nfsmw::renderer {

class Draws {
 public:
  Draws(const VulkanContext& context, FrameRing& frames, rex::memory::Memory* memory, RenderTargets& render_targets,
        TextureCache& textures, Pipelines& pipelines)
      : context_(context),
        frames_(frames),
        memory_(memory),
        render_targets_(render_targets),
        textures_(textures),
        pipelines_(pipelines),
        geometry_cache_(context, frames, memory) {}

  // false if the draw was dropped (the reason is logged once per cause).
  bool Draw(const nativo::PeticionDibujo& request);

  // Closes the open render pass, if any.
  void EndPass();

  // The frame's command buffer was submitted: everything per frame starts over.
  void FrameSubmitted();

  uint64_t draw_count() const { return draw_count_; }
  // Draws that asked for Xenos tessellation (dropped: not implemented).
  uint64_t tessellated_draws() const { return tessellated_draws_; }

  // Render passes started since the last call, by why the previous one could not continue.
  struct PassStats {
    uint64_t after_break = 0;        // the previous one was closed by a copy, a clear or the swap
    uint64_t depth_toggled = 0;      // same color targets, depth attached in one and not in the other
    uint64_t color_subset = 0;       // targets contained in the open pass's
    uint64_t different_targets = 0;  // anything else
  };
  PassStats TakePassStats() { return std::exchange(pass_stats_, PassStats{}); }

  // CPU time of Draw by stage (nfsmw_perfil_dibujos), summed since the last call, and the draws that got to the end.
  enum class DrawStage : uint8_t {
    kSetup,      // render targets, vertex input
    kGeometry,   // indices (decoded, cached)
    kTextures,   // texture lookups, uploads, descriptor writes prepared
    kPass,       // render pass begin and framing
    kPipeline,   // pipeline key and lookup
    kResources,  // constants upload, descriptor sets
    kVertices,   // vertex data copied (byte swapped) and bound
    kCommands,   // state binds and the draw command
    kCount
  };
  struct DrawProfile {
    std::array<uint64_t, size_t(DrawStage::kCount)> ns{};
    uint64_t draws = 0;
  };
  DrawProfile TakeDrawProfile() { return std::exchange(draw_profile_, DrawProfile{}); }
  GeometryCache::Stats TakeGeometryStats() { return geometry_cache_.TakeStats(); }

  // Diagnostic (nfsmw_diag_pixeles): every draw's scissor cut to the top-left quarter of what it covers, so the
  // geometry is processed whole but only a quarter of the pixels are shaded.
  void SetDiagnosticQuarter(bool quarter) {
    diagnostic_quarter_ = quarter;
    framing_generation_ = UINT64_MAX;  // the cached framing has the other scissor
  }

 private:
  struct PassState {
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::array<const Image*, kMaxColorTargets + 1> targets{};
    uint32_t width = 0, height = 0;  // the framebuffer's: viewports and scissors are relative to it
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    // The render area (from the top-left corner): what the targets were ever drawn into, rounded up; the whole
    // framebuffer while nothing is known. A draw beyond it reopens the pass larger.
    uint32_t area_width = 0, area_height = 0;
    // What this pass's draws reach (their scissors), given to the targets when it ends.
    uint32_t drawn_width = 0, drawn_height = 0;
    float scale = 1.0f;  // the targets' Image::scale: guest viewports and scissors are multiplied by it
  };

  struct Geometry {
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    uint32_t count = 0;        // indices, or vertices without an index buffer
    uint32_t first_vertex = 0;  // lowest vertex index used
    uint32_t vertex_count = 0;
    bool indexed = false;
  };

  bool Reject(uint32_t cause, const char* text);
  void DropStaleCopies();
  bool BuildGeometry(const GuestRegisters& r, uint32_t primitive_type, bool restart, Geometry& out);
  bool BindVertexBuffers(VkCommandBuffer commands, const GuestRegisters& r, const VertexInput& input,
                         const Geometry& geometry);
  uint32_t ConstantBytes(const nativo::EntradaShader& shader);
  bool UploadConstants(const nativo::PeticionDibujo& request, const GuestRegisters& r, const uint8_t* shared,
                       std::array<uint32_t, 3>& offsets, VkBuffer& buffer);
  bool JoinsOpenPass(const std::array<const Image*, kMaxColorTargets + 1>& targets) const;
  bool BeginPass(VkCommandBuffer commands, const std::array<const Image*, kMaxColorTargets + 1>& targets);
  // Viewport, scissor and the NDC transform of a draw: a pure function of the framing registers and the pass size.
  struct Framing {
    VkViewport viewport{};
    VkRect2D scissor{};
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    bool empty = true;
  };
  void ComputeFraming(const GuestRegisters& r, Framing& out) const;
  void ApplyFraming(VkCommandBuffer commands, const Framing& framing);

  const VulkanContext& context_;
  FrameRing& frames_;
  uint16_t pass_label_ = 0;     // GPU time label of the open pass (measuring only)
  uint16_t between_label_ = 0;  // ... and of the work between passes
  rex::memory::Memory* memory_;
  RenderTargets& render_targets_;
  TextureCache& textures_;
  Pipelines& pipelines_;
  VertexInputs vertex_inputs_;

  PassState pass_;
  uint64_t pass_draws_ = 0;  // draws in the open pass (traces)
  PassStats pass_stats_;
  bool in_pass_ = false;
  uint64_t draw_count_ = 0;
  uint64_t tessellated_draws_ = 0;
  bool logged_tessellation_ = false;

  // Per-frame vertex data: the same guest range is converted once per frame, until the game waits for the GPU
  // (only then may it rewrite data it has already referenced).
  struct VertexCopy {
    VkBuffer buffer;
    VkDeviceSize offset;
  };
  std::unordered_map<uint64_t, VertexCopy> vertex_copies_;
  // Converted index ranges of this frame, by the registers that define them.
  std::unordered_map<uint64_t, Geometry> index_copies_;
  uint32_t copies_sync_ = 0;
  // Converted vertex and index ranges that stay from frame to frame while the game does not rewrite them.
  GeometryCache geometry_cache_;

  // Constants of the previous draw, reused while their register generations do not change.
  uint64_t vs_constants_generation_ = UINT64_MAX, ps_constants_generation_ = UINT64_MAX;
  uint32_t vs_constants_bytes_ = 0, ps_constants_bytes_ = 0;  // how much of the reused uploads was written
  VkBuffer constants_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize vs_constants_offset_ = 0, ps_constants_offset_ = 0;
  VkDescriptorSet uniform_set_ = VK_NULL_HANDLE;
  VkBuffer uniform_set_buffer_ = VK_NULL_HANDLE;

  // What the command buffer has bound, to skip recording the same state again.
  struct TextureSlot {
    uint32_t binding = 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    bool operator==(const TextureSlot&) const = default;
  };
  struct BoundState {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet texture_set = VK_NULL_HANDLE;
    std::array<TextureSlot, kTextureBindings> texture_slots{};
    uint32_t texture_slot_count = 0;
    VkDescriptorSet uniform_set = VK_NULL_HANDLE;
    std::array<uint32_t, 3> uniform_offsets{};
    bool sets_dirty = true;
    bool dynamic_valid = false;
    float depth_bias[2] = {};
    std::array<float, 4> blend_constants{};
    uint32_t stencil[2] = {};
    VkBuffer index_buffer = VK_NULL_HANDLE;
    bool viewport_valid = false;
    VkViewport viewport{};
    VkRect2D scissor{};
    std::array<VkBuffer, 16> vertex_buffers{};
    std::array<VkDeviceSize, 16> vertex_offsets{};
    uint32_t vertex_buffer_count = 0;
  };
  BoundState bound_;

  std::vector<uint32_t> index_scratch_;  // quads before their expansion into triangles

  std::unordered_map<const nativo::EntradaShader*, uint32_t> constant_bytes_;

  // Work skipped while its inputs do not change (the native system's change counters in PeticionDibujo).
  Framing framing_;
  bool diagnostic_quarter_ = false;
  DrawProfile draw_profile_;
  uint64_t framing_generation_ = UINT64_MAX;  // generacion_encuadre it was computed with
  uint32_t framing_width_ = 0, framing_height_ = 0;
  const nativo::EntradaShader* input_vs_ = nullptr;
  uint64_t input_generation_ = UINT64_MAX;  // generacion_vs
  const ShaderVertexInput* input_ = nullptr;
  // The textures of the previous draw: valid for the same pixel shader, fetch constants (generacion_fetch), frame
  // (the cache validates once per frame) and copy generation.
  struct TextureReuse {
    const nativo::EntradaShader* ps = nullptr;
    uint64_t fetch_generation = UINT64_MAX;
    uint64_t frame = UINT64_MAX;
    uint64_t copies = UINT64_MAX;
    uint32_t count = 0;
    std::array<VkDescriptorImageInfo, kTextureBindings> images{};
    std::array<uint32_t, kTextureBindings> bindings{};
    std::array<uint8_t, 16 * 8> inv_sizes{};  // SharedBlockV11::kInvTextureSize
  };
  TextureReuse texture_reuse_;

  std::array<bool, 512> rejected_{};
};

}  // namespace nfsmw::renderer
