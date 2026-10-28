/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_INT32(audio_sdl_rafaga_tramas, 0, "Audio",
                     "Diagnostico: el driver SDL saca las tramas del juego de N en N, con N "
                     "liberaciones seguidas, como el driver de la Switch con buferes de 4 tramas; "
                     "0 o 1 = una a una, como siempre");
REXCVAR_DEFINE_BOOL(audio_sdl_bomba, false, "Audio",
                    "Diagnostico: un hilo pide una trama del juego cada 5,333 ms (como la bomba del "
                    "driver de la Switch) y SDL deja de liberar el semaforo al consumir");
REXCVAR_DEFINE_INT32(audio_volcado_salida_s, 0, "Audio",
                     "Diagnostico: segundos de lo que el driver SDL entrega al dispositivo (con los "
                     "silencios por falta de tramas) que se guardan en audio_salida.wav; 0 = nada");
REXCVAR_DEFINE_INT32(audio_volcado_salida_desde_s, 0, "Audio",
                     "Diagnostico: segundos de salida SDL que se saltan antes del volcado");
// On Android the exported logs carry it (the launcher's "Exportar log"), so a report of bad sound comes with the
// sound itself.
REXCVAR_DEFINE_INT32(audio_volcado_continuo_s, REX_PLATFORM_ANDROID ? 60 : 0, "Audio",
                     "Diagnostico: guarda siempre los ultimos N segundos de lo que el driver SDL entrega al "
                     "dispositivo y reescribe logs/audio_salida.wav (16 bits) cada 15 s; 0 = nada")
    .range(0, 120)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::audio::sdl {

namespace {

// Diagnostic audio_volcado_salida_s and the silence report: only SDL's audio thread uses them.
struct VolcadoSalida {
  std::vector<float> muestras;
  uint64_t saltadas = 0;  // samples per channel skipped before starting
  bool escrito = false;
};
VolcadoSalida volcado_salida;
uint64_t tramas_con_datos = 0;
uint64_t tramas_de_silencio = 0;
std::chrono::steady_clock::time_point ultimo_informe_salida{};

// audio_volcado_continuo_s: the last seconds of the output in a ring (16 bits), which a thread of its own copies out
// every 15 s and writes as logs/audio_salida.wav (to a temporary name first, then renamed: an app closed in the
// middle of a write keeps the previous file whole). SDL's audio thread only appends, under a lock the writer holds
// for the copy alone.
class VolcadoContinuo {
 public:
  void Anadir(const float* datos, size_t muestras, uint32_t canales) {
    std::lock_guard<std::mutex> cerrojo(mutex_);
    if (anillo_.empty()) {
      const int32_t segundos = REXCVAR_GET(audio_volcado_continuo_s);
      if (segundos <= 0) {
        return;
      }
      canales_ = canales;
      anillo_.resize(size_t(segundos) * 48000 * canales);
      std::thread([this] { Escribir(); }).detach();
    }
    if (canales != canales_) {
      return;
    }
    for (size_t i = 0; i < muestras; ++i) {
      anillo_[posicion_] = int16_t(std::clamp(datos[i], -1.0f, 1.0f) * 32767.0f);
      posicion_ = (posicion_ + 1) % anillo_.size();
    }
    llenas_ = std::min(llenas_ + muestras, anillo_.size());
  }

 private:
  void Escribir() {
    const auto carpeta = rex::filesystem::GetExecutableFolder() / "logs";
    bool avisado = false;
    std::vector<int16_t> copia;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(15));
      uint32_t canales;
      {
        std::lock_guard<std::mutex> cerrojo(mutex_);
        canales = canales_;
        // Oldest first: from the write position on when the ring has wrapped.
        copia.resize(llenas_);
        const size_t inicio = llenas_ < anillo_.size() ? 0 : posicion_;
        for (size_t i = 0; i < llenas_; ++i) {
          copia[i] = anillo_[(inicio + i) % anillo_.size()];
        }
      }
      if (copia.empty()) {
        continue;
      }
      std::error_code error;
      std::filesystem::create_directories(carpeta, error);
      const auto temporal = carpeta / "audio_salida.wav.tmp";
      {
        std::ofstream fichero(temporal, std::ios::binary | std::ios::trunc);
        if (!fichero) {
          continue;
        }
        const uint32_t bytes_datos = uint32_t(copia.size() * sizeof(int16_t));
        const auto u32 = [&](uint32_t v) { fichero.write(reinterpret_cast<const char*>(&v), 4); };
        const auto u16 = [&](uint16_t v) { fichero.write(reinterpret_cast<const char*>(&v), 2); };
        fichero.write("RIFF", 4);
        u32(36 + bytes_datos);
        fichero.write("WAVEfmt ", 8);
        u32(16);
        u16(1);  // PCM
        u16(uint16_t(canales));
        u32(48000);
        u32(48000 * canales * 2);
        u16(uint16_t(canales * 2));
        u16(16);
        fichero.write("data", 4);
        u32(bytes_datos);
        fichero.write(reinterpret_cast<const char*>(copia.data()), bytes_datos);
      }
      std::filesystem::rename(temporal, carpeta / "audio_salida.wav", error);
      if (!avisado && !error) {
        avisado = true;
        REXAPU_INFO("[audio] volcado continuo de la salida SDL: los ultimos {} s, {} canales, en {}",
                    copia.size() / canales / 48000, canales, (carpeta / "audio_salida.wav").string());
      }
    }
  }

  std::mutex mutex_;
  std::vector<int16_t> anillo_;
  size_t posicion_ = 0;
  size_t llenas_ = 0;
  uint32_t canales_ = 0;
};
VolcadoContinuo volcado_continuo;

