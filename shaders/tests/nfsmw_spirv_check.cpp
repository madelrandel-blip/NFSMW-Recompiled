// nfsmw_spirv_check: translates every container of a corpus with the direct SPIR-V translator and checks the result
// against the SPIR-V that DXC produced from the HLSL path (see CMakeLists.txt for the corpus layout).
//
//   1. Translation: every container translates, with nothing skipped that the HLSL path did not skip as well.
//   2. Validation: spirv-val (SPIRV-Tools) for the Vulkan 1.2 environment.
//   3. Interface: the same stage inputs and outputs, descriptor bindings, specialization constant, OpKill count and
//      shadow marker as the DXC module, which is what the native renderer relies on.
//   4. GPU (nfsmw_spirv_gpu.cpp, with --gpu): both modules run with the same random inputs and the outputs are
//      compared.
//
// Exit code 0 only if every check passes.

#include "nfsmw_contenedor.h"
#include "nfsmw_shader_library.h"
#include "nfsmw_spirv_gpu.h"
#include "spirv_recompiler.h"

#include <spirv-tools/libspirv.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// The race VisualTreatment composite, by the SHA-256 of its 2005 container (kComposicionSha256 in
// app/src/lanzador/shaders.cpp).
constexpr const char* kCompositeSha256 = "e80cca037bd4becd8a192daa4583e3a3a9281e06436015746583f30701a296f9";

