#include "pipelines.h"

#include "render_targets.h"

#include "shader_modules.h"

#include "../nfsmw_nativo_shaders.h"

#include <rex/logging.h>

#include <chrono>
#include <fstream>

#include <cstring>

#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nfsmw::renderer {

namespace {

// xenos::BlendFactor -> VkBlendFactor. Values 2 and 3 are unused by the hardware.
constexpr VkBlendFactor kBlendFactors[32] = {
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_SRC_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    VK_BLEND_FACTOR_DST_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_CONSTANT_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    VK_BLEND_FACTOR_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
};

// xenos::BlendOp -> VkBlendOp.
VkBlendOp BlendOp(uint32_t op) {
  switch (op) {
    case 1:
      return VK_BLEND_OP_SUBTRACT;
    case 2:
      return VK_BLEND_OP_MIN;
    case 3:
      return VK_BLEND_OP_MAX;
    case 4:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:
      return VK_BLEND_OP_ADD;
  }
}

constexpr uint32_t kPassThroughBlend = 0x00010001u;  // 1 * source + 0 * destination, add, for color and alpha

bool BlendsColor(uint32_t blend) { return !((blend & 0x1F) == 1 && ((blend >> 8) & 0x1F) == 0 && ((blend >> 5) & 0x7) == 0); }
bool BlendsAlpha(uint32_t blend) {
  return !(((blend >> 16) & 0x1F) == 1 && ((blend >> 24) & 0x1F) == 0 && ((blend >> 21) & 0x7) == 0);
}

}  // namespace

static_assert(sizeof(PipelineKey) == 72, "PipelineKey must have no implicit padding");

bool PipelineKey::operator==(const PipelineKey& other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }

size_t Pipelines::KeyHash::operator()(const PipelineKey& key) const { return size_t(XXH3_64bits(&key, sizeof(key))); }

Pipelines::~Pipelines() {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();
  for (auto& [key, pipeline] : pipelines_) {
    if (pipeline) fn.vkDestroyPipeline(device, pipeline, nullptr);
  }
  for (auto& [key, framebuffer] : framebuffers_) fn.vkDestroyFramebuffer(device, framebuffer, nullptr);
  for (auto& [key, pass] : render_passes_) fn.vkDestroyRenderPass(device, pass, nullptr);
  if (cache_) context_.extra().vkDestroyPipelineCache(device, cache_, nullptr);
  if (layout_) fn.vkDestroyPipelineLayout(device, layout_, nullptr);
  if (uniform_set_layout_) fn.vkDestroyDescriptorSetLayout(device, uniform_set_layout_, nullptr);
  if (texture_set_layout_) fn.vkDestroyDescriptorSetLayout(device, texture_set_layout_, nullptr);
}

