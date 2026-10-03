#!/usr/bin/env python3
"""
Instruments the entire audio chain.  (version 3)

    python tools/parche_xma.py            apply
    python tools/parche_xma.py --estado
    python tools/parche_xma.py --revertir

It touches three SDK files:

    src/audio/xma_context.cpp        the decoder
    src/audio/audio_system.cpp       the thread that calls the game
    src/audio/sdl/sdl_audio_driver.cpp   the output to the card

It keeps a .original of each on the first run and is idempotent. If it detects
a previous version of the patch, it restores from the .original before applying
this one, so the anchors fit on the clean code.


WHAT WE ALREADY KNOW, AND WHY A v3 IS NEEDED
============================================

The hang is located: the game's thread 0xD spins between
XMAGetOutputBufferWriteOffset and XMAGetOutputBufferReadOffset waiting for
decoded audio that never arrives. The audio dying and the hang on returning to
the menu are THE SAME FAILURE, not two.

v2 already measured what matters: 6739 kicks, 7160 Work passes, and 1232 early
exits ALL because of output_buffer_valid == 0. The "does not fit" branch -which
was my suspect- did not trigger even once. And then the XMA stops dead.

But v2 left two gaps, both mine:

  1. I put the Work state log BEFORE PrepareOutputRingBuffer. And it turns out
     PrepareOutputRingBuffer is precisely the one that recomputes
     remaining_subframe_blocks_in_output_buffer_ from the offsets. So the
     "gap" that came out was the LEFTOVER value from the previous pass. It
     always came out 0 and meant nothing. In v3 it goes after.

  2. The other side of the chain was not visible. And that is where the rest
     of the story is.


THE COMPLETE CHAIN, AND WHERE IT BREAKS
=======================================

The 360's audio goes through three pieces, and each one waits for the previous
one:

    game  ->  XmaContext::Work()  ->  frames_queued_  ->  SDLCallback
      ^                                                        |
      |                    semaphore, one release per frame     |
      +--------------------------------------------------------+

SDLCallback releases the semaphore ONLY when it consumes a frame for real. If
the queue empties, it releases nothing; then the AudioWorker's WaitAny times
out at 500 ms and does NOT call the game callback; and if the game is not
called, the game does not deliver more audio. It is a ring, and if one link
stops, all three stop.

The v2 log fits that to the detail: queued_count=8 at 22:31:25, nothing after
that, and "no frames queued (silence)" from 22:31:52. The 8 frames were used up
and not one more arrived.

What CANNOT be known with v2 is whether the AudioWorker kept calling the game
after the hang, because the SDK has those two counters capped:

    if (diag_pump_count < 10)      in audio_system.cpp
    if (sdl_callback_count < 10)   in sdl_audio_driver.cpp

At ten lines they go silent forever. Right before the failure. That is why v3
replaces them with a heartbeat of one line per second: it does not flood the
log and never goes silent, which is exactly what is needed here.


WHAT IT WILL ANSWER
===================

With the three pieces instrumented, the last line of each before the silence
says who stopped first:

  - if the kicks stop coming     -> the game stopped asking for audio
  - if the kicks continue but Work exits -> the decoder is stuck
  - if the worker heartbeat stays alive but with envios=0 frozen -> the
    semaphore is not released, and the broken link is the output
  - if the heartbeat stops entirely -> the audio thread itself is blocked

All behind log_noisy except the heartbeat, which goes to DEBUG because it is
one line per second and it is the one that matters. LOG_DETALLADO.bat already
turns both on.
"""

import argparse
import pathlib
import shutil
import sys

MARCA = "PARCHE LOCAL - instrumentacion de audio v4"

# Marks of previous versions. If any appears, the file is restored from its
# .original before applying, because the anchors below are written against the
# CLEAN SDK code and would not fit on the patched one.
MARCAS_VIEJAS = [
    "PARCHE LOCAL - instrumentacion de audio v3",
    "PARCHE LOCAL - el anillo de salida no se llena del todo",
    "PARCHE LOCAL - instrumentacion del XMA v2",
    "PARCHE LOCAL - instrumentacion de audio v2",
    "PARCHE LOCAL - el descodificador XMA no puede dormirse para siempre",
]

# ---------------------------------------------------------------------------
#  1) src/audio/xma_context.cpp   -  the decoder
# ---------------------------------------------------------------------------

