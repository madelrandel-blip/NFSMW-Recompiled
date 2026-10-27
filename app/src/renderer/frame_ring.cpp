#include "frame_ring.h"

#include <algorithm>
#include <chrono>

#include <rex/logging.h>

namespace nfsmw::renderer {

namespace {

// Per-draw sets come out of pools of this size; a slot adds pools as it needs them.
constexpr uint32_t kSetsPerDescriptorPool = 1024;
// Combined image samplers per set: the textures of one draw.
constexpr uint32_t kSamplersPerSet = 16;

}  // namespace

FrameRing::~FrameRing() {
  WaitIdle();
  for (Slot& slot : slots_) {
    DestroySlot(slot);
  }
}

bool FrameRing::Initialize(VkDeviceSize upload_size) {
  upload_size_ = upload_size;
  GpuLabel("");  // id 0: unlabelled
  end_label_ = GpuLabel("after the last pass (copies, resolves, the output)");
  for (Slot& slot : slots_) {
    if (!CreateSlot(slot)) {
      return false;
    }
  }
  return true;
}

bool FrameRing::CreateSlot(Slot& slot) {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = context_.queue_family();
  if (fn.vkCreateCommandPool(device, &pool_info, nullptr, &slot.pool) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = slot.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  if (fn.vkAllocateCommandBuffers(device, &allocate, &slot.commands) != VK_SUCCESS ||
      fn.vkAllocateCommandBuffers(device, &allocate, &slot.upload_commands) != VK_SUCCESS) {
    return false;
  }
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (fn.vkCreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS) {
    return false;
  }
  if (context_.timestamp_bits()) {
    VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = kMaxGpuMarks;
    if (fn.vkCreateQueryPool(device, &query_info, nullptr, &slot.timestamps) != VK_SUCCESS) {
      slot.timestamps = VK_NULL_HANDLE;  // optional: only for measuring
    }
  }

  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = upload_size_;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (fn.vkCreateBuffer(device, &buffer_info, nullptr, &slot.upload) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  fn.vkGetBufferMemoryRequirements(device, slot.upload, &requirements);
  // Host-visible; coherent if possible, so nothing has to be flushed before submitting.
  const uint32_t type =
      context_.FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type == UINT32_MAX) {
    return false;
  }
  upload_coherent_ = (context_.device().memory_types().host_coherent >> type) & 1;
  VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  memory_info.allocationSize = requirements.size;
  memory_info.memoryTypeIndex = type;
  if (fn.vkAllocateMemory(device, &memory_info, nullptr, &slot.upload_memory) != VK_SUCCESS ||
      fn.vkBindBufferMemory(device, slot.upload, slot.upload_memory, 0) != VK_SUCCESS) {
    return false;
  }
  void* mapped = nullptr;
  if (fn.vkMapMemory(device, slot.upload_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    return false;
  }
  slot.upload_data = static_cast<uint8_t*>(mapped);
  return true;
}

void FrameRing::DestroySlot(Slot& slot) {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  for (auto& destroy : slot.deferred) {
    destroy();
  }
  slot.deferred.clear();
  for (VkDescriptorPool pool : slot.descriptor_pools) {
    fn.vkDestroyDescriptorPool(device, pool, nullptr);
  }
  slot.descriptor_pools.clear();
  if (slot.upload_memory) {
    fn.vkFreeMemory(device, slot.upload_memory, nullptr);
  }
  if (slot.upload) {
    fn.vkDestroyBuffer(device, slot.upload, nullptr);
  }
  if (slot.fence) {
    fn.vkDestroyFence(device, slot.fence, nullptr);
  }
  if (slot.timestamps) {
    fn.vkDestroyQueryPool(device, slot.timestamps, nullptr);
  }
  if (slot.pool) {
    fn.vkDestroyCommandPool(device, slot.pool, nullptr);
  }
  slot = Slot{};
}

void FrameRing::WaitSlot(Slot& slot) {
  if (!slot.pending) {
    return;
  }
  const auto& fn = context_.fn();
  const auto start = std::chrono::steady_clock::now();
  // Waited in 2 s steps so a GPU that never finishes the frame (a hung submission, a lost device) shows up in
  // the log instead of silently stopping the ring.
  constexpr uint64_t kStepNs = 2'000'000'000ull;
  for (uint32_t steps = 1;; ++steps) {
    const VkResult result = fn.vkWaitForFences(context_.handle(), 1, &slot.fence, VK_TRUE, kStepNs);
    if (result == VK_SUCCESS) {
      if (steps > 1) {
        REXLOG_WARN("[renderer] frame fence signalled after {} s", (steps - 1) * 2);
      }
      break;
    }
    if (result != VK_TIMEOUT) {
      REXLOG_ERROR("[renderer] waiting for the frame fence failed: VkResult {}", int(result));
      break;
    }
    if (steps == 1 || steps % 5 == 0) {
      REXLOG_ERROR("[renderer] the GPU has not finished a frame in {} s (hung submission?)", steps * 2);
    }
  }
  wait_ns_ += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  fn.vkResetFences(context_.handle(), 1, &slot.fence);
  slot.pending = false;
  if (slot.timed) {
    slot.timed = false;
    std::array<uint64_t, kMaxGpuMarks> ticks{};
    if (context_.extra().vkGetQueryPoolResults(context_.handle(), slot.timestamps, 0, slot.marks,
                                               sizeof(uint64_t) * slot.marks, ticks.data(), sizeof(uint64_t),
                                               VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const uint32_t bits = context_.timestamp_bits();
      const uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
      const double period = context_.timestamp_period_ns();
      gpu_ns_ += uint64_t(double((ticks[slot.marks - 1] - ticks[0]) & mask) * period);
      ++gpu_frames_;
      for (uint32_t i = 1; i < slot.marks; ++i) {
        GpuSegment& segment = gpu_segments_[slot.labels[i]];
        segment.ns += uint64_t(double((ticks[i] - ticks[i - 1]) & mask) * period);
        ++segment.count;
      }
    }
  }
}

uint16_t FrameRing::GpuLabel(const std::string& name) {
  if (auto it = gpu_label_ids_.find(name); it != gpu_label_ids_.end()) {
    return it->second;
  }
  if (gpu_segments_.size() >= 0xFFFF) {
    return 0;
  }
  const uint16_t id = uint16_t(gpu_segments_.size());
  gpu_segments_.push_back(GpuSegment{name});
  gpu_label_ids_.emplace(name, id);
  return id;
}

void FrameRing::MarkGpu(uint16_t label) {
  Slot& slot = slots_[current_];
  // One mark is kept for the end of the submission.
  if (!recording_ || !slot.timestamps || slot.marks + 1 >= kMaxGpuMarks) {
    return;
  }
  context_.extra().vkCmdWriteTimestamp(slot.commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps,
                                       slot.marks);
  slot.labels[slot.marks++] = label;
}

std::vector<FrameRing::GpuSegment> FrameRing::TakeGpuSegments() {
  std::vector<GpuSegment> taken;
  for (GpuSegment& segment : gpu_segments_) {
    if (segment.count) {
      taken.push_back(segment);
      segment.ns = segment.count = 0;
    }
  }
  std::sort(taken.begin(), taken.end(), [](const GpuSegment& a, const GpuSegment& b) { return a.ns > b.ns; });
  return taken;
}

bool FrameRing::BeginSlot() {
  Slot& slot = slots_[current_];
  WaitSlot(slot);
  for (auto& destroy : slot.deferred) {
    destroy();
  }
  slot.deferred.clear();
  slot.upload_used = 0;
  const auto& fn = context_.fn();
  for (VkDescriptorPool pool : slot.descriptor_pools) {
    fn.vkResetDescriptorPool(context_.handle(), pool, 0);
  }
  slot.descriptor_pool_current = 0;
  fn.vkResetCommandPool(context_.handle(), slot.pool, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (fn.vkBeginCommandBuffer(slot.commands, &begin) != VK_SUCCESS) {
    return false;
  }
  if (slot.timestamps) {
    fn.vkCmdResetQueryPool(slot.commands, slot.timestamps, 0, kMaxGpuMarks);
    context_.extra().vkCmdWriteTimestamp(slot.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
    slot.marks = 1;
  }
  recording_ = true;
  ++frame_number_;
  return true;
}

VkCommandBuffer FrameRing::Commands() {
  if (!recording_ && !BeginSlot()) {
    return VK_NULL_HANDLE;
  }
  return slots_[current_].commands;
}

VkCommandBuffer FrameRing::UploadCommands() {
  if (!recording_ && !BeginSlot()) {
    return VK_NULL_HANDLE;
  }
  Slot& slot = slots_[current_];
  if (!slot.upload_recording) {
    const auto& fn = context_.fn();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (fn.vkBeginCommandBuffer(slot.upload_commands, &begin) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    // Images may still be read by earlier frames' draws, and resolved textures (copied from here into dynamic
    // cubemaps) were written by earlier frames' copies and passes.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    fn.vkCmdPipelineBarrier(slot.upload_commands,
                            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    slot.upload_recording = true;
  }
  return slot.upload_commands;
}

bool FrameRing::Submit() {
  if (!recording_) {
    return true;
  }
  Slot& slot = slots_[current_];
  const auto& fn = context_.fn();
  recording_ = false;
  current_ = (current_ + 1) % kFramesInFlight;
  VkCommandBuffer buffers[2];
  uint32_t buffer_count = 0;
  if (slot.upload_recording) {
    // What was uploaded is visible to everything recorded in the main command buffer.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    fn.vkCmdPipelineBarrier(slot.upload_commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            0, 1, &barrier, 0, nullptr, 0, nullptr);
    slot.upload_recording = false;
    if (fn.vkEndCommandBuffer(slot.upload_commands) != VK_SUCCESS) {
      return false;
    }
    buffers[buffer_count++] = slot.upload_commands;
  }
  if (slot.timestamps) {
    context_.extra().vkCmdWriteTimestamp(slot.commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps,
                                         slot.marks);
    slot.labels[slot.marks++] = end_label_;
    slot.timed = true;
  }
  if (fn.vkEndCommandBuffer(slot.commands) != VK_SUCCESS) {
    return false;
  }
  buffers[buffer_count++] = slot.commands;
  if (!upload_coherent_ && slot.upload_used) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = slot.upload_memory;
    range.size = VK_WHOLE_SIZE;
    fn.vkFlushMappedMemoryRanges(context_.handle(), 1, &range);
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = buffer_count;
  submit.pCommandBuffers = buffers;
  if (context_.Submit(submit, slot.fence) != VK_SUCCESS) {
    REXLOG_ERROR("[renderer] vkQueueSubmit failed");
    return false;
  }
  slot.pending = true;
  return true;
}

void FrameRing::WaitIdle() {
  for (Slot& slot : slots_) {
    WaitSlot(slot);
  }
}

bool FrameRing::Allocate(VkDeviceSize size, VkDeviceSize alignment, Allocation& out) {
  if (!recording_ && !BeginSlot()) {
    return false;
  }
  Slot& slot = slots_[current_];
  const VkDeviceSize offset = (slot.upload_used + alignment - 1) / alignment * alignment;
  if (offset + size > upload_size_) {
    return false;
  }
  slot.upload_used = offset + size;
  out.data = slot.upload_data + offset;
  out.buffer = slot.upload;
  out.offset = offset;
  return true;
}

VkDescriptorPool FrameRing::NewDescriptorPool() {
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetsPerDescriptorPool * kSamplersPerSet},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerDescriptorPool * 3},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerDescriptorPool},
  };
  VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.maxSets = kSetsPerDescriptorPool;
  info.poolSizeCount = uint32_t(std::size(sizes));
  info.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  context_.fn().vkCreateDescriptorPool(context_.handle(), &info, nullptr, &pool);
  return pool;
}

VkDescriptorSet FrameRing::AllocateDescriptorSet(VkDescriptorSetLayout layout) {
  Slot& slot = slots_[current_];
  VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  info.descriptorSetCount = 1;
  info.pSetLayouts = &layout;
  while (true) {
    if (slot.descriptor_pool_current == slot.descriptor_pools.size()) {
      VkDescriptorPool pool = NewDescriptorPool();
      if (!pool) {
        return VK_NULL_HANDLE;
      }
      slot.descriptor_pools.push_back(pool);
    }
    info.descriptorPool = slot.descriptor_pools[slot.descriptor_pool_current];
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkResult result = context_.fn().vkAllocateDescriptorSets(context_.handle(), &info, &set);
    if (result == VK_SUCCESS) {
      return set;
    }
    if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
      return VK_NULL_HANDLE;
    }
    ++slot.descriptor_pool_current;  // this pool is full: the next one, or a new one
  }
}

void FrameRing::InvalidateUploadMemory() {
  if (upload_coherent_) {
    return;
  }
  for (Slot& slot : slots_) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = slot.upload_memory;
    range.size = VK_WHOLE_SIZE;
    context_.fn().vkInvalidateMappedMemoryRanges(context_.handle(), 1, &range);
  }
}

void FrameRing::Defer(std::function<void()> destroy) {
  // The newest work is in the open slot or, between submissions, in the one submitted last. Its list runs only
  // after its fence, and the queue finishes submissions in order, so all older work is done by then too.
  const uint32_t newest = recording_ ? current_ : (current_ + kFramesInFlight - 1) % kFramesInFlight;
  slots_[newest].deferred.push_back(std::move(destroy));
}

}  // namespace nfsmw::renderer
