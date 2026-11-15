/**
 * Racing wheels for the SDL input driver.
 *
 * Most PC wheels (Logitech G29, Thrustmaster T300 and the like, through
 * DirectInput or evdev) have no SDL gamepad mapping, so the gamepad path never
 * opens them. They are opened here as plain joysticks and reported to the
 * guest as an Xbox 360 wheel: steering on the left stick, accelerator and
 * brake on the right and left triggers, buttons and the hat as pad buttons.
 * Wheels that speak XInput (G920, Xbox-mode Thrustmasters) already have a
 * mapping and stay on the gamepad path.
 *
 * Wheels differ in which axis is which, so the hid_wheel_* cvars set them; a
 * new wheel logs its axes, and with --hid_wheel_log_axes every axis that moves.
 *
 * Wheel base, pedals, shifter and handbrake are often separate USB devices.
 * Each would be a pad of its own, more players than the game allows, so they
 * join one wheel: axes and buttons numbered across the parts, the wheel base
 * first and the rest in connection order.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(hid_ignore_devices, "", "Input",
                      "Devices never used as controllers: comma-separated parts of their names, "
                      "or VID:PID in hex (e.g. 'button box, 1209:2301'); see the log's SDL "
                      "JoystickAdded lines");
REXCVAR_DEFINE_BOOL(hid_wheel, true, "Input",
                    "Use racing wheels that have no gamepad mapping as an Xbox 360 wheel");
REXCVAR_DEFINE_BOOL(hid_wheel_any_joystick, true, "Input",
                    "Treat every joystick without a gamepad mapping as part of the wheel (shifters "
                    "and pedals on USB adapters have generic names; such a device is unusable "
                    "otherwise)");
REXCVAR_DEFINE_INT32(hid_wheel_steer_axis, 0, "Input", "Wheel axis for steering");
REXCVAR_DEFINE_INT32(hid_wheel_throttle_axis, -1, "Input",
                     "Wheel axis for the accelerator (-1: the first pedal-like axis after steering)");
REXCVAR_DEFINE_INT32(hid_wheel_brake_axis, -1, "Input",
                     "Wheel axis for the brake (-1: the second pedal-like axis after steering; "
                     "the same axis as the accelerator for combined pedals)");
REXCVAR_DEFINE_DOUBLE(hid_wheel_steer_scale, 1.0, "Input",
                      "Steering sensitivity: 2.0 gives full lock at half the wheel's rotation");
REXCVAR_DEFINE_INT32(hid_wheel_deadzone, 0, "Input",
                     "Steering dead zone in axis units (0-32767) around the centre");
REXCVAR_DEFINE_BOOL(hid_wheel_invert_steering, false, "Input", "Reverse the steering direction");
REXCVAR_DEFINE_STRING(hid_wheel_pedals_rest, "auto", "Input",
                      "Where the pedals rest: auto (as read when the wheel connects), high or low");
REXCVAR_DEFINE_STRING(hid_pad_buttons, "", "Input",
                      "Controller button remapping: what each controller button gives, in the "
                      "order A B X Y LB RB LT RT BACK START LS RS UP DOWN LEFT RIGHT, comma "
                      "separated (the same names, or - for nothing). Empty: unchanged");
REXCVAR_DEFINE_STRING(hid_wheel_buttons, "A,B,X,Y,LB,RB,BACK,START,LS,RS", "Input",
                      "Pad button for each wheel button in order, comma separated: A B X Y LB RB "
                      "BACK START LS RS GUIDE UP DOWN LEFT RIGHT LT RT, or - for none");
REXCVAR_DEFINE_BOOL(hid_wheel_as_360_wheel, true, "Input",
                    "Report wheels to the game as an Xbox 360 wheel rather than a gamepad");
REXCVAR_DEFINE_BOOL(hid_wheel_log_axes, false, "Input",
                    "Log every wheel axis and button that changes, to find which is which");

namespace rex::input::sdl {

namespace {

constexpr int kNoAxis = -1;
// A pedal at rest sits near one end of its range.
constexpr int kPedalRestThreshold = 16000;

// Pseudo buttons for the triggers in hid_wheel_buttons.
constexpr uint32_t kLeftTriggerButton = 0x10000;
constexpr uint32_t kRightTriggerButton = 0x20000;

uint32_t ButtonByName(std::string_view name) {
  struct Entry {
    std::string_view name;
    uint32_t button;
  };
  static constexpr Entry kNames[] = {
      {"A", X_INPUT_GAMEPAD_A},
      {"B", X_INPUT_GAMEPAD_B},
      {"X", X_INPUT_GAMEPAD_X},
      {"Y", X_INPUT_GAMEPAD_Y},
      {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"BACK", X_INPUT_GAMEPAD_BACK},
      {"START", X_INPUT_GAMEPAD_START},
      {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},
      {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"GUIDE", X_INPUT_GAMEPAD_GUIDE},
      {"UP", X_INPUT_GAMEPAD_DPAD_UP},
      {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
      {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"LT", kLeftTriggerButton},
      {"RT", kRightTriggerButton},
  };
  for (const Entry& e : kNames) {
    if (e.name.size() == name.size() &&
        std::equal(name.begin(), name.end(), e.name.begin(),
                   [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == b; })) {
      return e.button;
    }
  }
  return 0;
}

// hid_wheel_buttons, parsed again when it changes (the game's Options menu sets it). Poll's
// thread alone reads wheels.
const std::vector<uint32_t>& ButtonMap() {
  static std::string parsed;
  static std::vector<uint32_t> map;
  std::string spec = REXCVAR_GET(hid_wheel_buttons);
  if (spec == parsed && !map.empty()) {
    return map;
  }
  parsed = spec;
  map = [&spec] {
    std::vector<uint32_t> out;
    size_t pos = 0;
    while (pos <= spec.size()) {
      size_t end = spec.find(',', pos);
      if (end == std::string::npos) {
        end = spec.size();
      }
      std::string_view item(spec.data() + pos, end - pos);
      while (!item.empty() && item.front() == ' ') {
        item.remove_prefix(1);
      }
      while (!item.empty() && item.back() == ' ') {
        item.remove_suffix(1);
      }
      out.push_back(ButtonByName(item));
      pos = end + 1;
    }
    return out;
  }();
  return map;
}

bool PedalLike(int16_t rest) { return std::abs(int(rest)) >= kPedalRestThreshold; }

struct Axes {
  int steer = kNoAxis, throttle = kNoAxis, brake = kNoAxis;
};

Axes ChooseAxes(const std::vector<int16_t>& rest) {
  const int count = int(rest.size());
  Axes a;
  a.steer = REXCVAR_GET(hid_wheel_steer_axis);
  a.throttle = REXCVAR_GET(hid_wheel_throttle_axis);
  a.brake = REXCVAR_GET(hid_wheel_brake_axis);
  // Auto: pedals in axis order after steering. Some DirectInput wheels read 0
  // on every axis until it first moves, so with no pedal-like axis the next
  // two axes are taken.
  std::vector<int> pedals;
  for (int i = 0; i < count; ++i) {
    if (i != a.steer && PedalLike(rest[i])) {
      pedals.push_back(i);
    }
  }
  if (pedals.size() < 2) {
    pedals.clear();
    for (int i = 0; i < count; ++i) {
      if (i != a.steer) {
        pedals.push_back(i);
      }
    }
  }
  if (a.throttle < 0 && pedals.size() > 0) {
    a.throttle = pedals[0];
  }
  if (a.brake < 0 && pedals.size() > 1) {
    a.brake = pedals[a.throttle == pedals[0] ? 1 : 0];
  }
  auto valid = [count](int& axis) {
    if (axis >= count) {
      axis = kNoAxis;
    }
  };
  valid(a.steer);
  valid(a.throttle);
  valid(a.brake);
  return a;
}

// 0..1 travel of a pedal on its own axis.
float PedalTravel(int16_t value, int16_t& rest) {
  const std::string mode = REXCVAR_GET(hid_wheel_pedals_rest);
  bool rest_high;
  if (mode == "high") {
    rest_high = true;
  } else if (mode == "low") {
    rest_high = false;
  } else {
    // DirectInput wheels (T248) read 0 on every axis until the device first
    // reports, which gives the pedals' released ends. Until an axis has been
    // at an end it reads released (an axis no pedal drives stays so), and its
    // first end is where it rests.
    if (!PedalLike(rest)) {
      if (std::abs(int(value)) < 30000) {
        return 0.0f;
      }
      rest = value;
    }
    rest_high = rest > 0;
  }
  const float v = (float(value) + 32768.0f) / 65535.0f;  // 0..1
  return std::clamp(rest_high ? 1.0f - v : v, 0.0f, 1.0f);
}

uint8_t ToTrigger(float travel) { return uint8_t(std::lround(std::clamp(travel, 0.0f, 1.0f) * 255.0f)); }

// Combined axis `index`, numbered across the wheel's parts.
template <typename Wheel>
int16_t WheelAxis(const Wheel& wheel, int index) {
  for (const auto& part : wheel.parts) {
    const int count = int(part.rest.size());
    if (index < count) {
      return SDL_GetJoystickAxis(part.sdl, index);
    }
    index -= count;
  }
  return 0;
}

}  // namespace

namespace {
std::string Lower(const char* s) {
  std::string out(s ? s : "");
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char ch) { return char(std::tolower(ch)); });
  return out;
}
}  // namespace

bool SDLInputDriver::IsButtonBox(SDL_JoystickID instance_id) {
  const std::string name = Lower(SDL_GetJoystickNameForID(instance_id));
  for (const char* word : {"button box", "buttonbox", "button-box", "button_box", "btn box"}) {
    if (name.find(word) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool SDLInputDriver::IsIgnored(SDL_JoystickID instance_id) {
  const std::string list = Lower(REXCVAR_GET(hid_ignore_devices).c_str());
  if (list.empty()) {
    return false;
  }
  const std::string name = Lower(SDL_GetJoystickNameForID(instance_id));
  const std::string vid_pid = fmt::format("{:04x}:{:04x}", SDL_GetJoystickVendorForID(instance_id),
                                          SDL_GetJoystickProductForID(instance_id));
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    std::string item = list.substr(start, end - start);
    item.erase(0, item.find_first_not_of(" \t"));
    item.erase(item.find_last_not_of(" \t") + 1);
    if (!item.empty() && (item == vid_pid || name.find(item) != std::string::npos)) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

bool SDLInputDriver::OnlyAccessories(const Wheel& wheel) {
  return std::all_of(wheel.parts.begin(), wheel.parts.end(),
                     [](const WheelPart& p) { return p.accessory; });
}

bool SDLInputDriver::IsWheel(SDL_JoystickID instance_id) {
  if (!REXCVAR_GET(hid_wheel) || IsIgnored(instance_id)) {
    return false;
  }
  // A button box joins the wheel even when SDL maps it as a gamepad.
  if (IsButtonBox(instance_id)) {
    return true;
  }
  if (SDL_IsGamepad(instance_id)) {
    return false;
  }
  if (REXCVAR_GET(hid_wheel_any_joystick) ||
      SDL_GetJoystickTypeForID(instance_id) == SDL_JOYSTICK_TYPE_WHEEL) {
    return true;
  }
  const char* name = SDL_GetJoystickNameForID(instance_id);
  if (!name) {
    return false;
  }
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* word : {"wheel", "racing", "pedal", "shifter", "gearbox", "handbrake", "clutch"}) {
    if (lower.find(word) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void SDLInputDriver::RebuildWheel(Wheel& wheel) {
  std::stable_sort(wheel.parts.begin(), wheel.parts.end(),
                   [](const WheelPart& a, const WheelPart& b) { return a.base && !b.base; });
  wheel.rest.clear();
  for (const WheelPart& part : wheel.parts) {
    wheel.rest.insert(wheel.rest.end(), part.rest.begin(), part.rest.end());
  }
  wheel.sdl = wheel.parts.empty() ? nullptr : wheel.parts.front().sdl;
}

void SDLInputDriver::AddWheel(SDL_JoystickID instance_id) {
  SDL_Joystick* joystick = SDL_OpenJoystick(instance_id);
  if (!joystick) {
    REXLOG_ERROR("SDL wheel: cannot open joystick {}: {}", instance_id, SDL_GetError());
    return;
  }
  WheelPart part;
  part.sdl = joystick;
  part.instance_id = instance_id;
  const char* name = SDL_GetJoystickName(joystick);
  std::string lower(name ? name : "");
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  part.base = SDL_GetJoystickType(joystick) == SDL_JOYSTICK_TYPE_WHEEL ||
              lower.find("wheel") != std::string::npos;
  const int axes = std::max(0, SDL_GetNumJoystickAxes(joystick));
  // Buttons only (or named as a button box): it adds buttons to a wheel but
  // never takes a controller slot of its own.
  part.accessory = !part.base && (axes == 0 || IsButtonBox(instance_id));
  for (int i = 0; i < axes; ++i) {
    part.rest.push_back(SDL_GetJoystickAxis(joystick, i));
  }
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, static_cast<int>(sizeof(guid)));
  std::string rest_text;
  for (size_t i = 0; i < part.rest.size(); ++i) {
    rest_text += fmt::format("{}{}", i ? " " : "", part.rest[i]);
  }
  REXLOG_INFO(
      "SDL wheel part: \"{}\"{}{}, VendorID(0x{:04X}), ProductID(0x{:04X}), GUID({}), {} axes [{}], "
      "{} buttons, {} hats",
      name ? name : "", part.base ? " (wheel base)" : "",
      part.accessory ? " (button box: joins a wheel, not a controller of its own)" : "",
      SDL_GetJoystickVendor(joystick),
      SDL_GetJoystickProduct(joystick), guid, axes, rest_text, SDL_GetNumJoystickButtons(joystick),
      SDL_GetNumJoystickHats(joystick));

  std::lock_guard<std::mutex> guard(controllers_mutex_);
  auto existing = std::find_if(controllers_.begin(), controllers_.end(),
                               [](const ControllerState& c) { return !c.sdl; });
  const bool joined = existing != controllers_.end();
  if (!joined) {
    ControllerState fresh = {};
    fresh.sdl = nullptr;
    fresh.guid = guid;
    fresh.subtype =
        REXCVAR_GET(hid_wheel_as_360_wheel) ? XINPUT_DEVSUBTYPE_WHEEL : XINPUT_DEVSUBTYPE_GAMEPAD;
    fresh.id = static_cast<DeviceId>(next_device_id_++);
    controllers_.push_back(std::move(fresh));
    existing = controllers_.end() - 1;
  }
  ControllerState& controller = *existing;
  controller.wheel.parts.push_back(std::move(part));
  RebuildWheel(controller.wheel);
  // Named after the wheel base once it is there.
  const char* base_name = SDL_GetJoystickName(controller.wheel.sdl);
  controller.name = base_name ? base_name : "";
  controller.instance_id = controller.wheel.parts.front().instance_id;
  controller.caps = ReadWheelCapabilities(controller.wheel);
  controller.state.gamepad = ReadWheel(controller.wheel);
  controller.state_changed = true;
  if (!joined) {
    SDL_SetJoystickPlayerIndex(joystick, static_cast<int>(controllers_.size() - 1));
  }

  const Axes a = ChooseAxes(controller.wheel.rest);
  REXLOG_INFO(
      "SDL wheel: \"{}\", {} part(s), {} axes in all; steering axis {}, accelerator axis {}, brake "
      "axis {}; device {}{}",
      controller.name, controller.wheel.parts.size(), controller.wheel.rest.size(), a.steer,
      a.throttle, a.brake, static_cast<uint64_t>(controller.id),
      joined ? " (the part joined the connected wheel)" : "");
}

bool SDLInputDriver::RemoveWheelPart(SDL_JoystickID instance_id) {
  SDL_Joystick* closing = nullptr;
  bool last = false;
  {
    std::lock_guard<std::mutex> guard(controllers_mutex_);
    for (auto it = controllers_.begin(); it != controllers_.end(); ++it) {
      if (it->sdl) {
        continue;
      }
      auto& parts = it->wheel.parts;
      auto part = std::find_if(parts.begin(), parts.end(), [instance_id](const WheelPart& p) {
        return p.instance_id == instance_id;
      });
      if (part == parts.end()) {
        continue;
      }
      closing = part->sdl;
      parts.erase(part);
      REXLOG_INFO("SDL wheel: a part disconnected, {} left", parts.size());
      if (parts.empty()) {
        controllers_.erase(it);
        last = true;
      } else {
        RebuildWheel(it->wheel);
        it->instance_id = parts.front().instance_id;
      }
      break;
    }
  }
  if (!closing) {
    return false;
  }
  SDL_CloseJoystick(closing);
  if (last) {
    REXLOG_INFO("SDL OnControllerDeviceRemoved: {} device(s) remain.", controllers_.size());
  }
  return true;
}

X_INPUT_GAMEPAD SDLInputDriver::ReadWheel(const Wheel& wheel) {
  const Axes a = ChooseAxes(wheel.rest);
  X_INPUT_GAMEPAD pad = {};

  if (a.steer != kNoAxis) {
    double steer = WheelAxis(wheel, a.steer);
    const double deadzone = std::clamp(REXCVAR_GET(hid_wheel_deadzone), 0, 32000);
    if (std::abs(steer) <= deadzone) {
      steer = 0.0;
    } else {
      // Rescale past the dead zone so full lock still reaches the end.
      steer = (steer - std::copysign(deadzone, steer)) * 32767.0 / (32767.0 - deadzone);
    }
    steer *= REXCVAR_GET(hid_wheel_steer_scale);
    if (REXCVAR_GET(hid_wheel_invert_steering)) {
      steer = -steer;
    }
    pad.thumb_lx = int16_t(std::clamp(std::lround(steer), -32767l, 32767l));
  }

  float throttle = 0.0f, brake = 0.0f;
  if (a.throttle != kNoAxis && a.throttle == a.brake) {
    // Combined pedals: accelerator one way, brake the other.
    const float v = WheelAxis(wheel, a.throttle) / 32767.0f;
    throttle = std::max(0.0f, -v);
    brake = std::max(0.0f, v);
  } else {
    if (a.throttle != kNoAxis) {
      throttle = PedalTravel(WheelAxis(wheel, a.throttle), wheel.rest[a.throttle]);
    }
    if (a.brake != kNoAxis) {
      brake = PedalTravel(WheelAxis(wheel, a.brake), wheel.rest[a.brake]);
    }
  }
  pad.right_trigger = ToTrigger(throttle);
  pad.left_trigger = ToTrigger(brake);

  uint16_t buttons = 0;
  const auto& map = ButtonMap();
  int index = 0;  // across the parts
  bool hat_done = false;
  for (const WheelPart& part : wheel.parts) {
    const int count = SDL_GetNumJoystickButtons(part.sdl);
    for (int i = 0; i < count; ++i, ++index) {
      if (index >= int(map.size()) || !map[index] || !SDL_GetJoystickButton(part.sdl, i)) {
        continue;
      }
      if (map[index] == kLeftTriggerButton) {
        pad.left_trigger = 255;
      } else if (map[index] == kRightTriggerButton) {
        pad.right_trigger = 255;
      } else {
        buttons |= uint16_t(map[index]);
      }
    }
    // The first part with a hat drives the D-pad.
    if (!hat_done && SDL_GetNumJoystickHats(part.sdl) > 0) {
      hat_done = true;
      const Uint8 hat = SDL_GetJoystickHat(part.sdl, 0);
      buttons |= (hat & SDL_HAT_UP) ? X_INPUT_GAMEPAD_DPAD_UP : 0;
      buttons |= (hat & SDL_HAT_DOWN) ? X_INPUT_GAMEPAD_DPAD_DOWN : 0;
      buttons |= (hat & SDL_HAT_LEFT) ? X_INPUT_GAMEPAD_DPAD_LEFT : 0;
      buttons |= (hat & SDL_HAT_RIGHT) ? X_INPUT_GAMEPAD_DPAD_RIGHT : 0;
    }
  }
  pad.buttons = buttons;

  if (REXCVAR_GET(hid_wheel_log_axes)) {
    // Poll's thread alone reads wheels, and there is one wheel.
    static std::vector<int16_t> last_axes;
    static std::vector<bool> last_buttons;
    const int axes = int(wheel.rest.size());
    last_axes.resize(std::max<size_t>(last_axes.size(), axes), 0);
    for (int i = 0; i < axes; ++i) {
      const int16_t v = WheelAxis(wheel, i);
      if (std::abs(int(v) - int(last_axes[i])) > 4000) {
        REXLOG_INFO("SDL wheel: axis {} = {}", i, v);
        last_axes[i] = v;
      }
    }
    int button = 0;
    for (const WheelPart& part : wheel.parts) {
      const int count = SDL_GetNumJoystickButtons(part.sdl);
      for (int i = 0; i < count; ++i, ++button) {
        if (button >= int(last_buttons.size())) {
          last_buttons.resize(button + 1, false);
        }
        const bool down = SDL_GetJoystickButton(part.sdl, i);
        if (down != last_buttons[button]) {
          REXLOG_INFO("SDL wheel: button {} {}", button, down ? "down" : "up");
          last_buttons[button] = down;
        }
      }
    }
  }
  return pad;
}

X_INPUT_CAPABILITIES SDLInputDriver::ReadWheelCapabilities(const Wheel& wheel) {
  X_INPUT_CAPABILITIES c = {};
  c.type = XINPUT_DEVTYPE_GAMEPAD;
  c.sub_type =
      REXCVAR_GET(hid_wheel_as_360_wheel) ? XINPUT_DEVSUBTYPE_WHEEL : XINPUT_DEVSUBTYPE_GAMEPAD;
  uint16_t cap_flags = 0;
  if (SDL_GetJoystickConnectionState(wheel.sdl) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }
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

// hid_pad_buttons applied to a controller's state: each physical button (the triggers pressed
// past half) gives the button its entry names. A trigger named keeps the analog value of a
// physical trigger, and is full for a button.
void RemapPadButtons(X_INPUT_GAMEPAD& pad) {
  static constexpr uint32_t kPhysical[] = {
      X_INPUT_GAMEPAD_A,         X_INPUT_GAMEPAD_B,          X_INPUT_GAMEPAD_X,
      X_INPUT_GAMEPAD_Y,         X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER,
      kLeftTriggerButton,        kRightTriggerButton,        X_INPUT_GAMEPAD_BACK,
      X_INPUT_GAMEPAD_START,     X_INPUT_GAMEPAD_LEFT_THUMB, X_INPUT_GAMEPAD_RIGHT_THUMB,
      X_INPUT_GAMEPAD_DPAD_UP,   X_INPUT_GAMEPAD_DPAD_DOWN,  X_INPUT_GAMEPAD_DPAD_LEFT,
      X_INPUT_GAMEPAD_DPAD_RIGHT};
  thread_local std::string parsed;
  thread_local std::vector<uint32_t> map;
  const std::string spec = REXCVAR_GET(hid_pad_buttons);
  if (spec.empty()) {
    return;
  }
  if (spec != parsed) {
    parsed = spec;
    map.clear();
    size_t pos = 0;
    while (pos <= spec.size() && map.size() < std::size(kPhysical)) {
      size_t end = spec.find(',', pos);
      if (end == std::string::npos) end = spec.size();
      std::string_view item(spec.data() + pos, end - pos);
      while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
      while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
      map.push_back(ButtonByName(item));
      pos = end + 1;
    }
    // Buttons the list does not reach keep their own.
    while (map.size() < std::size(kPhysical)) map.push_back(kPhysical[map.size()]);
  }
  constexpr uint8_t kTriggerPressed = 128;
  const X_INPUT_GAMEPAD in = pad;
  uint16_t buttons = 0;
  uint8_t left = 0, right = 0;
  for (size_t i = 0; i < std::size(kPhysical); ++i) {
    const uint32_t phys = kPhysical[i];
    const uint8_t analog = phys == kLeftTriggerButton    ? in.left_trigger
                           : phys == kRightTriggerButton ? in.right_trigger
                           : (in.buttons & phys)          ? 255
                                                          : 0;
    const bool pressed = phys == kLeftTriggerButton || phys == kRightTriggerButton
                             ? analog >= kTriggerPressed
                             : analog != 0;
    const uint32_t to = map[i];
    if (to == kLeftTriggerButton) {
      left = std::max(left, analog);
    } else if (to == kRightTriggerButton) {
      right = std::max(right, analog);
    } else if (pressed) {
      buttons |= uint16_t(to);
    }
  }
  // The Guide button is not in the list and stays.
  pad.buttons = uint16_t(buttons | (in.buttons & X_INPUT_GAMEPAD_GUIDE));
  pad.left_trigger = left;
  pad.right_trigger = right;
}

}  // namespace rex::input::sdl
