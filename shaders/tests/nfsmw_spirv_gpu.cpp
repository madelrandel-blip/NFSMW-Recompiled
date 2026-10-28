// GPU comparison of two SPIR-V modules of the same shader (see nfsmw_spirv_gpu.h).
//
// Both modules get exactly the same inputs: random constants (with zeros and large values mixed in, so the
// infinity and NaN paths run), random textures in the three descriptor heaps, random interpolators or vertex
// inputs, and the same specialization constant. The constants are read through the set 4 UBOs or, with the UBO
// bit clear, through the buffer device addresses in the push constants, so both paths are covered.
//
//   Pixel shaders: a full-screen triangle into 32x32 RGBA32F targets (one per color output), cleared to a sentinel
//   so a killed pixel shows as such. Specializations: plain, 1/size from constants, alpha test with each of the
//   eight functions, cheap PCF, shadow minimum, no blur, and the pointer path.
//   Vertex shaders: 96 vertices with rasterization discarded; transform feedback captures SV_Position and the 22
//   interpolators. The capture decorations are added to both modules the same way.

#include "nfsmw_spirv_gpu.h"

#include "shader.h"
#include "spirv_recompiler.h"

#include <SpvBuilder.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>

namespace
{

constexpr uint32_t kTargetSize = 32;
constexpr uint32_t kVertexCount = 96;
constexpr uint32_t kInterpolators = 22;
constexpr uint32_t kCaptureStride = (kInterpolators + 1) * 16;
constexpr uint32_t kTexturesPerHeap = 4;
constexpr uint32_t kSamplers = 2;
constexpr float kSentinel = -12345.0f;

constexpr uint32_t kSpecUbo = 1u << 8;
constexpr uint32_t kSpecInvSize = 1u << 9;
constexpr uint32_t kSpecAlphaTest = 1u << 1;
constexpr uint32_t kSpecCheapPcf = 1u << 15;
constexpr uint32_t kSpecNoBlur = 1u << 19;
constexpr uint32_t kSpecShadowMinimum = 1u << 23;

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// Instruction-level edit of a module: transform feedback capture of every stage output.
std::vector<uint32_t> AddCapture(const std::vector<uint32_t>& words)
{
    std::map<uint32_t, uint32_t> location, builtIn, storage;
    uint32_t mainId = 0;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (op == 71 && words[i + 2] == 30)  // OpDecorate Location
            location[words[i + 1]] = words[i + 3];
        if (op == 71 && words[i + 2] == 11)  // OpDecorate BuiltIn
            builtIn[words[i + 1]] = words[i + 3];
        if (op == 59)  // OpVariable
            storage[words[i + 2]] = words[i + 3];
        if (op == 15)  // OpEntryPoint
            mainId = words[i + 2];
        i += count;
    }
    std::vector<uint32_t> decorations;
    for (const auto& [id, storageClass] : storage)
    {
        if (storageClass != 3)  // Output
            continue;
        uint32_t offset;
        if (builtIn.count(id) && builtIn[id] == 0)  // Position
            offset = 0;
        else if (location.count(id))
            offset = (location[id] + 1) * 16;
        else
            continue;
        const uint32_t add[][4] = {{4u << 16 | 71, id, 36, 0},                // XfbBuffer 0
                                   {4u << 16 | 71, id, 37, kCaptureStride},   // XfbStride
                                   {4u << 16 | 71, id, 35, offset}};          // Offset
        for (const auto& instruction : add)
            decorations.insert(decorations.end(), instruction, instruction + 4);
    }

    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    bool capabilityAdded = false, modeAdded = false, decorationsAdded = false;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!capabilityAdded && op != 17)  // after the OpCapability block
        {
            out.insert(out.end(), {2u << 16 | 17, 53});  // OpCapability TransformFeedback
            capabilityAdded = true;
        }
        if (!decorationsAdded && (op == 71 || op == 72))  // before the first annotation
        {
            out.insert(out.end(), decorations.begin(), decorations.end());
            decorationsAdded = true;
        }
        out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        if (!modeAdded && op == 15)
        {
            out.insert(out.end(), {3u << 16 | 16, mainId, 11});  // OpExecutionMode main Xfb
            modeAdded = true;
        }
        i += count;
    }
    return out;
}

