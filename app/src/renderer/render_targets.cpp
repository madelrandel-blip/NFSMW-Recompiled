#include "render_targets.h"

#include "../nfsmw_nativo_destinos.h"
#include "frame_stats.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_escala, 100, "NFSMW",
                     "Los dos mapas de sombras de 1600x1600 del juego se dibujan y se resuelven a este porcentaje. "
                     "100 = como la Xbox 360 (1600, que son 2000 de sus 2048 baldosas de EDRAM); 64 = como la "
                     "version de PC de este mismo juego (1024). Se toma al crear el primer mapa")
    .range(50, 100);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_resolver_sin_copia, true, "NFSMW",
                    "Al resolver un destino entero a una textura del mismo tamano y formato, intercambiar las "
                    "imagenes en vez de copiar los pixeles. La imagen no cambia; si el juego vuelve a dibujar en "
                    "ese destino sin borrarlo antes, se restaura la copia");

namespace nfsmw::renderer {

namespace {

// The game's two shadow maps: 1600x1600 (2000 of the 2048 EDRAM tiles, not a quality choice; the PC version of
// the game uses 1024). No other render target has this pitch.
constexpr uint32_t kShadowSide = 1600;

// Image pixels per guest pixel for a render target of this pitch: the shadow maps at nfsmw_nativo_sombras_escala
// percent (read once, so images are never mixed), everything else 1. The maps are drawn into the smaller image
// and resolved at that size: the scene samples them with normalized coordinates and the texel size of the
// resolved image (TextureCache), so only their detail goes down.
float ScaleFor(uint32_t pitch) {
  static const float shadow_scale =
      float(std::clamp(REXCVAR_GET(nfsmw_nativo_sombras_escala), 50, 100)) / 100.0f;
  return pitch == kShadowSide ? shadow_scale : 1.0f;
}

// A side scaled to a whole number of 16-pixel units (an exact multiple keeps the 8-pixel resolve alignment
// whole in the image).
uint32_t ScaledSide(uint32_t side, float scale) {
  return scale >= 1.0f ? side : std::max<uint32_t>(64, (uint32_t(std::lround(float(side) * scale)) + 15) & ~15u);
}

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kMaxTargetHeight = 2048;
// Resolves up to this many texels are written back to guest memory (the 64x64 exposure measurements are 4096).
constexpr uint64_t kMaxReadbackTexels = 4096;
// Frames a depth target must go unread before its passes stop storing it.
constexpr uint64_t kDiscardAfterFrames = 60;

uint64_t TargetKey(uint32_t edram_base, uint32_t format, uint32_t pitch) {
  return (uint64_t(edram_base) << 32) | (uint64_t(format) << 16) | pitch;
}

// The EDRAM does not say how tall a render target is. The game never draws below the pitch nor below 720 rows, so
// this height holds everything it draws (the 1600x1600 shadow map included).
uint32_t TargetHeight(uint32_t pitch) {
  return std::min(kMaxTargetHeight, std::max<uint32_t>(720, (pitch + 15) & ~15u));
}

int32_t SignExtend15(uint32_t value) { return int32_t(value << 17) >> 17; }

// Direct3D 11 16.8 fixed point with rounding (rex::ui::FloatToD3D11Fixed16p8).
int32_t Fixed16p8(float value) {
  if (!(std::abs(value) >= 1.0f / 512.0f)) {
    return 0;
  }
  return int32_t(std::lround(std::clamp(double(value) * 256.0, -2147483392.0, 2147483392.0)));
}

}  // namespace

RenderTargets::~RenderTargets() {
  frames_.WaitIdle();
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  auto destroy = [&](Image& image) {
    if (image.sample_view && image.sample_view != image.view) {
      fn.vkDestroyImageView(device, image.sample_view, nullptr);
    }
    if (image.view) fn.vkDestroyImageView(device, image.view, nullptr);
    if (image.image) fn.vkDestroyImage(device, image.image, nullptr);
    if (image.memory) fn.vkFreeMemory(device, image.memory, nullptr);
  };
  for (auto& [key, image] : color_targets_) destroy(image);
  for (auto& [key, image] : depth_targets_) destroy(image);
  for (auto& [key, texture] : resolved_) destroy(texture.image);
  for (auto& [key, image] : resolve_scratch_) destroy(image);
}

bool RenderTargets::Initialize() { return depth_resolver_.Initialize(); }

void RenderTargets::RestoreSwapped(VkCommandBuffer commands, const Image& target) {
  if (!target.swapped_into) {
    return;
  }
  const uint32_t address = std::exchange(target.swapped_into, 0);
  const ResolvedTexture* texture = Resolved(address);
  // Gone or replaced since: nothing of this target's is left to bring back.
  if (!texture || texture->image.width != target.width || texture->image.height != target.height ||
      texture->image.format != target.format) {
    return;
  }
  TransferBarrier(context_, commands);
  VkImageCopy region{};
  const VkImageAspectFlags aspect =
      target.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                   : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
  region.srcSubresource = {aspect, 0, 0, 1};
  region.dstSubresource = {aspect, 0, 0, 1};
  region.extent = {target.width, target.height, 1};
  context_.extra().vkCmdCopyImage(commands, texture->image.image, VK_IMAGE_LAYOUT_GENERAL, target.image,
                                  VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  TransferBarrier(context_, commands);
  ++Stats().counters().swap_restores;
}

void RenderTargets::FlushClear(VkCommandBuffer commands, const Image& target) {
  if (!target.clear_pending) {
    return;
  }
  target.clear_pending = false;
  TransferBarrier(context_, commands);
  if (target.depth) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    context_.extra().vkCmdClearDepthStencilImage(commands, target.image, VK_IMAGE_LAYOUT_GENERAL,
                                                 &target.clear_value.depthStencil, 1, &range);
    ++Stats().counters().depth_clears;
    Stats().counters().depth_clear_pixels += uint64_t(target.width) * target.height;
  } else {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context_.fn().vkCmdClearColorImage(commands, target.image, VK_IMAGE_LAYOUT_GENERAL, &target.clear_value.color, 1,
                                       &range);
    ++Stats().counters().color_clears;
    Stats().counters().color_clear_pixels += uint64_t(target.width) * target.height;
  }
  Stats().Trace("  clear {} {:03X}/{} done now (not as a pass load)", target.depth ? "depth" : "color", target.edram_base,
                target.pitch);
  TransferBarrier(context_, commands);
}

bool RenderTargets::DepthDiscardable(const Image& target) const {
  return target.depth && !target.content_read && target.first_frame != UINT64_MAX &&
         frames_.frame_number() - target.first_frame >= kDiscardAfterFrames;
}

uint32_t RenderTargets::SourcePitchOf(uint32_t resolved_address) const {
  auto it = resolved_source_pitch_.find(resolved_address & 0x1FFFFFFF);
  return it != resolved_source_pitch_.end() ? it->second : 0;
}

void RenderTargets::LearnScenePitch(uint32_t pitch) {
  scene_pitch_ = pitch;
  auto drop = [&](std::unordered_map<uint64_t, Image>& targets) {
    for (auto it = targets.begin(); it != targets.end();) {
      if (it->second.width == pitch && it->second.samples != multisample_) {
        DestroyImageLater(it->second);
        it = targets.erase(it);
      } else {
        ++it;
      }
    }
  };
  drop(color_targets_);
  drop(depth_targets_);
  REXLOG_INFO("[renderer] MSAA {}x for the scene's render targets (pitch {})", uint32_t(multisample_), pitch);
}

Image* RenderTargets::ResolveScratch(const Image& target) {
  if (auto it = resolve_scratch_.find(target.image); it != resolve_scratch_.end()) {
    return &it->second;
  }
  Image image;
  if (!CreateImage(image, target.width, target.height, target.format,
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    return nullptr;
  }
  return &resolve_scratch_.emplace(target.image, image).first->second;
}

void RenderTargets::Barrier(const VulkanContext& context, VkCommandBuffer commands) {
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  context.fn().vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void RenderTargets::TransferBarrier(const VulkanContext& context, VkCommandBuffer commands) {
  // Around resolves, restores and clears: what passes and transfers wrote (attachments, copies, clears, the depth
  // resolve's pass) before what later reads or writes it: transfers, passes (their attachments) and fragment shaders
  // sampling resolved textures. Never the vertex stage: this renderer binds textures to pixel shaders only, so the
  // next pass's geometry need not wait for a copy, and on a tiler it no longer drains the GPU ~17 times per frame.
  constexpr VkPipelineStageFlags kStages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  context.fn().vkCmdPipelineBarrier(commands, kStages, kStages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void RenderTargets::AfterPassBarrier(const VulkanContext& context, VkCommandBuffer commands) {
  // What a render pass writes is only ever used by later passes loading or drawing into the same attachments
  // (fragment tests and color output) and by copies (resolves, restores). Render targets are never sampled
  // directly: shaders read resolved textures, which a copy writes behind a full Barrier. So the vertex stage of
  // the next pass does not wait here, and on a tiler its geometry can be processed while this pass's tiles are
  // still being shaded, instead of the GPU draining between every pass.
  constexpr VkPipelineStageFlags kFragmentStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  context.fn().vkCmdPipelineBarrier(commands, kFragmentStages,
                                    kFragmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    0, 1, &barrier, 0, nullptr, 0, nullptr);
}

bool RenderTargets::CreateImage(Image& image, uint32_t width, uint32_t height, VkFormat format,
                                VkImageUsageFlags usage, VkSampleCountFlagBits samples) {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  image = Image{};
  image.width = width;
  image.height = height;
  image.format = format;
  image.samples = samples;
  image.usage = usage;
  image.depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;

  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  // Imageless framebuffers list the view formats of their attachments, and the image must list the same ones.
  VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  format_list.viewFormatCount = 1;
  format_list.pViewFormats = &format;
  const VkImageUsageFlags attachment_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (context_.imageless_framebuffer() && (usage & attachment_usage)) {
    info.pNext = &format_list;
  }
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (fn.vkCreateImage(device, &info, nullptr, &image.image) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  fn.vkGetImageMemoryRequirements(device, image.image, &requirements);
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex =
      context_.FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (allocate.memoryTypeIndex == UINT32_MAX ||
      fn.vkAllocateMemory(device, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
      fn.vkBindImageMemory(device, image.image, image.memory, 0) != VK_SUCCESS) {
    DestroyImageLater(image);
    image = Image{};
    return false;
  }

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {image.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                       : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, 0, 1};
  if (fn.vkCreateImageView(device, &view, nullptr, &image.view) != VK_SUCCESS) {
    return false;
  }
  if (image.depth) {
    // A sampled view may only have one aspect.
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (fn.vkCreateImageView(device, &view, nullptr, &image.sample_view) != VK_SUCCESS) {
      return false;
    }
  } else {
    image.sample_view = image.view;
  }

  // From UNDEFINED to GENERAL, once, with the contents cleared so that a first read is defined.
  VkCommandBuffer commands = frames_.Commands();
  if (!commands) {
    return false;
  }
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = view.subresourceRange;
  barrier.subresourceRange.aspectMask = image.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                    : VK_IMAGE_ASPECT_COLOR_BIT;
  fn.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &barrier);
  if (image.depth) {
    const VkClearDepthStencilValue clear{1.0f, 0};
    context_.extra().vkCmdClearDepthStencilImage(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                   &barrier.subresourceRange);
  } else {
    const VkClearColorValue clear{};
    fn.vkCmdClearColorImage(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &barrier.subresourceRange);
  }
  Barrier(context_, commands);
  return true;
}

void RenderTargets::DestroyImageLater(Image image) {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  frames_.Defer([&fn, device, image] {
    if (image.sample_view && image.sample_view != image.view) fn.vkDestroyImageView(device, image.sample_view, nullptr);
    if (image.view) fn.vkDestroyImageView(device, image.view, nullptr);
    if (image.image) fn.vkDestroyImage(device, image.image, nullptr);
    if (image.memory) fn.vkFreeMemory(device, image.memory, nullptr);
  });
}

Image* RenderTargets::ColorTarget(uint32_t edram_base, uint32_t format, uint32_t pitch) {
  if (!pitch) {
    return nullptr;
  }
  const uint64_t key = TargetKey(edram_base, format, pitch);
  if (auto it = color_targets_.find(key); it != color_targets_.end()) {
    return &it->second;
  }
  Image image;
  const float scale = ScaleFor(pitch);
  if (!CreateImage(image, ScaledSide(pitch, scale), ScaledSide(TargetHeight(pitch), scale), kColorFormat,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   SamplesFor(pitch))) {
    REXLOG_ERROR("[renderer] could not create a {}x{} color render target", pitch, TargetHeight(pitch));
    return nullptr;
  }
  image.edram_base = edram_base;
  image.pitch = pitch;
  image.scale = float(image.width) / float(pitch);
  REXLOG_INFO("[renderer] color render target: EDRAM base {:03X}, format {}, {}x{}, {} sample(s)", edram_base, format,
              image.width, image.height, uint32_t(image.samples));
  return &color_targets_.emplace(key, image).first->second;
}

Image* RenderTargets::DepthTarget(uint32_t edram_base, uint32_t format, uint32_t pitch) {
  if (!pitch) {
    return nullptr;
  }
  const uint64_t key = TargetKey(edram_base, format, pitch);
  if (auto it = depth_targets_.find(key); it != depth_targets_.end()) {
    return &it->second;
  }
  Image image;
  const float scale = ScaleFor(pitch);
  if (!CreateImage(image, ScaledSide(pitch, scale), ScaledSide(TargetHeight(pitch), scale), context_.depth_format(),
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   SamplesFor(pitch))) {
    REXLOG_ERROR("[renderer] could not create a {}x{} depth render target", pitch, TargetHeight(pitch));
    return nullptr;
  }
  image.edram_base = edram_base;
  image.pitch = pitch;
  image.scale = float(image.width) / float(pitch);
  REXLOG_INFO("[renderer] depth render target: EDRAM base {:03X}, format {}, {}x{}, {} sample(s)", edram_base, format,
              image.width, image.height, uint32_t(image.samples));
  return &depth_targets_.emplace(key, image).first->second;
}

const ResolvedTexture* RenderTargets::Resolved(uint32_t guest_address) const {
  auto it = resolved_.find(guest_address & 0x1FFFFFFF);
  return it != resolved_.end() ? &it->second : nullptr;
}

ResolvedTexture* RenderTargets::ResolveDestination(uint32_t address, uint32_t width, uint32_t height, VkFormat format,
                                                   uint32_t guest_format, bool swap_red_blue) {
  if (!width || !height) {
    return nullptr;
  }
  if (auto it = resolved_.find(address); it != resolved_.end()) {
    ResolvedTexture& existing = it->second;
    if (existing.image.width == width && existing.image.height == height && existing.image.format == format) {
      existing.guest_format = guest_format;
      existing.swap_red_blue = swap_red_blue;
      return &existing;
    }
    // Another size or format at the same address: the old image goes once the GPU is done with it.
    if (resolved_destroyed_) {
      resolved_destroyed_(existing.id);
    }
    DestroyImageLater(existing.image);
    resolved_.erase(it);
  }
  const bool depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
  ResolvedTexture texture;
  if (!CreateImage(texture.image, width, height, format, usage)) {
    REXLOG_ERROR("[renderer] could not create a {}x{} resolved texture", width, height);
    return nullptr;
  }
  texture.guest_format = guest_format;
  texture.swap_red_blue = swap_red_blue;
  texture.id = next_resolved_id_++;
  REXLOG_INFO("[renderer] resolved texture at {:08X}: {}x{}, guest format {}", address, width, height, guest_format);
  return &resolved_.emplace(address, texture).first->second;
}

bool RenderTargets::CopyRectangle(const nativo::RegistrosCopia& r, uint32_t pitch, int32_t& x0, int32_t& y0,
                                  int32_t& x1, int32_t& y1) const {
  // D3D9 sends the rectangle as three vertices in fetch constant 0 (GetResolveInfo in the SDK's draw.cpp).
  const uint32_t type = r.fetch_vertices[0] & 0x3;
  const uint32_t address = r.fetch_vertices[0] >> 2;
  const auto endian = static_cast<xenos::Endian>(r.fetch_vertices[1] & 0x3);
  const uint32_t size = (r.fetch_vertices[1] >> 2) & 0xFFFFFF;
  if (type != uint32_t(xenos::FetchConstantType::kVertex) || size != 3 * 2) {
    return false;
  }
  const uint8_t* vertices = memory_->TranslatePhysical(address * 4);
  const float half_pixel = (r.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
  int32_t fixed[6];
  for (int i = 0; i < 6; ++i) {
    float value;
    std::memcpy(&value, vertices + i * 4, sizeof(value));
    fixed[i] = Fixed16p8(xenos::GpuSwap(value, endian) + half_pixel);
  }
  x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;

  const int32_t offset_x = SignExtend15(r.pa_sc_window_offset & 0x7FFF);
  const int32_t offset_y = SignExtend15((r.pa_sc_window_offset >> 16) & 0x7FFF);
  if ((r.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
    x0 += offset_x;
    y0 += offset_y;
    x1 += offset_x;
    y1 += offset_y;
  }
  // The window scissor, not clamped to the pitch.
  int32_t left = int32_t(r.pa_sc_window_scissor_tl & 0x3FFF);
  int32_t top = int32_t((r.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
  int32_t right = int32_t(r.pa_sc_window_scissor_br & 0x3FFF);
  int32_t bottom = int32_t((r.pa_sc_window_scissor_br >> 16) & 0x3FFF);
  if (!((r.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
    left += offset_x;
    top += offset_y;
    right += offset_x;
    bottom += offset_y;
  }
  left = std::max(left, 0);
  top = std::max(top, 0);
  right = std::max(right, left);
  bottom = std::max(bottom, top);
  x0 = std::clamp(x0, left, right);
  y0 = std::clamp(y0, top, bottom);
  x1 = std::clamp(x1, left, right);
  y1 = std::clamp(y1, top, bottom);
  // Resolves are aligned to 8 pixels.
  x0 &= ~int32_t(7);
  y0 &= ~int32_t(7);
  x1 = (x1 + 7) & ~int32_t(7);
  y1 = (y1 + 7) & ~int32_t(7);
  const int32_t aligned_pitch = int32_t(pitch & ~uint32_t(7));
  x0 = std::min(x0, aligned_pitch);
  x1 = std::min(x1, aligned_pitch);
  return x0 < x1 && y0 < y1;
}

void RenderTargets::Copy(VkCommandBuffer commands, const nativo::RegistrosCopia& r) {
  ++copy_generation_;
  const auto& fn = context_.fn();
  const reg::RB_COPY_CONTROL control{r.rb_copy_control};
  const uint32_t source = r.rb_copy_control & 0x7;  // 0-3 color, 4 depth
  const bool clear_color = (r.rb_copy_control >> 8) & 0x1;
  const bool clear_depth = (r.rb_copy_control >> 9) & 0x1;
  const uint32_t command = (r.rb_copy_control >> 20) & 0x3;
  const bool copy = command == uint32_t(xenos::CopyCommand::kRaw) || command == uint32_t(xenos::CopyCommand::kConvert);
  const uint32_t pitch = r.rb_surface_info & 0x3FFF;
  (void)control;

  TransferBarrier(context_, commands);
  const bool from_depth = source >= xenos::kMaxColorRenderTargets;
  const uint32_t color_info = from_depth ? 0 : r.rb_color_info[source];
  const uint32_t color_format = (color_info >> 16) & 0xF;
  Image* target = from_depth ? DepthTarget(r.rb_depth_info & 0xFFF, (r.rb_depth_info >> 16) & 0x1, pitch)
                             : ColorTarget(color_info & 0xFFF, color_format, pitch);
  if (!target) {
    return;
  }

  int32_t x0, y0, x1, y1;
  if (copy && CopyRectangle(r, pitch, x0, y0, x1, y1)) {
    const uint32_t dest_info = r.rb_copy_dest_info;
    const uint32_t dest_format = (dest_info >> 7) & 0x3F;
    const bool dest_array = (dest_info >> 3) & 0x1;
    const uint32_t dest_pitch = r.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t dest_height = (r.rb_copy_dest_pitch >> 16) & 0x3FFF;
    // The base is moved to the 32x32 tile holding the rectangle's corner; the rest is an offset in the image.
    const uint32_t tile_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t tile_y = uint32_t(y0) & ~uint32_t(31);
    const uint32_t base =
        (r.rb_copy_dest_base + uint32_t(TiledOffset2D(int32_t(tile_x), int32_t(tile_y), dest_pitch, 2))) & 0x1FFFFFFF;
    uint32_t dx = uint32_t(x0) - tile_x;
    uint32_t dy = uint32_t(y0) - tile_y;
    // A target drawn smaller (the shadow maps): the address above stays the game's, the rectangle, the offset and
    // the destination go to the image's scale, so the resolved texture is as small as the map.
    const float scale = target->scale;
    uint32_t dest_width = dest_pitch, dest_rows = dest_height;
    if (scale < 1.0f) {
      x0 = int32_t(std::floor(float(x0) * scale));
      y0 = int32_t(std::floor(float(y0) * scale));
      x1 = int32_t(std::ceil(float(x1) * scale));
      y1 = int32_t(std::ceil(float(y1) * scale));
      dx = uint32_t(std::lround(float(dx) * scale));
      dy = uint32_t(std::lround(float(dy) * scale));
      dest_width = std::max<uint32_t>(1, uint32_t(std::lround(float(dest_pitch) * scale)));
      dest_rows = std::max<uint32_t>(1, uint32_t(std::lround(float(dest_height) * scale)));
    }
    const bool color_dest = dest_format == uint32_t(xenos::ColorFormat::k_8_8_8_8) ||
                            dest_format == uint32_t(xenos::ColorFormat::k_8_8_8_8_A) ||
                            dest_format == uint32_t(xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16);
    ResolvedTexture* texture = nullptr;
    if (dest_array) {
      // 3D and array destinations are not used by this game.
    } else if (from_depth) {
      const uint32_t guest_format = ((r.rb_depth_info >> 16) & 0x1) ? uint32_t(xenos::TextureFormat::k_24_8_FLOAT)
                                                                    : uint32_t(xenos::TextureFormat::k_24_8);
      texture = ResolveDestination(base, dest_width, dest_rows, context_.depth_format(), guest_format, false);
    } else if (color_dest) {
      texture = ResolveDestination(base, dest_width, dest_rows, kColorFormat, dest_format, (dest_info >> 24) & 0x1);
    }
    if (texture && !from_depth) {
      resolved_source_pitch_[base] = pitch;
    }
    if (texture) {
      FrameCounters& c = Stats().counters();
      ++c.resolves;
      c.resolve_pixels += uint64_t(x1 - x0) * uint64_t(y1 - y0);
      c.cleared_then_copied += target->clear_pending ? 1 : 0;
      Stats().Trace("  copy {} {:03X}/{} ({}x) rect {},{}-{},{} -> {:08X} {}x{}{}", from_depth ? "depth" : "color",
                    target->edram_base, target->pitch, uint32_t(target->samples), x0, y0, x1, y1, base, dest_pitch,
                    dest_height, target->clear_pending ? " (target still cleared: nothing drawn since)" : "");
      FlushClear(commands, *target);
      RestoreSwapped(commands, *target);
      target->content_read = true;
    }
    if (texture) {
      const uint32_t requested_width = std::min(uint32_t(x1 - x0), target->width - std::min(uint32_t(x0), target->width));
      const uint32_t requested_height =
          std::min(uint32_t(y1 - y0), target->height - std::min(uint32_t(y0), target->height));
      const uint32_t fit_width = texture->image.width > dx ? texture->image.width - dx : 0;
      const uint32_t fit_height = texture->image.height > dy ? texture->image.height - dy : 0;
      const VkImageAspectFlags aspect = from_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      // A scene drawn larger than its front buffer (internal resolution above the output) is shrunk with a linear
      // blit; otherwise the copy is texel for texel.
      const bool shrink = !from_depth && fit_width && fit_height &&
                          (requested_width * 4 >= fit_width * 5 || requested_height * 4 >= fit_height * 5);
      uint32_t written_width = 0, written_height = 0;
      const bool multisampled = target->samples != VK_SAMPLE_COUNT_1_BIT;
      // A multisampled scaling source is resolved first into a single-sample image of its size.
      const Image* blit_source = target;
      if (shrink && multisampled) {
        blit_source = ResolveScratch(*target);
        if (blit_source) {
          VkImageResolve resolve{};
          resolve.srcSubresource = {aspect, 0, 0, 1};
          resolve.srcOffset = {x0, y0, 0};
          resolve.dstSubresource = {aspect, 0, 0, 1};
          resolve.dstOffset = {x0, y0, 0};
          resolve.extent = {requested_width, requested_height, 1};
          context_.extra().vkCmdResolveImage(commands, target->image, VK_IMAGE_LAYOUT_GENERAL, blit_source->image,
                                             VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
          TransferBarrier(context_, commands);
        }
      }
      if (shrink && !blit_source) {
        // no scratch image: nothing is written
      } else if (shrink) {
        VkImageBlit blit{};
        blit.srcSubresource = {aspect, 0, 0, 1};
        blit.srcOffsets[0] = {x0, y0, 0};
        blit.srcOffsets[1] = {x0 + int32_t(requested_width), y0 + int32_t(requested_height), 1};
        blit.dstSubresource = {aspect, 0, 0, 1};
        blit.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        blit.dstOffsets[1] = {int32_t(dx + fit_width), int32_t(dy + fit_height), 1};
        context_.extra().vkCmdBlitImage(commands, blit_source->image, VK_IMAGE_LAYOUT_GENERAL, texture->image.image,
                          VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        written_width = fit_width;
        written_height = fit_height;
      } else {
        written_width = std::min(requested_width, fit_width);
        written_height = std::min(requested_height, fit_height);
        if (written_width && written_height && multisampled && from_depth) {
          depth_resolver_.Resolve(commands, *target, texture->image, x0, y0, int32_t(dx), int32_t(dy), written_width,
                                  written_height);
        } else if (written_width && written_height && multisampled) {
          VkImageResolve resolve{};
          resolve.srcSubresource = {aspect, 0, 0, 1};
          resolve.srcOffset = {x0, y0, 0};
          resolve.dstSubresource = {aspect, 0, 0, 1};
          resolve.dstOffset = {int32_t(dx), int32_t(dy), 0};
          resolve.extent = {written_width, written_height, 1};
          context_.extra().vkCmdResolveImage(commands, target->image, VK_IMAGE_LAYOUT_GENERAL, texture->image.image,
                                             VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
        } else if (written_width && written_height && REXCVAR_GET(nfsmw_nativo_resolver_sin_copia) && x0 == 0 &&
                   y0 == 0 && dx == 0 && dy == 0 && written_width == target->width &&
                   written_height == target->height && texture->image.width == target->width &&
                   texture->image.height == target->height && texture->image.format == target->format &&
                   texture->image.usage == target->usage) {
          // The whole target into a texture just like it (the shadow maps): the two swap images instead of
          // copying the pixels. The texture now holds what was drawn; the target holds the texture's old contents,
          // which the game clears before drawing again (the next frame starts by clearing the maps). If it draws
          // without clearing, RestoreSwapped copies the contents back first: at worst the same copy, later.
          std::swap(target->image, texture->image.image);
          std::swap(target->memory, texture->image.memory);
          std::swap(target->view, texture->image.view);
          std::swap(target->sample_view, texture->image.sample_view);
          // Its views and descriptors were of the other image.
          if (resolved_destroyed_) {
            resolved_destroyed_(texture->id);
          }
          texture->id = next_resolved_id_++;
          target->swapped_into = base;
          ++Stats().counters().swap_resolves;
        } else if (written_width && written_height) {
          VkImageCopy region{};
          region.srcSubresource = {aspect, 0, 0, 1};
          region.srcOffset = {x0, y0, 0};
          region.dstSubresource = {aspect, 0, 0, 1};
          region.dstOffset = {int32_t(dx), int32_t(dy), 0};
          region.extent = {written_width, written_height, 1};
          context_.extra().vkCmdCopyImage(commands, target->image, VK_IMAGE_LAYOUT_GENERAL, texture->image.image,
                            VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        }
      }
      ++texture->write_count;
      // Written back only at the game's own scale (a scaled image has other dimensions than guest memory).
      if (!from_depth && scale >= 1.0f && written_width && written_height &&
          uint64_t(written_width) * written_height <= kMaxReadbackTexels) {
        Barrier(context_, commands);
        QueueReadback(commands, *texture, r, base, dx, dy, written_width, written_height);
      }
    }
  }

  if (clear_color && !from_depth) {
    // RB_COLOR_CLEAR holds the 8-bit channels in order.
    VkClearColorValue color{};
    for (uint32_t i = 0; i < 4; ++i) {
      color.float32[i] = float((r.rb_color_clear >> (i * 8)) & 0xFF) * (1.0f / 255.0f);
    }
    // Deferred: the next pass on this target clears it as it loads it (loadOp CLEAR).
    target->clear_pending = true;
    target->clear_value.color = color;
    target->has_content = false;
    target->swapped_into = 0;  // cleared: nothing to bring back
    Stats().Trace("  clear color {:03X}/{} ({}x{}) to {:08X}: deferred", target->edram_base, target->pitch,
                  target->width, target->height, r.rb_color_clear);
  }
  if (clear_depth) {
    Image* depth = DepthTarget(r.rb_depth_info & 0xFFF, (r.rb_depth_info >> 16) & 0x1, pitch);
    if (depth) {
      // RB_DEPTH_CLEAR: 24-bit depth above the 8-bit stencil.
      const VkClearDepthStencilValue value{float(r.rb_depth_clear >> 8) / 16777215.0f, r.rb_depth_clear & 0xFF};
      depth->clear_pending = true;
      depth->clear_value.depthStencil = value;
      depth->has_content = false;
      depth->swapped_into = 0;  // cleared: nothing to bring back
      Stats().Trace("  clear depth {:03X}/{} ({}x{}) to {:08X}: deferred", depth->edram_base, depth->pitch,
                    depth->width, depth->height, r.rb_depth_clear);
    }
  }
  TransferBarrier(context_, commands);
}

void RenderTargets::QueueReadback(VkCommandBuffer commands, const ResolvedTexture& texture,
                                  const nativo::RegistrosCopia& r, uint32_t guest_base, uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height) {
  FrameRing::Allocation allocation;
  if (!frames_.Allocate(VkDeviceSize(width) * height * 4, 16, allocation)) {
    return;  // the frame's upload memory is full: this measurement keeps its previous value
  }
  VkBufferImageCopy region{};
  region.bufferOffset = allocation.offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {int32_t(x), int32_t(y), 0};
  region.imageExtent = {width, height, 1};
  context_.fn().vkCmdCopyImageToBuffer(commands, texture.image.image, VK_IMAGE_LAYOUT_GENERAL, allocation.buffer, 1,
                                       &region);
  Readback readback;
  readback.data = allocation.data;
  readback.guest_base = guest_base;
  readback.guest_pitch = r.rb_copy_dest_pitch & 0x3FFF;
  readback.guest_height = (r.rb_copy_dest_pitch >> 16) & 0x3FFF;
  readback.x = x;
  readback.y = y;
  readback.width = width;
  readback.height = height;
  readback.copy_dest_info = r.rb_copy_dest_info;
  frames_.Defer([this, readback] {
    frames_.InvalidateUploadMemory();
    WriteReadback(readback);
  });
}

void RenderTargets::WriteReadback(const Readback& readback) {
  // Each texel is a permutation of its 4 bytes: the red/blue swap and the copy's endianness.
  const bool swap = (readback.copy_dest_info >> 24) & 0x1;
  const uint32_t endian_code = readback.copy_dest_info & 0x7;
  const auto endian = endian_code <= 3 ? static_cast<xenos::Endian>(endian_code) : xenos::Endian::k8in32;
  const uint32_t permutation = xenos::GpuSwap(swap ? uint32_t(0x03000102) : uint32_t(0x03020100), endian);
  const uint8_t order[4] = {uint8_t(permutation), uint8_t(permutation >> 8), uint8_t(permutation >> 16),
                            uint8_t(permutation >> 24)};
  uint8_t* const physical = memory_->TranslatePhysical(0);
  // Upload memory is usually not CPU-cached: one bulk copy out of it, then byte access on the copy.
  std::array<uint8_t, kMaxReadbackTexels * 4> texels;
  std::memcpy(texels.data(), readback.data, size_t(readback.width) * readback.height * 4);
  for (uint32_t j = 0; j < readback.height; ++j) {
    const uint32_t ty = readback.y + j;
    if (readback.guest_height && ty >= readback.guest_height) {
      break;
    }
    const uint8_t* source = texels.data() + size_t(j) * readback.width * 4;
    for (uint32_t i = 0; i < readback.width; ++i, source += 4) {
      const uint32_t tx = readback.x + i;
      if (tx >= readback.guest_pitch) {
        break;
      }
      const uint64_t address =
          uint64_t(readback.guest_base) + uint64_t(TiledOffset2D(int32_t(tx), int32_t(ty), readback.guest_pitch, 2));
      if (address + 4 > 0x20000000) {
        continue;
      }
      uint8_t* dest = physical + address;
      for (uint32_t b = 0; b < 4; ++b) {
        dest[b] = source[order[b]];
      }
    }
  }
}

}  // namespace nfsmw::renderer
