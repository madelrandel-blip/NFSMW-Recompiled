#include "shader_modules.h"

#include "../nfsmw_nativo_shaders.h"
#include "../nfsmw_shader_library.h"
#include "nfsmw_contenedor.h"
#include "spirv_recompiler.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/filesystem.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#if NFSMW_SPIRV_OPT
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#endif

#include <chrono>
#include <utility>

REXCVAR_DEFINE_BOOL(nfsmw_spirv_optimizar, true, "Graficos",
                    "Optimizar el SPIR-V de los shaders traducidos (SPIRV-Tools, las mismas pasadas que spirv-opt -O): "
                    "mismo resultado, unos 13 % menos de GPU por dibujo medido en PC. Se hace una vez por shader y se "
                    "guarda en cache/spirv. false = el SPIR-V tal cual sale de la traduccion");

namespace nfsmw::renderer {

namespace {

// XXH3 of the race VisualTreatment container (p_000139): its radial blur gets the SPEC_CONSTANT_SIN_DESENFOQUE
// switch. The same in every edition.
constexpr uint64_t kCompositeFingerprint = 0x19C0C358044A29BFull;

}  // namespace

ShaderModules::~ShaderModules() {
  for (auto& [shader, module] : modules_) {
    if (module.module) {
      context_.fn().vkDestroyShaderModule(context_.handle(), module.module, nullptr);
    }
  }
}

VkShaderModule ShaderModules::Get(const nativo::EntradaShader& shader) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = modules_.find(&shader); it != modules_.end()) {
      return it->second.module;
    }
  }
  // Translated outside the lock: the other thread may be translating another shader meanwhile.
  const auto start = std::chrono::steady_clock::now();
  const Module module = Translate(shader);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  std::lock_guard<std::mutex> lock(mutex_);
  auto [it, inserted] = modules_.emplace(&shader, module);
  if (!inserted && module.module) {
    // Both threads translated it: keep the first.
    context_.fn().vkDestroyShaderModule(context_.handle(), module.module, nullptr);
  }
  if (inserted) {
    ++stats_.translated;
    stats_.translate_ns += ns;
  }
  return it->second.module;
}

uint32_t ShaderModules::ConstantBytes(const nativo::EntradaShader& shader) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = modules_.find(&shader);
  return it != modules_.end() ? it->second.constant_bytes : 0;
}

ShaderModules::Stats ShaderModules::TakeStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(stats_, Stats{});
}

#if NFSMW_SPIRV_OPT
namespace {

// One background thread, lowest priority work: optimize and write to the cache.
struct OptimizeJob {
  std::vector<uint32_t> spirv;
  uint32_t version;
  std::filesystem::path folder, file;
};

void RunOptimizeJob(const OptimizeJob& job) {
  const std::string name = job.file.filename().string();
  const std::vector<uint32_t>& spirv = job.spirv;
  const uint32_t version = job.version;
  const std::filesystem::path& folder = job.folder;
  const std::filesystem::path& file = job.file;
  const auto start = std::chrono::steady_clock::now();
  const spv_target_env env = version >= 0x10400 ? SPV_ENV_VULKAN_1_1_SPIRV_1_4 : SPV_ENV_VULKAN_1_1;
  spvtools::Optimizer optimizer(env);
  optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char*) {});
  optimizer.RegisterPerformancePasses();
  std::vector<uint32_t> out;
  spvtools::SpirvTools tools(env);
  tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char*) {});
  if (!optimizer.Run(spirv.data(), spirv.size(), &out) || out.empty() || !tools.Validate(out)) {
    REXLOG_WARN("[renderer] shader {}: the optimizer failed, the translation is used as it is", name);
    return;
  }
  REXLOG_INFO("[renderer] shader optimized: {} -> {} words in {:.1f} ms (kept in cache/spirv)", spirv.size(),
              out.size(),
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  // Written aside and renamed: a session closed halfway never leaves a truncated module to load.
  std::filesystem::path partial = file;
  partial += ".tmp";
  {
    std::ofstream stream(partial, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(uint32_t));
    if (!stream) {
      return;
    }
  }
  std::filesystem::rename(partial, file, ec);
}

std::mutex g_jobs_mutex;
std::condition_variable g_jobs_ready;
std::deque<OptimizeJob> g_jobs;
bool g_worker_started = false;

void OptimizeLater(const std::vector<uint32_t>& spirv, uint32_t version, const std::filesystem::path& folder,
                   const std::filesystem::path& file) {
  std::lock_guard<std::mutex> lock(g_jobs_mutex);
  // The same translation can be asked for again before its job runs: once is enough.
  static std::unordered_set<std::string> queued;
  if (!queued.insert(file.filename().string()).second) {
    return;
  }
  g_jobs.push_back({spirv, version, folder, file});
  if (!g_worker_started) {
    g_worker_started = true;
    std::thread([] {
      for (;;) {
        OptimizeJob job;
        {
          std::unique_lock<std::mutex> lock(g_jobs_mutex);
          g_jobs_ready.wait(lock, [] { return !g_jobs.empty(); });
          job = std::move(g_jobs.front());
          g_jobs.pop_front();
        }
        RunOptimizeJob(job);
      }
    }).detach();
  }
  g_jobs_ready.notify_one();
}

}  // namespace
#endif

