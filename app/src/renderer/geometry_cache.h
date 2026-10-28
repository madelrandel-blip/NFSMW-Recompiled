// Vertex and index data kept on the GPU across frames.
//
// The game draws mostly the same geometry every frame: 99.0-99.6 % of the vertex bytes the legacy renderer copied
// were byte-for-byte equal to an earlier frame. Converting them (byte swap, 16 to 32-bit indices) every frame was
// ~37 % of the CPU time of the draws, on the phone and on the PC. Here a range is converted once into memory that
// stays (host-visible: on a phone's unified memory the GPU reads it where it is), and the guest pages it came from
// are write-protected through the SDK's physical memory watches. A write by the game (or a file read into them)
// marks those pages; a copy made before that is not used again and the range is converted anew.
//
// Ranges rewritten over and over (particles, skinned or animated data) stop being cached after a few rewrites and
// go back to the per-frame upload memory, so their pages are not watched and do not fault every frame.

#pragma once

#include "frame_ring.h"
#include "vulkan_context.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <vector>

namespace rex::memory {
class Memory;
}

namespace nfsmw::renderer {

class GeometryCache {
 public:
  struct Slice {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* data = nullptr;
  };
  using Extra = std::array<uint32_t, 4>;  // what the caller needs back with the copy (index range, counts)

  struct Stats {
    uint64_t hits = 0;           // copies reused
    uint64_t misses = 0;         // copies made
    uint64_t misses_bytes = 0;
    uint64_t stale = 0;          // copies found written since
    uint64_t dynamic = 0;        // draws whose range is rewritten too often to cache
    uint64_t full = 0;           // no room until the other half is free
    uint64_t flips = 0;          // halves switched (the cache emptied)
    uint64_t notifications = 0;  // write notifications from the watched pages
  };

  GeometryCache(const VulkanContext& context, FrameRing& frames, rex::memory::Memory* memory)
      : context_(context), frames_(frames), memory_(memory) {}
  ~GeometryCache();
  GeometryCache(const GeometryCache&) = delete;
  GeometryCache& operator=(const GeometryCache&) = delete;

  // A copy made earlier for `key` from the guest bytes [address, address + bytes) that the game has not written
  // since. `extra` gets what was committed with it.
  bool Find(uint64_t key, uint32_t address, uint32_t bytes, Slice& slice, Extra* extra = nullptr);
  // Room for a new copy of those bytes for `key`, `size` bytes long; their pages are watched from now on. False when
  // the range is dynamic, the cache is full or off: the caller uses the per-frame upload memory then.
  bool Begin(uint64_t key, uint32_t address, uint32_t bytes, VkDeviceSize size, VkDeviceSize alignment,
             Slice& slice);
  // The copy begun last (for `key`) is written: from now on Find returns it.
  void Commit(uint64_t key, const Extra& extra = {});

  Stats TakeStats();

 private:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kGuestBytes = 0x20000000;
  static constexpr VkDeviceSize kHalfBytes = 64ull << 20;
  // A range is dynamic after this many rewrites in a row, each found within kQuickRewriteFrames of its copy (one
  // rewritten now and then, a zone being loaded, is not); the mark expires after kDynamicFrames, and it is cached again.
  static constexpr uint32_t kRewritesToDynamic = 3;
  static constexpr uint64_t kQuickRewriteFrames = 2;
  static constexpr uint64_t kDynamicFrames = 300;

  struct Entry {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t address = 0, bytes = 0;
    uint32_t stamp = 0;  // write notifications counted when it was made
    Extra extra{};
    uint64_t frame = 0;  // made in
  };
  struct Half {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* data = nullptr;
    VkDeviceSize used = 0;
    uint64_t last_frame = 0;  // the last frame its copies could be used in
  };

  bool Initialize();
  bool Allocate(VkDeviceSize size, VkDeviceSize alignment, Slice& slice);
  bool Unchanged(const Entry& entry) const;
  static std::pair<uint32_t, uint32_t> OnWrite(void* context, uint32_t physical_address, uint32_t length,
                                               bool exact_range);

  const VulkanContext& context_;
  FrameRing& frames_;
  rex::memory::Memory* memory_;
  bool initialized_ = false;
  bool usable_ = false;
  void* callback_handle_ = nullptr;
  std::array<Half, 2> halves_;
  uint32_t current_ = 0;

  // Written from the threads that write guest memory (the SDK's callback), read from the ring thread.
  std::atomic<uint32_t> notifications_{0};
  std::unique_ptr<std::atomic<uint32_t>[]> page_stamps_;  // per 4 KB page: notifications when last written

  std::unordered_map<uint64_t, Entry> entries_;  // ring thread only
  std::unordered_map<uint64_t, uint32_t> rewrites_;     // quick rewrites in a row, by key
  std::unordered_map<uint64_t, uint64_t> dynamic_;      // dynamic keys and the frame they were marked in
  uint64_t pending_key_ = 0;
  Entry pending_;
  bool has_pending_ = false;
  Stats stats_;
  uint32_t reported_notifications_ = 0;
};

}  // namespace nfsmw::renderer
