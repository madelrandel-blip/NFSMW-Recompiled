#pragma once

// Direct Xenos microcode -> SPIR-V translator.
//
// It produces the same module interface as the HLSL path (ShaderRecompiler + shader_common.h compiled by DXC):
// the same push constants, set 4 UBOs, descriptor heaps in sets 0-3, specialization constant 0, vertex input
// locations, interpolator locations and OpKill count. The native renderer (app/src/nfsmw_nativo_*.cpp) loads the
// result without knowing which path produced it, so DXC and HLSL are no longer needed to build the shader library.
//
// The translation follows ShaderRecompiler instruction by instruction, including its choices (the clamp of
// rcp/rsq/log, the scalar half of a co-issued instruction reading registers after the vector half wrote them,
// the cube map and alpha test helpers). With SpirvRecompilerOptions::legacyMultiply off, the result computes the
// same values as the DXC library, which is how it is tested (shaders/tests/nfsmw_spirv_check.cpp).
//
// Input: a container in the normalized layout that ShaderRecompiler reads (nfsmw::Convertir2005 turns the 2005
// containers of this game into it).

#include <cstdint>
#include <string>
#include <vector>

// How a module reaches its constants and textures.
enum class SpirvResourceModel
{
    // The interface of the DXC library, for the current native renderer (nfsmw_nativo_*.cpp): SPIR-V 1.5 (Vulkan
    // 1.2), descriptor heaps in sets 0-3 indexed with values from the shared constants, set 4 UBOs or buffer
    // device addresses in the push constants.
    DescriptorHeaps,

    // The interface of the Vulkan 1.1 renderer (app/src/renderer): SPIR-V 1.4 (Vulkan 1.1 with VK_KHR_spirv_1_4,
    // the standard) or 1.3 (core Vulkan 1.1, see SpirvRecompilerOptions::spirvVersion), core features only: no
    // descriptor indexing, no buffer device addresses, no push constants.
    //   Set 0: one combined image sampler per fetch register and kind, binding = register * 4 + kind
    //          (0 = 2D, 1 = 3D, 2 = cube, 3 = the 2D pair of the shadow map minimum).
    //   Set 1: uniform buffers with dynamic offsets: 0 = vertex constants (256 float4), 1 = pixel constants
    //          (224 float4), 2 = the shared block (SharedBlockV11 below).
    Vulkan11,
};

// Byte offsets of the shared block in the Vulkan 1.1 model.
namespace SharedBlockV11
{
constexpr uint32_t kBooleans = 0;          // uint: 16 vertex + 16 pixel boolean constants
constexpr uint32_t kSwappedTexcoords = 4;  // uint: bit per texcoord index, swap xy and zw
constexpr uint32_t kHalfPixelOffset = 8;   // float2
constexpr uint32_t kAlphaThreshold = 16;   // float
constexpr uint32_t kNdcScale = 24;         // float2
constexpr uint32_t kNdcOffset = 32;        // float2
constexpr uint32_t kInputRemap = 48;       // uint[16]: per vertex input location, see remapInput
constexpr uint32_t kInvTextureSize = 112;  // float2[16]: 1/size of the host image of each fetch register
constexpr uint32_t kSize = 240;            // 15 float4
}  // namespace SharedBlockV11

struct SpirvRecompilerOptions
{
    SpirvResourceModel resources = SpirvResourceModel::DescriptorHeaps;

    // Xenos multiplication: 0 * x is 0 for any x, infinity and NaN included (mul, mad, dp2add, dp3, dp4, muls,
    // mulsc and the products of the previous-scalar forms). HLSL and SPIR-V follow IEEE, where 0 * inf is NaN,
    // and what a NaN turns into afterwards depends on the driver: on AMD it reached the bloom as white. Off, the
    // module computes exactly what the DXC library computes.
    bool legacyMultiply = true;

    // Pixel shader color outputs: NaN components are written as 0 (the same rewrite the HLSL path got for AMD).
    bool sanitizeColorOutputs = true;

    // The race VisualTreatment composite (identified by the caller by its SHA-256): the radial blur blend
    // r5.xyz = r5.xyz * r0.xxx + r3.xyz becomes r3.xyz when SPEC_CONSTANT_SIN_DESENFOQUE is set. Exactly one
    // instruction must match, as the HLSL rewrite required.
    bool compositionBlurSwitch = false;

    // SPIR-V version of the Vulkan11 model: 0x10400 (1.4, the standard; needs VK_KHR_spirv_1_4 on Vulkan 1.1) or
    // 0x10300 (1.3, core Vulkan 1.1). The DescriptorHeaps model is always 1.5.
    uint32_t spirvVersion = 0x10400;

    // Relative reads of a constant array (c[a0 + n]) stay inside the array: an index past its end reads 0 instead of
    // the registers after it, so the shader reads nothing past its declared constants. Off (the default), an index
    // runs to the end of the register file, as DXC's library does: 4 of the 302 vertex shaders give other results
    // with out-of-range indices, and whether the game produces them is unknown. Only for the Vulkan11 model.
    bool clampRelativeToArray = false;
};

struct SpirvRecompileResult
{
    std::vector<uint32_t> spirv;  // empty on failure
    std::string error;            // why it failed
    // Instructions the HLSL path also skipped without emitting anything (texture LOD and gradient setters,
    // CondCall, Return). Listed so nothing is dropped silently.
    std::vector<std::string> notes;
    // Shadow map samples rewritten to tfetch2DSombraMin; the library requires at least one in the whole set.
    uint32_t shadowFetches = 0;
    // Float constant registers the shader may read: the highest one plus one (0 if none). With relative reads
    // running to the end of the register file, that end.
    uint32_t floatConstantRegisters = 0;
};

SpirvRecompileResult RecompileToSpirv(const uint8_t* shaderData, const SpirvRecompilerOptions& options);
