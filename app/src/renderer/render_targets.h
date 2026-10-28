// EDRAM-less render targets and the textures the game resolves them to.
//
// The Xenos draws into 10 MB of EDRAM and copies ("resolves") rectangles of it to textures in memory. Here every
// render target is a Vulkan image, identified by its EDRAM base, format and pitch, and every resolve destination is
// another image, identified by its guest address; draws that sample that address get the image. Small color
// resolves are also written back to guest memory, because the game reads some on the CPU (the 64x64 exposure
// measurements).
//
// All images stay in VK_IMAGE_LAYOUT_GENERAL; work on them is ordered with explicit barriers.

#pragma once

#include "depth_resolve.h"
#include "frame_ring.h"
#include "guest_state.h"
#include "vulkan_context.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace nfsmw::nativo {
struct RegistrosCopia;
}

namespace nfsmw::renderer {

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;         // all aspects (attachments, copies)
  VkImageView sample_view = VK_NULL_HANDLE;  // color, or depth only (sampling)
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  VkImageUsageFlags usage = 0;  // as created: an imageless framebuffer's attachment must match it
  bool depth = false;
  // Image pixels per guest pixel: below 1 for a render target drawn smaller than the game asks (the shadow maps,
  // nfsmw_nativo_sombras_escala). Viewports, scissors and resolves are scaled by it.
  float scale = 1.0f;
  // Render targets only. Where the guest has it (traces), and the state that decides the load and store operations of
  // the passes that use it. Mutable: draws hold const pointers to their targets.
  uint32_t edram_base = 0;
  uint32_t pitch = 0;
  mutable bool clear_pending = false;  // a clear not done yet: the next pass applies it as loadOp CLEAR
  mutable VkClearValue clear_value{};
  mutable bool has_content = false;    // a pass stored what it drew (a target nobody drew holds nothing to read)
  mutable bool content_read = false;   // its stored contents were ever read: copied, or loaded by a pass
  mutable uint64_t first_frame = UINT64_MAX;
  // The largest extent the game ever drew into (its draws' scissors). The image is allocated for the largest the
  // EDRAM allows (1280x1280 for a 720p scene), and a pass whose render area covers all of it loads and stores
  // tiles nothing draws into: on a tiler that is most of the pass's memory traffic. 0 = nothing drawn yet.
  mutable uint32_t drawn_width = 0;
  mutable uint32_t drawn_height = 0;
  // Resolved without a copy (RenderTargets::Copy swapped this target's image with the texture's): the guest address
  // of that texture, which holds what this target last had. If the game draws into it again without clearing it,
  // RestoreSwapped copies it back first. 0 = nothing to restore.
  mutable uint32_t swapped_into = 0;
};

struct ResolvedTexture {
  Image image;
  uint64_t id = 0;            // unique for the life of the renderer (handles may be reused after destruction)
  uint32_t guest_format = 0;  // xenos::TextureFormat the game will sample it as
  bool swap_red_blue = false;
  uint64_t write_count = 0;   // increases with every resolve into it (texture descriptor caches)
};

class RenderTargets {
 public:
  RenderTargets(const VulkanContext& context, FrameRing& frames, rex::memory::Memory* memory)
      : context_(context), frames_(frames), memory_(memory), depth_resolver_(context, frames) {}
  ~RenderTargets();

  bool Initialize();

  // MSAA for the scene's render targets: those with the pitch of the render target the game presents (learned at
  // the first swap with LearnScenePitch). The rest (shadow map, reflections, bloom) keep one sample.
  void SetMultisample(VkSampleCountFlagBits samples) { multisample_ = samples; }
  VkSampleCountFlagBits multisample() const { return multisample_; }
  // The pitch of the render target the resolve at `resolved_address` came from; 0 if unknown.
  uint32_t SourcePitchOf(uint32_t resolved_address) const;
  // From now on, render targets with this pitch are multisampled; existing ones are recreated.
  void LearnScenePitch(uint32_t pitch);
  uint32_t scene_pitch() const { return scene_pitch_; }

  // Increases with every copy: a resolve can change what a texture address samples.
  uint64_t copy_generation() const { return copy_generation_; }

  // A color (format: xenos::ColorRenderTargetFormat) or depth (xenos::DepthRenderTargetFormat) render target.
  Image* ColorTarget(uint32_t edram_base, uint32_t format, uint32_t pitch);
  Image* DepthTarget(uint32_t edram_base, uint32_t format, uint32_t pitch);

