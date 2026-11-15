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

#include <chrono>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string_view>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");

namespace rex::input::sdl {

namespace {

// SDL clamps to SDL_MAX_RUMBLE_DURATION_MS, which is not a public constant.
constexpr uint32_t kRumbleDurationMs = 0xFFFF;

const char* JoystickTypeName(SDL_JoystickType t) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_UNKNOWN:
      return "Unknown";
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      return "Gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL:
      return "Wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return "ArcadeStick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return "FlightStick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return "DancePad";
    case SDL_JOYSTICK_TYPE_GUITAR:
      return "Guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return "DrumKit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return "ArcadePad";
    case SDL_JOYSTICK_TYPE_THROTTLE:
      return "Throttle";
    default:
      return "?";
  }
}

const char* XInputSubTypeName(uint8_t s) {
  switch (s) {
    case XINPUT_DEVSUBTYPE_GAMEPAD:
      return "GAMEPAD";
    case XINPUT_DEVSUBTYPE_WHEEL:
      return "WHEEL";
    case XINPUT_DEVSUBTYPE_ARCADE_STICK:
      return "ARCADE_STICK";
    case XINPUT_DEVSUBTYPE_FLIGHT_STICK:
      return "FLIGHT_STICK";
    case XINPUT_DEVSUBTYPE_DANCE_PAD:
      return "DANCE_PAD";
    case XINPUT_DEVSUBTYPE_GUITAR:
      return "GUITAR";
    case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE:
      return "GUITAR_ALTERNATE";
    case XINPUT_DEVSUBTYPE_DRUM_KIT:
      return "DRUM_KIT";
    case XINPUT_DEVSUBTYPE_GUITAR_BASS:
      return "GUITAR_BASS";
    case XINPUT_DEVSUBTYPE_ARCADE_PAD:
      return "ARCADE_PAD";
    default:
      return "?";
  }
}

// 'needle' must be lowercase ASCII.
bool NameContainsCI(const char* name, std::string_view needle) {
  if (!name || needle.empty()) {
    return false;
  }
  const std::string_view haystack(name);
  return std::search(
             haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
             }) != haystack.end();
}

// SDL_JoystickType numbering diverges from XINPUT_DEVSUBTYPE_* past value 6,
// so it cannot simply be cast.
uint8_t SdlTypeToXInputSubType(SDL_JoystickType t, const char* name) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      // SDL's XInput backend bakes the SubType into the device name, e.g.
      // "XInput Guitar #1".
      if (NameContainsCI(name, "guitar")) {
        return XINPUT_DEVSUBTYPE_GUITAR;
      }
      if (NameContainsCI(name, "drum")) {
        return XINPUT_DEVSUBTYPE_DRUM_KIT;
      }
      if (NameContainsCI(name, "wheel")) {
        return XINPUT_DEVSUBTYPE_WHEEL;
      }
      if (NameContainsCI(name, "dancepad") || NameContainsCI(name, "dance pad")) {
        return XINPUT_DEVSUBTYPE_DANCE_PAD;
      }
      if (NameContainsCI(name, "flightstick") || NameContainsCI(name, "flight stick") ||
          NameContainsCI(name, "hotas")) {
        return XINPUT_DEVSUBTYPE_FLIGHT_STICK;
      }
      if (NameContainsCI(name, "arcadepad") || NameContainsCI(name, "arcade pad")) {
        return XINPUT_DEVSUBTYPE_ARCADE_PAD;
      }
      if (NameContainsCI(name, "arcade")) {
        return XINPUT_DEVSUBTYPE_ARCADE_STICK;
      }
      return XINPUT_DEVSUBTYPE_GAMEPAD;
    case SDL_JOYSTICK_TYPE_WHEEL:
      return XINPUT_DEVSUBTYPE_WHEEL;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return XINPUT_DEVSUBTYPE_ARCADE_STICK;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return XINPUT_DEVSUBTYPE_FLIGHT_STICK;
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return XINPUT_DEVSUBTYPE_DANCE_PAD;
    case SDL_JOYSTICK_TYPE_GUITAR:
      return XINPUT_DEVSUBTYPE_GUITAR;
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return XINPUT_DEVSUBTYPE_DRUM_KIT;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return XINPUT_DEVSUBTYPE_ARCADE_PAD;
    default:
      return XINPUT_DEVSUBTYPE_GAMEPAD;
  }
}

