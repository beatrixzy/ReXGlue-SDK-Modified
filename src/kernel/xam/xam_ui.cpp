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

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <random>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

#include <imgui.h>

REXCVAR_DEFINE_BOOL(headless, false, "Kernel",
                    "Don't display any UI, using defaults for prompts as needed");
REXCVAR_DEFINE_BOOL(guide_combo, true, "Kernel",
                    "Back + Start pressed together also open the Xbox Guide (for pads whose "
                    "Guide button the OS or Steam keeps)");
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/private.h>
#include <rex/system/xam/eos_service.h>
#include <rex/system/xam/stats_service.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).

extern std::atomic<int> xam_dialogs_shown_;

class XamDialog : public rex::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamDialog(rex::ui::ImGuiDrawer* imgui_drawer) : rex::ui::ImGuiDialog(imgui_drawer) {}

  void OnClose() override {
    WantText(false);
    if (close_callback_) {
      close_callback_();
    }
  }

  // Typed characters only reach ImGui while the window's text input is on,
  // which ImGui asks for only when one of its own text boxes has focus. The
  // Guide's keyboard is drawn by hand, so it asks while it is up.
  void WantText(bool on) {
    if (wants_text_ == on) return;
    wants_text_ = on;
    imgui_drawer()->RequestTextInput(on);
  }

 private:
  std::function<void()> close_callback_ = nullptr;
  bool wants_text_ = false;
};

template <typename T>
X_RESULT xeXamDispatchDialog(T* dialog, std::function<X_RESULT(T*)> close_callback,
                             uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback]() -> X_RESULT {
    X_RESULT result;
    dialog->set_close_callback(
        [&dialog, &result, &close_callback]() { result = close_callback(dialog); });
    rex::thread::Fence fence;
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogEx(T* dialog,
                               std::function<X_RESULT(T*, uint32_t&, uint32_t&)> close_callback,
                               uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    X_RESULT result;
    dialog->set_close_callback([&dialog, &result, &extended_error, &length, &close_callback]() {
      result = close_callback(dialog, extended_error, length);
    });
    rex::thread::Fence fence;
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped) {
  auto pre = []() {
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = true");
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = false");
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadlessEx(std::function<X_RESULT(uint32_t&, uint32_t&)> run_callback,
                                 uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run_callback(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

u32 XamIsUIActive_entry() {
  return xeXamIsUIActive();
}

// ---------------------------------------------------------------------------
// Console-style system UI (the Xbox Guide look): one painter for the Guide,
// message boxes and pickers, driven by any pad, the keyboard or the mouse.
// Laid out on a 1920x1080 canvas and scaled to the window.

namespace sysui {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Row {
  std::string label;
  std::string value;  // right-aligned (counts, status)
};

struct Screen {
  std::string header = "Xbox Guide";
  std::string gamertag;                  // avatar + name in the header; empty = none
  std::vector<std::string> left_blades;  // outermost first
  std::vector<std::string> right_blades;  // innermost first
  std::string blade_label;               // vertical label of the open blade
  std::string message;                   // wrapped text above the rows
  std::vector<Row> rows;
  int selected = -1;
  std::vector<std::pair<std::string, std::string>> hints;  // button, label
  // With blades, the open blade stays where it sits when anchor_left of
  // anchor_total tabs are on its left (the home blade), and the tabs pile up
  // to either side, as on the console; without, the panel is centred.
  int anchor_left = 0, anchor_total = 0;
  // Animation: the whole panel scaled about the screen centre, the dimmed
  // backdrop, the frame (tabs, header, hints) and the open blade's contents
  // each with their own opacity.
  float zoom = 1;
  float backdrop_alpha = 1;
  float chrome_alpha = 1;
  float content_alpha = 1;
  float panel_alpha = 1;  // everything but the backdrop
};

// What the mouse did this frame.
struct Hit {
  int hover_row = -1;
  int click_row = -1;
  int click_blade = 0;  // -n / +n: a blade n steps left / right was clicked
};

constexpr float kRowH = 63, kSideW = 66, kLabelW = 95, kListW = 728;
constexpr float kPanelTop = 303, kPanelH = 473;
constexpr float kTextSize = 36, kSmallSize = 30;

class Canvas {
 public:
  Canvas(ImDrawList* dl, ImFont* font, const ImGuiIO& io, float alpha, float zoom = 1)
      : dl_(dl), font_(font), alpha_(std::clamp(alpha, 0.0f, 1.0f)) {
    scale_ = std::min(io.DisplaySize.x / 1920.0f, io.DisplaySize.y / 1080.0f) * zoom;
    ox_ = io.DisplaySize.x / 2 - 960 * scale_;
    oy_ = io.DisplaySize.y / 2 - 540 * scale_;
  }
  ImVec2 P(float x, float y) const { return ImVec2(ox_ + x * scale_, oy_ + y * scale_); }
  ImU32 C(int r, int g, int b, int a = 255) const {
    return IM_COL32(r, g, b, int(a * alpha_));
  }
  void Rect(float x0, float y0, float x1, float y1, ImU32 c, float round = 0) {
    dl_->AddRectFilled(P(x0, y0), P(x1, y1), c, round * scale_);
  }
  // size and positions in canvas units.
  void Text(float x, float y, float size, ImU32 c, const std::string& t, float wrap = 0) {
    dl_->AddText(font_, size * scale_, P(x, y), c, t.c_str(), nullptr, wrap * scale_);
  }
  float Width(const std::string& t, float size) const {
    return font_->CalcTextSizeA(size * scale_, FLT_MAX, 0, t.c_str()).x / scale_;
  }
  float WrappedHeight(const std::string& t, float size, float wrap) const {
    return font_->CalcTextSizeA(size * scale_, FLT_MAX, wrap * scale_, t.c_str()).y / scale_;
  }
  // Reads top to bottom, letters' tops facing right (the Guide's blade tabs).
  void VerticalText(float x, float y, float size, ImU32 c, const std::string& t) {
    ImVec2 pivot = P(x + size, y);
    int start = dl_->VtxBuffer.Size;
    dl_->AddText(font_, size * scale_, pivot, c, t.c_str());
    for (int i = start; i < dl_->VtxBuffer.Size; ++i) {
      ImVec2& p = dl_->VtxBuffer[i].pos;
      float dx = p.x - pivot.x, dy = p.y - pivot.y;
      p = ImVec2(pivot.x - dy, pivot.y + dx);
    }
  }
  void Circle(float x, float y, float r, ImU32 c) { dl_->AddCircleFilled(P(x, y), r * scale_, c); }
  // A speaker with two sound waves, centred on (x, y), `s` tall.
  void Speaker(float x, float y, float s, ImU32 c) {
    const float h = s / 2;
    Rect(x - h * 0.9f, y - h * 0.35f, x - h * 0.45f, y + h * 0.35f, c);
    dl_->AddTriangleFilled(P(x - h * 0.5f, y - h * 0.35f), P(x + h * 0.05f, y - h * 0.85f),
                           P(x + h * 0.05f, y + h * 0.85f), c);
    dl_->AddTriangleFilled(P(x - h * 0.5f, y - h * 0.35f), P(x + h * 0.05f, y + h * 0.85f),
                           P(x - h * 0.5f, y + h * 0.35f), c);
    for (int i = 1; i <= 2; ++i) {
      dl_->PathArcTo(P(x + h * 0.05f, y), h * (0.35f + 0.3f * i) * scale_, -0.8f, 0.8f, 12);
      dl_->PathStroke(c, 0, h * 0.14f * scale_);
    }
  }
  void Clip(float x0, float y0, float x1, float y1) { dl_->PushClipRect(P(x0, y0), P(x1, y1), true); }
  void Unclip() { dl_->PopClipRect(); }
  bool Inside(const ImVec2& m, float x0, float y0, float x1, float y1) const {
    ImVec2 a = P(x0, y0), b = P(x1, y1);
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
  }
  ImDrawList* dl() const { return dl_; }

 private:
  ImDrawList* dl_;
  ImFont* font_;
  float alpha_, scale_, ox_, oy_;
};

void ButtonGlyph(Canvas& cv, float x, float cy, const std::string& button) {
  if (button.size() == 1) {
    ImU32 fill = cv.C(90, 90, 90);
    switch (button[0]) {
      case 'A': fill = cv.C(96, 168, 40); break;
      case 'B': fill = cv.C(200, 40, 40); break;
      case 'X': fill = cv.C(40, 90, 200); break;
      case 'Y': fill = cv.C(230, 170, 20); break;
    }
    cv.Circle(x + 16, cy, 16, fill);
    float w = cv.Width(button, 24);
    cv.Text(x + 16 - w / 2, cy - 14, 24, cv.C(255, 255, 255), button);
  } else {
    float w = cv.Width(button, 22) + 16;
    cv.Rect(x, cy - 13, x + w, cy + 13, cv.C(70, 70, 70), 8);
    cv.Text(x + 8, cy - 13, 22, cv.C(235, 235, 235), button);
  }
}

float ButtonGlyphWidth(Canvas& cv, const std::string& button) {
  return button.size() == 1 ? 32 : cv.Width(button, 22) + 16;
}

// Draws the screen; *scroll keeps the selection in view between frames.
Hit DrawScreen(ImDrawList* dl, ImFont* font, const ImGuiIO& io, const Screen& s, int* scroll,
               float alpha) {
  const float backdrop = alpha * std::clamp(s.backdrop_alpha, 0.0f, 1.0f);
  alpha *= std::clamp(s.panel_alpha, 0.0f, 1.0f);
  Canvas cv(dl, font, io, alpha, s.zoom);                        // panel
  Canvas chrome(dl, font, io, alpha * s.chrome_alpha, s.zoom);    // tabs, header, hints
  Canvas content(dl, font, io, alpha * s.content_alpha, s.zoom);  // the open blade's contents
  Hit hit;
  const ImVec2 mouse = io.MousePos;
  const bool clicked = io.MouseClicked[0];

  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, int(150 * backdrop)));

  const bool anchored = s.anchor_total > 0;
  const float label_x =
      anchored ? (1920 - kLabelW - kListW - kSideW * s.anchor_total) / 2 + kSideW * s.anchor_left
               : (1920 - kLabelW - kListW - kSideW * float(s.left_blades.size() +
                                                           s.right_blades.size())) /
                         2 +
                     kSideW * float(s.left_blades.size());
  const float list_x = label_x + kLabelW;
  const float list_r = list_x + kListW;
  const float x0 = label_x - kSideW * float(s.left_blades.size());
  const float top = kPanelTop, bottom = kPanelTop + kPanelH;

  // Header: title, avatar + gamertag, clock.
  chrome.Text(label_x, top - 66, kTextSize, chrome.C(255, 255, 255), s.header);
  if (!s.gamertag.empty()) {
    float ax = list_x + kListW / 2 - 110;
    chrome.Rect(ax, top - 84, ax + 64, top - 14, chrome.C(120, 128, 136), 6);
    std::string initial = s.gamertag.substr(0, 1);
    chrome.Text(ax + 32 - chrome.Width(initial, 40) / 2, top - 74, 40, chrome.C(255, 255, 255),
                initial);
    chrome.Text(ax + 76, top - 62, kSmallSize, chrome.C(220, 220, 220), s.gamertag);
  }
  {
    std::time_t t = std::time(nullptr);
    std::tm tm_local{};
#if REX_PLATFORM_WIN32
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    char clock[16];
    int hour = tm_local.tm_hour % 12 == 0 ? 12 : tm_local.tm_hour % 12;
    std::snprintf(clock, sizeof(clock), "%d:%02d %s", hour, tm_local.tm_min,
                  tm_local.tm_hour < 12 ? "AM" : "PM");
    chrome.Text(list_r - chrome.Width(clock, kTextSize), top - 62, kTextSize,
                chrome.C(255, 255, 255), clock);
  }

  // Side blades: closed ones on either side, as tabs.
  auto side_blade = [&](float bx, const std::string& name, int delta) {
    bool shade = (delta & 1) != 0;
    chrome.Rect(bx, top, bx + kSideW, bottom, shade ? chrome.C(88, 101, 112) : chrome.C(78, 90, 101));
    chrome.Rect(bx, top, bx + 1, bottom, chrome.C(60, 70, 80));
    chrome.VerticalText(bx + (kSideW - 34) / 2 - 2, top + 26, 34, chrome.C(214, 220, 226), name);
    if (clicked && cv.Inside(mouse, bx, top, bx + kSideW, bottom)) hit.click_blade = delta;
  };
  for (size_t i = 0; i < s.left_blades.size(); ++i) {
    side_blade(x0 + kSideW * float(i), s.left_blades[i], -int(s.left_blades.size() - i));
  }
  for (size_t i = 0; i < s.right_blades.size(); ++i) {
    side_blade(list_r + kSideW * float(i), s.right_blades[i], int(i + 1));
  }

  // The open blade: its label, then the list.
  cv.Rect(label_x, top, list_x, bottom, cv.C(196, 201, 206));
  content.VerticalText(label_x + (kLabelW - 36) / 2 - 4, top + 24, 36, content.C(104, 110, 116),
                       s.blade_label);
  cv.Rect(list_x, top, list_r, bottom, cv.C(222, 225, 228));

  cv.Clip(list_x, top, list_r, bottom);
  const float cx = list_x;
  const float cr = list_r;
  float y = top;
  if (!s.message.empty()) {
    float h = cv.WrappedHeight(s.message, kSmallSize, kListW - 44);
    content.Text(cx + 22, y + 14, kSmallSize, content.C(70, 70, 70), s.message, kListW - 44);
    y += h + 28;
    if (!s.rows.empty()) {
      content.Rect(cx, y - 1, cr, y, content.C(190, 194, 198));
    }
  }
  const int visible = std::max(1, int((bottom - y) / kRowH));
  const int count = int(s.rows.size());
  if (s.selected >= 0) {
    if (s.selected < *scroll) *scroll = s.selected;
    if (s.selected >= *scroll + visible) *scroll = s.selected - visible + 1;
  }
  *scroll = std::clamp(*scroll, 0, std::max(0, count - visible));
  for (int i = *scroll; i < count && i < *scroll + visible; ++i) {
    const float ry = y + (i - *scroll) * kRowH;
    const bool sel = i == s.selected;
    if (sel) {
      content.Rect(cx, ry, cr, ry + kRowH, content.C(30, 132, 20));
    } else {
      content.Rect(cx, ry + kRowH - 2, cr, ry + kRowH, content.C(200, 204, 208));
    }
    const ImU32 text = sel ? content.C(255, 255, 255) : content.C(64, 64, 64);
    content.Text(cx + 22, ry + (kRowH - kTextSize) / 2 - 2, kTextSize, text, s.rows[i].label);
    if (!s.rows[i].value.empty()) {
      float w = content.Width(s.rows[i].value, kSmallSize);
      content.Text(cr - 22 - w, ry + (kRowH - kSmallSize) / 2 - 2, kSmallSize,
                   sel ? content.C(235, 245, 235) : content.C(96, 96, 96), s.rows[i].value);
    }
    if (cv.Inside(mouse, list_x, ry, list_r, ry + kRowH)) {
      if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) hit.hover_row = i;
      if (clicked) hit.click_row = i;
    }
  }
  // More rows than fit: a small scroll marker.
  if (count > visible) {
    float track = bottom - y;
    float h = track * visible / count;
    float off = track * *scroll / count;
    content.Rect(cr - 6, y + off, cr - 2, y + off + h, content.C(150, 155, 160), 2);
  }
  cv.Unclip();

  // Button hints.
  float hx = label_x;
  const float hy = bottom + 34;
  for (const auto& [button, label] : s.hints) {
    ButtonGlyph(chrome, hx, hy, button);
    hx += ButtonGlyphWidth(chrome, button) + 10;
    chrome.Text(hx, hy - kSmallSize / 2 - 2, kSmallSize, chrome.C(255, 255, 255), label);
    hx += chrome.Width(label, kSmallSize) + 30;
  }
  return hit;
}

// Edge-triggered navigation from every pad (sticks act as the d-pad, with
// repeat), plus keyboard arrows / Enter / Escape.
class PadNav {
 public:
  struct Input {
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, x = false, y = false;
    bool lb = false, rb = false, guide = false, start = false;
    bool lt = false, rt = false;  // triggers pulled (the keyboard's Symbols / Alphabet)
    bool typing = false;  // a physical keyboard key is down
  };

  // Bits above the pad's 16 button bits, standing for the triggers (0x0400 is Guide).
  static constexpr uint32_t kLeftTriggerBit = 0x10000;
  static constexpr uint32_t kRightTriggerBit = 0x20000;

  Input Poll() {
    using namespace rex::input;
    uint32_t cur = 0;
    if (auto* is = static_cast<rex::input::InputSystem*>(
            REX_KERNEL_STATE()->emulator()->input_system())) {
      for (uint32_t user = 0; user < 4; ++user) {
        X_INPUT_STATE state = {};
        if (is->GetState(user, &state) != X_ERROR_SUCCESS) continue;
        const auto& g = state.gamepad;
        cur |= g.buttons;
        constexpr int kDead = 20000;
        if (g.thumb_ly > kDead) cur |= X_INPUT_GAMEPAD_DPAD_UP;
        if (g.thumb_ly < -kDead) cur |= X_INPUT_GAMEPAD_DPAD_DOWN;
        if (g.thumb_lx < -kDead) cur |= X_INPUT_GAMEPAD_DPAD_LEFT;
        if (g.thumb_lx > kDead) cur |= X_INPUT_GAMEPAD_DPAD_RIGHT;
        if (g.left_trigger > 128) cur |= kLeftTriggerBit;
        if (g.right_trigger > 128) cur |= kRightTriggerBit;
      }
    }
    Input in;
    // Whatever is held when the screen opens (the button that opened it)
    // only counts once released and pressed again.
    if (!primed_) {
      primed_ = true;
      prev_ = cur;
      return in;
    }
    uint32_t pressed = cur & ~prev_;
    prev_ = cur;

    constexpr uint16_t kDirs = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN |
                               X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
    const double now = ImGui::GetTime();
    if (pressed & kDirs) {
      repeat_dir_ = pressed & kDirs;
      repeat_at_ = now + 0.4;
    } else if (repeat_dir_ && (cur & repeat_dir_) == repeat_dir_) {
      if (now >= repeat_at_) {
        pressed |= repeat_dir_;
        repeat_at_ = now + 0.1;
      }
    } else {
      repeat_dir_ = 0;
    }

    in.up = (pressed & X_INPUT_GAMEPAD_DPAD_UP) || ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    in.down = (pressed & X_INPUT_GAMEPAD_DPAD_DOWN) || ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    in.left = (pressed & X_INPUT_GAMEPAD_DPAD_LEFT) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    in.right = (pressed & X_INPUT_GAMEPAD_DPAD_RIGHT) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    in.a = pressed & X_INPUT_GAMEPAD_A;
    in.b = (pressed & X_INPUT_GAMEPAD_B) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    in.x = pressed & X_INPUT_GAMEPAD_X;
    in.y = pressed & X_INPUT_GAMEPAD_Y;
    in.lb = (pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER) || ImGui::IsKeyPressed(ImGuiKey_PageUp);
    in.rb = (pressed & X_INPUT_GAMEPAD_RIGHT_SHOULDER) || ImGui::IsKeyPressed(ImGuiKey_PageDown);
    in.guide = (pressed & X_INPUT_GAMEPAD_GUIDE) ||
               (!ImGui::GetIO().KeyAlt && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)));
    in.start = pressed & X_INPUT_GAMEPAD_START;
    in.lt = pressed & kLeftTriggerBit;
    in.rt = pressed & kRightTriggerBit;
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key) {
      if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) {
        in.typing = true;
        break;
      }
    }
    return in;
  }

 private:
  bool primed_ = false;
  uint32_t prev_ = 0;
  uint32_t repeat_dir_ = 0;
  double repeat_at_ = 0;
};

