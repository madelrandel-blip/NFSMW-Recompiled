// Resolve of a multisampled depth render target into a single-sample depth texture.
//
// Vulkan 1.1 resolves color with vkCmdResolveImage but has nothing for depth (VK_KHR_depth_stencil_resolve is
// 1.2), so a small pass copies sample 0 of each pixel with a fragment shader that writes gl_FragDepth
// (shaders/depth_resolve.frag). The game samples resolved depth with point sampling, so one sample is what the
// Xbox 360 resolve gives too.

#pragma once

#include "frame_ring.h"
#include "vulkan_context.h"

#include <cstdint>
#include <unordered_map>

namespace nfsmw::renderer {

struct Image;

class DepthResolver {
 public:
  DepthResolver(const VulkanContext& context, FrameRing& frames) : context_(context), frames_(frames) {}
  ~DepthResolver();
  DepthResolver(const DepthResolver&) = delete;
  DepthResolver& operator=(const DepthResolver&) = delete;

  bool Initialize();

  // Copies `width` x `height` pixels of `source` (multisampled, at source_x, source_y) into `destination` (at
  // destination_x, destination_y). Must be recorded outside a render pass.
  bool Resolve(VkCommandBuffer commands, const Image& source, const Image& destination, int32_t source_x,
               int32_t source_y, int32_t destination_x, int32_t destination_y, uint32_t width, uint32_t height);

 private:
  const VulkanContext& context_;
  FrameRing& frames_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
  VkShaderModule fragment_shader_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  // Imageless framebuffers by destination size and usage, kept for the life of the resolver.
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
};

}  // namespace nfsmw::renderer
