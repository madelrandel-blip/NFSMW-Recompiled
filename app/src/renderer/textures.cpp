#include "textures.h"

#include "bc_decode.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

// For now off on Android: the AYN Thor (Adreno 740) showed visual glitches in the menus that the phones decoding
// on the CPU (Mali) do not have.
REXCVAR_DEFINE_BOOL(nfsmw_texturas_bc_gpu, REX_PLATFORM_ANDROID == 0, "Graficos",
                    "Usar las texturas DXT (BC) tal cual en la GPU si las admite; si no, se descomprimen en la CPU "
                    "al subirlas (la misma imagen)");

// Same configuration as nfsmw_shader_library.cpp: byte reads through memcpy (see the comment there).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nfsmw::renderer {

namespace texture_util = rex::graphics::texture_util;

namespace {

// Cache budget: above it, textures unused for kEvictAfterFrames frames are released.
constexpr VkDeviceSize kBudgetBytes = VkDeviceSize(512) << 20;
constexpr uint64_t kEvictAfterFrames = 300;
// Data checks: every frame while a texture is new or changing; once it has been unchanged for kStableFrames frames,
// one frame out of kStableCheckInterval (staggered by key, so the work spreads over the frames).
constexpr uint64_t kStableFrames = 60;
constexpr uint64_t kStableCheckInterval = 16;

uint32_t Log2(uint32_t value) {
  uint32_t log = 0;
  while ((1u << (log + 1)) <= value) {
    ++log;
  }
  return log;
}

// In place, a 32-bit word at a time (upload allocations are 16-byte aligned and texture levels a multiple of 4
// bytes; a 2-byte tail is handled for k8in16).
void SwapEndian(uint8_t* data, size_t bytes, xenos::Endian endian) {
  const size_t words = bytes / 4;
  uint32_t* word = reinterpret_cast<uint32_t*>(data);
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i < words; ++i) {
        const uint32_t v = word[i];
        word[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      }
      if (bytes & 2) {
        std::swap(data[words * 4], data[words * 4 + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i < words; ++i) {
        word[i] = __builtin_bswap32(word[i]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i < words; ++i) {
        word[i] = (word[i] << 16) | (word[i] >> 16);
      }
      break;
    default:
      break;
  }
}

xenos::DataDimension GuestDimension(TextureDimension dimension) {
  switch (dimension) {
    case TextureDimension::k3D:
      return xenos::DataDimension::k3D;
    case TextureDimension::kCube:
      return xenos::DataDimension::kCube;
    default:
      return xenos::DataDimension::k2DOrStacked;
  }
}

// Fields of the fetch constant that change the texture's contents (not its swizzle or sampler state).
uint64_t TextureKey(const xenos::xe_gpu_texture_fetch_t& fetch, TextureDimension dimension) {
  uint32_t words[5] = {fetch.dword_0 & 0xFFC00000u,  // pitch and tiling
                       fetch.dword_1 & 0xFFFFF0FFu,  // format, endianness, base address
                       fetch.dword_2,                // size
                       fetch.dword_4 & 0x000003FCu,  // mip levels
                       fetch.dword_5 & 0xFFFFFE00u | uint32_t(dimension)};  // mip address, packed mips
  return XXH3_64bits(words, sizeof(words));
}

}  // namespace

bool TextureCache::GetHostFormat(xenos::TextureFormat format, HostFormat& out) {
  // The same host formats as the emulation (vulkan/texture_cache.cpp). Single- and two-channel formats expose their
  // data components on the host's red (and green) channels.
  constexpr std::array<uint8_t, 4> kRgba{0, 1, 2, 3}, kRrrr{0, 0, 0, 0}, kRggg{0, 1, 1, 1};
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8:
    case F::k_8_A:
      out = {VK_FORMAT_R8_UNORM, 1, 1, 0, kRrrr};
      return true;
    case F::k_8_8:
      out = {VK_FORMAT_R8G8_UNORM, 1, 2, 2, kRggg};
      return true;
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_AS_16_16_16_16:
      out = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 4, kRgba};
      return true;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
      out = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 4, kRgba};
      return true;
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      out = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 2, kRgba};
      return true;
    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      out = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 2, kRgba};
      return true;
    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      out = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 2, kRgba};
      return true;
    case F::k_DXN:
      out = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 2, kRggg};
      return true;
    case F::k_DXT5A:
      out = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 2, kRrrr};
      return true;
    case F::k_16:
      out = {VK_FORMAT_R16_UNORM, 1, 2, 2, kRrrr};
      return true;
    case F::k_16_16:
      out = {VK_FORMAT_R16G16_UNORM, 1, 4, 2, kRggg};
      return true;
    case F::k_16_16_16_16:
      out = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 2, kRgba};
      return true;
    case F::k_16_FLOAT:
      out = {VK_FORMAT_R16_SFLOAT, 1, 2, 2, kRrrr};
      return true;
    case F::k_16_16_FLOAT:
      out = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 2, kRggg};
      return true;
    case F::k_16_16_16_16_FLOAT:
      out = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 2, kRgba};
      return true;
    case F::k_32_FLOAT:
      out = {VK_FORMAT_R32_SFLOAT, 1, 4, 4, kRrrr};
      return true;
    case F::k_32_32_FLOAT:
      out = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 4, kRggg};
      return true;
    case F::k_32_32_32_32_FLOAT:
      out = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 4, kRgba};
      return true;
    default:
      return false;
  }
}