// Moves a selection over count rows by the d-pad / mouse; true when the row
// at *selected was chosen (A, Enter, click).
bool Navigate(const PadNav::Input& in, const Hit& hit, int count, int* selected) {
  if (count <= 0) {
    *selected = -1;
    return false;
  }
  *selected = std::clamp(*selected, 0, count - 1);
  if (in.up && *selected > 0) --*selected;
  if (in.down && *selected < count - 1) ++*selected;
  if (hit.hover_row >= 0) *selected = hit.hover_row;
  if (hit.click_row >= 0) {
    *selected = hit.click_row;
    return true;
  }
  return in.a;
}

// Opening, as on the console: the game dims, a blank panel grows out of the
// screen centre, then its frame and contents fade in. Returns false while the
// panel is still growing (no input yet).
bool OpenAnimation(Screen& s, double* opened_at) {
  const double now = ImGui::GetTime();
  if (*opened_at < 0) *opened_at = now;
  const double t = now - *opened_at;
  constexpr double kGrow = 0.22, kFade = 0.1;
  const double grow = std::min(1.0, t / kGrow);
  s.zoom = float(0.45 + 0.55 * (1.0 - std::pow(1.0 - grow, 3.0)));
  s.backdrop_alpha = float(std::min(1.0, t / 0.12));
  s.panel_alpha = float(std::min(1.0, t / 0.05));
  s.chrome_alpha = s.content_alpha = float(std::clamp((t - kGrow) / kFade, 0.0, 1.0));
  return t >= kGrow;
}

// Closing: the opening played backwards. Contents fade, the blank panel
// shrinks into the screen centre and the dimming lifts. True once finished.
bool CloseAnimation(Screen& s, double closing_at) {
  const double t = ImGui::GetTime() - closing_at;
  constexpr double kFade = 0.1, kShrink = 0.22, kTotal = kFade + kShrink;
  s.chrome_alpha = s.content_alpha = float(1.0 - std::clamp(t / kFade, 0.0, 1.0));
  const double shrink = std::clamp((t - kFade) / kShrink, 0.0, 1.0);
  s.zoom = float(0.45 + 0.55 * (1.0 - std::pow(shrink, 3.0)));
  s.panel_alpha = float(std::clamp((kTotal - t) / 0.05, 0.0, 1.0));
  s.backdrop_alpha = float(std::clamp((kTotal - t) / 0.12, 0.0, 1.0));
  return t >= kTotal;
}

// ---------------------------------------------------------------------------
// On-screen keyboard, laid out like the console's: a 10 x 4 character grid,
// big keys either side and Backspace / Space along the bottom, on two pages
// like the 360's. Letters: numbers + three letter rows, Left / Symbols (LT) /
// Caps and Right / Accents (RT) / Done, on a 12 x 5 grid (row 4 the bottom
// row). Symbols (free text only): five rows, Left / Accents (LT) / Caps and
// Right / Alphabet (RT) / Done, on a 12 x 6 grid (row 5 the bottom row).
// Columns 0 and 11 are the side keys.

enum KeyAction : char {
  kKeyNone = 0,
  kKeyLeft = 1,
  kKeyRight,
  kKeyBackspace,
  kKeyDone,
  kKeyCaps,
  kKeySpace,
  kKeySymbols,
};

// The 360's Symbols page (free-text keyboards: chat and messages): the five
// rows of character keys (UTF-8; the console's own button glyph in row 4 is a
// section sign here).
const char* const kSymbolRows[5][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {",", ";", ":", "'", "\"", "!", "?", "\u00A1", "\u00BF", "%"},
    {"[", "]", "{", "}", "`", "$", "\u00A3", "\u00AB", "\u00BB", "#"},
    {"<", ">", "(", ")", "\u20AC", "\u00A5", "\u00A7", "~", "^", "\\"},
    {"|", "=", "*", "/", "+", "-", "@", "_", "&", "."},
};

struct KeyDef {
  std::string label;
  char ch;  // character typed, or a KeyAction
  int c0, c1, r0, r1;
  std::string glyph;  // controller shortcut shown on the key
};

const std::vector<KeyDef>& Keys(bool symbols) {
  static const std::vector<KeyDef> letters = [] {
    std::vector<KeyDef> k;
    const char* rows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_@."};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 10; ++c) {
        const char ch = rows[r][c];
        k.push_back({std::string(1, ch), ch, c + 1, c + 1, r, r, ""});
      }
    }
    k.push_back({"Left", kKeyLeft, 0, 0, 0, 1, "LB"});
    k.push_back({"Symbols", kKeySymbols, 0, 0, 2, 3, "LT"});
    k.push_back({"Caps", kKeyCaps, 0, 0, 4, 4, "L"});
    k.push_back({"Right", kKeyRight, 11, 11, 0, 1, "RB"});
    k.push_back({"Accents", kKeyNone, 11, 11, 2, 3, "RT"});
    k.push_back({"Done", kKeyDone, 11, 11, 4, 4, "START"});
    k.push_back({"Backspace", kKeyBackspace, 1, 5, 4, 4, "X"});
    k.push_back({"Space", kKeySpace, 6, 10, 4, 4, "Y"});
    return k;
  }();
  static const std::vector<KeyDef> syms = [] {
    std::vector<KeyDef> k;
    for (int r = 0; r < 5; ++r) {
      for (int c = 0; c < 10; ++c) {
        // ch is only a placeholder: these keys type their label.
        k.push_back({kSymbolRows[r][c], '#', c + 1, c + 1, r, r, ""});
      }
    }
    k.push_back({"Left", kKeyLeft, 0, 0, 0, 1, "LB"});
    k.push_back({"Accents", kKeyNone, 0, 0, 2, 3, "LT"});
    k.push_back({"Caps", kKeyCaps, 0, 0, 4, 5, "L"});
    k.push_back({"Right", kKeyRight, 11, 11, 0, 1, "RB"});
    k.push_back({"Alphabet", kKeySymbols, 11, 11, 2, 3, "RT"});
    k.push_back({"Done", kKeyDone, 11, 11, 4, 5, "START"});
    k.push_back({"Backspace", kKeyBackspace, 1, 5, 5, 5, "X"});
    k.push_back({"Space", kKeySpace, 6, 10, 5, 5, "Y"});
    return k;
  }();
  return symbols ? syms : letters;
}

int KeyRows(bool symbols) {
  return symbols ? 6 : 5;
}

int KeyAt(bool symbols, int c, int r) {
  const auto& keys = Keys(symbols);
  for (int i = 0; i < int(keys.size()); ++i) {
    if (c >= keys[i].c0 && c <= keys[i].c1 && r >= keys[i].r0 && r <= keys[i].r1) return i;
  }
  return -1;
}

struct KeyboardState {
  std::string text;
  size_t cursor = 0;
  size_t max_length = 16;
  // Gamertags: letters in either case, digits and spaces. Otherwise room
  // codes: capitals, digits, '-' and '_'.
  bool gamertag = false;
  bool free_text = false;  // chat: any printable character (with gamertag's caps / space)
  bool symbols = false;    // free text: the Symbols page is showing
  bool caps = true;
  int selected = KeyAt(false, 1, 1);
  int focus_c = 1, focus_r = 1;  // remembered spot, for moving through wide keys
};

