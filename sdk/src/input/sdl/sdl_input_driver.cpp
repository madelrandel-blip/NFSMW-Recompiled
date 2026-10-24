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
#include <cstdlib>
#include <string>
#include <string_view>
#include <filesystem>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");

REXCVAR_DEFINE_STRING(mando_mapa, "", "Input",
                      "Remapeo del mando: pares boton_fisico=boton_del_juego separados por comas, p. ej. \"a=b,b=a\". "
                      "Nombres: a b x y back start guide ls rs lb rb up down left right");
REXCVAR_DEFINE_BOOL(volante_activo, true, "Input",
                    "Usar como mando los volantes y joysticks que SDL no reconoce como mando");
REXCVAR_DEFINE_INT32(volante_eje_direccion, 0, "Input", "Eje del volante para girar").range(0, 15);
REXCVAR_DEFINE_INT32(volante_eje_acelerador, 1, "Input", "Eje del pedal del acelerador (gatillo derecho)")
    .range(0, 15);
REXCVAR_DEFINE_INT32(volante_eje_freno, 2, "Input", "Eje del pedal del freno (gatillo izquierdo)").range(0, 15);
REXCVAR_DEFINE_BOOL(volante_pedales_invertidos, true, "Input",
                    "Los pedales sueltos dan el valor maximo del eje (lo normal en volantes)");
REXCVAR_DEFINE_INT32(volante_giro, 100, "Input",
                     "Cuanto giro del volante hace falta para el giro completo, en %: menos = mas sensible")
    .range(10, 100);
REXCVAR_DEFINE_INT32(volante_zona_muerta, 2, "Input", "Zona muerta del volante y los pedales, en %").range(0, 30);
REXCVAR_DEFINE_STRING(volante_botones, "a=0,b=1,x=2,y=3,rb=4,lb=5,back=8,start=9", "Input",
                      "Botones del volante: pares boton_del_juego=numero_de_boton, separados por comas");

