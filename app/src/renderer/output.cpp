#include "output.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

REXCVAR_DEFINE_STRING(nfsmw_rcas, "apagado", "Graficos",
                      "Nitidez FSR1 (RCAS de AMD) en la salida del renderizador Vulkan 1.1: apagado, bajo, medio o alto. "
                      "Se aplica en marcha; con FXAA activo no se usa")
    .allowed({"apagado", "bajo", "medio", "alto"});

namespace nfsmw::renderer {

namespace shaders {
#include "shaders/output_fs.h"
#include "shaders/output_vs.h"
}  // namespace shaders

namespace {

using rex::ui::vulkan::VulkanPresenter;

// nfsmw_rcas as RCAS's linear sharpness, exp2(-stops) (0 stops is the strongest; AMD's default is 0.2). 0 = off.
float RcasSharpness() {
  const std::string setting = REXCVAR_GET(nfsmw_rcas);
  const float stops = setting == "alto" ? 0.2f : setting == "medio" ? 0.5f : setting == "bajo" ? 1.0f : -1.0f;
  return stops < 0.0f ? 0.0f : std::exp2(-stops);
}

VkShaderModule CreateModule(const VulkanContext& context, const uint32_t* code, size_t size) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = size;
  info.pCode = code;
  VkShaderModule module = VK_NULL_HANDLE;
  context.fn().vkCreateShaderModule(context.handle(), &info, nullptr, &module);
  return module;
}

}  // namespace

Output::~Output() {
  frames_.WaitIdle();
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  for (Framebuffer& f : framebuffers_) {
    if (f.framebuffer) fn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
  }
  for (VkPipeline pipeline : pipelines_) {
    if (pipeline) fn.vkDestroyPipeline(device, pipeline, nullptr);
  }
  if (fragment_shader_) fn.vkDestroyShaderModule(device, fragment_shader_, nullptr);
  if (vertex_shader_) fn.vkDestroyShaderModule(device, vertex_shader_, nullptr);
  if (render_pass_) fn.vkDestroyRenderPass(device, render_pass_, nullptr);
  if (layout_) fn.vkDestroyPipelineLayout(device, layout_, nullptr);
  if (set_layout_) fn.vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
  if (sampler_) fn.vkDestroySampler(device, sampler_, nullptr);
}

