// Guest textures: decoding of the fetch constant, untiling, format conversion, upload and caching.
//
// A texture is identified by everything in its fetch constant that changes its contents (addresses, size, format,
// layout); the swizzle and the sampler state only change how it is read. Its guest memory is fingerprinted the
// first time it is used in a frame, and uploaded again only if the fingerprint changed. Addresses written by a
// resolve come from the render targets instead (RenderTargets::Resolved).

#pragma once

#include "frame_ring.h"
#include "guest_state.h"
#include "render_targets.h"
#include "vulkan_context.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace nfsmw::renderer {

enum class TextureDimension : uint32_t {
  k2D = 0,
  k3D = 1,
  kCube = 2,
};

struct TextureBinding {
  VkImageView view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  float inv_width = 1.0f;  // 1/size of the host image (the shaders' 1/size constants)
  float inv_height = 1.0f;
};

class TextureCache {
 public:
  TextureCache(const VulkanContext& context, FrameRing& frames, rex::memory::Memory* memory,
               const RenderTargets& render_targets)
      : context_(context), frames_(frames), memory_(memory), render_targets_(render_targets) {}
  ~TextureCache();

  bool Initialize();

  // What fetch constant `fetch_index` samples as `dimension`. Uploads go to the frame's upload command buffer. A
  // texture that cannot be made (unsupported format, bad constant) gives a black placeholder of that dimension.
  TextureBinding Get(const GuestRegisters& registers, uint32_t fetch_index, TextureDimension dimension);

  // Releases textures that have not been used for a while, if the cache is over its budget. Once per frame.
  void EndFrame();

  // Diagnostic (nfsmw_diag_texturas): extra mip LOD bias on every sampler, to measure how much GPU time goes into
  // reading textures. 0 = the game's samplers.
  void SetDiagnosticLodBias(uint32_t bias) { diagnostic_lod_bias_ = bias; }

  // A resolved texture was replaced: its views are released once the GPU is done with them.
  void ForgetResolved(uint64_t id);

  // Work done since the last call to TakeStats (performance report).
  struct Stats {
    uint64_t lookups = 0;
    uint64_t fingerprints = 0;
    uint64_t fingerprint_bytes = 0;
    uint64_t uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t created = 0;
    uint64_t create_ns = 0;
  };
  Stats TakeStats() { return std::exchange(stats_, Stats{}); }
  const Stats& stats() const { return stats_; }

 private:
  // A block-compressed format the device cannot sample, decoded on the CPU at upload (DecodeBlocks).
  enum class Decode : uint8_t { kNone, kBc1, kBc2, kBc3, kBc4, kBc5 };

  struct HostFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t block_size = 1;      // texels per block side, in guest memory
    uint32_t bytes_per_block = 1;  // in guest memory
    uint32_t endian_unit = 0;     // 0: no byte swap; 2 or 4: the size of the swapped units
    std::array<uint8_t, 4> channel_of_component{0, 1, 2, 3};  // host channel holding each guest component
    Decode decode = Decode::kNone;
  };

  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkImageView> views;  // by guest swizzle
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t width = 0, height = 0, depth = 1, layers = 1, levels = 1;
    uint64_t fingerprint = 0;
    uint64_t validated_frame = UINT64_MAX;
    uint64_t changed_frame = 0;  // the last frame its data was found changed
    uint64_t used_frame = 0;
    VkDeviceSize bytes = 0;
  };

  static bool GetHostFormat(xenos::TextureFormat format, HostFormat& out);
  // On devices without BC textures (Mali, most Android GPUs): the BCn formats become uncompressed ones.
  void AdaptToDevice(HostFormat& format) const;
  // Whether the device can sample the format and copy into it (asked once per format).
  bool FormatUsable(VkFormat format);
  // Bytes per texel of the uncompressed format a Decode produces.
  static uint32_t DecodedBytesPerTexel(Decode decode);
  // Decodes `slices` images of blocks_x x blocks_y blocks into texels, (blocks_x * 4) texels per row.
  static void DecodeBlocks(Decode decode, const uint8_t* blocks, uint32_t blocks_x, uint32_t blocks_y,
                           uint32_t slices, uint8_t* out);
  bool CreateTexture(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& format,
                     TextureDimension dimension, uint32_t width, uint32_t height, uint32_t depth_or_layers,
                     uint32_t levels);
  bool Upload(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& format,
              TextureDimension dimension);
  uint64_t Fingerprint(const xenos::xe_gpu_texture_fetch_t& fetch, TextureDimension dimension) const;
  VkImageView ViewFor(Texture& texture, uint32_t swizzle, const HostFormat& format);
  VkImageView ResolvedView(const ResolvedTexture& resolved, uint32_t swizzle);
  TextureBinding ResolvedCube(const xenos::xe_gpu_texture_fetch_t& fetch);
  VkSampler SamplerFor(const xenos::xe_gpu_texture_fetch_t& fetch, TextureDimension dimension);
  void DestroyTextureLater(Texture& texture);
  TextureBinding Placeholder(TextureDimension dimension);

  const VulkanContext& context_;
  FrameRing& frames_;
  rex::memory::Memory* memory_;
  const RenderTargets& render_targets_;
  std::unordered_map<uint64_t, Texture> textures_;
  std::unordered_map<uint64_t, VkImageView> resolved_views_;  // (resolved texture id, swizzle)
  std::unordered_map<uint64_t, VkSampler> samplers_;
  uint32_t diagnostic_lod_bias_ = 0;
  std::array<Texture, 3> placeholders_{};
  VkSampler placeholder_sampler_ = VK_NULL_HANDLE;
  bool bc_supported_ = true;  // BC1-BC5 sampleable (Initialize)
  std::unordered_map<VkFormat, bool> usable_formats_;
  std::vector<uint8_t> compressed_;  // a level's blocks before DecodeBlocks
  VkDeviceSize total_bytes_ = 0;
  mutable Stats stats_;  // mutable: Fingerprint is const
};

}  // namespace nfsmw::renderer