TextureCache::~TextureCache() {
  frames_.WaitIdle();
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  auto destroy = [&](Texture& texture) {
    for (auto& [swizzle, view] : texture.views) fn.vkDestroyImageView(device, view, nullptr);
    if (texture.image) fn.vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory) fn.vkFreeMemory(device, texture.memory, nullptr);
  };
  for (auto& [key, texture] : textures_) destroy(texture);
  for (auto& texture : placeholders_) destroy(texture);
  for (auto& [key, view] : resolved_views_) fn.vkDestroyImageView(device, view, nullptr);
  for (auto& [key, sampler] : samplers_) fn.vkDestroySampler(device, sampler, nullptr);
  if (placeholder_sampler_) fn.vkDestroySampler(device, placeholder_sampler_, nullptr);
}

void TextureCache::AdaptToDevice(HostFormat& format) const {
  if (bc_supported_) {
    return;
  }
  switch (format.format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      format.format = VK_FORMAT_R8G8B8A8_UNORM;
      format.decode = Decode::kBc1;
      break;
    case VK_FORMAT_BC2_UNORM_BLOCK:
      format.format = VK_FORMAT_R8G8B8A8_UNORM;
      format.decode = Decode::kBc2;
      break;
    case VK_FORMAT_BC3_UNORM_BLOCK:
      format.format = VK_FORMAT_R8G8B8A8_UNORM;
      format.decode = Decode::kBc3;
      break;
    case VK_FORMAT_BC4_UNORM_BLOCK:
      format.format = VK_FORMAT_R8_UNORM;
      format.decode = Decode::kBc4;
      break;
    case VK_FORMAT_BC5_UNORM_BLOCK:
      format.format = VK_FORMAT_R8G8_UNORM;
      format.decode = Decode::kBc5;
      break;
    default:
      break;
  }
}

bool TextureCache::FormatUsable(VkFormat format) {
  if (auto it = usable_formats_.find(format); it != usable_formats_.end()) {
    return it->second;
  }
  constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  VkFormatProperties properties{};
  context_.device().vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
      context_.device().physical_device(), format, &properties);
  const bool usable = (properties.optimalTilingFeatures & kNeeded) == kNeeded;
  if (!usable) {
    REXLOG_WARN("[renderer] the device cannot sample Vulkan format {}: those textures are black", int(format));
  }
  usable_formats_.emplace(format, usable);
  return usable;
}

uint32_t TextureCache::DecodedBytesPerTexel(Decode decode) {
  switch (decode) {
    case Decode::kBc4: return 1;
    case Decode::kBc5: return 2;
    default: return 4;
  }
}

