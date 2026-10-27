#include "draws.h"

#include "endian.h"
#include "frame_stats.h"
#include "shader_modules.h"

#include "../nfsmw_nativo_dibujos.h"
#include "../nfsmw_nativo_shaders.h"
#include "spirv_recompiler.h"

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(nfsmw_recortar_pasadas, true, "Graficos",
                    "Renderizador Vulkan 1.1: el area de cada pasada cubre solo lo que el juego dibuja en sus "
                    "destinos (en una GPU de movil ahorra la carga y el guardado de lo que nadie dibuja); false = "
                    "la imagen entera");
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);
REXCVAR_DEFINE_BOOL(nfsmw_perfil_dibujos, false, "NFSMW",
                    "Renderizador Vulkan 1.1 (analisis): mide el tiempo de CPU de cada dibujo por etapas (destinos, "
                    "indices, texturas, pasada, pipeline, constantes, vertices, comandos) y lo escribe en el log cada "
                    "5 s; cuesta unas 9 lecturas del reloj por dibujo");
REXCVAR_DEFINE_BOOL(nfsmw_gpu_por_pasada, false, "NFSMW",
                    "Renderizador Vulkan 1.1 (analisis): marcas de tiempo de GPU entre las pasadas, para el reparto "
                    "\"GPU per frame by piece\" del log. En una GPU de movil pueden impedir que una pasada se solape "
                    "con la siguiente, asi que cuestan algo; el tiempo total del fotograma se mide siempre");

namespace nfsmw::renderer {

namespace gr = rex::graphics;

namespace {

constexpr uint32_t kVertexConstantBytes = 256 * 16;
constexpr uint32_t kPixelConstantBytes = 224 * 16;
constexpr uint32_t kMaxVerticesPerDraw = 1u << 20;
constexpr uint64_t kGuestMemoryBytes = 0x20000000;
constexpr uint32_t kShadowPitch = 1600;  // the game's two shadow maps (RenderTargets' kShadowSide)

// Specialization constant bits (shader_common.h).
constexpr uint32_t kSpecAlphaTest = 1u << 1;
constexpr uint32_t kSpecInvTextureSize = 1u << 9;
constexpr uint32_t kSpecAlphaFuncShift = 16;

// xenos::PrimitiveType -> topology; quads become triangle lists.
bool Topology(uint32_t type, VkPrimitiveTopology& topology, bool& quads, bool& allows_restart) {
  quads = false;
  allows_restart = false;
  switch (type) {
    case 2:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      return true;
    case 3:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      allows_restart = true;
      return true;
    case 4:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      return true;
    case 5:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      allows_restart = true;
      return true;
    case 6:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      allows_restart = true;
      return true;
    case 13:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      return true;
    default:
      return false;
  }
}

int32_t SignExtend15(uint32_t value) { return int32_t(value << 17) >> 17; }

struct IndexRange {
  uint32_t lowest = UINT32_MAX;
  uint32_t highest = 0;
};

// One converted index: offset applied and wrapped to 24 bits, or the restart value. Everything is local so the
// loops below stay in registers (and vectorize without restarts).
template <bool kRestart>
inline uint32_t FinishIndex(uint32_t value, uint32_t offset, uint32_t reset_index, IndexRange& range) {
  if (kRestart && value == reset_index) {
    return UINT32_MAX;  // Vulkan's restart value for 32-bit indices
  }
  value = (value + offset) & 0xFFFFFF;
  range.lowest = std::min(range.lowest, value);
  range.highest = std::max(range.highest, value);
  return value;
}

// 32-bit indices, already byte swapped in place.
template <bool kRestart>
IndexRange FinishIndices(uint32_t* indices, uint32_t count, uint32_t offset, uint32_t reset_index) {
  IndexRange range;
  for (uint32_t i = 0; i < count; ++i) {
    indices[i] = FinishIndex<kRestart>(indices[i] & 0xFFFFFF, offset, reset_index, range);
  }
  return range;
}

// 16-bit guest indices to 32-bit host indices.
template <bool kRestart>
IndexRange DecodeIndices16(const uint8_t* source, uint32_t* dest, uint32_t count, xenos::Endian endian,
                           uint32_t offset, uint32_t reset_index) {
  IndexRange range;
  if (endian == xenos::Endian::k8in16) {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t raw;
      std::memcpy(&raw, source + size_t(i) * 2, 2);
      dest[i] = FinishIndex<kRestart>(uint32_t(__builtin_bswap16(raw)), offset, reset_index, range);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t raw;
      std::memcpy(&raw, source + size_t(i) * 2, 2);
      dest[i] = FinishIndex<kRestart>(uint32_t(xenos::GpuSwap(raw, endian)), offset, reset_index, range);
    }
  }
  return range;
}

}  // namespace

bool Draws::Reject(uint32_t cause, const char* text) {
  if (cause < rejected_.size() && !rejected_[cause]) {
    rejected_[cause] = true;
    REXLOG_WARN("[renderer] draw dropped: {}", text);
  }
  return false;
}

void Draws::EndPass() {
  if (in_pass_) {
    Stats().Trace("    {} draws", pass_draws_);
    for (const Image* target : pass_.targets) {
      if (target) {
        target->drawn_width = std::max(target->drawn_width, pass_.drawn_width);
        target->drawn_height = std::max(target->drawn_height, pass_.drawn_height);
      }
    }
    context_.fn().vkCmdEndRenderPass(frames_.Commands());
    if (pass_label_) {
      frames_.MarkGpu(pass_label_);
    }
    RenderTargets::AfterPassBarrier(context_, frames_.Commands());
    in_pass_ = false;
  }
}

void Draws::FrameSubmitted() {
  in_pass_ = false;
  vertex_copies_.clear();
  index_copies_.clear();
  vs_constants_generation_ = ps_constants_generation_ = UINT64_MAX;
  constants_buffer_ = VK_NULL_HANDLE;
  uniform_set_ = VK_NULL_HANDLE;
  uniform_set_buffer_ = VK_NULL_HANDLE;
  bound_ = BoundState{};  // a new command buffer starts with nothing bound
}

bool Draws::JoinsOpenPass(const std::array<const Image*, kMaxColorTargets + 1>& targets) const {
  if (!in_pass_) {
    return false;
  }
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i <= kMaxColorTargets; ++i) {
    if (!targets[i]) {
      continue;
    }
    if (targets[i] != pass_.targets[i]) {
      return false;
    }
    width = std::min(width, targets[i]->width);
    height = std::min(height, targets[i]->height);
  }
  // The pass's area is the smallest of its attachments: it must not cut the draw's own.
  return width == pass_.width && height == pass_.height;
}