// The DXC modules declare the shared block as float4[23] but read the 1/size of samplers 1-15 past its end (bytes
// 368-487), which is undefined behavior and on some drivers does not read the buffer. The translator declares 31
// registers, the size the renderer binds. For the comparison the reference gets the same declaration, so both read
// the same defined bytes.
std::vector<uint32_t> WidenSharedBlock(const std::vector<uint32_t>& words)
{
    std::map<uint32_t, uint32_t> uintConstants;  // id -> value
    std::set<uint32_t> sharedArrays;
    uint32_t sharedStruct = 0;
    std::map<uint32_t, std::string> names;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (op == 5)  // OpName
            names[words[i + 1]] = reinterpret_cast<const char*>(&words[i + 2]);
        if (op == 43 && count == 4)
            uintConstants[words[i + 2]] = words[i + 3];
        if (op == 30 && names[words[i + 1]] == "type.ConstantBuffer.NfsmwBloqueCompartidas")  // OpTypeStruct
            sharedStruct = words[i + 1];
        i += count;
    }
    if (!sharedStruct)
        return words;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (op == 30 && words[i + 1] == sharedStruct)
            sharedArrays.insert(words[i + 2]);
        i += count;
    }
    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (op == 28 && sharedArrays.count(words[i + 1]) && uintConstants[words[i + 3]] == 23)  // OpTypeArray
        {
            // A fresh OpConstant 31 of the same type, right before the array that uses it.
            uint32_t lengthType = 0;
            for (size_t j = 5; j < words.size(); j += words[j] >> 16)
            {
                if ((words[j] & 0xFFFF) == 43 && words[j + 2] == words[i + 3])
                    lengthType = words[j + 1];
            }
            const uint32_t length = out[3]++;  // a new id: the module's bound grows by one
            out.insert(out.end(), {4u << 16 | 43, lengthType, length, 31});
            out.insert(out.end(), {words[i], words[i + 1], words[i + 2], length});
        }
        else
        {
            out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        }
        i += count;
    }
    return out;
}

// Adds OpModuleProcessed "<text>" before the first annotation: the module changes its bytes (so no driver shader
// cache knows it) without changing what it computes.
std::vector<uint32_t> AddMarker(const std::vector<uint32_t>& words, const std::string& text)
{
    std::vector<uint32_t> literal((text.size() + 4) / 4, 0);
    std::memcpy(literal.data(), text.data(), text.size());
    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    bool added = false;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!added && (op == 71 || op == 72))
        {
            out.push_back(uint32_t(literal.size() + 1) << 16 | 330);  // OpModuleProcessed
            out.insert(out.end(), literal.begin(), literal.end());
            added = true;
        }
        out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        i += count;
    }
    return out;
}

class Comparer final : public GpuComparer
{
public:
    ~Comparer() override;
    bool Initialize();
    bool Compare(const std::string& name, bool vertex, const std::vector<uint8_t>& container,
                 const std::vector<uint32_t>& reference, const std::vector<uint32_t>& translated,
                 std::string& report) override;
    void MeasureCreation(bool vertex, const std::vector<uint8_t>& container, const std::vector<uint32_t>& reference,
                         const std::vector<uint32_t>& translated) override;
    void CreationTotals(double& referenceMs, double& translatedMs, uint32_t& shaders) const override
    {
        referenceMs = creationMs_[0];
        translatedMs = creationMs_[1];
        shaders = measured_;
    }

private:
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void DestroyBuffer(Buffer& buffer);
    Image CreateImage(VkImageType type, VkImageViewType viewType, VkFormat format, uint32_t width, uint32_t height,
                      uint32_t depth, uint32_t layers, VkImageUsageFlags usage, VkImageCreateFlags flags);
    void DestroyImage(Image& image);
    VkCommandBuffer Begin();
    void Submit(VkCommandBuffer commands);
    void FillRandomResources(uint32_t seed);
    std::vector<uint32_t> HarnessVertexShader();
    VkShaderModule Module(const std::vector<uint32_t>& words);
    bool RunPixel(const std::vector<uint32_t>& module, uint32_t spec, uint32_t targets, std::vector<float>& result,
                  std::string& error, bool v11 = false);
    bool RunVertex(const std::vector<uint32_t>& module, uint32_t spec, const std::vector<std::pair<uint32_t, bool>>& inputs,
                   std::vector<float>& result, std::string& error, bool v11 = false);
    void BindResources(VkCommandBuffer commands, bool v11);
    bool InitializeV11();
    void UpdateV11Resources();

public:
    bool v11_ = false;  // the translated modules use SpirvResourceModel::Vulkan11

private:
    void WritePushConstants(VkCommandBuffer commands);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    PFN_vkCmdBindTransformFeedbackBuffersEXT bindXfb_ = nullptr;
    PFN_vkCmdBeginTransformFeedbackEXT beginXfb_ = nullptr;
    PFN_vkCmdEndTransformFeedbackEXT endXfb_ = nullptr;

    std::array<VkDescriptorSetLayout, 5> setLayouts_{};
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 5> sets_{};
    std::array<std::array<Image, kTexturesPerHeap>, 3> textures_{};
    std::array<VkSampler, kSamplers> samplers_{};
    Buffer vertexConstants_, pixelConstants_, sharedConstants_;
    Buffer vertexData_, capture_, readback_;
    std::vector<uint32_t> harness_;
    // The Vulkan 1.1 model: set 0 with one combined image sampler per register and kind, set 1 with the three UBOs.
    std::array<VkDescriptorSetLayout, 2> setLayoutsV11_{};
    VkPipelineLayout pipelineLayoutV11_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPoolV11_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> setsV11_{};
    Buffer sharedV11_;
    double lastCreationMs_ = 0.0;
    double creationMs_[2] = {0.0, 0.0};
    uint32_t measured_ = 0;
    uint32_t markerCounter_ = 0;
};