void TextureCache::DecodeBlocks(Decode decode, const uint8_t* blocks, uint32_t blocks_x, uint32_t blocks_y,
                                uint32_t slices, uint8_t* out) {
  const uint32_t texel_bytes = DecodedBytesPerTexel(decode);
  const size_t block_bytes = (decode == Decode::kBc1 || decode == Decode::kBc4) ? 8 : 16;
  const size_t out_pitch = size_t(blocks_x) * 4 * texel_bytes;
  for (uint32_t s = 0; s < slices; ++s) {
    for (uint32_t by = 0; by < blocks_y; ++by) {
      for (uint32_t bx = 0; bx < blocks_x; ++bx, blocks += block_bytes) {
        uint8_t* texels = out + (size_t(by) * 4) * out_pitch + size_t(bx) * 4 * texel_bytes;
        switch (decode) {
          case Decode::kBc1:
            bc::DecodeColorBlock(blocks, true, texels, out_pitch);
            break;
          case Decode::kBc2:
            bc::DecodeColorBlock(blocks + 8, false, texels, out_pitch);
            bc::DecodeExplicitAlpha(blocks, texels, out_pitch);
            break;
          case Decode::kBc3:
            bc::DecodeColorBlock(blocks + 8, false, texels, out_pitch);
            bc::DecodeChannelBlock(blocks, texels + 3, out_pitch, 4);
            break;
          case Decode::kBc4:
            bc::DecodeChannelBlock(blocks, texels, out_pitch, 1);
            break;
          case Decode::kBc5:
            bc::DecodeChannelBlock(blocks, texels, out_pitch, 2);
            bc::DecodeChannelBlock(blocks + 8, texels + 1, out_pitch, 2);
            break;
          case Decode::kNone:
            break;
        }
      }
    }
    out += out_pitch * size_t(blocks_y) * 4;
  }
}

bool TextureCache::Initialize() {
  // Mali and most Android GPUs have no BC textures (Vulkan's textureCompressionBC), which nearly every Xbox 360
  // texture is; creating such an image anyway is not refused by every driver, and the upload then crashes inside it.
  {
    const auto& instance_fn = context_.device().vulkan_instance()->functions();
    constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    bc_supported_ = context_.device().properties().textureCompressionBC && REXCVAR_GET(nfsmw_texturas_bc_gpu);
    for (VkFormat bc : {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK}) {
      VkFormatProperties properties{};
      instance_fn.vkGetPhysicalDeviceFormatProperties(context_.device().physical_device(), bc, &properties);
      if ((properties.optimalTilingFeatures & kNeeded) != kNeeded) {
        bc_supported_ = false;
      }
    }
    if (!bc_supported_) {
      REXLOG_INFO("[renderer] the device has no BC textures: DXT textures are decoded on the CPU when uploaded");
    }
  }
  // Black placeholders, one per dimension, for textures that cannot be made.
  xenos::xe_gpu_texture_fetch_t none{};
  HostFormat black;
  GetHostFormat(xenos::TextureFormat::k_8_8_8_8, black);
  for (uint32_t d = 0; d < 3; ++d) {
    Texture& texture = placeholders_[d];
    const auto dimension = TextureDimension(d);
    if (!CreateTexture(texture, none, black, dimension, 1, 1, dimension == TextureDimension::kCube ? 6 : 1, 1)) {
      return false;
    }
    VkCommandBuffer commands = frames_.UploadCommands();
    const VkClearColorValue clear{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, texture.layers};
    context_.fn().vkCmdClearColorImage(commands, texture.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
  }
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  return context_.fn().vkCreateSampler(context_.handle(), &sampler, nullptr, &placeholder_sampler_) == VK_SUCCESS;
}

TextureBinding TextureCache::Placeholder(TextureDimension dimension) {
  HostFormat black;
  GetHostFormat(xenos::TextureFormat::k_8_8_8_8, black);
  TextureBinding binding;
  binding.view = ViewFor(placeholders_[uint32_t(dimension)], 0x688 /* xyzw */, black);
  binding.sampler = placeholder_sampler_;
  return binding;
}

bool TextureCache::CreateTexture(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& format,
                                 TextureDimension dimension, uint32_t width, uint32_t height,
                                 uint32_t depth_or_layers, uint32_t levels) {
  (void)fetch;
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  texture = Texture{};
  texture.format = format.format;
  texture.width = width;
  texture.height = height;
  texture.levels = levels;
  texture.depth = dimension == TextureDimension::k3D ? depth_or_layers : 1;
  texture.layers = dimension == TextureDimension::k3D ? 1 : depth_or_layers;
  texture.view_type = dimension == TextureDimension::k3D     ? VK_IMAGE_VIEW_TYPE_3D
                      : dimension == TextureDimension::kCube ? VK_IMAGE_VIEW_TYPE_CUBE
                                                             : VK_IMAGE_VIEW_TYPE_2D;

  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = dimension == TextureDimension::kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.imageType = dimension == TextureDimension::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = format.format;
  info.extent = {width, height, texture.depth};
  info.mipLevels = levels;
  info.arrayLayers = texture.layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (fn.vkCreateImage(device, &info, nullptr, &texture.image) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  fn.vkGetImageMemoryRequirements(device, texture.image, &requirements);
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex =
      context_.FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (allocate.memoryTypeIndex == UINT32_MAX ||
      fn.vkAllocateMemory(device, &allocate, nullptr, &texture.memory) != VK_SUCCESS ||
      fn.vkBindImageMemory(device, texture.image, texture.memory, 0) != VK_SUCCESS) {
    DestroyTextureLater(texture);
    return false;
  }
  texture.bytes = requirements.size;

  VkCommandBuffer commands = frames_.UploadCommands();
  if (!commands) {
    return false;
  }
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = texture.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, texture.layers};
  fn.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &barrier);
  return true;
}

