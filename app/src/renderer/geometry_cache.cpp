#include "geometry_cache.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include <algorithm>

// Off: measured on 27/10 it saved under 1 ms on the PC and cost ~20 ms on the Mali-G68 phone (watching pages takes
// the SDK's global lock, contended by the game threads there, and every watched write is a SIGSEGV). Worth trying
// again only with the pages protected from another thread and the copies validated by hash.
REXCVAR_DEFINE_BOOL(nfsmw_geometria_persistente, false, "Graficos",
                    "Renderizador Vulkan 1.1: los vertices e indices se convierten una vez y se quedan en la GPU "
                    "mientras el juego no reescribe su memoria (vigilada por paginas). Si alguna vez se viera "
                    "geometria congelada o estirada, false vuelve a convertirlos en cada fotograma")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nfsmw::renderer {

GeometryCache::~GeometryCache() {
  if (callback_handle_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(callback_handle_);
  }
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  for (Half& half : halves_) {
    if (half.buffer) fn.vkDestroyBuffer(device, half.buffer, nullptr);
    if (half.memory) fn.vkFreeMemory(device, half.memory, nullptr);
  }
}

bool GeometryCache::Initialize() {
  initialized_ = true;
  if (!REXCVAR_GET(nfsmw_geometria_persistente) || !memory_) {
    REXLOG_INFO("[renderer] persistent geometry: off");
    return false;
  }
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  for (Half& half : halves_) {
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = kHalfBytes;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (fn.vkCreateBuffer(device, &buffer_info, nullptr, &half.buffer) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements;
    fn.vkGetBufferMemoryRequirements(device, half.buffer, &requirements);
    // Written by the CPU once per copy and read by the GPU every frame: device-local where the host can also map
    // it (unified memory, or a resizable BAR), coherent so nothing has to be flushed.
    const uint32_t type = context_.FindMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    void* mapped = nullptr;
    if (type == UINT32_MAX || fn.vkAllocateMemory(device, &allocate, nullptr, &half.memory) != VK_SUCCESS ||
        fn.vkBindBufferMemory(device, half.buffer, half.memory, 0) != VK_SUCCESS ||
        fn.vkMapMemory(device, half.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      REXLOG_WARN("[renderer] persistent geometry: no memory for it, off");
      return false;
    }
    half.data = static_cast<uint8_t*>(mapped);
  }
  page_stamps_ = std::make_unique<std::atomic<uint32_t>[]>(kGuestBytes >> kPageShift);
  for (uint32_t i = 0; i < (kGuestBytes >> kPageShift); ++i) {
    page_stamps_[i].store(0, std::memory_order_relaxed);
  }
  callback_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(&OnWrite, this);
  if (!callback_handle_) {
    return false;
  }
  REXLOG_INFO("[renderer] persistent geometry: on, 2 x {} MB", kHalfBytes >> 20);
  usable_ = true;
  return true;
}

// From whichever thread wrote a watched page (or read a file into it), with the SDK's global lock held.
std::pair<uint32_t, uint32_t> GeometryCache::OnWrite(void* context, uint32_t physical_address, uint32_t length,
                                                     bool exact_range) {
  (void)exact_range;
  auto* cache = static_cast<GeometryCache*>(context);
  if (length) {
    const uint32_t stamp = cache->notifications_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t first = (physical_address & (kGuestBytes - 1)) >> kPageShift;
    const uint64_t end = std::min<uint64_t>(uint64_t(physical_address & (kGuestBytes - 1)) + length, kGuestBytes);
    const uint32_t last = uint32_t((end - 1) >> kPageShift);
    for (uint32_t page = first; page <= last; ++page) {
      cache->page_stamps_[page].store(stamp, std::memory_order_release);
    }
  }
  // Only the pages it was told about may be unwatched: wider, a later write to the others would go unnoticed.
  return {physical_address, length};
}

bool GeometryCache::Unchanged(const Entry& entry) const {
  const uint32_t first = entry.address >> kPageShift;
  const uint32_t last = (entry.address + entry.bytes - 1) >> kPageShift;
  for (uint32_t page = first; page <= last; ++page) {
    if (page_stamps_[page].load(std::memory_order_acquire) > entry.stamp) {
      return false;
    }
  }
  return true;
}

bool GeometryCache::Find(uint64_t key, uint32_t address, uint32_t bytes, Slice& slice, Extra* extra) {
  if (!usable_ || !bytes) {
    return false;
  }
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return false;  // never made, or dynamic (counted by Begin)
  }
  const Entry& entry = it->second;
  if (entry.address != address || entry.bytes != bytes || !Unchanged(entry)) {
    // Written since (or another range under the same key): converted anew, and a range rewritten again and again
    // stops being cached.
    ++stats_.stale;
    const uint64_t frame = frames_.frame_number();
    uint32_t& rewrites = rewrites_[key];
    rewrites = frame - entry.frame <= kQuickRewriteFrames ? rewrites + 1 : 0;
    if (rewrites >= kRewritesToDynamic) {
      dynamic_[key] = frame;
      rewrites_.erase(key);
    }
    entries_.erase(it);
    return false;
  }
  slice.buffer = entry.buffer;
  slice.offset = entry.offset;
  slice.data = nullptr;
  if (extra) {
    *extra = entry.extra;
  }
  ++stats_.hits;
  halves_[current_].last_frame = frames_.frame_number();
  return true;
}

bool GeometryCache::Allocate(VkDeviceSize size, VkDeviceSize alignment, Slice& slice) {
  Half* half = &halves_[current_];
  VkDeviceSize offset = (half->used + alignment - 1) & ~(alignment - 1);
  if (offset + size > kHalfBytes) {
    // Full: the other half takes over once the GPU can no longer be reading it, and the cache starts empty there
    // (every copy in this half is dropped, so this one's last use is this frame).
    Half& other = halves_[current_ ^ 1];
    const uint64_t frame = frames_.frame_number();
    if (size > kHalfBytes || frame - other.last_frame <= FrameRing::kFramesInFlight) {
      ++stats_.full;
      return false;
    }
    half->last_frame = frame;
    entries_.clear();
    rewrites_.clear();  // what is rewritten is judged again from here
    has_pending_ = false;
    current_ ^= 1;
    half = &halves_[current_];
    half->used = 0;
    offset = 0;
    ++stats_.flips;
  }
  half->used = offset + size;
  half->last_frame = frames_.frame_number();
  slice.buffer = half->buffer;
  slice.offset = offset;
  slice.data = half->data + offset;
  return true;
}

bool GeometryCache::Begin(uint64_t key, uint32_t address, uint32_t bytes, VkDeviceSize size,
                          VkDeviceSize alignment, Slice& slice) {
  if (!initialized_) {
    Initialize();
  }
  if (!usable_ || !bytes || uint64_t(address) + bytes > kGuestBytes) {
    return false;
  }
  if (auto it = dynamic_.find(key); it != dynamic_.end()) {
    if (frames_.frame_number() - it->second < kDynamicFrames) {
      ++stats_.dynamic;
      return false;
    }
    dynamic_.erase(it);  // tried again: it may have settled
  }
  if (!Allocate(size, alignment, slice)) {
    return false;
  }
  // The notification count is taken before the pages are watched: a write landing while the copy is made raises
  // their stamp above it, and the copy is redone next time instead of being trusted.
  pending_ = Entry{slice.buffer, slice.offset, address, bytes, notifications_.load(std::memory_order_acquire), {},
                   frames_.frame_number()};
  pending_key_ = key;
  has_pending_ = true;
  memory_->EnablePhysicalMemoryAccessCallbacks(address, bytes, true, false);
  ++stats_.misses;
  stats_.misses_bytes += bytes;
  return true;
}

void GeometryCache::Commit(uint64_t key, const Extra& extra) {
  if (!has_pending_ || key != pending_key_) {
    return;
  }
  has_pending_ = false;
  pending_.extra = extra;
  entries_[key] = pending_;
}

GeometryCache::Stats GeometryCache::TakeStats() {
  Stats taken = stats_;
  const uint32_t notifications = notifications_.load(std::memory_order_relaxed);
  taken.notifications = notifications - reported_notifications_;
  reported_notifications_ = notifications;
  stats_ = Stats{};
  return taken;
}

}  // namespace nfsmw::renderer