// The translated SPIR-V, optimized once with the performance passes and kept in cache/spirv under the hash of the
// translation (and of the SPIR-V version, which the passes target). Any failure (optimizer, validator, disk) gives the
// translation as it is: the result is never worse than without this.
// What this session runs, said plainly in the log so two sessions can be compared: the state of the switch once, then
// how many shaders came optimized from the cache and how many went in as translated (every 20).
void ShaderModules::NoteOptimization(bool optimized) {
  static std::atomic<bool> said{false};
  if (!said.exchange(true)) {
    REXLOG_INFO("[shaders] optimizacion de shaders: {} (nfsmw_spirv_optimizar = {})",
                REXCVAR_GET(nfsmw_spirv_optimizar) ? "ENCENDIDA" : "APAGADA",
                REXCVAR_GET(nfsmw_spirv_optimizar) ? "true" : "false");
  }
  const uint32_t done = (optimized ? optimized_loaded_ : original_loaded_).fetch_add(1) + 1;
  if ((optimized_loaded_.load() + original_loaded_.load()) % 20 == 0 || done == 1) {
    REXLOG_INFO("[shaders] en uso: {} optimizados y {} tal cual se tradujeron", optimized_loaded_.load(),
                original_loaded_.load());
  }
}

std::vector<uint32_t> ShaderModules::Optimized(const std::vector<uint32_t>& spirv) {
#if NFSMW_SPIRV_OPT
  if (!REXCVAR_GET(nfsmw_spirv_optimizar) || spirv.empty()) {
    NoteOptimization(false);
    return spirv;
  }
  const uint32_t version = context_.spirv_version();
  char name[40];
  std::snprintf(name, sizeof(name), "%016llX_%X.spv",
                static_cast<unsigned long long>(XXH3_64bits(spirv.data(), spirv.size() * sizeof(uint32_t))), version);
  const std::filesystem::path folder = rex::filesystem::GetExecutableFolder() / "cache" / "spirv";
  const std::filesystem::path file = folder / name;
  if (std::ifstream cached{file, std::ios::binary}) {
    std::vector<char> bytes((std::istreambuf_iterator<char>(cached)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 20 && bytes.size() % 4 == 0) {
      const uint32_t* words = reinterpret_cast<const uint32_t*>(bytes.data());
      if (words[0] == 0x07230203u) {
        NoteOptimization(true);
        return std::vector<uint32_t>(words, words + bytes.size() / 4);
      }
    }
  }
  // Not optimized yet: the translation is used now and the optimizer runs on its own thread, so a slow phone never
  // waits for it; the next session loads the result from the cache.
  OptimizeLater(spirv, version, folder, file);
  NoteOptimization(false);
  return spirv;

#else
  return spirv;
#endif
}

ShaderModules::Module ShaderModules::Translate(const nativo::EntradaShader& shader) {
  Module module;
  if (!shader.shader) {
    return module;
  }
  std::vector<uint8_t> container;
  try {
    nfsmw::Flujo flow;
    container = nfsmw::Convertir2005(shader.shader->original, flow);
  } catch (const std::exception& e) {
    REXLOG_ERROR("[renderer] shader n{}: container: {}", shader.numero, e.what());
    return module;
  }
  SpirvRecompilerOptions options;
  options.resources = SpirvResourceModel::Vulkan11;
  options.legacyMultiply = true;
  options.sanitizeColorOutputs = true;
  options.compositionBlurSwitch = shader.shader->huella == kCompositeFingerprint;
  options.spirvVersion = context_.spirv_version();
  const SpirvRecompileResult result = RecompileToSpirv(container.data(), options);
  if (result.spirv.empty()) {
    REXLOG_ERROR("[renderer] shader n{}: translation: {}", shader.numero, result.error);
    return module;
  }
  const std::vector<uint32_t> spirv = Optimized(result.spirv);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = spirv.size() * sizeof(uint32_t);
  info.pCode = spirv.data();
  if (context_.fn().vkCreateShaderModule(context_.handle(), &info, nullptr, &module.module) != VK_SUCCESS) {
    REXLOG_ERROR("[renderer] shader n{}: vkCreateShaderModule failed", shader.numero);
    module.module = VK_NULL_HANDLE;
  }
  module.constant_bytes = result.floatConstantRegisters * 16;
  return module;
}

}  // namespace nfsmw::renderer