Comparer::~Comparer()
{
    if (!device_)
    {
        if (instance_)
            vkDestroyInstance(instance_, nullptr);
        return;
    }
    vkDeviceWaitIdle(device_);
    for (auto& heap : textures_)
        for (auto& image : heap)
            DestroyImage(image);
    for (VkSampler sampler : samplers_)
        vkDestroySampler(device_, sampler, nullptr);
    for (Buffer* buffer : {&vertexConstants_, &pixelConstants_, &sharedConstants_, &vertexData_, &capture_, &readback_})
        DestroyBuffer(*buffer);
    DestroyBuffer(sharedV11_);
    if (descriptorPoolV11_)
        vkDestroyDescriptorPool(device_, descriptorPoolV11_, nullptr);
    if (pipelineLayoutV11_)
        vkDestroyPipelineLayout(device_, pipelineLayoutV11_, nullptr);
    for (VkDescriptorSetLayout layout : setLayoutsV11_)
    {
        if (layout)
            vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    for (VkDescriptorSetLayout layout : setLayouts_)
        vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
}

uint32_t Comparer::MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
    {
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

Buffer Comparer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer buffer;
    buffer.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vkCreateBuffer(device_, &info, nullptr, &buffer.buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &allocate, nullptr, &buffer.memory);
    vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0);
    vkMapMemory(device_, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped);
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(device_, &addressInfo);
    return buffer;
}

void Comparer::DestroyBuffer(Buffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = Buffer{};
}

Image Comparer::CreateImage(VkImageType type, VkImageViewType viewType, VkFormat format, uint32_t width,
                            uint32_t height, uint32_t depth, uint32_t layers, VkImageUsageFlags usage,
                            VkImageCreateFlags flags)
{
    Image image;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = type;
    info.format = format;
    info.extent = {width, height, depth};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    vkCreateImage(device_, &info, nullptr, &image.image);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device_, image.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &allocate, nullptr, &image.memory);
    vkBindImageMemory(device_, image.image, image.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = viewType;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCreateImageView(device_, &view, nullptr, &image.view);
    return image;
}

void Comparer::DestroyImage(Image& image)
{
    if (image.view)
        vkDestroyImageView(device_, image.view, nullptr);
    if (image.image)
        vkDestroyImage(device_, image.image, nullptr);
    if (image.memory)
        vkFreeMemory(device_, image.memory, nullptr);
    image = Image{};
}

VkCommandBuffer Comparer::Begin()
{
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands;
    vkAllocateCommandBuffers(device_, &allocate, &commands);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    return commands;
}

void Comparer::Submit(VkCommandBuffer commands)
{
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &commands);
}