uint8_t GamepadSubType(SDL_Gamepad* gamepad) {
  return SdlTypeToXInputSubType(SDL_GetJoystickType(SDL_GetGamepadJoystick(gamepad)),
                                SDL_GetGamepadName(gamepad));
}
// The XInput button of each SDL_GamepadButton up to the touchpad.
constexpr std::array<std::underlying_type_t<X_INPUT_GAMEPAD_BUTTON>, SDL_GAMEPAD_BUTTON_TOUCHPAD + 1>
    kButtons = {
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

// Poll reads the pads' state, so SDL need not build and dispatch an event per sample.
constexpr SDL_EventType kSampleEvents[] = {
    SDL_EVENT_JOYSTICK_AXIS_MOTION,    SDL_EVENT_JOYSTICK_BALL_MOTION,
    SDL_EVENT_JOYSTICK_HAT_MOTION,     SDL_EVENT_JOYSTICK_BUTTON_DOWN,
    SDL_EVENT_JOYSTICK_BUTTON_UP,      SDL_EVENT_JOYSTICK_UPDATE_COMPLETE,
    SDL_EVENT_GAMEPAD_AXIS_MOTION,     SDL_EVENT_GAMEPAD_BUTTON_DOWN,
    SDL_EVENT_GAMEPAD_BUTTON_UP,       SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN,
    SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION, SDL_EVENT_GAMEPAD_TOUCHPAD_UP,
    SDL_EVENT_GAMEPAD_SENSOR_UPDATE,   SDL_EVENT_GAMEPAD_UPDATE_COMPLETE,
    SDL_EVENT_SENSOR_UPDATE,
};

}  // namespace

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
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

      // The pads' device events are this driver's alone: the filter takes them before they
      // reach the queue, which nothing polls for them.
      SDL_SetEventFilter(
          [](void* userdata, SDL_Event* event) -> bool {
            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return true;
            }
            static_cast<SDLInputDriver*>(userdata)->HandleEvent(*event);
            return false;
          },
          this);

      // SDL's event loop would wake every millisecond to poll the open pads, under Wine a
      // handful of wineserver round trips each; the poll thread updates them instead.
      SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;
      for (const SDL_EventType type : kSampleEvents) {
        SDL_SetEventEnabled(type, false);
      }

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
                         REXCVAR_GET(hid_mappings_file), SDL_GetError());
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      poll_thread_ = std::thread([this] { PollThread(); });
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (poll_thread_.joinable()) {
      // A poll already started finishes before the pads close.
      {
        std::lock_guard<std::mutex> guard(poll_mutex_);
        polling_stopped_ = true;
      }
      poll_wake_.notify_all();
      poll_thread_.join();
    }
    {
      std::lock_guard<std::mutex> guard(controllers_mutex_);
      for (auto& controller : controllers_) {
        if (controller.sdl) {
          SDL_CloseGamepad(controller.sdl);
        }
        for (const WheelPart& part : controller.wheel.parts) {
          SDL_CloseJoystick(part.sdl);
        }
      }
      controllers_.clear();
    }
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_SetEventFilter(nullptr, nullptr);
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

  std::lock_guard<std::mutex> guard(controllers_mutex_);

  for (const auto& controller : controllers_) {
    // A button box with no wheel yet is not a controller.
    if (!controller.sdl && OnlyAccessories(controller.wheel)) {
      continue;
    }
    DeviceInfo info;
    info.id = controller.id;
    info.name = controller.name;
    info.guid = controller.guid;
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

  std::lock_guard<std::mutex> guard(controllers_mutex_);

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  auto is_active = this->is_active();

  std::lock_guard<std::mutex> guard(controllers_mutex_);

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

  std::lock_guard<std::mutex> guard(controllers_mutex_);
  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // A rumble report is device I/O (a HID feature report and several wineserver round trips
  // under Wine), and racing titles change the motor speeds every frame: Poll sends the latest
  // speeds instead of the guest thread waiting on it here.
  controller->vibration_low = vibration->left_motor_speed;
  controller->vibration_high = vibration->right_motor_speed;
  controller->vibration_pending = true;
  // As on the console, setting the motors of a connected pad succeeds.
  return X_ERROR_SUCCESS;
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

  std::lock_guard<std::mutex> guard(controllers_mutex_);

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
  // On the thread that sends the event, with SDL's joystick lock held: the poll thread
  // (Poll), or the UI thread while the subsystems start. Opening a pad here would take
  // controllers_mutex_ under that lock, the reverse of Poll's order.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

void SDLInputDriver::PollThread() {
  SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
  std::unique_lock<std::mutex> lock(poll_mutex_);
  while (!polling_stopped_) {
    Poll();
    poll_wake_.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs),
                        [this] { return polling_stopped_; });
  }
}