void TextureCache::DestroyTextureLater(Texture& texture) {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  std::vector<VkImageView> views;
  for (auto& [swizzle, view] : texture.views) views.push_back(view);
  frames_.Defer([&fn, device, views, image = texture.image, memory = texture.memory] {
    for (VkImageView view : views) fn.vkDestroyImageView(device, view, nullptr);
    if (image) fn.vkDestroyImage(device, image, nullptr);
    if (memory) fn.vkFreeMemory(device, memory, nullptr);
  });
  texture.views.clear();
  texture.image = VK_NULL_HANDLE;
  texture.memory = VK_NULL_HANDLE;
}

uint64_t TextureCache::Fingerprint(const xenos::xe_gpu_texture_fetch_t& fetch, TextureDimension dimension) const {
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  // GetTextureTotalSize is declared by the SDK but not defined: the layout gives the same extents.
  const texture_util::TextureGuestLayout extents = texture_util::GetGuestTextureLayout(
      GuestDimension(dimension), fetch.pitch, width_minus_1 + 1, height_minus_1 + 1, depth_minus_1 + 1, fetch.tiled,
      fetch.format, fetch.packed_mips, base_page != 0, mip_max);
  const uint32_t base_size = extents.base.level_data_extent_bytes;
  const uint32_t mip_size = extents.mips_total_extent_bytes;
  ++stats_.fingerprints;
  stats_.fingerprint_bytes += uint64_t(base_page ? base_size : 0) + uint64_t(mip_page ? mip_size : 0);
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  if (base_page && base_size) {
    XXH3_64bits_update(&state, memory_->TranslatePhysical(base_page << 12), base_size);
  }
  if (mip_page && mip_size) {
    XXH3_64bits_update(&state, memory_->TranslatePhysical(mip_page << 12), mip_size);
  }
  return XXH3_64bits_digest(&state);
}