ENABLE_ANCLA = """void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}
"""

ENABLE_NUEVO = """void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
  // PARCHE LOCAL - instrumentacion de audio v4
  //
  // Un "kick" del juego. Es lo unico que vuelve a encender un contexto:
  // Work() lo apaga tras un solo pase. Si estas lineas dejan de salir antes
  // del cuelgue, el juego dejo de pedir audio y el fallo no esta aqui.
  REXAPU_NOISY_DEBUG("XmaContext {}: kick (Enable)", id());
}
"""

WORK_ANCLA = """  if (!data.output_buffer_valid) {
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);
"""

WORK_NUEVO = """  if (!data.output_buffer_valid) {
    // PARCHE LOCAL - salida temprana 1 de 2. Devuelve "hecho" sin descodificar.
    //
    // OJO: esta rama sale SIN llamar a StoreContextMerged, o sea que no
    // escribe nada de vuelta en la memoria del juego. En la v2 fue la unica
    // que se disparo, 1232 veces.
    REXAPU_NOISY_DEBUG("XmaContext {}: se sale sin descodificar - salida no valida", id());
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  // PARCHE LOCAL - instrumentacion de audio v4
  //
  // Este log tiene que ir DESPUES de PrepareOutputRingBuffer y no antes. En
  // la v2 lo puse antes y el "hueco" que salia era el valor SOBRANTE de la
  // llamada anterior, no el de ahora: PrepareOutputRingBuffer es justo quien
  // recalcula remaining_subframe_blocks_in_output_buffer_ a partir de los
  // offsets. Salia 0 constantemente y no significaba nada.
  //
  // Los dos offsets son los mismos que el juego consulta en bucle mientras
  // esta colgado, asi que aqui se ve la otra mitad de esa conversacion.
  REXAPU_NOISY_DEBUG(
      "XmaContext {}: Work escritura={} lectura={} hueco={} sdc={} relleno={}", id(),
      uint32_t(data.output_buffer_write_offset), uint32_t(data.output_buffer_read_offset),
      remaining_subframe_blocks_in_output_buffer_, uint32_t(data.subframe_decode_count),
      uint32_t(data.output_buffer_padding));
"""

# The original PrepareOutputRingBuffer line was moved inside the block above,
# so the one left loose a few lines below must be removed. Otherwise it would
# be called twice and the second one would overwrite the ring's offsets.
DUPLICADO_ANCLA = """  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  // Consume-only context: no input, just drain remaining subframes.
"""

DUPLICADO_NUEVO = """  // Consume-only context: no input, just drain remaining subframes.
"""

HUECO_ANCLA = """  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }
"""

HUECO_NUEVO = """  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    // PARCHE LOCAL - salida temprana 2 de 2.
    //
    // Significa "no hay hueco en el buffer de salida para descodificar". Era
    // mi sospechosa en la v2 y NO se disparo ni una vez, asi que se queda
    // instrumentada solo para poder descartarla otra vez de un vistazo.
    REXAPU_NOISY_DEBUG(
        "XmaContext {}: se sale sin descodificar - no cabe (necesita {} bloques, hay {})", id(),
        minimum_subframe_decode_count, remaining_subframe_blocks_in_output_buffer_);
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }
"""

NADA_ANCLA = """  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  if (output_rb.empty()) {
    data.output_buffer_valid = 0;
  }
"""

NADA_NUEVO = """  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  if (output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  // PARCHE LOCAL - instrumentacion de audio v4
  //
  // LA PREGUNTA DE ESTA VERSION. En el cuelgue, este contexto tenia sitio de
  // sobra en la salida -hueco=19 de 24- y aun asi la escritura no se movio ni
  // un bloque. Si hay sitio y no produce, es que no tiene NADA QUE
  // DESCODIFICAR: el bucle de decodificacion se rompe en cuanto
  // IsAnyInputBufferValid() da falso.
  //
  // Asi que aqui se registra cada pase que no produjo una sola muestra, con
  // el estado de los dos buffers de entrada. Si salen los dos a cero, el juego
  // dejo de darle datos y el fallo esta arriba, en quien alimenta; si salen a
  // uno, el problema es del descodificador y hay que mirar error_status.
  if (data.output_buffer_write_offset == initial_data.output_buffer_write_offset) {
    REXAPU_NOISY_DEBUG(
        "XmaContext {}: NO PRODUJO NADA - ent0={} ent1={} err={} hueco={} escritura={} lectura={}",
        id(), uint32_t(data.input_buffer_0_valid), uint32_t(data.input_buffer_1_valid),
        uint32_t(data.error_status), remaining_subframe_blocks_in_output_buffer_,
        uint32_t(data.output_buffer_write_offset), uint32_t(data.output_buffer_read_offset));
  }
"""