void GrabarSalida(const float* datos, int bytes, uint32_t canales) {
  if (canales && bytes > 0) {
    volcado_continuo.Anadir(datos, size_t(bytes) / sizeof(float), canales);
  }
  const int32_t segundos = REXCVAR_GET(audio_volcado_salida_s);
  if (segundos <= 0 || volcado_salida.escrito || !canales || bytes <= 0) {
    return;
  }
  const size_t muestras = size_t(bytes) / sizeof(float);
  const uint64_t saltar = uint64_t(std::max(REXCVAR_GET(audio_volcado_salida_desde_s), 0)) * 48000;
  if (volcado_salida.saltadas < saltar) {
    volcado_salida.saltadas += muestras / canales;
    return;
  }
  volcado_salida.muestras.insert(volcado_salida.muestras.end(), datos, datos + muestras);
  if (volcado_salida.muestras.size() < size_t(segundos) * 48000 * canales) {
    return;
  }
  volcado_salida.escrito = true;
  const auto ruta = rex::filesystem::GetExecutableFolder() / "audio_salida.wav";
  std::ofstream fichero(ruta, std::ios::binary | std::ios::trunc);
  if (fichero) {
    const uint32_t bytes_datos = uint32_t(volcado_salida.muestras.size() * sizeof(float));
    const auto u32 = [&](uint32_t v) { fichero.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { fichero.write(reinterpret_cast<const char*>(&v), 2); };
    fichero.write("RIFF", 4);
    u32(36 + bytes_datos);
    fichero.write("WAVEfmt ", 8);
    u32(16);
    u16(3);  // flotante IEEE
    u16(uint16_t(canales));
    u32(48000);
    u32(48000 * canales * 4);
    u16(uint16_t(canales * 4));
    u16(32);
    fichero.write("data", 4);
    u32(bytes_datos);
    fichero.write(reinterpret_cast<const char*>(volcado_salida.muestras.data()), bytes_datos);
    REXAPU_INFO("[audio] volcado de la salida SDL: {} muestras por canal, {} canales, en {}",
                volcado_salida.muestras.size() / canales, canales, ruta.string());
  }
  volcado_salida.muestras = std::vector<float>();
}

void ContarTrama(bool silencio) {
  ++(silencio ? tramas_de_silencio : tramas_con_datos);
  const auto ahora = std::chrono::steady_clock::now();
  if (ahora - ultimo_informe_salida < std::chrono::seconds(10)) {
    return;
  }
  if (ultimo_informe_salida.time_since_epoch().count() != 0) {
    REXAPU_INFO("[audio] SDL en 10 s: {} tramas con datos y {} de silencio por falta de tramas",
                tramas_con_datos, tramas_de_silencio);
  }
  tramas_con_datos = 0;
  tramas_de_silencio = 0;
  ultimo_informe_salida = ahora;
}

}  // namespace

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  if (bomba_.joinable()) {
    bomba_activa_ = false;
    bomba_.join();
  }
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
#if REX_PLATFORM_ANDROID
  // AAudio's low latency mode plays from a buffer of a few milliseconds, and a phone running the game at its limit
  // misses those deadlines (clicks and dropouts that never show as missing frames here). A device buffer of 1024
  // frames (21 ms at 48 kHz, four of our 256-frame frames) in the default mode rides through them; latency is
  // still lower than the game's own audio buffering.
  SDL_SetHint(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, "0");
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
#endif

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix. Android reports
  // the 6 channels asked for whatever the output is (speaker, headphones and
  // Bluetooth are stereo), and the system's downmix then drops the balance of
  // ours, so it always gets the stereo fold.
  if (obtained_spec.channels <= 2 || REX_PLATFORM_ANDROID) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}; submitting {} ch",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_));

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (REXCVAR_GET(audio_sdl_bomba)) {
    bomba_activa_ = true;
    bomba_ = std::thread([this]() { Bomba(); });
    REXAPU_INFO("audio: bomba de diagnostico a 187,5 Hz activa");
  }

  return true;
}