bool Draws::BeginPass(VkCommandBuffer commands, const std::array<const Image*, kMaxColorTargets + 1>& targets) {
  if (in_pass_ && pass_.targets == targets) {
    return true;
  }
  // Why a new pass starts (pass statistics).
  if (!in_pass_) {
    ++pass_stats_.after_break;
  } else {
    bool same_colors = true, colors_subset = true;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
      same_colors &= targets[i] == pass_.targets[i];
      colors_subset &= !targets[i] || targets[i] == pass_.targets[i];
    }
    const bool depth_toggled = !targets[kMaxColorTargets] != !pass_.targets[kMaxColorTargets];
    const bool depth_compatible = !targets[kMaxColorTargets] || targets[kMaxColorTargets] == pass_.targets[kMaxColorTargets];
    if (same_colors && depth_toggled) {
      ++pass_stats_.depth_toggled;
    } else if (colors_subset && depth_compatible) {
      ++pass_stats_.color_subset;
    } else {
      ++pass_stats_.different_targets;
    }
  }
  EndPass();
  PassFormats formats{};
  std::array<VkImageView, kMaxColorTargets + 1> views{};  // the attachments in order (an imageless pass begin)
  uint32_t view_count = 0;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i <= kMaxColorTargets; ++i) {
    formats[i] = targets[i] ? targets[i]->format : VK_FORMAT_UNDEFINED;
    if (targets[i]) {
      views[view_count++] = targets[i]->view;
      width = std::min(width, targets[i]->width);
      height = std::min(height, targets[i]->height);
    }
  }
  if (width == UINT32_MAX) {
    return false;
  }
  // The render area: the largest extent any of the targets was drawn into, rounded up to 32 (whole tiles), or the
  // whole framebuffer for targets nothing has drawn into yet. Loads, stores and clears only touch it.
  uint32_t area_width = 0, area_height = 0;
  bool known = true;
  for (const Image* target : targets) {
    if (target) {
      known &= target->drawn_width != 0;
      area_width = std::max(area_width, target->drawn_width);
      area_height = std::max(area_height, target->drawn_height);
    }
  }
  if (!known || !REXCVAR_GET(nfsmw_recortar_pasadas)) {
    area_width = width;
    area_height = height;
  }
  area_width = std::min(width, (area_width + 31) & ~31u);
  area_height = std::min(height, (area_height + 31) & ~31u);
  Stats().counters().render_area_pixels += uint64_t(area_width) * area_height;
  Stats().counters().framebuffer_pixels += uint64_t(width) * height;
  // Every attachment of a pass has the same sample count (the scene's are all multisampled).
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  for (const Image* target : targets) {
    if (target) {
      samples = target->samples;
    }
  }
  // Pipelines and the framebuffer use the plain pass (LOAD and STORE everywhere).
  VkRenderPass pass = pipelines_.RenderPass(formats, samples);
  VkFramebuffer framebuffer = pass ? pipelines_.Framebuffer(pass, targets, width, height) : VK_NULL_HANDLE;
  if (!framebuffer) {
    return false;
  }

  // Load and store operations of this pass, from the state of each target:
  //   - a deferred clear becomes loadOp CLEAR (only where the attachment is the pass's size: CLEAR covers the render
  //     area, the clear the whole image); otherwise it is done now. With a trimmed area, what is left outside it
  //     keeps its old contents: nothing was ever drawn there, and copies read what the game drew;
  //   - anything else is LOAD, and loading counts as reading the contents;
  //   - a depth target nobody reads is not stored (DepthDiscardable).
  FrameCounters& counters = Stats().counters();
  ++counters.passes;
  uint32_t clear_mask = 0, discard_mask = 0;
  std::array<VkClearValue, kMaxColorTargets + 1> clear_values{};
  uint32_t attachment_count = 0;
  std::string described;
  for (uint32_t i = 0; i <= kMaxColorTargets; ++i) {
    const Image* target = targets[i];
    if (!target) {
      continue;
    }
    if (target->first_frame == UINT64_MAX) {
      target->first_frame = frames_.frame_number();
    }
    if (target->clear_pending && (target->width != width || target->height != height)) {
      render_targets_.FlushClear(commands, *target);
    }
    if (!target->clear_pending) {
      render_targets_.RestoreSwapped(commands, *target);  // loaded: it must hold what was drawn, not a swap's leftovers
    }
    const bool clear = target->clear_pending;
    if (clear) {
      clear_mask |= 1u << i;
      clear_values[attachment_count] = target->clear_value;
      target->clear_pending = false;
      ++counters.attachments_cleared_on_load;
    } else {
      // Loading what an earlier pass stored is a read; loading a target nobody has drawn into is not.
      target->content_read |= target->has_content;
      ++counters.attachments_loaded;
    }
    const bool discard = render_targets_.DepthDiscardable(*target);
    if (discard) {
      discard_mask |= 1u << i;
      ++counters.attachments_discarded;
    }
    target->has_content = !discard;
    ++attachment_count;
    if (Stats().tracing()) {
      described += fmt::format(" {}{}={:03X}/{} ({} {})", i == kMaxColorTargets ? "depth" : "color",
                               i == kMaxColorTargets ? "" : std::to_string(i), target->edram_base, target->pitch,
                               clear ? "CLEAR" : "LOAD", discard ? "DONT_CARE" : "STORE");
    }
  }
  Stats().Trace("  pass {}x{} {}x:{}", width, height, uint32_t(samples), described);
  pass_draws_ = 0;
  const VkRenderPass begin_pass = (clear_mask || discard_mask)
                                      ? pipelines_.RenderPass(formats, samples, clear_mask, discard_mask)
                                      : pass;
  if (!begin_pass) {
    return false;
  }

  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = begin_pass;
  begin.framebuffer = framebuffer;
  begin.renderArea = {{0, 0}, {area_width, area_height}};
  begin.clearValueCount = clear_mask ? attachment_count : 0;
  begin.pClearValues = clear_values.data();
  VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
  attachments.attachmentCount = view_count;
  attachments.pAttachments = views.data();
  if (context_.imageless_framebuffer()) {
    begin.pNext = &attachments;
  }
  // GPU time (measuring only): what came since the last pass, then, at EndPass, this pass by its targets.
  pass_label_ = 0;
  if (context_.timestamp_bits() && REXCVAR_GET(nfsmw_gpu_por_pasada)) {
    if (!between_label_) {
      between_label_ = frames_.GpuLabel("between passes (copies, clears, resolves)");
    }
    frames_.MarkGpu(between_label_);
    uint32_t colors = 0;
    const Image* first = nullptr;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
      colors += targets[i] ? 1 : 0;
      first = first ? first : targets[i];
    }
    first = first ? first : targets[kMaxColorTargets];
    pass_label_ = frames_.GpuLabel(fmt::format("pass {}x{} {}x, {} color{} at EDRAM {:03X}", area_width, area_height,
                                               uint32_t(samples), colors, targets[kMaxColorTargets] ? " + depth" : "",
                                               first->edram_base));
  }
  context_.fn().vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
  pass_ = {pass, framebuffer, targets, width, height, samples, area_width, area_height};
  for (const Image* target : targets) {
    if (target) {
      pass_.scale = target->scale;  // all the targets of a pass share their pitch, hence their scale
    }
  }
  in_pass_ = true;
  return true;
}

