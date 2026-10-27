#include "vulkan_context.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/instance.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(nfsmw_imageless_framebuffer, true, "NFSMW",
                    "Vulkan 1.1 renderer: imageless framebuffers where the device supports them (fewer framebuffer "
                    "objects and no framebuffer creation when render targets change). false = classic framebuffers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nfsmw::renderer {

std::unique_ptr<VulkanContext> VulkanContext::Create(const Device* device, std::string& error) {
  if (!device) {
    error = "no Vulkan device";
    return nullptr;
  }
  const Device::Properties& properties = device->properties();
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    error = "the device does not support Vulkan 1.1";
    return nullptr;
  }
  if (!properties.independentBlend) {
    // The game blends its render targets separately (the composite writes color and alpha differently).
    error = "the device lacks independentBlend";
    return nullptr;
  }

  auto context = std::unique_ptr<VulkanContext>(new VulkanContext(device));
  context->queue_family_ = device->queue_family_graphics_compute();
  if (context->queue_family_ == UINT32_MAX) {
    error = "the device has no graphics queue";
    return nullptr;
  }

  const auto& instance_fn = device->vulkan_instance()->functions();
  instance_fn.vkGetPhysicalDeviceMemoryProperties(device->physical_device(), &context->memory_);

  // The queues the driver exposes (a second one in the graphics family could present apart from the frame's
  // submissions) and what the GPU timestamps of the frame are measured with.
  {
    uint32_t family_count = 0;
    instance_fn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    instance_fn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count, families.data());
    for (uint32_t i = 0; i < family_count; ++i) {
      REXLOG_INFO("[renderer] queue family {}: {} queue(s), flags 0x{:X}, timestamps {} bits{}", i,
                  families[i].queueCount, uint32_t(families[i].queueFlags), families[i].timestampValidBits,
                  i == context->queue_family_ ? " (the one used)" : "");
    }
    VkPhysicalDeviceProperties physical_properties{};
    instance_fn.vkGetPhysicalDeviceProperties(device->physical_device(), &physical_properties);
    if (context->queue_family_ < family_count) {
      context->timestamp_bits_ = families[context->queue_family_].timestampValidBits;
    }
    context->timestamp_period_ns_ = double(physical_properties.limits.timestampPeriod);
  }

  // Everything the driver supports, not only what the SDK enabled (to plan zero-copy paths such as Android
  // hardware buffers or importing host memory), and the memory heaps and types (UMA: one heap, device-local and
  // host-visible at once).
  {
    uint32_t count = 0;
    instance_fn.vkEnumerateDeviceExtensionProperties(device->physical_device(), nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    instance_fn.vkEnumerateDeviceExtensionProperties(device->physical_device(), nullptr, &count, extensions.data());
    std::string names;
    for (uint32_t i = 0; i < count; ++i) {
      names += fmt::format("{}{} v{}", i ? ", " : "", extensions[i].extensionName, extensions[i].specVersion);
    }
    REXLOG_INFO("[renderer] the device supports {} extensions: {}", count, names);
    const VkPhysicalDeviceMemoryProperties& memory = context->memory_;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
      REXLOG_INFO("[renderer] memory heap {}: {} MB, flags 0x{:X}", i, memory.memoryHeaps[i].size >> 20,
                  uint32_t(memory.memoryHeaps[i].flags));
    }
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      REXLOG_INFO("[renderer] memory type {}: heap {}, flags 0x{:X}", i, memory.memoryTypes[i].heapIndex,
                  uint32_t(memory.memoryTypes[i].propertyFlags));
    }
  }

  // Depth must be a render target, a copy source and destination (resolves and restores) and sampled (shadows).
  constexpr VkFormatFeatureFlags kDepthFeatures =
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  for (VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    VkFormatProperties format_properties{};
    instance_fn.vkGetPhysicalDeviceFormatProperties(device->physical_device(), candidate, &format_properties);
    if ((format_properties.optimalTilingFeatures & kDepthFeatures) == kDepthFeatures) {
      context->depth_format_ = candidate;
      break;
    }
  }
  if (context->depth_format_ == VK_FORMAT_UNDEFINED) {
    error = "the device has no usable depth/stencil format";
    return nullptr;
  }

  context->uniform_alignment_ = std::max<VkDeviceSize>(properties.minUniformBufferOffsetAlignment, 16);

  context->spirv_version_ = device->extensions().ext_1_2_KHR_spirv_1_4 ? 0x10400 : 0x10300;
  REXLOG_INFO("[renderer] shaders as SPIR-V 1.{}{}", (context->spirv_version_ >> 8) & 0xFF,
              context->spirv_version_ == 0x10400 ? "" : " (the device lacks VK_KHR_spirv_1_4)");

  context->imageless_framebuffer_ = properties.imagelessFramebuffer && REXCVAR_GET(nfsmw_imageless_framebuffer);
  const char* imageless_state = "yes";
  if (!properties.imagelessFramebuffer) {
    imageless_state = "no (the device lacks them)";
  } else if (!context->imageless_framebuffer_) {
    imageless_state = "no (nfsmw_imageless_framebuffer = false)";
  }
  REXLOG_INFO("[renderer] imageless framebuffers: {}", imageless_state);

  auto load = [&](const char* name) { return instance_fn.vkGetDeviceProcAddr(device->device(), name); };
  ExtraFunctions& extra = context->extra_;
  extra.vkCmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(load("vkCmdCopyImage"));
  extra.vkCmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(load("vkCmdBlitImage"));
  extra.vkCmdClearDepthStencilImage =
      reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(load("vkCmdClearDepthStencilImage"));
  extra.vkCmdResolveImage = reinterpret_cast<PFN_vkCmdResolveImage>(load("vkCmdResolveImage"));
  extra.vkCreatePipelineCache = reinterpret_cast<PFN_vkCreatePipelineCache>(load("vkCreatePipelineCache"));
  extra.vkDestroyPipelineCache = reinterpret_cast<PFN_vkDestroyPipelineCache>(load("vkDestroyPipelineCache"));
  extra.vkGetPipelineCacheData = reinterpret_cast<PFN_vkGetPipelineCacheData>(load("vkGetPipelineCacheData"));
  extra.vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(load("vkCmdWriteTimestamp"));
  extra.vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(load("vkGetQueryPoolResults"));
  if (!extra.vkCmdWriteTimestamp || !extra.vkGetQueryPoolResults) {
    context->timestamp_bits_ = 0;  // optional: only for measuring
  }
  if (!extra.vkCmdCopyImage || !extra.vkCmdBlitImage || !extra.vkCmdClearDepthStencilImage ||
      !extra.vkCmdResolveImage || !extra.vkCreatePipelineCache || !extra.vkDestroyPipelineCache ||
      !extra.vkGetPipelineCacheData) {
    error = "core Vulkan functions are missing";
    return nullptr;
  }
  return context;
}

VkResult VulkanContext::Submit(const VkSubmitInfo& submit, VkFence fence) const {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  auto queue = device_->AcquireQueue(queue_family_, 0);
  const auto locked = Clock::now();
  submits_.fetch_add(1, std::memory_order_relaxed);
  const VkResult result = fn().vkQueueSubmit(queue.queue(), 1, &submit, fence);
  const auto end = Clock::now();
  submit_lock_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(locked - start).count()),
                            std::memory_order_relaxed);
  submit_call_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - locked).count()),
                            std::memory_order_relaxed);
  return result;
}

uint32_t VulkanContext::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required,
                                       VkMemoryPropertyFlags preferred) const {
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = memory_.memoryTypes[i].propertyFlags;
    if (!(type_bits & (1u << i)) || (flags & required) != required) {
      continue;
    }
    if ((flags & preferred) == preferred) {
      return i;
    }
    if (fallback == UINT32_MAX) {
      fallback = i;
    }
  }
  return fallback;
}

}  // namespace nfsmw::renderer