void SDLAudioDriver::Bomba() {
  using Reloj = std::chrono::steady_clock;
  const auto intervalo =
      std::chrono::nanoseconds(1'000'000'000ll * channel_samples_ / frame_frequency_);
  auto plazo = Reloj::now() + intervalo;
  while (bomba_activa_.load(std::memory_order_relaxed)) {
    const auto ahora = Reloj::now();
    if (ahora < plazo) {
      std::this_thread::yield();  // no sleep: on Windows it can overshoot by several ms
      continue;
    }
    size_t en_cola;
    {
      std::unique_lock<std::mutex> guard(frames_mutex_);
      en_cola = frames_queued_.size();
    }
    if (en_cola <= 6) {
      semaphore_->Release(1, nullptr);
    }
    if (en_cola < 2) {
      semaphore_->Release(1, nullptr);
    }
    plazo += intervalo;
    if (ahora >= plazo) {
      plazo = ahora + intervalo;
    }
  }
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  if (bomba_.joinable()) {
    bomba_activa_ = false;
    bomba_.join();
  }
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  const int32_t rafaga = REXCVAR_GET(audio_sdl_rafaga_tramas);
  if (rafaga > 1) {
    // Diagnostic: up to N frames at once, each with its release, into a private buffer SDL feeds from.
    // The game then gets the calls in bursts, as on the Switch.
    while (additional_amount > 0) {
      if (driver->rafaga_leido_ >= driver->rafaga_.size()) {
        driver->rafaga_.clear();
        driver->rafaga_leido_ = 0;
        std::unique_lock<std::mutex> guard(driver->frames_mutex_);
        for (int32_t k = 0; k < rafaga && !driver->frames_queued_.empty(); ++k) {
          float* buffer = driver->frames_queued_.front();
          driver->frames_queued_.pop();
          const size_t inicio = driver->rafaga_.size();
          driver->rafaga_.resize(inicio + size_t(sample_count));
          if (REXCVAR_GET(audio_mute)) {
            std::fill(driver->rafaga_.begin() + inicio, driver->rafaga_.end(), 0.0f);
          } else if (driver->sdl_device_channels_ == 2) {
            conversion::sequential_6_BE_to_interleaved_2_LE(driver->rafaga_.data() + inicio, buffer,
                                                            channel_samples_, fold, gain);
          } else {
            conversion::sequential_6_BE_to_interleaved_6_LE(driver->rafaga_.data() + inicio, buffer,
                                                            channel_samples_, mix, gain);
          }
          driver->frames_unused_.push(buffer);
          if (!driver->bomba_activa_.load(std::memory_order_relaxed)) {
            auto ret = driver->semaphore_->Release(1, nullptr);
            assert_true(ret);
          }
        }
      }
      if (driver->rafaga_leido_ >= driver->rafaga_.size()) {
        // Nothing queued: silence without a release, as in the normal path.
        std::memset(data, 0, len);
        if (!SDL_PutAudioStreamData(stream, data, len)) {
          break;
        }
        GrabarSalida(data, len, driver->sdl_device_channels_);
        ContarTrama(true);
        additional_amount -= len;
        continue;
      }
      if (!SDL_PutAudioStreamData(stream, driver->rafaga_.data() + driver->rafaga_leido_, len)) {
        break;
      }
      GrabarSalida(driver->rafaga_.data() + driver->rafaga_leido_, len, driver->sdl_device_channels_);
      ContarTrama(false);
      driver->rafaga_leido_ += size_t(sample_count);
      additional_amount -= len;
    }
    SDL_stack_free(data);
    return;
  }
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      GrabarSalida(data, len, driver->sdl_device_channels_);
      ContarTrama(true);
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      GrabarSalida(data, len, driver->sdl_device_channels_);
      ContarTrama(false);
      driver->frames_unused_.push(buffer);

      if (!driver->bomba_activa_.load(std::memory_order_relaxed)) {
        auto ret = driver->semaphore_->Release(1, nullptr);
        assert_true(ret);
      }
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
