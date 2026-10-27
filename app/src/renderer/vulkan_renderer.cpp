#include "vulkan_renderer.h"

#include "draws.h"
#include "frame_stats.h"
#include "frame_ring.h"
#include "output.h"
#include "pipelines.h"
#include "render_targets.h"
#include "shader_modules.h"
#include "textures.h"
#include "vulkan_context.h"

#include "../nfsmw_nativo_ganchos.h"
#include "../nfsmw_nativo_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/thread.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(nfsmw_msaa, "apagado", "Graficos",
                      "MSAA de la escena con el renderizador Vulkan 1.1 (nfsmw_renderer = vulkan11): apagado, 2x, 4x u "
                      "8x. La Xbox 360 dibujaba la escena con 4x. Cuesta GPU; se puede combinar con FXAA")
    .allowed({"apagado", "2x", "4x", "8x"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(nfsmw_renderer_traza_s, 0, "NFSMW",
                     "Renderizador Vulkan 1.1 (analisis): escribe en el log la traza completa del fotograma que empieza "
                     "en este segundo de vida del proceso (passes, sus destinos y dibujos, copias y borrados). 0 = nunca")
    .range(0, 3600);

// On Android only this renderer fits: the original one needs descriptor indexing and buffer device addresses, which
// many phone drivers lack.
#if defined(__ANDROID__)
#define NFSMW_RENDERER_DEFAULT "vulkan11"
#else
#define NFSMW_RENDERER_DEFAULT "legacy"
#endif
// Temporary, for the Android test builds: where the phone's GPU time goes (textures or geometry), measured on the
// same route without the player doing anything.
// Measured on the Mali-G68 (27/10): +2 LOD made no difference (44.0 vs 45.1 ms), textures are not its bottleneck.
REXCVAR_DEFINE_BOOL(nfsmw_diag_texturas, false, "NFSMW",
                    "Renderizador Vulkan 1.1 (analisis): cada 10 s alterna las texturas entre normales y con +2 de "
                    "sesgo de mipmap (4 y 16 veces mas pequenas, se ven borrosas), y el log dice en que modo va cada "
                    "informe: la diferencia de tiempo de GPU es lo que cuesta leer texturas");
// Measured on the Mali-G68 (27/10, high profile): a quarter of the pixels saved 6.9 of 46.5 ms, so shading pixels
// costs ~9 ms and the rest is fixed per draw, pass and copy work.
REXCVAR_DEFINE_BOOL(nfsmw_diag_pixeles, false, "NFSMW",
                    "Renderizador Vulkan 1.1 (analisis): cada 10 s alterna entre dibujar normal y recortar cada "
                    "dibujo al cuarto superior izquierdo (la geometria entera, un cuarto de los pixeles); el log dice "
                    "en que modo va cada informe: la diferencia de tiempo de GPU es lo que cuestan los pixeles");

REXCVAR_DEFINE_STRING(nfsmw_renderer, NFSMW_RENDERER_DEFAULT, "NFSMW",
                      "Backend of the native renderer: legacy = the original one; vulkan11 = the Vulkan 1.1 "
                      "renderer written from scratch (app/src/renderer)")
    .allowed({"legacy", "vulkan11"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::renderer {

namespace {

// Upload memory per frame in flight: indices, vertices, constants, texture data and readbacks.
constexpr VkDeviceSize kUploadBytesPerFrame = 96ull << 20;

class VulkanRenderer final : public nativo::DestinosNativos {
 public:
  VulkanRenderer(std::unique_ptr<VulkanContext> context, rex::memory::Memory* memory)
      : context_(std::move(context)),
        frames_(*context_),
        render_targets_(*context_, frames_, memory),
        textures_(*context_, frames_, memory, render_targets_),
        shaders_(*context_),
        pipelines_(*context_, shaders_),
        draws_(*context_, frames_, memory, render_targets_, textures_, pipelines_),
        output_(*context_, frames_) {}

  ~VulkanRenderer() override {
    StopMaintenance();
    pipelines_.SaveCacheFile();
    // Everything deferred runs while every module is still alive.
    frames_.WaitIdle();
  }

  bool Initialize() {
    if (!frames_.Initialize(kUploadBytesPerFrame) || !render_targets_.Initialize() || !textures_.Initialize() ||
        !pipelines_.Initialize() || !output_.Initialize()) {
      return false;
    }
    render_targets_.SetMultisample(SupportedMultisample());
    render_targets_.SetResolvedDestroyedCallback([this](uint64_t id) { textures_.ForgetResolved(id); });
    pipelines_.LoadCacheFile(rex::filesystem::GetExecutableFolder() / "cache" / "nfsmw_vk11_pipelines.bin");
    return true;
  }

  // --- Work ---------------------------------------------------------------------------------------------------------

  bool Copiar(const nativo::RegistrosCopia& registers) override {
    draws_.EndPass();
    const VkCommandBuffer commands = frames_.Commands();
    if (!commands) {
      ++rejected_;
      return false;
    }
    const auto copy_start = Clock::now();
    render_targets_.Copy(commands, registers);
    frame_.copy_ns += Nanoseconds(copy_start, Clock::now());
    ++copies_;
    return true;
  }

  bool Dibujar(const nativo::PeticionDibujo& request) override {
    const auto start = Clock::now();
    const bool drawn = draws_.Draw(request);
    const uint64_t draw_ns = Nanoseconds(start, Clock::now());
    report_.draw_ns += draw_ns;
    frame_.draw_ns += draw_ns;
    ++report_.draws;
    ++frame_.draws;
    if (!drawn) {
      ++dropped_draws_;
    }
    return drawn;
  }

  bool Presentar(rex::ui::Presenter* presenter, const nativo::TexturaSwap& swap, uint32_t width,
                 uint32_t height) override {
    draws_.EndPass();
    const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
    // The render target the game presents defines the scene's pitch, which gets MSAA from the next frame on.
    if (render_targets_.multisample() != VK_SAMPLE_COUNT_1_BIT && !render_targets_.scene_pitch()) {
      if (const uint32_t pitch = render_targets_.SourcePitchOf(base)) {
        render_targets_.LearnScenePitch(pitch);
      }
    }
    const ResolvedTexture* front_buffer = render_targets_.Resolved(base);
    if (!front_buffer && !warned_no_front_buffer_) {
      warned_no_front_buffer_ = true;
      REXLOG_WARN("[renderer] swap of {:08X} without a resolved texture", base);
    }
    // The output pass goes at the end of the frame's commands and submits them (one submission per frame). Without
    // an output (no front buffer, or the presenter did not ask) the frame's work is submitted on its own.
    const auto present_start = Clock::now();
    const bool presented = front_buffer && output_.Present(presenter, front_buffer->image, width, height);
    if (frames_.recording()) {
      frames_.Submit();
    }
    const uint64_t present_ns = Nanoseconds(present_start, Clock::now());
    frame_.present_ns += present_ns;
    report_.submit_ns += present_ns;
    draws_.FrameSubmitted();
    textures_.EndFrame();
    StartMaintenance();
    TraceFrame();
    Report();
    if (!presented) {
      ++rejected_;
      return false;
    }
    ++presented_;
    return true;
  }

  void RampaGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) override { output_.SetGammaRamp(ramp); }

  // Vertex data is copied while recording: nothing is pending.
  void EsperarSubidas() override {}

  // --- Occlusion queries: not measured yet (the native system then writes its default count) --------------------

  void EmpezarOclusion(uint32_t) override {}
  bool TerminarOclusion(uint32_t, uint64_t&) override { return false; }
  void EstadisticasOclusion(uint64_t values[5]) const override { std::fill(values, values + 5, 0); }

  // --- Statistics -------------------------------------------------------------------------------------------------

  nativo::EstadisticasDibujos EstadisticasDeDibujos() const override {
    nativo::EstadisticasDibujos stats;
    stats.dibujados = draws_.draw_count();
    stats.rechazados = dropped_draws_;
    return stats;
  }
  uint64_t CopiasProfundidad() const override { return 0; }
  uint64_t BorradosProfundidad() const override { return 0; }
  void Estadisticas(uint64_t& copies, uint64_t& clears, uint64_t& presented, uint64_t& rejected) const override {
    copies = copies_;
    clears = 0;
    presented = presented_;
    rejected = rejected_;
  }
  void TiempoGpu(uint64_t& ns, uint64_t& jobs) const override { ns = jobs = 0; }
  uint64_t MarcaGpuFinalNs() const override { return 0; }
  uint64_t TrabajosMarcasPrecisas() const override { return 0; }
  void EsperasGpu(uint64_t& count, uint64_t& ns) const override { count = ns = 0; }
  void DuracionTrabajosGpu(uint64_t& count, uint64_t& ns) const override { count = ns = 0; }
  void CosteGrabar(uint64_t cost[6]) const override { std::fill(cost, cost + 6, 0); }
  void TiempoGpuPorCategoria(std::array<uint64_t, nativo::kGpuCategorias>& ns) const override { ns.fill(0); }
  void EstadisticasPipeline(std::array<uint64_t, nativo::kGpuCategorias>& fragments,
                            std::array<uint64_t, nativo::kGpuCategorias>& vertices,
                            std::array<uint64_t, nativo::kGpuCategorias>& primitives) const override {
    fragments.fill(0);
    vertices.fill(0);
    primitives.fill(0);
  }
  void CopiasPorTamano(std::array<uint64_t, 4>& copies, std::array<uint64_t, 4>& pixels) const override {
    copies.fill(0);
    pixels.fill(0);
  }
  void EstadisticasPorShader(std::vector<uint64_t>& fragments, std::vector<uint64_t>& draws,
                             uint64_t& frames) const override {
    fragments.clear();
    draws.clear();
    frames = 0;
  }
  void IntervalosEntreSwaps(std::array<uint64_t, nativo::kCubetasSwap>& buckets, uint64_t& overlaps,
                            double& worst_ms) const override {
    buckets.fill(0);
    overlaps = 0;
    worst_ms = 0.0;
  }
  void CostePresentar(uint64_t cost[12]) const override { std::fill(cost, cost + 12, 0); }

 private:
  using Clock = std::chrono::steady_clock;

  // nfsmw_msaa, lowered to what the device supports for color and depth attachments and for sampling depth (the
  // depth resolve reads it).
  VkSampleCountFlagBits SupportedMultisample() const {
    const std::string setting = REXCVAR_GET(nfsmw_msaa);
    uint32_t wanted = setting == "2x" ? 2 : setting == "4x" ? 4 : setting == "8x" ? 8 : 1;
    const auto& properties = context_->device().properties();
    const VkSampleCountFlags supported = properties.framebufferColorSampleCounts &
                                         properties.framebufferDepthSampleCounts &
                                         properties.framebufferStencilSampleCounts &
                                         properties.sampledImageDepthSampleCounts;
    while (wanted > 1 && !(supported & wanted)) {
      wanted >>= 1;
    }
    if (wanted > 1) {
      REXLOG_INFO("[renderer] MSAA {}x requested ({})", wanted, setting);
    }
    return VkSampleCountFlagBits(wanted);
  }

  // --- Maintenance thread ------------------------------------------------------------------------------------------
  // Keeps pipeline and shader creation off the ring thread: once the shader library is loaded it recreates the
  // pipelines of the previous runs, then translates every other shader, then saves the pipeline cache whenever new
  // pipelines appear. Lowest priority, so it only uses idle CPU time.

  void StartMaintenance() {
    if (maintenance_.joinable()) {
      return;
    }
    const nativo::ShadersNativos* library = nativo::BibliotecaActiva();
    if (!library || !library->cargada()) {
      return;  // not loaded yet: checked again at the next swap
    }
    try {
      maintenance_ = std::thread([this, library] { MaintenanceLoop(*library); });
    } catch (const std::system_error& error) {
      REXLOG_WARN("[renderer] maintenance thread could not be started: {}", error.what());
    }
  }

  void StopMaintenance() {
    {
      std::lock_guard<std::mutex> lock(maintenance_mutex_);
      stop_maintenance_.store(true, std::memory_order_relaxed);
    }
    maintenance_wake_.notify_all();
    if (maintenance_.joinable()) {
      maintenance_.join();
    }
  }

  void MaintenanceLoop(const nativo::ShadersNativos& library) {
    rex::thread::set_current_thread_name("NFSMW renderer maintenance");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    const auto start = Clock::now();
    const uint32_t warmed = pipelines_.WarmUp(library, stop_maintenance_);
    REXLOG_INFO("[renderer] maintenance: {} pipelines warmed up in {:.1f} s", warmed,
                std::chrono::duration<double>(Clock::now() - start).count());

    // Library numbers are dense from 0; a long run of missing numbers is the end.
    uint32_t translated = 0;
    for (uint32_t number = 0, missing = 0; missing < 256 && !stop_maintenance_.load(std::memory_order_relaxed);
         ++number) {
      const nativo::EntradaShader* shader = library.PorNumero(number);
      if (!shader) {
        ++missing;
        continue;
      }
      missing = 0;
      shaders_.Get(*shader);
      ++translated;
    }
    REXLOG_INFO("[renderer] maintenance: {} shaders ready after {:.1f} s", translated,
                std::chrono::duration<double>(Clock::now() - start).count());

    std::unique_lock<std::mutex> lock(maintenance_mutex_);
    while (!stop_maintenance_.load(std::memory_order_relaxed)) {
      lock.unlock();
      pipelines_.SaveCacheFile();
      lock.lock();
      maintenance_wake_.wait_for(lock, std::chrono::seconds(10),
                                 [this] { return stop_maintenance_.load(std::memory_order_relaxed); });
    }
  }
  static uint64_t Nanoseconds(Clock::time_point a, Clock::time_point b) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
  }

  // nfsmw_renderer_traza_s: the frame starting at that second is traced and written out at its swap.
  void TraceFrame() {
    if (Stats().tracing()) {
      const std::vector<std::string> lines = Stats().FinishTrace();
      REXLOG_INFO("[renderer] trace of one frame ({} lines):", lines.size());
      for (const std::string& line : lines) {
        REXLOG_INFO("[renderer] trace |{}", line);
      }
      return;
    }
    const int32_t at = REXCVAR_GET(nfsmw_renderer_traza_s);
    if (!traced_ && at > 0 && std::chrono::duration<double>(Clock::now() - process_start_).count() >= at) {
      traced_ = true;
      Stats().StartTrace();
    }
  }

  // Once every 5 seconds: frame rate and where the ring thread's time went.
  void Report() {
    ++report_.frames;
    const auto now = Clock::now();
    // The previous frame (swap to swap): the ring thread's own work, its waits for the GPU, and the rest (the time
    // it spent outside the renderer: PM4 parsing, waiting for the game).
    frame_.gpu_wait_ns = frames_.TakeWaitNs();
    if (report_.last_frame != Clock::time_point{}) {
      const uint64_t frame_ns = Nanoseconds(report_.last_frame, now);
      report_.worst_frame_ns = std::max(report_.worst_frame_ns, frame_ns);
      if (frame_ns > 40'000'000) {
        ++report_.slow_frames;
        if (report_.slow_frames <= 3) {
          const TextureCache::Stats& t = textures_.stats();
          REXLOG_INFO("[renderer] slow frame {:.1f} ms: {} draws in {:.1f} ms, copies {:.1f} ms, present {:.1f} ms, GPU "
                      "waits {:.1f} ms (included in the others), outside the renderer {:.1f} ms | textures: {} created "
                      "in {:.1f} ms, {} uploads ({:.2f} MB), {} fingerprints ({:.2f} MB)",
                      double(frame_ns) / 1e6, frame_.draws, double(frame_.draw_ns) / 1e6, double(frame_.copy_ns) / 1e6,
                      double(frame_.present_ns) / 1e6, double(frame_.gpu_wait_ns) / 1e6,
                      double(frame_ns - std::min(frame_ns, frame_.draw_ns + frame_.copy_ns + frame_.present_ns)) / 1e6,
                      t.created - frame_.textures.created, double(t.create_ns - frame_.textures.create_ns) / 1e6,
                      t.uploads - frame_.textures.uploads,
                      double(t.upload_bytes - frame_.textures.upload_bytes) / 1048576.0,
                      t.fingerprints - frame_.textures.fingerprints,
                      double(t.fingerprint_bytes - frame_.textures.fingerprint_bytes) / 1048576.0);
        }
      }
    }
    report_.last_frame = now;
    frame_ = FrameTiming{};
    frame_.textures = textures_.stats();
    if (report_.start == Clock::time_point{}) {
      report_.start = now;
      return;
    }
    const double seconds = std::chrono::duration<double>(now - report_.start).count();
    if (seconds < 5.0) {
      return;
    }
    const TextureCache::Stats t = textures_.TakeStats();
    frame_.textures = TextureCache::Stats{};
    const Pipelines::CreationStats p = pipelines_.TakeStats();
    const double frames = double(report_.frames);
    REXLOG_INFO("[renderer] queue submissions per frame: {:.2f} (the presenter's own are not counted)",
                double(context_->TakeSubmitCount()) / frames);
    REXLOG_INFO("[renderer] {:.1f} fps | per frame: {:.0f} draws in {:.2f} ms, present {:.2f} ms | textures: {:.0f} "
                "lookups, {:.1f} fingerprints ({:.2f} MB), {:.1f} uploads ({:.2f} MB)",
                frames / seconds, double(report_.draws) / frames, double(report_.draw_ns) / 1e6 / frames,
                double(report_.submit_ns) / 1e6 / frames, double(t.lookups) / frames, double(t.fingerprints) / frames,
                double(t.fingerprint_bytes) / 1048576.0 / frames, double(t.uploads) / frames,
                double(t.upload_bytes) / 1048576.0 / frames);
    {
      // Where the CPU time of the draws goes (nfsmw_perfil_dibujos).
      const Draws::DrawProfile p = draws_.TakeDrawProfile();
      if (p.draws) {
        static constexpr const char* kStages[] = {"setup", "geometry", "textures", "pass", "pipeline",
                                                  "constants+sets", "vertices", "commands"};
        std::string parts;
        uint64_t total = 0;
        for (size_t i = 0; i < p.ns.size(); ++i) {
          total += p.ns[i];
          parts += fmt::format("{}{} {:.2f}", i ? ", " : "", kStages[i], double(p.ns[i]) / 1e6 / frames);
        }
        REXLOG_INFO("[renderer] draw CPU per frame (ms): {} | total {:.2f} for {:.0f} draws ({:.2f} us each)", parts,
                    double(total) / 1e6 / frames, double(p.draws) / frames,
                    double(total) / 1e3 / double(p.draws));
      }
      {
        const GeometryCache::Stats g = draws_.TakeGeometryStats();
        if (g.hits || g.misses || g.dynamic) {
          REXLOG_INFO("[renderer] persistent geometry per frame: {:.0f} reused, {:.0f} converted ({:.2f} MB), {:.1f} "
                      "found rewritten, {:.0f} dynamic (per-frame copy), {:.1f} without room | {} cache flips, {:.1f} "
                      "write notifications",
                      double(g.hits) / frames, double(g.misses) / frames, double(g.misses_bytes) / 1048576.0 / frames,
                      double(g.stale) / frames, double(g.dynamic) / frames, double(g.full) / frames, g.flips,
                      double(g.notifications) / frames);
        }
      }
      const Output::Timing o = output_.TakeTiming();
      const auto ms = [&](uint64_t v) { return double(v) / 1e6 / frames; };
      REXLOG_INFO("[renderer] present per frame: presenter before the output {:.2f} ms, recording the output {:.2f} "
                  "ms, submitting {:.2f} ms (all submissions: {:.2f} ms waiting for the queue lock, {:.2f} ms in "
                  "vkQueueSubmit), presenter after {:.2f} ms",
                  ms(o.before_ns), ms(o.record_ns), ms(o.submit_ns), ms(context_->TakeSubmitLockNs()),
                  ms(context_->TakeSubmitCallNs()), ms(o.after_ns));
      const uint64_t gpu_frames = frames_.TakeGpuFrames();
      const uint64_t gpu_ns = frames_.TakeGpuNs();
      if (gpu_frames) {
        REXLOG_INFO("[renderer] GPU per frame: {:.2f} ms from the start to the end of the frame's commands ({} frames "
                    "timed; the presenter's paint is not included)",
                    double(gpu_ns) / 1e6 / double(gpu_frames), gpu_frames);
        // Where that time goes, by GPU timestamps between the passes: the 8 costliest pieces.
        const std::vector<FrameRing::GpuSegment> segments = frames_.TakeGpuSegments();
        std::string parts;
        for (size_t i = 0; i < segments.size() && i < 8; ++i) {
          parts += fmt::format("{}{:.2f} ms {} (x{:.1f})", i ? " | " : "",
                               double(segments[i].ns) / 1e6 / double(gpu_frames), segments[i].name,
                               double(segments[i].count) / double(gpu_frames));
        }
        REXLOG_INFO("[renderer] GPU per frame by piece: {}", parts);
      }
    }
    const ShaderModules::Stats sh = shaders_.TakeStats();
    const Draws::PassStats ps = draws_.TakePassStats();
    {
      const FrameCounters c = Stats().Take();
      const auto per = [&](uint64_t v) { return double(v) / frames; };
      REXLOG_INFO("[renderer] per frame: {:.0f} draws | pipeline binds {:.0f}, descriptor binds {:.0f} (texture sets "
                  "written {:.0f}, uniform sets {:.0f}), textures reused {:.0f}, vertex binds {:.0f}, index binds {:.1f}, viewport {:.0f}, "
                  "scissor {:.0f}, depth bias {:.0f}, blend constants {:.0f}, stencil {:.0f} | VS constants {:.0f} "
                  "uploads ({:.2f} MB), PS {:.0f} ({:.2f} MB)",
                  per(c.draws), per(c.pipeline_binds), per(c.descriptor_binds), per(c.texture_set_writes),
                  per(c.uniform_set_writes), per(c.texture_reuses), per(c.vertex_buffer_binds), per(c.index_buffer_binds),
                  per(c.viewport_sets), per(c.scissor_sets), per(c.depth_bias_sets), per(c.blend_constant_sets),
                  per(c.stencil_sets), per(c.vs_constant_uploads), per(c.vs_constant_bytes) / 1048576.0,
                  per(c.ps_constant_uploads), per(c.ps_constant_bytes) / 1048576.0);
      REXLOG_INFO("[renderer] per frame: render area {:.2f} Mpixels of {:.2f} in the framebuffers, {:.1f} passes "
                  "reopened larger | resolves without a copy {:.2f}, restored afterwards {:.2f} | vegetation "
                  "left out of the shadow maps {:.0f} draws",
                  per(c.render_area_pixels) / 1e6, per(c.framebuffer_pixels) / 1e6, per(c.passes_reopened),
                  per(c.swap_resolves), per(c.swap_restores), per(c.shadow_vegetation_skipped));
      REXLOG_INFO("[renderer] per frame: {:.1f} passes; attachments: {:.1f} LOAD, {:.1f} CLEAR, {:.1f} not stored | "
                  "separate clears: color {:.1f} ({:.2f} Mpixels), depth {:.1f} ({:.2f} Mpixels) | resolves {:.1f} "
                  "({:.2f} Mpixels, {:.1f} of a target still cleared)",
                  per(c.passes), per(c.attachments_loaded), per(c.attachments_cleared_on_load),
                  per(c.attachments_discarded), per(c.color_clears),
                  per(c.color_clear_pixels) / 1e6, per(c.depth_clears), per(c.depth_clear_pixels) / 1e6,
                  per(c.resolves), per(c.resolve_pixels) / 1e6, per(c.cleared_then_copied));
    }
    if (draws_.tessellated_draws()) {
      REXLOG_INFO("[renderer] tessellated draws so far: {} (dropped)", draws_.tessellated_draws());
    }
    REXLOG_INFO("[renderer] render passes per frame: {:.1f} (after a copy/clear/swap {:.1f}, depth toggled {:.1f}, "
                "color subset {:.1f}, different targets {:.1f})",
                double(ps.after_break + ps.depth_toggled + ps.color_subset + ps.different_targets) / frames,
                double(ps.after_break) / frames, double(ps.depth_toggled) / frames, double(ps.color_subset) / frames,
                double(ps.different_targets) / frames);
    REXLOG_INFO("[renderer] stability: worst frame {:.1f} ms, {} frames over 40 ms | ring thread created {} pipelines "
                "in {:.1f} ms | {} shaders translated in {:.1f} ms (any thread)",
                double(report_.worst_frame_ns) / 1e6, report_.slow_frames, p.created, double(p.create_ns) / 1e6,
                sh.translated, double(sh.translate_ns) / 1e6);
    if (REXCVAR_GET(nfsmw_diag_texturas)) {
      // Every report is entirely in one mode (the GPU times lag by the frames in flight, a few out of ~150); the
      // mode changes every two reports.
      REXLOG_INFO("[renderer] diag textures: this report with {} textures", diag_lod_bias_ ? "+2 LOD (blurry)" : "normal");
      if (++diag_reports_ % 2 == 0) {
        diag_lod_bias_ = diag_lod_bias_ ? 0 : 2;
        textures_.SetDiagnosticLodBias(diag_lod_bias_);
      }
    } else if (REXCVAR_GET(nfsmw_diag_pixeles)) {
      // Same scheme: each report in one mode, the mode changing every two.
      REXLOG_INFO("[renderer] diag pixels: this report with {}", diag_quarter_ ? "a quarter of the pixels" : "all pixels");
      if (++diag_reports_ % 2 == 0) {
        diag_quarter_ = !diag_quarter_;
        draws_.SetDiagnosticQuarter(diag_quarter_);
      }
    }
    report_ = ReportCounters{};
    report_.start = now;
    report_.last_frame = now;
  }

  uint32_t diag_lod_bias_ = 0;
  bool diag_quarter_ = false;
  uint64_t diag_reports_ = 0;

  struct FrameTiming {
    uint64_t draws = 0;
    TextureCache::Stats textures;  // the cache's counters when the frame started
    uint64_t draw_ns = 0;
    uint64_t copy_ns = 0;
    uint64_t present_ns = 0;
    uint64_t gpu_wait_ns = 0;
  };

  struct ReportCounters {
    Clock::time_point start{};
    uint64_t frames = 0;
    uint64_t draws = 0;
    uint64_t draw_ns = 0;
    uint64_t submit_ns = 0;
    Clock::time_point last_frame{};
    uint64_t worst_frame_ns = 0;
    uint64_t slow_frames = 0;
  };

  std::unique_ptr<VulkanContext> context_;
  FrameRing frames_;
  RenderTargets render_targets_;
  TextureCache textures_;
  ShaderModules shaders_;
  Pipelines pipelines_;
  Draws draws_;
  Output output_;

  uint64_t copies_ = 0;
  uint64_t presented_ = 0;
  uint64_t rejected_ = 0;
  uint64_t dropped_draws_ = 0;
  bool warned_no_front_buffer_ = false;
  ReportCounters report_;
  FrameTiming frame_;

  const Clock::time_point process_start_ = Clock::now();
  bool traced_ = false;

  std::thread maintenance_;
  std::mutex maintenance_mutex_;
  std::condition_variable maintenance_wake_;
  std::atomic<bool> stop_maintenance_{false};
};

}  // namespace

FrameStats& Stats() {
  static FrameStats stats;
  return stats;
}

bool VulkanRendererSelected() { return REXCVAR_GET(nfsmw_renderer) == "vulkan11"; }

std::unique_ptr<nativo::DestinosNativos> CreateVulkanRenderer(const rex::ui::vulkan::VulkanDevice* device,
                                                              rex::memory::Memory* memory) {
  if (!device || !memory) {
    return nullptr;
  }
  std::string error;
  std::unique_ptr<VulkanContext> context = VulkanContext::Create(device, error);
  if (!context) {
    REXLOG_ERROR("[renderer] Vulkan 1.1 renderer not available: {}", error);
    return nullptr;
  }
  auto renderer = std::make_unique<VulkanRenderer>(std::move(context), memory);
  if (!renderer->Initialize()) {
    REXLOG_ERROR("[renderer] Vulkan 1.1 renderer could not be initialized");
    return nullptr;
  }
  REXLOG_INFO("[renderer] Vulkan 1.1 renderer (nfsmw_renderer = vulkan11)");
  return renderer;
}

}  // namespace nfsmw::renderer