XMA_ANCLAS = [
    ("kick del juego", ENABLE_ANCLA, ENABLE_NUEVO),
    ("pase que no produjo nada", NADA_ANCLA, NADA_NUEVO),
    ("estado al entrar en Work", WORK_ANCLA, WORK_NUEVO),
    ("PrepareOutputRingBuffer duplicado", DUPLICADO_ANCLA, DUPLICADO_NUEVO),
    ("salida por falta de hueco", HUECO_ANCLA, HUECO_NUEVO),
]

# ---------------------------------------------------------------------------
#  2) src/audio/audio_system.cpp  -  the thread that calls the game
# ---------------------------------------------------------------------------

LATIDO_ANCLA = """  // Main run loop.
  uint32_t diag_pump_count = 0;
  while (worker_running_) {
"""

LATIDO_NUEVO = """  // Main run loop.
  uint32_t diag_pump_count = 0;

  // PARCHE LOCAL - instrumentacion de audio v4, latido del hilo de audio.
  //
  // Los contadores de diagnostico que ya traia el SDK -diag_pump_count y su
  // gemelo en sdl_audio_driver.cpp- se topan a diez lineas y luego se callan
  // PARA SIEMPRE. Con eso se ve el arranque y nada mas, y aqui lo que hace
  // falta es justo lo contrario: saber si este hilo sigue vivo DESPUES de que
  // el audio se muera, que es cuando el SDK ya lleva rato mudo.
  //
  // Una linea por segundo no inunda nada y no se calla nunca. Va a DEBUG y no
  // a NOISY a proposito: es la linea que importa.
  //
  //   esperas  vueltas del bucle
  //   exitos   veces que un semaforo se solto -o sea, que SDL consumio audio-
  //   plazos   veces que WaitAny se agoto a los 500 ms sin senal ninguna
  //   fallos   errores del WaitAny
  //   envios   llamadas al callback del juego que volvieron
  //
  // Como leerlo cuando se cuelgue:
  //   plazos sube y exitos congelado -> SDL no consume: la cola de frames se
  //     vacio y nadie la rellena. El eslabon roto esta antes, en el juego o
  //     en el descodificador.
  //   exitos sube pero envios congelado -> se entra a llamar al juego y no
  //     vuelve: el callback del guest se quedo dentro.
  //   el latido para del todo -> este mismo hilo esta bloqueado.
  uint64_t lat_esperas = 0;
  uint64_t lat_exitos = 0;
  uint64_t lat_plazos = 0;
  uint64_t lat_fallos = 0;
  uint64_t lat_envios = 0;
  auto lat_ultimo = std::chrono::steady_clock::now();
  auto latido = [&]() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - lat_ultimo < std::chrono::seconds(1)) {
      return;
    }
    lat_ultimo = ahora;
    REXAPU_DEBUG("AudioWorker latido: esperas={} exitos={} plazos={} fallos={} envios={}",
                 lat_esperas, lat_exitos, lat_plazos, lat_fallos, lat_envios);
  };

  while (worker_running_) {
"""

ESPERA_ANCLA = """    if (result.first == rex::thread::WaitResult::kFailed) {
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      if (diag_pump_count < 5) {
        REXAPU_NOISY_DEBUG("AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    }
"""

ESPERA_NUEVO = """    // PARCHE LOCAL - instrumentacion de audio v4
    lat_esperas++;
    if (result.first == rex::thread::WaitResult::kFailed) {
      lat_fallos++;
      latido();
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      lat_plazos++;
      if (diag_pump_count < 5) {
        REXAPU_NOISY_DEBUG("AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    } else if (result.first == rex::thread::WaitResult::kSuccess) {
      lat_exitos++;
    }
    latido();
"""

ENVIO_ANCLA = """        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
"""

