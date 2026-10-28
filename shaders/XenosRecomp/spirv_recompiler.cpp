// Direct Xenos microcode -> SPIR-V translator (see spirv_recompiler.h).
//
// Every helper here has a counterpart in ShaderRecompiler (shader_recompiler.cpp) or shader_common.h, named in
// its comment. When one of them changes, the other has to change the same way: nfsmw_spirv_check compares the two
// paths on the whole shader set.

#include "spirv_recompiler.h"

#include "shader.h"
#include "shader_code.h"

#include <SpvBuilder.h>
#include <GLSL.std.450.h>

#include <array>
#include <cfloat>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include <fmt/core.h>

namespace
{

// shader_common.h
constexpr uint32_t kSpecConstantAlphaTest = 1u << 1;
constexpr uint32_t kSpecConstantConstantsUbo = 1u << 8;
constexpr uint32_t kSpecConstantInvTextureSize = 1u << 9;
constexpr uint32_t kSpecConstantCheapPcf = 1u << 15;
constexpr uint32_t kSpecConstantAlphaFuncShift = 16;
constexpr uint32_t kSpecConstantAlphaFuncMask = 7u << 16;
constexpr uint32_t kSpecConstantNoBlur = 1u << 19;
constexpr uint32_t kSpecConstantShadowMinimum = 1u << 23;
constexpr uint32_t kShadowMinimumMarker = 0x5E3B1A84u;

// Byte offsets in the shared constant block (shader_common.h and ShaderRecompiler's sampler defines).
constexpr uint32_t kSharedBooleans = 256;
constexpr uint32_t kSharedSwappedTexcoords = 260;
constexpr uint32_t kSharedHalfPixelOffset = 264;
constexpr uint32_t kSharedAlphaThreshold = 272;
constexpr uint32_t kSharedNdcScale = 280;
constexpr uint32_t kSharedNdcOffset = 288;
constexpr uint32_t kSharedInputRemap = 296;
constexpr uint32_t kSharedInvTextureSize = 360;

// Sizes of the set 4 blocks (NfsmwBloqueVs, NfsmwBloquePs and NfsmwBloqueCompartidas in shader_common.h).
constexpr uint32_t kVertexConstantRegisters = 256;
constexpr uint32_t kPixelConstantRegisters = 224;
// shader_common.h declares 23, but the 1/size of samplers 1-15 lives at bytes 368-487, past that array: the DXC
// modules index it out of bounds. The renderer binds 31 registers (kUboBytesCompartidas in
// the former native renderer), so declaring 31 here reads the same bytes with defined behavior.
constexpr uint32_t kSharedConstantRegisters = 31;

constexpr uint32_t kTemporaryRegisters = 32;

// FLT_MIN in shader_common.h is asfloat(0xff7fffff), that is, -FLT_MAX.
constexpr float kNegativeFltMax = -FLT_MAX;

enum class TextureKind : uint32_t
{
    Texture2D = 0,
    Texture3D = 1,
    TextureCube = 2,
    // The second 2D texture of tfetch2DSombraMin. In the descriptor heap model its index lives in the 3D word of
    // the same register (the shadow map is 2D, so that word is free).
    ShadowPair = 3,
};

// USAGE_LOCATIONS in shader_recompiler.cpp.
struct UsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

constexpr UsageLocation kUsageLocations[] = {
    {DeclUsage::Position, 0, 0},  {DeclUsage::Normal, 0, 1},       {DeclUsage::Tangent, 0, 2},
    {DeclUsage::Binormal, 0, 3},  {DeclUsage::TexCoord, 0, 4},     {DeclUsage::TexCoord, 1, 5},
    {DeclUsage::TexCoord, 2, 6},  {DeclUsage::TexCoord, 3, 7},     {DeclUsage::Color, 0, 8},
    {DeclUsage::BlendIndices, 0, 9}, {DeclUsage::BlendWeight, 0, 10}, {DeclUsage::Color, 1, 11},
    {DeclUsage::TexCoord, 4, 12}, {DeclUsage::TexCoord, 5, 13},    {DeclUsage::TexCoord, 6, 14},
    {DeclUsage::TexCoord, 7, 15}, {DeclUsage::Position, 1, 15},
};

std::optional<uint32_t> InputLocation(DeclUsage usage, uint32_t usageIndex)
{
    for (const auto& entry : kUsageLocations)
    {
        if (entry.usage == usage && entry.usageIndex == usageIndex)
            return entry.location;
    }
    return std::nullopt;
}

// INTERPOLATORS in shader_recompiler.cpp: TEXCOORD0-15 at locations 0-15 and COLOR0-5 at 16-21 (DXC numbers the
// stage variables in declaration order).
constexpr uint32_t kInterpolatorCount = 22;

std::optional<uint32_t> InterpolatorLocation(DeclUsage usage, uint32_t usageIndex)
{
    if (usage == DeclUsage::TexCoord && usageIndex < 16)
        return usageIndex;
    if (usage == DeclUsage::Color && usageIndex < 6)
        return 16 + usageIndex;
    return std::nullopt;
}

const char* InterpolatorName(uint32_t location)
{
    static const char* const kNames[kInterpolatorCount] = {
        "TEXCOORD0", "TEXCOORD1", "TEXCOORD2", "TEXCOORD3", "TEXCOORD4", "TEXCOORD5", "TEXCOORD6", "TEXCOORD7",
        "TEXCOORD8", "TEXCOORD9", "TEXCOORD10", "TEXCOORD11", "TEXCOORD12", "TEXCOORD13", "TEXCOORD14",
        "TEXCOORD15", "COLOR0", "COLOR1", "COLOR2", "COLOR3", "COLOR4", "COLOR5"};
    return kNames[location];
}

struct TranslationError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Fail(const std::string& message)
{
    throw TranslationError(message);
}

// One source operand of an ALU instruction, resolved like op() in ShaderRecompiler::recompile(AluInstruction).
struct Operand
{
    bool temporary = true;
    uint32_t reg = 0;
    // 0 = absolute index, 1 = + a0, 2 = + aL (constants only).
    uint32_t relative = 0;
    bool negate = false;
    bool abs = false;
    std::vector<uint32_t> components;  // source component for each value component

    bool operator==(const Operand& other) const
    {
        return temporary == other.temporary && reg == other.reg && relative == other.relative &&
               negate == other.negate && abs == other.abs && components == other.components;
    }
};

class Translator
{
public:
    Translator(const uint8_t* shaderData, const SpirvRecompilerOptions& options, SpirvRecompileResult& result)
        : data_(shaderData), options_(options), result_(result),
          v11_(options.resources == SpirvResourceModel::Vulkan11),
          spirv14_(!v11_ || options.spirvVersion >= 0x10400),
          clampRelative_(v11_ && options.clampRelativeToArray),
          b_(!v11_ ? spv::Spv_1_5 : spirv14_ ? spv::Spv_1_4 : spv::Spv_1_3, (0x4E46u << 16) | 1 /* generator: "NF" */,
             nullptr)
    {
    }

    std::vector<uint32_t> Translate();

private:
    // ---- Module setup -------------------------------------------------------------------------------------------
    void ReadContainer();
    void DeclareTypes();
    void DeclareResources();
    void DeclareStageInterface();
    void InitializeLocals();

    // ---- Constants and resources ------------------------------------------------------------------------------
    spv::Id SpecBitSet(uint32_t bit);
    spv::Id SpecConstantsValue();
    // NFSMW_UBO ? <ubo load> : <buffer pointer load>, as a branch (HLSL 2021 short-circuits the scalar ternary).
    spv::Id UboOrPointer(spv::Id type, const std::function<spv::Id()>& fromUbo,
                         const std::function<spv::Id()>& fromPointer);
    spv::Id PointerLoad(uint32_t pushConstantMember, spv::Id byteOffset64, spv::Id type, uint32_t alignment);
    spv::Id SharedUint(uint32_t byteOffset);
    spv::Id SharedFloat(uint32_t byteOffset);
    spv::Id SharedFloat2(uint32_t byteOffset);
    spv::Id FloatConstantRegister(spv::Id index);  // index: uint register number in this stage's block
    spv::Id LoadConstant(uint32_t reg, uint32_t relative);
    uint32_t SharedOffset(uint32_t heapModelOffset) const;
    spv::Id SharedUboVariable();
    spv::Id SharedUboComponent(uint32_t heapModelOffset);
    spv::Id SampledImage(uint32_t samplerRegister, TextureKind kind);

    // ---- Values -------------------------------------------------------------------------------------------------
    spv::Id FloatType(uint32_t components) const { return components == 1 ? tFloat_ : tFloatN_[components]; }
    spv::Id BoolType(uint32_t components) const { return components == 1 ? tBool_ : tBoolN_[components]; }
    spv::Id FloatConst(float value) { return b_.makeFloatConstant(value); }
    spv::Id FloatSplat(float value, uint32_t components);
    spv::Id Extract(spv::Id vec4, const std::vector<uint32_t>& components);
    spv::Id Component(spv::Id value, uint32_t index);
    spv::Id Std450(GLSLstd450 op, spv::Id type, std::vector<spv::Id> args);
    spv::Id BoolToFloat(spv::Id condition, uint32_t components);
    spv::Id Multiply(spv::Id a, spv::Id bValue, spv::Id aAbs, spv::Id bAbs, uint32_t components);
    spv::Id Saturate(spv::Id value, uint32_t components);
    spv::Id SelectIsNan(spv::Id value, uint32_t components);

    // ---- Registers ----------------------------------------------------------------------------------------------
    spv::Id TemporaryVariable(uint32_t reg);
    spv::Id LoadVariable4(spv::Id variable) { return b_.createLoad(variable, spv::NoPrecision); }
    void WriteMasked(spv::Id variable, uint32_t mask, spv::Id value, uint32_t valueComponents);
    void WriteComponent(spv::Id variable, uint32_t component, spv::Id scalar);

    // ---- Instructions -------------------------------------------------------------------------------------------
    Operand ResolveOperand(const AluInstruction& instr, uint32_t which);
    spv::Id LoadOperand(const Operand& operand);
    spv::Id LoadOperandAbs(const Operand& operand);  // |value| before negation, for the multiply rule
    void OpenPredicate(bool condition);
    void ClosePredicate();
    void TranslateVertexFetch(const VertexFetchInstruction& instr, uint32_t address);
    void TranslateTextureFetch(const TextureFetchInstruction& instr);
    void TranslateAlu(const AluInstruction& instr);
    spv::Id VectorOperation(const AluInstruction& instr, uint32_t components, bool& handled);
    spv::Id ScalarOperation(const AluInstruction& instr);
    void Kill(spv::Id condition);
    spv::Id Tfetch2D(uint32_t samplerRegister, TextureKind kind, spv::Id coord, spv::Id offset, spv::Id invSize);

    // ---- Control flow -------------------------------------------------------------------------------------------
    void TranslateControlFlow();
    void OpenIf(spv::Id condition);
    void OpenLoop(uint32_t loopId);
    void CloseInnermost();
    void EmitShaderEnd();  // the code ShaderRecompiler emits for an ExecEnd before its `return`
    void EmitReturn();
    void WriteStageOutputs();

    // ---- State --------------------------------------------------------------------------------------------------
    const uint8_t* data_;
    const SpirvRecompilerOptions& options_;
    SpirvRecompileResult& result_;
    const bool v11_;      // SpirvResourceModel::Vulkan11
    const bool spirv14_;  // SPIR-V 1.4 or later: the entry point lists every global it uses
    const bool clampRelative_;  // SpirvRecompilerOptions::clampRelativeToArray
    spv::Builder b_;

    const ShaderContainer* container_ = nullptr;
    const Shader* shader_ = nullptr;
    const uint8_t* constantTableData_ = nullptr;
    bool pixel_ = false;
    uint32_t pixelOutputs_ = 0;

    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants_;
    std::unordered_map<uint32_t, std::array<uint32_t, 4>> float4Definitions_;  // c{N} literals
    std::unordered_map<uint32_t, std::array<int32_t, 4>> int4Definitions_;     // i{N} literals
    std::unordered_map<uint32_t, uint32_t> boolConstants_;                     // CF address -> g_Booleans bit
    std::unordered_map<uint32_t, std::string> samplers_;                       // fetch constant -> name
    std::unordered_map<uint32_t, VertexElement> vertexElements_;               // fetch address -> element
    std::unordered_map<uint32_t, uint32_t> exportInterpolators_;               // VS export index -> location

    spv::Id glsl_ = 0;
    spv::Id tVoid_ = 0, tBool_ = 0, tFloat_ = 0, tInt_ = 0, tUint_ = 0, tUint64_ = 0;
    std::array<spv::Id, 5> tFloatN_{}, tBoolN_{}, tUintN_{};
    spv::Id tImage2D_ = 0, tImage3D_ = 0, tImageCube_ = 0, tSampler_ = 0;

    spv::Function* main_ = nullptr;
    std::vector<spv::Id> interface_;

