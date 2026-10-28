#pragma once

// Runs a DXC module and a directly translated module of the same shader on the GPU with the same inputs and
// compares what they write (nfsmw_spirv_gpu.cpp).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GpuComparer
{
public:
    // nullptr if there is no usable Vulkan device. With `translatedV11`, the translated modules use the Vulkan 1.1
    // resource model (SpirvResourceModel::Vulkan11) and get the same data through their own bindings.
    static std::unique_ptr<GpuComparer> Create(bool translatedV11 = false);
    virtual ~GpuComparer() = default;

    // container: the normalized container (constant table, interpolators, outputs). Returns false with the
    // reason in `report` if the outputs differ or a pipeline cannot be created; `report` may also carry
    // information on success.
    virtual bool Compare(const std::string& name, bool vertex, const std::vector<uint8_t>& container,
                         const std::vector<uint32_t>& reference, const std::vector<uint32_t>& translated,
                         std::string& report) = 0;

    // Cold pipeline creation time of both modules: each one gets a unique marker so no driver shader cache can
    // answer for it. Accumulated in milliseconds (reference, translated) with the number of shaders measured.
    virtual void MeasureCreation(bool vertex, const std::vector<uint8_t>& container,
                                 const std::vector<uint32_t>& reference, const std::vector<uint32_t>& translated) = 0;
    virtual void CreationTotals(double& referenceMs, double& translatedMs, uint32_t& shaders) const = 0;
};
