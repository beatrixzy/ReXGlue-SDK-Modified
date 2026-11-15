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

#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/input/input_driver.h>

#include <SDL3/SDL.h>

#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

// hid_pad_buttons (wheel_input.cpp): controller button remapping.
void RemapPadButtons(X_INPUT_GAMEPAD& pad);

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

 private:
  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  // A racing wheel opened as plain joysticks (wheel_input.cpp): its axes and
  // buttons are mapped by the hid_wheel_* cvars instead of a gamepad mapping.
  // Wheel base, pedals and shifter are often separate USB devices; they are
  // one wheel to the guest, their axes and buttons numbered in part order.
  struct WheelPart {
    SDL_Joystick* sdl;
    SDL_JoystickID instance_id;
    bool base;  // the wheel itself, ordered first
    bool accessory;  // a button box: adds buttons to a wheel, never a controller alone
    std::vector<int16_t> rest;  // axis values when opened
  };
  struct Wheel {
    SDL_Joystick* sdl;  // the first part's, for rumble
    std::vector<WheelPart> parts;
    // Each axis's value when its part was opened: pedals rest at one end of
    // their range, which tells the pressed direction.
    mutable std::vector<int16_t> rest;  // pedals calibrate as they first move
  };

  struct ControllerState {
    SDL_Gamepad* sdl;  // null for a wheel
    Wheel wheel;
    SDL_JoystickID instance_id;
    std::string name;
    std::string guid;
    uint8_t subtype;  // XINPUT_DEVSUBTYPE_*
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    DeviceId id;
    // Per pad rather than per guest user, so it survives reassignment.
    KeystrokeState keystroke;
    // The guest's latest motor speeds, sent by the next Poll.
    uint16_t vibration_low;
    uint16_t vibration_high;
    bool vibration_pending;
  };
  struct Rumble {
    SDL_Gamepad* sdl;
    SDL_Joystick* wheel;
    uint16_t low;
    uint16_t high;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  // Every kPollIntervalMs on the poll thread, which alone opens, closes, reads and rumbles the
  // pads once the subsystems are up. Its own thread waiting on a condition variable, after
  // pgr4-native's patch 0020: SDL's timer thread waits through SDL_WaitSemaphoreTimeoutNS, which
  // spins with a zero timeout through each interval's last millisecond (23 % of a core).
  void PollThread();
  void Poll();
  void LogJoystickAdded(SDL_JoystickID instance_id);
  void AddController(SDL_JoystickID instance_id);
  void RemoveController(SDL_JoystickID instance_id);
  // Wheels (wheel_input.cpp).
  static bool IsWheel(SDL_JoystickID instance_id);
  // A button box (by name), or a device listed in --hid_ignore_devices.
  static bool IsButtonBox(SDL_JoystickID instance_id);
  static bool IsIgnored(SDL_JoystickID instance_id);
  // A wheel made only of button boxes so far: not shown to the game.
  static bool OnlyAccessories(const Wheel& wheel);
  void AddWheel(SDL_JoystickID instance_id);
  // Whether instance_id was a wheel part; drops the wheel with its last part.
  bool RemoveWheelPart(SDL_JoystickID instance_id);
  static void RebuildWheel(Wheel& wheel);
  static X_INPUT_GAMEPAD ReadWheel(const Wheel& wheel);
  static X_INPUT_CAPABILITIES ReadWheelCapabilities(const Wheel& wheel);
  static X_INPUT_GAMEPAD ReadGamepad(SDL_Gamepad* gamepad);
  static X_INPUT_CAPABILITIES ReadCapabilities(SDL_Gamepad* gamepad, uint8_t subtype);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  ControllerState* FindController(DeviceId id);
  bool TestSDLVersion() const;

  // A pad's state reaches the guest at most this late.
  static constexpr Uint32 kPollIntervalMs = 4;

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::thread poll_thread_;
  std::mutex poll_mutex_;
  std::condition_variable poll_wake_;  // stop
  bool polling_stopped_ = false;       // poll_mutex_
  // Appended in connection order, never re-sorted, and unbounded: the
  // assignment decides how many the guest sees. Changed by Poll alone, under
  // controllers_mutex_.
  std::vector<ControllerState> controllers_;
  std::mutex controllers_mutex_;
  // The device events HandleEvent queued for Poll.
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  std::vector<SDL_Event> device_events_;  // Poll's
  std::vector<Rumble> rumbles_;           // Poll's
  // Never rewound, so a handle held past a disconnect resolves to nothing.
  uint64_t next_device_id_ = 1;
};

}  // namespace rex::input::sdl