bool KeyboardAllows(const KeyboardState& k, char c) {
  if (std::isalnum(static_cast<unsigned char>(c))) return true;
  if (k.free_text) return c >= 0x20 && c < 0x7F;
  return k.gamertag ? c == ' ' : c == '-' || c == '_';
}

bool KeyEnabled(const KeyboardState& k, const KeyDef& key) {
  switch (key.ch) {
    case kKeyNone: return false;
    case kKeyCaps:
    case kKeySpace: return k.gamertag;
    case kKeySymbols: return k.free_text;
    case kKeyLeft:
    case kKeyRight:
    case kKeyBackspace:
    case kKeyDone: return true;
    default: return k.symbols ? k.free_text : KeyboardAllows(k, key.ch);
  }
}

std::string KeyLabel(const KeyboardState& k, const KeyDef& key) {
  if (k.symbols) return key.label;
  if (k.gamertag && !k.caps && key.label.size() == 1) {
    return std::string(1, char(std::tolower(static_cast<unsigned char>(key.label[0]))));
  }
  return key.label;
}

// Moves the highlight one key over (wrapping), like the d-pad does.
void KeyboardMove(KeyboardState& k, int dc, int dr) {
  const KeyDef& key = Keys(k.symbols)[k.selected];
  const int rows = KeyRows(k.symbols);
  if (dc) k.focus_c = ((dc > 0 ? key.c1 + 1 : key.c0 - 1) + 12) % 12;
  if (dr) k.focus_r = ((dr > 0 ? key.r1 + 1 : key.r0 - 1) + rows) % rows;
  int i = KeyAt(k.symbols, k.focus_c, k.focus_r);
  if (i >= 0) k.selected = i;
}

// UTF-8: the cursor and lengths are in bytes, kept on character starts.
size_t PrevChar(const std::string& s, size_t i) {
  if (i == 0) return 0;
  --i;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
  return i;
}

size_t NextChar(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  ++i;
  while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
  return i;
}

// Types `s` (one character, UTF-8) at the cursor.
void KeyboardTypeText(KeyboardState& k, std::string s) {
  if (s.empty()) return;
  if (s.size() == 1) {
    char c = s[0];
    if (!k.gamertag) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (!KeyboardAllows(k, c)) return;
    // No leading or doubled spaces.
    if (c == ' ' && (k.cursor == 0 || k.text[k.cursor - 1] == ' ')) return;
    s[0] = c;
  } else if (!k.free_text) {
    return;  // codes and gamertags are plain ASCII
  }
  if (k.text.size() + s.size() > k.max_length) return;
  k.text.insert(k.cursor, s);
  k.cursor += s.size();
}

void KeyboardType(KeyboardState& k, char c) {
  KeyboardTypeText(k, std::string(1, c));
}

void KeyboardBackspace(KeyboardState& k) {
  if (k.cursor == 0) return;
  const size_t from = PrevChar(k.text, k.cursor);
  k.text.erase(from, k.cursor - from);
  k.cursor = from;
}

// Flips between the letters and Symbols pages, keeping the highlight on the
// same spot (or on the page switch key, if that was it).
void ToggleSymbols(KeyboardState& k) {
  const bool was_switch = Keys(k.symbols)[k.selected].ch == kKeySymbols;
  k.symbols = !k.symbols;
  const auto& keys = Keys(k.symbols);
  int i = -1;
  if (was_switch) {
    for (int j = 0; j < int(keys.size()); ++j) {
      if (keys[j].ch == kKeySymbols) i = j;
    }
  } else {
    k.focus_r = std::min(k.focus_r, KeyRows(k.symbols) - 1);
    i = KeyAt(k.symbols, k.focus_c, k.focus_r);
  }
  k.selected = i >= 0 ? i : KeyAt(k.symbols, 1, 1);
  k.focus_c = keys[k.selected].c0;
  k.focus_r = keys[k.selected].r0;
}

// Acts on a key; true for Done.
bool KeyboardPress(KeyboardState& k, int index) {
  const KeyDef& key = Keys(k.symbols)[index];
  if (!KeyEnabled(k, key)) return false;
  switch (key.ch) {
    case kKeyLeft: k.cursor = PrevChar(k.text, k.cursor); return false;
    case kKeyRight: k.cursor = NextChar(k.text, k.cursor); return false;
    case kKeyBackspace: KeyboardBackspace(k); return false;
    case kKeyDone: return true;
    case kKeyCaps: k.caps = !k.caps; return false;
    case kKeySymbols: ToggleSymbols(k); return false;
    case kKeySpace: KeyboardType(k, ' '); return false;
    default: KeyboardTypeText(k, KeyLabel(k, key)); return false;
  }
}

struct KeyRect {
  float x0, y0, x1, y1;
};

KeyRect RectOf(const KeyDef& key, bool symbols) {
  constexpr float kGridX = 577.5f, kPitchX = 76.5f, kKeyW = 70.5f;
  auto col_x0 = [&](int c) { return c == 0 ? 358.0f : c == 11 ? 1350.0f : kGridX + (c - 1) * kPitchX; };
  auto col_x1 = [&](int c) {
    return c == 0 ? 564.0f : c == 11 ? 1555.0f : kGridX + (c - 1) * kPitchX + kKeyW;
  };
  KeyRect r{col_x0(key.c0), 0, col_x1(key.c1), 0};
  if (symbols) {
    // Five character rows and the bottom row, evenly pitched.
    constexpr float kGridY = 499.5f, kPitchY = 59.5f, kKeyH = 53.0f;
    r.y0 = kGridY + key.r0 * kPitchY;
    r.y1 = kGridY + key.r1 * kPitchY + kKeyH;
    return r;
  }
  constexpr float kGridY = 499.5f, kPitchY = 60.75f, kKeyH = 54.0f;
  if (key.c0 == 0 || key.c0 == 11) {
    const float tops[5] = {499, 499, 621, 621, 741}, bottoms[5] = {613, 613, 733, 733, 856};
    r.y0 = tops[key.r0];
    r.y1 = bottoms[key.r1];
  } else if (key.r0 == 4) {
    r.y0 = 802;
    r.y1 = 856;
  } else {
    r.y0 = kGridY + key.r0 * kPitchY;
    r.y1 = r.y0 + kKeyH;
  }
  return r;
}

struct KeyboardHit {
  int hover = -1;
  int click = -1;
};

// The keyboard panel. anim carries the title (header), gamertag, the
// description (message) and the animation fields of a Screen.
KeyboardHit DrawKeyboard(ImDrawList* dl, ImFont* font, const ImGuiIO& io, const Screen& anim,
                         const KeyboardState& k) {
  Canvas cv(dl, font, io, anim.panel_alpha, anim.zoom);
  Canvas chrome(dl, font, io, anim.panel_alpha * anim.chrome_alpha, anim.zoom);
  Canvas content(dl, font, io, anim.panel_alpha * anim.content_alpha, anim.zoom);
  KeyboardHit hit;
  const ImVec2 mouse = io.MousePos;

  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                    IM_COL32(0, 0, 0, int(150 * std::clamp(anim.backdrop_alpha, 0.0f, 1.0f))));

  // Header.
  chrome.Text(351, 82, kTextSize, chrome.C(255, 255, 255), anim.header);
  if (!anim.gamertag.empty()) {
    chrome.Rect(924, 54, 996, 126, chrome.C(120, 128, 136), 6);
    std::string initial = anim.gamertag.substr(0, 1);
    chrome.Text(960 - chrome.Width(initial, 40) / 2, 66, 40, chrome.C(255, 255, 255), initial);
  }
  {
    std::time_t t = std::time(nullptr);
    std::tm tm_local{};
#if REX_PLATFORM_WIN32
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    char clock[16];
    int hour = tm_local.tm_hour % 12 == 0 ? 12 : tm_local.tm_hour % 12;
    std::snprintf(clock, sizeof(clock), "%d:%02d", hour, tm_local.tm_min);
    chrome.Text(1560 - chrome.Width(clock, kTextSize), 82, kTextSize, chrome.C(255, 255, 255),
                clock);
  }

  // Upper half: description and the entry box.
  dl->AddRectFilledMultiColor(cv.P(310, 135), cv.P(1605, 470), cv.C(162, 165, 168),
                              cv.C(162, 165, 168), cv.C(116, 119, 122), cv.C(116, 119, 122));
  if (!anim.message.empty()) {
    content.Text(358, 160, kSmallSize, content.C(245, 245, 245), anim.message, 1066);
  }
  cv.Rect(358, 386, 1424, 447, cv.C(226, 226, 226));
  content.Text(372, 396, 34, content.C(40, 40, 40), k.text);
  if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
    const float caret_x = 372 + content.Width(k.text.substr(0, k.cursor), 34);
    content.Rect(caret_x, 394, caret_x + 2, 440, content.C(40, 40, 40));
  }

  // Lower half: the keys.
  cv.Rect(310, 470, 1605, 925, cv.C(204, 207, 210));
  const auto& keys = Keys(k.symbols);
  for (int i = 0; i < int(keys.size()); ++i) {
    const KeyDef& key = keys[i];
    const KeyRect r = RectOf(key, k.symbols);
    const bool sel = i == k.selected;
    const bool done = key.ch == kKeyDone;
    ImU32 face = content.C(232, 234, 236), text = content.C(48, 48, 48);
    if (done) face = content.C(40, 140, 24), text = content.C(255, 255, 255);
    if (sel) face = content.C(30, 132, 20), text = content.C(255, 255, 255);
    const bool enabled = KeyEnabled(k, key);
    if (!enabled && !sel) text = content.C(165, 168, 171);
    if (key.ch == kKeyCaps && k.gamertag && k.caps && !sel) text = content.C(30, 132, 20);
    const std::string label = KeyLabel(k, key);
    content.Rect(r.x0, r.y0, r.x1, r.y1, content.C(150, 154, 158));
    content.Rect(r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, face);
    if (sel && done) {
      content.Rect(r.x0 + 3, r.y0 + 3, r.x1 - 3, r.y1 - 3, content.C(60, 170, 40));
    }
    const float w = content.Width(label, 28);
    if (key.c0 == 0 || key.c0 == 11) {
      // Side keys: shortcut on top, name below.
      const float gw = ButtonGlyphWidth(content, key.glyph);
      ButtonGlyph(content, (r.x0 + r.x1 - gw) / 2, r.y0 + 30, key.glyph);
      content.Text((r.x0 + r.x1 - w) / 2, r.y0 + 50, 28, text, label);
    } else if (!key.glyph.empty()) {
      // Bottom row: shortcut then name.
      ButtonGlyph(content, r.x0 + 12, (r.y0 + r.y1) / 2, key.glyph);
      content.Text(r.x0 + 54, r.y0 + 10, 28, text, label);
    } else {
      content.Text((r.x0 + r.x1 - w) / 2, r.y0 + 10, 30, text, label);
    }
    if (cv.Inside(mouse, r.x0, r.y0, r.x1, r.y1)) {
      if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) hit.hover = i;
      if (io.MouseClicked[0]) hit.click = i;
    }
  }

  // Button hints.
  float hx = 351;
  for (const auto& [button, label] : std::vector<std::pair<std::string, std::string>>{
           {"A", "Select"}, {"B", "Back"}}) {
    ButtonGlyph(chrome, hx, 958, button);
    hx += ButtonGlyphWidth(chrome, button) + 10;
    chrome.Text(hx, 958 - kSmallSize / 2 - 2, kSmallSize, chrome.C(255, 255, 255), label);
    hx += chrome.Width(label, kSmallSize) + 30;
  }
  return hit;
}

// Drives a KeyboardState from one frame of input. A physical keyboard types
// directly (the pad bindings it also drives are ignored while a key is down).
// Returns 1 for Done, -1 for Back, 0 otherwise.
int KeyboardInput(KeyboardState& k, const PadNav::Input& in, const KeyboardHit& hit) {
  if (in.typing) {
    for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
      if (c < 128) {
        KeyboardType(k, char(c));
      } else if (k.free_text && c >= 0xA0) {
        std::string s;
        if (c < 0x800) {
          s += char(0xC0 | (c >> 6));
          s += char(0x80 | (c & 0x3F));
        } else {
          s += char(0xE0 | (c >> 12));
          s += char(0x80 | ((c >> 6) & 0x3F));
          s += char(0x80 | (c & 0x3F));
        }
        KeyboardTypeText(k, s);
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) KeyboardBackspace(k);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && k.cursor < k.text.size()) {
      k.text.erase(k.cursor, NextChar(k.text, k.cursor) - k.cursor);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) k.cursor = PrevChar(k.text, k.cursor);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) k.cursor = NextChar(k.text, k.cursor);
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) k.cursor = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_End)) k.cursor = k.text.size();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      return 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return -1;
    return 0;
  }
  if (in.b) return -1;
  if (in.start) return 1;
  if (in.x) KeyboardBackspace(k);
  if (in.y && k.gamertag) KeyboardType(k, ' ');
  if (k.free_text && (k.symbols ? in.rt : in.lt)) ToggleSymbols(k);
  if (in.lb) k.cursor = PrevChar(k.text, k.cursor);
  if (in.rb) k.cursor = NextChar(k.text, k.cursor);
  if (in.up) KeyboardMove(k, 0, -1);
  if (in.down) KeyboardMove(k, 0, 1);
  if (in.left) KeyboardMove(k, -1, 0);
  if (in.right) KeyboardMove(k, 1, 0);
  if (hit.hover >= 0) k.selected = hit.hover;
  int press = hit.click >= 0 ? hit.click : in.a ? k.selected : -1;
  if (press >= 0) {
    k.selected = press;
    return KeyboardPress(k, press) ? 1 : 0;
  }
  return 0;
}


}  // namespace sysui