namespace rex::input::sdl {

namespace {

// SDL clamps to SDL_MAX_RUMBLE_DURATION_MS, which is not a public constant.
constexpr uint32_t kRumbleDurationMs = 0xFFFF;

// Button names used by mando_mapa and volante_botones, in X_INPUT_GAMEPAD terms.
struct NombreBoton {
  const char* nombre;
  uint16_t boton;
};
constexpr NombreBoton kNombresBoton[] = {
    {"a", X_INPUT_GAMEPAD_A},
    {"b", X_INPUT_GAMEPAD_B},
    {"x", X_INPUT_GAMEPAD_X},
    {"y", X_INPUT_GAMEPAD_Y},
    {"back", X_INPUT_GAMEPAD_BACK},
    {"start", X_INPUT_GAMEPAD_START},
    {"guide", X_INPUT_GAMEPAD_GUIDE},
    {"ls", X_INPUT_GAMEPAD_LEFT_THUMB},
    {"rs", X_INPUT_GAMEPAD_RIGHT_THUMB},
    {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {"up", X_INPUT_GAMEPAD_DPAD_UP},
    {"down", X_INPUT_GAMEPAD_DPAD_DOWN},
    {"left", X_INPUT_GAMEPAD_DPAD_LEFT},
    {"right", X_INPUT_GAMEPAD_DPAD_RIGHT},
};

uint16_t BotonPorNombre(std::string_view n) {
  for (const auto& b : kNombresBoton) {
    if (n == b.nombre) {
      return b.boton;
    }
  }
  return 0;
}

// "a=b,x=y": calls f(left, right) for each pair.
template <typename F>
void RecorrerPares(const std::string& texto, F f) {
  std::string_view resto(texto);
  while (!resto.empty()) {
    const size_t coma = resto.find(',');
    const std::string_view par = resto.substr(0, coma);
    resto = coma == std::string_view::npos ? std::string_view() : resto.substr(coma + 1);
    const size_t igual = par.find('=');
    if (igual != std::string_view::npos) {
      f(par.substr(0, igual), par.substr(igual + 1));
    }
  }
}

// The X button a physical gamepad button produces, after mando_mapa ("a=b": the A button acts as B).
uint16_t RemapearBoton(uint16_t fisico) {
  uint16_t resultado = fisico;
  RecorrerPares(REXCVAR_GET(mando_mapa), [&](std::string_view de, std::string_view a) {
    if (BotonPorNombre(de) == fisico) {
      resultado = BotonPorNombre(a);
    }
  });
  return resultado;
}

// The X buttons wheel button `n` produces (volante_botones: "a=0,b=1").
uint16_t BotonDelVolante(int n) {
  uint16_t resultado = 0;
  const std::string numero_n = std::to_string(n);
  RecorrerPares(REXCVAR_GET(volante_botones), [&](std::string_view boton, std::string_view numero) {
    if (numero == numero_n) {
      resultado |= BotonPorNombre(boton);
    }
  });
  return resultado;
}

// A pedal axis to 0..255. Most wheels report a released pedal as +32767 (volante_pedales_invertidos).
uint8_t Pedal(int16_t v) {
  int pulsado = REXCVAR_GET(volante_pedales_invertidos) ? 32767 - v : v + 32768;  // 0..65535
  const int muerta = REXCVAR_GET(volante_zona_muerta) * 65535 / 100;
  pulsado = pulsado <= muerta ? 0 : int(int64_t(pulsado - muerta) * 65535 / std::max(1, 65535 - muerta));
  return uint8_t(std::clamp(pulsado, 0, 65535) >> 8);
}

// The steering axis to left stick X. volante_giro 100 = full lock is full stick; lower needs less wheel.
int16_t Direccion(int16_t v) {
  const int giro = std::max(10, int(REXCVAR_GET(volante_giro)));
  const int muerta = REXCVAR_GET(volante_zona_muerta) * 32767 / 100;
  int x = v;
  if (std::abs(x) <= muerta) {
    return 0;
  }
  x = int(int64_t(x > 0 ? x - muerta : x + muerta) * 32767 / std::max(1, 32767 - muerta));
  return int16_t(std::clamp(x * 100 / giro, -32768, 32767));
}

}  // namespace

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_() {}

SDLInputDriver::~SDLInputDriver() {}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
    window->app_context().CallInUIThreadSynchronous([this]() {
      // Initialize SDL events subsystem
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // With an event watch we will always get notified, even if the event queue
      // is full, which can happen if another subsystem does not clear its events.
      SDL_AddEventWatch(
          [](void* userdata, SDL_Event* event) -> bool {
            if (!userdata || !event) {
              assert_always();
              return false;
            }

            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return false;
            }

            // If another part of rex uses another SDL subsystem that generates
            // events, this may seem like a bad idea. They will however not
            // subscribe to controller events so we get away with that.
            const auto driver = static_cast<SDLInputDriver*>(userdata);
            driver->HandleEvent(*event);

            return false;
          },
          this);

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;

      // Load custom controller mappings if available
      if (!REXCVAR_GET(hid_mappings_file).empty()) {
        std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                      REXCVAR_GET(hid_mappings_file));
        } else {
          auto mappings_result =
              SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                         REXCVAR_GET(hid_mappings_file), mappings_result);
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    for (auto& controller : controllers_) {
      if (controller.joy) {
        SDL_CloseJoystick(controller.joy);
      } else {
        SDL_CloseGamepad(controller.sdl);
      }
    }
    controllers_.clear();
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Polling can start before the window exists and the subsystems are up.
  if (!sdl_events_initialized_ || !SDL_Gamepad_initialized_) {
    return;
  }

  auto guard = DrainAndLock();

  for (const auto& controller : controllers_) {
    DeviceInfo info;
    info.id = controller.id;
    SDL_Joystick* joystick = controller.joy ? controller.joy : SDL_GetGamepadJoystick(controller.sdl);
    const char* name = SDL_GetJoystickName(joystick);
    info.name = name ? name : "";
    char guid_text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid_text,
                     static_cast<int>(sizeof(guid_text)));
    info.guid = guid_text;
    info.synthetic = false;
    out.push_back(info);
  }
}

X_RESULT SDLInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // XInput vibration holds until the guest changes it, but SDL rumble expires,
  // and a zero duration expires on the next SDL_UpdateJoysticks. Arm it for
  // SDL's maximum instead; each call cancels the previous effect anyway.
  if (controller->joy) {
    // Wheels: the plain rumble most of them map to their force feedback motor, if any.
    SDL_RumbleJoystick(controller->joy, vibration->left_motor_speed, vibration->right_motor_speed,
                       kRumbleDurationMs);
    return X_ERROR_SUCCESS;
  }
  return SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed,
                           vibration->right_motor_speed, kRumbleDurationMs)
             ? X_ERROR_SUCCESS
             : X_ERROR_FUNCTION_FAILED;
}