void SDLInputDriver::Poll() {
  // Reading a pad can be slow device I/O (under Wine a HID read is several wineserver and
  // winedevice round trips, ~0.7 ms in all for the Deck's own controller), which the guest
  // thread must not wait on: its reads take the state this left, at most kPollIntervalMs old.
  SDL_UpdateJoysticks();

  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    device_events_.swap(pending_events_);
  }
  for (const SDL_Event& event : device_events_) {
    // Wheels are opened as joysticks (wheel_input.cpp); every other pad
    // through its gamepad mapping. Each kind leaves on its own removal event.
    if (event.type == SDL_EVENT_JOYSTICK_ADDED) {
      LogJoystickAdded(event.jdevice.which);
      if (IsWheel(event.jdevice.which)) {
        AddWheel(event.jdevice.which);
      }
    } else if (event.type == SDL_EVENT_JOYSTICK_REMOVED) {
      RemoveWheelPart(event.jdevice.which);
    } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
      if (IsIgnored(event.gdevice.which)) {
        REXLOG_INFO("SDL: \"{}\" is in --hid_ignore_devices, not used",
                    SDL_GetJoystickNameForID(event.gdevice.which));
      } else if (!IsWheel(event.gdevice.which)) {  // button boxes went to the wheel
        AddController(event.gdevice.which);
      }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
      RemoveController(event.gdevice.which);
    }
  }
  device_events_.clear();

  {
    std::lock_guard<std::mutex> guard(controllers_mutex_);
    for (auto& controller : controllers_) {
      const X_INPUT_GAMEPAD pad =
          controller.sdl ? ReadGamepad(controller.sdl) : ReadWheel(controller.wheel);
      if (std::memcmp(&pad, &controller.state.gamepad, sizeof(pad)) != 0) {
        controller.state.gamepad = pad;
        controller.state_changed = true;
      }
      // Drivers report some of what the capabilities hold, such as the power state, only some
      // time after the pad connects.
      controller.caps = controller.sdl ? ReadCapabilities(controller.sdl, controller.subtype)
                                       : ReadWheelCapabilities(controller.wheel);
      if (controller.vibration_pending) {
        controller.vibration_pending = false;
        rumbles_.push_back({controller.sdl, controller.wheel.sdl, controller.vibration_low,
                            controller.vibration_high});
      }
    }
  }
  // XInput vibration holds until the guest changes it, but SDL rumble expires, and a zero
  // duration expires on the next SDL_UpdateJoysticks. Arm it for SDL's maximum instead; for
  // unchanged speeds SDL only moves the expiry and sends nothing to the device.
  for (const Rumble& rumble : rumbles_) {
    if (rumble.sdl) {
      SDL_RumbleGamepad(rumble.sdl, rumble.low, rumble.high, kRumbleDurationMs);
    } else {
      SDL_RumbleJoystick(rumble.wheel, rumble.low, rumble.high, kRumbleDurationMs);
    }
  }
  rumbles_.clear();
}

void SDLInputDriver::LogJoystickAdded(SDL_JoystickID id) {
  // Devices SDL cannot promote to a gamepad, such as some guitars and drum
  // kits, never reach the gamepad event and would otherwise vanish silently.
  const char* name = SDL_GetJoystickNameForID(id);
  char guid_text[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_text, static_cast<int>(sizeof(guid_text)));
  const bool is_gamepad = SDL_IsGamepad(id) || IsWheel(id);
  if (IsIgnored(id)) {
    REXLOG_INFO("SDL JoystickAdded: \"{}\" is in --hid_ignore_devices, not used",
                SDL_GetJoystickNameForID(id));
    return;
  }
  REXLOG_INFO(
      "SDL JoystickAdded: \"{}\", VendorID(0x{:04X}), ProductID(0x{:04X}), GUID({}), "
      "HasGamepadMapping({})",
      name ? name : "?", SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id), guid_text,
      is_gamepad);
  if (!is_gamepad) {
    REXLOG_WARN(
        "SDL JoystickAdded: \"{}\" has no gamepad mapping, so it will not be usable. Add one to "
        "the file named by --hid_mappings_file.",
        name ? name : "?");
  }
}