    spv::Id specConstants_ = 0;
    spv::Id pushConstants_ = 0;
    spv::Id uboStage_ = 0;   // g_UboVertex or g_UboPixel, created on first use
    spv::Id uboShared_ = 0;  // g_UboCompartidas, created on first use
    std::array<spv::Id, 3> textureHeaps_{};
    spv::Id samplerHeap_ = 0;
    std::map<uint32_t, spv::Id> combinedSamplers_;  // Vulkan11: binding -> combined image sampler

    // Stage interface
    std::unordered_map<uint32_t, spv::Id> vertexInputs_;  // fetch address -> Input variable
    std::unordered_map<uint32_t, bool> vertexInputIsUint_;
    std::unordered_map<uint32_t, uint32_t> vertexInputLocation_;
    spv::Id outPosition_ = 0;                             // VS: SV_Position Output
    std::array<spv::Id, kInterpolatorCount> outInterpolators_{};  // VS Outputs
    spv::Id fragCoord_ = 0, frontFacing_ = 0;
    std::array<spv::Id, 4> outColors_{};                  // PS Outputs

    // Function locals (the HLSL main's variables)
    std::array<spv::Id, kTemporaryRegisters> r_{};
    spv::Id oPos_ = 0;
    std::array<spv::Id, kInterpolatorCount> oInterp_{};
    std::array<spv::Id, 4> oC_{};
    spv::Id a0_ = 0, aL_ = 0, p0_ = 0, ps_ = 0;
    spv::Id cubeDirections_[2] = {0, 0};
    spv::Id cubeIndex_ = 0;

    // Control flow: the innermost-last stack of open braces, exactly like the HLSL text.
    struct OpenConstruct
    {
        bool loop = false;
        std::unique_ptr<spv::Builder::If> ifBuilder;
        spv::Builder::LoopBlocks* loopBlocks = nullptr;
    };
    std::vector<OpenConstruct> open_;
    std::unique_ptr<spv::Builder::If> predicate_;  // per-instruction `if (p0)`

    uint32_t blurMatches_ = 0;
};

// =================================================================================================================
// Module setup
// =================================================================================================================

void Translator::ReadContainer()
{
    container_ = reinterpret_cast<const ShaderContainer*>(data_);
    if ((container_->flags & 0xFFFF0000) != 0x102A0000)
        Fail("unknown container signature");
    if (container_->constantTableOffset == 0)
        Fail("container without a constant table");
    pixel_ = (container_->flags & 0x1) == 0;

    const auto constantTableContainer =
        reinterpret_cast<const ConstantTableContainer*>(data_ + container_->constantTableOffset);
    constantTableData_ = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);
    shader_ = reinterpret_cast<const Shader*>(data_ + container_->shaderOffset);

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto info = reinterpret_cast<const ConstantInfo*>(
            constantTableData_ + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));
        const char* name = reinterpret_cast<const char*>(constantTableData_ + info->name);
        switch (info->registerSet)
        {
        case RegisterSet::Float4:
            for (uint16_t j = 0; j < info->registerCount; j++)
                float4Constants_.emplace(info->registerIndex + j, info);
            break;
        case RegisterSet::Sampler:
            samplers_.emplace(info->registerIndex, name);
            break;
        case RegisterSet::Bool:
            // In NFSMW the CTAB numbers b0.. and the control flow uses 128.. for pixel shaders; g_Booleans keeps
            // 16 bits per stage.
            boolConstants_.emplace(info->registerIndex + (pixel_ ? 128 : 0),
                                   info->registerIndex + (pixel_ ? 16 : 0));
            break;
        default:
            break;
        }
    }

    if (container_->definitionTableOffset != 0)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(data_ + container_->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            auto value =
                reinterpret_cast<const be<uint32_t>*>(data_ + container_->virtualSize + definition->physicalOffset);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
                float4Definitions_[definition->registerIndex + i - (pixel_ ? 256 : 0)] = {
                    value[0].get(), value[1].get(), value[2].get(), value[3].get()};
                value += 4;
            }
            definitions += 2;
        }
        ++definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Int4Definition*>(definitions);
            for (uint16_t i = 0; i < definition->count; i++)
            {
                const uint32_t v = definition->values[i].get();
                // The bytes in memory order, as ShaderRecompiler's union reads them on a little-endian host.
                int4Definitions_[(definition->registerIndex - 8992) / 4 + i] = {
                    int8_t(v & 0xFF), int8_t((v >> 8) & 0xFF), int8_t((v >> 16) & 0xFF), int8_t(v >> 24)};
            }
            definitions += 2;
            definitions += definition->count;
        }
    }

    if (pixel_)
    {
        pixelOutputs_ = reinterpret_cast<const PixelShader*>(shader_)->outputs;
        if ((pixelOutputs_ & PIXEL_SHADER_OUTPUT_COLOR0) == 0)
            Fail("pixel shader without COLOR0 (the alpha test reads oC0)");
        if (pixelOutputs_ & PIXEL_SHADER_OUTPUT_DEPTH)
            Fail("pixel shader depth output is not supported");
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader_);
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement element;
                uint32_t value;
            } u;
            u.value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];
            vertexElements_.emplace(uint32_t(u.element.address), u.element);
        }
    }
}

void Translator::DeclareTypes()
{
    tVoid_ = b_.makeVoidType();
    tBool_ = b_.makeBoolType();
    tFloat_ = b_.makeFloatType(32);
    tInt_ = b_.makeIntType(32);
    tUint_ = b_.makeUintType(32);
    tUint64_ = b_.makeUintType(64);
    for (uint32_t n = 2; n <= 4; ++n)
    {
        tFloatN_[n] = b_.makeVectorType(tFloat_, n);
        tBoolN_[n] = b_.makeVectorType(tBool_, n);
        tUintN_[n] = b_.makeVectorType(tUint_, n);
    }
    tImage2D_ = b_.makeImageType(tFloat_, spv::Dim2D, false, false, false, 1, spv::ImageFormatUnknown);
    tImage3D_ = b_.makeImageType(tFloat_, spv::Dim3D, false, false, false, 1, spv::ImageFormatUnknown);
    tImageCube_ = b_.makeImageType(tFloat_, spv::DimCube, false, false, false, 1, spv::ImageFormatUnknown);
    tSampler_ = b_.makeSamplerType();
}

void Translator::DeclareResources()
{
    // [[vk::constant_id(0)]] const uint g_SpecConstants = 0;
    specConstants_ = b_.makeUintConstant(0, true);
    b_.addDecoration(specConstants_, spv::DecorationSpecId, 0);
    b_.addName(specConstants_, "g_SpecConstants");

    if (v11_)
        return;  // constants only come through the set 1 UBOs

    // [[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
    spv::Id pushType = b_.makeStructType({tUint64_, tUint64_, tUint64_}, "type_ConstantBuffer_PushConstants");
    b_.addMemberName(pushType, 0, "VertexShaderConstants");
    b_.addMemberName(pushType, 1, "PixelShaderConstants");
    b_.addMemberName(pushType, 2, "SharedConstants");
    b_.addMemberDecoration(pushType, 0, spv::DecorationOffset, 0);
    b_.addMemberDecoration(pushType, 1, spv::DecorationOffset, 8);
    b_.addMemberDecoration(pushType, 2, spv::DecorationOffset, 16);
    b_.addDecoration(pushType, spv::DecorationBlock);
    pushConstants_ = b_.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, pushType, "g_PushConstants");
}

spv::Id MakeUboVariable(spv::Builder& b, spv::Id float4, uint32_t registers, uint32_t set, uint32_t binding,
                        const char* typeName, const char* name)
{
    spv::Id array = b.makeArrayType(float4, b.makeUintConstant(registers), 16);
    // makeArrayType does not decorate: -fvk-use-dx-layout gives each float4 16 bytes.
    b.addDecoration(array, spv::DecorationArrayStride, 16);
    spv::Id block = b.makeStructType({array}, typeName);
    b.addMemberName(block, 0, "v");
    b.addMemberDecoration(block, 0, spv::DecorationOffset, 0);
    b.addDecoration(block, spv::DecorationBlock);
    spv::Id variable = b.createVariable(spv::NoPrecision, spv::StorageClassUniform, block, name);
    b.addDecoration(variable, spv::DecorationDescriptorSet, int(set));
    b.addDecoration(variable, spv::DecorationBinding, int(binding));
    return variable;
}

void Translator::DeclareStageInterface()
{
    if (pixel_)
    {
        // Interpolators the shader reads, each at its INTERPOLATORS location.
        uint32_t interpolatorCount = (shader_->interpolatorInfo >> 5) & 0x1F;
        for (uint32_t i = 0; i < interpolatorCount; i++)
        {
            union
            {
                Interpolator interpolator;
                uint32_t value;
            } u;
            u.value = reinterpret_cast<const PixelShader*>(shader_)->interpolators[i];
            auto location = InterpolatorLocation(u.interpolator.usage, u.interpolator.usageIndex);
            if (!location)
                Fail(fmt::format("pixel shader interpolator with usage {} index {} has no location",
                                 uint32_t(u.interpolator.usage), uint32_t(u.interpolator.usageIndex)));
        }
        for (uint32_t i = 0; i < 4; ++i)
        {
            if ((pixelOutputs_ & (PIXEL_SHADER_OUTPUT_COLOR0 << i)) == 0)
                continue;
            outColors_[i] = b_.createVariable(spv::NoPrecision, spv::StorageClassOutput, tFloatN_[4],
                                              fmt::format("out_var_SV_Target{}", i).c_str());
            b_.addDecoration(outColors_[i], spv::DecorationLocation, int(i));
            interface_.push_back(outColors_[i]);
        }
    }
    else
    {
        for (const auto& [address, element] : vertexElements_)
        {
            auto location = InputLocation(element.usage, element.usageIndex);
            if (!location)
                Fail(fmt::format("vertex element with usage {} index {} has no location", uint32_t(element.usage),
                                 uint32_t(element.usageIndex)));
            // USAGE_TYPES: everything is float4 except BLENDINDICES.
            const bool isUint = element.usage == DeclUsage::BlendIndices;
            spv::Id variable = b_.createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                 isUint ? tUintN_[4] : tFloatN_[4]);
            b_.addDecoration(variable, spv::DecorationLocation, int(*location));
            vertexInputs_[address] = variable;
            vertexInputIsUint_[address] = isUint;
            vertexInputLocation_[address] = *location;
            interface_.push_back(variable);
        }
        outPosition_ = b_.createVariable(spv::NoPrecision, spv::StorageClassOutput, tFloatN_[4], "gl_Position");
        b_.addDecoration(outPosition_, spv::DecorationBuiltIn, spv::BuiltInPosition);
        interface_.push_back(outPosition_);
        for (uint32_t i = 0; i < kInterpolatorCount; ++i)
        {
            outInterpolators_[i] = b_.createVariable(spv::NoPrecision, spv::StorageClassOutput, tFloatN_[4],
                                                     fmt::format("out_var_{}", InterpolatorName(i)).c_str());
            b_.addDecoration(outInterpolators_[i], spv::DecorationLocation, int(i));
            interface_.push_back(outInterpolators_[i]);
        }

        auto vertexShader = reinterpret_cast<const VertexShader*>(shader_);
        uint32_t interpolatorCount = (shader_->interpolatorInfo >> 5) & 0x1F;
        for (uint32_t i = 0; i < interpolatorCount; i++)
        {
            union
            {
                Interpolator interpolator;
                uint32_t value;
            } u;
            u.value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 +
                                                                   vertexShader->vertexElementCount + i];
            auto location = InterpolatorLocation(u.interpolator.usage, u.interpolator.usageIndex);
            if (!location)
                Fail(fmt::format("vertex shader interpolator with usage {} index {} has no location",
                                 uint32_t(u.interpolator.usage), uint32_t(u.interpolator.usageIndex)));
            exportInterpolators_[i] = *location;
        }
    }
}