bool TextureCache::Upload(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& format,
                          TextureDimension dimension) {
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  const uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1, depth = depth_minus_1 + 1;
  const bool is_3d = dimension == TextureDimension::k3D;
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      GuestDimension(dimension), fetch.pitch, width, height, depth, fetch.tiled, fetch.format, fetch.packed_mips,
      base_page != 0, mip_max);
  const uint32_t log2_bytes = Log2(format.bytes_per_block);
  // Contiguous bytes along X in the 2D tiled layout: 16, or 8 for 1-byte blocks.
  const uint32_t run_bytes = std::min(16u, 8u << log2_bytes);
  const uint32_t run_blocks = run_bytes >> log2_bytes;
  const auto endian = fetch.endianness;
  const uint8_t* physical = memory_->TranslatePhysical(0);

  VkCommandBuffer commands = frames_.UploadCommands();
  if (!commands) {
    return false;
  }
  std::vector<VkBufferImageCopy> regions;
  for (uint32_t level = 0; level < texture.levels; ++level) {
    const uint32_t level_width = std::max(width >> level, 1u);
    const uint32_t level_height = std::max(height >> level, 1u);
    const uint32_t level_depth = is_3d ? std::max(depth >> level, 1u) : 1;
    const uint32_t blocks_x = (level_width + format.block_size - 1) / format.block_size;
    const uint32_t blocks_y = (level_height + format.block_size - 1) / format.block_size;
    const uint32_t layers = texture.layers;

    // Where this level lives: the base level at the base address; the mips after the mip address, the smallest
    // ones packed into one tail with an offset inside it.
    const bool packed = layout.packed_level != UINT32_MAX && level >= layout.packed_level && level != 0;
    const uint32_t stored_level = packed ? layout.packed_level : level;
    const texture_util::TextureGuestLayout::Level& guest_level =
        stored_level == 0 ? layout.base : layout.mips[stored_level];
    const uint32_t level_address =
        stored_level == 0 ? (base_page << 12) : (mip_page << 12) + layout.mip_offsets_bytes[stored_level];
    if (!level_address || (stored_level != 0 && !mip_page)) {
      continue;
    }
    uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
    if (packed) {
      texture_util::GetPackedMipOffset(width, height, is_3d ? depth : 1, fetch.format, level, offset_x, offset_y,
                                       offset_z);
    }
    const uint32_t row_pitch_blocks = guest_level.row_pitch_bytes >> log2_bytes;
    const uint32_t height_blocks_aligned = guest_level.z_slice_stride_block_rows;

    const VkDeviceSize slice_bytes = VkDeviceSize(blocks_x) * blocks_y * format.bytes_per_block;
    const VkDeviceSize level_bytes = slice_bytes * level_depth * layers;
    // Without BC textures the blocks are gathered aside and decoded into the upload memory afterwards.
    const bool decode = format.decode != Decode::kNone;
    const VkDeviceSize upload_bytes =
        decode ? VkDeviceSize(blocks_x) * 4 * blocks_y * 4 * DecodedBytesPerTexel(format.decode) * level_depth * layers
               : level_bytes;
    FrameRing::Allocation allocation;
    if (!frames_.Allocate(upload_bytes, 16, allocation)) {
      REXLOG_WARN("[renderer] texture level of {} bytes does not fit in the frame's upload memory", upload_bytes);
      return false;
    }
    if (decode) {
      compressed_.resize(size_t(level_bytes));
    }
    uint8_t* const gathered = decode ? compressed_.data() : allocation.data;
    uint8_t* dest = gathered;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      const uint8_t* layer_source = physical + level_address + size_t(layer) * guest_level.array_slice_stride_bytes;
      for (uint32_t z = 0; z < level_depth; ++z) {
        const int32_t sz = int32_t(z + offset_z);
        for (uint32_t by = 0; by < blocks_y; ++by) {
          const int32_t sy = int32_t(by + offset_y);
          if (!fetch.tiled) {
            // Linear: one row is contiguous.
            const size_t row = (size_t(sz) * height_blocks_aligned + size_t(sy)) * guest_level.row_pitch_bytes +
                               (size_t(offset_x) << log2_bytes);
            std::memcpy(dest, layer_source + row, size_t(blocks_x) << log2_bytes);
            dest += size_t(blocks_x) << log2_bytes;
            continue;
          }
          if (is_3d) {
            for (uint32_t bx = 0; bx < blocks_x; ++bx) {
              const size_t source = size_t(texture_util::GetTiledOffset3D(int32_t(bx + offset_x), sy, sz,
                                                                          row_pitch_blocks, height_blocks_aligned,
                                                                          log2_bytes));
              std::memcpy(dest, layer_source + source, format.bytes_per_block);
              dest += format.bytes_per_block;
            }
            continue;
          }
          // 2D tiled: runs of kRunBytes bytes (8 blocks at most) are contiguous in guest memory.
          uint32_t bx = 0;
          while (bx < blocks_x) {
            const int32_t sx = int32_t(bx + offset_x);
            const uint8_t* source = layer_source + TiledOffset2D(sx, sy, row_pitch_blocks, log2_bytes);
            if ((uint32_t(sx) & (run_blocks - 1)) == 0 && bx + run_blocks <= blocks_x) {
              std::memcpy(dest, source, run_bytes);
              dest += run_bytes;
              bx += run_blocks;
            } else {
              std::memcpy(dest, source, format.bytes_per_block);
              dest += format.bytes_per_block;
              ++bx;
            }
          }
        }
      }
    }
    ++stats_.uploads;
    stats_.upload_bytes += upload_bytes;
    if (format.endian_unit) {
      SwapEndian(gathered, size_t(level_bytes), endian);
    }
    if (decode) {
      DecodeBlocks(format.decode, gathered, blocks_x, blocks_y, level_depth * layers, allocation.data);
    }
    VkBufferImageCopy region{};
    region.bufferOffset = allocation.offset;
    region.bufferRowLength = blocks_x * format.block_size;
    region.bufferImageHeight = blocks_y * format.block_size;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
    region.imageExtent = {level_width, level_height, level_depth};
    regions.push_back(region);
    context_.fn().vkCmdCopyBufferToImage(commands, allocation.buffer, texture.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                         &regions.back());
  }
  return !regions.empty();
}

