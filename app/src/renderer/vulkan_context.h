// The SDK Vulkan device as the renderer sees it.

#pragma once

#include <rex/ui/vulkan/device.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace nfsmw::renderer {

class VulkanContext {
 public:
  using Device = rex::ui::vulkan::VulkanDevice;

  // nullptr, with the reason in `error`, if the device lacks something the renderer needs.
  static std::unique_ptr<VulkanContext> Create(const Device* device, std::string& error);

  const Device& device() const { return *device_; }
  VkDevice handle() const { return device_->device(); }
  const Device::Functions& fn() const { return device_->functions(); }

  // Core functions the SDK's table does not load.
  struct ExtraFunctions {
    PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
    PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
    PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage = nullptr;
    PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
    PFN_vkCreatePipelineCache vkCreatePipelineCache = nullptr;
    PFN_vkDestroyPipelineCache vkDestroyPipelineCache = nullptr;
    PFN_vkGetPipelineCacheData vkGetPipelineCacheData = nullptr;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp = nullptr;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults = nullptr;
  };
  const ExtraFunctions& extra() const { return extra_; }
  uint32_t queue_family() const { return queue_family_; }

  // GPU timestamps on the graphics queue (0 bits: none), and nanoseconds per tick.
  uint32_t timestamp_bits() const { return timestamp_bits_; }
  double timestamp_period_ns() const { return timestamp_period_ns_; }

  // Submits to the graphics queue under the SDK's queue lock (the presenter shares the queue).
  VkResult Submit(const VkSubmitInfo& submit, VkFence fence) const;
  // vkQueueSubmit calls made through Submit since the last call.
  uint64_t TakeSubmitCount() const { return submits_.exchange(0, std::memory_order_relaxed); }
  // Time spent in Submit since the last call: waiting for the queue lock (the presenter holds it while it submits
  // and presents its own paints), and inside vkQueueSubmit.
  uint64_t TakeSubmitLockNs() const { return submit_lock_ns_.exchange(0, std::memory_order_relaxed); }
  uint64_t TakeSubmitCallNs() const { return submit_call_ns_.exchange(0, std::memory_order_relaxed); }

  // A memory type allowed by `type_bits` with all of `required`; among those, one with `preferred` if there is one.
  // UINT32_MAX if none.
  uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred = 0) const;

  // Depth/stencil format for the guest's 24-bit depth: D24_UNORM_S8_UINT where the driver has it (NVIDIA, Intel),
  // D32_SFLOAT_S8_UINT otherwise (AMD).
  VkFormat depth_format() const { return depth_format_; }
  bool depth_is_float() const { return depth_format_ == VK_FORMAT_D32_SFLOAT_S8_UINT; }

  VkDeviceSize uniform_alignment() const { return uniform_alignment_; }

  // SPIR-V version of the translated shaders: 1.4, the standard, where the device accepts it (Vulkan 1.2, or 1.1
  // with VK_KHR_spirv_1_4, which the SDK enables when present); 1.3 otherwise.
  uint32_t spirv_version() const { return spirv_version_; }

  // Imageless framebuffers (Vulkan 1.2, or 1.1 with VK_KHR_imageless_framebuffer): they only describe the
  // attachments (usage, size, format) and the views come with each vkCmdBeginRenderPass, so one framebuffer serves
  // every set of targets alike and none is created when targets are recreated. Images that may be attachments are
  // then created with a format list (required to match the framebuffer's).
  bool imageless_framebuffer() const { return imageless_framebuffer_; }

 private:
  explicit VulkanContext(const Device* device) : device_(device) {}

  const Device* device_;
  uint32_t queue_family_ = UINT32_MAX;
  VkPhysicalDeviceMemoryProperties memory_{};
  VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
  VkDeviceSize uniform_alignment_ = 256;
  uint32_t spirv_version_ = 0x10300;
  bool imageless_framebuffer_ = false;
  uint32_t timestamp_bits_ = 0;
  double timestamp_period_ns_ = 1.0;
  mutable std::atomic<uint64_t> submits_{0};
  mutable std::atomic<uint64_t> submit_lock_ns_{0};
  mutable std::atomic<uint64_t> submit_call_ns_{0};
  ExtraFunctions extra_;
};

}  // namespace nfsmw::renderer