bool Pipelines::Initialize() {
  const auto& fn = context_.fn();
  const VkDevice device = context_.handle();

  std::array<VkDescriptorSetLayoutBinding, kTextureBindings> textures{};
  for (uint32_t i = 0; i < kTextureBindings; ++i) {
    textures[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  }
  VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = kTextureBindings;
  info.pBindings = textures.data();
  if (fn.vkCreateDescriptorSetLayout(device, &info, nullptr, &texture_set_layout_) != VK_SUCCESS) {
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 3> uniforms{};
  for (uint32_t i = 0; i < 3; ++i) {
    uniforms[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  }
  info.bindingCount = 3;
  info.pBindings = uniforms.data();
  if (fn.vkCreateDescriptorSetLayout(device, &info, nullptr, &uniform_set_layout_) != VK_SUCCESS) {
    return false;
  }
  const VkDescriptorSetLayout sets[2] = {texture_set_layout_, uniform_set_layout_};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = sets;
  return fn.vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_) == VK_SUCCESS;
}

VkRenderPass Pipelines::RenderPass(const PassFormats& formats, VkSampleCountFlagBits samples, uint32_t clear_mask,
                                   uint32_t discard_mask) {
  std::lock_guard<std::mutex> lock(render_pass_mutex_);
  const uint64_t key = XXH3_64bits_withSeed(formats.data(), sizeof(formats),
                                            uint64_t(samples) | (uint64_t(clear_mask) << 8) |
                                                (uint64_t(discard_mask) << 16));
  if (auto it = render_passes_.find(key); it != render_passes_.end()) {
    return it->second;
  }
  std::vector<VkAttachmentDescription> attachments;
  std::array<VkAttachmentReference, kMaxColorTargets> color_refs{};
  VkAttachmentReference depth_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i <= kMaxColorTargets; ++i) {
    const bool is_depth = i == kMaxColorTargets;
    if (!is_depth) {
      color_refs[i] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
    }
    if (formats[i] == VK_FORMAT_UNDEFINED) {
      continue;
    }
    VkAttachmentDescription attachment{};
    attachment.format = formats[i];
    attachment.samples = samples;
    const bool clear = (clear_mask >> i) & 0x1;
    const bool discard = (discard_mask >> i) & 0x1;
    attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = discard ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = !is_depth ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : attachment.loadOp;
    attachment.stencilStoreOp = !is_depth ? VK_ATTACHMENT_STORE_OP_DONT_CARE : attachment.storeOp;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    const uint32_t index = uint32_t(attachments.size());
    attachments.push_back(attachment);
    if (is_depth) {
      depth_ref.attachment = index;
    } else {
      color_refs[i].attachment = index;
      color_count = i + 1;
    }
  }
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = color_count;
  subpass.pColorAttachments = color_refs.data();
  subpass.pDepthStencilAttachment = depth_ref.attachment != VK_ATTACHMENT_UNUSED ? &depth_ref : nullptr;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = uint32_t(attachments.size());
  info.pAttachments = attachments.data();
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  VkRenderPass pass = VK_NULL_HANDLE;
  if (context_.fn().vkCreateRenderPass(context_.handle(), &info, nullptr, &pass) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(key, pass);
  return pass;
}

VkFramebuffer Pipelines::Framebuffer(VkRenderPass pass, const std::array<const Image*, kMaxColorTargets + 1>& targets,
                                     uint32_t width, uint32_t height) {
  const bool imageless = context_.imageless_framebuffer();
  // Imageless: what each attachment is like; otherwise its view.
  struct Attachment {
    uint64_t view;
    VkImageUsageFlags usage;
    VkFormat format;
    uint32_t width, height;
  };
  struct {
    VkRenderPass pass;
    std::array<Attachment, kMaxColorTargets + 1> attachments;
    uint32_t width, height;
  } key_data{};
  key_data.pass = pass;
  key_data.width = width;
  key_data.height = height;
  for (uint32_t i = 0; i <= kMaxColorTargets; ++i) {
    if (const Image* target = targets[i]) {
      key_data.attachments[i] = imageless ? Attachment{0, target->usage, target->format, target->width, target->height}
                                          : Attachment{uint64_t(target->view), 0, VK_FORMAT_UNDEFINED, 0, 0};
    }
  }
  const uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));
  if (auto it = framebuffers_.find(key); it != framebuffers_.end()) {
    return it->second;
  }
  uint32_t attachment_count = 0;
  std::array<VkImageView, kMaxColorTargets + 1> views{};
  std::array<VkFramebufferAttachmentImageInfo, kMaxColorTargets + 1> image_infos{};
  for (const Image* target : targets) {
    if (!target) {
      continue;
    }
    views[attachment_count] = target->view;
    VkFramebufferAttachmentImageInfo& image_info = image_infos[attachment_count];
    image_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO};
    image_info.usage = target->usage;
    image_info.width = target->width;
    image_info.height = target->height;
    image_info.layerCount = 1;
    image_info.viewFormatCount = 1;
    image_info.pViewFormats = &target->format;
    ++attachment_count;
  }
  VkFramebufferAttachmentsCreateInfo attachments_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO};
  attachments_info.attachmentImageInfoCount = attachment_count;
  attachments_info.pAttachmentImageInfos = image_infos.data();
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = pass;
  info.attachmentCount = attachment_count;
  if (imageless) {
    info.pNext = &attachments_info;
    info.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
  } else {
    info.pAttachments = views.data();
  }
  info.width = width;
  info.height = height;
  info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (context_.fn().vkCreateFramebuffer(context_.handle(), &info, nullptr, &framebuffer) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  framebuffers_.emplace(key, framebuffer);
  return framebuffer;
}

