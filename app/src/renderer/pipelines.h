// Pipeline layout, render passes, framebuffers and graphics pipelines.
//
// One pipeline layout serves every draw (SpirvResourceModel::Vulkan11): set 0 holds the draw's textures, set 1 the
// three constant buffers. Render passes load and store every attachment in VK_IMAGE_LAYOUT_GENERAL, so passes can
// be split anywhere (copies, clears) without losing contents. Viewport, scissor, depth bias, blend constants and
// stencil reference and masks are dynamic; everything else is part of the pipeline key.
//
// Creating a pipeline (and translating its shaders) takes tens of milliseconds, so it must not happen in the middle
// of a race. Every pipeline created is recorded in a list saved with the driver's pipeline cache
// (cache/nfsmw_vk11_pipelines.bin); on the next start the maintenance thread recreates the list (WarmUp) before the
// game asks for those pipelines. The ring thread and that thread share this class.

#pragma once

#include "vulkan_context.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nfsmw::nativo {
struct EntradaShader;
class ShadersNativos;
}

namespace nfsmw::renderer {

class ShaderModules;
struct Image;

constexpr uint32_t kMaxColorTargets = 4;
constexpr uint32_t kTextureBindings = 64;  // 16 fetch registers x (2D, 3D, cube, shadow pair)

struct VertexAttribute {
  uint32_t location = 0;
  uint32_t binding = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t offset = 0;
};

struct VertexBinding {
  uint32_t fetch_slot = 0;  // vertex fetch constant index (0-95)
  uint32_t stride = 0;
};

struct VertexInput {
  std::vector<VertexAttribute> attributes;
  std::vector<VertexBinding> bindings;
  uint64_t hash = 0;
};

// The render pass formats: color 0-3 then depth; VK_FORMAT_UNDEFINED for an absent attachment.
using PassFormats = std::array<VkFormat, kMaxColorTargets + 1>;

struct PipelineKey {
  uint64_t vertex_shader = 0;  // EntradaShader address
  uint64_t pixel_shader = 0;   // 0: no fragment stage (depth-only)
  uint64_t vertex_input = 0;   // VertexInput::hash
  uint64_t render_pass = 0;
  uint32_t topology = 0;
  uint32_t rasterizer = 0;     // bit 0 cull front, 1 cull back, 2 clockwise front, 3 primitive restart, 4 depth bias
  uint32_t depth_stencil = 0;  // RB_DEPTHCONTROL, canonical
  std::array<uint32_t, kMaxColorTargets> blend{};  // RB_BLENDCONTROL per target, canonical
  uint32_t write_masks = 0;    // 4 bits per target
  uint32_t specialization = 0;
  uint32_t samples = 1;  // VkSampleCountFlagBits of the pass (the key is hashed as bytes: no implicit padding)

  bool operator==(const PipelineKey& other) const;
};

class Pipelines {
 public:
  Pipelines(const VulkanContext& context, ShaderModules& shaders) : context_(context), shaders_(shaders) {}
  ~Pipelines();

  bool Initialize();

  VkPipelineLayout layout() const { return layout_; }
  ShaderModules& shaders() { return shaders_; }
  VkDescriptorSetLayout texture_set_layout() const { return texture_set_layout_; }
  VkDescriptorSetLayout uniform_set_layout() const { return uniform_set_layout_; }

  // `clear_mask` (bit per attachment slot, 4 = depth): loadOp CLEAR instead of LOAD; `discard_mask`: storeOp
  // DONT_CARE instead of STORE. Passes that differ only in those are compatible: pipelines and framebuffers are
  // always made with the plain one (both masks 0), the others only begin passes.
  VkRenderPass RenderPass(const PassFormats& formats, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                          uint32_t clear_mask = 0, uint32_t discard_mask = 0);
  // For the targets of a pass (null slots are absent). Imageless (context().imageless_framebuffer()), it depends on
  // what the targets are like, not on which ones they are; the pass begin then gives their views.
  VkFramebuffer Framebuffer(VkRenderPass pass, const std::array<const Image*, kMaxColorTargets + 1>& targets,
                            uint32_t width, uint32_t height);

  // The pipeline for a draw; VK_NULL_HANDLE if it cannot be created (logged once per cause).
  VkPipeline Get(const PipelineKey& key, const nativo::EntradaShader& vertex_shader,
                 const nativo::EntradaShader* pixel_shader, const VertexInput& input, VkRenderPass pass,
                 const PassFormats& formats);

  // Canonical forms: fields that cannot change the result are cleared so equivalent states share a pipeline.
  static uint32_t CanonicalDepthStencil(uint32_t depth_control, bool has_depth);
  static uint32_t CanonicalBlend(uint32_t blend_control, uint32_t write_mask);

  // Pipelines the ring thread had to create itself (including their shaders' translation, the first time) since the
  // last call: each one is a potential hitch.
  struct CreationStats {
    uint64_t created = 0;
    uint64_t create_ns = 0;
  };
  CreationStats TakeStats();

  // Loads the cache file (call before any pipeline is created); without one, the cache starts empty.
  void LoadCacheFile(const std::filesystem::path& path);
  // Writes the cache file if pipelines were created since it was last written.
  bool SaveCacheFile();
  // Creates the pipelines of the loaded list whose shaders are still in the library, until `stop`. Returns how many.
  uint32_t WarmUp(const nativo::ShadersNativos& library, const std::atomic<bool>& stop);

 private:
  struct KeyHash {
    size_t operator()(const PipelineKey& key) const;
  };

  // What is needed to create a pipeline again in another run: shaders by library number (checked against their
  // fingerprint), the vertex input and the pass formats.
  struct Recipe {
    PipelineKey key;  // shader and render pass fields are rebuilt when loading
    uint32_t vertex_shader = 0;
    uint32_t pixel_shader = UINT32_MAX;  // UINT32_MAX: none
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    PassFormats formats{};
    std::vector<VertexAttribute> attributes;
    std::vector<VertexBinding> bindings;
  };

  VkPipeline Create(const PipelineKey& key, const nativo::EntradaShader& vertex_shader,
                    const nativo::EntradaShader* pixel_shader, const VertexInput& input, VkRenderPass pass,
                    const PassFormats& formats);
  // Publishes a created pipeline; returns the one to use (another thread may have published it first).
  VkPipeline Insert(const PipelineKey& key, VkPipeline pipeline, const nativo::EntradaShader& vertex_shader,
                    const nativo::EntradaShader* pixel_shader, const VertexInput& input, const PassFormats& formats);

  const VulkanContext& context_;
  ShaderModules& shaders_;
  VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout uniform_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  std::mutex render_pass_mutex_;
  std::unordered_map<uint64_t, VkRenderPass> render_passes_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;  // ring thread only

  std::mutex mutex_;  // pipelines_, recipes_, dirty_, stats_
  std::unordered_map<PipelineKey, VkPipeline, KeyHash> pipelines_;
  std::vector<Recipe> recipes_;  // every pipeline created in this run, in order
  bool dirty_ = false;
  CreationStats stats_;

  VkPipelineCache cache_ = VK_NULL_HANDLE;
  std::filesystem::path cache_path_;
  std::vector<Recipe> loaded_recipes_;
};

}  // namespace nfsmw::renderer