std::string Sha256(const std::vector<uint8_t>& data)
{
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rot = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
    std::vector<uint8_t> m(data);
    m.push_back(0x80);
    while (m.size() % 64 != 56)
        m.push_back(0);
    for (int i = 7; i >= 0; --i)
        m.push_back(uint8_t(uint64_t(data.size()) * 8 >> (i * 8)));
    for (size_t block = 0; block < m.size(); block += 64)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(m[block + i * 4]) << 24 | uint32_t(m[block + i * 4 + 1]) << 16 |
                   uint32_t(m[block + i * 4 + 2]) << 8 | m[block + i * 4 + 3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = hh + (rot(e, 6) ^ rot(e, 11) ^ rot(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rot(a, 2) ^ rot(a, 13) ^ rot(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (uint32_t v : h)
        for (int i = 28; i >= 0; i -= 4)
            s += kHex[(v >> i) & 0xF];
    return s;
}

std::vector<uint8_t> ReadBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// What the native renderer and the pipeline layout see of a module.
struct Interface
{
    std::set<uint32_t> inputLocations, outputLocations;
    std::set<uint32_t> builtIns;
    std::set<std::pair<uint32_t, uint32_t>> bindings;  // (set, binding)
    bool pushConstants = false;
    bool specId0 = false;
    uint32_t kills = 0;
    bool shadowMarker = false;
};

Interface Inspect(const std::vector<uint32_t>& words)
{
    Interface result;
    std::map<uint32_t, uint32_t> storage;  // variable -> storage class
    std::map<uint32_t, std::map<uint32_t, uint32_t>> decorations;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!count)
            break;
        switch (op)
        {
        case 71:  // OpDecorate
            decorations[words[i + 1]][words[i + 2]] = count > 3 ? words[i + 3] : 0;
            break;
        case 59:  // OpVariable
            storage[words[i + 2]] = words[i + 3];
            break;
        case 252:   // OpKill
        case 4416:  // OpTerminateInvocation
        case 5380:  // OpDemoteToHelperInvocation
            ++result.kills;
            break;
        case 43:  // OpConstant
            if (count == 4 && words[i + 3] == 0x5E3B1A84u)
                result.shadowMarker = true;
            break;
        default:
            break;
        }
        i += count;
    }
    for (const auto& [id, decoration] : decorations)
    {
        if (decoration.count(1) && decoration.at(1) == 0)  // SpecId 0
            result.specId0 = true;
        auto found = storage.find(id);
        if (found == storage.end())
            continue;
        const uint32_t storageClass = found->second;
        if (decoration.count(11))  // BuiltIn
            result.builtIns.insert(decoration.at(11));
        if (decoration.count(30))  // Location
            (storageClass == 1 ? result.inputLocations : result.outputLocations).insert(decoration.at(30));
        if (decoration.count(34) && decoration.count(33))  // DescriptorSet, Binding
            result.bindings.insert({decoration.at(34), decoration.at(33)});
    }
    for (const auto& [id, storageClass] : storage)
    {
        if (storageClass == 9)  // PushConstant
            result.pushConstants = true;
    }
    return result;
}

std::string Join(const std::set<uint32_t>& values)
{
    std::string s;
    for (uint32_t v : values)
        s += (s.empty() ? "" : ",") + std::to_string(v);
    return "{" + s + "}";
}

std::string Join(const std::set<std::pair<uint32_t, uint32_t>>& values)
{
    std::string s;
    for (const auto& [set, binding] : values)
        s += (s.empty() ? "" : ",") + std::to_string(set) + "." + std::to_string(binding);
    return "{" + s + "}";
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
                     "usage: nfsmw_spirv_check <corpus folder> [--xenos] [--gpu] [--dump <folder>]\n"
                     "  --xenos  translate with the Xenos multiply rule and output sanitizing (the shipping mode)\n"
                     "           instead of the mode that mirrors the DXC library\n"
                     "  --gpu    also run both modules on the GPU and compare the outputs\n"
                     "  --v11    translate with the Vulkan 1.1 renderer's model (SPIR-V 1.4)\n"
                     "  --spirv13  with --v11, SPIR-V 1.3 (core Vulkan 1.1) instead of 1.4\n");
        return 2;
    }
    const fs::path corpus = argv[1];
    bool xenos = false, gpu = false, timing = false, v11 = false, spirv13 = false;
    fs::path dump, pack;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--xenos")
            xenos = true;
        else if (arg == "--gpu")
            gpu = true;
        else if (arg == "--v11")
            v11 = true;
        else if (arg == "--spirv13")
            spirv13 = true;
        else if (arg == "--timing")
            timing = gpu = true;
        else if (arg == "--dump" && i + 1 < argc)
            dump = argv[++i];
        else if (arg == "--pack" && i + 1 < argc)
            pack = argv[++i];
    }

    std::vector<fs::path> containers;
    for (const auto& entry : fs::directory_iterator(corpus))
    {
        if (entry.path().extension() == ".cont")
            containers.push_back(entry.path());
    }
    std::sort(containers.begin(), containers.end());
    if (containers.empty())
    {
        std::fprintf(stderr, "no .cont files in %s\n", corpus.string().c_str());
        return 2;
    }
    if (!dump.empty())
        fs::create_directories(dump);

    // The Vulkan11 model is SPIR-V 1.4 (Vulkan 1.1 with VK_KHR_spirv_1_4) unless --spirv13 asks for core 1.3.
    spvtools::SpirvTools tools(!v11 ? SPV_ENV_VULKAN_1_2 : spirv13 ? SPV_ENV_VULKAN_1_1 : SPV_ENV_VULKAN_1_1_SPIRV_1_4);
    std::string lastMessage;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
        lastMessage = std::string(message) + " (word " + std::to_string(position.index) + ")";
    });

    std::unique_ptr<GpuComparer> comparer;
    if (gpu)
    {
        comparer = GpuComparer::Create(v11);
        if (!comparer)
        {
            std::fprintf(stderr, "no Vulkan device for the GPU comparison\n");
            return 2;
        }
    }

    std::vector<nfsmw::native::Shader> library;
    uint32_t failures = 0, translated = 0, compositeMatches = 0, shadowFetches = 0, gpuCompared = 0;
    for (const fs::path& path : containers)
    {
        const std::string name = path.stem().string();
        const std::vector<uint8_t> original = ReadBytes(path);
        std::vector<uint32_t> reference;
        {
            const std::vector<uint8_t> bytes = ReadBytes(fs::path(path).replace_extension(".spv"));
            reference.resize(bytes.size() / 4);
            std::memcpy(reference.data(), bytes.data(), reference.size() * 4);
        }

        std::vector<uint8_t> normalized;
        try
        {
            nfsmw::Flujo flow;
            normalized = nfsmw::Convertir2005(original, flow);
        }
        catch (const std::exception& e)
        {
            std::printf("FAIL %s: container: %s\n", name.c_str(), e.what());
            ++failures;
            continue;
        }

        SpirvRecompilerOptions options;
        options.resources = v11 ? SpirvResourceModel::Vulkan11 : SpirvResourceModel::DescriptorHeaps;
        options.legacyMultiply = xenos;
        options.sanitizeColorOutputs = xenos;
        options.compositionBlurSwitch = Sha256(original) == kCompositeSha256;
        options.spirvVersion = spirv13 ? 0x10300 : 0x10400;
        compositeMatches += options.compositionBlurSwitch;
        const SpirvRecompileResult result = RecompileToSpirv(normalized.data(), options);
        if (result.spirv.empty())
        {
            std::printf("FAIL %s: translation: %s\n", name.c_str(), result.error.c_str());
            ++failures;
            continue;
        }
        ++translated;
        shadowFetches += result.shadowFetches;
        for (const std::string& note : result.notes)
            std::printf("note %s: %s\n", name.c_str(), note.c_str());
        if (!pack.empty())
        {
            nfsmw::native::Shader entry;
            entry.original = original;
            entry.spirv = result.spirv;
            library.push_back(std::move(entry));
        }
        if (!dump.empty())
        {
            std::ofstream out(dump / (name + ".spv"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(result.spirv.data()), std::streamsize(result.spirv.size() * 4));
        }

        lastMessage.clear();
        if (!tools.Validate(result.spirv))
        {
            std::printf("FAIL %s: spirv-val: %s\n", name.c_str(), lastMessage.c_str());
            ++failures;
            continue;
        }

        const Interface mine = Inspect(result.spirv), theirs = Inspect(reference);
        std::vector<std::string> problems;
        // Inputs: DXC drops the interpolators whose register is never read; an extra one is harmless because the
        // vertex shaders write all 22. Missing ones are not.
        if (!std::includes(mine.inputLocations.begin(), mine.inputLocations.end(), theirs.inputLocations.begin(),
                           theirs.inputLocations.end()))
            problems.push_back("inputs " + Join(mine.inputLocations) + " vs " + Join(theirs.inputLocations));
        if (mine.outputLocations != theirs.outputLocations)
            problems.push_back("outputs " + Join(mine.outputLocations) + " vs " + Join(theirs.outputLocations));
        if (!std::includes(mine.builtIns.begin(), mine.builtIns.end(), theirs.builtIns.begin(), theirs.builtIns.end()))
            problems.push_back("built-ins " + Join(mine.builtIns) + " vs " + Join(theirs.builtIns));
        // Bindings: every one DXC uses must be there; an extra one in a dead specialization branch is harmless
        // because the pipeline layout declares all of them. The Vulkan 1.1 model has its own bindings and no push
        // constants by design.
        if (!v11 &&
            !std::includes(mine.bindings.begin(), mine.bindings.end(), theirs.bindings.begin(), theirs.bindings.end()))
            problems.push_back("bindings " + Join(mine.bindings) + " vs " + Join(theirs.bindings));
        if (!v11 && mine.pushConstants != theirs.pushConstants)
            problems.push_back("push constants differ");
        if (v11 && mine.pushConstants)
            problems.push_back("push constants in the Vulkan 1.1 model");
        if (!mine.specId0)
            problems.push_back("no SpecId 0");
        if (mine.kills != theirs.kills)
            problems.push_back("OpKill " + std::to_string(mine.kills) + " vs " + std::to_string(theirs.kills));
        if (mine.shadowMarker != theirs.shadowMarker)
            problems.push_back("shadow marker differs");
        if (!problems.empty())
        {
            std::string all;
            for (const auto& p : problems)
                all += (all.empty() ? "" : "; ") + p;
            std::printf("FAIL %s: interface: %s\n", name.c_str(), all.c_str());
            ++failures;
            continue;
        }

        if (comparer)
        {
            std::string report;
            const bool vertex = (original[3] & 1) != 0;
            if (!comparer->Compare(name, vertex, normalized, reference, result.spirv, report))
            {
                std::printf("FAIL %s: gpu: %s\n", name.c_str(), report.c_str());
                ++failures;
                continue;
            }
            if (!report.empty())
                std::printf("gpu  %s: %s\n", name.c_str(), report.c_str());
            if (timing)
                comparer->MeasureCreation(vertex, normalized, reference, result.spirv);
            ++gpuCompared;
        }
    }

    if (compositeMatches != 1)
    {
        std::printf("FAIL: the race composite was found %u times in the corpus instead of once\n", compositeMatches);
        ++failures;
    }
    if (shadowFetches == 0)
    {
        std::printf("FAIL: no shadow map sample was rewritten\n");
        ++failures;
    }
    if (!pack.empty() && failures == 0)
    {
        try
        {
            const std::vector<uint8_t> bytes = nfsmw::native::EmpaquetarShaders(std::move(library));
            {
                std::ofstream out(pack, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            }
            // Read back with the renderer's own loader.
            nfsmw::native::BibliotecaShaders check;
            check.Cargar(pack);
            std::printf("library %s: %zu bytes, %zu shaders\n", pack.string().c_str(), bytes.size(),
                        check.shaders().size());
        }
        catch (const std::exception& e)
        {
            std::printf("FAIL: library: %s\n", e.what());
            ++failures;
        }
    }
    if (comparer && timing)
    {
        double referenceMs = 0.0, translatedMs = 0.0;
        uint32_t measured = 0;
        comparer->CreationTotals(referenceMs, translatedMs, measured);
        std::printf("cold pipeline creation, %u shaders: DXC modules %.0f ms (%.1f ms each), direct modules %.0f ms "
                    "(%.1f ms each)\n",
                    measured, referenceMs, measured ? referenceMs / measured : 0.0, translatedMs,
                    measured ? translatedMs / measured : 0.0);
    }
    std::printf("%zu containers, %u translated, %u shadow samples, %u compared on the GPU, %u failures\n",
                containers.size(), translated, shadowFetches, gpuCompared, failures);
    return failures == 0 ? 0 : 1;
}