uint32_t Pipelines::CanonicalDepthStencil(uint32_t d, bool has_depth) {
  if (!has_depth) {
    return 0;
  }
  d &= ~0x8u;  // bit 3 is not read
  if (!((d >> 1) & 0x1)) {
    d &= ~0x74u;  // without the depth test, its write and function do not matter
  }
  if (!(d & 0x1)) {
    d &= 0x77u;  // without stencil, none of its functions or operations matter
  } else if (!((d >> 7) & 0x1)) {
    d &= 0x000FFFF7u;  // without its own back face state, the back face copies the front
  }
  return d;
}

uint32_t Pipelines::CanonicalBlend(uint32_t blend, uint32_t write_mask) {
  blend &= 0x1FFF1FFFu;
  const bool writes_color = (write_mask & 0x7) != 0;
  const bool writes_alpha = (write_mask & 0x8) != 0;
  if (!((writes_color && BlendsColor(blend)) || (writes_alpha && BlendsAlpha(blend)))) {
    return kPassThroughBlend;  // blending off: the equation is ignored
  }
  if (!writes_color) {
    blend = (blend & 0xFFFF0000u) | 0x00000001u;
  }
  if (!writes_alpha) {
    blend = (blend & 0x0000FFFFu) | 0x00010000u;
  }
  return blend;
}

VkPipeline Pipelines::Get(const PipelineKey& key, const nativo::EntradaShader& vertex_shader,
                          const nativo::EntradaShader* pixel_shader, const VertexInput& input, VkRenderPass pass,
                          const PassFormats& formats) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = pipelines_.find(key); it != pipelines_.end()) {
      return it->second;
    }
  }
  const auto start = std::chrono::steady_clock::now();
  const VkPipeline pipeline = Create(key, vertex_shader, pixel_shader, input, pass, formats);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.created;
    stats_.create_ns += ns;
  }
  return Insert(key, pipeline, vertex_shader, pixel_shader, input, formats);
}

VkPipeline Pipelines::Insert(const PipelineKey& key, VkPipeline pipeline, const nativo::EntradaShader& vertex_shader,
                             const nativo::EntradaShader* pixel_shader, const VertexInput& input,
                             const PassFormats& formats) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Failures are kept too, so they are not retried every draw.
  auto [it, inserted] = pipelines_.emplace(key, pipeline);
  if (!inserted) {
    if (pipeline) {
      context_.fn().vkDestroyPipeline(context_.handle(), pipeline, nullptr);
    }
    return it->second;
  }
  if (pipeline) {
    Recipe recipe;
    recipe.key = key;
    recipe.vertex_shader = vertex_shader.numero;
    recipe.vertex_shader_hash = vertex_shader.huella;
    if (pixel_shader) {
      recipe.pixel_shader = pixel_shader->numero;
      recipe.pixel_shader_hash = pixel_shader->huella;
    }
    recipe.formats = formats;
    recipe.attributes = input.attributes;
    recipe.bindings = input.bindings;
    recipes_.push_back(std::move(recipe));
    dirty_ = true;
  }
  return pipeline;
}

Pipelines::CreationStats Pipelines::TakeStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(stats_, CreationStats{});
}

namespace {

constexpr char kCacheMagic[8] = {'N', 'F', 'S', 'M', 'W', 'V', '1', '1'};
constexpr uint32_t kCacheVersion = 2;  // 2: the key carries the sample count

// Bounds-checked reading of the cache file.
struct Reader {
  const std::vector<char>& data;
  size_t position = 0;
  bool ok = true;

