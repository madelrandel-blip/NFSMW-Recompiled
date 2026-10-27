// The swap: draws the front buffer into the presenter's guest output image.
//
// The output pass (shaders/output.frag) applies the gamma ramp the game loaded (the Xbox 360 display applies it after
// scan-out), the optional post-processing of the Graficos menu, FXAA and FSR1 RCAS sharpening. It is recorded at the
// end of the frame's own command buffer, inside the presenter's refresh callback, and the frame is submitted from
// there: one queue submission per frame. Its constants go through a dynamic uniform buffer in the frame's upload
// memory; there are no push constants.

#pragma once

#include "frame_ring.h"
#include "render_targets.h"
#include "vulkan_context.h"

#include "../nfsmw_ajustes_graficos.h"

#include <array>
#include <cstdint>
#include <utility>

namespace rex::ui {
class Presenter;
}

namespace nfsmw::renderer {

class Output {
 public:
  using GammaRamp = std::array<std::array<uint16_t, 3>, 256>;

  Output(const VulkanContext& context, FrameRing& frames) : context_(context), frames_(frames) {}
  ~Output();
  Output(const Output&) = delete;
  Output& operator=(const Output&) = delete;

  bool Initialize();

  // 256 entries of 10 bits per channel (red, green, blue).
  void SetGammaRamp(const GammaRamp& ramp);

  // Records the output of `width` x `height` texels of `source` (its top-left corner) after the frame's work and
  // submits the frame. false if the presenter did not ask for an output: the frame is then still open, and the
  // caller submits it.
  bool Present(rex::ui::Presenter* presenter, const Image& source, uint32_t width, uint32_t height);

  // Where Present's time goes, summed since the last call: in the presenter before it hands over the output image
  // (it may wait for the GPU to release it), recording the output pass, submitting the frame (Submit's own lock and
  // call times are in the context), and in the presenter after the submission (sealing the refresh, asking for the
  // paint).
  struct Timing {
    uint64_t before_ns = 0;
    uint64_t record_ns = 0;
    uint64_t submit_ns = 0;
    uint64_t after_ns = 0;
  };
  Timing TakeTiming() { return std::exchange(timing_, Timing{}); }

 private:
  static constexpr uint32_t kVariants = 8;  // grading (1), FXAA (2), RCAS (4): the shader's constants
  // std140 layout of shaders/output.frag's Constants: ramp[256], grading, effects, texel.
  static constexpr size_t kConstantFloats = 256 * 4 + 3 * 4;

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t image_version = 0;
    uint32_t width = 0, height = 0;
  };

  VkPipeline Pipeline(uint32_t variant);
  VkFramebuffer FramebufferFor(VkImageView view, uint64_t image_version, uint32_t width, uint32_t height);
  void UpdateRampTable();
  bool Record(VkCommandBuffer commands, VkFramebuffer framebuffer, const Image& source, uint32_t width,
              uint32_t height);

  const VulkanContext& context_;
  FrameRing& frames_;
  std::array<Framebuffer, 4> framebuffers_;
  uint32_t next_framebuffer_ = 0;
  Timing timing_;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
  VkShaderModule fragment_shader_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kVariants> pipelines_{};
  std::array<bool, kVariants> pipeline_failed_{};

  // The constants: the game's ramp followed by the single-channel post-processing (see UpdateRampTable), then the
  // per-pixel parameters.
  GammaRamp game_ramp_{};
  std::array<float, kConstantFloats> constants_{};
  ajustes::Posproceso postprocess_;
  uint64_t postprocess_version_ = UINT64_MAX;
  bool grading_ = false;
  bool logged_effects_ = false;
};

}  // namespace nfsmw::renderer