VkImageView TextureCache::ViewFor(Texture& texture, uint32_t swizzle, const HostFormat& format) {
  if (auto it = texture.views.find(swizzle); it != texture.views.end()) {
    return it->second;
  }
  // The fetch swizzle picks, for each output component, a guest component (0-3) or a constant (4 = 0, 5 = 1).
  auto component = [&](uint32_t i) {
    const uint32_t source = (swizzle >> (i * 3)) & 0x7;
    if (source == 4) return VK_COMPONENT_SWIZZLE_ZERO;
    if (source == 5) return VK_COMPONENT_SWIZZLE_ONE;
    if (source > 5) return VK_COMPONENT_SWIZZLE_IDENTITY;
    return VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + format.channel_of_component[source]);
  };
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = texture.image;
  info.viewType = texture.view_type;
  info.format = texture.format;
  info.components = {component(0), component(1), component(2), component(3)};
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, texture.layers};
  VkImageView view = VK_NULL_HANDLE;
  if (context_.fn().vkCreateImageView(context_.handle(), &info, nullptr, &view) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  texture.views.emplace(swizzle, view);
  return view;
}

VkImageView TextureCache::ResolvedView(const ResolvedTexture& resolved, uint32_t swizzle) {
  const uint64_t key = (resolved.id << 16) ^ swizzle ^ (uint64_t(resolved.swap_red_blue) << 15);
  if (auto it = resolved_views_.find(key); it != resolved_views_.end()) {
    return it->second;
  }
  auto component = [&](uint32_t i) {
    uint32_t source = (swizzle >> (i * 3)) & 0x7;
    if (source == 4) return VK_COMPONENT_SWIZZLE_ZERO;
    if (source == 5) return VK_COMPONENT_SWIZZLE_ONE;
    if (source > 5) return VK_COMPONENT_SWIZZLE_IDENTITY;
    if (resolved.image.depth) return VK_COMPONENT_SWIZZLE_R;  // depth is the only channel
    if (resolved.swap_red_blue && (source == 0 || source == 2)) source ^= 2;
    return VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + source);
  };
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = resolved.image.image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = resolved.image.format;
  info.components = {component(0), component(1), component(2), component(3)};
  info.subresourceRange = {resolved.image.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                                : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  if (context_.fn().vkCreateImageView(context_.handle(), &info, nullptr, &view) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  resolved_views_.emplace(key, view);
  return view;
}

// The game renders its dynamic cubemap one face at a time and resolves each face to its slice of the cubemap's
// memory. When all six slices are resolved textures, they are copied into the layers of a cube image. The copy goes
// to the upload command buffer, which runs before the frame's work, so the cube shows the previous frame's faces.
TextureBinding TextureCache::ResolvedCube(const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  const uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  const texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::kCube, fetch.pitch, width, height, 6, fetch.tiled,
                                          fetch.format, false, true, 0);
  const uint32_t face_stride = layout.base.array_slice_stride_bytes;
  std::array<const ResolvedTexture*, 6> faces{};
  uint64_t version = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    faces[face] = render_targets_.Resolved((base_page << 12) + face * face_stride);
    if (!faces[face] || faces[face]->image.depth || faces[face]->image.width < width ||
        faces[face]->image.height < height || faces[face]->image.format != faces[0]->image.format) {
      return {};
    }
    version = XXH3_64bits_withSeed(&faces[face]->write_count, sizeof(uint64_t), version ^ faces[face]->id);
  }

  HostFormat format;
  format.format = faces[0]->image.format;
  format.bytes_per_block = 4;
  if (faces[0]->swap_red_blue) {
    format.channel_of_component = {2, 1, 0, 3};
  }
  const uint64_t key = TextureKey(fetch, TextureDimension::kCube) ^ 0x9E3779B97F4A7C15ull;
  Texture& texture = textures_[key];
  if (!texture.image) {
    if (!CreateTexture(texture, fetch, format, TextureDimension::kCube, width, height, 6, 1)) {
      textures_.erase(key);
      return {};
    }
    total_bytes_ += texture.bytes;
    REXLOG_INFO("[renderer] cubemap {:08X} {}x{} from its resolved faces", base_page << 12, width, height);
  }
  version |= 1;  // never 0, which means "not copied"
  if (texture.fingerprint != version) {
    const VkCommandBuffer commands = frames_.UploadCommands();
    if (!commands) {
      return {};
    }
    std::array<VkImageCopy, 6> copies{};
    for (uint32_t face = 0; face < 6; ++face) {
      copies[face].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copies[face].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
      copies[face].extent = {width, height, 1};
      context_.extra().vkCmdCopyImage(commands, faces[face]->image.image, VK_IMAGE_LAYOUT_GENERAL, texture.image,
                                      VK_IMAGE_LAYOUT_GENERAL, 1, &copies[face]);
    }
    texture.fingerprint = version;
  }
  texture.used_frame = frames_.frame_number();
  TextureBinding binding;
  binding.view = ViewFor(texture, fetch.swizzle, format);
  binding.inv_width = 1.0f / float(width);
  binding.inv_height = 1.0f / float(height);
  return binding;
}