ENVIO_NUEVO = """        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
        // PARCHE LOCAL - se cuenta DESPUES de Execute a proposito: si el
        // callback del juego se queda dentro y no vuelve, este contador se
        // congela mientras "exitos" sigue subiendo, y eso lo dice todo.
        lat_envios++;
"""

SISTEMA_INC_ANCLA = """#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
"""

SISTEMA_INC_NUEVO = """// PARCHE LOCAL - instrumentacion de audio v4. El fichero ya usaba
// std::chrono, pero le llegaba de rebote por otras cabeceras. El latido
// depende de el, asi que se pide explicitamente en vez de confiar en eso.
#include <chrono>

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
"""

SISTEMA_ANCLAS = [
    ("include de chrono", SISTEMA_INC_ANCLA, SISTEMA_INC_NUEVO),
    ("latido del worker", LATIDO_ANCLA, LATIDO_NUEVO),
    ("clasificacion de la espera", ESPERA_ANCLA, ESPERA_NUEVO),
    ("vuelta del callback del juego", ENVIO_ANCLA, ENVIO_NUEVO),
]

# ---------------------------------------------------------------------------
#  3) src/audio/sdl/sdl_audio_driver.cpp  -  the output to the card
# ---------------------------------------------------------------------------

SILENCIO_ANCLA = """    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
"""

SILENCIO_NUEVO = """    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      // PARCHE LOCAL - instrumentacion de audio v4
      //
      // El contador original se topa a diez lineas y luego se calla para
      // siempre, asi que el silencio del cuelgue no dejaba rastro: solo se
      // veian los diez primeros huecos del arranque, que son normales.
      //
      // Aqui va limitado por TIEMPO, una linea por segundo, y no se calla
      // nunca. Lleva la cuenta acumulada para distinguir un hueco suelto de
      // un silencio continuo.
      static std::atomic<uint64_t> sdl_silencios{0};
      static std::atomic<int64_t> sdl_ultimo_aviso{0};
      const uint64_t total_silencios = ++sdl_silencios;
      const int64_t ahora_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      int64_t anterior = sdl_ultimo_aviso.load(std::memory_order_relaxed);
      if (ahora_ms - anterior >= 1000 &&
          sdl_ultimo_aviso.compare_exchange_strong(anterior, ahora_ms)) {
        REXAPU_DEBUG("SDLCallback: sin frames en cola (silencio), acumulado={}", total_silencios);
      }
      (void)sdl_callback_count;
"""

SDL_INC_ANCLA = """#include <algorithm>
#include <array>
#include <cstring>
"""

SDL_INC_NUEVO = """#include <algorithm>
#include <array>
#include <atomic>   // PARCHE LOCAL - instrumentacion de audio v4
#include <chrono>   // PARCHE LOCAL - instrumentacion de audio v4
#include <cstring>
"""

ENTREGA_ANCLA = """  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}
"""

ENTREGA_NUEVO = """  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));

    // PARCHE LOCAL - instrumentacion de audio v4
    //
    // Esta es LA pregunta: despues del cuelgue, el juego sigue entregando
    // audio o no? El contador que traia el SDK aqui tambien se topaba a diez
    // -y los diez se gastan en el arranque-, asi que no se veia.
    //
    // Va dentro del cerrojo a proposito. La linea original del SDK lee
    // frames_queued_.size() FUERA de el, unas lineas mas arriba, y ahi el
    // valor puede estar cambiando bajo los pies. Aqui no.
    static std::atomic<uint64_t> sdl_entregas{0};
    static std::atomic<int64_t> sdl_ultima_entrega{0};
    const uint64_t total_entregas = ++sdl_entregas;
    const int64_t ahora_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    int64_t anterior = sdl_ultima_entrega.load(std::memory_order_relaxed);
    if (ahora_ms - anterior >= 1000 &&
        sdl_ultima_entrega.compare_exchange_strong(anterior, ahora_ms)) {
      REXAPU_DEBUG("SDLAudioDriver: el juego entrego audio, acumulado={} en cola={}",
                   total_entregas, frames_queued_.size());
    }
  }
}
"""

SDL_ANCLAS = [
    ("includes de la salida", SDL_INC_ANCLA, SDL_INC_NUEVO),
    ("entrega de audio del juego", ENTREGA_ANCLA, ENTREGA_NUEVO),
    ("silencio de la salida", SILENCIO_ANCLA, SILENCIO_NUEVO),
]