void Translator::InitializeLocals()
{
    spv::Id zero4 = FloatSplat(0.0f, 4);

    if (!pixel_)
    {
        // oPos = 0.0; o<interpolator> = 0.0;
        oPos_ = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[4], "oPos");
        b_.createStore(zero4, oPos_);
        for (uint32_t i = 0; i < kInterpolatorCount; ++i)
        {
            oInterp_[i] = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[4],
                                            fmt::format("o{}", InterpolatorName(i)).c_str());
            b_.createStore(zero4, oInterp_[i]);
        }
    }
    else
    {
        // The HLSL out parameters start undefined; 0 here, so a component the shader never exports is stable.
        for (uint32_t i = 0; i < 4; ++i)
        {
            if (!outColors_[i])
                continue;
            oC_[i] = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[4],
                                       fmt::format("oC{}", i).c_str());
            b_.createStore(zero4, oC_[i]);
        }
    }

    // float4 rN = <interpolator> / <svPos> / 0.0;
    std::array<bool, kTemporaryRegisters> initialized{};
    if (pixel_)
    {
        uint32_t interpolatorCount = (shader_->interpolatorInfo >> 5) & 0x1F;
        for (uint32_t i = 0; i < interpolatorCount; i++)
        {
            union
            {
                Interpolator interpolator;
                uint32_t value;
            } u;
            u.value = reinterpret_cast<const PixelShader*>(shader_)->interpolators[i];
            const uint32_t location = *InterpolatorLocation(u.interpolator.usage, u.interpolator.usageIndex);
            spv::Id input = b_.createVariable(spv::NoPrecision, spv::StorageClassInput, tFloatN_[4],
                                              fmt::format("in_var_{}", InterpolatorName(location)).c_str());
            b_.addDecoration(input, spv::DecorationLocation, int(location));
            interface_.push_back(input);
            const uint32_t reg = u.interpolator.reg;
            r_[reg] = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[4],
                                        fmt::format("r{}", reg).c_str());
            b_.createStore(b_.createLoad(input, spv::NoPrecision), r_[reg]);
            initialized[reg] = true;
        }
    }

    const uint32_t svPosRegister = (shader_->fieldC >> 8) & 0xFF;
    for (uint32_t i = 0; i < kTemporaryRegisters; ++i)
    {
        if (initialized[i])
            continue;
        r_[i] = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[4],
                                  fmt::format("r{}", i).c_str());
        if (pixel_ && i == svPosRegister)
        {
            // float4((iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0)
            fragCoord_ = b_.createVariable(spv::NoPrecision, spv::StorageClassInput, tFloatN_[4], "gl_FragCoord");
            b_.addDecoration(fragCoord_, spv::DecorationBuiltIn, spv::BuiltInFragCoord);
            frontFacing_ = b_.createVariable(spv::NoPrecision, spv::StorageClassInput, tBool_, "gl_FrontFacing");
            b_.addDecoration(frontFacing_, spv::DecorationBuiltIn, spv::BuiltInFrontFacing);
            interface_.push_back(fragCoord_);
            interface_.push_back(frontFacing_);
            spv::Id position = b_.createLoad(fragCoord_, spv::NoPrecision);
            spv::Id xy = b_.createRvalueSwizzle(spv::NoPrecision, tFloatN_[2], position, {0, 1});
            xy = b_.createBinOp(spv::OpFSub, tFloatN_[2], xy, FloatSplat(0.5f, 2));
            spv::Id face = b_.createTriOp(spv::OpSelect, tFloat_, b_.createLoad(frontFacing_, spv::NoPrecision),
                                          FloatConst(1.0f), FloatConst(-1.0f));
            xy = b_.createBinOp(spv::OpFMul, tFloatN_[2], xy,
                                b_.createCompositeConstruct(tFloatN_[2], {face, FloatConst(1.0f)}));
            spv::Id value = b_.createCompositeConstruct(
                tFloatN_[4], {Component(xy, 0), Component(xy, 1), FloatConst(0.0f), FloatConst(0.0f)});
            b_.createStore(value, r_[i]);
        }
        else
        {
            b_.createStore(zero4, r_[i]);
        }
    }

    // int a0 = 0; int aL = 0; bool p0 = false; float ps = 0.0;
    a0_ = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tInt_, "a0");
    b_.createStore(b_.makeIntConstant(0), a0_);
    aL_ = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tInt_, "aL");
    b_.createStore(b_.makeIntConstant(0), aL_);
    p0_ = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tBool_, "p0");
    b_.createStore(b_.makeBoolConstant(false), p0_);
    ps_ = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloat_, "ps");
    b_.createStore(FloatConst(0.0f), ps_);
    if (pixel_)
    {
        // CubeMapData cubeMapData = (CubeMapData)0;
        for (auto& direction : cubeDirections_)
        {
            direction = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[3]);
            b_.createStore(FloatSplat(0.0f, 3), direction);
        }
        cubeIndex_ = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tUint_, "cubeMapIndex");
        b_.createStore(b_.makeUintConstant(0), cubeIndex_);
    }
}

// =================================================================================================================
// Constants and resources
// =================================================================================================================

spv::Id Translator::SpecConstantsValue()
{
    return specConstants_;
}

spv::Id Translator::SpecBitSet(uint32_t bit)
{
    spv::Id masked = b_.createBinOp(spv::OpBitwiseAnd, tUint_, specConstants_, b_.makeUintConstant(bit));
    return b_.createBinOp(spv::OpINotEqual, tBool_, masked, b_.makeUintConstant(0));
}

spv::Id Translator::UboOrPointer(spv::Id type, const std::function<spv::Id()>& fromUbo,
                                 const std::function<spv::Id()>& fromPointer)
{
    if (v11_)
        return fromUbo();  // the Vulkan 1.1 model has no pointer path
    spv::Id variable = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, type);
    spv::Builder::If branch(SpecBitSet(kSpecConstantConstantsUbo), spv::SelectionControlMaskNone, b_);
    b_.createStore(fromUbo(), variable);
    branch.makeBeginElse();
    b_.createStore(fromPointer(), variable);
    branch.makeEndIf();
    return b_.createLoad(variable, spv::NoPrecision);
}

spv::Id Translator::PointerLoad(uint32_t pushConstantMember, spv::Id byteOffset64, spv::Id type, uint32_t alignment)
{
    // vk::RawBufferLoad<T>(g_PushConstants.<member> + offset, alignment)
    spv::Id memberPointer = b_.createAccessChain(spv::StorageClassPushConstant, pushConstants_,
                                                 {b_.makeIntConstant(int(pushConstantMember))});
    spv::Id base = b_.createLoad(memberPointer, spv::NoPrecision);
    spv::Id address = b_.createBinOp(spv::OpIAdd, tUint64_, base, byteOffset64);
    spv::Id pointerType = b_.makePointer(spv::StorageClassPhysicalStorageBuffer, type);
    spv::Id pointer = b_.createUnaryOp(spv::OpConvertUToPtr, pointerType, address);
    return b_.createLoad(pointer, spv::NoPrecision, spv::MemoryAccessAlignedMask, spv::ScopeMax, alignment);
}

uint32_t Translator::SharedOffset(uint32_t heapModelOffset) const
{
    if (!v11_)
        return heapModelOffset;
    // shader_common.h layout -> SharedBlockV11.
    if (heapModelOffset >= kSharedInvTextureSize)
        return SharedBlockV11::kInvTextureSize + (heapModelOffset - kSharedInvTextureSize);
    if (heapModelOffset >= kSharedInputRemap)
        return SharedBlockV11::kInputRemap + (heapModelOffset - kSharedInputRemap);
    switch (heapModelOffset)
    {
    case kSharedBooleans:
        return SharedBlockV11::kBooleans;
    case kSharedSwappedTexcoords:
        return SharedBlockV11::kSwappedTexcoords;
    case kSharedHalfPixelOffset:
    case kSharedHalfPixelOffset + 4:
        return SharedBlockV11::kHalfPixelOffset + (heapModelOffset - kSharedHalfPixelOffset);
    case kSharedAlphaThreshold:
        return SharedBlockV11::kAlphaThreshold;
    case kSharedNdcScale:
    case kSharedNdcScale + 4:
        return SharedBlockV11::kNdcScale + (heapModelOffset - kSharedNdcScale);
    case kSharedNdcOffset:
    case kSharedNdcOffset + 4:
        return SharedBlockV11::kNdcOffset + (heapModelOffset - kSharedNdcOffset);
    default:
        break;
    }
    Fail(fmt::format("shared constant at byte {} has no place in the Vulkan 1.1 block", heapModelOffset));
}

spv::Id Translator::SharedUboVariable()
{
    if (!uboShared_)
    {
        uboShared_ = v11_ ? MakeUboVariable(b_, tFloatN_[4], SharedBlockV11::kSize / 16, 1, 2, "SharedBlock", "g_Shared")
                          : MakeUboVariable(b_, tFloatN_[4], kSharedConstantRegisters, 4, 2,
                                            "type_ConstantBuffer_NfsmwBloqueCompartidas", "g_UboCompartidas");
    }
    return uboShared_;
}

spv::Id Translator::SharedUboComponent(uint32_t heapModelOffset)
{
    // With -fvk-use-dx-layout each float4 takes 16 bytes, so byte B is component (B % 16) / 4 of register B / 16.
    const uint32_t offset = SharedOffset(heapModelOffset);
    spv::Id pointer = b_.createAccessChain(
        spv::StorageClassUniform, SharedUboVariable(),
        {b_.makeIntConstant(0), b_.makeUintConstant(offset / 16), b_.makeUintConstant((offset % 16) / 4)});
    return b_.createLoad(pointer, spv::NoPrecision);
}

spv::Id Translator::SharedUint(uint32_t byteOffset)
{
    return UboOrPointer(
        tUint_, [&] { return b_.createUnaryOp(spv::OpBitcast, tUint_, SharedUboComponent(byteOffset)); },
        [&] { return PointerLoad(2, b_.makeUint64Constant(byteOffset), tUint_, 4); });
}

spv::Id Translator::SharedFloat(uint32_t byteOffset)
{
    return UboOrPointer(
        tFloat_, [&] { return SharedUboComponent(byteOffset); },
        [&] { return PointerLoad(2, b_.makeUint64Constant(byteOffset), tFloat_, 4); });
}

spv::Id Translator::SharedFloat2(uint32_t byteOffset)
{
    return UboOrPointer(
        tFloatN_[2],
        [&] {
            spv::Id x = SharedUboComponent(byteOffset);
            spv::Id y = SharedUboComponent(byteOffset + 4);
            return b_.createCompositeConstruct(tFloatN_[2], {x, y});
        },
        [&] { return PointerLoad(2, b_.makeUint64Constant(byteOffset), tFloatN_[2], 4); });
}

spv::Id Translator::FloatConstantRegister(spv::Id index)
{
    // NFSMW_UBO ? g_Ubo<Stage>.v[index] : vk::RawBufferLoad<float4>(g_PushConstants.<Stage>ShaderConstants + index * 16, 0x10)
    return UboOrPointer(
        tFloatN_[4],
        [&] {
            if (!uboStage_)
            {
                const uint32_t set = v11_ ? 1 : 4;
                uboStage_ = pixel_ ? MakeUboVariable(b_, tFloatN_[4], kPixelConstantRegisters, set, 1,
                                                     "type_ConstantBuffer_NfsmwBloquePs", "g_UboPixel")
                                   : MakeUboVariable(b_, tFloatN_[4], kVertexConstantRegisters, set, 0,
                                                     "type_ConstantBuffer_NfsmwBloqueVs", "g_UboVertex");
            }
            spv::Id pointer =
                b_.createAccessChain(spv::StorageClassUniform, uboStage_, {b_.makeIntConstant(0), index});
            return b_.createLoad(pointer, spv::NoPrecision);
        },
        [&] {
            spv::Id offset = b_.createBinOp(spv::OpIMul, tUint64_, b_.createUnaryOp(spv::OpUConvert, tUint64_, index),
                                            b_.makeUint64Constant(16));
            return PointerLoad(pixel_ ? 1 : 0, offset, tFloatN_[4], 16);
        });
}

spv::Id Translator::LoadConstant(uint32_t reg, uint32_t relative)
{
    auto found = float4Constants_.find(reg);
    if (found != float4Constants_.end())
    {
        const ConstantInfo* info = found->second;
        const uint32_t base = info->registerIndex;
        if (info->registerCount > 1)
        {
            // NAME(INDEX): select((INDEX) < tail, load(base + min(INDEX, tail - 1)), 0.0), with tail the registers
            // left in the file, or only the array's with clampRelativeToArray.
            const uint32_t fileTail = (pixel_ ? kPixelConstantRegisters : kVertexConstantRegisters) - base;
            const uint32_t tail = clampRelative_ ? std::min<uint32_t>(info->registerCount, fileTail) : fileTail;
            result_.floatConstantRegisters = std::max(result_.floatConstantRegisters, base + (relative ? tail : reg - base + 1));
            spv::Id index = b_.makeIntConstant(int(reg - base));
            if (relative)
            {
                spv::Id address = b_.createLoad(relative == 1 ? a0_ : aL_, spv::NoPrecision);
                index = b_.createBinOp(spv::OpIAdd, tInt_, index, address);
            }
            spv::Id inRange = b_.createBinOp(spv::OpSLessThan, tBool_, index, b_.makeIntConstant(int(tail)));
            spv::Id clamped = Std450(GLSLstd450SMin, tInt_, {index, b_.makeIntConstant(int(tail - 1))});
            spv::Id registerIndex = b_.createBinOp(spv::OpIAdd, tInt_, b_.makeIntConstant(int(base)), clamped);
            spv::Id value = FloatConstantRegister(b_.createUnaryOp(spv::OpBitcast, tUint_, registerIndex));
            spv::Id condition = b_.smearScalar(spv::NoPrecision, inRange, tBoolN_[4]);
            return b_.createTriOp(spv::OpSelect, tFloatN_[4], condition, value, FloatSplat(0.0f, 4));
        }
        if (relative)
            Fail("relative addressing of a scalar constant");
        result_.floatConstantRegisters = std::max(result_.floatConstantRegisters, reg + 1);
        return FloatConstantRegister(b_.makeUintConstant(reg));
    }
    if (relative)
        Fail("relative addressing without a declared constant");
    auto definition = float4Definitions_.find(reg);
    if (definition == float4Definitions_.end())
        Fail(fmt::format("constant c{} is neither declared nor defined", reg));
    std::vector<spv::Id> parts;
    for (uint32_t bits : definition->second)
    {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        // makeFloatConstant merges +0 and -0 and NaN payloads; the bits have to survive, as asfloat keeps them.
        parts.push_back(b_.createUnaryOp(spv::OpBitcast, tFloat_, b_.makeUintConstant(bits)));
    }
    return b_.createCompositeConstruct(tFloatN_[4], parts);
}