void TextureCache::ForgetResolved(uint64_t id) {
  for (auto it = resolved_views_.begin(); it != resolved_views_.end();) {
    if ((it->first >> 16) != id) {
      ++it;
      continue;
    }
    const VkImageView view = it->second;
    frames_.Defer([&fn = context_.fn(), device = context_.handle(), view] { fn.vkDestroyImageView(device, view, nullptr); });
    it = resolved_views_.erase(it);
  }
}

VkSampler TextureCache::SamplerFor(const xenos::xe_gpu_texture_fetch_t& fetch, TextureDimension dimension) {
  const uint32_t key = (fetch.dword_0 & 0x7FC00u) >> 10 | (fetch.dword_3 & 0x0FF80000u) >> 10 |
                       (fetch.dword_4 & 0x3FFFFCu) << 10;  // clamps, filters, aniso, mip range, lod bias
  const uint64_t full_key = uint64_t(key) | (uint64_t(fetch.border_color) << 40) | (uint64_t(dimension) << 44) |
                            (uint64_t(diagnostic_lod_bias_) << 48);
  if (auto it = samplers_.find(full_key); it != samplers_.end()) {
    return it->second;
  }
  auto address = [&](xenos::ClampMode mode) {
    switch (mode) {
      case xenos::ClampMode::kRepeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case xenos::ClampMode::kMirroredRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case xenos::ClampMode::kClampToBorder:
      case xenos::ClampMode::kMirrorClampToBorder:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      case xenos::ClampMode::kMirrorClampToEdge:
      case xenos::ClampMode::kMirrorClampToHalfway:
        return context_.device().properties().samplerMirrorClampToEdge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      default:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
  };
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = fetch.mag_filter == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = fetch.min_filter == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                      : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = address(fetch.clamp_x);
  info.addressModeV = address(fetch.clamp_y);
  info.addressModeW = address(fetch.clamp_z);
  info.mipLodBias = float(fetch.lod_bias) * (1.0f / 32.0f) + float(diagnostic_lod_bias_);
  const uint32_t aniso = uint32_t(fetch.aniso_filter);
  if (aniso >= 1 && aniso <= 5 && context_.device().properties().samplerAnisotropy) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = float(1u << (aniso - 1));
  }
  info.minLod = float(fetch.mip_min_level);
  info.maxLod = fetch.mip_filter == xenos::TextureFilter::kBaseMap ? info.minLod : float(fetch.mip_max_level);
  info.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                                            : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler sampler = VK_NULL_HANDLE;
  if (context_.fn().vkCreateSampler(context_.handle(), &info, nullptr, &sampler) != VK_SUCCESS) {
    return placeholder_sampler_;
  }
  samplers_.emplace(full_key, sampler);
  return sampler;
}