class MessageBoxDialog : public XamDialog {
 public:
  MessageBoxDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title, std::string description,
                   std::vector<std::string> buttons, uint32_t default_button)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
    selected_ = static_cast<int>(default_button_);
  }

  uint32_t chosen_button() const { return chosen_button_; }

  void OnDraw(ImGuiIO& io) override {
    sysui::Screen s;
    s.header = title_;
    s.blade_label = "Xbox Guide";
    s.message = description_;
    for (const auto& b : buttons_) s.rows.push_back({b, ""});
    if (s.rows.empty()) s.rows.push_back({"OK", ""});
    auto in = nav_.Poll();
    s.selected = std::clamp(selected_, 0, int(s.rows.size()) - 1);
    s.hints = {{"A", "Select"}};
    if (closing_at_ >= 0) {
      const bool done = sysui::CloseAnimation(s, closing_at_);
      sysui::DrawScreen(ImGui::GetBackgroundDrawList(), imgui_drawer()->ui_font(), io, s, &scroll_,
                        1.0f);
      if (done) Close();
      return;
    }
    const bool ready = sysui::OpenAnimation(s, &opened_at_);
    auto hit = sysui::DrawScreen(ImGui::GetBackgroundDrawList(), imgui_drawer()->ui_font(), io, s,
                                 &scroll_, 1.0f);
    if (ready && sysui::Navigate(in, hit, int(s.rows.size()), &selected_)) {
      chosen_button_ = static_cast<uint32_t>(selected_);
      closing_at_ = ImGui::GetTime();
    }
  }

 private:
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
  int selected_ = 0;
  int scroll_ = 0;
  double opened_at_ = -1;
  double closing_at_ = -1;
  sysui::PadNav nav_;
};

// A vertical list of choices; B cancels (player pickers, notices).
class ListDialog : public XamDialog {
 public:
  ListDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title, std::string description,
             std::vector<std::string> items)
      : XamDialog(imgui_drawer),
        title_(title.empty() ? "Select" : std::move(title)),
        description_(std::move(description)),
        items_(std::move(items)) {}

  int chosen() const { return chosen_; }

  void OnDraw(ImGuiIO& io) override {
    sysui::Screen s;
    s.header = "Xbox Guide";
    s.blade_label = title_;
    s.message = description_;
    for (const auto& item : items_) s.rows.push_back({item, ""});
    const bool notice = s.rows.empty();
    if (notice) s.rows.push_back({"OK", ""});
    auto in = nav_.Poll();
    s.selected = std::clamp(selected_, 0, int(s.rows.size()) - 1);
    s.hints = {{"A", "Select"}, {"B", "Back"}};
    if (closing_at_ >= 0) {
      const bool done = sysui::CloseAnimation(s, closing_at_);
      sysui::DrawScreen(ImGui::GetBackgroundDrawList(), imgui_drawer()->ui_font(), io, s, &scroll_,
                        1.0f);
      if (done) Close();
      return;
    }
    const bool ready = sysui::OpenAnimation(s, &opened_at_);
    auto hit = sysui::DrawScreen(ImGui::GetBackgroundDrawList(), imgui_drawer()->ui_font(), io, s,
                                 &scroll_, 1.0f);
    if (!ready) {
    } else if (sysui::Navigate(in, hit, int(s.rows.size()), &selected_)) {
      chosen_ = notice ? -1 : selected_;
      closing_at_ = ImGui::GetTime();
    } else if (in.b || in.guide) {
      closing_at_ = ImGui::GetTime();
    }
  }

 private:
  std::string title_;
  std::string description_;
  std::vector<std::string> items_;
  int chosen_ = -1;
  int selected_ = 0;
  int scroll_ = 0;
  double opened_at_ = -1;
  double closing_at_ = -1;
  sysui::PadNav nav_;
};

// A console-style notification at the bottom of the screen that closes
// itself and takes no input (the game keeps running). Several stack upward.
class ToastDialog : public XamDialog {
 public:
  ToastDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string text)
      : XamDialog(imgui_drawer), text_(std::move(text)), slot_(active_++) {}
  ~ToastDialog() override { --active_; }

  void OnDraw(ImGuiIO& io) override {
    const double now = ImGui::GetTime();
    if (start_ < 0) {
      start_ = now;
    }
    const double age = now - start_;
    if (age > 5.0) {
      Close();
      return;
    }
    // Slide up and fade in, fade out at the end.
    float alpha = float(std::min({1.0, age / 0.2, (5.0 - age) / 0.4}));
    sysui::Canvas cv(ImGui::GetForegroundDrawList(), imgui_drawer()->ui_font(), io, alpha);
    const float w = cv.Width(text_, 30) + 110;
    const float x0 = (1920 - w) / 2;
    const float y0 = 920 - slot_ * 84 + float(std::max(0.0, 0.2 - age) * 120);
    cv.Rect(x0, y0, x0 + w, y0 + 70, cv.C(38, 38, 38, 235), 35);
    cv.Circle(x0 + 38, y0 + 35, 22, cv.C(96, 168, 40));
    cv.Circle(x0 + 38, y0 + 35, 15, cv.C(38, 38, 38, 235));
    cv.Circle(x0 + 38, y0 + 35, 10, cv.C(96, 168, 40));
    cv.Text(x0 + 76, y0 + 16, 30, cv.C(255, 255, 255), text_);
  }

 private:
  static inline int active_ = 0;  // UI thread only
  std::string text_;
  int slot_;
  double start_ = -1;
};

void xeXamShowToast(const std::string& text) {
  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  ui::WindowedAppContext* app_context = emulator ? emulator->app_context() : nullptr;
  REXKRNL_INFO("Notice: {}", text);
  if (!imgui_drawer || !app_context || REXCVAR_GET(headless)) {
    return;
  }
  // Dialogs register with the drawer, which belongs to the UI thread.
  app_context->CallInUIThread([imgui_drawer, text]() { new ToastDialog(imgui_drawer, text); });
}

// Who is talking in voice chat: a pill per speaker in the top-right corner,
// 360 style, drawn over the game (like the toasts it is never "UI", so the
// game keeps its input). A name stays half a second after they go quiet.
class VoiceIndicator : public XamDialog {
 public:
  explicit VoiceIndicator(rex::ui::ImGuiDrawer* imgui_drawer) : XamDialog(imgui_drawer) {}

  void OnDraw(ImGuiIO& io) override {
    const double now = ImGui::GetTime();
    if (now >= next_poll_) {
      next_poll_ = now + 0.15;
      for (const auto& name : xeXFriendsTalkingNames()) last_heard_[name] = now;
      // Debug: voice_indicator_test.on beside the exe shows a pretend talker
      // (checked once a second).
      if (now >= next_test_check_) {
        next_test_check_ = now + 1.0;
        std::error_code ec;
        test_ = std::filesystem::exists("voice_indicator_test.on", ec);
      }
      if (test_) last_heard_["Test Buddy"] = now;
    }
    int row = 0;
    if (now >= next_mute_check_) {
      next_mute_check_ = now + 0.25;
      mic_muted_ = rex::system::xam::eos::VoiceEnabled() && rex::system::xam::eos::VoiceMicMuted();
    }
    if (mic_muted_) {
      // Muted: say so, so a silent mic is never a surprise.
      sysui::Canvas cv(ImGui::GetForegroundDrawList(), imgui_drawer()->ui_font(), io, 0.85f);
      const std::string label = "Mic muted";
      const float w = cv.Width(label, 26) + 84;
      const float x0 = 1920 - 40 - w, y0 = 40;
      cv.Rect(x0, y0, x0 + w, y0 + 52, cv.C(38, 38, 38, 220), 26);
      cv.Circle(x0 + 28, y0 + 26, 18, cv.C(176, 40, 40));
      cv.Speaker(x0 + 29, y0 + 26, 22, cv.C(255, 255, 255));
      cv.Text(x0 + 58, y0 + 12, 26, cv.C(255, 255, 255), label);
      row = 1;
    }
    for (auto it = last_heard_.begin(); it != last_heard_.end();) {
      const double quiet = now - it->second;
      if (quiet > 0.5) {
        it = last_heard_.erase(it);
        continue;
      }
      if (row < 6) {
        const float alpha = float(std::min(1.0, (0.5 - quiet) / 0.2));
        sysui::Canvas cv(ImGui::GetForegroundDrawList(), imgui_drawer()->ui_font(), io, alpha);
        const float w = cv.Width(it->first, 26) + 84;
        const float x0 = 1920 - 40 - w, y0 = 40 + row * 62.0f;
        cv.Rect(x0, y0, x0 + w, y0 + 52, cv.C(38, 38, 38, 220), 26);
        cv.Circle(x0 + 28, y0 + 26, 18, cv.C(96, 168, 40));
        cv.Speaker(x0 + 29, y0 + 26, 22, cv.C(255, 255, 255));
        cv.Text(x0 + 58, y0 + 12, 26, cv.C(255, 255, 255), it->first);
        ++row;
      }
      ++it;
    }
  }

 private:
  std::map<std::string, double> last_heard_;
  double next_poll_ = 0;
  double next_test_check_ = 0;
  bool test_ = false;
  double next_mute_check_ = 0;
  bool mic_muted_ = false;
};

void xeXamEnsureVoiceIndicator() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  ui::WindowedAppContext* app_context = emulator ? emulator->app_context() : nullptr;
  if (!imgui_drawer || !app_context || REXCVAR_GET(headless)) return;
  app_context->CallInUIThread([imgui_drawer]() { new VoiceIndicator(imgui_drawer); });
}

// Builds a dialog on the UI thread (dialogs register with the drawer there);
// null when there is no UI.
template <typename T, typename... Args>
T* xeXamNewDialog(Args... args) {
  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  ui::WindowedAppContext* app_context = emulator ? emulator->app_context() : nullptr;
  if (!imgui_drawer || !app_context || REXCVAR_GET(headless)) {
    return nullptr;
  }
  T* dialog = nullptr;
  app_context->CallInUIThreadSynchronous(
      [&]() { dialog = new T(imgui_drawer, std::move(args)...); });
  return dialog;
}

int xeXamShowListDialog(const std::string& title, const std::string& description,
                        const std::vector<std::string>& items) {
  auto* dialog = xeXamNewDialog<ListDialog>(title, description, items);
  if (!dialog) {
    return -1;
  }
  int chosen = -1;
  xeXamDispatchDialog<ListDialog>(
      dialog,
      [&chosen](ListDialog* dialog) -> X_RESULT {
        chosen = dialog->chosen();
        return X_ERROR_SUCCESS;
      },
      0);
  return chosen;
}

// The Xbox Guide: blades Friends | <gamertag> (home) | Recent Players |
// Online Room, switched with LB/RB or left/right; players, invites and the
// room code keyboard open pages on top of the blade (B goes back).
// Messages: announcements from the maintainer and messages from other players,
// fetched from the leaderboard server (stats_server) every minute. A new one
// shows a notice; while one is unread, the Guide button (or Enter) opens it
// instead of the Guide's home. Player messages carry negative ids here so they
// never meet an announcement's. Read ones are remembered in
// announcements_read.txt beside the exe.
namespace announce {

struct Item {
  int64_t id = 0;
  std::string from;  // a player's message: their gamertag; empty: an announcement
  std::string to;    // a message we sent: who to (from is then empty)
  std::string title, text;
  int64_t created_s = 0;  // unix seconds
  bool read = false;
};

std::mutex g_mutex;
std::vector<Item> g_items;  // newest first
std::set<int64_t> g_read;
std::atomic<bool> g_started{false};

std::filesystem::path ReadPath() {
  return rex::filesystem::GetExecutableFolder() / "announcements_read.txt";
}

void LoadRead() {
  std::ifstream in(ReadPath());
  for (std::string line; std::getline(in, line);) {
    const int64_t id = std::strtoll(line.c_str(), nullptr, 10);
    if (id != 0) g_read.insert(id);  // player messages are negative
  }
}

void SaveRead() {
  std::ofstream out(ReadPath(), std::ios::trunc);
  for (int64_t id : g_read) out << id << '\n';
}

std::string MyGamertag() {
  auto* profile = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->user_profile() : nullptr;
  return profile ? profile->name() : std::string();
}

// Messages we sent, so the Messages list shows both sides: kept in
// messages_sent.txt beside the exe (time, gamertag, text; tabs and newlines
// escaped), the last 50. Their ids sit far below any received one's.
constexpr int64_t kSentIdBase = -4000000000000LL;
int64_t g_sent_count = 0;

std::filesystem::path SentPath() {
  return rex::filesystem::GetExecutableFolder() / "messages_sent.txt";
}

std::string Escape(const std::string& s) {
  std::string out;
  for (char ch : s) {
    if (ch == '\\') out += "\\\\";
    else if (ch == '\t') out += "\\t";
    else if (ch == '\n') out += "\\n";
    else out += ch;
  }
  return out;
}

std::string Unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char e = s[++i];
      out += e == 't' ? '\t' : e == 'n' ? '\n' : e;
    } else {
      out += s[i];
    }
  }
  return out;
}

// With g_mutex held.
void AddSent(const std::string& to, const std::string& text, int64_t when, bool save) {
  Item item{kSentIdBase - (++g_sent_count), "", to, "", text, when, true};
  g_items.push_back(std::move(item));
  std::stable_sort(g_items.begin(), g_items.end(),
                   [](const Item& a, const Item& b) { return a.created_s > b.created_s; });
  if (g_items.size() > 60) g_items.resize(60);
  if (save) {
    std::vector<const Item*> sent;
    for (const auto& i : g_items) {
      if (!i.to.empty()) sent.push_back(&i);
    }
    std::ofstream out(SentPath(), std::ios::trunc);
    for (auto it = sent.rbegin(); it != sent.rend() && std::distance(sent.rbegin(), it) < 50; ++it) {
      out << (*it)->created_s << '\t' << Escape((*it)->to) << '\t' << Escape((*it)->text) << '\n';
    }
  }
}