bool Output::Initialize() {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();

  // Linear, for FXAA's in-between taps; the other variants read whole texels.
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (fn.vkCreateSampler(device, &sampler, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }

  // The front buffer (0) and the constants (1, a dynamic uniform buffer in the frame's upload memory).
  const VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_},
      {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout.bindingCount = 2;
  set_layout.pBindings = bindings;
  if (fn.vkCreateDescriptorSetLayout(device, &set_layout, nullptr, &set_layout_) != VK_SUCCESS) {
    return false;
  }
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &set_layout_;
  if (fn.vkCreatePipelineLayout(device, &layout, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkAttachmentDescription attachment{};
  attachment.format = VulkanPresenter::kGuestOutputFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
  const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkRenderPassCreateInfo render_pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  render_pass.attachmentCount = 1;
  render_pass.pAttachments = &attachment;
  render_pass.subpassCount = 1;
  render_pass.pSubpasses = &subpass;
  if (fn.vkCreateRenderPass(device, &render_pass, nullptr, &render_pass_) != VK_SUCCESS) {
    return false;
  }

  vertex_shader_ = CreateModule(context_, shaders::output_vs, sizeof(shaders::output_vs));
  fragment_shader_ = CreateModule(context_, shaders::output_fs, sizeof(shaders::output_fs));
  if (!vertex_shader_ || !fragment_shader_) {
    return false;
  }
  // Until the game loads its ramp: identity.
  for (uint32_t i = 0; i < 256; ++i) {
    const uint16_t value = uint16_t((i * 1023 + 127) / 255);
    game_ramp_[i] = {value, value, value};
  }
  UpdateRampTable();
  // The plain variant now; the others the first time they are needed.
  return Pipeline(0) != VK_NULL_HANDLE;
}

VkPipeline Output::Pipeline(uint32_t variant) {
  VkPipeline& pipeline = pipelines_[variant];
  if (pipeline || pipeline_failed_[variant]) {
    return pipeline;
  }
  const VkSpecializationMapEntry entries[3] = {
      {0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}, {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
  const VkBool32 values[3] = {VkBool32(variant & 1), VkBool32((variant >> 1) & 1), VkBool32((variant >> 2) & 1)};
  const VkSpecializationInfo specialization{3, entries, sizeof(values), values};
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_shader_;
  stages[0].pName = "main";
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment_shader_;
  stages[1].pName = "main";
  stages[1].pSpecializationInfo = &specialization;
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout_;
  info.renderPass = render_pass_;
  info.basePipelineIndex = -1;
  if (context_.fn().vkCreateGraphicsPipelines(context_.handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("[renderer] output pass variant {} could not be created", variant);
  }
  return pipeline;
}

void Output::SetGammaRamp(const GammaRamp& ramp) {
  game_ramp_ = ramp;
  UpdateRampTable();
}

// The table is the game's ramp followed by the post-processing, in the order of GoldenEye-Recomp's ge_grade.cs.hlsl:
// temperature, brightness, contrast, tint, saturation, vibrance and gamma. The single-channel steps are folded into
// the 256 entries; with saturation, vibrance, vignette or scanlines (grading_) the table stops before the gamma,
// unclamped, and the shader does the rest per pixel.
void Output::UpdateRampTable() {
  const ajustes::Posproceso& p = postprocess_;
  grading_ = p.activo && (p.saturacion != 1.0f || p.vibracion != 0.0f || p.vineta > 0.0f || p.lineas > 0.0f);
  const float tint[3] = {p.tinte_r, p.tinte_g, p.tinte_b};
  const float inverse_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
  for (uint32_t i = 0; i < 256; ++i) {
    for (uint32_t c = 0; c < 3; ++c) {
      float x = float(game_ramp_[i][c] & 0x3FF) / 1023.0f;
      if (p.activo) {
        x += c == 0 ? p.temperatura * 0.10f : c == 2 ? -p.temperatura * 0.10f : 0.0f;
        x += p.brillo;
        x = (x - 0.5f) * p.contraste + 0.5f;
        x = x + (x * tint[c] - x) * p.tinte;
        if (!grading_) {
          x = std::clamp(std::pow(std::max(x, 0.0001f), inverse_gamma), 0.0f, 1.0f);
        }
      }
      constants_[i * 4 + c] = x;
    }
    constants_[i * 4 + 3] = 1.0f;
  }
  float* grading = &constants_[256 * 4];
  grading[0] = p.saturacion;
  grading[1] = p.vibracion;
  grading[2] = inverse_gamma;
  grading[3] = 0.0f;
  float* effects = grading + 4;
  effects[0] = p.vineta;
  effects[1] = p.lineas;
}

VkFramebuffer Output::FramebufferFor(VkImageView view, uint64_t image_version, uint32_t width, uint32_t height) {
  for (const Framebuffer& f : framebuffers_) {
    if (f.framebuffer && f.image_version == image_version && f.width == width && f.height == height) {
      return f.framebuffer;
    }
  }
  Framebuffer& f = framebuffers_[next_framebuffer_];
  next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
  if (f.framebuffer) {
    // A frame in flight may still use it.
    frames_.Defer([&fn = context_.fn(), device = context_.handle(), framebuffer = f.framebuffer] {
      fn.vkDestroyFramebuffer(device, framebuffer, nullptr);
    });
    f.framebuffer = VK_NULL_HANDLE;
  }
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = render_pass_;
  info.attachmentCount = 1;
  info.pAttachments = &view;
  info.width = width;
  info.height = height;
  info.layers = 1;
  if (context_.fn().vkCreateFramebuffer(context_.handle(), &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
    f.framebuffer = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  f.image_version = image_version;
  f.width = width;
  f.height = height;
  return f.framebuffer;
}

bool Output::Record(VkCommandBuffer commands, VkFramebuffer framebuffer, const Image& source, uint32_t width,
                    uint32_t height) {
  const auto& fn = context_.fn();
  const bool fxaa = ajustes::AntialiasingFxaa();
  const float sharpness = RcasSharpness();
  const bool rcas = sharpness > 0.0f && !fxaa;
  if (!logged_effects_ || fxaa != bool(constants_[256 * 4 + 7]) || sharpness != constants_[256 * 4 + 6]) {
    REXLOG_INFO("[renderer] output: FXAA {}, RCAS {}", fxaa ? "on" : "off",
                rcas ? std::to_string(sharpness) : std::string(sharpness > 0.0f ? "off (FXAA wins)" : "off"));
    logged_effects_ = true;
  }
  // effects.z: RCAS sharpness; effects.w only remembers the FXAA state for the log above (the shader ignores it).
  constants_[256 * 4 + 6] = sharpness;
  constants_[256 * 4 + 7] = fxaa ? 1.0f : 0.0f;
  constants_[256 * 4 + 8] = 1.0f / float(source.width);
  constants_[256 * 4 + 9] = 1.0f / float(source.height);

  FrameRing::Allocation allocation;
  const VkDescriptorSet set = frames_.AllocateDescriptorSet(set_layout_);
  if (!set || !frames_.Allocate(sizeof(constants_), context_.uniform_alignment(), allocation)) {
    return false;
  }
  std::memcpy(allocation.data, constants_.data(), sizeof(constants_));
  const VkDescriptorImageInfo image{VK_NULL_HANDLE, source.sample_view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo buffer{allocation.buffer, 0, sizeof(constants_)};
  VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  writes[0].dstSet = set;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &image;
  writes[1].dstSet = set;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[1].pBufferInfo = &buffer;
  fn.vkUpdateDescriptorSets(context_.handle(), 2, writes, 0, nullptr);

  // The front buffer was written earlier in this same command buffer.
  RenderTargets::Barrier(context_, commands);
  VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = render_pass_;
  pass.framebuffer = framebuffer;
  pass.renderArea.extent = {width, height};
  fn.vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  fn.vkCmdSetViewport(commands, 0, 1, &viewport);
  const VkRect2D scissor{{0, 0}, {width, height}};
  fn.vkCmdSetScissor(commands, 0, 1, &scissor);
  VkPipeline pipeline = Pipeline((grading_ ? 1 : 0) | (fxaa ? 2 : 0) | (rcas ? 4 : 0));
  if (!pipeline) {
    pipeline = pipelines_[0];
  }
  fn.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const uint32_t offset = uint32_t(allocation.offset);
  fn.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 1, &offset);
  fn.vkCmdDraw(commands, 3, 1, 0, 0);
  fn.vkCmdEndRenderPass(commands);
  return true;
}

bool Output::Present(rex::ui::Presenter* presenter, const Image& source, uint32_t width, uint32_t height) {
  if (!presenter || !source.sample_view || source.depth) {
    return false;
  }
  width = std::min(width ? width : source.width, source.width);
  height = std::min(height ? height : source.height, source.height);
  // Post-processing settings changed in the menu (an atomic counter; the cvars are only read then).
  const uint64_t postprocess_version = ajustes::VersionPosproceso();
  if (postprocess_version != postprocess_version_) {
    postprocess_version_ = postprocess_version;
    postprocess_ = ajustes::LeerPosproceso();
    UpdateRampTable();
  }
  using Clock = std::chrono::steady_clock;
  const auto ns = [](Clock::time_point from, Clock::time_point to) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
  };
  const auto start = Clock::now();
  auto mark = start;
  bool drawn = false;
  presenter->RefreshGuestOutput(width, height, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base) {
    const auto entered = Clock::now();
    timing_.before_ns += ns(mark, entered);
    auto& refresh = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base);
    const VkCommandBuffer commands = frames_.Commands();
    const VkFramebuffer framebuffer = FramebufferFor(refresh.image_view(), refresh.image_version(), width, height);
    const bool recorded = commands && framebuffer && Record(commands, framebuffer, source, width, height);
    const auto record_end = Clock::now();
    timing_.record_ns += ns(entered, record_end);
    mark = record_end;
    if (!recorded) {
      return false;
    }
    // The frame's work and its output in one submission, before the presenter draws the output image.
    drawn = frames_.Submit();
    mark = Clock::now();
    timing_.submit_ns += ns(record_end, mark);
    return drawn;
  });
  // Without the callback (no output asked for) all of it counts as before.
  (mark == start ? timing_.before_ns : timing_.after_ns) += ns(mark, Clock::now());
  return drawn;
}

}  // namespace nfsmw::renderer