  template <typename T>
  T Read() {
    T value{};
    if (position + sizeof(T) > data.size()) {
      ok = false;
      return value;
    }
    std::memcpy(&value, data.data() + position, sizeof(T));
    position += sizeof(T);
    return value;
  }
};

template <typename T>
void Write(std::vector<char>& out, const T& value) {
  const char* bytes = reinterpret_cast<const char*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

}  // namespace

void Pipelines::LoadCacheFile(const std::filesystem::path& path) {
  cache_path_ = path;
  std::vector<char> data;
  {
    std::ifstream file(path, std::ios::binary);
    if (file) {
      data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
  }
  std::vector<char> driver_cache;
  Reader reader{data};
  char magic[8] = {};
  for (char& c : magic) {
    c = reader.Read<char>();
  }
  if (reader.ok && std::memcmp(magic, kCacheMagic, sizeof(magic)) == 0 && reader.Read<uint32_t>() == kCacheVersion) {
    const uint32_t count = reader.Read<uint32_t>();
    for (uint32_t i = 0; i < count && reader.ok; ++i) {
      Recipe recipe;
      recipe.key = reader.Read<PipelineKey>();
      recipe.vertex_shader = reader.Read<uint32_t>();
      recipe.pixel_shader = reader.Read<uint32_t>();
      recipe.vertex_shader_hash = reader.Read<uint64_t>();
      recipe.pixel_shader_hash = reader.Read<uint64_t>();
      recipe.formats = reader.Read<PassFormats>();
      const uint32_t attributes = reader.Read<uint32_t>();
      for (uint32_t a = 0; a < attributes && a < 16 && reader.ok; ++a) {
        recipe.attributes.push_back(reader.Read<VertexAttribute>());
      }
      const uint32_t bindings = reader.Read<uint32_t>();
      for (uint32_t b = 0; b < bindings && b < 16 && reader.ok; ++b) {
        recipe.bindings.push_back(reader.Read<VertexBinding>());
      }
      if (reader.ok && attributes <= 16 && bindings <= 16) {
        loaded_recipes_.push_back(std::move(recipe));
      }
    }
    const uint64_t driver_bytes = reader.Read<uint64_t>();
    if (reader.ok && driver_bytes <= data.size() - reader.position) {
      driver_cache.assign(data.begin() + reader.position, data.begin() + reader.position + driver_bytes);
    } else {
      loaded_recipes_.clear();  // truncated file
    }
  }
  // The driver checks the data's header (device, driver version) and ignores data that is not its own.
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  info.initialDataSize = driver_cache.size();
  info.pInitialData = driver_cache.empty() ? nullptr : driver_cache.data();
  if (context_.extra().vkCreatePipelineCache(context_.handle(), &info, nullptr, &cache_) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (context_.extra().vkCreatePipelineCache(context_.handle(), &info, nullptr, &cache_) != VK_SUCCESS) {
      cache_ = VK_NULL_HANDLE;
    }
  }
  REXLOG_INFO("[renderer] pipeline cache: {} pipelines to warm up, {} KB of driver cache", loaded_recipes_.size(),
              driver_cache.size() / 1024);
}

bool Pipelines::SaveCacheFile() {
  if (cache_path_.empty() || !cache_) {
    return false;
  }
  std::vector<Recipe> recipes;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dirty_) {
      return false;
    }
    recipes = recipes_;
    dirty_ = false;
  }
  size_t driver_bytes = 0;
  std::vector<char> driver_cache;
  if (context_.extra().vkGetPipelineCacheData(context_.handle(), cache_, &driver_bytes, nullptr) == VK_SUCCESS) {
    driver_cache.resize(driver_bytes);
    if (context_.extra().vkGetPipelineCacheData(context_.handle(), cache_, &driver_bytes, driver_cache.data()) !=
        VK_SUCCESS) {
      driver_bytes = 0;
    }
    driver_cache.resize(driver_bytes);
  }
  std::vector<char> out;
  out.insert(out.end(), kCacheMagic, kCacheMagic + sizeof(kCacheMagic));
  Write(out, kCacheVersion);
  Write(out, uint32_t(recipes.size()));
  for (const Recipe& recipe : recipes) {
    PipelineKey key = recipe.key;
    key.vertex_shader = key.pixel_shader = key.render_pass = 0;  // addresses of this run: rebuilt when loading
    Write(out, key);
    Write(out, recipe.vertex_shader);
    Write(out, recipe.pixel_shader);
    Write(out, recipe.vertex_shader_hash);
    Write(out, recipe.pixel_shader_hash);
    Write(out, recipe.formats);
    Write(out, uint32_t(recipe.attributes.size()));
    for (const VertexAttribute& attribute : recipe.attributes) {
      Write(out, attribute);
    }
    Write(out, uint32_t(recipe.bindings.size()));
    for (const VertexBinding& binding : recipe.bindings) {
      Write(out, binding);
    }
  }
  Write(out, uint64_t(driver_cache.size()));
  out.insert(out.end(), driver_cache.begin(), driver_cache.end());

