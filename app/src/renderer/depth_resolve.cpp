#include "depth_resolve.h"

#include "render_targets.h"

#include <rex/logging.h>

#include <cstring>

namespace nfsmw::renderer {

namespace shaders {
#include "shaders/depth_resolve_fs.h"
#include "shaders/depth_resolve_vs.h"
}  // namespace shaders

DepthResolver::~DepthResolver() {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  for (auto& [key, framebuffer] : framebuffers_) fn.vkDestroyFramebuffer(device, framebuffer, nullptr);
  if (pipeline_) fn.vkDestroyPipeline(device, pipeline_, nullptr);
  if (fragment_shader_) fn.vkDestroyShaderModule(device, fragment_shader_, nullptr);
  if (vertex_shader_) fn.vkDestroyShaderModule(device, vertex_shader_, nullptr);
  if (render_pass_) fn.vkDestroyRenderPass(device, render_pass_, nullptr);
  if (layout_) fn.vkDestroyPipelineLayout(device, layout_, nullptr);
  if (set_layout_) fn.vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
  if (sampler_) fn.vkDestroySampler(device, sampler_, nullptr);
}

bool DepthResolver::Initialize() {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (fn.vkCreateSampler(device, &sampler, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }
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

  // The destination keeps what is outside the rectangle (LOAD) and stays in GENERAL, like every image here.
  VkAttachmentDescription attachment{};
  attachment.format = context_.depth_format();
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
  const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.pDepthStencilAttachment = &reference;
  VkRenderPassCreateInfo render_pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  render_pass.attachmentCount = 1;
  render_pass.pAttachments = &attachment;
  render_pass.subpassCount = 1;
  render_pass.pSubpasses = &subpass;
  if (fn.vkCreateRenderPass(device, &render_pass, nullptr, &render_pass_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module.codeSize = sizeof(shaders::depth_resolve_vs);
  module.pCode = shaders::depth_resolve_vs;
  if (fn.vkCreateShaderModule(device, &module, nullptr, &vertex_shader_) != VK_SUCCESS) {
    return false;
  }
  module.codeSize = sizeof(shaders::depth_resolve_fs);
  module.pCode = shaders::depth_resolve_fs;
  if (fn.vkCreateShaderModule(device, &module, nullptr, &fragment_shader_) != VK_SUCCESS) {
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_shader_;
  stages[0].pName = "main";
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment_shader_;
  stages[1].pName = "main";
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
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
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
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout_;
  info.renderPass = render_pass_;
  info.basePipelineIndex = -1;
  return fn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_) == VK_SUCCESS;
}

bool DepthResolver::Resolve(VkCommandBuffer commands, const Image& source, const Image& destination, int32_t source_x,
                            int32_t source_y, int32_t destination_x, int32_t destination_y, uint32_t width,
                            uint32_t height) {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  const VkDescriptorSet set = frames_.AllocateDescriptorSet(set_layout_);
  FrameRing::Allocation constants;
  if (!set || !frames_.Allocate(16, context_.uniform_alignment(), constants)) {
    return false;
  }
  const int32_t offset[4] = {source_x - destination_x, source_y - destination_y, 0, 0};
  std::memcpy(constants.data, offset, sizeof(offset));
  const VkDescriptorImageInfo image{VK_NULL_HANDLE, source.sample_view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo buffer{constants.buffer, 0, sizeof(offset)};
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
  fn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

  // Imageless: one framebuffer per destination size, kept. Otherwise one for this resolve (one or two per frame),
  // released with the frame.
  const bool imageless = context_.imageless_framebuffer();
  const uint64_t key = (uint64_t(destination.width) << 48) | (uint64_t(destination.height) << 32) | destination.usage;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (auto it = framebuffers_.find(key); imageless && it != framebuffers_.end()) {
    framebuffer = it->second;
  } else {
    const VkFormat format = destination.format;
    VkFramebufferAttachmentImageInfo image_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO};
    image_info.usage = destination.usage;
    image_info.width = destination.width;
    image_info.height = destination.height;
    image_info.layerCount = 1;
    image_info.viewFormatCount = 1;
    image_info.pViewFormats = &format;
    VkFramebufferAttachmentsCreateInfo attachments_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO};
    attachments_info.attachmentImageInfoCount = 1;
    attachments_info.pAttachmentImageInfos = &image_info;
    VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.attachmentCount = 1;
    if (imageless) {
      framebuffer_info.pNext = &attachments_info;
      framebuffer_info.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
    } else {
      framebuffer_info.pAttachments = &destination.view;
    }
    framebuffer_info.width = destination.width;
    framebuffer_info.height = destination.height;
    framebuffer_info.layers = 1;
    if (fn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) {
      return false;
    }
    if (imageless) {
      framebuffers_.emplace(key, framebuffer);
    } else {
      frames_.Defer([&fn, device, framebuffer] { fn.vkDestroyFramebuffer(device, framebuffer, nullptr); });
    }
  }

  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
  attachments.attachmentCount = 1;
  attachments.pAttachments = &destination.view;
  if (imageless) {
    begin.pNext = &attachments;
  }
  begin.renderPass = render_pass_;
  begin.framebuffer = framebuffer;
  begin.renderArea = {{destination_x, destination_y}, {width, height}};
  fn.vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport{0.0f, 0.0f, float(destination.width), float(destination.height), 0.0f, 1.0f};
  fn.vkCmdSetViewport(commands, 0, 1, &viewport);
  fn.vkCmdSetScissor(commands, 0, 1, &begin.renderArea);
  fn.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  const uint32_t dynamic_offset = uint32_t(constants.offset);
  fn.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 1, &dynamic_offset);
  fn.vkCmdDraw(commands, 3, 1, 0, 0);
  fn.vkCmdEndRenderPass(commands);
  return true;
}

}  // namespace nfsmw::renderer