void SDLInputDriver::AddController(SDL_JoystickID instance_id) {
  SDL_Gamepad* gamepad = SDL_OpenGamepad(instance_id);
  if (!gamepad) {
    REXLOG_ERROR("SDL OnControllerDeviceAdded: cannot open gamepad {}: {}", instance_id,
                 SDL_GetError());
    return;
  }
  SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);

  ControllerState controller = {};
  controller.sdl = gamepad;
  controller.instance_id = instance_id;
  const char* name = SDL_GetGamepadName(gamepad);
  controller.name = name ? name : "";
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, static_cast<int>(sizeof(guid)));
  controller.guid = guid;
  controller.subtype = GamepadSubType(gamepad);
  const SDL_JoystickType joy_type = SDL_GetJoystickType(joystick);
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "XInputSubType({} = 0x{:02X}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X}), "
      "GUID({})",
      controller.name, JoystickTypeName(joy_type), static_cast<int>(SDL_GetGamepadType(gamepad)),
      XInputSubTypeName(controller.subtype), controller.subtype, SDL_GetGamepadVendor(gamepad),
      SDL_GetGamepadProduct(gamepad), controller.guid);

  controller.caps = ReadCapabilities(gamepad, controller.subtype);
  controller.state.gamepad = ReadGamepad(gamepad);
  controller.state_changed = true;  // XInput starts with packet_number = 1
  controller.id = static_cast<DeviceId>(next_device_id_++);
  const DeviceId id = controller.id;
  size_t ordinal;
  {
    std::lock_guard<std::mutex> guard(controllers_mutex_);
    ordinal = controllers_.size();
    controllers_.push_back(std::move(controller));
  }

  // SDL_GetGamepadPlayerIndex is deliberately ignored: on Windows it reports
  // the XInput user index the OS assigned, which is unrelated to connection
  // order and leaves non-XInput pads unnumbered.
  SDL_SetGamepadPlayerIndex(gamepad, static_cast<int>(ordinal));
  REXLOG_INFO("SDL OnControllerDeviceAdded: connection order {}, device {}.", ordinal,
              static_cast<uint64_t>(id));
  const char* mapping = SDL_GetGamepadMapping(gamepad);
  REXLOG_INFO("SDL Controller {}: {}", ordinal, mapping ? mapping : "<none>");
}

void SDLInputDriver::RemoveController(SDL_JoystickID instance_id) {
  SDL_Gamepad* gamepad = nullptr;
  SDL_Joystick* wheel = nullptr;
  {
    std::lock_guard<std::mutex> guard(controllers_mutex_);
    const auto it = std::find_if(
        controllers_.begin(), controllers_.end(),
        [instance_id](const ControllerState& c) { return c.instance_id == instance_id; });
    if (it == controllers_.end()) {
      // Expected when the pad was never opened, e.g. it had no gamepad mapping.
      REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored, instance_id({}) not in use.",
                  instance_id);
      return;
    }
    REXLOG_INFO("SDL OnControllerDeviceRemoved: \"{}\", GUID({}).", it->name, it->guid);
    gamepad = it->sdl;
    wheel = it->wheel.sdl;
    controllers_.erase(it);
  }
  if (gamepad) {
    SDL_CloseGamepad(gamepad);
  } else {
    SDL_CloseJoystick(wheel);
  }
  // LEDs only. Guest user assignment does not shift, because InputSystem holds
  // each device's ordinal. Poll alone changes controllers_, so it reads it unlocked.
  for (size_t i = 0; i < controllers_.size(); i++) {
    if (controllers_[i].sdl) {
      SDL_SetGamepadPlayerIndex(controllers_[i].sdl, static_cast<int>(i));
    } else {
      SDL_SetJoystickPlayerIndex(controllers_[i].wheel.sdl, static_cast<int>(i));
    }
  }
  REXLOG_INFO("SDL OnControllerDeviceRemoved: {} device(s) remain.", controllers_.size());
}

X_INPUT_GAMEPAD SDLInputDriver::ReadGamepad(SDL_Gamepad* gamepad) {
  // SDL's state as of the last SDL_UpdateJoysticks, through the pad's mapping: no device I/O.
  uint16_t buttons = 0;
  for (size_t i = 0; i < kButtons.size(); ++i) {
    if (SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(i))) {
      buttons |= static_cast<uint16_t>(kButtons[i]);
    }
  }
  // SDL's triggers run from 0 to 32767, and its sticks' Y axes point down.
  X_INPUT_GAMEPAD pad = {};
  pad.buttons = buttons;
  pad.left_trigger =
      static_cast<uint8_t>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
  pad.right_trigger =
      static_cast<uint8_t>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
  pad.thumb_lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  pad.thumb_ly = static_cast<int16_t>(~SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
  pad.thumb_rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
  pad.thumb_ry = static_cast<int16_t>(~SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
  RemapPadButtons(pad);
  return pad;
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

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_DEBUG("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION);
  return true;
}

X_INPUT_CAPABILITIES SDLInputDriver::ReadCapabilities(SDL_Gamepad* gamepad, uint8_t subtype) {
  uint16_t cap_flags = 0x0;

  SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
  if (SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  } else {
    // Backends leaving the connection state UNKNOWN still report power.
    const SDL_PowerState power_state = SDL_GetJoystickPowerInfo(joystick, nullptr);
    if (power_state == SDL_POWERSTATE_ON_BATTERY || power_state == SDL_POWERSTATE_CHARGING ||
        power_state == SDL_POWERSTATE_CHARGED) {
      cap_flags |= X_INPUT_CAPS_WIRELESS;
    }
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (const SDL_GamepadButton button : nav_buttons) {
    if (!SDL_GamepadHasButton(gamepad, button)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  X_INPUT_CAPABILITIES c = {};
  c.type = XINPUT_DEVTYPE_GAMEPAD;
  c.sub_type = subtype;
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
  return c;
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