void LoadSent() {
  std::ifstream in(SentPath());
  for (std::string line; std::getline(in, line);) {
    const size_t a = line.find('\t'), b2 = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b2 == std::string::npos) continue;
    AddSent(Unescape(line.substr(a + 1, b2 - a - 1)), Unescape(line.substr(b2 + 1)),
            std::strtoll(line.c_str(), nullptr, 10), false);
  }
}

void PollInbox(uint32_t title_id, int64_t& newest) {
  std::vector<rex::system::xam::stats::PlayerMessage> got;
  if (!title_id || !rex::system::xam::stats::GetInbox(title_id, MyGamertag(), newest, got) || got.empty()) {
    return;
  }
  std::vector<Item> unread;
  {
    std::lock_guard lock(g_mutex);
    std::vector<Item> fresh;
    for (const auto& m : got) {
      if (m.id <= newest) continue;
      Item item{-m.id, m.from, "", "", m.text, m.created_ms / 1000, g_read.count(-m.id) != 0};
      if (!item.read) unread.push_back(item);
      fresh.push_back(std::move(item));
    }
    g_items.insert(g_items.begin(), fresh.begin(), fresh.end());
    std::stable_sort(g_items.begin(), g_items.end(),
                     [](const Item& a, const Item& b) { return a.created_s > b.created_s; });
    if (g_items.size() > 60) g_items.resize(60);
    newest = std::max(newest, got.front().id);
  }
  if (unread.size() == 1) {
    xeXamShowToast("Message from " + unread.front().from + " - press Guide or Enter to read");
  } else if (unread.size() > 1) {
    xeXamShowToast(std::to_string(unread.size()) + " new messages - press Guide or Enter to read");
  }
}

// Sends in the background (the request blocks) and says how it went.
void Send(std::string to, std::string text) {
  std::thread([to = std::move(to), text = std::move(text)]() {
    const uint32_t title_id = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->title_id() : 0;
    std::string error;
    if (rex::system::xam::stats::SendPlayerMessage(title_id, MyGamertag(), to, text, &error)) {
      {
        std::lock_guard lock(g_mutex);
        AddSent(to, text, int64_t(std::time(nullptr)), true);
      }
      xeXamShowToast("Message sent to " + to);
    } else {
      xeXamShowToast("Message not sent: " + error);
    }
  }).detach();
}

void PollLoop() {
  {
    std::lock_guard lock(g_mutex);
    LoadRead();
    LoadSent();
  }
  std::this_thread::sleep_for(std::chrono::seconds(8));
  int64_t newest = 0, newest_message = 0;
  for (;;) {
    std::vector<rex::system::xam::stats::Announcement> got;
    const uint32_t title_id = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->title_id() : 0;
    if (title_id && rex::system::xam::stats::GetAnnouncements(title_id, newest, got) && !got.empty()) {
      std::vector<Item> unread;
      {
        std::lock_guard lock(g_mutex);
        // `got` is newest first; g_items stays newest first too.
        std::vector<Item> fresh;
        for (const auto& a : got) {
          if (a.id <= newest) continue;
          Item item{a.id, "", "", a.title, a.text, a.created_ms / 1000, g_read.count(a.id) != 0};
          if (!item.read) unread.push_back(item);
          fresh.push_back(std::move(item));
        }
        g_items.insert(g_items.begin(), fresh.begin(), fresh.end());
        if (g_items.size() > 20) g_items.resize(20);
        newest = std::max(newest, got.front().id);
      }
      if (unread.size() == 1) {
        std::string title = unread.front().title.empty() ? "New message" : unread.front().title;
        if (title.size() > 40) title = title.substr(0, 37) + "...";
        xeXamShowToast(title + " - press Guide or Enter to read");
      } else if (unread.size() > 1) {
        xeXamShowToast(std::to_string(unread.size()) + " new messages - press Guide or Enter to read");
      }
    }
    PollInbox(title_id, newest_message);
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
}

void EnsureStarted() {
  if (!g_started.exchange(true)) {
    std::thread(PollLoop).detach();
  }
}

// The oldest unread announcement's id, 0 when all are read.
int64_t FirstUnread() {
  std::lock_guard lock(g_mutex);
  for (auto it = g_items.rbegin(); it != g_items.rend(); ++it) {
    if (!it->read) return it->id;
  }
  return 0;
}

int UnreadCount() {
  std::lock_guard lock(g_mutex);
  int n = 0;
  for (const auto& item : g_items) n += !item.read;
  return n;
}

std::vector<Item> Snapshot() {
  std::lock_guard lock(g_mutex);
  return g_items;
}

void MarkRead(int64_t id) {
  std::lock_guard lock(g_mutex);
  for (auto& item : g_items) {
    if (item.id == id && !item.read) {
      item.read = true;
      g_read.insert(id);
      SaveRead();
    }
  }
}

}  // namespace announce

class GuideDialog : public XamDialog {
 public:
  GuideDialog(rex::ui::ImGuiDrawer* imgui_drawer, int blade, int64_t open_message = 0)
      : XamDialog(imgui_drawer), blade_(std::clamp(blade, 0, kBlades - 1)) {
    // Opened by the Guide button with a message unread: straight to it, and
    // its OK closes the Guide.
    if (open_message) {
      pages_.push_back({PageKind::kMessage, std::to_string(open_message)});
      close_after_message_ = true;
    }
  }

  void OnDraw(ImGuiIO& io) override {
    const double now = ImGui::GetTime();
    if (snapshot_at_ < 0 || now - snapshot_at_ > 0.5) {
      snap_ = xeXFriendsSnapshot();
      snapshot_at_ = now;
    }
    auto in = nav_.Poll();

    sysui::Screen s;
    s.gamertag = LocalGamertag();
    std::vector<std::function<void()>> actions;
    Page* page = pages_.empty() ? nullptr : &pages_.back();
    int* selected = page ? &page->selected : &root_selected_[blade_];
    int* scroll = page ? &page->scroll : &root_scroll_[blade_];
    const bool keyboard = page && page->kind == PageKind::kRoomCode;
    WantText(keyboard && closing_at_ < 0);
    if (keyboard) {
      switch (page->purpose) {
        case KeyboardPurpose::kMakeRoom:
          s.header = "Make a Private Room";
          s.message =
              "Pick a code for your room (or keep this one), then share it with your friends. "
              "Only players with the same code see each other's games.";
          break;
        case KeyboardPurpose::kJoinRoom:
          s.header = "Join a Private Room";
          s.message =
              "Type the code your friends are using. Only players with the same code see each "
              "other's games.";
          break;
        case KeyboardPurpose::kMessage:
          s.header = "Message to " + page->message_to;
          s.message = "Type your message, then press Enter (or Start). It waits for them if they're offline.";
          break;
        case KeyboardPurpose::kChat:
          s.header = "Room Chat";
          s.message = "Type a message for everyone in your online room, then press Enter (or Start).";
          break;
        case KeyboardPurpose::kGamertag:
          s.header = "Change Gamertag";
          s.message =
              "Type your new gamertag (up to 15 letters, numbers and spaces). Other players see "
              "it straight away; your saves and settings stay with you.";
          break;
      }
      if (snap_.in_game && page->purpose != KeyboardPurpose::kGamertag &&
          page->purpose != KeyboardPurpose::kChat && page->purpose != KeyboardPurpose::kMessage) {
        s.message += " Changing room leaves the game you're in.";
      }
    } else if (page) {
      BuildPage(*page, s, actions);
    } else {
      BuildBlade(s, actions);
    }
    // The blades stay on either side on pages too, so the panel keeps its
    // shape; the open blade stays put and the tabs pile up around it.
    for (int i = 0; i < blade_; ++i) s.left_blades.push_back(BladeName(i));
    for (int i = blade_ + 1; i < kBlades; ++i) s.right_blades.push_back(BladeName(i));
    s.anchor_left = kGuideBladeHome;
    s.anchor_total = kBlades - 1;
    if (!status_.empty()) {
      s.message = s.message.empty() ? status_ : status_ + "\n" + s.message;
    }
    s.selected = s.rows.empty() ? -1 : std::clamp(*selected, 0, int(s.rows.size()) - 1);
    if (!s.rows.empty()) s.hints.push_back({"A", "Select"});
    s.hints.push_back({"B", page ? "Back" : "Close"});
    s.hints.push_back({"LB RB", "Switch"});
    if (rex::system::xam::eos::VoiceEnabled()) {
      s.hints.push_back({"Y", rex::system::xam::eos::VoiceMicMuted() ? "Unmute Mic" : "Mute Mic"});
    }

    auto* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = imgui_drawer()->ui_font();
    if (closing_at_ >= 0) {
      const bool done = sysui::CloseAnimation(s, closing_at_);
      Draw(dl, font, io, s, page, scroll);
      if (done) {
        Close();
        if (exit_requested_) ExitToDesktop();
      }
      return;
    }
    const bool ready = sysui::OpenAnimation(s, &opened_at_);
    // Changing blade or page: the old contents fade out, then the new ones
    // fade in (the tabs move at the swap).
    // To or from the keyboard (a panel of its own) the whole panel fades.
    sysui::Hit hit;
    sysui::KeyboardHit key_hit;
    const bool whole_panel = leaving_keyboard_ != keyboard;
    const double fade_out = whole_panel ? kPanelFadeOut : kFadeOut;
    const double fade_in = whole_panel ? kPanelFadeIn : kFadeIn;
    const double since = now - transition_at_;
    if (transition_at_ >= 0 && since < fade_out) {
      const float a = float(1.0 - since / fade_out);
      leaving_.content_alpha = a;
      if (whole_panel) leaving_.chrome_alpha = leaving_.panel_alpha = a;
      if (leaving_keyboard_) {
        sysui::DrawKeyboard(dl, font, io, leaving_, leaving_keys_);
      } else {
        sysui::DrawScreen(dl, font, io, leaving_, &leaving_scroll_, 1.0f);
      }
    } else {
      if (transition_at_ >= 0 && since < fade_out + fade_in) {
        const float a = float((since - fade_out) / fade_in);
        s.content_alpha = std::min(s.content_alpha, a);
        if (whole_panel) {
          s.chrome_alpha = std::min(s.chrome_alpha, a);
          s.panel_alpha = std::min(s.panel_alpha, a);
        }
      }
      if (keyboard) {
        key_hit = sysui::DrawKeyboard(dl, font, io, s, page->keys);
      } else {
        hit = sysui::DrawScreen(dl, font, io, s, scroll, 1.0f);
      }
    }
    last_ = s;
    last_scroll_ = *scroll;
    last_keyboard_ = keyboard;
    if (keyboard) last_keys_ = page->keys;
    if (!ready) {
      return;
    }

    if (keyboard) {
      if (in.guide && !in.typing) {
        BeginClose();
        return;
      }
      const int result = sysui::KeyboardInput(page->keys, in, key_hit);
      if (result > 0) {
        SubmitKeyboard(page->keys.text, page->purpose);
      } else if (result < 0) {
        LeaveKeyboard();
      }
      return;
    }
    if (in.guide) {
      BeginClose();
      return;
    }
    // Y: mute or unmute your mic (others are still heard).
    if (in.y && rex::system::xam::eos::VoiceEnabled()) {
      const bool muted = !rex::system::xam::eos::VoiceMicMuted();
      rex::system::xam::eos::SetVoiceMicMuted(muted);
      xeXamShowToast(muted ? "Mic muted - you can still hear others" : "Mic on");
      return;
    }
    if (in.b) {
      status_.clear();
      if (pages_.empty()) {
        BeginClose();
      } else {
        PopPage();
      }
      return;
    }
    int delta = hit.click_blade;
    if (in.lb || in.left) delta = -1;
    if (in.rb || in.right) delta = 1;
    if (delta) {
      SwitchBlade(blade_ + delta);
      return;
    }
    if (sysui::Navigate(in, hit, int(s.rows.size()), selected) && *selected >= 0 &&
        *selected < int(actions.size()) && actions[*selected]) {
      status_.clear();
      const size_t depth = pages_.size();
      actions[*selected]();
      if (pages_.size() > depth) StartTransition();
    }
  }

 private:
  static constexpr int kBlades = 4;
  enum class PageKind { kPlayer, kInvites, kInvite, kRoomCode, kExitConfirm, kMessages, kMessage, kChat, kMics };
  enum class KeyboardPurpose { kJoinRoom, kMakeRoom, kGamertag, kChat, kMessage };
  struct Page {
    PageKind kind;
    std::string puid, gamertag;
    int selected = 0;
    int scroll = 0;
    sysui::KeyboardState keys;
    KeyboardPurpose purpose = KeyboardPurpose::kJoinRoom;  // keyboard pages
    std::string message_to;  // KeyboardPurpose::kMessage: the gamertag it goes to
  };

  void Draw(ImDrawList* dl, ImFont* font, ImGuiIO& io, const sysui::Screen& s, Page* page,
            int* scroll) {
    if (page && page->kind == PageKind::kRoomCode) {
      sysui::DrawKeyboard(dl, font, io, s, page->keys);
    } else {
      sysui::DrawScreen(dl, font, io, s, scroll, 1.0f);
    }
  }

  void BeginClose() {
    if (closing_at_ < 0) closing_at_ = ImGui::GetTime();
  }