spv::Id Translator::SampledImage(uint32_t samplerRegister, TextureKind kind)
{
    const spv::Id imageType = kind == TextureKind::Texture3D     ? tImage3D_
                              : kind == TextureKind::TextureCube ? tImageCube_
                                                                 : tImage2D_;
    const spv::Id sampledType = b_.makeSampledImageType(imageType);

    if (v11_)
    {
        // One combined image sampler per register and kind, at a fixed binding of set 0.
        const uint32_t binding = samplerRegister * 4 + uint32_t(kind);
        spv::Id& variable = combinedSamplers_[binding];
        if (!variable)
        {
            static const char* const kKinds[] = {"2D", "3D", "Cube", "ShadowPair"};
            variable = b_.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, sampledType,
                                         fmt::format("g_Texture{}_{}", kKinds[uint32_t(kind)], samplerRegister).c_str());
            b_.addDecoration(variable, spv::DecorationDescriptorSet, 0);
            b_.addDecoration(variable, spv::DecorationBinding, int(binding));
        }
        return b_.createLoad(variable, spv::NoPrecision);
    }

    // g_Texture<Dim>DescriptorHeap[<sampler>_Texture<Dim>DescriptorIndex] with
    // g_SamplerDescriptorHeap[<sampler>_SamplerDescriptorIndex].
    const uint32_t heap = kind == TextureKind::ShadowPair ? 0 : uint32_t(kind);
    const uint32_t indexWord = kind == TextureKind::ShadowPair ? 1 : uint32_t(kind);
    if (!textureHeaps_[heap])
    {
        static const char* const kHeapNames[] = {"g_Texture2DDescriptorHeap", "g_Texture3DDescriptorHeap",
                                                 "g_TextureCubeDescriptorHeap"};
        textureHeaps_[heap] = b_.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
                                                b_.makeRuntimeArray(imageType), kHeapNames[heap]);
        b_.addDecoration(textureHeaps_[heap], spv::DecorationDescriptorSet, int(heap));
        b_.addDecoration(textureHeaps_[heap], spv::DecorationBinding, 0);
    }
    if (!samplerHeap_)
    {
        samplerHeap_ = b_.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
                                         b_.makeRuntimeArray(tSampler_), "g_SamplerDescriptorHeap");
        b_.addDecoration(samplerHeap_, spv::DecorationDescriptorSet, 3);
        b_.addDecoration(samplerHeap_, spv::DecorationBinding, 0);
    }
    spv::Id imageIndex = SharedUint(indexWord * 64 + samplerRegister * 4);
    spv::Id samplerIndex = SharedUint(3 * 64 + samplerRegister * 4);
    spv::Id image = b_.createLoad(
        b_.createAccessChain(spv::StorageClassUniformConstant, textureHeaps_[heap], {imageIndex}), spv::NoPrecision);
    spv::Id sampler = b_.createLoad(
        b_.createAccessChain(spv::StorageClassUniformConstant, samplerHeap_, {samplerIndex}), spv::NoPrecision);
    return b_.createBinOp(spv::OpSampledImage, sampledType, image, sampler);
}

// =================================================================================================================
// Values
// =================================================================================================================

spv::Id Translator::FloatSplat(float value, uint32_t components)
{
    spv::Id scalar = FloatConst(value);
    if (components == 1)
        return scalar;
    std::vector<spv::Id> parts(components, scalar);
    return b_.makeCompositeConstant(tFloatN_[components], parts);
}

spv::Id Translator::Extract(spv::Id vec4, const std::vector<uint32_t>& components)
{
    if (components.size() == 1)
        return b_.createCompositeExtract(vec4, tFloat_, components[0]);
    std::vector<unsigned> channels(components.begin(), components.end());
    return b_.createRvalueSwizzle(spv::NoPrecision, tFloatN_[components.size()], vec4, channels);
}

spv::Id Translator::Component(spv::Id value, uint32_t index)
{
    if (b_.getNumComponents(value) == 1)
        return value;
    return b_.createCompositeExtract(value, tFloat_, index);
}

spv::Id Translator::Std450(GLSLstd450 op, spv::Id type, std::vector<spv::Id> args)
{
    return b_.createBuiltinCall(type, glsl_, op, args);
}

spv::Id Translator::BoolToFloat(spv::Id condition, uint32_t components)
{
    return b_.createTriOp(spv::OpSelect, FloatType(components), condition, FloatSplat(1.0f, components),
                          FloatSplat(0.0f, components));
}

spv::Id Translator::Multiply(spv::Id a, spv::Id bValue, spv::Id aAbs, spv::Id bAbs, uint32_t components)
{
    spv::Id type = FloatType(components);
    spv::Id product = b_.createBinOp(spv::OpFMul, type, a, bValue);
    if (!options_.legacyMultiply || a == bValue)
        return product;
    // Shader Model 3 / Xenos: +-0 * anything = +0 (ZeroIfAnyOperandIsZero in the SDK's spirv_translator_alu.cpp).
    spv::Id minimum = Std450(GLSLstd450NMin, type, {aAbs, bAbs});
    spv::Id isZero = b_.createBinOp(spv::OpFOrdEqual, BoolType(components), minimum, FloatSplat(0.0f, components));
    return b_.createTriOp(spv::OpSelect, type, isZero, FloatSplat(0.0f, components), product);
}

spv::Id Translator::Saturate(spv::Id value, uint32_t components)
{
    return Std450(GLSLstd450FClamp, FloatType(components),
                  {value, FloatSplat(0.0f, components), FloatSplat(1.0f, components)});
}

spv::Id Translator::SelectIsNan(spv::Id value, uint32_t components)
{
    spv::Id isNan = b_.createUnaryOp(spv::OpIsNan, BoolType(components), value);
    return b_.createTriOp(spv::OpSelect, FloatType(components), isNan, FloatSplat(0.0f, components), value);
}

// =================================================================================================================
// Registers
// =================================================================================================================

spv::Id Translator::TemporaryVariable(uint32_t reg)
{
    if (reg >= kTemporaryRegisters)
        Fail(fmt::format("temporary register r{} out of range", reg));
    return r_[reg];
}

void Translator::WriteMasked(spv::Id variable, uint32_t mask, spv::Id value, uint32_t valueComponents)
{
    // dst.<mask> = value: value component k goes to the k-th written component (HLSL truncation and swizzle
    // assignment rules). A scalar value is broadcast.
    spv::Id current = b_.createLoad(variable, spv::NoPrecision);
    std::vector<spv::Id> parts(4);
    uint32_t k = 0;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if ((mask >> i) & 1)
        {
            parts[i] = valueComponents == 1 ? Component(value, 0) : Component(value, k);
            ++k;
        }
        else
        {
            parts[i] = b_.createCompositeExtract(current, tFloat_, i);
        }
    }
    if (valueComponents != 1 && k > valueComponents)
        Fail("value narrower than its write mask");
    b_.createStore(b_.createCompositeConstruct(tFloatN_[4], parts), variable);
}

void Translator::WriteComponent(spv::Id variable, uint32_t component, spv::Id scalar)
{
    WriteMasked(variable, 1u << component, scalar, 1);
}

// =================================================================================================================
// Instructions
// =================================================================================================================

void Translator::OpenPredicate(bool condition)
{
    ClosePredicate();
    spv::Id p0 = b_.createLoad(p0_, spv::NoPrecision);
    if (!condition)
        p0 = b_.createUnaryOp(spv::OpLogicalNot, tBool_, p0);
    predicate_ = std::make_unique<spv::Builder::If>(p0, spv::SelectionControlMaskNone, b_);
}

void Translator::ClosePredicate()
{
    if (predicate_)
    {
        predicate_->makeEndIf();
        predicate_.reset();
    }
}

Operand Translator::ResolveOperand(const AluInstruction& instr, uint32_t which)
{
    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    Operand operand;
    uint32_t reg = 0;
    uint32_t swizzle = 0;
    bool select = true;

    switch (which)
    {
    case SCALAR_CONSTANT_0:
        reg = instr.src3Register;
        swizzle = instr.src3Swizzle;
        select = false;
        operand.negate = instr.src3Negate;
        operand.abs = instr.absConstants;
        break;
    case SCALAR_CONSTANT_1:
        reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
        swizzle = instr.src3Swizzle;
        select = true;
        operand.negate = instr.src3Negate;
        operand.abs = instr.absConstants;
        break;
    default:
        switch (which)
        {
        case VECTOR_0:
            reg = instr.src1Register;
            swizzle = instr.src1Swizzle;
            select = instr.src1Select;
            operand.negate = instr.src1Negate;
            break;
        case VECTOR_1:
            reg = instr.src2Register;
            swizzle = instr.src2Swizzle;
            select = instr.src2Select;
            operand.negate = instr.src2Negate;
            break;
        default:
            reg = instr.src3Register;
            swizzle = instr.src3Swizzle;
            select = instr.src3Select;
            operand.negate = instr.src3Negate;
            break;
        }
        if (select)
        {
            operand.abs = (reg & 0x80) != 0;
            reg &= 0x3F;
        }
        else
        {
            operand.abs = instr.absConstants;
        }
        break;
    }

    operand.temporary = select;
    operand.reg = reg;
    if (!select)
    {
        // The bits belong to the first constant and the following ones, not to the whole instruction.
        const bool relative = which == VECTOR_0 ? instr.const0Relative
                              : which == VECTOR_1 ? (instr.src1Select ? instr.const0Relative : instr.const1Relative)
                                                  : (instr.src1Select && instr.src2Select ? instr.const0Relative
                                                                                          : instr.const1Relative);
        if (relative)
            operand.relative = instr.constAddressRegisterRelative ? 1 : 2;
    }

    switch (which)
    {
    case VECTOR_0:
    case VECTOR_1:
    case VECTOR_2:
    {
        uint32_t mask;
        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Dp2Add:
            mask = which == VECTOR_2 ? 0b1 : 0b11;
            break;
        case AluVectorOpcode::Dp3:
            mask = 0b111;
            break;
        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Max4:
            mask = 0b1111;
            break;
        default:
            mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
            break;
        }
        for (uint32_t i = 0; i < 4; i++)
        {
            if ((mask >> i) & 1)
                operand.components.push_back(((swizzle >> (i * 2)) + i) & 0x3);
        }
        break;
    }
    case SCALAR_0:
    case SCALAR_CONSTANT_0:
        operand.components.push_back(((swizzle >> 6) + 3) & 0x3);
        break;
    case SCALAR_1:
    case SCALAR_CONSTANT_1:
        operand.components.push_back(swizzle & 0x3);
        break;
    }
    return operand;
}

spv::Id Translator::LoadOperandAbs(const Operand& operand)
{
    spv::Id vec4 = operand.temporary ? b_.createLoad(TemporaryVariable(operand.reg), spv::NoPrecision)
                                     : LoadConstant(operand.reg, operand.relative);
    spv::Id value = Extract(vec4, operand.components);
    if (operand.abs)
        value = Std450(GLSLstd450FAbs, FloatType(uint32_t(operand.components.size())), {value});
    return value;
}

spv::Id Translator::LoadOperand(const Operand& operand)
{
    spv::Id value = LoadOperandAbs(operand);
    if (operand.negate)
        value = b_.createUnaryOp(spv::OpFNegate, FloatType(uint32_t(operand.components.size())), value);
    return value;
}

void Translator::Kill(spv::Id condition)
{
    // clip(condition ? -1 : 1)
    spv::Builder::If kill(condition, spv::SelectionControlMaskNone, b_);
    b_.makeDiscard();
    kill.makeEndIf();
}