bool Comparer::Initialize()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nfsmw_spirv_check";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS)
        return false;
    uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(instance_, &count, &physical_) < 0 || count == 0)
        return false;
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> familyProperties(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, familyProperties.data());
    for (queueFamily_ = 0; queueFamily_ < families; ++queueFamily_)
    {
        if (familyProperties[queueFamily_].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    }

    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    xfb.transformFeedback = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &xfb};
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features12};
    features.features.shaderInt64 = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features.features.independentBlend = VK_TRUE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* extensions[] = {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = extensions;
    if (vkCreateDevice(physical_, &deviceInfo, nullptr, &device_) != VK_SUCCESS)
        return false;
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    bindXfb_ = reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdBindTransformFeedbackBuffersEXT"));
    beginXfb_ = reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdBeginTransformFeedbackEXT"));
    endXfb_ = reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdEndTransformFeedbackEXT"));

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_);

    // Sets 0-2: 2D, 3D and cube heaps; set 3: samplers; set 4: the three constant blocks. The same layout as the
    // native renderer, with fixed array sizes.
    const VkDescriptorType heapTypes[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                           VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    for (uint32_t set = 0; set < 4; ++set)
    {
        VkDescriptorSetLayoutBinding binding{0, heapTypes[set], set == 3 ? kSamplers : kTexturesPerHeap,
                                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1;
        info.pBindings = &binding;
        vkCreateDescriptorSetLayout(device_, &info, nullptr, &setLayouts_[set]);
    }
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (uint32_t i = 0; i < 3; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 3;
        info.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &info, nullptr, &setLayouts_[4]);
    }
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 5;
    layoutInfo.pSetLayouts = setLayouts_.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_);

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kTexturesPerHeap},
                                          {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplers},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}};
    VkDescriptorPoolCreateInfo poolCreate{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCreate.maxSets = 5;
    poolCreate.poolSizeCount = 3;
    poolCreate.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_, &poolCreate, nullptr, &descriptorPool_);
    VkDescriptorSetAllocateInfo setAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocate.descriptorPool = descriptorPool_;
    setAllocate.descriptorSetCount = 5;
    setAllocate.pSetLayouts = setLayouts_.data();
    vkAllocateDescriptorSets(device_, &setAllocate, sets_.data());

    // Textures: RGBA8, 16x16 2D, 8x8x8 3D, 8x8 cube.
    const VkImageUsageFlags textureUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (uint32_t i = 0; i < kTexturesPerHeap; ++i)
    {
        textures_[0][i] = CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1,
                                      textureUsage, 0);
        textures_[1][i] = CreateImage(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 8, 1,
                                      textureUsage, 0);
        textures_[2][i] = CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 6,
                                      textureUsage, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    }
    for (uint32_t i = 0; i < kSamplers; ++i)
    {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = info.minFilter = i == 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW =
            i == 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        vkCreateSampler(device_, &info, nullptr, &samplers_[i]);
    }

    vertexConstants_ = CreateBuffer(256 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    pixelConstants_ = CreateBuffer(224 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    sharedConstants_ = CreateBuffer(31 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    vertexData_ = CreateBuffer(kVertexCount * 16 * 23, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    capture_ = CreateBuffer(kVertexCount * kCaptureStride, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT);
    readback_ = CreateBuffer(4 * kTargetSize * kTargetSize * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Descriptors (they never change; only the contents do).
    std::vector<VkWriteDescriptorSet> writes;
    std::array<std::array<VkDescriptorImageInfo, kTexturesPerHeap>, 3> imageInfos{};
    for (uint32_t heap = 0; heap < 3; ++heap)
    {
        for (uint32_t i = 0; i < kTexturesPerHeap; ++i)
            imageInfos[heap][i] = {VK_NULL_HANDLE, textures_[heap][i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets_[heap];
        write.descriptorCount = kTexturesPerHeap;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = imageInfos[heap].data();
        writes.push_back(write);
    }
    std::array<VkDescriptorImageInfo, kSamplers> samplerInfos{};
    for (uint32_t i = 0; i < kSamplers; ++i)
        samplerInfos[i] = {samplers_[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets_[3];
        write.descriptorCount = kSamplers;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = samplerInfos.data();
        writes.push_back(write);
    }
    const VkDescriptorBufferInfo bufferInfos[3] = {{vertexConstants_.buffer, 0, VK_WHOLE_SIZE},
                                                   {pixelConstants_.buffer, 0, VK_WHOLE_SIZE},
                                                   {sharedConstants_.buffer, 0, VK_WHOLE_SIZE}};
    for (uint32_t i = 0; i < 3; ++i)
    {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets_[4];
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfos[i];
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);

    harness_ = HarnessVertexShader();
    return !v11_ || InitializeV11();
}

bool Comparer::InitializeV11()
{
    std::vector<VkDescriptorSetLayoutBinding> textures;
    for (uint32_t binding = 0; binding < 64; ++binding)
        textures.push_back({binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = uint32_t(textures.size());
    info.pBindings = textures.data();
    vkCreateDescriptorSetLayout(device_, &info, nullptr, &setLayoutsV11_[0]);
    std::array<VkDescriptorSetLayoutBinding, 3> buffers{};
    for (uint32_t i = 0; i < 3; ++i)
        buffers[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      nullptr};
    info.bindingCount = 3;
    info.pBindings = buffers.data();
    vkCreateDescriptorSetLayout(device_, &info, nullptr, &setLayoutsV11_[1]);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayoutsV11_.data();
    vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayoutV11_);

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPoolV11_);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPoolV11_;
    allocate.descriptorSetCount = 2;
    allocate.pSetLayouts = setLayoutsV11_.data();
    if (vkAllocateDescriptorSets(device_, &allocate, setsV11_.data()) != VK_SUCCESS)
        return false;

    sharedV11_ = CreateBuffer(SharedBlockV11::kSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const VkDescriptorBufferInfo bufferInfos[3] = {{vertexConstants_.buffer, 0, VK_WHOLE_SIZE},
                                                   {pixelConstants_.buffer, 0, VK_WHOLE_SIZE},
                                                   {sharedV11_.buffer, 0, VK_WHOLE_SIZE}};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = setsV11_[1];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes.data(), 0, nullptr);
    return true;
}

// The same data as the descriptor heap model, through the Vulkan 1.1 bindings: the texture and sampler each register
// selects in the heaps go to that register's combined image samplers, and the shared block is repacked.
void Comparer::UpdateV11Resources()
{
    const auto* shared = static_cast<const uint32_t*>(sharedConstants_.mapped);
    std::array<VkDescriptorImageInfo, 64> images{};
    std::array<VkWriteDescriptorSet, 64> writes{};
    for (uint32_t reg = 0; reg < 16; ++reg)
    {
        for (uint32_t kind = 0; kind < 4; ++kind)
        {
            // The shadow pair is a 2D texture whose index lives in the 3D word.
            const uint32_t heap = kind == 3 ? 0 : kind;
            const uint32_t word = kind == 3 ? 1 : kind;
            const uint32_t binding = reg * 4 + kind;
            images[binding] = {samplers_[shared[48 + reg] % kSamplers],
                               textures_[heap][shared[word * 16 + reg] % kTexturesPerHeap].view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[binding].dstSet = setsV11_[0];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &images[binding];
        }
    }
    vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);

    auto* out = static_cast<uint8_t*>(sharedV11_.mapped);
    const auto* in = static_cast<const uint8_t*>(sharedConstants_.mapped);
    std::memset(out, 0, SharedBlockV11::kSize);
    std::memcpy(out + SharedBlockV11::kBooleans, in + 256, 4);
    std::memcpy(out + SharedBlockV11::kSwappedTexcoords, in + 260, 4);
    std::memcpy(out + SharedBlockV11::kHalfPixelOffset, in + 264, 8);
    std::memcpy(out + SharedBlockV11::kAlphaThreshold, in + 272, 4);
    std::memcpy(out + SharedBlockV11::kNdcScale, in + 280, 8);
    std::memcpy(out + SharedBlockV11::kNdcOffset, in + 288, 8);
    std::memcpy(out + SharedBlockV11::kInputRemap, in + 296, 64);
    std::memcpy(out + SharedBlockV11::kInvTextureSize, in + 360, 128);
}

void Comparer::BindResources(VkCommandBuffer commands, bool v11)
{
    if (v11)
    {
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayoutV11_, 0, 2, setsV11_.data(), 0,
                                nullptr);
        return;
    }
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 5, sets_.data(), 0, nullptr);
    WritePushConstants(commands);
}

// Pass-through vertex shader for the pixel shader runs: location 0 is the position, locations 1-22 go to the 22
// interpolators.
std::vector<uint32_t> Comparer::HarnessVertexShader()
{
    spv::Builder b(spv::Spv_1_5, 0, nullptr);
    b.addCapability(spv::CapabilityShader);
    b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    spv::Id float4 = b.makeVectorType(b.makeFloatType(32), 4);
    spv::Function* main = b.makeEntryPoint("main");
    std::vector<spv::Id> interface;
    spv::Id position = b.createVariable(spv::NoPrecision, spv::StorageClassInput, float4);
    b.addDecoration(position, spv::DecorationLocation, 0);
    spv::Id outPosition = b.createVariable(spv::NoPrecision, spv::StorageClassOutput, float4);
    b.addDecoration(outPosition, spv::DecorationBuiltIn, spv::BuiltInPosition);
    b.createStore(b.createLoad(position, spv::NoPrecision), outPosition);
    interface = {position, outPosition};
    for (uint32_t i = 0; i < kInterpolators; ++i)
    {
        spv::Id in = b.createVariable(spv::NoPrecision, spv::StorageClassInput, float4);
        b.addDecoration(in, spv::DecorationLocation, int(i + 1));
        spv::Id out = b.createVariable(spv::NoPrecision, spv::StorageClassOutput, float4);
        b.addDecoration(out, spv::DecorationLocation, int(i));
        b.createStore(b.createLoad(in, spv::NoPrecision), out);
        interface.push_back(in);
        interface.push_back(out);
    }
    b.leaveFunction();
    spv::Instruction* entry = b.addEntryPoint(spv::ExecutionModelVertex, main, "main");
    for (spv::Id id : interface)
        entry->addIdOperand(id);
    std::vector<uint32_t> words;
    b.dump(words);
    return words;
}

VkShaderModule Comparer::Module(const std::vector<uint32_t>& words)
{
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * 4;
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &info, nullptr, &module);
    return module;
}

void Comparer::FillRandomResources(uint32_t seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> value(-2.0f, 2.0f);
    std::uniform_int_distribution<uint32_t> percent(0, 99);
    auto constant = [&] {
        const uint32_t p = percent(random);
        if (p < 10)
            return 0.0f;
        if (p < 13)
            return p & 1 ? 3.0e30f : -3.0e30f;
        if (p < 20)
            return 1.0f;
        return value(random);
    };
    for (Buffer* buffer : {&vertexConstants_, &pixelConstants_})
    {
        auto* floats = static_cast<float*>(buffer->mapped);
        for (VkDeviceSize i = 0; i < buffer->size / 4; ++i)
            floats[i] = constant();
    }

    // Shared block: descriptor indices, then the fixed fields (shader_common.h).
    auto* shared = static_cast<uint32_t*>(sharedConstants_.mapped);
    std::memset(shared, 0, sharedConstants_.size);
    for (uint32_t i = 0; i < 48; ++i)
        shared[i] = random() % kTexturesPerHeap;
    for (uint32_t i = 48; i < 64; ++i)
        shared[i] = random() % kSamplers;
    auto putFloat = [&](uint32_t byteOffset, float v) { std::memcpy(&shared[byteOffset / 4], &v, 4); };
    shared[256 / 4] = random();             // g_Booleans
    shared[260 / 4] = random() & 0xFF;      // g_SwappedTexcoords
    putFloat(264, value(random) * 0.01f);   // g_HalfPixelOffset
    putFloat(268, value(random) * 0.01f);
    putFloat(272, std::abs(value(random)) * 0.5f);  // g_AlphaThreshold
    putFloat(280, 1.0f + value(random) * 0.1f);     // g_NdcScale
    putFloat(284, 1.0f + value(random) * 0.1f);
    putFloat(288, value(random) * 0.1f);            // g_NdcOffset
    putFloat(292, value(random) * 0.1f);
    for (uint32_t i = 0; i < 16; ++i)  // g_InputRemap: identity or a random code
    {
        uint32_t code = 0xFFF;
        if (random() & 1)
        {
            code = 0;
            for (uint32_t c = 0; c < 4; ++c)
            {
                const uint32_t choices[] = {0, 1, 2, 3, 4, 5, 7};
                code |= choices[random() % 7] << (c * 3);
            }
        }
        shared[296 / 4 + i] = code;
    }
    for (uint32_t i = 0; i < 16; ++i)  // 1/size of each sampler slot: the 2D textures are 16x16
    {
        putFloat(360 + i * 8, 1.0f / 16.0f);
        putFloat(364 + i * 8, 1.0f / 16.0f);
    }

    // Textures, through a staging buffer.
    const VkDeviceSize maxBytes = 8 * 8 * 8 * 4 * kTexturesPerHeap * 3;
    Buffer staging = CreateBuffer(maxBytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* bytes = static_cast<uint8_t*>(staging.mapped);
    for (VkDeviceSize i = 0; i < staging.size; ++i)
        bytes[i] = uint8_t(random());
    VkCommandBuffer commands = Begin();
    VkDeviceSize offset = 0;
    for (uint32_t heap = 0; heap < 3; ++heap)
    {
        for (uint32_t i = 0; i < kTexturesPerHeap; ++i)
        {
            const uint32_t layers = heap == 2 ? 6 : 1;
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = textures_[heap][i].image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
            copy.imageExtent = heap == 0 ? VkExtent3D{16, 16, 1} : heap == 1 ? VkExtent3D{8, 8, 8} : VkExtent3D{8, 8, 1};
            vkCmdCopyBufferToImage(commands, staging.buffer, textures_[heap][i].image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            offset += 8 * 8 * 8 * 4;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
        }
    }
    Submit(commands);
    DestroyBuffer(staging);

    // Vertex data: 23 float4 per vertex. For the pixel runs, element 0 is the position of a full-screen
    // triangle; the vertex runs use the same buffer as their inputs.
    auto* vertices = static_cast<float*>(vertexData_.mapped);
    for (uint32_t i = 0; i < kVertexCount * 23 * 4; ++i)
        vertices[i] = value(random);
    const float triangle[3][4] = {{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}};
    for (uint32_t v = 0; v < 3; ++v)
        std::memcpy(&vertices[v * 23 * 4], triangle[v], 16);
    if (v11_)
        UpdateV11Resources();
}

void Comparer::WritePushConstants(VkCommandBuffer commands)
{
    const VkDeviceAddress addresses[3] = {vertexConstants_.address, pixelConstants_.address, sharedConstants_.address};
    vkCmdPushConstants(commands, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(addresses), addresses);
}

bool Comparer::RunPixel(const std::vector<uint32_t>& module, uint32_t spec, uint32_t targets, std::vector<float>& result,
                        std::string& error, bool v11)
{
    std::vector<Image> images(targets);
    std::vector<VkAttachmentDescription> attachments(targets);
    std::vector<VkAttachmentReference> references(targets);
    std::vector<VkImageView> views(targets);
    for (uint32_t i = 0; i < targets; ++i)
    {
        images[i] = CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, kTargetSize,
                                kTargetSize, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
        views[i] = images[i].view;
        attachments[i] = {0, VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                          VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        references[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = targets;
    subpass.pColorAttachments = references.data();
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = targets;
    passInfo.pAttachments = attachments.data();
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    VkRenderPass pass;
    vkCreateRenderPass(device_, &passInfo, nullptr, &pass);
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = pass;
    framebufferInfo.attachmentCount = targets;
    framebufferInfo.pAttachments = views.data();
    framebufferInfo.width = framebufferInfo.height = kTargetSize;
    framebufferInfo.layers = 1;
    VkFramebuffer framebuffer;
    vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer);

    VkShaderModule vs = Module(harness_), ps = Module(module);
    VkSpecializationMapEntry entry{0, 0, 4};
    VkSpecializationInfo specialization{1, &entry, 4, &spec};
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &specialization;

    VkVertexInputBindingDescription binding{0, 23 * 16, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (uint32_t i = 0; i < 23; ++i)
        attributes.push_back({i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16});
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(kTargetSize), float(kTargetSize), 0, 1};
    VkRect2D scissor{{0, 0}, {kTargetSize, kTargetSize}};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> blends(targets);
    for (auto& blend : blends)
        blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = targets;
    blend.pAttachments = blends.data();
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.layout = v11 ? pipelineLayoutV11_ : pipelineLayout_;
    pipelineInfo.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto start = std::chrono::steady_clock::now();
    const VkResult created = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    lastCreationMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bool ok = created == VK_SUCCESS;
    if (!ok)
        error = "vkCreateGraphicsPipelines failed (" + std::to_string(created) + ")";

    if (ok)
    {
        VkCommandBuffer commands = Begin();
        std::vector<VkClearValue> clears(targets);
        for (auto& clear : clears)
            clear.color = {{kSentinel, kSentinel, kSentinel, kSentinel}};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = scissor;
        begin.clearValueCount = targets;
        begin.pClearValues = clears.data();
        vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        BindResources(commands, v11);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(commands, 0, 1, &vertexData_.buffer, &zero);
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(commands);
        for (uint32_t i = 0; i < targets; ++i)
        {
            VkBufferImageCopy copy{};
            copy.bufferOffset = VkDeviceSize(i) * kTargetSize * kTargetSize * 16;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {kTargetSize, kTargetSize, 1};
            vkCmdCopyImageToBuffer(commands, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1,
                                   &copy);
        }
        Submit(commands);
        const float* data = static_cast<const float*>(readback_.mapped);
        result.assign(data, data + size_t(targets) * kTargetSize * kTargetSize * 4);
    }

    if (pipeline)
        vkDestroyPipeline(device_, pipeline, nullptr);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, ps, nullptr);
    vkDestroyFramebuffer(device_, framebuffer, nullptr);
    vkDestroyRenderPass(device_, pass, nullptr);
    for (auto& image : images)
        DestroyImage(image);
    return ok;
}

bool Comparer::RunVertex(const std::vector<uint32_t>& module, uint32_t spec,
                         const std::vector<std::pair<uint32_t, bool>>& inputs, std::vector<float>& result,
                         std::string& error, bool v11)
{
    const std::vector<uint32_t> captured = AddCapture(module);
    VkShaderModule vs = Module(captured);
    VkSpecializationMapEntry entry{0, 0, 4};
    VkSpecializationInfo specialization{1, &entry, 4, &spec};
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vs;
    stage.pName = "main";
    stage.pSpecializationInfo = &specialization;

    // Each input location reads its own slot of the 23 float4 per vertex (BLENDINDICES as small integers).
    VkVertexInputBindingDescription binding{0, 23 * 16, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const auto& [location, isUint] : inputs)
        attributes.push_back({location, 0, isUint ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R32G32B32A32_SFLOAT,
                              location * 16});
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, 1, 1, 0, 1};
    VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.rasterizerDiscardEnable = VK_TRUE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // A render pass with no attachments: rasterization is discarded anyway.
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    VkRenderPass pass;
    vkCreateRenderPass(device_, &passInfo, nullptr, &pass);
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = pass;
    framebufferInfo.width = framebufferInfo.height = 1;
    framebufferInfo.layers = 1;
    VkFramebuffer framebuffer;
    vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer);

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &stage;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.layout = v11 ? pipelineLayoutV11_ : pipelineLayout_;
    pipelineInfo.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto start = std::chrono::steady_clock::now();
    const VkResult created = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    lastCreationMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bool ok = created == VK_SUCCESS;
    if (!ok)
        error = "vkCreateGraphicsPipelines failed (" + std::to_string(created) + ")";

    if (ok)
    {
        std::memset(capture_.mapped, 0xCD, capture_.size);
        VkCommandBuffer commands = Begin();
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = scissor;
        vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        BindResources(commands, v11);
        VkDeviceSize zero = 0, captureSize = capture_.size;
        vkCmdBindVertexBuffers(commands, 0, 1, &vertexData_.buffer, &zero);
        bindXfb_(commands, 0, 1, &capture_.buffer, &zero, &captureSize);
        beginXfb_(commands, 0, 0, nullptr, nullptr);
        vkCmdDraw(commands, kVertexCount, 1, 0, 0);
        endXfb_(commands, 0, 0, nullptr, nullptr);
        vkCmdEndRenderPass(commands);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
        Submit(commands);
        const float* data = static_cast<const float*>(capture_.mapped);
        result.assign(data, data + kVertexCount * kCaptureStride / 4);
    }

    if (pipeline)
        vkDestroyPipeline(device_, pipeline, nullptr);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyFramebuffer(device_, framebuffer, nullptr);
    vkDestroyRenderPass(device_, pass, nullptr);
    return ok;
}

// Compares two runs value by value. Equal bits pass; so do two NaNs, and values within a few ULPs (the two
// compilers may order a sum differently). Returns the number of mismatches and describes the worst one.
uint32_t CompareValues(const std::vector<float>& a, const std::vector<float>& b, std::string& worst)
{
    uint32_t mismatches = 0;
    double worstError = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        const float x = a[i], y = b[i];
        if (std::memcmp(&x, &y, 4) == 0 || (std::isnan(x) && std::isnan(y)))
            continue;
        const double scale = std::max({1.0, std::abs(double(x)), std::abs(double(y))});
        const double error = std::abs(double(x) - double(y)) / scale;
        if (std::isfinite(error) && error <= 1e-5)
            continue;
        ++mismatches;
        if (!(error <= worstError))
        {
            worstError = error;
            char text[160];
            std::snprintf(text, sizeof(text), "value %zu: %.9g vs %.9g", i, double(x), double(y));
            worst = text;
        }
    }
    return mismatches;
}

bool Comparer::Compare(const std::string& name, bool vertex, const std::vector<uint8_t>& container,
                       const std::vector<uint32_t>& reference, const std::vector<uint32_t>& translated,
                       std::string& report)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(container.data());
    const auto shader = reinterpret_cast<const Shader*>(container.data() + shaderContainer->shaderOffset);

    std::vector<uint32_t> specializations;
    if (vertex)
    {
        specializations = {kSpecUbo, 0};
    }
    else
    {
        specializations = {kSpecUbo, kSpecUbo | kSpecInvSize, kSpecUbo | kSpecCheapPcf, kSpecUbo | kSpecShadowMinimum,
                           kSpecUbo | kSpecNoBlur, 0};
        for (uint32_t function = 0; function < 8; ++function)
            specializations.push_back(kSpecUbo | kSpecAlphaTest | (function << 16));
    }

    std::vector<std::pair<uint32_t, bool>> inputs;
    uint32_t targets = 1;
    if (vertex)
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement element;
                uint32_t value;
            } u;
            u.value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];
            // USAGE_LOCATIONS, the subset this game uses.
            static const std::map<std::pair<uint32_t, uint32_t>, uint32_t> kLocations = {
                {{0, 0}, 0}, {{3, 0}, 1}, {{6, 0}, 2}, {{7, 0}, 3}, {{5, 0}, 4}, {{5, 1}, 5}, {{5, 2}, 6},
                {{5, 3}, 7}, {{10, 0}, 8}, {{2, 0}, 9}, {{1, 0}, 10}, {{10, 1}, 11}, {{5, 4}, 12}, {{5, 5}, 13},
                {{5, 6}, 14}, {{5, 7}, 15}, {{0, 1}, 15}};
            auto found = kLocations.find({uint32_t(u.element.usage), uint32_t(u.element.usageIndex)});
            if (found == kLocations.end())
            {
                report = "vertex element without a location";
                return false;
            }
            inputs.push_back({found->second, u.element.usage == DeclUsage::BlendIndices});
        }
    }
    else
    {
        const uint32_t outputs = reinterpret_cast<const PixelShader*>(shader)->outputs & 0xF;
        targets = 32 - __builtin_clz(outputs);
    }

    const std::vector<uint32_t> widenedReference = WidenSharedBlock(reference);
    if (const char* folder = std::getenv("NFSMW_SPIRV_GPU_DUMP"))
    {
        if (FILE* f = std::fopen((std::string(folder) + "/" + name + ".ref.spv").c_str(), "wb"))
        {
            std::fwrite(widenedReference.data(), 4, widenedReference.size(), f);
            std::fclose(f);
        }
    }
    const uint32_t seed = uint32_t(std::hash<std::string>{}(name));
    uint32_t totalMismatches = 0;
    std::string details;
    for (uint32_t round = 0; round < 2; ++round)
    {
        FillRandomResources(seed + round);
        for (uint32_t spec : specializations)
        {
            std::vector<float> a, b;
            std::string error;
            const bool okA = vertex ? RunVertex(widenedReference, spec, inputs, a, error) : RunPixel(widenedReference, spec, targets, a, error);
            if (!okA)
            {
                report = "reference: " + error;
                return false;
            }
            const bool okB = vertex ? RunVertex(translated, spec, inputs, b, error, v11_)
                                    : RunPixel(translated, spec, targets, b, error, v11_);
            if (!okB)
            {
                report = "translated: " + error;
                return false;
            }
            std::string worst;
            const uint32_t mismatches = CompareValues(a, b, worst);
            if (mismatches)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "spec %08X round %u: %u values differ, worst ", spec, round, mismatches);
                details += (details.empty() ? "" : "; ") + std::string(text) + worst;
                totalMismatches += mismatches;
            }
        }
    }
    if (totalMismatches)
    {
        report = details;
        return false;
    }
    return true;
}