  // Closes the game the same way as the window's close button (OnClosing:
  // terminate the title, flush the logs, exit), after this frame.
  static void ExitToDesktop() {
    REXKRNL_INFO("Guide: exit to desktop");
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::WindowedAppContext* app_context = emulator ? emulator->app_context() : nullptr;
    if (!app_context) return;
    app_context->CallInUIThreadDeferred([emulator, app_context]() {
      if (ui::Window* window = emulator->display_window()) {
        window->RequestClose();
      } else {
        app_context->QuitFromUIThread();
      }
    });
  }

  static std::string LocalGamertag() {
    auto* profile = REX_KERNEL_STATE()->user_profile();
    return profile ? profile->name() : "Player";
  }

  std::string BladeName(int blade) const {
    switch (blade) {
      case kGuideBladeFriends: return "Friends";
      case kGuideBladeHome: return LocalGamertag();
      case kGuideBladeRecent: return "Recent Players";
      default: return "Online Room";
    }
  }

  void SwitchBlade(int blade) {
    blade = std::clamp(blade, 0, kBlades - 1);
    if (blade != blade_) {
      StartTransition();
      blade_ = blade;
      pages_.clear();
      status_.clear();
    }
  }

  void PopPage() {
    pages_.pop_back();
    StartTransition();
  }

  // Timings from the console's Guide (about 9 frames in all); a whole panel
  // (to or from the keyboard) fades a little slower.
  static constexpr double kFadeOut = 0.06, kFadeIn = 0.09;
  static constexpr double kPanelFadeOut = 0.1, kPanelFadeIn = 0.12;
  // Keeps showing what was on screen while it fades out.
  void StartTransition() {
    transition_at_ = ImGui::GetTime();
    leaving_ = last_;
    leaving_scroll_ = last_scroll_;
    leaving_keyboard_ = last_keyboard_;
    leaving_keys_ = last_keys_;
  }

  // A code that is easy to read out (no 0/O, 1/I).
  static std::string RandomRoomCode() {
    static constexpr char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::string code;
    for (int i = 0; i < 6; ++i) code += kChars[rd() % (sizeof(kChars) - 1)];
    return code;
  }

  void OpenKeyboard(KeyboardPurpose purpose, const std::string& message_to = {}) {
    Page page{PageKind::kRoomCode};
    page.purpose = purpose;
    page.message_to = message_to;
    switch (purpose) {
      case KeyboardPurpose::kMakeRoom: page.keys.text = RandomRoomCode(); break;
      case KeyboardPurpose::kJoinRoom: page.keys.text = snap_.room; break;
      case KeyboardPurpose::kGamertag:
        page.keys.gamertag = true;
        page.keys.max_length = 15;
        page.keys.text = LocalGamertag();
        break;
      case KeyboardPurpose::kChat:
      case KeyboardPurpose::kMessage:
        page.keys.gamertag = true;
        page.keys.free_text = true;
        page.keys.caps = false;
        page.keys.max_length = purpose == KeyboardPurpose::kChat ? 120 : 250;
        break;
    }
    page.keys.cursor = page.keys.text.size();
    pages_.push_back(std::move(page));
  }

  void LeaveKeyboard() { PopPage(); }

  void SubmitKeyboard(const std::string& text, KeyboardPurpose purpose) {
    if (purpose == KeyboardPurpose::kMessage) {
      if (!text.empty() && !pages_.empty()) announce::Send(pages_.back().message_to, text);
      LeaveKeyboard();
      return;
    }
    if (purpose == KeyboardPurpose::kChat) {
      if (!text.empty() && xeXFriendsSendChat(text) == 0) {
        status_ = "Nobody else is in your online room right now.";
      }
      LeaveKeyboard();
      return;
    }
    if (purpose == KeyboardPurpose::kGamertag) {
      auto* profile = REX_KERNEL_STATE()->user_profile();
      if (profile && text != profile->name() && profile->Rename(text)) {
        xeXamShowToast("Gamertag changed to " + profile->name());
      }
      LeaveKeyboard();
      return;
    }
    SubmitRoomCode(text, purpose == KeyboardPurpose::kMakeRoom);
  }

  void SubmitRoomCode(const std::string& code, bool create) {
    if (code.empty()) {
      LeaveKeyboard();
      return;
    }
    auto note = [](const std::string& text) { xeXamShowToast(text); };
    if (create) {
      rex::system::xam::eos::SwitchRoom(code, true, note);
      xeXamShowToast("Making private room " + code);
    } else {
      rex::system::xam::eos::SwitchRoom(code, false, note);
      xeXamShowToast("Moving to private room " + code);
    }
    snapshot_at_ = -1;
    LeaveKeyboard();
  }

  // The chosen microphone's name for the Online Room row.
  static std::string MicLabel() {
    namespace eos = rex::system::xam::eos;
    const std::string chosen = eos::VoiceMic();
    if (chosen.empty()) return "Windows Default";
    for (const auto& mic : eos::VoiceMics()) {
      if (mic.id == chosen) return mic.name.size() > 28 ? mic.name.substr(0, 25) + "..." : mic.name;
    }
    return "Chosen";
  }

  static std::string Status(const GuidePlayer& p) {
    if (p.online && rex::system::xam::eos::VoiceTalking(p.puid)) return "Talking";
    return p.in_game ? "In a game" : p.online ? "Online" : "Offline";
  }

  static std::string Ago(int64_t when) {
    int64_t mins = (int64_t(std::time(nullptr)) - when) / 60;
    if (mins < 1) return "Just now";
    if (mins < 60) return std::to_string(mins) + " min ago";
    if (mins < 48 * 60) return std::to_string(mins / 60) + " hr ago";
    return std::to_string(mins / (24 * 60)) + " days ago";
  }

  const GuidePlayer* FindPlayer(const std::string& puid) const {
    for (const auto* list : {&snap_.room_players, &snap_.friends, &snap_.recent}) {
      for (const auto& p : *list) {
        if (p.puid == puid) return &p;
      }
    }
    return nullptr;
  }

  void OpenPlayer(const GuidePlayer& p) {
    pages_.push_back({PageKind::kPlayer, p.puid, p.gamertag});
  }

  void PlayerRows(const std::vector<GuidePlayer>& players, sysui::Screen& s,
                  std::vector<std::function<void()>>& actions, bool show_seen) {
    for (const auto& p : players) {
      s.rows.push_back({p.gamertag, show_seen && !p.online ? Ago(p.last_seen) : Status(p)});
      actions.push_back([this, p]() { OpenPlayer(p); });
    }
  }

  void BuildBlade(sysui::Screen& s, std::vector<std::function<void()>>& actions) {
    s.blade_label = BladeName(blade_);
    const bool online = snap_.online_enabled;
    if (blade_ != kGuideBladeHome && !online) {
      s.message = "Online play is off. Set net_transport to eos to play online.";
      return;
    }
    switch (blade_) {
      case kGuideBladeHome: {
        s.rows.push_back({"Resume Game", ""});
        actions.push_back([this]() { BeginClose(); });
        int friends_online = 0;
        for (const auto& f : snap_.friends) friends_online += f.online;
        s.rows.push_back(
            {"Friends", online ? std::to_string(friends_online) + " / " +
                                     std::to_string(snap_.friends.size()) + " online"
                               : ""});
        actions.push_back([this]() { SwitchBlade(kGuideBladeFriends); });
        s.rows.push_back({"Game Invites", std::to_string(snap_.invites.size())});
        actions.push_back([this]() { pages_.push_back({PageKind::kInvites}); });
        if (online) {
          const int unread = xeXFriendsChatUnread();
          s.rows.push_back({"Room Chat", unread ? std::to_string(unread) + " new" : ""});
          actions.push_back([this]() { pages_.push_back({PageKind::kChat}); });
        }
        {
          const int unread = announce::UnreadCount();
          s.rows.push_back({"Messages", unread ? std::to_string(unread) + " new" : ""});
          actions.push_back([this]() { pages_.push_back({PageKind::kMessages}); });
        }
        s.rows.push_back({"Recent Players", std::to_string(snap_.recent.size())});
        actions.push_back([this]() { SwitchBlade(kGuideBladeRecent); });
        s.rows.push_back({"Change Gamertag", LocalGamertag()});
        actions.push_back([this]() { OpenKeyboard(KeyboardPurpose::kGamertag); });
        s.rows.push_back({"Exit to Desktop", ""});
        actions.push_back([this]() { pages_.push_back({PageKind::kExitConfirm}); });
        break;
      }
      case kGuideBladeFriends:
        if (snap_.friends.empty()) {
          s.message = "No friends yet. Add people from Recent Players or the Online Room.";
        }
        PlayerRows(snap_.friends, s, actions, false);
        break;
      case kGuideBladeRecent:
        if (snap_.recent.empty()) {
          s.message = "Nobody yet. People you race online show up here.";
        }
        PlayerRows(snap_.recent, s, actions, true);
        break;
      default: {
        std::string room = snap_.room.empty() ? "Public room" : "Private room \"" + snap_.room + "\"";
        if (!snap_.connected) {
          s.message = room + " - connecting...";
        } else if (snap_.room_players.empty()) {
          s.message = room + ". Nobody else is here right now.";
        } else {
          s.message = room + ", " + std::to_string(snap_.room_players.size()) +
                      (snap_.room_players.size() == 1 ? " other player." : " other players.");
        }
        // Public / private toggle: in the public room it makes a private one
        // (keyboard with a fresh code); in a private one it goes back to public.
        if (snap_.room.empty()) {
          s.rows.push_back({"Room: Public", "Make Private"});
          actions.push_back([this]() { OpenKeyboard(KeyboardPurpose::kMakeRoom); });
        } else {
          s.rows.push_back({"Room: " + snap_.room, "Go Public"});
          actions.push_back([this]() {
            rex::system::xam::eos::SwitchRoom("");
            xeXamShowToast("Moving to the public room");
            snapshot_at_ = -1;
          });
        }
        s.rows.push_back({"Join a Private Room", ""});
        actions.push_back([this]() { OpenKeyboard(KeyboardPurpose::kJoinRoom); });
        {
          namespace eos = rex::system::xam::eos;
          const bool on = eos::VoiceEnabled();
          s.rows.push_back({"Voice Chat", !eos::VoiceAvailable() ? (on ? "On (no voice in this room)" : "Off")
                                          : !on                 ? "Off"
                                          : eos::VoiceMicMuted() ? "On, mic muted (Y)"
                                                                 : "On"});
          actions.push_back([this, on]() {
            rex::system::xam::eos::SetVoiceEnabled(!on);
            xeXamShowToast(!on ? "Voice chat on - everyone in your room can hear you" : "Voice chat off");
            snapshot_at_ = -1;
          });
          s.rows.push_back({"Microphone", MicLabel()});
          actions.push_back([this]() { pages_.push_back({PageKind::kMics}); });
        }
        {
          const int unread = xeXFriendsChatUnread();
          s.rows.push_back({"Room Chat", unread ? std::to_string(unread) + " new" : ""});
          actions.push_back([this]() { pages_.push_back({PageKind::kChat}); });
        }
        PlayerRows(snap_.room_players, s, actions, false);
        break;
      }
    }
  }