  // Written next to it and renamed, so a crash never leaves half a file.
  std::error_code error;
  std::filesystem::create_directories(cache_path_.parent_path(), error);
  std::filesystem::path temporary = cache_path_;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(out.data(), std::streamsize(out.size()))) {
      return false;
    }
  }
  std::filesystem::rename(temporary, cache_path_, error);
  return !error;
}

uint32_t Pipelines::WarmUp(const nativo::ShadersNativos& library, const std::atomic<bool>& stop) {
  uint32_t created = 0;
  for (const Recipe& recipe : loaded_recipes_) {
    if (stop.load(std::memory_order_relaxed)) {
      break;
    }
    const nativo::EntradaShader* vs = library.PorNumero(recipe.vertex_shader);
    const nativo::EntradaShader* ps =
        recipe.pixel_shader != UINT32_MAX ? library.PorNumero(recipe.pixel_shader) : nullptr;
    if (!vs || vs->huella != recipe.vertex_shader_hash ||
        (recipe.pixel_shader != UINT32_MAX && (!ps || ps->huella != recipe.pixel_shader_hash))) {
      continue;  // another shader library: this entry no longer applies
    }
    VertexInput input;
    input.attributes = recipe.attributes;
    input.bindings = recipe.bindings;
    input.hash = recipe.key.vertex_input;
    const VkRenderPass pass = RenderPass(recipe.formats, VkSampleCountFlagBits(recipe.key.samples));
    if (!pass) {
      continue;
    }
    PipelineKey key = recipe.key;
    key.vertex_shader = uint64_t(uintptr_t(vs));
    key.pixel_shader = uint64_t(uintptr_t(ps));
    key.render_pass = uint64_t(uintptr_t(pass));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pipelines_.count(key)) {
        continue;  // the game got there first
      }
    }
    const VkPipeline pipeline = Create(key, *vs, ps, input, pass, recipe.formats);
    if (pipeline) {
      Insert(key, pipeline, *vs, ps, input, recipe.formats);
      ++created;
    }
  }
  return created;
}

