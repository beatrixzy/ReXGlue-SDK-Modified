/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>
#include <chrono>
#include <rex/input/flags.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

// XUSER_INDEX_ANY: the first keystroke from any slot, tagged with its user so
// "press START" screens can tell which pad joined.
X_RESULT GetKeystrokeAnyUser(rex::input::InputSystem* is, uint32_t flags,
                             X_INPUT_KEYSTROKE* keystroke) {
  X_RESULT result = X_ERROR_DEVICE_NOT_CONNECTED;
  for (uint32_t i = 0; i < 4; ++i) {
    X_RESULT r = is->GetKeystroke(i, flags, keystroke);
    if (r == X_ERROR_SUCCESS) {
      return r;
    }
    if (r != X_ERROR_DEVICE_NOT_CONNECTED) {
      result = r;
    }
  }
  return result;
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

u32 XamEnableInactivityProcessing_entry(u32 unk, u32 enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_TRACE("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index,
                (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}
// Menus repeat a held direction a fixed number of frames after the press,
// tuned for the 360's 30 fps: above that a short press scrolled past several
// items. A held direction now passes for its first 30 ms (the press), then
// reads as released until menu_repeat_delay, then passes again and the game's
// own repeat runs. Let go and press again: no wait. The D-pad always; the left
// stick only while the title says it is in its menus (it steers otherwise).
REXCVAR_DEFINE_DOUBLE(menu_repeat_delay, 0.5, "Input",
                      "Seconds a held direction waits before menus start scrolling (0 = off)");

namespace {
bool (*g_in_menus)() = nullptr;

struct HeldDirection {
  std::chrono::steady_clock::time_point since{};
  bool held = false;
};
HeldDirection g_held[4][4];  // [user][up, down, left, right]

// True while the direction should read as released.
bool HoldBack(HeldDirection& d, bool down, double delay) {
  const auto now = std::chrono::steady_clock::now();
  if (!down) {
    d.held = false;
    return false;
  }
  if (!d.held) {
    d.held = true;
    d.since = now;
  }
  const double held = std::chrono::duration<double>(now - d.since).count();
  return held > 0.03 && held < delay;
}

void ShapeMenuRepeat(uint32_t user, rex::input::X_INPUT_GAMEPAD& pad) {
  const double delay = REXCVAR_GET(menu_repeat_delay);
  if (delay <= 0 || user >= 4) return;
  // The stick only in menus (it steers in races); the D-pad always (races
  // only tap it, and the pause menu sits over a race).
  const bool menus = g_in_menus && g_in_menus();
  using namespace rex::input;
  constexpr int16_t kStick = 16000;  // about half way: a menu move
  const bool stick = menus;
  const int16_t lx = pad.thumb_lx, ly = pad.thumb_ly;
  const bool dir[4] = {
      (pad.buttons & X_INPUT_GAMEPAD_DPAD_UP) || (stick && ly > kStick),
      (pad.buttons & X_INPUT_GAMEPAD_DPAD_DOWN) || (stick && ly < -kStick),
      (pad.buttons & X_INPUT_GAMEPAD_DPAD_LEFT) || (stick && lx < -kStick),
      (pad.buttons & X_INPUT_GAMEPAD_DPAD_RIGHT) || (stick && lx > kStick),
  };
  constexpr uint16_t kBits[4] = {X_INPUT_GAMEPAD_DPAD_UP, X_INPUT_GAMEPAD_DPAD_DOWN, X_INPUT_GAMEPAD_DPAD_LEFT,
                                 X_INPUT_GAMEPAD_DPAD_RIGHT};
  for (int i = 0; i < 4; ++i) {
    if (!HoldBack(g_held[user][i], dir[i], delay)) continue;
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~kBits[i]);
    if (stick) {
      if (i < 2) pad.thumb_ly = 0;
      else pad.thumb_lx = 0;
    }
  }
}
}  // namespace

void xeXamSetMenuContext(bool (*in_menus)()) {
  g_in_menus = in_menus;
}


// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_TRACE("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index,
                  (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  // Pads arriving or leaving sign players 2-4 in or out.
  xeXamPollSigninChanges();

  auto* is = input_system();
  X_INPUT_STATE state = {};
  X_RESULT result = is->GetState(actual_user_index, &state);
  if (result != X_ERROR_SUCCESS) {
    // Still lets Enter open the Guide with no pad on this slot.
    xeXamFilterGameInput(actual_user_index, 0);
    return result;
  }
  // The system Guide sees the pad first; while it (or any system dialog) is
  // up the game gets an idle pad, like on the console.
  if (xeXamFilterGameInput(actual_user_index, state.gamepad.buttons)) {
    state.gamepad = {};
  } else if (!REXCVAR_GET(guide_button)) {
    state.gamepad.buttons =
        static_cast<uint16_t>(state.gamepad.buttons & ~rex::input::X_INPUT_GAMEPAD_GUIDE);
  }
  ShapeMenuRepeat(actual_user_index, state.gamepad);
  if (input_state) {
    *input_state = state;
  }
  return result;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  auto* is = input_system();
  X_RESULT result;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    result = GetKeystrokeAnyUser(is, flags, keystroke);
  } else {
    result = is->GetKeystroke(user_index, flags, keystroke);
  }
  // Presses meant for a system dialog are drained, not handed to the game.
  if (result == X_ERROR_SUCCESS && xeXamIsUIActive()) {
    return X_ERROR_EMPTY;
  }
  return result;
}

// Same as non-ex, just takes a pointer to user index.
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  auto* is = input_system();
  X_RESULT result;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    result = GetKeystrokeAnyUser(is, flags, keystroke);
  } else {
    result = is->GetKeystroke(user_index, flags, keystroke);
  }
  if (result == X_ERROR_SUCCESS && xeXamIsUIActive()) {
    return X_ERROR_EMPTY;
  }
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if ((user_index & 0xFF) == 0xFF || xeXamSignedInProfile(user_index)) {
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRawEx);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRawEx);
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