void Translator::TranslateVertexFetch(const VertexFetchInstruction& instr, uint32_t address)
{
    if (instr.isPredicated)
        OpenPredicate(instr.predicateCondition);
    else
        ClosePredicate();

    auto found = vertexElements_.find(address);
    if (found == vertexElements_.end())
        Fail("vertex fetch without a declared element");
    const VertexElement& element = found->second;

    spv::Id input = b_.createLoad(vertexInputs_.at(address), spv::NoPrecision);
    if (vertexInputIsUint_.at(address))
        input = b_.createUnaryOp(spv::OpConvertUToF, tFloatN_[4], input);

    if (element.usage == DeclUsage::TexCoord)
    {
        // tfetchTexcoord: (g_SwappedTexcoords & (1 << index)) != 0 ? value.yxwz : value
        spv::Id swapped = b_.createBinOp(spv::OpBitwiseAnd, tUint_, SharedUint(kSharedSwappedTexcoords),
                                         b_.makeUintConstant(1u << element.usageIndex));
        spv::Id isSwapped = b_.createBinOp(spv::OpINotEqual, tBool_, swapped, b_.makeUintConstant(0));
        spv::Id yxwz = b_.createRvalueSwizzle(spv::NoPrecision, tFloatN_[4], input, {1, 0, 3, 2});
        input = b_.createTriOp(spv::OpSelect, tFloatN_[4], b_.smearScalar(spv::NoPrecision, isSwapped, tBoolN_[4]),
                               yxwz, input);
    }

    // remapInput(value, g_InputRemap(location)): 3 bits per component, 0-3 = data component, 4 = 0, 5 = 1,
    // anything else = unchanged.
    spv::Id code = SharedUint(kSharedInputRemap + vertexInputLocation_.at(address) * 4);
    std::vector<spv::Id> remapped(4);
    for (uint32_t i = 0; i < 4; ++i)
    {
        spv::Id source = b_.createBinOp(spv::OpBitwiseAnd, tUint_,
                                        b_.createBinOp(spv::OpShiftRightLogical, tUint_, code, b_.makeUintConstant(i * 3)),
                                        b_.makeUintConstant(7));
        spv::Id dataIndex = b_.createBinOp(spv::OpBitwiseAnd, tUint_, source, b_.makeUintConstant(3));
        spv::Id data = b_.createVectorExtractDynamic(input, tFloat_, dataIndex);
        spv::Id own = b_.createCompositeExtract(input, tFloat_, i);
        spv::Id value = b_.createTriOp(spv::OpSelect, tFloat_,
                                       b_.createBinOp(spv::OpIEqual, tBool_, source, b_.makeUintConstant(5)),
                                       FloatConst(1.0f), own);
        value = b_.createTriOp(spv::OpSelect, tFloat_,
                               b_.createBinOp(spv::OpIEqual, tBool_, source, b_.makeUintConstant(4)),
                               FloatConst(0.0f), value);
        value = b_.createTriOp(spv::OpSelect, tFloat_,
                               b_.createBinOp(spv::OpULessThan, tBool_, source, b_.makeUintConstant(4)), data, value);
        remapped[i] = value;
    }
    spv::Id value = b_.createCompositeConstruct(tFloatN_[4], remapped);

    // r<dst>.<written> = value.<swizzle>; then the 0 and 1 components.
    spv::Id destination = TemporaryVariable(instr.dstRegister);
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t swizzle = (instr.dstSwizzle >> (i * 3)) & 0x7;
        if (swizzle <= 3)
            WriteComponent(destination, i, b_.createCompositeExtract(value, tFloat_, swizzle));
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t swizzle = (instr.dstSwizzle >> (i * 3)) & 0x7;
        if (swizzle == 4)
            WriteComponent(destination, i, FloatConst(0.0f));
        else if (swizzle == 5)
            WriteComponent(destination, i, FloatConst(1.0f));
    }
}

spv::Id Translator::Tfetch2D(uint32_t samplerRegister, TextureKind kind, spv::Id coord, spv::Id offset,
                             spv::Id invSize)
{
    // tfetch2D in shader_common.h. An OpSampledImage result may only be used in its own block, so the sampled image
    // is made right where each use is.
    spv::Id displacementVariable = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[2]);
    spv::Builder::If branch(SpecBitSet(kSpecConstantInvTextureSize), spv::SelectionControlMaskNone, b_);
    b_.createStore(b_.createBinOp(spv::OpFMul, tFloatN_[2], offset, invSize), displacementVariable);
    branch.makeBeginElse();
    {
        b_.addCapability(spv::CapabilityImageQuery);
        spv::Id image = b_.createUnaryOp(spv::OpImage, tImage2D_, SampledImage(samplerRegister, kind));
        spv::Id size = b_.createOp(spv::OpImageQuerySizeLod, tUintN_[2], {image, b_.makeIntConstant(0)});
        spv::Id sizeFloat = b_.createUnaryOp(spv::OpConvertUToF, tFloatN_[2], size);
        b_.createStore(b_.createBinOp(spv::OpFDiv, tFloatN_[2], offset, sizeFloat), displacementVariable);
    }
    branch.makeEndIf();
    spv::Id displacement = b_.createLoad(displacementVariable, spv::NoPrecision);
    spv::Id location = b_.createBinOp(spv::OpFAdd, tFloatN_[2], coord, displacement);
    return b_.createBinOp(spv::OpImageSampleImplicitLod, tFloatN_[4], SampledImage(samplerRegister, kind), location);
}

void Translator::TranslateTextureFetch(const TextureFetchInstruction& instr)
{
    if (instr.opcode != FetchOpcode::TextureFetch)
    {
        if (instr.opcode == FetchOpcode::GetTextureWeights)
            Fail("GetTextureWeights is not supported (the HLSL path cannot compile it either)");
        result_.notes.push_back(fmt::format("texture fetch opcode {} skipped, as in the HLSL path", uint32_t(instr.opcode)));
        return;
    }
    if (!pixel_)
        Fail("texture fetch in a vertex shader (implicit LOD is only valid in pixel shaders)");

    if (instr.isPredicated)
        OpenPredicate(instr.predCondition);
    else
        ClosePredicate();

    auto found = samplers_.find(instr.constIndex);
    if (found == samplers_.end())
        Fail(fmt::format("texture fetch from undeclared sampler s{}", uint32_t(instr.constIndex)));
    const std::string& name = found->second;

    auto sourceComponents = [&](uint32_t count) {
        std::vector<uint32_t> components;
        for (uint32_t i = 0; i < count; i++)
            components.push_back((instr.srcSwizzle >> (i * 2)) & 0x3);
        return Extract(b_.createLoad(TemporaryVariable(instr.srcRegister), spv::NoPrecision), components);
    };

    spv::Id sample = 0;
    switch (instr.dimension)
    {
    case TextureDimension::Texture2D:
    {
        spv::Id coord = sourceComponents(2);
        spv::Id offset = b_.makeCompositeConstant(
            tFloatN_[2], {FloatConst(instr.offsetX * 0.5f), FloatConst(instr.offsetY * 0.5f)});
        spv::Id invSize = SharedFloat2(kSharedInvTextureSize + instr.constIndex * 8);

        if (name == "SHADOWMAP_SAMPLER")
        {
            // tfetch2DSombraMin(tex2D, tex3D (the pair), sampler, coord, offset, invSize)
            ++result_.shadowFetches;
            spv::Id resultVariable = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[4]);
            spv::Id isMarker = b_.createBinOp(spv::OpIEqual, tBool_, specConstants_, b_.makeUintConstant(kShadowMinimumMarker));
            spv::Builder::If marker(isMarker, spv::SelectionControlMaskNone, b_);
            b_.createStore(FloatSplat(0.0f, 4), resultVariable);
            marker.makeBeginElse();
            {
                // tfetch2DSombra: with SPEC_CONSTANT_PCF_BARATO the offset is zero.
                spv::Id cheap = SpecBitSet(kSpecConstantCheapPcf);
                spv::Id shadowOffset = b_.createTriOp(spv::OpSelect, tFloatN_[2],
                                                      b_.smearScalar(spv::NoPrecision, cheap, tBoolN_[2]),
                                                      FloatSplat(0.0f, 2), offset);
                // Two branches as in the HLSL ([branch] if), so the dead one disappears with the specialization.
                spv::Id shadowVariable = b_.createVariable(spv::NoPrecision, spv::StorageClassFunction, tFloatN_[4]);
                b_.createStore(Tfetch2D(instr.constIndex, TextureKind::Texture2D, coord, shadowOffset, invSize),
                               shadowVariable);
                spv::Builder::If minimum(SpecBitSet(kSpecConstantShadowMinimum), spv::SelectionControlMaskNone, b_);
                {
                    spv::Id pair =
                        Tfetch2D(instr.constIndex, TextureKind::ShadowPair, coord, shadowOffset, invSize);
                    spv::Id current = b_.createLoad(shadowVariable, spv::NoPrecision);
                    b_.createStore(Std450(GLSLstd450FMin, tFloatN_[4], {current, pair}), shadowVariable);
                }
                minimum.makeEndIf();
                b_.createStore(b_.createLoad(shadowVariable, spv::NoPrecision), resultVariable);
            }
            marker.makeEndIf();
            sample = b_.createLoad(resultVariable, spv::NoPrecision);
        }
        else
        {
            sample = Tfetch2D(instr.constIndex, TextureKind::Texture2D, coord, offset, invSize);
        }
        break;
    }
    case TextureDimension::Texture3D:
    case TextureDimension::TextureCube:
    {
        const bool cube = instr.dimension == TextureDimension::TextureCube;
        spv::Id coord = sourceComponents(3);
        spv::Id sampledImage = SampledImage(instr.constIndex, cube ? TextureKind::TextureCube : TextureKind::Texture3D);
        if (cube)
        {
            // tfetchCube: the direction is the one cube() stored at index texCoord.z.
            spv::Id index = b_.createUnaryOp(spv::OpConvertFToU, tUint_, b_.createCompositeExtract(coord, tFloat_, 2));
            spv::Id first = b_.createLoad(cubeDirections_[0], spv::NoPrecision);
            spv::Id second = b_.createLoad(cubeDirections_[1], spv::NoPrecision);
            spv::Id isFirst = b_.createBinOp(spv::OpIEqual, tBool_, index, b_.makeUintConstant(0));
            coord = b_.createTriOp(spv::OpSelect, tFloatN_[3], b_.smearScalar(spv::NoPrecision, isFirst, tBoolN_[3]),
                                   first, second);
        }
        sample = b_.createBinOp(spv::OpImageSampleImplicitLod, tFloatN_[4], sampledImage, coord);
        break;
    }
    default:
        Fail("1D texture fetch is not supported (the HLSL path cannot compile it either)");
    }

    // r<dst>.<written> = sample.<swizzle>; then the 0 and 1 components.
    spv::Id destination = TemporaryVariable(instr.dstRegister);
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t swizzle = (instr.dstSwizzle >> (i * 3)) & 0x7;
        if (swizzle <= 3)
            WriteComponent(destination, i, b_.createCompositeExtract(sample, tFloat_, swizzle));
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t swizzle = (instr.dstSwizzle >> (i * 3)) & 0x7;
        if (swizzle == 4)
            WriteComponent(destination, i, FloatConst(0.0f));
        else if (swizzle == 5)
            WriteComponent(destination, i, FloatConst(1.0f));
    }
}