X_RESULT SDLInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // If input is not active (e.g. due to a dialog overlay), force buttons to
  // "unpressed". The algorithm will automatically send UP events when
  // 'is_active()' goes low and DOWN events when it goes high again.
  const uint64_t curr_butts =
      is_active ? (static_cast<uint64_t>(static_cast<uint16_t>(controller->state.gamepad.buttons)) |
                   AnalogToKeyfield(controller->state.gamepad))
                : uint64_t(0);
  KeystrokeState& last = controller->keystroke;

  // Handle repeating
  auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
  static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
  if (last.repeat_state == RepeatState::Waiting &&
      (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
    last.repeat_state = RepeatState::Repeating;
  }
  if (last.repeat_state == RepeatState::Repeating &&
      (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
    last.repeat_time = guest_now;
    rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
    assert_true(vk != rex::ui::VirtualKey::kNone);
    out_keystroke->virtual_key = uint16_t(vk);
    out_keystroke->unicode = 0;
    // InputSystem stamps the guest user this device is assigned to.
    out_keystroke->user_index = 0;
    out_keystroke->hid_code = 0;
    out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    return X_ERROR_SUCCESS;
  }

  auto butts_changed = curr_butts ^ last.buttons;
  if (!butts_changed) {
    return X_ERROR_EMPTY;
  }

  // First try to clear buttons with up events. This is to match xinput
  // behavior when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
  // up before THUMB_LEFT is down.
  for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
    for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
      auto fbutton = uint64_t(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      rex::ui::VirtualKey vk = kVkLookup.at(i);
      if (vk == rex::ui::VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;

      bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        // up
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        last.buttons &= ~fbutton;
        last.repeat_state = RepeatState::Idle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        // down
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        last.buttons |= fbutton;
        last.repeat_state = RepeatState::Waiting;
        last.repeat_butt_idx = i;
        last.repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_ADDED:
      OnWheelAddedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_REMOVED:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
      OnWheelEventLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  const auto controller = SDL_OpenGamepad(event.gdevice.which);
  if (!controller) {
    assert_always();
    return;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));

  // SDL_GetGamepadPlayerIndex is deliberately ignored: on Windows it reports
  // the XInput user index the OS assigned, which is unrelated to connection
  // order and leaves non-XInput pads unnumbered.
  ControllerState state = {};
  state.sdl = controller;
  state.id = AllocateDeviceId();
  state.state_changed = true;  // XInput starts with packet_number = 1
  UpdateXCapabilities(state);
  controllers_.push_back(state);

  const int ordinal = static_cast<int>(controllers_.size()) - 1;
  SDL_SetGamepadPlayerIndex(controller, ordinal);

  REXLOG_INFO("SDL OnControllerDeviceAdded: connection order {}, device {}.", ordinal,
              static_cast<uint64_t>(state.id));
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored. Unknown device.");
    return;
  }
  SDL_CloseGamepad(controllers_.at(*idx).sdl);
  controllers_.erase(controllers_.begin() + static_cast<ptrdiff_t>(*idx));

  // LEDs only. Guest user assignment does not shift, because InputSystem holds
  // each device's ordinal.
  for (size_t i = 0; i < controllers_.size(); i++) {
    if (controllers_[i].sdl) {
      SDL_SetGamepadPlayerIndex(controllers_[i].sdl, static_cast<int>(i));
    }
  }
  REXLOG_INFO("SDL OnControllerDeviceRemoved: {} device(s) remain.", controllers_.size());
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& pad = controllers_.at(*idx).state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  controllers_.at(*idx).state_changed = true;
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  // Define a lookup table to map between SDL and XInput button codes.
  // These need to be in the order of the SDL_GamepadButton enum.
  static constexpr std::array<std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          // Standard buttons:
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          // There are additional buttons only available on some controllers.
          // For now just assign sensible defaults
          // Misc:
          X_INPUT_GAMEPAD_GUIDE,
          // Xbox Elite paddles:
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          // PS touchpad button
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& controller = controllers_.at(*idx);

  uint16_t xbuttons = controller.state.gamepad.buttons;
  // Lookup the XInput button code.
  if (event.gbutton.button >= xbutton_lookup.size()) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = RemapearBoton(xbutton_lookup.at(event.gbutton.button));
  if (!xbutton) {
    return;  // mapped to a name that does not exist: the button does nothing
  }
  // Pressed or released?
  if (event.gbutton.down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return;
    }
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
}

// Steering wheels and other joysticks SDL does not know as gamepads (volante_activo). Gamepads also
// raise joystick events; those are left to the gamepad path.
void SDLInputDriver::OnWheelAddedLocked(const SDL_Event& event) {
  if (!REXCVAR_GET(volante_activo) || SDL_IsGamepad(event.jdevice.which) ||
      GetControllerIndexFromInstanceID(event.jdevice.which)) {
    return;
  }
  SDL_Joystick* joy = SDL_OpenJoystick(event.jdevice.which);
  if (!joy) {
    return;
  }
  ControllerState state = {};
  state.joy = joy;
  state.id = AllocateDeviceId();
  state.state_changed = true;
  UpdateXCapabilities(state);
  controllers_.push_back(state);
  REXLOG_INFO("SDL volante: \"{}\" ({} ejes, {} botones, tipo {}), dispositivo {}", SDL_GetJoystickName(joy),
              SDL_GetNumJoystickAxes(joy), SDL_GetNumJoystickButtons(joy),
              static_cast<int>(SDL_GetJoystickType(joy)), static_cast<uint64_t>(state.id));
}

void SDLInputDriver::OnWheelEventLocked(const SDL_Event& event) {
  const auto idx = GetControllerIndexFromInstanceID(event.jdevice.which);
  if (!idx || !controllers_.at(*idx).joy) {
    return;
  }
  auto& controller = controllers_.at(*idx);
  auto& pad = controller.state.gamepad;
  switch (event.type) {
    case SDL_EVENT_JOYSTICK_REMOVED:
      SDL_CloseJoystick(controller.joy);
      controllers_.erase(controllers_.begin() + static_cast<ptrdiff_t>(*idx));
      REXLOG_INFO("SDL volante desconectado");
      return;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
      if (event.jaxis.axis == REXCVAR_GET(volante_eje_direccion)) {
        pad.thumb_lx = Direccion(event.jaxis.value);
      } else if (event.jaxis.axis == REXCVAR_GET(volante_eje_acelerador)) {
        pad.right_trigger = Pedal(event.jaxis.value);
      } else if (event.jaxis.axis == REXCVAR_GET(volante_eje_freno)) {
        pad.left_trigger = Pedal(event.jaxis.value);
      } else {
        return;
      }
      break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP: {
      const uint16_t botones = BotonDelVolante(event.jbutton.button);
      if (!botones) {
        return;
      }
      pad.buttons = event.jbutton.down ? (pad.buttons | botones) : (pad.buttons & ~botones);
      break;
    }
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
      // The wheel's hat is the D-pad.
      const uint16_t cruceta = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN |
                               X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
      uint16_t b = pad.buttons & ~cruceta;
      if (event.jhat.value & SDL_HAT_UP) b |= X_INPUT_GAMEPAD_DPAD_UP;
      if (event.jhat.value & SDL_HAT_DOWN) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
      if (event.jhat.value & SDL_HAT_LEFT) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
      if (event.jhat.value & SDL_HAT_RIGHT) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
      pad.buttons = b;
      break;
    }
    default:
      return;
  }
  controller.state_changed = true;
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto joystick = controllers_.at(i).joy ? controllers_.at(i).joy : SDL_GetGamepadJoystick(controllers_.at(i).sdl);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::FindController(DeviceId id) {
  if (id == DeviceId::kInvalid) {
    return nullptr;
  }
  for (auto& controller : controllers_) {
    if (controller.id == id) {
      return &controller;
    }
  }
  return nullptr;
}

DeviceId SDLInputDriver::AllocateDeviceId() {
  return static_cast<DeviceId>(next_device_id_++);
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_DEBUG("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl || state.joy);
  uint16_t cap_flags = 0x0;
  if (state.joy) {
    // A wheel is presented as a full gamepad: the game reads the same fields whatever the subtype.
    auto& c = state.caps;
    c.type = 0x01;
    c.sub_type = 0x01;
    c.flags = 0;
    c.gamepad.buttons = 0xF3FF;
    c.gamepad.left_trigger = 0xFF;
    c.gamepad.right_trigger = 0xFF;
    c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
    c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
    c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
    c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
    c.vibration.left_motor_speed = 0xFFFFu;
    c.vibration.right_motor_speed = 0xFFFFu;
    return;
  }

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    attached_window_->app_context().CallInUIThread([this]() {
      SDL_PumpEvents();
      sdl_pumpevents_queued_ = false;
    });
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