void Draws::ComputeFraming(const GuestRegisters& r, Framing& out) const {
  const bool no_clipping = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
  VkViewport& viewport = out.viewport;
  float* ndc = out.ndc;
  bool& empty = out.empty;
  const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
  float scale_x = (vte & 0x1) ? r.Float(gr::XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  float center_x = (vte & 0x2) ? r.Float(gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  float scale_y = (vte & 0x4) ? r.Float(gr::XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float center_y = (vte & 0x8) ? r.Float(gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  const float scale_z = (vte & 0x10) ? r.Float(gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
  const float center_z = (vte & 0x20) ? r.Float(gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
  const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  const int32_t window_x = SignExtend15(window & 0x7FFF);
  const int32_t window_y = SignExtend15((window >> 16) & 0x7FFF);
  if ((r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL] >> 16) & 0x1) {  // vtx_window_offset_enable
    center_x += float(window_x);
    center_y += float(window_y);
  }
  viewport = VkViewport{};
  viewport.x = center_x - std::abs(scale_x);
  viewport.width = 2.0f * std::abs(scale_x);
  // A negative YSCALE (Direct3D's convention) gives a positive height; the shader flips Y at its end.
  viewport.y = center_y + scale_y;
  viewport.height = -2.0f * scale_y;
  viewport.minDepth = std::clamp(center_z, 0.0f, 1.0f);
  viewport.maxDepth = std::clamp(center_z + scale_z, 0.0f, 1.0f);
  // A target drawn smaller than the game asks (Image::scale): the guest's pixels map onto fewer of the image's.
  const float s = pass_.scale;
  viewport.x *= s;
  viewport.y *= s;
  viewport.width *= s;
  viewport.height *= s;
  // g_NdcScale and g_NdcOffset: identity with clipping on; with clipping off the position comes in pixels (the
  // guest's, so the framebuffer is measured in them).
  ndc[0] = ndc[1] = 1.0f;
  ndc[2] = ndc[3] = 0.0f;
  const float pass_width = float(pass_.width) / s, pass_height = float(pass_.height) / s;
  if (no_clipping) {
    ndc[0] = scale_x * 2.0f / pass_width;
    ndc[2] = (center_x - pass_width * 0.5f) * 2.0f / pass_width;
    ndc[1] = -scale_y * 2.0f / pass_height;
    ndc[3] = -(center_y - pass_height * 0.5f) * 2.0f / pass_height;
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = float(pass_.width);
    viewport.height = float(pass_.height);
  }
  empty = viewport.width < 1.0f || std::abs(viewport.height) < 1.0f;

  // The window scissor, clamped to the pass.
  const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
  int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
  if (!((tl >> 31) & 0x1)) {  // window_offset_disable
    x0 += window_x;
    y0 += window_y;
    x1 += window_x;
    y1 += window_y;
  }
  if (s < 1.0f) {
    x0 = int32_t(std::floor(float(x0) * s));
    y0 = int32_t(std::floor(float(y0) * s));
    x1 = int32_t(std::ceil(float(x1) * s));
    y1 = int32_t(std::ceil(float(y1) * s));
  }
  x0 = std::clamp(x0, 0, int32_t(pass_.width));
  x1 = std::clamp(x1, 0, int32_t(pass_.width));
  y0 = std::clamp(y0, 0, int32_t(pass_.height));
  y1 = std::clamp(y1, 0, int32_t(pass_.height));
  if (diagnostic_quarter_) {
    x1 = x0 + std::max((x1 - x0) / 2, 1);
    y1 = y0 + std::max((y1 - y0) / 2, 1);
  }
  empty = empty || x1 <= x0 || y1 <= y0;
  out.scissor = {{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
}

void Draws::ApplyFraming(VkCommandBuffer commands, const Framing& framing) {
  const VkViewport& viewport = framing.viewport;
  const VkRect2D& scissor = framing.scissor;
  // Like the rest of the dynamic state, only recorded when it changes (it survives render pass changes).
  if (!bound_.viewport_valid || std::memcmp(&viewport, &bound_.viewport, sizeof(viewport)) != 0) {
    context_.fn().vkCmdSetViewport(commands, 0, 1, &viewport);
    bound_.viewport = viewport;
    ++Stats().counters().viewport_sets;
  }
  if (!bound_.viewport_valid || std::memcmp(&scissor, &bound_.scissor, sizeof(scissor)) != 0) {
    context_.fn().vkCmdSetScissor(commands, 0, 1, &scissor);
    bound_.scissor = scissor;
    ++Stats().counters().scissor_sets;
  }
  bound_.viewport_valid = true;
}

bool Draws::BuildGeometry(const GuestRegisters& r, uint32_t primitive_type, bool restart, Geometry& out) {
  const uint32_t initiator = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
  const uint32_t source = (initiator >> 6) & 0x3;
  const bool indices_32 = (initiator >> 11) & 0x1;
  const uint32_t count = initiator >> 16;
  const uint32_t index_offset = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
  VkPrimitiveTopology topology;
  bool quads, allows_restart;
  Topology(primitive_type, topology, quads, allows_restart);
  restart = restart && allows_restart;

  // The same index range is often drawn several times per frame (the passes of one mesh): convert it once.
  uint64_t cache_key = 0;
  if (source == uint32_t(xenos::SourceSelect::kDMA)) {
    const uint32_t key_words[6] = {r[gr::XE_GPU_REG_VGT_DMA_BASE], r[gr::XE_GPU_REG_VGT_DMA_SIZE], initiator,
                                   index_offset,
                                   restart ? (r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF) : UINT32_MAX,
                                   primitive_type};
    cache_key = XXH3_64bits(key_words, sizeof(key_words));
    if (auto it = index_copies_.find(cache_key); it != index_copies_.end()) {
      out = it->second;
      return true;
    }
  }
  // The guest bytes of the indices (DMA source), for the persistent copy: converted in an earlier frame and not
  // written since, they are used as they are.
  uint32_t source_base = 0, source_bytes = 0;
  if (cache_key) {
    const uint32_t bytes_per_index = indices_32 ? 4 : 2;
    source_base = (r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes_per_index - 1)) & 0x1FFFFFFF;
    source_bytes = count * bytes_per_index;
    GeometryCache::Slice slice;
    GeometryCache::Extra extra;
    if (source_bytes && geometry_cache_.Find(cache_key, source_base, source_bytes, slice, &extra)) {
      out.index_buffer = slice.buffer;
      out.index_offset = slice.offset;
      out.count = extra[0];
      out.first_vertex = extra[1];
      out.vertex_count = extra[2];
      out.indexed = extra[3] != 0;
      index_copies_.emplace(cache_key, out);
      return true;
    }
  }

  // Indices are converted to 32 bits in upload memory: offset applied, restarts as 0xFFFFFFFF, quads as triangles.
  const uint32_t index_count = quads ? (count / 4) * 6 : count;
  const bool indexed = source == uint32_t(xenos::SourceSelect::kDMA) || quads;
  uint32_t* indices = nullptr;
  bool persistent = false;
  if (indexed) {
    GeometryCache::Slice slice;
    if (cache_key && index_count && source_bytes &&
        geometry_cache_.Begin(cache_key, source_base, source_bytes, VkDeviceSize(index_count) * 4, 4, slice)) {
      persistent = true;
      indices = reinterpret_cast<uint32_t*>(slice.data);
      out.index_buffer = slice.buffer;
      out.index_offset = slice.offset;
    } else {
      FrameRing::Allocation allocation;
      if (!index_count || !frames_.Allocate(VkDeviceSize(index_count) * 4, 4, allocation)) {
        return index_count ? Reject(13, "upload memory full (indices)") : false;
      }
      indices = reinterpret_cast<uint32_t*>(allocation.data);
      out.index_buffer = allocation.buffer;
      out.index_offset = allocation.offset;
    }
  }

  uint32_t lowest = UINT32_MAX, highest = 0;
  if (source == uint32_t(xenos::SourceSelect::kDMA)) {
    const uint32_t size = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
    const auto endian = static_cast<xenos::Endian>(size >> 30);
    const uint32_t bytes = indices_32 ? 4 : 2;
    const uint32_t base = (r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1)) & 0x1FFFFFFF;
    if (count > (size & 0xFFFFFF) || uint64_t(base) + uint64_t(count) * bytes > kGuestMemoryBytes) {
      return Reject(10, "indices outside their buffer");
    }
    const uint8_t* data = memory_->TranslatePhysical(base);
    const uint32_t reset_index = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
    // Quads go through the scratch buffer and are expanded afterwards.
    uint32_t* decoded = indices;
    if (quads) {
      index_scratch_.resize(count);
      decoded = index_scratch_.data();
    }
    IndexRange range;
    if (indices_32) {
      CopySwapped32(reinterpret_cast<uint8_t*>(decoded), data, size_t(count) * 4, endian);
      range = restart ? FinishIndices<true>(decoded, count, index_offset, reset_index)
                      : FinishIndices<false>(decoded, count, index_offset, reset_index);
    } else {
      range = restart ? DecodeIndices16<true>(data, decoded, count, endian, index_offset, reset_index)
                      : DecodeIndices16<false>(data, decoded, count, endian, index_offset, reset_index);
    }
    lowest = range.lowest;
    highest = range.highest;
    if (lowest > highest) {
      return false;  // only restarts: nothing to draw
    }
  } else if (source == uint32_t(xenos::SourceSelect::kAutoIndex)) {
    lowest = index_offset;
    highest = index_offset + count - 1;
    if (quads) {
      index_scratch_.resize(count);
      for (uint32_t i = 0; i < count; ++i) {
        index_scratch_[i] = index_offset + i;
      }
    }
  } else {
    return Reject(11, "immediate indices");
  }
  if (highest - lowest >= kMaxVerticesPerDraw) {
    return Reject(12, "vertex range too large");
  }
  if (quads) {
    for (uint32_t q = 0; q < count / 4; ++q) {
      const uint32_t* in = &index_scratch_[q * 4];
      uint32_t* o = &indices[q * 6];
      o[0] = in[0], o[1] = in[1], o[2] = in[2], o[3] = in[0], o[4] = in[2], o[5] = in[3];
    }
  }
  out.first_vertex = lowest;
  out.vertex_count = highest - lowest + 1;
  out.indexed = indexed;
  out.count = indexed ? index_count : count;
  if (cache_key) {
    index_copies_.emplace(cache_key, out);
  }
  if (persistent) {
    geometry_cache_.Commit(cache_key, {out.count, out.first_vertex, out.vertex_count, out.indexed ? 1u : 0u});
  }
  return true;
}

void Draws::DropStaleCopies() {
  const uint32_t sync = ::nfsmw::nativo::g_sincronizaciones_anillo.load(std::memory_order_relaxed);
  if (sync != copies_sync_) {
    copies_sync_ = sync;
    // The game waited for the GPU: it may have rewritten vertex and index data.
    vertex_copies_.clear();
    index_copies_.clear();
  }
}

bool Draws::BindVertexBuffers(VkCommandBuffer commands, const GuestRegisters& r, const VertexInput& input,
                              const Geometry& geometry) {
  std::array<VkBuffer, 16> buffers{};
  std::array<VkDeviceSize, 16> offsets{};
  if (input.bindings.size() > buffers.size()) {
    return Reject(20, "too many vertex streams");
  }
  for (size_t b = 0; b < input.bindings.size(); ++b) {
    const VertexBinding& binding = input.bindings[b];
    const xenos::xe_gpu_vertex_fetch_t fetch = r.VertexFetch(binding.fetch_slot);
    if (fetch.type != xenos::FetchConstantType::kVertex) {
      return Reject(21, "vertex fetch constant of another type");
    }
    // Only the vertices this draw uses: [first_vertex, first_vertex + vertex_count).
    const uint64_t buffer_address = uint64_t(fetch.address) << 2;
    const uint64_t buffer_bytes = uint64_t(fetch.size) << 2;
    const uint64_t start = buffer_address + uint64_t(geometry.first_vertex) * binding.stride;
    uint64_t bytes = uint64_t(geometry.vertex_count) * binding.stride;
    if (start >= buffer_address + buffer_bytes) {
      return Reject(22, "vertices outside their buffer");
    }
    bytes = std::min(bytes, buffer_address + buffer_bytes - start);
    if (start + bytes > kGuestMemoryBytes) {
      return Reject(22, "vertices outside their buffer");
    }
    const uint64_t key = start ^ (bytes << 32) ^ (uint64_t(fetch.endian) << 62);
    auto copy = vertex_copies_.find(key);
    if (copy == vertex_copies_.end()) {
      // Kept from an earlier frame while the game has not written those vertices since; else converted into the
      // persistent memory, or (a range rewritten too often, or no room) into this frame's upload memory.
      const uint64_t words[4] = {start, bytes, uint64_t(fetch.endian), uint64_t(binding.stride) | (1ull << 32)};
      const uint64_t persistent_key = XXH3_64bits(words, sizeof(words));
      GeometryCache::Slice slice;
      if (geometry_cache_.Find(persistent_key, uint32_t(start), uint32_t(bytes), slice)) {
        copy = vertex_copies_.emplace(key, VertexCopy{slice.buffer, slice.offset}).first;
      } else if (geometry_cache_.Begin(persistent_key, uint32_t(start), uint32_t(bytes), bytes + binding.stride, 16,
                                       slice)) {
        const uint8_t* source = memory_->TranslatePhysical(uint32_t(start));
        CopySwapped32(slice.data, source, size_t(bytes), fetch.endian);
        std::memset(slice.data + (bytes & ~uint64_t(3)), 0, size_t(binding.stride + (bytes & 3)));
        geometry_cache_.Commit(persistent_key);
        copy = vertex_copies_.emplace(key, VertexCopy{slice.buffer, slice.offset}).first;
      }
    }
    if (copy == vertex_copies_.end()) {
      FrameRing::Allocation allocation;
      // A stride larger than the data left keeps reads inside the binding: pad to one more stride.
      if (!frames_.Allocate(bytes + binding.stride, 16, allocation)) {
        return Reject(23, "upload memory full (vertices)");
      }
      const uint8_t* source = memory_->TranslatePhysical(uint32_t(start));
      // Vertex data is swapped in 32-bit units.
      CopySwapped32(allocation.data, source, size_t(bytes), fetch.endian);
      std::memset(allocation.data + (bytes & ~uint64_t(3)), 0, size_t(binding.stride + (bytes & 3)));
      copy = vertex_copies_.emplace(key, VertexCopy{allocation.buffer, allocation.offset}).first;
    }
    buffers[b] = copy->second.buffer;
    // The draw addresses vertex `first_vertex` as 0 (vertexOffset / firstVertex below).
    offsets[b] = copy->second.offset;
  }
  const uint32_t count = uint32_t(input.bindings.size());
  if (count && (count != bound_.vertex_buffer_count ||
                !std::equal(offsets.begin(), offsets.begin() + count, bound_.vertex_offsets.begin()) ||
                !std::equal(buffers.begin(), buffers.begin() + count, bound_.vertex_buffers.begin()))) {
    context_.fn().vkCmdBindVertexBuffers(commands, 0, count, buffers.data(), offsets.data());
    ++Stats().counters().vertex_buffer_binds;
    std::copy(buffers.begin(), buffers.begin() + count, bound_.vertex_buffers.begin());
    std::copy(offsets.begin(), offsets.begin() + count, bound_.vertex_offsets.begin());
    bound_.vertex_buffer_count = count;
  }
  return true;
}

// The registers a shader reads, as its translation reported; until then (or if it failed), the constant table's
// estimate. Cached here so the ring thread does not take the shader modules' lock on every draw.
uint32_t Draws::ConstantBytes(const nativo::EntradaShader& shader) {
  if (auto it = constant_bytes_.find(&shader); it != constant_bytes_.end()) {
    return it->second;
  }
  const uint32_t translated = pipelines_.shaders().ConstantBytes(shader);
  if (!translated) {
    return shader.constantes_bytes;  // not cached: asked again until the translation exists
  }
  constant_bytes_.emplace(&shader, translated);
  return translated;
}

bool Draws::UploadConstants(const nativo::PeticionDibujo& request, const GuestRegisters& r, const uint8_t* shared,
                            std::array<uint32_t, 3>& offsets, VkBuffer& buffer) {
  const VkDeviceSize alignment = context_.uniform_alignment();
  // `reserved` bytes are allocated (the descriptor's range must stay inside the buffer) but only `bytes` are written:
  // the shader reads no further. Upload memory is write-combined, so the copy is what costs.
  auto upload = [&](const void* data, uint32_t bytes, uint32_t reserved, VkDeviceSize& offset) {
    FrameRing::Allocation allocation;
    if (!frames_.Allocate(reserved, alignment, allocation)) {
      return false;
    }
    std::memcpy(allocation.data, data, bytes);
    if (constants_buffer_ != allocation.buffer) {
      // Another upload buffer: the previous offsets belong to the old one.
      vs_constants_generation_ = ps_constants_generation_ = UINT64_MAX;
      constants_buffer_ = allocation.buffer;
    }
    offset = allocation.offset;
    return true;
  };
  // EntradaShader::constantes_bytes: the registers the shader reads (all of them with relative addressing). The
  // previous upload is reused while the registers have not changed and it covers what this shader reads.
  const uint32_t vs_bytes = std::min(ConstantBytes(*request.vs), kVertexConstantBytes);
  if (request.generacion_constantes_vs != vs_constants_generation_ || !constants_buffer_ ||
      vs_bytes > vs_constants_bytes_) {
    if (!upload(r.data() + kVertexConstantsRegister, vs_bytes, kVertexConstantBytes, vs_constants_offset_)) {
      return Reject(30, "upload memory full (constants)");
    }
    ++Stats().counters().vs_constant_uploads;
    Stats().counters().vs_constant_bytes += vs_bytes;
    vs_constants_generation_ = request.generacion_constantes_vs;
    vs_constants_bytes_ = vs_bytes;
  }
  const uint32_t ps_bytes = request.ps ? std::max(std::min(ConstantBytes(*request.ps), kPixelConstantBytes), 16u) : 16;
  if (request.generacion_constantes_ps != ps_constants_generation_ || ps_bytes > ps_constants_bytes_) {
    if (!upload(r.data() + kPixelConstantsRegister, ps_bytes, kPixelConstantBytes, ps_constants_offset_)) {
      return Reject(30, "upload memory full (constants)");
    }
    ++Stats().counters().ps_constant_uploads;
    Stats().counters().ps_constant_bytes += ps_bytes;
    ps_constants_generation_ = request.generacion_constantes_ps;
    ps_constants_bytes_ = ps_bytes;
  }
  VkDeviceSize shared_offset;
  if (!upload(shared, SharedBlockV11::kSize, SharedBlockV11::kSize, shared_offset)) {
    return Reject(30, "upload memory full (constants)");
  }
  // Each frame slot has a single upload buffer, so the three blocks always share it.
  buffer = constants_buffer_;
  offsets = {uint32_t(vs_constants_offset_), uint32_t(ps_constants_offset_), uint32_t(shared_offset)};
  return true;
}

bool Draws::Draw(const nativo::PeticionDibujo& request) {
  if (!request.registros || !request.vs) {
    return false;
  }
  // CPU time per stage (nfsmw_perfil_dibujos): each Stage() call ends one and starts the next.
  using Clock = std::chrono::steady_clock;
  const bool profile = REXCVAR_GET(nfsmw_perfil_dibujos);
  Clock::time_point stage_start = profile ? Clock::now() : Clock::time_point{};
  const auto Stage = [&](DrawStage stage) {
    if (profile) {
      const Clock::time_point now = Clock::now();
      draw_profile_.ns[size_t(stage)] +=
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - stage_start).count());
      stage_start = now;
    }
  };
  const GuestRegisters r(request.registros);
  const auto& fn = context_.fn();

  // --- What is drawn --------------------------------------------------------------------------------------------
  const uint32_t edram_mode = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
  if (edram_mode != uint32_t(xenos::EdramMode::kColorDepth) && edram_mode != 5) {
    return Reject(1, "EDRAM mode without color or depth");
  }
  // Mode 5 (depth only): the Xenos does not run the pixel shader.
  const nativo::EntradaShader* ps = edram_mode == uint32_t(xenos::EdramMode::kColorDepth) ? request.ps : nullptr;
  if (!ps && edram_mode == uint32_t(xenos::EdramMode::kColorDepth)) {
    return false;
  }
  const uint32_t initiator = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
  const uint32_t primitive_type = initiator & 0x3F;
  if (!(initiator >> 16)) {
    return true;
  }
  // Xenos tessellation: VGT_OUTPUT_PATH_CNTL path 1 with the patch primitives (0x10 line, 0x11 triangle, 0x12 quad)
  // turns the vertex shader into a domain shader. Neither the translator nor this renderer implements it, so such a
  // draw is counted, its state logged once, and dropped.
  const uint32_t output_path = r[gr::XE_GPU_REG_VGT_OUTPUT_PATH_CNTL] & 0x3;
  if (output_path == 1 || (primitive_type >= 0x10 && primitive_type <= 0x12)) {
    ++tessellated_draws_;
    if (!logged_tessellation_) {
      logged_tessellation_ = true;
      REXLOG_WARN("[renderer] tessellated draw: primitive {:02X}, VGT_OUTPUT_PATH_CNTL {:08X}, VGT_HOS_CNTL {:08X}, "
                  "tessellation levels {} to {}, VS n{}",
                  primitive_type, r[gr::XE_GPU_REG_VGT_OUTPUT_PATH_CNTL], r[gr::XE_GPU_REG_VGT_HOS_CNTL],
                  r.Float(gr::XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL), r.Float(gr::XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL),
                  request.vs->numero);
    }
    return false;
  }
  VkPrimitiveTopology topology;
  bool quads, allows_restart;
  if (!Topology(primitive_type, topology, quads, allows_restart)) {
    return Reject(100 + std::min<uint32_t>(primitive_type, 63), "primitive type not supported");
  }
  const uint32_t mode_control = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  if (((mode_control >> 3) & 0x3) == 2 && ((mode_control >> 5) & 0x7) != 2) {
    return Reject(2, "polygons drawn as points or lines");
  }

  // --- Render targets -------------------------------------------------------------------------------------------
  const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
  const uint32_t color_mask_register = ps ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
  std::array<const Image*, kMaxColorTargets + 1> targets{};
  uint32_t write_masks = 0;
  for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
    const uint32_t mask = (color_mask_register >> (i * 4)) & 0xF;
    if (!mask || !ps || !((ps->salidas >> i) & 0x1)) {
      continue;
    }
    const uint32_t info = r[kColorInfoRegisters[i]];
    const uint32_t format = (info >> 16) & 0xF;
    if (format != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) &&
        format != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
      return Reject(200 + format, "color render target format not supported");
    }
    targets[i] = render_targets_.ColorTarget(info & 0xFFF, format, pitch);
    if (!targets[i]) {
      return false;
    }
    write_masks |= mask << (i * 4);
  }
  const uint32_t depth_control = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
  if (depth_control & 0x3) {  // stencil or depth test
    const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
    targets[kMaxColorTargets] = render_targets_.DepthTarget(info & 0xFFF, (info >> 16) & 0x1, pitch);
  }
  bool any_target = false;
  for (const Image* target : targets) {
    any_target |= target != nullptr;
  }
  if (!any_target) {
    return true;  // writes nothing
  }
  // Vegetation in the shadow maps (nfsmw_sombras_sin_vegetacion, on in the low and medium profiles): in the maps'
  // depth-only passes, a draw that writes no color and cuts its shape by alpha (alpha test or discard) is a tree,
  // a fence or a bush. Dropped whole, as the legacy renderer does (on the Switch -1.2 ms of GPU and -3.1 ms of CPU).
  if (targets[kMaxColorTargets] && pitch == kShadowPitch && ps && REXCVAR_GET(nfsmw_sombras_sin_vegetacion)) {
    bool color = false;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
      color |= targets[i] != nullptr;
    }
    const uint32_t color_control = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    const bool alpha_test = ((color_control >> 3) & 0x1) && (color_control & 0x7) != 7;
    if (!color && (alpha_test || ps->descarta || (ps->salidas & 0x10))) {
      ++Stats().counters().shadow_vegetation_skipped;
      return true;
    }
  }

  // --- Vertex input and geometry --------------------------------------------------------------------------------
  // The vertex input only changes with the vertex shader or its patched microcode (a new IM_LOAD).
  if (request.vs != input_vs_ || request.generacion_vs != input_generation_) {
    input_ = vertex_inputs_.Get(*request.vs, request.vs_microcodigo);
    input_vs_ = request.vs;
    input_generation_ = request.generacion_vs;
  }
  const ShaderVertexInput* vertex_input = input_;
  if (!vertex_input) {
    return Reject(3, "vertex input does not match the shader");
  }
  const bool restart = (mode_control >> 21) & 0x1;
  Stage(DrawStage::kSetup);
  Geometry geometry;
  DropStaleCopies();
  if (!BuildGeometry(r, primitive_type, restart, geometry)) {
    return false;
  }

  Stage(DrawStage::kGeometry);

  // --- Textures (before the pass: uploads go to their own command buffer) ---------------------------------------
  std::array<uint8_t, SharedBlockV11::kSize> shared{};
  std::array<VkDescriptorImageInfo, kTextureBindings> images;  // the first write_count entries are used
  std::array<VkWriteDescriptorSet, kTextureBindings> writes;
  uint32_t write_count = 0;
  const uint64_t frame = frames_.frame_number();
  const uint64_t copies = render_targets_.copy_generation();
  TextureReuse& reuse = texture_reuse_;
  if (ps && reuse.ps == ps && reuse.fetch_generation == request.generacion_fetch && reuse.frame == frame &&
      reuse.copies == copies) {
    // Same shader, same fetch constants, nothing resolved since: the same textures.
    ++Stats().counters().texture_reuses;
    std::memcpy(shared.data() + SharedBlockV11::kInvTextureSize, reuse.inv_sizes.data(), reuse.inv_sizes.size());
    for (uint32_t i = 0; i < reuse.count; ++i) {
      images[i] = reuse.images[i];
      VkWriteDescriptorSet& write = writes[i];
      write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstBinding = reuse.bindings[i];
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &images[i];
    }
    write_count = reuse.count;
  } else if (ps) {
    for (const nativo::SamplerShader& sampler : ps->samplers) {
      if (sampler.registro >= 16) {
        continue;
      }
      const TextureDimension dimension = sampler.tipo == 13   ? TextureDimension::k3D
                                         : sampler.tipo == 14 ? TextureDimension::kCube
                                                              : TextureDimension::k2D;
      const TextureBinding binding = textures_.Get(r, sampler.registro, dimension);
      const float inv_size[2] = {binding.inv_width, binding.inv_height};
      std::memcpy(shared.data() + SharedBlockV11::kInvTextureSize + sampler.registro * 8, inv_size, 8);
      // The shadow map pair (kind 3) is the same texture: the minimum path is off (SPEC_CONSTANT_SOMBRA_MINIMO).
      const uint32_t kinds[2] = {uint32_t(dimension), 3};
      for (uint32_t k = 0; k < (sampler.mapa_sombras ? 2u : 1u); ++k) {
        const uint32_t index = sampler.registro * 4 + kinds[k];
        images[write_count] = {binding.sampler, binding.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet& write = writes[write_count];
        write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstBinding = index;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &images[write_count];
        ++write_count;
      }
    }
    reuse.ps = ps;
    reuse.fetch_generation = request.generacion_fetch;
    reuse.frame = frame;
    reuse.copies = copies;
    reuse.count = write_count;
    for (uint32_t i = 0; i < write_count; ++i) {
      reuse.images[i] = images[i];
      reuse.bindings[i] = writes[i].dstBinding;
    }
    std::memcpy(reuse.inv_sizes.data(), shared.data() + SharedBlockV11::kInvTextureSize, reuse.inv_sizes.size());
  }

  Stage(DrawStage::kTextures);

  // --- Render pass and fixed-function state ---------------------------------------------------------------------
  VkCommandBuffer commands = frames_.Commands();
  // A draw whose targets are all in the open pass stays in it: the pass's other color targets get a zero write mask
  // and, without a depth test, the depth attachment is left alone. Ending the pass would store and reload every
  // attachment for nothing.
  const std::array<const Image*, kMaxColorTargets + 1> pass_targets = JoinsOpenPass(targets) ? pass_.targets : targets;
  if (!commands || !BeginPass(commands, pass_targets)) {
    return false;
  }
  if (request.generacion_encuadre != framing_generation_ || pass_.width != framing_width_ ||
      pass_.height != framing_height_) {
    ComputeFraming(r, framing_);
    framing_generation_ = request.generacion_encuadre;
    framing_width_ = pass_.width;
    framing_height_ = pass_.height;
  }
  if (framing_.empty) {
    return true;
  }
  {
    const uint32_t right = uint32_t(framing_.scissor.offset.x) + framing_.scissor.extent.width;
    const uint32_t bottom = uint32_t(framing_.scissor.offset.y) + framing_.scissor.extent.height;
    if (right > pass_.area_width || bottom > pass_.area_height) {
      // Beyond what these targets were ever drawn into (learning, or the game drew a larger region): the pass is
      // reopened with an area that covers it. Same framebuffer size, so the framing stays valid.
      const std::array<const Image*, kMaxColorTargets + 1> reopened = pass_.targets;
      for (const Image* target : reopened) {
        if (target) {
          target->drawn_width = std::max(target->drawn_width, right);
          target->drawn_height = std::max(target->drawn_height, bottom);
        }
      }
      EndPass();
      ++Stats().counters().passes_reopened;
      if (!BeginPass(commands, reopened)) {
        return false;
      }
    }
    pass_.drawn_width = std::max(pass_.drawn_width, right);
    pass_.drawn_height = std::max(pass_.drawn_height, bottom);
  }
  ApplyFraming(commands, framing_);
  const VkViewport& viewport = framing_.viewport;
  const float* ndc = framing_.ndc;

  // Shared block (SharedBlockV11).
  const uint32_t booleans = (r[kBooleanConstantsRegister] & 0xFFFF) | ((r[kBooleanConstantsRegister + 4] & 0xFFFF) << 16);
  std::memcpy(shared.data() + SharedBlockV11::kBooleans, &booleans, 4);
  if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero: half a pixel
    // Half a guest pixel (the viewport is in the image's pixels, pass_.scale of a guest one).
    const float half_pixel[2] = {pass_.scale / viewport.width, -pass_.scale / std::abs(viewport.height)};
    std::memcpy(shared.data() + SharedBlockV11::kHalfPixelOffset, half_pixel, 8);
  }
  uint32_t specialization = kSpecInvTextureSize;
  const uint32_t color_control = r[gr::XE_GPU_REG_RB_COLORCONTROL];
  if (ps && ((color_control >> 3) & 0x1) && (color_control & 0x7) != 7) {
    const float threshold = r.Float(gr::XE_GPU_REG_RB_ALPHA_REF);
    std::memcpy(shared.data() + SharedBlockV11::kAlphaThreshold, &threshold, 4);
    specialization |= kSpecAlphaTest | ((color_control & 0x7) << kSpecAlphaFuncShift);
  }
  std::memcpy(shared.data() + SharedBlockV11::kNdcScale, ndc, 16);
  std::memcpy(shared.data() + SharedBlockV11::kInputRemap, vertex_input->remap_codes.data(), 64);

  // Depth bias (GetPreferredFacePolygonOffset in the SDK's draw.cpp), in units of the host depth format.
  float bias_scale = 0.0f, bias_offset = 0.0f;
  if (primitive_type >= 4) {
    if (((mode_control >> 11) & 0x1) && !(mode_control & 0x1)) {
      bias_scale = r.Float(gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      bias_offset = r.Float(gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (((mode_control >> 12) & 0x1) && !((mode_control >> 1) & 0x1) && bias_scale == 0.0f && bias_offset == 0.0f) {
      bias_scale = r.Float(gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      bias_offset = r.Float(gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else if ((mode_control >> 13) & 0x1) {
    bias_scale = r.Float(gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
    bias_offset = r.Float(gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  }
  const bool float24 = (r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1;
  const float bias_constant = bias_offset * (float24 ? float(1u << 24) : float((1u << 24) - 1));
  const float bias_slope = bias_scale * (1.0f / 16.0f);
  const bool depth_bias = targets[kMaxColorTargets] && (bias_constant != 0.0f || bias_slope != 0.0f);
  // The pipeline is built for the pass's attachments, which may be more than the draw's.

  Stage(DrawStage::kPass);

  // --- Pipeline ---------------------------------------------------------------------------------------------------
  PassFormats formats{};
  for (uint32_t i = 0; i <= kMaxColorTargets; ++i) {
    formats[i] = pass_targets[i] ? pass_targets[i]->format : VK_FORMAT_UNDEFINED;
  }
  PipelineKey key;
  key.vertex_shader = uint64_t(uintptr_t(request.vs));
  key.pixel_shader = uint64_t(uintptr_t(ps));
  key.vertex_input = vertex_input->input.hash;
  key.render_pass = uint64_t(uintptr_t(pass_.pass));
  key.topology = uint32_t(topology);
  // PA_SU_SC_MODE_CNTL: cull_front (bit 0), cull_back (bit 1), face (bit 2, 1 = clockwise front).
  key.rasterizer = (mode_control & 0x7) | ((restart && allows_restart && geometry.indexed) ? 0x8u : 0u) |
                   (depth_bias ? 0x10u : 0u);
  // Without its own depth target (depth and stencil tests off) the canonical state disables the pass's depth.
  key.depth_stencil = Pipelines::CanonicalDepthStencil(targets[kMaxColorTargets] ? depth_control : 0,
                                                       pass_targets[kMaxColorTargets] != nullptr);
  for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
    key.blend[i] = targets[i] ? Pipelines::CanonicalBlend(r[kBlendControlRegisters[i]], (write_masks >> (i * 4)) & 0xF)
                              : 0;
  }
  key.write_masks = write_masks;
  key.specialization = specialization;
  key.samples = uint32_t(pass_.samples);
  VkPipeline pipeline = pipelines_.Get(key, *request.vs, ps, vertex_input->input, pass_.pass, formats);
  if (!pipeline) {
    return Reject(4, "pipeline could not be created");
  }

  Stage(DrawStage::kPipeline);

  // --- Resources ---------------------------------------------------------------------------------------------------
  std::array<uint32_t, 3> uniform_offsets{};
  VkBuffer uniform_buffer = VK_NULL_HANDLE;
  if (!UploadConstants(request, r, shared.data(), uniform_offsets, uniform_buffer)) {
    return false;
  }
  if (uniform_set_buffer_ != uniform_buffer) {
    uniform_set_ = frames_.AllocateDescriptorSet(pipelines_.uniform_set_layout());
    if (!uniform_set_) {
      return Reject(31, "descriptor pool exhausted");
    }
    const VkDescriptorBufferInfo infos[3] = {{uniform_buffer, 0, kVertexConstantBytes},
                                             {uniform_buffer, 0, kPixelConstantBytes},
                                             {uniform_buffer, 0, SharedBlockV11::kSize}};
    std::array<VkWriteDescriptorSet, 3> uniform_writes{};
    for (uint32_t i = 0; i < 3; ++i) {
      uniform_writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      uniform_writes[i].dstSet = uniform_set_;
      uniform_writes[i].dstBinding = i;
      uniform_writes[i].descriptorCount = 1;
      uniform_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      uniform_writes[i].pBufferInfo = &infos[i];
    }
    fn.vkUpdateDescriptorSets(context_.handle(), 3, uniform_writes.data(), 0, nullptr);
    ++Stats().counters().uniform_set_writes;
    uniform_set_buffer_ = uniform_buffer;
  }
  // The textures of consecutive draws are usually the same: reuse the previous set then.
  bool same_textures = bound_.texture_set && write_count == bound_.texture_slot_count;
  for (uint32_t i = 0; i < write_count && same_textures; ++i) {
    same_textures = bound_.texture_slots[i] == TextureSlot{writes[i].dstBinding, images[i].imageView, images[i].sampler};
  }
  if (!same_textures) {
    VkDescriptorSet texture_set = frames_.AllocateDescriptorSet(pipelines_.texture_set_layout());
    if (!texture_set) {
      return Reject(31, "descriptor pool exhausted");
    }
    for (uint32_t i = 0; i < write_count; ++i) {
      writes[i].dstSet = texture_set;
    }
    if (write_count) {
      fn.vkUpdateDescriptorSets(context_.handle(), write_count, writes.data(), 0, nullptr);
      ++Stats().counters().texture_set_writes;
    }
    bound_.texture_set = texture_set;
    for (uint32_t i = 0; i < write_count; ++i) {
      bound_.texture_slots[i] = {writes[i].dstBinding, images[i].imageView, images[i].sampler};
    }
    bound_.texture_slot_count = write_count;
    bound_.sets_dirty = true;
  }

  Stage(DrawStage::kResources);

  // --- Commands (state equal to the previous draw's is not recorded again) ---------------------------------------
  if (!BindVertexBuffers(commands, r, vertex_input->input, geometry)) {
    return false;
  }
  Stage(DrawStage::kVertices);
  if (pipeline != bound_.pipeline) {
    fn.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    ++Stats().counters().pipeline_binds;
    bound_.pipeline = pipeline;
  }
  if (bound_.sets_dirty || uniform_set_ != bound_.uniform_set || uniform_offsets != bound_.uniform_offsets) {
    const VkDescriptorSet sets[2] = {bound_.texture_set, uniform_set_};
    ++Stats().counters().descriptor_binds;
    fn.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.layout(), 0, 2, sets, 3,
                               uniform_offsets.data());
    bound_.uniform_set = uniform_set_;
    bound_.uniform_offsets = uniform_offsets;
    bound_.sets_dirty = false;
  }
  if (!bound_.dynamic_valid || bias_constant != bound_.depth_bias[0] || bias_slope != bound_.depth_bias[1]) {
    fn.vkCmdSetDepthBias(commands, bias_constant, 0.0f, bias_slope);
    ++Stats().counters().depth_bias_sets;
    bound_.depth_bias[0] = bias_constant;
    bound_.depth_bias[1] = bias_slope;
  }
  const std::array<float, 4> blend_constants = {r.Float(gr::XE_GPU_REG_RB_BLEND_RED), r.Float(gr::XE_GPU_REG_RB_BLEND_GREEN),
                                                r.Float(gr::XE_GPU_REG_RB_BLEND_BLUE), r.Float(gr::XE_GPU_REG_RB_BLEND_ALPHA)};
  if (!bound_.dynamic_valid || blend_constants != bound_.blend_constants) {
    fn.vkCmdSetBlendConstants(commands, blend_constants.data());
    ++Stats().counters().blend_constant_sets;
    bound_.blend_constants = blend_constants;
  }
  // RB_STENCILREFMASK: reference (bits 0-7), compare mask (8-15), write mask (16-23); the back face has its own
  // register when backface stencil is enabled (RB_DEPTHCONTROL bit 7).
  const uint32_t front = r[gr::XE_GPU_REG_RB_STENCILREFMASK] & 0xFFFFFF;
  const uint32_t back = ((depth_control >> 7) & 0x1) ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF] & 0xFFFFFF : front;
  if (!bound_.dynamic_valid || front != bound_.stencil[0] || back != bound_.stencil[1]) {
    ++Stats().counters().stencil_sets;
    fn.vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
    fn.vkCmdSetStencilReference(commands, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
    fn.vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_BIT, (front >> 8) & 0xFF);
    fn.vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
    fn.vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_BIT, (front >> 16) & 0xFF);
    fn.vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
    bound_.stencil[0] = front;
    bound_.stencil[1] = back;
  }
  bound_.dynamic_valid = true;
  if (geometry.indexed) {
    // All indices of a frame live in its upload buffer: it is bound once and each draw starts at its own index.
    if (geometry.index_buffer != bound_.index_buffer) {
      fn.vkCmdBindIndexBuffer(commands, geometry.index_buffer, 0, VK_INDEX_TYPE_UINT32);
      ++Stats().counters().index_buffer_binds;
      bound_.index_buffer = geometry.index_buffer;
    }
    fn.vkCmdDrawIndexed(commands, geometry.count, 1, uint32_t(geometry.index_offset / 4),
                        -int32_t(geometry.first_vertex), 0);
  } else {
    fn.vkCmdDraw(commands, geometry.count, 1, 0, 0);
  }
  ++draw_count_;
  ++pass_draws_;
  ++Stats().counters().draws;
  Stage(DrawStage::kCommands);
  ++draw_profile_.draws;
  return true;
}

}  // namespace nfsmw::renderer