TextureBinding TextureCache::Get(const GuestRegisters& registers, uint32_t fetch_index, TextureDimension dimension) {
  const xenos::xe_gpu_texture_fetch_t fetch = registers.TextureFetch(fetch_index);
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return Placeholder(dimension);
  }
  ++stats_.lookups;
  TextureBinding binding;
  binding.sampler = SamplerFor(fetch, dimension);

  // A resolve wrote this address: the render target copy, not guest memory.
  if (const ResolvedTexture* resolved = render_targets_.Resolved(fetch.base_address << 12)) {
    if (dimension == TextureDimension::k2D) {
      binding.view = ResolvedView(*resolved, fetch.swizzle);
      binding.inv_width = 1.0f / float(resolved->image.width);
      binding.inv_height = 1.0f / float(resolved->image.height);
      if (binding.view) {
        return binding;
      }
    }
  }

  // A dynamic cubemap: its faces are resolves (the car's reflection and the rear-view mirror).
  if (dimension == TextureDimension::kCube) {
    TextureBinding cube = ResolvedCube(fetch);
    if (cube.view) {
      cube.sampler = binding.sampler;
      return cube;
    }
  }

  HostFormat format;
  if (!GetHostFormat(fetch.format, format)) {
    static bool warned[64] = {};
    if (!warned[uint32_t(fetch.format) & 63]) {
      warned[uint32_t(fetch.format) & 63] = true;
      REXLOG_WARN("[renderer] texture format {} is not supported: a black texture is used", uint32_t(fetch.format));
    }
    return Placeholder(dimension);
  }
  AdaptToDevice(format);
  if (!FormatUsable(format.format)) {
    return Placeholder(dimension);
  }

  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  if (!base_page) {
    return Placeholder(dimension);
  }
  const uint32_t layers = dimension == TextureDimension::kCube ? 6 : depth_minus_1 + 1;
  const uint32_t levels = mip_page ? mip_max + 1 : 1;

  const uint64_t key = TextureKey(fetch, dimension);
  Texture& texture = textures_[key];
  const uint64_t frame = frames_.frame_number();
  if (!texture.image) {
    const auto create_start = std::chrono::steady_clock::now();
    const bool created =
        CreateTexture(texture, fetch, format, dimension, width_minus_1 + 1, height_minus_1 + 1, layers, levels);
    ++stats_.created;
    stats_.create_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - create_start)
                                     .count());
    if (!created) {
      textures_.erase(key);
      return Placeholder(dimension);
    }
    total_bytes_ += texture.bytes;
  }
  const bool stable = texture.fingerprint && frame - texture.changed_frame >= kStableFrames;
  if (texture.validated_frame != frame && (!stable || (frame + key) % kStableCheckInterval == 0)) {
    texture.validated_frame = frame;
    const uint64_t fingerprint = Fingerprint(fetch, dimension) | 1;  // never 0, which means "not uploaded"
    if (fingerprint != texture.fingerprint) {
      if (!Upload(texture, fetch, format, dimension)) {
        return Placeholder(dimension);
      }
      texture.fingerprint = fingerprint;
      texture.changed_frame = frame;
    }
  }
  texture.used_frame = frame;
  binding.view = ViewFor(texture, fetch.swizzle, format);
  binding.inv_width = 1.0f / float(texture.width);
  binding.inv_height = 1.0f / float(texture.height);
  return binding.view ? binding : Placeholder(dimension);
}

void TextureCache::EndFrame() {
  if (total_bytes_ <= kBudgetBytes) {
    return;
  }
  const uint64_t frame = frames_.frame_number();
  for (auto it = textures_.begin(); it != textures_.end() && total_bytes_ > kBudgetBytes * 3 / 4;) {
    if (it->second.used_frame + kEvictAfterFrames < frame) {
      total_bytes_ -= it->second.bytes;
      DestroyTextureLater(it->second);
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace nfsmw::renderer