void Comparer::MeasureCreation(bool vertex, const std::vector<uint8_t>& container,
                               const std::vector<uint32_t>& reference, const std::vector<uint32_t>& translated)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(container.data());
    const auto shader = reinterpret_cast<const Shader*>(container.data() + shaderContainer->shaderOffset);
    std::vector<std::pair<uint32_t, bool>> inputs;
    uint32_t targets = 1;
    if (vertex)
    {
        static const std::map<std::pair<uint32_t, uint32_t>, uint32_t> kLocations = {
            {{0, 0}, 0}, {{3, 0}, 1}, {{6, 0}, 2}, {{7, 0}, 3}, {{5, 0}, 4}, {{5, 1}, 5}, {{5, 2}, 6},
            {{5, 3}, 7}, {{10, 0}, 8}, {{2, 0}, 9}, {{1, 0}, 10}, {{10, 1}, 11}, {{5, 4}, 12}, {{5, 5}, 13},
            {{5, 6}, 14}, {{5, 7}, 15}, {{0, 1}, 15}};
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement element;
                uint32_t value;
            } u;
            u.value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];
            auto found = kLocations.find({uint32_t(u.element.usage), uint32_t(u.element.usageIndex)});
            if (found != kLocations.end())
                inputs.push_back({found->second, u.element.usage == DeclUsage::BlendIndices});
        }
    }
    else
    {
        targets = 32 - __builtin_clz(reinterpret_cast<const PixelShader*>(shader)->outputs & 0xF);
    }
    const std::vector<uint32_t>* modules[2] = {&reference, &translated};
    double times[2] = {0.0, 0.0};
    for (uint32_t which = 0; which < 2; ++which)
    {
        const std::string marker = "nfsmw-timing-" +
                                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                   std::to_string(markerCounter_++);
        const std::vector<uint32_t> marked =
            AddMarker(which == 0 ? WidenSharedBlock(*modules[which]) : *modules[which], marker);
        std::vector<float> ignored;
        std::string error;
        const bool useV11 = which == 1 && v11_;
        const bool ok = vertex ? RunVertex(marked, kSpecUbo, inputs, ignored, error, useV11)
                               : RunPixel(marked, kSpecUbo, targets, ignored, error, useV11);
        if (!ok)
            return;
        times[which] = lastCreationMs_;
    }
    creationMs_[0] += times[0];
    creationMs_[1] += times[1];
    ++measured_;
}

}  // namespace

std::unique_ptr<GpuComparer> GpuComparer::Create(bool translatedV11)
{
    auto comparer = std::make_unique<Comparer>();
    comparer->v11_ = translatedV11;
    if (!comparer->Initialize())
        return nullptr;
    return comparer;
}
