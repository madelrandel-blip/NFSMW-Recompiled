// What the renderer records per frame, for analysis: counters of every command that is issued again and again, and
// an optional trace of one whole frame (render passes, their targets and draws, copies and clears).
//
// Single-threaded: only the ring thread records.

#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nfsmw::renderer {

struct FrameCounters {
  // Per draw.
  uint64_t draws = 0;
  uint64_t pipeline_binds = 0;
  uint64_t descriptor_binds = 0;
  uint64_t texture_set_writes = 0;  // descriptor sets allocated and written for textures
  uint64_t texture_reuses = 0;      // draws that took the previous draw's textures without looking them up
  uint64_t uniform_set_writes = 0;
  uint64_t vertex_buffer_binds = 0;
  uint64_t index_buffer_binds = 0;
  uint64_t viewport_sets = 0;
  uint64_t scissor_sets = 0;
  uint64_t depth_bias_sets = 0;
  uint64_t blend_constant_sets = 0;
  uint64_t stencil_sets = 0;
  uint64_t vs_constant_uploads = 0, vs_constant_bytes = 0;
  uint64_t ps_constant_uploads = 0, ps_constant_bytes = 0;
  // Per pass and copy.
  uint64_t passes = 0;
  uint64_t passes_reopened = 0;              // a draw beyond the render area reopened the pass larger
  uint64_t swap_resolves = 0;                // resolves done by swapping images instead of copying
  uint64_t swap_restores = 0;                // ... whose target was later read without a clear (copied back)
  uint64_t shadow_vegetation_skipped = 0;    // nfsmw_sombras_sin_vegetacion
  uint64_t render_area_pixels = 0;           // of the passes begun
  uint64_t framebuffer_pixels = 0;           // ... and of their framebuffers (what the area would be untrimmed)
  uint64_t attachments_loaded = 0;          // loadOp LOAD: the previous contents are needed
  uint64_t attachments_cleared_on_load = 0;  // loadOp CLEAR: a deferred clear done by the pass itself
  uint64_t attachments_discarded = 0;        // storeOp DONT_CARE: depth nobody reads
  uint64_t color_clears = 0, color_clear_pixels = 0;  // clears still done as separate commands
  uint64_t depth_clears = 0, depth_clear_pixels = 0;
  uint64_t resolves = 0, resolve_pixels = 0;
  uint64_t cleared_then_copied = 0;  // a cleared target resolved before any draw: the clear was needed as is
};

class FrameStats {
 public:
  FrameCounters& counters() { return counters_; }
  FrameCounters Take() { return std::exchange(counters_, FrameCounters{}); }

  // The trace of one frame: started at a swap, written out at the next one.
  bool tracing() const { return tracing_; }
  void StartTrace() {
    tracing_ = true;
    trace_.clear();
  }
  std::vector<std::string> FinishTrace() {
    tracing_ = false;
    return std::exchange(trace_, {});
  }
  template <typename... Args>
  void Trace(fmt::format_string<Args...> format, Args&&... args) {
    if (tracing_) {
      trace_.push_back(fmt::format(format, std::forward<Args>(args)...));
    }
  }

 private:
  FrameCounters counters_;
  bool tracing_ = false;
  std::vector<std::string> trace_;
};

// The renderer's single instance.
FrameStats& Stats();

}  // namespace nfsmw::renderer