void Translator::TranslateAlu(const AluInstruction& instr)
{
    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    if (instr.isPredicated)
        OpenPredicate(instr.predicateCondition);
    else
        ClosePredicate();

    // Vector kills: clip(any(a <op> b) ? -1 : 1), before anything else.
    if (instr.vectorOpcode >= AluVectorOpcode::KillEq && instr.vectorOpcode <= AluVectorOpcode::KillNe)
    {
        Operand a = ResolveOperand(instr, VECTOR_0), bOperand = ResolveOperand(instr, VECTOR_1);
        const uint32_t n = uint32_t(a.components.size());
        spv::Op compare = instr.vectorOpcode == AluVectorOpcode::KillEq   ? spv::OpFOrdEqual
                          : instr.vectorOpcode == AluVectorOpcode::KillGt ? spv::OpFOrdGreaterThan
                          : instr.vectorOpcode == AluVectorOpcode::KillGe ? spv::OpFOrdGreaterThanEqual
                                                                          : spv::OpFUnordNotEqual;
        spv::Id result = b_.createBinOp(compare, BoolType(n), LoadOperand(a), LoadOperand(bOperand));
        if (n > 1)
            result = b_.createUnaryOp(spv::OpAny, tBool_, result);
        Kill(result);
    }

    // Destination of exports.
    spv::Id exportVariable = 0;
    if (instr.exportData)
    {
        if (pixel_)
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
            case ExportRegister::PSColor1:
            case ExportRegister::PSColor2:
            case ExportRegister::PSColor3:
            {
                const uint32_t index = instr.vectorDest;
                if (!oC_[index])
                    Fail(fmt::format("export to oC{} that the container does not declare", index));
                exportVariable = oC_[index];
                break;
            }
            case ExportRegister::PSDepth:
                Fail("depth export is not supported");
            default:
                // Memory export (eA/eM registers) or an unknown export: the HLSL path writes r<vectorDest>
                // instead, so the export is lost. Listed, so it is not dropped silently.
                result_.notes.push_back(fmt::format("export to register {} written to r{} (memory export is not "
                                                    "supported), as in the HLSL path",
                                                    uint32_t(instr.vectorDest), uint32_t(instr.vectorDest)));
                break;
            }
        }
        else
        {
            if (ExportRegister(instr.vectorDest) == ExportRegister::VSPosition)
            {
                exportVariable = oPos_;
            }
            else
            {
                auto found = exportInterpolators_.find(instr.vectorDest);
                if (found == exportInterpolators_.end())
                    Fail("vertex export without a declared interpolator");
                exportVariable = oInterp_[found->second];
            }
        }
    }

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        // p0 = a == 0.0 && b <op> 0.0;
        Operand a = ResolveOperand(instr, VECTOR_0), bOperand = ResolveOperand(instr, VECTOR_1);
        if (a.components.size() != 1)
            Fail("vector predicate push with more than one component");
        spv::Op compare = instr.vectorOpcode == AluVectorOpcode::SetpEqPush   ? spv::OpFOrdEqual
                          : instr.vectorOpcode == AluVectorOpcode::SetpNePush ? spv::OpFUnordNotEqual
                          : instr.vectorOpcode == AluVectorOpcode::SetpGtPush ? spv::OpFOrdGreaterThan
                                                                              : spv::OpFOrdGreaterThanEqual;
        spv::Id first = b_.createBinOp(spv::OpFOrdEqual, tBool_, LoadOperand(a), FloatConst(0.0f));
        spv::Id second = b_.createBinOp(compare, tBool_, LoadOperand(bOperand), FloatConst(0.0f));
        b_.createStore(b_.createBinOp(spv::OpLogicalAnd, tBool_, first, second), p0_);
    }
    else if (instr.vectorOpcode == AluVectorOpcode::MaxA)
    {
        // a0 = (int)clamp(floor((a).w + 0.5), -256.0, 255.0);
        Operand a = ResolveOperand(instr, VECTOR_0);
        if (a.components.size() != 4)
            Fail("MaxA without a full write mask");
        spv::Id w = b_.createCompositeExtract(LoadOperand(a), tFloat_, 3);
        spv::Id value = Std450(GLSLstd450Floor, tFloat_, {b_.createBinOp(spv::OpFAdd, tFloat_, w, FloatConst(0.5f))});
        value = Std450(GLSLstd450FClamp, tFloat_, {value, FloatConst(-256.0f), FloatConst(255.0f)});
        b_.createStore(b_.createUnaryOp(spv::OpConvertFToS, tInt_, value), a0_);
    }

    uint32_t vectorWriteMask = instr.vectorWriteMask;
    if (instr.exportData)
        vectorWriteMask &= ~instr.scalarWriteMask;

    if (vectorWriteMask != 0)
    {
        const uint32_t operandComponents = instr.vectorWriteMask != 0 ? uint32_t(__builtin_popcount(instr.vectorWriteMask)) : 1;
        bool broadcast = false;
        spv::Id value = VectorOperation(instr, operandComponents, broadcast);
        const uint32_t valueComponents = uint32_t(b_.getNumComponents(value));
        if (instr.vectorSaturate)
            value = Saturate(value, valueComponents);
        spv::Id destination = exportVariable ? exportVariable : TemporaryVariable(instr.vectorDest);
        WriteMasked(destination, vectorWriteMask, value, valueComponents);
    }

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            spv::Id predicate = 0;
            if (instr.scalarOpcode == AluScalarOpcode::SetpClr)
            {
                predicate = b_.makeBoolConstant(false);
            }
            else
            {
                spv::Id a = LoadOperand(ResolveOperand(instr, SCALAR_0));
                switch (instr.scalarOpcode)
                {
                case AluScalarOpcode::SetpEq:
                case AluScalarOpcode::SetpRstr:
                    predicate = b_.createBinOp(spv::OpFOrdEqual, tBool_, a, FloatConst(0.0f));
                    break;
                case AluScalarOpcode::SetpNe:
                    predicate = b_.createBinOp(spv::OpFUnordNotEqual, tBool_, a, FloatConst(0.0f));
                    break;
                case AluScalarOpcode::SetpGt:
                    predicate = b_.createBinOp(spv::OpFOrdGreaterThan, tBool_, a, FloatConst(0.0f));
                    break;
                case AluScalarOpcode::SetpGe:
                    predicate = b_.createBinOp(spv::OpFOrdGreaterThanEqual, tBool_, a, FloatConst(0.0f));
                    break;
                case AluScalarOpcode::SetpInv:
                    predicate = b_.createBinOp(spv::OpFOrdEqual, tBool_, a, FloatConst(1.0f));
                    break;
                case AluScalarOpcode::SetpPop:
                    predicate = b_.createBinOp(spv::OpFOrdLessThanEqual, tBool_,
                                               b_.createBinOp(spv::OpFSub, tFloat_, a, FloatConst(1.0f)), FloatConst(0.0f));
                    break;
                default:
                    break;
                }
            }
            b_.createStore(predicate, p0_);
        }

        spv::Id value = ScalarOperation(instr);
        if (instr.scalarSaturate)
            value = Saturate(value, 1);
        b_.createStore(value, ps_);

        if (instr.scalarOpcode == AluScalarOpcode::MaxAs || instr.scalarOpcode == AluScalarOpcode::MaxAsf)
        {
            // a0 = (int)clamp(floor(a [+ 0.5]), -256.0, 255.0);
            spv::Id a = LoadOperand(ResolveOperand(instr, SCALAR_0));
            if (instr.scalarOpcode == AluScalarOpcode::MaxAs)
                a = b_.createBinOp(spv::OpFAdd, tFloat_, a, FloatConst(0.5f));
            spv::Id clamped = Std450(GLSLstd450FClamp, tFloat_,
                                     {Std450(GLSLstd450Floor, tFloat_, {a}), FloatConst(-256.0f), FloatConst(255.0f)});
            b_.createStore(b_.createUnaryOp(spv::OpConvertFToS, tInt_, clamped), a0_);
        }
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;
    if (scalarWriteMask != 0)
    {
        spv::Id destination = exportVariable ? exportVariable : TemporaryVariable(instr.scalarDest);
        WriteMasked(destination, scalarWriteMask, b_.createLoad(ps_, spv::NoPrecision), 1);
    }

    if (instr.exportData)
    {
        const uint32_t zeroMask =
            instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        const uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;
        if ((zeroMask | oneMask) != 0 && !exportVariable)
            Fail("constant export components without an export register");
        for (uint32_t i = 0; i < 4; i++)
        {
            if (zeroMask & (1u << i))
                WriteComponent(exportVariable, i, FloatConst(0.0f));
            else if (oneMask & (1u << i))
                WriteComponent(exportVariable, i, FloatConst(1.0f));
        }
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        // clip(ps != 0.0 ? -1 : 1)
        Kill(b_.createBinOp(spv::OpFUnordNotEqual, tBool_, b_.createLoad(ps_, spv::NoPrecision), FloatConst(0.0f)));
    }
}

spv::Id Translator::VectorOperation(const AluInstruction& instr, uint32_t components, bool& handled)
{
    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2
    };
    (void)handled;
    spv::Id type = FloatType(components);
    auto operand = [&](uint32_t which) { return ResolveOperand(instr, which); };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::Add:
        return b_.createBinOp(spv::OpFAdd, type, LoadOperand(operand(VECTOR_0)), LoadOperand(operand(VECTOR_1)));

    case AluVectorOpcode::Mul:
    {
        Operand a = operand(VECTOR_0), c = operand(VECTOR_1);
        return Multiply(LoadOperand(a), LoadOperand(c), LoadOperandAbs(a), LoadOperandAbs(c), components);
    }

    case AluVectorOpcode::Max:
    case AluVectorOpcode::MaxA:
    case AluVectorOpcode::Min:
    {
        // The Xenos MOV is MAX dst, src, src: the operand is copied as is.
        Operand a = operand(VECTOR_0), c = operand(VECTOR_1);
        if (a == c)
            return LoadOperand(a);
        return Std450(instr.vectorOpcode == AluVectorOpcode::Min ? GLSLstd450FMin : GLSLstd450FMax, type,
                      {LoadOperand(a), LoadOperand(c)});
    }

    case AluVectorOpcode::Seq:
    case AluVectorOpcode::Sgt:
    case AluVectorOpcode::Sge:
    case AluVectorOpcode::Sne:
    {
        spv::Op compare = instr.vectorOpcode == AluVectorOpcode::Seq   ? spv::OpFOrdEqual
                          : instr.vectorOpcode == AluVectorOpcode::Sgt ? spv::OpFOrdGreaterThan
                          : instr.vectorOpcode == AluVectorOpcode::Sge ? spv::OpFOrdGreaterThanEqual
                                                                       : spv::OpFUnordNotEqual;
        return BoolToFloat(
            b_.createBinOp(compare, BoolType(components), LoadOperand(operand(VECTOR_0)), LoadOperand(operand(VECTOR_1))),
            components);
    }

    case AluVectorOpcode::Frc:
        return Std450(GLSLstd450Fract, type, {LoadOperand(operand(VECTOR_0))});
    case AluVectorOpcode::Trunc:
        return Std450(GLSLstd450Trunc, type, {LoadOperand(operand(VECTOR_0))});
    case AluVectorOpcode::Floor:
        return Std450(GLSLstd450Floor, type, {LoadOperand(operand(VECTOR_0))});

    case AluVectorOpcode::Mad:
    {
        Operand a = operand(VECTOR_0), c = operand(VECTOR_1), d = operand(VECTOR_2);
        auto computeMad = [&] {
            spv::Id product = Multiply(LoadOperand(a), LoadOperand(c), LoadOperandAbs(a), LoadOperandAbs(c), components);
            return b_.createBinOp(spv::OpFAdd, type, product, LoadOperand(d));
        };
        // The composite's radial blur blend: r5.xyz = r5.xyz * r0.xxx + r3.xyz.
        const bool blurBlend = options_.compositionBlurSwitch && !instr.exportData && instr.vectorDest == 5 &&
                               instr.vectorWriteMask == 0b0111 && !instr.vectorSaturate && a.temporary && a.reg == 5 &&
                               !a.negate && !a.abs && a.components == std::vector<uint32_t>{0, 1, 2} && c.temporary &&
                               c.reg == 0 && !c.negate && !c.abs && c.components == std::vector<uint32_t>{0, 0, 0} &&
                               d.temporary && d.reg == 3 && !d.negate && !d.abs &&
                               d.components == std::vector<uint32_t>{0, 1, 2};
        if (!blurBlend)
            return computeMad();
        ++blurMatches_;
        // (g_SpecConstants() & SPEC_CONSTANT_SIN_DESENFOQUE) ? r3.xyz : (r5.xyz * r0.xxx + r3.xyz)
        spv::Id center = LoadOperand(d);
        spv::Id blended = computeMad();
        return b_.createTriOp(spv::OpSelect, type,
                              b_.smearScalar(spv::NoPrecision, SpecBitSet(kSpecConstantNoBlur), BoolType(components)),
                              center, blended);
    }

    case AluVectorOpcode::CndEq:
    case AluVectorOpcode::CndGe:
    case AluVectorOpcode::CndGt:
    {
        spv::Op compare = instr.vectorOpcode == AluVectorOpcode::CndEq   ? spv::OpFOrdEqual
                          : instr.vectorOpcode == AluVectorOpcode::CndGe ? spv::OpFOrdGreaterThanEqual
                                                                         : spv::OpFOrdGreaterThan;
        spv::Id condition = b_.createBinOp(compare, BoolType(components), LoadOperand(operand(VECTOR_0)),
                                           FloatSplat(0.0f, components));
        return b_.createTriOp(spv::OpSelect, type, condition, LoadOperand(operand(VECTOR_1)),
                              LoadOperand(operand(VECTOR_2)));
    }

    case AluVectorOpcode::Dp4:
    case AluVectorOpcode::Dp3:
    case AluVectorOpcode::Dp2Add:
    {
        Operand a = operand(VECTOR_0), c = operand(VECTOR_1);
        const uint32_t n = uint32_t(a.components.size());
        spv::Id result = 0;
        if (!options_.legacyMultiply)
        {
            result = b_.createBinOp(spv::OpDot, tFloat_, LoadOperand(a), LoadOperand(c));
        }
        else
        {
            spv::Id av = LoadOperand(a), cv = LoadOperand(c), aa = LoadOperandAbs(a), ca = LoadOperandAbs(c);
            spv::Id products = Multiply(av, cv, aa, ca, n);
            result = Component(products, 0);
            for (uint32_t i = 1; i < n; ++i)
                result = b_.createBinOp(spv::OpFAdd, tFloat_, result, Component(products, i));
        }
        if (instr.vectorOpcode == AluVectorOpcode::Dp2Add)
            result = b_.createBinOp(spv::OpFAdd, tFloat_, result, LoadOperand(operand(VECTOR_2)));
        return result;  // scalar: broadcast to the write mask
    }

    case AluVectorOpcode::Cube:
    {
        if (!pixel_)
            Fail("cube instruction in a vertex shader");
        // cube(r<src1>, cubeMapData): stores the direction, returns float4(0, 0, 0, index).
        spv::Id source = b_.createLoad(TemporaryVariable(instr.src1Register), spv::NoPrecision);
        spv::Id direction = b_.createRvalueSwizzle(spv::NoPrecision, tFloatN_[3], source, {0, 1, 2});
        spv::Id index = b_.createLoad(cubeIndex_, spv::NoPrecision);
        spv::Id isFirst = b_.createBinOp(spv::OpIEqual, tBool_, index, b_.makeUintConstant(0));
        spv::Builder::If store(isFirst, spv::SelectionControlMaskNone, b_);
        b_.createStore(direction, cubeDirections_[0]);
        store.makeBeginElse();
        b_.createStore(direction, cubeDirections_[1]);
        store.makeEndIf();
        b_.createStore(b_.createBinOp(spv::OpIAdd, tUint_, index, b_.makeUintConstant(1)), cubeIndex_);
        spv::Id indexFloat = b_.createUnaryOp(spv::OpConvertUToF, tFloat_, index);
        return b_.createCompositeConstruct(tFloatN_[4], {FloatConst(0.0f), FloatConst(0.0f), FloatConst(0.0f), indexFloat});
    }

    case AluVectorOpcode::Max4:
    {
        spv::Id value = LoadOperand(operand(VECTOR_0));
        spv::Id xy = Std450(GLSLstd450FMax, tFloat_, {Component(value, 0), Component(value, 1)});
        spv::Id zw = Std450(GLSLstd450FMax, tFloat_, {Component(value, 2), Component(value, 3)});
        return Std450(GLSLstd450FMax, tFloat_, {xy, zw});
    }

    case AluVectorOpcode::SetpEqPush:
    case AluVectorOpcode::SetpNePush:
    case AluVectorOpcode::SetpGtPush:
    case AluVectorOpcode::SetpGePush:
    {
        // p0 ? 0.0 : a + 1.0
        spv::Id a = LoadOperand(operand(VECTOR_0));
        spv::Id p0 = b_.createLoad(p0_, spv::NoPrecision);
        return b_.createTriOp(spv::OpSelect, type, components == 1 ? p0 : b_.smearScalar(spv::NoPrecision, p0, BoolType(components)),
                              FloatSplat(0.0f, components),
                              b_.createBinOp(spv::OpFAdd, type, a, FloatSplat(1.0f, components)));
    }

    case AluVectorOpcode::KillEq:
    case AluVectorOpcode::KillGt:
    case AluVectorOpcode::KillGe:
    case AluVectorOpcode::KillNe:
    {
        spv::Op compare = instr.vectorOpcode == AluVectorOpcode::KillEq   ? spv::OpFOrdEqual
                          : instr.vectorOpcode == AluVectorOpcode::KillGt ? spv::OpFOrdGreaterThan
                          : instr.vectorOpcode == AluVectorOpcode::KillGe ? spv::OpFOrdGreaterThanEqual
                                                                          : spv::OpFUnordNotEqual;
        spv::Id result = b_.createBinOp(compare, BoolType(components), LoadOperand(operand(VECTOR_0)),
                                        LoadOperand(operand(VECTOR_1)));
        if (components > 1)
            result = b_.createUnaryOp(spv::OpAny, tBool_, result);
        return BoolToFloat(result, 1);
    }

    case AluVectorOpcode::Dst:
    {
        Operand a = operand(VECTOR_0), c = operand(VECTOR_1);
        if (a.components.size() != 4)
            Fail("dst without a full write mask");
        spv::Id av = LoadOperand(a), cv = LoadOperand(c);
        return b_.createCompositeConstruct(
            tFloatN_[4], {FloatConst(1.0f),
                          b_.createBinOp(spv::OpFMul, tFloat_, Component(av, 1), Component(cv, 1)),
                          Component(av, 2), Component(cv, 3)});
    }
    }
    Fail(fmt::format("vector opcode {} is not supported", uint32_t(instr.vectorOpcode)));
}