VkPipeline Pipelines::Create(const PipelineKey& key, const nativo::EntradaShader& vertex_shader,
                             const nativo::EntradaShader* pixel_shader, const VertexInput& input, VkRenderPass pass,
                             const PassFormats& formats) {
  VkShaderModule vs = shaders_.Get(vertex_shader);
  VkShaderModule ps = pixel_shader ? shaders_.Get(*pixel_shader) : VK_NULL_HANDLE;
  if (!vs || (pixel_shader && !ps)) {
    return VK_NULL_HANDLE;
  }

  const VkSpecializationMapEntry spec_entry{0, 0, sizeof(uint32_t)};
  const VkSpecializationInfo specialization{1, &spec_entry, sizeof(uint32_t), &key.specialization};
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[0].pSpecializationInfo = &specialization;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps;
  stages[1].pName = "main";
  stages[1].pSpecializationInfo = &specialization;

  std::vector<VkVertexInputBindingDescription> bindings;
  for (uint32_t i = 0; i < input.bindings.size(); ++i) {
    bindings.push_back({i, input.bindings[i].stride, VK_VERTEX_INPUT_RATE_VERTEX});
  }
  std::vector<VkVertexInputAttributeDescription> attributes;
  for (const VertexAttribute& a : input.attributes) {
    attributes.push_back({a.location, a.binding, a.format, a.offset});
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = uint32_t(bindings.size());
  vertex_input.pVertexBindingDescriptions = bindings.data();
  vertex_input.vertexAttributeDescriptionCount = uint32_t(attributes.size());
  vertex_input.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VkPrimitiveTopology(key.topology);
  assembly.primitiveRestartEnable = (key.rasterizer & 0x8) ? VK_TRUE : VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = ((key.rasterizer & 0x1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((key.rasterizer & 0x2) ? VK_CULL_MODE_BACK_BIT : 0);
  raster.frontFace = (key.rasterizer & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.depthBiasEnable = (key.rasterizer & 0x10) ? VK_TRUE : VK_FALSE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples);

  // RB_DEPTHCONTROL: xenos::CompareFunction and xenos::StencilOp have the same values as Vulkan's.
  const uint32_t d = key.depth_stencil;
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = (d >> 1) & 0x1;
  depth.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
  depth.depthCompareOp = VkCompareOp((d >> 4) & 0x7);
  depth.stencilTestEnable = d & 0x1;
  depth.front.compareOp = VkCompareOp((d >> 8) & 0x7);
  depth.front.failOp = VkStencilOp((d >> 11) & 0x7);
  depth.front.passOp = VkStencilOp((d >> 14) & 0x7);
  depth.front.depthFailOp = VkStencilOp((d >> 17) & 0x7);
  if ((d >> 7) & 0x1) {
    depth.back.compareOp = VkCompareOp((d >> 20) & 0x7);
    depth.back.failOp = VkStencilOp((d >> 23) & 0x7);
    depth.back.passOp = VkStencilOp((d >> 26) & 0x7);
    depth.back.depthFailOp = VkStencilOp((d >> 29) & 0x7);
  } else {
    depth.back = depth.front;
  }

  std::array<VkPipelineColorBlendAttachmentState, kMaxColorTargets> blends{};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
    if (formats[i] != VK_FORMAT_UNDEFINED) {
      color_count = i + 1;
    }
  }
  for (uint32_t i = 0; i < color_count; ++i) {
    VkPipelineColorBlendAttachmentState& blend = blends[i];
    if (formats[i] == VK_FORMAT_UNDEFINED) {
      continue;  // an unused slot: no writes
    }
    const uint32_t b = key.blend[i];
    blend.colorWriteMask = (key.write_masks >> (i * 4)) & 0xF;
    blend.blendEnable = b != kPassThroughBlend;
    blend.srcColorBlendFactor = kBlendFactors[b & 0x1F];
    blend.colorBlendOp = BlendOp((b >> 5) & 0x7);
    blend.dstColorBlendFactor = kBlendFactors[(b >> 8) & 0x1F];
    blend.srcAlphaBlendFactor = kBlendFactors[(b >> 16) & 0x1F];
    blend.alphaBlendOp = BlendOp((b >> 21) & 0x7);
    blend.dstAlphaBlendFactor = kBlendFactors[(b >> 24) & 0x1F];
  }
  VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend_state.attachmentCount = color_count;
  blend_state.pAttachments = blends.data();

  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,           VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = ps ? 2 : 1;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = formats[kMaxColorTargets] != VK_FORMAT_UNDEFINED ? &depth : nullptr;
  info.pColorBlendState = &blend_state;
  info.pDynamicState = &dynamic;
  info.layout = layout_;
  info.renderPass = pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result =
      context_.fn().vkCreateGraphicsPipelines(context_.handle(), cache_, 1, &info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    REXLOG_ERROR("[renderer] vkCreateGraphicsPipelines failed ({})", int(result));
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

}  // namespace nfsmw::renderer