  void BuildPage(Page& page, sysui::Screen& s, std::vector<std::function<void()>>& actions) {
    switch (page.kind) {
      case PageKind::kRoomCode:
        break;
      case PageKind::kExitConfirm:
        s.blade_label = "Exit to Desktop";
        s.message =
            "Are you sure you want to exit to the desktop? Anything not saved since the game "
            "last saved will be lost.";
        s.rows.push_back({"No", ""});
        actions.push_back([this]() { PopPage(); });
        s.rows.push_back({"Yes", ""});
        actions.push_back([this]() {
          exit_requested_ = true;
          BeginClose();
        });
        break;
      case PageKind::kMics: {
        namespace eos = rex::system::xam::eos;
        s.blade_label = "Microphone";
        const auto mics = eos::VoiceMics();
        const std::string chosen = eos::VoiceMic();
        s.message = mics.empty() ? "Looking for microphones..." : "Pick the microphone voice chat uses.";
        s.rows.push_back({"Windows Default", chosen.empty() ? "Selected" : ""});
        actions.push_back([this]() {
          rex::system::xam::eos::SetVoiceMic("");
          xeXamShowToast("Microphone: Windows default");
          PopPage();
        });
        for (const auto& mic : mics) {
          std::string label = mic.name;
          if (label.size() > 40) label = label.substr(0, 37) + "...";
          s.rows.push_back({label, mic.id == chosen && !chosen.empty() ? "Selected" : mic.is_default ? "Default" : ""});
          actions.push_back([this, mic]() {
            rex::system::xam::eos::SetVoiceMic(mic.id);
            xeXamShowToast("Microphone: " + mic.name);
            PopPage();
          });
        }
        break;
      }
      case PageKind::kChat: {
        s.blade_label = "Room Chat";
        xeXFriendsChatSeen();
        s.rows.push_back({"Send a Message", ""});
        actions.push_back([this]() { OpenKeyboard(KeyboardPurpose::kChat); });
        const auto lines = xeXFriendsChat();
        if (lines.empty()) {
          s.message = "No messages yet. Everyone in your online room sees what you send here.";
        }
        // Newest first, under the Send row.
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
          s.rows.push_back({(it->self ? std::string("You") : it->gamertag) + ": " + it->text, Ago(it->time)});
          actions.push_back([]() {});
        }
        break;
      }
      case PageKind::kMessages: {
        s.blade_label = "Messages";
        const auto items = announce::Snapshot();
        if (items.empty()) s.message = "No messages.";
        for (const auto& item : items) {
          std::string label = !item.to.empty()     ? "To " + item.to + ": " + item.text
                              : !item.from.empty() ? item.from + ": " + item.text
                              : item.title.empty() ? std::string("Message")
                                                   : item.title;
          if (label.size() > 48) label = label.substr(0, 45) + "...";
          s.rows.push_back({(item.read ? std::string() : std::string("* ")) + label, Ago(item.created_s)});
          const int64_t id = item.id;
          actions.push_back([this, id]() { pages_.push_back({PageKind::kMessage, std::to_string(id)}); });
        }
        break;
      }
      case PageKind::kMessage: {
        const int64_t id = std::strtoll(page.puid.c_str(), nullptr, 10);
        announce::Item found;
        for (const auto& item : announce::Snapshot()) {
          if (item.id == id) found = item;
        }
        s.blade_label = !found.to.empty()   ? "Message to " + found.to
                        : !found.from.empty() ? "Message from " + found.from
                        : found.title.empty() ? std::string("Message")
                                              : found.title;
        s.message = found.text.empty() ? "This message is no longer available." : found.text;
        if (!found.read && found.id) announce::MarkRead(found.id);
        if (!found.from.empty()) {
          const std::string from = found.from;
          s.rows.push_back({"Reply", ""});
          actions.push_back([this, from]() { OpenKeyboard(KeyboardPurpose::kMessage, from); });
        }
        if (!found.to.empty()) {
          const std::string to = found.to;
          s.rows.push_back({"Send Another", ""});
          actions.push_back([this, to]() { OpenKeyboard(KeyboardPurpose::kMessage, to); });
        }
        s.rows.push_back({"OK", ""});
        actions.push_back([this]() {
          if (close_after_message_ && pages_.size() == 1) {
            BeginClose();
          } else {
            PopPage();
          }
        });
        break;
      }
      case PageKind::kInvites:
        s.blade_label = "Game Invites";
        if (snap_.invites.empty()) s.message = "No game invites.";
        for (const auto& invite : snap_.invites) {
          s.rows.push_back({invite.gamertag, Ago(invite.time)});
          actions.push_back([this, invite]() {
            pages_.push_back({PageKind::kInvite, invite.puid, invite.gamertag});
          });
        }
        break;
      case PageKind::kInvite: {
        s.blade_label = "Game Invite";
        s.message = page.gamertag + " invited you to join their game.";
        const std::string puid = page.puid;
        s.rows.push_back({"Accept", ""});
        actions.push_back([this, puid]() {
          if (xeXFriendsAnswerInvite(puid, true)) {
            BeginClose();
          } else {
            status_ = "That invite is no longer available.";
            PopPage();
          }
        });
        s.rows.push_back({"Decline", ""});
        actions.push_back([this, puid]() {
          xeXFriendsAnswerInvite(puid, false);
          snapshot_at_ = -1;
          PopPage();
        });
        break;
      }
      case PageKind::kPlayer: {
        const GuidePlayer* found = FindPlayer(page.puid);
        GuidePlayer p = found ? *found : GuidePlayer{page.puid, page.gamertag};
        s.blade_label = p.gamertag;
        s.message = Status(p) + (p.is_friend ? "  -  Friend" : "");
        if (p.in_game) {
          s.rows.push_back({"Join Game", ""});
          actions.push_back([this, p]() {
            if (xeXFriendsJoin(p.puid)) {
              BeginClose();
            } else {
              status_ = p.gamertag + " is no longer in a game.";
            }
          });
        }
        if (p.online && snap_.in_game) {
          s.rows.push_back({"Invite to Game", ""});
          actions.push_back([this, p]() {
            if (!xeXFriendsInvite(p.puid, p.gamertag)) {
              status_ = "You are not in a game to invite people to.";
            }
          });
        }
        s.rows.push_back({"Send Message", ""});
        actions.push_back([this, p]() { OpenKeyboard(KeyboardPurpose::kMessage, p.gamertag); });
        if (p.online) {
          const bool muted = rex::system::xam::eos::VoiceMuted(p.puid);
          s.rows.push_back({muted ? "Unmute Voice" : "Mute Voice", ""});
          actions.push_back([this, p, muted]() {
            rex::system::xam::eos::SetVoiceMuted(p.puid, !muted);
            xeXamShowToast((muted ? "Unmuted " : "Muted ") + p.gamertag);
          });
        }
        s.rows.push_back({p.is_friend ? "Remove Friend" : "Add Friend", ""});
        actions.push_back([this, p]() {
          xeXFriendsSetFriend(p.puid, p.gamertag, !p.is_friend);
          snapshot_at_ = -1;
        });
        break;
      }
    }
  }

  int blade_;
  int root_selected_[kBlades] = {};
  int root_scroll_[kBlades] = {};
  std::vector<Page> pages_;
  std::string status_;  // a line about the last action that could not be done
  GuideSnapshot snap_;
  double snapshot_at_ = -1;
  double opened_at_ = -1;
  double closing_at_ = -1;
  bool exit_requested_ = false;  // Exit to Desktop: quit once the Guide has closed
  bool close_after_message_ = false;  // opened straight onto an unread message
  double transition_at_ = -1;
  sysui::Screen last_, leaving_;
  int last_scroll_ = 0, leaving_scroll_ = 0;
  bool last_keyboard_ = false, leaving_keyboard_ = false;
  sysui::KeyboardState last_keys_, leaving_keys_;
  sysui::PadNav nav_;
};

std::atomic<bool> g_guide_open{false};
std::atomic<int64_t> g_ui_closed_ms{0};
std::atomic<uint16_t> g_prev_buttons[4] = {};
std::atomic<uint32_t> g_swallow_mask{0};

