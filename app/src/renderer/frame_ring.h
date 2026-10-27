// Frames in flight: per-frame command buffer, fence, upload memory, descriptor sets and deferred destruction.
//
// The renderer records into the current slot and submits it at the swap (or earlier, when the upload memory runs
// out). Starting the next slot waits for the GPU to finish the last submission made from it, so everything that
// slot owned (upload ranges, descriptor sets, objects queued for destruction) is free to reuse.

#pragma once

#include "vulkan_context.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nfsmw::renderer {

class FrameRing {
 public:
  static constexpr uint32_t kFramesInFlight = 3;
  static constexpr uint32_t kMaxGpuMarks = 128;  // GPU timestamps per submission (measuring only)

  struct Allocation {
    uint8_t* data = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
  };

  explicit FrameRing(const VulkanContext& context) : context_(context) {}
  ~FrameRing();
  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;

  // `upload_size` bytes of host-visible memory per slot.
  bool Initialize(VkDeviceSize upload_size);

  // The command buffer being recorded, beginning a slot if none is open. VK_NULL_HANDLE on failure.
  VkCommandBuffer Commands();

  // A second command buffer of the open slot, submitted right before Commands(): texture uploads go here so they
  // never interrupt a render pass. It starts with a barrier against the previous frames' reads.
  VkCommandBuffer UploadCommands();
  bool recording() const { return recording_; }

  // Ends and submits the open slot, if any. The next Commands() starts the following slot.
  bool Submit();

  // Waits until the GPU has finished everything submitted.
  void WaitIdle();

  // `size` bytes of upload memory in the open slot. false if the slot is full: submit and retry in the next one.
  // The buffer can also be a copy destination: what the GPU writes there can be read in a Defer() callback, which
  // runs before the memory is reused.
  bool Allocate(VkDeviceSize size, VkDeviceSize alignment, Allocation& out);

  // Makes GPU writes to upload memory visible to the CPU (no-op with coherent memory). For Defer() callbacks.
  void InvalidateUploadMemory();

  // A descriptor set that lives until this slot is reused.
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);

  // Runs `destroy` once the GPU is done with everything recorded so far.
  void Defer(std::function<void()> destroy);

  // Increases with every slot started.
  uint64_t frame_number() const { return frame_number_; }

  // Nanoseconds spent waiting for the GPU to free a slot since the last call.
  uint64_t TakeWaitNs() { return std::exchange(wait_ns_, 0); }
  // GPU time of the frames finished since the last call (from the start to the end of their main command buffer,
  // by GPU timestamps; the presenter's own work is not in it), and how many frames that is.
  uint64_t TakeGpuNs() { return std::exchange(gpu_ns_, 0); }
  uint64_t TakeGpuFrames() { return std::exchange(gpu_frames_, 0); }

  // A GPU timestamp now in the frame's commands: the GPU time since the previous mark is counted under `label`
  // (from GpuLabel). Ignored without timestamps or past kMaxGpuMarks in a frame.
  void MarkGpu(uint16_t label);
  uint16_t GpuLabel(const std::string& name);
  // GPU time per label of the frames finished since the last call, most first: name, nanoseconds, marks.
  struct GpuSegment {
    std::string name;
    uint64_t ns = 0;
    uint64_t count = 0;
  };
  std::vector<GpuSegment> TakeGpuSegments();

 private:
  struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandBuffer upload_commands = VK_NULL_HANDLE;
    bool upload_recording = false;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;  // submitted and maybe not finished
    VkBuffer upload = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    uint8_t* upload_data = nullptr;
    VkDeviceSize upload_used = 0;
    std::vector<VkDescriptorPool> descriptor_pools;
    size_t descriptor_pool_current = 0;
    std::vector<std::function<void()>> deferred;
    // Measuring only: GPU timestamps through the main command buffer, from its start (mark 0) to its end, with
    // MarkGpu's in between; each mark's label names the work since the previous one.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    uint32_t marks = 0;
    std::array<uint16_t, kMaxGpuMarks> labels{};
    bool timed = false;  // the end was written in the last submission
  };

  bool CreateSlot(Slot& slot);
  void DestroySlot(Slot& slot);
  void WaitSlot(Slot& slot);
  bool BeginSlot();
  VkDescriptorPool NewDescriptorPool();

  const VulkanContext& context_;
  std::array<Slot, kFramesInFlight> slots_;
  uint32_t current_ = 0;
  bool recording_ = false;
  bool upload_coherent_ = true;
  VkDeviceSize upload_size_ = 0;
  uint64_t frame_number_ = 0;
  uint64_t wait_ns_ = 0;
  uint64_t gpu_ns_ = 0;
  uint64_t gpu_frames_ = 0;
  std::vector<GpuSegment> gpu_segments_;  // by label
  std::unordered_map<std::string, uint16_t> gpu_label_ids_;
  uint16_t end_label_ = 0;
};

}  // namespace nfsmw::renderer