  // The texture a resolve wrote at that guest address, or nullptr.
  const ResolvedTexture* Resolved(uint32_t guest_address) const;

  // One copy command: resolve and/or clear. Must be called outside a render pass.
  void Copy(VkCommandBuffer commands, const nativo::RegistrosCopia& registers);

  // Called with the id of every resolved texture that is replaced, so caches of views of it can drop them.
  void SetResolvedDestroyedCallback(std::function<void(uint64_t id)> callback) {
    resolved_destroyed_ = std::move(callback);
  }

  // Orders everything recorded before against everything recorded after (render passes, copies, clears).
  static void Barrier(const VulkanContext& context, VkCommandBuffer commands);
  // After a render pass: orders its attachment writes before later passes and copies, without holding back the
  // vertex stage of what follows.
  static void AfterPassBarrier(const VulkanContext& context, VkCommandBuffer commands);
  // Around copies and clears: like Barrier for everything but the vertex stage, which never reads their results.
  static void TransferBarrier(const VulkanContext& context, VkCommandBuffer commands);

  // Does the clear `target` is waiting for (outside a render pass), when it cannot become a loadOp CLEAR.
  void FlushClear(VkCommandBuffer commands, const Image& target);

  // Before anything reads `target` (a pass that loads it, a resolve from it): if its image was swapped into a
  // resolved texture and it has not been cleared since, copies those contents back (outside a render pass).
  void RestoreSwapped(VkCommandBuffer commands, const Image& target);

  // A depth target whose contents have never been read in kDiscardAfterFrames frames of use: its passes do not store
  // depth and stencil (storeOp DONT_CARE). The first read makes it stored again.
  bool DepthDiscardable(const Image& target) const;

 private:
  // A resolve copied to upload memory, to be written to guest memory once the GPU has done it.
  struct Readback {
    const uint8_t* data = nullptr;  // width * height RGBA8 texels
    uint32_t guest_base = 0;        // the texture's base address (32x32-tiled layout)
    uint32_t guest_pitch = 0;
    uint32_t guest_height = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t copy_dest_info = 0;  // endianness and red/blue swap of the copy
  };

  bool CreateImage(Image& image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  VkSampleCountFlagBits SamplesFor(uint32_t pitch) const {
    return pitch && pitch == scene_pitch_ ? multisample_ : VK_SAMPLE_COUNT_1_BIT;
  }
  // A single-sample image the size of `target`, to resolve into before a scaling blit.
  Image* ResolveScratch(const Image& target);
  void DestroyImageLater(Image image);
  ResolvedTexture* ResolveDestination(uint32_t address, uint32_t width, uint32_t height, VkFormat format,
                                      uint32_t guest_format, bool swap_red_blue);
  bool CopyRectangle(const nativo::RegistrosCopia& registers, uint32_t pitch, int32_t& x0, int32_t& y0, int32_t& x1,
                     int32_t& y1) const;
  void QueueReadback(VkCommandBuffer commands, const ResolvedTexture& texture, const nativo::RegistrosCopia& registers,
                     uint32_t guest_base, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void WriteReadback(const Readback& readback);

  const VulkanContext& context_;
  FrameRing& frames_;
  rex::memory::Memory* memory_;
  std::unordered_map<uint64_t, Image> color_targets_;
  std::unordered_map<uint64_t, Image> depth_targets_;
  std::unordered_map<uint32_t, ResolvedTexture> resolved_;
  std::unordered_map<uint32_t, uint32_t> resolved_source_pitch_;  // resolved address -> source render target pitch
  std::unordered_map<VkImage, Image> resolve_scratch_;           // by multisampled render target
  DepthResolver depth_resolver_;
  VkSampleCountFlagBits multisample_ = VK_SAMPLE_COUNT_1_BIT;
  uint32_t scene_pitch_ = 0;
  uint64_t copy_generation_ = 0;
  uint64_t next_resolved_id_ = 1;
  std::function<void(uint64_t id)> resolved_destroyed_;
};

// Address of a texel of a 32x32-tiled 2D texture (rex::graphics::texture_util::GetTiledOffset2D), inline: it runs
// for every block of every texture upload.
inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes_per_block) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes_per_block + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes_per_block;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}


}  // namespace nfsmw::renderer