void xeXamShowGuide(int blade, int64_t open_message) {
  bool expected = false;
  if (!g_guide_open.compare_exchange_strong(expected, true)) {
    return;
  }
  std::thread([blade, open_message]() {
    if (auto* dialog = xeXamNewDialog<GuideDialog>(blade, open_message)) {
      xeXamDispatchDialog<GuideDialog>(
          dialog, [](GuideDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
    }
    g_ui_closed_ms = sysui::NowMs();
    g_guide_open = false;
  }).detach();
}

bool xeXamFilterGameInput(uint32_t user_index, uint16_t buttons) {
  using namespace rex::input;
  if (user_index >= 4) {
    return false;
  }
  if (user_index == 0) {
    announce::EnsureStarted();
    xeXamEnsureVoiceIndicator();
  }
  const uint16_t prev = g_prev_buttons[user_index].exchange(buttons);
  const uint32_t bit = 1u << user_index;
  if (xeXamIsUIActive() || g_guide_open) {
    // Every pad stays idle until its buttons are let go after the UI closes,
    // so the press that closed it doesn't reach the game.
    g_swallow_mask |= 0xF;
    return true;
  }
  constexpr uint16_t kCombo = X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_START;
  // Enter on the keyboard (or a Steam Input button mapped to it), seen from
  // player 1's polls.
  static std::atomic<bool> enter_was_down{false};
  bool enter_press = false;
  if (user_index == 0) {
    const bool enter = xeXamEnterKeyDown();
    enter_press = enter && !enter_was_down.exchange(enter);
    if (!enter) enter_was_down = false;
  }
  const bool guide_press = ((buttons & X_INPUT_GAMEPAD_GUIDE) && !(prev & X_INPUT_GAMEPAD_GUIDE)) ||
                           enter_press;
  const bool combo_press = REXCVAR_GET(guide_combo) && (buttons & kCombo) == kCombo &&
                           (prev & kCombo) != kCombo;
  if ((guide_press || combo_press) && sysui::NowMs() - g_ui_closed_ms > 400) {
    // An unread announcement opens first.
    xeXamShowGuide(kGuideBladeHome, announce::FirstUnread());
    g_swallow_mask |= 0xF;
    return true;
  }
  if (g_swallow_mask & bit) {
    if (buttons) {
      return true;
    }
    g_swallow_mask &= ~bit;
  }
  return false;
}

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
u32 XamShowMessageBoxUI_entry(u32 user_index, mapped_wstring title_ptr, mapped_wstring text_ptr,
                              u32 button_count, mapped_u32 button_ptrs, u32 active_button,
                              u32 flags, mapped_u32 result_ptr, mapped_void overlapped) {
  REXKRNL_DEBUG(
      "XamShowMessageBoxUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
      uint32_t(user_index), title_ptr.guest_address(), text_ptr.guest_address(),
      uint32_t(button_count), button_ptrs.guest_address(), uint32_t(active_button), uint32_t(flags),
      result_ptr.guest_address(), overlapped.guest_address());
  std::string title;
  if (title_ptr) {
    title = rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
        REX_KERNEL_MEMORY()->TranslateVirtual(title_ptr.guest_address())));
  } else {
    title = "";  // TODO(gibbed): default title based on flags?
  }
  std::string text_str = text_ptr
                             ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                                   REX_KERNEL_MEMORY()->TranslateVirtual(text_ptr.guest_address())))
                             : "";

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = rex::memory::load_and_swap<std::u16string>(
        REX_KERNEL_MEMORY()->TranslateVirtual(button_ptr));
    buttons.push_back(rex::string::to_utf8(button));
  }

  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    // Auto-pick the focused button.
    auto run = [result_ptr, active_button]() -> X_RESULT {
      *result_ptr = static_cast<uint32_t>(active_button);
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    // TODO(benvanik): setup icon states.
    switch (flags & 0xF) {
      case 0:
        // config.pszMainIcon = nullptr;
        break;
      case 1:
        // config.pszMainIcon = TD_ERROR_ICON;
        break;
      case 2:
        // config.pszMainIcon = TD_WARNING_ICON;
        break;
      case 3:
        // config.pszMainIcon = TD_INFORMATION_ICON;
        break;
    }
    auto close = [result_ptr](MessageBoxDialog* dialog) -> X_RESULT {
      *result_ptr = dialog->chosen_button();
      return X_ERROR_SUCCESS;
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
    if (imgui_drawer) {
      result = xeXamDispatchDialog<MessageBoxDialog>(
          new MessageBoxDialog(imgui_drawer, title, text_str, buttons, active_button), close,
          overlapped.guest_address());
    } else {
      // Fallback to headless if no drawer available
      auto run = [result_ptr, active_button]() -> X_RESULT {
        *result_ptr = static_cast<uint32_t>(active_button);
        return X_ERROR_SUCCESS;
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

class KeyboardInputDialog : public XamDialog {
 public:
  KeyboardInputDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title,
                      std::string description, std::string default_text, size_t max_length)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    rex::string::copy_truncating(text_buffer_.data(), default_text_, text_buffer_.size());
  }

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::TextWrapped("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      if (ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        text_ = std::string(text_buffer_.data(), text_buffer_.size());
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      if (ImGui::Button("OK")) {
        text_ = std::string(text_buffer_.data(), text_buffer_.size());
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        text_ = "";
        cancelled_ = true;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  size_t max_length_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;
};

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
u32 XamShowKeyboardUI_entry(u32 user_index, u32 flags, mapped_wstring default_text,
                            mapped_wstring title, mapped_wstring description, mapped_wstring buffer,
                            u32 buffer_length, mapped_void overlapped) {
  REXKRNL_DEBUG("XamShowKeyboardUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(flags), default_text.guest_address(),
                title.guest_address(), description.guest_address(), buffer.guest_address(),
                uint32_t(buffer_length), overlapped.guest_address());
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  assert_true(overlapped.guest_address() != 0);

  auto buffer_size = static_cast<size_t>(buffer_length) * 2;

  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    auto run = [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
      // Redirect default_text back into the buffer.
      if (!default_text) {
        std::memset(buffer, 0, buffer_size);
      } else {
        rex::string::copy_and_swap_truncating(buffer, default_text.value(), buffer_length);
      }
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    auto close = [buffer, buffer_length](KeyboardInputDialog* dialog, uint32_t& extended_error,
                                         uint32_t& length) -> X_RESULT {
      if (dialog->cancelled()) {
        extended_error = X_ERROR_CANCELLED;
        length = 0;
        return X_ERROR_SUCCESS;
      } else {
        // Zero the output buffer.
        auto text = rex::string::to_utf16(dialog->text());
        rex::string::copy_and_swap_truncating(buffer, text, buffer_length);
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        return X_ERROR_SUCCESS;
      }
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

    // Read and convert title/description/default_text from guest memory as utf16 to utf8 strings
    std::string title_str = title
                                ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                                      REX_KERNEL_MEMORY()->TranslateVirtual(title.guest_address())))
                                : "";
    std::string desc_str =
        description ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                          REX_KERNEL_MEMORY()->TranslateVirtual(description.guest_address())))
                    : "";
    std::string def_text_str =
        default_text ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                           REX_KERNEL_MEMORY()->TranslateVirtual(default_text.guest_address())))
                     : "";

    if (imgui_drawer) {
      uint32_t buffer_length_safe = buffer_length + 1;  // +1 for null terminator, just in case
      result = xeXamDispatchDialogEx<KeyboardInputDialog>(
          new KeyboardInputDialog(imgui_drawer, title_str, desc_str, def_text_str,
                                  buffer_length_safe),
          close, overlapped.guest_address());
    } else {
      // Fallback to headless
      auto run = [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
        if (!default_text) {
          std::memset(buffer, 0, buffer_size);
        } else {
          rex::string::copy_and_swap_truncating(buffer, default_text.value(), buffer_length);
        }
        return X_ERROR_SUCCESS;
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

u32 XamShowDeviceSelectorUI_entry(u32 user_index, u32 content_type, u32 content_flags,
                                  u64 total_requested, mapped_u32 device_id_ptr,
                                  mapped_void overlapped) {
  REXKRNL_DEBUG("XamShowDeviceSelectorUI({:08X}, {:08X}, {:08X}, {:016X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(content_type), uint32_t(content_flags),
                uint64_t(total_requested), device_id_ptr.guest_address(),
                overlapped.guest_address());
  return xeXamDispatchHeadless(
      [device_id_ptr]() -> X_RESULT {
        // NOTE: 0x00000001 is our dummy device ID from xam_content.cc
        *device_id_ptr = 0x00000001;
        return X_ERROR_SUCCESS;
      },
      overlapped.guest_address());
}

void XamShowDirtyDiscErrorUI_entry(u32 user_index) {
  REXKRNL_ERROR("XamShowDirtyDiscErrorUI called! user_index={}", uint32_t(user_index));
  REXKRNL_ERROR("This indicates a disc/file read error - check that all game files exist");

  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (imgui_drawer) {
    xeXamDispatchDialog<MessageBoxDialog>(
        new MessageBoxDialog(imgui_drawer, "Disc Read Error",
                             "There's been an issue reading content from the game disc.\nThis is "
                             "likely caused by bad or unimplemented file IO calls.",
                             {"OK"}, 0),
        [](MessageBoxDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  } else {
    // No UI available - log prominently and pause to let user see the error
    REXKRNL_ERROR("===========================================");
    REXKRNL_ERROR("FATAL: Disc Read Error (no UI to display)");
    REXKRNL_ERROR("Check that all game content files are present");
    REXKRNL_ERROR("Missing files or bad mounts cause this error");
    REXKRNL_ERROR("===========================================");
  }
  // This is death, and should never return.
  // TODO(benvanik): cleaner exit.
  exit(1);
}

u32 XamShowPartyUI_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

// Friends / invite UIs (see xam_friends.cpp). Like the console's they return
// at once and run their dialog alongside the game.
u32 XamShowGameInviteUI_entry(u32 user_index, mapped_u64 recipients, u32 count, u32 unk) {
  std::vector<uint64_t> xuids;
  for (uint32_t i = 0; recipients && i < count && i < 16; ++i) {
    xuids.push_back(recipients[i]);
  }
  std::thread([user_index, xuids]() {
    xeXInviteShowUI(user_index, xuids.data(), static_cast<uint32_t>(xuids.size()));
  }).detach();
  return X_ERROR_SUCCESS;
}

u32 XamShowGamerCardUIForXUID_entry(u32 user_index, u64 xuid) {
  std::thread([]() { xeXFriendsShowPlayers(); }).detach();
  return X_ERROR_SUCCESS;
}

u32 XamShowPlayersUI_entry(u32 user_index) {
  std::thread([]() { xeXFriendsShowPlayers(); }).detach();
  return X_ERROR_SUCCESS;
}

u32 XamShowCommunitySessionsUI_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

uint32_t XamShowMessageBoxUIEx_entry() {
  // TODO(tomc): implement properly
  static bool warned = false;
  if (!warned) {
    REXKRNL_WARN("[STUB] XamShowMessageBoxUIEx - not implemented");
    warned = true;
  }
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamIsUIActive, rex::kernel::xam::XamIsUIActive_entry)
REX_EXPORT(__imp__XamShowMessageBoxUI, rex::kernel::xam::XamShowMessageBoxUI_entry)
REX_EXPORT(__imp__XamShowKeyboardUI, rex::kernel::xam::XamShowKeyboardUI_entry)
REX_EXPORT(__imp__XamShowDeviceSelectorUI, rex::kernel::xam::XamShowDeviceSelectorUI_entry)
REX_EXPORT(__imp__XamShowDirtyDiscErrorUI, rex::kernel::xam::XamShowDirtyDiscErrorUI_entry)
REX_EXPORT(__imp__XamShowPartyUI, rex::kernel::xam::XamShowPartyUI_entry)
REX_EXPORT(__imp__XamShowCommunitySessionsUI, rex::kernel::xam::XamShowCommunitySessionsUI_entry)
REX_EXPORT(__imp__XamShowMessageBoxUIEx, rex::kernel::xam::XamShowMessageBoxUIEx_entry)

REX_EXPORT_STUB(__imp__XamIsGuideDisabled);
REX_EXPORT_STUB(__imp__XamIsMessageBoxActive);
REX_EXPORT_STUB(__imp__XamIsNuiUIActive);
REX_EXPORT_STUB(__imp__XamIsSysUiInvokedByTitle);
REX_EXPORT_STUB(__imp__XamIsSysUiInvokedByXenonButton);
REX_EXPORT_STUB(__imp__XamIsUIThread);
REX_EXPORT_STUB(__imp__XamNavigate);
REX_EXPORT_STUB(__imp__XamNavigateBack);
REX_EXPORT_STUB(__imp__XamOverrideHudOpenType);
REX_EXPORT_STUB(__imp__XamShowAchievementDetailsUI);
REX_EXPORT_STUB(__imp__XamShowAchievementsUI);
REX_EXPORT_STUB(__imp__XamShowAchievementsUIEx);
REX_EXPORT_STUB(__imp__XamShowAndWaitForMessageBoxEx);
REX_EXPORT_STUB(__imp__XamShowAvatarAwardGamesUI);
REX_EXPORT_STUB(__imp__XamShowAvatarAwardsUI);
REX_EXPORT_STUB(__imp__XamShowAvatarMiniCreatorUI);
REX_EXPORT_STUB(__imp__XamShowBadDiscErrorUI);
REX_EXPORT_STUB(__imp__XamShowBeaconsUI);
REX_EXPORT_STUB(__imp__XamShowBrandedKeyboardUI);
REX_EXPORT_STUB(__imp__XamShowChangeGamerTileUI);
REX_EXPORT_STUB(__imp__XamShowComplaintUI);
REX_EXPORT_STUB(__imp__XamShowCreateProfileUI);
REX_EXPORT_STUB(__imp__XamShowCreateProfileUIEx);
REX_EXPORT_STUB(__imp__XamShowCsvTransitionUI);
REX_EXPORT_STUB(__imp__XamShowCustomMessageComposeUI);
REX_EXPORT_STUB(__imp__XamShowCustomPlayerListUI);
REX_EXPORT_STUB(__imp__XamShowDirectAcquireUI);
REX_EXPORT_STUB(__imp__XamShowEditProfileUI);
REX_EXPORT_STUB(__imp__XamShowFirstRunWelcomeUI);
REX_EXPORT_STUB(__imp__XamShowFitnessBodyProfileUI);
REX_EXPORT_STUB(__imp__XamShowFitnessClearUI);
REX_EXPORT_STUB(__imp__XamShowFitnessWarnAboutPrivacyUI);
REX_EXPORT_STUB(__imp__XamShowFitnessWarnAboutTimeUI);
REX_EXPORT_STUB(__imp__XamShowFofUI);
REX_EXPORT_STUB(__imp__XamShowForcedNameChangeUI);
REX_EXPORT_STUB(__imp__XamShowFriendRequestUI);
REX_EXPORT_STUB(__imp__XamShowFriendsUI);
REX_EXPORT_STUB(__imp__XamShowFriendsUIp);
REX_EXPORT(__imp__XamShowGameInviteUI, rex::kernel::xam::XamShowGameInviteUI_entry)
REX_EXPORT_STUB(__imp__XamShowGameVoiceChannelUI);
REX_EXPORT_STUB(__imp__XamShowGamerCardUI);
REX_EXPORT(__imp__XamShowGamerCardUIForXUID, rex::kernel::xam::XamShowGamerCardUIForXUID_entry)
REX_EXPORT_STUB(__imp__XamShowGamerCardUIForXUIDp);
REX_EXPORT_STUB(__imp__XamShowGamesUI);
REX_EXPORT_STUB(__imp__XamShowGenericOnlineAppUI);
REX_EXPORT_STUB(__imp__XamShowGoldUpgradeUI);
REX_EXPORT_STUB(__imp__XamShowGraduateUserUI);
REX_EXPORT_STUB(__imp__XamShowGuideUI);
REX_EXPORT_STUB(__imp__XamShowJoinPartyUI);
REX_EXPORT_STUB(__imp__XamShowJoinSessionByIdInProgressUI);
REX_EXPORT_STUB(__imp__XamShowJoinSessionInProgressUI);
REX_EXPORT_STUB(__imp__XamShowKeyboardUIMessenger);
REX_EXPORT_STUB(__imp__XamShowLiveSignupUI);
REX_EXPORT_STUB(__imp__XamShowLiveUpsellUI);
REX_EXPORT_STUB(__imp__XamShowLiveUpsellUIEx);
REX_EXPORT_STUB(__imp__XamShowMarketplaceDownloadItemsUI);
REX_EXPORT_STUB(__imp__XamShowMarketplaceGetOrderReceipts);
REX_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUI);
REX_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUIEx);
REX_EXPORT_STUB(__imp__XamShowMarketplaceUI);
REX_EXPORT_STUB(__imp__XamShowMarketplaceUIEx);
REX_EXPORT_STUB(__imp__XamShowMessageBox);
REX_EXPORT_STUB(__imp__XamShowMessageComposeUI);
REX_EXPORT_STUB(__imp__XamShowMessagesUI);
REX_EXPORT_STUB(__imp__XamShowMessagesUIEx);
REX_EXPORT_STUB(__imp__XamShowMessengerUI);
REX_EXPORT_STUB(__imp__XamShowMultiplayerUpgradeUI);
REX_EXPORT_STUB(__imp__XamShowNetworkStorageSyncUI);
REX_EXPORT_STUB(__imp__XamShowNuiAchievementsUI);
REX_EXPORT_STUB(__imp__XamShowNuiCommunitySessionsUI);
REX_EXPORT_STUB(__imp__XamShowNuiControllerRequiredUI);
REX_EXPORT_STUB(__imp__XamShowNuiDeviceSelectorUI);
REX_EXPORT_STUB(__imp__XamShowNuiDirtyDiscErrorUI);
REX_EXPORT_STUB(__imp__XamShowNuiFriendRequestUI);
REX_EXPORT_STUB(__imp__XamShowNuiFriendsUI);
REX_EXPORT_STUB(__imp__XamShowNuiGameInviteUI);
REX_EXPORT_STUB(__imp__XamShowNuiGamerCardUIForXUID);
REX_EXPORT_STUB(__imp__XamShowNuiGamesUI);
REX_EXPORT_STUB(__imp__XamShowNuiGuideUI);
REX_EXPORT_STUB(__imp__XamShowNuiHardwareRequiredUI);
REX_EXPORT_STUB(__imp__XamShowNuiJoinSessionInProgressUI);
REX_EXPORT_STUB(__imp__XamShowNuiMarketplaceDownloadItemsUI);
REX_EXPORT_STUB(__imp__XamShowNuiMarketplaceUI);
REX_EXPORT_STUB(__imp__XamShowNuiMessageBoxUI);
REX_EXPORT_STUB(__imp__XamShowNuiMessagesUI);
REX_EXPORT_STUB(__imp__XamShowNuiPartyUI);
REX_EXPORT_STUB(__imp__XamShowNuiSigninUI);
REX_EXPORT_STUB(__imp__XamShowNuiVideoRichPresenceUI);
REX_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUI);
REX_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUIEx);
REX_EXPORT_STUB(__imp__XamShowOptionsUI);
REX_EXPORT_STUB(__imp__XamShowPamUI);
REX_EXPORT_STUB(__imp__XamShowPartyInviteUI);
REX_EXPORT_STUB(__imp__XamShowPartyJoinInProgressUI);
REX_EXPORT_STUB(__imp__XamShowPasscodeVerifyUI);
REX_EXPORT_STUB(__imp__XamShowPasscodeVerifyUIEx);
REX_EXPORT_STUB(__imp__XamShowPaymentOptionsUI);
REX_EXPORT_STUB(__imp__XamShowPersonalizationUI);
REX_EXPORT_STUB(__imp__XamShowPlayerReviewUI);
REX_EXPORT(__imp__XamShowPlayersUI, rex::kernel::xam::XamShowPlayersUI_entry)
REX_EXPORT_STUB(__imp__XamShowPrivateChatInviteUI);
REX_EXPORT_STUB(__imp__XamShowQuickChatUI);
REX_EXPORT_STUB(__imp__XamShowQuickChatUIp);
REX_EXPORT_STUB(__imp__XamShowQuickLaunchUI);
REX_EXPORT_STUB(__imp__XamShowRecentMessageUI);
REX_EXPORT_STUB(__imp__XamShowRecentMessageUIEx);
REX_EXPORT_STUB(__imp__XamShowReputationUI);
REX_EXPORT_STUB(__imp__XamShowSigninUIEx);
REX_EXPORT_STUB(__imp__XamShowSigninUIp);
REX_EXPORT_STUB(__imp__XamShowSignupCreditCardUI);
REX_EXPORT_STUB(__imp__XamShowSocialPostUI);
REX_EXPORT_STUB(__imp__XamShowStorePickerUI);
REX_EXPORT_STUB(__imp__XamShowTFAUI);
REX_EXPORT_STUB(__imp__XamShowTermsOfUseUI);
REX_EXPORT_STUB(__imp__XamShowUpdaterUI);
REX_EXPORT_STUB(__imp__XamShowVideoChatInviteUI);
REX_EXPORT_STUB(__imp__XamShowVideoRichPresenceUI);
REX_EXPORT_STUB(__imp__XamShowVoiceMailUI);
REX_EXPORT_STUB(__imp__XamShowVoiceSettingsUI);
REX_EXPORT_STUB(__imp__XamShowWhatsOnUI);
REX_EXPORT_STUB(__imp__XamShowWordRegisterUI);
REX_EXPORT_STUB(__imp__XamSysUiDisableAutoClose);