# ---------------------------------------------------------------------------

def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "src" / "audio" / "xma_context.cpp").exists():
            return cand
    sys.exit("[ERROR] Cannot find the SDK's src/audio/xma_context.cpp.\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def original_de(f):
    return f.with_suffix(f.suffix + ".original")


def restaurar_si_hay_version_vieja(f):
    """Leaves the file as it was in the SDK if it carries a previous patch.

    Without this, the anchors -written against the clean code- would not fit on
    an already patched file, and the script would abort saying the SDK has
    changed, which would be a false lead.
    """
    if not f.exists():
        return
    txt = f.read_text(encoding="utf-8")
    if not any(m in txt for m in MARCAS_VIEJAS):
        return
    orig = original_de(f)
    if not orig.exists():
        sys.exit(f"[ERROR] {f.name} has a previous patch but there is no\n"
                 f"        {orig.name} to undo it. Restore it from the\n"
                 f"        SDK repository and try again.")
    shutil.copy2(orig, f)
    print(f"[ok] {f.name}: previous version of the patch removed")


def aplicar(f, anclas):
    txt = f.read_text(encoding="utf-8")

    if MARCA in txt:
        print(f"[ok] {f.name}: already up to date, leaving it alone")
        return

    for nombre, ancla, _ in anclas:
        n = txt.count(ancla)
        if n != 1:
            sys.exit(f"[ERROR] In {f.name}, the anchor '{nombre}' appears {n} times,\n"
                     f"        expected 1. The SDK must have changed. I have not touched anything.")

    orig = original_de(f)
    if not orig.exists():
        shutil.copy2(f, orig)
        print(f"[ok] Backup: {orig.name}")

    for _, ancla, nuevo in anclas:
        txt = txt.replace(ancla, nuevo)
    f.write_text(txt, encoding="utf-8")
    print(f"[ok] Patched {f.name}")


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    sdk = localizar_sdk()
    audio = sdk / "src" / "audio"
    trabajo = [
        (audio / "xma_context.cpp", XMA_ANCLAS),
        (audio / "audio_system.cpp", SISTEMA_ANCLAS),
        (audio / "sdl" / "sdl_audio_driver.cpp", SDL_ANCLAS),
    ]
    # v1 touched xma_decoder.cpp -it gave the worker's Wait a 4 ms deadline-.
    # It proved useless: each context turns itself off after one pass, so
    # sweeping more often finds the 320 turned off. And on a two-core machine
    # it was wasted CPU. It is undone if still installed.
    f_dec = audio / "xma_decoder.cpp"

    if args.estado:
        for f, _ in trabajo:
            if not f.exists():
                print(f"  {f.name:24s} DOES NOT EXIST")
                continue
            t = f.read_text(encoding="utf-8")
            if MARCA in t:
                estado = "v3 applied"
            elif any(m in t for m in MARCAS_VIEJAS):
                estado = "OLDER version installed (will be changed on apply)"
            else:
                estado = "not applied"
            print(f"  {f.name:24s} {estado}")
        if f_dec.exists() and any(m in f_dec.read_text(encoding="utf-8") for m in MARCAS_VIEJAS):
            print(f"  {f_dec.name:24s} v1 still installed (will be removed on apply)")
        return 0

    if args.revertir:
        for f in [t[0] for t in trabajo] + [f_dec]:
            orig = original_de(f)
            if orig.exists():
                shutil.copy2(orig, f)
                print(f"[ok] Restored {f.name} from {orig.name}")
        print()
        print("  THE SDK MUST BE RECOMPILED for this to do anything.")
        return 0

    restaurar_si_hay_version_vieja(f_dec)
    for f, anclas in trabajo:
        if not f.exists():
            sys.exit(f"[ERROR] Cannot find {f}")
        restaurar_si_hay_version_vieja(f)
        aplicar(f, anclas)

    print()
    print("  The three pieces of the audio chain will be shown:")
    print("    - the game's kicks and why Work() does not decode")
    print("    - a one-per-second heartbeat from the audio thread, which does NOT go silent")
    print("    - whether the game keeps delivering audio, and the output's silence")
    print()
    print("  It must be launched with --log_noisy=true (LOG_DETALLADO.bat does it).")
    print()
    print("  THE SDK MUST BE RECOMPILED for this to do anything:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