spv::Id Translator::ScalarOperation(const AluInstruction& instr)
{
    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };
    auto operand = [&](uint32_t which) { return ResolveOperand(instr, which); };
    auto a = [&] { return LoadOperand(operand(SCALAR_0)); };
    auto ps = [&] { return b_.createLoad(ps_, spv::NoPrecision); };
    auto clampFlt = [&](spv::Id value) {
        return Std450(GLSLstd450FClamp, tFloat_, {value, FloatConst(kNegativeFltMax), FloatConst(FLT_MAX)});
    };
    auto multiplyWithPs = [&] {
        Operand x = operand(SCALAR_0);
        spv::Id previous = ps();
        return Multiply(LoadOperand(x), previous, LoadOperandAbs(x), Std450(GLSLstd450FAbs, tFloat_, {previous}), 1);
    };

    switch (instr.scalarOpcode)
    {
    case AluScalarOpcode::Adds:
        return b_.createBinOp(spv::OpFAdd, tFloat_, a(), LoadOperand(operand(SCALAR_1)));
    case AluScalarOpcode::AddsPrev:
        return b_.createBinOp(spv::OpFAdd, tFloat_, a(), ps());
    case AluScalarOpcode::Muls:
    {
        Operand x = operand(SCALAR_0), y = operand(SCALAR_1);
        return Multiply(LoadOperand(x), LoadOperand(y), LoadOperandAbs(x), LoadOperandAbs(y), 1);
    }
    case AluScalarOpcode::MulsPrev:
    case AluScalarOpcode::MulsPrev2:
        return multiplyWithPs();
    case AluScalarOpcode::Maxs:
    case AluScalarOpcode::MaxAs:
    case AluScalarOpcode::MaxAsf:
    case AluScalarOpcode::Mins:
    {
        Operand x = operand(SCALAR_0), y = operand(SCALAR_1);
        if (x == y)
            return LoadOperand(x);
        return Std450(instr.scalarOpcode == AluScalarOpcode::Mins ? GLSLstd450FMin : GLSLstd450FMax, tFloat_,
                      {LoadOperand(x), LoadOperand(y)});
    }
    case AluScalarOpcode::Seqs:
        return BoolToFloat(b_.createBinOp(spv::OpFOrdEqual, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::Sgts:
        return BoolToFloat(b_.createBinOp(spv::OpFOrdGreaterThan, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::Sges:
        return BoolToFloat(b_.createBinOp(spv::OpFOrdGreaterThanEqual, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::Snes:
        return BoolToFloat(b_.createBinOp(spv::OpFUnordNotEqual, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::Frcs:
        return Std450(GLSLstd450Fract, tFloat_, {a()});
    case AluScalarOpcode::Truncs:
        return Std450(GLSLstd450Trunc, tFloat_, {a()});
    case AluScalarOpcode::Floors:
        return Std450(GLSLstd450Floor, tFloat_, {a()});
    case AluScalarOpcode::Exp:
        return Std450(GLSLstd450Exp2, tFloat_, {a()});
    case AluScalarOpcode::Logc:
    case AluScalarOpcode::Log:
        return clampFlt(Std450(GLSLstd450Log2, tFloat_, {a()}));
    case AluScalarOpcode::Rcpc:
    case AluScalarOpcode::Rcpf:
    case AluScalarOpcode::Rcp:
        return clampFlt(b_.createBinOp(spv::OpFDiv, tFloat_, FloatConst(1.0f), a()));
    case AluScalarOpcode::Rsqc:
    case AluScalarOpcode::Rsqf:
    case AluScalarOpcode::Rsq:
        return clampFlt(Std450(GLSLstd450InverseSqrt, tFloat_, {a()}));
    case AluScalarOpcode::Subs:
        return b_.createBinOp(spv::OpFSub, tFloat_, a(), LoadOperand(operand(SCALAR_1)));
    case AluScalarOpcode::SubsPrev:
        return b_.createBinOp(spv::OpFSub, tFloat_, a(), ps());
    case AluScalarOpcode::SetpEq:
    case AluScalarOpcode::SetpNe:
    case AluScalarOpcode::SetpGt:
    case AluScalarOpcode::SetpGe:
        return b_.createTriOp(spv::OpSelect, tFloat_, b_.createLoad(p0_, spv::NoPrecision), FloatConst(0.0f),
                              FloatConst(1.0f));
    case AluScalarOpcode::SetpInv:
    {
        // p0 ? 0.0 : (a == 0.0 ? 1.0 : a)
        spv::Id x = a();
        spv::Id inner = b_.createTriOp(spv::OpSelect, tFloat_,
                                       b_.createBinOp(spv::OpFOrdEqual, tBool_, x, FloatConst(0.0f)), FloatConst(1.0f), x);
        return b_.createTriOp(spv::OpSelect, tFloat_, b_.createLoad(p0_, spv::NoPrecision), FloatConst(0.0f), inner);
    }
    case AluScalarOpcode::SetpPop:
        return b_.createTriOp(spv::OpSelect, tFloat_, b_.createLoad(p0_, spv::NoPrecision), FloatConst(0.0f),
                              b_.createBinOp(spv::OpFSub, tFloat_, a(), FloatConst(1.0f)));
    case AluScalarOpcode::SetpClr:
        return FloatConst(FLT_MAX);
    case AluScalarOpcode::SetpRstr:
        return b_.createTriOp(spv::OpSelect, tFloat_, b_.createLoad(p0_, spv::NoPrecision), FloatConst(0.0f), a());
    case AluScalarOpcode::KillsEq:
        return BoolToFloat(b_.createBinOp(spv::OpFOrdEqual, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::KillsGt:
        return BoolToFloat(b_.createBinOp(spv::OpFOrdGreaterThan, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::KillsGe:
        return BoolToFloat(b_.createBinOp(spv::OpFOrdGreaterThanEqual, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::KillsNe:
        return BoolToFloat(b_.createBinOp(spv::OpFUnordNotEqual, tBool_, a(), FloatConst(0.0f)), 1);
    case AluScalarOpcode::KillsOne:
        return BoolToFloat(b_.createBinOp(spv::OpFOrdEqual, tBool_, a(), FloatConst(1.0f)), 1);
    case AluScalarOpcode::Sqrt:
        return Std450(GLSLstd450Sqrt, tFloat_, {a()});
    case AluScalarOpcode::Mulsc0:
    case AluScalarOpcode::Mulsc1:
    {
        Operand x = operand(SCALAR_CONSTANT_0), y = operand(SCALAR_CONSTANT_1);
        return Multiply(LoadOperand(x), LoadOperand(y), LoadOperandAbs(x), LoadOperandAbs(y), 1);
    }
    case AluScalarOpcode::Addsc0:
    case AluScalarOpcode::Addsc1:
        return b_.createBinOp(spv::OpFAdd, tFloat_, LoadOperand(operand(SCALAR_CONSTANT_0)),
                              LoadOperand(operand(SCALAR_CONSTANT_1)));
    case AluScalarOpcode::Subsc0:
    case AluScalarOpcode::Subsc1:
        return b_.createBinOp(spv::OpFSub, tFloat_, LoadOperand(operand(SCALAR_CONSTANT_0)),
                              LoadOperand(operand(SCALAR_CONSTANT_1)));
    case AluScalarOpcode::Sin:
        return Std450(GLSLstd450Sin, tFloat_, {a()});
    case AluScalarOpcode::Cos:
        return Std450(GLSLstd450Cos, tFloat_, {a()});
    default:
        break;
    }
    Fail(fmt::format("scalar opcode {} is not supported", uint32_t(instr.scalarOpcode)));
}

// =================================================================================================================
// Control flow
// =================================================================================================================

void Translator::OpenIf(spv::Id condition)
{
    OpenConstruct construct;
    construct.ifBuilder = std::make_unique<spv::Builder::If>(condition, spv::SelectionControlMaskNone, b_);
    open_.push_back(std::move(construct));
}

void Translator::OpenLoop(uint32_t loopId)
{
    // for (aL = 0; aL < i<loopId>.x; aL++)
    auto definition = int4Definitions_.find(loopId);
    if (definition == int4Definitions_.end())
        Fail(fmt::format("loop over undefined integer constant i{}", loopId));
    b_.createStore(b_.makeIntConstant(0), aL_);
    spv::Builder::LoopBlocks& blocks = b_.makeNewLoop();
    b_.createBranch(&blocks.head);
    b_.setBuildPoint(&blocks.head);
    spv::Id condition = b_.createBinOp(spv::OpSLessThan, tBool_, b_.createLoad(aL_, spv::NoPrecision),
                                       b_.makeIntConstant(definition->second[0]));
    b_.createLoopMerge(&blocks.merge, &blocks.continue_target, spv::LoopControlMaskNone, {});
    b_.createConditionalBranch(condition, &blocks.body, &blocks.merge);
    b_.setBuildPoint(&blocks.body);
    OpenConstruct construct;
    construct.loop = true;
    construct.loopBlocks = &blocks;
    open_.push_back(std::move(construct));
}

void Translator::CloseInnermost()
{
    // A `}` in the HLSL text closes whatever brace is innermost.
    if (open_.empty())
        Fail("control flow closes more blocks than it opened");
    OpenConstruct construct = std::move(open_.back());
    open_.pop_back();
    if (!construct.loop)
    {
        construct.ifBuilder->makeEndIf();
        return;
    }
    spv::Builder::LoopBlocks& blocks = *construct.loopBlocks;
    b_.createBranch(&blocks.continue_target);
    b_.setBuildPoint(&blocks.continue_target);
    b_.createStore(b_.createBinOp(spv::OpIAdd, tInt_, b_.createLoad(aL_, spv::NoPrecision), b_.makeIntConstant(1)),
                   aL_);
    b_.createBranch(&blocks.head);
    b_.setBuildPoint(&blocks.merge);
    b_.closeLoop();
}

void Translator::WriteStageOutputs()
{
    if (pixel_)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            if (outColors_[i])
                b_.createStore(b_.createLoad(oC_[i], spv::NoPrecision), outColors_[i]);
        }
        return;
    }
    // -fvk-invert-y: SV_Position.y is negated on its way to the stage output.
    spv::Id position = b_.createLoad(oPos_, spv::NoPrecision);
    spv::Id y = b_.createUnaryOp(spv::OpFNegate, tFloat_, b_.createCompositeExtract(position, tFloat_, 1));
    position = b_.createCompositeInsert(y, position, tFloatN_[4], 1);
    b_.createStore(position, outPosition_);
    for (uint32_t i = 0; i < kInterpolatorCount; ++i)
        b_.createStore(b_.createLoad(oInterp_[i], spv::NoPrecision), outInterpolators_[i]);
}

void Translator::EmitReturn()
{
    WriteStageOutputs();
    b_.makeReturn(false);
}

void Translator::EmitShaderEnd()
{
    if (pixel_)
    {
        if (options_.sanitizeColorOutputs)
        {
            for (uint32_t i = 0; i < 4; ++i)
            {
                if (oC_[i])
                    b_.createStore(SelectIsNan(b_.createLoad(oC_[i], spv::NoPrecision), 4), oC_[i]);
            }
        }
        // [branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST) { clip(alphaTestValue(oC0.w)); }
        spv::Builder::If alphaTest(SpecBitSet(kSpecConstantAlphaTest), spv::SelectionControlMaskNone, b_);
        {
            spv::Id alpha = b_.createCompositeExtract(b_.createLoad(oC_[0], spv::NoPrecision), tFloat_, 3);
            spv::Id threshold = SharedFloat(kSharedAlphaThreshold);
            spv::Id function = b_.createBinOp(
                spv::OpShiftRightLogical, tUint_,
                b_.createBinOp(spv::OpBitwiseAnd, tUint_, specConstants_, b_.makeUintConstant(kSpecConstantAlphaFuncMask)),
                b_.makeUintConstant(kSpecConstantAlphaFuncShift));
            // pass: 0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always.
            auto is = [&](uint32_t value) {
                return b_.createBinOp(spv::OpIEqual, tBool_, function, b_.makeUintConstant(value));
            };
            spv::Id pass = b_.makeBoolConstant(true);
            const std::pair<uint32_t, spv::Op> comparisons[] = {
                {1, spv::OpFOrdLessThan},    {2, spv::OpFOrdEqual},         {3, spv::OpFOrdLessThanEqual},
                {4, spv::OpFOrdGreaterThan}, {5, spv::OpFUnordNotEqual},    {6, spv::OpFOrdGreaterThanEqual}};
            for (const auto& [value, op] : comparisons)
                pass = b_.createTriOp(spv::OpSelect, tBool_, is(value), b_.createBinOp(op, tBool_, alpha, threshold), pass);
            pass = b_.createTriOp(spv::OpSelect, tBool_, is(0), b_.makeBoolConstant(false), pass);
            Kill(b_.createUnaryOp(spv::OpLogicalNot, tBool_, pass));
        }
        alphaTest.makeEndIf();
        return;
    }
    // To host clip space (ndc_scale/ndc_offset in draw.cpp), then the half pixel offset.
    spv::Id position = b_.createLoad(oPos_, spv::NoPrecision);
    spv::Id xy = b_.createRvalueSwizzle(spv::NoPrecision, tFloatN_[2], position, {0, 1});
    spv::Id w = b_.createCompositeExtract(position, tFloat_, 3);
    spv::Id wSplat = b_.smearScalar(spv::NoPrecision, w, tFloatN_[2]);
    xy = b_.createBinOp(spv::OpFAdd, tFloatN_[2], b_.createBinOp(spv::OpFMul, tFloatN_[2], xy, SharedFloat2(kSharedNdcScale)),
                        b_.createBinOp(spv::OpFMul, tFloatN_[2], SharedFloat2(kSharedNdcOffset), wSplat));
    xy = b_.createBinOp(spv::OpFAdd, tFloatN_[2], xy,
                        b_.createBinOp(spv::OpFMul, tFloatN_[2], SharedFloat2(kSharedHalfPixelOffset), wSplat));
    position = b_.createCompositeInsert(Component(xy, 0), position, tFloatN_[4], 0);
    position = b_.createCompositeInsert(Component(xy, 1), position, tFloatN_[4], 1);
    b_.createStore(position, oPos_);
}

void Translator::TranslateControlFlow()
{
    const auto code = reinterpret_cast<const be<uint32_t>*>(data_ + container_->virtualSize + shader_->physicalOffset);

    union ControlFlowWords
    {
        ControlFlowInstruction controlFlow[2];
        struct
        {
            uint32_t code0, code1, code2, code3;
        } w;
    } cfu;

    auto decodePair = [&](const be<uint32_t>* words) {
        cfu.w.code0 = words[0];
        cfu.w.code1 = words[1] & 0xFFFF;
        cfu.w.code2 = (words[1] >> 16) | (words[2] << 16);
        cfu.w.code3 = words[2] >> 16;
    };

    // First pass: where the control flow ends (the first EXEC address) and whether it is simple.
    std::map<uint32_t, uint32_t> ifEndLabels;
    uint32_t instrSize = shader_->size;
    bool simpleControlFlow = true;
    for (uint32_t address = 0; address < instrSize; address += 12)
    {
        decodePair(code + address / 4);
        for (auto& cf : cfu.controlFlow)
        {
            uint32_t execAddress = 0;
            switch (cf.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                execAddress = cf.exec.address;
                break;
            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                execAddress = cf.condExec.address;
                break;
            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                execAddress = cf.condExecPred.address;
                break;
            case ControlFlowOpcode::CondJmp:
                if (cf.condJmp.isUnconditional || cf.condJmp.direction)
                    simpleControlFlow = false;
                else
                    ++ifEndLabels[cf.condJmp.address];
                break;
            default:
                break;
            }
            if (execAddress != 0)
                instrSize = std::min<uint32_t>(instrSize, execAddress * 12);
        }
    }
    if (!simpleControlFlow)
        Fail("non-structured control flow (unconditional or backward jumps) is not supported yet");

    uint32_t pc = 0;
    for (uint32_t address = 0; address < instrSize; address += 12)
    {
        decodePair(code + address / 4);
        for (auto& cf : cfu.controlFlow)
        {
            ClosePredicate();
            if (auto found = ifEndLabels.find(pc); found != ifEndLabels.end())
            {
                for (uint32_t i = 0; i < found->second; i++)
                    CloseInnermost();
            }
            ++pc;

            uint32_t execAddress = 0, count = 0, sequence = 0;
            bool shouldReturn = false, closeBlock = false;

            switch (cf.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                execAddress = cf.exec.address;
                count = cf.exec.count;
                sequence = cf.exec.sequence;
                shouldReturn = cf.opcode == ControlFlowOpcode::ExecEnd;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
            {
                execAddress = cf.condExec.address;
                count = cf.condExec.count;
                sequence = cf.condExec.sequence;
                shouldReturn = cf.opcode == ControlFlowOpcode::CondExecEnd ||
                               cf.opcode == ControlFlowOpcode::CondExecPredCleanEnd;
                auto found = boolConstants_.find(cf.condExec.boolAddress);
                if (found == boolConstants_.end())
                    Fail("conditional EXEC without a declared boolean constant");
                // if ((g_Booleans & bit) <cond ? != : ==> 0)
                spv::Id bits = b_.createBinOp(spv::OpBitwiseAnd, tUint_, SharedUint(kSharedBooleans),
                                              b_.makeUintConstant(1u << found->second));
                OpenIf(b_.createBinOp(cf.condExec.condition ? spv::OpINotEqual : spv::OpIEqual, tBool_, bits,
                                      b_.makeUintConstant(0)));
                closeBlock = true;
                break;
            }

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
            {
                execAddress = cf.condExecPred.address;
                count = cf.condExecPred.count;
                sequence = cf.condExecPred.sequence;
                shouldReturn = cf.opcode == ControlFlowOpcode::CondExecPredEnd;
                spv::Id p0 = b_.createLoad(p0_, spv::NoPrecision);
                if (!cf.condExecPred.condition)
                    p0 = b_.createUnaryOp(spv::OpLogicalNot, tBool_, p0);
                OpenIf(p0);
                closeBlock = true;
                break;
            }

            case ControlFlowOpcode::LoopStart:
                OpenLoop(cf.loopStart.loopId);
                break;

            case ControlFlowOpcode::LoopEnd:
                CloseInnermost();
                break;

            case ControlFlowOpcode::CondJmp:
            {
                // Forward conditional jump: the code until the target runs when the jump is not taken.
                spv::Id condition = 0;
                if (cf.condJmp.isPredicated)
                {
                    condition = b_.createLoad(p0_, spv::NoPrecision);
                    if (cf.condJmp.condition)  // condition ^ simple: "!p0" when the jump is taken on p0
                        condition = b_.createUnaryOp(spv::OpLogicalNot, tBool_, condition);
                }
                else
                {
                    auto found = boolConstants_.find(cf.condJmp.boolAddress);
                    if (found == boolConstants_.end())
                        Fail(fmt::format("conditional jump on undeclared boolean b{}", uint32_t(cf.condJmp.boolAddress)));
                    spv::Id bits = b_.createBinOp(spv::OpBitwiseAnd, tUint_, SharedUint(kSharedBooleans),
                                                  b_.makeUintConstant(1u << found->second));
                    condition = b_.createBinOp(cf.condJmp.condition ? spv::OpIEqual : spv::OpINotEqual, tBool_, bits,
                                               b_.makeUintConstant(0));
                }
                OpenIf(condition);
                break;
            }

            case ControlFlowOpcode::CondCall:
            case ControlFlowOpcode::Return:
                result_.notes.push_back(fmt::format("control flow opcode {} skipped, as in the HLSL path", uint32_t(cf.opcode)));
                break;

            default:
                break;
            }

            const be<uint32_t>* instructionCode = code + execAddress * 3;
            for (uint32_t i = 0; i < count; i++)
            {
                union InstrWords
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct
                    {
                        uint32_t code0, code1, code2;
                    } w;
                } iu;
                iu.w.code0 = instructionCode[0];
                iu.w.code1 = instructionCode[1];
                iu.w.code2 = instructionCode[2];

                if (sequence & 0x1)
                {
                    if (iu.vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        if (pixel_)
                            Fail("vertex fetch in a pixel shader");
                        TranslateVertexFetch(iu.vertexFetch, execAddress + i);
                    }
                    else
                    {
                        TranslateTextureFetch(iu.textureFetch);
                    }
                }
                else
                {
                    TranslateAlu(iu.alu);
                }
                sequence >>= 2;
                instructionCode += 3;
            }
            ClosePredicate();

            if (shouldReturn)
            {
                EmitShaderEnd();
                EmitReturn();
            }
            if (closeBlock)
                CloseInnermost();
        }
    }
    ClosePredicate();
    while (!open_.empty())
        CloseInnermost();
}

std::vector<uint32_t> Translator::Translate()
{
    ReadContainer();

    b_.addCapability(spv::CapabilityShader);
    if (!v11_)
    {
        b_.addCapability(spv::CapabilityInt64);
        b_.addCapability(spv::CapabilityPhysicalStorageBufferAddresses);
        b_.addCapability(spv::CapabilityRuntimeDescriptorArray);
        b_.addExtension("SPV_EXT_descriptor_indexing");
    }
    glsl_ = b_.import("GLSL.std.450");
    b_.setMemoryModel(v11_ ? spv::AddressingModelLogical : spv::AddressingModelPhysicalStorageBuffer64,
                      spv::MemoryModelGLSL450);
    b_.setSource(spv::SourceLanguageUnknown, 0);

    DeclareTypes();
    DeclareResources();
    DeclareStageInterface();

    main_ = b_.makeEntryPoint("main");
    InitializeLocals();
    TranslateControlFlow();

    if (options_.compositionBlurSwitch && blurMatches_ != 1)
        Fail(fmt::format("composite blur blend found {} times instead of once", blurMatches_));

    // Shaders that end without an ExecEnd still write their outputs.
    if (!b_.getBuildPoint()->isTerminated())
        WriteStageOutputs();
    b_.leaveFunction();

    // From SPIR-V 1.4 the entry point lists every global it uses; before, only the stage inputs and outputs.
    if (v11_ && spirv14_)
    {
        if (uboStage_)
            interface_.push_back(uboStage_);
        if (uboShared_)
            interface_.push_back(uboShared_);
        for (const auto& [binding, variable] : combinedSamplers_)
            interface_.push_back(variable);
    }
    else if (!v11_)
    {
        interface_.push_back(pushConstants_);
        if (uboStage_)
            interface_.push_back(uboStage_);
        if (uboShared_)
            interface_.push_back(uboShared_);
        for (spv::Id heap : textureHeaps_)
        {
            if (heap)
                interface_.push_back(heap);
        }
        if (samplerHeap_)
            interface_.push_back(samplerHeap_);
    }

    spv::Instruction* entryPoint =
        b_.addEntryPoint(pixel_ ? spv::ExecutionModelFragment : spv::ExecutionModelVertex, main_, "main");
    for (spv::Id id : interface_)
        entryPoint->addIdOperand(id);
    if (pixel_)
        b_.addExecutionMode(main_, spv::ExecutionModeOriginUpperLeft);

    std::vector<uint32_t> words;
    b_.dump(words);
    return words;
}

}  // namespace

SpirvRecompileResult RecompileToSpirv(const uint8_t* shaderData, const SpirvRecompilerOptions& options)
{
    SpirvRecompileResult result;
    try
    {
        Translator translator(shaderData, options, result);
        result.spirv = translator.Translate();
    }
    catch (const std::exception& e)
    {
        result.spirv.clear();
        result.error = e.what();
    }
    return result;
}
